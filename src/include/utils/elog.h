/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error logging definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.29 2001/10/28 06:26:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

/* Error level codes */
#define NOTICE	0				/* random info, sent to frontend */
#define ERROR	(-1)			/* user error - return to known state */
#define FATAL	1				/* fatal error - abort process */
#define REALLYFATAL 2			/* take down the other backends with me */
#define DEBUG	(-2)			/* debug message */

/* temporary nonsense... */
#define STOP	REALLYFATAL
#define LOG		DEBUG

/* Configurable parameters */
#ifdef ENABLE_SYSLOG
extern int	Use_syslog;
#endif
extern bool Log_timestamp;
extern bool Log_pid;


extern void
elog(int lev, const char *fmt,...)
/* This extension allows gcc to check the format string for consistency with
   the supplied arguments. */
__attribute__((format(printf, 2, 3)));

extern int	DebugFileOpen(void);

#endif	 /* ELOG_H */
