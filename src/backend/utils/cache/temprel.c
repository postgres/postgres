/*-------------------------------------------------------------------------
 *
 * temprel.c
 *	  POSTGRES temporary relation handling
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/cache/Attic/temprel.c,v 1.35 2001/03/22 03:59:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/*
 * This implements temp tables by modifying the relname cache lookups
 * of pg_class.
 *
 * When a temp table is created, normal entries are made for it in pg_class,
 * pg_type, etc using a unique "physical" relation name.  We also make an
 * entry in the temp table list maintained by this module.	Subsequently,
 * relname lookups are filtered through the temp table list, and attempts
 * to look up a temp table name are changed to look up the physical name.
 * This allows temp table names to mask a regular table of the same name
 * for the duration of the session.  The temp table list is also used
 * to drop the underlying physical relations at session shutdown.
 */

#include "postgres.h"

#include <sys/types.h>

#include "catalog/heap.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "utils/temprel.h"


/* ----------------
 *		global variables
 * ----------------
 */

static List *temp_rels = NIL;

typedef struct TempTable
{
	NameData	user_relname;	/* logical name of temp table */
	NameData	relname;		/* underlying unique name */
	Oid			relid;			/* needed properties of rel */
	char		relkind;

	/*
	 * If this entry was created during this xact, it should be deleted at
	 * xact abort.	Conversely, if this entry was deleted during this
	 * xact, it should be removed at xact commit.  We leave deleted
	 * entries in the list until commit so that we can roll back if needed
	 * --- but we ignore them for purposes of lookup!
	 */
	bool		created_in_cur_xact;
	bool		deleted_in_cur_xact;
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

	StrNCpy(NameStr(temp_rel->user_relname), relname,
			NAMEDATALEN);
	StrNCpy(NameStr(temp_rel->relname), NameStr(pg_class_form->relname),
			NAMEDATALEN);
	temp_rel->relid = pg_class_tuple->t_data->t_oid;
	temp_rel->relkind = pg_class_form->relkind;
	temp_rel->created_in_cur_xact = true;
	temp_rel->deleted_in_cur_xact = false;

	temp_rels = lcons(temp_rel, temp_rels);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Remove a temp relation map entry (part of DROP TABLE on a temp table).
 * We don't actually remove the entry, just mark it dead.
 *
 * We don't have the relname for indexes, so we just pass the oid.
 */
void
remove_temp_rel_by_relid(Oid relid)
{
	List	   *l;

	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

		if (temp_rel->relid == relid)
			temp_rel->deleted_in_cur_xact = true;

		/*
		 * Keep scanning 'cause there could be multiple matches; see
		 * RENAME
		 */
	}
}

/*
 * To implement ALTER TABLE RENAME on a temp table, we shouldn't touch
 * the underlying physical table at all, just change the map entry!
 *
 * This routine is invoked early in ALTER TABLE RENAME to check for
 * the temp-table case.  If oldname matches a temp table name, change
 * the mapping to the new logical name and return TRUE (or elog if
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
 */
bool
rename_temp_relation(const char *oldname,
					 const char *newname)
{
	List	   *l;

	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);
		MemoryContext oldcxt;
		TempTable  *new_temp_rel;

		if (temp_rel->deleted_in_cur_xact)
			continue;			/* ignore it if logically deleted */

		if (strcmp(NameStr(temp_rel->user_relname), oldname) != 0)
			continue;			/* ignore non-matching entries */

		/* We are renaming a temp table --- is it OK to do so? */
		if (is_temp_rel_name(newname))
			elog(ERROR, "Cannot rename temp table \"%s\": temp table \"%s\" already exists",
				 oldname, newname);

		/*
		 * Create a new mapping entry and mark the old one deleted in this
		 * xact.  One of these entries will be deleted at xact end.
		 *
		 * NOTE: the new mapping entry is inserted into the list just after
		 * the old one.  We could alternatively insert it before the old
		 * one, but that'd take more code.  It does need to be in one spot
		 * or the other, to ensure that deletion of temp rels happens in
		 * the right order during remove_all_temp_relations().
		 */
		oldcxt = MemoryContextSwitchTo(CacheMemoryContext);

		new_temp_rel = (TempTable *) palloc(sizeof(TempTable));
		memcpy(new_temp_rel, temp_rel, sizeof(TempTable));

		StrNCpy(NameStr(new_temp_rel->user_relname), newname, NAMEDATALEN);
		new_temp_rel->created_in_cur_xact = true;

		lnext(l) = lcons(new_temp_rel, lnext(l));

		temp_rel->deleted_in_cur_xact = true;

		MemoryContextSwitchTo(oldcxt);

		return true;
	}

	/* Old name does not match any temp table name, what about new? */
	if (is_temp_rel_name(newname))
		elog(ERROR, "Cannot rename \"%s\" to \"%s\": a temp table by that name already exists",
			 oldname, newname);

	return false;
}


/*
 * Remove underlying relations for all temp rels at backend shutdown.
 */
void
remove_all_temp_relations(void)
{
	List	   *l;

	/* skip xact start overhead if nothing to do */
	if (temp_rels == NIL)
		return;

	AbortOutOfAnyTransaction();
	StartTransactionCommand();

	/*
	 * Scan the list and delete all entries not already deleted. We need
	 * not worry about list entries getting deleted from under us, because
	 * remove_temp_rel_by_relid() doesn't remove entries, only mark them
	 * dead.  Note that entries will be deleted in reverse order of
	 * creation --- that's critical for cases involving inheritance.
	 */
	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

		if (temp_rel->deleted_in_cur_xact)
			continue;			/* ignore it if deleted already */

		if (temp_rel->relkind != RELKIND_INDEX)
		{
			char		relname[NAMEDATALEN];

			/* safe from deallocation */
			strcpy(relname, NameStr(temp_rel->user_relname));
			heap_drop_with_catalog(relname, allowSystemTableMods);
		}
		else
			index_drop(temp_rel->relid);
		/* advance cmd counter to make catalog changes visible */
		CommandCounterIncrement();
	}

	CommitTransactionCommand();
}

/*
 * Clean up temprel mapping entries during transaction commit or abort.
 *
 * During commit, remove entries that were deleted during this transaction;
 * during abort, remove those created during this transaction.
 *
 * We do not need to worry about removing the underlying physical relation;
 * that's someone else's job.
 */
void
AtEOXact_temp_relations(bool isCommit)
{
	List	   *l,
			   *prev;

	prev = NIL;
	l = temp_rels;
	while (l != NIL)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

		if (isCommit ? temp_rel->deleted_in_cur_xact :
			temp_rel->created_in_cur_xact)
		{
			/* This entry must be removed */
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
			pfree(temp_rel);
		}
		else
		{
			/* This entry must be preserved */
			temp_rel->created_in_cur_xact = false;
			temp_rel->deleted_in_cur_xact = false;
			prev = l;
			l = lnext(l);
		}
	}
}


/*
 * Map user name to physical name --- returns NULL if no entry.
 *
 * This also supports testing whether a name is a temp table name;
 * see is_temp_rel_name() macro.
 */
char *
get_temp_rel_by_username(const char *user_relname)
{
	List	   *l;

	foreach(l, temp_rels)
	{
		TempTable  *temp_rel = (TempTable *) lfirst(l);

		if (temp_rel->deleted_in_cur_xact)
			continue;			/* ignore it if logically deleted */

		if (strcmp(NameStr(temp_rel->user_relname), user_relname) == 0)
			return NameStr(temp_rel->relname);
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

		if (temp_rel->deleted_in_cur_xact)
			continue;			/* ignore it if logically deleted */

		if (strcmp(NameStr(temp_rel->relname), relname) == 0)
			return NameStr(temp_rel->user_relname);
	}
	/* needed for bootstrapping temp tables */
	return pstrdup(relname);
}
