/* -------------------------------------------------------------------------
 *
 * pgstat_replslot.c
 *	  Implementation of replication slot statistics.
 *
 * This file contains the implementation of replication slot statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_replslot.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "replication/slot.h"
#include "utils/builtins.h"		/* for namestrcpy() */
#include "utils/pgstat_internal.h"


/*
 * Reset counters for a single replication slot.
 *
 * Permission checking for this function is managed through the normal
 * GRANT system.
 */
void
pgstat_reset_replslot(const char *name)
{
	ReplicationSlot *slot;
	PgStat_MsgResetreplslotcounter msg;

	AssertArg(name != NULL);

	if (pgStatSock == PGINVALID_SOCKET)
		return;

	/*
	 * Check if the slot exists with the given name. It is possible that by
	 * the time this message is executed the slot is dropped but at least this
	 * check will ensure that the given name is for a valid slot.
	 */
	slot = SearchNamedReplicationSlot(name, true);

	if (!slot)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("replication slot \"%s\" does not exist",
						name)));

	/*
	 * Nothing to do for physical slots as we collect stats only for logical
	 * slots.
	 */
	if (SlotIsPhysical(slot))
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_RESETREPLSLOTCOUNTER);
	namestrcpy(&msg.m_slotname, name);
	msg.clearall = false;
	pgstat_send(&msg, sizeof(msg));
}

/*
 * Report replication slot statistics.
 */
void
pgstat_report_replslot(const PgStat_StatReplSlotEntry *repSlotStat)
{
	PgStat_MsgReplSlot msg;

	/*
	 * Prepare and send the message
	 */
	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_REPLSLOT);
	namestrcpy(&msg.m_slotname, NameStr(repSlotStat->slotname));
	msg.m_create = false;
	msg.m_drop = false;
	msg.m_spill_txns = repSlotStat->spill_txns;
	msg.m_spill_count = repSlotStat->spill_count;
	msg.m_spill_bytes = repSlotStat->spill_bytes;
	msg.m_stream_txns = repSlotStat->stream_txns;
	msg.m_stream_count = repSlotStat->stream_count;
	msg.m_stream_bytes = repSlotStat->stream_bytes;
	msg.m_total_txns = repSlotStat->total_txns;
	msg.m_total_bytes = repSlotStat->total_bytes;
	pgstat_send(&msg, sizeof(PgStat_MsgReplSlot));
}

/*
 * Report replication slot creation.
 */
void
pgstat_report_replslot_create(const char *slotname)
{
	PgStat_MsgReplSlot msg;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_REPLSLOT);
	namestrcpy(&msg.m_slotname, slotname);
	msg.m_create = true;
	msg.m_drop = false;
	pgstat_send(&msg, sizeof(PgStat_MsgReplSlot));
}

/*
 * Report replication slot drop.
 */
void
pgstat_report_replslot_drop(const char *slotname)
{
	PgStat_MsgReplSlot msg;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_REPLSLOT);
	namestrcpy(&msg.m_slotname, slotname);
	msg.m_create = false;
	msg.m_drop = true;
	pgstat_send(&msg, sizeof(PgStat_MsgReplSlot));
}
