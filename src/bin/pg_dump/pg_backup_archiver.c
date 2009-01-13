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
 *		$PostgreSQL: pgsql/src/bin/pg_dump/pg_backup_archiver.c,v 1.138.2.3 2009/01/13 11:45:01 mha Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup_db.h"
#include "dumputils.h"

#include <ctype.h>

#include <unistd.h>

#ifdef WIN32
#include <io.h>
#endif

#include "libpq/libpq-fs.h"
#include "mb/pg_wchar.h"


const char *progname;

static char *modulename = gettext_noop("archiver");


static ArchiveHandle *_allocAH(const char *FileSpec, const ArchiveFormat fmt,
		 const int compression, ArchiveMode mode);
static void _getObjectDescription(PQExpBuffer buf, TocEntry *te,
					  ArchiveHandle *AH);
static void _printTocEntry(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt, bool isData, bool acl_pass);


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
static void _moveAfter(ArchiveHandle *AH, TocEntry *pos, TocEntry *te);
static int	_discoverArchiveFormat(ArchiveHandle *AH);

static void dump_lo_buf(ArchiveHandle *AH);
static void _write_msg(const char *modulename, const char *fmt, va_list ap);
static void _die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt, va_list ap);

static void dumpTimestamp(ArchiveHandle *AH, const char *msg, time_t tim);
static OutputContext SetOutput(ArchiveHandle *AH, char *filename, int compression);
static void ResetOutput(ArchiveHandle *AH, OutputContext savedContext);


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
			  const int compression)

{
	ArchiveHandle *AH = _allocAH(FileSpec, fmt, compression, archModeWrite);

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
	TocEntry   *te;
	teReqs		reqs;
	OutputContext sav;
	bool		defnDumped;

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
	 * -1 is not compatible with -C, because we can't create a database
	 *  inside a transaction block.
	 */
	if (ropt->create && ropt->single_txn)
		die_horribly(AH, modulename, "-C and -1 are incompatible options\n");

	/*
	 * If we're using a DB connection, then connect it.
	 */
	if (ropt->useDB)
	{
		ahlog(AH, 1, "connecting to database for restore\n");
		if (AH->version < K_VERS_1_3)
			die_horribly(AH, modulename, "direct database connections are not supported in pre-1.3 archives\n");

		/* XXX Should get this from the archive */
		AHX->minRemoteVersion = 070100;
		AHX->maxRemoteVersion = 999999;

		ConnectDatabase(AHX, ropt->dbname,
						ropt->pghost, ropt->pgport, ropt->username,
						ropt->requirePassword, ropt->ignoreVersion);

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
	}

	/*
	 * Now process each non-ACL TOC entry
	 */
	for (te = AH->toc->next; te != AH->toc; te = te->next)
	{
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

		if ((reqs & REQ_SCHEMA) != 0)	/* We want the schema */
		{
			ahlog(AH, 1, "creating %s %s\n", te->desc, te->tag);

			_printTocEntry(AH, te, ropt, false, false);
			defnDumped = true;

			/*
			 * If we could not create a table and --no-data-for-failed-tables
			 * was given, ignore the corresponding TABLE DATA
			 */
			if (ropt->noDataForFailedTables &&
				AH->lastErrorTE == te &&
				strcmp(te->desc, "TABLE") == 0)
			{
				TocEntry   *tes;

				ahlog(AH, 1, "table \"%s\" could not be created, will not restore its data\n",
					  te->tag);

				for (tes = te->next; tes != AH->toc; tes = tes->next)
				{
					if (strcmp(tes->desc, "TABLE DATA") == 0 &&
						strcmp(tes->tag, te->tag) == 0 &&
						strcmp(tes->namespace ? tes->namespace : "",
							   te->namespace ? te->namespace : "") == 0)
					{
						/* mark it unwanted */
						ropt->idWanted[tes->dumpId - 1] = false;
						break;
					}
				}
			}

			/* If we created a DB, connect to it... */
			if (strcmp(te->desc, "DATABASE") == 0)
			{
				ahlog(AH, 1, "connecting to new database \"%s\"\n", te->tag);
				_reconnectToDB(AH, te->tag);
			}
		}

		/*
		 * If we have a data component, then process it
		 */
		if ((reqs & REQ_DATA) != 0)
		{
			/*
			 * hadDumper will be set if there is genuine data component for
			 * this node. Otherwise, we need to check the defn field for
			 * statements that need to be executed in data-only restores.
			 */
			if (te->hadDumper)
			{
				/*
				 * If we can output the data, then restore it.
				 */
				if (AH->PrintTocDataPtr !=NULL && (reqs & REQ_DATA) != 0)
				{
#ifndef HAVE_LIBZ
					if (AH->compression != 0)
						die_horribly(AH, modulename, "cannot restore from compressed archive (compression not supported in this installation)\n");
#endif

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
						 * If we have a copy statement, use it. As of V1.3,
						 * these are separate to allow easy import from
						 * withing a database connection. Pre 1.3 archives can
						 * not use DB connections and are sent to output only.
						 *
						 * For V1.3+, the table data MUST have a copy
						 * statement so that we can go into appropriate mode
						 * with libpq.
						 */
						if (te->copyStmt && strlen(te->copyStmt) > 0)
						{
							ahprintf(AH, "%s", te->copyStmt);
							AH->writingCopyData = true;
						}

						(*AH->PrintTocDataPtr) (AH, te, ropt);

						AH->writingCopyData = false;

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
	}							/* end loop over TOC entries */

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
 * Allocate a new RestoreOptions block.
 * This is mainly so we can initialize it, but also for future expansion,
 */
RestoreOptions *
NewRestoreOptions(void)
{
	RestoreOptions *opts;

	opts = (RestoreOptions *) calloc(1, sizeof(RestoreOptions));

	opts->format = archUnknown;
	opts->suppressDumpWarnings = false;
	opts->exit_on_error = false;

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
			 const char *desc, const char *defn,
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
	TocEntry   *te = AH->toc->next;
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

	while (te != AH->toc)
	{
		if (_tocEntryRequired(te, ropt, true) != 0)
			ahprintf(AH, "%d; %u %u %s %s %s %s\n", te->dumpId,
					 te->catalogId.tableoid, te->catalogId.oid,
					 te->desc, te->namespace ? te->namespace : "-",
					 te->tag, te->owner);
		te = te->next;
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

	ahlog(AH, 1, "restored %d large objects\n", AH->blobCount);
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

	ahlog(AH, 2, "restoring large object with OID %u\n", oid);

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
	char		buf[1024];
	char	   *cmnt;
	char	   *endptr;
	DumpId		id;
	TocEntry   *te;
	TocEntry   *tePrev;

	/* Allocate space for the 'wanted' array, and init it */
	ropt->idWanted = (bool *) malloc(sizeof(bool) * AH->maxDumpId);
	memset(ropt->idWanted, 0, sizeof(bool) * AH->maxDumpId);

	/* Set prev entry as head of list */
	tePrev = AH->toc;

	/* Setup the file */
	fh = fopen(ropt->tocFile, PG_BINARY_R);
	if (!fh)
		die_horribly(AH, modulename, "could not open TOC file: %s\n",
					 strerror(errno));

	while (fgets(buf, sizeof(buf), fh) != NULL)
	{
		/* Truncate line at comment, if any */
		cmnt = strchr(buf, ';');
		if (cmnt != NULL)
			cmnt[0] = '\0';

		/* Ignore if all blank */
		if (strspn(buf, " \t\r") == strlen(buf))
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

		ropt->idWanted[id - 1] = true;

		_moveAfter(AH, tePrev, te);
		tePrev = te;
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
		if (fn >= 0)
			AH->OF = fdopen(dup(fn), PG_BINARY_W);
		else
			AH->OF = fopen(filename, PG_BINARY_W);
		AH->gzOut = 0;
	}

	if (!AH->OF)
		die_horribly(AH, modulename, "could not open output file: %s\n", strerror(errno));

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
	int			bSize = strlen(fmt) + 256;		/* Should be enough */
	int			cnt = -1;

	/*
	 * This is paranoid: deal with the possibility that vsnprintf is willing
	 * to ignore trailing null
	 */

	/*
	 * or returns > 0 even if string does not fit. It may be the case that it
	 * returns cnt = bufsize
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
		ahlog(AH, 5, "wrote %lu bytes of large object data (result = %lu)\n",
			  (unsigned long) AH->lo_buf_used, (unsigned long) res);
		if (res != AH->lo_buf_used)
			die_horribly(AH, modulename,
			"could not write to large object (result: %lu, expected: %lu)\n",
					   (unsigned long) res, (unsigned long) AH->lo_buf_used);
	}
	else
	{
		unsigned char *str;
		size_t		len;

		str = PQescapeBytea((const unsigned char *) AH->lo_buf,
							AH->lo_buf_used, &len);
		if (!str)
			die_horribly(AH, modulename, "out of memory\n");

		/* Hack: turn off writingBlob so ahwrite doesn't recurse to here */
		AH->writingBlob = 0;
		ahprintf(AH, "SELECT lowrite(0, '%s');\n", str);
		AH->writingBlob = 1;

		free(str);
	}
	AH->lo_buf_used = 0;
}


/*
 *	Write buffer to the output file (usually stdout). This is user for
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
			return ExecuteSqlCommandBuf(AH, (void *) ptr, size * nmemb);		/* Always 1, currently */
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

static void
_moveAfter(ArchiveHandle *AH, TocEntry *pos, TocEntry *te)
{
	te->prev->next = te->next;
	te->next->prev = te->prev;

	te->prev = pos;
	te->next = pos->next;

	pos->next->prev = te;
	pos->next = te;
}

#ifdef NOT_USED

static void
_moveBefore(ArchiveHandle *AH, TocEntry *pos, TocEntry *te)
{
	te->prev->next = te->next;
	te->next->prev = te->prev;

	te->prev = pos->prev;
	te->next = pos;
	pos->prev->next = te;
	pos->prev = te;
}
#endif

static TocEntry *
getTocEntryByDumpId(ArchiveHandle *AH, DumpId id)
{
	TocEntry   *te;

	te = AH->toc->next;
	while (te != AH->toc)
	{
		if (te->dumpId == id)
			return te;
		te = te->next;
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
ReadOffset(ArchiveHandle *AH, pgoff_t *o)
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
	}
	else
		fh = stdin;

	if (!fh)
		die_horribly(AH, modulename, "could not open input file: %s\n", strerror(errno));

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

#if 0
	write_msg(modulename, "read %lu bytes into lookahead buffer\n",
			  (unsigned long) AH->lookaheadLen);
#endif

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
	AH->public.encoding = PG_SQL_ASCII;
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

	AH->currUser = strdup("");	/* So it's valid, but we can free() it later
								 * if necessary */
	AH->currSchema = strdup("");	/* ditto */
	AH->currWithOids = -1;		/* force SET */

	AH->toc = (TocEntry *) calloc(1, sizeof(TocEntry));
	if (!AH->toc)
		die_horribly(AH, modulename, "out of memory\n");

	AH->toc->next = AH->toc;
	AH->toc->prev = AH->toc;

	AH->mode = mode;
	AH->compression = compression;

	AH->pgCopyBuf = createPQExpBuffer();
	AH->sqlBuf = createPQExpBuffer();

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

#if 0
	write_msg(modulename, "archive format is %d\n", fmt);
#endif

	if (fmt == archUnknown)
		AH->format = _discoverArchiveFormat(AH);
	else
		AH->format = fmt;

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
	TocEntry   *te = AH->toc->next;
	StartDataPtr startPtr;
	EndDataPtr	endPtr;

	while (te != AH->toc)
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
		te = te->next;
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

	TocEntry   *te = AH->toc->next;

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
	/* Select the correct character set encoding */
	ahprintf(AH, "SET client_encoding = '%s';\n",
			 pg_encoding_to_char(AH->public.encoding));

	/* Select the correct string literal syntax */
	ahprintf(AH, "SET standard_conforming_strings = %s;\n",
			 AH->public.std_strings ? "on" : "off");

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
	 * SQL requires a string literal here.	Might as well be correct.
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

	AH->currUser = strdup("");

	/* don't assume we still know the output schema */
	if (AH->currSchema)
		free(AH->currSchema);
	AH->currSchema = strdup("");
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
 * Become the owner of the the given TOC entry object.	If
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
		strcmp(type, "TYPE") == 0)
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
		strcmp(type, "SCHEMA") == 0)
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
		strcmp(type, "OPERATOR CLASS") == 0)
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
	 * we'd better recreate it.
	 */
	if (!ropt->dropSchema &&
		strcmp(te->desc, "SCHEMA") == 0 && strcmp(te->tag, "public") == 0)
		return;

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
		ahprintf(AH, "-- %sName: %s; Type: %s; Schema: %s; Owner: %s",
				 pfx, te->tag, te->desc,
				 te->namespace ? te->namespace : "-",
				 ropt->noOwner ? "-" : te->owner);
		if (te->tablespace)
			ahprintf(AH, "; Tablespace: %s", te->tablespace);
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
			strcmp(te->desc, "SCHEMA") == 0 ||
			strcmp(te->desc, "TABLE") == 0 ||
			strcmp(te->desc, "TYPE") == 0 ||
			strcmp(te->desc, "VIEW") == 0 ||
			strcmp(te->desc, "SEQUENCE") == 0)
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
				 strcmp(te->desc, "PROCEDURAL LANGUAGE") == 0 ||
				 strcmp(te->desc, "RULE") == 0 ||
				 strcmp(te->desc, "TRIGGER") == 0)
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

	/* If we haven't already read the header... */
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
			write_msg(modulename, "WARNING: archive was made on a machine with larger integers, some operations may fail\n");

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
 *	  check to see if fseek can be performed.
 */

bool
checkSeek(FILE *fp)
{

	if (fseeko(fp, 0, SEEK_CUR) != 0)
		return false;
	else if (sizeof(pgoff_t) > sizeof(long))

		/*
		 * At this point, pgoff_t is too large for long, so we return based on
		 * whether an pgoff_t version of fseek is available.
		 */
#ifdef HAVE_FSEEKO
		return true;
#else
		return false;
#endif
	else
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
	 * encodings; this has been seen to cause encoding errors when reading
	 * the dump script.
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
