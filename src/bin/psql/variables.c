#include <config.h>
#include <c.h>
#include "variables.h"

#include <stdlib.h>
#include <assert.h>


VariableSpace CreateVariableSpace(void)
{
    struct _variable *ptr;

    ptr = calloc(1, sizeof *ptr);
    if (!ptr) return NULL;

    ptr->name = strdup("@");
    ptr->value = strdup("");
    if (!ptr->name || !ptr->value) {
	free(ptr->name);
	free(ptr->value);
	free(ptr);
	return NULL;
    }
    
    return ptr;
}



const char * GetVariable(VariableSpace space, const char * name)
{
    struct _variable *current;

    if (!space)
	return NULL;

    if (strspn(name, VALID_VARIABLE_CHARS) != strlen(name)) return NULL;

    for (current = space; current; current = current->next) {
#ifdef USE_ASSERT_CHECKING
	assert(current->name);
	assert(current->value);
#endif
	if (strcmp(current->name, name)==0)
	    return current->value;
    }

    return NULL;
}



bool GetVariableBool(VariableSpace space, const char * name)
{
    return GetVariable(space, name)!=NULL ? true : false;
}



bool SetVariable(VariableSpace space, const char * name, const char * value)
{
    struct _variable *current, *previous;

    if (!space)
	return false;

    if (!value)
	return DeleteVariable(space, name);

    if (strspn(name, VALID_VARIABLE_CHARS) != strlen(name)) return false;

    for (current = space; current; previous = current, current = current->next) {
#ifdef USE_ASSERT_CHECKING
	assert(current->name);
	assert(current->value);
#endif
	if (strcmp(current->name, name)==0) {
	    free (current->value);
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



bool DeleteVariable(VariableSpace space, const char * name)
{
    struct _variable *current, *previous;

    if (!space)
	return false;

    if (strspn(name, VALID_VARIABLE_CHARS) != strlen(name)) return false;

    for (current = space, previous = NULL; current; previous = current, current = current->next) {
#ifdef USE_ASSERT_CHECKING
	assert(current->name);
	assert(current->value);
#endif
	if (strcmp(current->name, name)==0) {
	    free (current->name);
	    free (current->value);
	    if (previous)
		previous->next = current->next;
	    free(current);
	    return true;
	}
    }

    return true;
}



void DestroyVariableSpace(VariableSpace space)
{
    if (!space)
	return;

    DestroyVariableSpace(space->next);
    free(space);
}
