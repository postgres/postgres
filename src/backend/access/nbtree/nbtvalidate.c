/*-------------------------------------------------------------------------
 *
 * nbtvalidate.c
 *	  Opclass validator for btree.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
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
#include "access/xact.h"
#include "catalog/pg_am.h"
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
	int			usefulgroups;
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
			case BTINRANGE_PROC:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											5, 5,
											procform->amproclefttype,
											procform->amproclefttype,
											procform->amprocrighttype,
											BOOLOID, BOOLOID);
				break;
			case BTEQUALIMAGE_PROC:
				ok = check_amproc_signature(procform->amproc, BOOLOID, true,
											1, 1, OIDOID);
				break;
			case BTOPTIONS_PROC:
				ok = check_amoptsproc_signature(procform->amproc);
				break;
			default:
				ereport(INFO,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("operator family \"%s\" of access method %s contains function %s with invalid support number %d",
								opfamilyname, "btree",
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
							opfamilyname, "btree",
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
					 errmsg("operator family \"%s\" of access method %s contains operator %s with invalid strategy number %d",
							opfamilyname, "btree",
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
					 errmsg("operator family \"%s\" of access method %s contains invalid ORDER BY specification for operator %s",
							opfamilyname, "btree",
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
					 errmsg("operator family \"%s\" of access method %s contains operator %s with wrong signature",
							opfamilyname, "btree",
							format_operator(oprform->amopopr))));
			result = false;
		}
	}

	/* Now check for inconsistent groups of operators/functions */
	grouplist = identify_opfamily_groups(oprlist, proclist);
	usefulgroups = 0;
	opclassgroup = NULL;
	familytypes = NIL;
	foreach(lc, grouplist)
	{
		OpFamilyOpFuncGroup *thisgroup = (OpFamilyOpFuncGroup *) lfirst(lc);

		/*
		 * It is possible for an in_range support function to have a RHS type
		 * that is otherwise irrelevant to the opfamily --- for instance, SQL
		 * requires the datetime_ops opclass to have range support with an
		 * interval offset.  So, if this group appears to contain only an
		 * in_range function, ignore it: it doesn't represent a pair of
		 * supported types.
		 */
		if (thisgroup->operatorset == 0 &&
			thisgroup->functionset == (1 << BTINRANGE_PROC))
			continue;

		/* Else count it as a relevant group */
		usefulgroups++;

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
		 * or support functions for this datatype pair.  The sortsupport,
		 * in_range, and equalimage functions are considered optional.
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
					 errmsg("operator family \"%s\" of access method %s is missing operator(s) for types %s and %s",
							opfamilyname, "btree",
							format_type_be(thisgroup->lefttype),
							format_type_be(thisgroup->righttype))));
			result = false;
		}
		if ((thisgroup->functionset & (1 << BTORDER_PROC)) == 0)
		{
			ereport(INFO,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("operator family \"%s\" of access method %s is missing support function for types %s and %s",
							opfamilyname, "btree",
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
						opclassname, "btree")));
		result = false;
	}

	/*
	 * Complain if the opfamily doesn't have entries for all possible
	 * combinations of its supported datatypes.  While missing cross-type
	 * operators are not fatal, they do limit the planner's ability to derive
	 * additional qual clauses from equivalence classes, so it seems
	 * reasonable to insist that all built-in btree opfamilies be complete.
	 */
	if (usefulgroups != (list_length(familytypes) * list_length(familytypes)))
	{
		ereport(INFO,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("operator family \"%s\" of access method %s is missing cross-type operator(s)",
						opfamilyname, "btree")));
		result = false;
	}

	ReleaseCatCacheList(proclist);
	ReleaseCatCacheList(oprlist);
	ReleaseSysCache(familytup);
	ReleaseSysCache(classtup);

	return result;
}

/*
 * Prechecking function for adding operators/functions to a btree opfamily.
 */
void
btadjustmembers(Oid opfamilyoid,
				Oid opclassoid,
				List *operators,
				List *functions)
{
	Oid			opcintype;
	ListCell   *lc;

	/*
	 * Btree operators and comparison support functions are always "loose"
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

		if (op->is_func && op->number != BTORDER_PROC)
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
				opclassoid = opclass_for_family_datatype(BTREE_AM_OID,
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
