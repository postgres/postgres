/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 * src/bin/psql/psqlscan.h
 */
#ifndef PSQLSCAN_H
#define PSQLSCAN_H

#include "pqexpbuffer.h"

#include "prompt.h"


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

/* Callback functions to be used by the lexer */
typedef struct PsqlScanCallbacks
{
	/* Fetch value of a variable, as a pfree'able string; NULL if unknown */
	/* This pointer can be NULL if no variable substitution is wanted */
	char	   *(*get_variable) (const char *varname, bool escape, bool as_ident);
	/* Print an error message someplace appropriate */
	/* (very old gcc versions don't support attributes on function pointers) */
#if defined(__GNUC__) && __GNUC__ < 4
	void		(*write_error) (const char *fmt,...);
#else
	void		(*write_error) (const char *fmt,...) pg_attribute_printf(1, 2);
#endif
} PsqlScanCallbacks;


extern PsqlScanState psql_scan_create(const PsqlScanCallbacks *callbacks);
extern void psql_scan_destroy(PsqlScanState state);

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

#endif   /* PSQLSCAN_H */
