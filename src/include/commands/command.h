/*-------------------------------------------------------------------------
 *
 * command.h--
 *	  prototypes for command.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: command.h,v 1.7 1997/09/08 21:51:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMAND_H
#define COMMAND_H

#include <utils/portal.h>

extern MemoryContext PortalExecutorHeapMemory;

/*
 * PerformPortalFetch --
 *		Performs the POSTQUEL function FETCH.  Fetches count (or all if 0)
 * tuples in portal with name in the forward direction iff goForward.
 *
 * Exceptions:
 *		BadArg if forward invalid.
 *		"WARN" if portal not found.
 */
extern void
PerformPortalFetch(char *name, bool forward, int count,
				   char *tag, CommandDest dest);

/*
 * PerformPortalClose --
 *		Performs the POSTQUEL function CLOSE.
 */
extern void PerformPortalClose(char *name, CommandDest dest);

extern void PortalCleanup(Portal portal);

/*
 * PerformAddAttribute --
 *		Performs the POSTQUEL function ADD.
 */
extern void
PerformAddAttribute(char *relationName, char *userName,
					bool inh, ColumnDef *colDef);

#endif							/* COMMAND_H */
