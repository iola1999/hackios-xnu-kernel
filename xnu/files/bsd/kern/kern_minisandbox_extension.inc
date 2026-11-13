/*
 * iOS 26 file_to_process extensions use a 56-byte request and bind the token
 * to a pid and pidversion. A token names a checked vnode retained by its
 * target process. Consumed handles have independent per-process references.
 * This implements the file extension lifecycle within MiniSandbox's existing
 * non-enforcing policy; vnode_authorize() still supplies the DAC decision.
 */
#define SANDBOX_SYSCALL_EXTENSION_ISSUE 5
#define SANDBOX_SYSCALL_EXTENSION_CONSUME 6
#define SANDBOX_SYSCALL_EXTENSION_RELEASE 7
#define SANDBOX_SYSCALL_EXTENSION_RELEASE_DETECT 80
#define MINISANDBOX_EXTENSION_TARGET_AUDIT 0x30000ULL
#define MINISANDBOX_EXTENSION_TOKEN_LENGTH 49

struct minisandbox_extension_issue_args {
	user_addr_t class_name;
	uint64_t type;
	user_addr_t resource;
	uint64_t flags;
	user_addr_t token;
	int64_t pid;
	uint64_t pidversion;
};

struct minisandbox_extension_consume_args {
	user_addr_t token;
	uint64_t length;
	user_addr_t handle;
};

struct minisandbox_extension_release_args {
	int64_t handle;
	uint64_t options;
	user_addr_t path;
};

_Static_assert(sizeof(struct minisandbox_extension_issue_args) == 56, "Sandbox request 5 ABI");
_Static_assert(sizeof(struct minisandbox_extension_consume_args) == 24, "Sandbox request 6 ABI");
_Static_assert(sizeof(struct minisandbox_extension_release_args) == 24, "Sandbox request 7 ABI");

struct minisandbox_extension {
	struct minisandbox_extension *next;
	struct os_refcnt refs;
	vnode_t vnode;
	kauth_action_t rights;
	uint64_t target_uniqueid;
	uint32_t target_pidversion;
	pid_t target_pid;
	char token[MINISANDBOX_EXTENSION_TOKEN_LENGTH];
};

struct minisandbox_extension_grant {
	struct minisandbox_extension_grant *next;
	struct minisandbox_extension *extension;
	uint64_t owner_uniqueid;
	int64_t handle;
};

static LCK_GRP_DECLARE(minisandbox_extension_lock_group, "minisandbox-extensions");
static LCK_MTX_DECLARE(minisandbox_extension_lock, &minisandbox_extension_lock_group);
static struct minisandbox_extension *minisandbox_extensions;
static struct minisandbox_extension_grant *minisandbox_extension_grants;
static uint64_t minisandbox_extension_identifier;

static void
minisandbox_extension_put(struct minisandbox_extension *extension)
{
	if (os_ref_release(&extension->refs) == 0) {
		vnode_rele(extension->vnode);
		kfree_type(struct minisandbox_extension, extension);
	}
}

/* The table lock protects links and identifiers; VFS, user copies and allocation run outside it. */
static int
minisandbox_extension_next_identifier(int64_t *identifier)
{
	int error = 0;
	lck_mtx_lock(&minisandbox_extension_lock);
	if (minisandbox_extension_identifier == INT64_MAX) {
		error = EOVERFLOW;
	} else {
		*identifier = (int64_t)++minisandbox_extension_identifier;
	}
	lck_mtx_unlock(&minisandbox_extension_lock);
	return error;
}

static int
minisandbox_extension_proc_begin(proc_t process)
{
	proc_lock(process);
	int error = (process->p_lflag & P_LEXIT) ? ESRCH : proc_transstart(process, 1, 1);
	proc_unlock(process);
	return error;
}

static int
minisandbox_extension_class_rights(proc_t issuer, const char *class_name,
    vnode_t vnode, kauth_action_t *rights)
{
	bool writable = false, container = false;
	if (strcmp(class_name, "com.apple.app-sandbox.read") == 0) {
		*rights = KAUTH_VNODE_READ_DATA;
	} else if (strcmp(class_name, "com.apple.app-sandbox.read-write") == 0) {
		writable = true;
	} else if (strcmp(class_name, "com.apple.sandbox.container") == 0 ||
	    strcmp(class_name, "com.apple.sandbox.application-group") == 0 ||
	    strcmp(class_name, "com.apple.sandbox.system-container") == 0 ||
	    strcmp(class_name, "com.apple.sandbox.system-group") == 0) {
		container = true;
		writable = true;
	} else {
		return ENOTSUP;
	}
	if (vnode_vtype(vnode) != VREG && vnode_vtype(vnode) != VDIR) {
		return EINVAL;
	}
	if (container) {
		/* These classes come from ContainerManagerCommon's container configuration. */
		if (vnode_vtype(vnode) != VDIR) {
			return ENOTDIR;
		}
		if (miniamfi_entitlement_boolean(issuer, "com.apple.private.security.container-manager") != KERN_SUCCESS) {
			return EPERM;
		}
	}
	if (writable) {
		*rights = KAUTH_VNODE_READ_DATA | KAUTH_VNODE_WRITE_DATA;
	}
	if (vnode_vtype(vnode) == VDIR) {
		*rights |= KAUTH_VNODE_SEARCH;
		if (writable) {
			*rights |= KAUTH_VNODE_ADD_SUBDIRECTORY | KAUTH_VNODE_DELETE_CHILD;
		}
	}
	return 0;
}

static void
minisandbox_extension_make_token(char token[MINISANDBOX_EXTENSION_TOKEN_LENGTH], int64_t identifier)
{
	static const char hex[] = "0123456789abcdef";
	uint8_t bytes[24];
	for (size_t i = 0; i < 8; i++) {
		bytes[i] = (uint8_t)((uint64_t)identifier >> ((7 - i) * 8));
	}
	read_random(bytes + 8, 16);
	for (size_t i = 0; i < sizeof(bytes); i++) {
		token[i * 2] = hex[bytes[i] >> 4];
		token[i * 2 + 1] = hex[bytes[i] & 15];
	}
	token[MINISANDBOX_EXTENSION_TOKEN_LENGTH - 1] = '\0';
}

static int
minisandbox_extension_issue(proc_t issuer, user_addr_t arg)
{
	struct minisandbox_extension_issue_args args;
	struct minisandbox_extension *extension = NULL;
	char class_name[128], path[MAXPATHLEN];
	vfs_context_t context;
	vnode_t vnode = NULLVP;
	proc_t target;
	kauth_action_t rights = 0;
	size_t length;
	int64_t identifier;
	int error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	if (args.type != 0 || args.flags != MINISANDBOX_EXTENSION_TARGET_AUDIT) {
		return ENOTSUP;
	}
	if (args.pid <= 0 || args.pid > INT_MAX || args.pidversion > UINT32_MAX) {
		return ESRCH;
	}
	if (!args.token) {
		return EFAULT;
	}
	error = copyinstr(args.class_name, class_name, sizeof(class_name), &length);
	if (error) {
		return error;
	}
	error = copyinstr(args.resource, path, sizeof(path), &length);
	if (error) {
		return error;
	}
	if (length < 2 || path[0] != '/') {
		return EINVAL;
	}
	target = proc_find((pid_t)args.pid);
	if (target == PROC_NULL) {
		return ESRCH;
	}
	if ((uint32_t)proc_pidversion(target) != args.pidversion) {
		error = ESRCH;
		goto out_target;
	}
	context = vfs_context_create_with_proc(issuer);
	if (context == NULL) {
		error = ENOMEM;
		goto out_target;
	}
	error = vnode_lookup(path, 0, &vnode, context);
	if (error == 0) {
		error = minisandbox_extension_class_rights(issuer, class_name, vnode, &rights);
	}
	if (error == 0) {
		error = vnode_authorize(vnode, NULLVP, rights, context);
	}
	if (error == 0) {
		error = vnode_ref(vnode);
	}
	if (vnode != NULLVP) {
		vnode_put(vnode);
	}
	vfs_context_rele(context);
	if (error) {
		goto out_target;
	}
	extension = kalloc_type(struct minisandbox_extension, Z_WAITOK | Z_ZERO);
	if (extension == NULL) {
		vnode_rele(vnode);
		error = ENOMEM;
		goto out_target;
	}
	os_ref_init(&extension->refs, NULL);
	extension->vnode = vnode;
	extension->rights = rights;
	extension->target_uniqueid = proc_uniqueid(target);
	extension->target_pid = (pid_t)args.pid;
	extension->target_pidversion = (uint32_t)args.pidversion;
	error = minisandbox_extension_next_identifier(&identifier);
	if (error) {
		goto out_extension;
	}
	minisandbox_extension_make_token(extension->token, identifier);
	/* Hold the target's exec/exit transition across output and publication. */
	error = minisandbox_extension_proc_begin(target);
	if (error) {
		goto out_extension;
	}
	if ((uint32_t)proc_pidversion(target) != args.pidversion) {
		error = ESRCH;
	} else {
		error = copyout(extension->token, args.token, sizeof(extension->token));
		if (error == 0) {
			lck_mtx_lock(&minisandbox_extension_lock);
			extension->next = minisandbox_extensions;
			minisandbox_extensions = extension;
			lck_mtx_unlock(&minisandbox_extension_lock);
			extension = NULL;
		}
	}
	proc_transend(target, 0);
out_extension:
	if (extension != NULL) {
		minisandbox_extension_put(extension);
	}
out_target:
	proc_rele(target);
	return error;
}

static int
minisandbox_extension_consume(proc_t owner, user_addr_t arg)
{
	struct minisandbox_extension_consume_args args;
	struct minisandbox_extension *extension = NULL;
	struct minisandbox_extension_grant *grant;
	char token[MINISANDBOX_EXTENSION_TOKEN_LENGTH];
	int64_t handle = -1;
	int error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	error = copyout(&handle, args.handle, sizeof(handle));
	if (error) {
		return error;
	}
	if (args.length != sizeof(token)) {
		return EINVAL;
	}
	error = copyin(args.token, token, sizeof(token));
	if (error) {
		return error;
	}
	if (token[sizeof(token) - 1] != '\0') {
		return EINVAL;
	}
	error = minisandbox_extension_proc_begin(owner);
	if (error) {
		return error;
	}
	lck_mtx_lock(&minisandbox_extension_lock);
	for (extension = minisandbox_extensions; extension != NULL; extension = extension->next) {
		if (memcmp(extension->token, token, sizeof(token)) == 0) {
			break;
		}
	}
	if (extension == NULL) {
		error = EINVAL;
	} else if (extension->target_uniqueid != proc_uniqueid(owner) ||
	    extension->target_pid != proc_getpid(owner)) {
		error = EPERM;
		extension = NULL;
	} else if (extension->target_pidversion != (uint32_t)proc_pidversion(owner)) {
		error = ESRCH;
		extension = NULL;
	} else {
		os_ref_retain(&extension->refs);
	}
	lck_mtx_unlock(&minisandbox_extension_lock);
	if (error) {
		goto out_transition;
	}
	vfs_context_t context = vfs_context_create_with_proc(owner);
	if (context == NULL) {
		error = ENOMEM;
		goto out_extension;
	}
	error = vnode_getwithref(extension->vnode);
	if (error == 0) {
		error = vnode_authorize(extension->vnode, NULLVP, extension->rights, context);
		vnode_put(extension->vnode);
	}
	vfs_context_rele(context);
	if (error) {
		goto out_extension;
	}
	grant = kalloc_type(struct minisandbox_extension_grant, Z_WAITOK | Z_ZERO);
	if (grant == NULL) {
		error = ENOMEM;
		goto out_extension;
	}
	error = minisandbox_extension_next_identifier(&handle);
	if (error == 0) {
		error = copyout(&handle, args.handle, sizeof(handle));
	}
	if (error == 0) {
		grant->extension = extension;
		grant->owner_uniqueid = proc_uniqueid(owner);
		grant->handle = handle;
		lck_mtx_lock(&minisandbox_extension_lock);
		grant->next = minisandbox_extension_grants;
		minisandbox_extension_grants = grant;
		lck_mtx_unlock(&minisandbox_extension_lock);
		extension = NULL;
	} else {
		kfree_type(struct minisandbox_extension_grant, grant);
	}
out_extension:
	if (extension != NULL) {
		minisandbox_extension_put(extension);
	}
out_transition:
	proc_transend(owner, 0);
	return error;
}

static int
minisandbox_extension_release(proc_t owner, user_addr_t arg)
{
	struct minisandbox_extension_release_args args;
	struct minisandbox_extension_grant *grant = NULL;
	uint64_t uniqueid = proc_uniqueid(owner);
	int error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	/* release_file has a path-only request; its removal semantics need a separate native check. */
	if (args.options != 0 || args.path != 0) {
		return ENOTSUP;
	}
	if (args.handle <= 0) {
		return EINVAL;
	}
	lck_mtx_lock(&minisandbox_extension_lock);
	for (struct minisandbox_extension_grant **link = &minisandbox_extension_grants;
	    *link != NULL; link = &(*link)->next) {
		if ((*link)->owner_uniqueid == uniqueid && (*link)->handle == args.handle) {
			grant = *link;
			*link = grant->next;
			break;
		}
	}
	lck_mtx_unlock(&minisandbox_extension_lock);
	if (grant == NULL) {
		return EINVAL;
	}
	minisandbox_extension_put(grant->extension);
	kfree_type(struct minisandbox_extension_grant, grant);
	return 0;
}

static void
minisandbox_extension_proc_cleanup(proc_t process, bool exiting)
{
	uint64_t uniqueid = proc_uniqueid(process);
	uint32_t pidversion = (uint32_t)proc_pidversion(process);
	struct minisandbox_extension *extensions = NULL;
	struct minisandbox_extension_grant *grants = NULL;
	lck_mtx_lock(&minisandbox_extension_lock);
	for (struct minisandbox_extension **link = &minisandbox_extensions;
	    *link != NULL;) {
		struct minisandbox_extension *entry = *link;
		if (entry->target_uniqueid == uniqueid && (exiting || entry->target_pidversion != pidversion)) {
			*link = entry->next;
			entry->next = extensions;
			extensions = entry;
		} else {
			link = &entry->next;
		}
	}
	for (struct minisandbox_extension_grant **link = &minisandbox_extension_grants;
	    *link != NULL;) {
		struct minisandbox_extension_grant *entry = *link;
		if (entry->owner_uniqueid == uniqueid) {
			*link = entry->next;
			entry->next = grants;
			grants = entry;
		} else {
			link = &entry->next;
		}
	}
	lck_mtx_unlock(&minisandbox_extension_lock);
	while (grants != NULL) {
		struct minisandbox_extension_grant *entry = grants;
		grants = entry->next;
		minisandbox_extension_put(entry->extension);
		kfree_type(struct minisandbox_extension_grant, entry);
	}
	while (extensions != NULL) {
		struct minisandbox_extension *entry = extensions;
		extensions = entry->next;
		minisandbox_extension_put(entry);
	}
}

static void
minisandbox_extension_exec_complete(proc_t process)
{
	minisandbox_extension_proc_cleanup(process, false);
}

static void
minisandbox_extension_exit(proc_t process)
{
	minisandbox_extension_proc_cleanup(process, true);
}

static void
minisandbox_extension_fork(__unused kauth_cred_t cred, proc_t child)
{
	proc_t parent = current_proc();
	struct minisandbox_extension_grant *slots = NULL, *unused;
	size_t capacity = 0;
	/* spawn also uses this creation hook; exec_complete clears its references before EL0. */
	if (proc_getpid(parent) != proc_original_ppid(child) ||
	    proc_pidversion(parent) != proc_orig_ppidversion(child)) {
		return;
	}
	uint64_t parent_uniqueid = proc_uniqueid(parent);
	uint64_t child_uniqueid = proc_uniqueid(child);
	for (;;) {
		size_t required = 0;
		lck_mtx_lock(&minisandbox_extension_lock);
		for (struct minisandbox_extension_grant *grant = minisandbox_extension_grants;
		    grant != NULL; grant = grant->next) {
			if (grant->owner_uniqueid == parent_uniqueid) {
				required++;
			}
		}
		if (required <= capacity) {
			struct minisandbox_extension_grant *slot = slots;
			for (struct minisandbox_extension_grant *grant = minisandbox_extension_grants;
			    grant != NULL; grant = grant->next) {
				if (grant->owner_uniqueid == parent_uniqueid) {
					slot->extension = grant->extension;
					os_ref_retain(&slot->extension->refs);
					slot->owner_uniqueid = child_uniqueid;
					slot->handle = grant->handle;
					slot = slot->next;
				}
			}
			unused = slot;
			lck_mtx_unlock(&minisandbox_extension_lock);
			break;
		}
		lck_mtx_unlock(&minisandbox_extension_lock);
		/* The void hook cannot report ENOMEM after fork created the child. */
		while (capacity < required) {
			struct minisandbox_extension_grant *slot = kalloc_type(struct minisandbox_extension_grant,
			    Z_WAITOK | Z_ZERO | Z_NOFAIL);
			slot->next = slots;
			slots = slot;
			capacity++;
		}
	}
	lck_mtx_lock(&minisandbox_extension_lock);
	while (slots != unused) {
		struct minisandbox_extension_grant *entry = slots;
		slots = entry->next;
		entry->next = minisandbox_extension_grants;
		minisandbox_extension_grants = entry;
	}
	lck_mtx_unlock(&minisandbox_extension_lock);
	while (unused != NULL) {
		struct minisandbox_extension_grant *entry = unused;
		unused = entry->next;
		kfree_type(struct minisandbox_extension_grant, entry);
	}
}
