/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/variables.c,v 1.13 2003/08/04 23:59:40 tgl Exp $
 */
#include "postgres_fe.h"
#include "variables.h"

#include <assert.h>


VariableSpace
CreateVariableSpace(void)
{
	struct _variable *ptr;

	ptr = calloc(1, sizeof *ptr);
	if (!ptr)
		return NULL;

	ptr->name = strdup("@");
	ptr->value = strdup("");
	if (!ptr->name || !ptr->value)
	{
		free(ptr->name);
		free(ptr->value);
		free(ptr);
		return NULL;
	}

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
#ifdef USE_ASSERT_CHECKING
		assert(current->name);
		assert(current->value);
#endif
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
	if (strcmp(val, "off") == 0)
		return false;

	/*
	 * for backwards compatibility, anything except "off" is taken as
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
#ifdef USE_ASSERT_CHECKING
		assert(current->name);
		assert(current->value);
#endif
		if (strcmp(current->name, name) == 0)
		{
			free(current->value);
			current->value = strdup(value);
			return current->value ? true : false;
		}
	}

	previous->next = calloc(1, sizeof *(previous->next));
	if (!previous->next)
		return false;
	previous->next->name = strdup(name);
	if (!previous->next->name)
		return false;
	previous->next->value = strdup(value);
	return previous->next->value ? true : false;
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
#ifdef USE_ASSERT_CHECKING
		assert(current->name);
		assert(current->value);
#endif
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
