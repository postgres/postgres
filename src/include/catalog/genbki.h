/*-------------------------------------------------------------------------
 *
 * genbki.h
 *	  Required include file for all POSTGRES catalog header files
 *
 * genbki.h defines CATALOG(), BKI_BOOTSTRAP and related macros
 * so that the catalog header files can be read by the C compiler.
 * (These same words are recognized by genbki.pl to build the BKI
 * bootstrap file from these header files.)
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/genbki.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GENBKI_H
#define GENBKI_H

/* Introduces a catalog's structure definition */
#define CATALOG(name,oid,oidmacro)	typedef struct CppConcat(FormData_,name)

/* Options that may appear after CATALOG (on the same line) */
#define BKI_BOOTSTRAP
#define BKI_SHARED_RELATION
#define BKI_ROWTYPE_OID(oid,oidmacro)
#define BKI_SCHEMA_MACRO

/* Options that may appear after an attribute (on the same line) */
#define BKI_FORCE_NULL
#define BKI_FORCE_NOT_NULL
/* Specifies a default value for a catalog field */
#define BKI_DEFAULT(value)
/* Specifies a default value for auto-generated array types */
#define BKI_ARRAY_DEFAULT(value)
/*
 * Indicates how to perform name lookups, typically for an OID or
 * OID-array field
 */
#define BKI_LOOKUP(catalog)

/* The following are never defined; they are here only for documentation. */

/*
 * Variable-length catalog fields (except possibly the first not nullable one)
 * should not be visible in C structures, so they are made invisible by #ifdefs
 * of an undefined symbol.  See also MARKNOTNULL in bootstrap.c for how this is
 * handled.
 */
#undef CATALOG_VARLEN

/*
 * There is code in some catalog headers that needs to be visible to clients,
 * but we don't want clients to include the full header because of safety
 * issues with other code in the header.  To handle that, surround code that
 * should be visible to clients with "#ifdef EXPOSE_TO_CLIENT_CODE".  That
 * instructs genbki.pl to copy the section when generating the corresponding
 * "_d" header, which can be included by both client and backend code.
 */
#undef EXPOSE_TO_CLIENT_CODE

#endif							/* GENBKI_H */
