/*-------------------------------------------------------------------------
 *
 * prep.h--
 *	  prototypes for files in prep.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: prep.h,v 1.11 1997/12/29 01:13:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREP_H
#define PREP_H

#include <nodes/plannodes.h>
#include <nodes/parsenodes.h>

/*
 * prototypes for prepqual.h
 */
extern List *cnfify(Expr *qual, bool removeAndFlag);

/*
 * prototypes for preptlist.h
 */
extern List *preprocess_targetlist(List *tlist, int command_type,
					  Index result_relation, List *range_table);

extern List *find_all_inheritors(List *unexamined_relids,
					List *examined_relids);
extern int	first_inherit_rt_entry(List *rangetable);
extern Append *plan_union_queries(Query *parse);
extern Append *plan_inherit_queries(Query *parse, Index rt_index);

#endif							/* PREP_H */
