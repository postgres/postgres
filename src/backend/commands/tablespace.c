/*-------------------------------------------------------------------------
 *
 * tablespace.c
 *	  Commands to manipulate table spaces
 *
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
 * The implementation is designed to be backwards compatible. For this reason
 * (and also as a feature unto itself) when a user creates an object without
 * specifying a tablespace, we look at the object's parent and place
 * the object in the parent's tablespace. The hierarchy is as follows:
 *			database > schema > table > index
 *
 * To allow CREATE DATABASE to give a new database a default tablespace
 * that's different from the template database's default, we make the
 * provision that a zero in pg_class.reltablespace means the database's
 * default tablespace.	Without this, CREATE DATABASE would have to go in
 * and munge the system catalogs of the new database.  This special meaning
 * of zero also applies in pg_namespace.nsptablespace.
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/tablespace.c,v 1.9 2004/08/29 05:06:41 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void set_short_version(const char *path);
static bool directory_is_empty(const char *path);


/*
 * Each database using a table space is isolated into its own name space
 * by a subdirectory named for the database OID.  On first creation of an
 * object in the tablespace, create the subdirectory.  If the subdirectory
 * already exists, just fall through quietly.
 *
 * If tablespaces are not supported, this is just a no-op; CREATE DATABASE
 * is expected to create the default subdirectory for the database.
 *
 * isRedo indicates that we are creating an object during WAL replay;
 * we can skip doing locking in that case (and should do so to avoid
 * any possible problems with pg_tablespace not being valid).
 */
void
TablespaceCreateDbspace(Oid spcNode, Oid dbNode, bool isRedo)
{
#ifdef HAVE_SYMLINK
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
			 * Acquire ExclusiveLock on pg_tablespace to ensure that no
			 * DROP TABLESPACE or TablespaceCreateDbspace is running
			 * concurrently.  Simple reads from pg_tablespace are OK.
			 */
			Relation	rel;

			if (!isRedo)
				rel = heap_openr(TableSpaceRelationName, ExclusiveLock);
			else
				rel = NULL;

			/*
			 * Recheck to see if someone created the directory while we
			 * were waiting for lock.
			 */
			if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
			{
				/* need not do anything */
			}
			else
			{
				/* OK, go for it */
				if (mkdir(dir, S_IRWXU) < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
						  errmsg("could not create directory \"%s\": %m",
								 dir)));
			}

			/* OK to drop the exclusive lock */
			if (!isRedo)
				heap_close(rel, ExclusiveLock);
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
#endif   /* HAVE_SYMLINK */
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
	AclId		ownerid;

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
	{
		/* No need to check result, get_usesysid() does that */
		ownerid = get_usesysid(stmt->owner);
	}
	else
		ownerid = GetUserId();

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
	 * Check that location isn't too long. Remember that we're going to
	 * append '/<dboid>/<relid>.<nnn>'	(XXX but do we ever form the whole
	 * path explicitly?  This may be overly conservative.)
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
	 * Insert tuple into pg_tablespace.  The purpose of doing this first
	 * is to lock the proposed tablename against other would-be creators.
	 * The insertion will roll back if we find problems below.
	 */
	rel = heap_openr(TableSpaceRelationName, RowExclusiveLock);

	MemSet(nulls, ' ', Natts_pg_tablespace);

	values[Anum_pg_tablespace_spcname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->tablespacename));
	values[Anum_pg_tablespace_spcowner - 1] =
		Int32GetDatum(ownerid);
	values[Anum_pg_tablespace_spclocation - 1] =
		DirectFunctionCall1(textin, CStringGetDatum(location));
	nulls[Anum_pg_tablespace_spcacl - 1] = 'n';

	tuple = heap_formtuple(rel->rd_att, values, nulls);

	tablespaceoid = newoid();

	HeapTupleSetOid(tuple, tablespaceoid);

	simple_heap_insert(rel, tuple);

	CatalogUpdateIndexes(rel, tuple);

	heap_freetuple(tuple);

	/*
	 * Attempt to coerce target directory to safe permissions.	If this
	 * fails, it doesn't exist or has the wrong owner.
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
	 * Create the PG_VERSION file in the target directory.	This has
	 * several purposes: to make sure we can write in the directory, to
	 * prevent someone from creating another tablespace pointing at the
	 * same directory (the emptiness check above will fail), and to label
	 * tablespace directories by PG version.
	 */
	set_short_version(location);

	/*
	 * All seems well, create the symlink
	 */
	linkloc = (char *) palloc(strlen(DataDir) + 11 + 10 + 1);
	sprintf(linkloc, "%s/pg_tblspc/%u", DataDir, tablespaceoid);

	if (symlink(location, linkloc) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create symbolic link \"%s\": %m",
						linkloc)));

	pfree(linkloc);
	pfree(location);

	heap_close(rel, RowExclusiveLock);

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
	char	   *location;
	Oid			tablespaceoid;
	DIR		   *dirdesc;
	struct dirent *de;
	char	   *subfile;

	/* don't call this in a transaction block */
	PreventTransactionChain((void *) stmt, "DROP TABLESPACE");

	/*
	 * Acquire ExclusiveLock on pg_tablespace to ensure that no one else
	 * is trying to do DROP TABLESPACE or TablespaceCreateDbspace
	 * concurrently.
	 */
	rel = heap_openr(TableSpaceRelationName, ExclusiveLock);

	/*
	 * Find the target tuple
	 */
	ScanKeyInit(&entry[0],
				Anum_pg_tablespace_spcname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tablespacename));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tablespace \"%s\" does not exist",
						tablespacename)));

	tablespaceoid = HeapTupleGetOid(tuple);

	/* Must be superuser or owner */
	if (GetUserId() != ((Form_pg_tablespace) GETSTRUCT(tuple))->spcowner &&
		!superuser())
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TABLESPACE,
					   tablespacename);

	/* Disallow drop of the standard tablespaces, even by superuser */
	if (tablespaceoid == GLOBALTABLESPACE_OID ||
		tablespaceoid == DEFAULTTABLESPACE_OID)
		aclcheck_error(ACLCHECK_NO_PRIV, ACL_KIND_TABLESPACE,
					   tablespacename);

	location = (char *) palloc(strlen(DataDir) + 16 + 10 + 1);
	sprintf(location, "%s/pg_tblspc/%u", DataDir, tablespaceoid);

	/*
	 * Check if the tablespace still contains any files.  We try to rmdir
	 * each per-database directory we find in it.  rmdir failure implies
	 * there are still files in that subdirectory, so give up.	(We do not
	 * have to worry about undoing any already completed rmdirs, since the
	 * next attempt to use the tablespace from that database will simply
	 * recreate the subdirectory via TablespaceCreateDbspace.)
	 *
	 * Since we hold exclusive lock, no one else should be creating any fresh
	 * subdirectories in parallel.	It is possible that new files are
	 * being created within subdirectories, though, so the rmdir call
	 * could fail.	Worst consequence is a less friendly error message.
	 */
	dirdesc = AllocateDir(location);
	if (dirdesc == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						location)));

	errno = 0;
	while ((de = readdir(dirdesc)) != NULL)
	{
		/* Note we ignore PG_VERSION for the nonce */
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0 ||
			strcmp(de->d_name, "PG_VERSION") == 0)
		{
			errno = 0;
			continue;
		}

		subfile = palloc(strlen(location) + 1 + strlen(de->d_name) + 1);
		sprintf(subfile, "%s/%s", location, de->d_name);

		/* This check is just to deliver a friendlier error message */
		if (!directory_is_empty(subfile))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tablespace \"%s\" is not empty",
							tablespacename)));

		/* Do the real deed */
		if (rmdir(subfile) < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not delete directory \"%s\": %m",
							subfile)));

		pfree(subfile);
	}
#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but
	 * not in released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif
	if (errno)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m",
						location)));
	FreeDir(dirdesc);

	/*
	 * Okay, try to unlink PG_VERSION and then remove the symlink.
	 */
	subfile = palloc(strlen(location) + 11 + 1);
	sprintf(subfile, "%s/PG_VERSION", location);

	if (unlink(subfile) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not unlink file \"%s\": %m",
						subfile)));

#ifndef WIN32
	if (unlink(location) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not unlink symbolic link \"%s\": %m",
						location)));
#else
	/* The junction is a directory, not a file */
	if (rmdir(location) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not remove junction dir \"%s\": %m",
						location)));
#endif

	pfree(subfile);
	pfree(location);

	/*
	 * We have successfully destroyed the infrastructure ... there is now
	 * no way to roll back the DROP ... so proceed to remove the
	 * pg_tablespace tuple.
	 */
	simple_heap_delete(rel, &tuple->t_self);

	heap_endscan(scandesc);

	heap_close(rel, ExclusiveLock);

#else							/* !HAVE_SYMLINK */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("tablespaces are not supported on this platform")));
#endif   /* HAVE_SYMLINK */
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
 */
static bool
directory_is_empty(const char *path)
{
	DIR		   *dirdesc;
	struct dirent *de;

	dirdesc = AllocateDir(path);
	if (dirdesc == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						path)));

	errno = 0;
	while ((de = readdir(dirdesc)) != NULL)
	{
		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
		{
			errno = 0;
			continue;
		}
		FreeDir(dirdesc);
		return false;
	}
#ifdef WIN32

	/*
	 * This fix is in mingw cvs (runtime/mingwex/dirent.c rev 1.4), but
	 * not in released version
	 */
	if (GetLastError() == ERROR_NO_MORE_FILES)
		errno = 0;
#endif
	if (errno)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m",
						path)));
	FreeDir(dirdesc);
	return true;
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
	rel = heap_openr(TableSpaceRelationName, AccessShareLock);

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
	rel = heap_openr(TableSpaceRelationName, AccessShareLock);

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
	rel = heap_openr(TableSpaceRelationName, RowExclusiveLock);

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

	/* Must be owner or superuser */
	if (newform->spcowner != GetUserId() && !superuser())
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
AlterTableSpaceOwner(const char *name, AclId newOwnerSysId)
{
	Relation	rel;
	ScanKeyData entry[1];
	HeapScanDesc scandesc;
	Form_pg_tablespace spcForm;
	HeapTuple	tup;

	/* Search pg_tablespace */
	rel = heap_openr(TableSpaceRelationName, RowExclusiveLock);

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
	if (spcForm->spcowner != newOwnerSysId)
	{
		Datum		repl_val[Natts_pg_tablespace];
		char		repl_null[Natts_pg_tablespace];
		char		repl_repl[Natts_pg_tablespace];
		Acl		   *newAcl;
		Datum		aclDatum;
		bool		isNull;
		HeapTuple	newtuple;

		/* Otherwise, must be superuser to change object ownership */
		if (!superuser())
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to change owner")));

		memset(repl_null, ' ', sizeof(repl_null));
		memset(repl_repl, ' ', sizeof(repl_repl));

		repl_repl[Anum_pg_tablespace_spcowner - 1] = 'r';
		repl_val[Anum_pg_tablespace_spcowner - 1] = Int32GetDatum(newOwnerSysId);

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
								 spcForm->spcowner, newOwnerSysId);
			repl_repl[Anum_pg_tablespace_spcacl - 1] = 'r';
			repl_val[Anum_pg_tablespace_spcacl - 1] = PointerGetDatum(newAcl);
		}

		newtuple = heap_modifytuple(tup, rel, repl_val, repl_null, repl_repl);

		simple_heap_update(rel, &newtuple->t_self, newtuple);
		CatalogUpdateIndexes(rel, newtuple);

		heap_freetuple(newtuple);
	}

	heap_endscan(scandesc);
	heap_close(rel, NoLock);
}
