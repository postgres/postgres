/*-------------------------------------------------------------------------
 *
 * cluster.c
 *	  Paul Brown's implementation of cluster index.
 *
 *	  I am going to use the rename function as a model for this in the
 *	  parser and executor, and the vacuum code as an example in this
 *	  file. As I go - in contrast to the rest of postgres - there will
 *	  be BUCKETS of comments. This is to allow reviewers to understand
 *	  my (probably bogus) assumptions about the way this works.
 *														[pbrown '94]
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/cluster.c,v 1.38 1999/02/13 23:15:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include <postgres.h>

#include <catalog/pg_index.h>
#include <catalog/heap.h>
#include <access/heapam.h>
#include <access/genam.h>
#include <access/xact.h>
#include <catalog/catname.h>
#include <utils/syscache.h>
#include <catalog/index.h>
#include <catalog/indexing.h>
#include <catalog/pg_type.h>
#include <commands/copy.h>
#include <commands/cluster.h>
#include <commands/rename.h>
#include <storage/bufmgr.h>
#include <miscadmin.h>
#include <tcop/dest.h>
#include <commands/command.h>
#include <utils/builtins.h>
#include <utils/excid.h>
#include <utils/mcxt.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_class.h>
#include <optimizer/internal.h>
#ifndef NO_SECURITY
#include <utils/acl.h>
#endif	 /* !NO_SECURITY */

static Relation copy_heap(Oid OIDOldHeap);
static void copy_index(Oid OIDOldIndex, Oid OIDNewHeap);
static void rebuildheap(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex);

/*
 * cluster
 *
 *	 Check that the relation is a relation in the appropriate user
 *	 ACL. I will use the same security that limits users on the
 *	 renamerel() function.
 *
 *	 Check that the index specified is appropriate for the task
 *	 ( ie it's an index over this relation ). This is trickier.
 *
 *	 Create a list of all the other indicies on this relation. Because
 *	 the cluster will wreck all the tids, I'll need to destroy bogus
 *	 indicies. The user will have to re-create them. Not nice, but
 *	 I'm not a nice guy. The alternative is to try some kind of post
 *	 destroy re-build. This may be possible. I'll check out what the
 *	 index create functiond want in the way of paramaters. On the other
 *	 hand, re-creating n indicies may blow out the space.
 *
 *	 Create new (temporary) relations for the base heap and the new
 *	 index.
 *
 *	 Exclusively lock the relations.
 *
 *	 Create new clustered index and base heap relation.
 *
 */
void
cluster(char *oldrelname, char *oldindexname)
{
	Oid			OIDOldHeap,
				OIDOldIndex,
				OIDNewHeap;

	Relation	OldHeap,
				OldIndex;
	Relation	NewHeap;

	char		NewIndexName[NAMEDATALEN];
	char		NewHeapName[NAMEDATALEN];
	char		saveoldrelname[NAMEDATALEN];
	char		saveoldindexname[NAMEDATALEN];


	/*
	 * Save the old names because they will get lost when the old
	 * relations are destroyed.
	 */
	strcpy(saveoldrelname, oldrelname);
	strcpy(saveoldindexname, oldindexname);

	/*
	 * I'm going to force all checking back into the commands.c function.
	 *
	 * Get the list if indicies for this relation. If the index we want is
	 * among them, do not add it to the 'kill' list, as it will be handled
	 * by the 'clean up' code which commits this transaction.
	 *
	 * I'm not using the SysCache, because this will happen but once, and the
	 * slow way is the sure way in this case.
	 *
	 */

	/*
	 * Like vacuum, cluster spans transactions, so I'm going to handle it
	 * in the same way.
	 */

	/* matches the StartTransaction in PostgresMain() */

	OldHeap = heap_openr(oldrelname);
	if (!RelationIsValid(OldHeap))
	{
		elog(ERROR, "cluster: unknown relation: \"%s\"",
			 oldrelname);
	}
	OIDOldHeap = RelationGetRelid(OldHeap);		/* Get OID for the index
												 * scan    */

	OldIndex = index_openr(oldindexname);		/* Open old index relation	*/
	if (!RelationIsValid(OldIndex))
	{
		elog(ERROR, "cluster: unknown index: \"%s\"",
			 oldindexname);
	}
	OIDOldIndex = RelationGetRelid(OldIndex);	/* OID for the index scan		  */

	heap_close(OldHeap);
	index_close(OldIndex);

	/*
	 * I need to build the copies of the heap and the index. The Commit()
	 * between here is *very* bogus. If someone is appending stuff, they
	 * will get the lock after being blocked and add rows which won't be
	 * present in the new table. Bleagh! I'd be best to try and ensure
	 * that no-one's in the tables for the entire duration of this process
	 * with a pg_vlock.
	 */
	NewHeap = copy_heap(OIDOldHeap);
	OIDNewHeap = RelationGetRelid(NewHeap);
	strcpy(NewHeapName, NewHeap->rd_rel->relname.data);


	/* To make the new heap visible (which is until now empty). */
	CommandCounterIncrement();

	rebuildheap(OIDNewHeap, OIDOldHeap, OIDOldIndex);

	/* To flush the filled new heap (and the statistics about it). */
	CommandCounterIncrement();

	/* Create new index over the tuples of the new heap. */
	copy_index(OIDOldIndex, OIDNewHeap);
	snprintf(NewIndexName, NAMEDATALEN, "temp_%x", OIDOldIndex);

	/*
	 * make this really happen. Flush all the buffers. (Believe me, it is
	 * necessary ... ended up in a mess without it.)
	 */
	CommitTransactionCommand();
	StartTransactionCommand();


	/* Destroy old heap (along with its index) and rename new. */
	heap_destroy_with_catalog(oldrelname);

	CommitTransactionCommand();
	StartTransactionCommand();

	renamerel(NewHeapName, saveoldrelname);
	TypeRename(NewHeapName, saveoldrelname);

	renamerel(NewIndexName, saveoldindexname);

	/*
	 * Again flush all the buffers.
	 */
	CommitTransactionCommand();
	StartTransactionCommand();
}

static Relation
copy_heap(Oid OIDOldHeap)
{
	char		NewName[NAMEDATALEN];
	TupleDesc	OldHeapDesc,
				tupdesc;
	Oid			OIDNewHeap;
	Relation	NewHeap,
				OldHeap;

	/*
	 * Create a new heap relation with a temporary name, which has the
	 * same tuple description as the old one.
	 */
	snprintf(NewName, NAMEDATALEN, "temp_%x", OIDOldHeap);

	OldHeap = heap_open(OIDOldHeap);
	OldHeapDesc = RelationGetDescr(OldHeap);

	/*
	 * Need to make a copy of the tuple descriptor,
	 * heap_create_with_catalog modifies it.
	 */

	tupdesc = CreateTupleDescCopy(OldHeapDesc);

	OIDNewHeap = heap_create_with_catalog(NewName, tupdesc,
										  RELKIND_RELATION, false);

	if (!OidIsValid(OIDNewHeap))
		elog(ERROR, "clusterheap: cannot create temporary heap relation\n");

	NewHeap = heap_open(OIDNewHeap);

	heap_close(NewHeap);
	heap_close(OldHeap);

	return NewHeap;
}

static void
copy_index(Oid OIDOldIndex, Oid OIDNewHeap)
{
	Relation			OldIndex,
								NewHeap;
	HeapTuple			Old_pg_index_Tuple,
								Old_pg_index_relation_Tuple,
								pg_proc_Tuple;
	Form_pg_index Old_pg_index_Form;
	Form_pg_class Old_pg_index_relation_Form;
	Form_pg_proc	pg_proc_Form;
	char					*NewIndexName;
	AttrNumber		*attnumP;
	int						natts;
	FuncIndexInfo *finfo;

	NewHeap = heap_open(OIDNewHeap);
	OldIndex = index_open(OIDOldIndex);

	/*
	 * OK. Create a new (temporary) index for the one that's already here.
	 * To do this I get the info from pg_index, re-build the FunctInfo if
	 * I have to, and add a new index with a temporary name.
	 */
	Old_pg_index_Tuple = SearchSysCacheTuple(INDEXRELID,
							ObjectIdGetDatum(RelationGetRelid(OldIndex)),
							0, 0, 0);

	Assert(Old_pg_index_Tuple);
	Old_pg_index_Form = (Form_pg_index) GETSTRUCT(Old_pg_index_Tuple);

	Old_pg_index_relation_Tuple = SearchSysCacheTuple(RELOID,
							ObjectIdGetDatum(RelationGetRelid(OldIndex)),
							0, 0, 0);

	Assert(Old_pg_index_relation_Tuple);
	Old_pg_index_relation_Form = (Form_pg_class) GETSTRUCT(Old_pg_index_relation_Tuple);

	/* Set the name. */
	NewIndexName = palloc(NAMEDATALEN); /* XXX */
	snprintf(NewIndexName, NAMEDATALEN, "temp_%x", OIDOldIndex);

	/*
	 * Ugly as it is, the only way I have of working out the number of
	 * attribues is to count them. Mostly there'll be just one but I've
	 * got to be sure.
	 */
	for (attnumP = &(Old_pg_index_Form->indkey[0]), natts = 0;
		 natts < INDEX_MAX_KEYS && *attnumP != InvalidAttrNumber;
		 attnumP++, natts++);

	/*
	 * If this is a functional index, I need to rebuild the functional
	 * component to pass it to the defining procedure.
	 */
	if (Old_pg_index_Form->indproc != InvalidOid)
	{
		finfo = (FuncIndexInfo *) palloc(sizeof(FuncIndexInfo));
		FIgetnArgs(finfo) = natts;
		FIgetProcOid(finfo) = Old_pg_index_Form->indproc;

		pg_proc_Tuple = SearchSysCacheTuple(PROOID,
							ObjectIdGetDatum(Old_pg_index_Form->indproc),
								0, 0, 0);

		Assert(pg_proc_Tuple);
		pg_proc_Form = (Form_pg_proc) GETSTRUCT(pg_proc_Tuple);
		namecpy(&(finfo->funcName), &(pg_proc_Form->proname));
	}
	else
	{
		finfo = (FuncIndexInfo *) NULL;
		natts = 1;
	}

	index_create((NewHeap->rd_rel->relname).data,
				 NewIndexName,
				 finfo,
				 NULL,			/* type info is in the old index */
				 Old_pg_index_relation_Form->relam,
				 natts,
				 Old_pg_index_Form->indkey,
				 Old_pg_index_Form->indclass,
				 (uint16) 0, (Datum) NULL, NULL,
				 Old_pg_index_Form->indislossy,
				 Old_pg_index_Form->indisunique,
                 Old_pg_index_Form->indisprimary);

	heap_close(OldIndex);
	heap_close(NewHeap);
}


static void
rebuildheap(Oid OIDNewHeap, Oid OIDOldHeap, Oid OIDOldIndex)
{
	Relation			LocalNewHeap,
						LocalOldHeap,
						LocalOldIndex;
	IndexScanDesc		ScanDesc;
	RetrieveIndexResult	ScanResult;
	HeapTupleData		LocalHeapTuple;
	Buffer				LocalBuffer;
	Oid					OIDNewHeapInsert;

	/*
	 * Open the relations I need. Scan through the OldHeap on the OldIndex
	 * and insert each tuple into the NewHeap.
	 */
	LocalNewHeap = (Relation) heap_open(OIDNewHeap);
	LocalOldHeap = (Relation) heap_open(OIDOldHeap);
	LocalOldIndex = (Relation) index_open(OIDOldIndex);

	ScanDesc = index_beginscan(LocalOldIndex, false, 0, (ScanKey) NULL);

	while ((ScanResult = index_getnext(ScanDesc, ForwardScanDirection)) != NULL)
	{

		LocalHeapTuple.t_self = ScanResult->heap_iptr;
		heap_fetch(LocalOldHeap, SnapshotNow, &LocalHeapTuple, &LocalBuffer);
		OIDNewHeapInsert = heap_insert(LocalNewHeap, &LocalHeapTuple);
		pfree(ScanResult);
		ReleaseBuffer(LocalBuffer);
	}
	index_endscan(ScanDesc);

	index_close(LocalOldIndex);
	heap_close(LocalOldHeap);
	heap_close(LocalNewHeap);
}
