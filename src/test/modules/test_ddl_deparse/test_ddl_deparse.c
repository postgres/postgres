/*----------------------------------------------------------------------
 * test_ddl_deparse.c
 *		Support functions for the test_ddl_deparse module
 *
 * Copyright (c) 2014-2020, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/test/modules/test_ddl_deparse/test_ddl_deparse.c
 *----------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "tcop/deparse_utility.h"
#include "tcop/utility.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(get_command_type);
PG_FUNCTION_INFO_V1(get_command_tag);
PG_FUNCTION_INFO_V1(get_altertable_subcmdtypes);

/*
 * Return the textual representation of the struct type used to represent a
 * command in struct CollectedCommand format.
 */
Datum
get_command_type(PG_FUNCTION_ARGS)
{
	CollectedCommand *cmd = (CollectedCommand *) PG_GETARG_POINTER(0);
	const char *type;

	switch (cmd->type)
	{
		case SCT_Simple:
			type = "simple";
			break;
		case SCT_AlterTable:
			type = "alter table";
			break;
		case SCT_Grant:
			type = "grant";
			break;
		case SCT_AlterOpFamily:
			type = "alter operator family";
			break;
		case SCT_AlterDefaultPrivileges:
			type = "alter default privileges";
			break;
		case SCT_CreateOpClass:
			type = "create operator class";
			break;
		case SCT_AlterTSConfig:
			type = "alter text search configuration";
			break;
		default:
			type = "unknown command type";
			break;
	}

	PG_RETURN_TEXT_P(cstring_to_text(type));
}

/*
 * Return the command tag corresponding to a parse node contained in a
 * CollectedCommand struct.
 */
Datum
get_command_tag(PG_FUNCTION_ARGS)
{
	CollectedCommand *cmd = (CollectedCommand *) PG_GETARG_POINTER(0);

	if (!cmd->parsetree)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text(CreateCommandName(cmd->parsetree)));
}

/*
 * Return a text array representation of the subcommands of an ALTER TABLE
 * command.
 */
Datum
get_altertable_subcmdtypes(PG_FUNCTION_ARGS)
{
	CollectedCommand *cmd = (CollectedCommand *) PG_GETARG_POINTER(0);
	ArrayBuildState *astate = NULL;
	ListCell   *cell;

	if (cmd->type != SCT_AlterTable)
		elog(ERROR, "command is not ALTER TABLE");

	foreach(cell, cmd->d.alterTable.subcmds)
	{
		CollectedATSubcmd *sub = lfirst(cell);
		AlterTableCmd *subcmd = castNode(AlterTableCmd, sub->parsetree);
		const char *strtype;

		switch (subcmd->subtype)
		{
			case AT_AddColumn:
				strtype = "ADD COLUMN";
				break;
			case AT_AddColumnRecurse:
				strtype = "ADD COLUMN (and recurse)";
				break;
			case AT_AddColumnToView:
				strtype = "ADD COLUMN TO VIEW";
				break;
			case AT_ColumnDefault:
				strtype = "ALTER COLUMN SET DEFAULT";
				break;
			case AT_CookedColumnDefault:
				strtype = "ALTER COLUMN SET DEFAULT (precooked)";
				break;
			case AT_DropNotNull:
				strtype = "DROP NOT NULL";
				break;
			case AT_SetNotNull:
				strtype = "SET NOT NULL";
				break;
			case AT_CheckNotNull:
				strtype = "CHECK NOT NULL";
				break;
			case AT_SetStatistics:
				strtype = "SET STATS";
				break;
			case AT_SetOptions:
				strtype = "SET OPTIONS";
				break;
			case AT_ResetOptions:
				strtype = "RESET OPTIONS";
				break;
			case AT_SetStorage:
				strtype = "SET STORAGE";
				break;
			case AT_DropColumn:
				strtype = "DROP COLUMN";
				break;
			case AT_DropColumnRecurse:
				strtype = "DROP COLUMN (and recurse)";
				break;
			case AT_AddIndex:
				strtype = "ADD INDEX";
				break;
			case AT_ReAddIndex:
				strtype = "(re) ADD INDEX";
				break;
			case AT_AddConstraint:
				strtype = "ADD CONSTRAINT";
				break;
			case AT_AddConstraintRecurse:
				strtype = "ADD CONSTRAINT (and recurse)";
				break;
			case AT_ReAddConstraint:
				strtype = "(re) ADD CONSTRAINT";
				break;
			case AT_AlterConstraint:
				strtype = "ALTER CONSTRAINT";
				break;
			case AT_ValidateConstraint:
				strtype = "VALIDATE CONSTRAINT";
				break;
			case AT_ValidateConstraintRecurse:
				strtype = "VALIDATE CONSTRAINT (and recurse)";
				break;
			case AT_AddIndexConstraint:
				strtype = "ADD CONSTRAINT (using index)";
				break;
			case AT_DropConstraint:
				strtype = "DROP CONSTRAINT";
				break;
			case AT_DropConstraintRecurse:
				strtype = "DROP CONSTRAINT (and recurse)";
				break;
			case AT_ReAddComment:
				strtype = "(re) ADD COMMENT";
				break;
			case AT_AlterColumnType:
				strtype = "ALTER COLUMN SET TYPE";
				break;
			case AT_AlterColumnGenericOptions:
				strtype = "ALTER COLUMN SET OPTIONS";
				break;
			case AT_ChangeOwner:
				strtype = "CHANGE OWNER";
				break;
			case AT_ClusterOn:
				strtype = "CLUSTER";
				break;
			case AT_DropCluster:
				strtype = "DROP CLUSTER";
				break;
			case AT_SetLogged:
				strtype = "SET LOGGED";
				break;
			case AT_SetUnLogged:
				strtype = "SET UNLOGGED";
				break;
			case AT_DropOids:
				strtype = "DROP OIDS";
				break;
			case AT_SetTableSpace:
				strtype = "SET TABLESPACE";
				break;
			case AT_SetRelOptions:
				strtype = "SET RELOPTIONS";
				break;
			case AT_ResetRelOptions:
				strtype = "RESET RELOPTIONS";
				break;
			case AT_ReplaceRelOptions:
				strtype = "REPLACE RELOPTIONS";
				break;
			case AT_EnableTrig:
				strtype = "ENABLE TRIGGER";
				break;
			case AT_EnableAlwaysTrig:
				strtype = "ENABLE TRIGGER (always)";
				break;
			case AT_EnableReplicaTrig:
				strtype = "ENABLE TRIGGER (replica)";
				break;
			case AT_DisableTrig:
				strtype = "DISABLE TRIGGER";
				break;
			case AT_EnableTrigAll:
				strtype = "ENABLE TRIGGER (all)";
				break;
			case AT_DisableTrigAll:
				strtype = "DISABLE TRIGGER (all)";
				break;
			case AT_EnableTrigUser:
				strtype = "ENABLE TRIGGER (user)";
				break;
			case AT_DisableTrigUser:
				strtype = "DISABLE TRIGGER (user)";
				break;
			case AT_EnableRule:
				strtype = "ENABLE RULE";
				break;
			case AT_EnableAlwaysRule:
				strtype = "ENABLE RULE (always)";
				break;
			case AT_EnableReplicaRule:
				strtype = "ENABLE RULE (replica)";
				break;
			case AT_DisableRule:
				strtype = "DISABLE RULE";
				break;
			case AT_AddInherit:
				strtype = "ADD INHERIT";
				break;
			case AT_DropInherit:
				strtype = "DROP INHERIT";
				break;
			case AT_AddOf:
				strtype = "OF";
				break;
			case AT_DropOf:
				strtype = "NOT OF";
				break;
			case AT_ReplicaIdentity:
				strtype = "REPLICA IDENTITY";
				break;
			case AT_EnableRowSecurity:
				strtype = "ENABLE ROW SECURITY";
				break;
			case AT_DisableRowSecurity:
				strtype = "DISABLE ROW SECURITY";
				break;
			case AT_ForceRowSecurity:
				strtype = "FORCE ROW SECURITY";
				break;
			case AT_NoForceRowSecurity:
				strtype = "NO FORCE ROW SECURITY";
				break;
			case AT_GenericOptions:
				strtype = "SET OPTIONS";
				break;
			default:
				strtype = "unrecognized";
				break;
		}

		astate =
			accumArrayResult(astate, CStringGetTextDatum(strtype),
							 false, TEXTOID, CurrentMemoryContext);
	}

	if (astate == NULL)
		elog(ERROR, "empty alter table subcommand list");

	PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate, CurrentMemoryContext));
}
