/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *	  Declarations for routines exported from lexer and parser files.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: gramparse.h,v 1.24 2002/08/27 04:55:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef GRAMPARSE_H
#define GRAMPARSE_H

#include "lib/stringinfo.h"
#include "nodes/parsenodes.h"

/* from parser.c */
extern void parser_param_set(Oid *typev, int nargs);
extern Oid	param_type(int t);
extern int	yylex(void);

/* from scan.l */
extern void scanner_init(StringInfo str);
extern void scanner_finish(void);
extern int	base_yylex(void);
extern void yyerror(const char *message);

/* from gram.y */
extern void parser_init(void);
extern int	yyparse(void);
extern List *SystemFuncName(char *name);
extern TypeName *SystemTypeName(char *name);
extern bool	exprIsNullConstant(Node *arg);

#endif   /* GRAMPARSE_H */
