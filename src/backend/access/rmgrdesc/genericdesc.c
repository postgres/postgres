/*-------------------------------------------------------------------------
 *
 * genericdesc.c
 *	  rmgr descriptor routines for access/transam/generic_xlog.c
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/rmgrdesc/genericdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "lib/stringinfo.h"

/*
 * Description of generic xlog record: write page regions that this record
 * overrides.
 */
void
generic_desc(StringInfo buf, XLogReaderState *record)
{
	Pointer		ptr = XLogRecGetData(record),
				end = ptr + XLogRecGetDataLen(record);

	while (ptr < end)
	{
		OffsetNumber offset,
					length;

		memcpy(&offset, ptr, sizeof(offset));
		ptr += sizeof(offset);
		memcpy(&length, ptr, sizeof(length));
		ptr += sizeof(length);
		ptr += length;

		if (ptr < end)
			appendStringInfo(buf, "offset %u, length %u; ", offset, length);
		else
			appendStringInfo(buf, "offset %u, length %u", offset, length);
	}
}

/*
 * Identification of generic xlog record: we don't distinguish any subtypes
 * inside generic xlog records.
 */
const char *
generic_identify(uint8 info)
{
	return "Generic";
}
