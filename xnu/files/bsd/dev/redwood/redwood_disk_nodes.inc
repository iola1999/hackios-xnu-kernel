/*
 * redwood_disk_nodes.inc: create standard Apple APFS container and volume device nodes in /dev.
 *
 * Exposes:
 *   disk0, rdisk0 (whole physical storage)
 *   disk0s1, rdisk0s1 (Apple_APFS physical store partition)
 *   disk1, rdisk1 (AppleAPFSContainer)
 *   disk1s1, rdisk1s1 (System volume -> darwin partition)
 *   disk1s2, rdisk1s2 (Data volume -> darwin_var partition)
 *   disk1s3, rdisk1s3 (Preboot volume)
 *   disk2, rdisk2, disk2s1, rdisk2s1 (OS cryptex)
 *   disk3, rdisk3, disk3s1, rdisk3s1 (App cryptex)
 */

static void
redwood_create_disk_nodes(int bmajor, int cmajor, int darwin_part, int var_part)
{
	static const struct {
		const char *name;
		int is_var;
	} k_disk_nodes[] = {
		{ "disk0", 0 },
		{ "disk0s1", 0 },
		{ "disk1", 0 },
		{ "disk1s1", 0 },
		{ "disk1s2", 1 },
		{ "disk1s3", 0 },
		{ "disk2", 0 },
		{ "disk2s1", 0 },
		{ "disk3", 0 },
		{ "disk3s1", 0 },
	};
	int d_minor = darwin_part >= 0 ? darwin_part : 0;
	int v_minor = var_part >= 0 ? var_part : d_minor;

	for (size_t idx = 0; idx < sizeof(k_disk_nodes) / sizeof(k_disk_nodes[0]); idx++) {
		int minor = k_disk_nodes[idx].is_var ? v_minor : d_minor;
		char rname[32];

		(void)devfs_make_node(makedev(bmajor, minor), DEVFS_BLOCK,
		    UID_ROOT, GID_OPERATOR, 0640, "%s", k_disk_nodes[idx].name);
		snprintf(rname, sizeof(rname), "r%s", k_disk_nodes[idx].name);
		(void)devfs_make_node(makedev(cmajor, minor), DEVFS_CHAR,
		    UID_ROOT, GID_OPERATOR, 0640, "%s", rname);
	}
	printf("ufs: created standard Apple /dev/disk* and /dev/rdisk* devfs nodes\n");
}
