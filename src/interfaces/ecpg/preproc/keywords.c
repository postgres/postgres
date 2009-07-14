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
 *	  $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/keywords.c,v 1.89 2009/07/14 20:24:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "parser/keywords.h"
#include "type.h"
#include "preproc.h"

#define PG_KEYWORD(a,b,c) {a,b,c},


const ScanKeyword ScanKeywords[] = {
#include "parser/kwlist.h"
};

const int	NumScanKeywords = lengthof(ScanKeywords);
