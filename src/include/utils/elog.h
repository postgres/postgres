/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error logging definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.25 2001/01/24 19:43:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

/* Error level codes */
#define NOTICE	0				/* random info - no special action */
#define ERROR	(-1)			/* user error - return to known state */
#define FATAL	1				/* fatal error - abort process */
#define REALLYFATAL 2			/* take down the other backends with me */
#define STOP	REALLYFATAL
#define DEBUG	(-2)			/* debug message */
#define LOG		DEBUG
#define NOIND	(-3)			/* debug message, don't indent as far */

/* Configurable parameters */
#ifdef ENABLE_SYSLOG
extern int Use_syslog;
#endif
extern bool Log_timestamp;
extern bool Log_pid;


#ifndef __GNUC__
extern void elog(int lev, const char *fmt, ...);

#else
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
extern void elog(int lev, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#endif

extern int	DebugFileOpen(void);

#endif	 /* ELOG_H */
