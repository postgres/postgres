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
 *	result from it's use.
 *
 *
 * IDENTIFICATION
 *		$Header: /cvsroot/pgsql/src/bin/pg_dump/pg_backup_archiver.c,v 1.39 2002/01/18 17:13:50 tgl Exp $
 *
 * Modifications - 28-Jun-2000 - pjw@rhyme.com.au
 *
 *		Initial version.
 *
 * Modifications - 31-Jul-2000 - pjw@rhyme.com.au (1.46, 1.47)
 *		Fixed version number initialization in _allocAH (pg_backup_archiver.c)
 *
 *
 * Modifications - 30-Oct-2000 - pjw@rhyme.com.au
 *		Added {Start,End}RestoreBlobs to allow extended TX during BLOB restore.
 *
 * Modifications - 04-Jan-2001 - pjw@rhyme.com.au
 *	  - strdup() the current user just in case it's deallocated from it's TOC
 *		entry. Should *never* happen, but that's what they said about the
 *		Titanic...
 *
 *	  - Check results of IO routines more carefully.
 *
 * Modifications - 27-Jan-2001 - pjw@rhyme.com.au
 *	  - When dropping the schema, reconnect as owner of each object.
 *
 * Modifications - 6-Mar-2001 - pjw@rhyme.com.au
 *	  - Only disable triggers in DataOnly (or implied data-only) restores.
 *
 * Modifications - 31-Mar-2001 - pjw@rhyme.com.au
 *
 *	  - Rudimentary support for dependencies in archives. Current implementation
 *		uses dependencies to modify the OID used in sorting TOC entries.
 *		This will NOT handle multi-level dependencies, but will manage simple
 *		relationships like UDTs & their functions.
 *
 *	  - Treat OIDs with more respect (avoid using ints, use macros for
 *		conversion & comparison).
 *
 * Modifications - 10-May-2001 - pjw@rhyme.com.au
 *	  - Treat SEQUENCE SET TOC entries as data entries rather than schema
 *		entries.
 *	  - Make allowance for data entries that did not have a data dumper
 *		routine (eg. SEQUENCE SET)
 *
 * Modifications - 01-Nov-2001 - pjw@rhyme.com.au
 *	  - Fix handling of {data/schema}-only restores when using a full
 *		backup file; prior version was restoring schema in data-only
 *		restores. Added enum to make code easier to understand.
 *
 * Modifications - 18-Jan-2002 - pjw@rhyme.com.au
 *	  - Modified _tocEntryRequired to handle '<Init>/Max OID' as a special
 * 		case (ie. as a DATA item) as per bugs reported by Bruce Momjian
 *		around 17-Jan-2002.
 *
 *-------------------------------------------------------------------------
 */

#include "pg_backup.h"
#include "pg_backup_archiver.h"
#include "pg_backup_db.h"

#include <errno.h>
#include <unistd.h>				/* for dup */

#include "pqexpbuffer.h"
#include "libpq/libpq-fs.h"

typedef enum _teReqs_
{
	REQ_SCHEMA = 1,
	REQ_DATA = 2,
	REQ_ALL = REQ_SCHEMA + REQ_DATA
} teReqs;

static void _SortToc(ArchiveHandle *AH, TocSortCompareFn fn);
static int	_tocSortCompareByOIDNum(const void *p1, const void *p2);
static int	_tocSortCompareByIDNum(const void *p1, const void *p2);
static ArchiveHandle *_allocAH(const char *FileSpec, const ArchiveFormat fmt,
		 const int compression, ArchiveMode mode);
static int	_printTocEntry(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt, bool isData);

static void _reconnectAsOwner(ArchiveHandle *AH, const char *dbname, TocEntry *te);
static void _reconnectAsUser(ArchiveHandle *AH, const char *dbname, const char *user);

static teReqs _tocEntryRequired(TocEntry *te, RestoreOptions *ropt);
static void _disableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);
static void _enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt);
static TocEntry *_getTocEntry(ArchiveHandle *AH, int id);
static void _moveAfter(ArchiveHandle *AH, TocEntry *pos, TocEntry *te);
static void _moveBefore(ArchiveHandle *AH, TocEntry *pos, TocEntry *te);
static int	_discoverArchiveFormat(ArchiveHandle *AH);
static void _fixupOidInfo(TocEntry *te);
static Oid	_findMaxOID(const char *((*deps)[]));

const char *progname;
static char *modulename = gettext_noop("archiver");

static void _write_msg(const char *modulename, const char *fmt, va_list ap);
static void _die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt, va_list ap);

static int	_canRestoreBlobs(ArchiveHandle *AH);
static int	_restoringToDB(ArchiveHandle *AH);

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
		die_horribly(AH, modulename, "could not close the output file in CloseArchive\n");
}

/* Public */
void
RestoreArchive(Archive *AHX, RestoreOptions *ropt)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	TocEntry   *te = AH->toc->next;
	teReqs		reqs;
	OutputContext sav;
	int			impliedDataOnly;
	bool		defnDumped;

	AH->ropt = ropt;

	/*
	 * Check for nonsensical option combinations.
	 *
	 * NB: create+dropSchema is useless because if you're creating the DB,
	 * there's no need to drop individual items in it.  Moreover, if we
	 * tried to do that then we'd issue the drops in the database
	 * initially connected to, not the one we will create, which is very
	 * bad...
	 */
	if (ropt->create && ropt->noReconnect)
		die_horribly(AH, modulename, "-C and -R are incompatible options\n");

	if (ropt->create && ropt->dropSchema)
		die_horribly(AH, modulename, "-C and -c are incompatible options\n");

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

		ConnectDatabase(AHX, ropt->dbname, ropt->pghost, ropt->pgport, ropt->username,
						ropt->requirePassword, ropt->ignoreVersion);

		/*
		 * If no superuser was specified then see if the current user will
		 * do...
		 */
		if (!ropt->superuser)
		{
			if (UserIsSuperuser(AH, ConnectedUser(AH)))
				ropt->superuser = strdup(ConnectedUser(AH));
		}

	}

	/*
	 * Work out if we have an implied data-only retore. This can happen if
	 * the dump was data only or if the user has used a toc list to
	 * exclude all of the schema data. All we do is look for schema
	 * entries - if none are found then we set the dataOnly flag.
	 *
	 * We could scan for wanted TABLE entries, but that is not the same as
	 * dataOnly. At this stage, it seems unnecessary (6-Mar-2001).
	 */
	if (!ropt->dataOnly)
	{
		te = AH->toc->next;
		impliedDataOnly = 1;
		while (te != AH->toc)
		{
			reqs = _tocEntryRequired(te, ropt);
			if ((reqs & REQ_SCHEMA) != 0)
			{					/* It's schema, and it's wanted */
				impliedDataOnly = 0;
				break;
			}
			te = te->next;
		}
		if (impliedDataOnly)
		{
			ropt->dataOnly = impliedDataOnly;
			ahlog(AH, 1, "implied data-only restore\n");
		}
	}

	if (!ropt->superuser)
		write_msg(modulename, "WARNING:\n"
				  "  Data restoration may fail because existing triggers cannot be disabled\n"
				  "  (no superuser user name specified).  This is only a problem when\n"
		"  restoring into a database with already existing triggers.\n");

	/*
	 * Setup the output file if necessary.
	 */
	if (ropt->filename || ropt->compression)
		sav = SetOutput(AH, ropt->filename, ropt->compression);

	ahprintf(AH, "--\n-- Selected TOC Entries:\n--\n");

	/*
	 * Drop the items at the start, in reverse order
	 */
	if (ropt->dropSchema)
	{
		te = AH->toc->prev;
		while (te != AH->toc)
		{
			reqs = _tocEntryRequired(te, ropt);
			if (((reqs & REQ_SCHEMA) != 0) && te->dropStmt)
			{
				/* We want the schema */
				ahlog(AH, 1, "dropping %s %s\n", te->desc, te->name);
				/* Reconnect if necessary */
				_reconnectAsOwner(AH, NULL, te);
				/* Drop it */
				ahprintf(AH, "%s", te->dropStmt);
			}
			te = te->prev;
		}
	}

	/*
	 * Now process each TOC entry
	 */
	te = AH->toc->next;
	while (te != AH->toc)
	{

		/* Work out what, if anything, we want from this entry */
		reqs = _tocEntryRequired(te, ropt);

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
			/* Reconnect if necessary */
			_reconnectAsOwner(AH, NULL, te);

			ahlog(AH, 1, "creating %s %s\n", te->desc, te->name);
			_printTocEntry(AH, te, ropt, false);
			defnDumped = true;

			/* If we created a DB, connect to it... */
			if (strcmp(te->desc, "DATABASE") == 0)
			{
				ahlog(AH, 1, "connecting to new database %s as user %s\n", te->name, te->owner);
				_reconnectAsUser(AH, te->name, te->owner);
			}
		}

		/*
		 * If we have a data component, then process it
		 */
		if ((reqs & REQ_DATA) != 0)
		{
			/*
			 * hadDumper will be set if there is genuine data component
			 * for this node. Otherwise, we need to check the defn field
			 * for statements that need to be executed in data-only
			 * restores.
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
						die_horribly(AH, modulename, "unable to restore from compressed archive (not configured for compression support)\n");
#endif

					_printTocEntry(AH, te, ropt, true);

					/*
					 * Maybe we can't do BLOBS, so check if this node is
					 * for BLOBS
					 */
					if ((strcmp(te->desc, "BLOBS") == 0) && !_canRestoreBlobs(AH))
					{
						ahprintf(AH, "--\n-- SKIPPED \n--\n\n");

						/*
						 * This is a bit nasty - we assume, for the
						 * moment, that if a custom output is used, then
						 * we don't want warnings.
						 */
						if (!AH->CustomOutPtr)
							write_msg(modulename, "WARNING: skipping large object restoration\n");

					}
					else
					{

						_disableTriggersIfNecessary(AH, te, ropt);

						/*
						 * Reconnect if necessary (_disableTriggers may
						 * have reconnected)
						 */
						_reconnectAsOwner(AH, NULL, te);

						ahlog(AH, 1, "restoring data for table %s\n", te->name);

						/*
						 * If we have a copy statement, use it. As of
						 * V1.3, these are separate to allow easy import
						 * from withing a database connection. Pre 1.3
						 * archives can not use DB connections and are
						 * sent to output only.
						 *
						 * For V1.3+, the table data MUST have a copy
						 * statement so that we can go into appropriate
						 * mode with libpq.
						 */
						if (te->copyStmt && strlen(te->copyStmt) > 0)
							ahprintf(AH, te->copyStmt);

						(*AH->PrintTocDataPtr) (AH, te, ropt);

						_enableTriggersIfNecessary(AH, te, ropt);
					}
				}
			}
			else if (!defnDumped)
			{
				/* If we haven't already dumped the defn part, do so now */
				ahlog(AH, 1, "executing %s %s\n", te->desc, te->name);
				_printTocEntry(AH, te, ropt, false);
			}
		}
		te = te->next;
	}

	/*
	 * Now use blobs_xref (if used) to fixup any refs for tables that we
	 * loaded
	 */
	if (_canRestoreBlobs(AH) && AH->createdBlobXref)
	{
		/* NULL parameter means disable ALL user triggers */
		_disableTriggersIfNecessary(AH, NULL, ropt);

		te = AH->toc->next;
		while (te != AH->toc)
		{

			/* Is it table data? */
			if (strcmp(te->desc, "TABLE DATA") == 0)
			{

				ahlog(AH, 2, "checking whether we loaded %s\n", te->name);

				reqs = _tocEntryRequired(te, ropt);

				if ((reqs & REQ_DATA) != 0)		/* We loaded the data */
				{
					ahlog(AH, 1, "fixing up large object cross-reference for %s\n", te->name);
					FixupBlobRefs(AH, te->name);
				}
			}
			else
				ahlog(AH, 2, "ignoring large object cross-references for %s %s\n", te->desc, te->name);

			te = te->next;
		}

		/* NULL parameter means enable ALL user triggers */
		_enableTriggersIfNecessary(AH, NULL, ropt);
	}

	/*
	 * Clean up & we're done.
	 */
	if (ropt->filename)
		ResetOutput(AH, sav);

	if (ropt->useDB)
	{
		PQfinish(AH->connection);
		AH->connection = NULL;

		if (AH->blobConnection)
		{
			PQfinish(AH->blobConnection);
			AH->blobConnection = NULL;
		}
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

	return opts;
}

/*
 * Returns true if we're restoring directly to the database (and
 * aren't just making a psql script that can do the restoration).
 */
static int
_restoringToDB(ArchiveHandle *AH)
{
	return (AH->ropt->useDB && AH->connection);
}

static int
_canRestoreBlobs(ArchiveHandle *AH)
{
	return _restoringToDB(AH);
}

static void
_disableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt)
{
	char	   *oldUser = NULL;

	/* Can't do much if we're connected & don't have a superuser */
	/* Also, don't bother with triggers unless a data-only retore. */
	if (!ropt->dataOnly || (_restoringToDB(AH) && !ropt->superuser))
		return;

	/*
	 * Reconnect as superuser if possible, since they are the only ones
	 * who can update pg_class...
	 */
	if (ropt->superuser)
	{
		if (!_restoringToDB(AH) || !ConnectedUserIsSuperuser(AH))
		{
			/*
			 * If we're not allowing changes for ownership, then remember
			 * the user so we can change it back here. Otherwise, let
			 * _reconnectAsOwner do what it has to do.
			 */
			if (ropt->noOwner)
				oldUser = strdup(ConnectedUser(AH));
			_reconnectAsUser(AH, NULL, ropt->superuser);
		}
	}

	ahlog(AH, 1, "disabling triggers\n");

	/*
	 * Disable them. This is a hack. Needs to be done via an appropriate
	 * 'SET' command when one is available.
	 */
	ahprintf(AH, "-- Disable triggers\n");

	/*
	 * Just update the AFFECTED table, if known.
	 */

	if (te && te->name && strlen(te->name) > 0)
		ahprintf(AH, "UPDATE \"pg_class\" SET \"reltriggers\" = 0 WHERE \"relname\" = '%s';\n\n",
				 te->name);
	else
		ahprintf(AH, "UPDATE \"pg_class\" SET \"reltriggers\" = 0 WHERE \"relname\" !~ '^pg_';\n\n");

	/*
	 * Restore the user connection from the start of this procedure if
	 * _reconnectAsOwner is disabled.
	 */
	if (ropt->noOwner && oldUser)
	{
		_reconnectAsUser(AH, NULL, oldUser);
		free(oldUser);
	}
}

static void
_enableTriggersIfNecessary(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt)
{
	char	   *oldUser = NULL;

	/* Can't do much if we're connected & don't have a superuser */
	/* Also, don't bother with triggers unless a data-only retore. */
	if (!ropt->dataOnly || (_restoringToDB(AH) && !ropt->superuser))
		return;

	/*
	 * Reconnect as superuser if possible, since they are the only ones
	 * who can update pg_class...
	 */
	if (ropt->superuser)
	{
		if (!_restoringToDB(AH) || !ConnectedUserIsSuperuser(AH))
		{
			/*
			 * If we're not allowing changes for ownership, then remember
			 * the user so we can change it back here. Otherwise, let
			 * _reconnectAsOwner do what it has to do
			 */
			if (ropt->noOwner)
				oldUser = strdup(ConnectedUser(AH));

			_reconnectAsUser(AH, NULL, ropt->superuser);
		}
	}

	ahlog(AH, 1, "enabling triggers\n");

	/*
	 * Enable them. This is a hack. Needs to be done via an appropriate
	 * 'SET' command when one is available.
	 */
	ahprintf(AH, "-- Enable triggers\n");
	if (te && te->name && strlen(te->name) > 0)
	{
		ahprintf(AH, "UPDATE pg_class SET reltriggers = "
		"(SELECT count(*) FROM pg_trigger where pg_class.oid = tgrelid) "
				 "WHERE relname = '%s';\n\n",
				 te->name);
	}
	else
	{
		ahprintf(AH, "UPDATE \"pg_class\" SET \"reltriggers\" = "
		"(SELECT count(*) FROM pg_trigger where pg_class.oid = tgrelid) "
				 "WHERE \"relname\" !~ '^pg_';\n\n");
	}

	/*
	 * Restore the user connection from the start of this procedure if
	 * _reconnectAsOwner is disabled.
	 */
	if (ropt->noOwner && oldUser)
	{
		_reconnectAsUser(AH, NULL, oldUser);
		free(oldUser);
	}
}

/*
 * This is a routine that is part of the dumper interface, hence the 'Archive*' parameter.
 */

/* Public */
int
WriteData(Archive *AHX, const void *data, int dLen)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	if (!AH->currToc)
		die_horribly(AH, modulename, "WriteData cannot be called outside the context of a DataDumper routine\n");

	return (*AH->WriteDataPtr) (AH, data, dLen);
}

/*
 * Create a new TOC entry. The TOC was designed as a TOC, but is now the
 * repository for all metadata. But the name has stuck.
 */

/* Public */
void
ArchiveEntry(Archive *AHX, const char *oid, const char *name,
			 const char *desc, const char *((*deps)[]), const char *defn,
		   const char *dropStmt, const char *copyStmt, const char *owner,
			 DataDumperPtr dumpFn, void *dumpArg)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	TocEntry   *newToc;

	AH->lastID++;
	AH->tocCount++;

	newToc = (TocEntry *) calloc(1, sizeof(TocEntry));
	if (!newToc)
		die_horribly(AH, modulename, "out of memory\n");

	newToc->prev = AH->toc->prev;
	newToc->next = AH->toc;
	AH->toc->prev->next = newToc;
	AH->toc->prev = newToc;

	newToc->id = AH->lastID;

	newToc->name = strdup(name);
	newToc->desc = strdup(desc);

	newToc->oid = strdup(oid);
	newToc->depOid = deps;
	_fixupOidInfo(newToc);


	newToc->defn = strdup(defn);
	newToc->dropStmt = strdup(dropStmt);
	newToc->copyStmt = copyStmt ? strdup(copyStmt) : NULL;
	newToc->owner = strdup(owner);
	newToc->printed = 0;
	newToc->formatData = NULL;
	newToc->dataDumper = dumpFn,
		newToc->dataDumperArg = dumpArg;

	newToc->hadDumper = dumpFn ? 1 : 0;

	if (AH->ArchiveEntryPtr !=NULL)
		(*AH->ArchiveEntryPtr) (AH, newToc);

	/*
	 * printf("New toc owned by '%s', oid %d\n", newToc->owner,
	 * newToc->oidVal);
	 */
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
		sav = SetOutput(AH, ropt->filename, ropt->compression);

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
	ahprintf(AH, ";     Format: %s\n;\n", fmtName);

	ahprintf(AH, ";\n; Selected TOC Entries:\n;\n");

	while (te != AH->toc)
	{
		if (_tocEntryRequired(te, ropt) != 0)
			ahprintf(AH, "%d; %d %s %s %s\n", te->id, te->oidVal, te->desc, te->name, te->owner);
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
		die_horribly(AH, modulename, "large object output not supported in chosen format\n");

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
	AH->blobCount = 0;
}

/*
 * Called by a format handler after all blobs are restored
 */
void
EndRestoreBlobs(ArchiveHandle *AH)
{
	if (AH->txActive)
	{
		ahlog(AH, 2, "committing large object transactions\n");
		CommitTransaction(AH);
	}

	if (AH->blobTxActive)
		CommitTransactionXref(AH);

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

	if (!AH->createdBlobXref)
	{
		if (!AH->connection)
			die_horribly(AH, modulename, "cannot restore large objects without a database connection\n");

		CreateBlobXrefTable(AH);
		AH->createdBlobXref = 1;
	}

	/*
	 * Start long-running TXs if necessary
	 */
	if (!AH->txActive)
	{
		ahlog(AH, 2, "starting large object transactions\n");
		StartTransaction(AH);
	}
	if (!AH->blobTxActive)
		StartTransactionXref(AH);

	loOid = lo_creat(AH->connection, INV_READ | INV_WRITE);
	if (loOid == 0)
		die_horribly(AH, modulename, "could not create large object\n");

	ahlog(AH, 2, "restoring large object with oid %u as %u\n", oid, loOid);

	InsertBlobXref(AH, oid, loOid);

	AH->loFd = lo_open(AH->connection, loOid, INV_WRITE);
	if (AH->loFd == -1)
		die_horribly(AH, modulename, "could not open large object\n");

	AH->writingBlob = 1;
}

void
EndRestoreBlob(ArchiveHandle *AH, Oid oid)
{
	lo_close(AH->connection, AH->loFd);
	AH->writingBlob = 0;

	/*
	 * Commit every BLOB_BATCH_SIZE blobs...
	 */
	if (((AH->blobCount / BLOB_BATCH_SIZE) * BLOB_BATCH_SIZE) == AH->blobCount)
	{
		ahlog(AH, 2, "committing large object transactions\n");
		CommitTransaction(AH);
		CommitTransactionXref(AH);
	}
}

/***********
 * Sorting and Reordering
 ***********/

/*
 * Move TOC entries of the specified type to the START of the TOC.
 */

/* Public */
void
MoveToStart(Archive *AHX, char *oType)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	TocEntry   *te = AH->toc->next;
	TocEntry   *newTe;

	while (te != AH->toc)
	{
		te->_moved = 0;
		te = te->next;
	}

	te = AH->toc->prev;
	while (te != AH->toc && !te->_moved)
	{
		newTe = te->prev;
		if (strcmp(te->desc, oType) == 0)
			_moveAfter(AH, AH->toc, te);
		te = newTe;
	}
}


/*
 * Move TOC entries of the specified type to the end of the TOC.
 */
/* Public */
void
MoveToEnd(Archive *AHX, char *oType)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	TocEntry   *te = AH->toc->next;
	TocEntry   *newTe;

	while (te != AH->toc)
	{
		te->_moved = 0;
		te = te->next;
	}

	te = AH->toc->next;
	while (te != AH->toc && !te->_moved)
	{
		newTe = te->next;
		if (strcmp(te->desc, oType) == 0)
			_moveBefore(AH, AH->toc, te);
		te = newTe;
	}
}

/*
 * Sort TOC by OID
 */
/* Public */
void
SortTocByOID(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	_SortToc(AH, _tocSortCompareByOIDNum);
}

/*
 * Sort TOC by ID
 */
/* Public */
void
SortTocByID(Archive *AHX)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;

	_SortToc(AH, _tocSortCompareByIDNum);
}

void
SortTocFromFile(Archive *AHX, RestoreOptions *ropt)
{
	ArchiveHandle *AH = (ArchiveHandle *) AHX;
	FILE	   *fh;
	char		buf[1024];
	char	   *cmnt;
	char	   *endptr;
	int			id;
	TocEntry   *te;
	TocEntry   *tePrev;
	int			i;

	/* Allocate space for the 'wanted' array, and init it */
	ropt->idWanted = (int *) malloc(sizeof(int) * AH->tocCount);
	for (i = 0; i < AH->tocCount; i++)
		ropt->idWanted[i] = 0;

	ropt->limitToList = 1;

	/* Mark all entries as 'not moved' */
	te = AH->toc->next;
	while (te != AH->toc)
	{
		te->_moved = 0;
		te = te->next;
	}

	/* Set prev entry as head of list */
	tePrev = AH->toc;

	/* Setup the file */
	fh = fopen(ropt->tocFile, PG_BINARY_R);
	if (!fh)
		die_horribly(AH, modulename, "could not open TOC file\n");

	while (fgets(buf, 1024, fh) != NULL)
	{
		/* Find a comment */
		cmnt = strchr(buf, ';');
		if (cmnt == buf)
			continue;

		/* End string at comment */
		if (cmnt != NULL)
			cmnt[0] = '\0';

		/* Skip if all spaces */
		if (strspn(buf, " \t") == strlen(buf))
			continue;

		/* Get an ID */
		id = strtol(buf, &endptr, 10);
		if (endptr == buf)
		{
			write_msg(modulename, "WARNING: line ignored: %s\n", buf);
			continue;
		}

		/* Find TOC entry */
		te = _getTocEntry(AH, id);
		if (!te)
			die_horribly(AH, modulename, "could not find entry for id %d\n", id);

		ropt->idWanted[id - 1] = 1;

		_moveAfter(AH, tePrev, te);
		tePrev = te;
	}

	if (fclose(fh) != 0)
		die_horribly(AH, modulename, "could not close TOC file: %s\n", strerror(errno));
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
archputc(const char c, Archive *AH)
{
	return WriteData(AH, &c, 1);
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
	 * This is paranoid: deal with the possibility that vsnprintf is
	 * willing to ignore trailing null
	 */

	/*
	 * or returns > 0 even if string does not fit. It may be the case that
	 * it returns cnt = bufsize
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

OutputContext
SetOutput(ArchiveHandle *AH, char *filename, int compression)
{
	OutputContext sav;

#ifdef HAVE_LIBZ
	char		fmode[10];
#endif
	int			fn = 0;

	/* Replace the AH output file handle */
	sav.OF = AH->OF;
	sav.gzOut = AH->gzOut;

	if (filename)
		fn = 0;
	else if (AH->FH)
		fn = fileno(AH->FH);
	else if (AH->fSpec)
	{
		fn = 0;
		filename = AH->fSpec;
	}
	else
		fn = fileno(stdout);

	/* If compression explicitly requested, use gzopen */
#ifdef HAVE_LIBZ
	if (compression != 0)
	{
		sprintf(fmode, "wb%d", compression);
		if (fn)
		{
			AH->OF = gzdopen(dup(fn), fmode);	/* Don't use PG_BINARY_x
												 * since this is zlib */
		}
		else
			AH->OF = gzopen(filename, fmode);
		AH->gzOut = 1;
	}
	else
	{							/* Use fopen */
#endif
		if (fn)
			AH->OF = fdopen(dup(fn), PG_BINARY_W);
		else
			AH->OF = fopen(filename, PG_BINARY_W);
		AH->gzOut = 0;
#ifdef HAVE_LIBZ
	}
#endif

	if (!AH->OF)
		die_horribly(AH, modulename, "could not open output file: %s\n", strerror(errno));

	return sav;
}

void
ResetOutput(ArchiveHandle *AH, OutputContext sav)
{
	int			res;

	if (AH->gzOut)
		res = GZCLOSE(AH->OF);
	else
		res = fclose(AH->OF);

	if (res != 0)
		die_horribly(AH, modulename, "could not close output file: %s\n", strerror(errno));

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
	 * This is paranoid: deal with the possibility that vsnprintf is
	 * willing to ignore trailing null
	 */

	/*
	 * or returns > 0 even if string does not fit. It may be the case that
	 * it returns cnt = bufsize
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
int
RestoringToDB(ArchiveHandle *AH)
{
	return (AH->ropt && AH->ropt->useDB && AH->connection);
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
	int			res;

	if (AH->writingBlob)
	{
		res = lo_write(AH->connection, AH->loFd, (void *) ptr, size * nmemb);
		ahlog(AH, 5, "wrote %d bytes of large object data (result = %d)\n",
			  (int) (size * nmemb), res);
		if (res != size * nmemb)
			die_horribly(AH, modulename, "could not write to large object (result: %d, expected: %d)\n",
						 res, (int) (size * nmemb));

		return res;
	}
	else if (AH->gzOut)
	{
		res = GZWRITE((void *) ptr, size, nmemb, AH->OF);
		if (res != (nmemb * size))
			die_horribly(AH, modulename, "could not write to compressed archive\n");
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
				die_horribly(AH, modulename, "could not write to output file (%d != %d)\n",
							 res, (int) nmemb);
			return res;
		}
	}
}

/* Common exit code */
static void
_write_msg(const char *modulename, const char *fmt, va_list ap)
{
	if (modulename)
		fprintf(stderr, "%s: [%s] ", progname, gettext(modulename));
	else
		fprintf(stderr, "%s: ", progname);
	vfprintf(stderr, gettext(fmt), ap);
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
	if (AH->public.verbose)
		write_msg(NULL, "*** aborted because of error\n");

	if (AH)
		if (AH->connection)
			PQfinish(AH->connection);
	if (AH->blobConnection)
		PQfinish(AH->blobConnection);

	exit(1);
}

/* External use */
void
exit_horribly(Archive *AH, const char *modulename, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	_die_horribly((ArchiveHandle *) AH, modulename, fmt, ap);
}

/* Archiver use (just different arg declaration) */
void
die_horribly(ArchiveHandle *AH, const char *modulename, const char *fmt,...)
{
	va_list		ap;

	va_start(ap, fmt);
	_die_horribly(AH, modulename, fmt, ap);
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

	te->_moved = 1;
}

static void
_moveBefore(ArchiveHandle *AH, TocEntry *pos, TocEntry *te)
{
	te->prev->next = te->next;
	te->next->prev = te->prev;

	te->prev = pos->prev;
	te->next = pos;
	pos->prev->next = te;
	pos->prev = te;

	te->_moved = 1;
}

static TocEntry *
_getTocEntry(ArchiveHandle *AH, int id)
{
	TocEntry   *te;

	te = AH->toc->next;
	while (te != AH->toc)
	{
		if (te->id == id)
			return te;
		te = te->next;
	}
	return NULL;
}

int
TocIDRequired(ArchiveHandle *AH, int id, RestoreOptions *ropt)
{
	TocEntry   *te = _getTocEntry(AH, id);

	if (!te)
		return 0;

	return _tocEntryRequired(te, ropt);
}

int
WriteInt(ArchiveHandle *AH, int i)
{
	int			b;

	/*
	 * This is a bit yucky, but I don't want to make the binary format
	 * very dependant on representation, and not knowing much about it, I
	 * write out a sign byte. If you change this, don't forget to change
	 * the file version #, and modify readInt to read the new format AS
	 * WELL AS the old formats.
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
		i = i / 256;
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

int
WriteStr(ArchiveHandle *AH, const char *c)
{
	int			res;

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
	if (l == -1)
		buf = NULL;
	else
	{
		buf = (char *) malloc(l + 1);
		if (!buf)
			die_horribly(AH, modulename, "out of memory\n");

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
	int			cnt;
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
			die_horribly(AH, modulename, "input file is too short (read %d, expected 5)\n", cnt);
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

		AH->intSize = fgetc(fh);
		AH->lookahead[AH->lookaheadLen++] = AH->intSize;

		AH->format = fgetc(fh);
		AH->lookahead[AH->lookaheadLen++] = AH->format;

		/* Make a convenient integer <maj><min><rev>00 */
		AH->version = ((AH->vmaj * 256 + AH->vmin) * 256 + AH->vrev) * 256 + 0;
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
	if (fseek(fh, 0, SEEK_SET) != 0)
	{
		/*
		 * NOTE: Formats that use the looahead buffer can unset this in
		 * their Init routine.
		 */
		AH->readHeader = 1;
	}
	else
		AH->lookaheadLen = 0;	/* Don't bother since we've reset the file */

#if 0
	write_msg(modulename, "read %d bytes into lookahead buffer\n", AH->lookaheadLen);
#endif

	/* Close the file */
	if (wantClose)
		if (fclose(fh) != 0)
			die_horribly(AH, modulename, "could not close the input file after reading header: %s\n",
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

	AH->vmaj = K_VERS_MAJOR;
	AH->vmin = K_VERS_MINOR;
	AH->vrev = K_VERS_REV;

	AH->createDate = time(NULL);

	AH->intSize = sizeof(int);
	AH->lastID = 0;
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

	AH->currUser = strdup("");	/* So it's valid, but we can free() it
								 * later if necessary */

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
			die_horribly(AH, modulename, "unrecognized file format '%d'\n", fmt);
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
			 * printf("Dumper arg for %d is %x\n", te->id,
			 * te->dataDumperArg);
			 */

			/*
			 * The user-provided DataDumper routine needs to call
			 * AH->WriteData
			 */
			(*te->dataDumper) ((Archive *) AH, te->oid, te->dataDumperArg);

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
	TocEntry   *te = AH->toc->next;
	const char *dep;
	int			i;

	/* printf("%d TOC Entries to save\n", AH->tocCount); */

	WriteInt(AH, AH->tocCount);
	while (te != AH->toc)
	{
		WriteInt(AH, te->id);
		WriteInt(AH, te->dataDumper ? 1 : 0);
		WriteStr(AH, te->oid);

		WriteStr(AH, te->name);
		WriteStr(AH, te->desc);
		WriteStr(AH, te->defn);
		WriteStr(AH, te->dropStmt);
		WriteStr(AH, te->copyStmt);
		WriteStr(AH, te->owner);

		/* Dump list of dependencies */
		if (te->depOid != NULL)
		{
			i = 0;
			while ((dep = (*te->depOid)[i++]) != NULL)
				WriteStr(AH, dep);
		}
		WriteStr(AH, NULL);		/* Terminate List */

		if (AH->WriteExtraTocPtr)
			(*AH->WriteExtraTocPtr) (AH, te);
		te = te->next;
	}
}

void
ReadToc(ArchiveHandle *AH)
{
	int			i;
	char	   *((*deps)[]);
	int			depIdx;
	int			depSize;

	TocEntry   *te = AH->toc->next;

	AH->tocCount = ReadInt(AH);

	for (i = 0; i < AH->tocCount; i++)
	{

		te = (TocEntry *) calloc(1, sizeof(TocEntry));
		te->id = ReadInt(AH);

		/* Sanity check */
		if (te->id <= 0 || te->id > AH->tocCount)
			die_horribly(AH, modulename, "entry id out of range - perhaps a corrupt TOC\n");

		te->hadDumper = ReadInt(AH);
		te->oid = ReadStr(AH);
		te->oidVal = atooid(te->oid);

		te->name = ReadStr(AH);
		te->desc = ReadStr(AH);
		te->defn = ReadStr(AH);
		te->dropStmt = ReadStr(AH);

		if (AH->version >= K_VERS_1_3)
			te->copyStmt = ReadStr(AH);

		te->owner = ReadStr(AH);

		/* Read TOC entry dependencies */
		if (AH->version >= K_VERS_1_5)
		{
			depSize = 100;
			deps = malloc(sizeof(char *) * depSize);
			depIdx = 0;
			do
			{
				if (depIdx > depSize)
				{
					depSize *= 2;
					deps = realloc(deps, sizeof(char *) * depSize);
				}
				(*deps)[depIdx] = ReadStr(AH);
#if 0
				if ((*deps)[depIdx])
					write_msg(modulename, "read dependency for %s -> %s\n",
							  te->name, (*deps)[depIdx]);
#endif
			} while ((*deps)[depIdx++] != NULL);

			if (depIdx > 1)		/* We have a non-null entry */
				te->depOid = realloc(deps, sizeof(char *) * depIdx);	/* trim it */
			else
				te->depOid = NULL;		/* no deps */
		}
		else
			te->depOid = NULL;

		/* Set maxOidVal etc for use in sorting */
		_fixupOidInfo(te);

		if (AH->ReadExtraTocPtr)
			(*AH->ReadExtraTocPtr) (AH, te);

		ahlog(AH, 3, "read TOC entry %d (id %d) for %s %s\n", i, te->id, te->desc, te->name);

		te->prev = AH->toc->prev;
		AH->toc->prev->next = te;
		AH->toc->prev = te;
		te->next = AH->toc;
	}
}

static teReqs
_tocEntryRequired(TocEntry *te, RestoreOptions *ropt)
{
	teReqs		res = 3;		/* Schema = 1, Data = 2, Both = 3 */

	/* If it's an ACL, maybe ignore it */
	if (ropt->aclsSkip && strcmp(te->desc, "ACL") == 0)
		return 0;

	if (!ropt->create && strcmp(te->desc, "DATABASE") == 0)
		return 0;

	/* Check if tablename only is wanted */
	if (ropt->selTypes)
	{
		if ((strcmp(te->desc, "TABLE") == 0) || (strcmp(te->desc, "TABLE DATA") == 0))
		{
			if (!ropt->selTable)
				return 0;
			if (ropt->tableNames && strcmp(ropt->tableNames, te->name) != 0)
				return 0;
		}
		else if (strcmp(te->desc, "INDEX") == 0)
		{
			if (!ropt->selIndex)
				return 0;
			if (ropt->indexNames && strcmp(ropt->indexNames, te->name) != 0)
				return 0;
		}
		else if (strcmp(te->desc, "FUNCTION") == 0)
		{
			if (!ropt->selFunction)
				return 0;
			if (ropt->functionNames && strcmp(ropt->functionNames, te->name) != 0)
				return 0;
		}
		else if (strcmp(te->desc, "TRIGGER") == 0)
		{
			if (!ropt->selTrigger)
				return 0;
			if (ropt->triggerNames && strcmp(ropt->triggerNames, te->name) != 0)
				return 0;
		}
		else
			return 0;
	}

	/*
	 * Check if we had a dataDumper. Indicates if the entry is schema or
	 * data
	 */
	if (!te->hadDumper)
	{
		/*
		 * Special Case: If 'SEQUENCE SET' then it is considered a data
		 * entry
		 */
		if (strcmp(te->desc, "SEQUENCE SET") == 0)
			res = res & REQ_DATA;
		else
			res = res & ~REQ_DATA;
	}

    /* Special case: <Init> type with <Max OID> name; this is part of
     * a DATA restore even though it has SQL.
     */
	if (  ( strcmp(te->desc, "<Init>") == 0 ) && ( strcmp(te->name, "Max OID") == 0) ) {
		res = REQ_DATA;
	}

	/* Mask it if we only want schema */
	if (ropt->schemaOnly)
		res = res & REQ_SCHEMA;

	/* Mask it we only want data */
	if (ropt->dataOnly)
		res = res & REQ_DATA;

	/* Mask it if we don't have a schema contribition */
	if (!te->defn || strlen(te->defn) == 0)
		res = res & ~REQ_SCHEMA;

	/* Finally, if we used a list, limit based on that as well */
	if (ropt->limitToList && !ropt->idWanted[te->id - 1])
		return 0;

	return res;
}


/*
 * Issue the commands to connect to the database as the specified user
 * to the specified database.  The database name may be NULL, then the
 * current database is kept.  If reconnects were disallowed by the
 * user, this won't do anything.
 *
 * If we're currently restoring right into a database, this will
 * actuall establish a connection.	Otherwise it puts a \connect into
 * the script output.
 */
static void
_reconnectAsUser(ArchiveHandle *AH, const char *dbname, const char *user)
{
	if (!user || strlen(user) == 0
		|| (strcmp(AH->currUser, user) == 0 && !dbname))
		return;					/* no need to do anything */

	/*
	 * Use SET SESSION AUTHORIZATION if allowed and no database change
	 * needed
	 */
	if (!dbname && AH->ropt->use_setsessauth)
	{
		if (RestoringToDB(AH))
		{
			PQExpBuffer qry = createPQExpBuffer();
			PGresult   *res;

			appendPQExpBuffer(qry, "SET SESSION AUTHORIZATION '%s';", user);
			res = PQexec(AH->connection, qry->data);

			if (!res || PQresultStatus(res) != PGRES_COMMAND_OK)
				die_horribly(AH, modulename, "could not set session user to %s: %s",
							 user, PQerrorMessage(AH->connection));

			PQclear(res);
			destroyPQExpBuffer(qry);
		}
		else
			ahprintf(AH, "SET SESSION AUTHORIZATION '%s';\n\n", user);
	}
	/* When -R was given, don't do anything. */
	else if (AH->ropt && AH->ropt->noReconnect)
		return;

	else if (RestoringToDB(AH))
		ReconnectToServer(AH, dbname, user);
	else
		/* FIXME: does not handle mixed case user names */
		ahprintf(AH, "\\connect %s %s\n\n",
				 dbname ? dbname : "-",
				 user ? user : "-");

	/*
	 * NOTE: currUser keeps track of what the imaginary session user in
	 * our script is
	 */
	if (AH->currUser)
		free(AH->currUser);

	AH->currUser = strdup(user);
}


/*
 * Issues the commands to connect to the database (or the current one,
 * if NULL) as the owner of the the given TOC entry object.  If
 * changes in ownership are not allowed, this doesn't do anything.
 */
static void
_reconnectAsOwner(ArchiveHandle *AH, const char *dbname, TocEntry *te)
{
	if (AH->ropt && AH->ropt->noOwner)
		return;

	_reconnectAsUser(AH, dbname, te->owner);
}


static int
_printTocEntry(ArchiveHandle *AH, TocEntry *te, RestoreOptions *ropt, bool isData)
{
	char	   *pfx;

	if (isData)
		pfx = "Data for ";
	else
		pfx = "";

	ahprintf(AH, "--\n-- %sTOC Entry ID %d (OID %s)\n--\n-- Name: %s Type: %s Owner: %s\n",
			 pfx, te->id, te->oid, te->name, te->desc, te->owner);
	if (AH->PrintExtraTocPtr !=NULL)
		(*AH->PrintExtraTocPtr) (AH, te);
	ahprintf(AH, "--\n\n");

	ahprintf(AH, "%s\n", te->defn);

	return 1;
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
	(*AH->WriteBytePtr) (AH, AH->format);

#ifndef HAVE_LIBZ
	if (AH->compression != 0)
		write_msg(modulename, "WARNING: requested compression not available in this "
				  "installation - archive will be uncompressed\n");

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
	WriteStr(AH, AH->dbname);
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

		(*AH->ReadBufPtr) (AH, tmpMag, 5);

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
			die_horribly(AH, modulename, "sanity check on integer size (%d) failed\n", AH->intSize);

		if (AH->intSize > sizeof(int))
			write_msg(modulename, "WARNING: archive was made on a machine with larger integers, some operations may fail\n");

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
		write_msg(modulename, "WARNING: archive is compressed, but this installation does not support compression - no data will be available\n");
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

}


static void
_SortToc(ArchiveHandle *AH, TocSortCompareFn fn)
{
	TocEntry  **tea;
	TocEntry   *te;
	int			i;

	/* Allocate an array for quicksort (TOC size + head & foot) */
	tea = (TocEntry **) malloc(sizeof(TocEntry *) * (AH->tocCount + 2));

	/* Build array of toc entries, including header at start and end */
	te = AH->toc;
	for (i = 0; i <= AH->tocCount + 1; i++)
	{
		/*
		 * printf("%d: %x (%x, %x) - %d\n", i, te, te->prev, te->next,
		 * te->oidVal);
		 */
		tea[i] = te;
		te = te->next;
	}

	/* Sort it, but ignore the header entries */
	qsort(&(tea[1]), AH->tocCount, sizeof(TocEntry *), fn);

	/* Rebuild list: this works becuase we have headers at each end */
	for (i = 1; i <= AH->tocCount; i++)
	{
		tea[i]->next = tea[i + 1];
		tea[i]->prev = tea[i - 1];
	}


	te = AH->toc;
	for (i = 0; i <= AH->tocCount + 1; i++)
	{
		/*
		 * printf("%d: %x (%x, %x) - %d\n", i, te, te->prev, te->next,
		 * te->oidVal);
		 */
		te = te->next;
	}


	AH->toc->next = tea[1];
	AH->toc->prev = tea[AH->tocCount];
}

static int
_tocSortCompareByOIDNum(const void *p1, const void *p2)
{
	TocEntry   *te1 = *(TocEntry **) p1;
	TocEntry   *te2 = *(TocEntry **) p2;
	Oid			id1 = te1->maxOidVal;
	Oid			id2 = te2->maxOidVal;
	int			cmpval;

	/* printf("Comparing %d to %d\n", id1, id2); */

	cmpval = oidcmp(id1, id2);

	/* If we have a deterministic answer, return it. */
	if (cmpval != 0)
		return cmpval;

	/* More comparisons required */
	if (oideq(id1, te1->maxDepOidVal))	/* maxOid1 came from deps */
	{
		if (oideq(id2, te2->maxDepOidVal))		/* maxOid2 also came from
												 * deps */
		{
			cmpval = oidcmp(te1->oidVal, te2->oidVal);	/* Just compare base
														 * OIDs */
		}
		else
/* MaxOid2 was entry OID */
		{
			return 1;			/* entry1 > entry2 */
		};
	}
	else
/* must have oideq(id1, te1->oidVal) => maxOid1 = Oid1 */
	{
		if (oideq(id2, te2->maxDepOidVal))		/* maxOid2 came from deps */
		{
			return -1;			/* entry1 < entry2 */
		}
		else
/* MaxOid2 was entry OID - deps don't matter */
		{
			cmpval = 0;
		};
	};

	/*
	 * If we get here, then we've done another comparison Once again, a 0
	 * result means we require even more
	 */
	if (cmpval != 0)
		return cmpval;

	/*
	 * Entire OID details match, so use ID number (ie. original pg_dump
	 * order)
	 */
	return _tocSortCompareByIDNum(te1, te2);
}

static int
_tocSortCompareByIDNum(const void *p1, const void *p2)
{
	TocEntry   *te1 = *(TocEntry **) p1;
	TocEntry   *te2 = *(TocEntry **) p2;
	int			id1 = te1->id;
	int			id2 = te2->id;

	/* printf("Comparing %d to %d\n", id1, id2); */

	if (id1 < id2)
		return -1;
	else if (id1 > id2)
		return 1;
	else
		return 0;
}

/*
 * Assuming Oid and depOid are set, work out the various
 * Oid values used in sorting.
 */
static void
_fixupOidInfo(TocEntry *te)
{
	te->oidVal = atooid(te->oid);
	te->maxDepOidVal = _findMaxOID(te->depOid);

	/* For the purpose of sorting, find the max OID. */
	if (oidcmp(te->oidVal, te->maxDepOidVal) >= 0)
		te->maxOidVal = te->oidVal;
	else
		te->maxOidVal = te->maxDepOidVal;
}

/*
 * Find the max OID value for a given list of string Oid values
 */
static Oid
_findMaxOID(const char *((*deps)[]))
{
	const char *dep;
	int			i;
	Oid			maxOid = (Oid) 0;
	Oid			currOid;

	if (!deps)
		return maxOid;

	i = 0;
	while ((dep = (*deps)[i++]) != NULL)
	{
		currOid = atooid(dep);
		if (oidcmp(maxOid, currOid) < 0)
			maxOid = currOid;
	}

	return maxOid;
}

/*
 * Maybe I can use this somewhere...
 *
 *create table pgdump_blob_path(p text);
 *insert into pgdump_blob_path values('/home/pjw/work/postgresql-cvs/pgsql/src/bin/pg_dump_140');
 *
 *insert into dump_blob_xref select 12345,lo_import(p || '/q.q') from pgdump_blob_path;
 */
