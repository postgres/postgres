/* ----------
 * ri_triggers.c
 *
 *	Generic trigger procedures for referential integrity constraint
 *	checks.
 *
 *	1999 Jan Wieck
 *
 * $Header: /cvsroot/pgsql/src/backend/utils/adt/ri_triggers.c,v 1.2 1999/10/08 12:00:08 wieck Exp $
 *
 * ----------
 */

#include "postgres.h"
#include "fmgr.h"

#include "access/heapam.h"
#include "catalog/pg_operator.h"
#include "catalog/catname.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/mcxt.h"
#include "utils/syscache.h"
#include "lib/hasht.h"


/* ----------
 * Local definitions
 * ----------
 */
#define RI_CONSTRAINT_NAME_ARGNO		0
#define RI_FK_RELNAME_ARGNO				1
#define RI_PK_RELNAME_ARGNO				2
#define RI_MATCH_TYPE_ARGNO				3
#define RI_FIRST_ATTNAME_ARGNO			4

#define RI_MAX_NUMKEYS					16
#define RI_MAX_ARGUMENTS		(RI_FIRST_ATTNAME_ARGNO + (RI_MAX_NUMKEYS * 2))
#define RI_KEYPAIR_FK_IDX				0
#define RI_KEYPAIR_PK_IDX				1

#define RI_INIT_QUERYHASHSIZE			128
#define RI_INIT_OPREQHASHSIZE			128

#define RI_MATCH_TYPE_UNSPECIFIED		0
#define RI_MATCH_TYPE_FULL				1
#define RI_MATCH_TYPE_PARTIAL			2

#define RI_KEYS_ALL_NULL				0
#define RI_KEYS_SOME_NULL				1
#define RI_KEYS_NONE_NULL				2


#define RI_PLAN_TYPE_CHECK_FULL			0
#define RI_PLAN_TYPE_CASCADE_DEL_FULL	1


/* ----------
 * RI_QueryKey
 *
 *	The key identifying a prepared SPI plan in our private hashtable
 * ----------
 */
typedef struct RI_QueryKey {
	int32				constr_type;
	Oid					constr_id;
	int32				constr_queryno;
	Oid					fk_relid;
	Oid					pk_relid;
	int32				nkeypairs;
	int16				keypair[RI_MAX_NUMKEYS][2];
} RI_QueryKey;


/* ----------
 * RI_QueryHashEntry
 * ----------
 */
typedef struct RI_QueryHashEntry {
	RI_QueryKey			key;
	void			   *plan;
} RI_QueryHashEntry;


typedef struct RI_OpreqHashEntry {
	Oid					typeid;
	Oid					oprfnid;
	FmgrInfo			oprfmgrinfo;
} RI_OpreqHashEntry;



/* ----------
 * Local data
 * ----------
 */
static HTAB			   *ri_query_cache = (HTAB *)NULL;
static HTAB			   *ri_opreq_cache = (HTAB *)NULL;


/* ----------
 * Local function prototypes
 * ----------
 */
static int ri_DetermineMatchType(char *str);
static int ri_NullCheck(Relation rel, HeapTuple tup, 
							RI_QueryKey *key, int pairidx);
static void ri_BuildQueryKeyFull(RI_QueryKey *key, Oid constr_id,
							int32 constr_queryno,
							Relation fk_rel, Relation pk_rel,
							int argc, char **argv);
static bool ri_KeysEqual(Relation rel, HeapTuple oldtup, HeapTuple newtup, 
							RI_QueryKey *key, int pairidx);
static bool ri_AttributesEqual(Oid typeid, Datum oldvalue, Datum newvalue);

static void ri_InitHashTables(void);
static void *ri_FetchPreparedPlan(RI_QueryKey *key);
static void ri_HashPreparedPlan(RI_QueryKey *key, void *plan);



/* ----------
 * RI_FKey_check -
 *
 *	Check foreign key existance (combined for INSERT and UPDATE).
 * ----------
 */
static HeapTuple
RI_FKey_check (FmgrInfo *proinfo)
{
	TriggerData		   *trigdata;
	int					tgnargs;
	char			  **tgargs;
	Relation			fk_rel;
	Relation			pk_rel;
	HeapTuple			new_row;
	HeapTuple			old_row;
	RI_QueryKey			qkey;
	void			   *qplan;
	Datum				check_values[RI_MAX_NUMKEYS];
	char				check_nulls[RI_MAX_NUMKEYS + 1];
	bool				isnull;
	int					i;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	/* ----------
	 * Check that this is a valid trigger call on the right time and event.
	 * ----------
	 */
	if (trigdata == NULL)
		elog(ERROR, "RI_FKey_check() not fired by trigger manager");
	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) || 
				!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		elog(ERROR, "RI_FKey_check() must be fired AFTER ROW");
	if (!TRIGGER_FIRED_BY_INSERT(trigdata->tg_event) &&
				!TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
		elog(ERROR, "RI_FKey_check() must be fired for INSERT or UPDATE");

	/* ----------
	 * Check for the correct # of call arguments 
	 * ----------
	 */
	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs  = trigdata->tg_trigger->tgargs;
	if (tgnargs < 4 || (tgnargs % 2) != 0)
		elog(ERROR, "wrong # of arguments in call to RI_FKey_check()");
	if (tgnargs > RI_MAX_ARGUMENTS)
		elog(ERROR, "too many keys (%d max) in call to RI_FKey_check()",
						RI_MAX_NUMKEYS);

	/* ----------
	 * Get the relation descriptors of the FK and PK tables and
	 * the new tuple.
	 * ----------
	 */
	fk_rel  = trigdata->tg_relation;
	pk_rel	= heap_openr(tgargs[RI_PK_RELNAME_ARGNO], NoLock);
	if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
	{
		old_row = trigdata->tg_trigtuple;
		new_row = trigdata->tg_newtuple;
	} else {
		old_row = NULL;
		new_row = trigdata->tg_trigtuple;
	}

	/* ----------
	 * SQL3 11.9 <referential constraint definition>
	 *	Gereral rules 2) a):
	 *		If Rf and Rt are empty (no columns to compare given)
	 *		constraint is true if 0 < (SELECT COUNT(*) FROM T)
	 * ----------
	 */
	if (tgnargs == 4) {
		ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid, 1,
								fk_rel, pk_rel,
								tgnargs, tgargs);

		if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
		{
			char		querystr[8192];

			/* ----------
			 * The query string built is
			 *    SELECT oid FROM <pktable>
			 * ----------
			 */
			sprintf(querystr, "SELECT oid FROM \"%s\"", 
								tgargs[RI_PK_RELNAME_ARGNO]);

			/* ----------
			 * Prepare, save and remember the new plan.
			 * ----------
			 */
			qplan = SPI_prepare(querystr, 0, NULL);
			qplan = SPI_saveplan(qplan);
			ri_HashPreparedPlan(&qkey, qplan);
		}
		heap_close(pk_rel, NoLock);

		/* ----------
		 * Execute the plan
		 * ----------
		 */
		if (SPI_connect() != SPI_OK_CONNECT)
			elog(NOTICE, "SPI_connect() failed in RI_FKey_check()");

		if (SPI_execp(qplan, check_values, check_nulls, 1) != SPI_OK_SELECT)
			elog(ERROR, "SPI_execp() failed in RI_FKey_check()");
		
		if (SPI_processed == 0)
			elog(ERROR, "%s referential integrity violation - "
						"no rows found in %s",
					tgargs[RI_CONSTRAINT_NAME_ARGNO],
					tgargs[RI_PK_RELNAME_ARGNO]);

		if (SPI_finish() != SPI_OK_FINISH)
			elog(NOTICE, "SPI_finish() failed in RI_FKey_check()");

		return NULL;

	}

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
		/* ----------
		 * SQL3 11.9 <referential constraint definition>
		 *	Gereral rules 2) b):
		 * 		<match type> is not specified
		 * ----------
		 */
		case RI_MATCH_TYPE_UNSPECIFIED:
			elog(ERROR, "MATCH <unspecified> not yet supported");
			return NULL;

		/* ----------
		 * SQL3 11.9 <referential constraint definition>
		 *	Gereral rules 2) c):
		 * 		MATCH PARTIAL
		 * ----------
		 */
		case RI_MATCH_TYPE_PARTIAL:
			elog(ERROR, "MATCH PARTIAL not yet supported");
			return NULL;

		/* ----------
		 * SQL3 11.9 <referential constraint definition>
		 *	Gereral rules 2) d):
		 * 		MATCH FULL
		 * ----------
		 */
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid, 2,
												fk_rel, pk_rel,
												tgnargs, tgargs);

			switch (ri_NullCheck(fk_rel, new_row, &qkey, RI_KEYPAIR_FK_IDX))
			{
				case RI_KEYS_ALL_NULL:
					/* ----------
					 * No check - if NULLs are allowed at all is
					 * already checked by NOT NULL constraint.
					 * ----------
					 */
					heap_close(pk_rel, NoLock);
					return NULL;
					
				case RI_KEYS_SOME_NULL:
					/* ----------
					 * Not allowed - MATCH FULL says either all or none
					 * of the attributes can be NULLs
					 * ----------
					 */
					elog(ERROR, "%s referential integrity violation - "
								"MATCH FULL doesn't allow mixing of NULL "
								"and NON-NULL key values",
								tgargs[RI_CONSTRAINT_NAME_ARGNO]);
					break;

				case RI_KEYS_NONE_NULL:
					/* ----------
					 * Have a full qualified key - continue below
					 * ----------
					 */
					break;
			}
			heap_close(pk_rel, NoLock);

			/* ----------
			 * If we're called on UPDATE, check if there was a change
			 * in the foreign key at all.
			 * ----------
			 */
			if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event))
			{
				if (ri_KeysEqual(fk_rel, old_row, new_row, &qkey,
														RI_KEYPAIR_FK_IDX))
					return NULL;
			}

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(NOTICE, "SPI_connect() failed in RI_FKey_check()");

			/* ----------
			 * Fetch or prepare a saved plan for the real check
			 * ----------
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				char		buf[256];
				char		querystr[8192];
				char		*querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *    SELECT oid FROM <pktable> WHERE pkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding FK attributes. Thus, SPI_prepare could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				sprintf(querystr, "SELECT oid FROM \"%s\"", 
									tgargs[RI_PK_RELNAME_ARGNO]);
				querysep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					sprintf(buf, " %s \"%s\" = $%d", querysep, 
										tgargs[5 + i * 2], i + 1);
					strcat(querystr, buf);
					querysep = "AND";
					queryoids[i] = SPI_gettypeid(fk_rel->rd_att,
									qkey.keypair[i][RI_KEYPAIR_FK_IDX]);
				}

				/* ----------
				 * Prepare, save and remember the new plan.
				 * ----------
				 */
				qplan = SPI_prepare(querystr, qkey.nkeypairs, queryoids);
				qplan = SPI_saveplan(qplan);
				ri_HashPreparedPlan(&qkey, qplan);
			}

			/* ----------
			 * We have a plan now. Build up the arguments for SPI_execp()
			 * from the key values in the new FK tuple.
			 * ----------
			 */
			for (i = 0; i < qkey.nkeypairs; i++)
			{
				check_values[i] = SPI_getbinval(new_row,
									fk_rel->rd_att,
									qkey.keypair[i][RI_KEYPAIR_FK_IDX],
									&isnull);
				if (isnull) 
					check_nulls[i] = 'n';
				else
					check_nulls[i] = ' ';
			}
			check_nulls[RI_MAX_NUMKEYS] = '\0';

			/* ----------
			 * Now check that foreign key exists in PK table
			 * ----------
			 */
			if (SPI_execp(qplan, check_values, check_nulls, 1) != SPI_OK_SELECT)
				elog(ERROR, "SPI_execp() failed in RI_FKey_check()");
			
			if (SPI_processed == 0)
				elog(ERROR, "%s referential integrity violation - "
							"key referenced from %s not found in %s",
						tgargs[RI_CONSTRAINT_NAME_ARGNO],
						tgargs[RI_FK_RELNAME_ARGNO],
						tgargs[RI_PK_RELNAME_ARGNO]);

			if (SPI_finish() != SPI_OK_FINISH)
				elog(NOTICE, "SPI_finish() failed in RI_FKey_check()");

			return NULL;
	}

	/* ----------
	 * Never reached
	 * ----------
	 */
	elog(ERROR, "internal error #1 in ri_triggers.c");
	return NULL;
}


/* ----------
 * RI_FKey_check_ins -
 *
 *	Check foreign key existance at insert event on FK table.
 * ----------
 */
HeapTuple
RI_FKey_check_ins (FmgrInfo *proinfo)
{
	return RI_FKey_check(proinfo);
}


/* ----------
 * RI_FKey_check_upd -
 *
 *	Check foreign key existance at update event on FK table.
 * ----------
 */
HeapTuple
RI_FKey_check_upd (FmgrInfo *proinfo)
{
	return RI_FKey_check(proinfo);
}


/* ----------
 * RI_FKey_cascade_del -
 *
 *	Cascaded delete foreign key references at delete event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_cascade_del (FmgrInfo *proinfo)
{
	TriggerData		   *trigdata;
	int					tgnargs;
	char			  **tgargs;
	Relation			fk_rel;
	Relation			pk_rel;
	HeapTuple			old_row;
	RI_QueryKey			qkey;
	void			   *qplan;
	Datum				del_values[RI_MAX_NUMKEYS];
	char				del_nulls[RI_MAX_NUMKEYS + 1];
	bool				isnull;
	int					i;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	/* ----------
	 * Check that this is a valid trigger call on the right time and event.
	 * ----------
	 */
	if (trigdata == NULL)
		elog(ERROR, "RI_FKey_cascade_del() not fired by trigger manager");
	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) || 
				!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event))
		elog(ERROR, "RI_FKey_cascade_del() must be fired AFTER ROW");
	if (!TRIGGER_FIRED_BY_DELETE(trigdata->tg_event))
		elog(ERROR, "RI_FKey_cascade_del() must be fired for DELETE");

	/* ----------
	 * Check for the correct # of call arguments 
	 * ----------
	 */
	tgnargs = trigdata->tg_trigger->tgnargs;
	tgargs  = trigdata->tg_trigger->tgargs;
	if (tgnargs < 4 || (tgnargs % 2) != 0)
		elog(ERROR, "wrong # of arguments in call to RI_FKey_cascade_del()");
	if (tgnargs > RI_MAX_ARGUMENTS)
		elog(ERROR, "too many keys (%d max) in call to RI_FKey_cascade_del()",
						RI_MAX_NUMKEYS);

	/* ----------
	 * Nothing to do if no column names to compare given
	 * ----------
	 */
	if (tgnargs == 4)
		return NULL;

	/* ----------
	 * Get the relation descriptors of the FK and PK tables and
	 * the old tuple.
	 * ----------
	 */
	fk_rel	= heap_openr(tgargs[RI_FK_RELNAME_ARGNO], NoLock);
	pk_rel  = trigdata->tg_relation;
	old_row = trigdata->tg_trigtuple;

	switch (ri_DetermineMatchType(tgargs[RI_MATCH_TYPE_ARGNO]))
	{
		/* ----------
		 * SQL3 11.9 <referential constraint definition>
		 *	Gereral rules 6) a) i):
		 * 		MATCH <unspecified> or MATCH FULL
		 *			... ON DELETE CASCADE
		 * ----------
		 */
		case RI_MATCH_TYPE_UNSPECIFIED:
		case RI_MATCH_TYPE_FULL:
			ri_BuildQueryKeyFull(&qkey, trigdata->tg_trigger->tgoid, 1,
												fk_rel, pk_rel,
												tgnargs, tgargs);

			switch (ri_NullCheck(pk_rel, old_row, &qkey, RI_KEYPAIR_PK_IDX))
			{
				case RI_KEYS_ALL_NULL:
				case RI_KEYS_SOME_NULL:
					/* ----------
					 * No check - MATCH FULL means there cannot be any
					 * reference to old key if it contains NULL
					 * ----------
					 */
					heap_close(fk_rel, NoLock);
					return NULL;
					
				case RI_KEYS_NONE_NULL:
					/* ----------
					 * Have a full qualified key - continue below
					 * ----------
					 */
					break;
			}
			heap_close(fk_rel, NoLock);

			if (SPI_connect() != SPI_OK_CONNECT)
				elog(NOTICE, "SPI_connect() failed in RI_FKey_check()");

			/* ----------
			 * Fetch or prepare a saved plan for the cascaded delete
			 * ----------
			 */
			if ((qplan = ri_FetchPreparedPlan(&qkey)) == NULL)
			{
				char		buf[256];
				char		querystr[8192];
				char		*querysep;
				Oid			queryoids[RI_MAX_NUMKEYS];

				/* ----------
				 * The query string built is
				 *    DELETE FROM <fktable> WHERE fkatt1 = $1 [AND ...]
				 * The type id's for the $ parameters are those of the
				 * corresponding PK attributes. Thus, SPI_prepare could
				 * eventually fail if the parser cannot identify some way
				 * how to compare these two types by '='.
				 * ----------
				 */
				sprintf(querystr, "DELETE FROM \"%s\"", 
									tgargs[RI_FK_RELNAME_ARGNO]);
				querysep = "WHERE";
				for (i = 0; i < qkey.nkeypairs; i++)
				{
					sprintf(buf, " %s \"%s\" = $%d", querysep, 
										tgargs[4 + i * 2], i + 1);
					strcat(querystr, buf);
					querysep = "AND";
					queryoids[i] = SPI_gettypeid(pk_rel->rd_att,
									qkey.keypair[i][RI_KEYPAIR_PK_IDX]);
				}

				/* ----------
				 * Prepare, save and remember the new plan.
				 * ----------
				 */
				qplan = SPI_prepare(querystr, qkey.nkeypairs, queryoids);
				qplan = SPI_saveplan(qplan);
				ri_HashPreparedPlan(&qkey, qplan);
			}

			/* ----------
			 * We have a plan now. Build up the arguments for SPI_execp()
			 * from the key values in the deleted PK tuple.
			 * ----------
			 */
			for (i = 0; i < qkey.nkeypairs; i++)
			{
				del_values[i] = SPI_getbinval(old_row,
									pk_rel->rd_att,
									qkey.keypair[i][RI_KEYPAIR_PK_IDX],
									&isnull);
				if (isnull) 
					del_nulls[i] = 'n';
				else
					del_nulls[i] = ' ';
			}
			del_nulls[RI_MAX_NUMKEYS] = '\0';

			/* ----------
			 * Now delete constraint
			 * ----------
			 */
			if (SPI_execp(qplan, del_values, del_nulls, 1) != SPI_OK_DELETE)
				elog(ERROR, "SPI_execp() failed in RI_FKey_cascade_del()");
			
			if (SPI_finish() != SPI_OK_FINISH)
				elog(NOTICE, "SPI_finish() failed in RI_FKey_cascade_del()");

			return NULL;

		/* ----------
		 * Handle MATCH PARTIAL cascaded delete.
		 * ----------
		 */
		case RI_MATCH_TYPE_PARTIAL:
			elog(ERROR, "MATCH PARTIAL not yet supported");
			return NULL;
	}

	/* ----------
	 * Never reached
	 * ----------
	 */
	elog(ERROR, "internal error #2 in ri_triggers.c");
	return NULL;
}


/* ----------
 * RI_FKey_cascade_upd -
 *
 *	Cascaded update/delete foreign key references at update event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_cascade_upd (FmgrInfo *proinfo)
{
	TriggerData			*trigdata;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_cascade_upd() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_restrict_del -
 *
 *	Restrict delete from PK table to rows unreferenced by foreign key.
 * ----------
 */
HeapTuple
RI_FKey_restrict_del (FmgrInfo *proinfo)
{
	TriggerData			*trigdata;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_restrict_del() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_restrict_upd -
 *
 *	Restrict update of PK to rows unreferenced by foreign key.
 * ----------
 */
HeapTuple
RI_FKey_restrict_upd (FmgrInfo *proinfo)
{
	TriggerData			*trigdata;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_restrict_upd() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_setnull_del -
 *
 *	Set foreign key references to NULL values at delete event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_setnull_del (FmgrInfo *proinfo)
{
	TriggerData			*trigdata;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_setnull_del() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_setnull_upd -
 *
 *	Set foreign key references to NULL at update event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_setnull_upd (FmgrInfo *proinfo)
{
	TriggerData			*trigdata;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_setnull_upd() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_setdefault_del -
 *
 *	Set foreign key references to defaults at delete event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_setdefault_del (FmgrInfo *proinfo)
{
	TriggerData			*trigdata;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_setdefault_del() called\n");
	return NULL;
}


/* ----------
 * RI_FKey_setdefault_upd -
 *
 *	Set foreign key references to defaults at update event on PK table.
 * ----------
 */
HeapTuple
RI_FKey_setdefault_upd (FmgrInfo *proinfo)
{
	TriggerData			*trigdata;

	trigdata = CurrentTriggerData;
	CurrentTriggerData	= NULL;

	elog(NOTICE, "RI_FKey_setdefault_upd() called\n");
	return NULL;
}





/* ----------
 * Local functions below
 * ----------
 */





/* ----------
 * ri_DetermineMatchType -
 *
 *	Convert the MATCH TYPE string into a switchable int
 * ----------
 */
static int
ri_DetermineMatchType(char *str)
{
	if (!strcmp(str, "UNSPECIFIED"))
		return RI_MATCH_TYPE_UNSPECIFIED;
	if (!strcmp(str, "FULL"))
		return RI_MATCH_TYPE_FULL;
	if (!strcmp(str, "PARTIAL"))
		return RI_MATCH_TYPE_PARTIAL;

	elog(ERROR, "unrecognized referential integrity MATCH type '%s'", str);
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
	int 				i;
	int					j;
	int					fno;

	/* ----------
	 * Initialize the key and fill in type, oid's and number of keypairs
	 * ----------
	 */
	memset ((void *)key, 0, sizeof(RI_QueryKey));
	key->constr_type	= RI_MATCH_TYPE_FULL;
	key->constr_id		= constr_id;
	key->constr_queryno	= constr_queryno;
	key->fk_relid		= fk_rel->rd_id;
	key->pk_relid		= pk_rel->rd_id;
	key->nkeypairs		= (argc - RI_FIRST_ATTNAME_ARGNO) / 2;

	/* ----------
	 * Lookup the attribute numbers of the arguments to the trigger call
	 * and fill in the keypairs.
	 * ----------
	 */
	for (i = 0, j = RI_FIRST_ATTNAME_ARGNO; j < argc; i++, j += 2)
	{
		fno = SPI_fnumber(fk_rel->rd_att, argv[j]);
		if (fno == SPI_ERROR_NOATTRIBUTE)
			elog(ERROR, "constraint %s: table %s does not have an attribute %s",
					argv[RI_CONSTRAINT_NAME_ARGNO],
					argv[RI_FK_RELNAME_ARGNO],
					argv[j]);
		key->keypair[i][RI_KEYPAIR_FK_IDX] = fno;

		fno = SPI_fnumber(pk_rel->rd_att, argv[j + 1]);
		if (fno == SPI_ERROR_NOATTRIBUTE)
			elog(ERROR, "constraint %s: table %s does not have an attribute %s",
					argv[RI_CONSTRAINT_NAME_ARGNO],
					argv[RI_PK_RELNAME_ARGNO],
					argv[j + 1]);
		key->keypair[i][RI_KEYPAIR_PK_IDX] = fno;
	}

	return;
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
	int				i;
	bool			isnull;
	bool			allnull  = true;
	bool			nonenull = true;

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
 *	query plans and equal operators.
 * ----------
 */
static void
ri_InitHashTables(void)
{
	HASHCTL		ctl;

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize		= sizeof(RI_QueryKey);
	ctl.datasize	= sizeof(void *);
	ri_query_cache = hash_create(RI_INIT_QUERYHASHSIZE, &ctl, HASH_ELEM);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize		= sizeof(Oid);
	ctl.datasize	= sizeof(Oid) + sizeof(FmgrInfo);
	ctl.hash		= tag_hash;
	ri_opreq_cache = hash_create(RI_INIT_OPREQHASHSIZE, &ctl, 
												HASH_ELEM | HASH_FUNCTION);
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
	RI_QueryHashEntry  *entry;
	bool				found;

	/* ----------
	 * On the first call initialize the hashtable
	 * ----------
	 */
	if (!ri_query_cache)
		ri_InitHashTables();

	/* ----------
	 * Lookup for the key
	 * ----------
	 */
	entry = (RI_QueryHashEntry *)hash_search(ri_query_cache, 
										(char *)key, HASH_FIND, &found);
	if (entry == NULL)
		elog(FATAL, "error in RI plan cache");
	if (!found)
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
	RI_QueryHashEntry  *entry;
	bool				found;

	/* ----------
	 * On the first call initialize the hashtable
	 * ----------
	 */
	if (!ri_query_cache)
		ri_InitHashTables();

	/* ----------
	 * Add the new plan.
	 * ----------
	 */
	entry = (RI_QueryHashEntry *)hash_search(ri_query_cache, 
										(char *)key, HASH_ENTER, &found);
	if (entry == NULL)
		elog(FATAL, "can't insert into RI plan cache");
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
	int				i;
	Oid				typeid;
	Datum			oldvalue;
	Datum			newvalue;
	bool			isnull;

	for (i = 0; i < key->nkeypairs; i++)
	{
		/* ----------
		 * Get one attributes oldvalue. If it is NULL - they're not equal.
		 * ----------
		 */
		oldvalue = SPI_getbinval(oldtup, rel->rd_att, 
									key->keypair[i][pairidx], &isnull);
		if (isnull)
			return false;

		/* ----------
		 * Get one attributes oldvalue. If it is NULL - they're not equal.
		 * ----------
		 */
		newvalue = SPI_getbinval(newtup, rel->rd_att, 
									key->keypair[i][pairidx], &isnull);
		if (isnull)
			return false;

		/* ----------
		 * Get the attributes type OID and call the '=' operator
		 * to compare the values.
		 * ----------
		 */
		typeid = SPI_gettypeid(rel->rd_att, key->keypair[i][pairidx]);
		if (!ri_AttributesEqual(typeid, oldvalue, newvalue))
			return false;
	}

	return true;
}


/* ----------
 * ri_AttributesEqual -
 *
 *	Call the type specific '=' operator comparision function
 *	for two values.
 * ----------
 */
static bool
ri_AttributesEqual(Oid typeid, Datum oldvalue, Datum newvalue)
{
	RI_OpreqHashEntry	   *entry;
	bool					found;
	Datum					result;

	/* ----------
	 * On the first call initialize the hashtable
	 * ----------
	 */
	if (!ri_query_cache)
		ri_InitHashTables();

	/* ----------
	 * Try to find the '=' operator for this type in our cache
	 * ----------
	 */
	entry = (RI_OpreqHashEntry *)hash_search(ri_opreq_cache,
										(char *)&typeid, HASH_FIND, &found);
	if (entry == NULL)
		elog(FATAL, "error in RI operator cache");

	/* ----------
	 * If not found, lookup the OPRNAME system cache for it
	 * and remember that info.
	 * ----------
	 */
	if (!found)
	{
		HeapTuple			opr_tup;
		Form_pg_operator	opr_struct;

		opr_tup = SearchSysCacheTuple(OPRNAME,
							PointerGetDatum("="),
							ObjectIdGetDatum(typeid),
							ObjectIdGetDatum(typeid),
							CharGetDatum('b'));

		if (!HeapTupleIsValid(opr_tup))
			elog(ERROR, "ri_AttributesEqual(): cannot find '=' operator "
						"for type %d", typeid);
		opr_struct = (Form_pg_operator) GETSTRUCT(opr_tup);

		entry = (RI_OpreqHashEntry *)hash_search(ri_opreq_cache,
										(char *)&typeid, HASH_ENTER, &found);
		if (entry == NULL)
			elog(FATAL, "can't insert into RI operator cache");

		entry->oprfnid = opr_struct->oprcode;
		memset(&(entry->oprfmgrinfo), 0, sizeof(FmgrInfo));
	}

	/* ----------
	 * Call the type specific '=' function
	 * ----------
	 */
	fmgr_info(entry->oprfnid, &(entry->oprfmgrinfo));
	result = (Datum)(*fmgr_faddr(&(entry->oprfmgrinfo)))(oldvalue, newvalue);
	return (bool)result;
}


