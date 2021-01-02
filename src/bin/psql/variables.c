/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2021, PostgreSQL Global Development Group
 *
 * src/bin/psql/variables.c
 */
#include "postgres_fe.h"

#include "common.h"
#include "common/logging.h"
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
 *
 * The list entries are kept in name order (according to strcmp).  This
 * is mainly to make the results of PrintVariables() more pleasing.
 */
VariableSpace
CreateVariableSpace(void)
{
	struct _variable *ptr;

	ptr = pg_malloc(sizeof *ptr);
	ptr->name = NULL;
	ptr->value = NULL;
	ptr->substitute_hook = NULL;
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
		int			cmp = strcmp(current->name, name);

		if (cmp == 0)
		{
			/* this is correct answer when value is NULL, too */
			return current->value;
		}
		if (cmp > 0)
			break;				/* it's not there */
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

	/* Treat "unset" as an empty string, which will lead to error below */
	if (value == NULL)
		value = "";

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
			pg_log_error("unrecognized value \"%s\" for \"%s\": Boolean expected",
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

	/* Treat "unset" as an empty string, which will lead to error below */
	if (value == NULL)
		value = "";

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
			pg_log_error("invalid value \"%s\" for \"%s\": integer expected",
						 value, name);
		return false;
	}
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
		/* Deletion of non-existent variable is not an error */
		if (!value)
			return true;
		pg_log_error("invalid variable name: \"%s\"", name);
		return false;
	}

	for (previous = space, current = space->next;
		 current;
		 previous = current, current = current->next)
	{
		int			cmp = strcmp(current->name, name);

		if (cmp == 0)
		{
			/*
			 * Found entry, so update, unless assign hook returns false.
			 *
			 * We must duplicate the passed value to start with.  This
			 * simplifies the API for substitute hooks.  Moreover, some assign
			 * hooks assume that the passed value has the same lifespan as the
			 * variable.  Having to free the string again on failure is a
			 * small price to pay for keeping these APIs simple.
			 */
			char	   *new_value = value ? pg_strdup(value) : NULL;
			bool		confirmed;

			if (current->substitute_hook)
				new_value = current->substitute_hook(new_value);

			if (current->assign_hook)
				confirmed = current->assign_hook(new_value);
			else
				confirmed = true;

			if (confirmed)
			{
				if (current->value)
					pg_free(current->value);
				current->value = new_value;

				/*
				 * If we deleted the value, and there are no hooks to
				 * remember, we can discard the variable altogether.
				 */
				if (new_value == NULL &&
					current->substitute_hook == NULL &&
					current->assign_hook == NULL)
				{
					previous->next = current->next;
					free(current->name);
					free(current);
				}
			}
			else if (new_value)
				pg_free(new_value); /* current->value is left unchanged */

			return confirmed;
		}
		if (cmp > 0)
			break;				/* it's not there */
	}

	/* not present, make new entry ... unless we were asked to delete */
	if (value)
	{
		current = pg_malloc(sizeof *current);
		current->name = pg_strdup(name);
		current->value = pg_strdup(value);
		current->substitute_hook = NULL;
		current->assign_hook = NULL;
		current->next = previous->next;
		previous->next = current;
	}
	return true;
}

/*
 * Attach substitute and/or assign hook functions to the named variable.
 * If you need only one hook, pass NULL for the other.
 *
 * If the variable doesn't already exist, create it with value NULL, just so
 * we have a place to store the hook function(s).  (The substitute hook might
 * immediately change the NULL to something else; if not, this state is
 * externally the same as the variable not being defined.)
 *
 * The substitute hook, if given, is immediately called on the variable's
 * value.  Then the assign hook, if given, is called on the variable's value.
 * This is meant to let it update any derived psql state.  If the assign hook
 * doesn't like the current value, it will print a message to that effect,
 * but we'll ignore it.  Generally we do not expect any such failure here,
 * because this should get called before any user-supplied value is assigned.
 */
void
SetVariableHooks(VariableSpace space, const char *name,
				 VariableSubstituteHook shook,
				 VariableAssignHook ahook)
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
		int			cmp = strcmp(current->name, name);

		if (cmp == 0)
		{
			/* found entry, so update */
			current->substitute_hook = shook;
			current->assign_hook = ahook;
			if (shook)
				current->value = (*shook) (current->value);
			if (ahook)
				(void) (*ahook) (current->value);
			return;
		}
		if (cmp > 0)
			break;				/* it's not there */
	}

	/* not present, make new entry */
	current = pg_malloc(sizeof *current);
	current->name = pg_strdup(name);
	current->value = NULL;
	current->substitute_hook = shook;
	current->assign_hook = ahook;
	current->next = previous->next;
	previous->next = current;
	if (shook)
		current->value = (*shook) (current->value);
	if (ahook)
		(void) (*ahook) (current->value);
}

/*
 * Return true iff the named variable has substitute and/or assign hook
 * functions.
 */
bool
VariableHasHook(VariableSpace space, const char *name)
{
	struct _variable *current;

	Assert(space);
	Assert(name);

	for (current = space->next; current; current = current->next)
	{
		int			cmp = strcmp(current->name, name);

		if (cmp == 0)
			return (current->substitute_hook != NULL ||
					current->assign_hook != NULL);
		if (cmp > 0)
			break;				/* it's not there */
	}

	return false;
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
	return SetVariable(space, name, NULL);
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
	pg_log_error("unrecognized value \"%s\" for \"%s\"\n"
				 "Available values are: %s.",
				 value, name, suggestions);
}
