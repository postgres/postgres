/*-------------------------------------------------------------------------
 *
 * rename.c
 *	  renameatt() and renamerel() reside here.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/rename.c,v 1.64 2002/03/21 23:27:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/itup.h"
#include "catalog/catname.h"
#include "catalog/pg_index.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/catalog.h"
#include "commands/rename.h"
#include "commands/trigger.h"
#include "miscadmin.h"
#include "storage/smgr.h"
#include "optimizer/prep.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteSupport.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/temprel.h"


#define RI_TRIGGER_PK	1		/* is a trigger on the PK relation */
#define RI_TRIGGER_FK	2		/* is a trigger on the FK relation */
#define RI_TRIGGER_NONE 0		/* is not an RI trigger function */

static int	ri_trigger_type(Oid tgfoid);
static void update_ri_trigger_args(Oid relid,
					   const char *oldname,
					   const char *newname,
					   bool fk_scan,
					   bool update_relname);


/*
 *		renameatt		- changes the name of a attribute in a relation
 *
 *		Attname attribute is changed in attribute catalog.
 *		No record of the previous attname is kept (correct?).
 *
 *		get proper relrelation from relation catalog (if not arg)
 *		scan attribute catalog
 *				for name conflict (within rel)
 *				for original attribute (if not arg)
 *		modify attname in attribute tuple
 *		insert modified attribute in attribute catalog
 *		delete original attribute from attribute catalog
 */
void
renameatt(char *relname,
		  char *oldattname,
		  char *newattname,
		  int recurse)
{
	Relation	targetrelation;
	Relation	attrelation;
	HeapTuple	reltup,
				atttup;
	Oid			relid;
	List	   *indexoidlist;
	List	   *indexoidscan;

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	targetrelation = heap_openr(relname, AccessExclusiveLock);
	relid = RelationGetRelid(targetrelation);

	/*
	 * permissions checking.  this would normally be done in utility.c,
	 * but this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (!allowSystemTableMods && IsSystemRelationName(relname))
		elog(ERROR, "renameatt: class \"%s\" is a system catalog",
			 relname);
	if (!pg_class_ownercheck(relid, GetUserId()))
		elog(ERROR, "renameatt: you do not own class \"%s\"",
			 relname);

	/*
	 * if the 'recurse' flag is set then we are supposed to rename this
	 * attribute in all classes that inherit from 'relname' (as well as in
	 * 'relname').
	 *
	 * any permissions or problems with duplicate attributes will cause the
	 * whole transaction to abort, which is what we want -- all or
	 * nothing.
	 */
	if (recurse)
	{
		List	   *child,
				   *children;

		/* this routine is actually in the planner */
		children = find_all_inheritors(relid);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			Oid			childrelid = lfirsti(child);
			char		childname[NAMEDATALEN];

			if (childrelid == relid)
				continue;
			reltup = SearchSysCache(RELOID,
									ObjectIdGetDatum(childrelid),
									0, 0, 0);
			if (!HeapTupleIsValid(reltup))
			{
				elog(ERROR, "renameatt: can't find catalog entry for inheriting class with oid %u",
					 childrelid);
			}
			/* make copy of cache value, could disappear in call */
			StrNCpy(childname,
					NameStr(((Form_pg_class) GETSTRUCT(reltup))->relname),
					NAMEDATALEN);
			ReleaseSysCache(reltup);
			/* note we need not recurse again! */
			renameatt(childname, oldattname, newattname, 0);
		}
	}

	attrelation = heap_openr(AttributeRelationName, RowExclusiveLock);

	atttup = SearchSysCacheCopy(ATTNAME,
								ObjectIdGetDatum(relid),
								PointerGetDatum(oldattname),
								0, 0);
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "renameatt: attribute \"%s\" does not exist", oldattname);

	if (((Form_pg_attribute) GETSTRUCT(atttup))->attnum < 0)
		elog(ERROR, "renameatt: system attribute \"%s\" not renamed", oldattname);

	/* should not already exist */
	if (SearchSysCacheExists(ATTNAME,
							 ObjectIdGetDatum(relid),
							 PointerGetDatum(newattname),
							 0, 0))
		elog(ERROR, "renameatt: attribute \"%s\" exists", newattname);

	StrNCpy(NameStr(((Form_pg_attribute) GETSTRUCT(atttup))->attname),
			newattname, NAMEDATALEN);

	simple_heap_update(attrelation, &atttup->t_self, atttup);

	/* keep system catalog indices current */
	{
		Relation	irelations[Num_pg_attr_indices];

		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, irelations);
		CatalogIndexInsert(irelations, Num_pg_attr_indices, attrelation, atttup);
		CatalogCloseIndices(Num_pg_attr_indices, irelations);
	}

	heap_freetuple(atttup);

	/*
	 * Update column names of indexes that refer to the column being
	 * renamed.
	 */
	indexoidlist = RelationGetIndexList(targetrelation);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirsti(indexoidscan);
		HeapTuple	indextup;

		/*
		 * First check to see if index is a functional index. If so, its
		 * column name is a function name and shouldn't be renamed here.
		 */
		indextup = SearchSysCache(INDEXRELID,
								  ObjectIdGetDatum(indexoid),
								  0, 0, 0);
		if (!HeapTupleIsValid(indextup))
			elog(ERROR, "renameatt: can't find index id %u", indexoid);
		if (OidIsValid(((Form_pg_index) GETSTRUCT(indextup))->indproc))
		{
			ReleaseSysCache(indextup);
			continue;
		}
		ReleaseSysCache(indextup);

		/*
		 * Okay, look to see if any column name of the index matches the
		 * old attribute name.
		 */
		atttup = SearchSysCacheCopy(ATTNAME,
									ObjectIdGetDatum(indexoid),
									PointerGetDatum(oldattname),
									0, 0);
		if (!HeapTupleIsValid(atttup))
			continue;			/* Nope, so ignore it */

		/*
		 * Update the (copied) attribute tuple.
		 */
		StrNCpy(NameStr(((Form_pg_attribute) GETSTRUCT(atttup))->attname),
				newattname, NAMEDATALEN);

		simple_heap_update(attrelation, &atttup->t_self, atttup);

		/* keep system catalog indices current */
		{
			Relation	irelations[Num_pg_attr_indices];

			CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, irelations);
			CatalogIndexInsert(irelations, Num_pg_attr_indices, attrelation, atttup);
			CatalogCloseIndices(Num_pg_attr_indices, irelations);
		}
		heap_freetuple(atttup);
	}

	freeList(indexoidlist);

	heap_close(attrelation, RowExclusiveLock);

	/*
	 * Update att name in any RI triggers associated with the relation.
	 */
	if (targetrelation->rd_rel->reltriggers > 0)
	{
		/* update tgargs column reference where att is primary key */
		update_ri_trigger_args(RelationGetRelid(targetrelation),
							   oldattname, newattname,
							   false, false);
		/* update tgargs column reference where att is foreign key */
		update_ri_trigger_args(RelationGetRelid(targetrelation),
							   oldattname, newattname,
							   true, false);
	}

	heap_close(targetrelation, NoLock); /* close rel but keep lock! */
}

/*
 *		renamerel		- change the name of a relation
 */
void
renamerel(const char *oldrelname, const char *newrelname)
{
	Relation	targetrelation;
	Relation	relrelation;	/* for RELATION relation */
	HeapTuple	reltup;
	Oid			reloid;
	char		relkind;
	bool		relhastriggers;
	Relation	irelations[Num_pg_class_indices];

	if (!allowSystemTableMods && IsSystemRelationName(oldrelname))
		elog(ERROR, "renamerel: system relation \"%s\" may not be renamed",
			 oldrelname);

	if (!allowSystemTableMods && IsSystemRelationName(newrelname))
		elog(ERROR, "renamerel: Illegal class name: \"%s\" -- pg_ is reserved for system catalogs",
			 newrelname);

	/*
	 * Check for renaming a temp table, which only requires altering the
	 * temp-table mapping, not the underlying table.
	 */
	if (rename_temp_relation(oldrelname, newrelname))
		return;					/* all done... */

	/*
	 * Grab an exclusive lock on the target table or index, which we will
	 * NOT release until end of transaction.
	 */
	targetrelation = relation_openr(oldrelname, AccessExclusiveLock);

	reloid = RelationGetRelid(targetrelation);
	relkind = targetrelation->rd_rel->relkind;
	relhastriggers = (targetrelation->rd_rel->reltriggers > 0);

	/*
	 * Close rel, but keep exclusive lock!
	 */
	relation_close(targetrelation, NoLock);

	/*
	 * Flush the relcache entry (easier than trying to change it at
	 * exactly the right instant).	It'll get rebuilt on next access to
	 * relation.
	 *
	 * XXX What if relation is myxactonly?
	 *
	 * XXX this is probably not necessary anymore?
	 */
	RelationIdInvalidateRelationCacheByRelationId(reloid);

	/*
	 * Find relation's pg_class tuple, and make sure newrelname isn't in
	 * use.
	 */
	relrelation = heap_openr(RelationRelationName, RowExclusiveLock);

	reltup = SearchSysCacheCopy(RELNAME,
								PointerGetDatum(oldrelname),
								0, 0, 0);
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "renamerel: relation \"%s\" does not exist", oldrelname);

	if (RelnameFindRelid(newrelname) != InvalidOid)
		elog(ERROR, "renamerel: relation \"%s\" exists", newrelname);

	/*
	 * Update pg_class tuple with new relname.	(Scribbling on reltup is
	 * OK because it's a copy...)
	 */
	StrNCpy(NameStr(((Form_pg_class) GETSTRUCT(reltup))->relname),
			newrelname, NAMEDATALEN);

	simple_heap_update(relrelation, &reltup->t_self, reltup);

	/* keep the system catalog indices current */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, irelations);
	CatalogIndexInsert(irelations, Num_pg_class_indices, relrelation, reltup);
	CatalogCloseIndices(Num_pg_class_indices, irelations);

	heap_close(relrelation, NoLock);

	/*
	 * Also rename the associated type, if any.
	 */
	if (relkind != RELKIND_INDEX)
		TypeRename(oldrelname, newrelname);

	/*
	 * If it's a view, must also rename the associated ON SELECT rule.
	 */
	if (relkind == RELKIND_VIEW)
	{
		char	   *oldrulename,
				   *newrulename;

		oldrulename = MakeRetrieveViewRuleName(oldrelname);
		newrulename = MakeRetrieveViewRuleName(newrelname);
		RenameRewriteRule(oldrulename, newrulename);
	}

	/*
	 * Update rel name in any RI triggers associated with the relation.
	 */
	if (relhastriggers)
	{
		/* update tgargs where relname is primary key */
		update_ri_trigger_args(reloid,
							   oldrelname, newrelname,
							   false, true);
		/* update tgargs where relname is foreign key */
		update_ri_trigger_args(reloid,
							   oldrelname, newrelname,
							   true, true);
	}
}

/*
 * Given a trigger function OID, determine whether it is an RI trigger,
 * and if so whether it is attached to PK or FK relation.
 *
 * XXX this probably doesn't belong here; should be exported by
 * ri_triggers.c
 */
static int
ri_trigger_type(Oid tgfoid)
{
	switch (tgfoid)
	{
		case F_RI_FKEY_CASCADE_DEL:
		case F_RI_FKEY_CASCADE_UPD:
		case F_RI_FKEY_RESTRICT_DEL:
		case F_RI_FKEY_RESTRICT_UPD:
		case F_RI_FKEY_SETNULL_DEL:
		case F_RI_FKEY_SETNULL_UPD:
		case F_RI_FKEY_SETDEFAULT_DEL:
		case F_RI_FKEY_SETDEFAULT_UPD:
		case F_RI_FKEY_NOACTION_DEL:
		case F_RI_FKEY_NOACTION_UPD:
			return RI_TRIGGER_PK;

		case F_RI_FKEY_CHECK_INS:
		case F_RI_FKEY_CHECK_UPD:
			return RI_TRIGGER_FK;
	}

	return RI_TRIGGER_NONE;
}

/*
 * Scan pg_trigger for RI triggers that are on the specified relation
 * (if fk_scan is false) or have it as the tgconstrrel (if fk_scan
 * is true).  Update RI trigger args fields matching oldname to contain
 * newname instead.  If update_relname is true, examine the relname
 * fields; otherwise examine the attname fields.
 */
static void
update_ri_trigger_args(Oid relid,
					   const char *oldname,
					   const char *newname,
					   bool fk_scan,
					   bool update_relname)
{
	Relation	tgrel;
	Relation	irel;
	ScanKeyData skey[1];
	IndexScanDesc idxtgscan;
	RetrieveIndexResult idxres;
	Datum		values[Natts_pg_trigger];
	char		nulls[Natts_pg_trigger];
	char		replaces[Natts_pg_trigger];

	tgrel = heap_openr(TriggerRelationName, RowExclusiveLock);
	if (fk_scan)
		irel = index_openr(TriggerConstrRelidIndex);
	else
		irel = index_openr(TriggerRelidIndex);

	ScanKeyEntryInitialize(&skey[0], 0x0,
						   1,	/* always column 1 of index */
						   F_OIDEQ,
						   ObjectIdGetDatum(relid));
	idxtgscan = index_beginscan(irel, false, 1, skey);

	while ((idxres = index_getnext(idxtgscan, ForwardScanDirection)) != NULL)
	{
		HeapTupleData tupledata;
		Buffer		buffer;
		HeapTuple	tuple;
		Form_pg_trigger pg_trigger;
		bytea	   *val;
		bytea	   *newtgargs;
		bool		isnull;
		int			tg_type;
		bool		examine_pk;
		bool		changed;
		int			tgnargs;
		int			i;
		int			newlen;
		const char *arga[RI_MAX_ARGUMENTS];
		const char *argp;

		tupledata.t_self = idxres->heap_iptr;
		heap_fetch(tgrel, SnapshotNow, &tupledata, &buffer, idxtgscan);
		pfree(idxres);
		if (!tupledata.t_data)
			continue;
		tuple = &tupledata;
		pg_trigger = (Form_pg_trigger) GETSTRUCT(tuple);
		tg_type = ri_trigger_type(pg_trigger->tgfoid);
		if (tg_type == RI_TRIGGER_NONE)
		{
			/* Not an RI trigger, forget it */
			ReleaseBuffer(buffer);
			continue;
		}

		/*
		 * It is an RI trigger, so parse the tgargs bytea.
		 *
		 * NB: we assume the field will never be compressed or moved out of
		 * line; so does trigger.c ...
		 */
		tgnargs = pg_trigger->tgnargs;
		val = (bytea *) fastgetattr(tuple,
									Anum_pg_trigger_tgargs,
									tgrel->rd_att, &isnull);
		if (isnull || tgnargs < RI_FIRST_ATTNAME_ARGNO ||
			tgnargs > RI_MAX_ARGUMENTS)
		{
			/* This probably shouldn't happen, but ignore busted triggers */
			ReleaseBuffer(buffer);
			continue;
		}
		argp = (const char *) VARDATA(val);
		for (i = 0; i < tgnargs; i++)
		{
			arga[i] = argp;
			argp += strlen(argp) + 1;
		}

		/*
		 * Figure out which item(s) to look at.  If the trigger is
		 * primary-key type and attached to my rel, I should look at the
		 * PK fields; if it is foreign-key type and attached to my rel, I
		 * should look at the FK fields.  But the opposite rule holds when
		 * examining triggers found by tgconstrrel search.
		 */
		examine_pk = (tg_type == RI_TRIGGER_PK) == (!fk_scan);

		changed = false;
		if (update_relname)
		{
			/* Change the relname if needed */
			i = examine_pk ? RI_PK_RELNAME_ARGNO : RI_FK_RELNAME_ARGNO;
			if (strcmp(arga[i], oldname) == 0)
			{
				arga[i] = newname;
				changed = true;
			}
		}
		else
		{
			/* Change attname(s) if needed */
			i = examine_pk ? RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_PK_IDX :
				RI_FIRST_ATTNAME_ARGNO + RI_KEYPAIR_FK_IDX;
			for (; i < tgnargs; i += 2)
			{
				if (strcmp(arga[i], oldname) == 0)
				{
					arga[i] = newname;
					changed = true;
				}
			}
		}

		if (!changed)
		{
			/* Don't need to update this tuple */
			ReleaseBuffer(buffer);
			continue;
		}

		/*
		 * Construct modified tgargs bytea.
		 */
		newlen = VARHDRSZ;
		for (i = 0; i < tgnargs; i++)
			newlen += strlen(arga[i]) + 1;
		newtgargs = (bytea *) palloc(newlen);
		VARATT_SIZEP(newtgargs) = newlen;
		newlen = VARHDRSZ;
		for (i = 0; i < tgnargs; i++)
		{
			strcpy(((char *) newtgargs) + newlen, arga[i]);
			newlen += strlen(arga[i]) + 1;
		}

		/*
		 * Build modified tuple.
		 */
		for (i = 0; i < Natts_pg_trigger; i++)
		{
			values[i] = (Datum) 0;
			replaces[i] = ' ';
			nulls[i] = ' ';
		}
		values[Anum_pg_trigger_tgargs - 1] = PointerGetDatum(newtgargs);
		replaces[Anum_pg_trigger_tgargs - 1] = 'r';

		tuple = heap_modifytuple(tuple, tgrel, values, nulls, replaces);

		/*
		 * Now we can release hold on original tuple.
		 */
		ReleaseBuffer(buffer);

		/*
		 * Update pg_trigger and its indexes
		 */
		simple_heap_update(tgrel, &tuple->t_self, tuple);

		{
			Relation	irelations[Num_pg_attr_indices];

			CatalogOpenIndices(Num_pg_trigger_indices, Name_pg_trigger_indices, irelations);
			CatalogIndexInsert(irelations, Num_pg_trigger_indices, tgrel, tuple);
			CatalogCloseIndices(Num_pg_trigger_indices, irelations);
		}

		/* free up our scratch memory */
		pfree(newtgargs);
		heap_freetuple(tuple);
	}

	index_endscan(idxtgscan);
	index_close(irel);

	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Increment cmd counter to make updates visible; this is needed in
	 * case the same tuple has to be updated again by next pass (can
	 * happen in case of a self-referential FK relationship).
	 */
	CommandCounterIncrement();
}
