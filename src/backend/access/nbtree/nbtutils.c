/*-------------------------------------------------------------------------
 *
 * btutils.c--
 *	  Utility code for Postgres btree implementation.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtutils.c,v 1.13 1997/09/08 02:21:01 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/genam.h>
#include <fmgr.h>
#include <storage/bufpage.h>
#include <access/nbtree.h>
#include <access/istrat.h>
#include <access/iqual.h>
#include <catalog/pg_proc.h>
#include <executor/execdebug.h>

extern int	NIndexTupleProcessed;


#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

ScanKey
_bt_mkscankey(Relation rel, IndexTuple itup)
{
	ScanKey		skey;
	TupleDesc	itupdesc;
	int			natts;
	int			i;
	Datum		arg;
	RegProcedure proc;
	bool		null;
	bits16		flag;

	natts = rel->rd_rel->relnatts;
	itupdesc = RelationGetTupleDescriptor(rel);

	skey = (ScanKey) palloc(natts * sizeof(ScanKeyData));

	for (i = 0; i < natts; i++)
	{
		arg = index_getattr(itup, i + 1, itupdesc, &null);
		if (null)
		{
			proc = NullValueRegProcedure;
			flag = SK_ISNULL;
		}
		else
		{
			proc = index_getprocid(rel, i + 1, BTORDER_PROC);
			flag = 0x0;
		}
		ScanKeyEntryInitialize(&skey[i],
							   flag, (AttrNumber) (i + 1), proc, arg);
	}

	return (skey);
}

void
_bt_freeskey(ScanKey skey)
{
	pfree(skey);
}

void
_bt_freestack(BTStack stack)
{
	BTStack		ostack;

	while (stack != (BTStack) NULL)
	{
		ostack = stack;
		stack = stack->bts_parent;
		pfree(ostack->bts_btitem);
		pfree(ostack);
	}
}

/*
 *	_bt_orderkeys() -- Put keys in a sensible order for conjunctive quals.
 *
 *		The order of the keys in the qual match the ordering imposed by
 *		the index.	This routine only needs to be called if there are
 *		more than one qual clauses using this index.
 */
void
_bt_orderkeys(Relation relation, BTScanOpaque so)
{
	ScanKey		xform;
	ScanKeyData *cur;
	StrategyMap map;
	int			nbytes;
	long		test;
	int			i,
				j;
	int			init[BTMaxStrategyNumber + 1];
	ScanKey		key;
	uint16		numberOfKeys = so->numberOfKeys;
	uint16		new_numberOfKeys = 0;
	AttrNumber	attno = 1;

	if (numberOfKeys < 1)
		return;

	key = so->keyData;

	cur = &key[0];
	if (cur->sk_attno != 1)
		elog(WARN, "_bt_orderkeys: key(s) for attribute 1 missed");

	if (numberOfKeys == 1)
	{

		/*
		 * We don't use indices for 'A is null' and 'A is not null'
		 * currently and 'A < = > <> NULL' is non-sense' - so qual is not
		 * Ok.		- vadim 03/21/97
		 */
		if (cur->sk_flags & SK_ISNULL)
			so->qual_ok = 0;
		so->numberOfFirstKeys = 1;
		return;
	}

	/* get space for the modified array of keys */
	nbytes = BTMaxStrategyNumber * sizeof(ScanKeyData);
	xform = (ScanKey) palloc(nbytes);

	memset(xform, 0, nbytes);
	map = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
									  BTMaxStrategyNumber,
									  attno);
	for (j = 0; j <= BTMaxStrategyNumber; j++)
		init[j] = 0;

	/* check each key passed in */
	for (i = 0;;)
	{
		if (i < numberOfKeys)
			cur = &key[i];

		if (cur->sk_flags & SK_ISNULL)	/* see comments above */
			so->qual_ok = 0;

		if (i == numberOfKeys || cur->sk_attno != attno)
		{
			if (cur->sk_attno != attno + 1 && i < numberOfKeys)
			{
				elog(WARN, "_bt_orderkeys: key(s) for attribute %d missed", attno + 1);
			}

			/*
			 * If = has been specified, no other key will be used. In case
			 * of key < 2 && key == 1 and so on we have to set qual_ok to
			 * 0
			 */
			if (init[BTEqualStrategyNumber - 1])
			{
				ScanKeyData *eq,
						   *chk;

				eq = &xform[BTEqualStrategyNumber - 1];
				for (j = BTMaxStrategyNumber; --j >= 0;)
				{
					if (j == (BTEqualStrategyNumber - 1) || init[j] == 0)
						continue;
					chk = &xform[j];
					test = (long) fmgr(chk->sk_procedure, eq->sk_argument, chk->sk_argument);
					if (!test)
						so->qual_ok = 0;
				}
				init[BTLessStrategyNumber - 1] = 0;
				init[BTLessEqualStrategyNumber - 1] = 0;
				init[BTGreaterEqualStrategyNumber - 1] = 0;
				init[BTGreaterStrategyNumber - 1] = 0;
			}

			/* only one of <, <= */
			if (init[BTLessStrategyNumber - 1]
				&& init[BTLessEqualStrategyNumber - 1])
			{
				ScanKeyData *lt,
						   *le;

				lt = &xform[BTLessStrategyNumber - 1];
				le = &xform[BTLessEqualStrategyNumber - 1];

				/*
				 * DO NOT use the cached function stuff here -- this is
				 * key ordering, happens only when the user expresses a
				 * hokey qualification, and gets executed only once,
				 * anyway.	The transform maps are hard-coded, and can't
				 * be initialized in the correct way.
				 */
				test = (long) fmgr(le->sk_procedure, lt->sk_argument, le->sk_argument);
				if (test)
					init[BTLessEqualStrategyNumber - 1] = 0;
				else
					init[BTLessStrategyNumber - 1] = 0;
			}

			/* only one of >, >= */
			if (init[BTGreaterStrategyNumber - 1]
				&& init[BTGreaterEqualStrategyNumber - 1])
			{
				ScanKeyData *gt,
						   *ge;

				gt = &xform[BTGreaterStrategyNumber - 1];
				ge = &xform[BTGreaterEqualStrategyNumber - 1];

				/* see note above on function cache */
				test = (long) fmgr(ge->sk_procedure, gt->sk_argument, ge->sk_argument);
				if (test)
					init[BTGreaterEqualStrategyNumber - 1] = 0;
				else
					init[BTGreaterStrategyNumber - 1] = 0;
			}

			/* okay, reorder and count */
			for (j = BTMaxStrategyNumber; --j >= 0;)
				if (init[j])
					key[new_numberOfKeys++] = xform[j];

			if (attno == 1)
				so->numberOfFirstKeys = new_numberOfKeys;

			if (i == numberOfKeys)
				break;

			/* initialization for new attno */
			attno = cur->sk_attno;
			memset(xform, 0, nbytes);
			map = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
											  BTMaxStrategyNumber,
											  attno);
			/* haven't looked at any strategies yet */
			for (j = 0; j <= BTMaxStrategyNumber; j++)
				init[j] = 0;
		}

		for (j = BTMaxStrategyNumber; --j >= 0;)
		{
			if (cur->sk_procedure == map->entry[j].sk_procedure)
				break;
		}

		/* have we seen one of these before? */
		if (init[j])
		{
			/* yup, use the appropriate value */
			test =
				(long) FMGR_PTR2(cur->sk_func, cur->sk_procedure,
								 cur->sk_argument, xform[j].sk_argument);
			if (test)
				xform[j].sk_argument = cur->sk_argument;
			else if (j == (BTEqualStrategyNumber - 1))
				so->qual_ok = 0;/* key == a && key == b, but a != b */
		}
		else
		{
			/* nope, use this value */
			memmove(&xform[j], cur, sizeof(*cur));
			init[j] = 1;
		}

		i++;
	}

	so->numberOfKeys = new_numberOfKeys;

	pfree(xform);
}

BTItem
_bt_formitem(IndexTuple itup)
{
	int			nbytes_btitem;
	BTItem		btitem;
	Size		tuplen;
	extern Oid	newoid();

	/*
	 * see comments in btbuild
	 *
	 * if (itup->t_info & INDEX_NULL_MASK) elog(WARN, "btree indices cannot
	 * include null keys");
	 */

	/* make a copy of the index tuple with room for the sequence number */
	tuplen = IndexTupleSize(itup);
	nbytes_btitem = tuplen +
		(sizeof(BTItemData) - sizeof(IndexTupleData));

	btitem = (BTItem) palloc(nbytes_btitem);
	memmove((char *) &(btitem->bti_itup), (char *) itup, tuplen);

#ifndef BTREE_VERSION_1
	btitem->bti_oid = newoid();
#endif
	return (btitem);
}

#ifdef NOT_USED
bool
_bt_checkqual(IndexScanDesc scan, IndexTuple itup)
{
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;
	if (so->numberOfKeys > 0)
		return (index_keytest(itup, RelationGetTupleDescriptor(scan->relation),
							  so->numberOfKeys, so->keyData));
	else
		return (true);
}

#endif

#ifdef NOT_USED
bool
_bt_checkforkeys(IndexScanDesc scan, IndexTuple itup, Size keysz)
{
	BTScanOpaque so;

	so = (BTScanOpaque) scan->opaque;
	if (keysz > 0 && so->numberOfKeys >= keysz)
		return (index_keytest(itup, RelationGetTupleDescriptor(scan->relation),
							  keysz, so->keyData));
	else
		return (true);
}

#endif

bool
_bt_checkkeys(IndexScanDesc scan, IndexTuple tuple, Size * keysok)
{
	BTScanOpaque so = (BTScanOpaque) scan->opaque;
	Size		keysz = so->numberOfKeys;
	TupleDesc	tupdesc;
	ScanKey		key;
	Datum		datum;
	bool		isNull;
	int			test;

	*keysok = 0;
	if (keysz == 0)
		return (true);

	key = so->keyData;
	tupdesc = RelationGetTupleDescriptor(scan->relation);

	IncrIndexProcessed();

	while (keysz > 0)
	{
		datum = index_getattr(tuple,
							  key[0].sk_attno,
							  tupdesc,
							  &isNull);

		/* btree doesn't support 'A is null' clauses, yet */
		if (isNull || key[0].sk_flags & SK_ISNULL)
		{
			return (false);
		}

		if (key[0].sk_flags & SK_COMMUTE)
		{
			test = (int) (*(key[0].sk_func))
				(DatumGetPointer(key[0].sk_argument),
				 datum);
		}
		else
		{
			test = (int) (*(key[0].sk_func))
				(datum,
				 DatumGetPointer(key[0].sk_argument));
		}

		if (!test == !(key[0].sk_flags & SK_NEGATE))
		{
			return (false);
		}

		keysz -= 1;
		key++;
		(*keysok)++;
	}

	return (true);
}
