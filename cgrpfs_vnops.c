/*
 * vnode operations for PUFFS-based cgrpfs
 */

#include "cgrpfs.h"

#include <errno.h>
#include <stdio.h>

static int
nodevtype(cg_node_t *node)
{
	switch (node->type) {
	case CGN_PROCS: /* cgroup.procs file */
	case CGN_RELEASE_AGENT: /* release_agent file */
	case CGN_NOTIFY_ON_RELEASE: /* notify_on_release file */
	case CGN_PID_CGROUP:
		return VREG;

	case CGN_CG_DIR: /* cgroup directory */
	case CGN_PID_ROOT_DIR: /* cgroup.meta root dir */
	case CGN_PID_DIR: /* cgroup.meta/$pid directory */
		return VDIR;
	default:
		return VBAD;
	}
}

int
cgrpfs_node_lookup(struct puffs_usermount *pu, void *opc,
	struct puffs_newinfo *pni, const struct puffs_cn *pcn)
{
	cg_node_t *node = (cg_node_t *)opc, *file;

	printf("PCN: %s\n", pcn->pcn_name);

	if (PCNISDOTDOT(pcn)) {
		if (!node->parent)
			return -ENOENT;

		puffs_newinfo_setcookie(pni, node->parent);
		puffs_newinfo_setvtype(pni, VDIR);
	}

	file = lookupfile(node, pcn->pcn_name);
	if (file) {
		puffs_newinfo_setcookie(pni, file);
		puffs_newinfo_setvtype(pni, nodevtype(file));
		puffs_newinfo_setsize(pni, 0);
		puffs_newinfo_setrdev(pni, 0);

		return 0;
	}

	return 0;
}

int
cgrpfs_node_access(struct puffs_usermount *pu, void *opc, int acc_mode,
	const struct puffs_cred *pcr)
{
	cg_node_t *node = (cg_node_t *)opc;

	printf("ACCESS\n");

	return puffs_access(nodevtype(node), node->attr.st_mode & 07777,
		node->attr.st_uid, node->attr.st_gid, acc_mode, pcr);
}

int
cgrpfs_node_getattr(struct puffs_usermount *pu, void *opc, struct vattr *va,
	const struct puffs_cred *pcred)
{
	cg_node_t *node = (cg_node_t *)opc;

	puffs_stat2vattr(va, &node->attr);

	return 0;
}
