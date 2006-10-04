/*-------------------------------------------------------------------------
 *
 * libpq_be.h
 *	  This file contains definitions for structures and externs used
 *	  by the postmaster during client authentication.
 *
 *	  Note that this is backend-internal and is NOT exported to clients.
 *	  Structs that need to be client-visible are in pqcomm.h.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/libpq/libpq-be.h,v 1.57 2006/10/04 00:30:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_H
#define LIBPQ_BE_H

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#include "libpq/hba.h"
#include "libpq/pqcomm.h"
#include "utils/timestamp.h"


typedef enum CAC_state
{
	CAC_OK, CAC_STARTUP, CAC_SHUTDOWN, CAC_RECOVERY, CAC_TOOMANY
} CAC_state;

/*
 * This is used by the postmaster in its communication with frontends.	It
 * contains all state information needed during this communication before the
 * backend is run.	The Port structure is kept in malloc'd memory and is
 * still available when a backend is running (see MyProcPort).	The data
 * it points to must also be malloc'd, or else palloc'd in TopMemoryContext,
 * so that it survives into PostgresMain execution!
 */

typedef struct Port
{
	int			sock;			/* File descriptor */
	ProtocolVersion proto;		/* FE/BE protocol version */
	SockAddr	laddr;			/* local addr (postmaster) */
	SockAddr	raddr;			/* remote addr (client) */
	char	   *remote_host;	/* name (or ip addr) of remote host */
	char	   *remote_port;	/* text rep of remote port */
	CAC_state	canAcceptConnections;	/* postmaster connection status */

	/*
	 * Information that needs to be saved from the startup packet and passed
	 * into backend execution.	"char *" fields are NULL if not set.
	 * guc_options points to a List of alternating option names and values.
	 */
	char	   *database_name;
	char	   *user_name;
	char	   *cmdline_options;
	List	   *guc_options;

	/*
	 * Information that needs to be held during the authentication cycle.
	 */
	UserAuth	auth_method;
	char	   *auth_arg;
	char		md5Salt[4];		/* Password salt */
	char		cryptSalt[2];	/* Password salt */

	/*
	 * Information that really has no business at all being in struct Port,
	 * but since it gets used by elog.c in the same way as database_name and
	 * other members of this struct, we may as well keep it here.
	 */
	TimestampTz SessionStartTime;		/* backend start time */
	time_t		session_start;	/* same, in time_t format */

	/*
	 * TCP keepalive settings.
	 *
	 * default values are 0 if AF_UNIX or not yet known; current values are 0
	 * if AF_UNIX or using the default. Also, -1 in a default value means we
	 * were unable to find out the default (getsockopt failed).
	 */
	int			default_keepalives_idle;
	int			default_keepalives_interval;
	int			default_keepalives_count;
	int			keepalives_idle;
	int			keepalives_interval;
	int			keepalives_count;

	/*
	 * SSL structures (keep these last so that USE_SSL doesn't affect
	 * locations of other fields)
	 */
#ifdef USE_SSL
	SSL		   *ssl;
	X509	   *peer;
	char		peer_dn[128 + 1];
	char		peer_cn[SM_USER + 1];
	unsigned long count;
#endif
} Port;


extern ProtocolVersion FrontendProtocol;

/* TCP keepalives configuration. These are no-ops on an AF_UNIX socket. */

extern int	pq_getkeepalivesidle(Port *port);
extern int	pq_getkeepalivesinterval(Port *port);
extern int	pq_getkeepalivescount(Port *port);

extern int	pq_setkeepalivesidle(int idle, Port *port);
extern int	pq_setkeepalivesinterval(int interval, Port *port);
extern int	pq_setkeepalivescount(int count, Port *port);

#endif   /* LIBPQ_BE_H */
