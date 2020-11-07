/*-------------------------------------------------------------------------
 *
 * pg_ts_parser.h
 *	  definition of the "text search parser" system catalog (pg_ts_parser)
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_ts_parser.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TS_PARSER_H
#define PG_TS_PARSER_H

#include "catalog/genbki.h"
#include "catalog/pg_ts_parser_d.h"

/* ----------------
 *		pg_ts_parser definition.  cpp turns this into
 *		typedef struct FormData_pg_ts_parser
 * ----------------
 */
CATALOG(pg_ts_parser,3601,TSParserRelationId)
{
	Oid			oid;			/* oid */

	/* parser's name */
	NameData	prsname;

	/* name space */
	Oid			prsnamespace BKI_DEFAULT(PGNSP);

	/* init parsing session */
	regproc		prsstart BKI_LOOKUP(pg_proc);

	/* return next token */
	regproc		prstoken BKI_LOOKUP(pg_proc);

	/* finalize parsing session */
	regproc		prsend BKI_LOOKUP(pg_proc);

	/* return data for headline creation */
	regproc		prsheadline BKI_LOOKUP(pg_proc);

	/* return descriptions of lexeme's types */
	regproc		prslextype BKI_LOOKUP(pg_proc);
} FormData_pg_ts_parser;

typedef FormData_pg_ts_parser *Form_pg_ts_parser;

DECLARE_UNIQUE_INDEX(pg_ts_parser_prsname_index, 3606, on pg_ts_parser using btree(prsname name_ops, prsnamespace oid_ops));
#define TSParserNameNspIndexId	3606
DECLARE_UNIQUE_INDEX(pg_ts_parser_oid_index, 3607, on pg_ts_parser using btree(oid oid_ops));
#define TSParserOidIndexId	3607

#endif							/* PG_TS_PARSER_H */
