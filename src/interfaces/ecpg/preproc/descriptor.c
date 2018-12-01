/*
 * functions needed for descriptor handling
 *
 * src/interfaces/ecpg/preproc/descriptor.c
 *
 * since descriptor might be either a string constant or a string var
 * we need to check for a constant if we expect a constant
 */

#include "postgres_fe.h"

#include "preproc_extern.h"

/*
 * assignment handling function (descriptor)
 */

static struct assignment *assignments;

void
push_assignment(char *var, enum ECPGdtype value)
{
	struct assignment *new = (struct assignment *) mm_alloc(sizeof(struct assignment));

	new->next = assignments;
	new->variable = mm_alloc(strlen(var) + 1);
	strcpy(new->variable, var);
	new->value = value;
	assignments = new;
}

static void
drop_assignments(void)
{
	while (assignments)
	{
		struct assignment *old_head = assignments;

		assignments = old_head->next;
		free(old_head->variable);
		free(old_head);
	}
}

static void
ECPGnumeric_lvalue(char *name)
{
	const struct variable *v = find_variable(name);

	switch (v->type->type)
	{
		case ECPGt_short:
		case ECPGt_int:
		case ECPGt_long:
		case ECPGt_long_long:
		case ECPGt_unsigned_short:
		case ECPGt_unsigned_int:
		case ECPGt_unsigned_long:
		case ECPGt_unsigned_long_long:
		case ECPGt_const:
			fputs(name, base_yyout);
			break;
		default:
			mmerror(PARSE_ERROR, ET_ERROR, "variable \"%s\" must have a numeric type", name);
			break;
	}
}

/*
 * descriptor name lookup
 */

static struct descriptor *descriptors;

void
add_descriptor(char *name, char *connection)
{
	struct descriptor *new;

	if (name[0] != '"')
		return;

	new = (struct descriptor *) mm_alloc(sizeof(struct descriptor));

	new->next = descriptors;
	new->name = mm_alloc(strlen(name) + 1);
	strcpy(new->name, name);
	if (connection)
	{
		new->connection = mm_alloc(strlen(connection) + 1);
		strcpy(new->connection, connection);
	}
	else
		new->connection = connection;
	descriptors = new;
}

void
drop_descriptor(char *name, char *connection)
{
	struct descriptor *i;
	struct descriptor **lastptr = &descriptors;

	if (name[0] != '"')
		return;

	for (i = descriptors; i; lastptr = &i->next, i = i->next)
	{
		if (strcmp(name, i->name) == 0)
		{
			if ((!connection && !i->connection)
				|| (connection && i->connection
					&& strcmp(connection, i->connection) == 0))
			{
				*lastptr = i->next;
				if (i->connection)
					free(i->connection);
				free(i->name);
				free(i);
				return;
			}
		}
	}
	mmerror(PARSE_ERROR, ET_WARNING, "descriptor \"%s\" does not exist", name);
}

struct descriptor
		   *
lookup_descriptor(char *name, char *connection)
{
	struct descriptor *i;

	if (name[0] != '"')
		return NULL;

	for (i = descriptors; i; i = i->next)
	{
		if (strcmp(name, i->name) == 0)
		{
			if ((!connection && !i->connection)
				|| (connection && i->connection
					&& strcmp(connection, i->connection) == 0))
				return i;
		}
	}
	mmerror(PARSE_ERROR, ET_WARNING, "descriptor \"%s\" does not exist", name);
	return NULL;
}

void
output_get_descr_header(char *desc_name)
{
	struct assignment *results;

	fprintf(base_yyout, "{ ECPGget_desc_header(__LINE__, %s, &(", desc_name);
	for (results = assignments; results != NULL; results = results->next)
	{
		if (results->value == ECPGd_count)
			ECPGnumeric_lvalue(results->variable);
		else
			mmerror(PARSE_ERROR, ET_WARNING, "descriptor header item \"%d\" does not exist", results->value);
	}

	drop_assignments();
	fprintf(base_yyout, "));\n");
	whenever_action(3);
}

void
output_get_descr(char *desc_name, char *index)
{
	struct assignment *results;

	fprintf(base_yyout, "{ ECPGget_desc(__LINE__, %s, %s,", desc_name, index);
	for (results = assignments; results != NULL; results = results->next)
	{
		const struct variable *v = find_variable(results->variable);
		char	   *str_zero = mm_strdup("0");

		switch (results->value)
		{
			case ECPGd_nullable:
				mmerror(PARSE_ERROR, ET_WARNING, "nullable is always 1");
				break;
			case ECPGd_key_member:
				mmerror(PARSE_ERROR, ET_WARNING, "key_member is always 0");
				break;
			default:
				break;
		}
		fprintf(base_yyout, "%s,", get_dtype(results->value));
		ECPGdump_a_type(base_yyout, v->name, v->type, v->brace_level,
						NULL, NULL, -1, NULL, NULL, str_zero, NULL, NULL);
		free(str_zero);
	}
	drop_assignments();
	fputs("ECPGd_EODT);\n", base_yyout);

	whenever_action(2 | 1);
}

void
output_set_descr_header(char *desc_name)
{
	struct assignment *results;

	fprintf(base_yyout, "{ ECPGset_desc_header(__LINE__, %s, (int)(", desc_name);
	for (results = assignments; results != NULL; results = results->next)
	{
		if (results->value == ECPGd_count)
			ECPGnumeric_lvalue(results->variable);
		else
			mmerror(PARSE_ERROR, ET_WARNING, "descriptor header item \"%d\" does not exist", results->value);
	}

	drop_assignments();
	fprintf(base_yyout, "));\n");
	whenever_action(3);
}

static const char *
descriptor_item_name(enum ECPGdtype itemcode)
{
	switch (itemcode)
	{
		case ECPGd_cardinality:
			return "CARDINALITY";
		case ECPGd_count:
			return "COUNT";
		case ECPGd_data:
			return "DATA";
		case ECPGd_di_code:
			return "DATETIME_INTERVAL_CODE";
		case ECPGd_di_precision:
			return "DATETIME_INTERVAL_PRECISION";
		case ECPGd_indicator:
			return "INDICATOR";
		case ECPGd_key_member:
			return "KEY_MEMBER";
		case ECPGd_length:
			return "LENGTH";
		case ECPGd_name:
			return "NAME";
		case ECPGd_nullable:
			return "NULLABLE";
		case ECPGd_octet:
			return "OCTET_LENGTH";
		case ECPGd_precision:
			return "PRECISION";
		case ECPGd_ret_length:
			return "RETURNED_LENGTH";
		case ECPGd_ret_octet:
			return "RETURNED_OCTET_LENGTH";
		case ECPGd_scale:
			return "SCALE";
		case ECPGd_type:
			return "TYPE";
		default:
			return NULL;
	}
}

void
output_set_descr(char *desc_name, char *index)
{
	struct assignment *results;

	fprintf(base_yyout, "{ ECPGset_desc(__LINE__, %s, %s,", desc_name, index);
	for (results = assignments; results != NULL; results = results->next)
	{
		const struct variable *v = find_variable(results->variable);

		switch (results->value)
		{
			case ECPGd_cardinality:
			case ECPGd_di_code:
			case ECPGd_di_precision:
			case ECPGd_precision:
			case ECPGd_scale:
				mmfatal(PARSE_ERROR, "descriptor item \"%s\" is not implemented",
						descriptor_item_name(results->value));
				break;

			case ECPGd_key_member:
			case ECPGd_name:
			case ECPGd_nullable:
			case ECPGd_octet:
			case ECPGd_ret_length:
			case ECPGd_ret_octet:
				mmfatal(PARSE_ERROR, "descriptor item \"%s\" cannot be set",
						descriptor_item_name(results->value));
				break;

			case ECPGd_data:
			case ECPGd_indicator:
			case ECPGd_length:
			case ECPGd_type:
				{
					char	   *str_zero = mm_strdup("0");

					fprintf(base_yyout, "%s,", get_dtype(results->value));
					ECPGdump_a_type(base_yyout, v->name, v->type, v->brace_level,
									NULL, NULL, -1, NULL, NULL, str_zero, NULL, NULL);
					free(str_zero);
				}
				break;

			default:
				;
		}
	}
	drop_assignments();
	fputs("ECPGd_EODT);\n", base_yyout);

	whenever_action(2 | 1);
}

/* I consider dynamic allocation overkill since at most two descriptor
   variables are possible per statement. (input and output descriptor)
   And descriptors are no normal variables, so they don't belong into
   the variable list.
*/

#define MAX_DESCRIPTOR_NAMELEN 128
struct variable *
descriptor_variable(const char *name, int input)
{
	static char descriptor_names[2][MAX_DESCRIPTOR_NAMELEN];
	static struct ECPGtype descriptor_type = {ECPGt_descriptor, NULL, NULL, NULL, {NULL}, 0};
	static struct variable varspace[2] = {
		{descriptor_names[0], &descriptor_type, 0, NULL},
		{descriptor_names[1], &descriptor_type, 0, NULL}
	};

	strlcpy(descriptor_names[input], name, sizeof(descriptor_names[input]));
	return &varspace[input];
}

struct variable *
sqlda_variable(const char *name)
{
	struct variable *p = (struct variable *) mm_alloc(sizeof(struct variable));

	p->name = mm_strdup(name);
	p->type = (struct ECPGtype *) mm_alloc(sizeof(struct ECPGtype));
	p->type->type = ECPGt_sqlda;
	p->type->size = NULL;
	p->type->struct_sizeof = NULL;
	p->type->u.element = NULL;
	p->type->counter = 0;
	p->brace_level = 0;
	p->next = NULL;

	return p;
}
