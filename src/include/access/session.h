/*-------------------------------------------------------------------------
 *
 * session.h
 *	  Encapsulation of user session.
 *
 * Copyright (c) 2017-2025, PostgreSQL Global Development Group
 *
 * src/include/access/session.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SESSION_H
#define SESSION_H

#include "lib/dshash.h"

/* Avoid including typcache.h */
struct SharedRecordTypmodRegistry;

/*
 * A struct encapsulating some elements of a user's session.  For now this
 * manages state that applies to parallel query, but in principle it could
 * include other things that are currently global variables.
 */
typedef struct Session
{
	dsm_segment *segment;		/* The session-scoped DSM segment. */
	dsa_area   *area;			/* The session-scoped DSA area. */

	/* State managed by typcache.c. */
	struct SharedRecordTypmodRegistry *shared_typmod_registry;
	dshash_table *shared_record_table;
	dshash_table *shared_typmod_table;
} Session;

extern void InitializeSession(void);
extern dsm_handle GetSessionDsmHandle(void);
extern void AttachSession(dsm_handle handle);
extern void DetachSession(void);

/* The current session, or NULL for none. */
extern PGDLLIMPORT Session *CurrentSession;

#endif							/* SESSION_H */
