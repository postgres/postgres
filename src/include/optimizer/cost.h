/*-------------------------------------------------------------------------
 *
 * cost.h--
 *	  prototypes for costsize.c and clausesel.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: cost.h,v 1.3 1997/09/07 04:58:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef COST_H
#define COST_H

/*
 * prototypes for costsize.c--
 *	  routines to compute costs and sizes
 */
extern bool		_enable_seqscan_;
extern bool		_enable_indexscan_;
extern bool		_enable_sort_;
extern bool		_enable_hash_;
extern bool		_enable_nestloop_;
extern bool		_enable_mergesort_;
extern bool		_enable_hashjoin_;

extern Cost		cost_seqscan(int relid, int relpages, int reltuples);
extern Cost
cost_index(Oid indexid, int expected_indexpages, Cost selec,
		   int relpages, int reltuples, int indexpages,
		   int indextuples, bool is_injoin);
extern Cost		cost_sort(List * keys, int tuples, int width, bool noread);
extern Cost
cost_nestloop(Cost outercost, Cost innercost, int outertuples,
			  int innertuples, int outerpages, bool is_indexjoin);
extern Cost
cost_mergesort(Cost outercost, Cost innercost,
			   List * outersortkeys, List * innersortkeys,
		   int outersize, int innersize, int outerwidth, int innerwidth);
extern Cost
cost_hashjoin(Cost outercost, Cost innercost, List * outerkeys,
			  List * innerkeys, int outersize, int innersize,
			  int outerwidth, int innerwidth);
extern int		compute_rel_size(Rel * rel);
extern int		compute_rel_width(Rel * rel);
extern int		compute_joinrel_size(JoinPath * joinpath);
extern int		page_size(int tuples, int width);

/*
 * prototypes for fuctions in clausesel.h--
 *	  routines to compute clause selectivities
 */
extern void		set_clause_selectivities(List * clauseinfo_list, Cost new_selectivity);
extern Cost		product_selec(List * clauseinfo_list);
extern void		set_rest_relselec(Query * root, List * rel_list);
extern void		set_rest_selec(Query * root, List * clauseinfo_list);
extern Cost
compute_clause_selec(Query * root,
					 Node * clause, List * or_selectivities);

#endif							/* COST_H */
