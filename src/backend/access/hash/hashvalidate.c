/*-------------------------------------------------------------------------
 *
 * hashvalidate.c
 *	  Opclass validator for hash.
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/regproc.h"
#include "utils/syscache.h"


static bool check_hash_func_signature(Oid funcid, int16 amprocnum, Oid argtype);


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
					 errmsg("operator family \"%s\" of access method %s contains support function %s with different left and right input types",
							opfamilyname, "hash",
							format_procedure(procform->amproc))));
			result = false;
		}

		/* Check procedure numbers and function signatures */
		switch (procform->amprocnum)
		{
			case HASHSTANDARD_PROC:
			case HASHEXTENDED_PROC:
				if (!check_hash_func_signature(procform->amproc, procform->amprocnum,
											   procform->amproclefttype))
				{
					ereport(INFO,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("operator family \"%s\" of access method %s contains function %s with wrong signature for support number %d",
									opfamilyname, "hash",
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
			case HASHOPTIONS_PROC:
				if (!check_amoptsproc_signature(procform->amproc))
					result = false;
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "hash",
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
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "hash",
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
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "hash",
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
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "hash",
							format_operator(oprform->amopopr))));
			result = false;
		}

		/* There should be relevant hash functions for each datatype */
		if (!list_member_oid(hashabletypes, oprform->amoplefttype) ||
			!list_member_oid(hashabletypes, oprform->amoprighttype))
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s lacks support function for operator %s",
							opfamilyname, "hash",
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
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "hash",
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
				 errmsg("operator class \"%s\" of access method %s is missing operator(s)",
						opclassname, "hash")));
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
				 errmsg("operator family \"%s\" of access method %s is missing cross-type operator(s)",
						opfamilyname, "hash")));
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
check_hash_func_signature(Oid funcid, int16 amprocnum, Oid argtype)
{
	bool		result = true;
	Oid			restype;
	int16		nargs;
	HeapTuple	tp;
	Form_pg_proc procform;

	switch (amprocnum)
	{
		case HASHSTANDARD_PROC:
			restype = INT4OID;
			nargs = 1;
			break;

		case HASHEXTENDED_PROC:
			restype = INT8OID;
			nargs = 2;
			break;

		default:
			elog(ERROR, "invalid amprocnum");
	}

	tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);
	procform = (Form_pg_proc) GETSTRUCT(tp);

	if (procform->prorettype != restype || procform->proretset ||
		procform->pronargs != nargs)
		result = false;

	if (!IsBinaryCoercible(argtype, procform->proargtypes.values[0]))
	{
		/*
		 * Some of the built-in hash opclasses cheat by using hash functions
		 * that are different from but physically compatible with the opclass
		 * datatype.  In some of these cases, even a "binary coercible" check
		 * fails because there's no relevant cast.  For the moment, fix it by
		 * having a list of allowed cases.  Test the specific function
		 * identity, not just its input type, because hashvarlena() takes
		 * INTERNAL and allowing any such function seems too scary.
		 */
		if ((funcid == F_HASHINT4 || funcid == F_HASHINT4EXTENDED) &&
			(argtype == DATEOID ||
			 argtype == XIDOID || argtype == CIDOID))
			 /* okay, allowed use of hashint4() */ ;
		else if ((funcid == F_HASHINT8 || funcid == F_HASHINT8EXTENDED) &&
				 (argtype == XID8OID))
			 /* okay, allowed use of hashint8() */ ;
		else if ((funcid == F_TIMESTAMP_HASH ||
				  funcid == F_TIMESTAMP_HASH_EXTENDED) &&
				 argtype == TIMESTAMPTZOID)
			 /* okay, allowed use of timestamp_hash() */ ;
		else if ((funcid == F_HASHCHAR || funcid == F_HASHCHAREXTENDED) &&
				 argtype == BOOLOID)
			 /* okay, allowed use of hashchar() */ ;
		else if ((funcid == F_HASHVARLENA || funcid == F_HASHVARLENAEXTENDED) &&
				 argtype == BYTEAOID)
			 /* okay, allowed use of hashvarlena() */ ;
		else
			result = false;
	}

	/* If function takes a second argument, it must be for a 64-bit salt. */
	if (nargs == 2 && procform->proargtypes.values[1] != INT8OID)
		result = false;

	ReleaseSysCache(tp);
	return result;
}

/*
 * Prechecking function for adding operators/functions to a hash opfamily.
 */
void
hashadjustmembers(Oid opfamilyoid,
				  Oid opclassoid,
				  List *operators,
				  List *functions)
{
	Oid			opcintype;
	ListCell   *lc;

	/*
	 * Hash operators and required support functions are always "loose"
	 * members of the opfamily if they are cross-type.  If they are not
	 * cross-type, we prefer to tie them to the appropriate opclass ... but if
	 * the user hasn't created one, we can't do that, and must fall back to
	 * using the opfamily dependency.  (We mustn't force creation of an
	 * opclass in such a case, as leaving an incomplete opclass laying about
	 * would be bad.  Throwing an error is another undesirable alternative.)
	 *
	 * This behavior results in a bit of a dump/reload hazard, in that the
	 * order of restoring objects could affect what dependencies we end up
	 * with.  pg_dump's existing behavior will preserve the dependency choices
	 * in most cases, but not if a cross-type operator has been bound tightly
	 * into an opclass.  That's a mistake anyway, so silently "fixing" it
	 * isn't awful.
	 *
	 * Optional support functions are always "loose" family members.
	 *
	 * To avoid repeated lookups, we remember the most recently used opclass's
	 * input type.
	 */
	if (OidIsValid(opclassoid))
	{
		/* During CREATE OPERATOR CLASS, need CCI to see the pg_opclass row */
		CommandCounterIncrement();
		opcintype = get_opclass_input_type(opclassoid);
	}
	else
		opcintype = InvalidOid;

	/*
	 * We handle operators and support functions almost identically, so rather
	 * than duplicate this code block, just join the lists.
	 */
	foreach(lc, list_concat_copy(operators, functions))
	{
		OpFamilyMember *op = (OpFamilyMember *) lfirst(lc);

		if (op->is_func && op->number != HASHSTANDARD_PROC)
		{
			/* Optional support proc, so always a soft family dependency */
			op->ref_is_hard = false;
			op->ref_is_family = true;
			op->refobjid = opfamilyoid;
		}
		else if (op->lefttype != op->righttype)
		{
			/* Cross-type, so always a soft family dependency */
			op->ref_is_hard = false;
			op->ref_is_family = true;
			op->refobjid = opfamilyoid;
		}
		else
		{
			/* Not cross-type; is there a suitable opclass? */
			if (op->lefttype != opcintype)
			{
				/* Avoid repeating this expensive lookup, even if it fails */
				opcintype = op->lefttype;
				opclassoid = opclass_for_family_datatype(HASH_AM_OID,
														 opfamilyoid,
														 opcintype);
			}
			if (OidIsValid(opclassoid))
			{
				/* Hard dependency on opclass */
				op->ref_is_hard = true;
				op->ref_is_family = false;
				op->refobjid = opclassoid;
			}
			else
			{
				/* We're stuck, so make a soft dependency on the opfamily */
				op->ref_is_hard = false;
				op->ref_is_family = true;
				op->refobjid = opfamilyoid;
			}
		}
	}
}
