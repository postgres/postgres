/*-------------------------------------------------------------------------
 *
 * plpython.h - Python as a procedural language for PostgreSQL
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/pl/plpython/plpython.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLPYTHON_H
#define PLPYTHON_H

/*
 * Include order should be: postgres.h, other postgres headers, plpython.h,
 * other plpython headers
 */
#ifndef POSTGRES_H
#error postgres.h must be included before plpython.h
#endif

/*
 * Undefine some things that get (re)defined in the Python headers. They aren't
 * used by the PL/Python code, and all PostgreSQL headers should be included
 * earlier, so this should be pretty safe.
 */
#undef _POSIX_C_SOURCE
#undef _XOPEN_SOURCE
#undef HAVE_STRERROR
#undef HAVE_TZNAME

/*
 * Sometimes python carefully scribbles on our *printf macros.
 * So we undefine them here and redefine them after it's done its dirty deed.
 */

#ifdef USE_REPL_SNPRINTF
#undef snprintf
#undef vsnprintf
#endif

#if defined(_MSC_VER) && defined(_DEBUG)
/* Python uses #pragma to bring in a non-default libpython on VC++ if
 * _DEBUG is defined */
#undef _DEBUG
/* Also hide away errcode, since we load Python.h before postgres.h */
#define errcode __msvc_errcode
#include <Python.h>
#undef errcode
#define _DEBUG
#elif defined (_MSC_VER)
#define errcode __msvc_errcode
#include <Python.h>
#undef errcode
#else
#include <Python.h>
#endif

/*
 * Py_ssize_t compat for Python <= 2.4
 */
#if PY_VERSION_HEX < 0x02050000 && !defined(PY_SSIZE_T_MIN)
typedef int Py_ssize_t;

#define PY_SSIZE_T_MAX INT_MAX
#define PY_SSIZE_T_MIN INT_MIN
#endif

/*
 * Python 2/3 strings/unicode/bytes handling.  Python 2 has strings
 * and unicode, Python 3 has strings, which are unicode on the C
 * level, and bytes.  The porting convention, which is similarly used
 * in Python 2.6, is that "Unicode" is always unicode, and "Bytes" are
 * bytes in Python 3 and strings in Python 2.  Since we keep
 * supporting Python 2 and its usual strings, we provide a
 * compatibility layer for Python 3 that when asked to convert a C
 * string to a Python string it converts the C string from the
 * PostgreSQL server encoding to a Python Unicode object.
 */

#if PY_VERSION_HEX < 0x02060000
/* This is exactly the compatibility layer that Python 2.6 uses. */
#define PyBytes_AsString PyString_AsString
#define PyBytes_FromStringAndSize PyString_FromStringAndSize
#define PyBytes_Size PyString_Size
#define PyObject_Bytes PyObject_Str
#endif

#if PY_MAJOR_VERSION >= 3
#define PyString_Check(x) 0
#define PyString_AsString(x) PLyUnicode_AsString(x)
#define PyString_FromString(x) PLyUnicode_FromString(x)
#endif

/*
 * Python 3 only has long.
 */
#if PY_MAJOR_VERSION >= 3
#define PyInt_FromLong(x) PyLong_FromLong(x)
#define PyInt_AsLong(x) PyLong_AsLong(x)
#endif

/*
 * PyVarObject_HEAD_INIT was added in Python 2.6.  Its use is
 * necessary to handle both Python 2 and 3.  This replacement
 * definition is for Python <=2.5
 */
#ifndef PyVarObject_HEAD_INIT
#define PyVarObject_HEAD_INIT(type, size)		\
		PyObject_HEAD_INIT(type) size,
#endif

/* Python 3 removed the Py_TPFLAGS_HAVE_ITER flag */
#if PY_MAJOR_VERSION >= 3
#define Py_TPFLAGS_HAVE_ITER 0
#endif

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plpython")

#include <compile.h>
#include <eval.h>

/* put back our snprintf and vsnprintf */
#ifdef USE_REPL_SNPRINTF
#ifdef snprintf
#undef snprintf
#endif
#ifdef vsnprintf
#undef vsnprintf
#endif
#ifdef __GNUC__
#define vsnprintf(...)	pg_vsnprintf(__VA_ARGS__)
#define snprintf(...)	pg_snprintf(__VA_ARGS__)
#else
#define vsnprintf				pg_vsnprintf
#define snprintf				pg_snprintf
#endif   /* __GNUC__ */
#endif   /* USE_REPL_SNPRINTF */

/*
 * Used throughout, and also by the Python 2/3 porting layer, so it's easier to
 * just include it everywhere.
 */
#include "plpy_util.h"

#endif   /* PLPYTHON_H */
