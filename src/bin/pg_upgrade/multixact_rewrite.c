/*
 * multixact_rewrite.c
 *
 * Functions to convert multixact SLRUs from the pre-v19 format to the current
 * format with 64-bit MultiXactOffsets.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_rewrite.c
 */

#include "postgres_fe.h"

#include "access/multixact_internal.h"
#include "multixact_read_v18.h"
#include "pg_upgrade.h"

static void RecordMultiXactOffset(SlruSegState *offsets_writer, MultiXactId multi,
								  MultiXactOffset offset);
static void RecordMultiXactMembers(SlruSegState *members_writer,
								   MultiXactOffset offset,
								   int nmembers, MultiXactMember *members);

/*
 * Convert pg_multixact/offset and /members from the old pre-v19 format with
 * 32-bit offsets to the current format.
 *
 * Multixids in the range [from_multi, to_multi) are read from the old
 * cluster, and written in the new format.  An important edge case is that if
 * from_multi == to_multi, this initializes the new pg_multixact files in the
 * new format without trying to open any old files.  (We rely on that when
 * upgrading from PostgreSQL version 9.2 or below.)
 *
 * Returns the new nextOffset value; the caller should set it in the new
 * control file.  The new members always start from offset 1, regardless of
 * the offset range used in the old cluster.
 */
MultiXactOffset
rewrite_multixacts(MultiXactId from_multi, MultiXactId to_multi)
{
	MultiXactOffset next_offset;
	SlruSegState *offsets_writer;
	SlruSegState *members_writer;
	char		dir[MAXPGPATH] = {0};
	bool		prev_multixid_valid = false;

	/*
	 * The range of valid multi XIDs is unchanged by the conversion (they are
	 * referenced from the heap tables), but the members SLRU is rewritten to
	 * start from offset 1.
	 */
	next_offset = 1;

	/* Prepare to write the new SLRU files */
	pg_sprintf(dir, "%s/pg_multixact/offsets", new_cluster.pgdata);
	offsets_writer = AllocSlruWrite(dir, false);
	SlruWriteSwitchPage(offsets_writer, MultiXactIdToOffsetPage(from_multi));

	pg_sprintf(dir, "%s/pg_multixact/members", new_cluster.pgdata);
	members_writer = AllocSlruWrite(dir, true /* use long segment names */ );
	SlruWriteSwitchPage(members_writer, MXOffsetToMemberPage(next_offset));

	/*
	 * Convert old multixids, if needed, by reading them one-by-one from the
	 * old cluster.
	 */
	if (to_multi != from_multi)
	{
		OldMultiXactReader *old_reader;

		old_reader = AllocOldMultiXactRead(old_cluster.pgdata,
										   old_cluster.controldata.chkpnt_nxtmulti,
										   old_cluster.controldata.chkpnt_nxtmxoff);

		for (MultiXactId multi = from_multi; multi != to_multi;)
		{
			MultiXactMember member;
			bool		multixid_valid;

			/*
			 * Read this multixid's members.
			 *
			 * Locking-only XIDs that may be part of multi-xids don't matter
			 * after upgrade, as there can be no transactions running across
			 * upgrade.  So as a small optimization, we only read one member
			 * from each multixid: the one updating one, or if there was no
			 * update, arbitrarily the first locking xid.
			 */
			multixid_valid = GetOldMultiXactIdSingleMember(old_reader, multi, &member);

			/*
			 * Write the new offset to pg_multixact/offsets.
			 *
			 * Even if this multixid is invalid, we still need to write its
			 * offset if the *previous* multixid was valid.  That's because
			 * when reading a multixid, the number of members is calculated
			 * from the difference between the two offsets.
			 */
			RecordMultiXactOffset(offsets_writer, multi,
								  (multixid_valid || prev_multixid_valid) ? next_offset : 0);

			/* Write the members */
			if (multixid_valid)
			{
				RecordMultiXactMembers(members_writer, next_offset, 1, &member);
				next_offset += 1;
			}

			/* Advance to next multixid, handling wraparound */
			multi++;
			if (multi < FirstMultiXactId)
				multi = FirstMultiXactId;
			prev_multixid_valid = multixid_valid;
		}

		FreeOldMultiXactReader(old_reader);
	}

	/* Write the final 'next' offset to the last SLRU page */
	RecordMultiXactOffset(offsets_writer, to_multi,
						  prev_multixid_valid ? next_offset : 0);

	/* Flush the last SLRU pages */
	FreeSlruWrite(offsets_writer);
	FreeSlruWrite(members_writer);

	return next_offset;
}


/*
 * Write one offset to the offset SLRU
 */
static void
RecordMultiXactOffset(SlruSegState *offsets_writer, MultiXactId multi,
					  MultiXactOffset offset)
{
	int64		pageno;
	int			entryno;
	char	   *buf;
	MultiXactOffset *offptr;

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	buf = SlruWriteSwitchPage(offsets_writer, pageno);
	offptr = (MultiXactOffset *) buf;
	offptr[entryno] = offset;
}

/*
 * Write the members for one multixid in the members SLRU
 *
 * (Currently, this is only ever called with nmembers == 1)
 */
static void
RecordMultiXactMembers(SlruSegState *members_writer,
					   MultiXactOffset offset,
					   int nmembers, MultiXactMember *members)
{
	for (int i = 0; i < nmembers; i++, offset++)
	{
		int64		pageno;
		char	   *buf;
		TransactionId *memberptr;
		uint32	   *flagsptr;
		uint32		flagsval;
		int			bshift;
		int			flagsoff;
		int			memberoff;

		Assert(members[i].status <= MultiXactStatusUpdate);

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);
		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);

		buf = SlruWriteSwitchPage(members_writer, pageno);

		memberptr = (TransactionId *) (buf + memberoff);

		*memberptr = members[i].xid;

		flagsptr = (uint32 *) (buf + flagsoff);

		flagsval = *flagsptr;
		flagsval &= ~(((1 << MXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
		flagsval |= (members[i].status << bshift);
		*flagsptr = flagsval;
	}
}
