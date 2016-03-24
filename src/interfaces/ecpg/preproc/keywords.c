/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for key words in PostgreSQL
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/interfaces/ecpg/preproc/keywords.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

/*
 * This is much trickier than it looks.  We are #include'ing kwlist.h
 * but the "value" numbers that go into the table are from preproc.h
 * not the backend's gram.h.  Therefore this table will recognize all
 * keywords known to the backend, but will supply the token numbers used
 * by ecpg's grammar, which is what we need.  The ecpg grammar must
 * define all the same token names the backend does, else we'll get
 * undefined-symbol failures in this compile.
 */

#include "common/keywords.h"

#include "extern.h"
#include "preproc.h"


#define PG_KEYWORD(a,b,c) {a,b,c},

const ScanKeyword SQLScanKeywords[] = {
#include "parser/kwlist.h"
};

const int	NumSQLScanKeywords = lengthof(SQLScanKeywords);
