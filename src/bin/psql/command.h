/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/command.h,v 1.16 2003/07/28 00:14:43 tgl Exp $
 */
#ifndef COMMAND_H
#define COMMAND_H

#include "pqexpbuffer.h"

#include "settings.h"
#include "print.h"


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


extern backslashResult HandleSlashCmds(const char *line,
				PQExpBuffer query_buf,
				const char **end_of_cmd,
				volatile int *paren_level);

extern int	process_file(char *filename);

extern bool do_pset(const char *param,
		const char *value,
		printQueryOpt *popt,
		bool quiet);

extern void SyncVariables(void);

extern void UnsyncVariables(void);

extern void SyncVerbosityVariable(void);

#endif   /* COMMAND_H */
