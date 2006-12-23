/*-------------------------------------------------------------------------
 *
 * xml.h
 *	  Declarations for XML data type support.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/xml.h,v 1.2 2006/12/23 04:56:50 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef XML_H
#define XML_H

#include "fmgr.h"

typedef struct varlena xmltype;

#define DatumGetXmlP(X)		((xmltype *) PG_DETOAST_DATUM(X))

#define PG_GETARG_XML_P(n)	DatumGetXmlP(PG_GETARG_DATUM(n))
#define PG_RETURN_XML_P(x)	PG_RETURN_POINTER(x)

extern Datum xml_in(PG_FUNCTION_ARGS);
extern Datum xml_out(PG_FUNCTION_ARGS);
extern Datum xmlcomment(PG_FUNCTION_ARGS);
extern Datum xmlparse(PG_FUNCTION_ARGS);
extern Datum xmlpi(PG_FUNCTION_ARGS);
extern Datum xmlroot(PG_FUNCTION_ARGS);
extern Datum xmlvalidate(PG_FUNCTION_ARGS);

extern char *map_sql_identifier_to_xml_name(char *ident, bool fully_escaped);

#endif /* XML_H */
