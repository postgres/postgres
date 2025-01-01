/*--------------------------------------------------------------------
 *
 * guc_funcs.c
 *
 * SQL commands and SQL-accessible functions related to GUC variables.
 *
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 * Written by Peter Eisentraut <peter_e@gmx.net>.
 *
 * IDENTIFICATION
 *	  src/backend/utils/misc/guc_funcs.c
 *
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_parameter_acl.h"
#include "funcapi.h"
#include "guc_internal.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc_tables.h"
#include "utils/snapmgr.h"

static char *flatten_set_variable_args(const char *name, List *args);
static void ShowGUCConfigOption(const char *name, DestReceiver *dest);
static void ShowAllGUCConfig(DestReceiver *dest);


/*
 * SET command
 */
void
ExecSetVariableStmt(VariableSetStmt *stmt, bool isTopLevel)
{
	GucAction	action = stmt->is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET;

	/*
	 * Workers synchronize these parameters at the start of the parallel
	 * operation; then, we block SET during the operation.
	 */
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot set parameters during a parallel operation")));

	switch (stmt->kind)
	{
		case VAR_SET_VALUE:
		case VAR_SET_CURRENT:
			if (stmt->is_local)
				WarnNoTransactionBlock(isTopLevel, "SET LOCAL");
			(void) set_config_option(stmt->name,
									 ExtractSetVariableArgs(stmt),
									 (superuser() ? PGC_SUSET : PGC_USERSET),
									 PGC_S_SESSION,
									 action, true, 0, false);
			break;
		case VAR_SET_MULTI:

			/*
			 * Special-case SQL syntaxes.  The TRANSACTION and SESSION
			 * CHARACTERISTICS cases effectively set more than one variable
			 * per statement.  TRANSACTION SNAPSHOT only takes one argument,
			 * but we put it here anyway since it's a special case and not
			 * related to any GUC variable.
			 */
			if (strcmp(stmt->name, "TRANSACTION") == 0)
			{
				ListCell   *head;

				WarnNoTransactionBlock(isTopLevel, "SET TRANSACTION");

				foreach(head, stmt->args)
				{
					DefElem    *item = (DefElem *) lfirst(head);

					if (strcmp(item->defname, "transaction_isolation") == 0)
						SetPGVariable("transaction_isolation",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_read_only") == 0)
						SetPGVariable("transaction_read_only",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_deferrable") == 0)
						SetPGVariable("transaction_deferrable",
									  list_make1(item->arg), stmt->is_local);
					else
						elog(ERROR, "unexpected SET TRANSACTION element: %s",
							 item->defname);
				}
			}
			else if (strcmp(stmt->name, "SESSION CHARACTERISTICS") == 0)
			{
				ListCell   *head;

				foreach(head, stmt->args)
				{
					DefElem    *item = (DefElem *) lfirst(head);

					if (strcmp(item->defname, "transaction_isolation") == 0)
						SetPGVariable("default_transaction_isolation",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_read_only") == 0)
						SetPGVariable("default_transaction_read_only",
									  list_make1(item->arg), stmt->is_local);
					else if (strcmp(item->defname, "transaction_deferrable") == 0)
						SetPGVariable("default_transaction_deferrable",
									  list_make1(item->arg), stmt->is_local);
					else
						elog(ERROR, "unexpected SET SESSION element: %s",
							 item->defname);
				}
			}
			else if (strcmp(stmt->name, "TRANSACTION SNAPSHOT") == 0)
			{
				A_Const    *con = linitial_node(A_Const, stmt->args);

				if (stmt->is_local)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("SET LOCAL TRANSACTION SNAPSHOT is not implemented")));

				WarnNoTransactionBlock(isTopLevel, "SET TRANSACTION");
				ImportSnapshot(strVal(&con->val));
			}
			else
				elog(ERROR, "unexpected SET MULTI element: %s",
					 stmt->name);
			break;
		case VAR_SET_DEFAULT:
			if (stmt->is_local)
				WarnNoTransactionBlock(isTopLevel, "SET LOCAL");
			/* fall through */
		case VAR_RESET:
			(void) set_config_option(stmt->name,
									 NULL,
									 (superuser() ? PGC_SUSET : PGC_USERSET),
									 PGC_S_SESSION,
									 action, true, 0, false);
			break;
		case VAR_RESET_ALL:
			ResetAllOptions();
			break;
	}

	/* Invoke the post-alter hook for setting this GUC variable, by name. */
	InvokeObjectPostAlterHookArgStr(ParameterAclRelationId, stmt->name,
									ACL_SET, stmt->kind, false);
}

/*
 * Get the value to assign for a VariableSetStmt, or NULL if it's RESET.
 * The result is palloc'd.
 *
 * This is exported for use by actions such as ALTER ROLE SET.
 */
char *
ExtractSetVariableArgs(VariableSetStmt *stmt)
{
	switch (stmt->kind)
	{
		case VAR_SET_VALUE:
			return flatten_set_variable_args(stmt->name, stmt->args);
		case VAR_SET_CURRENT:
			return GetConfigOptionByName(stmt->name, NULL, false);
		default:
			return NULL;
	}
}

/*
 * flatten_set_variable_args
 *		Given a parsenode List as emitted by the grammar for SET,
 *		convert to the flat string representation used by GUC.
 *
 * We need to be told the name of the variable the args are for, because
 * the flattening rules vary (ugh).
 *
 * The result is NULL if args is NIL (i.e., SET ... TO DEFAULT), otherwise
 * a palloc'd string.
 */
static char *
flatten_set_variable_args(const char *name, List *args)
{
	struct config_generic *record;
	int			flags;
	StringInfoData buf;
	ListCell   *l;

	/* Fast path if just DEFAULT */
	if (args == NIL)
		return NULL;

	/*
	 * Get flags for the variable; if it's not known, use default flags.
	 * (Caller might throw error later, but not our business to do so here.)
	 */
	record = find_option(name, false, true, WARNING);
	if (record)
		flags = record->flags;
	else
		flags = 0;

	/* Complain if list input and non-list variable */
	if ((flags & GUC_LIST_INPUT) == 0 &&
		list_length(args) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("SET %s takes only one argument", name)));

	initStringInfo(&buf);

	/*
	 * Each list member may be a plain A_Const node, or an A_Const within a
	 * TypeCast; the latter case is supported only for ConstInterval arguments
	 * (for SET TIME ZONE).
	 */
	foreach(l, args)
	{
		Node	   *arg = (Node *) lfirst(l);
		char	   *val;
		TypeName   *typeName = NULL;
		A_Const    *con;

		if (l != list_head(args))
			appendStringInfoString(&buf, ", ");

		if (IsA(arg, TypeCast))
		{
			TypeCast   *tc = (TypeCast *) arg;

			arg = tc->arg;
			typeName = tc->typeName;
		}

		if (!IsA(arg, A_Const))
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(arg));
		con = (A_Const *) arg;

		switch (nodeTag(&con->val))
		{
			case T_Integer:
				appendStringInfo(&buf, "%d", intVal(&con->val));
				break;
			case T_Float:
				/* represented as a string, so just copy it */
				appendStringInfoString(&buf, castNode(Float, &con->val)->fval);
				break;
			case T_String:
				val = strVal(&con->val);
				if (typeName != NULL)
				{
					/*
					 * Must be a ConstInterval argument for TIME ZONE. Coerce
					 * to interval and back to normalize the value and account
					 * for any typmod.
					 */
					Oid			typoid;
					int32		typmod;
					Datum		interval;
					char	   *intervalout;

					typenameTypeIdAndMod(NULL, typeName, &typoid, &typmod);
					Assert(typoid == INTERVALOID);

					interval =
						DirectFunctionCall3(interval_in,
											CStringGetDatum(val),
											ObjectIdGetDatum(InvalidOid),
											Int32GetDatum(typmod));

					intervalout =
						DatumGetCString(DirectFunctionCall1(interval_out,
															interval));
					appendStringInfo(&buf, "INTERVAL '%s'", intervalout);
				}
				else
				{
					/*
					 * Plain string literal or identifier.  For quote mode,
					 * quote it if it's not a vanilla identifier.
					 */
					if (flags & GUC_LIST_QUOTE)
						appendStringInfoString(&buf, quote_identifier(val));
					else
						appendStringInfoString(&buf, val);
				}
				break;
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(&con->val));
				break;
		}
	}

	return buf.data;
}

/*
 * SetPGVariable - SET command exported as an easily-C-callable function.
 *
 * This provides access to SET TO value, as well as SET TO DEFAULT (expressed
 * by passing args == NIL), but not SET FROM CURRENT functionality.
 */
void
SetPGVariable(const char *name, List *args, bool is_local)
{
	char	   *argstring = flatten_set_variable_args(name, args);

	/* Note SET DEFAULT (argstring == NULL) is equivalent to RESET */
	(void) set_config_option(name,
							 argstring,
							 (superuser() ? PGC_SUSET : PGC_USERSET),
							 PGC_S_SESSION,
							 is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
							 true, 0, false);
}

/*
 * SET command wrapped as a SQL callable function.
 */
Datum
set_config_by_name(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *value;
	char	   *new_value;
	bool		is_local;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("SET requires parameter name")));

	/* Get the GUC variable name */
	name = TextDatumGetCString(PG_GETARG_DATUM(0));

	/* Get the desired value or set to NULL for a reset request */
	if (PG_ARGISNULL(1))
		value = NULL;
	else
		value = TextDatumGetCString(PG_GETARG_DATUM(1));

	/*
	 * Get the desired state of is_local. Default to false if provided value
	 * is NULL
	 */
	if (PG_ARGISNULL(2))
		is_local = false;
	else
		is_local = PG_GETARG_BOOL(2);

	/* Note SET DEFAULT (argstring == NULL) is equivalent to RESET */
	(void) set_config_option(name,
							 value,
							 (superuser() ? PGC_SUSET : PGC_USERSET),
							 PGC_S_SESSION,
							 is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET,
							 true, 0, false);

	/* get the new current value */
	new_value = GetConfigOptionByName(name, NULL, false);

	/* Convert return string to text */
	PG_RETURN_TEXT_P(cstring_to_text(new_value));
}


/*
 * SHOW command
 */
void
GetPGVariable(const char *name, DestReceiver *dest)
{
	if (guc_name_compare(name, "all") == 0)
		ShowAllGUCConfig(dest);
	else
		ShowGUCConfigOption(name, dest);
}

/*
 * Get a tuple descriptor for SHOW's result
 */
TupleDesc
GetPGVariableResultDesc(const char *name)
{
	TupleDesc	tupdesc;

	if (guc_name_compare(name, "all") == 0)
	{
		/* need a tuple descriptor representing three TEXT columns */
		tupdesc = CreateTemplateTupleDesc(3);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "description",
						   TEXTOID, -1, 0);
	}
	else
	{
		const char *varname;

		/* Get the canonical spelling of name */
		(void) GetConfigOptionByName(name, &varname, false);

		/* need a tuple descriptor representing a single TEXT column */
		tupdesc = CreateTemplateTupleDesc(1);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, varname,
						   TEXTOID, -1, 0);
	}
	return tupdesc;
}

/*
 * SHOW one variable
 */
static void
ShowGUCConfigOption(const char *name, DestReceiver *dest)
{
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	const char *varname;
	char	   *value;

	/* Get the value and canonical spelling of name */
	value = GetConfigOptionByName(name, &varname, false);

	/* need a tuple descriptor representing a single TEXT column */
	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, varname,
							  TEXTOID, -1, 0);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	/* Send it */
	do_text_output_oneline(tstate, value);

	end_tup_output(tstate);
}

/*
 * SHOW ALL command
 */
static void
ShowAllGUCConfig(DestReceiver *dest)
{
	struct config_generic **guc_vars;
	int			num_vars;
	TupOutputState *tstate;
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		isnull[3] = {false, false, false};

	/* collect the variables, in sorted order */
	guc_vars = get_guc_variables(&num_vars);

	/* need a tuple descriptor representing three TEXT columns */
	tupdesc = CreateTemplateTupleDesc(3);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 1, "name",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 2, "setting",
							  TEXTOID, -1, 0);
	TupleDescInitBuiltinEntry(tupdesc, (AttrNumber) 3, "description",
							  TEXTOID, -1, 0);

	/* prepare for projection of tuples */
	tstate = begin_tup_output_tupdesc(dest, tupdesc, &TTSOpsVirtual);

	for (int i = 0; i < num_vars; i++)
	{
		struct config_generic *conf = guc_vars[i];
		char	   *setting;

		/* skip if marked NO_SHOW_ALL */
		if (conf->flags & GUC_NO_SHOW_ALL)
			continue;

		/* return only options visible to the current user */
		if (!ConfigOptionIsVisible(conf))
			continue;

		/* assign to the values array */
		values[0] = PointerGetDatum(cstring_to_text(conf->name));

		setting = ShowGUCOption(conf, true);
		if (setting)
		{
			values[1] = PointerGetDatum(cstring_to_text(setting));
			isnull[1] = false;
		}
		else
		{
			values[1] = PointerGetDatum(NULL);
			isnull[1] = true;
		}

		if (conf->short_desc)
		{
			values[2] = PointerGetDatum(cstring_to_text(conf->short_desc));
			isnull[2] = false;
		}
		else
		{
			values[2] = PointerGetDatum(NULL);
			isnull[2] = true;
		}

		/* send it to dest */
		do_tup_output(tstate, values, isnull);

		/* clean up */
		pfree(DatumGetPointer(values[0]));
		if (setting)
		{
			pfree(setting);
			pfree(DatumGetPointer(values[1]));
		}
		if (conf->short_desc)
			pfree(DatumGetPointer(values[2]));
	}

	end_tup_output(tstate);
}

/*
 * Return some of the flags associated to the specified GUC in the shape of
 * a text array, and NULL if it does not exist.  An empty array is returned
 * if the GUC exists without any meaningful flags to show.
 */
Datum
pg_settings_get_flags(PG_FUNCTION_ARGS)
{
#define MAX_GUC_FLAGS	6
	char	   *varname = TextDatumGetCString(PG_GETARG_DATUM(0));
	struct config_generic *record;
	int			cnt = 0;
	Datum		flags[MAX_GUC_FLAGS];
	ArrayType  *a;

	record = find_option(varname, false, true, ERROR);

	/* return NULL if no such variable */
	if (record == NULL)
		PG_RETURN_NULL();

	if (record->flags & GUC_EXPLAIN)
		flags[cnt++] = CStringGetTextDatum("EXPLAIN");
	if (record->flags & GUC_NO_RESET)
		flags[cnt++] = CStringGetTextDatum("NO_RESET");
	if (record->flags & GUC_NO_RESET_ALL)
		flags[cnt++] = CStringGetTextDatum("NO_RESET_ALL");
	if (record->flags & GUC_NO_SHOW_ALL)
		flags[cnt++] = CStringGetTextDatum("NO_SHOW_ALL");
	if (record->flags & GUC_NOT_IN_SAMPLE)
		flags[cnt++] = CStringGetTextDatum("NOT_IN_SAMPLE");
	if (record->flags & GUC_RUNTIME_COMPUTED)
		flags[cnt++] = CStringGetTextDatum("RUNTIME_COMPUTED");

	Assert(cnt <= MAX_GUC_FLAGS);

	/* Returns the record as Datum */
	a = construct_array_builtin(flags, cnt, TEXTOID);
	PG_RETURN_ARRAYTYPE_P(a);
}

/*
 * Return whether or not the GUC variable is visible to the current user.
 */
bool
ConfigOptionIsVisible(struct config_generic *conf)
{
	if ((conf->flags & GUC_SUPERUSER_ONLY) &&
		!has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_SETTINGS))
		return false;
	else
		return true;
}

/*
 * Extract fields to show in pg_settings for given variable.
 */
static void
GetConfigOptionValues(struct config_generic *conf, const char **values)
{
	char		buffer[256];

	/* first get the generic attributes */

	/* name */
	values[0] = conf->name;

	/* setting: use ShowGUCOption in order to avoid duplicating the logic */
	values[1] = ShowGUCOption(conf, false);

	/* unit, if any (NULL is fine) */
	values[2] = get_config_unit_name(conf->flags);

	/* group */
	values[3] = _(config_group_names[conf->group]);

	/* short_desc */
	values[4] = conf->short_desc != NULL ? _(conf->short_desc) : NULL;

	/* extra_desc */
	values[5] = conf->long_desc != NULL ? _(conf->long_desc) : NULL;

	/* context */
	values[6] = GucContext_Names[conf->context];

	/* vartype */
	values[7] = config_type_names[conf->vartype];

	/* source */
	values[8] = GucSource_Names[conf->source];

	/* now get the type specific attributes */
	switch (conf->vartype)
	{
		case PGC_BOOL:
			{
				struct config_bool *lconf = (struct config_bool *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				values[12] = pstrdup(lconf->boot_val ? "on" : "off");

				/* reset_val */
				values[13] = pstrdup(lconf->reset_val ? "on" : "off");
			}
			break;

		case PGC_INT:
			{
				struct config_int *lconf = (struct config_int *) conf;

				/* min_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->min);
				values[9] = pstrdup(buffer);

				/* max_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->max);
				values[10] = pstrdup(buffer);

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->boot_val);
				values[12] = pstrdup(buffer);

				/* reset_val */
				snprintf(buffer, sizeof(buffer), "%d", lconf->reset_val);
				values[13] = pstrdup(buffer);
			}
			break;

		case PGC_REAL:
			{
				struct config_real *lconf = (struct config_real *) conf;

				/* min_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->min);
				values[9] = pstrdup(buffer);

				/* max_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->max);
				values[10] = pstrdup(buffer);

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->boot_val);
				values[12] = pstrdup(buffer);

				/* reset_val */
				snprintf(buffer, sizeof(buffer), "%g", lconf->reset_val);
				values[13] = pstrdup(buffer);
			}
			break;

		case PGC_STRING:
			{
				struct config_string *lconf = (struct config_string *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				if (lconf->boot_val == NULL)
					values[12] = NULL;
				else
					values[12] = pstrdup(lconf->boot_val);

				/* reset_val */
				if (lconf->reset_val == NULL)
					values[13] = NULL;
				else
					values[13] = pstrdup(lconf->reset_val);
			}
			break;

		case PGC_ENUM:
			{
				struct config_enum *lconf = (struct config_enum *) conf;

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */

				/*
				 * NOTE! enumvals with double quotes in them are not
				 * supported!
				 */
				values[11] = config_enum_get_options((struct config_enum *) conf,
													 "{\"", "\"}", "\",\"");

				/* boot_val */
				values[12] = pstrdup(config_enum_lookup_by_value(lconf,
																 lconf->boot_val));

				/* reset_val */
				values[13] = pstrdup(config_enum_lookup_by_value(lconf,
																 lconf->reset_val));
			}
			break;

		default:
			{
				/*
				 * should never get here, but in case we do, set 'em to NULL
				 */

				/* min_val */
				values[9] = NULL;

				/* max_val */
				values[10] = NULL;

				/* enumvals */
				values[11] = NULL;

				/* boot_val */
				values[12] = NULL;

				/* reset_val */
				values[13] = NULL;
			}
			break;
	}

	/*
	 * If the setting came from a config file, set the source location. For
	 * security reasons, we don't show source file/line number for
	 * insufficiently-privileged users.
	 */
	if (conf->source == PGC_S_FILE &&
		has_privs_of_role(GetUserId(), ROLE_PG_READ_ALL_SETTINGS))
	{
		values[14] = conf->sourcefile;
		snprintf(buffer, sizeof(buffer), "%d", conf->sourceline);
		values[15] = pstrdup(buffer);
	}
	else
	{
		values[14] = NULL;
		values[15] = NULL;
	}

	values[16] = (conf->status & GUC_PENDING_RESTART) ? "t" : "f";
}

/*
 * show_config_by_name - equiv to SHOW X command but implemented as
 * a function.
 */
Datum
show_config_by_name(PG_FUNCTION_ARGS)
{
	char	   *varname = TextDatumGetCString(PG_GETARG_DATUM(0));
	char	   *varval;

	/* Get the value */
	varval = GetConfigOptionByName(varname, NULL, false);

	/* Convert to text */
	PG_RETURN_TEXT_P(cstring_to_text(varval));
}

/*
 * show_config_by_name_missing_ok - equiv to SHOW X command but implemented as
 * a function.  If X does not exist, suppress the error and just return NULL
 * if missing_ok is true.
 */
Datum
show_config_by_name_missing_ok(PG_FUNCTION_ARGS)
{
	char	   *varname = TextDatumGetCString(PG_GETARG_DATUM(0));
	bool		missing_ok = PG_GETARG_BOOL(1);
	char	   *varval;

	/* Get the value */
	varval = GetConfigOptionByName(varname, NULL, missing_ok);

	/* return NULL if no such variable */
	if (varval == NULL)
		PG_RETURN_NULL();

	/* Convert to text */
	PG_RETURN_TEXT_P(cstring_to_text(varval));
}

/*
 * show_all_settings - equiv to SHOW ALL command but implemented as
 * a Table Function.
 */
#define NUM_PG_SETTINGS_ATTS	17

Datum
show_all_settings(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	struct config_generic **guc_vars;
	int			num_vars;
	TupleDesc	tupdesc;
	int			call_cntr;
	int			max_calls;
	AttInMetadata *attinmeta;
	MemoryContext oldcontext;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/*
		 * switch to memory context appropriate for multiple function calls
		 */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/*
		 * need a tuple descriptor representing NUM_PG_SETTINGS_ATTS columns
		 * of the appropriate types
		 */
		tupdesc = CreateTemplateTupleDesc(NUM_PG_SETTINGS_ATTS);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "setting",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "unit",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 4, "category",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 5, "short_desc",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 6, "extra_desc",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 7, "context",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 8, "vartype",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 9, "source",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 10, "min_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 11, "max_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 12, "enumvals",
						   TEXTARRAYOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 13, "boot_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 14, "reset_val",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 15, "sourcefile",
						   TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 16, "sourceline",
						   INT4OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 17, "pending_restart",
						   BOOLOID, -1, 0);

		/*
		 * Generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		/* collect the variables, in sorted order */
		guc_vars = get_guc_variables(&num_vars);

		/* use user_fctx to remember the array location */
		funcctx->user_fctx = guc_vars;

		/* total number of tuples to be returned */
		funcctx->max_calls = num_vars;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	guc_vars = (struct config_generic **) funcctx->user_fctx;
	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	while (call_cntr < max_calls)	/* do when there is more left to send */
	{
		struct config_generic *conf = guc_vars[call_cntr];
		char	   *values[NUM_PG_SETTINGS_ATTS];
		HeapTuple	tuple;
		Datum		result;

		/* skip if marked NO_SHOW_ALL or if not visible to current user */
		if ((conf->flags & GUC_NO_SHOW_ALL) ||
			!ConfigOptionIsVisible(conf))
		{
			call_cntr = ++funcctx->call_cntr;
			continue;
		}

		/* extract values for the current variable */
		GetConfigOptionValues(conf, (const char **) values);

		/* build a tuple */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* make the tuple into a datum */
		result = HeapTupleGetDatum(tuple);

		SRF_RETURN_NEXT(funcctx, result);
	}

	/* do when there is no more left */
	SRF_RETURN_DONE(funcctx);
}

/*
 * show_all_file_settings
 *
 * Returns a table of all parameter settings in all configuration files
 * which includes the config file pathname, the line number, a sequence number
 * indicating the order in which the settings were encountered, the parameter
 * name and value, a bool showing if the value could be applied, and possibly
 * an associated error message.  (For problems such as syntax errors, the
 * parameter name/value might be NULL.)
 *
 * Note: no filtering is done here, instead we depend on the GRANT system
 * to prevent unprivileged users from accessing this function or the view
 * built on top of it.
 */
Datum
show_all_file_settings(PG_FUNCTION_ARGS)
{
#define NUM_PG_FILE_SETTINGS_ATTS 7
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ConfigVariable *conf;
	int			seqno;

	/* Scan the config files using current context as workspace */
	conf = ProcessConfigFileInternal(PGC_SIGHUP, false, DEBUG3);

	/* Build a tuplestore to return our results in */
	InitMaterializedSRF(fcinfo, 0);

	/* Process the results and create a tuplestore */
	for (seqno = 1; conf != NULL; conf = conf->next, seqno++)
	{
		Datum		values[NUM_PG_FILE_SETTINGS_ATTS];
		bool		nulls[NUM_PG_FILE_SETTINGS_ATTS];

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		/* sourcefile */
		if (conf->filename)
			values[0] = PointerGetDatum(cstring_to_text(conf->filename));
		else
			nulls[0] = true;

		/* sourceline (not meaningful if no sourcefile) */
		if (conf->filename)
			values[1] = Int32GetDatum(conf->sourceline);
		else
			nulls[1] = true;

		/* seqno */
		values[2] = Int32GetDatum(seqno);

		/* name */
		if (conf->name)
			values[3] = PointerGetDatum(cstring_to_text(conf->name));
		else
			nulls[3] = true;

		/* setting */
		if (conf->value)
			values[4] = PointerGetDatum(cstring_to_text(conf->value));
		else
			nulls[4] = true;

		/* applied */
		values[5] = BoolGetDatum(conf->applied);

		/* error */
		if (conf->errmsg)
			values[6] = PointerGetDatum(cstring_to_text(conf->errmsg));
		else
			nulls[6] = true;

		/* shove row into tuplestore */
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	return (Datum) 0;
}
