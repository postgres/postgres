/*
 * src/pl/plpython/plpy_cursorobject.h
 */

#ifndef PLPY_CURSOROBJECT_H
#define PLPY_CURSOROBJECT_H

#include "plpy_typeio.h"


typedef struct PLyCursorObject
{
	PyObject_HEAD
	char	   *portalname;
	PLyTypeInfo result;
	bool		closed;
} PLyCursorObject;

extern void PLy_cursor_init_type(void);
extern PyObject *PLy_cursor(PyObject *self, PyObject *args);

#endif   /* PLPY_CURSOROBJECT_H */
