/*-------------------------------------------------------------------------
 *
 * libpq.h
 *	  POSTGRES LIBPQ buffer structure definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: libpq.h,v 1.43 2001/01/24 19:43:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPQ_H
#define LIBPQ_H

#include <sys/types.h>
#include <netinet/in.h>

#include "lib/stringinfo.h"
#include "libpq/libpq-be.h"

/* ----------------
 * PQArgBlock
 *		Information (pointer to array of this structure) required
 *		for the PQfn() call.  (This probably ought to go somewhere else...)
 * ----------------
 */
typedef struct
{
	int			len;
	int			isint;
	union
	{
		int		   *ptr;		/* can't use void (dec compiler barfs)	 */
		int			integer;
	}			u;
} PQArgBlock;

/*
 * PQerrormsg[] is used only for error messages generated within backend
 * libpq, none of which are remarkably long.  Note that this length should
 * NOT be taken as any indication of the maximum error message length that
 * the backend can create!	elog() can in fact produce extremely long messages.
 */

#define PQERRORMSG_LENGTH 1024

extern char PQerrormsg[PQERRORMSG_LENGTH];		/* in libpq/util.c */

/*
 * External functions.
 */

/*
 * prototypes for functions in pqcomm.c
 */
extern int	StreamServerPort(int family, char *hostName,
			unsigned short portNumber, char *unixSocketName, int *fdP);
extern int	StreamConnection(int server_fd, Port *port);
extern void StreamClose(int sock);
extern void pq_init(void);
extern int	pq_getbytes(char *s, size_t len);
extern int	pq_getstring(StringInfo s);
extern int	pq_peekbyte(void);
extern int	pq_putbytes(const char *s, size_t len);
extern int	pq_flush(void);
extern int	pq_putmessage(char msgtype, const char *s, size_t len);
extern void pq_startcopyout(void);
extern void pq_endcopyout(bool errorAbort);

/*
 * prototypes for functions in util.c
 */
extern void pqdebug(char *fmt, char *msg);
extern void PQtrace(void);
extern void PQuntrace(void);

#endif	 /* LIBPQ_H */
