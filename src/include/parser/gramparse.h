/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *	  scanner support routines.  used by both the bootstrap lexer
 * as well as the normal lexer
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: gramparse.h,v 1.12 2000/04/12 17:16:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef GRAMPARSE_H
#define GRAMPARSE_H				/* include once only */

/* from scan.l */
extern void init_io(void);
extern int	yylex(void);
extern void yyerror(const char *message);

/* from gram.y */
extern Oid	param_type(int t);
extern void parser_init(Oid *typev, int nargs);
extern int	yyparse(void);

#endif	 /* GRAMPARSE_H */
