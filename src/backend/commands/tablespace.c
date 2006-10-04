/*-------------------------------------------------------------------------
 *
 * tablespace.c
 *	  Commands to manipulate table spaces
 *
 * Tablespaces in PostgreSQL are designed to allow users to determine
 * where the data file(s) for a given database object reside on the file
 * system.
 *
 * A tablespace represents a directory on the file system. At tablespace
 * creation time, the directory must be empty. To simplify things and
 * remove the possibility of having file name conflicts, we isolate
 * files within a tablespace into database-specific subdirectories.
 *
 * To support file access via the information given in RelFileNode, we
 * maintain a symbolic-link map in $PGDATA/pg_tblspc. The symlinks are
 * named by tablespace OIDs and point to the actual tablespace directories.
 * Thus the full path to an arbitrary file is
 *			$PGDATA/pg_tblspc/spcoid/dboid/relfilenode
 *
 * There are two tablespaces created at initdb time: pg_global (for shared
 * tables) and pg_default (for everything else).  For backwards compatibility
 * and to remain functional on platforms without symlinks, these tablespaces
 * are accessed specially: they are respectively
 *			$PGDATA/global/relfilenode
 *			$PGDATA/base/dboid/relfilenode
 *
 * To allow CREATE DATABASE to give a new database a default tablespace
 * that's different from the template database's default, we make the
 * provision that a zero in pg_class.reltablespace means the database's
 * default tablespace.	Without this, CREATE DATABASE would have to go in
 * and munge the system catalogs of the new database.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/tablespace.c,v 1.39 2006/10/04 00:29:51 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"


/* GUC variable */
char	   *default_tablespace = NULL;


static bool remove_tablespace_directories(Oid tablespaceoid, bool redo);
static void set_short_version(const char *path);


/*
 * Each database using a table space is isolated into its own name space
 * by a subdirectory named for the database OID.  On first creation of an
 * object in the tablespace, create the subdirectory.  If the subdirectory
 * already exists, just fall through quietly.
 *
 * isRedo indicates that we are creating an object during WAL replay.
 * In this case we will cope with the possibility of the tablespace
 * directory not being there either --- this could happen if we are
 * replaying an operation on a table in a subsequently-dropped tablespace.
 * We handle this by making a directory in the place where the tablespace
 * symlink would normally be.  This isn't an exact replay of course, but
 * it's the best we can do given the available information.
 *
 * If tablespaces are not supported, you might think this could be a no-op,
 * but you'd be wrong: we still need it in case we have to re-create a
 * database subdirectory (of $PGDATA/base) during WAL replay.
 */
void
TablespaceCreateDbspace(Oid spcNode, Oid dbNode, bool isRedo)
{
	struct stat st;
	char	   *dir;

	/*
	 * The global tablespace doesn't have per-database subdirectories, so
	 * nothing to do for it.
	 */
	if (spcNode == GLOBALTABLESPACE_OID)
		return;

	Assert(OidIsValid(spcNode));
	Assert(OidIsValid(dbNode));

	dir = GetDatabasePath(dbNode, spcNode);

	if (stat(dir, &st) < 0)
	{
		if (errno == ENOENT)
		{
			/*
			 * Acquire TablespaceCreateLock to ensure that no DROP TABLESPACE
			 * or TablespaceCreateDbspace is running concurrently.
			 */
			LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

			/*
			 * Recheck to see if someone created the directory while we were
			 * waiting for lock.
			 */
			if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
			{
				/* need not do anything */
			}
			else
			{
				/* OK, go for it */
				if (mkdir(dir, S_IRWXU) < 0)
				{
					char	   *parentdir;

					if (errno != ENOENT || !isRedo)
						ereport(ERROR,
								(errcode_for_file_access(),
							  errmsg("could not create directory \"%s\": %m",
									 dir)));
					/* Try to make parent directory too */
					parentdir = pstrdup(dir);
					get_parent_directory(parentdir);
					if (mkdir(parentdir, S_IRWXU) < 0)
						ereport(ERROR,
								(errcode_for_file_access(),
							  errmsg("could not create directory \"%s\": %m",
									 parentdir)));
					pfree(parentdir);
					if (mkdir(dir, S_IRWXU) < 0)
						ereport(ERROR,
								(errcode_for_file_access(),
							  errmsg("could not create directory \"%s\": %m",
									 dir)));
				}
			}

			LWLockRelease(TablespaceCreateLock);
		}
		else
		{
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m", dir)));
		}
	}
	else
	{
		/* be paranoid */
		if (!S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" exists but is not a directory",
							dir)));
	}

	pfree(dir);
}

/*
 * Create a table space
 *
 * Only superusers can create a tablespace. This seems a reasonable restriction
 * since we're determining the system layout and, anyway, we probably have
 * root if we're doing this kind of activity
 */
void
CreateTableSpace(CreateTableSpaceStmt *stmt)
{
#ifdef HAVE_SYMLINK
	Relation	rel;
	Datum		values[Natts_pg_tablespace];
	char		nulls[Natts_pg_tablespace];
	HeapTuple	tuple;
	Oid			tablespaceoid;
	char	   *location;
	char	   *linkloc;
	Oid			ownerId;

	/* validate */

	/* don't call this in a transaction block */
	PreventTransactionChain((void *) stmt, "CREATE TABLESPACE");

	/* Must be super user */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create tablespace \"%s\"",
						stmt->tablespacename),
				 errhint("Must be superuser to create a tablespace.")));

	/* However, the eventual owner of the tablespace need not be */
	if (stmt->owner)
		ownerId = get_roleid_checked(stmt->owner);
	else
		ownerId = GetUserId();

	/* Unix-ify the offered path, and strip any trailing slashes */
	location = pstrdup(stmt->location);
	canonicalize_path(location);

	/* disallow quotes, else CREATE DATABASE would be at risk */
	if (strchr(location, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
			   errmsg("tablespace location may not contain single quotes")));

	/*
	 * Allowing relative paths seems risky
	 *
	 * this also helps us ensure that location is not empty or whitespace
	 */
	if (!is_absolute_path(location))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location must be an absolute path")));

	/*
	 * Check that location isn't too long. Remember that we're going to append
	 * '/<dboid>/<relid>.<nnn>'  (XXX but do we ever form the whole path
	 * explicitly?	This may be overly conservative.)
	 */
	if (strlen(location) >= (MAXPGPATH - 1 - 10 - 1 - 10 - 1 - 10))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location \"%s\" is too long",
						location)));

	/*
	 * Disallow creation of tablespaces named "pg_xxx"; we reserve this
	 * namespace for system purposes.
	 */
	if (!allowSystemTableMods && IsReservedName(stmt->tablespacename))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"",
						stmt->tablespacename),
		errdetail("The prefix \"pg_\" is reserved for system tablespaces.")));

	/*
	 * Check that there is no other tablespace by this name.  (The unique
	 * index would catch this anyway, but might as well give a friendlier
	 * message.)
	 */
	if (OidIsValid(get_tablespace_oid(stmt->tablespacename)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						stmt->tablespacename)));

	/*
	 * Insert tuple into pg_tablespace.  The purpose of doing this first is to
	 * lock the proposed tablename against other would-be creators. The
	 * insertion will roll back if we find problems below.
	 */
	rel = heap_open(TableSpaceRelationId, RowExclusiveLock);

	MemSet(nulls, ' ', Natts_pg_tablespace);

	values[Anum_pg_tablespace_spcname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->tablespacename));
	values[Anum_pg_tablespace_spcowner - 1] =
		ObjectIdGetDatum(ownerId);
	values[Anum_pg_tablespace_spclocation - 1] =
		DirectFunctionCall1(textin, CStringGetDatum(location));
	nulls[Anum_pg_tablespace_spcacl - 1] = 'n';

	tuple = heap_formtuple(rel->rd_att, values, nulls);

	tablespaceoid = simple_heap_insert(rel, tuple);

	CatalogUpdateIndexes(rel, tuple);

	heap_freetuple(tuple);

	/* Record dependency on owner */
	recordDependencyOnOwner(TableSpaceRelationId, tablespaceoid, ownerId);

	/*
	 * Attempt to coerce target directory to safe permissions.	If this fails,
	 * it doesn't exist or has the wrong owner.
	 */
	if (chmod(location, 0700) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not set permissions on directory \"%s\": %m",
						location)));

	/*
	 * Check the target directory is empty.
	 */
	if (!directory_is_empty(location))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("directory \"%s\" is not empty",
						location)));

	/*
	 * Create the PG_VERSION file in the target directory.	This has several
	 * purposes: to make sure we can write in the directory, to prevent
	 * someone from creating another tablespace pointing at the same directory
	 * (the emptiness check above will fail), and to label tablespace
	 * directories by PG version.
	 */
	set_short_version(location);

	/*
	 * All seems well, create the symlink
	 */
	linkloc = (char *) palloc(10 + 10 + 1);
	sprintf(linkloc, "pg_tblspc/%u", tablespaceoid);

	if (symlink(location, linkloc) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create symbolic link \"%s\": %m",
						linkloc)));

	/* Record the filesystem change in XLOG */
	{
		xl_tblspc_create_rec xlrec;
		XLogRecData rdata[2];

		xlrec.ts_id = tablespaceoid;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = offsetof(xl_tblspc_create_rec, ts_path);
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = &(rdata[1]);

		rdata[1].data = (char *) location;
		rdata[1].len = strlen(location) + 1;
		rdata[1].buffer = InvalidBuffer;
		rdata[1].next = NULL;

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_CREATE, rdata);
	}

	pfree(linkloc);
	pfree(location);

	/* We keep the lock on pg_tablespace until commit */
	heap_close(rel, NoLock);
#else							/* !HAVE_SYMLINK */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tablespaces are not supported on this platform")));
#endif   /* HAVE_SYMLINK */
}

/*
 * Drop a table space
 *
 * Be careful to check that the tablespace is empty.
 */
void
DropTableSpace(DropTableSpaceStmt *stmt)
{
#ifdef HAVE_SYMLINK
	char	   *tablespacename = stmt->tablespacename;
	HeapScanDesc scandesc;
	Relation	rel;
	HeapTuple	tuple;
	ScanKeyData entry[1];
	Oid			tablespaceoid;

	/* don't call this in a transaction block */
	PreventTransactionChain((void *) stmt, "DROP TABLESPACE");

	/*
	 * Find the target tuple
	 */
	rel = heap_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
	{
		if (!stmt->missing_ok)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tablespace \"%s\" does not exist",
							tablespacename)));
		}
		else
		{
			ereport(NOTICE,
					(errmsg("tablespace \"%s\" does not exist, skipping",
							tablespacename)));
			/* XXX I assume I need one or both of these next two calls */
			heap_endscan(scandesc);
			heap_close(rel, NoLock);
		}
		return;
	}

	tablespaceoid = HeapTupleGetOid(tuple);

	/* Must be tablespace owner */
	if (!pg_tablespace_ownercheck(tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TABLESPACE,
					   tablespacename);

	/* Disallow drop of the standard tablespaces, even by superuser */
	if (tablespaceoid == GLOBALTABLESPACE_OID ||
		tablespaceoid == DEFAULTTABLESPACE_OID)
		aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_TABLESPACE,
					   tablespacename);

	/*
	 * Remove the pg_tablespace tuple (this will roll back if we fail below)
	 */
	simple_heap_delete(rel, &tuple->t_self);

	heap_endscan(scandesc);

	/*
	 * Remove any comments on this tablespace.
	 */
	DeleteSharedComments(tablespaceoid, TableSpaceRelationId);

	/*
	 * Remove dependency on owner.
	 */
	deleteSharedDependencyRecordsFor(TableSpaceRelationId, tablespaceoid);

	/*
	 * Acquire TablespaceCreateLock to ensure that no TablespaceCreateDbspace
	 * is running concurrently.
	 */
	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

	/*
	 * Try to remove the physical infrastructure
	 */
	if (!remove_tablespace_directories(tablespaceoid, false))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tablespace \"%s\" is not empty",
						tablespacename)));

	/* Record the filesystem change in XLOG */
	{
		xl_tblspc_drop_rec xlrec;
		XLogRecData rdata[1];

		xlrec.ts_id = tablespaceoid;
		rdata[0].data = (char *) &xlrec;
		rdata[0].len = sizeof(xl_tblspc_drop_rec);
		rdata[0].buffer = InvalidBuffer;
		rdata[0].next = NULL;

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_DROP, rdata);
	}

	/*
	 * Note: because we checked that the tablespace was empty, there should be
	 * no need to worry about flushing shared buffers or free space map
	 * entries for relations in the tablespace.
	 */

	/*
	 * Allow TablespaceCreateDbspace again.
	 */
	LWLockRelease(TablespaceCreateLock);

	/* We keep the lock on pg_tablespace until commit */
	heap_close(rel, NoLock);
#else							/* !HAVE_SYMLINK */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tablespaces are not supported on this platform")));
#endif   /* HAVE_SYMLINK */
}

/*
 * remove_tablespace_directories: attempt to remove filesystem infrastructure
 *
 * Returns TRUE if successful, FALSE if some subdirectory is not empty
 *
 * redo indicates we are redoing a drop from XLOG; okay if nothing there
 */
static bool
remove_tablespace_directories(Oid tablespaceoid, bool redo)
{
	char	   *location;
	DIR		   *dirdesc;
	struct dirent *de;
	char	   *subfile;
	struct stat st;

	location = (char *) palloc(10 + 10 + 1);
	sprintf(location, "pg_tblspc/%u", tablespaceoid);

	/*
	 * Check if the tablespace still contains any files.  We try to rmdir each
	 * per-database directory we find in it.  rmdir failure implies there are
	 * still files in that subdirectory, so give up.  (We do not have to worry
	 * about undoing any already completed rmdirs, since the next attempt to
	 * use the tablespace from that database will simply recreate the
	 * subdirectory via TablespaceCreateDbspace.)
	 *
	 * Since we hold TablespaceCreateLock, no one else should be creating any
	 * fresh subdirectories in parallel. It is possible that new files are
	 * being created within subdirectories, though, so the rmdir call could
	 * fail.  Worst consequence is a less friendly error message.
	 */
	dirdesc = AllocateDir(location);
	if (dirdesc == NULL)
	{
		if (redo && errno == ENOENT)
		{
			pfree(location);
			return true;
		}
		/* else let ReadDir report the error */
	}

	while ((de = ReadDir(dirdesc, location)) != NULL)
	{
		/* Note we ignore PG_VERSION for the nonce */
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0 ||
			strcmp(de->d_name, "PG_VERSION") == 0)
			continue;

		subfile = palloc(strlen(location) + 1 + strlen(de->d_name) + 1);
		sprintf(subfile, "%s/%s", location, de->d_name);

		/* This check is just to deliver a friendlier error message */
		if (!directory_is_empty(subfile))
		{
			FreeDir(dirdesc);
			return false;
		}

		/* Do the real deed */
		if (rmdir(subfile) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not delete directory \"%s\": %m",
							subfile)));

		pfree(subfile);
	}

	FreeDir(dirdesc);

	/*
	 * Okay, try to unlink PG_VERSION (we allow it to not be there, even in
	 * non-REDO case, for robustness).
	 */
	subfile = palloc(strlen(location) + 11 + 1);
	sprintf(subfile, "%s/PG_VERSION", location);

	if (unlink(subfile) < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							subfile)));
	}

	pfree(subfile);

	/*
	 * Okay, try to remove the symlink.  We must however deal with the
	 * possibility that it's a directory instead of a symlink --- this could
	 * happen during WAL replay (see TablespaceCreateDbspace), and it is also
	 * the normal case on Windows.
	 */
	if (lstat(location, &st) == 0 && S_ISDIR(st.st_mode))
	{
		if (rmdir(location) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							location)));
	}
	else
	{
		if (unlink(location) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							location)));
	}

	pfree(location);

	return true;
}

/*
 * write out the PG_VERSION file in the specified directory
 */
static void
set_short_version(const char *path)
{
	char	   *short_version;
	bool		gotdot = false;
	int			end;
	char	   *fullname;
	FILE	   *version_file;

	/* Construct short version string (should match initdb.c) */
	short_version = pstrdup(PG_VERSION);

	for (end = 0; short_version[end] != '\0'; end++)
	{
		if (short_version[end] == '.')
		{
			Assert(end != 0);
			if (gotdot)
				break;
			else
				gotdot = true;
		}
		else if (short_version[end] < '0' || short_version[end] > '9')
		{
			/* gone past digits and dots */
			break;
		}
	}
	Assert(end > 0 && short_version[end - 1] != '.' && gotdot);
	short_version[end] = '\0';

	/* Now write the file */
	fullname = palloc(strlen(path) + 11 + 1);
	sprintf(fullname, "%s/PG_VERSION", path);
	version_file = AllocateFile(fullname, PG_BINARY_W);
	if (version_file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						fullname)));
	fprintf(version_file, "%s\n", short_version);
	if (FreeFile(version_file))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						fullname)));

	pfree(fullname);
	pfree(short_version);
}

/*
 * Check if a directory is empty.
 *
 * This probably belongs somewhere else, but not sure where...
 */
bool
directory_is_empty(const char *path)
{
	DIR		   *dirdesc;
	struct dirent *de;

	dirdesc = AllocateDir(path);

	while ((de = ReadDir(dirdesc, path)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;
		FreeDir(dirdesc);
		return false;
	}

	FreeDir(dirdesc);
	return true;
}

/*
 * Rename a tablespace
 */
void
RenameTableSpace(const char *oldname, const char *newname)
{
	Relation	rel;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	tup;
	HeapTuple	newtuple;
	Form_pg_tablespace newform;

	/* Search pg_tablespace */
	rel = heap_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(oldname));
	scan = heap_beginscan(rel, SnapshotNow, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						oldname)));

	newtuple = heap_copytuple(tup);
	newform = (Form_pg_tablespace) GETSTRUCT(newtuple);

	heap_endscan(scan);

	/* Must be owner */
	if (!pg_tablespace_ownercheck(HeapTupleGetOid(newtuple), GetUserId()))
		aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_TABLESPACE, oldname);

	/* Validate new name */
	if (!allowSystemTableMods && IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"", newname),
		errdetail("The prefix \"pg_\" is reserved for system tablespaces.")));

	/* Make sure the new name doesn't exist */
	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(newname));
	scan = heap_beginscan(rel, SnapshotNow, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						newname)));

	heap_endscan(scan);

	/* OK, update the entry */
	namestrcpy(&(newform->spcname), newname);

	simple_heap_update(rel, &newtuple->t_self, newtuple);
	CatalogUpdateIndexes(rel, newtuple);

	heap_close(rel, NoLock);
}

/*
 * Change tablespace owner
 */
void
AlterTableSpaceOwner(const char *name, Oid newOwnerId)
{
	Relation	rel;
	ScanKeyData entry[1];
	HeapScanDesc scandesc;
	Form_pg_tablespace spcForm;
	HeapTuple	tup;

	/* Search pg_tablespace */
	rel = heap_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(name));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tup = heap_getnext(scandesc, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist", name)));

	spcForm = (Form_pg_tablespace) GETSTRUCT(tup);

	/*
	 * If the new owner is the same as the existing owner, consider the
	 * command to have succeeded.  This is for dump restoration purposes.
	 */
	if (spcForm->spcowner != newOwnerId)
	{
		Datum		repl_val[Natts_pg_tablespace];
		char		repl_null[Natts_pg_tablespace];
		char		repl_repl[Natts_pg_tablespace];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Otherwise, must be owner of the existing object */
		if (!pg_tablespace_ownercheck(HeapTupleGetOid(tup), GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TABLESPACE,
						   name);

		/* Must be able to become new owner */
		check_is_member_of_role(GetUserId(), newOwnerId);

		/*
		 * Normally we would also check for create permissions here, but there
		 * are none for tablespaces so we follow what rename tablespace does
		 * and omit the create permissions check.
		 *
		 * NOTE: Only superusers may create tablespaces to begin with and so
		 * initially only a superuser would be able to change its ownership
		 * anyway.
		 */

		memset(repl_null, ' ', sizeof(repl_null));
		memset(repl_repl, ' ', sizeof(repl_repl));

		repl_repl[Anum_pg_tablespace_spcowner - 1] = 'r';
		repl_val[Anum_pg_tablespace_spcowner - 1] = ObjectIdGetDatum(newOwnerId);

		/*
		 * Determine the modified ACL for the new owner.  This is only
		 * necessary when the ACL is non-null.
		 */
		aclDatum = heap_getattr(tup,
								Anum_pg_tablespace_spcacl,
								RelationGetDescr(rel),
								&isNull);
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 spcForm->spcowner, newOwnerId);
			repl_repl[Anum_pg_tablespace_spcacl - 1] = 'r';
			repl_val[Anum_pg_tablespace_spcacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modifytuple(tup, RelationGetDescr(rel), repl_val, repl_null, repl_repl);

		simple_heap_update(rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(rel, newtuple);

		heap_freetuple(newtuple);

		/* Update owner dependency reference */
		changeDependencyOnOwner(TableSpaceRelationId, HeapTupleGetOid(tup),
								newOwnerId);
	}

	heap_endscan(scandesc);
	heap_close(rel, NoLock);
}


/*
 * Routines for handling the GUC variable 'default_tablespace'.
 */

/* assign_hook: validate new default_tablespace, do extra actions as needed */
const char *
assign_default_tablespace(const char *newval, bool doit, GucSource source)
{
	/*
	 * If we aren't inside a transaction, we cannot do database access so
	 * cannot verify the name.	Must accept the value on faith.
	 */
	if (IsTransactionState())
	{
		if (newval[0] != '\0' &&
			!OidIsValid(get_tablespace_oid(newval)))
		{
			if (source >= PGC_S_INTERACTIVE)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("tablespace \"%s\" does not exist",
								newval)));
			return NULL;
		}
	}

	return newval;
}

/*
 * GetDefaultTablespace -- get the OID of the current default tablespace
 *
 * May return InvalidOid to indicate "use the database's default tablespace"
 *
 * This exists to hide (and possibly optimize the use of) the
 * default_tablespace GUC variable.
 */
Oid
GetDefaultTablespace(void)
{
	Oid			result;

	/* Fast path for default_tablespace == "" */
	if (default_tablespace == NULL || default_tablespace[0] == '\0')
		return InvalidOid;

	/*
	 * It is tempting to cache this lookup for more speed, but then we would
	 * fail to detect the case where the tablespace was dropped since the GUC
	 * variable was set.  Note also that we don't complain if the value fails
	 * to refer to an existing tablespace; we just silently return InvalidOid,
	 * causing the new object to be created in the database's tablespace.
	 */
	result = get_tablespace_oid(default_tablespace);

	/*
	 * Allow explicit specification of database's default tablespace in
	 * default_tablespace without triggering permissions checks.
	 */
	if (result == MyDatabaseTableSpace)
		result = InvalidOid;
	return result;
}


/*
 * get_tablespace_oid - given a tablespace name, look up the OID
 *
 * Returns InvalidOid if tablespace name not found.
 */
Oid
get_tablespace_oid(const char *tablespacename)
{
	Oid			result;
	Relation	rel;
	HeapScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/* Search pg_tablespace */
	rel = heap_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (HeapTupleIsValid(tuple))
		result = HeapTupleGetOid(tuple);
	else
		result = InvalidOid;

	heap_endscan(scandesc);
	heap_close(rel, AccessShareLock);

	return result;
}

/*
 * get_tablespace_name - given a tablespace OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such tablespace.
 */
char *
get_tablespace_name(Oid spc_oid)
{
	char	   *result;
	Relation	rel;
	HeapScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/* Search pg_tablespace */
	rel = heap_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spc_oid));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_tablespace) GETSTRUCT(tuple))->spcname));
	else
		result = NULL;

	heap_endscan(scandesc);
	heap_close(rel, AccessShareLock);

	return result;
}


/*
 * TABLESPACE resource manager's routines
 */
void
tblspc_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_TBLSPC_CREATE)
	{
		xl_tblspc_create_rec *xlrec = (xl_tblspc_create_rec *) XLogRecGetData(record);
		char	   *location = xlrec->ts_path;
		char	   *linkloc;

		/*
		 * Attempt to coerce target directory to safe permissions.	If this
		 * fails, it doesn't exist or has the wrong owner.
		 */
		if (chmod(location, 0700) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
				  errmsg("could not set permissions on directory \"%s\": %m",
						 location)));

		/* Create or re-create the PG_VERSION file in the target directory */
		set_short_version(location);

		/* Create the symlink if not already present */
		linkloc = (char *) palloc(10 + 10 + 1);
		sprintf(linkloc, "pg_tblspc/%u", xlrec->ts_id);

		if (symlink(location, linkloc) < 0)
		{
			if (errno != EEXIST)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not create symbolic link \"%s\": %m",
								linkloc)));
		}

		pfree(linkloc);
	}
	else if (info == XLOG_TBLSPC_DROP)
	{
		xl_tblspc_drop_rec *xlrec = (xl_tblspc_drop_rec *) XLogRecGetData(record);

		if (!remove_tablespace_directories(xlrec->ts_id, true))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tablespace %u is not empty",
							xlrec->ts_id)));
	}
	else
		elog(PANIC, "tblspc_redo: unknown op code %u", info);
}

void
tblspc_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_TBLSPC_CREATE)
	{
		xl_tblspc_create_rec *xlrec = (xl_tblspc_create_rec *) rec;

		appendStringInfo(buf, "create ts: %u \"%s\"",
						 xlrec->ts_id, xlrec->ts_path);
	}
	else if (info == XLOG_TBLSPC_DROP)
	{
		xl_tblspc_drop_rec *xlrec = (xl_tblspc_drop_rec *) rec;

		appendStringInfo(buf, "drop ts: %u", xlrec->ts_id);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
