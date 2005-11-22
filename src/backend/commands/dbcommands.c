/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		Database management commands (create/drop database).
 *
 * Note: database creation/destruction commands take ExclusiveLock on
 * pg_database to ensure that no two proceed in parallel.  We must use
 * at least this level of locking to ensure that no two backends try to
 * write the flat-file copy of pg_database at once.  We avoid using
 * AccessExclusiveLock since there's no need to lock out ordinary readers
 * of pg_database.
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/dbcommands.c,v 1.173.2.1 2005/11/22 18:23:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/flatfiles.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* non-export function prototypes */
static bool get_db_info(const char *name, Oid *dbIdP, Oid *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP,
			Oid *dbLastSysOidP,
			TransactionId *dbVacuumXidP, TransactionId *dbFrozenXidP,
			Oid *dbTablespace);
static bool have_createdb_privilege(void);
static void remove_dbtablespaces(Oid db_id);


/*
 * CREATE DATABASE
 */
void
createdb(const CreatedbStmt *stmt)
{
	HeapScanDesc scan;
	Relation	rel;
	Oid			src_dboid;
	Oid			src_owner;
	int			src_encoding;
	bool		src_istemplate;
	bool		src_allowconn;
	Oid			src_lastsysoid;
	TransactionId src_vacuumxid;
	TransactionId src_frozenxid;
	Oid			src_deftablespace;
	volatile Oid dst_deftablespace;
	volatile Relation pg_database_rel;
	HeapTuple	tuple;
	TupleDesc	pg_database_dsc;
	Datum		new_record[Natts_pg_database];
	char		new_record_nulls[Natts_pg_database];
	Oid			dboid;
	volatile Oid datdba;
	ListCell   *option;
	DefElem    *dtablespacename = NULL;
	DefElem    *downer = NULL;
	DefElem    *dtemplate = NULL;
	DefElem    *dencoding = NULL;
	DefElem    *dconnlimit = NULL;
	char	   *dbname = stmt->dbname;
	char	   *dbowner = NULL;
	const char *dbtemplate = NULL;
	volatile int encoding = -1;
	volatile int dbconnlimit = -1;

	/* don't call this in a transaction block */
	PreventTransactionChain((void *) stmt, "CREATE DATABASE");

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
		else if (strcmp(defel->defname, "connectionlimit") == 0)
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
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (downer && downer->arg)
		dbowner = strVal(downer->arg);
	if (dtemplate && dtemplate->arg)
		dbtemplate = strVal(dtemplate->arg);
	if (dencoding && dencoding->arg)
	{
		const char *encoding_name;

		if (IsA(dencoding->arg, Integer))
		{
			encoding = intVal(dencoding->arg);
			encoding_name = pg_encoding_to_char(encoding);
			if (strcmp(encoding_name, "") == 0 ||
				pg_valid_server_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%d is not a valid encoding code",
								encoding)));
		}
		else if (IsA(dencoding->arg, String))
		{
			encoding_name = strVal(dencoding->arg);
			if (pg_valid_server_encoding(encoding_name) < 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("%s is not a valid encoding name",
								encoding_name)));
			encoding = pg_char_to_encoding(encoding_name);
		}
		else
			elog(ERROR, "unrecognized node type: %d",
				 nodeTag(dencoding->arg));
	}
	if (dconnlimit && dconnlimit->arg)
		dbconnlimit = intVal(dconnlimit->arg);

	/* obtain OID of proposed owner */
	if (dbowner)
		datdba = get_roleid_checked(dbowner);
	else
		datdba = GetUserId();

	/*
	 * To create a database, must have createdb privilege and must be able to
	 * become the target role (this does not imply that the target role itself
	 * must have createdb privilege).  The latter provision guards against
	 * "giveaway" attacks.	Note that a superuser will always have both of
	 * these privileges a fortiori.
	 */
	if (!have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create database")));

	check_is_member_of_role(GetUserId(), datdba);

	/*
	 * Check for db name conflict.	There is a race condition here, since
	 * another backend could create the same DB name before we commit.
	 * However, holding an exclusive lock on pg_database for the whole time we
	 * are copying the source database doesn't seem like a good idea, so
	 * accept possibility of race to create.  We will check again after we
	 * grab the exclusive lock.
	 */
	if (get_db_info(dbname, NULL, NULL, NULL,
					NULL, NULL, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", dbname)));

	/*
	 * Lookup database (template) to be cloned.
	 */
	if (!dbtemplate)
		dbtemplate = "template1";		/* Default template database name */

	if (!get_db_info(dbtemplate, &src_dboid, &src_owner, &src_encoding,
					 &src_istemplate, &src_allowconn, &src_lastsysoid,
					 &src_vacuumxid, &src_frozenxid, &src_deftablespace))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
			 errmsg("template database \"%s\" does not exist", dbtemplate)));

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

	/*
	 * The source DB can't have any active backends, except this one
	 * (exception is to allow CREATE DB while connected to template1).
	 * Otherwise we might copy inconsistent data.  This check is not
	 * bulletproof, since someone might connect while we are copying...
	 */
	if (DatabaseHasActiveBackends(src_dboid, true))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
			errmsg("source database \"%s\" is being accessed by other users",
				   dbtemplate)));

	/* If encoding is defaulted, use source's encoding */
	if (encoding < 0)
		encoding = src_encoding;

	/* Some encodings are client only */
	if (!PG_VALID_BE_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("invalid server encoding %d", encoding)));

	/* Resolve default tablespace for new database */
	if (dtablespacename && dtablespacename->arg)
	{
		char	   *tablespacename;
		AclResult	aclresult;

		tablespacename = strVal(dtablespacename->arg);
		dst_deftablespace = get_tablespace_oid(tablespacename);
		if (!OidIsValid(dst_deftablespace))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							tablespacename)));
		/* check permissions */
		aclresult = pg_tablespace_aclcheck(dst_deftablespace, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_TABLESPACE,
						   tablespacename);

		/*
		 * If we are trying to change the default tablespace of the template,
		 * we require that the template not have any files in the new default
		 * tablespace.	This is necessary because otherwise the copied
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
	 * Normally we mark the new database with the same datvacuumxid and
	 * datfrozenxid as the source.	However, if the source is not allowing
	 * connections then we assume it is fully frozen, and we can set the
	 * current transaction ID as the xid limits.  This avoids immediately
	 * starting to generate warnings after cloning template0.
	 */
	if (!src_allowconn)
		src_vacuumxid = src_frozenxid = GetCurrentTransactionId();

	/*
	 * Preassign OID for pg_database tuple, so that we can compute db path. We
	 * have to open pg_database to do this, but we don't want to take
	 * ExclusiveLock yet, so just do it and close again.
	 */
	pg_database_rel = heap_open(DatabaseRelationId, AccessShareLock);
	dboid = GetNewOid(pg_database_rel);
	heap_close(pg_database_rel, AccessShareLock);
	pg_database_rel = NULL;

	/*
	 * Force dirty buffers out to disk, to ensure source database is
	 * up-to-date for the copy.  (We really only need to flush buffers for the
	 * source database, but bufmgr.c provides no API for that.)
	 */
	BufferSync();

	/*
	 * Once we start copying subdirectories, we need to be able to clean 'em
	 * up if we fail.  Establish a TRY block to make sure this happens. (This
	 * is not a 100% solution, because of the possibility of failure during
	 * transaction commit after we leave this routine, but it should handle
	 * most scenarios.)
	 */
	PG_TRY();
	{
		/*
		 * Iterate through all tablespaces of the template database, and copy
		 * each one to the new database.
		 */
		rel = heap_open(TableSpaceRelationId, AccessShareLock);
		scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
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
				XLogRecData rdata[1];

				xlrec.db_id = dboid;
				xlrec.tablespace_id = dsttablespace;
				xlrec.src_db_id = src_dboid;
				xlrec.src_tablespace_id = srctablespace;

				rdata[0].data = (char *) &xlrec;
				rdata[0].len = sizeof(xl_dbase_create_rec);
				rdata[0].buffer = InvalidBuffer;
				rdata[0].next = NULL;

				(void) XLogInsert(RM_DBASE_ID, XLOG_DBASE_CREATE, rdata);
			}
		}
		heap_endscan(scan);
		heap_close(rel, AccessShareLock);

		/*
		 * Now OK to grab exclusive lock on pg_database.
		 */
		pg_database_rel = heap_open(DatabaseRelationId, ExclusiveLock);

		/* Check to see if someone else created same DB name meanwhile. */
		if (get_db_info(dbname, NULL, NULL, NULL,
						NULL, NULL, NULL, NULL, NULL, NULL))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_DATABASE),
					 errmsg("database \"%s\" already exists", dbname)));

		/*
		 * Insert a new tuple into pg_database
		 */
		pg_database_dsc = RelationGetDescr(pg_database_rel);

		/* Form tuple */
		MemSet(new_record, 0, sizeof(new_record));
		MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));

		new_record[Anum_pg_database_datname - 1] =
			DirectFunctionCall1(namein, CStringGetDatum(dbname));
		new_record[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(datdba);
		new_record[Anum_pg_database_encoding - 1] = Int32GetDatum(encoding);
		new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(false);
		new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(true);
		new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
		new_record[Anum_pg_database_datlastsysoid - 1] = ObjectIdGetDatum(src_lastsysoid);
		new_record[Anum_pg_database_datvacuumxid - 1] = TransactionIdGetDatum(src_vacuumxid);
		new_record[Anum_pg_database_datfrozenxid - 1] = TransactionIdGetDatum(src_frozenxid);
		new_record[Anum_pg_database_dattablespace - 1] = ObjectIdGetDatum(dst_deftablespace);

		/*
		 * We deliberately set datconfig and datacl to defaults (NULL), rather
		 * than copying them from the template database.  Copying datacl would
		 * be a bad idea when the owner is not the same as the template's
		 * owner. It's more debatable whether datconfig should be copied.
		 */
		new_record_nulls[Anum_pg_database_datconfig - 1] = 'n';
		new_record_nulls[Anum_pg_database_datacl - 1] = 'n';

		tuple = heap_formtuple(pg_database_dsc, new_record, new_record_nulls);

		HeapTupleSetOid(tuple, dboid);	/* override heap_insert's OID
										 * selection */

		simple_heap_insert(pg_database_rel, tuple);

		/* Update indexes */
		CatalogUpdateIndexes(pg_database_rel, tuple);

		/* Register owner dependency */
		recordDependencyOnOwner(DatabaseRelationId, dboid, datdba);

		/* Create pg_shdepend entries for objects within database */
		copyTemplateDependencies(src_dboid, dboid);

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
		RequestCheckpoint(true, false);

		/*
		 * Set flag to update flat database file at commit.
		 */
		database_file_update_needed();
	}
	PG_CATCH();
	{
		/* Don't hold pg_database lock while doing recursive remove */
		if (pg_database_rel != NULL)
			heap_close(pg_database_rel, ExclusiveLock);

		/* Throw away any successfully copied subdirectories */
		remove_dbtablespaces(dboid);

		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Close pg_database, but keep exclusive lock till commit */
	/* This has to be outside the PG_TRY */
	heap_close(pg_database_rel, NoLock);
}


/*
 * DROP DATABASE
 */
void
dropdb(const char *dbname)
{
	Oid			db_id;
	bool		db_istemplate;
	Relation	pgdbrel;
	SysScanDesc pgdbscan;
	ScanKeyData key;
	HeapTuple	tup;

	PreventTransactionChain((void *) dbname, "DROP DATABASE");

	AssertArg(dbname);

	if (strcmp(dbname, get_database_name(MyDatabaseId)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot drop the currently open database")));

	/*
	 * Obtain exclusive lock on pg_database.  We need this to ensure that no
	 * new backend starts up in the target database while we are deleting it.
	 * (Actually, a new backend might still manage to start up, because it
	 * isn't able to lock pg_database while starting.  But it will detect its
	 * error in ReverifyMyDatabase and shut down before any serious damage is
	 * done.  See postinit.c.)
	 *
	 * An ExclusiveLock, rather than AccessExclusiveLock, is sufficient since
	 * ReverifyMyDatabase takes RowShareLock.  This allows ordinary readers of
	 * pg_database to proceed in parallel.
	 */
	pgdbrel = heap_open(DatabaseRelationId, ExclusiveLock);

	if (!get_db_info(dbname, &db_id, NULL, NULL,
					 &db_istemplate, NULL, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	if (!pg_database_ownercheck(db_id, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   dbname);

	/*
	 * Disallow dropping a DB that is marked istemplate.  This is just to
	 * prevent people from accidentally dropping template0 or template1; they
	 * can do so if they're really determined ...
	 */
	if (db_istemplate)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("cannot drop a template database")));

	/*
	 * Check for active backends in the target database.
	 */
	if (DatabaseHasActiveBackends(db_id, false))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						dbname)));

	/*
	 * Find the database's tuple by OID (should be unique).
	 */
	ScanKeyInit(&key,
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(db_id));

	pgdbscan = systable_beginscan(pgdbrel, DatabaseOidIndexId, true,
								  SnapshotNow, 1, &key);

	tup = systable_getnext(pgdbscan);
	if (!HeapTupleIsValid(tup))
	{
		/*
		 * This error should never come up since the existence of the database
		 * is checked earlier
		 */
		elog(ERROR, "database \"%s\" doesn't exist despite earlier reports to the contrary",
			 dbname);
	}

	/* Remove the database's tuple from pg_database */
	simple_heap_delete(pgdbrel, &tup->t_self);

	systable_endscan(pgdbscan);

	/*
	 * Delete any comments associated with the database
	 *
	 * NOTE: this is probably dead code since any such comments should have
	 * been in that database, not mine.
	 */
	DeleteComments(db_id, DatabaseRelationId, 0);

	/*
	 * Remove shared dependency references for the database.
	 */
	dropDatabaseDependencies(db_id);

	/*
	 * Drop pages for this database that are in the shared buffer cache. This
	 * is important to ensure that no remaining backend tries to write out a
	 * dirty buffer to the dead database later...
	 */
	DropBuffers(db_id);

	/*
	 * Also, clean out any entries in the shared free space map.
	 */
	FreeSpaceMapForgetDatabase(db_id);

	/*
	 * On Windows, force a checkpoint so that the bgwriter doesn't hold any
	 * open files, which would cause rmdir() to fail.
	 */
#ifdef WIN32
	RequestCheckpoint(true, false);
#endif

	/*
	 * Remove all tablespace subdirs belonging to the database.
	 */
	remove_dbtablespaces(db_id);

	/* Close pg_database, but keep exclusive lock till commit */
	heap_close(pgdbrel, NoLock);

	/*
	 * Set flag to update flat database file at commit.
	 */
	database_file_update_needed();
}


/*
 * Rename database
 */
void
RenameDatabase(const char *oldname, const char *newname)
{
	HeapTuple	tup,
				newtup;
	Relation	rel;
	SysScanDesc scan,
				scan2;
	ScanKeyData key,
				key2;

	/*
	 * Obtain ExclusiveLock so that no new session gets started while the
	 * rename is in progress.
	 */
	rel = heap_open(DatabaseRelationId, ExclusiveLock);

	ScanKeyInit(&key,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(oldname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  SnapshotNow, 1, &key);

	tup = systable_getnext(scan);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", oldname)));

	/*
	 * XXX Client applications probably store the current database somewhere,
	 * so renaming it could cause confusion.  On the other hand, there may not
	 * be an actual problem besides a little confusion, so think about this
	 * and decide.
	 */
	if (HeapTupleGetOid(tup) == MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("current database may not be renamed")));

	/*
	 * Make sure the database does not have active sessions.  Might not be
	 * necessary, but it's consistent with other database operations.
	 */
	if (DatabaseHasActiveBackends(HeapTupleGetOid(tup), false))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("database \"%s\" is being accessed by other users",
						oldname)));

	/* make sure the new name doesn't exist */
	ScanKeyInit(&key2,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(newname));
	scan2 = systable_beginscan(rel, DatabaseNameIndexId, true,
							   SnapshotNow, 1, &key2);
	if (HeapTupleIsValid(systable_getnext(scan2)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", newname)));
	systable_endscan(scan2);

	/* must be owner */
	if (!pg_database_ownercheck(HeapTupleGetOid(tup), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   oldname);

	/* must have createdb rights */
	if (!have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to rename database")));

	/* rename */
	newtup = heap_copytuple(tup);
	namestrcpy(&(((Form_pg_database) GETSTRUCT(newtup))->datname), newname);
	simple_heap_update(rel, &newtup->t_self, newtup);
	CatalogUpdateIndexes(rel, newtup);

	systable_endscan(scan);

	/* Close pg_database, but keep exclusive lock till commit */
	heap_close(rel, NoLock);

	/*
	 * Set flag to update flat database file at commit.
	 */
	database_file_update_needed();
}


/*
 * ALTER DATABASE name ...
 */
void
AlterDatabase(AlterDatabaseStmt *stmt)
{
	Relation	rel;
	HeapTuple	tuple,
				newtuple;
	ScanKeyData scankey;
	SysScanDesc scan;
	ListCell   *option;
	int			connlimit = -1;
	DefElem    *dconnlimit = NULL;
	Datum		new_record[Natts_pg_database];
	char		new_record_nulls[Natts_pg_database];
	char		new_record_repl[Natts_pg_database];

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "connectionlimit") == 0)
		{
			if (dconnlimit)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dconnlimit = defel;
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (dconnlimit)
		connlimit = intVal(dconnlimit->arg);

	/*
	 * We don't need ExclusiveLock since we aren't updating the flat file.
	 */
	rel = heap_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  SnapshotNow, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", stmt->dbname)));

	if (!pg_database_ownercheck(HeapTupleGetOid(tuple), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   stmt->dbname);

	/*
	 * Build an updated tuple, perusing the information just obtained
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	MemSet(new_record_repl, ' ', sizeof(new_record_repl));

	if (dconnlimit)
	{
		new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(connlimit);
		new_record_repl[Anum_pg_database_datconnlimit - 1] = 'r';
	}

	newtuple = heap_modifytuple(tuple, RelationGetDescr(rel), new_record,
								new_record_nulls, new_record_repl);
	simple_heap_update(rel, &tuple->t_self, newtuple);

	/* Update indexes */
	CatalogUpdateIndexes(rel, newtuple);

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	heap_close(rel, NoLock);

	/*
	 * We don't bother updating the flat file since the existing options for
	 * ALTER DATABASE don't affect it.
	 */
}


/*
 * ALTER DATABASE name SET ...
 */
void
AlterDatabaseSet(AlterDatabaseSetStmt *stmt)
{
	char	   *valuestr;
	HeapTuple	tuple,
				newtuple;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	Datum		repl_val[Natts_pg_database];
	char		repl_null[Natts_pg_database];
	char		repl_repl[Natts_pg_database];

	valuestr = flatten_set_variable_args(stmt->variable, stmt->value);

	/*
	 * We don't need ExclusiveLock since we aren't updating the flat file.
	 */
	rel = heap_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  SnapshotNow, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", stmt->dbname)));

	if (!pg_database_ownercheck(HeapTupleGetOid(tuple), GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   stmt->dbname);

	MemSet(repl_repl, ' ', sizeof(repl_repl));
	repl_repl[Anum_pg_database_datconfig - 1] = 'r';

	if (strcmp(stmt->variable, "all") == 0 && valuestr == NULL)
	{
		/* RESET ALL */
		repl_null[Anum_pg_database_datconfig - 1] = 'n';
		repl_val[Anum_pg_database_datconfig - 1] = (Datum) 0;
	}
	else
	{
		Datum		datum;
		bool		isnull;
		ArrayType  *a;

		repl_null[Anum_pg_database_datconfig - 1] = ' ';

		datum = heap_getattr(tuple, Anum_pg_database_datconfig,
							 RelationGetDescr(rel), &isnull);

		a = isnull ? NULL : DatumGetArrayTypeP(datum);

		if (valuestr)
			a = GUCArrayAdd(a, stmt->variable, valuestr);
		else
			a = GUCArrayDelete(a, stmt->variable);

		if (a)
			repl_val[Anum_pg_database_datconfig - 1] = PointerGetDatum(a);
		else
			repl_null[Anum_pg_database_datconfig - 1] = 'n';
	}

	newtuple = heap_modifytuple(tuple, RelationGetDescr(rel), repl_val, repl_null, repl_repl);
	simple_heap_update(rel, &tuple->t_self, newtuple);

	/* Update indexes */
	CatalogUpdateIndexes(rel, newtuple);

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	heap_close(rel, NoLock);

	/*
	 * We don't bother updating the flat file since ALTER DATABASE SET doesn't
	 * affect it.
	 */
}


/*
 * ALTER DATABASE name OWNER TO newowner
 */
void
AlterDatabaseOwner(const char *dbname, Oid newOwnerId)
{
	HeapTuple	tuple;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	Form_pg_database datForm;

	/*
	 * We don't need ExclusiveLock since we aren't updating the flat file.
	 */
	rel = heap_open(DatabaseRelationId, RowExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(dbname));
	scan = systable_beginscan(rel, DatabaseNameIndexId, true,
							  SnapshotNow, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	datForm = (Form_pg_database) GETSTRUCT(tuple);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is to be consistent with other
	 * objects.
	 */
	if (datForm->datdba != newOwnerId)
	{
		Datum		repl_val[Natts_pg_database];
		char		repl_null[Natts_pg_database];
		char		repl_repl[Natts_pg_database];
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

		memset(repl_null, ' ', sizeof(repl_null));
		memset(repl_repl, ' ', sizeof(repl_repl));

		repl_repl[Anum_pg_database_datdba - 1] = 'r';
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
			repl_repl[Anum_pg_database_datacl - 1] = 'r';
			repl_val[Anum_pg_database_datacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modifytuple(tuple, RelationGetDescr(rel), repl_val, repl_null, repl_repl);
		simple_heap_update(rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(rel, newtuple);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(DatabaseRelationId, HeapTupleGetOid(tuple),
								newOwnerId);
	}

	systable_endscan(scan);

	/* Close pg_database, but keep lock till commit */
	heap_close(rel, NoLock);

	/*
	 * We don't bother updating the flat file since ALTER DATABASE OWNER
	 * doesn't affect it.
	 */
}


/*
 * Helper functions
 */

static bool
get_db_info(const char *name, Oid *dbIdP, Oid *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, bool *dbAllowConnP,
			Oid *dbLastSysOidP,
			TransactionId *dbVacuumXidP, TransactionId *dbFrozenXidP,
			Oid *dbTablespace)
{
	Relation	relation;
	ScanKeyData scanKey;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		gottuple;

	AssertArg(name);

	/* Caller may wish to grab a better lock on pg_database beforehand... */
	relation = heap_open(DatabaseRelationId, AccessShareLock);

	ScanKeyInit(&scanKey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(name));

	scan = systable_beginscan(relation, DatabaseNameIndexId, true,
							  SnapshotNow, 1, &scanKey);

	tuple = systable_getnext(scan);

	gottuple = HeapTupleIsValid(tuple);
	if (gottuple)
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);

		/* oid of the database */
		if (dbIdP)
			*dbIdP = HeapTupleGetOid(tuple);
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
		/* limit of vacuumed XIDs */
		if (dbVacuumXidP)
			*dbVacuumXidP = dbform->datvacuumxid;
		/* limit of frozen XIDs */
		if (dbFrozenXidP)
			*dbFrozenXidP = dbform->datfrozenxid;
		/* default tablespace for this database */
		if (dbTablespace)
			*dbTablespace = dbform->dattablespace;
	}

	systable_endscan(scan);
	heap_close(relation, AccessShareLock);

	return gottuple;
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

	utup = SearchSysCache(AUTHOID,
						  ObjectIdGetDatum(GetUserId()),
						  0, 0, 0);
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
	scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Oid			dsttablespace = HeapTupleGetOid(tuple);
		char	   *dstpath;
		struct stat st;

		/* Don't mess with the global tablespace */
		if (dsttablespace == GLOBALTABLESPACE_OID)
			continue;

		dstpath = GetDatabasePath(db_id, dsttablespace);

		if (stat(dstpath, &st) < 0 || !S_ISDIR(st.st_mode))
		{
			/* Assume we can ignore it */
			pfree(dstpath);
			continue;
		}

		if (!rmtree(dstpath, true))
			ereport(WARNING,
					(errmsg("could not remove database directory \"%s\"",
							dstpath)));

		/* Record the filesystem change in XLOG */
		{
			xl_dbase_drop_rec xlrec;
			XLogRecData rdata[1];

			xlrec.db_id = db_id;
			xlrec.tablespace_id = dsttablespace;

			rdata[0].data = (char *) &xlrec;
			rdata[0].len = sizeof(xl_dbase_drop_rec);
			rdata[0].buffer = InvalidBuffer;
			rdata[0].next = NULL;

			(void) XLogInsert(RM_DBASE_ID, XLOG_DBASE_DROP, rdata);
		}

		pfree(dstpath);
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);
}


/*
 * get_database_oid - given a database name, look up the OID
 *
 * Returns InvalidOid if database name not found.
 *
 * This is not actually used in this file, but is exported for use elsewhere.
 */
Oid
get_database_oid(const char *dbname)
{
	Relation	pg_database;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	dbtuple;
	Oid			oid;

	/* There's no syscache for pg_database, so must look the hard way */
	pg_database = heap_open(DatabaseRelationId, AccessShareLock);
	ScanKeyInit(&entry[0],
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	scan = systable_beginscan(pg_database, DatabaseNameIndexId, true,
							  SnapshotNow, 1, entry);

	dbtuple = systable_getnext(scan);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(dbtuple))
		oid = HeapTupleGetOid(dbtuple);
	else
		oid = InvalidOid;

	systable_endscan(scan);
	heap_close(pg_database, AccessShareLock);

	return oid;
}


/*
 * get_database_name - given a database OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such database.
 *
 * This is not actually used in this file, but is exported for use elsewhere.
 */
char *
get_database_name(Oid dbid)
{
	Relation	pg_database;
	ScanKeyData entry[1];
	SysScanDesc scan;
	HeapTuple	dbtuple;
	char	   *result;

	/* There's no syscache for pg_database, so must look the hard way */
	pg_database = heap_open(DatabaseRelationId, AccessShareLock);
	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));
	scan = systable_beginscan(pg_database, DatabaseOidIndexId, true,
							  SnapshotNow, 1, entry);

	dbtuple = systable_getnext(scan);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(dbtuple))
		result = pstrdup(NameStr(((Form_pg_database) GETSTRUCT(dbtuple))->datname));
	else
		result = NULL;

	systable_endscan(scan);
	heap_close(pg_database, AccessShareLock);

	return result;
}

/*
 * DATABASE resource manager's routines
 */
void
dbase_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

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
				ereport(WARNING,
						(errmsg("could not remove database directory \"%s\"",
								dst_path)));
		}

		/*
		 * Force dirty buffers out to disk, to ensure source database is
		 * up-to-date for the copy.  (We really only need to flush buffers for
		 * the source database, but bufmgr.c provides no API for that.)
		 */
		BufferSync();

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

		/*
		 * Drop pages for this database that are in the shared buffer cache
		 */
		DropBuffers(xlrec->db_id);

		if (!rmtree(dst_path, true))
			ereport(WARNING,
					(errmsg("could not remove database directory \"%s\"",
							dst_path)));
	}
	else
		elog(PANIC, "dbase_redo: unknown op code %u", info);
}

void
dbase_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_DBASE_CREATE)
	{
		xl_dbase_create_rec *xlrec = (xl_dbase_create_rec *) rec;

		sprintf(buf + strlen(buf), "create db: copy dir %u/%u to %u/%u",
				xlrec->src_db_id, xlrec->src_tablespace_id,
				xlrec->db_id, xlrec->tablespace_id);
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec = (xl_dbase_drop_rec *) rec;

		sprintf(buf + strlen(buf), "drop db: dir %u/%u",
				xlrec->db_id, xlrec->tablespace_id);
	}
	else
		strcat(buf, "UNKNOWN");
}
