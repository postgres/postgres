/* ----------
 * ri_triggers.c
 *
 *	Generic trigger procedures for referential integrity constraint
 *	checks.
 *
 *	Note about memory management: the private hashtables kept here live
 *	across query and transaction boundaries, in fact they live as long as
 *	the backend does.  This works because the hashtable structures
 *	themselves are allocated by dynahash.c in its permanent DynaHashCxt,
 *	and the SPI plans they point to are saved using SPI_saveplan().
 *	There is not currently any provision for throwing away a no-longer-needed
 *	plan --- consider improving this someday.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/ri_triggers.c,v 1.102 2008/01/25 04:46:07 tgl Exp $
 *
 * ----------
 */


/* ----------
 * Internal TODO:
 *
 *		Add MATCH PARTIAL logic.
 * ----------
 */

#include "postgres.h"

#include "catalog/pg_constraint.h"
#include "catalog/pg_operator.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


/* ----------
 * Local definitions
 * ----------
 */

#define RI_MAX_NUMKEYS					INDEX_MAX_KEYS

#define RI_INIT_QUERYHASHSIZE			128

#define RI_KEYS_ALL_NULL				0
#define RI_KEYS_SOME_NULL				1
#define RI_KEYS_NONE_NULL				2

/* queryno values must be distinct for the convenience of ri_PerformCheck */
#define RI_PLAN_CHECK_LOOKUPPK_NOCOLS	1
#define RI_PLAN_CHECK_LOOKUPPK			2
#define RI_PLAN_CASCADE_DEL_DODELETE	3
#define RI_PLAN_CASCADE_UPD_DOUPDATE	4
#define RI_PLAN_NOACTION_DEL_CHECKREF	5
#define RI_PLAN_NOACTION_UPD_CHECKREF	6
#define RI_PLAN_RESTRICT_DEL_CHECKREF	7
#define RI_PLAN_RESTRICT_UPD_CHECKREF	8
#define RI_PLAN_SETNULL_DEL_DOUPDATE	9
#define RI_PLAN_SETNULL_UPD_DOUPDATE	10
#define RI_PLAN_KEYEQUAL_UPD			11

#define MAX_QUOTED_NAME_LEN  (NAMEDATALEN*2+3)
#define MAX_QUOTED_REL_NAME_LEN  (MAX_QUOTED_NAME_LEN*2)

#define RIAttName(rel, attnum)	NameStr(*attnumAttName(rel, attnum))
#define RIAttType(rel, attnum)	SPI_gettypeid(RelationGetDescr(rel), attnum)

#define RI_TRIGTYPE_INSERT 1
#define RI_TRIGTYPE_UPDATE 2
#define RI_TRIGTYPE_INUP   3
#define RI_TRIGTYPE_DELETE 4

#define RI_KEYPAIR_FK_IDX	0
#define RI_KEYPAIR_PK_IDX	1


/* ----------
 * RI_ConstraintInfo
 *
 *	Information extracted from an FK pg_constraint entry.
 * ----------
 */
typedef struct RI_ConstraintInfo
{
	Oid			constraint_id;	/* OID of pg_constraint entry */
	NameData	conname;		/* name of the FK constraint */
	Oid			pk_relid;		/* referenced relation */
	Oid			fk_relid;		/* referencing relation */
	char		confupdtype;	/* foreign key's ON UPDATE action */
	char		confdeltype;	/* foreign key's ON DELETE action */
	char		confmatchtype;	/* foreign key's match type */
	int			nkeys;			/* number of key columns */
	int16		pk_attnums[RI_MAX_NUMKEYS];		/* attnums of referenced cols */
	int16		fk_attnums[RI_MAX_NUMKEYS];		/* attnums of referencing cols */
	Oid			pf_eq_oprs[RI_MAX_NUMKEYS];		/* equality operators (PK =
												 * FK) */
	Oid			pp_eq_oprs[RI_MAX_NUMKEYS];		/* equality operators (PK =
												 * PK) */
	Oid			ff_eq_oprs[RI_MAX_NUMKEYS];		/* equality operators (FK =
												 * FK) */
} RI_ConstraintInfo;


/* ----------
 * RI_QueryKey
 *
 *	The key identifying a prepared SPI plan in our query hashtable
 * ----------
 */
typedef struct RI_QueryKey
{
	char		constr_type;
	Oid			constr_id;
	int32		constr_queryno;
	Oid			fk_relid;
	Oid			pk_relid;
	int32		nkeypairs;
	int16		keypair[RI_MAX_NUMKEYS][2];
} RI_QueryKey;


/* ----------
 * RI_QueryHashEntry
 * ----------
 */
typedef struct RI_QueryHashEntry
{
	RI_QueryKey key;
	SPIPlanPtr	plan;
} RI_QueryHashEntry;


/* ----------
 * RI_CompareKey
 *
 *	The key identifying an entry showing how to compare two values
 * ----------
 */
typedef struct RI_CompareKey
{
	Oid			eq_opr;			/* the equality operator to apply */
	Oid			typeid;			/* the data type to apply it to */
} RI_CompareKey;


/* ----------
 * RI_CompareHashEntry
 * ----------
 */
typedef struct RI_CompareHashEntry
{
	RI_CompareKey key;
	bool		valid;			/* successfully initialized? */
	FmgrInfo	eq_opr_finfo;	/* call info for equality fn */
	FmgrInfo	cast_func_finfo;	/* in case we must coerce input */
} RI_CompareHashEntry;


/* ----------
 * Local data
 * ----------
 */
static HTAB *ri_query_cache = NULL;
static HTAB *ri_compare_cache = NULL;


/* ----------
 * Local function prototypes
 * ----------
 */
static void quoteOneName(char *buffer, const char *name);
static void quoteRelationName(char *buffer, Relation rel);
static void ri_GenerateQual(StringInfo buf,
				const char *sep,
				const char *leftop, Oid leftoptype,
				Oid opoid,
				const char *rightop, Oid rightoptype);
static int ri_NullCheck(Relation rel, HeapTuple tup,
			 RI_QueryKey *key, int pairidx);
static void ri_BuildQueryKeyFull(RI_QueryKey *key,
					 const RI_ConstraintInfo *riinfo,
					 int32 constr_queryno);
static void ri_BuildQueryKeyPkCheck(RI_QueryKey *key,
						const RI_ConstraintInfo *riinfo,
						int32 constr_queryno);
static bool ri_KeysEqual(Relation rel, HeapTuple oldtup, HeapTuple newtup,
			 const RI_ConstraintInfo *riinfo, bool rel_is_pk);
static bool ri_AllKeysUnequal(Relation rel, HeapTuple oldtup, HeapTuple newtup,
				  const RI_ConstraintInfo *riinfo, bool rel_is_pk);
static bool ri_OneKeyEqual(Relation rel, int column,
			   HeapTuple oldtup, HeapTuple newtup,
			   const RI_ConstraintInfo *riinfo, bool rel_is_pk);
static bool ri_AttributesEqual(Oid eq_opr, Oid typeid,
				   Datum oldvalue, Datum newvalue);
static bool ri_Check_Pk_Match(Relation pk_rel, Relation fk_rel,
				  HeapTuple old_row,
				  const RI_ConstraintInfo *riinfo);

static void ri_InitHashTables(void);
static SPIPlanPtr ri_FetchPreparedPlan(RI_QueryKey *key);
static void ri_HashPreparedPlan(RI_QueryKey *key, SPIPlanPtr plan);
static RI_CompareHashEntry *ri_HashCompareOp(Oid eq_opr, Oid typeid);

static void ri_CheckTrigger(FunctionCallInfo fcinfo, const char *funcname,
				int tgkind);
static void ri_FetchConstraintInfo(RI_ConstraintInfo *riinfo,
					   Trigger *trigger, Relation trig_rel, bool rel_is_pk);
static SPIPlanPtr ri_PlanCheck(const char *querystr, int nargs, Oid *argtypes,
			 RI_QueryKey *qkey, Relation fk_rel, Relation pk_rel,
			 bool cache_plan);
static bool ri_PerformCheck(RI_QueryKey *qkey, SPIPlanPtr qplan,
				Relation fk_rel, Relation pk_rel,
				HeapTuple old_tuple, HeapTuple new_tuple,
				bool detectNewRows,
				int expect_OK, const char *constrname);
static void ri_ExtractValues(RI_QueryKey *qkey, int key_idx,
				 Relation rel, HeapTuple tuple,
				 Datum *vals, char *nulls);
static void ri_ReportViolation(RI_QueryKey *qkey, const char *constrname,
				   Relation pk_rel, Relation fk_rel,
				   HeapTuple violator, TupleDesc tupdesc,
				   bool spi_err);


/* ----------
 * RI_FKey_check -
 *
 *	Check foreign key existence (combined for INSERT and UPDATE).
 * ----------
 */
static Datum
RI_FKey_check(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	Buffer		new_row_buf;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_check", RI_TRIGTYPE_INUP);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						 trigdata->tg_trigger, trigdata->tg_relation, false);

	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		old_row = trigdata->tg_trigtuple;
		new_row = trigdata->tg_newtuple;
		new_row_buf = trigdata->tg_newtuplebuf;
	}
	else
	{
		old_row = NULL;
		new_row = trigdata->tg_trigtuple;
		new_row_buf = trigdata->tg_trigtuplebuf;
	}

	/*
	 * We should not even consider checking the row if it is no longer valid,
	 * since it was either deleted (so the deferred check should be skipped)
	 * or updated (in which case only the latest version of the row should be
	 * checked).  Test its liveness according to SnapshotSelf.
	 *
	 * NOTE: The normal coding rule is that one must acquire the buffer
	 * content lock to call HeapTupleSatisfiesVisibility.  We can skip that
	 * here because we know that AfterTriggerExecute just fetched the tuple
	 * successfully, so there cannot be a VACUUM compaction in progress on the
	 * page (either heap_fetch would have waited for the VACUUM, or the
	 * VACUUM's LockBufferForCleanup would be waiting for us to drop pin). And
	 * since this is a row inserted by our open transaction, no one else can
	 * be entitled to change its xmin/xmax.
	 */
	Assert(new_row_buf != InvalidBuffer);
	if (!HeapTupleSatisfiesVisibility(new_row, SnapshotSelf, new_row_buf))
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables.
	 *
	 * pk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR SHARE will get on it.
	 */
	fk_rel = trigdata->tg_relation;
	pk_rel = heap_open(riinfo.pk_relid, RowShareLock);

	/* ----------
	 * SQL3 11.9 <referential constraint definition>
	 *	General rules 2) a):
	 *		If Rf and Rt are empty (no columns to compare given)
	 *		constraint is true if 0 < (SELECT COUNT(*) FROM T)
	 *
	 *	Note: The special case that no columns are given cannot
	 *		occur up to now in Postgres, it's just there for
	 *		future enhancements.
	 * ----------
	 */
	if (riinfo.nkeys == 0)
	{
		ri_BuildQueryKeyFull(&qkey, &riinfo, RI_PLAN_CHECK_LOOKUPPK_NOCOLS);

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "SPI_connect failed");

		if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
		{
			char		querystr[MAX_QUOTED_REL_NAME_LEN + 100];
			char		pkrelname[MAX_QUOTED_REL_NAME_LEN];

			/* ---------
			 * The query string built is
			 *	SELECT 1 FROM ONLY <pktable>
			 * ----------
			 */
			quoteRelationName(pkrelname, pk_rel);
			snprintf(querystr, sizeof(querystr),
					 "SELECT 1 FROM ONLY %s x FOR SHARE OF x",
					 pkrelname);

			/* Prepare and save the plan */
			qplan = ri_PlanCheck(querystr, 0, NULL,
								 &qkey, fk_rel, pk_rel, true);
		}

		/*
		 * Execute the plan
		 */
		ri_PerformCheck(&qkey, qplan,
						fk_rel, pk_rel,
						NULL, NULL,
						false,
						SPI_OK_SELECT,
						NameStr(riinfo.conname));

		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");

		heap_close(pk_rel, RowShareLock);

		return PointerGetDatum(NULL);
	}

	if (riinfo.confmatchtype == FKCONSTR_MATCH_PARTIAL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MATCH PARTIAL not yet implemented")));

	ri_BuildQueryKeyFull(&qkey, &riinfo, RI_PLAN_CHECK_LOOKUPPK);

	switch (ri_NullCheck(fk_rel, new_row, &qkey, RI_KEYPAIR_FK_IDX))
	{
		case RI_KEYS_ALL_NULL:

			/*
			 * No check - if NULLs are allowed at all is already checked by
			 * NOT NULL constraint.
			 *
			 * This is true for MATCH FULL, MATCH PARTIAL, and MATCH
			 * <unspecified>
			 */
			heap_close(pk_rel, RowShareLock);
			return PointerGetDatum(NULL);

		case RI_KEYS_SOME_NULL:

			/*
			 * This is the only case that differs between the three kinds of
			 * MATCH.
			 */
			switch (riinfo.confmatchtype)
			{
				case FKCONSTR_MATCH_FULL:

					/*
					 * Not allowed - MATCH FULL says either all or none of the
					 * attributes can be NULLs
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
							 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
							  RelationGetRelationName(trigdata->tg_relation),
									NameStr(riinfo.conname)),
							 errdetail("MATCH FULL does not allow mixing of null and nonnull key values.")));
					heap_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case FKCONSTR_MATCH_UNSPECIFIED:

					/*
					 * MATCH <unspecified> - if ANY column is null, we have a
					 * match.
					 */
					heap_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case FKCONSTR_MATCH_PARTIAL:

					/*
					 * MATCH PARTIAL - all non-null columns must match. (not
					 * implemented, can be done by modifying the query below
					 * to only include non-null columns, or by writing a
					 * special version here)
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("MATCH PARTIAL not yet implemented")));
					heap_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);
			}

		case RI_KEYS_NONE_NULL:

			/*
			 * Have a full qualified key - continue below for all three kinds
			 * of MATCH.
			 */
			break;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Fetch or prepare a saved plan for the real check
	 */
	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];

		/* ----------
		 * The query string built is
		 *	SELECT 1 FROM ONLY <pktable> WHERE pkatt1 = $1 [AND ...] FOR SHARE
		 * The type id's for the $ parameters are those of the
		 * corresponding FK attributes.
		 * ----------
		 */
		initStringInfo(&querybuf);
		quoteRelationName(pkrelname, pk_rel);
		appendStringInfo(&querybuf, "SELECT 1 FROM ONLY %s x", pkrelname);
		querysep = "WHERE";
		for (i = 0; i < riinfo.nkeys; i++)
		{
			Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
			Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

			quoteOneName(attname,
						 RIAttName(pk_rel, riinfo.pk_attnums[i]));
			sprintf(paramname, "$%d", i + 1);
			ri_GenerateQual(&querybuf, querysep,
							attname, pk_type,
							riinfo.pf_eq_oprs[i],
							paramname, fk_type);
			querysep = "AND";
			queryoids[i] = fk_type;
		}
		appendStringInfo(&querybuf, " FOR SHARE OF x");

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
							 &qkey, fk_rel, pk_rel, true);
	}

	/*
	 * Now check that foreign key exists in PK table
	 */
	ri_PerformCheck(&qkey, qplan,
					fk_rel, pk_rel,
					NULL, new_row,
					false,
					SPI_OK_SELECT,
					NameStr(riinfo.conname));

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	heap_close(pk_rel, RowShareLock);

	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_check_ins -
 *
 *	Check foreign key existence at insert event on FK table.
 * ----------
 */
Datum
RI_FKey_check_ins(PG_FUNCTION_ARGS)
{
	return RI_FKey_check(fcinfo);
}


/* ----------
 * RI_FKey_check_upd -
 *
 *	Check foreign key existence at update event on FK table.
 * ----------
 */
Datum
RI_FKey_check_upd(PG_FUNCTION_ARGS)
{
	return RI_FKey_check(fcinfo);
}


/* ----------
 * ri_Check_Pk_Match
 *
 * Check for matching value of old pk row in current state for
 * noaction triggers. Returns false if no row was found and a fk row
 * could potentially be referencing this row, true otherwise.
 * ----------
 */
static bool
ri_Check_Pk_Match(Relation pk_rel, Relation fk_rel,
				  HeapTuple old_row,
				  const RI_ConstraintInfo *riinfo)
{
	SPIPlanPtr	qplan;
	RI_QueryKey qkey;
	int			i;
	bool		result;

	ri_BuildQueryKeyPkCheck(&qkey, riinfo, RI_PLAN_CHECK_LOOKUPPK);

	switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
	{
		case RI_KEYS_ALL_NULL:

			/*
			 * No check - nothing could have been referencing this row anyway.
			 */
			return true;

		case RI_KEYS_SOME_NULL:

			/*
			 * This is the only case that differs between the three kinds of
			 * MATCH.
			 */
			switch (riinfo->confmatchtype)
			{
				case FKCONSTR_MATCH_FULL:
				case FKCONSTR_MATCH_UNSPECIFIED:

					/*
					 * MATCH <unspecified>/FULL  - if ANY column is null, we
					 * can't be matching to this row already.
					 */
					return true;

				case FKCONSTR_MATCH_PARTIAL:

					/*
					 * MATCH PARTIAL - all non-null columns must match. (not
					 * implemented, can be done by modifying the query below
					 * to only include non-null columns, or by writing a
					 * special version here)
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("MATCH PARTIAL not yet implemented")));
					break;
			}

		case RI_KEYS_NONE_NULL:

			/*
			 * Have a full qualified key - continue below for all three kinds
			 * of MATCH.
			 */
			break;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Fetch or prepare a saved plan for the real check
	 */
	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		StringInfoData querybuf;
		char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		char		paramname[16];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];

		/* ----------
		 * The query string built is
		 *	SELECT 1 FROM ONLY <pktable> WHERE pkatt1 = $1 [AND ...] FOR SHARE
		 * The type id's for the $ parameters are those of the
		 * PK attributes themselves.
		 * ----------
		 */
		initStringInfo(&querybuf);
		quoteRelationName(pkrelname, pk_rel);
		appendStringInfo(&querybuf, "SELECT 1 FROM ONLY %s x", pkrelname);
		querysep = "WHERE";
		for (i = 0; i < riinfo->nkeys; i++)
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
		appendStringInfo(&querybuf, " FOR SHARE OF x");

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querybuf.data, riinfo->nkeys, queryoids,
							 &qkey, fk_rel, pk_rel, true);
	}

	/*
	 * We have a plan now. Run it.
	 */
	result = ri_PerformCheck(&qkey, qplan,
							 fk_rel, pk_rel,
							 old_row, NULL,
							 true,		/* treat like update */
							 SPI_OK_SELECT, NULL);

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	return result;
}


/* ----------
 * RI_FKey_noaction_del -
 *
 *	Give an error and roll back the current transaction if the
 *	delete has resulted in a violation of the given referential
 *	integrity constraint.
 * ----------
 */
Datum
RI_FKey_noaction_del(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_noaction_del", RI_TRIGTYPE_DELETE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR SHARE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	if (ri_Check_Pk_Match(pk_rel, fk_rel, old_row, &riinfo))
	{
		/*
		 * There's either another row, or no row could match this one.  In
		 * either case, we don't need to do the check.
		 */
		heap_close(fk_rel, RowShareLock);
		return PointerGetDatum(NULL);
	}

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iv):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_NOACTION_DEL_CHECKREF);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No check - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the restrict delete lookup if
			 * foreign references exist
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				StringInfoData querybuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	SELECT 1 FROM ONLY <fktable> WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "SELECT 1 FROM ONLY %s x",
								 fkrelname);
				querysep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&querybuf, querysep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					querysep = "AND";
					queryoids[i] = pk_type;
				}
				appendStringInfo(&querybuf, " FOR SHARE OF x");

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_SELECT,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowShareLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL restrict delete.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_noaction_upd -
 *
 *	Give an error and roll back the current transaction if the
 *	update has resulted in a violation of the given referential
 *	integrity constraint.
 * ----------
 */
Datum
RI_FKey_noaction_upd(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_noaction_upd", RI_TRIGTYPE_UPDATE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the new and
	 * old tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR SHARE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iv):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_NOACTION_UPD_CHECKREF);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No check - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			/*
			 * No need to check anything if old and new keys are equal
			 */
			if (ri_KeysEqual(pk_rel, old_row, new_row, &riinfo, true))
			{
				heap_close(fk_rel, RowShareLock);
				return PointerGetDatum(NULL);
			}

			if (ri_Check_Pk_Match(pk_rel, fk_rel, old_row, &riinfo))
			{
				/*
				 * There's either another row, or no row could match this one.
				 * In either case, we don't need to do the check.
				 */
				heap_close(fk_rel, RowShareLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the noaction update lookup if
			 * foreign references exist
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				StringInfoData querybuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	SELECT 1 FROM ONLY <fktable> WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "SELECT 1 FROM ONLY %s x",
								 fkrelname);
				querysep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&querybuf, querysep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					querysep = "AND";
					queryoids[i] = pk_type;
				}
				appendStringInfo(&querybuf, " FOR SHARE OF x");

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_SELECT,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowShareLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL noaction update.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_cascade_del -
 *
 *	Cascaded delete foreign key references at delete event on PK table.
 * ----------
 */
Datum
RI_FKey_cascade_del(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_cascade_del", RI_TRIGTYPE_DELETE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual DELETE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) i):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_CASCADE_DEL_DODELETE);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No check - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowExclusiveLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the cascaded delete
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				StringInfoData querybuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	DELETE FROM ONLY <fktable> WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "DELETE FROM ONLY %s", fkrelname);
				querysep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&querybuf, querysep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					querysep = "AND";
					queryoids[i] = pk_type;
				}

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Build up the arguments from the key values
			 * in the deleted PK tuple and delete the referencing rows
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_DELETE,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL cascaded delete.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_cascade_upd -
 *
 *	Cascaded update/delete foreign key references at update event on PK table.
 * ----------
 */
Datum
RI_FKey_cascade_upd(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;
	int			j;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_cascade_upd", RI_TRIGTYPE_UPDATE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the new and
	 * old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 7) a) i):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON UPDATE CASCADE
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_CASCADE_UPD_DOUPDATE);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No update - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowExclusiveLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			/*
			 * No need to do anything if old and new keys are equal
			 */
			if (ri_KeysEqual(pk_rel, old_row, new_row, &riinfo, true))
			{
				heap_close(fk_rel, RowExclusiveLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the cascaded update of
			 * foreign references
			 */
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

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = $1 [, ...]
				 *			WHERE $n = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.  Note that we are assuming
				 * there is an assignment cast from the PK to the FK type;
				 * else the parser will fail.
				 * ----------
				 */
				initStringInfo(&querybuf);
				initStringInfo(&qualbuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "UPDATE ONLY %s SET", fkrelname);
				querysep = "";
				qualsep = "WHERE";
				for (i = 0, j = riinfo.nkeys; i < riinfo.nkeys; i++, j++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));
					appendStringInfo(&querybuf,
									 "%s %s = $%d",
									 querysep, attname, i + 1);
					sprintf(paramname, "$%d", j + 1);
					ri_GenerateQual(&qualbuf, qualsep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					querysep = ",";
					qualsep = "AND";
					queryoids[i] = pk_type;
					queryoids[j] = pk_type;
				}
				appendStringInfoString(&querybuf, qualbuf.data);

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys * 2, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to update the existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, new_row,
							true,		/* must detect new rows */
							SPI_OK_UPDATE,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL cascade update.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_restrict_del -
 *
 *	Restrict delete from PK table to rows unreferenced by foreign key.
 *
 *	SQL3 intends that this referential action occur BEFORE the
 *	update is performed, rather than after.  This appears to be
 *	the only difference between "NO ACTION" and "RESTRICT".
 *
 *	For now, however, we treat "RESTRICT" and "NO ACTION" as
 *	equivalent.
 * ----------
 */
Datum
RI_FKey_restrict_del(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_restrict_del", RI_TRIGTYPE_DELETE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR SHARE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iv):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_RESTRICT_DEL_CHECKREF);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No check - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the restrict delete lookup if
			 * foreign references exist
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				StringInfoData querybuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	SELECT 1 FROM ONLY <fktable> WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "SELECT 1 FROM ONLY %s x",
								 fkrelname);
				querysep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&querybuf, querysep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					querysep = "AND";
					queryoids[i] = pk_type;
				}
				appendStringInfo(&querybuf, " FOR SHARE OF x");

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_SELECT,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowShareLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL restrict delete.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_restrict_upd -
 *
 *	Restrict update of PK to rows unreferenced by foreign key.
 *
 *	SQL3 intends that this referential action occur BEFORE the
 *	update is performed, rather than after.  This appears to be
 *	the only difference between "NO ACTION" and "RESTRICT".
 *
 *	For now, however, we treat "RESTRICT" and "NO ACTION" as
 *	equivalent.
 * ----------
 */
Datum
RI_FKey_restrict_upd(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_restrict_upd", RI_TRIGTYPE_UPDATE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the new and
	 * old tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR SHARE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iv):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_RESTRICT_UPD_CHECKREF);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No check - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			/*
			 * No need to check anything if old and new keys are equal
			 */
			if (ri_KeysEqual(pk_rel, old_row, new_row, &riinfo, true))
			{
				heap_close(fk_rel, RowShareLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the restrict update lookup if
			 * foreign references exist
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				StringInfoData querybuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	SELECT 1 FROM ONLY <fktable> WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "SELECT 1 FROM ONLY %s x",
								 fkrelname);
				querysep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&querybuf, querysep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					querysep = "AND";
					queryoids[i] = pk_type;
				}
				appendStringInfo(&querybuf, " FOR SHARE OF x");

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_SELECT,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowShareLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL restrict update.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_setnull_del -
 *
 *	Set foreign key references to NULL values at delete event on PK table.
 * ----------
 */
Datum
RI_FKey_setnull_del(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setnull_del", RI_TRIGTYPE_DELETE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) ii):
			 *		MATCH <UNSPECIFIED> or MATCH FULL
			 *			... ON DELETE SET NULL
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_SETNULL_DEL_DOUPDATE);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No update - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowExclusiveLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the set null delete operation
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				StringInfoData querybuf;
				StringInfoData qualbuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = NULL [, ...]
				 *			WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				initStringInfo(&qualbuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "UPDATE ONLY %s SET", fkrelname);
				querysep = "";
				qualsep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));
					appendStringInfo(&querybuf,
									 "%s %s = NULL",
									 querysep, attname);
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&qualbuf, qualsep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					querysep = ",";
					qualsep = "AND";
					queryoids[i] = pk_type;
				}
				appendStringInfoString(&querybuf, qualbuf.data);

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_UPDATE,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL set null delete.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_setnull_upd -
 *
 *	Set foreign key references to NULL at update event on PK table.
 * ----------
 */
Datum
RI_FKey_setnull_upd(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;
	int			i;
	bool		use_cached_query;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setnull_upd", RI_TRIGTYPE_UPDATE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 7) a) ii) 2):
			 *		MATCH FULL
			 *			... ON UPDATE SET NULL
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_SETNULL_UPD_DOUPDATE);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No update - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowExclusiveLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			/*
			 * No need to do anything if old and new keys are equal
			 */
			if (ri_KeysEqual(pk_rel, old_row, new_row, &riinfo, true))
			{
				heap_close(fk_rel, RowExclusiveLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * "MATCH <unspecified>" only changes columns corresponding to the
			 * referenced columns that have changed in pk_rel.	This means the
			 * "SET attrn=NULL [, attrn=NULL]" string will be change as well.
			 * In this case, we need to build a temporary plan rather than use
			 * our cached plan, unless the update happens to change all
			 * columns in the key.	Fortunately, for the most common case of a
			 * single-column foreign key, this will be true.
			 *
			 * In case you're wondering, the inequality check works because we
			 * know that the old key value has no NULLs (see above).
			 */

			use_cached_query = (riinfo.confmatchtype == FKCONSTR_MATCH_FULL) ||
				ri_AllKeysUnequal(pk_rel, old_row, new_row,
								  &riinfo, true);

			/*
			 * Fetch or prepare a saved plan for the set null update operation
			 * if possible, or build a temporary plan if not.
			 */
			if (!use_cached_query ||
				(qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				StringInfoData querybuf;
				StringInfoData qualbuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = NULL [, ...]
				 *			WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				initStringInfo(&qualbuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "UPDATE ONLY %s SET", fkrelname);
				querysep = "";
				qualsep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));

					/*
					 * MATCH <unspecified> - only change columns corresponding
					 * to changed columns in pk_rel's key
					 */
					if (riinfo.confmatchtype == FKCONSTR_MATCH_FULL ||
						!ri_OneKeyEqual(pk_rel, i, old_row, new_row,
										&riinfo, true))
					{
						appendStringInfo(&querybuf,
										 "%s %s = NULL",
										 querysep, attname);
						querysep = ",";
					}
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&qualbuf, qualsep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					qualsep = "AND";
					queryoids[i] = pk_type;
				}
				appendStringInfoString(&querybuf, qualbuf.data);

				/*
				 * Prepare the plan.  Save it only if we're building the
				 * "standard" plan.
				 */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel,
									 use_cached_query);
			}

			/*
			 * We have a plan now. Run it to update the existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_UPDATE,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL set null update.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_setdefault_del -
 *
 *	Set foreign key references to defaults at delete event on PK table.
 * ----------
 */
Datum
RI_FKey_setdefault_del(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setdefault_del", RI_TRIGTYPE_DELETE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iii):
			 *		MATCH <UNSPECIFIED> or MATCH FULL
			 *			... ON DELETE SET DEFAULT
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_SETNULL_DEL_DOUPDATE);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No update - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowExclusiveLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Prepare a plan for the set default delete operation.
			 * Unfortunately we need to do it on every invocation because the
			 * default value could potentially change between calls.
			 */
			{
				StringInfoData querybuf;
				StringInfoData qualbuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS];
				int			i;

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = DEFAULT [, ...]
				 *			WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				initStringInfo(&qualbuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "UPDATE ONLY %s SET", fkrelname);
				querysep = "";
				qualsep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));
					appendStringInfo(&querybuf,
									 "%s %s = DEFAULT",
									 querysep, attname);
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&qualbuf, qualsep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					querysep = ",";
					qualsep = "AND";
					queryoids[i] = pk_type;
				}
				appendStringInfoString(&querybuf, qualbuf.data);

				/* Prepare the plan, don't save it */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel, false);
			}

			/*
			 * We have a plan now. Run it to update the existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_UPDATE,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			/*
			 * In the case we delete the row who's key is equal to the default
			 * values AND a referencing row in the foreign key table exists,
			 * we would just have updated it to the same values. We need to do
			 * another lookup now and in case a reference exists, abort the
			 * operation. That is already implemented in the NO ACTION
			 * trigger.
			 */
			RI_FKey_noaction_del(fcinfo);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL set null delete.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_setdefault_upd -
 *
 *	Set foreign key references to defaults at update event on PK table.
 * ----------
 */
Datum
RI_FKey_setdefault_upd(PG_FUNCTION_ARGS)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	SPIPlanPtr	qplan;

	/*
	 * Check that this is a valid trigger call on the right time and event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setdefault_upd", RI_TRIGTYPE_UPDATE);

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo,
						   trigdata->tg_trigger, trigdata->tg_relation, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(riinfo.fk_relid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	switch (riinfo.confmatchtype)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 7) a) iii):
			 *		MATCH <UNSPECIFIED> or MATCH FULL
			 *			... ON UPDATE SET DEFAULT
			 * ----------
			 */
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_SETNULL_DEL_DOUPDATE);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:

					/*
					 * No update - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 */
					heap_close(fk_rel, RowExclusiveLock);
					return PointerGetDatum(NULL);

				case RI_KEYS_NONE_NULL:

					/*
					 * Have a full qualified key - continue below
					 */
					break;
			}

			/*
			 * No need to do anything if old and new keys are equal
			 */
			if (ri_KeysEqual(pk_rel, old_row, new_row, &riinfo, true))
			{
				heap_close(fk_rel, RowExclusiveLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Prepare a plan for the set default delete operation.
			 * Unfortunately we need to do it on every invocation because the
			 * default value could potentially change between calls.
			 */
			{
				StringInfoData querybuf;
				StringInfoData qualbuf;
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				char		paramname[16];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS];
				int			i;

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = DEFAULT [, ...]
				 *			WHERE $1 = fkatt1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes.
				 * ----------
				 */
				initStringInfo(&querybuf);
				initStringInfo(&qualbuf);
				quoteRelationName(fkrelname, fk_rel);
				appendStringInfo(&querybuf, "UPDATE ONLY %s SET", fkrelname);
				querysep = "";
				qualsep = "WHERE";
				for (i = 0; i < riinfo.nkeys; i++)
				{
					Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
					Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

					quoteOneName(attname,
								 RIAttName(fk_rel, riinfo.fk_attnums[i]));

					/*
					 * MATCH <unspecified> - only change columns corresponding
					 * to changed columns in pk_rel's key
					 */
					if (riinfo.confmatchtype == FKCONSTR_MATCH_FULL ||
						!ri_OneKeyEqual(pk_rel, i, old_row, new_row,
										&riinfo, true))
					{
						appendStringInfo(&querybuf,
										 "%s %s = DEFAULT",
										 querysep, attname);
						querysep = ",";
					}
					sprintf(paramname, "$%d", i + 1);
					ri_GenerateQual(&qualbuf, qualsep,
									paramname, pk_type,
									riinfo.pf_eq_oprs[i],
									attname, fk_type);
					qualsep = "AND";
					queryoids[i] = pk_type;
				}
				appendStringInfoString(&querybuf, qualbuf.data);

				/* Prepare the plan, don't save it */
				qplan = ri_PlanCheck(querybuf.data, riinfo.nkeys, queryoids,
									 &qkey, fk_rel, pk_rel, false);
			}

			/*
			 * We have a plan now. Run it to update the existing references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true,		/* must detect new rows */
							SPI_OK_UPDATE,
							NameStr(riinfo.conname));

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			/*
			 * In the case we updated the row who's key was equal to the
			 * default values AND a referencing row in the foreign key table
			 * exists, we would just have updated it to the same values. We
			 * need to do another lookup now and in case a reference exists,
			 * abort the operation. That is already implemented in the NO
			 * ACTION trigger.
			 */
			RI_FKey_noaction_upd(fcinfo);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL set null delete.
			 */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid confmatchtype");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_keyequal_upd_pk -
 *
 *	Check if we have a key change on an update to a PK relation. This is
 *	used by the AFTER trigger queue manager to see if it can skip queuing
 *	an instance of an RI trigger.
 * ----------
 */
bool
RI_FKey_keyequal_upd_pk(Trigger *trigger, Relation pk_rel,
						HeapTuple old_row, HeapTuple new_row)
{
	RI_ConstraintInfo riinfo;
	Relation	fk_rel;
	RI_QueryKey qkey;

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo, trigger, pk_rel, true);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return true;

	fk_rel = heap_open(riinfo.fk_relid, AccessShareLock);

	switch (riinfo.confmatchtype)
	{
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_KEYEQUAL_UPD);

			heap_close(fk_rel, AccessShareLock);

			/* Return if key's are equal */
			return ri_KeysEqual(pk_rel, old_row, new_row, &riinfo, true);

			/* Handle MATCH PARTIAL set null delete. */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			break;
	}

	/* Never reached */
	elog(ERROR, "invalid confmatchtype");
	return false;
}

/* ----------
 * RI_FKey_keyequal_upd_fk -
 *
 *	Check if we have a key change on an update to an FK relation. This is
 *	used by the AFTER trigger queue manager to see if it can skip queuing
 *	an instance of an RI trigger.
 * ----------
 */
bool
RI_FKey_keyequal_upd_fk(Trigger *trigger, Relation fk_rel,
						HeapTuple old_row, HeapTuple new_row)
{
	RI_ConstraintInfo riinfo;
	Relation	pk_rel;
	RI_QueryKey qkey;

	/*
	 * Get arguments.
	 */
	ri_FetchConstraintInfo(&riinfo, trigger, fk_rel, false);

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (riinfo.nkeys == 0)
		return true;

	pk_rel = heap_open(riinfo.pk_relid, AccessShareLock);

	switch (riinfo.confmatchtype)
	{
		case FKCONSTR_MATCH_UNSPECIFIED:
		case FKCONSTR_MATCH_FULL:
			ri_BuildQueryKeyFull(&qkey, &riinfo,
								 RI_PLAN_KEYEQUAL_UPD);
			heap_close(pk_rel, AccessShareLock);

			/* Return if key's are equal */
			return ri_KeysEqual(fk_rel, old_row, new_row, &riinfo, false);

			/* Handle MATCH PARTIAL set null delete. */
		case FKCONSTR_MATCH_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			break;
	}

	/* Never reached */
	elog(ERROR, "invalid confmatchtype");
	return false;
}

/* ----------
 * RI_Initial_Check -
 *
 *	Check an entire table for non-matching values using a single query.
 *	This is not a trigger procedure, but is called during ALTER TABLE
 *	ADD FOREIGN KEY to validate the initial table contents.
 *
 *	We expect that an exclusive lock has been taken on rel and pkrel;
 *	hence, we do not need to lock individual rows for the check.
 *
 *	If the check fails because the current user doesn't have permissions
 *	to read both tables, return false to let our caller know that they will
 *	need to do something else to check the constraint.
 * ----------
 */
bool
RI_Initial_Check(Trigger *trigger, Relation fk_rel, Relation pk_rel)
{
	RI_ConstraintInfo riinfo;
	const char *constrname = trigger->tgname;
	StringInfoData querybuf;
	char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		pkattname[MAX_QUOTED_NAME_LEN + 3];
	char		fkattname[MAX_QUOTED_NAME_LEN + 3];
	const char *sep;
	int			i;
	int			old_work_mem;
	char		workmembuf[32];
	int			spi_result;
	SPIPlanPtr	qplan;

	/*
	 * Check to make sure current user has enough permissions to do the test
	 * query.  (If not, caller can fall back to the trigger method, which
	 * works because it changes user IDs on the fly.)
	 *
	 * XXX are there any other show-stopper conditions to check?
	 */
	if (pg_class_aclcheck(RelationGetRelid(fk_rel), GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		return false;
	if (pg_class_aclcheck(RelationGetRelid(pk_rel), GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		return false;

	ri_FetchConstraintInfo(&riinfo, trigger, fk_rel, false);

	/*----------
	 * The query string built is:
	 *	SELECT fk.keycols FROM ONLY relname fk
	 *	 LEFT OUTER JOIN ONLY pkrelname pk
	 *	 ON (pk.pkkeycol1=fk.keycol1 [AND ...])
	 *	 WHERE pk.pkkeycol1 IS NULL AND
	 * For MATCH unspecified:
	 *	 (fk.keycol1 IS NOT NULL [AND ...])
	 * For MATCH FULL:
	 *	 (fk.keycol1 IS NOT NULL [OR ...])
	 *----------
	 */
	initStringInfo(&querybuf);
	appendStringInfo(&querybuf, "SELECT ");
	sep = "";
	for (i = 0; i < riinfo.nkeys; i++)
	{
		quoteOneName(fkattname,
					 RIAttName(fk_rel, riinfo.fk_attnums[i]));
		appendStringInfo(&querybuf, "%sfk.%s", sep, fkattname);
		sep = ", ";
	}

	quoteRelationName(pkrelname, pk_rel);
	quoteRelationName(fkrelname, fk_rel);
	appendStringInfo(&querybuf,
					 " FROM ONLY %s fk LEFT OUTER JOIN ONLY %s pk ON",
					 fkrelname, pkrelname);

	strcpy(pkattname, "pk.");
	strcpy(fkattname, "fk.");
	sep = "(";
	for (i = 0; i < riinfo.nkeys; i++)
	{
		Oid			pk_type = RIAttType(pk_rel, riinfo.pk_attnums[i]);
		Oid			fk_type = RIAttType(fk_rel, riinfo.fk_attnums[i]);

		quoteOneName(pkattname + 3,
					 RIAttName(pk_rel, riinfo.pk_attnums[i]));
		quoteOneName(fkattname + 3,
					 RIAttName(fk_rel, riinfo.fk_attnums[i]));
		ri_GenerateQual(&querybuf, sep,
						pkattname, pk_type,
						riinfo.pf_eq_oprs[i],
						fkattname, fk_type);
		sep = "AND";
	}

	/*
	 * It's sufficient to test any one pk attribute for null to detect a join
	 * failure.
	 */
	quoteOneName(pkattname, RIAttName(pk_rel, riinfo.pk_attnums[0]));
	appendStringInfo(&querybuf, ") WHERE pk.%s IS NULL AND (", pkattname);

	sep = "";
	for (i = 0; i < riinfo.nkeys; i++)
	{
		quoteOneName(fkattname, RIAttName(fk_rel, riinfo.fk_attnums[i]));
		appendStringInfo(&querybuf,
						 "%sfk.%s IS NOT NULL",
						 sep, fkattname);
		switch (riinfo.confmatchtype)
		{
			case FKCONSTR_MATCH_UNSPECIFIED:
				sep = " AND ";
				break;
			case FKCONSTR_MATCH_FULL:
				sep = " OR ";
				break;
			case FKCONSTR_MATCH_PARTIAL:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("MATCH PARTIAL not yet implemented")));
				break;
			default:
				elog(ERROR, "unrecognized match type: %d",
					 riinfo.confmatchtype);
				break;
		}
	}
	appendStringInfo(&querybuf, ")");

	/*
	 * Temporarily increase work_mem so that the check query can be executed
	 * more efficiently.  It seems okay to do this because the query is simple
	 * enough to not use a multiple of work_mem, and one typically would not
	 * have many large foreign-key validations happening concurrently.	So
	 * this seems to meet the criteria for being considered a "maintenance"
	 * operation, and accordingly we use maintenance_work_mem.
	 *
	 * We do the equivalent of "SET LOCAL work_mem" so that transaction abort
	 * will restore the old value if we lose control due to an error.
	 */
	old_work_mem = work_mem;
	snprintf(workmembuf, sizeof(workmembuf), "%d", maintenance_work_mem);
	(void) set_config_option("work_mem", workmembuf,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_LOCAL, true);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Generate the plan.  We don't need to cache it, and there are no
	 * arguments to the plan.
	 */
	qplan = SPI_prepare(querybuf.data, 0, NULL);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %d for %s",
			 SPI_result, querybuf.data);

	/*
	 * Run the plan.  For safety we force a current snapshot to be used. (In
	 * serializable mode, this arguably violates serializability, but we
	 * really haven't got much choice.)  We need at most one tuple returned,
	 * so pass limit = 1.
	 */
	spi_result = SPI_execute_snapshot(qplan,
									  NULL, NULL,
									  CopySnapshot(GetLatestSnapshot()),
									  InvalidSnapshot,
									  true, false, 1);

	/* Check result */
	if (spi_result != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute_snapshot returned %d", spi_result);

	/* Did we find a tuple violating the constraint? */
	if (SPI_processed > 0)
	{
		HeapTuple	tuple = SPI_tuptable->vals[0];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		RI_QueryKey qkey;

		/*
		 * If it's MATCH FULL, and there are any nulls in the FK keys,
		 * complain about that rather than the lack of a match.  MATCH FULL
		 * disallows partially-null FK rows.
		 */
		if (riinfo.confmatchtype == FKCONSTR_MATCH_FULL)
		{
			bool		isnull = false;

			for (i = 1; i <= riinfo.nkeys; i++)
			{
				(void) SPI_getbinval(tuple, tupdesc, i, &isnull);
				if (isnull)
					break;
			}
			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
						 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
								RelationGetRelationName(fk_rel),
								constrname),
						 errdetail("MATCH FULL does not allow mixing of null and nonnull key values.")));
		}

		/*
		 * Although we didn't cache the query, we need to set up a fake query
		 * key to pass to ri_ReportViolation.
		 */
		MemSet(&qkey, 0, sizeof(qkey));
		qkey.constr_queryno = RI_PLAN_CHECK_LOOKUPPK;
		qkey.nkeypairs = riinfo.nkeys;
		for (i = 0; i < riinfo.nkeys; i++)
			qkey.keypair[i][RI_KEYPAIR_FK_IDX] = i + 1;

		ri_ReportViolation(&qkey, constrname,
						   pk_rel, fk_rel,
						   tuple, tupdesc,
						   false);
	}

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

	/*
	 * Restore work_mem for the remainder of the current transaction. This is
	 * another SET LOCAL, so it won't affect the session value.
	 */
	snprintf(workmembuf, sizeof(workmembuf), "%d", old_work_mem);
	(void) set_config_option("work_mem", workmembuf,
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_LOCAL, true);

	return true;
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
 * The idea is to append " sep leftop op rightop" to buf.  The complexity
 * comes from needing to be sure that the parser will select the desired
 * operator.  We always name the operator using OPERATOR(schema.op) syntax
 * (readability isn't a big priority here).  We have to emit casts too,
 * if either input isn't already the input type of the operator.
 */
static void
ri_GenerateQual(StringInfo buf,
				const char *sep,
				const char *leftop, Oid leftoptype,
				Oid opoid,
				const char *rightop, Oid rightoptype)
{
	HeapTuple	opertup;
	Form_pg_operator operform;
	char	   *oprname;
	char	   *nspname;

	opertup = SearchSysCache(OPEROID,
							 ObjectIdGetDatum(opoid),
							 0, 0, 0);
	if (!HeapTupleIsValid(opertup))
		elog(ERROR, "cache lookup failed for operator %u", opoid);
	operform = (Form_pg_operator) GETSTRUCT(opertup);
	Assert(operform->oprkind == 'b');
	oprname = NameStr(operform->oprname);

	nspname = get_namespace_name(operform->oprnamespace);

	appendStringInfo(buf, " %s %s", sep, leftop);
	if (leftoptype != operform->oprleft)
		appendStringInfo(buf, "::%s", format_type_be(operform->oprleft));
	appendStringInfo(buf, " OPERATOR(%s.", quote_identifier(nspname));
	appendStringInfoString(buf, oprname);
	appendStringInfo(buf, ") %s", rightop);
	if (rightoptype != operform->oprright)
		appendStringInfo(buf, "::%s", format_type_be(operform->oprright));

	ReleaseSysCache(opertup);
}

/* ----------
 * ri_BuildQueryKeyFull -
 *
 *	Build up a new hashtable key for a prepared SPI plan of a
 *	constraint trigger of MATCH FULL.
 *
 *		key: output argument, *key is filled in based on the other arguments
 *		riinfo: info from pg_constraint entry
 *		constr_queryno: an internal number of the query inside the proc
 *
 *	At least for MATCH FULL this builds a unique key per plan.
 * ----------
 */
static void
ri_BuildQueryKeyFull(RI_QueryKey *key, const RI_ConstraintInfo *riinfo,
					 int32 constr_queryno)
{
	int			i;

	MemSet(key, 0, sizeof(RI_QueryKey));
	key->constr_type = FKCONSTR_MATCH_FULL;
	key->constr_id = riinfo->constraint_id;
	key->constr_queryno = constr_queryno;
	key->fk_relid = riinfo->fk_relid;
	key->pk_relid = riinfo->pk_relid;
	key->nkeypairs = riinfo->nkeys;
	for (i = 0; i < riinfo->nkeys; i++)
	{
		key->keypair[i][RI_KEYPAIR_FK_IDX] = riinfo->fk_attnums[i];
		key->keypair[i][RI_KEYPAIR_PK_IDX] = riinfo->pk_attnums[i];
	}
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
		case RI_TRIGTYPE_INUP:
			if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) &&
				!TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
				ereport(ERROR,
						(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired for INSERT or UPDATE",
						funcname)));
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
 * Fetch the pg_constraint entry for the FK constraint, and fill *riinfo
 */
static void
ri_FetchConstraintInfo(RI_ConstraintInfo *riinfo,
					   Trigger *trigger, Relation trig_rel, bool rel_is_pk)
{
	Oid			constraintOid = trigger->tgconstraint;
	HeapTuple	tup;
	Form_pg_constraint conForm;
	Datum		adatum;
	bool		isNull;
	ArrayType  *arr;
	int			numkeys;

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

	/* OK, fetch the tuple */
	tup = SearchSysCache(CONSTROID,
						 ObjectIdGetDatum(constraintOid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for constraint %u", constraintOid);
	conForm = (Form_pg_constraint) GETSTRUCT(tup);

	/* Do some easy cross-checks against the trigger call data */
	if (rel_is_pk)
	{
		if (conForm->contype != CONSTRAINT_FOREIGN ||
			conForm->conrelid != trigger->tgconstrrelid ||
			conForm->confrelid != RelationGetRelid(trig_rel))
			elog(ERROR, "wrong pg_constraint entry for trigger \"%s\" on table \"%s\"",
				 trigger->tgname, RelationGetRelationName(trig_rel));
	}
	else
	{
		if (conForm->contype != CONSTRAINT_FOREIGN ||
			conForm->conrelid != RelationGetRelid(trig_rel) ||
			conForm->confrelid != trigger->tgconstrrelid)
			elog(ERROR, "wrong pg_constraint entry for trigger \"%s\" on table \"%s\"",
				 trigger->tgname, RelationGetRelationName(trig_rel));
	}

	/* And extract data */
	riinfo->constraint_id = constraintOid;
	memcpy(&riinfo->conname, &conForm->conname, sizeof(NameData));
	riinfo->pk_relid = conForm->confrelid;
	riinfo->fk_relid = conForm->conrelid;
	riinfo->confupdtype = conForm->confupdtype;
	riinfo->confdeltype = conForm->confdeltype;
	riinfo->confmatchtype = conForm->confmatchtype;

	/*
	 * We expect the arrays to be 1-D arrays of the right types; verify that.
	 * We don't need to use deconstruct_array() since the array data is just
	 * going to look like a C array of values.
	 */
	adatum = SysCacheGetAttr(CONSTROID, tup,
							 Anum_pg_constraint_conkey, &isNull);
	if (isNull)
		elog(ERROR, "null conkey for constraint %u", constraintOid);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	numkeys = ARR_DIMS(arr)[0];
	if (ARR_NDIM(arr) != 1 ||
		numkeys < 0 ||
		numkeys > RI_MAX_NUMKEYS ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != INT2OID)
		elog(ERROR, "conkey is not a 1-D smallint array");
	riinfo->nkeys = numkeys;
	memcpy(riinfo->fk_attnums, ARR_DATA_PTR(arr), numkeys * sizeof(int16));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* free de-toasted copy, if any */

	adatum = SysCacheGetAttr(CONSTROID, tup,
							 Anum_pg_constraint_confkey, &isNull);
	if (isNull)
		elog(ERROR, "null confkey for constraint %u", constraintOid);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	numkeys = ARR_DIMS(arr)[0];
	if (ARR_NDIM(arr) != 1 ||
		numkeys != riinfo->nkeys ||
		numkeys > RI_MAX_NUMKEYS ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != INT2OID)
		elog(ERROR, "confkey is not a 1-D smallint array");
	memcpy(riinfo->pk_attnums, ARR_DATA_PTR(arr), numkeys * sizeof(int16));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* free de-toasted copy, if any */

	adatum = SysCacheGetAttr(CONSTROID, tup,
							 Anum_pg_constraint_conpfeqop, &isNull);
	if (isNull)
		elog(ERROR, "null conpfeqop for constraint %u", constraintOid);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	numkeys = ARR_DIMS(arr)[0];
	if (ARR_NDIM(arr) != 1 ||
		numkeys != riinfo->nkeys ||
		numkeys > RI_MAX_NUMKEYS ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "conpfeqop is not a 1-D Oid array");
	memcpy(riinfo->pf_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* free de-toasted copy, if any */

	adatum = SysCacheGetAttr(CONSTROID, tup,
							 Anum_pg_constraint_conppeqop, &isNull);
	if (isNull)
		elog(ERROR, "null conppeqop for constraint %u", constraintOid);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	numkeys = ARR_DIMS(arr)[0];
	if (ARR_NDIM(arr) != 1 ||
		numkeys != riinfo->nkeys ||
		numkeys > RI_MAX_NUMKEYS ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "conppeqop is not a 1-D Oid array");
	memcpy(riinfo->pp_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* free de-toasted copy, if any */

	adatum = SysCacheGetAttr(CONSTROID, tup,
							 Anum_pg_constraint_conffeqop, &isNull);
	if (isNull)
		elog(ERROR, "null conffeqop for constraint %u", constraintOid);
	arr = DatumGetArrayTypeP(adatum);	/* ensure not toasted */
	numkeys = ARR_DIMS(arr)[0];
	if (ARR_NDIM(arr) != 1 ||
		numkeys != riinfo->nkeys ||
		numkeys > RI_MAX_NUMKEYS ||
		ARR_HASNULL(arr) ||
		ARR_ELEMTYPE(arr) != OIDOID)
		elog(ERROR, "conffeqop is not a 1-D Oid array");
	memcpy(riinfo->ff_eq_oprs, ARR_DATA_PTR(arr), numkeys * sizeof(Oid));
	if ((Pointer) arr != DatumGetPointer(adatum))
		pfree(arr);				/* free de-toasted copy, if any */

	ReleaseSysCache(tup);
}


/*
 * Prepare execution plan for a query to enforce an RI restriction
 *
 * If cache_plan is true, the plan is saved into our plan hashtable
 * so that we don't need to plan it again.
 */
static SPIPlanPtr
ri_PlanCheck(const char *querystr, int nargs, Oid *argtypes,
			 RI_QueryKey *qkey, Relation fk_rel, Relation pk_rel,
			 bool cache_plan)
{
	SPIPlanPtr	qplan;
	Relation	query_rel;
	Oid			save_userid;
	bool		save_secdefcxt;

	/*
	 * The query is always run against the FK table except when this is an
	 * update/insert trigger on the FK table itself - either
	 * RI_PLAN_CHECK_LOOKUPPK or RI_PLAN_CHECK_LOOKUPPK_NOCOLS
	 */
	if (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK ||
		qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK_NOCOLS)
		query_rel = pk_rel;
	else
		query_rel = fk_rel;

	/* Switch to proper UID to perform check as */
	GetUserIdAndContext(&save_userid, &save_secdefcxt);
	SetUserIdAndContext(RelationGetForm(query_rel)->relowner, true);

	/* Create the plan */
	qplan = SPI_prepare(querystr, nargs, argtypes);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %d for %s", SPI_result, querystr);

	/* Restore UID */
	SetUserIdAndContext(save_userid, save_secdefcxt);

	/* Save the plan if requested */
	if (cache_plan)
	{
		qplan = SPI_saveplan(qplan);
		ri_HashPreparedPlan(qkey, qplan);
	}

	return qplan;
}

/*
 * Perform a query to enforce an RI restriction
 */
static bool
ri_PerformCheck(RI_QueryKey *qkey, SPIPlanPtr qplan,
				Relation fk_rel, Relation pk_rel,
				HeapTuple old_tuple, HeapTuple new_tuple,
				bool detectNewRows,
				int expect_OK, const char *constrname)
{
	Relation	query_rel,
				source_rel;
	int			key_idx;
	Snapshot	test_snapshot;
	Snapshot	crosscheck_snapshot;
	int			limit;
	int			spi_result;
	Oid			save_userid;
	bool		save_secdefcxt;
	Datum		vals[RI_MAX_NUMKEYS * 2];
	char		nulls[RI_MAX_NUMKEYS * 2];

	/*
	 * The query is always run against the FK table except when this is an
	 * update/insert trigger on the FK table itself - either
	 * RI_PLAN_CHECK_LOOKUPPK or RI_PLAN_CHECK_LOOKUPPK_NOCOLS
	 */
	if (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK ||
		qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK_NOCOLS)
		query_rel = pk_rel;
	else
		query_rel = fk_rel;

	/*
	 * The values for the query are taken from the table on which the trigger
	 * is called - it is normally the other one with respect to query_rel. An
	 * exception is ri_Check_Pk_Match(), which uses the PK table for both (the
	 * case when constrname == NULL)
	 */
	if (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK && constrname != NULL)
	{
		source_rel = fk_rel;
		key_idx = RI_KEYPAIR_FK_IDX;
	}
	else
	{
		source_rel = pk_rel;
		key_idx = RI_KEYPAIR_PK_IDX;
	}

	/* Extract the parameters to be passed into the query */
	if (new_tuple)
	{
		ri_ExtractValues(qkey, key_idx, source_rel, new_tuple,
						 vals, nulls);
		if (old_tuple)
			ri_ExtractValues(qkey, key_idx, source_rel, old_tuple,
							 vals + qkey->nkeypairs, nulls + qkey->nkeypairs);
	}
	else
	{
		ri_ExtractValues(qkey, key_idx, source_rel, old_tuple,
						 vals, nulls);
	}

	/*
	 * In READ COMMITTED mode, we just need to use an up-to-date regular
	 * snapshot, and we will see all rows that could be interesting. But in
	 * SERIALIZABLE mode, we can't change the transaction snapshot. If the
	 * caller passes detectNewRows == false then it's okay to do the query
	 * with the transaction snapshot; otherwise we use a current snapshot, and
	 * tell the executor to error out if it finds any rows under the current
	 * snapshot that wouldn't be visible per the transaction snapshot.
	 */
	if (IsXactIsoLevelSerializable && detectNewRows)
	{
		CommandCounterIncrement();		/* be sure all my own work is visible */
		test_snapshot = CopySnapshot(GetLatestSnapshot());
		crosscheck_snapshot = CopySnapshot(GetTransactionSnapshot());
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
	GetUserIdAndContext(&save_userid, &save_secdefcxt);
	SetUserIdAndContext(RelationGetForm(query_rel)->relowner, true);

	/* Finally we can run the query. */
	spi_result = SPI_execute_snapshot(qplan,
									  vals, nulls,
									  test_snapshot, crosscheck_snapshot,
									  false, false, limit);

	/* Restore UID */
	SetUserIdAndContext(save_userid, save_secdefcxt);

	/* Check result */
	if (spi_result < 0)
		elog(ERROR, "SPI_execute_snapshot returned %d", spi_result);

	if (expect_OK >= 0 && spi_result != expect_OK)
		ri_ReportViolation(qkey, constrname ? constrname : "",
						   pk_rel, fk_rel,
						   new_tuple ? new_tuple : old_tuple,
						   NULL,
						   true);

	/* XXX wouldn't it be clearer to do this part at the caller? */
	if (constrname && expect_OK == SPI_OK_SELECT &&
	(SPI_processed == 0) == (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK))
		ri_ReportViolation(qkey, constrname,
						   pk_rel, fk_rel,
						   new_tuple ? new_tuple : old_tuple,
						   NULL,
						   false);

	return SPI_processed != 0;
}

/*
 * Extract fields from a tuple into Datum/nulls arrays
 */
static void
ri_ExtractValues(RI_QueryKey *qkey, int key_idx,
				 Relation rel, HeapTuple tuple,
				 Datum *vals, char *nulls)
{
	int			i;
	bool		isnull;

	for (i = 0; i < qkey->nkeypairs; i++)
	{
		vals[i] = SPI_getbinval(tuple, rel->rd_att,
								qkey->keypair[i][key_idx],
								&isnull);
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
ri_ReportViolation(RI_QueryKey *qkey, const char *constrname,
				   Relation pk_rel, Relation fk_rel,
				   HeapTuple violator, TupleDesc tupdesc,
				   bool spi_err)
{
#define BUFLENGTH	512
	char		key_names[BUFLENGTH];
	char		key_values[BUFLENGTH];
	char	   *name_ptr = key_names;
	char	   *val_ptr = key_values;
	bool		onfk;
	int			idx,
				key_idx;

	if (spi_err)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("referential integrity query on \"%s\" from constraint \"%s\" on \"%s\" gave unexpected result",
						RelationGetRelationName(pk_rel),
						constrname,
						RelationGetRelationName(fk_rel)),
				 errhint("This is most likely due to a rule having rewritten the query.")));

	/*
	 * Determine which relation to complain about.	If tupdesc wasn't passed
	 * by caller, assume the violator tuple came from there.
	 */
	onfk = (qkey->constr_queryno == RI_PLAN_CHECK_LOOKUPPK);
	if (onfk)
	{
		key_idx = RI_KEYPAIR_FK_IDX;
		if (tupdesc == NULL)
			tupdesc = fk_rel->rd_att;
	}
	else
	{
		key_idx = RI_KEYPAIR_PK_IDX;
		if (tupdesc == NULL)
			tupdesc = pk_rel->rd_att;
	}

	/*
	 * Special case - if there are no keys at all, this is a 'no column'
	 * constraint - no need to try to extract the values, and the message in
	 * this case looks different.
	 */
	if (qkey->nkeypairs == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
						RelationGetRelationName(fk_rel), constrname),
				 errdetail("No rows were found in \"%s\".",
						   RelationGetRelationName(pk_rel))));
	}

	/* Get printable versions of the keys involved */
	for (idx = 0; idx < qkey->nkeypairs; idx++)
	{
		int			fnum = qkey->keypair[idx][key_idx];
		char	   *name,
				   *val;

		name = SPI_fname(tupdesc, fnum);
		val = SPI_getvalue(violator, tupdesc, fnum);
		if (!val)
			val = "null";

		/*
		 * Go to "..." if name or value doesn't fit in buffer.  We reserve 5
		 * bytes to ensure we can add comma, "...", null.
		 */
		if (strlen(name) >= (key_names + BUFLENGTH - 5) - name_ptr ||
			strlen(val) >= (key_values + BUFLENGTH - 5) - val_ptr)
		{
			sprintf(name_ptr, "...");
			sprintf(val_ptr, "...");
			break;
		}

		name_ptr += sprintf(name_ptr, "%s%s", idx > 0 ? "," : "", name);
		val_ptr += sprintf(val_ptr, "%s%s", idx > 0 ? "," : "", val);
	}

	if (onfk)
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
						RelationGetRelationName(fk_rel), constrname),
				 errdetail("Key (%s)=(%s) is not present in table \"%s\".",
						   key_names, key_values,
						   RelationGetRelationName(pk_rel))));
	else
		ereport(ERROR,
				(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
				 errmsg("update or delete on table \"%s\" violates foreign key constraint \"%s\" on table \"%s\"",
						RelationGetRelationName(pk_rel),
						constrname, RelationGetRelationName(fk_rel)),
			errdetail("Key (%s)=(%s) is still referenced from table \"%s\".",
					  key_names, key_values,
					  RelationGetRelationName(fk_rel))));
}

/* ----------
 * ri_BuildQueryKeyPkCheck -
 *
 *	Build up a new hashtable key for a prepared SPI plan of a
 *	check for PK rows in noaction triggers.
 *
 *		key: output argument, *key is filled in based on the other arguments
 *		riinfo: info from pg_constraint entry
 *		constr_queryno: an internal number of the query inside the proc
 *
 *	At least for MATCH FULL this builds a unique key per plan.
 * ----------
 */
static void
ri_BuildQueryKeyPkCheck(RI_QueryKey *key, const RI_ConstraintInfo *riinfo,
						int32 constr_queryno)
{
	int			i;

	MemSet(key, 0, sizeof(RI_QueryKey));
	key->constr_type = FKCONSTR_MATCH_FULL;
	key->constr_id = riinfo->constraint_id;
	key->constr_queryno = constr_queryno;
	key->fk_relid = InvalidOid;
	key->pk_relid = riinfo->pk_relid;
	key->nkeypairs = riinfo->nkeys;
	for (i = 0; i < riinfo->nkeys; i++)
	{
		key->keypair[i][RI_KEYPAIR_FK_IDX] = 0;
		key->keypair[i][RI_KEYPAIR_PK_IDX] = riinfo->pk_attnums[i];
	}
}


/* ----------
 * ri_NullCheck -
 *
 *	Determine the NULL state of all key values in a tuple
 *
 *	Returns one of RI_KEYS_ALL_NULL, RI_KEYS_NONE_NULL or RI_KEYS_SOME_NULL.
 * ----------
 */
static int
ri_NullCheck(Relation rel, HeapTuple tup, RI_QueryKey *key, int pairidx)
{
	int			i;
	bool		isnull;
	bool		allnull = true;
	bool		nonenull = true;

	for (i = 0; i < key->nkeypairs; i++)
	{
		isnull = false;
		SPI_getbinval(tup, rel->rd_att, key->keypair[i][pairidx], &isnull);
		if (isnull)
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


/* ----------
 * ri_InitHashTables -
 *
 *	Initialize our internal hash tables for prepared
 *	query plans and comparison operators.
 * ----------
 */
static void
ri_InitHashTables(void)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(RI_QueryKey);
	ctl.entrysize = sizeof(RI_QueryHashEntry);
	ctl.hash = tag_hash;
	ri_query_cache = hash_create("RI query cache", RI_INIT_QUERYHASHSIZE,
								 &ctl, HASH_ELEM | HASH_FUNCTION);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(RI_CompareKey);
	ctl.entrysize = sizeof(RI_CompareHashEntry);
	ctl.hash = tag_hash;
	ri_compare_cache = hash_create("RI compare cache", RI_INIT_QUERYHASHSIZE,
								   &ctl, HASH_ELEM | HASH_FUNCTION);
}


/* ----------
 * ri_FetchPreparedPlan -
 *
 *	Lookup for a query key in our private hash table of prepared
 *	and saved SPI execution plans. Return the plan if found or NULL.
 * ----------
 */
static SPIPlanPtr
ri_FetchPreparedPlan(RI_QueryKey *key)
{
	RI_QueryHashEntry *entry;

	/*
	 * On the first call initialize the hashtable
	 */
	if (!ri_query_cache)
		ri_InitHashTables();

	/*
	 * Lookup for the key
	 */
	entry = (RI_QueryHashEntry *) hash_search(ri_query_cache,
											  (void *) key,
											  HASH_FIND, NULL);
	if (entry == NULL)
		return NULL;
	return entry->plan;
}


/* ----------
 * ri_HashPreparedPlan -
 *
 *	Add another plan to our private SPI query plan hashtable.
 * ----------
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
	 * Add the new plan.
	 */
	entry = (RI_QueryHashEntry *) hash_search(ri_query_cache,
											  (void *) key,
											  HASH_ENTER, &found);
	entry->plan = plan;
}


/* ----------
 * ri_KeysEqual -
 *
 *	Check if all key values in OLD and NEW are equal.
 * ----------
 */
static bool
ri_KeysEqual(Relation rel, HeapTuple oldtup, HeapTuple newtup,
			 const RI_ConstraintInfo *riinfo, bool rel_is_pk)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	const int16 *attnums;
	const Oid  *eq_oprs;
	int			i;

	if (rel_is_pk)
	{
		attnums = riinfo->pk_attnums;
		eq_oprs = riinfo->pp_eq_oprs;
	}
	else
	{
		attnums = riinfo->fk_attnums;
		eq_oprs = riinfo->ff_eq_oprs;
	}

	for (i = 0; i < riinfo->nkeys; i++)
	{
		Datum		oldvalue;
		Datum		newvalue;
		bool		isnull;

		/*
		 * Get one attribute's oldvalue. If it is NULL - they're not equal.
		 */
		oldvalue = SPI_getbinval(oldtup, tupdesc, attnums[i], &isnull);
		if (isnull)
			return false;

		/*
		 * Get one attribute's newvalue. If it is NULL - they're not equal.
		 */
		newvalue = SPI_getbinval(newtup, tupdesc, attnums[i], &isnull);
		if (isnull)
			return false;

		/*
		 * Compare them with the appropriate equality operator.
		 */
		if (!ri_AttributesEqual(eq_oprs[i], RIAttType(rel, attnums[i]),
								oldvalue, newvalue))
			return false;
	}

	return true;
}


/* ----------
 * ri_AllKeysUnequal -
 *
 *	Check if all key values in OLD and NEW are not equal.
 * ----------
 */
static bool
ri_AllKeysUnequal(Relation rel, HeapTuple oldtup, HeapTuple newtup,
				  const RI_ConstraintInfo *riinfo, bool rel_is_pk)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	const int16 *attnums;
	const Oid  *eq_oprs;
	int			i;

	if (rel_is_pk)
	{
		attnums = riinfo->pk_attnums;
		eq_oprs = riinfo->pp_eq_oprs;
	}
	else
	{
		attnums = riinfo->fk_attnums;
		eq_oprs = riinfo->ff_eq_oprs;
	}

	for (i = 0; i < riinfo->nkeys; i++)
	{
		Datum		oldvalue;
		Datum		newvalue;
		bool		isnull;

		/*
		 * Get one attribute's oldvalue. If it is NULL - they're not equal.
		 */
		oldvalue = SPI_getbinval(oldtup, tupdesc, attnums[i], &isnull);
		if (isnull)
			continue;

		/*
		 * Get one attribute's newvalue. If it is NULL - they're not equal.
		 */
		newvalue = SPI_getbinval(newtup, tupdesc, attnums[i], &isnull);
		if (isnull)
			continue;

		/*
		 * Compare them with the appropriate equality operator.
		 */
		if (ri_AttributesEqual(eq_oprs[i], RIAttType(rel, attnums[i]),
							   oldvalue, newvalue))
			return false;		/* found two equal items */
	}

	return true;
}


/* ----------
 * ri_OneKeyEqual -
 *
 *	Check if one key value in OLD and NEW is equal.  Note column is indexed
 *	from zero.
 *
 *	ri_KeysEqual could call this but would run a bit slower.  For
 *	now, let's duplicate the code.
 * ----------
 */
static bool
ri_OneKeyEqual(Relation rel, int column, HeapTuple oldtup, HeapTuple newtup,
			   const RI_ConstraintInfo *riinfo, bool rel_is_pk)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	const int16 *attnums;
	const Oid  *eq_oprs;
	Datum		oldvalue;
	Datum		newvalue;
	bool		isnull;

	if (rel_is_pk)
	{
		attnums = riinfo->pk_attnums;
		eq_oprs = riinfo->pp_eq_oprs;
	}
	else
	{
		attnums = riinfo->fk_attnums;
		eq_oprs = riinfo->ff_eq_oprs;
	}

	/*
	 * Get one attribute's oldvalue. If it is NULL - they're not equal.
	 */
	oldvalue = SPI_getbinval(oldtup, tupdesc, attnums[column], &isnull);
	if (isnull)
		return false;

	/*
	 * Get one attribute's newvalue. If it is NULL - they're not equal.
	 */
	newvalue = SPI_getbinval(newtup, tupdesc, attnums[column], &isnull);
	if (isnull)
		return false;

	/*
	 * Compare them with the appropriate equality operator.
	 */
	if (!ri_AttributesEqual(eq_oprs[column], RIAttType(rel, attnums[column]),
							oldvalue, newvalue))
		return false;

	return true;
}

/* ----------
 * ri_AttributesEqual -
 *
 *	Call the appropriate equality comparison operator for two values.
 *
 *	NB: we have already checked that neither value is null.
 * ----------
 */
static bool
ri_AttributesEqual(Oid eq_opr, Oid typeid,
				   Datum oldvalue, Datum newvalue)
{
	RI_CompareHashEntry *entry = ri_HashCompareOp(eq_opr, typeid);

	/* Do we need to cast the values? */
	if (OidIsValid(entry->cast_func_finfo.fn_oid))
	{
		oldvalue = FunctionCall3(&entry->cast_func_finfo,
								 oldvalue,
								 Int32GetDatum(-1),		/* typmod */
								 BoolGetDatum(false));	/* implicit coercion */
		newvalue = FunctionCall3(&entry->cast_func_finfo,
								 newvalue,
								 Int32GetDatum(-1),		/* typmod */
								 BoolGetDatum(false));	/* implicit coercion */
	}

	/* Apply the comparison operator */
	return DatumGetBool(FunctionCall2(&entry->eq_opr_finfo,
									  oldvalue, newvalue));
}

/* ----------
 * ri_HashCompareOp -
 *
 *	See if we know how to compare two values, and create a new hash entry
 *	if not.
 * ----------
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
												(void *) &key,
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
		 * here and in ri_AttributesEqual().  At the moment there is no point
		 * because cases involving nonidentical array types will be rejected
		 * at constraint creation time.
		 *
		 * XXX perhaps also consider supporting CoerceViaIO?  No need at the
		 * moment since that will never be generated for implicit coercions.
		 */
		op_input_types(eq_opr, &lefttype, &righttype);
		Assert(lefttype == righttype);
		if (typeid == lefttype)
			castfunc = InvalidOid;		/* simplest case */
		else
		{
			pathtype = find_coercion_pathway(lefttype, typeid,
											 COERCION_IMPLICIT,
											 &castfunc);
			if (pathtype != COERCION_PATH_FUNC &&
				pathtype != COERCION_PATH_RELABELTYPE)
			{
				/* If target is ANYARRAY, assume it's OK, else punt. */
				if (lefttype != ANYARRAYOID)
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
