/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/dbcommands.c,v 1.70 2000/11/30 08:46:22 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/catalog.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "storage/sinval.h"		/* for DatabaseHasActiveBackends */
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"


/* non-export function prototypes */
static bool get_db_info(const char *name, Oid *dbIdP, int4 *ownerIdP,
						int *encodingP, bool *dbIsTemplateP,
						Oid *dbLastSysOidP, char *dbpath);
static bool get_user_info(Oid use_sysid, bool *use_super, bool *use_createdb);
static char *resolve_alt_dbpath(const char *dbpath, Oid dboid);
static bool remove_dbdirs(const char *real_loc, const char *altloc);

/*
 * CREATE DATABASE
 */

void
createdb(const char *dbname, const char *dbpath,
		 const char *dbtemplate, int encoding)
{
	char	   *nominal_loc;
	char	   *alt_loc;
	char	   *target_dir;
	char		src_loc[MAXPGPATH];
	char		buf[2 * MAXPGPATH + 100];
	int			ret;
	bool		use_super,
				use_createdb;
	Oid			src_dboid;
	int4		src_owner;
	int			src_encoding;
	bool		src_istemplate;
	Oid			src_lastsysoid;
	char		src_dbpath[MAXPGPATH];
	Relation	pg_database_rel;
	HeapTuple	tuple;
	TupleDesc	pg_database_dsc;
	Datum		new_record[Natts_pg_database];
	char		new_record_nulls[Natts_pg_database];
	Oid			dboid;

	if (!get_user_info(GetUserId(), &use_super, &use_createdb))
		elog(ERROR, "current user name is invalid");

	if (!use_createdb && !use_super)
		elog(ERROR, "CREATE DATABASE: permission denied");

	/* don't call this in a transaction block */
	if (IsTransactionBlock())
		elog(ERROR, "CREATE DATABASE: may not be called in a transaction block");

	/*
	 * Check for db name conflict.  There is a race condition here, since
	 * another backend could create the same DB name before we commit.
	 * However, holding an exclusive lock on pg_database for the whole time
	 * we are copying the source database doesn't seem like a good idea,
	 * so accept possibility of race to create.  We will check again after
	 * we grab the exclusive lock.
	 */
	if (get_db_info(dbname, NULL, NULL, NULL, NULL, NULL, NULL))
		elog(ERROR, "CREATE DATABASE: database \"%s\" already exists", dbname);

	/*
	 * Lookup database (template) to be cloned.
	 */
	if (!dbtemplate)
		dbtemplate = "template1"; /* Default template database name */

	if (!get_db_info(dbtemplate, &src_dboid, &src_owner, &src_encoding,
					 &src_istemplate, &src_lastsysoid, src_dbpath))
		elog(ERROR, "CREATE DATABASE: template \"%s\" does not exist",
			 dbtemplate);
	/*
	 * Permission check: to copy a DB that's not marked datistemplate,
	 * you must be superuser or the owner thereof.
	 */
	if (!src_istemplate)
	{
		if (!use_super && GetUserId() != src_owner)
			elog(ERROR, "CREATE DATABASE: permission to copy \"%s\" denied",
				 dbtemplate);
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
		elog(ERROR, "CREATE DATABASE: source database \"%s\" is being accessed by other users", dbtemplate);

	/* If encoding is defaulted, use source's encoding */
	if (encoding < 0)
		encoding = src_encoding;

	/* 
	 * Preassign OID for pg_database tuple, so that we can compute db path.
	 */
	dboid = newoid();

	/*
	 * Compute nominal location (where we will try to access the database),
	 * and resolve alternate physical location if one is specified.
	 */
	nominal_loc = GetDatabasePath(dboid);
	alt_loc = resolve_alt_dbpath(dbpath, dboid);

	if (strchr(nominal_loc, '\''))
		elog(ERROR, "database path may not contain single quotes");
	if (alt_loc && strchr(alt_loc, '\''))
		elog(ERROR, "database path may not contain single quotes");
	if (strchr(src_loc, '\''))
		elog(ERROR, "database path may not contain single quotes");
	/* ... otherwise we'd be open to shell exploits below */

	/* Force dirty buffers out to disk, to ensure source database is
	 * up-to-date for the copy.  (We really only need to flush buffers
	 * for the source database...)
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
		elog(ERROR, "CREATE DATABASE: unable to create database directory '%s': %m",
			 target_dir);
	rmdir(target_dir);

	/* Make the symlink, if needed */
	if (alt_loc)
	{
		if (symlink(alt_loc, nominal_loc) != 0)
			elog(ERROR, "CREATE DATABASE: could not link '%s' to '%s': %m",
				 nominal_loc, alt_loc);
	}

	/* Copy the template database to the new location */
	snprintf(buf, sizeof(buf), "cp -r '%s' '%s'", src_loc, target_dir);

	ret = system(buf);
	/* Some versions of SunOS seem to return ECHILD after a system() call */
	if (ret != 0 && errno != ECHILD)
	{
		if (remove_dbdirs(nominal_loc, alt_loc))
			elog(ERROR, "CREATE DATABASE: could not initialize database directory");
		else
			elog(ERROR, "CREATE DATABASE: could not initialize database directory; delete failed as well");
	}

	/*
	 * Now OK to grab exclusive lock on pg_database.
	 */
	pg_database_rel = heap_openr(DatabaseRelationName, AccessExclusiveLock);

	/* Check to see if someone else created same DB name meanwhile. */
	if (get_db_info(dbname, NULL, NULL, NULL, NULL, NULL, NULL))
	{
		remove_dbdirs(nominal_loc, alt_loc);
		elog(ERROR, "CREATE DATABASE: database \"%s\" already exists", dbname);
	}

	/*
	 * Insert a new tuple into pg_database
	 */
	pg_database_dsc = RelationGetDescr(pg_database_rel);

	/* Form tuple */
	new_record[Anum_pg_database_datname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(dbname));
	new_record[Anum_pg_database_datdba - 1] = Int32GetDatum(GetUserId());
	new_record[Anum_pg_database_encoding - 1] = Int32GetDatum(encoding);
	new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(false);
	new_record[Anum_pg_database_datallowconn - 1] = BoolGetDatum(true);
	new_record[Anum_pg_database_datlastsysoid - 1] = ObjectIdGetDatum(src_lastsysoid);
	/* no nulls here, GetRawDatabaseInfo doesn't like them */
	new_record[Anum_pg_database_datpath - 1] =
		DirectFunctionCall1(textin, CStringGetDatum(dbpath ? dbpath : ""));

	memset(new_record_nulls, ' ', sizeof(new_record_nulls));

	tuple = heap_formtuple(pg_database_dsc, new_record, new_record_nulls);

	tuple->t_data->t_oid = dboid;	/* override heap_insert's OID selection */

	heap_insert(pg_database_rel, tuple);

	/*
	 * Update indexes (there aren't any currently)
	 */
#ifdef Num_pg_database_indices
	if (RelationGetForm(pg_database_rel)->relhasindex)
	{
		Relation	idescs[Num_pg_database_indices];

		CatalogOpenIndices(Num_pg_database_indices,
						   Name_pg_database_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_database_indices, pg_database_rel,
						   tuple);
		CatalogCloseIndices(Num_pg_database_indices, idescs);
	}
#endif

	/* Close pg_database, but keep lock till commit */
	heap_close(pg_database_rel, NoLock);

	/* Force dirty buffers out to disk, so that newly-connecting backends
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
	bool		use_super;
	Oid			db_id;
	char       *alt_loc;
	char       *nominal_loc;
	char        dbpath[MAXPGPATH];
	Relation	pgdbrel;
	HeapScanDesc pgdbscan;
	ScanKeyData key;
	HeapTuple	tup;

	AssertArg(dbname);

	if (strcmp(dbname, DatabaseName) == 0)
		elog(ERROR, "DROP DATABASE: cannot be executed on the currently open database");

	if (IsTransactionBlock())
		elog(ERROR, "DROP DATABASE: may not be called in a transaction block");

	if (!get_user_info(GetUserId(), &use_super, NULL))
		elog(ERROR, "current user name is invalid");

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
					 &db_istemplate, NULL, dbpath))
		elog(ERROR, "DROP DATABASE: database \"%s\" does not exist", dbname);

	if (!use_super && GetUserId() != db_owner)
		elog(ERROR, "DROP DATABASE: permission denied");

	/*
	 * Disallow dropping a DB that is marked istemplate.  This is just
	 * to prevent people from accidentally dropping template0 or template1;
	 * they can do so if they're really determined ...
	 */
	if (db_istemplate)
		elog(ERROR, "DROP DATABASE: database is marked as a template");

	nominal_loc = GetDatabasePath(db_id);
	alt_loc = resolve_alt_dbpath(dbpath, db_id);

	/*
	 * Check for active backends in the target database.
	 */
	if (DatabaseHasActiveBackends(db_id, false))
		elog(ERROR, "DROP DATABASE: database \"%s\" is being accessed by other users", dbname);

	/*
	 * Find the database's tuple by OID (should be unique, we trust).
	 */
	ScanKeyEntryInitialize(&key, 0, ObjectIdAttributeNumber,
						   F_OIDEQ, ObjectIdGetDatum(db_id));

	pgdbscan = heap_beginscan(pgdbrel, 0, SnapshotNow, 1, &key);

	tup = heap_getnext(pgdbscan, 0);
	if (!HeapTupleIsValid(tup))
	{
		/*
		 * This error should never come up since the existence of the
		 * database is checked earlier
		 */
		elog(ERROR, "DROP DATABASE: Database \"%s\" doesn't exist despite earlier reports to the contrary",
			 dbname);
	}

	/* Remove the database's tuple from pg_database */
	heap_delete(pgdbrel, &tup->t_self, NULL);

	heap_endscan(pgdbscan);

	/*
	 * Close pg_database, but keep exclusive lock till commit to ensure
	 * that any new backend scanning pg_database will see the tuple dead.
	 */
	heap_close(pgdbrel, NoLock);

	/* Delete any comments associated with the database */
	DeleteComments(db_id);

	/*
	 * Drop pages for this database that are in the shared buffer cache.
	 * This is important to ensure that no remaining backend tries to
	 * write out a dirty buffer to the dead database later...
	 */
	DropBuffers(db_id);

	/*
	 * Remove the database's subdirectory and everything in it.
	 */
	remove_dbdirs(nominal_loc, alt_loc);
}



/*
 * Helper functions
 */

static bool
get_db_info(const char *name, Oid *dbIdP, int4 *ownerIdP,
			int *encodingP, bool *dbIsTemplateP,
			Oid *dbLastSysOidP, char *dbpath)
{
	Relation	relation;
	ScanKeyData scanKey;
	HeapScanDesc scan;
	HeapTuple	tuple;

	AssertArg(name);

	/* Caller may wish to grab a better lock on pg_database beforehand... */
	relation = heap_openr(DatabaseRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&scanKey, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(name));

	scan = heap_beginscan(relation, 0, SnapshotNow, 1, &scanKey);
	if (!HeapScanIsValid(scan))
		elog(ERROR, "Cannot begin scan of %s", DatabaseRelationName);

	tuple = heap_getnext(scan, 0);

	if (HeapTupleIsValid(tuple))
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);
		text	   *tmptext;
		bool		isnull;

		/* oid of the database */
		if (dbIdP)
			*dbIdP = tuple->t_data->t_oid;
		/* uid of the owner */
		if (ownerIdP)
			*ownerIdP = dbform->datdba;
		/* multibyte encoding */
		if (encodingP)
			*encodingP = dbform->encoding;
		/* allowed as template? */
		if (dbIsTemplateP)
			*dbIsTemplateP = dbform->datistemplate;
		/* last system OID used in database */
		if (dbLastSysOidP)
			*dbLastSysOidP = dbform->datlastsysoid;
		/* database path (as registered in pg_database) */
		if (dbpath)
		{
			tmptext = DatumGetTextP(heap_getattr(tuple,
												 Anum_pg_database_datpath,
												 RelationGetDescr(relation),
												 &isnull));
			if (!isnull)
			{
				Assert(VARSIZE(tmptext) - VARHDRSZ < MAXPGPATH);

				strncpy(dbpath, VARDATA(tmptext), VARSIZE(tmptext) - VARHDRSZ);
				*(dbpath + VARSIZE(tmptext) - VARHDRSZ) = '\0';
			}
			else
				strcpy(dbpath, "");
		}
	}

	heap_endscan(scan);
	heap_close(relation, AccessShareLock);

	return HeapTupleIsValid(tuple);
}

static bool
get_user_info(Oid use_sysid, bool *use_super, bool *use_createdb)
{
	HeapTuple	utup;

	utup = SearchSysCache(SHADOWSYSID,
						  ObjectIdGetDatum(use_sysid),
						  0, 0, 0);

	if (!HeapTupleIsValid(utup))
		return false;

	if (use_super)
		*use_super = ((Form_pg_shadow) GETSTRUCT(utup))->usesuper;
	if (use_createdb)
		*use_createdb = ((Form_pg_shadow) GETSTRUCT(utup))->usecreatedb;

	ReleaseSysCache(utup);

	return true;
}


static char *
resolve_alt_dbpath(const char * dbpath, Oid dboid)
{
	const char * prefix;
	char * ret;
	size_t len;

	if (dbpath == NULL || dbpath[0] == '\0')
		return NULL;

	if (strchr(dbpath, '/'))
	{
		if (dbpath[0] != '/')
			elog(ERROR, "Relative paths are not allowed as database locations");
#ifndef ALLOW_ABSOLUTE_DBPATHS
		elog(ERROR, "Absolute paths are not allowed as database locations");
#endif
		prefix = dbpath;
	}
	else
	{
		/* must be environment variable */
		char * var = getenv(dbpath);
		if (!var)
			elog(ERROR, "Postmaster environment variable '%s' not set", dbpath);
		if (var[0] != '/')
			elog(ERROR, "Postmaster environment variable '%s' must be absolute path", dbpath);
		prefix = var;
	}

	len = strlen(prefix) + 6 + sizeof(Oid) * 8 + 1;
	ret = palloc(len);
	snprintf(ret, len, "%s/base/%u", prefix, dboid);

	return ret;
}


static bool
remove_dbdirs(const char * nominal_loc, const char * alt_loc)
{
	const char   *target_dir;
	char buf[MAXPGPATH + 100];
	bool success = true;

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
			elog(NOTICE, "could not remove '%s': %m", nominal_loc);
			success = false;
		}
	}

	snprintf(buf, sizeof(buf), "rm -rf '%s'", target_dir);

	if (system(buf) != 0 && errno != ECHILD)
	{
		elog(NOTICE, "database directory '%s' could not be removed",
			 target_dir);
		success = false;
	}

	return success;
}
