/*-------------------------------------------------------------------------
 *
 * psqlscan.h
 *	  lexical scanner for SQL commands
 *
 * This lexer used to be part of psql, and that heritage is reflected in
 * the file name as well as function and typedef names, though it can now
 * be used by other frontend programs as well.  It's also possible to extend
 * this lexer with a compatible add-on lexer to handle program-specific
 * backslash commands.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/fe_utils/psqlscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLSCAN_H
#define PSQLSCAN_H

#include "pqexpbuffer.h"


/* Abstract type for lexer's internal state */
typedef struct PsqlScanStateData *PsqlScanState;

/* Termination states for psql_scan() */
typedef enum
{
	PSCAN_SEMICOLON,			/* found command-ending semicolon */
	PSCAN_BACKSLASH,			/* found backslash command */
	PSCAN_INCOMPLETE,			/* end of line, SQL statement incomplete */
	PSCAN_EOL					/* end of line, SQL possibly complete */
} PsqlScanResult;

/* Prompt type returned by psql_scan() */
typedef enum _promptStatus
{
	PROMPT_READY,
	PROMPT_CONTINUE,
	PROMPT_COMMENT,
	PROMPT_SINGLEQUOTE,
	PROMPT_DOUBLEQUOTE,
	PROMPT_DOLLARQUOTE,
	PROMPT_PAREN,
	PROMPT_COPY
} promptStatus_t;

/* Quoting request types for get_variable() callback */
typedef enum
{
	PQUOTE_PLAIN,				/* just return the actual value */
	PQUOTE_SQL_LITERAL,			/* add quotes to make a valid SQL literal */
	PQUOTE_SQL_IDENT,			/* quote if needed to make a SQL identifier */
	PQUOTE_SHELL_ARG			/* quote if needed to be safe in a shell cmd */
} PsqlScanQuoteType;

/* Callback functions to be used by the lexer */
typedef struct PsqlScanCallbacks
{
	/* Fetch value of a variable, as a free'able string; NULL if unknown */
	/* This pointer can be NULL if no variable substitution is wanted */
	char	   *(*get_variable) (const char *varname, PsqlScanQuoteType quote,
								 void *passthrough);
} PsqlScanCallbacks;


extern PsqlScanState psql_scan_create(const PsqlScanCallbacks *callbacks);
extern void psql_scan_destroy(PsqlScanState state);

extern void psql_scan_set_passthrough(PsqlScanState state, void *passthrough);

extern void psql_scan_setup(PsqlScanState state,
							const char *line, int line_len,
							int encoding, bool std_strings);
extern void psql_scan_finish(PsqlScanState state);

extern PsqlScanResult psql_scan(PsqlScanState state,
								PQExpBuffer query_buf,
								promptStatus_t *prompt);

extern void psql_scan_reset(PsqlScanState state);

extern void psql_scan_reselect_sql_lexer(PsqlScanState state);

extern bool psql_scan_in_quote(PsqlScanState state);

#endif							/* PSQLSCAN_H */
