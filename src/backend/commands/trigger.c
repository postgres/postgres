/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"


#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/syscache.h"

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

	if (!allowSystemTableMods && IsSystemRelationName(stmt->relname))
		elog(ERROR, "CreateTrigger: can't create trigger for system relation %s", stmt->relname);

#ifndef NO_SECURITY
	if (!pg_ownercheck(GetPgUserName(), stmt->relname, RELNAME))
		elog(ERROR, "%s: %s", stmt->relname, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
#endif

	rel = heap_openr(stmt->relname);
	if (!RelationIsValid(rel))
		elog(ERROR, "CreateTrigger: there is no relation %s", stmt->relname);

	LockRelation(rel, AccessExclusiveLock);

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
	tgrel = heap_openr(TriggerRelationName);
	LockRelation(tgrel, AccessExclusiveLock);
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
	tuple = SearchSysCacheTuple(PRONAME,
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
	if (((Form_pg_proc) GETSTRUCT(tuple))->prolang != ClanguageId)
	{
		HeapTuple	langTup;

		langTup = SearchSysCacheTuple(LANOID,
			ObjectIdGetDatum(((Form_pg_proc) GETSTRUCT(tuple))->prolang),
									  0, 0, 0);
		if (!HeapTupleIsValid(langTup))
			elog(ERROR, "CreateTrigger: cache lookup for PL failed");

		if (((Form_pg_language) GETSTRUCT(langTup))->lanispl == false)
			elog(ERROR, "CreateTrigger: only C and PL functions are supported");
	}

	MemSet(nulls, ' ', Natts_pg_trigger * sizeof(char));

	values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_trigger_tgname - 1] = NameGetDatum(namein(stmt->trigname));
	values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum(tuple->t_data->t_oid);
	values[Anum_pg_trigger_tgtype - 1] = Int16GetDatum(tgtype);
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
	UnlockRelation(tgrel, AccessExclusiveLock);
	heap_close(tgrel);

	pfree(DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
	pfree(DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));

	/* update pg_class */
	tuple = SearchSysCacheTupleCopy(RELNAME,
									PointerGetDatum(stmt->relname),
									0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "CreateTrigger: relation %s not found in pg_class", stmt->relname);

	pgrel = heap_openr(RelationRelationName);
	((Form_pg_class) GETSTRUCT(tuple))->reltriggers = found + 1;
	RelationInvalidateHeapTuple(pgrel, tuple);
	heap_replace(pgrel, &tuple->t_self, tuple, NULL);
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
	CatalogIndexInsert(ridescs, Num_pg_class_indices, pgrel, tuple);
	CatalogCloseIndices(Num_pg_class_indices, ridescs);
	pfree(tuple);
	heap_close(pgrel);

	CommandCounterIncrement();
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
	FreeTriggerDesc(rel);
	rel->rd_rel->reltriggers = found + 1;
	RelationBuildTriggers(rel);
	MemoryContextSwitchTo(oldcxt);
	heap_close(rel);
	return;
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

	rel = heap_openr(stmt->relname);
	if (!RelationIsValid(rel))
		elog(ERROR, "DropTrigger: there is no relation %s", stmt->relname);

	LockRelation(rel, AccessExclusiveLock);

	tgrel = heap_openr(TriggerRelationName);
	LockRelation(tgrel, AccessExclusiveLock);
	ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgrelid,
						   F_OIDEQ, RelationGetRelid(rel));
	tgscan = heap_beginscan(tgrel, 0, SnapshotNow, 1, &key);
	while (HeapTupleIsValid(tuple = heap_getnext(tgscan, 0)))
	{
		Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);

		if (namestrcmp(&(pg_trigger->tgname), stmt->trigname) == 0)
		{
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
		elog(NOTICE, "DropTrigger: found (and deleted) %d trigger %s on relation %s",
			 tgfound, stmt->trigname, stmt->relname);
	heap_endscan(tgscan);
	UnlockRelation(tgrel, AccessExclusiveLock);
	heap_close(tgrel);

	tuple = SearchSysCacheTupleCopy(RELNAME,
									PointerGetDatum(stmt->relname),
									0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "DropTrigger: relation %s not found in pg_class", stmt->relname);

	/* update pg_class */
	pgrel = heap_openr(RelationRelationName);
	((Form_pg_class) GETSTRUCT(tuple))->reltriggers = found;
	RelationInvalidateHeapTuple(pgrel, tuple);
	heap_replace(pgrel, &tuple->t_self, tuple, NULL);
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, ridescs);
	CatalogIndexInsert(ridescs, Num_pg_class_indices, pgrel, tuple);
	CatalogCloseIndices(Num_pg_class_indices, ridescs);
	pfree(tuple);
	heap_close(pgrel);

	CommandCounterIncrement();
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);
	FreeTriggerDesc(rel);
	rel->rd_rel->reltriggers = found;
	if (found > 0)
		RelationBuildTriggers(rel);
	MemoryContextSwitchTo(oldcxt);
	heap_close(rel);
	return;
}

void
RelationRemoveTriggers(Relation rel)
{
	Relation	tgrel;
	HeapScanDesc tgscan;
	ScanKeyData key;
	HeapTuple	tup;

	tgrel = heap_openr(TriggerRelationName);
	LockRelation(tgrel, AccessExclusiveLock);
	ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgrelid,
						   F_OIDEQ, RelationGetRelid(rel));

	tgscan = heap_beginscan(tgrel, 0, SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tup = heap_getnext(tgscan, 0)))
		heap_delete(tgrel, &tup->t_self, NULL);

	heap_endscan(tgscan);
	UnlockRelation(tgrel, AccessExclusiveLock);
	heap_close(tgrel);

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

	tgrel = heap_openr(TriggerRelationName);
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
				 NAMEDATALEN, relation->rd_rel->relname.data);

		pg_trigger = (Form_pg_trigger) GETSTRUCT(&tuple);

		if (triggers == NULL)
			triggers = (Trigger *) palloc(sizeof(Trigger));
		else
			triggers = (Trigger *) repalloc(triggers, (found + 1) * sizeof(Trigger));
		build = &(triggers[found]);

		build->tgname = nameout(&(pg_trigger->tgname));
		build->tgfoid = pg_trigger->tgfoid;
		build->tgfunc.fn_addr = NULL;
		build->tgtype = pg_trigger->tgtype;
		build->tgnargs = pg_trigger->tgnargs;
		memcpy(build->tgattr, &(pg_trigger->tgattr), 8 * sizeof(int16));
		val = (struct varlena *) fastgetattr(&tuple,
											 Anum_pg_trigger_tgargs,
											 tgrel->rd_att, &isnull);
		if (isnull)
			elog(ERROR, "RelationBuildTriggers: tgargs IS NULL for rel %.*s",
				 NAMEDATALEN, relation->rd_rel->relname.data);
		if (build->tgnargs > 0)
		{
			char	   *p;
			int			i;

			val = (struct varlena *) fastgetattr(&tuple,
												 Anum_pg_trigger_tgargs,
												 tgrel->rd_att, &isnull);
			if (isnull)
				elog(ERROR, "RelationBuildTriggers: tgargs IS NULL for rel %.*s",
					 NAMEDATALEN, relation->rd_rel->relname.data);
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
			 NAMEDATALEN, relation->rd_rel->relname.data);

	index_endscan(sd);
	pfree(sd);
	index_close(irel);
	heap_close(tgrel);

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
	TriggerData *SaveTriggerData;
	int			ntrigs = rel->trigdesc->n_after_row[TRIGGER_EVENT_INSERT];
	Trigger   **trigger = rel->trigdesc->tg_after_row[TRIGGER_EVENT_INSERT];
	int			i;

	SaveTriggerData = (TriggerData *) palloc(sizeof(TriggerData));
	SaveTriggerData->tg_event = TRIGGER_EVENT_INSERT | TRIGGER_EVENT_ROW;
	SaveTriggerData->tg_relation = rel;
	SaveTriggerData->tg_newtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		CurrentTriggerData = SaveTriggerData;
		CurrentTriggerData->tg_trigtuple = trigtuple;
		CurrentTriggerData->tg_trigger = trigger[i];
		ExecCallTriggerFunc(trigger[i]);
	}
	CurrentTriggerData = NULL;
	pfree(SaveTriggerData);
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
	TriggerData *SaveTriggerData;
	int			ntrigs = rel->trigdesc->n_after_row[TRIGGER_EVENT_DELETE];
	Trigger   **trigger = rel->trigdesc->tg_after_row[TRIGGER_EVENT_DELETE];
	HeapTuple	trigtuple;
	int			i;

	trigtuple = GetTupleForTrigger(estate, tupleid, NULL);
	Assert(trigtuple != NULL);

	SaveTriggerData = (TriggerData *) palloc(sizeof(TriggerData));
	SaveTriggerData->tg_event = TRIGGER_EVENT_DELETE | TRIGGER_EVENT_ROW;
	SaveTriggerData->tg_relation = rel;
	SaveTriggerData->tg_newtuple = NULL;
	for (i = 0; i < ntrigs; i++)
	{
		CurrentTriggerData = SaveTriggerData;
		CurrentTriggerData->tg_trigtuple = trigtuple;
		CurrentTriggerData->tg_trigger = trigger[i];
		ExecCallTriggerFunc(trigger[i]);
	}
	CurrentTriggerData = NULL;
	pfree(SaveTriggerData);
	pfree(trigtuple);
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
	TriggerData *SaveTriggerData;
	int			ntrigs = rel->trigdesc->n_after_row[TRIGGER_EVENT_UPDATE];
	Trigger   **trigger = rel->trigdesc->tg_after_row[TRIGGER_EVENT_UPDATE];
	HeapTuple	trigtuple;
	int			i;

	trigtuple = GetTupleForTrigger(estate, tupleid, NULL);
	Assert(trigtuple != NULL);

	SaveTriggerData = (TriggerData *) palloc(sizeof(TriggerData));
	SaveTriggerData->tg_event = TRIGGER_EVENT_UPDATE | TRIGGER_EVENT_ROW;
	SaveTriggerData->tg_relation = rel;
	for (i = 0; i < ntrigs; i++)
	{
		CurrentTriggerData = SaveTriggerData;
		CurrentTriggerData->tg_trigtuple = trigtuple;
		CurrentTriggerData->tg_newtuple = newtuple;
		CurrentTriggerData->tg_trigger = trigger[i];
		ExecCallTriggerFunc(trigger[i]);
	}
	CurrentTriggerData = NULL;
	pfree(SaveTriggerData);
	pfree(trigtuple);
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
