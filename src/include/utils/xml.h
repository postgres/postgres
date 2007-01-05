/*-------------------------------------------------------------------------
 *
 * xml.h
 *	  Declarations for XML data type support.
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/xml.h,v 1.6 2007/01/05 22:20:00 momjian Exp $
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
extern Datum xml_recv(PG_FUNCTION_ARGS);
extern Datum xml_send(PG_FUNCTION_ARGS);
extern Datum xmlcomment(PG_FUNCTION_ARGS);
extern Datum texttoxml(PG_FUNCTION_ARGS);
extern Datum xmlvalidate(PG_FUNCTION_ARGS);

extern xmltype *xmlparse(text *data, bool is_doc, bool preserve_whitespace);
extern xmltype *xmlpi(char *target, text *arg);
extern xmltype *xmlroot(xmltype *data, text *version, int standalone);

extern char *map_sql_identifier_to_xml_name(char *ident, bool fully_escaped);
extern char *map_xml_name_to_sql_identifier(char *name);

#endif /* XML_H */
