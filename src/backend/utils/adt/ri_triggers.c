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
 *	and the parse/plan node trees they point to are copied into
 *	TopMemoryContext using SPI_saveplan().	This is pretty ugly, since there
 *	is no way to free a no-longer-needed plan tree, but then again we don't
 *	yet have any bookkeeping that would allow us to detect that a plan isn't
 *	needed anymore.  Improve it someday.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/ri_triggers.c,v 1.63.2.1 2004/10/13 22:22:03 tgl Exp $
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

#include "catalog/pg_operator.h"
#include "commands/trigger.h"
#include "executor/spi_priv.h"
#include "optimizer/planmain.h"
#include "parser/parse_oper.h"
#include "rewrite/rewriteHandler.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
#include "utils/acl.h"
#include "miscadmin.h"


/* ----------
 * Local definitions
 * ----------
 */

#define RI_INIT_QUERYHASHSIZE			128

#define RI_MATCH_TYPE_UNSPECIFIED		0
#define RI_MATCH_TYPE_FULL				1
#define RI_MATCH_TYPE_PARTIAL			2

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

#define RI_TRIGTYPE_INSERT 1
#define RI_TRIGTYPE_UPDATE 2
#define RI_TRIGTYPE_INUP   3
#define RI_TRIGTYPE_DELETE 4


/* ----------
 * RI_QueryKey
 *
 *	The key identifying a prepared SPI plan in our private hashtable
 * ----------
 */
typedef struct RI_QueryKey
{
	int32		constr_type;
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
	void	   *plan;
} RI_QueryHashEntry;


/* ----------
 * Local data
 * ----------
 */
static HTAB *ri_query_cache = (HTAB *) NULL;


/* ----------
 * Local function prototypes
 * ----------
 */
static void quoteOneName(char *buffer, const char *name);
static void quoteRelationName(char *buffer, Relation rel);
static int	ri_DetermineMatchType(char *str);
static int ri_NullCheck(Relation rel, HeapTuple tup,
			 RI_QueryKey *key, int pairidx);
static void ri_BuildQueryKeyFull(RI_QueryKey *key, Oid constr_id,
					 int32 constr_queryno,
					 Relation fk_rel, Relation pk_rel,
					 int argc, char **argv);
static void ri_BuildQueryKeyPkCheck(RI_QueryKey *key, Oid constr_id,
						int32 constr_queryno,
						Relation pk_rel,
						int argc, char **argv);
static bool ri_KeysEqual(Relation rel, HeapTuple oldtup, HeapTuple newtup,
			 RI_QueryKey *key, int pairidx);
static bool ri_AllKeysUnequal(Relation rel, HeapTuple oldtup, HeapTuple newtup,
				  RI_QueryKey *key, int pairidx);
static bool ri_OneKeyEqual(Relation rel, int column, HeapTuple oldtup,
			   HeapTuple newtup, RI_QueryKey *key, int pairidx);
static bool ri_AttributesEqual(Oid typeid, Datum oldvalue, Datum newvalue);
static bool ri_Check_Pk_Match(Relation pk_rel, Relation fk_rel,
				  HeapTuple old_row,
				  Oid tgoid, int match_type,
				  int tgnargs, char **tgargs);

static void ri_InitHashTables(void);
static void *ri_FetchPreparedPlan(RI_QueryKey *key);
static void ri_HashPreparedPlan(RI_QueryKey *key, void *plan);

static void ri_CheckTrigger(FunctionCallInfo fcinfo, const char *funcname,
				int tgkind);
static void *ri_PlanCheck(const char *querystr, int nargs, Oid *argtypes,
			 RI_QueryKey *qkey, Relation fk_rel, Relation pk_rel,
			 bool cache_plan);
static bool ri_PerformCheck(RI_QueryKey *qkey, void *qplan,
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;
	int			match_type;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_check", RI_TRIGTYPE_INUP);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Get the relation descriptors of the FK and PK tables and the new
	 * tuple.
	 *
	 * pk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR UPDATE will get on it.
	 */
	pk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowShareLock);
	fk_rel = trigdata->tg_relation;
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		old_row = trigdata->tg_trigtuple;
		new_row = trigdata->tg_newtuple;
	}
	else
	{
		old_row = NULL;
		new_row = trigdata->tg_trigtuple;
	}

	/*
	 * We should not even consider checking the row if it is no longer
	 * valid since it was either deleted (doesn't matter) or updated (in
	 * which case it'll be checked with its final values).
	 *
	 * Note: we need not SetBufferCommitInfoNeedsSave() here since the
	 * new tuple's commit state can't possibly change.
	 */
	if (new_row)
	{
		if (!HeapTupleSatisfiesItself(new_row->t_data))
		{
			heap_close(pk_rel, RowShareLock);
			return PointerGetDatum(NULL);
		}
	}

	/* ----------
	 * SQL3 11.9 <referential constraint definition>
	 *	Gereral rules 2) a):
	 *		If Rf and Rt are empty (no columns to compare given)
	 *		constraint is true if 0 < (SELECT COUNT(*) FROM T)
	 *
	 *	Note: The special case that no columns are given cannot
	 *		occur up to now in Postgres, it's just there for
	 *		future enhancements.
	 * ----------
	 */
	if (tgnargs == 4)
	{
		ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
							 RI_PLAN_CHECK_LOOKUPPK_NOCOLS,
							 fk_rel, pk_rel,
							 tgnargs, tgargs);

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
			snprintf(querystr, sizeof(querystr), "SELECT 1 FROM ONLY %s x FOR UPDATE OF x",
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
						tgargs[RI_CONSTRAINT_NAME_ARGNO]);

		if (SPI_finish() != SPI_OK_FINISH)
			elog(ERROR, "SPI_finish failed");

		heap_close(pk_rel, RowShareLock);

		return PointerGetDatum(NULL);

	}

	match_type = ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]);

	if (match_type == RI_MATCH_TYPE_PARTIAL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MATCH PARTIAL not yet implemented")));

	ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
						 RI_PLAN_CHECK_LOOKUPPK, fk_rel, pk_rel,
						 tgnargs, tgargs);

	switch (ri_NullCheck(fk_rel, new_row, &qkey, RI_KEYPAIR_FK_IDX))
	{
		case RI_KEYS_ALL_NULL:

			/*
			 * No check - if NULLs are allowed at all is already checked
			 * by NOT NULL constraint.
			 *
			 * This is true for MATCH FULL, MATCH PARTIAL, and MATCH
			 * <unspecified>
			 */
			heap_close(pk_rel, RowShareLock);
			return PointerGetDatum(NULL);

		case RI_KEYS_SOME_NULL:

			/*
			 * This is the only case that differs between the three kinds
			 * of MATCH.
			 */
			switch (match_type)
			{
				case RI_MATCH_TYPE_FULL:

					/*
					 * Not allowed - MATCH FULL says either all or none of
					 * the attributes can be NULLs
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
							 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
						  RelationGetRelationName(trigdata->tg_relation),
									tgargs[RI_CONSTRAINT_NAME_ARGNO]),
							 errdetail("MATCH FULL does not allow mixing of null and nonnull key values.")));
					heap_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case RI_MATCH_TYPE_UNSPECIFIED:

					/*
					 * MATCH <unspecified> - if ANY column is null, we
					 * have a match.
					 */
					heap_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);

				case RI_MATCH_TYPE_PARTIAL:

					/*
					 * MATCH PARTIAL - all non-null columns must match.
					 * (not implemented, can be done by modifying the
					 * query below to only include non-null columns, or by
					 * writing a special version here)
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						   errmsg("MATCH PARTIAL not yet implemented")));
					heap_close(pk_rel, RowShareLock);
					return PointerGetDatum(NULL);
			}

		case RI_KEYS_NONE_NULL:

			/*
			 * Have a full qualified key - continue below for all three
			 * kinds of MATCH.
			 */
			break;
	}

	/*
	 * No need to check anything if old and new references are the same on
	 * UPDATE.
	 */
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		if (HeapTupleHeaderGetXmin(old_row->t_data) !=
				GetCurrentTransactionId() &&
				ri_KeysEqual(fk_rel, old_row, new_row, &qkey,
						 RI_KEYPAIR_FK_IDX))
		{
			heap_close(pk_rel, RowShareLock);
			return PointerGetDatum(NULL);
		}
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Fetch or prepare a saved plan for the real check
	 */
	if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
	{
		char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
							(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
		char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];

		/* ----------
		 * The query string built is
		 *	SELECT 1 FROM ONLY <pktable> WHERE pkatt1 = $1 [AND ...]
		 * The type id's for the $ parameters are those of the
		 * corresponding FK attributes. Thus, ri_PlanCheck could
		 * eventually fail if the parser cannot identify some way
		 * how to compare these two types by '='.
		 * ----------
		 */
		quoteRelationName(pkrelname, pk_rel);
		snprintf(querystr, sizeof(querystr), "SELECT 1 FROM ONLY %s x", pkrelname);
		querysep = "WHERE";
		for (i = 0; i < qkey.nkeypairs; i++)
		{
			quoteOneName(attname,
			 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_PK_IDX]);
			snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), " %s %s = $%d",
					 querysep, attname, i + 1);
			querysep = "AND";
			queryoids[i] = SPI_gettypeid(fk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_FK_IDX]);
		}
		strcat(querystr, " FOR UPDATE OF x");

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
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
					tgargs[RI_CONSTRAINT_NAME_ARGNO]);

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
 *	Check for matching value of old pk row in current state for
 * noaction triggers. Returns false if no row was found and a fk row
 * could potentially be referencing this row, true otherwise.
 * ----------
 */
static bool
ri_Check_Pk_Match(Relation pk_rel, Relation fk_rel,
				  HeapTuple old_row,
				  Oid tgoid, int match_type,
				  int tgnargs, char **tgargs)
{
	void	   *qplan;
	RI_QueryKey qkey;
	int			i;
	bool		result;

	ri_BuildQueryKeyPkCheck(&qkey, tgoid,
							RI_PLAN_CHECK_LOOKUPPK, pk_rel,
							tgnargs, tgargs);

	switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
	{
		case RI_KEYS_ALL_NULL:

			/*
			 * No check - nothing could have been referencing this row
			 * anyway.
			 */
			return true;

		case RI_KEYS_SOME_NULL:

			/*
			 * This is the only case that differs between the three kinds
			 * of MATCH.
			 */
			switch (match_type)
			{
				case RI_MATCH_TYPE_FULL:
				case RI_MATCH_TYPE_UNSPECIFIED:

					/*
					 * MATCH <unspecified>/FULL  - if ANY column is null,
					 * we can't be matching to this row already.
					 */
					return true;

				case RI_MATCH_TYPE_PARTIAL:

					/*
					 * MATCH PARTIAL - all non-null columns must match.
					 * (not implemented, can be done by modifying the
					 * query below to only include non-null columns, or by
					 * writing a special version here)
					 */
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						   errmsg("MATCH PARTIAL not yet implemented")));
					break;
			}

		case RI_KEYS_NONE_NULL:

			/*
			 * Have a full qualified key - continue below for all three
			 * kinds of MATCH.
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
		char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
							(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
		char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
		char		attname[MAX_QUOTED_NAME_LEN];
		const char *querysep;
		Oid			queryoids[RI_MAX_NUMKEYS];

		/* ----------
		 * The query string built is
		 *	SELECT 1 FROM ONLY <pktable> WHERE pkatt1 = $1 [AND ...]
		 * The type id's for the $ parameters are those of the
		 * corresponding FK attributes. Thus, ri_PlanCheck could
		 * eventually fail if the parser cannot identify some way
		 * how to compare these two types by '='.
		 * ----------
		 */
		quoteRelationName(pkrelname, pk_rel);
		snprintf(querystr, sizeof(querystr), "SELECT 1 FROM ONLY %s x", pkrelname);
		querysep = "WHERE";
		for (i = 0; i < qkey.nkeypairs; i++)
		{
			quoteOneName(attname,
			 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_PK_IDX]);
			snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), " %s %s = $%d",
					 querysep, attname, i + 1);
			querysep = "AND";
			queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
		}
		strcat(querystr, " FOR UPDATE OF x");

		/* Prepare and save the plan */
		qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;
	int			match_type;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_noaction_del", RI_TRIGTYPE_DELETE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old
	 * tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	match_type = ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]);
	if (ri_Check_Pk_Match(pk_rel, fk_rel,
						  old_row, trigdata->tg_trigger->tgoid,
						  match_type, tgnargs, tgargs))
	{
		/*
		 * There's either another row, or no row could match this one.  In
		 * either case, we don't need to do the check.
		 */
		heap_close(fk_rel, RowShareLock);
		return PointerGetDatum(NULL);
	}

	switch (match_type)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iv):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_NOACTION_DEL_CHECKREF,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			 * Fetch or prepare a saved plan for the restrict delete
			 * lookup if foreign references exist
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
							(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	SELECT 1 FROM ONLY <fktable> WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "SELECT 1 FROM ONLY %s x", fkrelname);
				querysep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);
					snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), " %s %s = $%d",
							 querysep, attname, i + 1);
					querysep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}
				strcat(querystr, " FOR UPDATE OF x");

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_SELECT,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowShareLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL restrict delete.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;
	int			match_type;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_noaction_upd", RI_TRIGTYPE_UPDATE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the new
	 * and old tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	match_type = ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]);

	switch (match_type)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iv):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_NOACTION_UPD_CHECKREF,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			if (ri_KeysEqual(pk_rel, old_row, new_row, &qkey,
							 RI_KEYPAIR_PK_IDX))
			{
				heap_close(fk_rel, RowShareLock);
				return PointerGetDatum(NULL);
			}

			if (ri_Check_Pk_Match(pk_rel, fk_rel,
								  old_row, trigdata->tg_trigger->tgoid,
								  match_type, tgnargs, tgargs))
			{
				/*
				 * There's either another row, or no row could match this
				 * one.  In either case, we don't need to do the check.
				 */
				heap_close(fk_rel, RowShareLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the noaction update
			 * lookup if foreign references exist
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
							(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	SELECT 1 FROM ONLY <fktable> WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "SELECT 1 FROM ONLY %s x", fkrelname);
				querysep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);
					snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), " %s %s = $%d",
							 querysep, attname, i + 1);
					querysep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}
				strcat(querystr, " FOR UPDATE OF x");

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_SELECT,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowShareLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL noaction update.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_cascade_del", RI_TRIGTYPE_DELETE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old
	 * tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual DELETE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) i):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_CASCADE_DEL_DODELETE,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
							(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	DELETE FROM ONLY <fktable> WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "DELETE FROM ONLY %s", fkrelname);
				querysep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);
					snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), " %s %s = $%d",
							 querysep, attname, i + 1);
					querysep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Build up the arguments from the key
			 * values in the deleted PK tuple and delete the referencing
			 * rows
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_DELETE,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL cascaded delete.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;
	int			j;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_cascade_upd", RI_TRIGTYPE_UPDATE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the new
	 * and old tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 7) a) i):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON UPDATE CASCADE
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_CASCADE_UPD_DOUPDATE,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			if (ri_KeysEqual(pk_rel, old_row, new_row, &qkey,
							 RI_KEYPAIR_PK_IDX))
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
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
												 (MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS * 2];
				char		qualstr[(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS * 2];

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = $1 [, ...]
				 *			WHERE fkatt1 = $n [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "UPDATE ONLY %s SET", fkrelname);
				qualstr[0] = '\0';
				querysep = "";
				qualsep = "WHERE";
				for (i = 0, j = qkey.nkeypairs; i < qkey.nkeypairs; i++, j++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);
					snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), "%s %s = $%d",
							 querysep, attname, i + 1);
					snprintf(qualstr + strlen(qualstr), sizeof(qualstr) - strlen(qualstr), " %s %s = $%d",
							 qualsep, attname, j + 1);
					querysep = ",";
					qualsep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
					queryoids[j] = queryoids[i];
				}
				strcat(querystr, qualstr);

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs * 2, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to update the existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, new_row,
							true, /* must detect new rows */
							SPI_OK_UPDATE,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL cascade update.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_restrict_del", RI_TRIGTYPE_DELETE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old
	 * tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iv):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_RESTRICT_DEL_CHECKREF,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			 * Fetch or prepare a saved plan for the restrict delete
			 * lookup if foreign references exist
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
							(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	SELECT 1 FROM ONLY <fktable> WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "SELECT 1 FROM ONLY %s x", fkrelname);
				querysep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);
					snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), " %s %s = $%d",
							 querysep, attname, i + 1);
					querysep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}
				strcat(querystr, " FOR UPDATE OF x");

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_SELECT,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowShareLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL restrict delete.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_restrict_upd", RI_TRIGTYPE_UPDATE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the new
	 * and old tuple.
	 *
	 * fk_rel is opened in RowShareLock mode since that's what our eventual
	 * SELECT FOR UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowShareLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iv):
			 *		MATCH <unspecified> or MATCH FULL
			 *			... ON DELETE CASCADE
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_RESTRICT_UPD_CHECKREF,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			if (ri_KeysEqual(pk_rel, old_row, new_row, &qkey,
							 RI_KEYPAIR_PK_IDX))
			{
				heap_close(fk_rel, RowShareLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Fetch or prepare a saved plan for the restrict update
			 * lookup if foreign references exist
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
							(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	SELECT 1 FROM ONLY <fktable> WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "SELECT 1 FROM ONLY %s x", fkrelname);
				querysep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);
					snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), " %s %s = $%d",
							 querysep, attname, i + 1);
					querysep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}
				strcat(querystr, " FOR UPDATE OF x");

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_SELECT,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowShareLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL restrict update.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setnull_del", RI_TRIGTYPE_DELETE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old
	 * tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) ii):
			 *		MATCH <UNSPECIFIED> or MATCH FULL
			 *			... ON DELETE SET NULL
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_SETNULL_DEL_DOUPDATE,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			 * Fetch or prepare a saved plan for the set null delete
			 * operation
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
												 (MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS * 2];
				char		qualstr[(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = NULL [, ...]
				 *			WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "UPDATE ONLY %s SET", fkrelname);
				qualstr[0] = '\0';
				querysep = "";
				qualsep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);
					snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), "%s %s = NULL",
							 querysep, attname);
					snprintf(qualstr + strlen(qualstr), sizeof(qualstr) - strlen(qualstr), " %s %s = $%d",
							 qualsep, attname, i + 1);
					querysep = ",";
					qualsep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}
				strcat(querystr, qualstr);

				/* Prepare and save the plan */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel, true);
			}

			/*
			 * We have a plan now. Run it to check for existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_UPDATE,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL set null delete.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			i;
	int			match_type;
	bool		use_cached_query;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setnull_upd", RI_TRIGTYPE_UPDATE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old
	 * tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;
	match_type = ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]);

	switch (match_type)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 7) a) ii) 2):
			 *		MATCH FULL
			 *			... ON UPDATE SET NULL
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_SETNULL_UPD_DOUPDATE,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			if (ri_KeysEqual(pk_rel, old_row, new_row, &qkey,
							 RI_KEYPAIR_PK_IDX))
			{
				heap_close(fk_rel, RowExclusiveLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * "MATCH <unspecified>" only changes columns corresponding to
			 * the referenced columns that have changed in pk_rel.	This
			 * means the "SET attrn=NULL [, attrn=NULL]" string will be
			 * change as well.	In this case, we need to build a temporary
			 * plan rather than use our cached plan, unless the update
			 * happens to change all columns in the key.  Fortunately, for
			 * the most common case of a single-column foreign key, this
			 * will be true.
			 *
			 * In case you're wondering, the inequality check works because
			 * we know that the old key value has no NULLs (see above).
			 */

			use_cached_query = match_type == RI_MATCH_TYPE_FULL ||
				ri_AllKeysUnequal(pk_rel, old_row, new_row,
								  &qkey, RI_KEYPAIR_PK_IDX);

			/*
			 * Fetch or prepare a saved plan for the set null update
			 * operation if possible, or build a temporary plan if not.
			 */
			if (!use_cached_query ||
				(qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
												 (MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS * 2];
				char		qualstr[(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = NULL [, ...]
				 *			WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "UPDATE ONLY %s SET", fkrelname);
				qualstr[0] = '\0';
				querysep = "";
				qualsep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);

					/*
					 * MATCH <unspecified> - only change columns
					 * corresponding to changed columns in pk_rel's key
					 */
					if (match_type == RI_MATCH_TYPE_FULL ||
					  !ri_OneKeyEqual(pk_rel, i, old_row, new_row, &qkey,
									  RI_KEYPAIR_PK_IDX))
					{
						snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), "%s %s = NULL",
								 querysep, attname);
						querysep = ",";
					}
					snprintf(qualstr + strlen(qualstr), sizeof(qualstr) - strlen(qualstr), " %s %s = $%d",
							 qualsep, attname, i + 1);
					qualsep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}
				strcat(querystr, qualstr);

				/*
				 * Prepare the plan.  Save it only if we're building the
				 * "standard" plan.
				 */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel,
									 use_cached_query);
			}

			/*
			 * We have a plan now. Run it to update the existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_UPDATE,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL set null update.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setdefault_del", RI_TRIGTYPE_DELETE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old
	 * tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 6) a) iii):
			 *		MATCH <UNSPECIFIED> or MATCH FULL
			 *			... ON DELETE SET DEFAULT
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_SETNULL_DEL_DOUPDATE,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			 * Unfortunately we need to do it on every invocation because
			 * the default value could potentially change between calls.
			 */
			{
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
												 (MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS * 2];
				char		qualstr[(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS];
				int			i;

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = DEFAULT [, ...]
				 *			WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "UPDATE ONLY %s SET", fkrelname);
				qualstr[0] = '\0';
				querysep = "";
				qualsep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);
					snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), "%s %s = DEFAULT",
							 querysep, attname);
					snprintf(qualstr + strlen(qualstr), sizeof(qualstr) - strlen(qualstr), " %s %s = $%d",
							 qualsep, attname, i + 1);
					querysep = ",";
					qualsep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}
				strcat(querystr, qualstr);

				/* Prepare the plan, don't save it */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel, false);
			}

			/*
			 * We have a plan now. Run it to update the existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_UPDATE,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			/*
			 * In the case we delete the row who's key is equal to the
			 * default values AND a referencing row in the foreign key
			 * table exists, we would just have updated it to the same
			 * values. We need to do another lookup now and in case a
			 * reference exists, abort the operation. That is already
			 * implemented in the NO ACTION trigger.
			 */
			RI_FKey_noaction_del(fcinfo);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL set null delete.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;
	void	   *qplan;
	int			match_type;

	/*
	 * Check that this is a valid trigger call on the right time and
	 * event.
	 */
	ri_CheckTrigger(fcinfo, "RI_FKey_setdefault_upd", RI_TRIGTYPE_UPDATE);

	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return PointerGetDatum(NULL);

	/*
	 * Get the relation descriptors of the FK and PK tables and the old
	 * tuple.
	 *
	 * fk_rel is opened in RowExclusiveLock mode since that's what our
	 * eventual UPDATE will get on it.
	 */
	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, RowExclusiveLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	match_type = ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]);

	switch (match_type)
	{
			/* ----------
			 * SQL3 11.9 <referential constraint definition>
			 *	Gereral rules 7) a) iii):
			 *		MATCH <UNSPECIFIED> or MATCH FULL
			 *			... ON UPDATE SET DEFAULT
			 * ----------
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_SETNULL_DEL_DOUPDATE,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

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
			if (ri_KeysEqual(pk_rel, old_row, new_row, &qkey,
							 RI_KEYPAIR_PK_IDX))
			{
				heap_close(fk_rel, RowExclusiveLock);
				return PointerGetDatum(NULL);
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(ERROR, "SPI_connect failed");

			/*
			 * Prepare a plan for the set default delete operation.
			 * Unfortunately we need to do it on every invocation because
			 * the default value could potentially change between calls.
			 */
			{
				char		querystr[MAX_QUOTED_REL_NAME_LEN + 100 +
												 (MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS * 2];
				char		qualstr[(MAX_QUOTED_NAME_LEN + 32) * RI_MAX_NUMKEYS];
				char		fkrelname[MAX_QUOTED_REL_NAME_LEN];
				char		attname[MAX_QUOTED_NAME_LEN];
				const char *querysep;
				const char *qualsep;
				Oid			queryoids[RI_MAX_NUMKEYS];
				int			i;

				/* ----------
				 * The query string built is
				 *	UPDATE ONLY <fktable> SET fkatt1 = DEFAULT [, ...]
				 *			WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, ri_PlanCheck could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				quoteRelationName(fkrelname, fk_rel);
				snprintf(querystr, sizeof(querystr), "UPDATE ONLY %s SET", fkrelname);
				qualstr[0] = '\0';
				querysep = "";
				qualsep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					quoteOneName(attname,
								 tgargs[RI_FIRST_ATTNAME_ARGNO + i * 2 + RI_KEYPAIR_FK_IDX]);

					/*
					 * MATCH <unspecified> - only change columns
					 * corresponding to changed columns in pk_rel's key
					 */
					if (match_type == RI_MATCH_TYPE_FULL ||
						!ri_OneKeyEqual(pk_rel, i, old_row,
									  new_row, &qkey, RI_KEYPAIR_PK_IDX))
					{
						snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr), "%s %s = DEFAULT",
								 querysep, attname);
						querysep = ",";
					}
					snprintf(qualstr + strlen(qualstr), sizeof(qualstr) - strlen(qualstr), " %s %s = $%d",
							 qualsep, attname, i + 1);
					qualsep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									 qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}
				strcat(querystr, qualstr);

				/* Prepare the plan, don't save it */
				qplan = ri_PlanCheck(querystr, qkey.nkeypairs, queryoids,
									 &qkey, fk_rel, pk_rel, false);
			}

			/*
			 * We have a plan now. Run it to update the existing
			 * references.
			 */
			ri_PerformCheck(&qkey, qplan,
							fk_rel, pk_rel,
							old_row, NULL,
							true, /* must detect new rows */
							SPI_OK_UPDATE,
							tgargs[RI_CONSTRAINT_NAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(ERROR, "SPI_finish failed");

			heap_close(fk_rel, RowExclusiveLock);

			/*
			 * In the case we updated the row who's key was equal to the
			 * default values AND a referencing row in the foreign key
			 * table exists, we would just have updated it to the same
			 * values. We need to do another lookup now and in case a
			 * reference exists, abort the operation. That is already
			 * implemented in the NO ACTION trigger.
			 */
			RI_FKey_noaction_upd(fcinfo);

			return PointerGetDatum(NULL);

			/*
			 * Handle MATCH PARTIAL set null delete.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			return PointerGetDatum(NULL);
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
	return PointerGetDatum(NULL);
}


/* ----------
 * RI_FKey_keyequal_upd -
 *
 *	Check if we have a key change on update.
 *
 *	This is not a real trigger procedure. It is used by the deferred
 *	trigger queue manager to detect "triggered data change violation".
 * ----------
 */
bool
RI_FKey_keyequal_upd(TriggerData *trigdata)
{
	int			tgnargs;
	char	  **tgargs;
	Relation	fk_rel;
	Relation	pk_rel;
	HeapTuple	new_row;
	HeapTuple	old_row;
	RI_QueryKey qkey;

	/*
	 * Check for the correct # of call arguments
	 */
	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs = trigdata->tg_trigger->tgargs;
	if (tgnargs < 4 ||
		tgnargs > RI_MAX_ARGUMENTS ||
		(tgnargs % 2) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
			 errmsg("function \"%s\" called with wrong number of trigger arguments",
					"RI_FKey_keyequal_upd")));

	/*
	 * Nothing to do if no column names to compare given
	 */
	if (tgnargs == 4)
		return true;

	/*
	 * Get the relation descriptors of the FK and PK tables and the new
	 * and old tuple.
	 *
	 * Use minimal locking for fk_rel here.
	 */
	if (!OidIsValid(trigdata->tg_trigger->tgconstrrelid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			 errmsg("no target table given for trigger \"%s\" on table \"%s\"",
					trigdata->tg_trigger->tgname,
					RelationGetRelationName(trigdata->tg_relation)),
				 errhint("Remove this referential integrity trigger and its mates, then do ALTER TABLE ADD CONSTRAINT.")));

	fk_rel = heap_open(trigdata->tg_trigger->tgconstrrelid, AccessShareLock);
	pk_rel = trigdata->tg_relation;
	new_row = trigdata->tg_newtuple;
	old_row = trigdata->tg_trigtuple;

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
			/*
			 * MATCH <UNSPECIFIED>
			 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid,
								 RI_PLAN_KEYEQUAL_UPD,
								 fk_rel, pk_rel,
								 tgnargs, tgargs);

			heap_close(fk_rel, AccessShareLock);

			/*
			 * Return if key's are equal
			 */
			return ri_KeysEqual(pk_rel, old_row, new_row, &qkey,
								RI_KEYPAIR_PK_IDX);

			/*
			 * Handle MATCH PARTIAL set null delete.
			 */
		case RI_MATCH_TYPE_PARTIAL:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("MATCH PARTIAL not yet implemented")));
			break;
	}

	/*
	 * Never reached
	 */
	elog(ERROR, "invalid match_type");
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
RI_Initial_Check(FkConstraint *fkconstraint, Relation rel, Relation pkrel)
{
	const char *constrname = fkconstraint->constr_name;
	char		querystr[MAX_QUOTED_REL_NAME_LEN * 2 + 250 +
						(MAX_QUOTED_NAME_LEN + 32) * ((RI_MAX_NUMKEYS * 4)+1)];
	char		pkrelname[MAX_QUOTED_REL_NAME_LEN];
	char		relname[MAX_QUOTED_REL_NAME_LEN];
	char		attname[MAX_QUOTED_NAME_LEN];
	char		fkattname[MAX_QUOTED_NAME_LEN];
	const char *sep;
	List		*list;
	List		*list2;
	int			spi_result;
	void		*qplan;

	/*
	 * Check to make sure current user has enough permissions to do the
	 * test query.  (If not, caller can fall back to the trigger method,
	 * which works because it changes user IDs on the fly.)
	 *
	 * XXX are there any other show-stopper conditions to check?
	 */
	if (pg_class_aclcheck(RelationGetRelid(rel), GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		return false;
	if (pg_class_aclcheck(RelationGetRelid(pkrel), GetUserId(), ACL_SELECT) != ACLCHECK_OK) 
		return false;

	/*----------
	 * The query string built is:
	 *  SELECT fk.keycols FROM ONLY relname fk 
	 *   LEFT OUTER JOIN ONLY pkrelname pk 
	 *   ON (pk.pkkeycol1=fk.keycol1 [AND ...])
	 *   WHERE pk.pkkeycol1 IS NULL AND
	 * For MATCH unspecified:
	 *   (fk.keycol1 IS NOT NULL [AND ...])
	 * For MATCH FULL:
	 *   (fk.keycol1 IS NOT NULL [OR ...])
	 *----------
	 */

	sprintf(querystr, "SELECT ");
	sep="";
	foreach(list, fkconstraint->fk_attrs)
	{
		quoteOneName(attname, strVal(lfirst(list)));
		snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr),
				 "%sfk.%s", sep, attname);
		sep = ", ";
	}

	quoteRelationName(pkrelname, pkrel);
	quoteRelationName(relname, rel);
	snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr),
			 " FROM ONLY %s fk LEFT OUTER JOIN ONLY %s pk ON (",
			 relname, pkrelname);

	sep="";
	for (list=fkconstraint->pk_attrs, list2=fkconstraint->fk_attrs; 
		 list != NIL && list2 != NIL; 
		 list=lnext(list), list2=lnext(list2))
	{
		quoteOneName(attname, strVal(lfirst(list)));
		quoteOneName(fkattname, strVal(lfirst(list2)));
		snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr),
				 "%spk.%s=fk.%s",
				 sep, attname, fkattname);
		sep = " AND ";
	}
	/*
	 * It's sufficient to test any one pk attribute for null to detect a
	 * join failure.
	 */
	quoteOneName(attname, strVal(lfirst(fkconstraint->pk_attrs)));
	snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr),
			 ") WHERE pk.%s IS NULL AND (", attname);

	sep="";
	foreach(list, fkconstraint->fk_attrs)
	{
		quoteOneName(attname, strVal(lfirst(list)));
		snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr),
				 "%sfk.%s IS NOT NULL",
				 sep, attname);
		switch (fkconstraint->fk_matchtype)
		{
			case FKCONSTR_MATCH_UNSPECIFIED:
				sep=" AND ";
				break;
			case FKCONSTR_MATCH_FULL:
				sep=" OR ";
				break;
			case FKCONSTR_MATCH_PARTIAL:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("MATCH PARTIAL not yet implemented")));
				break;
			default:
				elog(ERROR, "unrecognized match type: %d",
					 fkconstraint->fk_matchtype);
				break;
		}
	}
	snprintf(querystr + strlen(querystr), sizeof(querystr) - strlen(querystr),
			 ")");

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");

	/*
	 * Generate the plan.  We don't need to cache it, and there are no
	 * arguments to the plan. 
	 */
	qplan = SPI_prepare(querystr, 0, NULL);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %d for %s", SPI_result, querystr);

	/*
	 * Run the plan.  For safety we force a current query snapshot to be
	 * used.  (In serializable mode, this arguably violates serializability,
	 * but we really haven't got much choice.)  We need at most one tuple
	 * returned, so pass limit = 1.
	 */
	spi_result = SPI_execp_current(qplan, NULL, NULL, true, 1);

	/* Check result */
	if (spi_result != SPI_OK_SELECT)
		elog(ERROR, "SPI_execp_current returned %d", spi_result);

	/* Did we find a tuple violating the constraint? */
	if (SPI_processed > 0)
	{
		HeapTuple	tuple = SPI_tuptable->vals[0];
		TupleDesc	tupdesc = SPI_tuptable->tupdesc;
		int			nkeys = length(fkconstraint->fk_attrs);
		int			i;
		RI_QueryKey	qkey;

		/*
		 * If it's MATCH FULL, and there are any nulls in the FK keys,
		 * complain about that rather than the lack of a match.  MATCH FULL
		 * disallows partially-null FK rows.
		 */
		if (fkconstraint->fk_matchtype == FKCONSTR_MATCH_FULL)
		{
			bool	isnull = false;

			for (i = 1; i <= nkeys; i++)
			{
				(void) SPI_getbinval(tuple, tupdesc, i, &isnull);
				if (isnull)
					break;
			}
			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_FOREIGN_KEY_VIOLATION),
						 errmsg("insert or update on table \"%s\" violates foreign key constraint \"%s\"",
								RelationGetRelationName(rel),
								constrname),
						 errdetail("MATCH FULL does not allow mixing of null and nonnull key values.")));
		}

		/*
		 * Although we didn't cache the query, we need to set up a fake
		 * query key to pass to ri_ReportViolation.
		 */
		MemSet(&qkey, 0, sizeof(qkey));
		qkey.constr_queryno = RI_PLAN_CHECK_LOOKUPPK;
		qkey.nkeypairs = nkeys;
		for (i = 0; i < nkeys; i++)
			qkey.keypair[i][RI_KEYPAIR_FK_IDX] = i + 1;

		ri_ReportViolation(&qkey, constrname,
						   pkrel, rel,
						   tuple, tupdesc,
						   false);
	}

	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");

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


/* ----------
 * ri_DetermineMatchType -
 *
 *	Convert the MATCH TYPE string into a switchable int
 * ----------
 */
static int
ri_DetermineMatchType(char *str)
{
	if (strcmp(str, "UNSPECIFIED") == 0)
		return RI_MATCH_TYPE_UNSPECIFIED;
	if (strcmp(str, "FULL") == 0)
		return RI_MATCH_TYPE_FULL;
	if (strcmp(str, "PARTIAL") == 0)
		return RI_MATCH_TYPE_PARTIAL;

	elog(ERROR, "unrecognized referential integrity match type \"%s\"", str);
	return 0;
}


/* ----------
 * ri_BuildQueryKeyFull -
 *
 *	Build up a new hashtable key for a prepared SPI plan of a
 *	constraint trigger of MATCH FULL. The key consists of:
 *
 *		constr_type is FULL
 *		constr_id is the OID of the pg_trigger row that invoked us
 *		constr_queryno is an internal number of the query inside the proc
 *		fk_relid is the OID of referencing relation
 *		pk_relid is the OID of referenced relation
 *		nkeypairs is the number of keypairs
 *		following are the attribute number keypairs of the trigger invocation
 *
 *	At least for MATCH FULL this builds a unique key per plan.
 * ----------
 */
static void
ri_BuildQueryKeyFull(RI_QueryKey *key, Oid constr_id, int32 constr_queryno,
					 Relation fk_rel, Relation pk_rel,
					 int argc, char **argv)
{
	int			i;
	int			j;
	int			fno;

	/*
	 * Initialize the key and fill in type, oid's and number of keypairs
	 */
	memset((void *) key, 0, sizeof(RI_QueryKey));
	key->constr_type = RI_MATCH_TYPE_FULL;
	key->constr_id = constr_id;
	key->constr_queryno = constr_queryno;
	key->fk_relid = fk_rel->rd_id;
	key->pk_relid = pk_rel->rd_id;
	key->nkeypairs = (argc - RI_FIRST_ATTNAME_ARGNO) / 2;

	/*
	 * Lookup the attribute numbers of the arguments to the trigger call
	 * and fill in the keypairs.
	 */
	for (i = 0, j = RI_FIRST_ATTNAME_ARGNO; j < argc; i++, j += 2)
	{
		fno = SPI_fnumber(fk_rel->rd_att, argv[j]);
		if (fno == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("table \"%s\" does not have column \"%s\" referenced by constraint \"%s\"",
							RelationGetRelationName(fk_rel),
							argv[j],
							argv[RI_CONSTRAINT_NAME_ARGNO])));
		key->keypair[i][RI_KEYPAIR_FK_IDX] = fno;

		fno = SPI_fnumber(pk_rel->rd_att, argv[j + 1]);
		if (fno == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("table \"%s\" does not have column \"%s\" referenced by constraint \"%s\"",
							RelationGetRelationName(pk_rel),
							argv[j + 1],
							argv[RI_CONSTRAINT_NAME_ARGNO])));
		key->keypair[i][RI_KEYPAIR_PK_IDX] = fno;
	}
}

/*
 * Check that RI trigger function was called in expected context
 */
static void
ri_CheckTrigger(FunctionCallInfo fcinfo, const char *funcname, int tgkind)
{
	TriggerData *trigdata = (TriggerData *) fcinfo->context;
	int			tgnargs;

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

	/*
	 * Check for the correct # of call arguments
	 */
	tgnargs = trigdata->tg_trigger->tgnargs;
	if (tgnargs < 4 ||
		tgnargs > RI_MAX_ARGUMENTS ||
		(tgnargs % 2) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
			 errmsg("function \"%s\" called with wrong number of trigger arguments",
					funcname)));

	/*
	 * Check that tgconstrrelid is known.  We need to check here because
	 * of ancient pg_dump bug; see notes in CreateTrigger().
	 */
	if (!OidIsValid(trigdata->tg_trigger->tgconstrrelid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
			 errmsg("no target table given for trigger \"%s\" on table \"%s\"",
					trigdata->tg_trigger->tgname,
					RelationGetRelationName(trigdata->tg_relation)),
				 errhint("Remove this referential integrity trigger and its mates, then do ALTER TABLE ADD CONSTRAINT.")));
}


/*
 * Prepare execution plan for a query to enforce an RI restriction
 *
 * If cache_plan is true, the plan is saved into our plan hashtable
 * so that we don't need to plan it again.
 */
static void *
ri_PlanCheck(const char *querystr, int nargs, Oid *argtypes,
			 RI_QueryKey *qkey, Relation fk_rel, Relation pk_rel,
			 bool cache_plan)
{
	void	   *qplan;
	Relation	query_rel;
	AclId		save_uid;

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
	save_uid = GetUserId();
	SetUserId(RelationGetForm(query_rel)->relowner);

	/* Create the plan */
	qplan = SPI_prepare(querystr, nargs, argtypes);

	if (qplan == NULL)
		elog(ERROR, "SPI_prepare returned %d for %s", SPI_result, querystr);

	/* Restore UID */
	SetUserId(save_uid);

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
ri_PerformCheck(RI_QueryKey *qkey, void *qplan,
				Relation fk_rel, Relation pk_rel,
				HeapTuple old_tuple, HeapTuple new_tuple,
				bool detectNewRows,
				int expect_OK, const char *constrname)
{
	Relation	query_rel,
				source_rel;
	int			key_idx;
	bool		useCurrentSnapshot;
	int			limit;
	int			spi_result;
	AclId		save_uid;
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
	 * The values for the query are taken from the table on which the
	 * trigger is called - it is normally the other one with respect to
	 * query_rel. An exception is ri_Check_Pk_Match(), which uses the PK
	 * table for both (the case when constrname == NULL)
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
	 * In READ COMMITTED mode, we just need to make sure the regular query
	 * snapshot is up-to-date, and we will see all rows that could be
	 * interesting.  In SERIALIZABLE mode, we can't update the regular query
	 * snapshot.  If the caller passes detectNewRows == false then it's okay
	 * to do the query with the transaction snapshot; otherwise we tell the
	 * executor to force a current snapshot (and error out if it finds any
	 * rows under current snapshot that wouldn't be visible per the
	 * transaction snapshot).
	 */
	if (XactIsoLevel == XACT_SERIALIZABLE)
	{
		useCurrentSnapshot = detectNewRows;
	}
	else
	{
		SetQuerySnapshot();
		useCurrentSnapshot = false;
	}

	/*
	 * If this is a select query (e.g., for a 'no action' or 'restrict'
	 * trigger), we only need to see if there is a single row in the
	 * table, matching the key.  Otherwise, limit = 0 - because we want
	 * the query to affect ALL the matching rows.
	 */
	limit = (expect_OK == SPI_OK_SELECT) ? 1 : 0;

	/* Switch to proper UID to perform check as */
	save_uid = GetUserId();
	SetUserId(RelationGetForm(query_rel)->relowner);

	/* Finally we can run the query. */
	spi_result = SPI_execp_current(qplan, vals, nulls,
								   useCurrentSnapshot, limit);

	/* Restore UID */
	SetUserId(save_uid);

	/* Check result */
	if (spi_result < 0)
		elog(ERROR, "SPI_execp_current returned %d", spi_result);

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
	 * Determine which relation to complain about.  If tupdesc wasn't
	 * passed by caller, assume the violator tuple came from there.
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
	 * constraint - no need to try to extract the values, and the message
	 * in this case looks different.
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
		 * Go to "..." if name or value doesn't fit in buffer.  We reserve
		 * 5 bytes to ensure we can add comma, "...", null.
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
				 errmsg("update or delete on \"%s\" violates foreign key constraint \"%s\" on \"%s\"",
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
 *		constr_type is FULL
 *		constr_id is the OID of the pg_trigger row that invoked us
 *		constr_queryno is an internal number of the query inside the proc
 *		pk_relid is the OID of referenced relation
 *		nkeypairs is the number of keypairs
 *		following are the attribute number keypairs of the trigger invocation
 *
 *	At least for MATCH FULL this builds a unique key per plan.
 * ----------
 */
static void
ri_BuildQueryKeyPkCheck(RI_QueryKey *key, Oid constr_id, int32 constr_queryno,
						Relation pk_rel,
						int argc, char **argv)
{
	int			i;
	int			j;
	int			fno;

	/*
	 * Initialize the key and fill in type, oid's and number of keypairs
	 */
	memset((void *) key, 0, sizeof(RI_QueryKey));
	key->constr_type = RI_MATCH_TYPE_FULL;
	key->constr_id = constr_id;
	key->constr_queryno = constr_queryno;
	key->fk_relid = 0;
	key->pk_relid = pk_rel->rd_id;
	key->nkeypairs = (argc - RI_FIRST_ATTNAME_ARGNO) / 2;

	/*
	 * Lookup the attribute numbers of the arguments to the trigger call
	 * and fill in the keypairs.
	 */
	for (i = 0, j = RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_PK_IDX; j < argc; i++, j += 2)
	{
		fno = SPI_fnumber(pk_rel->rd_att, argv[j]);
		if (fno == SPI_ERROR_NOATTRIBUTE)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("table \"%s\" does not have column \"%s\" referenced by constraint \"%s\"",
							RelationGetRelationName(pk_rel),
							argv[j],
							argv[RI_CONSTRAINT_NAME_ARGNO])));
		key->keypair[i][RI_KEYPAIR_PK_IDX] = fno;
		key->keypair[i][RI_KEYPAIR_FK_IDX] = 0;
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
 *	Initialize our internal hash table for prepared
 *	query plans.
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
}


/* ----------
 * ri_FetchPreparedPlan -
 *
 *	Lookup for a query key in our private hash table of prepared
 *	and saved SPI execution plans. Return the plan if found or NULL.
 * ----------
 */
static void *
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
ri_HashPreparedPlan(RI_QueryKey *key, void *plan)
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
	if (entry == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
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
			 RI_QueryKey *key, int pairidx)
{
	int			i;
	Oid			typeid;
	Datum		oldvalue;
	Datum		newvalue;
	bool		isnull;

	for (i = 0; i < key->nkeypairs; i++)
	{
		/*
		 * Get one attributes oldvalue. If it is NULL - they're not equal.
		 */
		oldvalue = SPI_getbinval(oldtup, rel->rd_att,
								 key->keypair[i][pairidx], &isnull);
		if (isnull)
			return false;

		/*
		 * Get one attributes oldvalue. If it is NULL - they're not equal.
		 */
		newvalue = SPI_getbinval(newtup, rel->rd_att,
								 key->keypair[i][pairidx], &isnull);
		if (isnull)
			return false;

		/*
		 * Get the attributes type OID and call the '=' operator to
		 * compare the values.
		 */
		typeid = SPI_gettypeid(rel->rd_att, key->keypair[i][pairidx]);
		if (!ri_AttributesEqual(typeid, oldvalue, newvalue))
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
				  RI_QueryKey *key, int pairidx)
{
	int			i;
	Oid			typeid;
	Datum		oldvalue;
	Datum		newvalue;
	bool		isnull;
	bool		keys_unequal;

	keys_unequal = true;
	for (i = 0; keys_unequal && i < key->nkeypairs; i++)
	{
		/*
		 * Get one attributes oldvalue. If it is NULL - they're not equal.
		 */
		oldvalue = SPI_getbinval(oldtup, rel->rd_att,
								 key->keypair[i][pairidx], &isnull);
		if (isnull)
			continue;

		/*
		 * Get one attributes oldvalue. If it is NULL - they're not equal.
		 */
		newvalue = SPI_getbinval(newtup, rel->rd_att,
								 key->keypair[i][pairidx], &isnull);
		if (isnull)
			continue;

		/*
		 * Get the attributes type OID and call the '=' operator to
		 * compare the values.
		 */
		typeid = SPI_gettypeid(rel->rd_att, key->keypair[i][pairidx]);
		if (!ri_AttributesEqual(typeid, oldvalue, newvalue))
			continue;
		keys_unequal = false;
	}

	return keys_unequal;
}


/* ----------
 * ri_OneKeyEqual -
 *
 *	Check if one key value in OLD and NEW is equal.
 *
 *	ri_KeysEqual could call this but would run a bit slower.  For
 *	now, let's duplicate the code.
 * ----------
 */
static bool
ri_OneKeyEqual(Relation rel, int column, HeapTuple oldtup, HeapTuple newtup,
			   RI_QueryKey *key, int pairidx)
{
	Oid			typeid;
	Datum		oldvalue;
	Datum		newvalue;
	bool		isnull;

	/*
	 * Get one attributes oldvalue. If it is NULL - they're not equal.
	 */
	oldvalue = SPI_getbinval(oldtup, rel->rd_att,
							 key->keypair[column][pairidx], &isnull);
	if (isnull)
		return false;

	/*
	 * Get one attributes oldvalue. If it is NULL - they're not equal.
	 */
	newvalue = SPI_getbinval(newtup, rel->rd_att,
							 key->keypair[column][pairidx], &isnull);
	if (isnull)
		return false;

	/*
	 * Get the attributes type OID and call the '=' operator to compare
	 * the values.
	 */
	typeid = SPI_gettypeid(rel->rd_att, key->keypair[column][pairidx]);
	if (!ri_AttributesEqual(typeid, oldvalue, newvalue))
		return false;

	return true;
}


/* ----------
 * ri_AttributesEqual -
 *
 *	Call the type specific '=' operator comparison function
 *	for two values.
 *
 *	NB: we have already checked that neither value is null.
 * ----------
 */
static bool
ri_AttributesEqual(Oid typeid, Datum oldvalue, Datum newvalue)
{
	TypeCacheEntry *typentry;

	/*
	 * Find the data type in the typcache, and ask for eq_opr info.
	 */
	typentry = lookup_type_cache(typeid, TYPECACHE_EQ_OPR_FINFO);

	if (!OidIsValid(typentry->eq_opr_finfo.fn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("could not identify an equality operator for type %s",
						format_type_be(typeid))));

	/*
	 * Call the type specific '=' function
	 */
	return DatumGetBool(FunctionCall2(&(typentry->eq_opr_finfo),
									  oldvalue, newvalue));
}
