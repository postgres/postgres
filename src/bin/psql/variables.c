/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/variables.c,v 1.22 2005/10/15 02:49:41 momjian Exp $
 */
#include "postgres_fe.h"
#include "common.h"
#include "variables.h"

VariableSpace
CreateVariableSpace(void)
{
	struct _variable *ptr;

	ptr = pg_calloc(1, sizeof *ptr);
	ptr->name = pg_strdup("@");
	ptr->value = pg_strdup("");

	return ptr;
}

const char *
GetVariable(VariableSpace space, const char *name)
{
	struct _variable *current;

	if (!space)
		return NULL;

	if (strspn(name, VALID_VARIABLE_CHARS) != strlen(name))
		return NULL;

	for (current = space; current; current = current->next)
	{
		psql_assert(current->name);
		psql_assert(current->value);
		if (strcmp(current->name, name) == 0)
			return current->value;
	}

	return NULL;
}

bool
GetVariableBool(VariableSpace space, const char *name)
{
	const char *val;

	val = GetVariable(space, name);
	if (val == NULL)
		return false;			/* not set -> assume "off" */
	if (pg_strcasecmp(val, "off") == 0)
		return false;			/* accept "off" or "OFF" as true */

	/*
	 * for backwards compatibility, anything except "off" or "OFF" is taken as
	 * "true"
	 */
	return true;
}

bool
VariableEquals(VariableSpace space, const char name[], const char value[])
{
	const char *var;

	var = GetVariable(space, name);
	return var && (strcmp(var, value) == 0);
}

int
GetVariableNum(VariableSpace space,
			   const char name[],
			   int defaultval,
			   int faultval,
			   bool allowtrail)
{
	const char *var;
	int			result;

	var = GetVariable(space, name);
	if (!var)
		result = defaultval;
	else if (!var[0])
		result = faultval;
	else
	{
		char	   *end;

		result = strtol(var, &end, 0);
		if (!allowtrail && *end)
			result = faultval;
	}

	return result;
}

int
SwitchVariable(VariableSpace space, const char name[], const char *opt,...)
{
	int			result;
	const char *var;

	var = GetVariable(space, name);
	if (var)
	{
		va_list		args;

		va_start(args, opt);
		for (result = 1; opt && (strcmp(var, opt) != 0); result++)
			opt = va_arg(args, const char *);
		if (!opt)
			result = VAR_NOTFOUND;
		va_end(args);
	}
	else
		result = VAR_NOTSET;

	return result;
}

void
PrintVariables(VariableSpace space)
{
	struct _variable *ptr;

	for (ptr = space->next; ptr; ptr = ptr->next)
		printf("%s = '%s'\n", ptr->name, ptr->value);
}

bool
SetVariable(VariableSpace space, const char *name, const char *value)
{
	struct _variable *current,
			   *previous;

	if (!space)
		return false;

	if (!value)
		return DeleteVariable(space, name);

	if (strspn(name, VALID_VARIABLE_CHARS) != strlen(name))
		return false;

	for (current = space, previous = NULL; current; previous = current, current = current->next)
	{
		psql_assert(current->name);
		psql_assert(current->value);
		if (strcmp(current->name, name) == 0)
		{
			free(current->value);
			current->value = pg_strdup(value);
			return true;
		}
	}

	previous->next = pg_calloc(1, sizeof *(previous->next));
	previous->next->name = pg_strdup(name);
	previous->next->value = pg_strdup(value);
	return true;
}

bool
SetVariableBool(VariableSpace space, const char *name)
{
	return SetVariable(space, name, "on");
}

bool
DeleteVariable(VariableSpace space, const char *name)
{
	struct _variable *current,
			   *previous;

	if (!space)
		return false;

	if (strspn(name, VALID_VARIABLE_CHARS) != strlen(name))
		return false;

	for (current = space, previous = NULL; current; previous = current, current = current->next)
	{
		psql_assert(current->name);
		psql_assert(current->value);
		if (strcmp(current->name, name) == 0)
		{
			free(current->name);
			free(current->value);
			if (previous)
				previous->next = current->next;
			free(current);
			return true;
		}
	}

	return true;
}
