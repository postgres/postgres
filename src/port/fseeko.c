/*-------------------------------------------------------------------------
 *
 * fseeko.c
 *	  64-bit versions of fseeko/ftello()
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/port/fseeko.c,v 1.1 2002/10/23 20:56:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifdef __bsdi__

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

/*
 *	On BSD/OS, off_t and fpos_t are the same.  Standards say
 *	off_t is an arithmetic type, but not necessarily integral,
 *	while fpos_t might be neither.
 *
 *	I don't think this is thread-safe.
 */

int
fseeko(FILE *stream, off_t offset, int whence)
{
	off_t floc;
	struct stat filestat;

	switch (whence)
	{
		case SEEK_CUR:
			if (fgetpos(stream, &floc) != 0)
				return -1;
			floc += offset;
			if (fsetpos(stream, &floc) != 0)
				return -1;
			return 0;
			break;
		case SEEK_SET:
			if (fsetpos(stream, &offset) != 0)
				return -1;
			return 0;
			break;
		case SEEK_END:
			if (fstat(fileno(stream), &filestat) != 0)
				return -1;
			floc = filestat.st_size;
			if (fsetpos(stream, &floc) != 0)
				return -1;
			return 0;
			break;
		default:
			errno =	EINVAL;
			return -1;
	}
}


off_t
ftello(FILE *stream)
{
	off_t floc;

	if (fgetpos(stream, &floc) != 0)
		return -1;
	return floc;
}
#endif
