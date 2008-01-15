/*-------------------------------------------------------------------------
 *
 * xml.h
 *	  Declarations for XML data type support.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/xml.h,v 1.23 2008/01/15 18:57:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef XML_H
#define XML_H

#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/primnodes.h"

typedef struct varlena xmltype;

#define DatumGetXmlP(X)		((xmltype *) PG_DETOAST_DATUM(X))
#define XmlPGetDatum(X)		PointerGetDatum(X)

#define PG_GETARG_XML_P(n)	DatumGetXmlP(PG_GETARG_DATUM(n))
#define PG_RETURN_XML_P(x)	PG_RETURN_POINTER(x)

extern Datum xml_in(PG_FUNCTION_ARGS);
extern Datum xml_out(PG_FUNCTION_ARGS);
extern Datum xml_recv(PG_FUNCTION_ARGS);
extern Datum xml_send(PG_FUNCTION_ARGS);
extern Datum xmlcomment(PG_FUNCTION_ARGS);
extern Datum xmlconcat2(PG_FUNCTION_ARGS);
extern Datum texttoxml(PG_FUNCTION_ARGS);
extern Datum xmltotext(PG_FUNCTION_ARGS);
extern Datum xmlvalidate(PG_FUNCTION_ARGS);
extern Datum xpath(PG_FUNCTION_ARGS);

extern Datum table_to_xml(PG_FUNCTION_ARGS);
extern Datum query_to_xml(PG_FUNCTION_ARGS);
extern Datum cursor_to_xml(PG_FUNCTION_ARGS);
extern Datum table_to_xmlschema(PG_FUNCTION_ARGS);
extern Datum query_to_xmlschema(PG_FUNCTION_ARGS);
extern Datum cursor_to_xmlschema(PG_FUNCTION_ARGS);
extern Datum table_to_xml_and_xmlschema(PG_FUNCTION_ARGS);
extern Datum query_to_xml_and_xmlschema(PG_FUNCTION_ARGS);

extern Datum schema_to_xml(PG_FUNCTION_ARGS);
extern Datum schema_to_xmlschema(PG_FUNCTION_ARGS);
extern Datum schema_to_xml_and_xmlschema(PG_FUNCTION_ARGS);

extern Datum database_to_xml(PG_FUNCTION_ARGS);
extern Datum database_to_xmlschema(PG_FUNCTION_ARGS);
extern Datum database_to_xml_and_xmlschema(PG_FUNCTION_ARGS);

typedef enum
{
	XML_STANDALONE_YES,
	XML_STANDALONE_NO,
	XML_STANDALONE_NO_VALUE,
	XML_STANDALONE_OMITTED
} XmlStandaloneType;

extern xmltype *xmlconcat(List *args);
extern xmltype *xmlelement(XmlExprState *xmlExpr, ExprContext *econtext);
extern xmltype *xmlparse(text *data, XmlOptionType xmloption, bool preserve_whitespace);
extern xmltype *xmlpi(char *target, text *arg, bool arg_is_null, bool *result_is_null);
extern xmltype *xmlroot(xmltype *data, text *version, int standalone);
extern bool xml_is_document(xmltype *arg);
extern text *xmltotext_with_xmloption(xmltype *data, XmlOptionType xmloption_arg);

extern char *map_sql_identifier_to_xml_name(char *ident, bool fully_escaped, bool escape_period);
extern char *map_xml_name_to_sql_identifier(char *name);
extern char *map_sql_value_to_xml_value(Datum value, Oid type);

extern void AtEOXact_xml(void);

typedef enum
{
	XMLBINARY_BASE64,
	XMLBINARY_HEX
} XmlBinaryType;

extern XmlBinaryType xmlbinary;

extern XmlOptionType xmloption;

#endif   /* XML_H */
