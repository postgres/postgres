/*-------------------------------------------------------------------------
 *
 * tcopprot.h--
 *    prototypes for postgres.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tcopprot.h,v 1.1.1.1.2.1 1996/08/19 13:36:45 scrappy Exp $
 *
 * OLD COMMENTS
 *    This file was created so that other c files could get the two
 *    function prototypes without having to include tcop.h which single
 *    handedly includes the whole f*cking tree -- mer 5 Nov. 1991
 *
 *-------------------------------------------------------------------------
 */
#ifndef TCOPPROT_H
#define TCOPPROT_H

#include "tcop/dest.h"
#include "nodes/pg_list.h"
#include "parser/parse_query.h"

#ifndef BOOTSTRAP_INCLUDE
extern List *pg_plan(char *query_string, Oid *typev, int nargs,
		     QueryTreeList **queryListP, CommandDest dest);
extern void pg_eval(char *query_string, char **argv, Oid *typev, int nargs);
extern void pg_eval_dest(char *query_string, char **argv, Oid *typev,
			 int nargs, CommandDest dest);
#endif /* BOOTSTRAP_HEADER */

extern void handle_warn();
extern void quickdie();
extern void die();
extern int PostgresMain(int argc, char *argv[]);
extern void ResetUsage();
extern void ShowUsage();

#endif /* tcopprotIncluded */
