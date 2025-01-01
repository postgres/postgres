/*-------------------------------------------------------------------------
 *
 * printsimple.c
 *	  Routines to print out tuples containing only a limited range of
 *	  builtin types without catalog access.  This is intended for
 *	  backends that don't have catalog access because they are not bound
 *	  to a specific database, such as some walsender processes.  It
 *	  doesn't handle standalone backends or protocol versions other than
 *	  3.0, because we don't need such handling for current applications.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/printsimple.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/printsimple.h"
#include "catalog/pg_type.h"
#include "libpq/pqformat.h"
#include "libpq/protocol.h"
#include "utils/builtins.h"

/*
 * At startup time, send a RowDescription message.
 */
void
printsimple_startup(DestReceiver *self, int operation, TupleDesc tupdesc)
{
	StringInfoData buf;
	int			i;

	pq_beginmessage(&buf, PqMsg_RowDescription);
	pq_sendint16(&buf, tupdesc->natts);

	for (i = 0; i < tupdesc->natts; ++i)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		pq_sendstring(&buf, NameStr(attr->attname));
		pq_sendint32(&buf, 0);	/* table oid */
		pq_sendint16(&buf, 0);	/* attnum */
		pq_sendint32(&buf, (int) attr->atttypid);
		pq_sendint16(&buf, attr->attlen);
		pq_sendint32(&buf, attr->atttypmod);
		pq_sendint16(&buf, 0);	/* format code */
	}

	pq_endmessage(&buf);
}

/*
 * For each tuple, send a DataRow message.
 */
bool
printsimple(TupleTableSlot *slot, DestReceiver *self)
{
	TupleDesc	tupdesc = slot->tts_tupleDescriptor;
	StringInfoData buf;
	int			i;

	/* Make sure the tuple is fully deconstructed */
	slot_getallattrs(slot);

	/* Prepare and send message */
	pq_beginmessage(&buf, PqMsg_DataRow);
	pq_sendint16(&buf, tupdesc->natts);

	for (i = 0; i < tupdesc->natts; ++i)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);
		Datum		value;

		if (slot->tts_isnull[i])
		{
			pq_sendint32(&buf, -1);
			continue;
		}

		value = slot->tts_values[i];

		/*
		 * We can't call the regular type output functions here because we
		 * might not have catalog access.  Instead, we must hard-wire
		 * knowledge of the required types.
		 */
		switch (attr->atttypid)
		{
			case TEXTOID:
				{
					text	   *t = DatumGetTextPP(value);

					pq_sendcountedtext(&buf,
									   VARDATA_ANY(t),
									   VARSIZE_ANY_EXHDR(t));
				}
				break;

			case INT4OID:
				{
					int32		num = DatumGetInt32(value);
					char		str[12];	/* sign, 10 digits and '\0' */
					int			len;

					len = pg_ltoa(num, str);
					pq_sendcountedtext(&buf, str, len);
				}
				break;

			case INT8OID:
				{
					int64		num = DatumGetInt64(value);
					char		str[MAXINT8LEN + 1];
					int			len;

					len = pg_lltoa(num, str);
					pq_sendcountedtext(&buf, str, len);
				}
				break;

			case OIDOID:
				{
					Oid			num = ObjectIdGetDatum(value);
					char		str[10];	/* 10 digits */
					int			len;

					len = pg_ultoa_n(num, str);
					pq_sendcountedtext(&buf, str, len);
				}
				break;

			default:
				elog(ERROR, "unsupported type OID: %u", attr->atttypid);
		}
	}

	pq_endmessage(&buf);

	return true;
}
