/*-------------------------------------------------------------------------
 *
 * rewriteSupport.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteSupport.h,v 1.15 2001/01/24 19:43:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESUPPORT_H
#define REWRITESUPPORT_H

extern bool IsDefinedRewriteRule(char *ruleName);

extern void SetRelationRuleStatus(Oid relationId, bool relHasRules,
								  bool relIsBecomingView);

#endif	 /* REWRITESUPPORT_H */
