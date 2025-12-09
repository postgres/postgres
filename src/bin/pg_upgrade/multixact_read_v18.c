/*
 * multixact_read_v18.c
 *
 * Functions to read multixact SLRUs from clusters of PostgreSQL version 18
 * and older.  In version 19, the multixid offsets were expanded from 32 to 64
 * bits.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 * src/bin/pg_upgrade/multixact_read_v18.c
 */

#include "postgres_fe.h"

#include "multixact_read_v18.h"
#include "pg_upgrade.h"

/*
 * NOTE: below are a bunch of definitions that are copy-pasted from
 * multixact.c from version 18.  It's important that this file doesn't
 * #include the new definitions with same names from "multixact_internal.h"!
 *
 * To further avoid confusion in the functions exposed outside this source
 * file, we use MultiXactOffset32 to represent the old-style 32-bit multixid
 * offsets.  The new 64-bit MultiXactOffset should not be used anywhere in
 * this file.
 */
#ifdef MULTIXACT_INTERNAL_H
#error multixact_internal.h should not be included in multixact_read_v18.c
#endif
#define MultiXactOffset should_not_be_used

/* We need four bytes per offset and 8 bytes per base for each page. */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset32))

static inline int64
MultiXactIdToOffsetPage(MultiXactId multi)
{
	return multi / MULTIXACT_OFFSETS_PER_PAGE;
}

static inline int
MultiXactIdToOffsetEntry(MultiXactId multi)
{
	return multi % MULTIXACT_OFFSETS_PER_PAGE;
}

/*
 * The situation for members is a bit more complex: we store one byte of
 * additional flag bits for each TransactionId.  To do this without getting
 * into alignment issues, we store four bytes of flags, and then the
 * corresponding 4 Xids.  Each such 5-word (20-byte) set we call a "group", and
 * are stored as a whole in pages.  Thus, with 8kB BLCKSZ, we keep 409 groups
 * per page.  This wastes 12 bytes per page, but that's OK -- simplicity (and
 * performance) trumps space efficiency here.
 *
 * Note that the "offset" macros work with byte offset, not array indexes, so
 * arithmetic must be done using "char *" pointers.
 */
/* We need eight bits per xact, so one xact fits in a byte */
#define MXACT_MEMBER_BITS_PER_XACT			8
#define MXACT_MEMBER_FLAGS_PER_BYTE			1
#define MXACT_MEMBER_XACT_BITMASK	((1 << MXACT_MEMBER_BITS_PER_XACT) - 1)

/* how many full bytes of flags are there in a group? */
#define MULTIXACT_FLAGBYTES_PER_GROUP		4
#define MULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(MULTIXACT_FLAGBYTES_PER_GROUP * MXACT_MEMBER_FLAGS_PER_BYTE)
/* size in bytes of a complete group */
#define MULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(TransactionId) * MULTIXACT_MEMBERS_PER_MEMBERGROUP + MULTIXACT_FLAGBYTES_PER_GROUP)
#define MULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)
#define MULTIXACT_MEMBERS_PER_PAGE	\
	(MULTIXACT_MEMBERGROUPS_PER_PAGE * MULTIXACT_MEMBERS_PER_MEMBERGROUP)

/* page in which a member is to be found */
static inline int64
MXOffsetToMemberPage(MultiXactOffset32 offset)
{
	return offset / MULTIXACT_MEMBERS_PER_PAGE;
}

/* Location (byte offset within page) of flag word for a given member */
static inline int
MXOffsetToFlagsOffset(MultiXactOffset32 offset)
{
	MultiXactOffset32 group = offset / MULTIXACT_MEMBERS_PER_MEMBERGROUP;
	int			grouponpg = group % MULTIXACT_MEMBERGROUPS_PER_PAGE;
	int			byteoff = grouponpg * MULTIXACT_MEMBERGROUP_SIZE;

	return byteoff;
}

/* Location (byte offset within page) of TransactionId of given member */
static inline int
MXOffsetToMemberOffset(MultiXactOffset32 offset)
{
	int			member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;

	return MXOffsetToFlagsOffset(offset) +
		MULTIXACT_FLAGBYTES_PER_GROUP +
		member_in_group * sizeof(TransactionId);
}

static inline int
MXOffsetToFlagsBitShift(MultiXactOffset32 offset)
{
	int			member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;
	int			bshift = member_in_group * MXACT_MEMBER_BITS_PER_XACT;

	return bshift;
}

/*
 * Construct reader of old multixacts.
 *
 * Returns the malloced memory used by the all other calls in this module.
 */
OldMultiXactReader *
AllocOldMultiXactRead(char *pgdata, MultiXactId nextMulti,
					  MultiXactOffset32 nextOffset)
{
	OldMultiXactReader *state = state = pg_malloc(sizeof(*state));
	char		dir[MAXPGPATH] = {0};

	state->nextMXact = nextMulti;
	state->nextOffset = nextOffset;

	pg_sprintf(dir, "%s/pg_multixact/offsets", pgdata);
	state->offset = AllocSlruRead(dir, false);

	pg_sprintf(dir, "%s/pg_multixact/members", pgdata);
	state->members = AllocSlruRead(dir, false);

	return state;
}

/*
 * This is a simplified version of the GetMultiXactIdMembers() server
 * function:
 *
 * - Only return the updating member, if any.  Upgrade only cares about the
 *   updaters.  If there is no updating member, return somewhat arbitrarily
 *   the first locking-only member, because we don't have any way to represent
 *   "no members".
 *
 * - Because there's no concurrent activity, we don't need to worry about
 *   locking and some corner cases.
 *
 * - Don't bail out on invalid entries.  If the server crashes, it can leave
 *   invalid or half-written entries on disk.  Such multixids won't appear
 *   anywhere else on disk, so the server will never try to read them.  During
 *   upgrade, however, we scan through all multixids in order, and will
 *   encounter such invalid but unreferenced multixids too.
 *
 * Returns true on success, false if the multixact was invalid.
 */
bool
GetOldMultiXactIdSingleMember(OldMultiXactReader *state, MultiXactId multi,
							  MultiXactMember *member)
{
	MultiXactId nextMXact,
				nextOffset,
				tmpMXact;
	int64		pageno,
				prev_pageno;
	int			entryno,
				length;
	char	   *buf;
	MultiXactOffset32 *offptr,
				offset;
	MultiXactOffset32 nextMXOffset;
	TransactionId result_xid = InvalidTransactionId;
	MultiXactStatus result_status = 0;

	nextMXact = state->nextMXact;
	nextOffset = state->nextOffset;

	/*
	 * Comment copied from GetMultiXactIdMembers in PostgreSQL v18
	 * multixact.c:
	 *
	 * Find out the offset at which we need to start reading MultiXactMembers
	 * and the number of members in the multixact.  We determine the latter as
	 * the difference between this multixact's starting offset and the next
	 * one's.  However, there are some corner cases to worry about:
	 *
	 * 1. This multixact may be the latest one created, in which case there is
	 * no next one to look at.  The next multixact's offset should be set
	 * already, as we set it in RecordNewMultiXact(), but we used to not do
	 * that in older minor versions.  To cope with that case, if this
	 * multixact is the latest one created, use the nextOffset value we read
	 * above as the endpoint.
	 *
	 * 2. Because GetNewMultiXactId skips over offset zero, to reserve zero
	 * for to mean "unset", there is an ambiguity near the point of offset
	 * wraparound.  If we see next multixact's offset is one, is that our
	 * multixact's actual endpoint, or did it end at zero with a subsequent
	 * increment?  We handle this using the knowledge that if the zero'th
	 * member slot wasn't filled, it'll contain zero, and zero isn't a valid
	 * transaction ID so it can't be a multixact member.  Therefore, if we
	 * read a zero from the members array, just ignore it.
	 */

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	buf = SlruReadSwitchPage(state->offset, pageno);
	offptr = (MultiXactOffset32 *) buf;
	offptr += entryno;
	offset = *offptr;

	if (offset == 0)
	{
		/* Invalid entry */
		return false;
	}

	/*
	 * Use the same increment rule as GetNewMultiXactId(), that is, don't
	 * handle wraparound explicitly until needed.
	 */
	tmpMXact = multi + 1;

	if (nextMXact == tmpMXact)
	{
		/* Corner case 1: there is no next multixact */
		nextMXOffset = nextOffset;
	}
	else
	{
		/* handle wraparound if needed */
		if (tmpMXact < FirstMultiXactId)
			tmpMXact = FirstMultiXactId;

		prev_pageno = pageno;

		pageno = MultiXactIdToOffsetPage(tmpMXact);
		entryno = MultiXactIdToOffsetEntry(tmpMXact);

		if (pageno != prev_pageno)
			buf = SlruReadSwitchPage(state->offset, pageno);

		offptr = (MultiXactOffset32 *) buf;
		offptr += entryno;
		nextMXOffset = *offptr;
	}

	if (nextMXOffset == 0)
	{
		/* Invalid entry */
		return false;
	}
	length = nextMXOffset - offset;

	/* read the members */
	prev_pageno = -1;
	for (int i = 0; i < length; i++, offset++)
	{
		TransactionId *xactptr;
		uint32	   *flagsptr;
		int			flagsoff;
		int			bshift;
		int			memberoff;
		MultiXactStatus status;

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);

		if (pageno != prev_pageno)
		{
			buf = SlruReadSwitchPage(state->members, pageno);
			prev_pageno = pageno;
		}

		xactptr = (TransactionId *) (buf + memberoff);
		if (!TransactionIdIsValid(*xactptr))
		{
			/*
			 * Corner case 2: we are looking at unused slot zero
			 */
			if (offset == 0)
				continue;

			/*
			 * Otherwise this is an invalid entry that should not be
			 * referenced from anywhere in the heap.  We could return 'false'
			 * here, but we prefer to continue reading the members and
			 * converting them the best we can, to preserve evidence in case
			 * this is corruption that should not happen.
			 */
		}

		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);
		flagsptr = (uint32 *) (buf + flagsoff);

		status = (*flagsptr >> bshift) & MXACT_MEMBER_XACT_BITMASK;

		/*
		 * Remember the updating XID among the members, or first locking XID
		 * if no updating XID.
		 */
		if (ISUPDATE_from_mxstatus(status))
		{
			/* sanity check */
			if (ISUPDATE_from_mxstatus(result_status))
			{
				/*
				 * We don't expect to see more than one updating member, even
				 * if the server had crashed.
				 */
				pg_fatal("multixact %u has more than one updating member",
						 multi);
			}
			result_xid = *xactptr;
			result_status = status;
		}
		else if (!TransactionIdIsValid(result_xid))
		{
			result_xid = *xactptr;
			result_status = status;
		}
	}

	member->xid = result_xid;
	member->status = result_status;
	return true;
}

/*
 * Frees the malloced reader.
 */
void
FreeOldMultiXactReader(OldMultiXactReader *state)
{
	FreeSlruRead(state->offset);
	FreeSlruRead(state->members);

	pfree(state);
}
