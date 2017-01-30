/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2017, PostgreSQL Global Development Group
 *
 * This implements a sort of variable repository.  One could also think of it
 * as a cheap version of an associative array.  Each variable has a string
 * name and a string value.  The value can't be NULL, or more precisely
 * that's not distinguishable from the variable being unset.
 *
 * src/bin/psql/variables.h
 */
#ifndef VARIABLES_H
#define VARIABLES_H

/*
 * Variables can be given "assign hook" functions.  The assign hook can
 * prevent invalid values from being assigned, and can update internal C
 * variables to keep them in sync with the variable's current value.
 *
 * A hook function is called before any attempted assignment, with the
 * proposed new value of the variable (or with NULL, if an \unset is being
 * attempted).  If it returns false, the assignment doesn't occur --- it
 * should print an error message with psql_error() to tell the user why.
 *
 * When a hook function is installed with SetVariableAssignHook(), it is
 * called with the variable's current value (or with NULL, if it wasn't set
 * yet).  But its return value is ignored in this case.  The hook should be
 * set before any possibly-invalid value can be assigned.
 */
typedef bool (*VariableAssignHook) (const char *newval);

/*
 * Data structure representing one variable.
 *
 * Note: if value == NULL then the variable is logically unset, but we are
 * keeping the struct around so as not to forget about its hook function.
 */
struct _variable
{
	char	   *name;
	char	   *value;
	VariableAssignHook assign_hook;
	struct _variable *next;
};

/* Data structure representing a set of variables */
typedef struct _variable *VariableSpace;


VariableSpace CreateVariableSpace(void);
const char *GetVariable(VariableSpace space, const char *name);

bool ParseVariableBool(const char *value, const char *name,
				  bool *result);

bool ParseVariableNum(const char *value, const char *name,
				 int *result);

int GetVariableNum(VariableSpace space,
			   const char *name,
			   int defaultval,
			   int faultval);

void		PrintVariables(VariableSpace space);

bool		SetVariable(VariableSpace space, const char *name, const char *value);
void		SetVariableAssignHook(VariableSpace space, const char *name, VariableAssignHook hook);
bool		SetVariableBool(VariableSpace space, const char *name);
bool		DeleteVariable(VariableSpace space, const char *name);

void		PsqlVarEnumError(const char *name, const char *value, const char *suggestions);

#endif   /* VARIABLES_H */
