/* This implements a sort of variable repository. One could also think of it
 * as cheap version of an associative array. In each one of these
 * datastructures you can store name/value pairs.
 *
 * All functions (should) follow the Shit-In-Shit-Out (SISO) principle, i.e.,
 * you can pass them NULL pointers and the like and they will return something
 * appropriate.
 */

#ifndef VARIABLES_H
#define VARIABLES_H
#include <c.h>

#define VALID_VARIABLE_CHARS "abcdefghijklmnopqrstuvwxyz0123456789_"

struct _variable {
    char * name;
    char * value;
    struct _variable * next;
};

typedef struct _variable * VariableSpace;


VariableSpace CreateVariableSpace(void);
const char * GetVariable(VariableSpace space, const char * name);
bool GetVariableBool(VariableSpace space, const char * name);
bool SetVariable(VariableSpace space, const char * name, const char * value);
bool DeleteVariable(VariableSpace space, const char * name);
void DestroyVariableSpace(VariableSpace space);


#endif /* VARIABLES_H */
