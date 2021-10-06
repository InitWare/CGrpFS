#include <sys/poll.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include "cgrpfs.h"

static int
cg_chmod(const char *path, mode_t mode)
{
	CGMGR_LOCKED;
	cg_node_t *node = lookupnode(path, false);

	if (!node)
		return -ENOENT;

	node->attr.st_mode &= ~(07777);
	node->attr.st_mode |= mode;

	return 0;
}

static int
cg_chown(const char *path, uid_t uid, gid_t gid)
{
	CGMGR_LOCKED;
	cg_node_t *node = lookupnode(path, false);

	if (!node)
		return -ENOENT;

	if (uid != -1)
		node->attr.st_uid = uid;
	if (gid != -1)
		node->attr.st_gid = gid;

	return 0;
}

static int
cg_getattr(const char *path, struct stat *st)
{
	CGMGR_LOCKED;
	cg_node_t *node = lookupnode(path, false);

	if (!node)
		return -ENOENT;

	*st = node->attr;

	return 0;
}

static int
cg_open(const char *path, struct fuse_file_info *fi)
{
	CGMGR_LOCKED;
	cg_node_t *node = lookupnode(path, false);
	cg_filedesc_t *filedesc;

	if (!node)
		return -ENOENT;

	filedesc = malloc(sizeof *filedesc);
	if (!filedesc)
		return -ENOMEM;
	filedesc->node = node;
	filedesc->buf = NULL;

	fi->fh = (uintptr_t)filedesc;
	fi->direct_io = 1;

	if (node->type != CGN_PROCS && node->type != CGN_PID_CGROUP)
		return -ENOTSUP;

	filedesc->buf = nodetxt(node);
	if (!filedesc->buf) {
		free(filedesc);
		return -ENOMEM;
			}

	return 0;
}

#ifndef SVC_PLATFORM_OpenBSD
static int
cg_poll(const char *path, struct fuse_file_info *fi, struct fuse_pollhandle *ph,
	unsigned *reventsp)
{
	*reventsp = POLLIN | POLLHUP;
	fuse_notify_poll(ph);
	return 0;
}
#endif

static int
cg_read(const char *path, char *buf, size_t len, off_t off,
	struct fuse_file_info *fi)
{
	CGMGR_LOCKED;
	cg_filedesc_t *filedesc = (void *)fi->fh;
	size_t maxlen;

	assert(filedesc);

	if (!filedesc->buf)
		return 0;

	maxlen = strlen(filedesc->buf);
	if (off > maxlen)
		return 0;
	else if (len < maxlen - off)
		maxlen = len;
	else
		maxlen -= off;

	memcpy(buf, filedesc->buf + off, maxlen);

	return maxlen;
}

static int
cg_write(const char *path, const char *buf, size_t len, off_t off,
	struct fuse_file_info *fi)
{
	CGMGR_LOCKED;
	cg_filedesc_t *filedesc = (void *)fi->fh;
	cg_node_t *node = filedesc->node;

	assert(node);

	if (node->type == CGN_PROCS) {
		long pid;
		int r;

		if (sscanf(buf, "%ld\n", &pid) < 1)
			return -EINVAL;

		r = attachpid(node->parent, pid);

		if (r < 0)
			return r;
		return len;
	} else
		return -ENODEV;
}

static int
cg_release(const char *path, struct fuse_file_info *fi)
{
	CGMGR_LOCKED;
	cg_filedesc_t *filedesc = (void *)fi->fh;

	assert(filedesc);
	free(filedesc->buf);
	free(filedesc);

	return 0;
}

static int
cg_opendir(const char *path, struct fuse_file_info *fi)
{
	CGMGR_LOCKED;
	cg_node_t *node = lookupnode(path, false);

	if (!node)
		return -ENOENT;
	else if (node->type != CGN_CG_DIR && node->type != CGN_PID_ROOT_DIR &&
		node->type != CGN_PID_DIR)
		return -ENOTDIR;

	fi->fh = (uintptr_t)node;

	return 0;
}

static int
cg_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
	struct fuse_file_info *fi)
{
	CGMGR_LOCKED;
	cg_node_t *node = (cg_node_t *)fi->fh;
	cg_node_t *dirent;

	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	LIST_FOREACH (dirent, &node->subnodes, entries) {
		filler(buf, dirent->name, &dirent->attr, 0);
	}

	return 0;
}

int
cg_mkdir(const char *path, mode_t mode)
{
	CGMGR_LOCKED;
	cg_node_t *node = lookupnode(path, false);
	cg_node_t *newdir;
	struct fuse_context *ctx = fuse_get_context();
	char *dirname = strrchr(path, '/');

	if (node != NULL)
		return -EEXIST;

	/* get containing node */
	node = lookupnode(path, true);

	if (!node)
		return -ENOENT;
	else if (node->type != CGN_CG_DIR)
		return -ENOTSUP;

	newdir = newcgdir(node, dirname + 1, 0755 & ~ctx->umask, ctx->uid,
		ctx->gid);
	if (!newdir)
		return -ENOMEM;

	return 0;
}

static int
cg_rmdir(const char *path)
{
	CGMGR_LOCKED;
	cg_node_t *node = lookupnode(path, false);

	if (!node)
		return -ENOENT;
	else if (node->type != CGN_CG_DIR || node == cgmgr.rootnode)
		return -ENOTSUP;

	delnode(node);

	return 0;
}

static int
cg_rename(const char *oldpath, const char *newpath)
{
	CGMGR_LOCKED;
	cg_node_t *old = lookupnode(oldpath, false);
	cg_node_t *newparent = lookupnode(newpath, true);
	char *dirname = strrchr(newpath, '/');

	if (!old || !newparent)
		return -ENOENT;
	else if (old->parent != newparent)
		return -EOPNOTSUPP;
	else if (old->type != CGN_CG_DIR || newparent->type != CGN_CG_DIR)
		return -EOPNOTSUPP;

	free(old->name);
	old->name = strdup(dirname);

	return 0;
}

struct fuse_operations cgops = {
	.chmod = cg_chmod,
	.chown = cg_chown,
	.getattr = cg_getattr,
	.open = cg_open,
#ifndef SVC_PLATFORM_OpenBSD /* no fuse poll on OpenBSD */
	.poll = cg_poll,
#endif
	.read = cg_read,
	.write = cg_write,
	.release = cg_release,
	.opendir = cg_opendir,
	.readdir = cg_readdir,
	.mkdir = cg_mkdir,
	.rmdir = cg_rmdir,
	.rename = cg_rename,
};
