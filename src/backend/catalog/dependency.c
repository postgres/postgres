/*-------------------------------------------------------------------------
 *
 * dependency.c
 *	  Routines to support inter-object dependencies.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/dependency.c,v 1.31 2003/08/11 23:04:49 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catname.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_language.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_trigger.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteRemove.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* This enum covers all system catalogs whose OIDs can appear in classid. */
typedef enum ObjectClasses
{
	OCLASS_CLASS,				/* pg_class */
	OCLASS_PROC,				/* pg_proc */
	OCLASS_TYPE,				/* pg_type */
	OCLASS_CAST,				/* pg_cast */
	OCLASS_CONSTRAINT,			/* pg_constraint */
	OCLASS_CONVERSION,			/* pg_conversion */
	OCLASS_DEFAULT,				/* pg_attrdef */
	OCLASS_LANGUAGE,			/* pg_language */
	OCLASS_OPERATOR,			/* pg_operator */
	OCLASS_OPCLASS,				/* pg_opclass */
	OCLASS_REWRITE,				/* pg_rewrite */
	OCLASS_TRIGGER,				/* pg_trigger */
	OCLASS_SCHEMA,				/* pg_namespace */
	MAX_OCLASS					/* MUST BE LAST */
} ObjectClasses;

/* expansible list of ObjectAddresses */
typedef struct
{
	ObjectAddress *refs;		/* => palloc'd array */
	int			numrefs;		/* current number of references */
	int			maxrefs;		/* current size of palloc'd array */
} ObjectAddresses;

/* for find_expr_references_walker */
typedef struct
{
	ObjectAddresses addrs;		/* addresses being accumulated */
	List	   *rtables;		/* list of rangetables to resolve Vars */
} find_expr_references_context;


/*
 * Because not all system catalogs have predetermined OIDs, we build a table
 * mapping between ObjectClasses and OIDs.	This is done at most once per
 * backend run, to minimize lookup overhead.
 */
static bool object_classes_initialized = false;
static Oid	object_classes[MAX_OCLASS];


static void findAutoDeletableObjects(const ObjectAddress *object,
						 ObjectAddresses *oktodelete,
						 Relation depRel);
static bool recursiveDeletion(const ObjectAddress *object,
				  DropBehavior behavior,
				  int msglevel,
				  const ObjectAddress *callingObject,
				  ObjectAddresses *oktodelete,
				  Relation depRel);
static bool deleteDependentObjects(const ObjectAddress *object,
					   const char *objDescription,
					   DropBehavior behavior,
					   int msglevel,
					   ObjectAddresses *oktodelete,
					   Relation depRel);
static void doDeletion(const ObjectAddress *object);
static bool find_expr_references_walker(Node *node,
							find_expr_references_context *context);
static void eliminate_duplicate_dependencies(ObjectAddresses *addrs);
static int	object_address_comparator(const void *a, const void *b);
static void init_object_addresses(ObjectAddresses *addrs);
static void add_object_address(ObjectClasses oclass, Oid objectId, int32 subId,
				   ObjectAddresses *addrs);
static void add_exact_object_address(const ObjectAddress *object,
						 ObjectAddresses *addrs);
static bool object_address_present(const ObjectAddress *object,
					   ObjectAddresses *addrs);
static void term_object_addresses(ObjectAddresses *addrs);
static void init_object_classes(void);
static ObjectClasses getObjectClass(const ObjectAddress *object);
static char *getObjectDescription(const ObjectAddress *object);
static void getRelationDescription(StringInfo buffer, Oid relid);


/*
 * performDeletion: attempt to drop the specified object.  If CASCADE
 * behavior is specified, also drop any dependent objects (recursively).
 * If RESTRICT behavior is specified, error out if there are any dependent
 * objects, except for those that should be implicitly dropped anyway
 * according to the dependency type.
 *
 * This is the outer control routine for all forms of DROP that drop objects
 * that can participate in dependencies.
 */
void
performDeletion(const ObjectAddress *object,
				DropBehavior behavior)
{
	char	   *objDescription;
	Relation	depRel;
	ObjectAddresses oktodelete;

	/*
	 * Get object description for possible use in failure message. Must do
	 * this before deleting it ...
	 */
	objDescription = getObjectDescription(object);

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_openr(DependRelationName, RowExclusiveLock);

	/*
	 * Construct a list of objects that are reachable by AUTO or INTERNAL
	 * dependencies from the target object.  These should be deleted
	 * silently, even if the actual deletion pass first reaches one of
	 * them via a non-auto dependency.
	 */
	init_object_addresses(&oktodelete);

	findAutoDeletableObjects(object, &oktodelete, depRel);

	if (!recursiveDeletion(object, behavior, NOTICE,
						   NULL, &oktodelete, depRel))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
			  errmsg("cannot drop %s because other objects depend on it",
					 objDescription),
				 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));

	term_object_addresses(&oktodelete);

	heap_close(depRel, RowExclusiveLock);

	pfree(objDescription);
}


/*
 * deleteWhatDependsOn: attempt to drop everything that depends on the
 * specified object, though not the object itself.	Behavior is always
 * CASCADE.
 *
 * This is currently used only to clean out the contents of a schema
 * (namespace): the passed object is a namespace.  We normally want this
 * to be done silently, so there's an option to suppress NOTICE messages.
 */
void
deleteWhatDependsOn(const ObjectAddress *object,
					bool showNotices)
{
	char	   *objDescription;
	Relation	depRel;
	ObjectAddresses oktodelete;

	/*
	 * Get object description for possible use in failure messages
	 */
	objDescription = getObjectDescription(object);

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_openr(DependRelationName, RowExclusiveLock);

	/*
	 * Construct a list of objects that are reachable by AUTO or INTERNAL
	 * dependencies from the target object.  These should be deleted
	 * silently, even if the actual deletion pass first reaches one of
	 * them via a non-auto dependency.
	 */
	init_object_addresses(&oktodelete);

	findAutoDeletableObjects(object, &oktodelete, depRel);

	/*
	 * Now invoke only step 2 of recursiveDeletion: just recurse to the
	 * stuff dependent on the given object.
	 */
	if (!deleteDependentObjects(object, objDescription,
								DROP_CASCADE,
								showNotices ? NOTICE : DEBUG2,
								&oktodelete, depRel))
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg("failed to drop all objects depending on %s",
						objDescription)));

	/*
	 * We do not need CommandCounterIncrement here, since if step 2 did
	 * anything then each recursive call will have ended with one.
	 */

	term_object_addresses(&oktodelete);

	heap_close(depRel, RowExclusiveLock);

	pfree(objDescription);
}


/*
 * findAutoDeletableObjects: find all objects that are reachable by AUTO or
 * INTERNAL dependency paths from the given object.  Add them all to the
 * oktodelete list.  Note that the originally given object will also be
 * added to the list.
 *
 * depRel is the already-open pg_depend relation.
 */
static void
findAutoDeletableObjects(const ObjectAddress *object,
						 ObjectAddresses *oktodelete,
						 Relation depRel)
{
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress otherObject;

	/*
	 * If this object is already in oktodelete, then we already visited
	 * it; don't do so again (this prevents infinite recursion if there's
	 * a loop in pg_depend).  Otherwise, add it.
	 */
	if (object_address_present(object, oktodelete))
		return;
	add_exact_object_address(object, oktodelete);

	/*
	 * Scan pg_depend records that link to this object, showing the things
	 * that depend on it.  For each one that is AUTO or INTERNAL, visit
	 * the referencing object.
	 *
	 * When dropping a whole object (subId = 0), find pg_depend records for
	 * its sub-objects too.
	 */
	ScanKeyEntryInitialize(&key[0], 0x0,
						   Anum_pg_depend_refclassid, F_OIDEQ,
						   ObjectIdGetDatum(object->classId));
	ScanKeyEntryInitialize(&key[1], 0x0,
						   Anum_pg_depend_refobjid, F_OIDEQ,
						   ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		ScanKeyEntryInitialize(&key[2], 0x0,
							   Anum_pg_depend_refobjsubid, F_INT4EQ,
							   Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(depRel, DependReferenceIndex, true,
							  SnapshotNow, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:
				/* ignore */
				break;
			case DEPENDENCY_AUTO:
			case DEPENDENCY_INTERNAL:
				/* recurse */
				otherObject.classId = foundDep->classid;
				otherObject.objectId = foundDep->objid;
				otherObject.objectSubId = foundDep->objsubid;
				findAutoDeletableObjects(&otherObject, oktodelete, depRel);
				break;
			case DEPENDENCY_PIN:

				/*
				 * For a PIN dependency we just ereport immediately; there
				 * won't be any others to examine, and we aren't ever
				 * going to let the user delete it.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot drop %s because it is required by the database system",
								getObjectDescription(object))));
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, getObjectDescription(object));
				break;
		}
	}

	systable_endscan(scan);
}


/*
 * recursiveDeletion: delete a single object for performDeletion, plus
 * (recursively) anything that depends on it.
 *
 * Returns TRUE if successful, FALSE if not.
 *
 * callingObject is NULL at the outer level, else identifies the object that
 * we recursed from (the reference object that someone else needs to delete).
 *
 * oktodelete is a list of objects verified deletable (ie, reachable by one
 * or more AUTO or INTERNAL dependencies from the original target).
 *
 * depRel is the already-open pg_depend relation.
 *
 *
 * In RESTRICT mode, we perform all the deletions anyway, but ereport a message
 * and return FALSE if we find a restriction violation.  performDeletion
 * will then abort the transaction to nullify the deletions.  We have to
 * do it this way to (a) report all the direct and indirect dependencies
 * while (b) not going into infinite recursion if there's a cycle.
 *
 * This is even more complex than one could wish, because it is possible for
 * the same pair of objects to be related by both NORMAL and AUTO/INTERNAL
 * dependencies.  Also, we might have a situation where we've been asked to
 * delete object A, and objects B and C both have AUTO dependencies on A,
 * but B also has a NORMAL dependency on C.  (Since any of these paths might
 * be indirect, we can't prevent these scenarios, but must cope instead.)
 * If we visit C before B then we would mistakenly decide that the B->C link
 * should prevent the restricted drop from occurring.  To handle this, we make
 * a pre-scan to find all the objects that are auto-deletable from A.  If we
 * visit C first, but B is present in the oktodelete list, then we make no
 * complaint but recurse to delete B anyway.  (Note that in general we must
 * delete B before deleting C; the drop routine for B may try to access C.)
 *
 * Note: in the case where the path to B is traversed first, we will not
 * see the NORMAL dependency when we reach C, because of the pg_depend
 * removals done in step 1.  The oktodelete list is necessary just
 * to make the behavior independent of the order in which pg_depend
 * entries are visited.
 */
static bool
recursiveDeletion(const ObjectAddress *object,
				  DropBehavior behavior,
				  int msglevel,
				  const ObjectAddress *callingObject,
				  ObjectAddresses *oktodelete,
				  Relation depRel)
{
	bool		ok = true;
	char	   *objDescription;
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress otherObject;
	ObjectAddress owningObject;
	bool		amOwned = false;

	/*
	 * Get object description for possible use in messages.  Must do this
	 * before deleting it ...
	 */
	objDescription = getObjectDescription(object);

	/*
	 * Step 1: find and remove pg_depend records that link from this
	 * object to others.  We have to do this anyway, and doing it first
	 * ensures that we avoid infinite recursion in the case of cycles.
	 * Also, some dependency types require extra processing here.
	 *
	 * When dropping a whole object (subId = 0), remove all pg_depend records
	 * for its sub-objects too.
	 */
	ScanKeyEntryInitialize(&key[0], 0x0,
						   Anum_pg_depend_classid, F_OIDEQ,
						   ObjectIdGetDatum(object->classId));
	ScanKeyEntryInitialize(&key[1], 0x0,
						   Anum_pg_depend_objid, F_OIDEQ,
						   ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		ScanKeyEntryInitialize(&key[2], 0x0,
							   Anum_pg_depend_objsubid, F_INT4EQ,
							   Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(depRel, DependDependerIndex, true,
							  SnapshotNow, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		otherObject.classId = foundDep->refclassid;
		otherObject.objectId = foundDep->refobjid;
		otherObject.objectSubId = foundDep->refobjsubid;

		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:
			case DEPENDENCY_AUTO:
				/* no problem */
				break;
			case DEPENDENCY_INTERNAL:

				/*
				 * This object is part of the internal implementation of
				 * another object.	We have three cases:
				 *
				 * 1. At the outermost recursion level, disallow the DROP.
				 * (We just ereport here, rather than proceeding, since no
				 * other dependencies are likely to be interesting.)
				 */
				if (callingObject == NULL)
				{
					char	   *otherObjDesc = getObjectDescription(&otherObject);

					ereport(ERROR,
						 (errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						  errmsg("cannot drop %s because %s requires it",
								 objDescription, otherObjDesc),
						  errhint("You may drop %s instead.",
								  otherObjDesc)));
				}

				/*
				 * 2. When recursing from the other end of this
				 * dependency, it's okay to continue with the deletion.
				 * This holds when recursing from a whole object that
				 * includes the nominal other end as a component, too.
				 */
				if (callingObject->classId == otherObject.classId &&
					callingObject->objectId == otherObject.objectId &&
				(callingObject->objectSubId == otherObject.objectSubId ||
				 callingObject->objectSubId == 0))
					break;

				/*
				 * 3. When recursing from anyplace else, transform this
				 * deletion request into a delete of the other object.
				 * (This will be an error condition iff RESTRICT mode.) In
				 * this case we finish deleting my dependencies except for
				 * the INTERNAL link, which will be needed to cause the
				 * owning object to recurse back to me.
				 */
				if (amOwned)	/* shouldn't happen */
					elog(ERROR, "multiple INTERNAL dependencies for %s",
						 objDescription);
				owningObject = otherObject;
				amOwned = true;
				/* "continue" bypasses the simple_heap_delete call below */
				continue;
			case DEPENDENCY_PIN:

				/*
				 * Should not happen; PIN dependencies should have zeroes
				 * in the depender fields...
				 */
				elog(ERROR, "incorrect use of PIN dependency with %s",
					 objDescription);
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, objDescription);
				break;
		}

		simple_heap_delete(depRel, &tup->t_self);
	}

	systable_endscan(scan);

	/*
	 * CommandCounterIncrement here to ensure that preceding changes are
	 * all visible; in particular, that the above deletions of pg_depend
	 * entries are visible.  That prevents infinite recursion in case of a
	 * dependency loop (which is perfectly legal).
	 */
	CommandCounterIncrement();

	/*
	 * If we found we are owned by another object, ask it to delete itself
	 * instead of proceeding.  Complain if RESTRICT mode, unless the other
	 * object is in oktodelete.
	 */
	if (amOwned)
	{
		if (object_address_present(&owningObject, oktodelete))
			ereport(DEBUG2,
					(errmsg("drop auto-cascades to %s",
							getObjectDescription(&owningObject))));
		else if (behavior == DROP_RESTRICT)
		{
			ereport(msglevel,
					(errmsg("%s depends on %s",
							getObjectDescription(&owningObject),
							objDescription)));
			ok = false;
		}
		else
			ereport(msglevel,
					(errmsg("drop cascades to %s",
							getObjectDescription(&owningObject))));

		if (!recursiveDeletion(&owningObject, behavior, msglevel,
							   object, oktodelete, depRel))
			ok = false;

		pfree(objDescription);

		return ok;
	}

	/*
	 * Step 2: scan pg_depend records that link to this object, showing
	 * the things that depend on it.  Recursively delete those things.
	 * Note it's important to delete the dependent objects before the
	 * referenced one, since the deletion routines might do things like
	 * try to update the pg_class record when deleting a check constraint.
	 */
	if (!deleteDependentObjects(object, objDescription,
								behavior, msglevel,
								oktodelete, depRel))
		ok = false;

	/*
	 * We do not need CommandCounterIncrement here, since if step 2 did
	 * anything then each recursive call will have ended with one.
	 */

	/*
	 * Step 3: delete the object itself.
	 */
	doDeletion(object);

	/*
	 * Delete any comments associated with this object.  (This is a
	 * convenient place to do it instead of having every object type know
	 * to do it.)
	 */
	DeleteComments(object->objectId, object->classId, object->objectSubId);

	/*
	 * CommandCounterIncrement here to ensure that preceding changes are
	 * all visible.
	 */
	CommandCounterIncrement();

	/*
	 * And we're done!
	 */
	pfree(objDescription);

	return ok;
}


/*
 * deleteDependentObjects - find and delete objects that depend on 'object'
 *
 * Scan pg_depend records that link to the given object, showing
 * the things that depend on it.  Recursively delete those things. (We
 * don't delete the pg_depend records here, as the recursive call will
 * do that.)  Note it's important to delete the dependent objects
 * before the referenced one, since the deletion routines might do
 * things like try to update the pg_class record when deleting a check
 * constraint.
 *
 * When dropping a whole object (subId = 0), find pg_depend records for
 * its sub-objects too.
 *
 *	object: the object to find dependencies on
 *	objDescription: description of object (only used for error messages)
 *	behavior: desired drop behavior
 *	oktodelete: stuff that's AUTO-deletable
 *	depRel: already opened pg_depend relation
 *
 * Returns TRUE if all is well, false if any problem found.
 *
 * NOTE: because we are using SnapshotNow, if a recursive call deletes
 * any pg_depend tuples that our scan hasn't yet visited, we will not
 * see them as good when we do visit them.	This is essential for
 * correct behavior if there are multiple dependency paths between two
 * objects --- else we might try to delete an already-deleted object.
 */
static bool
deleteDependentObjects(const ObjectAddress *object,
					   const char *objDescription,
					   DropBehavior behavior,
					   int msglevel,
					   ObjectAddresses *oktodelete,
					   Relation depRel)
{
	bool		ok = true;
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress otherObject;

	ScanKeyEntryInitialize(&key[0], 0x0,
						   Anum_pg_depend_refclassid, F_OIDEQ,
						   ObjectIdGetDatum(object->classId));
	ScanKeyEntryInitialize(&key[1], 0x0,
						   Anum_pg_depend_refobjid, F_OIDEQ,
						   ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		ScanKeyEntryInitialize(&key[2], 0x0,
							   Anum_pg_depend_refobjsubid, F_INT4EQ,
							   Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(depRel, DependReferenceIndex, true,
							  SnapshotNow, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);

		otherObject.classId = foundDep->classid;
		otherObject.objectId = foundDep->objid;
		otherObject.objectSubId = foundDep->objsubid;

		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:

				/*
				 * Perhaps there was another dependency path that would
				 * have allowed silent deletion of the otherObject, had we
				 * only taken that path first. In that case, act like this
				 * link is AUTO, too.
				 */
				if (object_address_present(&otherObject, oktodelete))
					ereport(DEBUG2,
							(errmsg("drop auto-cascades to %s",
									getObjectDescription(&otherObject))));
				else if (behavior == DROP_RESTRICT)
				{
					ereport(msglevel,
							(errmsg("%s depends on %s",
									getObjectDescription(&otherObject),
									objDescription)));
					ok = false;
				}
				else
					ereport(msglevel,
							(errmsg("drop cascades to %s",
									getObjectDescription(&otherObject))));

				if (!recursiveDeletion(&otherObject, behavior, msglevel,
									   object, oktodelete, depRel))
					ok = false;
				break;
			case DEPENDENCY_AUTO:
			case DEPENDENCY_INTERNAL:

				/*
				 * We propagate the DROP without complaint even in the
				 * RESTRICT case.  (However, normal dependencies on the
				 * component object could still cause failure.)
				 */
				ereport(DEBUG2,
						(errmsg("drop auto-cascades to %s",
								getObjectDescription(&otherObject))));

				if (!recursiveDeletion(&otherObject, behavior, msglevel,
									   object, oktodelete, depRel))
					ok = false;
				break;
			case DEPENDENCY_PIN:

				/*
				 * For a PIN dependency we just ereport immediately; there
				 * won't be any others to report.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot drop %s because it is required by the database system",
								objDescription)));
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, objDescription);
				break;
		}
	}

	systable_endscan(scan);

	return ok;
}


/*
 * doDeletion: actually delete a single object
 */
static void
doDeletion(const ObjectAddress *object)
{
	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			{
				char		relKind = get_rel_relkind(object->objectId);

				if (relKind == RELKIND_INDEX)
				{
					Assert(object->objectSubId == 0);
					index_drop(object->objectId);
				}
				else
				{
					if (object->objectSubId != 0)
						RemoveAttributeById(object->objectId,
											object->objectSubId);
					else
						heap_drop_with_catalog(object->objectId);
				}
				break;
			}

		case OCLASS_PROC:
			RemoveFunctionById(object->objectId);
			break;

		case OCLASS_TYPE:
			RemoveTypeById(object->objectId);
			break;

		case OCLASS_CAST:
			DropCastById(object->objectId);
			break;

		case OCLASS_CONSTRAINT:
			RemoveConstraintById(object->objectId);
			break;

		case OCLASS_CONVERSION:
			RemoveConversionById(object->objectId);
			break;

		case OCLASS_DEFAULT:
			RemoveAttrDefaultById(object->objectId);
			break;

		case OCLASS_LANGUAGE:
			DropProceduralLanguageById(object->objectId);
			break;

		case OCLASS_OPERATOR:
			RemoveOperatorById(object->objectId);
			break;

		case OCLASS_OPCLASS:
			RemoveOpClassById(object->objectId);
			break;

		case OCLASS_REWRITE:
			RemoveRewriteRuleById(object->objectId);
			break;

		case OCLASS_TRIGGER:
			RemoveTriggerById(object->objectId);
			break;

		case OCLASS_SCHEMA:
			RemoveSchemaById(object->objectId);
			break;

		default:
			elog(ERROR, "unrecognized object class: %u",
				 object->classId);
	}
}

/*
 * recordDependencyOnExpr - find expression dependencies
 *
 * This is used to find the dependencies of rules, constraint expressions,
 * etc.
 *
 * Given an expression or query in node-tree form, find all the objects
 * it refers to (tables, columns, operators, functions, etc).  Record
 * a dependency of the specified type from the given depender object
 * to each object mentioned in the expression.
 *
 * rtable is the rangetable to be used to interpret Vars with varlevelsup=0.
 * It can be NIL if no such variables are expected.
 *
 * XXX is it important to create dependencies on the datatypes mentioned in
 * the expression?	In most cases this would be redundant (eg, a ref to an
 * operator indirectly references its input and output datatypes), but I'm
 * not quite convinced there are no cases where we need it.
 */
void
recordDependencyOnExpr(const ObjectAddress *depender,
					   Node *expr, List *rtable,
					   DependencyType behavior)
{
	find_expr_references_context context;

	init_object_addresses(&context.addrs);

	/* Set up interpretation for Vars at varlevelsup = 0 */
	context.rtables = makeList1(rtable);

	/* Scan the expression tree for referenceable objects */
	find_expr_references_walker(expr, &context);

	/* Remove any duplicates */
	eliminate_duplicate_dependencies(&context.addrs);

	/* And record 'em */
	recordMultipleDependencies(depender,
							   context.addrs.refs, context.addrs.numrefs,
							   behavior);

	term_object_addresses(&context.addrs);
}

/*
 * recordDependencyOnSingleRelExpr - find expression dependencies
 *
 * As above, but only one relation is expected to be referenced (with
 * varno = 1 and varlevelsup = 0).	Pass the relation OID instead of a
 * range table.  An additional frammish is that dependencies on that
 * relation (or its component columns) will be marked with 'self_behavior',
 * whereas 'behavior' is used for everything else.
 */
void
recordDependencyOnSingleRelExpr(const ObjectAddress *depender,
								Node *expr, Oid relId,
								DependencyType behavior,
								DependencyType self_behavior)
{
	find_expr_references_context context;
	RangeTblEntry rte;

	init_object_addresses(&context.addrs);

	/* We gin up a rather bogus rangetable list to handle Vars */
	MemSet(&rte, 0, sizeof(rte));
	rte.type = T_RangeTblEntry;
	rte.rtekind = RTE_RELATION;
	rte.relid = relId;

	context.rtables = makeList1(makeList1(&rte));

	/* Scan the expression tree for referenceable objects */
	find_expr_references_walker(expr, &context);

	/* Remove any duplicates */
	eliminate_duplicate_dependencies(&context.addrs);

	/* Separate self-dependencies if necessary */
	if (behavior != self_behavior && context.addrs.numrefs > 0)
	{
		ObjectAddresses self_addrs;
		ObjectAddress *outobj;
		int			oldref,
					outrefs;

		init_object_addresses(&self_addrs);

		outobj = context.addrs.refs;
		outrefs = 0;
		for (oldref = 0; oldref < context.addrs.numrefs; oldref++)
		{
			ObjectAddress *thisobj = context.addrs.refs + oldref;

			if (thisobj->classId == RelOid_pg_class &&
				thisobj->objectId == relId)
			{
				/* Move this ref into self_addrs */
				add_object_address(OCLASS_CLASS, relId, thisobj->objectSubId,
								   &self_addrs);
			}
			else
			{
				/* Keep it in context.addrs */
				outobj->classId = thisobj->classId;
				outobj->objectId = thisobj->objectId;
				outobj->objectSubId = thisobj->objectSubId;
				outobj++;
				outrefs++;
			}
		}
		context.addrs.numrefs = outrefs;

		/* Record the self-dependencies */
		recordMultipleDependencies(depender,
								   self_addrs.refs, self_addrs.numrefs,
								   self_behavior);

		term_object_addresses(&self_addrs);
	}

	/* Record the external dependencies */
	recordMultipleDependencies(depender,
							   context.addrs.refs, context.addrs.numrefs,
							   behavior);

	term_object_addresses(&context.addrs);
}

/*
 * Recursively search an expression tree for object references.
 *
 * Note: we avoid creating references to columns of tables that participate
 * in an SQL JOIN construct, but are not actually used anywhere in the query.
 * To do so, we do not scan the joinaliasvars list of a join RTE while
 * scanning the query rangetable, but instead scan each individual entry
 * of the alias list when we find a reference to it.
 */
static bool
find_expr_references_walker(Node *node,
							find_expr_references_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		int			levelsup;
		List	   *rtable,
				   *rtables;
		RangeTblEntry *rte;

		/* Find matching rtable entry, or complain if not found */
		levelsup = var->varlevelsup;
		rtables = context->rtables;
		while (levelsup--)
		{
			if (rtables == NIL)
				break;
			rtables = lnext(rtables);
		}
		if (rtables == NIL)
			elog(ERROR, "invalid varlevelsup %d", var->varlevelsup);
		rtable = lfirst(rtables);
		if (var->varno <= 0 || var->varno > length(rtable))
			elog(ERROR, "invalid varno %d", var->varno);
		rte = rt_fetch(var->varno, rtable);
		if (rte->rtekind == RTE_RELATION)
		{
			/* If it's a plain relation, reference this column */
			/* NB: this code works for whole-row Var with attno 0, too */
			add_object_address(OCLASS_CLASS, rte->relid, var->varattno,
							   &context->addrs);
		}
		else if (rte->rtekind == RTE_JOIN)
		{
			/* Scan join output column to add references to join inputs */
			List	   *save_rtables;

			/* We must make the context appropriate for join's level */
			save_rtables = context->rtables;
			context->rtables = rtables;
			if (var->varattno <= 0 ||
				var->varattno > length(rte->joinaliasvars))
				elog(ERROR, "invalid varattno %d", var->varattno);
			find_expr_references_walker((Node *) nth(var->varattno - 1,
													 rte->joinaliasvars),
										context);
			context->rtables = save_rtables;
		}
		return false;
	}
	if (IsA(node, FuncExpr))
	{
		FuncExpr   *funcexpr = (FuncExpr *) node;

		add_object_address(OCLASS_PROC, funcexpr->funcid, 0,
						   &context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) node;

		add_object_address(OCLASS_OPERATOR, opexpr->opno, 0,
						   &context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, DistinctExpr))
	{
		DistinctExpr *distinctexpr = (DistinctExpr *) node;

		add_object_address(OCLASS_OPERATOR, distinctexpr->opno, 0,
						   &context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;

		add_object_address(OCLASS_OPERATOR, opexpr->opno, 0,
						   &context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, NullIfExpr))
	{
		NullIfExpr *nullifexpr = (NullIfExpr *) node;

		add_object_address(OCLASS_OPERATOR, nullifexpr->opno, 0,
						   &context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;

		add_object_address(OCLASS_PROC, aggref->aggfnoid, 0,
						   &context->addrs);
		/* fall through to examine arguments */
	}
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		List	   *opid;

		foreach(opid, sublink->operOids)
		{
			add_object_address(OCLASS_OPERATOR, lfirsto(opid), 0,
							   &context->addrs);
		}
		/* fall through to examine arguments */
	}
	if (is_subplan(node))
	{
		/* Extra work needed here if we ever need this case */
		elog(ERROR, "already-planned subqueries not supported");
	}
	if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		Query	   *query = (Query *) node;
		List	   *rtable;
		bool		result;

		/*
		 * Add whole-relation refs for each plain relation mentioned in
		 * the subquery's rtable.  (Note: query_tree_walker takes care of
		 * recursing into RTE_FUNCTION and RTE_SUBQUERY RTEs, so no need
		 * to do that here.  But keep it from looking at join alias
		 * lists.)
		 */
		foreach(rtable, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(rtable);

			if (rte->rtekind == RTE_RELATION)
				add_object_address(OCLASS_CLASS, rte->relid, 0,
								   &context->addrs);
		}

		/* Examine substructure of query */
		context->rtables = lcons(query->rtable, context->rtables);
		result = query_tree_walker(query,
								   find_expr_references_walker,
								   (void *) context,
								   QTW_IGNORE_JOINALIASES);
		context->rtables = lnext(context->rtables);
		return result;
	}
	return expression_tree_walker(node, find_expr_references_walker,
								  (void *) context);
}

/*
 * Given an array of dependency references, eliminate any duplicates.
 */
static void
eliminate_duplicate_dependencies(ObjectAddresses *addrs)
{
	ObjectAddress *priorobj;
	int			oldref,
				newrefs;

	if (addrs->numrefs <= 1)
		return;					/* nothing to do */

	/* Sort the refs so that duplicates are adjacent */
	qsort((void *) addrs->refs, addrs->numrefs, sizeof(ObjectAddress),
		  object_address_comparator);

	/* Remove dups */
	priorobj = addrs->refs;
	newrefs = 1;
	for (oldref = 1; oldref < addrs->numrefs; oldref++)
	{
		ObjectAddress *thisobj = addrs->refs + oldref;

		if (priorobj->classId == thisobj->classId &&
			priorobj->objectId == thisobj->objectId)
		{
			if (priorobj->objectSubId == thisobj->objectSubId)
				continue;		/* identical, so drop thisobj */

			/*
			 * If we have a whole-object reference and a reference to a
			 * part of the same object, we don't need the whole-object
			 * reference (for example, we don't need to reference both
			 * table foo and column foo.bar).  The whole-object reference
			 * will always appear first in the sorted list.
			 */
			if (priorobj->objectSubId == 0)
			{
				/* replace whole ref with partial */
				priorobj->objectSubId = thisobj->objectSubId;
				continue;
			}
		}
		/* Not identical, so add thisobj to output set */
		priorobj++;
		priorobj->classId = thisobj->classId;
		priorobj->objectId = thisobj->objectId;
		priorobj->objectSubId = thisobj->objectSubId;
		newrefs++;
	}

	addrs->numrefs = newrefs;
}

/*
 * qsort comparator for ObjectAddress items
 */
static int
object_address_comparator(const void *a, const void *b)
{
	const ObjectAddress *obja = (const ObjectAddress *) a;
	const ObjectAddress *objb = (const ObjectAddress *) b;

	if (obja->classId < objb->classId)
		return -1;
	if (obja->classId > objb->classId)
		return 1;
	if (obja->objectId < objb->objectId)
		return -1;
	if (obja->objectId > objb->objectId)
		return 1;

	/*
	 * We sort the subId as an unsigned int so that 0 will come first. See
	 * logic in eliminate_duplicate_dependencies.
	 */
	if ((unsigned int) obja->objectSubId < (unsigned int) objb->objectSubId)
		return -1;
	if ((unsigned int) obja->objectSubId > (unsigned int) objb->objectSubId)
		return 1;
	return 0;
}

/*
 * Routines for handling an expansible array of ObjectAddress items.
 *
 * init_object_addresses: initialize an ObjectAddresses array.
 */
static void
init_object_addresses(ObjectAddresses *addrs)
{
	/* Initialize array to empty */
	addrs->numrefs = 0;
	addrs->maxrefs = 32;		/* arbitrary initial array size */
	addrs->refs = (ObjectAddress *)
		palloc(addrs->maxrefs * sizeof(ObjectAddress));

	/* Initialize object_classes[] if not done yet */
	/* This will be needed by add_object_address() */
	if (!object_classes_initialized)
		init_object_classes();
}

/*
 * Add an entry to an ObjectAddresses array.
 *
 * It is convenient to specify the class by ObjectClass rather than directly
 * by catalog OID.
 */
static void
add_object_address(ObjectClasses oclass, Oid objectId, int32 subId,
				   ObjectAddresses *addrs)
{
	ObjectAddress *item;

	/* enlarge array if needed */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
	}
	/* record this item */
	item = addrs->refs + addrs->numrefs;
	item->classId = object_classes[oclass];
	item->objectId = objectId;
	item->objectSubId = subId;
	addrs->numrefs++;
}

/*
 * Add an entry to an ObjectAddresses array.
 *
 * As above, but specify entry exactly.
 */
static void
add_exact_object_address(const ObjectAddress *object,
						 ObjectAddresses *addrs)
{
	ObjectAddress *item;

	/* enlarge array if needed */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
	}
	/* record this item */
	item = addrs->refs + addrs->numrefs;
	*item = *object;
	addrs->numrefs++;
}

/*
 * Test whether an object is present in an ObjectAddresses array.
 *
 * We return "true" if object is a subobject of something in the array, too.
 */
static bool
object_address_present(const ObjectAddress *object,
					   ObjectAddresses *addrs)
{
	int			i;

	for (i = addrs->numrefs - 1; i >= 0; i--)
	{
		ObjectAddress *thisobj = addrs->refs + i;

		if (object->classId == thisobj->classId &&
			object->objectId == thisobj->objectId)
		{
			if (object->objectSubId == thisobj->objectSubId ||
				thisobj->objectSubId == 0)
				return true;
		}
	}

	return false;
}

/*
 * Clean up when done with an ObjectAddresses array.
 */
static void
term_object_addresses(ObjectAddresses *addrs)
{
	pfree(addrs->refs);
}

/*
 * Initialize the object_classes[] table.
 *
 * Although some of these OIDs aren't compile-time constants, they surely
 * shouldn't change during a backend's run.  So, we look them up the
 * first time through and then cache them.
 */
static void
init_object_classes(void)
{
	object_classes[OCLASS_CLASS] = RelOid_pg_class;
	object_classes[OCLASS_PROC] = RelOid_pg_proc;
	object_classes[OCLASS_TYPE] = RelOid_pg_type;
	object_classes[OCLASS_CAST] = get_system_catalog_relid(CastRelationName);
	object_classes[OCLASS_CONSTRAINT] = get_system_catalog_relid(ConstraintRelationName);
	object_classes[OCLASS_CONVERSION] = get_system_catalog_relid(ConversionRelationName);
	object_classes[OCLASS_DEFAULT] = get_system_catalog_relid(AttrDefaultRelationName);
	object_classes[OCLASS_LANGUAGE] = get_system_catalog_relid(LanguageRelationName);
	object_classes[OCLASS_OPERATOR] = get_system_catalog_relid(OperatorRelationName);
	object_classes[OCLASS_OPCLASS] = get_system_catalog_relid(OperatorClassRelationName);
	object_classes[OCLASS_REWRITE] = get_system_catalog_relid(RewriteRelationName);
	object_classes[OCLASS_TRIGGER] = get_system_catalog_relid(TriggerRelationName);
	object_classes[OCLASS_SCHEMA] = get_system_catalog_relid(NamespaceRelationName);
	object_classes_initialized = true;
}

/*
 * Determine the class of a given object identified by objectAddress.
 *
 * This function is needed just because some of the system catalogs do
 * not have hardwired-at-compile-time OIDs.
 */
static ObjectClasses
getObjectClass(const ObjectAddress *object)
{
	/* Easy for the bootstrapped catalogs... */
	switch (object->classId)
	{
		case RelOid_pg_class:
			/* caller must check objectSubId */
			return OCLASS_CLASS;

		case RelOid_pg_proc:
			Assert(object->objectSubId == 0);
			return OCLASS_PROC;

		case RelOid_pg_type:
			Assert(object->objectSubId == 0);
			return OCLASS_TYPE;
	}

	/*
	 * Handle cases where catalog's OID is not hardwired.
	 */
	if (!object_classes_initialized)
		init_object_classes();

	if (object->classId == object_classes[OCLASS_CAST])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_CAST;
	}
	if (object->classId == object_classes[OCLASS_CONSTRAINT])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_CONSTRAINT;
	}
	if (object->classId == object_classes[OCLASS_CONVERSION])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_CONVERSION;
	}
	if (object->classId == object_classes[OCLASS_DEFAULT])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_DEFAULT;
	}
	if (object->classId == object_classes[OCLASS_LANGUAGE])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_LANGUAGE;
	}
	if (object->classId == object_classes[OCLASS_OPERATOR])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_OPERATOR;
	}
	if (object->classId == object_classes[OCLASS_OPCLASS])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_OPCLASS;
	}
	if (object->classId == object_classes[OCLASS_REWRITE])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_REWRITE;
	}
	if (object->classId == object_classes[OCLASS_TRIGGER])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_TRIGGER;
	}
	if (object->classId == object_classes[OCLASS_SCHEMA])
	{
		Assert(object->objectSubId == 0);
		return OCLASS_SCHEMA;
	}

	elog(ERROR, "unrecognized object class: %u", object->classId);
	return OCLASS_CLASS;		/* keep compiler quiet */
}

/*
 * getObjectDescription: build an object description for messages
 *
 * The result is a palloc'd string.
 */
static char *
getObjectDescription(const ObjectAddress *object)
{
	StringInfoData buffer;

	initStringInfo(&buffer);

	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			getRelationDescription(&buffer, object->objectId);
			if (object->objectSubId != 0)
				appendStringInfo(&buffer, " column %s",
								 get_relid_attribute_name(object->objectId,
														  object->objectSubId));
			break;

		case OCLASS_PROC:
			appendStringInfo(&buffer, "function %s",
							 format_procedure(object->objectId));
			break;

		case OCLASS_TYPE:
			appendStringInfo(&buffer, "type %s",
							 format_type_be(object->objectId));
			break;

		case OCLASS_CAST:
			{
				Relation	castDesc;
				ScanKeyData skey[1];
				SysScanDesc rcscan;
				HeapTuple	tup;
				Form_pg_cast castForm;

				castDesc = heap_openr(CastRelationName, AccessShareLock);

				ScanKeyEntryInitialize(&skey[0], 0x0,
									   ObjectIdAttributeNumber, F_OIDEQ,
									 ObjectIdGetDatum(object->objectId));

				rcscan = systable_beginscan(castDesc, CastOidIndex, true,
											SnapshotNow, 1, skey);

				tup = systable_getnext(rcscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for cast %u",
						 object->objectId);

				castForm = (Form_pg_cast) GETSTRUCT(tup);

				appendStringInfo(&buffer, "cast from %s to %s",
								 format_type_be(castForm->castsource),
								 format_type_be(castForm->casttarget));

				systable_endscan(rcscan);
				heap_close(castDesc, AccessShareLock);
				break;
			}

		case OCLASS_CONSTRAINT:
			{
				Relation	conDesc;
				ScanKeyData skey[1];
				SysScanDesc rcscan;
				HeapTuple	tup;
				Form_pg_constraint con;

				conDesc = heap_openr(ConstraintRelationName, AccessShareLock);

				ScanKeyEntryInitialize(&skey[0], 0x0,
									   ObjectIdAttributeNumber, F_OIDEQ,
									 ObjectIdGetDatum(object->objectId));

				rcscan = systable_beginscan(conDesc, ConstraintOidIndex, true,
											SnapshotNow, 1, skey);

				tup = systable_getnext(rcscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for constraint %u",
						 object->objectId);

				con = (Form_pg_constraint) GETSTRUCT(tup);

				if (OidIsValid(con->conrelid))
				{
					appendStringInfo(&buffer, "constraint %s on ",
									 NameStr(con->conname));
					getRelationDescription(&buffer, con->conrelid);
				}
				else
				{
					appendStringInfo(&buffer, "constraint %s",
									 NameStr(con->conname));
				}

				systable_endscan(rcscan);
				heap_close(conDesc, AccessShareLock);
				break;
			}

		case OCLASS_CONVERSION:
			{
				HeapTuple	conTup;

				conTup = SearchSysCache(CONOID,
									  ObjectIdGetDatum(object->objectId),
										0, 0, 0);
				if (!HeapTupleIsValid(conTup))
					elog(ERROR, "cache lookup failed for conversion %u",
						 object->objectId);
				appendStringInfo(&buffer, "conversion %s",
								 NameStr(((Form_pg_conversion) GETSTRUCT(conTup))->conname));
				ReleaseSysCache(conTup);
				break;
			}

		case OCLASS_DEFAULT:
			{
				Relation	attrdefDesc;
				ScanKeyData skey[1];
				SysScanDesc adscan;
				HeapTuple	tup;
				Form_pg_attrdef attrdef;
				ObjectAddress colobject;

				attrdefDesc = heap_openr(AttrDefaultRelationName, AccessShareLock);

				ScanKeyEntryInitialize(&skey[0], 0x0,
									   ObjectIdAttributeNumber, F_OIDEQ,
									 ObjectIdGetDatum(object->objectId));

				adscan = systable_beginscan(attrdefDesc, AttrDefaultOidIndex, true,
											SnapshotNow, 1, skey);

				tup = systable_getnext(adscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for attrdef %u",
						 object->objectId);

				attrdef = (Form_pg_attrdef) GETSTRUCT(tup);

				colobject.classId = RelOid_pg_class;
				colobject.objectId = attrdef->adrelid;
				colobject.objectSubId = attrdef->adnum;

				appendStringInfo(&buffer, "default for %s",
								 getObjectDescription(&colobject));

				systable_endscan(adscan);
				heap_close(attrdefDesc, AccessShareLock);
				break;
			}

		case OCLASS_LANGUAGE:
			{
				HeapTuple	langTup;

				langTup = SearchSysCache(LANGOID,
									  ObjectIdGetDatum(object->objectId),
										 0, 0, 0);
				if (!HeapTupleIsValid(langTup))
					elog(ERROR, "cache lookup failed for language %u",
						 object->objectId);
				appendStringInfo(&buffer, "language %s",
								 NameStr(((Form_pg_language) GETSTRUCT(langTup))->lanname));
				ReleaseSysCache(langTup);
				break;
			}

		case OCLASS_OPERATOR:
			appendStringInfo(&buffer, "operator %s",
							 format_operator(object->objectId));
			break;

		case OCLASS_OPCLASS:
			{
				HeapTuple	opcTup;
				Form_pg_opclass opcForm;
				HeapTuple	amTup;
				Form_pg_am	amForm;
				char	   *nspname;

				opcTup = SearchSysCache(CLAOID,
									  ObjectIdGetDatum(object->objectId),
										0, 0, 0);
				if (!HeapTupleIsValid(opcTup))
					elog(ERROR, "cache lookup failed for opclass %u",
						 object->objectId);
				opcForm = (Form_pg_opclass) GETSTRUCT(opcTup);

				/* Qualify the name if not visible in search path */
				if (OpclassIsVisible(object->objectId))
					nspname = NULL;
				else
					nspname = get_namespace_name(opcForm->opcnamespace);

				appendStringInfo(&buffer, "operator class %s",
								 quote_qualified_identifier(nspname,
											 NameStr(opcForm->opcname)));

				amTup = SearchSysCache(AMOID,
									   ObjectIdGetDatum(opcForm->opcamid),
									   0, 0, 0);
				if (!HeapTupleIsValid(amTup))
					elog(ERROR, "cache lookup failed for access method %u",
						 opcForm->opcamid);
				amForm = (Form_pg_am) GETSTRUCT(amTup);

				appendStringInfo(&buffer, " for %s",
								 NameStr(amForm->amname));

				ReleaseSysCache(amTup);
				ReleaseSysCache(opcTup);
				break;
			}

		case OCLASS_REWRITE:
			{
				Relation	ruleDesc;
				ScanKeyData skey[1];
				SysScanDesc rcscan;
				HeapTuple	tup;
				Form_pg_rewrite rule;

				ruleDesc = heap_openr(RewriteRelationName, AccessShareLock);

				ScanKeyEntryInitialize(&skey[0], 0x0,
									   ObjectIdAttributeNumber, F_OIDEQ,
									 ObjectIdGetDatum(object->objectId));

				rcscan = systable_beginscan(ruleDesc, RewriteOidIndex, true,
											SnapshotNow, 1, skey);

				tup = systable_getnext(rcscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for rule %u",
						 object->objectId);

				rule = (Form_pg_rewrite) GETSTRUCT(tup);

				appendStringInfo(&buffer, "rule %s on ",
								 NameStr(rule->rulename));
				getRelationDescription(&buffer, rule->ev_class);

				systable_endscan(rcscan);
				heap_close(ruleDesc, AccessShareLock);
				break;
			}

		case OCLASS_TRIGGER:
			{
				Relation	trigDesc;
				ScanKeyData skey[1];
				SysScanDesc tgscan;
				HeapTuple	tup;
				Form_pg_trigger trig;

				trigDesc = heap_openr(TriggerRelationName, AccessShareLock);

				ScanKeyEntryInitialize(&skey[0], 0x0,
									   ObjectIdAttributeNumber, F_OIDEQ,
									 ObjectIdGetDatum(object->objectId));

				tgscan = systable_beginscan(trigDesc, TriggerOidIndex, true,
											SnapshotNow, 1, skey);

				tup = systable_getnext(tgscan);

				if (!HeapTupleIsValid(tup))
					elog(ERROR, "could not find tuple for trigger %u",
						 object->objectId);

				trig = (Form_pg_trigger) GETSTRUCT(tup);

				appendStringInfo(&buffer, "trigger %s on ",
								 NameStr(trig->tgname));
				getRelationDescription(&buffer, trig->tgrelid);

				systable_endscan(tgscan);
				heap_close(trigDesc, AccessShareLock);
				break;
			}

		case OCLASS_SCHEMA:
			{
				char	   *nspname;

				nspname = get_namespace_name(object->objectId);
				if (!nspname)
					elog(ERROR, "cache lookup failed for namespace %u",
						 object->objectId);
				appendStringInfo(&buffer, "schema %s", nspname);
				break;
			}

		default:
			appendStringInfo(&buffer, "unrecognized object %u %u %d",
							 object->classId,
							 object->objectId,
							 object->objectSubId);
			break;
	}

	return buffer.data;
}

/*
 * subroutine for getObjectDescription: describe a relation
 */
static void
getRelationDescription(StringInfo buffer, Oid relid)
{
	HeapTuple	relTup;
	Form_pg_class relForm;
	char	   *nspname;
	char	   *relname;

	relTup = SearchSysCache(RELOID,
							ObjectIdGetDatum(relid),
							0, 0, 0);
	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	relForm = (Form_pg_class) GETSTRUCT(relTup);

	/* Qualify the name if not visible in search path */
	if (RelationIsVisible(relid))
		nspname = NULL;
	else
		nspname = get_namespace_name(relForm->relnamespace);

	relname = quote_qualified_identifier(nspname, NameStr(relForm->relname));

	switch (relForm->relkind)
	{
		case RELKIND_RELATION:
			appendStringInfo(buffer, "table %s",
							 relname);
			break;
		case RELKIND_INDEX:
			appendStringInfo(buffer, "index %s",
							 relname);
			break;
		case RELKIND_SPECIAL:
			appendStringInfo(buffer, "special system relation %s",
							 relname);
			break;
		case RELKIND_SEQUENCE:
			appendStringInfo(buffer, "sequence %s",
							 relname);
			break;
		case RELKIND_UNCATALOGED:
			appendStringInfo(buffer, "uncataloged table %s",
							 relname);
			break;
		case RELKIND_TOASTVALUE:
			appendStringInfo(buffer, "toast table %s",
							 relname);
			break;
		case RELKIND_VIEW:
			appendStringInfo(buffer, "view %s",
							 relname);
			break;
		case RELKIND_COMPOSITE_TYPE:
			appendStringInfo(buffer, "composite type %s",
							 relname);
			break;
		default:
			/* shouldn't get here */
			appendStringInfo(buffer, "relation %s",
							 relname);
			break;
	}

	ReleaseSysCache(relTup);
}
