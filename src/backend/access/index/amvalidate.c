/*-------------------------------------------------------------------------
 *
 * amvalidate.c
 *	  Support routines for index access methods' amvalidate functions.
 *
 * Copyright (c) 2016, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/index/amvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "parser/parse_coerce.h"
#include "utils/syscache.h"


/*
 * identify_opfamily_groups() returns a List of OpFamilyOpFuncGroup structs,
 * one for each combination of lefttype/righttype present in the family's
 * operator and support function lists.  If amopstrategy K is present for
 * this datatype combination, we set bit 1 << K in operatorset, and similarly
 * for the support functions.  With uint64 fields we can handle operator and
 * function numbers up to 63, which is plenty for the foreseeable future.
 *
 * The given CatCLists are expected to represent a single opfamily fetched
 * from the AMOPSTRATEGY and AMPROCNUM caches, so that they will be in
 * order by those caches' second and third cache keys, namely the datatypes.
 */
List *
identify_opfamily_groups(CatCList *oprlist, CatCList *proclist)
{
	List	   *result = NIL;
	OpFamilyOpFuncGroup *thisgroup;
	Form_pg_amop oprform;
	Form_pg_amproc procform;
	int			io,
				ip;

	/* We need the lists to be ordered; should be true in normal operation */
	if (!oprlist->ordered || !proclist->ordered)
		elog(ERROR, "cannot validate operator family without ordered data");

	/*
	 * Advance through the lists concurrently.  Thanks to the ordering, we
	 * should see all operators and functions of a given datatype pair
	 * consecutively.
	 */
	thisgroup = NULL;
	io = ip = 0;
	if (io < oprlist->n_members)
	{
		oprform = (Form_pg_amop) GETSTRUCT(&oprlist->members[io]->tuple);
		io++;
	}
	else
		oprform = NULL;
	if (ip < proclist->n_members)
	{
		procform = (Form_pg_amproc) GETSTRUCT(&proclist->members[ip]->tuple);
		ip++;
	}
	else
		procform = NULL;

	while (oprform || procform)
	{
		if (oprform && thisgroup &&
			oprform->amoplefttype == thisgroup->lefttype &&
			oprform->amoprighttype == thisgroup->righttype)
		{
			/* Operator belongs to current group; include it and advance */

			/* Ignore strategy numbers outside supported range */
			if (oprform->amopstrategy > 0 && oprform->amopstrategy < 64)
				thisgroup->operatorset |= ((uint64) 1) << oprform->amopstrategy;

			if (io < oprlist->n_members)
			{
				oprform = (Form_pg_amop) GETSTRUCT(&oprlist->members[io]->tuple);
				io++;
			}
			else
				oprform = NULL;
			continue;
		}

		if (procform && thisgroup &&
			procform->amproclefttype == thisgroup->lefttype &&
			procform->amprocrighttype == thisgroup->righttype)
		{
			/* Procedure belongs to current group; include it and advance */

			/* Ignore function numbers outside supported range */
			if (procform->amprocnum > 0 && procform->amprocnum < 64)
				thisgroup->functionset |= ((uint64) 1) << procform->amprocnum;

			if (ip < proclist->n_members)
			{
				procform = (Form_pg_amproc) GETSTRUCT(&proclist->members[ip]->tuple);
				ip++;
			}
			else
				procform = NULL;
			continue;
		}

		/* Time for a new group */
		thisgroup = (OpFamilyOpFuncGroup *) palloc(sizeof(OpFamilyOpFuncGroup));
		if (oprform &&
			(!procform ||
			 (oprform->amoplefttype < procform->amproclefttype ||
			  (oprform->amoplefttype == procform->amproclefttype &&
			   oprform->amoprighttype < procform->amprocrighttype))))
		{
			thisgroup->lefttype = oprform->amoplefttype;
			thisgroup->righttype = oprform->amoprighttype;
		}
		else
		{
			thisgroup->lefttype = procform->amproclefttype;
			thisgroup->righttype = procform->amprocrighttype;
		}
		thisgroup->operatorset = thisgroup->functionset = 0;
		result = lappend(result, thisgroup);
	}

	return result;
}

/*
 * Validate the signature (argument and result types) of an opclass support
 * function.  Return TRUE if OK, FALSE if not.
 *
 * The "..." represents maxargs argument-type OIDs.  If "exact" is TRUE, they
 * must match the function arg types exactly, else only binary-coercibly.
 * In any case the function result type must match restype exactly.
 */
bool
check_amproc_signature(Oid funcid, Oid restype, bool exact,
					   int minargs, int maxargs,...)
{
	bool		result = true;
	HeapTuple	tp;
	Form_pg_proc procform;
	va_list		ap;
	int			i;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(tp);

	if (procform->prorettype != restype || procform->proretset ||
		procform->pronargs < minargs || procform->pronargs > maxargs)
		result = false;

	va_start(ap, maxargs);
	for (i = 0; i < maxargs; i++)
	{
		Oid			argtype = va_arg(ap, Oid);

		if (i >= procform->pronargs)
			continue;
		if (exact ? (argtype != procform->proargtypes.values[i]) :
			!IsBinaryCoercible(argtype, procform->proargtypes.values[i]))
			result = false;
	}
	va_end(ap);

	ReleaseSysCache(tp);
	return result;
}

/*
 * Validate the signature (argument and result types) of an opclass operator.
 * Return TRUE if OK, FALSE if not.
 *
 * Currently, we can hard-wire this as accepting only binary operators.  Also,
 * we can insist on exact type matches, since the given lefttype/righttype
 * come from pg_amop and should always match the operator exactly.
 */
bool
check_amop_signature(Oid opno, Oid restype, Oid lefttype, Oid righttype)
{
	bool		result = true;
	HeapTuple	tp;
	Form_pg_operator opform;

	tp = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (!HeapTupleIsValid(tp))	/* shouldn't happen */
		elog(ERROR, "cache lookup failed for operator %u", opno);
	opform = (Form_pg_operator) GETSTRUCT(tp);

	if (opform->oprresult != restype || opform->oprkind != 'b' ||
		opform->oprleft != lefttype || opform->oprright != righttype)
		result = false;

	ReleaseSysCache(tp);
	return result;
}

/*
 * Is the datatype a legitimate input type for the btree opfamily?
 */
bool
opfamily_can_sort_type(Oid opfamilyoid, Oid datatypeoid)
{
	bool		result = false;
	CatCList   *opclist;
	int			i;

	/*
	 * We search through all btree opclasses to see if one matches.  This is a
	 * bit inefficient but there is no better index available.  It also saves
	 * making an explicit check that the opfamily belongs to btree.
	 */
	opclist = SearchSysCacheList1(CLAAMNAMENSP, ObjectIdGetDatum(BTREE_AM_OID));

	for (i = 0; i < opclist->n_members; i++)
	{
		HeapTuple	classtup = &opclist->members[i]->tuple;
		Form_pg_opclass classform = (Form_pg_opclass) GETSTRUCT(classtup);

		if (classform->opcfamily == opfamilyoid &&
			classform->opcintype == datatypeoid)
		{
			result = true;
			break;
		}
	}

	ReleaseCatCacheList(opclist);

	return result;
}
