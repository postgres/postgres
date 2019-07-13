/*-------------------------------------------------------------------------
 *
 * condition_variable.h
 *	  Condition variables
 *
 * A condition variable is a method of waiting until a certain condition
 * becomes true.  Conventionally, a condition variable supports three
 * operations: (1) sleep; (2) signal, which wakes up one process sleeping
 * on the condition variable; and (3) broadcast, which wakes up every
 * process sleeping on the condition variable.  In our implementation,
 * condition variables put a process into an interruptible sleep (so it
 * can be cancelled prior to the fulfillment of the condition) and do not
 * use pointers internally (so that they are safe to use within DSMs).
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/condition_variable.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CONDITION_VARIABLE_H
#define CONDITION_VARIABLE_H

#include "storage/s_lock.h"
#include "storage/proclist_types.h"

typedef struct
{
	slock_t		mutex;			/* spinlock protecting the wakeup list */
	proclist_head wakeup;		/* list of wake-able processes */
} ConditionVariable;

/* Initialize a condition variable. */
extern void ConditionVariableInit(ConditionVariable *cv);

/*
 * To sleep on a condition variable, a process should use a loop which first
 * checks the condition, exiting the loop if it is met, and then calls
 * ConditionVariableSleep.  Spurious wakeups are possible, but should be
 * infrequent.  After exiting the loop, ConditionVariableCancelSleep must
 * be called to ensure that the process is no longer in the wait list for
 * the condition variable.
 */
extern void ConditionVariableSleep(ConditionVariable *cv, uint32 wait_event_info);
extern bool ConditionVariableTimedSleep(ConditionVariable *cv, long timeout,
										uint32 wait_event_info);
extern void ConditionVariableCancelSleep(void);

/*
 * Optionally, ConditionVariablePrepareToSleep can be called before entering
 * the test-and-sleep loop described above.  Doing so is more efficient if
 * at least one sleep is needed, whereas not doing so is more efficient when
 * no sleep is needed because the test condition is true the first time.
 */
extern void ConditionVariablePrepareToSleep(ConditionVariable *cv);

/* Wake up a single waiter (via signal) or all waiters (via broadcast). */
extern void ConditionVariableSignal(ConditionVariable *cv);
extern void ConditionVariableBroadcast(ConditionVariable *cv);

#endif							/* CONDITION_VARIABLE_H */
