/*------------------------------------------------------------------------------------
 *
 * test_custom_var_stats.c
 *		Test module for variable-sized custom pgstats
 *
 * Copyright (c) 2025-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_custom_var_stats/test_custom_var_stats.c
 *
 * ------------------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "funcapi.h"
#include "storage/dsm_registry.h"
#include "utils/builtins.h"
#include "utils/pgstat_internal.h"

PG_MODULE_MAGIC_EXT(
					.name = "test_custom_var_stats",
					.version = PG_VERSION
);

#define TEST_CUSTOM_VAR_MAGIC_NUMBER (0xBEEFBEEF)

/*--------------------------------------------------------------------------
 * Macros and constants
 *--------------------------------------------------------------------------
 */

/*
 * Kind ID for test_custom_var_stats statistics.
 */
#define PGSTAT_KIND_TEST_CUSTOM_VAR_STATS 25

/* File paths for auxiliary data serialization */
#define TEST_CUSTOM_AUX_DATA_DESC "pg_stat/test_custom_var_stats_desc.stats"

/*
 * Hash statistic name to generate entry index for pgstat lookup.
 */
#define PGSTAT_CUSTOM_VAR_STATS_IDX(name) hash_bytes_extended((const unsigned char *) name, strlen(name), 0)

/*--------------------------------------------------------------------------
 * Type definitions
 *--------------------------------------------------------------------------
 */

/* Backend-local pending statistics before flush to shared memory */
typedef struct PgStat_StatCustomVarEntry
{
	PgStat_Counter numcalls;	/* times statistic was incremented */
} PgStat_StatCustomVarEntry;

/* Shared memory statistics entry visible to all backends */
typedef struct PgStatShared_CustomVarEntry
{
	PgStatShared_Common header; /* standard pgstat entry header */
	PgStat_StatCustomVarEntry stats;	/* custom statistics data */
	dsa_pointer description;	/* pointer to description string in DSA */
} PgStatShared_CustomVarEntry;

/*--------------------------------------------------------------------------
 * Global Variables
 *--------------------------------------------------------------------------
 */

/* File handle for auxiliary data serialization */
static FILE *fd_description = NULL;

/* Current write offset in fd_description file */
static pgoff_t fd_description_offset = 0;

/* DSA area for storing variable-length description strings */
static dsa_area *custom_stats_description_dsa = NULL;

/*--------------------------------------------------------------------------
 * Function prototypes
 *--------------------------------------------------------------------------
 */

/* Flush callback: merge pending stats into shared memory */
static bool test_custom_stats_var_flush_pending_cb(PgStat_EntryRef *entry_ref,
												   bool nowait);

/* Serialization callback: write auxiliary entry data */
static void test_custom_stats_var_to_serialized_data(const PgStat_HashKey *key,
													 const PgStatShared_Common *header,
													 FILE *statfile);

/* Deserialization callback: read auxiliary entry data */
static bool test_custom_stats_var_from_serialized_data(const PgStat_HashKey *key,
													   PgStatShared_Common *header,
													   FILE *statfile);

/* Finish callback: end of statistics file operations */
static void test_custom_stats_var_finish(PgStat_StatsFileOp status);

/*--------------------------------------------------------------------------
 * Custom kind configuration
 *--------------------------------------------------------------------------
 */

static const PgStat_KindInfo custom_stats = {
	.name = "test_custom_var_stats",
	.fixed_amount = false,		/* variable number of entries */
	.write_to_file = true,		/* persist across restarts */
	.track_entry_count = true,	/* count active entries */
	.accessed_across_databases = true,	/* global statistics */
	.shared_size = sizeof(PgStatShared_CustomVarEntry),
	.shared_data_off = offsetof(PgStatShared_CustomVarEntry, stats),
	.shared_data_len = sizeof(((PgStatShared_CustomVarEntry *) 0)->stats),
	.pending_size = sizeof(PgStat_StatCustomVarEntry),
	.flush_pending_cb = test_custom_stats_var_flush_pending_cb,
	.to_serialized_data = test_custom_stats_var_to_serialized_data,
	.from_serialized_data = test_custom_stats_var_from_serialized_data,
	.finish = test_custom_stats_var_finish,
};

/*--------------------------------------------------------------------------
 * Module initialization
 *--------------------------------------------------------------------------
 */

void
_PG_init(void)
{
	/* Must be loaded via shared_preload_libraries */
	if (!process_shared_preload_libraries_in_progress)
		return;

	/* Register custom statistics kind */
	pgstat_register_kind(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, &custom_stats);
}

/*--------------------------------------------------------------------------
 * Statistics callback functions
 *--------------------------------------------------------------------------
 */

/*
 * test_custom_stats_var_flush_pending_cb
 *		Merge pending backend statistics into shared memory
 *
 * Called by pgstat collector to flush accumulated local statistics
 * to shared memory where other backends can read them.
 *
 * Returns false only if nowait=true and lock acquisition fails.
 */
static bool
test_custom_stats_var_flush_pending_cb(PgStat_EntryRef *entry_ref, bool nowait)
{
	PgStat_StatCustomVarEntry *pending_entry;
	PgStatShared_CustomVarEntry *shared_entry;

	pending_entry = (PgStat_StatCustomVarEntry *) entry_ref->pending;
	shared_entry = (PgStatShared_CustomVarEntry *) entry_ref->shared_stats;

	if (!pgstat_lock_entry(entry_ref, nowait))
		return false;

	/* Add pending counts to shared totals */
	shared_entry->stats.numcalls += pending_entry->numcalls;

	pgstat_unlock_entry(entry_ref);

	return true;
}

/*
 * test_custom_stats_var_to_serialized_data() -
 *
 * Serialize auxiliary data (descriptions) for custom statistics entries
 * to a secondary statistics file. This is called while writing the statistics
 * to disk.
 *
 * This callback writes a mix of data within the main pgstats file and a
 * secondary statistics file.  The following data is written to the main file for
 * each entry:
 * - An arbitrary magic number.
 * - An offset.  This is used to know the location we need to look at
 * to retrieve the information from the second file.
 *
 * The following data is written to the secondary statistics file:
 * - The entry key, cross-checked with the data from the main file
 * when reloaded.
 * - The length of the description.
 * - The description data itself.
 */
static void
test_custom_stats_var_to_serialized_data(const PgStat_HashKey *key,
										 const PgStatShared_Common *header,
										 FILE *statfile)
{
	char	   *description;
	size_t		len;
	const PgStatShared_CustomVarEntry *entry = (const PgStatShared_CustomVarEntry *) header;
	bool		found;
	uint32		magic_number = TEST_CUSTOM_VAR_MAGIC_NUMBER;

	/*
	 * First mark the main file with a magic number, keeping a trace that some
	 * auxiliary data will exist in the secondary statistics file.
	 */
	pgstat_write_chunk_s(statfile, &magic_number);

	/* Open statistics file for writing. */
	if (!fd_description)
	{
		fd_description = AllocateFile(TEST_CUSTOM_AUX_DATA_DESC, PG_BINARY_W);
		if (fd_description == NULL)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open statistics file \"%s\" for writing: %m",
							TEST_CUSTOM_AUX_DATA_DESC)));
			return;
		}

		/* Initialize offset for secondary statistics file. */
		fd_description_offset = 0;
	}

	/* Write offset to the main data file */
	pgstat_write_chunk_s(statfile, &fd_description_offset);

	/*
	 * First write the entry key to the secondary statistics file.  This will
	 * be cross-checked with the key read from main stats file at loading
	 * time.
	 */
	pgstat_write_chunk_s(fd_description, (PgStat_HashKey *) key);
	fd_description_offset += sizeof(PgStat_HashKey);

	if (!custom_stats_description_dsa)
		custom_stats_description_dsa = GetNamedDSA("test_custom_stat_dsa", &found);

	/* Handle entries without descriptions */
	if (!DsaPointerIsValid(entry->description) || !custom_stats_description_dsa)
	{
		/* length to description file */
		len = 0;
		pgstat_write_chunk_s(fd_description, &len);
		fd_description_offset += sizeof(size_t);
		return;
	}

	/*
	 * Retrieve description from DSA, then write the length followed by the
	 * description.
	 */
	description = dsa_get_address(custom_stats_description_dsa,
								  entry->description);
	len = strlen(description) + 1;
	pgstat_write_chunk_s(fd_description, &len);
	pgstat_write_chunk(fd_description, description, len);

	/*
	 * Update offset for next entry, counting for the length (size_t) of the
	 * description and the description contents.
	 */
	fd_description_offset += len + sizeof(size_t);
}

/*
 * test_custom_stats_var_from_serialized_data() -
 *
 * Read auxiliary data (descriptions) for custom statistics entries from
 * the secondary statistics file.  This is called while loading the statistics
 * at startup.
 *
 * See the top of test_custom_stats_var_to_serialized_data() for a
 * detailed description of the data layout read here.
 */
static bool
test_custom_stats_var_from_serialized_data(const PgStat_HashKey *key,
										   PgStatShared_Common *header,
										   FILE *statfile)
{
	PgStatShared_CustomVarEntry *entry;
	dsa_pointer dp;
	size_t		len;
	pgoff_t		offset;
	char	   *buffer;
	bool		found;
	uint32		magic_number = 0;
	PgStat_HashKey file_key;

	/* Check the magic number first, in the main file. */
	if (!pgstat_read_chunk_s(statfile, &magic_number))
	{
		elog(WARNING, "failed to read magic number from statistics file");
		return false;
	}

	if (magic_number != TEST_CUSTOM_VAR_MAGIC_NUMBER)
	{
		elog(WARNING, "found magic number %u from statistics file, should be %u",
			 magic_number, TEST_CUSTOM_VAR_MAGIC_NUMBER);
		return false;
	}

	/*
	 * Read the offset from the main stats file, to be able to read the
	 * auxiliary data from the secondary statistics file.
	 */
	if (!pgstat_read_chunk_s(statfile, &offset))
	{
		elog(WARNING, "failed to read metadata offset from statistics file");
		return false;
	}

	/* Open statistics file for reading if not already open */
	if (!fd_description)
	{
		fd_description = AllocateFile(TEST_CUSTOM_AUX_DATA_DESC, PG_BINARY_R);
		if (fd_description == NULL)
		{
			if (errno != ENOENT)
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not open statistics file \"%s\" for reading: %m",
								TEST_CUSTOM_AUX_DATA_DESC)));
			pgstat_reset_of_kind(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS);
			return false;
		}
	}

	/* Read data from the secondary statistics file, at the specified offset */
	if (fseeko(fd_description, offset, SEEK_SET) != 0)
	{
		elog(WARNING, "could not seek in file \"%s\": %m",
			 TEST_CUSTOM_AUX_DATA_DESC);
		return false;
	}

	/* Read the hash key from the secondary statistics file */
	if (!pgstat_read_chunk_s(fd_description, &file_key))
	{
		elog(WARNING, "failed to read hash key from file");
		return false;
	}

	/* Check key consistency */
	if (file_key.kind != key->kind ||
		file_key.dboid != key->dboid ||
		file_key.objid != key->objid)
	{
		elog(WARNING, "found entry key %u/%u/%" PRIu64 " not matching with %u/%u/%" PRIu64,
			 file_key.kind, file_key.dboid, file_key.objid,
			 key->kind, key->dboid, key->objid);
		return false;
	}

	entry = (PgStatShared_CustomVarEntry *) header;

	/* Read the description length and its data */
	if (!pgstat_read_chunk_s(fd_description, &len))
	{
		elog(WARNING, "failed to read metadata length from statistics file");
		return false;
	}

	/* Handle empty descriptions */
	if (len == 0)
	{
		entry->description = InvalidDsaPointer;
		return true;
	}

	/* Initialize DSA if needed */
	if (!custom_stats_description_dsa)
		custom_stats_description_dsa = GetNamedDSA("test_custom_stat_dsa", &found);

	if (!custom_stats_description_dsa)
	{
		elog(WARNING, "could not access DSA for custom statistics descriptions");
		return false;
	}

	buffer = palloc(len);
	if (!pgstat_read_chunk(fd_description, buffer, len))
	{
		pfree(buffer);
		elog(WARNING, "failed to read description from file");
		return false;
	}

	/* Allocate space in DSA and copy the description */
	dp = dsa_allocate(custom_stats_description_dsa, len);
	memcpy(dsa_get_address(custom_stats_description_dsa, dp), buffer, len);
	entry->description = dp;
	pfree(buffer);

	return true;
}

/*
 * test_custom_stats_var_finish() -
 *
 * Cleanup function called at the end of statistics file operations.
 * Handles closing files and cleanup based on the operation type.
 */
static void
test_custom_stats_var_finish(PgStat_StatsFileOp status)
{
	switch (status)
	{
		case STATS_WRITE:
			if (!fd_description)
				return;

			fd_description_offset = 0;

			/* Check for write errors and cleanup if necessary */
			if (ferror(fd_description))
			{
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not write to file \"%s\": %m",
								TEST_CUSTOM_AUX_DATA_DESC)));
				FreeFile(fd_description);
				unlink(TEST_CUSTOM_AUX_DATA_DESC);
			}
			else if (FreeFile(fd_description) < 0)
			{
				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("could not close file \"%s\": %m",
								TEST_CUSTOM_AUX_DATA_DESC)));
				unlink(TEST_CUSTOM_AUX_DATA_DESC);
			}
			break;

		case STATS_READ:
			if (fd_description)
				FreeFile(fd_description);

			/* Remove the file after reading */
			elog(DEBUG2, "removing file \"%s\"", TEST_CUSTOM_AUX_DATA_DESC);
			unlink(TEST_CUSTOM_AUX_DATA_DESC);
			break;

		case STATS_DISCARD:
			{
				int			ret;

				/* Attempt to remove the file */
				ret = unlink(TEST_CUSTOM_AUX_DATA_DESC);
				if (ret != 0)
				{
					if (errno == ENOENT)
						elog(LOG,
							 "didn't need to unlink file \"%s\" - didn't exist",
							 TEST_CUSTOM_AUX_DATA_DESC);
					else
						ereport(LOG,
								(errcode_for_file_access(),
								 errmsg("could not unlink file \"%s\": %m",
										TEST_CUSTOM_AUX_DATA_DESC)));
				}
				else
				{
					ereport(LOG,
							(errmsg_internal("unlinked file \"%s\"",
											 TEST_CUSTOM_AUX_DATA_DESC)));
				}
			}
			break;
	}

	fd_description = NULL;
}

/*--------------------------------------------------------------------------
 * Helper functions
 *--------------------------------------------------------------------------
 */

/*
 * test_custom_stats_var_fetch_entry
 *		Look up custom statistic by name
 *
 * Returns statistics entry from shared memory, or NULL if not found.
 */
static PgStat_StatCustomVarEntry *
test_custom_stats_var_fetch_entry(const char *stat_name)
{
	/* Fetch entry by hashed name */
	return (PgStat_StatCustomVarEntry *)
		pgstat_fetch_entry(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS,
						   InvalidOid,
						   PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name));
}

/*--------------------------------------------------------------------------
 * SQL-callable functions
 *--------------------------------------------------------------------------
 */

/*
 * test_custom_stats_var_create
 *		Create new custom statistic entry
 *
 * Initializes a statistics entry with the given name and description.
 */
PG_FUNCTION_INFO_V1(test_custom_stats_var_create);
Datum
test_custom_stats_var_create(PG_FUNCTION_ARGS)
{
	PgStat_EntryRef *entry_ref;
	PgStatShared_CustomVarEntry *shared_entry;
	char	   *stat_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *description = text_to_cstring(PG_GETARG_TEXT_PP(1));
	dsa_pointer dp = InvalidDsaPointer;
	bool		found;

	/* Validate name length first */
	if (strlen(stat_name) >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("custom statistic name \"%s\" is too long", stat_name),
				 errdetail("Name must be less than %d characters.", NAMEDATALEN)));

	/* Initialize DSA and description provided */
	if (!custom_stats_description_dsa)
		custom_stats_description_dsa = GetNamedDSA("test_custom_stat_dsa", &found);

	if (!custom_stats_description_dsa)
		ereport(ERROR,
				(errmsg("could not access DSA for custom statistics descriptions")));

	/* Allocate space in DSA and copy description */
	dp = dsa_allocate(custom_stats_description_dsa, strlen(description) + 1);
	memcpy(dsa_get_address(custom_stats_description_dsa, dp),
		   description,
		   strlen(description) + 1);

	/* Create or get existing entry */
	entry_ref = pgstat_get_entry_ref_locked(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, InvalidOid,
											PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name), true);

	if (!entry_ref)
		PG_RETURN_VOID();

	shared_entry = (PgStatShared_CustomVarEntry *) entry_ref->shared_stats;

	/* Zero-initialize statistics */
	memset(&shared_entry->stats, 0, sizeof(shared_entry->stats));

	/* Store description pointer */
	shared_entry->description = dp;

	pgstat_unlock_entry(entry_ref);

	PG_RETURN_VOID();
}

/*
 * test_custom_stats_var_update
 *		Increment custom statistic counter
 *
 * Increments call count in backend-local memory.  Changes are flushed
 * to shared memory by the statistics collector.
 */
PG_FUNCTION_INFO_V1(test_custom_stats_var_update);
Datum
test_custom_stats_var_update(PG_FUNCTION_ARGS)
{
	char	   *stat_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	PgStat_EntryRef *entry_ref;
	PgStat_StatCustomVarEntry *pending_entry;

	/* Get pending entry in local memory */
	entry_ref = pgstat_prep_pending_entry(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, InvalidOid,
										  PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name), NULL);

	pending_entry = (PgStat_StatCustomVarEntry *) entry_ref->pending;
	pending_entry->numcalls++;

	PG_RETURN_VOID();
}

/*
 * test_custom_stats_var_drop
 *		Remove custom statistic entry
 *
 * Drops the named statistic from shared memory.
 */
PG_FUNCTION_INFO_V1(test_custom_stats_var_drop);
Datum
test_custom_stats_var_drop(PG_FUNCTION_ARGS)
{
	char	   *stat_name = text_to_cstring(PG_GETARG_TEXT_PP(0));

	/* Drop entry and request GC if the entry could not be freed */
	if (!pgstat_drop_entry(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, InvalidOid,
						   PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name)))
		pgstat_request_entry_refs_gc();

	PG_RETURN_VOID();
}

/*
 * test_custom_stats_var_report
 *		Retrieve custom statistic values
 *
 * Returns single row with statistic name, call count, and description if the
 * statistic exists, otherwise returns no rows.
 */
PG_FUNCTION_INFO_V1(test_custom_stats_var_report);
Datum
test_custom_stats_var_report(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	char	   *stat_name;
	PgStat_StatCustomVarEntry *stat_entry;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		/* Initialize SRF context */
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* Get composite return type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			elog(ERROR, "test_custom_stats_var_report: return type is not composite");

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->max_calls = 1; /* single row result */

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		Datum		values[3];
		bool		nulls[3] = {false, false, false};
		HeapTuple	tuple;
		PgStat_EntryRef *entry_ref;
		PgStatShared_CustomVarEntry *shared_entry;
		char	   *description = NULL;
		bool		found;

		stat_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
		stat_entry = test_custom_stats_var_fetch_entry(stat_name);

		/* Return row only if entry exists */
		if (stat_entry)
		{
			/* Get entry ref to access shared entry */
			entry_ref = pgstat_get_entry_ref(PGSTAT_KIND_TEST_CUSTOM_VAR_STATS, InvalidOid,
											 PGSTAT_CUSTOM_VAR_STATS_IDX(stat_name), false, NULL);

			if (entry_ref)
			{
				shared_entry = (PgStatShared_CustomVarEntry *) entry_ref->shared_stats;

				/* Get description from DSA if available */
				if (DsaPointerIsValid(shared_entry->description))
				{
					if (!custom_stats_description_dsa)
						custom_stats_description_dsa = GetNamedDSA("test_custom_stat_dsa", &found);

					if (custom_stats_description_dsa)
						description = dsa_get_address(custom_stats_description_dsa, shared_entry->description);
				}
			}

			values[0] = PointerGetDatum(cstring_to_text(stat_name));
			values[1] = Int64GetDatum(stat_entry->numcalls);

			if (description)
				values[2] = PointerGetDatum(cstring_to_text(description));
			else
				nulls[2] = true;

			tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
			SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
		}
	}

	SRF_RETURN_DONE(funcctx);
}
