/*-------------------------------------------------------------------------
 *
 * prep.h--
 *	  prototypes for files in prep.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: prep.h,v 1.9 1997/12/20 07:59:44 momjian Exp $
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

/*
 * prototypes for prepunion.h
 */
typedef enum UnionFlag
{
	INHERITS_FLAG, VERSION_FLAG
} UnionFlag;

extern List *find_all_inheritors(List *unexamined_relids,
					List *examined_relids);
extern int	first_matching_rt_entry(List *rangetable, UnionFlag flag);
extern Append *plan_union_queries(Index rt_index, Query *parse,
				   UnionFlag flag);

#endif							/* PREP_H */
