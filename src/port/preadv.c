/*-------------------------------------------------------------------------
 *
 * preadv.c
 *	  Implementation of preadv(2) for platforms that lack one.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/preadv.c
 *
 *-------------------------------------------------------------------------
 */


#include "c.h"

#include <unistd.h>

#include "port/pg_iovec.h"

ssize_t
pg_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
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
}
