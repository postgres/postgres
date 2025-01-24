/*-------------------------------------------------------------------------
 *
 * gistvalidate.c
 *	  Opclass validator for GiST.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/gist_private.h"
#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_type.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"


/*
 * Validator for a GiST opclass.
 */
bool
gistvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	Oid			opckeytype;
	char	   *opclassname;
	char	   *opfamilyname;
	CatCList   *proclist,
			   *oprlist;
	List	   *grouplist;
	OpFamilyOpFuncGroup *opclassgroup;
	int			i;
	ListCell   *lc;

	/* Fetch opclass information */
	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opckeytype = classform->opckeytype;
	if (!OidIsValid(opckeytype))
		opckeytype = opcintype;
	opclassname = NameStr(classform->opcname);

	/* Fetch opfamily information */
	opfamilyname = get_opfamily_name(opfamilyoid, false);

	/* Fetch all operators and support functions of the opfamily */
	oprlist = SearchSysCacheList1(AMOPSTRATEGY, ObjectIdGetDatum(opfamilyoid));
	proclist = SearchSysCacheList1(AMPROCNUM, ObjectIdGetDatum(opfamilyoid));

	/* Check individual support functions */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);
		bool		ok;

		/*
		 * All GiST support functions should be registered with matching
		 * left/right types
		 */
		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "gist",
							format_procedure(procform->amproc))));
			result = false;
		}

		/*
		 * We can't check signatures except within the specific opclass, since
		 * we need to know the associated opckeytype in many cases.
		 */
		if (procform->amproclefttype != opcintype)
			continue;

		/* Check procedure numbers and function signatures */
		switch (procform->amprocnum)
		{
			case GIST_CONSISTENT_PROC:
				ok = check_amproc_signature(procform->amproc, BOOLOID, false,
											5, 5, INTERNALOID, opcintype,
											INT2OID, OIDOID, INTERNALOID);
				break;
			case GIST_UNION_PROC:
				ok = check_amproc_signature(procform->amproc, opckeytype, false,
											2, 2, INTERNALOID, INTERNALOID);
				break;
			case GIST_COMPRESS_PROC:
			case GIST_DECOMPRESS_PROC:
			case GIST_FETCH_PROC:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, true,
											1, 1, INTERNALOID);
				break;
			case GIST_PENALTY_PROC:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, true,
											3, 3, INTERNALOID,
											INTERNALOID, INTERNALOID);
				break;
			case GIST_PICKSPLIT_PROC:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, true,
											2, 2, INTERNALOID, INTERNALOID);
				break;
			case GIST_EQUAL_PROC:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, false,
											3, 3, opckeytype, opckeytype,
											INTERNALOID);
				break;
			case GIST_DISTANCE_PROC:
				ok = check_amproc_signature(procform->amproc, FLOAT8OID, false,
											5, 5, INTERNALOID, opcintype,
											INT2OID, OIDOID, INTERNALOID);
				break;
			case GIST_OPTIONS_PROC:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			case GIST_SORTSUPPORT_PROC:
				ok = check_amproc_signature(procform->amproc, VOIDOID, true,
											1, 1, INTERNALOID);
				break;
			case GIST_STRATNUM_PROC:
				ok = check_amproc_signature(procform->amproc, INT2OID, true,
											1, 1, INT4OID);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "gist",
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				continue;		/* don't want additional message */
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
							opfamilyname, "gist",
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
		Oid			op_rettype;

		/* TODO: Check that only allowed strategy numbers exist */
		if (oprform->amopstrategy < 1)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "gist",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* GiST supports ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH)
		{
			/* ... but must have matching distance proc */
			if (!OidIsValid(get_opfamily_proc(opfamilyoid,
											  oprform->amoplefttype,
											  oprform->amoplefttype,
											  GIST_DISTANCE_PROC)))
			{
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains unsupported ORDER BY specification for operator %s",
								opfamilyname, "gist",
								format_operator(oprform->amopopr))));
				result = false;
			}
			/* ... and operator result must match the claimed btree opfamily */
			op_rettype = get_op_rettype(oprform->amopopr);
			if (!opfamily_can_sort_type(oprform->amopsortfamily, op_rettype))
			{
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains incorrect ORDER BY opfamily specification for operator %s",
								opfamilyname, "gist",
								format_operator(oprform->amopopr))));
				result = false;
			}
		}
		else
		{
			/* Search operators must always return bool */
			op_rettype = BOOLOID;
		}

		/* Check operator signature */
		if (!check_amop_signature(oprform->amopopr, op_rettype,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "gist",
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* Now check for inconsistent groups of operators/functions */
	grouplist = identify_opfamily_groups(oprlist, proclist);
	opclassgroup = NULL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		/* Remember the group exactly matching the test opclass */
		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * There is not a lot we can do to check the operator sets, since each
		 * GiST opclass is more or less a law unto itself, and some contain
		 * only operators that are binary-compatible with the opclass datatype
		 * (meaning that empty operator sets can be OK).  That case also means
		 * that we shouldn't insist on nonempty function sets except for the
		 * opclass's own group.
		 */
	}

	/* Check that the originally-named opclass is complete */
	for (i = 1; i <= GISTNProcs; i++)
	{
		if (opclassgroup &&
			(opclassgroup->functionset & (((uint64) 1) << i)) != 0)
			continue;			/* got it */
		if (i == GIST_DISTANCE_PROC || i == GIST_FETCH_PROC ||
			i == GIST_COMPRESS_PROC || i == GIST_DECOMPRESS_PROC ||
			i == GIST_OPTIONS_PROC || i == GIST_SORTSUPPORT_PROC ||
			i == GIST_STRATNUM_PROC)
			continue;			/* optional methods */
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing support function %d",
						opclassname, "gist", i)));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * Prechecking function for adding operators/functions to a GiST opfamily.
 */
void
gistadjustmembers(Oid opfamilyoid,
				  Oid opclassoid,
				  List *operators,
				  List *functions)
{
	ListCell   *lc;

	/*
	 * Operator members of a GiST opfamily should never have hard
	 * dependencies, since their connection to the opfamily depends only on
	 * what the support functions think, and that can be altered.  For
	 * consistency, we make all soft dependencies point to the opfamily,
	 * though a soft dependency on the opclass would work as well in the
	 * CREATE OPERATOR CLASS case.
	 */
	foreach(lc, operators)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		op->ref_is_hard = false;
		op->ref_is_family = true;
		op->refobjid = opfamilyoid;
	}

	/*
	 * Required support functions should have hard dependencies.  Preferably
	 * those are just dependencies on the opclass, but if we're in ALTER
	 * OPERATOR FAMILY, we leave the dependency pointing at the whole
	 * opfamily.  (Given that GiST opclasses generally don't share opfamilies,
	 * it seems unlikely to be worth working harder.)
	 */
	foreach(lc, functions)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		switch (op->number)
		{
			case GIST_CONSISTENT_PROC:
			case GIST_UNION_PROC:
			case GIST_PENALTY_PROC:
			case GIST_PICKSPLIT_PROC:
			case GIST_EQUAL_PROC:
				/* Required support function */
				op->ref_is_hard = true;
				break;
			case GIST_COMPRESS_PROC:
			case GIST_DECOMPRESS_PROC:
			case GIST_DISTANCE_PROC:
			case GIST_FETCH_PROC:
			case GIST_OPTIONS_PROC:
			case GIST_SORTSUPPORT_PROC:
			case GIST_STRATNUM_PROC:
				/* Optional, so force it to be a soft family dependency */
				op->ref_is_hard = false;
				op->ref_is_family = true;
				op->refobjid = opfamilyoid;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("support function number %d is invalid for access method %s",
								op->number, "gist")));
				break;
		}
	}
}
