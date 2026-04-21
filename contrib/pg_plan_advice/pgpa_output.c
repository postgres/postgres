/*-------------------------------------------------------------------------
 *
 * pgpa_output.c
 *	  produce textual output from the results of a plan tree walk
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_output.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pgpa_output.h"
#include "pgpa_scan.h"

#include "nodes/parsenodes.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

/*
 * Context object for textual advice generation.
 *
 * rt_identifiers is the caller-provided array of range table identifiers.
 * See the comments at the top of pgpa_identifier.c for more details.
 *
 * buf is the caller-provided output buffer.
 *
 * wrap_column is the wrap column, so that we don't create output that is
 * too wide. See pgpa_maybe_linebreak() and comments in pgpa_output_advice.
 */
typedef struct pgpa_output_context
{
	const char **rid_strings;
	StringInfo	buf;
	int			wrap_column;
} pgpa_output_context;

static void pgpa_output_unrolled_join(pgpa_output_context *context,
									  pgpa_unrolled_join *join);
static void pgpa_output_join_member(pgpa_output_context *context,
									pgpa_join_member *member);
static void pgpa_output_scan_strategy(pgpa_output_context *context,
									  pgpa_scan_strategy strategy,
									  List *scans);
static void pgpa_output_relation_name(pgpa_output_context *context, Oid relid);
static void pgpa_output_query_feature(pgpa_output_context *context,
									  pgpa_qf_type type,
									  List *query_features);
static void pgpa_output_simple_strategy(pgpa_output_context *context,
										char *strategy,
										List *relid_sets);
static void pgpa_output_no_gather(pgpa_output_context *context,
								  Bitmapset *relids);
static void pgpa_output_do_not_scan(pgpa_output_context *context,
									List *identifiers);
static void pgpa_output_relations(pgpa_output_context *context, StringInfo buf,
								  Bitmapset *relids);

static char *pgpa_cstring_join_strategy(pgpa_join_strategy strategy);
static char *pgpa_cstring_scan_strategy(pgpa_scan_strategy strategy);
static char *pgpa_cstring_query_feature_type(pgpa_qf_type type);

static void pgpa_maybe_linebreak(StringInfo buf, int wrap_column);

/*
 * Append query advice to the provided buffer.
 *
 * Before calling this function, 'walker' must be used to iterate over the
 * main plan tree and all subplans from the PlannedStmt.
 *
 * 'rt_identifiers' is a table of unique identifiers, one for each RTI.
 * See pgpa_create_identifiers_for_planned_stmt().
 *
 * Results will be appended to 'buf'.
 */
void
pgpa_output_advice(StringInfo buf, pgpa_plan_walker_context *walker,
				   pgpa_identifier *rt_identifiers)
{
	Index		rtable_length = list_length(walker->pstmt->rtable);
	ListCell   *lc;
	pgpa_output_context context;

	/* Basic initialization. */
	memset(&context, 0, sizeof(pgpa_output_context));
	context.buf = buf;

	/*
	 * Convert identifiers to string form. Note that the loop variable here is
	 * not an RTI, because RTIs are 1-based. Some RTIs will have no
	 * identifier, either because the reloptkind is RTE_JOIN or because that
	 * portion of the query didn't make it into the final plan.
	 */
	context.rid_strings = palloc0_array(const char *, rtable_length);
	for (int i = 0; i < rtable_length; ++i)
		if (rt_identifiers[i].alias_name != NULL)
			context.rid_strings[i] = pgpa_identifier_string(&rt_identifiers[i]);

	/*
	 * If the user chooses to use EXPLAIN (PLAN_ADVICE) in an 80-column window
	 * from a psql client with default settings, psql will add one space to
	 * the left of the output and EXPLAIN will add two more to the left of the
	 * advice. Thus, lines of more than 77 characters will wrap. We set the
	 * wrap limit to 76 here so that the output won't reach all the way to the
	 * very last column of the terminal.
	 *
	 * Of course, this is fairly arbitrary set of assumptions, and one could
	 * well make an argument for a different wrap limit, or for a configurable
	 * one.
	 */
	context.wrap_column = 76;

	/*
	 * Each piece of JOIN_ORDER() advice fully describes the join order for a
	 * single unrolled join. Merging is not permitted, because that would
	 * change the meaning, e.g. SEQ_SCAN(a b c d) means simply that sequential
	 * scans should be used for all of those relations, and is thus equivalent
	 * to SEQ_SCAN(a b) SEQ_SCAN(c d), but JOIN_ORDER(a b c d) means that "a"
	 * is the driving table which is then joined to "b" then "c" then "d",
	 * which is totally different from JOIN_ORDER(a b) and JOIN_ORDER(c d).
	 */
	foreach(lc, walker->toplevel_unrolled_joins)
	{
		pgpa_unrolled_join *ujoin = lfirst(lc);

		if (buf->len > 0)
			appendStringInfoChar(buf, '\n');
		appendStringInfoString(context.buf, "JOIN_ORDER(");
		pgpa_output_unrolled_join(&context, ujoin);
		appendStringInfoChar(context.buf, ')');
		pgpa_maybe_linebreak(context.buf, context.wrap_column);
	}

	/* Emit join strategy advice. */
	for (int s = 0; s < NUM_PGPA_JOIN_STRATEGY; ++s)
	{
		char	   *strategy = pgpa_cstring_join_strategy(s);

		pgpa_output_simple_strategy(&context,
									strategy,
									walker->join_strategies[s]);
	}

	/*
	 * Emit scan strategy advice (but not for ordinary scans, which are
	 * definitionally uninteresting).
	 */
	for (int c = 0; c < NUM_PGPA_SCAN_STRATEGY; ++c)
		if (c != PGPA_SCAN_ORDINARY)
			pgpa_output_scan_strategy(&context, c, walker->scans[c]);

	/* Emit query feature advice. */
	for (int t = 0; t < NUM_PGPA_QF_TYPES; ++t)
		pgpa_output_query_feature(&context, t, walker->query_features[t]);

	/* Emit NO_GATHER advice. */
	pgpa_output_no_gather(&context, walker->no_gather_scans);

	/* Emit DO_NOT_SCAN advice. */
	pgpa_output_do_not_scan(&context, walker->do_not_scan_identifiers);
}

/*
 * Output the members of an unrolled join, first the outermost member, and
 * then the inner members one by one, as part of JOIN_ORDER() advice.
 */
static void
pgpa_output_unrolled_join(pgpa_output_context *context,
						  pgpa_unrolled_join *join)
{
	pgpa_output_join_member(context, &join->outer);

	for (int k = 0; k < join->ninner; ++k)
	{
		pgpa_join_member *member = &join->inner[k];

		pgpa_maybe_linebreak(context->buf, context->wrap_column);
		appendStringInfoChar(context->buf, ' ');
		pgpa_output_join_member(context, member);
	}
}

/*
 * Output a single member of an unrolled join as part of JOIN_ORDER() advice.
 */
static void
pgpa_output_join_member(pgpa_output_context *context,
						pgpa_join_member *member)
{
	if (member->unrolled_join != NULL)
	{
		appendStringInfoChar(context->buf, '(');
		pgpa_output_unrolled_join(context, member->unrolled_join);
		appendStringInfoChar(context->buf, ')');
	}
	else
	{
		pgpa_scan  *scan = member->scan;

		Assert(scan != NULL);
		if (bms_membership(scan->relids) == BMS_SINGLETON)
			pgpa_output_relations(context, context->buf, scan->relids);
		else
		{
			appendStringInfoChar(context->buf, '{');
			pgpa_output_relations(context, context->buf, scan->relids);
			appendStringInfoChar(context->buf, '}');
		}
	}
}

/*
 * Output advice for a List of pgpa_scan objects.
 *
 * All the scans must use the strategy specified by the "strategy" argument.
 */
static void
pgpa_output_scan_strategy(pgpa_output_context *context,
						  pgpa_scan_strategy strategy,
						  List *scans)
{
	bool		first = true;

	if (scans == NIL)
		return;

	if (context->buf->len > 0)
		appendStringInfoChar(context->buf, '\n');
	appendStringInfo(context->buf, "%s(",
					 pgpa_cstring_scan_strategy(strategy));

	foreach_ptr(pgpa_scan, scan, scans)
	{
		Plan	   *plan = scan->plan;

		if (first)
			first = false;
		else
		{
			pgpa_maybe_linebreak(context->buf, context->wrap_column);
			appendStringInfoChar(context->buf, ' ');
		}

		/* Output the relation identifiers. */
		if (bms_membership(scan->relids) == BMS_SINGLETON)
			pgpa_output_relations(context, context->buf, scan->relids);
		else
		{
			appendStringInfoChar(context->buf, '(');
			pgpa_output_relations(context, context->buf, scan->relids);
			appendStringInfoChar(context->buf, ')');
		}

		/* For index or index-only scans, output index information. */
		if (strategy == PGPA_SCAN_INDEX)
		{
			Assert(IsA(plan, IndexScan));
			pgpa_maybe_linebreak(context->buf, context->wrap_column);
			appendStringInfoChar(context->buf, ' ');
			pgpa_output_relation_name(context, ((IndexScan *) plan)->indexid);
		}
		else if (strategy == PGPA_SCAN_INDEX_ONLY)
		{
			Assert(IsA(plan, IndexOnlyScan));
			pgpa_maybe_linebreak(context->buf, context->wrap_column);
			appendStringInfoChar(context->buf, ' ');
			pgpa_output_relation_name(context,
									  ((IndexOnlyScan *) plan)->indexid);
		}
	}

	appendStringInfoChar(context->buf, ')');
	pgpa_maybe_linebreak(context->buf, context->wrap_column);
}

/*
 * Output a schema-qualified relation name.
 */
static void
pgpa_output_relation_name(pgpa_output_context *context, Oid relid)
{
	Oid			nspoid = get_rel_namespace(relid);
	char	   *relnamespace = get_namespace_name_or_temp(nspoid);
	char	   *relname = get_rel_name(relid);

	appendStringInfoString(context->buf, quote_identifier(relnamespace));
	appendStringInfoChar(context->buf, '.');
	appendStringInfoString(context->buf, quote_identifier(relname));
}

/*
 * Output advice for a List of pgpa_query_feature objects.
 *
 * All features must be of the type specified by the "type" argument.
 */
static void
pgpa_output_query_feature(pgpa_output_context *context, pgpa_qf_type type,
						  List *query_features)
{
	bool		first = true;

	if (query_features == NIL)
		return;

	if (context->buf->len > 0)
		appendStringInfoChar(context->buf, '\n');
	appendStringInfo(context->buf, "%s(",
					 pgpa_cstring_query_feature_type(type));

	foreach_ptr(pgpa_query_feature, qf, query_features)
	{
		if (first)
			first = false;
		else
		{
			pgpa_maybe_linebreak(context->buf, context->wrap_column);
			appendStringInfoChar(context->buf, ' ');
		}

		if (bms_membership(qf->relids) == BMS_SINGLETON)
			pgpa_output_relations(context, context->buf, qf->relids);
		else
		{
			appendStringInfoChar(context->buf, '(');
			pgpa_output_relations(context, context->buf, qf->relids);
			appendStringInfoChar(context->buf, ')');
		}
	}

	appendStringInfoChar(context->buf, ')');
	pgpa_maybe_linebreak(context->buf, context->wrap_column);
}

/*
 * Output "simple" advice for a List of Bitmapset objects each of which
 * contains one or more RTIs.
 *
 * By simple, we just mean that the advice emitted follows the most
 * straightforward pattern: the strategy name, followed by a list of items
 * separated by spaces and surrounded by parentheses. Individual items in
 * the list are a single relation identifier for a Bitmapset that contains
 * just one member, or a sub-list again separated by spaces and surrounded
 * by parentheses for a Bitmapset with multiple members. Bitmapsets with
 * no members probably shouldn't occur here, but if they do they'll be
 * rendered as an empty sub-list.
 */
static void
pgpa_output_simple_strategy(pgpa_output_context *context, char *strategy,
							List *relid_sets)
{
	bool		first = true;

	if (relid_sets == NIL)
		return;

	if (context->buf->len > 0)
		appendStringInfoChar(context->buf, '\n');
	appendStringInfo(context->buf, "%s(", strategy);

	foreach_node(Bitmapset, relids, relid_sets)
	{
		if (first)
			first = false;
		else
		{
			pgpa_maybe_linebreak(context->buf, context->wrap_column);
			appendStringInfoChar(context->buf, ' ');
		}

		if (bms_membership(relids) == BMS_SINGLETON)
			pgpa_output_relations(context, context->buf, relids);
		else
		{
			appendStringInfoChar(context->buf, '(');
			pgpa_output_relations(context, context->buf, relids);
			appendStringInfoChar(context->buf, ')');
		}
	}

	appendStringInfoChar(context->buf, ')');
	pgpa_maybe_linebreak(context->buf, context->wrap_column);
}

/*
 * Output NO_GATHER advice for all relations not appearing beneath any
 * Gather or Gather Merge node.
 */
static void
pgpa_output_no_gather(pgpa_output_context *context, Bitmapset *relids)
{
	if (relids == NULL)
		return;
	if (context->buf->len > 0)
		appendStringInfoChar(context->buf, '\n');
	appendStringInfoString(context->buf, "NO_GATHER(");
	pgpa_output_relations(context, context->buf, relids);
	appendStringInfoChar(context->buf, ')');
}

/*
 * Output DO_NOT_SCAN advice for all relations in the provided list of
 * identifiers.
 */
static void
pgpa_output_do_not_scan(pgpa_output_context *context, List *identifiers)
{
	bool		first = true;

	if (identifiers == NIL)
		return;
	if (context->buf->len > 0)
		appendStringInfoChar(context->buf, '\n');
	appendStringInfoString(context->buf, "DO_NOT_SCAN(");

	foreach_ptr(pgpa_identifier, rid, identifiers)
	{
		if (first)
			first = false;
		else
		{
			pgpa_maybe_linebreak(context->buf, context->wrap_column);
			appendStringInfoChar(context->buf, ' ');
		}
		appendStringInfoString(context->buf, pgpa_identifier_string(rid));
	}

	appendStringInfoChar(context->buf, ')');
}

/*
 * Output the identifiers for each RTI in the provided set.
 *
 * Identifiers are separated by spaces, and a line break is possible after
 * each one.
 */
static void
pgpa_output_relations(pgpa_output_context *context, StringInfo buf,
					  Bitmapset *relids)
{
	int			rti = -1;
	bool		first = true;

	while ((rti = bms_next_member(relids, rti)) >= 0)
	{
		const char *rid_string = context->rid_strings[rti - 1];

		if (rid_string == NULL)
			elog(ERROR, "no identifier for RTI %d", rti);

		if (first)
		{
			first = false;
			appendStringInfoString(buf, rid_string);
		}
		else
		{
			pgpa_maybe_linebreak(buf, context->wrap_column);
			appendStringInfo(buf, " %s", rid_string);
		}
	}
}

/*
 * Get a C string that corresponds to the specified join strategy.
 */
static char *
pgpa_cstring_join_strategy(pgpa_join_strategy strategy)
{
	switch (strategy)
	{
		case JSTRAT_MERGE_JOIN_PLAIN:
			return "MERGE_JOIN_PLAIN";
		case JSTRAT_MERGE_JOIN_MATERIALIZE:
			return "MERGE_JOIN_MATERIALIZE";
		case JSTRAT_NESTED_LOOP_PLAIN:
			return "NESTED_LOOP_PLAIN";
		case JSTRAT_NESTED_LOOP_MATERIALIZE:
			return "NESTED_LOOP_MATERIALIZE";
		case JSTRAT_NESTED_LOOP_MEMOIZE:
			return "NESTED_LOOP_MEMOIZE";
		case JSTRAT_HASH_JOIN:
			return "HASH_JOIN";
	}

	pg_unreachable();
	return NULL;
}

/*
 * Get a C string that corresponds to the specified scan strategy.
 */
static char *
pgpa_cstring_scan_strategy(pgpa_scan_strategy strategy)
{
	switch (strategy)
	{
		case PGPA_SCAN_ORDINARY:
			return "ORDINARY_SCAN";
		case PGPA_SCAN_SEQ:
			return "SEQ_SCAN";
		case PGPA_SCAN_BITMAP_HEAP:
			return "BITMAP_HEAP_SCAN";
		case PGPA_SCAN_FOREIGN:
			return "FOREIGN_JOIN";
		case PGPA_SCAN_INDEX:
			return "INDEX_SCAN";
		case PGPA_SCAN_INDEX_ONLY:
			return "INDEX_ONLY_SCAN";
		case PGPA_SCAN_PARTITIONWISE:
			return "PARTITIONWISE";
		case PGPA_SCAN_TID:
			return "TID_SCAN";
	}

	pg_unreachable();
	return NULL;
}

/*
 * Get a C string that corresponds to the query feature type.
 */
static char *
pgpa_cstring_query_feature_type(pgpa_qf_type type)
{
	switch (type)
	{
		case PGPAQF_GATHER:
			return "GATHER";
		case PGPAQF_GATHER_MERGE:
			return "GATHER_MERGE";
		case PGPAQF_SEMIJOIN_NON_UNIQUE:
			return "SEMIJOIN_NON_UNIQUE";
		case PGPAQF_SEMIJOIN_UNIQUE:
			return "SEMIJOIN_UNIQUE";
	}


	pg_unreachable();
	return NULL;
}

/*
 * Insert a line break into the StringInfoData, if needed.
 *
 * If wrap_column is zero or negative, this does nothing. Otherwise, we
 * consider inserting a newline. We only insert a newline if the length of
 * the last line in the buffer exceeds wrap_column, and not if we'd be
 * inserting a newline at or before the beginning of the current line.
 *
 * The position at which the newline is inserted is simply wherever the
 * buffer ended the last time this function was called. In other words,
 * the caller is expected to call this function every time we reach a good
 * place for a line break.
 */
static void
pgpa_maybe_linebreak(StringInfo buf, int wrap_column)
{
	char	   *trailing_nl;
	int			line_start;
	int			save_cursor;

	/* If line wrapping is disabled, exit quickly. */
	if (wrap_column <= 0)
		return;

	/*
	 * Set line_start to the byte offset within buf->data of the first
	 * character of the current line, where the current line means the last
	 * one in the buffer. Note that line_start could be the offset of the
	 * trailing '\0' if the last character in the buffer is a line break.
	 */
	trailing_nl = strrchr(buf->data, '\n');
	if (trailing_nl == NULL)
		line_start = 0;
	else
		line_start = (trailing_nl - buf->data) + 1;

	/*
	 * Remember that the current end of the buffer is a potential location to
	 * insert a line break on a future call to this function.
	 */
	save_cursor = buf->cursor;
	buf->cursor = buf->len;

	/* If we haven't passed the wrap column, we don't need a newline. */
	if (buf->len - line_start <= wrap_column)
		return;

	/*
	 * It only makes sense to insert a newline at a position later than the
	 * beginning of the current line.
	 */
	if (save_cursor <= line_start)
		return;

	/* Insert a newline at the previous cursor location. */
	enlargeStringInfo(buf, 1);
	memmove(&buf->data[save_cursor] + 1, &buf->data[save_cursor],
			buf->len - save_cursor);
	++buf->cursor;
	buf->data[++buf->len] = '\0';
	buf->data[save_cursor] = '\n';
}
