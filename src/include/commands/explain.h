/*-------------------------------------------------------------------------
 *
 * explain.h--
 *	  prototypes for explain.c
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: explain.h,v 1.4 1997/09/08 02:35:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

extern void ExplainQuery(Query * query, bool verbose, CommandDest dest);

#endif							/* EXPLAIN_H */
