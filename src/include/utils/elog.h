/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error logging definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.10 1999/02/13 23:22:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

#define NOTICE	0				/* random info - no special action */
#define ERROR	-1				/* user error - return to known state */
#define FATAL	1				/* Fatal error - abort process */
#define DEBUG	-2				/* debug message */
#define NOIND	-3				/* debug message, don't indent as far */

#ifdef NOT_USED
#define PTIME	0x100			/* prepend time to message */
#define POS		0x200			/* prepend source position to message */
#define USERMSG 0x400			/* send message to user */
#define TERM	0x800			/* send message to terminal */
#define DBLOG	0x1000			/* put message in per db log */
#define SLOG	0x2000			/* put message in system log */
#define ABORTX	0x4000			/* abort process after logging */
#endif

/***S*I***/
/* Increase this to be able to use postmaster -d 3 with complex
 * view definitions (which are transformed to very, very large INSERT statements
 * and if -d 3 is used the query string of these statements is printed using
 * vsprintf which expects enough memory reserved! */
#define ELOG_MAXLEN 12288


/* uncomment the following if you want your elog's to be timestamped */
/* #define ELOG_TIMESTAMPS */

extern void elog(int lev, const char *fmt,...);

#ifndef PG_STANDALONE
int			DebugFileOpen(void);

#endif

#endif	 /* ELOG_H */
