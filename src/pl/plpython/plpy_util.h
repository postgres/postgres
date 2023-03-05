/*--------------------------
 * common utility functions
 *--------------------------
 */

#ifndef PLPY_UTIL_H
#define PLPY_UTIL_H

#include "plpython.h"

extern PGDLLEXPORT PyObject *PLyUnicode_Bytes(PyObject *unicode);
extern PGDLLEXPORT char *PLyUnicode_AsString(PyObject *unicode);

extern PGDLLEXPORT PyObject *PLyUnicode_FromString(const char *s);
extern PGDLLEXPORT PyObject *PLyUnicode_FromStringAndSize(const char *s, Py_ssize_t size);

#endif							/* PLPY_UTIL_H */
