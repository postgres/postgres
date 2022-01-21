/*-------------------------------------------------------------------------
 *
 * reinit.c
 *	  Reinitialization of unlogged relations
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/reinit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "common/relpath.h"
#include "postmaster/startup.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/reinit.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

static void ResetUnloggedRelationsInTablespaceDir(const char *tsdirname,
												  int op);
static void ResetUnloggedRelationsInDbspaceDir(const char *dbspacedirname,
											   int op);

typedef struct
{
	Oid			reloid;			/* hash key */
} unlogged_relation_entry;

/*
 * Reset unlogged relations from before the last restart.
 *
 * If op includes UNLOGGED_RELATION_CLEANUP, we remove all forks of any
 * relation with an "init" fork, except for the "init" fork itself.
 *
 * If op includes UNLOGGED_RELATION_INIT, we copy the "init" fork to the main
 * fork.
 */
void
ResetUnloggedRelations(int op)
{
	char		temp_path[MAXPGPATH + 10 + sizeof(TABLESPACE_VERSION_DIRECTORY)];
	DIR		   *spc_dir;
	struct dirent *spc_de;
	MemoryContext tmpctx,
				oldctx;

	/* Log it. */
	elog(DEBUG1, "resetting unlogged relations: cleanup %d init %d",
		 (op & UNLOGGED_RELATION_CLEANUP) != 0,
		 (op & UNLOGGED_RELATION_INIT) != 0);

	/*
	 * Just to be sure we don't leak any memory, let's create a temporary
	 * memory context for this operation.
	 */
	tmpctx = AllocSetContextCreate(CurrentMemoryContext,
								   "ResetUnloggedRelations",
								   ALLOCSET_DEFAULT_SIZES);
	oldctx = MemoryContextSwitchTo(tmpctx);

	/* Prepare to report progress resetting unlogged relations. */
	begin_startup_progress_phase();

	/*
	 * First process unlogged files in pg_default ($PGDATA/base)
	 */
	ResetUnloggedRelationsInTablespaceDir("base", op);

	/*
	 * Cycle through directories for all non-default tablespaces.
	 */
	spc_dir = AllocateDir("pg_tblspc");

	while ((spc_de = ReadDir(spc_dir, "pg_tblspc")) != NULL)
	{
		if (strcmp(spc_de->d_name, ".") == 0 ||
			strcmp(spc_de->d_name, "..") == 0)
			continue;

		snprintf(temp_path, sizeof(temp_path), "pg_tblspc/%s/%s",
				 spc_de->d_name, TABLESPACE_VERSION_DIRECTORY);
		ResetUnloggedRelationsInTablespaceDir(temp_path, op);
	}

	FreeDir(spc_dir);

	/*
	 * Restore memory context.
	 */
	MemoryContextSwitchTo(oldctx);
	MemoryContextDelete(tmpctx);
}

/*
 * Process one tablespace directory for ResetUnloggedRelations
 */
static void
ResetUnloggedRelationsInTablespaceDir(const char *tsdirname, int op)
{
	DIR		   *ts_dir;
	struct dirent *de;
	char		dbspace_path[MAXPGPATH * 2];

	ts_dir = AllocateDir(tsdirname);

	/*
	 * If we get ENOENT on a tablespace directory, log it and return.  This
	 * can happen if a previous DROP TABLESPACE crashed between removing the
	 * tablespace directory and removing the symlink in pg_tblspc.  We don't
	 * really want to prevent database startup in that scenario, so let it
	 * pass instead.  Any other type of error will be reported by ReadDir
	 * (causing a startup failure).
	 */
	if (ts_dir == NULL && errno == ENOENT)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						tsdirname)));
		return;
	}

	while ((de = ReadDir(ts_dir, tsdirname)) != NULL)
	{
		/*
		 * We're only interested in the per-database directories, which have
		 * numeric names.  Note that this code will also (properly) ignore "."
		 * and "..".
		 */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(dbspace_path, sizeof(dbspace_path), "%s/%s",
				 tsdirname, de->d_name);

		if (op & UNLOGGED_RELATION_INIT)
			ereport_startup_progress("resetting unlogged relations (init), elapsed time: %ld.%02d s, current path: %s",
									 dbspace_path);
		else if (op & UNLOGGED_RELATION_CLEANUP)
			ereport_startup_progress("resetting unlogged relations (cleanup), elapsed time: %ld.%02d s, current path: %s",
									 dbspace_path);

		ResetUnloggedRelationsInDbspaceDir(dbspace_path, op);
	}

	FreeDir(ts_dir);
}

/*
 * Process one per-dbspace directory for ResetUnloggedRelations
 */
static void
ResetUnloggedRelationsInDbspaceDir(const char *dbspacedirname, int op)
{
	DIR		   *dbspace_dir;
	struct dirent *de;
	char		rm_path[MAXPGPATH * 2];

	/* Caller must specify at least one operation. */
	Assert((op & (UNLOGGED_RELATION_CLEANUP | UNLOGGED_RELATION_INIT)) != 0);

	/*
	 * Cleanup is a two-pass operation.  First, we go through and identify all
	 * the files with init forks.  Then, we go through again and nuke
	 * everything with the same OID except the init fork.
	 */
	if ((op & UNLOGGED_RELATION_CLEANUP) != 0)
	{
		HTAB	   *hash;
		HASHCTL		ctl;

		/*
		 * It's possible that someone could create a ton of unlogged relations
		 * in the same database & tablespace, so we'd better use a hash table
		 * rather than an array or linked list to keep track of which files
		 * need to be reset.  Otherwise, this cleanup operation would be
		 * O(n^2).
		 */
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(unlogged_relation_entry);
		ctl.hcxt = CurrentMemoryContext;
		hash = hash_create("unlogged relation OIDs", 32, &ctl,
						   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/* Scan the directory. */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			oidchars;
			unlogged_relation_entry ent;

			/* Skip anything that doesn't look like a relation data file. */
			if (!parse_filename_for_nontemp_relation(de->d_name, &oidchars,
													 &forkNum))
				continue;

			/* Also skip it unless this is the init fork. */
			if (forkNum != INIT_FORKNUM)
				continue;

			/*
			 * Put the OID portion of the name into the hash table, if it
			 * isn't already.
			 */
			ent.reloid = atooid(de->d_name);
			(void) hash_search(hash, &ent, HASH_ENTER, NULL);
		}

		/* Done with the first pass. */
		FreeDir(dbspace_dir);

		/*
		 * If we didn't find any init forks, there's no point in continuing;
		 * we can bail out now.
		 */
		if (hash_get_num_entries(hash) == 0)
		{
			hash_destroy(hash);
			return;
		}

		/*
		 * Now, make a second pass and remove anything that matches.
		 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			oidchars;
			unlogged_relation_entry ent;

			/* Skip anything that doesn't look like a relation data file. */
			if (!parse_filename_for_nontemp_relation(de->d_name, &oidchars,
													 &forkNum))
				continue;

			/* We never remove the init fork. */
			if (forkNum == INIT_FORKNUM)
				continue;

			/*
			 * See whether the OID portion of the name shows up in the hash
			 * table.  If so, nuke it!
			 */
			ent.reloid = atooid(de->d_name);
			if (hash_search(hash, &ent, HASH_FIND, NULL))
			{
				snprintf(rm_path, sizeof(rm_path), "%s/%s",
						 dbspacedirname, de->d_name);
				if (unlink(rm_path) < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m",
									rm_path)));
				else
					elog(DEBUG2, "unlinked file \"%s\"", rm_path);
			}
		}

		/* Cleanup is complete. */
		FreeDir(dbspace_dir);
		hash_destroy(hash);
	}

	/*
	 * Initialization happens after cleanup is complete: we copy each init
	 * fork file to the corresponding main fork file.  Note that if we are
	 * asked to do both cleanup and init, we may never get here: if the
	 * cleanup code determines that there are no init forks in this dbspace,
	 * it will return before we get to this point.
	 */
	if ((op & UNLOGGED_RELATION_INIT) != 0)
	{
		/* Scan the directory. */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			oidchars;
			char		oidbuf[OIDCHARS + 1];
			char		srcpath[MAXPGPATH * 2];
			char		dstpath[MAXPGPATH];

			/* Skip anything that doesn't look like a relation data file. */
			if (!parse_filename_for_nontemp_relation(de->d_name, &oidchars,
													 &forkNum))
				continue;

			/* Also skip it unless this is the init fork. */
			if (forkNum != INIT_FORKNUM)
				continue;

			/* Construct source pathname. */
			snprintf(srcpath, sizeof(srcpath), "%s/%s",
					 dbspacedirname, de->d_name);

			/* Construct destination pathname. */
			memcpy(oidbuf, de->d_name, oidchars);
			oidbuf[oidchars] = '\0';
			snprintf(dstpath, sizeof(dstpath), "%s/%s%s",
					 dbspacedirname, oidbuf, de->d_name + oidchars + 1 +
					 strlen(forkNames[INIT_FORKNUM]));

			/* OK, we're ready to perform the actual copy. */
			elog(DEBUG2, "copying %s to %s", srcpath, dstpath);
			copy_file(srcpath, dstpath);
		}

		FreeDir(dbspace_dir);

		/*
		 * copy_file() above has already called pg_flush_data() on the files
		 * it created. Now we need to fsync those files, because a checkpoint
		 * won't do it for us while we're in recovery. We do this in a
		 * separate pass to allow the kernel to perform all the flushes
		 * (especially the metadata ones) at once.
		 */
		dbspace_dir = AllocateDir(dbspacedirname);
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			oidchars;
			char		oidbuf[OIDCHARS + 1];
			char		mainpath[MAXPGPATH];

			/* Skip anything that doesn't look like a relation data file. */
			if (!parse_filename_for_nontemp_relation(de->d_name, &oidchars,
													 &forkNum))
				continue;

			/* Also skip it unless this is the init fork. */
			if (forkNum != INIT_FORKNUM)
				continue;

			/* Construct main fork pathname. */
			memcpy(oidbuf, de->d_name, oidchars);
			oidbuf[oidchars] = '\0';
			snprintf(mainpath, sizeof(mainpath), "%s/%s%s",
					 dbspacedirname, oidbuf, de->d_name + oidchars + 1 +
					 strlen(forkNames[INIT_FORKNUM]));

			fsync_fname(mainpath, false);
		}

		FreeDir(dbspace_dir);

		/*
		 * Lastly, fsync the database directory itself, ensuring the
		 * filesystem remembers the file creations and deletions we've done.
		 * We don't bother with this during a call that does only
		 * UNLOGGED_RELATION_CLEANUP, because if recovery crashes before we
		 * get to doing UNLOGGED_RELATION_INIT, we'll redo the cleanup step
		 * too at the next startup attempt.
		 */
		fsync_fname(dbspacedirname, true);
	}
}

/*
 * Basic parsing of putative relation filenames.
 *
 * This function returns true if the file appears to be in the correct format
 * for a non-temporary relation and false otherwise.
 *
 * NB: If this function returns true, the caller is entitled to assume that
 * *oidchars has been set to the a value no more than OIDCHARS, and thus
 * that a buffer of OIDCHARS+1 characters is sufficient to hold the OID
 * portion of the filename.  This is critical to protect against a possible
 * buffer overrun.
 */
bool
parse_filename_for_nontemp_relation(const char *name, int *oidchars,
									ForkNumber *fork)
{
	int			pos;

	/* Look for a non-empty string of digits (that isn't too long). */
	for (pos = 0; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (pos == 0 || pos > OIDCHARS)
		return false;
	*oidchars = pos;

	/* Check for a fork name. */
	if (name[pos] != '_')
		*fork = MAIN_FORKNUM;
	else
	{
		int			forkchar;

		forkchar = forkname_chars(&name[pos + 1], fork);
		if (forkchar <= 0)
			return false;
		pos += forkchar + 1;
	}

	/* Check for a segment number. */
	if (name[pos] == '.')
	{
		int			segchar;

		for (segchar = 1; isdigit((unsigned char) name[pos + segchar]); ++segchar)
			;
		if (segchar <= 1)
			return false;
		pos += segchar;
	}

	/* Now we should be at the end. */
	if (name[pos] != '\0')
		return false;
	return true;
}
