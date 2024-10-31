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
#include "access/table.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/namespace.h"
#include "commands/event_trigger.h"
#include "common/pg_tde_utils.h"
#include "pg_tde_event_capture.h"
#include "commands/tablespace.h"
#include "catalog/tde_principal_key.h"
#include "miscadmin.h"
#include "access/tableam.h"

/* Global variable that gets set at ddl start and cleard out at ddl end*/
TdeCreateEvent tdeCurrentCreateEvent = {.relation = NULL};


static void reset_current_tde_create_event(void);

PG_FUNCTION_INFO_V1(pg_tde_ddl_command_start_capture);
PG_FUNCTION_INFO_V1(pg_tde_ddl_command_end_capture);

TdeCreateEvent *
GetCurrentTdeCreateEvent(void)
{
	return &tdeCurrentCreateEvent;
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
	/* TODO: verify update_compare_indexes failure related to this */
#ifdef PERCONA_EXT
	EventTriggerData *trigdata;
	Node	   *parsetree;

	/* Ensure this function is being called as an event trigger */
	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))	/* internal error */
		ereport(ERROR,
				(errmsg("Function can only be fired by event trigger manager")));

	trigdata = (EventTriggerData *) fcinfo->context;
	parsetree = trigdata->parsetree;

	elog(LOG, "EVENT TRIGGER (%s) %s", trigdata->event, nodeToString(parsetree));
	reset_current_tde_create_event();

	if (IsA(parsetree, IndexStmt))
	{
		IndexStmt  *stmt = (IndexStmt *) parsetree;
		Oid			relationId = RangeVarGetRelid(stmt->relation, NoLock, true);

		tdeCurrentCreateEvent.eventType = TDE_INDEX_CREATE_EVENT;
		tdeCurrentCreateEvent.baseTableOid = relationId;
		tdeCurrentCreateEvent.relation = stmt->relation;

		if (relationId != InvalidOid)
		{
			LOCKMODE	lockmode = AccessShareLock; /* TODO. Verify lock mode? */
			Relation	rel = table_open(relationId, lockmode);

			if (rel->rd_rel->relam == get_tde_table_am_oid())
			{
				/* We are creating the index on encrypted table */
				/* set the global state */
				tdeCurrentCreateEvent.encryptMode = true;
			}
			table_close(rel, lockmode);
		}
		else
			ereport(DEBUG1, (errmsg("Failed to get relation Oid for relation:%s", stmt->relation->relname)));

	}
	else if (IsA(parsetree, CreateStmt))
	{
		CreateStmt *stmt = (CreateStmt *) parsetree;
		TDEPrincipalKey * principal_key;
		Oid tablespace_oid;

		tdeCurrentCreateEvent.eventType = TDE_TABLE_CREATE_EVENT;
		tdeCurrentCreateEvent.relation = stmt->relation;

		if (stmt->accessMethod && strcmp(stmt->accessMethod, "tde_heap") == 0)
		{
			tdeCurrentCreateEvent.encryptMode = true;
		} else if ((stmt->accessMethod == NULL || stmt->accessMethod[0] ==0) && strcmp(default_table_access_method, "tde_heap") == 0)
		{
			tdeCurrentCreateEvent.encryptMode = true;
		}

		if(tdeCurrentCreateEvent.encryptMode)
		{
			tablespace_oid = stmt->tablespacename != NULL ? get_tablespace_oid(stmt->tablespacename, false) 
							 : MyDatabaseTableSpace;  
			LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);
			principal_key = GetPrincipalKey(MyDatabaseId, tablespace_oid, LW_SHARED);
			LWLockRelease(tde_lwlock_enc_keys());
			if (principal_key == NULL)
			{
				ereport(ERROR,
						(errmsg("failed to retrieve principal key. Create one using pg_tde_set_principal_key before using encrypted tables.")));

			}
		}

	}
	else if (IsA(parsetree, AlterTableStmt))
	{
		LOCKMODE	lockmode = AccessShareLock; /* TODO. Verify lock mode? */
		AlterTableStmt *stmt = (AlterTableStmt *) parsetree;
		ListCell   *lcmd;

		foreach(lcmd, stmt->cmds)
		{
			AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
			if (cmd->subtype == AT_SetAccessMethod && 
				((cmd->name != NULL && strcmp(cmd->name, "tde_heap")==0) ||
				 (cmd->name == NULL && strcmp(default_table_access_method, "tde_heap") == 0))
				)
			{
				tdeCurrentCreateEvent.encryptMode = true;
				tdeCurrentCreateEvent.eventType = TDE_TABLE_CREATE_EVENT;
				tdeCurrentCreateEvent.relation = stmt->relation;
			}
		}

		// TODO: also check for tablespace change, if current or new AM is tde_heap!

		if (tdeCurrentCreateEvent.encryptMode)
		{
			TDEPrincipalKey * principal_key;
			Oid		relationId = RangeVarGetRelid(stmt->relation, NoLock, true);
			Relation	rel = table_open(relationId, lockmode);
			Oid tablespace_oid = rel->rd_locator.spcOid;
			table_close(rel, lockmode);

			LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);
			principal_key = GetPrincipalKey(MyDatabaseId, tablespace_oid, LW_SHARED);
			LWLockRelease(tde_lwlock_enc_keys());
			if (principal_key == NULL)
			{
				ereport(ERROR,
						(errmsg("failed to retrieve principal key. Create one using pg_tde_set_principal_key before using encrypted tables.")));

			}
		}
	}
#endif
	PG_RETURN_NULL();
}

/*
 * trigger function called at the end of DDL statement execution.
 * It just clears the tdeCurrentCreateEvent global variable.
 */
Datum
pg_tde_ddl_command_end_capture(PG_FUNCTION_ARGS)
{
#ifdef PERCONA_EXT
	/* Ensure this function is being called as an event trigger */
	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))	/* internal error */
		ereport(ERROR,
				(errmsg("Function can only be fired by event trigger manager")));

	elog(LOG, "Type:%s EncryptMode:%s, Oid:%d, Relation:%s ",
		 (tdeCurrentCreateEvent.eventType == TDE_INDEX_CREATE_EVENT) ? "CREATE INDEX" :
		 (tdeCurrentCreateEvent.eventType == TDE_TABLE_CREATE_EVENT) ? "CREATE TABLE" : "UNKNOWN",
		 tdeCurrentCreateEvent.encryptMode ? "true" : "false",
		 tdeCurrentCreateEvent.baseTableOid,
		 tdeCurrentCreateEvent.relation ? tdeCurrentCreateEvent.relation->relname : "UNKNOWN");

	/* All we need to do is to clear the event state */
	reset_current_tde_create_event();
#endif
	PG_RETURN_NULL();
}

static void
reset_current_tde_create_event(void)
{
	tdeCurrentCreateEvent.encryptMode = false;
	tdeCurrentCreateEvent.eventType = TDE_UNKNOWN_CREATE_EVENT;
	tdeCurrentCreateEvent.baseTableOid = InvalidOid;
	tdeCurrentCreateEvent.relation = NULL;
}
