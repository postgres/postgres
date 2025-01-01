/*-------------------------------------------------------------------------
 *
 * rmgrdesc_utils.h
 *	  Support functions for rmgrdesc routines
 *
 * Copyright (c) 2023-2025, PostgreSQL Global Development Group
 *
 * src/include/access/rmgrdesc_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RMGRDESC_UTILS_H_
#define RMGRDESC_UTILS_H_

extern void array_desc(StringInfo buf, void *array, size_t elem_size, int count,
					   void (*elem_desc) (StringInfo buf, void *elem, void *data),
					   void *data);
extern void offset_elem_desc(StringInfo buf, void *offset, void *data);
extern void redirect_elem_desc(StringInfo buf, void *offset, void *data);
extern void oid_elem_desc(StringInfo buf, void *relid, void *data);

#endif							/* RMGRDESC_UTILS_H_ */
