/*-------------------------------------------------------------------------
 * pg_replslotdata.h
 *	   Replication slot data structures required for pg_replslotdata tool.
 *
 * Copyright (c) 2022, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REPLSLOTDATA_H
#define PG_REPLSLOTDATA_H

/*
 * NOTE: All of these structures are borrowed as-is from replication/slot.c and
 * replication/slot.h. Don't forget to keep both of them in sync.
 */

/*
 * Behaviour of replication slots, upon release or crash.
 *
 * Slots marked as PERSISTENT are crash-safe and will not be dropped when
 * released. Slots marked as EPHEMERAL will be dropped when released or after
 * restarts.  Slots marked TEMPORARY will be dropped at the end of a session
 * or on error.
 *
 * EPHEMERAL is used as a not-quite-ready state when creating persistent
 * slots.  EPHEMERAL slots can be made PERSISTENT by calling
 * ReplicationSlotPersist().  For a slot that goes away at the end of a
 * session, TEMPORARY is the appropriate choice.
 */
typedef enum ReplicationSlotPersistency
{
	RS_PERSISTENT,
	RS_EPHEMERAL,
	RS_TEMPORARY
} ReplicationSlotPersistency;

/*
 * On-Disk data of a replication slot, preserved across restarts.
 */
typedef struct ReplicationSlotPersistentData
{
	/* The slot's identifier */
	NameData	name;

	/* database the slot is active on */
	Oid			database;

	/*
	 * The slot's behaviour when being dropped (or restored after a crash).
	 */
	ReplicationSlotPersistency persistency;

	/*
	 * xmin horizon for data
	 *
	 * NB: This may represent a value that hasn't been written to disk yet;
	 * see notes for effective_xmin, below.
	 */
	TransactionId xmin;

	/*
	 * xmin horizon for catalog tuples
	 *
	 * NB: This may represent a value that hasn't been written to disk yet;
	 * see notes for effective_xmin, below.
	 */
	TransactionId catalog_xmin;

	/* oldest LSN that might be required by this replication slot */
	XLogRecPtr	restart_lsn;

	/* restart_lsn is copied here when the slot is invalidated */
	XLogRecPtr	invalidated_at;

	/*
	 * Oldest LSN that the client has acked receipt for.  This is used as the
	 * start_lsn point in case the client doesn't specify one, and also as a
	 * safety measure to jump forwards in case the client specifies a
	 * start_lsn that's further in the past than this value.
	 */
	XLogRecPtr	confirmed_flush;

	/*
	 * LSN at which we enabled two_phase commit for this slot or LSN at which
	 * we found a consistent point at the time of slot creation.
	 */
	XLogRecPtr	two_phase_at;

	/*
	 * Allow decoding of prepared transactions?
	 */
	bool		two_phase;

	/* plugin name */
	NameData	plugin;
} ReplicationSlotPersistentData;

/*
 * Replication slot on-disk data structure.
 */
typedef struct ReplicationSlotOnDisk
{
	/* first part of this struct needs to be version independent */

	/* data not covered by checksum */
	uint32		magic;
	pg_crc32c	checksum;

	/* data covered by checksum */
	uint32		version;
	uint32		length;

	/*
	 * The actual data in the slot that follows can differ based on the above
	 * 'version'.
	 */

	ReplicationSlotPersistentData slotdata;
} ReplicationSlotOnDisk;

/* size of version independent data */
#define ReplicationSlotOnDiskConstantSize \
	offsetof(ReplicationSlotOnDisk, slotdata)
/* size of the part of the slot not covered by the checksum */
#define ReplicationSlotOnDiskNotChecksummedSize \
	offsetof(ReplicationSlotOnDisk, version)
/* size of the part covered by the checksum */
#define ReplicationSlotOnDiskChecksummedSize \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskNotChecksummedSize
/* size of the slot data that is version dependent */
#define ReplicationSlotOnDiskV2Size \
	sizeof(ReplicationSlotOnDisk) - ReplicationSlotOnDiskConstantSize

#define SLOT_MAGIC		0x1051CA1	/* format identifier */
#define SLOT_VERSION	2		/* version for new files */

#endif							/* PG_REPLSLOTDATA_H */
