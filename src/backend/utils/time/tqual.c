/*-------------------------------------------------------------------------
 *
 * tqual.c--
 *	  POSTGRES time qualification code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/time/tqual.c,v 1.5 1997/09/07 04:54:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/* #define TQUALDEBUG	1 */

#include "postgres.h"

#include "access/htup.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "access/transam.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/nabstime.h"

#include "utils/tqual.h"

static AbsoluteTime TimeQualGetEndTime(TimeQual qual);
static AbsoluteTime TimeQualGetSnapshotTime(TimeQual qual);
static AbsoluteTime TimeQualGetStartTime(TimeQual qual);
static bool		TimeQualIncludesNow(TimeQual qual);
static bool		TimeQualIndicatesDisableValidityChecking(TimeQual qual);
static bool		TimeQualIsLegal(TimeQual qual);
static bool		TimeQualIsRanged(TimeQual qual);
static bool		TimeQualIsSnapshot(TimeQual qual);
static bool		TimeQualIsValid(TimeQual qual);

/*
 * TimeQualMode --
 *		Mode indicator for treatment of time qualifications.
 */
typedef uint16	TimeQualMode;

#define TimeQualAt		0x1
#define TimeQualNewer	0x2
#define TimeQualOlder	0x4
#define TimeQualAll		0x8

#define TimeQualMask	0xf

#define TimeQualEvery	0x0
#define TimeQualRange	(TimeQualNewer | TimeQualOlder)
#define TimeQualAllAt	(TimeQualAt | TimeQualAll)

typedef struct TimeQualData
{
	AbsoluteTime	start;
	AbsoluteTime	end;
	TimeQualMode	mode;
}				TimeQualData;

typedef TimeQualData *InternalTimeQual;

static TimeQualData SelfTimeQualData;
TimeQual		SelfTimeQual = (Pointer) & SelfTimeQualData;

extern bool		PostgresIsInitialized;

/*
 * XXX Transaction system override hacks start here
 */
#ifndef GOODAMI

static TransactionId HeapSpecialTransactionId = InvalidTransactionId;
static CommandId HeapSpecialCommandId = FirstCommandId;

void
setheapoverride(bool on)
{
	if (on)
	{
		TransactionIdStore(GetCurrentTransactionId(),
						   &HeapSpecialTransactionId);
		HeapSpecialCommandId = GetCurrentCommandId();
	}
	else
	{
		HeapSpecialTransactionId = InvalidTransactionId;
	}
}

/* static */
bool
heapisoverride()
{
	if (!TransactionIdIsValid(HeapSpecialTransactionId))
	{
		return (false);
	}

	if (!TransactionIdEquals(GetCurrentTransactionId(),
							 HeapSpecialTransactionId) ||
		GetCurrentCommandId() != HeapSpecialCommandId)
	{
		HeapSpecialTransactionId = InvalidTransactionId;

		return (false);
	}
	return (true);
}

#endif							/* !defined(GOODAMI) */
/*
 * XXX Transaction system override hacks end here
 */

static bool		HeapTupleSatisfiesItself(HeapTuple tuple);
static bool		HeapTupleSatisfiesNow(HeapTuple tuple);
static bool
HeapTupleSatisfiesSnapshotInternalTimeQual(HeapTuple tuple,
										   InternalTimeQual qual);
static bool
HeapTupleSatisfiesUpperBoundedInternalTimeQual(HeapTuple tuple,
											   InternalTimeQual qual);
static bool
HeapTupleSatisfiesUpperUnboundedInternalTimeQual(HeapTuple tuple,
												 InternalTimeQual qual);



/*
 * TimeQualIsValid --
 *		True iff time qualification is valid.
 */
static			bool
TimeQualIsValid(TimeQual qual)
{
	bool			hasStartTime;

	if (!PointerIsValid(qual) || qual == SelfTimeQual)
	{
		return (true);
	}

	if (((InternalTimeQual) qual)->mode & ~TimeQualMask)
	{
		return (false);
	}

	if (((InternalTimeQual) qual)->mode & TimeQualAt)
	{
		return (AbsoluteTimeIsBackwardCompatiblyValid(((InternalTimeQual) qual)->start));
	}

	hasStartTime = false;

	if (((InternalTimeQual) qual)->mode & TimeQualNewer)
	{
		if (!AbsoluteTimeIsBackwardCompatiblyValid(((InternalTimeQual) qual)->start))
		{
			return (false);
		}
		hasStartTime = true;
	}

	if (((InternalTimeQual) qual)->mode & TimeQualOlder)
	{
		if (!AbsoluteTimeIsBackwardCompatiblyValid(((InternalTimeQual) qual)->end))
		{
			return (false);
		}
		if (hasStartTime)
		{
			return ((bool) ! AbsoluteTimeIsBefore(
										  ((InternalTimeQual) qual)->end,
									  ((InternalTimeQual) qual)->start));
		}
	}
	return (true);
}

/*
 * TimeQualIsLegal --
 *		True iff time qualification is legal.
 *		I.e., true iff time qualification does not intersects the future,
 *		relative to the transaction start time.
 *
 * Note:
 *		Assumes time qualification is valid.
 */
static			bool
TimeQualIsLegal(TimeQual qual)
{
	Assert(TimeQualIsValid(qual));

	if (qual == NowTimeQual || qual == SelfTimeQual)
	{
		return (true);
	}

	/* TimeQualAt */
	if (((InternalTimeQual) qual)->mode & TimeQualAt)
	{
		AbsoluteTime	a,
						b;

		a = ((InternalTimeQual) qual)->start;
		b = GetCurrentTransactionStartTime();

		if (AbsoluteTimeIsAfter(a, b))
			return (false);
		else
			return (true);
	}

	/* TimeQualOlder or TimeQualRange */
	if (((InternalTimeQual) qual)->mode & TimeQualOlder)
	{
		AbsoluteTime	a,
						b;

		a = ((InternalTimeQual) qual)->end;
		b = GetCurrentTransactionStartTime();

		if (AbsoluteTimeIsAfter(a, b))
			return (false);
		else
			return (true);
	}

	/* TimeQualNewer */
	if (((InternalTimeQual) qual)->mode & TimeQualNewer)
	{
		AbsoluteTime	a,
						b;

		a = ((InternalTimeQual) qual)->start;
		b = GetCurrentTransactionStartTime();

		if (AbsoluteTimeIsAfter(a, b))
			return (false);
		else
			return (true);
	}

	/* TimeQualEvery */
	return (true);
}

/*
 * TimeQualIncludesNow --
 *		True iff time qualification includes "now."
 *
 * Note:
 *		Assumes time qualification is valid.
 */
static			bool
TimeQualIncludesNow(TimeQual qual)
{
	Assert(TimeQualIsValid(qual));

	if (qual == NowTimeQual || qual == SelfTimeQual)
	{
		return (true);
	}

	if (((InternalTimeQual) qual)->mode & TimeQualAt)
	{
		return (false);
	}
	if (((InternalTimeQual) qual)->mode & TimeQualOlder &&
		!AbsoluteTimeIsAfter(
							 ((InternalTimeQual) qual)->end,
							 GetCurrentTransactionStartTime()))
	{

		return (false);
	}
	return (true);
}

/*
 * TimeQualIncludesPast --
 *		True iff time qualification includes some time in the past.
 *
 * Note:
 *		Assumes time qualification is valid.
 *		XXX may not be needed?
 */
#ifdef NOT_USED
bool
TimeQualIncludesPast(TimeQual qual)
{
	Assert(TimeQualIsValid(qual));

	if (qual == NowTimeQual || qual == SelfTimeQual)
	{
		return (false);
	}

	/* otherwise, must check archive (setting locks as appropriate) */
	return (true);
}

#endif

/*
 * TimeQualIsSnapshot --
 *		True iff time qualification is a snapshot qualification.
 *
 * Note:
 *		Assumes time qualification is valid.
 */
static			bool
TimeQualIsSnapshot(TimeQual qual)
{
	Assert(TimeQualIsValid(qual));

	if (qual == NowTimeQual || qual == SelfTimeQual)
	{
		return (false);
	}

	return ((bool) ! !(((InternalTimeQual) qual)->mode & TimeQualAt));
}

/*
 * TimeQualIsRanged --
 *		True iff time qualification is a ranged qualification.
 *
 * Note:
 *		Assumes time qualification is valid.
 */
static			bool
TimeQualIsRanged(TimeQual qual)
{
	Assert(TimeQualIsValid(qual));

	if (qual == NowTimeQual || qual == SelfTimeQual)
	{
		return (false);
	}

	return ((bool) ! (((InternalTimeQual) qual)->mode & TimeQualAt));
}

/*
 * TimeQualIndicatesDisableValidityChecking --
 *		True iff time qualification indicates validity checking should be
 *		disabled.
 *
 * Note:
 *		XXX This should not be implemented since this does not make sense.
 */
static			bool
TimeQualIndicatesDisableValidityChecking(TimeQual qual)
{
	Assert(TimeQualIsValid(qual));

	if (qual == NowTimeQual || qual == SelfTimeQual)
	{
		return (false);
	}

	if (((InternalTimeQual) qual)->mode & TimeQualAll)
	{
		return (true);
	}
	return (false);
}

/*
 * TimeQualGetSnapshotTime --
 *		Returns time for a snapshot time qual.
 *
 * Note:
 *		Assumes time qual is valid snapshot time qual.
 */
static			AbsoluteTime
TimeQualGetSnapshotTime(TimeQual qual)
{
	Assert(TimeQualIsSnapshot(qual));

	return (((InternalTimeQual) qual)->start);
}

/*
 * TimeQualGetStartTime --
 *		Returns start time for a ranged time qual.
 *
 * Note:
 *		Assumes time qual is valid ranged time qual.
 */
static			AbsoluteTime
TimeQualGetStartTime(TimeQual qual)
{
	Assert(TimeQualIsRanged(qual));

	return (((InternalTimeQual) qual)->start);
}

/*
 * TimeQualGetEndTime --
 *		Returns end time for a ranged time qual.
 *
 * Note:
 *		Assumes time qual is valid ranged time qual.
 */
static			AbsoluteTime
TimeQualGetEndTime(TimeQual qual)
{
	Assert(TimeQualIsRanged(qual));

	return (((InternalTimeQual) qual)->end);
}

/*
 * TimeFormSnapshotTimeQual --
 *		Returns snapshot time qual for a time.
 *
 * Note:
 *		Assumes time is valid.
 */
TimeQual
TimeFormSnapshotTimeQual(AbsoluteTime time)
{
	InternalTimeQual qual;

	Assert(AbsoluteTimeIsBackwardCompatiblyValid(time));

	qual = (InternalTimeQual) palloc(sizeof *qual);

	qual->start = time;
	qual->end = INVALID_ABSTIME;
	qual->mode = TimeQualAt;

	return ((TimeQual) qual);
}

/*
 * TimeFormRangedTimeQual --
 *		Returns ranged time qual for a pair of times.
 *
 * Note:
 *		If start time is invalid, it is regarded as the epoch.
 *		If end time is invalid, it is regarded as "now."
 *		Assumes start time is before (or the same as) end time.
 */
TimeQual
TimeFormRangedTimeQual(AbsoluteTime startTime,
					   AbsoluteTime endTime)
{
	InternalTimeQual qual;

	qual = (InternalTimeQual) palloc(sizeof *qual);

	qual->start = startTime;
	qual->end = endTime;
	qual->mode = TimeQualEvery;

	if (AbsoluteTimeIsBackwardCompatiblyValid(startTime))
	{
		qual->mode |= TimeQualNewer;
	}
	if (AbsoluteTimeIsBackwardCompatiblyValid(endTime))
	{
		qual->mode |= TimeQualOlder;
	}

	return ((TimeQual) qual);
}

/*
 * HeapTupleSatisfiesTimeQual --
 *		True iff heap tuple satsifies a time qual.
 *
 * Note:
 *		Assumes heap tuple is valid.
 *		Assumes time qual is valid.
 *		XXX Many of the checks may be simplified and still remain correct.
 *		XXX Partial answers to the checks may be cached in an ItemId.
 */
bool
HeapTupleSatisfiesTimeQual(HeapTuple tuple, TimeQual qual)
{
/*	  extern TransactionId AmiTransactionId; */

	Assert(HeapTupleIsValid(tuple));
	Assert(TimeQualIsValid(qual));

	if (TransactionIdEquals(tuple->t_xmax, AmiTransactionId))
		return (false);

	if (qual == SelfTimeQual || heapisoverride())
	{
		return (HeapTupleSatisfiesItself(tuple));
	}

	if (qual == NowTimeQual)
	{
		return (HeapTupleSatisfiesNow(tuple));
	}

	if (!TimeQualIsLegal(qual))
	{
		elog(WARN, "HeapTupleSatisfiesTimeQual: illegal time qual");
	}

	if (TimeQualIndicatesDisableValidityChecking(qual))
	{
		elog(WARN, "HeapTupleSatisfiesTimeQual: no disabled validity checking (yet)");
	}

	if (TimeQualIsSnapshot(qual))
	{
		return (HeapTupleSatisfiesSnapshotInternalTimeQual(tuple,
											   (InternalTimeQual) qual));
	}

	if (TimeQualIncludesNow(qual))
	{
		return (HeapTupleSatisfiesUpperUnboundedInternalTimeQual(tuple,
											   (InternalTimeQual) qual));
	}

	return (HeapTupleSatisfiesUpperBoundedInternalTimeQual(tuple,
											   (InternalTimeQual) qual));
}

/*
 * HeapTupleSatisfiesItself --
 *		True iff heap tuple is valid for "itself."
 *		"{it}self" means valid as of everything that's happened
 *		in the current transaction, _including_ the current command.
 *
 * Note:
 *		Assumes heap tuple is valid.
 */
/*
 * The satisfaction of "itself" requires the following:
 *
 * ((Xmin == my-transaction &&				the row was updated by the current transaction, and
 *		(Xmax is null						it was not deleted
 *		 [|| Xmax != my-transaction)])			[or it was deleted by another transaction]
 * ||
 *
 * (Xmin is committed &&					the row was modified by a committed transaction, and
 *		(Xmax is null ||					the row has not been deleted, or
 *			(Xmax != my-transaction &&			the row was deleted by another transaction
 *			 Xmax is not committed)))			that has not been committed
 */
static			bool
HeapTupleSatisfiesItself(HeapTuple tuple)
{

	/*
	 * XXX Several evil casts are made in this routine.  Casting XID to be
	 * TransactionId works only because TransactionId->data is the first
	 * (and only) field of the structure.
	 */
	if (!AbsoluteTimeIsBackwardCompatiblyValid(tuple->t_tmin))
	{
		if (TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmin) &&
			!TransactionIdIsValid((TransactionId) tuple->t_xmax))
		{
			return (true);
		}

		if (!TransactionIdDidCommit((TransactionId) tuple->t_xmin))
		{
			return (false);
		}

		tuple->t_tmin = TransactionIdGetCommitTime(tuple->t_xmin);
	}
	/* the tuple was inserted validly */

	if (AbsoluteTimeIsBackwardCompatiblyReal(tuple->t_tmax))
	{
		return (false);
	}

	if (!TransactionIdIsValid((TransactionId) tuple->t_xmax))
	{
		return (true);
	}

	if (TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmax))
	{
		return (false);
	}

	if (!TransactionIdDidCommit((TransactionId) tuple->t_xmax))
	{
		return (true);
	}

	/* by here, deleting transaction has committed */
	tuple->t_tmax = TransactionIdGetCommitTime(tuple->t_xmax);

	return (false);
}

/*
 * HeapTupleSatisfiesNow --
 *		True iff heap tuple is valid "now."
 *		"now" means valid including everything that's happened
 *		 in the current transaction _up to, but not including,_
 *		 the current command.
 *
 * Note:
 *		Assumes heap tuple is valid.
 */
/*
 * The satisfaction of "now" requires the following:
 *
 * ((Xmin == my-transaction &&				changed by the current transaction
 *	 Cmin != my-command &&					but not by this command, and
 *		(Xmax is null ||						the row has not been deleted, or
 *			(Xmax == my-transaction &&			it was deleted by the current transaction
 *			 Cmax != my-command)))				but not by this command,
 * ||										or
 *
 *	(Xmin is committed &&					the row was modified by a committed transaction, and
 *		(Xmax is null ||					the row has not been deleted, or
 *			(Xmax == my-transaction &&			the row is being deleted by this command, or
 *			 Cmax == my-command) ||
 *			(Xmax is not committed &&			the row was deleted by another transaction
 *			 Xmax != my-transaction))))			that has not been committed
 *
 * XXX
 *		CommandId stuff didn't work properly if one used SQL-functions in
 *		UPDATE/INSERT(fromSELECT)/DELETE scans: SQL-funcs call
 *		CommandCounterIncrement and made tuples changed/inserted by
 *		current command visible to command itself (so we had multiple
 *		update of updated tuples, etc).			- vadim 08/29/97
 *
 *		mao says 17 march 1993:  the tests in this routine are correct;
 *		if you think they're not, you're wrong, and you should think
 *		about it again.  i know, it happened to me.  we don't need to
 *		check commit time against the start time of this transaction
 *		because 2ph locking protects us from doing the wrong thing.
 *		if you mess around here, you'll break serializability.  the only
 *		problem with this code is that it does the wrong thing for system
 *		catalog updates, because the catalogs aren't subject to 2ph, so
 *		the serializability guarantees we provide don't extend to xacts
 *		that do catalog accesses.  this is unfortunate, but not critical.
 */
static			bool
HeapTupleSatisfiesNow(HeapTuple tuple)
{
	if (AMI_OVERRIDE)
		return true;

	/*
	 * If the transaction system isn't yet initialized, then we assume
	 * that transactions committed.  We only look at system catalogs
	 * during startup, so this is less awful than it seems, but it's still
	 * pretty awful.
	 */

	if (!PostgresIsInitialized)
		return ((bool) (TransactionIdIsValid((TransactionId) tuple->t_xmin) &&
				  !TransactionIdIsValid((TransactionId) tuple->t_xmax)));

	/*
	 * XXX Several evil casts are made in this routine.  Casting XID to be
	 * TransactionId works only because TransactionId->data is the first
	 * (and only) field of the structure.
	 */
	if (!AbsoluteTimeIsBackwardCompatiblyValid(tuple->t_tmin))
	{

		if (TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmin)
			&& CommandIdGEScanCommandId(tuple->t_cmin))
		{

			return (false);
		}

		if (TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmin)
			&& !CommandIdGEScanCommandId(tuple->t_cmin))
		{

			if (!TransactionIdIsValid((TransactionId) tuple->t_xmax))
			{
				return (true);
			}

			Assert(TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmax));

			if (CommandIdGEScanCommandId(tuple->t_cmax))
			{
				return (true);
			}
		}

		/*
		 * this call is VERY expensive - requires a log table lookup.
		 */

		if (!TransactionIdDidCommit((TransactionId) tuple->t_xmin))
		{
			return (false);
		}

		/*
		 * the transaction has been committed--store the commit time _now_
		 * instead of waiting for a vacuum so we avoid the expensive call
		 * next time.
		 */
		tuple->t_tmin = TransactionIdGetCommitTime(tuple->t_xmin);
	}

	/* by here, the inserting transaction has committed */
	if (!TransactionIdIsValid((TransactionId) tuple->t_xmax))
	{
		return (true);
	}

	if (TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmax))
	{
		return (false);
	}

	if (AbsoluteTimeIsBackwardCompatiblyReal(tuple->t_tmax))
	{
		return (false);
	}

	if (!TransactionIdDidCommit((TransactionId) tuple->t_xmax))
	{
		return (true);
	}

	/* xmax transaction committed, but no tmax set.  so set it. */
	tuple->t_tmax = TransactionIdGetCommitTime(tuple->t_xmax);

	return (false);
}

/*
 * HeapTupleSatisfiesSnapshotInternalTimeQual --
 *		True iff heap tuple is valid at the snapshot time qualification.
 *
 * Note:
 *		Assumes heap tuple is valid.
 *		Assumes internal time qualification is valid snapshot qualification.
 */
/*
 * The satisfaction of Rel[T] requires the following:
 *
 * (Xmin is committed && Tmin <= T &&
 *		(Xmax is null || (Xmax is not committed && Xmax != my-transaction) ||
 *				Tmax >= T))
 */
static			bool
HeapTupleSatisfiesSnapshotInternalTimeQual(HeapTuple tuple,
										   InternalTimeQual qual)
{

	/*
	 * XXX Several evil casts are made in this routine.  Casting XID to be
	 * TransactionId works only because TransactionId->data is the first
	 * (and only) field of the structure.
	 */
	if (!AbsoluteTimeIsBackwardCompatiblyValid(tuple->t_tmin))
	{

		if (!TransactionIdDidCommit((TransactionId) tuple->t_xmin))
		{
			return (false);
		}

		tuple->t_tmin = TransactionIdGetCommitTime(tuple->t_xmin);
	}

	if (AbsoluteTimeIsBefore(TimeQualGetSnapshotTime((TimeQual) qual), tuple->t_tmin))
	{
		return (false);
	}
	/* the tuple was inserted validly before the snapshot time */

	if (!AbsoluteTimeIsBackwardCompatiblyReal(tuple->t_tmax))
	{

		if (!TransactionIdIsValid((TransactionId) tuple->t_xmax) ||
			!TransactionIdDidCommit((TransactionId) tuple->t_xmax))
		{

			return (true);
		}

		tuple->t_tmax = TransactionIdGetCommitTime(tuple->t_xmax);
	}

	return ((bool)
			AbsoluteTimeIsAfter(tuple->t_tmax,
							  TimeQualGetSnapshotTime((TimeQual) qual)));
}

/*
 * HeapTupleSatisfiesUpperBoundedInternalTimeQual --
 *		True iff heap tuple is valid within a upper bounded time qualification.
 *
 * Note:
 *		Assumes heap tuple is valid.
 *		Assumes time qualification is valid ranged qualification with fixed
 *		upper bound.
 */
/*
 * The satisfaction of [T1,T2] requires the following:
 *
 * (Xmin is committed && Tmin <= T2 &&
 *		(Xmax is null || (Xmax is not committed && Xmax != my-transaction) ||
 *				T1 is null || Tmax >= T1))
 */
static			bool
HeapTupleSatisfiesUpperBoundedInternalTimeQual(HeapTuple tuple,
											   InternalTimeQual qual)
{

	/*
	 * XXX Several evil casts are made in this routine.  Casting XID to be
	 * TransactionId works only because TransactionId->data is the first
	 * (and only) field of the structure.
	 */
	if (!AbsoluteTimeIsBackwardCompatiblyValid(tuple->t_tmin))
	{

		if (!TransactionIdDidCommit((TransactionId) tuple->t_xmin))
		{
			return (false);
		}

		tuple->t_tmin = TransactionIdGetCommitTime(tuple->t_xmin);
	}

	if (AbsoluteTimeIsBefore(TimeQualGetEndTime((TimeQual) qual), tuple->t_tmin))
	{
		return (false);
	}
	/* the tuple was inserted validly before the range end */

	if (!AbsoluteTimeIsBackwardCompatiblyValid(TimeQualGetStartTime((TimeQual) qual)))
	{
		return (true);
	}

	if (!AbsoluteTimeIsBackwardCompatiblyReal(tuple->t_tmax))
	{

		if (!TransactionIdIsValid((TransactionId) tuple->t_xmax) ||
			!TransactionIdDidCommit((TransactionId) tuple->t_xmax))
		{

			return (true);
		}

		tuple->t_tmax = TransactionIdGetCommitTime(tuple->t_xmax);
	}

	return ((bool) AbsoluteTimeIsAfter(tuple->t_tmax,
								 TimeQualGetStartTime((TimeQual) qual)));
}

/*
 * HeapTupleSatisfiesUpperUnboundedInternalTimeQual --
 *		True iff heap tuple is valid within a upper bounded time qualification.
 *
 * Note:
 *		Assumes heap tuple is valid.
 *		Assumes time qualification is valid ranged qualification with no
 *		upper bound.
 */
/*
 * The satisfaction of [T1,] requires the following:
 *
 * ((Xmin == my-transaction && Cmin != my-command &&
 *		(Xmax is null || (Xmax == my-transaction && Cmax != my-command)))
 * ||
 *
 * (Xmin is committed &&
 *		(Xmax is null || (Xmax == my-transaction && Cmax == my-command) ||
 *				(Xmax is not committed && Xmax != my-transaction) ||
 *				T1 is null || Tmax >= T1)))
 */
static			bool
HeapTupleSatisfiesUpperUnboundedInternalTimeQual(HeapTuple tuple,
												 InternalTimeQual qual)
{
	if (!AbsoluteTimeIsBackwardCompatiblyValid(tuple->t_tmin))
	{

		if (TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmin) &&
			CommandIdGEScanCommandId(tuple->t_cmin))
		{

			return (false);
		}

		if (TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmin) &&
			!CommandIdGEScanCommandId(tuple->t_cmin))
		{

			if (!TransactionIdIsValid((TransactionId) tuple->t_xmax))
			{
				return (true);
			}

			Assert(TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmax));

			return ((bool) ! CommandIdGEScanCommandId(tuple->t_cmax));
		}

		if (!TransactionIdDidCommit((TransactionId) tuple->t_xmin))
		{
			return (false);
		}

		tuple->t_tmin = TransactionIdGetCommitTime(tuple->t_xmin);
	}
	/* the tuple was inserted validly */

	if (!AbsoluteTimeIsBackwardCompatiblyValid(TimeQualGetStartTime((TimeQual) qual)))
	{
		return (true);
	}

	if (!AbsoluteTimeIsBackwardCompatiblyReal(tuple->t_tmax))
	{

		if (!TransactionIdIsValid((TransactionId) tuple->t_xmax))
		{
			return (true);
		}

		if (TransactionIdIsCurrentTransactionId((TransactionId) tuple->t_xmax))
		{
			return (CommandIdGEScanCommandId(tuple->t_cmin));
			/* it looks like error					  ^^^^ */
		}

		if (!TransactionIdDidCommit((TransactionId) tuple->t_xmax))
		{
			return (true);
		}

		tuple->t_tmax = TransactionIdGetCommitTime(tuple->t_xmax);
	}

	return ((bool) AbsoluteTimeIsAfter(tuple->t_tmax,
								 TimeQualGetStartTime((TimeQual) qual)));
}
