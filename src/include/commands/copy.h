/*-------------------------------------------------------------------------
 *
 * copy.h
 *	  Definitions for using the POSTGRES copy command.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: copy.h,v 1.18 2002/06/20 16:00:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COPY_H
#define COPY_H

#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"

extern int	copy_lineno;

void DoCopy(const CopyStmt *stmt);

#endif   /* COPY_H */
