/*-------------------------------------------------------------------------
 *
 * pg_variable.h--
 *    the system variable relation "pg_variable" is not a "heap" relation.
 *    it is automatically created by the transam/ code and the
 *    information here is all bogus and is just here to make the
 *    relcache code happy.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_variable.h,v 1.1 1996/08/28 01:57:21 scrappy Exp $
 *
 * NOTES
 *    The structures and macros used by the transam/ code
 *    to access pg_variable should someday go here -cim 6/18/90
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_VARIABLE_H
#define PG_VARIABLE_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

CATALOG(pg_variable) BOOTSTRAP {
    Oid  	varfoo;
} FormData_pg_variable;

typedef FormData_pg_variable	*Form_pg_variable;

#define Natts_pg_variable	1
#define Anum_pg_variable_varfoo	1

#endif /* PG_VARIABLE_H */
