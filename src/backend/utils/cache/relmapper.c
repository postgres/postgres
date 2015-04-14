/*-------------------------------------------------------------------------
 *
 * relmapper.c
 *	  Catalog-to-filenode mapping
 *
 * For most tables, the physical file underlying the table is specified by
 * pg_class.relfilenode.  However, that obviously won't work for pg_class
 * itself, nor for the other "nailed" catalogs for which we have to be able
 * to set up working Relation entries without access to pg_class.  It also
 * does not work for shared catalogs, since there is no practical way to
 * update other databases' pg_class entries when relocating a shared catalog.
 * Therefore, for these special catalogs (henceforth referred to as "mapped
 * catalogs") we rely on a separately maintained file that shows the mapping
 * from catalog OIDs to filenode numbers.  Each database has a map file for
 * its local mapped catalogs, and there is a separate map file for shared
 * catalogs.  Mapped catalogs have zero in their pg_class.relfilenode entries.
 *
 * Relocation of a normal table is committed (ie, the new physical file becomes
 * authoritative) when the pg_class row update commits.  For mapped catalogs,
 * the act of updating the map file is effectively commit of the relocation.
 * We postpone the file update till just before commit of the transaction
 * doing the rewrite, but there is necessarily a window between.  Therefore
 * mapped catalogs can only be relocated by operations such as VACUUM FULL
 * and CLUSTER, which make no transactionally-significant changes: it must be
 * safe for the new file to replace the old, even if the transaction itself
 * aborts.  An important factor here is that the indexes and toast table of
 * a mapped catalog must also be mapped, so that the rewrites/relocations of
 * all these files commit in a single map file update rather than being tied
 * to transaction commit.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/relmapper.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/catalog.h"
#include "catalog/pg_tablespace.h"
#include "catalog/storage.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "utils/inval.h"
#include "utils/relmapper.h"


/*
 * The map file is critical data: we have no automatic method for recovering
 * from loss or corruption of it.  We use a CRC so that we can detect
 * corruption.  To minimize the risk of failed updates, the map file should
 * be kept to no more than one standard-size disk sector (ie 512 bytes),
 * and we use overwrite-in-place rather than playing renaming games.
 * The struct layout below is designed to occupy exactly 512 bytes, which
 * might make filesystem updates a bit more efficient.
 *
 * Entries in the mappings[] array are in no particular order.  We could
 * speed searching by insisting on OID order, but it really shouldn't be
 * worth the trouble given the intended size of the mapping sets.
 */
#define RELMAPPER_FILENAME		"pg_filenode.map"

#define RELMAPPER_FILEMAGIC		0x592717		/* version ID value */

#define MAX_MAPPINGS			62		/* 62 * 8 + 16 = 512 */

typedef struct RelMapping
{
	Oid			mapoid;			/* OID of a catalog */
	Oid			mapfilenode;	/* its filenode number */
} RelMapping;

typedef struct RelMapFile
{
	int32		magic;			/* always RELMAPPER_FILEMAGIC */
	int32		num_mappings;	/* number of valid RelMapping entries */
	RelMapping	mappings[MAX_MAPPINGS];
	pg_crc32c	crc;			/* CRC of all above */
	int32		pad;			/* to make the struct size be 512 exactly */
} RelMapFile;

/*
 * The currently known contents of the shared map file and our database's
 * local map file are stored here.  These can be reloaded from disk
 * immediately whenever we receive an update sinval message.
 */
static RelMapFile shared_map;
static RelMapFile local_map;

/*
 * We use the same RelMapFile data structure to track uncommitted local
 * changes in the mappings (but note the magic and crc fields are not made
 * valid in these variables).  Currently, map updates are not allowed within
 * subtransactions, so one set of transaction-level changes is sufficient.
 *
 * The active_xxx variables contain updates that are valid in our transaction
 * and should be honored by RelationMapOidToFilenode.  The pending_xxx
 * variables contain updates we have been told about that aren't active yet;
 * they will become active at the next CommandCounterIncrement.  This setup
 * lets map updates act similarly to updates of pg_class rows, ie, they
 * become visible only at the next CommandCounterIncrement boundary.
 */
static RelMapFile active_shared_updates;
static RelMapFile active_local_updates;
static RelMapFile pending_shared_updates;
static RelMapFile pending_local_updates;


/* non-export function prototypes */
static void apply_map_update(RelMapFile *map, Oid relationId, Oid fileNode,
				 bool add_okay);
static void merge_map_updates(RelMapFile *map, const RelMapFile *updates,
				  bool add_okay);
static void load_relmap_file(bool shared);
static void write_relmap_file(bool shared, RelMapFile *newmap,
				  bool write_wal, bool send_sinval, bool preserve_files,
				  Oid dbid, Oid tsid, const char *dbpath);
static void perform_relmap_update(bool shared, const RelMapFile *updates);


/*
 * RelationMapOidToFilenode
 *
 * The raison d' etre ... given a relation OID, look up its filenode.
 *
 * Although shared and local relation OIDs should never overlap, the caller
 * always knows which we need --- so pass that information to avoid useless
 * searching.
 *
 * Returns InvalidOid if the OID is not known (which should never happen,
 * but the caller is in a better position to report a meaningful error).
 */
Oid
RelationMapOidToFilenode(Oid relationId, bool shared)
{
	const RelMapFile *map;
	int32		i;

	/* If there are active updates, believe those over the main maps */
	if (shared)
	{
		map = &active_shared_updates;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (relationId == map->mappings[i].mapoid)
				return map->mappings[i].mapfilenode;
		}
		map = &shared_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (relationId == map->mappings[i].mapoid)
				return map->mappings[i].mapfilenode;
		}
	}
	else
	{
		map = &active_local_updates;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (relationId == map->mappings[i].mapoid)
				return map->mappings[i].mapfilenode;
		}
		map = &local_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (relationId == map->mappings[i].mapoid)
				return map->mappings[i].mapfilenode;
		}
	}

	return InvalidOid;
}

/*
 * RelationMapFilenodeToOid
 *
 * Do the reverse of the normal direction of mapping done in
 * RelationMapOidToFilenode.
 *
 * This is not supposed to be used during normal running but rather for
 * information purposes when looking at the filesystem or xlog.
 *
 * Returns InvalidOid if the OID is not known; this can easily happen if the
 * relfilenode doesn't pertain to a mapped relation.
 */
Oid
RelationMapFilenodeToOid(Oid filenode, bool shared)
{
	const RelMapFile *map;
	int32		i;

	/* If there are active updates, believe those over the main maps */
	if (shared)
	{
		map = &active_shared_updates;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenode == map->mappings[i].mapfilenode)
				return map->mappings[i].mapoid;
		}
		map = &shared_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenode == map->mappings[i].mapfilenode)
				return map->mappings[i].mapoid;
		}
	}
	else
	{
		map = &active_local_updates;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenode == map->mappings[i].mapfilenode)
				return map->mappings[i].mapoid;
		}
		map = &local_map;
		for (i = 0; i < map->num_mappings; i++)
		{
			if (filenode == map->mappings[i].mapfilenode)
				return map->mappings[i].mapoid;
		}
	}

	return InvalidOid;
}

/*
 * RelationMapUpdateMap
 *
 * Install a new relfilenode mapping for the specified relation.
 *
 * If immediate is true (or we're bootstrapping), the mapping is activated
 * immediately.  Otherwise it is made pending until CommandCounterIncrement.
 */
void
RelationMapUpdateMap(Oid relationId, Oid fileNode, bool shared,
					 bool immediate)
{
	RelMapFile *map;

	if (IsBootstrapProcessingMode())
	{
		/*
		 * In bootstrap mode, the mapping gets installed in permanent map.
		 */
		if (shared)
			map = &shared_map;
		else
			map = &local_map;
	}
	else
	{
		/*
		 * We don't currently support map changes within subtransactions. This
		 * could be done with more bookkeeping infrastructure, but it doesn't
		 * presently seem worth it.
		 */
		if (GetCurrentTransactionNestLevel() > 1)
			elog(ERROR, "cannot change relation mapping within subtransaction");

		if (immediate)
		{
			/* Make it active, but only locally */
			if (shared)
				map = &active_shared_updates;
			else
				map = &active_local_updates;
		}
		else
		{
			/* Make it pending */
			if (shared)
				map = &pending_shared_updates;
			else
				map = &pending_local_updates;
		}
	}
	apply_map_update(map, relationId, fileNode, true);
}

/*
 * apply_map_update
 *
 * Insert a new mapping into the given map variable, replacing any existing
 * mapping for the same relation.
 *
 * In some cases the caller knows there must be an existing mapping; pass
 * add_okay = false to draw an error if not.
 */
static void
apply_map_update(RelMapFile *map, Oid relationId, Oid fileNode, bool add_okay)
{
	int32		i;

	/* Replace any existing mapping */
	for (i = 0; i < map->num_mappings; i++)
	{
		if (relationId == map->mappings[i].mapoid)
		{
			map->mappings[i].mapfilenode = fileNode;
			return;
		}
	}

	/* Nope, need to add a new mapping */
	if (!add_okay)
		elog(ERROR, "attempt to apply a mapping to unmapped relation %u",
			 relationId);
	if (map->num_mappings >= MAX_MAPPINGS)
		elog(ERROR, "ran out of space in relation map");
	map->mappings[map->num_mappings].mapoid = relationId;
	map->mappings[map->num_mappings].mapfilenode = fileNode;
	map->num_mappings++;
}

/*
 * merge_map_updates
 *
 * Merge all the updates in the given pending-update map into the target map.
 * This is just a bulk form of apply_map_update.
 */
static void
merge_map_updates(RelMapFile *map, const RelMapFile *updates, bool add_okay)
{
	int32		i;

	for (i = 0; i < updates->num_mappings; i++)
	{
		apply_map_update(map,
						 updates->mappings[i].mapoid,
						 updates->mappings[i].mapfilenode,
						 add_okay);
	}
}

/*
 * RelationMapRemoveMapping
 *
 * Remove a relation's entry in the map.  This is only allowed for "active"
 * (but not committed) local mappings.  We need it so we can back out the
 * entry for the transient target file when doing VACUUM FULL/CLUSTER on
 * a mapped relation.
 */
void
RelationMapRemoveMapping(Oid relationId)
{
	RelMapFile *map = &active_local_updates;
	int32		i;

	for (i = 0; i < map->num_mappings; i++)
	{
		if (relationId == map->mappings[i].mapoid)
		{
			/* Found it, collapse it out */
			map->mappings[i] = map->mappings[map->num_mappings - 1];
			map->num_mappings--;
			return;
		}
	}
	elog(ERROR, "could not find temporary mapping for relation %u",
		 relationId);
}

/*
 * RelationMapInvalidate
 *
 * This routine is invoked for SI cache flush messages.  We must re-read
 * the indicated map file.  However, we might receive a SI message in a
 * process that hasn't yet, and might never, load the mapping files;
 * for example the autovacuum launcher, which *must not* try to read
 * a local map since it is attached to no particular database.
 * So, re-read only if the map is valid now.
 */
void
RelationMapInvalidate(bool shared)
{
	if (shared)
	{
		if (shared_map.magic == RELMAPPER_FILEMAGIC)
			load_relmap_file(true);
	}
	else
	{
		if (local_map.magic == RELMAPPER_FILEMAGIC)
			load_relmap_file(false);
	}
}

/*
 * RelationMapInvalidateAll
 *
 * Reload all map files.  This is used to recover from SI message buffer
 * overflow: we can't be sure if we missed an inval message.
 * Again, reload only currently-valid maps.
 */
void
RelationMapInvalidateAll(void)
{
	if (shared_map.magic == RELMAPPER_FILEMAGIC)
		load_relmap_file(true);
	if (local_map.magic == RELMAPPER_FILEMAGIC)
		load_relmap_file(false);
}

/*
 * AtCCI_RelationMap
 *
 * Activate any "pending" relation map updates at CommandCounterIncrement time.
 */
void
AtCCI_RelationMap(void)
{
	if (pending_shared_updates.num_mappings != 0)
	{
		merge_map_updates(&active_shared_updates,
						  &pending_shared_updates,
						  true);
		pending_shared_updates.num_mappings = 0;
	}
	if (pending_local_updates.num_mappings != 0)
	{
		merge_map_updates(&active_local_updates,
						  &pending_local_updates,
						  true);
		pending_local_updates.num_mappings = 0;
	}
}

/*
 * AtEOXact_RelationMap
 *
 * Handle relation mapping at main-transaction commit or abort.
 *
 * During commit, this must be called as late as possible before the actual
 * transaction commit, so as to minimize the window where the transaction
 * could still roll back after committing map changes.  Although nothing
 * critically bad happens in such a case, we still would prefer that it
 * not happen, since we'd possibly be losing useful updates to the relations'
 * pg_class row(s).
 *
 * During abort, we just have to throw away any pending map changes.
 * Normal post-abort cleanup will take care of fixing relcache entries.
 */
void
AtEOXact_RelationMap(bool isCommit)
{
	if (isCommit)
	{
		/*
		 * We should not get here with any "pending" updates.  (We could
		 * logically choose to treat such as committed, but in the current
		 * code this should never happen.)
		 */
		Assert(pending_shared_updates.num_mappings == 0);
		Assert(pending_local_updates.num_mappings == 0);

		/*
		 * Write any active updates to the actual map files, then reset them.
		 */
		if (active_shared_updates.num_mappings != 0)
		{
			perform_relmap_update(true, &active_shared_updates);
			active_shared_updates.num_mappings = 0;
		}
		if (active_local_updates.num_mappings != 0)
		{
			perform_relmap_update(false, &active_local_updates);
			active_local_updates.num_mappings = 0;
		}
	}
	else
	{
		/* Abort --- drop all local and pending updates */
		active_shared_updates.num_mappings = 0;
		active_local_updates.num_mappings = 0;
		pending_shared_updates.num_mappings = 0;
		pending_local_updates.num_mappings = 0;
	}
}

/*
 * AtPrepare_RelationMap
 *
 * Handle relation mapping at PREPARE.
 *
 * Currently, we don't support preparing any transaction that changes the map.
 */
void
AtPrepare_RelationMap(void)
{
	if (active_shared_updates.num_mappings != 0 ||
		active_local_updates.num_mappings != 0 ||
		pending_shared_updates.num_mappings != 0 ||
		pending_local_updates.num_mappings != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that modified relation mapping")));
}

/*
 * CheckPointRelationMap
 *
 * This is called during a checkpoint.  It must ensure that any relation map
 * updates that were WAL-logged before the start of the checkpoint are
 * securely flushed to disk and will not need to be replayed later.  This
 * seems unlikely to be a performance-critical issue, so we use a simple
 * method: we just take and release the RelationMappingLock.  This ensures
 * that any already-logged map update is complete, because write_relmap_file
 * will fsync the map file before the lock is released.
 */
void
CheckPointRelationMap(void)
{
	LWLockAcquire(RelationMappingLock, LW_SHARED);
	LWLockRelease(RelationMappingLock);
}

/*
 * RelationMapFinishBootstrap
 *
 * Write out the initial relation mapping files at the completion of
 * bootstrap.  All the mapped files should have been made known to us
 * via RelationMapUpdateMap calls.
 */
void
RelationMapFinishBootstrap(void)
{
	Assert(IsBootstrapProcessingMode());

	/* Shouldn't be anything "pending" ... */
	Assert(active_shared_updates.num_mappings == 0);
	Assert(active_local_updates.num_mappings == 0);
	Assert(pending_shared_updates.num_mappings == 0);
	Assert(pending_local_updates.num_mappings == 0);

	/* Write the files; no WAL or sinval needed */
	write_relmap_file(true, &shared_map, false, false, false,
					  InvalidOid, GLOBALTABLESPACE_OID, NULL);
	write_relmap_file(false, &local_map, false, false, false,
					  MyDatabaseId, MyDatabaseTableSpace, DatabasePath);
}

/*
 * RelationMapInitialize
 *
 * This initializes the mapper module at process startup.  We can't access the
 * database yet, so just make sure the maps are empty.
 */
void
RelationMapInitialize(void)
{
	/* The static variables should initialize to zeroes, but let's be sure */
	shared_map.magic = 0;		/* mark it not loaded */
	local_map.magic = 0;
	shared_map.num_mappings = 0;
	local_map.num_mappings = 0;
	active_shared_updates.num_mappings = 0;
	active_local_updates.num_mappings = 0;
	pending_shared_updates.num_mappings = 0;
	pending_local_updates.num_mappings = 0;
}

/*
 * RelationMapInitializePhase2
 *
 * This is called to prepare for access to pg_database during startup.
 * We should be able to read the shared map file now.
 */
void
RelationMapInitializePhase2(void)
{
	/*
	 * In bootstrap mode, the map file isn't there yet, so do nothing.
	 */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * Load the shared map file, die on error.
	 */
	load_relmap_file(true);
}

/*
 * RelationMapInitializePhase3
 *
 * This is called as soon as we have determined MyDatabaseId and set up
 * DatabasePath.  At this point we should be able to read the local map file.
 */
void
RelationMapInitializePhase3(void)
{
	/*
	 * In bootstrap mode, the map file isn't there yet, so do nothing.
	 */
	if (IsBootstrapProcessingMode())
		return;

	/*
	 * Load the local map file, die on error.
	 */
	load_relmap_file(false);
}

/*
 * load_relmap_file -- load data from the shared or local map file
 *
 * Because the map file is essential for access to core system catalogs,
 * failure to read it is a fatal error.
 *
 * Note that the local case requires DatabasePath to be set up.
 */
static void
load_relmap_file(bool shared)
{
	RelMapFile *map;
	char		mapfilename[MAXPGPATH];
	pg_crc32c	crc;
	int			fd;

	if (shared)
	{
		snprintf(mapfilename, sizeof(mapfilename), "global/%s",
				 RELMAPPER_FILENAME);
		map = &shared_map;
	}
	else
	{
		snprintf(mapfilename, sizeof(mapfilename), "%s/%s",
				 DatabasePath, RELMAPPER_FILENAME);
		map = &local_map;
	}

	/* Read data ... */
	fd = OpenTransientFile(mapfilename,
						   O_RDONLY | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not open relation mapping file \"%s\": %m",
						mapfilename)));

	/*
	 * Note: we could take RelationMappingLock in shared mode here, but it
	 * seems unnecessary since our read() should be atomic against any
	 * concurrent updater's write().  If the file is updated shortly after we
	 * look, the sinval signaling mechanism will make us re-read it before we
	 * are able to access any relation that's affected by the change.
	 */
	if (read(fd, map, sizeof(RelMapFile)) != sizeof(RelMapFile))
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not read relation mapping file \"%s\": %m",
						mapfilename)));

	CloseTransientFile(fd);

	/* check for correct magic number, etc */
	if (map->magic != RELMAPPER_FILEMAGIC ||
		map->num_mappings < 0 ||
		map->num_mappings > MAX_MAPPINGS)
		ereport(FATAL,
				(errmsg("relation mapping file \"%s\" contains invalid data",
						mapfilename)));

	/* verify the CRC */
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (char *) map, offsetof(RelMapFile, crc));
	FIN_CRC32C(crc);

	if (!EQ_CRC32C(crc, map->crc))
		ereport(FATAL,
		  (errmsg("relation mapping file \"%s\" contains incorrect checksum",
				  mapfilename)));
}

/*
 * Write out a new shared or local map file with the given contents.
 *
 * The magic number and CRC are automatically updated in *newmap.  On
 * success, we copy the data to the appropriate permanent static variable.
 *
 * If write_wal is TRUE then an appropriate WAL message is emitted.
 * (It will be false for bootstrap and WAL replay cases.)
 *
 * If send_sinval is TRUE then a SI invalidation message is sent.
 * (This should be true except in bootstrap case.)
 *
 * If preserve_files is TRUE then the storage manager is warned not to
 * delete the files listed in the map.
 *
 * Because this may be called during WAL replay when MyDatabaseId,
 * DatabasePath, etc aren't valid, we require the caller to pass in suitable
 * values.  The caller is also responsible for being sure no concurrent
 * map update could be happening.
 */
static void
write_relmap_file(bool shared, RelMapFile *newmap,
				  bool write_wal, bool send_sinval, bool preserve_files,
				  Oid dbid, Oid tsid, const char *dbpath)
{
	int			fd;
	RelMapFile *realmap;
	char		mapfilename[MAXPGPATH];

	/*
	 * Fill in the overhead fields and update CRC.
	 */
	newmap->magic = RELMAPPER_FILEMAGIC;
	if (newmap->num_mappings < 0 || newmap->num_mappings > MAX_MAPPINGS)
		elog(ERROR, "attempt to write bogus relation mapping");

	INIT_CRC32C(newmap->crc);
	COMP_CRC32C(newmap->crc, (char *) newmap, offsetof(RelMapFile, crc));
	FIN_CRC32C(newmap->crc);

	/*
	 * Open the target file.  We prefer to do this before entering the
	 * critical section, so that an open() failure need not force PANIC.
	 */
	if (shared)
	{
		snprintf(mapfilename, sizeof(mapfilename), "global/%s",
				 RELMAPPER_FILENAME);
		realmap = &shared_map;
	}
	else
	{
		snprintf(mapfilename, sizeof(mapfilename), "%s/%s",
				 dbpath, RELMAPPER_FILENAME);
		realmap = &local_map;
	}

	fd = OpenTransientFile(mapfilename,
						   O_WRONLY | O_CREAT | PG_BINARY,
						   S_IRUSR | S_IWUSR);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open relation mapping file \"%s\": %m",
						mapfilename)));

	if (write_wal)
	{
		xl_relmap_update xlrec;
		XLogRecPtr	lsn;

		/* now errors are fatal ... */
		START_CRIT_SECTION();

		xlrec.dbid = dbid;
		xlrec.tsid = tsid;
		xlrec.nbytes = sizeof(RelMapFile);

		XLogBeginInsert();
		XLogRegisterData((char *) (&xlrec), MinSizeOfRelmapUpdate);
		XLogRegisterData((char *) newmap, sizeof(RelMapFile));

		lsn = XLogInsert(RM_RELMAP_ID, XLOG_RELMAP_UPDATE);

		/* As always, WAL must hit the disk before the data update does */
		XLogFlush(lsn);
	}

	errno = 0;
	if (write(fd, newmap, sizeof(RelMapFile)) != sizeof(RelMapFile))
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to relation mapping file \"%s\": %m",
						mapfilename)));
	}

	/*
	 * We choose to fsync the data to disk before considering the task done.
	 * It would be possible to relax this if it turns out to be a performance
	 * issue, but it would complicate checkpointing --- see notes for
	 * CheckPointRelationMap.
	 */
	if (pg_fsync(fd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fsync relation mapping file \"%s\": %m",
						mapfilename)));

	if (CloseTransientFile(fd))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close relation mapping file \"%s\": %m",
						mapfilename)));

	/*
	 * Now that the file is safely on disk, send sinval message to let other
	 * backends know to re-read it.  We must do this inside the critical
	 * section: if for some reason we fail to send the message, we have to
	 * force a database-wide PANIC.  Otherwise other backends might continue
	 * execution with stale mapping information, which would be catastrophic
	 * as soon as others began to use the now-committed data.
	 */
	if (send_sinval)
		CacheInvalidateRelmap(dbid);

	/*
	 * Make sure that the files listed in the map are not deleted if the outer
	 * transaction aborts.  This had better be within the critical section
	 * too: it's not likely to fail, but if it did, we'd arrive at transaction
	 * abort with the files still vulnerable.  PANICing will leave things in a
	 * good state on-disk.
	 *
	 * Note: we're cheating a little bit here by assuming that mapped files
	 * are either in pg_global or the database's default tablespace.
	 */
	if (preserve_files)
	{
		int32		i;

		for (i = 0; i < newmap->num_mappings; i++)
		{
			RelFileNode rnode;

			rnode.spcNode = tsid;
			rnode.dbNode = dbid;
			rnode.relNode = newmap->mappings[i].mapfilenode;
			RelationPreserveStorage(rnode, false);
		}
	}

	/* Success, update permanent copy */
	memcpy(realmap, newmap, sizeof(RelMapFile));

	/* Critical section done */
	if (write_wal)
		END_CRIT_SECTION();
}

/*
 * Merge the specified updates into the appropriate "real" map,
 * and write out the changes.  This function must be used for committing
 * updates during normal multiuser operation.
 */
static void
perform_relmap_update(bool shared, const RelMapFile *updates)
{
	RelMapFile	newmap;

	/*
	 * Anyone updating a relation's mapping info should take exclusive lock on
	 * that rel and hold it until commit.  This ensures that there will not be
	 * concurrent updates on the same mapping value; but there could easily be
	 * concurrent updates on different values in the same file. We cover that
	 * by acquiring the RelationMappingLock, re-reading the target file to
	 * ensure it's up to date, applying the updates, and writing the data
	 * before releasing RelationMappingLock.
	 *
	 * There is only one RelationMappingLock.  In principle we could try to
	 * have one per mapping file, but it seems unlikely to be worth the
	 * trouble.
	 */
	LWLockAcquire(RelationMappingLock, LW_EXCLUSIVE);

	/* Be certain we see any other updates just made */
	load_relmap_file(shared);

	/* Prepare updated data in a local variable */
	if (shared)
		memcpy(&newmap, &shared_map, sizeof(RelMapFile));
	else
		memcpy(&newmap, &local_map, sizeof(RelMapFile));

	/*
	 * Apply the updates to newmap.  No new mappings should appear, unless
	 * somebody is adding indexes to system catalogs.
	 */
	merge_map_updates(&newmap, updates, allowSystemTableMods);

	/* Write out the updated map and do other necessary tasks */
	write_relmap_file(shared, &newmap, true, true, true,
					  (shared ? InvalidOid : MyDatabaseId),
					  (shared ? GLOBALTABLESPACE_OID : MyDatabaseTableSpace),
					  DatabasePath);

	/* Now we can release the lock */
	LWLockRelease(RelationMappingLock);
}

/*
 * RELMAP resource manager's routines
 */
void
relmap_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in relmap records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_RELMAP_UPDATE)
	{
		xl_relmap_update *xlrec = (xl_relmap_update *) XLogRecGetData(record);
		RelMapFile	newmap;
		char	   *dbpath;

		if (xlrec->nbytes != sizeof(RelMapFile))
			elog(PANIC, "relmap_redo: wrong size %u in relmap update record",
				 xlrec->nbytes);
		memcpy(&newmap, xlrec->data, sizeof(newmap));

		/* We need to construct the pathname for this database */
		dbpath = GetDatabasePath(xlrec->dbid, xlrec->tsid);

		/*
		 * Write out the new map and send sinval, but of course don't write a
		 * new WAL entry.  There's no surrounding transaction to tell to
		 * preserve files, either.
		 *
		 * There shouldn't be anyone else updating relmaps during WAL replay,
		 * so we don't bother to take the RelationMappingLock.  We would need
		 * to do so if load_relmap_file needed to interlock against writers.
		 */
		write_relmap_file((xlrec->dbid == InvalidOid), &newmap,
						  false, true, false,
						  xlrec->dbid, xlrec->tsid, dbpath);

		pfree(dbpath);
	}
	else
		elog(PANIC, "relmap_redo: unknown op code %u", info);
}
