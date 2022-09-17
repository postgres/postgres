/*-------------------------------------------------------------------------
 *
 * preadv.c
 *	  Implementation of preadv(2) for platforms that lack one.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/preadv.c
 *
 * Note that this implementation changes the current file position, unlike
 * the POSIX-like function, so we use the name pg_preadv().
 *
 *-------------------------------------------------------------------------
 */


#include "c.h"

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "port/pg_iovec.h"

ssize_t
pg_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
#ifdef HAVE_READV
	if (iovcnt == 1)
		return pg_pread(fd, iov[0].iov_base, iov[0].iov_len, offset);
	if (lseek(fd, offset, SEEK_SET) < 0)
		return -1;
	return readv(fd, iov, iovcnt);
#else
	ssize_t		sum = 0;
	ssize_t		part;

	for (int i = 0; i < iovcnt; ++i)
	{
		part = pg_pread(fd, iov[i].iov_base, iov[i].iov_len, offset);
		if (part < 0)
		{
			if (i == 0)
				return -1;
			else
				return sum;
		}
		sum += part;
		offset += part;
		if (part < iov[i].iov_len)
			return sum;
	}
	return sum;
#endif
}
