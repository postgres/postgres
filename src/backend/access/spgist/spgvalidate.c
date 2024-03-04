/*-------------------------------------------------------------------------
 *
 * spgvalidate.c
 *	  Opclass validator for SP-GiST.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgvalidate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/htup_details.h"
#include "access/spgist.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"


/*
 * Validator for an SP-GiST opclass.
 *
 * Some of the checks done here cover the whole opfamily, and therefore are
 * redundant when checking each opclass in a family.  But they don't run long
 * enough to be much of a problem, so we accept the duplication rather than
 * complicate the amvalidate API.
 */
bool
spgvalidate(Oid opclassoid)
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
	spgConfigIn configIn;
	spgConfigOut configOut;
	Oid			configOutLefttype = InvalidOid;
	Oid			configOutRighttype = InvalidOid;
	Oid			configOutLeafType = InvalidOid;

	/* Fetch opclass information */
	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	opfamilyoid = classform->opcfamily;
	opcintype = classform->opcintype;
	opckeytype = classform->opckeytype;
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
	grouplist = identify_opfamily_groups(oprlist, proclist);

	/* Check individual support functions */
	for (i = 0; i < proclist->n_members; i++)
	{
		HeapTuple	proctup = &proclist->members[i]->tuple;
		Form_pg_amproc procform = (Form_pg_amproc) GETSTRUCT(proctup);
		bool		ok;

		/*
		 * All SP-GiST support functions should be registered with matching
		 * left/right types
		 */
		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "spgist",
							format_procedure(procform->amproc))));
			result = false;
		}

		/* Check procedure numbers and function signatures */
		switch (procform->amprocnum)
		{
			case SPGIST_CONFIG_PROC:
				ok = check_amproc_signature(procform->amproc, VOIDOID, true,
											2, 2, INTERNALOID, INTERNALOID);
				configIn.attType = procform->amproclefttype;
				memset(&configOut, 0, sizeof(configOut));

				OidFunctionCall2(procform->amproc,
								 PointerGetDatum(&configIn),
								 PointerGetDatum(&configOut));

				configOutLefttype = procform->amproclefttype;
				configOutRighttype = procform->amprocrighttype;

				/* Default leaf type is opckeytype or input type */
				if (OidIsValid(opckeytype))
					configOutLeafType = opckeytype;
				else
					configOutLeafType = procform->amproclefttype;

				/* If some other leaf datum type is specified, warn */
				if (OidIsValid(configOut.leafType) &&
					configOutLeafType != configOut.leafType)
				{
					ereport(INFO,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("SP-GiST leaf data type %s does not match declared type %s",
									format_type_be(configOut.leafType),
									format_type_be(configOutLeafType))));
					result = false;
					configOutLeafType = configOut.leafType;
				}

				/*
				 * When leaf and attribute types are the same, compress
				 * function is not required and we set corresponding bit in
				 * functionset for later group consistency check.
				 */
				if (configOutLeafType == configIn.attType)
				{
					foreach(lc, grouplist)
					{
						OpFamilyOpFuncGroup *group = lfirst(lc);

						if (group->lefttype == procform->amproclefttype &&
							group->righttype == procform->amprocrighttype)
						{
							group->functionset |=
								((uint64) 1) << SPGIST_COMPRESS_PROC;
							break;
						}
					}
				}
				break;
			case SPGIST_CHOOSE_PROC:
			case SPGIST_PICKSPLIT_PROC:
			case SPGIST_INNER_CONSISTENT_PROC:
				ok = check_amproc_signature(procform->amproc, VOIDOID, true,
											2, 2, INTERNALOID, INTERNALOID);
				break;
			case SPGIST_LEAF_CONSISTENT_PROC:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											2, 2, INTERNALOID, INTERNALOID);
				break;
			case SPGIST_COMPRESS_PROC:
				if (configOutLefttype != procform->amproclefttype ||
					configOutRighttype != procform->amprocrighttype)
					ok = false;
				else
					ok = check_amproc_signature(procform->amproc,
												configOutLeafType, true,
												1, 1, procform->amproclefttype);
				break;
			case SPGIST_OPTIONS_PROC:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "spgist",
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
							opfamilyname, "spgist",
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
		if (oprform->amopstrategy < 1 || oprform->amopstrategy > 63)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "spgist",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* spgist supports ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH)
		{
			/* ... and operator result must match the claimed btree opfamily */
			op_rettype = get_op_rettype(oprform->amopopr);
			if (!opfamily_can_sort_type(oprform->amopsortfamily, op_rettype))
			{
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
								opfamilyname, "spgist",
								format_operator(oprform->amopopr))));
				result = false;
			}
		}
		else
			op_rettype = BOOLOID;

		/* Check operator signature --- same for all spgist strategies */
		if (!check_amop_signature(oprform->amopopr, op_rettype,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "spgist",
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* Now check for inconsistent groups of operators/functions */
	opclassgroup = NULL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		/* Remember the group exactly matching the test opclass */
		if (thisgroup->lefttype == opcintype &&
			thisgroup->righttype == opcintype)
			opclassgroup = thisgroup;

		/*
		 * Complain if there are any datatype pairs with functions but no
		 * operators.  This is about the best we can do for now to detect
		 * missing operators.
		 */
		if (thisgroup->operatorset == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "spgist",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}

		/*
		 * Complain if we're missing functions for any datatype, remembering
		 * that SP-GiST doesn't use cross-type support functions.
		 */
		if (thisgroup->lefttype != thisgroup->righttype)
			continue;

		for (i = 1; i <= SPGISTNProc; i++)
		{
			if ((thisgroup->functionset & (((uint64) 1) << i)) != 0)
				continue;		/* got it */
			if (i == SPGIST_OPTIONS_PROC)
				continue;		/* optional method */
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing support function %d for type %s",
							opfamilyname, "spgist", i,
							format_type_be(thisgroup->lefttype))));
			result = false;
		}
	}

	/* Check that the originally-named opclass is supported */
	/* (if group is there, we already checked it adequately above) */
	if (!opclassgroup)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
						opclassname, "spgist")));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * Prechecking function for adding operators/functions to an SP-GiST opfamily.
 */
void
spgadjustmembers(Oid opfamilyoid,
				 Oid opclassoid,
				 List *operators,
				 List *functions)
{
	ListCell   *lc;

	/*
	 * Operator members of an SP-GiST opfamily should never have hard
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
	 * opfamily.  (Given that SP-GiST opclasses generally don't share
	 * opfamilies, it seems unlikely to be worth working harder.)
	 */
	foreach(lc, functions)
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		switch (op->number)
		{
			case SPGIST_CONFIG_PROC:
			case SPGIST_CHOOSE_PROC:
			case SPGIST_PICKSPLIT_PROC:
			case SPGIST_INNER_CONSISTENT_PROC:
			case SPGIST_LEAF_CONSISTENT_PROC:
				/* Required support function */
				op->ref_is_hard = true;
				break;
			case SPGIST_COMPRESS_PROC:
			case SPGIST_OPTIONS_PROC:
				/* Optional, so force it to be a soft family dependency */
				op->ref_is_hard = false;
				op->ref_is_family = true;
				op->refobjid = opfamilyoid;
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("support function number %d is invalid for access method %s",
								op->number, "spgist")));
				break;
		}
	}
}
