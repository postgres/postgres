/*-------------------------------------------------------------------------
 *
 * pgtar.h
 *	  Functions for manipulating tarfile datastructures (src/port/tar.c)
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/pgtar.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TAR_H
#define PG_TAR_H

#define		TAR_BLOCK_SIZE	512

enum tarError
{
	TAR_OK = 0,
	TAR_NAME_TOO_LONG,
	TAR_SYMLINK_TOO_LONG
};

extern enum tarError tarCreateHeader(char *h, const char *filename,
									 const char *linktarget, pgoff_t size,
									 mode_t mode, uid_t uid, gid_t gid,
									 time_t mtime);
extern uint64 read_tar_number(const char *s, int len);
extern void print_tar_number(char *s, int len, uint64 val);
extern int	tarChecksum(char *header);

/*
 * Compute the number of padding bytes required for an entry in a tar
 * archive. We must pad out to a multiple of TAR_BLOCK_SIZE. Since that's
 * a power of 2, we can use TYPEALIGN().
 */
static inline size_t
tarPaddingBytesRequired(size_t len)
{
	return TYPEALIGN(TAR_BLOCK_SIZE, len) - len;
}

#endif
