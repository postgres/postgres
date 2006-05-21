/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *	  Declarations for routines exported from lexer and parser files.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: gramparse.h,v 1.28.4.1 2006/05/21 20:11:58 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef GRAMPARSE_H
#define GRAMPARSE_H

#include "nodes/parsenodes.h"


typedef enum
{
	BACKSLASH_QUOTE_OFF,
	BACKSLASH_QUOTE_ON,
	BACKSLASH_QUOTE_SAFE_ENCODING
} BackslashQuoteType;

/* GUC variables in scan.l (every one of these is a bad idea :-() */
extern BackslashQuoteType backslash_quote;


/* from parser.c */
extern int	yylex(void);

/* from scan.l */
extern void scanner_init(const char *str);
extern void scanner_finish(void);
extern int	base_yylex(void);
extern void yyerror(const char *message);

/* from gram.y */
extern void parser_init(void);
extern int	yyparse(void);
extern List *SystemFuncName(char *name);
extern TypeName *SystemTypeName(char *name);
extern bool exprIsNullConstant(Node *arg);

#endif   /* GRAMPARSE_H */
