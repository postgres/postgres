/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for key words in PostgreSQL
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/keywords.c
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

const ScanKeyword FEScanKeywords[] = {
#include "parser/kwlist.h"
};

const int	NumFEScanKeywords = lengthof(FEScanKeywords);
