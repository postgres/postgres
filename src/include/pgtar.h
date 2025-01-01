/*-------------------------------------------------------------------------
 *
 * pgtar.h
 *	  Functions for manipulating tarfile datastructures (src/port/tar.c)
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
	TAR_SYMLINK_TOO_LONG,
};

/*
 * Offsets of fields within a 512-byte tar header.
 *
 * "tar number" values should be generated using print_tar_number() and can be
 * read using read_tar_number(). Fields that contain strings are generally
 * both filled and read using strlcpy().
 *
 * The value for the checksum field can be computed using tarChecksum().
 *
 * Some fields are not used by PostgreSQL; see tarCreateHeader().
 */
enum tarHeaderOffset
{
	TAR_OFFSET_NAME = 0,		/* 100 byte string */
	TAR_OFFSET_MODE = 100,		/* 8 byte tar number, excludes S_IFMT */
	TAR_OFFSET_UID = 108,		/* 8 byte tar number */
	TAR_OFFSET_GID = 116,		/* 8 byte tar number */
	TAR_OFFSET_SIZE = 124,		/* 8 byte tar number */
	TAR_OFFSET_MTIME = 136,		/* 12 byte tar number */
	TAR_OFFSET_CHECKSUM = 148,	/* 8 byte tar number */
	TAR_OFFSET_TYPEFLAG = 156,	/* 1 byte file type, see TAR_FILETYPE_* */
	TAR_OFFSET_LINKNAME = 157,	/* 100 byte string */
	TAR_OFFSET_MAGIC = 257,		/* "ustar" with terminating zero byte */
	TAR_OFFSET_VERSION = 263,	/* "00" */
	TAR_OFFSET_UNAME = 265,		/* 32 byte string */
	TAR_OFFSET_GNAME = 297,		/* 32 byte string */
	TAR_OFFSET_DEVMAJOR = 329,	/* 8 byte tar number */
	TAR_OFFSET_DEVMINOR = 337,	/* 8 byte tar number */
	TAR_OFFSET_PREFIX = 345,	/* 155 byte string */
	/* last 12 bytes of the 512-byte block are unassigned */
};

enum tarFileType
{
	TAR_FILETYPE_PLAIN = '0',
	TAR_FILETYPE_SYMLINK = '2',
	TAR_FILETYPE_DIRECTORY = '5',
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
