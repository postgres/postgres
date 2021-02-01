/*-------------------------------------------------------------------------
 *
 * partprune.c
 *		Support for partition pruning during query planning and execution
 *
 * This module implements partition pruning using the information contained in
 * a table's partition descriptor, query clauses, and run-time parameters.
 *
 * During planning, clauses that can be matched to the table's partition key
 * are turned into a set of "pruning steps", which are then executed to
 * identify a set of partitions (as indexes in the RelOptInfo->part_rels
 * array) that satisfy the constraints in the step.  Partitions not in the set
 * are said to have been pruned.
 *
 * A base pruning step may involve expressions whose values are only known
 * during execution, such as Params, in which case pruning cannot occur
 * entirely during planning.  In that case, such steps are included alongside
 * the plan, so that they can be used by the executor for further pruning.
 *
 * There are two kinds of pruning steps.  A "base" pruning step represents
 * tests on partition key column(s), typically comparisons to expressions.
 * A "combine" pruning step represents a Boolean connector (AND/OR), and
 * combines the outputs of some previous steps using the appropriate
 * combination method.
 *
 * See gen_partprune_steps_internal() for more details on step generation.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		  src/backend/partitioning/partprune.c
 *
 *-------------------------------------------------------------------------
*/
#include "postgres.h"

#include "access/hash.h"
#include "access/nbtree.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "partitioning/partbounds.h"
#include "partitioning/partprune.h"
#include "rewrite/rewriteManip.h"
#include "utils/array.h"
#include "utils/lsyscache.h"


/*
 * Information about a clause matched with a partition key.
 */
typedef struct PartClauseInfo
{
	int			keyno;			/* Partition key number (0 to partnatts - 1) */
	Oid			opno;			/* operator used to compare partkey to expr */
	bool		op_is_ne;		/* is clause's original operator <> ? */
	Expr	   *expr;			/* expr the partition key is compared to */
	Oid			cmpfn;			/* Oid of function to compare 'expr' to the
								 * partition key */
	int			op_strategy;	/* btree strategy identifying the operator */
} PartClauseInfo;

/*
 * PartClauseMatchStatus
 *		Describes the result of match_clause_to_partition_key()
 */
typedef enum PartClauseMatchStatus
{
	PARTCLAUSE_NOMATCH,
	PARTCLAUSE_MATCH_CLAUSE,
	PARTCLAUSE_MATCH_NULLNESS,
	PARTCLAUSE_MATCH_STEPS,
	PARTCLAUSE_MATCH_CONTRADICT,
	PARTCLAUSE_UNSUPPORTED
} PartClauseMatchStatus;

/*
 * PartClauseTarget
 *		Identifies which qual clauses we can use for generating pruning steps
 */
typedef enum PartClauseTarget
{
	PARTTARGET_PLANNER,			/* want to prune during planning */
	PARTTARGET_INITIAL,			/* want to prune during executor startup */
	PARTTARGET_EXEC				/* want to prune during each plan node scan */
} PartClauseTarget;

/*
 * GeneratePruningStepsContext
 *		Information about the current state of generation of "pruning steps"
 *		for a given set of clauses
 *
 * gen_partprune_steps() initializes and returns an instance of this struct.
 *
 * Note that has_mutable_op, has_mutable_arg, and has_exec_param are set if
 * we found any potentially-useful-for-pruning clause having those properties,
 * whether or not we actually used the clause in the steps list.  This
 * definition allows us to skip the PARTTARGET_EXEC pass in some cases.
 */
typedef struct GeneratePruningStepsContext
{
	/* Copies of input arguments for gen_partprune_steps: */
	RelOptInfo *rel;			/* the partitioned relation */
	PartClauseTarget target;	/* use-case we're generating steps for */
	/* Result data: */
	List	   *steps;			/* list of PartitionPruneSteps */
	bool		has_mutable_op; /* clauses include any stable operators */
	bool		has_mutable_arg;	/* clauses include any mutable comparison
									 * values, *other than* exec params */
	bool		has_exec_param; /* clauses include any PARAM_EXEC params */
	bool		contradictory;	/* clauses were proven self-contradictory */
	/* Working state: */
	int			next_step_id;
} GeneratePruningStepsContext;

/* The result of performing one PartitionPruneStep */
typedef struct PruneStepResult
{
	/*
	 * The offsets of bounds (in a table's boundinfo) whose partition is
	 * selected by the pruning step.
	 */
	Bitmapset  *bound_offsets;

	bool		scan_default;	/* Scan the default partition? */
	bool		scan_null;		/* Scan the partition for NULL values? */
} PruneStepResult;


static List *add_part_relids(List *allpartrelids, Bitmapset *partrelids);
static List *make_partitionedrel_pruneinfo(PlannerInfo *root,
										   RelOptInfo *parentrel,
										   List *prunequal,
										   Bitmapset *partrelids,
										   int *relid_subplan_map,
										   Bitmapset **matchedsubplans);
static void gen_partprune_steps(RelOptInfo *rel, List *clauses,
								PartClauseTarget target,
								GeneratePruningStepsContext *context);
static List *gen_partprune_steps_internal(GeneratePruningStepsContext *context,
										  List *clauses);
static PartitionPruneStep *gen_prune_step_op(GeneratePruningStepsContext *context,
											 StrategyNumber opstrategy, bool op_is_ne,
											 List *exprs, List *cmpfns, Bitmapset *nullkeys);
static PartitionPruneStep *gen_prune_step_combine(GeneratePruningStepsContext *context,
												  List *source_stepids,
												  PartitionPruneCombineOp combineOp);
static PartitionPruneStep *gen_prune_steps_from_opexps(GeneratePruningStepsContext *context,
													   List **keyclauses, Bitmapset *nullkeys);
static PartClauseMatchStatus match_clause_to_partition_key(GeneratePruningStepsContext *context,
														   Expr *clause, Expr *partkey, int partkeyidx,
														   bool *clause_is_not_null,
														   PartClauseInfo **pc, List **clause_steps);
static List *get_steps_using_prefix(GeneratePruningStepsContext *context,
									StrategyNumber step_opstrategy,
									bool step_op_is_ne,
									Expr *step_lastexpr,
									Oid step_lastcmpfn,
									int step_lastkeyno,
									Bitmapset *step_nullkeys,
									List *prefix);
static List *get_steps_using_prefix_recurse(GeneratePruningStepsContext *context,
											StrategyNumber step_opstrategy,
											bool step_op_is_ne,
											Expr *step_lastexpr,
											Oid step_lastcmpfn,
											int step_lastkeyno,
											Bitmapset *step_nullkeys,
											List *prefix,
											ListCell *start,
											List *step_exprs,
											List *step_cmpfns);
static PruneStepResult *get_matching_hash_bounds(PartitionPruneContext *context,
												 StrategyNumber opstrategy, Datum *values, int nvalues,
												 FmgrInfo *partsupfunc, Bitmapset *nullkeys);
static PruneStepResult *get_matching_list_bounds(PartitionPruneContext *context,
												 StrategyNumber opstrategy, Datum value, int nvalues,
												 FmgrInfo *partsupfunc, Bitmapset *nullkeys);
static PruneStepResult *get_matching_range_bounds(PartitionPruneContext *context,
												  StrategyNumber opstrategy, Datum *values, int nvalues,
												  FmgrInfo *partsupfunc, Bitmapset *nullkeys);
static Bitmapset *pull_exec_paramids(Expr *expr);
static bool pull_exec_paramids_walker(Node *node, Bitmapset **context);
static Bitmapset *get_partkey_exec_paramids(List *steps);
static PruneStepResult *perform_pruning_base_step(PartitionPruneContext *context,
												  PartitionPruneStepOp *opstep);
static PruneStepResult *perform_pruning_combine_step(PartitionPruneContext *context,
													 PartitionPruneStepCombine *cstep,
													 PruneStepResult **step_results);
static PartClauseMatchStatus match_boolean_partition_clause(Oid partopfamily,
															Expr *clause,
															Expr *partkey,
															Expr **outconst);
static void partkey_datum_from_expr(PartitionPruneContext *context,
									Expr *expr, int stateidx,
									Datum *value, bool *isnull);


/*
 * make_partition_pruneinfo
 *		Builds a PartitionPruneInfo which can be used in the executor to allow
 *		additional partition pruning to take place.  Returns NULL when
 *		partition pruning would be useless.
 *
 * 'parentrel' is the RelOptInfo for an appendrel, and 'subpaths' is the list
 * of scan paths for its child rels.
 * 'prunequal' is a list of potential pruning quals (i.e., restriction
 * clauses that are applicable to the appendrel).
 */
PartitionPruneInfo *
make_partition_pruneinfo(PlannerInfo *root, RelOptInfo *parentrel,
						 List *subpaths,
						 List *prunequal)
{
	PartitionPruneInfo *pruneinfo;
	Bitmapset  *allmatchedsubplans = NULL;
	List	   *allpartrelids;
	List	   *prunerelinfos;
	int		   *relid_subplan_map;
	ListCell   *lc;
	int			i;

	/*
	 * Scan the subpaths to see which ones are scans of partition child
	 * relations, and identify their parent partitioned rels.  (Note: we must
	 * restrict the parent partitioned rels to be parentrel or children of
	 * parentrel, otherwise we couldn't translate prunequal to match.)
	 *
	 * Also construct a temporary array to map from partition-child-relation
	 * relid to the index in 'subpaths' of the scan plan for that partition.
	 * (Use of "subplan" rather than "subpath" is a bit of a misnomer, but
	 * we'll let it stand.)  For convenience, we use 1-based indexes here, so
	 * that zero can represent an un-filled array entry.
	 */
	allpartrelids = NIL;
	relid_subplan_map = palloc0(sizeof(int) * root->simple_rel_array_size);

	i = 1;
	foreach(lc, subpaths)
	{
		Path	   *path = (Path *) lfirst(lc);
		RelOptInfo *pathrel = path->parent;

		/* We don't consider partitioned joins here */
		if (pathrel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		{
			RelOptInfo *prel = pathrel;
			Bitmapset  *partrelids = NULL;

			/*
			 * Traverse up to the pathrel's topmost partitioned parent,
			 * collecting parent relids as we go; but stop if we reach
			 * parentrel.  (Normally, a pathrel's topmost partitioned parent
			 * is either parentrel or a UNION ALL appendrel child of
			 * parentrel.  But when handling partitionwise joins of
			 * multi-level partitioning trees, we can see an append path whose
			 * parentrel is an intermediate partitioned table.)
			 */
			do
			{
				AppendRelInfo *appinfo;

				Assert(prel->relid < root->simple_rel_array_size);
				appinfo = root->append_rel_array[prel->relid];
				prel = find_base_rel(root, appinfo->parent_relid);
				if (!IS_PARTITIONED_REL(prel))
					break;		/* reached a non-partitioned parent */
				/* accept this level as an interesting parent */
				partrelids = bms_add_member(partrelids, prel->relid);
				if (prel == parentrel)
					break;		/* don't traverse above parentrel */
			} while (prel->reloptkind == RELOPT_OTHER_MEMBER_REL);

			if (partrelids)
			{
				/*
				 * Found some relevant parent partitions, which may or may not
				 * overlap with partition trees we already found.  Add new
				 * information to the allpartrelids list.
				 */
				allpartrelids = add_part_relids(allpartrelids, partrelids);
				/* Also record the subplan in relid_subplan_map[] */
				/* No duplicates please */
				Assert(relid_subplan_map[pathrel->relid] == 0);
				relid_subplan_map[pathrel->relid] = i;
			}
		}
		i++;
	}

	/*
	 * We now build a PartitionedRelPruneInfo for each topmost partitioned rel
	 * (omitting any that turn out not to have useful pruning quals).
	 */
	prunerelinfos = NIL;
	foreach(lc, allpartrelids)
	{
		Bitmapset  *partrelids = (Bitmapset *) lfirst(lc);
		List	   *pinfolist;
		Bitmapset  *matchedsubplans = NULL;

		pinfolist = make_partitionedrel_pruneinfo(root, parentrel,
												  prunequal,
												  partrelids,
												  relid_subplan_map,
												  &matchedsubplans);

		/* When pruning is possible, record the matched subplans */
		if (pinfolist != NIL)
		{
			prunerelinfos = lappend(prunerelinfos, pinfolist);
			allmatchedsubplans = bms_join(matchedsubplans,
										  allmatchedsubplans);
		}
	}

	pfree(relid_subplan_map);

	/*
	 * If none of the partition hierarchies had any useful run-time pruning
	 * quals, then we can just not bother with run-time pruning.
	 */
	if (prunerelinfos == NIL)
		return NULL;

	/* Else build the result data structure */
	pruneinfo = makeNode(PartitionPruneInfo);
	pruneinfo->prune_infos = prunerelinfos;

	/*
	 * Some subplans may not belong to any of the identified partitioned rels.
	 * This can happen for UNION ALL queries which include a non-partitioned
	 * table, or when some of the hierarchies aren't run-time prunable.  Build
	 * a bitmapset of the indexes of all such subplans, so that the executor
	 * can identify which subplans should never be pruned.
	 */
	if (bms_num_members(allmatchedsubplans) < list_length(subpaths))
	{
		Bitmapset  *other_subplans;

		/* Create the complement of allmatchedsubplans */
		other_subplans = bms_add_range(NULL, 0, list_length(subpaths) - 1);
		other_subplans = bms_del_members(other_subplans, allmatchedsubplans);

		pruneinfo->other_subplans = other_subplans;
	}
	else
		pruneinfo->other_subplans = NULL;

	return pruneinfo;
}

/*
 * add_part_relids
 *		Add new info to a list of Bitmapsets of partitioned relids.
 *
 * Within 'allpartrelids', there is one Bitmapset for each topmost parent
 * partitioned rel.  Each Bitmapset contains the RT indexes of the topmost
 * parent as well as its relevant non-leaf child partitions.  Since (by
 * construction of the rangetable list) parent partitions must have lower
 * RT indexes than their children, we can distinguish the topmost parent
 * as being the lowest set bit in the Bitmapset.
 *
 * 'partrelids' contains the RT indexes of a parent partitioned rel, and
 * possibly some non-leaf children, that are newly identified as parents of
 * some subpath rel passed to make_partition_pruneinfo().  These are added
 * to an appropriate member of 'allpartrelids'.
 *
 * Note that the list contains only RT indexes of partitioned tables that
 * are parents of some scan-level relation appearing in the 'subpaths' that
 * make_partition_pruneinfo() is dealing with.  Also, "topmost" parents are
 * not allowed to be higher than the 'parentrel' associated with the append
 * path.  In this way, we avoid expending cycles on partitioned rels that
 * can't contribute useful pruning information for the problem at hand.
 * (It is possible for 'parentrel' to be a child partitioned table, and it
 * is also possible for scan-level relations to be child partitioned tables
 * rather than leaf partitions.  Hence we must construct this relation set
 * with reference to the particular append path we're dealing with, rather
 * than looking at the full partitioning structure represented in the
 * RelOptInfos.)
 */
static List *
add_part_relids(List *allpartrelids, Bitmapset *partrelids)
{
	Index		targetpart;
	ListCell   *lc;

	/* We can easily get the lowest set bit this way: */
	targetpart = bms_next_member(partrelids, -1);
	Assert(targetpart > 0);

	/* Look for a matching topmost parent */
	foreach(lc, allpartrelids)
	{
		Bitmapset  *currpartrelids = (Bitmapset *) lfirst(lc);
		Index		currtarget = bms_next_member(currpartrelids, -1);

		if (targetpart == currtarget)
		{
			/* Found a match, so add any new RT indexes to this hierarchy */
			currpartrelids = bms_add_members(currpartrelids, partrelids);
			lfirst(lc) = currpartrelids;
			return allpartrelids;
		}
	}
	/* No match, so add the new partition hierarchy to the list */
	return lappend(allpartrelids, partrelids);
}

/*
 * make_partitionedrel_pruneinfo
 *		Build a List of PartitionedRelPruneInfos, one for each interesting
 *		partitioned rel in a partitioning hierarchy.  These can be used in the
 *		executor to allow additional partition pruning to take place.
 *
 * parentrel: rel associated with the appendpath being considered
 * prunequal: potential pruning quals, represented for parentrel
 * partrelids: Set of RT indexes identifying relevant partitioned tables
 *   within a single partitioning hierarchy
 * relid_subplan_map[]: maps child relation relids to subplan indexes
 * matchedsubplans: on success, receives the set of subplan indexes which
 *   were matched to this partition hierarchy
 *
 * If we cannot find any useful run-time pruning steps, return NIL.
 * However, on success, each rel identified in partrelids will have
 * an element in the result list, even if some of them are useless.
 */
static List *
make_partitionedrel_pruneinfo(PlannerInfo *root, RelOptInfo *parentrel,
							  List *prunequal,
							  Bitmapset *partrelids,
							  int *relid_subplan_map,
							  Bitmapset **matchedsubplans)
{
	RelOptInfo *targetpart = NULL;
	List	   *pinfolist = NIL;
	bool		doruntimeprune = false;
	int		   *relid_subpart_map;
	Bitmapset  *subplansfound = NULL;
	ListCell   *lc;
	int			rti;
	int			i;

	/*
	 * Examine each partitioned rel, constructing a temporary array to map
	 * from planner relids to index of the partitioned rel, and building a
	 * PartitionedRelPruneInfo for each partitioned rel.
	 *
	 * In this phase we discover whether runtime pruning is needed at all; if
	 * not, we can avoid doing further work.
	 */
	relid_subpart_map = palloc0(sizeof(int) * root->simple_rel_array_size);

	i = 1;
	rti = -1;
	while ((rti = bms_next_member(partrelids, rti)) > 0)
	{
		RelOptInfo *subpart = find_base_rel(root, rti);
		PartitionedRelPruneInfo *pinfo;
		List	   *partprunequal;
		List	   *initial_pruning_steps;
		List	   *exec_pruning_steps;
		Bitmapset  *execparamids;
		GeneratePruningStepsContext context;

		/*
		 * Fill the mapping array.
		 *
		 * relid_subpart_map maps relid of a non-leaf partition to the index
		 * in the returned PartitionedRelPruneInfo list of the info for that
		 * partition.  We use 1-based indexes here, so that zero can represent
		 * an un-filled array entry.
		 */
		Assert(rti < root->simple_rel_array_size);
		relid_subpart_map[rti] = i++;

		/*
		 * Translate pruning qual, if necessary, for this partition.
		 *
		 * The first item in the list is the target partitioned relation.
		 */
		if (!targetpart)
		{
			targetpart = subpart;

			/*
			 * The prunequal is presented to us as a qual for 'parentrel'.
			 * Frequently this rel is the same as targetpart, so we can skip
			 * an adjust_appendrel_attrs step.  But it might not be, and then
			 * we have to translate.  We update the prunequal parameter here,
			 * because in later iterations of the loop for child partitions,
			 * we want to translate from parent to child variables.
			 */
			if (!bms_equal(parentrel->relids, subpart->relids))
			{
				int			nappinfos;
				AppendRelInfo **appinfos = find_appinfos_by_relids(root,
																   subpart->relids,
																   &nappinfos);

				prunequal = (List *) adjust_appendrel_attrs(root, (Node *)
															prunequal,
															nappinfos,
															appinfos);

				pfree(appinfos);
			}

			partprunequal = prunequal;
		}
		else
		{
			/*
			 * For sub-partitioned tables the columns may not be in the same
			 * order as the parent, so we must translate the prunequal to make
			 * it compatible with this relation.
			 */
			partprunequal = (List *)
				adjust_appendrel_attrs_multilevel(root,
												  (Node *) prunequal,
												  subpart->relids,
												  targetpart->relids);
		}

		/*
		 * Convert pruning qual to pruning steps.  We may need to do this
		 * twice, once to obtain executor startup pruning steps, and once for
		 * executor per-scan pruning steps.  This first pass creates startup
		 * pruning steps and detects whether there's any possibly-useful quals
		 * that would require per-scan pruning.
		 */
		gen_partprune_steps(subpart, partprunequal, PARTTARGET_INITIAL,
							&context);

		if (context.contradictory)
		{
			/*
			 * This shouldn't happen as the planner should have detected this
			 * earlier. However, we do use additional quals from parameterized
			 * paths here. These do only compare Params to the partition key,
			 * so this shouldn't cause the discovery of any new qual
			 * contradictions that were not previously discovered as the Param
			 * values are unknown during planning.  Anyway, we'd better do
			 * something sane here, so let's just disable run-time pruning.
			 */
			return NIL;
		}

		/*
		 * If no mutable operators or expressions appear in usable pruning
		 * clauses, then there's no point in running startup pruning, because
		 * plan-time pruning should have pruned everything prunable.
		 */
		if (context.has_mutable_op || context.has_mutable_arg)
			initial_pruning_steps = context.steps;
		else
			initial_pruning_steps = NIL;

		/*
		 * If no exec Params appear in potentially-usable pruning clauses,
		 * then there's no point in even thinking about per-scan pruning.
		 */
		if (context.has_exec_param)
		{
			/* ... OK, we'd better think about it */
			gen_partprune_steps(subpart, partprunequal, PARTTARGET_EXEC,
								&context);

			if (context.contradictory)
			{
				/* As above, skip run-time pruning if anything fishy happens */
				return NIL;
			}

			exec_pruning_steps = context.steps;

			/*
			 * Detect which exec Params actually got used; the fact that some
			 * were in available clauses doesn't mean we actually used them.
			 * Skip per-scan pruning if there are none.
			 */
			execparamids = get_partkey_exec_paramids(exec_pruning_steps);

			if (bms_is_empty(execparamids))
				exec_pruning_steps = NIL;
		}
		else
		{
			/* No exec Params anywhere, so forget about scan-time pruning */
			exec_pruning_steps = NIL;
			execparamids = NULL;
		}

		if (initial_pruning_steps || exec_pruning_steps)
			doruntimeprune = true;

		/* Begin constructing the PartitionedRelPruneInfo for this rel */
		pinfo = makeNode(PartitionedRelPruneInfo);
		pinfo->rtindex = rti;
		pinfo->initial_pruning_steps = initial_pruning_steps;
		pinfo->exec_pruning_steps = exec_pruning_steps;
		pinfo->execparamids = execparamids;
		/* Remaining fields will be filled in the next loop */

		pinfolist = lappend(pinfolist, pinfo);
	}

	if (!doruntimeprune)
	{
		/* No run-time pruning required. */
		pfree(relid_subpart_map);
		return NIL;
	}

	/*
	 * Run-time pruning will be required, so initialize other information.
	 * That includes two maps -- one needed to convert partition indexes of
	 * leaf partitions to the indexes of their subplans in the subplan list,
	 * another needed to convert partition indexes of sub-partitioned
	 * partitions to the indexes of their PartitionedRelPruneInfo in the
	 * PartitionedRelPruneInfo list.
	 */
	foreach(lc, pinfolist)
	{
		PartitionedRelPruneInfo *pinfo = lfirst(lc);
		RelOptInfo *subpart = find_base_rel(root, pinfo->rtindex);
		Bitmapset  *present_parts;
		int			nparts = subpart->nparts;
		int		   *subplan_map;
		int		   *subpart_map;
		Oid		   *relid_map;

		/*
		 * Construct the subplan and subpart maps for this partitioning level.
		 * Here we convert to zero-based indexes, with -1 for empty entries.
		 * Also construct a Bitmapset of all partitions that are present (that
		 * is, not pruned already).
		 */
		subplan_map = (int *) palloc(nparts * sizeof(int));
		memset(subplan_map, -1, nparts * sizeof(int));
		subpart_map = (int *) palloc(nparts * sizeof(int));
		memset(subpart_map, -1, nparts * sizeof(int));
		relid_map = (Oid *) palloc0(nparts * sizeof(Oid));
		present_parts = NULL;

		for (i = 0; i < nparts; i++)
		{
			RelOptInfo *partrel = subpart->part_rels[i];
			int			subplanidx;
			int			subpartidx;

			/* Skip processing pruned partitions. */
			if (partrel == NULL)
				continue;

			subplan_map[i] = subplanidx = relid_subplan_map[partrel->relid] - 1;
			subpart_map[i] = subpartidx = relid_subpart_map[partrel->relid] - 1;
			relid_map[i] = planner_rt_fetch(partrel->relid, root)->relid;
			if (subplanidx >= 0)
			{
				present_parts = bms_add_member(present_parts, i);

				/* Record finding this subplan  */
				subplansfound = bms_add_member(subplansfound, subplanidx);
			}
			else if (subpartidx >= 0)
				present_parts = bms_add_member(present_parts, i);
		}

		/*
		 * Ensure there were no stray PartitionedRelPruneInfo generated for
		 * partitioned tables that we have no sub-paths or
		 * sub-PartitionedRelPruneInfo for.
		 */
		Assert(!bms_is_empty(present_parts));

		/* Record the maps and other information. */
		pinfo->present_parts = present_parts;
		pinfo->nparts = nparts;
		pinfo->subplan_map = subplan_map;
		pinfo->subpart_map = subpart_map;
		pinfo->relid_map = relid_map;
	}

	pfree(relid_subpart_map);

	*matchedsubplans = subplansfound;

	return pinfolist;
}

/*
 * gen_partprune_steps
 *		Process 'clauses' (typically a rel's baserestrictinfo list of clauses)
 *		and create a list of "partition pruning steps".
 *
 * 'target' tells whether to generate pruning steps for planning (use
 * immutable clauses only), or for executor startup (use any allowable
 * clause except ones containing PARAM_EXEC Params), or for executor
 * per-scan pruning (use any allowable clause).
 *
 * 'context' is an output argument that receives the steps list as well as
 * some subsidiary flags; see the GeneratePruningStepsContext typedef.
 */
static void
gen_partprune_steps(RelOptInfo *rel, List *clauses, PartClauseTarget target,
					GeneratePruningStepsContext *context)
{
	/* Initialize all output values to zero/false/NULL */
	memset(context, 0, sizeof(GeneratePruningStepsContext));
	context->rel = rel;
	context->target = target;

	/*
	 * If this partitioned table is in turn a partition, and it shares any
	 * partition keys with its parent, then it's possible that the hierarchy
	 * allows the parent a narrower range of values than some of its
	 * partitions (particularly the default one).  This is normally not
	 * useful, but it can be to prune the default partition.
	 */
	if (partition_bound_has_default(rel->boundinfo) && rel->partition_qual)
	{
		/* Make a copy to avoid modifying the passed-in List */
		clauses = list_concat_copy(clauses, rel->partition_qual);
	}

	/* Down into the rabbit-hole. */
	(void) gen_partprune_steps_internal(context, clauses);
}

/*
 * prune_append_rel_partitions
 *		Process rel's baserestrictinfo and make use of quals which can be
 *		evaluated during query planning in order to determine the minimum set
 *		of partitions which must be scanned to satisfy these quals.  Returns
 *		the matching partitions in the form of a Bitmapset containing the
 *		partitions' indexes in the rel's part_rels array.
 *
 * Callers must ensure that 'rel' is a partitioned table.
 */
Bitmapset *
prune_append_rel_partitions(RelOptInfo *rel)
{
	List	   *clauses = rel->baserestrictinfo;
	List	   *pruning_steps;
	GeneratePruningStepsContext gcontext;
	PartitionPruneContext context;

	Assert(rel->part_scheme != NULL);

	/* If there are no partitions, return the empty set */
	if (rel->nparts == 0)
		return NULL;

	/*
	 * If pruning is disabled or if there are no clauses to prune with, return
	 * all partitions.
	 */
	if (!enable_partition_pruning || clauses == NIL)
		return bms_add_range(NULL, 0, rel->nparts - 1);

	/*
	 * Process clauses to extract pruning steps that are usable at plan time.
	 * If the clauses are found to be contradictory, we can return the empty
	 * set.
	 */
	gen_partprune_steps(rel, clauses, PARTTARGET_PLANNER,
						&gcontext);
	if (gcontext.contradictory)
		return NULL;
	pruning_steps = gcontext.steps;

	/* If there's nothing usable, return all partitions */
	if (pruning_steps == NIL)
		return bms_add_range(NULL, 0, rel->nparts - 1);

	/* Set up PartitionPruneContext */
	context.strategy = rel->part_scheme->strategy;
	context.partnatts = rel->part_scheme->partnatts;
	context.nparts = rel->nparts;
	context.boundinfo = rel->boundinfo;
	context.partcollation = rel->part_scheme->partcollation;
	context.partsupfunc = rel->part_scheme->partsupfunc;
	context.stepcmpfuncs = (FmgrInfo *) palloc0(sizeof(FmgrInfo) *
												context.partnatts *
												list_length(pruning_steps));
	context.ppccontext = CurrentMemoryContext;

	/* These are not valid when being called from the planner */
	context.planstate = NULL;
	context.exprstates = NULL;

	/* Actual pruning happens here. */
	return get_matching_partitions(&context, pruning_steps);
}

/*
 * get_matching_partitions
 *		Determine partitions that survive partition pruning
 *
 * Note: context->planstate must be set to a valid PlanState when the
 * pruning_steps were generated with a target other than PARTTARGET_PLANNER.
 *
 * Returns a Bitmapset of the RelOptInfo->part_rels indexes of the surviving
 * partitions.
 */
Bitmapset *
get_matching_partitions(PartitionPruneContext *context, List *pruning_steps)
{
	Bitmapset  *result;
	int			num_steps = list_length(pruning_steps),
				i;
	PruneStepResult **results,
			   *final_result;
	ListCell   *lc;
	bool		scan_default;

	/* If there are no pruning steps then all partitions match. */
	if (num_steps == 0)
	{
		Assert(context->nparts > 0);
		return bms_add_range(NULL, 0, context->nparts - 1);
	}

	/*
	 * Allocate space for individual pruning steps to store its result.  Each
	 * slot will hold a PruneStepResult after performing a given pruning step.
	 * Later steps may use the result of one or more earlier steps.  The
	 * result of applying all pruning steps is the value contained in the slot
	 * of the last pruning step.
	 */
	results = (PruneStepResult **)
		palloc0(num_steps * sizeof(PruneStepResult *));
	foreach(lc, pruning_steps)
	{
		PartitionPruneStep *step = lfirst(lc);

		switch (nodeTag(step))
		{
			case T_PartitionPruneStepOp:
				results[step->step_id] =
					perform_pruning_base_step(context,
											  (PartitionPruneStepOp *) step);
				break;

			case T_PartitionPruneStepCombine:
				results[step->step_id] =
					perform_pruning_combine_step(context,
												 (PartitionPruneStepCombine *) step,
												 results);
				break;

			default:
				elog(ERROR, "invalid pruning step type: %d",
					 (int) nodeTag(step));
		}
	}

	/*
	 * At this point we know the offsets of all the datums whose corresponding
	 * partitions need to be in the result, including special null-accepting
	 * and default partitions.  Collect the actual partition indexes now.
	 */
	final_result = results[num_steps - 1];
	Assert(final_result != NULL);
	i = -1;
	result = NULL;
	scan_default = final_result->scan_default;
	while ((i = bms_next_member(final_result->bound_offsets, i)) >= 0)
	{
		int			partindex;

		Assert(i < context->boundinfo->nindexes);
		partindex = context->boundinfo->indexes[i];

		if (partindex < 0)
		{
			/*
			 * In range partitioning cases, if a partition index is -1 it
			 * means that the bound at the offset is the upper bound for a
			 * range not covered by any partition (other than a possible
			 * default partition).  In hash partitioning, the same means no
			 * partition has been defined for the corresponding remainder
			 * value.
			 *
			 * In either case, the value is still part of the queried range of
			 * values, so mark to scan the default partition if one exists.
			 */
			scan_default |= partition_bound_has_default(context->boundinfo);
			continue;
		}

		result = bms_add_member(result, partindex);
	}

	/* Add the null and/or default partition if needed and present. */
	if (final_result->scan_null)
	{
		Assert(context->strategy == PARTITION_STRATEGY_LIST);
		Assert(partition_bound_accepts_nulls(context->boundinfo));
		result = bms_add_member(result, context->boundinfo->null_index);
	}
	if (scan_default)
	{
		Assert(context->strategy == PARTITION_STRATEGY_LIST ||
			   context->strategy == PARTITION_STRATEGY_RANGE);
		Assert(partition_bound_has_default(context->boundinfo));
		result = bms_add_member(result, context->boundinfo->default_index);
	}

	return result;
}

/*
 * gen_partprune_steps_internal
 *		Processes 'clauses' to generate partition pruning steps.
 *
 * From OpExpr clauses that are mutually AND'd, we find combinations of those
 * that match to the partition key columns and for every such combination,
 * we emit a PartitionPruneStepOp containing a vector of expressions whose
 * values are used as a look up key to search partitions by comparing the
 * values with partition bounds.  Relevant details of the operator and a
 * vector of (possibly cross-type) comparison functions is also included with
 * each step.
 *
 * For BoolExpr clauses, we recursively generate steps for each argument, and
 * return a PartitionPruneStepCombine of their results.
 *
 * The return value is a list of the steps generated, which are also added to
 * the context's steps list.  Each step is assigned a step identifier, unique
 * even across recursive calls.
 *
 * If we find clauses that are mutually contradictory, or contradictory with
 * the partitioning constraint, or a pseudoconstant clause that contains
 * false, we set context->contradictory to true and return NIL (that is, no
 * pruning steps).  Caller should consider all partitions as pruned in that
 * case.
 */
static List *
gen_partprune_steps_internal(GeneratePruningStepsContext *context,
							 List *clauses)
{
	PartitionScheme part_scheme = context->rel->part_scheme;
	List	   *keyclauses[PARTITION_MAX_KEYS];
	Bitmapset  *nullkeys = NULL,
			   *notnullkeys = NULL;
	bool		generate_opsteps = false;
	List	   *result = NIL;
	ListCell   *lc;

	/*
	 * If this partitioned relation has a default partition and is itself a
	 * partition (as evidenced by partition_qual being not NIL), we first
	 * check if the clauses contradict the partition constraint.  If they do,
	 * there's no need to generate any steps as it'd already be proven that no
	 * partitions need to be scanned.
	 *
	 * This is a measure of last resort only to be used because the default
	 * partition cannot be pruned using the steps generated from clauses that
	 * contradict the parent's partition constraint; regular pruning, which is
	 * cheaper, is sufficient when no default partition exists.
	 */
	if (partition_bound_has_default(context->rel->boundinfo) &&
		predicate_refuted_by(context->rel->partition_qual, clauses, false))
	{
		context->contradictory = true;
		return NIL;
	}

	memset(keyclauses, 0, sizeof(keyclauses));
	foreach(lc, clauses)
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		int			i;

		/* Look through RestrictInfo, if any */
		if (IsA(clause, RestrictInfo))
			clause = ((RestrictInfo *) clause)->clause;

		/* Constant-false-or-null is contradictory */
		if (IsA(clause, Const) &&
			(((Const *) clause)->constisnull ||
			 !DatumGetBool(((Const *) clause)->constvalue)))
		{
			context->contradictory = true;
			return NIL;
		}

		/* Get the BoolExpr's out of the way. */
		if (IsA(clause, BoolExpr))
		{
			/*
			 * Generate steps for arguments.
			 *
			 * While steps generated for the arguments themselves will be
			 * added to context->steps during recursion and will be evaluated
			 * independently, collect their step IDs to be stored in the
			 * combine step we'll be creating.
			 */
			if (is_orclause(clause))
			{
				List	   *arg_stepids = NIL;
				bool		all_args_contradictory = true;
				ListCell   *lc1;

				/*
				 * We can share the outer context area with the recursive
				 * call, but contradictory had better not be true yet.
				 */
				Assert(!context->contradictory);

				/*
				 * Get pruning step for each arg.  If we get contradictory for
				 * all args, it means the OR expression is false as a whole.
				 */
				foreach(lc1, ((BoolExpr *) clause)->args)
				{
					Expr	   *arg = lfirst(lc1);
					bool		arg_contradictory;
					List	   *argsteps;

					argsteps = gen_partprune_steps_internal(context,
															list_make1(arg));
					arg_contradictory = context->contradictory;
					/* Keep context->contradictory clear till we're done */
					context->contradictory = false;

					if (arg_contradictory)
					{
						/* Just ignore self-contradictory arguments. */
						continue;
					}
					else
						all_args_contradictory = false;

					if (argsteps != NIL)
					{
						PartitionPruneStep *step;

						Assert(list_length(argsteps) == 1);
						step = (PartitionPruneStep *) linitial(argsteps);
						arg_stepids = lappend_int(arg_stepids, step->step_id);
					}
					else
					{
						PartitionPruneStep *orstep;

						/*
						 * The arg didn't contain a clause matching this
						 * partition key.  We cannot prune using such an arg.
						 * To indicate that to the pruning code, we must
						 * construct a dummy PartitionPruneStepCombine whose
						 * source_stepids is set to an empty List.
						 */
						orstep = gen_prune_step_combine(context, NIL,
														PARTPRUNE_COMBINE_UNION);
						arg_stepids = lappend_int(arg_stepids, orstep->step_id);
					}
				}

				/* If all the OR arms are contradictory, we can stop */
				if (all_args_contradictory)
				{
					context->contradictory = true;
					return NIL;
				}

				if (arg_stepids != NIL)
				{
					PartitionPruneStep *step;

					step = gen_prune_step_combine(context, arg_stepids,
												  PARTPRUNE_COMBINE_UNION);
					result = lappend(result, step);
				}
				continue;
			}
			else if (is_andclause(clause))
			{
				List	   *args = ((BoolExpr *) clause)->args;
				List	   *argsteps,
						   *arg_stepids = NIL;
				ListCell   *lc1;

				/*
				 * args may itself contain clauses of arbitrary type, so just
				 * recurse and later combine the component partitions sets
				 * using a combine step.
				 */
				argsteps = gen_partprune_steps_internal(context, args);

				/* If any AND arm is contradictory, we can stop immediately */
				if (context->contradictory)
					return NIL;

				foreach(lc1, argsteps)
				{
					PartitionPruneStep *step = lfirst(lc1);

					arg_stepids = lappend_int(arg_stepids, step->step_id);
				}

				if (arg_stepids != NIL)
				{
					PartitionPruneStep *step;

					step = gen_prune_step_combine(context, arg_stepids,
												  PARTPRUNE_COMBINE_INTERSECT);
					result = lappend(result, step);
				}
				continue;
			}

			/*
			 * Fall-through for a NOT clause, which if it's a Boolean clause,
			 * will be handled in match_clause_to_partition_key(). We
			 * currently don't perform any pruning for more complex NOT
			 * clauses.
			 */
		}

		/*
		 * See if we can match this clause to any of the partition keys.
		 */
		for (i = 0; i < part_scheme->partnatts; i++)
		{
			Expr	   *partkey = linitial(context->rel->partexprs[i]);
			bool		clause_is_not_null = false;
			PartClauseInfo *pc = NULL;
			List	   *clause_steps = NIL;

			switch (match_clause_to_partition_key(context,
												  clause, partkey, i,
												  &clause_is_not_null,
												  &pc, &clause_steps))
			{
				case PARTCLAUSE_MATCH_CLAUSE:
					Assert(pc != NULL);

					/*
					 * Since we only allow strict operators, check for any
					 * contradicting IS NULL.
					 */
					if (bms_is_member(i, nullkeys))
					{
						context->contradictory = true;
						return NIL;
					}
					generate_opsteps = true;
					keyclauses[i] = lappend(keyclauses[i], pc);
					break;

				case PARTCLAUSE_MATCH_NULLNESS:
					if (!clause_is_not_null)
					{
						/*
						 * check for conflicting IS NOT NULL as well as
						 * contradicting strict clauses
						 */
						if (bms_is_member(i, notnullkeys) ||
							keyclauses[i] != NIL)
						{
							context->contradictory = true;
							return NIL;
						}
						nullkeys = bms_add_member(nullkeys, i);
					}
					else
					{
						/* check for conflicting IS NULL */
						if (bms_is_member(i, nullkeys))
						{
							context->contradictory = true;
							return NIL;
						}
						notnullkeys = bms_add_member(notnullkeys, i);
					}
					break;

				case PARTCLAUSE_MATCH_STEPS:
					Assert(clause_steps != NIL);
					result = list_concat(result, clause_steps);
					break;

				case PARTCLAUSE_MATCH_CONTRADICT:
					/* We've nothing more to do if a contradiction was found. */
					context->contradictory = true;
					return NIL;

				case PARTCLAUSE_NOMATCH:

					/*
					 * Clause didn't match this key, but it might match the
					 * next one.
					 */
					continue;

				case PARTCLAUSE_UNSUPPORTED:
					/* This clause cannot be used for pruning. */
					break;
			}

			/* done; go check the next clause. */
			break;
		}
	}

	/*-----------
	 * Now generate some (more) pruning steps.  We have three strategies:
	 *
	 * 1) Generate pruning steps based on IS NULL clauses:
	 *   a) For list partitioning, null partition keys can only be found in
	 *      the designated null-accepting partition, so if there are IS NULL
	 *      clauses containing partition keys we should generate a pruning
	 *      step that gets rid of all partitions but that one.  We can
	 *      disregard any OpExpr we may have found.
	 *   b) For range partitioning, only the default partition can contain
	 *      NULL values, so the same rationale applies.
	 *   c) For hash partitioning, we only apply this strategy if we have
	 *      IS NULL clauses for all the keys.  Strategy 2 below will take
	 *      care of the case where some keys have OpExprs and others have
	 *      IS NULL clauses.
	 *
	 * 2) If not, generate steps based on OpExprs we have (if any).
	 *
	 * 3) If this doesn't work either, we may be able to generate steps to
	 *    prune just the null-accepting partition (if one exists), if we have
	 *    IS NOT NULL clauses for all partition keys.
	 */
	if (!bms_is_empty(nullkeys) &&
		(part_scheme->strategy == PARTITION_STRATEGY_LIST ||
		 part_scheme->strategy == PARTITION_STRATEGY_RANGE ||
		 (part_scheme->strategy == PARTITION_STRATEGY_HASH &&
		  bms_num_members(nullkeys) == part_scheme->partnatts)))
	{
		PartitionPruneStep *step;

		/* Strategy 1 */
		step = gen_prune_step_op(context, InvalidStrategy,
								 false, NIL, NIL, nullkeys);
		result = lappend(result, step);
	}
	else if (generate_opsteps)
	{
		PartitionPruneStep *step;

		/* Strategy 2 */
		step = gen_prune_steps_from_opexps(context, keyclauses, nullkeys);
		if (step != NULL)
			result = lappend(result, step);
	}
	else if (bms_num_members(notnullkeys) == part_scheme->partnatts)
	{
		PartitionPruneStep *step;

		/* Strategy 3 */
		step = gen_prune_step_op(context, InvalidStrategy,
								 false, NIL, NIL, NULL);
		result = lappend(result, step);
	}

	/*
	 * Finally, results from all entries appearing in result should be
	 * combined using an INTERSECT combine step, if more than one.
	 */
	if (list_length(result) > 1)
	{
		List	   *step_ids = NIL;

		foreach(lc, result)
		{
			PartitionPruneStep *step = lfirst(lc);

			step_ids = lappend_int(step_ids, step->step_id);
		}

		if (step_ids != NIL)
		{
			PartitionPruneStep *step;

			step = gen_prune_step_combine(context, step_ids,
										  PARTPRUNE_COMBINE_INTERSECT);
			result = lappend(result, step);
		}
	}

	return result;
}

/*
 * gen_prune_step_op
 *		Generate a pruning step for a specific operator
 *
 * The step is assigned a unique step identifier and added to context's 'steps'
 * list.
 */
static PartitionPruneStep *
gen_prune_step_op(GeneratePruningStepsContext *context,
				  StrategyNumber opstrategy, bool op_is_ne,
				  List *exprs, List *cmpfns,
				  Bitmapset *nullkeys)
{
	PartitionPruneStepOp *opstep = makeNode(PartitionPruneStepOp);

	opstep->step.step_id = context->next_step_id++;

	/*
	 * For clauses that contain an <> operator, set opstrategy to
	 * InvalidStrategy to signal get_matching_list_bounds to do the right
	 * thing.
	 */
	opstep->opstrategy = op_is_ne ? InvalidStrategy : opstrategy;
	Assert(list_length(exprs) == list_length(cmpfns));
	opstep->exprs = exprs;
	opstep->cmpfns = cmpfns;
	opstep->nullkeys = nullkeys;

	context->steps = lappend(context->steps, opstep);

	return (PartitionPruneStep *) opstep;
}

/*
 * gen_prune_step_combine
 *		Generate a pruning step for a combination of several other steps
 *
 * The step is assigned a unique step identifier and added to context's
 * 'steps' list.
 */
static PartitionPruneStep *
gen_prune_step_combine(GeneratePruningStepsContext *context,
					   List *source_stepids,
					   PartitionPruneCombineOp combineOp)
{
	PartitionPruneStepCombine *cstep = makeNode(PartitionPruneStepCombine);

	cstep->step.step_id = context->next_step_id++;
	cstep->combineOp = combineOp;
	cstep->source_stepids = source_stepids;

	context->steps = lappend(context->steps, cstep);

	return (PartitionPruneStep *) cstep;
}

/*
 * gen_prune_steps_from_opexps
 *		Generate pruning steps based on clauses for partition keys
 *
 * 'keyclauses' contains one list of clauses per partition key.  We check here
 * if we have found clauses for a valid subset of the partition key. In some
 * cases, (depending on the type of partitioning being used) if we didn't
 * find clauses for a given key, we discard clauses that may have been
 * found for any subsequent keys; see specific notes below.
 */
static PartitionPruneStep *
gen_prune_steps_from_opexps(GeneratePruningStepsContext *context,
							List **keyclauses, Bitmapset *nullkeys)
{
	PartitionScheme part_scheme = context->rel->part_scheme;
	List	   *opsteps = NIL;
	List	   *btree_clauses[BTMaxStrategyNumber + 1],
			   *hash_clauses[HTMaxStrategyNumber + 1];
	int			i;
	ListCell   *lc;

	memset(btree_clauses, 0, sizeof(btree_clauses));
	memset(hash_clauses, 0, sizeof(hash_clauses));
	for (i = 0; i < part_scheme->partnatts; i++)
	{
		List	   *clauselist = keyclauses[i];
		bool		consider_next_key = true;

		/*
		 * For range partitioning, if we have no clauses for the current key,
		 * we can't consider any later keys either, so we can stop here.
		 */
		if (part_scheme->strategy == PARTITION_STRATEGY_RANGE &&
			clauselist == NIL)
			break;

		/*
		 * For hash partitioning, if a column doesn't have the necessary
		 * equality clause, there should be an IS NULL clause, otherwise
		 * pruning is not possible.
		 */
		if (part_scheme->strategy == PARTITION_STRATEGY_HASH &&
			clauselist == NIL && !bms_is_member(i, nullkeys))
			return NULL;

		foreach(lc, clauselist)
		{
			PartClauseInfo *pc = (PartClauseInfo *) lfirst(lc);
			Oid			lefttype,
						righttype;

			/* Look up the operator's btree/hash strategy number. */
			if (pc->op_strategy == InvalidStrategy)
				get_op_opfamily_properties(pc->opno,
										   part_scheme->partopfamily[i],
										   false,
										   &pc->op_strategy,
										   &lefttype,
										   &righttype);

			switch (part_scheme->strategy)
			{
				case PARTITION_STRATEGY_LIST:
				case PARTITION_STRATEGY_RANGE:
					btree_clauses[pc->op_strategy] =
						lappend(btree_clauses[pc->op_strategy], pc);

					/*
					 * We can't consider subsequent partition keys if the
					 * clause for the current key contains a non-inclusive
					 * operator.
					 */
					if (pc->op_strategy == BTLessStrategyNumber ||
						pc->op_strategy == BTGreaterStrategyNumber)
						consider_next_key = false;
					break;

				case PARTITION_STRATEGY_HASH:
					if (pc->op_strategy != HTEqualStrategyNumber)
						elog(ERROR, "invalid clause for hash partitioning");
					hash_clauses[pc->op_strategy] =
						lappend(hash_clauses[pc->op_strategy], pc);
					break;

				default:
					elog(ERROR, "invalid partition strategy: %c",
						 part_scheme->strategy);
					break;
			}
		}

		/*
		 * If we've decided that clauses for subsequent partition keys
		 * wouldn't be useful for pruning, don't search any further.
		 */
		if (!consider_next_key)
			break;
	}

	/*
	 * Now, we have divided clauses according to their operator strategies.
	 * Check for each strategy if we can generate pruning step(s) by
	 * collecting a list of expressions whose values will constitute a vector
	 * that can be used as a lookup key by a partition bound searching
	 * function.
	 */
	switch (part_scheme->strategy)
	{
		case PARTITION_STRATEGY_LIST:
		case PARTITION_STRATEGY_RANGE:
			{
				List	   *eq_clauses = btree_clauses[BTEqualStrategyNumber];
				List	   *le_clauses = btree_clauses[BTLessEqualStrategyNumber];
				List	   *ge_clauses = btree_clauses[BTGreaterEqualStrategyNumber];
				int			strat;

				/*
				 * For each clause under consideration for a given strategy,
				 * we collect expressions from clauses for earlier keys, whose
				 * operator strategy is inclusive, into a list called
				 * 'prefix'. By appending the clause's own expression to the
				 * 'prefix', we'll generate one step using the so generated
				 * vector and assign the current strategy to it.  Actually,
				 * 'prefix' might contain multiple clauses for the same key,
				 * in which case, we must generate steps for various
				 * combinations of expressions of different keys, which
				 * get_steps_using_prefix takes care of for us.
				 */
				for (strat = 1; strat <= BTMaxStrategyNumber; strat++)
				{
					foreach(lc, btree_clauses[strat])
					{
						PartClauseInfo *pc = lfirst(lc);
						ListCell   *eq_start;
						ListCell   *le_start;
						ListCell   *ge_start;
						ListCell   *lc1;
						List	   *prefix = NIL;
						List	   *pc_steps;
						bool		prefix_valid = true;
						bool		pk_has_clauses;
						int			keyno;

						/*
						 * If this is a clause for the first partition key,
						 * there are no preceding expressions; generate a
						 * pruning step without a prefix.
						 *
						 * Note that we pass NULL for step_nullkeys, because
						 * we don't search list/range partition bounds where
						 * some keys are NULL.
						 */
						if (pc->keyno == 0)
						{
							Assert(pc->op_strategy == strat);
							pc_steps = get_steps_using_prefix(context, strat,
															  pc->op_is_ne,
															  pc->expr,
															  pc->cmpfn,
															  0,
															  NULL,
															  NIL);
							opsteps = list_concat(opsteps, pc_steps);
							continue;
						}

						eq_start = list_head(eq_clauses);
						le_start = list_head(le_clauses);
						ge_start = list_head(ge_clauses);

						/*
						 * We arrange clauses into prefix in ascending order
						 * of their partition key numbers.
						 */
						for (keyno = 0; keyno < pc->keyno; keyno++)
						{
							pk_has_clauses = false;

							/*
							 * Expressions from = clauses can always be in the
							 * prefix, provided they're from an earlier key.
							 */
							for_each_cell(lc1, eq_clauses, eq_start)
							{
								PartClauseInfo *eqpc = lfirst(lc1);

								if (eqpc->keyno == keyno)
								{
									prefix = lappend(prefix, eqpc);
									pk_has_clauses = true;
								}
								else
								{
									Assert(eqpc->keyno > keyno);
									break;
								}
							}
							eq_start = lc1;

							/*
							 * If we're generating steps for </<= strategy, we
							 * can add other <= clauses to the prefix,
							 * provided they're from an earlier key.
							 */
							if (strat == BTLessStrategyNumber ||
								strat == BTLessEqualStrategyNumber)
							{
								for_each_cell(lc1, le_clauses, le_start)
								{
									PartClauseInfo *lepc = lfirst(lc1);

									if (lepc->keyno == keyno)
									{
										prefix = lappend(prefix, lepc);
										pk_has_clauses = true;
									}
									else
									{
										Assert(lepc->keyno > keyno);
										break;
									}
								}
								le_start = lc1;
							}

							/*
							 * If we're generating steps for >/>= strategy, we
							 * can add other >= clauses to the prefix,
							 * provided they're from an earlier key.
							 */
							if (strat == BTGreaterStrategyNumber ||
								strat == BTGreaterEqualStrategyNumber)
							{
								for_each_cell(lc1, ge_clauses, ge_start)
								{
									PartClauseInfo *gepc = lfirst(lc1);

									if (gepc->keyno == keyno)
									{
										prefix = lappend(prefix, gepc);
										pk_has_clauses = true;
									}
									else
									{
										Assert(gepc->keyno > keyno);
										break;
									}
								}
								ge_start = lc1;
							}

							/*
							 * If this key has no clauses, prefix is not valid
							 * anymore.
							 */
							if (!pk_has_clauses)
							{
								prefix_valid = false;
								break;
							}
						}

						/*
						 * If prefix_valid, generate PartitionPruneStepOps.
						 * Otherwise, we would not find clauses for a valid
						 * subset of the partition keys anymore for the
						 * strategy; give up on generating partition pruning
						 * steps further for the strategy.
						 *
						 * As mentioned above, if 'prefix' contains multiple
						 * expressions for the same key, the following will
						 * generate multiple steps, one for each combination
						 * of the expressions for different keys.
						 *
						 * Note that we pass NULL for step_nullkeys, because
						 * we don't search list/range partition bounds where
						 * some keys are NULL.
						 */
						if (prefix_valid)
						{
							Assert(pc->op_strategy == strat);
							pc_steps = get_steps_using_prefix(context, strat,
															  pc->op_is_ne,
															  pc->expr,
															  pc->cmpfn,
															  pc->keyno,
															  NULL,
															  prefix);
							opsteps = list_concat(opsteps, pc_steps);
						}
						else
							break;
					}
				}
				break;
			}

		case PARTITION_STRATEGY_HASH:
			{
				List	   *eq_clauses = hash_clauses[HTEqualStrategyNumber];

				/* For hash partitioning, we have just the = strategy. */
				if (eq_clauses != NIL)
				{
					PartClauseInfo *pc;
					List	   *pc_steps;
					List	   *prefix = NIL;
					int			last_keyno;
					ListCell   *lc1;

					/*
					 * Locate the clause for the greatest column.  This may
					 * not belong to the last partition key, but it is the
					 * clause belonging to the last partition key we found a
					 * clause for above.
					 */
					pc = llast(eq_clauses);

					/*
					 * There might be multiple clauses which matched to that
					 * partition key; find the first such clause.  While at
					 * it, add all the clauses before that one to 'prefix'.
					 */
					last_keyno = pc->keyno;
					foreach(lc, eq_clauses)
					{
						pc = lfirst(lc);
						if (pc->keyno == last_keyno)
							break;
						prefix = lappend(prefix, pc);
					}

					/*
					 * For each clause for the "last" column, after appending
					 * the clause's own expression to the 'prefix', we'll
					 * generate one step using the so generated vector and
					 * assign = as its strategy.  Actually, 'prefix' might
					 * contain multiple clauses for the same key, in which
					 * case, we must generate steps for various combinations
					 * of expressions of different keys, which
					 * get_steps_using_prefix will take care of for us.
					 */
					for_each_cell(lc1, eq_clauses, lc)
					{
						pc = lfirst(lc1);

						/*
						 * Note that we pass nullkeys for step_nullkeys,
						 * because we need to tell hash partition bound search
						 * function which of the keys we found IS NULL clauses
						 * for.
						 */
						Assert(pc->op_strategy == HTEqualStrategyNumber);
						pc_steps =
							get_steps_using_prefix(context,
												   HTEqualStrategyNumber,
												   false,
												   pc->expr,
												   pc->cmpfn,
												   pc->keyno,
												   nullkeys,
												   prefix);
						opsteps = list_concat(opsteps, pc_steps);
					}
				}
				break;
			}

		default:
			elog(ERROR, "invalid partition strategy: %c",
				 part_scheme->strategy);
			break;
	}

	/* Lastly, add a combine step to mutually AND these op steps, if needed */
	if (list_length(opsteps) > 1)
	{
		List	   *opstep_ids = NIL;

		foreach(lc, opsteps)
		{
			PartitionPruneStep *step = lfirst(lc);

			opstep_ids = lappend_int(opstep_ids, step->step_id);
		}

		if (opstep_ids != NIL)
			return gen_prune_step_combine(context, opstep_ids,
										  PARTPRUNE_COMBINE_INTERSECT);
		return NULL;
	}
	else if (opsteps != NIL)
		return linitial(opsteps);

	return NULL;
}

/*
 * If the partition key has a collation, then the clause must have the same
 * input collation.  If the partition key is non-collatable, we assume the
 * collation doesn't matter, because while collation wasn't considered when
 * performing partitioning, the clause still may have a collation assigned
 * due to the other input being of a collatable type.
 *
 * See also IndexCollMatchesExprColl.
 */
#define PartCollMatchesExprColl(partcoll, exprcoll) \
	((partcoll) == InvalidOid || (partcoll) == (exprcoll))

/*
 * match_clause_to_partition_key
 *		Attempt to match the given 'clause' with the specified partition key.
 *
 * Return value is:
 * * PARTCLAUSE_NOMATCH if the clause doesn't match this partition key (but
 *   caller should keep trying, because it might match a subsequent key).
 *   Output arguments: none set.
 *
 * * PARTCLAUSE_MATCH_CLAUSE if there is a match.
 *   Output arguments: *pc is set to a PartClauseInfo constructed for the
 *   matched clause.
 *
 * * PARTCLAUSE_MATCH_NULLNESS if there is a match, and the matched clause was
 *   either a "a IS NULL" or "a IS NOT NULL" clause.
 *   Output arguments: *clause_is_not_null is set to false in the former case
 *   true otherwise.
 *
 * * PARTCLAUSE_MATCH_STEPS if there is a match.
 *   Output arguments: *clause_steps is set to a list of PartitionPruneStep
 *   generated for the clause.
 *
 * * PARTCLAUSE_MATCH_CONTRADICT if the clause is self-contradictory, ie
 *   it provably returns FALSE or NULL.
 *   Output arguments: none set.
 *
 * * PARTCLAUSE_UNSUPPORTED if the clause doesn't match this partition key
 *   and couldn't possibly match any other one either, due to its form or
 *   properties (such as containing a volatile function).
 *   Output arguments: none set.
 */
static PartClauseMatchStatus
match_clause_to_partition_key(GeneratePruningStepsContext *context,
							  Expr *clause, Expr *partkey, int partkeyidx,
							  bool *clause_is_not_null, PartClauseInfo **pc,
							  List **clause_steps)
{
	PartClauseMatchStatus boolmatchstatus;
	PartitionScheme part_scheme = context->rel->part_scheme;
	Oid			partopfamily = part_scheme->partopfamily[partkeyidx],
				partcoll = part_scheme->partcollation[partkeyidx];
	Expr	   *expr;

	/*
	 * Recognize specially shaped clauses that match a Boolean partition key.
	 */
	boolmatchstatus = match_boolean_partition_clause(partopfamily, clause,
													 partkey, &expr);

	if (boolmatchstatus == PARTCLAUSE_MATCH_CLAUSE)
	{
		PartClauseInfo *partclause;

		partclause = (PartClauseInfo *) palloc(sizeof(PartClauseInfo));
		partclause->keyno = partkeyidx;
		/* Do pruning with the Boolean equality operator. */
		partclause->opno = BooleanEqualOperator;
		partclause->op_is_ne = false;
		partclause->expr = expr;
		/* We know that expr is of Boolean type. */
		partclause->cmpfn = part_scheme->partsupfunc[partkeyidx].fn_oid;
		partclause->op_strategy = InvalidStrategy;

		*pc = partclause;

		return PARTCLAUSE_MATCH_CLAUSE;
	}
	else if (IsA(clause, OpExpr) &&
			 list_length(((OpExpr *) clause)->args) == 2)
	{
		OpExpr	   *opclause = (OpExpr *) clause;
		Expr	   *leftop,
				   *rightop;
		Oid			opno,
					op_lefttype,
					op_righttype,
					negator = InvalidOid;
		Oid			cmpfn;
		int			op_strategy;
		bool		is_opne_listp = false;
		PartClauseInfo *partclause;

		leftop = (Expr *) get_leftop(clause);
		if (IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;
		rightop = (Expr *) get_rightop(clause);
		if (IsA(rightop, RelabelType))
			rightop = ((RelabelType *) rightop)->arg;
		opno = opclause->opno;

		/* check if the clause matches this partition key */
		if (equal(leftop, partkey))
			expr = rightop;
		else if (equal(rightop, partkey))
		{
			/*
			 * It's only useful if we can commute the operator to put the
			 * partkey on the left.  If we can't, the clause can be deemed
			 * UNSUPPORTED.  Even if its leftop matches some later partkey, we
			 * now know it has Vars on the right, so it's no use.
			 */
			opno = get_commutator(opno);
			if (!OidIsValid(opno))
				return PARTCLAUSE_UNSUPPORTED;
			expr = leftop;
		}
		else
			/* clause does not match this partition key, but perhaps next. */
			return PARTCLAUSE_NOMATCH;

		/*
		 * Partition key match also requires collation match.  There may be
		 * multiple partkeys with the same expression but different
		 * collations, so failure is NOMATCH.
		 */
		if (!PartCollMatchesExprColl(partcoll, opclause->inputcollid))
			return PARTCLAUSE_NOMATCH;

		/*
		 * See if the operator is relevant to the partitioning opfamily.
		 *
		 * Normally we only care about operators that are listed as being part
		 * of the partitioning operator family.  But there is one exception:
		 * the not-equals operators are not listed in any operator family
		 * whatsoever, but their negators (equality) are.  We can use one of
		 * those if we find it, but only for list partitioning.
		 *
		 * Note: we report NOMATCH on failure, in case a later partkey has the
		 * same expression but different opfamily.  That's unlikely, but not
		 * much more so than duplicate expressions with different collations.
		 */
		if (op_in_opfamily(opno, partopfamily))
		{
			get_op_opfamily_properties(opno, partopfamily, false,
									   &op_strategy, &op_lefttype,
									   &op_righttype);
		}
		else
		{
			if (part_scheme->strategy != PARTITION_STRATEGY_LIST)
				return PARTCLAUSE_NOMATCH;

			/* See if the negator is equality */
			negator = get_negator(opno);
			if (OidIsValid(negator) && op_in_opfamily(negator, partopfamily))
			{
				get_op_opfamily_properties(negator, partopfamily, false,
										   &op_strategy, &op_lefttype,
										   &op_righttype);
				if (op_strategy == BTEqualStrategyNumber)
					is_opne_listp = true;	/* bingo */
			}

			/* Nope, it's not <> either. */
			if (!is_opne_listp)
				return PARTCLAUSE_NOMATCH;
		}

		/*
		 * Only allow strict operators.  This will guarantee nulls are
		 * filtered.  (This test is likely useless, since btree and hash
		 * comparison operators are generally strict.)
		 */
		if (!op_strict(opno))
			return PARTCLAUSE_UNSUPPORTED;

		/*
		 * OK, we have a match to the partition key and a suitable operator.
		 * Examine the other argument to see if it's usable for pruning.
		 *
		 * In most of these cases, we can return UNSUPPORTED because the same
		 * failure would occur no matter which partkey it's matched to.  (In
		 * particular, now that we've successfully matched one side of the
		 * opclause to a partkey, there is no chance that matching the other
		 * side to another partkey will produce a usable result, since that'd
		 * mean there are Vars on both sides.)
		 *
		 * Also, if we reject an argument for a target-dependent reason, set
		 * appropriate fields of *context to report that.  We postpone these
		 * tests until after matching the partkey and the operator, so as to
		 * reduce the odds of setting the context fields for clauses that do
		 * not end up contributing to pruning steps.
		 *
		 * First, check for non-Const argument.  (We assume that any immutable
		 * subexpression will have been folded to a Const already.)
		 */
		if (!IsA(expr, Const))
		{
			Bitmapset  *paramids;

			/*
			 * When pruning in the planner, we only support pruning using
			 * comparisons to constants.  We cannot prune on the basis of
			 * anything that's not immutable.  (Note that has_mutable_arg and
			 * has_exec_param do not get set for this target value.)
			 */
			if (context->target == PARTTARGET_PLANNER)
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * We can never prune using an expression that contains Vars.
			 */
			if (contain_var_clause((Node *) expr))
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * And we must reject anything containing a volatile function.
			 * Stable functions are OK though.
			 */
			if (contain_volatile_functions((Node *) expr))
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * See if there are any exec Params.  If so, we can only use this
			 * expression during per-scan pruning.
			 */
			paramids = pull_exec_paramids(expr);
			if (!bms_is_empty(paramids))
			{
				context->has_exec_param = true;
				if (context->target != PARTTARGET_EXEC)
					return PARTCLAUSE_UNSUPPORTED;
			}
			else
			{
				/* It's potentially usable, but mutable */
				context->has_mutable_arg = true;
			}
		}

		/*
		 * Check whether the comparison operator itself is immutable.  (We
		 * assume anything that's in a btree or hash opclass is at least
		 * stable, but we need to check for immutability.)
		 */
		if (op_volatile(opno) != PROVOLATILE_IMMUTABLE)
		{
			context->has_mutable_op = true;

			/*
			 * When pruning in the planner, we cannot prune with mutable
			 * operators.
			 */
			if (context->target == PARTTARGET_PLANNER)
				return PARTCLAUSE_UNSUPPORTED;
		}

		/*
		 * Now find the procedure to use, based on the types.  If the clause's
		 * other argument is of the same type as the partitioning opclass's
		 * declared input type, we can use the procedure cached in
		 * PartitionKey.  If not, search for a cross-type one in the same
		 * opfamily; if one doesn't exist, report no match.
		 */
		if (op_righttype == part_scheme->partopcintype[partkeyidx])
			cmpfn = part_scheme->partsupfunc[partkeyidx].fn_oid;
		else
		{
			switch (part_scheme->strategy)
			{
					/*
					 * For range and list partitioning, we need the ordering
					 * procedure with lefttype being the partition key's type,
					 * and righttype the clause's operator's right type.
					 */
				case PARTITION_STRATEGY_LIST:
				case PARTITION_STRATEGY_RANGE:
					cmpfn =
						get_opfamily_proc(part_scheme->partopfamily[partkeyidx],
										  part_scheme->partopcintype[partkeyidx],
										  op_righttype, BTORDER_PROC);
					break;

					/*
					 * For hash partitioning, we need the hashing procedure
					 * for the clause's type.
					 */
				case PARTITION_STRATEGY_HASH:
					cmpfn =
						get_opfamily_proc(part_scheme->partopfamily[partkeyidx],
										  op_righttype, op_righttype,
										  HASHEXTENDED_PROC);
					break;

				default:
					elog(ERROR, "invalid partition strategy: %c",
						 part_scheme->strategy);
					cmpfn = InvalidOid; /* keep compiler quiet */
					break;
			}

			if (!OidIsValid(cmpfn))
				return PARTCLAUSE_NOMATCH;
		}

		/*
		 * Build the clause, passing the negator if applicable.
		 */
		partclause = (PartClauseInfo *) palloc(sizeof(PartClauseInfo));
		partclause->keyno = partkeyidx;
		if (is_opne_listp)
		{
			Assert(OidIsValid(negator));
			partclause->opno = negator;
			partclause->op_is_ne = true;
			partclause->op_strategy = InvalidStrategy;
		}
		else
		{
			partclause->opno = opno;
			partclause->op_is_ne = false;
			partclause->op_strategy = op_strategy;
		}
		partclause->expr = expr;
		partclause->cmpfn = cmpfn;

		*pc = partclause;

		return PARTCLAUSE_MATCH_CLAUSE;
	}
	else if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Oid			saop_op = saop->opno;
		Oid			saop_coll = saop->inputcollid;
		Expr	   *leftop = (Expr *) linitial(saop->args),
				   *rightop = (Expr *) lsecond(saop->args);
		List	   *elem_exprs,
				   *elem_clauses;
		ListCell   *lc1;

		if (IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;

		/* check if the LHS matches this partition key */
		if (!equal(leftop, partkey) ||
			!PartCollMatchesExprColl(partcoll, saop->inputcollid))
			return PARTCLAUSE_NOMATCH;

		/*
		 * See if the operator is relevant to the partitioning opfamily.
		 *
		 * In case of NOT IN (..), we get a '<>', which we handle if list
		 * partitioning is in use and we're able to confirm that it's negator
		 * is a btree equality operator belonging to the partitioning operator
		 * family.  As above, report NOMATCH for non-matching operator.
		 */
		if (!op_in_opfamily(saop_op, partopfamily))
		{
			Oid			negator;

			if (part_scheme->strategy != PARTITION_STRATEGY_LIST)
				return PARTCLAUSE_NOMATCH;

			negator = get_negator(saop_op);
			if (OidIsValid(negator) && op_in_opfamily(negator, partopfamily))
			{
				int			strategy;
				Oid			lefttype,
							righttype;

				get_op_opfamily_properties(negator, partopfamily,
										   false, &strategy,
										   &lefttype, &righttype);
				if (strategy != BTEqualStrategyNumber)
					return PARTCLAUSE_NOMATCH;
			}
			else
				return PARTCLAUSE_NOMATCH;	/* no useful negator */
		}

		/*
		 * Only allow strict operators.  This will guarantee nulls are
		 * filtered.  (This test is likely useless, since btree and hash
		 * comparison operators are generally strict.)
		 */
		if (!op_strict(saop_op))
			return PARTCLAUSE_UNSUPPORTED;

		/*
		 * OK, we have a match to the partition key and a suitable operator.
		 * Examine the array argument to see if it's usable for pruning.  This
		 * is identical to the logic for a plain OpExpr.
		 */
		if (!IsA(rightop, Const))
		{
			Bitmapset  *paramids;

			/*
			 * When pruning in the planner, we only support pruning using
			 * comparisons to constants.  We cannot prune on the basis of
			 * anything that's not immutable.  (Note that has_mutable_arg and
			 * has_exec_param do not get set for this target value.)
			 */
			if (context->target == PARTTARGET_PLANNER)
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * We can never prune using an expression that contains Vars.
			 */
			if (contain_var_clause((Node *) rightop))
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * And we must reject anything containing a volatile function.
			 * Stable functions are OK though.
			 */
			if (contain_volatile_functions((Node *) rightop))
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * See if there are any exec Params.  If so, we can only use this
			 * expression during per-scan pruning.
			 */
			paramids = pull_exec_paramids(rightop);
			if (!bms_is_empty(paramids))
			{
				context->has_exec_param = true;
				if (context->target != PARTTARGET_EXEC)
					return PARTCLAUSE_UNSUPPORTED;
			}
			else
			{
				/* It's potentially usable, but mutable */
				context->has_mutable_arg = true;
			}
		}

		/*
		 * Check whether the comparison operator itself is immutable.  (We
		 * assume anything that's in a btree or hash opclass is at least
		 * stable, but we need to check for immutability.)
		 */
		if (op_volatile(saop_op) != PROVOLATILE_IMMUTABLE)
		{
			context->has_mutable_op = true;

			/*
			 * When pruning in the planner, we cannot prune with mutable
			 * operators.
			 */
			if (context->target == PARTTARGET_PLANNER)
				return PARTCLAUSE_UNSUPPORTED;
		}

		/*
		 * Examine the contents of the array argument.
		 */
		elem_exprs = NIL;
		if (IsA(rightop, Const))
		{
			/*
			 * For a constant array, convert the elements to a list of Const
			 * nodes, one for each array element (excepting nulls).
			 */
			Const	   *arr = (Const *) rightop;
			ArrayType  *arrval;
			int16		elemlen;
			bool		elembyval;
			char		elemalign;
			Datum	   *elem_values;
			bool	   *elem_nulls;
			int			num_elems,
						i;

			/* If the array itself is null, the saop returns null */
			if (arr->constisnull)
				return PARTCLAUSE_MATCH_CONTRADICT;

			arrval = DatumGetArrayTypeP(arr->constvalue);
			get_typlenbyvalalign(ARR_ELEMTYPE(arrval),
								 &elemlen, &elembyval, &elemalign);
			deconstruct_array(arrval,
							  ARR_ELEMTYPE(arrval),
							  elemlen, elembyval, elemalign,
							  &elem_values, &elem_nulls,
							  &num_elems);
			for (i = 0; i < num_elems; i++)
			{
				Const	   *elem_expr;

				/*
				 * A null array element must lead to a null comparison result,
				 * since saop_op is known strict.  We can ignore it in the
				 * useOr case, but otherwise it implies self-contradiction.
				 */
				if (elem_nulls[i])
				{
					if (saop->useOr)
						continue;
					return PARTCLAUSE_MATCH_CONTRADICT;
				}

				elem_expr = makeConst(ARR_ELEMTYPE(arrval), -1,
									  arr->constcollid, elemlen,
									  elem_values[i], false, elembyval);
				elem_exprs = lappend(elem_exprs, elem_expr);
			}
		}
		else if (IsA(rightop, ArrayExpr))
		{
			ArrayExpr  *arrexpr = castNode(ArrayExpr, rightop);

			/*
			 * For a nested ArrayExpr, we don't know how to get the actual
			 * scalar values out into a flat list, so we give up doing
			 * anything with this ScalarArrayOpExpr.
			 */
			if (arrexpr->multidims)
				return PARTCLAUSE_UNSUPPORTED;

			/*
			 * Otherwise, we can just use the list of element values.
			 */
			elem_exprs = arrexpr->elements;
		}
		else
		{
			/* Give up on any other clause types. */
			return PARTCLAUSE_UNSUPPORTED;
		}

		/*
		 * Now generate a list of clauses, one for each array element, of the
		 * form leftop saop_op elem_expr
		 */
		elem_clauses = NIL;
		foreach(lc1, elem_exprs)
		{
			Expr	   *rightop = (Expr *) lfirst(lc1),
					   *elem_clause;

			elem_clause = make_opclause(saop_op, BOOLOID, false,
										leftop, rightop,
										InvalidOid, saop_coll);
			elem_clauses = lappend(elem_clauses, elem_clause);
		}

		/*
		 * If we have an ANY clause and multiple elements, now turn the list
		 * of clauses into an OR expression.
		 */
		if (saop->useOr && list_length(elem_clauses) > 1)
			elem_clauses = list_make1(makeBoolExpr(OR_EXPR, elem_clauses, -1));

		/* Finally, generate steps */
		*clause_steps = gen_partprune_steps_internal(context, elem_clauses);
		if (context->contradictory)
			return PARTCLAUSE_MATCH_CONTRADICT;
		else if (*clause_steps == NIL)
			return PARTCLAUSE_UNSUPPORTED;	/* step generation failed */
		return PARTCLAUSE_MATCH_STEPS;
	}
	else if (IsA(clause, NullTest))
	{
		NullTest   *nulltest = (NullTest *) clause;
		Expr	   *arg = nulltest->arg;

		if (IsA(arg, RelabelType))
			arg = ((RelabelType *) arg)->arg;

		/* Does arg match with this partition key column? */
		if (!equal(arg, partkey))
			return PARTCLAUSE_NOMATCH;

		*clause_is_not_null = (nulltest->nulltesttype == IS_NOT_NULL);

		return PARTCLAUSE_MATCH_NULLNESS;
	}

	/*
	 * If we get here then the return value depends on the result of the
	 * match_boolean_partition_clause call above.  If the call returned
	 * PARTCLAUSE_UNSUPPORTED then we're either not dealing with a bool qual
	 * or the bool qual is not suitable for pruning.  Since the qual didn't
	 * match up to any of the other qual types supported here, then trying to
	 * match it against any other partition key is a waste of time, so just
	 * return PARTCLAUSE_UNSUPPORTED.  If the qual just couldn't be matched to
	 * this partition key, then it may match another, so return
	 * PARTCLAUSE_NOMATCH.  The only other value that
	 * match_boolean_partition_clause can return is PARTCLAUSE_MATCH_CLAUSE,
	 * and since that value was already dealt with above, then we can just
	 * return boolmatchstatus.
	 */
	return boolmatchstatus;
}

/*
 * get_steps_using_prefix
 *		Generate list of PartitionPruneStepOp steps each consisting of given
 *		opstrategy
 *
 * To generate steps, step_lastexpr and step_lastcmpfn are appended to
 * expressions and cmpfns, respectively, extracted from the clauses in
 * 'prefix'.  Actually, since 'prefix' may contain multiple clauses for the
 * same partition key column, we must generate steps for various combinations
 * of the clauses of different keys.
 *
 * For list/range partitioning, callers must ensure that step_nullkeys is
 * NULL, and that prefix contains at least one clause for each of the
 * partition keys earlier than one specified in step_lastkeyno if it's
 * greater than zero.  For hash partitioning, step_nullkeys is allowed to be
 * non-NULL, but they must ensure that prefix contains at least one clause
 * for each of the partition keys other than those specified in step_nullkeys
 * and step_lastkeyno.
 *
 * For both cases, callers must also ensure that clauses in prefix are sorted
 * in ascending order of their partition key numbers.
 */
static List *
get_steps_using_prefix(GeneratePruningStepsContext *context,
					   StrategyNumber step_opstrategy,
					   bool step_op_is_ne,
					   Expr *step_lastexpr,
					   Oid step_lastcmpfn,
					   int step_lastkeyno,
					   Bitmapset *step_nullkeys,
					   List *prefix)
{
	Assert(step_nullkeys == NULL ||
		   context->rel->part_scheme->strategy == PARTITION_STRATEGY_HASH);

	/* Quick exit if there are no values to prefix with. */
	if (list_length(prefix) == 0)
	{
		PartitionPruneStep *step;

		step = gen_prune_step_op(context,
								 step_opstrategy,
								 step_op_is_ne,
								 list_make1(step_lastexpr),
								 list_make1_oid(step_lastcmpfn),
								 step_nullkeys);
		return list_make1(step);
	}

	/* Recurse to generate steps for various combinations. */
	return get_steps_using_prefix_recurse(context,
										  step_opstrategy,
										  step_op_is_ne,
										  step_lastexpr,
										  step_lastcmpfn,
										  step_lastkeyno,
										  step_nullkeys,
										  prefix,
										  list_head(prefix),
										  NIL, NIL);
}

/*
 * get_steps_using_prefix_recurse
 *		Recursively generate combinations of clauses for different partition
 *		keys and start generating steps upon reaching clauses for the greatest
 *		column that is less than the one for which we're currently generating
 *		steps (that is, step_lastkeyno)
 *
 * 'prefix' is the list of PartClauseInfos.
 * 'start' is where we should start iterating for the current invocation.
 * 'step_exprs' and 'step_cmpfns' each contains the expressions and cmpfns
 * we've generated so far from the clauses for the previous part keys.
 */
static List *
get_steps_using_prefix_recurse(GeneratePruningStepsContext *context,
							   StrategyNumber step_opstrategy,
							   bool step_op_is_ne,
							   Expr *step_lastexpr,
							   Oid step_lastcmpfn,
							   int step_lastkeyno,
							   Bitmapset *step_nullkeys,
							   List *prefix,
							   ListCell *start,
							   List *step_exprs,
							   List *step_cmpfns)
{
	List	   *result = NIL;
	ListCell   *lc;
	int			cur_keyno;

	/* Actually, recursion would be limited by PARTITION_MAX_KEYS. */
	check_stack_depth();

	/* Check if we need to recurse. */
	Assert(start != NULL);
	cur_keyno = ((PartClauseInfo *) lfirst(start))->keyno;
	if (cur_keyno < step_lastkeyno - 1)
	{
		PartClauseInfo *pc;
		ListCell   *next_start;

		/*
		 * For each clause with cur_keyno, add its expr and cmpfn to
		 * step_exprs and step_cmpfns, respectively, and recurse after setting
		 * next_start to the ListCell of the first clause for the next
		 * partition key.
		 */
		for_each_cell(lc, prefix, start)
		{
			pc = lfirst(lc);

			if (pc->keyno > cur_keyno)
				break;
		}
		next_start = lc;

		for_each_cell(lc, prefix, start)
		{
			List	   *moresteps;
			List	   *step_exprs1,
					   *step_cmpfns1;

			pc = lfirst(lc);
			if (pc->keyno == cur_keyno)
			{
				/* Leave the original step_exprs unmodified. */
				step_exprs1 = list_copy(step_exprs);
				step_exprs1 = lappend(step_exprs1, pc->expr);

				/* Leave the original step_cmpfns unmodified. */
				step_cmpfns1 = list_copy(step_cmpfns);
				step_cmpfns1 = lappend_oid(step_cmpfns1, pc->cmpfn);
			}
			else
			{
				Assert(pc->keyno > cur_keyno);
				break;
			}

			moresteps = get_steps_using_prefix_recurse(context,
													   step_opstrategy,
													   step_op_is_ne,
													   step_lastexpr,
													   step_lastcmpfn,
													   step_lastkeyno,
													   step_nullkeys,
													   prefix,
													   next_start,
													   step_exprs1,
													   step_cmpfns1);
			result = list_concat(result, moresteps);

			list_free(step_exprs1);
			list_free(step_cmpfns1);
		}
	}
	else
	{
		/*
		 * End the current recursion cycle and start generating steps, one for
		 * each clause with cur_keyno, which is all clauses from here onward
		 * till the end of the list.  Note that for hash partitioning,
		 * step_nullkeys is allowed to be non-empty, in which case step_exprs
		 * would only contain expressions for the earlier partition keys that
		 * are not specified in step_nullkeys.
		 */
		Assert(list_length(step_exprs) == cur_keyno ||
			   !bms_is_empty(step_nullkeys));

		/*
		 * Note also that for hash partitioning, each partition key should
		 * have either equality clauses or an IS NULL clause, so if a
		 * partition key doesn't have an expression, it would be specified in
		 * step_nullkeys.
		 */
		Assert(context->rel->part_scheme->strategy
			   != PARTITION_STRATEGY_HASH ||
			   list_length(step_exprs) + 2 + bms_num_members(step_nullkeys) ==
			   context->rel->part_scheme->partnatts);
		for_each_cell(lc, prefix, start)
		{
			PartClauseInfo *pc = lfirst(lc);
			PartitionPruneStep *step;
			List	   *step_exprs1,
					   *step_cmpfns1;

			Assert(pc->keyno == cur_keyno);

			/* Leave the original step_exprs unmodified. */
			step_exprs1 = list_copy(step_exprs);
			step_exprs1 = lappend(step_exprs1, pc->expr);
			step_exprs1 = lappend(step_exprs1, step_lastexpr);

			/* Leave the original step_cmpfns unmodified. */
			step_cmpfns1 = list_copy(step_cmpfns);
			step_cmpfns1 = lappend_oid(step_cmpfns1, pc->cmpfn);
			step_cmpfns1 = lappend_oid(step_cmpfns1, step_lastcmpfn);

			step = gen_prune_step_op(context,
									 step_opstrategy, step_op_is_ne,
									 step_exprs1, step_cmpfns1,
									 step_nullkeys);
			result = lappend(result, step);
		}
	}

	return result;
}

/*
 * get_matching_hash_bounds
 *		Determine offset of the hash bound matching the specified values,
 *		considering that all the non-null values come from clauses containing
 *		a compatible hash equality operator and any keys that are null come
 *		from an IS NULL clause.
 *
 * Generally this function will return a single matching bound offset,
 * although if a partition has not been setup for a given modulus then we may
 * return no matches.  If the number of clauses found don't cover the entire
 * partition key, then we'll need to return all offsets.
 *
 * 'opstrategy' if non-zero must be HTEqualStrategyNumber.
 *
 * 'values' contains Datums indexed by the partition key to use for pruning.
 *
 * 'nvalues', the number of Datums in the 'values' array.
 *
 * 'partsupfunc' contains partition hashing functions that can produce correct
 * hash for the type of the values contained in 'values'.
 *
 * 'nullkeys' is the set of partition keys that are null.
 */
static PruneStepResult *
get_matching_hash_bounds(PartitionPruneContext *context,
						 StrategyNumber opstrategy, Datum *values, int nvalues,
						 FmgrInfo *partsupfunc, Bitmapset *nullkeys)
{
	PruneStepResult *result = (PruneStepResult *) palloc0(sizeof(PruneStepResult));
	PartitionBoundInfo boundinfo = context->boundinfo;
	int		   *partindices = boundinfo->indexes;
	int			partnatts = context->partnatts;
	bool		isnull[PARTITION_MAX_KEYS];
	int			i;
	uint64		rowHash;
	int			greatest_modulus;
	Oid		   *partcollation = context->partcollation;

	Assert(context->strategy == PARTITION_STRATEGY_HASH);

	/*
	 * For hash partitioning we can only perform pruning based on equality
	 * clauses to the partition key or IS NULL clauses.  We also can only
	 * prune if we got values for all keys.
	 */
	if (nvalues + bms_num_members(nullkeys) == partnatts)
	{
		/*
		 * If there are any values, they must have come from clauses
		 * containing an equality operator compatible with hash partitioning.
		 */
		Assert(opstrategy == HTEqualStrategyNumber || nvalues == 0);

		for (i = 0; i < partnatts; i++)
			isnull[i] = bms_is_member(i, nullkeys);

		rowHash = compute_partition_hash_value(partnatts, partsupfunc, partcollation,
											   values, isnull);

		greatest_modulus = boundinfo->nindexes;
		if (partindices[rowHash % greatest_modulus] >= 0)
			result->bound_offsets =
				bms_make_singleton(rowHash % greatest_modulus);
	}
	else
	{
		/* Report all valid offsets into the boundinfo->indexes array. */
		result->bound_offsets = bms_add_range(NULL, 0,
											  boundinfo->nindexes - 1);
	}

	/*
	 * There is neither a special hash null partition or the default hash
	 * partition.
	 */
	result->scan_null = result->scan_default = false;

	return result;
}

/*
 * get_matching_list_bounds
 *		Determine the offsets of list bounds matching the specified value,
 *		according to the semantics of the given operator strategy
 *
 * scan_default will be set in the returned struct, if the default partition
 * needs to be scanned, provided one exists at all.  scan_null will be set if
 * the special null-accepting partition needs to be scanned.
 *
 * 'opstrategy' if non-zero must be a btree strategy number.
 *
 * 'value' contains the value to use for pruning.
 *
 * 'nvalues', if non-zero, should be exactly 1, because of list partitioning.
 *
 * 'partsupfunc' contains the list partitioning comparison function to be used
 * to perform partition_list_bsearch
 *
 * 'nullkeys' is the set of partition keys that are null.
 */
static PruneStepResult *
get_matching_list_bounds(PartitionPruneContext *context,
						 StrategyNumber opstrategy, Datum value, int nvalues,
						 FmgrInfo *partsupfunc, Bitmapset *nullkeys)
{
	PruneStepResult *result = (PruneStepResult *) palloc0(sizeof(PruneStepResult));
	PartitionBoundInfo boundinfo = context->boundinfo;
	int			off,
				minoff,
				maxoff;
	bool		is_equal;
	bool		inclusive = false;
	Oid		   *partcollation = context->partcollation;

	Assert(context->strategy == PARTITION_STRATEGY_LIST);
	Assert(context->partnatts == 1);

	result->scan_null = result->scan_default = false;

	if (!bms_is_empty(nullkeys))
	{
		/*
		 * Nulls may exist in only one partition - the partition whose
		 * accepted set of values includes null or the default partition if
		 * the former doesn't exist.
		 */
		if (partition_bound_accepts_nulls(boundinfo))
			result->scan_null = true;
		else
			result->scan_default = partition_bound_has_default(boundinfo);
		return result;
	}

	/*
	 * If there are no datums to compare keys with, but there are partitions,
	 * just return the default partition if one exists.
	 */
	if (boundinfo->ndatums == 0)
	{
		result->scan_default = partition_bound_has_default(boundinfo);
		return result;
	}

	minoff = 0;
	maxoff = boundinfo->ndatums - 1;

	/*
	 * If there are no values to compare with the datums in boundinfo, it
	 * means the caller asked for partitions for all non-null datums.  Add
	 * indexes of *all* partitions, including the default if any.
	 */
	if (nvalues == 0)
	{
		Assert(boundinfo->ndatums > 0);
		result->bound_offsets = bms_add_range(NULL, 0,
											  boundinfo->ndatums - 1);
		result->scan_default = partition_bound_has_default(boundinfo);
		return result;
	}

	/* Special case handling of values coming from a <> operator clause. */
	if (opstrategy == InvalidStrategy)
	{
		/*
		 * First match to all bounds.  We'll remove any matching datums below.
		 */
		Assert(boundinfo->ndatums > 0);
		result->bound_offsets = bms_add_range(NULL, 0,
											  boundinfo->ndatums - 1);

		off = partition_list_bsearch(partsupfunc, partcollation, boundinfo,
									 value, &is_equal);
		if (off >= 0 && is_equal)
		{

			/* We have a match. Remove from the result. */
			Assert(boundinfo->indexes[off] >= 0);
			result->bound_offsets = bms_del_member(result->bound_offsets,
												   off);
		}

		/* Always include the default partition if any. */
		result->scan_default = partition_bound_has_default(boundinfo);

		return result;
	}

	/*
	 * With range queries, always include the default list partition, because
	 * list partitions divide the key space in a discontinuous manner, not all
	 * values in the given range will have a partition assigned.  This may not
	 * technically be true for some data types (e.g. integer types), however,
	 * we currently lack any sort of infrastructure to provide us with proofs
	 * that would allow us to do anything smarter here.
	 */
	if (opstrategy != BTEqualStrategyNumber)
		result->scan_default = partition_bound_has_default(boundinfo);

	switch (opstrategy)
	{
		case BTEqualStrategyNumber:
			off = partition_list_bsearch(partsupfunc,
										 partcollation,
										 boundinfo, value,
										 &is_equal);
			if (off >= 0 && is_equal)
			{
				Assert(boundinfo->indexes[off] >= 0);
				result->bound_offsets = bms_make_singleton(off);
			}
			else
				result->scan_default = partition_bound_has_default(boundinfo);
			return result;

		case BTGreaterEqualStrategyNumber:
			inclusive = true;
			/* fall through */
		case BTGreaterStrategyNumber:
			off = partition_list_bsearch(partsupfunc,
										 partcollation,
										 boundinfo, value,
										 &is_equal);
			if (off >= 0)
			{
				/* We don't want the matched datum to be in the result. */
				if (!is_equal || !inclusive)
					off++;
			}
			else
			{
				/*
				 * This case means all partition bounds are greater, which in
				 * turn means that all partitions satisfy this key.
				 */
				off = 0;
			}

			/*
			 * off is greater than the numbers of datums we have partitions
			 * for.  The only possible partition that could contain a match is
			 * the default partition, but we must've set context->scan_default
			 * above anyway if one exists.
			 */
			if (off > boundinfo->ndatums - 1)
				return result;

			minoff = off;
			break;

		case BTLessEqualStrategyNumber:
			inclusive = true;
			/* fall through */
		case BTLessStrategyNumber:
			off = partition_list_bsearch(partsupfunc,
										 partcollation,
										 boundinfo, value,
										 &is_equal);
			if (off >= 0 && is_equal && !inclusive)
				off--;

			/*
			 * off is smaller than the datums of all non-default partitions.
			 * The only possible partition that could contain a match is the
			 * default partition, but we must've set context->scan_default
			 * above anyway if one exists.
			 */
			if (off < 0)
				return result;

			maxoff = off;
			break;

		default:
			elog(ERROR, "invalid strategy number %d", opstrategy);
			break;
	}

	Assert(minoff >= 0 && maxoff >= 0);
	result->bound_offsets = bms_add_range(NULL, minoff, maxoff);
	return result;
}


/*
 * get_matching_range_bounds
 *		Determine the offsets of range bounds matching the specified values,
 *		according to the semantics of the given operator strategy
 *
 * Each datum whose offset is in result is to be treated as the upper bound of
 * the partition that will contain the desired values.
 *
 * scan_default is set in the returned struct if a default partition exists
 * and we're absolutely certain that it needs to be scanned.  We do *not* set
 * it just because values match portions of the key space uncovered by
 * partitions other than default (space which we normally assume to belong to
 * the default partition): the final set of bounds obtained after combining
 * multiple pruning steps might exclude it, so we infer its inclusion
 * elsewhere.
 *
 * 'opstrategy' if non-zero must be a btree strategy number.
 *
 * 'values' contains Datums indexed by the partition key to use for pruning.
 *
 * 'nvalues', number of Datums in 'values' array. Must be <= context->partnatts.
 *
 * 'partsupfunc' contains the range partitioning comparison functions to be
 * used to perform partition_range_datum_bsearch or partition_rbound_datum_cmp
 * using.
 *
 * 'nullkeys' is the set of partition keys that are null.
 */
static PruneStepResult *
get_matching_range_bounds(PartitionPruneContext *context,
						  StrategyNumber opstrategy, Datum *values, int nvalues,
						  FmgrInfo *partsupfunc, Bitmapset *nullkeys)
{
	PruneStepResult *result = (PruneStepResult *) palloc0(sizeof(PruneStepResult));
	PartitionBoundInfo boundinfo = context->boundinfo;
	Oid		   *partcollation = context->partcollation;
	int			partnatts = context->partnatts;
	int		   *partindices = boundinfo->indexes;
	int			off,
				minoff,
				maxoff;
	bool		is_equal;
	bool		inclusive = false;

	Assert(context->strategy == PARTITION_STRATEGY_RANGE);
	Assert(nvalues <= partnatts);

	result->scan_null = result->scan_default = false;

	/*
	 * If there are no datums to compare keys with, or if we got an IS NULL
	 * clause just return the default partition, if it exists.
	 */
	if (boundinfo->ndatums == 0 || !bms_is_empty(nullkeys))
	{
		result->scan_default = partition_bound_has_default(boundinfo);
		return result;
	}

	minoff = 0;
	maxoff = boundinfo->ndatums;

	/*
	 * If there are no values to compare with the datums in boundinfo, it
	 * means the caller asked for partitions for all non-null datums.  Add
	 * indexes of *all* partitions, including the default partition if one
	 * exists.
	 */
	if (nvalues == 0)
	{
		/* ignore key space not covered by any partitions */
		if (partindices[minoff] < 0)
			minoff++;
		if (partindices[maxoff] < 0)
			maxoff--;

		result->scan_default = partition_bound_has_default(boundinfo);
		Assert(partindices[minoff] >= 0 &&
			   partindices[maxoff] >= 0);
		result->bound_offsets = bms_add_range(NULL, minoff, maxoff);

		return result;
	}

	/*
	 * If the query does not constrain all key columns, we'll need to scan the
	 * default partition, if any.
	 */
	if (nvalues < partnatts)
		result->scan_default = partition_bound_has_default(boundinfo);

	switch (opstrategy)
	{
		case BTEqualStrategyNumber:
			/* Look for the smallest bound that is = lookup value. */
			off = partition_range_datum_bsearch(partsupfunc,
												partcollation,
												boundinfo,
												nvalues, values,
												&is_equal);

			if (off >= 0 && is_equal)
			{
				if (nvalues == partnatts)
				{
					/* There can only be zero or one matching partition. */
					result->bound_offsets = bms_make_singleton(off + 1);
					return result;
				}
				else
				{
					int			saved_off = off;

					/*
					 * Since the lookup value contains only a prefix of keys,
					 * we must find other bounds that may also match the
					 * prefix.  partition_range_datum_bsearch() returns the
					 * offset of one of them, find others by checking adjacent
					 * bounds.
					 */

					/*
					 * First find greatest bound that's smaller than the
					 * lookup value.
					 */
					while (off >= 1)
					{
						int32		cmpval;

						cmpval =
							partition_rbound_datum_cmp(partsupfunc,
													   partcollation,
													   boundinfo->datums[off - 1],
													   boundinfo->kind[off - 1],
													   values, nvalues);
						if (cmpval != 0)
							break;
						off--;
					}

					Assert(0 ==
						   partition_rbound_datum_cmp(partsupfunc,
													  partcollation,
													  boundinfo->datums[off],
													  boundinfo->kind[off],
													  values, nvalues));

					/*
					 * We can treat 'off' as the offset of the smallest bound
					 * to be included in the result, if we know it is the
					 * upper bound of the partition in which the lookup value
					 * could possibly exist.  One case it couldn't is if the
					 * bound, or precisely the matched portion of its prefix,
					 * is not inclusive.
					 */
					if (boundinfo->kind[off][nvalues] ==
						PARTITION_RANGE_DATUM_MINVALUE)
						off++;

					minoff = off;

					/*
					 * Now find smallest bound that's greater than the lookup
					 * value.
					 */
					off = saved_off;
					while (off < boundinfo->ndatums - 1)
					{
						int32		cmpval;

						cmpval = partition_rbound_datum_cmp(partsupfunc,
															partcollation,
															boundinfo->datums[off + 1],
															boundinfo->kind[off + 1],
															values, nvalues);
						if (cmpval != 0)
							break;
						off++;
					}

					Assert(0 ==
						   partition_rbound_datum_cmp(partsupfunc,
													  partcollation,
													  boundinfo->datums[off],
													  boundinfo->kind[off],
													  values, nvalues));

					/*
					 * off + 1, then would be the offset of the greatest bound
					 * to be included in the result.
					 */
					maxoff = off + 1;
				}

				Assert(minoff >= 0 && maxoff >= 0);
				result->bound_offsets = bms_add_range(NULL, minoff, maxoff);
			}
			else
			{
				/*
				 * The lookup value falls in the range between some bounds in
				 * boundinfo.  'off' would be the offset of the greatest bound
				 * that is <= lookup value, so add off + 1 to the result
				 * instead as the offset of the upper bound of the only
				 * partition that may contain the lookup value.  If 'off' is
				 * -1 indicating that all bounds are greater, then we simply
				 * end up adding the first bound's offset, that is, 0.
				 */
				result->bound_offsets = bms_make_singleton(off + 1);
			}

			return result;

		case BTGreaterEqualStrategyNumber:
			inclusive = true;
			/* fall through */
		case BTGreaterStrategyNumber:

			/*
			 * Look for the smallest bound that is > or >= lookup value and
			 * set minoff to its offset.
			 */
			off = partition_range_datum_bsearch(partsupfunc,
												partcollation,
												boundinfo,
												nvalues, values,
												&is_equal);
			if (off < 0)
			{
				/*
				 * All bounds are greater than the lookup value, so include
				 * all of them in the result.
				 */
				minoff = 0;
			}
			else
			{
				if (is_equal && nvalues < partnatts)
				{
					/*
					 * Since the lookup value contains only a prefix of keys,
					 * we must find other bounds that may also match the
					 * prefix.  partition_range_datum_bsearch() returns the
					 * offset of one of them, find others by checking adjacent
					 * bounds.
					 *
					 * Based on whether the lookup values are inclusive or
					 * not, we must either include the indexes of all such
					 * bounds in the result (that is, set minoff to the index
					 * of smallest such bound) or find the smallest one that's
					 * greater than the lookup values and set minoff to that.
					 */
					while (off >= 1 && off < boundinfo->ndatums - 1)
					{
						int32		cmpval;
						int			nextoff;

						nextoff = inclusive ? off - 1 : off + 1;
						cmpval =
							partition_rbound_datum_cmp(partsupfunc,
													   partcollation,
													   boundinfo->datums[nextoff],
													   boundinfo->kind[nextoff],
													   values, nvalues);
						if (cmpval != 0)
							break;

						off = nextoff;
					}

					Assert(0 ==
						   partition_rbound_datum_cmp(partsupfunc,
													  partcollation,
													  boundinfo->datums[off],
													  boundinfo->kind[off],
													  values, nvalues));

					minoff = inclusive ? off : off + 1;
				}
				else
				{

					/*
					 * lookup value falls in the range between some bounds in
					 * boundinfo.  off would be the offset of the greatest
					 * bound that is <= lookup value, so add off + 1 to the
					 * result instead as the offset of the upper bound of the
					 * smallest partition that may contain the lookup value.
					 */
					minoff = off + 1;
				}
			}
			break;

		case BTLessEqualStrategyNumber:
			inclusive = true;
			/* fall through */
		case BTLessStrategyNumber:

			/*
			 * Look for the greatest bound that is < or <= lookup value and
			 * set maxoff to its offset.
			 */
			off = partition_range_datum_bsearch(partsupfunc,
												partcollation,
												boundinfo,
												nvalues, values,
												&is_equal);
			if (off >= 0)
			{
				/*
				 * See the comment above.
				 */
				if (is_equal && nvalues < partnatts)
				{
					while (off >= 1 && off < boundinfo->ndatums - 1)
					{
						int32		cmpval;
						int			nextoff;

						nextoff = inclusive ? off + 1 : off - 1;
						cmpval = partition_rbound_datum_cmp(partsupfunc,
															partcollation,
															boundinfo->datums[nextoff],
															boundinfo->kind[nextoff],
															values, nvalues);
						if (cmpval != 0)
							break;

						off = nextoff;
					}

					Assert(0 ==
						   partition_rbound_datum_cmp(partsupfunc,
													  partcollation,
													  boundinfo->datums[off],
													  boundinfo->kind[off],
													  values, nvalues));

					maxoff = inclusive ? off + 1 : off;
				}

				/*
				 * The lookup value falls in the range between some bounds in
				 * boundinfo.  'off' would be the offset of the greatest bound
				 * that is <= lookup value, so add off + 1 to the result
				 * instead as the offset of the upper bound of the greatest
				 * partition that may contain lookup value.  If the lookup
				 * value had exactly matched the bound, but it isn't
				 * inclusive, no need add the adjacent partition.
				 */
				else if (!is_equal || inclusive)
					maxoff = off + 1;
				else
					maxoff = off;
			}
			else
			{
				/*
				 * 'off' is -1 indicating that all bounds are greater, so just
				 * set the first bound's offset as maxoff.
				 */
				maxoff = off + 1;
			}
			break;

		default:
			elog(ERROR, "invalid strategy number %d", opstrategy);
			break;
	}

	Assert(minoff >= 0 && minoff <= boundinfo->ndatums);
	Assert(maxoff >= 0 && maxoff <= boundinfo->ndatums);

	/*
	 * If the smallest partition to return has MINVALUE (negative infinity) as
	 * its lower bound, increment it to point to the next finite bound
	 * (supposedly its upper bound), so that we don't inadvertently end up
	 * scanning the default partition.
	 */
	if (minoff < boundinfo->ndatums && partindices[minoff] < 0)
	{
		int			lastkey = nvalues - 1;

		if (boundinfo->kind[minoff][lastkey] ==
			PARTITION_RANGE_DATUM_MINVALUE)
		{
			minoff++;
			Assert(boundinfo->indexes[minoff] >= 0);
		}
	}

	/*
	 * If the previous greatest partition has MAXVALUE (positive infinity) as
	 * its upper bound (something only possible to do with multi-column range
	 * partitioning), we scan switch to it as the greatest partition to
	 * return.  Again, so that we don't inadvertently end up scanning the
	 * default partition.
	 */
	if (maxoff >= 1 && partindices[maxoff] < 0)
	{
		int			lastkey = nvalues - 1;

		if (boundinfo->kind[maxoff - 1][lastkey] ==
			PARTITION_RANGE_DATUM_MAXVALUE)
		{
			maxoff--;
			Assert(boundinfo->indexes[maxoff] >= 0);
		}
	}

	Assert(minoff >= 0 && maxoff >= 0);
	if (minoff <= maxoff)
		result->bound_offsets = bms_add_range(NULL, minoff, maxoff);

	return result;
}

/*
 * pull_exec_paramids
 *		Returns a Bitmapset containing the paramids of all Params with
 *		paramkind = PARAM_EXEC in 'expr'.
 */
static Bitmapset *
pull_exec_paramids(Expr *expr)
{
	Bitmapset  *result = NULL;

	(void) pull_exec_paramids_walker((Node *) expr, &result);

	return result;
}

static bool
pull_exec_paramids_walker(Node *node, Bitmapset **context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_EXEC)
			*context = bms_add_member(*context, param->paramid);
		return false;
	}
	return expression_tree_walker(node, pull_exec_paramids_walker,
								  (void *) context);
}

/*
 * get_partkey_exec_paramids
 *		Loop through given pruning steps and find out which exec Params
 *		are used.
 *
 * Returns a Bitmapset of Param IDs.
 */
static Bitmapset *
get_partkey_exec_paramids(List *steps)
{
	Bitmapset  *execparamids = NULL;
	ListCell   *lc;

	foreach(lc, steps)
	{
		PartitionPruneStepOp *step = (PartitionPruneStepOp *) lfirst(lc);
		ListCell   *lc2;

		if (!IsA(step, PartitionPruneStepOp))
			continue;

		foreach(lc2, step->exprs)
		{
			Expr	   *expr = lfirst(lc2);

			/* We can be quick for plain Consts */
			if (!IsA(expr, Const))
				execparamids = bms_join(execparamids,
										pull_exec_paramids(expr));
		}
	}

	return execparamids;
}

/*
 * perform_pruning_base_step
 *		Determines the indexes of datums that satisfy conditions specified in
 *		'opstep'.
 *
 * Result also contains whether special null-accepting and/or default
 * partition need to be scanned.
 */
static PruneStepResult *
perform_pruning_base_step(PartitionPruneContext *context,
						  PartitionPruneStepOp *opstep)
{
	ListCell   *lc1,
			   *lc2;
	int			keyno,
				nvalues;
	Datum		values[PARTITION_MAX_KEYS];
	FmgrInfo   *partsupfunc;
	int			stateidx;

	/*
	 * There better be the same number of expressions and compare functions.
	 */
	Assert(list_length(opstep->exprs) == list_length(opstep->cmpfns));

	nvalues = 0;
	lc1 = list_head(opstep->exprs);
	lc2 = list_head(opstep->cmpfns);

	/*
	 * Generate the partition lookup key that will be used by one of the
	 * get_matching_*_bounds functions called below.
	 */
	for (keyno = 0; keyno < context->partnatts; keyno++)
	{
		/*
		 * For hash partitioning, it is possible that values of some keys are
		 * not provided in operator clauses, but instead the planner found
		 * that they appeared in a IS NULL clause.
		 */
		if (bms_is_member(keyno, opstep->nullkeys))
			continue;

		/*
		 * For range partitioning, we must only perform pruning with values
		 * for either all partition keys or a prefix thereof.
		 */
		if (keyno > nvalues && context->strategy == PARTITION_STRATEGY_RANGE)
			break;

		if (lc1 != NULL)
		{
			Expr	   *expr;
			Datum		datum;
			bool		isnull;
			Oid			cmpfn;

			expr = lfirst(lc1);
			stateidx = PruneCxtStateIdx(context->partnatts,
										opstep->step.step_id, keyno);
			partkey_datum_from_expr(context, expr, stateidx,
									&datum, &isnull);

			/*
			 * Since we only allow strict operators in pruning steps, any
			 * null-valued comparison value must cause the comparison to fail,
			 * so that no partitions could match.
			 */
			if (isnull)
			{
				PruneStepResult *result;

				result = (PruneStepResult *) palloc(sizeof(PruneStepResult));
				result->bound_offsets = NULL;
				result->scan_default = false;
				result->scan_null = false;

				return result;
			}

			/* Set up the stepcmpfuncs entry, unless we already did */
			cmpfn = lfirst_oid(lc2);
			Assert(OidIsValid(cmpfn));
			if (cmpfn != context->stepcmpfuncs[stateidx].fn_oid)
			{
				/*
				 * If the needed support function is the same one cached in
				 * the relation's partition key, copy the cached FmgrInfo.
				 * Otherwise (i.e., when we have a cross-type comparison), an
				 * actual lookup is required.
				 */
				if (cmpfn == context->partsupfunc[keyno].fn_oid)
					fmgr_info_copy(&context->stepcmpfuncs[stateidx],
								   &context->partsupfunc[keyno],
								   context->ppccontext);
				else
					fmgr_info_cxt(cmpfn, &context->stepcmpfuncs[stateidx],
								  context->ppccontext);
			}

			values[keyno] = datum;
			nvalues++;

			lc1 = lnext(opstep->exprs, lc1);
			lc2 = lnext(opstep->cmpfns, lc2);
		}
	}

	/*
	 * Point partsupfunc to the entry for the 0th key of this step; the
	 * additional support functions, if any, follow consecutively.
	 */
	stateidx = PruneCxtStateIdx(context->partnatts, opstep->step.step_id, 0);
	partsupfunc = &context->stepcmpfuncs[stateidx];

	switch (context->strategy)
	{
		case PARTITION_STRATEGY_HASH:
			return get_matching_hash_bounds(context,
											opstep->opstrategy,
											values, nvalues,
											partsupfunc,
											opstep->nullkeys);

		case PARTITION_STRATEGY_LIST:
			return get_matching_list_bounds(context,
											opstep->opstrategy,
											values[0], nvalues,
											&partsupfunc[0],
											opstep->nullkeys);

		case PARTITION_STRATEGY_RANGE:
			return get_matching_range_bounds(context,
											 opstep->opstrategy,
											 values, nvalues,
											 partsupfunc,
											 opstep->nullkeys);

		default:
			elog(ERROR, "unexpected partition strategy: %d",
				 (int) context->strategy);
			break;
	}

	return NULL;
}

/*
 * perform_pruning_combine_step
 *		Determines the indexes of datums obtained by combining those given
 *		by the steps identified by cstep->source_stepids using the specified
 *		combination method
 *
 * Since cstep may refer to the result of earlier steps, we also receive
 * step_results here.
 */
static PruneStepResult *
perform_pruning_combine_step(PartitionPruneContext *context,
							 PartitionPruneStepCombine *cstep,
							 PruneStepResult **step_results)
{
	PruneStepResult *result = (PruneStepResult *) palloc0(sizeof(PruneStepResult));
	bool		firststep;
	ListCell   *lc1;

	/*
	 * A combine step without any source steps is an indication to not perform
	 * any partition pruning.  Return all datum indexes in that case.
	 */
	if (cstep->source_stepids == NIL)
	{
		PartitionBoundInfo boundinfo = context->boundinfo;

		result->bound_offsets =
			bms_add_range(NULL, 0, boundinfo->nindexes - 1);
		result->scan_default = partition_bound_has_default(boundinfo);
		result->scan_null = partition_bound_accepts_nulls(boundinfo);
		return result;
	}

	switch (cstep->combineOp)
	{
		case PARTPRUNE_COMBINE_UNION:
			foreach(lc1, cstep->source_stepids)
			{
				int			step_id = lfirst_int(lc1);
				PruneStepResult *step_result;

				/*
				 * step_results[step_id] must contain a valid result, which is
				 * confirmed by the fact that cstep's step_id is greater than
				 * step_id and the fact that results of the individual steps
				 * are evaluated in sequence of their step_ids.
				 */
				if (step_id >= cstep->step.step_id)
					elog(ERROR, "invalid pruning combine step argument");
				step_result = step_results[step_id];
				Assert(step_result != NULL);

				/* Record any additional datum indexes from this step */
				result->bound_offsets = bms_add_members(result->bound_offsets,
														step_result->bound_offsets);

				/* Update whether to scan null and default partitions. */
				if (!result->scan_null)
					result->scan_null = step_result->scan_null;
				if (!result->scan_default)
					result->scan_default = step_result->scan_default;
			}
			break;

		case PARTPRUNE_COMBINE_INTERSECT:
			firststep = true;
			foreach(lc1, cstep->source_stepids)
			{
				int			step_id = lfirst_int(lc1);
				PruneStepResult *step_result;

				if (step_id >= cstep->step.step_id)
					elog(ERROR, "invalid pruning combine step argument");
				step_result = step_results[step_id];
				Assert(step_result != NULL);

				if (firststep)
				{
					/* Copy step's result the first time. */
					result->bound_offsets =
						bms_copy(step_result->bound_offsets);
					result->scan_null = step_result->scan_null;
					result->scan_default = step_result->scan_default;
					firststep = false;
				}
				else
				{
					/* Record datum indexes common to both steps */
					result->bound_offsets =
						bms_int_members(result->bound_offsets,
										step_result->bound_offsets);

					/* Update whether to scan null and default partitions. */
					if (result->scan_null)
						result->scan_null = step_result->scan_null;
					if (result->scan_default)
						result->scan_default = step_result->scan_default;
				}
			}
			break;
	}

	return result;
}

/*
 * match_boolean_partition_clause
 *
 * If we're able to match the clause to the partition key as specially-shaped
 * boolean clause, set *outconst to a Const containing a true or false value
 * and return PARTCLAUSE_MATCH_CLAUSE.  Returns PARTCLAUSE_UNSUPPORTED if the
 * clause is not a boolean clause or if the boolean clause is unsuitable for
 * partition pruning.  Returns PARTCLAUSE_NOMATCH if it's a bool quals but
 * just does not match this partition key.  *outconst is set to NULL in the
 * latter two cases.
 */
static PartClauseMatchStatus
match_boolean_partition_clause(Oid partopfamily, Expr *clause, Expr *partkey,
							   Expr **outconst)
{
	Expr	   *leftop;

	*outconst = NULL;

	if (!IsBooleanOpfamily(partopfamily))
		return PARTCLAUSE_UNSUPPORTED;

	if (IsA(clause, BooleanTest))
	{
		BooleanTest *btest = (BooleanTest *) clause;

		/* Only IS [NOT] TRUE/FALSE are any good to us */
		if (btest->booltesttype == IS_UNKNOWN ||
			btest->booltesttype == IS_NOT_UNKNOWN)
			return PARTCLAUSE_UNSUPPORTED;

		leftop = btest->arg;
		if (IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;

		if (equal(leftop, partkey))
			*outconst = (btest->booltesttype == IS_TRUE ||
						 btest->booltesttype == IS_NOT_FALSE)
				? (Expr *) makeBoolConst(true, false)
				: (Expr *) makeBoolConst(false, false);

		if (*outconst)
			return PARTCLAUSE_MATCH_CLAUSE;
	}
	else
	{
		bool		is_not_clause = is_notclause(clause);

		leftop = is_not_clause ? get_notclausearg(clause) : clause;

		if (IsA(leftop, RelabelType))
			leftop = ((RelabelType *) leftop)->arg;

		/* Compare to the partition key, and make up a clause ... */
		if (equal(leftop, partkey))
			*outconst = is_not_clause ?
				(Expr *) makeBoolConst(false, false) :
				(Expr *) makeBoolConst(true, false);
		else if (equal(negate_clause((Node *) leftop), partkey))
			*outconst = (Expr *) makeBoolConst(false, false);

		if (*outconst)
			return PARTCLAUSE_MATCH_CLAUSE;
	}

	return PARTCLAUSE_NOMATCH;
}

/*
 * partkey_datum_from_expr
 *		Evaluate expression for potential partition pruning
 *
 * Evaluate 'expr'; set *value and *isnull to the resulting Datum and nullflag.
 *
 * If expr isn't a Const, its ExprState is in stateidx of the context
 * exprstate array.
 *
 * Note that the evaluated result may be in the per-tuple memory context of
 * context->planstate->ps_ExprContext, and we may have leaked other memory
 * there too.  This memory must be recovered by resetting that ExprContext
 * after we're done with the pruning operation (see execPartition.c).
 */
static void
partkey_datum_from_expr(PartitionPruneContext *context,
						Expr *expr, int stateidx,
						Datum *value, bool *isnull)
{
	if (IsA(expr, Const))
	{
		/* We can always determine the value of a constant */
		Const	   *con = (Const *) expr;

		*value = con->constvalue;
		*isnull = con->constisnull;
	}
	else
	{
		ExprState  *exprstate;
		ExprContext *ectx;

		/*
		 * We should never see a non-Const in a step unless we're running in
		 * the executor.
		 */
		Assert(context->planstate != NULL);

		exprstate = context->exprstates[stateidx];
		ectx = context->planstate->ps_ExprContext;
		*value = ExecEvalExprSwitchContext(exprstate, ectx, isnull);
	}
}
