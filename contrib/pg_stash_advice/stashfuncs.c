/*-------------------------------------------------------------------------
 *
 * stashfuncs.c
 *	  SQL interface to pg_stash_advice
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_stash_advice/stashfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pg_stash_advice.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

PG_FUNCTION_INFO_V1(pg_create_advice_stash);
PG_FUNCTION_INFO_V1(pg_drop_advice_stash);
PG_FUNCTION_INFO_V1(pg_get_advice_stash_contents);
PG_FUNCTION_INFO_V1(pg_get_advice_stashes);
PG_FUNCTION_INFO_V1(pg_set_stashed_advice);
PG_FUNCTION_INFO_V1(pg_start_stash_advice_worker);

typedef struct pgsa_stash_count
{
	uint32		status;
	uint64		pgsa_stash_id;
	int64		num_entries;
} pgsa_stash_count;

#define SH_PREFIX pgsa_stash_count_table
#define SH_ELEMENT_TYPE pgsa_stash_count
#define SH_KEY_TYPE uint64
#define SH_KEY pgsa_stash_id
#define SH_HASH_KEY(tb, key) hash_bytes((const unsigned char *) &(key), sizeof(uint64))
#define SH_EQUAL(tb, a, b) (a == b)
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

/*
 * SQL-callable function to create an advice stash
 */
Datum
pg_create_advice_stash(PG_FUNCTION_ARGS)
{
	char	   *stash_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	pgsa_check_stash_name(stash_name);
	if (unlikely(pgsa_entry_dshash == NULL))
		pgsa_attach();
	pgsa_check_lockout();
	LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
	pgsa_create_stash(stash_name);
	LWLockRelease(&pgsa_state->lock);
	PG_RETURN_VOID();
}

/*
 * SQL-callable function to drop an advice stash
 */
Datum
pg_drop_advice_stash(PG_FUNCTION_ARGS)
{
	char	   *stash_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	pgsa_check_stash_name(stash_name);
	if (unlikely(pgsa_entry_dshash == NULL))
		pgsa_attach();
	pgsa_check_lockout();
	LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
	pgsa_drop_stash(stash_name);
	LWLockRelease(&pgsa_state->lock);
	PG_RETURN_VOID();
}

/*
 * SQL-callable function to provide a list of advice stashes
 */
Datum
pg_get_advice_stashes(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	dshash_seq_status iterator;
	pgsa_entry *entry;
	pgsa_stash *stash;
	pgsa_stash_count_table_hash *chash;

	InitMaterializedSRF(fcinfo, 0);

	/* Attach to dynamic shared memory if not already done. */
	if (unlikely(pgsa_entry_dshash == NULL))
		pgsa_attach();

	/* If stash data is still being restored from disk, ignore. */
	if (pg_atomic_unlocked_test_flag(&pgsa_state->stashes_ready))
		return (Datum) 0;

	/* Tally up the number of entries per stash. */
	chash = pgsa_stash_count_table_create(CurrentMemoryContext, 64, NULL);
	dshash_seq_init(&iterator, pgsa_entry_dshash, true);
	while ((entry = dshash_seq_next(&iterator)) != NULL)
	{
		pgsa_stash_count *c;
		bool		found;

		c = pgsa_stash_count_table_insert(chash,
										  entry->key.pgsa_stash_id,
										  &found);
		if (!found)
			c->num_entries = 1;
		else
			c->num_entries++;
	}
	dshash_seq_term(&iterator);

	/* Emit results. */
	dshash_seq_init(&iterator, pgsa_stash_dshash, true);
	while ((stash = dshash_seq_next(&iterator)) != NULL)
	{
		Datum		values[2];
		bool		nulls[2];
		pgsa_stash_count *c;

		values[0] = CStringGetTextDatum(stash->name);
		nulls[0] = false;

		c = pgsa_stash_count_table_lookup(chash, stash->pgsa_stash_id);
		values[1] = Int64GetDatum(c == NULL ? 0 : c->num_entries);
		nulls[1] = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
							 nulls);
	}
	dshash_seq_term(&iterator);

	return (Datum) 0;
}

/*
 * SQL-callable function to provide advice stash contents
 */
Datum
pg_get_advice_stash_contents(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	dshash_seq_status iterator;
	char	   *stash_name = NULL;
	pgsa_stash_name_table_hash *nhash = NULL;
	uint64		stash_id = 0;
	pgsa_entry *entry;

	InitMaterializedSRF(fcinfo, 0);

	/* Attach to dynamic shared memory if not already done. */
	if (unlikely(pgsa_entry_dshash == NULL))
		pgsa_attach();

	/* If stash data is still being restored from disk, ignore. */
	if (pg_atomic_unlocked_test_flag(&pgsa_state->stashes_ready))
		return (Datum) 0;

	/* User can pass NULL for all stashes, or the name of a specific stash. */
	if (!PG_ARGISNULL(0))
	{
		stash_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
		pgsa_check_stash_name(stash_name);
		stash_id = pgsa_lookup_stash_id(stash_name);

		/* If the user specified a stash name, it should exist. */
		if (stash_id == 0)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					errmsg("advice stash \"%s\" does not exist", stash_name));
	}
	else
	{
		pgsa_stash *stash;

		/*
		 * If we're dumping data about all stashes, we need an ID->name lookup
		 * table.
		 */
		nhash = pgsa_stash_name_table_create(CurrentMemoryContext, 64, NULL);
		dshash_seq_init(&iterator, pgsa_stash_dshash, true);
		while ((stash = dshash_seq_next(&iterator)) != NULL)
		{
			pgsa_stash_name *n;
			bool		found;

			n = pgsa_stash_name_table_insert(nhash,
											 stash->pgsa_stash_id,
											 &found);
			Assert(!found);
			n->name = pstrdup(stash->name);
		}
		dshash_seq_term(&iterator);
	}

	/* Now iterate over all the entries. */
	dshash_seq_init(&iterator, pgsa_entry_dshash, false);
	while ((entry = dshash_seq_next(&iterator)) != NULL)
	{
		Datum		values[3];
		bool		nulls[3];
		char	   *this_stash_name;
		char	   *advice_string;

		/* Skip incomplete entries where the advice string was never set. */
		if (entry->advice_string == InvalidDsaPointer)
			continue;

		if (stash_id != 0)
		{
			/*
			 * We're only dumping data for one particular stash, so skip
			 * entries for any other stash and use the stash name specified by
			 * the user.
			 */
			if (stash_id != entry->key.pgsa_stash_id)
				continue;
			this_stash_name = stash_name;
		}
		else
		{
			pgsa_stash_name *n;

			/*
			 * We're dumping data for all stashes, so look up the correct name
			 * to use in the hash table. If nothing is found, which is
			 * possible due to race conditions, make up a string to use.
			 */
			n = pgsa_stash_name_table_lookup(nhash, entry->key.pgsa_stash_id);
			if (n != NULL)
				this_stash_name = n->name;
			else
				this_stash_name = psprintf("<stash %" PRIu64 ">",
										   entry->key.pgsa_stash_id);
		}

		/* Work out tuple values. */
		values[0] = CStringGetTextDatum(this_stash_name);
		nulls[0] = false;
		values[1] = Int64GetDatum(entry->key.queryId);
		nulls[1] = false;
		advice_string = dsa_get_address(pgsa_dsa_area, entry->advice_string);
		values[2] = CStringGetTextDatum(advice_string);
		nulls[2] = false;

		/* Emit the tuple. */
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values,
							 nulls);
	}
	dshash_seq_term(&iterator);

	return (Datum) 0;
}

/*
 * SQL-callable function to update an advice stash entry for a particular
 * query ID
 *
 * If the second argument is NULL, we delete any existing advice stash
 * entry; otherwise, we either create an entry or update it with the new
 * advice string.
 */
Datum
pg_set_stashed_advice(PG_FUNCTION_ARGS)
{
	char	   *stash_name;
	int64		queryId;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	/* Get and check advice stash name. */
	stash_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	pgsa_check_stash_name(stash_name);

	/*
	 * Get and check query ID.
	 *
	 * Query ID 0 means no query ID was computed, so reject that.
	 */
	queryId = PG_GETARG_INT64(1);
	if (queryId == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("cannot set advice string for query ID 0"));

	/* Attach to dynamic shared memory if not already done. */
	if (unlikely(pgsa_entry_dshash == NULL))
		pgsa_attach();

	/* Don't allow writes if stash data is still being restored from disk. */
	pgsa_check_lockout();

	/* Now call the appropriate function to do the real work. */
	if (PG_ARGISNULL(2))
	{
		LWLockAcquire(&pgsa_state->lock, LW_SHARED);
		pgsa_clear_advice_string(stash_name, queryId);
		LWLockRelease(&pgsa_state->lock);
	}
	else
	{
		char	   *advice_string = text_to_cstring(PG_GETARG_TEXT_PP(2));

		LWLockAcquire(&pgsa_state->lock, LW_SHARED);
		pgsa_set_advice_string(stash_name, queryId, advice_string);
		LWLockRelease(&pgsa_state->lock);
	}

	PG_RETURN_VOID();
}

/*
 * SQL-callable function to start the persistence background worker.
 */
Datum
pg_start_stash_advice_worker(PG_FUNCTION_ARGS)
{
	pid_t		pid;

	if (unlikely(pgsa_entry_dshash == NULL))
		pgsa_attach();

	LWLockAcquire(&pgsa_state->lock, LW_SHARED);
	pid = pgsa_state->bgworker_pid;
	LWLockRelease(&pgsa_state->lock);

	if (pid != InvalidPid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_stash_advice worker is already running under PID %d",
						(int) pid)));

	pgsa_start_worker();

	PG_RETURN_VOID();
}
