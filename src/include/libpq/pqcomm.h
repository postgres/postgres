/*-------------------------------------------------------------------------
 *
 * pqcomm.h--
 *      Definitions common to frontends and backends.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pqcomm.h,v 1.20 1998/01/27 04:08:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQCOMM_H
#define PQCOMM_H

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>

#include "c.h"


/* Define a generic socket address type. */

typedef union SockAddr {
	struct sockaddr		sa;
	struct sockaddr_in	in;
	struct sockaddr_un	un;
} SockAddr;


/* Configure the UNIX socket address for the well known port. */

#define	UNIXSOCK_PATH(sun,port) \
	(sprintf((sun).sun_path, "/tmp/.s.PGSQL.%d", (port)) + \
		sizeof ((sun).sun_len) + sizeof ((sun).sun_family))


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

#define	PG_PROTOCOL_MAJOR(v)	((v) >> 16)
#define	PG_PROTOCOL_MINOR(v)	((v) & 0x0000ffff)
#define	PG_PROTOCOL(m,n)	(((m) << 16) | (n))

/* The earliest and latest frontend/backend protocol version supported. */

#define	PG_PROTOCOL_EARLIEST	PG_PROTOCOL(0,0)
#define	PG_PROTOCOL_LATEST	PG_PROTOCOL(1,0)

/*
 * All packets sent to the postmaster start with the length.  This is omitted
 * from the different packet definitions specified below.
 */

typedef uint32 PacketLen;

 
/*
 * Startup message parameters sizes.  These must not be changed without changing
 * the protcol version.  These are all strings that are '\0' terminated only if
 * there is room.
 */

#define	SM_DATABASE		64
#define	SM_USER			32
#define	SM_OPTIONS		64
#define	SM_UNUSED		64
#define	SM_TTY			64

typedef uint32 ProtocolVersion;			/* Fe/Be protocol version nr. */

typedef struct StartupPacket {
	ProtocolVersion	protoVersion;		/* Protocol version */
	char		database[SM_DATABASE];	/* Database name */
	char		user[SM_USER];		/* User name */
	char		options[SM_OPTIONS];	/* Optional additional args */
	char		unused[SM_UNUSED];	/* Unused */
	char		tty[SM_TTY];		/* Tty for debug output */
} StartupPacket;


/* These are the authentication requests sent by the backend. */

#define	AUTH_REQ_OK		0		/* User is authenticated  */
#define	AUTH_REQ_KRB4		1		/* Kerberos V4 */
#define	AUTH_REQ_KRB5		2		/* Kerberos V5 */
#define	AUTH_REQ_PASSWORD	3		/* Password */
#define	AUTH_REQ_CRYPT		4		/* Encrypted password */

typedef uint32 AuthRequest;


/* This next section is to maintain compatibility with protocol v0.0. */

#define	STARTUP_MSG		7	/* Initialise a connection */
#define	STARTUP_KRB4_MSG	10	/* krb4 session follows */
#define	STARTUP_KRB5_MSG	11	/* krb5 session follows */
#define	STARTUP_PASSWORD_MSG	14	/* Password follows */

typedef ProtocolVersion MsgType;


/* in pqcompriv.c */
int			pqGetShort(int *, FILE *);
int			pqGetLong(int *, FILE *);
int			pqGetNBytes(char *, size_t, FILE *);
int			pqGetString(char *, size_t, FILE *);
int			pqGetByte(FILE *);

int			pqPutShort(int, FILE *);
int			pqPutLong(int, FILE *);
int			pqPutNBytes(const char *, size_t, FILE *);
int			pqPutString(const char *, FILE *);
int			pqPutByte(int, FILE *);

#endif							/* PQCOMM_H */
