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
 *		$PostgreSQL: pgsql/src/bin/pg_dump/pg_backup_archiver.c,v 1.172.2.3 2010/08/21 13:59:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup_db.h"
#include "dumputils.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifdef WIN32
#include <io.h>
#endif

#include "libpq/libpq-fs.h"

/*
 * Special exit values from worker children.  We reserve 0 for normal
 * success; 1 and other small values should be interpreted as crashes.
 */
#define WORKER_CREATE_DONE		10
#define WORKER_INHIBIT_DATA		11
#define WORKER_IGNORED_ERRORS	12

/*
 * Unix uses exit to return result from worker child, so function is void.
 * Windows thread result comes via function return.
 */
#ifndef WIN32
#define parallel_restore_result void
#else
#define parallel_restore_result DWORD
#endif

/* IDs for worker children are either PIDs or thread handles */
#ifndef WIN32
#define thandle pid_t
#else
#define thandle HANDLE
#endif

typedef struct _restore_args
{
	ArchiveHandle *AH;
	TocEntry   *te;
} RestoreArgs;

typedef struct _parallel_slot
{
	thandle		child_id;
	RestoreArgs *args;
} ParallelSlot;

#define NO_SLOT (-1)

const char *progname;

static const char *modulename = gettext_noop("archiver");


static ArchiveHandle *_allocAH(const char *FileSpec, const ArchiveFormat fmt,
		 const int compression, ArchiveMode mode);
static void _getObjectDescription(PQExpBuffer buf, TocEntry *te,
					  ArchiveHandle *AH);
static void _printTocEntry(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt, bool isData, bool acl_pass);
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
static teReqs _tocEntryRequired(TocEntry *te, RestoreOptions *ropt, bool include_acls);
static void _disableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);
static void _enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);
static TocEntry *getTocEntryByDumpId(ArchiveHandle *AH, DumpId id);
static void _moveBefore(ArchiveHandle *AH, TocEntry *pos, TocEntry *te);
static int	_discoverArchiveFormat(ArchiveHandle *AH);

static int	RestoringToDB(ArchiveHandle *AH);
static void dump_lo_buf(ArchiveHandle *AH);
static void _write_msg(const char *modulename, const char *fmt, va_list ap);
static void _die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt, va_list ap);

static void dumpTimestamp(ArchiveHandle *AH, const char *msg, time_t tim);
static OutputContext SetOutput(ArchiveHandle *AH, char *filename, int compression);
static void ResetOutput(ArchiveHandle *AH, OutputContext savedContext);

static int restore_toc_entry(ArchiveHandle *AH, TocEntry *te,
				  RestoreOptions *ropt, bool is_parallel);
static void restore_toc_entries_parallel(ArchiveHandle *AH);
static thandle spawn_restore(RestoreArgs *args);
static thandle reap_child(ParallelSlot *slots, int n_slots, int *work_status);
static bool work_in_progress(ParallelSlot *slots, int n_slots);
static int	get_next_slot(ParallelSlot *slots, int n_slots);
static TocEntry *get_next_work_item(ArchiveHandle *AH,
				   TocEntry **first_unprocessed,
				   ParallelSlot *slots, int n_slots);
static parallel_restore_result parallel_restore(RestoreArgs *args);
static void mark_work_done(ArchiveHandle *AH, thandle worker, int status,
			   ParallelSlot *slots, int n_slots);
static void fix_dependencies(ArchiveHandle *AH);
static bool has_lock_conflicts(TocEntry *te1, TocEntry *te2);
static void repoint_table_dependencies(ArchiveHandle *AH,
						   DumpId tableId, DumpId tableDataId);
static void identify_locking_dependencies(TocEntry *te,
							  TocEntry **tocsByDumpId,
							  DumpId maxDumpId);
static void reduce_dependencies(ArchiveHandle *AH, TocEntry *te);
static void mark_create_done(ArchiveHandle *AH, TocEntry *te);
static void inhibit_data_for_failed_table(ArchiveHandle *AH, TocEntry *te);
static ArchiveHandle *CloneArchive(ArchiveHandle *AH);
static void DeCloneArchive(ArchiveHandle *AH);


/*
 *	Wrapper functions.
 *
 *	The objective it to make writing new formats and dumpers as simple
 *	as possible, if necessary at the expense of extra function calls etc.
 *
 */


/* Create a new archive */
/* Public */
Archive *
CreateArchive(const char *FileSpec, const ArchiveFormat fmt,
			  const int compression, ArchiveMode mode)

{
	ArchiveHandle *AH = _allocAH(FileSpec, fmt, compression, mode);

	return (Archive *) AH;
}

/* Open an existing archive */
/* Public */
Archive *
OpenArchive(const char *FileSpec, const ArchiveFormat fmt)
{
	ArchiveHandle *AH = _allocAH(FileSpec, fmt, 0, archModeRead);

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
		die_horribly(AH, modulename, "could not close output file: %s\n",
					 strerror(errno));
}

/* Public */
void
RestoreArchive(Archive *AHX, RestoreOptions *ropt)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	bool		parallel_mode;
	TocEntry   *te;
	teReqs		reqs;
	OutputContext sav;

	AH->ropt = ropt;
	AH->stage = STAGE_INITIALIZING;

	/*
	 * Check for nonsensical option combinations.
	 *
	 * NB: create+dropSchema is useless because if you're creating the DB,
	 * there's no need to drop individual items in it.  Moreover, if we tried
	 * to do that then we'd issue the drops in the database initially
	 * connected to, not the one we will create, which is very bad...
	 */
	if (ropt->create && ropt->dropSchema)
		die_horribly(AH, modulename, "-C and -c are incompatible options\n");

	/*
	 * -1 is not compatible with -C, because we can't create a database inside
	 * a transaction block.
	 */
	if (ropt->create && ropt->single_txn)
		die_horribly(AH, modulename, "-C and -1 are incompatible options\n");

	/*
	 * If we're going to do parallel restore, there are some restrictions.
	 */
	parallel_mode = (ropt->number_of_jobs > 1 && ropt->useDB);
	if (parallel_mode)
	{
		/* We haven't got round to making this work for all archive formats */
		if (AH->ClonePtr == NULL || AH->ReopenPtr == NULL)
			die_horribly(AH, modulename, "parallel restore is not supported with this archive file format\n");

		/* Doesn't work if the archive represents dependencies as OIDs */
		if (AH->version < K_VERS_1_8)
			die_horribly(AH, modulename, "parallel restore is not supported with archives made by pre-8.0 pg_dump\n");

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
			reqs = _tocEntryRequired(te, ropt, false);
			if (te->hadDumper && (reqs & REQ_DATA) != 0)
				die_horribly(AH, modulename, "cannot restore from compressed archive (compression not supported in this installation)\n");
		}
	}
#endif

	/*
	 * If we're using a DB connection, then connect it.
	 */
	if (ropt->useDB)
	{
		ahlog(AH, 1, "connecting to database for restore\n");
		if (AH->version < K_VERS_1_3)
			die_horribly(AH, modulename, "direct database connections are not supported in pre-1.3 archives\n");

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
			reqs = _tocEntryRequired(te, ropt, true);
			if ((reqs & REQ_SCHEMA) != 0)
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
	if (ropt->filename || ropt->compression)
		sav = SetOutput(AH, ropt->filename, ropt->compression);

	ahprintf(AH, "--\n-- PostgreSQL database dump\n--\n\n");

	if (AH->public.verbose)
		dumpTimestamp(AH, "Started on", AH->createDate);

	if (ropt->single_txn)
	{
		if (AH->connection)
			StartTransaction(AH);
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

			reqs = _tocEntryRequired(te, ropt, false /* needn't drop ACLs */ );
			if (((reqs & REQ_SCHEMA) != 0) && te->dropStmt)
			{
				/* We want the schema */
				ahlog(AH, 1, "dropping %s %s\n", te->desc, te->tag);
				/* Select owner and schema as necessary */
				_becomeOwner(AH, te);
				_selectOutputSchema(AH, te->namespace);
				/* Drop it */
				ahprintf(AH, "%s", te->dropStmt);
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
		restore_toc_entries_parallel(AH);
	else
	{
		for (te = AH->toc->next; te != AH->toc; te = te->next)
			(void) restore_toc_entry(AH, te, ropt, false);
	}

	/*
	 * Scan TOC again to output ownership commands and ACLs
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		AH->currentTE = te;

		/* Work out what, if anything, we want from this entry */
		reqs = _tocEntryRequired(te, ropt, true);

		if ((reqs & REQ_SCHEMA) != 0)	/* We want the schema */
		{
			ahlog(AH, 1, "setting owner and privileges for %s %s\n",
				  te->desc, te->tag);
			_printTocEntry(AH, te, ropt, false, true);
		}
	}

	if (ropt->single_txn)
	{
		if (AH->connection)
			CommitTransaction(AH);
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
		ResetOutput(AH, sav);

	if (ropt->useDB)
	{
		PQfinish(AH->connection);
		AH->connection = NULL;
	}
}

/*
 * Restore a single TOC item.  Used in both parallel and non-parallel restore;
 * is_parallel is true if we are in a worker child process.
 *
 * Returns 0 normally, but WORKER_CREATE_DONE or WORKER_INHIBIT_DATA if
 * the parallel parent has to make the corresponding status update.
 */
static int
restore_toc_entry(ArchiveHandle *AH, TocEntry *te,
				  RestoreOptions *ropt, bool is_parallel)
{
	int			retval = 0;
	teReqs		reqs;
	bool		defnDumped;

	AH->currentTE = te;

	/* Work out what, if anything, we want from this entry */
	reqs = _tocEntryRequired(te, ropt, false);

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
		ahlog(AH, 1, "creating %s %s\n", te->desc, te->tag);

		_printTocEntry(AH, te, ropt, false, false);
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
						retval = WORKER_INHIBIT_DATA;
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
					retval = WORKER_CREATE_DONE;
				else
					mark_create_done(AH, te);
			}
		}

		/* If we created a DB, connect to it... */
		if (strcmp(te->desc, "DATABASE") == 0)
		{
			ahlog(AH, 1, "connecting to new database \"%s\"\n", te->tag);
			_reconnectToDB(AH, te->tag);
			ropt->dbname = strdup(te->tag);
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
			if (AH->PrintTocDataPtr !=NULL && (reqs & REQ_DATA) != 0)
			{
				_printTocEntry(AH, te, ropt, true, false);

				if (strcmp(te->desc, "BLOBS") == 0 ||
					strcmp(te->desc, "BLOB COMMENTS") == 0)
				{
					ahlog(AH, 1, "restoring %s\n", te->desc);

					_selectOutputSchema(AH, "pg_catalog");

					(*AH->PrintTocDataPtr) (AH, te, ropt);
				}
				else
				{
					_disableTriggersIfNecessary(AH, te, ropt);

					/* Select owner and schema as necessary */
					_becomeOwner(AH, te);
					_selectOutputSchema(AH, te->namespace);

					ahlog(AH, 1, "restoring data for table \"%s\"\n",
						  te->tag);

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
						StartTransaction(AH);

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

					(*AH->PrintTocDataPtr) (AH, te, ropt);

					/*
					 * Terminate COPY if needed.
					 */
					if (AH->outputKind == OUTPUT_COPYDATA &&
						RestoringToDB(AH))
						EndDBCopyMode(AH, te);
					AH->outputKind = OUTPUT_SQLCMDS;

					/* close out the transaction started above */
					if (is_parallel && te->created)
						CommitTransaction(AH);

					_enableTriggersIfNecessary(AH, te, ropt);
				}
			}
		}
		else if (!defnDumped)
		{
			/* If we haven't already dumped the defn part, do so now */
			ahlog(AH, 1, "executing %s %s\n", te->desc, te->tag);
			_printTocEntry(AH, te, ropt, false, false);
		}
	}

	return retval;
}

/*
 * Allocate a new RestoreOptions block.
 * This is mainly so we can initialize it, but also for future expansion,
 */
RestoreOptions *
NewRestoreOptions(void)
{
	RestoreOptions *opts;

	opts = (RestoreOptions *) calloc(1, sizeof(RestoreOptions));

	/* set any fields that shouldn't default to zeroes */
	opts->format = archUnknown;
	opts->promptPassword = TRI_DEFAULT;

	return opts;
}

static void
_disableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt)
{
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
_enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt)
{
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
size_t
WriteData(Archive *AHX, const void *data, size_t dLen)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (!AH->currToc)
		die_horribly(AH, modulename, "internal error -- WriteData cannot be called outside the context of a DataDumper routine\n");

	return (*AH->WriteDataPtr) (AH, data, dLen);
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

	newToc = (TocEntry *) calloc(1, sizeof(TocEntry));
	if (!newToc)
		die_horribly(AH, modulename, "out of memory\n");

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

	newToc->tag = strdup(tag);
	newToc->namespace = namespace ? strdup(namespace) : NULL;
	newToc->tablespace = tablespace ? strdup(tablespace) : NULL;
	newToc->owner = strdup(owner);
	newToc->withOids = withOids;
	newToc->desc = strdup(desc);
	newToc->defn = strdup(defn);
	newToc->dropStmt = strdup(dropStmt);
	newToc->copyStmt = copyStmt ? strdup(copyStmt) : NULL;

	if (nDeps > 0)
	{
		newToc->dependencies = (DumpId *) malloc(nDeps * sizeof(DumpId));
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
PrintTOCSummary(Archive *AHX, RestoreOptions *ropt)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	TocEntry   *te;
	OutputContext sav;
	char	   *fmtName;

	if (ropt->filename)
		sav = SetOutput(AH, ropt->filename, 0 /* no compression */ );

	ahprintf(AH, ";\n; Archive created at %s", ctime(&AH->createDate));
	ahprintf(AH, ";     dbname: %s\n;     TOC Entries: %d\n;     Compression: %d\n",
			 AH->archdbname, AH->tocCount, AH->compression);

	switch (AH->format)
	{
		case archFiles:
			fmtName = "FILES";
			break;
		case archCustom:
			fmtName = "CUSTOM";
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

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (ropt->verbose || _tocEntryRequired(te, ropt, true) != 0)
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

	if (ropt->filename)
		ResetOutput(AH, sav);
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
		die_horribly(AH, modulename, "large-object output not supported in chosen format\n");

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
	if (!AH->ropt->single_txn)
	{
		if (AH->connection)
			StartTransaction(AH);
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
	if (!AH->ropt->single_txn)
	{
		if (AH->connection)
			CommitTransaction(AH);
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
StartRestoreBlob(ArchiveHandle *AH, Oid oid)
{
	Oid			loOid;

	AH->blobCount++;

	/* Initialize the LO Buffer */
	AH->lo_buf_used = 0;

	ahlog(AH, 1, "restoring large object with OID %u\n", oid);

	if (AH->connection)
	{
		loOid = lo_create(AH->connection, oid);
		if (loOid == 0 || loOid != oid)
			die_horribly(AH, modulename, "could not create large object %u\n",
						 oid);

		AH->loFd = lo_open(AH->connection, oid, INV_WRITE);
		if (AH->loFd == -1)
			die_horribly(AH, modulename, "could not open large object\n");
	}
	else
	{
		ahprintf(AH, "SELECT lo_open(lo_create(%u), %d);\n", oid, INV_WRITE);
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
		ahprintf(AH, "SELECT lo_close(0);\n\n");
	}
}

/***********
 * Sorting and Reordering
 ***********/

void
SortTocFromFile(Archive *AHX, RestoreOptions *ropt)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	FILE	   *fh;
	char		buf[100];
	bool		incomplete_line;

	/* Allocate space for the 'wanted' array, and init it */
	ropt->idWanted = (bool *) malloc(sizeof(bool) * AH->maxDumpId);
	memset(ropt->idWanted, 0, sizeof(bool) * AH->maxDumpId);

	/* Setup the file */
	fh = fopen(ropt->tocFile, PG_BINARY_R);
	if (!fh)
		die_horribly(AH, modulename, "could not open TOC file \"%s\": %s\n",
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
			die_horribly(AH, modulename, "could not find entry for ID %d\n",
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
		die_horribly(AH, modulename, "could not close TOC file: %s\n",
					 strerror(errno));
}

/*
 * Set up a dummy ID filter that selects all dump IDs
 */
void
InitDummyWantedList(Archive *AHX, RestoreOptions *ropt)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	/* Allocate space for the 'wanted' array, and init it to 1's */
	ropt->idWanted = (bool *) malloc(sizeof(bool) * AH->maxDumpId);
	memset(ropt->idWanted, 1, sizeof(bool) * AH->maxDumpId);
}

/**********************
 * 'Convenience functions that look like standard IO functions
 * for writing data when in dump mode.
 **********************/

/* Public */
int
archputs(const char *s, Archive *AH)
{
	return WriteData(AH, s, strlen(s));
}

/* Public */
int
archprintf(Archive *AH, const char *fmt,...)
{
	char	   *p = NULL;
	va_list		ap;
	int			bSize = strlen(fmt) + 256;
	int			cnt = -1;

	/*
	 * This is paranoid: deal with the possibility that vsnprintf is willing
	 * to ignore trailing null or returns > 0 even if string does not fit. It
	 * may be the case that it returns cnt = bufsize
	 */
	while (cnt < 0 || cnt >= (bSize - 1))
	{
		if (p != NULL)
			free(p);
		bSize *= 2;
		p = (char *) malloc(bSize);
		if (p == NULL)
			exit_horribly(AH, modulename, "out of memory\n");
		va_start(ap, fmt);
		cnt = vsnprintf(p, bSize, fmt, ap);
		va_end(ap);
	}
	WriteData(AH, p, cnt);
	free(p);
	return cnt;
}


/*******************************
 * Stuff below here should be 'private' to the archiver routines
 *******************************/

static OutputContext
SetOutput(ArchiveHandle *AH, char *filename, int compression)
{
	OutputContext sav;
	int			fn;

	/* Replace the AH output file handle */
	sav.OF = AH->OF;
	sav.gzOut = AH->gzOut;

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
			die_horribly(AH, modulename, "could not open output file \"%s\": %s\n",
						 filename, strerror(errno));
		else
			die_horribly(AH, modulename, "could not open output file: %s\n",
						 strerror(errno));
	}

	return sav;
}

static void
ResetOutput(ArchiveHandle *AH, OutputContext sav)
{
	int			res;

	if (AH->gzOut)
		res = GZCLOSE(AH->OF);
	else
		res = fclose(AH->OF);

	if (res != 0)
		die_horribly(AH, modulename, "could not close output file: %s\n",
					 strerror(errno));

	AH->gzOut = sav.gzOut;
	AH->OF = sav.OF;
}



/*
 *	Print formatted text to the output file (usually stdout).
 */
int
ahprintf(ArchiveHandle *AH, const char *fmt,...)
{
	char	   *p = NULL;
	va_list		ap;
	int			bSize = strlen(fmt) + 256;		/* Usually enough */
	int			cnt = -1;

	/*
	 * This is paranoid: deal with the possibility that vsnprintf is willing
	 * to ignore trailing null or returns > 0 even if string does not fit.
	 * It may be the case that it returns cnt = bufsize.
	 */
	while (cnt < 0 || cnt >= (bSize - 1))
	{
		if (p != NULL)
			free(p);
		bSize *= 2;
		p = (char *) malloc(bSize);
		if (p == NULL)
			die_horribly(AH, modulename, "out of memory\n");
		va_start(ap, fmt);
		cnt = vsnprintf(p, bSize, fmt, ap);
		va_end(ap);
	}
	ahwrite(p, 1, cnt, AH);
	free(p);
	return cnt;
}

void
ahlog(ArchiveHandle *AH, int level, const char *fmt,...)
{
	va_list		ap;

	if (AH->debugLevel < level && (!AH->public.verbose || level > 1))
		return;

	va_start(ap, fmt);
	_write_msg(NULL, fmt, ap);
	va_end(ap);
}

/*
 * Single place for logic which says 'We are restoring to a direct DB connection'.
 */
static int
RestoringToDB(ArchiveHandle *AH)
{
	return (AH->ropt && AH->ropt->useDB && AH->connection);
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
			die_horribly(AH, modulename,
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
int
ahwrite(const void *ptr, size_t size, size_t nmemb, ArchiveHandle *AH)
{
	size_t		res;

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

		return size * nmemb;
	}
	else if (AH->gzOut)
	{
		res = GZWRITE((void *) ptr, size, nmemb, AH->OF);
		if (res != (nmemb * size))
			die_horribly(AH, modulename, "could not write to output file: %s\n", strerror(errno));
		return res;
	}
	else if (AH->CustomOutPtr)
	{
		res = AH->CustomOutPtr (AH, ptr, size * nmemb);

		if (res != (nmemb * size))
			die_horribly(AH, modulename, "could not write to custom output routine\n");
		return res;
	}
	else
	{
		/*
		 * If we're doing a restore, and it's direct to DB, and we're
		 * connected then send it to the DB.
		 */
		if (RestoringToDB(AH))
			return ExecuteSqlCommandBuf(AH, (const char *) ptr, size * nmemb);
		else
		{
			res = fwrite((void *) ptr, size, nmemb, AH->OF);
			if (res != nmemb)
				die_horribly(AH, modulename, "could not write to output file: %s\n",
							 strerror(errno));
			return res;
		}
	}
}

/* Common exit code */
static void
_write_msg(const char *modulename, const char *fmt, va_list ap)
{
	if (modulename)
		fprintf(stderr, "%s: [%s] ", progname, _(modulename));
	else
		fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, _(fmt), ap);
}

void
write_msg(const char *modulename, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	_write_msg(modulename, fmt, ap);
	va_end(ap);
}


static void
_die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt, va_list ap)
{
	_write_msg(modulename, fmt, ap);

	if (AH)
	{
		if (AH->public.verbose)
			write_msg(NULL, "*** aborted because of error\n");
		if (AH->connection)
			PQfinish(AH->connection);
	}

	exit(1);
}

/* External use */
void
exit_horribly(Archive *AH, const char *modulename, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	_die_horribly((ArchiveHandle *) AH, modulename, fmt, ap);
	va_end(ap);
}

/* Archiver use (just different arg declaration) */
void
die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	_die_horribly(AH, modulename, fmt, ap);
	va_end(ap);
}

/* on some error, we may decide to go on... */
void
warn_or_die_horribly(ArchiveHandle *AH,
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
	if (AH->public.exit_on_error)
		_die_horribly(AH, modulename, fmt, ap);
	else
	{
		_write_msg(modulename, fmt, ap);
		AH->public.n_errors++;
	}
	va_end(ap);
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

static TocEntry *
getTocEntryByDumpId(ArchiveHandle *AH, DumpId id)
{
	TocEntry   *te;

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (te->dumpId == id)
			return te;
	}
	return NULL;
}

teReqs
TocIDRequired(ArchiveHandle *AH, DumpId id, RestoreOptions *ropt)
{
	TocEntry   *te = getTocEntryByDumpId(AH, id);

	if (!te)
		return 0;

	return _tocEntryRequired(te, ropt, true);
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
			die_horribly(AH, modulename, "unexpected data offset flag %d\n", offsetFlg);
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
				die_horribly(AH, modulename, "file offset in dump file is too large\n");
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
		res = WriteInt(AH, strlen(c));
		res += (*AH->WriteBufPtr) (AH, c, strlen(c));
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
		buf = (char *) malloc(l + 1);
		if (!buf)
			die_horribly(AH, modulename, "out of memory\n");

		if ((*AH->ReadBufPtr) (AH, (void *) buf, l) != l)
			die_horribly(AH, modulename, "unexpected end of file\n");

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
	AH->lookahead = calloc(1, 512);
	AH->lookaheadLen = 0;
	AH->lookaheadPos = 0;

	if (AH->fSpec)
	{
		wantClose = 1;
		fh = fopen(AH->fSpec, PG_BINARY_R);
		if (!fh)
			die_horribly(AH, modulename, "could not open input file \"%s\": %s\n",
						 AH->fSpec, strerror(errno));
	}
	else
	{
		fh = stdin;
		if (!fh)
			die_horribly(AH, modulename, "could not open input file: %s\n",
						 strerror(errno));
	}

	cnt = fread(sig, 1, 5, fh);

	if (cnt != 5)
	{
		if (ferror(fh))
			die_horribly(AH, modulename, "could not read input file: %s\n", strerror(errno));
		else
			die_horribly(AH, modulename, "input file is too short (read %lu, expected 5)\n",
						 (unsigned long) cnt);
	}

	/* Save it, just in case we need it later */
	strncpy(&AH->lookahead[0], sig, 5);
	AH->lookaheadLen = 5;

	if (strncmp(sig, "PGDMP", 5) == 0)
	{
		/*
		 * Finish reading (most of) a custom-format header.
		 *
		 * NB: this code must agree with ReadHead().
		 */
		AH->vmaj = fgetc(fh);
		AH->vmin = fgetc(fh);

		/* Save these too... */
		AH->lookahead[AH->lookaheadLen++] = AH->vmaj;
		AH->lookahead[AH->lookaheadLen++] = AH->vmin;

		/* Check header version; varies from V1.0 */
		if (AH->vmaj > 1 || ((AH->vmaj == 1) && (AH->vmin > 0)))		/* Version > 1.0 */
		{
			AH->vrev = fgetc(fh);
			AH->lookahead[AH->lookaheadLen++] = AH->vrev;
		}
		else
			AH->vrev = 0;

		/* Make a convenient integer <maj><min><rev>00 */
		AH->version = ((AH->vmaj * 256 + AH->vmin) * 256 + AH->vrev) * 256 + 0;

		AH->intSize = fgetc(fh);
		AH->lookahead[AH->lookaheadLen++] = AH->intSize;

		if (AH->version >= K_VERS_1_7)
		{
			AH->offSize = fgetc(fh);
			AH->lookahead[AH->lookaheadLen++] = AH->offSize;
		}
		else
			AH->offSize = AH->intSize;

		AH->format = fgetc(fh);
		AH->lookahead[AH->lookaheadLen++] = AH->format;
	}
	else
	{
		/*
		 * *Maybe* we have a tar archive format file... So, read first 512
		 * byte header...
		 */
		cnt = fread(&AH->lookahead[AH->lookaheadLen], 1, 512 - AH->lookaheadLen, fh);
		AH->lookaheadLen += cnt;

		if (AH->lookaheadLen != 512)
			die_horribly(AH, modulename, "input file does not appear to be a valid archive (too short?)\n");

		if (!isValidTarHeader(AH->lookahead))
			die_horribly(AH, modulename, "input file does not appear to be a valid archive\n");

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
			die_horribly(AH, modulename, "could not close input file: %s\n",
						 strerror(errno));

	return AH->format;
}


/*
 * Allocate an archive handle
 */
static ArchiveHandle *
_allocAH(const char *FileSpec, const ArchiveFormat fmt,
		 const int compression, ArchiveMode mode)
{
	ArchiveHandle *AH;

#if 0
	write_msg(modulename, "allocating AH for %s, format %d\n", FileSpec, fmt);
#endif

	AH = (ArchiveHandle *) calloc(1, sizeof(ArchiveHandle));
	if (!AH)
		die_horribly(AH, modulename, "out of memory\n");

	/* AH->debugLevel = 100; */

	AH->vmaj = K_VERS_MAJOR;
	AH->vmin = K_VERS_MINOR;
	AH->vrev = K_VERS_REV;

	/* initialize for backwards compatible string processing */
	AH->public.encoding = 0;	/* PG_SQL_ASCII */
	AH->public.std_strings = false;

	/* sql error handling */
	AH->public.exit_on_error = true;
	AH->public.n_errors = 0;

	AH->createDate = time(NULL);

	AH->intSize = sizeof(int);
	AH->offSize = sizeof(pgoff_t);
	if (FileSpec)
	{
		AH->fSpec = strdup(FileSpec);

		/*
		 * Not used; maybe later....
		 *
		 * AH->workDir = strdup(FileSpec); for(i=strlen(FileSpec) ; i > 0 ;
		 * i--) if (AH->workDir[i-1] == '/')
		 */
	}
	else
		AH->fSpec = NULL;

	AH->currUser = NULL;		/* unknown */
	AH->currSchema = NULL;		/* ditto */
	AH->currTablespace = NULL;	/* ditto */
	AH->currWithOids = -1;		/* force SET */

	AH->toc = (TocEntry *) calloc(1, sizeof(TocEntry));
	if (!AH->toc)
		die_horribly(AH, modulename, "out of memory\n");

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

		case archFiles:
			InitArchiveFmt_Files(AH);
			break;

		case archNull:
			InitArchiveFmt_Null(AH);
			break;

		case archTar:
			InitArchiveFmt_Tar(AH);
			break;

		default:
			die_horribly(AH, modulename, "unrecognized file format \"%d\"\n", fmt);
	}

	return AH;
}


void
WriteDataChunks(ArchiveHandle *AH)
{
	TocEntry   *te;
	StartDataPtr startPtr;
	EndDataPtr	endPtr;

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (te->dataDumper != NULL)
		{
			AH->currToc = te;
			/* printf("Writing data for %d (%x)\n", te->id, te); */

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
			 * printf("Dumper arg for %d is %x\n", te->id, te->dataDumperArg);
			 */

			/*
			 * The user-provided DataDumper routine needs to call
			 * AH->WriteData
			 */
			(*te->dataDumper) ((Archive *) AH, te->dataDumperArg);

			if (endPtr != NULL)
				(*endPtr) (AH, te);
			AH->currToc = NULL;
		}
	}
}

void
WriteToc(ArchiveHandle *AH)
{
	TocEntry   *te;
	char		workbuf[32];
	int			i;

	/* printf("%d TOC Entries to save\n", AH->tocCount); */

	WriteInt(AH, AH->tocCount);

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
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
		te = (TocEntry *) calloc(1, sizeof(TocEntry));
		te->dumpId = ReadInt(AH);

		if (te->dumpId > AH->maxDumpId)
			AH->maxDumpId = te->dumpId;

		/* Sanity check */
		if (te->dumpId <= 0)
			die_horribly(AH, modulename,
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
			 * rules for pre-8.4 archives wherein pg_dump hasn't classified
			 * the entries into sections
			 */
			if (strcmp(te->desc, "COMMENT") == 0 ||
				strcmp(te->desc, "ACL") == 0)
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
			deps = (DumpId *) malloc(sizeof(DumpId) * depSize);
			depIdx = 0;
			for (;;)
			{
				tmp = ReadStr(AH);
				if (!tmp)
					break;		/* end of list */
				if (depIdx >= depSize)
				{
					depSize *= 2;
					deps = (DumpId *) realloc(deps, sizeof(DumpId) * depSize);
				}
				sscanf(tmp, "%d", &deps[depIdx]);
				free(tmp);
				depIdx++;
			}

			if (depIdx > 0)		/* We have a non-null entry */
			{
				deps = (DumpId *) realloc(deps, sizeof(DumpId) * depIdx);
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
	char	   *defn = strdup(te->defn);
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
			die_horribly(AH, modulename, "unrecognized encoding \"%s\"\n",
						 ptr1);
		AH->public.encoding = encoding;
	}
	else
		die_horribly(AH, modulename, "invalid ENCODING item: %s\n",
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
		die_horribly(AH, modulename, "invalid STDSTRINGS item: %s\n",
					 te->defn);
}

static teReqs
_tocEntryRequired(TocEntry *te, RestoreOptions *ropt, bool include_acls)
{
	teReqs		res = REQ_ALL;

	/* ENCODING and STDSTRINGS items are dumped specially, so always reject */
	if (strcmp(te->desc, "ENCODING") == 0 ||
		strcmp(te->desc, "STDSTRINGS") == 0)
		return 0;

	/* If it's an ACL, maybe ignore it */
	if ((!include_acls || ropt->aclsSkip) && strcmp(te->desc, "ACL") == 0)
		return 0;

	if (!ropt->create && strcmp(te->desc, "DATABASE") == 0)
		return 0;

	/* Check options for selective dump/restore */
	if (ropt->schemaNames)
	{
		/* If no namespace is specified, it means all. */
		if (!te->namespace)
			return 0;
		if (strcmp(ropt->schemaNames, te->namespace) != 0)
			return 0;
	}

	if (ropt->selTypes)
	{
		if (strcmp(te->desc, "TABLE") == 0 ||
			strcmp(te->desc, "TABLE DATA") == 0)
		{
			if (!ropt->selTable)
				return 0;
			if (ropt->tableNames && strcmp(ropt->tableNames, te->tag) != 0)
				return 0;
		}
		else if (strcmp(te->desc, "INDEX") == 0)
		{
			if (!ropt->selIndex)
				return 0;
			if (ropt->indexNames && strcmp(ropt->indexNames, te->tag) != 0)
				return 0;
		}
		else if (strcmp(te->desc, "FUNCTION") == 0)
		{
			if (!ropt->selFunction)
				return 0;
			if (ropt->functionNames && strcmp(ropt->functionNames, te->tag) != 0)
				return 0;
		}
		else if (strcmp(te->desc, "TRIGGER") == 0)
		{
			if (!ropt->selTrigger)
				return 0;
			if (ropt->triggerNames && strcmp(ropt->triggerNames, te->tag) != 0)
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
		 * Special Case: If 'SEQUENCE SET' then it is considered a data entry
		 */
		if (strcmp(te->desc, "SEQUENCE SET") == 0)
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

	/* Mask it we only want data */
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
 * Issue SET commands for parameters that we want to have set the same way
 * at all times during execution of a restore script.
 */
static void
_doSetFixedOutputState(ArchiveHandle *AH)
{
	/* Disable statement_timeout in archive for pg_restore/psql  */
	ahprintf(AH, "SET statement_timeout = 0;\n");

	/* Select the correct character set encoding */
	ahprintf(AH, "SET client_encoding = '%s';\n",
			 pg_encoding_to_char(AH->public.encoding));

	/* Select the correct string literal syntax */
	ahprintf(AH, "SET standard_conforming_strings = %s;\n",
			 AH->public.std_strings ? "on" : "off");

	/* Select the role to be used during restore */
	if (AH->ropt && AH->ropt->use_role)
		ahprintf(AH, "SET ROLE %s;\n", fmtId(AH->ropt->use_role));

	/* Make sure function checking is disabled */
	ahprintf(AH, "SET check_function_bodies = false;\n");

	/* Avoid annoying notices etc */
	ahprintf(AH, "SET client_min_messages = warning;\n");
	if (!AH->public.std_strings)
		ahprintf(AH, "SET escape_string_warning = off;\n");

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

	appendPQExpBuffer(cmd, "SET SESSION AUTHORIZATION ");

	/*
	 * SQL requires a string literal here.  Might as well be correct.
	 */
	if (user && *user)
		appendStringLiteralAHX(cmd, user, AH);
	else
		appendPQExpBuffer(cmd, "DEFAULT");
	appendPQExpBuffer(cmd, ";");

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, cmd->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			/* NOT warn_or_die_horribly... use -O instead to skip this. */
			die_horribly(AH, modulename, "could not set session user to \"%s\": %s",
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
			warn_or_die_horribly(AH, modulename,
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
	AH->currUser = strdup(user);
}

/*
 * Become the owner of the the given TOC entry object.  If
 * changes in ownership are not allowed, this doesn't do anything.
 */
static void
_becomeOwner(ArchiveHandle *AH, TocEntry *te)
{
	if (AH->ropt && (AH->ropt->noOwner || !AH->ropt->use_setsessauth))
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
		appendPQExpBuffer(qry, ", pg_catalog");

	if (RestoringToDB(AH))
	{
		PGresult   *res;

		res = PQexec(AH->connection, qry->data);

		if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
			warn_or_die_horribly(AH, modulename,
								 "could not set search_path to \"%s\": %s",
								 schemaName, PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s;\n\n", qry->data);

	if (AH->currSchema)
		free(AH->currSchema);
	AH->currSchema = strdup(schemaName);

	destroyPQExpBuffer(qry);
}

/*
 * Issue the commands to select the specified tablespace as the current one
 * in the target database.
 */
static void
_selectTablespace(ArchiveHandle *AH, const char *tablespace)
{
	PQExpBuffer qry;
	const char *want,
			   *have;

	/* do nothing in --no-tablespaces mode */
	if (AH->ropt->noTablespace)
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
		appendPQExpBuffer(qry, "SET default_tablespace = ''");
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
			warn_or_die_horribly(AH, modulename,
								 "could not set default_tablespace to %s: %s",
								 fmtId(want), PQerrorMessage(AH->connection));

		PQclear(res);
	}
	else
		ahprintf(AH, "%s;\n\n", qry->data);

	if (AH->currTablespace)
		free(AH->currTablespace);
	AH->currTablespace = strdup(want);

	destroyPQExpBuffer(qry);
}

/*
 * Extract an object description for a TOC entry, and append it to buf.
 *
 * This is not quite as general as it may seem, since it really only
 * handles constructing the right thing to put into ALTER ... OWNER TO.
 *
 * The whole thing is pretty grotty, but we are kind of stuck since the
 * information used is all that's available in older dump files.
 */
static void
_getObjectDescription(PQExpBuffer buf, TocEntry *te, ArchiveHandle *AH)
{
	const char *type = te->desc;

	/* Use ALTER TABLE for views and sequences */
	if (strcmp(type, "VIEW") == 0 || strcmp(type, "SEQUENCE") == 0)
		type = "TABLE";

	/* objects named by a schema and name */
	if (strcmp(type, "CONVERSION") == 0 ||
		strcmp(type, "DOMAIN") == 0 ||
		strcmp(type, "TABLE") == 0 ||
		strcmp(type, "TYPE") == 0 ||
		strcmp(type, "TEXT SEARCH DICTIONARY") == 0 ||
		strcmp(type, "TEXT SEARCH CONFIGURATION") == 0)
	{
		appendPQExpBuffer(buf, "%s ", type);
		if (te->namespace && te->namespace[0])	/* is null pre-7.3 */
			appendPQExpBuffer(buf, "%s.", fmtId(te->namespace));

		/*
		 * Pre-7.3 pg_dump would sometimes (not always) put a fmtId'd name
		 * into te->tag for an index. This check is heuristic, so make its
		 * scope as narrow as possible.
		 */
		if (AH->version < K_VERS_1_7 &&
			te->tag[0] == '"' &&
			te->tag[strlen(te->tag) - 1] == '"' &&
			strcmp(type, "INDEX") == 0)
			appendPQExpBuffer(buf, "%s", te->tag);
		else
			appendPQExpBuffer(buf, "%s", fmtId(te->tag));
		return;
	}

	/* objects named by just a name */
	if (strcmp(type, "DATABASE") == 0 ||
		strcmp(type, "PROCEDURAL LANGUAGE") == 0 ||
		strcmp(type, "SCHEMA") == 0 ||
		strcmp(type, "FOREIGN DATA WRAPPER") == 0 ||
		strcmp(type, "SERVER") == 0 ||
		strcmp(type, "USER MAPPING") == 0)
	{
		appendPQExpBuffer(buf, "%s %s", type, fmtId(te->tag));
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
		char	   *first = strdup(te->dropStmt + 5);
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
_printTocEntry(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt, bool isData, bool acl_pass)
{
	/* ACLs are dumped only during acl pass */
	if (acl_pass)
	{
		if (strcmp(te->desc, "ACL") != 0)
			return;
	}
	else
	{
		if (strcmp(te->desc, "ACL") == 0)
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
			sanitized_schema = strdup("-");
		if (!ropt->noOwner)
			sanitized_owner = replace_line_endings(te->owner);
		else
			sanitized_owner = strdup("-");

		ahprintf(AH, "-- %sName: %s; Type: %s; Schema: %s; Owner: %s",
				 pfx, sanitized_name, te->desc, sanitized_schema,
				 sanitized_owner);

		free(sanitized_name);
		free(sanitized_schema);
		free(sanitized_owner);

		if (te->tablespace && !ropt->noTablespace)
		{
			char   *sanitized_tablespace;

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
			strcmp(te->desc, "SEQUENCE") == 0 ||
			strcmp(te->desc, "TEXT SEARCH DICTIONARY") == 0 ||
			strcmp(te->desc, "TEXT SEARCH CONFIGURATION") == 0 ||
			strcmp(te->desc, "FOREIGN DATA WRAPPER") == 0 ||
			strcmp(te->desc, "SERVER") == 0)
		{
			PQExpBuffer temp = createPQExpBuffer();

			appendPQExpBuffer(temp, "ALTER ");
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
	if (strncmp(te->desc, "ACL", 3) == 0)
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
	char   *result;
	char   *s;

	result = strdup(str);

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
	 * NB: this code must agree with _discoverArchiveFormat().  Maybe find
	 * a way to unify the cases?
	 */
	if (!AH->readHeader)
	{
		if ((*AH->ReadBufPtr) (AH, tmpMag, 5) != 5)
			die_horribly(AH, modulename, "unexpected end of file\n");

		if (strncmp(tmpMag, "PGDMP", 5) != 0)
			die_horribly(AH, modulename, "did not find magic string in file header\n");

		AH->vmaj = (*AH->ReadBytePtr) (AH);
		AH->vmin = (*AH->ReadBytePtr) (AH);

		if (AH->vmaj > 1 || ((AH->vmaj == 1) && (AH->vmin > 0)))		/* Version > 1.0 */
			AH->vrev = (*AH->ReadBytePtr) (AH);
		else
			AH->vrev = 0;

		AH->version = ((AH->vmaj * 256 + AH->vmin) * 256 + AH->vrev) * 256 + 0;

		if (AH->version < K_VERS_1_0 || AH->version > K_VERS_MAX)
			die_horribly(AH, modulename, "unsupported version (%d.%d) in file header\n",
						 AH->vmaj, AH->vmin);

		AH->intSize = (*AH->ReadBytePtr) (AH);
		if (AH->intSize > 32)
			die_horribly(AH, modulename, "sanity check on integer size (%lu) failed\n",
						 (unsigned long) AH->intSize);

		if (AH->intSize > sizeof(int))
			write_msg(modulename, "WARNING: archive was made on a machine with larger integers, some operations might fail\n");

		if (AH->version >= K_VERS_1_7)
			AH->offSize = (*AH->ReadBytePtr) (AH);
		else
			AH->offSize = AH->intSize;

		fmt = (*AH->ReadBytePtr) (AH);

		if (AH->format != fmt)
			die_horribly(AH, modulename, "expected format (%d) differs from format found in file (%d)\n",
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
	 * If pgoff_t is wider than long, we must have "real" fseeko and not
	 * an emulation using fseek.  Otherwise report no seek capability.
	 */
#ifndef HAVE_FSEEKO
	if (sizeof(pgoff_t) > sizeof(long))
		return false;
#endif

	/* Check that ftello works on this file */
	errno = 0;
	tpos = ftello(fp);
	if (errno)
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
	char		buf[256];

	/*
	 * We don't print the timezone on Win32, because the names are long and
	 * localized, which means they may contain characters in various random
	 * encodings; this has been seen to cause encoding errors when reading the
	 * dump script.
	 */
	if (strftime(buf, sizeof(buf),
#ifndef WIN32
				 "%Y-%m-%d %H:%M:%S %Z",
#else
				 "%Y-%m-%d %H:%M:%S",
#endif
				 localtime(&tim)) != 0)
		ahprintf(AH, "-- %s %s\n\n", msg, buf);
}


/*
 * Main engine for parallel restore.
 *
 * Work is done in three phases.
 * First we process tocEntries until we come to one that is marked
 * SECTION_DATA or SECTION_POST_DATA, in a single connection, just as for a
 * standard restore.  Second we process the remaining non-ACL steps in
 * parallel worker children (threads on Windows, processes on Unix), each of
 * which connects separately to the database.  Finally we process all the ACL
 * entries in a single connection (that happens back in RestoreArchive).
 */
static void
restore_toc_entries_parallel(ArchiveHandle *AH)
{
	RestoreOptions *ropt = AH->ropt;
	int			n_slots = ropt->number_of_jobs;
	ParallelSlot *slots;
	int			work_status;
	int			next_slot;
	TocEntry   *first_unprocessed = AH->toc->next;
	TocEntry   *next_work_item;
	thandle		ret_child;
	TocEntry   *te;

	ahlog(AH, 2, "entering restore_toc_entries_parallel\n");

	slots = (ParallelSlot *) calloc(sizeof(ParallelSlot), n_slots);

	/* Adjust dependency information */
	fix_dependencies(AH);

	/*
	 * Do all the early stuff in a single connection in the parent. There's no
	 * great point in running it in parallel, in fact it will actually run
	 * faster in a single connection because we avoid all the connection and
	 * setup overhead.
	 */
	while ((next_work_item = get_next_work_item(AH, &first_unprocessed,
												NULL, 0)) != NULL)
	{
		if (next_work_item->section == SECTION_DATA ||
			next_work_item->section == SECTION_POST_DATA)
		{
			teReqs		reqs;

			/* If not to be dumped, we can stay in this loop */
			reqs = _tocEntryRequired(next_work_item, AH->ropt, false);
			if ((reqs & (REQ_SCHEMA | REQ_DATA)) != 0)
				break;
		}

		ahlog(AH, 1, "processing item %d %s %s\n",
			  next_work_item->dumpId,
			  next_work_item->desc, next_work_item->tag);

		(void) restore_toc_entry(AH, next_work_item, ropt, false);

		next_work_item->restored = true;
		reduce_dependencies(AH, next_work_item);
	}

	/*
	 * Now close parent connection in prep for parallel steps.  We do this
	 * mainly to ensure that we don't exceed the specified number of parallel
	 * connections.
	 */
	PQfinish(AH->connection);
	AH->connection = NULL;

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

	/*
	 * main parent loop
	 *
	 * Keep going until there is no worker still running AND there is no work
	 * left to be done.
	 */

	ahlog(AH, 1, "entering main parallel loop\n");

	while ((next_work_item = get_next_work_item(AH, &first_unprocessed,
												slots, n_slots)) != NULL ||
		   work_in_progress(slots, n_slots))
	{
		if (next_work_item != NULL)
		{
			teReqs		reqs;

			/* If not to be dumped, don't waste time launching a worker */
			reqs = _tocEntryRequired(next_work_item, AH->ropt, false);
			if ((reqs & (REQ_SCHEMA | REQ_DATA)) == 0)
			{
				ahlog(AH, 1, "skipping item %d %s %s\n",
					  next_work_item->dumpId,
					  next_work_item->desc, next_work_item->tag);

				next_work_item->restored = true;
				reduce_dependencies(AH, next_work_item);

				continue;
			}

			if ((next_slot = get_next_slot(slots, n_slots)) != NO_SLOT)
			{
				/* There is work still to do and a worker slot available */
				thandle		child;
				RestoreArgs *args;

				ahlog(AH, 1, "launching item %d %s %s\n",
					  next_work_item->dumpId,
					  next_work_item->desc, next_work_item->tag);

				next_work_item->restored = true;

				/* this memory is dealloced in mark_work_done() */
				args = malloc(sizeof(RestoreArgs));
				args->AH = CloneArchive(AH);
				args->te = next_work_item;

				/* run the step in a worker child */
				child = spawn_restore(args);

				slots[next_slot].child_id = child;
				slots[next_slot].args = args;

				continue;
			}
		}

		/*
		 * If we get here there must be work being done.  Either there is no
		 * work available to schedule (and work_in_progress returned true) or
		 * there are no slots available.  So we wait for a worker to finish,
		 * and process the result.
		 */
		ret_child = reap_child(slots, n_slots, &work_status);

		if (WIFEXITED(work_status))
		{
			mark_work_done(AH, ret_child, WEXITSTATUS(work_status),
						   slots, n_slots);
		}
		else
		{
			die_horribly(AH, modulename, "worker process crashed: status %d\n",
						 work_status);
		}
	}

	ahlog(AH, 1, "finished main parallel loop\n");

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
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (!te->restored)
		{
			ahlog(AH, 1, "processing missed item %d %s %s\n",
				  te->dumpId, te->desc, te->tag);
			(void) restore_toc_entry(AH, te, ropt, false);
		}
	}

	/* The ACLs will be handled back in RestoreArchive. */
}

/*
 * create a worker child to perform a restore step in parallel
 */
static thandle
spawn_restore(RestoreArgs *args)
{
	thandle		child;

	/* Ensure stdio state is quiesced before forking */
	fflush(NULL);

#ifndef WIN32
	child = fork();
	if (child == 0)
	{
		/* in child process */
		parallel_restore(args);
		die_horribly(args->AH, modulename,
					 "parallel_restore should not return\n");
	}
	else if (child < 0)
	{
		/* fork failed */
		die_horribly(args->AH, modulename,
					 "could not create worker process: %s\n",
					 strerror(errno));
	}
#else
	child = (HANDLE) _beginthreadex(NULL, 0, (void *) parallel_restore,
									args, 0, NULL);
	if (child == 0)
		die_horribly(args->AH, modulename,
					 "could not create worker thread: %s\n",
					 strerror(errno));
#endif

	return child;
}

/*
 *	collect status from a completed worker child
 */
static thandle
reap_child(ParallelSlot *slots, int n_slots, int *work_status)
{
#ifndef WIN32
	/* Unix is so much easier ... */
	return wait(work_status);
#else
	static HANDLE *handles = NULL;
	int			hindex,
				snum,
				tnum;
	thandle		ret_child;
	DWORD		res;

	/* first time around only, make space for handles to listen on */
	if (handles == NULL)
		handles = (HANDLE *) calloc(sizeof(HANDLE), n_slots);

	/* set up list of handles to listen to */
	for (snum = 0, tnum = 0; snum < n_slots; snum++)
		if (slots[snum].child_id != 0)
			handles[tnum++] = slots[snum].child_id;

	/* wait for one to finish */
	hindex = WaitForMultipleObjects(tnum, handles, false, INFINITE);

	/* get handle of finished thread */
	ret_child = handles[hindex - WAIT_OBJECT_0];

	/* get the result */
	GetExitCodeThread(ret_child, &res);
	*work_status = res;

	/* dispose of handle to stop leaks */
	CloseHandle(ret_child);

	return ret_child;
#endif
}

/*
 * are we doing anything now?
 */
static bool
work_in_progress(ParallelSlot *slots, int n_slots)
{
	int			i;

	for (i = 0; i < n_slots; i++)
	{
		if (slots[i].child_id != 0)
			return true;
	}
	return false;
}

/*
 * find the first free parallel slot (if any).
 */
static int
get_next_slot(ParallelSlot *slots, int n_slots)
{
	int			i;

	for (i = 0; i < n_slots; i++)
	{
		if (slots[i].child_id == 0)
			return i;
	}
	return NO_SLOT;
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
 * Find the next work item (if any) that is capable of being run now.
 *
 * To qualify, the item must have no remaining dependencies
 * and no requirement for locks that is incompatible with
 * items currently running.
 *
 * first_unprocessed is state data that tracks the location of the first
 * TocEntry that's not marked 'restored'.  This avoids O(N^2) search time
 * with long TOC lists.  (Even though the constant is pretty small, it'd
 * get us eventually.)
 *
 * pref_non_data is for an alternative selection algorithm that gives
 * preference to non-data items if there is already a data load running.
 * It is currently disabled.
 */
static TocEntry *
get_next_work_item(ArchiveHandle *AH, TocEntry **first_unprocessed,
				   ParallelSlot *slots, int n_slots)
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

		for (k = 0; k < n_slots; k++)
			if (slots[k].args->te != NULL &&
				slots[k].args->te->section == SECTION_DATA)
				count++;
		if (n_slots == 0 || count * 4 < n_slots)
			pref_non_data = false;
	}

	/*
	 * Advance first_unprocessed if possible.
	 */
	for (te = *first_unprocessed; te != AH->toc; te = te->next)
	{
		if (!te->restored)
			break;
	}
	*first_unprocessed = te;

	/*
	 * Search from first_unprocessed until we find an available item.
	 */
	for (; te != AH->toc; te = te->next)
	{
		bool		conflicts = false;

		/* Ignore if already done or still waiting on dependencies */
		if (te->restored || te->depCount > 0)
			continue;

		/*
		 * Check to see if the item would need exclusive lock on something
		 * that a currently running item also needs lock on, or vice versa. If
		 * so, we don't want to schedule them together.
		 */
		for (i = 0; i < n_slots && !conflicts; i++)
		{
			TocEntry   *running_te;

			if (slots[i].args == NULL)
				continue;
			running_te = slots[i].args->te;

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
 * this is the procedure run as a thread (Windows) or a
 * separate process (everything else).
 */
static parallel_restore_result
parallel_restore(RestoreArgs *args)
{
	ArchiveHandle *AH = args->AH;
	TocEntry   *te = args->te;
	RestoreOptions *ropt = AH->ropt;
	int			retval;

	/*
	 * Close and reopen the input file so we have a private file pointer that
	 * doesn't stomp on anyone else's file pointer, if we're actually going to
	 * need to read from the file. Otherwise, just close it except on Windows,
	 * where it will possibly be needed by other threads.
	 *
	 * Note: on Windows, since we are using threads not processes, the reopen
	 * call *doesn't* close the original file pointer but just open a new one.
	 */
	if (te->section == SECTION_DATA)
		(AH->ReopenPtr) (AH);
#ifndef WIN32
	else
		(AH->ClosePtr) (AH);
#endif

	/*
	 * We need our own database connection, too
	 */
	ConnectDatabase((Archive *) AH, ropt->dbname,
					ropt->pghost, ropt->pgport, ropt->username,
					ropt->promptPassword);

	_doSetFixedOutputState(AH);

	/* Restore the TOC item */
	retval = restore_toc_entry(AH, te, ropt, true);

	/* And clean up */
	PQfinish(AH->connection);
	AH->connection = NULL;

	/* If we reopened the file, we are done with it, so close it now */
	if (te->section == SECTION_DATA)
		(AH->ClosePtr) (AH);

	if (retval == 0 && AH->public.n_errors)
		retval = WORKER_IGNORED_ERRORS;

#ifndef WIN32
	exit(retval);
#else
	return retval;
#endif
}


/*
 * Housekeeping to be done after a step has been parallel restored.
 *
 * Clear the appropriate slot, free all the extra memory we allocated,
 * update status, and reduce the dependency count of any dependent items.
 */
static void
mark_work_done(ArchiveHandle *AH, thandle worker, int status,
			   ParallelSlot *slots, int n_slots)
{
	TocEntry   *te = NULL;
	int			i;

	for (i = 0; i < n_slots; i++)
	{
		if (slots[i].child_id == worker)
		{
			slots[i].child_id = 0;
			te = slots[i].args->te;
			DeCloneArchive(slots[i].args->AH);
			free(slots[i].args);
			slots[i].args = NULL;

			break;
		}
	}

	if (te == NULL)
		die_horribly(AH, modulename, "could not find slot of finished worker\n");

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
		die_horribly(AH, modulename, "worker process failed: exit code %d\n",
					 status);

	reduce_dependencies(AH, te);
}


/*
 * Process the dependency information into a form useful for parallel restore.
 *
 * We set up depCount fields that are the number of as-yet-unprocessed
 * dependencies for each TOC entry.
 *
 * We also identify locking dependencies so that we can avoid trying to
 * schedule conflicting items at the same time.
 */
static void
fix_dependencies(ArchiveHandle *AH)
{
	TocEntry  **tocsByDumpId;
	TocEntry   *te;
	DumpId		maxDumpId;
	int			i;

	/*
	 * For some of the steps here, it is convenient to have an array that
	 * indexes the TOC entries by dump ID, rather than searching the TOC list
	 * repeatedly.  Entries for dump IDs not present in the TOC will be NULL.
	 *
	 * NOTE: because maxDumpId is just the highest dump ID defined in the
	 * archive, there might be dependencies for IDs > maxDumpId.  All uses
	 * of this array must guard against out-of-range dependency numbers.
	 *
	 * Also, initialize the depCount fields.
	 */
	maxDumpId = AH->maxDumpId;
	tocsByDumpId = (TocEntry **) calloc(maxDumpId, sizeof(TocEntry *));
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		tocsByDumpId[te->dumpId - 1] = te;
		te->depCount = te->nDeps;
	}

	/*
	 * POST_DATA items that are shown as depending on a table need to be
	 * re-pointed to depend on that table's data, instead.  This ensures they
	 * won't get scheduled until the data has been loaded.  We handle this by
	 * first finding TABLE/TABLE DATA pairs and then scanning all the
	 * dependencies.
	 *
	 * Note: currently, a TABLE DATA should always have exactly one
	 * dependency, on its TABLE item.  So we don't bother to search, but look
	 * just at the first dependency.  We do trouble to make sure that it's a
	 * TABLE, if possible.  However, if the dependency isn't in the archive
	 * then just assume it was a TABLE; this is to cover cases where the table
	 * was suppressed but we have the data and some dependent post-data items.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (strcmp(te->desc, "TABLE DATA") == 0 && te->nDeps > 0)
		{
			DumpId		tableId = te->dependencies[0];

			if (tableId > maxDumpId ||
				tocsByDumpId[tableId - 1] == NULL ||
				strcmp(tocsByDumpId[tableId - 1]->desc, "TABLE") == 0)
			{
				repoint_table_dependencies(AH, tableId, te->dumpId);
			}
		}
	}

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
						te->dependencies = (DumpId *) malloc(sizeof(DumpId));
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
	 * It is possible that the dependencies list items that are not in the
	 * archive at all.  Subtract such items from the depCounts.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		for (i = 0; i < te->nDeps; i++)
		{
			DumpId		depid = te->dependencies[i];

			if (depid > maxDumpId || tocsByDumpId[depid - 1] == NULL)
				te->depCount--;
		}
	}

	/*
	 * Lastly, work out the locking dependencies.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		te->lockDeps = NULL;
		te->nLockDeps = 0;
		identify_locking_dependencies(te, tocsByDumpId, maxDumpId);
	}

	free(tocsByDumpId);
}

/*
 * Change dependencies on tableId to depend on tableDataId instead,
 * but only in POST_DATA items.
 */
static void
repoint_table_dependencies(ArchiveHandle *AH,
						   DumpId tableId, DumpId tableDataId)
{
	TocEntry   *te;
	int			i;

	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		if (te->section != SECTION_POST_DATA)
			continue;
		for (i = 0; i < te->nDeps; i++)
		{
			if (te->dependencies[i] == tableId)
			{
				te->dependencies[i] = tableDataId;
				ahlog(AH, 2, "transferring dependency %d -> %d to %d\n",
					  te->dumpId, tableId, tableDataId);
			}
		}
	}
}

/*
 * Identify which objects we'll need exclusive lock on in order to restore
 * the given TOC entry (*other* than the one identified by the TOC entry
 * itself).  Record their dump IDs in the entry's lockDeps[] array.
 * tocsByDumpId[] is a convenience array (of size maxDumpId) to avoid
 * searching the TOC for each dependency.
 */
static void
identify_locking_dependencies(TocEntry *te,
							  TocEntry **tocsByDumpId,
							  DumpId maxDumpId)
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
	 * We assume the item requires exclusive lock on each TABLE DATA item
	 * listed among its dependencies.  (This was originally a dependency on
	 * the TABLE, but fix_dependencies repointed it to the data item. Note
	 * that all the entry types we are interested in here are POST_DATA, so
	 * they will all have been changed this way.)
	 */
	lockids = (DumpId *) malloc(te->nDeps * sizeof(DumpId));
	nlockids = 0;
	for (i = 0; i < te->nDeps; i++)
	{
		DumpId		depid = te->dependencies[i];

		if (depid <= maxDumpId && tocsByDumpId[depid - 1] &&
			strcmp(tocsByDumpId[depid - 1]->desc, "TABLE DATA") == 0)
			lockids[nlockids++] = depid;
	}

	if (nlockids == 0)
	{
		free(lockids);
		return;
	}

	te->lockDeps = realloc(lockids, nlockids * sizeof(DumpId));
	te->nLockDeps = nlockids;
}

/*
 * Remove the specified TOC entry from the depCounts of items that depend on
 * it, thereby possibly making them ready-to-run.
 */
static void
reduce_dependencies(ArchiveHandle *AH, TocEntry *te)
{
	DumpId		target = te->dumpId;
	int			i;

	ahlog(AH, 2, "reducing dependencies for %d\n", target);

	/*
	 * We must examine all entries, not only the ones after the target item,
	 * because if the user used a -L switch then the original dependency-
	 * respecting order has been destroyed by SortTocFromFile.
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
		for (i = 0; i < te->nDeps; i++)
		{
			if (te->dependencies[i] == target)
				te->depCount--;
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
	TocEntry   *tes;

	for (tes = AH->toc->next; tes != AH->toc; tes = tes->next)
	{
		if (strcmp(tes->desc, "TABLE DATA") == 0 &&
			strcmp(tes->tag, te->tag) == 0 &&
			strcmp(tes->namespace ? tes->namespace : "",
				   te->namespace ? te->namespace : "") == 0)
		{
			tes->created = true;
			break;
		}
	}
}

/*
 * Mark the DATA member corresponding to the given TABLE member
 * as not wanted
 */
static void
inhibit_data_for_failed_table(ArchiveHandle *AH, TocEntry *te)
{
	RestoreOptions *ropt = AH->ropt;
	TocEntry   *tes;

	ahlog(AH, 1, "table \"%s\" could not be created, will not restore its data\n",
		  te->tag);

	for (tes = AH->toc->next; tes != AH->toc; tes = tes->next)
	{
		if (strcmp(tes->desc, "TABLE DATA") == 0 &&
			strcmp(tes->tag, te->tag) == 0 &&
			strcmp(tes->namespace ? tes->namespace : "",
				   te->namespace ? te->namespace : "") == 0)
		{
			/* mark it unwanted; we assume idWanted array already exists */
			ropt->idWanted[tes->dumpId - 1] = false;
			break;
		}
	}
}


/*
 * Clone and de-clone routines used in parallel restoration.
 *
 * Enough of the structure is cloned to ensure that there is no
 * conflict between different threads each with their own clone.
 *
 * These could be public, but no need at present.
 */
static ArchiveHandle *
CloneArchive(ArchiveHandle *AH)
{
	ArchiveHandle *clone;

	/* Make a "flat" copy */
	clone = (ArchiveHandle *) malloc(sizeof(ArchiveHandle));
	if (clone == NULL)
		die_horribly(AH, modulename, "out of memory\n");
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
		clone->savedPassword = strdup(clone->savedPassword);

	/* clone has its own error count, too */
	clone->public.n_errors = 0;

	/* Let the format-specific code have a chance too */
	(clone->ClonePtr) (clone);

	return clone;
}

/*
 * Release clone-local storage.
 *
 * Note: we assume any clone-local connection was already closed.
 */
static void
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
