/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error logging definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.32 2002/03/04 01:46:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

/* Error level codes */
#define DEBUG5	10				/* Debugging messages, in categories
								 * of decreasing detail. */
#define DEBUG4	11
#define DEBUG3	12				
#define DEBUG2	13				
#define DEBUG1	14				
#define LOG		15				/* Server operational history messages;
								 * sent only to server log by default. */
#define COMMERROR 16			/* Client communication problems; same as
								 * LOG for server reporting, but never ever
								 * try to send to client. */
#define INFO	17				/* Informative messages that are part of
								 * normal query operation; sent only to
								 * client by default. */
#define NOTICE	18				/* Important messages, for unusual cases that
								 * should be reported but are not serious 
								 * enough to abort the query.  Sent to client
								 * and server log by default. */
#define ERROR	19				/* user error - return to known state */
#define FATAL	20				/* fatal error - abort process */
#define PANIC	21				/* take down the other backends with me */

/*#define DEBUG	DEBUG1*/		/* Backward compatibility with pre-7.3 */

/* Configurable parameters */
#ifdef ENABLE_SYSLOG
extern int	Use_syslog;
#endif
extern bool Log_timestamp;
extern bool Log_pid;

extern char	   *server_min_messages_str;
extern char	   *client_min_messages_str;
extern const char server_min_messages_str_default[];
extern const char client_min_messages_str_default[];

extern void
elog(int lev, const char *fmt,...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 2, 3)));

extern int	DebugFileOpen(void);

extern bool check_server_min_messages(const char *lev);
extern void assign_server_min_messages(const char *lev);
extern bool check_client_min_messages(const char *lev);
extern void assign_client_min_messages(const char *lev);

#endif   /* ELOG_H */
