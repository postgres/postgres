/*-------------------------------------------------------------------------
 *
 * pgtar.h
 *	  Functions for manipulating tarfile datastructures (src/port/tar.c)
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/pgtar.h
 *
 *-------------------------------------------------------------------------
 */

enum tarError
{
	TAR_OK = 0,
	TAR_NAME_TOO_LONG,
	TAR_SYMLINK_TOO_LONG
};

extern enum tarError tarCreateHeader(char *h, const char *filename, const char *linktarget,
			  pgoff_t size, mode_t mode, uid_t uid, gid_t gid, time_t mtime);
extern uint64 read_tar_number(const char *s, int len);
extern int	tarChecksum(char *header);
