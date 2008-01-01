/*-------------------------------------------------------------------------
 *
 * socket.c
 *	  Microsoft Windows Win32 Socket Functions
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/port/win32/socket.c,v 1.20 2008/01/01 19:45:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#undef socket
#undef accept
#undef connect
#undef select
#undef recv
#undef send

/*
 * Blocking socket functions implemented so they listen on both
 * the socket and the signal event, required for signal handling.
 */

/*
 * Convert the last socket error code into errno
 */
static void
TranslateSocketError(void)
{
	switch (WSAGetLastError())
	{
		case WSANOTINITIALISED:
		case WSAENETDOWN:
		case WSAEINPROGRESS:
		case WSAEINVAL:
		case WSAESOCKTNOSUPPORT:
		case WSAEFAULT:
		case WSAEINVALIDPROVIDER:
		case WSAEINVALIDPROCTABLE:
		case WSAEMSGSIZE:
			errno = EINVAL;
			break;
		case WSAEAFNOSUPPORT:
			errno = EAFNOSUPPORT;
			break;
		case WSAEMFILE:
			errno = EMFILE;
			break;
		case WSAENOBUFS:
			errno = ENOBUFS;
			break;
		case WSAEPROTONOSUPPORT:
		case WSAEPROTOTYPE:
			errno = EPROTONOSUPPORT;
			break;
		case WSAECONNREFUSED:
			errno = ECONNREFUSED;
			break;
		case WSAEINTR:
			errno = EINTR;
			break;
		case WSAENOTSOCK:
			errno = EBADFD;
			break;
		case WSAEOPNOTSUPP:
			errno = EOPNOTSUPP;
			break;
		case WSAEWOULDBLOCK:
			errno = EWOULDBLOCK;
			break;
		case WSAEACCES:
			errno = EACCES;
			break;
		case WSAENOTCONN:
		case WSAENETRESET:
		case WSAECONNRESET:
		case WSAESHUTDOWN:
		case WSAECONNABORTED:
		case WSAEDISCON:
			errno = ECONNREFUSED;		/* ENOTCONN? */
			break;
		default:
			ereport(NOTICE,
					(errmsg_internal("Unknown win32 socket error code: %i", WSAGetLastError())));
			errno = EINVAL;
	}
}

static int
pgwin32_poll_signals(void)
{
	if (UNBLOCKED_SIGNAL_QUEUE())
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		return 1;
	}
	return 0;
}

static int
isDataGram(SOCKET s)
{
	int			type;
	int			typelen = sizeof(type);

	if (getsockopt(s, SOL_SOCKET, SO_TYPE, (char *) &type, &typelen))
		return 1;

	return (type == SOCK_DGRAM) ? 1 : 0;
}

int
pgwin32_waitforsinglesocket(SOCKET s, int what, int timeout)
{
	static HANDLE waitevent = INVALID_HANDLE_VALUE;
	static SOCKET current_socket = -1;
	static int	isUDP = 0;
	HANDLE		events[2];
	int			r;

	if (waitevent == INVALID_HANDLE_VALUE)
	{
		waitevent = CreateEvent(NULL, TRUE, FALSE, NULL);

		if (waitevent == INVALID_HANDLE_VALUE)
			ereport(ERROR,
					(errmsg_internal("Failed to create socket waiting event: %i", (int) GetLastError())));
	}
	else if (!ResetEvent(waitevent))
		ereport(ERROR,
				(errmsg_internal("Failed to reset socket waiting event: %i", (int) GetLastError())));

	/*
	 * make sure we don't multiplex this kernel event object with a different
	 * socket from a previous call
	 */

	if (current_socket != s)
	{
		if (current_socket != -1)
			WSAEventSelect(current_socket, waitevent, 0);
		isUDP = isDataGram(s);
	}

	current_socket = s;

	if (WSAEventSelect(s, waitevent, what) == SOCKET_ERROR)
	{
		TranslateSocketError();
		return 0;
	}

	events[0] = pgwin32_signal_event;
	events[1] = waitevent;

	/*
	 * Just a workaround of unknown locking problem with writing in UDP socket
	 * under high load: Client's pgsql backend sleeps infinitely in
	 * WaitForMultipleObjectsEx, pgstat process sleeps in pgwin32_select().
	 * So, we will wait with small timeout(0.1 sec) and if sockect is still
	 * blocked, try WSASend (see comments in pgwin32_select) and wait again.
	 */
	if ((what & FD_WRITE) && isUDP)
	{
		for (;;)
		{
			r = WaitForMultipleObjectsEx(2, events, FALSE, 100, TRUE);

			if (r == WAIT_TIMEOUT)
			{
				char		c;
				WSABUF		buf;
				DWORD		sent;

				buf.buf = &c;
				buf.len = 0;

				r = WSASend(s, &buf, 1, &sent, 0, NULL, NULL);
				if (r == 0)		/* Completed - means things are fine! */
					return 1;
				else if (WSAGetLastError() != WSAEWOULDBLOCK)
				{
					TranslateSocketError();
					return 0;
				}
			}
			else
				break;
		}
	}
	else
		r = WaitForMultipleObjectsEx(2, events, FALSE, timeout, TRUE);

	if (r == WAIT_OBJECT_0 || r == WAIT_IO_COMPLETION)
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		return 0;
	}
	if (r == WAIT_OBJECT_0 + 1)
		return 1;
	if (r == WAIT_TIMEOUT)
		return 0;
	ereport(ERROR,
			(errmsg_internal("Bad return from WaitForMultipleObjects: %i (%i)", r, (int) GetLastError())));
	return 0;
}

/*
 * Create a socket, setting it to overlapped and non-blocking
 */
SOCKET
pgwin32_socket(int af, int type, int protocol)
{
	SOCKET		s;
	unsigned long on = 1;

	s = WSASocket(af, type, protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (s == INVALID_SOCKET)
	{
		TranslateSocketError();
		return INVALID_SOCKET;
	}

	if (ioctlsocket(s, FIONBIO, &on))
	{
		TranslateSocketError();
		return INVALID_SOCKET;
	}
	errno = 0;

	return s;
}


SOCKET
pgwin32_accept(SOCKET s, struct sockaddr * addr, int *addrlen)
{
	SOCKET		rs;

	/*
	 * Poll for signals, but don't return with EINTR, since we don't handle
	 * that in pqcomm.c
	 */
	pgwin32_poll_signals();

	rs = WSAAccept(s, addr, addrlen, NULL, 0);
	if (rs == INVALID_SOCKET)
	{
		TranslateSocketError();
		return INVALID_SOCKET;
	}
	return rs;
}


/* No signal delivery during connect. */
int
pgwin32_connect(SOCKET s, const struct sockaddr * addr, int addrlen)
{
	int			r;

	r = WSAConnect(s, addr, addrlen, NULL, NULL, NULL, NULL);
	if (r == 0)
		return 0;

	if (WSAGetLastError() != WSAEWOULDBLOCK)
	{
		TranslateSocketError();
		return -1;
	}

	while (pgwin32_waitforsinglesocket(s, FD_CONNECT, INFINITE) == 0)
	{
		/* Loop endlessly as long as we are just delivering signals */
	}

	return 0;
}

int
pgwin32_recv(SOCKET s, char *buf, int len, int f)
{
	WSABUF		wbuf;
	int			r;
	DWORD		b;
	DWORD		flags = f;
	int			n;

	if (pgwin32_poll_signals())
		return -1;

	wbuf.len = len;
	wbuf.buf = buf;

	r = WSARecv(s, &wbuf, 1, &b, &flags, NULL, NULL);
	if (r != SOCKET_ERROR && b > 0)
		/* Read succeeded right away */
		return b;

	if (r == SOCKET_ERROR &&
		WSAGetLastError() != WSAEWOULDBLOCK)
	{
		TranslateSocketError();
		return -1;
	}

	/* No error, zero bytes (win2000+) or error+WSAEWOULDBLOCK (<=nt4) */

	for (n = 0; n < 5; n++)
	{
		if (pgwin32_waitforsinglesocket(s, FD_READ | FD_CLOSE | FD_ACCEPT,
										INFINITE) == 0)
			return -1;			/* errno already set */

		r = WSARecv(s, &wbuf, 1, &b, &flags, NULL, NULL);
		if (r == SOCKET_ERROR)
		{
			if (WSAGetLastError() == WSAEWOULDBLOCK)
			{
				/*
				 * There seem to be cases on win2k (at least) where WSARecv
				 * can return WSAEWOULDBLOCK even when
				 * pgwin32_waitforsinglesocket claims the socket is readable.
				 * In this case, just sleep for a moment and try again. We try
				 * up to 5 times - if it fails more than that it's not likely
				 * to ever come back.
				 */
				pg_usleep(10000);
				continue;
			}
			TranslateSocketError();
			return -1;
		}
		return b;
	}
	ereport(NOTICE,
	  (errmsg_internal("Failed to read from ready socket (after retries)")));
	errno = EWOULDBLOCK;
	return -1;
}

int
pgwin32_send(SOCKET s, char *buf, int len, int flags)
{
	WSABUF		wbuf;
	int			r;
	DWORD		b;

	if (pgwin32_poll_signals())
		return -1;

	wbuf.len = len;
	wbuf.buf = buf;

	/*
	 * Readiness of socket to send data to UDP socket may be not true: socket
	 * can become busy again! So loop until send or error occurs.
	 */
	for (;;)
	{
		r = WSASend(s, &wbuf, 1, &b, flags, NULL, NULL);
		if (r != SOCKET_ERROR && b > 0)
			/* Write succeeded right away */
			return b;

		if (r == SOCKET_ERROR &&
			WSAGetLastError() != WSAEWOULDBLOCK)
		{
			TranslateSocketError();
			return -1;
		}

		/* No error, zero bytes (win2000+) or error+WSAEWOULDBLOCK (<=nt4) */

		if (pgwin32_waitforsinglesocket(s, FD_WRITE | FD_CLOSE, INFINITE) == 0)
			return -1;
	}

	return -1;
}


/*
 * Wait for activity on one or more sockets.
 * While waiting, allow signals to run
 *
 * NOTE! Currently does not implement exceptfds check,
 * since it is not used in postgresql!
 */
int
pgwin32_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timeval * timeout)
{
	WSAEVENT	events[FD_SETSIZE * 2]; /* worst case is readfds totally
										 * different from writefds, so
										 * 2*FD_SETSIZE sockets */
	SOCKET		sockets[FD_SETSIZE * 2];
	int			numevents = 0;
	int			i;
	int			r;
	DWORD		timeoutval = WSA_INFINITE;
	FD_SET		outreadfds;
	FD_SET		outwritefds;
	int			nummatches = 0;

	Assert(exceptfds == NULL);

	if (pgwin32_poll_signals())
		return -1;

	FD_ZERO(&outreadfds);
	FD_ZERO(&outwritefds);

	/*
	 * Write FDs are different in the way that it is only flagged by
	 * WSASelectEvent() if we have tried to write to them first. So try an
	 * empty write
	 */
	if (writefds)
	{
		for (i = 0; i < writefds->fd_count; i++)
		{
			char		c;
			WSABUF		buf;
			DWORD		sent;

			buf.buf = &c;
			buf.len = 0;

			r = WSASend(writefds->fd_array[i], &buf, 1, &sent, 0, NULL, NULL);
			if (r == 0)			/* Completed - means things are fine! */
				FD_SET(writefds->fd_array[i], &outwritefds);
			else
			{					/* Not completed */
				if (WSAGetLastError() != WSAEWOULDBLOCK)

					/*
					 * Not completed, and not just "would block", so an error
					 * occured
					 */
					FD_SET(writefds->fd_array[i], &outwritefds);
			}
		}
		if (outwritefds.fd_count > 0)
		{
			memcpy(writefds, &outwritefds, sizeof(fd_set));
			if (readfds)
				FD_ZERO(readfds);
			return outwritefds.fd_count;
		}
	}


	/* Now set up for an actual select */

	if (timeout != NULL)
	{
		/* timeoutval is in milliseconds */
		timeoutval = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
	}

	if (readfds != NULL)
	{
		for (i = 0; i < readfds->fd_count; i++)
		{
			events[numevents] = WSACreateEvent();
			sockets[numevents] = readfds->fd_array[i];
			numevents++;
		}
	}
	if (writefds != NULL)
	{
		for (i = 0; i < writefds->fd_count; i++)
		{
			if (!readfds ||
				!FD_ISSET(writefds->fd_array[i], readfds))
			{
				/* If the socket is not in the read list */
				events[numevents] = WSACreateEvent();
				sockets[numevents] = writefds->fd_array[i];
				numevents++;
			}
		}
	}

	for (i = 0; i < numevents; i++)
	{
		int			flags = 0;

		if (readfds && FD_ISSET(sockets[i], readfds))
			flags |= FD_READ | FD_ACCEPT | FD_CLOSE;

		if (writefds && FD_ISSET(sockets[i], writefds))
			flags |= FD_WRITE | FD_CLOSE;

		if (WSAEventSelect(sockets[i], events[i], flags) == SOCKET_ERROR)
		{
			TranslateSocketError();
			for (i = 0; i < numevents; i++)
				WSACloseEvent(events[i]);
			return -1;
		}
	}

	events[numevents] = pgwin32_signal_event;
	r = WaitForMultipleObjectsEx(numevents + 1, events, FALSE, timeoutval, TRUE);
	if (r != WAIT_TIMEOUT && r != WAIT_IO_COMPLETION && r != (WAIT_OBJECT_0 + numevents))
	{
		/*
		 * We scan all events, even those not signalled, in case more than one
		 * event has been tagged but Wait.. can only return one.
		 */
		WSANETWORKEVENTS resEvents;

		for (i = 0; i < numevents; i++)
		{
			ZeroMemory(&resEvents, sizeof(resEvents));
			if (WSAEnumNetworkEvents(sockets[i], events[i], &resEvents) == SOCKET_ERROR)
				ereport(FATAL,
						(errmsg_internal("failed to enumerate network events: %i", (int) GetLastError())));
			/* Read activity? */
			if (readfds && FD_ISSET(sockets[i], readfds))
			{
				if ((resEvents.lNetworkEvents & FD_READ) ||
					(resEvents.lNetworkEvents & FD_ACCEPT) ||
					(resEvents.lNetworkEvents & FD_CLOSE))
				{
					FD_SET(sockets[i], &outreadfds);
					nummatches++;
				}
			}
			/* Write activity? */
			if (writefds && FD_ISSET(sockets[i], writefds))
			{
				if ((resEvents.lNetworkEvents & FD_WRITE) ||
					(resEvents.lNetworkEvents & FD_CLOSE))
				{
					FD_SET(sockets[i], &outwritefds);
					nummatches++;
				}
			}
		}
	}

	/* Clean up all handles */
	for (i = 0; i < numevents; i++)
	{
		WSAEventSelect(sockets[i], events[i], 0);
		WSACloseEvent(events[i]);
	}

	if (r == WSA_WAIT_TIMEOUT)
	{
		if (readfds)
			FD_ZERO(readfds);
		if (writefds)
			FD_ZERO(writefds);
		return 0;
	}

	if (r == WAIT_OBJECT_0 + numevents)
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		if (readfds)
			FD_ZERO(readfds);
		if (writefds)
			FD_ZERO(writefds);
		return -1;
	}

	/* Overwrite socket sets with our resulting values */
	if (readfds)
		memcpy(readfds, &outreadfds, sizeof(fd_set));
	if (writefds)
		memcpy(writefds, &outwritefds, sizeof(fd_set));
	return nummatches;
}


/*
 * Return win32 error string, since strerror can't
 * handle winsock codes
 */
static char wserrbuf[256];
const char *
pgwin32_socket_strerror(int err)
{
	static HANDLE handleDLL = INVALID_HANDLE_VALUE;

	if (handleDLL == INVALID_HANDLE_VALUE)
	{
		handleDLL = LoadLibraryEx("netmsg.dll", NULL, DONT_RESOLVE_DLL_REFERENCES | LOAD_LIBRARY_AS_DATAFILE);
		if (handleDLL == NULL)
			ereport(FATAL,
					(errmsg_internal("Failed to load netmsg.dll: %i", (int) GetLastError())));
	}

	ZeroMemory(&wserrbuf, sizeof(wserrbuf));
	if (FormatMessage(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE,
					  handleDLL,
					  err,
					  MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
					  wserrbuf,
					  sizeof(wserrbuf) - 1,
					  NULL) == 0)
	{
		/* Failed to get id */
		sprintf(wserrbuf, "Unknown winsock error %i", err);
	}
	return wserrbuf;
}
