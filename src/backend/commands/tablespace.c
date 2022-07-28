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
 * There is also a per-cluster version directory in each tablespace.
 * Thus the full path to an arbitrary file is
 *			$PGDATA/pg_tblspc/spcoid/PG_MAJORVER_CATVER/dboid/relfilenode
 * e.g.
 *			$PGDATA/pg_tblspc/20981/PG_9.0_201002161/719849/83292814
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
 * default tablespace.  Without this, CREATE DATABASE would have to go in
 * and munge the system catalogs of the new database.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/seclabel.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "miscadmin.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/standby.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/varlena.h"

/* GUC variables */
char	   *default_tablespace = NULL;
char	   *temp_tablespaces = NULL;
bool		allow_in_place_tablespaces = false;


static void create_tablespace_directories(const char *location,
										  const Oid tablespaceoid);
static bool destroy_tablespace_directories(Oid tablespaceoid, bool redo);


/*
 * Each database using a table space is isolated into its own name space
 * by a subdirectory named for the database OID.  On first creation of an
 * object in the tablespace, create the subdirectory.  If the subdirectory
 * already exists, fall through quietly.
 *
 * isRedo indicates that we are creating an object during WAL replay.
 * In this case we will cope with the possibility of the tablespace
 * directory not being there either --- this could happen if we are
 * replaying an operation on a table in a subsequently-dropped tablespace.
 * We handle this by making a directory in the place where the tablespace
 * symlink would normally be.  This isn't an exact replay of course, but
 * it's the best we can do given the available information.
 *
 * If tablespaces are not supported, we still need it in case we have to
 * re-create a database subdirectory (of $PGDATA/base) during WAL replay.
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
		/* Directory does not exist? */
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
				/* Directory was created */
			}
			else
			{
				/* Directory creation failed? */
				if (MakePGDirectory(dir) < 0)
				{
					/* Failure other than not exists or not in WAL replay? */
					if (errno != ENOENT || !isRedo)
						ereport(ERROR,
								(errcode_for_file_access(),
								 errmsg("could not create directory \"%s\": %m",
										dir)));

					/*
					 * During WAL replay, it's conceivable that several levels
					 * of directories are missing if tablespaces are dropped
					 * further ahead of the WAL stream than we're currently
					 * replaying.  An easy way forward is to create them as
					 * plain directories and hope they are removed by further
					 * WAL replay if necessary.  If this also fails, there is
					 * trouble we cannot get out of, so just report that and
					 * bail out.
					 */
					if (pg_mkdir_p(dir, pg_dir_create_mode) < 0)
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
		/* Is it not a directory? */
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
Oid
CreateTableSpace(CreateTableSpaceStmt *stmt)
{
#ifdef HAVE_SYMLINK
	Relation	rel;
	Datum		values[Natts_pg_tablespace];
	bool		nulls[Natts_pg_tablespace];
	HeapTuple	tuple;
	Oid			tablespaceoid;
	char	   *location;
	Oid			ownerId;
	Datum		newOptions;
	bool		in_place;

	/* Must be super user */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create tablespace \"%s\"",
						stmt->tablespacename),
				 errhint("Must be superuser to create a tablespace.")));

	/* However, the eventual owner of the tablespace need not be */
	if (stmt->owner)
		ownerId = get_rolespec_oid(stmt->owner, false);
	else
		ownerId = GetUserId();

	/* Unix-ify the offered path, and strip any trailing slashes */
	location = pstrdup(stmt->location);
	canonicalize_path(location);

	/* disallow quotes, else CREATE DATABASE would be at risk */
	if (strchr(location, '\''))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("tablespace location cannot contain single quotes")));

	in_place = allow_in_place_tablespaces && strlen(location) == 0;

	/*
	 * Allowing relative paths seems risky
	 *
	 * This also helps us ensure that location is not empty or whitespace,
	 * unless specifying a developer-only in-place tablespace.
	 */
	if (!in_place && !is_absolute_path(location))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location must be an absolute path")));

	/*
	 * Check that location isn't too long. Remember that we're going to append
	 * 'PG_XXX/<dboid>/<relid>_<fork>.<nnn>'.  FYI, we never actually
	 * reference the whole path here, but MakePGDirectory() uses the first two
	 * parts.
	 */
	if (strlen(location) + 1 + strlen(TABLESPACE_VERSION_DIRECTORY) + 1 +
		OIDCHARS + 1 + OIDCHARS + 1 + FORKNAMECHARS + 1 + OIDCHARS > MAXPGPATH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location \"%s\" is too long",
						location)));

	/* Warn if the tablespace is in the data directory. */
	if (path_is_prefix_of_path(DataDir, location))
		ereport(WARNING,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("tablespace location should not be inside the data directory")));

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
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for tablespace names are violated.
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(stmt->tablespacename, "regress_", 8) != 0)
		elog(WARNING, "tablespaces created by regression test cases should have names starting with \"regress_\"");
#endif

	/*
	 * Check that there is no other tablespace by this name.  (The unique
	 * index would catch this anyway, but might as well give a friendlier
	 * message.)
	 */
	if (OidIsValid(get_tablespace_oid(stmt->tablespacename, true)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						stmt->tablespacename)));

	/*
	 * Insert tuple into pg_tablespace.  The purpose of doing this first is to
	 * lock the proposed tablename against other would-be creators. The
	 * insertion will roll back if we find problems below.
	 */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	MemSet(nulls, false, sizeof(nulls));

	tablespaceoid = GetNewOidWithIndex(rel, TablespaceOidIndexId,
									   Anum_pg_tablespace_oid);
	values[Anum_pg_tablespace_oid - 1] = ObjectIdGetDatum(tablespaceoid);
	values[Anum_pg_tablespace_spcname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->tablespacename));
	values[Anum_pg_tablespace_spcowner - 1] =
		ObjectIdGetDatum(ownerId);
	nulls[Anum_pg_tablespace_spcacl - 1] = true;

	/* Generate new proposed spcoptions (text array) */
	newOptions = transformRelOptions((Datum) 0,
									 stmt->options,
									 NULL, NULL, false, false);
	(void) tablespace_reloptions(newOptions, true);
	if (newOptions != (Datum) 0)
		values[Anum_pg_tablespace_spcoptions - 1] = newOptions;
	else
		nulls[Anum_pg_tablespace_spcoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* Record dependency on owner */
	recordDependencyOnOwner(TableSpaceRelationId, tablespaceoid, ownerId);

	/* Post creation hook for new tablespace */
	InvokeObjectPostCreateHook(TableSpaceRelationId, tablespaceoid, 0);

	create_tablespace_directories(location, tablespaceoid);

	/* Record the filesystem change in XLOG */
	{
		xl_tblspc_create_rec xlrec;

		xlrec.ts_id = tablespaceoid;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec,
						 offsetof(xl_tblspc_create_rec, ts_path));
		XLogRegisterData((char *) location, strlen(location) + 1);

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_CREATE);
	}

	/*
	 * Force synchronous commit, to minimize the window between creating the
	 * symlink on-disk and marking the transaction committed.  It's not great
	 * that there is any window at all, but definitely we don't want to make
	 * it larger than necessary.
	 */
	ForceSyncCommit();

	pfree(location);

	/* We keep the lock on pg_tablespace until commit */
	table_close(rel, NoLock);

	return tablespaceoid;
#else							/* !HAVE_SYMLINK */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tablespaces are not supported on this platform")));
	return InvalidOid;			/* keep compiler quiet */
#endif							/* HAVE_SYMLINK */
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
	TableScanDesc scandesc;
	Relation	rel;
	HeapTuple	tuple;
	Form_pg_tablespace spcform;
	ScanKeyData entry[1];
	Oid			tablespaceoid;
	char	   *detail;
	char	   *detail_log;

	/*
	 * Find the target tuple
	 */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
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
			table_endscan(scandesc);
			table_close(rel, NoLock);
		}
		return;
	}

	spcform = (Form_pg_tablespace) GETSTRUCT(tuple);
	tablespaceoid = spcform->oid;

	/* Must be tablespace owner */
	if (!pg_tablespace_ownercheck(tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLESPACE,
					   tablespacename);

	/* Disallow drop of the standard tablespaces, even by superuser */
	if (tablespaceoid == GLOBALTABLESPACE_OID ||
		tablespaceoid == DEFAULTTABLESPACE_OID)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE,
					   tablespacename);

	/* Check for pg_shdepend entries depending on this tablespace */
	if (checkSharedDependencies(TableSpaceRelationId, tablespaceoid,
								&detail, &detail_log))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("tablespace \"%s\" cannot be dropped because some objects depend on it",
						tablespacename),
				 errdetail_internal("%s", detail),
				 errdetail_log("%s", detail_log)));

	/* DROP hook for the tablespace being removed */
	InvokeObjectDropHook(TableSpaceRelationId, tablespaceoid, 0);

	/*
	 * Remove the pg_tablespace tuple (this will roll back if we fail below)
	 */
	CatalogTupleDelete(rel, &tuple->t_self);

	table_endscan(scandesc);

	/*
	 * Remove any comments or security labels on this tablespace.
	 */
	DeleteSharedComments(tablespaceoid, TableSpaceRelationId);
	DeleteSharedSecurityLabel(tablespaceoid, TableSpaceRelationId);

	/*
	 * Remove dependency on owner.
	 */
	deleteSharedDependencyRecordsFor(TableSpaceRelationId, tablespaceoid, 0);

	/*
	 * Acquire TablespaceCreateLock to ensure that no TablespaceCreateDbspace
	 * is running concurrently.
	 */
	LWLockAcquire(TablespaceCreateLock, LW_EXCLUSIVE);

	/*
	 * Try to remove the physical infrastructure.
	 */
	if (!destroy_tablespace_directories(tablespaceoid, false))
	{
		/*
		 * Not all files deleted?  However, there can be lingering empty files
		 * in the directories, left behind by for example DROP TABLE, that
		 * have been scheduled for deletion at next checkpoint (see comments
		 * in mdunlink() for details).  We could just delete them immediately,
		 * but we can't tell them apart from important data files that we
		 * mustn't delete.  So instead, we force a checkpoint which will clean
		 * out any lingering files, and try again.
		 *
		 * XXX On Windows, an unlinked file persists in the directory listing
		 * until no process retains an open handle for the file.  The DDL
		 * commands that schedule files for unlink send invalidation messages
		 * directing other PostgreSQL processes to close the files.  DROP
		 * TABLESPACE should not give up on the tablespace becoming empty
		 * until all relevant invalidation processing is complete.
		 */
		RequestCheckpoint(CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT);
		if (!destroy_tablespace_directories(tablespaceoid, false))
		{
			/* Still not empty, the files must be important then */
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tablespace \"%s\" is not empty",
							tablespacename)));
		}
	}

	/* Record the filesystem change in XLOG */
	{
		xl_tblspc_drop_rec xlrec;

		xlrec.ts_id = tablespaceoid;

		XLogBeginInsert();
		XLogRegisterData((char *) &xlrec, sizeof(xl_tblspc_drop_rec));

		(void) XLogInsert(RM_TBLSPC_ID, XLOG_TBLSPC_DROP);
	}

	/*
	 * Note: because we checked that the tablespace was empty, there should be
	 * no need to worry about flushing shared buffers or free space map
	 * entries for relations in the tablespace.
	 */

	/*
	 * Force synchronous commit, to minimize the window between removing the
	 * files on-disk and marking the transaction committed.  It's not great
	 * that there is any window at all, but definitely we don't want to make
	 * it larger than necessary.
	 */
	ForceSyncCommit();

	/*
	 * Allow TablespaceCreateDbspace again.
	 */
	LWLockRelease(TablespaceCreateLock);

	/* We keep the lock on pg_tablespace until commit */
	table_close(rel, NoLock);
#else							/* !HAVE_SYMLINK */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tablespaces are not supported on this platform")));
#endif							/* HAVE_SYMLINK */
}


/*
 * create_tablespace_directories
 *
 *	Attempt to create filesystem infrastructure linking $PGDATA/pg_tblspc/
 *	to the specified directory
 */
static void
create_tablespace_directories(const char *location, const Oid tablespaceoid)
{
	char	   *linkloc;
	char	   *location_with_version_dir;
	struct stat st;
	bool		in_place;

	linkloc = psprintf("pg_tblspc/%u", tablespaceoid);

	/*
	 * If we're asked to make an 'in place' tablespace, create the directory
	 * directly where the symlink would normally go.  This is a developer-only
	 * option for now, to facilitate regression testing.
	 */
	in_place = strlen(location) == 0;

	if (in_place)
	{
		if (MakePGDirectory(linkloc) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							linkloc)));
	}

	location_with_version_dir = psprintf("%s/%s", in_place ? linkloc : location,
										 TABLESPACE_VERSION_DIRECTORY);

	/*
	 * Attempt to coerce target directory to safe permissions.  If this fails,
	 * it doesn't exist or has the wrong owner.  Not needed for in-place mode,
	 * because in that case we created the directory with the desired
	 * permissions.
	 */
	if (!in_place && chmod(location, pg_dir_create_mode) != 0)
	{
		if (errno == ENOENT)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FILE),
					 errmsg("directory \"%s\" does not exist", location),
					 InRecovery ? errhint("Create this directory for the tablespace before "
										  "restarting the server.") : 0));
		else
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not set permissions on directory \"%s\": %m",
							location)));
	}

	/*
	 * The creation of the version directory prevents more than one tablespace
	 * in a single location.  This imitates TablespaceCreateDbspace(), but it
	 * ignores concurrency and missing parent directories.  The chmod() would
	 * have failed in the absence of a parent.  pg_tablespace_spcname_index
	 * prevents concurrency.
	 */
	if (stat(location_with_version_dir, &st) < 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat directory \"%s\": %m",
							location_with_version_dir)));
		else if (MakePGDirectory(location_with_version_dir) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create directory \"%s\": %m",
							location_with_version_dir)));
	}
	else if (!S_ISDIR(st.st_mode))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" exists but is not a directory",
						location_with_version_dir)));
	else if (!InRecovery)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("directory \"%s\" already in use as a tablespace",
						location_with_version_dir)));

	/*
	 * In recovery, remove old symlink, in case it points to the wrong place.
	 */
	if (!in_place && InRecovery)
		remove_tablespace_symlink(linkloc);

	/*
	 * Create the symlink under PGDATA
	 */
	if (!in_place && symlink(location, linkloc) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create symbolic link \"%s\": %m",
						linkloc)));

	pfree(linkloc);
	pfree(location_with_version_dir);
}


/*
 * destroy_tablespace_directories
 *
 * Attempt to remove filesystem infrastructure for the tablespace.
 *
 * 'redo' indicates we are redoing a drop from XLOG; in that case we should
 * not throw an ERROR for problems, just LOG them.  The worst consequence of
 * not removing files here would be failure to release some disk space, which
 * does not justify throwing an error that would require manual intervention
 * to get the database running again.
 *
 * Returns true if successful, false if some subdirectory is not empty
 */
static bool
destroy_tablespace_directories(Oid tablespaceoid, bool redo)
{
	char	   *linkloc;
	char	   *linkloc_with_version_dir;
	DIR		   *dirdesc;
	struct dirent *de;
	char	   *subfile;
	struct stat st;

	linkloc_with_version_dir = psprintf("pg_tblspc/%u/%s", tablespaceoid,
										TABLESPACE_VERSION_DIRECTORY);

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
	 *
	 * If redo is true then ENOENT is a likely outcome here, and we allow it
	 * to pass without comment.  In normal operation we still allow it, but
	 * with a warning.  This is because even though ProcessUtility disallows
	 * DROP TABLESPACE in a transaction block, it's possible that a previous
	 * DROP failed and rolled back after removing the tablespace directories
	 * and/or symlink.  We want to allow a new DROP attempt to succeed at
	 * removing the catalog entries (and symlink if still present), so we
	 * should not give a hard error here.
	 */
	dirdesc = AllocateDir(linkloc_with_version_dir);
	if (dirdesc == NULL)
	{
		if (errno == ENOENT)
		{
			if (!redo)
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not open directory \"%s\": %m",
								linkloc_with_version_dir)));
			/* The symlink might still exist, so go try to remove it */
			goto remove_symlink;
		}
		else if (redo)
		{
			/* in redo, just log other types of error */
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not open directory \"%s\": %m",
							linkloc_with_version_dir)));
			pfree(linkloc_with_version_dir);
			return false;
		}
		/* else let ReadDir report the error */
	}

	while ((de = ReadDir(dirdesc, linkloc_with_version_dir)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		subfile = psprintf("%s/%s", linkloc_with_version_dir, de->d_name);

		/* This check is just to deliver a friendlier error message */
		if (!redo && !directory_is_empty(subfile))
		{
			FreeDir(dirdesc);
			pfree(subfile);
			pfree(linkloc_with_version_dir);
			return false;
		}

		/* remove empty directory */
		if (rmdir(subfile) < 0)
			ereport(redo ? LOG : ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							subfile)));

		pfree(subfile);
	}

	FreeDir(dirdesc);

	/* remove version directory */
	if (rmdir(linkloc_with_version_dir) < 0)
	{
		ereport(redo ? LOG : ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove directory \"%s\": %m",
						linkloc_with_version_dir)));
		pfree(linkloc_with_version_dir);
		return false;
	}

	/*
	 * Try to remove the symlink.  We must however deal with the possibility
	 * that it's a directory instead of a symlink --- this could happen during
	 * WAL replay (see TablespaceCreateDbspace), and it is also the case on
	 * Windows where junction points lstat() as directories.
	 *
	 * Note: in the redo case, we'll return true if this final step fails;
	 * there's no point in retrying it.  Also, ENOENT should provoke no more
	 * than a warning.
	 */
remove_symlink:
	linkloc = pstrdup(linkloc_with_version_dir);
	get_parent_directory(linkloc);
	if (lstat(linkloc, &st) < 0)
	{
		int			saved_errno = errno;

		ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						linkloc)));
	}
	else if (S_ISDIR(st.st_mode))
	{
		if (rmdir(linkloc) < 0)
		{
			int			saved_errno = errno;

			ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							linkloc)));
		}
	}
#ifdef S_ISLNK
	else if (S_ISLNK(st.st_mode))
	{
		if (unlink(linkloc) < 0)
		{
			int			saved_errno = errno;

			ereport(redo ? LOG : (saved_errno == ENOENT ? WARNING : ERROR),
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							linkloc)));
		}
	}
#endif
	else
	{
		/* Refuse to remove anything that's not a directory or symlink */
		ereport(redo ? LOG : ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is not a directory or symbolic link",
						linkloc)));
	}

	pfree(linkloc_with_version_dir);
	pfree(linkloc);

	return true;
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
 *	remove_tablespace_symlink
 *
 * This function removes symlinks in pg_tblspc.  On Windows, junction points
 * act like directories so we must be able to apply rmdir.  This function
 * works like the symlink removal code in destroy_tablespace_directories,
 * except that failure to remove is always an ERROR.  But if the file doesn't
 * exist at all, that's OK.
 */
void
remove_tablespace_symlink(const char *linkloc)
{
	struct stat st;

	if (lstat(linkloc, &st) < 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", linkloc)));
	}

	if (S_ISDIR(st.st_mode))
	{
		/*
		 * This will fail if the directory isn't empty, but not if it's a
		 * junction point.
		 */
		if (rmdir(linkloc) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m",
							linkloc)));
	}
#ifdef S_ISLNK
	else if (S_ISLNK(st.st_mode))
	{
		if (unlink(linkloc) < 0 && errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not remove symbolic link \"%s\": %m",
							linkloc)));
	}
#endif
	else
	{
		/* Refuse to remove anything that's not a directory or symlink */
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("\"%s\" is not a directory or symbolic link",
						linkloc)));
	}
}

/*
 * Rename a tablespace
 */
ObjectAddress
RenameTableSpace(const char *oldname, const char *newname)
{
	Oid			tspId;
	Relation	rel;
	ScanKeyData entry[1];
	TableScanDesc scan;
	HeapTuple	tup;
	HeapTuple	newtuple;
	Form_pg_tablespace newform;
	ObjectAddress address;

	/* Search pg_tablespace */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(oldname));
	scan = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						oldname)));

	newtuple = heap_copytuple(tup);
	newform = (Form_pg_tablespace) GETSTRUCT(newtuple);
	tspId = newform->oid;

	table_endscan(scan);

	/* Must be owner */
	if (!pg_tablespace_ownercheck(tspId, GetUserId()))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLESPACE, oldname);

	/* Validate new name */
	if (!allowSystemTableMods && IsReservedName(newname))
		ereport(ERROR,
				(errcode(ERRCODE_RESERVED_NAME),
				 errmsg("unacceptable tablespace name \"%s\"", newname),
				 errdetail("The prefix \"pg_\" is reserved for system tablespaces.")));

	/*
	 * If built with appropriate switch, whine when regression-testing
	 * conventions for tablespace names are violated.
	 */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
	if (strncmp(newname, "regress_", 8) != 0)
		elog(WARNING, "tablespaces created by regression test cases should have names starting with \"regress_\"");
#endif

	/* Make sure the new name doesn't exist */
	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(newname));
	scan = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scan, ForwardScanDirection);
	if (HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tablespace \"%s\" already exists",
						newname)));

	table_endscan(scan);

	/* OK, update the entry */
	namestrcpy(&(newform->spcname), newname);

	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(TableSpaceRelationId, tspId, 0);

	ObjectAddressSet(address, TableSpaceRelationId, tspId);

	table_close(rel, NoLock);

	return address;
}

/*
 * Alter table space options
 */
Oid
AlterTableSpaceOptions(AlterTableSpaceOptionsStmt *stmt)
{
	Relation	rel;
	ScanKeyData entry[1];
	TableScanDesc scandesc;
	HeapTuple	tup;
	Oid			tablespaceoid;
	Datum		datum;
	Datum		newOptions;
	Datum		repl_val[Natts_pg_tablespace];
	bool		isnull;
	bool		repl_null[Natts_pg_tablespace];
	bool		repl_repl[Natts_pg_tablespace];
	HeapTuple	newtuple;

	/* Search pg_tablespace */
	rel = table_open(TableSpaceRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tup = heap_getnext(scandesc, ForwardScanDirection);
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						stmt->tablespacename)));

	tablespaceoid = ((Form_pg_tablespace) GETSTRUCT(tup))->oid;

	/* Must be owner of the existing object */
	if (!pg_tablespace_ownercheck(tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLESPACE,
					   stmt->tablespacename);

	/* Generate new proposed spcoptions (text array) */
	datum = heap_getattr(tup, Anum_pg_tablespace_spcoptions,
						 RelationGetDescr(rel), &isnull);
	newOptions = transformRelOptions(isnull ? (Datum) 0 : datum,
									 stmt->options, NULL, NULL, false,
									 stmt->isReset);
	(void) tablespace_reloptions(newOptions, true);

	/* Build new tuple. */
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newOptions != (Datum) 0)
		repl_val[Anum_pg_tablespace_spcoptions - 1] = newOptions;
	else
		repl_null[Anum_pg_tablespace_spcoptions - 1] = true;
	repl_repl[Anum_pg_tablespace_spcoptions - 1] = true;
	newtuple = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val,
								 repl_null, repl_repl);

	/* Update system catalog. */
	CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);

	InvokeObjectPostAlterHook(TableSpaceRelationId, tablespaceoid, 0);

	heap_freetuple(newtuple);

	/* Conclude heap scan. */
	table_endscan(scandesc);
	table_close(rel, NoLock);

	return tablespaceoid;
}

/*
 * Routines for handling the GUC variable 'default_tablespace'.
 */

/* check_hook: validate new default_tablespace */
bool
check_default_tablespace(char **newval, void **extra, GucSource source)
{
	/*
	 * If we aren't inside a transaction, or connected to a database, we
	 * cannot do the catalog accesses necessary to verify the name.  Must
	 * accept the value on faith.
	 */
	if (IsTransactionState() && MyDatabaseId != InvalidOid)
	{
		if (**newval != '\0' &&
			!OidIsValid(get_tablespace_oid(*newval, true)))
		{
			/*
			 * When source == PGC_S_TEST, don't throw a hard error for a
			 * nonexistent tablespace, only a NOTICE.  See comments in guc.h.
			 */
			if (source == PGC_S_TEST)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("tablespace \"%s\" does not exist",
								*newval)));
			}
			else
			{
				GUC_check_errdetail("Tablespace \"%s\" does not exist.",
									*newval);
				return false;
			}
		}
	}

	return true;
}

/*
 * GetDefaultTablespace -- get the OID of the current default tablespace
 *
 * Temporary objects have different default tablespaces, hence the
 * relpersistence parameter must be specified.  Also, for partitioned tables,
 * we disallow specifying the database default, so that needs to be specified
 * too.
 *
 * May return InvalidOid to indicate "use the database's default tablespace".
 *
 * Note that caller is expected to check appropriate permissions for any
 * result other than InvalidOid.
 *
 * This exists to hide (and possibly optimize the use of) the
 * default_tablespace GUC variable.
 */
Oid
GetDefaultTablespace(char relpersistence, bool partitioned)
{
	Oid			result;

	/* The temp-table case is handled elsewhere */
	if (relpersistence == RELPERSISTENCE_TEMP)
	{
		PrepareTempTablespaces();
		return GetNextTempTableSpace();
	}

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
	result = get_tablespace_oid(default_tablespace, true);

	/*
	 * Allow explicit specification of database's default tablespace in
	 * default_tablespace without triggering permissions checks.  Don't allow
	 * specifying that when creating a partitioned table, however, since the
	 * result is confusing.
	 */
	if (result == MyDatabaseTableSpace)
	{
		if (partitioned)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot specify default tablespace for partitioned relations")));
		result = InvalidOid;
	}
	return result;
}


/*
 * Routines for handling the GUC variable 'temp_tablespaces'.
 */

typedef struct
{
	/* Array of OIDs to be passed to SetTempTablespaces() */
	int			numSpcs;
	Oid			tblSpcs[FLEXIBLE_ARRAY_MEMBER];
} temp_tablespaces_extra;

/* check_hook: validate new temp_tablespaces */
bool
check_temp_tablespaces(char **newval, void **extra, GucSource source)
{
	char	   *rawname;
	List	   *namelist;

	/* Need a modifiable copy of string */
	rawname = pstrdup(*newval);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawname);
		list_free(namelist);
		return false;
	}

	/*
	 * If we aren't inside a transaction, or connected to a database, we
	 * cannot do the catalog accesses necessary to verify the name.  Must
	 * accept the value on faith. Fortunately, there's then also no need to
	 * pass the data to fd.c.
	 */
	if (IsTransactionState() && MyDatabaseId != InvalidOid)
	{
		temp_tablespaces_extra *myextra;
		Oid		   *tblSpcs;
		int			numSpcs;
		ListCell   *l;

		/* temporary workspace until we are done verifying the list */
		tblSpcs = (Oid *) palloc(list_length(namelist) * sizeof(Oid));
		numSpcs = 0;
		foreach(l, namelist)
		{
			char	   *curname = (char *) lfirst(l);
			Oid			curoid;
			AclResult	aclresult;

			/* Allow an empty string (signifying database default) */
			if (curname[0] == '\0')
			{
				/* InvalidOid signifies database's default tablespace */
				tblSpcs[numSpcs++] = InvalidOid;
				continue;
			}

			/*
			 * In an interactive SET command, we ereport for bad info.  When
			 * source == PGC_S_TEST, don't throw a hard error for a
			 * nonexistent tablespace, only a NOTICE.  See comments in guc.h.
			 */
			curoid = get_tablespace_oid(curname, source <= PGC_S_TEST);
			if (curoid == InvalidOid)
			{
				if (source == PGC_S_TEST)
					ereport(NOTICE,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("tablespace \"%s\" does not exist",
									curname)));
				continue;
			}

			/*
			 * Allow explicit specification of database's default tablespace
			 * in temp_tablespaces without triggering permissions checks.
			 */
			if (curoid == MyDatabaseTableSpace)
			{
				/* InvalidOid signifies database's default tablespace */
				tblSpcs[numSpcs++] = InvalidOid;
				continue;
			}

			/* Check permissions, similarly complaining only if interactive */
			aclresult = pg_tablespace_aclcheck(curoid, GetUserId(),
											   ACL_CREATE);
			if (aclresult != ACLCHECK_OK)
			{
				if (source >= PGC_S_INTERACTIVE)
					aclcheck_error(aclresult, OBJECT_TABLESPACE, curname);
				continue;
			}

			tblSpcs[numSpcs++] = curoid;
		}

		/* Now prepare an "extra" struct for assign_temp_tablespaces */
		myextra = malloc(offsetof(temp_tablespaces_extra, tblSpcs) +
						 numSpcs * sizeof(Oid));
		if (!myextra)
			return false;
		myextra->numSpcs = numSpcs;
		memcpy(myextra->tblSpcs, tblSpcs, numSpcs * sizeof(Oid));
		*extra = (void *) myextra;

		pfree(tblSpcs);
	}

	pfree(rawname);
	list_free(namelist);

	return true;
}

/* assign_hook: do extra actions as needed */
void
assign_temp_tablespaces(const char *newval, void *extra)
{
	temp_tablespaces_extra *myextra = (temp_tablespaces_extra *) extra;

	/*
	 * If check_temp_tablespaces was executed inside a transaction, then pass
	 * the list it made to fd.c.  Otherwise, clear fd.c's list; we must be
	 * still outside a transaction, or else restoring during transaction exit,
	 * and in either case we can just let the next PrepareTempTablespaces call
	 * make things sane.
	 */
	if (myextra)
		SetTempTablespaces(myextra->tblSpcs, myextra->numSpcs);
	else
		SetTempTablespaces(NULL, 0);
}

/*
 * PrepareTempTablespaces -- prepare to use temp tablespaces
 *
 * If we have not already done so in the current transaction, parse the
 * temp_tablespaces GUC variable and tell fd.c which tablespace(s) to use
 * for temp files.
 */
void
PrepareTempTablespaces(void)
{
	char	   *rawname;
	List	   *namelist;
	Oid		   *tblSpcs;
	int			numSpcs;
	ListCell   *l;

	/* No work if already done in current transaction */
	if (TempTablespacesAreSet())
		return;

	/*
	 * Can't do catalog access unless within a transaction.  This is just a
	 * safety check in case this function is called by low-level code that
	 * could conceivably execute outside a transaction.  Note that in such a
	 * scenario, fd.c will fall back to using the current database's default
	 * tablespace, which should always be OK.
	 */
	if (!IsTransactionState())
		return;

	/* Need a modifiable copy of string */
	rawname = pstrdup(temp_tablespaces);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		SetTempTablespaces(NULL, 0);
		pfree(rawname);
		list_free(namelist);
		return;
	}

	/* Store tablespace OIDs in an array in TopTransactionContext */
	tblSpcs = (Oid *) MemoryContextAlloc(TopTransactionContext,
										 list_length(namelist) * sizeof(Oid));
	numSpcs = 0;
	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);
		Oid			curoid;
		AclResult	aclresult;

		/* Allow an empty string (signifying database default) */
		if (curname[0] == '\0')
		{
			/* InvalidOid signifies database's default tablespace */
			tblSpcs[numSpcs++] = InvalidOid;
			continue;
		}

		/* Else verify that name is a valid tablespace name */
		curoid = get_tablespace_oid(curname, true);
		if (curoid == InvalidOid)
		{
			/* Skip any bad list elements */
			continue;
		}

		/*
		 * Allow explicit specification of database's default tablespace in
		 * temp_tablespaces without triggering permissions checks.
		 */
		if (curoid == MyDatabaseTableSpace)
		{
			/* InvalidOid signifies database's default tablespace */
			tblSpcs[numSpcs++] = InvalidOid;
			continue;
		}

		/* Check permissions similarly */
		aclresult = pg_tablespace_aclcheck(curoid, GetUserId(),
										   ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			continue;

		tblSpcs[numSpcs++] = curoid;
	}

	SetTempTablespaces(tblSpcs, numSpcs);

	pfree(rawname);
	list_free(namelist);
}


/*
 * get_tablespace_oid - given a tablespace name, look up the OID
 *
 * If missing_ok is false, throw an error if tablespace name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_tablespace_oid(const char *tablespacename, bool missing_ok)
{
	Oid			result;
	Relation	rel;
	TableScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_tablespace.  We use a heapscan here even though there is an
	 * index on name, on the theory that pg_tablespace will usually have just
	 * a few entries and so an indexed lookup is a waste of effort.
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_tablespace) GETSTRUCT(tuple))->oid;
	else
		result = InvalidOid;

	table_endscan(scandesc);
	table_close(rel, AccessShareLock);

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						tablespacename)));

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
	TableScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/*
	 * Search pg_tablespace.  We use a heapscan here even though there is an
	 * index on oid, on the theory that pg_tablespace will usually have just a
	 * few entries and so an indexed lookup is a waste of effort.
	 */
	rel = table_open(TableSpaceRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(spc_oid));
	scandesc = table_beginscan_catalog(rel, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(NameStr(((Form_pg_tablespace) GETSTRUCT(tuple))->spcname));
	else
		result = NULL;

	table_endscan(scandesc);
	table_close(rel, AccessShareLock);

	return result;
}


/*
 * TABLESPACE resource manager's routines
 */
void
tblspc_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in tblspc records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_TBLSPC_CREATE)
	{
		xl_tblspc_create_rec *xlrec = (xl_tblspc_create_rec *) XLogRecGetData(record);
		char	   *location = xlrec->ts_path;

		create_tablespace_directories(location, xlrec->ts_id);
	}
	else if (info == XLOG_TBLSPC_DROP)
	{
		xl_tblspc_drop_rec *xlrec = (xl_tblspc_drop_rec *) XLogRecGetData(record);

		/*
		 * If we issued a WAL record for a drop tablespace it implies that
		 * there were no files in it at all when the DROP was done. That means
		 * that no permanent objects can exist in it at this point.
		 *
		 * It is possible for standby users to be using this tablespace as a
		 * location for their temporary files, so if we fail to remove all
		 * files then do conflict processing and try again, if currently
		 * enabled.
		 *
		 * Other possible reasons for failure include bollixed file
		 * permissions on a standby server when they were okay on the primary,
		 * etc etc. There's not much we can do about that, so just remove what
		 * we can and press on.
		 */
		if (!destroy_tablespace_directories(xlrec->ts_id, true))
		{
			ResolveRecoveryConflictWithTablespace(xlrec->ts_id);

			/*
			 * If we did recovery processing then hopefully the backends who
			 * wrote temp files should have cleaned up and exited by now.  So
			 * retry before complaining.  If we fail again, this is just a LOG
			 * condition, because it's not worth throwing an ERROR for (as
			 * that would crash the database and require manual intervention
			 * before we could get past this WAL record on restart).
			 */
			if (!destroy_tablespace_directories(xlrec->ts_id, true))
				ereport(LOG,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("directories for tablespace %u could not be removed",
								xlrec->ts_id),
						 errhint("You can remove the directories manually if necessary.")));
		}
	}
	else
		elog(PANIC, "tblspc_redo: unknown op code %u", info);
}
