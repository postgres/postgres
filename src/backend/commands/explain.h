/*-------------------------------------------------------------------------
 *
 * explain.h--
 *    prototypes for explain.c
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: explain.h,v 1.1.1.1 1996/07/09 06:21:21 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	EXPLAIN_H
#define	EXPLAIN_H

extern void ExplainQuery(Query *query, List *options, CommandDest dest);

#endif	/* EXPLAIN_H*/
