/*-------------------------------------------------------------------------
 *
 * rename.c
 *	  renameatt() and renamerel() reside here.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/rename.c,v 1.21 1999/02/13 23:15:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <access/heapam.h>
#include <access/relscan.h>
#include <utils/builtins.h>
#include <catalog/catname.h>
#include <utils/syscache.h>
#include <catalog/heap.h>
#include <catalog/indexing.h>
#include <catalog/catalog.h>
#include <commands/copy.h>
#include <commands/rename.h>
#include <executor/execdefs.h>	/* for EXEC_{FOR,BACK,FDEBUG,BDEBUG} */
#include <miscadmin.h>
#include <utils/portal.h>
#include <tcop/dest.h>
#include <commands/command.h>
#include <storage/bufmgr.h>
#include <utils/excid.h>
#include <utils/mcxt.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_class.h>
#include <optimizer/internal.h>
#include <optimizer/prep.h>		/* for find_all_inheritors */
#ifndef NO_SECURITY
#include <utils/acl.h>
#endif	 /* !NO_SECURITY */
#ifndef HAVE_MEMMOVE
#include <regex/utils.h>
#else
#include <string.h>
#endif

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
	Relation	attrelation;
	HeapTuple	reltup,
				oldatttup,
				newatttup;
	Relation	irelations[Num_pg_attr_indices];
	Oid			relid;

	/*
	 * permissions checking.  this would normally be done in utility.c,
	 * but this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (IsSystemRelationName(relname))
		elog(ERROR, "renameatt: class \"%s\" is a system catalog",
			 relname);
#ifndef NO_SECURITY
	if (!IsBootstrapProcessingMode() &&
		!pg_ownercheck(userName, relname, RELNAME))
		elog(ERROR, "renameatt: you do not own class \"%s\"",
			 relname);
#endif

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
		Oid			myrelid,
					childrelid;
		List	   *child,
				   *children;

		if ((myrelid = RelnameFindRelid(relname)) == InvalidOid)
			elog(ERROR, "renameatt: unknown relation: \"%s\"", relname);

		/* this routine is actually in the planner */
		children = find_all_inheritors(lconsi(myrelid, NIL), NIL);

		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			char		childname[NAMEDATALEN];

			childrelid = lfirsti(child);
			if (childrelid == myrelid)
				continue;
			reltup = SearchSysCacheTuple(RELOID,
										 ObjectIdGetDatum(childrelid),
										 0, 0, 0);
			if (!HeapTupleIsValid(reltup))
			{
				elog(ERROR, "renameatt: can't find catalog entry for inheriting class with oid %d",
					 childrelid);
			}
			/* make copy of cache value, could disappear in call */
			StrNCpy(childname,
					((Form_pg_class) GETSTRUCT(reltup))->relname.data,
					NAMEDATALEN);
			/* no more recursion! */
			renameatt(childname, oldattname, newattname, userName, 0);
		}
	}

	
	if ((relid = RelnameFindRelid(relname)) == InvalidOid)
		elog(ERROR, "renameatt: relation \"%s\" nonexistent", relname);

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
		pfree(oldatttup);
		elog(ERROR, "renameatt: attribute \"%s\" exists", newattname);
	}

	StrNCpy((((Form_pg_attribute) (GETSTRUCT(oldatttup)))->attname.data),
			newattname, NAMEDATALEN);

	attrelation = heap_openr(AttributeRelationName);
	heap_replace(attrelation, &oldatttup->t_self, oldatttup, NULL);

	/* keep system catalog indices current */
	CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, irelations);
	CatalogIndexInsert(irelations, Num_pg_attr_indices, attrelation, oldatttup);
	CatalogCloseIndices(Num_pg_attr_indices, irelations);

	pfree(oldatttup);
	heap_close(attrelation);
}

/*
 *		renamerel		- change the name of a relation
 *
 *		Relname attribute is changed in relation catalog.
 *		No record of the previous relname is kept (correct?).
 *
 *		scan relation catalog
 *				for name conflict
 *				for original relation (if not arg)
 *		modify relname in relation tuple
 *		insert modified relation in relation catalog
 *		delete original relation from relation catalog
 *
 *		XXX Will currently lose track of a relation if it is unable to
 *				properly replace the new relation tuple.
 */
void
renamerel(char *oldrelname, char *newrelname)
{
	Relation	relrelation;	/* for RELATION relation */
	HeapTuple	oldreltup;
	char		oldpath[MAXPGPATH],
				newpath[MAXPGPATH];
	Relation	irelations[Num_pg_class_indices];

	if (IsSystemRelationName(oldrelname))
		elog(ERROR, "renamerel: system relation \"%s\" not renamed",
			 oldrelname);

	if (IsSystemRelationName(newrelname))
		elog(ERROR, "renamerel: Illegal class name: \"%s\" -- pg_ is reserved for system catalogs",
			 newrelname);

	oldreltup = SearchSysCacheTupleCopy(RELNAME,
										PointerGetDatum(oldrelname),
										0, 0, 0);
	if (!HeapTupleIsValid(oldreltup))
		elog(ERROR, "renamerel: relation \"%s\" does not exist", oldrelname);

	if (RelnameFindRelid(newrelname) != InvalidOid)
		elog(ERROR, "renamerel: relation \"%s\" exists", newrelname);

	/* rename the path first, so if this fails the rename's not done */
	strcpy(oldpath, relpath(oldrelname));
	strcpy(newpath, relpath(newrelname));
	if (rename(oldpath, newpath) < 0)
		elog(ERROR, "renamerel: unable to rename file: %s", oldpath);

	StrNCpy((((Form_pg_class) GETSTRUCT(oldreltup))->relname.data),
			newrelname, NAMEDATALEN);

	/* insert fixed rel tuple */
	relrelation = heap_openr(RelationRelationName);
	heap_replace(relrelation, &oldreltup->t_self, oldreltup, NULL);

	/* keep the system catalog indices current */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, irelations);
	CatalogIndexInsert(irelations, Num_pg_class_indices, relrelation, oldreltup);
	CatalogCloseIndices(Num_pg_class_indices, irelations);

	heap_close(relrelation);
}
