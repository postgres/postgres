/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteDefine.h,v 1.9 2001/01/24 19:43:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEDEFINE_H
#define REWRITEDEFINE_H

#include "nodes/parsenodes.h"

extern void DefineQueryRewrite(RuleStmt *args);

#endif	 /* REWRITEDEFINE_H */
