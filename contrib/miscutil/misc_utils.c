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

#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <errno.h>

#include "postgres.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/tupdesc.h"
#include "catalog/catname.h"
#include "catalog/pg_listener.h"
#include "storage/lmgr.h"
#include "utils/fmgr.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/tqual.h"

#include "misc_utils.h"

#define MIN(x,y)	((x)<=(y) ? (x) : (y))

extern int	ExecutorLimit(int limit);
extern void Async_Unlisten(char *relname, int pid);
extern int	assertTest(int val);

#ifdef ASSERT_CHECKING_TEST
extern int	assertEnable(int val);

#endif

int
query_limit(int limit)
{
	return ExecutorLimit(limit);
}

int
backend_pid()
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
max(int x, int y)
{
	return ((x > y) ? x : y);
}

int
min(int x, int y)
{
	return ((x < y) ? x : y);
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
		len = MIN(VARSIZE(relname) - VARHDRSZ, NAMEDATALEN - 1);
		strncpy(listen_name, VARDATA(relname), len);
		listen_name[len] = '\0';
		ScanKeyEntryInitialize(&key, 0,
							   Anum_pg_listener_relname,
							   F_NAMEEQ,
							   PointerGetDatum(listen_name));
		sRel = heap_beginscan(lRel, 0, SnapshotNow, 1, &key);
	}
	else
		sRel = heap_beginscan(lRel, 0, SnapshotNow, 0, (ScanKey) NULL);

	while (HeapTupleIsValid(lTuple = heap_getnext(sRel, 0)))
	{
		d = heap_getattr(lTuple, Anum_pg_listener_pid, tdesc, &isnull);
		pid = DatumGetInt32(d);
		if ((pid == ourpid) || (kill(pid, SIGTSTP) == 0))
		{
			/* elog(NOTICE, "%d ok", pid); */
			count++;
		}
	}
	heap_endscan(sRel);

	heap_close(lRel, AccessShareLock);

	return count;
}

#ifdef USE_ASSERT_CHECKING
int
assert_enable(int val)
{
	return assertEnable(val);
}

#ifdef ASSERT_CHECKING_TEST
int
assert_test(int val)
{
	return assertTest(val);
}

#endif
#endif

/* end of file */

/*
 * Local Variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
