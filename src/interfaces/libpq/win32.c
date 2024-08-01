/*
 * src/interfaces/libpq/win32.c
 *
 *
 *	FILE
 *		win32.c
 *
 *	DESCRIPTION
 *		Win32 support functions.
 *
 * Contains table and functions for looking up win32 socket error
 * descriptions. But will/may contain other win32 helper functions
 * for libpq.
 *
 * The error constants are taken from the Frambak Bakfram LGSOCKET
 * library guys who in turn took them from the Winsock FAQ.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 */

/* Make stuff compile faster by excluding not used stuff */

#define VC_EXTRALEAN
#ifndef __MINGW32__
#define NOGDI
#endif
#define NOCRYPT

#include "postgres_fe.h"

#include "win32.h"

/* Declared here to avoid pulling in all includes, which causes name collisions */
#ifdef ENABLE_NLS
extern char *libpq_gettext(const char *msgid) pg_attribute_format_arg(1);
#else
#define libpq_gettext(x) (x)
#endif


static struct WSErrorEntry
{
	DWORD		error;
	const char *description;
}			WSErrors[] =

{
	{
		0, "No error"
	},
	{
		WSAEINTR, "Interrupted system call"
	},
	{
		WSAEBADF, "Bad file number"
	},
	{
		WSAEACCES, "Permission denied"
	},
	{
		WSAEFAULT, "Bad address"
	},
	{
		WSAEINVAL, "Invalid argument"
	},
	{
		WSAEMFILE, "Too many open sockets"
	},
	{
		WSAEWOULDBLOCK, "Operation would block"
	},
	{
		WSAEINPROGRESS, "Operation now in progress"
	},
	{
		WSAEALREADY, "Operation already in progress"
	},
	{
		WSAENOTSOCK, "Socket operation on non-socket"
	},
	{
		WSAEDESTADDRREQ, "Destination address required"
	},
	{
		WSAEMSGSIZE, "Message too long"
	},
	{
		WSAEPROTOTYPE, "Protocol wrong type for socket"
	},
	{
		WSAENOPROTOOPT, "Bad protocol option"
	},
	{
		WSAEPROTONOSUPPORT, "Protocol not supported"
	},
	{
		WSAESOCKTNOSUPPORT, "Socket type not supported"
	},
	{
		WSAEOPNOTSUPP, "Operation not supported on socket"
	},
	{
		WSAEPFNOSUPPORT, "Protocol family not supported"
	},
	{
		WSAEAFNOSUPPORT, "Address family not supported"
	},
	{
		WSAEADDRINUSE, "Address already in use"
	},
	{
		WSAEADDRNOTAVAIL, "Cannot assign requested address"
	},
	{
		WSAENETDOWN, "Network is down"
	},
	{
		WSAENETUNREACH, "Network is unreachable"
	},
	{
		WSAENETRESET, "Net connection reset"
	},
	{
		WSAECONNABORTED, "Software caused connection abort"
	},
	{
		WSAECONNRESET, "Connection reset by peer"
	},
	{
		WSAENOBUFS, "No buffer space available"
	},
	{
		WSAEISCONN, "Socket is already connected"
	},
	{
		WSAENOTCONN, "Socket is not connected"
	},
	{
		WSAESHUTDOWN, "Cannot send after socket shutdown"
	},
	{
		WSAETOOMANYREFS, "Too many references, cannot splice"
	},
	{
		WSAETIMEDOUT, "Connection timed out"
	},
	{
		WSAECONNREFUSED, "Connection refused"
	},
	{
		WSAELOOP, "Too many levels of symbolic links"
	},
	{
		WSAENAMETOOLONG, "File name too long"
	},
	{
		WSAEHOSTDOWN, "Host is down"
	},
	{
		WSAEHOSTUNREACH, "No route to host"
	},
	{
		WSAENOTEMPTY, "Directory not empty"
	},
	{
		WSAEPROCLIM, "Too many processes"
	},
	{
		WSAEUSERS, "Too many users"
	},
	{
		WSAEDQUOT, "Disc quota exceeded"
	},
	{
		WSAESTALE, "Stale NFS file handle"
	},
	{
		WSAEREMOTE, "Too many levels of remote in path"
	},
	{
		WSASYSNOTREADY, "Network system is unavailable"
	},
	{
		WSAVERNOTSUPPORTED, "Winsock version out of range"
	},
	{
		WSANOTINITIALISED, "WSAStartup not yet called"
	},
	{
		WSAEDISCON, "Graceful shutdown in progress"
	},
	{
		WSAHOST_NOT_FOUND, "Host not found"
	},
	{
		WSATRY_AGAIN, "NA Host not found / SERVFAIL"
	},
	{
		WSANO_RECOVERY, "Non recoverable FORMERR||REFUSED||NOTIMP"
	},
	{
		WSANO_DATA, "No host data of that type was found"
	},
	{
		0, 0
	}							/* End of table */
};


/*
 * Returns 0 if not found, linear but who cares, at this moment
 * we're already in pain :)
 */

static int
LookupWSErrorMessage(DWORD err, char *dest)
{
	struct WSErrorEntry *e;

	for (e = WSErrors; e->description; e++)
	{
		if (e->error == err)
		{
			strcpy(dest, e->description);
			return 1;
		}
	}
	return 0;
}


static struct MessageDLL
{
	const char *dll_name;
	void	   *handle;
	int			loaded;			/* BOOL */
}			dlls[] =

{
	{
		"netmsg.dll", 0, 0
	},
	{
		"winsock.dll", 0, 0
	},
	{
		"ws2_32.dll", 0, 0
	},
	{
		"wsock32n.dll", 0, 0
	},
	{
		"mswsock.dll", 0, 0
	},
	{
		"ws2help.dll", 0, 0
	},
	{
		"ws2thk.dll", 0, 0
	},
	{
		0, 0, 1
	}							/* Last one, no dll, always loaded */
};

#define DLLS_SIZE (sizeof(dlls)/sizeof(struct MessageDLL))

/*
 * Returns a description of the socket error by first trying
 * to find it in the lookup table, and if that fails, tries
 * to load any of the winsock dlls to find that message.
 */

const char *
winsock_strerror(int err, char *strerrbuf, size_t buflen)
{
	unsigned long flags;
	int			offs,
				i;
	int			success = LookupWSErrorMessage(err, strerrbuf);

	for (i = 0; !success && i < DLLS_SIZE; i++)
	{

		if (!dlls[i].loaded)
		{
			dlls[i].loaded = 1; /* Only load once */
			dlls[i].handle = (void *) LoadLibraryEx(dlls[i].dll_name,
													0,
													LOAD_LIBRARY_AS_DATAFILE);
		}

		if (dlls[i].dll_name && !dlls[i].handle)
			continue;			/* Didn't load */

		flags = FORMAT_MESSAGE_FROM_SYSTEM
			| FORMAT_MESSAGE_IGNORE_INSERTS
			| (dlls[i].handle ? FORMAT_MESSAGE_FROM_HMODULE : 0);

		success = 0 != FormatMessage(flags,
									 dlls[i].handle, err,
									 MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
									 strerrbuf, buflen - 64,
									 0);
	}

	if (!success)
		sprintf(strerrbuf, libpq_gettext("unrecognized socket error: 0x%08X/%d"), err, err);
	else
	{
		strerrbuf[buflen - 1] = '\0';
		offs = strlen(strerrbuf);
		if (offs > (int) buflen - 64)
			offs = buflen - 64;
		sprintf(strerrbuf + offs, " (0x%08X/%d)", err, err);
	}
	return strerrbuf;
}
