/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  prototypes for utility.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: utility.h,v 1.16 2002/09/04 20:31:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef UTILITY_H
#define UTILITY_H

#include "executor/execdesc.h"

extern void ProcessUtility(Node *parsetree, CommandDest dest,
			   char *completionTag);

#endif   /* UTILITY_H */
