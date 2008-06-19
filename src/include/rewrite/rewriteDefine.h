/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h
 *
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/rewrite/rewriteDefine.h,v 1.30 2008/06/19 00:46:06 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEDEFINE_H
#define REWRITEDEFINE_H

#include "nodes/parsenodes.h"
#include "utils/relcache.h"

#define RULE_FIRES_ON_ORIGIN	'O'
#define RULE_FIRES_ALWAYS		'A'
#define RULE_FIRES_ON_REPLICA	'R'
#define RULE_DISABLED			'D'

extern void DefineRule(RuleStmt *stmt, const char *queryString);

extern void DefineQueryRewrite(char *rulename,
				   Oid event_relid,
				   Node *event_qual,
				   CmdType event_type,
				   bool is_instead,
				   bool replace,
				   List *action);

extern void RenameRewriteRule(Oid owningRel, const char *oldName,
				  const char *newName);

extern void setRuleCheckAsUser(Node *node, Oid userid);

extern void EnableDisableRule(Relation rel, const char *rulename,
				  char fires_when);

#endif   /* REWRITEDEFINE_H */
