/*-------------------------------------------------------------------------
 *
 * regproc.h
 *	  Functions for the built-in types regproc, regclass, regtype, etc.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/regproc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REGPROC_H
#define REGPROC_H

#include "nodes/pg_list.h"

extern List *stringToQualifiedNameList(const char *string);
extern char *format_procedure(Oid procedure_oid);
extern char *format_procedure_qualified(Oid procedure_oid);
extern void format_procedure_parts(Oid operator_oid, List **objnames,
								   List **objargs);
extern char *format_operator(Oid operator_oid);
extern char *format_operator_qualified(Oid operator_oid);
extern void format_operator_parts(Oid operator_oid, List **objnames,
								  List **objargs);

#endif
