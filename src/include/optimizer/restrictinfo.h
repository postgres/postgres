/*-------------------------------------------------------------------------
 *
 * restrictinfo.h
 *	  prototypes for restrictinfo.c.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/optimizer/restrictinfo.h,v 1.38.2.1 2007/07/31 19:53:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RESTRICTINFO_H
#define RESTRICTINFO_H

#include "nodes/relation.h"


extern RestrictInfo *make_restrictinfo(Expr *clause,
				  bool is_pushed_down,
				  bool outerjoin_delayed,
				  bool pseudoconstant,
				  Relids required_relids);
extern List *make_restrictinfo_from_bitmapqual(Path *bitmapqual,
								  bool is_pushed_down,
								  bool include_predicates);
extern bool restriction_is_or_clause(RestrictInfo *restrictinfo);
extern List *get_actual_clauses(List *restrictinfo_list);
extern List *extract_actual_clauses(List *restrictinfo_list,
					   bool pseudoconstant);
extern void extract_actual_join_clauses(List *restrictinfo_list,
							List **joinquals,
							List **otherquals);
extern List *remove_redundant_join_clauses(PlannerInfo *root,
							  List *restrictinfo_list,
							  Relids outer_relids,
							  Relids inner_relids,
							  bool isouterjoin);
extern List *select_nonredundant_join_clauses(PlannerInfo *root,
								 List *restrictinfo_list,
								 List *reference_list,
								 Relids outer_relids,
								 Relids inner_relids,
								 bool isouterjoin);

#endif   /* RESTRICTINFO_H */
