/*-------------------------------------------------------------------------
 *
 * elog.h--
 *	  POSTGRES error logging definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.6 1998/01/05 03:35:05 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

#define NOTICE	0				/* random info - no special action */
#define ERROR	-2				/* user error - return to known state */
#define ABORT	-1				/* system error - return to known state */
#define FATAL	1				/* Fatal error - abort process */
#define DEBUG	-3				/* debug message */
#define NOIND	-4				/* debug message, don't indent as far */

#ifdef NOT_USED
#define PTIME	0x100			/* prepend time to message */
#define POS		0x200			/* prepend source position to message */
#define USERMSG 0x400			/* send message to user */
#define TERM	0x800			/* send message to terminal */
#define DBLOG	0x1000			/* put message in per db log */
#define SLOG	0x2000			/* put message in system log */
#define ABORTX	0x4000			/* abort process after logging */
#endif

#define ELOG_MAXLEN 4096


/* uncomment the following if you want your elog's to be timestamped */
/* #define ELOG_TIMESTAMPS */

extern void elog(int lev, const char *fmt,...);

#ifndef PG_STANDALONE
int			DebugFileOpen(void);

#endif

#endif							/* ELOG_H */
