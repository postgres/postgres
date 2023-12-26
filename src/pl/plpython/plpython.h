/*-------------------------------------------------------------------------
 *
 * plpython.h - Python as a procedural language for PostgreSQL
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
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
 * Pull in Python headers via a wrapper header, to control the scope of
 * the system_header pragma therein.
 */
#include "plpython_system.h"

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plpython")

/*
 * Used throughout, and also by the Python 2/3 porting layer, so it's easier to
 * just include it everywhere.
 */
#include "plpy_util.h"

#endif							/* PLPYTHON_H */
