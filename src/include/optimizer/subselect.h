/*-------------------------------------------------------------------------
 *
 * subselect.h
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/subselect.h,v 1.33 2008/08/17 01:20:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SUBSELECT_H
#define SUBSELECT_H

#include "nodes/plannodes.h"
#include "nodes/relation.h"

extern bool convert_ANY_sublink_to_join(PlannerInfo *root, SubLink *sublink,
										Relids available_rels,
										Node **new_qual, List **fromlist);
extern bool convert_EXISTS_sublink_to_join(PlannerInfo *root, SubLink *sublink,
										   bool under_not,
										   Relids available_rels,
										   Node **new_qual, List **fromlist);
extern Node *SS_replace_correlation_vars(PlannerInfo *root, Node *expr);
extern Node *SS_process_sublinks(PlannerInfo *root, Node *expr, bool isQual);
extern void SS_finalize_plan(PlannerInfo *root, Plan *plan,
							 bool attach_initplans);
extern Param *SS_make_initplan_from_plan(PlannerInfo *root, Plan *plan,
						   Oid resulttype, int32 resulttypmod);

#endif   /* SUBSELECT_H */
