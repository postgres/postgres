/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error logging definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.31 2002/03/02 21:39:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

/* Error level codes */
#define DEBUG5	10				/* sent only to server logs, label DEBUG */
#define DEBUG4	11				/* logs in decreasing detail */
#define DEBUG3	12				
#define DEBUG2	13				
#define DEBUG1	14				
#define LOG		15				/* sent only to server logs by default, 
								 * label LOG. */
#define INFO	16				/* sent only to client by default, for
								 * informative messages that are part of
								 * normal query operation. */
#define NOTICE	17				/* sent to client and server by default,
								 * important messages, for unusual cases that
								 * should be reported but are not serious 
								 * enough to abort the query. */
#define ERROR	18				/* user error - return to known state */
#define FATAL	19				/* fatal error - abort process */
#define PANIC	20				/* take down the other backends with me */

/*#define DEBUG	DEBUG5*/		/* Backward compatibility with pre-7.3 */

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
