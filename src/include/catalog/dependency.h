/*-------------------------------------------------------------------------
 *
 * dependency.h
 *	  Routines to support inter-object dependencies.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dependency.h,v 1.10 2003/08/04 02:40:10 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEPENDENCY_H
#define DEPENDENCY_H

#include "nodes/parsenodes.h"	/* for DropBehavior */


/*
 * Precise semantics of a dependency relationship are specified by the
 * DependencyType code (which is stored in a "char" field in pg_depend,
 * so we assign ASCII-code values to the enumeration members).
 *
 * In all cases, a dependency relationship indicates that the referenced
 * object may not be dropped without also dropping the dependent object.
 * However, there are several subflavors:
 *
 * DEPENDENCY_NORMAL ('n'): normal relationship between separately-created
 * objects.  The dependent object may be dropped without affecting the
 * referenced object.  The referenced object may only be dropped by
 * specifying CASCADE, in which case the dependent object is dropped too.
 * Example: a table column has a normal dependency on its datatype.
 *
 * DEPENDENCY_AUTO ('a'): the dependent object can be dropped separately
 * from the referenced object, and should be automatically dropped
 * (regardless of RESTRICT or CASCADE mode) if the referenced object
 * is dropped.
 * Example: a named constraint on a table is made auto-dependent on
 * the table, so that it will go away if the table is dropped.
 *
 * DEPENDENCY_INTERNAL ('i'): the dependent object was created as part
 * of creation of the referenced object, and is really just a part of
 * its internal implementation.  A DROP of the dependent object will be
 * disallowed outright (we'll tell the user to issue a DROP against the
 * referenced object, instead).  A DROP of the referenced object will be
 * propagated through to drop the dependent object whether CASCADE is
 * specified or not.
 * Example: a trigger that's created to enforce a foreign-key constraint
 * is made internally dependent on the constraint's pg_constraint entry.
 *
 * DEPENDENCY_PIN ('p'): there is no dependent object; this type of entry
 * is a signal that the system itself depends on the referenced object,
 * and so that object must never be deleted.  Entries of this type are
 * created only during initdb.	The fields for the dependent object
 * contain zeroes.
 *
 * Other dependency flavors may be needed in future.
 */

typedef enum DependencyType
{
	DEPENDENCY_NORMAL = 'n',
	DEPENDENCY_AUTO = 'a',
	DEPENDENCY_INTERNAL = 'i',
	DEPENDENCY_PIN = 'p'
} DependencyType;


/*
 * The two objects related by a dependency are identified by ObjectAddresses.
 */
typedef struct ObjectAddress
{
	Oid			classId;		/* Class Id from pg_class */
	Oid			objectId;		/* OID of the object */
	int32		objectSubId;	/* Subitem within the object (column of
								 * table) */
} ObjectAddress;


/* in dependency.c */

extern void performDeletion(const ObjectAddress *object,
				DropBehavior behavior);

extern void deleteWhatDependsOn(const ObjectAddress *object,
					bool showNotices);

extern void recordDependencyOnExpr(const ObjectAddress *depender,
					   Node *expr, List *rtable,
					   DependencyType behavior);

extern void recordDependencyOnSingleRelExpr(const ObjectAddress *depender,
								Node *expr, Oid relId,
								DependencyType behavior,
								DependencyType self_behavior);

/* in pg_depend.c */

extern void recordDependencyOn(const ObjectAddress *depender,
				   const ObjectAddress *referenced,
				   DependencyType behavior);

extern void recordMultipleDependencies(const ObjectAddress *depender,
						   const ObjectAddress *referenced,
						   int nreferenced,
						   DependencyType behavior);

extern long deleteDependencyRecordsFor(Oid classId, Oid objectId);

#endif   /* DEPENDENCY_H */
