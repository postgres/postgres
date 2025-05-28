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

#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_inherits.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/sequence.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "access/pg_tde_tdemap.h"
#include "catalog/tde_global_space.h"
#include "catalog/tde_principal_key.h"
#include "common/pg_tde_utils.h"
#include "pg_tde_event_capture.h"
#include "pg_tde_guc.h"

typedef struct
{
	Node	   *parsetree;
	TDEEncryptMode encryptMode;
	Oid			rebuildSequencesFor;
	Oid			rebuildSequence;
} TdeDdlEvent;

static FullTransactionId ddlEventStackTid = {0};
static List *ddlEventStack = NIL;

static Oid	get_db_oid(const char *name);
static Oid	get_tde_table_am_oid(void);

PG_FUNCTION_INFO_V1(pg_tde_ddl_command_start_capture);
PG_FUNCTION_INFO_V1(pg_tde_ddl_command_end_capture);

static TDEEncryptMode
currentTdeEncryptMode(void)
{
	if (ddlEventStack == NIL)
		return TDE_ENCRYPT_MODE_RETAIN;
	else
		return ((TdeDdlEvent *) llast(ddlEventStack))->encryptMode;
}

/*
 * Make sure that even if a statement failed, and an event trigger end
 * trigger didn't fire, we don't accidentaly create encrypted files when
 * we don't have to.
 */
TDEEncryptMode
currentTdeEncryptModeValidated(void)
{
	if (!FullTransactionIdEquals(ddlEventStackTid, GetCurrentFullTransactionIdIfAny()))
		return TDE_ENCRYPT_MODE_RETAIN;

	return currentTdeEncryptMode();
}

static bool
shouldEncryptTable(const char *accessMethod)
{
	if (accessMethod)
		return strcmp(accessMethod, "tde_heap") == 0;
	else
		return strcmp(default_table_access_method, "tde_heap") == 0;
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
checkEncryptionStatus(void)
{
	if (currentTdeEncryptMode() == TDE_ENCRYPT_MODE_ENCRYPT)
	{
		checkPrincipalKeyConfigured();
	}
	else if (EnforceEncryption)
	{
		ereport(ERROR,
				errmsg("pg_tde.enforce_encryption is ON, only the tde_heap access method is allowed."));
	}
}

static void
verify_event_stack(void)
{
	FullTransactionId tid = GetCurrentFullTransactionId();

	if (!FullTransactionIdEquals(ddlEventStackTid, tid))
	{
		ListCell   *lc;

		foreach(lc, ddlEventStack)
			pfree(lfirst(lc));

		ddlEventStack = NIL;
		ddlEventStackTid = tid;
	}
}

static void
push_event_stack(const TdeDdlEvent *event)
{
	MemoryContext oldCtx;
	TdeDdlEvent *e;

	verify_event_stack();

	oldCtx = MemoryContextSwitchTo(TopMemoryContext);
	e = palloc_object(TdeDdlEvent);
	*e = *event;
	ddlEventStack = lappend(ddlEventStack, e);
	MemoryContextSwitchTo(oldCtx);
}

static List *
find_typed_table_dependencies(Oid typeOid)
{
	Relation	classRel;
	ScanKeyData key[1];
	TableScanDesc scan;
	HeapTuple	tuple;
	List	   *result = NIL;

	classRel = table_open(RelationRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_class_reloftype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(typeOid));

	scan = table_beginscan_catalog(classRel, 1, key);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classform = (Form_pg_class) GETSTRUCT(tuple);

		LockRelationOid(classform->oid, AccessShareLock);
		result = lappend_oid(result, classform->oid);
	}

	table_endscan(scan);
	table_close(classRel, AccessShareLock);

	return result;
}

typedef enum
{
	ENC_MIX_UNKNOWN,
	ENC_MIX_PLAIN,
	ENC_MIX_ENCRYPTED,
	ENC_MIX_MIXED,
} EncryptionMix;

/*
 * Since ALTER TABLE can modify multiple tables due to inheritance or typed
 * tables which can for example result in TOAST tables being created for some
 * or all of the modified tables while the event trigger is only fired once we
 * cannot rely on the event stack to make sure we get the correct encryption
 * status.
 *
 * Our solution is to be cautious and only modify tables when all tables with
 * storage are either encrypted or not encrypted. If there is a mix we will
 * throw and error. The result of this is also used to properly inform the
 * SMGR of the current encryption status.
 */
static EncryptionMix
alter_table_encryption_mix(Oid relid)
{
	EncryptionMix enc = ENC_MIX_UNKNOWN;
	Relation	rel;
	List	   *children;
	ListCell   *lc;

	rel = relation_open(relid, NoLock);

	if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		enc = rel->rd_rel->relam == get_tde_table_am_oid() ? ENC_MIX_ENCRYPTED : ENC_MIX_PLAIN;

	if (rel->rd_rel->relkind == RELKIND_COMPOSITE_TYPE)
		children = find_typed_table_dependencies(rel->rd_rel->reltype);
	else
		children = find_inheritance_children(relid, AccessShareLock);

	relation_close(rel, NoLock);

	foreach(lc, children)
	{
		Oid			childid = lfirst_oid(lc);
		EncryptionMix childenc;

		childenc = alter_table_encryption_mix(childid);

		if (childenc != ENC_MIX_UNKNOWN)
		{
			if (enc == ENC_MIX_UNKNOWN)
				enc = childenc;
			else if (enc != childenc)
				return ENC_MIX_MIXED;
		}
	}

	return enc;
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

	trigdata = castNode(EventTriggerData, fcinfo->context);
	parsetree = trigdata->parsetree;

	if (IsA(parsetree, IndexStmt))
	{
		IndexStmt  *stmt = castNode(IndexStmt, parsetree);
		Relation	rel;
		TdeDdlEvent event = {.parsetree = parsetree};

		rel = table_openrv(stmt->relation, AccessShareLock);

		if (rel->rd_rel->relam == get_tde_table_am_oid())
		{
			event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
			checkPrincipalKeyConfigured();
		}
		else
			event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;

		/* Hold on to lock until end of transaction */
		table_close(rel, NoLock);

		push_event_stack(&event);
	}
	else if (IsA(parsetree, CreateStmt))
	{
		CreateStmt *stmt = castNode(CreateStmt, parsetree);
		bool		foundAccessMethod = false;
		TdeDdlEvent event = {.parsetree = parsetree};

		if (stmt->accessMethod)
		{
			foundAccessMethod = true;
			if (strcmp(stmt->accessMethod, "tde_heap") == 0)
				event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
			else
				event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;
		}
		else if (stmt->partbound)
		{
			/*
			 * If no access method is specified, and this is a partition of a
			 * parent table, access method can be inherited from the parent
			 * table if it has one set.
			 *
			 * AccessExclusiveLock might seem excessive, but it's what
			 * DefineRelation() will take on any partitioned parent relation
			 * in this transaction anyway.
			 */
			Oid			parentOid;
			Oid			parentAmOid;

			Assert(list_length(stmt->inhRelations) == 1);

			parentOid = RangeVarGetRelid(linitial(stmt->inhRelations),
										 AccessExclusiveLock,
										 false);
			parentAmOid = get_rel_relam(parentOid);
			foundAccessMethod = parentAmOid != InvalidOid;

			if (foundAccessMethod)
			{
				if (parentAmOid == get_tde_table_am_oid())
					event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
				else
					event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;
			}
		}

		if (!foundAccessMethod)
		{
			if (strcmp(default_table_access_method, "tde_heap") == 0)
				event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
			else
				event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;
		}

		push_event_stack(&event);
		checkEncryptionStatus();
	}
	else if (IsA(parsetree, CreateTableAsStmt))
	{
		CreateTableAsStmt *stmt = castNode(CreateTableAsStmt, parsetree);
		TdeDdlEvent event = {.parsetree = parsetree};

		if (shouldEncryptTable(stmt->into->accessMethod))
			event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
		else
			event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;

		push_event_stack(&event);
		checkEncryptionStatus();
	}
	else if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = castNode(AlterTableStmt, parsetree);
		Oid			relid = RangeVarGetRelid(stmt->relation, AccessShareLock, true);

		if (relid != InvalidOid)
		{
			AlterTableCmd *setAccessMethod = NULL;
			ListCell   *lcmd;
			TdeDdlEvent event = {.parsetree = parsetree};
			EncryptionMix encmix;

			foreach(lcmd, stmt->cmds)
			{
				AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lcmd);

				if (cmd->subtype == AT_SetAccessMethod)
					setAccessMethod = cmd;
			}

			encmix = alter_table_encryption_mix(relid);

			/*
			 * This check is very braod and could be limited only to commands
			 * which recurse to child tables or to those which may create new
			 * relfilenodes, but this restrictive code is good enough for now.
			 */
			if (encmix == ENC_MIX_MIXED)
			{
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("Recursive ALTER TABLE on a mix of encrypted and unencrypted relations is not supported"));
			}

			/*
			 * With a SET ACCESS METHOD clause, use that as the basis for
			 * decisions. But if it's not present, look up encryption status
			 * of the table.
			 */
			if (setAccessMethod)
			{
				event.rebuildSequencesFor = relid;

				if (shouldEncryptTable(setAccessMethod->name))
					event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
				else
					event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;
			}
			else
			{
				if (encmix == ENC_MIX_ENCRYPTED)
				{
					event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
					checkPrincipalKeyConfigured();
				}
				else if (encmix == ENC_MIX_PLAIN)
					event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;
			}

			push_event_stack(&event);
			checkEncryptionStatus();
		}
	}
	else if (IsA(parsetree, CreateSeqStmt))
	{
		CreateSeqStmt *stmt = (CreateSeqStmt *) parsetree;
		ListCell   *option;
		List	   *owned_by = NIL;
		TdeDdlEvent event = {.parsetree = parsetree};

		foreach(option, stmt->options)
		{
			DefElem    *defel = lfirst_node(DefElem, option);

			if (strcmp(defel->defname, "owned_by") == 0)
			{
				owned_by = defGetQualifiedName(defel);
				break;
			}
		}

		if (list_length(owned_by) > 1)
		{
			List	   *relname;
			RangeVar   *rel;
			Relation	tablerel;

			relname = list_copy_head(owned_by, list_length(owned_by) - 1);

			/* Open and lock rel to ensure it won't go away meanwhile */
			rel = makeRangeVarFromNameList(relname);
			tablerel = relation_openrv(rel, AccessShareLock);

			if (tablerel->rd_rel->relam == get_tde_table_am_oid())
			{
				event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
				checkPrincipalKeyConfigured();
			}
			else
				event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;

			/* Hold lock until end of transaction */
			relation_close(tablerel, NoLock);
		}
		else
			event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;

		push_event_stack(&event);
	}
	else if (IsA(parsetree, AlterSeqStmt))
	{
		AlterSeqStmt *stmt = (AlterSeqStmt *) parsetree;
		ListCell   *option;
		List	   *owned_by = NIL;
		TdeDdlEvent event = {.parsetree = parsetree};
		Oid			relid = RangeVarGetRelid(stmt->sequence, AccessShareLock, true);

		if (relid != InvalidOid)
		{
			foreach(option, stmt->options)
			{
				DefElem    *defel = lfirst_node(DefElem, option);

				if (strcmp(defel->defname, "owned_by") == 0)
				{
					owned_by = defGetQualifiedName(defel);
					break;
				}
			}

			if (list_length(owned_by) > 1)
			{
				List	   *relname;
				RangeVar   *rel;
				Relation	tablerel;

				event.rebuildSequence = relid;

				relname = list_copy_head(owned_by, list_length(owned_by) - 1);

				/* Open and lock rel to ensure it won't go away meanwhile */
				rel = makeRangeVarFromNameList(relname);
				tablerel = relation_openrv(rel, AccessShareLock);

				if (tablerel->rd_rel->relam == get_tde_table_am_oid())
				{
					event.encryptMode = TDE_ENCRYPT_MODE_ENCRYPT;
					checkPrincipalKeyConfigured();
				}
				else
					event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;

				/* Hold lock until end of transaction */
				relation_close(tablerel, NoLock);
			}
			else if (list_length(owned_by) == 1)
			{
				event.rebuildSequence = relid;

				event.encryptMode = TDE_ENCRYPT_MODE_PLAIN;
			}

			push_event_stack(&event);
		}
	}

	PG_RETURN_VOID();
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
	TdeDdlEvent *event;

	/* Ensure this function is being called as an event trigger */
	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))	/* internal error */
		ereport(ERROR,
				errmsg("Function can only be fired by event trigger manager"));

	trigdata = castNode(EventTriggerData, fcinfo->context);
	parsetree = trigdata->parsetree;

	if (ddlEventStack == NIL || ((TdeDdlEvent *) llast(ddlEventStack))->parsetree != parsetree)
		PG_RETURN_VOID();

	event = (TdeDdlEvent *) llast(ddlEventStack);

	if (event->rebuildSequencesFor != InvalidOid)
	{
		/*
		 * sequences are not updated automatically so force rewrite by
		 * updating their persistence to be the same as before.
		 */
		List	   *seqlist = getOwnedSequences(event->rebuildSequencesFor);
		ListCell   *lc;
		Relation	rel = relation_open(event->rebuildSequencesFor, NoLock);
		char		persistence = rel->rd_rel->relpersistence;

		relation_close(rel, NoLock);

		foreach(lc, seqlist)
		{
			Oid			seq_relid = lfirst_oid(lc);

			SequenceChangePersistence(seq_relid, persistence);
		}
	}

	if (event->rebuildSequence != InvalidOid)
	{
		/*
		 * Seqeunces are not rewritten when just changing owner so force a
		 * rewrite. There is a small risk of extra overhead if someone changes
		 * sequence owner and something else at the same time.
		 */
		Relation	rel = relation_open(event->rebuildSequence, NoLock);
		char		persistence = rel->rd_rel->relpersistence;

		relation_close(rel, NoLock);

		SequenceChangePersistence(event->rebuildSequence, persistence);
	}

	ddlEventStack = list_delete_last(ddlEventStack);
	pfree(event);

	PG_RETURN_VOID();
}

static Oid
get_tde_table_am_oid(void)
{
	return get_table_am_oid("tde_heap", false);
}

static ProcessUtility_hook_type next_ProcessUtility_hook = NULL;

/*
 * Handles utility commands which we cannot handle in the event trigger.
 */
static void
pg_tde_proccess_utility(PlannedStmt *pstmt,
						const char *queryString,
						bool readOnlyTree,
						ProcessUtilityContext context,
						ParamListInfo params,
						QueryEnvironment *queryEnv,
						DestReceiver *dest,
						QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;

	switch (nodeTag(parsetree))
	{
		case T_CreatedbStmt:
			{
				CreatedbStmt *stmt = castNode(CreatedbStmt, parsetree);
				ListCell   *option;
				char	   *dbtemplate = "template1";
				char	   *strategy = "wal_log";

				foreach(option, stmt->options)
				{
					DefElem    *defel = lfirst_node(DefElem, option);

					if (strcmp(defel->defname, "template") == 0)
						dbtemplate = defGetString(defel);
					else if (strcmp(defel->defname, "strategy") == 0)
						strategy = defGetString(defel);
				}

				if (pg_strcasecmp(strategy, "file_copy") == 0)
				{
					Oid			dbOid = get_db_oid(dbtemplate);

					if (dbOid != InvalidOid)
					{
						int			count;

						LWLockAcquire(tde_lwlock_enc_keys(), LW_SHARED);
						count = pg_tde_count_relations(dbOid);
						LWLockRelease(tde_lwlock_enc_keys());

						if (count > 0)
							ereport(ERROR,
									errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("The FILE_COPY strategy cannot be used when there are encrypted objects in the template database: %d objects found", count),
									errhint("Use the WAL_LOG strategy instead."));
					}
				}
			}
			break;
		default:
			break;
	}

	if (next_ProcessUtility_hook)
		(*next_ProcessUtility_hook) (pstmt, queryString, readOnlyTree,
									 context, params, queryEnv,
									 dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree,
								context, params, queryEnv,
								dest, qc);
}

void
TdeEventCaptureInit(void)
{
	next_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = pg_tde_proccess_utility;
}

/*
 * A stripped down version of get_db_info() from src/backend/commands/dbcommands.c
 */
static Oid
get_db_oid(const char *name)
{
	Oid			resDbOid = InvalidOid;
	Relation	relation;

	Assert(name);

	relation = table_open(DatabaseRelationId, AccessShareLock);

	/*
	 * Loop covers the rare case where the database is renamed before we can
	 * lock it.  We try again just in case we can find a new one of the same
	 * name.
	 */
	for (;;)
	{
		ScanKeyData scanKey;
		SysScanDesc scan;
		HeapTuple	tuple;
		Oid			dbOid;

		/*
		 * there's no syscache for database-indexed-by-name, so must do it the
		 * hard way
		 */
		ScanKeyInit(&scanKey,
					Anum_pg_database_datname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(name));

		scan = systable_beginscan(relation, DatabaseNameIndexId, true,
								  NULL, 1, &scanKey);

		tuple = systable_getnext(scan);

		if (!HeapTupleIsValid(tuple))
		{
			/* definitely no database of that name */
			systable_endscan(scan);
			break;
		}

		dbOid = ((Form_pg_database) GETSTRUCT(tuple))->oid;

		systable_endscan(scan);

		/*
		 * Now that we have a database OID, we can try to lock the DB.
		 */
		LockSharedObject(DatabaseRelationId, dbOid, 0, AccessExclusiveLock);

		/*
		 * And now, re-fetch the tuple by OID.  If it's still there and still
		 * the same name, we win; else, drop the lock and loop back to try
		 * again.
		 */
		tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbOid));
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);

			if (strcmp(name, NameStr(dbform->datname)) == 0)
			{
				resDbOid = dbOid;
				ReleaseSysCache(tuple);
				break;
			}
			/* can only get here if it was just renamed */
			ReleaseSysCache(tuple);
		}

		UnlockSharedObject(DatabaseRelationId, dbOid, 0, AccessExclusiveLock);
	}

	table_close(relation, AccessShareLock);

	return resDbOid;
}
