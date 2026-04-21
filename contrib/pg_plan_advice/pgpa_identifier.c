/*-------------------------------------------------------------------------
 *
 * pgpa_identifier.c
 *	  create appropriate identifiers for range table entries
 *
 * The goal of this module is to be able to produce identifiers for range
 * table entries that are unique, understandable to human beings, and
 * able to be reconstructed during future planning cycles. As an
 * exception, we do not care about, or want to produce, identifiers for
 * RTE_JOIN entries. This is because (1) we would end up with a ton of
 * RTEs with unhelpful names like unnamed_join_17; (2) not all joins have
 * RTEs; and (3) we intend to refer to joins by their constituent members
 * rather than by reference to the join RTE.
 *
 * In general, we construct identifiers of the following form:
 *
 * alias_name#occurrence_number/child_table_name@subquery_name
 *
 * However, occurrence_number is omitted when it is the first occurrence
 * within the same subquery, child_table_name is omitted for relations that
 * are not child tables, and subquery_name is omitted for the topmost
 * query level. Whenever an item is omitted, the preceding punctuation mark
 * is also omitted.  Identifier-style escaping is applied to alias_name and
 * subquery_name.  In generated advice, child table names are always
 * schema-qualified, but users can supply advice where the schema name is
 * not mentioned. Identifier-style escaping is applied to the schema and to
 * the relation name separately.
 *
 * The upshot of all of these rules is that in simple cases, the relation
 * identifier is textually identical to the alias name, making life easier
 * for users. However, even in complex cases, every relation identifier
 * for a given query will be unique (or at least we hope so: if not, this
 * code is buggy and the identifier format might need to be rethought).
 *
 * A key goal of this system is that we want to be able to reconstruct the
 * same identifiers during a future planning cycle for the same query, so
 * that if a certain behavior is specified for a certain identifier, we can
 * properly identify the RTI for which that behavior is mandated. In order
 * for this to work, subquery names must be unique and known before the
 * subquery is planned, and the remainder of the identifier must not depend
 * on any part of the query outside of the current subquery level. In
 * particular, occurrence_number must be calculated relative to the range
 * table for the relevant subquery, not the final flattened range table.
 *
 * NB: All of this code must use rt_fetch(), not planner_rt_fetch()!
 * Join removal and self-join elimination remove rels from the arrays
 * that planner_rt_fetch() uses; using rt_fetch() is necessary to get
 * stable results.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_identifier.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgpa_identifier.h"

#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

static Index *pgpa_create_top_rti_map(Index rtable_length, List *rtable,
									  List *appinfos);
static int	pgpa_occurrence_number(List *rtable, Index *top_rti_map,
								   SubPlanRTInfo *rtinfo, Index rti);

/*
 * Create a range table identifier from scratch.
 *
 * This function leaves the caller to do all the heavy lifting, so it's
 * generally better to use one of the functions below instead.
 *
 * See the file header comments for more details on the format of an
 * identifier.
 */
const char *
pgpa_identifier_string(const pgpa_identifier *rid)
{
	const char *result;

	Assert(rid->alias_name != NULL);
	result = quote_identifier(rid->alias_name);

	Assert(rid->occurrence >= 0);
	if (rid->occurrence > 1)
		result = psprintf("%s#%d", result, rid->occurrence);

	if (rid->partrel != NULL)
	{
		if (rid->partnsp == NULL)
			result = psprintf("%s/%s", result,
							  quote_identifier(rid->partrel));
		else
			result = psprintf("%s/%s.%s", result,
							  quote_identifier(rid->partnsp),
							  quote_identifier(rid->partrel));
	}

	if (rid->plan_name != NULL)
		result = psprintf("%s@%s", result, quote_identifier(rid->plan_name));

	return result;
}

/*
 * Compute a relation identifier for a particular RTI.
 *
 * The caller provides root and rti, and gets the necessary details back via
 * the remaining parameters.
 */
void
pgpa_compute_identifier_by_rti(PlannerInfo *root, Index rti,
							   pgpa_identifier *rid)
{
	Index		top_rti = rti;
	int			occurrence = 1;
	RangeTblEntry *rte;
	RangeTblEntry *top_rte;
	char	   *partnsp = NULL;
	char	   *partrel = NULL;

	/*
	 * If this is a child RTE, find the topmost parent that is still of type
	 * RTE_RELATION. We do this because we identify children of partitioned
	 * tables by the name of the child table, but subqueries can also have
	 * child rels and we don't care about those here.
	 */
	for (;;)
	{
		AppendRelInfo *appinfo;
		RangeTblEntry *parent_rte;

		/* append_rel_array can be NULL if there are no children */
		if (root->append_rel_array == NULL ||
			(appinfo = root->append_rel_array[top_rti]) == NULL)
			break;

		parent_rte = rt_fetch(appinfo->parent_relid, root->parse->rtable);
		if (parent_rte->rtekind != RTE_RELATION)
			break;

		top_rti = appinfo->parent_relid;
	}

	/* Get the range table entries for the RTI and top RTI. */
	rte = rt_fetch(rti, root->parse->rtable);
	top_rte = rt_fetch(top_rti, root->parse->rtable);
	Assert(rte->rtekind != RTE_JOIN);
	Assert(top_rte->rtekind != RTE_JOIN);

	/* Work out the correct occurrence number. */
	for (Index prior_rti = 1; prior_rti < top_rti; ++prior_rti)
	{
		RangeTblEntry *prior_rte;
		AppendRelInfo *appinfo;

		/*
		 * If this is a child rel of a parent that is a relation, skip it.
		 *
		 * Such range table entries are disambiguated by mentioning the schema
		 * and name of the table, not by counting them as separate occurrences
		 * of the same table.
		 *
		 * NB: append_rel_array can be NULL if there are no children
		 */
		if (root->append_rel_array != NULL &&
			(appinfo = root->append_rel_array[prior_rti]) != NULL)
		{
			RangeTblEntry *parent_rte;

			parent_rte = rt_fetch(appinfo->parent_relid, root->parse->rtable);
			if (parent_rte->rtekind == RTE_RELATION)
				continue;
		}

		/* Skip NULL entries and joins. */
		prior_rte = rt_fetch(prior_rti, root->parse->rtable);
		if (prior_rte == NULL || prior_rte->rtekind == RTE_JOIN)
			continue;

		/* Skip if the alias name differs. */
		if (strcmp(prior_rte->eref->aliasname, rte->eref->aliasname) != 0)
			continue;

		/* Looks like a true duplicate. */
		++occurrence;
	}

	/* If this is a child table, get the schema and relation names. */
	if (rti != top_rti)
	{
		partnsp = get_namespace_name_or_temp(get_rel_namespace(rte->relid));
		partrel = get_rel_name(rte->relid);
	}

	/* OK, we have all the answers we need. Return them to the caller. */
	rid->alias_name = top_rte->eref->aliasname;
	rid->occurrence = occurrence;
	rid->partnsp = partnsp;
	rid->partrel = partrel;
	rid->plan_name = root->plan_name;
}

/*
 * Compute a relation identifier for a set of RTIs, except for any RTE_JOIN
 * RTIs that may be present.
 *
 * RTE_JOIN entries are excluded because they cannot be mentioned by plan
 * advice.
 *
 * The caller is responsible for making sure that the "rids" array is large
 * enough to store the results.
 *
 * The return value is the number of identifiers computed.
 */
int
pgpa_compute_identifiers_by_relids(PlannerInfo *root, Bitmapset *relids,
								   pgpa_identifier *rids)
{
	int			count = 0;
	int			rti = -1;

	while ((rti = bms_next_member(relids, rti)) >= 0)
	{
		RangeTblEntry *rte = rt_fetch(rti, root->parse->rtable);

		if (rte->rtekind == RTE_JOIN)
			continue;
		pgpa_compute_identifier_by_rti(root, rti, &rids[count++]);
	}

	Assert(count > 0);
	return count;
}

/*
 * Create an array of range table identifiers for all the non-NULL,
 * non-RTE_JOIN entries in the PlannedStmt's range table.
 */
pgpa_identifier *
pgpa_create_identifiers_for_planned_stmt(PlannedStmt *pstmt)
{
	Index		rtable_length = list_length(pstmt->rtable);
	pgpa_identifier *result = palloc0_array(pgpa_identifier, rtable_length);
	Index	   *top_rti_map;
	int			rtinfoindex = 0;
	SubPlanRTInfo *rtinfo = NULL;
	SubPlanRTInfo *nextrtinfo = NULL;

	/*
	 * Account for relations added by inheritance expansion of partitioned
	 * tables.
	 */
	top_rti_map = pgpa_create_top_rti_map(rtable_length, pstmt->rtable,
										  pstmt->appendRelations);

	/*
	 * When we begin iterating, we're processing the portion of the range
	 * table that originated from the top-level PlannerInfo, so subrtinfo is
	 * NULL. Later, subrtinfo will be the SubPlanRTInfo for the subquery whose
	 * portion of the range table we are processing. nextrtinfo is always the
	 * SubPlanRTInfo that follows the current one, if any, so when we're
	 * processing the top-level query's portion of the range table, the next
	 * SubPlanRTInfo is the very first one.
	 */
	if (pstmt->subrtinfos != NULL)
		nextrtinfo = linitial(pstmt->subrtinfos);

	/* Main loop over the range table. */
	for (Index rti = 1; rti <= rtable_length; rti++)
	{
		const char *plan_name;
		Index		top_rti;
		RangeTblEntry *rte;
		RangeTblEntry *top_rte;
		char	   *partnsp = NULL;
		char	   *partrel = NULL;
		int			occurrence;
		pgpa_identifier *rid;

		/*
		 * Advance to the next SubPlanRTInfo, if it's time to do that.
		 *
		 * This loop probably shouldn't ever iterate more than once, because
		 * that would imply that a subquery was planned but added nothing to
		 * the range table; but let's be defensive and assume it can happen.
		 */
		while (nextrtinfo != NULL && rti > nextrtinfo->rtoffset)
		{
			rtinfo = nextrtinfo;
			if (++rtinfoindex >= list_length(pstmt->subrtinfos))
				nextrtinfo = NULL;
			else
				nextrtinfo = list_nth(pstmt->subrtinfos, rtinfoindex);
		}

		/* Fetch the range table entry, if any. */
		rte = rt_fetch(rti, pstmt->rtable);

		/*
		 * We can't and don't need to identify null entries, and we don't want
		 * to identify join entries.
		 */
		if (rte == NULL || rte->rtekind == RTE_JOIN)
			continue;

		/*
		 * If this is not a relation added by partitioned table expansion,
		 * then the top RTI/RTE are just the same as this RTI/RTE. Otherwise,
		 * we need the information for the top RTI/RTE, and must also fetch
		 * the partition schema and name.
		 */
		top_rti = top_rti_map[rti - 1];
		if (rti == top_rti)
			top_rte = rte;
		else
		{
			top_rte = rt_fetch(top_rti, pstmt->rtable);
			partnsp =
				get_namespace_name_or_temp(get_rel_namespace(rte->relid));
			partrel = get_rel_name(rte->relid);
		}

		/* Compute the correct occurrence number. */
		occurrence = pgpa_occurrence_number(pstmt->rtable, top_rti_map,
											rtinfo, top_rti);

		/* Get the name of the current plan (NULL for toplevel query). */
		plan_name = rtinfo == NULL ? NULL : rtinfo->plan_name;

		/* Save all the details we've derived. */
		rid = &result[rti - 1];
		rid->alias_name = top_rte->eref->aliasname;
		rid->occurrence = occurrence;
		rid->partnsp = partnsp;
		rid->partrel = partrel;
		rid->plan_name = plan_name;
	}

	return result;
}

/*
 * Search for a pgpa_identifier in the array of identifiers computed for the
 * range table. If exactly one match is found, return the matching RTI; else
 * return 0.
 */
Index
pgpa_compute_rti_from_identifier(int rtable_length,
								 pgpa_identifier *rt_identifiers,
								 pgpa_identifier *rid)
{
	Index		result = 0;

	for (Index rti = 1; rti <= rtable_length; ++rti)
	{
		pgpa_identifier *rti_rid = &rt_identifiers[rti - 1];

		/* If there's no identifier for this RTI, skip it. */
		if (rti_rid->alias_name == NULL)
			continue;

		/*
		 * If it matches, return this RTI. As usual, an omitted partition
		 * schema matches anything, but partition and plan names must either
		 * match exactly or be omitted on both sides.
		 */
		if (strcmp(rid->alias_name, rti_rid->alias_name) == 0 &&
			rid->occurrence == rti_rid->occurrence &&
			(rid->partnsp == NULL || rti_rid->partnsp == NULL ||
			 strcmp(rid->partnsp, rti_rid->partnsp) == 0) &&
			strings_equal_or_both_null(rid->partrel, rti_rid->partrel) &&
			strings_equal_or_both_null(rid->plan_name, rti_rid->plan_name))
		{
			if (result != 0)
			{
				/* Multiple matches were found. */
				return 0;
			}
			result = rti;
		}
	}

	return result;
}

/*
 * Build a mapping from each RTI to the RTI whose alias_name will be used to
 * construct the range table identifier.
 *
 * For child relations, this is the topmost parent that is still of type
 * RTE_RELATION. For other relations, it's just the original RTI.
 *
 * Since we're eventually going to need this information for every RTI in
 * the range table, it's best to compute all the answers in a single pass over
 * the AppendRelInfo list. Otherwise, we might end up searching through that
 * list repeatedly for entries of interest.
 *
 * Note that the returned array is uses zero-based indexing, while RTIs use
 * 1-based indexing, so subtract 1 from the RTI before looking it up in the
 * array.
 */
static Index *
pgpa_create_top_rti_map(Index rtable_length, List *rtable, List *appinfos)
{
	Index	   *top_rti_map = palloc0_array(Index, rtable_length);

	/* Initially, make every RTI point to itself. */
	for (Index rti = 1; rti <= rtable_length; ++rti)
		top_rti_map[rti - 1] = rti;

	/* Update the map for each AppendRelInfo object. */
	foreach_node(AppendRelInfo, appinfo, appinfos)
	{
		Index		parent_rti = appinfo->parent_relid;
		RangeTblEntry *parent_rte = rt_fetch(parent_rti, rtable);

		/* If the parent is not RTE_RELATION, ignore this entry. */
		if (parent_rte->rtekind != RTE_RELATION)
			continue;

		/*
		 * Map the child to wherever we mapped the parent. Parents always
		 * precede their children in the AppendRelInfo list, so this should
		 * work out.
		 */
		top_rti_map[appinfo->child_relid - 1] = top_rti_map[parent_rti - 1];
	}

	return top_rti_map;
}

/*
 * Find the occurrence number of a certain relation within a certain subquery.
 *
 * The same alias name can occur multiple times within a subquery, but we want
 * to disambiguate by giving different occurrences different integer indexes.
 * However, child tables are disambiguated by including the table name rather
 * than by incrementing the occurrence number; and joins are not named and so
 * shouldn't increment the occurrence number either.
 */
static int
pgpa_occurrence_number(List *rtable, Index *top_rti_map,
					   SubPlanRTInfo *rtinfo, Index rti)
{
	Index		rtoffset = (rtinfo == NULL) ? 0 : rtinfo->rtoffset;
	int			occurrence = 1;
	RangeTblEntry *rte = rt_fetch(rti, rtable);

	for (Index prior_rti = rtoffset + 1; prior_rti < rti; ++prior_rti)
	{
		RangeTblEntry *prior_rte;

		/*
		 * If this is a child rel of a parent that is a relation, skip it.
		 *
		 * Such range table entries are disambiguated by mentioning the schema
		 * and name of the table, not by counting them as separate occurrences
		 * of the same table.
		 */
		if (top_rti_map[prior_rti - 1] != prior_rti)
			continue;

		/* Skip joins. */
		prior_rte = rt_fetch(prior_rti, rtable);
		if (prior_rte->rtekind == RTE_JOIN)
			continue;

		/* Skip if the alias name differs. */
		if (strcmp(prior_rte->eref->aliasname, rte->eref->aliasname) != 0)
			continue;

		/* Looks like a true duplicate. */
		++occurrence;
	}

	return occurrence;
}
