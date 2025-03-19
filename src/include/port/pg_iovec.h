/*-------------------------------------------------------------------------
 *
 * pg_iovec.h
 *	  Header for vectored I/O functions, to use in place of <sys/uio.h>.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/port/pg_iovec.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_IOVEC_H
#define PG_IOVEC_H

#ifndef WIN32

#include <limits.h>
#include <sys/uio.h>			/* IWYU pragma: export */
#include <unistd.h>

#else

/* POSIX requires at least 16 as a maximum iovcnt. */
#define IOV_MAX 16

/* Define our own POSIX-compatible iovec struct. */
struct iovec
{
	void	   *iov_base;
	size_t		iov_len;
};

#endif

/*
 * Define a reasonable maximum that is safe to use on the stack in arrays of
 * struct iovec and other small types.  The operating system could limit us to
 * a number as low as 16, but most systems have 1024.
 */
#define PG_IOV_MAX Min(IOV_MAX, 128)

/*
 * Like preadv(), but with a prefix to remind us of a side-effect: on Windows
 * this changes the current file position.
 */
static inline ssize_t
pg_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
#if HAVE_DECL_PREADV
	/*
	 * Avoid a small amount of argument copying overhead in the kernel if
	 * there is only one iovec.
	 */
	if (iovcnt == 1)
		return pread(fd, iov[0].iov_base, iov[0].iov_len, offset);
	else
		return preadv(fd, iov, iovcnt, offset);
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
		if ((size_t) part < iov[i].iov_len)
			return sum;
	}
	return sum;
#endif
}

/*
 * Like pwritev(), but with a prefix to remind us of a side-effect: on Windows
 * this changes the current file position.
 */
static inline ssize_t
pg_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
{
#if HAVE_DECL_PWRITEV
	/*
	 * Avoid a small amount of argument copying overhead in the kernel if
	 * there is only one iovec.
	 */
	if (iovcnt == 1)
		return pwrite(fd, iov[0].iov_base, iov[0].iov_len, offset);
	else
		return pwritev(fd, iov, iovcnt, offset);
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
		if ((size_t) part < iov[i].iov_len)
			return sum;
	}
	return sum;
#endif
}

#endif							/* PG_IOVEC_H */
