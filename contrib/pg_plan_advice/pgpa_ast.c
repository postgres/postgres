/*-------------------------------------------------------------------------
 *
 * pgpa_ast.c
 *	  additional supporting code related to plan advice parsing
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_ast.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgpa_ast.h"

#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"

static bool pgpa_identifiers_cover_target(int nrids, pgpa_identifier *rids,
										  pgpa_advice_target *target,
										  bool *rids_used);

/*
 * Get a C string that corresponds to the specified advice tag.
 */
char *
pgpa_cstring_advice_tag(pgpa_advice_tag_type advice_tag)
{
	switch (advice_tag)
	{
		case PGPA_TAG_BITMAP_HEAP_SCAN:
			return "BITMAP_HEAP_SCAN";
		case PGPA_TAG_FOREIGN_JOIN:
			return "FOREIGN_JOIN";
		case PGPA_TAG_GATHER:
			return "GATHER";
		case PGPA_TAG_GATHER_MERGE:
			return "GATHER_MERGE";
		case PGPA_TAG_HASH_JOIN:
			return "HASH_JOIN";
		case PGPA_TAG_INDEX_ONLY_SCAN:
			return "INDEX_ONLY_SCAN";
		case PGPA_TAG_INDEX_SCAN:
			return "INDEX_SCAN";
		case PGPA_TAG_JOIN_ORDER:
			return "JOIN_ORDER";
		case PGPA_TAG_MERGE_JOIN_MATERIALIZE:
			return "MERGE_JOIN_MATERIALIZE";
		case PGPA_TAG_MERGE_JOIN_PLAIN:
			return "MERGE_JOIN_PLAIN";
		case PGPA_TAG_NESTED_LOOP_MATERIALIZE:
			return "NESTED_LOOP_MATERIALIZE";
		case PGPA_TAG_NESTED_LOOP_MEMOIZE:
			return "NESTED_LOOP_MEMOIZE";
		case PGPA_TAG_NESTED_LOOP_PLAIN:
			return "NESTED_LOOP_PLAIN";
		case PGPA_TAG_NO_GATHER:
			return "NO_GATHER";
		case PGPA_TAG_PARTITIONWISE:
			return "PARTITIONWISE";
		case PGPA_TAG_SEMIJOIN_NON_UNIQUE:
			return "SEMIJOIN_NON_UNIQUE";
		case PGPA_TAG_SEMIJOIN_UNIQUE:
			return "SEMIJOIN_UNIQUE";
		case PGPA_TAG_SEQ_SCAN:
			return "SEQ_SCAN";
		case PGPA_TAG_TID_SCAN:
			return "TID_SCAN";
	}

	pg_unreachable();
	return NULL;
}

/*
 * Convert an advice tag, formatted as a string that has already been
 * downcased as appropriate, to a pgpa_advice_tag_type.
 *
 * If we succeed, set *fail = false and return the result; if we fail,
 * set *fail = true and return an arbitrary value.
 */
pgpa_advice_tag_type
pgpa_parse_advice_tag(const char *tag, bool *fail)
{
	*fail = false;

	switch (tag[0])
	{
		case 'b':
			if (strcmp(tag, "bitmap_heap_scan") == 0)
				return PGPA_TAG_BITMAP_HEAP_SCAN;
			break;
		case 'f':
			if (strcmp(tag, "foreign_join") == 0)
				return PGPA_TAG_FOREIGN_JOIN;
			break;
		case 'g':
			if (strcmp(tag, "gather") == 0)
				return PGPA_TAG_GATHER;
			if (strcmp(tag, "gather_merge") == 0)
				return PGPA_TAG_GATHER_MERGE;
			break;
		case 'h':
			if (strcmp(tag, "hash_join") == 0)
				return PGPA_TAG_HASH_JOIN;
			break;
		case 'i':
			if (strcmp(tag, "index_scan") == 0)
				return PGPA_TAG_INDEX_SCAN;
			if (strcmp(tag, "index_only_scan") == 0)
				return PGPA_TAG_INDEX_ONLY_SCAN;
			break;
		case 'j':
			if (strcmp(tag, "join_order") == 0)
				return PGPA_TAG_JOIN_ORDER;
			break;
		case 'm':
			if (strcmp(tag, "merge_join_materialize") == 0)
				return PGPA_TAG_MERGE_JOIN_MATERIALIZE;
			if (strcmp(tag, "merge_join_plain") == 0)
				return PGPA_TAG_MERGE_JOIN_PLAIN;
			break;
		case 'n':
			if (strcmp(tag, "nested_loop_materialize") == 0)
				return PGPA_TAG_NESTED_LOOP_MATERIALIZE;
			if (strcmp(tag, "nested_loop_memoize") == 0)
				return PGPA_TAG_NESTED_LOOP_MEMOIZE;
			if (strcmp(tag, "nested_loop_plain") == 0)
				return PGPA_TAG_NESTED_LOOP_PLAIN;
			if (strcmp(tag, "no_gather") == 0)
				return PGPA_TAG_NO_GATHER;
			break;
		case 'p':
			if (strcmp(tag, "partitionwise") == 0)
				return PGPA_TAG_PARTITIONWISE;
			break;
		case 's':
			if (strcmp(tag, "semijoin_non_unique") == 0)
				return PGPA_TAG_SEMIJOIN_NON_UNIQUE;
			if (strcmp(tag, "semijoin_unique") == 0)
				return PGPA_TAG_SEMIJOIN_UNIQUE;
			if (strcmp(tag, "seq_scan") == 0)
				return PGPA_TAG_SEQ_SCAN;
			break;
		case 't':
			if (strcmp(tag, "tid_scan") == 0)
				return PGPA_TAG_TID_SCAN;
			break;
	}

	/* didn't work out */
	*fail = true;

	/* return an arbitrary value to unwind the call stack */
	return PGPA_TAG_SEQ_SCAN;
}

/*
 * Format a pgpa_advice_target as a string and append result to a StringInfo.
 */
void
pgpa_format_advice_target(StringInfo str, pgpa_advice_target *target)
{
	if (target->ttype != PGPA_TARGET_IDENTIFIER)
	{
		bool		first = true;
		char	   *delims;

		if (target->ttype == PGPA_TARGET_UNORDERED_LIST)
			delims = "{}";
		else
			delims = "()";

		appendStringInfoChar(str, delims[0]);
		foreach_ptr(pgpa_advice_target, child_target, target->children)
		{
			if (first)
				first = false;
			else
				appendStringInfoChar(str, ' ');
			pgpa_format_advice_target(str, child_target);
		}
		appendStringInfoChar(str, delims[1]);
	}
	else
	{
		const char *rt_identifier;

		rt_identifier = pgpa_identifier_string(&target->rid);
		appendStringInfoString(str, rt_identifier);
	}
}

/*
 * Format a pgpa_index_target as a string and append result to a StringInfo.
 */
void
pgpa_format_index_target(StringInfo str, pgpa_index_target *itarget)
{
	if (itarget->indnamespace != NULL)
		appendStringInfo(str, "%s.",
						 quote_identifier(itarget->indnamespace));
	appendStringInfoString(str, quote_identifier(itarget->indname));
}

/*
 * Determine whether two pgpa_index_target objects are exactly identical.
 */
bool
pgpa_index_targets_equal(pgpa_index_target *i1, pgpa_index_target *i2)
{
	/* indnamespace can be NULL, and two NULL values are equal */
	if ((i1->indnamespace != NULL || i2->indnamespace != NULL) &&
		(i1->indnamespace == NULL || i2->indnamespace == NULL ||
		 strcmp(i1->indnamespace, i2->indnamespace) != 0))
		return false;
	if (strcmp(i1->indname, i2->indname) != 0)
		return false;

	return true;
}

/*
 * Check whether an identifier matches an any part of an advice target.
 */
bool
pgpa_identifier_matches_target(pgpa_identifier *rid, pgpa_advice_target *target)
{
	/* For non-identifiers, check all descendants. */
	if (target->ttype != PGPA_TARGET_IDENTIFIER)
	{
		foreach_ptr(pgpa_advice_target, child_target, target->children)
		{
			if (pgpa_identifier_matches_target(rid, child_target))
				return true;
		}
		return false;
	}

	/* Straightforward comparisons of alias name and occurrence number. */
	if (strcmp(rid->alias_name, target->rid.alias_name) != 0)
		return false;
	if (rid->occurrence != target->rid.occurrence)
		return false;

	/*
	 * If a relation identifier mentions a partition name, it should also
	 * specify a partition schema. But the target may leave the schema NULL to
	 * match anything.
	 */
	Assert(rid->partnsp != NULL || rid->partrel == NULL);
	if (rid->partnsp != NULL && target->rid.partnsp != NULL &&
		strcmp(rid->partnsp, target->rid.partnsp) != 0)
		return false;

	/*
	 * These fields can be NULL on either side, but NULL only matches another
	 * NULL.
	 */
	if (!strings_equal_or_both_null(rid->partrel, target->rid.partrel))
		return false;
	if (!strings_equal_or_both_null(rid->plan_name, target->rid.plan_name))
		return false;

	return true;
}

/*
 * Match identifiers to advice targets and return an enum value indicating
 * the relationship between the set of keys and the set of targets.
 *
 * See the comments for pgpa_itm_type.
 */
pgpa_itm_type
pgpa_identifiers_match_target(int nrids, pgpa_identifier *rids,
							  pgpa_advice_target *target)
{
	bool		all_rids_used = true;
	bool		any_rids_used = false;
	bool		all_targets_used;
	bool	   *rids_used = palloc0_array(bool, nrids);

	all_targets_used =
		pgpa_identifiers_cover_target(nrids, rids, target, rids_used);

	for (int i = 0; i < nrids; ++i)
	{
		if (rids_used[i])
			any_rids_used = true;
		else
			all_rids_used = false;
	}

	if (all_rids_used)
	{
		if (all_targets_used)
			return PGPA_ITM_EQUAL;
		else
			return PGPA_ITM_KEYS_ARE_SUBSET;
	}
	else
	{
		if (all_targets_used)
			return PGPA_ITM_TARGETS_ARE_SUBSET;
		else if (any_rids_used)
			return PGPA_ITM_INTERSECTING;
		else
			return PGPA_ITM_DISJOINT;
	}
}

/*
 * Returns true if every target or sub-target is matched by at least one
 * identifier, and otherwise false.
 *
 * Also sets rids_used[i] = true for each idenifier that matches at least one
 * target.
 */
static bool
pgpa_identifiers_cover_target(int nrids, pgpa_identifier *rids,
							  pgpa_advice_target *target, bool *rids_used)
{
	bool		result = false;

	if (target->ttype != PGPA_TARGET_IDENTIFIER)
	{
		result = true;

		foreach_ptr(pgpa_advice_target, child_target, target->children)
		{
			if (!pgpa_identifiers_cover_target(nrids, rids, child_target,
											   rids_used))
				result = false;
		}
	}
	else
	{
		for (int i = 0; i < nrids; ++i)
		{
			if (pgpa_identifier_matches_target(&rids[i], target))
			{
				rids_used[i] = true;
				result = true;
			}
		}
	}

	return result;
}
