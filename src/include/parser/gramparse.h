/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *	  Declarations for routines exported from lexer and parser files.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: gramparse.h,v 1.19 2001/11/05 17:46:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef GRAMPARSE_H
#define GRAMPARSE_H

/* from parser.c */
extern int	yylex(void);

/* from scan.l */
extern void scanner_init(void);
extern int	base_yylex(void);
extern void yyerror(const char *message);

/* from gram.y */
extern void parser_init(Oid *typev, int nargs);
extern Oid	param_type(int t);
extern int	yyparse(void);
extern char *xlateSqlFunc(char *name);
extern char *xlateSqlType(char *name);
bool		exprIsNullConstant(Node *arg);

#endif   /* GRAMPARSE_H */
