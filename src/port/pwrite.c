/*-------------------------------------------------------------------------
 *
 * pwrite.c
 *	  Implementation of pwrite[v](2) for platforms that lack one.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/pwrite.c
 *
 * Note that this implementation changes the current file position, unlike
 * the POSIX function, so we use the name pg_pwrite().  Likewise for the
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

#ifndef HAVE_PWRITE
ssize_t
pg_pwrite(int fd, const void *buf, size_t size, off_t offset)
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
	if (!WriteFile(handle, buf, size, &result, &overlapped))
	{
		_dosmaperr(GetLastError());
		return -1;
	}

	return result;
#else
	if (lseek(fd, offset, SEEK_SET) < 0)
		return -1;

	return write(fd, buf, size);
#endif
}
#endif

#ifndef HAVE_PWRITEV
ssize_t
pg_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
#ifdef HAVE_WRITEV
	if (iovcnt == 1)
		return pg_pwrite(fd, iov[0].iov_base, iov[0].iov_len, offset);
	if (lseek(fd, offset, SEEK_SET) < 0)
		return -1;
	return writev(fd, iov, iovcnt);
#else
	ssize_t		sum = 0;
	ssize_t		part;

	for (int i = 0; i < iovcnt; ++i)
	{
		part = pg_pwrite(fd, iov[i].iov_base, iov[i].iov_len, offset);
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

/*
 * A convenience wrapper for pg_pwritev() that retries on partial write.  If an
 * error is returned, it is unspecified how much has been written.
 */
ssize_t
pg_pwritev_with_retry(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
	struct iovec iov_copy[PG_IOV_MAX];
	ssize_t		sum = 0;
	ssize_t		part;

	/* We'd better have space to make a copy, in case we need to retry. */
	if (iovcnt > PG_IOV_MAX)
	{
		errno = EINVAL;
		return -1;
	}

	for (;;)
	{
		/* Write as much as we can. */
		part = pg_pwritev(fd, iov, iovcnt, offset);
		if (part < 0)
			return -1;

#ifdef SIMULATE_SHORT_WRITE
		part = Min(part, 4096);
#endif

		/* Count our progress. */
		sum += part;
		offset += part;

		/* Step over iovecs that are done. */
		while (iovcnt > 0 && iov->iov_len <= part)
		{
			part -= iov->iov_len;
			++iov;
			--iovcnt;
		}

		/* Are they all done? */
		if (iovcnt == 0)
		{
			/* We don't expect the kernel to write more than requested. */
			Assert(part == 0);
			break;
		}

		/*
		 * Move whatever's left to the front of our mutable copy and adjust
		 * the leading iovec.
		 */
		Assert(iovcnt > 0);
		memmove(iov_copy, iov, sizeof(*iov) * iovcnt);
		Assert(iov->iov_len > part);
		iov_copy[0].iov_base = (char *) iov_copy[0].iov_base + part;
		iov_copy[0].iov_len -= part;
		iov = iov_copy;
	}

	return sum;
}
