/*
 * functions needed for descriptor handling
 *
 * since descriptor might be either a string constant or a string var
 * we need to check for a constant if we expect a constant
 */

#include "postgres_fe.h"

#include "extern.h"

/*
 * assignment handling function (descriptor)
 */

struct assignment *assignments;

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
ECPGnumeric_lvalue(FILE *f, char *name)
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
			fputs(name, yyout);
			break;
		default:
			snprintf(errortext, sizeof errortext, "variable %s: numeric type needed"
					 ,name);
			mmerror(PARSE_ERROR, ET_ERROR, errortext);
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
		if (!strcmp(name, i->name))
		{
			if ((!connection && !i->connection)
				|| (connection && i->connection
					&& !strcmp(connection, i->connection)))
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
	snprintf(errortext, sizeof errortext, "unknown descriptor %s", name);
	mmerror(PARSE_ERROR, ET_WARNING, errortext);
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
		if (!strcmp(name, i->name))
		{
			if ((!connection && !i->connection)
				|| (connection && i->connection
					&& !strcmp(connection, i->connection)))
				return i;
		}
	}
	snprintf(errortext, sizeof errortext, "unknown descriptor %s", name);
	mmerror(PARSE_ERROR, ET_WARNING, errortext);
	return NULL;
}

void
output_get_descr_header(char *desc_name)
{
	struct assignment *results;

	fprintf(yyout, "{ ECPGget_desc_header(%d, %s, &(", yylineno, desc_name);
	for (results = assignments; results != NULL; results = results->next)
	{
		if (results->value == ECPGd_count)
			ECPGnumeric_lvalue(yyout, results->variable);
		else
		{
			snprintf(errortext, sizeof errortext, "unknown descriptor header item '%d'", results->value);
			mmerror(PARSE_ERROR, ET_WARNING, errortext);
		}
	}

	drop_assignments();
	fprintf(yyout, "));\n");
	whenever_action(3);
}

void
output_get_descr(char *desc_name, char *index)
{
	struct assignment *results;

	fprintf(yyout, "{ ECPGget_desc(%d, %s, %s,", yylineno, desc_name, index);
	for (results = assignments; results != NULL; results = results->next)
	{
		const struct variable *v = find_variable(results->variable);

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
		fprintf(yyout, "%s,", get_dtype(results->value));
		ECPGdump_a_type(yyout, v->name, v->type, NULL, NULL, NULL, NULL, make_str("0"), NULL, NULL);
	}
	drop_assignments();
	fputs("ECPGd_EODT);\n", yyout);

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
	static const struct ECPGtype descriptor_type =
	{ECPGt_descriptor, 0};
	static const struct variable varspace[2] =
	{{descriptor_names[0], (struct ECPGtype *) & descriptor_type, 0, NULL},
	{descriptor_names[1], (struct ECPGtype *) & descriptor_type, 0, NULL}
	};

	strncpy(descriptor_names[input], name, MAX_DESCRIPTOR_NAMELEN);
	descriptor_names[input][MAX_DESCRIPTOR_NAMELEN - 1] = 0;
	return (struct variable *) & varspace[input];
}
