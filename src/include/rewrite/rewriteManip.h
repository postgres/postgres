/*-------------------------------------------------------------------------
 *
 * rewriteManip.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rewriteManip.h,v 1.12 1999/01/18 00:10:16 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITEMANIP_H
#define REWRITEMANIP_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "rewrite/rewriteHandler.h"

/* RewriteManip.c */
void		OffsetVarNodes(Node *node, int offset, int sublevels_up);
void ChangeVarNodes(Node *node, int old_varno, int new_varno,
			   int sublevels_up);
void		AddQual(Query *parsetree, Node *qual);
void		AddHavingQual(Query *parsetree, Node *havingQual);

void		AddNotQual(Query *parsetree, Node *qual);
void            AddNotHavingQual(Query *parsetree, Node *havingQual);

void		FixNew(RewriteInfo *info, Query *parsetree);

void HandleRIRAttributeRule(Query *parsetree, List *rtable, List *targetlist,
					   int rt_index, int attr_num, int *modified,
					   int *badpostquel);

#endif	 /* REWRITEMANIP_H */
