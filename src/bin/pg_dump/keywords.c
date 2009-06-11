/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for key words in PostgreSQL
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/bin/pg_dump/keywords.c,v 1.3 2009/06/11 14:49:07 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "parser/keywords.h"

/*
 * We don't need the token number, so leave it out to avoid requiring other
 * backend headers.
 */
#define PG_KEYWORD(a,b,c) {a,0,c},

const ScanKeyword ScanKeywords[] = {
#include "parser/kwlist.h"
};

/* End of ScanKeywords, for use in kwlookup.c */
const ScanKeyword *LastScanKeyword = endof(ScanKeywords);
