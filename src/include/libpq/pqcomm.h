/*-------------------------------------------------------------------------
 *
 * pqcomm.h
 *		Definitions common to frontends and backends.
 *
 * NOTE: for historical reasons, this does not correspond to pqcomm.c.
 * pqcomm.c's routines are declared in libpq.h.
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pqcomm.h,v 1.75 2003/01/06 09:58:36 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQCOMM_H
#define PQCOMM_H

#include <sys/types.h>
#ifdef WIN32
#include <winsock.h>
/* workaround for clashing defines of "ERROR" */
#ifdef ELOG_H
#undef ERROR
#define ERROR	(-1)
#endif
#else							/* not WIN32 */
#include <sys/socket.h>
#include <netdb.h>
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#include <netinet/in.h>
#endif   /* not WIN32 */


#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif

#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46
#endif


#ifndef HAVE_STRUCT_SOCKADDR_UN
struct sockaddr_un
{
	short int	sun_family;		/* AF_UNIX */
	char		sun_path[108];	/* path name (gag) */
};
#endif

/* Define a generic socket address type. */

typedef union SockAddr
{
	struct sockaddr sa;
	struct sockaddr_in in;
#ifdef HAVE_IPV6
	struct sockaddr_in6 in6;
#endif
	struct sockaddr_un un;
} SockAddr;


/* Configure the UNIX socket location for the well known port. */

#define UNIXSOCK_PATH(sun,port,defpath) \
		snprintf((sun).sun_path, sizeof((sun).sun_path), "%s/.s.PGSQL.%d", \
				((defpath) && *(defpath) != '\0') ? (defpath) : \
				DEFAULT_PGSOCKET_DIR, \
				(port))

/*
 *		We do this because sun_len is in BSD's struct, while others don't.
 *		We never actually set BSD's sun_len, and I can't think of a
 *		platform-safe way of doing it, but the code still works. bjm
 */
#if defined(SUN_LEN)
#define UNIXSOCK_LEN(sun) \
		(SUN_LEN(&(sun)))
#else
#define UNIXSOCK_LEN(sun) \
		(strlen((sun).sun_path) + offsetof(struct sockaddr_un, sun_path))
#endif

/*
 * These manipulate the frontend/backend protocol version number.
 *
 * The major number should be incremented for incompatible changes.  The minor
 * number should be incremented for compatible changes (eg. additional
 * functionality).
 *
 * If a backend supports version m.n of the protocol it must actually support
 * versions m.0..n].  Backend support for version m-1 can be dropped after a
 * `reasonable' length of time.
 *
 * A frontend isn't required to support anything other than the current
 * version.
 */

#define PG_PROTOCOL_MAJOR(v)	((v) >> 16)
#define PG_PROTOCOL_MINOR(v)	((v) & 0x0000ffff)
#define PG_PROTOCOL(m,n)	(((m) << 16) | (n))

/* The earliest and latest frontend/backend protocol version supported. */

#define PG_PROTOCOL_EARLIEST	PG_PROTOCOL(1,0)
#define PG_PROTOCOL_LATEST	PG_PROTOCOL(2,0)

/*
 * All packets sent to the postmaster start with the length.  This is omitted
 * from the different packet definitions specified below.
 */

typedef uint32 PacketLen;


/*
 * Startup message parameters sizes.  These must not be changed without changing
 * the protocol version.  These are all strings that are '\0' terminated only if
 * there is room.
 */

/*
 * FIXME: remove the fixed size limitations on the database name, user
 * name, and options fields and use a variable length field instead. The
 * actual limits on database & user name will then be NAMEDATALEN, which
 * can be changed without changing the FE/BE protocol. -neilc,2002/08/27
 */

#define SM_DATABASE		64
#define SM_USER			32
/* We append database name if db_user_namespace true. */
#define SM_DATABASE_USER (SM_DATABASE+SM_USER+1)		/* +1 for @ */
#define SM_OPTIONS		64
#define SM_UNUSED		64
#define SM_TTY			64

typedef uint32 ProtocolVersion; /* Fe/Be protocol version number */

typedef ProtocolVersion MsgType;


typedef struct StartupPacket
{
	ProtocolVersion protoVersion;		/* Protocol version */
	char		database[SM_DATABASE];	/* Database name */
	/* Db_user_namespace appends dbname */
	char		user[SM_USER];	/* User name */
	char		options[SM_OPTIONS];	/* Optional additional args */
	char		unused[SM_UNUSED];		/* Unused */
	char		tty[SM_TTY];	/* Tty for debug output */
} StartupPacket;

extern bool Db_user_namespace;

/* These are the authentication requests sent by the backend. */

#define AUTH_REQ_OK			0	/* User is authenticated  */
#define AUTH_REQ_KRB4		1	/* Kerberos V4 */
#define AUTH_REQ_KRB5		2	/* Kerberos V5 */
#define AUTH_REQ_PASSWORD	3	/* Password */
#define AUTH_REQ_CRYPT		4	/* crypt password */
#define AUTH_REQ_MD5		5	/* md5 password */
#define AUTH_REQ_SCM_CREDS	6	/* transfer SCM credentials */

typedef uint32 AuthRequest;


/* A client can also send a cancel-current-operation request to the postmaster.
 * This is uglier than sending it directly to the client's backend, but it
 * avoids depending on out-of-band communication facilities.
 */

/* The cancel request code must not match any protocol version number
 * we're ever likely to use.  This random choice should do.
 */
#define CANCEL_REQUEST_CODE PG_PROTOCOL(1234,5678)

typedef struct CancelRequestPacket
{
	/* Note that each field is stored in network byte order! */
	MsgType		cancelRequestCode;		/* code to identify a cancel
										 * request */
	uint32		backendPID;		/* PID of client's backend */
	uint32		cancelAuthCode; /* secret key to authorize cancel */
} CancelRequestPacket;


/*
 * A client can also start by sending a SSL negotiation request, to get a
 * secure channel.
 */
#define NEGOTIATE_SSL_CODE PG_PROTOCOL(1234,5679)

#endif   /* PQCOMM_H */
