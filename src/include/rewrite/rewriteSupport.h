/*-------------------------------------------------------------------------
 *
 * rewriteSupport.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteSupport.h,v 1.12 2000/06/30 07:04:04 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESUPPORT_H
#define REWRITESUPPORT_H

extern int	IsDefinedRewriteRule(char *ruleName);

extern void setRelhasrulesInRelation(Oid relationId, bool relhasrules);

#endif	 /* REWRITESUPPORT_H */
