/*-------------------------------------------------------------------------
 *
 * trigger.c--
 *    PostgreSQL TRIGGERs support code.
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include "postgres.h"

#include "nodes/parsenodes.h"
#include "nodes/memnodes.h"
#include "commands/trigger.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#include "catalog/pg_trigger.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "storage/lmgr.h"
#include "storage/bufmgr.h"
#include "utils/mcxt.h"
#include "utils/inval.h"
#include "utils/builtins.h"

#ifndef NO_SECURITY
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/syscache.h"
#endif

TriggerData *CurrentTriggerData = NULL;

void RelationBuildTriggers (Relation relation);
void FreeTriggerDesc (Relation relation);

static void DescribeTrigger (TriggerDesc *trigdesc, Trigger *trigger);

extern void fmgr_info(Oid procedureId, func_ptr *function, int *nargs);
extern GlobalMemory CacheCxt;

void
CreateTrigger (CreateTrigStmt *stmt)
{
    int16 tgtype;
    int16 tgattr[8] = {0};
    Datum values[Natts_pg_trigger];
    char nulls[Natts_pg_trigger];
    Relation rel;
    Relation tgrel;
    HeapScanDesc tgscan;
    ScanKeyData	key;
    Relation relrdesc;
    HeapTuple tuple;
    ItemPointerData oldTID;
    Relation idescs[Num_pg_trigger_indices];
    Relation ridescs[Num_pg_class_indices];
    MemoryContext oldcxt;
    Oid fargtypes[8];
    int found = 0;
    int i;
    
    if ( IsSystemRelationName (stmt->relname) )
	elog(WARN, "CreateTrigger: can't create trigger for system relation %s", stmt->relname);

#ifndef NO_SECURITY
    if ( !pg_ownercheck (GetPgUserName (), stmt->relname, RELNAME))
    	elog(WARN, "%s: %s", stmt->relname, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
#endif
    
    rel = heap_openr (stmt->relname);
    if ( !RelationIsValid (rel) )
    	elog (WARN, "CreateTrigger: there is no relation %s", stmt->relname);
    
    RelationSetLockForWrite (rel);
    
    TRIGGER_CLEAR_TYPE (tgtype);
    if ( stmt->before )
    	TRIGGER_SETT_BEFORE (tgtype);
    if ( stmt->row )
    	TRIGGER_SETT_ROW (tgtype);
    for (i = 0; i < 3 && stmt->actions[i]; i++)
    {
    	switch ( stmt->actions[i] )
    	{
    	    case 'i': 
    	    	if ( TRIGGER_FOR_INSERT (tgtype) )
    	    	    elog (WARN, "CreateTrigger: double INSERT event specified");
    	    	TRIGGER_SETT_INSERT (tgtype);
    	    	break;
    	    case 'd': 
    	    	if ( TRIGGER_FOR_DELETE (tgtype) )
    	    	    elog (WARN, "CreateTrigger: double DELETE event specified");
    	    	TRIGGER_SETT_DELETE (tgtype);
    	    	break;
    	    case 'u': 
    	    	if ( TRIGGER_FOR_UPDATE (tgtype) )
    	    	    elog (WARN, "CreateTrigger: double UPDATE event specified");
    	    	TRIGGER_SETT_UPDATE (tgtype);
    	    	break;
    	    default: 
    	    	elog (WARN, "CreateTrigger: unknown event specified");
    	    	break;
    	}
    }
    
    /* Scan pg_trigger */
    tgrel = heap_openr (TriggerRelationName);
    RelationSetLockForWrite (tgrel);
    ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgrelid,
			   ObjectIdEqualRegProcedure, rel->rd_id);
    tgscan = heap_beginscan (tgrel, 0, NowTimeQual, 1, &key);
    while (tuple = heap_getnext (tgscan, 0, (Buffer *)NULL), PointerIsValid(tuple))
    {
    	Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT (tuple);
    	if ( namestrcmp (&(pg_trigger->tgname), stmt->trigname) == 0 )
    	    elog (WARN, "CreateTrigger: trigger %s already defined on relation %s", 
    	    			stmt->trigname, stmt->relname);
    	else
    	    found++;
    }
    heap_endscan (tgscan);
    
    memset (fargtypes, 0, 8 * sizeof(Oid));
    tuple = SearchSysCacheTuple (PRONAME, 
    				 PointerGetDatum (stmt->funcname),
    				 0, PointerGetDatum (fargtypes), 0);
    if ( !HeapTupleIsValid (tuple) || 
    		((Form_pg_proc)GETSTRUCT(tuple))->prorettype != 0 ||
	    		((Form_pg_proc)GETSTRUCT(tuple))->pronargs != 0 )
    	elog (WARN, "CreateTrigger: function %s () does not exist", stmt->funcname);
    
    if ( ((Form_pg_proc)GETSTRUCT(tuple))->prolang != ClanguageId )
    	elog (WARN, "CreateTrigger: only C functions are supported");
    
    memset (nulls, ' ', Natts_pg_trigger * sizeof (char));
    
    values[Anum_pg_trigger_tgrelid - 1] = ObjectIdGetDatum (rel->rd_id);
    values[Anum_pg_trigger_tgname - 1] = NameGetDatum (namein (stmt->trigname));
    values[Anum_pg_trigger_tgfoid - 1] = ObjectIdGetDatum (tuple->t_oid);
    values[Anum_pg_trigger_tgtype - 1] = Int16GetDatum (tgtype);
    if ( stmt->args )
    {
    	List *le;
    	char *args;
    	int16 nargs = length (stmt->args);
    	int len = 0;
    	
    	foreach (le, stmt->args)
    	{
    	    char *ar = (char *) lfirst (le);
    	    len += strlen (ar) + 4;
    	}
    	args = (char *) palloc (len + 1);
    	args[0] = 0;
    	foreach (le, stmt->args)
    	    sprintf (args + strlen (args), "%s\\000", (char *)lfirst (le));
    	values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum (nargs);
    	values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum (byteain (args));
    }
    else
    {
    	values[Anum_pg_trigger_tgnargs - 1] = Int16GetDatum (0);
    	values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum (byteain (""));
    }
    values[Anum_pg_trigger_tgattr - 1] = PointerGetDatum (tgattr);
    
    tuple = heap_formtuple (tgrel->rd_att, values, nulls);
    heap_insert (tgrel, tuple);
    CatalogOpenIndices (Num_pg_trigger_indices, Name_pg_trigger_indices, idescs);
    CatalogIndexInsert (idescs, Num_pg_trigger_indices, tgrel, tuple);
    CatalogCloseIndices (Num_pg_trigger_indices, idescs);
    pfree (tuple);
    RelationUnsetLockForWrite (tgrel);
    heap_close (tgrel);
    
    pfree (DatumGetPointer(values[Anum_pg_trigger_tgname - 1]));
    pfree (DatumGetPointer(values[Anum_pg_trigger_tgargs - 1]));
    
    /* update pg_class */
    relrdesc = heap_openr (RelationRelationName);
    tuple = ClassNameIndexScan (relrdesc, stmt->relname);
    if ( !PointerIsValid (tuple) )
    {
	heap_close(relrdesc);
	elog(WARN, "CreateTrigger: relation %s not found in pg_class", stmt->relname);
    }
    ((Form_pg_class) GETSTRUCT(tuple))->reltriggers = found + 1;
    RelationInvalidateHeapTuple (relrdesc, tuple);
    oldTID = tuple->t_ctid;
    heap_replace (relrdesc, &oldTID, tuple);
    CatalogOpenIndices (Num_pg_class_indices, Name_pg_class_indices, ridescs);
    CatalogIndexInsert (ridescs, Num_pg_class_indices, relrdesc, tuple);
    CatalogCloseIndices (Num_pg_class_indices, ridescs);
    pfree(tuple);
    heap_close(relrdesc);
    
    CommandCounterIncrement ();
    oldcxt = MemoryContextSwitchTo ((MemoryContext)CacheCxt);
    FreeTriggerDesc (rel);
    rel->rd_rel->reltriggers = found + 1;
    RelationBuildTriggers (rel);
    MemoryContextSwitchTo (oldcxt);
    heap_close (rel);
    return;
}

void
DropTrigger (DropTrigStmt *stmt)
{
    Relation rel;
    Relation tgrel;
    HeapScanDesc tgscan;
    ScanKeyData	key;
    Relation relrdesc;
    HeapTuple tuple;
    ItemPointerData oldTID;
    Relation ridescs[Num_pg_class_indices];
    MemoryContext oldcxt;
    int found = 0;
    int tgfound = 0;
    
#ifndef NO_SECURITY
    if ( !pg_ownercheck (GetPgUserName (), stmt->relname, RELNAME))
    	elog(WARN, "%s: %s", stmt->relname, aclcheck_error_strings[ACLCHECK_NOT_OWNER]);
#endif
    
    rel = heap_openr (stmt->relname);
    if ( !RelationIsValid (rel) )
    	elog (WARN, "DropTrigger: there is no relation %s", stmt->relname);
    
    RelationSetLockForWrite (rel);
    
    tgrel = heap_openr (TriggerRelationName);
    RelationSetLockForWrite (tgrel);
    ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgrelid,
			   ObjectIdEqualRegProcedure, rel->rd_id);
    tgscan = heap_beginscan (tgrel, 0, NowTimeQual, 1, &key);
    while (tuple = heap_getnext (tgscan, 0, (Buffer *)NULL), PointerIsValid(tuple))
    {
    	Form_pg_trigger pg_trigger = (Form_pg_trigger) GETSTRUCT (tuple);
    	if ( namestrcmp (&(pg_trigger->tgname), stmt->trigname) == 0 )
    	{
	    heap_delete (tgrel, &tuple->t_ctid);
	    tgfound++;
    	}
    	else
    	    found++;
    }
    if ( tgfound == 0 )
    	elog (WARN, "DropTrigger: there is no trigger %s on relation %s", 
    	    			stmt->trigname, stmt->relname);
    if ( tgfound > 1 )
    	elog (NOTICE, "DropTrigger: found (and deleted) %d trigger %s on relation %s",
    	    			tgfound, stmt->trigname, stmt->relname);
    heap_endscan (tgscan);
    RelationUnsetLockForWrite (tgrel);
    heap_close (tgrel);
    
    /* update pg_class */
    relrdesc = heap_openr (RelationRelationName);
    tuple = ClassNameIndexScan (relrdesc, stmt->relname);
    if ( !PointerIsValid (tuple) )
    {
	heap_close(relrdesc);
	elog(WARN, "DropTrigger: relation %s not found in pg_class", stmt->relname);
    }
    ((Form_pg_class) GETSTRUCT(tuple))->reltriggers = found;
    RelationInvalidateHeapTuple (relrdesc, tuple);
    oldTID = tuple->t_ctid;
    heap_replace (relrdesc, &oldTID, tuple);
    CatalogOpenIndices (Num_pg_class_indices, Name_pg_class_indices, ridescs);
    CatalogIndexInsert (ridescs, Num_pg_class_indices, relrdesc, tuple);
    CatalogCloseIndices (Num_pg_class_indices, ridescs);
    pfree(tuple);
    heap_close(relrdesc);
    
    CommandCounterIncrement ();
    oldcxt = MemoryContextSwitchTo ((MemoryContext)CacheCxt);
    FreeTriggerDesc (rel);
    rel->rd_rel->reltriggers = found;
    if ( found > 0 )
    	RelationBuildTriggers (rel);
    MemoryContextSwitchTo (oldcxt);
    heap_close (rel);
    return;
}

void 
RelationRemoveTriggers (Relation rel)
{
    Relation		tgrel;
    HeapScanDesc	tgscan;
    ScanKeyData	        key;
    HeapTuple		tup;
    
    tgrel = heap_openr (TriggerRelationName);
    RelationSetLockForWrite (tgrel);
    ScanKeyEntryInitialize(&key, 0, Anum_pg_trigger_tgrelid,
			   ObjectIdEqualRegProcedure, rel->rd_id);
    
    tgscan = heap_beginscan (tgrel, 0, NowTimeQual, 1, &key);
    
    while (tup = heap_getnext (tgscan, 0, (Buffer *)NULL), PointerIsValid(tup))
	heap_delete (tgrel, &tup->t_ctid);
    
    heap_endscan (tgscan);
    RelationUnsetLockForWrite (tgrel);
    heap_close (tgrel);

}

void
RelationBuildTriggers (Relation relation)
{
    TriggerDesc *trigdesc = (TriggerDesc *) palloc (sizeof (TriggerDesc));
    int ntrigs = relation->rd_rel->reltriggers;
    Trigger *triggers = NULL;
    Trigger *build;
    Relation tgrel;
    Form_pg_trigger pg_trigger;
    Relation irel;
    ScanKeyData skey;
    HeapTuple tuple;
    IndexScanDesc sd;
    RetrieveIndexResult indexRes;
    Buffer buffer;
    ItemPointer iptr;
    struct varlena *val;
    bool isnull;
    int found;
    
    memset (trigdesc, 0, sizeof (TriggerDesc));
    
    ScanKeyEntryInitialize(&skey,
			   (bits16)0x0,
			   (AttrNumber)1,
			   (RegProcedure)ObjectIdEqualRegProcedure,
			   ObjectIdGetDatum(relation->rd_id));
    
    tgrel = heap_openr(TriggerRelationName);
    RelationSetLockForRead (tgrel);
    irel = index_openr(TriggerRelidIndex);
    sd = index_beginscan(irel, false, 1, &skey);
    
    for (found = 0; ; )
    {
	indexRes = index_getnext(sd, ForwardScanDirection);
	if (!indexRes)
	    break;
	    
	iptr = &indexRes->heap_iptr;
	tuple = heap_fetch(tgrel, NowTimeQual, iptr, &buffer);
	pfree(indexRes);
    	if (!HeapTupleIsValid(tuple))
    	    continue;
    	if ( found == ntrigs )
    	    elog (WARN, "RelationBuildTriggers: unexpected record found for rel %.*s",
    	    	    	NAMEDATALEN, relation->rd_rel->relname.data);
    	
    	pg_trigger = (Form_pg_trigger) GETSTRUCT (tuple);
    	
    	if ( triggers == NULL )
    	    triggers = (Trigger *) palloc (sizeof (Trigger));
    	else
    	    triggers = (Trigger *) repalloc (triggers, (found + 1) * sizeof (Trigger));
    	build = &(triggers[found]);
    	
    	build->tgname = nameout (&(pg_trigger->tgname));
    	build->tgfoid = pg_trigger->tgfoid;
    	build->tgfunc = NULL;
    	build->tgtype = pg_trigger->tgtype;
    	build->tgnargs = pg_trigger->tgnargs;
    	memcpy (build->tgattr, &(pg_trigger->tgattr), 8 * sizeof (int16));
    	val = (struct varlena*) fastgetattr (tuple, 
    	    					Anum_pg_trigger_tgargs,
    	    					tgrel->rd_att, &isnull);
    	if ( isnull )
    	    elog (WARN, "RelationBuildTriggers: tgargs IS NULL for rel %.*s",
    	    	    NAMEDATALEN, relation->rd_rel->relname.data);
    	if (  build->tgnargs > 0 )
    	{
    	    char *p;
    	    int i;
    	    
    	    val = (struct varlena*) fastgetattr (tuple, 
    	    					Anum_pg_trigger_tgargs,
    	    					tgrel->rd_att, &isnull);
    	    if ( isnull )
    	    	elog (WARN, "RelationBuildTriggers: tgargs IS NULL for rel %.*s",
    	    	    NAMEDATALEN, relation->rd_rel->relname.data);
    	    p = (char *) VARDATA (val);
    	    build->tgargs = (char**) palloc (build->tgnargs * sizeof (char*));
    	    for (i = 0; i < build->tgnargs; i++)
    	    {
    	    	build->tgargs[i] = (char*) palloc (strlen (p) + 1);
    	    	strcpy (build->tgargs[i], p);
    	    	p += strlen (p) + 1;
    	    }
    	}
    	else
    	    build->tgargs =  NULL;
    	
    	found++;
	ReleaseBuffer(buffer);
    }
    
    if ( found < ntrigs )
    	elog (WARN, "RelationBuildTriggers: %d record not found for rel %.*s",
    	    	    	ntrigs - found,
    	    	    	NAMEDATALEN, relation->rd_rel->relname.data);
    
    index_endscan (sd);
    pfree (sd);
    index_close (irel);
    RelationUnsetLockForRead (tgrel);
    heap_close (tgrel);
    
    /* Build trigdesc */
    trigdesc->triggers = triggers;
    for (found = 0; found < ntrigs; found++)
    {
    	build = &(triggers[found]);
    	DescribeTrigger (trigdesc, build);
    }
    
    relation->trigdesc = trigdesc;
    
}

void 
FreeTriggerDesc (Relation relation)
{
    TriggerDesc *trigdesc = relation->trigdesc;
    Trigger ***t;
    Trigger *trigger;
    int i;
    
    if ( trigdesc == NULL )
    	return;
    
    t = trigdesc->tg_before_statement;
    for (i = 0; i < 3; i++)
    	if ( t[i] != NULL )
    	    pfree (t[i]);
    t = trigdesc->tg_before_row;
    for (i = 0; i < 3; i++)
    	if ( t[i] != NULL )
    	    pfree (t[i]);
    t = trigdesc->tg_after_row;
    for (i = 0; i < 3; i++)
    	if ( t[i] != NULL )
    	    pfree (t[i]);
    t = trigdesc->tg_after_statement;
    for (i = 0; i < 3; i++)
    	if ( t[i] != NULL )
    	    pfree (t[i]);
    
    trigger = trigdesc->triggers;
    for (i = 0; i < relation->rd_rel->reltriggers; i++)
    {
    	pfree (trigger->tgname);
    	if ( trigger->tgnargs > 0 )
    	{
    	    while ( --(trigger->tgnargs) >= 0 )
    	    	pfree (trigger->tgargs[trigger->tgnargs]);
    	    pfree (trigger->tgargs);
    	}
    	trigger++;
    }
    pfree (trigdesc->triggers);
    pfree (trigdesc);
    relation->trigdesc = NULL;
    return;
}

static void
DescribeTrigger (TriggerDesc *trigdesc, Trigger *trigger)
{
    uint16 *n;
    Trigger ***t, ***tp;
    	
    if ( TRIGGER_FOR_ROW (trigger->tgtype) )	/* Is ROW/STATEMENT trigger */
    {
    	if ( TRIGGER_FOR_BEFORE (trigger->tgtype) )
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
    else					/* STATEMENT (NI) */
    {
    	if ( TRIGGER_FOR_BEFORE (trigger->tgtype) )
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
    
    if ( TRIGGER_FOR_INSERT (trigger->tgtype) )
    {
    	tp = &(t[TRIGGER_EVENT_INSERT]);
    	if ( *tp == NULL )
    	    *tp = (Trigger **) palloc (sizeof (Trigger *));
    	else
    	    *tp = (Trigger **) repalloc (*tp, (n[TRIGGER_EVENT_INSERT] + 1) * 
    	    					sizeof (Trigger *));
    	(*tp)[n[TRIGGER_EVENT_INSERT]] = trigger;
    	(n[TRIGGER_EVENT_INSERT])++;
    }
    
    if ( TRIGGER_FOR_DELETE (trigger->tgtype) )
    {
    	tp = &(t[TRIGGER_EVENT_DELETE]);
    	if ( *tp == NULL )
    	    *tp = (Trigger **) palloc (sizeof (Trigger *));
    	else
    	    *tp = (Trigger **) repalloc (*tp, (n[TRIGGER_EVENT_DELETE] + 1) * 
    	    					sizeof (Trigger *));
    	(*tp)[n[TRIGGER_EVENT_DELETE]] = trigger;
    	(n[TRIGGER_EVENT_DELETE])++;
    }
    
    if ( TRIGGER_FOR_UPDATE (trigger->tgtype) )
    {
    	tp = &(t[TRIGGER_EVENT_UPDATE]);
    	if ( *tp == NULL )
    	    *tp = (Trigger **) palloc (sizeof (Trigger *));
    	else
    	    *tp = (Trigger **) repalloc (*tp, (n[TRIGGER_EVENT_UPDATE] + 1) * 
    	    					sizeof (Trigger *));
    	(*tp)[n[TRIGGER_EVENT_UPDATE]] = trigger;
    	(n[TRIGGER_EVENT_UPDATE])++;
    }
    
}

HeapTuple 
ExecBRInsertTriggers (Relation rel, HeapTuple tuple)
{
    int ntrigs = rel->trigdesc->n_before_row[TRIGGER_EVENT_INSERT];
    Trigger **trigger = rel->trigdesc->tg_before_row[TRIGGER_EVENT_INSERT];
    HeapTuple newtuple = tuple;
    int nargs;
    int i;
    
    CurrentTriggerData = (TriggerData *) palloc (sizeof (TriggerData));
    CurrentTriggerData->tg_event = TRIGGER_EVENT_INSERT|TRIGGER_EVENT_ROW;
    CurrentTriggerData->tg_relation = rel;
    CurrentTriggerData->tg_newtuple = NULL;
    for (i = 0; i < ntrigs; i++)
    {
    	CurrentTriggerData->tg_trigtuple = newtuple;
    	CurrentTriggerData->tg_trigger = trigger[i];
    	if ( trigger[i]->tgfunc == NULL )
    	    fmgr_info (trigger[i]->tgfoid, &(trigger[i]->tgfunc), &nargs);
    	newtuple = (HeapTuple) ( (*(trigger[i]->tgfunc)) () );
    	if ( newtuple == NULL )
    	    break;
    }
    pfree (CurrentTriggerData);
    CurrentTriggerData = NULL;
    return (newtuple);
}

void
ExecARInsertTriggers (Relation rel, HeapTuple tuple)
{
    
    return;
}

bool
ExecBRDeleteTriggers (Relation rel, ItemPointer tupleid)
{
    
    return (true);
}

void
ExecARDeleteTriggers (Relation rel, ItemPointer tupleid)
{
    
    return;
}

HeapTuple
ExecBRUpdateTriggers (Relation rel, ItemPointer tupleid, HeapTuple tuple)
{
    
    return (tuple);
}

void
ExecARUpdateTriggers (Relation rel, ItemPointer tupleid, HeapTuple tuple)
{
    
    return;
}
