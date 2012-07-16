/*-------------------------------------------------------------------------
 *
 * timeout.h
 *	  Routines to multiplex SIGALRM interrupts for multiple timeout reasons.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/timeout.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TIMEOUT_H
#define TIMEOUT_H

#include "datatype/timestamp.h"

/*
 * Identifiers for timeout reasons.  Note that in case multiple timeouts
 * trigger at the same time, they are serviced in the order of this enum.
 */
typedef enum TimeoutId
{
	/* Predefined timeout reasons */
	STARTUP_PACKET_TIMEOUT,
	DEADLOCK_TIMEOUT,
	STATEMENT_TIMEOUT,
	STANDBY_DEADLOCK_TIMEOUT,
	STANDBY_TIMEOUT,
	/* First user-definable timeout reason */
	USER_TIMEOUT,
	/* Maximum number of timeout reasons */
	MAX_TIMEOUTS = 16
} TimeoutId;

/* callback function signature */
typedef void (*timeout_handler) (void);

/* timeout setup */
extern void InitializeTimeouts(void);
extern TimeoutId RegisterTimeout(TimeoutId id, timeout_handler handler);

/* timeout operation */
extern void enable_timeout_after(TimeoutId id, int delay_ms);
extern void enable_timeout_at(TimeoutId id, TimestampTz fin_time);
extern void disable_timeout(TimeoutId id, bool keep_indicator);
extern void disable_all_timeouts(bool keep_indicators);

/* accessors */
extern bool get_timeout_indicator(TimeoutId id);
extern TimestampTz get_timeout_start_time(TimeoutId id);

#endif   /* TIMEOUT_H */
