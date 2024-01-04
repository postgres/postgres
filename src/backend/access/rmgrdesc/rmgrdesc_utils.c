/*-------------------------------------------------------------------------
 *
 * rmgrdesc_utils.c
 *	  Support functions for rmgrdesc routines
 *
 * Copyright (c) 2023-2024, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/rmgrdesc_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/rmgrdesc_utils.h"
#include "storage/off.h"

/*
 * Helper function to print an array, in the format described in the
 * README.
 */
void
array_desc(StringInfo buf, void *array, size_t elem_size, int count,
		   void (*elem_desc) (StringInfo buf, void *elem, void *data),
		   void *data)
{
	if (count == 0)
	{
		appendStringInfoString(buf, " []");
		return;
	}
	appendStringInfoString(buf, " [");
	for (int i = 0; i < count; i++)
	{
		elem_desc(buf, (char *) array + elem_size * i, data);
		if (i < count - 1)
			appendStringInfoString(buf, ", ");
	}
	appendStringInfoChar(buf, ']');
}

void
offset_elem_desc(StringInfo buf, void *offset, void *data)
{
	appendStringInfo(buf, "%u", *(OffsetNumber *) offset);
}

void
redirect_elem_desc(StringInfo buf, void *offset, void *data)
{
	OffsetNumber *new_offset = (OffsetNumber *) offset;

	appendStringInfo(buf, "%u->%u", new_offset[0], new_offset[1]);
}

void
oid_elem_desc(StringInfo buf, void *relid, void *data)
{
	appendStringInfo(buf, "%u", *(Oid *) relid);
}
