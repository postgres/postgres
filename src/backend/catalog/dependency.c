/*-------------------------------------------------------------------------
 *
 * dependency.c
 *	  Routines to support inter-object dependencies.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/catalog/dependency.c,v 1.2 2002/07/15 16:33:31 tgl Exp $
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
#include "catalog/pg_attrdef.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_language.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/defrem.h"
#include "commands/proclang.h"
#include "commands/trigger.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "rewrite/rewriteRemove.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* This enum covers all system catalogs whose OIDs can appear in classid. */
typedef enum ObjectClasses
{
	OCLASS_CLASS,				/* pg_class */
	OCLASS_PROC,				/* pg_proc */
	OCLASS_TYPE,				/* pg_type */
	OCLASS_CONSTRAINT,			/* pg_constraint */
	OCLASS_DEFAULT,				/* pg_attrdef */
	OCLASS_LANGUAGE,			/* pg_language */
	OCLASS_OPERATOR,			/* pg_operator */
	OCLASS_REWRITE,				/* pg_rewrite */
	OCLASS_TRIGGER				/* pg_trigger */
} ObjectClasses;

static bool recursiveDeletion(const ObjectAddress *object,
							  DropBehavior behavior,
							  int recursionLevel,
							  Relation depRel);
static void doDeletion(const ObjectAddress *object);
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
	char		   *objDescription;
	Relation		depRel;

	/*
	 * Get object description for possible use in failure message.
	 * Must do this before deleting it ...
	 */
	objDescription = getObjectDescription(object);

	/*
	 * We save some cycles by opening pg_depend just once and passing the
	 * Relation pointer down to all the recursive deletion steps.
	 */
	depRel = heap_openr(DependRelationName, RowExclusiveLock);

	if (!recursiveDeletion(object, behavior, 0, depRel))
		elog(ERROR, "Cannot drop %s because other objects depend on it"
			 "\n\tUse DROP ... CASCADE to drop the dependent objects too",
			 objDescription);

	heap_close(depRel, RowExclusiveLock);

	pfree(objDescription);
}


/*
 * recursiveDeletion: delete a single object for performDeletion.
 *
 * Returns TRUE if successful, FALSE if not.  recursionLevel is 0 at the
 * outer level, >0 when deleting a dependent object.
 *
 * In RESTRICT mode, we perform all the deletions anyway, but elog a NOTICE
 * and return FALSE if we find a restriction violation.  performDeletion
 * will then abort the transaction to nullify the deletions.  We have to
 * do it this way to (a) report all the direct and indirect dependencies
 * while (b) not going into infinite recursion if there's a cycle.
 */
static bool
recursiveDeletion(const ObjectAddress *object,
				  DropBehavior behavior,
				  int recursionLevel,
				  Relation depRel)
{
	bool			ok = true;
	char		   *objDescription;
	ScanKeyData		key[3];
	int				nkeys;
	SysScanDesc		scan;
	HeapTuple		tup;
	ObjectAddress	otherObject;

	/*
	 * Get object description for possible use in messages.  Must do this
	 * before deleting it ...
	 */
	objDescription = getObjectDescription(object);

	/*
	 * Step 1: find and remove pg_depend records that link from this
	 * object to others.  We have to do this anyway, and doing it first
	 * ensures that we avoid infinite recursion in the case of cycles.
	 * Also, some dependency types require an error here.
	 *
	 * When dropping a whole object (subId = 0), remove all pg_depend
	 * records for its sub-objects too.
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
		Form_pg_depend	foundDep = (Form_pg_depend) GETSTRUCT(tup);

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
				 * Disallow direct DROP of an object that is part of the
				 * implementation of another object.  (We just elog here,
				 * rather than issuing a notice and continuing, since
				 * no other dependencies are likely to be interesting.)
				 */
				if (recursionLevel == 0)
					elog(ERROR, "Cannot drop %s because %s requires it"
						 "\n\tYou may DROP the other object instead",
						 objDescription,
						 getObjectDescription(&otherObject));
				break;
			case DEPENDENCY_PIN:
				/*
				 * Should not happen; PIN dependencies should have zeroes
				 * in the depender fields...
				 */
				elog(ERROR, "recursiveDeletion: incorrect use of PIN dependency with %s",
					 objDescription);
				break;
			default:
				elog(ERROR, "recursiveDeletion: unknown dependency type '%c' for %s",
					 foundDep->deptype, objDescription);
				break;
		}

		simple_heap_delete(depRel, &tup->t_self);
	}

	systable_endscan(scan);

	/*
	 * CommandCounterIncrement here to ensure that preceding changes
	 * are all visible; in particular, that the above deletions of pg_depend
	 * entries are visible.  That prevents infinite recursion in case of
	 * a dependency loop (which is perfectly legal).
	 */
	CommandCounterIncrement();

	/*
	 * Step 2: scan pg_depend records that link to this object, showing
	 * the things that depend on it.  Recursively delete those things.
	 * (We don't delete the pg_depend records here, as the recursive call
	 * will do that.)  Note it's important to delete the dependent objects
	 * before the referenced one, since the deletion routines might do
	 * things like try to update the pg_class record when deleting a
	 * check constraint.
	 *
	 * Again, when dropping a whole object (subId = 0), find pg_depend
	 * records for its sub-objects too.
	 *
	 * NOTE: because we are using SnapshotNow, if a recursive call deletes
	 * any pg_depend tuples that our scan hasn't yet visited, we will not see
	 * them as good when we do visit them.  This is essential for correct
	 * behavior if there are multiple dependency paths between two objects
	 * --- else we might try to delete an already-deleted object.
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
		Form_pg_depend	foundDep = (Form_pg_depend) GETSTRUCT(tup);

		otherObject.classId = foundDep->classid;
		otherObject.objectId = foundDep->objid;
		otherObject.objectSubId = foundDep->objsubid;

		switch (foundDep->deptype)
		{
			case DEPENDENCY_NORMAL:
				if (behavior == DROP_RESTRICT)
				{
					elog(NOTICE, "%s depends on %s",
						 getObjectDescription(&otherObject),
						 objDescription);
					ok = false;
				}
				else
					elog(NOTICE, "Drop cascades to %s",
						 getObjectDescription(&otherObject));

				if (!recursiveDeletion(&otherObject, behavior,
									   recursionLevel + 1, depRel))
					ok = false;
				break;
			case DEPENDENCY_AUTO:
			case DEPENDENCY_INTERNAL:
				/*
				 * We propagate the DROP without complaint even in the
				 * RESTRICT case.  (However, normal dependencies on the
				 * component object could still cause failure.)
				 */
				elog(DEBUG1, "Drop auto-cascades to %s",
					 getObjectDescription(&otherObject));

				if (!recursiveDeletion(&otherObject, behavior,
									   recursionLevel + 1, depRel))
					ok = false;
				break;
			case DEPENDENCY_PIN:
				/*
				 * For a PIN dependency we just elog immediately; there
				 * won't be any others to report.
				 */
				elog(ERROR, "Cannot drop %s because it is required by the database system",
					 objDescription);
				break;
			default:
				elog(ERROR, "recursiveDeletion: unknown dependency type '%c' for %s",
					 foundDep->deptype, objDescription);
				break;
		}
	}

	systable_endscan(scan);

	/*
	 * We do not need CommandCounterIncrement here, since if step 2 did
	 * anything then each recursive call will have ended with one.
	 */

	/*
	 * Step 3: delete the object itself.
	 */
	doDeletion(object);

	/*
	 * Delete any comments associated with this object.  (This is a convenient
	 * place to do it instead of having every object type know to do it.)
	 */
	DeleteComments(object->objectId, object->classId, object->objectSubId);

	/*
	 * CommandCounterIncrement here to ensure that preceding changes
	 * are all visible.
	 */
	CommandCounterIncrement();

	/*
	 * And we're done!
	 */
	pfree(objDescription);

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
			HeapTuple	relTup;
			char		relKind;

			/*
			 * Need the relkind to figure out how to drop.
			 */
			relTup = SearchSysCache(RELOID,
									ObjectIdGetDatum(object->objectId),
									0, 0, 0);
			if (!HeapTupleIsValid(relTup))
				elog(ERROR, "doDeletion: Relation %u does not exist",
					 object->objectId);
			relKind = ((Form_pg_class) GETSTRUCT(relTup))->relkind;
			ReleaseSysCache(relTup);

			if (relKind == RELKIND_INDEX)
			{
				Assert(object->objectSubId == 0);
				index_drop(object->objectId);
			}
			else
			{
				if (object->objectSubId != 0)
					elog(ERROR, "DROP COLUMN not implemented yet");
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

		case OCLASS_CONSTRAINT:
			RemoveConstraintById(object->objectId);
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

		case OCLASS_REWRITE:
			RemoveRewriteRuleById(object->objectId);
			break;

		case OCLASS_TRIGGER:
			RemoveTriggerById(object->objectId);
			break;

		default:
			elog(ERROR, "doDeletion: Unsupported object class %u",
				 object->classId);
	}
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
	static bool reloids_initialized = false;
	static Oid	reloid_pg_constraint;
	static Oid	reloid_pg_attrdef;
	static Oid	reloid_pg_language;
	static Oid	reloid_pg_operator;
	static Oid	reloid_pg_rewrite;
	static Oid	reloid_pg_trigger;

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
	 *
	 * Although these OIDs aren't compile-time constants, they surely
	 * shouldn't change during a backend's run.  So, look them up the
	 * first time through and then cache them.
	 */
	if (!reloids_initialized)
	{
		reloid_pg_constraint = get_system_catalog_relid(ConstraintRelationName);
		reloid_pg_attrdef = get_system_catalog_relid(AttrDefaultRelationName);
		reloid_pg_language = get_system_catalog_relid(LanguageRelationName);
		reloid_pg_operator = get_system_catalog_relid(OperatorRelationName);
		reloid_pg_rewrite = get_system_catalog_relid(RewriteRelationName);
		reloid_pg_trigger = get_system_catalog_relid(TriggerRelationName);
		reloids_initialized = true;
	}

	if (object->classId == reloid_pg_constraint)
	{
		Assert(object->objectSubId == 0);
		return OCLASS_CONSTRAINT;
	}
	if (object->classId == reloid_pg_attrdef)
	{
		Assert(object->objectSubId == 0);
		return OCLASS_DEFAULT;
	}
	if (object->classId == reloid_pg_language)
	{
		Assert(object->objectSubId == 0);
		return OCLASS_LANGUAGE;
	}
	if (object->classId == reloid_pg_operator)
	{
		Assert(object->objectSubId == 0);
		return OCLASS_OPERATOR;
	}
	if (object->classId == reloid_pg_rewrite)
	{
		Assert(object->objectSubId == 0);
		return OCLASS_REWRITE;
	}
	if (object->classId == reloid_pg_trigger)
	{
		Assert(object->objectSubId == 0);
		return OCLASS_TRIGGER;
	}

	elog(ERROR, "getObjectClass: Unknown object class %u",
		 object->classId);
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
								 get_attname(object->objectId,
											 object->objectSubId));
			break;

		case OCLASS_PROC:
			/* XXX could improve on this */
			appendStringInfo(&buffer, "function %s",
							 get_func_name(object->objectId));
			break;

		case OCLASS_TYPE:
		{
			HeapTuple		typeTup;

			typeTup = SearchSysCache(TYPEOID,
									 ObjectIdGetDatum(object->objectId),
									 0, 0, 0);
			if (!HeapTupleIsValid(typeTup))
				elog(ERROR, "getObjectDescription: Type %u does not exist",
					 object->objectId);
			appendStringInfo(&buffer, "type %s",
							 NameStr(((Form_pg_type) GETSTRUCT(typeTup))->typname));
			ReleaseSysCache(typeTup);
			break;
		}

		case OCLASS_CONSTRAINT:
		{
			Relation		conDesc;
			ScanKeyData		skey[1];
			SysScanDesc		rcscan;
			HeapTuple		tup;
			Form_pg_constraint con;

			conDesc = heap_openr(ConstraintRelationName, AccessShareLock);

			ScanKeyEntryInitialize(&skey[0], 0x0,
								   ObjectIdAttributeNumber, F_OIDEQ,
								   ObjectIdGetDatum(object->objectId));

			rcscan = systable_beginscan(conDesc, ConstraintOidIndex, true,
										SnapshotNow, 1, skey);

			tup = systable_getnext(rcscan);

			if (!HeapTupleIsValid(tup))
				elog(ERROR, "getObjectDescription: Constraint %u does not exist",
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

		case OCLASS_DEFAULT:
		{
			Relation		attrdefDesc;
			ScanKeyData		skey[1];
			SysScanDesc		adscan;
			HeapTuple		tup;
			Form_pg_attrdef attrdef;
			ObjectAddress	colobject;

			attrdefDesc = heap_openr(AttrDefaultRelationName, AccessShareLock);

			ScanKeyEntryInitialize(&skey[0], 0x0,
								   ObjectIdAttributeNumber, F_OIDEQ,
								   ObjectIdGetDatum(object->objectId));

			adscan = systable_beginscan(attrdefDesc, AttrDefaultOidIndex, true,
										SnapshotNow, 1, skey);

			tup = systable_getnext(adscan);

			if (!HeapTupleIsValid(tup))
				elog(ERROR, "getObjectDescription: Default %u does not exist",
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
			HeapTuple		langTup;

			langTup = SearchSysCache(LANGOID,
									 ObjectIdGetDatum(object->objectId),
									 0, 0, 0);
			if (!HeapTupleIsValid(langTup))
				elog(ERROR, "getObjectDescription: Language %u does not exist",
					 object->objectId);
			appendStringInfo(&buffer, "language %s",
							 NameStr(((Form_pg_language) GETSTRUCT(langTup))->lanname));
			ReleaseSysCache(langTup);
			break;
		}

		case OCLASS_OPERATOR:
			/* XXX could improve on this */
			appendStringInfo(&buffer, "operator %s",
							 get_opname(object->objectId));
			break;

		case OCLASS_REWRITE:
		{
			Relation		ruleDesc;
			ScanKeyData		skey[1];
			SysScanDesc		rcscan;
			HeapTuple		tup;
			Form_pg_rewrite	rule;

			ruleDesc = heap_openr(RewriteRelationName, AccessShareLock);

			ScanKeyEntryInitialize(&skey[0], 0x0,
								   ObjectIdAttributeNumber, F_OIDEQ,
								   ObjectIdGetDatum(object->objectId));

			rcscan = systable_beginscan(ruleDesc, RewriteOidIndex, true,
										SnapshotNow, 1, skey);

			tup = systable_getnext(rcscan);

			if (!HeapTupleIsValid(tup))
				elog(ERROR, "getObjectDescription: Rule %u does not exist",
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
			Relation		trigDesc;
			ScanKeyData		skey[1];
			SysScanDesc		tgscan;
			HeapTuple		tup;
			Form_pg_trigger	trig;

			trigDesc = heap_openr(TriggerRelationName, AccessShareLock);

			ScanKeyEntryInitialize(&skey[0], 0x0,
								   ObjectIdAttributeNumber, F_OIDEQ,
								   ObjectIdGetDatum(object->objectId));

			tgscan = systable_beginscan(trigDesc, TriggerOidIndex, true,
										SnapshotNow, 1, skey);

			tup = systable_getnext(tgscan);

			if (!HeapTupleIsValid(tup))
				elog(ERROR, "getObjectDescription: Trigger %u does not exist",
					 object->objectId);

			trig = (Form_pg_trigger) GETSTRUCT(tup);

			appendStringInfo(&buffer, "trigger %s on ",
							 NameStr(trig->tgname));
			getRelationDescription(&buffer, trig->tgrelid);

			systable_endscan(tgscan);
			heap_close(trigDesc, AccessShareLock);
			break;
		}

		default:
			appendStringInfo(&buffer, "unknown object %u %u %d",
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
	Form_pg_class	relForm;

	relTup = SearchSysCache(RELOID,
							ObjectIdGetDatum(relid),
							0, 0, 0);
	if (!HeapTupleIsValid(relTup))
		elog(ERROR, "getObjectDescription: Relation %u does not exist",
			 relid);
	relForm = (Form_pg_class) GETSTRUCT(relTup);

	switch (relForm->relkind)
	{
		case RELKIND_RELATION:
			appendStringInfo(buffer, "table %s",
							 NameStr(relForm->relname));
			break;
		case RELKIND_INDEX:
			appendStringInfo(buffer, "index %s",
							 NameStr(relForm->relname));
			break;
		case RELKIND_SPECIAL:
			appendStringInfo(buffer, "special system relation %s",
							 NameStr(relForm->relname));
			break;
		case RELKIND_SEQUENCE:
			appendStringInfo(buffer, "sequence %s",
							 NameStr(relForm->relname));
			break;
		case RELKIND_UNCATALOGED:
			appendStringInfo(buffer, "uncataloged table %s",
							 NameStr(relForm->relname));
			break;
		case RELKIND_TOASTVALUE:
			appendStringInfo(buffer, "toast table %s",
							 NameStr(relForm->relname));
			break;
		case RELKIND_VIEW:
			appendStringInfo(buffer, "view %s",
							 NameStr(relForm->relname));
			break;
		default:
			/* shouldn't get here */
			appendStringInfo(buffer, "relation %s",
							 NameStr(relForm->relname));
			break;
	}

	ReleaseSysCache(relTup);
}
