/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright 2000 by PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/variables.h,v 1.11 2003/03/20 06:43:35 momjian Exp $
 */

/*
 * This implements a sort of variable repository. One could also think of it
 * as cheap version of an associative array. In each one of these
 * datastructures you can store name/value pairs.
 */

#ifndef VARIABLES_H
#define VARIABLES_H

#define VALID_VARIABLE_CHARS "abcdefghijklmnopqrstuvwxyz"\
							 "ABCDEFGHIJKLMNOPQRSTUVWXYZ" "0123456789_"

struct _variable
{
	char	   *name;
	char	   *value;
	struct _variable *next;
};

typedef struct _variable *VariableSpace;


VariableSpace CreateVariableSpace(void);
const char *GetVariable(VariableSpace space, const char *name);
bool	GetVariableBool(VariableSpace space, const char *name);
bool	VariableEquals(VariableSpace space, const char name[], const char *opt);

/* Read numeric variable, or defaultval if it is not set, or faultval if its
 * value is not a valid numeric string.  If allowtrail is false, this will 
 * include the case where there are trailing characters after the number.
 */
int GetVariableNum(VariableSpace space, 
	 					const char name[], 
						int defaultval, 
						int faultval,
						bool allowtrail);


/* Find value of variable <name> among NULL-terminated list of alternative 
 * options.  Returns var_notset if the variable was not set, var_notfound if its
 * value did not occur in the list of options, or the number of the matching
 * option.  The first option is 1, the second is 2 and so on.
 */
enum { var_notset = 0, var_notfound = -1 };
int 	SwitchVariable(VariableSpace space, const char name[], const char *opt,...);

void 	PrintVariables(VariableSpace space);

bool	SetVariable(VariableSpace space, const char *name, const char *value);
bool	SetVariableBool(VariableSpace space, const char *name);
bool	DeleteVariable(VariableSpace space, const char *name);
void	DestroyVariableSpace(VariableSpace space);

#endif   /* VARIABLES_H */
