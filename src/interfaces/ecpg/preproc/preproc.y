/* header */
/* $PostgreSQL: pgsql/src/interfaces/ecpg/preproc/preproc.y,v 1.382 2008/11/11 11:41:23 meskes Exp $ */

/* Copyright comment */
%{
#include "postgres_fe.h"

#include "extern.h"
#include <unistd.h>

/* Location tracking support --- simpler than bison's default */
#define YYLLOC_DEFAULT(Current, Rhs, N) \
	do { \
                if (N) \
			(Current) = (Rhs)[1]; \
		else \
		        (Current) = (Rhs)[0]; \
	} while (0)

/*
 * The %name-prefix option below will make bison call base_yylex, but we
 * really want it to call filtered_base_yylex (see parser.c).
 */
#define base_yylex filtered_base_yylex

/*
 * Variables containing simple states.
 */
int struct_level = 0;
int braces_open; /* brace level counter */
int ecpg_informix_var = 0;
char	*connection = NULL;
char	*input_filename = NULL;

static int	QueryIsRule = 0, FoundInto = 0;
static int	initializer = 0;
static int	pacounter = 1;
static char     pacounter_buffer[sizeof(int) * CHAR_BIT * 10 / 3]; /* a rough guess at the size we need */
static struct this_type actual_type[STRUCT_DEPTH];
static char *actual_startline[STRUCT_DEPTH];

/* temporarily store struct members while creating the data structure */
struct ECPGstruct_member *struct_member_list[STRUCT_DEPTH] = { NULL };

/* also store struct type so we can do a sizeof() later */
static char *ECPGstruct_sizeof = NULL;

/* for forward declarations we have to store some data as well */
static char *forward_name = NULL;

struct ECPGtype ecpg_no_indicator = {ECPGt_NO_INDICATOR, NULL, NULL, {NULL}, 0};
struct variable no_indicator = {"no_indicator", &ecpg_no_indicator, 0, NULL};

struct ECPGtype ecpg_query = {ECPGt_char_variable, NULL, NULL, {NULL}, 0};

/*
 * Handle parsing errors and warnings
 */
void
mmerror(int error_code, enum errortype type, char * error, ...)
{
	va_list ap;

	/* internationalize the error message string */
	error = _(error);

	fprintf(stderr, "%s:%d: ", input_filename, yylineno);

	switch(type)
	{
		case ET_WARNING:
			fprintf(stderr, _("WARNING: "));
			break;
		case ET_ERROR:
		case ET_FATAL:
			fprintf(stderr, _("ERROR: "));
			break;
	}

	va_start(ap, error);
	vfprintf(stderr, error, ap);
	va_end(ap);

	fprintf(stderr, "\n");

	switch(type)
	{
		case ET_WARNING:
			break;
		case ET_ERROR:
			ret_value = error_code;
			break;
		case ET_FATAL:
			if (yyin)
				fclose(yyin);
			if (yyout)
				fclose(yyout);
			if (unlink(output_filename) != 0 && *output_filename != '-')
			        fprintf(stderr, _("could not remove output file \"%s\"\n"), output_filename);
			exit(error_code);
	}
}

/*
 * string concatenation
 */

static char *
cat2_str(char *str1, char *str2)
{
	char * res_str	= (char *)mm_alloc(strlen(str1) + strlen(str2) + 2);

	strcpy(res_str, str1);
	strcat(res_str, " ");
	strcat(res_str, str2);
	free(str1);
	free(str2);
	return(res_str);
}

static char *
cat_str(int count, ...)
{
	va_list		args;
	int			i;
	char		*res_str;

	va_start(args, count);

	res_str = va_arg(args, char *);

	/* now add all other strings */
	for (i = 1; i < count; i++)
		res_str = cat2_str(res_str, va_arg(args, char *));

	va_end(args);

	return(res_str);
}

char *
make_str(const char *str)
{
	char * res_str = (char *)mm_alloc(strlen(str) + 1);

	strcpy(res_str, str);
	return res_str;
}

static char *
make2_str(char *str1, char *str2)
{
	char * res_str	= (char *)mm_alloc(strlen(str1) + strlen(str2) + 1);

	strcpy(res_str, str1);
	strcat(res_str, str2);
	free(str1);
	free(str2);
	return(res_str);
}

static char *
make3_str(char *str1, char *str2, char *str3)
{
	char * res_str	= (char *)mm_alloc(strlen(str1) + strlen(str2) +strlen(str3) + 1);

	strcpy(res_str, str1);
	strcat(res_str, str2);
	strcat(res_str, str3);
	free(str1);
	free(str2);
	free(str3);
	return(res_str);
}

/* and the rest */
static char *
make_name(void)
{
	char * name = (char *)mm_alloc(yyleng + 1);

	strncpy(name, yytext, yyleng);
	name[yyleng] = '\0';
	return(name);
}

static char *
create_questionmarks(char *name, bool array)
{
	struct variable *p = find_variable(name);
	int count;
	char *result = EMPTY;

	/* In case we have a struct, we have to print as many "?" as there are attributes in the struct
	 * An array is only allowed together with an element argument
	 * This is essantially only used for inserts, but using a struct as input parameter is an error anywhere else
	 * so we don't have to worry here. */

	if (p->type->type == ECPGt_struct || (array && p->type->type == ECPGt_array && p->type->u.element->type == ECPGt_struct))
	{
		struct ECPGstruct_member *m;

		if (p->type->type == ECPGt_struct)
			m = p->type->u.members;
		else
			m = p->type->u.element->u.members;

		for (count = 0; m != NULL; m=m->next, count++);
	}
	else
		count = 1;

	for (; count > 0; count --)
	{
		sprintf(pacounter_buffer, "$%d", pacounter++);
		result = cat_str(3, result, strdup(pacounter_buffer), make_str(" , "));
	}

	/* removed the trailing " ," */

	result[strlen(result)-3] = '\0';
	return(result);
}

static char *
adjust_informix(struct arguments *list)
{
	/* Informix accepts DECLARE with variables that are out of scope when OPEN is called.
 	 * for instance you can declare variables in a function, and then subsequently use them
	 * {
	 *      declare_vars();
	 *      exec sql ... which uses vars declared in the above function
	 *
	 * This breaks standard and leads to some very dangerous programming.
	 * Since they do, we have to work around and accept their syntax as well.
	 * But we will do so ONLY in Informix mode.
	 * We have to change the variables to our own struct and just store the pointer instead of the variable
	 */

	 struct arguments *ptr;
	 char *result = make_str("");

	 for (ptr = list; ptr != NULL; ptr = ptr->next)
	 {
	 	char temp[20]; /* this should be sufficient unless you have 8 byte integers */
		char *original_var;

	 	/* change variable name to "ECPG_informix_get_var(<counter>)" */
		original_var = ptr->variable->name;
		sprintf(temp, "%d))", ecpg_informix_var);

		if ((ptr->variable->type->type != ECPGt_varchar && ptr->variable->type->type != ECPGt_char && ptr->variable->type->type != ECPGt_unsigned_char) && atoi(ptr->variable->type->size) > 1)
		{
			ptr->variable = new_variable(cat_str(4, make_str("("), mm_strdup(ecpg_type_name(ptr->variable->type->u.element->type)), make_str(" *)(ECPG_informix_get_var("), mm_strdup(temp)), ECPGmake_array_type(ECPGmake_simple_type(ptr->variable->type->u.element->type, make_str("1"), ptr->variable->type->u.element->lineno), ptr->variable->type->size), 0);
			sprintf(temp, "%d, (", ecpg_informix_var++);
		}
		else if ((ptr->variable->type->type == ECPGt_varchar || ptr->variable->type->type == ECPGt_char || ptr->variable->type->type == ECPGt_unsigned_char) && atoi(ptr->variable->type->size) > 1)
		{
			ptr->variable = new_variable(cat_str(4, make_str("("), mm_strdup(ecpg_type_name(ptr->variable->type->type)), make_str(" *)(ECPG_informix_get_var("), mm_strdup(temp)), ECPGmake_simple_type(ptr->variable->type->type, ptr->variable->type->size, ptr->variable->type->lineno), 0);
			sprintf(temp, "%d, (", ecpg_informix_var++);
		}
		else
		{
			ptr->variable = new_variable(cat_str(4, make_str("*("), mm_strdup(ecpg_type_name(ptr->variable->type->type)), make_str(" *)(ECPG_informix_get_var("), mm_strdup(temp)), ECPGmake_simple_type(ptr->variable->type->type, ptr->variable->type->size, ptr->variable->type->lineno), 0);
			sprintf(temp, "%d, &(", ecpg_informix_var++);
		}

		/* create call to "ECPG_informix_set_var(<counter>, <pointer>. <linen number>)" */
		result = cat_str(5, result, make_str("ECPG_informix_set_var("), mm_strdup(temp), mm_strdup(original_var), make_str("), __LINE__);\n"));

		/* now the indicator if there is one */
		if (ptr->indicator->type->type != ECPGt_NO_INDICATOR)
		{
			/* change variable name to "ECPG_informix_get_var(<counter>)" */
			original_var = ptr->indicator->name;
			sprintf(temp, "%d))", ecpg_informix_var);

			/* create call to "ECPG_informix_set_var(<counter>, <pointer>. <linen number>)" */
			if (atoi(ptr->indicator->type->size) > 1)
			{
				ptr->indicator = new_variable(cat_str(4, make_str("("), mm_strdup(ecpg_type_name(ptr->indicator->type->type)), make_str(" *)(ECPG_informix_get_var("), mm_strdup(temp)), ECPGmake_simple_type(ptr->indicator->type->type, ptr->indicator->type->size, ptr->variable->type->lineno), 0);
				sprintf(temp, "%d, (", ecpg_informix_var++);
			}
			else
			{
				ptr->indicator = new_variable(cat_str(4, make_str("*("), mm_strdup(ecpg_type_name(ptr->indicator->type->type)), make_str(" *)(ECPG_informix_get_var("), mm_strdup(temp)), ECPGmake_simple_type(ptr->indicator->type->type, ptr->indicator->type->size, ptr->variable->type->lineno), 0);
				sprintf(temp, "%d, &(", ecpg_informix_var++);
			}
			result = cat_str(5, result, make_str("ECPG_informix_set_var("), mm_strdup(temp), mm_strdup(original_var), make_str("), __LINE__);\n"));
		}
	 }

	 return result;
}

static struct cursor *
add_additional_variables(char *name, bool insert)
{
	struct cursor *ptr;
	struct arguments *p;

	for (ptr = cur; ptr != NULL; ptr=ptr->next)
	{
		if (strcmp(ptr->name, name) == 0)
			break;
	}

	if (ptr == NULL)
	{
		mmerror(PARSE_ERROR, ET_ERROR, "trying to access an undeclared cursor \"%s\"\n", name);
		return NULL;
	}

	if (insert)
	{
		/* add all those input variables that were given earlier
		 * note that we have to append here but have to keep the existing order */
		for (p = ptr->argsinsert; p; p = p->next)
			add_variable_to_tail(&argsinsert, p->variable, p->indicator);
	}

	/* add all those output variables that were given earlier */
	for (p = ptr->argsresult; p; p = p->next)
		add_variable_to_tail(&argsresult, p->variable, p->indicator);

	return ptr;
}

static void
add_typedef(char *name, char * dimension, char * length, enum ECPGttype type_enum, char *type_dimension, char *type_index, int initializer, int array)
{
	/* add entry to list */
	struct typedefs *ptr, *this;

	if ((type_enum == ECPGt_struct ||
	     type_enum == ECPGt_union) &&
	    initializer == 1)
		mmerror(PARSE_ERROR, ET_ERROR, "initializer not allowed in typedef command");
	else
	{
		for (ptr = types; ptr != NULL; ptr = ptr->next)
		{
			if (strcmp(name, ptr->name) == 0)
				/* re-definition is a bug */
				mmerror(PARSE_ERROR, ET_ERROR, "type %s already defined", name);
		}
		adjust_array(type_enum, &dimension, &length, type_dimension, type_index, array, true);

		this = (struct typedefs *) mm_alloc(sizeof(struct typedefs));

		/* initial definition */
		this->next = types;
		this->name = name;
		this->brace_level = braces_open;
		this->type = (struct this_type *) mm_alloc(sizeof(struct this_type));
		this->type->type_enum = type_enum;
		this->type->type_str = mm_strdup(name);
		this->type->type_dimension = dimension; /* dimension of array */
		this->type->type_index = length;	/* length of string */
		this->type->type_sizeof = ECPGstruct_sizeof;
		this->struct_member_list = (type_enum == ECPGt_struct || type_enum == ECPGt_union) ?
		ECPGstruct_member_dup(struct_member_list[struct_level]) : NULL;

		if (type_enum != ECPGt_varchar &&
			type_enum != ECPGt_char &&
			type_enum != ECPGt_unsigned_char &&
			atoi(this->type->type_index) >= 0)
			mmerror(PARSE_ERROR, ET_ERROR, "no multidimensional array support for simple data types");

		types = this;
	}
}
%}

%name-prefix="base_yy"
%locations

%union {
	double	dval;
	char	*str;
	int     ival;
	struct	when		action;
	struct	index		index;
	int		tagname;
	struct	this_type	type;
	enum	ECPGttype	type_enum;
	enum	ECPGdtype	dtype_enum;
	struct	fetch_desc	descriptor;
	struct  su_symbol	struct_union;
	struct	prep		prep;
}
/* tokens */
/* special embedded SQL token */
%token  SQL_ALLOCATE SQL_AUTOCOMMIT SQL_BOOL SQL_BREAK
                SQL_CALL SQL_CARDINALITY SQL_CONNECT
                SQL_COUNT 
                SQL_DATETIME_INTERVAL_CODE
                SQL_DATETIME_INTERVAL_PRECISION SQL_DESCRIBE
                SQL_DESCRIPTOR SQL_DISCONNECT SQL_FOUND
                SQL_FREE SQL_GET SQL_GO SQL_GOTO SQL_IDENTIFIED
                SQL_INDICATOR SQL_KEY_MEMBER SQL_LENGTH
                SQL_LONG SQL_NULLABLE SQL_OCTET_LENGTH
                SQL_OPEN SQL_OUTPUT SQL_REFERENCE
                SQL_RETURNED_LENGTH SQL_RETURNED_OCTET_LENGTH SQL_SCALE
                SQL_SECTION SQL_SHORT SQL_SIGNED SQL_SQL SQL_SQLERROR
                SQL_SQLPRINT SQL_SQLWARNING SQL_START SQL_STOP
                SQL_STRUCT SQL_UNSIGNED SQL_VAR SQL_WHENEVER

/* C token */
%token  S_ADD S_AND S_ANYTHING S_AUTO S_CONST S_DEC S_DIV
                S_DOTPOINT S_EQUAL S_EXTERN S_INC S_LSHIFT S_MEMPOINT
                S_MEMBER S_MOD S_MUL S_NEQUAL S_OR S_REGISTER S_RSHIFT
                S_STATIC S_SUB S_VOLATILE
                S_TYPEDEF

%token TYPECAST
%token CSTRING CVARIABLE CPP_LINE IP 
%token DOLCONST ECONST NCONST UCONST UIDENT

/* types */
%type <str> stmt
%type <str> CreateRoleStmt
%type <str> opt_with
%type <str> OptRoleList
%type <str> OptRoleElem
%type <str> CreateUserStmt
%type <str> AlterRoleStmt
%type <str> AlterRoleSetStmt
%type <str> AlterUserStmt
%type <str> AlterUserSetStmt
%type <str> DropRoleStmt
%type <str> DropUserStmt
%type <str> CreateGroupStmt
%type <str> AlterGroupStmt
%type <str> add_drop
%type <str> DropGroupStmt
%type <str> CreateSchemaStmt
%type <str> OptSchemaName
%type <str> OptSchemaEltList
%type <str> schema_stmt
%type <str> VariableSetStmt
%type <str> set_rest
%type <str> var_name
%type <str> var_list
%type <str> var_value
%type <str> iso_level
%type <str> opt_boolean
%type <str> zone_value
%type <str> opt_encoding
%type <str> ColId_or_Sconst
%type <str> VariableResetStmt
%type <str> SetResetClause
%type <str> VariableShowStmt
%type <str> ConstraintsSetStmt
%type <str> constraints_set_list
%type <str> constraints_set_mode
%type <str> CheckPointStmt
%type <str> DiscardStmt
%type <str> AlterTableStmt
%type <str> alter_table_cmds
%type <str> alter_table_cmd
%type <str> alter_column_default
%type <str> opt_drop_behavior
%type <str> alter_using
%type <str> ClosePortalStmt
%type <str> CopyStmt
%type <str> copy_from
%type <str> copy_file_name
%type <str> copy_opt_list
%type <str> copy_opt_item
%type <str> opt_binary
%type <str> opt_oids
%type <str> copy_delimiter
%type <str> opt_using
%type <str> CreateStmt
%type <str> OptTemp
%type <str> OptTableElementList
%type <str> TableElementList
%type <str> TableElement
%type <str> columnDef
%type <str> ColQualList
%type <str> ColConstraint
%type <str> ColConstraintElem
%type <str> ConstraintAttr
%type <str> TableLikeClause
%type <str> TableLikeOptionList
%type <str> TableLikeOption
%type <str> TableConstraint
%type <str> ConstraintElem
%type <str> opt_column_list
%type <str> columnList
%type <str> columnElem
%type <str> key_match
%type <str> key_actions
%type <str> key_update
%type <str> key_delete
%type <str> key_action
%type <str> OptInherit
%type <str> OptWith
%type <str> OnCommitOption
%type <str> OptTableSpace
%type <str> OptConsTableSpace
%type <str> create_as_target
%type <str> OptCreateAs
%type <str> CreateAsList
%type <str> CreateAsElement
%type <str> opt_with_data
%type <str> CreateSeqStmt
%type <str> AlterSeqStmt
%type <str> OptSeqOptList
%type <str> SeqOptList
%type <str> SeqOptElem
%type <str> opt_by
%type <str> NumericOnly
%type <str> FloatOnly
%type <str> IntegerOnly
%type <str> CreatePLangStmt
%type <str> opt_trusted
%type <str> handler_name
%type <str> opt_validator
%type <str> opt_lancompiler
%type <str> DropPLangStmt
%type <str> opt_procedural
%type <str> CreateTableSpaceStmt
%type <str> OptTableSpaceOwner
%type <str> DropTableSpaceStmt
%type <str> CreateTrigStmt
%type <str> TriggerActionTime
%type <str> TriggerEvents
%type <str> TriggerOneEvent
%type <str> TriggerForSpec
%type <str> TriggerForOpt
%type <str> TriggerForType
%type <str> TriggerFuncArgs
%type <str> TriggerFuncArg
%type <str> OptConstrFromTable
%type <str> ConstraintAttributeSpec
%type <str> ConstraintDeferrabilitySpec
%type <str> ConstraintTimeSpec
%type <str> DropTrigStmt
%type <str> CreateAssertStmt
%type <str> DropAssertStmt
%type <str> DefineStmt
%type <str> definition
%type <str> def_list
%type <str> def_elem
%type <str> def_arg
%type <str> aggr_args
%type <str> old_aggr_definition
%type <str> old_aggr_list
%type <str> old_aggr_elem
%type <str> enum_val_list
%type <str> CreateOpClassStmt
%type <str> opclass_item_list
%type <str> opclass_item
%type <str> opt_default
%type <str> opt_opfamily
%type <str> opt_recheck
%type <str> CreateOpFamilyStmt
%type <str> AlterOpFamilyStmt
%type <str> opclass_drop_list
%type <str> opclass_drop
%type <str> DropOpClassStmt
%type <str> DropOpFamilyStmt
%type <str> DropOwnedStmt
%type <str> ReassignOwnedStmt
%type <str> DropStmt
%type <str> drop_type
%type <str> any_name_list
%type <str> any_name
%type <str> attrs
%type <str> TruncateStmt
%type <str> opt_restart_seqs
%type <str> CommentStmt
%type <str> comment_type
%type <str> comment_text
%type <str> FetchStmt
%type <str> fetch_direction
%type <str> from_in
%type <str> GrantStmt
%type <str> RevokeStmt
%type <str> privileges
%type <str> privilege_list
%type <str> privilege
%type <str> privilege_target
%type <str> grantee_list
%type <str> grantee
%type <str> opt_grant_grant_option
%type <str> function_with_argtypes_list
%type <str> function_with_argtypes
%type <str> GrantRoleStmt
%type <str> RevokeRoleStmt
%type <str> opt_grant_admin_option
%type <str> opt_granted_by
%type <str> IndexStmt
%type <str> index_opt_unique
%type <str> access_method_clause
%type <str> index_params
%type <str> index_elem
%type <str> opt_class
%type <str> opt_asc_desc
%type <str> opt_nulls_order
%type <str> CreateFunctionStmt
%type <str> opt_or_replace
%type <str> func_args
%type <str> func_args_list
%type <str> func_arg
%type <str> arg_class
%type <str> param_name
%type <str> func_return
%type <str> func_type
%type <str> createfunc_opt_list
%type <str> common_func_opt_item
%type <str> createfunc_opt_item
%type <str> func_as
%type <str> opt_definition
%type <str> table_func_column
%type <str> table_func_column_list
%type <str> AlterFunctionStmt
%type <str> alterfunc_opt_list
%type <str> opt_restrict
%type <str> RemoveFuncStmt
%type <str> RemoveAggrStmt
%type <str> RemoveOperStmt
%type <str> oper_argtypes
%type <str> any_operator
%type <str> CreateCastStmt
%type <str> cast_context
%type <str> DropCastStmt
%type <str> opt_if_exists
%type <str> ReindexStmt
%type <str> reindex_type
%type <str> opt_force
%type <str> RenameStmt
%type <str> opt_column
%type <str> opt_set_data
%type <str> AlterObjectSchemaStmt
%type <str> AlterOwnerStmt
%type <str> RuleActionList
%type <str> RuleActionMulti
%type <str> RuleActionStmt
%type <str> RuleActionStmtOrEmpty
%type <str> event
%type <str> opt_instead
%type <str> DropRuleStmt
%type <str> NotifyStmt
%type <str> ListenStmt
%type <str> UnlistenStmt
%type <str> TransactionStmt
%type <str> opt_transaction
%type <str> transaction_mode_item
%type <str> transaction_mode_list
%type <str> transaction_mode_list_or_empty
%type <str> ViewStmt
%type <str> opt_check_option
%type <str> LoadStmt
%type <str> CreatedbStmt
%type <str> createdb_opt_list
%type <str> createdb_opt_item
%type <str> opt_equal
%type <str> AlterDatabaseStmt
%type <str> AlterDatabaseSetStmt
%type <str> alterdb_opt_list
%type <str> alterdb_opt_item
%type <str> DropdbStmt
%type <str> CreateDomainStmt
%type <str> AlterDomainStmt
%type <str> opt_as
%type <str> AlterTSDictionaryStmt
%type <str> AlterTSConfigurationStmt
%type <str> CreateConversionStmt
%type <str> ClusterStmt
%type <str> cluster_index_specification
%type <str> VacuumStmt
%type <str> AnalyzeStmt
%type <str> analyze_keyword
%type <str> opt_verbose
%type <str> opt_full
%type <str> opt_freeze
%type <str> opt_name_list
%type <str> ExplainStmt
%type <str> ExplainableStmt
%type <str> opt_analyze
%type <prep> PrepareStmt
%type <str> prep_type_clause
%type <str> PreparableStmt
%type <str> ExecuteStmt
%type <str> execute_param_clause
%type <str> InsertStmt
%type <str> insert_rest
%type <str> insert_column_list
%type <str> insert_column_item
%type <str> returning_clause
%type <str> DeleteStmt
%type <str> using_clause
%type <str> LockStmt
%type <str> opt_lock
%type <str> lock_type
%type <str> opt_nowait
%type <str> UpdateStmt
%type <str> set_clause_list
%type <str> set_clause
%type <str> single_set_clause
%type <str> multiple_set_clause
%type <str> set_target
%type <str> set_target_list
%type <str> DeclareCursorStmt
%type <str> cursor_options
%type <str> opt_hold
%type <str> SelectStmt
%type <str> select_with_parens
%type <str> select_no_parens
%type <str> select_clause
%type <str> simple_select
%type <str> with_clause
%type <str> cte_list
%type <str> common_table_expr
%type <str> into_clause
%type <str> OptTempTableName
%type <str> opt_table
%type <str> opt_all
%type <str> opt_distinct
%type <str> opt_sort_clause
%type <str> sort_clause
%type <str> sortby_list
%type <str> sortby
%type <str> select_limit
%type <str> opt_select_limit
%type <str> select_limit_value
%type <str> opt_select_fetch_first_value
%type <str> select_offset_value
%type <str> select_offset_value2
%type <str> row_or_rows
%type <str> first_or_next
%type <str> group_clause
%type <str> having_clause
%type <str> for_locking_clause
%type <str> opt_for_locking_clause
%type <str> for_locking_items
%type <str> for_locking_item
%type <str> locked_rels_list
%type <str> values_clause
%type <str> from_clause
%type <str> from_list
%type <str> table_ref
%type <str> joined_table
%type <str> alias_clause
%type <str> join_type
%type <str> join_outer
%type <str> join_qual
%type <str> relation_expr
%type <str> relation_expr_opt_alias
%type <str> func_table
%type <str> where_clause
%type <str> where_or_current_clause
%type <str> TableFuncElementList
%type <str> TableFuncElement
%type <str> Typename
%type <index> opt_array_bounds
%type <str> SimpleTypename
%type <str> ConstTypename
%type <str> GenericType
%type <str> opt_type_modifiers
%type <str> Numeric
%type <str> opt_float
%type <str> Bit
%type <str> ConstBit
%type <str> BitWithLength
%type <str> BitWithoutLength
%type <str> Character
%type <str> ConstCharacter
%type <str> CharacterWithLength
%type <str> CharacterWithoutLength
%type <str> character
%type <str> opt_varying
%type <str> opt_charset
%type <str> ConstDatetime
%type <str> ConstInterval
%type <str> opt_timezone
%type <str> opt_interval
%type <str> interval_second
%type <str> a_expr
%type <str> b_expr
%type <str> c_expr
%type <str> func_expr
%type <str> xml_root_version
%type <str> opt_xml_root_standalone
%type <str> xml_attributes
%type <str> xml_attribute_list
%type <str> xml_attribute_el
%type <str> document_or_content
%type <str> xml_whitespace_option
%type <str> row
%type <str> sub_type
%type <str> all_Op
%type <str> MathOp
%type <str> qual_Op
%type <str> qual_all_Op
%type <str> subquery_Op
%type <str> expr_list
%type <str> type_list
%type <str> array_expr
%type <str> array_expr_list
%type <str> extract_list
%type <str> extract_arg
%type <str> overlay_list
%type <str> overlay_placing
%type <str> position_list
%type <str> substr_list
%type <str> substr_from
%type <str> substr_for
%type <str> trim_list
%type <str> in_expr
%type <str> case_expr
%type <str> when_clause_list
%type <str> when_clause
%type <str> case_default
%type <str> case_arg
%type <str> columnref
%type <str> indirection_el
%type <str> indirection
%type <str> opt_indirection
%type <str> opt_asymmetric
%type <str> ctext_expr
%type <str> ctext_expr_list
%type <str> ctext_row
%type <str> target_list
%type <str> target_el
%type <str> relation_name
%type <str> qualified_name_list
%type <str> qualified_name
%type <str> name_list
%type <str> name
%type <str> database_name
%type <str> access_method
%type <str> attr_name
%type <str> index_name
%type <str> file_name
%type <str> func_name
%type <str> AexprConst
%type <str> RoleId
%type <str> SignedIconst
%type <str> ColId
%type <str> type_function_name
%type <str> col_name_keyword
%type <str> type_func_name_keyword
%type <str> reserved_keyword
%type <str> SpecialRuleRelation
/* ecpgtype */
%type <str> ECPGAllocateDescr
%type <str> ECPGCKeywords
%type <str> ECPGColId
%type <str> ECPGColLabel
%type <str> ECPGColLabelCommon
%type <str> ECPGConnect
%type <str> ECPGCursorStmt
%type <str> ECPGDeallocateDescr
%type <str> ECPGDeclaration
%type <str> ECPGDeclare
%type <str> ECPGDescribe
%type <str> ECPGDisconnect
%type <str> ECPGExecuteImmediateStmt
%type <str> ECPGFree
%type <str> ECPGGetDescHeaderItem
%type <str> ECPGGetDescItem
%type <str> ECPGGetDescriptorHeader
%type <str> ECPGKeywords
%type <str> ECPGKeywords_rest
%type <str> ECPGKeywords_vanames
%type <str> ECPGOpen
%type <str> ECPGSetAutocommit
%type <str> ECPGSetConnection
%type <str> ECPGSetDescHeaderItem
%type <str> ECPGSetDescItem
%type <str> ECPGSetDescriptorHeader
%type <str> ECPGTypeName
%type <str> ECPGTypedef
%type <str> ECPGVar
%type <str> ECPGVarDeclaration
%type <str> ECPGWhenever
%type <str> ECPGunreserved
%type <str> ECPGunreserved_con
%type <str> ECPGunreserved_interval
%type <str> UsingConst
%type <str> UsingValue
%type <str> c_anything
%type <str> c_args
%type <str> c_list
%type <str> c_stuff
%type <str> c_stuff_item
%type <str> c_term
%type <str> c_thing
%type <str> char_variable
%type <str> civar
%type <str> civarind
%type <str> ColId_or_real_sconst
%type <str> ColLabel
%type <str> connect_options
%type <str> connection_object
%type <str> connection_target
%type <str> coutputvariable
%type <str> cvariable
%type <str> db_prefix
%type <str> CreateAsStmt
%type <str> DeallocateStmt
%type <str> dis_name
%type <str> ecpg_bconst
%type <str> ecpg_fconst
%type <str> ecpg_iconst
%type <str> ecpg_ident
%type <str> ecpg_interval
%type <str> ecpg_into
%type <str> ecpg_param
%type <str> ecpg_real_iconst
%type <str> ecpg_real_sconst
%type <str> ecpg_sconst
%type <str> ecpg_using
%type <str> ecpg_xconst
%type <str> enum_definition
%type <str> enum_type
%type <str> execstring
%type <str> execute_rest
%type <str> indicator
%type <str> into_descriptor
%type <str> Iresult
%type <str> on_off
%type <str> opt_bit_field
%type <str> opt_connection_name
%type <str> opt_database_name
%type <str> opt_ecpg_using
%type <str> opt_initializer
%type <str> opt_options
%type <str> opt_output
%type <str> opt_pointer
%type <str> opt_port
%type <str> opt_reference
%type <str> opt_scale
%type <str> opt_server
%type <str> opt_user
%type <str> opt_opt_value
%type <str> ora_user
%type <str> precision
%type <str> prepared_name
%type <str> quoted_ident_stringvar
%type <str> RuleStmt
%type <str> s_struct_union
%type <str> server
%type <str> server_name
%type <str> single_vt_declaration
%type <str> storage_clause
%type <str> storage_declaration
%type <str> storage_modifier
%type <str> struct_union_type
%type <str> struct_union_type_with_symbol
%type <str> symbol
%type <str> type_declaration
%type <str> unreserved_keyword 
%type <str> user_name
%type <str> using_descriptor
%type <str> var_declaration
%type <str> var_type_declarations
%type <str> variable
%type <str> variable_declarations
%type <str> variable_list
%type <str> vt_declarations 

%type <str> Op
%type <str> IntConstVar
%type <str> AllConstVar
%type <str> CSTRING
%type <str> CPP_LINE
%type <str> CVARIABLE
%type <str> DOLCONST
%type <str> ECONST
%type <str> NCONST
%type <str> SCONST
%type <str> UCONST
%type <str> UIDENT

%type  <struct_union> s_struct_union_symbol

%type  <descriptor> ECPGGetDescriptor
%type  <descriptor> ECPGSetDescriptor

%type  <type_enum> simple_type
%type  <type_enum> signed_type
%type  <type_enum> unsigned_type

%type  <dtype_enum> descriptor_item
%type  <dtype_enum> desc_header_item

%type  <type>   var_type

%type  <action> action

/* orig_tokens */
 %token ABORT_P ABSOLUTE_P ACCESS ACTION ADD_P ADMIN AFTER
 AGGREGATE ALL ALSO ALTER ALWAYS ANALYSE ANALYZE AND ANY ARRAY AS ASC
 ASSERTION ASSIGNMENT ASYMMETRIC AT AUTHORIZATION

 BACKWARD BEFORE BEGIN_P BETWEEN BIGINT BINARY BIT
 BOOLEAN_P BOTH BY

 CACHE CALLED CASCADE CASCADED CASE CAST CATALOG_P CHAIN CHAR_P
 CHARACTER CHARACTERISTICS CHECK CHECKPOINT CLASS CLOSE
 CLUSTER COALESCE COLLATE COLUMN COMMENT COMMIT
 COMMITTED CONCURRENTLY CONFIGURATION CONNECTION CONSTRAINT CONSTRAINTS
 CONTENT_P CONTINUE_P CONVERSION_P COPY COST CREATE CREATEDB
 CREATEROLE CREATEUSER CROSS CSV CTYPE CURRENT_P
 CURRENT_CATALOG CURRENT_DATE CURRENT_ROLE CURRENT_SCHEMA
 CURRENT_TIME CURRENT_TIMESTAMP CURRENT_USER CURSOR CYCLE

 DATA_P DATABASE DAY_P DEALLOCATE DEC DECIMAL_P DECLARE DEFAULT DEFAULTS
 DEFERRABLE DEFERRED DEFINER DELETE_P DELIMITER DELIMITERS DESC
 DICTIONARY DISABLE_P DISCARD DISTINCT DO DOCUMENT_P DOMAIN_P DOUBLE_P DROP

 EACH ELSE ENABLE_P ENCODING ENCRYPTED END_P ENUM_P ESCAPE EXCEPT EXCLUDING
 EXCLUSIVE EXECUTE EXISTS EXPLAIN EXTERNAL EXTRACT

 FALSE_P FAMILY FETCH FIRST_P FLOAT_P FOR FORCE FOREIGN FORWARD
 FREEZE FROM FULL FUNCTION

 GLOBAL GRANT GRANTED GREATEST GROUP_P

 HANDLER HAVING HEADER_P HOLD HOUR_P

 IDENTITY_P IF_P ILIKE IMMEDIATE IMMUTABLE IMPLICIT_P IN_P
 INCLUDING INCREMENT INDEX INDEXES INHERIT INHERITS INITIALLY
 INNER_P INOUT INPUT_P INSENSITIVE INSERT INSTEAD INT_P INTEGER
 INTERSECT INTERVAL INTO INVOKER IS ISNULL ISOLATION

 JOIN

 KEY

 LANCOMPILER LANGUAGE LARGE_P LAST_P LEADING LEAST LEFT LEVEL
 LIKE LIMIT LISTEN LOAD LOCAL LOCALTIME LOCALTIMESTAMP LOCATION
 LOCK_P LOGIN_P

 MAPPING MATCH MAXVALUE MINUTE_P MINVALUE MODE MONTH_P MOVE

 NAME_P NAMES NATIONAL NATURAL NCHAR NEW NEXT NO NOCREATEDB
 NOCREATEROLE NOCREATEUSER NOINHERIT NOLOGIN_P NONE NOSUPERUSER
 NOT NOTHING NOTIFY NOTNULL NOWAIT NULL_P NULLIF NULLS_P NUMERIC

 OBJECT_P OF OFF OFFSET OIDS OLD ON ONLY OPERATOR OPTION OR
 ORDER OUT_P OUTER_P OVERLAPS OVERLAY OWNED OWNER

 PARSER PARTIAL PASSWORD PLACING PLANS POSITION
 PRECISION PRESERVE PREPARE PREPARED PRIMARY
 PRIOR PRIVILEGES PROCEDURAL PROCEDURE

 QUOTE

 READ REAL REASSIGN RECHECK RECURSIVE REFERENCES REINDEX RELATIVE_P RELEASE
 RENAME REPEATABLE REPLACE REPLICA RESET RESTART RESTRICT RETURNING RETURNS
 REVOKE RIGHT ROLE ROLLBACK ROW ROWS RULE

 SAVEPOINT SCHEMA SCROLL SEARCH SECOND_P SECURITY SELECT SEQUENCE
 SERIALIZABLE SESSION SESSION_USER SET SETOF SHARE
 SHOW SIMILAR SIMPLE SMALLINT SOME STABLE STANDALONE_P START STATEMENT
 STATISTICS STDIN STDOUT STORAGE STRICT_P STRIP_P SUBSTRING SUPERUSER_P
 SYMMETRIC SYSID SYSTEM_P

 TABLE TABLESPACE TEMP TEMPLATE TEMPORARY TEXT_P THEN TIME TIMESTAMP
 TO TRAILING TRANSACTION TREAT TRIGGER TRIM TRUE_P
 TRUNCATE TRUSTED TYPE_P

 UNCOMMITTED UNENCRYPTED UNION UNIQUE UNKNOWN UNLISTEN UNTIL
 UPDATE USER USING

 VACUUM VALID VALIDATOR VALUE_P VALUES VARCHAR VARIADIC VARYING
 VERBOSE VERSION_P VIEW VOLATILE

 WHEN WHERE WHITESPACE_P WITH WITHOUT WORK WRITE

 XML_P XMLATTRIBUTES XMLCONCAT XMLELEMENT XMLFOREST XMLPARSE
 XMLPI XMLROOT XMLSERIALIZE

 YEAR_P YES_P

 ZONE





 %token NULLS_FIRST NULLS_LAST WITH_TIME


 %token IDENT FCONST SCONST BCONST XCONST Op
 %token ICONST PARAM


 %nonassoc SET
 %left UNION EXCEPT
 %left INTERSECT
 %left OR
 %left AND
 %right NOT
 %right '='
 %nonassoc '<' '>'
 %nonassoc LIKE ILIKE SIMILAR
 %nonassoc ESCAPE
 %nonassoc OVERLAPS
 %nonassoc BETWEEN
 %nonassoc IN_P
 %left POSTFIXOP
 %nonassoc IDENT
%nonassoc CSTRING
%nonassoc UIDENT
 %left Op OPERATOR
 %nonassoc NOTNULL
 %nonassoc ISNULL
 %nonassoc IS NULL_P TRUE_P FALSE_P UNKNOWN
 %left '+' '-'
 %left '*' '/' '%'
 %left '^'

 %left AT ZONE
 %right UMINUS
 %left '[' ']'
 %left '(' ')'
 %left TYPECAST
 %left '.'







 %left JOIN CROSS LEFT FULL RIGHT INNER_P NATURAL

 %right PRESERVE STRIP_P
%%
prog: statements;
/* rules */
 stmt:
 AlterDatabaseStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterDatabaseSetStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterDomainStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterFunctionStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterGroupStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterObjectSchemaStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterOwnerStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterSeqStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterTableStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterRoleSetStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterRoleStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterTSConfigurationStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterTSDictionaryStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterUserSetStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterUserStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AnalyzeStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CheckPointStmt
 { output_statement($1, 0, ECPGst_normal); }
|  ClosePortalStmt
	{
		if (INFORMIX_MODE)
		{
			if (pg_strcasecmp($1+strlen("close "), "database") == 0)
			{
				if (connection)
					mmerror(PARSE_ERROR, ET_ERROR, "no at option for close database statement\n");

				fprintf(yyout, "{ ECPGdisconnect(__LINE__, \"CURRENT\");");
				whenever_action(2);
				free($1);
				break;
			}
		}

		output_statement($1, 0, ECPGst_normal);
	}
|  ClusterStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CommentStmt
 { output_statement($1, 0, ECPGst_normal); }
|  ConstraintsSetStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CopyStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateAsStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateAssertStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateCastStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateConversionStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateDomainStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateFunctionStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateGroupStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateOpClassStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateOpFamilyStmt
 { output_statement($1, 0, ECPGst_normal); }
|  AlterOpFamilyStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreatePLangStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateSchemaStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateSeqStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateTableSpaceStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateTrigStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateRoleStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreateUserStmt
 { output_statement($1, 0, ECPGst_normal); }
|  CreatedbStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DeallocateStmt
	{
		if (connection)
			mmerror(PARSE_ERROR, ET_ERROR, "no at option for deallocate statement\n");

		output_deallocate_prepare_statement($1);
	}
|  DeclareCursorStmt
	{ output_simple_statement($1); }
|  DefineStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DeleteStmt
	{ output_statement($1, 1, ECPGst_normal); }
|  DiscardStmt
	{ output_statement($1, 1, ECPGst_normal); }
|  DropAssertStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropCastStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropGroupStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropOpClassStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropOpFamilyStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropOwnedStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropPLangStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropRuleStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropTableSpaceStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropTrigStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropRoleStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropUserStmt
 { output_statement($1, 0, ECPGst_normal); }
|  DropdbStmt
 { output_statement($1, 0, ECPGst_normal); }
|  ExecuteStmt
	{ output_statement($1, 1, ECPGst_execute); }
|  ExplainStmt
 { output_statement($1, 0, ECPGst_normal); }
|  FetchStmt
	{ output_statement($1, 1, ECPGst_normal); }
|  GrantStmt
 { output_statement($1, 0, ECPGst_normal); }
|  GrantRoleStmt
 { output_statement($1, 0, ECPGst_normal); }
|  IndexStmt
 { output_statement($1, 0, ECPGst_normal); }
|  InsertStmt
	{ output_statement($1, 1, ECPGst_normal); }
|  ListenStmt
 { output_statement($1, 0, ECPGst_normal); }
|  LoadStmt
 { output_statement($1, 0, ECPGst_normal); }
|  LockStmt
 { output_statement($1, 0, ECPGst_normal); }
|  NotifyStmt
 { output_statement($1, 0, ECPGst_normal); }
|  PrepareStmt
	{
		if ($1.type == NULL || strlen($1.type) == 0)
			output_prepare_statement($1.name, $1.stmt);
		else	
			output_statement(cat_str(5, make_str("prepare"), $1.name, $1.type, make_str("as"), $1.stmt), 0, ECPGst_normal);
	}
|  ReassignOwnedStmt
 { output_statement($1, 0, ECPGst_normal); }
|  ReindexStmt
 { output_statement($1, 0, ECPGst_normal); }
|  RemoveAggrStmt
 { output_statement($1, 0, ECPGst_normal); }
|  RemoveFuncStmt
 { output_statement($1, 0, ECPGst_normal); }
|  RemoveOperStmt
 { output_statement($1, 0, ECPGst_normal); }
|  RenameStmt
 { output_statement($1, 0, ECPGst_normal); }
|  RevokeStmt
 { output_statement($1, 0, ECPGst_normal); }
|  RevokeRoleStmt
 { output_statement($1, 0, ECPGst_normal); }
|  RuleStmt
 { output_statement($1, 0, ECPGst_normal); }
|  SelectStmt
	{ output_statement($1, 1, ECPGst_normal); }
|  TransactionStmt
	{
		fprintf(yyout, "{ ECPGtrans(__LINE__, %s, \"%s\");", connection ? connection : "NULL", $1);
		whenever_action(2);
		free($1);
	}
|  TruncateStmt
 { output_statement($1, 0, ECPGst_normal); }
|  UnlistenStmt
 { output_statement($1, 0, ECPGst_normal); }
|  UpdateStmt
	{ output_statement($1, 1, ECPGst_normal); }
|  VacuumStmt
 { output_statement($1, 0, ECPGst_normal); }
|  VariableResetStmt
 { output_statement($1, 0, ECPGst_normal); }
|  VariableSetStmt
 { output_statement($1, 0, ECPGst_normal); }
|  VariableShowStmt
 { output_statement($1, 0, ECPGst_normal); }
|  ViewStmt
 { output_statement($1, 0, ECPGst_normal); }
	| ECPGAllocateDescr
	{
		fprintf(yyout,"ECPGallocate_desc(__LINE__, %s);",$1);
		whenever_action(0);
		free($1);
	}
	| ECPGConnect
	{
		if (connection)
			mmerror(PARSE_ERROR, ET_ERROR, "no at option for connect statement\n");

		fprintf(yyout, "{ ECPGconnect(__LINE__, %d, %s, %d); ", compat, $1, autocommit);
		reset_variables();
		whenever_action(2);
		free($1);
	}
	| ECPGCursorStmt
	{
		output_simple_statement($1);
	}
	| ECPGDeallocateDescr
	{
		if (connection)
			mmerror(PARSE_ERROR, ET_ERROR, "no at option for deallocate statement\n");
		fprintf(yyout,"ECPGdeallocate_desc(__LINE__, %s);",$1);
		whenever_action(0);
		free($1);
	}
	| ECPGDeclare
	{
		output_simple_statement($1);
	}
	| ECPGDescribe
	{
		fprintf(yyout, "{ ECPGdescribe(__LINE__, %s,", $1);
		dump_variables(argsresult, 1);
		fputs("ECPGt_EORT);", yyout);
		fprintf(yyout, "}");
		output_line_number();

		free($1);
	}
	| ECPGDisconnect
	{
		if (connection)
			mmerror(PARSE_ERROR, ET_ERROR, "no at option for disconnect statement\n");

		fprintf(yyout, "{ ECPGdisconnect(__LINE__, %s);",
				$1 ? $1 : "\"CURRENT\"");
		whenever_action(2);
		free($1);
	}
	| ECPGExecuteImmediateStmt	{ output_statement($1, 0, ECPGst_exec_immediate); }
	| ECPGFree
	{
		const char *con = connection ? connection : "NULL";
		if (strcmp($1, "all"))
			fprintf(yyout, "{ ECPGdeallocate(__LINE__, %d, %s, \"%s\");", compat, con, $1);
		else
			fprintf(yyout, "{ ECPGdeallocate_all(__LINE__, %d, %s);", compat, con);

		whenever_action(2);
		free($1);
	}
	| ECPGGetDescriptor
	{
		lookup_descriptor($1.name, connection);
		output_get_descr($1.name, $1.str);
		free($1.name);
		free($1.str);
	}
	| ECPGGetDescriptorHeader
	{
		lookup_descriptor($1, connection);
		output_get_descr_header($1);
		free($1);
	}
	| ECPGOpen
	{
		struct cursor *ptr;

		if ((ptr = add_additional_variables($1, true)) != NULL)
		{
			connection = ptr->connection ? mm_strdup(ptr->connection) : NULL;
			output_statement(mm_strdup(ptr->command), 0, 0);
			ptr->opened = true;
		}
	}
	| ECPGSetAutocommit
	{
		fprintf(yyout, "{ ECPGsetcommit(__LINE__, \"%s\", %s);", $1, connection ? connection : "NULL");
		whenever_action(2);
		free($1);
	}
	| ECPGSetConnection
	{
		if (connection)
			mmerror(PARSE_ERROR, ET_ERROR, "no at option for set connection statement\n");

		fprintf(yyout, "{ ECPGsetconn(__LINE__, %s);", $1);
		whenever_action(2);
		free($1);
	}
	| ECPGSetDescriptor
	{
		lookup_descriptor($1.name, connection);
		output_set_descr($1.name, $1.str);
		free($1.name);
		free($1.str);
	}
	| ECPGSetDescriptorHeader
	{
		lookup_descriptor($1, connection);
		output_set_descr_header($1);
		free($1);
	}
	| ECPGTypedef
	{
		if (connection)
			mmerror(PARSE_ERROR, ET_ERROR, "no at option for typedef statement\n");

		fprintf(yyout, "%s", $1);
		free($1);
		output_line_number();
	}
	| ECPGVar
	{
		if (connection)
			mmerror(PARSE_ERROR, ET_ERROR, "no at option for var statement\n");

		output_simple_statement($1);
	}
	| ECPGWhenever
	{
		if (connection)
			mmerror(PARSE_ERROR, ET_ERROR, "no at option for whenever statement\n");

		output_simple_statement($1);
	}
| 
 { $$ = NULL; }
;


 CreateRoleStmt:
 CREATE ROLE RoleId opt_with OptRoleList
 { 
 $$ = cat_str(4,make_str("create role"),$3,$4,$5);
}
;


 opt_with:
 WITH
 { 
 $$ = make_str("with");
}
| 
 { 
 $$=EMPTY; }
;


 OptRoleList:
 OptRoleList OptRoleElem
 { 
 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 OptRoleElem:
 PASSWORD ecpg_sconst
 { 
 $$ = cat_str(2,make_str("password"),$2);
}
|  PASSWORD NULL_P
 { 
 $$ = make_str("password null");
}
|  ENCRYPTED PASSWORD ecpg_sconst
 { 
 $$ = cat_str(2,make_str("encrypted password"),$3);
}
|  UNENCRYPTED PASSWORD ecpg_sconst
 { 
 $$ = cat_str(2,make_str("unencrypted password"),$3);
}
|  SUPERUSER_P
 { 
 $$ = make_str("superuser");
}
|  NOSUPERUSER
 { 
 $$ = make_str("nosuperuser");
}
|  INHERIT
 { 
 $$ = make_str("inherit");
}
|  NOINHERIT
 { 
 $$ = make_str("noinherit");
}
|  CREATEDB
 { 
 $$ = make_str("createdb");
}
|  NOCREATEDB
 { 
 $$ = make_str("nocreatedb");
}
|  CREATEROLE
 { 
 $$ = make_str("createrole");
}
|  NOCREATEROLE
 { 
 $$ = make_str("nocreaterole");
}
|  CREATEUSER
 { 
 $$ = make_str("createuser");
}
|  NOCREATEUSER
 { 
 $$ = make_str("nocreateuser");
}
|  LOGIN_P
 { 
 $$ = make_str("login");
}
|  NOLOGIN_P
 { 
 $$ = make_str("nologin");
}
|  CONNECTION LIMIT SignedIconst
 { 
 $$ = cat_str(2,make_str("connection limit"),$3);
}
|  VALID UNTIL ecpg_sconst
 { 
 $$ = cat_str(2,make_str("valid until"),$3);
}
|  USER name_list
 { 
 $$ = cat_str(2,make_str("user"),$2);
}
|  SYSID ecpg_iconst
 { 
 $$ = cat_str(2,make_str("sysid"),$2);
}
|  ADMIN name_list
 { 
 $$ = cat_str(2,make_str("admin"),$2);
}
|  ROLE name_list
 { 
 $$ = cat_str(2,make_str("role"),$2);
}
|  IN_P ROLE name_list
 { 
 $$ = cat_str(2,make_str("in role"),$3);
}
|  IN_P GROUP_P name_list
 { 
 $$ = cat_str(2,make_str("in group"),$3);
}
;


 CreateUserStmt:
 CREATE USER RoleId opt_with OptRoleList
 { 
 $$ = cat_str(4,make_str("create user"),$3,$4,$5);
}
;


 AlterRoleStmt:
 ALTER ROLE RoleId opt_with OptRoleList
 { 
 $$ = cat_str(4,make_str("alter role"),$3,$4,$5);
}
;


 AlterRoleSetStmt:
 ALTER ROLE RoleId SetResetClause
 { 
 $$ = cat_str(3,make_str("alter role"),$3,$4);
}
;


 AlterUserStmt:
 ALTER USER RoleId opt_with OptRoleList
 { 
 $$ = cat_str(4,make_str("alter user"),$3,$4,$5);
}
;


 AlterUserSetStmt:
 ALTER USER RoleId SetResetClause
 { 
 $$ = cat_str(3,make_str("alter user"),$3,$4);
}
;


 DropRoleStmt:
 DROP ROLE name_list
 { 
 $$ = cat_str(2,make_str("drop role"),$3);
}
|  DROP ROLE IF_P EXISTS name_list
 { 
 $$ = cat_str(2,make_str("drop role if exists"),$5);
}
;


 DropUserStmt:
 DROP USER name_list
 { 
 $$ = cat_str(2,make_str("drop user"),$3);
}
|  DROP USER IF_P EXISTS name_list
 { 
 $$ = cat_str(2,make_str("drop user if exists"),$5);
}
;


 CreateGroupStmt:
 CREATE GROUP_P RoleId opt_with OptRoleList
 { 
 $$ = cat_str(4,make_str("create group"),$3,$4,$5);
}
;


 AlterGroupStmt:
 ALTER GROUP_P RoleId add_drop USER name_list
 { 
 $$ = cat_str(5,make_str("alter group"),$3,$4,make_str("user"),$6);
}
;


 add_drop:
 ADD_P
 { 
 $$ = make_str("add");
}
|  DROP
 { 
 $$ = make_str("drop");
}
;


 DropGroupStmt:
 DROP GROUP_P name_list
 { 
 $$ = cat_str(2,make_str("drop group"),$3);
}
|  DROP GROUP_P IF_P EXISTS name_list
 { 
 $$ = cat_str(2,make_str("drop group if exists"),$5);
}
;


 CreateSchemaStmt:
 CREATE SCHEMA OptSchemaName AUTHORIZATION RoleId OptSchemaEltList
 { 
 $$ = cat_str(5,make_str("create schema"),$3,make_str("authorization"),$5,$6);
}
|  CREATE SCHEMA ColId OptSchemaEltList
 { 
 $$ = cat_str(3,make_str("create schema"),$3,$4);
}
;


 OptSchemaName:
 ColId
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 OptSchemaEltList:
 OptSchemaEltList schema_stmt
 { 
 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 schema_stmt:
 CreateStmt
 { 
 $$ = $1;
}
|  IndexStmt
 { 
 $$ = $1;
}
|  CreateSeqStmt
 { 
 $$ = $1;
}
|  CreateTrigStmt
 { 
 $$ = $1;
}
|  GrantStmt
 { 
 $$ = $1;
}
|  ViewStmt
 { 
 $$ = $1;
}
;


 VariableSetStmt:
 SET set_rest
 { 
 $$ = cat_str(2,make_str("set"),$2);
}
|  SET LOCAL set_rest
 { 
 $$ = cat_str(2,make_str("set local"),$3);
}
|  SET SESSION set_rest
 { 
 $$ = cat_str(2,make_str("set session"),$3);
}
;


 set_rest:
 var_name TO var_list
 { 
 $$ = cat_str(3,$1,make_str("to"),$3);
}
|  var_name '=' var_list
 { 
 $$ = cat_str(3,$1,make_str("="),$3);
}
|  var_name TO DEFAULT
 { 
 $$ = cat_str(2,$1,make_str("to default"));
}
|  var_name '=' DEFAULT
 { 
 $$ = cat_str(2,$1,make_str("= default"));
}
|  var_name FROM CURRENT_P
 { 
 $$ = cat_str(2,$1,make_str("from current"));
}
|  TIME ZONE zone_value
 { 
 $$ = cat_str(2,make_str("time zone"),$3);
}
|  TRANSACTION transaction_mode_list
 { 
 $$ = cat_str(2,make_str("transaction"),$2);
}
|  SESSION CHARACTERISTICS AS TRANSACTION transaction_mode_list
 { 
 $$ = cat_str(2,make_str("session characteristics as transaction"),$5);
}
|  CATALOG_P ecpg_sconst
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = cat_str(2,make_str("catalog"),$2);
}
|  SCHEMA ecpg_sconst
 { 
 $$ = cat_str(2,make_str("schema"),$2);
}
|  NAMES opt_encoding
 { 
 $$ = cat_str(2,make_str("names"),$2);
}
|  ROLE ColId_or_Sconst
 { 
 $$ = cat_str(2,make_str("role"),$2);
}
|  SESSION AUTHORIZATION ColId_or_Sconst
 { 
 $$ = cat_str(2,make_str("session authorization"),$3);
}
|  SESSION AUTHORIZATION DEFAULT
 { 
 $$ = make_str("session authorization default");
}
|  XML_P OPTION document_or_content
 { 
 $$ = cat_str(2,make_str("xml option"),$3);
}
;


 var_name:
ECPGColId
 { 
 $$ = $1;
}
|  var_name '.' ColId
 { 
 $$ = cat_str(3,$1,make_str("."),$3);
}
;


 var_list:
 var_value
 { 
 $$ = $1;
}
|  var_list ',' var_value
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 var_value:
 opt_boolean
 { 
 $$ = $1;
}
| ColId_or_real_sconst
 { 
 $$ = $1;
}
|  NumericOnly
 { 
		if ($1[1] == '$')
		{
			free($1);
			$1 = make_str("$0");
		}

 $$ = $1;
}
;


 iso_level:
 READ UNCOMMITTED
 { 
 $$ = make_str("read uncommitted");
}
|  READ COMMITTED
 { 
 $$ = make_str("read committed");
}
|  REPEATABLE READ
 { 
 $$ = make_str("repeatable read");
}
|  SERIALIZABLE
 { 
 $$ = make_str("serializable");
}
;


 opt_boolean:
 TRUE_P
 { 
 $$ = make_str("true");
}
|  FALSE_P
 { 
 $$ = make_str("false");
}
|  ON
 { 
 $$ = make_str("on");
}
|  OFF
 { 
 $$ = make_str("off");
}
;


 zone_value:
ecpg_real_sconst
 { 
 $$ = $1;
}
|  ecpg_ident
 { 
 $$ = $1;
}
|  ConstInterval ecpg_sconst opt_interval
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  ConstInterval '(' ecpg_iconst ')' ecpg_sconst opt_interval
 { 
 $$ = cat_str(6,$1,make_str("("),$3,make_str(")"),$5,$6);
}
|  NumericOnly
 { 
 $$ = $1;
}
|  DEFAULT
 { 
 $$ = make_str("default");
}
|  LOCAL
 { 
 $$ = make_str("local");
}
;


 opt_encoding:
 ecpg_sconst
 { 
 $$ = $1;
}
|  DEFAULT
 { 
 $$ = make_str("default");
}
| 
 { 
 $$=EMPTY; }
;


 ColId_or_Sconst:
 ColId
 { 
 $$ = $1;
}
|  ecpg_sconst
 { 
 $$ = $1;
}
;


 VariableResetStmt:
 RESET var_name
 { 
 $$ = cat_str(2,make_str("reset"),$2);
}
|  RESET TIME ZONE
 { 
 $$ = make_str("reset time zone");
}
|  RESET TRANSACTION ISOLATION LEVEL
 { 
 $$ = make_str("reset transaction isolation level");
}
|  RESET SESSION AUTHORIZATION
 { 
 $$ = make_str("reset session authorization");
}
|  RESET ALL
 { 
 $$ = make_str("reset all");
}
;


 SetResetClause:
 SET set_rest
 { 
 $$ = cat_str(2,make_str("set"),$2);
}
|  VariableResetStmt
 { 
 $$ = $1;
}
;


 VariableShowStmt:
SHOW var_name ecpg_into
 { 
 $$ = cat_str(2,make_str("show"),$2);
}
| SHOW TIME ZONE ecpg_into
 { 
 $$ = make_str("show time zone");
}
| SHOW TRANSACTION ISOLATION LEVEL ecpg_into
 { 
 $$ = make_str("show transaction isolation level");
}
| SHOW SESSION AUTHORIZATION ecpg_into
 { 
 $$ = make_str("show session authorization");
}
|  SHOW ALL
	{
		mmerror(PARSE_ERROR, ET_ERROR, "SHOW ALL not implemented\n");
		$$ = EMPTY;
	}
;


 ConstraintsSetStmt:
 SET CONSTRAINTS constraints_set_list constraints_set_mode
 { 
 $$ = cat_str(3,make_str("set constraints"),$3,$4);
}
;


 constraints_set_list:
 ALL
 { 
 $$ = make_str("all");
}
|  qualified_name_list
 { 
 $$ = $1;
}
;


 constraints_set_mode:
 DEFERRED
 { 
 $$ = make_str("deferred");
}
|  IMMEDIATE
 { 
 $$ = make_str("immediate");
}
;


 CheckPointStmt:
 CHECKPOINT
 { 
 $$ = make_str("checkpoint");
}
;


 DiscardStmt:
 DISCARD ALL
 { 
 $$ = make_str("discard all");
}
|  DISCARD TEMP
 { 
 $$ = make_str("discard temp");
}
|  DISCARD TEMPORARY
 { 
 $$ = make_str("discard temporary");
}
|  DISCARD PLANS
 { 
 $$ = make_str("discard plans");
}
;


 AlterTableStmt:
 ALTER TABLE relation_expr alter_table_cmds
 { 
 $$ = cat_str(3,make_str("alter table"),$3,$4);
}
|  ALTER INDEX relation_expr alter_table_cmds
 { 
 $$ = cat_str(3,make_str("alter index"),$3,$4);
}
|  ALTER SEQUENCE relation_expr alter_table_cmds
 { 
 $$ = cat_str(3,make_str("alter sequence"),$3,$4);
}
|  ALTER VIEW relation_expr alter_table_cmds
 { 
 $$ = cat_str(3,make_str("alter view"),$3,$4);
}
;


 alter_table_cmds:
 alter_table_cmd
 { 
 $$ = $1;
}
|  alter_table_cmds ',' alter_table_cmd
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 alter_table_cmd:
 ADD_P opt_column columnDef
 { 
 $$ = cat_str(3,make_str("add"),$2,$3);
}
|  ALTER opt_column ColId alter_column_default
 { 
 $$ = cat_str(4,make_str("alter"),$2,$3,$4);
}
|  ALTER opt_column ColId DROP NOT NULL_P
 { 
 $$ = cat_str(4,make_str("alter"),$2,$3,make_str("drop not null"));
}
|  ALTER opt_column ColId SET NOT NULL_P
 { 
 $$ = cat_str(4,make_str("alter"),$2,$3,make_str("set not null"));
}
|  ALTER opt_column ColId SET STATISTICS IntegerOnly
 { 
 $$ = cat_str(5,make_str("alter"),$2,$3,make_str("set statistics"),$6);
}
|  ALTER opt_column ColId SET STORAGE ColId
 { 
 $$ = cat_str(5,make_str("alter"),$2,$3,make_str("set storage"),$6);
}
|  DROP opt_column ColId opt_drop_behavior
 { 
 $$ = cat_str(4,make_str("drop"),$2,$3,$4);
}
|  ALTER opt_column ColId opt_set_data TYPE_P Typename alter_using
 { 
 $$ = cat_str(7,make_str("alter"),$2,$3,$4,make_str("type"),$6,$7);
}
|  ADD_P TableConstraint
 { 
 $$ = cat_str(2,make_str("add"),$2);
}
|  DROP CONSTRAINT name opt_drop_behavior
 { 
 $$ = cat_str(3,make_str("drop constraint"),$3,$4);
}
|  SET WITHOUT OIDS
 { 
 $$ = make_str("set without oids");
}
|  CLUSTER ON name
 { 
 $$ = cat_str(2,make_str("cluster on"),$3);
}
|  SET WITHOUT CLUSTER
 { 
 $$ = make_str("set without cluster");
}
|  ENABLE_P TRIGGER name
 { 
 $$ = cat_str(2,make_str("enable trigger"),$3);
}
|  ENABLE_P ALWAYS TRIGGER name
 { 
 $$ = cat_str(2,make_str("enable always trigger"),$4);
}
|  ENABLE_P REPLICA TRIGGER name
 { 
 $$ = cat_str(2,make_str("enable replica trigger"),$4);
}
|  ENABLE_P TRIGGER ALL
 { 
 $$ = make_str("enable trigger all");
}
|  ENABLE_P TRIGGER USER
 { 
 $$ = make_str("enable trigger user");
}
|  DISABLE_P TRIGGER name
 { 
 $$ = cat_str(2,make_str("disable trigger"),$3);
}
|  DISABLE_P TRIGGER ALL
 { 
 $$ = make_str("disable trigger all");
}
|  DISABLE_P TRIGGER USER
 { 
 $$ = make_str("disable trigger user");
}
|  ENABLE_P RULE name
 { 
 $$ = cat_str(2,make_str("enable rule"),$3);
}
|  ENABLE_P ALWAYS RULE name
 { 
 $$ = cat_str(2,make_str("enable always rule"),$4);
}
|  ENABLE_P REPLICA RULE name
 { 
 $$ = cat_str(2,make_str("enable replica rule"),$4);
}
|  DISABLE_P RULE name
 { 
 $$ = cat_str(2,make_str("disable rule"),$3);
}
|  INHERIT qualified_name
 { 
 $$ = cat_str(2,make_str("inherit"),$2);
}
|  NO INHERIT qualified_name
 { 
 $$ = cat_str(2,make_str("no inherit"),$3);
}
|  OWNER TO RoleId
 { 
 $$ = cat_str(2,make_str("owner to"),$3);
}
|  SET TABLESPACE name
 { 
 $$ = cat_str(2,make_str("set tablespace"),$3);
}
|  SET definition
 { 
 $$ = cat_str(2,make_str("set"),$2);
}
|  RESET definition
 { 
 $$ = cat_str(2,make_str("reset"),$2);
}
;


 alter_column_default:
 SET DEFAULT a_expr
 { 
 $$ = cat_str(2,make_str("set default"),$3);
}
|  DROP DEFAULT
 { 
 $$ = make_str("drop default");
}
;


 opt_drop_behavior:
 CASCADE
 { 
 $$ = make_str("cascade");
}
|  RESTRICT
 { 
 $$ = make_str("restrict");
}
| 
 { 
 $$=EMPTY; }
;


 alter_using:
 USING a_expr
 { 
 $$ = cat_str(2,make_str("using"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 ClosePortalStmt:
 CLOSE name
 { 
 $$ = cat_str(2,make_str("close"),$2);
}
|  CLOSE ALL
 { 
 $$ = make_str("close all");
}
;


 CopyStmt:
 COPY opt_binary qualified_name opt_column_list opt_oids copy_from copy_file_name copy_delimiter opt_with copy_opt_list
 { 
			if (strcmp($6, "to") == 0 && strcmp($7, "stdin") == 0)
				mmerror(PARSE_ERROR, ET_ERROR, "copy to stdin not possible\n");
			else if (strcmp($6, "from") == 0 && strcmp($7, "stdout") == 0)
				mmerror(PARSE_ERROR, ET_ERROR, "copy from stdout not possible\n");
			else if (strcmp($6, "from") == 0 && strcmp($7, "stdin") == 0)
				mmerror(PARSE_ERROR, ET_WARNING, "copy from stdin not implemented\n");

 $$ = cat_str(10,make_str("copy"),$2,$3,$4,$5,$6,$7,$8,$9,$10);
}
|  COPY select_with_parens TO copy_file_name opt_with copy_opt_list
 { 
			if (strcmp($4, "stdin") == 0)
				mmerror(PARSE_ERROR, ET_ERROR, "copy to stdin not possible\n");

 $$ = cat_str(6,make_str("copy"),$2,make_str("to"),$4,$5,$6);
}
;


 copy_from:
 FROM
 { 
 $$ = make_str("from");
}
|  TO
 { 
 $$ = make_str("to");
}
;


 copy_file_name:
 ecpg_sconst
 { 
 $$ = $1;
}
|  STDIN
 { 
 $$ = make_str("stdin");
}
|  STDOUT
 { 
 $$ = make_str("stdout");
}
;


 copy_opt_list:
 copy_opt_list copy_opt_item
 { 
 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 copy_opt_item:
 BINARY
 { 
 $$ = make_str("binary");
}
|  OIDS
 { 
 $$ = make_str("oids");
}
|  DELIMITER opt_as ecpg_sconst
 { 
 $$ = cat_str(3,make_str("delimiter"),$2,$3);
}
|  NULL_P opt_as ecpg_sconst
 { 
 $$ = cat_str(3,make_str("null"),$2,$3);
}
|  CSV
 { 
 $$ = make_str("csv");
}
|  HEADER_P
 { 
 $$ = make_str("header");
}
|  QUOTE opt_as ecpg_sconst
 { 
 $$ = cat_str(3,make_str("quote"),$2,$3);
}
|  ESCAPE opt_as ecpg_sconst
 { 
 $$ = cat_str(3,make_str("escape"),$2,$3);
}
|  FORCE QUOTE columnList
 { 
 $$ = cat_str(2,make_str("force quote"),$3);
}
|  FORCE NOT NULL_P columnList
 { 
 $$ = cat_str(2,make_str("force not null"),$4);
}
;


 opt_binary:
 BINARY
 { 
 $$ = make_str("binary");
}
| 
 { 
 $$=EMPTY; }
;


 opt_oids:
 WITH OIDS
 { 
 $$ = make_str("with oids");
}
| 
 { 
 $$=EMPTY; }
;


 copy_delimiter:
 opt_using DELIMITERS ecpg_sconst
 { 
 $$ = cat_str(3,$1,make_str("delimiters"),$3);
}
| 
 { 
 $$=EMPTY; }
;


 opt_using:
 USING
 { 
 $$ = make_str("using");
}
| 
 { 
 $$=EMPTY; }
;


 CreateStmt:
 CREATE OptTemp TABLE qualified_name '(' OptTableElementList ')' OptInherit OptWith OnCommitOption OptTableSpace
 { 
 $$ = cat_str(11,make_str("create"),$2,make_str("table"),$4,make_str("("),$6,make_str(")"),$8,$9,$10,$11);
}
|  CREATE OptTemp TABLE qualified_name OF qualified_name '(' OptTableElementList ')' OptWith OnCommitOption OptTableSpace
 { 
 $$ = cat_str(12,make_str("create"),$2,make_str("table"),$4,make_str("of"),$6,make_str("("),$8,make_str(")"),$10,$11,$12);
}
;


 OptTemp:
 TEMPORARY
 { 
 $$ = make_str("temporary");
}
|  TEMP
 { 
 $$ = make_str("temp");
}
|  LOCAL TEMPORARY
 { 
 $$ = make_str("local temporary");
}
|  LOCAL TEMP
 { 
 $$ = make_str("local temp");
}
|  GLOBAL TEMPORARY
 { 
 $$ = make_str("global temporary");
}
|  GLOBAL TEMP
 { 
 $$ = make_str("global temp");
}
| 
 { 
 $$=EMPTY; }
;


 OptTableElementList:
 TableElementList
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 TableElementList:
 TableElement
 { 
 $$ = $1;
}
|  TableElementList ',' TableElement
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 TableElement:
 columnDef
 { 
 $$ = $1;
}
|  TableLikeClause
 { 
 $$ = $1;
}
|  TableConstraint
 { 
 $$ = $1;
}
;


 columnDef:
 ColId Typename ColQualList
 { 
 $$ = cat_str(3,$1,$2,$3);
}
;


 ColQualList:
 ColQualList ColConstraint
 { 
 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 ColConstraint:
 CONSTRAINT name ColConstraintElem
 { 
 $$ = cat_str(3,make_str("constraint"),$2,$3);
}
|  ColConstraintElem
 { 
 $$ = $1;
}
|  ConstraintAttr
 { 
 $$ = $1;
}
;


 ColConstraintElem:
 NOT NULL_P
 { 
 $$ = make_str("not null");
}
|  NULL_P
 { 
 $$ = make_str("null");
}
|  UNIQUE opt_definition OptConsTableSpace
 { 
 $$ = cat_str(3,make_str("unique"),$2,$3);
}
|  PRIMARY KEY opt_definition OptConsTableSpace
 { 
 $$ = cat_str(3,make_str("primary key"),$3,$4);
}
|  CHECK '(' a_expr ')'
 { 
 $$ = cat_str(3,make_str("check ("),$3,make_str(")"));
}
|  DEFAULT b_expr
 { 
 $$ = cat_str(2,make_str("default"),$2);
}
|  REFERENCES qualified_name opt_column_list key_match key_actions
 { 
 $$ = cat_str(5,make_str("references"),$2,$3,$4,$5);
}
;


 ConstraintAttr:
 DEFERRABLE
 { 
 $$ = make_str("deferrable");
}
|  NOT DEFERRABLE
 { 
 $$ = make_str("not deferrable");
}
|  INITIALLY DEFERRED
 { 
 $$ = make_str("initially deferred");
}
|  INITIALLY IMMEDIATE
 { 
 $$ = make_str("initially immediate");
}
;


 TableLikeClause:
 LIKE qualified_name TableLikeOptionList
 { 
 $$ = cat_str(3,make_str("like"),$2,$3);
}
;


 TableLikeOptionList:
 TableLikeOptionList TableLikeOption
 { 
 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 TableLikeOption:
 INCLUDING DEFAULTS
 { 
 $$ = make_str("including defaults");
}
|  EXCLUDING DEFAULTS
 { 
 $$ = make_str("excluding defaults");
}
|  INCLUDING CONSTRAINTS
 { 
 $$ = make_str("including constraints");
}
|  EXCLUDING CONSTRAINTS
 { 
 $$ = make_str("excluding constraints");
}
|  INCLUDING INDEXES
 { 
 $$ = make_str("including indexes");
}
|  EXCLUDING INDEXES
 { 
 $$ = make_str("excluding indexes");
}
;


 TableConstraint:
 CONSTRAINT name ConstraintElem
 { 
 $$ = cat_str(3,make_str("constraint"),$2,$3);
}
|  ConstraintElem
 { 
 $$ = $1;
}
;


 ConstraintElem:
 CHECK '(' a_expr ')'
 { 
 $$ = cat_str(3,make_str("check ("),$3,make_str(")"));
}
|  UNIQUE '(' columnList ')' opt_definition OptConsTableSpace
 { 
 $$ = cat_str(5,make_str("unique ("),$3,make_str(")"),$5,$6);
}
|  PRIMARY KEY '(' columnList ')' opt_definition OptConsTableSpace
 { 
 $$ = cat_str(5,make_str("primary key ("),$4,make_str(")"),$6,$7);
}
|  FOREIGN KEY '(' columnList ')' REFERENCES qualified_name opt_column_list key_match key_actions ConstraintAttributeSpec
 { 
 $$ = cat_str(8,make_str("foreign key ("),$4,make_str(") references"),$7,$8,$9,$10,$11);
}
;


 opt_column_list:
 '(' columnList ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 columnList:
 columnElem
 { 
 $$ = $1;
}
|  columnList ',' columnElem
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 columnElem:
 ColId
 { 
 $$ = $1;
}
;


 key_match:
 MATCH FULL
 { 
 $$ = make_str("match full");
}
|  MATCH PARTIAL
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = make_str("match partial");
}
|  MATCH SIMPLE
 { 
 $$ = make_str("match simple");
}
| 
 { 
 $$=EMPTY; }
;


 key_actions:
 key_update
 { 
 $$ = $1;
}
|  key_delete
 { 
 $$ = $1;
}
|  key_update key_delete
 { 
 $$ = cat_str(2,$1,$2);
}
|  key_delete key_update
 { 
 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 key_update:
 ON UPDATE key_action
 { 
 $$ = cat_str(2,make_str("on update"),$3);
}
;


 key_delete:
 ON DELETE_P key_action
 { 
 $$ = cat_str(2,make_str("on delete"),$3);
}
;


 key_action:
 NO ACTION
 { 
 $$ = make_str("no action");
}
|  RESTRICT
 { 
 $$ = make_str("restrict");
}
|  CASCADE
 { 
 $$ = make_str("cascade");
}
|  SET NULL_P
 { 
 $$ = make_str("set null");
}
|  SET DEFAULT
 { 
 $$ = make_str("set default");
}
;


 OptInherit:
 INHERITS '(' qualified_name_list ')'
 { 
 $$ = cat_str(3,make_str("inherits ("),$3,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 OptWith:
 WITH definition
 { 
 $$ = cat_str(2,make_str("with"),$2);
}
|  WITH OIDS
 { 
 $$ = make_str("with oids");
}
|  WITHOUT OIDS
 { 
 $$ = make_str("without oids");
}
| 
 { 
 $$=EMPTY; }
;


 OnCommitOption:
 ON COMMIT DROP
 { 
 $$ = make_str("on commit drop");
}
|  ON COMMIT DELETE_P ROWS
 { 
 $$ = make_str("on commit delete rows");
}
|  ON COMMIT PRESERVE ROWS
 { 
 $$ = make_str("on commit preserve rows");
}
| 
 { 
 $$=EMPTY; }
;


 OptTableSpace:
 TABLESPACE name
 { 
 $$ = cat_str(2,make_str("tablespace"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 OptConsTableSpace:
 USING INDEX TABLESPACE name
 { 
 $$ = cat_str(2,make_str("using index tablespace"),$4);
}
| 
 { 
 $$=EMPTY; }
;


 create_as_target:
 qualified_name OptCreateAs OptWith OnCommitOption OptTableSpace
 { 
 $$ = cat_str(5,$1,$2,$3,$4,$5);
}
;


 OptCreateAs:
 '(' CreateAsList ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 CreateAsList:
 CreateAsElement
 { 
 $$ = $1;
}
|  CreateAsList ',' CreateAsElement
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 CreateAsElement:
 ColId
 { 
 $$ = $1;
}
;


 opt_with_data:
 WITH DATA_P
 { 
 $$ = make_str("with data");
}
|  WITH NO DATA_P
 { 
 $$ = make_str("with no data");
}
| 
 { 
 $$=EMPTY; }
;


 CreateSeqStmt:
 CREATE OptTemp SEQUENCE qualified_name OptSeqOptList
 { 
 $$ = cat_str(5,make_str("create"),$2,make_str("sequence"),$4,$5);
}
;


 AlterSeqStmt:
 ALTER SEQUENCE relation_expr SeqOptList
 { 
 $$ = cat_str(3,make_str("alter sequence"),$3,$4);
}
;


 OptSeqOptList:
 SeqOptList
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 SeqOptList:
 SeqOptElem
 { 
 $$ = $1;
}
|  SeqOptList SeqOptElem
 { 
 $$ = cat_str(2,$1,$2);
}
;


 SeqOptElem:
 CACHE NumericOnly
 { 
 $$ = cat_str(2,make_str("cache"),$2);
}
|  CYCLE
 { 
 $$ = make_str("cycle");
}
|  NO CYCLE
 { 
 $$ = make_str("no cycle");
}
|  INCREMENT opt_by NumericOnly
 { 
 $$ = cat_str(3,make_str("increment"),$2,$3);
}
|  MAXVALUE NumericOnly
 { 
 $$ = cat_str(2,make_str("maxvalue"),$2);
}
|  MINVALUE NumericOnly
 { 
 $$ = cat_str(2,make_str("minvalue"),$2);
}
|  NO MAXVALUE
 { 
 $$ = make_str("no maxvalue");
}
|  NO MINVALUE
 { 
 $$ = make_str("no minvalue");
}
|  OWNED BY any_name
 { 
 $$ = cat_str(2,make_str("owned by"),$3);
}
|  START opt_with NumericOnly
 { 
 $$ = cat_str(3,make_str("start"),$2,$3);
}
|  RESTART
 { 
 $$ = make_str("restart");
}
|  RESTART opt_with NumericOnly
 { 
 $$ = cat_str(3,make_str("restart"),$2,$3);
}
;


 opt_by:
 BY
 { 
 $$ = make_str("by");
}
| 
 { 
 $$=EMPTY; }
;


 NumericOnly:
 FloatOnly
 { 
 $$ = $1;
}
|  IntegerOnly
 { 
 $$ = $1;
}
;


 FloatOnly:
 ecpg_fconst
 { 
 $$ = $1;
}
|  '-' ecpg_fconst
 { 
 $$ = cat_str(2,make_str("-"),$2);
}
;


 IntegerOnly:
 SignedIconst
 { 
 $$ = $1;
}
;


 CreatePLangStmt:
 CREATE opt_trusted opt_procedural LANGUAGE ColId_or_Sconst
 { 
 $$ = cat_str(5,make_str("create"),$2,$3,make_str("language"),$5);
}
|  CREATE opt_trusted opt_procedural LANGUAGE ColId_or_Sconst HANDLER handler_name opt_validator opt_lancompiler
 { 
 $$ = cat_str(9,make_str("create"),$2,$3,make_str("language"),$5,make_str("handler"),$7,$8,$9);
}
;


 opt_trusted:
 TRUSTED
 { 
 $$ = make_str("trusted");
}
| 
 { 
 $$=EMPTY; }
;


 handler_name:
 name
 { 
 $$ = $1;
}
|  name attrs
 { 
 $$ = cat_str(2,$1,$2);
}
;


 opt_validator:
 VALIDATOR handler_name
 { 
 $$ = cat_str(2,make_str("validator"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 opt_lancompiler:
 LANCOMPILER ecpg_sconst
 { 
 $$ = cat_str(2,make_str("lancompiler"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 DropPLangStmt:
 DROP opt_procedural LANGUAGE ColId_or_Sconst opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop"),$2,make_str("language"),$4,$5);
}
|  DROP opt_procedural LANGUAGE IF_P EXISTS ColId_or_Sconst opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop"),$2,make_str("language if exists"),$6,$7);
}
;


 opt_procedural:
 PROCEDURAL
 { 
 $$ = make_str("procedural");
}
| 
 { 
 $$=EMPTY; }
;


 CreateTableSpaceStmt:
 CREATE TABLESPACE name OptTableSpaceOwner LOCATION ecpg_sconst
 { 
 $$ = cat_str(5,make_str("create tablespace"),$3,$4,make_str("location"),$6);
}
;


 OptTableSpaceOwner:
 OWNER name
 { 
 $$ = cat_str(2,make_str("owner"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 DropTableSpaceStmt:
 DROP TABLESPACE name
 { 
 $$ = cat_str(2,make_str("drop tablespace"),$3);
}
|  DROP TABLESPACE IF_P EXISTS name
 { 
 $$ = cat_str(2,make_str("drop tablespace if exists"),$5);
}
;


 CreateTrigStmt:
 CREATE TRIGGER name TriggerActionTime TriggerEvents ON qualified_name TriggerForSpec EXECUTE PROCEDURE func_name '(' TriggerFuncArgs ')'
 { 
 $$ = cat_str(12,make_str("create trigger"),$3,$4,$5,make_str("on"),$7,$8,make_str("execute procedure"),$11,make_str("("),$13,make_str(")"));
}
|  CREATE CONSTRAINT TRIGGER name AFTER TriggerEvents ON qualified_name OptConstrFromTable ConstraintAttributeSpec FOR EACH ROW EXECUTE PROCEDURE func_name '(' TriggerFuncArgs ')'
 { 
 $$ = cat_str(13,make_str("create constraint trigger"),$4,make_str("after"),$6,make_str("on"),$8,$9,$10,make_str("for each row execute procedure"),$16,make_str("("),$18,make_str(")"));
}
;


 TriggerActionTime:
 BEFORE
 { 
 $$ = make_str("before");
}
|  AFTER
 { 
 $$ = make_str("after");
}
;


 TriggerEvents:
 TriggerOneEvent
 { 
 $$ = $1;
}
|  TriggerOneEvent OR TriggerOneEvent
 { 
 $$ = cat_str(3,$1,make_str("or"),$3);
}
|  TriggerOneEvent OR TriggerOneEvent OR TriggerOneEvent
 { 
 $$ = cat_str(5,$1,make_str("or"),$3,make_str("or"),$5);
}
;


 TriggerOneEvent:
 INSERT
 { 
 $$ = make_str("insert");
}
|  DELETE_P
 { 
 $$ = make_str("delete");
}
|  UPDATE
 { 
 $$ = make_str("update");
}
|  TRUNCATE
 { 
 $$ = make_str("truncate");
}
;


 TriggerForSpec:
 FOR TriggerForOpt TriggerForType
 { 
 $$ = cat_str(3,make_str("for"),$2,$3);
}
| 
 { 
 $$=EMPTY; }
;


 TriggerForOpt:
 EACH
 { 
 $$ = make_str("each");
}
| 
 { 
 $$=EMPTY; }
;


 TriggerForType:
 ROW
 { 
 $$ = make_str("row");
}
|  STATEMENT
 { 
 $$ = make_str("statement");
}
;


 TriggerFuncArgs:
 TriggerFuncArg
 { 
 $$ = $1;
}
|  TriggerFuncArgs ',' TriggerFuncArg
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
| 
 { 
 $$=EMPTY; }
;


 TriggerFuncArg:
 ecpg_iconst
 { 
 $$ = $1;
}
|  ecpg_fconst
 { 
 $$ = $1;
}
| ecpg_real_sconst
 { 
 $$ = $1;
}
|  ecpg_bconst
 { 
 $$ = $1;
}
|  XCONST
 { 
 $$ = make_str("xconst");
}
|  ColId
 { 
 $$ = $1;
}
;


 OptConstrFromTable:
 FROM qualified_name
 { 
 $$ = cat_str(2,make_str("from"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 ConstraintAttributeSpec:
 ConstraintDeferrabilitySpec
 { 
 $$ = $1;
}
|  ConstraintDeferrabilitySpec ConstraintTimeSpec
 { 
			if (strcmp($1, "deferrable") != 0 && strcmp($2, "initially deferrable") == 0 )
				mmerror(PARSE_ERROR, ET_ERROR, "INITIALLY DEFERRED constraint must be DEFERRABLE\n");

 $$ = cat_str(2,$1,$2);
}
|  ConstraintTimeSpec
 { 
 $$ = $1;
}
|  ConstraintTimeSpec ConstraintDeferrabilitySpec
 { 
			if (strcmp($2, "deferrable") != 0 && strcmp($1, "initially deferrable") == 0 )
				mmerror(PARSE_ERROR, ET_ERROR, "INITIALLY DEFERRED constraint must be DEFERRABLE\n");

 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 ConstraintDeferrabilitySpec:
 NOT DEFERRABLE
 { 
 $$ = make_str("not deferrable");
}
|  DEFERRABLE
 { 
 $$ = make_str("deferrable");
}
;


 ConstraintTimeSpec:
 INITIALLY IMMEDIATE
 { 
 $$ = make_str("initially immediate");
}
|  INITIALLY DEFERRED
 { 
 $$ = make_str("initially deferred");
}
;


 DropTrigStmt:
 DROP TRIGGER name ON qualified_name opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop trigger"),$3,make_str("on"),$5,$6);
}
|  DROP TRIGGER IF_P EXISTS name ON qualified_name opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop trigger if exists"),$5,make_str("on"),$7,$8);
}
;


 CreateAssertStmt:
 CREATE ASSERTION name CHECK '(' a_expr ')' ConstraintAttributeSpec
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = cat_str(6,make_str("create assertion"),$3,make_str("check ("),$6,make_str(")"),$8);
}
;


 DropAssertStmt:
 DROP ASSERTION name opt_drop_behavior
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = cat_str(3,make_str("drop assertion"),$3,$4);
}
;


 DefineStmt:
 CREATE AGGREGATE func_name aggr_args definition
 { 
 $$ = cat_str(4,make_str("create aggregate"),$3,$4,$5);
}
|  CREATE AGGREGATE func_name old_aggr_definition
 { 
 $$ = cat_str(3,make_str("create aggregate"),$3,$4);
}
|  CREATE OPERATOR any_operator definition
 { 
 $$ = cat_str(3,make_str("create operator"),$3,$4);
}
|  CREATE TYPE_P any_name definition
 { 
 $$ = cat_str(3,make_str("create type"),$3,$4);
}
|  CREATE TYPE_P any_name
 { 
 $$ = cat_str(2,make_str("create type"),$3);
}
|  CREATE TYPE_P any_name AS '(' TableFuncElementList ')'
 { 
 $$ = cat_str(5,make_str("create type"),$3,make_str("as ("),$6,make_str(")"));
}
|  CREATE TYPE_P any_name AS ENUM_P '(' enum_val_list ')'
 { 
 $$ = cat_str(5,make_str("create type"),$3,make_str("as enum ("),$7,make_str(")"));
}
|  CREATE TEXT_P SEARCH PARSER any_name definition
 { 
 $$ = cat_str(3,make_str("create text search parser"),$5,$6);
}
|  CREATE TEXT_P SEARCH DICTIONARY any_name definition
 { 
 $$ = cat_str(3,make_str("create text search dictionary"),$5,$6);
}
|  CREATE TEXT_P SEARCH TEMPLATE any_name definition
 { 
 $$ = cat_str(3,make_str("create text search template"),$5,$6);
}
|  CREATE TEXT_P SEARCH CONFIGURATION any_name definition
 { 
 $$ = cat_str(3,make_str("create text search configuration"),$5,$6);
}
;


 definition:
 '(' def_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
;


 def_list:
 def_elem
 { 
 $$ = $1;
}
|  def_list ',' def_elem
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 def_elem:
 ColLabel '=' def_arg
 { 
 $$ = cat_str(3,$1,make_str("="),$3);
}
|  ColLabel
 { 
 $$ = $1;
}
;


 def_arg:
 func_type
 { 
 $$ = $1;
}
|  reserved_keyword
 { 
 $$ = $1;
}
|  qual_all_Op
 { 
 $$ = $1;
}
|  NumericOnly
 { 
 $$ = $1;
}
| ecpg_real_sconst
 { 
 $$ = $1;
}
;


 aggr_args:
 '(' type_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
|  '(' '*' ')'
 { 
 $$ = make_str("( * )");
}
;


 old_aggr_definition:
 '(' old_aggr_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
;


 old_aggr_list:
 old_aggr_elem
 { 
 $$ = $1;
}
|  old_aggr_list ',' old_aggr_elem
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 old_aggr_elem:
 ecpg_ident '=' def_arg
 { 
 $$ = cat_str(3,$1,make_str("="),$3);
}
;


 enum_val_list:
 ecpg_sconst
 { 
 $$ = $1;
}
|  enum_val_list ',' ecpg_sconst
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 CreateOpClassStmt:
 CREATE OPERATOR CLASS any_name opt_default FOR TYPE_P Typename USING access_method opt_opfamily AS opclass_item_list
 { 
 $$ = cat_str(10,make_str("create operator class"),$4,$5,make_str("for type"),$8,make_str("using"),$10,$11,make_str("as"),$13);
}
;


 opclass_item_list:
 opclass_item
 { 
 $$ = $1;
}
|  opclass_item_list ',' opclass_item
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 opclass_item:
 OPERATOR ecpg_iconst any_operator opt_recheck
 { 
 $$ = cat_str(4,make_str("operator"),$2,$3,$4);
}
|  OPERATOR ecpg_iconst any_operator oper_argtypes opt_recheck
 { 
 $$ = cat_str(5,make_str("operator"),$2,$3,$4,$5);
}
|  FUNCTION ecpg_iconst func_name func_args
 { 
 $$ = cat_str(4,make_str("function"),$2,$3,$4);
}
|  FUNCTION ecpg_iconst '(' type_list ')' func_name func_args
 { 
 $$ = cat_str(7,make_str("function"),$2,make_str("("),$4,make_str(")"),$6,$7);
}
|  STORAGE Typename
 { 
 $$ = cat_str(2,make_str("storage"),$2);
}
;


 opt_default:
 DEFAULT
 { 
 $$ = make_str("default");
}
| 
 { 
 $$=EMPTY; }
;


 opt_opfamily:
 FAMILY any_name
 { 
 $$ = cat_str(2,make_str("family"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 opt_recheck:
 RECHECK
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = make_str("recheck");
}
| 
 { 
 $$=EMPTY; }
;


 CreateOpFamilyStmt:
 CREATE OPERATOR FAMILY any_name USING access_method
 { 
 $$ = cat_str(4,make_str("create operator family"),$4,make_str("using"),$6);
}
;


 AlterOpFamilyStmt:
 ALTER OPERATOR FAMILY any_name USING access_method ADD_P opclass_item_list
 { 
 $$ = cat_str(6,make_str("alter operator family"),$4,make_str("using"),$6,make_str("add"),$8);
}
|  ALTER OPERATOR FAMILY any_name USING access_method DROP opclass_drop_list
 { 
 $$ = cat_str(6,make_str("alter operator family"),$4,make_str("using"),$6,make_str("drop"),$8);
}
;


 opclass_drop_list:
 opclass_drop
 { 
 $$ = $1;
}
|  opclass_drop_list ',' opclass_drop
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 opclass_drop:
 OPERATOR ecpg_iconst '(' type_list ')'
 { 
 $$ = cat_str(5,make_str("operator"),$2,make_str("("),$4,make_str(")"));
}
|  FUNCTION ecpg_iconst '(' type_list ')'
 { 
 $$ = cat_str(5,make_str("function"),$2,make_str("("),$4,make_str(")"));
}
;


 DropOpClassStmt:
 DROP OPERATOR CLASS any_name USING access_method opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop operator class"),$4,make_str("using"),$6,$7);
}
|  DROP OPERATOR CLASS IF_P EXISTS any_name USING access_method opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop operator class if exists"),$6,make_str("using"),$8,$9);
}
;


 DropOpFamilyStmt:
 DROP OPERATOR FAMILY any_name USING access_method opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop operator family"),$4,make_str("using"),$6,$7);
}
|  DROP OPERATOR FAMILY IF_P EXISTS any_name USING access_method opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop operator family if exists"),$6,make_str("using"),$8,$9);
}
;


 DropOwnedStmt:
 DROP OWNED BY name_list opt_drop_behavior
 { 
 $$ = cat_str(3,make_str("drop owned by"),$4,$5);
}
;


 ReassignOwnedStmt:
 REASSIGN OWNED BY name_list TO name
 { 
 $$ = cat_str(4,make_str("reassign owned by"),$4,make_str("to"),$6);
}
;


 DropStmt:
 DROP drop_type IF_P EXISTS any_name_list opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop"),$2,make_str("if exists"),$5,$6);
}
|  DROP drop_type any_name_list opt_drop_behavior
 { 
 $$ = cat_str(4,make_str("drop"),$2,$3,$4);
}
;


 drop_type:
 TABLE
 { 
 $$ = make_str("table");
}
|  SEQUENCE
 { 
 $$ = make_str("sequence");
}
|  VIEW
 { 
 $$ = make_str("view");
}
|  INDEX
 { 
 $$ = make_str("index");
}
|  TYPE_P
 { 
 $$ = make_str("type");
}
|  DOMAIN_P
 { 
 $$ = make_str("domain");
}
|  CONVERSION_P
 { 
 $$ = make_str("conversion");
}
|  SCHEMA
 { 
 $$ = make_str("schema");
}
|  TEXT_P SEARCH PARSER
 { 
 $$ = make_str("text search parser");
}
|  TEXT_P SEARCH DICTIONARY
 { 
 $$ = make_str("text search dictionary");
}
|  TEXT_P SEARCH TEMPLATE
 { 
 $$ = make_str("text search template");
}
|  TEXT_P SEARCH CONFIGURATION
 { 
 $$ = make_str("text search configuration");
}
;


 any_name_list:
 any_name
 { 
 $$ = $1;
}
|  any_name_list ',' any_name
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 any_name:
 ColId
 { 
 $$ = $1;
}
|  ColId attrs
 { 
 $$ = cat_str(2,$1,$2);
}
;


 attrs:
 '.' attr_name
 { 
 $$ = cat_str(2,make_str("."),$2);
}
|  attrs '.' attr_name
 { 
 $$ = cat_str(3,$1,make_str("."),$3);
}
;


 TruncateStmt:
 TRUNCATE opt_table qualified_name_list opt_restart_seqs opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("truncate"),$2,$3,$4,$5);
}
;


 opt_restart_seqs:
 CONTINUE_P IDENTITY_P
 { 
 $$ = make_str("continue identity");
}
|  RESTART IDENTITY_P
 { 
 $$ = make_str("restart identity");
}
| 
 { 
 $$=EMPTY; }
;


 CommentStmt:
 COMMENT ON comment_type any_name IS comment_text
 { 
 $$ = cat_str(5,make_str("comment on"),$3,$4,make_str("is"),$6);
}
|  COMMENT ON AGGREGATE func_name aggr_args IS comment_text
 { 
 $$ = cat_str(5,make_str("comment on aggregate"),$4,$5,make_str("is"),$7);
}
|  COMMENT ON FUNCTION func_name func_args IS comment_text
 { 
 $$ = cat_str(5,make_str("comment on function"),$4,$5,make_str("is"),$7);
}
|  COMMENT ON OPERATOR any_operator oper_argtypes IS comment_text
 { 
 $$ = cat_str(5,make_str("comment on operator"),$4,$5,make_str("is"),$7);
}
|  COMMENT ON CONSTRAINT name ON any_name IS comment_text
 { 
 $$ = cat_str(6,make_str("comment on constraint"),$4,make_str("on"),$6,make_str("is"),$8);
}
|  COMMENT ON RULE name ON any_name IS comment_text
 { 
 $$ = cat_str(6,make_str("comment on rule"),$4,make_str("on"),$6,make_str("is"),$8);
}
|  COMMENT ON RULE name IS comment_text
 { 
 $$ = cat_str(4,make_str("comment on rule"),$4,make_str("is"),$6);
}
|  COMMENT ON TRIGGER name ON any_name IS comment_text
 { 
 $$ = cat_str(6,make_str("comment on trigger"),$4,make_str("on"),$6,make_str("is"),$8);
}
|  COMMENT ON OPERATOR CLASS any_name USING access_method IS comment_text
 { 
 $$ = cat_str(6,make_str("comment on operator class"),$5,make_str("using"),$7,make_str("is"),$9);
}
|  COMMENT ON OPERATOR FAMILY any_name USING access_method IS comment_text
 { 
 $$ = cat_str(6,make_str("comment on operator family"),$5,make_str("using"),$7,make_str("is"),$9);
}
|  COMMENT ON LARGE_P OBJECT_P NumericOnly IS comment_text
 { 
 $$ = cat_str(4,make_str("comment on large object"),$5,make_str("is"),$7);
}
|  COMMENT ON CAST '(' Typename AS Typename ')' IS comment_text
 { 
 $$ = cat_str(6,make_str("comment on cast ("),$5,make_str("as"),$7,make_str(") is"),$10);
}
|  COMMENT ON opt_procedural LANGUAGE any_name IS comment_text
 { 
 $$ = cat_str(6,make_str("comment on"),$3,make_str("language"),$5,make_str("is"),$7);
}
|  COMMENT ON TEXT_P SEARCH PARSER any_name IS comment_text
 { 
 $$ = cat_str(4,make_str("comment on text search parser"),$6,make_str("is"),$8);
}
|  COMMENT ON TEXT_P SEARCH DICTIONARY any_name IS comment_text
 { 
 $$ = cat_str(4,make_str("comment on text search dictionary"),$6,make_str("is"),$8);
}
|  COMMENT ON TEXT_P SEARCH TEMPLATE any_name IS comment_text
 { 
 $$ = cat_str(4,make_str("comment on text search template"),$6,make_str("is"),$8);
}
|  COMMENT ON TEXT_P SEARCH CONFIGURATION any_name IS comment_text
 { 
 $$ = cat_str(4,make_str("comment on text search configuration"),$6,make_str("is"),$8);
}
;


 comment_type:
 COLUMN
 { 
 $$ = make_str("column");
}
|  DATABASE
 { 
 $$ = make_str("database");
}
|  SCHEMA
 { 
 $$ = make_str("schema");
}
|  INDEX
 { 
 $$ = make_str("index");
}
|  SEQUENCE
 { 
 $$ = make_str("sequence");
}
|  TABLE
 { 
 $$ = make_str("table");
}
|  DOMAIN_P
 { 
 $$ = make_str("domain");
}
|  TYPE_P
 { 
 $$ = make_str("type");
}
|  VIEW
 { 
 $$ = make_str("view");
}
|  CONVERSION_P
 { 
 $$ = make_str("conversion");
}
|  TABLESPACE
 { 
 $$ = make_str("tablespace");
}
|  ROLE
 { 
 $$ = make_str("role");
}
;


 comment_text:
 ecpg_sconst
 { 
 $$ = $1;
}
|  NULL_P
 { 
 $$ = make_str("null");
}
;


 FetchStmt:
 FETCH fetch_direction from_in name
	{
		add_additional_variables($4, false);
		$$ = cat_str(4, make_str("fetch"), $2, $3, $4);
	}
|  FETCH name
	{
		add_additional_variables($2, false);
		$$ = cat_str(2, make_str("fetch"), $2);
	}
|  MOVE fetch_direction from_in name
 { 
 $$ = cat_str(4,make_str("move"),$2,$3,$4);
}
|  MOVE name
 { 
 $$ = cat_str(2,make_str("move"),$2);
}
	| FETCH fetch_direction from_in name ecpg_into
		{
			add_additional_variables($4, false);
			$$ = cat_str(4, make_str("fetch"), $2, $3, $4);
		}
	| FETCH fetch_direction name ecpg_into
		{
			add_additional_variables($3, false);
			$$ = cat_str(4, make_str("fetch"), $2, make_str("from"), $3);
		}
	| FETCH from_in name ecpg_into
		{
			add_additional_variables($3, false);
			$$ = cat_str(3, make_str("fetch"), $2, $3);
		}
	| FETCH name ecpg_into
		{
			add_additional_variables($2, false);
			$$ = cat2_str(make_str("fetch"), $2);
		}
	| FETCH fetch_direction name
		{
			add_additional_variables($3, false);
			$$ = cat_str(4, make_str("fetch"), $2, make_str("from"), $3);
		}
	| FETCH from_in name
		{
			add_additional_variables($3, false);
			$$ = cat_str(3, make_str("fetch"), $2, $3);
		}
;


 fetch_direction:
 NEXT
 { 
 $$ = make_str("next");
}
|  PRIOR
 { 
 $$ = make_str("prior");
}
|  FIRST_P
 { 
 $$ = make_str("first");
}
|  LAST_P
 { 
 $$ = make_str("last");
}
|  ABSOLUTE_P SignedIconst
 { 
		if ($2[1] == '$')
		{
			free($2);
			$2 = make_str("$0");
		}

 $$ = cat_str(2,make_str("absolute"),$2);
}
|  RELATIVE_P SignedIconst
 { 
		if ($2[1] == '$')
		{
			free($2);
			$2 = make_str("$0");
		}

 $$ = cat_str(2,make_str("relative"),$2);
}
|  SignedIconst
 { 
		if ($1[1] == '$')
		{
			free($1);
			$1 = make_str("$0");
		}

 $$ = $1;
}
|  ALL
 { 
 $$ = make_str("all");
}
|  FORWARD
 { 
 $$ = make_str("forward");
}
|  FORWARD SignedIconst
 { 
		if ($2[1] == '$')
		{
			free($2);
			$2 = make_str("$0");
		}

 $$ = cat_str(2,make_str("forward"),$2);
}
|  FORWARD ALL
 { 
 $$ = make_str("forward all");
}
|  BACKWARD
 { 
 $$ = make_str("backward");
}
|  BACKWARD SignedIconst
 { 
		if ($2[1] == '$')
		{
			free($2);
			$2 = make_str("$0");
		}

 $$ = cat_str(2,make_str("backward"),$2);
}
|  BACKWARD ALL
 { 
 $$ = make_str("backward all");
}
;


 from_in:
 FROM
 { 
 $$ = make_str("from");
}
|  IN_P
 { 
 $$ = make_str("in");
}
;


 GrantStmt:
 GRANT privileges ON privilege_target TO grantee_list opt_grant_grant_option
 { 
 $$ = cat_str(7,make_str("grant"),$2,make_str("on"),$4,make_str("to"),$6,$7);
}
;


 RevokeStmt:
 REVOKE privileges ON privilege_target FROM grantee_list opt_drop_behavior
 { 
 $$ = cat_str(7,make_str("revoke"),$2,make_str("on"),$4,make_str("from"),$6,$7);
}
|  REVOKE GRANT OPTION FOR privileges ON privilege_target FROM grantee_list opt_drop_behavior
 { 
 $$ = cat_str(7,make_str("revoke grant option for"),$5,make_str("on"),$7,make_str("from"),$9,$10);
}
;


 privileges:
 privilege_list
 { 
 $$ = $1;
}
|  ALL
 { 
 $$ = make_str("all");
}
|  ALL PRIVILEGES
 { 
 $$ = make_str("all privileges");
}
;


 privilege_list:
 privilege
 { 
 $$ = $1;
}
|  privilege_list ',' privilege
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 privilege:
 SELECT
 { 
 $$ = make_str("select");
}
|  REFERENCES
 { 
 $$ = make_str("references");
}
|  CREATE
 { 
 $$ = make_str("create");
}
|  ColId
 { 
 $$ = $1;
}
;


 privilege_target:
 qualified_name_list
 { 
 $$ = $1;
}
|  TABLE qualified_name_list
 { 
 $$ = cat_str(2,make_str("table"),$2);
}
|  SEQUENCE qualified_name_list
 { 
 $$ = cat_str(2,make_str("sequence"),$2);
}
|  FUNCTION function_with_argtypes_list
 { 
 $$ = cat_str(2,make_str("function"),$2);
}
|  DATABASE name_list
 { 
 $$ = cat_str(2,make_str("database"),$2);
}
|  LANGUAGE name_list
 { 
 $$ = cat_str(2,make_str("language"),$2);
}
|  SCHEMA name_list
 { 
 $$ = cat_str(2,make_str("schema"),$2);
}
|  TABLESPACE name_list
 { 
 $$ = cat_str(2,make_str("tablespace"),$2);
}
;


 grantee_list:
 grantee
 { 
 $$ = $1;
}
|  grantee_list ',' grantee
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 grantee:
 RoleId
 { 
 $$ = $1;
}
|  GROUP_P RoleId
 { 
 $$ = cat_str(2,make_str("group"),$2);
}
;


 opt_grant_grant_option:
 WITH GRANT OPTION
 { 
 $$ = make_str("with grant option");
}
| 
 { 
 $$=EMPTY; }
;


 function_with_argtypes_list:
 function_with_argtypes
 { 
 $$ = $1;
}
|  function_with_argtypes_list ',' function_with_argtypes
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 function_with_argtypes:
 func_name func_args
 { 
 $$ = cat_str(2,$1,$2);
}
;


 GrantRoleStmt:
 GRANT privilege_list TO name_list opt_grant_admin_option opt_granted_by
 { 
 $$ = cat_str(6,make_str("grant"),$2,make_str("to"),$4,$5,$6);
}
;


 RevokeRoleStmt:
 REVOKE privilege_list FROM name_list opt_granted_by opt_drop_behavior
 { 
 $$ = cat_str(6,make_str("revoke"),$2,make_str("from"),$4,$5,$6);
}
|  REVOKE ADMIN OPTION FOR privilege_list FROM name_list opt_granted_by opt_drop_behavior
 { 
 $$ = cat_str(6,make_str("revoke admin option for"),$5,make_str("from"),$7,$8,$9);
}
;


 opt_grant_admin_option:
 WITH ADMIN OPTION
 { 
 $$ = make_str("with admin option");
}
| 
 { 
 $$=EMPTY; }
;


 opt_granted_by:
 GRANTED BY RoleId
 { 
 $$ = cat_str(2,make_str("granted by"),$3);
}
| 
 { 
 $$=EMPTY; }
;


 IndexStmt:
 CREATE index_opt_unique INDEX index_name ON qualified_name access_method_clause '(' index_params ')' opt_definition OptTableSpace where_clause
 { 
 $$ = cat_str(13,make_str("create"),$2,make_str("index"),$4,make_str("on"),$6,$7,make_str("("),$9,make_str(")"),$11,$12,$13);
}
|  CREATE index_opt_unique INDEX CONCURRENTLY index_name ON qualified_name access_method_clause '(' index_params ')' opt_definition OptTableSpace where_clause
 { 
 $$ = cat_str(13,make_str("create"),$2,make_str("index concurrently"),$5,make_str("on"),$7,$8,make_str("("),$10,make_str(")"),$12,$13,$14);
}
;


 index_opt_unique:
 UNIQUE
 { 
 $$ = make_str("unique");
}
| 
 { 
 $$=EMPTY; }
;


 access_method_clause:
 USING access_method
 { 
 $$ = cat_str(2,make_str("using"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 index_params:
 index_elem
 { 
 $$ = $1;
}
|  index_params ',' index_elem
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 index_elem:
 ColId opt_class opt_asc_desc opt_nulls_order
 { 
 $$ = cat_str(4,$1,$2,$3,$4);
}
|  func_expr opt_class opt_asc_desc opt_nulls_order
 { 
 $$ = cat_str(4,$1,$2,$3,$4);
}
|  '(' a_expr ')' opt_class opt_asc_desc opt_nulls_order
 { 
 $$ = cat_str(6,make_str("("),$2,make_str(")"),$4,$5,$6);
}
;


 opt_class:
 any_name
 { 
 $$ = $1;
}
|  USING any_name
 { 
 $$ = cat_str(2,make_str("using"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 opt_asc_desc:
 ASC
 { 
 $$ = make_str("asc");
}
|  DESC
 { 
 $$ = make_str("desc");
}
| 
 { 
 $$=EMPTY; }
;


 opt_nulls_order:
 NULLS_FIRST
 { 
 $$ = make_str("nulls first");
}
|  NULLS_LAST
 { 
 $$ = make_str("nulls last");
}
| 
 { 
 $$=EMPTY; }
;


 CreateFunctionStmt:
 CREATE opt_or_replace FUNCTION func_name func_args RETURNS func_return createfunc_opt_list opt_definition
 { 
 $$ = cat_str(9,make_str("create"),$2,make_str("function"),$4,$5,make_str("returns"),$7,$8,$9);
}
|  CREATE opt_or_replace FUNCTION func_name func_args RETURNS TABLE '(' table_func_column_list ')' createfunc_opt_list opt_definition
 { 
 $$ = cat_str(10,make_str("create"),$2,make_str("function"),$4,$5,make_str("returns table ("),$9,make_str(")"),$11,$12);
}
|  CREATE opt_or_replace FUNCTION func_name func_args createfunc_opt_list opt_definition
 { 
 $$ = cat_str(7,make_str("create"),$2,make_str("function"),$4,$5,$6,$7);
}
;


 opt_or_replace:
 OR REPLACE
 { 
 $$ = make_str("or replace");
}
| 
 { 
 $$=EMPTY; }
;


 func_args:
 '(' func_args_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
|  '(' ')'
 { 
 $$ = make_str("( )");
}
;


 func_args_list:
 func_arg
 { 
 $$ = $1;
}
|  func_args_list ',' func_arg
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 func_arg:
 arg_class param_name func_type
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  param_name arg_class func_type
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  param_name func_type
 { 
 $$ = cat_str(2,$1,$2);
}
|  arg_class func_type
 { 
 $$ = cat_str(2,$1,$2);
}
|  func_type
 { 
 $$ = $1;
}
;


 arg_class:
 IN_P
 { 
 $$ = make_str("in");
}
|  OUT_P
 { 
 $$ = make_str("out");
}
|  INOUT
 { 
 $$ = make_str("inout");
}
|  IN_P OUT_P
 { 
 $$ = make_str("in out");
}
|  VARIADIC
 { 
 $$ = make_str("variadic");
}
;


 param_name:
 type_function_name
 { 
 $$ = $1;
}
;


 func_return:
 func_type
 { 
 $$ = $1;
}
;


 func_type:
 Typename
 { 
 $$ = $1;
}
|  type_function_name attrs '%' TYPE_P
 { 
 $$ = cat_str(3,$1,$2,make_str("% type"));
}
|  SETOF type_function_name attrs '%' TYPE_P
 { 
 $$ = cat_str(4,make_str("setof"),$2,$3,make_str("% type"));
}
;


 createfunc_opt_list:
 createfunc_opt_item
 { 
 $$ = $1;
}
|  createfunc_opt_list createfunc_opt_item
 { 
 $$ = cat_str(2,$1,$2);
}
;


 common_func_opt_item:
 CALLED ON NULL_P INPUT_P
 { 
 $$ = make_str("called on null input");
}
|  RETURNS NULL_P ON NULL_P INPUT_P
 { 
 $$ = make_str("returns null on null input");
}
|  STRICT_P
 { 
 $$ = make_str("strict");
}
|  IMMUTABLE
 { 
 $$ = make_str("immutable");
}
|  STABLE
 { 
 $$ = make_str("stable");
}
|  VOLATILE
 { 
 $$ = make_str("volatile");
}
|  EXTERNAL SECURITY DEFINER
 { 
 $$ = make_str("external security definer");
}
|  EXTERNAL SECURITY INVOKER
 { 
 $$ = make_str("external security invoker");
}
|  SECURITY DEFINER
 { 
 $$ = make_str("security definer");
}
|  SECURITY INVOKER
 { 
 $$ = make_str("security invoker");
}
|  COST NumericOnly
 { 
 $$ = cat_str(2,make_str("cost"),$2);
}
|  ROWS NumericOnly
 { 
 $$ = cat_str(2,make_str("rows"),$2);
}
|  SetResetClause
 { 
 $$ = $1;
}
;


 createfunc_opt_item:
 AS func_as
 { 
 $$ = cat_str(2,make_str("as"),$2);
}
|  LANGUAGE ColId_or_Sconst
 { 
 $$ = cat_str(2,make_str("language"),$2);
}
|  common_func_opt_item
 { 
 $$ = $1;
}
;


 func_as:
 ecpg_sconst
 { 
 $$ = $1;
}
|  ecpg_sconst ',' ecpg_sconst
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 opt_definition:
 WITH definition
 { 
 $$ = cat_str(2,make_str("with"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 table_func_column:
 param_name func_type
 { 
 $$ = cat_str(2,$1,$2);
}
;


 table_func_column_list:
 table_func_column
 { 
 $$ = $1;
}
|  table_func_column_list ',' table_func_column
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 AlterFunctionStmt:
 ALTER FUNCTION function_with_argtypes alterfunc_opt_list opt_restrict
 { 
 $$ = cat_str(4,make_str("alter function"),$3,$4,$5);
}
;


 alterfunc_opt_list:
 common_func_opt_item
 { 
 $$ = $1;
}
|  alterfunc_opt_list common_func_opt_item
 { 
 $$ = cat_str(2,$1,$2);
}
;


 opt_restrict:
 RESTRICT
 { 
 $$ = make_str("restrict");
}
| 
 { 
 $$=EMPTY; }
;


 RemoveFuncStmt:
 DROP FUNCTION func_name func_args opt_drop_behavior
 { 
 $$ = cat_str(4,make_str("drop function"),$3,$4,$5);
}
|  DROP FUNCTION IF_P EXISTS func_name func_args opt_drop_behavior
 { 
 $$ = cat_str(4,make_str("drop function if exists"),$5,$6,$7);
}
;


 RemoveAggrStmt:
 DROP AGGREGATE func_name aggr_args opt_drop_behavior
 { 
 $$ = cat_str(4,make_str("drop aggregate"),$3,$4,$5);
}
|  DROP AGGREGATE IF_P EXISTS func_name aggr_args opt_drop_behavior
 { 
 $$ = cat_str(4,make_str("drop aggregate if exists"),$5,$6,$7);
}
;


 RemoveOperStmt:
 DROP OPERATOR any_operator oper_argtypes opt_drop_behavior
 { 
 $$ = cat_str(4,make_str("drop operator"),$3,$4,$5);
}
|  DROP OPERATOR IF_P EXISTS any_operator oper_argtypes opt_drop_behavior
 { 
 $$ = cat_str(4,make_str("drop operator if exists"),$5,$6,$7);
}
;


 oper_argtypes:
 '(' Typename ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
|  '(' Typename ',' Typename ')'
 { 
 $$ = cat_str(5,make_str("("),$2,make_str(","),$4,make_str(")"));
}
|  '(' NONE ',' Typename ')'
 { 
 $$ = cat_str(3,make_str("( none ,"),$4,make_str(")"));
}
|  '(' Typename ',' NONE ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(", none )"));
}
;


 any_operator:
 all_Op
 { 
 $$ = $1;
}
|  ColId '.' any_operator
 { 
 $$ = cat_str(3,$1,make_str("."),$3);
}
;


 CreateCastStmt:
 CREATE CAST '(' Typename AS Typename ')' WITH FUNCTION function_with_argtypes cast_context
 { 
 $$ = cat_str(7,make_str("create cast ("),$4,make_str("as"),$6,make_str(") with function"),$10,$11);
}
|  CREATE CAST '(' Typename AS Typename ')' WITHOUT FUNCTION cast_context
 { 
 $$ = cat_str(6,make_str("create cast ("),$4,make_str("as"),$6,make_str(") without function"),$10);
}
|  CREATE CAST '(' Typename AS Typename ')' WITH INOUT cast_context
 { 
 $$ = cat_str(6,make_str("create cast ("),$4,make_str("as"),$6,make_str(") with inout"),$10);
}
;


 cast_context:
 AS IMPLICIT_P
 { 
 $$ = make_str("as implicit");
}
|  AS ASSIGNMENT
 { 
 $$ = make_str("as assignment");
}
| 
 { 
 $$=EMPTY; }
;


 DropCastStmt:
 DROP CAST opt_if_exists '(' Typename AS Typename ')' opt_drop_behavior
 { 
 $$ = cat_str(8,make_str("drop cast"),$3,make_str("("),$5,make_str("as"),$7,make_str(")"),$9);
}
;


 opt_if_exists:
 IF_P EXISTS
 { 
 $$ = make_str("if exists");
}
| 
 { 
 $$=EMPTY; }
;


 ReindexStmt:
 REINDEX reindex_type qualified_name opt_force
 { 
 $$ = cat_str(4,make_str("reindex"),$2,$3,$4);
}
|  REINDEX SYSTEM_P name opt_force
 { 
 $$ = cat_str(3,make_str("reindex system"),$3,$4);
}
|  REINDEX DATABASE name opt_force
 { 
 $$ = cat_str(3,make_str("reindex database"),$3,$4);
}
;


 reindex_type:
 INDEX
 { 
 $$ = make_str("index");
}
|  TABLE
 { 
 $$ = make_str("table");
}
;


 opt_force:
 FORCE
 { 
 $$ = make_str("force");
}
| 
 { 
 $$=EMPTY; }
;


 RenameStmt:
 ALTER AGGREGATE func_name aggr_args RENAME TO name
 { 
 $$ = cat_str(5,make_str("alter aggregate"),$3,$4,make_str("rename to"),$7);
}
|  ALTER CONVERSION_P any_name RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter conversion"),$3,make_str("rename to"),$6);
}
|  ALTER DATABASE database_name RENAME TO database_name
 { 
 $$ = cat_str(4,make_str("alter database"),$3,make_str("rename to"),$6);
}
|  ALTER FUNCTION function_with_argtypes RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter function"),$3,make_str("rename to"),$6);
}
|  ALTER GROUP_P RoleId RENAME TO RoleId
 { 
 $$ = cat_str(4,make_str("alter group"),$3,make_str("rename to"),$6);
}
|  ALTER opt_procedural LANGUAGE name RENAME TO name
 { 
 $$ = cat_str(6,make_str("alter"),$2,make_str("language"),$4,make_str("rename to"),$7);
}
|  ALTER OPERATOR CLASS any_name USING access_method RENAME TO name
 { 
 $$ = cat_str(6,make_str("alter operator class"),$4,make_str("using"),$6,make_str("rename to"),$9);
}
|  ALTER OPERATOR FAMILY any_name USING access_method RENAME TO name
 { 
 $$ = cat_str(6,make_str("alter operator family"),$4,make_str("using"),$6,make_str("rename to"),$9);
}
|  ALTER SCHEMA name RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter schema"),$3,make_str("rename to"),$6);
}
|  ALTER TABLE relation_expr RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter table"),$3,make_str("rename to"),$6);
}
|  ALTER SEQUENCE relation_expr RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter sequence"),$3,make_str("rename to"),$6);
}
|  ALTER VIEW relation_expr RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter view"),$3,make_str("rename to"),$6);
}
|  ALTER INDEX relation_expr RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter index"),$3,make_str("rename to"),$6);
}
|  ALTER TABLE relation_expr RENAME opt_column name TO name
 { 
 $$ = cat_str(7,make_str("alter table"),$3,make_str("rename"),$5,$6,make_str("to"),$8);
}
|  ALTER TRIGGER name ON relation_expr RENAME TO name
 { 
 $$ = cat_str(6,make_str("alter trigger"),$3,make_str("on"),$5,make_str("rename to"),$8);
}
|  ALTER ROLE RoleId RENAME TO RoleId
 { 
 $$ = cat_str(4,make_str("alter role"),$3,make_str("rename to"),$6);
}
|  ALTER USER RoleId RENAME TO RoleId
 { 
 $$ = cat_str(4,make_str("alter user"),$3,make_str("rename to"),$6);
}
|  ALTER TABLESPACE name RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter tablespace"),$3,make_str("rename to"),$6);
}
|  ALTER TEXT_P SEARCH PARSER any_name RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter text search parser"),$5,make_str("rename to"),$8);
}
|  ALTER TEXT_P SEARCH DICTIONARY any_name RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter text search dictionary"),$5,make_str("rename to"),$8);
}
|  ALTER TEXT_P SEARCH TEMPLATE any_name RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter text search template"),$5,make_str("rename to"),$8);
}
|  ALTER TEXT_P SEARCH CONFIGURATION any_name RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter text search configuration"),$5,make_str("rename to"),$8);
}
|  ALTER TYPE_P any_name RENAME TO name
 { 
 $$ = cat_str(4,make_str("alter type"),$3,make_str("rename to"),$6);
}
;


 opt_column:
 COLUMN
 { 
 $$ = make_str("column");
}
| 
 { 
 $$=EMPTY; }
;


 opt_set_data:
 SET DATA_P
 { 
 $$ = make_str("set data");
}
| 
 { 
 $$=EMPTY; }
;


 AlterObjectSchemaStmt:
 ALTER AGGREGATE func_name aggr_args SET SCHEMA name
 { 
 $$ = cat_str(5,make_str("alter aggregate"),$3,$4,make_str("set schema"),$7);
}
|  ALTER DOMAIN_P any_name SET SCHEMA name
 { 
 $$ = cat_str(4,make_str("alter domain"),$3,make_str("set schema"),$6);
}
|  ALTER FUNCTION function_with_argtypes SET SCHEMA name
 { 
 $$ = cat_str(4,make_str("alter function"),$3,make_str("set schema"),$6);
}
|  ALTER TABLE relation_expr SET SCHEMA name
 { 
 $$ = cat_str(4,make_str("alter table"),$3,make_str("set schema"),$6);
}
|  ALTER SEQUENCE relation_expr SET SCHEMA name
 { 
 $$ = cat_str(4,make_str("alter sequence"),$3,make_str("set schema"),$6);
}
|  ALTER VIEW relation_expr SET SCHEMA name
 { 
 $$ = cat_str(4,make_str("alter view"),$3,make_str("set schema"),$6);
}
|  ALTER TYPE_P any_name SET SCHEMA name
 { 
 $$ = cat_str(4,make_str("alter type"),$3,make_str("set schema"),$6);
}
;


 AlterOwnerStmt:
 ALTER AGGREGATE func_name aggr_args OWNER TO RoleId
 { 
 $$ = cat_str(5,make_str("alter aggregate"),$3,$4,make_str("owner to"),$7);
}
|  ALTER CONVERSION_P any_name OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter conversion"),$3,make_str("owner to"),$6);
}
|  ALTER DATABASE database_name OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter database"),$3,make_str("owner to"),$6);
}
|  ALTER DOMAIN_P any_name OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter domain"),$3,make_str("owner to"),$6);
}
|  ALTER FUNCTION function_with_argtypes OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter function"),$3,make_str("owner to"),$6);
}
|  ALTER opt_procedural LANGUAGE name OWNER TO RoleId
 { 
 $$ = cat_str(6,make_str("alter"),$2,make_str("language"),$4,make_str("owner to"),$7);
}
|  ALTER OPERATOR any_operator oper_argtypes OWNER TO RoleId
 { 
 $$ = cat_str(5,make_str("alter operator"),$3,$4,make_str("owner to"),$7);
}
|  ALTER OPERATOR CLASS any_name USING access_method OWNER TO RoleId
 { 
 $$ = cat_str(6,make_str("alter operator class"),$4,make_str("using"),$6,make_str("owner to"),$9);
}
|  ALTER OPERATOR FAMILY any_name USING access_method OWNER TO RoleId
 { 
 $$ = cat_str(6,make_str("alter operator family"),$4,make_str("using"),$6,make_str("owner to"),$9);
}
|  ALTER SCHEMA name OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter schema"),$3,make_str("owner to"),$6);
}
|  ALTER TYPE_P any_name OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter type"),$3,make_str("owner to"),$6);
}
|  ALTER TABLESPACE name OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter tablespace"),$3,make_str("owner to"),$6);
}
|  ALTER TEXT_P SEARCH DICTIONARY any_name OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter text search dictionary"),$5,make_str("owner to"),$8);
}
|  ALTER TEXT_P SEARCH CONFIGURATION any_name OWNER TO RoleId
 { 
 $$ = cat_str(4,make_str("alter text search configuration"),$5,make_str("owner to"),$8);
}
;


 RuleActionList:
 NOTHING
 { 
 $$ = make_str("nothing");
}
|  RuleActionStmt
 { 
 $$ = $1;
}
|  '(' RuleActionMulti ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
;


 RuleActionMulti:
 RuleActionMulti ';' RuleActionStmtOrEmpty
 { 
 $$ = cat_str(3,$1,make_str(";"),$3);
}
|  RuleActionStmtOrEmpty
 { 
 $$ = $1;
}
;


 RuleActionStmt:
 SelectStmt
 { 
 $$ = $1;
}
|  InsertStmt
 { 
 $$ = $1;
}
|  UpdateStmt
 { 
 $$ = $1;
}
|  DeleteStmt
 { 
 $$ = $1;
}
|  NotifyStmt
 { 
 $$ = $1;
}
;


 RuleActionStmtOrEmpty:
 RuleActionStmt
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 event:
 SELECT
 { 
 $$ = make_str("select");
}
|  UPDATE
 { 
 $$ = make_str("update");
}
|  DELETE_P
 { 
 $$ = make_str("delete");
}
|  INSERT
 { 
 $$ = make_str("insert");
}
;


 opt_instead:
 INSTEAD
 { 
 $$ = make_str("instead");
}
|  ALSO
 { 
 $$ = make_str("also");
}
| 
 { 
 $$=EMPTY; }
;


 DropRuleStmt:
 DROP RULE name ON qualified_name opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop rule"),$3,make_str("on"),$5,$6);
}
|  DROP RULE IF_P EXISTS name ON qualified_name opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("drop rule if exists"),$5,make_str("on"),$7,$8);
}
;


 NotifyStmt:
 NOTIFY ColId
 { 
 $$ = cat_str(2,make_str("notify"),$2);
}
;


 ListenStmt:
 LISTEN ColId
 { 
 $$ = cat_str(2,make_str("listen"),$2);
}
;


 UnlistenStmt:
 UNLISTEN ColId
 { 
 $$ = cat_str(2,make_str("unlisten"),$2);
}
|  UNLISTEN '*'
 { 
 $$ = make_str("unlisten *");
}
;


 TransactionStmt:
 ABORT_P opt_transaction
 { 
 $$ = cat_str(2,make_str("abort"),$2);
}
|  BEGIN_P opt_transaction transaction_mode_list_or_empty
 { 
 $$ = cat_str(3,make_str("begin"),$2,$3);
}
|  START TRANSACTION transaction_mode_list_or_empty
 { 
 $$ = cat_str(2,make_str("start transaction"),$3);
}
|  COMMIT opt_transaction
 { 
 $$ = cat_str(2,make_str("commit"),$2);
}
|  END_P opt_transaction
 { 
 $$ = cat_str(2,make_str("end"),$2);
}
|  ROLLBACK opt_transaction
 { 
 $$ = cat_str(2,make_str("rollback"),$2);
}
|  SAVEPOINT ColId
 { 
 $$ = cat_str(2,make_str("savepoint"),$2);
}
|  RELEASE SAVEPOINT ColId
 { 
 $$ = cat_str(2,make_str("release savepoint"),$3);
}
|  RELEASE ColId
 { 
 $$ = cat_str(2,make_str("release"),$2);
}
|  ROLLBACK opt_transaction TO SAVEPOINT ColId
 { 
 $$ = cat_str(4,make_str("rollback"),$2,make_str("to savepoint"),$5);
}
|  ROLLBACK opt_transaction TO ColId
 { 
 $$ = cat_str(4,make_str("rollback"),$2,make_str("to"),$4);
}
|  PREPARE TRANSACTION ecpg_sconst
 { 
 $$ = cat_str(2,make_str("prepare transaction"),$3);
}
|  COMMIT PREPARED ecpg_sconst
 { 
 $$ = cat_str(2,make_str("commit prepared"),$3);
}
|  ROLLBACK PREPARED ecpg_sconst
 { 
 $$ = cat_str(2,make_str("rollback prepared"),$3);
}
;


 opt_transaction:
 WORK
 { 
 $$ = make_str("work");
}
|  TRANSACTION
 { 
 $$ = make_str("transaction");
}
| 
 { 
 $$=EMPTY; }
;


 transaction_mode_item:
 ISOLATION LEVEL iso_level
 { 
 $$ = cat_str(2,make_str("isolation level"),$3);
}
|  READ ONLY
 { 
 $$ = make_str("read only");
}
|  READ WRITE
 { 
 $$ = make_str("read write");
}
;


 transaction_mode_list:
 transaction_mode_item
 { 
 $$ = $1;
}
|  transaction_mode_list ',' transaction_mode_item
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
|  transaction_mode_list transaction_mode_item
 { 
 $$ = cat_str(2,$1,$2);
}
;


 transaction_mode_list_or_empty:
 transaction_mode_list
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 ViewStmt:
 CREATE OptTemp VIEW qualified_name opt_column_list AS SelectStmt opt_check_option
 { 
 $$ = cat_str(8,make_str("create"),$2,make_str("view"),$4,$5,make_str("as"),$7,$8);
}
|  CREATE OR REPLACE OptTemp VIEW qualified_name opt_column_list AS SelectStmt opt_check_option
 { 
 $$ = cat_str(8,make_str("create or replace"),$4,make_str("view"),$6,$7,make_str("as"),$9,$10);
}
;


 opt_check_option:
 WITH CHECK OPTION
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = make_str("with check option");
}
|  WITH CASCADED CHECK OPTION
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = make_str("with cascaded check option");
}
|  WITH LOCAL CHECK OPTION
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = make_str("with local check option");
}
| 
 { 
 $$=EMPTY; }
;


 LoadStmt:
 LOAD file_name
 { 
 $$ = cat_str(2,make_str("load"),$2);
}
;


 CreatedbStmt:
 CREATE DATABASE database_name opt_with createdb_opt_list
 { 
 $$ = cat_str(4,make_str("create database"),$3,$4,$5);
}
;


 createdb_opt_list:
 createdb_opt_list createdb_opt_item
 { 
 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 createdb_opt_item:
 TABLESPACE opt_equal name
 { 
 $$ = cat_str(3,make_str("tablespace"),$2,$3);
}
|  TABLESPACE opt_equal DEFAULT
 { 
 $$ = cat_str(3,make_str("tablespace"),$2,make_str("default"));
}
|  LOCATION opt_equal ecpg_sconst
 { 
 $$ = cat_str(3,make_str("location"),$2,$3);
}
|  LOCATION opt_equal DEFAULT
 { 
 $$ = cat_str(3,make_str("location"),$2,make_str("default"));
}
|  TEMPLATE opt_equal name
 { 
 $$ = cat_str(3,make_str("template"),$2,$3);
}
|  TEMPLATE opt_equal DEFAULT
 { 
 $$ = cat_str(3,make_str("template"),$2,make_str("default"));
}
| ENCODING opt_equal ecpg_real_sconst
 { 
 $$ = cat_str(3,make_str("encoding"),$2,$3);
}
|  ENCODING opt_equal ecpg_iconst
 { 
 $$ = cat_str(3,make_str("encoding"),$2,$3);
}
|  ENCODING opt_equal DEFAULT
 { 
 $$ = cat_str(3,make_str("encoding"),$2,make_str("default"));
}
|  COLLATE opt_equal ecpg_sconst
 { 
 $$ = cat_str(3,make_str("collate"),$2,$3);
}
|  COLLATE opt_equal DEFAULT
 { 
 $$ = cat_str(3,make_str("collate"),$2,make_str("default"));
}
|  CTYPE opt_equal ecpg_sconst
 { 
 $$ = cat_str(3,make_str("ctype"),$2,$3);
}
|  CTYPE opt_equal DEFAULT
 { 
 $$ = cat_str(3,make_str("ctype"),$2,make_str("default"));
}
|  CONNECTION LIMIT opt_equal SignedIconst
 { 
 $$ = cat_str(3,make_str("connection limit"),$3,$4);
}
|  OWNER opt_equal name
 { 
 $$ = cat_str(3,make_str("owner"),$2,$3);
}
|  OWNER opt_equal DEFAULT
 { 
 $$ = cat_str(3,make_str("owner"),$2,make_str("default"));
}
;


 opt_equal:
 '='
 { 
 $$ = make_str("=");
}
| 
 { 
 $$=EMPTY; }
;


 AlterDatabaseStmt:
 ALTER DATABASE database_name opt_with alterdb_opt_list
 { 
 $$ = cat_str(4,make_str("alter database"),$3,$4,$5);
}
|  ALTER DATABASE database_name SET TABLESPACE name
 { 
 $$ = cat_str(4,make_str("alter database"),$3,make_str("set tablespace"),$6);
}
;


 AlterDatabaseSetStmt:
 ALTER DATABASE database_name SetResetClause
 { 
 $$ = cat_str(3,make_str("alter database"),$3,$4);
}
;


 alterdb_opt_list:
 alterdb_opt_list alterdb_opt_item
 { 
 $$ = cat_str(2,$1,$2);
}
| 
 { 
 $$=EMPTY; }
;


 alterdb_opt_item:
 CONNECTION LIMIT opt_equal SignedIconst
 { 
 $$ = cat_str(3,make_str("connection limit"),$3,$4);
}
;


 DropdbStmt:
 DROP DATABASE database_name
 { 
 $$ = cat_str(2,make_str("drop database"),$3);
}
|  DROP DATABASE IF_P EXISTS database_name
 { 
 $$ = cat_str(2,make_str("drop database if exists"),$5);
}
;


 CreateDomainStmt:
 CREATE DOMAIN_P any_name opt_as Typename ColQualList
 { 
 $$ = cat_str(5,make_str("create domain"),$3,$4,$5,$6);
}
;


 AlterDomainStmt:
 ALTER DOMAIN_P any_name alter_column_default
 { 
 $$ = cat_str(3,make_str("alter domain"),$3,$4);
}
|  ALTER DOMAIN_P any_name DROP NOT NULL_P
 { 
 $$ = cat_str(3,make_str("alter domain"),$3,make_str("drop not null"));
}
|  ALTER DOMAIN_P any_name SET NOT NULL_P
 { 
 $$ = cat_str(3,make_str("alter domain"),$3,make_str("set not null"));
}
|  ALTER DOMAIN_P any_name ADD_P TableConstraint
 { 
 $$ = cat_str(4,make_str("alter domain"),$3,make_str("add"),$5);
}
|  ALTER DOMAIN_P any_name DROP CONSTRAINT name opt_drop_behavior
 { 
 $$ = cat_str(5,make_str("alter domain"),$3,make_str("drop constraint"),$6,$7);
}
;


 opt_as:
 AS
 { 
 $$ = make_str("as");
}
| 
 { 
 $$=EMPTY; }
;


 AlterTSDictionaryStmt:
 ALTER TEXT_P SEARCH DICTIONARY any_name definition
 { 
 $$ = cat_str(3,make_str("alter text search dictionary"),$5,$6);
}
;


 AlterTSConfigurationStmt:
 ALTER TEXT_P SEARCH CONFIGURATION any_name ADD_P MAPPING FOR name_list WITH any_name_list
 { 
 $$ = cat_str(6,make_str("alter text search configuration"),$5,make_str("add mapping for"),$9,make_str("with"),$11);
}
|  ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING FOR name_list WITH any_name_list
 { 
 $$ = cat_str(6,make_str("alter text search configuration"),$5,make_str("alter mapping for"),$9,make_str("with"),$11);
}
|  ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING REPLACE any_name WITH any_name
 { 
 $$ = cat_str(6,make_str("alter text search configuration"),$5,make_str("alter mapping replace"),$9,make_str("with"),$11);
}
|  ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING FOR name_list REPLACE any_name WITH any_name
 { 
 $$ = cat_str(8,make_str("alter text search configuration"),$5,make_str("alter mapping for"),$9,make_str("replace"),$11,make_str("with"),$13);
}
|  ALTER TEXT_P SEARCH CONFIGURATION any_name DROP MAPPING FOR name_list
 { 
 $$ = cat_str(4,make_str("alter text search configuration"),$5,make_str("drop mapping for"),$9);
}
|  ALTER TEXT_P SEARCH CONFIGURATION any_name DROP MAPPING IF_P EXISTS FOR name_list
 { 
 $$ = cat_str(4,make_str("alter text search configuration"),$5,make_str("drop mapping if exists for"),$11);
}
;


 CreateConversionStmt:
 CREATE opt_default CONVERSION_P any_name FOR ecpg_sconst TO ecpg_sconst FROM any_name
 { 
 $$ = cat_str(10,make_str("create"),$2,make_str("conversion"),$4,make_str("for"),$6,make_str("to"),$8,make_str("from"),$10);
}
;


 ClusterStmt:
 CLUSTER qualified_name cluster_index_specification
 { 
 $$ = cat_str(3,make_str("cluster"),$2,$3);
}
|  CLUSTER
 { 
 $$ = make_str("cluster");
}
|  CLUSTER index_name ON qualified_name
 { 
 $$ = cat_str(4,make_str("cluster"),$2,make_str("on"),$4);
}
;


 cluster_index_specification:
 USING index_name
 { 
 $$ = cat_str(2,make_str("using"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 VacuumStmt:
 VACUUM opt_full opt_freeze opt_verbose
 { 
 $$ = cat_str(4,make_str("vacuum"),$2,$3,$4);
}
|  VACUUM opt_full opt_freeze opt_verbose qualified_name
 { 
 $$ = cat_str(5,make_str("vacuum"),$2,$3,$4,$5);
}
|  VACUUM opt_full opt_freeze opt_verbose AnalyzeStmt
 { 
 $$ = cat_str(5,make_str("vacuum"),$2,$3,$4,$5);
}
;


 AnalyzeStmt:
 analyze_keyword opt_verbose
 { 
 $$ = cat_str(2,$1,$2);
}
|  analyze_keyword opt_verbose qualified_name opt_name_list
 { 
 $$ = cat_str(4,$1,$2,$3,$4);
}
;


 analyze_keyword:
 ANALYZE
 { 
 $$ = make_str("analyze");
}
|  ANALYSE
 { 
 $$ = make_str("analyse");
}
;


 opt_verbose:
 VERBOSE
 { 
 $$ = make_str("verbose");
}
| 
 { 
 $$=EMPTY; }
;


 opt_full:
 FULL
 { 
 $$ = make_str("full");
}
| 
 { 
 $$=EMPTY; }
;


 opt_freeze:
 FREEZE
 { 
 $$ = make_str("freeze");
}
| 
 { 
 $$=EMPTY; }
;


 opt_name_list:
 '(' name_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 ExplainStmt:
 EXPLAIN opt_analyze opt_verbose ExplainableStmt
 { 
 $$ = cat_str(4,make_str("explain"),$2,$3,$4);
}
;


 ExplainableStmt:
 SelectStmt
 { 
 $$ = $1;
}
|  InsertStmt
 { 
 $$ = $1;
}
|  UpdateStmt
 { 
 $$ = $1;
}
|  DeleteStmt
 { 
 $$ = $1;
}
|  DeclareCursorStmt
 { 
 $$ = $1;
}
|  CreateAsStmt
 { 
 $$ = $1;
}
|  ExecuteStmt
 { 
 $$ = $1;
}
;


 opt_analyze:
 analyze_keyword
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 PrepareStmt:
PREPARE prepared_name prep_type_clause AS PreparableStmt
	{
		$$.name = $2;
		$$.type = $3;
		$$.stmt = cat_str(3, make_str("\""), $5, make_str("\""));
	}
	| PREPARE prepared_name FROM execstring
	{
		$$.name = $2;
		$$.type = NULL;
		$$.stmt = $4;
	}
;


 prep_type_clause:
 '(' type_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 PreparableStmt:
 SelectStmt
 { 
 $$ = $1;
}
|  InsertStmt
 { 
 $$ = $1;
}
|  UpdateStmt
 { 
 $$ = $1;
}
|  DeleteStmt
 { 
 $$ = $1;
}
;


 ExecuteStmt:
EXECUTE prepared_name execute_param_clause execute_rest
	{ $$ = $2; }
| CREATE OptTemp TABLE create_as_target AS EXECUTE prepared_name execute_param_clause
 { 
 $$ = cat_str(7,make_str("create"),$2,make_str("table"),$4,make_str("as execute"),$7,$8);
}
;


 execute_param_clause:
 '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 InsertStmt:
 INSERT INTO qualified_name insert_rest returning_clause
 { 
 $$ = cat_str(4,make_str("insert into"),$3,$4,$5);
}
;


 insert_rest:
 SelectStmt
 { 
 $$ = $1;
}
|  '(' insert_column_list ')' SelectStmt
 { 
 $$ = cat_str(4,make_str("("),$2,make_str(")"),$4);
}
|  DEFAULT VALUES
 { 
 $$ = make_str("default values");
}
;


 insert_column_list:
 insert_column_item
 { 
 $$ = $1;
}
|  insert_column_list ',' insert_column_item
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 insert_column_item:
 ColId opt_indirection
 { 
 $$ = cat_str(2,$1,$2);
}
;


 returning_clause:
 RETURNING target_list
 { 
 $$ = cat_str(2,make_str("returning"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 DeleteStmt:
 DELETE_P FROM relation_expr_opt_alias using_clause where_or_current_clause returning_clause
 { 
 $$ = cat_str(5,make_str("delete from"),$3,$4,$5,$6);
}
;


 using_clause:
 USING from_list
 { 
 $$ = cat_str(2,make_str("using"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 LockStmt:
 LOCK_P opt_table qualified_name_list opt_lock opt_nowait
 { 
 $$ = cat_str(5,make_str("lock"),$2,$3,$4,$5);
}
;


 opt_lock:
 IN_P lock_type MODE
 { 
 $$ = cat_str(3,make_str("in"),$2,make_str("mode"));
}
| 
 { 
 $$=EMPTY; }
;


 lock_type:
 ACCESS SHARE
 { 
 $$ = make_str("access share");
}
|  ROW SHARE
 { 
 $$ = make_str("row share");
}
|  ROW EXCLUSIVE
 { 
 $$ = make_str("row exclusive");
}
|  SHARE UPDATE EXCLUSIVE
 { 
 $$ = make_str("share update exclusive");
}
|  SHARE
 { 
 $$ = make_str("share");
}
|  SHARE ROW EXCLUSIVE
 { 
 $$ = make_str("share row exclusive");
}
|  EXCLUSIVE
 { 
 $$ = make_str("exclusive");
}
|  ACCESS EXCLUSIVE
 { 
 $$ = make_str("access exclusive");
}
;


 opt_nowait:
 NOWAIT
 { 
 $$ = make_str("nowait");
}
| 
 { 
 $$=EMPTY; }
;


 UpdateStmt:
 UPDATE relation_expr_opt_alias SET set_clause_list from_clause where_or_current_clause returning_clause
 { 
 $$ = cat_str(7,make_str("update"),$2,make_str("set"),$4,$5,$6,$7);
}
;


 set_clause_list:
 set_clause
 { 
 $$ = $1;
}
|  set_clause_list ',' set_clause
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 set_clause:
 single_set_clause
 { 
 $$ = $1;
}
|  multiple_set_clause
 { 
 $$ = $1;
}
;


 single_set_clause:
 set_target '=' ctext_expr
 { 
 $$ = cat_str(3,$1,make_str("="),$3);
}
;


 multiple_set_clause:
 '(' set_target_list ')' '=' ctext_row
 { 
 $$ = cat_str(4,make_str("("),$2,make_str(") ="),$5);
}
;


 set_target:
 ColId opt_indirection
 { 
 $$ = cat_str(2,$1,$2);
}
;


 set_target_list:
 set_target
 { 
 $$ = $1;
}
|  set_target_list ',' set_target
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 DeclareCursorStmt:
 DECLARE name cursor_options CURSOR opt_hold FOR SelectStmt
	{
		struct cursor *ptr, *this;

		for (ptr = cur; ptr != NULL; ptr = ptr->next)
		{
			if (strcmp($2, ptr->name) == 0)
				mmerror(PARSE_ERROR, ET_ERROR, "cursor \"%s\" already defined\n", $2);
		}

		this = (struct cursor *) mm_alloc(sizeof(struct cursor));

		this->next = cur;
		this->name = $2;
		this->connection = connection;
		this->opened = false;
		this->command =  cat_str(7, make_str("declare"), mm_strdup($2), $3, make_str("cursor"), $5, make_str("for"), $7);
		this->argsinsert = argsinsert;
		this->argsresult = argsresult;
		argsinsert = argsresult = NULL;
		cur = this;

		if (INFORMIX_MODE)
			$$ = cat_str(5, adjust_informix(this->argsinsert), adjust_informix(this->argsresult), make_str("/*"), mm_strdup(this->command), make_str("*/"));
		else
			$$ = cat_str(3, make_str("/*"), mm_strdup(this->command), make_str("*/"));
	}
;


 cursor_options:

 { 
 $$=EMPTY; }
|  cursor_options NO SCROLL
 { 
 $$ = cat_str(2,$1,make_str("no scroll"));
}
|  cursor_options SCROLL
 { 
 $$ = cat_str(2,$1,make_str("scroll"));
}
|  cursor_options BINARY
 { 
 $$ = cat_str(2,$1,make_str("binary"));
}
|  cursor_options INSENSITIVE
 { 
 $$ = cat_str(2,$1,make_str("insensitive"));
}
;


 opt_hold:

	{
		if (compat == ECPG_COMPAT_INFORMIX_SE && autocommit == true)
			$$ = make_str("with hold");
		else
			$$ = EMPTY;
	}
|  WITH HOLD
 { 
 $$ = make_str("with hold");
}
|  WITHOUT HOLD
 { 
 $$ = make_str("without hold");
}
;


 SelectStmt:
 select_no_parens %prec UMINUS
 { 
 $$ = $1;
}
|  select_with_parens %prec UMINUS
 { 
 $$ = $1;
}
;


 select_with_parens:
 '(' select_no_parens ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
|  '(' select_with_parens ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
;


 select_no_parens:
 simple_select
 { 
 $$ = $1;
}
|  select_clause sort_clause
 { 
 $$ = cat_str(2,$1,$2);
}
|  select_clause opt_sort_clause for_locking_clause opt_select_limit
 { 
 $$ = cat_str(4,$1,$2,$3,$4);
}
|  select_clause opt_sort_clause select_limit opt_for_locking_clause
 { 
 $$ = cat_str(4,$1,$2,$3,$4);
}
|  with_clause simple_select
 { 
 $$ = cat_str(2,$1,$2);
}
|  with_clause select_clause sort_clause
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  with_clause select_clause opt_sort_clause for_locking_clause opt_select_limit
 { 
 $$ = cat_str(5,$1,$2,$3,$4,$5);
}
|  with_clause select_clause opt_sort_clause select_limit opt_for_locking_clause
 { 
 $$ = cat_str(5,$1,$2,$3,$4,$5);
}
;


 select_clause:
 simple_select
 { 
 $$ = $1;
}
|  select_with_parens
 { 
 $$ = $1;
}
;


 simple_select:
 SELECT opt_distinct target_list into_clause from_clause where_clause group_clause having_clause
 { 
 $$ = cat_str(8,make_str("select"),$2,$3,$4,$5,$6,$7,$8);
}
|  values_clause
 { 
 $$ = $1;
}
|  select_clause UNION opt_all select_clause
 { 
 $$ = cat_str(4,$1,make_str("union"),$3,$4);
}
|  select_clause INTERSECT opt_all select_clause
 { 
 $$ = cat_str(4,$1,make_str("intersect"),$3,$4);
}
|  select_clause EXCEPT opt_all select_clause
 { 
 $$ = cat_str(4,$1,make_str("except"),$3,$4);
}
;


 with_clause:
 WITH cte_list
 { 
 $$ = cat_str(2,make_str("with"),$2);
}
|  WITH RECURSIVE cte_list
 { 
 $$ = cat_str(2,make_str("with recursive"),$3);
}
;


 cte_list:
 common_table_expr
 { 
 $$ = $1;
}
|  cte_list ',' common_table_expr
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 common_table_expr:
 name opt_name_list AS select_with_parens
 { 
 $$ = cat_str(4,$1,$2,make_str("as"),$4);
}
;


 into_clause:
 INTO OptTempTableName
					{
						FoundInto = 1;
						$$= cat2_str(make_str("into"), $2);
					}
	| ecpg_into                     { $$ = EMPTY; }
| 
 { 
 $$=EMPTY; }
;


 OptTempTableName:
 TEMPORARY opt_table qualified_name
 { 
 $$ = cat_str(3,make_str("temporary"),$2,$3);
}
|  TEMP opt_table qualified_name
 { 
 $$ = cat_str(3,make_str("temp"),$2,$3);
}
|  LOCAL TEMPORARY opt_table qualified_name
 { 
 $$ = cat_str(3,make_str("local temporary"),$3,$4);
}
|  LOCAL TEMP opt_table qualified_name
 { 
 $$ = cat_str(3,make_str("local temp"),$3,$4);
}
|  GLOBAL TEMPORARY opt_table qualified_name
 { 
 $$ = cat_str(3,make_str("global temporary"),$3,$4);
}
|  GLOBAL TEMP opt_table qualified_name
 { 
 $$ = cat_str(3,make_str("global temp"),$3,$4);
}
|  TABLE qualified_name
 { 
 $$ = cat_str(2,make_str("table"),$2);
}
|  qualified_name
 { 
 $$ = $1;
}
;


 opt_table:
 TABLE
 { 
 $$ = make_str("table");
}
| 
 { 
 $$=EMPTY; }
;


 opt_all:
 ALL
 { 
 $$ = make_str("all");
}
|  DISTINCT
 { 
 $$ = make_str("distinct");
}
| 
 { 
 $$=EMPTY; }
;


 opt_distinct:
 DISTINCT
 { 
 $$ = make_str("distinct");
}
|  DISTINCT ON '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("distinct on ("),$4,make_str(")"));
}
|  ALL
 { 
 $$ = make_str("all");
}
| 
 { 
 $$=EMPTY; }
;


 opt_sort_clause:
 sort_clause
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 sort_clause:
 ORDER BY sortby_list
 { 
 $$ = cat_str(2,make_str("order by"),$3);
}
;


 sortby_list:
 sortby
 { 
 $$ = $1;
}
|  sortby_list ',' sortby
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 sortby:
 a_expr USING qual_all_Op opt_nulls_order
 { 
 $$ = cat_str(4,$1,make_str("using"),$3,$4);
}
|  a_expr opt_asc_desc opt_nulls_order
 { 
 $$ = cat_str(3,$1,$2,$3);
}
;


 select_limit:
 LIMIT select_limit_value OFFSET select_offset_value
 { 
 $$ = cat_str(4,make_str("limit"),$2,make_str("offset"),$4);
}
|  OFFSET select_offset_value LIMIT select_limit_value
 { 
 $$ = cat_str(4,make_str("offset"),$2,make_str("limit"),$4);
}
|  LIMIT select_limit_value
 { 
 $$ = cat_str(2,make_str("limit"),$2);
}
|  OFFSET select_offset_value
 { 
 $$ = cat_str(2,make_str("offset"),$2);
}
|  LIMIT select_limit_value ',' select_offset_value
        {
                mmerror(PARSE_ERROR, ET_WARNING, "no longer supported LIMIT #,# syntax passed to backend");
                $$ = cat_str(4, make_str("limit"), $2, make_str(","), $4);
        }
|  OFFSET select_offset_value2 row_or_rows
 { 
 $$ = cat_str(3,make_str("offset"),$2,$3);
}
|  FETCH first_or_next opt_select_fetch_first_value row_or_rows ONLY
 { 
 $$ = cat_str(5,make_str("fetch"),$2,$3,$4,make_str("only"));
}
|  OFFSET select_offset_value2 row_or_rows FETCH first_or_next opt_select_fetch_first_value row_or_rows ONLY
 { 
 $$ = cat_str(8,make_str("offset"),$2,$3,make_str("fetch"),$5,$6,$7,make_str("only"));
}
;


 opt_select_limit:
 select_limit
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 select_limit_value:
 a_expr
 { 
 $$ = $1;
}
|  ALL
 { 
 $$ = make_str("all");
}
;


 opt_select_fetch_first_value:
 SignedIconst
 { 
 $$ = $1;
}
|  '(' a_expr ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 select_offset_value:
 a_expr
 { 
 $$ = $1;
}
;


 select_offset_value2:
 c_expr
 { 
 $$ = $1;
}
;


 row_or_rows:
 ROW
 { 
 $$ = make_str("row");
}
|  ROWS
 { 
 $$ = make_str("rows");
}
;


 first_or_next:
 FIRST_P
 { 
 $$ = make_str("first");
}
|  NEXT
 { 
 $$ = make_str("next");
}
;


 group_clause:
 GROUP_P BY expr_list
 { 
 $$ = cat_str(2,make_str("group by"),$3);
}
| 
 { 
 $$=EMPTY; }
;


 having_clause:
 HAVING a_expr
 { 
 $$ = cat_str(2,make_str("having"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 for_locking_clause:
 for_locking_items
 { 
 $$ = $1;
}
|  FOR READ ONLY
 { 
 $$ = make_str("for read only");
}
;


 opt_for_locking_clause:
 for_locking_clause
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 for_locking_items:
 for_locking_item
 { 
 $$ = $1;
}
|  for_locking_items for_locking_item
 { 
 $$ = cat_str(2,$1,$2);
}
;


 for_locking_item:
 FOR UPDATE locked_rels_list opt_nowait
 { 
 $$ = cat_str(3,make_str("for update"),$3,$4);
}
|  FOR SHARE locked_rels_list opt_nowait
 { 
 $$ = cat_str(3,make_str("for share"),$3,$4);
}
;


 locked_rels_list:
 OF qualified_name_list
 { 
 $$ = cat_str(2,make_str("of"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 values_clause:
 VALUES ctext_row
 { 
 $$ = cat_str(2,make_str("values"),$2);
}
|  values_clause ',' ctext_row
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 from_clause:
 FROM from_list
 { 
 $$ = cat_str(2,make_str("from"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 from_list:
 table_ref
 { 
 $$ = $1;
}
|  from_list ',' table_ref
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 table_ref:
 relation_expr
 { 
 $$ = $1;
}
|  relation_expr alias_clause
 { 
 $$ = cat_str(2,$1,$2);
}
|  func_table
 { 
 $$ = $1;
}
|  func_table alias_clause
 { 
 $$ = cat_str(2,$1,$2);
}
|  func_table AS '(' TableFuncElementList ')'
 { 
 $$ = cat_str(4,$1,make_str("as ("),$4,make_str(")"));
}
|  func_table AS ColId '(' TableFuncElementList ')'
 { 
 $$ = cat_str(6,$1,make_str("as"),$3,make_str("("),$5,make_str(")"));
}
|  func_table ColId '(' TableFuncElementList ')'
 { 
 $$ = cat_str(5,$1,$2,make_str("("),$4,make_str(")"));
}
|  select_with_parens
 { 
		mmerror(PARSE_ERROR, ET_ERROR, "sub-SELECT in FROM must have an alias\n");

 $$ = $1;
}
|  select_with_parens alias_clause
 { 
 $$ = cat_str(2,$1,$2);
}
|  joined_table
 { 
 $$ = $1;
}
|  '(' joined_table ')' alias_clause
 { 
 $$ = cat_str(4,make_str("("),$2,make_str(")"),$4);
}
;


 joined_table:
 '(' joined_table ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
|  table_ref CROSS JOIN table_ref
 { 
 $$ = cat_str(3,$1,make_str("cross join"),$4);
}
|  table_ref join_type JOIN table_ref join_qual
 { 
 $$ = cat_str(5,$1,$2,make_str("join"),$4,$5);
}
|  table_ref JOIN table_ref join_qual
 { 
 $$ = cat_str(4,$1,make_str("join"),$3,$4);
}
|  table_ref NATURAL join_type JOIN table_ref
 { 
 $$ = cat_str(5,$1,make_str("natural"),$3,make_str("join"),$5);
}
|  table_ref NATURAL JOIN table_ref
 { 
 $$ = cat_str(3,$1,make_str("natural join"),$4);
}
;


 alias_clause:
 AS ColId '(' name_list ')'
 { 
 $$ = cat_str(5,make_str("as"),$2,make_str("("),$4,make_str(")"));
}
|  AS ColId
 { 
 $$ = cat_str(2,make_str("as"),$2);
}
|  ColId '(' name_list ')'
 { 
 $$ = cat_str(4,$1,make_str("("),$3,make_str(")"));
}
|  ColId
 { 
 $$ = $1;
}
;


 join_type:
 FULL join_outer
 { 
 $$ = cat_str(2,make_str("full"),$2);
}
|  LEFT join_outer
 { 
 $$ = cat_str(2,make_str("left"),$2);
}
|  RIGHT join_outer
 { 
 $$ = cat_str(2,make_str("right"),$2);
}
|  INNER_P
 { 
 $$ = make_str("inner");
}
;


 join_outer:
 OUTER_P
 { 
 $$ = make_str("outer");
}
| 
 { 
 $$=EMPTY; }
;


 join_qual:
 USING '(' name_list ')'
 { 
 $$ = cat_str(3,make_str("using ("),$3,make_str(")"));
}
|  ON a_expr
 { 
 $$ = cat_str(2,make_str("on"),$2);
}
;


 relation_expr:
 qualified_name
 { 
 $$ = $1;
}
|  qualified_name '*'
 { 
 $$ = cat_str(2,$1,make_str("*"));
}
|  ONLY qualified_name
 { 
 $$ = cat_str(2,make_str("only"),$2);
}
|  ONLY '(' qualified_name ')'
 { 
 $$ = cat_str(3,make_str("only ("),$3,make_str(")"));
}
;


 relation_expr_opt_alias:
 relation_expr %prec UMINUS
 { 
 $$ = $1;
}
|  relation_expr ColId
 { 
 $$ = cat_str(2,$1,$2);
}
|  relation_expr AS ColId
 { 
 $$ = cat_str(3,$1,make_str("as"),$3);
}
;


 func_table:
 func_expr
 { 
 $$ = $1;
}
;


 where_clause:
 WHERE a_expr
 { 
 $$ = cat_str(2,make_str("where"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 where_or_current_clause:
 WHERE a_expr
 { 
 $$ = cat_str(2,make_str("where"),$2);
}
|  WHERE CURRENT_P OF name
 { 
 $$ = cat_str(2,make_str("where current of"),$4);
}
|  WHERE CURRENT_P OF ecpg_param
 { 
 $$ = cat_str(2,make_str("where current of"),$4);
}
| 
 { 
 $$=EMPTY; }
;


 TableFuncElementList:
 TableFuncElement
 { 
 $$ = $1;
}
|  TableFuncElementList ',' TableFuncElement
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 TableFuncElement:
 ColId Typename
 { 
 $$ = cat_str(2,$1,$2);
}
;


 Typename:
 SimpleTypename opt_array_bounds
	{	$$ = cat2_str($1, $2.str); }
|  SETOF SimpleTypename opt_array_bounds
	{	$$ = $$ = cat_str(3, make_str("setof"), $2, $3.str); }
|  SimpleTypename ARRAY '[' ecpg_iconst ']'
 { 
 $$ = cat_str(4,$1,make_str("array ["),$4,make_str("]"));
}
|  SETOF SimpleTypename ARRAY '[' ecpg_iconst ']'
 { 
 $$ = cat_str(5,make_str("setof"),$2,make_str("array ["),$5,make_str("]"));
}
|  SimpleTypename ARRAY
 { 
 $$ = cat_str(2,$1,make_str("array"));
}
|  SETOF SimpleTypename ARRAY
 { 
 $$ = cat_str(3,make_str("setof"),$2,make_str("array"));
}
;


 opt_array_bounds:
 opt_array_bounds '[' ']'
	{
		$$.index1 = $1.index1;
		$$.index2 = $1.index2;
		if (strcmp($$.index1, "-1") == 0)
			$$.index1 = make_str("0");
		else if (strcmp($1.index2, "-1") == 0)
			$$.index2 = make_str("0");
		$$.str = cat_str(2, $1.str, make_str("[]"));
	}
	| opt_array_bounds '[' Iresult ']'
	{
		$$.index1 = $1.index1;
		$$.index2 = $1.index2;
		if (strcmp($1.index1, "-1") == 0)
			$$.index1 = strdup($3);
		else if (strcmp($1.index2, "-1") == 0)
			$$.index2 = strdup($3);
		$$.str = cat_str(4, $1.str, make_str("["), $3, make_str("]"));
	}
| 
	{
		$$.index1 = make_str("-1");
		$$.index2 = make_str("-1");
		$$.str= EMPTY;
	}
;


 SimpleTypename:
 GenericType
 { 
 $$ = $1;
}
|  Numeric
 { 
 $$ = $1;
}
|  Bit
 { 
 $$ = $1;
}
|  Character
 { 
 $$ = $1;
}
|  ConstDatetime
 { 
 $$ = $1;
}
|  ConstInterval opt_interval
 { 
 $$ = cat_str(2,$1,$2);
}
|  ConstInterval '(' ecpg_iconst ')' opt_interval
 { 
 $$ = cat_str(5,$1,make_str("("),$3,make_str(")"),$5);
}
;


 ConstTypename:
 Numeric
 { 
 $$ = $1;
}
|  ConstBit
 { 
 $$ = $1;
}
|  ConstCharacter
 { 
 $$ = $1;
}
|  ConstDatetime
 { 
 $$ = $1;
}
;


 GenericType:
 type_function_name opt_type_modifiers
 { 
 $$ = cat_str(2,$1,$2);
}
|  type_function_name attrs opt_type_modifiers
 { 
 $$ = cat_str(3,$1,$2,$3);
}
;


 opt_type_modifiers:
 '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 Numeric:
 INT_P
 { 
 $$ = make_str("int");
}
|  INTEGER
 { 
 $$ = make_str("integer");
}
|  SMALLINT
 { 
 $$ = make_str("smallint");
}
|  BIGINT
 { 
 $$ = make_str("bigint");
}
|  REAL
 { 
 $$ = make_str("real");
}
|  FLOAT_P opt_float
 { 
 $$ = cat_str(2,make_str("float"),$2);
}
|  DOUBLE_P PRECISION
 { 
 $$ = make_str("double precision");
}
|  DECIMAL_P opt_type_modifiers
 { 
 $$ = cat_str(2,make_str("decimal"),$2);
}
|  DEC opt_type_modifiers
 { 
 $$ = cat_str(2,make_str("dec"),$2);
}
|  NUMERIC opt_type_modifiers
 { 
 $$ = cat_str(2,make_str("numeric"),$2);
}
|  BOOLEAN_P
 { 
 $$ = make_str("boolean");
}
;


 opt_float:
 '(' ecpg_iconst ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
| 
 { 
 $$=EMPTY; }
;


 Bit:
 BitWithLength
 { 
 $$ = $1;
}
|  BitWithoutLength
 { 
 $$ = $1;
}
;


 ConstBit:
 BitWithLength
 { 
 $$ = $1;
}
|  BitWithoutLength
 { 
 $$ = $1;
}
;


 BitWithLength:
 BIT opt_varying '(' expr_list ')'
 { 
 $$ = cat_str(5,make_str("bit"),$2,make_str("("),$4,make_str(")"));
}
;


 BitWithoutLength:
 BIT opt_varying
 { 
 $$ = cat_str(2,make_str("bit"),$2);
}
;


 Character:
 CharacterWithLength
 { 
 $$ = $1;
}
|  CharacterWithoutLength
 { 
 $$ = $1;
}
;


 ConstCharacter:
 CharacterWithLength
 { 
 $$ = $1;
}
|  CharacterWithoutLength
 { 
 $$ = $1;
}
;


 CharacterWithLength:
 character '(' ecpg_iconst ')' opt_charset
 { 
 $$ = cat_str(5,$1,make_str("("),$3,make_str(")"),$5);
}
;


 CharacterWithoutLength:
 character opt_charset
 { 
 $$ = cat_str(2,$1,$2);
}
;


 character:
 CHARACTER opt_varying
 { 
 $$ = cat_str(2,make_str("character"),$2);
}
|  CHAR_P opt_varying
 { 
 $$ = cat_str(2,make_str("char"),$2);
}
|  VARCHAR
 { 
 $$ = make_str("varchar");
}
|  NATIONAL CHARACTER opt_varying
 { 
 $$ = cat_str(2,make_str("national character"),$3);
}
|  NATIONAL CHAR_P opt_varying
 { 
 $$ = cat_str(2,make_str("national char"),$3);
}
|  NCHAR opt_varying
 { 
 $$ = cat_str(2,make_str("nchar"),$2);
}
;


 opt_varying:
 VARYING
 { 
 $$ = make_str("varying");
}
| 
 { 
 $$=EMPTY; }
;


 opt_charset:
 CHARACTER SET ColId
 { 
 $$ = cat_str(2,make_str("character set"),$3);
}
| 
 { 
 $$=EMPTY; }
;


 ConstDatetime:
 TIMESTAMP '(' ecpg_iconst ')' opt_timezone
 { 
 $$ = cat_str(4,make_str("timestamp ("),$3,make_str(")"),$5);
}
|  TIMESTAMP opt_timezone
 { 
 $$ = cat_str(2,make_str("timestamp"),$2);
}
|  TIME '(' ecpg_iconst ')' opt_timezone
 { 
 $$ = cat_str(4,make_str("time ("),$3,make_str(")"),$5);
}
|  TIME opt_timezone
 { 
 $$ = cat_str(2,make_str("time"),$2);
}
;


 ConstInterval:
 INTERVAL
 { 
 $$ = make_str("interval");
}
;


 opt_timezone:
 WITH_TIME ZONE
 { 
 $$ = make_str("with time zone");
}
|  WITHOUT TIME ZONE
 { 
 $$ = make_str("without time zone");
}
| 
 { 
 $$=EMPTY; }
;


 opt_interval:
 YEAR_P
 { 
 $$ = make_str("year");
}
|  MONTH_P
 { 
 $$ = make_str("month");
}
|  DAY_P
 { 
 $$ = make_str("day");
}
|  HOUR_P
 { 
 $$ = make_str("hour");
}
|  MINUTE_P
 { 
 $$ = make_str("minute");
}
|  interval_second
 { 
 $$ = $1;
}
|  YEAR_P TO MONTH_P
 { 
 $$ = make_str("year to month");
}
|  DAY_P TO HOUR_P
 { 
 $$ = make_str("day to hour");
}
|  DAY_P TO MINUTE_P
 { 
 $$ = make_str("day to minute");
}
|  DAY_P TO interval_second
 { 
 $$ = cat_str(2,make_str("day to"),$3);
}
|  HOUR_P TO MINUTE_P
 { 
 $$ = make_str("hour to minute");
}
|  HOUR_P TO interval_second
 { 
 $$ = cat_str(2,make_str("hour to"),$3);
}
|  MINUTE_P TO interval_second
 { 
 $$ = cat_str(2,make_str("minute to"),$3);
}
| 
 { 
 $$=EMPTY; }
;


 interval_second:
 SECOND_P
 { 
 $$ = make_str("second");
}
|  SECOND_P '(' ecpg_iconst ')'
 { 
 $$ = cat_str(3,make_str("second ("),$3,make_str(")"));
}
;


 a_expr:
 c_expr
 { 
 $$ = $1;
}
|  a_expr TYPECAST Typename
 { 
 $$ = cat_str(3,$1,make_str("::"),$3);
}
|  a_expr AT TIME ZONE a_expr
 { 
 $$ = cat_str(3,$1,make_str("at time zone"),$5);
}
|  '+' a_expr %prec UMINUS
 { 
 $$ = cat_str(2,make_str("+"),$2);
}
|  '-' a_expr %prec UMINUS
 { 
 $$ = cat_str(2,make_str("-"),$2);
}
|  a_expr '+' a_expr
 { 
 $$ = cat_str(3,$1,make_str("+"),$3);
}
|  a_expr '-' a_expr
 { 
 $$ = cat_str(3,$1,make_str("-"),$3);
}
|  a_expr '*' a_expr
 { 
 $$ = cat_str(3,$1,make_str("*"),$3);
}
|  a_expr '/' a_expr
 { 
 $$ = cat_str(3,$1,make_str("/"),$3);
}
|  a_expr '%' a_expr
 { 
 $$ = cat_str(3,$1,make_str("%"),$3);
}
|  a_expr '^' a_expr
 { 
 $$ = cat_str(3,$1,make_str("^"),$3);
}
|  a_expr '<' a_expr
 { 
 $$ = cat_str(3,$1,make_str("<"),$3);
}
|  a_expr '>' a_expr
 { 
 $$ = cat_str(3,$1,make_str(">"),$3);
}
|  a_expr '=' a_expr
 { 
 $$ = cat_str(3,$1,make_str("="),$3);
}
|  a_expr qual_Op a_expr %prec Op
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  qual_Op a_expr %prec Op
 { 
 $$ = cat_str(2,$1,$2);
}
|  a_expr qual_Op %prec POSTFIXOP
 { 
 $$ = cat_str(2,$1,$2);
}
|  a_expr AND a_expr
 { 
 $$ = cat_str(3,$1,make_str("and"),$3);
}
|  a_expr OR a_expr
 { 
 $$ = cat_str(3,$1,make_str("or"),$3);
}
|  NOT a_expr
 { 
 $$ = cat_str(2,make_str("not"),$2);
}
|  a_expr LIKE a_expr
 { 
 $$ = cat_str(3,$1,make_str("like"),$3);
}
|  a_expr LIKE a_expr ESCAPE a_expr
 { 
 $$ = cat_str(5,$1,make_str("like"),$3,make_str("escape"),$5);
}
|  a_expr NOT LIKE a_expr
 { 
 $$ = cat_str(3,$1,make_str("not like"),$4);
}
|  a_expr NOT LIKE a_expr ESCAPE a_expr
 { 
 $$ = cat_str(5,$1,make_str("not like"),$4,make_str("escape"),$6);
}
|  a_expr ILIKE a_expr
 { 
 $$ = cat_str(3,$1,make_str("ilike"),$3);
}
|  a_expr ILIKE a_expr ESCAPE a_expr
 { 
 $$ = cat_str(5,$1,make_str("ilike"),$3,make_str("escape"),$5);
}
|  a_expr NOT ILIKE a_expr
 { 
 $$ = cat_str(3,$1,make_str("not ilike"),$4);
}
|  a_expr NOT ILIKE a_expr ESCAPE a_expr
 { 
 $$ = cat_str(5,$1,make_str("not ilike"),$4,make_str("escape"),$6);
}
|  a_expr SIMILAR TO a_expr %prec SIMILAR
 { 
 $$ = cat_str(3,$1,make_str("similar to"),$4);
}
|  a_expr SIMILAR TO a_expr ESCAPE a_expr
 { 
 $$ = cat_str(5,$1,make_str("similar to"),$4,make_str("escape"),$6);
}
|  a_expr NOT SIMILAR TO a_expr %prec SIMILAR
 { 
 $$ = cat_str(3,$1,make_str("not similar to"),$5);
}
|  a_expr NOT SIMILAR TO a_expr ESCAPE a_expr
 { 
 $$ = cat_str(5,$1,make_str("not similar to"),$5,make_str("escape"),$7);
}
|  a_expr IS NULL_P
 { 
 $$ = cat_str(2,$1,make_str("is null"));
}
|  a_expr ISNULL
 { 
 $$ = cat_str(2,$1,make_str("isnull"));
}
|  a_expr IS NOT NULL_P
 { 
 $$ = cat_str(2,$1,make_str("is not null"));
}
|  a_expr NOTNULL
 { 
 $$ = cat_str(2,$1,make_str("notnull"));
}
|  row OVERLAPS row
 { 
 $$ = cat_str(3,$1,make_str("overlaps"),$3);
}
|  a_expr IS TRUE_P
 { 
 $$ = cat_str(2,$1,make_str("is true"));
}
|  a_expr IS NOT TRUE_P
 { 
 $$ = cat_str(2,$1,make_str("is not true"));
}
|  a_expr IS FALSE_P
 { 
 $$ = cat_str(2,$1,make_str("is false"));
}
|  a_expr IS NOT FALSE_P
 { 
 $$ = cat_str(2,$1,make_str("is not false"));
}
|  a_expr IS UNKNOWN
 { 
 $$ = cat_str(2,$1,make_str("is unknown"));
}
|  a_expr IS NOT UNKNOWN
 { 
 $$ = cat_str(2,$1,make_str("is not unknown"));
}
|  a_expr IS DISTINCT FROM a_expr %prec IS
 { 
 $$ = cat_str(3,$1,make_str("is distinct from"),$5);
}
|  a_expr IS NOT DISTINCT FROM a_expr %prec IS
 { 
 $$ = cat_str(3,$1,make_str("is not distinct from"),$6);
}
|  a_expr IS OF '(' type_list ')' %prec IS
 { 
 $$ = cat_str(4,$1,make_str("is of ("),$5,make_str(")"));
}
|  a_expr IS NOT OF '(' type_list ')' %prec IS
 { 
 $$ = cat_str(4,$1,make_str("is not of ("),$6,make_str(")"));
}
|  a_expr BETWEEN opt_asymmetric b_expr AND b_expr %prec BETWEEN
 { 
 $$ = cat_str(6,$1,make_str("between"),$3,$4,make_str("and"),$6);
}
|  a_expr NOT BETWEEN opt_asymmetric b_expr AND b_expr %prec BETWEEN
 { 
 $$ = cat_str(6,$1,make_str("not between"),$4,$5,make_str("and"),$7);
}
|  a_expr BETWEEN SYMMETRIC b_expr AND b_expr %prec BETWEEN
 { 
 $$ = cat_str(5,$1,make_str("between symmetric"),$4,make_str("and"),$6);
}
|  a_expr NOT BETWEEN SYMMETRIC b_expr AND b_expr %prec BETWEEN
 { 
 $$ = cat_str(5,$1,make_str("not between symmetric"),$5,make_str("and"),$7);
}
|  a_expr IN_P in_expr
 { 
 $$ = cat_str(3,$1,make_str("in"),$3);
}
|  a_expr NOT IN_P in_expr
 { 
 $$ = cat_str(3,$1,make_str("not in"),$4);
}
|  a_expr subquery_Op sub_type select_with_parens %prec Op
 { 
 $$ = cat_str(4,$1,$2,$3,$4);
}
|  a_expr subquery_Op sub_type '(' a_expr ')' %prec Op
 { 
 $$ = cat_str(6,$1,$2,$3,make_str("("),$5,make_str(")"));
}
|  UNIQUE select_with_parens
 { 
mmerror(PARSE_ERROR, ET_WARNING, "unsupported feature will be passed to backend\n");
 $$ = cat_str(2,make_str("unique"),$2);
}
|  a_expr IS DOCUMENT_P %prec IS
 { 
 $$ = cat_str(2,$1,make_str("is document"));
}
|  a_expr IS NOT DOCUMENT_P %prec IS
 { 
 $$ = cat_str(2,$1,make_str("is not document"));
}
;


 b_expr:
 c_expr
 { 
 $$ = $1;
}
|  b_expr TYPECAST Typename
 { 
 $$ = cat_str(3,$1,make_str("::"),$3);
}
|  '+' b_expr %prec UMINUS
 { 
 $$ = cat_str(2,make_str("+"),$2);
}
|  '-' b_expr %prec UMINUS
 { 
 $$ = cat_str(2,make_str("-"),$2);
}
|  b_expr '+' b_expr
 { 
 $$ = cat_str(3,$1,make_str("+"),$3);
}
|  b_expr '-' b_expr
 { 
 $$ = cat_str(3,$1,make_str("-"),$3);
}
|  b_expr '*' b_expr
 { 
 $$ = cat_str(3,$1,make_str("*"),$3);
}
|  b_expr '/' b_expr
 { 
 $$ = cat_str(3,$1,make_str("/"),$3);
}
|  b_expr '%' b_expr
 { 
 $$ = cat_str(3,$1,make_str("%"),$3);
}
|  b_expr '^' b_expr
 { 
 $$ = cat_str(3,$1,make_str("^"),$3);
}
|  b_expr '<' b_expr
 { 
 $$ = cat_str(3,$1,make_str("<"),$3);
}
|  b_expr '>' b_expr
 { 
 $$ = cat_str(3,$1,make_str(">"),$3);
}
|  b_expr '=' b_expr
 { 
 $$ = cat_str(3,$1,make_str("="),$3);
}
|  b_expr qual_Op b_expr %prec Op
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  qual_Op b_expr %prec Op
 { 
 $$ = cat_str(2,$1,$2);
}
|  b_expr qual_Op %prec POSTFIXOP
 { 
 $$ = cat_str(2,$1,$2);
}
|  b_expr IS DISTINCT FROM b_expr %prec IS
 { 
 $$ = cat_str(3,$1,make_str("is distinct from"),$5);
}
|  b_expr IS NOT DISTINCT FROM b_expr %prec IS
 { 
 $$ = cat_str(3,$1,make_str("is not distinct from"),$6);
}
|  b_expr IS OF '(' type_list ')' %prec IS
 { 
 $$ = cat_str(4,$1,make_str("is of ("),$5,make_str(")"));
}
|  b_expr IS NOT OF '(' type_list ')' %prec IS
 { 
 $$ = cat_str(4,$1,make_str("is not of ("),$6,make_str(")"));
}
|  b_expr IS DOCUMENT_P %prec IS
 { 
 $$ = cat_str(2,$1,make_str("is document"));
}
|  b_expr IS NOT DOCUMENT_P %prec IS
 { 
 $$ = cat_str(2,$1,make_str("is not document"));
}
;


 c_expr:
 columnref
 { 
 $$ = $1;
}
|  AexprConst
 { 
 $$ = $1;
}
|  ecpg_param opt_indirection
 { 
 $$ = cat_str(2,$1,$2);
}
|  '(' a_expr ')' opt_indirection
 { 
 $$ = cat_str(4,make_str("("),$2,make_str(")"),$4);
}
|  case_expr
 { 
 $$ = $1;
}
|  func_expr
 { 
 $$ = $1;
}
|  select_with_parens %prec UMINUS
 { 
 $$ = $1;
}
|  EXISTS select_with_parens
 { 
 $$ = cat_str(2,make_str("exists"),$2);
}
|  ARRAY select_with_parens
 { 
 $$ = cat_str(2,make_str("array"),$2);
}
|  ARRAY array_expr
 { 
 $$ = cat_str(2,make_str("array"),$2);
}
|  row
 { 
 $$ = $1;
}
;


 func_expr:
 func_name '(' ')'
 { 
 $$ = cat_str(2,$1,make_str("( )"));
}
|  func_name '(' expr_list ')'
 { 
 $$ = cat_str(4,$1,make_str("("),$3,make_str(")"));
}
|  func_name '(' VARIADIC a_expr ')'
 { 
 $$ = cat_str(4,$1,make_str("( variadic"),$4,make_str(")"));
}
|  func_name '(' expr_list ',' VARIADIC a_expr ')'
 { 
 $$ = cat_str(6,$1,make_str("("),$3,make_str(", variadic"),$6,make_str(")"));
}
|  func_name '(' ALL expr_list ')'
 { 
 $$ = cat_str(4,$1,make_str("( all"),$4,make_str(")"));
}
|  func_name '(' DISTINCT expr_list ')'
 { 
 $$ = cat_str(4,$1,make_str("( distinct"),$4,make_str(")"));
}
|  func_name '(' '*' ')'
 { 
 $$ = cat_str(2,$1,make_str("( * )"));
}
|  CURRENT_DATE
 { 
 $$ = make_str("current_date");
}
|  CURRENT_TIME
 { 
 $$ = make_str("current_time");
}
|  CURRENT_TIME '(' ecpg_iconst ')'
 { 
 $$ = cat_str(3,make_str("current_time ("),$3,make_str(")"));
}
|  CURRENT_TIMESTAMP
 { 
 $$ = make_str("current_timestamp");
}
|  CURRENT_TIMESTAMP '(' ecpg_iconst ')'
 { 
 $$ = cat_str(3,make_str("current_timestamp ("),$3,make_str(")"));
}
|  LOCALTIME
 { 
 $$ = make_str("localtime");
}
|  LOCALTIME '(' ecpg_iconst ')'
 { 
 $$ = cat_str(3,make_str("localtime ("),$3,make_str(")"));
}
|  LOCALTIMESTAMP
 { 
 $$ = make_str("localtimestamp");
}
|  LOCALTIMESTAMP '(' ecpg_iconst ')'
 { 
 $$ = cat_str(3,make_str("localtimestamp ("),$3,make_str(")"));
}
|  CURRENT_ROLE
 { 
 $$ = make_str("current_role");
}
|  CURRENT_USER
 { 
 $$ = make_str("current_user");
}
|  SESSION_USER
 { 
 $$ = make_str("session_user");
}
|  USER
 { 
 $$ = make_str("user");
}
|  CURRENT_CATALOG
 { 
 $$ = make_str("current_catalog");
}
|  CURRENT_SCHEMA
 { 
 $$ = make_str("current_schema");
}
|  CAST '(' a_expr AS Typename ')'
 { 
 $$ = cat_str(5,make_str("cast ("),$3,make_str("as"),$5,make_str(")"));
}
|  EXTRACT '(' extract_list ')'
 { 
 $$ = cat_str(3,make_str("extract ("),$3,make_str(")"));
}
|  OVERLAY '(' overlay_list ')'
 { 
 $$ = cat_str(3,make_str("overlay ("),$3,make_str(")"));
}
|  POSITION '(' position_list ')'
 { 
 $$ = cat_str(3,make_str("position ("),$3,make_str(")"));
}
|  SUBSTRING '(' substr_list ')'
 { 
 $$ = cat_str(3,make_str("substring ("),$3,make_str(")"));
}
|  TREAT '(' a_expr AS Typename ')'
 { 
 $$ = cat_str(5,make_str("treat ("),$3,make_str("as"),$5,make_str(")"));
}
|  TRIM '(' BOTH trim_list ')'
 { 
 $$ = cat_str(3,make_str("trim ( both"),$4,make_str(")"));
}
|  TRIM '(' LEADING trim_list ')'
 { 
 $$ = cat_str(3,make_str("trim ( leading"),$4,make_str(")"));
}
|  TRIM '(' TRAILING trim_list ')'
 { 
 $$ = cat_str(3,make_str("trim ( trailing"),$4,make_str(")"));
}
|  TRIM '(' trim_list ')'
 { 
 $$ = cat_str(3,make_str("trim ("),$3,make_str(")"));
}
|  NULLIF '(' a_expr ',' a_expr ')'
 { 
 $$ = cat_str(5,make_str("nullif ("),$3,make_str(","),$5,make_str(")"));
}
|  COALESCE '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("coalesce ("),$3,make_str(")"));
}
|  GREATEST '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("greatest ("),$3,make_str(")"));
}
|  LEAST '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("least ("),$3,make_str(")"));
}
|  XMLCONCAT '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("xmlconcat ("),$3,make_str(")"));
}
|  XMLELEMENT '(' NAME_P ColLabel ')'
 { 
 $$ = cat_str(3,make_str("xmlelement ( name"),$4,make_str(")"));
}
|  XMLELEMENT '(' NAME_P ColLabel ',' xml_attributes ')'
 { 
 $$ = cat_str(5,make_str("xmlelement ( name"),$4,make_str(","),$6,make_str(")"));
}
|  XMLELEMENT '(' NAME_P ColLabel ',' expr_list ')'
 { 
 $$ = cat_str(5,make_str("xmlelement ( name"),$4,make_str(","),$6,make_str(")"));
}
|  XMLELEMENT '(' NAME_P ColLabel ',' xml_attributes ',' expr_list ')'
 { 
 $$ = cat_str(7,make_str("xmlelement ( name"),$4,make_str(","),$6,make_str(","),$8,make_str(")"));
}
|  XMLFOREST '(' xml_attribute_list ')'
 { 
 $$ = cat_str(3,make_str("xmlforest ("),$3,make_str(")"));
}
|  XMLPARSE '(' document_or_content a_expr xml_whitespace_option ')'
 { 
 $$ = cat_str(5,make_str("xmlparse ("),$3,$4,$5,make_str(")"));
}
|  XMLPI '(' NAME_P ColLabel ')'
 { 
 $$ = cat_str(3,make_str("xmlpi ( name"),$4,make_str(")"));
}
|  XMLPI '(' NAME_P ColLabel ',' a_expr ')'
 { 
 $$ = cat_str(5,make_str("xmlpi ( name"),$4,make_str(","),$6,make_str(")"));
}
|  XMLROOT '(' a_expr ',' xml_root_version opt_xml_root_standalone ')'
 { 
 $$ = cat_str(6,make_str("xmlroot ("),$3,make_str(","),$5,$6,make_str(")"));
}
|  XMLSERIALIZE '(' document_or_content a_expr AS SimpleTypename ')'
 { 
 $$ = cat_str(6,make_str("xmlserialize ("),$3,$4,make_str("as"),$6,make_str(")"));
}
;


 xml_root_version:
 VERSION_P a_expr
 { 
 $$ = cat_str(2,make_str("version"),$2);
}
|  VERSION_P NO VALUE_P
 { 
 $$ = make_str("version no value");
}
;


 opt_xml_root_standalone:
 ',' STANDALONE_P YES_P
 { 
 $$ = make_str(", standalone yes");
}
|  ',' STANDALONE_P NO
 { 
 $$ = make_str(", standalone no");
}
|  ',' STANDALONE_P NO VALUE_P
 { 
 $$ = make_str(", standalone no value");
}
| 
 { 
 $$=EMPTY; }
;


 xml_attributes:
 XMLATTRIBUTES '(' xml_attribute_list ')'
 { 
 $$ = cat_str(3,make_str("xmlattributes ("),$3,make_str(")"));
}
;


 xml_attribute_list:
 xml_attribute_el
 { 
 $$ = $1;
}
|  xml_attribute_list ',' xml_attribute_el
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 xml_attribute_el:
 a_expr AS ColLabel
 { 
 $$ = cat_str(3,$1,make_str("as"),$3);
}
|  a_expr
 { 
 $$ = $1;
}
;


 document_or_content:
 DOCUMENT_P
 { 
 $$ = make_str("document");
}
|  CONTENT_P
 { 
 $$ = make_str("content");
}
;


 xml_whitespace_option:
 PRESERVE WHITESPACE_P
 { 
 $$ = make_str("preserve whitespace");
}
|  STRIP_P WHITESPACE_P
 { 
 $$ = make_str("strip whitespace");
}
| 
 { 
 $$=EMPTY; }
;


 row:
 ROW '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("row ("),$3,make_str(")"));
}
|  ROW '(' ')'
 { 
 $$ = make_str("row ( )");
}
|  '(' expr_list ',' a_expr ')'
 { 
 $$ = cat_str(5,make_str("("),$2,make_str(","),$4,make_str(")"));
}
;


 sub_type:
 ANY
 { 
 $$ = make_str("any");
}
|  SOME
 { 
 $$ = make_str("some");
}
|  ALL
 { 
 $$ = make_str("all");
}
;


 all_Op:
 Op
 { 
 $$ = $1;
}
|  MathOp
 { 
 $$ = $1;
}
;


 MathOp:
 '+'
 { 
 $$ = make_str("+");
}
|  '-'
 { 
 $$ = make_str("-");
}
|  '*'
 { 
 $$ = make_str("*");
}
|  '/'
 { 
 $$ = make_str("/");
}
|  '%'
 { 
 $$ = make_str("%");
}
|  '^'
 { 
 $$ = make_str("^");
}
|  '<'
 { 
 $$ = make_str("<");
}
|  '>'
 { 
 $$ = make_str(">");
}
|  '='
 { 
 $$ = make_str("=");
}
;


 qual_Op:
 Op
 { 
 $$ = $1;
}
|  OPERATOR '(' any_operator ')'
 { 
 $$ = cat_str(3,make_str("operator ("),$3,make_str(")"));
}
;


 qual_all_Op:
 all_Op
 { 
 $$ = $1;
}
|  OPERATOR '(' any_operator ')'
 { 
 $$ = cat_str(3,make_str("operator ("),$3,make_str(")"));
}
;


 subquery_Op:
 all_Op
 { 
 $$ = $1;
}
|  OPERATOR '(' any_operator ')'
 { 
 $$ = cat_str(3,make_str("operator ("),$3,make_str(")"));
}
|  LIKE
 { 
 $$ = make_str("like");
}
|  NOT LIKE
 { 
 $$ = make_str("not like");
}
|  ILIKE
 { 
 $$ = make_str("ilike");
}
|  NOT ILIKE
 { 
 $$ = make_str("not ilike");
}
;


 expr_list:
 a_expr
 { 
 $$ = $1;
}
|  expr_list ',' a_expr
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 type_list:
 Typename
 { 
 $$ = $1;
}
|  type_list ',' Typename
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 array_expr:
 '[' expr_list ']'
 { 
 $$ = cat_str(3,make_str("["),$2,make_str("]"));
}
|  '[' array_expr_list ']'
 { 
 $$ = cat_str(3,make_str("["),$2,make_str("]"));
}
|  '[' ']'
 { 
 $$ = make_str("[ ]");
}
;


 array_expr_list:
 array_expr
 { 
 $$ = $1;
}
|  array_expr_list ',' array_expr
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 extract_list:
 extract_arg FROM a_expr
 { 
 $$ = cat_str(3,$1,make_str("from"),$3);
}
| 
 { 
 $$=EMPTY; }
;


 extract_arg:
 ecpg_ident
 { 
 $$ = $1;
}
|  YEAR_P
 { 
 $$ = make_str("year");
}
|  MONTH_P
 { 
 $$ = make_str("month");
}
|  DAY_P
 { 
 $$ = make_str("day");
}
|  HOUR_P
 { 
 $$ = make_str("hour");
}
|  MINUTE_P
 { 
 $$ = make_str("minute");
}
|  SECOND_P
 { 
 $$ = make_str("second");
}
|  ecpg_sconst
 { 
 $$ = $1;
}
;


 overlay_list:
 a_expr overlay_placing substr_from substr_for
 { 
 $$ = cat_str(4,$1,$2,$3,$4);
}
|  a_expr overlay_placing substr_from
 { 
 $$ = cat_str(3,$1,$2,$3);
}
;


 overlay_placing:
 PLACING a_expr
 { 
 $$ = cat_str(2,make_str("placing"),$2);
}
;


 position_list:
 b_expr IN_P b_expr
 { 
 $$ = cat_str(3,$1,make_str("in"),$3);
}
| 
 { 
 $$=EMPTY; }
;


 substr_list:
 a_expr substr_from substr_for
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  a_expr substr_for substr_from
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  a_expr substr_from
 { 
 $$ = cat_str(2,$1,$2);
}
|  a_expr substr_for
 { 
 $$ = cat_str(2,$1,$2);
}
|  expr_list
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 substr_from:
 FROM a_expr
 { 
 $$ = cat_str(2,make_str("from"),$2);
}
;


 substr_for:
 FOR a_expr
 { 
 $$ = cat_str(2,make_str("for"),$2);
}
;


 trim_list:
 a_expr FROM expr_list
 { 
 $$ = cat_str(3,$1,make_str("from"),$3);
}
|  FROM expr_list
 { 
 $$ = cat_str(2,make_str("from"),$2);
}
|  expr_list
 { 
 $$ = $1;
}
;


 in_expr:
 select_with_parens
 { 
 $$ = $1;
}
|  '(' expr_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
;


 case_expr:
 CASE case_arg when_clause_list case_default END_P
 { 
 $$ = cat_str(5,make_str("case"),$2,$3,$4,make_str("end"));
}
;


 when_clause_list:
 when_clause
 { 
 $$ = $1;
}
|  when_clause_list when_clause
 { 
 $$ = cat_str(2,$1,$2);
}
;


 when_clause:
 WHEN a_expr THEN a_expr
 { 
 $$ = cat_str(4,make_str("when"),$2,make_str("then"),$4);
}
;


 case_default:
 ELSE a_expr
 { 
 $$ = cat_str(2,make_str("else"),$2);
}
| 
 { 
 $$=EMPTY; }
;


 case_arg:
 a_expr
 { 
 $$ = $1;
}
| 
 { 
 $$=EMPTY; }
;


 columnref:
 relation_name
 { 
 $$ = $1;
}
|  relation_name indirection
 { 
 $$ = cat_str(2,$1,$2);
}
;


 indirection_el:
 '.' attr_name
 { 
 $$ = cat_str(2,make_str("."),$2);
}
|  '.' '*'
 { 
 $$ = make_str(". *");
}
|  '[' a_expr ']'
 { 
 $$ = cat_str(3,make_str("["),$2,make_str("]"));
}
|  '[' a_expr ':' a_expr ']'
 { 
 $$ = cat_str(5,make_str("["),$2,make_str(":"),$4,make_str("]"));
}
;


 indirection:
 indirection_el
 { 
 $$ = $1;
}
|  indirection indirection_el
 { 
 $$ = cat_str(2,$1,$2);
}
;


 opt_indirection:

 { 
 $$=EMPTY; }
|  opt_indirection indirection_el
 { 
 $$ = cat_str(2,$1,$2);
}
;


 opt_asymmetric:
 ASYMMETRIC
 { 
 $$ = make_str("asymmetric");
}
| 
 { 
 $$=EMPTY; }
;


 ctext_expr:
 a_expr
 { 
 $$ = $1;
}
|  DEFAULT
 { 
 $$ = make_str("default");
}
;


 ctext_expr_list:
 ctext_expr
 { 
 $$ = $1;
}
|  ctext_expr_list ',' ctext_expr
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 ctext_row:
 '(' ctext_expr_list ')'
 { 
 $$ = cat_str(3,make_str("("),$2,make_str(")"));
}
;


 target_list:
 target_el
 { 
 $$ = $1;
}
|  target_list ',' target_el
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 target_el:
 a_expr AS ColLabel
 { 
 $$ = cat_str(3,$1,make_str("as"),$3);
}
|  a_expr ecpg_ident
 { 
 $$ = cat_str(2,$1,$2);
}
|  a_expr
 { 
 $$ = $1;
}
|  '*'
 { 
 $$ = make_str("*");
}
;


 relation_name:
 SpecialRuleRelation
 { 
 $$ = $1;
}
|  ColId
 { 
 $$ = $1;
}
;


 qualified_name_list:
 qualified_name
 { 
 $$ = $1;
}
|  qualified_name_list ',' qualified_name
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 qualified_name:
 relation_name
 { 
 $$ = $1;
}
|  relation_name indirection
 { 
 $$ = cat_str(2,$1,$2);
}
;


 name_list:
 name
 { 
 $$ = $1;
}
|  name_list ',' name
 { 
 $$ = cat_str(3,$1,make_str(","),$3);
}
;


 name:
 ColId
 { 
 $$ = $1;
}
;


 database_name:
 ColId
 { 
 $$ = $1;
}
;


 access_method:
 ColId
 { 
 $$ = $1;
}
;


 attr_name:
 ColLabel
 { 
 $$ = $1;
}
;


 index_name:
 ColId
 { 
 $$ = $1;
}
;


 file_name:
 ecpg_sconst
 { 
 $$ = $1;
}
;


 func_name:
 type_function_name
 { 
 $$ = $1;
}
|  relation_name indirection
 { 
 $$ = cat_str(2,$1,$2);
}
;


 AexprConst:
 ecpg_iconst
 { 
 $$ = $1;
}
|  ecpg_fconst
 { 
 $$ = $1;
}
| ecpg_real_sconst
 { 
 $$ = $1;
}
|  ecpg_bconst
 { 
 $$ = $1;
}
|  XCONST
 { 
 $$ = make_str("xconst");
}
|  func_name ecpg_sconst
 { 
 $$ = cat_str(2,$1,$2);
}
|  func_name '(' expr_list ')' ecpg_sconst
 { 
 $$ = cat_str(5,$1,make_str("("),$3,make_str(")"),$5);
}
|  ConstTypename ecpg_sconst
 { 
 $$ = cat_str(2,$1,$2);
}
|  ConstInterval ecpg_sconst opt_interval
 { 
 $$ = cat_str(3,$1,$2,$3);
}
|  ConstInterval '(' ecpg_iconst ')' ecpg_sconst opt_interval
 { 
 $$ = cat_str(6,$1,make_str("("),$3,make_str(")"),$5,$6);
}
|  TRUE_P
 { 
 $$ = make_str("true");
}
|  FALSE_P
 { 
 $$ = make_str("false");
}
|  NULL_P
 { 
 $$ = make_str("null");
}
	| civarind		{ $$ = $1; }
;


 RoleId:
 ColId
 { 
 $$ = $1;
}
;


 SignedIconst:
 ecpg_iconst
 { 
 $$ = $1;
}
	| '+' ecpg_real_iconst	{ $$ = cat_str(2, make_str("+"), $2); }
	| '-' ecpg_real_iconst	{ $$ = cat_str(2, make_str("-"), $2); }
;


 ColId:
 ecpg_ident
 { 
 $$ = $1;
}
|  unreserved_keyword
 { 
 $$ = $1;
}
|  col_name_keyword
 { 
 $$ = $1;
}
	| ECPGKeywords                  { $$ = $1; }
	| ECPGCKeywords                 { $$ = $1; }
	| CHAR_P                        { $$ = make_str("char"); }
	| VALUES                        { $$ = make_str("values"); }
;


 type_function_name:
 ecpg_ident
 { 
 $$ = $1;
}
|  unreserved_keyword
 { 
 $$ = $1;
}
|  type_func_name_keyword
 { 
 $$ = $1;
}
	| ECPGKeywords                          { $$ = $1; }
	| ECPGTypeName                          { $$ = $1; }
	| ECPGCKeywords                         { $$ = $1; }
;


 col_name_keyword:
 BIGINT
 { 
 $$ = make_str("bigint");
}
|  BIT
 { 
 $$ = make_str("bit");
}
|  BOOLEAN_P
 { 
 $$ = make_str("boolean");
}
|  CHARACTER
 { 
 $$ = make_str("character");
}
|  COALESCE
 { 
 $$ = make_str("coalesce");
}
|  DEC
 { 
 $$ = make_str("dec");
}
|  DECIMAL_P
 { 
 $$ = make_str("decimal");
}
|  EXISTS
 { 
 $$ = make_str("exists");
}
|  EXTRACT
 { 
 $$ = make_str("extract");
}
|  FLOAT_P
 { 
 $$ = make_str("float");
}
|  GREATEST
 { 
 $$ = make_str("greatest");
}
|  INOUT
 { 
 $$ = make_str("inout");
}
|  INTEGER
 { 
 $$ = make_str("integer");
}
|  INTERVAL
 { 
 $$ = make_str("interval");
}
|  LEAST
 { 
 $$ = make_str("least");
}
|  NATIONAL
 { 
 $$ = make_str("national");
}
|  NCHAR
 { 
 $$ = make_str("nchar");
}
|  NONE
 { 
 $$ = make_str("none");
}
|  NULLIF
 { 
 $$ = make_str("nullif");
}
|  NUMERIC
 { 
 $$ = make_str("numeric");
}
|  OUT_P
 { 
 $$ = make_str("out");
}
|  OVERLAY
 { 
 $$ = make_str("overlay");
}
|  POSITION
 { 
 $$ = make_str("position");
}
|  PRECISION
 { 
 $$ = make_str("precision");
}
|  REAL
 { 
 $$ = make_str("real");
}
|  ROW
 { 
 $$ = make_str("row");
}
|  SETOF
 { 
 $$ = make_str("setof");
}
|  SMALLINT
 { 
 $$ = make_str("smallint");
}
|  SUBSTRING
 { 
 $$ = make_str("substring");
}
|  TIME
 { 
 $$ = make_str("time");
}
|  TIMESTAMP
 { 
 $$ = make_str("timestamp");
}
|  TREAT
 { 
 $$ = make_str("treat");
}
|  TRIM
 { 
 $$ = make_str("trim");
}
|  VARCHAR
 { 
 $$ = make_str("varchar");
}
|  XMLATTRIBUTES
 { 
 $$ = make_str("xmlattributes");
}
|  XMLCONCAT
 { 
 $$ = make_str("xmlconcat");
}
|  XMLELEMENT
 { 
 $$ = make_str("xmlelement");
}
|  XMLFOREST
 { 
 $$ = make_str("xmlforest");
}
|  XMLPARSE
 { 
 $$ = make_str("xmlparse");
}
|  XMLPI
 { 
 $$ = make_str("xmlpi");
}
|  XMLROOT
 { 
 $$ = make_str("xmlroot");
}
|  XMLSERIALIZE
 { 
 $$ = make_str("xmlserialize");
}
;


 type_func_name_keyword:
 AUTHORIZATION
 { 
 $$ = make_str("authorization");
}
|  BETWEEN
 { 
 $$ = make_str("between");
}
|  BINARY
 { 
 $$ = make_str("binary");
}
|  CROSS
 { 
 $$ = make_str("cross");
}
|  CURRENT_SCHEMA
 { 
 $$ = make_str("current_schema");
}
|  FREEZE
 { 
 $$ = make_str("freeze");
}
|  FULL
 { 
 $$ = make_str("full");
}
|  ILIKE
 { 
 $$ = make_str("ilike");
}
|  INNER_P
 { 
 $$ = make_str("inner");
}
|  IS
 { 
 $$ = make_str("is");
}
|  ISNULL
 { 
 $$ = make_str("isnull");
}
|  JOIN
 { 
 $$ = make_str("join");
}
|  LEFT
 { 
 $$ = make_str("left");
}
|  LIKE
 { 
 $$ = make_str("like");
}
|  NATURAL
 { 
 $$ = make_str("natural");
}
|  NOTNULL
 { 
 $$ = make_str("notnull");
}
|  OUTER_P
 { 
 $$ = make_str("outer");
}
|  OVERLAPS
 { 
 $$ = make_str("overlaps");
}
|  RIGHT
 { 
 $$ = make_str("right");
}
|  SIMILAR
 { 
 $$ = make_str("similar");
}
|  VERBOSE
 { 
 $$ = make_str("verbose");
}
;


 reserved_keyword:
 ALL
 { 
 $$ = make_str("all");
}
|  ANALYSE
 { 
 $$ = make_str("analyse");
}
|  ANALYZE
 { 
 $$ = make_str("analyze");
}
|  AND
 { 
 $$ = make_str("and");
}
|  ANY
 { 
 $$ = make_str("any");
}
|  ARRAY
 { 
 $$ = make_str("array");
}
|  AS
 { 
 $$ = make_str("as");
}
|  ASC
 { 
 $$ = make_str("asc");
}
|  ASYMMETRIC
 { 
 $$ = make_str("asymmetric");
}
|  BOTH
 { 
 $$ = make_str("both");
}
|  CASE
 { 
 $$ = make_str("case");
}
|  CAST
 { 
 $$ = make_str("cast");
}
|  CHECK
 { 
 $$ = make_str("check");
}
|  COLLATE
 { 
 $$ = make_str("collate");
}
|  COLUMN
 { 
 $$ = make_str("column");
}
|  CONSTRAINT
 { 
 $$ = make_str("constraint");
}
|  CREATE
 { 
 $$ = make_str("create");
}
|  CURRENT_CATALOG
 { 
 $$ = make_str("current_catalog");
}
|  CURRENT_DATE
 { 
 $$ = make_str("current_date");
}
|  CURRENT_ROLE
 { 
 $$ = make_str("current_role");
}
|  CURRENT_TIME
 { 
 $$ = make_str("current_time");
}
|  CURRENT_TIMESTAMP
 { 
 $$ = make_str("current_timestamp");
}
|  CURRENT_USER
 { 
 $$ = make_str("current_user");
}
|  DEFAULT
 { 
 $$ = make_str("default");
}
|  DEFERRABLE
 { 
 $$ = make_str("deferrable");
}
|  DESC
 { 
 $$ = make_str("desc");
}
|  DISTINCT
 { 
 $$ = make_str("distinct");
}
|  DO
 { 
 $$ = make_str("do");
}
|  ELSE
 { 
 $$ = make_str("else");
}
|  END_P
 { 
 $$ = make_str("end");
}
|  EXCEPT
 { 
 $$ = make_str("except");
}
|  FALSE_P
 { 
 $$ = make_str("false");
}
|  FETCH
 { 
 $$ = make_str("fetch");
}
|  FOR
 { 
 $$ = make_str("for");
}
|  FOREIGN
 { 
 $$ = make_str("foreign");
}
|  FROM
 { 
 $$ = make_str("from");
}
|  GRANT
 { 
 $$ = make_str("grant");
}
|  GROUP_P
 { 
 $$ = make_str("group");
}
|  HAVING
 { 
 $$ = make_str("having");
}
|  IN_P
 { 
 $$ = make_str("in");
}
|  INITIALLY
 { 
 $$ = make_str("initially");
}
|  INTERSECT
 { 
 $$ = make_str("intersect");
}
|  INTO
 { 
 $$ = make_str("into");
}
|  LEADING
 { 
 $$ = make_str("leading");
}
|  LIMIT
 { 
 $$ = make_str("limit");
}
|  LOCALTIME
 { 
 $$ = make_str("localtime");
}
|  LOCALTIMESTAMP
 { 
 $$ = make_str("localtimestamp");
}
|  NEW
 { 
 $$ = make_str("new");
}
|  NOT
 { 
 $$ = make_str("not");
}
|  NULL_P
 { 
 $$ = make_str("null");
}
|  OFF
 { 
 $$ = make_str("off");
}
|  OFFSET
 { 
 $$ = make_str("offset");
}
|  OLD
 { 
 $$ = make_str("old");
}
|  ON
 { 
 $$ = make_str("on");
}
|  ONLY
 { 
 $$ = make_str("only");
}
|  OR
 { 
 $$ = make_str("or");
}
|  ORDER
 { 
 $$ = make_str("order");
}
|  PLACING
 { 
 $$ = make_str("placing");
}
|  PRIMARY
 { 
 $$ = make_str("primary");
}
|  REFERENCES
 { 
 $$ = make_str("references");
}
|  RETURNING
 { 
 $$ = make_str("returning");
}
|  SELECT
 { 
 $$ = make_str("select");
}
|  SESSION_USER
 { 
 $$ = make_str("session_user");
}
|  SOME
 { 
 $$ = make_str("some");
}
|  SYMMETRIC
 { 
 $$ = make_str("symmetric");
}
|  TABLE
 { 
 $$ = make_str("table");
}
|  THEN
 { 
 $$ = make_str("then");
}
|  TRAILING
 { 
 $$ = make_str("trailing");
}
|  TRUE_P
 { 
 $$ = make_str("true");
}
|  UNIQUE
 { 
 $$ = make_str("unique");
}
|  USER
 { 
 $$ = make_str("user");
}
|  USING
 { 
 $$ = make_str("using");
}
|  VARIADIC
 { 
 $$ = make_str("variadic");
}
|  WHEN
 { 
 $$ = make_str("when");
}
|  WHERE
 { 
 $$ = make_str("where");
}
|  WITH
 { 
 $$ = make_str("with");
}
;


 SpecialRuleRelation:
 OLD
 { 
		if (!QueryIsRule)
			mmerror(PARSE_ERROR, ET_ERROR, "OLD used in non-rule query\n");

 $$ = make_str("old");
}
|  NEW
 { 
		if (!QueryIsRule)
			mmerror(PARSE_ERROR, ET_ERROR, "NEW used in non-rule query\n");

 $$ = make_str("new");
}
;


/* trailer */
statements: /*EMPTY*/
                | statements statement
                ;

statement: ecpgstart at stmt ';'        { connection = NULL; }
                | ecpgstart stmt ';'
                | ecpgstart ECPGVarDeclaration
                {
                        fprintf(yyout, "%s", $2);
                        free($2);
                        output_line_number();
                }
                | ECPGDeclaration
                | c_thing               { fprintf(yyout, "%s", $1); free($1); }
                | CPP_LINE              { fprintf(yyout, "%s", $1); free($1); }
                | '{'                   { braces_open++; fputs("{", yyout); }
                | '}'                   { remove_typedefs(braces_open); remove_variables(braces_open--); fputs("}", yyout); }
                ;

CreateAsStmt: CREATE OptTemp TABLE create_as_target AS {FoundInto = 0;} SelectStmt opt_with_data
		{
			if (FoundInto == 1)
				mmerror(PARSE_ERROR, ET_ERROR, "CREATE TABLE / AS SELECT cannot specify INTO\n");

			$$ = cat_str(6, make_str("create"), $2, make_str("table"), $4, make_str("as"), $7);
		}
	;

RuleStmt: CREATE opt_or_replace RULE name AS
		{QueryIsRule = 1;}
		ON event TO qualified_name where_clause
		DO opt_instead RuleActionList
		{
			QueryIsRule=0;
			$$ = cat_str(12, make_str("create"), $2, make_str("rule"), $4, make_str("as on"), $8, make_str("to"), $10, $11, make_str("do"), $13, $14);
		}
	;

at: AT connection_object
                {
                        connection = $2;
                        /*
                         *      Do we have a variable as connection target?
                         *      Remove the variable from the variable
                         *      list or else it will be used twice
                         */
                        if (argsinsert != NULL)
                                argsinsert = NULL;
                }
        ;

/*
 * the exec sql connect statement: connect to the given database
 */
ECPGConnect: SQL_CONNECT TO connection_target opt_connection_name opt_user
			{ $$ = cat_str(5, $3, make_str(","), $5, make_str(","), $4); }
		| SQL_CONNECT TO DEFAULT
			{ $$ = make_str("NULL, NULL, NULL, \"DEFAULT\""); }
		  /* also allow ORACLE syntax */
		| SQL_CONNECT ora_user
			{ $$ = cat_str(3, make_str("NULL,"), $2, make_str(", NULL")); }
		| DATABASE connection_target
			{ $$ = cat2_str($2, make_str(", NULL, NULL, NULL")); }
		;

connection_target: opt_database_name opt_server opt_port
		{
			/* old style: dbname[@server][:port] */
			if (strlen($2) > 0 && *($2) != '@')
				mmerror(PARSE_ERROR, ET_ERROR, "expected \"@\", found \"%s\"", $2);
			
			/* C strings need to be handled differently */
			if ($1[0] == '\"')
				$$ = $1;
			else
				$$ = make3_str(make_str("\""), make3_str($1, $2, $3), make_str("\""));
		}
		|  db_prefix ':' server opt_port '/' opt_database_name opt_options
		{
			/* new style: <tcp|unix>:postgresql://server[:port][/dbname] */
			if (strncmp($1, "unix:postgresql", strlen("unix:postgresql")) != 0 && strncmp($1, "tcp:postgresql", strlen("tcp:postgresql")) != 0)
				mmerror(PARSE_ERROR, ET_ERROR, "only protocols \"tcp\" and \"unix\" and database type \"postgresql\" are supported");

			if (strncmp($3, "//", strlen("//")) != 0)
				mmerror(PARSE_ERROR, ET_ERROR, "expected \"://\", found \"%s\"", $3);

			if (strncmp($1, "unix", strlen("unix")) == 0 &&
				strncmp($3 + strlen("//"), "localhost", strlen("localhost")) != 0 &&
				strncmp($3 + strlen("//"), "127.0.0.1", strlen("127.0.0.1")) != 0)
				mmerror(PARSE_ERROR, ET_ERROR, "unix domain sockets only work on \"localhost\" but not on \"%s\"", $3 + strlen("//"));

			$$ = make3_str(make3_str(make_str("\""), $1, make_str(":")), $3, make3_str(make3_str($4, make_str("/"), $6),	$7, make_str("\"")));
		}
		| char_variable
		{
			$$ = $1;
		}
		| ecpg_real_sconst
		{
			/* We can only process double quoted strings not single quotes ones,
			 * so we change the quotes.
			 * Note, that the rule for ecpg_real_sconst adds these single quotes. */
			$1[0] = '\"';
			$1[strlen($1)-1] = '\"';
			$$ = $1;
		}
		;

opt_database_name: database_name		{ $$ = $1; }
		| /*EMPTY*/			{ $$ = EMPTY; }
		;

db_prefix: ecpg_ident cvariable
		{
			if (strcmp($2, "postgresql") != 0 && strcmp($2, "postgres") != 0)
				mmerror(PARSE_ERROR, ET_ERROR, "expected \"postgresql\", found \"%s\"", $2);

			if (strcmp($1, "tcp") != 0 && strcmp($1, "unix") != 0)
				mmerror(PARSE_ERROR, ET_ERROR, "illegal connection type %s", $1);

			$$ = make3_str($1, make_str(":"), $2);
		}
		;

server: Op server_name
		{
			if (strcmp($1, "@") != 0 && strcmp($1, "//") != 0)
				mmerror(PARSE_ERROR, ET_ERROR, "expected \"@\" or \"://\", found \"%s\"", $1);

			$$ = make2_str($1, $2);
		}
		;

opt_server: server			{ $$ = $1; }
		| /*EMPTY*/			{ $$ = EMPTY; }
		;

server_name: ColId					{ $$ = $1; }
		| ColId '.' server_name		{ $$ = make3_str($1, make_str("."), $3); }
		| IP						{ $$ = make_name(); }
		;

opt_port: ':' ecpg_iconst	{ $$ = make2_str(make_str(":"), $2); }
		| /*EMPTY*/	{ $$ = EMPTY; }
		;

opt_connection_name: AS connection_object	{ $$ = $2; }
		| /*EMPTY*/			{ $$ = make_str("NULL"); }
		;

opt_user: USER ora_user		{ $$ = $2; }
		| /*EMPTY*/			{ $$ = make_str("NULL, NULL"); }
		;

ora_user: user_name
			{ $$ = cat2_str($1, make_str(", NULL")); }
		| user_name '/' user_name
			{ $$ = cat_str(3, $1, make_str(","), $3); }
		| user_name SQL_IDENTIFIED BY user_name
			{ $$ = cat_str(3, $1, make_str(","), $4); }
		| user_name USING user_name
			{ $$ = cat_str(3, $1, make_str(","), $3); }
		;

user_name: RoleId
		{
			if ($1[0] == '\"')
				$$ = $1;
			else
				$$ = make3_str(make_str("\""), $1, make_str("\""));
		}
		| ecpg_sconst
		{
			if ($1[0] == '\"')
				$$ = $1;
			else if ($1[1] == '$') /* variable */
			{
				enum ECPGttype type = argsinsert->variable->type->type;

				/* if array see what's inside */
				if (type == ECPGt_array)
					type = argsinsert->variable->type->u.element->type;

				/* handle varchars */
				if (type == ECPGt_varchar)
					$$ = make2_str(mm_strdup(argsinsert->variable->name), make_str(".arr"));
				else
					$$ = mm_strdup(argsinsert->variable->name);
			}
			else
				$$ = make3_str(make_str("\""), $1, make_str("\""));
		}
		;

char_variable: cvariable
		{
			/* check if we have a string variable */
			struct variable *p = find_variable($1);
			enum ECPGttype type = p->type->type;

			/* If we have just one character this is not a string */
			if (atol(p->type->size) == 1)
					mmerror(PARSE_ERROR, ET_ERROR, "invalid datatype");
			else
			{
				/* if array see what's inside */
				if (type == ECPGt_array)
					type = p->type->u.element->type;

				switch (type)
				{
					case ECPGt_char:
					case ECPGt_unsigned_char:
						$$ = $1;
						break;
					case ECPGt_varchar:
						$$ = make2_str($1, make_str(".arr"));
						break;
					default:
						mmerror(PARSE_ERROR, ET_ERROR, "invalid datatype");
						$$ = $1;
						break;
				}
			}
		}
		;

opt_options: Op connect_options
		{
			if (strlen($1) == 0)
				mmerror(PARSE_ERROR, ET_ERROR, "incomplete statement");

			if (strcmp($1, "?") != 0)
				mmerror(PARSE_ERROR, ET_ERROR, "unrecognized token \"%s\"", $1);

			$$ = make2_str(make_str("?"), $2);
		}
		| /*EMPTY*/ 	{ $$ = EMPTY; }
		;

connect_options:  ColId opt_opt_value 
			{ $$ = make2_str($1, $2); }
	| ColId opt_opt_value Op connect_options
			{
				if (strlen($3) == 0)
					mmerror(PARSE_ERROR, ET_ERROR, "incomplete statement");

				if (strcmp($3, "&") != 0)
					mmerror(PARSE_ERROR, ET_ERROR, "unrecognized token \"%s\"", $3);

				$$ = cat_str(3, make2_str($1, $2), $3, $4);
			}
	;

opt_opt_value: /*EMPTY*/
			{ $$ = EMPTY; }
		| '=' ecpg_iconst
			{ $$ = make2_str(make_str("="), $2); }
		| '=' ecpg_ident
			{ $$ = make2_str(make_str("="), $2); }
		;

prepared_name: name             {
                                        if ($1[0] == '\"' && $1[strlen($1)-1] == '\"') /* already quoted? */
                                                $$ = $1;
                                        else /* not quoted => convert to lowercase */
                                        {
                                                int i;

                                                for (i = 0; i< strlen($1); i++)
                                                        $1[i] = tolower((unsigned char) $1[i]);

                                                $$ = make3_str(make_str("\""), $1, make_str("\""));
                                        }
                                }
                | char_variable { $$ = $1; }
                ;

/*
 * Declare a prepared cursor. The syntax is different from the standard
 * declare statement, so we create a new rule.
 */
ECPGCursorStmt:  DECLARE name cursor_options CURSOR opt_hold FOR prepared_name
		{
			struct cursor *ptr, *this;
			struct variable *thisquery = (struct variable *)mm_alloc(sizeof(struct variable));
			const char *con = connection ? connection : "NULL";

			for (ptr = cur; ptr != NULL; ptr = ptr->next)
			{
				if (strcmp($2, ptr->name) == 0)
					/* re-definition is a bug */
					mmerror(PARSE_ERROR, ET_ERROR, "cursor \"%s\" already defined", $2);
			}

			this = (struct cursor *) mm_alloc(sizeof(struct cursor));

			/* initial definition */
			this->next = cur;
			this->name = $2;
			this->connection = connection;
			this->command =  cat_str(6, make_str("declare"), mm_strdup($2), $3, make_str("cursor"), $5, make_str("for $1"));
			this->argsresult = NULL;

			thisquery->type = &ecpg_query;
			thisquery->brace_level = 0;
			thisquery->next = NULL;
			thisquery->name = (char *) mm_alloc(sizeof("ECPGprepared_statement(, , __LINE__)") + strlen(con) + strlen($7));
			sprintf(thisquery->name, "ECPGprepared_statement(%s, %s, __LINE__)", con, $7);

			this->argsinsert = NULL;
			add_variable_to_head(&(this->argsinsert), thisquery, &no_indicator);

			cur = this;

			$$ = cat_str(3, make_str("/*"), mm_strdup(this->command), make_str("*/"));
		}
		;

ECPGExecuteImmediateStmt: EXECUTE IMMEDIATE execstring
			{ 
			  /* execute immediate means prepare the statement and
			   * immediately execute it */
			  $$ = $3;
			};
/*
 * variable decalartion outside exec sql declare block
 */
ECPGVarDeclaration: single_vt_declaration;

single_vt_declaration: type_declaration		{ $$ = $1; }
		| var_declaration		{ $$ = $1; }
		;

precision:	NumericOnly	{ $$ = $1; };

opt_scale:	',' NumericOnly	{ $$ = $2; }
		| /* EMPTY */	{ $$ = EMPTY; }
		;

ecpg_interval:	opt_interval	{ $$ = $1; }
		| YEAR_P TO MINUTE_P	{ $$ = make_str("year to minute"); }
		| YEAR_P TO SECOND_P	{ $$ = make_str("year to second"); }
		| DAY_P TO DAY_P		{ $$ = make_str("day to day"); }
		| MONTH_P TO MONTH_P	{ $$ = make_str("month to month"); }
		;

/*
 * variable declaration inside exec sql declare block
 */
ECPGDeclaration: sql_startdeclare
		{ fputs("/* exec sql begin declare section */", yyout); }
		var_type_declarations sql_enddeclare
		{
			fprintf(yyout, "%s/* exec sql end declare section */", $3);
			free($3);
			output_line_number();
		}
		;

sql_startdeclare: ecpgstart BEGIN_P DECLARE SQL_SECTION ';' {};

sql_enddeclare: ecpgstart END_P DECLARE SQL_SECTION ';' {};

var_type_declarations:	/*EMPTY*/			{ $$ = EMPTY; }
		| vt_declarations			{ $$ = $1; }
		| CPP_LINE				{ $$ = $1; }
		;

vt_declarations:  var_declaration			{ $$ = $1; }
		| type_declaration			{ $$ = $1; }
		| vt_declarations var_declaration	{ $$ = cat2_str($1, $2); }
		| vt_declarations type_declaration	{ $$ = cat2_str($1, $2); }
		| vt_declarations CPP_LINE		{ $$ = cat2_str($1, $2); }
		;

variable_declarations:	var_declaration 	{ $$ = $1; }
		| variable_declarations var_declaration 	{ $$ = cat2_str($1, $2); }
		;

type_declaration: S_TYPEDEF
	{
		/* reset this variable so we see if there was */
		/* an initializer specified */
		initializer = 0;
	}
	var_type opt_pointer ECPGColLabelCommon opt_array_bounds ';'
	{
		add_typedef($5, $6.index1, $6.index2, $3.type_enum, $3.type_dimension, $3.type_index, initializer, *$4 ? 1 : 0);

		fprintf(yyout, "typedef %s %s %s %s;\n", $3.type_str, *$4 ? "*" : "", $5, $6.str);
		output_line_number();
		$$ = make_str("");
	};

var_declaration: storage_declaration
		var_type
		{
			actual_type[struct_level].type_enum = $2.type_enum;
			actual_type[struct_level].type_dimension = $2.type_dimension;
			actual_type[struct_level].type_index = $2.type_index;
			actual_type[struct_level].type_sizeof = $2.type_sizeof;

			actual_startline[struct_level] = hashline_number();
		}
		variable_list ';'
		{
			$$ = cat_str(5, actual_startline[struct_level], $1, $2.type_str, $4, make_str(";\n"));
		}
		| var_type
		{
			actual_type[struct_level].type_enum = $1.type_enum;
			actual_type[struct_level].type_dimension = $1.type_dimension;
			actual_type[struct_level].type_index = $1.type_index;
			actual_type[struct_level].type_sizeof = $1.type_sizeof;

			actual_startline[struct_level] = hashline_number();
		}
		variable_list ';'
		{
			$$ = cat_str(4, actual_startline[struct_level], $1.type_str, $3, make_str(";\n"));
		}
		| struct_union_type_with_symbol ';'
		{
			$$ = cat2_str($1, make_str(";"));
		}
		;

opt_bit_field:	':' ecpg_iconst	{ $$ =cat2_str(make_str(":"), $2); }
		| /* EMPTY */	{ $$ = EMPTY; }
		;

storage_declaration: storage_clause storage_modifier
			{$$ = cat2_str ($1, $2); }
		| storage_clause		{$$ = $1; }
		| storage_modifier		{$$ = $1; }
		;

storage_clause : S_EXTERN	{ $$ = make_str("extern"); }
		| S_STATIC			{ $$ = make_str("static"); }
		| S_REGISTER		{ $$ = make_str("register"); }
		| S_AUTO			{ $$ = make_str("auto"); }
		;

storage_modifier : S_CONST	{ $$ = make_str("const"); }
		| S_VOLATILE		{ $$ = make_str("volatile"); }
		;

var_type:	simple_type
		{
			$$.type_enum = $1;
			$$.type_str = mm_strdup(ecpg_type_name($1));
			$$.type_dimension = make_str("-1");
			$$.type_index = make_str("-1");
			$$.type_sizeof = NULL;
		}
		| struct_union_type
		{
			$$.type_str = $1;
			$$.type_dimension = make_str("-1");
			$$.type_index = make_str("-1");

			if (strncmp($1, "struct", sizeof("struct")-1) == 0)
			{
				$$.type_enum = ECPGt_struct;
				$$.type_sizeof = ECPGstruct_sizeof;
			}
			else
			{
				$$.type_enum = ECPGt_union;
				$$.type_sizeof = NULL;
			}
		}
		| enum_type
		{
			$$.type_str = $1;
			$$.type_enum = ECPGt_int;
			$$.type_dimension = make_str("-1");
			$$.type_index = make_str("-1");
			$$.type_sizeof = NULL;
		}
		| ECPGColLabelCommon '(' precision opt_scale ')'
		{
			if (strcmp($1, "numeric") == 0)
			{
				$$.type_enum = ECPGt_numeric;
				$$.type_str = make_str("numeric");
			}
			else if (strcmp($1, "decimal") == 0)
			{
				$$.type_enum = ECPGt_decimal;
				$$.type_str = make_str("decimal");
			}
			else
			{
				mmerror(PARSE_ERROR, ET_ERROR, "only numeric/decimal have precision/scale argument");
				$$.type_enum = ECPGt_numeric;
				$$.type_str = make_str("numeric");
			}

			$$.type_dimension = make_str("-1");
			$$.type_index = make_str("-1");
			$$.type_sizeof = NULL;
		}
		| ECPGColLabelCommon ecpg_interval
		{
			if (strlen($2) != 0 && strcmp ($1, "datetime") != 0 && strcmp ($1, "interval") != 0)
				mmerror (PARSE_ERROR, ET_ERROR, "interval specification not allowed here");

			/*
			 * Check for type names that the SQL grammar treats as
			 * unreserved keywords
			 */
			if (strcmp($1, "varchar") == 0)
			{
				$$.type_enum = ECPGt_varchar;
				$$.type_str = EMPTY; /*make_str("varchar");*/
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else if (strcmp($1, "float") == 0)
			{
				$$.type_enum = ECPGt_float;
				$$.type_str = make_str("float");
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else if (strcmp($1, "double") == 0)
			{
				$$.type_enum = ECPGt_double;
				$$.type_str = make_str("double");
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else if (strcmp($1, "numeric") == 0)
			{
				$$.type_enum = ECPGt_numeric;
				$$.type_str = make_str("numeric");
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else if (strcmp($1, "decimal") == 0)
			{
				$$.type_enum = ECPGt_decimal;
				$$.type_str = make_str("decimal");
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else if (strcmp($1, "date") == 0)
			{
				$$.type_enum = ECPGt_date;
				$$.type_str = make_str("date");
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else if (strcmp($1, "timestamp") == 0)
			{
				$$.type_enum = ECPGt_timestamp;
				$$.type_str = make_str("timestamp");
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else if (strcmp($1, "interval") == 0)
			{
				$$.type_enum = ECPGt_interval;
				$$.type_str = make_str("interval");
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else if (strcmp($1, "datetime") == 0)
			{
				$$.type_enum = ECPGt_timestamp;
				$$.type_str = make_str("timestamp");
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = NULL;
			}
			else
			{
				/* this is for typedef'ed types */
				struct typedefs *this = get_typedef($1);

				$$.type_str = (this->type->type_enum == ECPGt_varchar) ? EMPTY : mm_strdup(this->name);
				$$.type_enum = this->type->type_enum;
				$$.type_dimension = this->type->type_dimension;
				$$.type_index = this->type->type_index;
				if (this->type->type_sizeof && strlen(this->type->type_sizeof) != 0)
					$$.type_sizeof = this->type->type_sizeof;
				else 
					$$.type_sizeof = cat_str(3, make_str("sizeof("), mm_strdup(this->name), make_str(")"));

				struct_member_list[struct_level] = ECPGstruct_member_dup(this->struct_member_list);
			}
		}
		| s_struct_union_symbol
		{
			/* this is for named structs/unions */
			char *name;
			struct typedefs *this;
			bool forward = (forward_name != NULL && strcmp($1.symbol, forward_name) == 0 && strcmp($1.su, "struct") == 0);

			name = cat2_str($1.su, $1.symbol);
			/* Do we have a forward definition? */
			if (!forward)
			{
				/* No */

				this = get_typedef(name);
				$$.type_str = mm_strdup(this->name);
				$$.type_enum = this->type->type_enum;
				$$.type_dimension = this->type->type_dimension;
				$$.type_index = this->type->type_index;
				$$.type_sizeof = this->type->type_sizeof;
				struct_member_list[struct_level] = ECPGstruct_member_dup(this->struct_member_list);
				free(name);
			}
			else
			{
				$$.type_str = name;
				$$.type_enum = ECPGt_long;
				$$.type_dimension = make_str("-1");
				$$.type_index = make_str("-1");
				$$.type_sizeof = make_str("");
				struct_member_list[struct_level] = NULL;
			}
		}
		;

enum_type: ENUM_P symbol enum_definition
			{ $$ = cat_str(3, make_str("enum"), $2, $3); }
		| ENUM_P enum_definition
			{ $$ = cat2_str(make_str("enum"), $2); }
		| ENUM_P symbol
			{ $$ = cat2_str(make_str("enum"), $2); }
		;

enum_definition: '{' c_list '}'
			{ $$ = cat_str(3, make_str("{"), $2, make_str("}")); };

struct_union_type_with_symbol: s_struct_union_symbol
		{
			struct_member_list[struct_level++] = NULL;
			if (struct_level >= STRUCT_DEPTH)
				 mmerror(PARSE_ERROR, ET_ERROR, "too many levels in nested structure/union definition");
			forward_name = mm_strdup($1.symbol);
		}
		'{' variable_declarations '}'
		{
			struct typedefs *ptr, *this;
			struct this_type su_type;

			ECPGfree_struct_member(struct_member_list[struct_level]);
			struct_member_list[struct_level] = NULL;
			struct_level--;
			if (strncmp($1.su, "struct", sizeof("struct")-1) == 0)
				su_type.type_enum = ECPGt_struct;
			else
				su_type.type_enum = ECPGt_union;
			su_type.type_str = cat2_str($1.su, $1.symbol);
			free(forward_name);
			forward_name = NULL;

			/* This is essantially a typedef but needs the keyword struct/union as well.
			 * So we create the typedef for each struct definition with symbol */
			for (ptr = types; ptr != NULL; ptr = ptr->next)
			{
					if (strcmp(su_type.type_str, ptr->name) == 0)
							/* re-definition is a bug */
							mmerror(PARSE_ERROR, ET_ERROR, "type \"%s\" already defined", su_type.type_str);
			}

			this = (struct typedefs *) mm_alloc(sizeof(struct typedefs));

			/* initial definition */
			this->next = types;
			this->name = mm_strdup(su_type.type_str);
			this->brace_level = braces_open;
			this->type = (struct this_type *) mm_alloc(sizeof(struct this_type));
			this->type->type_enum = su_type.type_enum;
			this->type->type_str = mm_strdup(su_type.type_str);
			this->type->type_dimension = make_str("-1"); /* dimension of array */
			this->type->type_index = make_str("-1");	/* length of string */
			this->type->type_sizeof = ECPGstruct_sizeof;
			this->struct_member_list = struct_member_list[struct_level];

			types = this;
			$$ = cat_str(4, su_type.type_str, make_str("{"), $4, make_str("}"));
		}
		;

struct_union_type: struct_union_type_with_symbol	{ $$ = $1; }
		| s_struct_union
		{
			struct_member_list[struct_level++] = NULL;
			if (struct_level >= STRUCT_DEPTH)
				 mmerror(PARSE_ERROR, ET_ERROR, "too many levels in nested structure/union definition");
		}
		'{' variable_declarations '}'
		{
			ECPGfree_struct_member(struct_member_list[struct_level]);
			struct_member_list[struct_level] = NULL;
			struct_level--;
			$$ = cat_str(4, $1, make_str("{"), $4, make_str("}"));
		}
		;

s_struct_union_symbol: SQL_STRUCT symbol
		{
			$$.su = make_str("struct");
			$$.symbol = $2;
			ECPGstruct_sizeof = cat_str(3, make_str("sizeof("), cat2_str(mm_strdup($$.su), mm_strdup($$.symbol)), make_str(")"));
		}
		| UNION symbol
		{
			$$.su = make_str("union");
			$$.symbol = $2;
		}
		;

s_struct_union: SQL_STRUCT
		{
			ECPGstruct_sizeof = make_str(""); /* This must not be NULL to distinguish from simple types. */
			$$ = make_str("struct");
		}
		| UNION 	{ $$ = make_str("union"); }
		;

simple_type: unsigned_type					{ $$=$1; }
		|	opt_signed signed_type			{ $$=$2; }
		;

unsigned_type: SQL_UNSIGNED SQL_SHORT		{ $$ = ECPGt_unsigned_short; }
		| SQL_UNSIGNED SQL_SHORT INT_P	{ $$ = ECPGt_unsigned_short; }
		| SQL_UNSIGNED						{ $$ = ECPGt_unsigned_int; }
		| SQL_UNSIGNED INT_P				{ $$ = ECPGt_unsigned_int; }
		| SQL_UNSIGNED SQL_LONG				{ $$ = ECPGt_unsigned_long; }
		| SQL_UNSIGNED SQL_LONG INT_P		{ $$ = ECPGt_unsigned_long; }
		| SQL_UNSIGNED SQL_LONG SQL_LONG
		{
#ifdef HAVE_LONG_LONG_INT_64
			$$ = ECPGt_unsigned_long_long;
#else
			$$ = ECPGt_unsigned_long;
#endif
		}
		| SQL_UNSIGNED SQL_LONG SQL_LONG INT_P
		{
#ifdef HAVE_LONG_LONG_INT_64
			$$ = ECPGt_unsigned_long_long;
#else
			$$ = ECPGt_unsigned_long;
#endif
		}
		| SQL_UNSIGNED CHAR_P			{ $$ = ECPGt_unsigned_char; }
		;

signed_type: SQL_SHORT				{ $$ = ECPGt_short; }
		| SQL_SHORT INT_P			{ $$ = ECPGt_short; }
		| INT_P						{ $$ = ECPGt_int; }
		| SQL_LONG					{ $$ = ECPGt_long; }
		| SQL_LONG INT_P			{ $$ = ECPGt_long; }
		| SQL_LONG SQL_LONG
		{
#ifdef HAVE_LONG_LONG_INT_64
			$$ = ECPGt_long_long;
#else
			$$ = ECPGt_long;
#endif
		}
		| SQL_LONG SQL_LONG INT_P
		{
#ifdef HAVE_LONG_LONG_INT_64
			$$ = ECPGt_long_long;
#else
			$$ = ECPGt_long;
#endif
		}
		| SQL_BOOL					{ $$ = ECPGt_bool; }
		| CHAR_P					{ $$ = ECPGt_char; }
		| DOUBLE_P					{ $$ = ECPGt_double; }
		;

opt_signed: SQL_SIGNED
		|	/* EMPTY */
		;

variable_list: variable
			{ $$ = $1; }
		| variable_list ',' variable
			{ $$ = cat_str(3, $1, make_str(","), $3); }
		;

variable: opt_pointer ECPGColLabel opt_array_bounds opt_bit_field opt_initializer
		{
			struct ECPGtype * type;
			char *dimension = $3.index1; /* dimension of array */
			char *length = $3.index2;    /* length of string */
			char dim[14L];
			char *vcn;

			adjust_array(actual_type[struct_level].type_enum, &dimension, &length, actual_type[struct_level].type_dimension, actual_type[struct_level].type_index, strlen($1), false);

			switch (actual_type[struct_level].type_enum)
			{
				case ECPGt_struct:
				case ECPGt_union:
					if (atoi(dimension) < 0)
						type = ECPGmake_struct_type(struct_member_list[struct_level], actual_type[struct_level].type_enum, actual_type[struct_level].type_sizeof);
					else
						type = ECPGmake_array_type(ECPGmake_struct_type(struct_member_list[struct_level], actual_type[struct_level].type_enum, actual_type[struct_level].type_sizeof), dimension);

					$$ = cat_str(5, $1, mm_strdup($2), $3.str, $4, $5);
					break;

				case ECPGt_varchar:
					if (atoi(dimension) < 0)
						type = ECPGmake_simple_type(actual_type[struct_level].type_enum, length, yylineno);
					else
						type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, length, yylineno), dimension);
					
					if (strcmp(dimension, "0") == 0 || abs(atoi(dimension)) == 1)
							*dim = '\0';
					else
							sprintf(dim, "[%s]", dimension);
					/* cannot check for atoi <= 0 because a defined constant will yield 0 here as well */
					if (atoi(length) < 0 || strcmp(length, "0") == 0)
						mmerror(PARSE_ERROR, ET_ERROR, "pointer to varchar are not implemented");

					/* make sure varchar struct name is unique by adding linenumer of its definition */
					vcn = (char *) mm_alloc(strlen($2) + sizeof(int) * CHAR_BIT * 10 / 3);
					sprintf(vcn, "%s_%d", $2, yylineno);
					if (strcmp(dimension, "0") == 0)
						$$ = cat_str(7, make2_str(make_str(" struct varchar_"), vcn), make_str(" { int len; char arr["), mm_strdup(length), make_str("]; } *"), mm_strdup($2), $4, $5);
					else
						$$ = cat_str(8, make2_str(make_str(" struct varchar_"), vcn), make_str(" { int len; char arr["), mm_strdup(length), make_str("]; } "), mm_strdup($2), mm_strdup(dim), $4, $5);
					break;

				case ECPGt_char:
				case ECPGt_unsigned_char:
					if (atoi(dimension) == -1)
					{
						int i = strlen($5);

						if (atoi(length) == -1 && i > 0) /* char <var>[] = "string" */
						{
							/* if we have an initializer but no string size set, let's use the initializer's length */
							free(length);
							length = mm_alloc(i+sizeof("sizeof()"));
							sprintf(length, "sizeof(%s)", $5+2);
						}
						type = ECPGmake_simple_type(actual_type[struct_level].type_enum, length, 0);
					}
					else
						type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, length, 0), dimension);

					$$ = cat_str(5, $1, mm_strdup($2), $3.str, $4, $5);
					break;

				default:
					if (atoi(dimension) < 0)
						type = ECPGmake_simple_type(actual_type[struct_level].type_enum, make_str("1"), 0);
					else
						type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, make_str("1"), 0), dimension);

					$$ = cat_str(5, $1, mm_strdup($2), $3.str, $4, $5);
					break;
			}

			if (struct_level == 0)
				new_variable($2, type, braces_open);
			else
				ECPGmake_struct_member($2, type, &(struct_member_list[struct_level - 1]));

			free($2);
		}
		;

opt_initializer: /*EMPTY*/
			{ $$ = EMPTY; }
		| '=' c_term
		{
			initializer = 1;
			$$ = cat2_str(make_str("="), $2);
		}
		;

opt_pointer: /*EMPTY*/				{ $$ = EMPTY; }
		| '*'						{ $$ = make_str("*"); }
		| '*' '*'					{ $$ = make_str("**"); }
		;

/*
 * We try to simulate the correct DECLARE syntax here so we get dynamic SQL
 */
ECPGDeclare: DECLARE STATEMENT ecpg_ident
		{
			/* this is only supported for compatibility */
			$$ = cat_str(3, make_str("/* declare statement"), $3, make_str("*/"));
		}
		;
/*
 * the exec sql disconnect statement: disconnect from the given database
 */
ECPGDisconnect: SQL_DISCONNECT dis_name { $$ = $2; }
		;

dis_name: connection_object			{ $$ = $1; }
		| CURRENT_P			{ $$ = make_str("\"CURRENT\""); }
		| ALL				{ $$ = make_str("\"ALL\""); }
		| /* EMPTY */			{ $$ = make_str("\"CURRENT\""); }
		;

connection_object: database_name		{ $$ = make3_str(make_str("\""), $1, make_str("\"")); }
		| DEFAULT			{ $$ = make_str("\"DEFAULT\""); }
		| char_variable			{ $$ = $1; }
		;

execstring: char_variable
			{ $$ = $1; }
		|	CSTRING
			{ $$ = make3_str(make_str("\""), $1, make_str("\"")); }
		;

/*
 * the exec sql free command to deallocate a previously
 * prepared statement
 */
ECPGFree:	SQL_FREE name	{ $$ = $2; }
		| SQL_FREE ALL	{ $$ = make_str("all"); }
		;

/*
 * open is an open cursor, at the moment this has to be removed
 */
ECPGOpen: SQL_OPEN name opt_ecpg_using { $$ = $2; };

opt_ecpg_using: /*EMPTY*/	{ $$ = EMPTY; }
		| ecpg_using		{ $$ = $1; }
		;

ecpg_using:	USING using_list 	{ $$ = EMPTY; }
		| using_descriptor      { $$ = $1; }
		;

using_descriptor: USING opt_sql SQL_DESCRIPTOR quoted_ident_stringvar
		{
			add_variable_to_head(&argsinsert, descriptor_variable($4,0), &no_indicator);
			$$ = EMPTY;
		}
		;

into_descriptor: INTO opt_sql SQL_DESCRIPTOR quoted_ident_stringvar
		{
			add_variable_to_head(&argsresult, descriptor_variable($4,1), &no_indicator);
			$$ = EMPTY;
		}
		;

opt_sql: /*EMPTY*/ | SQL_SQL;

using_list: UsingValue | UsingValue ',' using_list;

UsingValue: UsingConst
		{
			char *length = mm_alloc(32);

			sprintf(length, "%d", (int) strlen($1));
			add_variable_to_head(&argsinsert, new_variable($1, ECPGmake_simple_type(ECPGt_const, length, 0), 0), &no_indicator);
		}
		| civar { $$ = EMPTY; }
		| civarind { $$ = EMPTY; }
		; 

UsingConst: ecpg_real_iconst		{ $$ = $1; }
		| ecpg_fconst		{ $$ = $1; }
		| ecpg_real_sconst	{ $$ = $1; }
		| ecpg_bconst		{ $$ = $1; }
		| ecpg_xconst		{ $$ = $1; }
		;

/*
 * We accept descibe but do nothing with it so far.
 */
ECPGDescribe: SQL_DESCRIBE INPUT_P name using_descriptor
	{
		const char *con = connection ? connection : "NULL";
		mmerror(PARSE_ERROR, ET_WARNING, "using unsupported describe statement\n");
		$$ = (char *) mm_alloc(sizeof("1, ECPGprepared_statement(, \"\", __LINE__)") + strlen(con) + strlen($3));
		sprintf($$, "1, ECPGprepared_statement(%s, \"%s\", __LINE__)", con, $3);
	}
	| SQL_DESCRIBE opt_output name using_descriptor
	{
		const char *con = connection ? connection : "NULL";
		mmerror(PARSE_ERROR, ET_WARNING, "using unsupported describe statement\n");
		$$ = (char *) mm_alloc(sizeof("0, ECPGprepared_statement(, \"\", __LINE__)") + strlen(con) + strlen($3));
		sprintf($$, "0, ECPGprepared_statement(%s, \"%s\", __LINE__)", con, $3);
	}
	| SQL_DESCRIBE opt_output name into_descriptor
	{
		const char *con = connection ? connection : "NULL";
		mmerror(PARSE_ERROR, ET_WARNING, "using unsupported describe statement\n");
		$$ = (char *) mm_alloc(sizeof("0, ECPGprepared_statement(, \"\", __LINE__)") + strlen(con) + strlen($3));
		sprintf($$, "0, ECPGprepared_statement(%s, \"%s\", __LINE__)", con, $3);
	}
	;

opt_output:	SQL_OUTPUT	{ $$ = make_str("output"); }
	| 	/* EMPTY */	{ $$ = EMPTY; }
	;

/*
 * dynamic SQL: descriptor based access
 *	originall written by Christof Petig <christof.petig@wtal.de>
 *			and Peter Eisentraut <peter.eisentraut@credativ.de>
 */

/*
 * allocate a descriptor
 */
ECPGAllocateDescr:     SQL_ALLOCATE SQL_DESCRIPTOR quoted_ident_stringvar
		{
			add_descriptor($3,connection);
			$$ = $3;
		}
		;


/*
 * deallocate a descriptor
 */
ECPGDeallocateDescr:	DEALLOCATE SQL_DESCRIPTOR quoted_ident_stringvar
		{
			drop_descriptor($3,connection);
			$$ = $3;
		}
		;

/*
 * manipulate a descriptor header
 */

ECPGGetDescriptorHeader: SQL_GET SQL_DESCRIPTOR quoted_ident_stringvar ECPGGetDescHeaderItems
			{  $$ = $3; }
		;

ECPGGetDescHeaderItems: ECPGGetDescHeaderItem
		| ECPGGetDescHeaderItems ',' ECPGGetDescHeaderItem
		;

ECPGGetDescHeaderItem: cvariable '=' desc_header_item
			{ push_assignment($1, $3); }
		;


ECPGSetDescriptorHeader: SET SQL_DESCRIPTOR quoted_ident_stringvar ECPGSetDescHeaderItems
			{ $$ = $3; }
		;

ECPGSetDescHeaderItems: ECPGSetDescHeaderItem
		| ECPGSetDescHeaderItems ',' ECPGSetDescHeaderItem
		;

ECPGSetDescHeaderItem: desc_header_item '=' IntConstVar
		{
			push_assignment($3, $1);
		}
		;

IntConstVar:    ecpg_real_iconst
                {
                        char *length = mm_alloc(sizeof(int) * CHAR_BIT * 10 / 3);

                        sprintf(length, "%d", (int) strlen($1));
                        new_variable($1, ECPGmake_simple_type(ECPGt_const, length, 0), 0);
                        $$ = $1;
                }
                | cvariable     { $$ = $1; }
                ;

desc_header_item:	SQL_COUNT			{ $$ = ECPGd_count; }
		;

/*
 * manipulate a descriptor
 */

ECPGGetDescriptor:	SQL_GET SQL_DESCRIPTOR quoted_ident_stringvar VALUE_P IntConstVar ECPGGetDescItems
			{  $$.str = $5; $$.name = $3; }
		;

ECPGGetDescItems: ECPGGetDescItem
		| ECPGGetDescItems ',' ECPGGetDescItem
		;

ECPGGetDescItem: cvariable '=' descriptor_item	{ push_assignment($1, $3); };


ECPGSetDescriptor:	SET SQL_DESCRIPTOR quoted_ident_stringvar VALUE_P IntConstVar ECPGSetDescItems
			{  $$.str = $5; $$.name = $3; }
		;

ECPGSetDescItems: ECPGSetDescItem
		| ECPGSetDescItems ',' ECPGSetDescItem
		;

ECPGSetDescItem: descriptor_item '=' AllConstVar
		{
			push_assignment($3, $1);
		}
		;

AllConstVar:    ecpg_fconst
                {
                        char *length = mm_alloc(sizeof(int) * CHAR_BIT * 10 / 3);

                        sprintf(length, "%d", (int) strlen($1));
                        new_variable($1, ECPGmake_simple_type(ECPGt_const, length, 0), 0);
                        $$ = $1;
                }
                | IntConstVar           { $$ = $1; }
                | '-' ecpg_fconst
                {
                        char *length = mm_alloc(sizeof(int) * CHAR_BIT * 10 / 3);
                        char *var = cat2_str(make_str("-"), $2);

                        sprintf(length, "%d", (int) strlen(var));
                        new_variable(var, ECPGmake_simple_type(ECPGt_const, length, 0), 0);
                        $$ = var;
                }
                | '-' ecpg_real_iconst
                {
                        char *length = mm_alloc(sizeof(int) * CHAR_BIT * 10 / 3);
                        char *var = cat2_str(make_str("-"), $2);

                        sprintf(length, "%d", (int) strlen(var));
                        new_variable(var, ECPGmake_simple_type(ECPGt_const, length, 0), 0);
                        $$ = var;
                }
                | ecpg_real_sconst
                {
                        char *length = mm_alloc(sizeof(int) * CHAR_BIT * 10 / 3);
                        char *var = $1 + 1;

                        var[strlen(var) - 1] = '\0';
                        sprintf(length, "%d", (int) strlen(var));
                        new_variable(var, ECPGmake_simple_type(ECPGt_const, length, 0), 0);
                        $$ = var;
                }
		;

descriptor_item:	SQL_CARDINALITY			{ $$ = ECPGd_cardinality; }
		| DATA_P				{ $$ = ECPGd_data; }
		| SQL_DATETIME_INTERVAL_CODE		{ $$ = ECPGd_di_code; }
		| SQL_DATETIME_INTERVAL_PRECISION 	{ $$ = ECPGd_di_precision; }
		| SQL_INDICATOR				{ $$ = ECPGd_indicator; }
		| SQL_KEY_MEMBER			{ $$ = ECPGd_key_member; }
		| SQL_LENGTH				{ $$ = ECPGd_length; }
		| NAME_P				{ $$ = ECPGd_name; }
		| SQL_NULLABLE				{ $$ = ECPGd_nullable; }
		| SQL_OCTET_LENGTH			{ $$ = ECPGd_octet; }
		| PRECISION				{ $$ = ECPGd_precision; }
		| SQL_RETURNED_LENGTH			{ $$ = ECPGd_length; }
		| SQL_RETURNED_OCTET_LENGTH		{ $$ = ECPGd_ret_octet; }
		| SQL_SCALE				{ $$ = ECPGd_scale; }
		| TYPE_P				{ $$ = ECPGd_type; }
		;

/*
 * set/reset the automatic transaction mode, this needs a differnet handling
 * as the other set commands
 */
ECPGSetAutocommit:	SET SQL_AUTOCOMMIT '=' on_off	{ $$ = $4; }
		|  SET SQL_AUTOCOMMIT TO on_off   { $$ = $4; }
		;

on_off: ON				{ $$ = make_str("on"); }
		| OFF			{ $$ = make_str("off"); }
		;

/*
 * set the actual connection, this needs a differnet handling as the other
 * set commands
 */
ECPGSetConnection:	SET CONNECTION TO connection_object { $$ = $4; }
		| SET CONNECTION '=' connection_object { $$ = $4; }
		| SET CONNECTION  connection_object { $$ = $3; }
		;

/*
 * define a new type for embedded SQL
 */
ECPGTypedef: TYPE_P
		{
			/* reset this variable so we see if there was */
			/* an initializer specified */
			initializer = 0;
		}
		ECPGColLabelCommon IS var_type opt_array_bounds opt_reference
		{
			add_typedef($3, $6.index1, $6.index2, $5.type_enum, $5.type_dimension, $5.type_index, initializer, *$7 ? 1 : 0);

			if (auto_create_c == false)
				$$ = cat_str(7, make_str("/* exec sql type"), mm_strdup($3), make_str("is"), mm_strdup($5.type_str), mm_strdup($6.str), $7, make_str("*/"));
			else
				$$ = cat_str(6, make_str("typedef "), mm_strdup($5.type_str), *$7?make_str("*"):make_str(""), mm_strdup($6.str), mm_strdup($3), make_str(";"));
		}
		;

opt_reference: SQL_REFERENCE 		{ $$ = make_str("reference"); }
		| /*EMPTY*/		 			{ $$ = EMPTY; }
		;

/*
 * define the type of one variable for embedded SQL
 */
ECPGVar: SQL_VAR
		{
			/* reset this variable so we see if there was */
			/* an initializer specified */
			initializer = 0;
		}
		ColLabel IS var_type opt_array_bounds opt_reference
		{
			struct variable *p = find_variable($3);
			char *dimension = $6.index1;
			char *length = $6.index2;
			struct ECPGtype * type;

			if (($5.type_enum == ECPGt_struct ||
				 $5.type_enum == ECPGt_union) &&
				initializer == 1)
				mmerror(PARSE_ERROR, ET_ERROR, "initializer not allowed in EXEC SQL VAR command");
			else
			{
				adjust_array($5.type_enum, &dimension, &length, $5.type_dimension, $5.type_index, *$7?1:0, false);

				switch ($5.type_enum)
				{
					case ECPGt_struct:
					case ECPGt_union:
						if (atoi(dimension) < 0)
							type = ECPGmake_struct_type(struct_member_list[struct_level], $5.type_enum, $5.type_sizeof);
						else
							type = ECPGmake_array_type(ECPGmake_struct_type(struct_member_list[struct_level], $5.type_enum,$5.type_sizeof), dimension);
						break;

					case ECPGt_varchar:
						if (atoi(dimension) == -1)
							type = ECPGmake_simple_type($5.type_enum, length, 0);
						else
							type = ECPGmake_array_type(ECPGmake_simple_type($5.type_enum, length, 0), dimension);
						break;

					case ECPGt_char:
					case ECPGt_unsigned_char:
						if (atoi(dimension) == -1)
							type = ECPGmake_simple_type($5.type_enum, length, 0);
						else
							type = ECPGmake_array_type(ECPGmake_simple_type($5.type_enum, length, 0), dimension);
						break;

					default:
						if (atoi(length) >= 0)
							mmerror(PARSE_ERROR, ET_ERROR, "no multidimensional array support for simple data types");

						if (atoi(dimension) < 0)
							type = ECPGmake_simple_type($5.type_enum, make_str("1"), 0);
						else
							type = ECPGmake_array_type(ECPGmake_simple_type($5.type_enum, make_str("1"), 0), dimension);
						break;
				}

				ECPGfree_type(p->type);
				p->type = type;
			}

			$$ = cat_str(7, make_str("/* exec sql var"), mm_strdup($3), make_str("is"), mm_strdup($5.type_str), mm_strdup($6.str), $7, make_str("*/"));
		}
		;

/*
 * whenever statement: decide what to do in case of error/no data found
 * according to SQL standards we lack: SQLSTATE, CONSTRAINT and SQLEXCEPTION
 */
ECPGWhenever: SQL_WHENEVER SQL_SQLERROR action
		{
			when_error.code = $<action>3.code;
			when_error.command = $<action>3.command;
			$$ = cat_str(3, make_str("/* exec sql whenever sqlerror "), $3.str, make_str("; */"));
		}
		| SQL_WHENEVER NOT SQL_FOUND action
		{
			when_nf.code = $<action>4.code;
			when_nf.command = $<action>4.command;
			$$ = cat_str(3, make_str("/* exec sql whenever not found "), $4.str, make_str("; */"));
		}
		| SQL_WHENEVER SQL_SQLWARNING action
		{
			when_warn.code = $<action>3.code;
			when_warn.command = $<action>3.command;
			$$ = cat_str(3, make_str("/* exec sql whenever sql_warning "), $3.str, make_str("; */"));
		}
		;

action : CONTINUE_P
		{
			$<action>$.code = W_NOTHING;
			$<action>$.command = NULL;
			$<action>$.str = make_str("continue");
		}
		| SQL_SQLPRINT
		{
			$<action>$.code = W_SQLPRINT;
			$<action>$.command = NULL;
			$<action>$.str = make_str("sqlprint");
		}
		| SQL_STOP
		{
			$<action>$.code = W_STOP;
			$<action>$.command = NULL;
			$<action>$.str = make_str("stop");
		}
		| SQL_GOTO name
		{
			$<action>$.code = W_GOTO;
			$<action>$.command = strdup($2);
			$<action>$.str = cat2_str(make_str("goto "), $2);
		}
		| SQL_GO TO name
		{
			$<action>$.code = W_GOTO;
			$<action>$.command = strdup($3);
			$<action>$.str = cat2_str(make_str("goto "), $3);
		}
		| DO name '(' c_args ')'
		{
			$<action>$.code = W_DO;
			$<action>$.command = cat_str(4, $2, make_str("("), $4, make_str(")"));
			$<action>$.str = cat2_str(make_str("do"), mm_strdup($<action>$.command));
		}
		| DO SQL_BREAK
		{
			$<action>$.code = W_BREAK;
			$<action>$.command = NULL;
			$<action>$.str = make_str("break");
		}
		| SQL_CALL name '(' c_args ')'
		{
			$<action>$.code = W_DO;
			$<action>$.command = cat_str(4, $2, make_str("("), $4, make_str(")"));
			$<action>$.str = cat2_str(make_str("call"), mm_strdup($<action>$.command));
		}
		| SQL_CALL name
		{
			$<action>$.code = W_DO;
			$<action>$.command = cat2_str($2, make_str("()"));
			$<action>$.str = cat2_str(make_str("call"), mm_strdup($<action>$.command));
		}
		;

/* some other stuff for ecpg */

/* additional unreserved keywords */
ECPGKeywords: ECPGKeywords_vanames	{ $$ = $1; }
		| ECPGKeywords_rest 	{ $$ = $1; }
		;

ECPGKeywords_vanames:  SQL_BREAK		{ $$ = make_str("break"); }
		| SQL_CALL						{ $$ = make_str("call"); }
		| SQL_CARDINALITY				{ $$ = make_str("cardinality"); }
		| SQL_COUNT						{ $$ = make_str("count"); }
		| SQL_DATETIME_INTERVAL_CODE	{ $$ = make_str("datetime_interval_code"); }
		| SQL_DATETIME_INTERVAL_PRECISION	{ $$ = make_str("datetime_interval_precision"); }
		| SQL_FOUND						{ $$ = make_str("found"); }
		| SQL_GO						{ $$ = make_str("go"); }
		| SQL_GOTO						{ $$ = make_str("goto"); }
		| SQL_IDENTIFIED				{ $$ = make_str("identified"); }
		| SQL_INDICATOR				{ $$ = make_str("indicator"); }
		| SQL_KEY_MEMBER			{ $$ = make_str("key_member"); }
		| SQL_LENGTH				{ $$ = make_str("length"); }
		| SQL_NULLABLE				{ $$ = make_str("nullable"); }
		| SQL_OCTET_LENGTH			{ $$ = make_str("octet_length"); }
		| SQL_RETURNED_LENGTH		{ $$ = make_str("returned_length"); }
		| SQL_RETURNED_OCTET_LENGTH	{ $$ = make_str("returned_octet_length"); }
		| SQL_SCALE					{ $$ = make_str("scale"); }
		| SQL_SECTION				{ $$ = make_str("section"); }
		| SQL_SQL				{ $$ = make_str("sql"); }
		| SQL_SQLERROR				{ $$ = make_str("sqlerror"); }
		| SQL_SQLPRINT				{ $$ = make_str("sqlprint"); }
		| SQL_SQLWARNING			{ $$ = make_str("sqlwarning"); }
		| SQL_STOP					{ $$ = make_str("stop"); }
		;

ECPGKeywords_rest:  SQL_CONNECT		{ $$ = make_str("connect"); }
		| SQL_DESCRIBE				{ $$ = make_str("describe"); }
		| SQL_DISCONNECT			{ $$ = make_str("disconnect"); }
		| SQL_OPEN					{ $$ = make_str("open"); }
		| SQL_VAR					{ $$ = make_str("var"); }
		| SQL_WHENEVER				{ $$ = make_str("whenever"); }
		;

/* additional keywords that can be SQL type names (but not ECPGColLabels) */
ECPGTypeName:  SQL_BOOL				{ $$ = make_str("bool"); }
		| SQL_LONG					{ $$ = make_str("long"); }
		| SQL_OUTPUT				{ $$ = make_str("output"); }
		| SQL_SHORT					{ $$ = make_str("short"); }
		| SQL_STRUCT				{ $$ = make_str("struct"); }
		| SQL_SIGNED				{ $$ = make_str("signed"); }
		| SQL_UNSIGNED				{ $$ = make_str("unsigned"); }
		;

symbol: ColLabel					{ $$ = $1; }
		;

ECPGColId: ecpg_ident				{ $$ = $1; }
		| ECPGunreserved_interval	{ $$ = $1; }
		| ECPGunreserved_con		{ $$ = $1; }
		| col_name_keyword		{ $$ = $1; }
		| ECPGKeywords			{ $$ = $1; }
		| ECPGCKeywords			{ $$ = $1; }
		| CHAR_P			{ $$ = make_str("char"); }
		| VALUES			{ $$ = make_str("values"); }
		;
/* Column label --- allowed labels in "AS" clauses.
 * This presently includes *all* Postgres keywords.
 */
ColLabel:  ECPGColLabel				{ $$ = $1; }
		| ECPGTypeName			{ $$ = $1; }
		| CHAR_P			{ $$ = make_str("char"); }
		| INPUT_P			{ $$ = make_str("input"); }
		| INT_P				{ $$ = make_str("int"); }
		| UNION				{ $$ = make_str("union"); }
		| TO				{ $$ = make_str("to"); }
		| ECPGCKeywords			{ $$ = $1; }
		| ECPGunreserved_interval	{ $$ = $1; }
		;

ECPGColLabelCommon:  ecpg_ident			{ $$ = $1; }
		| col_name_keyword		{ $$ = $1; }
		| type_func_name_keyword	{ $$ = $1; }
		| ECPGKeywords_vanames		{ $$ = $1; }
		;

ECPGColLabel:  ECPGColLabelCommon	{ $$ = $1; }
		| reserved_keyword		{ $$ = $1; }
		| ECPGunreserved		{ $$ = $1; }
		| ECPGKeywords_rest		{ $$ = $1; }
		;

ECPGCKeywords: S_AUTO			{ $$ = make_str("auto"); }
		| S_CONST				{ $$ = make_str("const"); }
		| S_EXTERN				{ $$ = make_str("extern"); }
		| S_REGISTER			{ $$ = make_str("register"); }
		| S_STATIC				{ $$ = make_str("static"); }
		| S_TYPEDEF				{ $$ = make_str("typedef"); }
		| S_VOLATILE			{ $$ = make_str("volatile"); }
		;

/*
 * Keyword classification lists.  Generally, every keyword present in
 * the Postgres grammar should appear in exactly one of these lists.
 *
 * Put a new keyword into the first list that it can go into without causing
 * shift or reduce conflicts.  The earlier lists define "less reserved"
 * categories of keywords.
 */

/* "Unreserved" keywords --- available for use as any kind of name.
 */
/* The following symbols must be excluded from ECPGColLabel and directly included into ColLabel
   to enable C variables to get names from ECPGColLabel:
   DAY_P, HOUR_P, MINUTE_P, MONTH_P, SECOND_P, YEAR_P
 */
unreserved_keyword: ECPGunreserved_interval | ECPGunreserved;

ECPGunreserved_interval: DAY_P			{ $$ = make_str("day"); }
		| HOUR_P			{ $$ = make_str("hour"); }
		| MINUTE_P			{ $$ = make_str("minute"); }
		| MONTH_P			{ $$ = make_str("month"); }
		| SECOND_P			{ $$ = make_str("second"); }
		| YEAR_P			{ $$ = make_str("year"); }
		;

/* The following symbol must be excluded from var_name but still included in ColId
   to enable ecpg special postgresql variables with this name:  CONNECTION
 */
ECPGunreserved:	ECPGunreserved_con		{ $$ = $1; }
		| CONNECTION			{ $$ = make_str("connection"); }
		;

ECPGunreserved_con:	  ABORT_P			{ $$ = make_str("abort"); }
		| ABSOLUTE_P		{ $$ = make_str("absolute"); }
		| ACCESS			{ $$ = make_str("access"); }
		| ACTION			{ $$ = make_str("action"); }
		| ADD_P				{ $$ = make_str("add"); }
		| ADMIN				{ $$ = make_str("admin"); }
		| AFTER				{ $$ = make_str("after"); }
		| AGGREGATE			{ $$ = make_str("aggregate"); }
		| ALSO				{ $$ = make_str("also"); }
		| ALTER				{ $$ = make_str("alter"); }
		| ALWAYS			{ $$ = make_str("always"); }
		| ASSERTION			{ $$ = make_str("assertion"); }
		| ASSIGNMENT		{ $$ = make_str("assignment"); }
		| AT				{ $$ = make_str("at"); }
		| BACKWARD			{ $$ = make_str("backward"); }
		| BEFORE			{ $$ = make_str("before"); }
		| BEGIN_P			{ $$ = make_str("begin"); }
		| BY				{ $$ = make_str("by"); }
		| CACHE				{ $$ = make_str("cache"); }
		| CASCADE			{ $$ = make_str("cascade"); }
		| CASCADED			{ $$ = make_str("cascaded"); }
		| CHAIN				{ $$ = make_str("chain"); }
		| CHARACTERISTICS	{ $$ = make_str("characteristics"); }
		| CHECKPOINT		{ $$ = make_str("checkpoint"); }
		| CLASS				{ $$ = make_str("class"); }
		| CLOSE				{ $$ = make_str("close"); }
		| CLUSTER			{ $$ = make_str("cluster"); }
		| COMMENT			{ $$ = make_str("comment"); }
		| COMMIT			{ $$ = make_str("commit"); }
		| COMMITTED			{ $$ = make_str("committed"); }
		| CONCURRENTLY		{ $$ = make_str("concurrently"); }
		| CONFIGURATION		{ $$ = make_str("configuration"); }
/*		| CONNECTION		{ $$ = make_str("connection"); }*/
		| CONSTRAINTS		{ $$ = make_str("constraints"); }
		| CONTENT_P		{ $$ = make_str("content"); }
		| CONTINUE_P		{ $$ = make_str("continue"); }
		| CONVERSION_P		{ $$ = make_str("conversion"); }
		| COPY				{ $$ = make_str("copy"); }
		| COST				{ $$ = make_str("cost"); }
		| CREATEDB			{ $$ = make_str("createdb"); }
		| CREATEROLE		{ $$ = make_str("createrole"); }
		| CREATEUSER		{ $$ = make_str("createuser"); }
		| CSV				{ $$ = make_str("csv"); }
		| CTYPE			{ $$ = make_str("ctype"); }
		| CURSOR			{ $$ = make_str("cursor"); }
		| CYCLE				{ $$ = make_str("cycle"); }
		| DATA_P			{ $$ = make_str("data"); }
		| DATABASE			{ $$ = make_str("database"); }
/*		| DAY_P				{ $$ = make_str("day"); }*/
		| DEALLOCATE		{ $$ = make_str("deallocate"); }
		| DECLARE			{ $$ = make_str("declare"); }
		| DEFAULTS			{ $$ = make_str("defaults"); }
		| DEFERRED			{ $$ = make_str("deferred"); }
		| DELETE_P			{ $$ = make_str("delete"); }
		| DELIMITER			{ $$ = make_str("delimiter"); }
		| DELIMITERS		{ $$ = make_str("delimiters"); }
		| DICTIONARY		{ $$ = make_str("dictionary"); }
		| DISABLE_P			{ $$ = make_str("disable"); }
		| DISCARD			{ $$ = make_str("discard"); }
		| DOCUMENT_P			{ $$ = make_str("document"); }
		| DOMAIN_P			{ $$ = make_str("domain"); }
		| DOUBLE_P			{ $$ = make_str("double"); }
		| DROP				{ $$ = make_str("drop"); }
		| EACH				{ $$ = make_str("each"); }
		| ENABLE_P			{ $$ = make_str("enable"); }
		| ENCODING			{ $$ = make_str("encoding"); }
		| ENCRYPTED			{ $$ = make_str("encrypted"); }
/*		| ENUM_P			{ $$ = make_str("enum"); }*/
		| ESCAPE			{ $$ = make_str("escape"); }
		| EXCLUDING			{ $$ = make_str("excluding"); }
		| EXCLUSIVE			{ $$ = make_str("exclusive"); }
		| EXECUTE			{ $$ = make_str("execute"); }
		| EXPLAIN			{ $$ = make_str("explain"); }
		| EXTERNAL			{ $$ = make_str("external"); }
		| FAMILY			{ $$ = make_str("family"); }
/*		| FETCH				{ $$ = make_str("fetch"); }*/
		| FIRST_P			{ $$ = make_str("first"); }
		| FORCE				{ $$ = make_str("force"); }
		| FORWARD			{ $$ = make_str("forward"); }
		| FUNCTION			{ $$ = make_str("function"); }
		| GLOBAL			{ $$ = make_str("global"); }
		| GRANTED			{ $$ = make_str("granted"); }
		| HANDLER			{ $$ = make_str("handler"); }
		| HEADER_P			{ $$ = make_str("header"); }
		| HOLD				{ $$ = make_str("hold"); }
/*		| HOUR_P			{ $$ = make_str("hour"); }*/
		| IDENTITY_P			{ $$ = make_str("identity"); }
		| IF_P				{ $$ = make_str("if"); }
		| IMMEDIATE			{ $$ = make_str("immediate"); }
		| IMMUTABLE			{ $$ = make_str("immutable"); }
		| IMPLICIT_P		{ $$ = make_str("implicit"); }
		| INCLUDING			{ $$ = make_str("including"); }
		| INCREMENT			{ $$ = make_str("increment"); }
		| INDEX				{ $$ = make_str("index"); }
		| INDEXES			{ $$ = make_str("indexes"); }
		| INHERIT			{ $$ = make_str("inherit"); }
		| INHERITS			{ $$ = make_str("inherits"); }
		| INSENSITIVE		{ $$ = make_str("insensitive"); }
		| INSERT			{ $$ = make_str("insert"); }
		| INSTEAD			{ $$ = make_str("instead"); }
		| ISOLATION			{ $$ = make_str("isolation"); }
		| KEY				{ $$ = make_str("key"); }
		| LANCOMPILER		{ $$ = make_str("lancompiler"); }
		| LANGUAGE			{ $$ = make_str("language"); }
		| LARGE_P			{ $$ = make_str("large"); }
		| LAST_P			{ $$ = make_str("last"); }
		| LEVEL				{ $$ = make_str("level"); }
		| LISTEN			{ $$ = make_str("listen"); }
		| LOAD				{ $$ = make_str("load"); }
		| LOCAL				{ $$ = make_str("local"); }
		| LOCATION			{ $$ = make_str("location"); }
		| LOCK_P			{ $$ = make_str("lock"); }
		| LOGIN_P			{ $$ = make_str("login"); }
		| MAPPING			{ $$ = make_str("mapping"); }
		| MATCH				{ $$ = make_str("match"); }
		| MAXVALUE			{ $$ = make_str("maxvalue"); }
/*		| MINUTE_P			{ $$ = make_str("minute"); }*/
		| MINVALUE			{ $$ = make_str("minvalue"); }
		| MODE				{ $$ = make_str("mode"); }
/*		| MONTH_P			{ $$ = make_str("month"); }*/
		| MOVE				{ $$ = make_str("move"); }
		| NAME_P			{ $$ = make_str("name"); }
		| NAMES				{ $$ = make_str("names"); }
		| NEXT				{ $$ = make_str("next"); }
		| NO				{ $$ = make_str("no"); }
		| NOCREATEDB		{ $$ = make_str("nocreatedb"); }
		| NOCREATEROLE		{ $$ = make_str("nocreaterole"); }
		| NOCREATEUSER		{ $$ = make_str("nocreateuser"); }
		| NOINHERIT			{ $$ = make_str("noinherit"); }
		| NOLOGIN_P 		{ $$ = make_str("nologin"); }
		| NOSUPERUSER		{ $$ = make_str("nosuperuser"); }
		| NOTHING			{ $$ = make_str("nothing"); }
		| NOTIFY			{ $$ = make_str("notify"); }
		| NOWAIT			{ $$ = make_str("nowait"); }
		| NULLS_P			{ $$ = make_str("nulls"); }
		| OBJECT_P			{ $$ = make_str("object"); }
		| OF				{ $$ = make_str("of"); }
		| OIDS				{ $$ = make_str("oids"); }
		| OPERATOR			{ $$ = make_str("operator"); }
		| OPTION			{ $$ = make_str("option"); }
		| OWNED				{ $$ = make_str("owned"); }
		| OWNER				{ $$ = make_str("owner"); }
		| PARSER			{ $$ = make_str("parser"); }
		| PARTIAL			{ $$ = make_str("partial"); }
		| PASSWORD			{ $$ = make_str("password"); }
		| PLANS				{ $$ = make_str("plans"); }
		| PREPARE			{ $$ = make_str("prepare"); }
		| PREPARED			{ $$ = make_str("prepared"); }
		| PRESERVE			{ $$ = make_str("preserver"); }
		| PRIOR				{ $$ = make_str("prior"); }
		| PRIVILEGES		{ $$ = make_str("privileges"); }
		| PROCEDURAL		{ $$ = make_str("procedural"); }
		| PROCEDURE			{ $$ = make_str("procedure"); }
		| QUOTE				{ $$ = make_str("quote"); }
		| READ				{ $$ = make_str("read"); }
		| REASSIGN			{ $$ = make_str("reassign"); }
		| RECHECK			{ $$ = make_str("recheck"); }
		| RECURSIVE			{ $$ = make_str("recursive"); }
		| REINDEX			{ $$ = make_str("reindex"); }
		| RELATIVE_P		{ $$ = make_str("relative"); }
		| RELEASE			{ $$ = make_str("release"); }
		| RENAME			{ $$ = make_str("rename"); }
		| REPEATABLE		{ $$ = make_str("repeatable"); }
		| REPLACE			{ $$ = make_str("replace"); }
		| REPLICA			{ $$ = make_str("replica"); }
		| RESET				{ $$ = make_str("reset"); }
		| RESTART			{ $$ = make_str("restart"); }
		| RESTRICT			{ $$ = make_str("restrict"); }
		| RETURNS			{ $$ = make_str("returns"); }
		| REVOKE			{ $$ = make_str("revoke"); }
		| ROLE				{ $$ = make_str("role"); }
		| ROLLBACK			{ $$ = make_str("rollback"); }
		| ROWS				{ $$ = make_str("rows"); }
		| RULE				{ $$ = make_str("rule"); }
		| SAVEPOINT			{ $$ = make_str("savepoint"); }
		| SCHEMA			{ $$ = make_str("schema"); }
		| SCROLL			{ $$ = make_str("scroll"); }
		| SEARCH			{ $$ = make_str("search"); }
/*		| SECOND_P			{ $$ = make_str("second"); }*/
		| SEQUENCE			{ $$ = make_str("sequence"); }
		| SERIALIZABLE		{ $$ = make_str("serializable"); }
		| SESSION			{ $$ = make_str("session"); }
		| SET				{ $$ = make_str("set"); }
		| SHARE				{ $$ = make_str("share"); }
		| SHOW				{ $$ = make_str("show"); }
		| SIMPLE			{ $$ = make_str("simple"); }
		| STABLE			{ $$ = make_str("stable"); }
		| STANDALONE_P			{ $$ = make_str("standalone"); }
		| START				{ $$ = make_str("start"); }
		| STATEMENT			{ $$ = make_str("statement"); }
		| STATISTICS		{ $$ = make_str("statistics"); }
		| STDIN				{ $$ = make_str("stdin"); }
		| STDOUT			{ $$ = make_str("stdout"); }
		| STORAGE			{ $$ = make_str("storage"); }
		| STRICT_P			{ $$ = make_str("strict"); }
		| STRIP_P			{ $$ = make_str("strip"); }
		| SUPERUSER_P		{ $$ = make_str("superuser"); }
		| SYSTEM_P			{ $$ = make_str("system"); }
		| SYSID				{ $$ = make_str("sysid"); }
		| TABLESPACE		{ $$ = make_str("tablespace"); }
		| TEMP				{ $$ = make_str("temp"); }
		| TEMPLATE			{ $$ = make_str("template"); }
		| TEMPORARY			{ $$ = make_str("temporary"); }
		| TEXT_P			{ $$ = make_str("text"); }
		| TRANSACTION		{ $$ = make_str("transaction"); }
		| TRIGGER			{ $$ = make_str("trigger"); }
		| TRUNCATE			{ $$ = make_str("truncate"); }
		| TRUSTED			{ $$ = make_str("trusted"); }
		| TYPE_P			{ $$ = make_str("type"); }
		| UNCOMMITTED		{ $$ = make_str("uncommitted"); }
		| UNENCRYPTED		{ $$ = make_str("unencrypted"); }
		| UNKNOWN			{ $$ = make_str("unknown"); }
		| UNLISTEN			{ $$ = make_str("unlisten"); }
		| UNTIL				{ $$ = make_str("until"); }
		| UPDATE			{ $$ = make_str("update"); }
		| VACUUM			{ $$ = make_str("vacuum"); }
		| VALID				{ $$ = make_str("valid"); }
		| VALIDATOR			{ $$ = make_str("validator"); }
		| VALUE_P			{ $$ = make_str("value"); }
		| VARYING			{ $$ = make_str("varying"); }
		| VERSION_P			{ $$ = make_str("version"); }
		| VIEW				{ $$ = make_str("view"); }
		| VOLATILE			{ $$ = make_str("volatile"); }
		| WHITESPACE_P			{ $$ = make_str("whitespace"); }
		| WITHOUT			{ $$ = make_str("without"); }
		| WORK				{ $$ = make_str("work"); }
		| WRITE  			{ $$ = make_str("write"); }
		| XML_P  			{ $$ = make_str("xml"); }
		| YES_P  			{ $$ = make_str("yes"); }
/*		| YEAR_P			{ $$ = make_str("year"); }*/
		| ZONE				{ $$ = make_str("zone"); }
		;

into_list : coutputvariable | into_list ',' coutputvariable
		;

ecpgstart: SQL_START	{
				reset_variables();
				pacounter = 1;
			}
		;

c_args: /*EMPTY*/		{ $$ = EMPTY; }
		| c_list		{ $$ = $1; }
		;

coutputvariable: cvariable indicator
			{ add_variable_to_head(&argsresult, find_variable($1), find_variable($2)); }
		| cvariable
			{ add_variable_to_head(&argsresult, find_variable($1), &no_indicator); }
		;


civarind: cvariable indicator
		{
			if (find_variable($2)->type->type == ECPGt_array)
				mmerror(PARSE_ERROR, ET_ERROR, "arrays of indicators are not allowed on input");

			add_variable_to_head(&argsinsert, find_variable($1), find_variable($2));
			$$ = create_questionmarks($1, false);
		}
		;

civar: cvariable
		{
			add_variable_to_head(&argsinsert, find_variable($1), &no_indicator);
			$$ = create_questionmarks($1, false);
		}
		;

indicator: cvariable				{ check_indicator((find_variable($1))->type); $$ = $1; }
		| SQL_INDICATOR cvariable	{ check_indicator((find_variable($2))->type); $$ = $2; }
		| SQL_INDICATOR name		{ check_indicator((find_variable($2))->type); $$ = $2; }
		;

cvariable:	CVARIABLE
		{
			/* As long as multidimensional arrays are not implemented we have to check for those here */
			char *ptr = $1;
			int brace_open=0, brace = false;

			for (; *ptr; ptr++)
			{
				switch (*ptr)
				{
					case '[':
							if (brace)
								mmerror(PARSE_ERROR, ET_FATAL, "no multidimensional array support for simple data types");
							brace_open++;
							break;
					case ']':
							brace_open--;
							if (brace_open == 0)
								brace = true;
							break;
					case '\t':
					case ' ':
							break;
					default:
							if (brace_open == 0)
								brace = false;
							break;
				}
			}
			$$ = $1;
		}
		;

ecpg_param:	PARAM		{ $$ = make_name(); } ;

ecpg_bconst:	BCONST		{ $$ = make_name(); } ;

ecpg_fconst:	FCONST		{ $$ = make_name(); } ;

ecpg_real_iconst:
		ICONST		{ $$ = make_name(); } ;

ecpg_iconst:	ecpg_real_iconst	{ $$ = $1; } 
		| civar			{ $$ = $1; }
		;

ecpg_real_sconst:
		SCONST
		{
			/* could have been input as '' or $$ */
			$$ = (char *)mm_alloc(strlen($1) + 3);
			$$[0]='\'';
			strcpy($$+1, $1);
			$$[strlen($1)+1]='\'';
			$$[strlen($1)+2]='\0';
			free($1);
		}
		| ECONST
		{
			$$ = (char *)mm_alloc(strlen($1) + 4);
			$$[0]='E';
			$$[1]='\'';
			strcpy($$+2, $1);
			$$[strlen($1)+2]='\'';
			$$[strlen($1)+3]='\0';
			free($1);
		}
		| NCONST
		{
			$$ = (char *)mm_alloc(strlen($1) + 4);
			$$[0]='N';
			$$[1]='\'';
			strcpy($$+2, $1);
			$$[strlen($1)+2]='\'';
			$$[strlen($1)+3]='\0';
			free($1);
		}
		| UCONST 	{ $$ = $1; }
		| DOLCONST 	{ $$ = $1; }
		;

ecpg_sconst:	ecpg_real_sconst	{ $$ = $1; } 
		| civar			{ $$ = $1; }
		;

ecpg_xconst:	XCONST		{ $$ = make_name(); } ;

ecpg_ident:	IDENT		{ $$ = make_name(); }
		| CSTRING	{ $$ = make3_str(make_str("\""), $1, make_str("\"")) }
		| UIDENT	{ $$ = $1; }
		;

ColId_or_real_sconst:	ColId			{ $$ = $1; }
		|	ecpg_real_sconst	{ $$ = $1; }
		;

quoted_ident_stringvar: name
			{ $$ = make3_str(make_str("\""), $1, make_str("\"")); }
		| char_variable
			{ $$ = make3_str(make_str("("), $1, make_str(")")); }
		;

/*
 * C stuff
 */

c_stuff_item: c_anything			{ $$ = $1; }
		| '(' ')'			{ $$ = make_str("()"); }
		| '(' c_stuff ')'
			{ $$ = cat_str(3, make_str("("), $2, make_str(")")); }
		;

c_stuff: c_stuff_item			{ $$ = $1; }
		| c_stuff c_stuff_item
			{ $$ = cat2_str($1, $2); }
		;

c_list: c_term				{ $$ = $1; }
		| c_list ',' c_term	{ $$ = cat_str(3, $1, make_str(","), $3); }
		;

c_term:  c_stuff			{ $$ = $1; }
		| '{' c_list '}'	{ $$ = cat_str(3, make_str("{"), $2, make_str("}")); }
		;

c_thing:	c_anything		{ $$ = $1; }
		|	'('		{ $$ = make_str("("); }
		|	')'		{ $$ = make_str(")"); }
		|	','		{ $$ = make_str(","); }
		|	';'		{ $$ = make_str(";"); }
		;

c_anything:  ecpg_ident				{ $$ = $1; }
		| ecpg_real_iconst		{ $$ = $1; }
		| ecpg_fconst			{ $$ = $1; }
		| ecpg_real_sconst		{ $$ = $1; }
		| '*'				{ $$ = make_str("*"); }
		| '+'				{ $$ = make_str("+"); }
		| '-'				{ $$ = make_str("-"); }
		| '/'				{ $$ = make_str("/"); }
		| '%'				{ $$ = make_str("%"); }
		| NULL_P			{ $$ = make_str("NULL"); }
		| S_ADD				{ $$ = make_str("+="); }
		| S_AND				{ $$ = make_str("&&"); }
		| S_ANYTHING			{ $$ = make_name(); }
		| S_AUTO			{ $$ = make_str("auto"); }
		| S_CONST			{ $$ = make_str("const"); }
		| S_DEC				{ $$ = make_str("--"); }
		| S_DIV				{ $$ = make_str("/="); }
		| S_DOTPOINT			{ $$ = make_str(".*"); }
		| S_EQUAL			{ $$ = make_str("=="); }
		| S_EXTERN			{ $$ = make_str("extern"); }
		| S_INC				{ $$ = make_str("++"); }
		| S_LSHIFT			{ $$ = make_str("<<"); }
		| S_MEMBER			{ $$ = make_str("->"); }
		| S_MEMPOINT			{ $$ = make_str("->*"); }
		| S_MOD				{ $$ = make_str("%="); }
		| S_MUL				{ $$ = make_str("*="); }
		| S_NEQUAL			{ $$ = make_str("!="); }
		| S_OR				{ $$ = make_str("||"); }
		| S_REGISTER			{ $$ = make_str("register"); }
		| S_RSHIFT			{ $$ = make_str(">>"); }
		| S_STATIC			{ $$ = make_str("static"); }
		| S_SUB				{ $$ = make_str("-="); }
		| S_TYPEDEF			{ $$ = make_str("typedef"); }
		| S_VOLATILE			{ $$ = make_str("volatile"); }
		| SQL_BOOL			{ $$ = make_str("bool"); }
		| ENUM_P			{ $$ = make_str("enum"); }
		| HOUR_P			{ $$ = make_str("hour"); }
		| INT_P				{ $$ = make_str("int"); }
		| SQL_LONG			{ $$ = make_str("long"); }
		| MINUTE_P			{ $$ = make_str("minute"); }
		| MONTH_P			{ $$ = make_str("month"); }
		| SECOND_P			{ $$ = make_str("second"); }
		| SQL_SHORT			{ $$ = make_str("short"); }
		| SQL_SIGNED			{ $$ = make_str("signed"); }
		| SQL_STRUCT			{ $$ = make_str("struct"); }
		| SQL_UNSIGNED			{ $$ = make_str("unsigned"); }
		| YEAR_P			{ $$ = make_str("year"); }
		| CHAR_P			{ $$ = make_str("char"); }
		| FLOAT_P			{ $$ = make_str("float"); }
		| TO				{ $$ = make_str("to"); }
		| UNION				{ $$ = make_str("union"); }
		| VARCHAR			{ $$ = make_str("varchar"); }
		| '['				{ $$ = make_str("["); }
		| ']'				{ $$ = make_str("]"); }
		| '='				{ $$ = make_str("="); }
		| ':' 				{ $$ = make_str(":"); }
		;

DeallocateStmt: DEALLOCATE prepared_name                { $$ = $2; }
                | DEALLOCATE PREPARE prepared_name      { $$ = $3; }
                | DEALLOCATE ALL                        { $$ = make_str("all"); }
                | DEALLOCATE PREPARE ALL                { $$ = make_str("all"); }
                ;

Iresult:        ecpg_iconst		{ $$ = $1; }
                | '(' Iresult ')'       { $$ = cat_str(3, make_str("("), $2, make_str(")")); }
                | Iresult '+' Iresult   { $$ = cat_str(3, $1, make_str("+"), $3); }
                | Iresult '-' Iresult   { $$ = cat_str(3, $1, make_str("-"), $3); }
                | Iresult '*' Iresult   { $$ = cat_str(3, $1, make_str("*"), $3); }
                | Iresult '/' Iresult   { $$ = cat_str(3, $1, make_str("/"), $3); }
                | Iresult '%' Iresult   { $$ = cat_str(3, $1, make_str("%"), $3); }
                | ecpg_real_sconst	{ $$ = $1; }
                | ColId                 { $$ = $1; }
                ;

execute_rest: /* EMPTY */	{ $$ = EMPTY; }
	| ecpg_using ecpg_into  { $$ = EMPTY; }
	| ecpg_into ecpg_using  { $$ = EMPTY; }
	| ecpg_using            { $$ = EMPTY; }
	| ecpg_into             { $$ = EMPTY; }
	; 

ecpg_into: INTO into_list	{ $$ = EMPTY; }
        | into_descriptor	{ $$ = $1; }
	;

%%

void base_yyerror(const char * error)
{
	char buf[1024];

	snprintf(buf,sizeof buf, _("%s at or near \"%s\""), error, token_start ? token_start : yytext);
	buf[sizeof(buf)-1]=0;
	mmerror(PARSE_ERROR, ET_ERROR, buf);
}

void parser_init(void)
{
 /* This function is empty. It only exists for compatibility with the backend parser right now. */
}

/*
 * Must undefine base_yylex before including pgc.c, since we want it
 * to create the function base_yylex not filtered_base_yylex.
 */
#undef base_yylex

#include "pgc.c"
