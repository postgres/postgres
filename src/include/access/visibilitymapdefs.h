/*-------------------------------------------------------------------------
 *
 * visibilitymapdefs.h
 *		macros for accessing contents of visibility map pages
 *
 *
 * Copyright (c) 2021, PostgreSQL Global Development Group
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

#endif							/* VISIBILITYMAPDEFS_H */
