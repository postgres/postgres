/*-------------------------------------------------------------------------
 *
 * jsonpath_internal.h
 *     Private definitions for jsonpath scanner & parser
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/jsonpath_internal.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef JSONPATH_INTERNAL_H
#define JSONPATH_INTERNAL_H

/* struct JsonPathString is shared between scan and gram */
typedef struct JsonPathString
{
	char	   *val;
	int			len;
	int			total;
} JsonPathString;

#include "utils/jsonpath.h"
#include "jsonpath_gram.h"

#define YY_DECL extern int     jsonpath_yylex(YYSTYPE *yylval_param, \
							  JsonPathParseResult **result, \
							  struct Node *escontext)
YY_DECL;
extern int	jsonpath_yyparse(JsonPathParseResult **result,
							 struct Node *escontext);
extern void jsonpath_yyerror(JsonPathParseResult **result,
							 struct Node *escontext,
							 const char *message);

#endif							/* JSONPATH_INTERNAL_H */
