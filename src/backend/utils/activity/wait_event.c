/* ----------
 * wait_event.c
 *	  Wait event reporting infrastructure.
 *
 * Copyright (c) 2001-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/wait_event.c
 *
 * NOTES
 *
 * To make pgstat_report_wait_start() and pgstat_report_wait_end() as
 * lightweight as possible, they do not check if shared memory (MyProc
 * specifically, where the wait event is stored) is already available. Instead
 * we initially set my_wait_event_info to a process local variable, which then
 * is redirected to shared memory using pgstat_set_wait_event_storage(). For
 * the same reason pgstat_track_activities is not checked - the check adds
 * more work than it saves.
 *
 * ----------
 */
#include "postgres.h"

#include "port/pg_bitutils.h"
#include "storage/lmgr.h"		/* for GetLockNameFromTagType */
#include "storage/lwlock.h"		/* for GetLWLockIdentifier */
#include "storage/spin.h"
#include "utils/wait_event.h"


static const char *pgstat_get_wait_activity(WaitEventActivity w);
static const char *pgstat_get_wait_bufferpin(WaitEventBufferPin w);
static const char *pgstat_get_wait_client(WaitEventClient w);
static const char *pgstat_get_wait_ipc(WaitEventIPC w);
static const char *pgstat_get_wait_timeout(WaitEventTimeout w);
static const char *pgstat_get_wait_io(WaitEventIO w);


static uint32 local_my_wait_event_info;
uint32	   *my_wait_event_info = &local_my_wait_event_info;

#define WAIT_EVENT_CLASS_MASK	0xFF000000
#define WAIT_EVENT_ID_MASK		0x0000FFFF

/*
 * Hash tables for storing custom wait event ids and their names in
 * shared memory.
 *
 * WaitEventExtensionHashById is used to find the name from an event id.
 * Any backend can search it to find custom wait events.
 *
 * WaitEventExtensionHashByName is used to find the event ID from a name.
 * It is used to ensure that no duplicated entries are registered.
 *
 * The size of the hash table is based on the assumption that
 * WAIT_EVENT_EXTENSION_HASH_INIT_SIZE is enough for most cases, and it seems
 * unlikely that the number of entries will reach
 * WAIT_EVENT_EXTENSION_HASH_MAX_SIZE.
 */
static HTAB *WaitEventExtensionHashById;	/* find names from IDs */
static HTAB *WaitEventExtensionHashByName;	/* find IDs from names */

#define WAIT_EVENT_EXTENSION_HASH_INIT_SIZE	16
#define WAIT_EVENT_EXTENSION_HASH_MAX_SIZE	128

/* hash table entries */
typedef struct WaitEventExtensionEntryById
{
	uint16		event_id;		/* hash key */
	char		wait_event_name[NAMEDATALEN];	/* custom wait event name */
} WaitEventExtensionEntryById;

typedef struct WaitEventExtensionEntryByName
{
	char		wait_event_name[NAMEDATALEN];	/* hash key */
	uint16		event_id;		/* wait event ID */
} WaitEventExtensionEntryByName;


/* dynamic allocation counter for custom wait events in extensions */
typedef struct WaitEventExtensionCounterData
{
	int			nextId;			/* next ID to assign */
	slock_t		mutex;			/* protects the counter */
} WaitEventExtensionCounterData;

/* pointer to the shared memory */
static WaitEventExtensionCounterData *WaitEventExtensionCounter;

/* first event ID of custom wait events for extensions */
#define WAIT_EVENT_EXTENSION_INITIAL_ID	1

/* wait event info for extensions */
#define WAIT_EVENT_EXTENSION_INFO(eventId)	(PG_WAIT_EXTENSION | eventId)

static const char *GetWaitEventExtensionIdentifier(uint16 eventId);

/*
 *  Return the space for dynamic shared hash tables and dynamic allocation counter.
 */
Size
WaitEventExtensionShmemSize(void)
{
	Size		sz;

	sz = MAXALIGN(sizeof(WaitEventExtensionCounterData));
	sz = add_size(sz, hash_estimate_size(WAIT_EVENT_EXTENSION_HASH_MAX_SIZE,
										 sizeof(WaitEventExtensionEntryById)));
	sz = add_size(sz, hash_estimate_size(WAIT_EVENT_EXTENSION_HASH_MAX_SIZE,
										 sizeof(WaitEventExtensionEntryByName)));
	return sz;
}

/*
 * Allocate shmem space for dynamic shared hash and dynamic allocation counter.
 */
void
WaitEventExtensionShmemInit(void)
{
	bool		found;
	HASHCTL		info;

	WaitEventExtensionCounter = (WaitEventExtensionCounterData *)
		ShmemInitStruct("WaitEventExtensionCounterData",
						sizeof(WaitEventExtensionCounterData), &found);

	if (!found)
	{
		/* initialize the allocation counter and its spinlock. */
		WaitEventExtensionCounter->nextId = WAIT_EVENT_EXTENSION_INITIAL_ID;
		SpinLockInit(&WaitEventExtensionCounter->mutex);
	}

	/* initialize or attach the hash tables to store custom wait events */
	info.keysize = sizeof(uint16);
	info.entrysize = sizeof(WaitEventExtensionEntryById);
	WaitEventExtensionHashById = ShmemInitHash("WaitEventExtension hash by id",
											   WAIT_EVENT_EXTENSION_HASH_INIT_SIZE,
											   WAIT_EVENT_EXTENSION_HASH_MAX_SIZE,
											   &info,
											   HASH_ELEM | HASH_BLOBS);

	/* key is a NULL-terminated string */
	info.keysize = sizeof(char[NAMEDATALEN]);
	info.entrysize = sizeof(WaitEventExtensionEntryByName);
	WaitEventExtensionHashByName = ShmemInitHash("WaitEventExtension hash by name",
												 WAIT_EVENT_EXTENSION_HASH_INIT_SIZE,
												 WAIT_EVENT_EXTENSION_HASH_MAX_SIZE,
												 &info,
												 HASH_ELEM | HASH_STRINGS);
}

/*
 * Allocate a new event ID and return the wait event info.
 *
 * If the wait event name is already defined, this does not allocate a new
 * entry; it returns the wait event information associated to the name.
 */
uint32
WaitEventExtensionNew(const char *wait_event_name)
{
	uint16		eventId;
	bool		found;
	WaitEventExtensionEntryByName *entry_by_name;
	WaitEventExtensionEntryById *entry_by_id;

	/* Check the limit of the length of the event name */
	if (strlen(wait_event_name) >= NAMEDATALEN)
		elog(ERROR,
			 "cannot use custom wait event string longer than %u characters",
			 NAMEDATALEN - 1);

	/*
	 * Check if the wait event info associated to the name is already defined,
	 * and return it if so.
	 */
	LWLockAcquire(WaitEventExtensionLock, LW_SHARED);
	entry_by_name = (WaitEventExtensionEntryByName *)
		hash_search(WaitEventExtensionHashByName, wait_event_name,
					HASH_FIND, &found);
	LWLockRelease(WaitEventExtensionLock);
	if (found)
		return WAIT_EVENT_EXTENSION_INFO(entry_by_name->event_id);

	/*
	 * Allocate and register a new wait event.  Recheck if the event name
	 * exists, as it could be possible that a concurrent process has inserted
	 * one with the same name since the LWLock acquired again here was
	 * previously released.
	 */
	LWLockAcquire(WaitEventExtensionLock, LW_EXCLUSIVE);
	entry_by_name = (WaitEventExtensionEntryByName *)
		hash_search(WaitEventExtensionHashByName, wait_event_name,
					HASH_FIND, &found);
	if (found)
	{
		LWLockRelease(WaitEventExtensionLock);
		return WAIT_EVENT_EXTENSION_INFO(entry_by_name->event_id);
	}

	/* Allocate a new event Id */
	SpinLockAcquire(&WaitEventExtensionCounter->mutex);

	if (WaitEventExtensionCounter->nextId >= WAIT_EVENT_EXTENSION_HASH_MAX_SIZE)
	{
		SpinLockRelease(&WaitEventExtensionCounter->mutex);
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("too many wait events for extensions"));
	}

	eventId = WaitEventExtensionCounter->nextId++;

	SpinLockRelease(&WaitEventExtensionCounter->mutex);

	/* Register the new wait event */
	entry_by_id = (WaitEventExtensionEntryById *)
		hash_search(WaitEventExtensionHashById, &eventId,
					HASH_ENTER, &found);
	Assert(!found);
	strlcpy(entry_by_id->wait_event_name, wait_event_name,
			sizeof(entry_by_id->wait_event_name));

	entry_by_name = (WaitEventExtensionEntryByName *)
		hash_search(WaitEventExtensionHashByName, wait_event_name,
					HASH_ENTER, &found);
	Assert(!found);
	entry_by_name->event_id = eventId;

	LWLockRelease(WaitEventExtensionLock);

	return WAIT_EVENT_EXTENSION_INFO(eventId);
}

/*
 * Return the name of an wait event ID for extension.
 */
static const char *
GetWaitEventExtensionIdentifier(uint16 eventId)
{
	bool		found;
	WaitEventExtensionEntryById *entry;

	/* Built-in event? */
	if (eventId < WAIT_EVENT_EXTENSION_INITIAL_ID)
		return "Extension";

	/* It is a user-defined wait event, so lookup hash table. */
	LWLockAcquire(WaitEventExtensionLock, LW_SHARED);
	entry = (WaitEventExtensionEntryById *)
		hash_search(WaitEventExtensionHashById, &eventId,
					HASH_FIND, &found);
	LWLockRelease(WaitEventExtensionLock);

	if (!entry)
		elog(ERROR, "could not find custom wait event name for ID %u",
			 eventId);

	return entry->wait_event_name;
}


/*
 * Returns a list of currently defined custom wait event names for extensions.
 * The result is a palloc'd array, with the number of elements saved in
 * *nwaitevents.
 */
char	  **
GetWaitEventExtensionNames(int *nwaitevents)
{
	char	  **waiteventnames;
	WaitEventExtensionEntryByName *hentry;
	HASH_SEQ_STATUS hash_seq;
	int			index;
	int			els;

	LWLockAcquire(WaitEventExtensionLock, LW_SHARED);

	/* Now we can safely count the number of entries */
	els = hash_get_num_entries(WaitEventExtensionHashByName);

	/* Allocate enough space for all entries */
	waiteventnames = palloc(els * sizeof(char *));

	/* Now scan the hash table to copy the data */
	hash_seq_init(&hash_seq, WaitEventExtensionHashByName);

	index = 0;
	while ((hentry = (WaitEventExtensionEntryByName *) hash_seq_search(&hash_seq)) != NULL)
	{
		waiteventnames[index] = pstrdup(hentry->wait_event_name);
		index++;
	}

	LWLockRelease(WaitEventExtensionLock);

	Assert(index == els);

	*nwaitevents = index;
	return waiteventnames;
}

/*
 * Configure wait event reporting to report wait events to *wait_event_info.
 * *wait_event_info needs to be valid until pgstat_reset_wait_event_storage()
 * is called.
 *
 * Expected to be called during backend startup, to point my_wait_event_info
 * into shared memory.
 */
void
pgstat_set_wait_event_storage(uint32 *wait_event_info)
{
	my_wait_event_info = wait_event_info;
}

/*
 * Reset wait event storage location.
 *
 * Expected to be called during backend shutdown, before the location set up
 * pgstat_set_wait_event_storage() becomes invalid.
 */
void
pgstat_reset_wait_event_storage(void)
{
	my_wait_event_info = &local_my_wait_event_info;
}

/* ----------
 * pgstat_get_wait_event_type() -
 *
 *	Return a string representing the current wait event type, backend is
 *	waiting on.
 */
const char *
pgstat_get_wait_event_type(uint32 wait_event_info)
{
	uint32		classId;
	const char *event_type;

	/* report process as not waiting. */
	if (wait_event_info == 0)
		return NULL;

	classId = wait_event_info & WAIT_EVENT_CLASS_MASK;

	switch (classId)
	{
		case PG_WAIT_LWLOCK:
			event_type = "LWLock";
			break;
		case PG_WAIT_LOCK:
			event_type = "Lock";
			break;
		case PG_WAIT_BUFFERPIN:
			event_type = "BufferPin";
			break;
		case PG_WAIT_ACTIVITY:
			event_type = "Activity";
			break;
		case PG_WAIT_CLIENT:
			event_type = "Client";
			break;
		case PG_WAIT_EXTENSION:
			event_type = "Extension";
			break;
		case PG_WAIT_IPC:
			event_type = "IPC";
			break;
		case PG_WAIT_TIMEOUT:
			event_type = "Timeout";
			break;
		case PG_WAIT_IO:
			event_type = "IO";
			break;
		default:
			event_type = "???";
			break;
	}

	return event_type;
}

/* ----------
 * pgstat_get_wait_event() -
 *
 *	Return a string representing the current wait event, backend is
 *	waiting on.
 */
const char *
pgstat_get_wait_event(uint32 wait_event_info)
{
	uint32		classId;
	uint16		eventId;
	const char *event_name;

	/* report process as not waiting. */
	if (wait_event_info == 0)
		return NULL;

	classId = wait_event_info & WAIT_EVENT_CLASS_MASK;
	eventId = wait_event_info & WAIT_EVENT_ID_MASK;

	switch (classId)
	{
		case PG_WAIT_LWLOCK:
			event_name = GetLWLockIdentifier(classId, eventId);
			break;
		case PG_WAIT_LOCK:
			event_name = GetLockNameFromTagType(eventId);
			break;
		case PG_WAIT_EXTENSION:
			event_name = GetWaitEventExtensionIdentifier(eventId);
			break;
		case PG_WAIT_BUFFERPIN:
			{
				WaitEventBufferPin w = (WaitEventBufferPin) wait_event_info;

				event_name = pgstat_get_wait_bufferpin(w);
				break;
			}
		case PG_WAIT_ACTIVITY:
			{
				WaitEventActivity w = (WaitEventActivity) wait_event_info;

				event_name = pgstat_get_wait_activity(w);
				break;
			}
		case PG_WAIT_CLIENT:
			{
				WaitEventClient w = (WaitEventClient) wait_event_info;

				event_name = pgstat_get_wait_client(w);
				break;
			}
		case PG_WAIT_IPC:
			{
				WaitEventIPC w = (WaitEventIPC) wait_event_info;

				event_name = pgstat_get_wait_ipc(w);
				break;
			}
		case PG_WAIT_TIMEOUT:
			{
				WaitEventTimeout w = (WaitEventTimeout) wait_event_info;

				event_name = pgstat_get_wait_timeout(w);
				break;
			}
		case PG_WAIT_IO:
			{
				WaitEventIO w = (WaitEventIO) wait_event_info;

				event_name = pgstat_get_wait_io(w);
				break;
			}
		default:
			event_name = "unknown wait event";
			break;
	}

	return event_name;
}

#include "pgstat_wait_event.c"
