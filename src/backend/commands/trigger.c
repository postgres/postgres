/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "commands/comment.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/syscache.h"
#include "utils/tqual.h"

DLLIMPORT TriggerData *CurrentTriggerData = NULL;

void		RelationBuildTriggers(Relation relation);
void		FreeTriggerDesc(Relation relation);

static void DescribeTrigger(TriggerDesc *trigdesc, Trigger *trigger);
static HeapTuple GetTupleForTrigger(EState *estate, ItemPointer tid,
				   TupleTableSlot **newSlot);

extern GlobalMemory CacheCxt;

void
CreateTrigger(CreateTrigStmt *stmt)
{
	int16		tgtype;
	int16		tgattr[8] = {0};
	Datum		values[Natts_pg_trigger];
	char		nulls[Natts_pg_trigger];
	Relation	rel;
	Relation	tgrel;
	HeapScanDesc tgscan;
	ScanKeyData key;
	Relation	pgrel;
	HeapTuple	tuple;
	Relation	idescs[Num_pg_trigger_indices];
	Relation	ridescs[Num_pg_class_indices];
	MemoryContext oldcxt;
	Oid			fargtypes[8];
	int			found = 0;
	int			i;
	char		constrtrigname[NAMEDATALEN];
	char	   *constrname = "";
	Oid			constrrelid = 0;

	if (!allowSystemTableMods && IsSystemRelationName(stmt->relname))
		elog(ERROR, "CreateTrigger: can't create trigger for system relation %s", stmt->relname);

#ifndef NO_SECURITY
	if (!pg_ownercheck(GetPgUserName(), stmt->relname, RELNAME))
		elog(ERROR, "%s: %s", stmt->relname, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
#endif

	/* ----------
	 * If trigger is a constraint, user trigger name as constraint
	 * name and build a unique trigger name instead.
	 * ----------
	 */
	if (stmt->isconstraint)
	{
		constrname = stmt->trigname;
		stmt->trigname = constrtrigname;
		sprintf(constrtrigname, "RI_ConstraintTrigger_%d", newoid());

		if (strcmp(stmt->constrrelname, "") == 0)
			constrrelid = 0;
		else
		{
			rel = heap_openr(stmt->constrrelname, NoLock);
			if (rel == NULL)
				elog(ERROR, "table \"%s\" does not exist",
							stmt->constrrelname);
			constrrelid = rel->rd_id;
			heap_close(rel, NoLock);
		}
	}

	rel = heap_openr(stmt->relname, AccessExclusiveLock);

	TRIGGER_CLEAR_TYPE(tgtype);
	if (stmt->before)
		TRIGGER_SETT_BEFORE(tgtype);
	if (stmt->row)
		TRIGGER_SETT_ROW(tgtype);
	else
		elog(ERROR, "CreateTrigger: STATEMENT triggers are unimplemented, yet");

	for (i = 0; i < 3 && stmt->actions[i]; i++)
	{
		switch (stmt->actions[i])
		{
			case 'i':
				if (TRIGGER_FOR_INSERT(tgtype))
					elog(ERROR, "CreateTrigger: double INSERT event specified");
				TRIGGER_SETT_INSERT(tgtype);
				break;
			case 'd':
				if (TRIGGER_FOR_DELETE(tgtype))
					elog(ERROR, "CreateTrigger: double DELETE event specified");
				TRIGGER_SETT_DELETE(tgtype);
				break;
			case 'u':
				if (TRIGGER_FOR_UPDATE(tgtype))
					elog(ERROR, "CreateTrigger: double UPDATE event specified");
				TRIGGER_SETT_UPDATE(tgtype);
				break;
			default:
				elog(ERROR, "CreateTrigger: unknown event specified");
				break;
		}
	}

	/* Scan pg_trigger */
	tgrel = heap_openr(TriggerRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgrelid,
						   F_OIDEQ, RelationGetRelid(rel));
	tgscan = heap_beginscan(tgrel, 0, SnapshotNow, 1, &key);
	while (HeapTupleIsValid(tuple = heap_getnext(tgscan, 0)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);

		if (namestrcmp(&(pg_trigger->tgname), stmt->trigname) == 0)
			elog(ERROR, "CreateTrigger: trigger %s already defined on relation %s",
				 stmt->trigname, stmt->relname);
		else
			found++;
	}
	heap_endscan(tgscan);

	MemSet(fargtypes, 0, 8 * sizeof(Oid));
	tuple = SearchSysCacheTuple(PROCNAME,
								PointerGetDatum(stmt->funcname),
								Int32GetDatum(0),
								PointerGetDatum(fargtypes),
								0);
	if (!HeapTupleIsValid(tuple) ||
		((Form_pg_proc) GETSTRUCT(tuple))->pronargs != 0)
		elog(ERROR, "CreateTrigger: function %s() does not exist",
			 stmt->funcname);
	if (((Form_pg_proc) GETSTRUCT(tuple))->prorettype != 0)
		elog(ERROR, "CreateTrigger: function %s() must return OPAQUE",
			 stmt->funcname);
	if (((Form_pg_proc) GETSTRUCT(tuple))->prolang != ClanguageId &&
		((Form_pg_proc) GETSTRUCT(tuple))->prolang != INTERNALlanguageId)
	{
		HeapTuple	langTup;

		langTup = SearchSysCacheTuple(LANGOID,
			ObjectIdGetDatum(((Form_pg_proc) GETSTRUCT(tuple))->prolang),
									  0, 0, 0);
		if (!HeapTupleIsValid(langTup))
			elog(ERROR, "CreateTrigger: cache lookup for PL failed");

		if (((Form_pg_language) GETSTRUCT(langTup))->lanispl == false)
			elog(ERROR, "CreateTrigger: only builtin, C and PL functions are supported");
	}

	MemSet(nulls, ' ', Natts_pg_trigger * sizeof(char));

	values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_trigger_tgname - 1] = NameGetDatum(namein(stmt->trigname));
	values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum(tuple->t_data->t_oid);
	values[Anum_pg_trigger_tgtype - 1] = Int16GetDatum(tgtype);

	values[Anum_pg_trigger_tgenabled - 1]		= true;
	values[Anum_pg_trigger_tgisconstraint - 1]	= stmt->isconstraint;
	values[Anum_pg_trigger_tgconstrname - 1]	= PointerGetDatum(constrname);;
	values[Anum_pg_trigger_tgconstrrelid - 1]	= constrrelid;
	values[Anum_pg_trigger_tgdeferrable - 1]	= stmt->deferrable;
	values[Anum_pg_trigger_tginitdeferred - 1]	= stmt->initdeferred;

	if (stmt->args)
	{
		List	   *le;
		char	   *args;
		int16		nargs = length(stmt->args);
		int			len = 0;

		foreach(le, stmt->args)
		{
			char	   *ar = (char *) lfirst(le);

			len += strlen(ar) + VARHDRSZ;
			for (; *ar; ar++)
			{
				if (*ar == '\\')
					len++;
			}
		}
		args = (char *) palloc(len + 1);
		args[0] = 0;
		foreach(le, stmt->args)
		{
			char	   *s = (char *) lfirst(le);
			char	   *d = args + strlen(args);

			while (*s)
			{
				if (*s == '\\')
					*d++ = '\\';
				*d++ = *s++;
			}
			*d = 0;
			strcat(args, "\\000");
		}
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(nargs);
		values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum(byteain(args));
	}
	else
	{
		values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum(0);
		values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum(byteain(""));
	}
	values[Anum_pg_trigger_tgattr - 1] = PointerGetDatum(tgattr);

	tuple = heap_formtuple(tgrel->rd_att, values, nulls);
	heap_insert(tgrel, tuple);
	CatalogOpenIndices(Num_pg_trigger_indices, Name_pg_trigger_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_trigger_indices, tgrel, tuple);
	CatalogCloseIndices(Num_pg_trigger_indices, idescs);
	pfree(tuple);
	heap_close(tgrel, RowExclusiveLock);

	pfree(DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));

	/* update pg_class */
	pgrel = heap_openr(RelationRelationName, RowExclusiveLock);
	tuple = SearchSysCacheTupleCopy(RELNAME,
									PointerGetDatum(stmt->relname),
									0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "CreateTrigger: relation %s not found in pg_class", stmt->relname);

	((Form_pg_class) GETSTRUCT(tuple))->reltriggers = found + 1;
	RelationInvalidateHeapTuple(pgrel, tuple);
	heap_update(pgrel, &tuple->t_self, tuple, NULL);
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
	CatalogIndexInsert(ridescs, Num_pg_class_indices, pgrel, tuple);
	CatalogCloseIndices(Num_pg_class_indices, ridescs);
	pfree(tuple);
	heap_close(pgrel, RowExclusiveLock);

	CommandCounterIncrement();
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
	FreeTriggerDesc(rel);
	rel->rd_rel->reltriggers = found + 1;
	RelationBuildTriggers(rel);
	MemoryContextSwitchTo(oldcxt);
	/* Keep lock on target rel until end of xact */
	heap_close(rel, NoLock);
}

void
DropTrigger(DropTrigStmt *stmt)
{
	Relation	rel;
	Relation	tgrel;
	HeapScanDesc tgscan;
	ScanKeyData key;
	Relation	pgrel;
	HeapTuple	tuple;
	Relation	ridescs[Num_pg_class_indices];
	MemoryContext oldcxt;
	int			found = 0;
	int			tgfound = 0;

#ifndef NO_SECURITY
	if (!pg_ownercheck(GetPgUserName(), stmt->relname, RELNAME))
		elog(ERROR, "%s: %s", stmt->relname, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
#endif

	rel = heap_openr(stmt->relname, AccessExclusiveLock);

	tgrel = heap_openr(TriggerRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgrelid,
						   F_OIDEQ, RelationGetRelid(rel));
	tgscan = heap_beginscan(tgrel, 0, SnapshotNow, 1, &key);
	while (HeapTupleIsValid(tuple = heap_getnext(tgscan, 0)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);

		if (namestrcmp(&(pg_trigger->tgname), stmt->trigname) == 0)
		{

		  /*** Delete any comments associated with this trigger ***/

		  DeleteComments(tuple->t_data->t_oid);

		  heap_delete(tgrel, &tuple->t_self, NULL);
		  tgfound++;
		  
		}
		else
			found++;
	}
	if (tgfound == 0)
		elog(ERROR, "DropTrigger: there is no trigger %s on relation %s",
			 stmt->trigname, stmt->relname);
	if (tgfound > 1)
		elog(NOTICE, "DropTrigger: found (and deleted) %d triggers %s on relation %s",
			 tgfound, stmt->trigname, stmt->relname);
	heap_endscan(tgscan);
	heap_close(tgrel, RowExclusiveLock);

	/* update pg_class */
	pgrel = heap_openr(RelationRelationName, RowExclusiveLock);
	tuple = SearchSysCacheTupleCopy(RELNAME,
									PointerGetDatum(stmt->relname),
									0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "DropTrigger: relation %s not found in pg_class", stmt->relname);

	((Form_pg_class) GETSTRUCT(tuple))->reltriggers = found;
	RelationInvalidateHeapTuple(pgrel, tuple);
	heap_update(pgrel, &tuple->t_self, tuple, NULL);
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
	CatalogIndexInsert(ridescs, Num_pg_class_indices, pgrel, tuple);
	CatalogCloseIndices(Num_pg_class_indices, ridescs);
	pfree(tuple);
	heap_close(pgrel, RowExclusiveLock);

	CommandCounterIncrement();
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
	FreeTriggerDesc(rel);
	rel->rd_rel->reltriggers = found;
	if (found > 0)
		RelationBuildTriggers(rel);
	MemoryContextSwitchTo(oldcxt);
	/* Keep lock on target rel until end of xact */
	heap_close(rel, NoLock);
}

void
RelationRemoveTriggers(Relation rel)
{
	Relation	tgrel;
	HeapScanDesc tgscan;
	ScanKeyData key;
	HeapTuple	tup;
	Form_pg_trigger	pg_trigger;

	tgrel = heap_openr(TriggerRelationName, RowExclusiveLock);
	ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgrelid,
						   F_OIDEQ, RelationGetRelid(rel));

	tgscan = heap_beginscan(tgrel, 0, SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tup = heap_getnext(tgscan, 0))) {

	  /*** Delete any comments associated with this trigger ***/

	  DeleteComments(tup->t_data->t_oid);
	  
	  heap_delete(tgrel, &tup->t_self, NULL);

	}

	heap_endscan(tgscan);


	/* ----------
	 * Also drop all constraint triggers referencing this relation
	 * ----------
	 */
	ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgconstrrelid,
							F_OIDEQ, RelationGetRelid(rel));

	tgscan = heap_beginscan(tgrel, 0, SnapshotNow, 1, &key);
	while (HeapTupleIsValid(tup = heap_getnext(tgscan, 0)))
	{
		Relation		refrel;
		DropTrigStmt	stmt;

		pg_trigger = (Form_pg_trigger) GETSTRUCT(tup);
		refrel = heap_open(pg_trigger->tgrelid, NoLock);

		stmt.relname = pstrdup(RelationGetRelationName(refrel));
		stmt.trigname = nameout(&pg_trigger->tgname);

		elog(NOTICE, "DROP TABLE implicitly drops referential integrity trigger from table \"%s\"", stmt.relname);

		DropTrigger(&stmt);

		pfree(stmt.relname);
		pfree(stmt.trigname);

		heap_close(refrel, NoLock);
	}
	heap_endscan(tgscan);

	heap_close(tgrel, RowExclusiveLock);
}

void
RelationBuildTriggers(Relation relation)
{
	TriggerDesc *trigdesc = (TriggerDesc *) palloc(sizeof(TriggerDesc));
	int			ntrigs = relation->rd_rel->reltriggers;
	Trigger    *triggers = NULL;
	Trigger    *build;
	Relation	tgrel;
	Form_pg_trigger pg_trigger;
	Relation	irel;
	ScanKeyData skey;
	HeapTupleData tuple;
	IndexScanDesc sd;
	RetrieveIndexResult indexRes;
	Buffer		buffer;
	struct varlena *val;
	bool		isnull;
	int			found;

	MemSet(trigdesc, 0, sizeof(TriggerDesc));

	ScanKeyEntryInitialize(&skey,
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(RelationGetRelid(relation)));

	tgrel = heap_openr(TriggerRelationName, AccessShareLock);
	irel = index_openr(TriggerRelidIndex);
	sd = index_beginscan(irel, false, 1, &skey);

	for (found = 0;;)
	{
		indexRes = index_getnext(sd, ForwardScanDirection);
		if (!indexRes)
			break;

		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(tgrel, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);
		if (!tuple.t_data)
			continue;
		if (found == ntrigs)
			elog(ERROR, "RelationBuildTriggers: unexpected record found for rel %.*s",
				 NAMEDATALEN, RelationGetRelationName(relation));

		pg_trigger = (Form_pg_trigger) GETSTRUCT(&tuple);

		if (triggers == NULL)
			triggers = (Trigger *) palloc(sizeof(Trigger));
		else
			triggers = (Trigger *) repalloc(triggers, (found + 1) * sizeof(Trigger));
		build = &(triggers[found]);

		build->tgoid = tuple.t_data->t_oid;
		build->tgname = nameout(&pg_trigger->tgname);
		build->tgfoid = pg_trigger->tgfoid;
		build->tgfunc.fn_addr = NULL;
		build->tgtype = pg_trigger->tgtype;
		build->tgenabled = pg_trigger->tgenabled;
		build->tgisconstraint = pg_trigger->tgisconstraint;
		build->tgdeferrable = pg_trigger->tgdeferrable;
		build->tginitdeferred = pg_trigger->tginitdeferred;
		build->tgnargs = pg_trigger->tgnargs;
		memcpy(build->tgattr, &(pg_trigger->tgattr), 8 * sizeof(int16));
		val = (struct varlena *) fastgetattr(&tuple,
											 Anum_pg_trigger_tgargs,
											 tgrel->rd_att, &isnull);
		if (isnull)
			elog(ERROR, "RelationBuildTriggers: tgargs IS NULL for rel %.*s",
				 NAMEDATALEN, RelationGetRelationName(relation));
		if (build->tgnargs > 0)
		{
			char	   *p;
			int			i;

			val = (struct varlena *) fastgetattr(&tuple,
												 Anum_pg_trigger_tgargs,
												 tgrel->rd_att, &isnull);
			if (isnull)
				elog(ERROR, "RelationBuildTriggers: tgargs IS NULL for rel %.*s",
					 NAMEDATALEN, RelationGetRelationName(relation));
			p = (char *) VARDATA(val);
			build->tgargs = (char **) palloc(build->tgnargs * sizeof(char *));
			for (i = 0; i < build->tgnargs; i++)
			{
				build->tgargs[i] = (char *) palloc(strlen(p) + 1);
				strcpy(build->tgargs[i], p);
				p += strlen(p) + 1;
			}
		}
		else
			build->tgargs = NULL;

		found++;
		ReleaseBuffer(buffer);
	}

	if (found < ntrigs)
		elog(ERROR, "RelationBuildTriggers: %d record not found for rel %.*s",
			 ntrigs - found,
			 NAMEDATALEN, RelationGetRelationName(relation));

	index_endscan(sd);
	pfree(sd);
	index_close(irel);
	heap_close(tgrel, AccessShareLock);

	/* Build trigdesc */
	trigdesc->triggers = triggers;
	for (found = 0; found < ntrigs; found++)
	{
		build = &(triggers[found]);
		DescribeTrigger(trigdesc, build);
	}

	relation->trigdesc = trigdesc;

}

void
FreeTriggerDesc(Relation relation)
{
	TriggerDesc *trigdesc = relation->trigdesc;
	Trigger  ***t;
	Trigger    *trigger;
	int			i;

	if (trigdesc == NULL)
		return;

	t = trigdesc->tg_before_statement;
	for (i = 0; i < 3; i++)
		if (t[i] != NULL)
			pfree(t[i]);
	t = trigdesc->tg_before_row;
	for (i = 0; i < 3; i++)
		if (t[i] != NULL)
			pfree(t[i]);
	t = trigdesc->tg_after_row;
	for (i = 0; i < 3; i++)
		if (t[i] != NULL)
			pfree(t[i]);
	t = trigdesc->tg_after_statement;
	for (i = 0; i < 3; i++)
		if (t[i] != NULL)
			pfree(t[i]);

	trigger = trigdesc->triggers;
	for (i = 0; i < relation->rd_rel->reltriggers; i++)
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
	relation->trigdesc = NULL;
	return;
}

static void
DescribeTrigger(TriggerDesc *trigdesc, Trigger *trigger)
{
	uint16	   *n;
	Trigger  ***t,
			 ***tp;

	if (TRIGGER_FOR_ROW(trigger->tgtype))		/* Is ROW/STATEMENT
												 * trigger */
	{
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
/* STATEMENT (NI) */
	{
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
			*tp = (Trigger **) palloc(sizeof(Trigger *));
		else
			*tp = (Trigger **) repalloc(*tp, (n[TRIGGER_EVENT_INSERT] + 1) *
										sizeof(Trigger *));
		(*tp)[n[TRIGGER_EVENT_INSERT]] = trigger;
		(n[TRIGGER_EVENT_INSERT])++;
	}

	if (TRIGGER_FOR_DELETE(trigger->tgtype))
	{
		tp = &(t[TRIGGER_EVENT_DELETE]);
		if (*tp == NULL)
			*tp = (Trigger **) palloc(sizeof(Trigger *));
		else
			*tp = (Trigger **) repalloc(*tp, (n[TRIGGER_EVENT_DELETE] + 1) *
										sizeof(Trigger *));
		(*tp)[n[TRIGGER_EVENT_DELETE]] = trigger;
		(n[TRIGGER_EVENT_DELETE])++;
	}

	if (TRIGGER_FOR_UPDATE(trigger->tgtype))
	{
		tp = &(t[TRIGGER_EVENT_UPDATE]);
		if (*tp == NULL)
			*tp = (Trigger **) palloc(sizeof(Trigger *));
		else
			*tp = (Trigger **) repalloc(*tp, (n[TRIGGER_EVENT_UPDATE] + 1) *
										sizeof(Trigger *));
		(*tp)[n[TRIGGER_EVENT_UPDATE]] = trigger;
		(n[TRIGGER_EVENT_UPDATE])++;
	}

}

static HeapTuple
ExecCallTriggerFunc(Trigger *trigger)
{

	if (trigger->tgfunc.fn_addr == NULL)
		fmgr_info(trigger->tgfoid, &trigger->tgfunc);

	if (trigger->tgfunc.fn_plhandler != NULL)
	{
		return (HeapTuple) (*(trigger->tgfunc.fn_plhandler))
			(&trigger->tgfunc);
	}

	return (HeapTuple) ((*fmgr_faddr(&trigger->tgfunc)) ());
}

HeapTuple
ExecBRInsertTriggers(Relation rel, HeapTuple trigtuple)
{
	TriggerData *SaveTriggerData;
	int			ntrigs = rel->trigdesc->n_before_row[TRIGGER_EVENT_INSERT];
	Trigger   **trigger = rel->trigdesc->tg_before_row[TRIGGER_EVENT_INSERT];
	HeapTuple	newtuple = trigtuple;
	HeapTuple	oldtuple;
	int			i;

	SaveTriggerData = (TriggerData *) palloc(sizeof(TriggerData));
	SaveTriggerData->tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW | TRIGGER_EVENT_BEFORE;
	SaveTriggerData->tg_relation = rel;
	SaveTriggerData->tg_newtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		if (!trigger[i]->tgenabled)
			continue;
		CurrentTriggerData = SaveTriggerData;
		CurrentTriggerData->tg_trigtuple = oldtuple = newtuple;
		CurrentTriggerData->tg_trigger = trigger[i];
		newtuple = ExecCallTriggerFunc(trigger[i]);
		if (newtuple == NULL)
			break;
		else if (oldtuple != newtuple && oldtuple != trigtuple)
			pfree(oldtuple);
	}
	CurrentTriggerData = NULL;
	pfree(SaveTriggerData);
	return newtuple;
}

void
ExecARInsertTriggers(Relation rel, HeapTuple trigtuple)
{
	DeferredTriggerSaveEvent(rel, TRIGGER_EVENT_INSERT, NULL, trigtuple);
	return;
}

bool
ExecBRDeleteTriggers(EState *estate, ItemPointer tupleid)
{
	Relation	rel = estate->es_result_relation_info->ri_RelationDesc;
	TriggerData *SaveTriggerData;
	int			ntrigs = rel->trigdesc->n_before_row[TRIGGER_EVENT_DELETE];
	Trigger   **trigger = rel->trigdesc->tg_before_row[TRIGGER_EVENT_DELETE];
	HeapTuple	trigtuple;
	HeapTuple	newtuple = NULL;
	TupleTableSlot *newSlot;
	int			i;

	trigtuple = GetTupleForTrigger(estate, tupleid, &newSlot);
	if (trigtuple == NULL)
		return false;

	SaveTriggerData = (TriggerData *) palloc(sizeof(TriggerData));
	SaveTriggerData->tg_event = TRIGGER_EVENT_DELETE | TRIGGER_EVENT_ROW | TRIGGER_EVENT_BEFORE;
	SaveTriggerData->tg_relation = rel;
	SaveTriggerData->tg_newtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		if (!trigger[i]->tgenabled)
			continue;
		CurrentTriggerData = SaveTriggerData;
		CurrentTriggerData->tg_trigtuple = trigtuple;
		CurrentTriggerData->tg_trigger = trigger[i];
		newtuple = ExecCallTriggerFunc(trigger[i]);
		if (newtuple == NULL)
			break;
		if (newtuple != trigtuple)
			pfree(newtuple);
	}
	CurrentTriggerData = NULL;
	pfree(SaveTriggerData);
	pfree(trigtuple);

	return (newtuple == NULL) ? false : true;
}

void
ExecARDeleteTriggers(EState *estate, ItemPointer tupleid)
{
	Relation	rel = estate->es_result_relation_info->ri_RelationDesc;
	HeapTuple	trigtuple = GetTupleForTrigger(estate, tupleid, NULL);

	DeferredTriggerSaveEvent(rel, TRIGGER_EVENT_DELETE, trigtuple, NULL);
	return;
}

HeapTuple
ExecBRUpdateTriggers(EState *estate, ItemPointer tupleid, HeapTuple newtuple)
{
	Relation	rel = estate->es_result_relation_info->ri_RelationDesc;
	TriggerData *SaveTriggerData;
	int			ntrigs = rel->trigdesc->n_before_row[TRIGGER_EVENT_UPDATE];
	Trigger   **trigger = rel->trigdesc->tg_before_row[TRIGGER_EVENT_UPDATE];
	HeapTuple	trigtuple;
	HeapTuple	oldtuple;
	HeapTuple	intuple = newtuple;
	TupleTableSlot *newSlot;
	int			i;

	trigtuple = GetTupleForTrigger(estate, tupleid, &newSlot);
	if (trigtuple == NULL)
		return NULL;

	/*
	 * In READ COMMITTED isolevel it's possible that newtuple was changed
	 * due to concurrent update.
	 */
	if (newSlot != NULL)
		intuple = newtuple = ExecRemoveJunk(estate->es_junkFilter, newSlot);

	SaveTriggerData = (TriggerData *) palloc(sizeof(TriggerData));
	SaveTriggerData->tg_event = TRIGGER_EVENT_UPDATE | TRIGGER_EVENT_ROW | TRIGGER_EVENT_BEFORE;
	SaveTriggerData->tg_relation = rel;
	for (i = 0; i < ntrigs; i++)
	{
		if (!trigger[i]->tgenabled)
			continue;
		CurrentTriggerData = SaveTriggerData;
		CurrentTriggerData->tg_trigtuple = trigtuple;
		CurrentTriggerData->tg_newtuple = oldtuple = newtuple;
		CurrentTriggerData->tg_trigger = trigger[i];
		newtuple = ExecCallTriggerFunc(trigger[i]);
		if (newtuple == NULL)
			break;
		else if (oldtuple != newtuple && oldtuple != intuple)
			pfree(oldtuple);
	}
	CurrentTriggerData = NULL;
	pfree(SaveTriggerData);
	pfree(trigtuple);
	return newtuple;
}

void
ExecARUpdateTriggers(EState *estate, ItemPointer tupleid, HeapTuple newtuple)
{
	Relation	rel = estate->es_result_relation_info->ri_RelationDesc;
	HeapTuple	trigtuple = GetTupleForTrigger(estate, tupleid, NULL);

	DeferredTriggerSaveEvent(rel, TRIGGER_EVENT_UPDATE, trigtuple, newtuple);
	return;
}

extern TupleTableSlot *EvalPlanQual(EState *estate, Index rti, ItemPointer tid);

static HeapTuple
GetTupleForTrigger(EState *estate, ItemPointer tid, TupleTableSlot **newSlot)
{
	Relation	relation = estate->es_result_relation_info->ri_RelationDesc;
	HeapTupleData tuple;
	HeapTuple	result;
	Buffer		buffer;

	if (newSlot != NULL)
	{
		int			test;

		/*
		 * mark tuple for update
		 */
		*newSlot = NULL;
		tuple.t_self = *tid;
ltrmark:;
		test = heap_mark4update(relation, &tuple, &buffer);
		switch (test)
		{
			case HeapTupleSelfUpdated:
				ReleaseBuffer(buffer);
				return (NULL);

			case HeapTupleMayBeUpdated:
				break;

			case HeapTupleUpdated:
				ReleaseBuffer(buffer);
				if (XactIsoLevel == XACT_SERIALIZABLE)
					elog(ERROR, "Can't serialize access due to concurrent update");
				else if (!(ItemPointerEquals(&(tuple.t_self), tid)))
				{
					TupleTableSlot *epqslot = EvalPlanQual(estate,
					 estate->es_result_relation_info->ri_RangeTableIndex,
														&(tuple.t_self));

					if (!(TupIsNull(epqslot)))
					{
						*tid = tuple.t_self;
						*newSlot = epqslot;
						goto ltrmark;
					}
				}

				/*
				 * if tuple was deleted or PlanQual failed for updated
				 * tuple - we have not process this tuple!
				 */
				return (NULL);

			default:
				ReleaseBuffer(buffer);
				elog(ERROR, "Unknown status %u from heap_mark4update", test);
				return (NULL);
		}
	}
	else
	{
		PageHeader	dp;
		ItemId		lp;

		buffer = ReadBuffer(relation, ItemPointerGetBlockNumber(tid));

		if (!BufferIsValid(buffer))
			elog(ERROR, "GetTupleForTrigger: failed ReadBuffer");

		dp = (PageHeader) BufferGetPage(buffer);
		lp = PageGetItemId(dp, ItemPointerGetOffsetNumber(tid));

		Assert(ItemIdIsUsed(lp));

		tuple.t_data = (HeapTupleHeader) PageGetItem((Page) dp, lp);
		tuple.t_len = ItemIdGetLength(lp);
		tuple.t_self = *tid;
	}

	result = heap_copytuple(&tuple);
	ReleaseBuffer(buffer);

	return result;
}


/* ----------
 * Deferred trigger stuff
 * ----------
 */


/* ----------
 * Internal data to the deferred trigger mechanism is held
 * during entire session in a global memor created at startup and
 * over statements/commands in a separate global memory which
 * is created at transaction start and destroyed at transaction
 * end.
 * ----------
 */
static GlobalMemory		deftrig_gcxt = NULL;
static GlobalMemory		deftrig_cxt = NULL;

/* ----------
 * Global data that tells which triggers are actually in
 * state IMMEDIATE or DEFERRED.
 * ----------
 */
static bool				deftrig_dfl_all_isset = false;
static bool				deftrig_dfl_all_isdeferred = false;
static List			   *deftrig_dfl_trigstates = NIL;

static bool				deftrig_all_isset;
static bool				deftrig_all_isdeferred;
static List			   *deftrig_trigstates;

/* ----------
 * The list of events during the entire transaction.
 *
 * XXX This must finally be held in a file because of the huge
 *     number of events that could occur in the real world.
 * ----------
 */
static int				deftrig_n_events;
static List			   *deftrig_events;


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
	MemoryContext			oldcxt;
	List				   *sl;
	DeferredTriggerStatus	trigstate;

	/* ----------
	 * Not deferrable triggers (i.e. normal AFTER ROW triggers
	 * and constraints declared NOT DEFERRABLE, the state is
	 * allways false.
	 * ----------
	 */
	if ((itemstate & TRIGGER_DEFERRED_DEFERRABLE) == 0)
		return false;

	/* ----------
	 * Lookup if we know an individual state for this trigger
	 * ----------
	 */
	foreach (sl, deftrig_trigstates)
	{
		trigstate = (DeferredTriggerStatus) lfirst(sl);
		if (trigstate->dts_tgoid == tgoid)
			return trigstate->dts_tgisdeferred;
	}

	/* ----------
	 * No individual state known - so if the user issued a
	 * SET CONSTRAINT ALL ..., we return that instead of the
	 * triggers default state.
	 * ----------
	 */
	if (deftrig_all_isset)
		return deftrig_all_isdeferred;

	/* ----------
	 * No ALL state known either, remember the default state
	 * as the current and return that.
	 * ----------
	 */
	oldcxt = MemoryContextSwitchTo((MemoryContext) deftrig_cxt);

	trigstate = (DeferredTriggerStatus)
			palloc(sizeof(DeferredTriggerStatusData));
	trigstate->dts_tgoid		= tgoid;
	trigstate->dts_tgisdeferred	= 
			((itemstate & TRIGGER_DEFERRED_INITDEFERRED) != 0);
	deftrig_trigstates = lappend(deftrig_trigstates, trigstate);

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
	deftrig_events = lappend(deftrig_events, event);
	deftrig_n_events++;

	return;
}


/* ----------
 * deferredTriggerGetPreviousEvent()
 *
 *	Backward scan the eventlist to find the event a given OLD tuple
 *	resulted from in the same transaction.
 * ----------
 */
static DeferredTriggerEvent
deferredTriggerGetPreviousEvent(Oid relid, ItemPointer ctid)
{
	DeferredTriggerEvent	previous;
	int						n;

	for (n = deftrig_n_events - 1; n >= 0; n--)
	{
		previous = (DeferredTriggerEvent) nth(n, deftrig_events);

		if (previous->dte_relid != relid)
			continue;
		if (previous->dte_event & TRIGGER_DEFERRED_CANCELED)
			continue;

		if (ItemPointerGetBlockNumber(ctid) == 
					ItemPointerGetBlockNumber(&(previous->dte_newctid)) &&
					ItemPointerGetOffsetNumber(ctid) ==
					ItemPointerGetOffsetNumber(&(previous->dte_newctid)))
			return previous;
	}

	elog(ERROR, 
		"deferredTriggerGetPreviousEvent(): event for tuple %s not found",
		tidout(ctid));
	return NULL;
}


/* ----------
 * deferredTriggerCancelEvent()
 *
 *	Mark an event in the eventlist as cancelled because it isn't
 *	required anymore (replaced by anoter event).
 * ----------
 */
static void
deferredTriggerCancelEvent(DeferredTriggerEvent event)
{
	event->dte_event |= TRIGGER_DEFERRED_CANCELED;
}


/* ----------
 * deferredTriggerExecute()
 *
 *	Fetch the required tuples back from the heap and fire one
 *	single trigger function.
 * ----------
 */
static void
deferredTriggerExecute(DeferredTriggerEvent event, int itemno)
{
	Relation		rel;
	TriggerData		SaveTriggerData;
	HeapTupleData	oldtuple;
	HeapTupleData	newtuple;
	HeapTuple		rettuple;
	Buffer			oldbuffer;
	Buffer			newbuffer;

	/* ----------
	 * Open the heap and fetch the required OLD and NEW tuples.
	 * ----------
	 */
	rel = heap_open(event->dte_relid, NoLock);

	if (ItemPointerIsValid(&(event->dte_oldctid)))
	{
		ItemPointerCopy(&(event->dte_oldctid), &(oldtuple.t_self));
		heap_fetch(rel, SnapshotAny, &oldtuple, &oldbuffer);
		if (!oldtuple.t_data)
			elog(ERROR, "deferredTriggerExecute(): failed to fetch old tuple");
	}

	if (ItemPointerIsValid(&(event->dte_newctid)))
	{
		ItemPointerCopy(&(event->dte_newctid), &(newtuple.t_self));
		heap_fetch(rel, SnapshotAny, &newtuple, &newbuffer);
		if (!newtuple.t_data)
			elog(ERROR, "deferredTriggerExecute(): failed to fetch new tuple");
	}

	/* ----------
	 * Setup the trigger information
	 * ----------
	 */
	SaveTriggerData.tg_event    = event->dte_event | TRIGGER_EVENT_ROW;
	SaveTriggerData.tg_relation = rel;

	switch (event->dte_event)
	{
		case TRIGGER_EVENT_INSERT:
			SaveTriggerData.tg_trigtuple = &newtuple;
			SaveTriggerData.tg_newtuple  = NULL;
			SaveTriggerData.tg_trigger   = 
					rel->trigdesc->tg_after_row[TRIGGER_EVENT_INSERT][itemno];
			break;

		case TRIGGER_EVENT_UPDATE:
			SaveTriggerData.tg_trigtuple = &oldtuple;
			SaveTriggerData.tg_newtuple  = &newtuple;
			SaveTriggerData.tg_trigger   = 
					rel->trigdesc->tg_after_row[TRIGGER_EVENT_UPDATE][itemno];
			break;

		case TRIGGER_EVENT_DELETE:
			SaveTriggerData.tg_trigtuple = &oldtuple;
			SaveTriggerData.tg_newtuple  = NULL;
			SaveTriggerData.tg_trigger   = 
					rel->trigdesc->tg_after_row[TRIGGER_EVENT_DELETE][itemno];
			break;

		default:
	} 

	/* ----------
	 * Call the trigger and throw away an eventually returned
	 * updated tuple.
	 * ----------
	 */
	CurrentTriggerData = &SaveTriggerData;
	rettuple = ExecCallTriggerFunc(SaveTriggerData.tg_trigger);
	CurrentTriggerData = NULL;
	if (rettuple != NULL && rettuple != &oldtuple && rettuple != &newtuple)
		pfree(rettuple);

	/* ----------
	 * Might have been a referential integrity constraint trigger.
	 * Reset the snapshot overriding flag.
	 * ----------
	 */
	ReferentialIntegritySnapshotOverride = false;

	/* ----------
	 * Release buffers and close the relation
	 * ----------
	 */
	if (ItemPointerIsValid(&(event->dte_oldctid)))
		ReleaseBuffer(oldbuffer);
	if (ItemPointerIsValid(&(event->dte_newctid)))
		ReleaseBuffer(newbuffer);

	heap_close(rel, NoLock);

	return;
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
	List					*el;
	DeferredTriggerEvent	event;
	int						still_deferred_ones;
	int						eventno = -1;
	int						i;

	/* ----------
	 * For now we process all events - to speedup transaction blocks
	 * we need to remember the actual end of the queue at EndQuery
	 * and process only events that are newer. On state changes we
	 * simply reset the position to the beginning of the queue and
	 * process all events once with the new states when the
	 * SET CONSTRAINTS ... command finishes and calls EndQuery.
	 * ----------
	 */
	foreach (el, deftrig_events)
	{
		eventno++;

		/* ----------
		 * Get the event and check if it is completely done.
		 * ----------
		 */
		event = (DeferredTriggerEvent) lfirst(el);
		if (event->dte_event & (TRIGGER_DEFERRED_DONE | 
								TRIGGER_DEFERRED_CANCELED))
			continue;
	
		/* ----------
		 * Check each trigger item in the event.
		 * ----------
		 */
		still_deferred_ones = false;
		for (i = 0; i < event->dte_n_items; i++)
		{
			if (event->dte_item[i].dti_state & TRIGGER_DEFERRED_DONE)
				continue;

			/* ----------
			 * This trigger item hasn't been called yet. Check if
			 * we should call it now.
			 * ----------
			 */
			if (immediate_only && deferredTriggerCheckState(
								event->dte_item[i].dti_tgoid, 
								event->dte_item[i].dti_state))
			{
				still_deferred_ones = true;
				continue;
			}

			/* ----------
			 * So let's fire it...
			 * ----------
			 */
			deferredTriggerExecute(event, i);
			event->dte_item[i].dti_state |= TRIGGER_DEFERRED_DONE;
		}

		/* ----------
		 * Remember in the event itself if all trigger items are
		 * done.
		 * ----------
		 */
		if (!still_deferred_ones)
			event->dte_event |= TRIGGER_DEFERRED_DONE;
	}
}


/* ----------
 * DeferredTriggerInit()
 *
 *	Initialize the deferred trigger mechanism. This is called during
 *	backend startup and is guaranteed to be before the first of all
 *	transactions.
 * ----------
 */
int
DeferredTriggerInit(void)
{
	deftrig_gcxt = CreateGlobalMemory("DeferredTriggerSession");
	return 0;
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
	MemoryContext			oldcxt;
	List					*l;
	DeferredTriggerStatus	dflstat;
	DeferredTriggerStatus	stat;

	if (deftrig_cxt != NULL)
		elog(FATAL, 
			"DeferredTriggerBeginXact() called while inside transaction");

	/* ----------
	 * Create the per transaction memory context and copy all states
	 * from the per session context to here.
	 * ----------
	 */
	deftrig_cxt				= CreateGlobalMemory("DeferredTriggerXact");
	oldcxt = MemoryContextSwitchTo((MemoryContext)deftrig_cxt);

	deftrig_all_isset		= deftrig_dfl_all_isset;
	deftrig_all_isdeferred	= deftrig_dfl_all_isdeferred;
	
	deftrig_trigstates		= NIL;
	foreach (l, deftrig_dfl_trigstates)
	{
		dflstat = (DeferredTriggerStatus) lfirst(l);
		stat    = (DeferredTriggerStatus) 
								palloc(sizeof(DeferredTriggerStatusData));

		stat->dts_tgoid        = dflstat->dts_tgoid;
		stat->dts_tgisdeferred = dflstat->dts_tgisdeferred;

		deftrig_trigstates = lappend(deftrig_trigstates, stat);
	}

	MemoryContextSwitchTo(oldcxt);

	deftrig_n_events	= 0;
	deftrig_events		= NIL;
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
	/* ----------
	 * Ignore call if we aren't in a transaction.
	 * ----------
	 */
	if (deftrig_cxt == NULL)
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
	/* ----------
	 * Ignore call if we aren't in a transaction.
	 * ----------
	 */
	if (deftrig_cxt == NULL)
		return;

	deferredTriggerInvokeEvents(false);

	GlobalMemoryDestroy(deftrig_cxt);
	deftrig_cxt = NULL;
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
	/* ----------
	 * Ignore call if we aren't in a transaction.
	 * ----------
	 */
	if (deftrig_cxt == NULL)
		return;

	GlobalMemoryDestroy(deftrig_cxt);
	deftrig_cxt = NULL;
}


/* ----------
 * DeferredTriggerSetState()
 *
 *	Called for the users SET CONSTRAINTS ... utility command.
 * ----------
 */
void
DeferredTriggerSetState(ConstraintsSetStmt *stmt)
{
	Relation				tgrel;
	Relation				irel;
	List					*l;
	List					*ls;
	List					*lnext;
	List					*loid = NIL;
	MemoryContext			oldcxt;
	bool					found;
	DeferredTriggerStatus	state;

	/* ----------
	 * Handle SET CONSTRAINTS ALL ...
	 * ----------
	 */
	if (stmt->constraints == NIL) {
		if (!IsTransactionBlock())
		{
			/* ----------
			 * ... outside of a transaction block
			 * ----------
			 */
			oldcxt = MemoryContextSwitchTo((MemoryContext) deftrig_gcxt);

			/* ----------
			 * Drop all information about individual trigger states per
			 * session.
			 * ----------
			 */
			l = deftrig_dfl_trigstates;
			while (l != NIL)
			{
				lnext = lnext(l);
				pfree(lfirst(l));
				pfree(l);
				l = lnext;
			}
			deftrig_dfl_trigstates = NIL;

			/* ----------
			 * Set the session ALL state to known.
			 * ----------
			 */
			deftrig_dfl_all_isset      = true;
			deftrig_dfl_all_isdeferred = stmt->deferred;

			MemoryContextSwitchTo(oldcxt);

			return;
		} else {
			/* ----------
			 * ... inside of a transaction block
			 * ----------
			 */
			oldcxt = MemoryContextSwitchTo((MemoryContext) deftrig_cxt);

			/* ----------
			 * Drop all information about individual trigger states per
			 * transaction.
			 * ----------
			 */
			l = deftrig_trigstates;
			while (l != NIL)
			{
				lnext = lnext(l);
				pfree(lfirst(l));
				pfree(l);
				l = lnext;
			}
			deftrig_trigstates = NIL;

			/* ----------
			 * Set the per transaction ALL state to known.
			 * ----------
			 */
			deftrig_all_isset      = true;
			deftrig_all_isdeferred = stmt->deferred;

			MemoryContextSwitchTo(oldcxt);

			return;
		}
	}

	/* ----------
	 * Handle SET CONSTRAINTS constraint-name [, ...]
	 * First lookup all trigger Oid's for the constraint names.
	 * ----------
	 */
	tgrel = heap_openr(TriggerRelationName, AccessShareLock);
	irel = index_openr(TriggerConstrNameIndex);

	foreach (l, stmt->constraints)
	{
		ScanKeyData			skey;
		HeapTupleData		tuple;
		IndexScanDesc		sd;
		RetrieveIndexResult	indexRes;
		Buffer				buffer;
		Form_pg_trigger		pg_trigger;
		Oid					constr_oid;

		/* ----------
		 * Check that only named constraints are set explicitly
		 * ----------
		 */
		if (strcmp((char *)lfirst(l), "") == 0)
			elog(ERROR, "unnamed constraints cannot be set explicitly");

		/* ----------
		 * Setup to scan pg_trigger by tgconstrname ...
		 * ----------
		 */
		ScanKeyEntryInitialize(&skey,
							   (bits16) 0x0,
							   (AttrNumber) 1,
							   (RegProcedure) F_NAMEEQ,
							   PointerGetDatum((char *)lfirst(l)));

		sd = index_beginscan(irel, false, 1, &skey);

		/* ----------
		 * ... and search for the constraint trigger row
		 * ----------
		 */
		found = false;
		for (;;)
		{
			indexRes = index_getnext(sd, ForwardScanDirection);
			if (!indexRes)
				break;

			tuple.t_self = indexRes->heap_iptr;
			heap_fetch(tgrel, SnapshotNow, &tuple, &buffer);
			pfree(indexRes);
			if (!tuple.t_data)
			{
				ReleaseBuffer(buffer);
				continue;
			}

			/* ----------
			 * If we found some, check that they fit the deferrability
			 * ----------
			 */
			pg_trigger = (Form_pg_trigger) GETSTRUCT(&tuple);
			if (stmt->deferred & !pg_trigger->tgdeferrable)
				elog(ERROR, "Constraint '%s' is not deferrable", 
									(char *)lfirst(l));

			constr_oid = tuple.t_data->t_oid;
			loid = lappend(loid, (Node *)constr_oid);
			found = true;

			ReleaseBuffer(buffer);
		}

		/* ----------
		 * Not found ?
		 * ----------
		 */
		if (!found)
			elog(ERROR, "Constraint '%s' does not exist", (char *)lfirst(l));

		index_endscan(sd);

	}
	index_close(irel);
	heap_close(tgrel, AccessShareLock);


	if (!IsTransactionBlock())
	{
		/* ----------
		 * Outside of a transaction block set the trigger
		 * states of individual triggers on session level.
		 * ----------
		 */
		oldcxt = MemoryContextSwitchTo((MemoryContext) deftrig_gcxt);

		foreach (l, loid)
		{
			found = false;
			foreach (ls, deftrig_dfl_trigstates)
			{
				state = (DeferredTriggerStatus) lfirst(ls);
				if (state->dts_tgoid == (Oid) lfirst(l))
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
				state->dts_tgoid        = (Oid) lfirst(l);
				state->dts_tgisdeferred = stmt->deferred;

				deftrig_dfl_trigstates = 
									lappend(deftrig_dfl_trigstates, state);
			}
		}

		MemoryContextSwitchTo(oldcxt);

		return;
	} else {
		/* ----------
		 * Inside of a transaction block set the trigger
		 * states of individual triggers on transaction level.
		 * ----------
		 */
		oldcxt = MemoryContextSwitchTo((MemoryContext) deftrig_cxt);

		foreach (l, loid)
		{
			found = false;
			foreach (ls, deftrig_trigstates)
			{
				state = (DeferredTriggerStatus) lfirst(ls);
				if (state->dts_tgoid == (Oid) lfirst(l))
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
				state->dts_tgoid        = (Oid) lfirst(l);
				state->dts_tgisdeferred = stmt->deferred;

				deftrig_trigstates = 
									lappend(deftrig_trigstates, state);
			}
		}

		MemoryContextSwitchTo(oldcxt);

		return;
	}
}


/* ----------
 * DeferredTriggerSaveEvent()
 *
 *	Called by ExecAR...Triggers() to add the event to the queue.
 * ----------
 */
void
DeferredTriggerSaveEvent(Relation rel, int event,
					HeapTuple oldtup, HeapTuple newtup)
{
	MemoryContext			oldcxt;
	DeferredTriggerEvent	new_event;
	DeferredTriggerEvent	prev_event;
	bool					prev_done = false;
	int						new_size;
	int						i;
	int						ntriggers;
	Trigger				  **triggers;
	ItemPointerData			oldctid;
	ItemPointerData			newctid;

	if (deftrig_cxt == NULL)
		elog(ERROR, 
			"DeferredTriggerSaveEvent() called outside of transaction");

	/* ----------
	 * Check if we're interested in this row at all
	 * ----------
	 */
	if (rel->trigdesc->n_after_row[TRIGGER_EVENT_INSERT] == 0 &&
				rel->trigdesc->n_after_row[TRIGGER_EVENT_UPDATE] == 0 &&
				rel->trigdesc->n_after_row[TRIGGER_EVENT_DELETE] == 0 &&
				rel->trigdesc->n_before_row[TRIGGER_EVENT_INSERT] == 0 &&
				rel->trigdesc->n_before_row[TRIGGER_EVENT_UPDATE] == 0 &&
				rel->trigdesc->n_before_row[TRIGGER_EVENT_DELETE] == 0)
		return;

	/* ----------
	 * Get the CTID's of OLD and NEW
	 * ----------
	 */
	if (oldtup != NULL)
		ItemPointerCopy(&(oldtup->t_self), &(oldctid));
	else
		ItemPointerSetInvalid(&(oldctid));
	if (newtup != NULL)
		ItemPointerCopy(&(newtup->t_self), &(newctid));
	else
		ItemPointerSetInvalid(&(newctid));

	/* ----------
	 * Eventually modify the event and do some general RI violation checks
	 * ----------
	 */
	switch (event)
	{
		case TRIGGER_EVENT_INSERT:
			/* ----------
			 * Don't know how to (surely) check if another tuple with
			 * this meaning (from all FK's point of view) got deleted
			 * in the same transaction. Thus not handled yet.
			 * ----------
			 */
			break;

		case TRIGGER_EVENT_UPDATE:
			/* ----------
			 * On UPDATE check if the tuple updated is a result
			 * of the same transaction.
			 * ----------
			 */
			if (oldtup->t_data->t_xmin != GetCurrentTransactionId())
				break;

			/* ----------
			 * Look at the previous event to the same tuple if
			 * any of it's triggers has already been executed.
			 * Such a situation would potentially violate RI
			 * so we abort the transaction.
			 * ----------
			 */
			prev_event = deferredTriggerGetPreviousEvent(rel->rd_id, &oldctid);
			if (prev_event->dte_event & TRIGGER_DEFERRED_HAS_BEFORE	||
					(prev_event->dte_n_items != 0 &&
					 prev_event->dte_event & TRIGGER_DEFERRED_DONE))
				prev_done = true;
			else
				for (i = 0; i < prev_event->dte_n_items; i++)
				{
					if (prev_event->dte_item[i].dti_state &
									TRIGGER_DEFERRED_DONE)
					{
						prev_done = true;
						break;
					}
				}

			if (prev_done)
			{
				elog(NOTICE, "UPDATE of row inserted/updated in same "
						"transaction violates");
				elog(NOTICE, "referential integrity semantics. Other "
						"triggers or IMMEDIATE ");
				elog(ERROR, " constraints have already been executed.");
			}

			/* ----------
			 * Anything's fine so far - i.e. none of the previous
			 * events triggers has been executed up to now. Let's
			 * the REAL event that happened so far.
			 * ----------
			 */
			switch (prev_event->dte_event & TRIGGER_EVENT_OPMASK)
			{
				case TRIGGER_EVENT_INSERT:
					/* ----------
					 * The previous operation was an insert.
					 * So the REAL new event is an INSERT of
					 * the new tuple.
					 * ----------
					 */
					event = TRIGGER_EVENT_INSERT;
					ItemPointerSetInvalid(&oldctid);
					deferredTriggerCancelEvent(prev_event);
					break;

				case TRIGGER_EVENT_UPDATE:
					/* ----------
					 * The previous operation was an UPDATE.
					 * So the REAL new event is still an UPDATE
					 * but from the original tuple to this new one.
					 * ----------
					 */
					event = TRIGGER_EVENT_UPDATE;
					ItemPointerCopy(&(prev_event->dte_oldctid), &oldctid);
					deferredTriggerCancelEvent(prev_event);
					break;
			}

			break;

		case TRIGGER_EVENT_DELETE:
			/* ----------
			 * On DELETE check if the tuple updated is a result
			 * of the same transaction.
			 * ----------
			 */
			if (oldtup->t_data->t_xmin != GetCurrentTransactionId())
				break;

			/* ----------
			 * Look at the previous event to the same tuple if
			 * any of it's triggers has already been executed.
			 * Such a situation would potentially violate RI
			 * so we abort the transaction.
			 * ----------
			 */
			prev_event = deferredTriggerGetPreviousEvent(rel->rd_id, &oldctid);
			if (prev_event->dte_event & TRIGGER_DEFERRED_HAS_BEFORE	||
					(prev_event->dte_n_items != 0 &&
					 prev_event->dte_event & TRIGGER_DEFERRED_DONE))
				prev_done = true;
			else
				for (i = 0; i < prev_event->dte_n_items; i++)
				{
					if (prev_event->dte_item[i].dti_state &
									TRIGGER_DEFERRED_DONE)
					{
						prev_done = true;
						break;
					}
				}

			if (prev_done)
			{
				elog(NOTICE, "DELETE of row inserted/updated in same "
						"transaction violates");
				elog(NOTICE, "referential integrity semantics. Other "
						"triggers or IMMEDIATE ");
				elog(ERROR, " constraints have already been executed.");
			}

			/* ----------
			 * Anything's fine so far - i.e. none of the previous
			 * events triggers has been executed up to now. Let's
			 * the REAL event that happened so far.
			 * ----------
			 */
			switch (prev_event->dte_event & TRIGGER_EVENT_OPMASK)
			{
				case TRIGGER_EVENT_INSERT:
					/* ----------
					 * The previous operation was an insert.
					 * So the REAL new event is nothing.
					 * ----------
					 */
					deferredTriggerCancelEvent(prev_event);
					return;

				case TRIGGER_EVENT_UPDATE:
					/* ----------
					 * The previous operation was an UPDATE.
					 * So the REAL new event is a DELETE
					 * but from the original tuple.
					 * ----------
					 */
					event = TRIGGER_EVENT_DELETE;
					ItemPointerCopy(&(prev_event->dte_oldctid), &oldctid);
					deferredTriggerCancelEvent(prev_event);
					break;
			}

			break;
	}
	
	/* ----------
	 * Create a new event and save it.
	 * ----------
	 */
	oldcxt = MemoryContextSwitchTo((MemoryContext) deftrig_cxt);

	ntriggers = rel->trigdesc->n_after_row[event];
	triggers  = rel->trigdesc->tg_after_row[event];

	new_size  = sizeof(DeferredTriggerEventData) +
				ntriggers * sizeof(DeferredTriggerEventItem);
				
	new_event = (DeferredTriggerEvent) palloc(new_size);
	new_event->dte_event	= event;
	new_event->dte_relid	= rel->rd_id;
	ItemPointerCopy(&oldctid, &(new_event->dte_oldctid));
	ItemPointerCopy(&newctid, &(new_event->dte_newctid));
	new_event->dte_n_items = ntriggers;
	new_event->dte_item[ntriggers].dti_state = new_size;
	for (i = 0; i < ntriggers; i++)
	{
		new_event->dte_item[i].dti_tgoid = triggers[i]->tgoid;
		new_event->dte_item[i].dti_state = 
			((triggers[i]->tgdeferrable) ? 
						TRIGGER_DEFERRED_DEFERRABLE : 0)	|
			((triggers[i]->tginitdeferred) ? 
						TRIGGER_DEFERRED_INITDEFERRED : 0)	|
			((rel->trigdesc->n_before_row[event] > 0) ?
						TRIGGER_DEFERRED_HAS_BEFORE : 0);
	}

	deferredTriggerAddEvent(new_event);

	MemoryContextSwitchTo(oldcxt);

	return;
}


