/*-------------------------------------------------------------------------
 *
 * rewriteRemove.h
 *
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/rewrite/rewriteRemove.h,v 1.22 2006/06/16 20:23:45 adunstan Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEREMOVE_H
#define REWRITEREMOVE_H

#include "nodes/parsenodes.h"


extern void RemoveRewriteRule(Oid owningRel, const char *ruleName,
				  DropBehavior behavior, bool missing_ok);
extern void RemoveRewriteRuleById(Oid ruleOid);

#endif   /* REWRITEREMOVE_H */
