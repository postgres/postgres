/* -------------------------------------------------------------------------
 *
 * test_session_hooks.c
 * 		Code for testing SESSION hooks.
 *
 * Copyright (c) 2010-2017, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_session_hooks/test_session_hooks.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

/* Entry point of library loading/unloading */
void		_PG_init(void);
void		_PG_fini(void);

/* GUC variables */
static char *session_hook_username = "postgres";

/* Original Hook */
static session_start_hook_type prev_session_start_hook = NULL;
static session_end_hook_type prev_session_end_hook = NULL;

static void
register_session_hook(const char *hook_at)
{
	const char *username;

	StartTransactionCommand();
	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	username = GetUserNameFromId(GetUserId(), false);

	/* Register log just for configured username */
	if (!strcmp(username, session_hook_username))
	{
		const char *dbname;
		int ret;
		StringInfoData	buf;

		dbname = get_database_name(MyDatabaseId);

		initStringInfo(&buf);

		appendStringInfo(&buf, "INSERT INTO session_hook_log (dbname, username, hook_at) ");
		appendStringInfo(&buf, "VALUES ('%s', '%s', '%s');",
			dbname, username, hook_at);

		ret = SPI_exec(buf.data, 0);
		if (ret != SPI_OK_INSERT)
			elog(ERROR, "SPI_execute failed: error code %d", ret);
	}

	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
}

/* sample session start hook function */
static void
sample_session_start_hook()
{
	/* Hook just normal backends */
	if (MyBackendId != InvalidBackendId)
	{
		(void) register_session_hook("START");

		if (prev_session_start_hook)
			prev_session_start_hook();
	}
}

/* sample session end hook function */
static void
sample_session_end_hook()
{
	/* Hook just normal backends */
	if (MyBackendId != InvalidBackendId)
	{
		if (prev_session_end_hook)
			prev_session_end_hook();

		(void) register_session_hook("END");
	}
}

/*
 * Module Load Callback
 */
void
_PG_init(void)
{
	/* Save Hooks for Unload */
	prev_session_start_hook = session_start_hook;
	prev_session_end_hook = session_end_hook;

	/* Set New Hooks */
	session_start_hook = sample_session_start_hook;
	session_end_hook = sample_session_end_hook;

	/* Load GUCs */
	DefineCustomStringVariable("test_session_hooks.username",
							   "Username to register log on session start or end",
							   NULL,
							   &session_hook_username,
							   "postgres",
							   PGC_SIGHUP,
							   0, NULL, NULL, NULL);
}

/*
 * Module Unload Callback
 */
void
_PG_fini(void)
{
	/* Uninstall Hooks */
	session_start_hook = prev_session_start_hook;
	session_end_hook = prev_session_end_hook;
}
