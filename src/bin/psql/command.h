/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/command.h,v 1.22 2005/01/01 05:43:08 momjian Exp $
 */
#ifndef COMMAND_H
#define COMMAND_H

#include "settings.h"
#include "print.h"
#include "psqlscan.h"


typedef enum _backslashResult
{
	CMD_UNKNOWN = 0,			/* not done parsing yet (internal only) */
	CMD_SEND,					/* query complete; send off */
	CMD_SKIP_LINE,				/* keep building query */
	CMD_TERMINATE,				/* quit program */
	CMD_NEWEDIT,				/* query buffer was changed (e.g., via \e) */
	CMD_ERROR					/* the execution of the backslash command
								 * resulted in an error */
} backslashResult;


extern backslashResult HandleSlashCmds(PsqlScanState scan_state,
				PQExpBuffer query_buf);

extern int	process_file(char *filename);

extern bool do_pset(const char *param,
		const char *value,
		printQueryOpt *popt,
		bool quiet);

extern void SyncVariables(void);

extern void UnsyncVariables(void);

extern void SyncVerbosityVariable(void);

#endif   /* COMMAND_H */
