/*-------------------------------------------------------------------------
 *
 * ginvalidate.c
 *	  Opclass validator for GIN.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin_private.h"
#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "utils/catcache.h"
#include "utils/syscache.h"


/*
 * Validator for a GIN opclass.
 */
bool
ginvalidate(Oid opclassoid)
{
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	int			numclassops;
	int32		classfuncbits;
	CatCList   *proclist,
			   *oprlist;
	int			i;

	/* Fetch opclass information */
	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;

	ReleaseSysCache(classtup);

	/* Fetch all operators and support functions of the opfamily */
	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));

	/* We'll track the ops and functions belonging to the named opclass */
	numclassops = 0;
	classfuncbits = 0;

	/* Check support functions */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);

		/* Check that only allowed procedure numbers exist */
		if (procform->amprocnum < 1 ||
			procform->amprocnum > GINNProcs)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("gin opfamily %u contains invalid support number %d for procedure %u",
							opfamilyoid,
							procform->amprocnum, procform->amproc)));

		/* Remember functions that are specifically for the named opclass */
		if (procform->amproclefttype == opcintype &&
			procform->amprocrighttype == opcintype)
			classfuncbits |= (1 << procform->amprocnum);
	}

	/* Check operators */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* TODO: Check that only allowed strategy numbers exist */
		if (oprform->amopstrategy < 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("gin opfamily %u contains invalid strategy number %d for operator %u",
							opfamilyoid,
							oprform->amopstrategy, oprform->amopopr)));

		/* gin doesn't support ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("gin opfamily %u contains invalid ORDER BY specification for operator %u",
							opfamilyoid, oprform->amopopr)));

		/* Count operators that are specifically for the named opclass */
		if (oprform->amoplefttype == opcintype &&
			oprform->amoprighttype == opcintype)
			numclassops++;
	}

	/* Check that the named opclass is complete */

	/* XXX needs work: we need to detect applicability of ANYARRAY operators */
#ifdef NOT_USED
	if (numclassops == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("gin opclass %u is missing operator(s)",
						opclassoid)));
#endif

	for (i = 1; i <= GINNProcs; i++)
	{
		if ((classfuncbits & (1 << i)) != 0)
			continue;			/* got it */
		if (i == GIN_COMPARE_PARTIAL_PROC)
			continue;			/* optional method */
		if (i == GIN_CONSISTENT_PROC || i == GIN_TRICONSISTENT_PROC)
			continue;			/* don't need to have both, see check below
								 * loop */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			 errmsg("gin opclass %u is missing required support function %d",
					opclassoid, i)));
	}
	if ((classfuncbits & (1 << GIN_CONSISTENT_PROC)) == 0 &&
		(classfuncbits & (1 << GIN_TRICONSISTENT_PROC)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("gin opclass %u is missing required support function",
						opclassoid)));

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);

	return true;
}
