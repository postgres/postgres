/*-------------------------------------------------------------------------
 *
 * sets.c--
 *	  Functions for sets, which are defined by queries.
 *	  Example:	 a set is defined as being the result of the query
 *			retrieve (X.all)
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/Attic/sets.c,v 1.20 1998/12/15 12:46:34 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>				/* for sprintf() */
#include <string.h>

#include "postgres.h"

#include "access/heapam.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/pg_proc.h"	/* for Form_pg_proc */
#include "catalog/catname.h"	/* for ProcedureRelationName */
#include "catalog/indexing.h"	/* for Num_pg_proc_indices */
#include "fmgr.h"
#include "storage/lmgr.h"
#include "tcop/dest.h"
#include "utils/sets.h"			/* for GENERICSETNAME	   */
#include "utils/syscache.h"		/* for PROOID */
#include "utils/tqual.h"

extern CommandDest whereToSendOutput;	/* defined in tcop/postgres.c */


/*
 *	  SetDefine		   - converts query string defining set to an oid
 *
 *	  The query string is used to store the set as a function in
 *	  pg_proc.	The name of the function is then changed to use the
 *	  OID of its tuple in pg_proc.
 */
Oid
SetDefine(char *querystr, char *typename)
{
	Oid			setoid;
	char	   *procname = GENERICSETNAME;
	char	   *fileName = "-";
	char		realprocname[NAMEDATALEN];
	HeapTuple	tup,
				newtup = NULL;
	Form_pg_proc proc;
	Relation	procrel;
	int			i;
	Datum		replValue[Natts_pg_proc];
	char		replNull[Natts_pg_proc];
	char		repl[Natts_pg_proc];

	setoid = ProcedureCreate(procname,	/* changed below, after oid known */
							 true,		/* returnsSet */
							 typename,	/* returnTypeName */
							 "sql",		/* languageName */
							 querystr,	/* sourceCode */
							 fileName,	/* fileName */
							 false,		/* canCache */
							 true,		/* trusted */
							 100,		/* byte_pct */
							 0, /* perbyte_cpu */
							 0, /* percall_cpu */
							 100,		/* outin_ratio */
							 NIL,		/* argList */
							 whereToSendOutput);

	/*
	 * Since we're still inside this command of the transaction, we can't
	 * see the results of the procedure definition unless we pretend we've
	 * started the next command.  (Postgres's solution to the Halloween
	 * problem is to not allow you to see the results of your command
	 * until you start the next command.)
	 */
	CommandCounterIncrement();
	tup = SearchSysCacheTuple(PROOID,
							  ObjectIdGetDatum(setoid),
							  0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "setin: unable to define set %s", querystr);

	/*
	 * We can tell whether the set was already defined by checking the
	 * name.   If it's GENERICSETNAME, the set is new.  If it's "set<some
	 * oid>" it's already defined.
	 */
	proc = (Form_pg_proc) GETSTRUCT(tup);
	if (!strcmp((char *) procname, (char *) &(proc->proname)))
	{
		/* make the real proc name */
		sprintf(realprocname, "set%u", setoid);

		/* set up the attributes to be modified or kept the same */
		repl[0] = 'r';
		for (i = 1; i < Natts_pg_proc; i++)
			repl[i] = ' ';
		replValue[0] = (Datum) realprocname;
		for (i = 1; i < Natts_pg_proc; i++)
			replValue[i] = (Datum) 0;
		for (i = 0; i < Natts_pg_proc; i++)
			replNull[i] = ' ';

		/* change the pg_proc tuple */
		procrel = heap_openr(ProcedureRelationName);
		LockRelation(procrel, AccessExclusiveLock);

		tup = SearchSysCacheTuple(PROOID,
								  ObjectIdGetDatum(setoid),
								  0, 0, 0);
		if (HeapTupleIsValid(tup))
		{
			newtup = heap_modifytuple(tup,
									  procrel,
									  replValue,
									  replNull,
									  repl);

			setheapoverride(true);
			heap_replace(procrel, &tup->t_self, newtup, NULL);
			setheapoverride(false);

			setoid = newtup->t_data->t_oid;
		}
		else
			elog(ERROR, "setin: could not find new set oid tuple");

		if (RelationGetForm(procrel)->relhasindex)
		{
			Relation	idescs[Num_pg_proc_indices];

			CatalogOpenIndices(Num_pg_proc_indices, Name_pg_proc_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_proc_indices, procrel, newtup);
			CatalogCloseIndices(Num_pg_proc_indices, idescs);
		}
		UnlockRelation(procrel, AccessExclusiveLock);
		heap_close(procrel);
	}
	return setoid;
}

/* This function is a placeholder.	The parser uses the OID of this
 * function to fill in the :funcid field  of a set.  This routine is
 * never executed.	At runtime, the OID of the actual set is substituted
 * into the :funcid.
 */
int
seteval(Oid funcoid)
{
	return 17;
}
