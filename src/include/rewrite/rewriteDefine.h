/*-------------------------------------------------------------------------
 *
 * rewriteDefine.h
 *
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/rewrite/rewriteDefine.h
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

extern Oid	DefineRule(RuleStmt *stmt, const char *queryString);

extern Oid DefineQueryRewrite(char *rulename,
				   Oid event_relid,
				   Node *event_qual,
				   CmdType event_type,
				   bool is_instead,
				   bool replace,
				   List *action);

extern Oid RenameRewriteRule(RangeVar *relation, const char *oldName,
				  const char *newName);

extern void setRuleCheckAsUser(Node *node, Oid userid);

extern void EnableDisableRule(Relation rel, const char *rulename,
				  char fires_when);

#endif   /* REWRITEDEFINE_H */
