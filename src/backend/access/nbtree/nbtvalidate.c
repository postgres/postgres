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

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
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
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	char	   *opclassname;
	HeapTuple	familytup;
	Form_pg_opfamily familyform;
	char	   *opfamilyname;
	CatCList   *proclist,
			   *oprlist;
	List	   *grouplist;
	OpFamilyOpFuncGroup *opclassgroup;
	List	   *familytypes;
	int			i;
	ListCell   *lc;

	/* Fetch opclass information */
	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opclassname = NameStr(classform->opcname);

	/* Fetch opfamily information */
	familytup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfamilyoid));
	if (!HeapTupleIsValid(familytup))
		elog(ERROR, "cache lookup failed for operator family %u", opfamilyoid);
	familyform = (Form_pg_opfamily) GETSTRUCT(familytup);

	opfamilyname = NameStr(familyform->opfname);

	/* Fetch all operators and support functions of the opfamily */
	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));

	/* Check individual support functions */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);
		bool		ok;

		/* Check procedure numbers and function signatures */
		switch (procform->amprocnum)
		{
			case BTORDER_PROC:
				ok = check_amproc_signature(procform->amproc, INT4OID, true,
											2, 2, procform->amproclefttype,
											procform->amprocrighttype);
				break;
			case BTSORTSUPPORT_PROC:
				ok = check_amproc_signature(procform->amproc, VOIDOID, true,
											1, 1, INTERNALOID);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("btree operator family \"%s\" contains function %s with invalid support number %d",
								opfamilyname,
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				continue;		/* don't want additional message */
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree operator family \"%s\" contains function %s with wrong signature for support number %d",
							opfamilyname,
							format_procedure(procform->amproc),
							procform->amprocnum)));
			result = false;
		}
	}

	/* Check individual operators */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* Check that only allowed strategy numbers exist */
		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > BTMaxStrategyNumber)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree operator family \"%s\" contains operator %s with invalid strategy number %d",
							opfamilyname,
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* btree doesn't support ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree operator family \"%s\" contains invalid ORDER BY specification for operator %s",
							opfamilyname,
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* Check operator signature --- same for all btree strategies */
		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree operator family \"%s\" contains operator %s with wrong signature",
							opfamilyname,
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* Now check for inconsistent groups of operators/functions */
	grouplist = identify_opfamily_groups(oprlist, proclist);
	opclassgroup = NULL;
	familytypes = NIL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		/* Remember the group exactly matching the test opclass */
		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * Identify all distinct data types handled in this opfamily.  This
		 * implementation is O(N^2), but there aren't likely to be enough
		 * types in the family for it to matter.
		 */
		familytypes = list_append_unique_oid(familytypes, thisgroup->lefttype);
		familytypes = list_append_unique_oid(familytypes, thisgroup->righttype);

		/*
		 * Complain if there seems to be an incomplete set of either operators
		 * or support functions for this datatype pair.  The only thing that
		 * is considered optional is the sortsupport function.
		 */
		if (thisgroup->operatorset !=
			((1 << BTLessStrategyNumber) |
			 (1 << BTLessEqualStrategyNumber) |
			 (1 << BTEqualStrategyNumber) |
			 (1 << BTGreaterEqualStrategyNumber) |
			 (1 << BTGreaterStrategyNumber)))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree operator family \"%s\" is missing operator(s) for types %s and %s",
							opfamilyname,
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
		if ((thisgroup->functionset & (1 << BTORDER_PROC)) == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("btree operator family \"%s\" is missing support function for types %s and %s",
							opfamilyname,
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
	}

	/* Check that the originally-named opclass is supported */
	/* (if group is there, we already checked it adequately above) */
	if (!opclassgroup)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("btree operator class \"%s\" is missing operator(s)",
						opclassname)));
		result = false;
	}

	/*
	 * Complain if the opfamily doesn't have entries for all possible
	 * combinations of its supported datatypes.  While missing cross-type
	 * operators are not fatal, they do limit the planner's ability to derive
	 * additional qual clauses from equivalence classes, so it seems
	 * reasonable to insist that all built-in btree opfamilies be complete.
	 */
	if (list_length(grouplist) !=
		list_length(familytypes) * list_length(familytypes))
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("btree operator family \"%s\" is missing cross-type operator(s)",
						opfamilyname)));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}
