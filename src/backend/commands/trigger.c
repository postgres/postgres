/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/commands/trigger.c,v 1.210.2.7 2008/12/13 02:00:52 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"


static void InsertTrigger(TriggerDesc *trigdesc, Trigger *trigger, int indx);
static HeapTuple GetTupleForTrigger(EState *estate,
				   ResultRelInfo *relinfo,
				   ItemPointer tid,
				   CommandId cid,
				   TupleTableSlot **newSlot);
static HeapTuple ExecCallTriggerFunc(TriggerData *trigdata,
					int tgindx,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context);
static void AfterTriggerSaveEvent(ResultRelInfo *relinfo, int event,
					  bool row_trigger, HeapTuple oldtup, HeapTuple newtup);


/*
 * Create a trigger.  Returns the OID of the created trigger.
 *
 * forConstraint, if true, says that this trigger is being created to
 * implement a constraint.	The caller will then be expected to make
 * a pg_depend entry linking the trigger to that constraint (and thereby
 * to the owning relation(s)).
 */
Oid
CreateTrigger(CreateTrigStmt *stmt, bool forConstraint)
{
	int16		tgtype;
	int2vector *tgattr;
	Datum		values[Natts_pg_trigger];
	char		nulls[Natts_pg_trigger];
	Relation	rel;
	AclResult	aclresult;
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData key;
	Relation	pgrel;
	HeapTuple	tuple;
	Oid			fargtypes[1];	/* dummy */
	Oid			funcoid;
	Oid			funcrettype;
	Oid			trigoid;
	int			found = 0;
	int			i;
	char		constrtrigname[NAMEDATALEN];
	char	   *trigname;
	char	   *constrname;
	Oid			constrrelid = InvalidOid;
	ObjectAddress myself,
				referenced;

	rel = heap_openrv(stmt->relation, AccessExclusiveLock);

	if (stmt->constrrel != NULL)
		constrrelid = RangeVarGetRelid(stmt->constrrel, false);
	else if (stmt->isconstraint)
	{
		/*
		 * If this trigger is a constraint (and a foreign key one) then we
		 * really need a constrrelid.  Since we don't have one, we'll try to
		 * generate one from the argument information.
		 *
		 * This is really just a workaround for a long-ago pg_dump bug that
		 * omitted the FROM clause in dumped CREATE CONSTRAINT TRIGGER
		 * commands.  We don't want to bomb out completely here if we can't
		 * determine the correct relation, because that would prevent loading
		 * the dump file.  Instead, NOTICE here and ERROR in the trigger.
		 */
		bool		needconstrrelid = false;
		void	   *elem = NULL;

		if (strncmp(strVal(lfirst(list_tail((stmt->funcname)))), "RI_FKey_check_", 14) == 0)
		{
			/* A trigger on FK table. */
			needconstrrelid = true;
			if (list_length(stmt->args) > RI_PK_RELNAME_ARGNO)
				elem = list_nth(stmt->args, RI_PK_RELNAME_ARGNO);
		}
		else if (strncmp(strVal(lfirst(list_tail((stmt->funcname)))), "RI_FKey_", 8) == 0)
		{
			/* A trigger on PK table. */
			needconstrrelid = true;
			if (list_length(stmt->args) > RI_FK_RELNAME_ARGNO)
				elem = list_nth(stmt->args, RI_FK_RELNAME_ARGNO);
		}
		if (elem != NULL)
		{
			RangeVar   *rel = makeRangeVar(NULL, strVal(elem));

			constrrelid = RangeVarGetRelid(rel, true);
		}
		if (needconstrrelid && constrrelid == InvalidOid)
			ereport(NOTICE,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("could not determine referenced table for constraint \"%s\"",
							stmt->trigname)));
	}

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	/* permission checks */

	if (stmt->isconstraint)
	{
		/* foreign key constraint trigger */

		aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
									  ACL_REFERENCES);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));
		if (constrrelid != InvalidOid)
		{
			aclresult = pg_class_aclcheck(constrrelid, GetUserId(),
										  ACL_REFERENCES);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, ACL_KIND_CLASS,
							   get_rel_name(constrrelid));
		}
	}
	else
	{
		/* real trigger */
		aclresult = pg_class_aclcheck(RelationGetRelid(rel), GetUserId(),
									  ACL_TRIGGER);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, ACL_KIND_CLASS,
						   RelationGetRelationName(rel));
	}

	/*
	 * Generate the trigger's OID now, so that we can use it in the name if
	 * needed.
	 */
	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	trigoid = GetNewOid(tgrel);

	/*
	 * If trigger is an RI constraint, use specified trigger name as
	 * constraint name and build a unique trigger name instead. This is mainly
	 * for backwards compatibility with CREATE CONSTRAINT TRIGGER commands.
	 */
	if (stmt->isconstraint)
	{
		snprintf(constrtrigname, sizeof(constrtrigname),
				 "RI_ConstraintTrigger_%u", trigoid);
		trigname = constrtrigname;
		constrname = stmt->trigname;
	}
	else
	{
		trigname = stmt->trigname;
		constrname = "";
	}

	TRIGGER_CLEAR_TYPE(tgtype);
	if (stmt->before)
		TRIGGER_SETT_BEFORE(tgtype);
	if (stmt->row)
		TRIGGER_SETT_ROW(tgtype);

	for (i = 0; stmt->actions[i]; i++)
	{
		switch (stmt->actions[i])
		{
			case 'i':
				if (TRIGGER_FOR_INSERT(tgtype))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple INSERT events specified")));
				TRIGGER_SETT_INSERT(tgtype);
				break;
			case 'd':
				if (TRIGGER_FOR_DELETE(tgtype))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple DELETE events specified")));
				TRIGGER_SETT_DELETE(tgtype);
				break;
			case 'u':
				if (TRIGGER_FOR_UPDATE(tgtype))
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("multiple UPDATE events specified")));
				TRIGGER_SETT_UPDATE(tgtype);
				break;
			default:
				elog(ERROR, "unrecognized trigger event: %d",
					 (int) stmt->actions[i]);
				break;
		}
	}

	/*
	 * Scan pg_trigger for existing triggers on relation.  We do this mainly
	 * because we must count them; a secondary benefit is to give a nice error
	 * message if there's already a trigger of the same name. (The unique
	 * index on tgrelid/tgname would complain anyway.)
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on the
	 * relation, so the trigger set won't be changing underneath us.
	 */
	ScanKeyInit(&key,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								SnapshotNow, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);

		if (namestrcmp(&(pg_trigger->tgname), trigname) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
				  errmsg("trigger \"%s\" for relation \"%s\" already exists",
						 trigname, stmt->relation->relname)));
		found++;
	}
	systable_endscan(tgscan);

	/*
	 * Find and validate the trigger function.
	 */
	funcoid = LookupFuncName(stmt->funcname, 0, fargtypes, false);
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != TRIGGEROID)
	{
		/*
		 * We allow OPAQUE just so we can load old dump files.	When we see a
		 * trigger function declared OPAQUE, change it to TRIGGER.
		 */
		if (funcrettype == OPAQUEOID)
		{
			ereport(WARNING,
					(errmsg("changing return type of function %s from \"opaque\" to \"trigger\"",
							NameListToString(stmt->funcname))));
			SetFunctionReturnType(funcoid, TRIGGEROID);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("function %s must return type \"trigger\"",
							NameListToString(stmt->funcname))));
	}

	/*
	 * Build the new pg_trigger tuple.
	 */
	MemSet(nulls, ' ', Natts_pg_trigger * sizeof(char));

	values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_trigger_tgname - 1] = DirectFunctionCall1(namein,
												  CStringGetDatum(trigname));
	values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_trigger_tgtype - 1] = Int16GetDatum(tgtype);
	values[Anum_pg_trigger_tgenabled - 1] = BoolGetDatum(true);
	values[Anum_pg_trigger_tgisconstraint - 1] = BoolGetDatum(stmt->isconstraint);
	values[Anum_pg_trigger_tgconstrname - 1] = DirectFunctionCall1(namein,
												CStringGetDatum(constrname));
	values[Anum_pg_trigger_tgconstrrelid - 1] = ObjectIdGetDatum(constrrelid);
	values[Anum_pg_trigger_tgdeferrable - 1] = BoolGetDatum(stmt->deferrable);
	values[Anum_pg_trigger_tginitdeferred - 1] = BoolGetDatum(stmt->initdeferred);

	if (stmt->args)
	{
		ListCell   *le;
		char	   *args;
		int16		nargs = list_length(stmt->args);
		int			len = 0;

		foreach(le, stmt->args)
		{
			char	   *ar = strVal(lfirst(le));

			len += strlen(ar) + 4;
			for (; *ar; ar++)
			{
				if (*ar == '\\')
					len++;
			}
		}
		args = (char *) palloc(len + 1);
		args[0] = '\0';
		foreach(le, stmt->args)
		{
			char	   *s = strVal(lfirst(le));
			char	   *d = args + strlen(args);

			while (*s)
			{
				if (*s == '\\')
					*d++ = '\\';
				*d++ = *s++;
			}
			strcpy(d, "\\000");
		}
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(nargs);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
													  CStringGetDatum(args));
	}
	else
	{
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(0);
		values[Anum_pg_trigger_tgargs - 1] = DirectFunctionCall1(byteain,
														CStringGetDatum(""));
	}
	/* tgattr is currently always a zero-length array */
	tgattr = buildint2vector(NULL, 0);
	values[Anum_pg_trigger_tgattr - 1] = PointerGetDatum(tgattr);

	tuple = heap_formtuple(tgrel->rd_att, values, nulls);

	/* force tuple to have the desired OID */
	HeapTupleSetOid(tuple, trigoid);

	/*
	 * Insert tuple into pg_trigger.
	 */
	simple_heap_insert(tgrel, tuple);

	CatalogUpdateIndexes(tgrel, tuple);

	myself.classId = TriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;

	heap_freetuple(tuple);
	heap_close(tgrel, RowExclusiveLock);

	pfree(DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other backends
	 * (and this one too!) are sent SI message to make them rebuild relcache
	 * entries.
	 */
	pgrel = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(RelationGetRelid(rel)),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));

	((Form_pg_class) GETSTRUCT(tuple))->reltriggers = found + 1;

	simple_heap_update(pgrel, &tuple->t_self, tuple);

	CatalogUpdateIndexes(pgrel, tuple);

	heap_freetuple(tuple);
	heap_close(pgrel, RowExclusiveLock);

	/*
	 * We used to try to update the rel's relcache entry here, but that's
	 * fairly pointless since it will happen as a byproduct of the upcoming
	 * CommandCounterIncrement...
	 */

	/*
	 * Record dependencies for trigger.  Always place a normal dependency on
	 * the function.  If we are doing this in response to an explicit CREATE
	 * TRIGGER command, also make trigger be auto-dropped if its relation is
	 * dropped or if the FK relation is dropped.  (Auto drop is compatible
	 * with our pre-7.3 behavior.)	If the trigger is being made for a
	 * constraint, we can skip the relation links; the dependency on the
	 * constraint will indirectly depend on the relations.
	 */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (!forConstraint)
	{
		referenced.classId = RelationRelationId;
		referenced.objectId = RelationGetRelid(rel);
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		if (constrrelid != InvalidOid)
		{
			referenced.classId = RelationRelationId;
			referenced.objectId = constrrelid;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		}
	}

	/* Keep lock on target rel until end of xact */
	heap_close(rel, NoLock);

	return trigoid;
}

/*
 * DropTrigger - drop an individual trigger by name
 */
void
DropTrigger(Oid relid, const char *trigname, DropBehavior behavior,
			bool missing_ok)
{
	Relation	tgrel;
	ScanKeyData skey[2];
	SysScanDesc tgscan;
	HeapTuple	tup;
	ObjectAddress object;

	/*
	 * Find the trigger, verify permissions, set up object address
	 */
	tgrel = heap_open(TriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	ScanKeyInit(&skey[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								SnapshotNow, 2, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("trigger \"%s\" for table \"%s\" does not exist",
							trigname, get_rel_name(relid))));
		else
			ereport(NOTICE,
					(errmsg("trigger \"%s\" for table \"%s\" does not exist, skipping",
							trigname, get_rel_name(relid))));
		/* cleanup */
		systable_endscan(tgscan);
		heap_close(tgrel, AccessShareLock);
		return;
	}

	if (!pg_class_ownercheck(relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   get_rel_name(relid));

	object.classId = TriggerRelationId;
	object.objectId = HeapTupleGetOid(tup);
	object.objectSubId = 0;

	systable_endscan(tgscan);
	heap_close(tgrel, AccessShareLock);

	/*
	 * Do the deletion
	 */
	performDeletion(&object, behavior);
}

/*
 * Guts of trigger deletion.
 */
void
RemoveTriggerById(Oid trigOid)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Oid			relid;
	Relation	rel;
	Relation	pgrel;
	HeapTuple	tuple;
	Form_pg_class classForm;

	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * Find the trigger to delete.
	 */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigOid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for trigger %u", trigOid);

	/*
	 * Open and exclusive-lock the relation the trigger belongs to.
	 */
	relid = ((Form_pg_trigger) GETSTRUCT(tup))->tgrelid;

	rel = heap_open(relid, AccessExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a table",
						RelationGetRelationName(rel))));

	if (!allowSystemTableMods && IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied: \"%s\" is a system catalog",
						RelationGetRelationName(rel))));

	/*
	 * Delete the pg_trigger tuple.
	 */
	simple_heap_delete(tgrel, &tup->t_self);

	systable_endscan(tgscan);
	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other backends
	 * (and this one too!) are sent SI message to make them rebuild relcache
	 * entries.
	 *
	 * Note this is OK only because we have AccessExclusiveLock on the rel, so
	 * no one else is creating/deleting triggers on this rel at the same time.
	 */
	pgrel = heap_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy(RELOID,
							   ObjectIdGetDatum(relid),
							   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	classForm = (Form_pg_class) GETSTRUCT(tuple);

	if (classForm->reltriggers == 0)	/* should not happen */
		elog(ERROR, "relation \"%s\" has reltriggers = 0",
			 RelationGetRelationName(rel));
	classForm->reltriggers--;

	simple_heap_update(pgrel, &tuple->t_self, tuple);

	CatalogUpdateIndexes(pgrel, tuple);

	heap_freetuple(tuple);

	heap_close(pgrel, RowExclusiveLock);

	/* Keep lock on trigger's rel until end of xact */
	heap_close(rel, NoLock);
}

/*
 *		renametrig		- changes the name of a trigger on a relation
 *
 *		trigger name is changed in trigger catalog.
 *		No record of the previous name is kept.
 *
 *		get proper relrelation from relation catalog (if not arg)
 *		scan trigger catalog
 *				for name conflict (within rel)
 *				for original trigger (if not arg)
 *		modify tgname in trigger tuple
 *		update row in catalog
 */
void
renametrig(Oid relid,
		   const char *oldname,
		   const char *newname)
{
	Relation	targetrel;
	Relation	tgrel;
	HeapTuple	tuple;
	SysScanDesc tgscan;
	ScanKeyData key[2];

	/*
	 * Grab an exclusive lock on the target table, which we will NOT release
	 * until end of transaction.
	 */
	targetrel = heap_open(relid, AccessExclusiveLock);

	/*
	 * Scan pg_trigger twice for existing triggers on relation.  We do this in
	 * order to ensure a trigger does not exist with newname (The unique index
	 * on tgrelid/tgname would complain anyway) and to ensure a trigger does
	 * exist with oldname.
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on the
	 * relation, so the trigger set won't be changing underneath us.
	 */
	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	/*
	 * First pass -- look for name conflict
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(newname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								SnapshotNow, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("trigger \"%s\" for relation \"%s\" already exists",
						newname, RelationGetRelationName(targetrel))));
	systable_endscan(tgscan);

	/*
	 * Second pass -- look for trigger existing with oldname and update
	 */
	ScanKeyInit(&key[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_trigger_tgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(oldname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								SnapshotNow, 2, key);
	if (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		/*
		 * Update pg_trigger tuple with new tgname.
		 */
		tuple = heap_copytuple(tuple);	/* need a modifiable copy */

		namestrcpy(&((Form_pg_trigger) GETSTRUCT(tuple))->tgname, newname);

		simple_heap_update(tgrel, &tuple->t_self, tuple);

		/* keep system catalog indexes current */
		CatalogUpdateIndexes(tgrel, tuple);

		/*
		 * Invalidate relation's relcache entry so that other backends (and
		 * this one too!) are sent SI message to make them rebuild relcache
		 * entries.  (Ideally this should happen automatically...)
		 */
		CacheInvalidateRelcache(targetrel);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						oldname, RelationGetRelationName(targetrel))));
	}

	systable_endscan(tgscan);

	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Close rel, but keep exclusive lock!
	 */
	heap_close(targetrel, NoLock);
}


/*
 * EnableDisableTrigger()
 *
 *	Called by ALTER TABLE ENABLE/DISABLE TRIGGER
 *	to change 'tgenabled' flag for the specified trigger(s)
 *
 * rel: relation to process (caller must hold suitable lock on it)
 * tgname: trigger to process, or NULL to scan all triggers
 * enable: new value for tgenabled flag
 * skip_system: if true, skip "system" triggers (constraint triggers)
 *
 * Caller should have checked permissions for the table; here we also
 * enforce that superuser privilege is required to alter the state of
 * system triggers
 */
void
EnableDisableTrigger(Relation rel, const char *tgname,
					 bool enable, bool skip_system)
{
	Relation	tgrel;
	int			nkeys;
	ScanKeyData keys[2];
	SysScanDesc tgscan;
	HeapTuple	tuple;
	bool		found;
	bool		changed;

	/* Scan the relevant entries in pg_triggers */
	tgrel = heap_open(TriggerRelationId, RowExclusiveLock);

	ScanKeyInit(&keys[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	if (tgname)
	{
		ScanKeyInit(&keys[1],
					Anum_pg_trigger_tgname,
					BTEqualStrategyNumber, F_NAMEEQ,
					CStringGetDatum(tgname));
		nkeys = 2;
	}
	else
		nkeys = 1;

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								SnapshotNow, nkeys, keys);

	found = changed = false;

	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger oldtrig = (Form_pg_trigger) GETSTRUCT(tuple);

		if (oldtrig->tgisconstraint)
		{
			/* system trigger ... ok to process? */
			if (skip_system)
				continue;
			if (!superuser())
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					  errmsg("permission denied: \"%s\" is a system trigger",
							 NameStr(oldtrig->tgname))));
		}

		found = true;

		if (oldtrig->tgenabled != enable)
		{
			/* need to change this one ... make a copy to scribble on */
			HeapTuple	newtup = heap_copytuple(tuple);
			Form_pg_trigger newtrig = (Form_pg_trigger) GETSTRUCT(newtup);

			newtrig->tgenabled = enable;

			simple_heap_update(tgrel, &newtup->t_self, newtup);

			/* Keep catalog indexes current */
			CatalogUpdateIndexes(tgrel, newtup);

			heap_freetuple(newtup);

			changed = true;
		}
	}

	systable_endscan(tgscan);

	heap_close(tgrel, RowExclusiveLock);

	if (tgname && !found)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for table \"%s\" does not exist",
						tgname, RelationGetRelationName(rel))));

	/*
	 * If we changed anything, broadcast a SI inval message to force each
	 * backend (including our own!) to rebuild relation's relcache entry.
	 * Otherwise they will fail to apply the change promptly.
	 */
	if (changed)
		CacheInvalidateRelcache(rel);
}


/*
 * Build trigger data to attach to the given relcache entry.
 *
 * Note that trigger data attached to a relcache entry must be stored in
 * CacheMemoryContext to ensure it survives as long as the relcache entry.
 * But we should be running in a less long-lived working context.  To avoid
 * leaking cache memory if this routine fails partway through, we build a
 * temporary TriggerDesc in working memory and then copy the completed
 * structure into cache memory.
 */
void
RelationBuildTriggers(Relation relation)
{
	TriggerDesc *trigdesc;
	int			ntrigs = relation->rd_rel->reltriggers;
	Trigger    *triggers;
	int			found = 0;
	Relation	tgrel;
	ScanKeyData skey;
	SysScanDesc tgscan;
	HeapTuple	htup;
	MemoryContext oldContext;

	Assert(ntrigs > 0);			/* else I should not have been called */

	triggers = (Trigger *) palloc(ntrigs * sizeof(Trigger));

	/*
	 * Note: since we scan the triggers using TriggerRelidNameIndexId, we will
	 * be reading the triggers in name order, except possibly during
	 * emergency-recovery operations (ie, IgnoreSystemIndexes). This in turn
	 * ensures that triggers will be fired in name order.
	 */
	ScanKeyInit(&skey,
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(relation)));

	tgrel = heap_open(TriggerRelationId, AccessShareLock);
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true,
								SnapshotNow, 1, &skey);

	while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);
		Trigger    *build;

		if (found >= ntrigs)
			elog(ERROR, "too many trigger records found for relation \"%s\"",
				 RelationGetRelationName(relation));
		build = &(triggers[found]);

		build->tgoid = HeapTupleGetOid(htup);
		build->tgname = DatumGetCString(DirectFunctionCall1(nameout,
										 NameGetDatum(&pg_trigger->tgname)));
		build->tgfoid = pg_trigger->tgfoid;
		build->tgtype = pg_trigger->tgtype;
		build->tgenabled = pg_trigger->tgenabled;
		build->tgisconstraint = pg_trigger->tgisconstraint;
		build->tgconstrrelid = pg_trigger->tgconstrrelid;
		build->tgdeferrable = pg_trigger->tgdeferrable;
		build->tginitdeferred = pg_trigger->tginitdeferred;
		build->tgnargs = pg_trigger->tgnargs;
		/* tgattr is first var-width field, so OK to access directly */
		build->tgnattr = pg_trigger->tgattr.dim1;
		if (build->tgnattr > 0)
		{
			build->tgattr = (int2 *) palloc(build->tgnattr * sizeof(int2));
			memcpy(build->tgattr, &(pg_trigger->tgattr.values),
				   build->tgnattr * sizeof(int2));
		}
		else
			build->tgattr = NULL;
		if (build->tgnargs > 0)
		{
			bytea	   *val;
			bool		isnull;
			char	   *p;
			int			i;

			val = DatumGetByteaP(fastgetattr(htup,
											Anum_pg_trigger_tgargs,
											tgrel->rd_att, &isnull));
			if (isnull)
				elog(ERROR, "tgargs is null in trigger for relation \"%s\"",
					 RelationGetRelationName(relation));
			p = (char *) VARDATA(val);
			build->tgargs = (char **) palloc(build->tgnargs * sizeof(char *));
			for (i = 0; i < build->tgnargs; i++)
			{
				build->tgargs[i] = pstrdup(p);
				p += strlen(p) + 1;
			}
		}
		else
			build->tgargs = NULL;

		found++;
	}

	systable_endscan(tgscan);
	heap_close(tgrel, AccessShareLock);

	if (found != ntrigs)
		elog(ERROR, "%d trigger record(s) not found for relation \"%s\"",
			 ntrigs - found,
			 RelationGetRelationName(relation));

	/* Build trigdesc */
	trigdesc = (TriggerDesc *) palloc0(sizeof(TriggerDesc));
	trigdesc->triggers = triggers;
	trigdesc->numtriggers = ntrigs;
	for (found = 0; found < ntrigs; found++)
		InsertTrigger(trigdesc, &(triggers[found]), found);

	/* Copy completed trigdesc into cache storage */
	oldContext = MemoryContextSwitchTo(CacheMemoryContext);
	relation->trigdesc = CopyTriggerDesc(trigdesc);
	MemoryContextSwitchTo(oldContext);

	/* Release working memory */
	FreeTriggerDesc(trigdesc);
}

/*
 * Insert the given trigger into the appropriate index list(s) for it
 *
 * To simplify storage management, we allocate each index list at the max
 * possible size (trigdesc->numtriggers) if it's used at all.  This does
 * not waste space permanently since we're only building a temporary
 * trigdesc at this point.
 */
static void
InsertTrigger(TriggerDesc *trigdesc, Trigger *trigger, int indx)
{
	uint16	   *n;
	int		  **t,
			  **tp;

	if (TRIGGER_FOR_ROW(trigger->tgtype))
	{
		/* ROW trigger */
		if (TRIGGER_FOR_BEFORE(trigger->tgtype))
		{
			n = trigdesc->n_before_row;
			t = trigdesc->tg_before_row;
		}
		else
		{
			n = trigdesc->n_after_row;
			t = trigdesc->tg_after_row;
		}
	}
	else
	{
		/* STATEMENT trigger */
		if (TRIGGER_FOR_BEFORE(trigger->tgtype))
		{
			n = trigdesc->n_before_statement;
			t = trigdesc->tg_before_statement;
		}
		else
		{
			n = trigdesc->n_after_statement;
			t = trigdesc->tg_after_statement;
		}
	}

	if (TRIGGER_FOR_INSERT(trigger->tgtype))
	{
		tp = &(t[TRIGGER_EVENT_INSERT]);
		if (*tp == NULL)
			*tp = (int *) palloc(trigdesc->numtriggers * sizeof(int));
		(*tp)[n[TRIGGER_EVENT_INSERT]] = indx;
		(n[TRIGGER_EVENT_INSERT])++;
	}

	if (TRIGGER_FOR_DELETE(trigger->tgtype))
	{
		tp = &(t[TRIGGER_EVENT_DELETE]);
		if (*tp == NULL)
			*tp = (int *) palloc(trigdesc->numtriggers * sizeof(int));
		(*tp)[n[TRIGGER_EVENT_DELETE]] = indx;
		(n[TRIGGER_EVENT_DELETE])++;
	}

	if (TRIGGER_FOR_UPDATE(trigger->tgtype))
	{
		tp = &(t[TRIGGER_EVENT_UPDATE]);
		if (*tp == NULL)
			*tp = (int *) palloc(trigdesc->numtriggers * sizeof(int));
		(*tp)[n[TRIGGER_EVENT_UPDATE]] = indx;
		(n[TRIGGER_EVENT_UPDATE])++;
	}
}

/*
 * Copy a TriggerDesc data structure.
 *
 * The copy is allocated in the current memory context.
 */
TriggerDesc *
CopyTriggerDesc(TriggerDesc *trigdesc)
{
	TriggerDesc *newdesc;
	uint16	   *n;
	int		  **t,
			   *tnew;
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL || trigdesc->numtriggers <= 0)
		return NULL;

	newdesc = (TriggerDesc *) palloc(sizeof(TriggerDesc));
	memcpy(newdesc, trigdesc, sizeof(TriggerDesc));

	trigger = (Trigger *) palloc(trigdesc->numtriggers * sizeof(Trigger));
	memcpy(trigger, trigdesc->triggers,
		   trigdesc->numtriggers * sizeof(Trigger));
	newdesc->triggers = trigger;

	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		trigger->tgname = pstrdup(trigger->tgname);
		if (trigger->tgnattr > 0)
		{
			int2	   *newattr;

			newattr = (int2 *) palloc(trigger->tgnattr * sizeof(int2));
			memcpy(newattr, trigger->tgattr,
				   trigger->tgnattr * sizeof(int2));
			trigger->tgattr = newattr;
		}
		if (trigger->tgnargs > 0)
		{
			char	  **newargs;
			int16		j;

			newargs = (char **) palloc(trigger->tgnargs * sizeof(char *));
			for (j = 0; j < trigger->tgnargs; j++)
				newargs[j] = pstrdup(trigger->tgargs[j]);
			trigger->tgargs = newargs;
		}
		trigger++;
	}

	n = newdesc->n_before_statement;
	t = newdesc->tg_before_statement;
	for (i = 0; i < TRIGGER_NUM_EVENT_CLASSES; i++)
	{
		if (n[i] > 0)
		{
			tnew = (int *) palloc(n[i] * sizeof(int));
			memcpy(tnew, t[i], n[i] * sizeof(int));
			t[i] = tnew;
		}
		else
			t[i] = NULL;
	}
	n = newdesc->n_before_row;
	t = newdesc->tg_before_row;
	for (i = 0; i < TRIGGER_NUM_EVENT_CLASSES; i++)
	{
		if (n[i] > 0)
		{
			tnew = (int *) palloc(n[i] * sizeof(int));
			memcpy(tnew, t[i], n[i] * sizeof(int));
			t[i] = tnew;
		}
		else
			t[i] = NULL;
	}
	n = newdesc->n_after_row;
	t = newdesc->tg_after_row;
	for (i = 0; i < TRIGGER_NUM_EVENT_CLASSES; i++)
	{
		if (n[i] > 0)
		{
			tnew = (int *) palloc(n[i] * sizeof(int));
			memcpy(tnew, t[i], n[i] * sizeof(int));
			t[i] = tnew;
		}
		else
			t[i] = NULL;
	}
	n = newdesc->n_after_statement;
	t = newdesc->tg_after_statement;
	for (i = 0; i < TRIGGER_NUM_EVENT_CLASSES; i++)
	{
		if (n[i] > 0)
		{
			tnew = (int *) palloc(n[i] * sizeof(int));
			memcpy(tnew, t[i], n[i] * sizeof(int));
			t[i] = tnew;
		}
		else
			t[i] = NULL;
	}

	return newdesc;
}

/*
 * Free a TriggerDesc data structure.
 */
void
FreeTriggerDesc(TriggerDesc *trigdesc)
{
	int		  **t;
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL)
		return;

	t = trigdesc->tg_before_statement;
	for (i = 0; i < TRIGGER_NUM_EVENT_CLASSES; i++)
		if (t[i] != NULL)
			pfree(t[i]);
	t = trigdesc->tg_before_row;
	for (i = 0; i < TRIGGER_NUM_EVENT_CLASSES; i++)
		if (t[i] != NULL)
			pfree(t[i]);
	t = trigdesc->tg_after_row;
	for (i = 0; i < TRIGGER_NUM_EVENT_CLASSES; i++)
		if (t[i] != NULL)
			pfree(t[i]);
	t = trigdesc->tg_after_statement;
	for (i = 0; i < TRIGGER_NUM_EVENT_CLASSES; i++)
		if (t[i] != NULL)
			pfree(t[i]);

	trigger = trigdesc->triggers;
	for (i = 0; i < trigdesc->numtriggers; i++)
	{
		pfree(trigger->tgname);
		if (trigger->tgnattr > 0)
			pfree(trigger->tgattr);
		if (trigger->tgnargs > 0)
		{
			while (--(trigger->tgnargs) >= 0)
				pfree(trigger->tgargs[trigger->tgnargs]);
			pfree(trigger->tgargs);
		}
		trigger++;
	}
	pfree(trigdesc->triggers);
	pfree(trigdesc);
}

/*
 * Compare two TriggerDesc structures for logical equality.
 */
#ifdef NOT_USED
bool
equalTriggerDescs(TriggerDesc *trigdesc1, TriggerDesc *trigdesc2)
{
	int			i,
				j;

	/*
	 * We need not examine the "index" data, just the trigger array itself; if
	 * we have the same triggers with the same types, the derived index data
	 * should match.
	 *
	 * As of 7.3 we assume trigger set ordering is significant in the
	 * comparison; so we just compare corresponding slots of the two sets.
	 */
	if (trigdesc1 != NULL)
	{
		if (trigdesc2 == NULL)
			return false;
		if (trigdesc1->numtriggers != trigdesc2->numtriggers)
			return false;
		for (i = 0; i < trigdesc1->numtriggers; i++)
		{
			Trigger    *trig1 = trigdesc1->triggers + i;
			Trigger    *trig2 = trigdesc2->triggers + i;

			if (trig1->tgoid != trig2->tgoid)
				return false;
			if (strcmp(trig1->tgname, trig2->tgname) != 0)
				return false;
			if (trig1->tgfoid != trig2->tgfoid)
				return false;
			if (trig1->tgtype != trig2->tgtype)
				return false;
			if (trig1->tgenabled != trig2->tgenabled)
				return false;
			if (trig1->tgisconstraint != trig2->tgisconstraint)
				return false;
			if (trig1->tgconstrrelid != trig2->tgconstrrelid)
				return false;
			if (trig1->tgdeferrable != trig2->tgdeferrable)
				return false;
			if (trig1->tginitdeferred != trig2->tginitdeferred)
				return false;
			if (trig1->tgnargs != trig2->tgnargs)
				return false;
			if (trig1->tgnattr != trig2->tgnattr)
				return false;
			if (trig1->tgnattr > 0 &&
				memcmp(trig1->tgattr, trig2->tgattr,
					   trig1->tgnattr * sizeof(int2)) != 0)
				return false;
			for (j = 0; j < trig1->tgnargs; j++)
				if (strcmp(trig1->tgargs[j], trig2->tgargs[j]) != 0)
					return false;
		}
	}
	else if (trigdesc2 != NULL)
		return false;
	return true;
}
#endif   /* NOT_USED */

/*
 * Call a trigger function.
 *
 *		trigdata: trigger descriptor.
 *		tgindx: trigger's index in finfo and instr arrays.
 *		finfo: array of cached trigger function call information.
 *		instr: optional array of EXPLAIN ANALYZE instrumentation state.
 *		per_tuple_context: memory context to execute the function in.
 *
 * Returns the tuple (or NULL) as returned by the function.
 */
static HeapTuple
ExecCallTriggerFunc(TriggerData *trigdata,
					int tgindx,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context)
{
	FunctionCallInfoData fcinfo;
	Datum		result;
	MemoryContext oldContext;

	finfo += tgindx;

	/*
	 * We cache fmgr lookup info, to avoid making the lookup again on each
	 * call.
	 */
	if (finfo->fn_oid == InvalidOid)
		fmgr_info(trigdata->tg_trigger->tgfoid, finfo);

	Assert(finfo->fn_oid == trigdata->tg_trigger->tgfoid);

	/*
	 * If doing EXPLAIN ANALYZE, start charging time to this trigger.
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * Do the function evaluation in the per-tuple memory context, so that
	 * leaked memory will be reclaimed once per tuple. Note in particular that
	 * any new tuple created by the trigger function will live till the end of
	 * the tuple cycle.
	 */
	oldContext = MemoryContextSwitchTo(per_tuple_context);

	/*
	 * Call the function, passing no arguments but setting a context.
	 */
	InitFunctionCallInfoData(fcinfo, finfo, 0, (Node *) trigdata, NULL);

	result = FunctionCallInvoke(&fcinfo);

	MemoryContextSwitchTo(oldContext);

	/*
	 * Trigger protocol allows function to return a null pointer, but NOT to
	 * set the isnull result flag.
	 */
	if (fcinfo.isnull)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("trigger function %u returned null value",
						fcinfo.flinfo->fn_oid)));

	/*
	 * If doing EXPLAIN ANALYZE, stop charging time to this trigger, and count
	 * one "tuple returned" (really the number of firings).
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);

	return (HeapTuple) DatumGetPointer(result);
}

void
ExecBSInsertTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			ntrigs;
	int		   *tgindx;
	int			i;
	TriggerData LocTriggerData;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;

	ntrigs = trigdesc->n_before_statement[TRIGGER_EVENT_INSERT];
	tgindx = trigdesc->tg_before_statement[TRIGGER_EVENT_INSERT];

	if (ntrigs == 0)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_trigtuple = NULL;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];
		HeapTuple	newtuple;

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   tgindx[i],
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				  errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASInsertTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->n_after_statement[TRIGGER_EVENT_INSERT] > 0)
		AfterTriggerSaveEvent(relinfo, TRIGGER_EVENT_INSERT,
							  false, NULL, NULL);
}

HeapTuple
ExecBRInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	int			ntrigs = trigdesc->n_before_row[TRIGGER_EVENT_INSERT];
	int		   *tgindx = trigdesc->tg_before_row[TRIGGER_EVENT_INSERT];
	HeapTuple	newtuple = trigtuple;
	HeapTuple	oldtuple;
	TriggerData LocTriggerData;
	int			i;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   tgindx[i],
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (oldtuple != newtuple && oldtuple != trigtuple)
			heap_freetuple(oldtuple);
		if (newtuple == NULL)
			break;
	}
	return newtuple;
}

void
ExecARInsertTriggers(EState *estate, ResultRelInfo *relinfo,
					 HeapTuple trigtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->n_after_row[TRIGGER_EVENT_INSERT] > 0)
		AfterTriggerSaveEvent(relinfo, TRIGGER_EVENT_INSERT,
							  true, NULL, trigtuple);
}

void
ExecBSDeleteTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			ntrigs;
	int		   *tgindx;
	int			i;
	TriggerData LocTriggerData;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;

	ntrigs = trigdesc->n_before_statement[TRIGGER_EVENT_DELETE];
	tgindx = trigdesc->tg_before_statement[TRIGGER_EVENT_DELETE];

	if (ntrigs == 0)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_trigtuple = NULL;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];
		HeapTuple	newtuple;

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   tgindx[i],
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				  errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASDeleteTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->n_after_statement[TRIGGER_EVENT_DELETE] > 0)
		AfterTriggerSaveEvent(relinfo, TRIGGER_EVENT_DELETE,
							  false, NULL, NULL);
}

bool
ExecBRDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 ItemPointer tupleid,
					 CommandId cid)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	int			ntrigs = trigdesc->n_before_row[TRIGGER_EVENT_DELETE];
	int		   *tgindx = trigdesc->tg_before_row[TRIGGER_EVENT_DELETE];
	bool		result = true;
	TriggerData LocTriggerData;
	HeapTuple	trigtuple;
	HeapTuple	newtuple;
	TupleTableSlot *newSlot;
	int			i;

	trigtuple = GetTupleForTrigger(estate, relinfo, tupleid, cid, &newSlot);
	if (trigtuple == NULL)
		return false;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   tgindx[i],
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
		{
			result = false;		/* tell caller to suppress delete */
			break;
		}
		if (newtuple != trigtuple)
			heap_freetuple(newtuple);
	}
	heap_freetuple(trigtuple);

	return result;
}

void
ExecARDeleteTriggers(EState *estate, ResultRelInfo *relinfo,
					 ItemPointer tupleid)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->n_after_row[TRIGGER_EVENT_DELETE] > 0)
	{
		HeapTuple	trigtuple = GetTupleForTrigger(estate, relinfo,
												   tupleid,
												   (CommandId) 0,
												   NULL);

		AfterTriggerSaveEvent(relinfo, TRIGGER_EVENT_DELETE,
							  true, trigtuple, NULL);
		heap_freetuple(trigtuple);
	}
}

void
ExecBSUpdateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc;
	int			ntrigs;
	int		   *tgindx;
	int			i;
	TriggerData LocTriggerData;

	trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc == NULL)
		return;

	ntrigs = trigdesc->n_before_statement[TRIGGER_EVENT_UPDATE];
	tgindx = trigdesc->tg_before_statement[TRIGGER_EVENT_UPDATE];

	if (ntrigs == 0)
		return;

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_trigtuple = NULL;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];
		HeapTuple	newtuple;

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   tgindx[i],
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));

		if (newtuple)
			ereport(ERROR,
					(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				  errmsg("BEFORE STATEMENT trigger cannot return a value")));
	}
}

void
ExecASUpdateTriggers(EState *estate, ResultRelInfo *relinfo)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->n_after_statement[TRIGGER_EVENT_UPDATE] > 0)
		AfterTriggerSaveEvent(relinfo, TRIGGER_EVENT_UPDATE,
							  false, NULL, NULL);
}

HeapTuple
ExecBRUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 ItemPointer tupleid, HeapTuple newtuple,
					 CommandId cid)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	int			ntrigs = trigdesc->n_before_row[TRIGGER_EVENT_UPDATE];
	int		   *tgindx = trigdesc->tg_before_row[TRIGGER_EVENT_UPDATE];
	TriggerData LocTriggerData;
	HeapTuple	trigtuple;
	HeapTuple	oldtuple;
	HeapTuple	intuple = newtuple;
	TupleTableSlot *newSlot;
	int			i;

	trigtuple = GetTupleForTrigger(estate, relinfo, tupleid, cid, &newSlot);
	if (trigtuple == NULL)
		return NULL;

	/*
	 * In READ COMMITTED isolation level it's possible that newtuple was
	 * changed due to concurrent update.
	 */
	if (newSlot != NULL)
		intuple = newtuple = ExecRemoveJunk(estate->es_junkFilter, newSlot);

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_newtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
		LocTriggerData.tg_newtuplebuf = InvalidBuffer;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
									   tgindx[i],
									   relinfo->ri_TrigFunctions,
									   relinfo->ri_TrigInstrument,
									   GetPerTupleMemoryContext(estate));
		if (oldtuple != newtuple && oldtuple != intuple)
			heap_freetuple(oldtuple);
		if (newtuple == NULL)
			break;
	}
	heap_freetuple(trigtuple);
	return newtuple;
}

void
ExecARUpdateTriggers(EState *estate, ResultRelInfo *relinfo,
					 ItemPointer tupleid, HeapTuple newtuple)
{
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;

	if (trigdesc && trigdesc->n_after_row[TRIGGER_EVENT_UPDATE] > 0)
	{
		HeapTuple	trigtuple = GetTupleForTrigger(estate, relinfo,
												   tupleid,
												   (CommandId) 0,
												   NULL);

		AfterTriggerSaveEvent(relinfo, TRIGGER_EVENT_UPDATE,
							  true, trigtuple, newtuple);
		heap_freetuple(trigtuple);
	}
}


static HeapTuple
GetTupleForTrigger(EState *estate, ResultRelInfo *relinfo,
				   ItemPointer tid, CommandId cid,
				   TupleTableSlot **newSlot)
{
	Relation	relation = relinfo->ri_RelationDesc;
	HeapTupleData tuple;
	HeapTuple	result;
	Buffer		buffer;

	if (newSlot != NULL)
	{
		HTSU_Result test;
		ItemPointerData update_ctid;
		TransactionId update_xmax;

		*newSlot = NULL;

		/*
		 * lock tuple for update
		 */
ltrmark:;
		tuple.t_self = *tid;
		test = heap_lock_tuple(relation, &tuple, &buffer,
							   &update_ctid, &update_xmax, cid,
							   LockTupleExclusive, false);
		switch (test)
		{
			case HeapTupleSelfUpdated:
				/* treat it as deleted; do not process */
				ReleaseBuffer(buffer);
				return NULL;

			case HeapTupleMayBeUpdated:
				break;

			case HeapTupleUpdated:
				ReleaseBuffer(buffer);
				if (IsXactIsoLevelSerializable)
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("could not serialize access due to concurrent update")));
				else if (!ItemPointerEquals(&update_ctid, &tuple.t_self))
				{
					/* it was updated, so look at the updated version */
					TupleTableSlot *epqslot;

					epqslot = EvalPlanQual(estate,
										   relinfo->ri_RangeTableIndex,
										   &update_ctid,
										   update_xmax,
										   cid);
					if (!TupIsNull(epqslot))
					{
						*tid = update_ctid;
						*newSlot = epqslot;
						goto ltrmark;
					}
				}

				/*
				 * if tuple was deleted or PlanQual failed for updated tuple -
				 * we have not process this tuple!
				 */
				return NULL;

			default:
				ReleaseBuffer(buffer);
				elog(ERROR, "unrecognized heap_lock_tuple status: %u", test);
				return NULL;	/* keep compiler quiet */
		}
	}
	else
	{
		PageHeader	dp;
		ItemId		lp;

		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

		dp = (PageHeader) BufferGetPage(buffer);
		lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));

		Assert(ItemIdIsUsed(lp));

		tuple.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
		tuple.t_len = ItemIdGetLength(lp);
		tuple.t_self = *tid;
		tuple.t_tableOid = RelationGetRelid(relation);
	}

	result = heap_copytuple(&tuple);
	ReleaseBuffer(buffer);

	return result;
}


/* ----------
 * After-trigger stuff
 *
 * The AfterTriggersData struct holds data about pending AFTER trigger events
 * during the current transaction tree.  (BEFORE triggers are fired
 * immediately so we don't need any persistent state about them.)  The struct
 * and most of its subsidiary data are kept in TopTransactionContext; however
 * the individual event records are kept in separate contexts, to make them
 * easy to delete during subtransaction abort.
 *
 * Because the list of pending events can grow large, we go to some effort
 * to minimize memory consumption.	We do not use the generic List mechanism
 * but thread the events manually.
 *
 * XXX We need to be able to save the per-event data in a file if it grows too
 * large.
 * ----------
 */

/* Per-trigger SET CONSTRAINT status */
typedef struct SetConstraintTriggerData
{
	Oid			sct_tgoid;
	bool		sct_tgisdeferred;
} SetConstraintTriggerData;

typedef struct SetConstraintTriggerData *SetConstraintTrigger;

/*
 * SET CONSTRAINT intra-transaction status.
 *
 * We make this a single palloc'd object so it can be copied and freed easily.
 *
 * all_isset and all_isdeferred are used to keep track
 * of SET CONSTRAINTS ALL {DEFERRED, IMMEDIATE}.
 *
 * trigstates[] stores per-trigger tgisdeferred settings.
 */
typedef struct SetConstraintStateData
{
	bool		all_isset;
	bool		all_isdeferred;
	int			numstates;		/* number of trigstates[] entries in use */
	int			numalloc;		/* allocated size of trigstates[] */
	SetConstraintTriggerData trigstates[1];		/* VARIABLE LENGTH ARRAY */
} SetConstraintStateData;

typedef SetConstraintStateData *SetConstraintState;


/*
 * Per-trigger-event data
 *
 * Note: ate_firing_id is meaningful when either AFTER_TRIGGER_DONE
 * or AFTER_TRIGGER_IN_PROGRESS is set.  It indicates which trigger firing
 * cycle the trigger was or will be fired in.
 */
typedef struct AfterTriggerEventData *AfterTriggerEvent;

typedef struct AfterTriggerEventData
{
	AfterTriggerEvent ate_next; /* list link */
	TriggerEvent ate_event;		/* event type and status bits */
	CommandId	ate_firing_id;	/* ID for firing cycle */
	Oid			ate_tgoid;		/* the trigger's ID */
	Oid			ate_relid;		/* the relation it's on */
	ItemPointerData ate_oldctid;	/* specific tuple(s) involved */
	ItemPointerData ate_newctid;
} AfterTriggerEventData;

/* A list of events */
typedef struct AfterTriggerEventList
{
	AfterTriggerEvent head;
	AfterTriggerEvent tail;
} AfterTriggerEventList;


/*
 * All per-transaction data for the AFTER TRIGGERS module.
 *
 * AfterTriggersData has the following fields:
 *
 * firing_counter is incremented for each call of afterTriggerInvokeEvents.
 * We mark firable events with the current firing cycle's ID so that we can
 * tell which ones to work on.	This ensures sane behavior if a trigger
 * function chooses to do SET CONSTRAINTS: the inner SET CONSTRAINTS will
 * only fire those events that weren't already scheduled for firing.
 *
 * state keeps track of the transaction-local effects of SET CONSTRAINTS.
 * This is saved and restored across failed subtransactions.
 *
 * events is the current list of deferred events.  This is global across
 * all subtransactions of the current transaction.	In a subtransaction
 * abort, we know that the events added by the subtransaction are at the
 * end of the list, so it is relatively easy to discard them.  The event
 * structs themselves are stored in event_cxt if generated by the top-level
 * transaction, else in per-subtransaction contexts identified by the
 * entries in cxt_stack.
 *
 * query_depth is the current depth of nested AfterTriggerBeginQuery calls
 * (-1 when the stack is empty).
 *
 * query_stack[query_depth] is a list of AFTER trigger events queued by the
 * current query (and the query_stack entries below it are lists of trigger
 * events queued by calling queries).  None of these are valid until the
 * matching AfterTriggerEndQuery call occurs.  At that point we fire
 * immediate-mode triggers, and append any deferred events to the main events
 * list.
 *
 * maxquerydepth is just the allocated length of query_stack.
 *
 * state_stack is a stack of pointers to saved copies of the SET CONSTRAINTS
 * state data; each subtransaction level that modifies that state first
 * saves a copy, which we use to restore the state if we abort.
 *
 * events_stack is a stack of copies of the events head/tail pointers,
 * which we use to restore those values during subtransaction abort.
 *
 * depth_stack is a stack of copies of subtransaction-start-time query_depth,
 * which we similarly use to clean up at subtransaction abort.
 *
 * firing_stack is a stack of copies of subtransaction-start-time
 * firing_counter.	We use this to recognize which deferred triggers were
 * fired (or marked for firing) within an aborted subtransaction.
 *
 * We use GetCurrentTransactionNestLevel() to determine the correct array
 * index in these stacks.  maxtransdepth is the number of allocated entries in
 * each stack.	(By not keeping our own stack pointer, we can avoid trouble
 * in cases where errors during subxact abort cause multiple invocations
 * of AfterTriggerEndSubXact() at the same nesting depth.)
 */
typedef struct AfterTriggersData
{
	CommandId	firing_counter; /* next firing ID to assign */
	SetConstraintState state;	/* the active S C state */
	AfterTriggerEventList events;		/* deferred-event list */
	int			query_depth;	/* current query list index */
	AfterTriggerEventList *query_stack; /* events pending from each query */
	int			maxquerydepth;	/* allocated len of above array */
	MemoryContext event_cxt;	/* top transaction's event context, if any */
	MemoryContext *cxt_stack;	/* per-subtransaction event contexts */

	/* these fields are just for resetting at subtrans abort: */

	SetConstraintState *state_stack;	/* stacked S C states */
	AfterTriggerEventList *events_stack;		/* stacked list pointers */
	int		   *depth_stack;	/* stacked query_depths */
	CommandId  *firing_stack;	/* stacked firing_counters */
	int			maxtransdepth;	/* allocated len of above arrays */
} AfterTriggersData;

typedef AfterTriggersData *AfterTriggers;

static AfterTriggers afterTriggers;


static void AfterTriggerExecute(AfterTriggerEvent event,
					Relation rel, TriggerDesc *trigdesc,
					FmgrInfo *finfo,
					Instrumentation *instr,
					MemoryContext per_tuple_context);
static SetConstraintState SetConstraintStateCreate(int numalloc);
static SetConstraintState SetConstraintStateCopy(SetConstraintState state);
static SetConstraintState SetConstraintStateAddItem(SetConstraintState state,
						  Oid tgoid, bool tgisdeferred);


/* ----------
 * afterTriggerCheckState()
 *
 *	Returns true if the trigger identified by tgoid is actually
 *	in state DEFERRED.
 * ----------
 */
static bool
afterTriggerCheckState(Oid tgoid, TriggerEvent eventstate)
{
	SetConstraintState state = afterTriggers->state;
	int			i;

	/*
	 * For not-deferrable triggers (i.e. normal AFTER ROW triggers and
	 * constraints declared NOT DEFERRABLE), the state is always false.
	 */
	if ((eventstate & AFTER_TRIGGER_DEFERRABLE) == 0)
		return false;

	/*
	 * Check if SET CONSTRAINTS has been executed for this specific trigger.
	 */
	for (i = 0; i < state->numstates; i++)
	{
		if (state->trigstates[i].sct_tgoid == tgoid)
			return state->trigstates[i].sct_tgisdeferred;
	}

	/*
	 * Check if SET CONSTRAINTS ALL has been executed; if so use that.
	 */
	if (state->all_isset)
		return state->all_isdeferred;

	/*
	 * Otherwise return the default state for the trigger.
	 */
	return ((eventstate & AFTER_TRIGGER_INITDEFERRED) != 0);
}


/* ----------
 * afterTriggerAddEvent()
 *
 *	Add a new trigger event to the current query's queue.
 * ----------
 */
static void
afterTriggerAddEvent(AfterTriggerEvent event)
{
	AfterTriggerEventList *events;

	Assert(event->ate_next == NULL);

	/* Must be inside a query */
	Assert(afterTriggers->query_depth >= 0);

	events = &afterTriggers->query_stack[afterTriggers->query_depth];
	if (events->tail == NULL)
	{
		/* first list entry */
		events->head = event;
		events->tail = event;
	}
	else
	{
		events->tail->ate_next = event;
		events->tail = event;
	}
}


/* ----------
 * AfterTriggerExecute()
 *
 *	Fetch the required tuples back from the heap and fire one
 *	single trigger function.
 *
 *	Frequently, this will be fired many times in a row for triggers of
 *	a single relation.	Therefore, we cache the open relation and provide
 *	fmgr lookup cache space at the caller level.  (For triggers fired at
 *	the end of a query, we can even piggyback on the executor's state.)
 *
 *	event: event currently being fired.
 *	rel: open relation for event.
 *	trigdesc: working copy of rel's trigger info.
 *	finfo: array of fmgr lookup cache entries (one per trigger in trigdesc).
 *	instr: array of EXPLAIN ANALYZE instrumentation nodes (one per trigger),
 *		or NULL if no instrumentation is wanted.
 *	per_tuple_context: memory context to call trigger function in.
 * ----------
 */
static void
AfterTriggerExecute(AfterTriggerEvent event,
					Relation rel, TriggerDesc *trigdesc,
					FmgrInfo *finfo, Instrumentation *instr,
					MemoryContext per_tuple_context)
{
	Oid			tgoid = event->ate_tgoid;
	TriggerData LocTriggerData;
	HeapTupleData oldtuple;
	HeapTupleData newtuple;
	HeapTuple	rettuple;
	Buffer		oldbuffer = InvalidBuffer;
	Buffer		newbuffer = InvalidBuffer;
	int			tgindx;

	/*
	 * Locate trigger in trigdesc.
	 */
	LocTriggerData.tg_trigger = NULL;
	for (tgindx = 0; tgindx < trigdesc->numtriggers; tgindx++)
	{
		if (trigdesc->triggers[tgindx].tgoid == tgoid)
		{
			LocTriggerData.tg_trigger = &(trigdesc->triggers[tgindx]);
			break;
		}
	}
	if (LocTriggerData.tg_trigger == NULL)
		elog(ERROR, "could not find trigger %u", tgoid);

	/*
	 * If doing EXPLAIN ANALYZE, start charging time to this trigger. We want
	 * to include time spent re-fetching tuples in the trigger cost.
	 */
	if (instr)
		InstrStartNode(instr + tgindx);

	/*
	 * Fetch the required OLD and NEW tuples.
	 */
	LocTriggerData.tg_trigtuple = NULL;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuplebuf = InvalidBuffer;
	LocTriggerData.tg_newtuplebuf = InvalidBuffer;

	if (ItemPointerIsValid(&(event->ate_oldctid)))
	{
		ItemPointerCopy(&(event->ate_oldctid), &(oldtuple.t_self));
		if (!heap_fetch(rel, SnapshotAny, &oldtuple, &oldbuffer, false, NULL))
			elog(ERROR, "failed to fetch old tuple for AFTER trigger");
		LocTriggerData.tg_trigtuple = &oldtuple;
		LocTriggerData.tg_trigtuplebuf = oldbuffer;
	}

	if (ItemPointerIsValid(&(event->ate_newctid)))
	{
		ItemPointerCopy(&(event->ate_newctid), &(newtuple.t_self));
		if (!heap_fetch(rel, SnapshotAny, &newtuple, &newbuffer, false, NULL))
			elog(ERROR, "failed to fetch new tuple for AFTER trigger");
		if (LocTriggerData.tg_trigtuple != NULL)
		{
			LocTriggerData.tg_newtuple = &newtuple;
			LocTriggerData.tg_newtuplebuf = newbuffer;
		}
		else
		{
			LocTriggerData.tg_trigtuple = &newtuple;
			LocTriggerData.tg_trigtuplebuf = newbuffer;
		}
	}

	/*
	 * Setup the remaining trigger information
	 */
	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event =
		event->ate_event & (TRIGGER_EVENT_OPMASK | TRIGGER_EVENT_ROW);
	LocTriggerData.tg_relation = rel;

	MemoryContextReset(per_tuple_context);

	/*
	 * Call the trigger and throw away any possibly returned updated tuple.
	 * (Don't let ExecCallTriggerFunc measure EXPLAIN time.)
	 */
	rettuple = ExecCallTriggerFunc(&LocTriggerData,
								   tgindx,
								   finfo,
								   NULL,
								   per_tuple_context);
	if (rettuple != NULL && rettuple != &oldtuple && rettuple != &newtuple)
		heap_freetuple(rettuple);

	/*
	 * Release buffers
	 */
	if (oldbuffer != InvalidBuffer)
		ReleaseBuffer(oldbuffer);
	if (newbuffer != InvalidBuffer)
		ReleaseBuffer(newbuffer);

	/*
	 * If doing EXPLAIN ANALYZE, stop charging time to this trigger, and count
	 * one "tuple returned" (really the number of firings).
	 */
	if (instr)
		InstrStopNode(instr + tgindx, 1);
}


/*
 * afterTriggerMarkEvents()
 *
 *	Scan the given event list for not yet invoked events.  Mark the ones
 *	that can be invoked now with the current firing ID.
 *
 *	If move_list isn't NULL, events that are not to be invoked now are
 *	removed from the given list and appended to move_list.
 *
 *	When immediate_only is TRUE, do not invoke currently-deferred triggers.
 *	(This will be FALSE only at main transaction exit.)
 *
 *	Returns TRUE if any invokable events were found.
 */
static bool
afterTriggerMarkEvents(AfterTriggerEventList *events,
					   AfterTriggerEventList *move_list,
					   bool immediate_only)
{
	bool		found = false;
	AfterTriggerEvent event,
				prev_event;

	prev_event = NULL;
	event = events->head;

	while (event != NULL)
	{
		bool		defer_it = false;
		AfterTriggerEvent next_event;

		if (!(event->ate_event &
			  (AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS)))
		{
			/*
			 * This trigger hasn't been called or scheduled yet. Check if we
			 * should call it now.
			 */
			if (immediate_only &&
				afterTriggerCheckState(event->ate_tgoid, event->ate_event))
			{
				defer_it = true;
			}
			else
			{
				/*
				 * Mark it as to be fired in this firing cycle.
				 */
				event->ate_firing_id = afterTriggers->firing_counter;
				event->ate_event |= AFTER_TRIGGER_IN_PROGRESS;
				found = true;
			}
		}

		/*
		 * If it's deferred, move it to move_list, if requested.
		 */
		next_event = event->ate_next;

		if (defer_it && move_list != NULL)
		{
			/* Delink it from input list */
			if (prev_event)
				prev_event->ate_next = next_event;
			else
				events->head = next_event;
			/* and add it to move_list */
			event->ate_next = NULL;
			if (move_list->tail == NULL)
			{
				/* first list entry */
				move_list->head = event;
				move_list->tail = event;
			}
			else
			{
				move_list->tail->ate_next = event;
				move_list->tail = event;
			}
		}
		else
		{
			/* Keep it in input list */
			prev_event = event;
		}

		event = next_event;
	}

	/* Update list tail pointer in case we moved tail event */
	events->tail = prev_event;

	return found;
}

/* ----------
 * afterTriggerInvokeEvents()
 *
 *	Scan the given event list for events that are marked as to be fired
 *	in the current firing cycle, and fire them.  query_depth is the index in
 *	afterTriggers->query_stack, or -1 to examine afterTriggers->events.
 *	(We have to be careful here because query_stack could move under us.)
 *
 *	If estate isn't NULL, then we expect that all the firable events are
 *	for triggers of the relations included in the estate's result relation
 *	array.	This allows us to re-use the estate's open relations and
 *	trigger cache info.  When estate is NULL, we have to find the relations
 *	the hard way.
 *
 *	When delete_ok is TRUE, it's okay to delete fully-processed events.
 *	The events list pointers are updated.
 * ----------
 */
static void
afterTriggerInvokeEvents(int query_depth,
						 CommandId firing_id,
						 EState *estate,
						 bool delete_ok)
{
	AfterTriggerEventList *events;
	AfterTriggerEvent event,
				prev_event;
	MemoryContext per_tuple_context;
	bool		locally_opened = false;
	Relation	rel = NULL;
	TriggerDesc *trigdesc = NULL;
	FmgrInfo   *finfo = NULL;
	Instrumentation *instr = NULL;

	/* Make a per-tuple memory context for trigger function calls */
	per_tuple_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AfterTriggerTupleContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	prev_event = NULL;
	events = (query_depth >= 0) ? &afterTriggers->query_stack[query_depth] : &afterTriggers->events;
	event = events->head;

	while (event != NULL)
	{
		AfterTriggerEvent next_event;

		/*
		 * Is it one for me to fire?
		 */
		if ((event->ate_event & AFTER_TRIGGER_IN_PROGRESS) &&
			event->ate_firing_id == firing_id)
		{
			/*
			 * So let's fire it... but first, open the correct relation if
			 * this is not the same relation as before.
			 */
			if (rel == NULL || rel->rd_id != event->ate_relid)
			{
				if (locally_opened)
				{
					/* close prior rel if any */
					if (rel)
						heap_close(rel, NoLock);
					if (trigdesc)
						FreeTriggerDesc(trigdesc);
					if (finfo)
						pfree(finfo);
					Assert(instr == NULL);		/* never used in this case */
				}
				locally_opened = true;

				if (estate)
				{
					/* Find target relation among estate's result rels */
					ResultRelInfo *rInfo;
					int			nr;

					rInfo = estate->es_result_relations;
					nr = estate->es_num_result_relations;
					while (nr > 0)
					{
						if (rInfo->ri_RelationDesc->rd_id == event->ate_relid)
						{
							rel = rInfo->ri_RelationDesc;
							trigdesc = rInfo->ri_TrigDesc;
							finfo = rInfo->ri_TrigFunctions;
							instr = rInfo->ri_TrigInstrument;
							locally_opened = false;
							break;
						}
						rInfo++;
						nr--;
					}
				}

				if (locally_opened)
				{
					/* Hard way: open target relation for ourselves */

					/*
					 * We assume that an appropriate lock is still held by the
					 * executor, so grab no new lock here.
					 */
					rel = heap_open(event->ate_relid, NoLock);

					/*
					 * Copy relation's trigger info so that we have a stable
					 * copy no matter what the called triggers do.
					 */
					trigdesc = CopyTriggerDesc(rel->trigdesc);

					if (trigdesc == NULL)		/* should not happen */
						elog(ERROR, "relation %u has no triggers",
							 event->ate_relid);

					/*
					 * Allocate space to cache fmgr lookup info for triggers.
					 */
					finfo = (FmgrInfo *)
						palloc0(trigdesc->numtriggers * sizeof(FmgrInfo));

					/* Never any EXPLAIN info in this case */
					instr = NULL;
				}
			}

			/*
			 * Fire it.  Note that the AFTER_TRIGGER_IN_PROGRESS flag is still
			 * set, so recursive examinations of the event list won't try to
			 * re-fire it.
			 */
			AfterTriggerExecute(event, rel, trigdesc, finfo, instr,
								per_tuple_context);

			/*
			 * Mark the event as done.
			 */
			event->ate_event &= ~AFTER_TRIGGER_IN_PROGRESS;
			event->ate_event |= AFTER_TRIGGER_DONE;
		}

		/*
		 * If it's now done, throw it away, if allowed.
		 *
		 * NB: it's possible the trigger call above added more events to the
		 * queue, or that calls we will do later will want to add more, so we
		 * have to be careful about maintaining list validity at all points
		 * here.
		 */
		next_event = event->ate_next;

		if ((event->ate_event & AFTER_TRIGGER_DONE) && delete_ok)
		{
			/* Delink it from list and free it */
			if (prev_event)
				prev_event->ate_next = next_event;
			else
			{
				events = (query_depth >= 0) ? &afterTriggers->query_stack[query_depth] : &afterTriggers->events;
				events->head = next_event;
			}
			pfree(event);
		}
		else
		{
			/* Keep it in list */
			prev_event = event;
		}

		event = next_event;
	}

	/* Update list tail pointer in case we just deleted tail event */
	events = (query_depth >= 0) ? &afterTriggers->query_stack[query_depth] : &afterTriggers->events;
	events->tail = prev_event;

	/* Release working resources */
	if (locally_opened)
	{
		if (rel)
			heap_close(rel, NoLock);
		if (trigdesc)
			FreeTriggerDesc(trigdesc);
		if (finfo)
			pfree(finfo);
		Assert(instr == NULL);	/* never used in this case */
	}
	MemoryContextDelete(per_tuple_context);
}


/* ----------
 * AfterTriggerBeginXact()
 *
 *	Called at transaction start (either BEGIN or implicit for single
 *	statement outside of transaction block).
 * ----------
 */
void
AfterTriggerBeginXact(void)
{
	Assert(afterTriggers == NULL);

	/*
	 * Build empty after-trigger state structure
	 */
	afterTriggers = (AfterTriggers)
		MemoryContextAlloc(TopTransactionContext,
						   sizeof(AfterTriggersData));

	afterTriggers->firing_counter = FirstCommandId;
	afterTriggers->state = SetConstraintStateCreate(8);
	afterTriggers->events.head = NULL;
	afterTriggers->events.tail = NULL;
	afterTriggers->query_depth = -1;

	/* We initialize the query stack to a reasonable size */
	afterTriggers->query_stack = (AfterTriggerEventList *)
		MemoryContextAlloc(TopTransactionContext,
						   8 * sizeof(AfterTriggerEventList));
	afterTriggers->maxquerydepth = 8;

	/* Context for events is created only when needed */
	afterTriggers->event_cxt = NULL;

	/* Subtransaction stack is empty until/unless needed */
	afterTriggers->cxt_stack = NULL;
	afterTriggers->state_stack = NULL;
	afterTriggers->events_stack = NULL;
	afterTriggers->depth_stack = NULL;
	afterTriggers->firing_stack = NULL;
	afterTriggers->maxtransdepth = 0;
}


/* ----------
 * AfterTriggerBeginQuery()
 *
 *	Called just before we start processing a single query within a
 *	transaction (or subtransaction).  Set up to record AFTER trigger
 *	events queued by the query.  Note that it is allowed to have
 *	nested queries within a (sub)transaction.
 * ----------
 */
void
AfterTriggerBeginQuery(void)
{
	/* Must be inside a transaction */
	Assert(afterTriggers != NULL);

	/* Increase the query stack depth */
	afterTriggers->query_depth++;

	/*
	 * Allocate more space in the query stack if needed.
	 */
	if (afterTriggers->query_depth >= afterTriggers->maxquerydepth)
	{
		/* repalloc will keep the stack in the same context */
		int			new_alloc = afterTriggers->maxquerydepth * 2;

		afterTriggers->query_stack = (AfterTriggerEventList *)
			repalloc(afterTriggers->query_stack,
					 new_alloc * sizeof(AfterTriggerEventList));
		afterTriggers->maxquerydepth = new_alloc;
	}

	/* Initialize this query's list to empty */
	afterTriggers->query_stack[afterTriggers->query_depth].head = NULL;
	afterTriggers->query_stack[afterTriggers->query_depth].tail = NULL;
}


/* ----------
 * AfterTriggerEndQuery()
 *
 *	Called after one query has been completely processed. At this time
 *	we invoke all AFTER IMMEDIATE trigger events queued by the query, and
 *	transfer deferred trigger events to the global deferred-trigger list.
 *
 *	Note that this should be called just BEFORE closing down the executor
 *	with ExecutorEnd, because we make use of the EState's info about
 *	target relations.
 * ----------
 */
void
AfterTriggerEndQuery(EState *estate)
{
	/* Must be inside a transaction */
	Assert(afterTriggers != NULL);

	/* Must be inside a query, too */
	Assert(afterTriggers->query_depth >= 0);

	/*
	 * Process all immediate-mode triggers queued by the query, and move the
	 * deferred ones to the main list of deferred events.
	 *
	 * Notice that we decide which ones will be fired, and put the deferred
	 * ones on the main list, before anything is actually fired.  This ensures
	 * reasonably sane behavior if a trigger function does SET CONSTRAINTS ...
	 * IMMEDIATE: all events we have decided to defer will be available for it
	 * to fire.
	 *
	 * We loop in case a trigger queues more events at the same query level
	 * (is that even possible?).  Be careful here: firing a trigger could
	 * result in query_stack being repalloc'd, so we can't save its address
	 * across afterTriggerInvokeEvents calls.
	 *
	 * If we find no firable events, we don't have to increment
	 * firing_counter.
	 */
	while (afterTriggerMarkEvents(&afterTriggers->query_stack[afterTriggers->query_depth], &afterTriggers->events, true))
	{
		CommandId	firing_id = afterTriggers->firing_counter++;

		/* OK to delete the immediate events after processing them */
		afterTriggerInvokeEvents(afterTriggers->query_depth, firing_id, estate, true);
	}

	afterTriggers->query_depth--;
}


/* ----------
 * AfterTriggerFireDeferred()
 *
 *	Called just before the current transaction is committed. At this
 *	time we invoke all pending DEFERRED triggers.
 *
 *	It is possible for other modules to queue additional deferred triggers
 *	during pre-commit processing; therefore xact.c may have to call this
 *	multiple times.
 * ----------
 */
void
AfterTriggerFireDeferred(void)
{
	AfterTriggerEventList *events;

	/* Must be inside a transaction */
	Assert(afterTriggers != NULL);

	/* ... but not inside a query */
	Assert(afterTriggers->query_depth == -1);

	/*
	 * If there are any triggers to fire, make sure we have set a snapshot for
	 * them to use.  (Since PortalRunUtility doesn't set a snap for COMMIT, we
	 * can't assume ActiveSnapshot is valid on entry.)
	 */
	events = &afterTriggers->events;
	if (events->head != NULL)
		ActiveSnapshot = CopySnapshot(GetTransactionSnapshot());

	/*
	 * Run all the remaining triggers.	Loop until they are all gone, in
	 * case some trigger queues more for us to do.
	 */
	while (afterTriggerMarkEvents(events, NULL, false))
	{
		CommandId	firing_id = afterTriggers->firing_counter++;

		afterTriggerInvokeEvents(-1, firing_id, NULL, true);
	}

	Assert(events->head == NULL);
}


/* ----------
 * AfterTriggerEndXact()
 *
 *	The current transaction is finishing.
 *
 *	Any unfired triggers are canceled so we simply throw
 *	away anything we know.
 *
 *	Note: it is possible for this to be called repeatedly in case of
 *	error during transaction abort; therefore, do not complain if
 *	already closed down.
 * ----------
 */
void
AfterTriggerEndXact(bool isCommit)
{
	/*
	 * Forget everything we know about AFTER triggers.
	 *
	 * Since all the info is in TopTransactionContext or children thereof, we
	 * don't really need to do anything to reclaim memory.  However, the
	 * pending-events list could be large, and so it's useful to discard
	 * it as soon as possible --- especially if we are aborting because we
	 * ran out of memory for the list!
	 *
	 * (Note: any event_cxts of child subtransactions could also be
	 * deleted here, but we have no convenient way to find them, so we
	 * leave it to TopTransactionContext reset to clean them up.)
	 */
	if (afterTriggers && afterTriggers->event_cxt)
		MemoryContextDelete(afterTriggers->event_cxt);

	afterTriggers = NULL;
}

/*
 * AfterTriggerBeginSubXact()
 *
 *	Start a subtransaction.
 */
void
AfterTriggerBeginSubXact(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * Ignore call if the transaction is in aborted state.	(Probably
	 * shouldn't happen?)
	 */
	if (afterTriggers == NULL)
		return;

	/*
	 * Allocate more space in the stacks if needed.  (Note: because the
	 * minimum nest level of a subtransaction is 2, we waste the first
	 * couple entries of each array; not worth the notational effort to
	 * avoid it.)
	 */
	while (my_level >= afterTriggers->maxtransdepth)
	{
		if (afterTriggers->maxtransdepth == 0)
		{
			MemoryContext old_cxt;

			old_cxt = MemoryContextSwitchTo(TopTransactionContext);

#define DEFTRIG_INITALLOC 8
			afterTriggers->cxt_stack = (MemoryContext *)
				palloc(DEFTRIG_INITALLOC * sizeof(MemoryContext));
			afterTriggers->state_stack = (SetConstraintState *)
				palloc(DEFTRIG_INITALLOC * sizeof(SetConstraintState));
			afterTriggers->events_stack = (AfterTriggerEventList *)
				palloc(DEFTRIG_INITALLOC * sizeof(AfterTriggerEventList));
			afterTriggers->depth_stack = (int *)
				palloc(DEFTRIG_INITALLOC * sizeof(int));
			afterTriggers->firing_stack = (CommandId *)
				palloc(DEFTRIG_INITALLOC * sizeof(CommandId));
			afterTriggers->maxtransdepth = DEFTRIG_INITALLOC;

			MemoryContextSwitchTo(old_cxt);
		}
		else
		{
			/* repalloc will keep the stacks in the same context */
			int			new_alloc = afterTriggers->maxtransdepth * 2;

			afterTriggers->cxt_stack = (MemoryContext *)
				repalloc(afterTriggers->cxt_stack,
						 new_alloc * sizeof(MemoryContext));
			afterTriggers->state_stack = (SetConstraintState *)
				repalloc(afterTriggers->state_stack,
						 new_alloc * sizeof(SetConstraintState));
			afterTriggers->events_stack = (AfterTriggerEventList *)
				repalloc(afterTriggers->events_stack,
						 new_alloc * sizeof(AfterTriggerEventList));
			afterTriggers->depth_stack = (int *)
				repalloc(afterTriggers->depth_stack,
						 new_alloc * sizeof(int));
			afterTriggers->firing_stack = (CommandId *)
				repalloc(afterTriggers->firing_stack,
						 new_alloc * sizeof(CommandId));
			afterTriggers->maxtransdepth = new_alloc;
		}
	}

	/*
	 * Push the current information into the stack.  The SET CONSTRAINTS state
	 * is not saved until/unless changed.  Likewise, we don't make a
	 * per-subtransaction event context until needed.
	 */
	afterTriggers->cxt_stack[my_level] = NULL;
	afterTriggers->state_stack[my_level] = NULL;
	afterTriggers->events_stack[my_level] = afterTriggers->events;
	afterTriggers->depth_stack[my_level] = afterTriggers->query_depth;
	afterTriggers->firing_stack[my_level] = afterTriggers->firing_counter;
}

/*
 * AfterTriggerEndSubXact()
 *
 *	The current subtransaction is ending.
 */
void
AfterTriggerEndSubXact(bool isCommit)
{
	int			my_level = GetCurrentTransactionNestLevel();
	SetConstraintState state;
	AfterTriggerEvent event;
	CommandId	subxact_firing_id;

	/*
	 * Ignore call if the transaction is in aborted state.	(Probably
	 * unneeded)
	 */
	if (afterTriggers == NULL)
		return;

	/*
	 * Pop the prior state if needed.
	 */
	Assert(my_level < afterTriggers->maxtransdepth);

	if (isCommit)
	{
		/* If we saved a prior state, we don't need it anymore */
		state = afterTriggers->state_stack[my_level];
		if (state != NULL)
			pfree(state);
		/* this avoids double pfree if error later: */
		afterTriggers->state_stack[my_level] = NULL;
		Assert(afterTriggers->query_depth ==
			   afterTriggers->depth_stack[my_level]);
		/*
		 * It's entirely possible that the subxact created an event_cxt but
		 * there is not anything left in it (because all the triggers were
		 * fired at end-of-statement).  If so, we should release the context
		 * to prevent memory leakage in a long sequence of subtransactions.
		 * We can detect whether there's anything of use in the context by
		 * seeing if anything was added to the global events list since
		 * subxact start.  (This test doesn't catch every case where the
		 * context is deletable; for instance maybe the only additions were
		 * from a sub-sub-xact.  But it handles the common case.)
		 */
		if (afterTriggers->cxt_stack[my_level] &&
			afterTriggers->events.tail == afterTriggers->events_stack[my_level].tail)
		{
			MemoryContextDelete(afterTriggers->cxt_stack[my_level]);
			/* avoid double delete if abort later */
			afterTriggers->cxt_stack[my_level] = NULL;
		}
	}
	else
	{
		/*
		 * Aborting.  We don't really need to release the subxact's event_cxt,
		 * since it will go away anyway when CurTransactionContext gets reset,
		 * but doing so early in subxact abort helps free space we might need.
		 *
		 * (Note: any event_cxts of child subtransactions could also be
		 * deleted here, but we have no convenient way to find them, so we
		 * leave it to CurTransactionContext reset to clean them up.)
		 */
		if (afterTriggers->cxt_stack[my_level])
		{
			MemoryContextDelete(afterTriggers->cxt_stack[my_level]);
			/* avoid double delete if repeated aborts */
			afterTriggers->cxt_stack[my_level] = NULL;
		}

		/*
		 * Restore the pointers from the stacks.
		 */
		afterTriggers->events = afterTriggers->events_stack[my_level];
		afterTriggers->query_depth = afterTriggers->depth_stack[my_level];

		/*
		 * Cleanup the tail of the list.
		 */
		if (afterTriggers->events.tail != NULL)
			afterTriggers->events.tail->ate_next = NULL;

		/*
		 * Restore the trigger state.  If the saved state is NULL, then this
		 * subxact didn't save it, so it doesn't need restoring.
		 */
		state = afterTriggers->state_stack[my_level];
		if (state != NULL)
		{
			pfree(afterTriggers->state);
			afterTriggers->state = state;
		}
		/* this avoids double pfree if error later: */
		afterTriggers->state_stack[my_level] = NULL;

		/*
		 * Scan for any remaining deferred events that were marked DONE or IN
		 * PROGRESS by this subxact or a child, and un-mark them. We can
		 * recognize such events because they have a firing ID greater than or
		 * equal to the firing_counter value we saved at subtransaction start.
		 * (This essentially assumes that the current subxact includes all
		 * subxacts started after it.)
		 */
		subxact_firing_id = afterTriggers->firing_stack[my_level];
		for (event = afterTriggers->events.head;
			 event != NULL;
			 event = event->ate_next)
		{
			if (event->ate_event &
				(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS))
			{
				if (event->ate_firing_id >= subxact_firing_id)
					event->ate_event &=
						~(AFTER_TRIGGER_DONE | AFTER_TRIGGER_IN_PROGRESS);
			}
		}
	}
}

/*
 * Create an empty SetConstraintState with room for numalloc trigstates
 */
static SetConstraintState
SetConstraintStateCreate(int numalloc)
{
	SetConstraintState state;

	/* Behave sanely with numalloc == 0 */
	if (numalloc <= 0)
		numalloc = 1;

	/*
	 * We assume that zeroing will correctly initialize the state values.
	 */
	state = (SetConstraintState)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(SetConstraintStateData) +
						   (numalloc - 1) *sizeof(SetConstraintTriggerData));

	state->numalloc = numalloc;

	return state;
}

/*
 * Copy a SetConstraintState
 */
static SetConstraintState
SetConstraintStateCopy(SetConstraintState origstate)
{
	SetConstraintState state;

	state = SetConstraintStateCreate(origstate->numstates);

	state->all_isset = origstate->all_isset;
	state->all_isdeferred = origstate->all_isdeferred;
	state->numstates = origstate->numstates;
	memcpy(state->trigstates, origstate->trigstates,
		   origstate->numstates * sizeof(SetConstraintTriggerData));

	return state;
}

/*
 * Add a per-trigger item to a SetConstraintState.	Returns possibly-changed
 * pointer to the state object (it will change if we have to repalloc).
 */
static SetConstraintState
SetConstraintStateAddItem(SetConstraintState state,
						  Oid tgoid, bool tgisdeferred)
{
	if (state->numstates >= state->numalloc)
	{
		int			newalloc = state->numalloc * 2;

		newalloc = Max(newalloc, 8);	/* in case original has size 0 */
		state = (SetConstraintState)
			repalloc(state,
					 sizeof(SetConstraintStateData) +
					 (newalloc - 1) *sizeof(SetConstraintTriggerData));
		state->numalloc = newalloc;
		Assert(state->numstates < state->numalloc);
	}

	state->trigstates[state->numstates].sct_tgoid = tgoid;
	state->trigstates[state->numstates].sct_tgisdeferred = tgisdeferred;
	state->numstates++;

	return state;
}

/* ----------
 * AfterTriggerSetState()
 *
 *	Execute the SET CONSTRAINTS ... utility command.
 * ----------
 */
void
AfterTriggerSetState(ConstraintsSetStmt *stmt)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * Ignore call if we aren't in a transaction.  (Shouldn't happen?)
	 */
	if (afterTriggers == NULL)
		return;

	/*
	 * If in a subtransaction, and we didn't save the current state already,
	 * save it so it can be restored if the subtransaction aborts.
	 */
	if (my_level > 1 &&
		afterTriggers->state_stack[my_level] == NULL)
	{
		afterTriggers->state_stack[my_level] =
			SetConstraintStateCopy(afterTriggers->state);
	}

	/*
	 * Handle SET CONSTRAINTS ALL ...
	 */
	if (stmt->constraints == NIL)
	{
		/*
		 * Forget any previous SET CONSTRAINTS commands in this transaction.
		 */
		afterTriggers->state->numstates = 0;

		/*
		 * Set the per-transaction ALL state to known.
		 */
		afterTriggers->state->all_isset = true;
		afterTriggers->state->all_isdeferred = stmt->deferred;
	}
	else
	{
		Relation	tgrel;
		ListCell   *l;
		List	   *oidlist = NIL;

		/* ----------
		 * Handle SET CONSTRAINTS constraint-name [, ...]
		 * First lookup all trigger Oid's for the constraint names.
		 * ----------
		 */
		tgrel = heap_open(TriggerRelationId, AccessShareLock);

		foreach(l, stmt->constraints)
		{
			RangeVar   *constraint = lfirst(l);
			ScanKeyData skey;
			SysScanDesc tgscan;
			HeapTuple	htup;
			bool		found;
			List	   *namespaceSearchList;
			ListCell   *namespaceSearchCell;

			if (constraint->catalogname)
			{
				if (strcmp(constraint->catalogname, get_database_name(MyDatabaseId)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							 constraint->catalogname, constraint->schemaname,
									constraint->relname)));
			}

			/*
			 * If we're given the schema name with the constraint, look only
			 * in that schema.	If given a bare constraint name, use the
			 * search path to find the first matching constraint.
			 */
			if (constraint->schemaname)
			{
				Oid			namespaceId = LookupExplicitNamespace(constraint->schemaname);

				namespaceSearchList = list_make1_oid(namespaceId);
			}
			else
			{
				namespaceSearchList = fetch_search_path(true);
			}

			found = false;
			foreach(namespaceSearchCell, namespaceSearchList)
			{
				Oid			searchNamespaceId = lfirst_oid(namespaceSearchCell);

				/*
				 * Setup to scan pg_trigger by tgconstrname ...
				 */
				ScanKeyInit(&skey,
							Anum_pg_trigger_tgconstrname,
							BTEqualStrategyNumber, F_NAMEEQ,
							PointerGetDatum(constraint->relname));

				tgscan = systable_beginscan(tgrel, TriggerConstrNameIndexId, true,
											SnapshotNow, 1, &skey);

				/*
				 * ... and search for the constraint trigger row
				 */
				while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
				{
					Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);
					Oid			constraintNamespaceId;

					/*
					 * Foreign key constraints have triggers on both the
					 * parent and child tables.  Since these tables may be in
					 * different schemas we must pick the child table because
					 * that table "owns" the constraint.
					 *
					 * Referential triggers on the parent table other than
					 * NOACTION_DEL and NOACTION_UPD are ignored below, so it
					 * is possible to not check them here, but it seems safer
					 * to always check.
					 */
					if (pg_trigger->tgfoid == F_RI_FKEY_NOACTION_DEL ||
						pg_trigger->tgfoid == F_RI_FKEY_NOACTION_UPD ||
						pg_trigger->tgfoid == F_RI_FKEY_RESTRICT_UPD ||
						pg_trigger->tgfoid == F_RI_FKEY_RESTRICT_DEL ||
						pg_trigger->tgfoid == F_RI_FKEY_CASCADE_UPD ||
						pg_trigger->tgfoid == F_RI_FKEY_CASCADE_DEL ||
						pg_trigger->tgfoid == F_RI_FKEY_SETNULL_UPD ||
						pg_trigger->tgfoid == F_RI_FKEY_SETNULL_DEL ||
						pg_trigger->tgfoid == F_RI_FKEY_SETDEFAULT_UPD ||
						pg_trigger->tgfoid == F_RI_FKEY_SETDEFAULT_DEL)
						constraintNamespaceId = get_rel_namespace(pg_trigger->tgconstrrelid);
					else
						constraintNamespaceId = get_rel_namespace(pg_trigger->tgrelid);

					/*
					 * If this constraint is not in the schema we're currently
					 * searching for, keep looking.
					 */
					if (constraintNamespaceId != searchNamespaceId)
						continue;

					/*
					 * If we found some, check that they fit the deferrability
					 * but skip referential action ones, since they are
					 * silently never deferrable.
					 */
					if (pg_trigger->tgfoid != F_RI_FKEY_RESTRICT_UPD &&
						pg_trigger->tgfoid != F_RI_FKEY_RESTRICT_DEL &&
						pg_trigger->tgfoid != F_RI_FKEY_CASCADE_UPD &&
						pg_trigger->tgfoid != F_RI_FKEY_CASCADE_DEL &&
						pg_trigger->tgfoid != F_RI_FKEY_SETNULL_UPD &&
						pg_trigger->tgfoid != F_RI_FKEY_SETNULL_DEL &&
						pg_trigger->tgfoid != F_RI_FKEY_SETDEFAULT_UPD &&
						pg_trigger->tgfoid != F_RI_FKEY_SETDEFAULT_DEL)
					{
						if (stmt->deferred && !pg_trigger->tgdeferrable)
							ereport(ERROR,
									(errcode(ERRCODE_WRONG_OBJECT_TYPE),
								errmsg("constraint \"%s\" is not deferrable",
									   constraint->relname)));
						oidlist = lappend_oid(oidlist, HeapTupleGetOid(htup));
					}
					found = true;
				}

				systable_endscan(tgscan);

				/*
				 * Once we've found a matching constraint we do not search
				 * later parts of the search path.
				 */
				if (found)
					break;

			}

			list_free(namespaceSearchList);

			/*
			 * Not found ?
			 */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("constraint \"%s\" does not exist",
								constraint->relname)));
		}
		heap_close(tgrel, AccessShareLock);

		/*
		 * Set the trigger states of individual triggers for this xact.
		 */
		foreach(l, oidlist)
		{
			Oid			tgoid = lfirst_oid(l);
			SetConstraintState state = afterTriggers->state;
			bool		found = false;
			int			i;

			for (i = 0; i < state->numstates; i++)
			{
				if (state->trigstates[i].sct_tgoid == tgoid)
				{
					state->trigstates[i].sct_tgisdeferred = stmt->deferred;
					found = true;
					break;
				}
			}
			if (!found)
			{
				afterTriggers->state =
					SetConstraintStateAddItem(state, tgoid, stmt->deferred);
			}
		}
	}

	/*
	 * SQL99 requires that when a constraint is set to IMMEDIATE, any deferred
	 * checks against that constraint must be made when the SET CONSTRAINTS
	 * command is executed -- i.e. the effects of the SET CONSTRAINTS command
	 * apply retroactively.  We've updated the constraints state, so scan the
	 * list of previously deferred events to fire any that have now become
	 * immediate.
	 *
	 * Obviously, if this was SET ... DEFERRED then it can't have converted
	 * any unfired events to immediate, so we need do nothing in that case.
	 */
	if (!stmt->deferred)
	{
		AfterTriggerEventList *events = &afterTriggers->events;
		Snapshot	saveActiveSnapshot = ActiveSnapshot;

		/* PG_TRY to ensure previous ActiveSnapshot is restored on error */
		PG_TRY();
		{
			Snapshot	mySnapshot = NULL;

			while (afterTriggerMarkEvents(events, NULL, true))
			{
				CommandId	firing_id = afterTriggers->firing_counter++;

				/*
				 * Make sure a snapshot has been established in case trigger
				 * functions need one.  Note that we avoid setting a snapshot
				 * if we don't find at least one trigger that has to be fired
				 * now.  This is so that BEGIN; SET CONSTRAINTS ...; SET
				 * TRANSACTION ISOLATION LEVEL SERIALIZABLE; ... works
				 * properly.  (If we are at the start of a transaction it's
				 * not possible for any trigger events to be queued yet.)
				 */
				if (mySnapshot == NULL)
				{
					mySnapshot = CopySnapshot(GetTransactionSnapshot());
					ActiveSnapshot = mySnapshot;
				}

				/*
				 * We can delete fired events if we are at top transaction level,
				 * but we'd better not if inside a subtransaction, since the
				 * subtransaction could later get rolled back.
				 */
				afterTriggerInvokeEvents(-1, firing_id, NULL,
										 !IsSubTransaction());
			}

			if (mySnapshot)
				FreeSnapshot(mySnapshot);
		}
		PG_CATCH();
		{
			ActiveSnapshot = saveActiveSnapshot;
			PG_RE_THROW();
		}
		PG_END_TRY();
		ActiveSnapshot = saveActiveSnapshot;
	}
}

/* ----------
 * AfterTriggerPendingOnRel()
 *		Test to see if there are any pending after-trigger events for rel.
 *
 * This is used by TRUNCATE, CLUSTER, ALTER TABLE, etc to detect whether
 * it is unsafe to perform major surgery on a relation.  Note that only
 * local pending events are examined.  We assume that having exclusive lock
 * on a rel guarantees there are no unserviced events in other backends ---
 * but having a lock does not prevent there being such events in our own.
 *
 * In some scenarios it'd be reasonable to remove pending events (more
 * specifically, mark them DONE by the current subxact) but without a lot
 * of knowledge of the trigger semantics we can't do this in general.
 * ----------
 */
bool
AfterTriggerPendingOnRel(Oid relid)
{
	AfterTriggerEvent event;
	int			depth;

	/* No-op if we aren't in a transaction.  (Shouldn't happen?) */
	if (afterTriggers == NULL)
		return false;

	/* Scan queued events */
	for (event = afterTriggers->events.head;
		 event != NULL;
		 event = event->ate_next)
	{
		/*
		 * We can ignore completed events.	(Even if a DONE flag is rolled
		 * back by subxact abort, it's OK because the effects of the TRUNCATE
		 * or whatever must get rolled back too.)
		 */
		if (event->ate_event & AFTER_TRIGGER_DONE)
			continue;

		if (event->ate_relid == relid)
			return true;
	}

	/*
	 * Also scan events queued by incomplete queries.  This could only matter
	 * if TRUNCATE/etc is executed by a function or trigger within an updating
	 * query on the same relation, which is pretty perverse, but let's check.
	 */
	for (depth = 0; depth <= afterTriggers->query_depth; depth++)
	{
		for (event = afterTriggers->query_stack[depth].head;
			 event != NULL;
			 event = event->ate_next)
		{
			if (event->ate_event & AFTER_TRIGGER_DONE)
				continue;

			if (event->ate_relid == relid)
				return true;
		}
	}

	return false;
}


/* ----------
 * AfterTriggerSaveEvent()
 *
 *	Called by ExecA[RS]...Triggers() to add the event to the queue.
 *
 *	NOTE: should be called only if we've determined that an event must
 *	be added to the queue.
 * ----------
 */
static void
AfterTriggerSaveEvent(ResultRelInfo *relinfo, int event, bool row_trigger,
					  HeapTuple oldtup, HeapTuple newtup)
{
	Relation	rel = relinfo->ri_RelationDesc;
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	int			my_level = GetCurrentTransactionNestLevel();
	MemoryContext *cxtptr;
	AfterTriggerEvent new_event;
	int			i;
	int			ntriggers;
	int		   *tgindx;
	ItemPointerData oldctid;
	ItemPointerData newctid;

	if (afterTriggers == NULL)
		elog(ERROR, "AfterTriggerSaveEvent() called outside of transaction");

	/*
	 * Get the CTID's of OLD and NEW
	 */
	if (oldtup != NULL)
		ItemPointerCopy(&(oldtup->t_self), &(oldctid));
	else
		ItemPointerSetInvalid(&(oldctid));
	if (newtup != NULL)
		ItemPointerCopy(&(newtup->t_self), &(newctid));
	else
		ItemPointerSetInvalid(&(newctid));

	/*
	 * Scan the appropriate set of triggers
	 */
	if (row_trigger)
	{
		ntriggers = trigdesc->n_after_row[event];
		tgindx = trigdesc->tg_after_row[event];
	}
	else
	{
		ntriggers = trigdesc->n_after_statement[event];
		tgindx = trigdesc->tg_after_statement[event];
	}

	for (i = 0; i < ntriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];

		/* Ignore disabled triggers */
		if (!trigger->tgenabled)
			continue;

		/*
		 * If this is an UPDATE of a PK table or FK table that does not change
		 * the PK or FK respectively, we can skip queuing the event: there is
		 * no need to fire the trigger.
		 */
		if ((event & TRIGGER_EVENT_OPMASK) == TRIGGER_EVENT_UPDATE)
		{
			switch (RI_FKey_trigger_type(trigger->tgfoid))
			{
				case RI_TRIGGER_PK:
					/* Update on PK table */
					if (RI_FKey_keyequal_upd_pk(trigger, rel, oldtup, newtup))
					{
						/* key unchanged, so skip queuing this event */
						continue;
					}
					break;

				case RI_TRIGGER_FK:

					/*
					 * Update on FK table
					 *
					 * There is one exception when updating FK tables: if the
					 * updated row was inserted by our own transaction and the
					 * FK is deferred, we still need to fire the trigger. This
					 * is because our UPDATE will invalidate the INSERT so the
					 * end-of-transaction INSERT RI trigger will not do
					 * anything, so we have to do the check for the UPDATE
					 * anyway.
					 */
					if (!TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(oldtup->t_data)) &&
						RI_FKey_keyequal_upd_fk(trigger, rel, oldtup, newtup))
					{
						continue;
					}
					break;

				case RI_TRIGGER_NONE:
					/* Not an FK trigger */
					break;
			}
		}

		/*
		 * If we don't yet have an event context for the current (sub)xact,
		 * create one.  Make it a child of CurTransactionContext to ensure it
		 * will go away if the subtransaction aborts.
		 */
		if (my_level > 1)			/* subtransaction? */
		{
			Assert(my_level < afterTriggers->maxtransdepth);
			cxtptr = &afterTriggers->cxt_stack[my_level];
		}
		else
			cxtptr = &afterTriggers->event_cxt;
		if (*cxtptr == NULL)
			*cxtptr = AllocSetContextCreate(CurTransactionContext,
											"AfterTriggerEvents",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);

		/*
		 * Create a new event.
		 */
		new_event = (AfterTriggerEvent)
			MemoryContextAlloc(*cxtptr, sizeof(AfterTriggerEventData));
		new_event->ate_next = NULL;
		new_event->ate_event =
			(event & TRIGGER_EVENT_OPMASK) |
			(row_trigger ? TRIGGER_EVENT_ROW : 0) |
			(trigger->tgdeferrable ? AFTER_TRIGGER_DEFERRABLE : 0) |
			(trigger->tginitdeferred ? AFTER_TRIGGER_INITDEFERRED : 0);
		new_event->ate_firing_id = 0;
		new_event->ate_tgoid = trigger->tgoid;
		new_event->ate_relid = rel->rd_id;
		ItemPointerCopy(&oldctid, &(new_event->ate_oldctid));
		ItemPointerCopy(&newctid, &(new_event->ate_newctid));

		/*
		 * Add the new event to the queue.
		 */
		afterTriggerAddEvent(new_event);
	}
}
