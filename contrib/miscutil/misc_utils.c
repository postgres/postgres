/*
 * misc_utils.c --
 *
 * This file defines miscellaneous PostgreSQL utility functions.
 *
 * Copyright (C) 1999, Massimo Dal Zotto <dz@cs.unitn.it>
 *
 * This file is distributed under the GNU General Public License
 * either version 2, or (at your option) any later version.
 */

#include "postgres.h"

#include <unistd.h>
#include <signal.h>

#include "access/heapam.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/tupdesc.h"
#include "catalog/catname.h"
#include "catalog/pg_listener.h"
#include "commands/async.h"
#include "fmgr.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/tqual.h"

#include "misc_utils.h"


int
backend_pid(void)
{
	return getpid();
}

int
unlisten(char *relname)
{
	Async_Unlisten(relname, getpid());
	return 0;
}

int
int4max(int x, int y)
{
	return Max(x, y);
}

int
int4min(int x, int y)
{
	return Min(x, y);
}

/*
 * Return the number of active listeners on a relation name.
 */
int
active_listeners(text *relname)
{
	HeapTuple	lTuple;
	Relation	lRel;
	HeapScanDesc sRel;
	TupleDesc	tdesc;
	ScanKeyData key;
	Datum		d;
	bool		isnull;
	int			len,
				pid;
	int			count = 0;
	int			ourpid = getpid();
	char		listen_name[NAMEDATALEN];

	lRel = heap_openr(ListenerRelationName, AccessShareLock);
	tdesc = RelationGetDescr(lRel);

	if (relname && (VARSIZE(relname) > VARHDRSZ))
	{
		MemSet(listen_name, 0, NAMEDATALEN);
		len = Min(VARSIZE(relname) - VARHDRSZ, NAMEDATALEN - 1);
		memcpy(listen_name, VARDATA(relname), len);
		ScanKeyEntryInitialize(&key, 0,
							   Anum_pg_listener_relname,
							   F_NAMEEQ,
							   PointerGetDatum(listen_name));
		sRel = heap_beginscan(lRel, SnapshotNow, 1, &key);
	}
	else
		sRel = heap_beginscan(lRel, SnapshotNow, 0, (ScanKey) NULL);

	while ((lTuple = heap_getnext(sRel, ForwardScanDirection)) != NULL)
	{
		d = heap_getattr(lTuple, Anum_pg_listener_pid, tdesc, &isnull);
		pid = DatumGetInt32(d);
		if ((pid == ourpid) || (kill(pid, 0) == 0))
			count++;
	}
	heap_endscan(sRel);

	heap_close(lRel, AccessShareLock);

	return count;
}
