/*-------------------------------------------------------------------------
 *
 * socket.c
 *	  Microsoft Windows Win32 Socket Functions
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/port/win32/socket.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

/*
 * Indicate if pgwin32_recv() and pgwin32_send() should operate
 * in non-blocking mode.
 *
 * Since the socket emulation layer always sets the actual socket to
 * non-blocking mode in order to be able to deliver signals, we must
 * specify this in a separate flag if we actually need non-blocking
 * operation.
 *
 * This flag changes the behaviour *globally* for all socket operations,
 * so it should only be set for very short periods of time.
 */
int			pgwin32_noblock = 0;

/* Undef the macros defined in win32.h, so we can access system functions */
#undef socket
#undef bind
#undef listen
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
 *
 * Note: where there is a direct correspondence between a WSAxxx error code
 * and a Berkeley error symbol, this mapping is actually a no-op, because
 * in win32.h we redefine the network-related Berkeley error symbols to have
 * the values of their WSAxxx counterparts.  The point of the switch is
 * mostly to translate near-miss error codes into something that's sensible
 * in the Berkeley universe.
 */
static void
TranslateSocketError(void)
{
	switch (WSAGetLastError())
	{
		case WSAEINVAL:
		case WSANOTINITIALISED:
		case WSAEINVALIDPROVIDER:
		case WSAEINVALIDPROCTABLE:
		case WSAEDESTADDRREQ:
			errno = EINVAL;
			break;
		case WSAEINPROGRESS:
			errno = EINPROGRESS;
			break;
		case WSAEFAULT:
			errno = EFAULT;
			break;
		case WSAEISCONN:
			errno = EISCONN;
			break;
		case WSAEMSGSIZE:
			errno = EMSGSIZE;
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
		case WSAESOCKTNOSUPPORT:
			errno = EPROTONOSUPPORT;
			break;
		case WSAECONNABORTED:
			errno = ECONNABORTED;
			break;
		case WSAECONNREFUSED:
			errno = ECONNREFUSED;
			break;
		case WSAECONNRESET:
			errno = ECONNRESET;
			break;
		case WSAEINTR:
			errno = EINTR;
			break;
		case WSAENOTSOCK:
			errno = ENOTSOCK;
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
		case WSAEADDRINUSE:
			errno = EADDRINUSE;
			break;
		case WSAEADDRNOTAVAIL:
			errno = EADDRNOTAVAIL;
			break;
		case WSAEHOSTUNREACH:
		case WSAEHOSTDOWN:
		case WSAHOST_NOT_FOUND:
		case WSAENETDOWN:
		case WSAENETUNREACH:
		case WSAENETRESET:
			errno = EHOSTUNREACH;
			break;
		case WSAENOTCONN:
		case WSAESHUTDOWN:
		case WSAEDISCON:
			errno = ENOTCONN;
			break;
		default:
			ereport(NOTICE,
					(errmsg_internal("unrecognized win32 socket error code: %d", WSAGetLastError())));
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
	static SOCKET current_socket = INVALID_SOCKET;
	static int	isUDP = 0;
	HANDLE		events[2];
	int			r;

	/* Create an event object just once and use it on all future calls */
	if (waitevent == INVALID_HANDLE_VALUE)
	{
		waitevent = CreateEvent(NULL, TRUE, FALSE, NULL);

		if (waitevent == INVALID_HANDLE_VALUE)
			ereport(ERROR,
					(errmsg_internal("could not create socket waiting event: error code %lu", GetLastError())));
	}
	else if (!ResetEvent(waitevent))
		ereport(ERROR,
				(errmsg_internal("could not reset socket waiting event: error code %lu", GetLastError())));

	/*
	 * Track whether socket is UDP or not.  (NB: most likely, this is both
	 * useless and wrong; there is no reason to think that the behavior of
	 * WSAEventSelect is different for TCP and UDP.)
	 */
	if (current_socket != s)
		isUDP = isDataGram(s);
	current_socket = s;

	/*
	 * Attach event to socket.  NOTE: we must detach it again before
	 * returning, since other bits of code may try to attach other events to
	 * the socket.
	 */
	if (WSAEventSelect(s, waitevent, what) != 0)
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
	 * So, we will wait with small timeout(0.1 sec) and if socket is still
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
				{
					WSAEventSelect(s, NULL, 0);
					return 1;
				}
				else if (WSAGetLastError() != WSAEWOULDBLOCK)
				{
					TranslateSocketError();
					WSAEventSelect(s, NULL, 0);
					return 0;
				}
			}
			else
				break;
		}
	}
	else
		r = WaitForMultipleObjectsEx(2, events, FALSE, timeout, TRUE);

	WSAEventSelect(s, NULL, 0);

	if (r == WAIT_OBJECT_0 || r == WAIT_IO_COMPLETION)
	{
		pgwin32_dispatch_queued_signals();
		errno = EINTR;
		return 0;
	}
	if (r == WAIT_OBJECT_0 + 1)
		return 1;
	if (r == WAIT_TIMEOUT)
	{
		errno = EWOULDBLOCK;
		return 0;
	}
	ereport(ERROR,
			(errmsg_internal("unrecognized return value from WaitForMultipleObjects: %d (error code %lu)", r, GetLastError())));
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

int
pgwin32_bind(SOCKET s, struct sockaddr *addr, int addrlen)
{
	int			res;

	res = bind(s, addr, addrlen);
	if (res < 0)
		TranslateSocketError();
	return res;
}

int
pgwin32_listen(SOCKET s, int backlog)
{
	int			res;

	res = listen(s, backlog);
	if (res < 0)
		TranslateSocketError();
	return res;
}

SOCKET
pgwin32_accept(SOCKET s, struct sockaddr *addr, int *addrlen)
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
pgwin32_connect(SOCKET s, const struct sockaddr *addr, int addrlen)
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
	if (r != SOCKET_ERROR)
		return b;				/* success */

	if (WSAGetLastError() != WSAEWOULDBLOCK)
	{
		TranslateSocketError();
		return -1;
	}

	if (pgwin32_noblock)
	{
		/*
		 * No data received, and we are in "emulated non-blocking mode", so
		 * return indicating that we'd block if we were to continue.
		 */
		errno = EWOULDBLOCK;
		return -1;
	}

	/* We're in blocking mode, so wait for data */

	for (n = 0; n < 5; n++)
	{
		if (pgwin32_waitforsinglesocket(s, FD_READ | FD_CLOSE | FD_ACCEPT,
										INFINITE) == 0)
			return -1;			/* errno already set */

		r = WSARecv(s, &wbuf, 1, &b, &flags, NULL, NULL);
		if (r != SOCKET_ERROR)
			return b;			/* success */
		if (WSAGetLastError() != WSAEWOULDBLOCK)
		{
			TranslateSocketError();
			return -1;
		}

		/*
		 * There seem to be cases on win2k (at least) where WSARecv can return
		 * WSAEWOULDBLOCK even when pgwin32_waitforsinglesocket claims the
		 * socket is readable.  In this case, just sleep for a moment and try
		 * again.  We try up to 5 times - if it fails more than that it's not
		 * likely to ever come back.
		 */
		pg_usleep(10000);
	}
	ereport(NOTICE,
			(errmsg_internal("could not read from ready socket (after retries)")));
	errno = EWOULDBLOCK;
	return -1;
}

/*
 * The second argument to send() is defined by SUS to be a "const void *"
 * and so we use the same signature here to keep compilers happy when
 * handling callers.
 *
 * But the buf member of a WSABUF struct is defined as "char *", so we cast
 * the second argument to that here when assigning it, also to keep compilers
 * happy.
 */

int
pgwin32_send(SOCKET s, const void *buf, int len, int flags)
{
	WSABUF		wbuf;
	int			r;
	DWORD		b;

	if (pgwin32_poll_signals())
		return -1;

	wbuf.len = len;
	wbuf.buf = (char *) buf;

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

		if (pgwin32_noblock)
		{
			/*
			 * No data sent, and we are in "emulated non-blocking mode", so
			 * return indicating that we'd block if we were to continue.
			 */
			errno = EWOULDBLOCK;
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
pgwin32_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, const struct timeval *timeout)
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
	 * Windows does not guarantee to log an FD_WRITE network event indicating
	 * that more data can be sent unless the previous send() failed with
	 * WSAEWOULDBLOCK.  While our caller might well have made such a call, we
	 * cannot assume that here.  Therefore, if waiting for write-ready, force
	 * the issue by doing a dummy send().  If the dummy send() succeeds,
	 * assume that the socket is in fact write-ready, and return immediately.
	 * Also, if it fails with something other than WSAEWOULDBLOCK, return a
	 * write-ready indication to let our caller deal with the error condition.
	 */
	if (writefds != NULL)
	{
		for (i = 0; i < writefds->fd_count; i++)
		{
			char		c;
			WSABUF		buf;
			DWORD		sent;

			buf.buf = &c;
			buf.len = 0;

			r = WSASend(writefds->fd_array[i], &buf, 1, &sent, 0, NULL, NULL);
			if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
				FD_SET(writefds->fd_array[i], &outwritefds);
		}

		/* If we found any write-ready sockets, just return them immediately */
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

		if (WSAEventSelect(sockets[i], events[i], flags) != 0)
		{
			TranslateSocketError();
			/* release already-assigned event objects */
			while (--i >= 0)
				WSAEventSelect(sockets[i], NULL, 0);
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
		 * We scan all events, even those not signaled, in case more than one
		 * event has been tagged but Wait.. can only return one.
		 */
		WSANETWORKEVENTS resEvents;

		for (i = 0; i < numevents; i++)
		{
			ZeroMemory(&resEvents, sizeof(resEvents));
			if (WSAEnumNetworkEvents(sockets[i], events[i], &resEvents) != 0)
				elog(ERROR, "failed to enumerate network events: error code %u",
					 WSAGetLastError());
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

	/* Clean up all the event objects */
	for (i = 0; i < numevents; i++)
	{
		WSAEventSelect(sockets[i], NULL, 0);
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

	/* Signal-like events. */
	if (r == WAIT_OBJECT_0 + numevents || r == WAIT_IO_COMPLETION)
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
