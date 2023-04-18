/*-------------------------------------------------------------------------
 *
 * rmgrdesc_utils.h
 *	  Support functions for rmgrdesc routines
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
 *
 * src/include/access/rmgrdesc_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RMGRDESC_UTILS_H_
#define RMGRDESC_UTILS_H_

/*
 * Guidelines for rmgrdesc routine authors:
 *
 * The goal of these guidelines is to avoid gratuitous inconsistencies across
 * each rmgr, and to allow users to parse desc output strings without too much
 * difficulty.  This is not an API specification or an interchange format.
 * (Only heapam and nbtree desc routines follow these guidelines at present,
 * in any case.)
 *
 * Record descriptions are similar to JSON style key/value objects.  However,
 * there is no explicit "string" type/string escaping.  Top-level { } brackets
 * should be omitted.  For example:
 *
 * snapshotConflictHorizon: 0, flags: 0x03
 *
 * Record descriptions may contain variable-length arrays.  For example:
 *
 * nunused: 5, unused: [1, 2, 3, 4, 5]
 *
 * Nested objects are supported via { } brackets.  They generally appear
 * inside variable-length arrays.  For example:
 *
 * ndeleted: 0, nupdated: 1, deleted: [], updated: [{ off: 45, nptids: 1, ptids: [0] }]
 *
 * Try to output things in an order that faithfully represents the order of
 * fields from the underlying physical WAL record struct.  Key names should be
 * unique (at the same nesting level) to make parsing easy.  It's a good idea
 * if the number of items in the array appears before the array.
 *
 * It's okay for individual WAL record types to invent their own conventions.
 * For example, Heap2's PRUNE record descriptions use a custom array format
 * for the record's "redirected" field:
 *
 * ... redirected: [1->4, 5->9], dead: [10, 11], unused: [3, 7, 8]
 *
 * Arguably the desc routine should be using object notation for this instead.
 * However, there is value in using a custom format when it conveys useful
 * information about the underlying physical data structures.
 *
 * This ad-hoc format has the advantage of being close to the format used for
 * the "dead" and "unused" arrays (which follow the standard desc convention
 * for page offset number arrays).  It suggests that the "redirected" elements
 * shown are just pairs of page offset numbers (which is how it really works).
 */
extern void array_desc(StringInfo buf, void *array, size_t elem_size, int count,
					   void (*elem_desc) (StringInfo buf, void *elem, void *data),
					   void *data);
extern void offset_elem_desc(StringInfo buf, void *offset, void *data);
extern void redirect_elem_desc(StringInfo buf, void *offset, void *data);
extern void oid_elem_desc(StringInfo buf, void *relid, void *data);

#endif							/* RMGRDESC_UTILS_H_ */
