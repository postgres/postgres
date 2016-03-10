/*-------------------------------------------------------------------------
 *
 * reinit.c
 *	  Reinitialization of unlogged relations
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/reinit.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "catalog/catalog.h"
#include "common/relpath.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/reinit.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

static void ResetUnloggedRelationsInTablespaceDir(const char *tsdirname,
									  int op);
static void ResetUnloggedRelationsInDbspaceDir(const char *dbspacedirname,
								   int op);
static bool parse_filename_for_nontemp_relation(const char *name,
									int *oidchars, ForkNumber *fork);

typedef struct
{
	char		oid[OIDCHARS + 1];
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
	char		temp_path[MAXPGPATH];
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
								   ALLOCSET_DEFAULT_MINSIZE,
								   ALLOCSET_DEFAULT_INITSIZE,
								   ALLOCSET_DEFAULT_MAXSIZE);
	oldctx = MemoryContextSwitchTo(tmpctx);

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

/* Process one tablespace directory for ResetUnloggedRelations */
static void
ResetUnloggedRelationsInTablespaceDir(const char *tsdirname, int op)
{
	DIR		   *ts_dir;
	struct dirent *de;
	char		dbspace_path[MAXPGPATH];

	ts_dir = AllocateDir(tsdirname);
	if (ts_dir == NULL)
	{
		/* anything except ENOENT is fishy */
		if (errno != ENOENT)
			elog(LOG,
				 "could not open tablespace directory \"%s\": %m",
				 tsdirname);
		return;
	}

	while ((de = ReadDir(ts_dir, tsdirname)) != NULL)
	{
		int			i = 0;

		/*
		 * We're only interested in the per-database directories, which have
		 * numeric names.  Note that this code will also (properly) ignore "."
		 * and "..".
		 */
		while (isdigit((unsigned char) de->d_name[i]))
			++i;
		if (de->d_name[i] != '\0' || i == 0)
			continue;

		snprintf(dbspace_path, sizeof(dbspace_path), "%s/%s",
				 tsdirname, de->d_name);
		ResetUnloggedRelationsInDbspaceDir(dbspace_path, op);
	}

	FreeDir(ts_dir);
}

/* Process one per-dbspace directory for ResetUnloggedRelations */
static void
ResetUnloggedRelationsInDbspaceDir(const char *dbspacedirname, int op)
{
	DIR		   *dbspace_dir;
	struct dirent *de;
	char		rm_path[MAXPGPATH];

	/* Caller must specify at least one operation. */
	Assert((op & (UNLOGGED_RELATION_CLEANUP | UNLOGGED_RELATION_INIT)) != 0);

	/*
	 * Cleanup is a two-pass operation.  First, we go through and identify all
	 * the files with init forks.  Then, we go through again and nuke
	 * everything with the same OID except the init fork.
	 */
	if ((op & UNLOGGED_RELATION_CLEANUP) != 0)
	{
		HTAB	   *hash = NULL;
		HASHCTL		ctl;

		/* Open the directory. */
		dbspace_dir = AllocateDir(dbspacedirname);
		if (dbspace_dir == NULL)
		{
			elog(LOG,
				 "could not open dbspace directory \"%s\": %m",
				 dbspacedirname);
			return;
		}

		/*
		 * It's possible that someone could create a ton of unlogged relations
		 * in the same database & tablespace, so we'd better use a hash table
		 * rather than an array or linked list to keep track of which files
		 * need to be reset.  Otherwise, this cleanup operation would be
		 * O(n^2).
		 */
		ctl.keysize = sizeof(unlogged_relation_entry);
		ctl.entrysize = sizeof(unlogged_relation_entry);
		hash = hash_create("unlogged hash", 32, &ctl, HASH_ELEM);

		/* Scan the directory. */
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
			memset(ent.oid, 0, sizeof(ent.oid));
			memcpy(ent.oid, de->d_name, oidchars);
			hash_search(hash, &ent, HASH_ENTER, NULL);
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
		 * Now, make a second pass and remove anything that matches. First,
		 * reopen the directory.
		 */
		dbspace_dir = AllocateDir(dbspacedirname);
		if (dbspace_dir == NULL)
		{
			elog(LOG,
				 "could not open dbspace directory \"%s\": %m",
				 dbspacedirname);
			hash_destroy(hash);
			return;
		}

		/* Scan the directory. */
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			oidchars;
			bool		found;
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
			 * table.
			 */
			memset(ent.oid, 0, sizeof(ent.oid));
			memcpy(ent.oid, de->d_name, oidchars);
			hash_search(hash, &ent, HASH_FIND, &found);

			/* If so, nuke it! */
			if (found)
			{
				snprintf(rm_path, sizeof(rm_path), "%s/%s",
						 dbspacedirname, de->d_name);

				/*
				 * It's tempting to actually throw an error here, but since
				 * this code gets run during database startup, that could
				 * result in the database failing to start.  (XXX Should we do
				 * it anyway?)
				 */
				if (unlink(rm_path))
					elog(LOG, "could not unlink file \"%s\": %m", rm_path);
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
		/* Open the directory. */
		dbspace_dir = AllocateDir(dbspacedirname);
		if (dbspace_dir == NULL)
		{
			/* we just saw this directory, so it really ought to be there */
			elog(LOG,
				 "could not open dbspace directory \"%s\": %m",
				 dbspacedirname);
			return;
		}

		/* Scan the directory. */
		while ((de = ReadDir(dbspace_dir, dbspacedirname)) != NULL)
		{
			ForkNumber	forkNum;
			int			oidchars;
			char		oidbuf[OIDCHARS + 1];
			char		srcpath[MAXPGPATH];
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
		if (dbspace_dir == NULL)
		{
			/* we just saw this directory, so it really ought to be there */
			elog(LOG,
				 "could not open dbspace directory \"%s\": %m",
				 dbspacedirname);
			return;
		}

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
static bool
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
