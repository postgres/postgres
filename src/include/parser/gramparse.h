/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *	  Declarations for routines exported from lexer and parser files.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: gramparse.h,v 1.13 2000/09/12 21:07:12 tgl Exp $
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

#endif	 /* GRAMPARSE_H */
