/*-------------------------------------------------------------------------
 *
 * temprel.c
 *	  POSTGRES temporary relation handling
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/temprel.c,v 1.26 2000/07/04 06:11:47 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * This implements temp tables by modifying the relname cache lookups
 * of pg_class.
 *
 * When a temp table is created, normal entries are made for it in pg_class,
 * pg_type, etc using a unique "physical" relation name.  We also make an
 * entry in the temp table list maintained by this module.  Subsequently,
 * relname lookups are filtered through the temp table list, and attempts
 * to look up a temp table name are changed to look up the physical name.
 * This allows temp table names to mask a regular table of the same name
 * for the duration of the session.  The temp table list is also used
 * to drop the underlying physical relations at session shutdown.
 */

#include <sys/types.h>

#include "postgres.h"

#include "catalog/heap.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "utils/catcache.h"
#include "utils/temprel.h"


/* ----------------
 *		global variables
 * ----------------
 */

static List *temp_rels = NIL;

typedef struct TempTable
{
	char	   *user_relname;	/* logical name of temp table */
	char	   *relname;		/* underlying unique name */
	Oid			relid;			/* needed properties of rel */
	char		relkind;
	TransactionId xid;			/* xact in which temp tab was created */
} TempTable;


/*
 * Create a temp-relation list entry given the logical temp table name
 * and the already-created pg_class tuple for the underlying relation.
 *
 * NB: we assume a check has already been made for a duplicate logical name.
 */
void
create_temp_relation(const char *relname, HeapTuple pg_class_tuple)
{
	Form_pg_class pg_class_form = (Form_pg_class) GETSTRUCT(pg_class_tuple);
	MemoryContext oldcxt;
	TempTable  *temp_rel;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	temp_rel = (TempTable *) palloc(sizeof(TempTable));
	temp_rel->user_relname = (char *) palloc(NAMEDATALEN);
	temp_rel->relname = (char *) palloc(NAMEDATALEN);

	StrNCpy(temp_rel->user_relname, relname, NAMEDATALEN);
	StrNCpy(temp_rel->relname, NameStr(pg_class_form->relname), NAMEDATALEN);
	temp_rel->relid = pg_class_tuple->t_data->t_oid;
	temp_rel->relkind = pg_class_form->relkind;
	temp_rel->xid = GetCurrentTransactionId();

	temp_rels = lcons(temp_rel, temp_rels);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Remove underlying relations for all temp rels at backend shutdown.
 */
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
		TempTable  *temp_rel = (TempTable *) lfirst(l);

		next = lnext(l);		/* do this first, l is deallocated */

		if (temp_rel->relkind != RELKIND_INDEX)
		{
			char		relname[NAMEDATALEN];

			/* safe from deallocation */
			strcpy(relname, temp_rel->user_relname);
			heap_drop_with_catalog(relname, allowSystemTableMods);
		}
		else
			index_drop(temp_rel->relid);

		l = next;
	}
	temp_rels = NIL;
	CommitTransactionCommand();
}

/*
 * Remove a temp relation map entry (part of DROP TABLE on a temp table)
 *
 * we don't have the relname for indexes, so we just pass the oid
 */
void
remove_temp_relation(Oid relid)
{
	MemoryContext oldcxt;
	List	   *l,
			   *prev;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	prev = NIL;
	l = temp_rels;
	while (l != NIL)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

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

/*
 * Remove freshly-created map entries during transaction abort.
 *
 * The underlying physical rel will be removed by normal abort processing.
 * We just have to delete the map entry.
 */
void
invalidate_temp_relations(void)
{
	MemoryContext oldcxt;
	List	   *l,
			   *prev;

	oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

	prev = NIL;
	l = temp_rels;
	while (l != NIL)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

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

/*
 * To implement ALTER TABLE RENAME on a temp table, we shouldn't touch
 * the underlying physical table at all, just change the map entry!
 *
 * This routine is invoked early in ALTER TABLE RENAME to check for
 * the temp-table case.  If oldname matches a temp table name, change
 * the map entry to the new logical name and return TRUE (or elog if
 * there is a conflict with another temp table name).  If there is
 * no match, return FALSE indicating that normal rename should proceed.
 *
 * We also reject an attempt to rename a normal table to a name in use
 * as a temp table name.  That would fail later on anyway when rename.c
 * looks for a rename conflict, but we can give a more specific error 
 * message for the problem here.
 *
 * It might seem that we need to check for attempts to rename the physical
 * file underlying a temp table, but that'll be rejected anyway because
 * pg_tempXXX looks like a system table name.
 *
 * A nitpicker might complain that the rename should be undone if the
 * current xact is later aborted, but I'm not going to fix that now.
 * This whole mapping mechanism ought to be replaced with something
 * schema-based, anyhow.
 */
bool
rename_temp_relation(const char *oldname,
					 const char *newname)
{
	List	   *l;

	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

		if (strcmp(temp_rel->user_relname, oldname) == 0)
		{
			if (get_temp_rel_by_username(newname) != NULL)
				elog(ERROR, "Cannot rename temp table \"%s\": temp table \"%s\" already exists",
					 oldname, newname);
			/* user_relname was palloc'd NAMEDATALEN, so safe to re-use it */
			StrNCpy(temp_rel->user_relname, newname, NAMEDATALEN);
			return true;
		}
	}

	/* Old name does not match any temp table name, what about new? */
	if (get_temp_rel_by_username(newname) != NULL)
		elog(ERROR, "Cannot rename \"%s\" to \"%s\": a temp table by that name already exists",
			 oldname, newname);

	return false;
}


/*
 * Map user name to physical name --- returns NULL if no entry.
 *
 * This is the normal way to test whether a name is a temp table name.
 */
char *
get_temp_rel_by_username(const char *user_relname)
{
	List	   *l;

	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

		if (strcmp(temp_rel->user_relname, user_relname) == 0)
			return temp_rel->relname;
	}
	return NULL;
}

/*
 * Map physical name to user name --- returns pstrdup'd input if no match.
 */
char *
get_temp_rel_by_physicalname(const char *relname)
{
	List	   *l;

	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

		if (strcmp(temp_rel->relname, relname) == 0)
			return temp_rel->user_relname;
	}
	/* needed for bootstrapping temp tables */
	return pstrdup(relname);
}
