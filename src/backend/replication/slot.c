/*-------------------------------------------------------------------------
 *
 * slot.c
 *	   Replication slot management.
 *
 *
 * Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/slot.c
 *
 * NOTES
 *
 * Replication slots are used to keep state about replication streams
 * originating from this cluster.  Their primary purpose is to prevent the
 * premature removal of WAL or of old tuple versions in a manner that would
 * interfere with replication; they also useful for monitoring purposes.
 * Slots need to be permanent (to allow restarts), crash-safe, and allocatable
 * on standbys (to support cascading setups).  The requirement that slots be
 * usable on standbys precludes storing them in the system catalogs.
 *
 * Each replication slot gets its own directory inside the $PGDATA/pg_replslot
 * directory. Inside that directory the state file will contain the slot's
 * own data. Additional data can be stored alongside that file if required.
 * While the server is running, the state data is also cached in memory for
 * efficiency.
 *
 * ReplicationSlotAllocationLock must be taken in exclusive mode to allocate
 * or free a slot. ReplicationSlotControlLock must be taken in shared mode
 * to iterate over the slots, and in exclusive mode to change the in_use flag
 * of a slot.  The remaining data in each slot is protected by its mutex.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/transam.h"
#include "miscadmin.h"
#include "replication/slot.h"
#include "storage/fd.h"
#include "storage/procarray.h"

/*
 * Replication slot on-disk data structure.
 */
typedef struct ReplicationSlotOnDisk
{
	/* first part of this struct needs to be version independent */

	/* data not covered by checksum */
	uint32		magic;
	pg_crc32	checksum;

	/* data covered by checksum */
	uint32		version;
	uint32		length;

	ReplicationSlotPersistentData slotdata;
} ReplicationSlotOnDisk;

/* size of the part of the slot that is version independent */
#define ReplicationSlotOnDiskConstantSize \
	offsetof(ReplicationSlotOnDisk, slotdata)
/* size of the slots that is not version indepenent */
#define ReplicationSlotOnDiskDynamicSize \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskConstantSize

#define SLOT_MAGIC		0x1051CA1		/* format identifier */
#define SLOT_VERSION	1				/* version for new files */

/* Control array for replication slot management */
ReplicationSlotCtlData *ReplicationSlotCtl = NULL;

/* My backend's replication slot in the shared memory array */
ReplicationSlot *MyReplicationSlot = NULL;

/* GUCs */
int			max_replication_slots = 0;	/* the maximum number of replication slots */

/* internal persistency functions */
static void RestoreSlotFromDisk(const char *name);
static void CreateSlotOnDisk(ReplicationSlot *slot);
static void SaveSlotToPath(ReplicationSlot *slot, const char *path, int elevel);

/*
 * Report shared-memory space needed by ReplicationSlotShmemInit.
 */
Size
ReplicationSlotsShmemSize(void)
{
	Size		size = 0;

	if (max_replication_slots == 0)
		return size;

	size = offsetof(ReplicationSlotCtlData, replication_slots);
	size = add_size(size,
					mul_size(max_replication_slots, sizeof(ReplicationSlot)));

	return size;
}

/*
 * Allocate and initialize walsender-related shared memory.
 */
void
ReplicationSlotsShmemInit(void)
{
	bool		found;

	if (max_replication_slots == 0)
		return;

	ReplicationSlotCtl = (ReplicationSlotCtlData *)
		ShmemInitStruct("ReplicationSlot Ctl", ReplicationSlotsShmemSize(),
						&found);

	if (!found)
	{
		int			i;

		/* First time through, so initialize */
		MemSet(ReplicationSlotCtl, 0, ReplicationSlotsShmemSize());

		for (i = 0; i < max_replication_slots; i++)
		{
			ReplicationSlot *slot = &ReplicationSlotCtl->replication_slots[i];

			/* everything else is zeroed by the memset above */
			SpinLockInit(&slot->mutex);
			slot->io_in_progress_lock = LWLockAssign();
		}
	}
}

/*
 * Check whether the passed slot name is valid and report errors at elevel.
 *
 * Slot names may consist out of [a-z0-9_]{1,NAMEDATALEN-1} which should allow
 * the name to be uses as a directory name on every supported OS.
 *
 * Returns whether the directory name is valid or not if elevel < ERROR.
 */
bool
ReplicationSlotValidateName(const char *name, int elevel)
{
	const char *cp;

	if (strlen(name) == 0)
	{
		ereport(elevel,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("replication slot name \"%s\" is too short",
						name)));
		return false;
	}

	if (strlen(name) >= NAMEDATALEN)
	{
		ereport(elevel,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("replication slot name \"%s\" is too long",
						name)));
		return false;
	}

	for (cp = name; *cp; cp++)
	{
		if (!((*cp >= 'a' && *cp <= 'z')
			  || (*cp >= '0' && *cp <= '9')
			  || (*cp == '_')))
		{
			ereport(elevel,
					(errcode(ERRCODE_INVALID_NAME),
					 errmsg("replication slot name \"%s\" contains invalid character",
							name),
					 errhint("Replication slot names may only contain letters, numbers and the underscore character.")));
			return false;
		}
	}
	return true;
}

/*
 * Create a new replication slot and mark it as used by this backend.
 *
 * name: Name of the slot
 * db_specific: changeset extraction is db specific, if the slot is going to
 *     be used for that pass true, otherwise false.
 */
void
ReplicationSlotCreate(const char *name, bool db_specific)
{
	ReplicationSlot *slot = NULL;
	int			i;

	Assert(MyReplicationSlot == NULL);

	ReplicationSlotValidateName(name, ERROR);

	/*
	 * If some other backend ran this code currently with us, we'd likely
	 * both allocate the same slot, and that would be bad.  We'd also be
	 * at risk of missing a name collision.  Also, we don't want to try to
	 * create a new slot while somebody's busy cleaning up an old one, because
	 * we might both be monkeying with the same directory.
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/*
	 * Check for name collision, and identify an allocatable slot.  We need
	 * to hold ReplicationSlotControlLock in shared mode for this, so that
	 * nobody else can change the in_use flags while we're looking at them.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("replication slot \"%s\" already exists", name)));
		if (!s->in_use && slot == NULL)
			slot = s;
	}
	LWLockRelease(ReplicationSlotControlLock);

	/* If all slots are in use, we're out of luck. */
	if (slot == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("all replication slots are in use"),
				 errhint("Free one or increase max_replication_slots.")));

	/*
	 * Since this slot is not in use, nobody should be looking at any
	 * part of it other than the in_use field unless they're trying to allocate
	 * it.  And since we hold ReplicationSlotAllocationLock, nobody except us
	 * can be doing that.  So it's safe to initialize the slot.
	 */
	Assert(!slot->in_use);
	Assert(!slot->active);
	slot->data.xmin = InvalidTransactionId;
	slot->effective_xmin = InvalidTransactionId;
	strncpy(NameStr(slot->data.name), name, NAMEDATALEN);
	NameStr(slot->data.name)[NAMEDATALEN - 1] = '\0';
	slot->data.database = db_specific ? MyDatabaseId : InvalidOid;
	slot->data.restart_lsn = InvalidXLogRecPtr;

	/*
	 * Create the slot on disk.  We haven't actually marked the slot allocated
	 * yet, so no special cleanup is required if this errors out.
	 */
	CreateSlotOnDisk(slot);

	/*
	 * We need to briefly prevent any other backend from iterating over the
	 * slots while we flip the in_use flag. We also need to set the active
	 * flag while holding the ControlLock as otherwise a concurrent
	 * SlotAcquire() could acquire the slot as well.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);

	slot->in_use = true;

	/* We can now mark the slot active, and that makes it our slot. */
	{
		volatile ReplicationSlot *vslot = slot;

		SpinLockAcquire(&slot->mutex);
		Assert(!vslot->active);
		vslot->active = true;
		SpinLockRelease(&slot->mutex);
		MyReplicationSlot = slot;
	}

	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * Now that the slot has been marked as in_use and in_active, it's safe to
	 * let somebody else try to allocate a slot.
	 */
	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Find an previously created slot and mark it as used by this backend.
 */
void
ReplicationSlotAcquire(const char *name)
{
	ReplicationSlot *slot = NULL;
	int			i;
	bool		active = false;

	Assert(MyReplicationSlot == NULL);

	ReplicationSlotValidateName(name, ERROR);

	/* Search for the named slot and mark it active if we find it. */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
		{
			volatile ReplicationSlot *vslot = s;

			SpinLockAcquire(&s->mutex);
			active = vslot->active;
			vslot->active = true;
			SpinLockRelease(&s->mutex);
			slot = s;
			break;
		}
	}
	LWLockRelease(ReplicationSlotControlLock);

	/* If we did not find the slot or it was already active, error out. */
	if (slot == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist", name)));
	if (active)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("replication slot \"%s\" is already active", name)));

	/* We made this slot active, so it's ours now. */
	MyReplicationSlot = slot;
}

/*
 * Release a replication slot, this or another backend can ReAcquire it
 * later. Resources this slot requires will be preserved.
 */
void
ReplicationSlotRelease(void)
{
	ReplicationSlot *slot = MyReplicationSlot;

	Assert(slot != NULL && slot->active);

	/* Mark slot inactive.  We're not freeing it, just disconnecting. */
	{
		volatile ReplicationSlot *vslot = slot;
		SpinLockAcquire(&slot->mutex);
		vslot->active = false;
		SpinLockRelease(&slot->mutex);
		MyReplicationSlot = NULL;
	}
}

/*
 * Permanently drop replication slot identified by the passed in name.
 */
void
ReplicationSlotDrop(const char *name)
{
	ReplicationSlot *slot = NULL;
	int			i;
	bool		active;
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];

	ReplicationSlotValidateName(name, ERROR);

	/*
	 * If some other backend ran this code currently with us, we might both
	 * try to free the same slot at the same time.  Or we might try to delete
	 * a slot with a certain name while someone else was trying to create a
	 * slot with the same name.
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

	/* Search for the named slot and mark it active if we find it. */
	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];

		if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0)
		{
			volatile ReplicationSlot *vslot = s;

			SpinLockAcquire(&s->mutex);
			active = vslot->active;
			vslot->active = true;
			SpinLockRelease(&s->mutex);
			slot = s;
			break;
		}
	}
	LWLockRelease(ReplicationSlotControlLock);

	/* If we did not find the slot or it was already active, error out. */
	if (slot == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("replication slot \"%s\" does not exist", name)));
	if (active)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("replication slot \"%s\" is already active", name)));

	/* Generate pathnames. */
	sprintf(path, "pg_replslot/%s", NameStr(slot->data.name));
	sprintf(tmppath, "pg_replslot/%s.tmp", NameStr(slot->data.name));

	/*
	 * Rename the slot directory on disk, so that we'll no longer recognize
	 * this as a valid slot.  Note that if this fails, we've got to mark the
	 * slot inactive again before bailing out.
	 */
	if (rename(path, tmppath) != 0)
	{
		volatile ReplicationSlot *vslot = slot;

		SpinLockAcquire(&slot->mutex);
		vslot->active = false;
		SpinLockRelease(&slot->mutex);

		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename \"%s\" to \"%s\": %m",
						path, tmppath)));
	}

	/*
	 * We need to fsync() the directory we just renamed and its parent to make
	 * sure that our changes are on disk in a crash-safe fashion.  If fsync()
	 * fails, we can't be sure whether the changes are on disk or not.  For
	 * now, we handle that by panicking; StartupReplicationSlots() will
	 * try to straighten it out after restart.
	 */
	START_CRIT_SECTION();
	fsync_fname(tmppath, true);
	fsync_fname("pg_replslot", true);
	END_CRIT_SECTION();

	/*
	 * The slot is definitely gone.  Lock out concurrent scans of the array
	 * long enough to kill it.  It's OK to clear the active flag here without
	 * grabbing the mutex because nobody else can be scanning the array here,
	 * and nobody can be attached to this slot and thus access it without
	 * scanning the array.
	 */
	LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);
	slot->active = false;
	slot->in_use = false;
	LWLockRelease(ReplicationSlotControlLock);

	/*
	 * Slot is dead and doesn't prevent resource removal anymore, recompute
	 * limits.
	 */
	ReplicationSlotsComputeRequiredXmin();
	ReplicationSlotsComputeRequiredLSN();

	/*
	 * If removing the directory fails, the worst thing that will happen is
	 * that the user won't be able to create a new slot with the same name
	 * until the next server restart.  We warn about it, but that's all.
	 */
	if (!rmtree(tmppath, true))
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not remove directory \"%s\"", tmppath)));

	/*
	 * We release this at the very end, so that nobody starts trying to create
	 * a slot while we're still cleaning up the detritus of the old one.
	 */
	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Serialize the currently acquired slot's state from memory to disk, thereby
 * guaranteeing the current state will survive a crash.
 */
void
ReplicationSlotSave(void)
{
	char		path[MAXPGPATH];

	Assert(MyReplicationSlot != NULL);

	sprintf(path, "pg_replslot/%s", NameStr(MyReplicationSlot->data.name));
	SaveSlotToPath(MyReplicationSlot, path, ERROR);
}

/*
 * Signal that it would be useful if the currently acquired slot would be
 * flushed out to disk.
 *
 * Note that the actual flush to disk can be delayed for a long time, if
 * required for correctness explicitly do a ReplicationSlotSave().
 */
void
ReplicationSlotMarkDirty(void)
{
	Assert(MyReplicationSlot != NULL);

	{
		volatile ReplicationSlot *vslot = MyReplicationSlot;

		SpinLockAcquire(&vslot->mutex);
		MyReplicationSlot->just_dirtied = true;
		MyReplicationSlot->dirty = true;
		SpinLockRelease(&vslot->mutex);
	}
}

/*
 * Compute the oldest xmin across all slots and store it in the ProcArray.
 */
void
ReplicationSlotsComputeRequiredXmin(void)
{
	int			i;
	TransactionId agg_xmin = InvalidTransactionId;

	Assert(ReplicationSlotCtl != NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		TransactionId	effective_xmin;

		if (!s->in_use)
			continue;

		{
			volatile ReplicationSlot *vslot = s;

			SpinLockAcquire(&s->mutex);
			effective_xmin = vslot->effective_xmin;
			SpinLockRelease(&s->mutex);
		}

		/* check the data xmin */
		if (TransactionIdIsValid(effective_xmin) &&
			(!TransactionIdIsValid(agg_xmin) ||
			 TransactionIdPrecedes(effective_xmin, agg_xmin)))
			agg_xmin = effective_xmin;
	}
	LWLockRelease(ReplicationSlotControlLock);

	ProcArraySetReplicationSlotXmin(agg_xmin);
}

/*
 * Compute the oldest restart LSN across all slots and inform xlog module.
 */
void
ReplicationSlotsComputeRequiredLSN(void)
{
	int			i;
	XLogRecPtr  min_required = InvalidXLogRecPtr;

	Assert(ReplicationSlotCtl != NULL);

	LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		XLogRecPtr restart_lsn;

		if (!s->in_use)
			continue;

		{
			volatile ReplicationSlot *vslot = s;

			SpinLockAcquire(&s->mutex);
			restart_lsn = vslot->data.restart_lsn;
			SpinLockRelease(&s->mutex);
		}

		if (restart_lsn != InvalidXLogRecPtr &&
			(min_required == InvalidXLogRecPtr ||
			 restart_lsn < min_required))
			min_required = restart_lsn;
	}
	LWLockRelease(ReplicationSlotControlLock);

	XLogSetReplicationSlotMinimumLSN(min_required);
}

/*
 * Check whether the server's configuration supports using replication
 * slots.
 */
void
CheckSlotRequirements(void)
{
	if (max_replication_slots == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 (errmsg("replication slots can only be used if max_replication_slots > 0"))));

	if (wal_level < WAL_LEVEL_ARCHIVE)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("replication slots can only be used if wal_level >= archive")));
}

/*
 * Returns whether the string `str' has the postfix `end'.
 */
static bool
string_endswith(const char *str, const char *end)
{
	size_t slen = strlen(str);
	size_t elen = strlen(end);

	/* can't be a postfix if longer */
	if (elen > slen)
		return false;

	/* compare the end of the strings */
	str += slen - elen;
	return strcmp(str, end) == 0;
}

/*
 * Flush all replication slots to disk.
 *
 * This needn't actually be part of a checkpoint, but it's a convenient
 * location.
 */
void
CheckPointReplicationSlots(void)
{
	int			i;

	ereport(DEBUG1,
			(errmsg("performing replication slot checkpoint")));

	/*
	 * Prevent any slot from being created/dropped while we're active. As we
	 * explicitly do *not* want to block iterating over replication_slots or
	 * acquiring a slot we cannot take the control lock - but that's OK,
	 * because holding ReplicationSlotAllocationLock is strictly stronger,
	 * and enough to guarantee that nobody can change the in_use bits on us.
	 */
	LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);

	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *s = &ReplicationSlotCtl->replication_slots[i];
		char		path[MAXPGPATH];

		if (!s->in_use)
			continue;

		/* save the slot to disk, locking is handled in SaveSlotToPath() */
		sprintf(path, "pg_replslot/%s", NameStr(s->data.name));
		SaveSlotToPath(s, path, LOG);
	}
	LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Load all replication slots from disk into memory at server startup. This
 * needs to be run before we start crash recovery.
 */
void
StartupReplicationSlots(XLogRecPtr checkPointRedo)
{
	DIR		   *replication_dir;
	struct dirent *replication_de;

	ereport(DEBUG1,
			(errmsg("starting up replication slots")));

	/* restore all slots by iterating over all on-disk entries */
	replication_dir = AllocateDir("pg_replslot");
	while ((replication_de = ReadDir(replication_dir, "pg_replslot")) != NULL)
	{
		struct stat	statbuf;
		char		path[MAXPGPATH];

		if (strcmp(replication_de->d_name, ".") == 0 ||
			strcmp(replication_de->d_name, "..") == 0)
			continue;

		snprintf(path, MAXPGPATH, "pg_replslot/%s", replication_de->d_name);

		/* we're only creating directories here, skip if it's not our's */
		if (lstat(path, &statbuf) == 0 && !S_ISDIR(statbuf.st_mode))
			continue;

		/* we crashed while a slot was being setup or deleted, clean up */
		if (string_endswith(replication_de->d_name, ".tmp"))
		{
			if (!rmtree(path, true))
			{
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove directory \"%s\"", path)));
				continue;
			}
			fsync_fname("pg_replslot", true);
			continue;
		}

		/* looks like a slot in a normal state, restore */
		RestoreSlotFromDisk(replication_de->d_name);
	}
	FreeDir(replication_dir);

	/* currently no slots exist, we're done. */
	if (max_replication_slots <= 0)
		return;

	/* Now that we have recovered all the data, compute replication xmin */
	ReplicationSlotsComputeRequiredXmin();
	ReplicationSlotsComputeRequiredLSN();
}

/* ----
 * Manipulation of ondisk state of replication slots
 *
 * NB: none of the routines below should take any notice whether a slot is the
 * current one or not, that's all handled a layer above.
 * ----
 */
static void
CreateSlotOnDisk(ReplicationSlot *slot)
{
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	struct stat	st;

	/*
	 * No need to take out the io_in_progress_lock, nobody else can see this
	 * slot yet, so nobody else wil write. We're reusing SaveSlotToPath which
	 * takes out the lock, if we'd take the lock here, we'd deadlock.
	 */

	sprintf(path, "pg_replslot/%s", NameStr(slot->data.name));
	sprintf(tmppath, "pg_replslot/%s.tmp", NameStr(slot->data.name));

	/*
	 * It's just barely possible that some previous effort to create or
	 * drop a slot with this name left a temp directory lying around.
	 * If that seems to be the case, try to remove it.  If the rmtree()
	 * fails, we'll error out at the mkdir() below, so we don't bother
	 * checking success.
	 */
	if (stat(tmppath, &st) == 0 && S_ISDIR(st.st_mode))
		rmtree(tmppath, true);

	/* Create and fsync the temporary slot directory. */
	if (mkdir(tmppath, S_IRWXU) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m",
						tmppath)));
	fsync_fname(tmppath, true);

	/* Write the actual state file. */
	slot->dirty = true; /* signal that we really need to write */
	SaveSlotToPath(slot, tmppath, ERROR);

	/* Rename the directory into place. */
	if (rename(tmppath, path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						tmppath, path)));

	/*
	 * If we'd now fail - really unlikely - we wouldn't know wether this slot
	 * would persist after an OS crash or not - so, force a restart. The
	 * restart would try to fysnc this again till it works.
	 */
	START_CRIT_SECTION();

	fsync_fname(path, true);
	fsync_fname("pg_replslot", true);

	END_CRIT_SECTION();
}

/*
 * Shared functionality between saving and creating a replication slot.
 */
static void
SaveSlotToPath(ReplicationSlot *slot, const char *dir, int elevel)
{
	char		tmppath[MAXPGPATH];
	char		path[MAXPGPATH];
	int			fd;
	ReplicationSlotOnDisk cp;
	bool		was_dirty;

	/* first check whether there's something to write out */
	{
		volatile ReplicationSlot *vslot = slot;

		SpinLockAcquire(&vslot->mutex);
		was_dirty = vslot->dirty;
		vslot->just_dirtied = false;
		SpinLockRelease(&vslot->mutex);
	}

	/* and don't do anything if there's nothing to write */
	if (!was_dirty)
		return;

	LWLockAcquire(slot->io_in_progress_lock, LW_EXCLUSIVE);

	/* silence valgrind :( */
	memset(&cp, 0, sizeof(ReplicationSlotOnDisk));

	sprintf(tmppath, "%s/state.tmp", dir);
	sprintf(path, "%s/state", dir);

	fd = OpenTransientFile(tmppath,
						   O_CREAT | O_EXCL | O_WRONLY | PG_BINARY,
						   S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m",
						tmppath)));
		return;
	}

	cp.magic = SLOT_MAGIC;
	INIT_CRC32(cp.checksum);
	cp.version = 1;
	cp.length = ReplicationSlotOnDiskDynamicSize;

	SpinLockAcquire(&slot->mutex);

	memcpy(&cp.slotdata, &slot->data, sizeof(ReplicationSlotPersistentData));

	SpinLockRelease(&slot->mutex);

	COMP_CRC32(cp.checksum,
			   (char *)(&cp) + ReplicationSlotOnDiskConstantSize,
			   ReplicationSlotOnDiskDynamicSize);

	if ((write(fd, &cp, sizeof(cp))) != sizeof(cp))
	{
		int save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m",
						tmppath)));
		return;
	}

	/* fsync the temporary file */
	if (pg_fsync(fd) != 0)
	{
		int save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						tmppath)));
		return;
	}

	CloseTransientFile(fd);

	/* rename to permanent file, fsync file and directory */
	if (rename(tmppath, path) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename \"%s\" to \"%s\": %m",
						tmppath, path)));
		return;
	}

	/* Check CreateSlot() for the reasoning of using a crit. section. */
	START_CRIT_SECTION();

	fsync_fname(path, false);
	fsync_fname((char *) dir, true);
	fsync_fname("pg_replslot", true);

	END_CRIT_SECTION();

	/*
	 * Successfully wrote, unset dirty bit, unless somebody dirtied again
	 * already.
	 */
	{
		volatile ReplicationSlot *vslot = slot;

		SpinLockAcquire(&vslot->mutex);
		if (!vslot->just_dirtied)
			vslot->dirty = false;
		SpinLockRelease(&vslot->mutex);
	}

	LWLockRelease(slot->io_in_progress_lock);
}

/*
 * Load a single slot from disk into memory.
 */
static void
RestoreSlotFromDisk(const char *name)
{
	ReplicationSlotOnDisk cp;
	int			i;
	char		path[MAXPGPATH];
	int			fd;
	bool		restored = false;
	int			readBytes;
	pg_crc32	checksum;

	/* no need to lock here, no concurrent access allowed yet */

	/* delete temp file if it exists */
	sprintf(path, "pg_replslot/%s/state.tmp", name);
	if (unlink(path) < 0 && errno != ENOENT)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not unlink file \"%s\": %m", path)));

	sprintf(path, "pg_replslot/%s/state", name);

	elog(DEBUG1, "restoring replication slot from \"%s\"", path);

	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY, 0);

	/*
	 * We do not need to handle this as we are rename()ing the directory into
	 * place only after we fsync()ed the state file.
	 */
	if (fd < 0)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	/*
	 * Sync state file before we're reading from it. We might have crashed
	 * while it wasn't synced yet and we shouldn't continue on that basis.
	 */
	if (pg_fsync(fd) != 0)
	{
		CloseTransientFile(fd);
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m",
						path)));
	}

	/* Also sync the parent directory */
	START_CRIT_SECTION();
	fsync_fname(path, true);
	END_CRIT_SECTION();

	/* read part of statefile that's guaranteed to be version independent */
	readBytes = read(fd, &cp, ReplicationSlotOnDiskConstantSize);
	if (readBytes != ReplicationSlotOnDiskConstantSize)
	{
		int			saved_errno = errno;

		CloseTransientFile(fd);
		errno = saved_errno;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\", read %d of %u: %m",
						path, readBytes,
						(uint32) ReplicationSlotOnDiskConstantSize)));
	}

	/* verify magic */
	if (cp.magic != SLOT_MAGIC)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("replication slot file \"%s\" has wrong magic %u instead of %u",
						path, cp.magic, SLOT_MAGIC)));

	/* verify version */
	if (cp.version != SLOT_VERSION)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("replication slot file \"%s\" has unsupported version %u",
						path, cp.version)));

	/* boundary check on length */
	if (cp.length != ReplicationSlotOnDiskDynamicSize)
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("replication slot file \"%s\" has corrupted length %u",
						path, cp.length)));

	/* Now that we know the size, read the entire file */
	readBytes = read(fd,
					 (char *)&cp + ReplicationSlotOnDiskConstantSize,
					 cp.length);
	if (readBytes != cp.length)
	{
		int			saved_errno = errno;

		CloseTransientFile(fd);
		errno = saved_errno;
		ereport(PANIC,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\", read %d of %u: %m",
						path, readBytes, cp.length)));
	}

	CloseTransientFile(fd);

	/* now verify the CRC32 */
	INIT_CRC32(checksum);
	COMP_CRC32(checksum,
			   (char *)&cp + ReplicationSlotOnDiskConstantSize,
			   ReplicationSlotOnDiskDynamicSize);

	if (!EQ_CRC32(checksum, cp.checksum))
		ereport(PANIC,
				(errmsg("replication slot file %s: checksum mismatch, is %u, should be %u",
						path, checksum, cp.checksum)));

	/* nothing can be active yet, don't lock anything */
	for (i = 0; i < max_replication_slots; i++)
	{
		ReplicationSlot *slot;

		slot = &ReplicationSlotCtl->replication_slots[i];

		if (slot->in_use)
			continue;

		/* restore the entire set of persistent data */
		memcpy(&slot->data, &cp.slotdata,
			   sizeof(ReplicationSlotPersistentData));

		/* initialize in memory state */
		slot->effective_xmin = cp.slotdata.xmin;
		slot->in_use = true;
		slot->active = false;

		restored = true;
		break;
	}

	if (!restored)
		ereport(PANIC,
				(errmsg("too many replication slots active before shutdown"),
				 errhint("Increase max_replication_slots and try again.")));
}
