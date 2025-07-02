/*-------------------------------------------------------------------------
 *
 * fe-cancel.c
 *	  functions related to setting up a connection to the backend
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/libpq/fe-cancel.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <unistd.h>

#include "libpq-fe.h"
#include "libpq-int.h"
#include "port/pg_bswap.h"


/*
 * pg_cancel_conn (backing struct for PGcancelConn) is a wrapper around a
 * PGconn to send cancellations using PQcancelBlocking and PQcancelStart.
 * This isn't just a typedef because we want the compiler to complain when a
 * PGconn is passed to a function that expects a PGcancelConn, and vice versa.
 */
struct pg_cancel_conn
{
	PGconn		conn;
};

/*
 * pg_cancel (backing struct for PGcancel) stores all data necessary to send a
 * cancel request.
 */
struct pg_cancel
{
	SockAddr	raddr;			/* Remote address */
	int			be_pid;			/* PID of to-be-canceled backend */
	int			be_key;			/* cancel key of to-be-canceled backend */
	int			pgtcp_user_timeout; /* tcp user timeout */
	int			keepalives;		/* use TCP keepalives? */
	int			keepalives_idle;	/* time between TCP keepalives */
	int			keepalives_interval;	/* time between TCP keepalive
										 * retransmits */
	int			keepalives_count;	/* maximum number of TCP keepalive
									 * retransmits */
};


/*
 *		PQcancelCreate
 *
 * Create and return a PGcancelConn, which can be used to securely cancel a
 * query on the given connection.
 *
 * This requires either following the non-blocking flow through
 * PQcancelStart() and PQcancelPoll(), or the blocking PQcancelBlocking().
 */
PGcancelConn *
PQcancelCreate(PGconn *conn)
{
	PGconn	   *cancelConn = pqMakeEmptyPGconn();
	pg_conn_host originalHost;

	if (cancelConn == NULL)
		return NULL;

	/* Check we have an open connection */
	if (!conn)
	{
		libpq_append_conn_error(cancelConn, "connection pointer is NULL");
		return (PGcancelConn *) cancelConn;
	}

	if (conn->sock == PGINVALID_SOCKET)
	{
		libpq_append_conn_error(cancelConn, "connection not open");
		return (PGcancelConn *) cancelConn;
	}

	/*
	 * Indicate that this connection is used to send a cancellation
	 */
	cancelConn->cancelRequest = true;

	if (!pqCopyPGconn(conn, cancelConn))
		return (PGcancelConn *) cancelConn;

	/*
	 * Compute derived options
	 */
	if (!pqConnectOptions2(cancelConn))
		return (PGcancelConn *) cancelConn;

	/*
	 * Copy cancellation token data from the original connection
	 */
	cancelConn->be_pid = conn->be_pid;
	cancelConn->be_key = conn->be_key;

	/*
	 * Cancel requests should not iterate over all possible hosts. The request
	 * needs to be sent to the exact host and address that the original
	 * connection used. So we manually create the host and address arrays with
	 * a single element after freeing the host array that we generated from
	 * the connection options.
	 */
	pqReleaseConnHosts(cancelConn);
	cancelConn->nconnhost = 1;
	cancelConn->naddr = 1;

	cancelConn->connhost = calloc(cancelConn->nconnhost, sizeof(pg_conn_host));
	if (!cancelConn->connhost)
		goto oom_error;

	originalHost = conn->connhost[conn->whichhost];
	cancelConn->connhost[0].type = originalHost.type;
	if (originalHost.host)
	{
		cancelConn->connhost[0].host = strdup(originalHost.host);
		if (!cancelConn->connhost[0].host)
			goto oom_error;
	}
	if (originalHost.hostaddr)
	{
		cancelConn->connhost[0].hostaddr = strdup(originalHost.hostaddr);
		if (!cancelConn->connhost[0].hostaddr)
			goto oom_error;
	}
	if (originalHost.port)
	{
		cancelConn->connhost[0].port = strdup(originalHost.port);
		if (!cancelConn->connhost[0].port)
			goto oom_error;
	}
	if (originalHost.password)
	{
		cancelConn->connhost[0].password = strdup(originalHost.password);
		if (!cancelConn->connhost[0].password)
			goto oom_error;
	}

	cancelConn->addr = calloc(cancelConn->naddr, sizeof(AddrInfo));
	if (!cancelConn->addr)
		goto oom_error;

	cancelConn->addr[0].addr = conn->raddr;
	cancelConn->addr[0].family = conn->raddr.addr.ss_family;

	cancelConn->status = CONNECTION_ALLOCATED;
	return (PGcancelConn *) cancelConn;

oom_error:
	cancelConn->status = CONNECTION_BAD;
	libpq_append_conn_error(cancelConn, "out of memory");
	return (PGcancelConn *) cancelConn;
}


/*
 *		PQcancelBlocking
 *
 * Send a cancellation request in a blocking fashion.
 * Returns 1 if successful 0 if not.
 */
int
PQcancelBlocking(PGcancelConn *cancelConn)
{
	if (!PQcancelStart(cancelConn))
		return 0;
	return pqConnectDBComplete(&cancelConn->conn);
}

/*
 *		PQcancelStart
 *
 * Starts sending a cancellation request in a non-blocking fashion. Returns
 * 1 if successful 0 if not.
 */
int
PQcancelStart(PGcancelConn *cancelConn)
{
	if (!cancelConn || cancelConn->conn.status == CONNECTION_BAD)
		return 0;

	if (cancelConn->conn.status != CONNECTION_ALLOCATED)
	{
		libpq_append_conn_error(&cancelConn->conn,
								"cancel request is already being sent on this connection");
		cancelConn->conn.status = CONNECTION_BAD;
		return 0;
	}

	return pqConnectDBStart(&cancelConn->conn);
}

/*
 *		PQcancelPoll
 *
 * Poll a cancel connection. For usage details see PQconnectPoll.
 */
PostgresPollingStatusType
PQcancelPoll(PGcancelConn *cancelConn)
{
	PGconn	   *conn = &cancelConn->conn;
	int			n;

	/*
	 * We leave most of the connection establishment to PQconnectPoll, since
	 * it's very similar to normal connection establishment. But once we get
	 * to the CONNECTION_AWAITING_RESPONSE we need to start doing our own
	 * thing.
	 */
	if (conn->status != CONNECTION_AWAITING_RESPONSE)
	{
		return PQconnectPoll(conn);
	}

	/*
	 * At this point we are waiting on the server to close the connection,
	 * which is its way of communicating that the cancel has been handled.
	 */

	n = pqReadData(conn);

	if (n == 0)
		return PGRES_POLLING_READING;

#ifndef WIN32

	/*
	 * If we receive an error report it, but only if errno is non-zero.
	 * Otherwise we assume it's an EOF, which is what we expect from the
	 * server.
	 *
	 * We skip this for Windows, because Windows is a bit special in its EOF
	 * behaviour for TCP. Sometimes it will error with an ECONNRESET when
	 * there is a clean connection closure. See these threads for details:
	 * https://www.postgresql.org/message-id/flat/90b34057-4176-7bb0-0dbb-9822a5f6425b%40greiz-reinsdorf.de
	 *
	 * https://www.postgresql.org/message-id/flat/CA%2BhUKG%2BOeoETZQ%3DQw5Ub5h3tmwQhBmDA%3DnuNO3KG%3DzWfUypFAw%40mail.gmail.com
	 *
	 * PQcancel ignores such errors and reports success for the cancellation
	 * anyway, so even if this is not always correct we do the same here.
	 */
	if (n < 0 && errno != 0)
	{
		conn->status = CONNECTION_BAD;
		return PGRES_POLLING_FAILED;
	}
#endif

	/*
	 * We don't expect any data, only connection closure. So if we strangely
	 * do receive some data we consider that an error.
	 */
	if (n > 0)
	{
		libpq_append_conn_error(conn, "unexpected response from server");
		conn->status = CONNECTION_BAD;
		return PGRES_POLLING_FAILED;
	}

	/*
	 * Getting here means that we received an EOF, which is what we were
	 * expecting -- the cancel request has completed.
	 */
	cancelConn->conn.status = CONNECTION_OK;
	resetPQExpBuffer(&conn->errorMessage);
	return PGRES_POLLING_OK;
}

/*
 *		PQcancelStatus
 *
 * Get the status of a cancel connection.
 */
ConnStatusType
PQcancelStatus(const PGcancelConn *cancelConn)
{
	return PQstatus(&cancelConn->conn);
}

/*
 *		PQcancelSocket
 *
 * Get the socket of the cancel connection.
 */
int
PQcancelSocket(const PGcancelConn *cancelConn)
{
	return PQsocket(&cancelConn->conn);
}

/*
 *		PQcancelErrorMessage
 *
 * Returns the error message most recently generated by an operation on the
 * cancel connection.
 */
char *
PQcancelErrorMessage(const PGcancelConn *cancelConn)
{
	return PQerrorMessage(&cancelConn->conn);
}

/*
 *		PQcancelReset
 *
 * Resets the cancel connection, so it can be reused to send a new cancel
 * request.
 */
void
PQcancelReset(PGcancelConn *cancelConn)
{
	pqClosePGconn(&cancelConn->conn);
	cancelConn->conn.status = CONNECTION_ALLOCATED;
	cancelConn->conn.whichhost = 0;
	cancelConn->conn.whichaddr = 0;
	cancelConn->conn.try_next_host = false;
	cancelConn->conn.try_next_addr = false;
}

/*
 *		PQcancelFinish
 *
 * Closes and frees the cancel connection.
 */
void
PQcancelFinish(PGcancelConn *cancelConn)
{
	PQfinish(&cancelConn->conn);
}

/*
 * PQgetCancel: get a PGcancel structure corresponding to a connection.
 *
 * A copy is needed to be able to cancel a running query from a different
 * thread. If the same structure is used all structure members would have
 * to be individually locked (if the entire structure was locked, it would
 * be impossible to cancel a synchronous query because the structure would
 * have to stay locked for the duration of the query).
 */
PGcancel *
PQgetCancel(PGconn *conn)
{
	PGcancel   *cancel;

	if (!conn)
		return NULL;

	if (conn->sock == PGINVALID_SOCKET)
		return NULL;

	cancel = malloc(sizeof(PGcancel));
	if (cancel == NULL)
		return NULL;

	memcpy(&cancel->raddr, &conn->raddr, sizeof(SockAddr));
	cancel->be_pid = conn->be_pid;
	cancel->be_key = conn->be_key;
	/* We use -1 to indicate an unset connection option */
	cancel->pgtcp_user_timeout = -1;
	cancel->keepalives = -1;
	cancel->keepalives_idle = -1;
	cancel->keepalives_interval = -1;
	cancel->keepalives_count = -1;
	if (conn->pgtcp_user_timeout != NULL)
	{
		if (!pqParseIntParam(conn->pgtcp_user_timeout,
							 &cancel->pgtcp_user_timeout,
							 conn, "tcp_user_timeout"))
			goto fail;
	}
	if (conn->keepalives != NULL)
	{
		if (!pqParseIntParam(conn->keepalives,
							 &cancel->keepalives,
							 conn, "keepalives"))
			goto fail;
	}
	if (conn->keepalives_idle != NULL)
	{
		if (!pqParseIntParam(conn->keepalives_idle,
							 &cancel->keepalives_idle,
							 conn, "keepalives_idle"))
			goto fail;
	}
	if (conn->keepalives_interval != NULL)
	{
		if (!pqParseIntParam(conn->keepalives_interval,
							 &cancel->keepalives_interval,
							 conn, "keepalives_interval"))
			goto fail;
	}
	if (conn->keepalives_count != NULL)
	{
		if (!pqParseIntParam(conn->keepalives_count,
							 &cancel->keepalives_count,
							 conn, "keepalives_count"))
			goto fail;
	}

	return cancel;

fail:
	free(cancel);
	return NULL;
}

/* PQfreeCancel: free a cancel structure */
void
PQfreeCancel(PGcancel *cancel)
{
	free(cancel);
}


/*
 * Sets an integer socket option on a TCP socket, if the provided value is
 * not negative.  Returns false if setsockopt fails for some reason.
 *
 * CAUTION: This needs to be signal safe, since it's used by PQcancel.
 */
#if defined(TCP_USER_TIMEOUT) || !defined(WIN32)
static bool
optional_setsockopt(int fd, int protoid, int optid, int value)
{
	if (value < 0)
		return true;
	if (setsockopt(fd, protoid, optid, (char *) &value, sizeof(value)) < 0)
		return false;
	return true;
}
#endif


/*
 * PQcancel: old, non-encrypted, but signal-safe way of requesting query cancel
 *
 * The return value is true if the cancel request was successfully
 * dispatched, false if not (in which case an error message is available).
 * Note: successful dispatch is no guarantee that there will be any effect at
 * the backend.  The application must read the operation result as usual.
 *
 * On failure, an error message is stored in *errbuf, which must be of size
 * errbufsize (recommended size is 256 bytes).  *errbuf is not changed on
 * success return.
 *
 * CAUTION: we want this routine to be safely callable from a signal handler
 * (for example, an application might want to call it in a SIGINT handler).
 * This means we cannot use any C library routine that might be non-reentrant.
 * malloc/free are often non-reentrant, and anything that might call them is
 * just as dangerous.  We avoid sprintf here for that reason.  Building up
 * error messages with strcpy/strcat is tedious but should be quite safe.
 * We also save/restore errno in case the signal handler support doesn't.
 */
int
PQcancel(PGcancel *cancel, char *errbuf, int errbufsize)
{
	int			save_errno = SOCK_ERRNO;
	pgsocket	tmpsock = PGINVALID_SOCKET;
	int			maxlen;
	struct
	{
		uint32		packetlen;
		CancelRequestPacket cp;
	}			crp;

	if (!cancel)
	{
		strlcpy(errbuf, "PQcancel() -- no cancel object supplied", errbufsize);
		/* strlcpy probably doesn't change errno, but be paranoid */
		SOCK_ERRNO_SET(save_errno);
		return false;
	}

	/*
	 * We need to open a temporary connection to the postmaster. Do this with
	 * only kernel calls.
	 */
	if ((tmpsock = socket(cancel->raddr.addr.ss_family, SOCK_STREAM, 0)) == PGINVALID_SOCKET)
	{
		strlcpy(errbuf, "PQcancel() -- socket() failed: ", errbufsize);
		goto cancel_errReturn;
	}

	/*
	 * Since this connection will only be used to send a single packet of
	 * data, we don't need NODELAY.  We also don't set the socket to
	 * nonblocking mode, because the API definition of PQcancel requires the
	 * cancel to be sent in a blocking way.
	 *
	 * We do set socket options related to keepalives and other TCP timeouts.
	 * This ensures that this function does not block indefinitely when
	 * reasonable keepalive and timeout settings have been provided.
	 */
	if (cancel->raddr.addr.ss_family != AF_UNIX &&
		cancel->keepalives != 0)
	{
#ifndef WIN32
		if (!optional_setsockopt(tmpsock, SOL_SOCKET, SO_KEEPALIVE, 1))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(SO_KEEPALIVE) failed: ", errbufsize);
			goto cancel_errReturn;
		}

#ifdef PG_TCP_KEEPALIVE_IDLE
		if (!optional_setsockopt(tmpsock, IPPROTO_TCP, PG_TCP_KEEPALIVE_IDLE,
								 cancel->keepalives_idle))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(" PG_TCP_KEEPALIVE_IDLE_STR ") failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif

#ifdef TCP_KEEPINTVL
		if (!optional_setsockopt(tmpsock, IPPROTO_TCP, TCP_KEEPINTVL,
								 cancel->keepalives_interval))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(TCP_KEEPINTVL) failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif

#ifdef TCP_KEEPCNT
		if (!optional_setsockopt(tmpsock, IPPROTO_TCP, TCP_KEEPCNT,
								 cancel->keepalives_count))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(TCP_KEEPCNT) failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif

#else							/* WIN32 */

#ifdef SIO_KEEPALIVE_VALS
		if (!pqSetKeepalivesWin32(tmpsock,
								  cancel->keepalives_idle,
								  cancel->keepalives_interval))
		{
			strlcpy(errbuf, "PQcancel() -- WSAIoctl(SIO_KEEPALIVE_VALS) failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif							/* SIO_KEEPALIVE_VALS */
#endif							/* WIN32 */

		/* TCP_USER_TIMEOUT works the same way on Unix and Windows */
#ifdef TCP_USER_TIMEOUT
		if (!optional_setsockopt(tmpsock, IPPROTO_TCP, TCP_USER_TIMEOUT,
								 cancel->pgtcp_user_timeout))
		{
			strlcpy(errbuf, "PQcancel() -- setsockopt(TCP_USER_TIMEOUT) failed: ", errbufsize);
			goto cancel_errReturn;
		}
#endif
	}

retry3:
	if (connect(tmpsock, (struct sockaddr *) &cancel->raddr.addr,
				cancel->raddr.salen) < 0)
	{
		if (SOCK_ERRNO == EINTR)
			/* Interrupted system call - we'll just try again */
			goto retry3;
		strlcpy(errbuf, "PQcancel() -- connect() failed: ", errbufsize);
		goto cancel_errReturn;
	}

	/* Create and send the cancel request packet. */

	crp.packetlen = pg_hton32((uint32) sizeof(crp));
	crp.cp.cancelRequestCode = (MsgType) pg_hton32(CANCEL_REQUEST_CODE);
	crp.cp.backendPID = pg_hton32(cancel->be_pid);
	crp.cp.cancelAuthCode = pg_hton32(cancel->be_key);

retry4:
	if (send(tmpsock, (char *) &crp, sizeof(crp), 0) != (int) sizeof(crp))
	{
		if (SOCK_ERRNO == EINTR)
			/* Interrupted system call - we'll just try again */
			goto retry4;
		strlcpy(errbuf, "PQcancel() -- send() failed: ", errbufsize);
		goto cancel_errReturn;
	}

	/*
	 * Wait for the postmaster to close the connection, which indicates that
	 * it's processed the request.  Without this delay, we might issue another
	 * command only to find that our cancel zaps that command instead of the
	 * one we thought we were canceling.  Note we don't actually expect this
	 * read to obtain any data, we are just waiting for EOF to be signaled.
	 */
retry5:
	if (recv(tmpsock, (char *) &crp, 1, 0) < 0)
	{
		if (SOCK_ERRNO == EINTR)
			/* Interrupted system call - we'll just try again */
			goto retry5;
		/* we ignore other error conditions */
	}

	/* All done */
	closesocket(tmpsock);
	SOCK_ERRNO_SET(save_errno);
	return true;

cancel_errReturn:

	/*
	 * Make sure we don't overflow the error buffer. Leave space for the \n at
	 * the end, and for the terminating zero.
	 */
	maxlen = errbufsize - strlen(errbuf) - 2;
	if (maxlen >= 0)
	{
		/*
		 * We can't invoke strerror here, since it's not signal-safe.  Settle
		 * for printing the decimal value of errno.  Even that has to be done
		 * the hard way.
		 */
		int			val = SOCK_ERRNO;
		char		buf[32];
		char	   *bufp;

		bufp = buf + sizeof(buf) - 1;
		*bufp = '\0';
		do
		{
			*(--bufp) = (val % 10) + '0';
			val /= 10;
		} while (val > 0);
		bufp -= 6;
		memcpy(bufp, "error ", 6);
		strncat(errbuf, bufp, maxlen);
		strcat(errbuf, "\n");
	}
	if (tmpsock != PGINVALID_SOCKET)
		closesocket(tmpsock);
	SOCK_ERRNO_SET(save_errno);
	return false;
}

/*
 * PQrequestCancel: old, not thread-safe function for requesting query cancel
 *
 * Returns true if able to send the cancel request, false if not.
 *
 * On failure, the error message is saved in conn->errorMessage; this means
 * that this can't be used when there might be other active operations on
 * the connection object.
 *
 * NOTE: error messages will be cut off at the current size of the
 * error message buffer, since we dare not try to expand conn->errorMessage!
 */
int
PQrequestCancel(PGconn *conn)
{
	int			r;
	PGcancel   *cancel;

	/* Check we have an open connection */
	if (!conn)
		return false;

	if (conn->sock == PGINVALID_SOCKET)
	{
		strlcpy(conn->errorMessage.data,
				"PQrequestCancel() -- connection is not open\n",
				conn->errorMessage.maxlen);
		conn->errorMessage.len = strlen(conn->errorMessage.data);
		conn->errorReported = 0;

		return false;
	}

	cancel = PQgetCancel(conn);
	if (cancel)
	{
		r = PQcancel(cancel, conn->errorMessage.data,
					 conn->errorMessage.maxlen);
		PQfreeCancel(cancel);
	}
	else
	{
		strlcpy(conn->errorMessage.data, "out of memory",
				conn->errorMessage.maxlen);
		r = false;
	}

	if (!r)
	{
		conn->errorMessage.len = strlen(conn->errorMessage.data);
		conn->errorReported = 0;
	}

	return r;
}
