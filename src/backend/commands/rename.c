/*-------------------------------------------------------------------------
 *
 * rename.c
 *	  renameatt() and renamerel() reside here.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/rename.c,v 1.43 2000/05/11 03:54:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>

#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/pg_type.h"
#include "utils/syscache.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/catalog.h"
#include "commands/rename.h"
#include "miscadmin.h"
#include "storage/smgr.h"
#include "optimizer/prep.h"
#include "utils/acl.h"
#include "utils/relcache.h"


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
 *
 *		XXX Renaming an indexed attribute must (eventually) also change
 *				the attribute name in the associated indexes.
 */
void
renameatt(char *relname,
		  char *oldattname,
		  char *newattname,
		  char *userName,
		  int recurse)
{
	Relation	targetrelation;
	Relation	attrelation;
	HeapTuple	reltup,
				oldatttup,
				newatttup;
	Oid			relid;

	/*
	 * permissions checking.  this would normally be done in utility.c,
	 * but this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (!allowSystemTableMods && IsSystemRelationName(relname))
		elog(ERROR, "renameatt: class \"%s\" is a system catalog",
			 relname);
#ifndef NO_SECURITY
	if (!IsBootstrapProcessingMode() &&
		!pg_ownercheck(userName, relname, RELNAME))
		elog(ERROR, "renameatt: you do not own class \"%s\"",
			 relname);
#endif

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	targetrelation = heap_openr(relname, AccessExclusiveLock);
	relid = RelationGetRelid(targetrelation);
	heap_close(targetrelation, NoLock); /* close rel but keep lock! */

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
			reltup = SearchSysCacheTuple(RELOID,
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
			/* note we need not recurse again! */
			renameatt(childname, oldattname, newattname, userName, 0);
		}
	}

	attrelation = heap_openr(AttributeRelationName, RowExclusiveLock);

	oldatttup = SearchSysCacheTupleCopy(ATTNAME,
										ObjectIdGetDatum(relid),
										PointerGetDatum(oldattname),
										0, 0);
	if (!HeapTupleIsValid(oldatttup))
		elog(ERROR, "renameatt: attribute \"%s\" nonexistent", oldattname);

	if (((Form_pg_attribute) GETSTRUCT(oldatttup))->attnum < 0)
		elog(ERROR, "renameatt: system attribute \"%s\" not renamed", oldattname);

	newatttup = SearchSysCacheTuple(ATTNAME,
									ObjectIdGetDatum(relid),
									PointerGetDatum(newattname),
									0, 0);
	/* should not already exist */
	if (HeapTupleIsValid(newatttup))
	{
		heap_freetuple(oldatttup);
		elog(ERROR, "renameatt: attribute \"%s\" exists", newattname);
	}

	StrNCpy(NameStr(((Form_pg_attribute) GETSTRUCT(oldatttup))->attname),
			newattname, NAMEDATALEN);

	heap_update(attrelation, &oldatttup->t_self, oldatttup, NULL);

	/* keep system catalog indices current */
	{
		Relation	irelations[Num_pg_attr_indices];

		CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, irelations);
		CatalogIndexInsert(irelations, Num_pg_attr_indices, attrelation, oldatttup);
		CatalogCloseIndices(Num_pg_attr_indices, irelations);
	}

	heap_freetuple(oldatttup);
	heap_close(attrelation, RowExclusiveLock);
}

/*
 *		renamerel		- change the name of a relation
 */
void
renamerel(const char *oldrelname, const char *newrelname)
{
	int			i;
	Relation	targetrelation;
	Relation	relrelation;	/* for RELATION relation */
	HeapTuple	oldreltup;
	char		relkind;
	char		oldpath[MAXPGPATH],
				newpath[MAXPGPATH],
				toldpath[MAXPGPATH + 10],
				tnewpath[MAXPGPATH + 10];
	Relation	irelations[Num_pg_class_indices];

	if (!allowSystemTableMods && IsSystemRelationName(oldrelname))
		elog(ERROR, "renamerel: system relation \"%s\" not renamed",
			 oldrelname);

	if (!allowSystemTableMods && IsSystemRelationName(newrelname))
		elog(ERROR, "renamerel: Illegal class name: \"%s\" -- pg_ is reserved for system catalogs",
			 newrelname);

	/*
	 * Instead of using heap_openr(), do it the hard way, so that we
	 * can rename indexes as well as regular relations.
	 */
	targetrelation = RelationNameGetRelation(oldrelname);

	if (!RelationIsValid(targetrelation))
		elog(ERROR, "Relation '%s' does not exist", oldrelname);

	/*
	 * Grab an exclusive lock on the target table, which we will NOT
	 * release until end of transaction.
	 */
	LockRelation(targetrelation, AccessExclusiveLock);

	/* ----------------
	 *	RENAME TABLE within a transaction block is dangerous, because
	 *	if the transaction is later rolled back we have no way to
	 *	undo the rename of the relation's physical file.  For now, allow it
	 *	but emit a warning message.
	 *	Someday we might want to consider postponing the physical rename
	 *	until transaction commit, but that's a lot of work...
	 *	The only case that actually works right is for relations created
	 *	in the current transaction, since the post-abort state would be that
	 *	they don't exist anyway.  So, no warning in that case.
	 * ----------------
	 */
	if (IsTransactionBlock() && !targetrelation->rd_myxactonly)
		elog(NOTICE, "Caution: RENAME TABLE cannot be rolled back, so don't abort now");

	relkind = targetrelation->rd_rel->relkind;

	/*
	 * Flush all blocks of the relation out of the buffer pool.  We need
	 * this because the blocks are marked with the relation's name as well
	 * as OID. If some backend tries to write a dirty buffer with
	 * mdblindwrt after we've renamed the physical file, we'll be in big
	 * trouble.
	 *
	 * Since we hold the exclusive lock on the relation, we don't have to
	 * worry about more blocks being read in while we finish the rename.
	 */
	if (FlushRelationBuffers(targetrelation, (BlockNumber) 0, true) < 0)
		elog(ERROR, "renamerel: unable to flush relation from buffer pool");

	/*
	 * Make sure smgr and lower levels close the relation's files. (Next
	 * access to rel will reopen them.)
	 *
	 * Note: we rely on shared cache invalidation message to make other
	 * backends close and re-open the files.
	 */
	smgrclose(DEFAULT_SMGR, targetrelation);

	/*
	 * Close rel, but keep exclusive lock!
	 *
	 * Note: we don't do anything about updating the relcache entry; we
	 * assume it will be flushed by shared cache invalidate. XXX is this
	 * good enough?  What if relation is myxactonly?
	 */
	heap_close(targetrelation, NoLock);

	/*
	 * Find relation's pg_class tuple, and make sure newrelname isn't in
	 * use.
	 */
	relrelation = heap_openr(RelationRelationName, RowExclusiveLock);

	oldreltup = SearchSysCacheTupleCopy(RELNAME,
										PointerGetDatum(oldrelname),
										0, 0, 0);
	if (!HeapTupleIsValid(oldreltup))
		elog(ERROR, "renamerel: relation \"%s\" does not exist", oldrelname);

	if (RelnameFindRelid(newrelname) != InvalidOid)
		elog(ERROR, "renamerel: relation \"%s\" exists", newrelname);

	/*
	 * Perform physical rename of files.  If this fails, we haven't yet
	 * done anything irreversible.
	 *
	 * XXX smgr.c ought to provide an interface for this; doing it directly
	 * is bletcherous.
	 */
	strcpy(oldpath, relpath(oldrelname));
	strcpy(newpath, relpath(newrelname));
	if (rename(oldpath, newpath) < 0)
		elog(ERROR, "renamerel: unable to rename %s to %s: %m",
			 oldpath, newpath);

	/* rename additional segments of relation, too */
	for (i = 1;; i++)
	{
		sprintf(toldpath, "%s.%d", oldpath, i);
		sprintf(tnewpath, "%s.%d", newpath, i);
		if (rename(toldpath, tnewpath) < 0)
		{
			/* expected case is that there's not another segment file */
			if (errno == ENOENT)
				break;
			/* otherwise we're up the creek... */
			elog(ERROR, "renamerel: unable to rename %s to %s: %m",
				 toldpath, tnewpath);
		}
	}

	/*
	 * Update pg_class tuple with new relname.
	 */
	StrNCpy(NameStr(((Form_pg_class) GETSTRUCT(oldreltup))->relname),
			newrelname, NAMEDATALEN);

	heap_update(relrelation, &oldreltup->t_self, oldreltup, NULL);

	/* keep the system catalog indices current */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, irelations);
	CatalogIndexInsert(irelations, Num_pg_class_indices, relrelation, oldreltup);
	CatalogCloseIndices(Num_pg_class_indices, irelations);

	heap_close(relrelation, RowExclusiveLock);

	/*
	 * Also rename the associated type, if any.
	 */
	if (relkind != RELKIND_INDEX)
		TypeRename(oldrelname, newrelname);
}
