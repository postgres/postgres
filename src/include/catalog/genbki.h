/*-------------------------------------------------------------------------
 *
 * genbki.h
 *	  Required include file for all POSTGRES catalog header files
 *
 * genbki.h defines CATALOG(), DATA(), BKI_BOOTSTRAP and related macros
 * so that the catalog header files can be read by the C compiler.
 * (These same words are recognized by genbki.pl to build the BKI
 * bootstrap file from these header files.)
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/genbki.h,v 1.6 2010/01/05 01:06:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef GENBKI_H
#define GENBKI_H

/* Introduces a catalog's structure definition */
#define CATALOG(name,oid)	typedef struct CppConcat(FormData_,name)

/* Options that may appear after CATALOG (on the same line) */
#define BKI_BOOTSTRAP
#define BKI_SHARED_RELATION
#define BKI_WITHOUT_OIDS
#define BKI_ROWTYPE_OID(oid)
#define BKI_SCHEMA_MACRO

/* Declarations that provide the initial content of a catalog */
/* In C, these need to expand into some harmless, repeatable declaration */
#define DATA(x)   extern int no_such_variable
#define DESCR(x)  extern int no_such_variable
#define SHDESCR(x) extern int no_such_variable

/* PHONY type definition for use in catalog structure definitions only */
typedef int aclitem;

#endif   /* GENBKI_H */
