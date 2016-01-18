/*-------------------------------------------------------------------------
 *
 * nbtvalidate.c
 *	  Opclass validator for btree.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/nbtree/nbtvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/syscache.h"


/*
 * Validator for a btree opclass.
 *
 * Some of the checks done here cover the whole opfamily, and therefore are
 * redundant when checking each opclass in a family.  But they don't run long
 * enough to be much of a problem, so we accept the duplication rather than
 * complicate the amvalidate API.
 */
bool
btvalidate(Oid opclassoid)
{
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	int			numclassops;
	int32		classfuncbits;
	CatCList   *proclist,
			   *oprlist;
	Oid			lastlefttype,
				lastrighttype;
	int			numOps;
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

	/* We rely on the oprlist to be ordered */
	if (!oprlist->ordered)
		elog(ERROR, "cannot validate btree opclass without ordered data");

	/* We'll track the ops and functions belonging to the named opclass */
	numclassops = 0;
	classfuncbits = 0;

	/* Check support functions */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);

		/* Check that only allowed procedure numbers exist */
		if (procform->amprocnum != BTORDER_PROC &&
			procform->amprocnum != BTSORTSUPPORT_PROC)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree opfamily %u contains invalid support number %d for procedure %u",
							opfamilyoid,
							procform->amprocnum, procform->amproc)));

		/* Remember functions that are specifically for the named opclass */
		if (procform->amproclefttype == opcintype &&
			procform->amprocrighttype == opcintype)
			classfuncbits |= (1 << procform->amprocnum);
	}

	/* Check operators */
	lastlefttype = lastrighttype = InvalidOid;
	numOps = 0;
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* Check that only allowed strategy numbers exist */
		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > BTMaxStrategyNumber)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree opfamily %u contains invalid strategy number %d for operator %u",
							opfamilyoid,
							oprform->amopstrategy, oprform->amopopr)));

		/*
		 * Check that we have all strategies for each supported datatype
		 * combination.  This is easy since the list will be sorted in
		 * datatype order and there can't be duplicate strategy numbers.
		 */
		if (oprform->amoplefttype == lastlefttype &&
			oprform->amoprighttype == lastrighttype)
			numOps++;
		else
		{
			/* reached a group boundary, so check ... */
			if (numOps > 0 && numOps != BTMaxStrategyNumber)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree opfamily %u has a partial set of operators for datatypes %s and %s",
								opfamilyoid,
								format_type_be(lastlefttype),
								format_type_be(lastrighttype))));
			/* ... and reset for new group */
			lastlefttype = oprform->amoplefttype;
			lastrighttype = oprform->amoprighttype;
			numOps = 1;
		}

		/*
		 * There should be a relevant support function for each operator, but
		 * we only need to check this once per pair of datatypes.
		 */
		if (numOps == 1)
		{
			bool		found = false;

			for (j = 0; j < proclist->n_members; j++)
			{
				HeapTuple	proctup = &proclist->members[j]->tuple;
				Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);

				if (procform->amprocnum == BTORDER_PROC &&
					procform->amproclefttype == oprform->amoplefttype &&
					procform->amprocrighttype == oprform->amoprighttype)
				{
					found = true;
					break;
				}
			}

			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree opfamily %u lacks support function for operator %u",
								opfamilyoid, oprform->amopopr)));
		}

		/* btree doesn't support ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree opfamily %u contains invalid ORDER BY specification for operator %u",
							opfamilyoid, oprform->amopopr)));

		/* Count operators that are specifically for the named opclass */
		if (oprform->amoplefttype == opcintype &&
			oprform->amoprighttype == opcintype)
			numclassops++;
	}

	/* don't forget to check the last batch of operators for completeness */
	if (numOps > 0 && numOps != BTMaxStrategyNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("btree opfamily %u has a partial set of operators for datatypes %s and %s",
						opfamilyoid,
						format_type_be(lastlefttype),
						format_type_be(lastrighttype))));

	/* Check that the named opclass is complete */
	if (numclassops != BTMaxStrategyNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("btree opclass %u is missing operator(s)",
						opclassoid)));
	if ((classfuncbits & (1 << BTORDER_PROC)) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			  errmsg("btree opclass %u is missing required support function",
					 opclassoid)));

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);

	return true;
}
