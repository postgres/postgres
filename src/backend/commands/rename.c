/*-------------------------------------------------------------------------
 *
 * rename.c--
 *	  renameatt() and renamerel() reside here.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/Attic/rename.c,v 1.10 1998/01/05 03:30:52 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <postgres.h>

#include <access/heapam.h>
#include <access/relscan.h>
#include <utils/builtins.h>
#include <catalog/catname.h>
#include <utils/syscache.h>
#include <catalog/indexing.h>
#include <catalog/catalog.h>
#include <commands/copy.h>
#include <commands/rename.h>
#include <executor/execdefs.h>	/* for EXEC_{FOR,BACK,FDEBUG,BDEBUG} */
#include <miscadmin.h>
#include <utils/portal.h>
#include <tcop/dest.h>
#include <commands/command.h>
#include <utils/excid.h>
#include <utils/mcxt.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_class.h>
#include <optimizer/internal.h>
#include <optimizer/prep.h>		/* for find_all_inheritors */
#ifndef NO_SECURITY
#include <utils/acl.h>
#endif							/* !NO_SECURITY */
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
 *		get proper reldesc from relation catalog (if not arg)
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
	Relation	relrdesc,
				attrdesc;
	HeapTuple	reltup,
				oldatttup,
				newatttup;
	ItemPointerData oldTID;
	Relation	idescs[Num_pg_attr_indices];

	/*
	 * permissions checking.  this would normally be done in utility.c,
	 * but this particular routine is recursive.
	 *
	 * normally, only the owner of a class can change its schema.
	 */
	if (IsSystemRelationName(relname))
		elog(ABORT, "renameatt: class \"%s\" is a system catalog",
			 relname);
#ifndef NO_SECURITY
	if (!IsBootstrapProcessingMode() &&
		!pg_ownercheck(userName, relname, RELNAME))
		elog(ABORT, "renameatt: you do not own class \"%s\"",
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

		relrdesc = heap_openr(relname);
		if (!RelationIsValid(relrdesc))
		{
			elog(ABORT, "renameatt: unknown relation: \"%s\"",
				 relname);
		}
		myrelid = relrdesc->rd_id;
		heap_close(relrdesc);

		/* this routine is actually in the planner */
		children = find_all_inheritors(lconsi(myrelid, NIL), NIL);


		/*
		 * find_all_inheritors does the recursive search of the
		 * inheritance hierarchy, so all we have to do is process all of
		 * the relids in the list that it returns.
		 */
		foreach(child, children)
		{
			char	   *childname;

			childrelid = lfirsti(child);
			if (childrelid == myrelid)
				continue;
			relrdesc = heap_open(childrelid);
			if (!RelationIsValid(relrdesc))
			{
				elog(ABORT, "renameatt: can't find catalog entry for inheriting class with oid %d",
					 childrelid);
			}
			childname = (relrdesc->rd_rel->relname).data;
			heap_close(relrdesc);
			renameatt(childname, oldattname, newattname,
					  userName, 0);		/* no more recursion! */
		}
	}

	relrdesc = heap_openr(RelationRelationName);
	reltup = ClassNameIndexScan(relrdesc, relname);
	if (!PointerIsValid(reltup))
	{
		heap_close(relrdesc);
		elog(ABORT, "renameatt: relation \"%s\" nonexistent",
			 relname);
		return;
	}
	heap_close(relrdesc);

	attrdesc = heap_openr(AttributeRelationName);
	oldatttup = AttributeNameIndexScan(attrdesc, reltup->t_oid, oldattname);
	if (!PointerIsValid(oldatttup))
	{
		heap_close(attrdesc);
		elog(ABORT, "renameatt: attribute \"%s\" nonexistent",
			 oldattname);
	}
	if (((AttributeTupleForm) GETSTRUCT(oldatttup))->attnum < 0)
	{
		elog(ABORT, "renameatt: system attribute \"%s\" not renamed",
			 oldattname);
	}

	newatttup = AttributeNameIndexScan(attrdesc, reltup->t_oid, newattname);
	if (PointerIsValid(newatttup))
	{
		pfree(oldatttup);
		heap_close(attrdesc);
		elog(ABORT, "renameatt: attribute \"%s\" exists",
			 newattname);
	}

	namestrcpy(&(((AttributeTupleForm) (GETSTRUCT(oldatttup)))->attname),
			   newattname);
	oldTID = oldatttup->t_ctid;

	/* insert "fixed" tuple */
	heap_replace(attrdesc, &oldTID, oldatttup);

	/* keep system catalog indices current */
	CatalogOpenIndices(Num_pg_attr_indices, Name_pg_attr_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_attr_indices, attrdesc, oldatttup);
	CatalogCloseIndices(Num_pg_attr_indices, idescs);

	heap_close(attrdesc);
	pfree(oldatttup);
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
renamerel(char oldrelname[], char newrelname[])
{
	Relation	relrdesc;		/* for RELATION relation */
	HeapTuple	oldreltup,
				newreltup;
	ItemPointerData oldTID;
	char		oldpath[MAXPGPATH],
				newpath[MAXPGPATH];
	Relation	idescs[Num_pg_class_indices];

	if (IsSystemRelationName(oldrelname))
	{
		elog(ABORT, "renamerel: system relation \"%s\" not renamed",
			 oldrelname);
		return;
	}
	if (IsSystemRelationName(newrelname))
	{
		elog(ABORT, "renamerel: Illegal class name: \"%s\" -- pg_ is reserved for system catalogs",
			 newrelname);
		return;
	}

	relrdesc = heap_openr(RelationRelationName);
	oldreltup = ClassNameIndexScan(relrdesc, oldrelname);

	if (!PointerIsValid(oldreltup))
	{
		heap_close(relrdesc);
		elog(ABORT, "renamerel: relation \"%s\" does not exist",
			 oldrelname);
	}

	newreltup = ClassNameIndexScan(relrdesc, newrelname);
	if (PointerIsValid(newreltup))
	{
		pfree(oldreltup);
		heap_close(relrdesc);
		elog(ABORT, "renamerel: relation \"%s\" exists",
			 newrelname);
	}

	/* rename the directory first, so if this fails the rename's not done */
	strcpy(oldpath, relpath(oldrelname));
	strcpy(newpath, relpath(newrelname));
	if (rename(oldpath, newpath) < 0)
		elog(ABORT, "renamerel: unable to rename file: %m");

	memmove((char *) (((Form_pg_class) GETSTRUCT(oldreltup))->relname.data),
			newrelname,
			NAMEDATALEN);
	oldTID = oldreltup->t_ctid;

	/* insert fixed rel tuple */
	heap_replace(relrdesc, &oldTID, oldreltup);

	/* keep the system catalog indices current */
	CatalogOpenIndices(Num_pg_class_indices, Name_pg_class_indices, idescs);
	CatalogIndexInsert(idescs, Num_pg_class_indices, relrdesc, oldreltup);
	CatalogCloseIndices(Num_pg_class_indices, idescs);

	pfree(oldreltup);
	heap_close(relrdesc);
}
