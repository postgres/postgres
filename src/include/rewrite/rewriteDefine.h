/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h
 *
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/rewrite/rewriteDefine.h,v 1.20 2004/12/31 22:03:41 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEDEFINE_H
#define REWRITEDEFINE_H

#include "nodes/parsenodes.h"

extern void DefineQueryRewrite(RuleStmt *args);

extern void RenameRewriteRule(Oid owningRel, const char *oldName,
				  const char *newName);

#endif   /* REWRITEDEFINE_H */
