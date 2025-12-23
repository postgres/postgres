%{
/*-------------------------------------------------------------------------
 *
 * repl_gram.y				- Parser for the replication commands
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
#include "nodes/parsenodes.h"
#include "nodes/replnodes.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"

#include "repl_gram.h"


/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

%}

%parse-param {Node **replication_parse_result_p}
%parse-param {yyscan_t yyscanner}
%lex-param   {yyscan_t yyscanner}
%pure-parser
%expect 0
%name-prefix="replication_yy"

%union
{
	char	   *str;
	bool		boolval;
	uint32		uintval;
	XLogRecPtr	recptr;
	Node	   *node;
	List	   *list;
	DefElem	   *defelt;
}

/* Non-keyword tokens */
%token <str> SCONST IDENT
%token <uintval> UCONST
%token <recptr> RECPTR

/* Keyword tokens. */
%token K_BASE_BACKUP
%token K_IDENTIFY_SYSTEM
%token K_READ_REPLICATION_SLOT
%token K_SHOW
%token K_START_REPLICATION
%token K_CREATE_REPLICATION_SLOT
%token K_DROP_REPLICATION_SLOT
%token K_ALTER_REPLICATION_SLOT
%token K_TIMELINE_HISTORY
%token K_WAIT
%token K_TIMELINE
%token K_PHYSICAL
%token K_LOGICAL
%token K_SLOT
%token K_RESERVE_WAL
%token K_TEMPORARY
%token K_TWO_PHASE
%token K_EXPORT_SNAPSHOT
%token K_NOEXPORT_SNAPSHOT
%token K_USE_SNAPSHOT
%token K_UPLOAD_MANIFEST

%type <node>	command
%type <node>	base_backup start_replication start_logical_replication
				create_replication_slot drop_replication_slot
				alter_replication_slot identify_system read_replication_slot
				timeline_history show upload_manifest
%type <list>	generic_option_list
%type <defelt>	generic_option
%type <uintval>	opt_timeline
%type <list>	plugin_options plugin_opt_list
%type <defelt>	plugin_opt_elem
%type <node>	plugin_opt_arg
%type <str>		opt_slot var_name ident_or_keyword
%type <boolval>	opt_temporary
%type <list>	create_slot_options create_slot_legacy_opt_list
%type <defelt>	create_slot_legacy_opt

%%

firstcmd: command opt_semicolon
				{
					*replication_parse_result_p = $1;

					(void) yynerrs; /* suppress compiler warning */
				}
			;

opt_semicolon:	';'
				| /* EMPTY */
				;

command:
			identify_system
			| base_backup
			| start_replication
			| start_logical_replication
			| create_replication_slot
			| drop_replication_slot
			| alter_replication_slot
			| read_replication_slot
			| timeline_history
			| show
			| upload_manifest
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
 * READ_REPLICATION_SLOT %s
 */
read_replication_slot:
			K_READ_REPLICATION_SLOT var_name
				{
					ReadReplicationSlotCmd *n = makeNode(ReadReplicationSlotCmd);
					n->slotname = $2;
					$$ = (Node *) n;
				}
			;

/*
 * SHOW setting
 */
show:
			K_SHOW var_name
				{
					VariableShowStmt *n = makeNode(VariableShowStmt);
					n->name = $2;
					$$ = (Node *) n;
				}

var_name:	IDENT	{ $$ = $1; }
			| var_name '.' IDENT
				{ $$ = psprintf("%s.%s", $1, $3); }
		;

/*
 * BASE_BACKUP [ ( option [ 'value' ] [, ...] ) ]
 */
base_backup:
			K_BASE_BACKUP '(' generic_option_list ')'
				{
					BaseBackupCmd *cmd = makeNode(BaseBackupCmd);
					cmd->options = $3;
					$$ = (Node *) cmd;
				}
			| K_BASE_BACKUP
				{
					BaseBackupCmd *cmd = makeNode(BaseBackupCmd);
					$$ = (Node *) cmd;
				}
			;

create_replication_slot:
			/* CREATE_REPLICATION_SLOT slot [TEMPORARY] PHYSICAL [options] */
			K_CREATE_REPLICATION_SLOT IDENT opt_temporary K_PHYSICAL create_slot_options
				{
					CreateReplicationSlotCmd *cmd;
					cmd = makeNode(CreateReplicationSlotCmd);
					cmd->kind = REPLICATION_KIND_PHYSICAL;
					cmd->slotname = $2;
					cmd->temporary = $3;
					cmd->options = $5;
					$$ = (Node *) cmd;
				}
			/* CREATE_REPLICATION_SLOT slot [TEMPORARY] LOGICAL plugin [options] */
			| K_CREATE_REPLICATION_SLOT IDENT opt_temporary K_LOGICAL IDENT create_slot_options
				{
					CreateReplicationSlotCmd *cmd;
					cmd = makeNode(CreateReplicationSlotCmd);
					cmd->kind = REPLICATION_KIND_LOGICAL;
					cmd->slotname = $2;
					cmd->temporary = $3;
					cmd->plugin = $5;
					cmd->options = $6;
					$$ = (Node *) cmd;
				}
			;

create_slot_options:
			'(' generic_option_list ')'			{ $$ = $2; }
			| create_slot_legacy_opt_list		{ $$ = $1; }
			;

create_slot_legacy_opt_list:
			create_slot_legacy_opt_list create_slot_legacy_opt
				{ $$ = lappend($1, $2); }
			| /* EMPTY */
				{ $$ = NIL; }
			;

create_slot_legacy_opt:
			K_EXPORT_SNAPSHOT
				{
				  $$ = makeDefElem("snapshot",
								   (Node *) makeString("export"), -1);
				}
			| K_NOEXPORT_SNAPSHOT
				{
				  $$ = makeDefElem("snapshot",
								   (Node *) makeString("nothing"), -1);
				}
			| K_USE_SNAPSHOT
				{
				  $$ = makeDefElem("snapshot",
								   (Node *) makeString("use"), -1);
				}
			| K_RESERVE_WAL
				{
				  $$ = makeDefElem("reserve_wal",
								   (Node *) makeBoolean(true), -1);
				}
			| K_TWO_PHASE
				{
				  $$ = makeDefElem("two_phase",
								   (Node *) makeBoolean(true), -1);
				}
			;

/* DROP_REPLICATION_SLOT slot */
drop_replication_slot:
			K_DROP_REPLICATION_SLOT IDENT
				{
					DropReplicationSlotCmd *cmd;
					cmd = makeNode(DropReplicationSlotCmd);
					cmd->slotname = $2;
					cmd->wait = false;
					$$ = (Node *) cmd;
				}
			| K_DROP_REPLICATION_SLOT IDENT K_WAIT
				{
					DropReplicationSlotCmd *cmd;
					cmd = makeNode(DropReplicationSlotCmd);
					cmd->slotname = $2;
					cmd->wait = true;
					$$ = (Node *) cmd;
				}
			;

/* ALTER_REPLICATION_SLOT slot (options) */
alter_replication_slot:
			K_ALTER_REPLICATION_SLOT IDENT '(' generic_option_list ')'
				{
					AlterReplicationSlotCmd *cmd;
					cmd = makeNode(AlterReplicationSlotCmd);
					cmd->slotname = $2;
					cmd->options = $4;
					$$ = (Node *) cmd;
				}
			;

/*
 * START_REPLICATION [SLOT slot] [PHYSICAL] %X/%08X [TIMELINE %u]
 */
start_replication:
			K_START_REPLICATION opt_slot opt_physical RECPTR opt_timeline
				{
					StartReplicationCmd *cmd;

					cmd = makeNode(StartReplicationCmd);
					cmd->kind = REPLICATION_KIND_PHYSICAL;
					cmd->slotname = $2;
					cmd->startpoint = $4;
					cmd->timeline = $5;
					$$ = (Node *) cmd;
				}
			;

/* START_REPLICATION SLOT slot LOGICAL %X/%08X options */
start_logical_replication:
			K_START_REPLICATION K_SLOT IDENT K_LOGICAL RECPTR plugin_options
				{
					StartReplicationCmd *cmd;
					cmd = makeNode(StartReplicationCmd);
					cmd->kind = REPLICATION_KIND_LOGICAL;
					cmd->slotname = $3;
					cmd->startpoint = $5;
					cmd->options = $6;
					$$ = (Node *) cmd;
				}
			;
/*
 * TIMELINE_HISTORY %u
 */
timeline_history:
			K_TIMELINE_HISTORY UCONST
				{
					TimeLineHistoryCmd *cmd;

					if ($2 <= 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("invalid timeline %u", $2)));

					cmd = makeNode(TimeLineHistoryCmd);
					cmd->timeline = $2;

					$$ = (Node *) cmd;
				}
			;

/* UPLOAD_MANIFEST doesn't currently accept any arguments */
upload_manifest:
			K_UPLOAD_MANIFEST
				{
					UploadManifestCmd *cmd = makeNode(UploadManifestCmd);

					$$ = (Node *) cmd;
				}

opt_physical:
			K_PHYSICAL
			| /* EMPTY */
			;

opt_temporary:
			K_TEMPORARY						{ $$ = true; }
			| /* EMPTY */					{ $$ = false; }
			;

opt_slot:
			K_SLOT IDENT
				{ $$ = $2; }
			| /* EMPTY */
				{ $$ = NULL; }
			;

opt_timeline:
			K_TIMELINE UCONST
				{
					if ($2 <= 0)
						ereport(ERROR,
								(errcode(ERRCODE_SYNTAX_ERROR),
								 errmsg("invalid timeline %u", $2)));
					$$ = $2;
				}
				| /* EMPTY */			{ $$ = 0; }
			;


plugin_options:
			'(' plugin_opt_list ')'			{ $$ = $2; }
			| /* EMPTY */					{ $$ = NIL; }
		;

plugin_opt_list:
			plugin_opt_elem
				{
					$$ = list_make1($1);
				}
			| plugin_opt_list ',' plugin_opt_elem
				{
					$$ = lappend($1, $3);
				}
		;

plugin_opt_elem:
			IDENT plugin_opt_arg
				{
					$$ = makeDefElem($1, $2, -1);
				}
		;

plugin_opt_arg:
			SCONST							{ $$ = (Node *) makeString($1); }
			| /* EMPTY */					{ $$ = NULL; }
		;

generic_option_list:
			generic_option_list ',' generic_option
				{ $$ = lappend($1, $3); }
			| generic_option
				{ $$ = list_make1($1); }
			;

generic_option:
			ident_or_keyword
				{
					$$ = makeDefElem($1, NULL, -1);
				}
			| ident_or_keyword IDENT
				{
					$$ = makeDefElem($1, (Node *) makeString($2), -1);
				}
			| ident_or_keyword SCONST
				{
					$$ = makeDefElem($1, (Node *) makeString($2), -1);
				}
			| ident_or_keyword UCONST
				{
					$$ = makeDefElem($1, (Node *) makeInteger($2), -1);
				}
			;

ident_or_keyword:
			IDENT							{ $$ = $1; }
			| K_BASE_BACKUP					{ $$ = "base_backup"; }
			| K_IDENTIFY_SYSTEM				{ $$ = "identify_system"; }
			| K_SHOW						{ $$ = "show"; }
			| K_START_REPLICATION			{ $$ = "start_replication"; }
			| K_CREATE_REPLICATION_SLOT	{ $$ = "create_replication_slot"; }
			| K_DROP_REPLICATION_SLOT		{ $$ = "drop_replication_slot"; }
			| K_ALTER_REPLICATION_SLOT		{ $$ = "alter_replication_slot"; }
			| K_TIMELINE_HISTORY			{ $$ = "timeline_history"; }
			| K_WAIT						{ $$ = "wait"; }
			| K_TIMELINE					{ $$ = "timeline"; }
			| K_PHYSICAL					{ $$ = "physical"; }
			| K_LOGICAL						{ $$ = "logical"; }
			| K_SLOT						{ $$ = "slot"; }
			| K_RESERVE_WAL					{ $$ = "reserve_wal"; }
			| K_TEMPORARY					{ $$ = "temporary"; }
			| K_TWO_PHASE					{ $$ = "two_phase"; }
			| K_EXPORT_SNAPSHOT				{ $$ = "export_snapshot"; }
			| K_NOEXPORT_SNAPSHOT			{ $$ = "noexport_snapshot"; }
			| K_USE_SNAPSHOT				{ $$ = "use_snapshot"; }
			| K_UPLOAD_MANIFEST				{ $$ = "upload_manifest"; }
		;

%%
