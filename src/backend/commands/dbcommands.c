/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		Database management commands (create/drop database).
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/dbcommands.c,v 1.149 2005/01/27 23:23:55 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/catalog.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "catalog/pg_tablespace.h"
#include "catalog/indexing.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/tablespace.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/freespace.h"
#include "storage/sinval.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* non-export function prototypes */
static bool get_db_info(const char *name, Oid *dbIdP, int4 *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, Oid *dbLastSysOidP,
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
	AclId		src_owner;
	int			src_encoding;
	bool		src_istemplate;
	Oid			src_lastsysoid;
	TransactionId src_vacuumxid;
	TransactionId src_frozenxid;
	Oid			src_deftablespace;
	Oid			dst_deftablespace;
	Relation	pg_database_rel;
	HeapTuple	tuple;
	TupleDesc	pg_database_dsc;
	Datum		new_record[Natts_pg_database];
	char		new_record_nulls[Natts_pg_database];
	Oid			dboid;
	AclId		datdba;
	ListCell   *option;
	DefElem    *dtablespacename = NULL;
	DefElem    *downer = NULL;
	DefElem    *dtemplate = NULL;
	DefElem    *dencoding = NULL;
	char	   *dbname = stmt->dbname;
	char	   *dbowner = NULL;
	char	   *dbtemplate = NULL;
	int			encoding = -1;

#ifndef WIN32
	char		buf[2 * MAXPGPATH + 100];
#endif

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

	/* obtain sysid of proposed owner */
	if (dbowner)
		datdba = get_usesysid(dbowner); /* will ereport if no such user */
	else
		datdba = GetUserId();

	if (datdba == GetUserId())
	{
		/* creating database for self: can be superuser or createdb */
		if (!superuser() && !have_createdb_privilege())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to create database")));
	}
	else
	{
		/* creating database for someone else: must be superuser */
		/* note that the someone else need not have any permissions */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to create database for another user")));
	}

	/*
	 * Check for db name conflict.	There is a race condition here, since
	 * another backend could create the same DB name before we commit.
	 * However, holding an exclusive lock on pg_database for the whole
	 * time we are copying the source database doesn't seem like a good
	 * idea, so accept possibility of race to create.  We will check again
	 * after we grab the exclusive lock.
	 */
	if (get_db_info(dbname, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", dbname)));

	/*
	 * Lookup database (template) to be cloned.
	 */
	if (!dbtemplate)
		dbtemplate = "template1";		/* Default template database name */

	if (!get_db_info(dbtemplate, &src_dboid, &src_owner, &src_encoding,
					 &src_istemplate, &src_lastsysoid,
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
		if (!superuser() && GetUserId() != src_owner)
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
	 * Preassign OID for pg_database tuple, so that we can compute db
	 * path.
	 */
	dboid = newoid();

	/*
	 * Force dirty buffers out to disk, to ensure source database is
	 * up-to-date for the copy.  (We really only need to flush buffers for
	 * the source database, but bufmgr.c provides no API for that.)
	 */
	BufferSync(-1, -1);

	/*
	 * Close virtual file descriptors so the kernel has more available for
	 * the system() calls below.
	 */
	closeAllVfds();

	/*
	 * Iterate through all tablespaces of the template database, and copy
	 * each one to the new database.
	 */
	rel = heap_openr(TableSpaceRelationName, AccessShareLock);
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

		if (stat(dstpath, &st) == 0 || errno != ENOENT)
		{
			remove_dbtablespaces(dboid);
			ereport(ERROR,
					(errmsg("could not initialize database directory"),
					 errdetail("Directory \"%s\" already exists.",
							   dstpath)));
		}

#ifndef WIN32

		/*
		 * Copy this subdirectory to the new location
		 *
		 * XXX use of cp really makes this code pretty grotty, particularly
		 * with respect to lack of ability to report errors well.  Someday
		 * rewrite to do it for ourselves.
		 */

		/* We might need to use cp -R one day for portability */
		snprintf(buf, sizeof(buf), "cp -r '%s' '%s'",
				 srcpath, dstpath);
		if (system(buf) != 0)
		{
			remove_dbtablespaces(dboid);
			ereport(ERROR,
					(errmsg("could not initialize database directory"),
					 errdetail("Failing system command was: %s", buf),
					 errhint("Look in the postmaster's stderr log for more information.")));
		}
#else							/* WIN32 */
		if (copydir(srcpath, dstpath) != 0)
		{
			/* copydir should already have given details of its troubles */
			remove_dbtablespaces(dboid);
			ereport(ERROR,
					(errmsg("could not initialize database directory")));
		}
#endif   /* WIN32 */

		/* Record the filesystem change in XLOG */
		{
			xl_dbase_create_rec xlrec;
			XLogRecData rdata[3];

			xlrec.db_id = dboid;
			rdata[0].buffer = InvalidBuffer;
			rdata[0].data = (char *) &xlrec;
			rdata[0].len = offsetof(xl_dbase_create_rec, src_path);
			rdata[0].next = &(rdata[1]);

			rdata[1].buffer = InvalidBuffer;
			rdata[1].data = (char *) srcpath;
			rdata[1].len = strlen(srcpath) + 1;
			rdata[1].next = &(rdata[2]);

			rdata[2].buffer = InvalidBuffer;
			rdata[2].data = (char *) dstpath;
			rdata[2].len = strlen(dstpath) + 1;
			rdata[2].next = NULL;

			(void) XLogInsert(RM_DBASE_ID, XLOG_DBASE_CREATE, rdata);
		}
	}
	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	/*
	 * Now OK to grab exclusive lock on pg_database.
	 */
	pg_database_rel = heap_openr(DatabaseRelationName, AccessExclusiveLock);

	/* Check to see if someone else created same DB name meanwhile. */
	if (get_db_info(dbname, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
	{
		/* Don't hold lock while doing recursive remove */
		heap_close(pg_database_rel, AccessExclusiveLock);
		remove_dbtablespaces(dboid);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_DATABASE),
				 errmsg("database \"%s\" already exists", dbname)));
	}

	/*
	 * Insert a new tuple into pg_database
	 */
	pg_database_dsc = RelationGetDescr(pg_database_rel);

	/* Form tuple */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));

	new_record[Anum_pg_database_datname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(dbname));
	new_record[Anum_pg_database_datdba - 1] = Int32GetDatum(datdba);
	new_record[Anum_pg_database_encoding - 1] = Int32GetDatum(encoding);
	new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(false);
	new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(true);
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

	HeapTupleSetOid(tuple, dboid);		/* override heap_insert's OID
										 * selection */

	simple_heap_insert(pg_database_rel, tuple);

	/* Update indexes */
	CatalogUpdateIndexes(pg_database_rel, tuple);

	/*
	 * Force dirty buffers out to disk, so that newly-connecting backends
	 * will see the new database in pg_database right away.  (They'll see
	 * an uncommitted tuple, but they don't care; see GetRawDatabaseInfo.)
	 */
	FlushRelationBuffers(pg_database_rel, MaxBlockNumber);

	/* Close pg_database, but keep exclusive lock till commit */
	heap_close(pg_database_rel, NoLock);
}


/*
 * DROP DATABASE
 */
void
dropdb(const char *dbname)
{
	int4		db_owner;
	bool		db_istemplate;
	Oid			db_id;
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
	 * Obtain exclusive lock on pg_database.  We need this to ensure that
	 * no new backend starts up in the target database while we are
	 * deleting it.  (Actually, a new backend might still manage to start
	 * up, because it will read pg_database without any locking to
	 * discover the database's OID.  But it will detect its error in
	 * ReverifyMyDatabase and shut down before any serious damage is done.
	 * See postinit.c.)
	 */
	pgdbrel = heap_openr(DatabaseRelationName, AccessExclusiveLock);

	if (!get_db_info(dbname, &db_id, &db_owner, NULL,
					 &db_istemplate, NULL, NULL, NULL, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", dbname)));

	if (GetUserId() != db_owner && !superuser())
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
					   dbname);

	/*
	 * Disallow dropping a DB that is marked istemplate.  This is just to
	 * prevent people from accidentally dropping template0 or template1;
	 * they can do so if they're really determined ...
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

	pgdbscan = systable_beginscan(pgdbrel, DatabaseOidIndex, true,
								  SnapshotNow, 1, &key);

	tup = systable_getnext(pgdbscan);
	if (!HeapTupleIsValid(tup))
	{
		/*
		 * This error should never come up since the existence of the
		 * database is checked earlier
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
	DeleteComments(db_id, RelationGetRelid(pgdbrel), 0);

	/*
	 * Drop pages for this database that are in the shared buffer cache.
	 * This is important to ensure that no remaining backend tries to
	 * write out a dirty buffer to the dead database later...
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
	RequestCheckpoint(true);
#endif

	/*
	 * Remove all tablespace subdirs belonging to the database.
	 */
	remove_dbtablespaces(db_id);

	/*
	 * Force dirty buffers out to disk, so that newly-connecting backends
	 * will see the database tuple marked dead in pg_database right away.
	 * (They'll see an uncommitted deletion, but they don't care; see
	 * GetRawDatabaseInfo.)
	 */
	FlushRelationBuffers(pgdbrel, MaxBlockNumber);

	/* Close pg_database, but keep exclusive lock till commit */
	heap_close(pgdbrel, NoLock);
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
	 * Obtain AccessExclusiveLock so that no new session gets started
	 * while the rename is in progress.
	 */
	rel = heap_openr(DatabaseRelationName, AccessExclusiveLock);

	ScanKeyInit(&key,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(oldname));
	scan = systable_beginscan(rel, DatabaseNameIndex, true,
							  SnapshotNow, 1, &key);

	tup = systable_getnext(scan);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", oldname)));

	/*
	 * XXX Client applications probably store the current database
	 * somewhere, so renaming it could cause confusion.  On the other
	 * hand, there may not be an actual problem besides a little
	 * confusion, so think about this and decide.
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
	scan2 = systable_beginscan(rel, DatabaseNameIndex, true,
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

	/* must have createdb */
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

	/*
	 * Force dirty buffers out to disk, so that newly-connecting backends
	 * will see the renamed database in pg_database right away.  (They'll
	 * see an uncommitted tuple, but they don't care; see
	 * GetRawDatabaseInfo.)
	 */
	FlushRelationBuffers(rel, MaxBlockNumber);

	/* Close pg_database, but keep exclusive lock till commit */
	heap_close(rel, NoLock);
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
	 * We need AccessExclusiveLock so we can safely do FlushRelationBuffers.
	 */
	rel = heap_openr(DatabaseRelationName, AccessExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndex, true,
							  SnapshotNow, 1, &scankey);
	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_DATABASE),
				 errmsg("database \"%s\" does not exist", stmt->dbname)));

	if (!(superuser()
		|| ((Form_pg_database) GETSTRUCT(tuple))->datdba == GetUserId()))
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

	/*
	 * Force dirty buffers out to disk, so that newly-connecting backends
	 * will see the altered row in pg_database right away.  (They'll
	 * see an uncommitted tuple, but they don't care; see
	 * GetRawDatabaseInfo.)
	 */
	FlushRelationBuffers(rel, MaxBlockNumber);

	/* Close pg_database, but keep exclusive lock till commit */
	heap_close(rel, NoLock);
}


/*
 * ALTER DATABASE name OWNER TO newowner
 */
void
AlterDatabaseOwner(const char *dbname, AclId newOwnerSysId)
{
	HeapTuple	tuple;
	Relation	rel;
	ScanKeyData scankey;
	SysScanDesc scan;
	Form_pg_database datForm;

	/*
	 * We need AccessExclusiveLock so we can safely do FlushRelationBuffers.
	 */
	rel = heap_openr(DatabaseRelationName, AccessExclusiveLock);
	ScanKeyInit(&scankey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(dbname));
	scan = systable_beginscan(rel, DatabaseNameIndex, true,
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
	if (datForm->datdba != newOwnerSysId)
	{
		Datum		repl_val[Natts_pg_database];
		char		repl_null[Natts_pg_database];
		char		repl_repl[Natts_pg_database];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* changing owner's database for someone else: must be superuser */
		/* note that the someone else need not have any permissions */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to change owner")));

		memset(repl_null, ' ', sizeof(repl_null));
		memset(repl_repl, ' ', sizeof(repl_repl));

		repl_repl[Anum_pg_database_datdba - 1] = 'r';
		repl_val[Anum_pg_database_datdba - 1] = Int32GetDatum(newOwnerSysId);

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
								 datForm->datdba, newOwnerSysId);
			repl_repl[Anum_pg_database_datacl - 1] = 'r';
			repl_val[Anum_pg_database_datacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modifytuple(tuple, RelationGetDescr(rel), repl_val, repl_null, repl_repl);
		simple_heap_update(rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(rel, newtuple);

		heap_freetuple(newtuple);

		/* must release buffer pins before FlushRelationBuffers */
		systable_endscan(scan);

		/*
		 * Force dirty buffers out to disk, so that newly-connecting backends
		 * will see the altered row in pg_database right away.  (They'll
		 * see an uncommitted tuple, but they don't care; see
		 * GetRawDatabaseInfo.)
		 */
		FlushRelationBuffers(rel, MaxBlockNumber);
	}
	else
		systable_endscan(scan);

	/* Close pg_database, but keep exclusive lock till commit */
	heap_close(rel, NoLock);
}


/*
 * Helper functions
 */

static bool
get_db_info(const char *name, Oid *dbIdP, int4 *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, Oid *dbLastSysOidP,
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
	relation = heap_openr(DatabaseRelationName, AccessShareLock);

	ScanKeyInit(&scanKey,
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				NameGetDatum(name));

	scan = systable_beginscan(relation, DatabaseNameIndex, true,
							  SnapshotNow, 1, &scanKey);

	tuple = systable_getnext(scan);

	gottuple = HeapTupleIsValid(tuple);
	if (gottuple)
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);

		/* oid of the database */
		if (dbIdP)
			*dbIdP = HeapTupleGetOid(tuple);
		/* sysid of the owner */
		if (ownerIdP)
			*ownerIdP = dbform->datdba;
		/* character encoding */
		if (encodingP)
			*encodingP = dbform->encoding;
		/* allowed as template? */
		if (dbIsTemplateP)
			*dbIsTemplateP = dbform->datistemplate;
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

static bool
have_createdb_privilege(void)
{
	HeapTuple	utup;
	bool		retval;

	utup = SearchSysCache(SHADOWSYSID,
						  Int32GetDatum(GetUserId()),
						  0, 0, 0);

	if (!HeapTupleIsValid(utup))
		retval = false;
	else
		retval = ((Form_pg_shadow) GETSTRUCT(utup))->usecreatedb;

	ReleaseSysCache(utup);

	return retval;
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

	rel = heap_openr(TableSpaceRelationName, AccessShareLock);
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
			XLogRecData rdata[2];

			xlrec.db_id = db_id;
			rdata[0].buffer = InvalidBuffer;
			rdata[0].data = (char *) &xlrec;
			rdata[0].len = offsetof(xl_dbase_drop_rec, dir_path);
			rdata[0].next = &(rdata[1]);

			rdata[1].buffer = InvalidBuffer;
			rdata[1].data = (char *) dstpath;
			rdata[1].len = strlen(dstpath) + 1;
			rdata[1].next = NULL;

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
	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyInit(&entry[0],
				Anum_pg_database_datname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(dbname));
	scan = systable_beginscan(pg_database, DatabaseNameIndex, true,
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
	pg_database = heap_openr(DatabaseRelationName, AccessShareLock);
	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(dbid));
	scan = systable_beginscan(pg_database, DatabaseOidIndex, true,
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
		char	   *dst_path = xlrec->src_path + strlen(xlrec->src_path) + 1;
		struct stat st;

#ifndef WIN32
		char		buf[2 * MAXPGPATH + 100];
#endif

		/*
		 * Our theory for replaying a CREATE is to forcibly drop the
		 * target subdirectory if present, then re-copy the source data.
		 * This may be more work than needed, but it is simple to
		 * implement.
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
		BufferSync(-1, -1);

#ifndef WIN32

		/*
		 * Copy this subdirectory to the new location
		 *
		 * XXX use of cp really makes this code pretty grotty, particularly
		 * with respect to lack of ability to report errors well.  Someday
		 * rewrite to do it for ourselves.
		 */

		/* We might need to use cp -R one day for portability */
		snprintf(buf, sizeof(buf), "cp -r '%s' '%s'",
				 xlrec->src_path, dst_path);
		if (system(buf) != 0)
			ereport(ERROR,
					(errmsg("could not initialize database directory"),
					 errdetail("Failing system command was: %s", buf),
					 errhint("Look in the postmaster's stderr log for more information.")));
#else							/* WIN32 */
		if (copydir(xlrec->src_path, dst_path) != 0)
		{
			/* copydir should already have given details of its troubles */
			ereport(ERROR,
					(errmsg("could not initialize database directory")));
		}
#endif   /* WIN32 */
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec = (xl_dbase_drop_rec *) XLogRecGetData(record);

		/*
		 * Drop pages for this database that are in the shared buffer
		 * cache
		 */
		DropBuffers(xlrec->db_id);

		if (!rmtree(xlrec->dir_path, true))
			ereport(WARNING,
					(errmsg("could not remove database directory \"%s\"",
							xlrec->dir_path)));
	}
	else
		elog(PANIC, "dbase_redo: unknown op code %u", info);
}

void
dbase_undo(XLogRecPtr lsn, XLogRecord *record)
{
	elog(PANIC, "dbase_undo: unimplemented");
}

void
dbase_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_DBASE_CREATE)
	{
		xl_dbase_create_rec *xlrec = (xl_dbase_create_rec *) rec;
		char	   *dst_path = xlrec->src_path + strlen(xlrec->src_path) + 1;

		sprintf(buf + strlen(buf), "create db: %u copy \"%s\" to \"%s\"",
				xlrec->db_id, xlrec->src_path, dst_path);
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec = (xl_dbase_drop_rec *) rec;

		sprintf(buf + strlen(buf), "drop db: %u directory: \"%s\"",
				xlrec->db_id, xlrec->dir_path);
	}
	else
		strcat(buf, "UNKNOWN");
}
