/*-------------------------------------------------------------------------
 *
 * brin_validate.c
 *	  Opclass validator for BRIN.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_validate.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/amvalidate.h"
#include "access/brin_internal.h"
#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/regproc.h"
#include "utils/syscache.h"

/*
 * Validator for a BRIN opclass.
 *
 * Some of the checks done here cover the whole opfamily, and therefore are
 * redundant when checking each opclass in a family.  But they don't run long
 * enough to be much of a problem, so we accept the duplication rather than
 * complicate the amvalidate API.
 */
bool
brinvalidate(Oid opclassoid)
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
	uint64		allfuncs = 0;
	uint64		allops = 0;
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
			case BRIN_PROCNUM_OPCINFO:
				ok = check_amproc_signature(procform->amproc, INTERNALOID, true,
											1, 1, INTERNALOID);
				break;
			case BRIN_PROCNUM_ADDVALUE:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											4, 4, INTERNALOID, INTERNALOID,
											INTERNALOID, INTERNALOID);
				break;
			case BRIN_PROCNUM_CONSISTENT:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											3, 4, INTERNALOID, INTERNALOID,
											INTERNALOID, INT4OID);
				break;
			case BRIN_PROCNUM_UNION:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											3, 3, INTERNALOID, INTERNALOID,
											INTERNALOID);
				break;
			case BRIN_PROCNUM_OPTIONS:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			default:
				/* Complain if it's not a valid optional proc number */
				if (procform->amprocnum < BRIN_FIRST_OPTIONAL_PROCNUM ||
					procform->amprocnum > BRIN_LAST_OPTIONAL_PROCNUM)
				{
					ereport(INFO,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
									opfamilyname, "brin",
									format_procedure(procform->amproc),
									procform->amprocnum)));
					result = false;
					continue;	/* omit bad proc numbers from allfuncs */
				}
				/* Can't check signatures of optional procs, so assume OK */
				ok = true;
				break;
		}

		if (!ok)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
							opfamilyname, "brin",
							format_procedure(procform->amproc),
							procform->amprocnum)));
			result = false;
		}

		/* Track all valid procedure numbers seen in opfamily */
		allfuncs |= ((uint64) 1) << procform->amprocnum;
	}

	/* Check individual operators */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* Check that only allowed strategy numbers exist */
		if (oprform->amopstrategy < 1 || oprform->amopstrategy > 63)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "brin",
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}
		else
		{
			/*
			 * The set of operators supplied varies across BRIN opfamilies.
			 * Our plan is to identify all operator strategy numbers used in
			 * the opfamily and then complain about datatype combinations that
			 * are missing any operator(s).  However, consider only numbers
			 * that appear in some non-cross-type case, since cross-type
			 * operators may have unique strategies.  (This is not a great
			 * heuristic, in particular an erroneous number used in a
			 * cross-type operator will not get noticed; but the core BRIN
			 * opfamilies are messy enough to make it necessary.)
			 */
			if (oprform->amoplefttype == oprform->amoprighttype)
				allops |= ((uint64) 1) << oprform->amopstrategy;
		}

		/* brin doesn't support ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "brin",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* Check operator signature --- same for all brin strategies */
		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "brin",
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
		 * Some BRIN opfamilies expect cross-type support functions to exist,
		 * and some don't.  We don't know exactly which are which, so if we
		 * find a cross-type operator for which there are no support functions
		 * at all, let it pass.  (Don't expect that all operators exist for
		 * such cross-type cases, either.)
		 */
		if (thisgroup->functionset == 0 &&
			thisgroup->lefttype != thisgroup->righttype)
			continue;

		/*
		 * Else complain if there seems to be an incomplete set of either
		 * operators or support functions for this datatype pair.
		 */
		if (thisgroup->operatorset != allops)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "brin",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
		if (thisgroup->functionset != allfuncs)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing support function(s) for types %s and %s",
							opfamilyname, "brin",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
	}

	/* Check that the originally-named opclass is complete */
	if (!opclassgroup || opclassgroup->operatorset != allops)
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
						opclassname, "brin")));
		result = false;
	}
	for (i = 1; i <= BRIN_MANDATORY_NPROCS; i++)
	{
		if (opclassgroup &&
			(opclassgroup->functionset & (((int64) 1) << i)) != 0)
			continue;			/* got it */
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator class \"%s\" of access method %s is missing support function %d",
						opclassname, "brin", i)));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}
