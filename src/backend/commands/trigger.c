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
#include "commands/trigger.h"
#include "catalog/catname.h"
#include "catalog/indexing.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_language.h"
#include "catalog/pg_trigger.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"

void RelationBuildTriggers (Relation relation);
void FreeTriggerDesc (Relation relation);

void
CreateTrigger (CreateTrigStmt *stmt)
{

    return;
}

void
DropTrigger (DropTrigStmt *stmt)
{

    return;
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
    	build->tgfunc = nameout (&(pg_trigger->tgfunc));
    	build->tglang = pg_trigger->tglang;
    	if ( build->tglang != ClanguageId )
    	    elog (WARN, "RelationBuildTriggers: unsupported language %u for trigger %s of rel %.*s",
    	    	build->tglang, build->tgname, NAMEDATALEN, relation->rd_rel->relname.data);
    	build->tgtype = pg_trigger->tgtype;
    	build->tgnargs = pg_trigger->tgnargs;
    	memcpy (build->tgattr, &(pg_trigger->tgattr), 8 * sizeof (int16));
    	val = (struct varlena*) fastgetattr (tuple, 
    	    					Anum_pg_trigger_tgtext,
    	    					tgrel->rd_att, &isnull);
    	if ( isnull )
    	    elog (WARN, "RelationBuildTriggers: tgtext IS NULL for rel %.*s",
    	    	    NAMEDATALEN, relation->rd_rel->relname.data);
    	build->tgtext = byteaout (val);
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
    	val = (struct varlena*) fastgetattr (tuple, 
    	    					Anum_pg_trigger_tgwhen,
    	    					tgrel->rd_att, &isnull);
    	if ( !isnull )
    	    build->tgwhen = textout (val);
    	else
    	    build->tgwhen = NULL;
    	
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
    heap_close (tgrel);
    
    /* Build trigdesc */
    trigdesc->triggers = triggers;
    for (found = 0; found < ntrigs; found++)
    {
    	uint16 *n;
    	Trigger ***t, ***tp;
    	
    	build = &(triggers[found]);
    	
    	if ( TRIGGER_FOR_ROW (build->tgtype) )	/* Is ROW/STATEMENT trigger */
    	{
    	    if ( TRIGGER_FOR_BEFORE (build->tgtype) )
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
    	    if ( TRIGGER_FOR_BEFORE (build->tgtype) )
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
    	
    	if ( TRIGGER_FOR_INSERT (build->tgtype) )
    	{
    	    tp = &(t[TRIGGER_ACTION_INSERT]);
    	    if ( *tp == NULL )
    	    	*tp = (Trigger **) palloc (sizeof (Trigger *));
    	    else
    	    	*tp = (Trigger **) repalloc (*tp, (n[TRIGGER_ACTION_INSERT] + 1) * 
    	    					sizeof (Trigger *));
    	    (*tp)[n[TRIGGER_ACTION_INSERT]] = build;
    	    (n[TRIGGER_ACTION_INSERT])++;
    	}
    	
    	if ( TRIGGER_FOR_DELETE (build->tgtype) )
    	{
    	    tp = &(t[TRIGGER_ACTION_DELETE]);
    	    if ( *tp == NULL )
    	    	*tp = (Trigger **) palloc (sizeof (Trigger *));
    	    else
    	    	*tp = (Trigger **) repalloc (*tp, (n[TRIGGER_ACTION_DELETE] + 1) * 
    	    					sizeof (Trigger *));
    	    (*tp)[n[TRIGGER_ACTION_DELETE]] = build;
    	    (n[TRIGGER_ACTION_DELETE])++;
    	}
    	
    	if ( TRIGGER_FOR_UPDATE (build->tgtype) )
    	{
    	    tp = &(t[TRIGGER_ACTION_UPDATE]);
    	    if ( *tp == NULL )
    	    	*tp = (Trigger **) palloc (sizeof (Trigger *));
    	    else
    	    	*tp = (Trigger **) repalloc (*tp, (n[TRIGGER_ACTION_UPDATE] + 1) * 
    	    					sizeof (Trigger *));
    	    (*tp)[n[TRIGGER_ACTION_UPDATE]] = build;
    	    (n[TRIGGER_ACTION_UPDATE])++;
    	}
    }
    	
    relation->trigdesc = trigdesc;
    
}

void 
FreeTriggerDesc (Relation relation)
{
    
    return;
}

HeapTuple 
ExecBRInsertTriggers (Relation rel, HeapTuple tuple)
{
    
    return (tuple);
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
