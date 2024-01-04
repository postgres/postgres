/*-------------------------------------------------------------------------
 *
 * plpython_system.h - pull in Python's system header files
 *
 * We break this out as a separate header file to precisely control
 * the scope of the "system_header" pragma.  No Postgres-specific
 * declarations should be put here.  However, we do include some stuff
 * that is meant to prevent conflicts between our code and Python.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/pl/plpython/plpython_system.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLPYTHON_SYSTEM_H
#define PLPYTHON_SYSTEM_H

/*
 * Newer versions of the Python headers trigger a lot of warnings with our
 * preferred compiler flags (at least -Wdeclaration-after-statement is known
 * to be problematic). The system_header pragma hides warnings from within
 * the rest of this file, if supported.
 */
#ifdef HAVE_PRAGMA_GCC_SYSTEM_HEADER
#pragma GCC system_header
#endif

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

#endif							/* PLPYTHON_SYSTEM_H */
