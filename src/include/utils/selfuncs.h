/*-------------------------------------------------------------------------
 *
 * selfuncs.h
 *	  Selectivity functions and index cost estimation functions for
 *	  standard operators and index access methods.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/selfuncs.h,v 1.36.2.2 2007/11/07 22:37:33 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SELFUNCS_H
#define SELFUNCS_H

#include "fmgr.h"
#include "access/htup.h"
#include "nodes/relation.h"


/*
 * Note: the default selectivity estimates are not chosen entirely at random.
 * We want them to be small enough to ensure that indexscans will be used if
 * available, for typical table densities of ~100 tuples/page.	Thus, for
 * example, 0.01 is not quite small enough, since that makes it appear that
 * nearly all pages will be hit anyway.  Also, since we sometimes estimate
 * eqsel as 1/num_distinct, we probably want DEFAULT_NUM_DISTINCT to equal
 * 1/DEFAULT_EQ_SEL.
 */

/* default selectivity estimate for equalities such as "A = b" */
#define DEFAULT_EQ_SEL	0.005

/* default selectivity estimate for inequalities such as "A < b" */
#define DEFAULT_INEQ_SEL  0.3333333333333333

/* default selectivity estimate for range inequalities "A > b AND A < c" */
#define DEFAULT_RANGE_INEQ_SEL	0.005

/* default selectivity estimate for pattern-match operators such as LIKE */
#define DEFAULT_MATCH_SEL	0.005

/* default number of distinct values in a table */
#define DEFAULT_NUM_DISTINCT  200

/* default selectivity estimate for boolean and null test nodes */
#define DEFAULT_UNK_SEL			0.005
#define DEFAULT_NOT_UNK_SEL		(1.0 - DEFAULT_UNK_SEL)


/*
 * Clamp a computed probability estimate (which may suffer from roundoff or
 * estimation errors) to valid range.  Argument must be a float variable.
 */
#define CLAMP_PROBABILITY(p) \
	do { \
		if (p < 0.0) \
			p = 0.0; \
		else if (p > 1.0) \
			p = 1.0; \
	} while (0)


/* Return data from examine_variable and friends */
typedef struct
{
	Node	   *var;			/* the Var or expression tree */
	RelOptInfo *rel;			/* Relation, or NULL if not identifiable */
	HeapTuple	statsTuple;		/* pg_statistic tuple, or NULL if none */
	/* NB: if statsTuple!=NULL, it must be freed when caller is done */
	Oid			vartype;		/* exposed type of expression */
	Oid			atttype;		/* type to pass to get_attstatsslot */
	int32		atttypmod;		/* typmod to pass to get_attstatsslot */
	bool		isunique;		/* true if matched to a unique index */
} VariableStatData;

#define ReleaseVariableStats(vardata)  \
	do { \
		if (HeapTupleIsValid((vardata).statsTuple)) \
			ReleaseSysCache((vardata).statsTuple); \
	} while(0)


typedef enum
{
	Pattern_Type_Like, Pattern_Type_Like_IC,
	Pattern_Type_Regex, Pattern_Type_Regex_IC
} Pattern_Type;

typedef enum
{
	Pattern_Prefix_None, Pattern_Prefix_Partial, Pattern_Prefix_Exact
} Pattern_Prefix_Status;


/* selfuncs.c */

extern void examine_variable(PlannerInfo *root, Node *node, int varRelid,
				 VariableStatData *vardata);
extern bool get_restriction_variable(PlannerInfo *root, List *args,
						 int varRelid,
						 VariableStatData *vardata, Node **other,
						 bool *varonleft);
extern void get_join_variables(PlannerInfo *root, List *args,
				   VariableStatData *vardata1,
				   VariableStatData *vardata2);
extern double get_variable_numdistinct(VariableStatData *vardata);
extern double mcv_selectivity(VariableStatData *vardata, FmgrInfo *opproc,
				Datum constval, bool varonleft,
				double *sumcommonp);
extern double histogram_selectivity(VariableStatData *vardata, FmgrInfo *opproc,
					  Datum constval, bool varonleft,
					  int min_hist_size, int n_skip);

extern Pattern_Prefix_Status pattern_fixed_prefix(Const *patt,
					 Pattern_Type ptype,
					 Const **prefix,
					 Const **rest);
extern Const *make_greater_string(const Const *str_const, FmgrInfo *ltproc);

extern Datum eqsel(PG_FUNCTION_ARGS);
extern Datum neqsel(PG_FUNCTION_ARGS);
extern Datum scalarltsel(PG_FUNCTION_ARGS);
extern Datum scalargtsel(PG_FUNCTION_ARGS);
extern Datum regexeqsel(PG_FUNCTION_ARGS);
extern Datum icregexeqsel(PG_FUNCTION_ARGS);
extern Datum likesel(PG_FUNCTION_ARGS);
extern Datum iclikesel(PG_FUNCTION_ARGS);
extern Datum regexnesel(PG_FUNCTION_ARGS);
extern Datum icregexnesel(PG_FUNCTION_ARGS);
extern Datum nlikesel(PG_FUNCTION_ARGS);
extern Datum icnlikesel(PG_FUNCTION_ARGS);

extern Datum eqjoinsel(PG_FUNCTION_ARGS);
extern Datum neqjoinsel(PG_FUNCTION_ARGS);
extern Datum scalarltjoinsel(PG_FUNCTION_ARGS);
extern Datum scalargtjoinsel(PG_FUNCTION_ARGS);
extern Datum regexeqjoinsel(PG_FUNCTION_ARGS);
extern Datum icregexeqjoinsel(PG_FUNCTION_ARGS);
extern Datum likejoinsel(PG_FUNCTION_ARGS);
extern Datum iclikejoinsel(PG_FUNCTION_ARGS);
extern Datum regexnejoinsel(PG_FUNCTION_ARGS);
extern Datum icregexnejoinsel(PG_FUNCTION_ARGS);
extern Datum nlikejoinsel(PG_FUNCTION_ARGS);
extern Datum icnlikejoinsel(PG_FUNCTION_ARGS);

extern Selectivity booltestsel(PlannerInfo *root, BoolTestType booltesttype,
			Node *arg, int varRelid, JoinType jointype);
extern Selectivity nulltestsel(PlannerInfo *root, NullTestType nulltesttype,
			Node *arg, int varRelid, JoinType jointype);
extern Selectivity scalararraysel(PlannerInfo *root,
			   ScalarArrayOpExpr *clause,
			   bool is_join_clause,
			   int varRelid, JoinType jointype);
extern int	estimate_array_length(Node *arrayexpr);
extern Selectivity rowcomparesel(PlannerInfo *root,
			  RowCompareExpr *clause,
			  int varRelid, JoinType jointype);

extern void mergejoinscansel(PlannerInfo *root, Node *clause,
				 Selectivity *leftscan,
				 Selectivity *rightscan);

extern double estimate_num_groups(PlannerInfo *root, List *groupExprs,
					double input_rows);

extern Selectivity estimate_hash_bucketsize(PlannerInfo *root, Node *hashkey,
						 double nbuckets);

extern Datum btcostestimate(PG_FUNCTION_ARGS);
extern Datum hashcostestimate(PG_FUNCTION_ARGS);
extern Datum gistcostestimate(PG_FUNCTION_ARGS);
extern Datum gincostestimate(PG_FUNCTION_ARGS);

#endif   /* SELFUNCS_H */
