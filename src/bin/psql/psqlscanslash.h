/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/psqlscanslash.h
 */
#ifndef PSQLSCANSLASH_H
#define PSQLSCANSLASH_H

#include "psqlscan.h"


/* Different ways for scan_slash_option to handle parameter words */
enum slash_option_type
{
	OT_NORMAL,					/* normal case */
	OT_SQLID,					/* treat as SQL identifier */
	OT_SQLIDHACK,				/* SQL identifier, but don't downcase */
	OT_FILEPIPE,				/* it's a filename or pipe */
	OT_WHOLE_LINE,				/* just snarf the rest of the line */
	OT_NO_EVAL					/* no expansion of backticks or variables */
};


extern char *psql_scan_slash_command(PsqlScanState state);

extern char *psql_scan_slash_option(PsqlScanState state,
					   enum slash_option_type type,
					   char *quote,
					   bool semicolon);

extern void psql_scan_slash_command_end(PsqlScanState state);

#endif   /* PSQLSCANSLASH_H */
