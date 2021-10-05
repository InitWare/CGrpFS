/*
 * vnode operations for PUFFS-based cgrpfs
 */

#include "cgrpfs.h"

#include <sys/poll.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>

static void
setaccessed(cg_node_t *node)
{
	node->accessed++;
}

static int
nodevtype(cg_node_t *node)
{
	switch (node->type) {
	case CGN_EVENTS:
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
	CGMGR_LOCKED;
	cg_node_t *node = (cg_node_t *)opc, *file;

	if (PCNISDOTDOT(pcn)) {
		if (!node->parent)
			return -ENOENT;

		puffs_newinfo_setcookie(pni, node->parent);
		setaccessed(node->parent);
		puffs_newinfo_setvtype(pni, VDIR);

		return 0;
	}

	file = lookupfile(node, pcn->pcn_name);
	if (file) {
		puffs_newinfo_setcookie(pni, file);
		setaccessed(file);
		puffs_newinfo_setvtype(pni, nodevtype(file));
		puffs_newinfo_setsize(pni, 0);
		puffs_newinfo_setrdev(pni, 0);

		return 0;
	}

	if ((pcn->pcn_flags & NAMEI_ISLASTCN) &&
		(pcn->pcn_nameiop == NAMEI_CREATE ||
			pcn->pcn_nameiop == NAMEI_RENAME)) {
		int r = puffs_access(VDIR, node->attr.st_mode,
			node->attr.st_uid, node->attr.st_gid, PUFFS_VWRITE,
			pcn->pcn_cred);
		if (r)
			return r;
	}

	return ENOENT;
}

int
cgrpfs_node_mkdir(struct puffs_usermount *pu, void *opc,
	struct puffs_newinfo *pni, const struct puffs_cn *pcn,
	const struct vattr *va)
{
	CGMGR_LOCKED;
	cg_node_t *node_parent = (cg_node_t *)opc;
	cg_node_t *node_new;
	uid_t uid;
	gid_t gid;

	if (node_parent->type != CGN_CG_DIR)
		return EOPNOTSUPP;

	if (lookupfile(node_parent, pcn->pcn_name) != NULL)
		return EEXIST;

	assert(puffs_cred_getuid(pcn->pcn_cred, &uid) == 0);
	assert(puffs_cred_getgid(pcn->pcn_cred, &gid) == 0);

	// FIXME: umask? And I don't think we have any further info to extract
	// from vattr.
	node_new = newcgdir(node_parent, pcn->pcn_name, va->va_mode & 07777,
		uid, gid);

	setaccessed(node_new);
	puffs_newinfo_setcookie(pni, node_new);

	return 0;
}

int
cgrpfs_node_rmdir(struct puffs_usermount *pu, void *opc, void *targ,
	const struct puffs_cn *pcn)
{
	CGMGR_LOCKED;
	cg_node_t *node = (cg_node_t *)targ;

	if (node->type != CGN_CG_DIR || node == cgmgr.rootnode)
		return -ENOTSUP;

	removenode(node);
	puffs_setback(puffs_cc_getcc(pu), PUFFS_SETBACK_NOREF_N2);

	return 0;
}

int
cgrpfs_node_access(struct puffs_usermount *pu, void *opc, int acc_mode,
	const struct puffs_cred *pcr)
{
	CGMGR_LOCKED;
	cg_node_t *node = (cg_node_t *)opc;

	return puffs_access(nodevtype(node), node->attr.st_mode & 07777,
		node->attr.st_uid, node->attr.st_gid, acc_mode, pcr);
}

int
cgrpfs_node_getattr(struct puffs_usermount *pu, void *opc, struct vattr *va,
	const struct puffs_cred *pcred)
{
	CGMGR_LOCKED;
	cg_node_t *node = (cg_node_t *)opc;

	puffs_stat2vattr(va, &node->attr);

	return 0;
}

int
cgrpfs_node_setattr(struct puffs_usermount *pu, void *opc,
	const struct vattr *va, const struct puffs_cred *pcr)
{
	CGMGR_LOCKED;
	cg_node_t *node = (cg_node_t *)opc;
	int rv;

	/* check permissions */
	if (va->va_flags != PUFFS_VNOVAL)
		return EOPNOTSUPP;

	if (va->va_uid != PUFFS_VNOVAL || va->va_gid != PUFFS_VNOVAL) {
		rv = puffs_access_chown(node->attr.st_uid, node->attr.st_gid,
			va->va_uid, va->va_gid, pcr);
		if (rv)
			return rv;
		if (va->va_uid != PUFFS_VNOVAL)
			node->attr.st_uid = va->va_uid;
		if (va->va_gid != PUFFS_VNOVAL)
			node->attr.st_gid = va->va_gid;
	}

	if (va->va_mode != PUFFS_VNOVAL) {
		rv = puffs_access_chmod(node->attr.st_uid, node->attr.st_gid,
			nodevtype(node), node->attr.st_mode & 07777, pcr);
		if (rv)
			return rv;
		node->attr.st_mode &= ~(07777);
		node->attr.st_mode |= va->va_mode & 07777;
	}

	if ((va->va_atime.tv_sec != PUFFS_VNOVAL &&
		    va->va_atime.tv_nsec != PUFFS_VNOVAL) ||
		(va->va_mtime.tv_sec != PUFFS_VNOVAL &&
			va->va_mtime.tv_nsec != PUFFS_VNOVAL)) {
		rv = puffs_access_times(node->attr.st_uid, node->attr.st_gid,
			node->attr.st_mode & 07777,
			va->va_vaflags & VA_UTIMES_NULL, pcr);
		if (rv)
			return rv;
		if (va->va_atime.tv_sec != PUFFS_VNOVAL)
			node->attr.st_atim.tv_sec = va->va_atime.tv_sec;
		if (va->va_atime.tv_nsec != PUFFS_VNOVAL)
			node->attr.st_atim.tv_nsec = va->va_atime.tv_nsec;
		if (va->va_mtime.tv_sec != PUFFS_VNOVAL)
			node->attr.st_mtim.tv_sec = va->va_mtime.tv_sec;
		if (va->va_mtime.tv_nsec != PUFFS_VNOVAL)
			node->attr.st_mtim.tv_nsec = va->va_mtime.tv_nsec;
	}

	if (va->va_size != PUFFS_VNOVAL)
		return EOPNOTSUPP;

	return 0;
}

/* xxx: not usable until PUFFS fixed in NetBSD. */
int
cgrpfs_node_poll(struct puffs_usermount *pu, void *opc, int *revents)
{
	CGMGR_LOCKED;
	*revents &= POLLIN | POLLHUP;
	return EOPNOTSUPP;
}

int
cgrpfs_node_readdir(struct puffs_usermount *pu, void *opc, struct dirent *dent,
	off_t *readoff, size_t *reslen, const struct puffs_cred *pcr,
	int *eofflag, off_t *cookies, size_t *ncookies)
{
	CGMGR_LOCKED;
	cg_node_t *node = (cg_node_t *)opc;
	cg_node_t *subnode; /* iterator */
	int i = 0;

	if (nodevtype(node) != VDIR)
		return ENOTDIR;

	*ncookies = 0;
again:
	if (*readoff == DENT_DOT || *readoff == DENT_DOTDOT) {
		puffs_gendotdent(&dent, (ino_t)node, *readoff, reslen);
		(*readoff)++;
		PUFFS_STORE_DCOOKIE(cookies, ncookies, *readoff);
		goto again;
	}

	LIST_FOREACH (subnode, &node->subnodes, entries) {
		if (i < DENT_ADJ(*readoff))
			continue;

		i++;

		if (!puffs_nextdent(&dent, subnode->name, (ino_t)subnode,
			    puffs_vtype2dt(nodevtype(subnode)), reslen))
			return 0;

		(*readoff)++;
		PUFFS_STORE_DCOOKIE(cookies, ncookies, *readoff);
	}

	*eofflag = 1;

	return 0;
}

int
cgrpfs_node_open(struct puffs_usermount *pu, void *opc, int modep,
	const struct puffs_cred *pcr)
{
	return 0;
}

int
cgrpfs_node_open2(struct puffs_usermount *pu, void *opc, int modep,
	const struct puffs_cred *pcr, int *oflags)
{
	*oflags |= PUFFS_OPEN_IO_DIRECT;
	return 0;
}

int
cgrpfs_node_rename(struct puffs_usermount *pu, void *opc, void *src,
	const struct puffs_cn *pcn_src, void *targ_dir, void *targ,
	const struct puffs_cn *pcn_targ)
{
	CGMGR_LOCKED;
	cg_node_t *cgn_sdir = opc;
	cg_node_t *cgn_sfile = src;
	cg_node_t *cgn_tdir = targ_dir;
	/* Target file doesn't matter. It doesn't exist yet. */

	if (cgn_sdir != cgn_tdir)
		return EPERM; /* only rename within same dir */
	else if (cgn_sfile->type != CGN_CG_DIR)
		return EOPNOTSUPP; /* only cgdirs may be renamed */

	// TODO: double check source still exists?

	free(cgn_sfile->name);
	cgn_sfile->name = strdup(pcn_targ->pcn_name);

	return 0;
}

int
cgrpfs_node_read(struct puffs_usermount *pu, void *opc, uint8_t *buf,
	off_t offset, size_t *resid, const struct puffs_cred *pcr, int ioflag)
{
	CGMGR_LOCKED;
	cg_node_t *node = opc;
	char *txt;
	size_t maxlen;

	txt = nodetxt(node);

	if (!txt)
		return ENOMEM;

	maxlen = strlen(txt);
	if (offset > maxlen)
		return 0;
	else if (*resid < maxlen - offset)
		maxlen = *resid;
	else
		maxlen -= offset;

	memcpy(buf, txt + offset, maxlen);

	*resid -= maxlen;

	return 0;
}

int
cgrpfs_node_write(struct puffs_usermount *pu, void *opc, uint8_t *buf,
	off_t offset, size_t *resid, const struct puffs_cred *pcr, int ioflag)
{
	CGMGR_LOCKED;
	cg_node_t *node = opc;

	if (node->type == CGN_PROCS) {
		long pid;
		int r;

		if (sscanf((const char *)buf, "%ld\n", &pid) < 1)
			return -EINVAL;

		r = attachpid(node->parent, pid);
		if (r < 0)
			return -r;

		*resid = 0;

		return 0;
	} else
		return ENODEV;
}

int
cgrpfs_node_inactive(struct puffs_usermount *pu, void *opc)
{
	puffs_setback(puffs_cc_getcc(pu), PUFFS_SETBACK_NOREF_N1);
	return 0;
}

int
cgrpfs_node_reclaim(struct puffs_usermount *pu, void *opc)
{
	CGMGR_LOCKED;
	cg_node_t *node = opc;

	if (node->todel)
		delnode(node);
	else
		node->accessed = 0;

	return 0;
}