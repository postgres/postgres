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
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq-be.h,v 1.37 2003/08/04 02:40:13 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_BE_H
#define LIBPQ_BE_H

#include "libpq/hba.h"
#include "libpq/pqcomm.h"

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif


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

	/*
	 * Information that needs to be saved from the startup packet and
	 * passed into backend execution.  "char *" fields are NULL if not
	 * set. guc_options points to a List of alternating option names and
	 * values.
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
	 * SSL structures
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

#endif   /* LIBPQ_BE_H */
