/*-------------------------------------------------------------------------
 *
 * tcopprot.h
 *	  prototypes for postgres.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: tcopprot.h,v 1.30 2000/06/15 03:33:04 momjian Exp $
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

extern DLLIMPORT sigjmp_buf Warn_restart;
extern bool Warn_restart_ready;
extern bool InError;
extern bool ExitAfterAbort;

extern bool HostnameLookup;
extern bool ShowPortNumber;

#ifndef BOOTSTRAP_INCLUDE

extern List *pg_parse_and_rewrite(char *query_string,
					 Oid *typev, int nargs,
					 bool aclOverride);
extern Plan *pg_plan_query(Query *querytree);
extern void pg_exec_query_dest(char *query_string,
				   CommandDest dest,
				   bool aclOverride);

#endif	 /* BOOTSTRAP_INCLUDE */

extern void handle_warn(SIGNAL_ARGS);
extern void die(SIGNAL_ARGS);
extern void CancelQuery(void);
extern int PostgresMain(int argc, char *argv[],
			 int real_argc, char *real_argv[]);
extern void ResetUsage(void);
extern void ShowUsage(void);
extern FILE * StatFp;

#endif	 /* TCOPPROT_H */
