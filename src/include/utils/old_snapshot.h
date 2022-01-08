/*-------------------------------------------------------------------------
 *
 * old_snapshot.h
 *		Data structures for 'snapshot too old'
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/utils/old_snapshot.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef OLD_SNAPSHOT_H
#define OLD_SNAPSHOT_H

#include "datatype/timestamp.h"
#include "storage/s_lock.h"

/*
 * Structure for dealing with old_snapshot_threshold implementation.
 */
typedef struct OldSnapshotControlData
{
	/*
	 * Variables for old snapshot handling are shared among processes and are
	 * only allowed to move forward.
	 */
	slock_t		mutex_current;	/* protect current_timestamp */
	TimestampTz current_timestamp;	/* latest snapshot timestamp */
	slock_t		mutex_latest_xmin;	/* protect latest_xmin and next_map_update */
	TransactionId latest_xmin;	/* latest snapshot xmin */
	TimestampTz next_map_update;	/* latest snapshot valid up to */
	slock_t		mutex_threshold;	/* protect threshold fields */
	TimestampTz threshold_timestamp;	/* earlier snapshot is old */
	TransactionId threshold_xid;	/* earlier xid may be gone */

	/*
	 * Keep one xid per minute for old snapshot error handling.
	 *
	 * Use a circular buffer with a head offset, a count of entries currently
	 * used, and a timestamp corresponding to the xid at the head offset.  A
	 * count_used value of zero means that there are no times stored; a
	 * count_used value of OLD_SNAPSHOT_TIME_MAP_ENTRIES means that the buffer
	 * is full and the head must be advanced to add new entries.  Use
	 * timestamps aligned to minute boundaries, since that seems less
	 * surprising than aligning based on the first usage timestamp.  The
	 * latest bucket is effectively stored within latest_xmin.  The circular
	 * buffer is updated when we get a new xmin value that doesn't fall into
	 * the same interval.
	 *
	 * It is OK if the xid for a given time slot is from earlier than
	 * calculated by adding the number of minutes corresponding to the
	 * (possibly wrapped) distance from the head offset to the time of the
	 * head entry, since that just results in the vacuuming of old tuples
	 * being slightly less aggressive.  It would not be OK for it to be off in
	 * the other direction, since it might result in vacuuming tuples that are
	 * still expected to be there.
	 *
	 * Use of an SLRU was considered but not chosen because it is more
	 * heavyweight than is needed for this, and would probably not be any less
	 * code to implement.
	 *
	 * Persistence is not needed.
	 */
	int			head_offset;	/* subscript of oldest tracked time */
	TimestampTz head_timestamp; /* time corresponding to head xid */
	int			count_used;		/* how many slots are in use */
	TransactionId xid_by_minute[FLEXIBLE_ARRAY_MEMBER];
} OldSnapshotControlData;

extern PGDLLIMPORT volatile OldSnapshotControlData *oldSnapshotControl;

#endif
