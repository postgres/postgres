/*-------------------------------------------------------------------------
 *
 * plpython.h - Python as a procedural language for PostgreSQL
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
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
 * other plpython headers.  (In practice, other plpython headers will also
 * include this file, so that they can compile standalone.)
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

/*
 * Python versions <= 3.8 otherwise define a replacement, causing macro
 * redefinition warnings.
 */
#define HAVE_SNPRINTF 1

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

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plpython")

/*
 * Used throughout, so it's easier to just include it everywhere.
 */
#include "plpy_util.h"

#endif							/* PLPYTHON_H */
