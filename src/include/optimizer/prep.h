/*-------------------------------------------------------------------------
 *
 * prep.h
 *	  prototypes for files in optimizer/prep/
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: prep.h,v 1.22 2000/06/08 22:37:51 momjian Exp $
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
extern List *find_all_inheritors(Oid parentrel);
extern int	first_inherit_rt_entry(List *rangetable);
extern Append *plan_union_queries(Query *parse);
extern Append *plan_inherit_queries(Query *parse, List *tlist, Index rt_index);

#endif	 /* PREP_H */
