/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		Database management commands (create/drop database).
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/dbcommands.c,v 1.124.2.2 2005/03/12 21:12:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/catalog.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "catalog/indexing.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/sinval.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "mb/pg_wchar.h"		/* encoding check */


/* non-export function prototypes */
static bool get_db_info(const char *name, Oid *dbIdP, int4 *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, Oid *dbLastSysOidP,
			TransactionId *dbVacuumXidP, TransactionId *dbFrozenXidP,
			char *dbpath);
static bool have_createdb_privilege(void);
static char *resolve_alt_dbpath(const char *dbpath, Oid dboid);
static bool remove_dbdirs(const char *real_loc, const char *altloc);

/*
 * CREATE DATABASE
 */

void
createdb(const CreatedbStmt *stmt)
{
	char	   *nominal_loc;
	char	   *alt_loc;
	char	   *target_dir;
	char		src_loc[MAXPGPATH];
	char		buf[2 * MAXPGPATH + 100];
	Oid			src_dboid;
	AclId		src_owner;
	int			src_encoding;
	bool		src_istemplate;
	Oid			src_lastsysoid;
	TransactionId src_vacuumxid;
	TransactionId src_frozenxid;
	char		src_dbpath[MAXPGPATH];
	Relation	pg_database_rel;
	HeapTuple	tuple;
	TupleDesc	pg_database_dsc;
	Datum		new_record[Natts_pg_database];
	char		new_record_nulls[Natts_pg_database];
	Oid			dboid;
	AclId		datdba;
	List	   *option;
	DefElem    *downer = NULL;
	DefElem    *dpath = NULL;
	DefElem    *dtemplate = NULL;
	DefElem    *dencoding = NULL;
	char	   *dbname = stmt->dbname;
	char	   *dbowner = NULL;
	char	   *dbpath = NULL;
	char	   *dbtemplate = NULL;
	int			encoding = -1;

	/* Extract options from the statement node tree */
	foreach(option, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(option);

		if (strcmp(defel->defname, "owner") == 0)
		{
			if (downer)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			downer = defel;
		}
		else if (strcmp(defel->defname, "location") == 0)
		{
			if (dpath)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			dpath = defel;
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
		else
			elog(ERROR, "option \"%s\" not recognized",
				 defel->defname);
	}

	if (downer && downer->arg)
		dbowner = strVal(downer->arg);
	if (dpath && dpath->arg)
		dbpath = strVal(dpath->arg);
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

	/* don't call this in a transaction block */
	PreventTransactionChain((void *) stmt, "CREATE DATABASE");

	/* alternate location requires symlinks */
#ifndef HAVE_SYMLINK
	if (dbpath != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		   errmsg("cannot use an alternative location on this platform")));
#endif

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
					 &src_vacuumxid, &src_frozenxid,
					 src_dbpath))
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
	 * Determine physical path of source database
	 */
	alt_loc = resolve_alt_dbpath(src_dbpath, src_dboid);
	if (!alt_loc)
		alt_loc = GetDatabasePath(src_dboid);
	strcpy(src_loc, alt_loc);

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

	/*
	 * Preassign OID for pg_database tuple, so that we can compute db
	 * path.
	 */
	dboid = newoid();

	/*
	 * Compute nominal location (where we will try to access the
	 * database), and resolve alternate physical location if one is
	 * specified.
	 *
	 * If an alternate location is specified but is the same as the normal
	 * path, just drop the alternate-location spec (this seems friendlier
	 * than erroring out).	We must test this case to avoid creating a
	 * circular symlink below.
	 */
	nominal_loc = GetDatabasePath(dboid);
	alt_loc = resolve_alt_dbpath(dbpath, dboid);

	if (alt_loc && strcmp(alt_loc, nominal_loc) == 0)
	{
		alt_loc = NULL;
		dbpath = NULL;
	}

	if (strchr(nominal_loc, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("database path may not contain single quotes")));
	if (alt_loc && strchr(alt_loc, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("database path may not contain single quotes")));
	if (strchr(src_loc, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("database path may not contain single quotes")));
	/* ... otherwise we'd be open to shell exploits below */

	/*
	 * Force dirty buffers out to disk, to ensure source database is
	 * up-to-date for the copy.  (We really only need to flush buffers for
	 * the source database...)
	 */
	BufferSync();

	/*
	 * Close virtual file descriptors so the kernel has more available for
	 * the mkdir() and system() calls below.
	 */
	closeAllVfds();

	/*
	 * Check we can create the target directory --- but then remove it
	 * because we rely on cp(1) to create it for real.
	 */
	target_dir = alt_loc ? alt_loc : nominal_loc;

	if (mkdir(target_dir, S_IRWXU) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create database directory \"%s\": %m",
						target_dir)));
	if (rmdir(target_dir) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove temporary directory \"%s\": %m",
						target_dir)));

	/* Make the symlink, if needed */
	if (alt_loc)
	{
#ifdef HAVE_SYMLINK				/* already throws error above */
		if (symlink(alt_loc, nominal_loc) != 0)
#endif
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not link file \"%s\" to \"%s\": %m",
							nominal_loc, alt_loc)));
	}

	/*
	 * Copy the template database to the new location
	 *
	 * XXX use of cp really makes this code pretty grotty, particularly
	 * with respect to lack of ability to report errors well.  Someday
	 * rewrite to do it for ourselves.
	 */
#ifndef WIN32
	snprintf(buf, sizeof(buf), "cp -r '%s' '%s'", src_loc, target_dir);
	if (system(buf) != 0)
	{
		if (remove_dbdirs(nominal_loc, alt_loc))
			ereport(ERROR,
					(errmsg("could not initialize database directory"),
					 errdetail("Failing system command was: %s", buf),
					 errhint("Look in the postmaster's stderr log for more information.")));
		else
			ereport(ERROR,
					(errmsg("could not initialize database directory; delete failed as well"),
					 errdetail("Failing system command was: %s", buf),
					 errhint("Look in the postmaster's stderr log for more information.")));
	}
#else	/* WIN32 */
	if (copydir(src_loc, target_dir) != 0)
	{
		/* copydir should already have given details of its troubles */
		if (remove_dbdirs(nominal_loc, alt_loc))
			ereport(ERROR,
					(errmsg("could not initialize database directory")));
		else
			ereport(ERROR,
					(errmsg("could not initialize database directory; delete failed as well")));
	}
#endif	/* WIN32 */

	/*
	 * Now OK to grab exclusive lock on pg_database.
	 */
	pg_database_rel = heap_openr(DatabaseRelationName, AccessExclusiveLock);

	/* Check to see if someone else created same DB name meanwhile. */
	if (get_db_info(dbname, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
	{
		/* Don't hold lock while doing recursive remove */
		heap_close(pg_database_rel, AccessExclusiveLock);
		remove_dbdirs(nominal_loc, alt_loc);
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
	/* do not set datpath to null, GetRawDatabaseInfo won't cope */
	new_record[Anum_pg_database_datpath - 1] =
		DirectFunctionCall1(textin, CStringGetDatum(dbpath ? dbpath : ""));

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

	/* Close pg_database, but keep lock till commit */
	heap_close(pg_database_rel, NoLock);

	/*
	 * Force dirty buffers out to disk, so that newly-connecting backends
	 * will see the new database in pg_database right away.  (They'll see
	 * an uncommitted tuple, but they don't care; see GetRawDatabaseInfo.)
	 */
	BufferSync();
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
	char	   *alt_loc;
	char	   *nominal_loc;
	char		dbpath[MAXPGPATH];
	Relation	pgdbrel;
	SysScanDesc pgdbscan;
	ScanKeyData key;
	HeapTuple	tup;

	AssertArg(dbname);

	if (strcmp(dbname, get_database_name(MyDatabaseId)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("cannot drop the currently open database")));

	PreventTransactionChain((void *) dbname, "DROP DATABASE");

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
					 &db_istemplate, NULL, NULL, NULL, dbpath))
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

	nominal_loc = GetDatabasePath(db_id);
	alt_loc = resolve_alt_dbpath(dbpath, db_id);

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
	ScanKeyEntryInitialize(&key, 0, ObjectIdAttributeNumber,
						   F_OIDEQ, ObjectIdGetDatum(db_id));

	pgdbscan = systable_beginscan(pgdbrel, DatabaseOidIndex, true, SnapshotNow, 1, &key);

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
	 * Close pg_database, but keep exclusive lock till commit to ensure
	 * that any new backend scanning pg_database will see the tuple dead.
	 */
	heap_close(pgdbrel, NoLock);

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
	 * Remove the database's subdirectory and everything in it.
	 */
	remove_dbdirs(nominal_loc, alt_loc);

	/*
	 * Force dirty buffers out to disk, so that newly-connecting backends
	 * will see the database tuple marked dead in pg_database right away.
	 * (They'll see an uncommitted deletion, but they don't care; see
	 * GetRawDatabaseInfo.)
	 */
	BufferSync();
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

	ScanKeyEntryInitialize(&key, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(oldname));
	scan = systable_beginscan(rel, DatabaseNameIndex, true, SnapshotNow, 1, &key);

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
	ScanKeyEntryInitialize(&key2, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(newname));
	scan2 = systable_beginscan(rel, DatabaseNameIndex, true, SnapshotNow, 1, &key2);
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
	if (!superuser() && !have_createdb_privilege())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to rename database")));

	/* rename */
	newtup = heap_copytuple(tup);
	namestrcpy(&(((Form_pg_database) GETSTRUCT(newtup))->datname), newname);
	simple_heap_update(rel, &tup->t_self, newtup);
	CatalogUpdateIndexes(rel, newtup);

	systable_endscan(scan);
	heap_close(rel, NoLock);

	/*
	 * Force dirty buffers out to disk, so that newly-connecting backends
	 * will see the renamed database in pg_database right away.  (They'll
	 * see an uncommitted tuple, but they don't care; see
	 * GetRawDatabaseInfo.)
	 */
	BufferSync();
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

	rel = heap_openr(DatabaseRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&scankey, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(stmt->dbname));
	scan = systable_beginscan(rel, DatabaseNameIndex, true, SnapshotNow, 1, &scankey);
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

		a = isnull ? ((ArrayType *) NULL) : DatumGetArrayTypeP(datum);

		if (valuestr)
			a = GUCArrayAdd(a, stmt->variable, valuestr);
		else
			a = GUCArrayDelete(a, stmt->variable);

		if (a)
			repl_val[Anum_pg_database_datconfig - 1] = PointerGetDatum(a);
		else
			repl_null[Anum_pg_database_datconfig - 1] = 'n';
	}

	newtuple = heap_modifytuple(tuple, rel, repl_val, repl_null, repl_repl);
	simple_heap_update(rel, &tuple->t_self, newtuple);

	/* Update indexes */
	CatalogUpdateIndexes(rel, newtuple);

	systable_endscan(scan);
	heap_close(rel, RowExclusiveLock);

	/*
	 * Force dirty buffers out to disk, so that newly-connecting backends
	 * will see the updated database in pg_database right away.  (They'll
	 * see an uncommitted tuple, but they don't care; see
	 * GetRawDatabaseInfo.)
	 */
	BufferSync();
}



/*
 * Helper functions
 */

static bool
get_db_info(const char *name, Oid *dbIdP, int4 *ownerIdP,
			int *encodingP, bool *dbIsTemplateP, Oid *dbLastSysOidP,
			TransactionId *dbVacuumXidP, TransactionId *dbFrozenXidP,
			char *dbpath)
{
	Relation	relation;
	ScanKeyData scanKey;
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		gottuple;

	AssertArg(name);

	/* Caller may wish to grab a better lock on pg_database beforehand... */
	relation = heap_openr(DatabaseRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&scanKey, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(name));

	scan = systable_beginscan(relation, DatabaseNameIndex, true, SnapshotNow, 1, &scanKey);

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
		/* database path (as registered in pg_database) */
		if (dbpath)
		{
			Datum		datum;
			bool		isnull;

			datum = heap_getattr(tuple,
								 Anum_pg_database_datpath,
								 RelationGetDescr(relation),
								 &isnull);
			if (!isnull)
			{
				text	   *pathtext = DatumGetTextP(datum);
				int			pathlen = VARSIZE(pathtext) - VARHDRSZ;

				Assert(pathlen >= 0 && pathlen < MAXPGPATH);
				strncpy(dbpath, VARDATA(pathtext), pathlen);
				*(dbpath + pathlen) = '\0';
			}
			else
				strcpy(dbpath, "");
		}
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

	utup = SearchSysCache(SHADOWSYSID,
						  Int32GetDatum(GetUserId()),
						  0, 0, 0);
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_shadow) GETSTRUCT(utup))->usecreatedb;
		ReleaseSysCache(utup);
	}
	return result;
}


static char *
resolve_alt_dbpath(const char *dbpath, Oid dboid)
{
	const char *prefix;
	char	   *ret;
	size_t		len;

	if (dbpath == NULL || dbpath[0] == '\0')
		return NULL;

	if (first_path_separator(dbpath))
	{
		if (!is_absolute_path(dbpath))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("relative paths are not allowed as database locations")));
#ifndef ALLOW_ABSOLUTE_DBPATHS
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		errmsg("absolute paths are not allowed as database locations")));
#endif
		prefix = dbpath;
	}
	else
	{
		/* must be environment variable */
		char	   *var = getenv(dbpath);

		if (!var)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
			   errmsg("postmaster environment variable \"%s\" not found",
					  dbpath)));
		if (!is_absolute_path(var))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("postmaster environment variable \"%s\" must be absolute path",
							dbpath)));
		prefix = var;
	}

	len = strlen(prefix) + 6 + sizeof(Oid) * 8 + 1;
	if (len >= MAXPGPATH - 100)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("alternative path is too long")));

	ret = palloc(len);
	snprintf(ret, len, "%s/base/%u", prefix, dboid);

	return ret;
}


static bool
remove_dbdirs(const char *nominal_loc, const char *alt_loc)
{
	const char *target_dir;
	char		buf[MAXPGPATH + 100];
	bool		success = true;

	target_dir = alt_loc ? alt_loc : nominal_loc;

	/*
	 * Close virtual file descriptors so the kernel has more available for
	 * the system() call below.
	 */
	closeAllVfds();

	if (alt_loc)
	{
		/* remove symlink */
		if (unlink(nominal_loc) != 0)
		{
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m", nominal_loc)));
			success = false;
		}
	}

#ifndef WIN32
	snprintf(buf, sizeof(buf), "rm -rf '%s'", target_dir);
#else
	snprintf(buf, sizeof(buf), "rmdir /s /q \"%s\"", target_dir);
#endif

	if (system(buf) != 0)
	{
		ereport(WARNING,
				(errmsg("could not remove database directory \"%s\"",
						target_dir),
				 errdetail("Failing system command was: %s", buf),
				 errhint("Look in the postmaster's stderr log for more information.")));
		success = false;
	}

	return success;
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
	ScanKeyEntryInitialize(&entry[0], 0x0,
						   Anum_pg_database_datname, F_NAMEEQ,
						   CStringGetDatum(dbname));
	scan = systable_beginscan(pg_database, DatabaseNameIndex, true, SnapshotNow, 1, entry);

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
 * Returns InvalidOid if database name not found.
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
	ScanKeyEntryInitialize(&entry[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(dbid));
	scan = systable_beginscan(pg_database, DatabaseOidIndex, true, SnapshotNow, 1, entry);

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
