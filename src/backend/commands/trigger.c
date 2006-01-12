/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/trigger.c,v 1.159.2.2 2006/01/12 21:49:31 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


static void InsertTrigger(TriggerDesc *trigdesc, Trigger *trigger, int indx);
static HeapTuple GetTupleForTrigger(EState *estate,
				   ResultRelInfo *relinfo,
				   ItemPointer tid,
				   CommandId cid,
				   TupleTableSlot **newSlot);
static HeapTuple ExecCallTriggerFunc(TriggerData *trigdata,
					FmgrInfo *finfo,
					MemoryContext per_tuple_context);
static void DeferredTriggerSaveEvent(ResultRelInfo *relinfo, int event,
				   bool row_trigger, HeapTuple oldtup, HeapTuple newtup);
static void DeferredTriggerExecute(DeferredTriggerEvent event, int itemno,
					Relation rel, TriggerDesc *trigdesc, FmgrInfo *finfo,
					   MemoryContext per_tuple_context);


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
	int16		tgattr[FUNC_MAX_ARGS];
	Datum		values[Natts_pg_trigger];
	char		nulls[Natts_pg_trigger];
	Relation	rel;
	AclResult	aclresult;
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData key;
	Relation	pgrel;
	HeapTuple	tuple;
	Oid			fargtypes[FUNC_MAX_ARGS];
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
		 * really need a constrrelid.  Since we don't have one, we'll try
		 * to generate one from the argument information.
		 *
		 * This is really just a workaround for a long-ago pg_dump bug that
		 * omitted the FROM clause in dumped CREATE CONSTRAINT TRIGGER
		 * commands.  We don't want to bomb out completely here if we
		 * can't determine the correct relation, because that would
		 * prevent loading the dump file.  Instead, NOTICE here and ERROR
		 * in the trigger.
		 */
		bool		needconstrrelid = false;
		void	   *elem = NULL;

		if (strncmp(strVal(llast(stmt->funcname)), "RI_FKey_check_", 14) == 0)
		{
			/* A trigger on FK table. */
			needconstrrelid = true;
			if (length(stmt->args) > RI_PK_RELNAME_ARGNO)
				elem = nth(RI_PK_RELNAME_ARGNO, stmt->args);
		}
		else if (strncmp(strVal(llast(stmt->funcname)), "RI_FKey_", 8) == 0)
		{
			/* A trigger on PK table. */
			needconstrrelid = true;
			if (length(stmt->args) > RI_FK_RELNAME_ARGNO)
				elem = nth(RI_FK_RELNAME_ARGNO, stmt->args);
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
	 * Generate the trigger's OID now, so that we can use it in the name
	 * if needed.
	 */
	trigoid = newoid();

	/*
	 * If trigger is an RI constraint, use specified trigger name as
	 * constraint name and build a unique trigger name instead. This is
	 * mainly for backwards compatibility with CREATE CONSTRAINT TRIGGER
	 * commands.
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
	 * Scan pg_trigger for existing triggers on relation.  We do this
	 * mainly because we must count them; a secondary benefit is to give a
	 * nice error message if there's already a trigger of the same name.
	 * (The unique index on tgrelid/tgname would complain anyway.)
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on the
	 * relation, so the trigger set won't be changing underneath us.
	 */
	tgrel = heap_openr(TriggerRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&key, 0,
						   Anum_pg_trigger_tgrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(rel)));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndex, true,
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
	MemSet(fargtypes, 0, FUNC_MAX_ARGS * sizeof(Oid));
	funcoid = LookupFuncName(stmt->funcname, 0, fargtypes, false);
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != TRIGGEROID)
	{
		/*
		 * We allow OPAQUE just so we can load old dump files.	When we
		 * see a trigger function declared OPAQUE, change it to TRIGGER.
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
		List	   *le;
		char	   *args;
		int16		nargs = length(stmt->args);
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
	MemSet(tgattr, 0, FUNC_MAX_ARGS * sizeof(int16));
	values[Anum_pg_trigger_tgattr - 1] = PointerGetDatum(tgattr);

	tuple = heap_formtuple(tgrel->rd_att, values, nulls);

	/* force tuple to have the desired OID */
	HeapTupleSetOid(tuple, trigoid);

	/*
	 * Insert tuple into pg_trigger.
	 */
	simple_heap_insert(tgrel, tuple);

	CatalogUpdateIndexes(tgrel, tuple);

	myself.classId = RelationGetRelid(tgrel);
	myself.objectId = trigoid;
	myself.objectSubId = 0;

	heap_freetuple(tuple);
	heap_close(tgrel, RowExclusiveLock);

	pfree(DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other
	 * backends (and this one too!) are sent SI message to make them
	 * rebuild relcache entries.
	 */
	pgrel = heap_openr(RelationRelationName, RowExclusiveLock);
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
	 * fairly pointless since it will happen as a byproduct of the
	 * upcoming CommandCounterIncrement...
	 */

	/*
	 * Record dependencies for trigger.  Always place a normal dependency
	 * on the function.  If we are doing this in response to an explicit
	 * CREATE TRIGGER command, also make trigger be auto-dropped if its
	 * relation is dropped or if the FK relation is dropped.  (Auto drop
	 * is compatible with our pre-7.3 behavior.)  If the trigger is being
	 * made for a constraint, we can skip the relation links; the
	 * dependency on the constraint will indirectly depend on the
	 * relations.
	 */
	referenced.classId = RelOid_pg_proc;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (!forConstraint)
	{
		referenced.classId = RelOid_pg_class;
		referenced.objectId = RelationGetRelid(rel);
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);
		if (constrrelid != InvalidOid)
		{
			referenced.classId = RelOid_pg_class;
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
DropTrigger(Oid relid, const char *trigname, DropBehavior behavior)
{
	Relation	tgrel;
	ScanKeyData skey[2];
	SysScanDesc tgscan;
	HeapTuple	tup;
	ObjectAddress object;

	/*
	 * Find the trigger, verify permissions, set up object address
	 */
	tgrel = heap_openr(TriggerRelationName, AccessShareLock);

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   Anum_pg_trigger_tgrelid, F_OIDEQ,
						   ObjectIdGetDatum(relid));

	ScanKeyEntryInitialize(&skey[1], 0x0,
						   Anum_pg_trigger_tgname, F_NAMEEQ,
						   CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndex, true,
								SnapshotNow, 2, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
			  errmsg("trigger \"%s\" for table \"%s\" does not exist",
					 trigname, get_rel_name(relid))));

	if (!pg_class_ownercheck(relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_CLASS,
					   get_rel_name(relid));

	object.classId = RelationGetRelid(tgrel);
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

	tgrel = heap_openr(TriggerRelationName, RowExclusiveLock);

	/*
	 * Find the trigger to delete.
	 */
	ScanKeyEntryInitialize(&skey[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(trigOid));

	tgscan = systable_beginscan(tgrel, TriggerOidIndex, true,
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
	 * Update relation's pg_class entry.  Crucial side-effect: other
	 * backends (and this one too!) are sent SI message to make them
	 * rebuild relcache entries.
	 *
	 * Note this is OK only because we have AccessExclusiveLock on the rel,
	 * so no one else is creating/deleting triggers on this rel at the
	 * same time.
	 */
	pgrel = heap_openr(RelationRelationName, RowExclusiveLock);
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
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	targetrel = heap_open(relid, AccessExclusiveLock);

	/*
	 * Scan pg_trigger twice for existing triggers on relation.  We do
	 * this in order to ensure a trigger does not exist with newname (The
	 * unique index on tgrelid/tgname would complain anyway) and to ensure
	 * a trigger does exist with oldname.
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on the
	 * relation, so the trigger set won't be changing underneath us.
	 */
	tgrel = heap_openr(TriggerRelationName, RowExclusiveLock);

	/*
	 * First pass -- look for name conflict
	 */
	ScanKeyEntryInitialize(&key[0], 0,
						   Anum_pg_trigger_tgrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(relid));
	ScanKeyEntryInitialize(&key[1], 0,
						   Anum_pg_trigger_tgname,
						   F_NAMEEQ,
						   PointerGetDatum(newname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndex, true,
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
	ScanKeyEntryInitialize(&key[0], 0,
						   Anum_pg_trigger_tgrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(relid));
	ScanKeyEntryInitialize(&key[1], 0,
						   Anum_pg_trigger_tgname,
						   F_NAMEEQ,
						   PointerGetDatum(oldname));
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndex, true,
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
		 * Invalidate relation's relcache entry so that other backends
		 * (and this one too!) are sent SI message to make them rebuild
		 * relcache entries.  (Ideally this should happen
		 * automatically...)
		 */
		CacheInvalidateRelcache(relid);
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
	 * Note: since we scan the triggers using TriggerRelidNameIndex, we
	 * will be reading the triggers in name order, except possibly during
	 * emergency-recovery operations (ie, IsIgnoringSystemIndexes). This
	 * in turn ensures that triggers will be fired in name order.
	 */
	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) Anum_pg_trigger_tgrelid,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	tgrel = heap_openr(TriggerRelationName, AccessShareLock);
	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndex, true,
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
		memcpy(build->tgattr, &(pg_trigger->tgattr),
			   FUNC_MAX_ARGS * sizeof(int16));
		if (build->tgnargs > 0)
		{
			bytea	   *val;
			bool		isnull;
			char	   *p;
			int			i;

			val = (bytea *) fastgetattr(htup,
										Anum_pg_trigger_tgargs,
										tgrel->rd_att, &isnull);
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
	 * We need not examine the "index" data, just the trigger array
	 * itself; if we have the same triggers with the same types, the
	 * derived index data should match.
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
			if (memcmp(trig1->tgattr, trig2->tgattr,
					   sizeof(trig1->tgattr)) != 0)
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
 *		finfo: possibly-cached call info for the function.
 *		per_tuple_context: memory context to execute the function in.
 *
 * Returns the tuple (or NULL) as returned by the function.
 */
static HeapTuple
ExecCallTriggerFunc(TriggerData *trigdata,
					FmgrInfo *finfo,
					MemoryContext per_tuple_context)
{
	FunctionCallInfoData fcinfo;
	Datum		result;
	MemoryContext oldContext;

	/*
	 * We cache fmgr lookup info, to avoid making the lookup again on each
	 * call.
	 */
	if (finfo->fn_oid == InvalidOid)
		fmgr_info(trigdata->tg_trigger->tgfoid, finfo);

	Assert(finfo->fn_oid == trigdata->tg_trigger->tgfoid);

	/*
	 * Do the function evaluation in the per-tuple memory context, so that
	 * leaked memory will be reclaimed once per tuple. Note in particular
	 * that any new tuple created by the trigger function will live till
	 * the end of the tuple cycle.
	 */
	oldContext = MemoryContextSwitchTo(per_tuple_context);

	/*
	 * Call the function, passing no arguments but setting a context.
	 */
	MemSet(&fcinfo, 0, sizeof(fcinfo));

	fcinfo.flinfo = finfo;
	fcinfo.context = (Node *) trigdata;

	result = FunctionCallInvoke(&fcinfo);

	MemoryContextSwitchTo(oldContext);

	/*
	 * Trigger protocol allows function to return a null pointer, but NOT
	 * to set the isnull result flag.
	 */
	if (fcinfo.isnull)
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("trigger function %u returned null value",
						fcinfo.flinfo->fn_oid)));

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

	/* Allocate cache space for fmgr lookup info, if not done yet */
	if (relinfo->ri_TrigFunctions == NULL)
		relinfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(trigdesc->numtriggers * sizeof(FmgrInfo));

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];
		HeapTuple	newtuple;

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
								   relinfo->ri_TrigFunctions + tgindx[i],
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
		DeferredTriggerSaveEvent(relinfo, TRIGGER_EVENT_INSERT,
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

	/* Allocate cache space for fmgr lookup info, if not done yet */
	if (relinfo->ri_TrigFunctions == NULL)
		relinfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(trigdesc->numtriggers * sizeof(FmgrInfo));

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_INSERT |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigtuple = oldtuple = newtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
								   relinfo->ri_TrigFunctions + tgindx[i],
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
		DeferredTriggerSaveEvent(relinfo, TRIGGER_EVENT_INSERT,
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

	/* Allocate cache space for fmgr lookup info, if not done yet */
	if (relinfo->ri_TrigFunctions == NULL)
		relinfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(trigdesc->numtriggers * sizeof(FmgrInfo));

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];
		HeapTuple	newtuple;

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
								   relinfo->ri_TrigFunctions + tgindx[i],
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
		DeferredTriggerSaveEvent(relinfo, TRIGGER_EVENT_DELETE,
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
	TriggerData LocTriggerData;
	HeapTuple	trigtuple;
	HeapTuple	newtuple = NULL;
	TupleTableSlot *newSlot;
	int			i;

	trigtuple = GetTupleForTrigger(estate, relinfo, tupleid, cid, &newSlot);
	if (trigtuple == NULL)
		return false;

	/* Allocate cache space for fmgr lookup info, if not done yet */
	if (relinfo->ri_TrigFunctions == NULL)
		relinfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(trigdesc->numtriggers * sizeof(FmgrInfo));

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_DELETE |
		TRIGGER_EVENT_ROW |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigtuple = trigtuple;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
								   relinfo->ri_TrigFunctions + tgindx[i],
									   GetPerTupleMemoryContext(estate));
		if (newtuple == NULL)
			break;
		if (newtuple != trigtuple)
			heap_freetuple(newtuple);
	}
	heap_freetuple(trigtuple);

	return (newtuple == NULL) ? false : true;
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

		DeferredTriggerSaveEvent(relinfo, TRIGGER_EVENT_DELETE,
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

	/* Allocate cache space for fmgr lookup info, if not done yet */
	if (relinfo->ri_TrigFunctions == NULL)
		relinfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(trigdesc->numtriggers * sizeof(FmgrInfo));

	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE |
		TRIGGER_EVENT_BEFORE;
	LocTriggerData.tg_relation = relinfo->ri_RelationDesc;
	LocTriggerData.tg_newtuple = NULL;
	LocTriggerData.tg_trigtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];
		HeapTuple	newtuple;

		if (!trigger->tgenabled)
			continue;
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
								   relinfo->ri_TrigFunctions + tgindx[i],
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
		DeferredTriggerSaveEvent(relinfo, TRIGGER_EVENT_UPDATE,
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

	/* Allocate cache space for fmgr lookup info, if not done yet */
	if (relinfo->ri_TrigFunctions == NULL)
		relinfo->ri_TrigFunctions = (FmgrInfo *)
			palloc0(trigdesc->numtriggers * sizeof(FmgrInfo));

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
		LocTriggerData.tg_trigger = trigger;
		newtuple = ExecCallTriggerFunc(&LocTriggerData,
								   relinfo->ri_TrigFunctions + tgindx[i],
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

		DeferredTriggerSaveEvent(relinfo, TRIGGER_EVENT_UPDATE,
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
		int			test;
		ItemPointerData update_ctid;
		TransactionId update_xmax;

		*newSlot = NULL;

		/*
		 * mark tuple for update
		 */
ltrmark:;
		tuple.t_self = *tid;
		test = heap_mark4update(relation, &tuple, &buffer,
								&update_ctid, &update_xmax, cid);
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
				if (XactIsoLevel == XACT_SERIALIZABLE)
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
				 * if tuple was deleted or PlanQual failed for updated
				 * tuple - we have not process this tuple!
				 */
				return NULL;

			default:
				ReleaseBuffer(buffer);
				elog(ERROR, "unrecognized heap_mark4update status: %u",
					 test);
				return NULL;	/* keep compiler quiet */
		}
	}
	else
	{
		PageHeader	dp;
		ItemId		lp;

		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

		if (!BufferIsValid(buffer))
			elog(ERROR, "ReadBuffer failed");

		dp = (PageHeader) BufferGetPage(buffer);
		lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));

		Assert(ItemIdIsUsed(lp));

		tuple.t_datamcxt = NULL;
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
 * Deferred trigger stuff
 * ----------
 */

typedef struct DeferredTriggersData
{
	/* Internal data is held in a per-transaction memory context */
	MemoryContext deftrig_cxt;
	/* ALL DEFERRED or ALL IMMEDIATE */
	bool		deftrig_all_isset;
	bool		deftrig_all_isdeferred;
	/* Per trigger state */
	List	   *deftrig_trigstates;
	/* List of pending deferred triggers. Previous comment below */
	DeferredTriggerEvent deftrig_events;
	DeferredTriggerEvent deftrig_events_imm;
	DeferredTriggerEvent deftrig_event_tail;
} DeferredTriggersData;

/* ----------
 * deftrig_events, deftrig_event_tail:
 * The list of pending deferred trigger events during the current transaction.
 *
 * deftrig_events is the head, deftrig_event_tail is the last entry.
 * Because this can grow pretty large, we don't use separate List nodes,
 * but instead thread the list through the dte_next fields of the member
 * nodes.  Saves just a few bytes per entry, but that adds up.
 *
 * deftrig_events_imm holds the tail pointer as of the last
 * deferredTriggerInvokeEvents call; we can use this to avoid rescanning
 * entries unnecessarily.  It is NULL if deferredTriggerInvokeEvents
 * hasn't run since the last state change.
 *
 * XXX Need to be able to shove this data out to a file if it grows too
 *	   large...
 * ----------
 */

typedef DeferredTriggersData *DeferredTriggers;

static DeferredTriggers deferredTriggers;

/* ----------
 * deferredTriggerCheckState()
 *
 *	Returns true if the trigger identified by tgoid is actually
 *	in state DEFERRED.
 * ----------
 */
static bool
deferredTriggerCheckState(Oid tgoid, int32 itemstate)
{
	MemoryContext oldcxt;
	List	   *sl;
	DeferredTriggerStatus trigstate;

	/*
	 * Not deferrable triggers (i.e. normal AFTER ROW triggers and
	 * constraints declared NOT DEFERRABLE, the state is always false.
	 */
	if ((itemstate & TRIGGER_DEFERRED_DEFERRABLE) == 0)
		return false;

	/*
	 * Lookup if we know an individual state for this trigger
	 */
	foreach(sl, deferredTriggers->deftrig_trigstates)
	{
		trigstate = (DeferredTriggerStatus) lfirst(sl);
		if (trigstate->dts_tgoid == tgoid)
			return trigstate->dts_tgisdeferred;
	}

	/*
	 * No individual state known - so if the user issued a SET CONSTRAINT
	 * ALL ..., we return that instead of the triggers default state.
	 */
	if (deferredTriggers->deftrig_all_isset)
		return deferredTriggers->deftrig_all_isdeferred;

	/*
	 * No ALL state known either, remember the default state as the
	 * current and return that.
	 */
	oldcxt = MemoryContextSwitchTo(deferredTriggers->deftrig_cxt);

	trigstate = (DeferredTriggerStatus)
		palloc(sizeof(DeferredTriggerStatusData));
	trigstate->dts_tgoid = tgoid;
	trigstate->dts_tgisdeferred =
		((itemstate & TRIGGER_DEFERRED_INITDEFERRED) != 0);
	deferredTriggers->deftrig_trigstates =
		lappend(deferredTriggers->deftrig_trigstates, trigstate);

	MemoryContextSwitchTo(oldcxt);

	return trigstate->dts_tgisdeferred;
}


/* ----------
 * deferredTriggerAddEvent()
 *
 *	Add a new trigger event to the queue.
 * ----------
 */
static void
deferredTriggerAddEvent(DeferredTriggerEvent event)
{
	/*
	 * Since the event list could grow quite long, we keep track of the
	 * list tail and append there, rather than just doing a stupid
	 * "lappend". This avoids O(N^2) behavior for large numbers of events.
	 */
	event->dte_next = NULL;
	if (deferredTriggers->deftrig_event_tail == NULL)
	{
		/* first list entry */
		deferredTriggers->deftrig_events = event;
		deferredTriggers->deftrig_event_tail = event;
	}
	else
	{
		deferredTriggers->deftrig_event_tail->dte_next = event;
		deferredTriggers->deftrig_event_tail = event;
	}
}


/* ----------
 * DeferredTriggerExecute()
 *
 *	Fetch the required tuples back from the heap and fire one
 *	single trigger function.
 *
 *	Frequently, this will be fired many times in a row for triggers of
 *	a single relation.	Therefore, we cache the open relation and provide
 *	fmgr lookup cache space at the caller level.
 *
 *	event: event currently being fired.
 *	itemno: item within event currently being fired.
 *	rel: open relation for event.
 *	trigdesc: working copy of rel's trigger info.
 *	finfo: array of fmgr lookup cache entries (one per trigger in trigdesc).
 *	per_tuple_context: memory context to call trigger function in.
 * ----------
 */
static void
DeferredTriggerExecute(DeferredTriggerEvent event, int itemno,
					Relation rel, TriggerDesc *trigdesc, FmgrInfo *finfo,
					   MemoryContext per_tuple_context)
{
	Oid			tgoid = event->dte_item[itemno].dti_tgoid;
	TriggerData LocTriggerData;
	HeapTupleData oldtuple;
	HeapTupleData newtuple;
	HeapTuple	rettuple;
	Buffer		oldbuffer;
	Buffer		newbuffer;
	int			tgindx;

	/*
	 * Fetch the required OLD and NEW tuples.
	 */
	if (ItemPointerIsValid(&(event->dte_oldctid)))
	{
		ItemPointerCopy(&(event->dte_oldctid), &(oldtuple.t_self));
		if (!heap_fetch(rel, SnapshotAny, &oldtuple, &oldbuffer, false, NULL))
			elog(ERROR, "failed to fetch old tuple for deferred trigger");
	}

	if (ItemPointerIsValid(&(event->dte_newctid)))
	{
		ItemPointerCopy(&(event->dte_newctid), &(newtuple.t_self));
		if (!heap_fetch(rel, SnapshotAny, &newtuple, &newbuffer, false, NULL))
			elog(ERROR, "failed to fetch new tuple for deferred trigger");
	}

	/*
	 * Setup the trigger information
	 */
	LocTriggerData.type = T_TriggerData;
	LocTriggerData.tg_event = (event->dte_event & TRIGGER_EVENT_OPMASK) |
		(event->dte_event & TRIGGER_EVENT_ROW);
	LocTriggerData.tg_relation = rel;

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

	switch (event->dte_event & TRIGGER_EVENT_OPMASK)
	{
		case TRIGGER_EVENT_INSERT:
			LocTriggerData.tg_trigtuple = &newtuple;
			LocTriggerData.tg_newtuple = NULL;
			break;

		case TRIGGER_EVENT_UPDATE:
			LocTriggerData.tg_trigtuple = &oldtuple;
			LocTriggerData.tg_newtuple = &newtuple;
			break;

		case TRIGGER_EVENT_DELETE:
			LocTriggerData.tg_trigtuple = &oldtuple;
			LocTriggerData.tg_newtuple = NULL;
			break;
	}

	/*
	 * Call the trigger and throw away any eventually returned updated
	 * tuple.
	 */
	rettuple = ExecCallTriggerFunc(&LocTriggerData,
								   finfo + tgindx,
								   per_tuple_context);
	if (rettuple != NULL && rettuple != &oldtuple && rettuple != &newtuple)
		heap_freetuple(rettuple);

	/*
	 * Release buffers
	 */
	if (ItemPointerIsValid(&(event->dte_oldctid)))
		ReleaseBuffer(oldbuffer);
	if (ItemPointerIsValid(&(event->dte_newctid)))
		ReleaseBuffer(newbuffer);
}


/* ----------
 * deferredTriggerInvokeEvents()
 *
 *	Scan the event queue for not yet invoked triggers. Check if they
 *	should be invoked now and do so.
 * ----------
 */
static void
deferredTriggerInvokeEvents(bool immediate_only)
{
	DeferredTriggerEvent event,
				prev_event;
	MemoryContext per_tuple_context;
	Relation	rel = NULL;
	TriggerDesc *trigdesc = NULL;
	FmgrInfo   *finfo = NULL;

	/*
	 * If immediate_only is true, we remove fully-processed events from
	 * the event queue to recycle space.  If immediate_only is false, we
	 * are going to discard the whole event queue on return anyway, so no
	 * need to bother with "retail" pfree's.
	 *
	 * If immediate_only is true, we need only scan from where the end of the
	 * queue was at the previous deferredTriggerInvokeEvents call; any
	 * non-deferred events before that point are already fired. (But if
	 * the deferral state changes, we must reset the saved position to the
	 * beginning of the queue, so as to process all events once with the
	 * new states.	See DeferredTriggerSetState.)
	 */

	/* Make a per-tuple memory context for trigger function calls */
	per_tuple_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "DeferredTriggerTupleContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * If immediate_only is true, then the only events that could need
	 * firing are those since deftrig_events_imm.  (But if
	 * deftrig_events_imm is NULL, we must scan the entire list.)
	 */
	if (immediate_only && deferredTriggers->deftrig_events_imm != NULL)
	{
		prev_event = deferredTriggers->deftrig_events_imm;
		event = prev_event->dte_next;
	}
	else
	{
		prev_event = NULL;
		event = deferredTriggers->deftrig_events;
	}

	while (event != NULL)
	{
		bool		still_deferred_ones = false;
		DeferredTriggerEvent next_event;
		int			i;

		/*
		 * Check if event is already completely done.
		 */
		if (!(event->dte_event & (TRIGGER_DEFERRED_DONE |
								  TRIGGER_DEFERRED_CANCELED)))
		{
			MemoryContextReset(per_tuple_context);

			/*
			 * Check each trigger item in the event.
			 */
			for (i = 0; i < event->dte_n_items; i++)
			{
				if (event->dte_item[i].dti_state & TRIGGER_DEFERRED_DONE)
					continue;

				/*
				 * This trigger item hasn't been called yet. Check if we
				 * should call it now.
				 */
				if (immediate_only &&
				  deferredTriggerCheckState(event->dte_item[i].dti_tgoid,
											event->dte_item[i].dti_state))
				{
					still_deferred_ones = true;
					continue;
				}

				/*
				 * So let's fire it... but first, open the correct
				 * relation if this is not the same relation as before.
				 */
				if (rel == NULL || rel->rd_id != event->dte_relid)
				{
					if (rel)
						heap_close(rel, NoLock);
					FreeTriggerDesc(trigdesc);
					if (finfo)
						pfree(finfo);

					/*
					 * We assume that an appropriate lock is still held by
					 * the executor, so grab no new lock here.
					 */
					rel = heap_open(event->dte_relid, NoLock);

					/*
					 * Copy relation's trigger info so that we have a
					 * stable copy no matter what the called triggers do.
					 */
					trigdesc = CopyTriggerDesc(rel->trigdesc);

					if (trigdesc == NULL)		/* should not happen */
						elog(ERROR, "relation %u has no triggers",
							 event->dte_relid);

					/*
					 * Allocate space to cache fmgr lookup info for
					 * triggers.
					 */
					finfo = (FmgrInfo *)
						palloc0(trigdesc->numtriggers * sizeof(FmgrInfo));
				}

				DeferredTriggerExecute(event, i, rel, trigdesc, finfo,
									   per_tuple_context);

				event->dte_item[i].dti_state |= TRIGGER_DEFERRED_DONE;
			}					/* end loop over items within event */
		}

		/*
		 * If it's now completely done, throw it away.
		 *
		 * NB: it's possible the trigger calls above added more events to the
		 * queue, or that calls we will do later will want to add more, so
		 * we have to be careful about maintaining list validity here.
		 */
		next_event = event->dte_next;

		if (still_deferred_ones)
		{
			/* Not done, keep in list */
			prev_event = event;
		}
		else
		{
			/* Done */
			if (immediate_only)
			{
				/* delink it from list and free it */
				if (prev_event)
					prev_event->dte_next = next_event;
				else
					deferredTriggers->deftrig_events = next_event;
				pfree(event);
			}
			else
			{
				/*
				 * We will clean up later, but just for paranoia's sake,
				 * mark the event done.
				 */
				event->dte_event |= TRIGGER_DEFERRED_DONE;
			}
		}

		event = next_event;
	}

	/* Update list tail pointer in case we just deleted tail event */
	deferredTriggers->deftrig_event_tail = prev_event;

	/* Set the immediate event pointer for next time */
	deferredTriggers->deftrig_events_imm = prev_event;

	/* Release working resources */
	if (rel)
		heap_close(rel, NoLock);
	FreeTriggerDesc(trigdesc);
	if (finfo)
		pfree(finfo);
	MemoryContextDelete(per_tuple_context);
}


/* ----------
 * DeferredTriggerInit()
 *
 *	Initialize the deferred trigger mechanism. This is called during
 *	backend startup and is guaranteed to be before the first of all
 *	transactions.
 * ----------
 */
void
DeferredTriggerInit(void)
{
	/* Nothing to do */
	;
}


/* ----------
 * DeferredTriggerBeginXact()
 *
 *	Called at transaction start (either BEGIN or implicit for single
 *	statement outside of transaction block).
 * ----------
 */
void
DeferredTriggerBeginXact(void)
{
	/*
	 * This will be changed to a special context when the nested
	 * transactions project moves forward.
	 */
	MemoryContext cxt = TopTransactionContext;

	deferredTriggers = (DeferredTriggers) MemoryContextAlloc(TopTransactionContext,
										   sizeof(DeferredTriggersData));

	/*
	 * Create the per transaction memory context
	 */
	deferredTriggers->deftrig_cxt = AllocSetContextCreate(cxt,
												   "DeferredTriggerXact",
												ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * If unspecified, constraints default to IMMEDIATE, per SQL
	 */
	deferredTriggers->deftrig_all_isdeferred = false;
	deferredTriggers->deftrig_all_isset = false;

	deferredTriggers->deftrig_trigstates = NIL;
	deferredTriggers->deftrig_events = NULL;
	deferredTriggers->deftrig_events_imm = NULL;
	deferredTriggers->deftrig_event_tail = NULL;
}


/* ----------
 * DeferredTriggerEndQuery()
 *
 *	Called after one query sent down by the user has completely been
 *	processed. At this time we invoke all outstanding IMMEDIATE triggers.
 * ----------
 */
void
DeferredTriggerEndQuery(void)
{
	/*
	 * Ignore call if we aren't in a transaction.
	 */
	if (deferredTriggers == NULL)
		return;

	deferredTriggerInvokeEvents(true);
}


/* ----------
 * DeferredTriggerEndXact()
 *
 *	Called just before the current transaction is committed. At this
 *	time we invoke all DEFERRED triggers and tidy up.
 * ----------
 */
void
DeferredTriggerEndXact(void)
{
	/*
	 * Ignore call if we aren't in a transaction.
	 */
	if (deferredTriggers == NULL)
		return;

	deferredTriggerInvokeEvents(false);

	deferredTriggers = NULL;
}


/* ----------
 * DeferredTriggerAbortXact()
 *
 *	The current transaction has entered the abort state.
 *	All outstanding triggers are canceled so we simply throw
 *	away anything we know.
 * ----------
 */
void
DeferredTriggerAbortXact(void)
{
	/*
	 * Ignore call if we aren't in a transaction.
	 */
	if (deferredTriggers == NULL)
		return;

	/*
	 * Forget everything we know about deferred triggers.
	 */
	deferredTriggers = NULL;
}


/* ----------
 * DeferredTriggerSetState()
 *
 *	Called for the SET CONSTRAINTS ... utility command.
 * ----------
 */
void
DeferredTriggerSetState(ConstraintsSetStmt *stmt)
{
	List	   *l;

	/*
	 * Ignore call if we aren't in a transaction.
	 */
	if (deferredTriggers == NULL)
		return;

	/*
	 * Handle SET CONSTRAINTS ALL ...
	 */
	if (stmt->constraints == NIL)
	{
		/*
		 * Drop all per-transaction information about individual trigger
		 * states.
		 */
		l = deferredTriggers->deftrig_trigstates;
		while (l != NIL)
		{
			List	   *next = lnext(l);

			pfree(lfirst(l));
			pfree(l);
			l = next;
		}
		deferredTriggers->deftrig_trigstates = NIL;

		/*
		 * Set the per-transaction ALL state to known.
		 */
		deferredTriggers->deftrig_all_isset = true;
		deferredTriggers->deftrig_all_isdeferred = stmt->deferred;
	}
	else
	{
		Relation	tgrel;
		MemoryContext oldcxt;
		bool		found;
		DeferredTriggerStatus state;
		List	   *ls;
		List	   *loid = NIL;

		/* ----------
		 * Handle SET CONSTRAINTS constraint-name [, ...]
		 * First lookup all trigger Oid's for the constraint names.
		 * ----------
		 */
		tgrel = heap_openr(TriggerRelationName, AccessShareLock);

		foreach(l, stmt->constraints)
		{
			char	   *cname = strVal(lfirst(l));
			ScanKeyData skey;
			SysScanDesc tgscan;
			HeapTuple	htup;

			/*
			 * Check that only named constraints are set explicitly
			 */
			if (strlen(cname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_NAME),
				errmsg("unnamed constraints cannot be set explicitly")));

			/*
			 * Setup to scan pg_trigger by tgconstrname ...
			 */
			ScanKeyEntryInitialize(&skey, (bits16) 0x0,
							   (AttrNumber) Anum_pg_trigger_tgconstrname,
								   (RegProcedure) F_NAMEEQ,
								   PointerGetDatum(cname));

			tgscan = systable_beginscan(tgrel, TriggerConstrNameIndex, true,
										SnapshotNow, 1, &skey);

			/*
			 * ... and search for the constraint trigger row
			 */
			found = false;

			while (HeapTupleIsValid(htup = systable_getnext(tgscan)))
			{
				Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(htup);
				Oid			constr_oid;

				/*
				 * If we found some, check that they fit the deferrability
				 * but skip ON <event> RESTRICT ones, since they are
				 * silently never deferrable.
				 */
				if (stmt->deferred && !pg_trigger->tgdeferrable &&
					pg_trigger->tgfoid != F_RI_FKEY_RESTRICT_UPD &&
					pg_trigger->tgfoid != F_RI_FKEY_RESTRICT_DEL)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("constraint \"%s\" is not deferrable",
									cname)));

				constr_oid = HeapTupleGetOid(htup);
				loid = lappendo(loid, constr_oid);
				found = true;
			}

			systable_endscan(tgscan);

			/*
			 * Not found ?
			 */
			if (!found)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("constraint \"%s\" does not exist", cname)));
		}
		heap_close(tgrel, AccessShareLock);

		/*
		 * Inside of a transaction block set the trigger states of
		 * individual triggers on transaction level.
		 */
		oldcxt = MemoryContextSwitchTo(deferredTriggers->deftrig_cxt);

		foreach(l, loid)
		{
			found = false;
			foreach(ls, deferredTriggers->deftrig_trigstates)
			{
				state = (DeferredTriggerStatus) lfirst(ls);
				if (state->dts_tgoid == lfirsto(l))
				{
					state->dts_tgisdeferred = stmt->deferred;
					found = true;
					break;
				}
			}
			if (!found)
			{
				state = (DeferredTriggerStatus)
					palloc(sizeof(DeferredTriggerStatusData));
				state->dts_tgoid = lfirsto(l);
				state->dts_tgisdeferred = stmt->deferred;

				deferredTriggers->deftrig_trigstates =
					lappend(deferredTriggers->deftrig_trigstates, state);
			}
		}

		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * SQL99 requires that when a constraint is set to IMMEDIATE, any
	 * deferred checks against that constraint must be made when the SET
	 * CONSTRAINTS command is executed -- i.e. the effects of the SET
	 * CONSTRAINTS command applies retroactively. This happens "for free"
	 * since we have already made the necessary modifications to the
	 * constraints, and deferredTriggerEndQuery() is called by
	 * finish_xact_command().  But we must reset
	 * deferredTriggerInvokeEvents' tail pointer to make it rescan the
	 * entire list, in case some deferred events are now immediately
	 * invokable.
	 */
	deferredTriggers->deftrig_events_imm = NULL;
}


/* ----------
 * DeferredTriggerSaveEvent()
 *
 *	Called by ExecAR...Triggers() to add the event to the queue.
 *
 *	NOTE: should be called only if we've determined that an event must
 *	be added to the queue.
 * ----------
 */
static void
DeferredTriggerSaveEvent(ResultRelInfo *relinfo, int event, bool row_trigger,
						 HeapTuple oldtup, HeapTuple newtup)
{
	Relation	rel = relinfo->ri_RelationDesc;
	TriggerDesc *trigdesc = relinfo->ri_TrigDesc;
	MemoryContext oldcxt;
	DeferredTriggerEvent new_event;
	int			new_size;
	int			i;
	int			ntriggers;
	int			n_enabled_triggers = 0;
	int		   *tgindx;
	ItemPointerData oldctid;
	ItemPointerData newctid;

	if (deferredTriggers == NULL)
		elog(ERROR, "DeferredTriggerSaveEvent() called outside of transaction");

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

	/*
	 * Count the number of triggers that are actually enabled. Since we
	 * only add enabled triggers to the queue, we only need allocate
	 * enough space to hold them (and not any disabled triggers that may
	 * be associated with the relation).
	 */
	for (i = 0; i < ntriggers; i++)
	{
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];

		if (trigger->tgenabled)
			n_enabled_triggers++;
	}

	/*
	 * If all the triggers on this relation are disabled, we're done.
	 */
	if (n_enabled_triggers == 0)
		return;

	/*
	 * Create a new event
	 */
	oldcxt = MemoryContextSwitchTo(deferredTriggers->deftrig_cxt);

	new_size = offsetof(DeferredTriggerEventData, dte_item[0]) +
		n_enabled_triggers * sizeof(DeferredTriggerEventItem);

	new_event = (DeferredTriggerEvent) palloc(new_size);
	new_event->dte_next = NULL;
	new_event->dte_event = event & TRIGGER_EVENT_OPMASK;
	if (row_trigger)
		new_event->dte_event |= TRIGGER_EVENT_ROW;
	new_event->dte_relid = rel->rd_id;
	ItemPointerCopy(&oldctid, &(new_event->dte_oldctid));
	ItemPointerCopy(&newctid, &(new_event->dte_newctid));
	new_event->dte_n_items = ntriggers;
	for (i = 0; i < ntriggers; i++)
	{
		DeferredTriggerEventItem *ev_item;
		Trigger    *trigger = &trigdesc->triggers[tgindx[i]];

		if (!trigger->tgenabled)
			continue;

		ev_item = &(new_event->dte_item[i]);
		ev_item->dti_tgoid = trigger->tgoid;
		ev_item->dti_state =
			((trigger->tgdeferrable) ?
			 TRIGGER_DEFERRED_DEFERRABLE : 0) |
			((trigger->tginitdeferred) ?
			 TRIGGER_DEFERRED_INITDEFERRED : 0);

		if (row_trigger && (trigdesc->n_before_row[event] > 0))
			ev_item->dti_state |= TRIGGER_DEFERRED_HAS_BEFORE;
		else if (!row_trigger && (trigdesc->n_before_statement[event] > 0))
			ev_item->dti_state |= TRIGGER_DEFERRED_HAS_BEFORE;
	}

	MemoryContextSwitchTo(oldcxt);

	switch (event & TRIGGER_EVENT_OPMASK)
	{
		case TRIGGER_EVENT_INSERT:
			/* nothing to do */
			break;

		case TRIGGER_EVENT_UPDATE:

			/*
			 * Check if one of the referenced keys is changed.
			 */
			for (i = 0; i < ntriggers; i++)
			{
				Trigger    *trigger = &trigdesc->triggers[tgindx[i]];
				bool		is_ri_trigger;
				bool		key_unchanged;
				TriggerData LocTriggerData;

				/*
				 * We are interested in RI_FKEY triggers only.
				 */
				switch (trigger->tgfoid)
				{
					case F_RI_FKEY_NOACTION_UPD:
					case F_RI_FKEY_CASCADE_UPD:
					case F_RI_FKEY_RESTRICT_UPD:
					case F_RI_FKEY_SETNULL_UPD:
					case F_RI_FKEY_SETDEFAULT_UPD:
						is_ri_trigger = true;
						break;

					default:
						is_ri_trigger = false;
						break;
				}
				if (!is_ri_trigger)
					continue;

				LocTriggerData.type = T_TriggerData;
				LocTriggerData.tg_event = TRIGGER_EVENT_UPDATE;
				LocTriggerData.tg_relation = rel;
				LocTriggerData.tg_trigtuple = oldtup;
				LocTriggerData.tg_newtuple = newtup;
				LocTriggerData.tg_trigger = trigger;

				key_unchanged = RI_FKey_keyequal_upd(&LocTriggerData);

				if (key_unchanged)
				{
					/*
					 * The key hasn't changed, so no need later to invoke
					 * the trigger at all.
					 */
					new_event->dte_item[i].dti_state |= TRIGGER_DEFERRED_DONE;
				}
			}

			break;

		case TRIGGER_EVENT_DELETE:
			/* nothing to do */
			break;
	}

	/*
	 * Add the new event to the queue.
	 */
	deferredTriggerAddEvent(new_event);
}
