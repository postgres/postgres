/*-------------------------------------------------------------------------
 *
 * prep.h
 *	  prototypes for files in optimizer/prep/
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: prep.h,v 1.25 2000/11/12 00:37:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREP_H
#define PREP_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

/*
 * prototypes for prepqual.c
 */
extern List *canonicalize_qual(Expr *qual, bool removeAndFlag);
extern List *cnfify(Expr *qual, bool removeAndFlag);

/*
 * prototypes for preptlist.c
 */
extern List *preprocess_targetlist(List *tlist, int command_type,
					  Index result_relation, List *range_table);

/*
 * prototypes for prepunion.c
 */
extern Plan *plan_set_operations(Query *parse);

extern List *find_all_inheritors(Oid parentrel);
extern List *expand_inherted_rtentry(Query *parse, Index rti);

extern Node *adjust_inherited_attrs(Node *node,
									Index old_rt_index, Oid old_relid,
									Index new_rt_index, Oid new_relid);

#endif	 /* PREP_H */
