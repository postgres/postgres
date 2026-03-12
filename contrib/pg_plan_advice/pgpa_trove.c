/*-------------------------------------------------------------------------
 *
 * pgpa_trove.c
 *	  All of the advice given for a particular query, appropriately
 *    organized for convenient access.
 *
 * This name comes from the English expression "trove of advice", which
 * means a collection of wisdom. This slightly unusual term is chosen
 * partly because it seems to fit and partly because it's not presently
 * used for anything else, making it easy to grep. Note that, while we
 * don't know whether the provided advice is actually wise, it's not our
 * job to question the user's choices.
 *
 * The goal of this module is to make it easy to locate the specific
 * bits of advice that pertain to any given part of a query, or to
 * determine that there are none.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_plan_advice/pgpa_trove.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "pgpa_trove.h"

#include "common/hashfn_unstable.h"

/*
 * An advice trove is organized into a series of "slices", each of which
 * contains information about one topic e.g. scan methods. Each slice consists
 * of an array of trove entries plus a hash table that we can use to determine
 * which ones are relevant to a particular part of the query.
 */
typedef struct pgpa_trove_slice
{
	unsigned	nallocated;
	unsigned	nused;
	pgpa_trove_entry *entries;
	struct pgpa_trove_entry_hash *hash;
} pgpa_trove_slice;

/*
 * Scan advice is stored into 'scan'; join advice is stored into 'join'; and
 * advice that can apply to both cases is stored into 'rel'. This lets callers
 * ask just for what's relevant. These slices correspond to the possible values
 * of pgpa_trove_lookup_type.
 */
struct pgpa_trove
{
	pgpa_trove_slice join;
	pgpa_trove_slice rel;
	pgpa_trove_slice scan;
};

/*
 * We're going to build a hash table to allow clients of this module to find
 * relevant advice for a given part of the query quickly. However, we're going
 * to use only three of the five key fields as hash keys. There are two reasons
 * for this.
 *
 * First, it's allowable to set partition_schema to NULL to match a partition
 * with the correct name in any schema.
 *
 * Second, we expect the "occurrence" and "partition_schema" portions of the
 * relation identifiers to be mostly uninteresting. Most of the time, the
 * occurrence field will be 1 and the partition_schema values will all be the
 * same. Even when there is some variation, the absolute number of entries
 * that have the same values for all three of these key fields should be
 * quite small.
 */
typedef struct
{
	const char *alias_name;
	const char *partition_name;
	const char *plan_name;
} pgpa_trove_entry_key;

typedef struct
{
	pgpa_trove_entry_key key;
	int			status;
	Bitmapset  *indexes;
} pgpa_trove_entry_element;

static uint32 pgpa_trove_entry_hash_key(pgpa_trove_entry_key key);

static inline bool
pgpa_trove_entry_compare_key(pgpa_trove_entry_key a, pgpa_trove_entry_key b)
{
	if (strcmp(a.alias_name, b.alias_name) != 0)
		return false;

	if (!strings_equal_or_both_null(a.partition_name, b.partition_name))
		return false;

	if (!strings_equal_or_both_null(a.plan_name, b.plan_name))
		return false;

	return true;
}

#define SH_PREFIX			pgpa_trove_entry
#define SH_ELEMENT_TYPE		pgpa_trove_entry_element
#define SH_KEY_TYPE			pgpa_trove_entry_key
#define SH_KEY				key
#define SH_HASH_KEY(tb, key)	pgpa_trove_entry_hash_key(key)
#define	SH_EQUAL(tb, a, b)	pgpa_trove_entry_compare_key(a, b)
#define SH_SCOPE			static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

static void pgpa_init_trove_slice(pgpa_trove_slice *tslice);
static void pgpa_trove_add_to_slice(pgpa_trove_slice *tslice,
									pgpa_advice_tag_type tag,
									pgpa_advice_target *target);
static void pgpa_trove_add_to_hash(pgpa_trove_entry_hash *hash,
								   pgpa_advice_target *target,
								   int index);
static Bitmapset *pgpa_trove_slice_lookup(pgpa_trove_slice *tslice,
										  pgpa_identifier *rid);

/*
 * Build a trove of advice from a list of advice items.
 *
 * Caller can obtain a list of advice items to pass to this function by
 * calling pgpa_parse().
 */
pgpa_trove *
pgpa_build_trove(List *advice_items)
{
	pgpa_trove *trove = palloc_object(pgpa_trove);

	pgpa_init_trove_slice(&trove->join);
	pgpa_init_trove_slice(&trove->rel);
	pgpa_init_trove_slice(&trove->scan);

	foreach_ptr(pgpa_advice_item, item, advice_items)
	{
		switch (item->tag)
		{
			case PGPA_TAG_JOIN_ORDER:
				{
					pgpa_advice_target *target;

					/*
					 * For most advice types, each element in the top-level
					 * list is a separate target, but it's most convenient to
					 * regard the entirety of a JOIN_ORDER specification as a
					 * single target. Since it wasn't represented that way
					 * during parsing, build a surrogate object now.
					 */
					target = palloc0_object(pgpa_advice_target);
					target->ttype = PGPA_TARGET_ORDERED_LIST;
					target->children = item->targets;

					pgpa_trove_add_to_slice(&trove->join,
											item->tag, target);
				}
				break;

			case PGPA_TAG_BITMAP_HEAP_SCAN:
			case PGPA_TAG_INDEX_ONLY_SCAN:
			case PGPA_TAG_INDEX_SCAN:
			case PGPA_TAG_SEQ_SCAN:
			case PGPA_TAG_TID_SCAN:

				/*
				 * Scan advice.
				 */
				foreach_ptr(pgpa_advice_target, target, item->targets)
				{
					/*
					 * For now, all of our scan types target single relations,
					 * but in the future this might not be true, e.g. a custom
					 * scan could replace a join.
					 */
					Assert(target->ttype == PGPA_TARGET_IDENTIFIER);
					pgpa_trove_add_to_slice(&trove->scan,
											item->tag, target);
				}
				break;

			case PGPA_TAG_FOREIGN_JOIN:
			case PGPA_TAG_HASH_JOIN:
			case PGPA_TAG_MERGE_JOIN_MATERIALIZE:
			case PGPA_TAG_MERGE_JOIN_PLAIN:
			case PGPA_TAG_NESTED_LOOP_MATERIALIZE:
			case PGPA_TAG_NESTED_LOOP_MEMOIZE:
			case PGPA_TAG_NESTED_LOOP_PLAIN:
			case PGPA_TAG_SEMIJOIN_NON_UNIQUE:
			case PGPA_TAG_SEMIJOIN_UNIQUE:

				/*
				 * Join strategy advice.
				 */
				foreach_ptr(pgpa_advice_target, target, item->targets)
				{
					pgpa_trove_add_to_slice(&trove->join,
											item->tag, target);
				}
				break;

			case PGPA_TAG_PARTITIONWISE:
			case PGPA_TAG_GATHER:
			case PGPA_TAG_GATHER_MERGE:
			case PGPA_TAG_NO_GATHER:

				/*
				 * Advice about a RelOptInfo relevant to both scans and joins.
				 */
				foreach_ptr(pgpa_advice_target, target, item->targets)
				{
					pgpa_trove_add_to_slice(&trove->rel,
											item->tag, target);
				}
				break;
		}
	}

	return trove;
}

/*
 * Search a trove of advice for relevant entries.
 *
 * All parameters are input parameters except for *result, which is an output
 * parameter used to return results to the caller.
 */
void
pgpa_trove_lookup(pgpa_trove *trove, pgpa_trove_lookup_type type,
				  int nrids, pgpa_identifier *rids, pgpa_trove_result *result)
{
	pgpa_trove_slice *tslice;
	Bitmapset  *indexes;

	Assert(nrids > 0);

	if (type == PGPA_TROVE_LOOKUP_SCAN)
		tslice = &trove->scan;
	else if (type == PGPA_TROVE_LOOKUP_JOIN)
		tslice = &trove->join;
	else
		tslice = &trove->rel;

	indexes = pgpa_trove_slice_lookup(tslice, &rids[0]);
	for (int i = 1; i < nrids; ++i)
	{
		Bitmapset  *other_indexes;

		/*
		 * If the caller is asking about two relations that aren't part of the
		 * same subquery, they've messed up.
		 */
		Assert(strings_equal_or_both_null(rids[0].plan_name,
										  rids[i].plan_name));

		other_indexes = pgpa_trove_slice_lookup(tslice, &rids[i]);
		indexes = bms_union(indexes, other_indexes);
	}

	result->entries = tslice->entries;
	result->indexes = indexes;
}

/*
 * Return all entries in a trove slice to the caller.
 *
 * The first two arguments are input arguments, and the remainder are output
 * arguments.
 */
void
pgpa_trove_lookup_all(pgpa_trove *trove, pgpa_trove_lookup_type type,
					  pgpa_trove_entry **entries, int *nentries)
{
	pgpa_trove_slice *tslice;

	if (type == PGPA_TROVE_LOOKUP_SCAN)
		tslice = &trove->scan;
	else if (type == PGPA_TROVE_LOOKUP_JOIN)
		tslice = &trove->join;
	else
		tslice = &trove->rel;

	*entries = tslice->entries;
	*nentries = tslice->nused;
}

/*
 * Convert a trove entry to an item of plan advice that would produce it.
 */
char *
pgpa_cstring_trove_entry(pgpa_trove_entry *entry)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s", pgpa_cstring_advice_tag(entry->tag));

	/* JOIN_ORDER tags are transformed by pgpa_build_trove; undo that here */
	if (entry->tag != PGPA_TAG_JOIN_ORDER)
		appendStringInfoChar(&buf, '(');
	else
		Assert(entry->target->ttype == PGPA_TARGET_ORDERED_LIST);

	pgpa_format_advice_target(&buf, entry->target);

	if (entry->target->itarget != NULL)
	{
		appendStringInfoChar(&buf, ' ');
		pgpa_format_index_target(&buf, entry->target->itarget);
	}

	if (entry->tag != PGPA_TAG_JOIN_ORDER)
		appendStringInfoChar(&buf, ')');

	return buf.data;
}

/*
 * Set PGPA_TE_* flags on a set of trove entries.
 */
void
pgpa_trove_set_flags(pgpa_trove_entry *entries, Bitmapset *indexes, int flags)
{
	int			i = -1;

	while ((i = bms_next_member(indexes, i)) >= 0)
	{
		pgpa_trove_entry *entry = &entries[i];

		entry->flags |= flags;
	}
}

/*
 * Append a string representation of the specified PGPA_TE_* flags to the
 * given StringInfo.
 */
void
pgpa_trove_append_flags(StringInfo buf, int flags)
{
	if ((flags & PGPA_TE_MATCH_FULL) != 0)
	{
		Assert((flags & PGPA_TE_MATCH_PARTIAL) != 0);
		appendStringInfo(buf, "matched");
	}
	else if ((flags & PGPA_TE_MATCH_PARTIAL) != 0)
		appendStringInfo(buf, "partially matched");
	else
		appendStringInfo(buf, "not matched");
	if ((flags & PGPA_TE_INAPPLICABLE) != 0)
		appendStringInfo(buf, ", inapplicable");
	if ((flags & PGPA_TE_CONFLICTING) != 0)
		appendStringInfo(buf, ", conflicting");
	if ((flags & PGPA_TE_FAILED) != 0)
		appendStringInfo(buf, ", failed");
}

/*
 * Add a new advice target to an existing pgpa_trove_slice object.
 */
static void
pgpa_trove_add_to_slice(pgpa_trove_slice *tslice,
						pgpa_advice_tag_type tag,
						pgpa_advice_target *target)
{
	pgpa_trove_entry *entry;

	if (tslice->nused >= tslice->nallocated)
	{
		int			new_allocated;

		new_allocated = tslice->nallocated * 2;
		tslice->entries = repalloc_array(tslice->entries, pgpa_trove_entry,
										 new_allocated);
		tslice->nallocated = new_allocated;
	}

	entry = &tslice->entries[tslice->nused];
	entry->tag = tag;
	entry->target = target;
	entry->flags = 0;

	pgpa_trove_add_to_hash(tslice->hash, target, tslice->nused);

	tslice->nused++;
}

/*
 * Update the hash table for a newly-added advice target.
 */
static void
pgpa_trove_add_to_hash(pgpa_trove_entry_hash *hash, pgpa_advice_target *target,
					   int index)
{
	pgpa_trove_entry_key key;
	pgpa_trove_entry_element *element;
	bool		found;

	/* For non-identifiers, add entries for all descendants. */
	if (target->ttype != PGPA_TARGET_IDENTIFIER)
	{
		foreach_ptr(pgpa_advice_target, child_target, target->children)
		{
			pgpa_trove_add_to_hash(hash, child_target, index);
		}
		return;
	}

	/* Sanity checks. */
	Assert(target->rid.occurrence > 0);
	Assert(target->rid.alias_name != NULL);

	/* Add an entry for this relation identifier. */
	key.alias_name = target->rid.alias_name;
	key.partition_name = target->rid.partrel;
	key.plan_name = target->rid.plan_name;
	element = pgpa_trove_entry_insert(hash, key, &found);
	if (!found)
		element->indexes = NULL;
	element->indexes = bms_add_member(element->indexes, index);
}

/*
 * Create and initialize a new pgpa_trove_slice object.
 */
static void
pgpa_init_trove_slice(pgpa_trove_slice *tslice)
{
	/*
	 * In an ideal world, we'll make tslice->nallocated big enough that the
	 * array and hash table will be large enough to contain the number of
	 * advice items in this trove slice, but a generous default value is not
	 * good for performance, because pgpa_init_trove_slice() has to zero an
	 * amount of memory proportional to tslice->nallocated. Hence, we keep the
	 * starting value quite small, on the theory that advice strings will
	 * often be relatively short.
	 */
	tslice->nallocated = 16;
	tslice->nused = 0;
	tslice->entries = palloc_array(pgpa_trove_entry, tslice->nallocated);
	tslice->hash = pgpa_trove_entry_create(CurrentMemoryContext,
										   tslice->nallocated, NULL);
}

/*
 * Fast hash function for a key consisting of alias_name, partition_name,
 * and plan_name.
 */
static uint32
pgpa_trove_entry_hash_key(pgpa_trove_entry_key key)
{
	fasthash_state hs;
	int			sp_len;

	fasthash_init(&hs, 0);

	/* alias_name may not be NULL */
	sp_len = fasthash_accum_cstring(&hs, key.alias_name);

	/* partition_name and plan_name, however, can be NULL */
	if (key.partition_name != NULL)
		sp_len += fasthash_accum_cstring(&hs, key.partition_name);
	if (key.plan_name != NULL)
		sp_len += fasthash_accum_cstring(&hs, key.plan_name);

	/*
	 * hashfn_unstable.h recommends using string length as tweak. It's not
	 * clear to me what to do if there are multiple strings, so for now I'm
	 * just using the total of all of the lengths.
	 */
	return fasthash_final32(&hs, sp_len);
}

/*
 * Look for matching entries.
 */
static Bitmapset *
pgpa_trove_slice_lookup(pgpa_trove_slice *tslice, pgpa_identifier *rid)
{
	pgpa_trove_entry_key key;
	pgpa_trove_entry_element *element;
	Bitmapset  *result = NULL;

	Assert(rid->occurrence >= 1);

	key.alias_name = rid->alias_name;
	key.partition_name = rid->partrel;
	key.plan_name = rid->plan_name;

	element = pgpa_trove_entry_lookup(tslice->hash, key);

	if (element != NULL)
	{
		int			i = -1;

		while ((i = bms_next_member(element->indexes, i)) >= 0)
		{
			pgpa_trove_entry *entry = &tslice->entries[i];

			/*
			 * We know that this target or one of its descendants matches the
			 * identifier on the three key fields above, but we don't know
			 * which descendant or whether the occurrence and schema also
			 * match.
			 */
			if (pgpa_identifier_matches_target(rid, entry->target))
				result = bms_add_member(result, i);
		}
	}

	return result;
}
