/*-------------------------------------------------------------------------
 *
 * tcopprot.h
 *	  prototypes for postgres.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tcopprot.h,v 1.18 1999/04/20 02:19:55 tgl Exp $
 *
 * OLD COMMENTS
 *	  This file was created so that other c files could get the two
 *	  function prototypes without having to include tcop.h which single
 *	  handedly includes the whole f*cking tree -- mer 5 Nov. 1991
 *
 *-------------------------------------------------------------------------
 */
#ifndef TCOPPROT_H
#define TCOPPROT_H

#include <setjmp.h>
#include "executor/execdesc.h"
#include "parser/parse_node.h"

/*  Autoconf's test for HAVE_SIGSETJMP fails on Linux 2.0.x because the test
 *	explicitly disallows sigsetjmp being a #define, which is how it
 *	is declared in Linux. So, to avoid compiler warnings about
 *	sigsetjmp() being redefined, let's not redefine unless necessary.
 * - thomas 1997-12-27
 * Autoconf really ought to be brighter about macro-ized system functions...
 * and this code really ought to be in config.h ...
 */

#if !defined(HAVE_SIGSETJMP) && !defined(sigsetjmp)
#define sigjmp_buf jmp_buf
#define sigsetjmp(x,y)	setjmp(x)
#define siglongjmp longjmp
#endif
extern DLLIMPORT sigjmp_buf	Warn_restart;
extern bool InError;

#ifndef BOOTSTRAP_INCLUDE
extern List *pg_parse_and_plan(char *query_string, Oid *typev, int nargs,
				  QueryTreeList **queryListP, CommandDest dest,
				  bool aclOverride);
extern void pg_exec_query(char *query_string);
extern void pg_exec_query_acl_override(char *query_string);
extern void
			pg_exec_query_dest(char *query_string, CommandDest dest, bool aclOverride);

#endif	 /* BOOTSTRAP_INCLUDE */

extern void handle_warn(SIGNAL_ARGS);
extern void quickdie(SIGNAL_ARGS);
extern void die(SIGNAL_ARGS);
extern void FloatExceptionHandler(SIGNAL_ARGS);
extern void CancelQuery(void);
extern int PostgresMain(int argc, char *argv[],
			 int real_argc, char *real_argv[]);
extern void ResetUsage(void);
extern void ShowUsage(void);

#endif	 /* TCOPPROT_H */
