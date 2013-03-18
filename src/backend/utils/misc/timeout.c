/*-------------------------------------------------------------------------
 *
 * timeout.c
 *	  Routines to multiplex SIGALRM interrupts for multiple timeout reasons.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/timeout.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/time.h>

#include "storage/proc.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"


/* Data about any one timeout reason */
typedef struct timeout_params
{
	TimeoutId	index;			/* identifier of timeout reason */

	/* volatile because it may be changed from the signal handler */
	volatile bool indicator;	/* true if timeout has occurred */

	/* callback function for timeout, or NULL if timeout not registered */
	timeout_handler_proc timeout_handler;

	TimestampTz start_time;		/* time that timeout was last activated */
	TimestampTz fin_time;		/* if active, time it is due to fire */
} timeout_params;

/*
 * List of possible timeout reasons in the order of enum TimeoutId.
 */
static timeout_params all_timeouts[MAX_TIMEOUTS];
static bool all_timeouts_initialized = false;

/*
 * List of active timeouts ordered by their fin_time and priority.
 * This list is subject to change by the interrupt handler, so it's volatile.
 */
static volatile int num_active_timeouts = 0;
static timeout_params *volatile active_timeouts[MAX_TIMEOUTS];

/*
 * Flag controlling whether the signal handler is allowed to do anything.
 * We leave this "false" when we're not expecting interrupts, just in case.
 *
 * Note that we don't bother to reset any pending timer interrupt when we
 * disable the signal handler; it's not really worth the cycles to do so,
 * since the probability of the interrupt actually occurring while we have
 * it disabled is low.	See comments in schedule_alarm() about that.
 */
static volatile sig_atomic_t alarm_enabled = false;

#define disable_alarm() (alarm_enabled = false)
#define enable_alarm()	(alarm_enabled = true)


/*****************************************************************************
 * Internal helper functions
 *
 * For all of these, it is caller's responsibility to protect them from
 * interruption by the signal handler.	Generally, call disable_alarm()
 * first to prevent interruption, then update state, and last call
 * schedule_alarm(), which will re-enable the signal handler if needed.
 *****************************************************************************/

/*
 * Find the index of a given timeout reason in the active array.
 * If it's not there, return -1.
 */
static int
find_active_timeout(TimeoutId id)
{
	int			i;

	for (i = 0; i < num_active_timeouts; i++)
	{
		if (active_timeouts[i]->index == id)
			return i;
	}

	return -1;
}

/*
 * Insert specified timeout reason into the list of active timeouts
 * at the given index.
 */
static void
insert_timeout(TimeoutId id, int index)
{
	int			i;

	if (index < 0 || index > num_active_timeouts)
		elog(FATAL, "timeout index %d out of range 0..%d", index,
			 num_active_timeouts);

	for (i = num_active_timeouts - 1; i >= index; i--)
		active_timeouts[i + 1] = active_timeouts[i];

	active_timeouts[index] = &all_timeouts[id];

	num_active_timeouts++;
}

/*
 * Remove the index'th element from the timeout list.
 */
static void
remove_timeout_index(int index)
{
	int			i;

	if (index < 0 || index >= num_active_timeouts)
		elog(FATAL, "timeout index %d out of range 0..%d", index,
			 num_active_timeouts - 1);

	for (i = index + 1; i < num_active_timeouts; i++)
		active_timeouts[i - 1] = active_timeouts[i];

	num_active_timeouts--;
}

/*
 * Enable the specified timeout reason
 */
static void
enable_timeout(TimeoutId id, TimestampTz now, TimestampTz fin_time)
{
	int			i;

	/* Assert request is sane */
	Assert(all_timeouts_initialized);
	Assert(all_timeouts[id].timeout_handler != NULL);

	/*
	 * If this timeout was already active, momentarily disable it.	We
	 * interpret the call as a directive to reschedule the timeout.
	 */
	i = find_active_timeout(id);
	if (i >= 0)
		remove_timeout_index(i);

	/*
	 * Find out the index where to insert the new timeout.	We sort by
	 * fin_time, and for equal fin_time by priority.
	 */
	for (i = 0; i < num_active_timeouts; i++)
	{
		timeout_params *old_timeout = active_timeouts[i];

		if (fin_time < old_timeout->fin_time)
			break;
		if (fin_time == old_timeout->fin_time && id < old_timeout->index)
			break;
	}

	/*
	 * Mark the timeout active, and insert it into the active list.
	 */
	all_timeouts[id].indicator = false;
	all_timeouts[id].start_time = now;
	all_timeouts[id].fin_time = fin_time;

	insert_timeout(id, i);
}

/*
 * Schedule alarm for the next active timeout, if any
 *
 * We assume the caller has obtained the current time, or a close-enough
 * approximation.
 */
static void
schedule_alarm(TimestampTz now)
{
	if (num_active_timeouts > 0)
	{
		struct itimerval timeval;
		long		secs;
		int			usecs;

		MemSet(&timeval, 0, sizeof(struct itimerval));

		/* Get the time remaining till the nearest pending timeout */
		TimestampDifference(now, active_timeouts[0]->fin_time,
							&secs, &usecs);

		/*
		 * It's possible that the difference is less than a microsecond;
		 * ensure we don't cancel, rather than set, the interrupt.
		 */
		if (secs == 0 && usecs == 0)
			usecs = 1;

		timeval.it_value.tv_sec = secs;
		timeval.it_value.tv_usec = usecs;

		/*
		 * We must enable the signal handler before calling setitimer(); if we
		 * did it in the other order, we'd have a race condition wherein the
		 * interrupt could occur before we can set alarm_enabled, so that the
		 * signal handler would fail to do anything.
		 *
		 * Because we didn't bother to reset the timer in disable_alarm(),
		 * it's possible that a previously-set interrupt will fire between
		 * enable_alarm() and setitimer().	This is safe, however.	There are
		 * two possible outcomes:
		 *
		 * 1. The signal handler finds nothing to do (because the nearest
		 * timeout event is still in the future).  It will re-set the timer
		 * and return.	Then we'll overwrite the timer value with a new one.
		 * This will mean that the timer fires a little later than we
		 * intended, but only by the amount of time it takes for the signal
		 * handler to do nothing useful, which shouldn't be much.
		 *
		 * 2. The signal handler executes and removes one or more timeout
		 * events.	When it returns, either the queue is now empty or the
		 * frontmost event is later than the one we looked at above.  So we'll
		 * overwrite the timer value with one that is too soon (plus or minus
		 * the signal handler's execution time), causing a useless interrupt
		 * to occur.  But the handler will then re-set the timer and
		 * everything will still work as expected.
		 *
		 * Since these cases are of very low probability (the window here
		 * being quite narrow), it's not worth adding cycles to the mainline
		 * code to prevent occasional wasted interrupts.
		 */
		enable_alarm();

		/* Set the alarm timer */
		if (setitimer(ITIMER_REAL, &timeval, NULL) != 0)
			elog(FATAL, "could not enable SIGALRM timer: %m");
	}
}


/*****************************************************************************
 * Signal handler
 *****************************************************************************/

/*
 * Signal handler for SIGALRM
 *
 * Process any active timeout reasons and then reschedule the interrupt
 * as needed.
 */
static void
handle_sig_alarm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	/*
	 * SIGALRM is always cause for waking anything waiting on the process
	 * latch.  Cope with MyProc not being there, as the startup process also
	 * uses this signal handler.
	 */
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	/*
	 * Fire any pending timeouts, but only if we're enabled to do so.
	 */
	if (alarm_enabled)
	{
		/*
		 * Disable alarms, just in case this platform allows signal handlers
		 * to interrupt themselves.  schedule_alarm() will re-enable if
		 * appropriate.
		 */
		disable_alarm();

		if (num_active_timeouts > 0)
		{
			TimestampTz now = GetCurrentTimestamp();

			/* While the first pending timeout has been reached ... */
			while (num_active_timeouts > 0 &&
				   now >= active_timeouts[0]->fin_time)
			{
				timeout_params *this_timeout = active_timeouts[0];

				/* Remove it from the active list */
				remove_timeout_index(0);

				/* Mark it as fired */
				this_timeout->indicator = true;

				/* And call its handler function */
				(*this_timeout->timeout_handler) ();

				/*
				 * The handler might not take negligible time (CheckDeadLock
				 * for instance isn't too cheap), so let's update our idea of
				 * "now" after each one.
				 */
				now = GetCurrentTimestamp();
			}

			/* Done firing timeouts, so reschedule next interrupt if any */
			schedule_alarm(now);
		}
	}

	errno = save_errno;
}


/*****************************************************************************
 * Public API
 *****************************************************************************/

/*
 * Initialize timeout module.
 *
 * This must be called in every process that wants to use timeouts.
 *
 * If the process was forked from another one that was also using this
 * module, be sure to call this before re-enabling signals; else handlers
 * meant to run in the parent process might get invoked in this one.
 */
void
InitializeTimeouts(void)
{
	int			i;

	/* Initialize, or re-initialize, all local state */
	disable_alarm();

	num_active_timeouts = 0;

	for (i = 0; i < MAX_TIMEOUTS; i++)
	{
		all_timeouts[i].index = i;
		all_timeouts[i].indicator = false;
		all_timeouts[i].timeout_handler = NULL;
		all_timeouts[i].start_time = 0;
		all_timeouts[i].fin_time = 0;
	}

	all_timeouts_initialized = true;

	/* Now establish the signal handler */
	pqsignal(SIGALRM, handle_sig_alarm);
}

/*
 * Register a timeout reason
 *
 * For predefined timeouts, this just registers the callback function.
 *
 * For user-defined timeouts, pass id == USER_TIMEOUT; we then allocate and
 * return a timeout ID.
 */
TimeoutId
RegisterTimeout(TimeoutId id, timeout_handler_proc handler)
{
	Assert(all_timeouts_initialized);

	/* There's no need to disable the signal handler here. */

	if (id >= USER_TIMEOUT)
	{
		/* Allocate a user-defined timeout reason */
		for (id = USER_TIMEOUT; id < MAX_TIMEOUTS; id++)
			if (all_timeouts[id].timeout_handler == NULL)
				break;
		if (id >= MAX_TIMEOUTS)
			ereport(FATAL,
					(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
					 errmsg("cannot add more timeout reasons")));
	}

	Assert(all_timeouts[id].timeout_handler == NULL);

	all_timeouts[id].timeout_handler = handler;

	return id;
}

/*
 * Enable the specified timeout to fire after the specified delay.
 *
 * Delay is given in milliseconds.
 */
void
enable_timeout_after(TimeoutId id, int delay_ms)
{
	TimestampTz now;
	TimestampTz fin_time;

	/* Disable timeout interrupts for safety. */
	disable_alarm();

	/* Queue the timeout at the appropriate time. */
	now = GetCurrentTimestamp();
	fin_time = TimestampTzPlusMilliseconds(now, delay_ms);
	enable_timeout(id, now, fin_time);

	/* Set the timer interrupt. */
	schedule_alarm(now);
}

/*
 * Enable the specified timeout to fire at the specified time.
 *
 * This is provided to support cases where there's a reason to calculate
 * the timeout by reference to some point other than "now".  If there isn't,
 * use enable_timeout_after(), to avoid calling GetCurrentTimestamp() twice.
 */
void
enable_timeout_at(TimeoutId id, TimestampTz fin_time)
{
	TimestampTz now;

	/* Disable timeout interrupts for safety. */
	disable_alarm();

	/* Queue the timeout at the appropriate time. */
	now = GetCurrentTimestamp();
	enable_timeout(id, now, fin_time);

	/* Set the timer interrupt. */
	schedule_alarm(now);
}

/*
 * Enable multiple timeouts at once.
 *
 * This works like calling enable_timeout_after() and/or enable_timeout_at()
 * multiple times.	Use this to reduce the number of GetCurrentTimestamp()
 * and setitimer() calls needed to establish multiple timeouts.
 */
void
enable_timeouts(const EnableTimeoutParams *timeouts, int count)
{
	TimestampTz now;
	int			i;

	/* Disable timeout interrupts for safety. */
	disable_alarm();

	/* Queue the timeout(s) at the appropriate times. */
	now = GetCurrentTimestamp();

	for (i = 0; i < count; i++)
	{
		TimeoutId	id = timeouts[i].id;
		TimestampTz fin_time;

		switch (timeouts[i].type)
		{
			case TMPARAM_AFTER:
				fin_time = TimestampTzPlusMilliseconds(now,
													   timeouts[i].delay_ms);
				enable_timeout(id, now, fin_time);
				break;

			case TMPARAM_AT:
				enable_timeout(id, now, timeouts[i].fin_time);
				break;

			default:
				elog(ERROR, "unrecognized timeout type %d",
					 (int) timeouts[i].type);
				break;
		}
	}

	/* Set the timer interrupt. */
	schedule_alarm(now);
}

/*
 * Cancel the specified timeout.
 *
 * The timeout's I've-been-fired indicator is reset,
 * unless keep_indicator is true.
 *
 * When a timeout is canceled, any other active timeout remains in force.
 * It's not an error to disable a timeout that is not enabled.
 */
void
disable_timeout(TimeoutId id, bool keep_indicator)
{
	int			i;

	/* Assert request is sane */
	Assert(all_timeouts_initialized);
	Assert(all_timeouts[id].timeout_handler != NULL);

	/* Disable timeout interrupts for safety. */
	disable_alarm();

	/* Find the timeout and remove it from the active list. */
	i = find_active_timeout(id);
	if (i >= 0)
		remove_timeout_index(i);

	/* Mark it inactive, whether it was active or not. */
	if (!keep_indicator)
		all_timeouts[id].indicator = false;

	/* Reschedule the interrupt, if any timeouts remain active. */
	if (num_active_timeouts > 0)
		schedule_alarm(GetCurrentTimestamp());
}

/*
 * Cancel multiple timeouts at once.
 *
 * The timeouts' I've-been-fired indicators are reset,
 * unless timeouts[i].keep_indicator is true.
 *
 * This works like calling disable_timeout() multiple times.
 * Use this to reduce the number of GetCurrentTimestamp()
 * and setitimer() calls needed to cancel multiple timeouts.
 */
void
disable_timeouts(const DisableTimeoutParams *timeouts, int count)
{
	int			i;

	Assert(all_timeouts_initialized);

	/* Disable timeout interrupts for safety. */
	disable_alarm();

	/* Cancel the timeout(s). */
	for (i = 0; i < count; i++)
	{
		TimeoutId	id = timeouts[i].id;
		int			idx;

		Assert(all_timeouts[id].timeout_handler != NULL);

		idx = find_active_timeout(id);
		if (idx >= 0)
			remove_timeout_index(idx);

		if (!timeouts[i].keep_indicator)
			all_timeouts[id].indicator = false;
	}

	/* Reschedule the interrupt, if any timeouts remain active. */
	if (num_active_timeouts > 0)
		schedule_alarm(GetCurrentTimestamp());
}

/*
 * Disable SIGALRM and remove all timeouts from the active list,
 * and optionally reset their timeout indicators.
 */
void
disable_all_timeouts(bool keep_indicators)
{
	disable_alarm();

	/*
	 * Only bother to reset the timer if we think it's active.  We could just
	 * let the interrupt happen anyway, but it's probably a bit cheaper to do
	 * setitimer() than to let the useless interrupt happen.
	 */
	if (num_active_timeouts > 0)
	{
		struct itimerval timeval;

		MemSet(&timeval, 0, sizeof(struct itimerval));
		if (setitimer(ITIMER_REAL, &timeval, NULL) != 0)
			elog(FATAL, "could not disable SIGALRM timer: %m");
	}

	num_active_timeouts = 0;

	if (!keep_indicators)
	{
		int			i;

		for (i = 0; i < MAX_TIMEOUTS; i++)
			all_timeouts[i].indicator = false;
	}
}

/*
 * Return the timeout's I've-been-fired indicator
 *
 * If reset_indicator is true, reset the indicator when returning true.
 * To avoid missing timeouts due to race conditions, we are careful not to
 * reset the indicator when returning false.
 */
bool
get_timeout_indicator(TimeoutId id, bool reset_indicator)
{
	if (all_timeouts[id].indicator)
	{
		if (reset_indicator)
			all_timeouts[id].indicator = false;
		return true;
	}
	return false;
}

/*
 * Return the time when the timeout was most recently activated
 *
 * Note: will return 0 if timeout has never been activated in this process.
 * However, we do *not* reset the start_time when a timeout occurs, so as
 * not to create a race condition if SIGALRM fires just as some code is
 * about to fetch the value.
 */
TimestampTz
get_timeout_start_time(TimeoutId id)
{
	return all_timeouts[id].start_time;
}
