/*-------------------------------------------------------------------------
 *
 * temprel.c
 *	  POSTGRES temporary relation handling
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/temprel.c,v 1.2 1999/02/13 23:19:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * This implements temp tables by modifying the relname cache lookups
 * of pg_class.
 * When a temp table is created, a linked list of temp table tuples is
 * stored here.  When a relname cache lookup is done, references to user-named
 * temp tables are converted to the internal temp table names.
 *
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>

#include "postgres.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "utils/mcxt.h"
#include "utils/temprel.h"
#include "access/htup.h"
#include "access/heapam.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"

GlobalMemory CacheCxt;

/* ----------------
 *		global variables
 * ----------------
 */

static List *temp_rels = NIL;

typedef struct TempTable
{
	char 		*user_relname;
	HeapTuple	pg_class_tuple;
} TempTable;


void
create_temp_relation(char *relname, HeapTuple pg_class_tuple)
{
	MemoryContext oldcxt;
	TempTable	*temp_rel;

	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	temp_rel = palloc(sizeof(TempTable));
	temp_rel->user_relname = palloc(NAMEDATALEN);

	/* save user-supplied name */
	strcpy(temp_rel->user_relname, relname);
            
	temp_rel->pg_class_tuple = heap_copytuple(pg_class_tuple);

	temp_rels = lcons(temp_rel, temp_rels);

	MemoryContextSwitchTo(oldcxt);
}

void
remove_all_temp_relations(void)
{
	List *l, *next;

	l = temp_rels;
	while (l != NIL)
	{
		TempTable	*temp_rel = lfirst(l);
		Form_pg_class classtuple;

		classtuple = (Form_pg_class)GETSTRUCT(temp_rel->pg_class_tuple);

		next = lnext(l); /* do this first, l is deallocated */

		if (classtuple->relkind != RELKIND_INDEX)
		{
			char relname[NAMEDATALEN];

			/* safe from deallocation */
			strcpy(relname, temp_rel->user_relname); 
			heap_destroy_with_catalog(relname);
		}
		else
			index_destroy(temp_rel->pg_class_tuple->t_data->t_oid);

		l = next;
	}
}

/* we don't have the relname for indexes, so we just pass the oid */
void
remove_temp_relation(Oid relid)
{

	MemoryContext oldcxt;
	List		*l, *prev;
	
	oldcxt = MemoryContextSwitchTo((MemoryContext) CacheCxt);

	prev = NIL;
	l = temp_rels;
	while (l != NIL)
	{
		TempTable	*temp_rel = lfirst(l);

		if (temp_rel->pg_class_tuple->t_data->t_oid == relid)
		{
			pfree(temp_rel->user_relname);
			pfree(temp_rel->pg_class_tuple);
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

HeapTuple
get_temp_rel_by_name(char *user_relname)
{
	List *l;

	foreach(l, temp_rels)
	{
		TempTable	*temp_rel = lfirst(l);

		if (strcmp(temp_rel->user_relname, user_relname) == 0)
			return temp_rel->pg_class_tuple;
	}
	return NULL;
}
