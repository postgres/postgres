/*-------------------------------------------------------------------------
 *
 * rmgrdesc_utils.c
 *	  Support functions for rmgrdesc routines
 *
 * Copyright (c) 2023, PostgreSQL Global Development Group
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
 * Guidelines for formatting desc functions:
 *
 * member1_name: member1_value, member2_name: member2_value
 *
 * If the value is a list, please use:
 *
 * member3_name: [ member3_list_value1, member3_list_value2 ]
 *
 * The first item appended to the string should not be prepended by any spaces
 * or comma, however all subsequent appends to the string are responsible for
 * prepending themselves with a comma followed by a space.
 *
 * Arrays should have a space between the opening square bracket and first
 * element and between the last element and closing brace.
 *
 * Flags should be in ALL CAPS.
 *
 * For lists/arrays of items, the number of those items should be listed at
 * the beginning with all of the other numbers.
 *
 * List punctuation should still be included even if there are 0 items.
 *
 * Composite objects in a list should be surrounded with { }.
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
	appendStringInfo(buf, " [");
	for (int i = 0; i < count; i++)
	{
		if (i > 0)
			appendStringInfoString(buf, ",");
		appendStringInfoString(buf, " ");

		elem_desc(buf, (char *) array + elem_size * i, data);
	}
	appendStringInfoString(buf, " ]");
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
relid_desc(StringInfo buf, void *relid, void *data)
{
	appendStringInfo(buf, "%u", *(Oid *) relid);
}

void
uint16_elem_desc(StringInfo buf, void *value, void *data)
{
	appendStringInfo(buf, "%u", *(uint16 *) value);
}
