/*-------------------------------------------------------------------------
 *
 * libpq-int.h--
 *	  This file contains internal definitions meant to be used only by
 *	  the frontend libpq library, not by applications that call it.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq-int.h,v 1.2 1998/09/01 04:40:12 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef LIBPQ_INT_H
#define LIBPQ_INT_H

/* We assume libpq-fe.h has already been included. */

/* ----------------
 *		include stuff common to fe and be
 * ----------------
 */
#include "libpq/pqcomm.h"

/* libpq supports this version of the frontend/backend protocol.
 *
 * NB: we used to use PG_PROTOCOL_LATEST from the backend pqcomm.h file,
 * but that's not really the right thing: just recompiling libpq
 * against a more recent backend isn't going to magically update it
 * for most sorts of protocol changes.	So, when you change libpq
 * to support a different protocol revision, you have to change this
 * constant too.  PG_PROTOCOL_EARLIEST and PG_PROTOCOL_LATEST in
 * pqcomm.h describe what the backend knows, not what libpq knows.
 */

#define PG_PROTOCOL_LIBPQ	PG_PROTOCOL(2,0)

/* ----------------
 * Internal functions of libpq
 * Functions declared here need to be visible across files of libpq,
 * but are not intended to be called by applications.  We use the
 * convention "pqXXX" for internal functions, vs. the "PQxxx" names
 * used for application-visible routines.
 * ----------------
 */

/* === in fe-connect.c === */

extern int	pqPacketSend(PGconn *conn, const char *buf, size_t len);

/* === in fe-exec.c === */

extern void pqClearAsyncResult(PGconn *conn);

/* === in fe-misc.c === */

 /*
  * "Get" and "Put" routines return 0 if successful, EOF if not. Note that
  * for Get, EOF merely means the buffer is exhausted, not that there is
  * necessarily any error.
  */
extern int	pqGetc(char *result, PGconn *conn);
extern int	pqGets(char *s, int maxlen, PGconn *conn);
extern int	pqPuts(const char *s, PGconn *conn);
extern int	pqGetnchar(char *s, int len, PGconn *conn);
extern int	pqPutnchar(const char *s, int len, PGconn *conn);
extern int	pqGetInt(int *result, int bytes, PGconn *conn);
extern int	pqPutInt(int value, int bytes, PGconn *conn);
extern int	pqReadData(PGconn *conn);
extern int	pqFlush(PGconn *conn);
extern int	pqWait(int forRead, int forWrite, PGconn *conn);

/* max length of message to send  */
#define MAX_MESSAGE_LEN 8193

/* maximum number of fields in a tuple */
#define MAX_FIELDS 512

/* bits in a byte */
#define BYTELEN 8

/* fall back options if they are not specified by arguments or defined
   by environment variables */
#define DefaultHost		"localhost"
#define DefaultTty		""
#define DefaultOption	""
#define DefaultAuthtype		  ""
#define DefaultPassword		  ""

/* supply an implementation of strerror() macro if system doesn't have it */
#ifndef strerror
#if defined(sun) && defined(sparc) && !defined(__SVR4)
extern char *sys_errlist[];

#define strerror(A) (sys_errlist[(A)])
#endif	 /* sunos4 */
#endif	 /* !strerror */

#endif	 /* LIBPQ_INT_H */
