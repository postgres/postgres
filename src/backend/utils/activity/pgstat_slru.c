/* -------------------------------------------------------------------------
 *
 * pgstat_slru.c
 *	  Implementation of SLRU statistics.
 *
 * This file contains the implementation of SLRU statistics. It is kept
 * separate from pgstat.c to enforce the line between the statistics access /
 * storage implementation and the details about individual types of
 * statistics.
 *
 * Copyright (c) 2001-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/activity/pgstat_slru.c
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/pgstat_internal.h"


static inline PgStat_MsgSLRU *slru_entry(int slru_idx);


/*
 * SLRU statistics counts waiting to be sent to the collector.  These are
 * stored directly in stats message format so they can be sent without needing
 * to copy things around.  We assume this variable inits to zeroes.  Entries
 * are one-to-one with slru_names[].
 */
static PgStat_MsgSLRU SLRUStats[SLRU_NUM_ELEMENTS];


/*
 * Tell the statistics collector to reset a single SLRU counter, or all
 * SLRU counters (when name is null).
 *
 * Permission checking for this function is managed through the normal
 * GRANT system.
 */
void
pgstat_reset_slru_counter(const char *name)
{
	PgStat_MsgResetslrucounter msg;

	if (pgStatSock == PGINVALID_SOCKET)
		return;

	pgstat_setheader(&msg.m_hdr, PGSTAT_MTYPE_RESETSLRUCOUNTER);
	msg.m_index = (name) ? pgstat_slru_index(name) : -1;

	pgstat_send(&msg, sizeof(msg));
}

/*
 * SLRU statistics count accumulation functions --- called from slru.c
 */

void
pgstat_count_slru_page_zeroed(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_zeroed += 1;
}

void
pgstat_count_slru_page_hit(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_hit += 1;
}

void
pgstat_count_slru_page_exists(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_exists += 1;
}

void
pgstat_count_slru_page_read(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_read += 1;
}

void
pgstat_count_slru_page_written(int slru_idx)
{
	slru_entry(slru_idx)->m_blocks_written += 1;
}

void
pgstat_count_slru_flush(int slru_idx)
{
	slru_entry(slru_idx)->m_flush += 1;
}

void
pgstat_count_slru_truncate(int slru_idx)
{
	slru_entry(slru_idx)->m_truncate += 1;
}

/*
 * Returns SLRU name for an index. The index may be above SLRU_NUM_ELEMENTS,
 * in which case this returns NULL. This allows writing code that does not
 * know the number of entries in advance.
 */
const char *
pgstat_slru_name(int slru_idx)
{
	if (slru_idx < 0 || slru_idx >= SLRU_NUM_ELEMENTS)
		return NULL;

	return slru_names[slru_idx];
}

/*
 * Determine index of entry for a SLRU with a given name. If there's no exact
 * match, returns index of the last "other" entry used for SLRUs defined in
 * external projects.
 */
int
pgstat_slru_index(const char *name)
{
	int			i;

	for (i = 0; i < SLRU_NUM_ELEMENTS; i++)
	{
		if (strcmp(slru_names[i], name) == 0)
			return i;
	}

	/* return index of the last entry (which is the "other" one) */
	return (SLRU_NUM_ELEMENTS - 1);
}

/*
 * Send SLRU statistics to the collector
 */
void
pgstat_send_slru(void)
{
	/* We assume this initializes to zeroes */
	static const PgStat_MsgSLRU all_zeroes;

	for (int i = 0; i < SLRU_NUM_ELEMENTS; i++)
	{
		/*
		 * This function can be called even if nothing at all has happened. In
		 * this case, avoid sending a completely empty message to the stats
		 * collector.
		 */
		if (memcmp(&SLRUStats[i], &all_zeroes, sizeof(PgStat_MsgSLRU)) == 0)
			continue;

		/* set the SLRU type before each send */
		SLRUStats[i].m_index = i;

		/*
		 * Prepare and send the message
		 */
		pgstat_setheader(&SLRUStats[i].m_hdr, PGSTAT_MTYPE_SLRU);
		pgstat_send(&SLRUStats[i], sizeof(PgStat_MsgSLRU));

		/*
		 * Clear out the statistics buffer, so it can be re-used.
		 */
		MemSet(&SLRUStats[i], 0, sizeof(PgStat_MsgSLRU));
	}
}

/*
 * Returns pointer to entry with counters for given SLRU (based on the name
 * stored in SlruCtl as lwlock tranche name).
 */
static inline PgStat_MsgSLRU *
slru_entry(int slru_idx)
{
	pgstat_assert_is_up();

	/*
	 * The postmaster should never register any SLRU statistics counts; if it
	 * did, the counts would be duplicated into child processes via fork().
	 */
	Assert(IsUnderPostmaster || !IsPostmasterEnvironment);

	Assert((slru_idx >= 0) && (slru_idx < SLRU_NUM_ELEMENTS));

	return &SLRUStats[slru_idx];
}
