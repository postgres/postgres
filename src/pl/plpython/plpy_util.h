/*--------------------------
 * common utility functions
 *--------------------------
 */

#ifndef PLPY_UTIL_H
#define PLPY_UTIL_H

#include "plpython.h"

extern PyObject *PLyUnicode_Bytes(PyObject *unicode);
extern char *PLyUnicode_AsString(PyObject *unicode);

extern PyObject *PLyUnicode_FromString(const char *s);
extern PyObject *PLyUnicode_FromStringAndSize(const char *s, Py_ssize_t size);

#endif							/* PLPY_UTIL_H */
