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

#include "parallel.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"
#include "pg_backup_utils.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef WIN32
#include <io.h>
#endif

#include "libpq/libpq-fs.h"

#define TEXT_DUMP_HEADER "--\n-- PostgreSQL database dump\n--\n\n"
#define TEXT_DUMPALL_HEADER "--\n-- PostgreSQL database cluster dump\n--\n\n"

/* state needed to save/restore an archive's output target */
typedef struct _outputContext
{
	void	   *OF;
	int			gzOut;
} OutputContext;

/* translator: this is a module name */
static const char *modulename = gettext_noop("archiver");


static ArchiveHandle *_allocAH(const char *FileSpec, const ArchiveFormat fmt,
	 const int compression, ArchiveMode mode, SetupWorkerPtr setupWorkerPtr);
static void _getObjectDescription(PQExpBuffer buf, TocEntry *te,
					  ArchiveHandle *AH);
static void _printTocEntry(ArchiveHandle *AH, TocEntry *te, bool isData, bool acl_pass);
static char *replace_line_endings(const char *str);
static void _doSetFixedOutputState(ArchiveHandle *AH);
static void _doSetSessionAuth(ArchiveHandle *AH, const char *user);
static void _doSetWithOids(ArchiveHandle *AH, const bool withOids);
static void _reconnectToDB(ArchiveHandle *AH, const char *dbname);
static void _becomeUser(ArchiveHandle *AH, const char *user);
static void _becomeOwner(ArchiveHandle *AH, TocEntry *te);
static void _selectOutputSchema(ArchiveHandle *AH, const char *schemaName);
static void _selectTablespace(ArchiveHandle *AH, const char *tablespace);
static void processEncodingEntry(ArchiveHandle *AH, TocEntry *te);
static void processStdStringsEntry(ArchiveHandle *AH, TocEntry *te);
static teReqs _tocEntryRequired(TocEntry *te, teSection curSection, RestoreOptions *ropt);
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
static void restore_toc_entries_prefork(ArchiveHandle *AH);
static void restore_toc_entries_parallel(ArchiveHandle *AH, ParallelState *pstate,
							 TocEntry *pending_list);
static void restore_toc_entries_postfork(ArchiveHandle *AH, TocEntry *pending_list);
static void par_list_header_init(TocEntry *l);
static void par_list_append(TocEntry *l, TocEntry *te);
static void par_list_remove(TocEntry *te);
static TocEntry *get_next_work_item(ArchiveHandle *AH,
				   TocEntry *ready_list,
				   ParallelState *pstate);
static void mark_work_done(ArchiveHandle *AH, TocEntry *ready_list,
			   int worker, int status,
			   ParallelState *pstate);
static void fix_dependencies(ArchiveHandle *AH);
static bool has_lock_conflicts(TocEntry *te1, TocEntry *te2);
static void repoint_table_dependencies(ArchiveHandle *AH);
static void identify_locking_dependencies(ArchiveHandle *AH, TocEntry *te);
static void reduce_dependencies(ArchiveHandle *AH, TocEntry *te,
					TocEntry *ready_list);
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
	dopt->no_security_labels = ropt->no_security_labels;
	dopt->lockWaitTimeout = ropt->lockWaitTimeout;
	dopt->include_everything = ropt->include_everything;
	dopt->enable_row_security = ropt->enable_row_security;

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

	(AH->ReopenPtr) (AH);
}


/* Create a new archive */
/* Public */
Archive *
CreateArchive(const char *FileSpec, const ArchiveFormat fmt,
	 const int compression, ArchiveMode mode, SetupWorkerPtr setupDumpWorker)

{
	ArchiveHandle *AH = _allocAH(FileSpec, fmt, compression, mode, setupDumpWorker);

	return (Archive *) AH;
}

/* Open an existing archive */
/* Public */
Archive *
OpenArchive(const char *FileSpec, const ArchiveFormat fmt)
{
	ArchiveHandle *AH = _allocAH(FileSpec, fmt, 0, archModeRead, setupRestoreWorker);

	return (Archive *) AH;
}

/* Public */
void
CloseArchive(Archive *AHX)
{
	int			res = 0;
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	(*AH->ClosePtr) (AH);

	/* Close the output */
	if (AH->gzOut)
		res = GZCLOSE(AH->OF);
	else if (AH->OF != stdout)
		res = fclose(AH->OF);

	if (res != 0)
		exit_horribly(modulename, "could not close output file: %s\n",
					  strerror(errno));
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
						write_msg(modulename,
								  "WARNING: archive items not in correct section order\n");
					break;
				case SECTION_DATA:
					if (curSection == SECTION_POST_DATA)
						write_msg(modulename,
								  "WARNING: archive items not in correct section order\n");
					break;
				case SECTION_POST_DATA:
					/* ok no matter which section we were in */
					break;
				default:
					exit_horribly(modulename, "unexpected section code %d\n",
								  (int) te->section);
					break;
			}
		}

		if (te->section != SECTION_NONE)
			curSection = te->section;

		te->reqs = _tocEntryRequired(te, curSection, ropt);
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
	 * Check for nonsensical option combinations.
	 *
	 * -C is not compatible with -1, because we can't create a database inside
	 * a transaction block.
	 */
	if (ropt->createDB && ropt->single_txn)
		exit_horribly(modulename, "-C and -1 are incompatible options\n");

	/*
	 * If we're going to do parallel restore, there are some restrictions.
	 */
	parallel_mode = (AH->public.numWorkers > 1 && ropt->useDB);
	if (parallel_mode)
	{
		/* We haven't got round to making this work for all archive formats */
		if (AH->ClonePtr == NULL || AH->ReopenPtr == NULL)
			exit_horribly(modulename, "parallel restore is not supported with this archive file format\n");

		/* Doesn't work if the archive represents dependencies as OIDs */
		if (AH->version < K_VERS_1_8)
			exit_horribly(modulename, "parallel restore is not supported with archives made by pre-8.0 pg_dump\n");

		/*
		 * It's also not gonna work if we can't reopen the input file, so
		 * let's try that immediately.
		 */
		(AH->ReopenPtr) (AH);
	}

	/*
	 * Make sure we won't need (de)compression we haven't got
	 */
#ifndef HAVE_LIBZ
	if (AH->compression != 0 && AH->PrintTocDataPtr !=NULL)
	{
		for (te = AH->toc->next; te != AH->toc; te = te->next)
		{
			if (te->hadDumper && (te->reqs & REQ_DATA) != 0)
				exit_horribly(modulename, "cannot restore from compressed archive (compression not supported in this installation)\n");
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
		ahlog(AH, 1, "connecting to database for restore\n");
		if (AH->version < K_VERS_1_3)
			exit_horribly(modulename, "direct database connections are not supported in pre-1.3 archives\n");

		/*
		 * We don't want to guess at whether the dump will successfully
		 * restore; allow the attempt regardless of the version of the restore
		 * target.
		 */
		AHX->minRemoteVersion = 0;
		AHX->maxRemoteVersion = 999999;

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
			ahlog(AH, 1, "implied data-only restore\n");
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
			 * Conversely, if we're not in createDB mode, we'd better not
			 * issue a DROP against the database at all.
			 */
			if (ropt->createDB)
			{
				if (strcmp(te->desc, "DATABASE") != 0)
					continue;
			}
			else
			{
				if (strcmp(te->desc, "DATABASE") == 0)
					continue;
			}

			/* Otherwise, drop anything that's selected and has a dropStmt */
			if (((te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0) && te->dropStmt)
			{
				ahlog(AH, 1, "dropping %s %s\n", te->desc, te->tag);
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
						 *
						 */
						if (strncmp(te->desc, "BLOB", 4) == 0)
						{
							DropBlobIfExists(AH, te->catalogId.oid);
						}
						else
						{
							char		buffer[40];
							char	   *mark;
							char	   *dropStmt = pg_strdup(te->dropStmt);
							char	   *dropStmtPtr = dropStmt;
							PQExpBuffer ftStmt = createPQExpBuffer();

							/*
							 * Need to inject IF EXISTS clause after ALTER
							 * TABLE part in ALTER TABLE .. DROP statement
							 */
							if (strncmp(dropStmt, "ALTER TABLE", 11) == 0)
							{
								appendPQExpBuffer(ftStmt,
												  "ALTER TABLE IF EXISTS");
								dropStmt = dropStmt + 11;
							}

							/*
							 * ALTER TABLE..ALTER COLUMN..DROP DEFAULT does
							 * not support the IF EXISTS clause, and therefore
							 * we simply emit the original command for such
							 * objects. For other objects, we need to extract
							 * the first part of the DROP which includes the
							 * object type. Most of the time this matches
							 * te->desc, so search for that; however for the
							 * different kinds of CONSTRAINTs, we know to
							 * search for hardcoded "DROP CONSTRAINT" instead.
							 */
							if (strcmp(te->desc, "DEFAULT") == 0)
								appendPQExpBufferStr(ftStmt, dropStmt);
							else
							{
								if (strcmp(te->desc, "CONSTRAINT") == 0 ||
								 strcmp(te->desc, "CHECK CONSTRAINT") == 0 ||
									strcmp(te->desc, "FK CONSTRAINT") == 0)
									strcpy(buffer, "DROP CONSTRAINT");
								else
									snprintf(buffer, sizeof(buffer), "DROP %s",
											 te->desc);

								mark = strstr(dropStmt, buffer);
								Assert(mark != NULL);

								*mark = '\0';
								appendPQExpBuffer(ftStmt, "%s%s IF EXISTS%s",
												  dropStmt, buffer,
												  mark + strlen(buffer));
							}

							ahprintf(AH, "%s", ftStmt->data);

							destroyPQExpBuffer(ftStmt);

							pg_free(dropStmtPtr);
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

	/*
	 * In serial mode, we now process each non-ACL TOC entry.
	 *
	 * In parallel mode, turn control over to the parallel-restore logic.
	 */
	if (parallel_mode)
	{
		ParallelState *pstate;
		TocEntry	pending_list;

		par_list_header_init(&pending_list);

		/* This runs PRE_DATA items and then disconnects from the database */
		restore_toc_entries_prefork(AH);
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
		for (te = AH->toc->next; te != AH->toc; te = te->next)
			(void) restore_toc_entry(AH, te, false);
	}

	/*
	 * Scan TOC again to output ownership commands and ACLs
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		AH->currentTE = te;

		/* Both schema and data objects might now have ownership/ACLs */
		if ((te->reqs & (REQ_SCHEMA | REQ_DATA)) != 0)
		{
			/* Show namespace if available */
			if (te->namespace)
				ahlog(AH, 1, "setting owner and privileges for %s \"%s.%s\"\n",
					  te->desc, te->namespace, te->tag);
			else
				ahlog(AH, 1, "setting owner and privileges for %s \"%s\"\n",
					  te->desc, te->tag);
			_printTocEntry(AH, te, false, true);
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

	/* Work out what, if anything, we want from this entry */
	if (_tocEntryIsACL(te))
		reqs = 0;				/* ACLs are never restored here */
	else
		reqs = te->reqs;

	/*
	 * Ignore DATABASE entry unless we should create it.  We must check this
	 * here, not in _tocEntryRequired, because the createDB option should not
	 * affect emitting a DATABASE entry to an archive file.
	 */
	if (!ropt->createDB && strcmp(te->desc, "DATABASE") == 0)
		reqs = 0;

	/* Dump any relevant dump warnings to stderr */
	if (!ropt->suppressDumpWarnings && strcmp(te->desc, "WARNING") == 0)
	{
		if (!ropt->dataOnly && te->defn != NULL && strlen(te->defn) != 0)
			write_msg(modulename, "warning from original dump file: %s\n", te->defn);
		else if (te->copyStmt != NULL && strlen(te->copyStmt) != 0)
			write_msg(modulename, "warning from original dump file: %s\n", te->copyStmt);
	}

	defnDumped = false;

	if ((reqs & REQ_SCHEMA) != 0)		/* We want the schema */
	{
		/* Show namespace if available */
		if (te->namespace)
			ahlog(AH, 1, "creating %s \"%s.%s\"\n",
				  te->desc, te->namespace, te->tag);
		else
			ahlog(AH, 1, "creating %s \"%s\"\n", te->desc, te->tag);


		_printTocEntry(AH, te, false, false);
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

		/* If we created a DB, connect to it... */
		if (strcmp(te->desc, "DATABASE") == 0)
		{
			ahlog(AH, 1, "connecting to new database \"%s\"\n", te->tag);
			_reconnectToDB(AH, te->tag);
			ropt->dbname = pg_strdup(te->tag);
		}
	}

	/*
	 * If we have a data component, then process it
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
			if (AH->PrintTocDataPtr !=NULL)
			{
				_printTocEntry(AH, te, true, false);

				if (strcmp(te->desc, "BLOBS") == 0 ||
					strcmp(te->desc, "BLOB COMMENTS") == 0)
				{
					ahlog(AH, 1, "processing %s\n", te->desc);

					_selectOutputSchema(AH, "pg_catalog");

					/* Send BLOB COMMENTS data to ExecuteSimpleCommands() */
					if (strcmp(te->desc, "BLOB COMMENTS") == 0)
						AH->outputKind = OUTPUT_OTHERDATA;

					(*AH->PrintTocDataPtr) (AH, te);

					AH->outputKind = OUTPUT_SQLCMDS;
				}
				else
				{
					_disableTriggersIfNecessary(AH, te);

					/* Select owner and schema as necessary */
					_becomeOwner(AH, te);
					_selectOutputSchema(AH, te->namespace);

					ahlog(AH, 1, "processing data for table \"%s.%s\"\n",
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
								 fmtId(te->tag));
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

					(*AH->PrintTocDataPtr) (AH, te);

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
			ahlog(AH, 1, "executing %s %s\n", te->desc, te->tag);
			_printTocEntry(AH, te, false, false);
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

	ahlog(AH, 1, "disabling triggers for %s\n", te->tag);

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
	_selectOutputSchema(AH, te->namespace);

	ahprintf(AH, "ALTER TABLE %s DISABLE TRIGGER ALL;\n\n",
			 fmtId(te->tag));
}

static void
_enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te)
{
	RestoreOptions *ropt = AH->public.ropt;

	/* This hack is only needed in a data-only restore */
	if (!ropt->dataOnly || !ropt->disable_triggers)
		return;

	ahlog(AH, 1, "enabling triggers for %s\n", te->tag);

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
	_selectOutputSchema(AH, te->namespace);

	ahprintf(AH, "ALTER TABLE %s ENABLE TRIGGER ALL;\n\n",
			 fmtId(te->tag));
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
		exit_horribly(modulename, "internal error -- WriteData cannot be called outside the context of a DataDumper routine\n");

	(*AH->WriteDataPtr) (AH, data, dLen);

	return;
}

/*
 * Create a new TOC entry. The TOC was designed as a TOC, but is now the
 * repository for all metadata. But the name has stuck.
 */

/* Public */
void
ArchiveEntry(Archive *AHX,
			 CatalogId catalogId, DumpId dumpId,
			 const char *tag,
			 const char *namespace,
			 const char *tablespace,
			 const char *owner, bool withOids,
			 const char *desc, teSection section,
			 const char *defn,
			 const char *dropStmt, const char *copyStmt,
			 const DumpId *deps, int nDeps,
			 DataDumperPtr dumpFn, void *dumpArg)
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
	newToc->section = section;

	newToc->tag = pg_strdup(tag);
	newToc->namespace = namespace ? pg_strdup(namespace) : NULL;
	newToc->tablespace = tablespace ? pg_strdup(tablespace) : NULL;
	newToc->owner = pg_strdup(owner);
	newToc->withOids = withOids;
	newToc->desc = pg_strdup(desc);
	newToc->defn = pg_strdup(defn);
	newToc->dropStmt = pg_strdup(dropStmt);
	newToc->copyStmt = copyStmt ? pg_strdup(copyStmt) : NULL;

	if (nDeps > 0)
	{
		newToc->dependencies = (DumpId *) pg_malloc(nDeps * sizeof(DumpId));
		memcpy(newToc->dependencies, deps, nDeps * sizeof(DumpId));
		newToc->nDeps = nDeps;
	}
	else
	{
		newToc->dependencies = NULL;
		newToc->nDeps = 0;
	}

	newToc->dataDumper = dumpFn;
	newToc->dataDumperArg = dumpArg;
	newToc->hadDumper = dumpFn ? true : false;

	newToc->formatData = NULL;

	if (AH->ArchiveEntryPtr !=NULL)
		(*AH->ArchiveEntryPtr) (AH, newToc);
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
			 AH->archdbname, AH->tocCount, AH->compression);

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

	ahprintf(AH, ";     Dump Version: %d.%d-%d\n", AH->vmaj, AH->vmin, AH->vrev);
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
			(_tocEntryRequired(te, curSection, ropt) & (REQ_SCHEMA | REQ_DATA)) != 0)
			ahprintf(AH, "%d; %u %u %s %s %s %s\n", te->dumpId,
					 te->catalogId.tableoid, te->catalogId.oid,
					 te->desc, te->namespace ? te->namespace : "-",
					 te->tag, te->owner);
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
		exit_horribly(modulename, "large-object output not supported in chosen format\n");

	(*AH->StartBlobPtr) (AH, AH->currToc, oid);

	return 1;
}

/* Called by a dumper to signal end of a BLOB */
int
EndBlob(Archive *AHX, Oid oid)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (AH->EndBlobPtr)
		(*AH->EndBlobPtr) (AH, AH->currToc, oid);

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

	ahlog(AH, 1, ngettext("restored %d large object\n",
						  "restored %d large objects\n",
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

	ahlog(AH, 1, "restoring large object with OID %u\n", oid);

	/* With an old archive we must do drop and create logic here */
	if (old_blob_style && drop)
		DropBlobIfExists(AH, oid);

	if (AH->connection)
	{
		if (old_blob_style)
		{
			loOid = lo_create(AH->connection, oid);
			if (loOid == 0 || loOid != oid)
				exit_horribly(modulename, "could not create large object %u: %s",
							  oid, PQerrorMessage(AH->connection));
		}
		AH->loFd = lo_open(AH->connection, oid, INV_WRITE);
		if (AH->loFd == -1)
			exit_horribly(modulename, "could not open large object %u: %s",
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
	ropt->idWanted = (bool *) pg_malloc(sizeof(bool) * AH->maxDumpId);
	memset(ropt->idWanted, 0, sizeof(bool) * AH->maxDumpId);

	/* Setup the file */
	fh = fopen(ropt->tocFile, PG_BINARY_R);
	if (!fh)
		exit_horribly(modulename, "could not open TOC file \"%s\": %s\n",
					  ropt->tocFile, strerror(errno));

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
			write_msg(modulename, "WARNING: line ignored: %s\n", buf);
			continue;
		}

		/* Find TOC entry */
		te = getTocEntryByDumpId(AH, id);
		if (!te)
			exit_horribly(modulename, "could not find entry for ID %d\n",
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
		exit_horribly(modulename, "could not close TOC file: %s\n",
					  strerror(errno));
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
	return;
}

/* Public */
int
archprintf(Archive *AH, const char *fmt,...)
{
	char	   *p;
	size_t		len = 128;		/* initial assumption about buffer size */
	size_t		cnt;

	for (;;)
	{
		va_list		args;

		/* Allocate work buffer. */
		p = (char *) pg_malloc(len);

		/* Try to format the data. */
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
		fn = -1;
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
		char		fmode[10];

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
			exit_horribly(modulename, "could not open output file \"%s\": %s\n",
						  filename, strerror(errno));
		else
			exit_horribly(modulename, "could not open output file: %s\n",
						  strerror(errno));
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
		exit_horribly(modulename, "could not close output file: %s\n",
					  strerror(errno));

	AH->gzOut = savedContext.gzOut;
	AH->OF = savedContext.OF;
}



/*
 *	Print formatted text to the output file (usually stdout).
 */
int
ahprintf(ArchiveHandle *AH, const char *fmt,...)
{
	char	   *p;
	size_t		len = 128;		/* initial assumption about buffer size */
	size_t		cnt;

	for (;;)
	{
		va_list		args;

		/* Allocate work buffer. */
		p = (char *) pg_malloc(len);

		/* Try to format the data. */
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

void
ahlog(ArchiveHandle *AH, int level, const char *fmt,...)
{
	va_list		ap;

	if (AH->debugLevel < level && (!AH->public.verbose || level > 1))
		return;

	va_start(ap, fmt);
	vwrite_msg(NULL, fmt, ap);
	va_end(ap);
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
		ahlog(AH, 5, ngettext("wrote %lu byte of large object data (result = %lu)\n",
					 "wrote %lu bytes of large object data (result = %lu)\n",
							  AH->lo_buf_used),
			  (unsigned long) AH->lo_buf_used, (unsigned long) res);
		if (res != AH->lo_buf_used)
			exit_horribly(modulename,
			"could not write to large object (result: %lu, expected: %lu)\n",
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
		bytes_written = AH->CustomOutPtr (AH, ptr, size * nmemb);

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

	return;
}

/* on some error, we may decide to go on... */
void
warn_or_exit_horribly(ArchiveHandle *AH,
					  const char *modulename, const char *fmt,...)
{
	va_list		ap;

	switch (AH->stage)
	{

		case STAGE_NONE:
			/* Do nothing special */
			break;

		case STAGE_INITIALIZING:
			if (AH->stage != AH->lastErrorStage)
				write_msg(modulename, "Error while INITIALIZING:\n");
			break;

		case STAGE_PROCESSING:
			if (AH->stage != AH->lastErrorStage)
				write_msg(modulename, "Error while PROCESSING TOC:\n");
			break;

		case STAGE_FINALIZING:
			if (AH->stage != AH->lastErrorStage)
				write_msg(modulename, "Error while FINALIZING:\n");
			break;
	}
	if (AH->currentTE != NULL && AH->currentTE != AH->lastErrorTE)
	{
		write_msg(modulename, "Error from TOC entry %d; %u %u %s %s %s\n",
				  AH->currentTE->dumpId,
			 AH->currentTE->catalogId.tableoid, AH->currentTE->catalogId.oid,
			  AH->currentTE->desc, AH->currentTE->tag, AH->currentTE->owner);
	}
	AH->lastErrorStage = AH->stage;
	AH->lastErrorTE = AH->currentTE;

	va_start(ap, fmt);
	vwrite_msg(modulename, fmt, ap);
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
			exit_horribly(modulename, "bad dumpId\n");

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
				exit_horribly(modulename, "bad table dumpId for TABLE DATA item\n");

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
	(*AH->WriteBytePtr) (AH, wasSet);

	/* Write out pgoff_t smallest byte first, prevents endian mismatch */
	for (off = 0; off < sizeof(pgoff_t); off++)
	{
		(*AH->WriteBytePtr) (AH, o & 0xFF);
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
	offsetFlg = (*AH->ReadBytePtr) (AH) & 0xFF;

	switch (offsetFlg)
	{
		case K_OFFSET_POS_NOT_SET:
		case K_OFFSET_NO_DATA:
		case K_OFFSET_POS_SET:

			break;

		default:
			exit_horribly(modulename, "unexpected data offset flag %d\n", offsetFlg);
	}

	/*
	 * Read the bytes
	 */
	for (off = 0; off < AH->offSize; off++)
	{
		if (off < sizeof(pgoff_t))
			*o |= ((pgoff_t) ((*AH->ReadBytePtr) (AH))) << (off * 8);
		else
		{
			if ((*AH->ReadBytePtr) (AH) != 0)
				exit_horribly(modulename, "file offset in dump file is too large\n");
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
	 * version #, and modify readInt to read the new format AS WELL AS the old
	 * formats.
	 */

	/* SIGN byte */
	if (i < 0)
	{
		(*AH->WriteBytePtr) (AH, 1);
		i = -i;
	}
	else
		(*AH->WriteBytePtr) (AH, 0);

	for (b = 0; b < AH->intSize; b++)
	{
		(*AH->WriteBytePtr) (AH, i & 0xFF);
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
		sign = (*AH->ReadBytePtr) (AH);

	for (b = 0; b < AH->intSize; b++)
	{
		bv = (*AH->ReadBytePtr) (AH) & 0xFF;
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
		(*AH->WriteBufPtr) (AH, c, len);
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
		(*AH->ReadBufPtr) (AH, (void *) buf, l);

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

#if 0
	write_msg(modulename, "attempting to ascertain archive format\n");
#endif

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
				exit_horribly(modulename, "directory name too long: \"%s\"\n",
							  AH->fSpec);
			if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
			{
				AH->format = archDirectory;
				return AH->format;
			}

#ifdef HAVE_LIBZ
			if (snprintf(buf, MAXPGPATH, "%s/toc.dat.gz", AH->fSpec) >= MAXPGPATH)
				exit_horribly(modulename, "directory name too long: \"%s\"\n",
							  AH->fSpec);
			if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
			{
				AH->format = archDirectory;
				return AH->format;
			}
#endif
			exit_horribly(modulename, "directory \"%s\" does not appear to be a valid archive (\"toc.dat\" does not exist)\n",
						  AH->fSpec);
			fh = NULL;			/* keep compiler quiet */
		}
		else
		{
			fh = fopen(AH->fSpec, PG_BINARY_R);
			if (!fh)
				exit_horribly(modulename, "could not open input file \"%s\": %s\n",
							  AH->fSpec, strerror(errno));
		}
	}
	else
	{
		fh = stdin;
		if (!fh)
			exit_horribly(modulename, "could not open input file: %s\n",
						  strerror(errno));
	}

	if ((cnt = fread(sig, 1, 5, fh)) != 5)
	{
		if (ferror(fh))
			exit_horribly(modulename, "could not read input file: %s\n", strerror(errno));
		else
			exit_horribly(modulename, "input file is too short (read %lu, expected 5)\n",
						  (unsigned long) cnt);
	}

	/* Save it, just in case we need it later */
	memcpy(&AH->lookahead[0], sig, 5);
	AH->lookaheadLen = 5;

	if (strncmp(sig, "PGDMP", 5) == 0)
	{
		int			byteread;

		/*
		 * Finish reading (most of) a custom-format header.
		 *
		 * NB: this code must agree with ReadHead().
		 */
		if ((byteread = fgetc(fh)) == EOF)
			READ_ERROR_EXIT(fh);

		AH->vmaj = byteread;

		if ((byteread = fgetc(fh)) == EOF)
			READ_ERROR_EXIT(fh);

		AH->vmin = byteread;

		/* Save these too... */
		AH->lookahead[AH->lookaheadLen++] = AH->vmaj;
		AH->lookahead[AH->lookaheadLen++] = AH->vmin;

		/* Check header version; varies from V1.0 */
		if (AH->vmaj > 1 || ((AH->vmaj == 1) && (AH->vmin > 0)))		/* Version > 1.0 */
		{
			if ((byteread = fgetc(fh)) == EOF)
				READ_ERROR_EXIT(fh);

			AH->vrev = byteread;
			AH->lookahead[AH->lookaheadLen++] = AH->vrev;
		}
		else
			AH->vrev = 0;

		/* Make a convenient integer <maj><min><rev>00 */
		AH->version = ((AH->vmaj * 256 + AH->vmin) * 256 + AH->vrev) * 256 + 0;

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
			exit_horribly(modulename, "input file appears to be a text format dump. Please use psql.\n");
		}

		if (AH->lookaheadLen != 512)
		{
			if (feof(fh))
				exit_horribly(modulename, "input file does not appear to be a valid archive (too short?)\n");
			else
				READ_ERROR_EXIT(fh);
		}

		if (!isValidTarHeader(AH->lookahead))
			exit_horribly(modulename, "input file does not appear to be a valid archive\n");

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
			exit_horribly(modulename, "could not close input file: %s\n",
						  strerror(errno));

	return AH->format;
}


/*
 * Allocate an archive handle
 */
static ArchiveHandle *
_allocAH(const char *FileSpec, const ArchiveFormat fmt,
	  const int compression, ArchiveMode mode, SetupWorkerPtr setupWorkerPtr)
{
	ArchiveHandle *AH;

#if 0
	write_msg(modulename, "allocating AH for %s, format %d\n", FileSpec, fmt);
#endif

	AH = (ArchiveHandle *) pg_malloc0(sizeof(ArchiveHandle));

	/* AH->debugLevel = 100; */

	AH->vmaj = K_VERS_MAJOR;
	AH->vmin = K_VERS_MINOR;
	AH->vrev = K_VERS_REV;

	/* Make a convenient integer <maj><min><rev>00 */
	AH->version = ((AH->vmaj * 256 + AH->vmin) * 256 + AH->vrev) * 256 + 0;

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
	AH->currWithOids = -1;		/* force SET */

	AH->toc = (TocEntry *) pg_malloc0(sizeof(TocEntry));

	AH->toc->next = AH->toc;
	AH->toc->prev = AH->toc;

	AH->mode = mode;
	AH->compression = compression;

	memset(&(AH->sqlparse), 0, sizeof(AH->sqlparse));

	/* Open stdout with no compression for AH output handle */
	AH->gzOut = 0;
	AH->OF = stdout;

	/*
	 * On Windows, we need to use binary mode to read/write non-text archive
	 * formats.  Force stdin/stdout into binary mode if that is what we are
	 * using.
	 */
#ifdef WIN32
	if (fmt != archNull &&
		(AH->fSpec == NULL || strcmp(AH->fSpec, "") == 0))
	{
		if (mode == archModeWrite)
			setmode(fileno(stdout), O_BINARY);
		else
			setmode(fileno(stdin), O_BINARY);
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
			exit_horribly(modulename, "unrecognized file format \"%d\"\n", fmt);
	}

	return AH;
}

void
WriteDataChunks(ArchiveHandle *AH, ParallelState *pstate)
{
	TocEntry   *te;

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (!te->dataDumper)
			continue;

		if ((te->reqs & REQ_DATA) == 0)
			continue;

		if (pstate && pstate->numWorkers > 1)
		{
			/*
			 * If we are in a parallel backup, then we are always the master
			 * process.
			 */
			EnsureIdleWorker(AH, pstate);
			Assert(GetIdleWorker(pstate) != NO_SLOT);
			DispatchJobForTocEntry(AH, pstate, te, ACT_DUMP);
		}
		else
			WriteDataChunksForTocEntry(AH, te);
	}
	EnsureWorkersFinished(AH, pstate);
}

void
WriteDataChunksForTocEntry(ArchiveHandle *AH, TocEntry *te)
{
	StartDataPtr startPtr;
	EndDataPtr	endPtr;

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
	(*te->dataDumper) ((Archive *) AH, te->dataDumperArg);

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
		WriteStr(AH, te->owner);
		WriteStr(AH, te->withOids ? "true" : "false");

		/* Dump list of dependencies */
		for (i = 0; i < te->nDeps; i++)
		{
			sprintf(workbuf, "%d", te->dependencies[i]);
			WriteStr(AH, workbuf);
		}
		WriteStr(AH, NULL);		/* Terminate List */

		if (AH->WriteExtraTocPtr)
			(*AH->WriteExtraTocPtr) (AH, te);
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
			exit_horribly(modulename,
					   "entry ID %d out of range -- perhaps a corrupt TOC\n",
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

		te->owner = ReadStr(AH);
		if (AH->version >= K_VERS_1_9)
		{
			if (strcmp(ReadStr(AH), "true") == 0)
				te->withOids = true;
			else
				te->withOids = false;
		}
		else
			te->withOids = true;

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

		if (AH->ReadExtraTocPtr)
			(*AH->ReadExtraTocPtr) (AH, te);

		ahlog(AH, 3, "read TOC entry %d (ID %d) for %s %s\n",
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
			exit_horribly(modulename, "unrecognized encoding \"%s\"\n",
						  ptr1);
		AH->public.encoding = encoding;
	}
	else
		exit_horribly(modulename, "invalid ENCODING item: %s\n",
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
		exit_horribly(modulename, "invalid STDSTRINGS item: %s\n",
					  te->defn);
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
			exit_horribly(modulename, "Schema \"%s\" not found.\n", missing_name);
	}

	if (ropt->tableNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->tableNames);
		if (missing_name != NULL)
			exit_horribly(modulename, "Table \"%s\" not found.\n", missing_name);
	}

	if (ropt->indexNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->indexNames);
		if (missing_name != NULL)
			exit_horribly(modulename, "Index \"%s\" not found.\n", missing_name);
	}

	if (ropt->functionNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->functionNames);
		if (missing_name != NULL)
			exit_horribly(modulename, "Function \"%s\" not found.\n", missing_name);
	}

	if (ropt->triggerNames.head != NULL)
	{
		missing_name = simple_string_list_not_touched(&ropt->triggerNames);
		if (missing_name != NULL)
			exit_horribly(modulename, "Trigger \"%s\" not found.\n", missing_name);
	}
}

static teReqs
_tocEntryRequired(TocEntry *te, teSection curSection, RestoreOptions *ropt)
{
	teReqs		res = REQ_SCHEMA | REQ_DATA;

	/* ENCODING and STDSTRINGS items are treated specially */
	if (strcmp(te->desc, "ENCODING") == 0 ||
		strcmp(te->desc, "STDSTRINGS") == 0)
		return REQ_SPECIAL;

	/* If it's an ACL, maybe ignore it */
	if (ropt->aclsSkip && _tocEntryIsACL(te))
		return 0;

	/* If it's security labels, maybe ignore it */
	if (ropt->no_security_labels && strcmp(te->desc, "SECURITY LABEL") == 0)
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

	/* Check options for selective dump/restore */
	if (ropt->schemaNames.head != NULL)
	{
		/* If no namespace is specified, it means all. */
		if (!te->namespace)
			return 0;
		if (!(simple_string_list_member(&ropt->schemaNames, te->namespace)))
			return 0;
	}

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
			if (ropt->tableNames.head != NULL && (!(simple_string_list_member(&ropt->tableNames, te->tag))))
				return 0;
		}
		else if (strcmp(te->desc, "INDEX") == 0)
		{
			if (!ropt->selIndex)
				return 0;
			if (ropt->indexNames.head != NULL && (!(simple_string_list_member(&ropt->indexNames, te->tag))))
				return 0;
		}
		else if (strcmp(te->desc, "FUNCTION") == 0)
		{
			if (!ropt->selFunction)
				return 0;
			if (ropt->functionNames.head != NULL && (!(simple_string_list_member(&ropt->functionNames, te->tag))))
				return 0;
		}
		else if (strcmp(te->desc, "TRIGGER") == 0)
		{
			if (!ropt->selTrigger)
				return 0;
			if (ropt->triggerNames.head != NULL && (!(simple_string_list_member(&ropt->triggerNames, te->tag))))
				return 0;
		}
		else
			return 0;
	}

	/*
	 * Check if we had a dataDumper. Indicates if the entry is schema or data
	 */
	if (!te->hadDumper)
	{
		/*
		 * Special Case: If 'SEQUENCE SET' or anything to do with BLOBs, then
		 * it is considered a data entry.  We don't need to check for the
		 * BLOBS entry or old-style BLOB COMMENTS, because they will have
		 * hadDumper = true ... but we do need to check new-style BLOB
		 * comments.
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

	/*
	 * Special case: <Init> type with <Max OID> tag; this is obsolete and we
	 * always ignore it.
	 */
	if ((strcmp(te->desc, "<Init>") == 0) && (strcmp(te->tag, "Max OID") == 0))
		return 0;

	/* Mask it if we only want schema */
	if (ropt->schemaOnly)
		res = res & REQ_SCHEMA;

	/* Mask it if we only want data */
	if (ropt->dataOnly)
		res = res & REQ_DATA;

	/* Mask it if we don't have a schema contribution */
	if (!te->defn || strlen(te->defn) == 0)
		res = res & ~REQ_SCHEMA;

	/* Finally, if there's a per-ID filter, limit based on that as well */
	if (ropt->idWanted && !ropt->idWanted[te->dumpId - 1])
		return 0;

	return res;
}

/*
 * Identify TOC entries that are ACLs.
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

	/* Disable statement_timeout since restore is probably slow */
	ahprintf(AH, "SET statement_timeout = 0;\n");

	/* Likewise for lock_timeout */
	ahprintf(AH, "SET lock_timeout = 0;\n");

	/* Select the correct character set encoding */
	ahprintf(AH, "SET client_encoding = '%s';\n",
			 pg_encoding_to_char(AH->public.encoding));

	/* Select the correct string literal syntax */
	ahprintf(AH, "SET standard_conforming_strings = %s;\n",
			 AH->public.std_strings ? "on" : "off");

	/* Select the role to be used during restore */
	if (ropt && ropt->use_role)
		ahprintf(AH, "SET ROLE %s;\n", fmtId(ropt->use_role));

	/* Make sure function checking is disabled */
	ahprintf(AH, "SET check_function_bodies = false;\n");

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
			exit_horribly(modulename, "could not set session user to \"%s\": %s",
						  user, PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s\n\n", cmd->data);

	destroyPQExpBuffer(cmd);
}


/*
 * Issue a SET default_with_oids command.  Caller is responsible
 * for updating state if appropriate.
 */
static void
_doSetWithOids(ArchiveHandle *AH, const bool withOids)
{
	PQExpBuffer cmd = createPQExpBuffer();

	appendPQExpBuffer(cmd, "SET default_with_oids = %s;", withOids ?
					  "true" : "false");

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, cmd->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_exit_horribly(AH, modulename,
								  "could not set default_with_oids: %s",
								  PQerrorMessage(AH->connection));

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
		PQExpBuffer qry = createPQExpBuffer();

		appendPQExpBuffer(qry, "\\connect %s\n\n",
						  dbname ? fmtId(dbname) : "-");
		ahprintf(AH, "%s", qry->data);
		destroyPQExpBuffer(qry);
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
	AH->currWithOids = -1;

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
 * Set the proper default_with_oids value for the table.
 */
static void
_setWithOids(ArchiveHandle *AH, TocEntry *te)
{
	if (AH->currWithOids != te->withOids)
	{
		_doSetWithOids(AH, te->withOids);
		AH->currWithOids = te->withOids;
	}
}


/*
 * Issue the commands to select the specified schema as the current schema
 * in the target database.
 */
static void
_selectOutputSchema(ArchiveHandle *AH, const char *schemaName)
{
	PQExpBuffer qry;

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
			warn_or_exit_horribly(AH, modulename,
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
			warn_or_exit_horribly(AH, modulename,
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
	/* non-schema-specified objects */
		strcmp(type, "DATABASE") == 0 ||
		strcmp(type, "PROCEDURAL LANGUAGE") == 0 ||
		strcmp(type, "SCHEMA") == 0 ||
		strcmp(type, "FOREIGN DATA WRAPPER") == 0 ||
		strcmp(type, "SERVER") == 0 ||
		strcmp(type, "USER MAPPING") == 0)
	{
		/* We already know that search_path was set properly */
		appendPQExpBuffer(buf, "%s %s", type, fmtId(te->tag));
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
		strcmp(type, "OPERATOR FAMILY") == 0)
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

	write_msg(modulename, "WARNING: don't know how to set owner for object type %s\n",
			  type);
}

static void
_printTocEntry(ArchiveHandle *AH, TocEntry *te, bool isData, bool acl_pass)
{
	RestoreOptions *ropt = AH->public.ropt;

	/* ACLs are dumped only during acl pass */
	if (acl_pass)
	{
		if (!_tocEntryIsACL(te))
			return;
	}
	else
	{
		if (_tocEntryIsACL(te))
			return;
	}

	/*
	 * Avoid dumping the public schema, as it will already be created ...
	 * unless we are using --clean mode, in which case it's been deleted and
	 * we'd better recreate it.  Likewise for its comment, if any.
	 */
	if (!ropt->dropSchema)
	{
		if (strcmp(te->desc, "SCHEMA") == 0 &&
			strcmp(te->tag, "public") == 0)
			return;
		/* The comment restore would require super-user privs, so avoid it. */
		if (strcmp(te->desc, "COMMENT") == 0 &&
			strcmp(te->tag, "SCHEMA public") == 0)
			return;
	}

	/* Select owner, schema, and tablespace as necessary */
	_becomeOwner(AH, te);
	_selectOutputSchema(AH, te->namespace);
	_selectTablespace(AH, te->tablespace);

	/* Set up OID mode too */
	if (strcmp(te->desc, "TABLE") == 0)
		_setWithOids(AH, te);

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

		/*
		 * Zap any line endings embedded in user-supplied fields, to prevent
		 * corruption of the dump (which could, in the worst case, present an
		 * SQL injection vulnerability if someone were to incautiously load a
		 * dump containing objects with maliciously crafted names).
		 */
		sanitized_name = replace_line_endings(te->tag);
		if (te->namespace)
			sanitized_schema = replace_line_endings(te->namespace);
		else
			sanitized_schema = pg_strdup("-");
		if (!ropt->noOwner)
			sanitized_owner = replace_line_endings(te->owner);
		else
			sanitized_owner = pg_strdup("-");

		ahprintf(AH, "-- %sName: %s; Type: %s; Schema: %s; Owner: %s",
				 pfx, sanitized_name, te->desc, sanitized_schema,
				 sanitized_owner);

		free(sanitized_name);
		free(sanitized_schema);
		free(sanitized_owner);

		if (te->tablespace && strlen(te->tablespace) > 0 && !ropt->noTablespace)
		{
			char	   *sanitized_tablespace;

			sanitized_tablespace = replace_line_endings(te->tablespace);
			ahprintf(AH, "; Tablespace: %s", sanitized_tablespace);
			free(sanitized_tablespace);
		}
		ahprintf(AH, "\n");

		if (AH->PrintExtraTocPtr !=NULL)
			(*AH->PrintExtraTocPtr) (AH, te);
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
		if (strlen(te->defn) > 0)
			ahprintf(AH, "%s\n\n", te->defn);
	}

	/*
	 * If we aren't using SET SESSION AUTH to determine ownership, we must
	 * instead issue an ALTER OWNER command.  We assume that anything without
	 * a DROP command is not a separately ownable object.  All the categories
	 * with DROP commands must appear in one list or the other.
	 */
	if (!ropt->noOwner && !ropt->use_setsessauth &&
		strlen(te->owner) > 0 && strlen(te->dropStmt) > 0)
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
			strcmp(te->desc, "PROCEDURAL LANGUAGE") == 0 ||
			strcmp(te->desc, "SCHEMA") == 0 ||
			strcmp(te->desc, "TABLE") == 0 ||
			strcmp(te->desc, "TYPE") == 0 ||
			strcmp(te->desc, "VIEW") == 0 ||
			strcmp(te->desc, "MATERIALIZED VIEW") == 0 ||
			strcmp(te->desc, "SEQUENCE") == 0 ||
			strcmp(te->desc, "FOREIGN TABLE") == 0 ||
			strcmp(te->desc, "TEXT SEARCH DICTIONARY") == 0 ||
			strcmp(te->desc, "TEXT SEARCH CONFIGURATION") == 0 ||
			strcmp(te->desc, "FOREIGN DATA WRAPPER") == 0 ||
			strcmp(te->desc, "SERVER") == 0)
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
			write_msg(modulename, "WARNING: don't know how to set owner for object type %s\n",
					  te->desc);
		}
	}

	/*
	 * If it's an ACL entry, it might contain SET SESSION AUTHORIZATION
	 * commands, so we can no longer assume we know the current auth setting.
	 */
	if (acl_pass)
	{
		if (AH->currUser)
			free(AH->currUser);
		AH->currUser = NULL;
	}
}

/*
 * Sanitize a string to be included in an SQL comment, by replacing any
 * newlines with spaces.
 */
static char *
replace_line_endings(const char *str)
{
	char	   *result;
	char	   *s;

	result = pg_strdup(str);

	for (s = result; *s != '\0'; s++)
	{
		if (*s == '\n' || *s == '\r')
			*s = ' ';
	}

	return result;
}

void
WriteHead(ArchiveHandle *AH)
{
	struct tm	crtm;

	(*AH->WriteBufPtr) (AH, "PGDMP", 5);		/* Magic code */
	(*AH->WriteBytePtr) (AH, AH->vmaj);
	(*AH->WriteBytePtr) (AH, AH->vmin);
	(*AH->WriteBytePtr) (AH, AH->vrev);
	(*AH->WriteBytePtr) (AH, AH->intSize);
	(*AH->WriteBytePtr) (AH, AH->offSize);
	(*AH->WriteBytePtr) (AH, AH->format);

#ifndef HAVE_LIBZ
	if (AH->compression != 0)
		write_msg(modulename, "WARNING: requested compression not available in this "
				  "installation -- archive will be uncompressed\n");

	AH->compression = 0;
#endif

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
		(*AH->ReadBufPtr) (AH, tmpMag, 5);

		if (strncmp(tmpMag, "PGDMP", 5) != 0)
			exit_horribly(modulename, "did not find magic string in file header\n");

		AH->vmaj = (*AH->ReadBytePtr) (AH);
		AH->vmin = (*AH->ReadBytePtr) (AH);

		if (AH->vmaj > 1 || ((AH->vmaj == 1) && (AH->vmin > 0)))		/* Version > 1.0 */
			AH->vrev = (*AH->ReadBytePtr) (AH);
		else
			AH->vrev = 0;

		AH->version = ((AH->vmaj * 256 + AH->vmin) * 256 + AH->vrev) * 256 + 0;

		if (AH->version < K_VERS_1_0 || AH->version > K_VERS_MAX)
			exit_horribly(modulename, "unsupported version (%d.%d) in file header\n",
						  AH->vmaj, AH->vmin);

		AH->intSize = (*AH->ReadBytePtr) (AH);
		if (AH->intSize > 32)
			exit_horribly(modulename, "sanity check on integer size (%lu) failed\n",
						  (unsigned long) AH->intSize);

		if (AH->intSize > sizeof(int))
			write_msg(modulename, "WARNING: archive was made on a machine with larger integers, some operations might fail\n");

		if (AH->version >= K_VERS_1_7)
			AH->offSize = (*AH->ReadBytePtr) (AH);
		else
			AH->offSize = AH->intSize;

		fmt = (*AH->ReadBytePtr) (AH);

		if (AH->format != fmt)
			exit_horribly(modulename, "expected format (%d) differs from format found in file (%d)\n",
						  AH->format, fmt);
	}

	if (AH->version >= K_VERS_1_2)
	{
		if (AH->version < K_VERS_1_4)
			AH->compression = (*AH->ReadBytePtr) (AH);
		else
			AH->compression = ReadInt(AH);
	}
	else
		AH->compression = Z_DEFAULT_COMPRESSION;

#ifndef HAVE_LIBZ
	if (AH->compression != 0)
		write_msg(modulename, "WARNING: archive is compressed, but this installation does not support compression -- no data will be available\n");
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
			write_msg(modulename, "WARNING: invalid creation date in header\n");
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

	/*
	 * If pgoff_t is wider than long, we must have "real" fseeko and not an
	 * emulation using fseek.  Otherwise report no seek capability.
	 */
#ifndef HAVE_FSEEKO
	if (sizeof(pgoff_t) > sizeof(long))
		return false;
#endif

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
 * Work is done in three phases.
 * First we process all SECTION_PRE_DATA tocEntries, in a single connection,
 * just as for a standard restore.  Second we process the remaining non-ACL
 * steps in parallel worker children (threads on Windows, processes on Unix),
 * each of which connects separately to the database.  Finally we process all
 * the ACL entries in a single connection (that happens back in
 * RestoreArchive).
 */
static void
restore_toc_entries_prefork(ArchiveHandle *AH)
{
	bool		skipped_some;
	TocEntry   *next_work_item;

	ahlog(AH, 2, "entering restore_toc_entries_prefork\n");

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
	 * Note: as of 9.2, it should be guaranteed that all PRE_DATA items appear
	 * before DATA items, and all DATA items before POST_DATA items.  That is
	 * not certain to be true in older archives, though, so this loop is coded
	 * to not assume it.
	 */
	skipped_some = false;
	for (next_work_item = AH->toc->next; next_work_item != AH->toc; next_work_item = next_work_item->next)
	{
		/* NB: process-or-continue logic must be the inverse of loop below */
		if (next_work_item->section != SECTION_PRE_DATA)
		{
			/* DATA and POST_DATA items are just ignored for now */
			if (next_work_item->section == SECTION_DATA ||
				next_work_item->section == SECTION_POST_DATA)
			{
				skipped_some = true;
				continue;
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
					continue;
			}
		}

		ahlog(AH, 1, "processing item %d %s %s\n",
			  next_work_item->dumpId,
			  next_work_item->desc, next_work_item->tag);

		(void) restore_toc_entry(AH, next_work_item, false);

		/* there should be no touch of ready_list here, so pass NULL */
		reduce_dependencies(AH, next_work_item, NULL);
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
	AH->currWithOids = -1;
}

/*
 * Main engine for parallel restore.
 *
 * Work is done in three phases.
 * First we process all SECTION_PRE_DATA tocEntries, in a single connection,
 * just as for a standard restore. This is done in restore_toc_entries_prefork().
 * Second we process the remaining non-ACL steps in parallel worker children
 * (threads on Windows, processes on Unix), these fork off and set up their
 * connections before we call restore_toc_entries_parallel_forked.
 * Finally we process all the ACL entries in a single connection (that happens
 * back in RestoreArchive).
 */
static void
restore_toc_entries_parallel(ArchiveHandle *AH, ParallelState *pstate,
							 TocEntry *pending_list)
{
	int			work_status;
	bool		skipped_some;
	TocEntry	ready_list;
	TocEntry   *next_work_item;
	int			ret_child;

	ahlog(AH, 2, "entering restore_toc_entries_parallel\n");

	/*
	 * Initialize the lists of ready items, the list for pending items has
	 * already been initialized in the caller.  After this setup, the pending
	 * list is everything that needs to be done but is blocked by one or more
	 * dependencies, while the ready list contains items that have no
	 * remaining dependencies. Note: we don't yet filter out entries that
	 * aren't going to be restored. They might participate in dependency
	 * chains connecting entries that should be restored, so we treat them as
	 * live until we actually process them.
	 */
	par_list_header_init(&ready_list);
	skipped_some = false;
	for (next_work_item = AH->toc->next; next_work_item != AH->toc; next_work_item = next_work_item->next)
	{
		/* NB: process-or-continue logic must be the inverse of loop above */
		if (next_work_item->section == SECTION_PRE_DATA)
		{
			/* All PRE_DATA items were dealt with above */
			continue;
		}
		if (next_work_item->section == SECTION_DATA ||
			next_work_item->section == SECTION_POST_DATA)
		{
			/* set this flag at same point that previous loop did */
			skipped_some = true;
		}
		else
		{
			/* SECTION_NONE items must be processed if previous loop didn't */
			if (!skipped_some)
				continue;
		}

		if (next_work_item->depCount > 0)
			par_list_append(pending_list, next_work_item);
		else
			par_list_append(&ready_list, next_work_item);
	}

	/*
	 * main parent loop
	 *
	 * Keep going until there is no worker still running AND there is no work
	 * left to be done.
	 */

	ahlog(AH, 1, "entering main parallel loop\n");

	while ((next_work_item = get_next_work_item(AH, &ready_list, pstate)) != NULL ||
		   !IsEveryWorkerIdle(pstate))
	{
		if (next_work_item != NULL)
		{
			/* If not to be restored, don't waste time launching a worker */
			if ((next_work_item->reqs & (REQ_SCHEMA | REQ_DATA)) == 0 ||
				_tocEntryIsACL(next_work_item))
			{
				ahlog(AH, 1, "skipping item %d %s %s\n",
					  next_work_item->dumpId,
					  next_work_item->desc, next_work_item->tag);

				par_list_remove(next_work_item);
				reduce_dependencies(AH, next_work_item, &ready_list);

				continue;
			}

			ahlog(AH, 1, "launching item %d %s %s\n",
				  next_work_item->dumpId,
				  next_work_item->desc, next_work_item->tag);

			par_list_remove(next_work_item);

			Assert(GetIdleWorker(pstate) != NO_SLOT);
			DispatchJobForTocEntry(AH, pstate, next_work_item, ACT_RESTORE);
		}
		else
		{
			/* at least one child is working and we have nothing ready. */
			Assert(!IsEveryWorkerIdle(pstate));
		}

		for (;;)
		{
			int			nTerm = 0;

			/*
			 * In order to reduce dependencies as soon as possible and
			 * especially to reap the status of workers who are working on
			 * items that pending items depend on, we do a non-blocking check
			 * for ended workers first.
			 *
			 * However, if we do not have any other work items currently that
			 * workers can work on, we do not busy-loop here but instead
			 * really wait for at least one worker to terminate. Hence we call
			 * ListenToWorkers(..., ..., do_wait = true) in this case.
			 */
			ListenToWorkers(AH, pstate, !next_work_item);

			while ((ret_child = ReapWorkerStatus(pstate, &work_status)) != NO_SLOT)
			{
				nTerm++;
				mark_work_done(AH, &ready_list, ret_child, work_status, pstate);
			}

			/*
			 * We need to make sure that we have an idle worker before
			 * re-running the loop. If nTerm > 0 we already have that (quick
			 * check).
			 */
			if (nTerm > 0)
				break;

			/* if nobody terminated, explicitly check for an idle worker */
			if (GetIdleWorker(pstate) != NO_SLOT)
				break;

			/*
			 * If we have no idle worker, read the result of one or more
			 * workers and loop the loop to call ReapWorkerStatus() on them.
			 */
			ListenToWorkers(AH, pstate, true);
		}
	}

	ahlog(AH, 1, "finished main parallel loop\n");
}

static void
restore_toc_entries_postfork(ArchiveHandle *AH, TocEntry *pending_list)
{
	RestoreOptions *ropt = AH->public.ropt;
	TocEntry   *te;

	ahlog(AH, 2, "entering restore_toc_entries_postfork\n");

	/*
	 * Now reconnect the single parent connection.
	 */
	ConnectDatabase((Archive *) AH, ropt->dbname,
					ropt->pghost, ropt->pgport, ropt->username,
					ropt->promptPassword);

	_doSetFixedOutputState(AH);

	/*
	 * Make sure there is no non-ACL work left due to, say, circular
	 * dependencies, or some other pathological condition. If so, do it in the
	 * single parent connection.
	 */
	for (te = pending_list->par_next; te != pending_list; te = te->par_next)
	{
		ahlog(AH, 1, "processing missed item %d %s %s\n",
			  te->dumpId, te->desc, te->tag);
		(void) restore_toc_entry(AH, te, false);
	}

	/* The ACLs will be handled back in RestoreArchive. */
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
 * Initialize the header of a parallel-processing list.
 *
 * These are circular lists with a dummy TocEntry as header, just like the
 * main TOC list; but we use separate list links so that an entry can be in
 * the main TOC list as well as in a parallel-processing list.
 */
static void
par_list_header_init(TocEntry *l)
{
	l->par_prev = l->par_next = l;
}

/* Append te to the end of the parallel-processing list headed by l */
static void
par_list_append(TocEntry *l, TocEntry *te)
{
	te->par_prev = l->par_prev;
	l->par_prev->par_next = te;
	l->par_prev = te;
	te->par_next = l;
}

/* Remove te from whatever parallel-processing list it's in */
static void
par_list_remove(TocEntry *te)
{
	te->par_prev->par_next = te->par_next;
	te->par_next->par_prev = te->par_prev;
	te->par_prev = NULL;
	te->par_next = NULL;
}


/*
 * Find the next work item (if any) that is capable of being run now.
 *
 * To qualify, the item must have no remaining dependencies
 * and no requirements for locks that are incompatible with
 * items currently running.  Items in the ready_list are known to have
 * no remaining dependencies, but we have to check for lock conflicts.
 *
 * Note that the returned item has *not* been removed from ready_list.
 * The caller must do that after successfully dispatching the item.
 *
 * pref_non_data is for an alternative selection algorithm that gives
 * preference to non-data items if there is already a data load running.
 * It is currently disabled.
 */
static TocEntry *
get_next_work_item(ArchiveHandle *AH, TocEntry *ready_list,
				   ParallelState *pstate)
{
	bool		pref_non_data = false;	/* or get from AH->ropt */
	TocEntry   *data_te = NULL;
	TocEntry   *te;
	int			i,
				k;

	/*
	 * Bogus heuristics for pref_non_data
	 */
	if (pref_non_data)
	{
		int			count = 0;

		for (k = 0; k < pstate->numWorkers; k++)
			if (pstate->parallelSlot[k].args->te != NULL &&
				pstate->parallelSlot[k].args->te->section == SECTION_DATA)
				count++;
		if (pstate->numWorkers == 0 || count * 4 < pstate->numWorkers)
			pref_non_data = false;
	}

	/*
	 * Search the ready_list until we find a suitable item.
	 */
	for (te = ready_list->par_next; te != ready_list; te = te->par_next)
	{
		bool		conflicts = false;

		/*
		 * Check to see if the item would need exclusive lock on something
		 * that a currently running item also needs lock on, or vice versa. If
		 * so, we don't want to schedule them together.
		 */
		for (i = 0; i < pstate->numWorkers && !conflicts; i++)
		{
			TocEntry   *running_te;

			if (pstate->parallelSlot[i].workerStatus != WRKR_WORKING)
				continue;
			running_te = pstate->parallelSlot[i].args->te;

			if (has_lock_conflicts(te, running_te) ||
				has_lock_conflicts(running_te, te))
			{
				conflicts = true;
				break;
			}
		}

		if (conflicts)
			continue;

		if (pref_non_data && te->section == SECTION_DATA)
		{
			if (data_te == NULL)
				data_te = te;
			continue;
		}

		/* passed all tests, so this item can run */
		return te;
	}

	if (data_te != NULL)
		return data_te;

	ahlog(AH, 2, "no item ready\n");
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
parallel_restore(ParallelArgs *args)
{
	ArchiveHandle *AH = args->AH;
	TocEntry   *te = args->te;
	int			status;

	_doSetFixedOutputState(AH);

	Assert(AH->connection != NULL);

	AH->public.n_errors = 0;

	/* Restore the TOC item */
	status = restore_toc_entry(AH, te, true);

	return status;
}


/*
 * Housekeeping to be done after a step has been parallel restored.
 *
 * Clear the appropriate slot, free all the extra memory we allocated,
 * update status, and reduce the dependency count of any dependent items.
 */
static void
mark_work_done(ArchiveHandle *AH, TocEntry *ready_list,
			   int worker, int status,
			   ParallelState *pstate)
{
	TocEntry   *te = NULL;

	te = pstate->parallelSlot[worker].args->te;

	if (te == NULL)
		exit_horribly(modulename, "could not find slot of finished worker\n");

	ahlog(AH, 1, "finished item %d %s %s\n",
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
		exit_horribly(modulename, "worker process failed: exit code %d\n",
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
		te->par_prev = NULL;
		te->par_next = NULL;
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
				te->dependencies[i] = AH->tableDataId[olddep];
				ahlog(AH, 2, "transferring dependency %d -> %d to %d\n",
					  te->dumpId, olddep, AH->tableDataId[olddep]);
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

	/* Quick exit if no dependencies at all */
	if (te->nDeps == 0)
		return;

	/* Exit if this entry doesn't need exclusive lock on other objects */
	if (!(strcmp(te->desc, "CONSTRAINT") == 0 ||
		  strcmp(te->desc, "CHECK CONSTRAINT") == 0 ||
		  strcmp(te->desc, "FK CONSTRAINT") == 0 ||
		  strcmp(te->desc, "RULE") == 0 ||
		  strcmp(te->desc, "TRIGGER") == 0))
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
 * becomes ready should be moved to the ready list.
 */
static void
reduce_dependencies(ArchiveHandle *AH, TocEntry *te, TocEntry *ready_list)
{
	int			i;

	ahlog(AH, 2, "reducing dependencies for %d\n", te->dumpId);

	for (i = 0; i < te->nRevDeps; i++)
	{
		TocEntry   *otherte = AH->tocsByDumpId[te->revDeps[i]];

		otherte->depCount--;
		if (otherte->depCount == 0 && otherte->par_prev != NULL)
		{
			/* It must be in the pending list, so remove it ... */
			par_list_remove(otherte);
			/* ... and add to ready_list */
			par_list_append(ready_list, otherte);
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
	ahlog(AH, 1, "table \"%s\" could not be created, will not restore its data\n",
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
	clone->currUser = NULL;
	clone->currSchema = NULL;
	clone->currTablespace = NULL;
	clone->currWithOids = -1;

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
	}
	else
	{
		char	   *dbname;
		char	   *pghost;
		char	   *pgport;
		char	   *username;
		const char *encname;

		Assert(AH->connection != NULL);

		/*
		 * Even though we are technically accessing the parent's database
		 * object here, these functions are fine to be called like that
		 * because all just return a pointer and do not actually send/receive
		 * any data to/from the database.
		 */
		dbname = PQdb(AH->connection);
		pghost = PQhost(AH->connection);
		pgport = PQport(AH->connection);
		username = PQuser(AH->connection);
		encname = pg_encoding_to_char(AH->public.encoding);

		/* this also sets clone->connection */
		ConnectDatabase((Archive *) clone, dbname, pghost, pgport, username, TRI_NO);

		/*
		 * Set the same encoding, whatever we set here is what we got from
		 * pg_encoding_to_char(), so we really shouldn't run into an error
		 * setting that very same value. Also see the comment in
		 * SetupConnection().
		 */
		PQsetClientEncoding(clone->connection, encname);
	}

	/* Let the format-specific code have a chance too */
	(clone->ClonePtr) (clone);

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
	/* Clear format-specific state */
	(AH->DeClonePtr) (AH);

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
	if (AH->savedPassword)
		free(AH->savedPassword);

	free(AH);
}
