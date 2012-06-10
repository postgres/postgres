/*
 * objectaccess.h
 *
 *		Object access hooks.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 */

#ifndef OBJECTACCESS_H
#define OBJECTACCESS_H

/*
 * Object access hooks are intended to be called just before or just after
 * performing certain actions on a SQL object.	This is intended as
 * infrastructure for security or logging pluggins.
 *
 * OAT_POST_CREATE should be invoked just after the object is created.
 * Typically, this is done after inserting the primary catalog records and
 * associated dependencies.
 *
 * OAT_DROP should be invoked just before deletion of objects; typically
 * deleteOneObject(). Its arguments are packed within ObjectAccessDrop.
 *
 * Other types may be added in the future.
 */
typedef enum ObjectAccessType
{
	OAT_POST_CREATE,
	OAT_DROP,
} ObjectAccessType;

/*
 * Arguments of OAT_DROP event
 */
typedef struct
{
	/*
	 * Flags to inform extensions the context of this deletion. Also see
	 * PERFORM_DELETION_* in dependency.h
	 */
	int			dropflags;
} ObjectAccessDrop;

/*
 * Hook, and a macro to invoke it.
 */
typedef void (*object_access_hook_type) (ObjectAccessType access,
													 Oid classId,
													 Oid objectId,
													 int subId,
													 void *arg);

extern PGDLLIMPORT object_access_hook_type object_access_hook;

#define InvokeObjectAccessHook(access,classId,objectId,subId,arg)	\
	do {															\
		if (object_access_hook)										\
			(*object_access_hook)((access),(classId),				\
								  (objectId),(subId),(arg));		\
	} while(0)

#endif   /* OBJECTACCESS_H */
