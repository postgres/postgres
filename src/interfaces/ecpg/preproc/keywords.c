/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  lexical token lookup for key words in PostgreSQL
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
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
 * but the token numbers that go into the table are from preproc.h
 * not the backend's gram.h.  Therefore this token table will match
 * the ScanKeywords table supplied from common/keywords.c, including all
 * keywords known to the backend, but it will supply the token numbers used
 * by ecpg's grammar, which is what we need.  The ecpg grammar must
 * define all the same token names the backend does, else we'll get
 * undefined-symbol failures in this compile.
 */

#include "preproc_extern.h"
#include "preproc.h"

#define PG_KEYWORD(kwname, value, category, collabel) value,

const uint16 SQLScanKeywordTokens[] = {
#include "parser/kwlist.h"
};

#undef PG_KEYWORD
