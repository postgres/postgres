/*-------------------------------------------------------------------------
 *
 * spgvalidate.c
 *	  Opclass validator for SP-GiST.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/spgist_private.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "utils/catcache.h"
#include "utils/syscache.h"


/*
 * Validator for an SP-GiST opclass.
 */
bool
spgvalidate(Oid opclassoid)
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
			procform->amprocnum > SPGISTNProc)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("spgist opfamily %u contains invalid support number %d for procedure %u",
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
					 errmsg("spgist opfamily %u contains invalid strategy number %d for operator %u",
							opfamilyoid,
							oprform->amopstrategy, oprform->amopopr)));

		/* spgist doesn't support ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("spgist opfamily %u contains invalid ORDER BY specification for operator %u",
							opfamilyoid, oprform->amopopr)));

		/* Count operators that are specifically for the named opclass */
		if (oprform->amoplefttype == opcintype &&
			oprform->amoprighttype == opcintype)
			numclassops++;
	}

	/* Check that the named opclass is complete */
	if (numclassops == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("spgist opclass %u is missing operator(s)",
						opclassoid)));
	for (i = 1; i <= SPGISTNProc; i++)
	{
		if ((classfuncbits & (1 << i)) != 0)
			continue;			/* got it */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
		  errmsg("spgist opclass %u is missing required support function %d",
				 opclassoid, i)));
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);

	return true;
}
