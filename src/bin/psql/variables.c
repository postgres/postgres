/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2017, PostgreSQL Global Development Group
 *
 * src/bin/psql/variables.c
 */
#include "postgres_fe.h"

#include "common.h"
#include "variables.h"


/*
 * Check whether a variable's name is allowed.
 *
 * We allow any non-ASCII character, as well as ASCII letters, digits, and
 * underscore.  Keep this in sync with the definition of variable_char in
 * psqlscan.l and psqlscanslash.l.
 */
static bool
valid_variable_name(const char *name)
{
	const unsigned char *ptr = (const unsigned char *) name;

	/* Mustn't be zero-length */
	if (*ptr == '\0')
		return false;

	while (*ptr)
	{
		if (IS_HIGHBIT_SET(*ptr) ||
			strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ" "abcdefghijklmnopqrstuvwxyz"
				   "_0123456789", *ptr) != NULL)
			ptr++;
		else
			return false;
	}

	return true;
}

/*
 * A "variable space" is represented by an otherwise-unused struct _variable
 * that serves as list header.
 */
VariableSpace
CreateVariableSpace(void)
{
	struct _variable *ptr;

	ptr = pg_malloc(sizeof *ptr);
	ptr->name = NULL;
	ptr->value = NULL;
	ptr->assign_hook = NULL;
	ptr->next = NULL;

	return ptr;
}

/*
 * Get string value of variable, or NULL if it's not defined.
 *
 * Note: result is valid until variable is next assigned to.
 */
const char *
GetVariable(VariableSpace space, const char *name)
{
	struct _variable *current;

	if (!space)
		return NULL;

	for (current = space->next; current; current = current->next)
	{
		if (strcmp(current->name, name) == 0)
		{
			/* this is correct answer when value is NULL, too */
			return current->value;
		}
	}

	return NULL;
}

/*
 * Try to interpret "value" as a boolean value, and if successful,
 * store it in *result.  Otherwise don't clobber *result.
 *
 * Valid values are: true, false, yes, no, on, off, 1, 0; as well as unique
 * prefixes thereof.
 *
 * "name" is the name of the variable we're assigning to, to use in error
 * report if any.  Pass name == NULL to suppress the error report.
 *
 * Return true when "value" is syntactically valid, false otherwise.
 */
bool
ParseVariableBool(const char *value, const char *name, bool *result)
{
	size_t		len;
	bool		valid = true;

	if (value == NULL)
	{
		*result = false;		/* not set -> assume "off" */
		return valid;
	}

	len = strlen(value);

	if (len > 0 && pg_strncasecmp(value, "true", len) == 0)
		*result = true;
	else if (len > 0 && pg_strncasecmp(value, "false", len) == 0)
		*result = false;
	else if (len > 0 && pg_strncasecmp(value, "yes", len) == 0)
		*result = true;
	else if (len > 0 && pg_strncasecmp(value, "no", len) == 0)
		*result = false;
	/* 'o' is not unique enough */
	else if (pg_strncasecmp(value, "on", (len > 2 ? len : 2)) == 0)
		*result = true;
	else if (pg_strncasecmp(value, "off", (len > 2 ? len : 2)) == 0)
		*result = false;
	else if (pg_strcasecmp(value, "1") == 0)
		*result = true;
	else if (pg_strcasecmp(value, "0") == 0)
		*result = false;
	else
	{
		/* string is not recognized; don't clobber *result */
		if (name)
			psql_error("unrecognized value \"%s\" for \"%s\": boolean expected\n",
					   value, name);
		valid = false;
	}
	return valid;
}

/*
 * Try to interpret "value" as an integer value, and if successful,
 * store it in *result.  Otherwise don't clobber *result.
 *
 * "name" is the name of the variable we're assigning to, to use in error
 * report if any.  Pass name == NULL to suppress the error report.
 *
 * Return true when "value" is syntactically valid, false otherwise.
 */
bool
ParseVariableNum(const char *value, const char *name, int *result)
{
	char	   *end;
	long		numval;

	if (value == NULL)
		return false;
	errno = 0;
	numval = strtol(value, &end, 0);
	if (errno == 0 && *end == '\0' && end != value && numval == (int) numval)
	{
		*result = (int) numval;
		return true;
	}
	else
	{
		/* string is not recognized; don't clobber *result */
		if (name)
			psql_error("invalid value \"%s\" for \"%s\": integer expected\n",
					   value, name);
		return false;
	}
}

/*
 * Read integer value of the numeric variable named "name".
 *
 * Return defaultval if it is not set, or faultval if its value is not a
 * valid integer.  (No error message is issued.)
 */
int
GetVariableNum(VariableSpace space,
			   const char *name,
			   int defaultval,
			   int faultval)
{
	const char *val;
	int			result;

	val = GetVariable(space, name);
	if (!val)
		return defaultval;

	if (ParseVariableNum(val, NULL, &result))
		return result;
	else
		return faultval;
}

/*
 * Print values of all variables.
 */
void
PrintVariables(VariableSpace space)
{
	struct _variable *ptr;

	if (!space)
		return;

	for (ptr = space->next; ptr; ptr = ptr->next)
	{
		if (ptr->value)
			printf("%s = '%s'\n", ptr->name, ptr->value);
		if (cancel_pressed)
			break;
	}
}

/*
 * Set the variable named "name" to value "value",
 * or delete it if "value" is NULL.
 *
 * Returns true if successful, false if not; in the latter case a suitable
 * error message has been printed, except for the unexpected case of
 * space or name being NULL.
 */
bool
SetVariable(VariableSpace space, const char *name, const char *value)
{
	struct _variable *current,
			   *previous;

	if (!space || !name)
		return false;

	if (!valid_variable_name(name))
	{
		psql_error("invalid variable name: \"%s\"\n", name);
		return false;
	}

	if (!value)
		return DeleteVariable(space, name);

	for (previous = space, current = space->next;
		 current;
		 previous = current, current = current->next)
	{
		if (strcmp(current->name, name) == 0)
		{
			/*
			 * Found entry, so update, unless hook returns false.  The hook
			 * may need the passed value to have the same lifespan as the
			 * variable, so allocate it right away, even though we'll have to
			 * free it again if the hook returns false.
			 */
			char	   *new_value = pg_strdup(value);
			bool		confirmed;

			if (current->assign_hook)
				confirmed = (*current->assign_hook) (new_value);
			else
				confirmed = true;

			if (confirmed)
			{
				if (current->value)
					pg_free(current->value);
				current->value = new_value;
			}
			else
				pg_free(new_value);		/* current->value is left unchanged */

			return confirmed;
		}
	}

	/* not present, make new entry */
	current = pg_malloc(sizeof *current);
	current->name = pg_strdup(name);
	current->value = pg_strdup(value);
	current->assign_hook = NULL;
	current->next = NULL;
	previous->next = current;
	return true;
}

/*
 * Attach an assign hook function to the named variable.
 *
 * If the variable doesn't already exist, create it with value NULL,
 * just so we have a place to store the hook function.  (Externally,
 * this isn't different from it not being defined.)
 *
 * The hook is immediately called on the variable's current value.  This is
 * meant to let it update any derived psql state.  If the hook doesn't like
 * the current value, it will print a message to that effect, but we'll ignore
 * it.  Generally we do not expect any such failure here, because this should
 * get called before any user-supplied value is assigned.
 */
void
SetVariableAssignHook(VariableSpace space, const char *name, VariableAssignHook hook)
{
	struct _variable *current,
			   *previous;

	if (!space || !name)
		return;

	if (!valid_variable_name(name))
		return;

	for (previous = space, current = space->next;
		 current;
		 previous = current, current = current->next)
	{
		if (strcmp(current->name, name) == 0)
		{
			/* found entry, so update */
			current->assign_hook = hook;
			(void) (*hook) (current->value);
			return;
		}
	}

	/* not present, make new entry */
	current = pg_malloc(sizeof *current);
	current->name = pg_strdup(name);
	current->value = NULL;
	current->assign_hook = hook;
	current->next = NULL;
	previous->next = current;
	(void) (*hook) (NULL);
}

/*
 * Convenience function to set a variable's value to "on".
 */
bool
SetVariableBool(VariableSpace space, const char *name)
{
	return SetVariable(space, name, "on");
}

/*
 * Attempt to delete variable.
 *
 * If unsuccessful, print a message and return "false".
 * Deleting a nonexistent variable is not an error.
 */
bool
DeleteVariable(VariableSpace space, const char *name)
{
	struct _variable *current,
			   *previous;

	if (!space)
		return true;

	for (previous = space, current = space->next;
		 current;
		 previous = current, current = current->next)
	{
		if (strcmp(current->name, name) == 0)
		{
			if (current->assign_hook)
			{
				/* Allow deletion only if hook is okay with NULL value */
				if (!(*current->assign_hook) (NULL))
					return false;		/* message printed by hook */
				if (current->value)
					free(current->value);
				current->value = NULL;
				/* Don't delete entry, or we'd forget the hook function */
			}
			else
			{
				/* We can delete the entry as well as its value */
				if (current->value)
					free(current->value);
				previous->next = current->next;
				free(current->name);
				free(current);
			}
			return true;
		}
	}

	return true;
}

/*
 * Emit error with suggestions for variables or commands
 * accepting enum-style arguments.
 * This function just exists to standardize the wording.
 * suggestions should follow the format "fee, fi, fo, fum".
 */
void
PsqlVarEnumError(const char *name, const char *value, const char *suggestions)
{
	psql_error("unrecognized value \"%s\" for \"%s\"\nAvailable values are: %s.\n",
			   value, name, suggestions);
}
