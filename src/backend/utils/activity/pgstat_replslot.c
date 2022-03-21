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


/* ----------
 * pgstat_reset_replslot_counter() -
 *
 *	Tell the statistics collector to reset a single replication slot
 *	counter, or all replication slots counters (when name is null).
 *
 *	Permission checking for this function is managed through the normal
 *	GRANT system.
 * ----------
 */
void
pgstat_reset_replslot_counter(const char *name)
{
	PgStat_MsgResetreplslotcounter msg;

	if (pgStatSock == PGINVALID_SOCKET)
		return;

	if (name)
	{
		namestrcpy(&msg.m_slotname, name);
		msg.clearall = false;
	}
	else
		msg.clearall = true;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_RESETREPLSLOTCOUNTER);

	pgstat_send(&msg, sizeof(msg));
}

/* ----------
 * pgstat_report_replslot() -
 *
 *	Tell the collector about replication slot statistics.
 * ----------
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

/* ----------
 * pgstat_report_replslot_create() -
 *
 *	Tell the collector about creating the replication slot.
 * ----------
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

/* ----------
 * pgstat_report_replslot_drop() -
 *
 *	Tell the collector about dropping the replication slot.
 * ----------
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
