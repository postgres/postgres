/*-------------------------------------------------------------------------
 *
 * command.h--
 *    prototypes for command.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: command.h,v 1.1 1996/08/28 07:21:43 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COMMAND_H
#define COMMAND_H

#include "utils/portal.h"
#include "tcop/dest.h"

extern MemoryContext PortalExecutorHeapMemory;

/*
 * PortalCleanup --
 *	Cleans up the query state of the portal.
 *
 * Exceptions:
 *	BadArg if portal invalid.
 */
extern void PortalCleanup(Portal portal);


/*
 * PerformPortalFetch --
 *	Performs the POSTQUEL function FETCH.  Fetches count (or all if 0)
 * tuples in portal with name in the forward direction iff goForward.
 *
 * Exceptions:
 *	BadArg if forward invalid.
 *	"WARN" if portal not found.
 */
extern void PerformPortalFetch(char *name, bool forward, int count,
			       char *tag, CommandDest dest);

/*
 * PerformPortalClose --
 *	Performs the POSTQUEL function CLOSE.
 */
extern void PerformPortalClose(char *name, CommandDest dest);

/*
 * PerformAddAttribute --
 *	Performs the POSTQUEL function ADD.
 */
extern void PerformAddAttribute(char *relationName, char *userName,
				bool inh, ColumnDef *colDef);

#endif /* COMMAND_H */
