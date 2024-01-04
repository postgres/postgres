/*-------------------------------------------------------------------------
 *
 * basebackup_throttle.c
 *	  Basebackup sink implementing throttling. Data is forwarded to the
 *	  next base backup sink in the chain at a rate no greater than the
 *	  configured maximum.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_throttle.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "backup/basebackup_sink.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"
#include "utils/timestamp.h"

typedef struct bbsink_throttle
{
	/* Common information for all types of sink. */
	bbsink		base;

	/* The actual number of bytes, transfer of which may cause sleep. */
	uint64		throttling_sample;

	/* Amount of data already transferred but not yet throttled.  */
	int64		throttling_counter;

	/* The minimum time required to transfer throttling_sample bytes. */
	TimeOffset	elapsed_min_unit;

	/* The last check of the transfer rate. */
	TimestampTz throttled_last;
} bbsink_throttle;

static void bbsink_throttle_begin_backup(bbsink *sink);
static void bbsink_throttle_archive_contents(bbsink *sink, size_t len);
static void bbsink_throttle_manifest_contents(bbsink *sink, size_t len);
static void throttle(bbsink_throttle *sink, size_t increment);

static const bbsink_ops bbsink_throttle_ops = {
	.begin_backup = bbsink_throttle_begin_backup,
	.begin_archive = bbsink_forward_begin_archive,
	.archive_contents = bbsink_throttle_archive_contents,
	.end_archive = bbsink_forward_end_archive,
	.begin_manifest = bbsink_forward_begin_manifest,
	.manifest_contents = bbsink_throttle_manifest_contents,
	.end_manifest = bbsink_forward_end_manifest,
	.end_backup = bbsink_forward_end_backup,
	.cleanup = bbsink_forward_cleanup
};

/*
 * How frequently to throttle, as a fraction of the specified rate-second.
 */
#define THROTTLING_FREQUENCY	8

/*
 * Create a new basebackup sink that performs throttling and forwards data
 * to a successor sink.
 */
bbsink *
bbsink_throttle_new(bbsink *next, uint32 maxrate)
{
	bbsink_throttle *sink;

	Assert(next != NULL);
	Assert(maxrate > 0);

	sink = palloc0(sizeof(bbsink_throttle));
	*((const bbsink_ops **) &sink->base.bbs_ops) = &bbsink_throttle_ops;
	sink->base.bbs_next = next;

	sink->throttling_sample =
		(int64) maxrate * (int64) 1024 / THROTTLING_FREQUENCY;

	/*
	 * The minimum amount of time for throttling_sample bytes to be
	 * transferred.
	 */
	sink->elapsed_min_unit = USECS_PER_SEC / THROTTLING_FREQUENCY;

	return &sink->base;
}

/*
 * There's no real work to do here, but we need to record the current time so
 * that it can be used for future calculations.
 */
static void
bbsink_throttle_begin_backup(bbsink *sink)
{
	bbsink_throttle *mysink = (bbsink_throttle *) sink;

	bbsink_forward_begin_backup(sink);

	/* The 'real data' starts now (header was ignored). */
	mysink->throttled_last = GetCurrentTimestamp();
}

/*
 * First throttle, and then pass archive contents to next sink.
 */
static void
bbsink_throttle_archive_contents(bbsink *sink, size_t len)
{
	throttle((bbsink_throttle *) sink, len);

	bbsink_forward_archive_contents(sink, len);
}

/*
 * First throttle, and then pass manifest contents to next sink.
 */
static void
bbsink_throttle_manifest_contents(bbsink *sink, size_t len)
{
	throttle((bbsink_throttle *) sink, len);

	bbsink_forward_manifest_contents(sink, len);
}

/*
 * Increment the network transfer counter by the given number of bytes,
 * and sleep if necessary to comply with the requested network transfer
 * rate.
 */
static void
throttle(bbsink_throttle *sink, size_t increment)
{
	TimeOffset	elapsed_min;

	Assert(sink->throttling_counter >= 0);

	sink->throttling_counter += increment;
	if (sink->throttling_counter < sink->throttling_sample)
		return;

	/* How much time should have elapsed at minimum? */
	elapsed_min = sink->elapsed_min_unit *
		(sink->throttling_counter / sink->throttling_sample);

	/*
	 * Since the latch could be set repeatedly because of concurrently WAL
	 * activity, sleep in a loop to ensure enough time has passed.
	 */
	for (;;)
	{
		TimeOffset	elapsed,
					sleep;
		int			wait_result;

		/* Time elapsed since the last measurement (and possible wake up). */
		elapsed = GetCurrentTimestamp() - sink->throttled_last;

		/* sleep if the transfer is faster than it should be */
		sleep = elapsed_min - elapsed;
		if (sleep <= 0)
			break;

		ResetLatch(MyLatch);

		/* We're eating a potentially set latch, so check for interrupts */
		CHECK_FOR_INTERRUPTS();

		/*
		 * (TAR_SEND_SIZE / throttling_sample * elapsed_min_unit) should be
		 * the maximum time to sleep. Thus the cast to long is safe.
		 */
		wait_result = WaitLatch(MyLatch,
								WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								(long) (sleep / 1000),
								WAIT_EVENT_BASE_BACKUP_THROTTLE);

		if (wait_result & WL_LATCH_SET)
			CHECK_FOR_INTERRUPTS();

		/* Done waiting? */
		if (wait_result & WL_TIMEOUT)
			break;
	}

	/*
	 * As we work with integers, only whole multiple of throttling_sample was
	 * processed. The rest will be done during the next call of this function.
	 */
	sink->throttling_counter %= sink->throttling_sample;

	/*
	 * Time interval for the remaining amount and possible next increments
	 * starts now.
	 */
	sink->throttled_last = GetCurrentTimestamp();
}
