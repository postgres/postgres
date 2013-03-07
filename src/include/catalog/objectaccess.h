/*
 * objectaccess.h
 *
 *		Object access hooks.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
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
	OAT_POST_ALTER,
} ObjectAccessType;

/*
 * Arguments of OAT_POST_CREATE event
 */
typedef struct
{
	/*
	 * This flag informs extensions whether the context of this creation
	 * is invoked by user's operations, or not. E.g, it shall be dealt
	 * as internal stuff on toast tables or indexes due to type changes.
	 */
	bool		is_internal;
} ObjectAccessPostCreate;

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

extern void RunObjectPostCreateHook(Oid classId, Oid objectId, int subId,
									bool is_internal);
extern void RunObjectDropHook(Oid classId, Oid objectId, int subId,
							  int dropflags);

#define InvokeObjectPostCreateHook(classId,objectId,subId)			\
	InvokeObjectPostCreateHookArg((classId),(objectId),(subId),false)
#define InvokeObjectPostCreateHookArg(classId,objectId,subId,is_internal) \
	do {															\
		if (object_access_hook)										\
			RunObjectPostCreateHook((classId),(objectId),(subId),	\
									(is_internal));					\
	} while(0)

#define InvokeObjectDropHook(classId,objectId,subId)				\
	InvokeObjectDropHookArg((classId),(objectId),(subId),0)
#define InvokeObjectDropHookArg(classId,objectId,subId,dropflags)	\
	do {															\
		if (object_access_hook)										\
			RunObjectDropHook((classId),(objectId),(subId),			\
							  (dropflags));							\
	} while(0)

#endif   /* OBJECTACCESS_H */
