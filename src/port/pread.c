/*-------------------------------------------------------------------------
 *
 * pread.c
 *	  Implementation of pread[v](2) for platforms that lack one.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pread.c
 *
 * Note that this implementation changes the current file position, unlike
 * the POSIX function, so we use the name pg_pread().  Likewise for the
 * iovec version.
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "port/pg_iovec.h"

#ifndef HAVE_PREAD
ssize_t
pg_pread(int fd, void *buf, size_t size, off_t offset)
{
#ifdef WIN32
	OVERLAPPED	overlapped = {0};
	HANDLE		handle;
	DWORD		result;

	handle = (HANDLE) _get_osfhandle(fd);
	if (handle == INVALID_HANDLE_VALUE)
	{
		errno = EBADF;
		return -1;
	}

	overlapped.Offset = offset;
	if (!ReadFile(handle, buf, size, &result, &overlapped))
	{
		if (GetLastError() == ERROR_HANDLE_EOF)
			return 0;

		_dosmaperr(GetLastError());
		return -1;
	}

	return result;
#else
	if (lseek(fd, offset, SEEK_SET) < 0)
		return -1;

	return read(fd, buf, size);
#endif
}
#endif

#ifndef HAVE_PREADV
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
	ssize_t 	sum = 0;
	ssize_t 	part;

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
#endif
