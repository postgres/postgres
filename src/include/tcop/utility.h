/*-------------------------------------------------------------------------
 *
 * utility.h--
 *    prototypes for utility.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: utility.h,v 1.2 1996/11/04 12:07:05 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef UTILITY_H
#define UTILITY_H

#include <executor/execdesc.h>

extern void ProcessUtility(Node *parsetree, CommandDest dest);

#endif	/* UTILITY_H */
