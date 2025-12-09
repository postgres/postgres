/*
 * multixact_internal.h
 *
 * PostgreSQL multi-transaction-log manager internal declarations
 *
 * These functions and definitions are for dealing with pg_multixact SLRU
 * pages.  They are internal to multixact.c, but they are exported here to
 * allow pg_upgrade to write pg_multixact files directly.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/multixact_internal.h
 */
#ifndef MULTIXACT_INTERNAL_H

/*
 * Note: This is not only to prevent including this file twice.
 * MULTIXACT_INTERNAL_H is checked explicitly in multixact_read_v18.c.
 */
#define MULTIXACT_INTERNAL_H

#include "access/multixact.h"


/*
 * Defines for MultiXactOffset page sizes.  A page is the same BLCKSZ as is
 * used everywhere else in Postgres.
 */

/* We need 8 bytes per offset */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset))

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

static inline int64
MultiXactIdToOffsetSegment(MultiXactId multi)
{
	return MultiXactIdToOffsetPage(multi) / SLRU_PAGES_PER_SEGMENT;
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
MXOffsetToMemberPage(MultiXactOffset offset)
{
	return offset / MULTIXACT_MEMBERS_PER_PAGE;
}

static inline int64
MXOffsetToMemberSegment(MultiXactOffset offset)
{
	return MXOffsetToMemberPage(offset) / SLRU_PAGES_PER_SEGMENT;
}

/* Location (byte offset within page) of flag word for a given member */
static inline int
MXOffsetToFlagsOffset(MultiXactOffset offset)
{
	MultiXactOffset group = offset / MULTIXACT_MEMBERS_PER_MEMBERGROUP;
	int			grouponpg = group % MULTIXACT_MEMBERGROUPS_PER_PAGE;
	int			byteoff = grouponpg * MULTIXACT_MEMBERGROUP_SIZE;

	return byteoff;
}

static inline int
MXOffsetToFlagsBitShift(MultiXactOffset offset)
{
	int			member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;
	int			bshift = member_in_group * MXACT_MEMBER_BITS_PER_XACT;

	return bshift;
}

/* Location (byte offset within page) of TransactionId of given member */
static inline int
MXOffsetToMemberOffset(MultiXactOffset offset)
{
	int			member_in_group = offset % MULTIXACT_MEMBERS_PER_MEMBERGROUP;

	return MXOffsetToFlagsOffset(offset) +
		MULTIXACT_FLAGBYTES_PER_GROUP +
		member_in_group * sizeof(TransactionId);
}

#endif							/* MULTIXACT_INTERNAL_H */
