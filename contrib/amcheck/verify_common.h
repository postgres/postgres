/*-------------------------------------------------------------------------
 *
 * verify_common.h
 *		Shared routines for amcheck verifications.
 *
 * Copyright (c) 2016-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/amcheck/verify_common.h
 *
 *-------------------------------------------------------------------------
 */
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/lockdefs.h"
#include "utils/relcache.h"
#include "miscadmin.h"

/* Typedef for callback function for amcheck_lock_relation_and_check */
typedef void (*IndexDoCheckCallback) (Relation rel,
									  Relation heaprel,
									  void *state,
									  bool readonly);

extern void amcheck_lock_relation_and_check(Oid indrelid,
											Oid am_id,
											IndexDoCheckCallback check,
											LOCKMODE lockmode, void *state);
