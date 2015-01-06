/*-------------------------------------------------------------------------
 *
 * arch-arm.h
 *	  Atomic operations considerations specific to ARM
 *
 * Portions Copyright (c) 2013-2015, PostgreSQL Global Development Group
 *
 * NOTES:
 *
 * src/include/port/atomics/arch-arm.h
 *
 *-------------------------------------------------------------------------
 */

/* intentionally no include guards, should only be included by atomics.h */
#ifndef INSIDE_ATOMICS_H
#error "should be included via atomics.h"
#endif

/*
 * 64 bit atomics on arm are implemented using kernel fallbacks and might be
 * slow, so disable entirely for now.
 * XXX: We might want to change that at some point for AARCH64
 */
#define PG_DISABLE_64_BIT_ATOMICS
