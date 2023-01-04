/*-------------------------------------------------------------------------
 *
 * win32error.c
 *	  Map win32 error codes to errno values
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/port/win32error.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

static const struct
{
	DWORD		winerr;
	int			doserr;
}			doserrors[] =

{
	{
		ERROR_INVALID_FUNCTION, EINVAL
	},
	{
		ERROR_FILE_NOT_FOUND, ENOENT
	},
	{
		ERROR_PATH_NOT_FOUND, ENOENT
	},
	{
		ERROR_TOO_MANY_OPEN_FILES, EMFILE
	},
	{
		ERROR_ACCESS_DENIED, EACCES
	},
	{
		ERROR_INVALID_HANDLE, EBADF
	},
	{
		ERROR_ARENA_TRASHED, ENOMEM
	},
	{
		ERROR_NOT_ENOUGH_MEMORY, ENOMEM
	},
	{
		ERROR_INVALID_BLOCK, ENOMEM
	},
	{
		ERROR_BAD_ENVIRONMENT, E2BIG
	},
	{
		ERROR_BAD_FORMAT, ENOEXEC
	},
	{
		ERROR_INVALID_ACCESS, EINVAL
	},
	{
		ERROR_INVALID_DATA, EINVAL
	},
	{
		ERROR_INVALID_DRIVE, ENOENT
	},
	{
		ERROR_CURRENT_DIRECTORY, EACCES
	},
	{
		ERROR_NOT_SAME_DEVICE, EXDEV
	},
	{
		ERROR_NO_MORE_FILES, ENOENT
	},
	{
		ERROR_LOCK_VIOLATION, EACCES
	},
	{
		ERROR_SHARING_VIOLATION, EACCES
	},
	{
		ERROR_BAD_NETPATH, ENOENT
	},
	{
		ERROR_NETWORK_ACCESS_DENIED, EACCES
	},
	{
		ERROR_BAD_NET_NAME, ENOENT
	},
	{
		ERROR_FILE_EXISTS, EEXIST
	},
	{
		ERROR_CANNOT_MAKE, EACCES
	},
	{
		ERROR_FAIL_I24, EACCES
	},
	{
		ERROR_INVALID_PARAMETER, EINVAL
	},
	{
		ERROR_NO_PROC_SLOTS, EAGAIN
	},
	{
		ERROR_DRIVE_LOCKED, EACCES
	},
	{
		ERROR_BROKEN_PIPE, EPIPE
	},
	{
		ERROR_DISK_FULL, ENOSPC
	},
	{
		ERROR_INVALID_TARGET_HANDLE, EBADF
	},
	{
		ERROR_INVALID_HANDLE, EINVAL
	},
	{
		ERROR_WAIT_NO_CHILDREN, ECHILD
	},
	{
		ERROR_CHILD_NOT_COMPLETE, ECHILD
	},
	{
		ERROR_DIRECT_ACCESS_HANDLE, EBADF
	},
	{
		ERROR_NEGATIVE_SEEK, EINVAL
	},
	{
		ERROR_SEEK_ON_DEVICE, EACCES
	},
	{
		ERROR_DIR_NOT_EMPTY, ENOTEMPTY
	},
	{
		ERROR_NOT_LOCKED, EACCES
	},
	{
		ERROR_BAD_PATHNAME, ENOENT
	},
	{
		ERROR_MAX_THRDS_REACHED, EAGAIN
	},
	{
		ERROR_LOCK_FAILED, EACCES
	},
	{
		ERROR_ALREADY_EXISTS, EEXIST
	},
	{
		ERROR_FILENAME_EXCED_RANGE, ENOENT
	},
	{
		ERROR_NESTING_NOT_ALLOWED, EAGAIN
	},
	{
		ERROR_NOT_ENOUGH_QUOTA, ENOMEM
	},
	{
		ERROR_DELETE_PENDING, ENOENT
	},
	{
		ERROR_INVALID_NAME, ENOENT
	},
	{
		ERROR_CANT_RESOLVE_FILENAME, ENOENT
	}
};

void
_dosmaperr(unsigned long e)
{
	int			i;

	if (e == 0)
	{
		errno = 0;
		return;
	}

	for (i = 0; i < lengthof(doserrors); i++)
	{
		if (doserrors[i].winerr == e)
		{
			int			doserr = doserrors[i].doserr;

#ifndef FRONTEND
			ereport(DEBUG5,
					(errmsg_internal("mapped win32 error code %lu to %d",
									 e, doserr)));
#elif defined(FRONTEND_DEBUG)
			fprintf(stderr, "mapped win32 error code %lu to %d", e, doserr);
#endif
			errno = doserr;
			return;
		}
	}

#ifndef FRONTEND
	ereport(LOG,
			(errmsg_internal("unrecognized win32 error code: %lu",
							 e)));
#else
	fprintf(stderr, "unrecognized win32 error code: %lu", e);
#endif

	errno = EINVAL;
}
