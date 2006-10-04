/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2006, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/bin/psql/variables.h,v 1.20 2006/10/04 00:30:06 momjian Exp $
 */
#ifndef VARIABLES_H
#define VARIABLES_H

/*
 * This implements a sort of variable repository. One could also think of it
 * as a cheap version of an associative array. In each one of these
 * datastructures you can store name/value pairs.  There can also be an
 * "assign hook" function that is called whenever the variable's value is
 * changed.
 *
 * An "unset" operation causes the hook to be called with newval == NULL.
 *
 * Note: if value == NULL then the variable is logically unset, but we are
 * keeping the struct around so as not to forget about its hook function.
 */
typedef void (*VariableAssignHook) (const char *newval);

struct _variable
{
	char	   *name;
	char	   *value;
	VariableAssignHook assign_hook;
	struct _variable *next;
};

typedef struct _variable *VariableSpace;

/* Allowed chars in a variable's name */
#define VALID_VARIABLE_CHARS "abcdefghijklmnopqrstuvwxyz"\
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "0123456789_"

VariableSpace CreateVariableSpace(void);
const char *GetVariable(VariableSpace space, const char *name);

bool		ParseVariableBool(const char *val);
int ParseVariableNum(const char *val,
				 int defaultval,
				 int faultval,
				 bool allowtrail);
int GetVariableNum(VariableSpace space,
			   const char *name,
			   int defaultval,
			   int faultval,
			   bool allowtrail);

void		PrintVariables(VariableSpace space);

bool		SetVariable(VariableSpace space, const char *name, const char *value);
bool		SetVariableAssignHook(VariableSpace space, const char *name, VariableAssignHook hook);
bool		SetVariableBool(VariableSpace space, const char *name);
bool		DeleteVariable(VariableSpace space, const char *name);

#endif   /* VARIABLES_H */
