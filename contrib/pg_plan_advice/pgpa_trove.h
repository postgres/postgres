/*-------------------------------------------------------------------------
 *
 * pgpa_trove.h
 *	  All of the advice given for a particular query, appropriately
 *    organized for convenient access.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_trove.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPA_TROVE_H
#define PGPA_TROVE_H

#include "pgpa_ast.h"

#include "nodes/bitmapset.h"

typedef struct pgpa_trove pgpa_trove;

/*
 * Each entry in a trove of advice represents the application of a tag to
 * a single target.
 */
typedef struct pgpa_trove_entry
{
	pgpa_advice_tag_type tag;
	pgpa_advice_target *target;
	int			flags;
} pgpa_trove_entry;

/*
 * What kind of information does the caller want to find in a trove?
 *
 * PGPA_TROVE_LOOKUP_SCAN means we're looking for scan advice.
 *
 * PGPA_TROVE_LOOKUP_JOIN means we're looking for join-related advice.
 * This includes join order advice, join method advice, and semijoin-uniqueness
 * advice.
 *
 * PGPA_TROVE_LOOKUP_REL means we're looking for general advice about this
 * a RelOptInfo that may correspond to either a scan or a join. This includes
 * gather-related advice and partitionwise advice. Note that partitionwise
 * advice might seem like join advice, but that's not a helpful way of viewing
 * the matter because (1) partitionwise advice is also relevant at the scan
 * level and (2) other types of join advice affect only what to do from
 * join_path_setup_hook, but partitionwise advice affects what to do in
 * joinrel_setup_hook.
 */
typedef enum pgpa_trove_lookup_type
{
	PGPA_TROVE_LOOKUP_JOIN,
	PGPA_TROVE_LOOKUP_REL,
	PGPA_TROVE_LOOKUP_SCAN
} pgpa_trove_lookup_type;

/*
 * This struct is used to store the result of a trove lookup. For each member
 * of "indexes", the entry at the corresponding offset within "entries" is one
 * of the results.
 */
typedef struct pgpa_trove_result
{
	pgpa_trove_entry *entries;
	Bitmapset  *indexes;
} pgpa_trove_result;

extern pgpa_trove *pgpa_build_trove(List *advice_items);
extern void pgpa_trove_lookup(pgpa_trove *trove,
							  pgpa_trove_lookup_type type,
							  int nrids,
							  pgpa_identifier *rids,
							  pgpa_trove_result *result);
extern void pgpa_trove_lookup_all(pgpa_trove *trove,
								  pgpa_trove_lookup_type type,
								  pgpa_trove_entry **entries,
								  int *nentries);
extern char *pgpa_cstring_trove_entry(pgpa_trove_entry *entry);
extern void pgpa_trove_set_flags(pgpa_trove_entry *entries,
								 Bitmapset *indexes, int flags);
extern void pgpa_trove_append_flags(StringInfo buf, int flags);

#endif
