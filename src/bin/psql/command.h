/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/command.h,v 1.25 2006/03/05 15:58:51 momjian Exp $
 */
#ifndef COMMAND_H
#define COMMAND_H

#include "settings.h"
#include "print.h"
#include "psqlscan.h"


typedef enum _backslashResult
{
	PSQL_CMD_UNKNOWN = 0,			/* not done parsing yet (internal only) */
	PSQL_CMD_SEND,					/* query complete; send off */
	PSQL_CMD_SKIP_LINE,				/* keep building query */
	PSQL_CMD_TERMINATE,				/* quit program */
	PSQL_CMD_NEWEDIT,				/* query buffer was changed (e.g., via \e) */
	PSQL_CMD_ERROR					/* the execution of the backslash command
								 * resulted in an error */
} backslashResult;


extern backslashResult HandleSlashCmds(PsqlScanState scan_state,
				PQExpBuffer query_buf);

extern int	process_file(char *filename, bool single_txn);

extern bool do_pset(const char *param,
		const char *value,
		printQueryOpt *popt,
		bool quiet);

extern void SyncVariables(void);

extern void UnsyncVariables(void);

extern void SyncVerbosityVariable(void);

#endif   /* COMMAND_H */
