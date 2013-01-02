/*-------------------------------------------------------------------------
 *
 * pgtar.h
 *	  Functions for manipulating tarfile datastructures (src/port/tar.c)
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/pgtar.h
 *
 *-------------------------------------------------------------------------
 */
extern void tarCreateHeader(char *h, const char *filename, const char *linktarget, size_t size, mode_t mode, uid_t uid, gid_t gid, time_t mtime);
extern int	tarChecksum(char *header);
