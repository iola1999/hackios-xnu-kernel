/*
 * iOS 26 libsystem_sandbox passes syscall 66 a pid, pidversion and path pointer.
 * ContainerManagerCommon calls it after obtaining the process's data container.
 * Paths belong to immutable credentials: fork retains them and credential updates
 * copy the reference. A replacement never changes another process's credential.
 */
#define SANDBOX_SYSCALL_SET_CONTAINER_PATH 66
#define SANDBOX_SPAWN_CONTAINER_LENGTH_OFFSET 0x0c
#define SANDBOX_SPAWN_CONTAINER_PATH_OFFSET 0x50

static int minisandbox_slot;

struct minisandbox_container {
	struct os_refcnt refs;
	char path[MAXPATHLEN];
};

static struct minisandbox_container *
minisandbox_cred_container(kauth_cred_t cred)
{
	return (void *)mac_label_get(mac_cred_label(cred), minisandbox_slot);
}

static void
minisandbox_container_release(struct minisandbox_container *container)
{
	if (container != NULL && os_ref_release(&container->refs) == 0) {
		kfree_type(struct minisandbox_container, container);
	}
}

static void
minisandbox_label_destroy(struct label *label)
{
	minisandbox_container_release((void *)mac_label_get(label, minisandbox_slot));
}

static void
minisandbox_label_associate(kauth_cred_t parent, kauth_cred_t child)
{
	struct minisandbox_container *container = minisandbox_cred_container(parent);
	if (container != NULL) {
		os_ref_retain(&container->refs);
	}
	mac_label_set(mac_cred_label(child), minisandbox_slot, (intptr_t)container);
}

static void
minisandbox_label_replace(kauth_cred_t model, struct minisandbox_container *container)
{
	if (container != NULL) {
		os_ref_retain(&container->refs);
	}
	minisandbox_container_release(minisandbox_cred_container(model));
	mac_label_set(mac_cred_label(model), minisandbox_slot, (intptr_t)container);
	/* Credential deduplication must include the container label. */
	posix_cred_get(model)->cr_flags |= CRF_MAC_ENFORCE;
}

static int
minisandbox_set_container_path(struct proc *caller, user_addr_t arg)
{
	struct {
		uint64_t pid;
		uint64_t pidversion;
		user_addr_t path;
	} args;
	struct minisandbox_container *container;
	proc_t target;
	size_t length;
	int error;

	/* Both stock containermanagerd executables carry this entitlement. */
	if (miniamfi_entitlement_boolean(caller, "com.apple.private.security.container-manager") != KERN_SUCCESS) {
		return EPERM;
	}
	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	if (args.pid == 0 || args.pid > INT_MAX) {
		return ESRCH;
	}
	target = proc_find((pid_t)args.pid);
	if (target == PROC_NULL) {
		return ESRCH;
	}
	if ((uint64_t)proc_pidversion(target) != args.pidversion) {
		proc_rele(target);
		return ESRCH;
	}
	container = kalloc_type(struct minisandbox_container, Z_WAITOK | Z_ZERO);
	if (container == NULL) {
		proc_rele(target);
		return ENOMEM;
	}
	os_ref_init(&container->refs, NULL);
	error = copyinstr(args.path, container->path, sizeof(container->path), &length);
	if (error == 0 && (length < 2 || container->path[0] != '/')) {
		error = EINVAL;
	}
	if (error == 0) {
		/* Exec changes pidversion. Keep its transition outside the label update. */
		proc_lock(target);
		error = (target->p_lflag & P_LEXIT) ? ESRCH : proc_transstart(target, 1, 1);
		proc_unlock(target);
		if (error == 0) {
			if ((uint64_t)proc_pidversion(target) != args.pidversion) {
				error = ESRCH;
			} else {
				kauth_cred_proc_update(target, PROC_SETTOKEN_NONE,
				    ^bool (kauth_cred_t parent, kauth_cred_t model) {
					mac_cred_label_init(model);
					mac_cred_label_associate(parent, model);
					minisandbox_label_replace(model, container);
					return true;
				});
			}
			proc_transend(target, 0);
		}
	}
	minisandbox_container_release(container);
	proc_rele(target);
	return error;
}

/* The path and its byte count are written by sandbox_spawnattrs_setcontainer(). */
static int
minisandbox_spawn_container(void *attributes, size_t size, const char **path)
{
	uint32_t header[2], length;
	*path = NULL;
	if (attributes == NULL || size == 0) {
		return 0;
	}
	if (size < sizeof(header)) {
		return EINVAL;
	}
	memcpy(header, attributes, sizeof(header));
	/* sandbox_spawnattrs_init() writes version 0 and a 0x450-byte structure. */
	if (header[0] != 0 || header[1] != 0x450 || size < header[1]) {
		return EINVAL;
	}
	memcpy(&length, (char *)attributes + SANDBOX_SPAWN_CONTAINER_LENGTH_OFFSET, sizeof(length));
	if (length == 0) {
		return 0;
	}
	if (length >= MAXPATHLEN || length >= size - SANDBOX_SPAWN_CONTAINER_PATH_OFFSET) {
		return EINVAL;
	}
	const char *value = (char *)attributes + SANDBOX_SPAWN_CONTAINER_PATH_OFFSET;
	if (value[0] != '/' || strnlen(value, length + 1) != length) {
		return EINVAL;
	}
	*path = value;
	return 0;
}

static int
minisandbox_check_exec(__unused kauth_cred_t old, __unused struct vnode *vp,
    __unused off_t offset, __unused struct vnode *scriptvp, __unused struct label *vnodelabel,
    __unused struct label *scriptvnodelabel, __unused struct label *execlabel,
    __unused struct proc *p, void *attributes, size_t size)
{
	const char *path;
	/* A malformed attribute must reach the update hook so exec returns its error. */
	return minisandbox_spawn_container(attributes, size, &path) != 0 || path != NULL;
}

static int
minisandbox_update_exec(__unused kauth_cred_t old, kauth_cred_t model,
    __unused struct proc *p, __unused struct vnode *vp, __unused off_t offset,
    __unused struct vnode *scriptvp, __unused struct label *vnodelabel,
    __unused struct label *scriptvnodelabel, __unused struct label *execlabel,
    __unused u_int *csflags, void *attributes, size_t size, __unused int *disjoint)
{
	const char *path;
	int error = minisandbox_spawn_container(attributes, size, &path);
	if (error || path == NULL) {
		return error;
	}
	struct minisandbox_container *container = kalloc_type(struct minisandbox_container, Z_WAITOK | Z_ZERO);
	if (container == NULL) {
		return ENOMEM;
	}
	os_ref_init(&container->refs, NULL);
	strlcpy(container->path, path, sizeof(container->path));
	minisandbox_label_replace(model, container);
	minisandbox_container_release(container);
	return 0;
}

static int
minisandbox_label_container_path(proc_t process, user_addr_t buffer, user_size_t size, bool *found)
{
	kauth_cred_t cred = kauth_cred_proc_ref(process);
	struct minisandbox_container *container = minisandbox_cred_container(cred);
	int error = 0;
	*found = container != NULL;
	if (container != NULL) {
		size_t length = strlen(container->path) + 1;
		error = size < length ? ENAMETOOLONG : copyout(container->path, buffer, length);
	}
	kauth_cred_unref(&cred);
	return error;
}
