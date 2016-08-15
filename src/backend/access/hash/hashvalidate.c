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

#include "access/amvalidate.h"
#include "access/hash.h"
#include "access/htup_details.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"


static bool check_hash_func_signature(Oid funcid, Oid restype, Oid argtype);


/*
 * Validator for a hash opclass.
 *
 * Some of the checks done here cover the whole opfamily, and therefore are
 * redundant when checking each opclass in a family.  But they don't run long
 * enough to be much of a problem, so we accept the duplication rather than
 * complicate the amvalidate API.
 */
bool
hashvalidate(Oid opclassoid)
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
	List	   *hashabletypes = NIL;
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

		/*
		 * All hash functions should be registered with matching left/right
		 * types
		 */
		if (procform->amproclefttype != procform->amprocrighttype)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash operator family \"%s\" contains support procedure %s with cross-type registration",
							opfamilyname,
							format_procedure(procform->amproc))));
			result = false;
		}

		/* Check procedure numbers and function signatures */
		switch (procform->amprocnum)
		{
			case HASHPROC:
				if (!check_hash_func_signature(procform->amproc, INT4OID,
											   procform->amproclefttype))
				{
					ereport(INFO,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("hash operator family \"%s\" contains function %s with wrong signature for support number %d",
									opfamilyname,
									format_procedure(procform->amproc),
									procform->amprocnum)));
					result = false;
				}
				else
				{
					/* Remember which types we can hash */
					hashabletypes =
						list_append_unique_oid(hashabletypes,
											   procform->amproclefttype);
				}
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("hash operator family \"%s\" contains function %s with invalid support number %d",
								opfamilyname,
								format_procedure(procform->amproc),
								procform->amprocnum)));
				result = false;
				break;
		}
	}

	/* Check individual operators */
	for (i = 0; i < oprlist->n_members; i++)
	{
		HeapTuple	oprtup = &oprlist->members[i]->tuple;
		Form_pg_amop oprform = (Form_pg_amop) GETSTRUCT(oprtup);

		/* Check that only allowed strategy numbers exist */
		if (oprform->amopstrategy < 1 ||
			oprform->amopstrategy > HTMaxStrategyNumber)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash operator family \"%s\" contains operator %s with invalid strategy number %d",
							opfamilyname,
							format_operator(oprform->amopopr),
							oprform->amopstrategy)));
			result = false;
		}

		/* hash doesn't support ORDER BY operators */
		if (oprform->amoppurpose != AMOP_SEARCH ||
			OidIsValid(oprform->amopsortfamily))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash operator family \"%s\" contains invalid ORDER BY specification for operator %s",
							opfamilyname,
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* Check operator signature --- same for all hash strategies */
		if (!check_amop_signature(oprform->amopopr, BOOLOID,
								  oprform->amoplefttype,
								  oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash operator family \"%s\" contains operator %s with wrong signature",
							opfamilyname,
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* There should be relevant hash procedures for each datatype */
		if (!list_member_oid(hashabletypes, oprform->amoplefttype) ||
			!list_member_oid(hashabletypes, oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash operator family \"%s\" lacks support function for operator %s",
							opfamilyname,
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
		 * Complain if there seems to be an incomplete set of operators for
		 * this datatype pair (implying that we have a hash function but no
		 * operator).
		 */
		if (thisgroup->operatorset != (1 << HTEqualStrategyNumber))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("hash operator family \"%s\" is missing operator(s) for types %s and %s",
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
				 errmsg("hash operator class \"%s\" is missing operator(s)",
						opclassname)));
		result = false;
	}

	/*
	 * Complain if the opfamily doesn't have entries for all possible
	 * combinations of its supported datatypes.  While missing cross-type
	 * operators are not fatal, it seems reasonable to insist that all
	 * built-in hash opfamilies be complete.
	 */
	if (list_length(grouplist) !=
		list_length(hashabletypes) * list_length(hashabletypes))
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("hash operator family \"%s\" is missing cross-type operator(s)",
						opfamilyname)));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}


/*
 * We need a custom version of check_amproc_signature because of assorted
 * hacks in the core hash opclass definitions.
 */
static bool
check_hash_func_signature(Oid funcid, Oid restype, Oid argtype)
{
	bool		result = true;
	HeapTuple	tp;
	Form_pg_proc procform;

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(tp);

	if (procform->prorettype != restype || procform->proretset ||
		procform->pronargs != 1)
		result = false;

	if (!IsBinaryCoercible(argtype, procform->proargtypes.values[0]))
	{
		/*
		 * Some of the built-in hash opclasses cheat by using hash functions
		 * that are different from but physically compatible with the opclass
		 * datatype.  In some of these cases, even a "binary coercible" check
		 * fails because there's no relevant cast.  For the moment, fix it by
		 * having a whitelist of allowed cases.  Test the specific function
		 * identity, not just its input type, because hashvarlena() takes
		 * INTERNAL and allowing any such function seems too scary.
		 */
		if (funcid == F_HASHINT4 &&
			(argtype == DATEOID ||
			 argtype == ABSTIMEOID || argtype == RELTIMEOID ||
			 argtype == XIDOID || argtype == CIDOID))
			 /* okay, allowed use of hashint4() */ ;
		else if (funcid == F_TIMESTAMP_HASH &&
				 argtype == TIMESTAMPTZOID)
			 /* okay, allowed use of timestamp_hash() */ ;
		else if (funcid == F_HASHCHAR &&
				 argtype == BOOLOID)
			 /* okay, allowed use of hashchar() */ ;
		else if (funcid == F_HASHVARLENA &&
				 argtype == BYTEAOID)
			 /* okay, allowed use of hashvarlena() */ ;
		else
			result = false;
	}

	ReleaseSysCache(tp);
	return result;
}
