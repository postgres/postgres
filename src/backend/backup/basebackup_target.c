/*-------------------------------------------------------------------------
 *
 * basebackup_target.c
 *	  Base backups can be "targeted", which means that they can be sent
 *	  somewhere other than to the client which requested the backup.
 *	  Furthermore, new targets can be defined by extensions. This file
 *	  contains code to support that functionality.
 *
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup_target.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "backup/basebackup_target.h"
#include "utils/memutils.h"

typedef struct BaseBackupTargetType
{
	char	   *name;
	void	   *(*check_detail) (char *, char *);
	bbsink	   *(*get_sink) (bbsink *, void *);
} BaseBackupTargetType;

struct BaseBackupTargetHandle
{
	BaseBackupTargetType *type;
	void	   *detail_arg;
};

static void initialize_target_list(void);
static bbsink *blackhole_get_sink(bbsink *next_sink, void *detail_arg);
static bbsink *server_get_sink(bbsink *next_sink, void *detail_arg);
static void *reject_target_detail(char *target, char *target_detail);
static void *server_check_detail(char *target, char *target_detail);

static BaseBackupTargetType builtin_backup_targets[] =
{
	{
		"blackhole", reject_target_detail, blackhole_get_sink
	},
	{
		"server", server_check_detail, server_get_sink
	},
	{
		NULL
	}
};

static List *BaseBackupTargetTypeList = NIL;

/*
 * Add a new base backup target type.
 *
 * This is intended for use by server extensions.
 */
void
BaseBackupAddTarget(char *name,
					void *(*check_detail) (char *, char *),
					bbsink *(*get_sink) (bbsink *, void *))
{
	BaseBackupTargetType *newtype;
	MemoryContext oldcontext;
	ListCell   *lc;

	/* If the target list is not yet initialized, do that first. */
	if (BaseBackupTargetTypeList == NIL)
		initialize_target_list();

	/* Search the target type list for an existing entry with this name. */
	foreach(lc, BaseBackupTargetTypeList)
	{
		BaseBackupTargetType *ttype = lfirst(lc);

		if (strcmp(ttype->name, name) == 0)
		{
			/*
			 * We found one, so update it.
			 *
			 * It is probably not a great idea to call BaseBackupAddTarget for
			 * the same name multiple times, but if it happens, this seems
			 * like the sanest behavior.
			 */
			ttype->check_detail = check_detail;
			ttype->get_sink = get_sink;
			return;
		}
	}

	/*
	 * We use TopMemoryContext for allocations here to make sure that the data
	 * we need doesn't vanish under us; that's also why we copy the target
	 * name into a newly-allocated chunk of memory.
	 */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	newtype = palloc(sizeof(BaseBackupTargetType));
	newtype->name = pstrdup(name);
	newtype->check_detail = check_detail;
	newtype->get_sink = get_sink;
	BaseBackupTargetTypeList = lappend(BaseBackupTargetTypeList, newtype);
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Look up a base backup target and validate the target_detail.
 *
 * Extensions that define new backup targets will probably define a new
 * type of bbsink to match. Validation of the target_detail can be performed
 * either in the check_detail routine called here, or in the bbsink
 * constructor, which will be called from BaseBackupGetSink. It's mostly
 * a matter of taste, but the check_detail function runs somewhat earlier.
 */
BaseBackupTargetHandle *
BaseBackupGetTargetHandle(char *target, char *target_detail)
{
	ListCell   *lc;

	/* If the target list is not yet initialized, do that first. */
	if (BaseBackupTargetTypeList == NIL)
		initialize_target_list();

	/* Search the target type list for a match. */
	foreach(lc, BaseBackupTargetTypeList)
	{
		BaseBackupTargetType *ttype = lfirst(lc);

		if (strcmp(ttype->name, target) == 0)
		{
			BaseBackupTargetHandle *handle;

			/* Found the target. */
			handle = palloc(sizeof(BaseBackupTargetHandle));
			handle->type = ttype;
			handle->detail_arg = ttype->check_detail(target, target_detail);

			return handle;
		}
	}

	/* Did not find the target. */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("unrecognized target: \"%s\"", target)));

	/* keep compiler quiet */
	return NULL;
}

/*
 * Construct a bbsink that will implement the backup target.
 *
 * The get_sink function does all the real work, so all we have to do here
 * is call it with the correct arguments. Whatever the check_detail function
 * returned is here passed through to the get_sink function. This lets those
 * two functions communicate with each other, if they wish. If not, the
 * check_detail function can simply return the target_detail and let the
 * get_sink function take it from there.
 */
bbsink *
BaseBackupGetSink(BaseBackupTargetHandle *handle, bbsink *next_sink)
{
	return handle->type->get_sink(next_sink, handle->detail_arg);
}

/*
 * Load predefined target types into BaseBackupTargetTypeList.
 */
static void
initialize_target_list(void)
{
	BaseBackupTargetType *ttype = builtin_backup_targets;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
	while (ttype->name != NULL)
	{
		BaseBackupTargetTypeList = lappend(BaseBackupTargetTypeList, ttype);
		++ttype;
	}
	MemoryContextSwitchTo(oldcontext);
}

/*
 * Normally, a get_sink function should construct and return a new bbsink that
 * implements the backup target, but the 'blackhole' target just throws the
 * data away. We could implement that by adding a bbsink that does nothing
 * but forward, but it's even cheaper to implement that by not adding a bbsink
 * at all.
 */
static bbsink *
blackhole_get_sink(bbsink *next_sink, void *detail_arg)
{
	return next_sink;
}

/*
 * Create a bbsink implementing a server-side backup.
 */
static bbsink *
server_get_sink(bbsink *next_sink, void *detail_arg)
{
	return bbsink_server_new(next_sink, detail_arg);
}

/*
 * Implement target-detail checking for a target that does not accept a
 * detail.
 */
static void *
reject_target_detail(char *target, char *target_detail)
{
	if (target_detail != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("target \"%s\" does not accept a target detail",
						target)));

	return NULL;
}

/*
 * Implement target-detail checking for a server-side backup.
 *
 * target_detail should be the name of the directory to which the backup
 * should be written, but we don't check that here. Rather, that check,
 * as well as the necessary permissions checking, happens in bbsink_server_new.
 */
static void *
server_check_detail(char *target, char *target_detail)
{
	if (target_detail == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("target \"%s\" requires a target detail",
						target)));

	return target_detail;
}
