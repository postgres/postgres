/*-------------------------------------------------------------------------
 *
 *	checkfiles.c
 *	  support to clean up stale relation files on crash recovery
 *
 *	If a backend crashes while in a transaction that has created or
 *	deleted a relfilenode, a stale file can be left over in the data
 *	directory. This file contains routines to clean up those stale
 *	files on recovery.
 *
 *	This adds a 17% increase in startup cost for 100 empty databases.  bjm
 *	One optimization would be to create a 'dirty' file on a postmaster recovery
 *	and remove the dirty flag only when a clean startup detects no unreferenced
 *	files, and use the 'dirty' flag to determine if we should run this on
 *	a clean startup.
 *
 * $PostgreSQL: pgsql/src/backend/utils/init/checkfiles.c,v 1.1 2005/05/02 18:26:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/fd.h"

#include "utils/flatfiles.h"
#include "miscadmin.h"
#include "catalog/pg_tablespace.h"
#include "catalog/catalog.h"
#include "access/skey.h"
#include "utils/fmgroids.h"
#include "access/relscan.h"
#include "access/heapam.h"
#include "utils/resowner.h"

static void CheckStaleRelFilesFrom(Oid tablespaceoid, Oid dboid);
static void CheckStaleRelFilesFromTablespace(Oid tablespaceoid);

/* Like AllocateDir, but ereports on failure */
static DIR *
AllocateDirChecked(char *path)
{
	DIR		   *dirdesc = AllocateDir(path);

	if (dirdesc == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						path)));
	return dirdesc;
}

/*
 * Scan through all tablespaces for relations left over
 * by aborted transactions.
 *
 * For example, if a transaction issues
 * BEGIN; CREATE TABLE foobar ();
 * and then the backend crashes, the file is left in the
 * tablespace until CheckStaleRelFiles deletes it.
 */
void
CheckStaleRelFiles(void)
{
	DIR		   *dirdesc;
	struct dirent *de;
	char	   *path;
	int			pathlen;

	pathlen = strlen(DataDir) + 11 + 1;
	path = (char *) palloc(pathlen);
	snprintf(path, pathlen, "%s/pg_tblspc/", DataDir);
	dirdesc = AllocateDirChecked(path);
	while ((de = readdir(dirdesc)) != NULL)
	{
		char	   *invalid;
		Oid			tablespaceoid;

		/* Check that the directory name looks like valid tablespace link.	*/
		tablespaceoid = (Oid) strtol(de->d_name, &invalid, 10);
		if (invalid[0] == '\0')
			CheckStaleRelFilesFromTablespace(tablespaceoid);
	}
	FreeDir(dirdesc);
	pfree(path);

	CheckStaleRelFilesFromTablespace(DEFAULTTABLESPACE_OID);
}

/* Scan a specific tablespace for stale relations */
static void
CheckStaleRelFilesFromTablespace(Oid tablespaceoid)
{
	DIR		   *dirdesc;
	struct dirent *de;
	char	   *path;

	path = GetTablespacePath(tablespaceoid);

	dirdesc = AllocateDirChecked(path);
	while ((de = readdir(dirdesc)) != NULL)
	{
		char	   *invalid;
		Oid			dboid;

		dboid = (Oid) strtol(de->d_name, &invalid, 10);
		if (invalid[0] == '\0')
			CheckStaleRelFilesFrom(tablespaceoid, dboid);
	}
	FreeDir(dirdesc);
	pfree(path);
}

/* Scan a specific database in a specific tablespace for stale relations.
 *
 * First, pg_class for the database is opened, and the relfilenodes of all
 * relations mentioned there are stored in a hash table.
 *
 * Then the directory is scanned. Every file in the directory that's not
 * found in pg_class (the hash table) is logged.
 */
static void
CheckStaleRelFilesFrom(Oid tablespaceoid, Oid dboid)
{
	DIR		   *dirdesc;
	struct dirent *de;
	HASHCTL		hashctl;
	HTAB	   *relfilenodeHash;
	MemoryContext mcxt;
	RelFileNode rnode;
	char	   *path;

	/*
	 * We create a private memory context so that we can easily deallocate the
	 * hash table and its contents
	 */
	mcxt = AllocSetContextCreate(TopMemoryContext, "CheckStaleRelFiles",
								 ALLOCSET_DEFAULT_MINSIZE,
								 ALLOCSET_DEFAULT_INITSIZE,
								 ALLOCSET_DEFAULT_MAXSIZE);

	hashctl.hash = tag_hash;

	/*
	 * The entry contents is not used for anything, we just check if an oid is
	 * in the hash table or not.
	 */
	hashctl.keysize = sizeof(Oid);
	hashctl.entrysize = 1;
	hashctl.hcxt = mcxt;
	relfilenodeHash = hash_create("relfilenodeHash", 100, &hashctl,
								  HASH_FUNCTION
								  | HASH_ELEM | HASH_CONTEXT);

	/* Read all relfilenodes from pg_class into the hash table */
	{
		ResourceOwner owner,
					oldowner;
		Relation	rel;
		HeapScanDesc scan;
		HeapTuple	tuple;

		/* Need a resowner to keep the heapam and buffer code happy */
		owner = ResourceOwnerCreate(NULL, "CheckStaleRelFiles");
		oldowner = CurrentResourceOwner;
		CurrentResourceOwner = owner;

		rnode.spcNode = tablespaceoid;
		rnode.dbNode = dboid;
		rnode.relNode = RelationRelationId;
		rel = XLogOpenRelation(true, 0, rnode);

		scan = heap_beginscan(rel, SnapshotNow, 0, NULL);
		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class classform = (Form_pg_class) GETSTRUCT(tuple);

			hash_search(relfilenodeHash, &classform->relfilenode,
						HASH_ENTER, NULL);
		}
		heap_endscan(scan);

		XLogCloseRelation(rnode);
		CurrentResourceOwner = oldowner;
		ResourceOwnerDelete(owner);
	}

	/* Scan the directory */
	path = GetDatabasePath(dboid, tablespaceoid);

	dirdesc = AllocateDirChecked(path);
	while ((de = readdir(dirdesc)) != NULL)
	{
		char	   *invalid;
		Oid			relfilenode;

		relfilenode = strtol(de->d_name, &invalid, 10);
		if (invalid[0] == '\0')
		{
			/*
			 * Filename was a valid number, check if pg_class knows about it
			 */
			if (hash_search(relfilenodeHash, &relfilenode,
							HASH_FIND, NULL) == NULL)
			{
				char	   *filepath;

				rnode.spcNode = tablespaceoid;
				rnode.dbNode = dboid;
				rnode.relNode = relfilenode;

				filepath = relpath(rnode);

				ereport(LOG,
						(errcode_for_file_access(),
						 errmsg("The table or index file \"%s\" is stale and can be safely removed",
								filepath)));
				pfree(filepath);
			}
		}
	}
	FreeDir(dirdesc);
	pfree(path);
	hash_destroy(relfilenodeHash);
	MemoryContextDelete(mcxt);
}
