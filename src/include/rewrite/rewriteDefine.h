/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h
 *
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/rewrite/rewriteDefine.h,v 1.24 2007/03/13 00:33:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEDEFINE_H
#define REWRITEDEFINE_H

#include "nodes/parsenodes.h"

extern void DefineRule(RuleStmt *stmt, const char *queryString);

extern void DefineQueryRewrite(char *rulename,
				   RangeVar *event_obj,
				   Node *event_qual,
				   CmdType event_type,
				   bool is_instead,
				   bool replace,
				   List *action);

extern void RenameRewriteRule(Oid owningRel, const char *oldName,
				  const char *newName);

extern void setRuleCheckAsUser(Node *node, Oid userid);

#endif   /* REWRITEDEFINE_H */
