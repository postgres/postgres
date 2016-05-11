/*-------------------------------------------------------------------------
 *
 * dependency.c
 *	  Routines to support inter-object dependencies.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/dependency.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_collation_fn.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_constraint_fn.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_conversion_fn.h"
#include "catalog/pg_database.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/extension.h"
#include "commands/policy.h"
#include "commands/proclang.h"
#include "commands/schemacmds.h"
#include "commands/seclabel.h"
#include "commands/trigger.h"
#include "commands/typecmds.h"
#include "nodes/nodeFuncs.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteRemove.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/*
 * Deletion processing requires additional state for each ObjectAddress that
 * it's planning to delete.  For simplicity and code-sharing we make the
 * ObjectAddresses code support arrays with or without this extra state.
 */
typedef struct
{
	int			flags;			/* bitmask, see bit definitions below */
	ObjectAddress dependee;		/* object whose deletion forced this one */
} ObjectAddressExtra;

/* ObjectAddressExtra flag bits */
#define DEPFLAG_ORIGINAL	0x0001		/* an original deletion target */
#define DEPFLAG_NORMAL		0x0002		/* reached via normal dependency */
#define DEPFLAG_AUTO		0x0004		/* reached via auto dependency */
#define DEPFLAG_INTERNAL	0x0008		/* reached via internal dependency */
#define DEPFLAG_EXTENSION	0x0010		/* reached via extension dependency */
#define DEPFLAG_REVERSE		0x0020		/* reverse internal/extension link */


/* expansible list of ObjectAddresses */
struct ObjectAddresses
{
	ObjectAddress *refs;		/* => palloc'd array */
	ObjectAddressExtra *extras; /* => palloc'd array, or NULL if not used */
	int			numrefs;		/* current number of references */
	int			maxrefs;		/* current size of palloc'd array(s) */
};

/* typedef ObjectAddresses appears in dependency.h */

/* threaded list of ObjectAddresses, for recursion detection */
typedef struct ObjectAddressStack
{
	const ObjectAddress *object;	/* object being visited */
	int			flags;			/* its current flag bits */
	struct ObjectAddressStack *next;	/* next outer stack level */
} ObjectAddressStack;

/* for find_expr_references_walker */
typedef struct
{
	ObjectAddresses *addrs;		/* addresses being accumulated */
	List	   *rtables;		/* list of rangetables to resolve Vars */
} find_expr_references_context;

/*
 * This constant table maps ObjectClasses to the corresponding catalog OIDs.
 * See also getObjectClass().
 */
static const Oid object_classes[] = {
	RelationRelationId,			/* OCLASS_CLASS */
	ProcedureRelationId,		/* OCLASS_PROC */
	TypeRelationId,				/* OCLASS_TYPE */
	CastRelationId,				/* OCLASS_CAST */
	CollationRelationId,		/* OCLASS_COLLATION */
	ConstraintRelationId,		/* OCLASS_CONSTRAINT */
	ConversionRelationId,		/* OCLASS_CONVERSION */
	AttrDefaultRelationId,		/* OCLASS_DEFAULT */
	LanguageRelationId,			/* OCLASS_LANGUAGE */
	LargeObjectRelationId,		/* OCLASS_LARGEOBJECT */
	OperatorRelationId,			/* OCLASS_OPERATOR */
	OperatorClassRelationId,	/* OCLASS_OPCLASS */
	OperatorFamilyRelationId,	/* OCLASS_OPFAMILY */
	AccessMethodRelationId,		/* OCLASS_AM */
	AccessMethodOperatorRelationId,		/* OCLASS_AMOP */
	AccessMethodProcedureRelationId,	/* OCLASS_AMPROC */
	RewriteRelationId,			/* OCLASS_REWRITE */
	TriggerRelationId,			/* OCLASS_TRIGGER */
	NamespaceRelationId,		/* OCLASS_SCHEMA */
	TSParserRelationId,			/* OCLASS_TSPARSER */
	TSDictionaryRelationId,		/* OCLASS_TSDICT */
	TSTemplateRelationId,		/* OCLASS_TSTEMPLATE */
	TSConfigRelationId,			/* OCLASS_TSCONFIG */
	AuthIdRelationId,			/* OCLASS_ROLE */
	DatabaseRelationId,			/* OCLASS_DATABASE */
	TableSpaceRelationId,		/* OCLASS_TBLSPACE */
	ForeignDataWrapperRelationId,		/* OCLASS_FDW */
	ForeignServerRelationId,	/* OCLASS_FOREIGN_SERVER */
	UserMappingRelationId,		/* OCLASS_USER_MAPPING */
	DefaultAclRelationId,		/* OCLASS_DEFACL */
	ExtensionRelationId,		/* OCLASS_EXTENSION */
	EventTriggerRelationId,		/* OCLASS_EVENT_TRIGGER */
	PolicyRelationId,			/* OCLASS_POLICY */
	TransformRelationId			/* OCLASS_TRANSFORM */
};


static void findDependentObjects(const ObjectAddress *object,
					 int flags,
					 ObjectAddressStack *stack,
					 ObjectAddresses *targetObjects,
					 const ObjectAddresses *pendingObjects,
					 Relation *depRel);
static void reportDependentObjects(const ObjectAddresses *targetObjects,
					   DropBehavior behavior,
					   int msglevel,
					   const ObjectAddress *origObject);
static void deleteOneObject(const ObjectAddress *object,
				Relation *depRel, int32 flags);
static void doDeletion(const ObjectAddress *object, int flags);
static void AcquireDeletionLock(const ObjectAddress *object, int flags);
static void ReleaseDeletionLock(const ObjectAddress *object);
static bool find_expr_references_walker(Node *node,
							find_expr_references_context *context);
static void eliminate_duplicate_dependencies(ObjectAddresses *addrs);
static int	object_address_comparator(const void *a, const void *b);
static void add_object_address(ObjectClass oclass, Oid objectId, int32 subId,
				   ObjectAddresses *addrs);
static void add_exact_object_address_extra(const ObjectAddress *object,
							   const ObjectAddressExtra *extra,
							   ObjectAddresses *addrs);
static bool object_address_present_add_flags(const ObjectAddress *object,
								 int flags,
								 ObjectAddresses *addrs);
static bool stack_address_present_add_flags(const ObjectAddress *object,
								int flags,
								ObjectAddressStack *stack);
static void DeleteInitPrivs(const ObjectAddress *object);


/*
 * Go through the objects given running the final actions on them, and execute
 * the actual deletion.
 */
static void
deleteObjectsInList(ObjectAddresses *targetObjects, Relation *depRel,
					int flags)
{
	int			i;

	/*
	 * Keep track of objects for event triggers, if necessary.
	 */
	if (trackDroppedObjectsNeeded() && !(flags & PERFORM_DELETION_INTERNAL))
	{
		for (i = 0; i < targetObjects->numrefs; i++)
		{
			const ObjectAddress *thisobj = &targetObjects->refs[i];
			const ObjectAddressExtra *extra = &targetObjects->extras[i];
			bool		original = false;
			bool		normal = false;

			if (extra->flags & DEPFLAG_ORIGINAL)
				original = true;
			if (extra->flags & DEPFLAG_NORMAL)
				normal = true;
			if (extra->flags & DEPFLAG_REVERSE)
				normal = true;

			if (EventTriggerSupportsObjectClass(getObjectClass(thisobj)))
			{
				EventTriggerSQLDropAddObject(thisobj, original, normal);
			}
		}
	}

	/*
	 * Delete all the objects in the proper order.
	 */
	for (i = 0; i < targetObjects->numrefs; i++)
	{
		ObjectAddress *thisobj = targetObjects->refs + i;

		deleteOneObject(thisobj, depRel, flags);
	}
}

/*
 * performDeletion: attempt to drop the specified object.  If CASCADE
 * behavior is specified, also drop any dependent objects (recursively).
 * If RESTRICT behavior is specified, error out if there are any dependent
 * objects, except for those that should be implicitly dropped anyway
 * according to the dependency type.
 *
 * This is the outer control routine for all forms of DROP that drop objects
 * that can participate in dependencies.  Note that the next two routines
 * are variants on the same theme; if you change anything here you'll likely
 * need to fix them too.
 *
 * flags should include PERFORM_DELETION_INTERNAL when the drop operation is
 * not the direct result of a user-initiated action.  For example, when a
 * temporary schema is cleaned out so that a new backend can use it, or when
 * a column default is dropped as an intermediate step while adding a new one,
 * that's an internal operation.  On the other hand, when we drop something
 * because the user issued a DROP statement against it, that's not internal.
 */
void
performDeletion(const ObjectAddress *object,
				DropBehavior behavior, int flags)
{
	Relation	depRel;
	ObjectAddresses *targetObjects;

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Acquire deletion lock on the target object.  (Ideally the caller has
	 * done this already, but many places are sloppy about it.)
	 */
	AcquireDeletionLock(object, 0);

	/*
	 * Construct a list of objects to delete (ie, the given object plus
	 * everything directly or indirectly dependent on it).
	 */
	targetObjects = new_object_addresses();

	findDependentObjects(object,
						 DEPFLAG_ORIGINAL,
						 NULL,	/* empty stack */
						 targetObjects,
						 NULL,	/* no pendingObjects */
						 &depRel);

	/*
	 * Check if deletion is allowed, and report about cascaded deletes.
	 */
	reportDependentObjects(targetObjects,
						   behavior,
						   NOTICE,
						   object);

	/* do the deed */
	deleteObjectsInList(targetObjects, &depRel, flags);

	/* And clean up */
	free_object_addresses(targetObjects);

	heap_close(depRel, RowExclusiveLock);
}

/*
 * performMultipleDeletions: Similar to performDeletion, but act on multiple
 * objects at once.
 *
 * The main difference from issuing multiple performDeletion calls is that the
 * list of objects that would be implicitly dropped, for each object to be
 * dropped, is the union of the implicit-object list for all objects.  This
 * makes each check be more relaxed.
 */
void
performMultipleDeletions(const ObjectAddresses *objects,
						 DropBehavior behavior, int flags)
{
	Relation	depRel;
	ObjectAddresses *targetObjects;
	int			i;

	/* No work if no objects... */
	if (objects->numrefs <= 0)
		return;

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Construct a list of objects to delete (ie, the given objects plus
	 * everything directly or indirectly dependent on them).  Note that
	 * because we pass the whole objects list as pendingObjects context, we
	 * won't get a failure from trying to delete an object that is internally
	 * dependent on another one in the list; we'll just skip that object and
	 * delete it when we reach its owner.
	 */
	targetObjects = new_object_addresses();

	for (i = 0; i < objects->numrefs; i++)
	{
		const ObjectAddress *thisobj = objects->refs + i;

		/*
		 * Acquire deletion lock on each target object.  (Ideally the caller
		 * has done this already, but many places are sloppy about it.)
		 */
		AcquireDeletionLock(thisobj, flags);

		findDependentObjects(thisobj,
							 DEPFLAG_ORIGINAL,
							 NULL,		/* empty stack */
							 targetObjects,
							 objects,
							 &depRel);
	}

	/*
	 * Check if deletion is allowed, and report about cascaded deletes.
	 *
	 * If there's exactly one object being deleted, report it the same way as
	 * in performDeletion(), else we have to be vaguer.
	 */
	reportDependentObjects(targetObjects,
						   behavior,
						   NOTICE,
						   (objects->numrefs == 1 ? objects->refs : NULL));

	/* do the deed */
	deleteObjectsInList(targetObjects, &depRel, flags);

	/* And clean up */
	free_object_addresses(targetObjects);

	heap_close(depRel, RowExclusiveLock);
}

/*
 * deleteWhatDependsOn: attempt to drop everything that depends on the
 * specified object, though not the object itself.  Behavior is always
 * CASCADE.
 *
 * This is currently used only to clean out the contents of a schema
 * (namespace): the passed object is a namespace.  We normally want this
 * to be done silently, so there's an option to suppress NOTICE messages.
 *
 * Note we don't fire object drop event triggers here; it would be wrong to do
 * so for the current only use of this function, but if more callers are added
 * this might need to be reconsidered.
 */
void
deleteWhatDependsOn(const ObjectAddress *object,
					bool showNotices)
{
	Relation	depRel;
	ObjectAddresses *targetObjects;
	int			i;

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Acquire deletion lock on the target object.  (Ideally the caller has
	 * done this already, but many places are sloppy about it.)
	 */
	AcquireDeletionLock(object, 0);

	/*
	 * Construct a list of objects to delete (ie, the given object plus
	 * everything directly or indirectly dependent on it).
	 */
	targetObjects = new_object_addresses();

	findDependentObjects(object,
						 DEPFLAG_ORIGINAL,
						 NULL,	/* empty stack */
						 targetObjects,
						 NULL,	/* no pendingObjects */
						 &depRel);

	/*
	 * Check if deletion is allowed, and report about cascaded deletes.
	 */
	reportDependentObjects(targetObjects,
						   DROP_CASCADE,
						   showNotices ? NOTICE : DEBUG2,
						   object);

	/*
	 * Delete all the objects in the proper order, except we skip the original
	 * object.
	 */
	for (i = 0; i < targetObjects->numrefs; i++)
	{
		ObjectAddress *thisobj = targetObjects->refs + i;
		ObjectAddressExtra *thisextra = targetObjects->extras + i;

		if (thisextra->flags & DEPFLAG_ORIGINAL)
			continue;

		/*
		 * Since this function is currently only used to clean out temporary
		 * schemas, we pass PERFORM_DELETION_INTERNAL here, indicating that
		 * the operation is an automatic system operation rather than a user
		 * action.  If, in the future, this function is used for other
		 * purposes, we might need to revisit this.
		 */
		deleteOneObject(thisobj, &depRel, PERFORM_DELETION_INTERNAL);
	}

	/* And clean up */
	free_object_addresses(targetObjects);

	heap_close(depRel, RowExclusiveLock);
}

/*
 * findDependentObjects - find all objects that depend on 'object'
 *
 * For every object that depends on the starting object, acquire a deletion
 * lock on the object, add it to targetObjects (if not already there),
 * and recursively find objects that depend on it.  An object's dependencies
 * will be placed into targetObjects before the object itself; this means
 * that the finished list's order represents a safe deletion order.
 *
 * The caller must already have a deletion lock on 'object' itself,
 * but must not have added it to targetObjects.  (Note: there are corner
 * cases where we won't add the object either, and will also release the
 * caller-taken lock.  This is a bit ugly, but the API is set up this way
 * to allow easy rechecking of an object's liveness after we lock it.  See
 * notes within the function.)
 *
 * When dropping a whole object (subId = 0), we find dependencies for
 * its sub-objects too.
 *
 *	object: the object to add to targetObjects and find dependencies on
 *	flags: flags to be ORed into the object's targetObjects entry
 *	stack: list of objects being visited in current recursion; topmost item
 *			is the object that we recursed from (NULL for external callers)
 *	targetObjects: list of objects that are scheduled to be deleted
 *	pendingObjects: list of other objects slated for destruction, but
 *			not necessarily in targetObjects yet (can be NULL if none)
 *	*depRel: already opened pg_depend relation
 */
static void
findDependentObjects(const ObjectAddress *object,
					 int flags,
					 ObjectAddressStack *stack,
					 ObjectAddresses *targetObjects,
					 const ObjectAddresses *pendingObjects,
					 Relation *depRel)
{
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;
	ObjectAddress otherObject;
	ObjectAddressStack mystack;
	ObjectAddressExtra extra;

	/*
	 * If the target object is already being visited in an outer recursion
	 * level, just report the current flags back to that level and exit. This
	 * is needed to avoid infinite recursion in the face of circular
	 * dependencies.
	 *
	 * The stack check alone would result in dependency loops being broken at
	 * an arbitrary point, ie, the first member object of the loop to be
	 * visited is the last one to be deleted.  This is obviously unworkable.
	 * However, the check for internal dependency below guarantees that we
	 * will not break a loop at an internal dependency: if we enter the loop
	 * at an "owned" object we will switch and start at the "owning" object
	 * instead.  We could probably hack something up to avoid breaking at an
	 * auto dependency, too, if we had to.  However there are no known cases
	 * where that would be necessary.
	 */
	if (stack_address_present_add_flags(object, flags, stack))
		return;

	/*
	 * It's also possible that the target object has already been completely
	 * processed and put into targetObjects.  If so, again we just add the
	 * specified flags to its entry and return.
	 *
	 * (Note: in these early-exit cases we could release the caller-taken
	 * lock, since the object is presumably now locked multiple times; but it
	 * seems not worth the cycles.)
	 */
	if (object_address_present_add_flags(object, flags, targetObjects))
		return;

	/*
	 * The target object might be internally dependent on some other object
	 * (its "owner"), and/or be a member of an extension (also considered its
	 * owner).  If so, and if we aren't recursing from the owning object, we
	 * have to transform this deletion request into a deletion request of the
	 * owning object.  (We'll eventually recurse back to this object, but the
	 * owning object has to be visited first so it will be deleted after.) The
	 * way to find out about this is to scan the pg_depend entries that show
	 * what this object depends on.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&key[2],
					Anum_pg_depend_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(*depRel, DependDependerIndexId, true,
							  NULL, nkeys, key);

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
			case DEPENDENCY_AUTO_EXTENSION:
				/* no problem */
				break;
			case DEPENDENCY_INTERNAL:
			case DEPENDENCY_EXTENSION:

				/*
				 * This object is part of the internal implementation of
				 * another object, or is part of the extension that is the
				 * other object.  We have three cases:
				 *
				 * 1. At the outermost recursion level, we normally disallow
				 * the DROP.  (We just ereport here, rather than proceeding,
				 * since no other dependencies are likely to be interesting.)
				 * However, there are exceptions.
				 */
				if (stack == NULL)
				{
					char	   *otherObjDesc;

					/*
					 * Exception 1a: if the owning object is listed in
					 * pendingObjects, just release the caller's lock and
					 * return.  We'll eventually complete the DROP when we
					 * reach that entry in the pending list.
					 */
					if (pendingObjects &&
						object_address_present(&otherObject, pendingObjects))
					{
						systable_endscan(scan);
						/* need to release caller's lock; see notes below */
						ReleaseDeletionLock(object);
						return;
					}

					/*
					 * Exception 1b: if the owning object is the extension
					 * currently being created/altered, it's okay to continue
					 * with the deletion.  This allows dropping of an
					 * extension's objects within the extension's scripts, as
					 * well as corner cases such as dropping a transient
					 * object created within such a script.
					 */
					if (creating_extension &&
						otherObject.classId == ExtensionRelationId &&
						otherObject.objectId == CurrentExtensionObject)
						break;

					/* No exception applies, so throw the error */
					otherObjDesc = getObjectDescription(&otherObject);
					ereport(ERROR,
							(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
							 errmsg("cannot drop %s because %s requires it",
									getObjectDescription(object),
									otherObjDesc),
							 errhint("You can drop %s instead.",
									 otherObjDesc)));
				}

				/*
				 * 2. When recursing from the other end of this dependency,
				 * it's okay to continue with the deletion.  This holds when
				 * recursing from a whole object that includes the nominal
				 * other end as a component, too.  Since there can be more
				 * than one "owning" object, we have to allow matches that are
				 * more than one level down in the stack.
				 */
				if (stack_address_present_add_flags(&otherObject, 0, stack))
					break;

				/*
				 * 3. Not all the owning objects have been visited, so
				 * transform this deletion request into a delete of this
				 * owning object.
				 *
				 * First, release caller's lock on this object and get
				 * deletion lock on the owning object.  (We must release
				 * caller's lock to avoid deadlock against a concurrent
				 * deletion of the owning object.)
				 */
				ReleaseDeletionLock(object);
				AcquireDeletionLock(&otherObject, 0);

				/*
				 * The owning object might have been deleted while we waited
				 * to lock it; if so, neither it nor the current object are
				 * interesting anymore.  We test this by checking the
				 * pg_depend entry (see notes below).
				 */
				if (!systable_recheck_tuple(scan, tup))
				{
					systable_endscan(scan);
					ReleaseDeletionLock(&otherObject);
					return;
				}

				/*
				 * Okay, recurse to the owning object instead of proceeding.
				 *
				 * We do not need to stack the current object; we want the
				 * traversal order to be as if the original reference had
				 * linked to the owning object instead of this one.
				 *
				 * The dependency type is a "reverse" dependency: we need to
				 * delete the owning object if this one is to be deleted, but
				 * this linkage is never a reason for an automatic deletion.
				 */
				findDependentObjects(&otherObject,
									 DEPFLAG_REVERSE,
									 stack,
									 targetObjects,
									 pendingObjects,
									 depRel);
				/* And we're done here. */
				systable_endscan(scan);
				return;
			case DEPENDENCY_PIN:

				/*
				 * Should not happen; PIN dependencies should have zeroes in
				 * the depender fields...
				 */
				elog(ERROR, "incorrect use of PIN dependency with %s",
					 getObjectDescription(object));
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, getObjectDescription(object));
				break;
		}
	}

	systable_endscan(scan);

	/*
	 * Now recurse to any dependent objects.  We must visit them first since
	 * they have to be deleted before the current object.
	 */
	mystack.object = object;	/* set up a new stack level */
	mystack.flags = flags;
	mystack.next = stack;

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&key[2],
					Anum_pg_depend_refobjsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(*depRel, DependReferenceIndexId, true,
							  NULL, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);
		int			subflags;

		otherObject.classId = foundDep->classid;
		otherObject.objectId = foundDep->objid;
		otherObject.objectSubId = foundDep->objsubid;

		/*
		 * Must lock the dependent object before recursing to it.
		 */
		AcquireDeletionLock(&otherObject, 0);

		/*
		 * The dependent object might have been deleted while we waited to
		 * lock it; if so, we don't need to do anything more with it. We can
		 * test this cheaply and independently of the object's type by seeing
		 * if the pg_depend tuple we are looking at is still live. (If the
		 * object got deleted, the tuple would have been deleted too.)
		 */
		if (!systable_recheck_tuple(scan, tup))
		{
			/* release the now-useless lock */
			ReleaseDeletionLock(&otherObject);
			/* and continue scanning for dependencies */
			continue;
		}

		/* Recurse, passing flags indicating the dependency type */
		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:
				subflags = DEPFLAG_NORMAL;
				break;
			case DEPENDENCY_AUTO:
			case DEPENDENCY_AUTO_EXTENSION:
				subflags = DEPFLAG_AUTO;
				break;
			case DEPENDENCY_INTERNAL:
				subflags = DEPFLAG_INTERNAL;
				break;
			case DEPENDENCY_EXTENSION:
				subflags = DEPFLAG_EXTENSION;
				break;
			case DEPENDENCY_PIN:

				/*
				 * For a PIN dependency we just ereport immediately; there
				 * won't be any others to report.
				 */
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot drop %s because it is required by the database system",
								getObjectDescription(object))));
				subflags = 0;	/* keep compiler quiet */
				break;
			default:
				elog(ERROR, "unrecognized dependency type '%c' for %s",
					 foundDep->deptype, getObjectDescription(object));
				subflags = 0;	/* keep compiler quiet */
				break;
		}

		findDependentObjects(&otherObject,
							 subflags,
							 &mystack,
							 targetObjects,
							 pendingObjects,
							 depRel);
	}

	systable_endscan(scan);

	/*
	 * Finally, we can add the target object to targetObjects.  Be careful to
	 * include any flags that were passed back down to us from inner recursion
	 * levels.
	 */
	extra.flags = mystack.flags;
	if (stack)
		extra.dependee = *stack->object;
	else
		memset(&extra.dependee, 0, sizeof(extra.dependee));
	add_exact_object_address_extra(object, &extra, targetObjects);
}

/*
 * reportDependentObjects - report about dependencies, and fail if RESTRICT
 *
 * Tell the user about dependent objects that we are going to delete
 * (or would need to delete, but are prevented by RESTRICT mode);
 * then error out if there are any and it's not CASCADE mode.
 *
 *	targetObjects: list of objects that are scheduled to be deleted
 *	behavior: RESTRICT or CASCADE
 *	msglevel: elog level for non-error report messages
 *	origObject: base object of deletion, or NULL if not available
 *		(the latter case occurs in DROP OWNED)
 */
static void
reportDependentObjects(const ObjectAddresses *targetObjects,
					   DropBehavior behavior,
					   int msglevel,
					   const ObjectAddress *origObject)
{
	bool		ok = true;
	StringInfoData clientdetail;
	StringInfoData logdetail;
	int			numReportedClient = 0;
	int			numNotReportedClient = 0;
	int			i;

	/*
	 * If no error is to be thrown, and the msglevel is too low to be shown to
	 * either client or server log, there's no need to do any of the work.
	 *
	 * Note: this code doesn't know all there is to be known about elog
	 * levels, but it works for NOTICE and DEBUG2, which are the only values
	 * msglevel can currently have.  We also assume we are running in a normal
	 * operating environment.
	 */
	if (behavior == DROP_CASCADE &&
		msglevel < client_min_messages &&
		(msglevel < log_min_messages || log_min_messages == LOG))
		return;

	/*
	 * We limit the number of dependencies reported to the client to
	 * MAX_REPORTED_DEPS, since client software may not deal well with
	 * enormous error strings.  The server log always gets a full report.
	 */
#define MAX_REPORTED_DEPS 100

	initStringInfo(&clientdetail);
	initStringInfo(&logdetail);

	/*
	 * We process the list back to front (ie, in dependency order not deletion
	 * order), since this makes for a more understandable display.
	 */
	for (i = targetObjects->numrefs - 1; i >= 0; i--)
	{
		const ObjectAddress *obj = &targetObjects->refs[i];
		const ObjectAddressExtra *extra = &targetObjects->extras[i];
		char	   *objDesc;

		/* Ignore the original deletion target(s) */
		if (extra->flags & DEPFLAG_ORIGINAL)
			continue;

		objDesc = getObjectDescription(obj);

		/*
		 * If, at any stage of the recursive search, we reached the object via
		 * an AUTO, INTERNAL, or EXTENSION dependency, then it's okay to
		 * delete it even in RESTRICT mode.
		 */
		if (extra->flags & (DEPFLAG_AUTO |
							DEPFLAG_INTERNAL |
							DEPFLAG_EXTENSION))
		{
			/*
			 * auto-cascades are reported at DEBUG2, not msglevel.  We don't
			 * try to combine them with the regular message because the
			 * results are too confusing when client_min_messages and
			 * log_min_messages are different.
			 */
			ereport(DEBUG2,
					(errmsg("drop auto-cascades to %s",
							objDesc)));
		}
		else if (behavior == DROP_RESTRICT)
		{
			char	   *otherDesc = getObjectDescription(&extra->dependee);

			if (numReportedClient < MAX_REPORTED_DEPS)
			{
				/* separate entries with a newline */
				if (clientdetail.len != 0)
					appendStringInfoChar(&clientdetail, '\n');
				appendStringInfo(&clientdetail, _("%s depends on %s"),
								 objDesc, otherDesc);
				numReportedClient++;
			}
			else
				numNotReportedClient++;
			/* separate entries with a newline */
			if (logdetail.len != 0)
				appendStringInfoChar(&logdetail, '\n');
			appendStringInfo(&logdetail, _("%s depends on %s"),
							 objDesc, otherDesc);
			pfree(otherDesc);
			ok = false;
		}
		else
		{
			if (numReportedClient < MAX_REPORTED_DEPS)
			{
				/* separate entries with a newline */
				if (clientdetail.len != 0)
					appendStringInfoChar(&clientdetail, '\n');
				appendStringInfo(&clientdetail, _("drop cascades to %s"),
								 objDesc);
				numReportedClient++;
			}
			else
				numNotReportedClient++;
			/* separate entries with a newline */
			if (logdetail.len != 0)
				appendStringInfoChar(&logdetail, '\n');
			appendStringInfo(&logdetail, _("drop cascades to %s"),
							 objDesc);
		}

		pfree(objDesc);
	}

	if (numNotReportedClient > 0)
		appendStringInfo(&clientdetail, ngettext("\nand %d other object "
												 "(see server log for list)",
												 "\nand %d other objects "
												 "(see server log for list)",
												 numNotReportedClient),
						 numNotReportedClient);

	if (!ok)
	{
		if (origObject)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				  errmsg("cannot drop %s because other objects depend on it",
						 getObjectDescription(origObject)),
					 errdetail("%s", clientdetail.data),
					 errdetail_log("%s", logdetail.data),
					 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop desired object(s) because other objects depend on them"),
					 errdetail("%s", clientdetail.data),
					 errdetail_log("%s", logdetail.data),
					 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));
	}
	else if (numReportedClient > 1)
	{
		ereport(msglevel,
		/* translator: %d always has a value larger than 1 */
				(errmsg_plural("drop cascades to %d other object",
							   "drop cascades to %d other objects",
							   numReportedClient + numNotReportedClient,
							   numReportedClient + numNotReportedClient),
				 errdetail("%s", clientdetail.data),
				 errdetail_log("%s", logdetail.data)));
	}
	else if (numReportedClient == 1)
	{
		/* we just use the single item as-is */
		ereport(msglevel,
				(errmsg_internal("%s", clientdetail.data)));
	}

	pfree(clientdetail.data);
	pfree(logdetail.data);
}

/*
 * deleteOneObject: delete a single object for performDeletion.
 *
 * *depRel is the already-open pg_depend relation.
 */
static void
deleteOneObject(const ObjectAddress *object, Relation *depRel, int flags)
{
	ScanKeyData key[3];
	int			nkeys;
	SysScanDesc scan;
	HeapTuple	tup;

	/* DROP hook of the objects being removed */
	InvokeObjectDropHookArg(object->classId, object->objectId,
							object->objectSubId, flags);

	/*
	 * Close depRel if we are doing a drop concurrently.  The object deletion
	 * subroutine will commit the current transaction, so we can't keep the
	 * relation open across doDeletion().
	 */
	if (flags & PERFORM_DELETION_CONCURRENTLY)
		heap_close(*depRel, RowExclusiveLock);

	/*
	 * Delete the object itself, in an object-type-dependent way.
	 *
	 * We used to do this after removing the outgoing dependency links, but it
	 * seems just as reasonable to do it beforehand.  In the concurrent case
	 * we *must* do it in this order, because we can't make any transactional
	 * updates before calling doDeletion() --- they'd get committed right
	 * away, which is not cool if the deletion then fails.
	 */
	doDeletion(object, flags);

	/*
	 * Reopen depRel if we closed it above
	 */
	if (flags & PERFORM_DELETION_CONCURRENTLY)
		*depRel = heap_open(DependRelationId, RowExclusiveLock);

	/*
	 * Now remove any pg_depend records that link from this object to others.
	 * (Any records linking to this object should be gone already.)
	 *
	 * When dropping a whole object (subId = 0), remove all pg_depend records
	 * for its sub-objects too.
	 */
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	if (object->objectSubId != 0)
	{
		ScanKeyInit(&key[2],
					Anum_pg_depend_objsubid,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(object->objectSubId));
		nkeys = 3;
	}
	else
		nkeys = 2;

	scan = systable_beginscan(*depRel, DependDependerIndexId, true,
							  NULL, nkeys, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		simple_heap_delete(*depRel, &tup->t_self);
	}

	systable_endscan(scan);

	/*
	 * Delete shared dependency references related to this object.  Again, if
	 * subId = 0, remove records for sub-objects too.
	 */
	deleteSharedDependencyRecordsFor(object->classId, object->objectId,
									 object->objectSubId);


	/*
	 * Delete any comments, security labels, or initial privileges associated
	 * with this object.  (This is a convenient place to do these things,
	 * rather than having every object type know to do it.)
	 */
	DeleteComments(object->objectId, object->classId, object->objectSubId);
	DeleteSecurityLabel(object);
	DeleteInitPrivs(object);

	/*
	 * CommandCounterIncrement here to ensure that preceding changes are all
	 * visible to the next deletion step.
	 */
	CommandCounterIncrement();

	/*
	 * And we're done!
	 */
}

/*
 * doDeletion: actually delete a single object
 */
static void
doDeletion(const ObjectAddress *object, int flags)
{
	switch (getObjectClass(object))
	{
		case OCLASS_CLASS:
			{
				char		relKind = get_rel_relkind(object->objectId);

				if (relKind == RELKIND_INDEX)
				{
					bool		concurrent = ((flags & PERFORM_DELETION_CONCURRENTLY)
										   == PERFORM_DELETION_CONCURRENTLY);

					Assert(object->objectSubId == 0);
					index_drop(object->objectId, concurrent);
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

		case OCLASS_COLLATION:
			RemoveCollationById(object->objectId);
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

		case OCLASS_LARGEOBJECT:
			LargeObjectDrop(object->objectId);
			break;

		case OCLASS_OPERATOR:
			RemoveOperatorById(object->objectId);
			break;

		case OCLASS_OPCLASS:
			RemoveOpClassById(object->objectId);
			break;

		case OCLASS_OPFAMILY:
			RemoveOpFamilyById(object->objectId);
			break;

		case OCLASS_AM:
			RemoveAccessMethodById(object->objectId);
			break;

		case OCLASS_AMOP:
			RemoveAmOpEntryById(object->objectId);
			break;

		case OCLASS_AMPROC:
			RemoveAmProcEntryById(object->objectId);
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

		case OCLASS_TSPARSER:
			RemoveTSParserById(object->objectId);
			break;

		case OCLASS_TSDICT:
			RemoveTSDictionaryById(object->objectId);
			break;

		case OCLASS_TSTEMPLATE:
			RemoveTSTemplateById(object->objectId);
			break;

		case OCLASS_TSCONFIG:
			RemoveTSConfigurationById(object->objectId);
			break;

			/*
			 * OCLASS_ROLE, OCLASS_DATABASE, OCLASS_TBLSPACE intentionally not
			 * handled here
			 */

		case OCLASS_FDW:
			RemoveForeignDataWrapperById(object->objectId);
			break;

		case OCLASS_FOREIGN_SERVER:
			RemoveForeignServerById(object->objectId);
			break;

		case OCLASS_USER_MAPPING:
			RemoveUserMappingById(object->objectId);
			break;

		case OCLASS_DEFACL:
			RemoveDefaultACLById(object->objectId);
			break;

		case OCLASS_EXTENSION:
			RemoveExtensionById(object->objectId);
			break;

		case OCLASS_EVENT_TRIGGER:
			RemoveEventTriggerById(object->objectId);
			break;

		case OCLASS_POLICY:
			RemovePolicyById(object->objectId);
			break;

		case OCLASS_TRANSFORM:
			DropTransformById(object->objectId);
			break;

		default:
			elog(ERROR, "unrecognized object class: %u",
				 object->classId);
	}
}

/*
 * AcquireDeletionLock - acquire a suitable lock for deleting an object
 *
 * We use LockRelation for relations, LockDatabaseObject for everything
 * else.  Note that dependency.c is not concerned with deleting any kind of
 * shared-across-databases object, so we have no need for LockSharedObject.
 */
static void
AcquireDeletionLock(const ObjectAddress *object, int flags)
{
	if (object->classId == RelationRelationId)
	{
		/*
		 * In DROP INDEX CONCURRENTLY, take only ShareUpdateExclusiveLock on
		 * the index for the moment.  index_drop() will promote the lock once
		 * it's safe to do so.  In all other cases we need full exclusive
		 * lock.
		 */
		if (flags & PERFORM_DELETION_CONCURRENTLY)
			LockRelationOid(object->objectId, ShareUpdateExclusiveLock);
		else
			LockRelationOid(object->objectId, AccessExclusiveLock);
	}
	else
	{
		/* assume we should lock the whole object not a sub-object */
		LockDatabaseObject(object->classId, object->objectId, 0,
						   AccessExclusiveLock);
	}
}

/*
 * ReleaseDeletionLock - release an object deletion lock
 */
static void
ReleaseDeletionLock(const ObjectAddress *object)
{
	if (object->classId == RelationRelationId)
		UnlockRelationOid(object->objectId, AccessExclusiveLock);
	else
		/* assume we should lock the whole object not a sub-object */
		UnlockDatabaseObject(object->classId, object->objectId, 0,
							 AccessExclusiveLock);
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
 */
void
recordDependencyOnExpr(const ObjectAddress *depender,
					   Node *expr, List *rtable,
					   DependencyType behavior)
{
	find_expr_references_context context;

	context.addrs = new_object_addresses();

	/* Set up interpretation for Vars at varlevelsup = 0 */
	context.rtables = list_make1(rtable);

	/* Scan the expression tree for referenceable objects */
	find_expr_references_walker(expr, &context);

	/* Remove any duplicates */
	eliminate_duplicate_dependencies(context.addrs);

	/* And record 'em */
	recordMultipleDependencies(depender,
							   context.addrs->refs, context.addrs->numrefs,
							   behavior);

	free_object_addresses(context.addrs);
}

/*
 * recordDependencyOnSingleRelExpr - find expression dependencies
 *
 * As above, but only one relation is expected to be referenced (with
 * varno = 1 and varlevelsup = 0).  Pass the relation OID instead of a
 * range table.  An additional frammish is that dependencies on that
 * relation (or its component columns) will be marked with 'self_behavior',
 * whereas 'behavior' is used for everything else.
 *
 * NOTE: the caller should ensure that a whole-table dependency on the
 * specified relation is created separately, if one is needed.  In particular,
 * a whole-row Var "relation.*" will not cause this routine to emit any
 * dependency item.  This is appropriate behavior for subexpressions of an
 * ordinary query, so other cases need to cope as necessary.
 */
void
recordDependencyOnSingleRelExpr(const ObjectAddress *depender,
								Node *expr, Oid relId,
								DependencyType behavior,
								DependencyType self_behavior)
{
	find_expr_references_context context;
	RangeTblEntry rte;

	context.addrs = new_object_addresses();

	/* We gin up a rather bogus rangetable list to handle Vars */
	MemSet(&rte, 0, sizeof(rte));
	rte.type = T_RangeTblEntry;
	rte.rtekind = RTE_RELATION;
	rte.relid = relId;
	rte.relkind = RELKIND_RELATION;		/* no need for exactness here */

	context.rtables = list_make1(list_make1(&rte));

	/* Scan the expression tree for referenceable objects */
	find_expr_references_walker(expr, &context);

	/* Remove any duplicates */
	eliminate_duplicate_dependencies(context.addrs);

	/* Separate self-dependencies if necessary */
	if (behavior != self_behavior && context.addrs->numrefs > 0)
	{
		ObjectAddresses *self_addrs;
		ObjectAddress *outobj;
		int			oldref,
					outrefs;

		self_addrs = new_object_addresses();

		outobj = context.addrs->refs;
		outrefs = 0;
		for (oldref = 0; oldref < context.addrs->numrefs; oldref++)
		{
			ObjectAddress *thisobj = context.addrs->refs + oldref;

			if (thisobj->classId == RelationRelationId &&
				thisobj->objectId == relId)
			{
				/* Move this ref into self_addrs */
				add_exact_object_address(thisobj, self_addrs);
			}
			else
			{
				/* Keep it in context.addrs */
				*outobj = *thisobj;
				outobj++;
				outrefs++;
			}
		}
		context.addrs->numrefs = outrefs;

		/* Record the self-dependencies */
		recordMultipleDependencies(depender,
								   self_addrs->refs, self_addrs->numrefs,
								   self_behavior);

		free_object_addresses(self_addrs);
	}

	/* Record the external dependencies */
	recordMultipleDependencies(depender,
							   context.addrs->refs, context.addrs->numrefs,
							   behavior);

	free_object_addresses(context.addrs);
}

/*
 * Recursively search an expression tree for object references.
 *
 * Note: we avoid creating references to columns of tables that participate
 * in an SQL JOIN construct, but are not actually used anywhere in the query.
 * To do so, we do not scan the joinaliasvars list of a join RTE while
 * scanning the query rangetable, but instead scan each individual entry
 * of the alias list when we find a reference to it.
 *
 * Note: in many cases we do not need to create dependencies on the datatypes
 * involved in an expression, because we'll have an indirect dependency via
 * some other object.  For instance Var nodes depend on a column which depends
 * on the datatype, and OpExpr nodes depend on the operator which depends on
 * the datatype.  However we do need a type dependency if there is no such
 * indirect dependency, as for example in Const and CoerceToDomain nodes.
 *
 * Similarly, we don't need to create dependencies on collations except where
 * the collation is being freshly introduced to the expression.
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
		List	   *rtable;
		RangeTblEntry *rte;

		/* Find matching rtable entry, or complain if not found */
		if (var->varlevelsup >= list_length(context->rtables))
			elog(ERROR, "invalid varlevelsup %d", var->varlevelsup);
		rtable = (List *) list_nth(context->rtables, var->varlevelsup);
		if (var->varno <= 0 || var->varno > list_length(rtable))
			elog(ERROR, "invalid varno %d", var->varno);
		rte = rt_fetch(var->varno, rtable);

		/*
		 * A whole-row Var references no specific columns, so adds no new
		 * dependency.  (We assume that there is a whole-table dependency
		 * arising from each underlying rangetable entry.  While we could
		 * record such a dependency when finding a whole-row Var that
		 * references a relation directly, it's quite unclear how to extend
		 * that to whole-row Vars for JOINs, so it seems better to leave the
		 * responsibility with the range table.  Note that this poses some
		 * risks for identifying dependencies of stand-alone expressions:
		 * whole-table references may need to be created separately.)
		 */
		if (var->varattno == InvalidAttrNumber)
			return false;
		if (rte->rtekind == RTE_RELATION)
		{
			/* If it's a plain relation, reference this column */
			add_object_address(OCLASS_CLASS, rte->relid, var->varattno,
							   context->addrs);
		}
		else if (rte->rtekind == RTE_JOIN)
		{
			/* Scan join output column to add references to join inputs */
			List	   *save_rtables;

			/* We must make the context appropriate for join's level */
			save_rtables = context->rtables;
			context->rtables = list_copy_tail(context->rtables,
											  var->varlevelsup);
			if (var->varattno <= 0 ||
				var->varattno > list_length(rte->joinaliasvars))
				elog(ERROR, "invalid varattno %d", var->varattno);
			find_expr_references_walker((Node *) list_nth(rte->joinaliasvars,
														  var->varattno - 1),
										context);
			list_free(context->rtables);
			context->rtables = save_rtables;
		}
		return false;
	}
	else if (IsA(node, Const))
	{
		Const	   *con = (Const *) node;
		Oid			objoid;

		/* A constant must depend on the constant's datatype */
		add_object_address(OCLASS_TYPE, con->consttype, 0,
						   context->addrs);

		/*
		 * We must also depend on the constant's collation: it could be
		 * different from the datatype's, if a CollateExpr was const-folded to
		 * a simple constant.  However we can save work in the most common
		 * case where the collation is "default", since we know that's pinned.
		 */
		if (OidIsValid(con->constcollid) &&
			con->constcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, con->constcollid, 0,
							   context->addrs);

		/*
		 * If it's a regclass or similar literal referring to an existing
		 * object, add a reference to that object.  (Currently, only the
		 * regclass and regconfig cases have any likely use, but we may as
		 * well handle all the OID-alias datatypes consistently.)
		 */
		if (!con->constisnull)
		{
			switch (con->consttype)
			{
				case REGPROCOID:
				case REGPROCEDUREOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(PROCOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_PROC, objoid, 0,
										   context->addrs);
					break;
				case REGOPEROID:
				case REGOPERATOROID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(OPEROID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_OPERATOR, objoid, 0,
										   context->addrs);
					break;
				case REGCLASSOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(RELOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_CLASS, objoid, 0,
										   context->addrs);
					break;
				case REGTYPEOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(TYPEOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_TYPE, objoid, 0,
										   context->addrs);
					break;
				case REGCONFIGOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(TSCONFIGOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_TSCONFIG, objoid, 0,
										   context->addrs);
					break;
				case REGDICTIONARYOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(TSDICTOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_TSDICT, objoid, 0,
										   context->addrs);
					break;

				case REGNAMESPACEOID:
					objoid = DatumGetObjectId(con->constvalue);
					if (SearchSysCacheExists1(NAMESPACEOID,
											  ObjectIdGetDatum(objoid)))
						add_object_address(OCLASS_SCHEMA, objoid, 0,
										   context->addrs);
					break;

					/*
					 * Dependencies for regrole should be shared among all
					 * databases, so explicitly inhibit to have dependencies.
					 */
				case REGROLEOID:
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("constant of the type \"regrole\" cannot be used here")));
					break;
			}
		}
		return false;
	}
	else if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		/* A parameter must depend on the parameter's datatype */
		add_object_address(OCLASS_TYPE, param->paramtype, 0,
						   context->addrs);
		/* and its collation, just as for Consts */
		if (OidIsValid(param->paramcollid) &&
			param->paramcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, param->paramcollid, 0,
							   context->addrs);
	}
	else if (IsA(node, FuncExpr))
	{
		FuncExpr   *funcexpr = (FuncExpr *) node;

		add_object_address(OCLASS_PROC, funcexpr->funcid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) node;

		add_object_address(OCLASS_OPERATOR, opexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, DistinctExpr))
	{
		DistinctExpr *distinctexpr = (DistinctExpr *) node;

		add_object_address(OCLASS_OPERATOR, distinctexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, NullIfExpr))
	{
		NullIfExpr *nullifexpr = (NullIfExpr *) node;

		add_object_address(OCLASS_OPERATOR, nullifexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *opexpr = (ScalarArrayOpExpr *) node;

		add_object_address(OCLASS_OPERATOR, opexpr->opno, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;

		add_object_address(OCLASS_PROC, aggref->aggfnoid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, WindowFunc))
	{
		WindowFunc *wfunc = (WindowFunc *) node;

		add_object_address(OCLASS_PROC, wfunc->winfnoid, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, SubPlan))
	{
		/* Extra work needed here if we ever need this case */
		elog(ERROR, "already-planned subqueries not supported");
	}
	else if (IsA(node, RelabelType))
	{
		RelabelType *relab = (RelabelType *) node;

		/* since there is no function dependency, need to depend on type */
		add_object_address(OCLASS_TYPE, relab->resulttype, 0,
						   context->addrs);
		/* the collation might not be referenced anywhere else, either */
		if (OidIsValid(relab->resultcollid) &&
			relab->resultcollid != DEFAULT_COLLATION_OID)
			add_object_address(OCLASS_COLLATION, relab->resultcollid, 0,
							   context->addrs);
	}
	else if (IsA(node, CoerceViaIO))
	{
		CoerceViaIO *iocoerce = (CoerceViaIO *) node;

		/* since there is no exposed function, need to depend on type */
		add_object_address(OCLASS_TYPE, iocoerce->resulttype, 0,
						   context->addrs);
	}
	else if (IsA(node, ArrayCoerceExpr))
	{
		ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;

		if (OidIsValid(acoerce->elemfuncid))
			add_object_address(OCLASS_PROC, acoerce->elemfuncid, 0,
							   context->addrs);
		add_object_address(OCLASS_TYPE, acoerce->resulttype, 0,
						   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, ConvertRowtypeExpr))
	{
		ConvertRowtypeExpr *cvt = (ConvertRowtypeExpr *) node;

		/* since there is no function dependency, need to depend on type */
		add_object_address(OCLASS_TYPE, cvt->resulttype, 0,
						   context->addrs);
	}
	else if (IsA(node, CollateExpr))
	{
		CollateExpr *coll = (CollateExpr *) node;

		add_object_address(OCLASS_COLLATION, coll->collOid, 0,
						   context->addrs);
	}
	else if (IsA(node, RowExpr))
	{
		RowExpr    *rowexpr = (RowExpr *) node;

		add_object_address(OCLASS_TYPE, rowexpr->row_typeid, 0,
						   context->addrs);
	}
	else if (IsA(node, RowCompareExpr))
	{
		RowCompareExpr *rcexpr = (RowCompareExpr *) node;
		ListCell   *l;

		foreach(l, rcexpr->opnos)
		{
			add_object_address(OCLASS_OPERATOR, lfirst_oid(l), 0,
							   context->addrs);
		}
		foreach(l, rcexpr->opfamilies)
		{
			add_object_address(OCLASS_OPFAMILY, lfirst_oid(l), 0,
							   context->addrs);
		}
		/* fall through to examine arguments */
	}
	else if (IsA(node, CoerceToDomain))
	{
		CoerceToDomain *cd = (CoerceToDomain *) node;

		add_object_address(OCLASS_TYPE, cd->resulttype, 0,
						   context->addrs);
	}
	else if (IsA(node, OnConflictExpr))
	{
		OnConflictExpr *onconflict = (OnConflictExpr *) node;

		if (OidIsValid(onconflict->constraint))
			add_object_address(OCLASS_CONSTRAINT, onconflict->constraint, 0,
							   context->addrs);
		/* fall through to examine arguments */
	}
	else if (IsA(node, SortGroupClause))
	{
		SortGroupClause *sgc = (SortGroupClause *) node;

		add_object_address(OCLASS_OPERATOR, sgc->eqop, 0,
						   context->addrs);
		if (OidIsValid(sgc->sortop))
			add_object_address(OCLASS_OPERATOR, sgc->sortop, 0,
							   context->addrs);
		return false;
	}
	else if (IsA(node, Query))
	{
		/* Recurse into RTE subquery or not-yet-planned sublink subquery */
		Query	   *query = (Query *) node;
		ListCell   *lc;
		bool		result;

		/*
		 * Add whole-relation refs for each plain relation mentioned in the
		 * subquery's rtable.
		 *
		 * Note: query_tree_walker takes care of recursing into RTE_FUNCTION
		 * RTEs, subqueries, etc, so no need to do that here.  But keep it
		 * from looking at join alias lists.
		 *
		 * Note: we don't need to worry about collations mentioned in
		 * RTE_VALUES or RTE_CTE RTEs, because those must just duplicate
		 * collations referenced in other parts of the Query.  We do have to
		 * worry about collations mentioned in RTE_FUNCTION, but we take care
		 * of those when we recurse to the RangeTblFunction node(s).
		 */
		foreach(lc, query->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			switch (rte->rtekind)
			{
				case RTE_RELATION:
					add_object_address(OCLASS_CLASS, rte->relid, 0,
									   context->addrs);
					break;
				default:
					break;
			}
		}

		/*
		 * If the query is an INSERT or UPDATE, we should create a dependency
		 * on each target column, to prevent the specific target column from
		 * being dropped.  Although we will visit the TargetEntry nodes again
		 * during query_tree_walker, we won't have enough context to do this
		 * conveniently, so do it here.
		 */
		if (query->commandType == CMD_INSERT ||
			query->commandType == CMD_UPDATE)
		{
			RangeTblEntry *rte;

			if (query->resultRelation <= 0 ||
				query->resultRelation > list_length(query->rtable))
				elog(ERROR, "invalid resultRelation %d",
					 query->resultRelation);
			rte = rt_fetch(query->resultRelation, query->rtable);
			if (rte->rtekind == RTE_RELATION)
			{
				foreach(lc, query->targetList)
				{
					TargetEntry *tle = (TargetEntry *) lfirst(lc);

					if (tle->resjunk)
						continue;		/* ignore junk tlist items */
					add_object_address(OCLASS_CLASS, rte->relid, tle->resno,
									   context->addrs);
				}
			}
		}

		/*
		 * Add dependencies on constraints listed in query's constraintDeps
		 */
		foreach(lc, query->constraintDeps)
		{
			add_object_address(OCLASS_CONSTRAINT, lfirst_oid(lc), 0,
							   context->addrs);
		}

		/* query_tree_walker ignores ORDER BY etc, but we need those opers */
		find_expr_references_walker((Node *) query->sortClause, context);
		find_expr_references_walker((Node *) query->groupClause, context);
		find_expr_references_walker((Node *) query->windowClause, context);
		find_expr_references_walker((Node *) query->distinctClause, context);

		/* Examine substructure of query */
		context->rtables = lcons(query->rtable, context->rtables);
		result = query_tree_walker(query,
								   find_expr_references_walker,
								   (void *) context,
								   QTW_IGNORE_JOINALIASES);
		context->rtables = list_delete_first(context->rtables);
		return result;
	}
	else if (IsA(node, SetOperationStmt))
	{
		SetOperationStmt *setop = (SetOperationStmt *) node;

		/* we need to look at the groupClauses for operator references */
		find_expr_references_walker((Node *) setop->groupClauses, context);
		/* fall through to examine child nodes */
	}
	else if (IsA(node, RangeTblFunction))
	{
		RangeTblFunction *rtfunc = (RangeTblFunction *) node;
		ListCell   *ct;

		/*
		 * Add refs for any datatypes and collations used in a column
		 * definition list for a RECORD function.  (For other cases, it should
		 * be enough to depend on the function itself.)
		 */
		foreach(ct, rtfunc->funccoltypes)
		{
			add_object_address(OCLASS_TYPE, lfirst_oid(ct), 0,
							   context->addrs);
		}
		foreach(ct, rtfunc->funccolcollations)
		{
			Oid			collid = lfirst_oid(ct);

			if (OidIsValid(collid) && collid != DEFAULT_COLLATION_OID)
				add_object_address(OCLASS_COLLATION, collid, 0,
								   context->addrs);
		}
	}
	else if (IsA(node, TableSampleClause))
	{
		TableSampleClause *tsc = (TableSampleClause *) node;

		add_object_address(OCLASS_PROC, tsc->tsmhandler, 0,
						   context->addrs);
		/* fall through to examine arguments */
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

	/*
	 * We can't sort if the array has "extra" data, because there's no way to
	 * keep it in sync.  Fortunately that combination of features is not
	 * needed.
	 */
	Assert(!addrs->extras);

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
			 * If we have a whole-object reference and a reference to a part
			 * of the same object, we don't need the whole-object reference
			 * (for example, we don't need to reference both table foo and
			 * column foo.bar).  The whole-object reference will always appear
			 * first in the sorted list.
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
		*priorobj = *thisobj;
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
 * new_object_addresses: create a new ObjectAddresses array.
 */
ObjectAddresses *
new_object_addresses(void)
{
	ObjectAddresses *addrs;

	addrs = palloc(sizeof(ObjectAddresses));

	addrs->numrefs = 0;
	addrs->maxrefs = 32;
	addrs->refs = (ObjectAddress *)
		palloc(addrs->maxrefs * sizeof(ObjectAddress));
	addrs->extras = NULL;		/* until/unless needed */

	return addrs;
}

/*
 * Add an entry to an ObjectAddresses array.
 *
 * It is convenient to specify the class by ObjectClass rather than directly
 * by catalog OID.
 */
static void
add_object_address(ObjectClass oclass, Oid objectId, int32 subId,
				   ObjectAddresses *addrs)
{
	ObjectAddress *item;

	/*
	 * Make sure object_classes is kept up to date with the ObjectClass enum.
	 */
	StaticAssertStmt(lengthof(object_classes) == LAST_OCLASS + 1,
					 "object_classes[] must cover all ObjectClasses");

	/* enlarge array if needed */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
		Assert(!addrs->extras);
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
void
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
		Assert(!addrs->extras);
	}
	/* record this item */
	item = addrs->refs + addrs->numrefs;
	*item = *object;
	addrs->numrefs++;
}

/*
 * Add an entry to an ObjectAddresses array.
 *
 * As above, but specify entry exactly and provide some "extra" data too.
 */
static void
add_exact_object_address_extra(const ObjectAddress *object,
							   const ObjectAddressExtra *extra,
							   ObjectAddresses *addrs)
{
	ObjectAddress *item;
	ObjectAddressExtra *itemextra;

	/* allocate extra space if first time */
	if (!addrs->extras)
		addrs->extras = (ObjectAddressExtra *)
			palloc(addrs->maxrefs * sizeof(ObjectAddressExtra));

	/* enlarge array if needed */
	if (addrs->numrefs >= addrs->maxrefs)
	{
		addrs->maxrefs *= 2;
		addrs->refs = (ObjectAddress *)
			repalloc(addrs->refs, addrs->maxrefs * sizeof(ObjectAddress));
		addrs->extras = (ObjectAddressExtra *)
			repalloc(addrs->extras, addrs->maxrefs * sizeof(ObjectAddressExtra));
	}
	/* record this item */
	item = addrs->refs + addrs->numrefs;
	*item = *object;
	itemextra = addrs->extras + addrs->numrefs;
	*itemextra = *extra;
	addrs->numrefs++;
}

/*
 * Test whether an object is present in an ObjectAddresses array.
 *
 * We return "true" if object is a subobject of something in the array, too.
 */
bool
object_address_present(const ObjectAddress *object,
					   const ObjectAddresses *addrs)
{
	int			i;

	for (i = addrs->numrefs - 1; i >= 0; i--)
	{
		const ObjectAddress *thisobj = addrs->refs + i;

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
 * As above, except that if the object is present then also OR the given
 * flags into its associated extra data (which must exist).
 */
static bool
object_address_present_add_flags(const ObjectAddress *object,
								 int flags,
								 ObjectAddresses *addrs)
{
	bool		result = false;
	int			i;

	for (i = addrs->numrefs - 1; i >= 0; i--)
	{
		ObjectAddress *thisobj = addrs->refs + i;

		if (object->classId == thisobj->classId &&
			object->objectId == thisobj->objectId)
		{
			if (object->objectSubId == thisobj->objectSubId)
			{
				ObjectAddressExtra *thisextra = addrs->extras + i;

				thisextra->flags |= flags;
				result = true;
			}
			else if (thisobj->objectSubId == 0)
			{
				/*
				 * We get here if we find a need to delete a column after
				 * having already decided to drop its whole table.  Obviously
				 * we no longer need to drop the subobject, so report that we
				 * found the subobject in the array.  But don't plaster its
				 * flags on the whole object.
				 */
				result = true;
			}
			else if (object->objectSubId == 0)
			{
				/*
				 * We get here if we find a need to delete a whole table after
				 * having already decided to drop one of its columns.  We
				 * can't report that the whole object is in the array, but we
				 * should mark the subobject with the whole object's flags.
				 *
				 * It might seem attractive to physically delete the column's
				 * array entry, or at least mark it as no longer needing
				 * separate deletion.  But that could lead to, e.g., dropping
				 * the column's datatype before we drop the table, which does
				 * not seem like a good idea.  This is a very rare situation
				 * in practice, so we just take the hit of doing a separate
				 * DROP COLUMN action even though we know we're gonna delete
				 * the table later.
				 *
				 * Because there could be other subobjects of this object in
				 * the array, this case means we always have to loop through
				 * the whole array; we cannot exit early on a match.
				 */
				ObjectAddressExtra *thisextra = addrs->extras + i;

				thisextra->flags |= flags;
			}
		}
	}

	return result;
}

/*
 * Similar to above, except we search an ObjectAddressStack.
 */
static bool
stack_address_present_add_flags(const ObjectAddress *object,
								int flags,
								ObjectAddressStack *stack)
{
	bool		result = false;
	ObjectAddressStack *stackptr;

	for (stackptr = stack; stackptr; stackptr = stackptr->next)
	{
		const ObjectAddress *thisobj = stackptr->object;

		if (object->classId == thisobj->classId &&
			object->objectId == thisobj->objectId)
		{
			if (object->objectSubId == thisobj->objectSubId)
			{
				stackptr->flags |= flags;
				result = true;
			}
			else if (thisobj->objectSubId == 0)
			{
				/*
				 * We're visiting a column with whole table already on stack.
				 * As in object_address_present_add_flags(), we can skip
				 * further processing of the subobject, but we don't want to
				 * propagate flags for the subobject to the whole object.
				 */
				result = true;
			}
			else if (object->objectSubId == 0)
			{
				/*
				 * We're visiting a table with column already on stack.  As in
				 * object_address_present_add_flags(), we should propagate
				 * flags for the whole object to each of its subobjects.
				 */
				stackptr->flags |= flags;
			}
		}
	}

	return result;
}

/*
 * Record multiple dependencies from an ObjectAddresses array, after first
 * removing any duplicates.
 */
void
record_object_address_dependencies(const ObjectAddress *depender,
								   ObjectAddresses *referenced,
								   DependencyType behavior)
{
	eliminate_duplicate_dependencies(referenced);
	recordMultipleDependencies(depender,
							   referenced->refs, referenced->numrefs,
							   behavior);
}

/*
 * Clean up when done with an ObjectAddresses array.
 */
void
free_object_addresses(ObjectAddresses *addrs)
{
	pfree(addrs->refs);
	if (addrs->extras)
		pfree(addrs->extras);
	pfree(addrs);
}

/*
 * Determine the class of a given object identified by objectAddress.
 *
 * This function is essentially the reverse mapping for the object_classes[]
 * table.  We implement it as a function because the OIDs aren't consecutive.
 */
ObjectClass
getObjectClass(const ObjectAddress *object)
{
	/* only pg_class entries can have nonzero objectSubId */
	if (object->classId != RelationRelationId &&
		object->objectSubId != 0)
		elog(ERROR, "invalid non-zero objectSubId for object class %u",
			 object->classId);

	switch (object->classId)
	{
		case RelationRelationId:
			/* caller must check objectSubId */
			return OCLASS_CLASS;

		case ProcedureRelationId:
			return OCLASS_PROC;

		case TypeRelationId:
			return OCLASS_TYPE;

		case CastRelationId:
			return OCLASS_CAST;

		case CollationRelationId:
			return OCLASS_COLLATION;

		case ConstraintRelationId:
			return OCLASS_CONSTRAINT;

		case ConversionRelationId:
			return OCLASS_CONVERSION;

		case AttrDefaultRelationId:
			return OCLASS_DEFAULT;

		case LanguageRelationId:
			return OCLASS_LANGUAGE;

		case LargeObjectRelationId:
			return OCLASS_LARGEOBJECT;

		case OperatorRelationId:
			return OCLASS_OPERATOR;

		case OperatorClassRelationId:
			return OCLASS_OPCLASS;

		case OperatorFamilyRelationId:
			return OCLASS_OPFAMILY;

		case AccessMethodRelationId:
			return OCLASS_AM;

		case AccessMethodOperatorRelationId:
			return OCLASS_AMOP;

		case AccessMethodProcedureRelationId:
			return OCLASS_AMPROC;

		case RewriteRelationId:
			return OCLASS_REWRITE;

		case TriggerRelationId:
			return OCLASS_TRIGGER;

		case NamespaceRelationId:
			return OCLASS_SCHEMA;

		case TSParserRelationId:
			return OCLASS_TSPARSER;

		case TSDictionaryRelationId:
			return OCLASS_TSDICT;

		case TSTemplateRelationId:
			return OCLASS_TSTEMPLATE;

		case TSConfigRelationId:
			return OCLASS_TSCONFIG;

		case AuthIdRelationId:
			return OCLASS_ROLE;

		case DatabaseRelationId:
			return OCLASS_DATABASE;

		case TableSpaceRelationId:
			return OCLASS_TBLSPACE;

		case ForeignDataWrapperRelationId:
			return OCLASS_FDW;

		case ForeignServerRelationId:
			return OCLASS_FOREIGN_SERVER;

		case UserMappingRelationId:
			return OCLASS_USER_MAPPING;

		case DefaultAclRelationId:
			return OCLASS_DEFACL;

		case ExtensionRelationId:
			return OCLASS_EXTENSION;

		case EventTriggerRelationId:
			return OCLASS_EVENT_TRIGGER;

		case PolicyRelationId:
			return OCLASS_POLICY;

		case TransformRelationId:
			return OCLASS_TRANSFORM;
	}

	/* shouldn't get here */
	elog(ERROR, "unrecognized object class: %u", object->classId);
	return OCLASS_CLASS;		/* keep compiler quiet */
}

/*
 * delete initial ACL for extension objects
 */
static void
DeleteInitPrivs(const ObjectAddress *object)
{
	Relation	relation;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	oldtuple;

	relation = heap_open(InitPrivsRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_init_privs_objoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->objectId));
	ScanKeyInit(&key[1],
				Anum_pg_init_privs_classoid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(object->classId));
	ScanKeyInit(&key[2],
				Anum_pg_init_privs_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(object->objectSubId));

	scan = systable_beginscan(relation, InitPrivsObjIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(oldtuple = systable_getnext(scan)))
		simple_heap_delete(relation, &oldtuple->t_self);

	systable_endscan(scan);

	heap_close(relation, RowExclusiveLock);
}
