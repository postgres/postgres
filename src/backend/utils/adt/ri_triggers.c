/*-------------------------------------------------------------------------
 *
 * ri_triggers.c
 *
 *	Generic trigger procedures for referential integrity constraint
 *	checks.
 *
 *	Note about memory management: the private hashtables kept here live
 *	across query and transaction boundaries, in fact they live as long as
 *	the backend does.  This works because the hashtable structures
 *	themselves are allocated by dynahash.c in its permanent DynaHashCxt,
 *	and the SPI plans they point to are saved using SPI_keepplan().
 *	There is not currently any provision for throwing away a no-longer-needed
 *	plan --- consider improving this someday.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/ri_triggers.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_proc.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rangetypes.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * Local definitions
 */

#define RI_MAX_NUMKEYS					INDEX_MAX_KEYS

#define RI_INIT_CONSTRAINTHASHSIZE		64
#define RI_INIT_QUERYHASHSIZE			(RI_INIT_CONSTRAINTHASHSIZE * 4)

#define RI_KEYS_ALL_NULL				0
#define RI_KEYS_SOME_NULL				1
#define RI_KEYS_NONE_NULL				2

/* RI query type codes */
/* these queries are executed against the PK (referenced) table: */
#define RI_PLAN_CHECK_LOOKUPPK			1
#define RI_PLAN_CHECK_LOOKUPPK_FROM_PK	2
#define RI_PLAN_LAST_ON_PK				RI_PLAN_CHECK_LOOKUPPK_FROM_PK
/* these queries are executed against the FK (referencing) table: */
#define RI_PLAN_CASCADE_ONDELETE		3
#define RI_PLAN_CASCADE_ONUPDATE		4
/* For RESTRICT, the same plan can be used for both ON DELETE and ON UPDATE triggers. */
#define RI_PLAN_RESTRICT				5
#define RI_PLAN_SETNULL_ONDELETE		6
#define RI_PLAN_SETNULL_ONUPDATE		7
#define RI_PLAN_SETDEFAULT_ONDELETE		8
#define RI_PLAN_SETDEFAULT_ONUPDATE		9

#define MAX_QUOTED_NAME_LEN  (NAMEDATALEN*2+3)
#define MAX_QUOTED_REL_NAME_LEN  (MAX_QUOTED_NAME_LEN*2)

#define RIAttName(rel, attnum)	NameStr(*attnumAttName(rel, attnum))
#define RIAttType(rel, attnum)	attnumTypeId(rel, attnum)
#define RIAttCollation(rel, attnum) attnumCollationId(rel, attnum)

#define RI_TRIGTYPE_INSERT 1
#define RI_TRIGTYPE_UPDATE 2
#define RI_TRIGTYPE_DELETE 3


/*
 * RI_ConstraintInfo
 *
 * Information extracted from an FK pg_constraint entry.  This is cached in
 * ri_constraint_cache.
 *
 * Note that pf/pp/ff_eq_oprs may hold the overlaps operator instead of equals
 * for the PERIOD part of a temporal foreign key.
 */
typedef struct RI_ConstraintInfo
{
	Oid			constraint_id;	/* OID of pg_constraint entry (hash key) */
	bool		valid;			/* successfully initialized? */
	Oid			constraint_root_id; /* OID of topmost ancestor constraint;
									 * same as constraint_id if not inherited */
	uint32		oidHashValue;	/* hash value of constraint_id */
	uint32		rootHashValue;	/* hash value of constraint_root_id */
	NameData	conname;		/* name of the FK constraint */
	Oid			pk_relid;		/* referenced relation */
	Oid			fk_relid;		/* referencing relation */
	char		confupdtype;	/* foreign key's ON UPDATE action */
	char		confdeltype;	/* foreign key's ON DELETE action */
	int			ndelsetcols;	/* number of columns referenced in ON DELETE
								 * SET clause */
	int16		confdelsetcols[RI_MAX_NUMKEYS]; /* attnums of cols to set on
												 * delete */
	char		confmatchtype;	/* foreign key's match type */
	bool		hasperiod;		/* if the foreign key uses PERIOD */
	int			nkeys;			/* number of key columns */
	int16		pk_attnums[RI_MAX_NUMKEYS]; /* attnums of referenced cols */
	int16		fk_attnums[RI_MAX_NUMKEYS]; /* attnums of referencing cols */
	Oid			pf_eq_oprs[RI_MAX_NUMKEYS]; /* equality operators (PK = FK) */
	Oid			pp_eq_oprs[RI_MAX_NUMKEYS]; /* equality operators (PK = PK) */
	Oid			ff_eq_oprs[RI_MAX_NUMKEYS]; /* equality operators (FK = FK) */
	Oid			period_contained_by_oper;	/* anyrange <@ anyrange */
	Oid			agged_period_contained_by_oper; /* fkattr <@ range_agg(pkattr) */
	dlist_node	valid_link;		/* Link in list of valid entries */
} RI_ConstraintInfo;

/*
 * RI_QueryKey
 *
 * The key identifying a prepared SPI plan in our query hashtable
 */
typedef struct RI_QueryKey
{
	Oid			constr_id;		/* OID of pg_constraint entry */
	int32		constr_queryno; /* query type ID, see RI_PLAN_XXX above */
} RI_QueryKey;

/*
 * RI_QueryHashEntry
 */
typedef struct RI_QueryHashEntry
{
	RI_QueryKey key;
	SPIPlanPtr	plan;
} RI_QueryHashEntry;

/*
 * RI_CompareKey
 *
 * The key identifying an entry showing how to compare two values
 */
typedef struct RI_CompareKey
{
	Oid			eq_opr;			/* the equality operator to apply */
	Oid			typeid;			/* the data type to apply it to */
} RI_CompareKey;

/*
 * RI_CompareHashEntry
 */
typedef struct RI_CompareHashEntry
{
	RI_CompareKey key;
	bool		valid;			/* successfully initialized? */
	FmgrInfo	eq_opr_finfo;	/* call info for equality fn */
	FmgrInfo	cast_func_finfo;	/* in case we must coerce input */
} RI_CompareHashEntry;


/*
 * Local data
 */
static HTAB *ri_constraint_cache = NULL;
static HTAB *ri_query_cache = NULL;
static HTAB *ri_compare_cache = NULL;
static dclist_head ri_constraint_cache_valid_list;


/*
 * Local function prototypes
 */
static bool ri_Check_Pk_Match(Relation pk_rel, Relation fk_rel,
							  TupleTableSlot *oldslot,
							  const RI_ConstraintInfo *riinfo);
static Datum ri_restrict(TriggerData *trigdata, bool is_no_action);
static Datum ri_set(TriggerData *trigdata, bool is_set_null, int tgkind);
static void quoteOneName(char *buffer, const char *name);
static void quoteRelationName(char *buffer, Relation rel);
static void ri_GenerateQual(StringInfo buf,
							const char *sep,
							const char *leftop, Oid leftoptype,
							Oid opoid,
							const char *rightop, Oid rightoptype);
static void ri_GenerateQualCollation(StringInfo buf, Oid collation);
static int	ri_NullCheck(TupleDesc tupDesc, TupleTableSlot *slot,
						 const RI_ConstraintInfo *riinfo, bool rel_is_pk);
static void ri_BuildQueryKey(RI_QueryKey *key,
							 const RI_ConstraintInfo *riinfo,
							 int32 constr_queryno);
static bool ri_KeysEqual(Relation rel, TupleTableSlot *oldslot, TupleTableSlot *newslot,
						 const RI_ConstraintInfo *riinfo, bool rel_is_pk);
static bool ri_CompareWithCast(Oid eq_opr, Oid typeid, Oid collid,
							   Datum lhs, Datum rhs);

static void ri_InitHashTables(void);
static void InvalidateConstraintCacheCallBack(Datum arg, int cacheid, uint32 hashvalue);
static SPIPlanPtr ri_FetchPreparedPlan(RI_QueryKey *key);
static void ri_HashPreparedPlan(RI_QueryKey *key, SPIPlanPtr plan);
static RI_CompareHashEntry *ri_HashCompareOp(Oid eq_opr, Oid typeid);

static void ri_CheckTrigger(FunctionCallInfo fcinfo, const char *funcname,
							int tgkind);
static const RI_ConstraintInfo *ri_FetchConstraintInfo(Trigger *trigger,
													   Relation trig_rel, bool rel_is_pk);
static const RI_ConstraintInfo *ri_LoadConstraintInfo(Oid constraintOid);
static Oid	get_ri_constraint_root(Oid constrOid);
static SPIPlanPtr ri_PlanCheck(const char *querystr, int nargs, Oid *argtypes,
							   RI_QueryKey *qkey, Relation fk_rel, Relation pk_rel);
static bool ri_PerformCheck(const RI_ConstraintInfo *riinfo,
							RI_QueryKey *qkey, SPIPlanPtr qplan,
							Relation fk_rel, Relation pk_rel,
							TupleTableSlot *oldslot, TupleTableSlot *newslot,
							bool detectNewRows, int expect_OK);
static void ri_ExtractValues(Relation rel, TupleTableSlot *slot,
							 const RI_ConstraintInfo *riinfo, bool rel_is_pk,
							 Datum *vals, char *nulls);
static void ri_ReportViolation(const RI_ConstraintInfo *riinfo,
							   Relation pk_rel, Relation fk_rel,
							   TupleTableSlot *violatorslot, TupleDesc tupdesc,
							   int queryno, bool partgone) pg_attribute_noreturn();


/*
 * RI_FKey_check -
 *
 * Check foreign key existence (combined for INSERT and UPDATE).
 */
static Datum
RI_FKey_check(TriggerData *trigdata)
{
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *newslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, false);

	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		newslot = trigdata->tg_newslot;
	else
		newslot = trigdata->tg_trigslot;

	/*
	 * We should not even consider checking the row if it is no longer valid,
	 * since it was either deleted (so the deferred check should be skipped)
	 * or updated (in which case only the latest version of the row should be
	 * checked).  Test its liveness according to SnapshotSelf.  We need pin
	 * and lock on the buffer to call HeapTupleSatisfiesVisibility.  Caller
	 * should be holding pin, but not lock.
	 */
	if (!table_tuple_satisfies_snapshot(trigdata->tg_relation, newslot, SnapshotSelf))
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables.
	 *
	 * pk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR KEY SHARE will get on it.
	 */
	fk_rel = trigdata->tg_relation;
	pk_rel = table_open(riinfo->pk_relid, RowShareLock);

	switch (ri_NullCheck(RelationGetDescr(fk_rel), newslot, riinfo, false))
	{
		case RI_KEYS_ALL_NULL:

			/*
			 * No further check needed - an all-NULL key passes every type of
			 * foreign key constraint.
			 */
			table_close(pk_rel, RowShareLock);
			return PointerGetDatum(NULL);

		case RI_KEYS_SOME_NULL:

			/*
			 * This is the only case that differs between the three kinds of
			 * MATCH.
			 */
			switch (riinfo->confmatchtype)
			{
				case FKCONSTR_MATCH_FULL:

					/*
					 * Not allowed - MATCH FULL says either all or none of the
					 * attributes can be NULLs
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
							 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
									RelationGetRelationName(fk_rel),
									NameStr(riinfo->conname)),
							 errdetail("MATCH FULL does not allow mixing of null and nonnull key values."),
							 errtableconstraint(fk_rel,
												NameStr(riinfo->conname))));
					table_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case FKCONSTR_MATCH_SIMPLE:

					/*
					 * MATCH SIMPLE - if ANY column is null, the key passes
					 * the constraint.
					 */
					table_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);

#ifdef NOT_USED
				case FKCONSTR_MATCH_PARTIAL:

					/*
					 * MATCH PARTIAL - all non-null columns must match. (not
					 * implemented, can be done by modifying the query below
					 * to only include non-null columns, or by writing a
					 * special version here)
					 */
					break;
#endif
			}

		case RI_KEYS_NONE_NULL:

			/*
			 * Have a full qualified key - continue below for all three kinds
			 * of MATCH.
			 */
			break;
	}

	SPI_connect();

	/* Fetch or prepare a saved plan for the real check */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_CHECK_LOOKUPPK);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];
		const char *pk_only;

		/* ----------
		 * The query string built is
		 *	SELECT 1 FROM [ONLY] <pktable> x WHERE pkatt1 = $1 [AND ...]
		 *		   FOR KEY SHARE OF x
		 * The type id's for the $ parameters are those of the
		 * corresponding FK attributes.
		 *
		 * But for temporal FKs we need to make sure
		 * the FK's range is completely covered.
		 * So we use this query instead:
		 *  SELECT 1
		 *	FROM	(
		 *		SELECT pkperiodatt AS r
		 *		FROM   [ONLY] pktable x
		 *		WHERE  pkatt1 = $1 [AND ...]
		 *		AND    pkperiodatt && $n
		 *		FOR KEY SHARE OF x
		 *	) x1
		 *  HAVING $n <@ range_agg(x1.r)
		 * Note if FOR KEY SHARE ever allows GROUP BY and HAVING
		 * we can make this a bit simpler.
		 * ----------
		 */
		initStringInfo(&querybuf);
		pk_only = pk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(pkrelname, pk_rel);
		if (riinfo->hasperiod)
		{
			quoteOneName(attname,
						 RIAttName(pk_rel, riinfo->pk_attnums[riinfo->nkeys - 1]));

			appendStringInfo(&querybuf,
							 "SELECT 1 FROM (SELECT %s AS r FROM %s%s x",
							 attname, pk_only, pkrelname);
		}
		else
		{
			appendStringInfo(&querybuf, "SELECT 1 FROM %s%s x",
							 pk_only, pkrelname);
		}
		querysep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(pk_rel, riinfo->pk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							attname, pk_type,
							riinfo->pf_eq_oprs[i],
							paramname, fk_type);
			querysep = "AND";
			queryoids[i] = fk_type;
		}
		appendStringInfoString(&querybuf, " FOR KEY SHARE OF x");
		if (riinfo->hasperiod)
		{
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[riinfo->nkeys - 1]);

			appendStringInfo(&querybuf, ") x1 HAVING ");
			sprintf(paramname, "$%d", riinfo->nkeys);
			ri_GenerateQual(&querybuf, "",
							paramname, fk_type,
							riinfo->agged_period_contained_by_oper,
							"pg_catalog.range_agg", ANYMULTIRANGEOID);
			appendStringInfo(&querybuf, "(x1.r)");
		}

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * Now check that foreign key exists in PK table
	 *
	 * XXX detectNewRows must be true when a partitioned table is on the
	 * referenced side.  The reason is that our snapshot must be fresh in
	 * order for the hack in find_inheritance_children() to work.
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					NULL, newslot,
					pk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE,
					SPI_OK_SELECT);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(pk_rel, RowShareLock);

	return PointerGetDatum(NULL);
}


/*
 * RI_FKey_check_ins -
 *
 * Check foreign key existence at insert event on FK table.
 */
Datum
RI_FKey_check_ins(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_check_ins", RI_TRIGTYPE_INSERT);

	/* Share code with UPDATE case. */
	return RI_FKey_check((TriggerData *) fcinfo->context);
}


/*
 * RI_FKey_check_upd -
 *
 * Check foreign key existence at update event on FK table.
 */
Datum
RI_FKey_check_upd(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_check_upd", RI_TRIGTYPE_UPDATE);

	/* Share code with INSERT case. */
	return RI_FKey_check((TriggerData *) fcinfo->context);
}


/*
 * ri_Check_Pk_Match
 *
 * Check to see if another PK row has been created that provides the same
 * key values as the "oldslot" that's been modified or deleted in our trigger
 * event.  Returns true if a match is found in the PK table.
 *
 * We assume the caller checked that the oldslot contains no NULL key values,
 * since otherwise a match is impossible.
 */
static bool
ri_Check_Pk_Match(Relation pk_rel, Relation fk_rel,
				  TupleTableSlot *oldslot,
				  const RI_ConstraintInfo *riinfo)
{
	SPIPlanPtr	qplan;
	RI_QueryKey qkey;
	bool		result;

	/* Only called for non-null rows */
	Assert(ri_NullCheck(RelationGetDescr(pk_rel), oldslot, riinfo, true) == RI_KEYS_NONE_NULL);

	SPI_connect();

	/*
	 * Fetch or prepare a saved plan for checking PK table with values coming
	 * from a PK row
	 */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_CHECK_LOOKUPPK_FROM_PK);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		const char *pk_only;
		Oid			queryoids[RI_MAX_NUMKEYS];

		/* ----------
		 * The query string built is
		 *	SELECT 1 FROM [ONLY] <pktable> x WHERE pkatt1 = $1 [AND ...]
		 *		   FOR KEY SHARE OF x
		 * The type id's for the $ parameters are those of the
		 * PK attributes themselves.
		 *
		 * But for temporal FKs we need to make sure
		 * the old PK's range is completely covered.
		 * So we use this query instead:
		 *  SELECT 1
		 *  FROM    (
		 *	  SELECT pkperiodatt AS r
		 *	  FROM   [ONLY] pktable x
		 *	  WHERE  pkatt1 = $1 [AND ...]
		 *	  AND    pkperiodatt && $n
		 *	  FOR KEY SHARE OF x
		 *  ) x1
		 *  HAVING $n <@ range_agg(x1.r)
		 * Note if FOR KEY SHARE ever allows GROUP BY and HAVING
		 * we can make this a bit simpler.
		 * ----------
		 */
		initStringInfo(&querybuf);
		pk_only = pk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(pkrelname, pk_rel);
		if (riinfo->hasperiod)
		{
			quoteOneName(attname, RIAttName(pk_rel, riinfo->pk_attnums[riinfo->nkeys - 1]));

			appendStringInfo(&querybuf,
							 "SELECT 1 FROM (SELECT %s AS r FROM %s%s x",
							 attname, pk_only, pkrelname);
		}
		else
		{
			appendStringInfo(&querybuf, "SELECT 1 FROM %s%s x",
							 pk_only, pkrelname);
		}
		querysep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(pk_rel, riinfo->pk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							attname, pk_type,
							riinfo->pp_eq_oprs[i],
							paramname, pk_type);
			querysep = "AND";
			queryoids[i] = pk_type;
		}
		appendStringInfoString(&querybuf, " FOR KEY SHARE OF x");
		if (riinfo->hasperiod)
		{
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[riinfo->nkeys - 1]);

			appendStringInfo(&querybuf, ") x1 HAVING ");
			sprintf(paramname, "$%d", riinfo->nkeys);
			ri_GenerateQual(&querybuf, "",
							paramname, fk_type,
							riinfo->agged_period_contained_by_oper,
							"pg_catalog.range_agg", ANYMULTIRANGEOID);
			appendStringInfo(&querybuf, "(x1.r)");
		}

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * We have a plan now. Run it.
	 */
	result = ri_PerformCheck(riinfo, &qkey, qplan,
							 fk_rel, pk_rel,
							 oldslot, NULL,
							 true,	/* treat like update */
							 SPI_OK_SELECT);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return result;
}


/*
 * RI_FKey_noaction_del -
 *
 * Give an error and roll back the current transaction if the
 * delete has resulted in a violation of the given referential
 * integrity constraint.
 */
Datum
RI_FKey_noaction_del(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_noaction_del", RI_TRIGTYPE_DELETE);

	/* Share code with RESTRICT/UPDATE cases. */
	return ri_restrict((TriggerData *) fcinfo->context, true);
}

/*
 * RI_FKey_restrict_del -
 *
 * Restrict delete from PK table to rows unreferenced by foreign key.
 *
 * The SQL standard intends that this referential action occur exactly when
 * the delete is performed, rather than after.  This appears to be
 * the only difference between "NO ACTION" and "RESTRICT".  In Postgres
 * we still implement this as an AFTER trigger, but it's non-deferrable.
 */
Datum
RI_FKey_restrict_del(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_restrict_del", RI_TRIGTYPE_DELETE);

	/* Share code with NO ACTION/UPDATE cases. */
	return ri_restrict((TriggerData *) fcinfo->context, false);
}

/*
 * RI_FKey_noaction_upd -
 *
 * Give an error and roll back the current transaction if the
 * update has resulted in a violation of the given referential
 * integrity constraint.
 */
Datum
RI_FKey_noaction_upd(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_noaction_upd", RI_TRIGTYPE_UPDATE);

	/* Share code with RESTRICT/DELETE cases. */
	return ri_restrict((TriggerData *) fcinfo->context, true);
}

/*
 * RI_FKey_restrict_upd -
 *
 * Restrict update of PK to rows unreferenced by foreign key.
 *
 * The SQL standard intends that this referential action occur exactly when
 * the update is performed, rather than after.  This appears to be
 * the only difference between "NO ACTION" and "RESTRICT".  In Postgres
 * we still implement this as an AFTER trigger, but it's non-deferrable.
 */
Datum
RI_FKey_restrict_upd(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_restrict_upd", RI_TRIGTYPE_UPDATE);

	/* Share code with NO ACTION/DELETE cases. */
	return ri_restrict((TriggerData *) fcinfo->context, false);
}

/*
 * ri_restrict -
 *
 * Common code for ON DELETE RESTRICT, ON DELETE NO ACTION,
 * ON UPDATE RESTRICT, and ON UPDATE NO ACTION.
 */
static Datum
ri_restrict(TriggerData *trigdata, bool is_no_action)
{
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *oldslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, true);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR KEY SHARE will get on it.
	 */
	fk_rel = table_open(riinfo->fk_relid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	oldslot = trigdata->tg_trigslot;

	/*
	 * If another PK row now exists providing the old key values, we should
	 * not do anything.  However, this check should only be made in the NO
	 * ACTION case; in RESTRICT cases we don't wish to allow another row to be
	 * substituted.
	 */
	if (is_no_action &&
		ri_Check_Pk_Match(pk_rel, fk_rel, oldslot, riinfo))
	{
		table_close(fk_rel, RowShareLock);
		return PointerGetDatum(NULL);
	}

	SPI_connect();

	/*
	 * Fetch or prepare a saved plan for the restrict lookup (it's the same
	 * query for delete and update cases)
	 */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_RESTRICT);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];
		const char *fk_only;

		/* ----------
		 * The query string built is
		 *	SELECT 1 FROM [ONLY] <fktable> x WHERE $1 = fkatt1 [AND ...]
		 *		   FOR KEY SHARE OF x
		 * The type id's for the $ parameters are those of the
		 * corresponding PK attributes.
		 * ----------
		 */
		initStringInfo(&querybuf);
		fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(fkrelname, fk_rel);
		appendStringInfo(&querybuf, "SELECT 1 FROM %s%s x",
						 fk_only, fkrelname);
		querysep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(fk_rel, riinfo->fk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							paramname, pk_type,
							riinfo->pf_eq_oprs[i],
							attname, fk_type);
			querysep = "AND";
			queryoids[i] = pk_type;
		}
		appendStringInfoString(&querybuf, " FOR KEY SHARE OF x");

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * We have a plan now. Run it to check for existing references.
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					oldslot, NULL,
					true,		/* must detect new rows */
					SPI_OK_SELECT);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(fk_rel, RowShareLock);

	return PointerGetDatum(NULL);
}


/*
 * RI_FKey_cascade_del -
 *
 * Cascaded delete foreign key references at delete event on PK table.
 */
Datum
RI_FKey_cascade_del(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *oldslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_cascade_del", RI_TRIGTYPE_DELETE);

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, true);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual DELETE will get on it.
	 */
	fk_rel = table_open(riinfo->fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	oldslot = trigdata->tg_trigslot;

	SPI_connect();

	/* Fetch or prepare a saved plan for the cascaded delete */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_CASCADE_ONDELETE);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];
		const char *fk_only;

		/* ----------
		 * The query string built is
		 *	DELETE FROM [ONLY] <fktable> WHERE $1 = fkatt1 [AND ...]
		 * The type id's for the $ parameters are those of the
		 * corresponding PK attributes.
		 * ----------
		 */
		initStringInfo(&querybuf);
		fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(fkrelname, fk_rel);
		appendStringInfo(&querybuf, "DELETE FROM %s%s",
						 fk_only, fkrelname);
		querysep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(fk_rel, riinfo->fk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							paramname, pk_type,
							riinfo->pf_eq_oprs[i],
							attname, fk_type);
			querysep = "AND";
			queryoids[i] = pk_type;
		}

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * We have a plan now. Build up the arguments from the key values in the
	 * deleted PK tuple and delete the referencing rows
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					oldslot, NULL,
					true,		/* must detect new rows */
					SPI_OK_DELETE);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(fk_rel, RowExclusiveLock);

	return PointerGetDatum(NULL);
}


/*
 * RI_FKey_cascade_upd -
 *
 * Cascaded update foreign key references at update event on PK table.
 */
Datum
RI_FKey_cascade_upd(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *newslot;
	TupleTableSlot *oldslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_cascade_upd", RI_TRIGTYPE_UPDATE);

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, true);

	/*
	 * Get the relation descriptors of the FK and PK tables and the new and
	 * old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = table_open(riinfo->fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	newslot = trigdata->tg_newslot;
	oldslot = trigdata->tg_trigslot;

	SPI_connect();

	/* Fetch or prepare a saved plan for the cascaded update */
	ri_BuildQueryKey(&qkey, riinfo, RI_PLAN_CASCADE_ONUPDATE);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		StringInfoData qualbuf;
		char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		const char *qualsep;
		Oid			queryoids[RI_MAX_NUMKEYS * 2];
		const char *fk_only;

		/* ----------
		 * The query string built is
		 *	UPDATE [ONLY] <fktable> SET fkatt1 = $1 [, ...]
		 *			WHERE $n = fkatt1 [AND ...]
		 * The type id's for the $ parameters are those of the
		 * corresponding PK attributes.  Note that we are assuming
		 * there is an assignment cast from the PK to the FK type;
		 * else the parser will fail.
		 * ----------
		 */
		initStringInfo(&querybuf);
		initStringInfo(&qualbuf);
		fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(fkrelname, fk_rel);
		appendStringInfo(&querybuf, "UPDATE %s%s SET",
						 fk_only, fkrelname);
		querysep = "";
		qualsep = "WHERE";
		for (int i = 0, j = riinfo->nkeys; i < riinfo->nkeys; i++, j++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(fk_rel, riinfo->fk_attnums[i]));
			appendStringInfo(&querybuf,
							 "%s %s = $%d",
							 querysep, attname, i + 1);
			sprintf(paramname, "$%d", j + 1);
			ri_GenerateQual(&qualbuf, qualsep,
							paramname, pk_type,
							riinfo->pf_eq_oprs[i],
							attname, fk_type);
			querysep = ",";
			qualsep = "AND";
			queryoids[i] = pk_type;
			queryoids[j] = pk_type;
		}
		appendBinaryStringInfo(&querybuf, qualbuf.data, qualbuf.len);

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys * 2, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * We have a plan now. Run it to update the existing references.
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					oldslot, newslot,
					true,		/* must detect new rows */
					SPI_OK_UPDATE);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(fk_rel, RowExclusiveLock);

	return PointerGetDatum(NULL);
}


/*
 * RI_FKey_setnull_del -
 *
 * Set foreign key references to NULL values at delete event on PK table.
 */
Datum
RI_FKey_setnull_del(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_setnull_del", RI_TRIGTYPE_DELETE);

	/* Share code with UPDATE case */
	return ri_set((TriggerData *) fcinfo->context, true, RI_TRIGTYPE_DELETE);
}

/*
 * RI_FKey_setnull_upd -
 *
 * Set foreign key references to NULL at update event on PK table.
 */
Datum
RI_FKey_setnull_upd(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_setnull_upd", RI_TRIGTYPE_UPDATE);

	/* Share code with DELETE case */
	return ri_set((TriggerData *) fcinfo->context, true, RI_TRIGTYPE_UPDATE);
}

/*
 * RI_FKey_setdefault_del -
 *
 * Set foreign key references to defaults at delete event on PK table.
 */
Datum
RI_FKey_setdefault_del(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_setdefault_del", RI_TRIGTYPE_DELETE);

	/* Share code with UPDATE case */
	return ri_set((TriggerData *) fcinfo->context, false, RI_TRIGTYPE_DELETE);
}

/*
 * RI_FKey_setdefault_upd -
 *
 * Set foreign key references to defaults at update event on PK table.
 */
Datum
RI_FKey_setdefault_upd(PG_FUNCTION_ARGS)
{
	/* Check that this is a valid trigger call on the right time and event. */
	ri_CheckTrigger(fcinfo, "RI_FKey_setdefault_upd", RI_TRIGTYPE_UPDATE);

	/* Share code with DELETE case */
	return ri_set((TriggerData *) fcinfo->context, false, RI_TRIGTYPE_UPDATE);
}

/*
 * ri_set -
 *
 * Common code for ON DELETE SET NULL, ON DELETE SET DEFAULT, ON UPDATE SET
 * NULL, and ON UPDATE SET DEFAULT.
 */
static Datum
ri_set(TriggerData *trigdata, bool is_set_null, int tgkind)
{
	const RI_ConstraintInfo *riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	TupleTableSlot *oldslot;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int32		queryno;

	riinfo = ri_FetchConstraintInfo(trigdata->tg_trigger,
									trigdata->tg_relation, true);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = table_open(riinfo->fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	oldslot = trigdata->tg_trigslot;

	SPI_connect();

	/*
	 * Fetch or prepare a saved plan for the trigger.
	 */
	switch (tgkind)
	{
		case RI_TRIGTYPE_UPDATE:
			queryno = is_set_null
				? RI_PLAN_SETNULL_ONUPDATE
				: RI_PLAN_SETDEFAULT_ONUPDATE;
			break;
		case RI_TRIGTYPE_DELETE:
			queryno = is_set_null
				? RI_PLAN_SETNULL_ONDELETE
				: RI_PLAN_SETDEFAULT_ONDELETE;
			break;
		default:
			elog(ERROR, "invalid tgkind passed to ri_set");
	}

	ri_BuildQueryKey(&qkey, riinfo, queryno);

	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		const char *qualsep;
		Oid			queryoids[RI_MAX_NUMKEYS];
		const char *fk_only;
		int			num_cols_to_set;
		const int16 *set_cols;

		switch (tgkind)
		{
			case RI_TRIGTYPE_UPDATE:
				num_cols_to_set = riinfo->nkeys;
				set_cols = riinfo->fk_attnums;
				break;
			case RI_TRIGTYPE_DELETE:

				/*
				 * If confdelsetcols are present, then we only update the
				 * columns specified in that array, otherwise we update all
				 * the referencing columns.
				 */
				if (riinfo->ndelsetcols != 0)
				{
					num_cols_to_set = riinfo->ndelsetcols;
					set_cols = riinfo->confdelsetcols;
				}
				else
				{
					num_cols_to_set = riinfo->nkeys;
					set_cols = riinfo->fk_attnums;
				}
				break;
			default:
				elog(ERROR, "invalid tgkind passed to ri_set");
		}

		/* ----------
		 * The query string built is
		 *	UPDATE [ONLY] <fktable> SET fkatt1 = {NULL|DEFAULT} [, ...]
		 *			WHERE $1 = fkatt1 [AND ...]
		 * The type id's for the $ parameters are those of the
		 * corresponding PK attributes.
		 * ----------
		 */
		initStringInfo(&querybuf);
		fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
			"" : "ONLY ";
		quoteRelationName(fkrelname, fk_rel);
		appendStringInfo(&querybuf, "UPDATE %s%s SET",
						 fk_only, fkrelname);

		/*
		 * Add assignment clauses
		 */
		querysep = "";
		for (int i = 0; i < num_cols_to_set; i++)
		{
			quoteOneName(attname, RIAttName(fk_rel, set_cols[i]));
			appendStringInfo(&querybuf,
							 "%s %s = %s",
							 querysep, attname,
							 is_set_null ? "NULL" : "DEFAULT");
			querysep = ",";
		}

		/*
		 * Add WHERE clause
		 */
		qualsep = "WHERE";
		for (int i = 0; i < riinfo->nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(fk_rel, riinfo->fk_attnums[i]));

			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, qualsep,
							paramname, pk_type,
							riinfo->pf_eq_oprs[i],
							attname, fk_type);
			qualsep = "AND";
			queryoids[i] = pk_type;
		}

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel);
	}

	/*
	 * We have a plan now. Run it to update the existing references.
	 */
	ri_PerformCheck(riinfo, &qkey, qplan,
					fk_rel, pk_rel,
					oldslot, NULL,
					true,		/* must detect new rows */
					SPI_OK_UPDATE);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	table_close(fk_rel, RowExclusiveLock);

	if (is_set_null)
		return PointerGetDatum(NULL);
	else
	{
		/*
		 * If we just deleted or updated the PK row whose key was equal to the
		 * FK columns' default values, and a referencing row exists in the FK
		 * table, we would have updated that row to the same values it already
		 * had --- and RI_FKey_fk_upd_check_required would hence believe no
		 * check is necessary.  So we need to do another lookup now and in
		 * case a reference still exists, abort the operation.  That is
		 * already implemented in the NO ACTION trigger, so just run it. (This
		 * recheck is only needed in the SET DEFAULT case, since CASCADE would
		 * remove such rows in case of a DELETE operation or would change the
		 * FK key values in case of an UPDATE, while SET NULL is certain to
		 * result in rows that satisfy the FK constraint.)
		 */
		return ri_restrict(trigdata, true);
	}
}


/*
 * RI_FKey_pk_upd_check_required -
 *
 * Check if we really need to fire the RI trigger for an update or delete to a PK
 * relation.  This is called by the AFTER trigger queue manager to see if
 * it can skip queuing an instance of an RI trigger.  Returns true if the
 * trigger must be fired, false if we can prove the constraint will still
 * be satisfied.
 *
 * newslot will be NULL if this is called for a delete.
 */
bool
RI_FKey_pk_upd_check_required(Trigger *trigger, Relation pk_rel,
							  TupleTableSlot *oldslot, TupleTableSlot *newslot)
{
	const RI_ConstraintInfo *riinfo;

	riinfo = ri_FetchConstraintInfo(trigger, pk_rel, true);

	/*
	 * If any old key value is NULL, the row could not have been referenced by
	 * an FK row, so no check is needed.
	 */
	if (ri_NullCheck(RelationGetDescr(pk_rel), oldslot, riinfo, true) != RI_KEYS_NONE_NULL)
		return false;

	/* If all old and new key values are equal, no check is needed */
	if (newslot && ri_KeysEqual(pk_rel, oldslot, newslot, riinfo, true))
		return false;

	/* Else we need to fire the trigger. */
	return true;
}

/*
 * RI_FKey_fk_upd_check_required -
 *
 * Check if we really need to fire the RI trigger for an update to an FK
 * relation.  This is called by the AFTER trigger queue manager to see if
 * it can skip queuing an instance of an RI trigger.  Returns true if the
 * trigger must be fired, false if we can prove the constraint will still
 * be satisfied.
 */
bool
RI_FKey_fk_upd_check_required(Trigger *trigger, Relation fk_rel,
							  TupleTableSlot *oldslot, TupleTableSlot *newslot)
{
	const RI_ConstraintInfo *riinfo;
	int			ri_nullcheck;

	/*
	 * AfterTriggerSaveEvent() handles things such that this function is never
	 * called for partitioned tables.
	 */
	Assert(fk_rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE);

	riinfo = ri_FetchConstraintInfo(trigger, fk_rel, false);

	ri_nullcheck = ri_NullCheck(RelationGetDescr(fk_rel), newslot, riinfo, false);

	/*
	 * If all new key values are NULL, the row satisfies the constraint, so no
	 * check is needed.
	 */
	if (ri_nullcheck == RI_KEYS_ALL_NULL)
		return false;

	/*
	 * If some new key values are NULL, the behavior depends on the match
	 * type.
	 */
	else if (ri_nullcheck == RI_KEYS_SOME_NULL)
	{
		switch (riinfo->confmatchtype)
		{
			case FKCONSTR_MATCH_SIMPLE:

				/*
				 * If any new key value is NULL, the row must satisfy the
				 * constraint, so no check is needed.
				 */
				return false;

			case FKCONSTR_MATCH_PARTIAL:

				/*
				 * Don't know, must run full check.
				 */
				break;

			case FKCONSTR_MATCH_FULL:

				/*
				 * If some new key values are NULL, the row fails the
				 * constraint.  We must not throw error here, because the row
				 * might get invalidated before the constraint is to be
				 * checked, but we should queue the event to apply the check
				 * later.
				 */
				return true;
		}
	}

	/*
	 * Continues here for no new key values are NULL, or we couldn't decide
	 * yet.
	 */

	/*
	 * If the original row was inserted by our own transaction, we must fire
	 * the trigger whether or not the keys are equal.  This is because our
	 * UPDATE will invalidate the INSERT so that the INSERT RI trigger will
	 * not do anything; so we had better do the UPDATE check.  (We could skip
	 * this if we knew the INSERT trigger already fired, but there is no easy
	 * way to know that.)
	 */
	if (slot_is_current_xact_tuple(oldslot))
		return true;

	/* If all old and new key values are equal, no check is needed */
	if (ri_KeysEqual(fk_rel, oldslot, newslot, riinfo, false))
		return false;

	/* Else we need to fire the trigger. */
	return true;
}

/*
 * RI_Initial_Check -
 *
 * Check an entire table for non-matching values using a single query.
 * This is not a trigger procedure, but is called during ALTER TABLE
 * ADD FOREIGN KEY to validate the initial table contents.
 *
 * We expect that the caller has made provision to prevent any problems
 * caused by concurrent actions. This could be either by locking rel and
 * pkrel at ShareRowExclusiveLock or higher, or by otherwise ensuring
 * that triggers implementing the checks are already active.
 * Hence, we do not need to lock individual rows for the check.
 *
 * If the check fails because the current user doesn't have permissions
 * to read both tables, return false to let our caller know that they will
 * need to do something else to check the constraint.
 */
bool
RI_Initial_Check(Trigger *trigger, Relation fk_rel, Relation pk_rel)
{
	const RI_ConstraintInfo *riinfo;
	StringInfoData querybuf;
	char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		pkattname[MAX_QUOTED_NAME_LEN + 3];
	char		fkattname[MAX_QUOTED_NAME_LEN + 3];
	RangeTblEntry *rte;
	RTEPermissionInfo *pk_perminfo;
	RTEPermissionInfo *fk_perminfo;
	List	   *rtes = NIL;
	List	   *perminfos = NIL;
	const char *sep;
	const char *fk_only;
	const char *pk_only;
	int			save_nestlevel;
	char		workmembuf[32];
	int			spi_result;
	SPIPlanPtr	qplan;

	riinfo = ri_FetchConstraintInfo(trigger, fk_rel, false);

	/*
	 * Check to make sure current user has enough permissions to do the test
	 * query.  (If not, caller can fall back to the trigger method, which
	 * works because it changes user IDs on the fly.)
	 *
	 * XXX are there any other show-stopper conditions to check?
	 */
	pk_perminfo = makeNode(RTEPermissionInfo);
	pk_perminfo->relid = RelationGetRelid(pk_rel);
	pk_perminfo->requiredPerms = ACL_SELECT;
	perminfos = lappend(perminfos, pk_perminfo);
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(pk_rel);
	rte->relkind = pk_rel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;
	rte->perminfoindex = list_length(perminfos);
	rtes = lappend(rtes, rte);

	fk_perminfo = makeNode(RTEPermissionInfo);
	fk_perminfo->relid = RelationGetRelid(fk_rel);
	fk_perminfo->requiredPerms = ACL_SELECT;
	perminfos = lappend(perminfos, fk_perminfo);
	rte = makeNode(RangeTblEntry);
	rte->rtekind = RTE_RELATION;
	rte->relid = RelationGetRelid(fk_rel);
	rte->relkind = fk_rel->rd_rel->relkind;
	rte->rellockmode = AccessShareLock;
	rte->perminfoindex = list_length(perminfos);
	rtes = lappend(rtes, rte);

	for (int i = 0; i < riinfo->nkeys; i++)
	{
		int			attno;

		attno = riinfo->pk_attnums[i] - FirstLowInvalidHeapAttributeNumber;
		pk_perminfo->selectedCols = bms_add_member(pk_perminfo->selectedCols, attno);

		attno = riinfo->fk_attnums[i] - FirstLowInvalidHeapAttributeNumber;
		fk_perminfo->selectedCols = bms_add_member(fk_perminfo->selectedCols, attno);
	}

	if (!ExecCheckPermissions(rtes, perminfos, false))
		return false;

	/*
	 * Also punt if RLS is enabled on either table unless this role has the
	 * bypassrls right or is the table owner of the table(s) involved which
	 * have RLS enabled.
	 */
	if (!has_bypassrls_privilege(GetUserId()) &&
		((pk_rel->rd_rel->relrowsecurity &&
		  !object_ownercheck(RelationRelationId, RelationGetRelid(pk_rel),
							 GetUserId())) ||
		 (fk_rel->rd_rel->relrowsecurity &&
		  !object_ownercheck(RelationRelationId, RelationGetRelid(fk_rel),
							 GetUserId()))))
		return false;

	/*----------
	 * The query string built is:
	 *	SELECT fk.keycols FROM [ONLY] relname fk
	 *	 LEFT OUTER JOIN [ONLY] pkrelname pk
	 *	 ON (pk.pkkeycol1=fk.keycol1 [AND ...])
	 *	 WHERE pk.pkkeycol1 IS NULL AND
	 * For MATCH SIMPLE:
	 *	 (fk.keycol1 IS NOT NULL [AND ...])
	 * For MATCH FULL:
	 *	 (fk.keycol1 IS NOT NULL [OR ...])
	 *
	 * We attach COLLATE clauses to the operators when comparing columns
	 * that have different collations.
	 *----------
	 */
	initStringInfo(&querybuf);
	appendStringInfoString(&querybuf, "SELECT ");
	sep = "";
	for (int i = 0; i < riinfo->nkeys; i++)
	{
		quoteOneName(fkattname,
					 RIAttName(fk_rel, riinfo->fk_attnums[i]));
		appendStringInfo(&querybuf, "%sfk.%s", sep, fkattname);
		sep = ", ";
	}

	quoteRelationName(pkrelname, pk_rel);
	quoteRelationName(fkrelname, fk_rel);
	fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
		"" : "ONLY ";
	pk_only = pk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
		"" : "ONLY ";
	appendStringInfo(&querybuf,
					 " FROM %s%s fk LEFT OUTER JOIN %s%s pk ON",
					 fk_only, fkrelname, pk_only, pkrelname);

	strcpy(pkattname, "pk.");
	strcpy(fkattname, "fk.");
	sep = "(";
	for (int i = 0; i < riinfo->nkeys; i++)
	{
		Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
		Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);
		Oid			pk_coll = RIAttCollation(pk_rel, riinfo->pk_attnums[i]);
		Oid			fk_coll = RIAttCollation(fk_rel, riinfo->fk_attnums[i]);

		quoteOneName(pkattname + 3,
					 RIAttName(pk_rel, riinfo->pk_attnums[i]));
		quoteOneName(fkattname + 3,
					 RIAttName(fk_rel, riinfo->fk_attnums[i]));
		ri_GenerateQual(&querybuf, sep,
						pkattname, pk_type,
						riinfo->pf_eq_oprs[i],
						fkattname, fk_type);
		if (pk_coll != fk_coll)
			ri_GenerateQualCollation(&querybuf, pk_coll);
		sep = "AND";
	}

	/*
	 * It's sufficient to test any one pk attribute for null to detect a join
	 * failure.
	 */
	quoteOneName(pkattname, RIAttName(pk_rel, riinfo->pk_attnums[0]));
	appendStringInfo(&querybuf, ") WHERE pk.%s IS NULL AND (", pkattname);

	sep = "";
	for (int i = 0; i < riinfo->nkeys; i++)
	{
		quoteOneName(fkattname, RIAttName(fk_rel, riinfo->fk_attnums[i]));
		appendStringInfo(&querybuf,
						 "%sfk.%s IS NOT NULL",
						 sep, fkattname);
		switch (riinfo->confmatchtype)
		{
			case FKCONSTR_MATCH_SIMPLE:
				sep = " AND ";
				break;
			case FKCONSTR_MATCH_FULL:
				sep = " OR ";
				break;
		}
	}
	appendStringInfoChar(&querybuf, ')');

	/*
	 * Temporarily increase work_mem so that the check query can be executed
	 * more efficiently.  It seems okay to do this because the query is simple
	 * enough to not use a multiple of work_mem, and one typically would not
	 * have many large foreign-key validations happening concurrently.  So
	 * this seems to meet the criteria for being considered a "maintenance"
	 * operation, and accordingly we use maintenance_work_mem.  However, we
	 * must also set hash_mem_multiplier to 1, since it is surely not okay to
	 * let that get applied to the maintenance_work_mem value.
	 *
	 * We use the equivalent of a function SET option to allow the setting to
	 * persist for exactly the duration of the check query.  guc.c also takes
	 * care of undoing the setting on error.
	 */
	save_nestlevel = NewGUCNestLevel();

	snprintf(workmembuf, sizeof(workmembuf), "%d", maintenance_work_mem);
	(void) set_config_option("work_mem", workmembuf,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("hash_mem_multiplier", "1",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	SPI_connect();

	/*
	 * Generate the plan.  We don't need to cache it, and there are no
	 * arguments to the plan.
	 */
	qplan = SPI_prepare(querybuf.data, 0, NULL);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %s for %s",
			 SPI_result_code_string(SPI_result), querybuf.data);

	/*
	 * Run the plan.  For safety we force a current snapshot to be used. (In
	 * transaction-snapshot mode, this arguably violates transaction isolation
	 * rules, but we really haven't got much choice.) We don't need to
	 * register the snapshot, because SPI_execute_snapshot will see to it. We
	 * need at most one tuple returned, so pass limit = 1.
	 */
	spi_result = SPI_execute_snapshot(qplan,
									  NULL, NULL,
									  GetLatestSnapshot(),
									  InvalidSnapshot,
									  true, false, 1);

	/* Check result */
	if (spi_result != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_snapshot returned %s", SPI_result_code_string(spi_result));

	/* Did we find a tuple violating the constraint? */
	if (SPI_processed > 0)
	{
		TupleTableSlot *slot;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		RI_ConstraintInfo fake_riinfo;

		slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

		heap_deform_tuple(tuple, tupdesc,
						  slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		/*
		 * The columns to look at in the result tuple are 1..N, not whatever
		 * they are in the fk_rel.  Hack up riinfo so that the subroutines
		 * called here will behave properly.
		 *
		 * In addition to this, we have to pass the correct tupdesc to
		 * ri_ReportViolation, overriding its normal habit of using the pk_rel
		 * or fk_rel's tupdesc.
		 */
		memcpy(&fake_riinfo, riinfo, sizeof(RI_ConstraintInfo));
		for (int i = 0; i < fake_riinfo.nkeys; i++)
			fake_riinfo.fk_attnums[i] = i + 1;

		/*
		 * If it's MATCH FULL, and there are any nulls in the FK keys,
		 * complain about that rather than the lack of a match.  MATCH FULL
		 * disallows partially-null FK rows.
		 */
		if (fake_riinfo.confmatchtype == FKCONSTR_MATCH_FULL &&
			ri_NullCheck(tupdesc, slot, &fake_riinfo, false) != RI_KEYS_NONE_NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
					 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
							RelationGetRelationName(fk_rel),
							NameStr(fake_riinfo.conname)),
					 errdetail("MATCH FULL does not allow mixing of null and nonnull key values."),
					 errtableconstraint(fk_rel,
										NameStr(fake_riinfo.conname))));

		/*
		 * We tell ri_ReportViolation we were doing the RI_PLAN_CHECK_LOOKUPPK
		 * query, which isn't true, but will cause it to use
		 * fake_riinfo.fk_attnums as we need.
		 */
		ri_ReportViolation(&fake_riinfo,
						   pk_rel, fk_rel,
						   slot, tupdesc,
						   RI_PLAN_CHECK_LOOKUPPK, false);

		ExecDropSingleTupleTableSlot(slot);
	}

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/*
	 * Restore work_mem and hash_mem_multiplier.
	 */
	AtEOXact_GUC(true, save_nestlevel);

	return true;
}

/*
 * RI_PartitionRemove_Check -
 *
 * Verify no referencing values exist, when a partition is detached on
 * the referenced side of a foreign key constraint.
 */
void
RI_PartitionRemove_Check(Trigger *trigger, Relation fk_rel, Relation pk_rel)
{
	const RI_ConstraintInfo *riinfo;
	StringInfoData querybuf;
	char	   *constraintDef;
	char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		pkattname[MAX_QUOTED_NAME_LEN + 3];
	char		fkattname[MAX_QUOTED_NAME_LEN + 3];
	const char *sep;
	const char *fk_only;
	int			save_nestlevel;
	char		workmembuf[32];
	int			spi_result;
	SPIPlanPtr	qplan;
	int			i;

	riinfo = ri_FetchConstraintInfo(trigger, fk_rel, false);

	/*
	 * We don't check permissions before displaying the error message, on the
	 * assumption that the user detaching the partition must have enough
	 * privileges to examine the table contents anyhow.
	 */

	/*----------
	 * The query string built is:
	 *  SELECT fk.keycols FROM [ONLY] relname fk
	 *    JOIN pkrelname pk
	 *    ON (pk.pkkeycol1=fk.keycol1 [AND ...])
	 *    WHERE (<partition constraint>) AND
	 * For MATCH SIMPLE:
	 *   (fk.keycol1 IS NOT NULL [AND ...])
	 * For MATCH FULL:
	 *   (fk.keycol1 IS NOT NULL [OR ...])
	 *
	 * We attach COLLATE clauses to the operators when comparing columns
	 * that have different collations.
	 *----------
	 */
	initStringInfo(&querybuf);
	appendStringInfoString(&querybuf, "SELECT ");
	sep = "";
	for (i = 0; i < riinfo->nkeys; i++)
	{
		quoteOneName(fkattname,
					 RIAttName(fk_rel, riinfo->fk_attnums[i]));
		appendStringInfo(&querybuf, "%sfk.%s", sep, fkattname);
		sep = ", ";
	}

	quoteRelationName(pkrelname, pk_rel);
	quoteRelationName(fkrelname, fk_rel);
	fk_only = fk_rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ?
		"" : "ONLY ";
	appendStringInfo(&querybuf,
					 " FROM %s%s fk JOIN %s pk ON",
					 fk_only, fkrelname, pkrelname);
	strcpy(pkattname, "pk.");
	strcpy(fkattname, "fk.");
	sep = "(";
	for (i = 0; i < riinfo->nkeys; i++)
	{
		Oid			pk_type = RIAttType(pk_rel, riinfo->pk_attnums[i]);
		Oid			fk_type = RIAttType(fk_rel, riinfo->fk_attnums[i]);
		Oid			pk_coll = RIAttCollation(pk_rel, riinfo->pk_attnums[i]);
		Oid			fk_coll = RIAttCollation(fk_rel, riinfo->fk_attnums[i]);

		quoteOneName(pkattname + 3,
					 RIAttName(pk_rel, riinfo->pk_attnums[i]));
		quoteOneName(fkattname + 3,
					 RIAttName(fk_rel, riinfo->fk_attnums[i]));
		ri_GenerateQual(&querybuf, sep,
						pkattname, pk_type,
						riinfo->pf_eq_oprs[i],
						fkattname, fk_type);
		if (pk_coll != fk_coll)
			ri_GenerateQualCollation(&querybuf, pk_coll);
		sep = "AND";
	}

	/*
	 * Start the WHERE clause with the partition constraint (except if this is
	 * the default partition and there's no other partition, because the
	 * partition constraint is the empty string in that case.)
	 */
	constraintDef = pg_get_partconstrdef_string(RelationGetRelid(pk_rel), "pk");
	if (constraintDef && constraintDef[0] != '\0')
		appendStringInfo(&querybuf, ") WHERE %s AND (",
						 constraintDef);
	else
		appendStringInfoString(&querybuf, ") WHERE (");

	sep = "";
	for (i = 0; i < riinfo->nkeys; i++)
	{
		quoteOneName(fkattname, RIAttName(fk_rel, riinfo->fk_attnums[i]));
		appendStringInfo(&querybuf,
						 "%sfk.%s IS NOT NULL",
						 sep, fkattname);
		switch (riinfo->confmatchtype)
		{
			case FKCONSTR_MATCH_SIMPLE:
				sep = " AND ";
				break;
			case FKCONSTR_MATCH_FULL:
				sep = " OR ";
				break;
		}
	}
	appendStringInfoChar(&querybuf, ')');

	/*
	 * Temporarily increase work_mem so that the check query can be executed
	 * more efficiently.  It seems okay to do this because the query is simple
	 * enough to not use a multiple of work_mem, and one typically would not
	 * have many large foreign-key validations happening concurrently.  So
	 * this seems to meet the criteria for being considered a "maintenance"
	 * operation, and accordingly we use maintenance_work_mem.  However, we
	 * must also set hash_mem_multiplier to 1, since it is surely not okay to
	 * let that get applied to the maintenance_work_mem value.
	 *
	 * We use the equivalent of a function SET option to allow the setting to
	 * persist for exactly the duration of the check query.  guc.c also takes
	 * care of undoing the setting on error.
	 */
	save_nestlevel = NewGUCNestLevel();

	snprintf(workmembuf, sizeof(workmembuf), "%d", maintenance_work_mem);
	(void) set_config_option("work_mem", workmembuf,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);
	(void) set_config_option("hash_mem_multiplier", "1",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	SPI_connect();

	/*
	 * Generate the plan.  We don't need to cache it, and there are no
	 * arguments to the plan.
	 */
	qplan = SPI_prepare(querybuf.data, 0, NULL);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %s for %s",
			 SPI_result_code_string(SPI_result), querybuf.data);

	/*
	 * Run the plan.  For safety we force a current snapshot to be used. (In
	 * transaction-snapshot mode, this arguably violates transaction isolation
	 * rules, but we really haven't got much choice.) We don't need to
	 * register the snapshot, because SPI_execute_snapshot will see to it. We
	 * need at most one tuple returned, so pass limit = 1.
	 */
	spi_result = SPI_execute_snapshot(qplan,
									  NULL, NULL,
									  GetLatestSnapshot(),
									  InvalidSnapshot,
									  true, false, 1);

	/* Check result */
	if (spi_result != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_snapshot returned %s", SPI_result_code_string(spi_result));

	/* Did we find a tuple that would violate the constraint? */
	if (SPI_processed > 0)
	{
		TupleTableSlot *slot;
		HeapTuple	tuple = SPI_tuptable->vals[0];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		RI_ConstraintInfo fake_riinfo;

		slot = MakeSingleTupleTableSlot(tupdesc, &TTSOpsVirtual);

		heap_deform_tuple(tuple, tupdesc,
						  slot->tts_values, slot->tts_isnull);
		ExecStoreVirtualTuple(slot);

		/*
		 * The columns to look at in the result tuple are 1..N, not whatever
		 * they are in the fk_rel.  Hack up riinfo so that ri_ReportViolation
		 * will behave properly.
		 *
		 * In addition to this, we have to pass the correct tupdesc to
		 * ri_ReportViolation, overriding its normal habit of using the pk_rel
		 * or fk_rel's tupdesc.
		 */
		memcpy(&fake_riinfo, riinfo, sizeof(RI_ConstraintInfo));
		for (i = 0; i < fake_riinfo.nkeys; i++)
			fake_riinfo.pk_attnums[i] = i + 1;

		ri_ReportViolation(&fake_riinfo, pk_rel, fk_rel,
						   slot, tupdesc, 0, true);
	}

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/*
	 * Restore work_mem and hash_mem_multiplier.
	 */
	AtEOXact_GUC(true, save_nestlevel);
}


/* ----------
 * Local functions below
 * ----------
 */


/*
 * quoteOneName --- safely quote a single SQL name
 *
 * buffer must be MAX_QUOTED_NAME_LEN long (includes room for \0)
 */
static void
quoteOneName(char *buffer, const char *name)
{
	/* Rather than trying to be smart, just always quote it. */
	*buffer++ = '"';
	while (*name)
	{
		if (*name == '"')
			*buffer++ = '"';
		*buffer++ = *name++;
	}
	*buffer++ = '"';
	*buffer = '\0';
}

/*
 * quoteRelationName --- safely quote a fully qualified relation name
 *
 * buffer must be MAX_QUOTED_REL_NAME_LEN long (includes room for \0)
 */
static void
quoteRelationName(char *buffer, Relation rel)
{
	quoteOneName(buffer, get_namespace_name(RelationGetNamespace(rel)));
	buffer += strlen(buffer);
	*buffer++ = '.';
	quoteOneName(buffer, RelationGetRelationName(rel));
}

/*
 * ri_GenerateQual --- generate a WHERE clause equating two variables
 *
 * This basically appends " sep leftop op rightop" to buf, adding casts
 * and schema qualification as needed to ensure that the parser will select
 * the operator we specify.  leftop and rightop should be parenthesized
 * if they aren't variables or parameters.
 */
static void
ri_GenerateQual(StringInfo buf,
				const char *sep,
				const char *leftop, Oid leftoptype,
				Oid opoid,
				const char *rightop, Oid rightoptype)
{
	appendStringInfo(buf, " %s ", sep);
	generate_operator_clause(buf, leftop, leftoptype, opoid,
							 rightop, rightoptype);
}

/*
 * ri_GenerateQualCollation --- add a COLLATE spec to a WHERE clause
 *
 * We only have to use this function when directly comparing the referencing
 * and referenced columns, if they are of different collations; else the
 * parser will fail to resolve the collation to use.  We don't need to use
 * this function for RI queries that compare a variable to a $n parameter.
 * Since parameter symbols always have default collation, the effect will be
 * to use the variable's collation.
 *
 * Note that we require that the collations of the referencing and the
 * referenced column have the same notion of equality: Either they have to
 * both be deterministic or else they both have to be the same.  (See also
 * ATAddForeignKeyConstraint().)
 */
static void
ri_GenerateQualCollation(StringInfo buf, Oid collation)
{
	HeapTuple	tp;
	Form_pg_collation colltup;
	char	   *collname;
	char		onename[MAX_QUOTED_NAME_LEN];

	/* Nothing to do if it's a noncollatable data type */
	if (!OidIsValid(collation))
		return;

	tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(collation));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for collation %u", collation);
	colltup = (Form_pg_collation) GETSTRUCT(tp);
	collname = NameStr(colltup->collname);

	/*
	 * We qualify the name always, for simplicity and to ensure the query is
	 * not search-path-dependent.
	 */
	quoteOneName(onename, get_namespace_name(colltup->collnamespace));
	appendStringInfo(buf, " COLLATE %s", onename);
	quoteOneName(onename, collname);
	appendStringInfo(buf, ".%s", onename);

	ReleaseSysCache(tp);
}

/* ----------
 * ri_BuildQueryKey -
 *
 *	Construct a hashtable key for a prepared SPI plan of an FK constraint.
 *
 *		key: output argument, *key is filled in based on the other arguments
 *		riinfo: info derived from pg_constraint entry
 *		constr_queryno: an internal number identifying the query type
 *			(see RI_PLAN_XXX constants at head of file)
 * ----------
 */
static void
ri_BuildQueryKey(RI_QueryKey *key, const RI_ConstraintInfo *riinfo,
				 int32 constr_queryno)
{
	/*
	 * Inherited constraints with a common ancestor can share ri_query_cache
	 * entries for all query types except RI_PLAN_CHECK_LOOKUPPK_FROM_PK.
	 * Except in that case, the query processes the other table involved in
	 * the FK constraint (i.e., not the table on which the trigger has been
	 * fired), and so it will be the same for all members of the inheritance
	 * tree.  So we may use the root constraint's OID in the hash key, rather
	 * than the constraint's own OID.  This avoids creating duplicate SPI
	 * plans, saving lots of work and memory when there are many partitions
	 * with similar FK constraints.
	 *
	 * (Note that we must still have a separate RI_ConstraintInfo for each
	 * constraint, because partitions can have different column orders,
	 * resulting in different pk_attnums[] or fk_attnums[] array contents.)
	 *
	 * We assume struct RI_QueryKey contains no padding bytes, else we'd need
	 * to use memset to clear them.
	 */
	if (constr_queryno != RI_PLAN_CHECK_LOOKUPPK_FROM_PK)
		key->constr_id = riinfo->constraint_root_id;
	else
		key->constr_id = riinfo->constraint_id;
	key->constr_queryno = constr_queryno;
}

/*
 * Check that RI trigger function was called in expected context
 */
static void
ri_CheckTrigger(FunctionCallInfo fcinfo, const char *funcname, int tgkind)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;

	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by trigger manager", funcname)));

	/*
	 * Check proper event
	 */
	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) ||
		!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired AFTER ROW", funcname)));

	switch (tgkind)
	{
		case RI_TRIGTYPE_INSERT:
			if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("function \"%s\" must be fired for INSERT", funcname)));
			break;
		case RI_TRIGTYPE_UPDATE:
			if (!TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("function \"%s\" must be fired for UPDATE", funcname)));
			break;
		case RI_TRIGTYPE_DELETE:
			if (!TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
						 errmsg("function \"%s\" must be fired for DELETE", funcname)));
			break;
	}
}


/*
 * Fetch the RI_ConstraintInfo struct for the trigger's FK constraint.
 */
static const RI_ConstraintInfo *
ri_FetchConstraintInfo(Trigger *trigger, Relation trig_rel, bool rel_is_pk)
{
	Oid			constraintOid = trigger->tgconstraint;
	const RI_ConstraintInfo *riinfo;

	/*
	 * Check that the FK constraint's OID is available; it might not be if
	 * we've been invoked via an ordinary trigger or an old-style "constraint
	 * trigger".
	 */
	if (!OidIsValid(constraintOid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("no pg_constraint entry for trigger \"%s\" on table \"%s\"",
						trigger->tgname, RelationGetRelationName(trig_rel)),
				 errhint("Remove this referential integrity trigger and its mates, then do ALTER TABLE ADD CONSTRAINT.")));

	/* Find or create a hashtable entry for the constraint */
	riinfo = ri_LoadConstraintInfo(constraintOid);

	/* Do some easy cross-checks against the trigger call data */
	if (rel_is_pk)
	{
		if (riinfo->fk_relid != trigger->tgconstrrelid ||
			riinfo->pk_relid != RelationGetRelid(trig_rel))
			elog(ERROR, "wrong pg_constraint entry for trigger \"%s\" on table \"%s\"",
				 trigger->tgname, RelationGetRelationName(trig_rel));
	}
	else
	{
		if (riinfo->fk_relid != RelationGetRelid(trig_rel) ||
			riinfo->pk_relid != trigger->tgconstrrelid)
			elog(ERROR, "wrong pg_constraint entry for trigger \"%s\" on table \"%s\"",
				 trigger->tgname, RelationGetRelationName(trig_rel));
	}

	if (riinfo->confmatchtype != FKCONSTR_MATCH_FULL &&
		riinfo->confmatchtype != FKCONSTR_MATCH_PARTIAL &&
		riinfo->confmatchtype != FKCONSTR_MATCH_SIMPLE)
		elog(ERROR, "unrecognized confmatchtype: %d",
			 riinfo->confmatchtype);

	if (riinfo->confmatchtype == FKCONSTR_MATCH_PARTIAL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MATCH PARTIAL not yet implemented")));

	return riinfo;
}

/*
 * Fetch or create the RI_ConstraintInfo struct for an FK constraint.
 */
static const RI_ConstraintInfo *
ri_LoadConstraintInfo(Oid constraintOid)
{
	RI_ConstraintInfo *riinfo;
	bool		found;
	HeapTuple	tup;
	Form_pg_constraint conForm;

	/*
	 * On the first call initialize the hashtable
	 */
	if (!ri_constraint_cache)
		ri_InitHashTables();

	/*
	 * Find or create a hash entry.  If we find a valid one, just return it.
	 */
	riinfo = (RI_ConstraintInfo *) hash_search(ri_constraint_cache,
											   &constraintOid,
											   HASH_ENTER, &found);
	if (!found)
		riinfo->valid = false;
	else if (riinfo->valid)
		return riinfo;

	/*
	 * Fetch the pg_constraint row so we can fill in the entry.
	 */
	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constraintOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for constraint %u", constraintOid);
	conForm = (Form_pg_constraint) GETSTRUCT(tup);

	if (conForm->contype != CONSTRAINT_FOREIGN) /* should not happen */
		elog(ERROR, "constraint %u is not a foreign key constraint",
			 constraintOid);

	/* And extract data */
	Assert(riinfo->constraint_id == constraintOid);
	if (OidIsValid(conForm->conparentid))
		riinfo->constraint_root_id =
			get_ri_constraint_root(conForm->conparentid);
	else
		riinfo->constraint_root_id = constraintOid;
	riinfo->oidHashValue = GetSysCacheHashValue1(CONSTROID,
												 ObjectIdGetDatum(constraintOid));
	riinfo->rootHashValue = GetSysCacheHashValue1(CONSTROID,
												  ObjectIdGetDatum(riinfo->constraint_root_id));
	memcpy(&riinfo->conname, &conForm->conname, sizeof(NameData));
	riinfo->pk_relid = conForm->confrelid;
	riinfo->fk_relid = conForm->conrelid;
	riinfo->confupdtype = conForm->confupdtype;
	riinfo->confdeltype = conForm->confdeltype;
	riinfo->confmatchtype = conForm->confmatchtype;
	riinfo->hasperiod = conForm->conperiod;

	DeconstructFkConstraintRow(tup,
							   &riinfo->nkeys,
							   riinfo->fk_attnums,
							   riinfo->pk_attnums,
							   riinfo->pf_eq_oprs,
							   riinfo->pp_eq_oprs,
							   riinfo->ff_eq_oprs,
							   &riinfo->ndelsetcols,
							   riinfo->confdelsetcols);

	/*
	 * For temporal FKs, get the operators and functions we need. We ask the
	 * opclass of the PK element for these. This all gets cached (as does the
	 * generated plan), so there's no performance issue.
	 */
	if (riinfo->hasperiod)
	{
		Oid			opclass = get_index_column_opclass(conForm->conindid, riinfo->nkeys);

		FindFKPeriodOpers(opclass,
						  &riinfo->period_contained_by_oper,
						  &riinfo->agged_period_contained_by_oper);
	}

	ReleaseSysCache(tup);

	/*
	 * For efficient processing of invalidation messages below, we keep a
	 * doubly-linked count list of all currently valid entries.
	 */
	dclist_push_tail(&ri_constraint_cache_valid_list, &riinfo->valid_link);

	riinfo->valid = true;

	return riinfo;
}

/*
 * get_ri_constraint_root
 *		Returns the OID of the constraint's root parent
 */
static Oid
get_ri_constraint_root(Oid constrOid)
{
	for (;;)
	{
		HeapTuple	tuple;
		Oid			constrParentOid;

		tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(constrOid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for constraint %u", constrOid);
		constrParentOid = ((Form_pg_constraint) GETSTRUCT(tuple))->conparentid;
		ReleaseSysCache(tuple);
		if (!OidIsValid(constrParentOid))
			break;				/* we reached the root constraint */
		constrOid = constrParentOid;
	}
	return constrOid;
}

/*
 * Callback for pg_constraint inval events
 *
 * While most syscache callbacks just flush all their entries, pg_constraint
 * gets enough update traffic that it's probably worth being smarter.
 * Invalidate any ri_constraint_cache entry associated with the syscache
 * entry with the specified hash value, or all entries if hashvalue == 0.
 *
 * Note: at the time a cache invalidation message is processed there may be
 * active references to the cache.  Because of this we never remove entries
 * from the cache, but only mark them invalid, which is harmless to active
 * uses.  (Any query using an entry should hold a lock sufficient to keep that
 * data from changing under it --- but we may get cache flushes anyway.)
 */
static void
InvalidateConstraintCacheCallBack(Datum arg, int cacheid, uint32 hashvalue)
{
	dlist_mutable_iter iter;

	Assert(ri_constraint_cache != NULL);

	/*
	 * If the list of currently valid entries gets excessively large, we mark
	 * them all invalid so we can empty the list.  This arrangement avoids
	 * O(N^2) behavior in situations where a session touches many foreign keys
	 * and also does many ALTER TABLEs, such as a restore from pg_dump.
	 */
	if (dclist_count(&ri_constraint_cache_valid_list) > 1000)
		hashvalue = 0;			/* pretend it's a cache reset */

	dclist_foreach_modify(iter, &ri_constraint_cache_valid_list)
	{
		RI_ConstraintInfo *riinfo = dclist_container(RI_ConstraintInfo,
													 valid_link, iter.cur);

		/*
		 * We must invalidate not only entries directly matching the given
		 * hash value, but also child entries, in case the invalidation
		 * affects a root constraint.
		 */
		if (hashvalue == 0 ||
			riinfo->oidHashValue == hashvalue ||
			riinfo->rootHashValue == hashvalue)
		{
			riinfo->valid = false;
			/* Remove invalidated entries from the list, too */
			dclist_delete_from(&ri_constraint_cache_valid_list, iter.cur);
		}
	}
}


/*
 * Prepare execution plan for a query to enforce an RI restriction
 */
static SPIPlanPtr
ri_PlanCheck(const char *querystr, int nargs, Oid *argtypes,
			 RI_QueryKey *qkey, Relation fk_rel, Relation pk_rel)
{
	SPIPlanPtr	qplan;
	Relation	query_rel;
	Oid			save_userid;
	int			save_sec_context;

	/*
	 * Use the query type code to determine whether the query is run against
	 * the PK or FK table; we'll do the check as that table's owner
	 */
	if (qkey->constr_queryno <= RI_PLAN_LAST_ON_PK)
		query_rel = pk_rel;
	else
		query_rel = fk_rel;

	/* Switch to proper UID to perform check as */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(RelationGetForm(query_rel)->relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_NOFORCE_RLS);

	/* Create the plan */
	qplan = SPI_prepare(querystr, nargs, argtypes);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %s for %s", SPI_result_code_string(SPI_result), querystr);

	/* Restore UID and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* Save the plan */
	SPI_keepplan(qplan);
	ri_HashPreparedPlan(qkey, qplan);

	return qplan;
}

/*
 * Perform a query to enforce an RI restriction
 */
static bool
ri_PerformCheck(const RI_ConstraintInfo *riinfo,
				RI_QueryKey *qkey, SPIPlanPtr qplan,
				Relation fk_rel, Relation pk_rel,
				TupleTableSlot *oldslot, TupleTableSlot *newslot,
				bool detectNewRows, int expect_OK)
{
	Relation	query_rel,
				source_rel;
	bool		source_is_pk;
	Snapshot	test_snapshot;
	Snapshot	crosscheck_snapshot;
	int			limit;
	int			spi_result;
	Oid			save_userid;
	int			save_sec_context;
	Datum		vals[RI_MAX_NUMKEYS * 2];
	char		nulls[RI_MAX_NUMKEYS * 2];

	/*
	 * Use the query type code to determine whether the query is run against
	 * the PK or FK table; we'll do the check as that table's owner
	 */
	if (qkey->constr_queryno <= RI_PLAN_LAST_ON_PK)
		query_rel = pk_rel;
	else
		query_rel = fk_rel;

	/*
	 * The values for the query are taken from the table on which the trigger
	 * is called - it is normally the other one with respect to query_rel. An
	 * exception is ri_Check_Pk_Match(), which uses the PK table for both (and
	 * sets queryno to RI_PLAN_CHECK_LOOKUPPK_FROM_PK).  We might eventually
	 * need some less klugy way to determine this.
	 */
	if (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK)
	{
		source_rel = fk_rel;
		source_is_pk = false;
	}
	else
	{
		source_rel = pk_rel;
		source_is_pk = true;
	}

	/* Extract the parameters to be passed into the query */
	if (newslot)
	{
		ri_ExtractValues(source_rel, newslot, riinfo, source_is_pk,
						 vals, nulls);
		if (oldslot)
			ri_ExtractValues(source_rel, oldslot, riinfo, source_is_pk,
							 vals + riinfo->nkeys, nulls + riinfo->nkeys);
	}
	else
	{
		ri_ExtractValues(source_rel, oldslot, riinfo, source_is_pk,
						 vals, nulls);
	}

	/*
	 * In READ COMMITTED mode, we just need to use an up-to-date regular
	 * snapshot, and we will see all rows that could be interesting. But in
	 * transaction-snapshot mode, we can't change the transaction snapshot. If
	 * the caller passes detectNewRows == false then it's okay to do the query
	 * with the transaction snapshot; otherwise we use a current snapshot, and
	 * tell the executor to error out if it finds any rows under the current
	 * snapshot that wouldn't be visible per the transaction snapshot.  Note
	 * that SPI_execute_snapshot will register the snapshots, so we don't need
	 * to bother here.
	 */
	if (IsolationUsesXactSnapshot() && detectNewRows)
	{
		CommandCounterIncrement();	/* be sure all my own work is visible */
		test_snapshot = GetLatestSnapshot();
		crosscheck_snapshot = GetTransactionSnapshot();
	}
	else
	{
		/* the default SPI behavior is okay */
		test_snapshot = InvalidSnapshot;
		crosscheck_snapshot = InvalidSnapshot;
	}

	/*
	 * If this is a select query (e.g., for a 'no action' or 'restrict'
	 * trigger), we only need to see if there is a single row in the table,
	 * matching the key.  Otherwise, limit = 0 - because we want the query to
	 * affect ALL the matching rows.
	 */
	limit = (expect_OK == SPI_OK_SELECT) ? 1 : 0;

	/* Switch to proper UID to perform check as */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(RelationGetForm(query_rel)->relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE |
						   SECURITY_NOFORCE_RLS);

	/* Finally we can run the query. */
	spi_result = SPI_execute_snapshot(qplan,
									  vals, nulls,
									  test_snapshot, crosscheck_snapshot,
									  false, false, limit);

	/* Restore UID and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* Check result */
	if (spi_result < 0)
		elog(ERROR, "SPI_execute_snapshot returned %s", SPI_result_code_string(spi_result));

	if (expect_OK >= 0 && spi_result != expect_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("referential integrity query on \"%s\" from constraint \"%s\" on \"%s\" gave unexpected result",
						RelationGetRelationName(pk_rel),
						NameStr(riinfo->conname),
						RelationGetRelationName(fk_rel)),
				 errhint("This is most likely due to a rule having rewritten the query.")));

	/* XXX wouldn't it be clearer to do this part at the caller? */
	if (qkey->constr_queryno != RI_PLAN_CHECK_LOOKUPPK_FROM_PK &&
		expect_OK == SPI_OK_SELECT &&
		(SPI_processed == 0) == (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK))
		ri_ReportViolation(riinfo,
						   pk_rel, fk_rel,
						   newslot ? newslot : oldslot,
						   NULL,
						   qkey->constr_queryno, false);

	return SPI_processed != 0;
}

/*
 * Extract fields from a tuple into Datum/nulls arrays
 */
static void
ri_ExtractValues(Relation rel, TupleTableSlot *slot,
				 const RI_ConstraintInfo *riinfo, bool rel_is_pk,
				 Datum *vals, char *nulls)
{
	const int16 *attnums;
	bool		isnull;

	if (rel_is_pk)
		attnums = riinfo->pk_attnums;
	else
		attnums = riinfo->fk_attnums;

	for (int i = 0; i < riinfo->nkeys; i++)
	{
		vals[i] = slot_getattr(slot, attnums[i], &isnull);
		nulls[i] = isnull ? 'n' : ' ';
	}
}

/*
 * Produce an error report
 *
 * If the failed constraint was on insert/update to the FK table,
 * we want the key names and values extracted from there, and the error
 * message to look like 'key blah is not present in PK'.
 * Otherwise, the attr names and values come from the PK table and the
 * message looks like 'key blah is still referenced from FK'.
 */
static void
ri_ReportViolation(const RI_ConstraintInfo *riinfo,
				   Relation pk_rel, Relation fk_rel,
				   TupleTableSlot *violatorslot, TupleDesc tupdesc,
				   int queryno, bool partgone)
{
	StringInfoData key_names;
	StringInfoData key_values;
	bool		onfk;
	const int16 *attnums;
	Oid			rel_oid;
	AclResult	aclresult;
	bool		has_perm = true;

	/*
	 * Determine which relation to complain about.  If tupdesc wasn't passed
	 * by caller, assume the violator tuple came from there.
	 */
	onfk = (queryno == RI_PLAN_CHECK_LOOKUPPK);
	if (onfk)
	{
		attnums = riinfo->fk_attnums;
		rel_oid = fk_rel->rd_id;
		if (tupdesc == NULL)
			tupdesc = fk_rel->rd_att;
	}
	else
	{
		attnums = riinfo->pk_attnums;
		rel_oid = pk_rel->rd_id;
		if (tupdesc == NULL)
			tupdesc = pk_rel->rd_att;
	}

	/*
	 * Check permissions- if the user does not have access to view the data in
	 * any of the key columns then we don't include the errdetail() below.
	 *
	 * Check if RLS is enabled on the relation first.  If so, we don't return
	 * any specifics to avoid leaking data.
	 *
	 * Check table-level permissions next and, failing that, column-level
	 * privileges.
	 *
	 * When a partition at the referenced side is being detached/dropped, we
	 * needn't check, since the user must be the table owner anyway.
	 */
	if (partgone)
		has_perm = true;
	else if (check_enable_rls(rel_oid, InvalidOid, true) != RLS_ENABLED)
	{
		aclresult = pg_class_aclcheck(rel_oid, GetUserId(), ACL_SELECT);
		if (aclresult != ACLCHECK_OK)
		{
			/* Try for column-level permissions */
			for (int idx = 0; idx < riinfo->nkeys; idx++)
			{
				aclresult = pg_attribute_aclcheck(rel_oid, attnums[idx],
												  GetUserId(),
												  ACL_SELECT);

				/* No access to the key */
				if (aclresult != ACLCHECK_OK)
				{
					has_perm = false;
					break;
				}
			}
		}
	}
	else
		has_perm = false;

	if (has_perm)
	{
		/* Get printable versions of the keys involved */
		initStringInfo(&key_names);
		initStringInfo(&key_values);
		for (int idx = 0; idx < riinfo->nkeys; idx++)
		{
			int			fnum = attnums[idx];
			Form_pg_attribute att = TupleDescAttr(tupdesc, fnum - 1);
			char	   *name,
					   *val;
			Datum		datum;
			bool		isnull;

			name = NameStr(att->attname);

			datum = slot_getattr(violatorslot, fnum, &isnull);
			if (!isnull)
			{
				Oid			foutoid;
				bool		typisvarlena;

				getTypeOutputInfo(att->atttypid, &foutoid, &typisvarlena);
				val = OidOutputFunctionCall(foutoid, datum);
			}
			else
				val = "null";

			if (idx > 0)
			{
				appendStringInfoString(&key_names, ", ");
				appendStringInfoString(&key_values, ", ");
			}
			appendStringInfoString(&key_names, name);
			appendStringInfoString(&key_values, val);
		}
	}

	if (partgone)
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("removing partition \"%s\" violates foreign key constraint \"%s\"",
						RelationGetRelationName(pk_rel),
						NameStr(riinfo->conname)),
				 errdetail("Key (%s)=(%s) is still referenced from table \"%s\".",
						   key_names.data, key_values.data,
						   RelationGetRelationName(fk_rel)),
				 errtableconstraint(fk_rel, NameStr(riinfo->conname))));
	else if (onfk)
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
						RelationGetRelationName(fk_rel),
						NameStr(riinfo->conname)),
				 has_perm ?
				 errdetail("Key (%s)=(%s) is not present in table \"%s\".",
						   key_names.data, key_values.data,
						   RelationGetRelationName(pk_rel)) :
				 errdetail("Key is not present in table \"%s\".",
						   RelationGetRelationName(pk_rel)),
				 errtableconstraint(fk_rel, NameStr(riinfo->conname))));
	else
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("update or delete on table \"%s\" violates foreign key constraint \"%s\" on table \"%s\"",
						RelationGetRelationName(pk_rel),
						NameStr(riinfo->conname),
						RelationGetRelationName(fk_rel)),
				 has_perm ?
				 errdetail("Key (%s)=(%s) is still referenced from table \"%s\".",
						   key_names.data, key_values.data,
						   RelationGetRelationName(fk_rel)) :
				 errdetail("Key is still referenced from table \"%s\".",
						   RelationGetRelationName(fk_rel)),
				 errtableconstraint(fk_rel, NameStr(riinfo->conname))));
}


/*
 * ri_NullCheck -
 *
 * Determine the NULL state of all key values in a tuple
 *
 * Returns one of RI_KEYS_ALL_NULL, RI_KEYS_NONE_NULL or RI_KEYS_SOME_NULL.
 */
static int
ri_NullCheck(TupleDesc tupDesc,
			 TupleTableSlot *slot,
			 const RI_ConstraintInfo *riinfo, bool rel_is_pk)
{
	const int16 *attnums;
	bool		allnull = true;
	bool		nonenull = true;

	if (rel_is_pk)
		attnums = riinfo->pk_attnums;
	else
		attnums = riinfo->fk_attnums;

	for (int i = 0; i < riinfo->nkeys; i++)
	{
		if (slot_attisnull(slot, attnums[i]))
			nonenull = false;
		else
			allnull = false;
	}

	if (allnull)
		return RI_KEYS_ALL_NULL;

	if (nonenull)
		return RI_KEYS_NONE_NULL;

	return RI_KEYS_SOME_NULL;
}


/*
 * ri_InitHashTables -
 *
 * Initialize our internal hash tables.
 */
static void
ri_InitHashTables(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(RI_ConstraintInfo);
	ri_constraint_cache = hash_create("RI constraint cache",
									  RI_INIT_CONSTRAINTHASHSIZE,
									  &ctl, HASH_ELEM | HASH_BLOBS);

	/* Arrange to flush cache on pg_constraint changes */
	CacheRegisterSyscacheCallback(CONSTROID,
								  InvalidateConstraintCacheCallBack,
								  (Datum) 0);

	ctl.keysize = sizeof(RI_QueryKey);
	ctl.entrysize = sizeof(RI_QueryHashEntry);
	ri_query_cache = hash_create("RI query cache",
								 RI_INIT_QUERYHASHSIZE,
								 &ctl, HASH_ELEM | HASH_BLOBS);

	ctl.keysize = sizeof(RI_CompareKey);
	ctl.entrysize = sizeof(RI_CompareHashEntry);
	ri_compare_cache = hash_create("RI compare cache",
								   RI_INIT_QUERYHASHSIZE,
								   &ctl, HASH_ELEM | HASH_BLOBS);
}


/*
 * ri_FetchPreparedPlan -
 *
 * Lookup for a query key in our private hash table of prepared
 * and saved SPI execution plans. Return the plan if found or NULL.
 */
static SPIPlanPtr
ri_FetchPreparedPlan(RI_QueryKey *key)
{
	RI_QueryHashEntry *entry;
	SPIPlanPtr	plan;

	/*
	 * On the first call initialize the hashtable
	 */
	if (!ri_query_cache)
		ri_InitHashTables();

	/*
	 * Lookup for the key
	 */
	entry = (RI_QueryHashEntry *) hash_search(ri_query_cache,
											  key,
											  HASH_FIND, NULL);
	if (entry == NULL)
		return NULL;

	/*
	 * Check whether the plan is still valid.  If it isn't, we don't want to
	 * simply rely on plancache.c to regenerate it; rather we should start
	 * from scratch and rebuild the query text too.  This is to cover cases
	 * such as table/column renames.  We depend on the plancache machinery to
	 * detect possible invalidations, though.
	 *
	 * CAUTION: this check is only trustworthy if the caller has already
	 * locked both FK and PK rels.
	 */
	plan = entry->plan;
	if (plan && SPI_plan_is_valid(plan))
		return plan;

	/*
	 * Otherwise we might as well flush the cached plan now, to free a little
	 * memory space before we make a new one.
	 */
	entry->plan = NULL;
	if (plan)
		SPI_freeplan(plan);

	return NULL;
}


/*
 * ri_HashPreparedPlan -
 *
 * Add another plan to our private SPI query plan hashtable.
 */
static void
ri_HashPreparedPlan(RI_QueryKey *key, SPIPlanPtr plan)
{
	RI_QueryHashEntry *entry;
	bool		found;

	/*
	 * On the first call initialize the hashtable
	 */
	if (!ri_query_cache)
		ri_InitHashTables();

	/*
	 * Add the new plan.  We might be overwriting an entry previously found
	 * invalid by ri_FetchPreparedPlan.
	 */
	entry = (RI_QueryHashEntry *) hash_search(ri_query_cache,
											  key,
											  HASH_ENTER, &found);
	Assert(!found || entry->plan == NULL);
	entry->plan = plan;
}


/*
 * ri_KeysEqual -
 *
 * Check if all key values in OLD and NEW are "equivalent":
 * For normal FKs we check for equality.
 * For temporal FKs we check that the PK side is a superset of its old value,
 * or the FK side is a subset of its old value.
 *
 * Note: at some point we might wish to redefine this as checking for
 * "IS NOT DISTINCT" rather than "=", that is, allow two nulls to be
 * considered equal.  Currently there is no need since all callers have
 * previously found at least one of the rows to contain no nulls.
 */
static bool
ri_KeysEqual(Relation rel, TupleTableSlot *oldslot, TupleTableSlot *newslot,
			 const RI_ConstraintInfo *riinfo, bool rel_is_pk)
{
	const int16 *attnums;

	if (rel_is_pk)
		attnums = riinfo->pk_attnums;
	else
		attnums = riinfo->fk_attnums;

	/* XXX: could be worthwhile to fetch all necessary attrs at once */
	for (int i = 0; i < riinfo->nkeys; i++)
	{
		Datum		oldvalue;
		Datum		newvalue;
		bool		isnull;

		/*
		 * Get one attribute's oldvalue. If it is NULL - they're not equal.
		 */
		oldvalue = slot_getattr(oldslot, attnums[i], &isnull);
		if (isnull)
			return false;

		/*
		 * Get one attribute's newvalue. If it is NULL - they're not equal.
		 */
		newvalue = slot_getattr(newslot, attnums[i], &isnull);
		if (isnull)
			return false;

		if (rel_is_pk)
		{
			/*
			 * If we are looking at the PK table, then do a bytewise
			 * comparison.  We must propagate PK changes if the value is
			 * changed to one that "looks" different but would compare as
			 * equal using the equality operator.  This only makes a
			 * difference for ON UPDATE CASCADE, but for consistency we treat
			 * all changes to the PK the same.
			 */
			Form_pg_attribute att = TupleDescAttr(oldslot->tts_tupleDescriptor, attnums[i] - 1);

			if (!datum_image_eq(oldvalue, newvalue, att->attbyval, att->attlen))
				return false;
		}
		else
		{
			Oid			eq_opr;

			/*
			 * When comparing the PERIOD columns we can skip the check
			 * whenever the referencing column stayed equal or shrank, so test
			 * with the contained-by operator instead.
			 */
			if (riinfo->hasperiod && i == riinfo->nkeys - 1)
				eq_opr = riinfo->period_contained_by_oper;
			else
				eq_opr = riinfo->ff_eq_oprs[i];

			/*
			 * For the FK table, compare with the appropriate equality
			 * operator.  Changes that compare equal will still satisfy the
			 * constraint after the update.
			 */
			if (!ri_CompareWithCast(eq_opr, RIAttType(rel, attnums[i]), RIAttCollation(rel, attnums[i]),
									newvalue, oldvalue))
				return false;
		}
	}

	return true;
}


/*
 * ri_CompareWithCast -
 *
 * Call the appropriate comparison operator for two values.
 * Normally this is equality, but for the PERIOD part of foreign keys
 * it is ContainedBy, so the order of lhs vs rhs is significant.
 * See below for how the collation is applied.
 *
 * NB: we have already checked that neither value is null.
 */
static bool
ri_CompareWithCast(Oid eq_opr, Oid typeid, Oid collid,
				   Datum lhs, Datum rhs)
{
	RI_CompareHashEntry *entry = ri_HashCompareOp(eq_opr, typeid);

	/* Do we need to cast the values? */
	if (OidIsValid(entry->cast_func_finfo.fn_oid))
	{
		lhs = FunctionCall3(&entry->cast_func_finfo,
							lhs,
							Int32GetDatum(-1),	/* typmod */
							BoolGetDatum(false));	/* implicit coercion */
		rhs = FunctionCall3(&entry->cast_func_finfo,
							rhs,
							Int32GetDatum(-1),	/* typmod */
							BoolGetDatum(false));	/* implicit coercion */
	}

	/*
	 * Apply the comparison operator.
	 *
	 * Note: This function is part of a call stack that determines whether an
	 * update to a row is significant enough that it needs checking or action
	 * on the other side of a foreign-key constraint.  Therefore, the
	 * comparison here would need to be done with the collation of the *other*
	 * table.  For simplicity (e.g., we might not even have the other table
	 * open), we'll use our own collation.  This is fine because we require
	 * that both collations have the same notion of equality (either they are
	 * both deterministic or else they are both the same).
	 *
	 * With range/multirangetypes, the collation of the base type is stored as
	 * part of the rangetype (pg_range.rngcollation), and always used, so
	 * there is no danger of inconsistency even using a non-equals operator.
	 * But if we support arbitrary types with PERIOD, we should perhaps just
	 * always force a re-check.
	 */
	return DatumGetBool(FunctionCall2Coll(&entry->eq_opr_finfo, collid, lhs, rhs));
}

/*
 * ri_HashCompareOp -
 *
 * See if we know how to compare two values, and create a new hash entry
 * if not.
 */
static RI_CompareHashEntry *
ri_HashCompareOp(Oid eq_opr, Oid typeid)
{
	RI_CompareKey key;
	RI_CompareHashEntry *entry;
	bool		found;

	/*
	 * On the first call initialize the hashtable
	 */
	if (!ri_compare_cache)
		ri_InitHashTables();

	/*
	 * Find or create a hash entry.  Note we're assuming RI_CompareKey
	 * contains no struct padding.
	 */
	key.eq_opr = eq_opr;
	key.typeid = typeid;
	entry = (RI_CompareHashEntry *) hash_search(ri_compare_cache,
												&key,
												HASH_ENTER, &found);
	if (!found)
		entry->valid = false;

	/*
	 * If not already initialized, do so.  Since we'll keep this hash entry
	 * for the life of the backend, put any subsidiary info for the function
	 * cache structs into TopMemoryContext.
	 */
	if (!entry->valid)
	{
		Oid			lefttype,
					righttype,
					castfunc;
		CoercionPathType pathtype;

		/* We always need to know how to call the equality operator */
		fmgr_info_cxt(get_opcode(eq_opr), &entry->eq_opr_finfo,
					  TopMemoryContext);

		/*
		 * If we chose to use a cast from FK to PK type, we may have to apply
		 * the cast function to get to the operator's input type.
		 *
		 * XXX eventually it would be good to support array-coercion cases
		 * here and in ri_CompareWithCast().  At the moment there is no point
		 * because cases involving nonidentical array types will be rejected
		 * at constraint creation time.
		 *
		 * XXX perhaps also consider supporting CoerceViaIO?  No need at the
		 * moment since that will never be generated for implicit coercions.
		 */
		op_input_types(eq_opr, &lefttype, &righttype);
		Assert(lefttype == righttype);
		if (typeid == lefttype)
			castfunc = InvalidOid;	/* simplest case */
		else
		{
			pathtype = find_coercion_pathway(lefttype, typeid,
											 COERCION_IMPLICIT,
											 &castfunc);
			if (pathtype != COERCION_PATH_FUNC &&
				pathtype != COERCION_PATH_RELABELTYPE)
			{
				/*
				 * The declared input type of the eq_opr might be a
				 * polymorphic type such as ANYARRAY or ANYENUM, or other
				 * special cases such as RECORD; find_coercion_pathway
				 * currently doesn't subsume these special cases.
				 */
				if (!IsBinaryCoercible(typeid, lefttype))
					elog(ERROR, "no conversion function from %s to %s",
						 format_type_be(typeid),
						 format_type_be(lefttype));
			}
		}
		if (OidIsValid(castfunc))
			fmgr_info_cxt(castfunc, &entry->cast_func_finfo,
						  TopMemoryContext);
		else
			entry->cast_func_finfo.fn_oid = InvalidOid;
		entry->valid = true;
	}

	return entry;
}


/*
 * Given a trigger function OID, determine whether it is an RI trigger,
 * and if so whether it is attached to PK or FK relation.
 */
int
RI_FKey_trigger_type(Oid tgfoid)
{
	switch (tgfoid)
	{
		case F_RI_FKEY_CASCADE_DEL:
		case F_RI_FKEY_CASCADE_UPD:
		case F_RI_FKEY_RESTRICT_DEL:
		case F_RI_FKEY_RESTRICT_UPD:
		case F_RI_FKEY_SETNULL_DEL:
		case F_RI_FKEY_SETNULL_UPD:
		case F_RI_FKEY_SETDEFAULT_DEL:
		case F_RI_FKEY_SETDEFAULT_UPD:
		case F_RI_FKEY_NOACTION_DEL:
		case F_RI_FKEY_NOACTION_UPD:
			return RI_TRIGGER_PK;

		case F_RI_FKEY_CHECK_INS:
		case F_RI_FKEY_CHECK_UPD:
			return RI_TRIGGER_FK;
	}

	return RI_TRIGGER_NONE;
}
