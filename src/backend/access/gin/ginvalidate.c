/*-------------------------------------------------------------------------
 *
 * ginvalidate.c
 *	  Opclass validator for GIN.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/gin/ginvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/gin_private.h"
#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

/*
 * Validator for a GIN opclass.
 */
bool
ginvalidate(Oid opclassoid)
{
	bool		result = true;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			opfamilyoid;
	Oid			opcintype;
	Oid			opckeytype;
	char	   *opclassname;
	HeapTuple	familytup;
	Form_pg_opfamily familyform;
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

		/*
		 * All GIN support functions should be registered with matching
		 * left/right types
		 */
		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "gin",
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
			case GIN_COMPARE_PROC:
				ok = check_amproc_signature(procform->amproc, INT4OID, false,
											2, 2, opckeytype, opckeytype);
				break;
			case GIN_EXTRACTVALUE_PROC:
				/* Some opclasses omit nullFlags */
				ok = check_amproc_signature(procform->amproc, INTERNALOID, false,
											2, 3, opcintype, INTERNALOID,
											INTERNALOID);
				break;
			case GIN_EXTRACTQUERY_PROC:
				/* Some opclasses omit nullFlags and searchMode */
				ok = check_amproc_signature(procform->amproc, INTERNALOID, false,
											5, 7, opcintype, INTERNALOID,
											INT2OID, INTERNALOID, INTERNALOID,
											INTERNALOID, INTERNALOID);
				break;
			case GIN_CONSISTENT_PROC:
				/* Some opclasses omit queryKeys and nullFlags */
				ok = check_amproc_signature(procform->amproc, BOOLOID, false,
											6, 8, INTERNALOID, INT2OID,
											opcintype, INT4OID,
											INTERNALOID, INTERNALOID,
											INTERNALOID, INTERNALOID);
				break;
			case GIN_COMPARE_PARTIAL_PROC:
				ok = check_amproc_signature(procform->amproc, INT4OID, false,
											4, 4, opckeytype, opckeytype,
											INT2OID, INTERNALOID);
				break;
			case GIN_TRICONSISTENT_PROC:
				ok = check_amproc_signature(procform->amproc, CHAROID, false,
											7, 7, INTERNALOID, INT2OID,
											opcintype, INT4OID,
											INTERNALOID, INTERNALOID,
											INTERNALOID);
				break;
			case GIN_OPTIONS_PROC:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "gin",
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
							opfamilyname, "gin",
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

		/* TODO: Check that only allowed strategy numbers exist */
		if (oprform->amopstrategy < 1 || oprform->amopstrategy > 63)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "gin",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* gin doesn't support ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "gin",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* Check operator signature --- same for all gin strategies */
		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "gin",
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
		 * GIN opclass is more or less a law unto itself, and some contain
		 * only operators that are binary-compatible with the opclass datatype
		 * (meaning that empty operator sets can be OK).  That case also means
		 * that we shouldn't insist on nonempty function sets except for the
		 * opclass's own group.
		 */
	}

	/* Check that the originally-named opclass is complete */
	for (i = 1; i <= GINNProcs; i++)
	{
		if (opclassgroup &&
			(opclassgroup->functionset & (((uint64) 1) << i)) != 0)
			continue;			/* got it */
		if (i == GIN_COMPARE_PROC || i == GIN_COMPARE_PARTIAL_PROC ||
			i == GIN_OPTIONS_PROC)
			continue;			/* optional method */
		if (i == GIN_CONSISTENT_PROC || i == GIN_TRICONSISTENT_PROC)
			continue;			/* don't need both, see check below loop */
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing support function %d",
						opclassname, "gin", i)));
		result = false;
	}
	if (!opclassgroup ||
		((opclassgroup->functionset & (1 << GIN_CONSISTENT_PROC)) == 0 &&
		 (opclassgroup->functionset & (1 << GIN_TRICONSISTENT_PROC)) == 0))
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing support function %d or %d",
						opclassname, "gin",
						GIN_CONSISTENT_PROC, GIN_TRICONSISTENT_PROC)));
		result = false;
	}


	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * Prechecking function for adding operators/functions to a GIN opfamily.
 */
void
ginadjustmembers(Oid opfamilyoid,
				 Oid opclassoid,
				 List *operators,
				 List *functions)
{
	ListCell   *lc;

	/*
	 * Operator members of a GIN opfamily should never have hard dependencies,
	 * since their connection to the opfamily depends only on what the support
	 * functions think, and that can be altered.  For consistency, we make all
	 * soft dependencies point to the opfamily, though a soft dependency on
	 * the opclass would work as well in the CREATE OPERATOR CLASS case.
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
	 * opfamily.  (Given that GIN opclasses generally don't share opfamilies,
	 * it seems unlikely to be worth working harder.)
	 */
	foreach(lc, functions)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		switch (op->number)
		{
			case GIN_EXTRACTVALUE_PROC:
			case GIN_EXTRACTQUERY_PROC:
				/* Required support function */
				op->ref_is_hard = true;
				break;
			case GIN_COMPARE_PROC:
			case GIN_CONSISTENT_PROC:
			case GIN_COMPARE_PARTIAL_PROC:
			case GIN_TRICONSISTENT_PROC:
			case GIN_OPTIONS_PROC:
				/* Optional, so force it to be a soft family dependency */
				op->ref_is_hard = false;
				op->ref_is_family = true;
				op->refobjid = opfamilyoid;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("support function number %d is invalid for access method %s",
								op->number, "gin")));
				break;
		}
	}
}
