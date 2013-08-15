%{
/*-------------------------------------------------------------------------
 *
 * repl_gram.y				- Parser for the replication commands
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/replication/repl_gram.y
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlogdefs.h"
#include "nodes/makefuncs.h"
#include "nodes/replnodes.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"


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
		uint32					uintval;

		XLogRecPtr				recptr;
		Node					*node;
		List					*list;
		DefElem					*defelt;
}

/* Non-keyword tokens */
%token <str> SCONST
%token <uintval> UCONST
%token <recptr> RECPTR

/* Keyword tokens. */
%token K_BASE_BACKUP
%token K_IDENTIFY_SYSTEM
%token K_START_REPLICATION
%token K_TIMELINE_HISTORY
%token K_LABEL
%token K_PROGRESS
%token K_FAST
%token K_NOWAIT
%token K_WAL
%token K_TIMELINE

%type <node>	command
%type <node>	base_backup start_replication identify_system timeline_history
%type <list>	base_backup_opt_list
%type <defelt>	base_backup_opt
%type <uintval>	opt_timeline
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
			| timeline_history
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
 * BASE_BACKUP [LABEL '<label>'] [PROGRESS] [FAST] [WAL] [NOWAIT]
 */
base_backup:
			K_BASE_BACKUP base_backup_opt_list
				{
					BaseBackupCmd *cmd = (BaseBackupCmd *) makeNode(BaseBackupCmd);
					cmd->options = $2;
					$$ = (Node *) cmd;
				}
			;

base_backup_opt_list: base_backup_opt_list base_backup_opt { $$ = lappend($1, $2); }
			| /* EMPTY */			{ $$ = NIL; }

base_backup_opt:
			K_LABEL SCONST
				{
				  $$ = makeDefElem("label",
						   (Node *)makeString($2));
				}
			| K_PROGRESS
				{
				  $$ = makeDefElem("progress",
						   (Node *)makeInteger(TRUE));
				}
			| K_FAST
				{
				  $$ = makeDefElem("fast",
						   (Node *)makeInteger(TRUE));
				}
			| K_WAL
				{
				  $$ = makeDefElem("wal",
						   (Node *)makeInteger(TRUE));
				}
			| K_NOWAIT
				{
				  $$ = makeDefElem("nowait",
						   (Node *)makeInteger(TRUE));
				}
			;

/*
 * START_REPLICATION %X/%X [TIMELINE %d]
 */
start_replication:
			K_START_REPLICATION RECPTR opt_timeline
				{
					StartReplicationCmd *cmd;

					cmd = makeNode(StartReplicationCmd);
					cmd->startpoint = $2;
					cmd->timeline = $3;

					$$ = (Node *) cmd;
				}
			;

opt_timeline:
			K_TIMELINE UCONST
				{
					if ($2 <= 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 (errmsg("invalid timeline %u", $2))));
					$$ = $2;
				}
				| /* nothing */			{ $$ = 0; }
			;

/*
 * TIMELINE_HISTORY %d
 */
timeline_history:
			K_TIMELINE_HISTORY UCONST
				{
					TimeLineHistoryCmd *cmd;

					if ($2 <= 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 (errmsg("invalid timeline %u", $2))));

					cmd = makeNode(TimeLineHistoryCmd);
					cmd->timeline = $2;

					$$ = (Node *) cmd;
				}
			;
%%

#include "repl_scanner.c"
