/*-------------------------------------------------------------------------
 *
 * rewriteSupport.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteSupport.h,v 1.14 2000/11/16 22:30:46 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESUPPORT_H
#define REWRITESUPPORT_H

extern bool IsDefinedRewriteRule(char *ruleName);

extern void SetRelationRuleStatus(Oid relationId, bool relHasRules,
								  bool relIsBecomingView);

#endif	 /* REWRITESUPPORT_H */
