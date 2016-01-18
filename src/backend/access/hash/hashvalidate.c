/*-------------------------------------------------------------------------
 *
 * hashvalidate.c
 *	  Opclass validator for hash.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/syscache.h"


/*
 * Validator for a hash opclass.
 *
 * Some of the checks done here cover the whole opfamily, and therefore are
 * redundant when checking each opclass in a family.  But they don't run long
 * enough to be much of a problem, so we accept the duplication rather than
 * complicate the amvalidate API.
 *
 * Some of the code here relies on the fact that hash has only one operator
 * strategy and support function; we don't have to check for incomplete sets.
 */
bool
hashvalidate(Oid opclassoid)
{
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	int			numclassops;
	int32		classfuncbits;
	CatCList   *proclist,
			   *oprlist;
	int			i,
				j;

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
		if (procform->amprocnum != HASHPROC)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash opfamily %u contains invalid support number %d for procedure %u",
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
		bool		leftFound = false,
					rightFound = false;

		/* Check that only allowed strategy numbers exist */
		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > HTMaxStrategyNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash opfamily %u contains invalid strategy number %d for operator %u",
							opfamilyoid,
							oprform->amopstrategy, oprform->amopopr)));

		/*
		 * There should be relevant hash procedures for each operator
		 */
		for (j = 0; j < proclist->n_members; j++)
		{
			HeapTuple	proctup = &proclist->members[j]->tuple;
			Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);

			if (procform->amproclefttype == oprform->amoplefttype)
				leftFound = true;
			if (procform->amproclefttype == oprform->amoprighttype)
				rightFound = true;
		}

		if (!leftFound || !rightFound)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			errmsg("hash opfamily %u lacks support function for operator %u",
				   opfamilyoid, oprform->amopopr)));

		/* hash doesn't support ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash opfamily %u contains invalid ORDER BY specification for operator %u",
							opfamilyoid, oprform->amopopr)));

		/* Count operators that are specifically for the named opclass */
		if (oprform->amoplefttype == opcintype &&
			oprform->amoprighttype == opcintype)
			numclassops++;
	}

	/* Check that the named opclass is complete */
	if (numclassops != HTMaxStrategyNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("hash opclass %u is missing operator(s)",
						opclassoid)));
	if ((classfuncbits & (1 << HASHPROC)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			   errmsg("hash opclass %u is missing required support function",
					  opclassoid)));

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);

	return true;
}
