/*-------------------------------------------------------------------------
 *
 * gramparse.h--
 *    scanner support routines.  used by both the bootstrap lexer
 * as well as the normal lexer
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: gramparse.h,v 1.1 1996/11/08 20:46:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef GRAMPARSE_H
#define GRAMPARSE_H		/* include once only */

/* from scan.l */
extern void init_io(); 

/* from gram.y */
extern void parser_init(Oid *typev, int nargs); 
extern int yyparse();

#endif /* GRAMPARSE_H */
