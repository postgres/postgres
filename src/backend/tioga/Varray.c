/* ************************************************************************
 *
 * Varray.c
 *
 *	  routines to provide a generic set of functions to handle variable sized
 * arrays.	  originally by Jiang Wu
 * ************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include "Varray.h"

Varray *
NewVarray(size_t nobj, size_t size)
/*
 * NewVarray -- allocate a Varray to contain an array of val each of which
 *				is size valSize.  Returns the Varray if successful,
 *				returns NULL otherwise.
 */
{
	Varray	   *result;

	if (nobj == 0)
		nobj = VARRAY_INITIAL_SIZE;
	result = (Varray *) malloc(sizeof(Varray));
	result->val = (void *) calloc(nobj, size);
	if (result == NULL)
		return NULL;
	result->size = size;
	result->nobj = 0;
	result->maxObj = nobj;
	return result;
}

int
AppendVarray(Varray * array, void *value, CopyingFunct copy)
/*
 * AppendVarray -- append value to the end of array.  This function
 *				   returns the size of the array after the addition of
 *				   the new element.
 */
{
	copy(value, VARRAY_NTH(array->val, array->size, array->nobj));
	array->nobj++;
	if (array->nobj >= array->maxObj)
		ENLARGE_VARRAY(array, array->maxObj / 2);
	return array->nobj;
}
