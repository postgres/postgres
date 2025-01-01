/*-------------------------------------------------------------------------
 *
 * selfuncs.h
 *	  Selectivity functions for standard operators, and assorted
 *	  infrastructure for selectivity and cost estimation.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/selfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SELFUNCS_H
#define SELFUNCS_H

#include "access/htup.h"
#include "fmgr.h"
#include "nodes/pathnodes.h"


/*
 * Note: the default selectivity estimates are not chosen entirely at random.
 * We want them to be small enough to ensure that indexscans will be used if
 * available, for typical table densities of ~100 tuples/page.  Thus, for
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

/* default selectivity estimate for multirange inequalities "A > b AND A < c" */
#define DEFAULT_MULTIRANGE_INEQ_SEL	0.005

/* default selectivity estimate for pattern-match operators such as LIKE */
#define DEFAULT_MATCH_SEL	0.005

/* default selectivity estimate for other matching operators */
#define DEFAULT_MATCHING_SEL	0.010

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

/*
 * A set of flags which some selectivity estimation functions can pass back to
 * callers to provide further details about some assumptions which were made
 * during the estimation.
 */
#define SELFLAG_USED_DEFAULT		(1 << 0)	/* Estimation fell back on one
												 * of the DEFAULTs as defined
												 * above. */

typedef struct EstimationInfo
{
	uint32		flags;			/* Flags, as defined above to mark special
								 * properties of the estimation. */
} EstimationInfo;

/* Return data from examine_variable and friends */
typedef struct VariableStatData
{
	Node	   *var;			/* the Var or expression tree */
	RelOptInfo *rel;			/* Relation, or NULL if not identifiable */
	HeapTuple	statsTuple;		/* pg_statistic tuple, or NULL if none */
	/* NB: if statsTuple!=NULL, it must be freed when caller is done */
	void		(*freefunc) (HeapTuple tuple);	/* how to free statsTuple */
	Oid			vartype;		/* exposed type of expression */
	Oid			atttype;		/* actual type (after stripping relabel) */
	int32		atttypmod;		/* actual typmod (after stripping relabel) */
	bool		isunique;		/* matches unique index or DISTINCT clause */
	bool		acl_ok;			/* result of ACL check on table or column */
} VariableStatData;

#define ReleaseVariableStats(vardata)  \
	do { \
		if (HeapTupleIsValid((vardata).statsTuple)) \
			(vardata).freefunc((vardata).statsTuple); \
	} while(0)


/*
 * genericcostestimate is a general-purpose estimator that can be used for
 * most index types.  In some cases we use genericcostestimate as the base
 * code and then incorporate additional index-type-specific knowledge in
 * the type-specific calling function.  To avoid code duplication, we make
 * genericcostestimate return a number of intermediate values as well as
 * its preliminary estimates of the output cost values.  The GenericCosts
 * struct includes all these values.
 *
 * Callers should initialize all fields of GenericCosts to zero.  In addition,
 * they can set numIndexTuples to some positive value if they have a better
 * than default way of estimating the number of leaf index tuples visited.
 * Similarly, they can set num_sa_scans to some value >= 1 for an index AM
 * that doesn't necessarily perform exactly one primitive index scan per
 * distinct combination of ScalarArrayOp array elements.
 */
typedef struct
{
	/* These are the values the cost estimator must return to the planner */
	Cost		indexStartupCost;	/* index-related startup cost */
	Cost		indexTotalCost; /* total index-related scan cost */
	Selectivity indexSelectivity;	/* selectivity of index */
	double		indexCorrelation;	/* order correlation of index */

	/* Intermediate values we obtain along the way */
	double		numIndexPages;	/* number of leaf pages visited */
	double		numIndexTuples; /* number of leaf tuples visited */
	double		spc_random_page_cost;	/* relevant random_page_cost value */
	double		num_sa_scans;	/* # indexscans from ScalarArrayOpExprs */
} GenericCosts;

/* Hooks for plugins to get control when we ask for stats */
typedef bool (*get_relation_stats_hook_type) (PlannerInfo *root,
											  RangeTblEntry *rte,
											  AttrNumber attnum,
											  VariableStatData *vardata);
extern PGDLLIMPORT get_relation_stats_hook_type get_relation_stats_hook;
typedef bool (*get_index_stats_hook_type) (PlannerInfo *root,
										   Oid indexOid,
										   AttrNumber indexattnum,
										   VariableStatData *vardata);
extern PGDLLIMPORT get_index_stats_hook_type get_index_stats_hook;

/* Functions in selfuncs.c */

extern void examine_variable(PlannerInfo *root, Node *node, int varRelid,
							 VariableStatData *vardata);
extern bool statistic_proc_security_check(VariableStatData *vardata, Oid func_oid);
extern bool get_restriction_variable(PlannerInfo *root, List *args,
									 int varRelid,
									 VariableStatData *vardata, Node **other,
									 bool *varonleft);
extern void get_join_variables(PlannerInfo *root, List *args,
							   SpecialJoinInfo *sjinfo,
							   VariableStatData *vardata1,
							   VariableStatData *vardata2,
							   bool *join_is_reversed);
extern double get_variable_numdistinct(VariableStatData *vardata,
									   bool *isdefault);
extern double mcv_selectivity(VariableStatData *vardata,
							  FmgrInfo *opproc, Oid collation,
							  Datum constval, bool varonleft,
							  double *sumcommonp);
extern double histogram_selectivity(VariableStatData *vardata,
									FmgrInfo *opproc, Oid collation,
									Datum constval, bool varonleft,
									int min_hist_size, int n_skip,
									int *hist_size);
extern double generic_restriction_selectivity(PlannerInfo *root,
											  Oid oproid, Oid collation,
											  List *args, int varRelid,
											  double default_selectivity);
extern double ineq_histogram_selectivity(PlannerInfo *root,
										 VariableStatData *vardata,
										 Oid opoid, FmgrInfo *opproc,
										 bool isgt, bool iseq,
										 Oid collation,
										 Datum constval, Oid consttype);
extern double var_eq_const(VariableStatData *vardata,
						   Oid oproid, Oid collation,
						   Datum constval, bool constisnull,
						   bool varonleft, bool negate);
extern double var_eq_non_const(VariableStatData *vardata,
							   Oid oproid, Oid collation,
							   Node *other,
							   bool varonleft, bool negate);

extern Selectivity boolvarsel(PlannerInfo *root, Node *arg, int varRelid);
extern Selectivity booltestsel(PlannerInfo *root, BoolTestType booltesttype,
							   Node *arg, int varRelid,
							   JoinType jointype, SpecialJoinInfo *sjinfo);
extern Selectivity nulltestsel(PlannerInfo *root, NullTestType nulltesttype,
							   Node *arg, int varRelid,
							   JoinType jointype, SpecialJoinInfo *sjinfo);
extern Selectivity scalararraysel(PlannerInfo *root,
								  ScalarArrayOpExpr *clause,
								  bool is_join_clause,
								  int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo);
extern double estimate_array_length(PlannerInfo *root, Node *arrayexpr);
extern Selectivity rowcomparesel(PlannerInfo *root,
								 RowCompareExpr *clause,
								 int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo);

extern void mergejoinscansel(PlannerInfo *root, Node *clause,
							 Oid opfamily, int strategy, bool nulls_first,
							 Selectivity *leftstart, Selectivity *leftend,
							 Selectivity *rightstart, Selectivity *rightend);

extern double estimate_num_groups(PlannerInfo *root, List *groupExprs,
								  double input_rows, List **pgset,
								  EstimationInfo *estinfo);

extern void estimate_hash_bucket_stats(PlannerInfo *root,
									   Node *hashkey, double nbuckets,
									   Selectivity *mcv_freq,
									   Selectivity *bucketsize_frac);
extern double estimate_hashagg_tablesize(PlannerInfo *root, Path *path,
										 const AggClauseCosts *agg_costs,
										 double dNumGroups);

extern List *get_quals_from_indexclauses(List *indexclauses);
extern Cost index_other_operands_eval_cost(PlannerInfo *root,
										   List *indexquals);
extern List *add_predicate_to_index_quals(IndexOptInfo *index,
										  List *indexQuals);
extern void genericcostestimate(PlannerInfo *root, IndexPath *path,
								double loop_count,
								GenericCosts *costs);

/* Functions in array_selfuncs.c */

extern Selectivity scalararraysel_containment(PlannerInfo *root,
											  Node *leftop, Node *rightop,
											  Oid elemtype, bool isEquality, bool useOr,
											  int varRelid);

#endif							/* SELFUNCS_H */
