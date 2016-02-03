/* -------------------------------------------------------------------------
 *
 * objectaccess.c
 *		functions for object_access_hook on various events
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/objectaccess.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"

/*
 * Hook on object accesses.  This is intended as infrastructure for security
 * and logging plugins.
 */
object_access_hook_type object_access_hook = NULL;

/*
 * RunObjectPostCreateHook
 *
 * It is entrypoint of OAT_POST_CREATE event
 */
void
RunObjectPostCreateHook(Oid classId, Oid objectId, int subId,
						bool is_internal)
{
	ObjectAccessPostCreate pc_arg;

	/* caller should check, but just in case... */
	Assert(object_access_hook != NULL);

	memset(&pc_arg, 0, sizeof(ObjectAccessPostCreate));
	pc_arg.is_internal = is_internal;

	(*object_access_hook) (OAT_POST_CREATE,
						   classId, objectId, subId,
						   (void *) &pc_arg);
}

/*
 * RunObjectDropHook
 *
 * It is entrypoint of OAT_DROP event
 */
void
RunObjectDropHook(Oid classId, Oid objectId, int subId,
				  int dropflags)
{
	ObjectAccessDrop drop_arg;

	/* caller should check, but just in case... */
	Assert(object_access_hook != NULL);

	memset(&drop_arg, 0, sizeof(ObjectAccessDrop));
	drop_arg.dropflags = dropflags;

	(*object_access_hook) (OAT_DROP,
						   classId, objectId, subId,
						   (void *) &drop_arg);
}

/*
 * RunObjectPostAlterHook
 *
 * It is entrypoint of OAT_POST_ALTER event
 */
void
RunObjectPostAlterHook(Oid classId, Oid objectId, int subId,
					   Oid auxiliaryId, bool is_internal)
{
	ObjectAccessPostAlter pa_arg;

	/* caller should check, but just in case... */
	Assert(object_access_hook != NULL);

	memset(&pa_arg, 0, sizeof(ObjectAccessPostAlter));
	pa_arg.auxiliary_id = auxiliaryId;
	pa_arg.is_internal = is_internal;

	(*object_access_hook) (OAT_POST_ALTER,
						   classId, objectId, subId,
						   (void *) &pa_arg);
}

/*
 * RunNamespaceSearchHook
 *
 * It is entrypoint of OAT_NAMESPACE_SEARCH event
 */
bool
RunNamespaceSearchHook(Oid objectId, bool ereport_on_violation)
{
	ObjectAccessNamespaceSearch ns_arg;

	/* caller should check, but just in case... */
	Assert(object_access_hook != NULL);

	memset(&ns_arg, 0, sizeof(ObjectAccessNamespaceSearch));
	ns_arg.ereport_on_violation = ereport_on_violation;
	ns_arg.result = true;

	(*object_access_hook) (OAT_NAMESPACE_SEARCH,
						   NamespaceRelationId, objectId, 0,
						   (void *) &ns_arg);

	return ns_arg.result;
}

/*
 * RunFunctionExecuteHook
 *
 * It is entrypoint of OAT_FUNCTION_EXECUTE event
 */
void
RunFunctionExecuteHook(Oid objectId)
{
	/* caller should check, but just in case... */
	Assert(object_access_hook != NULL);

	(*object_access_hook) (OAT_FUNCTION_EXECUTE,
						   ProcedureRelationId, objectId, 0,
						   NULL);
}
