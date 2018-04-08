/*-------------------------------------------------------------------------
 *
 * pg_conversion.h
 *	  definition of the system "conversion" relation (pg_conversion)
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_conversion.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CONVERSION_H
#define PG_CONVERSION_H

#include "catalog/genbki.h"
#include "catalog/pg_conversion_d.h"

/* ----------------------------------------------------------------
 *		pg_conversion definition.
 *
 *		cpp turns this into typedef struct FormData_pg_namespace
 *
 *	conname				name of the conversion
 *	connamespace		name space which the conversion belongs to
 *	conowner			owner of the conversion
 *	conforencoding		FOR encoding id
 *	contoencoding		TO encoding id
 *	conproc				OID of the conversion proc
 *	condefault			true if this is a default conversion
 * ----------------------------------------------------------------
 */
CATALOG(pg_conversion,2607,ConversionRelationId)
{
	NameData	conname;
	Oid			connamespace;
	Oid			conowner;
	int32		conforencoding;
	int32		contoencoding;
	regproc		conproc;
	bool		condefault;
} FormData_pg_conversion;

/* ----------------
 *		Form_pg_conversion corresponds to a pointer to a tuple with
 *		the format of pg_conversion relation.
 * ----------------
 */
typedef FormData_pg_conversion *Form_pg_conversion;

#endif							/* PG_CONVERSION_H */
