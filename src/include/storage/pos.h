/*-------------------------------------------------------------------------
 *
 * pos.h
 *	  POSTGRES "position" definitions.
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pos.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef POS_H
#define POS_H


/*
 * a 'position' used to be <pagenumber, offset> in postgres.  this has
 * been changed to just <offset> as the notion of having multiple pages
 * within a block has been removed.
 *
 * the 'offset' abstraction is somewhat confusing.  it is NOT a byte
 * offset within the page; instead, it is an offset into the line
 * pointer array contained on every page that store (heap or index)
 * tuples.
 */
typedef bits16 PositionIdData;
typedef PositionIdData *PositionId;

/* ----------------
 *		support macros
 * ----------------
 */

/*
 * PositionIdIsValid
 *		True iff the position identifier is valid.
 */
#define PositionIdIsValid(positionId) \
	PointerIsValid(positionId)

/*
 * PositionIdSetInvalid
 *		Make an invalid position.
 */
#define PositionIdSetInvalid(positionId) \
	*(positionId) = (bits16) 0

/*
 * PositionIdSet
 *		Sets a position identifier to the specified value.
 */
#define PositionIdSet(positionId, offsetNumber) \
	*(positionId) = (offsetNumber)

/*
 * PositionIdGetOffsetNumber
 *		Retrieve the offset number from a position identifier.
 */
#define PositionIdGetOffsetNumber(positionId) \
	((OffsetNumber) *(positionId))

#endif   /* POS_H */
