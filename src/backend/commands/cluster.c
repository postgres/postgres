/*-------------------------------------------------------------------------
 *
 * cluster.c--
 *    Paul Brown's implementation of cluster index. 
 *
 *    I am going to use the rename function as a model for this in the
 *    parser and executor, and the vacuum code as an example in this
 *    file. As I go - in contrast to the rest of postgres - there will
 *    be BUCKETS of comments. This is to allow reviewers to understand
 *    my (probably bogus) assumptions about the way this works.
 *							[pbrown '94]
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/cluster.c,v 1.6 1996/09/26 16:29:05 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>
#include <stdio.h>

#include "postgres.h"

#include "nodes/pg_list.h"

#include "access/attnum.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "access/htup.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/xact.h"
#include "utils/tqual.h"

#include "catalog/catname.h"
#include "utils/syscache.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"

#include "commands/copy.h"
#include "commands/cluster.h"
#include "commands/rename.h"

#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"

#include "miscadmin.h"
#include "tcop/dest.h"
#include "commands/command.h"

#include "utils/builtins.h"
#include "utils/excid.h"
#include "utils/elog.h"
#include "utils/mcxt.h"
#include "utils/palloc.h"
#include "utils/rel.h"

#include "catalog/pg_attribute.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_class.h"

#include "optimizer/internal.h"

#ifndef NO_SECURITY
#include "utils/acl.h"
#include "utils/syscache.h"
#endif /* !NO_SECURITY */

/*
 * cluster
 *
 *   Check that the relation is a relation in the appropriate user
 *   ACL. I will use the same security that limits users on the
 *   renamerel() function.
 *
 *   Check that the index specified is appropriate for the task
 *   ( ie it's an index over this relation ). This is trickier.
 *
 *   Create a list of all the other indicies on this relation. Because
 *   the cluster will wreck all the tids, I'll need to destroy bogus
 *   indicies. The user will have to re-create them. Not nice, but
 *   I'm not a nice guy. The alternative is to try some kind of post
 *   destroy re-build. This may be possible. I'll check out what the
 *   index create functiond want in the way of paramaters. On the other
 *   hand, re-creating n indicies may blow out the space. 
 *
 *   Create new (temporary) relations for the base heap and the new 
 *   index. 
 *  
 *   Exclusively lock the relations.
 * 
 *   Create new clustered index and base heap relation.
 *
 */
void
cluster(char oldrelname[], char oldindexname[])
{
    Oid OIDOldHeap, OIDOldIndex, OIDNewHeap;
    
    Relation OldHeap, OldIndex;
    Relation NewHeap;
    
    char NewIndexName[NAMEDATALEN+1];
    char NewHeapName[NAMEDATALEN+1];
    char saveoldrelname[NAMEDATALEN+1];
    char saveoldindexname[NAMEDATALEN+1];


    /* Save the old names because they will get lost when the old relations
     * are destroyed.
     */
    strcpy(saveoldrelname, oldrelname);
    strcpy(saveoldindexname, oldindexname);
   
    /*
     *
     * I'm going to force all checking back into the commands.c function.
     *
     * Get the list if indicies for this relation. If the index we want
     * is among them, do not add it to the 'kill' list, as it will be
     * handled by the 'clean up' code which commits this transaction.
     *
     * I'm not using the SysCache, because this will happen but
     * once, and the slow way is the sure way in this case.
     *
     */
    /*
     * Like vacuum, cluster spans transactions, so I'm going to handle it in
     * the same way.
     */
    
    /* matches the StartTransaction in PostgresMain() */
    
    OldHeap = heap_openr(oldrelname);
    if (!RelationIsValid(OldHeap)) {
	elog(WARN, "cluster: unknown relation: \"%-.*s\"",
	     NAMEDATALEN, oldrelname);
    }
    OIDOldHeap = OldHeap->rd_id; /* Get OID for the index scan   */
    
    OldIndex=index_openr(oldindexname);/* Open old index relation  */
    if (!RelationIsValid(OldIndex)) {
	elog(WARN, "cluster: unknown index: \"%-.*s\"",
	     NAMEDATALEN, oldindexname);
    }
    OIDOldIndex = OldIndex->rd_id;     /* OID for the index scan         */
    
    heap_close(OldHeap);
    index_close(OldIndex);
    
    /*
     * I need to build the copies of the heap and the index. The Commit()
     * between here is *very* bogus. If someone is appending stuff, they will
     * get the lock after being blocked and add rows which won't be present in
     * the new table. Bleagh! I'd be best to try and ensure that no-one's
     * in the tables for the entire duration of this process with a pg_vlock.
     */
    NewHeap    = copy_heap(OIDOldHeap);
    OIDNewHeap = NewHeap->rd_id;
    strcpy(NewHeapName,NewHeap->rd_rel->relname.data);


    /* To make the new heap visible (which is until now empty). */
    CommandCounterIncrement();
    
    rebuildheap(OIDNewHeap, OIDOldHeap, OIDOldIndex);
    
    /* To flush the filled new heap (and the statistics about it). */
    CommandCounterIncrement();

    /* Create new index over the tuples of the new heap. */
    copy_index(OIDOldIndex, OIDNewHeap);
    sprintf(NewIndexName, "temp_%x", OIDOldIndex);
    
    /* 
     * make this really happen. Flush all the buffers.
     * (Believe me, it is necessary ... ended up in a mess without it.)
     */
    CommitTransactionCommand();
    StartTransactionCommand();


    /* Destroy old heap (along with its index) and rename new. */
    heap_destroy(oldrelname);    
    
    renamerel(NewHeapName, saveoldrelname);
    TypeRename(NewHeapName, saveoldrelname);

    renamerel(NewIndexName, saveoldindexname);

    /* 
     * Again flush all the buffers.
     */
    CommitTransactionCommand();
    StartTransactionCommand();
}

Relation
copy_heap(Oid OIDOldHeap)
{
    char NewName[NAMEDATALEN];
    TupleDesc OldHeapDesc, tupdesc;
    Oid OIDNewHeap;
    Relation NewHeap, OldHeap;

    /*
     *  Create a new heap relation with a temporary name, which has the
     *  same tuple description as the old one.
     */
    sprintf(NewName,"temp_%x", OIDOldHeap);

    OldHeap= heap_open(OIDOldHeap);
    OldHeapDesc= RelationGetTupleDescriptor(OldHeap);

    /*
     * Need to make a copy of the tuple descriptor, heap_create modifies
     * it.
     */

    tupdesc = CreateTupleDescCopy(OldHeapDesc);
    
    OIDNewHeap=heap_create(NewName,
			   NULL,
			   OldHeap->rd_rel->relarch,
			   OldHeap->rd_rel->relsmgr,
			   tupdesc);

    if (!OidIsValid(OIDNewHeap))
	elog(WARN,"clusterheap: cannot create temporary heap relation\n");

    NewHeap=heap_open(OIDNewHeap);

    heap_close(NewHeap);
    heap_close(OldHeap);

    return NewHeap;
}

void
copy_index(Oid OIDOldIndex, Oid OIDNewHeap)
{
    Relation OldIndex, NewHeap;
    HeapTuple Old_pg_index_Tuple, Old_pg_index_relation_Tuple, pg_proc_Tuple;
    IndexTupleForm        Old_pg_index_Form;
    Form_pg_class	  Old_pg_index_relation_Form;
    Form_pg_proc          pg_proc_Form;
    char                 *NewIndexName;
    AttrNumber 		 *attnumP;
    int                   natts;
    FuncIndexInfo *       finfo;

    NewHeap  = heap_open(OIDNewHeap);
    OldIndex = index_open(OIDOldIndex);

    /*
     * OK. Create a new (temporary) index for the one that's already
     * here. To do this I get the info from pg_index, re-build the
     * FunctInfo if I have to, and add a new index with a temporary
     * name.
     */
    Old_pg_index_Tuple =
	SearchSysCacheTuple(INDEXRELID,
			    ObjectIdGetDatum(OldIndex->rd_id),
			    0,0,0);

    Assert(Old_pg_index_Tuple);
    Old_pg_index_Form = (IndexTupleForm)GETSTRUCT(Old_pg_index_Tuple);

    Old_pg_index_relation_Tuple =
	SearchSysCacheTuple(RELOID,
			    ObjectIdGetDatum(OldIndex->rd_id),
			    0,0,0);

    Assert(Old_pg_index_relation_Tuple);
    Old_pg_index_relation_Form =
	(Form_pg_class)GETSTRUCT(Old_pg_index_relation_Tuple);

     NewIndexName = palloc(NAMEDATALEN+1);  /* XXX */
     sprintf(NewIndexName, "temp_%x", OIDOldIndex); /* Set the name. */

    /*
     * Ugly as it is, the only way I have of working out the number of
     * attribues is to count them. Mostly there'll be just one but 
     * I've got to be sure.
     */
    for (attnumP = &(Old_pg_index_Form->indkey[0]), natts = 0;
	 *attnumP != InvalidAttrNumber;
	 attnumP++, natts++);

    /*
     * If this is a functional index, I need to rebuild the functional
     * component to pass it to the defining procedure.
     */
    if (Old_pg_index_Form->indproc != InvalidOid) {
        finfo = (FuncIndexInfo *) palloc(sizeof(FuncIndexInfo));
	FIgetnArgs(finfo) = natts;
	FIgetProcOid(finfo) = Old_pg_index_Form->indproc;

	pg_proc_Tuple =
	    SearchSysCacheTuple(PROOID,
				ObjectIdGetDatum(Old_pg_index_Form->indproc),
				0,0,0);

	Assert(pg_proc_Tuple);
	pg_proc_Form = (Form_pg_proc)GETSTRUCT(pg_proc_Tuple);
	namecpy(&(finfo->funcName), &(pg_proc_Form->proname));
    } else {
	finfo = (FuncIndexInfo *) NULL;
	natts = 1;
    }

    index_create((NewHeap->rd_rel->relname).data,
		 NewIndexName, 
		 finfo,
  		 NULL, /* type info is in the old index */
		 Old_pg_index_relation_Form->relam,
		 natts, 
		 Old_pg_index_Form->indkey,
		 Old_pg_index_Form->indclass,
		 (uint16)0, (Datum) NULL, NULL, Old_pg_index_Form->indislossy);

    heap_close(OldIndex);
    heap_close(NewHeap);
}


void
rebuildheap(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex)
{
    Relation              LocalNewHeap, LocalOldHeap, LocalOldIndex;
    IndexScanDesc         ScanDesc;
    RetrieveIndexResult   ScanResult;
    ItemPointer           HeapTid;
    HeapTuple             LocalHeapTuple;
    Buffer                LocalBuffer;
    Oid              	  OIDNewHeapInsert;

    /*
     * Open the relations I need. Scan through the OldHeap on the OldIndex and
     * insert each tuple into the NewHeap.
     */
    LocalNewHeap=(Relation)heap_open(OIDNewHeap);
    LocalOldHeap=(Relation)heap_open(OIDOldHeap);
    LocalOldIndex=(Relation)index_open(OIDOldIndex);

    ScanDesc=index_beginscan(LocalOldIndex, false, 0, (ScanKey) NULL);

    while ((ScanResult =
	    index_getnext(ScanDesc, ForwardScanDirection)) != NULL) {

	HeapTid = &ScanResult->heap_iptr;
	LocalHeapTuple = heap_fetch(LocalOldHeap, 0, HeapTid, &LocalBuffer);
	OIDNewHeapInsert =
	    heap_insert(LocalNewHeap, LocalHeapTuple);
	pfree(ScanResult);
	ReleaseBuffer(LocalBuffer);
    }
    index_endscan(ScanDesc);

    index_close(LocalOldIndex);
    heap_close(LocalOldHeap);
    heap_close(LocalNewHeap);
}

