/*-------------------------------------------------------------------------
 *
 * pg_stash_advice.c
 *	  core infrastructure for pg_stash_advice contrib module
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_stash_advice/pg_stash_advice.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "common/string.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
#include "pg_plan_advice.h"
#include "pg_stash_advice.h"
#include "postmaster/bgworker.h"
#include "storage/dsm_registry.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

/* Shared memory hash table parameters */
static dshash_parameters pgsa_stash_dshash_parameters = {
	NAMEDATALEN,
	sizeof(pgsa_stash),
	dshash_strcmp,
	dshash_strhash,
	dshash_strcpy,
	LWTRANCHE_INVALID			/* gets set at runtime */
};

static dshash_parameters pgsa_entry_dshash_parameters = {
	sizeof(pgsa_entry_key),
	sizeof(pgsa_entry),
	dshash_memcmp,
	dshash_memhash,
	dshash_memcpy,
	LWTRANCHE_INVALID			/* gets set at runtime */
};

/* GUC variables */
static char *pg_stash_advice_stash_name = "";
bool		pg_stash_advice_persist = true;
int			pg_stash_advice_persist_interval = 30;

/* Shared memory pointers */
pgsa_shared_state *pgsa_state;
dsa_area   *pgsa_dsa_area;
dshash_table *pgsa_stash_dshash;
dshash_table *pgsa_entry_dshash;

/* Other global variables */
static MemoryContext pg_stash_advice_mcxt;

/* Function prototypes */
static char *pgsa_advisor(PlannerGlobal *glob,
						  Query *parse,
						  const char *query_string,
						  int cursorOptions,
						  ExplainState *es);
static bool pgsa_check_stash_name_guc(char **newval, void **extra,
									  GucSource source);
static void pgsa_init_shared_state(void *ptr, void *arg);
static bool pgsa_is_identifier(char *str);

/* Stash name -> stash ID hash table */
#define SH_PREFIX pgsa_stash_name_table
#define SH_ELEMENT_TYPE pgsa_stash_name
#define SH_KEY_TYPE uint64
#define SH_KEY pgsa_stash_id
#define SH_HASH_KEY(tb, key) hash_bytes((const unsigned char *) &(key), sizeof(uint64))
#define SH_EQUAL(tb, a, b) (a == b)
#define SH_SCOPE extern
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * Initialize this module.
 */
void
_PG_init(void)
{
	void		(*add_advisor_fn) (pg_plan_advice_advisor_hook hook);

	/* If compute_query_id = 'auto', we would like query IDs. */
	EnableQueryId();

	/* Define our GUCs. */
	if (process_shared_preload_libraries_in_progress)
		DefineCustomBoolVariable("pg_stash_advice.persist",
								 "Save and restore advice stash contents across restarts.",
								 NULL,
								 &pg_stash_advice_persist,
								 true,
								 PGC_POSTMASTER,
								 0,
								 NULL,
								 NULL,
								 NULL);
	else
		pg_stash_advice_persist = false;

	DefineCustomIntVariable("pg_stash_advice.persist_interval",
							"Interval between advice stash saves, in seconds.",
							NULL,
							&pg_stash_advice_persist_interval,
							30,
							0,
							3600,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("pg_stash_advice.stash_name",
							   "Name of the advice stash to be used in this session.",
							   NULL,
							   &pg_stash_advice_stash_name,
							   "",
							   PGC_USERSET,
							   0,
							   pgsa_check_stash_name_guc,
							   NULL,
							   NULL);

	MarkGUCPrefixReserved("pg_stash_advice");

	/* Start the background worker for persistence, if enabled. */
	if (pg_stash_advice_persist)
		pgsa_start_worker();

	/* Tell pg_plan_advice that we want to provide advice strings. */
	add_advisor_fn =
		load_external_function("pg_plan_advice", "pg_plan_advice_add_advisor",
							   true, NULL);
	(*add_advisor_fn) (pgsa_advisor);
}

/*
 * Get the advice string that has been configured for this query, if any,
 * and return it. Otherwise, return NULL.
 */
static char *
pgsa_advisor(PlannerGlobal *glob, Query *parse,
			 const char *query_string, int cursorOptions,
			 ExplainState *es)
{
	pgsa_entry_key key;
	pgsa_entry *entry;
	char	   *advice_string;
	uint64		stash_id;

	/*
	 * Exit quickly if the stash name is empty or there's no query ID.
	 */
	if (pg_stash_advice_stash_name[0] == '\0' || parse->queryId == 0)
		return NULL;

	/* Attach to dynamic shared memory if not already done. */
	if (unlikely(pgsa_entry_dshash == NULL))
		pgsa_attach();

	/* If stash data is still being restored from disk, ignore. */
	if (pg_atomic_unlocked_test_flag(&pgsa_state->stashes_ready))
		return NULL;

	/*
	 * Translate pg_stash_advice.stash_name to an integer ID.
	 *
	 * pgsa_check_stash_name_guc() has already validated the advice stash
	 * name, so we don't need to call pgsa_check_stash_name() here.
	 */
	stash_id = pgsa_lookup_stash_id(pg_stash_advice_stash_name);
	if (stash_id == 0)
		return NULL;

	/*
	 * Look up the advice string for the given stash ID + query ID.
	 *
	 * If we find an advice string, we copy it into the current memory
	 * context, presumably short-lived, so that we can release the lock on the
	 * dshash entry. pg_plan_advice only needs the value to remain allocated
	 * long enough for it to be parsed, so this should be good enough.
	 */
	memset(&key, 0, sizeof(pgsa_entry_key));
	key.pgsa_stash_id = stash_id;
	key.queryId = parse->queryId;
	entry = dshash_find(pgsa_entry_dshash, &key, false);
	if (entry == NULL)
		return NULL;
	if (entry->advice_string == InvalidDsaPointer)
		advice_string = NULL;
	else
		advice_string = pstrdup(dsa_get_address(pgsa_dsa_area,
												entry->advice_string));
	dshash_release_lock(pgsa_entry_dshash, entry);

	/* If we found an advice string, emit a debug message. */
	if (advice_string != NULL)
		elog(DEBUG2, "supplying automatic advice for stash \"%s\", query ID %" PRId64 ": %s",
			 pg_stash_advice_stash_name, key.queryId, advice_string);

	return advice_string;
}

/*
 * Attach to various structures in dynamic shared memory.
 *
 * This function is designed to be resilient against errors. That is, if it
 * fails partway through, it should be possible to call it again, repeat no
 * work already completed, and potentially succeed or at least get further if
 * whatever caused the previous failure has been corrected.
 */
void
pgsa_attach(void)
{
	bool		found;
	MemoryContext oldcontext;

	/*
	 * Create a memory context to make sure that any control structures
	 * allocated in local memory are sufficiently persistent.
	 */
	if (pg_stash_advice_mcxt == NULL)
		pg_stash_advice_mcxt = AllocSetContextCreate(TopMemoryContext,
													 "pg_stash_advice",
													 ALLOCSET_DEFAULT_SIZES);
	oldcontext = MemoryContextSwitchTo(pg_stash_advice_mcxt);

	/* Attach to the fixed-size state object if not already done. */
	if (pgsa_state == NULL)
		pgsa_state = GetNamedDSMSegment("pg_stash_advice",
										sizeof(pgsa_shared_state),
										pgsa_init_shared_state,
										&found, NULL);

	/* Attach to the DSA area if not already done. */
	if (pgsa_dsa_area == NULL)
	{
		dsa_handle	area_handle;

		LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
		area_handle = pgsa_state->area;
		if (area_handle == DSA_HANDLE_INVALID)
		{
			pgsa_dsa_area = dsa_create(pgsa_state->dsa_tranche);
			dsa_pin(pgsa_dsa_area);
			pgsa_state->area = dsa_get_handle(pgsa_dsa_area);
			LWLockRelease(&pgsa_state->lock);
		}
		else
		{
			LWLockRelease(&pgsa_state->lock);
			pgsa_dsa_area = dsa_attach(area_handle);
		}
		dsa_pin_mapping(pgsa_dsa_area);
	}

	/* Attach to the stash_name->stash_id hash table if not already done. */
	if (pgsa_stash_dshash == NULL)
	{
		dshash_table_handle stash_handle;

		LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
		pgsa_stash_dshash_parameters.tranche_id = pgsa_state->stash_tranche;
		stash_handle = pgsa_state->stash_hash;
		if (stash_handle == DSHASH_HANDLE_INVALID)
		{
			pgsa_stash_dshash = dshash_create(pgsa_dsa_area,
											  &pgsa_stash_dshash_parameters,
											  NULL);
			pgsa_state->stash_hash =
				dshash_get_hash_table_handle(pgsa_stash_dshash);
			LWLockRelease(&pgsa_state->lock);
		}
		else
		{
			LWLockRelease(&pgsa_state->lock);
			pgsa_stash_dshash = dshash_attach(pgsa_dsa_area,
											  &pgsa_stash_dshash_parameters,
											  stash_handle, NULL);
		}
	}

	/* Attach to the entry hash table if not already done. */
	if (pgsa_entry_dshash == NULL)
	{
		dshash_table_handle entry_handle;

		LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
		pgsa_entry_dshash_parameters.tranche_id = pgsa_state->entry_tranche;
		entry_handle = pgsa_state->entry_hash;
		if (entry_handle == DSHASH_HANDLE_INVALID)
		{
			pgsa_entry_dshash = dshash_create(pgsa_dsa_area,
											  &pgsa_entry_dshash_parameters,
											  NULL);
			pgsa_state->entry_hash =
				dshash_get_hash_table_handle(pgsa_entry_dshash);
			LWLockRelease(&pgsa_state->lock);
		}
		else
		{
			LWLockRelease(&pgsa_state->lock);
			pgsa_entry_dshash = dshash_attach(pgsa_dsa_area,
											  &pgsa_entry_dshash_parameters,
											  entry_handle, NULL);
		}
	}

	/* Restore previous memory context. */
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Error out if the stashes have not been loaded from disk yet.
 */
void
pgsa_check_lockout(void)
{
	if (pg_atomic_unlocked_test_flag(&pgsa_state->stashes_ready))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("stash modifications are not allowed because \"%s\" has not been loaded yet",
						PGSA_DUMP_FILE)));
}

/*
 * Check whether an advice stash name is legal, and signal an error if not.
 *
 * Keep this in sync with pgsa_check_stash_name_guc, below.
 */
void
pgsa_check_stash_name(char *stash_name)
{
	/* Reject empty advice stash name. */
	if (stash_name[0] == '\0')
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("advice stash name may not be zero length"));

	/* Reject overlong advice stash names. */
	if (strlen(stash_name) + 1 > NAMEDATALEN)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("advice stash names may not be longer than %d bytes",
					   NAMEDATALEN - 1));

	/*
	 * Reject non-ASCII advice stash names, since advice stashes are visible
	 * across all databases and the encodings of those databases might differ.
	 */
	if (!pg_is_ascii(stash_name))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("advice stash name must not contain non-ASCII characters"));

	/*
	 * Reject things that do not look like identifiers, since the ability to
	 * create an advice stash with non-printable characters or weird symbols
	 * in the name is not likely to be useful to anyone.
	 */
	if (!pgsa_is_identifier(stash_name))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("advice stash name must begin with a letter or underscore and contain only letters, digits, and underscores"));
}

/*
 * As above, but for the GUC check_hook. We allow the empty string here,
 * though, as equivalent to disabling the feature.
 */
static bool
pgsa_check_stash_name_guc(char **newval, void **extra, GucSource source)
{
	char	   *stash_name = *newval;

	/* Reject overlong advice stash names. */
	if (strlen(stash_name) + 1 > NAMEDATALEN)
	{
		GUC_check_errcode(ERRCODE_INVALID_PARAMETER_VALUE);
		GUC_check_errdetail("advice stash names may not be longer than %d bytes",
							NAMEDATALEN - 1);
		return false;
	}

	/*
	 * Reject non-ASCII advice stash names, since advice stashes are visible
	 * across all databases and the encodings of those databases might differ.
	 */
	if (!pg_is_ascii(stash_name))
	{
		GUC_check_errcode(ERRCODE_INVALID_PARAMETER_VALUE);
		GUC_check_errdetail("advice stash name must not contain non-ASCII characters");
		return false;
	}

	/*
	 * Reject things that do not look like identifiers, since the ability to
	 * create an advice stash with non-printable characters or weird symbols
	 * in the name is not likely to be useful to anyone.
	 */
	if (!pgsa_is_identifier(stash_name))
	{
		GUC_check_errcode(ERRCODE_INVALID_PARAMETER_VALUE);
		GUC_check_errdetail("advice stash name must begin with a letter or underscore and contain only letters, digits, and underscores");
		return false;
	}

	return true;
}

/*
 * Create an advice stash.
 */
void
pgsa_create_stash(char *stash_name)
{
	pgsa_stash *stash;
	bool		found;

	Assert(LWLockHeldByMeInMode(&pgsa_state->lock, LW_EXCLUSIVE));

	/* Create a stash with this name, unless one already exists. */
	stash = dshash_find_or_insert(pgsa_stash_dshash, stash_name, &found);
	if (found)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("advice stash \"%s\" already exists", stash_name));
	stash->pgsa_stash_id = pgsa_state->next_stash_id++;
	dshash_release_lock(pgsa_stash_dshash, stash);

	/* Bump change count. */
	pg_atomic_add_fetch_u64(&pgsa_state->change_count, 1);
}

/*
 * Remove any stored advice string for the given advice stash and query ID.
 */
void
pgsa_clear_advice_string(char *stash_name, int64 queryId)
{
	pgsa_entry *entry;
	pgsa_entry_key key;
	uint64		stash_id;
	dsa_pointer old_dp;

	Assert(LWLockHeldByMe(&pgsa_state->lock));

	/* Translate the stash name to an integer ID. */
	if ((stash_id = pgsa_lookup_stash_id(stash_name)) == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("advice stash \"%s\" does not exist", stash_name));

	/*
	 * Look for an existing entry, and free it. But, be sure to save the
	 * pointer to the associated advice string, if any.
	 */
	memset(&key, 0, sizeof(pgsa_entry_key));
	key.pgsa_stash_id = stash_id;
	key.queryId = queryId;
	entry = dshash_find(pgsa_entry_dshash, &key, true);
	if (entry == NULL)
		old_dp = InvalidDsaPointer;
	else
	{
		old_dp = entry->advice_string;
		dshash_delete_entry(pgsa_entry_dshash, entry);
	}

	/* Now we free the advice string as well, if there was one. */
	if (old_dp != InvalidDsaPointer)
		dsa_free(pgsa_dsa_area, old_dp);

	/* Bump change count. */
	pg_atomic_add_fetch_u64(&pgsa_state->change_count, 1);
}

/*
 * Drop an advice stash.
 */
void
pgsa_drop_stash(char *stash_name)
{
	pgsa_entry *entry;
	pgsa_stash *stash;
	dshash_seq_status iterator;
	uint64		stash_id;

	Assert(LWLockHeldByMeInMode(&pgsa_state->lock, LW_EXCLUSIVE));

	/* Remove the entry for this advice stash. */
	stash = dshash_find(pgsa_stash_dshash, stash_name, true);
	if (stash == NULL)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("advice stash \"%s\" does not exist", stash_name));
	stash_id = stash->pgsa_stash_id;
	dshash_delete_entry(pgsa_stash_dshash, stash);

	/*
	 * Now remove all the entries. Since pgsa_state->lock must be held at
	 * least in shared mode to insert entries into pgsa_entry_dshash, it
	 * doesn't matter whether we do this before or after deleting the entry
	 * from pgsa_stash_dshash.
	 */
	dshash_seq_init(&iterator, pgsa_entry_dshash, true);
	while ((entry = dshash_seq_next(&iterator)) != NULL)
	{
		if (stash_id == entry->key.pgsa_stash_id)
		{
			if (entry->advice_string != InvalidDsaPointer)
				dsa_free(pgsa_dsa_area, entry->advice_string);
			dshash_delete_current(&iterator);
		}
	}
	dshash_seq_term(&iterator);

	/* Bump change count. */
	pg_atomic_add_fetch_u64(&pgsa_state->change_count, 1);
}

/*
 * Remove all stashes and entries from shared memory.
 *
 * This is intended to be called before reloading from a dump file, so that
 * a failed previous attempt doesn't leave stale data behind.
 */
void
pgsa_reset_all_stashes(void)
{
	dshash_seq_status iter;
	pgsa_entry *entry;

	Assert(LWLockHeldByMeInMode(&pgsa_state->lock, LW_EXCLUSIVE));

	/* Remove all stashes. */
	dshash_seq_init(&iter, pgsa_stash_dshash, true);
	while (dshash_seq_next(&iter) != NULL)
		dshash_delete_current(&iter);
	dshash_seq_term(&iter);

	/* Remove all entries. */
	dshash_seq_init(&iter, pgsa_entry_dshash, true);
	while ((entry = dshash_seq_next(&iter)) != NULL)
	{
		if (entry->advice_string != InvalidDsaPointer)
			dsa_free(pgsa_dsa_area, entry->advice_string);
		dshash_delete_current(&iter);
	}
	dshash_seq_term(&iter);

	/* Reset the stash ID counter. */
	pgsa_state->next_stash_id = UINT64CONST(1);
}

/*
 * Initialize shared state when first created.
 */
static void
pgsa_init_shared_state(void *ptr, void *arg)
{
	pgsa_shared_state *state = (pgsa_shared_state *) ptr;

	LWLockInitialize(&state->lock,
					 LWLockNewTrancheId("pg_stash_advice_lock"));
	state->dsa_tranche = LWLockNewTrancheId("pg_stash_advice_dsa");
	state->stash_tranche = LWLockNewTrancheId("pg_stash_advice_stash");
	state->entry_tranche = LWLockNewTrancheId("pg_stash_advice_entry");
	state->next_stash_id = UINT64CONST(1);
	state->area = DSA_HANDLE_INVALID;
	state->stash_hash = DSHASH_HANDLE_INVALID;
	state->entry_hash = DSHASH_HANDLE_INVALID;
	state->bgworker_pid = InvalidPid;
	pg_atomic_init_flag(&state->stashes_ready);
	pg_atomic_init_u64(&state->change_count, 0);

	/*
	 * If this module was loaded via shared_preload_libraries, then
	 * pg_stash_advice_persist is a GUC variable. If it's true, that means
	 * that we should lock out manual stash modifications until the dump file
	 * has been successfully loaded. If it's false, there's nothing to load,
	 * so we set stashes_ready immediately.
	 *
	 * If this module was not loaded via shared_preload_libraries, then
	 * pg_stash_advice_persist is not a GUC variable, but it will be false,
	 * which leads to the correct behavior.
	 */
	if (!pg_stash_advice_persist)
		pg_atomic_test_set_flag(&state->stashes_ready);
}

/*
 * Check whether a string looks like a valid identifier. It must contain only
 * ASCII identifier characters, and must not begin with a digit.
 */
static bool
pgsa_is_identifier(char *str)
{
	if (*str >= '0' && *str <= '9')
		return false;

	while (*str != '\0')
	{
		char		c = *str++;

		if ((c < '0' || c > '9') && (c < 'a' || c > 'z') &&
			(c < 'A' || c > 'Z') && c != '_')
			return false;
	}

	return true;
}

/*
 * Look up the integer ID that corresponds to the given stash name.
 *
 * Returns 0 if no such stash exists.
 */
uint64
pgsa_lookup_stash_id(char *stash_name)
{
	pgsa_stash *stash;
	uint64		stash_id;

	/* Search the shared hash table. */
	stash = dshash_find(pgsa_stash_dshash, stash_name, false);
	if (stash == NULL)
		return 0;
	stash_id = stash->pgsa_stash_id;
	dshash_release_lock(pgsa_stash_dshash, stash);

	return stash_id;
}

/*
 * Store a new or updated advice string for the given advice stash and query ID.
 */
void
pgsa_set_advice_string(char *stash_name, int64 queryId, char *advice_string)
{
	pgsa_entry *entry;
	bool		found;
	pgsa_entry_key key;
	uint64		stash_id;
	dsa_pointer new_dp;
	dsa_pointer old_dp;

	/*
	 * The caller must hold our lock, at least in shared mode.  This is
	 * important for two reasons.
	 *
	 * First, it holds off interrupts, so that we can't bail out of this code
	 * after allocating DSA memory for the advice string and before storing
	 * the resulting pointer somewhere that others can find it.
	 *
	 * Second, we need to avoid a race against pgsa_drop_stash(). That
	 * function removes a stash_name->stash_id mapping and all the entries for
	 * that stash_id. Without the lock, there's a race condition no matter
	 * which of those things it does first, because as soon as we've looked up
	 * the stash ID, that whole function can execute before we do the rest of
	 * our work, which would result in us adding an entry for a stash that no
	 * longer exists.
	 */
	Assert(LWLockHeldByMe(&pgsa_state->lock));

	/* Look up the stash ID. */
	if ((stash_id = pgsa_lookup_stash_id(stash_name)) == 0)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("advice stash \"%s\" does not exist", stash_name));

	/* Allocate space for the advice string. */
	new_dp = dsa_allocate(pgsa_dsa_area, strlen(advice_string) + 1);
	strcpy(dsa_get_address(pgsa_dsa_area, new_dp), advice_string);

	/* Attempt to insert an entry into the hash table. */
	memset(&key, 0, sizeof(pgsa_entry_key));
	key.pgsa_stash_id = stash_id;
	key.queryId = queryId;
	entry = dshash_find_or_insert_extended(pgsa_entry_dshash, &key, &found,
										   DSHASH_INSERT_NO_OOM);

	/*
	 * If it didn't work, bail out, being careful to free the shared memory
	 * we've already allocated before, since error cleanup will not do so.
	 */
	if (entry == NULL)
	{
		dsa_free(pgsa_dsa_area, new_dp);
		ereport(ERROR,
				errcode(ERRCODE_OUT_OF_MEMORY),
				errmsg("out of memory"),
				errdetail("could not insert advice string into shared hash table"));
	}

	/* Update the entry and release the lock. */
	old_dp = found ? entry->advice_string : InvalidDsaPointer;
	entry->advice_string = new_dp;
	dshash_release_lock(pgsa_entry_dshash, entry);

	/*
	 * We're not safe from leaks yet!
	 *
	 * There's now a pointer to new_dp in the entry that we just updated, but
	 * that means that there's no longer anything pointing to old_dp.
	 */
	if (DsaPointerIsValid(old_dp))
		dsa_free(pgsa_dsa_area, old_dp);

	/* Bump change count. */
	pg_atomic_add_fetch_u64(&pgsa_state->change_count, 1);
}

/*
 * Start our worker process.
 */
void
pgsa_start_worker(void)
{
	BackgroundWorker worker = {0};
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_ConsistentState;
	worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
	strcpy(worker.bgw_library_name, "pg_stash_advice");
	strcpy(worker.bgw_function_name, "pg_stash_advice_worker_main");
	strcpy(worker.bgw_name, "pg_stash_advice worker");
	strcpy(worker.bgw_type, "pg_stash_advice worker");

	/*
	 * If process_shared_preload_libraries_in_progress = true, we may be in
	 * the postmaster, in which case this will really register the worker, or
	 * we may be in a child process in an EXEC_BACKEND build, in which case it
	 * will silently do nothing (which is the correct behavior).
	 */
	if (process_shared_preload_libraries_in_progress)
	{
		RegisterBackgroundWorker(&worker);
		return;
	}

	/*
	 * If process_shared_preload_libraries_in_progress = false, we're being
	 * asked to start the worker after system startup time. In other words,
	 * unless this is single-user mode, we're not in the postmaster, so we
	 * should use RegisterDynamicBackgroundWorker and then wait for startup to
	 * complete. (If we do happen to be in single-user mode, this will error
	 * out, which is fine.)
	 */
	worker.bgw_notify_pid = MyProcPid;
	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not register background process"),
				 errhint("You may need to increase \"max_worker_processes\".")));
	status = WaitForBackgroundWorkerStartup(handle, &pid);
	if (status != BGWH_STARTED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
				 errhint("More details may be available in the server log.")));
}
