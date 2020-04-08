/*-------------------------------------------------------------------------
 *
 * pg_backup_archiver.c
 *
 *	Private implementation of the archiver routines.
 *
 *	See the headers to pg_restore for more details.
 *
 * Copyright (c) 2000, Philip Warner
 *	Rights are granted to use this software in any way so long
 *	as this notice is not removed.
 *
 *	The author is not responsible for loss or damages that may
 *	result from its use.
 *
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_archiver.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef WIN32
#include <io.h>
#endif

#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "libpq/libpq-fs.h"
#include "parallel.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"

#define TEXT_DUMP_HEADER "--\n-- PostgreSQL database dump\n--\n\n"
#define TEXT_DUMPALL_HEADER "--\n-- PostgreSQL database cluster dump\n--\n\n"

/* state needed to save/restore an archive's output target */
typedef struct _outputContext
{
	void	   *OF;
	int			gzOut;
} OutputContext;

/*
 * State for tracking TocEntrys that are ready to process during a parallel
 * restore.  (This used to be a list, and we still call it that, though now
 * it's really an array so that we can apply qsort to it.)
 *
 * tes[] is sized large enough that we can't overrun it.
 * The valid entries are indexed first_te .. last_te inclusive.
 * We periodically sort the array to bring larger-by-dataLength entries to
 * the front; "sorted" is true if the valid entries are known sorted.
 */
typedef struct _parallelReadyList
{
	TocEntry  **tes;			/* Ready-to-dump TocEntrys */
	int			first_te;		/* index of first valid entry in tes[] */
	int			last_te;		/* index of last valid entry in tes[] */
	bool		sorted;			/* are valid entries currently sorted? */
} ParallelReadyList;


static ArchiveHandle *_allocAH(const char *FileSpec, const ArchiveFormat fmt,
							   const int compression, bool dosync, ArchiveMode mode,
							   SetupWorkerPtrType setupWorkerPtr);
static void _getObjectDescription(PQExpBuffer buf, TocEntry *te,
								  ArchiveHandle *AH);
static void _printTocEntry(ArchiveHandle *AH, TocEntry *te, bool isData);
static char *sanitize_line(const char *str, bool want_hyphen);
static void _doSetFixedOutputState(ArchiveHandle *AH);
static void _doSetSessionAuth(ArchiveHandle *AH, const char *user);
static void _reconnectToDB(ArchiveHandle *AH, const char *dbname);
static void _becomeUser(ArchiveHandle *AH, const char *user);
static void _becomeOwner(ArchiveHandle *AH, TocEntry *te);
static void _selectOutputSchema(ArchiveHandle *AH, const char *schemaName);
static void _selectTablespace(ArchiveHandle *AH, const char *tablespace);
static void _selectTableAccessMethod(ArchiveHandle *AH, const char *tableam);
static void processEncodingEntry(ArchiveHandle *AH, TocEntry *te);
static void processStdStringsEntry(ArchiveHandle *AH, TocEntry *te);
static void processSearchPathEntry(ArchiveHandle *AH, TocEntry *te);
static teReqs _tocEntryRequired(TocEntry *te, teSection curSection, ArchiveHandle *AH);
static RestorePass _tocEntryRestorePass(TocEntry *te);
static bool _tocEntryIsACL(TocEntry *te);
static void _disableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te);
static void _enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te);
static void buildTocEntryArrays(ArchiveHandle *AH);
static void _moveBefore(ArchiveHandle *AH, TocEntry *pos, TocEntry *te);
static int	_discoverArchiveFormat(ArchiveHandle *AH);

static int	RestoringToDB(ArchiveHandle *AH);
static void dump_lo_buf(ArchiveHandle *AH);
static void dumpTimestamp(ArchiveHandle *AH, const char *msg, time_t tim);
static void SetOutput(ArchiveHandle *AH, const char *filename, int compression);
static OutputContext SaveOutput(ArchiveHandle *AH);
static void RestoreOutput(ArchiveHandle *AH, OutputContext savedContext);

static int	restore_toc_entry(ArchiveHandle *AH, TocEntry *te, bool is_parallel);
static void restore_toc_entries_prefork(ArchiveHandle *AH,
										TocEntry *pending_list);
static void restore_toc_entries_parallel(ArchiveHandle *AH,
										 ParallelState *pstate,
										 TocEntry *pending_list);
static void restore_toc_entries_postfork(ArchiveHandle *AH,
										 TocEntry *pending_list);
static void pending_list_header_init(TocEntry *l);
static void pending_list_append(TocEntry *l, TocEntry *te);
static void pending_list_remove(TocEntry *te);
static void ready_list_init(ParallelReadyList *ready_list, int tocCount);
static void ready_list_free(ParallelReadyList *ready_list);
static void ready_list_insert(ParallelReadyList *ready_list, TocEntry *te);
static void ready_list_remove(ParallelReadyList *ready_list, int i);
static void ready_list_sort(ParallelReadyList *ready_list);
static int	TocEntrySizeCompare(const void *p1, const void *p2);
static void move_to_ready_list(TocEntry *pending_list,
							   ParallelReadyList *ready_list,
							   RestorePass pass);
static TocEntry *pop_next_work_item(ArchiveHandle *AH,
									ParallelReadyList *ready_list,
									ParallelState *pstate);
static void mark_dump_job_done(ArchiveHandle *AH,
							   TocEntry *te,
							   int status,
							   void *callback_data);
static void mark_restore_job_done(ArchiveHandle *AH,
								  TocEntry *te,
								  int status,
								  void *callback_data);
static void fix_dependencies(ArchiveHandle *AH);
static bool has_lock_conflicts(TocEntry *te1, TocEntry *te2);
static void repoint_table_dependencies(ArchiveHandle *AH);
static void identify_locking_dependencies(ArchiveHandle *AH, TocEntry *te);
static void reduce_dependencies(ArchiveHandle *AH, TocEntry *te,
								ParallelReadyList *ready_list);
static void mark_create_done(ArchiveHandle *AH, TocEntry *te);
static void inhibit_data_for_failed_table(ArchiveHandle *AH, TocEntry *te);

static void StrictNamesCheck(RestoreOptions *ropt);


/*
 * Allocate a new DumpOptions block containing all default values.
 */
DumpOptions *
NewDumpOptions(void)
{
	DumpOptions *opts = (DumpOptions *) pg_malloc(sizeof(DumpOptions));

	InitDumpOptions(opts);
	return opts;
}

/*
 * Initialize a DumpOptions struct to all default values
 */
void
InitDumpOptions(DumpOptions *opts)
{
	memset(opts, 0, sizeof(DumpOptions));
	/* set any fields that shouldn't default to zeroes */
	opts->include_everything = true;
	opts->dumpSections = DUMP_UNSECTIONED;
}

/*
 * Create a freshly allocated DumpOptions with options equivalent to those
 * found in the given RestoreOptions.
 */
DumpOptions *
dumpOptionsFromRestoreOptions(RestoreOptions *ropt)
{
	DumpOptions *dopt = NewDumpOptions();

	/* this is the inverse of what's at the end of pg_dump.c's main() */
	dopt->outputClean = ropt->dropSchema;
	dopt->dataOnly = ropt->dataOnly;
	dopt->schemaOnly = ropt->schemaOnly;
	dopt->if_exists = ropt->if_exists;
	dopt->column_inserts = ropt->column_inserts;
	dopt->dumpSections = ropt->dumpSections;
	dopt->aclsSkip = ropt->aclsSkip;
	dopt->outputSuperuser = ropt->superuser;
	dopt->outputCreateDB = ropt->createDB;
	dopt->outputNoOwner = ropt->noOwner;
	dopt->outputNoTablespaces = ropt->noTablespace;
	dopt->disable_triggers = ropt->disable_triggers;
	dopt->use_setsessauth = ropt->use_setsessauth;
	dopt->disable_dollar_quoting = ropt->disable_dollar_quoting;
	dopt->dump_inserts = ropt->dump_inserts;
	dopt->no_comments = ropt->no_comments;
	dopt->no_publications = ropt->no_publications;
	dopt->no_security_labels = ropt->no_security_labels;
	dopt->no_subscriptions = ropt->no_subscriptions;
	dopt->lockWaitTimeout = ropt->lockWaitTimeout;
	dopt->include_everything = ropt->include_everything;
	dopt->enable_row_security = ropt->enable_row_security;
	dopt->sequence_data = ropt->sequence_data;

	return dopt;
}


/*
 *	Wrapper functions.
 *
 *	The objective it to make writing new formats and dumpers as simple
 *	as possible, if necessary at the expense of extra function calls etc.
 *
 */

/*
 * The dump worker setup needs lots of knowledge of the internals of pg_dump,
 * so It's defined in pg_dump.c and passed into OpenArchive. The restore worker
 * setup doesn't need to know anything much, so it's defined here.
 */
static void
setupRestoreWorker(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	AH->ReopenPtr(AH);
}


/* Create a new archive */
/* Public */
Archive *
CreateArchive(const char *FileSpec, const ArchiveFormat fmt,
			  const int compression, bool dosync, ArchiveMode mode,
			  SetupWorkerPtrType setupDumpWorker)

{
	ArchiveHandle *AH = _allocAH(FileSpec, fmt, compression, dosync,
								 mode, setupDumpWorker);

	return (Archive *) AH;
}

/* Open an existing archive */
/* Public */
Archive *
OpenArchive(const char *FileSpec, const ArchiveFormat fmt)
{
	ArchiveHandle *AH = _allocAH(FileSpec, fmt, 0, true, archModeRead, setupRestoreWorker);

	return (Archive *) AH;
}

/* Public */
void
CloseArchive(Archive *AHX)
{
	int			res = 0;
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	AH->ClosePtr(AH);

	/* Close the output */
	if (AH->gzOut)
		res = GZCLOSE(AH->OF);
	else if (AH->OF != stdout)
		res = fclose(AH->OF);

	if (res != 0)
		fatal("could not close output file: %m");
}

/* Public */
void
SetArchiveOptions(Archive *AH, DumpOptions *dopt, RestoreOptions *ropt)
{
	/* Caller can omit dump options, in which case we synthesize them */
	if (dopt == NULL && ropt != NULL)
		dopt = dumpOptionsFromRestoreOptions(ropt);

	/* Save options for later access */
	AH->dopt = dopt;
	AH->ropt = ropt;
}

/* Public */
void
ProcessArchiveRestoreOptions(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	RestoreOptions *ropt = AH->public.ropt;
	TocEntry   *te;
	teSection	curSection;

	/* Decide which TOC entries will be dumped/restored, and mark them */
	curSection = SECTION_PRE_DATA;
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		/*
		 * When writing an archive, we also take this opportunity to check
		 * that we have generated the entries in a sane order that respects
		 * the section divisions.  When reading, don't complain, since buggy
		 * old versions of pg_dump might generate out-of-order archives.
		 */
		if (AH->mode != archModeRead)
		{
			switch (te->section)
			{
				case SECTION_NONE:
					/* ok to be anywhere */
					break;
				case SECTION_PRE_DATA:
					if (curSection != SECTION_PRE_DATA)
						pg_log_warning("archive items not in correct section order");
					break;
				case SECTION_DATA:
					if (curSection == SECTION_POST_DATA)
						pg_log_warning("archive items not in correct section order");
					break;
				case SECTION_POST_DATA:
					/* ok no matter which section we were in */
					break;
				default:
					fatal("unexpected section code %d",
						  (int) te->section);
					break;
			}
		}

		if (te->section != SECTION_NONE)
			curSection = te->section;

		te->reqs = _tocEntryRequired(te, curSection, AH);
	}

	/* Enforce strict names checking */
	if (ropt->strict_names)
		StrictNamesCheck(ropt);
}

/* Public */
void
RestoreArchive(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	RestoreOptions *ropt = AH->public.ropt;
	bool		parallel_mode;
	TocEntry   *te;
	OutputContext sav;

	AH->stage = STAGE_INITIALIZING;

	/*
	 * If we're going to do parallel restore, there are some restrictions.
	 */
	parallel_mode = (AH->public.numWorkers > 1 && ropt->useDB);
	if (parallel_mode)
	{
		/* We haven't got round to making this work for all archive formats */
		if (AH->ClonePtr == NULL || AH->ReopenPtr == NULL)
			fatal("parallel restore is not supported with this archive file format");

		/* Doesn't work if the archive represents dependencies as OIDs */
		if (AH->version < K_VERS_1_8)
			fatal("parallel restore is not supported with archives made by pre-8.0 pg_dump");

		/*
		 * It's also not gonna work if we can't reopen the input file, so
		 * let's try that immediately.
		 */
		AH->ReopenPtr(AH);
	}

	/*
	 * Make sure we won't need (de)compression we haven't got
	 */
#ifndef HAVE_LIBZ
	if (AH->compression != 0 && AH->PrintTocDataPtr != NULL)
	{
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if (te->hadDumper && (te->reqs & REQ_DATA) != 0)
				fatal("cannot restore from compressed archive (compression not supported in this installation)");
		}
	}
#endif

	/*
	 * Prepare index arrays, so we can assume we have them throughout restore.
	 * It's possible we already did this, though.
	 */
	if (AH->tocsByDumpId == NULL)
		buildTocEntryArrays(AH);

	/*
	 * If we're using a DB connection, then connect it.
	 */
	if (ropt->useDB)
	{
		pg_log_info("connecting to database for restore");
		if (AH->version < K_VERS_1_3)
			fatal("direct database connections are not supported in pre-1.3 archives");

		/*
		 * We don't want to guess at whether the dump will successfully
		 * restore; allow the attempt regardless of the version of the restore
		 * target.
		 */
		AHX->minRemoteVersion = 0;
		AHX->maxRemoteVersion = 9999999;

		ConnectDatabase(AHX, ropt->dbname,
						ropt->pghost, ropt->pgport, ropt->username,
						ropt->promptPassword);

		/*
		 * If we're talking to the DB directly, don't send comments since they
		 * obscure SQL when displaying errors
		 */
		AH->noTocComments = 1;
	}

	/*
	 * Work out if we have an implied data-only restore. This can happen if
	 * the dump was data only or if the user has used a toc list to exclude
	 * all of the schema data. All we do is look for schema entries - if none
	 * are found then we set the dataOnly flag.
	 *
	 * We could scan for wanted TABLE entries, but that is not the same as
	 * dataOnly. At this stage, it seems unnecessary (6-Mar-2001).
	 */
	if (!ropt->dataOnly)
	{
		int			impliedDataOnly = 1;

		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if ((te->reqs & REQ_SCHEMA) != 0)
			{					/* It's schema, and it's wanted */
				impliedDataOnly = 0;
				break;
			}
		}
		if (impliedDataOnly)
		{
			ropt->dataOnly = impliedDataOnly;
			pg_log_info("implied data-only restore");
		}
	}

	/*
	 * Setup the output file if necessary.
	 */
	sav = SaveOutput(AH);
	if (ropt->filename || ropt->compression)
		SetOutput(AH, ropt->filename, ropt->compression);

	ahprintf(AH, "--\n-- PostgreSQL database dump\n--\n\n");

	if (AH->archiveRemoteVersion)
		ahprintf(AH, "-- Dumped from database version %s\n",
				 AH->archiveRemoteVersion);
	if (AH->archiveDumpVersion)
		ahprintf(AH, "-- Dumped by pg_dump version %s\n",
				 AH->archiveDumpVersion);

	ahprintf(AH, "\n");

	if (AH->public.verbose)
		dumpTimestamp(AH, "Started on", AH->createDate);

	if (ropt->single_txn)
	{
		if (AH->connection)
			StartTransaction(AHX);
		else
			ahprintf(AH, "BEGIN;\n\n");
	}

	/*
	 * Establish important parameter values right away.
	 */
	_doSetFixedOutputState(AH);

	AH->stage = STAGE_PROCESSING;

	/*
	 * Drop the items at the start, in reverse order
	 */
	if (ropt->dropSchema)
	{
		for (te = AH->toc->prev; te != AH->toc; te = te->prev)
		{
			AH->currentTE = te;

			/*
			 * In createDB mode, issue a DROP *only* for the database as a
			 * whole.  Issuing drops against anything else would be wrong,
			 * because at this point we're connected to the wrong database.
			 * (The DATABASE PROPERTIES entry, if any, should be treated like
			 * the DATABASE entry.)
			 */
			if (ropt->createDB)
			{
				if (strcmp(te->desc, "DATABASE") != 0 &&
					strcmp(te->desc, "DATABASE PROPERTIES") != 0)
					continue;
			}

			/* Otherwise, drop anything that's selected and has a dropStmt */
			if (((te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0) && te->dropStmt)
			{
				pg_log_info("dropping %s %s", te->desc, te->tag);
				/* Select owner and schema as necessary */
				_becomeOwner(AH, te);
				_selectOutputSchema(AH, te->namespace);

				/*
				 * Now emit the DROP command, if the object has one.  Note we
				 * don't necessarily emit it verbatim; at this point we add an
				 * appropriate IF EXISTS clause, if the user requested it.
				 */
				if (*te->dropStmt != '\0')
				{
					if (!ropt->if_exists)
					{
						/* No --if-exists?	Then just use the original */
						ahprintf(AH, "%s", te->dropStmt);
					}
					else
					{
						/*
						 * Inject an appropriate spelling of "if exists".  For
						 * large objects, we have a separate routine that
						 * knows how to do it, without depending on
						 * te->dropStmt; use that.  For other objects we need
						 * to parse the command.
						 */
						if (strncmp(te->desc, "BLOB", 4) == 0)
						{
							DropBlobIfExists(AH, te->catalogId.oid);
						}
						else
						{
							char	   *dropStmt = pg_strdup(te->dropStmt);
							char	   *dropStmtOrig = dropStmt;
							PQExpBuffer ftStmt = createPQExpBuffer();

							/*
							 * Need to inject IF EXISTS clause after ALTER
							 * TABLE part in ALTER TABLE .. DROP statement
							 */
							if (strncmp(dropStmt, "ALTER TABLE", 11) == 0)
							{
								appendPQExpBufferStr(ftStmt,
													 "ALTER TABLE IF EXISTS");
								dropStmt = dropStmt + 11;
							}

							/*
							 * ALTER TABLE..ALTER COLUMN..DROP DEFAULT does
							 * not support the IF EXISTS clause, and therefore
							 * we simply emit the original command for DEFAULT
							 * objects (modulo the adjustment made above).
							 *
							 * Likewise, don't mess with DATABASE PROPERTIES.
							 *
							 * If we used CREATE OR REPLACE VIEW as a means of
							 * quasi-dropping an ON SELECT rule, that should
							 * be emitted unchanged as well.
							 *
							 * For other object types, we need to extract the
							 * first part of the DROP which includes the
							 * object type.  Most of the time this matches
							 * te->desc, so search for that; however for the
							 * different kinds of CONSTRAINTs, we know to
							 * search for hardcoded "DROP CONSTRAINT" instead.
							 */
							if (strcmp(te->desc, "DEFAULT") == 0 ||
								strcmp(te->desc, "DATABASE PROPERTIES") == 0 ||
								strncmp(dropStmt, "CREATE OR REPLACE VIEW", 22) == 0)
								appendPQExpBufferStr(ftStmt, dropStmt);
							else
							{
								char		buffer[40];
								char	   *mark;

								if (strcmp(te->desc, "CONSTRAINT") == 0 ||
									strcmp(te->desc, "CHECK CONSTRAINT") == 0 ||
									strcmp(te->desc, "FK CONSTRAINT") == 0)
									strcpy(buffer, "DROP CONSTRAINT");
								else
									snprintf(buffer, sizeof(buffer), "DROP %s",
											 te->desc);

								mark = strstr(dropStmt, buffer);

								if (mark)
								{
									*mark = '\0';
									appendPQExpBuffer(ftStmt, "%s%s IF EXISTS%s",
													  dropStmt, buffer,
													  mark + strlen(buffer));
								}
								else
								{
									/* complain and emit unmodified command */
									pg_log_warning("could not find where to insert IF EXISTS in statement \"%s\"",
												   dropStmtOrig);
									appendPQExpBufferStr(ftStmt, dropStmt);
								}
							}

							ahprintf(AH, "%s", ftStmt->data);

							destroyPQExpBuffer(ftStmt);
							pg_free(dropStmtOrig);
						}
					}
				}
			}
		}

		/*
		 * _selectOutputSchema may have set currSchema to reflect the effect
		 * of a "SET search_path" command it emitted.  However, by now we may
		 * have dropped that schema; or it might not have existed in the first
		 * place.  In either case the effective value of search_path will not
		 * be what we think.  Forcibly reset currSchema so that we will
		 * re-establish the search_path setting when needed (after creating
		 * the schema).
		 *
		 * If we treated users as pg_dump'able objects then we'd need to reset
		 * currUser here too.
		 */
		if (AH->currSchema)
			free(AH->currSchema);
		AH->currSchema = NULL;
	}

	if (parallel_mode)
	{
		/*
		 * In parallel mode, turn control over to the parallel-restore logic.
		 */
		ParallelState *pstate;
		TocEntry	pending_list;

		/* The archive format module may need some setup for this */
		if (AH->PrepParallelRestorePtr)
			AH->PrepParallelRestorePtr(AH);

		pending_list_header_init(&pending_list);

		/* This runs PRE_DATA items and then disconnects from the database */
		restore_toc_entries_prefork(AH, &pending_list);
		Assert(AH->connection == NULL);

		/* ParallelBackupStart() will actually fork the processes */
		pstate = ParallelBackupStart(AH);
		restore_toc_entries_parallel(AH, pstate, &pending_list);
		ParallelBackupEnd(AH, pstate);

		/* reconnect the master and see if we missed something */
		restore_toc_entries_postfork(AH, &pending_list);
		Assert(AH->connection != NULL);
	}
	else
	{
		/*
		 * In serial mode, process everything in three phases: normal items,
		 * then ACLs, then post-ACL items.  We might be able to skip one or
		 * both extra phases in some cases, eg data-only restores.
		 */
		bool		haveACL = false;
		bool		havePostACL = false;

		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if ((te->reqs & (REQ_SCHEMA | REQ_DATA)) == 0)
				continue;		/* ignore if not to be dumped at all */

			switch (_tocEntryRestorePass(te))
			{
				case RESTORE_PASS_MAIN:
					(void) restore_toc_entry(AH, te, false);
					break;
				case RESTORE_PASS_ACL:
					haveACL = true;
					break;
				case RESTORE_PASS_POST_ACL:
					havePostACL = true;
					break;
			}
		}

		if (haveACL)
		{
			for (te = AH->toc->next; te != AH->toc; te = te->next)
			{
				if ((te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0 &&
					_tocEntryRestorePass(te) == RESTORE_PASS_ACL)
					(void) restore_toc_entry(AH, te, false);
			}
		}

		if (havePostACL)
		{
			for (te = AH->toc->next; te != AH->toc; te = te->next)
			{
				if ((te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0 &&
					_tocEntryRestorePass(te) == RESTORE_PASS_POST_ACL)
					(void) restore_toc_entry(AH, te, false);
			}
		}
	}

	if (ropt->single_txn)
	{
		if (AH->connection)
			CommitTransaction(AHX);
		else
			ahprintf(AH, "COMMIT;\n\n");
	}

	if (AH->public.verbose)
		dumpTimestamp(AH, "Completed on", time(NULL));

	ahprintf(AH, "--\n-- PostgreSQL database dump complete\n--\n\n");

	/*
	 * Clean up & we're done.
	 */
	AH->stage = STAGE_FINALIZING;

	if (ropt->filename || ropt->compression)
		RestoreOutput(AH, sav);

	if (ropt->useDB)
		DisconnectDatabase(&AH->public);
}

/*
 * Restore a single TOC item.  Used in both parallel and non-parallel restore;
 * is_parallel is true if we are in a worker child process.
 *
 * Returns 0 normally, but WORKER_CREATE_DONE or WORKER_INHIBIT_DATA if
 * the parallel parent has to make the corresponding status update.
 */
static int
restore_toc_entry(ArchiveHandle *AH, TocEntry *te, bool is_parallel)
{
	RestoreOptions *ropt = AH->public.ropt;
	int			status = WORKER_OK;
	teReqs		reqs;
	bool		defnDumped;

	AH->currentTE = te;

	/* Dump any relevant dump warnings to stderr */
	if (!ropt->suppressDumpWarnings && strcmp(te->desc, "WARNING") == 0)
	{
		if (!ropt->dataOnly && te->defn != NULL && strlen(te->defn) != 0)
			pg_log_warning("warning from original dump file: %s", te->defn);
		else if (te->copyStmt != NULL && strlen(te->copyStmt) != 0)
			pg_log_warning("warning from original dump file: %s", te->copyStmt);
	}

	/* Work out what, if anything, we want from this entry */
	reqs = te->reqs;

	defnDumped = false;

	/*
	 * If it has a schema component that we want, then process that
	 */
	if ((reqs & REQ_SCHEMA) != 0)
	{
		/* Show namespace in log message if available */
		if (te->namespace)
			pg_log_info("creating %s \"%s.%s\"",
						te->desc, te->namespace, te->tag);
		else
			pg_log_info("creating %s \"%s\"",
						te->desc, te->tag);

		_printTocEntry(AH, te, false);
		defnDumped = true;

		if (strcmp(te->desc, "TABLE") == 0)
		{
			if (AH->lastErrorTE == te)
			{
				/*
				 * We failed to create the table. If
				 * --no-data-for-failed-tables was given, mark the
				 * corresponding TABLE DATA to be ignored.
				 *
				 * In the parallel case this must be done in the parent, so we
				 * just set the return value.
				 */
				if (ropt->noDataForFailedTables)
				{
					if (is_parallel)
						status = WORKER_INHIBIT_DATA;
					else
						inhibit_data_for_failed_table(AH, te);
				}
			}
			else
			{
				/*
				 * We created the table successfully.  Mark the corresponding
				 * TABLE DATA for possible truncation.
				 *
				 * In the parallel case this must be done in the parent, so we
				 * just set the return value.
				 */
				if (is_parallel)
					status = WORKER_CREATE_DONE;
				else
					mark_create_done(AH, te);
			}
		}

		/*
		 * If we created a DB, connect to it.  Also, if we changed DB
		 * properties, reconnect to ensure that relevant GUC settings are
		 * applied to our session.
		 */
		if (strcmp(te->desc, "DATABASE") == 0 ||
			strcmp(te->desc, "DATABASE PROPERTIES") == 0)
		{
			PQExpBufferData connstr;

			initPQExpBuffer(&connstr);
			appendPQExpBufferStr(&connstr, "dbname=");
			appendConnStrVal(&connstr, te->tag);
			/* Abandon struct, but keep its buffer until process exit. */

			pg_log_info("connecting to new database \"%s\"", te->tag);
			_reconnectToDB(AH, te->tag);
			ropt->dbname = connstr.data;
		}
	}

	/*
	 * If it has a data component that we want, then process that
	 */
	if ((reqs & REQ_DATA) != 0)
	{
		/*
		 * hadDumper will be set if there is genuine data component for this
		 * node. Otherwise, we need to check the defn field for statements
		 * that need to be executed in data-only restores.
		 */
		if (te->hadDumper)
		{
			/*
			 * If we can output the data, then restore it.
			 */
			if (AH->PrintTocDataPtr != NULL)
			{
				_printTocEntry(AH, te, true);

				if (strcmp(te->desc, "BLOBS") == 0 ||
					strcmp(te->desc, "BLOB COMMENTS") == 0)
				{
					pg_log_info("processing %s", te->desc);

					_selectOutputSchema(AH, "pg_catalog");

					/* Send BLOB COMMENTS data to ExecuteSimpleCommands() */
					if (strcmp(te->desc, "BLOB COMMENTS") == 0)
						AH->outputKind = OUTPUT_OTHERDATA;

					AH->PrintTocDataPtr(AH, te);

					AH->outputKind = OUTPUT_SQLCMDS;
				}
				else
				{
					_disableTriggersIfNecessary(AH, te);

					/* Select owner and schema as necessary */
					_becomeOwner(AH, te);
					_selectOutputSchema(AH, te->namespace);

					pg_log_info("processing data for table \"%s.%s\"",
								te->namespace, te->tag);

					/*
					 * In parallel restore, if we created the table earlier in
					 * the run then we wrap the COPY in a transaction and
					 * precede it with a TRUNCATE.  If archiving is not on
					 * this prevents WAL-logging the COPY.  This obtains a
					 * speedup similar to that from using single_txn mode in
					 * non-parallel restores.
					 */
					if (is_parallel && te->created)
					{
						/*
						 * Parallel restore is always talking directly to a
						 * server, so no need to see if we should issue BEGIN.
						 */
						StartTransaction(&AH->public);

						/*
						 * If the server version is >= 8.4, make sure we issue
						 * TRUNCATE with ONLY so that child tables are not
						 * wiped.
						 */
						ahprintf(AH, "TRUNCATE TABLE %s%s;\n\n",
								 (PQserverVersion(AH->connection) >= 80400 ?
								  "ONLY " : ""),
								 fmtQualifiedId(te->namespace, te->tag));
					}

					/*
					 * If we have a copy statement, use it.
					 */
					if (te->copyStmt && strlen(te->copyStmt) > 0)
					{
						ahprintf(AH, "%s", te->copyStmt);
						AH->outputKind = OUTPUT_COPYDATA;
					}
					else
						AH->outputKind = OUTPUT_OTHERDATA;

					AH->PrintTocDataPtr(AH, te);

					/*
					 * Terminate COPY if needed.
					 */
					if (AH->outputKind == OUTPUT_COPYDATA &&
						RestoringToDB(AH))
						EndDBCopyMode(&AH->public, te->tag);
					AH->outputKind = OUTPUT_SQLCMDS;

					/* close out the transaction started above */
					if (is_parallel && te->created)
						CommitTransaction(&AH->public);

					_enableTriggersIfNecessary(AH, te);
				}
			}
		}
		else if (!defnDumped)
		{
			/* If we haven't already dumped the defn part, do so now */
			pg_log_info("executing %s %s", te->desc, te->tag);
			_printTocEntry(AH, te, false);
		}
	}

	if (AH->public.n_errors > 0 && status == WORKER_OK)
		status = WORKER_IGNORED_ERRORS;

	return status;
}

/*
 * Allocate a new RestoreOptions block.
 * This is mainly so we can initialize it, but also for future expansion,
 */
RestoreOptions *
NewRestoreOptions(void)
{
	RestoreOptions *opts;

	opts = (RestoreOptions *) pg_malloc0(sizeof(RestoreOptions));

	/* set any fields that shouldn't default to zeroes */
	opts->format = archUnknown;
	opts->promptPassword = TRI_DEFAULT;
	opts->dumpSections = DUMP_UNSECTIONED;

	return opts;
}

static void
_disableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te)
{
	RestoreOptions *ropt = AH->public.ropt;

	/* This hack is only needed in a data-only restore */
	if (!ropt->dataOnly || !ropt->disable_triggers)
		return;

	pg_log_info("disabling triggers for %s", te->tag);

	/*
	 * Become superuser if possible, since they are the only ones who can
	 * disable constraint triggers.  If -S was not given, assume the initial
	 * user identity is a superuser.  (XXX would it be better to become the
	 * table owner?)
	 */
	_becomeUser(AH, ropt->superuser);

	/*
	 * Disable them.
	 */
	ahprintf(AH, "ALTER TABLE %s DISABLE TRIGGER ALL;\n\n",
			 fmtQualifiedId(te->namespace, te->tag));
}

static void
_enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te)
{
	RestoreOptions *ropt = AH->public.ropt;

	/* This hack is only needed in a data-only restore */
	if (!ropt->dataOnly || !ropt->disable_triggers)
		return;

	pg_log_info("enabling triggers for %s", te->tag);

	/*
	 * Become superuser if possible, since they are the only ones who can
	 * disable constraint triggers.  If -S was not given, assume the initial
	 * user identity is a superuser.  (XXX would it be better to become the
	 * table owner?)
	 */
	_becomeUser(AH, ropt->superuser);

	/*
	 * Enable them.
	 */
	ahprintf(AH, "ALTER TABLE %s ENABLE TRIGGER ALL;\n\n",
			 fmtQualifiedId(te->namespace, te->tag));
}

/*
 * This is a routine that is part of the dumper interface, hence the 'Archive*' parameter.
 */

/* Public */
void
WriteData(Archive *AHX, const void *data, size_t dLen)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (!AH->currToc)
		fatal("internal error -- WriteData cannot be called outside the context of a DataDumper routine");

	AH->WriteDataPtr(AH, data, dLen);
}

/*
 * Create a new TOC entry. The TOC was designed as a TOC, but is now the
 * repository for all metadata. But the name has stuck.
 *
 * The new entry is added to the Archive's TOC list.  Most callers can ignore
 * the result value because nothing else need be done, but a few want to
 * manipulate the TOC entry further.
 */

/* Public */
TocEntry *
ArchiveEntry(Archive *AHX, CatalogId catalogId, DumpId dumpId,
			 ArchiveOpts *opts)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	TocEntry   *newToc;

	newToc = (TocEntry *) pg_malloc0(sizeof(TocEntry));

	AH->tocCount++;
	if (dumpId > AH->maxDumpId)
		AH->maxDumpId = dumpId;

	newToc->prev = AH->toc->prev;
	newToc->next = AH->toc;
	AH->toc->prev->next = newToc;
	AH->toc->prev = newToc;

	newToc->catalogId = catalogId;
	newToc->dumpId = dumpId;
	newToc->section = opts->section;

	newToc->tag = pg_strdup(opts->tag);
	newToc->namespace = opts->namespace ? pg_strdup(opts->namespace) : NULL;
	newToc->tablespace = opts->tablespace ? pg_strdup(opts->tablespace) : NULL;
	newToc->tableam = opts->tableam ? pg_strdup(opts->tableam) : NULL;
	newToc->owner = opts->owner ? pg_strdup(opts->owner) : NULL;
	newToc->desc = pg_strdup(opts->description);
	newToc->defn = opts->createStmt ? pg_strdup(opts->createStmt) : NULL;
	newToc->dropStmt = opts->dropStmt ? pg_strdup(opts->dropStmt) : NULL;
	newToc->copyStmt = opts->copyStmt ? pg_strdup(opts->copyStmt) : NULL;

	if (opts->nDeps > 0)
	{
		newToc->dependencies = (DumpId *) pg_malloc(opts->nDeps * sizeof(DumpId));
		memcpy(newToc->dependencies, opts->deps, opts->nDeps * sizeof(DumpId));
		newToc->nDeps = opts->nDeps;
	}
	else
	{
		newToc->dependencies = NULL;
		newToc->nDeps = 0;
	}

	newToc->dataDumper = opts->dumpFn;
	newToc->dataDumperArg = opts->dumpArg;
	newToc->hadDumper = opts->dumpFn ? true : false;

	newToc->formatData = NULL;
	newToc->dataLength = 0;

	if (AH->ArchiveEntryPtr != NULL)
		AH->ArchiveEntryPtr(AH, newToc);

	return newToc;
}

/* Public */
void
PrintTOCSummary(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	RestoreOptions *ropt = AH->public.ropt;
	TocEntry   *te;
	teSection	curSection;
	OutputContext sav;
	const char *fmtName;
	char		stamp_str[64];

	sav = SaveOutput(AH);
	if (ropt->filename)
		SetOutput(AH, ropt->filename, 0 /* no compression */ );

	if (strftime(stamp_str, sizeof(stamp_str), PGDUMP_STRFTIME_FMT,
				 localtime(&AH->createDate)) == 0)
		strcpy(stamp_str, "[unknown]");

	ahprintf(AH, ";\n; Archive created at %s\n", stamp_str);
	ahprintf(AH, ";     dbname: %s\n;     TOC Entries: %d\n;     Compression: %d\n",
			 sanitize_line(AH->archdbname, false),
			 AH->tocCount, AH->compression);

	switch (AH->format)
	{
		case archCustom:
			fmtName = "CUSTOM";
			break;
		case archDirectory:
			fmtName = "DIRECTORY";
			break;
		case archTar:
			fmtName = "TAR";
			break;
		default:
			fmtName = "UNKNOWN";
	}

	ahprintf(AH, ";     Dump Version: %d.%d-%d\n",
			 ARCHIVE_MAJOR(AH->version), ARCHIVE_MINOR(AH->version), ARCHIVE_REV(AH->version));
	ahprintf(AH, ";     Format: %s\n", fmtName);
	ahprintf(AH, ";     Integer: %d bytes\n", (int) AH->intSize);
	ahprintf(AH, ";     Offset: %d bytes\n", (int) AH->offSize);
	if (AH->archiveRemoteVersion)
		ahprintf(AH, ";     Dumped from database version: %s\n",
				 AH->archiveRemoteVersion);
	if (AH->archiveDumpVersion)
		ahprintf(AH, ";     Dumped by pg_dump version: %s\n",
				 AH->archiveDumpVersion);

	ahprintf(AH, ";\n;\n; Selected TOC Entries:\n;\n");

	curSection = SECTION_PRE_DATA;
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (te->section != SECTION_NONE)
			curSection = te->section;
		if (ropt->verbose ||
			(_tocEntryRequired(te, curSection, AH) & (REQ_SCHEMA | REQ_DATA)) != 0)
		{
			char	   *sanitized_name;
			char	   *sanitized_schema;
			char	   *sanitized_owner;

			/*
			 */
			sanitized_name = sanitize_line(te->tag, false);
			sanitized_schema = sanitize_line(te->namespace, true);
			sanitized_owner = sanitize_line(te->owner, false);

			ahprintf(AH, "%d; %u %u %s %s %s %s\n", te->dumpId,
					 te->catalogId.tableoid, te->catalogId.oid,
					 te->desc, sanitized_schema, sanitized_name,
					 sanitized_owner);

			free(sanitized_name);
			free(sanitized_schema);
			free(sanitized_owner);
		}
		if (ropt->verbose && te->nDeps > 0)
		{
			int			i;

			ahprintf(AH, ";\tdepends on:");
			for (i = 0; i < te->nDeps; i++)
				ahprintf(AH, " %d", te->dependencies[i]);
			ahprintf(AH, "\n");
		}
	}

	/* Enforce strict names checking */
	if (ropt->strict_names)
		StrictNamesCheck(ropt);

	if (ropt->filename)
		RestoreOutput(AH, sav);
}

/***********
 * BLOB Archival
 ***********/

/* Called by a dumper to signal start of a BLOB */
int
StartBlob(Archive *AHX, Oid oid)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (!AH->StartBlobPtr)
		fatal("large-object output not supported in chosen format");

	AH->StartBlobPtr(AH, AH->currToc, oid);

	return 1;
}

/* Called by a dumper to signal end of a BLOB */
int
EndBlob(Archive *AHX, Oid oid)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (AH->EndBlobPtr)
		AH->EndBlobPtr(AH, AH->currToc, oid);

	return 1;
}

/**********
 * BLOB Restoration
 **********/

/*
 * Called by a format handler before any blobs are restored
 */
void
StartRestoreBlobs(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->public.ropt;

	if (!ropt->single_txn)
	{
		if (AH->connection)
			StartTransaction(&AH->public);
		else
			ahprintf(AH, "BEGIN;\n\n");
	}

	AH->blobCount = 0;
}

/*
 * Called by a format handler after all blobs are restored
 */
void
EndRestoreBlobs(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->public.ropt;

	if (!ropt->single_txn)
	{
		if (AH->connection)
			CommitTransaction(&AH->public);
		else
			ahprintf(AH, "COMMIT;\n\n");
	}

	pg_log_info(ngettext("restored %d large object",
						 "restored %d large objects",
						 AH->blobCount),
				AH->blobCount);
}


/*
 * Called by a format handler to initiate restoration of a blob
 */
void
StartRestoreBlob(ArchiveHandle *AH, Oid oid, bool drop)
{
	bool		old_blob_style = (AH->version < K_VERS_1_12);
	Oid			loOid;

	AH->blobCount++;

	/* Initialize the LO Buffer */
	AH->lo_buf_used = 0;

	pg_log_info("restoring large object with OID %u", oid);

	/* With an old archive we must do drop and create logic here */
	if (old_blob_style && drop)
		DropBlobIfExists(AH, oid);

	if (AH->connection)
	{
		if (old_blob_style)
		{
			loOid = lo_create(AH->connection, oid);
			if (loOid == 0 || loOid != oid)
				fatal("could not create large object %u: %s",
					  oid, PQerrorMessage(AH->connection));
		}
		AH->loFd = lo_open(AH->connection, oid, INV_WRITE);
		if (AH->loFd == -1)
			fatal("could not open large object %u: %s",
				  oid, PQerrorMessage(AH->connection));
	}
	else
	{
		if (old_blob_style)
			ahprintf(AH, "SELECT pg_catalog.lo_open(pg_catalog.lo_create('%u'), %d);\n",
					 oid, INV_WRITE);
		else
			ahprintf(AH, "SELECT pg_catalog.lo_open('%u', %d);\n",
					 oid, INV_WRITE);
	}

	AH->writingBlob = 1;
}

void
EndRestoreBlob(ArchiveHandle *AH, Oid oid)
{
	if (AH->lo_buf_used > 0)
	{
		/* Write remaining bytes from the LO buffer */
		dump_lo_buf(AH);
	}

	AH->writingBlob = 0;

	if (AH->connection)
	{
		lo_close(AH->connection, AH->loFd);
		AH->loFd = -1;
	}
	else
	{
		ahprintf(AH, "SELECT pg_catalog.lo_close(0);\n\n");
	}
}

/***********
 * Sorting and Reordering
 ***********/

void
SortTocFromFile(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	RestoreOptions *ropt = AH->public.ropt;
	FILE	   *fh;
	char		buf[100];
	bool		incomplete_line;

	/* Allocate space for the 'wanted' array, and init it */
	ropt->idWanted = (bool *) pg_malloc0(sizeof(bool) * AH->maxDumpId);

	/* Setup the file */
	fh = fopen(ropt->tocFile, PG_BINARY_R);
	if (!fh)
		fatal("could not open TOC file \"%s\": %m", ropt->tocFile);

	incomplete_line = false;
	while (fgets(buf, sizeof(buf), fh) != NULL)
	{
		bool		prev_incomplete_line = incomplete_line;
		int			buflen;
		char	   *cmnt;
		char	   *endptr;
		DumpId		id;
		TocEntry   *te;

		/*
		 * Some lines in the file might be longer than sizeof(buf).  This is
		 * no problem, since we only care about the leading numeric ID which
		 * can be at most a few characters; but we have to skip continuation
		 * bufferloads when processing a long line.
		 */
		buflen = strlen(buf);
		if (buflen > 0 && buf[buflen - 1] == '\n')
			incomplete_line = false;
		else
			incomplete_line = true;
		if (prev_incomplete_line)
			continue;

		/* Truncate line at comment, if any */
		cmnt = strchr(buf, ';');
		if (cmnt != NULL)
			cmnt[0] = '\0';

		/* Ignore if all blank */
		if (strspn(buf, " \t\r\n") == strlen(buf))
			continue;

		/* Get an ID, check it's valid and not already seen */
		id = strtol(buf, &endptr, 10);
		if (endptr == buf || id <= 0 || id > AH->maxDumpId ||
			ropt->idWanted[id - 1])
		{
			pg_log_warning("line ignored: %s", buf);
			continue;
		}

		/* Find TOC entry */
		te = getTocEntryByDumpId(AH, id);
		if (!te)
			fatal("could not find entry for ID %d",
				  id);

		/* Mark it wanted */
		ropt->idWanted[id - 1] = true;

		/*
		 * Move each item to the end of the list as it is selected, so that
		 * they are placed in the desired order.  Any unwanted items will end
		 * up at the front of the list, which may seem unintuitive but it's
		 * what we need.  In an ordinary serial restore that makes no
		 * difference, but in a parallel restore we need to mark unrestored
		 * items' dependencies as satisfied before we start examining
		 * restorable items.  Otherwise they could have surprising
		 * side-effects on the order in which restorable items actually get
		 * restored.
		 */
		_moveBefore(AH, AH->toc, te);
	}

	if (fclose(fh) != 0)
		fatal("could not close TOC file: %m");
}

/**********************
 * 'Convenience functions that look like standard IO functions
 * for writing data when in dump mode.
 **********************/

/* Public */
void
archputs(const char *s, Archive *AH)
{
	WriteData(AH, s, strlen(s));
}

/* Public */
int
archprintf(Archive *AH, const char *fmt,...)
{
	int			save_errno = errno;
	char	   *p;
	size_t		len = 128;		/* initial assumption about buffer size */
	size_t		cnt;

	for (;;)
	{
		va_list		args;

		/* Allocate work buffer. */
		p = (char *) pg_malloc(len);

		/* Try to format the data. */
		errno = save_errno;
		va_start(args, fmt);
		cnt = pvsnprintf(p, len, fmt, args);
		va_end(args);

		if (cnt < len)
			break;				/* success */

		/* Release buffer and loop around to try again with larger len. */
		free(p);
		len = cnt;
	}

	WriteData(AH, p, cnt);
	free(p);
	return (int) cnt;
}


/*******************************
 * Stuff below here should be 'private' to the archiver routines
 *******************************/

static void
SetOutput(ArchiveHandle *AH, const char *filename, int compression)
{
	int			fn;

	if (filename)
	{
		if (strcmp(filename, "-") == 0)
			fn = fileno(stdout);
		else
			fn = -1;
	}
	else if (AH->FH)
		fn = fileno(AH->FH);
	else if (AH->fSpec)
	{
		fn = -1;
		filename = AH->fSpec;
	}
	else
		fn = fileno(stdout);

	/* If compression explicitly requested, use gzopen */
#ifdef HAVE_LIBZ
	if (compression != 0)
	{
		char		fmode[14];

		/* Don't use PG_BINARY_x since this is zlib */
		sprintf(fmode, "wb%d", compression);
		if (fn >= 0)
			AH->OF = gzdopen(dup(fn), fmode);
		else
			AH->OF = gzopen(filename, fmode);
		AH->gzOut = 1;
	}
	else
#endif
	{							/* Use fopen */
		if (AH->mode == archModeAppend)
		{
			if (fn >= 0)
				AH->OF = fdopen(dup(fn), PG_BINARY_A);
			else
				AH->OF = fopen(filename, PG_BINARY_A);
		}
		else
		{
			if (fn >= 0)
				AH->OF = fdopen(dup(fn), PG_BINARY_W);
			else
				AH->OF = fopen(filename, PG_BINARY_W);
		}
		AH->gzOut = 0;
	}

	if (!AH->OF)
	{
		if (filename)
			fatal("could not open output file \"%s\": %m", filename);
		else
			fatal("could not open output file: %m");
	}
}

static OutputContext
SaveOutput(ArchiveHandle *AH)
{
	OutputContext sav;

	sav.OF = AH->OF;
	sav.gzOut = AH->gzOut;

	return sav;
}

static void
RestoreOutput(ArchiveHandle *AH, OutputContext savedContext)
{
	int			res;

	if (AH->gzOut)
		res = GZCLOSE(AH->OF);
	else
		res = fclose(AH->OF);

	if (res != 0)
		fatal("could not close output file: %m");

	AH->gzOut = savedContext.gzOut;
	AH->OF = savedContext.OF;
}



/*
 *	Print formatted text to the output file (usually stdout).
 */
int
ahprintf(ArchiveHandle *AH, const char *fmt,...)
{
	int			save_errno = errno;
	char	   *p;
	size_t		len = 128;		/* initial assumption about buffer size */
	size_t		cnt;

	for (;;)
	{
		va_list		args;

		/* Allocate work buffer. */
		p = (char *) pg_malloc(len);

		/* Try to format the data. */
		errno = save_errno;
		va_start(args, fmt);
		cnt = pvsnprintf(p, len, fmt, args);
		va_end(args);

		if (cnt < len)
			break;				/* success */

		/* Release buffer and loop around to try again with larger len. */
		free(p);
		len = cnt;
	}

	ahwrite(p, 1, cnt, AH);
	free(p);
	return (int) cnt;
}

/*
 * Single place for logic which says 'We are restoring to a direct DB connection'.
 */
static int
RestoringToDB(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->public.ropt;

	return (ropt && ropt->useDB && AH->connection);
}

/*
 * Dump the current contents of the LO data buffer while writing a BLOB
 */
static void
dump_lo_buf(ArchiveHandle *AH)
{
	if (AH->connection)
	{
		size_t		res;

		res = lo_write(AH->connection, AH->loFd, AH->lo_buf, AH->lo_buf_used);
		pg_log_debug(ngettext("wrote %lu byte of large object data (result = %lu)",
							  "wrote %lu bytes of large object data (result = %lu)",
							  AH->lo_buf_used),
					 (unsigned long) AH->lo_buf_used, (unsigned long) res);
		if (res != AH->lo_buf_used)
			fatal("could not write to large object (result: %lu, expected: %lu)",
				  (unsigned long) res, (unsigned long) AH->lo_buf_used);
	}
	else
	{
		PQExpBuffer buf = createPQExpBuffer();

		appendByteaLiteralAHX(buf,
							  (const unsigned char *) AH->lo_buf,
							  AH->lo_buf_used,
							  AH);

		/* Hack: turn off writingBlob so ahwrite doesn't recurse to here */
		AH->writingBlob = 0;
		ahprintf(AH, "SELECT pg_catalog.lowrite(0, %s);\n", buf->data);
		AH->writingBlob = 1;

		destroyPQExpBuffer(buf);
	}
	AH->lo_buf_used = 0;
}


/*
 *	Write buffer to the output file (usually stdout). This is used for
 *	outputting 'restore' scripts etc. It is even possible for an archive
 *	format to create a custom output routine to 'fake' a restore if it
 *	wants to generate a script (see TAR output).
 */
void
ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle *AH)
{
	int			bytes_written = 0;

	if (AH->writingBlob)
	{
		size_t		remaining = size * nmemb;

		while (AH->lo_buf_used + remaining > AH->lo_buf_size)
		{
			size_t		avail = AH->lo_buf_size - AH->lo_buf_used;

			memcpy((char *) AH->lo_buf + AH->lo_buf_used, ptr, avail);
			ptr = (const void *) ((const char *) ptr + avail);
			remaining -= avail;
			AH->lo_buf_used += avail;
			dump_lo_buf(AH);
		}

		memcpy((char *) AH->lo_buf + AH->lo_buf_used, ptr, remaining);
		AH->lo_buf_used += remaining;

		bytes_written = size * nmemb;
	}
	else if (AH->gzOut)
		bytes_written = GZWRITE(ptr, size, nmemb, AH->OF);
	else if (AH->CustomOutPtr)
		bytes_written = AH->CustomOutPtr(AH, ptr, size * nmemb);

	else
	{
		/*
		 * If we're doing a restore, and it's direct to DB, and we're
		 * connected then send it to the DB.
		 */
		if (RestoringToDB(AH))
			bytes_written = ExecuteSqlCommandBuf(&AH->public, (const char *) ptr, size * nmemb);
		else
			bytes_written = fwrite(ptr, size, nmemb, AH->OF) * size;
	}

	if (bytes_written != size * nmemb)
		WRITE_ERROR_EXIT;
}

/* on some error, we may decide to go on... */
void
warn_or_exit_horribly(ArchiveHandle *AH, const char *fmt,...)
{
	va_list		ap;

	switch (AH->stage)
	{

		case STAGE_NONE:
			/* Do nothing special */
			break;

		case STAGE_INITIALIZING:
			if (AH->stage != AH->lastErrorStage)
				pg_log_generic(PG_LOG_INFO, "while INITIALIZING:");
			break;

		case STAGE_PROCESSING:
			if (AH->stage != AH->lastErrorStage)
				pg_log_generic(PG_LOG_INFO, "while PROCESSING TOC:");
			break;

		case STAGE_FINALIZING:
			if (AH->stage != AH->lastErrorStage)
				pg_log_generic(PG_LOG_INFO, "while FINALIZING:");
			break;
	}
	if (AH->currentTE != NULL && AH->currentTE != AH->lastErrorTE)
	{
		pg_log_generic(PG_LOG_INFO, "from TOC entry %d; %u %u %s %s %s",
					   AH->currentTE->dumpId,
					   AH->currentTE->catalogId.tableoid,
					   AH->currentTE->catalogId.oid,
					   AH->currentTE->desc ? AH->currentTE->desc : "(no desc)",
					   AH->currentTE->tag ? AH->currentTE->tag : "(no tag)",
					   AH->currentTE->owner ? AH->currentTE->owner : "(no owner)");
	}
	AH->lastErrorStage = AH->stage;
	AH->lastErrorTE = AH->currentTE;

	va_start(ap, fmt);
	pg_log_generic_v(PG_LOG_ERROR, fmt, ap);
	va_end(ap);

	if (AH->public.exit_on_error)
		exit_nicely(1);
	else
		AH->public.n_errors++;
}

#ifdef NOT_USED

static void
_moveAfter(ArchiveHandle *AH, TocEntry *pos, TocEntry *te)
{
	/* Unlink te from list */
	te->prev->next = te->next;
	te->next->prev = te->prev;

	/* and insert it after "pos" */
	te->prev = pos;
	te->next = pos->next;
	pos->next->prev = te;
	pos->next = te;
}
#endif

static void
_moveBefore(ArchiveHandle *AH, TocEntry *pos, TocEntry *te)
{
	/* Unlink te from list */
	te->prev->next = te->next;
	te->next->prev = te->prev;

	/* and insert it before "pos" */
	te->prev = pos->prev;
	te->next = pos;
	pos->prev->next = te;
	pos->prev = te;
}

/*
 * Build index arrays for the TOC list
 *
 * This should be invoked only after we have created or read in all the TOC
 * items.
 *
 * The arrays are indexed by dump ID (so entry zero is unused).  Note that the
 * array entries run only up to maxDumpId.  We might see dependency dump IDs
 * beyond that (if the dump was partial); so always check the array bound
 * before trying to touch an array entry.
 */
static void
buildTocEntryArrays(ArchiveHandle *AH)
{
	DumpId		maxDumpId = AH->maxDumpId;
	TocEntry   *te;

	AH->tocsByDumpId = (TocEntry **) pg_malloc0((maxDumpId + 1) * sizeof(TocEntry *));
	AH->tableDataId = (DumpId *) pg_malloc0((maxDumpId + 1) * sizeof(DumpId));

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		/* this check is purely paranoia, maxDumpId should be correct */
		if (te->dumpId <= 0 || te->dumpId > maxDumpId)
			fatal("bad dumpId");

		/* tocsByDumpId indexes all TOCs by their dump ID */
		AH->tocsByDumpId[te->dumpId] = te;

		/*
		 * tableDataId provides the TABLE DATA item's dump ID for each TABLE
		 * TOC entry that has a DATA item.  We compute this by reversing the
		 * TABLE DATA item's dependency, knowing that a TABLE DATA item has
		 * just one dependency and it is the TABLE item.
		 */
		if (strcmp(te->desc, "TABLE DATA") == 0 && te->nDeps > 0)
		{
			DumpId		tableId = te->dependencies[0];

			/*
			 * The TABLE item might not have been in the archive, if this was
			 * a data-only dump; but its dump ID should be less than its data
			 * item's dump ID, so there should be a place for it in the array.
			 */
			if (tableId <= 0 || tableId > maxDumpId)
				fatal("bad table dumpId for TABLE DATA item");

			AH->tableDataId[tableId] = te->dumpId;
		}
	}
}

TocEntry *
getTocEntryByDumpId(ArchiveHandle *AH, DumpId id)
{
	/* build index arrays if we didn't already */
	if (AH->tocsByDumpId == NULL)
		buildTocEntryArrays(AH);

	if (id > 0 && id <= AH->maxDumpId)
		return AH->tocsByDumpId[id];

	return NULL;
}

teReqs
TocIDRequired(ArchiveHandle *AH, DumpId id)
{
	TocEntry   *te = getTocEntryByDumpId(AH, id);

	if (!te)
		return 0;

	return te->reqs;
}

size_t
WriteOffset(ArchiveHandle *AH, pgoff_t o, int wasSet)
{
	int			off;

	/* Save the flag */
	AH->WriteBytePtr(AH, wasSet);

	/* Write out pgoff_t smallest byte first, prevents endian mismatch */
	for (off = 0; off < sizeof(pgoff_t); off++)
	{
		AH->WriteBytePtr(AH, o & 0xFF);
		o >>= 8;
	}
	return sizeof(pgoff_t) + 1;
}

int
ReadOffset(ArchiveHandle *AH, pgoff_t * o)
{
	int			i;
	int			off;
	int			offsetFlg;

	/* Initialize to zero */
	*o = 0;

	/* Check for old version */
	if (AH->version < K_VERS_1_7)
	{
		/* Prior versions wrote offsets using WriteInt */
		i = ReadInt(AH);
		/* -1 means not set */
		if (i < 0)
			return K_OFFSET_POS_NOT_SET;
		else if (i == 0)
			return K_OFFSET_NO_DATA;

		/* Cast to pgoff_t because it was written as an int. */
		*o = (pgoff_t) i;
		return K_OFFSET_POS_SET;
	}

	/*
	 * Read the flag indicating the state of the data pointer. Check if valid
	 * and die if not.
	 *
	 * This used to be handled by a negative or zero pointer, now we use an
	 * extra byte specifically for the state.
	 */
	offsetFlg = AH->ReadBytePtr(AH) & 0xFF;

	switch (offsetFlg)
	{
		case K_OFFSET_POS_NOT_SET:
		case K_OFFSET_NO_DATA:
		case K_OFFSET_POS_SET:

			break;

		default:
			fatal("unexpected data offset flag %d", offsetFlg);
	}

	/*
	 * Read the bytes
	 */
	for (off = 0; off < AH->offSize; off++)
	{
		if (off < sizeof(pgoff_t))
			*o |= ((pgoff_t) (AH->ReadBytePtr(AH))) << (off * 8);
		else
		{
			if (AH->ReadBytePtr(AH) != 0)
				fatal("file offset in dump file is too large");
		}
	}

	return offsetFlg;
}

size_t
WriteInt(ArchiveHandle *AH, int i)
{
	int			b;

	/*
	 * This is a bit yucky, but I don't want to make the binary format very
	 * dependent on representation, and not knowing much about it, I write out
	 * a sign byte. If you change this, don't forget to change the file
	 * version #, and modify ReadInt to read the new format AS WELL AS the old
	 * formats.
	 */

	/* SIGN byte */
	if (i < 0)
	{
		AH->WriteBytePtr(AH, 1);
		i = -i;
	}
	else
		AH->WriteBytePtr(AH, 0);

	for (b = 0; b < AH->intSize; b++)
	{
		AH->WriteBytePtr(AH, i & 0xFF);
		i >>= 8;
	}

	return AH->intSize + 1;
}

int
ReadInt(ArchiveHandle *AH)
{
	int			res = 0;
	int			bv,
				b;
	int			sign = 0;		/* Default positive */
	int			bitShift = 0;

	if (AH->version > K_VERS_1_0)
		/* Read a sign byte */
		sign = AH->ReadBytePtr(AH);

	for (b = 0; b < AH->intSize; b++)
	{
		bv = AH->ReadBytePtr(AH) & 0xFF;
		if (bv != 0)
			res = res + (bv << bitShift);
		bitShift += 8;
	}

	if (sign)
		res = -res;

	return res;
}

size_t
WriteStr(ArchiveHandle *AH, const char *c)
{
	size_t		res;

	if (c)
	{
		int			len = strlen(c);

		res = WriteInt(AH, len);
		AH->WriteBufPtr(AH, c, len);
		res += len;
	}
	else
		res = WriteInt(AH, -1);

	return res;
}

char *
ReadStr(ArchiveHandle *AH)
{
	char	   *buf;
	int			l;

	l = ReadInt(AH);
	if (l < 0)
		buf = NULL;
	else
	{
		buf = (char *) pg_malloc(l + 1);
		AH->ReadBufPtr(AH, (void *) buf, l);

		buf[l] = '\0';
	}

	return buf;
}

static int
_discoverArchiveFormat(ArchiveHandle *AH)
{
	FILE	   *fh;
	char		sig[6];			/* More than enough */
	size_t		cnt;
	int			wantClose = 0;

	pg_log_debug("attempting to ascertain archive format");

	if (AH->lookahead)
		free(AH->lookahead);

	AH->lookaheadSize = 512;
	AH->lookahead = pg_malloc0(512);
	AH->lookaheadLen = 0;
	AH->lookaheadPos = 0;

	if (AH->fSpec)
	{
		struct stat st;

		wantClose = 1;

		/*
		 * Check if the specified archive is a directory. If so, check if
		 * there's a "toc.dat" (or "toc.dat.gz") file in it.
		 */
		if (stat(AH->fSpec, &st) == 0 && S_ISDIR(st.st_mode))
		{
			char		buf[MAXPGPATH];

			if (snprintf(buf, MAXPGPATH, "%s/toc.dat", AH->fSpec) >= MAXPGPATH)
				fatal("directory name too long: \"%s\"",
					  AH->fSpec);
			if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
			{
				AH->format = archDirectory;
				return AH->format;
			}

#ifdef HAVE_LIBZ
			if (snprintf(buf, MAXPGPATH, "%s/toc.dat.gz", AH->fSpec) >= MAXPGPATH)
				fatal("directory name too long: \"%s\"",
					  AH->fSpec);
			if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
			{
				AH->format = archDirectory;
				return AH->format;
			}
#endif
			fatal("directory \"%s\" does not appear to be a valid archive (\"toc.dat\" does not exist)",
				  AH->fSpec);
			fh = NULL;			/* keep compiler quiet */
		}
		else
		{
			fh = fopen(AH->fSpec, PG_BINARY_R);
			if (!fh)
				fatal("could not open input file \"%s\": %m", AH->fSpec);
		}
	}
	else
	{
		fh = stdin;
		if (!fh)
			fatal("could not open input file: %m");
	}

	if ((cnt = fread(sig, 1, 5, fh)) != 5)
	{
		if (ferror(fh))
			fatal("could not read input file: %m");
		else
			fatal("input file is too short (read %lu, expected 5)",
				  (unsigned long) cnt);
	}

	/* Save it, just in case we need it later */
	memcpy(&AH->lookahead[0], sig, 5);
	AH->lookaheadLen = 5;

	if (strncmp(sig, "PGDMP", 5) == 0)
	{
		int			byteread;
		char		vmaj,
					vmin,
					vrev;

		/*
		 * Finish reading (most of) a custom-format header.
		 *
		 * NB: this code must agree with ReadHead().
		 */
		if ((byteread = fgetc(fh)) == EOF)
			READ_ERROR_EXIT(fh);

		vmaj = byteread;

		if ((byteread = fgetc(fh)) == EOF)
			READ_ERROR_EXIT(fh);

		vmin = byteread;

		/* Save these too... */
		AH->lookahead[AH->lookaheadLen++] = vmaj;
		AH->lookahead[AH->lookaheadLen++] = vmin;

		/* Check header version; varies from V1.0 */
		if (vmaj > 1 || (vmaj == 1 && vmin > 0))	/* Version > 1.0 */
		{
			if ((byteread = fgetc(fh)) == EOF)
				READ_ERROR_EXIT(fh);

			vrev = byteread;
			AH->lookahead[AH->lookaheadLen++] = vrev;
		}
		else
			vrev = 0;

		AH->version = MAKE_ARCHIVE_VERSION(vmaj, vmin, vrev);

		if ((AH->intSize = fgetc(fh)) == EOF)
			READ_ERROR_EXIT(fh);
		AH->lookahead[AH->lookaheadLen++] = AH->intSize;

		if (AH->version >= K_VERS_1_7)
		{
			if ((AH->offSize = fgetc(fh)) == EOF)
				READ_ERROR_EXIT(fh);
			AH->lookahead[AH->lookaheadLen++] = AH->offSize;
		}
		else
			AH->offSize = AH->intSize;

		if ((byteread = fgetc(fh)) == EOF)
			READ_ERROR_EXIT(fh);

		AH->format = byteread;
		AH->lookahead[AH->lookaheadLen++] = AH->format;
	}
	else
	{
		/*
		 * *Maybe* we have a tar archive format file or a text dump ... So,
		 * read first 512 byte header...
		 */
		cnt = fread(&AH->lookahead[AH->lookaheadLen], 1, 512 - AH->lookaheadLen, fh);
		/* read failure is checked below */
		AH->lookaheadLen += cnt;

		if (AH->lookaheadLen >= strlen(TEXT_DUMPALL_HEADER) &&
			(strncmp(AH->lookahead, TEXT_DUMP_HEADER, strlen(TEXT_DUMP_HEADER)) == 0 ||
			 strncmp(AH->lookahead, TEXT_DUMPALL_HEADER, strlen(TEXT_DUMPALL_HEADER)) == 0))
		{
			/*
			 * looks like it's probably a text format dump. so suggest they
			 * try psql
			 */
			fatal("input file appears to be a text format dump. Please use psql.");
		}

		if (AH->lookaheadLen != 512)
		{
			if (feof(fh))
				fatal("input file does not appear to be a valid archive (too short?)");
			else
				READ_ERROR_EXIT(fh);
		}

		if (!isValidTarHeader(AH->lookahead))
			fatal("input file does not appear to be a valid archive");

		AH->format = archTar;
	}

	/* If we can't seek, then mark the header as read */
	if (fseeko(fh, 0, SEEK_SET) != 0)
	{
		/*
		 * NOTE: Formats that use the lookahead buffer can unset this in their
		 * Init routine.
		 */
		AH->readHeader = 1;
	}
	else
		AH->lookaheadLen = 0;	/* Don't bother since we've reset the file */

	/* Close the file */
	if (wantClose)
		if (fclose(fh) != 0)
			fatal("could not close input file: %m");

	return AH->format;
}


/*
 * Allocate an archive handle
 */
static ArchiveHandle *
_allocAH(const char *FileSpec, const ArchiveFormat fmt,
		 const int compression, bool dosync, ArchiveMode mode,
		 SetupWorkerPtrType setupWorkerPtr)
{
	ArchiveHandle *AH;

	pg_log_debug("allocating AH for %s, format %d", FileSpec, fmt);

	AH = (ArchiveHandle *) pg_malloc0(sizeof(ArchiveHandle));

	AH->version = K_VERS_SELF;

	/* initialize for backwards compatible string processing */
	AH->public.encoding = 0;	/* PG_SQL_ASCII */
	AH->public.std_strings = false;

	/* sql error handling */
	AH->public.exit_on_error = true;
	AH->public.n_errors = 0;

	AH->archiveDumpVersion = PG_VERSION;

	AH->createDate = time(NULL);

	AH->intSize = sizeof(int);
	AH->offSize = sizeof(pgoff_t);
	if (FileSpec)
	{
		AH->fSpec = pg_strdup(FileSpec);

		/*
		 * Not used; maybe later....
		 *
		 * AH->workDir = pg_strdup(FileSpec); for(i=strlen(FileSpec) ; i > 0 ;
		 * i--) if (AH->workDir[i-1] == '/')
		 */
	}
	else
		AH->fSpec = NULL;

	AH->currUser = NULL;		/* unknown */
	AH->currSchema = NULL;		/* ditto */
	AH->currTablespace = NULL;	/* ditto */
	AH->currTableAm = NULL;		/* ditto */

	AH->toc = (TocEntry *) pg_malloc0(sizeof(TocEntry));

	AH->toc->next = AH->toc;
	AH->toc->prev = AH->toc;

	AH->mode = mode;
	AH->compression = compression;
	AH->dosync = dosync;

	memset(&(AH->sqlparse), 0, sizeof(AH->sqlparse));

	/* Open stdout with no compression for AH output handle */
	AH->gzOut = 0;
	AH->OF = stdout;

	/*
	 * On Windows, we need to use binary mode to read/write non-text files,
	 * which include all archive formats as well as compressed plain text.
	 * Force stdin/stdout into binary mode if that is what we are using.
	 */
#ifdef WIN32
	if ((fmt != archNull || compression != 0) &&
		(AH->fSpec == NULL || strcmp(AH->fSpec, "") == 0))
	{
		if (mode == archModeWrite)
			_setmode(fileno(stdout), O_BINARY);
		else
			_setmode(fileno(stdin), O_BINARY);
	}
#endif

	AH->SetupWorkerPtr = setupWorkerPtr;

	if (fmt == archUnknown)
		AH->format = _discoverArchiveFormat(AH);
	else
		AH->format = fmt;

	AH->promptPassword = TRI_DEFAULT;

	switch (AH->format)
	{
		case archCustom:
			InitArchiveFmt_Custom(AH);
			break;

		case archNull:
			InitArchiveFmt_Null(AH);
			break;

		case archDirectory:
			InitArchiveFmt_Directory(AH);
			break;

		case archTar:
			InitArchiveFmt_Tar(AH);
			break;

		default:
			fatal("unrecognized file format \"%d\"", fmt);
	}

	return AH;
}

/*
 * Write out all data (tables & blobs)
 */
void
WriteDataChunks(ArchiveHandle *AH, ParallelState *pstate)
{
	TocEntry   *te;

	if (pstate && pstate->numWorkers > 1)
	{
		/*
		 * In parallel mode, this code runs in the master process.  We
		 * construct an array of candidate TEs, then sort it into decreasing
		 * size order, then dispatch each TE to a data-transfer worker.  By
		 * dumping larger tables first, we avoid getting into a situation
		 * where we're down to one job and it's big, losing parallelism.
		 */
		TocEntry  **tes;
		int			ntes;

		tes = (TocEntry **) pg_malloc(AH->tocCount * sizeof(TocEntry *));
		ntes = 0;
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			/* Consider only TEs with dataDumper functions ... */
			if (!te->dataDumper)
				continue;
			/* ... and ignore ones not enabled for dump */
			if ((te->reqs & REQ_DATA) == 0)
				continue;

			tes[ntes++] = te;
		}

		if (ntes > 1)
			qsort((void *) tes, ntes, sizeof(TocEntry *),
				  TocEntrySizeCompare);

		for (int i = 0; i < ntes; i++)
			DispatchJobForTocEntry(AH, pstate, tes[i], ACT_DUMP,
								   mark_dump_job_done, NULL);

		pg_free(tes);

		/* Now wait for workers to finish. */
		WaitForWorkers(AH, pstate, WFW_ALL_IDLE);
	}
	else
	{
		/* Non-parallel mode: just dump all candidate TEs sequentially. */
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			/* Must have same filter conditions as above */
			if (!te->dataDumper)
				continue;
			if ((te->reqs & REQ_DATA) == 0)
				continue;

			WriteDataChunksForTocEntry(AH, te);
		}
	}
}


/*
 * Callback function that's invoked in the master process after a step has
 * been parallel dumped.
 *
 * We don't need to do anything except check for worker failure.
 */
static void
mark_dump_job_done(ArchiveHandle *AH,
				   TocEntry *te,
				   int status,
				   void *callback_data)
{
	pg_log_info("finished item %d %s %s",
				te->dumpId, te->desc, te->tag);

	if (status != 0)
		fatal("worker process failed: exit code %d",
			  status);
}


void
WriteDataChunksForTocEntry(ArchiveHandle *AH, TocEntry *te)
{
	StartDataPtrType startPtr;
	EndDataPtrType endPtr;

	AH->currToc = te;

	if (strcmp(te->desc, "BLOBS") == 0)
	{
		startPtr = AH->StartBlobsPtr;
		endPtr = AH->EndBlobsPtr;
	}
	else
	{
		startPtr = AH->StartDataPtr;
		endPtr = AH->EndDataPtr;
	}

	if (startPtr != NULL)
		(*startPtr) (AH, te);

	/*
	 * The user-provided DataDumper routine needs to call AH->WriteData
	 */
	te->dataDumper((Archive *) AH, te->dataDumperArg);

	if (endPtr != NULL)
		(*endPtr) (AH, te);

	AH->currToc = NULL;
}

void
WriteToc(ArchiveHandle *AH)
{
	TocEntry   *te;
	char		workbuf[32];
	int			tocCount;
	int			i;

	/* count entries that will actually be dumped */
	tocCount = 0;
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if ((te->reqs & (REQ_SCHEMA | REQ_DATA | REQ_SPECIAL)) != 0)
			tocCount++;
	}

	/* printf("%d TOC Entries to save\n", tocCount); */

	WriteInt(AH, tocCount);

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if ((te->reqs & (REQ_SCHEMA | REQ_DATA | REQ_SPECIAL)) == 0)
			continue;

		WriteInt(AH, te->dumpId);
		WriteInt(AH, te->dataDumper ? 1 : 0);

		/* OID is recorded as a string for historical reasons */
		sprintf(workbuf, "%u", te->catalogId.tableoid);
		WriteStr(AH, workbuf);
		sprintf(workbuf, "%u", te->catalogId.oid);
		WriteStr(AH, workbuf);

		WriteStr(AH, te->tag);
		WriteStr(AH, te->desc);
		WriteInt(AH, te->section);
		WriteStr(AH, te->defn);
		WriteStr(AH, te->dropStmt);
		WriteStr(AH, te->copyStmt);
		WriteStr(AH, te->namespace);
		WriteStr(AH, te->tablespace);
		WriteStr(AH, te->tableam);
		WriteStr(AH, te->owner);
		WriteStr(AH, "false");

		/* Dump list of dependencies */
		for (i = 0; i < te->nDeps; i++)
		{
			sprintf(workbuf, "%d", te->dependencies[i]);
			WriteStr(AH, workbuf);
		}
		WriteStr(AH, NULL);		/* Terminate List */

		if (AH->WriteExtraTocPtr)
			AH->WriteExtraTocPtr(AH, te);
	}
}

void
ReadToc(ArchiveHandle *AH)
{
	int			i;
	char	   *tmp;
	DumpId	   *deps;
	int			depIdx;
	int			depSize;
	TocEntry   *te;

	AH->tocCount = ReadInt(AH);
	AH->maxDumpId = 0;

	for (i = 0; i < AH->tocCount; i++)
	{
		te = (TocEntry *) pg_malloc0(sizeof(TocEntry));
		te->dumpId = ReadInt(AH);

		if (te->dumpId > AH->maxDumpId)
			AH->maxDumpId = te->dumpId;

		/* Sanity check */
		if (te->dumpId <= 0)
			fatal("entry ID %d out of range -- perhaps a corrupt TOC",
				  te->dumpId);

		te->hadDumper = ReadInt(AH);

		if (AH->version >= K_VERS_1_8)
		{
			tmp = ReadStr(AH);
			sscanf(tmp, "%u", &te->catalogId.tableoid);
			free(tmp);
		}
		else
			te->catalogId.tableoid = InvalidOid;
		tmp = ReadStr(AH);
		sscanf(tmp, "%u", &te->catalogId.oid);
		free(tmp);

		te->tag = ReadStr(AH);
		te->desc = ReadStr(AH);

		if (AH->version >= K_VERS_1_11)
		{
			te->section = ReadInt(AH);
		}
		else
		{
			/*
			 * Rules for pre-8.4 archives wherein pg_dump hasn't classified
			 * the entries into sections.  This list need not cover entry
			 * types added later than 8.4.
			 */
			if (strcmp(te->desc, "COMMENT") == 0 ||
				strcmp(te->desc, "ACL") == 0 ||
				strcmp(te->desc, "ACL LANGUAGE") == 0)
				te->section = SECTION_NONE;
			else if (strcmp(te->desc, "TABLE DATA") == 0 ||
					 strcmp(te->desc, "BLOBS") == 0 ||
					 strcmp(te->desc, "BLOB COMMENTS") == 0)
				te->section = SECTION_DATA;
			else if (strcmp(te->desc, "CONSTRAINT") == 0 ||
					 strcmp(te->desc, "CHECK CONSTRAINT") == 0 ||
					 strcmp(te->desc, "FK CONSTRAINT") == 0 ||
					 strcmp(te->desc, "INDEX") == 0 ||
					 strcmp(te->desc, "RULE") == 0 ||
					 strcmp(te->desc, "TRIGGER") == 0)
				te->section = SECTION_POST_DATA;
			else
				te->section = SECTION_PRE_DATA;
		}

		te->defn = ReadStr(AH);
		te->dropStmt = ReadStr(AH);

		if (AH->version >= K_VERS_1_3)
			te->copyStmt = ReadStr(AH);

		if (AH->version >= K_VERS_1_6)
			te->namespace = ReadStr(AH);

		if (AH->version >= K_VERS_1_10)
			te->tablespace = ReadStr(AH);

		if (AH->version >= K_VERS_1_14)
			te->tableam = ReadStr(AH);

		te->owner = ReadStr(AH);
		if (AH->version < K_VERS_1_9 || strcmp(ReadStr(AH), "true") == 0)
			pg_log_warning("restoring tables WITH OIDS is not supported anymore");

		/* Read TOC entry dependencies */
		if (AH->version >= K_VERS_1_5)
		{
			depSize = 100;
			deps = (DumpId *) pg_malloc(sizeof(DumpId) * depSize);
			depIdx = 0;
			for (;;)
			{
				tmp = ReadStr(AH);
				if (!tmp)
					break;		/* end of list */
				if (depIdx >= depSize)
				{
					depSize *= 2;
					deps = (DumpId *) pg_realloc(deps, sizeof(DumpId) * depSize);
				}
				sscanf(tmp, "%d", &deps[depIdx]);
				free(tmp);
				depIdx++;
			}

			if (depIdx > 0)		/* We have a non-null entry */
			{
				deps = (DumpId *) pg_realloc(deps, sizeof(DumpId) * depIdx);
				te->dependencies = deps;
				te->nDeps = depIdx;
			}
			else
			{
				free(deps);
				te->dependencies = NULL;
				te->nDeps = 0;
			}
		}
		else
		{
			te->dependencies = NULL;
			te->nDeps = 0;
		}
		te->dataLength = 0;

		if (AH->ReadExtraTocPtr)
			AH->ReadExtraTocPtr(AH, te);

		pg_log_debug("read TOC entry %d (ID %d) for %s %s",
					 i, te->dumpId, te->desc, te->tag);

		/* link completed entry into TOC circular list */
		te->prev = AH->toc->prev;
		AH->toc->prev->next = te;
		AH->toc->prev = te;
		te->next = AH->toc;

		/* special processing immediately upon read for some items */
		if (strcmp(te->desc, "ENCODING") == 0)
			processEncodingEntry(AH, te);
		else if (strcmp(te->desc, "STDSTRINGS") == 0)
			processStdStringsEntry(AH, te);
		else if (strcmp(te->desc, "SEARCHPATH") == 0)
			processSearchPathEntry(AH, te);
	}
}

static void
processEncodingEntry(ArchiveHandle *AH, TocEntry *te)
{
	/* te->defn should have the form SET client_encoding = 'foo'; */
	char	   *defn = pg_strdup(te->defn);
	char	   *ptr1;
	char	   *ptr2 = NULL;
	int			encoding;

	ptr1 = strchr(defn, '\'');
	if (ptr1)
		ptr2 = strchr(++ptr1, '\'');
	if (ptr2)
	{
		*ptr2 = '\0';
		encoding = pg_char_to_encoding(ptr1);
		if (encoding < 0)
			fatal("unrecognized encoding \"%s\"",
				  ptr1);
		AH->public.encoding = encoding;
	}
	else
		fatal("invalid ENCODING item: %s",
			  te->defn);

	free(defn);
}

static void
processStdStringsEntry(ArchiveHandle *AH, TocEntry *te)
{
	/* te->defn should have the form SET standard_conforming_strings = 'x'; */
	char	   *ptr1;

	ptr1 = strchr(te->defn, '\'');
	if (ptr1 && strncmp(ptr1, "'on'", 4) == 0)
		AH->public.std_strings = true;
	else if (ptr1 && strncmp(ptr1, "'off'", 5) == 0)
		AH->public.std_strings = false;
	else
		fatal("invalid STDSTRINGS item: %s",
			  te->defn);
}

static void
processSearchPathEntry(ArchiveHandle *AH, TocEntry *te)
{
	/*
	 * te->defn should contain a command to set search_path.  We just copy it
	 * verbatim for use later.
	 */
	AH->public.searchpath = pg_strdup(te->defn);
}

static void
StrictNamesCheck(RestoreOptions *ropt)
{
	const char *missing_name;

	Assert(ropt->strict_names);

	if (ropt->schemaNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->schemaNames);
		if (missing_name != NULL)
			fatal("schema \"%s\" not found", missing_name);
	}

	if (ropt->tableNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->tableNames);
		if (missing_name != NULL)
			fatal("table \"%s\" not found", missing_name);
	}

	if (ropt->indexNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->indexNames);
		if (missing_name != NULL)
			fatal("index \"%s\" not found", missing_name);
	}

	if (ropt->functionNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->functionNames);
		if (missing_name != NULL)
			fatal("function \"%s\" not found", missing_name);
	}

	if (ropt->triggerNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->triggerNames);
		if (missing_name != NULL)
			fatal("trigger \"%s\" not found", missing_name);
	}
}

/*
 * Determine whether we want to restore this TOC entry.
 *
 * Returns 0 if entry should be skipped, or some combination of the
 * REQ_SCHEMA and REQ_DATA bits if we want to restore schema and/or data
 * portions of this TOC entry, or REQ_SPECIAL if it's a special entry.
 */
static teReqs
_tocEntryRequired(TocEntry *te, teSection curSection, ArchiveHandle *AH)
{
	teReqs		res = REQ_SCHEMA | REQ_DATA;
	RestoreOptions *ropt = AH->public.ropt;

	/* These items are treated specially */
	if (strcmp(te->desc, "ENCODING") == 0 ||
		strcmp(te->desc, "STDSTRINGS") == 0 ||
		strcmp(te->desc, "SEARCHPATH") == 0)
		return REQ_SPECIAL;

	/*
	 * DATABASE and DATABASE PROPERTIES also have a special rule: they are
	 * restored in createDB mode, and not restored otherwise, independently of
	 * all else.
	 */
	if (strcmp(te->desc, "DATABASE") == 0 ||
		strcmp(te->desc, "DATABASE PROPERTIES") == 0)
	{
		if (ropt->createDB)
			return REQ_SCHEMA;
		else
			return 0;
	}

	/*
	 * Process exclusions that affect certain classes of TOC entries.
	 */

	/* If it's an ACL, maybe ignore it */
	if (ropt->aclsSkip && _tocEntryIsACL(te))
		return 0;

	/* If it's a comment, maybe ignore it */
	if (ropt->no_comments && strcmp(te->desc, "COMMENT") == 0)
		return 0;

	/*
	 * If it's a publication or a table part of a publication, maybe ignore
	 * it.
	 */
	if (ropt->no_publications &&
		(strcmp(te->desc, "PUBLICATION") == 0 ||
		 strcmp(te->desc, "PUBLICATION TABLE") == 0))
		return 0;

	/* If it's a security label, maybe ignore it */
	if (ropt->no_security_labels && strcmp(te->desc, "SECURITY LABEL") == 0)
		return 0;

	/* If it's a subscription, maybe ignore it */
	if (ropt->no_subscriptions && strcmp(te->desc, "SUBSCRIPTION") == 0)
		return 0;

	/* Ignore it if section is not to be dumped/restored */
	switch (curSection)
	{
		case SECTION_PRE_DATA:
			if (!(ropt->dumpSections & DUMP_PRE_DATA))
				return 0;
			break;
		case SECTION_DATA:
			if (!(ropt->dumpSections & DUMP_DATA))
				return 0;
			break;
		case SECTION_POST_DATA:
			if (!(ropt->dumpSections & DUMP_POST_DATA))
				return 0;
			break;
		default:
			/* shouldn't get here, really, but ignore it */
			return 0;
	}

	/* Ignore it if rejected by idWanted[] (cf. SortTocFromFile) */
	if (ropt->idWanted && !ropt->idWanted[te->dumpId - 1])
		return 0;

	/*
	 * Check options for selective dump/restore.
	 */
	if (strcmp(te->desc, "ACL") == 0 ||
		strcmp(te->desc, "COMMENT") == 0 ||
		strcmp(te->desc, "SECURITY LABEL") == 0)
	{
		/* Database properties react to createDB, not selectivity options. */
		if (strncmp(te->tag, "DATABASE ", 9) == 0)
		{
			if (!ropt->createDB)
				return 0;
		}
		else if (ropt->schemaNames.head != NULL ||
				 ropt->schemaExcludeNames.head != NULL ||
				 ropt->selTypes)
		{
			/*
			 * In a selective dump/restore, we want to restore these dependent
			 * TOC entry types only if their parent object is being restored.
			 * Without selectivity options, we let through everything in the
			 * archive.  Note there may be such entries with no parent, eg
			 * non-default ACLs for built-in objects.
			 *
			 * This code depends on the parent having been marked already,
			 * which should be the case; if it isn't, perhaps due to
			 * SortTocFromFile rearrangement, skipping the dependent entry
			 * seems prudent anyway.
			 *
			 * Ideally we'd handle, eg, table CHECK constraints this way too.
			 * But it's hard to tell which of their dependencies is the one to
			 * consult.
			 */
			if (te->nDeps != 1 ||
				TocIDRequired(AH, te->dependencies[0]) == 0)
				return 0;
		}
	}
	else
	{
		/* Apply selective-restore rules for standalone TOC entries. */
		if (ropt->schemaNames.head != NULL)
		{
			/* If no namespace is specified, it means all. */
			if (!te->namespace)
				return 0;
			if (!simple_string_list_member(&ropt->schemaNames, te->namespace))
				return 0;
		}

		if (ropt->schemaExcludeNames.head != NULL &&
			te->namespace &&
			simple_string_list_member(&ropt->schemaExcludeNames, te->namespace))
			return 0;

		if (ropt->selTypes)
		{
			if (strcmp(te->desc, "TABLE") == 0 ||
				strcmp(te->desc, "TABLE DATA") == 0 ||
				strcmp(te->desc, "VIEW") == 0 ||
				strcmp(te->desc, "FOREIGN TABLE") == 0 ||
				strcmp(te->desc, "MATERIALIZED VIEW") == 0 ||
				strcmp(te->desc, "MATERIALIZED VIEW DATA") == 0 ||
				strcmp(te->desc, "SEQUENCE") == 0 ||
				strcmp(te->desc, "SEQUENCE SET") == 0)
			{
				if (!ropt->selTable)
					return 0;
				if (ropt->tableNames.head != NULL &&
					!simple_string_list_member(&ropt->tableNames, te->tag))
					return 0;
			}
			else if (strcmp(te->desc, "INDEX") == 0)
			{
				if (!ropt->selIndex)
					return 0;
				if (ropt->indexNames.head != NULL &&
					!simple_string_list_member(&ropt->indexNames, te->tag))
					return 0;
			}
			else if (strcmp(te->desc, "FUNCTION") == 0 ||
					 strcmp(te->desc, "AGGREGATE") == 0 ||
					 strcmp(te->desc, "PROCEDURE") == 0)
			{
				if (!ropt->selFunction)
					return 0;
				if (ropt->functionNames.head != NULL &&
					!simple_string_list_member(&ropt->functionNames, te->tag))
					return 0;
			}
			else if (strcmp(te->desc, "TRIGGER") == 0)
			{
				if (!ropt->selTrigger)
					return 0;
				if (ropt->triggerNames.head != NULL &&
					!simple_string_list_member(&ropt->triggerNames, te->tag))
					return 0;
			}
			else
				return 0;
		}
	}

	/*
	 * Determine whether the TOC entry contains schema and/or data components,
	 * and mask off inapplicable REQ bits.  If it had a dataDumper, assume
	 * it's both schema and data.  Otherwise it's probably schema-only, but
	 * there are exceptions.
	 */
	if (!te->hadDumper)
	{
		/*
		 * Special Case: If 'SEQUENCE SET' or anything to do with BLOBs, then
		 * it is considered a data entry.  We don't need to check for the
		 * BLOBS entry or old-style BLOB COMMENTS, because they will have
		 * hadDumper = true ... but we do need to check new-style BLOB ACLs,
		 * comments, etc.
		 */
		if (strcmp(te->desc, "SEQUENCE SET") == 0 ||
			strcmp(te->desc, "BLOB") == 0 ||
			(strcmp(te->desc, "ACL") == 0 &&
			 strncmp(te->tag, "LARGE OBJECT ", 13) == 0) ||
			(strcmp(te->desc, "COMMENT") == 0 &&
			 strncmp(te->tag, "LARGE OBJECT ", 13) == 0) ||
			(strcmp(te->desc, "SECURITY LABEL") == 0 &&
			 strncmp(te->tag, "LARGE OBJECT ", 13) == 0))
			res = res & REQ_DATA;
		else
			res = res & ~REQ_DATA;
	}

	/* If there's no definition command, there's no schema component */
	if (!te->defn || !te->defn[0])
		res = res & ~REQ_SCHEMA;

	/*
	 * Special case: <Init> type with <Max OID> tag; this is obsolete and we
	 * always ignore it.
	 */
	if ((strcmp(te->desc, "<Init>") == 0) && (strcmp(te->tag, "Max OID") == 0))
		return 0;

	/* Mask it if we only want schema */
	if (ropt->schemaOnly)
	{
		/*
		 * The sequence_data option overrides schemaOnly for SEQUENCE SET.
		 *
		 * In binary-upgrade mode, even with schemaOnly set, we do not mask
		 * out large objects.  (Only large object definitions, comments and
		 * other metadata should be generated in binary-upgrade mode, not the
		 * actual data, but that need not concern us here.)
		 */
		if (!(ropt->sequence_data && strcmp(te->desc, "SEQUENCE SET") == 0) &&
			!(ropt->binary_upgrade &&
			  (strcmp(te->desc, "BLOB") == 0 ||
			   (strcmp(te->desc, "ACL") == 0 &&
				strncmp(te->tag, "LARGE OBJECT ", 13) == 0) ||
			   (strcmp(te->desc, "COMMENT") == 0 &&
				strncmp(te->tag, "LARGE OBJECT ", 13) == 0) ||
			   (strcmp(te->desc, "SECURITY LABEL") == 0 &&
				strncmp(te->tag, "LARGE OBJECT ", 13) == 0))))
			res = res & REQ_SCHEMA;
	}

	/* Mask it if we only want data */
	if (ropt->dataOnly)
		res = res & REQ_DATA;

	return res;
}

/*
 * Identify which pass we should restore this TOC entry in.
 *
 * See notes with the RestorePass typedef in pg_backup_archiver.h.
 */
static RestorePass
_tocEntryRestorePass(TocEntry *te)
{
	/* "ACL LANGUAGE" was a crock emitted only in PG 7.4 */
	if (strcmp(te->desc, "ACL") == 0 ||
		strcmp(te->desc, "ACL LANGUAGE") == 0 ||
		strcmp(te->desc, "DEFAULT ACL") == 0)
		return RESTORE_PASS_ACL;
	if (strcmp(te->desc, "EVENT TRIGGER") == 0 ||
		strcmp(te->desc, "MATERIALIZED VIEW DATA") == 0)
		return RESTORE_PASS_POST_ACL;

	/*
	 * Comments need to be emitted in the same pass as their parent objects.
	 * ACLs haven't got comments, and neither do matview data objects, but
	 * event triggers do.  (Fortunately, event triggers haven't got ACLs, or
	 * we'd need yet another weird special case.)
	 */
	if (strcmp(te->desc, "COMMENT") == 0 &&
		strncmp(te->tag, "EVENT TRIGGER ", 14) == 0)
		return RESTORE_PASS_POST_ACL;

	/* All else can be handled in the main pass. */
	return RESTORE_PASS_MAIN;
}

/*
 * Identify TOC entries that are ACLs.
 *
 * Note: it seems worth duplicating some code here to avoid a hard-wired
 * assumption that these are exactly the same entries that we restore during
 * the RESTORE_PASS_ACL phase.
 */
static bool
_tocEntryIsACL(TocEntry *te)
{
	/* "ACL LANGUAGE" was a crock emitted only in PG 7.4 */
	if (strcmp(te->desc, "ACL") == 0 ||
		strcmp(te->desc, "ACL LANGUAGE") == 0 ||
		strcmp(te->desc, "DEFAULT ACL") == 0)
		return true;
	return false;
}

/*
 * Issue SET commands for parameters that we want to have set the same way
 * at all times during execution of a restore script.
 */
static void
_doSetFixedOutputState(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->public.ropt;

	/*
	 * Disable timeouts to allow for slow commands, idle parallel workers, etc
	 */
	ahprintf(AH, "SET statement_timeout = 0;\n");
	ahprintf(AH, "SET lock_timeout = 0;\n");
	ahprintf(AH, "SET idle_in_transaction_session_timeout = 0;\n");

	/* Select the correct character set encoding */
	ahprintf(AH, "SET client_encoding = '%s';\n",
			 pg_encoding_to_char(AH->public.encoding));

	/* Select the correct string literal syntax */
	ahprintf(AH, "SET standard_conforming_strings = %s;\n",
			 AH->public.std_strings ? "on" : "off");

	/* Select the role to be used during restore */
	if (ropt && ropt->use_role)
		ahprintf(AH, "SET ROLE %s;\n", fmtId(ropt->use_role));

	/* Select the dump-time search_path */
	if (AH->public.searchpath)
		ahprintf(AH, "%s", AH->public.searchpath);

	/* Make sure function checking is disabled */
	ahprintf(AH, "SET check_function_bodies = false;\n");

	/* Ensure that all valid XML data will be accepted */
	ahprintf(AH, "SET xmloption = content;\n");

	/* Avoid annoying notices etc */
	ahprintf(AH, "SET client_min_messages = warning;\n");
	if (!AH->public.std_strings)
		ahprintf(AH, "SET escape_string_warning = off;\n");

	/* Adjust row-security state */
	if (ropt && ropt->enable_row_security)
		ahprintf(AH, "SET row_security = on;\n");
	else
		ahprintf(AH, "SET row_security = off;\n");

	ahprintf(AH, "\n");
}

/*
 * Issue a SET SESSION AUTHORIZATION command.  Caller is responsible
 * for updating state if appropriate.  If user is NULL or an empty string,
 * the specification DEFAULT will be used.
 */
static void
_doSetSessionAuth(ArchiveHandle *AH, const char *user)
{
	PQExpBuffer cmd = createPQExpBuffer();

	appendPQExpBufferStr(cmd, "SET SESSION AUTHORIZATION ");

	/*
	 * SQL requires a string literal here.  Might as well be correct.
	 */
	if (user && *user)
		appendStringLiteralAHX(cmd, user, AH);
	else
		appendPQExpBufferStr(cmd, "DEFAULT");
	appendPQExpBufferChar(cmd, ';');

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, cmd->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			/* NOT warn_or_exit_horribly... use -O instead to skip this. */
			fatal("could not set session user to \"%s\": %s",
				  user, PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s\n\n", cmd->data);

	destroyPQExpBuffer(cmd);
}


/*
 * Issue the commands to connect to the specified database.
 *
 * If we're currently restoring right into a database, this will
 * actually establish a connection. Otherwise it puts a \connect into
 * the script output.
 *
 * NULL dbname implies reconnecting to the current DB (pretty useless).
 */
static void
_reconnectToDB(ArchiveHandle *AH, const char *dbname)
{
	if (RestoringToDB(AH))
		ReconnectToServer(AH, dbname, NULL);
	else
	{
		if (dbname)
		{
			PQExpBufferData connectbuf;

			initPQExpBuffer(&connectbuf);
			appendPsqlMetaConnect(&connectbuf, dbname);
			ahprintf(AH, "%s\n", connectbuf.data);
			termPQExpBuffer(&connectbuf);
		}
		else
			ahprintf(AH, "%s\n", "\\connect -\n");
	}

	/*
	 * NOTE: currUser keeps track of what the imaginary session user in our
	 * script is.  It's now effectively reset to the original userID.
	 */
	if (AH->currUser)
		free(AH->currUser);
	AH->currUser = NULL;

	/* don't assume we still know the output schema, tablespace, etc either */
	if (AH->currSchema)
		free(AH->currSchema);
	AH->currSchema = NULL;
	if (AH->currTablespace)
		free(AH->currTablespace);
	AH->currTablespace = NULL;

	/* re-establish fixed state */
	_doSetFixedOutputState(AH);
}

/*
 * Become the specified user, and update state to avoid redundant commands
 *
 * NULL or empty argument is taken to mean restoring the session default
 */
static void
_becomeUser(ArchiveHandle *AH, const char *user)
{
	if (!user)
		user = "";				/* avoid null pointers */

	if (AH->currUser && strcmp(AH->currUser, user) == 0)
		return;					/* no need to do anything */

	_doSetSessionAuth(AH, user);

	/*
	 * NOTE: currUser keeps track of what the imaginary session user in our
	 * script is
	 */
	if (AH->currUser)
		free(AH->currUser);
	AH->currUser = pg_strdup(user);
}

/*
 * Become the owner of the given TOC entry object.  If
 * changes in ownership are not allowed, this doesn't do anything.
 */
static void
_becomeOwner(ArchiveHandle *AH, TocEntry *te)
{
	RestoreOptions *ropt = AH->public.ropt;

	if (ropt && (ropt->noOwner || !ropt->use_setsessauth))
		return;

	_becomeUser(AH, te->owner);
}


/*
 * Issue the commands to select the specified schema as the current schema
 * in the target database.
 */
static void
_selectOutputSchema(ArchiveHandle *AH, const char *schemaName)
{
	PQExpBuffer qry;

	/*
	 * If there was a SEARCHPATH TOC entry, we're supposed to just stay with
	 * that search_path rather than switching to entry-specific paths.
	 * Otherwise, it's an old archive that will not restore correctly unless
	 * we set the search_path as it's expecting.
	 */
	if (AH->public.searchpath)
		return;

	if (!schemaName || *schemaName == '\0' ||
		(AH->currSchema && strcmp(AH->currSchema, schemaName) == 0))
		return;					/* no need to do anything */

	qry = createPQExpBuffer();

	appendPQExpBuffer(qry, "SET search_path = %s",
					  fmtId(schemaName));
	if (strcmp(schemaName, "pg_catalog") != 0)
		appendPQExpBufferStr(qry, ", pg_catalog");

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, qry->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH,
								  "could not set search_path to \"%s\": %s",
								  schemaName, PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s;\n\n", qry->data);

	if (AH->currSchema)
		free(AH->currSchema);
	AH->currSchema = pg_strdup(schemaName);

	destroyPQExpBuffer(qry);
}

/*
 * Issue the commands to select the specified tablespace as the current one
 * in the target database.
 */
static void
_selectTablespace(ArchiveHandle *AH, const char *tablespace)
{
	RestoreOptions *ropt = AH->public.ropt;
	PQExpBuffer qry;
	const char *want,
			   *have;

	/* do nothing in --no-tablespaces mode */
	if (ropt->noTablespace)
		return;

	have = AH->currTablespace;
	want = tablespace;

	/* no need to do anything for non-tablespace object */
	if (!want)
		return;

	if (have && strcmp(want, have) == 0)
		return;					/* no need to do anything */

	qry = createPQExpBuffer();

	if (strcmp(want, "") == 0)
	{
		/* We want the tablespace to be the database's default */
		appendPQExpBufferStr(qry, "SET default_tablespace = ''");
	}
	else
	{
		/* We want an explicit tablespace */
		appendPQExpBuffer(qry, "SET default_tablespace = %s", fmtId(want));
	}

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, qry->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH,
								  "could not set default_tablespace to %s: %s",
								  fmtId(want), PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s;\n\n", qry->data);

	if (AH->currTablespace)
		free(AH->currTablespace);
	AH->currTablespace = pg_strdup(want);

	destroyPQExpBuffer(qry);
}

/*
 * Set the proper default_table_access_method value for the table.
 */
static void
_selectTableAccessMethod(ArchiveHandle *AH, const char *tableam)
{
	PQExpBuffer cmd;
	const char *want,
			   *have;

	have = AH->currTableAm;
	want = tableam;

	if (!want)
		return;

	if (have && strcmp(want, have) == 0)
		return;

	cmd = createPQExpBuffer();
	appendPQExpBuffer(cmd, "SET default_table_access_method = %s;", fmtId(want));

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, cmd->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH,
								  "could not set default_table_access_method: %s",
								  PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s\n\n", cmd->data);

	destroyPQExpBuffer(cmd);

	AH->currTableAm = pg_strdup(want);
}

/*
 * Extract an object description for a TOC entry, and append it to buf.
 *
 * This is used for ALTER ... OWNER TO.
 */
static void
_getObjectDescription(PQExpBuffer buf, TocEntry *te, ArchiveHandle *AH)
{
	const char *type = te->desc;

	/* Use ALTER TABLE for views and sequences */
	if (strcmp(type, "VIEW") == 0 || strcmp(type, "SEQUENCE") == 0 ||
		strcmp(type, "MATERIALIZED VIEW") == 0)
		type = "TABLE";

	/* objects that don't require special decoration */
	if (strcmp(type, "COLLATION") == 0 ||
		strcmp(type, "CONVERSION") == 0 ||
		strcmp(type, "DOMAIN") == 0 ||
		strcmp(type, "TABLE") == 0 ||
		strcmp(type, "TYPE") == 0 ||
		strcmp(type, "FOREIGN TABLE") == 0 ||
		strcmp(type, "TEXT SEARCH DICTIONARY") == 0 ||
		strcmp(type, "TEXT SEARCH CONFIGURATION") == 0 ||
		strcmp(type, "STATISTICS") == 0 ||
	/* non-schema-specified objects */
		strcmp(type, "DATABASE") == 0 ||
		strcmp(type, "PROCEDURAL LANGUAGE") == 0 ||
		strcmp(type, "SCHEMA") == 0 ||
		strcmp(type, "EVENT TRIGGER") == 0 ||
		strcmp(type, "FOREIGN DATA WRAPPER") == 0 ||
		strcmp(type, "SERVER") == 0 ||
		strcmp(type, "PUBLICATION") == 0 ||
		strcmp(type, "SUBSCRIPTION") == 0 ||
		strcmp(type, "USER MAPPING") == 0)
	{
		appendPQExpBuffer(buf, "%s ", type);
		if (te->namespace && *te->namespace)
			appendPQExpBuffer(buf, "%s.", fmtId(te->namespace));
		appendPQExpBufferStr(buf, fmtId(te->tag));
		return;
	}

	/* BLOBs just have a name, but it's numeric so must not use fmtId */
	if (strcmp(type, "BLOB") == 0)
	{
		appendPQExpBuffer(buf, "LARGE OBJECT %s", te->tag);
		return;
	}

	/*
	 * These object types require additional decoration.  Fortunately, the
	 * information needed is exactly what's in the DROP command.
	 */
	if (strcmp(type, "AGGREGATE") == 0 ||
		strcmp(type, "FUNCTION") == 0 ||
		strcmp(type, "OPERATOR") == 0 ||
		strcmp(type, "OPERATOR CLASS") == 0 ||
		strcmp(type, "OPERATOR FAMILY") == 0 ||
		strcmp(type, "PROCEDURE") == 0)
	{
		/* Chop "DROP " off the front and make a modifiable copy */
		char	   *first = pg_strdup(te->dropStmt + 5);
		char	   *last;

		/* point to last character in string */
		last = first + strlen(first) - 1;

		/* Strip off any ';' or '\n' at the end */
		while (last >= first && (*last == '\n' || *last == ';'))
			last--;
		*(last + 1) = '\0';

		appendPQExpBufferStr(buf, first);

		free(first);
		return;
	}

	pg_log_warning("don't know how to set owner for object type \"%s\"",
				   type);
}

/*
 * Emit the SQL commands to create the object represented by a TOC entry
 *
 * This now also includes issuing an ALTER OWNER command to restore the
 * object's ownership, if wanted.  But note that the object's permissions
 * will remain at default, until the matching ACL TOC entry is restored.
 */
static void
_printTocEntry(ArchiveHandle *AH, TocEntry *te, bool isData)
{
	RestoreOptions *ropt = AH->public.ropt;

	/* Select owner, schema, tablespace and default AM as necessary */
	_becomeOwner(AH, te);
	_selectOutputSchema(AH, te->namespace);
	_selectTablespace(AH, te->tablespace);
	_selectTableAccessMethod(AH, te->tableam);

	/* Emit header comment for item */
	if (!AH->noTocComments)
	{
		const char *pfx;
		char	   *sanitized_name;
		char	   *sanitized_schema;
		char	   *sanitized_owner;

		if (isData)
			pfx = "Data for ";
		else
			pfx = "";

		ahprintf(AH, "--\n");
		if (AH->public.verbose)
		{
			ahprintf(AH, "-- TOC entry %d (class %u OID %u)\n",
					 te->dumpId, te->catalogId.tableoid, te->catalogId.oid);
			if (te->nDeps > 0)
			{
				int			i;

				ahprintf(AH, "-- Dependencies:");
				for (i = 0; i < te->nDeps; i++)
					ahprintf(AH, " %d", te->dependencies[i]);
				ahprintf(AH, "\n");
			}
		}

		sanitized_name = sanitize_line(te->tag, false);
		sanitized_schema = sanitize_line(te->namespace, true);
		sanitized_owner = sanitize_line(ropt->noOwner ? NULL : te->owner, true);

		ahprintf(AH, "-- %sName: %s; Type: %s; Schema: %s; Owner: %s",
				 pfx, sanitized_name, te->desc, sanitized_schema,
				 sanitized_owner);

		free(sanitized_name);
		free(sanitized_schema);
		free(sanitized_owner);

		if (te->tablespace && strlen(te->tablespace) > 0 && !ropt->noTablespace)
		{
			char	   *sanitized_tablespace;

			sanitized_tablespace = sanitize_line(te->tablespace, false);
			ahprintf(AH, "; Tablespace: %s", sanitized_tablespace);
			free(sanitized_tablespace);
		}
		ahprintf(AH, "\n");

		if (AH->PrintExtraTocPtr != NULL)
			AH->PrintExtraTocPtr(AH, te);
		ahprintf(AH, "--\n\n");
	}

	/*
	 * Actually print the definition.
	 *
	 * Really crude hack for suppressing AUTHORIZATION clause that old pg_dump
	 * versions put into CREATE SCHEMA.  We have to do this when --no-owner
	 * mode is selected.  This is ugly, but I see no other good way ...
	 */
	if (ropt->noOwner && strcmp(te->desc, "SCHEMA") == 0)
	{
		ahprintf(AH, "CREATE SCHEMA %s;\n\n\n", fmtId(te->tag));
	}
	else
	{
		if (te->defn && strlen(te->defn) > 0)
			ahprintf(AH, "%s\n\n", te->defn);
	}

	/*
	 * If we aren't using SET SESSION AUTH to determine ownership, we must
	 * instead issue an ALTER OWNER command.  We assume that anything without
	 * a DROP command is not a separately ownable object.  All the categories
	 * with DROP commands must appear in one list or the other.
	 */
	if (!ropt->noOwner && !ropt->use_setsessauth &&
		te->owner && strlen(te->owner) > 0 &&
		te->dropStmt && strlen(te->dropStmt) > 0)
	{
		if (strcmp(te->desc, "AGGREGATE") == 0 ||
			strcmp(te->desc, "BLOB") == 0 ||
			strcmp(te->desc, "COLLATION") == 0 ||
			strcmp(te->desc, "CONVERSION") == 0 ||
			strcmp(te->desc, "DATABASE") == 0 ||
			strcmp(te->desc, "DOMAIN") == 0 ||
			strcmp(te->desc, "FUNCTION") == 0 ||
			strcmp(te->desc, "OPERATOR") == 0 ||
			strcmp(te->desc, "OPERATOR CLASS") == 0 ||
			strcmp(te->desc, "OPERATOR FAMILY") == 0 ||
			strcmp(te->desc, "PROCEDURE") == 0 ||
			strcmp(te->desc, "PROCEDURAL LANGUAGE") == 0 ||
			strcmp(te->desc, "SCHEMA") == 0 ||
			strcmp(te->desc, "EVENT TRIGGER") == 0 ||
			strcmp(te->desc, "TABLE") == 0 ||
			strcmp(te->desc, "TYPE") == 0 ||
			strcmp(te->desc, "VIEW") == 0 ||
			strcmp(te->desc, "MATERIALIZED VIEW") == 0 ||
			strcmp(te->desc, "SEQUENCE") == 0 ||
			strcmp(te->desc, "FOREIGN TABLE") == 0 ||
			strcmp(te->desc, "TEXT SEARCH DICTIONARY") == 0 ||
			strcmp(te->desc, "TEXT SEARCH CONFIGURATION") == 0 ||
			strcmp(te->desc, "FOREIGN DATA WRAPPER") == 0 ||
			strcmp(te->desc, "SERVER") == 0 ||
			strcmp(te->desc, "STATISTICS") == 0 ||
			strcmp(te->desc, "PUBLICATION") == 0 ||
			strcmp(te->desc, "SUBSCRIPTION") == 0)
		{
			PQExpBuffer temp = createPQExpBuffer();

			appendPQExpBufferStr(temp, "ALTER ");
			_getObjectDescription(temp, te, AH);
			appendPQExpBuffer(temp, " OWNER TO %s;", fmtId(te->owner));
			ahprintf(AH, "%s\n\n", temp->data);
			destroyPQExpBuffer(temp);
		}
		else if (strcmp(te->desc, "CAST") == 0 ||
				 strcmp(te->desc, "CHECK CONSTRAINT") == 0 ||
				 strcmp(te->desc, "CONSTRAINT") == 0 ||
				 strcmp(te->desc, "DATABASE PROPERTIES") == 0 ||
				 strcmp(te->desc, "DEFAULT") == 0 ||
				 strcmp(te->desc, "FK CONSTRAINT") == 0 ||
				 strcmp(te->desc, "INDEX") == 0 ||
				 strcmp(te->desc, "RULE") == 0 ||
				 strcmp(te->desc, "TRIGGER") == 0 ||
				 strcmp(te->desc, "ROW SECURITY") == 0 ||
				 strcmp(te->desc, "POLICY") == 0 ||
				 strcmp(te->desc, "USER MAPPING") == 0)
		{
			/* these object types don't have separate owners */
		}
		else
		{
			pg_log_warning("don't know how to set owner for object type \"%s\"",
						   te->desc);
		}
	}

	/*
	 * If it's an ACL entry, it might contain SET SESSION AUTHORIZATION
	 * commands, so we can no longer assume we know the current auth setting.
	 */
	if (_tocEntryIsACL(te))
	{
		if (AH->currUser)
			free(AH->currUser);
		AH->currUser = NULL;
	}
}

/*
 * Sanitize a string to be included in an SQL comment or TOC listing, by
 * replacing any newlines with spaces.  This ensures each logical output line
 * is in fact one physical output line, to prevent corruption of the dump
 * (which could, in the worst case, present an SQL injection vulnerability
 * if someone were to incautiously load a dump containing objects with
 * maliciously crafted names).
 *
 * The result is a freshly malloc'd string.  If the input string is NULL,
 * return a malloc'ed empty string, unless want_hyphen, in which case return a
 * malloc'ed hyphen.
 *
 * Note that we currently don't bother to quote names, meaning that the name
 * fields aren't automatically parseable.  "pg_restore -L" doesn't care because
 * it only examines the dumpId field, but someday we might want to try harder.
 */
static char *
sanitize_line(const char *str, bool want_hyphen)
{
	char	   *result;
	char	   *s;

	if (!str)
		return pg_strdup(want_hyphen ? "-" : "");

	result = pg_strdup(str);

	for (s = result; *s != '\0'; s++)
	{
		if (*s == '\n' || *s == '\r')
			*s = ' ';
	}

	return result;
}

/*
 * Write the file header for a custom-format archive
 */
void
WriteHead(ArchiveHandle *AH)
{
	struct tm	crtm;

	AH->WriteBufPtr(AH, "PGDMP", 5);	/* Magic code */
	AH->WriteBytePtr(AH, ARCHIVE_MAJOR(AH->version));
	AH->WriteBytePtr(AH, ARCHIVE_MINOR(AH->version));
	AH->WriteBytePtr(AH, ARCHIVE_REV(AH->version));
	AH->WriteBytePtr(AH, AH->intSize);
	AH->WriteBytePtr(AH, AH->offSize);
	AH->WriteBytePtr(AH, AH->format);
	WriteInt(AH, AH->compression);
	crtm = *localtime(&AH->createDate);
	WriteInt(AH, crtm.tm_sec);
	WriteInt(AH, crtm.tm_min);
	WriteInt(AH, crtm.tm_hour);
	WriteInt(AH, crtm.tm_mday);
	WriteInt(AH, crtm.tm_mon);
	WriteInt(AH, crtm.tm_year);
	WriteInt(AH, crtm.tm_isdst);
	WriteStr(AH, PQdb(AH->connection));
	WriteStr(AH, AH->public.remoteVersionStr);
	WriteStr(AH, PG_VERSION);
}

void
ReadHead(ArchiveHandle *AH)
{
	char		tmpMag[7];
	int			fmt;
	struct tm	crtm;

	/*
	 * If we haven't already read the header, do so.
	 *
	 * NB: this code must agree with _discoverArchiveFormat().  Maybe find a
	 * way to unify the cases?
	 */
	if (!AH->readHeader)
	{
		char		vmaj,
					vmin,
					vrev;

		AH->ReadBufPtr(AH, tmpMag, 5);

		if (strncmp(tmpMag, "PGDMP", 5) != 0)
			fatal("did not find magic string in file header");

		vmaj = AH->ReadBytePtr(AH);
		vmin = AH->ReadBytePtr(AH);

		if (vmaj > 1 || (vmaj == 1 && vmin > 0))	/* Version > 1.0 */
			vrev = AH->ReadBytePtr(AH);
		else
			vrev = 0;

		AH->version = MAKE_ARCHIVE_VERSION(vmaj, vmin, vrev);

		if (AH->version < K_VERS_1_0 || AH->version > K_VERS_MAX)
			fatal("unsupported version (%d.%d) in file header",
				  vmaj, vmin);

		AH->intSize = AH->ReadBytePtr(AH);
		if (AH->intSize > 32)
			fatal("sanity check on integer size (%lu) failed",
				  (unsigned long) AH->intSize);

		if (AH->intSize > sizeof(int))
			pg_log_warning("archive was made on a machine with larger integers, some operations might fail");

		if (AH->version >= K_VERS_1_7)
			AH->offSize = AH->ReadBytePtr(AH);
		else
			AH->offSize = AH->intSize;

		fmt = AH->ReadBytePtr(AH);

		if (AH->format != fmt)
			fatal("expected format (%d) differs from format found in file (%d)",
				  AH->format, fmt);
	}

	if (AH->version >= K_VERS_1_2)
	{
		if (AH->version < K_VERS_1_4)
			AH->compression = AH->ReadBytePtr(AH);
		else
			AH->compression = ReadInt(AH);
	}
	else
		AH->compression = Z_DEFAULT_COMPRESSION;

#ifndef HAVE_LIBZ
	if (AH->compression != 0)
		pg_log_warning("archive is compressed, but this installation does not support compression -- no data will be available");
#endif

	if (AH->version >= K_VERS_1_4)
	{
		crtm.tm_sec = ReadInt(AH);
		crtm.tm_min = ReadInt(AH);
		crtm.tm_hour = ReadInt(AH);
		crtm.tm_mday = ReadInt(AH);
		crtm.tm_mon = ReadInt(AH);
		crtm.tm_year = ReadInt(AH);
		crtm.tm_isdst = ReadInt(AH);

		AH->archdbname = ReadStr(AH);

		AH->createDate = mktime(&crtm);

		if (AH->createDate == (time_t) -1)
			pg_log_warning("invalid creation date in header");
	}

	if (AH->version >= K_VERS_1_10)
	{
		AH->archiveRemoteVersion = ReadStr(AH);
		AH->archiveDumpVersion = ReadStr(AH);
	}
}


/*
 * checkSeek
 *	  check to see if ftell/fseek can be performed.
 */
bool
checkSeek(FILE *fp)
{
	pgoff_t		tpos;

	/* Check that ftello works on this file */
	tpos = ftello(fp);
	if (tpos < 0)
		return false;

	/*
	 * Check that fseeko(SEEK_SET) works, too.  NB: we used to try to test
	 * this with fseeko(fp, 0, SEEK_CUR).  But some platforms treat that as a
	 * successful no-op even on files that are otherwise unseekable.
	 */
	if (fseeko(fp, tpos, SEEK_SET) != 0)
		return false;

	return true;
}


/*
 * dumpTimestamp
 */
static void
dumpTimestamp(ArchiveHandle *AH, const char *msg, time_t tim)
{
	char		buf[64];

	if (strftime(buf, sizeof(buf), PGDUMP_STRFTIME_FMT, localtime(&tim)) != 0)
		ahprintf(AH, "-- %s %s\n\n", msg, buf);
}

/*
 * Main engine for parallel restore.
 *
 * Parallel restore is done in three phases.  In this first phase,
 * we'll process all SECTION_PRE_DATA TOC entries that are allowed to be
 * processed in the RESTORE_PASS_MAIN pass.  (In practice, that's all
 * PRE_DATA items other than ACLs.)  Entries we can't process now are
 * added to the pending_list for later phases to deal with.
 */
static void
restore_toc_entries_prefork(ArchiveHandle *AH, TocEntry *pending_list)
{
	bool		skipped_some;
	TocEntry   *next_work_item;

	pg_log_debug("entering restore_toc_entries_prefork");

	/* Adjust dependency information */
	fix_dependencies(AH);

	/*
	 * Do all the early stuff in a single connection in the parent. There's no
	 * great point in running it in parallel, in fact it will actually run
	 * faster in a single connection because we avoid all the connection and
	 * setup overhead.  Also, pre-9.2 pg_dump versions were not very good
	 * about showing all the dependencies of SECTION_PRE_DATA items, so we do
	 * not risk trying to process them out-of-order.
	 *
	 * Stuff that we can't do immediately gets added to the pending_list.
	 * Note: we don't yet filter out entries that aren't going to be restored.
	 * They might participate in dependency chains connecting entries that
	 * should be restored, so we treat them as live until we actually process
	 * them.
	 *
	 * Note: as of 9.2, it should be guaranteed that all PRE_DATA items appear
	 * before DATA items, and all DATA items before POST_DATA items.  That is
	 * not certain to be true in older archives, though, and in any case use
	 * of a list file would destroy that ordering (cf. SortTocFromFile).  So
	 * this loop cannot assume that it holds.
	 */
	AH->restorePass = RESTORE_PASS_MAIN;
	skipped_some = false;
	for (next_work_item = AH->toc->next; next_work_item != AH->toc; next_work_item = next_work_item->next)
	{
		bool		do_now = true;

		if (next_work_item->section != SECTION_PRE_DATA)
		{
			/* DATA and POST_DATA items are just ignored for now */
			if (next_work_item->section == SECTION_DATA ||
				next_work_item->section == SECTION_POST_DATA)
			{
				do_now = false;
				skipped_some = true;
			}
			else
			{
				/*
				 * SECTION_NONE items, such as comments, can be processed now
				 * if we are still in the PRE_DATA part of the archive.  Once
				 * we've skipped any items, we have to consider whether the
				 * comment's dependencies are satisfied, so skip it for now.
				 */
				if (skipped_some)
					do_now = false;
			}
		}

		/*
		 * Also skip items that need to be forced into later passes.  We need
		 * not set skipped_some in this case, since by assumption no main-pass
		 * items could depend on these.
		 */
		if (_tocEntryRestorePass(next_work_item) != RESTORE_PASS_MAIN)
			do_now = false;

		if (do_now)
		{
			/* OK, restore the item and update its dependencies */
			pg_log_info("processing item %d %s %s",
						next_work_item->dumpId,
						next_work_item->desc, next_work_item->tag);

			(void) restore_toc_entry(AH, next_work_item, false);

			/* Reduce dependencies, but don't move anything to ready_list */
			reduce_dependencies(AH, next_work_item, NULL);
		}
		else
		{
			/* Nope, so add it to pending_list */
			pending_list_append(pending_list, next_work_item);
		}
	}

	/*
	 * Now close parent connection in prep for parallel steps.  We do this
	 * mainly to ensure that we don't exceed the specified number of parallel
	 * connections.
	 */
	DisconnectDatabase(&AH->public);

	/* blow away any transient state from the old connection */
	if (AH->currUser)
		free(AH->currUser);
	AH->currUser = NULL;
	if (AH->currSchema)
		free(AH->currSchema);
	AH->currSchema = NULL;
	if (AH->currTablespace)
		free(AH->currTablespace);
	AH->currTablespace = NULL;
	if (AH->currTableAm)
		free(AH->currTableAm);
	AH->currTableAm = NULL;
}

/*
 * Main engine for parallel restore.
 *
 * Parallel restore is done in three phases.  In this second phase,
 * we process entries by dispatching them to parallel worker children
 * (processes on Unix, threads on Windows), each of which connects
 * separately to the database.  Inter-entry dependencies are respected,
 * and so is the RestorePass multi-pass structure.  When we can no longer
 * make any entries ready to process, we exit.  Normally, there will be
 * nothing left to do; but if there is, the third phase will mop up.
 */
static void
restore_toc_entries_parallel(ArchiveHandle *AH, ParallelState *pstate,
							 TocEntry *pending_list)
{
	ParallelReadyList ready_list;
	TocEntry   *next_work_item;

	pg_log_debug("entering restore_toc_entries_parallel");

	/* Set up ready_list with enough room for all known TocEntrys */
	ready_list_init(&ready_list, AH->tocCount);

	/*
	 * The pending_list contains all items that we need to restore.  Move all
	 * items that are available to process immediately into the ready_list.
	 * After this setup, the pending list is everything that needs to be done
	 * but is blocked by one or more dependencies, while the ready list
	 * contains items that have no remaining dependencies and are OK to
	 * process in the current restore pass.
	 */
	AH->restorePass = RESTORE_PASS_MAIN;
	move_to_ready_list(pending_list, &ready_list, AH->restorePass);

	/*
	 * main parent loop
	 *
	 * Keep going until there is no worker still running AND there is no work
	 * left to be done.  Note invariant: at top of loop, there should always
	 * be at least one worker available to dispatch a job to.
	 */
	pg_log_info("entering main parallel loop");

	for (;;)
	{
		/* Look for an item ready to be dispatched to a worker */
		next_work_item = pop_next_work_item(AH, &ready_list, pstate);
		if (next_work_item != NULL)
		{
			/* If not to be restored, don't waste time launching a worker */
			if ((next_work_item->reqs & (REQ_SCHEMA | REQ_DATA)) == 0)
			{
				pg_log_info("skipping item %d %s %s",
							next_work_item->dumpId,
							next_work_item->desc, next_work_item->tag);
				/* Update its dependencies as though we'd completed it */
				reduce_dependencies(AH, next_work_item, &ready_list);
				/* Loop around to see if anything else can be dispatched */
				continue;
			}

			pg_log_info("launching item %d %s %s",
						next_work_item->dumpId,
						next_work_item->desc, next_work_item->tag);

			/* Dispatch to some worker */
			DispatchJobForTocEntry(AH, pstate, next_work_item, ACT_RESTORE,
								   mark_restore_job_done, &ready_list);
		}
		else if (IsEveryWorkerIdle(pstate))
		{
			/*
			 * Nothing is ready and no worker is running, so we're done with
			 * the current pass or maybe with the whole process.
			 */
			if (AH->restorePass == RESTORE_PASS_LAST)
				break;			/* No more parallel processing is possible */

			/* Advance to next restore pass */
			AH->restorePass++;
			/* That probably allows some stuff to be made ready */
			move_to_ready_list(pending_list, &ready_list, AH->restorePass);
			/* Loop around to see if anything's now ready */
			continue;
		}
		else
		{
			/*
			 * We have nothing ready, but at least one child is working, so
			 * wait for some subjob to finish.
			 */
		}

		/*
		 * Before dispatching another job, check to see if anything has
		 * finished.  We should check every time through the loop so as to
		 * reduce dependencies as soon as possible.  If we were unable to
		 * dispatch any job this time through, wait until some worker finishes
		 * (and, hopefully, unblocks some pending item).  If we did dispatch
		 * something, continue as soon as there's at least one idle worker.
		 * Note that in either case, there's guaranteed to be at least one
		 * idle worker when we return to the top of the loop.  This ensures we
		 * won't block inside DispatchJobForTocEntry, which would be
		 * undesirable: we'd rather postpone dispatching until we see what's
		 * been unblocked by finished jobs.
		 */
		WaitForWorkers(AH, pstate,
					   next_work_item ? WFW_ONE_IDLE : WFW_GOT_STATUS);
	}

	/* There should now be nothing in ready_list. */
	Assert(ready_list.first_te > ready_list.last_te);

	ready_list_free(&ready_list);

	pg_log_info("finished main parallel loop");
}

/*
 * Main engine for parallel restore.
 *
 * Parallel restore is done in three phases.  In this third phase,
 * we mop up any remaining TOC entries by processing them serially.
 * This phase normally should have nothing to do, but if we've somehow
 * gotten stuck due to circular dependencies or some such, this provides
 * at least some chance of completing the restore successfully.
 */
static void
restore_toc_entries_postfork(ArchiveHandle *AH, TocEntry *pending_list)
{
	RestoreOptions *ropt = AH->public.ropt;
	TocEntry   *te;

	pg_log_debug("entering restore_toc_entries_postfork");

	/*
	 * Now reconnect the single parent connection.
	 */
	ConnectDatabase((Archive *) AH, ropt->dbname,
					ropt->pghost, ropt->pgport, ropt->username,
					ropt->promptPassword);

	/* re-establish fixed state */
	_doSetFixedOutputState(AH);

	/*
	 * Make sure there is no work left due to, say, circular dependencies, or
	 * some other pathological condition.  If so, do it in the single parent
	 * connection.  We don't sweat about RestorePass ordering; it's likely we
	 * already violated that.
	 */
	for (te = pending_list->pending_next; te != pending_list; te = te->pending_next)
	{
		pg_log_info("processing missed item %d %s %s",
					te->dumpId, te->desc, te->tag);
		(void) restore_toc_entry(AH, te, false);
	}
}

/*
 * Check if te1 has an exclusive lock requirement for an item that te2 also
 * requires, whether or not te2's requirement is for an exclusive lock.
 */
static bool
has_lock_conflicts(TocEntry *te1, TocEntry *te2)
{
	int			j,
				k;

	for (j = 0; j < te1->nLockDeps; j++)
	{
		for (k = 0; k < te2->nDeps; k++)
		{
			if (te1->lockDeps[j] == te2->dependencies[k])
				return true;
		}
	}
	return false;
}


/*
 * Initialize the header of the pending-items list.
 *
 * This is a circular list with a dummy TocEntry as header, just like the
 * main TOC list; but we use separate list links so that an entry can be in
 * the main TOC list as well as in the pending list.
 */
static void
pending_list_header_init(TocEntry *l)
{
	l->pending_prev = l->pending_next = l;
}

/* Append te to the end of the pending-list headed by l */
static void
pending_list_append(TocEntry *l, TocEntry *te)
{
	te->pending_prev = l->pending_prev;
	l->pending_prev->pending_next = te;
	l->pending_prev = te;
	te->pending_next = l;
}

/* Remove te from the pending-list */
static void
pending_list_remove(TocEntry *te)
{
	te->pending_prev->pending_next = te->pending_next;
	te->pending_next->pending_prev = te->pending_prev;
	te->pending_prev = NULL;
	te->pending_next = NULL;
}


/*
 * Initialize the ready_list with enough room for up to tocCount entries.
 */
static void
ready_list_init(ParallelReadyList *ready_list, int tocCount)
{
	ready_list->tes = (TocEntry **)
		pg_malloc(tocCount * sizeof(TocEntry *));
	ready_list->first_te = 0;
	ready_list->last_te = -1;
	ready_list->sorted = false;
}

/*
 * Free storage for a ready_list.
 */
static void
ready_list_free(ParallelReadyList *ready_list)
{
	pg_free(ready_list->tes);
}

/* Add te to the ready_list */
static void
ready_list_insert(ParallelReadyList *ready_list, TocEntry *te)
{
	ready_list->tes[++ready_list->last_te] = te;
	/* List is (probably) not sorted anymore. */
	ready_list->sorted = false;
}

/* Remove the i'th entry in the ready_list */
static void
ready_list_remove(ParallelReadyList *ready_list, int i)
{
	int			f = ready_list->first_te;

	Assert(i >= f && i <= ready_list->last_te);

	/*
	 * In the typical case where the item to be removed is the first ready
	 * entry, we need only increment first_te to remove it.  Otherwise, move
	 * the entries before it to compact the list.  (This preserves sortedness,
	 * if any.)  We could alternatively move the entries after i, but there
	 * are typically many more of those.
	 */
	if (i > f)
	{
		TocEntry  **first_te_ptr = &ready_list->tes[f];

		memmove(first_te_ptr + 1, first_te_ptr, (i - f) * sizeof(TocEntry *));
	}
	ready_list->first_te++;
}

/* Sort the ready_list into the desired order */
static void
ready_list_sort(ParallelReadyList *ready_list)
{
	if (!ready_list->sorted)
	{
		int			n = ready_list->last_te - ready_list->first_te + 1;

		if (n > 1)
			qsort(ready_list->tes + ready_list->first_te, n,
				  sizeof(TocEntry *),
				  TocEntrySizeCompare);
		ready_list->sorted = true;
	}
}

/* qsort comparator for sorting TocEntries by dataLength */
static int
TocEntrySizeCompare(const void *p1, const void *p2)
{
	const TocEntry *te1 = *(const TocEntry *const *) p1;
	const TocEntry *te2 = *(const TocEntry *const *) p2;

	/* Sort by decreasing dataLength */
	if (te1->dataLength > te2->dataLength)
		return -1;
	if (te1->dataLength < te2->dataLength)
		return 1;

	/* For equal dataLengths, sort by dumpId, just to be stable */
	if (te1->dumpId < te2->dumpId)
		return -1;
	if (te1->dumpId > te2->dumpId)
		return 1;

	return 0;
}


/*
 * Move all immediately-ready items from pending_list to ready_list.
 *
 * Items are considered ready if they have no remaining dependencies and
 * they belong in the current restore pass.  (See also reduce_dependencies,
 * which applies the same logic one-at-a-time.)
 */
static void
move_to_ready_list(TocEntry *pending_list,
				   ParallelReadyList *ready_list,
				   RestorePass pass)
{
	TocEntry   *te;
	TocEntry   *next_te;

	for (te = pending_list->pending_next; te != pending_list; te = next_te)
	{
		/* must save list link before possibly removing te from list */
		next_te = te->pending_next;

		if (te->depCount == 0 &&
			_tocEntryRestorePass(te) == pass)
		{
			/* Remove it from pending_list ... */
			pending_list_remove(te);
			/* ... and add to ready_list */
			ready_list_insert(ready_list, te);
		}
	}
}

/*
 * Find the next work item (if any) that is capable of being run now,
 * and remove it from the ready_list.
 *
 * Returns the item, or NULL if nothing is runnable.
 *
 * To qualify, the item must have no remaining dependencies
 * and no requirements for locks that are incompatible with
 * items currently running.  Items in the ready_list are known to have
 * no remaining dependencies, but we have to check for lock conflicts.
 */
static TocEntry *
pop_next_work_item(ArchiveHandle *AH, ParallelReadyList *ready_list,
				   ParallelState *pstate)
{
	/*
	 * Sort the ready_list so that we'll tackle larger jobs first.
	 */
	ready_list_sort(ready_list);

	/*
	 * Search the ready_list until we find a suitable item.
	 */
	for (int i = ready_list->first_te; i <= ready_list->last_te; i++)
	{
		TocEntry   *te = ready_list->tes[i];
		bool		conflicts = false;

		/*
		 * Check to see if the item would need exclusive lock on something
		 * that a currently running item also needs lock on, or vice versa. If
		 * so, we don't want to schedule them together.
		 */
		for (int k = 0; k < pstate->numWorkers; k++)
		{
			TocEntry   *running_te = pstate->te[k];

			if (running_te == NULL)
				continue;
			if (has_lock_conflicts(te, running_te) ||
				has_lock_conflicts(running_te, te))
			{
				conflicts = true;
				break;
			}
		}

		if (conflicts)
			continue;

		/* passed all tests, so this item can run */
		ready_list_remove(ready_list, i);
		return te;
	}

	pg_log_debug("no item ready");
	return NULL;
}


/*
 * Restore a single TOC item in parallel with others
 *
 * this is run in the worker, i.e. in a thread (Windows) or a separate process
 * (everything else). A worker process executes several such work items during
 * a parallel backup or restore. Once we terminate here and report back that
 * our work is finished, the master process will assign us a new work item.
 */
int
parallel_restore(ArchiveHandle *AH, TocEntry *te)
{
	int			status;

	Assert(AH->connection != NULL);

	/* Count only errors associated with this TOC entry */
	AH->public.n_errors = 0;

	/* Restore the TOC item */
	status = restore_toc_entry(AH, te, true);

	return status;
}


/*
 * Callback function that's invoked in the master process after a step has
 * been parallel restored.
 *
 * Update status and reduce the dependency count of any dependent items.
 */
static void
mark_restore_job_done(ArchiveHandle *AH,
					  TocEntry *te,
					  int status,
					  void *callback_data)
{
	ParallelReadyList *ready_list = (ParallelReadyList *) callback_data;

	pg_log_info("finished item %d %s %s",
				te->dumpId, te->desc, te->tag);

	if (status == WORKER_CREATE_DONE)
		mark_create_done(AH, te);
	else if (status == WORKER_INHIBIT_DATA)
	{
		inhibit_data_for_failed_table(AH, te);
		AH->public.n_errors++;
	}
	else if (status == WORKER_IGNORED_ERRORS)
		AH->public.n_errors++;
	else if (status != 0)
		fatal("worker process failed: exit code %d",
			  status);

	reduce_dependencies(AH, te, ready_list);
}


/*
 * Process the dependency information into a form useful for parallel restore.
 *
 * This function takes care of fixing up some missing or badly designed
 * dependencies, and then prepares subsidiary data structures that will be
 * used in the main parallel-restore logic, including:
 * 1. We build the revDeps[] arrays of incoming dependency dumpIds.
 * 2. We set up depCount fields that are the number of as-yet-unprocessed
 * dependencies for each TOC entry.
 *
 * We also identify locking dependencies so that we can avoid trying to
 * schedule conflicting items at the same time.
 */
static void
fix_dependencies(ArchiveHandle *AH)
{
	TocEntry   *te;
	int			i;

	/*
	 * Initialize the depCount/revDeps/nRevDeps fields, and make sure the TOC
	 * items are marked as not being in any parallel-processing list.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		te->depCount = te->nDeps;
		te->revDeps = NULL;
		te->nRevDeps = 0;
		te->pending_prev = NULL;
		te->pending_next = NULL;
	}

	/*
	 * POST_DATA items that are shown as depending on a table need to be
	 * re-pointed to depend on that table's data, instead.  This ensures they
	 * won't get scheduled until the data has been loaded.
	 */
	repoint_table_dependencies(AH);

	/*
	 * Pre-8.4 versions of pg_dump neglected to set up a dependency from BLOB
	 * COMMENTS to BLOBS.  Cope.  (We assume there's only one BLOBS and only
	 * one BLOB COMMENTS in such files.)
	 */
	if (AH->version < K_VERS_1_11)
	{
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if (strcmp(te->desc, "BLOB COMMENTS") == 0 && te->nDeps == 0)
			{
				TocEntry   *te2;

				for (te2 = AH->toc->next; te2 != AH->toc; te2 = te2->next)
				{
					if (strcmp(te2->desc, "BLOBS") == 0)
					{
						te->dependencies = (DumpId *) pg_malloc(sizeof(DumpId));
						te->dependencies[0] = te2->dumpId;
						te->nDeps++;
						te->depCount++;
						break;
					}
				}
				break;
			}
		}
	}

	/*
	 * At this point we start to build the revDeps reverse-dependency arrays,
	 * so all changes of dependencies must be complete.
	 */

	/*
	 * Count the incoming dependencies for each item.  Also, it is possible
	 * that the dependencies list items that are not in the archive at all
	 * (that should not happen in 9.2 and later, but is highly likely in older
	 * archives).  Subtract such items from the depCounts.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		for (i = 0; i < te->nDeps; i++)
		{
			DumpId		depid = te->dependencies[i];

			if (depid <= AH->maxDumpId && AH->tocsByDumpId[depid] != NULL)
				AH->tocsByDumpId[depid]->nRevDeps++;
			else
				te->depCount--;
		}
	}

	/*
	 * Allocate space for revDeps[] arrays, and reset nRevDeps so we can use
	 * it as a counter below.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (te->nRevDeps > 0)
			te->revDeps = (DumpId *) pg_malloc(te->nRevDeps * sizeof(DumpId));
		te->nRevDeps = 0;
	}

	/*
	 * Build the revDeps[] arrays of incoming-dependency dumpIds.  This had
	 * better agree with the loops above.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		for (i = 0; i < te->nDeps; i++)
		{
			DumpId		depid = te->dependencies[i];

			if (depid <= AH->maxDumpId && AH->tocsByDumpId[depid] != NULL)
			{
				TocEntry   *otherte = AH->tocsByDumpId[depid];

				otherte->revDeps[otherte->nRevDeps++] = te->dumpId;
			}
		}
	}

	/*
	 * Lastly, work out the locking dependencies.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		te->lockDeps = NULL;
		te->nLockDeps = 0;
		identify_locking_dependencies(AH, te);
	}
}

/*
 * Change dependencies on table items to depend on table data items instead,
 * but only in POST_DATA items.
 *
 * Also, for any item having such dependency(s), set its dataLength to the
 * largest dataLength of the table data items it depends on.  This ensures
 * that parallel restore will prioritize larger jobs (index builds, FK
 * constraint checks, etc) over smaller ones, avoiding situations where we
 * end a restore with only one active job working on a large table.
 */
static void
repoint_table_dependencies(ArchiveHandle *AH)
{
	TocEntry   *te;
	int			i;
	DumpId		olddep;

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (te->section != SECTION_POST_DATA)
			continue;
		for (i = 0; i < te->nDeps; i++)
		{
			olddep = te->dependencies[i];
			if (olddep <= AH->maxDumpId &&
				AH->tableDataId[olddep] != 0)
			{
				DumpId		tabledataid = AH->tableDataId[olddep];
				TocEntry   *tabledatate = AH->tocsByDumpId[tabledataid];

				te->dependencies[i] = tabledataid;
				te->dataLength = Max(te->dataLength, tabledatate->dataLength);
				pg_log_debug("transferring dependency %d -> %d to %d",
							 te->dumpId, olddep, tabledataid);
			}
		}
	}
}

/*
 * Identify which objects we'll need exclusive lock on in order to restore
 * the given TOC entry (*other* than the one identified by the TOC entry
 * itself).  Record their dump IDs in the entry's lockDeps[] array.
 */
static void
identify_locking_dependencies(ArchiveHandle *AH, TocEntry *te)
{
	DumpId	   *lockids;
	int			nlockids;
	int			i;

	/*
	 * We only care about this for POST_DATA items.  PRE_DATA items are not
	 * run in parallel, and DATA items are all independent by assumption.
	 */
	if (te->section != SECTION_POST_DATA)
		return;

	/* Quick exit if no dependencies at all */
	if (te->nDeps == 0)
		return;

	/*
	 * Most POST_DATA items are ALTER TABLEs or some moral equivalent of that,
	 * and hence require exclusive lock.  However, we know that CREATE INDEX
	 * does not.  (Maybe someday index-creating CONSTRAINTs will fall in that
	 * category too ... but today is not that day.)
	 */
	if (strcmp(te->desc, "INDEX") == 0)
		return;

	/*
	 * We assume the entry requires exclusive lock on each TABLE or TABLE DATA
	 * item listed among its dependencies.  Originally all of these would have
	 * been TABLE items, but repoint_table_dependencies would have repointed
	 * them to the TABLE DATA items if those are present (which they might not
	 * be, eg in a schema-only dump).  Note that all of the entries we are
	 * processing here are POST_DATA; otherwise there might be a significant
	 * difference between a dependency on a table and a dependency on its
	 * data, so that closer analysis would be needed here.
	 */
	lockids = (DumpId *) pg_malloc(te->nDeps * sizeof(DumpId));
	nlockids = 0;
	for (i = 0; i < te->nDeps; i++)
	{
		DumpId		depid = te->dependencies[i];

		if (depid <= AH->maxDumpId && AH->tocsByDumpId[depid] != NULL &&
			((strcmp(AH->tocsByDumpId[depid]->desc, "TABLE DATA") == 0) ||
			 strcmp(AH->tocsByDumpId[depid]->desc, "TABLE") == 0))
			lockids[nlockids++] = depid;
	}

	if (nlockids == 0)
	{
		free(lockids);
		return;
	}

	te->lockDeps = pg_realloc(lockids, nlockids * sizeof(DumpId));
	te->nLockDeps = nlockids;
}

/*
 * Remove the specified TOC entry from the depCounts of items that depend on
 * it, thereby possibly making them ready-to-run.  Any pending item that
 * becomes ready should be moved to the ready_list, if that's provided.
 */
static void
reduce_dependencies(ArchiveHandle *AH, TocEntry *te,
					ParallelReadyList *ready_list)
{
	int			i;

	pg_log_debug("reducing dependencies for %d", te->dumpId);

	for (i = 0; i < te->nRevDeps; i++)
	{
		TocEntry   *otherte = AH->tocsByDumpId[te->revDeps[i]];

		Assert(otherte->depCount > 0);
		otherte->depCount--;

		/*
		 * It's ready if it has no remaining dependencies, and it belongs in
		 * the current restore pass, and it is currently a member of the
		 * pending list (that check is needed to prevent double restore in
		 * some cases where a list-file forces out-of-order restoring).
		 * However, if ready_list == NULL then caller doesn't want any list
		 * memberships changed.
		 */
		if (otherte->depCount == 0 &&
			_tocEntryRestorePass(otherte) == AH->restorePass &&
			otherte->pending_prev != NULL &&
			ready_list != NULL)
		{
			/* Remove it from pending list ... */
			pending_list_remove(otherte);
			/* ... and add to ready_list */
			ready_list_insert(ready_list, otherte);
		}
	}
}

/*
 * Set the created flag on the DATA member corresponding to the given
 * TABLE member
 */
static void
mark_create_done(ArchiveHandle *AH, TocEntry *te)
{
	if (AH->tableDataId[te->dumpId] != 0)
	{
		TocEntry   *ted = AH->tocsByDumpId[AH->tableDataId[te->dumpId]];

		ted->created = true;
	}
}

/*
 * Mark the DATA member corresponding to the given TABLE member
 * as not wanted
 */
static void
inhibit_data_for_failed_table(ArchiveHandle *AH, TocEntry *te)
{
	pg_log_info("table \"%s\" could not be created, will not restore its data",
				te->tag);

	if (AH->tableDataId[te->dumpId] != 0)
	{
		TocEntry   *ted = AH->tocsByDumpId[AH->tableDataId[te->dumpId]];

		ted->reqs = 0;
	}
}

/*
 * Clone and de-clone routines used in parallel restoration.
 *
 * Enough of the structure is cloned to ensure that there is no
 * conflict between different threads each with their own clone.
 */
ArchiveHandle *
CloneArchive(ArchiveHandle *AH)
{
	ArchiveHandle *clone;

	/* Make a "flat" copy */
	clone = (ArchiveHandle *) pg_malloc(sizeof(ArchiveHandle));
	memcpy(clone, AH, sizeof(ArchiveHandle));

	/* Handle format-independent fields */
	memset(&(clone->sqlparse), 0, sizeof(clone->sqlparse));

	/* The clone will have its own connection, so disregard connection state */
	clone->connection = NULL;
	clone->connCancel = NULL;
	clone->currUser = NULL;
	clone->currSchema = NULL;
	clone->currTablespace = NULL;

	/* savedPassword must be local in case we change it while connecting */
	if (clone->savedPassword)
		clone->savedPassword = pg_strdup(clone->savedPassword);

	/* clone has its own error count, too */
	clone->public.n_errors = 0;

	/*
	 * Connect our new clone object to the database: In parallel restore the
	 * parent is already disconnected, because we can connect the worker
	 * processes independently to the database (no snapshot sync required). In
	 * parallel backup we clone the parent's existing connection.
	 */
	if (AH->mode == archModeRead)
	{
		RestoreOptions *ropt = AH->public.ropt;

		Assert(AH->connection == NULL);

		/* this also sets clone->connection */
		ConnectDatabase((Archive *) clone, ropt->dbname,
						ropt->pghost, ropt->pgport, ropt->username,
						ropt->promptPassword);

		/* re-establish fixed state */
		_doSetFixedOutputState(clone);
	}
	else
	{
		PQExpBufferData connstr;
		char	   *pghost;
		char	   *pgport;
		char	   *username;

		Assert(AH->connection != NULL);

		/*
		 * Even though we are technically accessing the parent's database
		 * object here, these functions are fine to be called like that
		 * because all just return a pointer and do not actually send/receive
		 * any data to/from the database.
		 */
		initPQExpBuffer(&connstr);
		appendPQExpBufferStr(&connstr, "dbname=");
		appendConnStrVal(&connstr, PQdb(AH->connection));
		pghost = PQhost(AH->connection);
		pgport = PQport(AH->connection);
		username = PQuser(AH->connection);

		/* this also sets clone->connection */
		ConnectDatabase((Archive *) clone, connstr.data,
						pghost, pgport, username, TRI_NO);

		termPQExpBuffer(&connstr);
		/* setupDumpWorker will fix up connection state */
	}

	/* Let the format-specific code have a chance too */
	clone->ClonePtr(clone);

	Assert(clone->connection != NULL);
	return clone;
}

/*
 * Release clone-local storage.
 *
 * Note: we assume any clone-local connection was already closed.
 */
void
DeCloneArchive(ArchiveHandle *AH)
{
	/* Should not have an open database connection */
	Assert(AH->connection == NULL);

	/* Clear format-specific state */
	AH->DeClonePtr(AH);

	/* Clear state allocated by CloneArchive */
	if (AH->sqlparse.curCmd)
		destroyPQExpBuffer(AH->sqlparse.curCmd);

	/* Clear any connection-local state */
	if (AH->currUser)
		free(AH->currUser);
	if (AH->currSchema)
		free(AH->currSchema);
	if (AH->currTablespace)
		free(AH->currTablespace);
	if (AH->currTableAm)
		free(AH->currTableAm);
	if (AH->savedPassword)
		free(AH->savedPassword);

	free(AH);
}
