/*-------------------------------------------------------------------------
 *
 * rewriteManip.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteManip.h,v 1.23 2000/09/29 18:21:24 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEMANIP_H
#define REWRITEMANIP_H

#include "rewrite/rewriteHandler.h"

/* RewriteManip.c */
extern void OffsetVarNodes(Node *node, int offset, int sublevels_up);
extern void ChangeVarNodes(Node *node, int old_varno, int new_varno,
			   int sublevels_up);
extern void IncrementVarSublevelsUp(Node *node, int delta_sublevels_up,
						int min_sublevels_up);

extern bool rangeTableEntry_used(Node *node, int rt_index,
								 int sublevels_up);
extern bool attribute_used(Node *node, int rt_index, int attno,
						   int sublevels_up);

extern void AddQual(Query *parsetree, Node *qual);
extern void AddHavingQual(Query *parsetree, Node *havingQual);
extern void AddNotQual(Query *parsetree, Node *qual);

extern bool checkExprHasAggs(Node *node);
extern bool checkExprHasSubLink(Node *node);

extern Node *ResolveNew(Node *node, int target_varno, int sublevels_up,
						List *targetlist, int event, int update_varno);
extern void FixNew(RewriteInfo *info, Query *parsetree);

#endif	 /* REWRITEMANIP_H */
