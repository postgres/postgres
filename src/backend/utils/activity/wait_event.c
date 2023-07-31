/* ----------
 * wait_event.c
 *	  Wait event reporting infrastructure.
 *
 * Copyright (c) 2001-2023, PostgreSQL Global Development Group
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

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/lmgr.h"		/* for GetLockNameFromTagType */
#include "storage/lwlock.h"		/* for GetLWLockIdentifier */
#include "storage/spin.h"
#include "utils/memutils.h"
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

/* dynamic allocation counter for custom wait events in extensions */
typedef struct WaitEventExtensionCounterData
{
	int			nextId;			/* next ID to assign */
	slock_t		mutex;			/* protects the counter */
} WaitEventExtensionCounterData;

/* pointer to the shared memory */
static WaitEventExtensionCounterData *WaitEventExtensionCounter;

/* first event ID of custom wait events for extensions */
#define NUM_BUILTIN_WAIT_EVENT_EXTENSION	\
	(WAIT_EVENT_EXTENSION_FIRST_USER_DEFINED - WAIT_EVENT_EXTENSION)

/*
 * This is indexed by event ID minus NUM_BUILTIN_WAIT_EVENT_EXTENSION, and
 * stores the names of all dynamically-created event IDs known to the current
 * process.  Any unused entries in the array will contain NULL.
 */
static const char **WaitEventExtensionNames = NULL;
static int	WaitEventExtensionNamesAllocated = 0;

static const char *GetWaitEventExtensionIdentifier(uint16 eventId);

/*
 *  Return the space for dynamic allocation counter.
 */
Size
WaitEventExtensionShmemSize(void)
{
	return sizeof(WaitEventExtensionCounterData);
}

/*
 * Allocate shmem space for dynamic allocation counter.
 */
void
WaitEventExtensionShmemInit(void)
{
	bool		found;

	WaitEventExtensionCounter = (WaitEventExtensionCounterData *)
		ShmemInitStruct("WaitEventExtensionCounterData",
						WaitEventExtensionShmemSize(), &found);

	if (!found)
	{
		/* initialize the allocation counter and its spinlock. */
		WaitEventExtensionCounter->nextId = NUM_BUILTIN_WAIT_EVENT_EXTENSION;
		SpinLockInit(&WaitEventExtensionCounter->mutex);
	}
}

/*
 * Allocate a new event ID and return the wait event.
 */
uint32
WaitEventExtensionNew(void)
{
	uint16		eventId;

	Assert(LWLockHeldByMeInMode(AddinShmemInitLock, LW_EXCLUSIVE));

	SpinLockAcquire(&WaitEventExtensionCounter->mutex);

	if (WaitEventExtensionCounter->nextId > PG_UINT16_MAX)
	{
		SpinLockRelease(&WaitEventExtensionCounter->mutex);
		ereport(ERROR,
				errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				errmsg("too many wait events for extensions"));
	}

	eventId = WaitEventExtensionCounter->nextId++;

	SpinLockRelease(&WaitEventExtensionCounter->mutex);

	return PG_WAIT_EXTENSION | eventId;
}

/*
 * Register a dynamic wait event name for extension in the lookup table
 * of the current process.
 *
 * This routine will save a pointer to the wait event name passed as an argument,
 * so the name should be allocated in a backend-lifetime context
 * (shared memory, TopMemoryContext, static constant, or similar).
 *
 * The "wait_event_name" will be user-visible as a wait event name, so try to
 * use a name that fits the style for those.
 */
void
WaitEventExtensionRegisterName(uint32 wait_event_info,
							   const char *wait_event_name)
{
	uint32		classId;
	uint16		eventId;

	classId = wait_event_info & WAIT_EVENT_CLASS_MASK;
	eventId = wait_event_info & WAIT_EVENT_ID_MASK;

	/* Check the wait event class. */
	if (classId != PG_WAIT_EXTENSION)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("invalid wait event class %u", classId));

	/* This should only be called for user-defined wait event. */
	if (eventId < NUM_BUILTIN_WAIT_EVENT_EXTENSION)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("invalid wait event ID %u", eventId));

	/* Convert to array index. */
	eventId -= NUM_BUILTIN_WAIT_EVENT_EXTENSION;

	/* If necessary, create or enlarge array. */
	if (eventId >= WaitEventExtensionNamesAllocated)
	{
		uint32		newalloc;

		newalloc = pg_nextpower2_32(Max(8, eventId + 1));

		if (WaitEventExtensionNames == NULL)
			WaitEventExtensionNames = (const char **)
				MemoryContextAllocZero(TopMemoryContext,
									   newalloc * sizeof(char *));
		else
			WaitEventExtensionNames =
				repalloc0_array(WaitEventExtensionNames, const char *,
								WaitEventExtensionNamesAllocated, newalloc);
		WaitEventExtensionNamesAllocated = newalloc;
	}

	WaitEventExtensionNames[eventId] = wait_event_name;
}

/*
 * Return the name of an wait event ID for extension.
 */
static const char *
GetWaitEventExtensionIdentifier(uint16 eventId)
{
	/* Built-in event? */
	if (eventId < NUM_BUILTIN_WAIT_EVENT_EXTENSION)
		return "Extension";

	/*
	 * It is a user-defined wait event, so look at WaitEventExtensionNames[].
	 * However, it is possible that the name has never been registered by
	 * calling WaitEventExtensionRegisterName() in the current process, in
	 * which case give up and return "extension".
	 */
	eventId -= NUM_BUILTIN_WAIT_EVENT_EXTENSION;

	if (eventId >= WaitEventExtensionNamesAllocated ||
		WaitEventExtensionNames[eventId] == NULL)
		return "extension";

	return WaitEventExtensionNames[eventId];
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
