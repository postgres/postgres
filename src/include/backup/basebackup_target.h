/*-------------------------------------------------------------------------
 *
 * basebackup_target.h
 *	  Extensibility framework for adding base backup targets.
 *
 * Portions Copyright (c) 2010-2023, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup_target.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BASEBACKUP_TARGET_H
#define BASEBACKUP_TARGET_H

#include "backup/basebackup_sink.h"

struct BaseBackupTargetHandle;
typedef struct BaseBackupTargetHandle BaseBackupTargetHandle;

/*
 * Extensions can call this function to create new backup targets.
 *
 * 'name' is the name of the new target.
 *
 * 'check_detail' is a function that accepts a target name and target detail
 * and either throws an error (if the target detail is not valid or some other
 * problem, such as a permissions issue, is detected) or returns a pointer to
 * the data that will be needed to create a bbsink implementing that target.
 * The second argument will be NULL if the TARGET_DETAIL option to the
 * BASE_BACKUP command was not specified.
 *
 * 'get_sink' is a function that creates the bbsink. The first argument
 * is the successor sink; the sink created by this function should always
 * forward to this sink. The second argument is the pointer returned by a
 * previous call to the 'check_detail' function.
 *
 * In practice, a user will type something like "pg_basebackup --target foo:bar
 * -Xfetch". That will cause the server to look for a backup target named
 * "foo". If one is found, the check_detail callback will be invoked for the
 * string "bar", and whatever that callback returns will be passed as the
 * second argument to the get_sink callback.
 */
extern void BaseBackupAddTarget(char *name,
								void *(*check_detail) (char *, char *),
								bbsink *(*get_sink) (bbsink *, void *));

/*
 * These functions are used by the core code to access base backup targets
 * added via BaseBackupAddTarget(). The core code will pass the TARGET and
 * TARGET_DETAIL strings obtained from the user to BaseBackupGetTargetHandle,
 * which will either throw an error (if the TARGET is not recognized or the
 * check_detail hook for that TARGET doesn't like the TARGET_DETAIL) or
 * return a BaseBackupTargetHandle object that can later be passed to
 * BaseBackupGetSink.
 *
 * BaseBackupGetSink constructs a bbsink implementing the desired target
 * using the BaseBackupTargetHandle and the successor bbsink. It does this
 * by arranging to call the get_sink() callback provided by the extension
 * that implements the base backup target.
 */
extern BaseBackupTargetHandle *BaseBackupGetTargetHandle(char *target,
														 char *target_detail);
extern bbsink *BaseBackupGetSink(BaseBackupTargetHandle *handle,
								 bbsink *next_sink);

#endif
