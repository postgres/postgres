/* rserv.c
 * Support functions for erServer replication.
 * (c) 2000 Vadim Mikheev, PostgreSQL Inc.
 */

#include "executor/spi.h"		/* this is what you need to work with SPI */
#include "commands/trigger.h"	/* -"- and triggers */
#include "utils/tqual.h"		/* -"- and SnapshotData */
#include <ctype.h>				/* tolower () */

#ifdef PG_FUNCTION_INFO_V1
#define CurrentTriggerData ((TriggerData *) fcinfo->context)
#endif

#ifdef PG_FUNCTION_INFO_V1
PG_FUNCTION_INFO_V1(_rserv_log_);
PG_FUNCTION_INFO_V1(_rserv_sync_);
PG_FUNCTION_INFO_V1(_rserv_debug_);
Datum		_rserv_log_(PG_FUNCTION_ARGS);
Datum		_rserv_sync_(PG_FUNCTION_ARGS);
Datum		_rserv_debug_(PG_FUNCTION_ARGS);

#else
HeapTuple	_rserv_log_(void);
int32		_rserv_sync_(int32);
int32		_rserv_debug_(int32);

#endif

static int	debug = 0;

static char *OutputValue(char *key, char *buf, int size);

#ifdef PG_FUNCTION_INFO_V1
Datum
_rserv_log_(PG_FUNCTION_ARGS)
#else
HeapTuple
_rserv_log_()
#endif
{
	Trigger    *trigger;		/* to get trigger name */
	int			nargs;			/* # of args specified in CREATE TRIGGER */
	char	  **args;			/* argument: argnum */
	Relation	rel;			/* triggered relation */
	HeapTuple	tuple;			/* tuple to return */
	HeapTuple	newtuple = NULL;/* tuple to return */
	TupleDesc	tupdesc;		/* tuple description */
	int			keynum;
	char	   *key;
	char	   *okey;
	char	   *newkey = NULL;
	int			deleted;
	char		sql[8192];
	char		outbuf[8192];
	char		oidbuf[64];
	int			ret;

	/* Called by trigger manager ? */
	if (!CurrentTriggerData)
		elog(ERROR, "_rserv_log_: triggers are not initialized");

	/* Should be called for ROW trigger */
	if (TRIGGER_FIRED_FOR_STATEMENT(CurrentTriggerData->tg_event))
		elog(ERROR, "_rserv_log_: can't process STATEMENT events");

	tuple = CurrentTriggerData->tg_trigtuple;

	trigger = CurrentTriggerData->tg_trigger;
	nargs = trigger->tgnargs;
	args = trigger->tgargs;

	if (nargs != 1)				/* odd number of arguments! */
		elog(ERROR, "_rserv_log_: need in *one* argument");

	keynum = atoi(args[0]);

	if (keynum < 0 && keynum != ObjectIdAttributeNumber)
		elog(ERROR, "_rserv_log_: invalid keynum %d", keynum);

	rel = CurrentTriggerData->tg_relation;
	tupdesc = rel->rd_att;

	deleted = (TRIGGER_FIRED_BY_DELETE(CurrentTriggerData->tg_event)) ?
		1 : 0;

	if (TRIGGER_FIRED_BY_UPDATE(CurrentTriggerData->tg_event))
		newtuple = CurrentTriggerData->tg_newtuple;

	/*
	 * Setting CurrentTriggerData to NULL prevents direct calls to trigger
	 * functions in queries. Normally, trigger functions have to be called
	 * by trigger manager code only.
	 */
	CurrentTriggerData = NULL;

	/* Connect to SPI manager */
	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "_rserv_log_: SPI_connect returned %d", ret);

	if (keynum == ObjectIdAttributeNumber)
	{
		sprintf(oidbuf, "%u", tuple->t_data->t_oid);
		key = oidbuf;
	}
	else
		key = SPI_getvalue(tuple, tupdesc, keynum);

	if (key == NULL)
		elog(ERROR, "_rserv_log_: key must be not null");

	if (newtuple && keynum != ObjectIdAttributeNumber)
	{
		newkey = SPI_getvalue(newtuple, tupdesc, keynum);
		if (newkey == NULL)
			elog(ERROR, "_rserv_log_: key must be not null");
		if (strcmp(newkey, key) == 0)
			newkey = NULL;
		else
			deleted = 1;		/* old key was deleted */
	}

	if (strpbrk(key, "\\	\n'"))
		okey = OutputValue(key, outbuf, sizeof(outbuf));
	else
		okey = key;

	sprintf(sql, "update _RSERV_LOG_ set logid = %d, logtime = now(), "
			"deleted = %d where reloid = %u and key = '%s'",
			GetCurrentTransactionId(), deleted, rel->rd_id, okey);

	if (debug)
		elog(NOTICE, sql);

	ret = SPI_exec(sql, 0);

	if (ret < 0)
		elog(ERROR, "_rserv_log_: SPI_exec(update) returned %d", ret);

	/*
	 * If no tuple was UPDATEd then do INSERT...
	 */
	if (SPI_processed > 1)
		elog(ERROR, "_rserv_log_: duplicate tuples");
	else if (SPI_processed == 0)
	{
		sprintf(sql, "insert into _RSERV_LOG_ "
				"(reloid, logid, logtime, deleted, key) "
				"values (%u, %d, now(), %d, '%s')",
				rel->rd_id, GetCurrentTransactionId(),
				deleted, okey);

		if (debug)
			elog(NOTICE, sql);

		ret = SPI_exec(sql, 0);

		if (ret < 0)
			elog(ERROR, "_rserv_log_: SPI_exec(insert) returned %d", ret);
	}

	if (okey != key && okey != outbuf)
		pfree(okey);

	if (newkey)
	{
		if (strpbrk(newkey, "\\	\n'"))
			okey = OutputValue(newkey, outbuf, sizeof(outbuf));
		else
			okey = newkey;

		sprintf(sql, "insert into _RSERV_LOG_ "
				"(reloid, logid, logtime, deleted, key) "
				"values (%u, %d, now(), 0, '%s')",
				rel->rd_id, GetCurrentTransactionId(), okey);

		if (debug)
			elog(NOTICE, sql);

		ret = SPI_exec(sql, 0);

		if (ret < 0)
			elog(ERROR, "_rserv_log_: SPI_exec returned %d", ret);

		if (okey != newkey && okey != outbuf)
			pfree(okey);
	}

	SPI_finish();

#ifdef PG_FUNCTION_INFO_V1
	return (PointerGetDatum(tuple));
#else
	return (tuple);
#endif
}

#ifdef PG_FUNCTION_INFO_V1
Datum
_rserv_sync_(PG_FUNCTION_ARGS)
#else
int32
_rserv_sync_(int32 server)
#endif
{
#ifdef PG_FUNCTION_INFO_V1
	int32		server = PG_GETARG_INT32(0);

#endif
	char		sql[8192];
	char		buf[8192];
	char	   *active = buf;
	uint32		xcnt;
	int			ret;

	if (SerializableSnapshot == NULL)
		elog(ERROR, "_rserv_sync_: SerializableSnapshot is NULL");

	buf[0] = 0;
	for (xcnt = 0; xcnt < SerializableSnapshot->xcnt; xcnt++)
	{
		sprintf(buf + strlen(buf), "%s%u", (xcnt) ? ", " : "",
				SerializableSnapshot->xip[xcnt]);
	}

	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "_rserv_sync_: SPI_connect returned %d", ret);

	sprintf(sql, "insert into _RSERV_SYNC_ "
			"(server, syncid, synctime, status, minid, maxid, active) "
	  "values (%u, currval('_rserv_sync_seq_'), now(), 0, %d, %d, '%s')",
			server, SerializableSnapshot->xmin, SerializableSnapshot->xmax, active);

	ret = SPI_exec(sql, 0);

	if (ret < 0)
		elog(ERROR, "_rserv_sync_: SPI_exec returned %d", ret);

	SPI_finish();

	return (0);
}

#ifdef PG_FUNCTION_INFO_V1
Datum
_rserv_debug_(PG_FUNCTION_ARGS)
#else
int32
_rserv_debug_(int32 newval)
#endif
{
#ifdef PG_FUNCTION_INFO_V1
	int32		newval = PG_GETARG_INT32(0);

#endif
	int32		oldval = debug;

	debug = newval;

	return (oldval);
}

#define ExtendBy	1024

static char *
OutputValue(char *key, char *buf, int size)
{
	int			i = 0;
	char	   *out = buf;
	char	   *subst = NULL;
	int			slen = 0;

	size--;
	for (;;)
	{
		switch (*key)
		{
			case '\\':
				subst = "\\\\";
				slen = 2;
				break;
			case '	':
				subst = "\\011";
				slen = 4;
				break;
			case '\n':
				subst = "\\012";
				slen = 4;
				break;
			case '\'':
				subst = "\\047";
				slen = 4;
				break;
			case '\0':
				out[i] = 0;
				return (out);
			default:
				slen = 1;
				break;
		}

		if (i + slen >= size)
		{
			if (out == buf)
			{
				out = (char *) palloc(size + ExtendBy);
				strncpy(out, buf, i);
				size += ExtendBy;
			}
			else
			{
				out = (char *) repalloc(out, size + ExtendBy);
				size += ExtendBy;
			}
		}

		if (slen == 1)
			out[i++] = *key;
		else
		{
			memcpy(out + i, subst, slen);
			i += slen;
		}
		key++;
	}

	return (out);

}
