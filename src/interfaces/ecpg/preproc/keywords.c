/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for key words in PostgreSQL
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/ecpg/preproc/keywords.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "parser/keywords.h"
#include "type.h"
#include "extern.h"
#include "preproc.h"

#define PG_KEYWORD(a,b,c) {a,b,c},


const ScanKeyword SQLScanKeywords[] = {
#include "parser/kwlist.h"
};

const int	NumSQLScanKeywords = lengthof(SQLScanKeywords);
