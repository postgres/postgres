/*-------------------------------------------------------------------------
 *
 * visibilitymapdefs.h
 *		macros for accessing contents of visibility map pages
 *
 *
 * Copyright (c) 2021-2023, PostgreSQL Global Development Group
 *
 * src/include/access/visibilitymapdefs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VISIBILITYMAPDEFS_H
#define VISIBILITYMAPDEFS_H

/* Number of bits for one heap page */
#define BITS_PER_HEAPBLOCK 2

/* Flags for bit map */
#define VISIBILITYMAP_ALL_VISIBLE	0x01
#define VISIBILITYMAP_ALL_FROZEN	0x02
#define VISIBILITYMAP_VALID_BITS	0x03	/* OR of all valid visibilitymap
											 * flags bits */
/*
 * To detect recovery conflicts during logical decoding on a standby, we need
 * to know if a table is a user catalog table. For that we add an additional
 * bit into xl_heap_visible.flags, in addition to the above.
 *
 * NB: VISIBILITYMAP_XLOG_* may not be passed to visibilitymap_set().
 */
#define VISIBILITYMAP_XLOG_CATALOG_REL	0x04
#define VISIBILITYMAP_XLOG_VALID_BITS	(VISIBILITYMAP_VALID_BITS | VISIBILITYMAP_XLOG_CATALOG_REL)

#endif							/* VISIBILITYMAPDEFS_H */
