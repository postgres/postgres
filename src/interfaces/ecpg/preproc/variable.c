#include "postgres_fe.h"

#include "extern.h"

struct variable *allvariables = NULL;

struct variable *
new_variable(const char *name, struct ECPGtype * type)
{
	struct variable *p = (struct variable *) mm_alloc(sizeof(struct variable));

	p->name = mm_strdup(name);
	p->type = type;
	p->brace_level = braces_open;

	p->next = allvariables;
	allvariables = p;

	return (p);
}

static struct variable *
find_struct_member(char *name, char *str, struct ECPGstruct_member * members)
{
	char	   *next = strchr(++str, '.'),
				c = '\0';

	if (next != NULL)
	{
		c = *next;
		*next = '\0';
	}

	for (; members; members = members->next)
	{
		if (strcmp(members->name, str) == 0)
		{
			if (c == '\0')
			{
				/* found the end */
				switch (members->typ->typ)
				{
					case ECPGt_array:
						return (new_variable(name, ECPGmake_array_type(members->typ->u.element, members->typ->size)));
					case ECPGt_struct:
					case ECPGt_union:
						return (new_variable(name, ECPGmake_struct_type(members->typ->u.members, members->typ->typ)));
					default:
						return (new_variable(name, ECPGmake_simple_type(members->typ->typ, members->typ->size)));
				}
			}
			else
			{
				*next = c;
				if (c == '-')
				{
					next++;
					return (find_struct_member(name, next, members->typ->u.element->u.members));
				}
				else
					return (find_struct_member(name, next, members->typ->u.members));
			}
		}
	}

	return (NULL);
}

static struct variable *
find_struct(char *name, char *next)
{
	struct variable *p;
	char		c = *next;

	/* first get the mother structure entry */
	*next = '\0';
	p = find_variable(name);

	if (c == '-')
	{
		if (p->type->typ != ECPGt_array)
		{
			sprintf(errortext, "variable %s is not a pointer", name);
			mmerror(ET_FATAL, errortext);
		}

		if (p->type->u.element->typ != ECPGt_struct && p->type->u.element->typ != ECPGt_union)
		{
			sprintf(errortext, "variable %s is not a pointer to a structure or a union", name);
			mmerror(ET_FATAL, errortext);
		}

		/* restore the name, we will need it later on */
		*next = c;
		next++;

		return find_struct_member(name, next, p->type->u.element->u.members);
	}
	else
	{
		if (p->type->typ != ECPGt_struct && p->type->typ != ECPGt_union)
		{
			sprintf(errortext, "variable %s is neither a structure nor a union", name);
			mmerror(ET_FATAL, errortext);
		}

		/* restore the name, we will need it later on */
		*next = c;

		return find_struct_member(name, next, p->type->u.members);
	}
}

static struct variable *
find_simple(char *name)
{
	struct variable *p;

	for (p = allvariables; p; p = p->next)
	{
		if (strcmp(p->name, name) == 0)
			return p;
	}

	return (NULL);
}

/* Note that this function will end the program in case of an unknown */
/* variable */
struct variable *
find_variable(char *name)
{
	char	   *next;
	struct variable *p;

	if ((next = strchr(name, '.')) != NULL)
		p = find_struct(name, next);
	else if ((next = strstr(name, "->")) != NULL)
		p = find_struct(name, next);
	else
		p = find_simple(name);

	if (p == NULL)
	{
		sprintf(errortext, "The variable %s is not declared", name);
		mmerror(ET_FATAL, errortext);
	}

	return (p);
}

void
remove_variables(int brace_level)
{
	struct variable *p,
			   *prev;

	for (p = prev = allvariables; p; p = p ? p->next : NULL)
	{
		if (p->brace_level >= brace_level)
		{
			/* remove it */
			if (p == allvariables)
				prev = allvariables = p->next;
			else
				prev->next = p->next;

			ECPGfree_type(p->type);
			free(p->name);
			free(p);
			p = prev;
		}
		else
			prev = p;
	}
}


/*
 * Here are the variables that need to be handled on every request.
 * These are of two kinds: input and output.
 * I will make two lists for them.
 */

struct arguments *argsinsert = NULL;
struct arguments *argsresult = NULL;

void
reset_variables(void)
{
	argsinsert = NULL;
	argsresult = NULL;
}

/* Insert a new variable into our request list. */
void
add_variable(struct arguments ** list, struct variable * var, struct variable * ind)
{
	struct arguments *p = (struct arguments *) mm_alloc(sizeof(struct arguments));

	p->variable = var;
	p->indicator = ind;
	p->next = *list;
	*list = p;
}

/* Append a new variable to our request list. */
void
append_variable(struct arguments ** list, struct variable * var, struct variable * ind)
{
	struct arguments *p,
			   *new = (struct arguments *) mm_alloc(sizeof(struct arguments));

	for (p = *list; p && p->next; p = p->next);

	new->variable = var;
	new->indicator = ind;
	new->next = NULL;

	if (p)
		p->next = new;
	else
		*list = new;
}

/* Dump out a list of all the variable on this list.
   This is a recursive function that works from the end of the list and
   deletes the list as we go on.
 */
void
dump_variables(struct arguments * list, int mode)
{
	if (list == NULL)
		return;

	/*
	 * The list is build up from the beginning so lets first dump the end
	 * of the list:
	 */

	dump_variables(list->next, mode);

	/* Then the current element and its indicator */
	ECPGdump_a_type(yyout, list->variable->name, list->variable->type,
			   list->indicator->name, list->indicator->type, NULL, NULL);

	/* Then release the list element. */
	if (mode != 0)
		free(list);
}

void
check_indicator(struct ECPGtype * var)
{
	/* make sure this is a valid indicator variable */
	switch (var->typ)
	{
			struct ECPGstruct_member *p;

		case ECPGt_short:
		case ECPGt_int:
		case ECPGt_long:
		case ECPGt_long_long:
		case ECPGt_unsigned_short:
		case ECPGt_unsigned_int:
		case ECPGt_unsigned_long:
		case ECPGt_unsigned_long_long:
			break;

		case ECPGt_struct:
		case ECPGt_union:
			for (p = var->u.members; p; p = p->next)
				check_indicator(p->typ);
			break;

		case ECPGt_array:
			check_indicator(var->u.element);
			break;
		default:
			mmerror(ET_ERROR, "indicator variable must be integer type");
			break;
	}
}

struct typedefs *
get_typedef(char *name)
{
	struct typedefs *this;

	for (this = types; this && strcmp(this->name, name); this = this->next);
	if (!this)
	{
		sprintf(errortext, "invalid datatype '%s'", name);
		mmerror(ET_FATAL, errortext);
	}

	return (this);
}

void
adjust_array(enum ECPGttype type_enum, int *dimension, int *length, int type_dimension, int type_index, bool pointer)
{
	if (type_index >= 0)
	{
		if (*length >= 0)
			mmerror(ET_FATAL, "No multi-dimensional array support");

		*length = type_index;
	}

	if (type_dimension >= 0)
	{
		if (*dimension >= 0 && *length >= 0)
			mmerror(ET_FATAL, "No multi-dimensional array support");

		if (*dimension >= 0)
			*length = *dimension;

		*dimension = type_dimension;
	}

	if (*length >= 0 && *dimension >= 0 && pointer)
		mmerror(ET_FATAL, "No multi-dimensional array support");

	switch (type_enum)
	{
		case ECPGt_struct:
		case ECPGt_union:
			/* pointer has to get dimension 0 */
			if (pointer)
			{
				*length = *dimension;
				*dimension = 0;
			}

			if (*length >= 0)
				mmerror(ET_FATAL, "No multi-dimensional array support for structures");

			break;
		case ECPGt_varchar:
			/* pointer has to get dimension 0 */
			if (pointer)
				*dimension = 0;

			/* one index is the string length */
			if (*length < 0)
			{
				*length = *dimension;
				*dimension = -1;
			}

			break;
		case ECPGt_char:
		case ECPGt_unsigned_char:
			/* pointer has to get length 0 */
			if (pointer)
				*length = 0;

			/* one index is the string length */
			if (*length < 0)
			{
				*length = (*dimension < 0) ? 1 : *dimension;
				*dimension = -1;
			}

			break;
		default:
			/* a pointer has dimension = 0 */
			if (pointer)
			{
				*length = *dimension;
				*dimension = 0;
			}

			if (*length >= 0)
				mmerror(ET_FATAL, "No multi-dimensional array support for simple data types");

			break;
	}
}
