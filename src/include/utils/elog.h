/*-------------------------------------------------------------------------
 *
 * elog.h
 *	  POSTGRES error logging definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: elog.h,v 1.13 1999/09/27 15:48:12 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ELOG_H
#define ELOG_H

#define NOTICE	0				/* random info - no special action */
#define ERROR	(-1)			/* user error - return to known state */
#define FATAL	1				/* fatal error - abort process */
#define REALLYFATAL	2			/* take down the other backends with me */
#define	STOP	REALLYFATAL
#define DEBUG	(-2)			/* debug message */
#define	LOG		DEBUG
#define NOIND	(-3)			/* debug message, don't indent as far */

extern void elog(int lev, const char *fmt, ...);

#ifndef PG_STANDALONE
extern int	DebugFileOpen(void);
#endif

#endif	 /* ELOG_H */
