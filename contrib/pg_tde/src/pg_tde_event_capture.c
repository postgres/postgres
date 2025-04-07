/*-------------------------------------------------------------------------
 *
 * pg_tde_event_capture.c
 *      event trigger logic to identify if we are creating the encrypted table or not.
 *
 * IDENTIFICATION
 *    contrib/pg_tde/src/pg_tde_event_trigger.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "funcapi.h"
#include "fmgr.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "commands/sequence.h"
#include "access/table.h"
#include "access/relation.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/namespace.h"
#include "commands/event_trigger.h"
#include "common/pg_tde_utils.h"
#include "pg_tde_event_capture.h"
#include "pg_tde_guc.h"
#include "catalog/tde_principal_key.h"
#include "miscadmin.h"
#include "access/tableam.h"
#include "catalog/tde_global_space.h"

/* Global variable that gets set at ddl start and cleard out at ddl end*/
static TdeCreateEvent tdeCurrentCreateEvent = {.tid = {.value = 0},.relation = NULL};
static bool alterSetAccessMethod = false;

static void reset_current_tde_create_event(void);
static Oid	get_tde_table_am_oid(void);

PG_FUNCTION_INFO_V1(pg_tde_ddl_command_start_capture);
PG_FUNCTION_INFO_V1(pg_tde_ddl_command_end_capture);

TdeCreateEvent *
GetCurrentTdeCreateEvent(void)
{
	return &tdeCurrentCreateEvent;
}

static void
checkPrincipalKeyConfigured(void)
{
	if (!pg_tde_principal_key_configured(MyDatabaseId))
		ereport(ERROR,
				errmsg("principal key not configured"),
				errhint("create one using pg_tde_set_key before using encrypted tables"));
}

static void
checkEncryptionClause(const char *accessMethod)
{
	if (accessMethod && strcmp(accessMethod, "tde_heap") == 0)
	{
		tdeCurrentCreateEvent.encryptMode = true;
	}
	else if (accessMethod == NULL && strcmp(default_table_access_method, "tde_heap") == 0)
	{
		tdeCurrentCreateEvent.encryptMode = true;
	}

	if (tdeCurrentCreateEvent.encryptMode)
	{
		checkPrincipalKeyConfigured();
	}
	else if (EnforceEncryption)
	{
		ereport(ERROR,
				errmsg("pg_tde.enforce_encryption is ON, only the tde_heap access method is allowed."));
	}
}

void
validateCurrentEventTriggerState(bool mightStartTransaction)
{
	FullTransactionId tid = mightStartTransaction ? GetCurrentFullTransactionId() : GetCurrentFullTransactionIdIfAny();

	if (RecoveryInProgress())
	{
		reset_current_tde_create_event();
		return;
	}
	if (tdeCurrentCreateEvent.tid.value != InvalidFullTransactionId.value && tid.value != tdeCurrentCreateEvent.tid.value)
	{
		/* There was a failed query, end event trigger didn't execute */
		reset_current_tde_create_event();
	}
}

/*
 * pg_tde_ddl_command_start_capture is an event trigger function triggered
 * at the start of any DDL command execution.
 *
 * The function specifically focuses on CREATE INDEX and CREATE TABLE statements,
 * aiming to determine if the create table or the table on which an index is being created
 * utilizes the pg_tde access method for encryption.
 * Once it confirms the table's encryption requirement or usage,
 * it updates the table information in the tdeCurrentCreateEvent global variable.
 * This information can be accessed by SMGR or any other component
 * during the execution of this DDL statement.
 */
Datum
pg_tde_ddl_command_start_capture(PG_FUNCTION_ARGS)
{
	EventTriggerData *trigdata;
	Node	   *parsetree;

	/* Ensure this function is being called as an event trigger */
	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))	/* internal error */
		ereport(ERROR,
				errmsg("Function can only be fired by event trigger manager"));

	trigdata = (EventTriggerData *) fcinfo->context;
	parsetree = trigdata->parsetree;

	if (IsA(parsetree, IndexStmt))
	{
		IndexStmt  *stmt = (IndexStmt *) parsetree;
		Oid			relationId = RangeVarGetRelid(stmt->relation, AccessShareLock, true);

		validateCurrentEventTriggerState(true);
		tdeCurrentCreateEvent.tid = GetCurrentFullTransactionId();

		tdeCurrentCreateEvent.baseTableOid = relationId;
		tdeCurrentCreateEvent.relation = stmt->relation;

		if (relationId != InvalidOid)
		{
			Relation	rel = table_open(relationId, NoLock);

			if (rel->rd_rel->relam == get_tde_table_am_oid())
			{
				/* We are creating the index on encrypted table */
				/* set the global state */
				tdeCurrentCreateEvent.encryptMode = true;
			}

			table_close(rel, NoLock);

			if (tdeCurrentCreateEvent.encryptMode)
				checkPrincipalKeyConfigured();
		}
		else
			ereport(DEBUG1, errmsg("Failed to get relation Oid for relation:%s", stmt->relation->relname));

	}
	else if (IsA(parsetree, CreateStmt))
	{
		CreateStmt *stmt = (CreateStmt *) parsetree;
		const char *accessMethod = stmt->accessMethod;

		validateCurrentEventTriggerState(true);
		tdeCurrentCreateEvent.tid = GetCurrentFullTransactionId();


		tdeCurrentCreateEvent.relation = stmt->relation;

		checkEncryptionClause(accessMethod);
	}
	else if (IsA(parsetree, CreateTableAsStmt))
	{
		CreateTableAsStmt *stmt = (CreateTableAsStmt *) parsetree;
		const char *accessMethod = stmt->into->accessMethod;

		validateCurrentEventTriggerState(true);
		tdeCurrentCreateEvent.tid = GetCurrentFullTransactionId();

		tdeCurrentCreateEvent.relation = stmt->into->rel;

		checkEncryptionClause(accessMethod);
	}
	else if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *) parsetree;
		ListCell   *lcmd;
		Oid			relationId = RangeVarGetRelid(stmt->relation, AccessShareLock, true);

		validateCurrentEventTriggerState(true);
		tdeCurrentCreateEvent.tid = GetCurrentFullTransactionId();

		foreach(lcmd, stmt->cmds)
		{
			AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

			if (cmd->subtype == AT_SetAccessMethod)
			{
				const char *accessMethod = cmd->name;

				tdeCurrentCreateEvent.relation = stmt->relation;
				tdeCurrentCreateEvent.baseTableOid = relationId;
				tdeCurrentCreateEvent.alterAccessMethodMode = true;

				checkEncryptionClause(accessMethod);
				alterSetAccessMethod = true;
			}
		}

		if (!alterSetAccessMethod)
		{
			/*
			 * With a SET ACCESS METHOD clause, use that as the basis for
			 * decisions. But if it's not present, look up encryption status
			 * of the table.
			 */

			tdeCurrentCreateEvent.baseTableOid = relationId;
			tdeCurrentCreateEvent.relation = stmt->relation;

			if (relationId != InvalidOid)
			{
				Relation	rel = relation_open(relationId, NoLock);

				if (rel->rd_rel->relam == get_tde_table_am_oid())
				{
					/*
					 * We are altering an encrypted table ALTER TABLE can
					 * create possibly new files set the global state
					 */
					tdeCurrentCreateEvent.encryptMode = true;
				}

				relation_close(rel, NoLock);

				if (tdeCurrentCreateEvent.encryptMode)
					checkPrincipalKeyConfigured();
			}
		}
	}
	else
	{
		if (!tdeCurrentCreateEvent.alterAccessMethodMode)
		{
			/*
			 * Any other type of statement doesn't need TDE mode, except
			 * during alter access method. To make sure that we have no
			 * leftover setting from a previous error or something, we just
			 * reset the status here.
			 */
			reset_current_tde_create_event();
		}
	}
	PG_RETURN_NULL();
}

/*
 * trigger function called at the end of DDL statement execution.
 * It just clears the tdeCurrentCreateEvent global variable.
 */
Datum
pg_tde_ddl_command_end_capture(PG_FUNCTION_ARGS)
{
	EventTriggerData *trigdata;
	Node	   *parsetree;

	trigdata = (EventTriggerData *) fcinfo->context;
	parsetree = trigdata->parsetree;

	/* Ensure this function is being called as an event trigger */
	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))	/* internal error */
		ereport(ERROR,
				errmsg("Function can only be fired by event trigger manager"));

	if (IsA(parsetree, AlterTableStmt) && tdeCurrentCreateEvent.alterAccessMethodMode)
	{
		/*
		 * sequences are not updated automatically so force rewrite by
		 * updating their persistence to be the same as before.
		 */
		List	   *seqlist = getOwnedSequences(tdeCurrentCreateEvent.baseTableOid);
		ListCell   *lc;
		Relation	rel = relation_open(tdeCurrentCreateEvent.baseTableOid, NoLock);
		char		persistence = rel->rd_rel->relpersistence;

		relation_close(rel, NoLock);

		foreach(lc, seqlist)
		{
			Oid			seq_relid = lfirst_oid(lc);

			SequenceChangePersistence(seq_relid, persistence);
		}

		tdeCurrentCreateEvent.alterAccessMethodMode = false;
	}

	/*
	 * All we need to do is to clear the event state. Except when we are in
	 * alter access method mode, because during that, we have multiple nested
	 * event trigger running. Reset should only be called in the end, when it
	 * is set to false.
	 */
	if (!tdeCurrentCreateEvent.alterAccessMethodMode)
	{
		reset_current_tde_create_event();
	}

	PG_RETURN_NULL();
}

static void
reset_current_tde_create_event(void)
{
	tdeCurrentCreateEvent.encryptMode = false;
	tdeCurrentCreateEvent.baseTableOid = InvalidOid;
	tdeCurrentCreateEvent.relation = NULL;
	tdeCurrentCreateEvent.tid = InvalidFullTransactionId;
	alterSetAccessMethod = false;
	tdeCurrentCreateEvent.alterAccessMethodMode = false;
}

static Oid
get_tde_table_am_oid(void)
{
	return get_table_am_oid("tde_heap", false);
}
