/*-------------------------------------------------------------------------
 *
 * temprel.c
 *	  POSTGRES temporary relation handling
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/temprel.c,v 1.18 1999/12/10 03:56:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * This implements temp tables by modifying the relname cache lookups
 * of pg_class.
 * When a temp table is created, a linked list of temp table tuples is
 * stored here.  When a relname cache lookup is done, references to user-named
 * temp tables are converted to the internal temp table names.
 */

#include <sys/types.h>

#include "postgres.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "utils/temprel.h"

GlobalMemory CacheCxt;

/* ----------------
 *		global variables
 * ----------------
 */

static List *temp_rels = NIL;

typedef struct TempTable
{
	char	   *user_relname;
	char	   *relname;
	Oid			relid;
	char		relkind;
	TransactionId xid;
} TempTable;


void
create_temp_relation(char *relname, HeapTuple pg_class_tuple)
{
	MemoryContext oldcxt;
	TempTable  *temp_rel;

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	temp_rel = palloc(sizeof(TempTable));
	temp_rel->user_relname = palloc(NAMEDATALEN);
	temp_rel->relname = palloc(NAMEDATALEN);

	/* save user-supplied name */
	strcpy(temp_rel->user_relname, relname);
	StrNCpy(temp_rel->relname, NameStr(((Form_pg_class)
		GETSTRUCT(pg_class_tuple))->relname), NAMEDATALEN);
	temp_rel->relid = pg_class_tuple->t_data->t_oid;
	temp_rel->relkind = ((Form_pg_class) GETSTRUCT(pg_class_tuple))->relkind;
	temp_rel->xid = GetCurrentTransactionId();

	temp_rels = lcons(temp_rel, temp_rels);

	MemoryContextSwitchTo(oldcxt);
}

void
remove_all_temp_relations(void)
{
	List	   *l,
			   *next;

	if (temp_rels == NIL)
		return;

	AbortOutOfAnyTransaction();
	StartTransactionCommand();

	l = temp_rels;
	while (l != NIL)
	{
		TempTable  *temp_rel = lfirst(l);

		next = lnext(l);		/* do this first, l is deallocated */

		if (temp_rel->relkind != RELKIND_INDEX)
		{
			char	relname[NAMEDATALEN];

			/* safe from deallocation */
			strcpy(relname, temp_rel->user_relname);
			heap_drop_with_catalog(relname);
		}
		else
			index_drop(temp_rel->relid);

		l = next;
	}
	temp_rels = NIL;
	CommitTransactionCommand();
}

/* we don't have the relname for indexes, so we just pass the oid */
void
remove_temp_relation(Oid relid)
{

	MemoryContext oldcxt;
	List	   *l,
			   *prev;
			   
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	prev = NIL;
	l = temp_rels;
	while (l != NIL)
	{
		TempTable  *temp_rel = lfirst(l);

		if (temp_rel->relid == relid)
		{
			pfree(temp_rel->user_relname);
			pfree(temp_rel->relname);
			pfree(temp_rel);
			/* remove from linked list */
			if (prev != NIL)
			{
				lnext(prev) = lnext(l);
				pfree(l);
				l = lnext(prev);
			}
			else
			{
				temp_rels = lnext(l);
				pfree(l);
				l = temp_rels;
			}
		}
		else
		{
			prev = l;
			l = lnext(l);
		}
	}

	MemoryContextSwitchTo(oldcxt);
}

/* remove entries from aborted transactions */
void
invalidate_temp_relations(void)
{
	MemoryContext oldcxt;
	List	   *l,
			   *prev;

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	prev = NIL;
	l = temp_rels;
	while (l != NIL)
	{
		TempTable  *temp_rel = lfirst(l);

		if (temp_rel->xid == GetCurrentTransactionId())
		{
			pfree(temp_rel->user_relname);
			pfree(temp_rel->relname);
			pfree(temp_rel);
			/* remove from linked list */
			if (prev != NIL)
			{
				lnext(prev) = lnext(l);
				pfree(l);
				l = lnext(prev);
			}
			else
			{
				temp_rels = lnext(l);
				pfree(l);
				l = temp_rels;
			}
		}
		else
		{
			prev = l;
			l = lnext(l);
		}

	}

	MemoryContextSwitchTo(oldcxt);
}

char *
get_temp_rel_by_username(char *user_relname)
{
	List	   *l;

	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = lfirst(l);

		if (strcmp(temp_rel->user_relname, user_relname) == 0)
			return temp_rel->relname;
	}
	return NULL;
}

char *
get_temp_rel_by_physicalname(char *relname)
{
	List	   *l;

	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = lfirst(l);

		if (strcmp(temp_rel->relname, relname) == 0)
			return temp_rel->user_relname;
	}
	/* needed for bootstrapping temp tables */
	return relname;
}
