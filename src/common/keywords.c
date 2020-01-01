/*-------------------------------------------------------------------------
 *
 * keywords.c
 *	  PostgreSQL's list of SQL keywords
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/common/keywords.c
 *
 *-------------------------------------------------------------------------
 */
#include "c.h"

#include "common/keywords.h"


/* ScanKeywordList lookup data for SQL keywords */

#include "kwlist_d.h"

/* Keyword categories for SQL keywords */

#define PG_KEYWORD(kwname, value, category) category,

const uint8 ScanKeywordCategories[SCANKEYWORDS_NUM_KEYWORDS] = {
#include "parser/kwlist.h"
};

#undef PG_KEYWORD
