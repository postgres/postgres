/*-------------------------------------------------------------------------
 *
 * rewriteSupport.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteSupport.h,v 1.13 2000/09/29 18:21:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESUPPORT_H
#define REWRITESUPPORT_H

extern int	IsDefinedRewriteRule(char *ruleName);

extern void SetRelationRuleStatus(Oid relationId, bool relHasRules,
								  bool relIsBecomingView);

#endif	 /* REWRITESUPPORT_H */
