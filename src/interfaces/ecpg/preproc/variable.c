#include "postgres_fe.h"

#include "extern.h"

struct variable *allvariables = NULL;

struct variable *
new_variable(const char *name, struct ECPGtype * type, int brace_level)
{
	struct variable *p = (struct variable *) mm_alloc(sizeof(struct variable));

	p->name = mm_strdup(name);
	p->type = type;
	p->brace_level = brace_level;

	p->next = allvariables;
	allvariables = p;

	return (p);
}

static struct variable *
find_struct_member(char *name, char *str, struct ECPGstruct_member * members, int brace_level)
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
				switch (members->type->type)
				{
					case ECPGt_array:
						return (new_variable(name, ECPGmake_array_type(members->type->u.element, members->type->size), brace_level));
					case ECPGt_struct:
					case ECPGt_union:
						return (new_variable(name, ECPGmake_struct_type(members->type->u.members, members->type->type, members->type->struct_sizeof), brace_level));
					default:
						return (new_variable(name, ECPGmake_simple_type(members->type->type, members->type->size), brace_level));
				}
			}
			else
			{
				*next = c;
				if (c == '-')
				{
					next++;
					return (find_struct_member(name, next, members->type->u.element->u.members, brace_level));
				}
				else
					return (find_struct_member(name, next, members->type->u.members, brace_level));
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
		if (p->type->type != ECPGt_array)
		{
			sprintf(errortext, "variable %s is not a pointer", name);
			mmerror(PARSE_ERROR, ET_FATAL, errortext);
		}

		if (p->type->u.element->type != ECPGt_struct && p->type->u.element->type != ECPGt_union)
		{
			sprintf(errortext, "variable %s is not a pointer to a structure or a union", name);
			mmerror(PARSE_ERROR, ET_FATAL, errortext);
		}

		/* restore the name, we will need it later on */
		*next = c;
		next++;

		return find_struct_member(name, next, p->type->u.element->u.members, p->brace_level);
	}
	else
	{
		if (p->type->type != ECPGt_struct && p->type->type != ECPGt_union)
		{
			sprintf(errortext, "variable %s is neither a structure nor a union", name);
			mmerror(PARSE_ERROR, ET_FATAL, errortext);
		}

		/* restore the name, we will need it later on */
		*next = c;

		return find_struct_member(name, next, p->type->u.members, p->brace_level);
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
		mmerror(PARSE_ERROR, ET_FATAL, errortext);
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
			   list->indicator->name, list->indicator->type, NULL, NULL, 0, NULL, NULL);

	/* Then release the list element. */
	if (mode != 0)
		free(list);
}

void
check_indicator(struct ECPGtype * var)
{
	/* make sure this is a valid indicator variable */
	switch (var->type)
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
				check_indicator(p->type);
			break;

		case ECPGt_array:
			check_indicator(var->u.element);
			break;
		default:
			mmerror(PARSE_ERROR, ET_ERROR, "indicator variable must be integer type");
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
		mmerror(PARSE_ERROR, ET_FATAL, errortext);
	}

	return (this);
}

void
adjust_array(enum ECPGttype type_enum, int *dimension, int *length, int type_dimension, int type_index, int pointer_len)
{
	if (type_index >= 0)
	{
		if (*length >= 0)
			mmerror(PARSE_ERROR, ET_FATAL, "No multi-dimensional array support");

		*length = type_index;
	}

	if (type_dimension >= 0)
	{
		if (*dimension >= 0 && *length >= 0)
			mmerror(PARSE_ERROR, ET_FATAL, "No multi-dimensional array support");

		if (*dimension >= 0)
			*length = *dimension;

		*dimension = type_dimension;
	}
	
	if (pointer_len>2)
	{	sprintf(errortext, "No multilevel (more than 2) pointer supported %d",pointer_len);
	    mmerror(PARSE_ERROR, ET_FATAL, errortext);
/*		mmerror(PARSE_ERROR, ET_FATAL, "No multilevel (more than 2) pointer supported %d",pointer_len);*/
	}
	if (pointer_len>1 && type_enum!=ECPGt_char && type_enum!=ECPGt_unsigned_char)
		mmerror(PARSE_ERROR, ET_FATAL, "No pointer to pointer supported for this type");

	if (pointer_len>1 && (*length >= 0 || *dimension >= 0))
		mmerror(PARSE_ERROR, ET_FATAL, "No multi-dimensional array support");

	if (*length >= 0 && *dimension >= 0 && pointer_len)
		mmerror(PARSE_ERROR, ET_FATAL, "No multi-dimensional array support");

	switch (type_enum)
	{
		case ECPGt_struct:
		case ECPGt_union:
			/* pointer has to get dimension 0 */
			if (pointer_len)
			{
				*length = *dimension;
				*dimension = 0;
			}

			if (*length >= 0)
				mmerror(PARSE_ERROR, ET_FATAL, "No multi-dimensional array support for structures");

			break;
		case ECPGt_varchar:
			/* pointer has to get dimension 0 */
			if (pointer_len)
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
			/* char ** */
			if (pointer_len==2)
			{
				*length = *dimension = 0;
				break;
			}
			
			/* pointer has to get length 0 */
			if (pointer_len==1)
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
			if (pointer_len)
			{
				*length = *dimension;
				*dimension = 0;
			}

			if (*length >= 0)
				mmerror(PARSE_ERROR, ET_FATAL, "No multi-dimensional array support for simple data types");

			break;
	}
}
