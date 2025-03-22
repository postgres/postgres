/*-------------------------------------------------------------------------
 *
 * aio_types.h
 *    AIO related types that are useful to include separately, to reduce the
 *    "include burden".
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/aio_types.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef AIO_TYPES_H
#define AIO_TYPES_H

#include "storage/block.h"
#include "storage/relfilelocator.h"


typedef struct PgAioHandle PgAioHandle;
typedef struct PgAioHandleCallbacks PgAioHandleCallbacks;
typedef struct PgAioTargetInfo PgAioTargetInfo;

/*
 * A reference to an IO that can be used to wait for the IO (using
 * pgaio_wref_wait()) to complete.
 *
 * These can be passed across process boundaries.
 */
typedef struct PgAioWaitRef
{
	/* internal ID identifying the specific PgAioHandle */
	uint32		aio_index;

	/*
	 * IO handles are reused. To detect if a handle was reused, and thereby
	 * avoid unnecessarily waiting for a newer IO, each time the handle is
	 * reused a generation number is increased.
	 *
	 * To avoid requiring alignment sufficient for an int64, split the
	 * generation into two.
	 */
	uint32		generation_upper;
	uint32		generation_lower;
} PgAioWaitRef;


/*
 * Information identifying what the IO is being performed on.
 *
 * This needs sufficient information to
 *
 * a) Reopen the file for the IO if the IO is executed in a context that
 *    cannot use the FD provided initially (e.g. because the IO is executed in
 *    a worker process).
 *
 * b) Describe the object the IO is performed on in log / error messages.
 */
typedef union PgAioTargetData
{
	/* just as an example placeholder for later */
	struct
	{
		uint32		queue_id;
	}			wal;
} PgAioTargetData;


/*
 * The status of an AIO operation.
 */
typedef enum PgAioResultStatus
{
	PGAIO_RS_UNKNOWN,			/* not yet completed / uninitialized */
	PGAIO_RS_OK,
	PGAIO_RS_PARTIAL,			/* did not fully succeed, but no error */
	PGAIO_RS_ERROR,
} PgAioResultStatus;


/*
 * Result of IO operation, visible only to the initiator of IO.
 */
typedef struct PgAioResult
{
	/*
	 * This is of type PgAioHandleCallbackID, but can't use a bitfield of an
	 * enum, because some compilers treat enums as signed.
	 */
	uint32		id:8;

	/* of type PgAioResultStatus, see above */
	uint32		status:2;

	/* meaning defined by callback->error */
	uint32		error_data:22;

	int32		result;
} PgAioResult;


/*
 * Combination of PgAioResult with minimal metadata about the IO.
 *
 * Contains sufficient information to be able, in case the IO [partially]
 * fails, to log/raise an error under control of the IO issuing code.
 */
typedef struct PgAioReturn
{
	PgAioResult result;
	PgAioTargetData target_data;
} PgAioReturn;


#endif							/* AIO_TYPES_H */
