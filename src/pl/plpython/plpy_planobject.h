/*
 * src/pl/plpython/plpy_planobject.h
 */

#ifndef PLPY_PLANOBJECT_H
#define PLPY_PLANOBJECT_H

#include "executor/spi.h"
#include "plpy_typeio.h"


typedef struct PLyPlanObject
{
	PyObject_HEAD
	SPIPlanPtr	plan;
	int			nargs;
	Oid		   *types;
	Datum	   *values;
	PLyTypeInfo *args;
} PLyPlanObject;

extern void PLy_plan_init_type(void);
extern PyObject *PLy_plan_new(void);
extern bool is_PLyPlanObject(PyObject *ob);

#endif   /* PLPY_PLANOBJECT_H */
