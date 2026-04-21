/*-------------------------------------------------------------------------
 *
 * stashpersist.c
 *	  Persistence support for pg_stash_advice.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 *	  contrib/pg_stash_advice/stashpersist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>

#include "common/hashfn.h"
#include "miscadmin.h"
#include "pg_stash_advice.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

typedef struct pgsa_writer_context
{
	char		pathname[MAXPGPATH];
	FILE	   *file;
	pgsa_stash_name_table_hash *nhash;
	StringInfoData buf;
	int			entries_written;
} pgsa_writer_context;

/*
 * A parsed entry line, with pointers into the slurp buffer.
 */
typedef struct pgsa_saved_entry
{
	char	   *stash_name;
	int64		queryId;
	char	   *advice_string;
} pgsa_saved_entry;

/*
 * simplehash for detecting duplicate stash names during parsing.
 * Keyed by stash name (char *), pointing into the slurp buffer.
 */
typedef struct pgsa_saved_stash
{
	uint32		status;
	char	   *name;
} pgsa_saved_stash;

#define SH_PREFIX pgsa_saved_stash_table
#define SH_ELEMENT_TYPE pgsa_saved_stash
#define SH_KEY_TYPE char *
#define SH_KEY name
#define SH_HASH_KEY(tb, key) hash_bytes((const unsigned char *) (key), strlen(key))
#define SH_EQUAL(tb, a, b) (strcmp(a, b) == 0)
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

extern PGDLLEXPORT void pg_stash_advice_worker_main(Datum main_arg);
static void pgsa_append_tsv_escaped_string(StringInfo buf, const char *str);
static void pgsa_detach_shmem(int code, Datum arg);
static char *pgsa_next_tsv_field(char **cursor);
static void pgsa_read_from_disk(void);
static void pgsa_restore_entries(pgsa_saved_entry *entries, int num_entries);
static void pgsa_restore_stashes(pgsa_saved_stash_table_hash *saved_stashes);
static void pgsa_unescape_tsv_field(char *str, const char *filename,
									unsigned lineno);
static void pgsa_write_entries(pgsa_writer_context *wctx);
pg_noreturn static void pgsa_write_error(pgsa_writer_context *wctx);
static void pgsa_write_stashes(pgsa_writer_context *wctx);
static void pgsa_write_to_disk(void);

/*
 * Background worker entry point for pg_stash_advice persistence.
 *
 * On startup, if stashes_ready is set, we load previously saved
 * stash data from disk.  Then we enter a loop, periodically checking whether
 * any changes have been made (via the change_count atomic counter) and
 * writing them to disk.  On shutdown, we perform a final write.
 */
PGDLLEXPORT void
pg_stash_advice_worker_main(Datum main_arg)
{
	uint64		last_change_count;
	TimestampTz last_write_time = 0;

	/* Establish signal handlers; once that's done, unblock signals. */
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	BackgroundWorkerUnblockSignals();

	/* Log a debug message */
	ereport(DEBUG1,
			errmsg("pg_stash_advice worker started"));

	/* Set up session user so pgstat can report it. */
	InitializeSessionUserIdStandalone();

	/* Report this worker in pg_stat_activity. */
	pgstat_beinit();
	pgstat_bestart_initial();
	pgstat_bestart_final();

	/* Attach to shared memory structures. */
	pgsa_attach();

	/* Set on-detach hook so that our PID will be cleared on exit. */
	before_shmem_exit(pgsa_detach_shmem, 0);

	/*
	 * Store our PID in shared memory, unless there's already another worker
	 * running, in which case just exit.
	 */
	LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
	if (pgsa_state->bgworker_pid != InvalidPid)
	{
		LWLockRelease(&pgsa_state->lock);
		ereport(LOG,
				(errmsg("pg_stash_advice worker is already running under PID %d",
						(int) pgsa_state->bgworker_pid)));
		return;
	}
	pgsa_state->bgworker_pid = MyProcPid;
	LWLockRelease(&pgsa_state->lock);

	/*
	 * If pg_stash_advice.persist was set to true during
	 * process_shared_preload_libraries() and the data has not yet been
	 * successfully loaded, load it now.
	 */
	if (pg_atomic_unlocked_test_flag(&pgsa_state->stashes_ready))
	{
		pgsa_read_from_disk();
		pg_atomic_test_set_flag(&pgsa_state->stashes_ready);
	}

	/* Note the current change count so we can detect future changes. */
	last_change_count = pg_atomic_read_u64(&pgsa_state->change_count);

	/* Periodically write to disk until terminated. */
	while (!ShutdownRequestPending)
	{
		/* In case of a SIGHUP, just reload the configuration. */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (pg_stash_advice_persist_interval <= 0)
		{
			/* Only writing at shutdown, so just wait forever. */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_EXIT_ON_PM_DEATH,
							 -1L,
							 PG_WAIT_EXTENSION);
		}
		else
		{
			TimestampTz next_write_time;
			long		delay_in_ms;
			uint64		current_change_count;

			/* Compute when the next write should happen. */
			next_write_time =
				TimestampTzPlusMilliseconds(last_write_time,
											pg_stash_advice_persist_interval * 1000);
			delay_in_ms =
				TimestampDifferenceMilliseconds(GetCurrentTimestamp(),
												next_write_time);

			/*
			 * When we reach next_write_time, we always update last_write_time
			 * (which is really the time at which we last considered writing),
			 * but we only actually write to disk if something has changed.
			 */
			if (delay_in_ms <= 0)
			{
				current_change_count =
					pg_atomic_read_u64(&pgsa_state->change_count);
				if (current_change_count != last_change_count)
				{
					pgsa_write_to_disk();
					last_change_count = current_change_count;
				}
				last_write_time = GetCurrentTimestamp();
				continue;
			}

			/* Sleep until the next write time. */
			(void) WaitLatch(MyLatch,
							 WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 delay_in_ms,
							 PG_WAIT_EXTENSION);
		}

		ResetLatch(MyLatch);
	}

	/* Write one last time before exiting. */
	pgsa_write_to_disk();
}

/*
 * Clear our PID from shared memory on exit.
 */
static void
pgsa_detach_shmem(int code, Datum arg)
{
	LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
	if (pgsa_state->bgworker_pid == MyProcPid)
		pgsa_state->bgworker_pid = InvalidPid;
	LWLockRelease(&pgsa_state->lock);
}

/*
 * Load advice stash data from a dump file on disk, if there is one.
 */
static void
pgsa_read_from_disk(void)
{
	struct stat statbuf;
	FILE	   *file;
	char	   *filebuf;
	size_t		nread;
	char	   *p;
	unsigned	lineno;
	pgsa_saved_stash_table_hash *saved_stashes;
	int			num_stashes = 0;
	pgsa_saved_entry *entries;
	int			num_entries = 0;
	int			max_entries = 64;
	MemoryContext tmpcxt;
	MemoryContext oldcxt;

	Assert(pgsa_entry_dshash != NULL);

	/*
	 * Clear any existing shared memory state.
	 *
	 * Normally, there won't be any, but if this function was called before
	 * and failed after beginning to apply changes to shared memory, then we
	 * need to get rid of any entries created at that time before trying
	 * again.
	 */
	LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
	pgsa_reset_all_stashes();
	LWLockRelease(&pgsa_state->lock);

	/* Open the dump file. If it doesn't exist, we're done. */
	file = AllocateFile(PGSA_DUMP_FILE, "r");
	if (!file)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", PGSA_DUMP_FILE)));
	}

	/* Use a temporary context for all parse-phase allocations. */
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "pg_stash_advice load",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* Figure out how long the file is. */
	if (fstat(fileno(file), &statbuf) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", PGSA_DUMP_FILE)));

	/*
	 * Slurp the entire file into memory all at once.
	 *
	 * We could avoid this by reading the file incrementally and applying
	 * changes to pgsa_stash_dshash and pgsa_entry_dshash as we go. Given the
	 * lockout mechanism implemented by stashes_ready, that shouldn't have any
	 * user-visible behavioral consequences, but it would consume shared
	 * memory to no benefit. It seems better to buffer everything in private
	 * memory first, and then only apply the changes once the file has been
	 * successfully parsed in its entirety.
	 *
	 * That also has the advantage of possibly being more future-proof: if we
	 * decide to remove the stashes_ready mechanism in the future, or say
	 * allow for multiple save files, fully validating the file before
	 * applying any changes will become much more important.
	 *
	 * Of course, this approach does have one major disadvantage, which is
	 * that we'll temporarily use about twice as much memory as we're
	 * ultimately going to need, but that seems like it shouldn't be a problem
	 * in practice. If there's so much stashed advice that parsing the disk
	 * file runs us out of memory, something has gone terribly wrong. In that
	 * situation, there probably also isn't enough free memory for the
	 * workload that the advice is attempting to manipulate to run
	 * successfully.
	 */
	filebuf = palloc_extended(statbuf.st_size + 1, MCXT_ALLOC_HUGE);
	nread = fread(filebuf, 1, statbuf.st_size, file);
	if (ferror(file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", PGSA_DUMP_FILE)));
	FreeFile(file);
	filebuf[nread] = '\0';

	/* Initial memory allocations. */
	saved_stashes = pgsa_saved_stash_table_create(tmpcxt, 64, NULL);
	entries = palloc(max_entries * sizeof(pgsa_saved_entry));

	/*
	 * For memory and CPU efficiency, we parse the file in place. The end of
	 * each line gets replaced with a NUL byte, and then the end of each field
	 * within a line gets the same treatment. The advice string is unescaped
	 * in place, and stash names and query IDs can't contain any special
	 * characters. All of the resulting pointers point right back into the
	 * buffer; we only need additional memory to grow the 'entries' array and
	 * the 'saved_stashes' hash table.
	 */
	for (p = filebuf, lineno = 1; *p != '\0'; lineno++)
	{
		char	   *cursor = p;
		char	   *eol;
		char	   *line_type;

		/* Find end of line and NUL-terminate. */
		eol = strchr(p, '\n');
		if (eol != NULL)
		{
			*eol = '\0';
			p = eol + 1;
			if (eol > cursor && eol[-1] == '\r')
				eol[-1] = '\0';
		}
		else
			p += strlen(p);

		/* Skip empty lines. */
		if (*cursor == '\0')
			continue;

		/* First field is the type of line, either "stash" or "entry". */
		line_type = pgsa_next_tsv_field(&cursor);
		if (strcmp(line_type, "stash") == 0)
		{
			char	   *name;
			bool		found;

			/* Second field should be the stash name. */
			name = pgsa_next_tsv_field(&cursor);
			if (name == NULL || *name == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: expected stash name",
								PGSA_DUMP_FILE, lineno)));

			/* No further fields are expected. */
			if (*cursor != '\0')
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: expected end of line",
								PGSA_DUMP_FILE, lineno)));

			/* Duplicate check. */
			(void) pgsa_saved_stash_table_insert(saved_stashes, name, &found);
			if (found)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: duplicate stash name \"%s\"",
								PGSA_DUMP_FILE, lineno, name)));
			num_stashes++;
		}
		else if (strcmp(line_type, "entry") == 0)
		{
			char	   *stash_name;
			char	   *queryid_str;
			char	   *advice_str;
			char	   *endptr;
			int64		queryId;

			/* Second field should be the stash name. */
			stash_name = pgsa_next_tsv_field(&cursor);
			if (stash_name == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: expected stash name",
								PGSA_DUMP_FILE, lineno)));

			/* Third field should be the query ID. */
			queryid_str = pgsa_next_tsv_field(&cursor);
			if (queryid_str == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: expected query ID",
								PGSA_DUMP_FILE, lineno)));

			/* Fourth field should be the advice string. */
			advice_str = pgsa_next_tsv_field(&cursor);
			if (advice_str == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: expected advice string",
								PGSA_DUMP_FILE, lineno)));

			/* No further fields are expected. */
			if (*cursor != '\0')
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: expected end of line",
								PGSA_DUMP_FILE, lineno)));

			/* Make sure the stash is one we've actually seen. */
			if (pgsa_saved_stash_table_lookup(saved_stashes,
											  stash_name) == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: unknown stash \"%s\"",
								PGSA_DUMP_FILE, lineno, stash_name)));

			/* Parse the query ID. */
			errno = 0;
			queryId = strtoll(queryid_str, &endptr, 10);
			if (*endptr != '\0' || errno != 0 || queryid_str == endptr ||
				queryId == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: invalid query ID \"%s\"",
								PGSA_DUMP_FILE, lineno, queryid_str)));

			/* Unescape the advice string. */
			pgsa_unescape_tsv_field(advice_str, PGSA_DUMP_FILE, lineno);

			/* Append to the entry array. */
			if (num_entries >= max_entries)
			{
				max_entries *= 2;
				entries = repalloc(entries,
								   max_entries * sizeof(pgsa_saved_entry));
			}
			entries[num_entries].stash_name = stash_name;
			entries[num_entries].queryId = queryId;
			entries[num_entries].advice_string = advice_str;
			num_entries++;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("syntax error in file \"%s\" line %u: unrecognized line type",
							PGSA_DUMP_FILE, lineno)));
		}
	}

	/*
	 * Parsing succeeded. Apply everything to shared memory.
	 *
	 * At this point, we know that the file we just read is fully valid, but
	 * it's still possible for this to fail if, for example, DSA memory cannot
	 * be allocated. If that happens, the worker will die, the postmaster will
	 * eventually restart it, and we'll try again after clearing any data that
	 * we did manage to put into shared memory. (Note that we call
	 * pgsa_reset_all_stashes() at the top of this function.)
	 */
	pgsa_restore_stashes(saved_stashes);
	pgsa_restore_entries(entries, num_entries);

	/* Hooray, it worked! Notify the user. */
	ereport(LOG,
			(errmsg("loaded %d advice stashes and %d entries from \"%s\"",
					num_stashes, num_entries, PGSA_DUMP_FILE)));

	/* Clean up. */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);
}

/*
 * Write all advice stash data to disk.
 *
 * The file format is a simple TSV with a line-type prefix:
 *   stash\tstash_name
 *   entry\tstash_name\tquery_id\tadvice_string
 */
static void
pgsa_write_to_disk(void)
{
	pgsa_writer_context wctx = {0};
	MemoryContext tmpcxt;
	MemoryContext oldcxt;

	Assert(pgsa_entry_dshash != NULL);

	/* Use a temporary context so all allocations are freed at the end. */
	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "pg_stash_advice dump",
								   ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tmpcxt);

	/* Set up the writer context. */
	snprintf(wctx.pathname, MAXPGPATH, "%s.tmp", PGSA_DUMP_FILE);
	wctx.file = AllocateFile(wctx.pathname, "w");
	if (!wctx.file)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", wctx.pathname)));
	wctx.nhash = pgsa_stash_name_table_create(tmpcxt, 64, NULL);
	initStringInfo(&wctx.buf);

	/* Write stash lines, then entry lines. */
	pgsa_write_stashes(&wctx);
	pgsa_write_entries(&wctx);

	/*
	 * If nothing was written, remove both the temp file and any existing dump
	 * file rather than installing a zero-length file.
	 */
	if (wctx.nhash->members == 0)
	{
		ereport(DEBUG1,
				errmsg("there are no advice stashes to save"));
		FreeFile(wctx.file);
		unlink(wctx.pathname);
		if (unlink(PGSA_DUMP_FILE) == 0)
			ereport(DEBUG1,
					errmsg("removed \"%s\"", PGSA_DUMP_FILE));
	}
	else
	{
		if (FreeFile(wctx.file) != 0)
		{
			int			save_errno = errno;

			unlink(wctx.pathname);
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m",
							wctx.pathname)));
		}
		(void) durable_rename(wctx.pathname, PGSA_DUMP_FILE, ERROR);

		ereport(LOG,
				errmsg("saved %d advice stashes and %d entries to \"%s\"",
					   (int) wctx.nhash->members, wctx.entries_written,
					   PGSA_DUMP_FILE));
	}

	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(tmpcxt);
}

/*
 * Append the TSV-escaped form of str to buf.
 *
 * Backslash, tab, newline, and carriage return are escaped with backslash
 * sequences.  All other characters are passed through unchanged.
 */
static void
pgsa_append_tsv_escaped_string(StringInfo buf, const char *str)
{
	for (const char *p = str; *p != '\0'; p++)
	{
		switch (*p)
		{
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			default:
				appendStringInfoChar(buf, *p);
				break;
		}
	}
}

/*
 * Extract the next tab-delimited field from *cursor.
 *
 * The tab delimiter is replaced with '\0' and *cursor is advanced past it.
 * If *cursor already points to '\0' (no more fields), returns NULL.
 */
static char *
pgsa_next_tsv_field(char **cursor)
{
	char	   *start = *cursor;
	char	   *p = start;

	if (*p == '\0')
		return NULL;

	while (*p != '\0' && *p != '\t')
		p++;

	if (*p == '\t')
		*p++ = '\0';

	*cursor = p;
	return start;
}

/*
 * Insert entries into shared memory from the parsed entry array.
 */
static void
pgsa_restore_entries(pgsa_saved_entry *entries, int num_entries)
{
	LWLockAcquire(&pgsa_state->lock, LW_SHARED);
	for (int i = 0; i < num_entries; i++)
	{
		ereport(DEBUG2,
				errmsg("restoring advice stash entry for \"%s\", query ID %" PRId64,
					   entries[i].stash_name, entries[i].queryId));
		pgsa_set_advice_string(entries[i].stash_name,
							   entries[i].queryId,
							   entries[i].advice_string);
	}
	LWLockRelease(&pgsa_state->lock);
}

/*
 * Create stashes in shared memory from the parsed stash hash table.
 */
static void
pgsa_restore_stashes(pgsa_saved_stash_table_hash *saved_stashes)
{
	pgsa_saved_stash_table_iterator iter;
	pgsa_saved_stash *s;

	LWLockAcquire(&pgsa_state->lock, LW_EXCLUSIVE);
	pgsa_saved_stash_table_start_iterate(saved_stashes, &iter);
	while ((s = pgsa_saved_stash_table_iterate(saved_stashes,
											   &iter)) != NULL)
	{
		ereport(DEBUG2,
				errmsg("restoring advice stash \"%s\"", s->name));
		pgsa_create_stash(s->name);
	}
	LWLockRelease(&pgsa_state->lock);
}

/*
 * Unescape a TSV field in place.
 *
 * Recognized escape sequences are \\, \t, \n, and \r.  A trailing backslash
 * or an unrecognized escape sequence is a syntax error.
 */
static void
pgsa_unescape_tsv_field(char *str, const char *filename, unsigned lineno)
{
	char	   *src = str;
	char	   *dst = str;

	while (*src != '\0')
	{
		/* Just pass through anything that's not a backslash-escape. */
		if (likely(*src != '\\'))
		{
			*dst++ = *src++;
			continue;
		}

		/* Check what sort of escape we've got. */
		switch (src[1])
		{
			case '\\':
				*dst++ = '\\';
				break;
			case 't':
				*dst++ = '\t';
				break;
			case 'n':
				*dst++ = '\n';
				break;
			case 'r':
				*dst++ = '\r';
				break;
			case '\0':
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: trailing backslash",
								filename, lineno)));
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("syntax error in file \"%s\" line %u: unrecognized escape \"\\%c\"",
								filename, lineno, src[1])));
				break;
		}

		/* We consumed the backslash and the following character. */
		src += 2;
	}
	*dst = '\0';
}

/*
 * Write an entry line for each advice entry.
 */
static void
pgsa_write_entries(pgsa_writer_context *wctx)
{
	dshash_seq_status iter;
	pgsa_entry *entry;

	dshash_seq_init(&iter, pgsa_entry_dshash, false);
	while ((entry = dshash_seq_next(&iter)) != NULL)
	{
		pgsa_stash_name *n;
		char	   *advice_string;

		if (entry->advice_string == InvalidDsaPointer)
			continue;

		n = pgsa_stash_name_table_lookup(wctx->nhash,
										 entry->key.pgsa_stash_id);
		if (n == NULL)
			continue;			/* orphan entry, skip */

		advice_string = dsa_get_address(pgsa_dsa_area, entry->advice_string);

		resetStringInfo(&wctx->buf);
		appendStringInfo(&wctx->buf, "entry\t%s\t%" PRId64 "\t",
						 n->name, entry->key.queryId);
		pgsa_append_tsv_escaped_string(&wctx->buf, advice_string);
		appendStringInfoChar(&wctx->buf, '\n');
		fwrite(wctx->buf.data, 1, wctx->buf.len, wctx->file);
		if (ferror(wctx->file))
			pgsa_write_error(wctx);
		wctx->entries_written++;
	}
	dshash_seq_term(&iter);
}

/*
 * Clean up and report a write error.  Does not return.
 */
static void
pgsa_write_error(pgsa_writer_context *wctx)
{
	int			save_errno = errno;

	FreeFile(wctx->file);
	unlink(wctx->pathname);
	errno = save_errno;
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not write to file \"%s\": %m", wctx->pathname)));
}

/*
 * Write a stash line for each advice stash, and populate the ID-to-name
 * hash table for use by pgsa_write_entries.
 */
static void
pgsa_write_stashes(pgsa_writer_context *wctx)
{
	dshash_seq_status iter;
	pgsa_stash *stash;

	dshash_seq_init(&iter, pgsa_stash_dshash, false);
	while ((stash = dshash_seq_next(&iter)) != NULL)
	{
		pgsa_stash_name *n;
		bool		found;

		n = pgsa_stash_name_table_insert(wctx->nhash, stash->pgsa_stash_id,
										 &found);
		Assert(!found);
		n->name = pstrdup(stash->name);

		resetStringInfo(&wctx->buf);
		appendStringInfo(&wctx->buf, "stash\t%s\n", n->name);
		fwrite(wctx->buf.data, 1, wctx->buf.len, wctx->file);
		if (ferror(wctx->file))
			pgsa_write_error(wctx);
	}
	dshash_seq_term(&iter);
}
