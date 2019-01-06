/*-------------------------------------------------------------------------
 *
 * keywords.h
 *	  PostgreSQL's list of SQL keywords
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/keywords.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef KEYWORDS_H
#define KEYWORDS_H

#include "common/kwlookup.h"

/* Keyword categories --- should match lists in gram.y */
#define UNRESERVED_KEYWORD		0
#define COL_NAME_KEYWORD		1
#define TYPE_FUNC_NAME_KEYWORD	2
#define RESERVED_KEYWORD		3

#ifndef FRONTEND
extern PGDLLIMPORT const ScanKeywordList ScanKeywords;
extern PGDLLIMPORT const uint8 ScanKeywordCategories[];
#else
extern const ScanKeywordList ScanKeywords;
extern const uint8 ScanKeywordCategories[];
#endif

#endif							/* KEYWORDS_H */
