 /*-------------------------------------------------------------------------
 *
 * regproc.c
 *	  Functions for the built-in type "RegProcedure".
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/adt/regproc.c,v 1.65 2002/04/05 00:31:29 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"


/*****************************************************************************
 *	 USER I/O ROUTINES														 *
 *****************************************************************************/

/*
 *		regprocin		- converts "proname" or "proid" to proid
 *
 *		We need to accept an OID for cases where the name is ambiguous.
 *
 *		proid of '-' signifies unknown, for consistency with regprocout
 */
Datum
regprocin(PG_FUNCTION_ARGS)
{
	char	   *pro_name_or_oid = PG_GETARG_CSTRING(0);
	RegProcedure result = InvalidOid;
	int			matches = 0;

	if (pro_name_or_oid[0] == '-' && pro_name_or_oid[1] == '\0')
		PG_RETURN_OID(InvalidOid);

	if (pro_name_or_oid[0] >= '0' &&
		pro_name_or_oid[0] <= '9' &&
		strspn(pro_name_or_oid, "0123456789") == strlen(pro_name_or_oid))
	{
		Oid			searchOid;

		searchOid = DatumGetObjectId(DirectFunctionCall1(oidin,
									  CStringGetDatum(pro_name_or_oid)));
		result = (RegProcedure) GetSysCacheOid(PROCOID,
											 ObjectIdGetDatum(searchOid),
											   0, 0, 0);
		if (!RegProcedureIsValid(result))
			elog(ERROR, "No procedure with oid %s", pro_name_or_oid);
		matches = 1;
	}
	else
	{
		Relation	hdesc;
		ScanKeyData skey[1];
		SysScanDesc	funcscan;
		HeapTuple	tuple;

		ScanKeyEntryInitialize(&skey[0], 0x0,
							   (AttrNumber) Anum_pg_proc_proname,
							   (RegProcedure) F_NAMEEQ,
							   CStringGetDatum(pro_name_or_oid));

		hdesc = heap_openr(ProcedureRelationName, AccessShareLock);

		funcscan = systable_beginscan(hdesc, ProcedureNameNspIndex, true,
									  SnapshotNow, 1, skey);

		while (HeapTupleIsValid(tuple = systable_getnext(funcscan)))
		{
			result = (RegProcedure) tuple->t_data->t_oid;
			if (++matches > 1)
				break;
		}

		systable_endscan(funcscan);

		heap_close(hdesc, AccessShareLock);
	}

	if (matches > 1)
		elog(ERROR, "There is more than one procedure named %s.\n\tSupply the pg_proc oid inside single quotes.", pro_name_or_oid);
	else if (matches == 0)
		elog(ERROR, "No procedure with name %s", pro_name_or_oid);

	PG_RETURN_OID(result);
}

/*
 *		regprocout		- converts proid to "pro_name"
 */
Datum
regprocout(PG_FUNCTION_ARGS)
{
	RegProcedure proid = PG_GETARG_OID(0);
	HeapTuple	proctup;
	char	   *result;

	result = (char *) palloc(NAMEDATALEN);

	if (proid == InvalidOid)
	{
		result[0] = '-';
		result[1] = '\0';
		PG_RETURN_CSTRING(result);
	}

	proctup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(proid),
							 0, 0, 0);

	if (HeapTupleIsValid(proctup))
	{
		char	   *s;

		s = NameStr(((Form_pg_proc) GETSTRUCT(proctup))->proname);
		StrNCpy(result, s, NAMEDATALEN);
		ReleaseSysCache(proctup);
	}
	else
	{
		result[0] = '-';
		result[1] = '\0';
	}

	PG_RETURN_CSTRING(result);
}



/*****************************************************************************
 *	 PUBLIC ROUTINES														 *
 *****************************************************************************/

/* regproctooid()
 * Lowercase version of RegprocToOid() to allow case-insensitive SQL.
 * Define RegprocToOid() as a macro in builtins.h.
 * Referenced in pg_proc.h. - tgl 97/04/26
 */
Datum
regproctooid(PG_FUNCTION_ARGS)
{
	RegProcedure rp = PG_GETARG_OID(0);

	PG_RETURN_OID((Oid) rp);
}

/* (see int.c for comparison/operation routines) */
