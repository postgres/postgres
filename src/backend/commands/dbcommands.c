/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/dbcommands.c,v 1.43 1999/10/26 03:12:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <signal.h>
#include <sys/stat.h>

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_database.h"
#include "catalog/pg_shadow.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "tcop/tcopprot.h"
#include "utils/syscache.h"


/* non-export function prototypes */
static void check_permissions(char *command, char *dbpath, char *dbname,
				  Oid *dbIdP, int4 *userIdP);
static HeapTuple get_pg_dbtup(char *command, char *dbname, Relation dbrel);
static void stop_vacuum(char *dbpath, char *dbname);

void
createdb(char *dbname, char *dbpath, int encoding, CommandDest dest)
{
	Oid			db_id;
	int4		user_id;
	char		buf[MAXPGPATH + 100];
	char	   *lp,
				loc[MAXPGPATH];

	/*
	 * If this call returns, the database does not exist and we're allowed
	 * to create databases.
	 */
	check_permissions("createdb", dbpath, dbname, &db_id, &user_id);

	/* close virtual file descriptors so we can do system() calls */
	closeAllVfds();

	/* Now create directory for this new database */
	if ((dbpath != NULL) && (strcmp(dbpath, dbname) != 0))
	{
		if (*(dbpath + strlen(dbpath) - 1) == SEP_CHAR)
			*(dbpath + strlen(dbpath) - 1) = '\0';
		snprintf(loc, sizeof(loc), "%s%c%s", dbpath, SEP_CHAR, dbname);
	}
	else
		strcpy(loc, dbname);

	lp = ExpandDatabasePath(loc);

	if (lp == NULL)
		elog(ERROR, "Unable to locate path '%s'"
			 "\n\tThis may be due to a missing environment variable"
			 " in the server", loc);

	if (mkdir(lp, S_IRWXU) != 0)
		elog(ERROR, "Unable to create database directory '%s'", lp);

	snprintf(buf, sizeof(buf), "%s %s%cbase%ctemplate1%c* '%s'",
			 COPY_CMD, DataDir, SEP_CHAR, SEP_CHAR, SEP_CHAR, lp);
	system(buf);

	snprintf(buf, sizeof(buf),
		   "insert into pg_database (datname, datdba, encoding, datpath)"
		  " values ('%s', '%d', '%d', '%s');", dbname, user_id, encoding,
			 loc);

	pg_exec_query_dest(buf, dest, false);
}

void
destroydb(char *dbname, CommandDest dest)
{
	int4		user_id;
	Oid			db_id;
	char	   *path,
				dbpath[MAXPGPATH],
				buf[MAXPGPATH + 100];
	Relation	pgdbrel;
	HeapScanDesc pgdbscan;
	ScanKeyData	key;
	HeapTuple	tup;

	/*
	 * If this call returns, the database exists and we're allowed to
	 * remove it.
	 */
	check_permissions("destroydb", dbpath, dbname, &db_id, &user_id);

	/* do as much checking as we can... */
	if (!OidIsValid(db_id))
		elog(FATAL, "pg_database instance has an invalid OID");

	path = ExpandDatabasePath(dbpath);
	if (path == NULL)
		elog(ERROR, "Unable to locate path '%s'"
			 "\n\tThis may be due to a missing environment variable"
			 " in the server", dbpath);

	/* stop the vacuum daemon (dead code...) */
	stop_vacuum(dbpath, dbname);

	/*
	 * Obtain exclusive lock on pg_database.  We need this to ensure
	 * that no new backend starts up in the target database while we
	 * are deleting it.  (Actually, a new backend might still manage to
	 * start up, because it will read pg_database without any locking
	 * to discover the database's OID.  But it will detect its error
	 * in ReverifyMyDatabase and shut down before any serious damage
	 * is done.  See postinit.c.)
	 */
	pgdbrel = heap_openr(DatabaseRelationName, AccessExclusiveLock);

	/*
	 * Check for active backends in the target database.
	 */
	if (DatabaseHasActiveBackends(db_id))
		elog(ERROR, "Database '%s' has running backends, can't destroy it",
			 dbname);

	/*
	 * Find the database's tuple by OID (should be unique, we trust).
	 */
	ScanKeyEntryInitialize(&key, 0, ObjectIdAttributeNumber,
						   F_OIDEQ, ObjectIdGetDatum(db_id));

	pgdbscan = heap_beginscan(pgdbrel, 0, SnapshotNow, 1, &key);

	tup = heap_getnext(pgdbscan, 0);
	if (!HeapTupleIsValid(tup))
	{
		heap_close(pgdbrel, AccessExclusiveLock);
		elog(ERROR, "Database '%s', OID %u, not found in pg_database",
			 dbname, db_id);
	}

	/*** Delete any comments associated with the database ***/
	
	DeleteComments(db_id);

	/*
	 * Houston, we have launch commit...
	 *
	 * Remove the database's tuple from pg_database.
	 */
	heap_delete(pgdbrel, &tup->t_self, NULL);

	heap_endscan(pgdbscan);

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
	 * Remove the database's subdirectory and everything in it.
	 */
	snprintf(buf, sizeof(buf), "rm -r '%s'", path);
	system(buf);
}

static HeapTuple
get_pg_dbtup(char *command, char *dbname, Relation dbrel)
{
	HeapTuple	dbtup;
	HeapTuple	tup;
	HeapScanDesc scan;
	ScanKeyData scanKey;

	ScanKeyEntryInitialize(&scanKey, 0, Anum_pg_database_datname,
						   F_NAMEEQ, NameGetDatum(dbname));

	scan = heap_beginscan(dbrel, 0, SnapshotNow, 1, &scanKey);
	if (!HeapScanIsValid(scan))
		elog(ERROR, "%s: cannot begin scan of pg_database", command);

	/*
	 * since we want to return the tuple out of this proc, and we're going
	 * to close the relation, copy the tuple and return the copy.
	 */
	tup = heap_getnext(scan, 0);

	if (HeapTupleIsValid(tup))
		dbtup = heap_copytuple(tup);
	else
		dbtup = tup;

	heap_endscan(scan);
	return dbtup;
}

/*
 *	check_permissions() -- verify that the user is permitted to do this.
 *
 *	If the user is not allowed to carry out this operation, this routine
 *	elog(ERROR, ...)s, which will abort the xact.  As a side effect, the
 *	user's pg_user tuple OID is returned in userIdP and the target database's
 *	OID is returned in dbIdP.
 */

static void
check_permissions(char *command,
				  char *dbpath,
				  char *dbname,
				  Oid *dbIdP,
				  int4 *userIdP)
{
	Relation	dbrel;
	HeapTuple	dbtup,
				utup;
	int4		dbowner = 0;
	char		use_createdb;
	bool		dbfound;
	bool		use_super;
	char	   *userName;
	text	   *dbtext;
	char		path[MAXPGPATH];

	userName = GetPgUserName();
	utup = SearchSysCacheTuple(USENAME,
							   PointerGetDatum(userName),
							   0, 0, 0);
	Assert(utup);
	*userIdP = ((Form_pg_shadow) GETSTRUCT(utup))->usesysid;
	use_super = ((Form_pg_shadow) GETSTRUCT(utup))->usesuper;
	use_createdb = ((Form_pg_shadow) GETSTRUCT(utup))->usecreatedb;

	/* Check to make sure user has permission to use createdb */
	if (!use_createdb)
	{
		elog(ERROR, "user '%s' is not allowed to create/destroy databases",
			 userName);
	}

	/* Make sure we are not mucking with the template database */
	if (!strcmp(dbname, "template1"))
		elog(ERROR, "%s: cannot be executed on the template database", command);

	/* Check to make sure database is not the currently open database */
	if (!strcmp(dbname, DatabaseName))
		elog(ERROR, "%s: cannot be executed on an open database", command);

	/* Check to make sure database is owned by this user */

	/*
	 * Acquire exclusive lock on pg_database from the beginning, even though
	 * we only need read access right here, to avoid potential deadlocks
	 * from upgrading our lock later.  (Is this still necessary?  Could we
	 * use something weaker than exclusive lock?)
	 */
	dbrel = heap_openr(DatabaseRelationName, AccessExclusiveLock);

	dbtup = get_pg_dbtup(command, dbname, dbrel);
	dbfound = HeapTupleIsValid(dbtup);

	if (dbfound)
	{
		dbowner = (int4) heap_getattr(dbtup,
									  Anum_pg_database_datdba,
									  RelationGetDescr(dbrel),
									  (char *) NULL);
		*dbIdP = dbtup->t_data->t_oid;
		dbtext = (text *) heap_getattr(dbtup,
									   Anum_pg_database_datpath,
									   RelationGetDescr(dbrel),
									   (char *) NULL);

		strncpy(path, VARDATA(dbtext), (VARSIZE(dbtext) - VARHDRSZ));
		*(path + VARSIZE(dbtext) - VARHDRSZ) = '\0';
	}
	else
		*dbIdP = InvalidOid;

	/* We will keep the lock on dbrel until end of transaction. */
	heap_close(dbrel, NoLock);

	/*
	 * Now be sure that the user is allowed to do this.
	 */

	if (dbfound && !strcmp(command, "createdb"))
	{

		elog(ERROR, "createdb: database '%s' already exists", dbname);

	}
	else if (!dbfound && !strcmp(command, "destroydb"))
	{

		elog(ERROR, "destroydb: database '%s' does not exist", dbname);

	}
	else if (dbfound && !strcmp(command, "destroydb")
			 && dbowner != *userIdP && use_super == false)
	{

		elog(ERROR, "%s: database '%s' is not owned by you", command, dbname);

	}

	if (dbfound && !strcmp(command, "destroydb"))
		strcpy(dbpath, path);
}	/* check_permissions() */

/*
 *	stop_vacuum -- stop the vacuum daemon on the database, if one is running.
 *
 *	This is currently dead code, since we don't *have* vacuum daemons.
 *	If you want to re-enable it, think about the interlock against deleting
 *	a database out from under running backends, in destroydb() above.
 */
static void
stop_vacuum(char *dbpath, char *dbname)
{
#ifdef NOT_USED
	char		filename[MAXPGPATH];
	FILE	   *fp;
	int			pid;

	if (strchr(dbpath, SEP_CHAR) != 0)
	{
		snprintf(filename, sizeof(filename), "%s%cbase%c%s%c%s.vacuum",
				 DataDir, SEP_CHAR, SEP_CHAR, dbname, SEP_CHAR, dbname);
	}
	else
		snprintf(filename, sizeof(filename), "%s%c%s.vacuum",
				 dbpath, SEP_CHAR, dbname);

#ifndef __CYGWIN32__
	if ((fp = AllocateFile(filename, "r")) != NULL)
#else
	if ((fp = AllocateFile(filename, "rb")) != NULL)
#endif
	{
		fscanf(fp, "%d", &pid);
		FreeFile(fp);
		if (kill(pid, SIGKILLDAEMON1) < 0)
		{
			elog(ERROR, "can't kill vacuum daemon (pid %d) on '%s'",
				 pid, dbname);
		}
	}
#endif
}
