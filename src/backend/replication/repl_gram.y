%{
/*-------------------------------------------------------------------------
 *
 * repl_gram.y				- Parser for the replication commands
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/repl_gram.y
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "replication/replnodes.h"
#include "replication/walsender.h"

/* Result of the parsing is returned here */
Node *replication_parse_result;

/* Location tracking support --- simpler than bison's default */
#define YYLLOC_DEFAULT(Current, Rhs, N) \
	do { \
		if (N) \
			(Current) = (Rhs)[1]; \
		else \
			(Current) = (Rhs)[0]; \
	} while (0)

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.  Note this only works with
 * bison >= 2.0.  However, in bison 1.875 the default is to use alloca()
 * if possible, so there's not really much problem anyhow, at least if
 * you're building with gcc.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

#define parser_yyerror(msg)  replication_yyerror(msg, yyscanner)
#define parser_errposition(pos)  replication_scanner_errposition(pos)

%}

%expect 0
%name-prefix="replication_yy"

%union {
		char					*str;
		bool					boolval;

		XLogRecPtr				recptr;
		Node					*node;
}

/* Non-keyword tokens */
%token <str> SCONST
%token <recptr> RECPTR

/* Keyword tokens. */
%token K_BASE_BACKUP
%token K_IDENTIFY_SYSTEM
%token K_LABEL
%token K_PROGRESS
%token K_FAST
%token K_START_REPLICATION

%type <node>	command
%type <node>	base_backup start_replication identify_system
%type <boolval>	opt_progress opt_fast
%type <str>     opt_label

%%

firstcmd: command opt_semicolon
				{
					replication_parse_result = $1;
				}
			;

opt_semicolon:	';'
				| /* EMPTY */
				;

command:
			identify_system
			| base_backup
			| start_replication
			;

/*
 * IDENTIFY_SYSTEM
 */
identify_system:
			K_IDENTIFY_SYSTEM
				{
					$$ = (Node *) makeNode(IdentifySystemCmd);
				}
			;

/*
 * BASE_BACKUP [LABEL <label>] [PROGRESS] [FAST]
 */
base_backup:
			K_BASE_BACKUP opt_label opt_progress opt_fast
				{
					BaseBackupCmd *cmd = (BaseBackupCmd *) makeNode(BaseBackupCmd);

					cmd->label = $2;
					cmd->progress = $3;
					cmd->fastcheckpoint = $4;

					$$ = (Node *) cmd;
				}
			;

opt_label: K_LABEL SCONST { $$ = $2; }
			| /* EMPTY */		{ $$ = NULL; }
			;

opt_progress: K_PROGRESS		{ $$ = true; }
			| /* EMPTY */		{ $$ = false; }
			;
opt_fast: K_FAST		{ $$ = true; }
			| /* EMPTY */		{ $$ = false; }
			;

/*
 * START_REPLICATION %X/%X
 */
start_replication:
			K_START_REPLICATION RECPTR
				{
					StartReplicationCmd *cmd;

					cmd = makeNode(StartReplicationCmd);
					cmd->startpoint = $2;

					$$ = (Node *) cmd;
				}
			;
%%

#include "repl_scanner.c"
