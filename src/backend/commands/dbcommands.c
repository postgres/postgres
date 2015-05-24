/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		Database management commands (create/drop database).
 *
 * Note: database creation/destruction commands use exclusive locks on
 * the database objects (as expressed by LockSharedObject()) to avoid
 * stepping on each others' toes.  Formerly we used table-level locks
 * on pg_database, but that's too coarse-grained.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dbcommands.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <locale.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/dbcommands_xlog.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgwriter.h"
#include "replication/slot.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_locale.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


typedef struct
{
	Oid			src_dboid;		/* source (template) DB */
	Oid			dest_dboid;		/* DB we are trying to create */
} createdb_failure_params;

typedef struct
{
	Oid			dest_dboid;		/* DB we are trying to move */
	Oid			dest_tsoid;		/* tablespace we are trying to move to */
} movedb_failure_params;

/* non-export function prototypes */
static void createdb_failure_callback(int code, Datum arg);
static void movedb(const char *dbname, const char *tblspcname);
static void movedb_failure_callback(int code, Datum arg);
static bool get_db_info(const char *name, LOCKMODE lockmode,
			Oid *dbIdP, Oid *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP,
			Oid *dbLastSysOidP, TransactionId *dbFrozenXidP,
			MultiXactId *dbMinMultiP,
			Oid *dbTablespace, char **dbCollate, char **dbCtype);
static bool have_createdb_privilege(void);
static void remove_dbtablespaces(Oid db_id);
static bool check_db_file_conflict(Oid db_id);
static int	errdetail_busy_db(int notherbackends, int npreparedxacts);


/*
 * CREATE DATABASE
 */
Oid
createdb(const CreatedbStmt *stmt)
{
	HeapScanDesc scan;
	Relation	rel;
	Oid			src_dboid;
	Oid			src_owner;
	int			src_encoding;
	char	   *src_collate;
	char	   *src_ctype;
	bool		src_istemplate;
	bool		src_allowconn;
	Oid			src_lastsysoid;
	TransactionId src_frozenxid;
	MultiXactId src_minmxid;
	Oid			src_deftablespace;
	volatile Oid dst_deftablespace;
	Relation	pg_database_rel;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_database];
	bool		new_record_nulls[Natts_pg_database];
	Oid			dboid;
	Oid			datdba;
	ListCell   *option;
	DefElem    *dtablespacename = NULL;
	DefElem    *downer = NULL;
	DefElem    *dtemplate = NULL;
	DefElem    *dencoding = NULL;
	DefElem    *dcollate = NULL;
	DefElem    *dctype = NULL;
	DefElem    *distemplate = NULL;
	DefElem    *dallowconnections = NULL;
	DefElem    *dconnlimit = NULL;
	char	   *dbname = stmt->dbname;
	char	   *dbowner = NULL;
	const char *dbtemplate = NULL;
	char	   *dbcollate = NULL;
	char	   *dbctype = NULL;
	char	   *canonname;
	int			encoding = -1;
	bool		dbistemplate = false;
	bool		dballowconnections = true;
	int			dbconnlimit = -1;
	int			notherbackends;
	int			npreparedxacts;
	createdb_failure_params fparms;

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "tablespace") == 0)
		{
			if (dtablespacename)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dtablespacename = defel;
		}
		else if (strcmp(defel->defname, "owner") == 0)
		{
			if (downer)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			downer = defel;
		}
		else if (strcmp(defel->defname, "template") == 0)
		{
			if (dtemplate)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dtemplate = defel;
		}
		else if (strcmp(defel->defname, "encoding") == 0)
		{
			if (dencoding)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dencoding = defel;
		}
		else if (strcmp(defel->defname, "lc_collate") == 0)
		{
			if (dcollate)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dcollate = defel;
		}
		else if (strcmp(defel->defname, "lc_ctype") == 0)
		{
			if (dctype)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dctype = defel;
		}
		else if (strcmp(defel->defname, "is_template") == 0)
		{
			if (distemplate)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			distemplate = defel;
		}
		else if (strcmp(defel->defname, "allow_connections") == 0)
		{
			if (dallowconnections)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dallowconnections = defel;
		}
		else if (strcmp(defel->defname, "connection_limit") == 0)
		{
			if (dconnlimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "location") == 0)
		{
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("LOCATION is not supported anymore"),
					 errhint("Consider using tablespaces instead.")));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized", defel->defname)));
	}

	if (downer && downer->arg)
		dbowner = defGetString(downer);
	if (dtemplate && dtemplate->arg)
		dbtemplate = defGetString(dtemplate);
	if (dencoding && dencoding->arg)
	{
		const char *encoding_name;

		if (IsA(dencoding->arg, Integer))
		{
			encoding = defGetInt32(dencoding);
			encoding_name = pg_encoding_to_char(encoding);
			if (strcmp(encoding_name, "") == 0 ||
				pg_valid_server_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%d is not a valid encoding code",
								encoding)));
		}
		else
		{
			encoding_name = defGetString(dencoding);
			encoding = pg_valid_server_encoding(encoding_name);
			if (encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%s is not a valid encoding name",
								encoding_name)));
		}
	}
	if (dcollate && dcollate->arg)
		dbcollate = defGetString(dcollate);
	if (dctype && dctype->arg)
		dbctype = defGetString(dctype);
	if (distemplate && distemplate->arg)
		dbistemplate = defGetBoolean(distemplate);
	if (dallowconnections && dallowconnections->arg)
		dballowconnections = defGetBoolean(dallowconnections);
	if (dconnlimit && dconnlimit->arg)
	{
		dbconnlimit = defGetInt32(dconnlimit);
		if (dbconnlimit < -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", dbconnlimit)));
	}

	/* obtain OID of proposed owner */
	if (dbowner)
		datdba = get_role_oid(dbowner, false);
	else
		datdba = GetUserId();

	/*
	 * To create a database, must have createdb privilege and must be able to
	 * become the target role (this does not imply that the target role itself
	 * must have createdb privilege).  The latter provision guards against
	 * "giveaway" attacks.  Note that a superuser will always have both of
	 * these privileges a fortiori.
	 */
	if (!have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create database")));

	check_is_member_of_role(GetUserId(), datdba);

	/*
	 * Lookup database (template) to be cloned, and obtain share lock on it.
	 * ShareLock allows two CREATE DATABASEs to work from the same template
	 * concurrently, while ensuring no one is busy dropping it in parallel
	 * (which would be Very Bad since we'd likely get an incomplete copy
	 * without knowing it).  This also prevents any new connections from being
	 * made to the source until we finish copying it, so we can be sure it
	 * won't change underneath us.
	 */
	if (!dbtemplate)
		dbtemplate = "template1";		/* Default template database name */

	if (!get_db_info(dbtemplate, ShareLock,
					 &src_dboid, &src_owner, &src_encoding,
					 &src_istemplate, &src_allowconn, &src_lastsysoid,
					 &src_frozenxid, &src_minmxid, &src_deftablespace,
					 &src_collate, &src_ctype))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("template database \"%s\" does not exist",
						dbtemplate)));

	/*
	 * Permission check: to copy a DB that's not marked datistemplate, you
	 * must be superuser or the owner thereof.
	 */
	if (!src_istemplate)
	{
		if (!pg_database_ownercheck(src_dboid, GetUserId()))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to copy database \"%s\"",
							dbtemplate)));
	}

	/* If encoding or locales are defaulted, use source's setting */
	if (encoding < 0)
		encoding = src_encoding;
	if (dbcollate == NULL)
		dbcollate = src_collate;
	if (dbctype == NULL)
		dbctype = src_ctype;

	/* Some encodings are client only */
	if (!PG_VALID_BE_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid server encoding %d", encoding)));

	/* Check that the chosen locales are valid, and get canonical spellings */
	if (!check_locale(LC_COLLATE, dbcollate, &canonname))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid locale name: \"%s\"", dbcollate)));
	dbcollate = canonname;
	if (!check_locale(LC_CTYPE, dbctype, &canonname))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid locale name: \"%s\"", dbctype)));
	dbctype = canonname;

	check_encoding_locale_matches(encoding, dbcollate, dbctype);

	/*
	 * Check that the new encoding and locale settings match the source
	 * database.  We insist on this because we simply copy the source data ---
	 * any non-ASCII data would be wrongly encoded, and any indexes sorted
	 * according to the source locale would be wrong.
	 *
	 * However, we assume that template0 doesn't contain any non-ASCII data
	 * nor any indexes that depend on collation or ctype, so template0 can be
	 * used as template for creating a database with any encoding or locale.
	 */
	if (strcmp(dbtemplate, "template0") != 0)
	{
		if (encoding != src_encoding)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new encoding (%s) is incompatible with the encoding of the template database (%s)",
							pg_encoding_to_char(encoding),
							pg_encoding_to_char(src_encoding)),
					 errhint("Use the same encoding as in the template database, or use template0 as template.")));

		if (strcmp(dbcollate, src_collate) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new collation (%s) is incompatible with the collation of the template database (%s)",
							dbcollate, src_collate),
					 errhint("Use the same collation as in the template database, or use template0 as template.")));

		if (strcmp(dbctype, src_ctype) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("new LC_CTYPE (%s) is incompatible with the LC_CTYPE of the template database (%s)",
							dbctype, src_ctype),
					 errhint("Use the same LC_CTYPE as in the template database, or use template0 as template.")));
	}

	/* Resolve default tablespace for new database */
	if (dtablespacename && dtablespacename->arg)
	{
		char	   *tablespacename;
		AclResult	aclresult;

		tablespacename = defGetString(dtablespacename);
		dst_deftablespace = get_tablespace_oid(tablespacename, false);
		/* check permissions */
		aclresult = pg_tablespace_aclcheck(dst_deftablespace, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   tablespacename);

		/* pg_global must never be the default tablespace */
		if (dst_deftablespace == GLOBALTABLESPACE_OID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				  errmsg("pg_global cannot be used as default tablespace")));

		/*
		 * If we are trying to change the default tablespace of the template,
		 * we require that the template not have any files in the new default
		 * tablespace.  This is necessary because otherwise the copied
		 * database would contain pg_class rows that refer to its default
		 * tablespace both explicitly (by OID) and implicitly (as zero), which
		 * would cause problems.  For example another CREATE DATABASE using
		 * the copied database as template, and trying to change its default
		 * tablespace again, would yield outright incorrect results (it would
		 * improperly move tables to the new default tablespace that should
		 * stay in the same tablespace).
		 */
		if (dst_deftablespace != src_deftablespace)
		{
			char	   *srcpath;
			struct stat st;

			srcpath = GetDatabasePath(src_dboid, dst_deftablespace);

			if (stat(srcpath, &st) == 0 &&
				S_ISDIR(st.st_mode) &&
				!directory_is_empty(srcpath))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot assign new default tablespace \"%s\"",
								tablespacename),
						 errdetail("There is a conflict because database \"%s\" already has some tables in this tablespace.",
								   dbtemplate)));
			pfree(srcpath);
		}
	}
	else
	{
		/* Use template database's default tablespace */
		dst_deftablespace = src_deftablespace;
		/* Note there is no additional permission check in this path */
	}

	/*
	 * Check for db name conflict.  This is just to give a more friendly error
	 * message than "unique index violation".  There's a race condition but
	 * we're willing to accept the less friendly message in that case.
	 */
	if (OidIsValid(get_database_oid(dbname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", dbname)));

	/*
	 * The source DB can't have any active backends, except this one
	 * (exception is to allow CREATE DB while connected to template1).
	 * Otherwise we might copy inconsistent data.
	 *
	 * This should be last among the basic error checks, because it involves
	 * potential waiting; we may as well throw an error first if we're gonna
	 * throw one.
	 */
	if (CountOtherDBBackends(src_dboid, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
			errmsg("source database \"%s\" is being accessed by other users",
				   dbtemplate),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * Select an OID for the new database, checking that it doesn't have a
	 * filename conflict with anything already existing in the tablespace
	 * directories.
	 */
	pg_database_rel = heap_open(DatabaseRelationId, RowExclusiveLock);

	do
	{
		dboid = GetNewOid(pg_database_rel);
	} while (check_db_file_conflict(dboid));

	/*
	 * Insert a new tuple into pg_database.  This establishes our ownership of
	 * the new database name (anyone else trying to insert the same name will
	 * block on the unique index, and fail after we commit).
	 */

	/* Form tuple */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));

	new_record[Anum_pg_database_datname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(dbname));
	new_record[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(datdba);
	new_record[Anum_pg_database_encoding - 1] = Int32GetDatum(encoding);
	new_record[Anum_pg_database_datcollate - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(dbcollate));
	new_record[Anum_pg_database_datctype - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(dbctype));
	new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(dbistemplate);
	new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(dballowconnections);
	new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
	new_record[Anum_pg_database_datlastsysoid - 1] = ObjectIdGetDatum(src_lastsysoid);
	new_record[Anum_pg_database_datfrozenxid - 1] = TransactionIdGetDatum(src_frozenxid);
	new_record[Anum_pg_database_datminmxid - 1] = TransactionIdGetDatum(src_minmxid);
	new_record[Anum_pg_database_dattablespace - 1] = ObjectIdGetDatum(dst_deftablespace);

	/*
	 * We deliberately set datacl to default (NULL), rather than copying it
	 * from the template database.  Copying it would be a bad idea when the
	 * owner is not the same as the template's owner.
	 */
	new_record_nulls[Anum_pg_database_datacl - 1] = true;

	tuple = heap_form_tuple(RelationGetDescr(pg_database_rel),
							new_record, new_record_nulls);

	HeapTupleSetOid(tuple, dboid);

	simple_heap_insert(pg_database_rel, tuple);

	/* Update indexes */
	CatalogUpdateIndexes(pg_database_rel, tuple);

	/*
	 * Now generate additional catalog entries associated with the new DB
	 */

	/* Register owner dependency */
	recordDependencyOnOwner(DatabaseRelationId, dboid, datdba);

	/* Create pg_shdepend entries for objects within database */
	copyTemplateDependencies(src_dboid, dboid);

	/* Post creation hook for new database */
	InvokeObjectPostCreateHook(DatabaseRelationId, dboid, 0);

	/*
	 * Force a checkpoint before starting the copy. This will force all dirty
	 * buffers, including those of unlogged tables, out to disk, to ensure
	 * source database is up-to-date on disk for the copy.
	 * FlushDatabaseBuffers() would suffice for that, but we also want to
	 * process any pending unlink requests. Otherwise, if a checkpoint
	 * happened while we're copying files, a file might be deleted just when
	 * we're about to copy it, causing the lstat() call in copydir() to fail
	 * with ENOENT.
	 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT
					  | CHECKPOINT_FLUSH_ALL);

	/*
	 * Once we start copying subdirectories, we need to be able to clean 'em
	 * up if we fail.  Use an ENSURE block to make sure this happens.  (This
	 * is not a 100% solution, because of the possibility of failure during
	 * transaction commit after we leave this routine, but it should handle
	 * most scenarios.)
	 */
	fparms.src_dboid = src_dboid;
	fparms.dest_dboid = dboid;
	PG_ENSURE_ERROR_CLEANUP(createdb_failure_callback,
							PointerGetDatum(&fparms));
	{
		/*
		 * Iterate through all tablespaces of the template database, and copy
		 * each one to the new database.
		 */
		rel = heap_open(TableSpaceRelationId, AccessShareLock);
		scan = heap_beginscan_catalog(rel, 0, NULL);
		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Oid			srctablespace = HeapTupleGetOid(tuple);
			Oid			dsttablespace;
			char	   *srcpath;
			char	   *dstpath;
			struct stat st;

			/* No need to copy global tablespace */
			if (srctablespace == GLOBALTABLESPACE_OID)
				continue;

			srcpath = GetDatabasePath(src_dboid, srctablespace);

			if (stat(srcpath, &st) < 0 || !S_ISDIR(st.st_mode) ||
				directory_is_empty(srcpath))
			{
				/* Assume we can ignore it */
				pfree(srcpath);
				continue;
			}

			if (srctablespace == src_deftablespace)
				dsttablespace = dst_deftablespace;
			else
				dsttablespace = srctablespace;

			dstpath = GetDatabasePath(dboid, dsttablespace);

			/*
			 * Copy this subdirectory to the new location
			 *
			 * We don't need to copy subdirectories
			 */
			copydir(srcpath, dstpath, false);

			/* Record the filesystem change in XLOG */
			{
				xl_dbase_create_rec xlrec;

				xlrec.db_id = dboid;
				xlrec.tablespace_id = dsttablespace;
				xlrec.src_db_id = src_dboid;
				xlrec.src_tablespace_id = srctablespace;

				XLogBeginInsert();
				XLogRegisterData((char *) &xlrec, sizeof(xl_dbase_create_rec));

				(void) XLogInsert(RM_DBASE_ID,
								  XLOG_DBASE_CREATE | XLR_SPECIAL_REL_UPDATE);
			}
		}
		heap_endscan(scan);
		heap_close(rel, AccessShareLock);

		/*
		 * We force a checkpoint before committing.  This effectively means
		 * that committed XLOG_DBASE_CREATE operations will never need to be
		 * replayed (at least not in ordinary crash recovery; we still have to
		 * make the XLOG entry for the benefit of PITR operations). This
		 * avoids two nasty scenarios:
		 *
		 * #1: When PITR is off, we don't XLOG the contents of newly created
		 * indexes; therefore the drop-and-recreate-whole-directory behavior
		 * of DBASE_CREATE replay would lose such indexes.
		 *
		 * #2: Since we have to recopy the source database during DBASE_CREATE
		 * replay, we run the risk of copying changes in it that were
		 * committed after the original CREATE DATABASE command but before the
		 * system crash that led to the replay.  This is at least unexpected
		 * and at worst could lead to inconsistencies, eg duplicate table
		 * names.
		 *
		 * (Both of these were real bugs in releases 8.0 through 8.0.3.)
		 *
		 * In PITR replay, the first of these isn't an issue, and the second
		 * is only a risk if the CREATE DATABASE and subsequent template
		 * database change both occur while a base backup is being taken.
		 * There doesn't seem to be much we can do about that except document
		 * it as a limitation.
		 *
		 * Perhaps if we ever implement CREATE DATABASE in a less cheesy way,
		 * we can avoid this.
		 */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

		/*
		 * Close pg_database, but keep lock till commit.
		 */
		heap_close(pg_database_rel, NoLock);

		/*
		 * Force synchronous commit, thus minimizing the window between
		 * creation of the database files and commital of the transaction. If
		 * we crash before committing, we'll have a DB that's taking up disk
		 * space but is not in pg_database, which is not good.
		 */
		ForceSyncCommit();
	}
	PG_END_ENSURE_ERROR_CLEANUP(createdb_failure_callback,
								PointerGetDatum(&fparms));

	return dboid;
}

/*
 * Check whether chosen encoding matches chosen locale settings.  This
 * restriction is necessary because libc's locale-specific code usually
 * fails when presented with data in an encoding it's not expecting. We
 * allow mismatch in four cases:
 *
 * 1. locale encoding = SQL_ASCII, which means that the locale is C/POSIX
 * which works with any encoding.
 *
 * 2. locale encoding = -1, which means that we couldn't determine the
 * locale's encoding and have to trust the user to get it right.
 *
 * 3. selected encoding is UTF8 and platform is win32. This is because
 * UTF8 is a pseudo codepage that is supported in all locales since it's
 * converted to UTF16 before being used.
 *
 * 4. selected encoding is SQL_ASCII, but only if you're a superuser. This
 * is risky but we have historically allowed it --- notably, the
 * regression tests require it.
 *
 * Note: if you change this policy, fix initdb to match.
 */
void
check_encoding_locale_matches(int encoding, const char *collate, const char *ctype)
{
	int			ctype_encoding = pg_get_encoding_from_locale(ctype, true);
	int			collate_encoding = pg_get_encoding_from_locale(collate, true);

	if (!(ctype_encoding == encoding ||
		  ctype_encoding == PG_SQL_ASCII ||
		  ctype_encoding == -1 ||
#ifdef WIN32
		  encoding == PG_UTF8 ||
#endif
		  (encoding == PG_SQL_ASCII && superuser())))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding \"%s\" does not match locale \"%s\"",
						pg_encoding_to_char(encoding),
						ctype),
		   errdetail("The chosen LC_CTYPE setting requires encoding \"%s\".",
					 pg_encoding_to_char(ctype_encoding))));

	if (!(collate_encoding == encoding ||
		  collate_encoding == PG_SQL_ASCII ||
		  collate_encoding == -1 ||
#ifdef WIN32
		  encoding == PG_UTF8 ||
#endif
		  (encoding == PG_SQL_ASCII && superuser())))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding \"%s\" does not match locale \"%s\"",
						pg_encoding_to_char(encoding),
						collate),
		 errdetail("The chosen LC_COLLATE setting requires encoding \"%s\".",
				   pg_encoding_to_char(collate_encoding))));
}

/* Error cleanup callback for createdb */
static void
createdb_failure_callback(int code, Datum arg)
{
	createdb_failure_params *fparms = (createdb_failure_params *) DatumGetPointer(arg);

	/*
	 * Release lock on source database before doing recursive remove. This is
	 * not essential but it seems desirable to release the lock as soon as
	 * possible.
	 */
	UnlockSharedObject(DatabaseRelationId, fparms->src_dboid, 0, ShareLock);

	/* Throw away any successfully copied subdirectories */
	remove_dbtablespaces(fparms->dest_dboid);
}


/*
 * DROP DATABASE
 */
void
dropdb(const char *dbname, bool missing_ok)
{
	Oid			db_id;
	bool		db_istemplate;
	Relation	pgdbrel;
	HeapTuple	tup;
	int			notherbackends;
	int			npreparedxacts;
	int			nslots,
				nslots_active;

	/*
	 * Look up the target database's OID, and get exclusive lock on it. We
	 * need this to ensure that no new backend starts up in the target
	 * database while we are deleting it (see postinit.c), and that no one is
	 * using it as a CREATE DATABASE template or trying to delete it for
	 * themselves.
	 */
	pgdbrel = heap_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(dbname, AccessExclusiveLock, &db_id, NULL, NULL,
				   &db_istemplate, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
	{
		if (!missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", dbname)));
		}
		else
		{
			/* Close pg_database, release the lock, since we changed nothing */
			heap_close(pgdbrel, RowExclusiveLock);
			ereport(NOTICE,
					(errmsg("database \"%s\" does not exist, skipping",
							dbname)));
			return;
		}
	}

	/*
	 * Permission checks
	 */
	if (!pg_database_ownercheck(db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   dbname);

	/* DROP hook for the database being removed */
	InvokeObjectDropHook(DatabaseRelationId, db_id, 0);

	/*
	 * Disallow dropping a DB that is marked istemplate.  This is just to
	 * prevent people from accidentally dropping template0 or template1; they
	 * can do so if they're really determined ...
	 */
	if (db_istemplate)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop a template database")));

	/* Obviously can't drop my own database */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot drop the currently open database")));

	/*
	 * Check whether there are, possibly unconnected, logical slots that refer
	 * to the to-be-dropped database. The database lock we are holding
	 * prevents the creation of new slots using the database.
	 */
	if (ReplicationSlotsCountDBSlots(db_id, &nslots, &nslots_active))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
			  errmsg("database \"%s\" is used by a logical replication slot",
					 dbname),
				 errdetail_plural("There is %d slot, %d of them active.",
								  "There are %d slots, %d of them active.",
								  nslots,
								  nslots, nslots_active)));

	/*
	 * Check for other backends in the target database.  (Because we hold the
	 * database lock, no new ones can start after this.)
	 *
	 * As in CREATE DATABASE, check this after other error conditions.
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						dbname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * Remove the database's tuple from pg_database.
	 */
	tup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(db_id));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for database %u", db_id);

	simple_heap_delete(pgdbrel, &tup->t_self);

	ReleaseSysCache(tup);

	/*
	 * Delete any comments or security labels associated with the database.
	 */
	DeleteSharedComments(db_id, DatabaseRelationId);
	DeleteSharedSecurityLabel(db_id, DatabaseRelationId);

	/*
	 * Remove settings associated with this database
	 */
	DropSetting(db_id, InvalidOid);

	/*
	 * Remove shared dependency references for the database.
	 */
	dropDatabaseDependencies(db_id);

	/*
	 * Drop pages for this database that are in the shared buffer cache. This
	 * is important to ensure that no remaining backend tries to write out a
	 * dirty buffer to the dead database later...
	 */
	DropDatabaseBuffers(db_id);

	/*
	 * Tell the stats collector to forget it immediately, too.
	 */
	pgstat_drop_database(db_id);

	/*
	 * Tell checkpointer to forget any pending fsync and unlink requests for
	 * files in the database; else the fsyncs will fail at next checkpoint, or
	 * worse, it will delete files that belong to a newly created database
	 * with the same OID.
	 */
	ForgetDatabaseFsyncRequests(db_id);

	/*
	 * Force a checkpoint to make sure the checkpointer has received the
	 * message sent by ForgetDatabaseFsyncRequests. On Windows, this also
	 * ensures that background procs don't hold any open files, which would
	 * cause rmdir() to fail.
	 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

	/*
	 * Remove all tablespace subdirs belonging to the database.
	 */
	remove_dbtablespaces(db_id);

	/*
	 * Close pg_database, but keep lock till commit.
	 */
	heap_close(pgdbrel, NoLock);

	/*
	 * Force synchronous commit, thus minimizing the window between removal of
	 * the database files and commital of the transaction. If we crash before
	 * committing, we'll have a DB that's gone on disk but still there
	 * according to pg_database, which is not good.
	 */
	ForceSyncCommit();
}


/*
 * Rename database
 */
ObjectAddress
RenameDatabase(const char *oldname, const char *newname)
{
	Oid			db_id;
	HeapTuple	newtup;
	Relation	rel;
	int			notherbackends;
	int			npreparedxacts;
	ObjectAddress address;

	/*
	 * Look up the target database's OID, and get exclusive lock on it. We
	 * need this for the same reasons as DROP DATABASE.
	 */
	rel = heap_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(oldname, AccessExclusiveLock, &db_id, NULL, NULL,
					 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", oldname)));

	/* must be owner */
	if (!pg_database_ownercheck(db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   oldname);

	/* must have createdb rights */
	if (!have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to rename database")));

	/*
	 * Make sure the new name doesn't exist.  See notes for same error in
	 * CREATE DATABASE.
	 */
	if (OidIsValid(get_database_oid(newname, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", newname)));

	/*
	 * XXX Client applications probably store the current database somewhere,
	 * so renaming it could cause confusion.  On the other hand, there may not
	 * be an actual problem besides a little confusion, so think about this
	 * and decide.
	 */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("current database cannot be renamed")));

	/*
	 * Make sure the database does not have active sessions.  This is the same
	 * concern as above, but applied to other sessions.
	 *
	 * As in CREATE DATABASE, check this after other error conditions.
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						oldname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/* rename */
	newtup = SearchSysCacheCopy1(DATABASEOID, ObjectIdGetDatum(db_id));
	if (!HeapTupleIsValid(newtup))
		elog(ERROR, "cache lookup failed for database %u", db_id);
	namestrcpy(&(((Form_pg_database) GETSTRUCT(newtup))->datname), newname);
	simple_heap_update(rel, &newtup->t_self, newtup);
	CatalogUpdateIndexes(rel, newtup);

	InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

	ObjectAddressSet(address, DatabaseRelationId, db_id);

	/*
	 * Close pg_database, but keep lock till commit.
	 */
	heap_close(rel, NoLock);

	return address;
}


/*
 * ALTER DATABASE SET TABLESPACE
 */
static void
movedb(const char *dbname, const char *tblspcname)
{
	Oid			db_id;
	Relation	pgdbrel;
	int			notherbackends;
	int			npreparedxacts;
	HeapTuple	oldtuple,
				newtuple;
	Oid			src_tblspcoid,
				dst_tblspcoid;
	Datum		new_record[Natts_pg_database];
	bool		new_record_nulls[Natts_pg_database];
	bool		new_record_repl[Natts_pg_database];
	ScanKeyData scankey;
	SysScanDesc sysscan;
	AclResult	aclresult;
	char	   *src_dbpath;
	char	   *dst_dbpath;
	DIR		   *dstdir;
	struct dirent *xlde;
	movedb_failure_params fparms;

	/*
	 * Look up the target database's OID, and get exclusive lock on it. We
	 * need this to ensure that no new backend starts up in the database while
	 * we are moving it, and that no one is using it as a CREATE DATABASE
	 * template or trying to delete it.
	 */
	pgdbrel = heap_open(DatabaseRelationId, RowExclusiveLock);

	if (!get_db_info(dbname, AccessExclusiveLock, &db_id, NULL, NULL,
				   NULL, NULL, NULL, NULL, NULL, &src_tblspcoid, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	/*
	 * We actually need a session lock, so that the lock will persist across
	 * the commit/restart below.  (We could almost get away with letting the
	 * lock be released at commit, except that someone could try to move
	 * relations of the DB back into the old directory while we rmtree() it.)
	 */
	LockSharedObjectForSession(DatabaseRelationId, db_id, 0,
							   AccessExclusiveLock);

	/*
	 * Permission checks
	 */
	if (!pg_database_ownercheck(db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   dbname);

	/*
	 * Obviously can't move the tables of my own database
	 */
	if (db_id == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot change the tablespace of the currently open database")));

	/*
	 * Get tablespace's oid
	 */
	dst_tblspcoid = get_tablespace_oid(tblspcname, false);

	/*
	 * Permission checks
	 */
	aclresult = pg_tablespace_aclcheck(dst_tblspcoid, GetUserId(),
									   ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
					   tblspcname);

	/*
	 * pg_global must never be the default tablespace
	 */
	if (dst_tblspcoid == GLOBALTABLESPACE_OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_global cannot be used as default tablespace")));

	/*
	 * No-op if same tablespace
	 */
	if (src_tblspcoid == dst_tblspcoid)
	{
		heap_close(pgdbrel, NoLock);
		UnlockSharedObjectForSession(DatabaseRelationId, db_id, 0,
									 AccessExclusiveLock);
		return;
	}

	/*
	 * Check for other backends in the target database.  (Because we hold the
	 * database lock, no new ones can start after this.)
	 *
	 * As in CREATE DATABASE, check this after other error conditions.
	 */
	if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						dbname),
				 errdetail_busy_db(notherbackends, npreparedxacts)));

	/*
	 * Get old and new database paths
	 */
	src_dbpath = GetDatabasePath(db_id, src_tblspcoid);
	dst_dbpath = GetDatabasePath(db_id, dst_tblspcoid);

	/*
	 * Force a checkpoint before proceeding. This will force all dirty
	 * buffers, including those of unlogged tables, out to disk, to ensure
	 * source database is up-to-date on disk for the copy.
	 * FlushDatabaseBuffers() would suffice for that, but we also want to
	 * process any pending unlink requests. Otherwise, the check for existing
	 * files in the target directory might fail unnecessarily, not to mention
	 * that the copy might fail due to source files getting deleted under it.
	 * On Windows, this also ensures that background procs don't hold any open
	 * files, which would cause rmdir() to fail.
	 */
	RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT
					  | CHECKPOINT_FLUSH_ALL);

	/*
	 * Now drop all buffers holding data of the target database; they should
	 * no longer be dirty so DropDatabaseBuffers is safe.
	 *
	 * It might seem that we could just let these buffers age out of shared
	 * buffers naturally, since they should not get referenced anymore.  The
	 * problem with that is that if the user later moves the database back to
	 * its original tablespace, any still-surviving buffers would appear to
	 * contain valid data again --- but they'd be missing any changes made in
	 * the database while it was in the new tablespace.  In any case, freeing
	 * buffers that should never be used again seems worth the cycles.
	 *
	 * Note: it'd be sufficient to get rid of buffers matching db_id and
	 * src_tblspcoid, but bufmgr.c presently provides no API for that.
	 */
	DropDatabaseBuffers(db_id);

	/*
	 * Check for existence of files in the target directory, i.e., objects of
	 * this database that are already in the target tablespace.  We can't
	 * allow the move in such a case, because we would need to change those
	 * relations' pg_class.reltablespace entries to zero, and we don't have
	 * access to the DB's pg_class to do so.
	 */
	dstdir = AllocateDir(dst_dbpath);
	if (dstdir != NULL)
	{
		while ((xlde = ReadDir(dstdir, dst_dbpath)) != NULL)
		{
			if (strcmp(xlde->d_name, ".") == 0 ||
				strcmp(xlde->d_name, "..") == 0)
				continue;

			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("some relations of database \"%s\" are already in tablespace \"%s\"",
							dbname, tblspcname),
					 errhint("You must move them back to the database's default tablespace before using this command.")));
		}

		FreeDir(dstdir);

		/*
		 * The directory exists but is empty. We must remove it before using
		 * the copydir function.
		 */
		if (rmdir(dst_dbpath) != 0)
			elog(ERROR, "could not remove directory \"%s\": %m",
				 dst_dbpath);
	}

	/*
	 * Use an ENSURE block to make sure we remove the debris if the copy fails
	 * (eg, due to out-of-disk-space).  This is not a 100% solution, because
	 * of the possibility of failure during transaction commit, but it should
	 * handle most scenarios.
	 */
	fparms.dest_dboid = db_id;
	fparms.dest_tsoid = dst_tblspcoid;
	PG_ENSURE_ERROR_CLEANUP(movedb_failure_callback,
							PointerGetDatum(&fparms));
	{
		/*
		 * Copy files from the old tablespace to the new one
		 */
		copydir(src_dbpath, dst_dbpath, false);

		/*
		 * Record the filesystem change in XLOG
		 */
		{
			xl_dbase_create_rec xlrec;

			xlrec.db_id = db_id;
			xlrec.tablespace_id = dst_tblspcoid;
			xlrec.src_db_id = db_id;
			xlrec.src_tablespace_id = src_tblspcoid;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xl_dbase_create_rec));

			(void) XLogInsert(RM_DBASE_ID,
							  XLOG_DBASE_CREATE | XLR_SPECIAL_REL_UPDATE);
		}

		/*
		 * Update the database's pg_database tuple
		 */
		ScanKeyInit(&scankey,
					Anum_pg_database_datname,
					BTEqualStrategyNumber, F_NAMEEQ,
					NameGetDatum(dbname));
		sysscan = systable_beginscan(pgdbrel, DatabaseNameIndexId, true,
									 NULL, 1, &scankey);
		oldtuple = systable_getnext(sysscan);
		if (!HeapTupleIsValid(oldtuple))		/* shouldn't happen... */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_DATABASE),
					 errmsg("database \"%s\" does not exist", dbname)));

		MemSet(new_record, 0, sizeof(new_record));
		MemSet(new_record_nulls, false, sizeof(new_record_nulls));
		MemSet(new_record_repl, false, sizeof(new_record_repl));

		new_record[Anum_pg_database_dattablespace - 1] = ObjectIdGetDatum(dst_tblspcoid);
		new_record_repl[Anum_pg_database_dattablespace - 1] = true;

		newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(pgdbrel),
									 new_record,
									 new_record_nulls, new_record_repl);
		simple_heap_update(pgdbrel, &oldtuple->t_self, newtuple);

		/* Update indexes */
		CatalogUpdateIndexes(pgdbrel, newtuple);

		InvokeObjectPostAlterHook(DatabaseRelationId,
								  HeapTupleGetOid(newtuple), 0);

		systable_endscan(sysscan);

		/*
		 * Force another checkpoint here.  As in CREATE DATABASE, this is to
		 * ensure that we don't have to replay a committed XLOG_DBASE_CREATE
		 * operation, which would cause us to lose any unlogged operations
		 * done in the new DB tablespace before the next checkpoint.
		 */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);

		/*
		 * Force synchronous commit, thus minimizing the window between
		 * copying the database files and commital of the transaction. If we
		 * crash before committing, we'll leave an orphaned set of files on
		 * disk, which is not fatal but not good either.
		 */
		ForceSyncCommit();

		/*
		 * Close pg_database, but keep lock till commit.
		 */
		heap_close(pgdbrel, NoLock);
	}
	PG_END_ENSURE_ERROR_CLEANUP(movedb_failure_callback,
								PointerGetDatum(&fparms));

	/*
	 * Commit the transaction so that the pg_database update is committed. If
	 * we crash while removing files, the database won't be corrupt, we'll
	 * just leave some orphaned files in the old directory.
	 *
	 * (This is OK because we know we aren't inside a transaction block.)
	 *
	 * XXX would it be safe/better to do this inside the ensure block?	Not
	 * convinced it's a good idea; consider elog just after the transaction
	 * really commits.
	 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/* Start new transaction for the remaining work; don't need a snapshot */
	StartTransactionCommand();

	/*
	 * Remove files from the old tablespace
	 */
	if (!rmtree(src_dbpath, true))
		ereport(WARNING,
				(errmsg("some useless files may be left behind in old database directory \"%s\"",
						src_dbpath)));

	/*
	 * Record the filesystem change in XLOG
	 */
	{
		xl_dbase_drop_rec xlrec;

		xlrec.db_id = db_id;
		xlrec.tablespace_id = src_tblspcoid;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xl_dbase_drop_rec));

		(void) XLogInsert(RM_DBASE_ID,
						  XLOG_DBASE_DROP | XLR_SPECIAL_REL_UPDATE);
	}

	/* Now it's safe to release the database lock */
	UnlockSharedObjectForSession(DatabaseRelationId, db_id, 0,
								 AccessExclusiveLock);
}

/* Error cleanup callback for movedb */
static void
movedb_failure_callback(int code, Datum arg)
{
	movedb_failure_params *fparms = (movedb_failure_params *) DatumGetPointer(arg);
	char	   *dstpath;

	/* Get rid of anything we managed to copy to the target directory */
	dstpath = GetDatabasePath(fparms->dest_dboid, fparms->dest_tsoid);

	(void) rmtree(dstpath, true);
}


/*
 * ALTER DATABASE name ...
 */
Oid
AlterDatabase(AlterDatabaseStmt *stmt, bool isTopLevel)
{
	Relation	rel;
	Oid			dboid;
	HeapTuple	tuple,
				newtuple;
	ScanKeyData scankey;
	SysScanDesc scan;
	ListCell   *option;
	bool		dbistemplate = false;
	bool		dballowconnections = true;
	int			dbconnlimit = -1;
	DefElem    *distemplate = NULL;
	DefElem    *dallowconnections = NULL;
	DefElem    *dconnlimit = NULL;
	DefElem    *dtablespace = NULL;
	Datum		new_record[Natts_pg_database];
	bool		new_record_nulls[Natts_pg_database];
	bool		new_record_repl[Natts_pg_database];

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "is_template") == 0)
		{
			if (distemplate)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			distemplate = defel;
		}
		else if (strcmp(defel->defname, "allow_connections") == 0)
		{
			if (dallowconnections)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dallowconnections = defel;
		}
		else if (strcmp(defel->defname, "connection_limit") == 0)
		{
			if (dconnlimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dconnlimit = defel;
		}
		else if (strcmp(defel->defname, "tablespace") == 0)
		{
			if (dtablespace)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dtablespace = defel;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized", defel->defname)));
	}

	if (dtablespace)
	{
		/*
		 * While the SET TABLESPACE syntax doesn't allow any other options,
		 * somebody could write "WITH TABLESPACE ...".  Forbid any other
		 * options from being specified in that case.
		 */
		if (list_length(stmt->options) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			   errmsg("option \"%s\" cannot be specified with other options",
					  dtablespace->defname)));
		/* this case isn't allowed within a transaction block */
		PreventTransactionChain(isTopLevel, "ALTER DATABASE SET TABLESPACE");
		movedb(stmt->dbname, defGetString(dtablespace));
		return InvalidOid;
	}

	if (distemplate && distemplate->arg)
		dbistemplate = defGetBoolean(distemplate);
	if (dallowconnections && dallowconnections->arg)
		dballowconnections = defGetBoolean(dallowconnections);
	if (dconnlimit && dconnlimit->arg)
	{
		dbconnlimit = defGetInt32(dconnlimit);
		if (dbconnlimit < -1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid connection limit: %d", dbconnlimit)));
	}

	/*
	 * Get the old tuple.  We don't need a lock on the database per se,
	 * because we're not going to do anything that would mess up incoming
	 * connections.
	 */
	rel = heap_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  NULL, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", stmt->dbname)));

	dboid = HeapTupleGetOid(tuple);

	if (!pg_database_ownercheck(HeapTupleGetOid(tuple), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   stmt->dbname);

	/*
	 * In order to avoid getting locked out and having to go through
	 * standalone mode, we refuse to disallow connections to the database
	 * we're currently connected to.  Lockout can still happen with concurrent
	 * sessions but the likeliness of that is not high enough to worry about.
	 */
	if (!dballowconnections && dboid == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot disallow connections for current database")));

	/*
	 * Build an updated tuple, perusing the information just obtained
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));
	MemSet(new_record_repl, false, sizeof(new_record_repl));

	if (distemplate)
	{
		new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(dbistemplate);
		new_record_repl[Anum_pg_database_datistemplate - 1] = true;
	}
	if (dallowconnections)
	{
		new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(dballowconnections);
		new_record_repl[Anum_pg_database_datallowconn - 1] = true;
	}
	if (dconnlimit)
	{
		new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
		new_record_repl[Anum_pg_database_datconnlimit - 1] = true;
	}

	newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), new_record,
								 new_record_nulls, new_record_repl);
	simple_heap_update(rel, &tuple->t_self, newtuple);

	/* Update indexes */
	CatalogUpdateIndexes(rel, newtuple);

	InvokeObjectPostAlterHook(DatabaseRelationId,
							  HeapTupleGetOid(newtuple), 0);

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	heap_close(rel, NoLock);

	return dboid;
}


/*
 * ALTER DATABASE name SET ...
 */
Oid
AlterDatabaseSet(AlterDatabaseSetStmt *stmt)
{
	Oid			datid = get_database_oid(stmt->dbname, false);

	/*
	 * Obtain a lock on the database and make sure it didn't go away in the
	 * meantime.
	 */
	shdepLockAndCheckObject(DatabaseRelationId, datid);

	if (!pg_database_ownercheck(datid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   stmt->dbname);

	AlterSetting(datid, InvalidOid, stmt->setstmt);

	UnlockSharedObject(DatabaseRelationId, datid, 0, AccessShareLock);

	return datid;
}


/*
 * ALTER DATABASE name OWNER TO newowner
 */
ObjectAddress
AlterDatabaseOwner(const char *dbname, Oid newOwnerId)
{
	Oid			db_id;
	HeapTuple	tuple;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	Form_pg_database datForm;
	ObjectAddress address;

	/*
	 * Get the old tuple.  We don't need a lock on the database per se,
	 * because we're not going to do anything that would mess up incoming
	 * connections.
	 */
	rel = heap_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  NULL, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	db_id = HeapTupleGetOid(tuple);
	datForm = (Form_pg_database) GETSTRUCT(tuple);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is to be consistent with other
	 * objects.
	 */
	if (datForm->datdba != newOwnerId)
	{
		Datum		repl_val[Natts_pg_database];
		bool		repl_null[Natts_pg_database];
		bool		repl_repl[Natts_pg_database];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Otherwise, must be owner of the existing object */
		if (!pg_database_ownercheck(HeapTupleGetOid(tuple), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
						   dbname);

		/* Must be able to become new owner */
		check_is_member_of_role(GetUserId(), newOwnerId);

		/*
		 * must have createdb rights
		 *
		 * NOTE: This is different from other alter-owner checks in that the
		 * current user is checked for createdb privileges instead of the
		 * destination owner.  This is consistent with the CREATE case for
		 * databases.  Because superusers will always have this right, we need
		 * no special case for them.
		 */
		if (!have_createdb_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				   errmsg("permission denied to change owner of database")));

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_database_datdba - 1] = true;
		repl_val[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = heap_getattr(tuple,
								Anum_pg_database_datacl,
								RelationGetDescr(rel),
								&isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 datForm->datdba, newOwnerId);
			repl_repl[Anum_pg_database_datacl - 1] = true;
			repl_val[Anum_pg_database_datacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), repl_val, repl_null, repl_repl);
		simple_heap_update(rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(rel, newtuple);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(DatabaseRelationId, HeapTupleGetOid(tuple),
								newOwnerId);
	}

	InvokeObjectPostAlterHook(DatabaseRelationId, HeapTupleGetOid(tuple), 0);

	ObjectAddressSet(address, DatabaseRelationId, db_id);

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	heap_close(rel, NoLock);

	return address;
}


/*
 * Helper functions
 */

/*
 * Look up info about the database named "name".  If the database exists,
 * obtain the specified lock type on it, fill in any of the remaining
 * parameters that aren't NULL, and return TRUE.  If no such database,
 * return FALSE.
 */
static bool
get_db_info(const char *name, LOCKMODE lockmode,
			Oid *dbIdP, Oid *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP,
			Oid *dbLastSysOidP, TransactionId *dbFrozenXidP,
			MultiXactId *dbMinMultiP,
			Oid *dbTablespace, char **dbCollate, char **dbCtype)
{
	bool		result = false;
	Relation	relation;

	AssertArg(name);

	/* Caller may wish to grab a better lock on pg_database beforehand... */
	relation = heap_open(DatabaseRelationId, AccessShareLock);

	/*
	 * Loop covers the rare case where the database is renamed before we can
	 * lock it.  We try again just in case we can find a new one of the same
	 * name.
	 */
	for (;;)
	{
		ScanKeyData scanKey;
		SysScanDesc scan;
		HeapTuple	tuple;
		Oid			dbOid;

		/*
		 * there's no syscache for database-indexed-by-name, so must do it the
		 * hard way
		 */
		ScanKeyInit(&scanKey,
					Anum_pg_database_datname,
					BTEqualStrategyNumber, F_NAMEEQ,
					NameGetDatum(name));

		scan = systable_beginscan(relation, DatabaseNameIndexId, true,
								  NULL, 1, &scanKey);

		tuple = systable_getnext(scan);

		if (!HeapTupleIsValid(tuple))
		{
			/* definitely no database of that name */
			systable_endscan(scan);
			break;
		}

		dbOid = HeapTupleGetOid(tuple);

		systable_endscan(scan);

		/*
		 * Now that we have a database OID, we can try to lock the DB.
		 */
		if (lockmode != NoLock)
			LockSharedObject(DatabaseRelationId, dbOid, 0, lockmode);

		/*
		 * And now, re-fetch the tuple by OID.  If it's still there and still
		 * the same name, we win; else, drop the lock and loop back to try
		 * again.
		 */
		tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbOid));
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);

			if (strcmp(name, NameStr(dbform->datname)) == 0)
			{
				/* oid of the database */
				if (dbIdP)
					*dbIdP = dbOid;
				/* oid of the owner */
				if (ownerIdP)
					*ownerIdP = dbform->datdba;
				/* character encoding */
				if (encodingP)
					*encodingP = dbform->encoding;
				/* allowed as template? */
				if (dbIsTemplateP)
					*dbIsTemplateP = dbform->datistemplate;
				/* allowing connections? */
				if (dbAllowConnP)
					*dbAllowConnP = dbform->datallowconn;
				/* last system OID used in database */
				if (dbLastSysOidP)
					*dbLastSysOidP = dbform->datlastsysoid;
				/* limit of frozen XIDs */
				if (dbFrozenXidP)
					*dbFrozenXidP = dbform->datfrozenxid;
				/* minimum MultixactId */
				if (dbMinMultiP)
					*dbMinMultiP = dbform->datminmxid;
				/* default tablespace for this database */
				if (dbTablespace)
					*dbTablespace = dbform->dattablespace;
				/* default locale settings for this database */
				if (dbCollate)
					*dbCollate = pstrdup(NameStr(dbform->datcollate));
				if (dbCtype)
					*dbCtype = pstrdup(NameStr(dbform->datctype));
				ReleaseSysCache(tuple);
				result = true;
				break;
			}
			/* can only get here if it was just renamed */
			ReleaseSysCache(tuple);
		}

		if (lockmode != NoLock)
			UnlockSharedObject(DatabaseRelationId, dbOid, 0, lockmode);
	}

	heap_close(relation, AccessShareLock);

	return result;
}

/* Check if current user has createdb privileges */
static bool
have_createdb_privilege(void)
{
	bool		result = false;
	HeapTuple	utup;

	/* Superusers can always do everything */
	if (superuser())
		return true;

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(GetUserId()));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolcreatedb;
		ReleaseSysCache(utup);
	}
	return result;
}

/*
 * Remove tablespace directories
 *
 * We don't know what tablespaces db_id is using, so iterate through all
 * tablespaces removing <tablespace>/db_id
 */
static void
remove_dbtablespaces(Oid db_id)
{
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tuple;

	rel = heap_open(TableSpaceRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			dsttablespace = HeapTupleGetOid(tuple);
		char	   *dstpath;
		struct stat st;

		/* Don't mess with the global tablespace */
		if (dsttablespace == GLOBALTABLESPACE_OID)
			continue;

		dstpath = GetDatabasePath(db_id, dsttablespace);

		if (lstat(dstpath, &st) < 0 || !S_ISDIR(st.st_mode))
		{
			/* Assume we can ignore it */
			pfree(dstpath);
			continue;
		}

		if (!rmtree(dstpath, true))
			ereport(WARNING,
					(errmsg("some useless files may be left behind in old database directory \"%s\"",
							dstpath)));

		/* Record the filesystem change in XLOG */
		{
			xl_dbase_drop_rec xlrec;

			xlrec.db_id = db_id;
			xlrec.tablespace_id = dsttablespace;

			XLogBeginInsert();
			XLogRegisterData((char *) &xlrec, sizeof(xl_dbase_drop_rec));

			(void) XLogInsert(RM_DBASE_ID,
							  XLOG_DBASE_DROP | XLR_SPECIAL_REL_UPDATE);
		}

		pfree(dstpath);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);
}

/*
 * Check for existing files that conflict with a proposed new DB OID;
 * return TRUE if there are any
 *
 * If there were a subdirectory in any tablespace matching the proposed new
 * OID, we'd get a create failure due to the duplicate name ... and then we'd
 * try to remove that already-existing subdirectory during the cleanup in
 * remove_dbtablespaces.  Nuking existing files seems like a bad idea, so
 * instead we make this extra check before settling on the OID of the new
 * database.  This exactly parallels what GetNewRelFileNode() does for table
 * relfilenode values.
 */
static bool
check_db_file_conflict(Oid db_id)
{
	bool		result = false;
	Relation	rel;
	HeapScanDesc scan;
	HeapTuple	tuple;

	rel = heap_open(TableSpaceRelationId, AccessShareLock);
	scan = heap_beginscan_catalog(rel, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			dsttablespace = HeapTupleGetOid(tuple);
		char	   *dstpath;
		struct stat st;

		/* Don't mess with the global tablespace */
		if (dsttablespace == GLOBALTABLESPACE_OID)
			continue;

		dstpath = GetDatabasePath(db_id, dsttablespace);

		if (lstat(dstpath, &st) == 0)
		{
			/* Found a conflicting file (or directory, whatever) */
			pfree(dstpath);
			result = true;
			break;
		}

		pfree(dstpath);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	return result;
}

/*
 * Issue a suitable errdetail message for a busy database
 */
static int
errdetail_busy_db(int notherbackends, int npreparedxacts)
{
	if (notherbackends > 0 && npreparedxacts > 0)

		/*
		 * We don't deal with singular versus plural here, since gettext
		 * doesn't support multiple plurals in one string.
		 */
		errdetail("There are %d other session(s) and %d prepared transaction(s) using the database.",
				  notherbackends, npreparedxacts);
	else if (notherbackends > 0)
		errdetail_plural("There is %d other session using the database.",
						 "There are %d other sessions using the database.",
						 notherbackends,
						 notherbackends);
	else
		errdetail_plural("There is %d prepared transaction using the database.",
					"There are %d prepared transactions using the database.",
						 npreparedxacts,
						 npreparedxacts);
	return 0;					/* just to keep ereport macro happy */
}

/*
 * get_database_oid - given a database name, look up the OID
 *
 * If missing_ok is false, throw an error if database name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_database_oid(const char *dbname, bool missing_ok)
{
	Relation	pg_database;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	dbtuple;
	Oid			oid;

	/*
	 * There's no syscache for pg_database indexed by name, so we must look
	 * the hard way.
	 */
	pg_database = heap_open(DatabaseRelationId, AccessShareLock);
	ScanKeyInit(&entry[0],
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	scan = systable_beginscan(pg_database, DatabaseNameIndexId, true,
							  NULL, 1, entry);

	dbtuple = systable_getnext(scan);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(dbtuple))
		oid = HeapTupleGetOid(dbtuple);
	else
		oid = InvalidOid;

	systable_endscan(scan);
	heap_close(pg_database, AccessShareLock);

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist",
						dbname)));

	return oid;
}


/*
 * get_database_name - given a database OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such database.
 */
char *
get_database_name(Oid dbid)
{
	HeapTuple	dbtuple;
	char	   *result;

	dbtuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
	if (HeapTupleIsValid(dbtuple))
	{
		result = pstrdup(NameStr(((Form_pg_database) GETSTRUCT(dbtuple))->datname));
		ReleaseSysCache(dbtuple);
	}
	else
		result = NULL;

	return result;
}

/*
 * DATABASE resource manager's routines
 */
void
dbase_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in dbase records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_DBASE_CREATE)
	{
		xl_dbase_create_rec *xlrec = (xl_dbase_create_rec *) XLogRecGetData(record);
		char	   *src_path;
		char	   *dst_path;
		struct stat st;

		src_path = GetDatabasePath(xlrec->src_db_id, xlrec->src_tablespace_id);
		dst_path = GetDatabasePath(xlrec->db_id, xlrec->tablespace_id);

		/*
		 * Our theory for replaying a CREATE is to forcibly drop the target
		 * subdirectory if present, then re-copy the source data. This may be
		 * more work than needed, but it is simple to implement.
		 */
		if (stat(dst_path, &st) == 0 && S_ISDIR(st.st_mode))
		{
			if (!rmtree(dst_path, true))
				/* If this failed, copydir() below is going to error. */
				ereport(WARNING,
						(errmsg("some useless files may be left behind in old database directory \"%s\"",
								dst_path)));
		}

		/*
		 * Force dirty buffers out to disk, to ensure source database is
		 * up-to-date for the copy.
		 */
		FlushDatabaseBuffers(xlrec->src_db_id);

		/*
		 * Copy this subdirectory to the new location
		 *
		 * We don't need to copy subdirectories
		 */
		copydir(src_path, dst_path, false);
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec = (xl_dbase_drop_rec *) XLogRecGetData(record);
		char	   *dst_path;

		dst_path = GetDatabasePath(xlrec->db_id, xlrec->tablespace_id);

		if (InHotStandby)
		{
			/*
			 * Lock database while we resolve conflicts to ensure that
			 * InitPostgres() cannot fully re-execute concurrently. This
			 * avoids backends re-connecting automatically to same database,
			 * which can happen in some cases.
			 */
			LockSharedObjectForSession(DatabaseRelationId, xlrec->db_id, 0, AccessExclusiveLock);
			ResolveRecoveryConflictWithDatabase(xlrec->db_id);
		}

		/* Drop pages for this database that are in the shared buffer cache */
		DropDatabaseBuffers(xlrec->db_id);

		/* Also, clean out any fsync requests that might be pending in md.c */
		ForgetDatabaseFsyncRequests(xlrec->db_id);

		/* Clean out the xlog relcache too */
		XLogDropDatabase(xlrec->db_id);

		/* And remove the physical files */
		if (!rmtree(dst_path, true))
			ereport(WARNING,
					(errmsg("some useless files may be left behind in old database directory \"%s\"",
							dst_path)));

		if (InHotStandby)
		{
			/*
			 * Release locks prior to commit. XXX There is a race condition
			 * here that may allow backends to reconnect, but the window for
			 * this is small because the gap between here and commit is mostly
			 * fairly small and it is unlikely that people will be dropping
			 * databases that we are trying to connect to anyway.
			 */
			UnlockSharedObjectForSession(DatabaseRelationId, xlrec->db_id, 0, AccessExclusiveLock);
		}
	}
	else
		elog(PANIC, "dbase_redo: unknown op code %u", info);
}
