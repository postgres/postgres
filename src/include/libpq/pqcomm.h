/*-------------------------------------------------------------------------
 *
 * pqcomm.h
 *		Definitions common to frontends and backends.
 *
 * NOTE: for historical reasons, this does not correspond to pqcomm.c.
 * pqcomm.c's routines are declared in libpq.h.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/libpq/pqcomm.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQCOMM_H
#define PQCOMM_H

#include <sys/socket.h>
#include <sys/un.h>
#include <netdb.h>
#include <netinet/in.h>

/*
 * The definitions for the request/response codes are kept in a separate file
 * for ease of use in third party programs.
 */
#include "libpq/protocol.h"

typedef struct
{
	struct sockaddr_storage addr;
	socklen_t	salen;
} SockAddr;

typedef struct
{
	int			family;
	SockAddr	addr;
} AddrInfo;

/* Configure the UNIX socket location for the well known port. */

#define UNIXSOCK_PATH(path, port, sockdir) \
	   (AssertMacro(sockdir), \
		AssertMacro(*(sockdir) != '\0'), \
		snprintf(path, sizeof(path), "%s/.s.PGSQL.%d", \
				 (sockdir), (port)))

/*
 * The maximum workable length of a socket path is what will fit into
 * struct sockaddr_un.  This is usually only 100 or so bytes :-(.
 *
 * For consistency, always pass a MAXPGPATH-sized buffer to UNIXSOCK_PATH(),
 * then complain if the resulting string is >= UNIXSOCK_PATH_BUFLEN bytes.
 * (Because the standard API for getaddrinfo doesn't allow it to complain in
 * a useful way when the socket pathname is too long, we have to test for
 * this explicitly, instead of just letting the subroutine return an error.)
 */
#define UNIXSOCK_PATH_BUFLEN sizeof(((struct sockaddr_un *) NULL)->sun_path)

/*
 * A host that looks either like an absolute path or starts with @ is
 * interpreted as a Unix-domain socket address.
 */
static inline bool
is_unixsock_path(const char *path)
{
	return is_absolute_path(path) || path[0] == '@';
}

/*
 * These manipulate the frontend/backend protocol version number.
 *
 * The major number should be incremented for incompatible changes.  The minor
 * number should be incremented for compatible changes (eg. additional
 * functionality).
 *
 * If a backend supports version m.n of the protocol it must actually support
 * versions m.[0..n].  Backend support for version m-1 can be dropped after a
 * `reasonable' length of time.
 *
 * A frontend isn't required to support anything other than the current
 * version.
 */

#define PG_PROTOCOL_MAJOR(v)	((v) >> 16)
#define PG_PROTOCOL_MINOR(v)	((v) & 0x0000ffff)
#define PG_PROTOCOL(m,n)	(((m) << 16) | (n))

/*
 * The earliest and latest frontend/backend protocol version supported.
 * (Only protocol version 3 is currently supported)
 */

#define PG_PROTOCOL_EARLIEST	PG_PROTOCOL(3,0)
#define PG_PROTOCOL_LATEST		PG_PROTOCOL(3,0)

typedef uint32 ProtocolVersion; /* FE/BE protocol version number */

typedef ProtocolVersion MsgType;


/*
 * Packet lengths are 4 bytes in network byte order.
 *
 * The initial length is omitted from the packet layouts appearing below.
 */

typedef uint32 PacketLen;

/*
 * In protocol 3.0 and later, the startup packet length is not fixed, but
 * we set an arbitrary limit on it anyway.  This is just to prevent simple
 * denial-of-service attacks via sending enough data to run the server
 * out of memory.
 */
#define MAX_STARTUP_PACKET_LENGTH 10000


typedef uint32 AuthRequest;


/*
 * A client can also send a cancel-current-operation request to the postmaster.
 * This is uglier than sending it directly to the client's backend, but it
 * avoids depending on out-of-band communication facilities.
 *
 * The cancel request code must not match any protocol version number
 * we're ever likely to use.  This random choice should do.
 */
#define CANCEL_REQUEST_CODE PG_PROTOCOL(1234,5678)

typedef struct CancelRequestPacket
{
	/* Note that each field is stored in network byte order! */
	MsgType		cancelRequestCode;	/* code to identify a cancel request */
	uint32		backendPID;		/* PID of client's backend */
	uint32		cancelAuthCode; /* secret key to authorize cancel */
} CancelRequestPacket;

/* Application-Layer Protocol Negotiation is required for direct connections
 * to avoid protocol confusion attacks (e.g https://alpaca-attack.com/).
 *
 * ALPN is specified in RFC 7301
 *
 * This string should be registered at:
 * https://www.iana.org/assignments/tls-extensiontype-values/tls-extensiontype-values.xhtml#alpn-protocol-ids
 *
 * OpenSSL uses this wire-format for the list of alpn protocols even in the
 * API. Both server and client take the same format parameter but the client
 * actually sends it to the server as-is and the server it specifies the
 * preference order to use to choose the one selected to send back.
 *
 * c.f. https://www.openssl.org/docs/manmaster/man3/SSL_CTX_set_alpn_select_cb.html
 *
 * The #define can be used to initialize a char[] vector to use directly in the API
 */
#define PG_ALPN_PROTOCOL "postgresql"
#define PG_ALPN_PROTOCOL_VECTOR { 10, 'p','o','s','t','g','r','e','s','q','l' }

/*
 * A client can also start by sending a SSL or GSSAPI negotiation request to
 * get a secure channel.
 */
#define NEGOTIATE_SSL_CODE PG_PROTOCOL(1234,5679)
#define NEGOTIATE_GSS_CODE PG_PROTOCOL(1234,5680)

#endif							/* PQCOMM_H */
