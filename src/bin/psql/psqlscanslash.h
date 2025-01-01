/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * src/bin/psql/psqlscanslash.h
 */
#ifndef PSQLSCANSLASH_H
#define PSQLSCANSLASH_H

#include "fe_utils/psqlscan.h"


/* Different ways for scan_slash_option to handle parameter words */
enum slash_option_type
{
	OT_NORMAL,					/* normal case */
	OT_SQLID,					/* treat as SQL identifier */
	OT_SQLIDHACK,				/* SQL identifier, but don't downcase */
	OT_FILEPIPE,				/* it's a filename or pipe */
	OT_WHOLE_LINE,				/* just snarf the rest of the line */
};


extern char *psql_scan_slash_command(PsqlScanState state);

extern char *psql_scan_slash_option(PsqlScanState state,
					   enum slash_option_type type,
					   char *quote,
					   bool semicolon);

extern void psql_scan_slash_command_end(PsqlScanState state);

extern int	psql_scan_get_paren_depth(PsqlScanState state);

extern void psql_scan_set_paren_depth(PsqlScanState state, int depth);

extern void dequote_downcase_identifier(char *str, bool downcase, int encoding);

#endif   /* PSQLSCANSLASH_H */
