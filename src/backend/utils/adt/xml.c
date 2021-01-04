/*-------------------------------------------------------------------------
 *
 * xml.c
 *	  XML data type support.
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/xml.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * Generally, XML type support is only available when libxml use was
 * configured during the build.  But even if that is not done, the
 * type and all the functions are available, but most of them will
 * fail.  For one thing, this avoids having to manage variant catalog
 * installations.  But it also has nice effects such as that you can
 * dump a database containing XML type data even if the server is not
 * linked with libxml.  Thus, make sure xml_out() works even if nothing
 * else does.
 */

/*
 * Notes on memory management:
 *
 * Sometimes libxml allocates global structures in the hope that it can reuse
 * them later on.  This makes it impractical to change the xmlMemSetup
 * functions on-the-fly; that is likely to lead to trying to pfree() chunks
 * allocated with malloc() or vice versa.  Since libxml might be used by
 * loadable modules, eg libperl, our only safe choices are to change the
 * functions at postmaster/backend launch or not at all.  Since we'd rather
 * not activate libxml in sessions that might never use it, the latter choice
 * is the preferred one.  However, for debugging purposes it can be awfully
 * handy to constrain libxml's allocations to be done in a specific palloc
 * context, where they're easy to track.  Therefore there is code here that
 * can be enabled in debug builds to redirect libxml's allocations into a
 * special context LibxmlContext.  It's not recommended to turn this on in
 * a production build because of the possibility of bad interactions with
 * external modules.
 */
/* #define USE_LIBXMLCONTEXT */

#include "postgres.h"

#ifdef USE_LIBXML
#include <libxml/chvalid.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlversion.h>
#include <libxml/xmlwriter.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

/*
 * We used to check for xmlStructuredErrorContext via a configure test; but
 * that doesn't work on Windows, so instead use this grottier method of
 * testing the library version number.
 */
#if LIBXML_VERSION >= 20704
#define HAVE_XMLSTRUCTUREDERRORCONTEXT 1
#endif
#endif							/* USE_LIBXML */

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "executor/tablefunc.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/xml.h"


/* GUC variables */
int			xmlbinary;
int			xmloption;

#ifdef USE_LIBXML

/* random number to identify PgXmlErrorContext */
#define ERRCXT_MAGIC	68275028

struct PgXmlErrorContext
{
	int			magic;
	/* strictness argument passed to pg_xml_init */
	PgXmlStrictness strictness;
	/* current error status and accumulated message, if any */
	bool		err_occurred;
	StringInfoData err_buf;
	/* previous libxml error handling state (saved by pg_xml_init) */
	xmlStructuredErrorFunc saved_errfunc;
	void	   *saved_errcxt;
	/* previous libxml entity handler (saved by pg_xml_init) */
	xmlExternalEntityLoader saved_entityfunc;
};

static xmlParserInputPtr xmlPgEntityLoader(const char *URL, const char *ID,
										   xmlParserCtxtPtr ctxt);
static void xml_errorHandler(void *data, xmlErrorPtr error);
static void xml_ereport_by_code(int level, int sqlcode,
								const char *msg, int errcode);
static void chopStringInfoNewlines(StringInfo str);
static void appendStringInfoLineSeparator(StringInfo str);

#ifdef USE_LIBXMLCONTEXT

static MemoryContext LibxmlContext = NULL;

static void xml_memory_init(void);
static void *xml_palloc(size_t size);
static void *xml_repalloc(void *ptr, size_t size);
static void xml_pfree(void *ptr);
static char *xml_pstrdup(const char *string);
#endif							/* USE_LIBXMLCONTEXT */

static xmlChar *xml_text2xmlChar(text *in);
static int	parse_xml_decl(const xmlChar *str, size_t *lenp,
						   xmlChar **version, xmlChar **encoding, int *standalone);
static bool print_xml_decl(StringInfo buf, const xmlChar *version,
						   pg_enc encoding, int standalone);
static bool xml_doctype_in_content(const xmlChar *str);
static xmlDocPtr xml_parse(text *data, XmlOptionType xmloption_arg,
						   bool preserve_whitespace, int encoding);
static text *xml_xmlnodetoxmltype(xmlNodePtr cur, PgXmlErrorContext *xmlerrcxt);
static int	xml_xpathobjtoxmlarray(xmlXPathObjectPtr xpathobj,
								   ArrayBuildState *astate,
								   PgXmlErrorContext *xmlerrcxt);
static xmlChar *pg_xmlCharStrndup(const char *str, size_t len);
#endif							/* USE_LIBXML */

static void xmldata_root_element_start(StringInfo result, const char *eltname,
									   const char *xmlschema, const char *targetns,
									   bool top_level);
static void xmldata_root_element_end(StringInfo result, const char *eltname);
static StringInfo query_to_xml_internal(const char *query, char *tablename,
										const char *xmlschema, bool nulls, bool tableforest,
										const char *targetns, bool top_level);
static const char *map_sql_table_to_xmlschema(TupleDesc tupdesc, Oid relid,
											  bool nulls, bool tableforest, const char *targetns);
static const char *map_sql_schema_to_xmlschema_types(Oid nspid,
													 List *relid_list, bool nulls,
													 bool tableforest, const char *targetns);
static const char *map_sql_catalog_to_xmlschema_types(List *nspid_list,
													  bool nulls, bool tableforest,
													  const char *targetns);
static const char *map_sql_type_to_xml_name(Oid typeoid, int typmod);
static const char *map_sql_typecoll_to_xmlschema_types(List *tupdesc_list);
static const char *map_sql_type_to_xmlschema_type(Oid typeoid, int typmod);
static void SPI_sql_row_to_xmlelement(uint64 rownum, StringInfo result,
									  char *tablename, bool nulls, bool tableforest,
									  const char *targetns, bool top_level);

/* XMLTABLE support */
#ifdef USE_LIBXML
/* random number to identify XmlTableContext */
#define XMLTABLE_CONTEXT_MAGIC	46922182
typedef struct XmlTableBuilderData
{
	int			magic;
	int			natts;
	long int	row_count;
	PgXmlErrorContext *xmlerrcxt;
	xmlParserCtxtPtr ctxt;
	xmlDocPtr	doc;
	xmlXPathContextPtr xpathcxt;
	xmlXPathCompExprPtr xpathcomp;
	xmlXPathObjectPtr xpathobj;
	xmlXPathCompExprPtr *xpathscomp;
} XmlTableBuilderData;
#endif

static void XmlTableInitOpaque(struct TableFuncScanState *state, int natts);
static void XmlTableSetDocument(struct TableFuncScanState *state, Datum value);
static void XmlTableSetNamespace(struct TableFuncScanState *state, const char *name,
								 const char *uri);
static void XmlTableSetRowFilter(struct TableFuncScanState *state, const char *path);
static void XmlTableSetColumnFilter(struct TableFuncScanState *state,
									const char *path, int colnum);
static bool XmlTableFetchRow(struct TableFuncScanState *state);
static Datum XmlTableGetValue(struct TableFuncScanState *state, int colnum,
							  Oid typid, int32 typmod, bool *isnull);
static void XmlTableDestroyOpaque(struct TableFuncScanState *state);

const TableFuncRoutine XmlTableRoutine =
{
	XmlTableInitOpaque,
	XmlTableSetDocument,
	XmlTableSetNamespace,
	XmlTableSetRowFilter,
	XmlTableSetColumnFilter,
	XmlTableFetchRow,
	XmlTableGetValue,
	XmlTableDestroyOpaque
};

#define NO_XML_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("unsupported XML feature"), \
			 errdetail("This functionality requires the server to be built with libxml support."), \
			 errhint("You need to rebuild PostgreSQL using --with-libxml.")))


/* from SQL/XML:2008 section 4.9 */
#define NAMESPACE_XSD "http://www.w3.org/2001/XMLSchema"
#define NAMESPACE_XSI "http://www.w3.org/2001/XMLSchema-instance"
#define NAMESPACE_SQLXML "http://standards.iso.org/iso/9075/2003/sqlxml"


#ifdef USE_LIBXML

static int
xmlChar_to_encoding(const xmlChar *encoding_name)
{
	int			encoding = pg_char_to_encoding((const char *) encoding_name);

	if (encoding < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding name \"%s\"",
						(const char *) encoding_name)));
	return encoding;
}
#endif


/*
 * xml_in uses a plain C string to VARDATA conversion, so for the time being
 * we use the conversion function for the text datatype.
 *
 * This is only acceptable so long as xmltype and text use the same
 * representation.
 */
Datum
xml_in(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	char	   *s = PG_GETARG_CSTRING(0);
	xmltype    *vardata;
	xmlDocPtr	doc;

	vardata = (xmltype *) cstring_to_text(s);

	/*
	 * Parse the data to check if it is well-formed XML data.  Assume that
	 * ERROR occurred if parsing failed.
	 */
	doc = xml_parse(vardata, xmloption, true, GetDatabaseEncoding());
	xmlFreeDoc(doc);

	PG_RETURN_XML_P(vardata);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}


#define PG_XML_DEFAULT_VERSION "1.0"


/*
 * xml_out_internal uses a plain VARDATA to C string conversion, so for the
 * time being we use the conversion function for the text datatype.
 *
 * This is only acceptable so long as xmltype and text use the same
 * representation.
 */
static char *
xml_out_internal(xmltype *x, pg_enc target_encoding)
{
	char	   *str = text_to_cstring((text *) x);

#ifdef USE_LIBXML
	size_t		len = strlen(str);
	xmlChar    *version;
	int			standalone;
	int			res_code;

	if ((res_code = parse_xml_decl((xmlChar *) str,
								   &len, &version, NULL, &standalone)) == 0)
	{
		StringInfoData buf;

		initStringInfo(&buf);

		if (!print_xml_decl(&buf, version, target_encoding, standalone))
		{
			/*
			 * If we are not going to produce an XML declaration, eat a single
			 * newline in the original string to prevent empty first lines in
			 * the output.
			 */
			if (*(str + len) == '\n')
				len += 1;
		}
		appendStringInfoString(&buf, str + len);

		pfree(str);

		return buf.data;
	}

	xml_ereport_by_code(WARNING, ERRCODE_INTERNAL_ERROR,
						"could not parse XML declaration in stored value",
						res_code);
#endif
	return str;
}


Datum
xml_out(PG_FUNCTION_ARGS)
{
	xmltype    *x = PG_GETARG_XML_P(0);

	/*
	 * xml_out removes the encoding property in all cases.  This is because we
	 * cannot control from here whether the datum will be converted to a
	 * different client encoding, so we'd do more harm than good by including
	 * it.
	 */
	PG_RETURN_CSTRING(xml_out_internal(x, 0));
}


Datum
xml_recv(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	xmltype    *result;
	char	   *str;
	char	   *newstr;
	int			nbytes;
	xmlDocPtr	doc;
	xmlChar    *encodingStr = NULL;
	int			encoding;

	/*
	 * Read the data in raw format. We don't know yet what the encoding is, as
	 * that information is embedded in the xml declaration; so we have to
	 * parse that before converting to server encoding.
	 */
	nbytes = buf->len - buf->cursor;
	str = (char *) pq_getmsgbytes(buf, nbytes);

	/*
	 * We need a null-terminated string to pass to parse_xml_decl().  Rather
	 * than make a separate copy, make the temporary result one byte bigger
	 * than it needs to be.
	 */
	result = palloc(nbytes + 1 + VARHDRSZ);
	SET_VARSIZE(result, nbytes + VARHDRSZ);
	memcpy(VARDATA(result), str, nbytes);
	str = VARDATA(result);
	str[nbytes] = '\0';

	parse_xml_decl((const xmlChar *) str, NULL, NULL, &encodingStr, NULL);

	/*
	 * If encoding wasn't explicitly specified in the XML header, treat it as
	 * UTF-8, as that's the default in XML. This is different from xml_in(),
	 * where the input has to go through the normal client to server encoding
	 * conversion.
	 */
	encoding = encodingStr ? xmlChar_to_encoding(encodingStr) : PG_UTF8;

	/*
	 * Parse the data to check if it is well-formed XML data.  Assume that
	 * xml_parse will throw ERROR if not.
	 */
	doc = xml_parse(result, xmloption, true, encoding);
	xmlFreeDoc(doc);

	/* Now that we know what we're dealing with, convert to server encoding */
	newstr = pg_any_to_server(str, nbytes, encoding);

	if (newstr != str)
	{
		pfree(result);
		result = (xmltype *) cstring_to_text(newstr);
		pfree(newstr);
	}

	PG_RETURN_XML_P(result);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}


Datum
xml_send(PG_FUNCTION_ARGS)
{
	xmltype    *x = PG_GETARG_XML_P(0);
	char	   *outval;
	StringInfoData buf;

	/*
	 * xml_out_internal doesn't convert the encoding, it just prints the right
	 * declaration. pq_sendtext will do the conversion.
	 */
	outval = xml_out_internal(x, pg_get_client_encoding());

	pq_begintypsend(&buf);
	pq_sendtext(&buf, outval, strlen(outval));
	pfree(outval);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


#ifdef USE_LIBXML
static void
appendStringInfoText(StringInfo str, const text *t)
{
	appendBinaryStringInfo(str, VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
}
#endif


static xmltype *
stringinfo_to_xmltype(StringInfo buf)
{
	return (xmltype *) cstring_to_text_with_len(buf->data, buf->len);
}


static xmltype *
cstring_to_xmltype(const char *string)
{
	return (xmltype *) cstring_to_text(string);
}


#ifdef USE_LIBXML
static xmltype *
xmlBuffer_to_xmltype(xmlBufferPtr buf)
{
	return (xmltype *) cstring_to_text_with_len((const char *) xmlBufferContent(buf),
												xmlBufferLength(buf));
}
#endif


Datum
xmlcomment(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *arg = PG_GETARG_TEXT_PP(0);
	char	   *argdata = VARDATA_ANY(arg);
	int			len = VARSIZE_ANY_EXHDR(arg);
	StringInfoData buf;
	int			i;

	/* check for "--" in string or "-" at the end */
	for (i = 1; i < len; i++)
	{
		if (argdata[i] == '-' && argdata[i - 1] == '-')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_XML_COMMENT),
					 errmsg("invalid XML comment")));
	}
	if (len > 0 && argdata[len - 1] == '-')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_XML_COMMENT),
				 errmsg("invalid XML comment")));

	initStringInfo(&buf);
	appendStringInfoString(&buf, "<!--");
	appendStringInfoText(&buf, arg);
	appendStringInfoString(&buf, "-->");

	PG_RETURN_XML_P(stringinfo_to_xmltype(&buf));
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}



/*
 * TODO: xmlconcat needs to merge the notations and unparsed entities
 * of the argument values.  Not very important in practice, though.
 */
xmltype *
xmlconcat(List *args)
{
#ifdef USE_LIBXML
	int			global_standalone = 1;
	xmlChar    *global_version = NULL;
	bool		global_version_no_value = false;
	StringInfoData buf;
	ListCell   *v;

	initStringInfo(&buf);
	foreach(v, args)
	{
		xmltype    *x = DatumGetXmlP(PointerGetDatum(lfirst(v)));
		size_t		len;
		xmlChar    *version;
		int			standalone;
		char	   *str;

		len = VARSIZE(x) - VARHDRSZ;
		str = text_to_cstring((text *) x);

		parse_xml_decl((xmlChar *) str, &len, &version, NULL, &standalone);

		if (standalone == 0 && global_standalone == 1)
			global_standalone = 0;
		if (standalone < 0)
			global_standalone = -1;

		if (!version)
			global_version_no_value = true;
		else if (!global_version)
			global_version = version;
		else if (xmlStrcmp(version, global_version) != 0)
			global_version_no_value = true;

		appendStringInfoString(&buf, str + len);
		pfree(str);
	}

	if (!global_version_no_value || global_standalone >= 0)
	{
		StringInfoData buf2;

		initStringInfo(&buf2);

		print_xml_decl(&buf2,
					   (!global_version_no_value) ? global_version : NULL,
					   0,
					   global_standalone);

		appendBinaryStringInfo(&buf2, buf.data, buf.len);
		buf = buf2;
	}

	return stringinfo_to_xmltype(&buf);
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


/*
 * XMLAGG support
 */
Datum
xmlconcat2(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();
		else
			PG_RETURN_XML_P(PG_GETARG_XML_P(1));
	}
	else if (PG_ARGISNULL(1))
		PG_RETURN_XML_P(PG_GETARG_XML_P(0));
	else
		PG_RETURN_XML_P(xmlconcat(list_make2(PG_GETARG_XML_P(0),
											 PG_GETARG_XML_P(1))));
}


Datum
texttoxml(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_PP(0);

	PG_RETURN_XML_P(xmlparse(data, xmloption, true));
}


Datum
xmltotext(PG_FUNCTION_ARGS)
{
	xmltype    *data = PG_GETARG_XML_P(0);

	/* It's actually binary compatible. */
	PG_RETURN_TEXT_P((text *) data);
}


text *
xmltotext_with_xmloption(xmltype *data, XmlOptionType xmloption_arg)
{
	if (xmloption_arg == XMLOPTION_DOCUMENT && !xml_is_document(data))
		ereport(ERROR,
				(errcode(ERRCODE_NOT_AN_XML_DOCUMENT),
				 errmsg("not an XML document")));

	/* It's actually binary compatible, save for the above check. */
	return (text *) data;
}


xmltype *
xmlelement(XmlExpr *xexpr,
		   Datum *named_argvalue, bool *named_argnull,
		   Datum *argvalue, bool *argnull)
{
#ifdef USE_LIBXML
	xmltype    *result;
	List	   *named_arg_strings;
	List	   *arg_strings;
	int			i;
	ListCell   *arg;
	ListCell   *narg;
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlBufferPtr buf = NULL;
	volatile xmlTextWriterPtr writer = NULL;

	/*
	 * All arguments are already evaluated, and their values are passed in the
	 * named_argvalue/named_argnull or argvalue/argnull arrays.  This avoids
	 * issues if one of the arguments involves a call to some other function
	 * or subsystem that wants to use libxml on its own terms.  We examine the
	 * original XmlExpr to identify the numbers and types of the arguments.
	 */
	named_arg_strings = NIL;
	i = 0;
	foreach(arg, xexpr->named_args)
	{
		Expr	   *e = (Expr *) lfirst(arg);
		char	   *str;

		if (named_argnull[i])
			str = NULL;
		else
			str = map_sql_value_to_xml_value(named_argvalue[i],
											 exprType((Node *) e),
											 false);
		named_arg_strings = lappend(named_arg_strings, str);
		i++;
	}

	arg_strings = NIL;
	i = 0;
	foreach(arg, xexpr->args)
	{
		Expr	   *e = (Expr *) lfirst(arg);
		char	   *str;

		/* here we can just forget NULL elements immediately */
		if (!argnull[i])
		{
			str = map_sql_value_to_xml_value(argvalue[i],
											 exprType((Node *) e),
											 true);
			arg_strings = lappend(arg_strings, str);
		}
		i++;
	}

	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

	PG_TRY();
	{
		buf = xmlBufferCreate();
		if (buf == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate xmlBuffer");
		writer = xmlNewTextWriterMemory(buf, 0);
		if (writer == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate xmlTextWriter");

		xmlTextWriterStartElement(writer, (xmlChar *) xexpr->name);

		forboth(arg, named_arg_strings, narg, xexpr->arg_names)
		{
			char	   *str = (char *) lfirst(arg);
			char	   *argname = strVal(lfirst(narg));

			if (str)
				xmlTextWriterWriteAttribute(writer,
											(xmlChar *) argname,
											(xmlChar *) str);
		}

		foreach(arg, arg_strings)
		{
			char	   *str = (char *) lfirst(arg);

			xmlTextWriterWriteRaw(writer, (xmlChar *) str);
		}

		xmlTextWriterEndElement(writer);

		/* we MUST do this now to flush data out to the buffer ... */
		xmlFreeTextWriter(writer);
		writer = NULL;

		result = xmlBuffer_to_xmltype(buf);
	}
	PG_CATCH();
	{
		if (writer)
			xmlFreeTextWriter(writer);
		if (buf)
			xmlBufferFree(buf);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xmlBufferFree(buf);

	pg_xml_done(xmlerrcxt, false);

	return result;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlparse(text *data, XmlOptionType xmloption_arg, bool preserve_whitespace)
{
#ifdef USE_LIBXML
	xmlDocPtr	doc;

	doc = xml_parse(data, xmloption_arg, preserve_whitespace,
					GetDatabaseEncoding());
	xmlFreeDoc(doc);

	return (xmltype *) data;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlpi(const char *target, text *arg, bool arg_is_null, bool *result_is_null)
{
#ifdef USE_LIBXML
	xmltype    *result;
	StringInfoData buf;

	if (pg_strcasecmp(target, "xml") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR), /* really */
				 errmsg("invalid XML processing instruction"),
				 errdetail("XML processing instruction target name cannot be \"%s\".", target)));

	/*
	 * Following the SQL standard, the null check comes after the syntax check
	 * above.
	 */
	*result_is_null = arg_is_null;
	if (*result_is_null)
		return NULL;

	initStringInfo(&buf);

	appendStringInfo(&buf, "<?%s", target);

	if (arg != NULL)
	{
		char	   *string;

		string = text_to_cstring(arg);
		if (strstr(string, "?>") != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_XML_PROCESSING_INSTRUCTION),
					 errmsg("invalid XML processing instruction"),
					 errdetail("XML processing instruction cannot contain \"?>\".")));

		appendStringInfoChar(&buf, ' ');
		appendStringInfoString(&buf, string + strspn(string, " "));
		pfree(string);
	}
	appendStringInfoString(&buf, "?>");

	result = stringinfo_to_xmltype(&buf);
	pfree(buf.data);
	return result;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlroot(xmltype *data, text *version, int standalone)
{
#ifdef USE_LIBXML
	char	   *str;
	size_t		len;
	xmlChar    *orig_version;
	int			orig_standalone;
	StringInfoData buf;

	len = VARSIZE(data) - VARHDRSZ;
	str = text_to_cstring((text *) data);

	parse_xml_decl((xmlChar *) str, &len, &orig_version, NULL, &orig_standalone);

	if (version)
		orig_version = xml_text2xmlChar(version);
	else
		orig_version = NULL;

	switch (standalone)
	{
		case XML_STANDALONE_YES:
			orig_standalone = 1;
			break;
		case XML_STANDALONE_NO:
			orig_standalone = 0;
			break;
		case XML_STANDALONE_NO_VALUE:
			orig_standalone = -1;
			break;
		case XML_STANDALONE_OMITTED:
			/* leave original value */
			break;
	}

	initStringInfo(&buf);
	print_xml_decl(&buf, orig_version, 0, orig_standalone);
	appendStringInfoString(&buf, str + len);

	return stringinfo_to_xmltype(&buf);
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


/*
 * Validate document (given as string) against DTD (given as external link)
 *
 * This has been removed because it is a security hole: unprivileged users
 * should not be able to use Postgres to fetch arbitrary external files,
 * which unfortunately is exactly what libxml is willing to do with the DTD
 * parameter.
 */
Datum
xmlvalidate(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("xmlvalidate is not implemented")));
	return 0;
}


bool
xml_is_document(xmltype *arg)
{
#ifdef USE_LIBXML
	bool		result;
	volatile xmlDocPtr doc = NULL;
	MemoryContext ccxt = CurrentMemoryContext;

	/* We want to catch ereport(INVALID_XML_DOCUMENT) and return false */
	PG_TRY();
	{
		doc = xml_parse((text *) arg, XMLOPTION_DOCUMENT, true,
						GetDatabaseEncoding());
		result = true;
	}
	PG_CATCH();
	{
		ErrorData  *errdata;
		MemoryContext ecxt;

		ecxt = MemoryContextSwitchTo(ccxt);
		errdata = CopyErrorData();
		if (errdata->sqlerrcode == ERRCODE_INVALID_XML_DOCUMENT)
		{
			FlushErrorState();
			result = false;
		}
		else
		{
			MemoryContextSwitchTo(ecxt);
			PG_RE_THROW();
		}
	}
	PG_END_TRY();

	if (doc)
		xmlFreeDoc(doc);

	return result;
#else							/* not USE_LIBXML */
	NO_XML_SUPPORT();
	return false;
#endif							/* not USE_LIBXML */
}


#ifdef USE_LIBXML

/*
 * pg_xml_init_library --- set up for use of libxml
 *
 * This should be called by each function that is about to use libxml
 * facilities but doesn't require error handling.  It initializes libxml
 * and verifies compatibility with the loaded libxml version.  These are
 * once-per-session activities.
 *
 * TODO: xmlChar is utf8-char, make proper tuning (initdb with enc!=utf8 and
 * check)
 */
void
pg_xml_init_library(void)
{
	static bool first_time = true;

	if (first_time)
	{
		/* Stuff we need do only once per session */

		/*
		 * Currently, we have no pure UTF-8 support for internals -- check if
		 * we can work.
		 */
		if (sizeof(char) != sizeof(xmlChar))
			ereport(ERROR,
					(errmsg("could not initialize XML library"),
					 errdetail("libxml2 has incompatible char type: sizeof(char)=%u, sizeof(xmlChar)=%u.",
							   (int) sizeof(char), (int) sizeof(xmlChar))));

#ifdef USE_LIBXMLCONTEXT
		/* Set up libxml's memory allocation our way */
		xml_memory_init();
#endif

		/* Check library compatibility */
		LIBXML_TEST_VERSION;

		first_time = false;
	}
}

/*
 * pg_xml_init --- set up for use of libxml and register an error handler
 *
 * This should be called by each function that is about to use libxml
 * facilities and requires error handling.  It initializes libxml with
 * pg_xml_init_library() and establishes our libxml error handler.
 *
 * strictness determines which errors are reported and which are ignored.
 *
 * Calls to this function MUST be followed by a PG_TRY block that guarantees
 * that pg_xml_done() is called during either normal or error exit.
 *
 * This is exported for use by contrib/xml2, as well as other code that might
 * wish to share use of this module's libxml error handler.
 */
PgXmlErrorContext *
pg_xml_init(PgXmlStrictness strictness)
{
	PgXmlErrorContext *errcxt;
	void	   *new_errcxt;

	/* Do one-time setup if needed */
	pg_xml_init_library();

	/* Create error handling context structure */
	errcxt = (PgXmlErrorContext *) palloc(sizeof(PgXmlErrorContext));
	errcxt->magic = ERRCXT_MAGIC;
	errcxt->strictness = strictness;
	errcxt->err_occurred = false;
	initStringInfo(&errcxt->err_buf);

	/*
	 * Save original error handler and install ours. libxml originally didn't
	 * distinguish between the contexts for generic and for structured error
	 * handlers.  If we're using an old libxml version, we must thus save the
	 * generic error context, even though we're using a structured error
	 * handler.
	 */
	errcxt->saved_errfunc = xmlStructuredError;

#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
	errcxt->saved_errcxt = xmlStructuredErrorContext;
#else
	errcxt->saved_errcxt = xmlGenericErrorContext;
#endif

	xmlSetStructuredErrorFunc((void *) errcxt, xml_errorHandler);

	/*
	 * Verify that xmlSetStructuredErrorFunc set the context variable we
	 * expected it to.  If not, the error context pointer we just saved is not
	 * the correct thing to restore, and since that leaves us without a way to
	 * restore the context in pg_xml_done, we must fail.
	 *
	 * The only known situation in which this test fails is if we compile with
	 * headers from a libxml2 that doesn't track the structured error context
	 * separately (< 2.7.4), but at runtime use a version that does, or vice
	 * versa.  The libxml2 authors did not treat that change as constituting
	 * an ABI break, so the LIBXML_TEST_VERSION test in pg_xml_init_library
	 * fails to protect us from this.
	 */

#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
	new_errcxt = xmlStructuredErrorContext;
#else
	new_errcxt = xmlGenericErrorContext;
#endif

	if (new_errcxt != (void *) errcxt)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not set up XML error handler"),
				 errhint("This probably indicates that the version of libxml2"
						 " being used is not compatible with the libxml2"
						 " header files that PostgreSQL was built with.")));

	/*
	 * Also, install an entity loader to prevent unwanted fetches of external
	 * files and URLs.
	 */
	errcxt->saved_entityfunc = xmlGetExternalEntityLoader();
	xmlSetExternalEntityLoader(xmlPgEntityLoader);

	return errcxt;
}


/*
 * pg_xml_done --- restore previous libxml error handling
 *
 * Resets libxml's global error-handling state to what it was before
 * pg_xml_init() was called.
 *
 * This routine verifies that all pending errors have been dealt with
 * (in assert-enabled builds, anyway).
 */
void
pg_xml_done(PgXmlErrorContext *errcxt, bool isError)
{
	void	   *cur_errcxt;

	/* An assert seems like enough protection here */
	Assert(errcxt->magic == ERRCXT_MAGIC);

	/*
	 * In a normal exit, there should be no un-handled libxml errors.  But we
	 * shouldn't try to enforce this during error recovery, since the longjmp
	 * could have been thrown before xml_ereport had a chance to run.
	 */
	Assert(!errcxt->err_occurred || isError);

	/*
	 * Check that libxml's global state is correct, warn if not.  This is a
	 * real test and not an Assert because it has a higher probability of
	 * happening.
	 */
#ifdef HAVE_XMLSTRUCTUREDERRORCONTEXT
	cur_errcxt = xmlStructuredErrorContext;
#else
	cur_errcxt = xmlGenericErrorContext;
#endif

	if (cur_errcxt != (void *) errcxt)
		elog(WARNING, "libxml error handling state is out of sync with xml.c");

	/* Restore the saved handlers */
	xmlSetStructuredErrorFunc(errcxt->saved_errcxt, errcxt->saved_errfunc);
	xmlSetExternalEntityLoader(errcxt->saved_entityfunc);

	/*
	 * Mark the struct as invalid, just in case somebody somehow manages to
	 * call xml_errorHandler or xml_ereport with it.
	 */
	errcxt->magic = 0;

	/* Release memory */
	pfree(errcxt->err_buf.data);
	pfree(errcxt);
}


/*
 * pg_xml_error_occurred() --- test the error flag
 */
bool
pg_xml_error_occurred(PgXmlErrorContext *errcxt)
{
	return errcxt->err_occurred;
}


/*
 * SQL/XML allows storing "XML documents" or "XML content".  "XML
 * documents" are specified by the XML specification and are parsed
 * easily by libxml.  "XML content" is specified by SQL/XML as the
 * production "XMLDecl? content".  But libxml can only parse the
 * "content" part, so we have to parse the XML declaration ourselves
 * to complete this.
 */

#define CHECK_XML_SPACE(p) \
	do { \
		if (!xmlIsBlank_ch(*(p))) \
			return XML_ERR_SPACE_REQUIRED; \
	} while (0)

#define SKIP_XML_SPACE(p) \
	while (xmlIsBlank_ch(*(p))) (p)++

/* Letter | Digit | '.' | '-' | '_' | ':' | CombiningChar | Extender */
/* Beware of multiple evaluations of argument! */
#define PG_XMLISNAMECHAR(c) \
	(xmlIsBaseChar_ch(c) || xmlIsIdeographicQ(c) \
			|| xmlIsDigit_ch(c) \
			|| c == '.' || c == '-' || c == '_' || c == ':' \
			|| xmlIsCombiningQ(c) \
			|| xmlIsExtender_ch(c))

/* pnstrdup, but deal with xmlChar not char; len is measured in xmlChars */
static xmlChar *
xml_pnstrdup(const xmlChar *str, size_t len)
{
	xmlChar    *result;

	result = (xmlChar *) palloc((len + 1) * sizeof(xmlChar));
	memcpy(result, str, len * sizeof(xmlChar));
	result[len] = 0;
	return result;
}

/* Ditto, except input is char* */
static xmlChar *
pg_xmlCharStrndup(const char *str, size_t len)
{
	xmlChar    *result;

	result = (xmlChar *) palloc((len + 1) * sizeof(xmlChar));
	memcpy(result, str, len);
	result[len] = '\0';

	return result;
}

/*
 * Copy xmlChar string to PostgreSQL-owned memory, freeing the input.
 *
 * The input xmlChar is freed regardless of success of the copy.
 */
static char *
xml_pstrdup_and_free(xmlChar *str)
{
	char	   *result;

	if (str)
	{
		PG_TRY();
		{
			result = pstrdup((char *) str);
		}
		PG_FINALLY();
		{
			xmlFree(str);
		}
		PG_END_TRY();
	}
	else
		result = NULL;

	return result;
}

/*
 * str is the null-terminated input string.  Remaining arguments are
 * output arguments; each can be NULL if value is not wanted.
 * version and encoding are returned as locally-palloc'd strings.
 * Result is 0 if OK, an error code if not.
 */
static int
parse_xml_decl(const xmlChar *str, size_t *lenp,
			   xmlChar **version, xmlChar **encoding, int *standalone)
{
	const xmlChar *p;
	const xmlChar *save_p;
	size_t		len;
	int			utf8char;
	int			utf8len;

	/*
	 * Only initialize libxml.  We don't need error handling here, but we do
	 * need to make sure libxml is initialized before calling any of its
	 * functions.  Note that this is safe (and a no-op) if caller has already
	 * done pg_xml_init().
	 */
	pg_xml_init_library();

	/* Initialize output arguments to "not present" */
	if (version)
		*version = NULL;
	if (encoding)
		*encoding = NULL;
	if (standalone)
		*standalone = -1;

	p = str;

	if (xmlStrncmp(p, (xmlChar *) "<?xml", 5) != 0)
		goto finished;

	/*
	 * If next char is a name char, it's a PI like <?xml-stylesheet ...?>
	 * rather than an XMLDecl, so we have done what we came to do and found no
	 * XMLDecl.
	 *
	 * We need an input length value for xmlGetUTF8Char, but there's no need
	 * to count the whole document size, so use strnlen not strlen.
	 */
	utf8len = strnlen((const char *) (p + 5), MAX_MULTIBYTE_CHAR_LEN);
	utf8char = xmlGetUTF8Char(p + 5, &utf8len);
	if (PG_XMLISNAMECHAR(utf8char))
		goto finished;

	p += 5;

	/* version */
	CHECK_XML_SPACE(p);
	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *) "version", 7) != 0)
		return XML_ERR_VERSION_MISSING;
	p += 7;
	SKIP_XML_SPACE(p);
	if (*p != '=')
		return XML_ERR_VERSION_MISSING;
	p += 1;
	SKIP_XML_SPACE(p);

	if (*p == '\'' || *p == '"')
	{
		const xmlChar *q;

		q = xmlStrchr(p + 1, *p);
		if (!q)
			return XML_ERR_VERSION_MISSING;

		if (version)
			*version = xml_pnstrdup(p + 1, q - p - 1);
		p = q + 1;
	}
	else
		return XML_ERR_VERSION_MISSING;

	/* encoding */
	save_p = p;
	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *) "encoding", 8) == 0)
	{
		CHECK_XML_SPACE(save_p);
		p += 8;
		SKIP_XML_SPACE(p);
		if (*p != '=')
			return XML_ERR_MISSING_ENCODING;
		p += 1;
		SKIP_XML_SPACE(p);

		if (*p == '\'' || *p == '"')
		{
			const xmlChar *q;

			q = xmlStrchr(p + 1, *p);
			if (!q)
				return XML_ERR_MISSING_ENCODING;

			if (encoding)
				*encoding = xml_pnstrdup(p + 1, q - p - 1);
			p = q + 1;
		}
		else
			return XML_ERR_MISSING_ENCODING;
	}
	else
	{
		p = save_p;
	}

	/* standalone */
	save_p = p;
	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *) "standalone", 10) == 0)
	{
		CHECK_XML_SPACE(save_p);
		p += 10;
		SKIP_XML_SPACE(p);
		if (*p != '=')
			return XML_ERR_STANDALONE_VALUE;
		p += 1;
		SKIP_XML_SPACE(p);
		if (xmlStrncmp(p, (xmlChar *) "'yes'", 5) == 0 ||
			xmlStrncmp(p, (xmlChar *) "\"yes\"", 5) == 0)
		{
			if (standalone)
				*standalone = 1;
			p += 5;
		}
		else if (xmlStrncmp(p, (xmlChar *) "'no'", 4) == 0 ||
				 xmlStrncmp(p, (xmlChar *) "\"no\"", 4) == 0)
		{
			if (standalone)
				*standalone = 0;
			p += 4;
		}
		else
			return XML_ERR_STANDALONE_VALUE;
	}
	else
	{
		p = save_p;
	}

	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *) "?>", 2) != 0)
		return XML_ERR_XMLDECL_NOT_FINISHED;
	p += 2;

finished:
	len = p - str;

	for (p = str; p < str + len; p++)
		if (*p > 127)
			return XML_ERR_INVALID_CHAR;

	if (lenp)
		*lenp = len;

	return XML_ERR_OK;
}


/*
 * Write an XML declaration.  On output, we adjust the XML declaration
 * as follows.  (These rules are the moral equivalent of the clause
 * "Serialization of an XML value" in the SQL standard.)
 *
 * We try to avoid generating an XML declaration if possible.  This is
 * so that you don't get trivial things like xml '<foo/>' resulting in
 * '<?xml version="1.0"?><foo/>', which would surely be annoying.  We
 * must provide a declaration if the standalone property is specified
 * or if we include an encoding declaration.  If we have a
 * declaration, we must specify a version (XML requires this).
 * Otherwise we only make a declaration if the version is not "1.0",
 * which is the default version specified in SQL:2003.
 */
static bool
print_xml_decl(StringInfo buf, const xmlChar *version,
			   pg_enc encoding, int standalone)
{
	if ((version && strcmp((const char *) version, PG_XML_DEFAULT_VERSION) != 0)
		|| (encoding && encoding != PG_UTF8)
		|| standalone != -1)
	{
		appendStringInfoString(buf, "<?xml");

		if (version)
			appendStringInfo(buf, " version=\"%s\"", version);
		else
			appendStringInfo(buf, " version=\"%s\"", PG_XML_DEFAULT_VERSION);

		if (encoding && encoding != PG_UTF8)
		{
			/*
			 * XXX might be useful to convert this to IANA names (ISO-8859-1
			 * instead of LATIN1 etc.); needs field experience
			 */
			appendStringInfo(buf, " encoding=\"%s\"",
							 pg_encoding_to_char(encoding));
		}

		if (standalone == 1)
			appendStringInfoString(buf, " standalone=\"yes\"");
		else if (standalone == 0)
			appendStringInfoString(buf, " standalone=\"no\"");
		appendStringInfoString(buf, "?>");

		return true;
	}
	else
		return false;
}

/*
 * Test whether an input that is to be parsed as CONTENT contains a DTD.
 *
 * The SQL/XML:2003 definition of CONTENT ("XMLDecl? content") is not
 * satisfied by a document with a DTD, which is a bit of a wart, as it means
 * the CONTENT type is not a proper superset of DOCUMENT.  SQL/XML:2006 and
 * later fix that, by redefining content with reference to the "more
 * permissive" Document Node of the XQuery/XPath Data Model, such that any
 * DOCUMENT value is indeed also a CONTENT value.  That definition is more
 * useful, as CONTENT becomes usable for parsing input of unknown form (think
 * pg_restore).
 *
 * As used below in parse_xml when parsing for CONTENT, libxml does not give
 * us the 2006+ behavior, but only the 2003; it will choke if the input has
 * a DTD.  But we can provide the 2006+ definition of CONTENT easily enough,
 * by detecting this case first and simply doing the parse as DOCUMENT.
 *
 * A DTD can be found arbitrarily far in, but that would be a contrived case;
 * it will ordinarily start within a few dozen characters.  The only things
 * that can precede it are an XMLDecl (here, the caller will have called
 * parse_xml_decl already), whitespace, comments, and processing instructions.
 * This function need only return true if it sees a valid sequence of such
 * things leading to <!DOCTYPE.  It can simply return false in any other
 * cases, including malformed input; that will mean the input gets parsed as
 * CONTENT as originally planned, with libxml reporting any errors.
 *
 * This is only to be called from xml_parse, when pg_xml_init has already
 * been called.  The input is already in UTF8 encoding.
 */
static bool
xml_doctype_in_content(const xmlChar *str)
{
	const xmlChar *p = str;

	for (;;)
	{
		const xmlChar *e;

		SKIP_XML_SPACE(p);
		if (*p != '<')
			return false;
		p++;

		if (*p == '!')
		{
			p++;

			/* if we see <!DOCTYPE, we can return true */
			if (xmlStrncmp(p, (xmlChar *) "DOCTYPE", 7) == 0)
				return true;

			/* otherwise, if it's not a comment, fail */
			if (xmlStrncmp(p, (xmlChar *) "--", 2) != 0)
				return false;
			/* find end of comment: find -- and a > must follow */
			p = xmlStrstr(p + 2, (xmlChar *) "--");
			if (!p || p[2] != '>')
				return false;
			/* advance over comment, and keep scanning */
			p += 3;
			continue;
		}

		/* otherwise, if it's not a PI <?target something?>, fail */
		if (*p != '?')
			return false;
		p++;

		/* find end of PI (the string ?> is forbidden within a PI) */
		e = xmlStrstr(p, (xmlChar *) "?>");
		if (!e)
			return false;

		/* advance over PI, keep scanning */
		p = e + 2;
	}
}


/*
 * Convert a C string to XML internal representation
 *
 * Note: it is caller's responsibility to xmlFreeDoc() the result,
 * else a permanent memory leak will ensue!
 *
 * TODO maybe libxml2's xmlreader is better? (do not construct DOM,
 * yet do not use SAX - see xmlreader.c)
 */
static xmlDocPtr
xml_parse(text *data, XmlOptionType xmloption_arg, bool preserve_whitespace,
		  int encoding)
{
	int32		len;
	xmlChar    *string;
	xmlChar    *utf8string;
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlParserCtxtPtr ctxt = NULL;
	volatile xmlDocPtr doc = NULL;

	len = VARSIZE_ANY_EXHDR(data);	/* will be useful later */
	string = xml_text2xmlChar(data);

	utf8string = pg_do_encoding_conversion(string,
										   len,
										   encoding,
										   PG_UTF8);

	/* Start up libxml and its parser */
	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_WELLFORMED);

	/* Use a TRY block to ensure we clean up correctly */
	PG_TRY();
	{
		bool		parse_as_document = false;
		int			res_code;
		size_t		count = 0;
		xmlChar    *version = NULL;
		int			standalone = 0;

		xmlInitParser();

		ctxt = xmlNewParserCtxt();
		if (ctxt == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate parser context");

		/* Decide whether to parse as document or content */
		if (xmloption_arg == XMLOPTION_DOCUMENT)
			parse_as_document = true;
		else
		{
			/* Parse and skip over the XML declaration, if any */
			res_code = parse_xml_decl(utf8string,
									  &count, &version, NULL, &standalone);
			if (res_code != 0)
				xml_ereport_by_code(ERROR, ERRCODE_INVALID_XML_CONTENT,
									"invalid XML content: invalid XML declaration",
									res_code);

			/* Is there a DOCTYPE element? */
			if (xml_doctype_in_content(utf8string + count))
				parse_as_document = true;
		}

		if (parse_as_document)
		{
			/*
			 * Note, that here we try to apply DTD defaults
			 * (XML_PARSE_DTDATTR) according to SQL/XML:2008 GR 10.16.7.d:
			 * 'Default values defined by internal DTD are applied'. As for
			 * external DTDs, we try to support them too, (see SQL/XML:2008 GR
			 * 10.16.7.e)
			 */
			doc = xmlCtxtReadDoc(ctxt, utf8string,
								 NULL,
								 "UTF-8",
								 XML_PARSE_NOENT | XML_PARSE_DTDATTR
								 | (preserve_whitespace ? 0 : XML_PARSE_NOBLANKS));
			if (doc == NULL || xmlerrcxt->err_occurred)
			{
				/* Use original option to decide which error code to throw */
				if (xmloption_arg == XMLOPTION_DOCUMENT)
					xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
								"invalid XML document");
				else
					xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_CONTENT,
								"invalid XML content");
			}
		}
		else
		{
			doc = xmlNewDoc(version);
			Assert(doc->encoding == NULL);
			doc->encoding = xmlStrdup((const xmlChar *) "UTF-8");
			doc->standalone = standalone;

			/* allow empty content */
			if (*(utf8string + count))
			{
				res_code = xmlParseBalancedChunkMemory(doc, NULL, NULL, 0,
													   utf8string + count, NULL);
				if (res_code != 0 || xmlerrcxt->err_occurred)
					xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_CONTENT,
								"invalid XML content");
			}
		}
	}
	PG_CATCH();
	{
		if (doc != NULL)
			xmlFreeDoc(doc);
		if (ctxt != NULL)
			xmlFreeParserCtxt(ctxt);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xmlFreeParserCtxt(ctxt);

	pg_xml_done(xmlerrcxt, false);

	return doc;
}


/*
 * xmlChar<->text conversions
 */
static xmlChar *
xml_text2xmlChar(text *in)
{
	return (xmlChar *) text_to_cstring(in);
}


#ifdef USE_LIBXMLCONTEXT

/*
 * Manage the special context used for all libxml allocations (but only
 * in special debug builds; see notes at top of file)
 */
static void
xml_memory_init(void)
{
	/* Create memory context if not there already */
	if (LibxmlContext == NULL)
		LibxmlContext = AllocSetContextCreate(TopMemoryContext,
											  "Libxml context",
											  ALLOCSET_DEFAULT_SIZES);

	/* Re-establish the callbacks even if already set */
	xmlMemSetup(xml_pfree, xml_palloc, xml_repalloc, xml_pstrdup);
}

/*
 * Wrappers for memory management functions
 */
static void *
xml_palloc(size_t size)
{
	return MemoryContextAlloc(LibxmlContext, size);
}


static void *
xml_repalloc(void *ptr, size_t size)
{
	return repalloc(ptr, size);
}


static void
xml_pfree(void *ptr)
{
	/* At least some parts of libxml assume xmlFree(NULL) is allowed */
	if (ptr)
		pfree(ptr);
}


static char *
xml_pstrdup(const char *string)
{
	return MemoryContextStrdup(LibxmlContext, string);
}
#endif							/* USE_LIBXMLCONTEXT */


/*
 * xmlPgEntityLoader --- entity loader callback function
 *
 * Silently prevent any external entity URL from being loaded.  We don't want
 * to throw an error, so instead make the entity appear to expand to an empty
 * string.
 *
 * We would prefer to allow loading entities that exist in the system's
 * global XML catalog; but the available libxml2 APIs make that a complex
 * and fragile task.  For now, just shut down all external access.
 */
static xmlParserInputPtr
xmlPgEntityLoader(const char *URL, const char *ID,
				  xmlParserCtxtPtr ctxt)
{
	return xmlNewStringInputStream(ctxt, (const xmlChar *) "");
}


/*
 * xml_ereport --- report an XML-related error
 *
 * The "msg" is the SQL-level message; some can be adopted from the SQL/XML
 * standard.  This function adds libxml's native error message, if any, as
 * detail.
 *
 * This is exported for modules that want to share the core libxml error
 * handler.  Note that pg_xml_init() *must* have been called previously.
 */
void
xml_ereport(PgXmlErrorContext *errcxt, int level, int sqlcode, const char *msg)
{
	char	   *detail;

	/* Defend against someone passing us a bogus context struct */
	if (errcxt->magic != ERRCXT_MAGIC)
		elog(ERROR, "xml_ereport called with invalid PgXmlErrorContext");

	/* Flag that the current libxml error has been reported */
	errcxt->err_occurred = false;

	/* Include detail only if we have some text from libxml */
	if (errcxt->err_buf.len > 0)
		detail = errcxt->err_buf.data;
	else
		detail = NULL;

	ereport(level,
			(errcode(sqlcode),
			 errmsg_internal("%s", msg),
			 detail ? errdetail_internal("%s", detail) : 0));
}


/*
 * Error handler for libxml errors and warnings
 */
static void
xml_errorHandler(void *data, xmlErrorPtr error)
{
	PgXmlErrorContext *xmlerrcxt = (PgXmlErrorContext *) data;
	xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) error->ctxt;
	xmlParserInputPtr input = (ctxt != NULL) ? ctxt->input : NULL;
	xmlNodePtr	node = error->node;
	const xmlChar *name = (node != NULL &&
						   node->type == XML_ELEMENT_NODE) ? node->name : NULL;
	int			domain = error->domain;
	int			level = error->level;
	StringInfo	errorBuf;

	/*
	 * Defend against someone passing us a bogus context struct.
	 *
	 * We force a backend exit if this check fails because longjmp'ing out of
	 * libxml would likely render it unsafe to use further.
	 */
	if (xmlerrcxt->magic != ERRCXT_MAGIC)
		elog(FATAL, "xml_errorHandler called with invalid PgXmlErrorContext");

	/*----------
	 * Older libxml versions report some errors differently.
	 * First, some errors were previously reported as coming from the parser
	 * domain but are now reported as coming from the namespace domain.
	 * Second, some warnings were upgraded to errors.
	 * We attempt to compensate for that here.
	 *----------
	 */
	switch (error->code)
	{
		case XML_WAR_NS_URI:
			level = XML_ERR_ERROR;
			domain = XML_FROM_NAMESPACE;
			break;

		case XML_ERR_NS_DECL_ERROR:
		case XML_WAR_NS_URI_RELATIVE:
		case XML_WAR_NS_COLUMN:
		case XML_NS_ERR_XML_NAMESPACE:
		case XML_NS_ERR_UNDEFINED_NAMESPACE:
		case XML_NS_ERR_QNAME:
		case XML_NS_ERR_ATTRIBUTE_REDEFINED:
		case XML_NS_ERR_EMPTY:
			domain = XML_FROM_NAMESPACE;
			break;
	}

	/* Decide whether to act on the error or not */
	switch (domain)
	{
		case XML_FROM_PARSER:
		case XML_FROM_NONE:
		case XML_FROM_MEMORY:
		case XML_FROM_IO:

			/*
			 * Suppress warnings about undeclared entities.  We need to do
			 * this to avoid problems due to not loading DTD definitions.
			 */
			if (error->code == XML_WAR_UNDECLARED_ENTITY)
				return;

			/* Otherwise, accept error regardless of the parsing purpose */
			break;

		default:
			/* Ignore error if only doing well-formedness check */
			if (xmlerrcxt->strictness == PG_XML_STRICTNESS_WELLFORMED)
				return;
			break;
	}

	/* Prepare error message in errorBuf */
	errorBuf = makeStringInfo();

	if (error->line > 0)
		appendStringInfo(errorBuf, "line %d: ", error->line);
	if (name != NULL)
		appendStringInfo(errorBuf, "element %s: ", name);
	if (error->message != NULL)
		appendStringInfoString(errorBuf, error->message);
	else
		appendStringInfoString(errorBuf, "(no message provided)");

	/*
	 * Append context information to errorBuf.
	 *
	 * xmlParserPrintFileContext() uses libxml's "generic" error handler to
	 * write the context.  Since we don't want to duplicate libxml
	 * functionality here, we set up a generic error handler temporarily.
	 *
	 * We use appendStringInfo() directly as libxml's generic error handler.
	 * This should work because it has essentially the same signature as
	 * libxml expects, namely (void *ptr, const char *msg, ...).
	 */
	if (input != NULL)
	{
		xmlGenericErrorFunc errFuncSaved = xmlGenericError;
		void	   *errCtxSaved = xmlGenericErrorContext;

		xmlSetGenericErrorFunc((void *) errorBuf,
							   (xmlGenericErrorFunc) appendStringInfo);

		/* Add context information to errorBuf */
		appendStringInfoLineSeparator(errorBuf);

		xmlParserPrintFileContext(input);

		/* Restore generic error func */
		xmlSetGenericErrorFunc(errCtxSaved, errFuncSaved);
	}

	/* Get rid of any trailing newlines in errorBuf */
	chopStringInfoNewlines(errorBuf);

	/*
	 * Legacy error handling mode.  err_occurred is never set, we just add the
	 * message to err_buf.  This mode exists because the xml2 contrib module
	 * uses our error-handling infrastructure, but we don't want to change its
	 * behaviour since it's deprecated anyway.  This is also why we don't
	 * distinguish between notices, warnings and errors here --- the old-style
	 * generic error handler wouldn't have done that either.
	 */
	if (xmlerrcxt->strictness == PG_XML_STRICTNESS_LEGACY)
	{
		appendStringInfoLineSeparator(&xmlerrcxt->err_buf);
		appendBinaryStringInfo(&xmlerrcxt->err_buf, errorBuf->data,
							   errorBuf->len);

		pfree(errorBuf->data);
		pfree(errorBuf);
		return;
	}

	/*
	 * We don't want to ereport() here because that'd probably leave libxml in
	 * an inconsistent state.  Instead, we remember the error and ereport()
	 * from xml_ereport().
	 *
	 * Warnings and notices can be reported immediately since they won't cause
	 * a longjmp() out of libxml.
	 */
	if (level >= XML_ERR_ERROR)
	{
		appendStringInfoLineSeparator(&xmlerrcxt->err_buf);
		appendBinaryStringInfo(&xmlerrcxt->err_buf, errorBuf->data,
							   errorBuf->len);

		xmlerrcxt->err_occurred = true;
	}
	else if (level >= XML_ERR_WARNING)
	{
		ereport(WARNING,
				(errmsg_internal("%s", errorBuf->data)));
	}
	else
	{
		ereport(NOTICE,
				(errmsg_internal("%s", errorBuf->data)));
	}

	pfree(errorBuf->data);
	pfree(errorBuf);
}


/*
 * Wrapper for "ereport" function for XML-related errors.  The "msg"
 * is the SQL-level message; some can be adopted from the SQL/XML
 * standard.  This function uses "code" to create a textual detail
 * message.  At the moment, we only need to cover those codes that we
 * may raise in this file.
 */
static void
xml_ereport_by_code(int level, int sqlcode,
					const char *msg, int code)
{
	const char *det;

	switch (code)
	{
		case XML_ERR_INVALID_CHAR:
			det = gettext_noop("Invalid character value.");
			break;
		case XML_ERR_SPACE_REQUIRED:
			det = gettext_noop("Space required.");
			break;
		case XML_ERR_STANDALONE_VALUE:
			det = gettext_noop("standalone accepts only 'yes' or 'no'.");
			break;
		case XML_ERR_VERSION_MISSING:
			det = gettext_noop("Malformed declaration: missing version.");
			break;
		case XML_ERR_MISSING_ENCODING:
			det = gettext_noop("Missing encoding in text declaration.");
			break;
		case XML_ERR_XMLDECL_NOT_FINISHED:
			det = gettext_noop("Parsing XML declaration: '?>' expected.");
			break;
		default:
			det = gettext_noop("Unrecognized libxml error code: %d.");
			break;
	}

	ereport(level,
			(errcode(sqlcode),
			 errmsg_internal("%s", msg),
			 errdetail(det, code)));
}


/*
 * Remove all trailing newlines from a StringInfo string
 */
static void
chopStringInfoNewlines(StringInfo str)
{
	while (str->len > 0 && str->data[str->len - 1] == '\n')
		str->data[--str->len] = '\0';
}


/*
 * Append a newline after removing any existing trailing newlines
 */
static void
appendStringInfoLineSeparator(StringInfo str)
{
	chopStringInfoNewlines(str);
	if (str->len > 0)
		appendStringInfoChar(str, '\n');
}


/*
 * Convert one char in the current server encoding to a Unicode codepoint.
 */
static pg_wchar
sqlchar_to_unicode(const char *s)
{
	char	   *utf8string;
	pg_wchar	ret[2];			/* need space for trailing zero */

	/* note we're not assuming s is null-terminated */
	utf8string = pg_server_to_any(s, pg_mblen(s), PG_UTF8);

	pg_encoding_mb2wchar_with_len(PG_UTF8, utf8string, ret,
								  pg_encoding_mblen(PG_UTF8, utf8string));

	if (utf8string != s)
		pfree(utf8string);

	return ret[0];
}


static bool
is_valid_xml_namefirst(pg_wchar c)
{
	/* (Letter | '_' | ':') */
	return (xmlIsBaseCharQ(c) || xmlIsIdeographicQ(c)
			|| c == '_' || c == ':');
}


static bool
is_valid_xml_namechar(pg_wchar c)
{
	/* Letter | Digit | '.' | '-' | '_' | ':' | CombiningChar | Extender */
	return (xmlIsBaseCharQ(c) || xmlIsIdeographicQ(c)
			|| xmlIsDigitQ(c)
			|| c == '.' || c == '-' || c == '_' || c == ':'
			|| xmlIsCombiningQ(c)
			|| xmlIsExtenderQ(c));
}
#endif							/* USE_LIBXML */


/*
 * Map SQL identifier to XML name; see SQL/XML:2008 section 9.1.
 */
char *
map_sql_identifier_to_xml_name(const char *ident, bool fully_escaped,
							   bool escape_period)
{
#ifdef USE_LIBXML
	StringInfoData buf;
	const char *p;

	/*
	 * SQL/XML doesn't make use of this case anywhere, so it's probably a
	 * mistake.
	 */
	Assert(fully_escaped || !escape_period);

	initStringInfo(&buf);

	for (p = ident; *p; p += pg_mblen(p))
	{
		if (*p == ':' && (p == ident || fully_escaped))
			appendStringInfoString(&buf, "_x003A_");
		else if (*p == '_' && *(p + 1) == 'x')
			appendStringInfoString(&buf, "_x005F_");
		else if (fully_escaped && p == ident &&
				 pg_strncasecmp(p, "xml", 3) == 0)
		{
			if (*p == 'x')
				appendStringInfoString(&buf, "_x0078_");
			else
				appendStringInfoString(&buf, "_x0058_");
		}
		else if (escape_period && *p == '.')
			appendStringInfoString(&buf, "_x002E_");
		else
		{
			pg_wchar	u = sqlchar_to_unicode(p);

			if ((p == ident)
				? !is_valid_xml_namefirst(u)
				: !is_valid_xml_namechar(u))
				appendStringInfo(&buf, "_x%04X_", (unsigned int) u);
			else
				appendBinaryStringInfo(&buf, p, pg_mblen(p));
		}
	}

	return buf.data;
#else							/* not USE_LIBXML */
	NO_XML_SUPPORT();
	return NULL;
#endif							/* not USE_LIBXML */
}


/*
 * Map XML name to SQL identifier; see SQL/XML:2008 section 9.3.
 */
char *
map_xml_name_to_sql_identifier(const char *name)
{
	StringInfoData buf;
	const char *p;

	initStringInfo(&buf);

	for (p = name; *p; p += pg_mblen(p))
	{
		if (*p == '_' && *(p + 1) == 'x'
			&& isxdigit((unsigned char) *(p + 2))
			&& isxdigit((unsigned char) *(p + 3))
			&& isxdigit((unsigned char) *(p + 4))
			&& isxdigit((unsigned char) *(p + 5))
			&& *(p + 6) == '_')
		{
			char		cbuf[MAX_UNICODE_EQUIVALENT_STRING + 1];
			unsigned int u;

			sscanf(p + 2, "%X", &u);
			pg_unicode_to_server(u, (unsigned char *) cbuf);
			appendStringInfoString(&buf, cbuf);
			p += 6;
		}
		else
			appendBinaryStringInfo(&buf, p, pg_mblen(p));
	}

	return buf.data;
}

/*
 * Map SQL value to XML value; see SQL/XML:2008 section 9.8.
 *
 * When xml_escape_strings is true, then certain characters in string
 * values are replaced by entity references (&lt; etc.), as specified
 * in SQL/XML:2008 section 9.8 GR 9) a) iii).   This is normally what is
 * wanted.  The false case is mainly useful when the resulting value
 * is used with xmlTextWriterWriteAttribute() to write out an
 * attribute, because that function does the escaping itself.
 */
char *
map_sql_value_to_xml_value(Datum value, Oid type, bool xml_escape_strings)
{
	if (type_is_array_domain(type))
	{
		ArrayType  *array;
		Oid			elmtype;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		StringInfoData buf;
		int			i;

		array = DatumGetArrayTypeP(value);
		elmtype = ARR_ELEMTYPE(array);
		get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

		deconstruct_array(array, elmtype,
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls,
						  &num_elems);

		initStringInfo(&buf);

		for (i = 0; i < num_elems; i++)
		{
			if (elem_nulls[i])
				continue;
			appendStringInfoString(&buf, "<element>");
			appendStringInfoString(&buf,
								   map_sql_value_to_xml_value(elem_values[i],
															  elmtype, true));
			appendStringInfoString(&buf, "</element>");
		}

		pfree(elem_values);
		pfree(elem_nulls);

		return buf.data;
	}
	else
	{
		Oid			typeOut;
		bool		isvarlena;
		char	   *str;

		/*
		 * Flatten domains; the special-case treatments below should apply to,
		 * eg, domains over boolean not just boolean.
		 */
		type = getBaseType(type);

		/*
		 * Special XSD formatting for some data types
		 */
		switch (type)
		{
			case BOOLOID:
				if (DatumGetBool(value))
					return "true";
				else
					return "false";

			case DATEOID:
				{
					DateADT		date;
					struct pg_tm tm;
					char		buf[MAXDATELEN + 1];

					date = DatumGetDateADT(value);
					/* XSD doesn't support infinite values */
					if (DATE_NOT_FINITE(date))
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("date out of range"),
								 errdetail("XML does not support infinite date values.")));
					j2date(date + POSTGRES_EPOCH_JDATE,
						   &(tm.tm_year), &(tm.tm_mon), &(tm.tm_mday));
					EncodeDateOnly(&tm, USE_XSD_DATES, buf);

					return pstrdup(buf);
				}

			case TIMESTAMPOID:
				{
					Timestamp	timestamp;
					struct pg_tm tm;
					fsec_t		fsec;
					char		buf[MAXDATELEN + 1];

					timestamp = DatumGetTimestamp(value);

					/* XSD doesn't support infinite values */
					if (TIMESTAMP_NOT_FINITE(timestamp))
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range"),
								 errdetail("XML does not support infinite timestamp values.")));
					else if (timestamp2tm(timestamp, NULL, &tm, &fsec, NULL, NULL) == 0)
						EncodeDateTime(&tm, fsec, false, 0, NULL, USE_XSD_DATES, buf);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range")));

					return pstrdup(buf);
				}

			case TIMESTAMPTZOID:
				{
					TimestampTz timestamp;
					struct pg_tm tm;
					int			tz;
					fsec_t		fsec;
					const char *tzn = NULL;
					char		buf[MAXDATELEN + 1];

					timestamp = DatumGetTimestamp(value);

					/* XSD doesn't support infinite values */
					if (TIMESTAMP_NOT_FINITE(timestamp))
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range"),
								 errdetail("XML does not support infinite timestamp values.")));
					else if (timestamp2tm(timestamp, &tz, &tm, &fsec, &tzn, NULL) == 0)
						EncodeDateTime(&tm, fsec, true, tz, tzn, USE_XSD_DATES, buf);
					else
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range")));

					return pstrdup(buf);
				}

#ifdef USE_LIBXML
			case BYTEAOID:
				{
					bytea	   *bstr = DatumGetByteaPP(value);
					PgXmlErrorContext *xmlerrcxt;
					volatile xmlBufferPtr buf = NULL;
					volatile xmlTextWriterPtr writer = NULL;
					char	   *result;

					xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

					PG_TRY();
					{
						buf = xmlBufferCreate();
						if (buf == NULL || xmlerrcxt->err_occurred)
							xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
										"could not allocate xmlBuffer");
						writer = xmlNewTextWriterMemory(buf, 0);
						if (writer == NULL || xmlerrcxt->err_occurred)
							xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
										"could not allocate xmlTextWriter");

						if (xmlbinary == XMLBINARY_BASE64)
							xmlTextWriterWriteBase64(writer, VARDATA_ANY(bstr),
													 0, VARSIZE_ANY_EXHDR(bstr));
						else
							xmlTextWriterWriteBinHex(writer, VARDATA_ANY(bstr),
													 0, VARSIZE_ANY_EXHDR(bstr));

						/* we MUST do this now to flush data out to the buffer */
						xmlFreeTextWriter(writer);
						writer = NULL;

						result = pstrdup((const char *) xmlBufferContent(buf));
					}
					PG_CATCH();
					{
						if (writer)
							xmlFreeTextWriter(writer);
						if (buf)
							xmlBufferFree(buf);

						pg_xml_done(xmlerrcxt, true);

						PG_RE_THROW();
					}
					PG_END_TRY();

					xmlBufferFree(buf);

					pg_xml_done(xmlerrcxt, false);

					return result;
				}
#endif							/* USE_LIBXML */

		}

		/*
		 * otherwise, just use the type's native text representation
		 */
		getTypeOutputInfo(type, &typeOut, &isvarlena);
		str = OidOutputFunctionCall(typeOut, value);

		/* ... exactly as-is for XML, and when escaping is not wanted */
		if (type == XMLOID || !xml_escape_strings)
			return str;

		/* otherwise, translate special characters as needed */
		return escape_xml(str);
	}
}


/*
 * Escape characters in text that have special meanings in XML.
 *
 * Returns a palloc'd string.
 *
 * NB: this is intentionally not dependent on libxml.
 */
char *
escape_xml(const char *str)
{
	StringInfoData buf;
	const char *p;

	initStringInfo(&buf);
	for (p = str; *p; p++)
	{
		switch (*p)
		{
			case '&':
				appendStringInfoString(&buf, "&amp;");
				break;
			case '<':
				appendStringInfoString(&buf, "&lt;");
				break;
			case '>':
				appendStringInfoString(&buf, "&gt;");
				break;
			case '\r':
				appendStringInfoString(&buf, "&#x0d;");
				break;
			default:
				appendStringInfoCharMacro(&buf, *p);
				break;
		}
	}
	return buf.data;
}


static char *
_SPI_strdup(const char *s)
{
	size_t		len = strlen(s) + 1;
	char	   *ret = SPI_palloc(len);

	memcpy(ret, s, len);
	return ret;
}


/*
 * SQL to XML mapping functions
 *
 * What follows below was at one point intentionally organized so that
 * you can read along in the SQL/XML standard. The functions are
 * mostly split up the way the clauses lay out in the standards
 * document, and the identifiers are also aligned with the standard
 * text.  Unfortunately, SQL/XML:2006 reordered the clauses
 * differently than SQL/XML:2003, so the order below doesn't make much
 * sense anymore.
 *
 * There are many things going on there:
 *
 * There are two kinds of mappings: Mapping SQL data (table contents)
 * to XML documents, and mapping SQL structure (the "schema") to XML
 * Schema.  And there are functions that do both at the same time.
 *
 * Then you can map a database, a schema, or a table, each in both
 * ways.  This breaks down recursively: Mapping a database invokes
 * mapping schemas, which invokes mapping tables, which invokes
 * mapping rows, which invokes mapping columns, although you can't
 * call the last two from the outside.  Because of this, there are a
 * number of xyz_internal() functions which are to be called both from
 * the function manager wrapper and from some upper layer in a
 * recursive call.
 *
 * See the documentation about what the common function arguments
 * nulls, tableforest, and targetns mean.
 *
 * Some style guidelines for XML output: Use double quotes for quoting
 * XML attributes.  Indent XML elements by two spaces, but remember
 * that a lot of code is called recursively at different levels, so
 * it's better not to indent rather than create output that indents
 * and outdents weirdly.  Add newlines to make the output look nice.
 */


/*
 * Visibility of objects for XML mappings; see SQL/XML:2008 section
 * 4.10.8.
 */

/*
 * Given a query, which must return type oid as first column, produce
 * a list of Oids with the query results.
 */
static List *
query_to_oid_list(const char *query)
{
	uint64		i;
	List	   *list = NIL;
	int			spi_result;

	spi_result = SPI_execute(query, true, 0);
	if (spi_result != SPI_OK_SELECT)
		elog(ERROR, "SPI_execute returned %s for %s",
			 SPI_result_code_string(spi_result), query);

	for (i = 0; i < SPI_processed; i++)
	{
		Datum		oid;
		bool		isnull;

		oid = SPI_getbinval(SPI_tuptable->vals[i],
							SPI_tuptable->tupdesc,
							1,
							&isnull);
		if (!isnull)
			list = lappend_oid(list, DatumGetObjectId(oid));
	}

	return list;
}


static List *
schema_get_xml_visible_tables(Oid nspid)
{
	StringInfoData query;

	initStringInfo(&query);
	appendStringInfo(&query, "SELECT oid FROM pg_catalog.pg_class"
					 " WHERE relnamespace = %u AND relkind IN ("
					 CppAsString2(RELKIND_RELATION) ","
					 CppAsString2(RELKIND_MATVIEW) ","
					 CppAsString2(RELKIND_VIEW) ")"
					 " AND pg_catalog.has_table_privilege (oid, 'SELECT')"
					 " ORDER BY relname;", nspid);

	return query_to_oid_list(query.data);
}


/*
 * Including the system schemas is probably not useful for a database
 * mapping.
 */
#define XML_VISIBLE_SCHEMAS_EXCLUDE "(nspname ~ '^pg_' OR nspname = 'information_schema')"

#define XML_VISIBLE_SCHEMAS "SELECT oid FROM pg_catalog.pg_namespace WHERE pg_catalog.has_schema_privilege (oid, 'USAGE') AND NOT " XML_VISIBLE_SCHEMAS_EXCLUDE


static List *
database_get_xml_visible_schemas(void)
{
	return query_to_oid_list(XML_VISIBLE_SCHEMAS " ORDER BY nspname;");
}


static List *
database_get_xml_visible_tables(void)
{
	/* At the moment there is no order required here. */
	return query_to_oid_list("SELECT oid FROM pg_catalog.pg_class"
							 " WHERE relkind IN ("
							 CppAsString2(RELKIND_RELATION) ","
							 CppAsString2(RELKIND_MATVIEW) ","
							 CppAsString2(RELKIND_VIEW) ")"
							 " AND pg_catalog.has_table_privilege(pg_class.oid, 'SELECT')"
							 " AND relnamespace IN (" XML_VISIBLE_SCHEMAS ");");
}


/*
 * Map SQL table to XML and/or XML Schema document; see SQL/XML:2008
 * section 9.11.
 */

static StringInfo
table_to_xml_internal(Oid relid,
					  const char *xmlschema, bool nulls, bool tableforest,
					  const char *targetns, bool top_level)
{
	StringInfoData query;

	initStringInfo(&query);
	appendStringInfo(&query, "SELECT * FROM %s",
					 DatumGetCString(DirectFunctionCall1(regclassout,
														 ObjectIdGetDatum(relid))));
	return query_to_xml_internal(query.data, get_rel_name(relid),
								 xmlschema, nulls, tableforest,
								 targetns, top_level);
}


Datum
table_to_xml(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	PG_RETURN_XML_P(stringinfo_to_xmltype(table_to_xml_internal(relid, NULL,
																nulls, tableforest,
																targetns, true)));
}


Datum
query_to_xml(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	PG_RETURN_XML_P(stringinfo_to_xmltype(query_to_xml_internal(query, NULL,
																NULL, nulls, tableforest,
																targetns, true)));
}


Datum
cursor_to_xml(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int32		count = PG_GETARG_INT32(1);
	bool		nulls = PG_GETARG_BOOL(2);
	bool		tableforest = PG_GETARG_BOOL(3);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(4));

	StringInfoData result;
	Portal		portal;
	uint64		i;

	initStringInfo(&result);

	if (!tableforest)
	{
		xmldata_root_element_start(&result, "table", NULL, targetns, true);
		appendStringInfoChar(&result, '\n');
	}

	SPI_connect();
	portal = SPI_cursor_find(name);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", name)));

	SPI_cursor_fetch(portal, true, count);
	for (i = 0; i < SPI_processed; i++)
		SPI_sql_row_to_xmlelement(i, &result, NULL, nulls,
								  tableforest, targetns, true);

	SPI_finish();

	if (!tableforest)
		xmldata_root_element_end(&result, "table");

	PG_RETURN_XML_P(stringinfo_to_xmltype(&result));
}


/*
 * Write the start tag of the root element of a data mapping.
 *
 * top_level means that this is the very top level of the eventual
 * output.  For example, when the user calls table_to_xml, then a call
 * with a table name to this function is the top level.  When the user
 * calls database_to_xml, then a call with a schema name to this
 * function is not the top level.  If top_level is false, then the XML
 * namespace declarations are omitted, because they supposedly already
 * appeared earlier in the output.  Repeating them is not wrong, but
 * it looks ugly.
 */
static void
xmldata_root_element_start(StringInfo result, const char *eltname,
						   const char *xmlschema, const char *targetns,
						   bool top_level)
{
	/* This isn't really wrong but currently makes no sense. */
	Assert(top_level || !xmlschema);

	appendStringInfo(result, "<%s", eltname);
	if (top_level)
	{
		appendStringInfoString(result, " xmlns:xsi=\"" NAMESPACE_XSI "\"");
		if (strlen(targetns) > 0)
			appendStringInfo(result, " xmlns=\"%s\"", targetns);
	}
	if (xmlschema)
	{
		/* FIXME: better targets */
		if (strlen(targetns) > 0)
			appendStringInfo(result, " xsi:schemaLocation=\"%s #\"", targetns);
		else
			appendStringInfoString(result, " xsi:noNamespaceSchemaLocation=\"#\"");
	}
	appendStringInfoString(result, ">\n");
}


static void
xmldata_root_element_end(StringInfo result, const char *eltname)
{
	appendStringInfo(result, "</%s>\n", eltname);
}


static StringInfo
query_to_xml_internal(const char *query, char *tablename,
					  const char *xmlschema, bool nulls, bool tableforest,
					  const char *targetns, bool top_level)
{
	StringInfo	result;
	char	   *xmltn;
	uint64		i;

	if (tablename)
		xmltn = map_sql_identifier_to_xml_name(tablename, true, false);
	else
		xmltn = "table";

	result = makeStringInfo();

	SPI_connect();
	if (SPI_execute(query, true, 0) != SPI_OK_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("invalid query")));

	if (!tableforest)
	{
		xmldata_root_element_start(result, xmltn, xmlschema,
								   targetns, top_level);
		appendStringInfoChar(result, '\n');
	}

	if (xmlschema)
		appendStringInfo(result, "%s\n\n", xmlschema);

	for (i = 0; i < SPI_processed; i++)
		SPI_sql_row_to_xmlelement(i, result, tablename, nulls,
								  tableforest, targetns, top_level);

	if (!tableforest)
		xmldata_root_element_end(result, xmltn);

	SPI_finish();

	return result;
}


Datum
table_to_xmlschema(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	const char *result;
	Relation	rel;

	rel = table_open(relid, AccessShareLock);
	result = map_sql_table_to_xmlschema(rel->rd_att, relid, nulls,
										tableforest, targetns);
	table_close(rel, NoLock);

	PG_RETURN_XML_P(cstring_to_xmltype(result));
}


Datum
query_to_xmlschema(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	const char *result;
	SPIPlanPtr	plan;
	Portal		portal;

	SPI_connect();

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		elog(ERROR, "SPI_prepare(\"%s\") failed", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

	result = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc,
													InvalidOid, nulls,
													tableforest, targetns));
	SPI_cursor_close(portal);
	SPI_finish();

	PG_RETURN_XML_P(cstring_to_xmltype(result));
}


Datum
cursor_to_xmlschema(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	const char *xmlschema;
	Portal		portal;

	SPI_connect();
	portal = SPI_cursor_find(name);
	if (portal == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_CURSOR),
				 errmsg("cursor \"%s\" does not exist", name)));

	xmlschema = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc,
													   InvalidOid, nulls,
													   tableforest, targetns));
	SPI_finish();

	PG_RETURN_XML_P(cstring_to_xmltype(xmlschema));
}


Datum
table_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	Relation	rel;
	const char *xmlschema;

	rel = table_open(relid, AccessShareLock);
	xmlschema = map_sql_table_to_xmlschema(rel->rd_att, relid, nulls,
										   tableforest, targetns);
	table_close(rel, NoLock);

	PG_RETURN_XML_P(stringinfo_to_xmltype(table_to_xml_internal(relid,
																xmlschema, nulls, tableforest,
																targetns, true)));
}


Datum
query_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	const char *xmlschema;
	SPIPlanPtr	plan;
	Portal		portal;

	SPI_connect();

	if ((plan = SPI_prepare(query, 0, NULL)) == NULL)
		elog(ERROR, "SPI_prepare(\"%s\") failed", query);

	if ((portal = SPI_cursor_open(NULL, plan, NULL, NULL, true)) == NULL)
		elog(ERROR, "SPI_cursor_open(\"%s\") failed", query);

	xmlschema = _SPI_strdup(map_sql_table_to_xmlschema(portal->tupDesc,
													   InvalidOid, nulls, tableforest, targetns));
	SPI_cursor_close(portal);
	SPI_finish();

	PG_RETURN_XML_P(stringinfo_to_xmltype(query_to_xml_internal(query, NULL,
																xmlschema, nulls, tableforest,
																targetns, true)));
}


/*
 * Map SQL schema to XML and/or XML Schema document; see SQL/XML:2008
 * sections 9.13, 9.14.
 */

static StringInfo
schema_to_xml_internal(Oid nspid, const char *xmlschema, bool nulls,
					   bool tableforest, const char *targetns, bool top_level)
{
	StringInfo	result;
	char	   *xmlsn;
	List	   *relid_list;
	ListCell   *cell;

	xmlsn = map_sql_identifier_to_xml_name(get_namespace_name(nspid),
										   true, false);
	result = makeStringInfo();

	xmldata_root_element_start(result, xmlsn, xmlschema, targetns, top_level);
	appendStringInfoChar(result, '\n');

	if (xmlschema)
		appendStringInfo(result, "%s\n\n", xmlschema);

	SPI_connect();

	relid_list = schema_get_xml_visible_tables(nspid);

	foreach(cell, relid_list)
	{
		Oid			relid = lfirst_oid(cell);
		StringInfo	subres;

		subres = table_to_xml_internal(relid, NULL, nulls, tableforest,
									   targetns, false);

		appendBinaryStringInfo(result, subres->data, subres->len);
		appendStringInfoChar(result, '\n');
	}

	SPI_finish();

	xmldata_root_element_end(result, xmlsn);

	return result;
}


Datum
schema_to_xml(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	char	   *schemaname;
	Oid			nspid;

	schemaname = NameStr(*name);
	nspid = LookupExplicitNamespace(schemaname, false);

	PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xml_internal(nspid, NULL,
																 nulls, tableforest, targetns, true)));
}


/*
 * Write the start element of the root element of an XML Schema mapping.
 */
static void
xsd_schema_element_start(StringInfo result, const char *targetns)
{
	appendStringInfoString(result,
						   "<xsd:schema\n"
						   "    xmlns:xsd=\"" NAMESPACE_XSD "\"");
	if (strlen(targetns) > 0)
		appendStringInfo(result,
						 "\n"
						 "    targetNamespace=\"%s\"\n"
						 "    elementFormDefault=\"qualified\"",
						 targetns);
	appendStringInfoString(result,
						   ">\n\n");
}


static void
xsd_schema_element_end(StringInfo result)
{
	appendStringInfoString(result, "</xsd:schema>");
}


static StringInfo
schema_to_xmlschema_internal(const char *schemaname, bool nulls,
							 bool tableforest, const char *targetns)
{
	Oid			nspid;
	List	   *relid_list;
	List	   *tupdesc_list;
	ListCell   *cell;
	StringInfo	result;

	result = makeStringInfo();

	nspid = LookupExplicitNamespace(schemaname, false);

	xsd_schema_element_start(result, targetns);

	SPI_connect();

	relid_list = schema_get_xml_visible_tables(nspid);

	tupdesc_list = NIL;
	foreach(cell, relid_list)
	{
		Relation	rel;

		rel = table_open(lfirst_oid(cell), AccessShareLock);
		tupdesc_list = lappend(tupdesc_list, CreateTupleDescCopy(rel->rd_att));
		table_close(rel, NoLock);
	}

	appendStringInfoString(result,
						   map_sql_typecoll_to_xmlschema_types(tupdesc_list));

	appendStringInfoString(result,
						   map_sql_schema_to_xmlschema_types(nspid, relid_list,
															 nulls, tableforest, targetns));

	xsd_schema_element_end(result);

	SPI_finish();

	return result;
}


Datum
schema_to_xmlschema(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));

	PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xmlschema_internal(NameStr(*name),
																	   nulls, tableforest, targetns)));
}


Datum
schema_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
	Name		name = PG_GETARG_NAME(0);
	bool		nulls = PG_GETARG_BOOL(1);
	bool		tableforest = PG_GETARG_BOOL(2);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(3));
	char	   *schemaname;
	Oid			nspid;
	StringInfo	xmlschema;

	schemaname = NameStr(*name);
	nspid = LookupExplicitNamespace(schemaname, false);

	xmlschema = schema_to_xmlschema_internal(schemaname, nulls,
											 tableforest, targetns);

	PG_RETURN_XML_P(stringinfo_to_xmltype(schema_to_xml_internal(nspid,
																 xmlschema->data, nulls,
																 tableforest, targetns, true)));
}


/*
 * Map SQL database to XML and/or XML Schema document; see SQL/XML:2008
 * sections 9.16, 9.17.
 */

static StringInfo
database_to_xml_internal(const char *xmlschema, bool nulls,
						 bool tableforest, const char *targetns)
{
	StringInfo	result;
	List	   *nspid_list;
	ListCell   *cell;
	char	   *xmlcn;

	xmlcn = map_sql_identifier_to_xml_name(get_database_name(MyDatabaseId),
										   true, false);
	result = makeStringInfo();

	xmldata_root_element_start(result, xmlcn, xmlschema, targetns, true);
	appendStringInfoChar(result, '\n');

	if (xmlschema)
		appendStringInfo(result, "%s\n\n", xmlschema);

	SPI_connect();

	nspid_list = database_get_xml_visible_schemas();

	foreach(cell, nspid_list)
	{
		Oid			nspid = lfirst_oid(cell);
		StringInfo	subres;

		subres = schema_to_xml_internal(nspid, NULL, nulls,
										tableforest, targetns, false);

		appendBinaryStringInfo(result, subres->data, subres->len);
		appendStringInfoChar(result, '\n');
	}

	SPI_finish();

	xmldata_root_element_end(result, xmlcn);

	return result;
}


Datum
database_to_xml(PG_FUNCTION_ARGS)
{
	bool		nulls = PG_GETARG_BOOL(0);
	bool		tableforest = PG_GETARG_BOOL(1);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));

	PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xml_internal(NULL, nulls,
																   tableforest, targetns)));
}


static StringInfo
database_to_xmlschema_internal(bool nulls, bool tableforest,
							   const char *targetns)
{
	List	   *relid_list;
	List	   *nspid_list;
	List	   *tupdesc_list;
	ListCell   *cell;
	StringInfo	result;

	result = makeStringInfo();

	xsd_schema_element_start(result, targetns);

	SPI_connect();

	relid_list = database_get_xml_visible_tables();
	nspid_list = database_get_xml_visible_schemas();

	tupdesc_list = NIL;
	foreach(cell, relid_list)
	{
		Relation	rel;

		rel = table_open(lfirst_oid(cell), AccessShareLock);
		tupdesc_list = lappend(tupdesc_list, CreateTupleDescCopy(rel->rd_att));
		table_close(rel, NoLock);
	}

	appendStringInfoString(result,
						   map_sql_typecoll_to_xmlschema_types(tupdesc_list));

	appendStringInfoString(result,
						   map_sql_catalog_to_xmlschema_types(nspid_list, nulls, tableforest, targetns));

	xsd_schema_element_end(result);

	SPI_finish();

	return result;
}


Datum
database_to_xmlschema(PG_FUNCTION_ARGS)
{
	bool		nulls = PG_GETARG_BOOL(0);
	bool		tableforest = PG_GETARG_BOOL(1);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));

	PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xmlschema_internal(nulls,
																		 tableforest, targetns)));
}


Datum
database_to_xml_and_xmlschema(PG_FUNCTION_ARGS)
{
	bool		nulls = PG_GETARG_BOOL(0);
	bool		tableforest = PG_GETARG_BOOL(1);
	const char *targetns = text_to_cstring(PG_GETARG_TEXT_PP(2));
	StringInfo	xmlschema;

	xmlschema = database_to_xmlschema_internal(nulls, tableforest, targetns);

	PG_RETURN_XML_P(stringinfo_to_xmltype(database_to_xml_internal(xmlschema->data,
																   nulls, tableforest, targetns)));
}


/*
 * Map a multi-part SQL name to an XML name; see SQL/XML:2008 section
 * 9.2.
 */
static char *
map_multipart_sql_identifier_to_xml_name(const char *a, const char *b, const char *c, const char *d)
{
	StringInfoData result;

	initStringInfo(&result);

	if (a)
		appendStringInfoString(&result,
							   map_sql_identifier_to_xml_name(a, true, true));
	if (b)
		appendStringInfo(&result, ".%s",
						 map_sql_identifier_to_xml_name(b, true, true));
	if (c)
		appendStringInfo(&result, ".%s",
						 map_sql_identifier_to_xml_name(c, true, true));
	if (d)
		appendStringInfo(&result, ".%s",
						 map_sql_identifier_to_xml_name(d, true, true));

	return result.data;
}


/*
 * Map an SQL table to an XML Schema document; see SQL/XML:2008
 * section 9.11.
 *
 * Map an SQL table to XML Schema data types; see SQL/XML:2008 section
 * 9.9.
 */
static const char *
map_sql_table_to_xmlschema(TupleDesc tupdesc, Oid relid, bool nulls,
						   bool tableforest, const char *targetns)
{
	int			i;
	char	   *xmltn;
	char	   *tabletypename;
	char	   *rowtypename;
	StringInfoData result;

	initStringInfo(&result);

	if (OidIsValid(relid))
	{
		HeapTuple	tuple;
		Form_pg_class reltuple;

		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", relid);
		reltuple = (Form_pg_class) GETSTRUCT(tuple);

		xmltn = map_sql_identifier_to_xml_name(NameStr(reltuple->relname),
											   true, false);

		tabletypename = map_multipart_sql_identifier_to_xml_name("TableType",
																 get_database_name(MyDatabaseId),
																 get_namespace_name(reltuple->relnamespace),
																 NameStr(reltuple->relname));

		rowtypename = map_multipart_sql_identifier_to_xml_name("RowType",
															   get_database_name(MyDatabaseId),
															   get_namespace_name(reltuple->relnamespace),
															   NameStr(reltuple->relname));

		ReleaseSysCache(tuple);
	}
	else
	{
		if (tableforest)
			xmltn = "row";
		else
			xmltn = "table";

		tabletypename = "TableType";
		rowtypename = "RowType";
	}

	xsd_schema_element_start(&result, targetns);

	appendStringInfoString(&result,
						   map_sql_typecoll_to_xmlschema_types(list_make1(tupdesc)));

	appendStringInfo(&result,
					 "<xsd:complexType name=\"%s\">\n"
					 "  <xsd:sequence>\n",
					 rowtypename);

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);

		if (att->attisdropped)
			continue;
		appendStringInfo(&result,
						 "    <xsd:element name=\"%s\" type=\"%s\"%s></xsd:element>\n",
						 map_sql_identifier_to_xml_name(NameStr(att->attname),
														true, false),
						 map_sql_type_to_xml_name(att->atttypid, -1),
						 nulls ? " nillable=\"true\"" : " minOccurs=\"0\"");
	}

	appendStringInfoString(&result,
						   "  </xsd:sequence>\n"
						   "</xsd:complexType>\n\n");

	if (!tableforest)
	{
		appendStringInfo(&result,
						 "<xsd:complexType name=\"%s\">\n"
						 "  <xsd:sequence>\n"
						 "    <xsd:element name=\"row\" type=\"%s\" minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n"
						 "  </xsd:sequence>\n"
						 "</xsd:complexType>\n\n",
						 tabletypename, rowtypename);

		appendStringInfo(&result,
						 "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
						 xmltn, tabletypename);
	}
	else
		appendStringInfo(&result,
						 "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
						 xmltn, rowtypename);

	xsd_schema_element_end(&result);

	return result.data;
}


/*
 * Map an SQL schema to XML Schema data types; see SQL/XML:2008
 * section 9.12.
 */
static const char *
map_sql_schema_to_xmlschema_types(Oid nspid, List *relid_list, bool nulls,
								  bool tableforest, const char *targetns)
{
	char	   *dbname;
	char	   *nspname;
	char	   *xmlsn;
	char	   *schematypename;
	StringInfoData result;
	ListCell   *cell;

	dbname = get_database_name(MyDatabaseId);
	nspname = get_namespace_name(nspid);

	initStringInfo(&result);

	xmlsn = map_sql_identifier_to_xml_name(nspname, true, false);

	schematypename = map_multipart_sql_identifier_to_xml_name("SchemaType",
															  dbname,
															  nspname,
															  NULL);

	appendStringInfo(&result,
					 "<xsd:complexType name=\"%s\">\n", schematypename);
	if (!tableforest)
		appendStringInfoString(&result,
							   "  <xsd:all>\n");
	else
		appendStringInfoString(&result,
							   "  <xsd:sequence>\n");

	foreach(cell, relid_list)
	{
		Oid			relid = lfirst_oid(cell);
		char	   *relname = get_rel_name(relid);
		char	   *xmltn = map_sql_identifier_to_xml_name(relname, true, false);
		char	   *tabletypename = map_multipart_sql_identifier_to_xml_name(tableforest ? "RowType" : "TableType",
																			 dbname,
																			 nspname,
																			 relname);

		if (!tableforest)
			appendStringInfo(&result,
							 "    <xsd:element name=\"%s\" type=\"%s\"/>\n",
							 xmltn, tabletypename);
		else
			appendStringInfo(&result,
							 "    <xsd:element name=\"%s\" type=\"%s\" minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n",
							 xmltn, tabletypename);
	}

	if (!tableforest)
		appendStringInfoString(&result,
							   "  </xsd:all>\n");
	else
		appendStringInfoString(&result,
							   "  </xsd:sequence>\n");
	appendStringInfoString(&result,
						   "</xsd:complexType>\n\n");

	appendStringInfo(&result,
					 "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
					 xmlsn, schematypename);

	return result.data;
}


/*
 * Map an SQL catalog to XML Schema data types; see SQL/XML:2008
 * section 9.15.
 */
static const char *
map_sql_catalog_to_xmlschema_types(List *nspid_list, bool nulls,
								   bool tableforest, const char *targetns)
{
	char	   *dbname;
	char	   *xmlcn;
	char	   *catalogtypename;
	StringInfoData result;
	ListCell   *cell;

	dbname = get_database_name(MyDatabaseId);

	initStringInfo(&result);

	xmlcn = map_sql_identifier_to_xml_name(dbname, true, false);

	catalogtypename = map_multipart_sql_identifier_to_xml_name("CatalogType",
															   dbname,
															   NULL,
															   NULL);

	appendStringInfo(&result,
					 "<xsd:complexType name=\"%s\">\n", catalogtypename);
	appendStringInfoString(&result,
						   "  <xsd:all>\n");

	foreach(cell, nspid_list)
	{
		Oid			nspid = lfirst_oid(cell);
		char	   *nspname = get_namespace_name(nspid);
		char	   *xmlsn = map_sql_identifier_to_xml_name(nspname, true, false);
		char	   *schematypename = map_multipart_sql_identifier_to_xml_name("SchemaType",
																			  dbname,
																			  nspname,
																			  NULL);

		appendStringInfo(&result,
						 "    <xsd:element name=\"%s\" type=\"%s\"/>\n",
						 xmlsn, schematypename);
	}

	appendStringInfoString(&result,
						   "  </xsd:all>\n");
	appendStringInfoString(&result,
						   "</xsd:complexType>\n\n");

	appendStringInfo(&result,
					 "<xsd:element name=\"%s\" type=\"%s\"/>\n\n",
					 xmlcn, catalogtypename);

	return result.data;
}


/*
 * Map an SQL data type to an XML name; see SQL/XML:2008 section 9.4.
 */
static const char *
map_sql_type_to_xml_name(Oid typeoid, int typmod)
{
	StringInfoData result;

	initStringInfo(&result);

	switch (typeoid)
	{
		case BPCHAROID:
			if (typmod == -1)
				appendStringInfoString(&result, "CHAR");
			else
				appendStringInfo(&result, "CHAR_%d", typmod - VARHDRSZ);
			break;
		case VARCHAROID:
			if (typmod == -1)
				appendStringInfoString(&result, "VARCHAR");
			else
				appendStringInfo(&result, "VARCHAR_%d", typmod - VARHDRSZ);
			break;
		case NUMERICOID:
			if (typmod == -1)
				appendStringInfoString(&result, "NUMERIC");
			else
				appendStringInfo(&result, "NUMERIC_%d_%d",
								 ((typmod - VARHDRSZ) >> 16) & 0xffff,
								 (typmod - VARHDRSZ) & 0xffff);
			break;
		case INT4OID:
			appendStringInfoString(&result, "INTEGER");
			break;
		case INT2OID:
			appendStringInfoString(&result, "SMALLINT");
			break;
		case INT8OID:
			appendStringInfoString(&result, "BIGINT");
			break;
		case FLOAT4OID:
			appendStringInfoString(&result, "REAL");
			break;
		case FLOAT8OID:
			appendStringInfoString(&result, "DOUBLE");
			break;
		case BOOLOID:
			appendStringInfoString(&result, "BOOLEAN");
			break;
		case TIMEOID:
			if (typmod == -1)
				appendStringInfoString(&result, "TIME");
			else
				appendStringInfo(&result, "TIME_%d", typmod);
			break;
		case TIMETZOID:
			if (typmod == -1)
				appendStringInfoString(&result, "TIME_WTZ");
			else
				appendStringInfo(&result, "TIME_WTZ_%d", typmod);
			break;
		case TIMESTAMPOID:
			if (typmod == -1)
				appendStringInfoString(&result, "TIMESTAMP");
			else
				appendStringInfo(&result, "TIMESTAMP_%d", typmod);
			break;
		case TIMESTAMPTZOID:
			if (typmod == -1)
				appendStringInfoString(&result, "TIMESTAMP_WTZ");
			else
				appendStringInfo(&result, "TIMESTAMP_WTZ_%d", typmod);
			break;
		case DATEOID:
			appendStringInfoString(&result, "DATE");
			break;
		case XMLOID:
			appendStringInfoString(&result, "XML");
			break;
		default:
			{
				HeapTuple	tuple;
				Form_pg_type typtuple;

				tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeoid));
				if (!HeapTupleIsValid(tuple))
					elog(ERROR, "cache lookup failed for type %u", typeoid);
				typtuple = (Form_pg_type) GETSTRUCT(tuple);

				appendStringInfoString(&result,
									   map_multipart_sql_identifier_to_xml_name((typtuple->typtype == TYPTYPE_DOMAIN) ? "Domain" : "UDT",
																				get_database_name(MyDatabaseId),
																				get_namespace_name(typtuple->typnamespace),
																				NameStr(typtuple->typname)));

				ReleaseSysCache(tuple);
			}
	}

	return result.data;
}


/*
 * Map a collection of SQL data types to XML Schema data types; see
 * SQL/XML:2008 section 9.7.
 */
static const char *
map_sql_typecoll_to_xmlschema_types(List *tupdesc_list)
{
	List	   *uniquetypes = NIL;
	int			i;
	StringInfoData result;
	ListCell   *cell0;

	/* extract all column types used in the set of TupleDescs */
	foreach(cell0, tupdesc_list)
	{
		TupleDesc	tupdesc = (TupleDesc) lfirst(cell0);

		for (i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);

			if (att->attisdropped)
				continue;
			uniquetypes = list_append_unique_oid(uniquetypes, att->atttypid);
		}
	}

	/* add base types of domains */
	foreach(cell0, uniquetypes)
	{
		Oid			typid = lfirst_oid(cell0);
		Oid			basetypid = getBaseType(typid);

		if (basetypid != typid)
			uniquetypes = list_append_unique_oid(uniquetypes, basetypid);
	}

	/* Convert to textual form */
	initStringInfo(&result);

	foreach(cell0, uniquetypes)
	{
		appendStringInfo(&result, "%s\n",
						 map_sql_type_to_xmlschema_type(lfirst_oid(cell0),
														-1));
	}

	return result.data;
}


/*
 * Map an SQL data type to a named XML Schema data type; see
 * SQL/XML:2008 sections 9.5 and 9.6.
 *
 * (The distinction between 9.5 and 9.6 is basically that 9.6 adds
 * a name attribute, which this function does.  The name-less version
 * 9.5 doesn't appear to be required anywhere.)
 */
static const char *
map_sql_type_to_xmlschema_type(Oid typeoid, int typmod)
{
	StringInfoData result;
	const char *typename = map_sql_type_to_xml_name(typeoid, typmod);

	initStringInfo(&result);

	if (typeoid == XMLOID)
	{
		appendStringInfoString(&result,
							   "<xsd:complexType mixed=\"true\">\n"
							   "  <xsd:sequence>\n"
							   "    <xsd:any name=\"element\" minOccurs=\"0\" maxOccurs=\"unbounded\" processContents=\"skip\"/>\n"
							   "  </xsd:sequence>\n"
							   "</xsd:complexType>\n");
	}
	else
	{
		appendStringInfo(&result,
						 "<xsd:simpleType name=\"%s\">\n", typename);

		switch (typeoid)
		{
			case BPCHAROID:
			case VARCHAROID:
			case TEXTOID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:string\">\n");
				if (typmod != -1)
					appendStringInfo(&result,
									 "    <xsd:maxLength value=\"%d\"/>\n",
									 typmod - VARHDRSZ);
				appendStringInfoString(&result, "  </xsd:restriction>\n");
				break;

			case BYTEAOID:
				appendStringInfo(&result,
								 "  <xsd:restriction base=\"xsd:%s\">\n"
								 "  </xsd:restriction>\n",
								 xmlbinary == XMLBINARY_BASE64 ? "base64Binary" : "hexBinary");
				break;

			case NUMERICOID:
				if (typmod != -1)
					appendStringInfo(&result,
									 "  <xsd:restriction base=\"xsd:decimal\">\n"
									 "    <xsd:totalDigits value=\"%d\"/>\n"
									 "    <xsd:fractionDigits value=\"%d\"/>\n"
									 "  </xsd:restriction>\n",
									 ((typmod - VARHDRSZ) >> 16) & 0xffff,
									 (typmod - VARHDRSZ) & 0xffff);
				break;

			case INT2OID:
				appendStringInfo(&result,
								 "  <xsd:restriction base=\"xsd:short\">\n"
								 "    <xsd:maxInclusive value=\"%d\"/>\n"
								 "    <xsd:minInclusive value=\"%d\"/>\n"
								 "  </xsd:restriction>\n",
								 SHRT_MAX, SHRT_MIN);
				break;

			case INT4OID:
				appendStringInfo(&result,
								 "  <xsd:restriction base=\"xsd:int\">\n"
								 "    <xsd:maxInclusive value=\"%d\"/>\n"
								 "    <xsd:minInclusive value=\"%d\"/>\n"
								 "  </xsd:restriction>\n",
								 INT_MAX, INT_MIN);
				break;

			case INT8OID:
				appendStringInfo(&result,
								 "  <xsd:restriction base=\"xsd:long\">\n"
								 "    <xsd:maxInclusive value=\"" INT64_FORMAT "\"/>\n"
								 "    <xsd:minInclusive value=\"" INT64_FORMAT "\"/>\n"
								 "  </xsd:restriction>\n",
								 (((uint64) 1) << (sizeof(int64) * 8 - 1)) - 1,
								 (((uint64) 1) << (sizeof(int64) * 8 - 1)));
				break;

			case FLOAT4OID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:float\"></xsd:restriction>\n");
				break;

			case FLOAT8OID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:double\"></xsd:restriction>\n");
				break;

			case BOOLOID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:boolean\"></xsd:restriction>\n");
				break;

			case TIMEOID:
			case TIMETZOID:
				{
					const char *tz = (typeoid == TIMETZOID ? "(+|-)\\p{Nd}{2}:\\p{Nd}{2}" : "");

					if (typmod == -1)
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:time\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}(.\\p{Nd}+)?%s\"/>\n"
										 "  </xsd:restriction>\n", tz);
					else if (typmod == 0)
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:time\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}%s\"/>\n"
										 "  </xsd:restriction>\n", tz);
					else
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:time\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}.\\p{Nd}{%d}%s\"/>\n"
										 "  </xsd:restriction>\n", typmod - VARHDRSZ, tz);
					break;
				}

			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
				{
					const char *tz = (typeoid == TIMESTAMPTZOID ? "(+|-)\\p{Nd}{2}:\\p{Nd}{2}" : "");

					if (typmod == -1)
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:dateTime\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}(.\\p{Nd}+)?%s\"/>\n"
										 "  </xsd:restriction>\n", tz);
					else if (typmod == 0)
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:dateTime\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}%s\"/>\n"
										 "  </xsd:restriction>\n", tz);
					else
						appendStringInfo(&result,
										 "  <xsd:restriction base=\"xsd:dateTime\">\n"
										 "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}T\\p{Nd}{2}:\\p{Nd}{2}:\\p{Nd}{2}.\\p{Nd}{%d}%s\"/>\n"
										 "  </xsd:restriction>\n", typmod - VARHDRSZ, tz);
					break;
				}

			case DATEOID:
				appendStringInfoString(&result,
									   "  <xsd:restriction base=\"xsd:date\">\n"
									   "    <xsd:pattern value=\"\\p{Nd}{4}-\\p{Nd}{2}-\\p{Nd}{2}\"/>\n"
									   "  </xsd:restriction>\n");
				break;

			default:
				if (get_typtype(typeoid) == TYPTYPE_DOMAIN)
				{
					Oid			base_typeoid;
					int32		base_typmod = -1;

					base_typeoid = getBaseTypeAndTypmod(typeoid, &base_typmod);

					appendStringInfo(&result,
									 "  <xsd:restriction base=\"%s\"/>\n",
									 map_sql_type_to_xml_name(base_typeoid, base_typmod));
				}
				break;
		}
		appendStringInfoString(&result, "</xsd:simpleType>\n");
	}

	return result.data;
}


/*
 * Map an SQL row to an XML element, taking the row from the active
 * SPI cursor.  See also SQL/XML:2008 section 9.10.
 */
static void
SPI_sql_row_to_xmlelement(uint64 rownum, StringInfo result, char *tablename,
						  bool nulls, bool tableforest,
						  const char *targetns, bool top_level)
{
	int			i;
	char	   *xmltn;

	if (tablename)
		xmltn = map_sql_identifier_to_xml_name(tablename, true, false);
	else
	{
		if (tableforest)
			xmltn = "row";
		else
			xmltn = "table";
	}

	if (tableforest)
		xmldata_root_element_start(result, xmltn, NULL, targetns, top_level);
	else
		appendStringInfoString(result, "<row>\n");

	for (i = 1; i <= SPI_tuptable->tupdesc->natts; i++)
	{
		char	   *colname;
		Datum		colval;
		bool		isnull;

		colname = map_sql_identifier_to_xml_name(SPI_fname(SPI_tuptable->tupdesc, i),
												 true, false);
		colval = SPI_getbinval(SPI_tuptable->vals[rownum],
							   SPI_tuptable->tupdesc,
							   i,
							   &isnull);
		if (isnull)
		{
			if (nulls)
				appendStringInfo(result, "  <%s xsi:nil=\"true\"/>\n", colname);
		}
		else
			appendStringInfo(result, "  <%s>%s</%s>\n",
							 colname,
							 map_sql_value_to_xml_value(colval,
														SPI_gettypeid(SPI_tuptable->tupdesc, i), true),
							 colname);
	}

	if (tableforest)
	{
		xmldata_root_element_end(result, xmltn);
		appendStringInfoChar(result, '\n');
	}
	else
		appendStringInfoString(result, "</row>\n\n");
}


/*
 * XPath related functions
 */

#ifdef USE_LIBXML

/*
 * Convert XML node to text.
 *
 * For attribute and text nodes, return the escaped text.  For anything else,
 * dump the whole subtree.
 */
static text *
xml_xmlnodetoxmltype(xmlNodePtr cur, PgXmlErrorContext *xmlerrcxt)
{
	xmltype    *result = NULL;

	if (cur->type != XML_ATTRIBUTE_NODE && cur->type != XML_TEXT_NODE)
	{
		void		(*volatile nodefree) (xmlNodePtr) = NULL;
		volatile xmlBufferPtr buf = NULL;
		volatile xmlNodePtr cur_copy = NULL;

		PG_TRY();
		{
			int			bytes;

			buf = xmlBufferCreate();
			if (buf == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not allocate xmlBuffer");

			/*
			 * Produce a dump of the node that we can serialize.  xmlNodeDump
			 * does that, but the result of that function won't contain
			 * namespace definitions from ancestor nodes, so we first do a
			 * xmlCopyNode() which duplicates the node along with its required
			 * namespace definitions.
			 *
			 * Some old libxml2 versions such as 2.7.6 produce partially
			 * broken XML_DOCUMENT_NODE nodes (unset content field) when
			 * copying them.  xmlNodeDump of such a node works fine, but
			 * xmlFreeNode crashes; set us up to call xmlFreeDoc instead.
			 */
			cur_copy = xmlCopyNode(cur, 1);
			if (cur_copy == NULL || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not copy node");
			nodefree = (cur_copy->type == XML_DOCUMENT_NODE) ?
				(void (*) (xmlNodePtr)) xmlFreeDoc : xmlFreeNode;

			bytes = xmlNodeDump(buf, NULL, cur_copy, 0, 0);
			if (bytes == -1 || xmlerrcxt->err_occurred)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
							"could not dump node");

			result = xmlBuffer_to_xmltype(buf);
		}
		PG_FINALLY();
		{
			if (nodefree)
				nodefree(cur_copy);
			if (buf)
				xmlBufferFree(buf);
		}
		PG_END_TRY();
	}
	else
	{
		xmlChar    *str;

		str = xmlXPathCastNodeToString(cur);
		PG_TRY();
		{
			/* Here we rely on XML having the same representation as TEXT */
			char	   *escaped = escape_xml((char *) str);

			result = (xmltype *) cstring_to_text(escaped);
			pfree(escaped);
		}
		PG_FINALLY();
		{
			xmlFree(str);
		}
		PG_END_TRY();
	}

	return result;
}

/*
 * Convert an XML XPath object (the result of evaluating an XPath expression)
 * to an array of xml values, which are appended to astate.  The function
 * result value is the number of elements in the array.
 *
 * If "astate" is NULL then we don't generate the array value, but we still
 * return the number of elements it would have had.
 *
 * Nodesets are converted to an array containing the nodes' textual
 * representations.  Primitive values (float, double, string) are converted
 * to a single-element array containing the value's string representation.
 */
static int
xml_xpathobjtoxmlarray(xmlXPathObjectPtr xpathobj,
					   ArrayBuildState *astate,
					   PgXmlErrorContext *xmlerrcxt)
{
	int			result = 0;
	Datum		datum;
	Oid			datumtype;
	char	   *result_str;

	switch (xpathobj->type)
	{
		case XPATH_NODESET:
			if (xpathobj->nodesetval != NULL)
			{
				result = xpathobj->nodesetval->nodeNr;
				if (astate != NULL)
				{
					int			i;

					for (i = 0; i < result; i++)
					{
						datum = PointerGetDatum(xml_xmlnodetoxmltype(xpathobj->nodesetval->nodeTab[i],
																	 xmlerrcxt));
						(void) accumArrayResult(astate, datum, false,
												XMLOID, CurrentMemoryContext);
					}
				}
			}
			return result;

		case XPATH_BOOLEAN:
			if (astate == NULL)
				return 1;
			datum = BoolGetDatum(xpathobj->boolval);
			datumtype = BOOLOID;
			break;

		case XPATH_NUMBER:
			if (astate == NULL)
				return 1;
			datum = Float8GetDatum(xpathobj->floatval);
			datumtype = FLOAT8OID;
			break;

		case XPATH_STRING:
			if (astate == NULL)
				return 1;
			datum = CStringGetDatum((char *) xpathobj->stringval);
			datumtype = CSTRINGOID;
			break;

		default:
			elog(ERROR, "xpath expression result type %d is unsupported",
				 xpathobj->type);
			return 0;			/* keep compiler quiet */
	}

	/* Common code for scalar-value cases */
	result_str = map_sql_value_to_xml_value(datum, datumtype, true);
	datum = PointerGetDatum(cstring_to_xmltype(result_str));
	(void) accumArrayResult(astate, datum, false,
							XMLOID, CurrentMemoryContext);
	return 1;
}


/*
 * Common code for xpath() and xmlexists()
 *
 * Evaluate XPath expression and return number of nodes in res_nitems
 * and array of XML values in astate.  Either of those pointers can be
 * NULL if the corresponding result isn't wanted.
 *
 * It is up to the user to ensure that the XML passed is in fact
 * an XML document - XPath doesn't work easily on fragments without
 * a context node being known.
 */
static void
xpath_internal(text *xpath_expr_text, xmltype *data, ArrayType *namespaces,
			   int *res_nitems, ArrayBuildState *astate)
{
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlParserCtxtPtr ctxt = NULL;
	volatile xmlDocPtr doc = NULL;
	volatile xmlXPathContextPtr xpathctx = NULL;
	volatile xmlXPathCompExprPtr xpathcomp = NULL;
	volatile xmlXPathObjectPtr xpathobj = NULL;
	char	   *datastr;
	int32		len;
	int32		xpath_len;
	xmlChar    *string;
	xmlChar    *xpath_expr;
	size_t		xmldecl_len = 0;
	int			i;
	int			ndim;
	Datum	   *ns_names_uris;
	bool	   *ns_names_uris_nulls;
	int			ns_count;

	/*
	 * Namespace mappings are passed as text[].  If an empty array is passed
	 * (ndim = 0, "0-dimensional"), then there are no namespace mappings.
	 * Else, a 2-dimensional array with length of the second axis being equal
	 * to 2 should be passed, i.e., every subarray contains 2 elements, the
	 * first element defining the name, the second one the URI.  Example:
	 * ARRAY[ARRAY['myns', 'http://example.com'], ARRAY['myns2',
	 * 'http://example2.com']].
	 */
	ndim = namespaces ? ARR_NDIM(namespaces) : 0;
	if (ndim != 0)
	{
		int		   *dims;

		dims = ARR_DIMS(namespaces);

		if (ndim != 2 || dims[1] != 2)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("invalid array for XML namespace mapping"),
					 errdetail("The array must be two-dimensional with length of the second axis equal to 2.")));

		Assert(ARR_ELEMTYPE(namespaces) == TEXTOID);

		deconstruct_array(namespaces, TEXTOID, -1, false, TYPALIGN_INT,
						  &ns_names_uris, &ns_names_uris_nulls,
						  &ns_count);

		Assert((ns_count % 2) == 0);	/* checked above */
		ns_count /= 2;			/* count pairs only */
	}
	else
	{
		ns_names_uris = NULL;
		ns_names_uris_nulls = NULL;
		ns_count = 0;
	}

	datastr = VARDATA(data);
	len = VARSIZE(data) - VARHDRSZ;
	xpath_len = VARSIZE_ANY_EXHDR(xpath_expr_text);
	if (xpath_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("empty XPath expression")));

	string = pg_xmlCharStrndup(datastr, len);
	xpath_expr = pg_xmlCharStrndup(VARDATA_ANY(xpath_expr_text), xpath_len);

	/*
	 * In a UTF8 database, skip any xml declaration, which might assert
	 * another encoding.  Ignore parse_xml_decl() failure, letting
	 * xmlCtxtReadMemory() report parse errors.  Documentation disclaims
	 * xpath() support for non-ASCII data in non-UTF8 databases, so leave
	 * those scenarios bug-compatible with historical behavior.
	 */
	if (GetDatabaseEncoding() == PG_UTF8)
		parse_xml_decl(string, &xmldecl_len, NULL, NULL, NULL);

	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

	PG_TRY();
	{
		xmlInitParser();

		/*
		 * redundant XML parsing (two parsings for the same value during one
		 * command execution are possible)
		 */
		ctxt = xmlNewParserCtxt();
		if (ctxt == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate parser context");
		doc = xmlCtxtReadMemory(ctxt, (char *) string + xmldecl_len,
								len - xmldecl_len, NULL, NULL, 0);
		if (doc == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"could not parse XML document");
		xpathctx = xmlXPathNewContext(doc);
		if (xpathctx == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate XPath context");
		xpathctx->node = (xmlNodePtr) doc;

		/* register namespaces, if any */
		if (ns_count > 0)
		{
			for (i = 0; i < ns_count; i++)
			{
				char	   *ns_name;
				char	   *ns_uri;

				if (ns_names_uris_nulls[i * 2] ||
					ns_names_uris_nulls[i * 2 + 1])
					ereport(ERROR,
							(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							 errmsg("neither namespace name nor URI may be null")));
				ns_name = TextDatumGetCString(ns_names_uris[i * 2]);
				ns_uri = TextDatumGetCString(ns_names_uris[i * 2 + 1]);
				if (xmlXPathRegisterNs(xpathctx,
									   (xmlChar *) ns_name,
									   (xmlChar *) ns_uri) != 0)
					ereport(ERROR,	/* is this an internal error??? */
							(errmsg("could not register XML namespace with name \"%s\" and URI \"%s\"",
									ns_name, ns_uri)));
			}
		}

		xpathcomp = xmlXPathCompile(xpath_expr);
		if (xpathcomp == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"invalid XPath expression");

		/*
		 * Version 2.6.27 introduces a function named
		 * xmlXPathCompiledEvalToBoolean, which would be enough for xmlexists,
		 * but we can derive the existence by whether any nodes are returned,
		 * thereby preventing a library version upgrade and keeping the code
		 * the same.
		 */
		xpathobj = xmlXPathCompiledEval(xpathcomp, xpathctx);
		if (xpathobj == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"could not create XPath object");

		/*
		 * Extract the results as requested.
		 */
		if (res_nitems != NULL)
			*res_nitems = xml_xpathobjtoxmlarray(xpathobj, astate, xmlerrcxt);
		else
			(void) xml_xpathobjtoxmlarray(xpathobj, astate, xmlerrcxt);
	}
	PG_CATCH();
	{
		if (xpathobj)
			xmlXPathFreeObject(xpathobj);
		if (xpathcomp)
			xmlXPathFreeCompExpr(xpathcomp);
		if (xpathctx)
			xmlXPathFreeContext(xpathctx);
		if (doc)
			xmlFreeDoc(doc);
		if (ctxt)
			xmlFreeParserCtxt(ctxt);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xmlXPathFreeObject(xpathobj);
	xmlXPathFreeCompExpr(xpathcomp);
	xmlXPathFreeContext(xpathctx);
	xmlFreeDoc(doc);
	xmlFreeParserCtxt(ctxt);

	pg_xml_done(xmlerrcxt, false);
}
#endif							/* USE_LIBXML */

/*
 * Evaluate XPath expression and return array of XML values.
 *
 * As we have no support of XQuery sequences yet, this function seems
 * to be the most useful one (array of XML functions plays a role of
 * some kind of substitution for XQuery sequences).
 */
Datum
xpath(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *xpath_expr_text = PG_GETARG_TEXT_PP(0);
	xmltype    *data = PG_GETARG_XML_P(1);
	ArrayType  *namespaces = PG_GETARG_ARRAYTYPE_P(2);
	ArrayBuildState *astate;

	astate = initArrayResult(XMLOID, CurrentMemoryContext, true);
	xpath_internal(xpath_expr_text, data, namespaces,
				   NULL, astate);
	PG_RETURN_ARRAYTYPE_P(makeArrayResult(astate, CurrentMemoryContext));
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}

/*
 * Determines if the node specified by the supplied XPath exists
 * in a given XML document, returning a boolean.
 */
Datum
xmlexists(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *xpath_expr_text = PG_GETARG_TEXT_PP(0);
	xmltype    *data = PG_GETARG_XML_P(1);
	int			res_nitems;

	xpath_internal(xpath_expr_text, data, NULL,
				   &res_nitems, NULL);

	PG_RETURN_BOOL(res_nitems > 0);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}

/*
 * Determines if the node specified by the supplied XPath exists
 * in a given XML document, returning a boolean. Differs from
 * xmlexists as it supports namespaces and is not defined in SQL/XML.
 */
Datum
xpath_exists(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *xpath_expr_text = PG_GETARG_TEXT_PP(0);
	xmltype    *data = PG_GETARG_XML_P(1);
	ArrayType  *namespaces = PG_GETARG_ARRAYTYPE_P(2);
	int			res_nitems;

	xpath_internal(xpath_expr_text, data, namespaces,
				   &res_nitems, NULL);

	PG_RETURN_BOOL(res_nitems > 0);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}

/*
 * Functions for checking well-formed-ness
 */

#ifdef USE_LIBXML
static bool
wellformed_xml(text *data, XmlOptionType xmloption_arg)
{
	bool		result;
	volatile xmlDocPtr doc = NULL;

	/* We want to catch any exceptions and return false */
	PG_TRY();
	{
		doc = xml_parse(data, xmloption_arg, true, GetDatabaseEncoding());
		result = true;
	}
	PG_CATCH();
	{
		FlushErrorState();
		result = false;
	}
	PG_END_TRY();

	if (doc)
		xmlFreeDoc(doc);

	return result;
}
#endif

Datum
xml_is_well_formed(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *data = PG_GETARG_TEXT_PP(0);

	PG_RETURN_BOOL(wellformed_xml(data, xmloption));
#else
	NO_XML_SUPPORT();
	return 0;
#endif							/* not USE_LIBXML */
}

Datum
xml_is_well_formed_document(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *data = PG_GETARG_TEXT_PP(0);

	PG_RETURN_BOOL(wellformed_xml(data, XMLOPTION_DOCUMENT));
#else
	NO_XML_SUPPORT();
	return 0;
#endif							/* not USE_LIBXML */
}

Datum
xml_is_well_formed_content(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text	   *data = PG_GETARG_TEXT_PP(0);

	PG_RETURN_BOOL(wellformed_xml(data, XMLOPTION_CONTENT));
#else
	NO_XML_SUPPORT();
	return 0;
#endif							/* not USE_LIBXML */
}

/*
 * support functions for XMLTABLE
 *
 */
#ifdef USE_LIBXML

/*
 * Returns private data from executor state. Ensure validity by check with
 * MAGIC number.
 */
static inline XmlTableBuilderData *
GetXmlTableBuilderPrivateData(TableFuncScanState *state, const char *fname)
{
	XmlTableBuilderData *result;

	if (!IsA(state, TableFuncScanState))
		elog(ERROR, "%s called with invalid TableFuncScanState", fname);
	result = (XmlTableBuilderData *) state->opaque;
	if (result->magic != XMLTABLE_CONTEXT_MAGIC)
		elog(ERROR, "%s called with invalid TableFuncScanState", fname);

	return result;
}
#endif

/*
 * XmlTableInitOpaque
 *		Fill in TableFuncScanState->opaque for XmlTable processor; initialize
 *		the XML parser.
 *
 * Note: Because we call pg_xml_init() here and pg_xml_done() in
 * XmlTableDestroyOpaque, it is critical for robustness that no other
 * executor nodes run until this node is processed to completion.  Caller
 * must execute this to completion (probably filling a tuplestore to exhaust
 * this node in a single pass) instead of using row-per-call mode.
 */
static void
XmlTableInitOpaque(TableFuncScanState *state, int natts)
{
#ifdef USE_LIBXML
	volatile xmlParserCtxtPtr ctxt = NULL;
	XmlTableBuilderData *xtCxt;
	PgXmlErrorContext *xmlerrcxt;

	xtCxt = palloc0(sizeof(XmlTableBuilderData));
	xtCxt->magic = XMLTABLE_CONTEXT_MAGIC;
	xtCxt->natts = natts;
	xtCxt->xpathscomp = palloc0(sizeof(xmlXPathCompExprPtr) * natts);

	xmlerrcxt = pg_xml_init(PG_XML_STRICTNESS_ALL);

	PG_TRY();
	{
		xmlInitParser();

		ctxt = xmlNewParserCtxt();
		if (ctxt == NULL || xmlerrcxt->err_occurred)
			xml_ereport(xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate parser context");
	}
	PG_CATCH();
	{
		if (ctxt != NULL)
			xmlFreeParserCtxt(ctxt);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xtCxt->xmlerrcxt = xmlerrcxt;
	xtCxt->ctxt = ctxt;

	state->opaque = xtCxt;
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableSetDocument
 *		Install the input document
 */
static void
XmlTableSetDocument(TableFuncScanState *state, Datum value)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;
	xmltype    *xmlval = DatumGetXmlP(value);
	char	   *str;
	xmlChar    *xstr;
	int			length;
	volatile xmlDocPtr doc = NULL;
	volatile xmlXPathContextPtr xpathcxt = NULL;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableSetDocument");

	/*
	 * Use out function for casting to string (remove encoding property). See
	 * comment in xml_out.
	 */
	str = xml_out_internal(xmlval, 0);

	length = strlen(str);
	xstr = pg_xmlCharStrndup(str, length);

	PG_TRY();
	{
		doc = xmlCtxtReadMemory(xtCxt->ctxt, (char *) xstr, length, NULL, NULL, 0);
		if (doc == NULL || xtCxt->xmlerrcxt->err_occurred)
			xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"could not parse XML document");
		xpathcxt = xmlXPathNewContext(doc);
		if (xpathcxt == NULL || xtCxt->xmlerrcxt->err_occurred)
			xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_OUT_OF_MEMORY,
						"could not allocate XPath context");
		xpathcxt->node = (xmlNodePtr) doc;
	}
	PG_CATCH();
	{
		if (xpathcxt != NULL)
			xmlXPathFreeContext(xpathcxt);
		if (doc != NULL)
			xmlFreeDoc(doc);

		PG_RE_THROW();
	}
	PG_END_TRY();

	xtCxt->doc = doc;
	xtCxt->xpathcxt = xpathcxt;
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableSetNamespace
 *		Add a namespace declaration
 */
static void
XmlTableSetNamespace(TableFuncScanState *state, const char *name, const char *uri)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;

	if (name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("DEFAULT namespace is not supported")));
	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableSetNamespace");

	if (xmlXPathRegisterNs(xtCxt->xpathcxt,
						   pg_xmlCharStrndup(name, strlen(name)),
						   pg_xmlCharStrndup(uri, strlen(uri))))
		xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_DATA_EXCEPTION,
					"could not set XML namespace");
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableSetRowFilter
 *		Install the row-filter Xpath expression.
 */
static void
XmlTableSetRowFilter(TableFuncScanState *state, const char *path)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;
	xmlChar    *xstr;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableSetRowFilter");

	if (*path == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("row path filter must not be empty string")));

	xstr = pg_xmlCharStrndup(path, strlen(path));

	xtCxt->xpathcomp = xmlXPathCompile(xstr);
	if (xtCxt->xpathcomp == NULL || xtCxt->xmlerrcxt->err_occurred)
		xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_SYNTAX_ERROR,
					"invalid XPath expression");
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableSetColumnFilter
 *		Install the column-filter Xpath expression, for the given column.
 */
static void
XmlTableSetColumnFilter(TableFuncScanState *state, const char *path, int colnum)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;
	xmlChar    *xstr;

	AssertArg(PointerIsValid(path));

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableSetColumnFilter");

	if (*path == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("column path filter must not be empty string")));

	xstr = pg_xmlCharStrndup(path, strlen(path));

	xtCxt->xpathscomp[colnum] = xmlXPathCompile(xstr);
	if (xtCxt->xpathscomp[colnum] == NULL || xtCxt->xmlerrcxt->err_occurred)
		xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_DATA_EXCEPTION,
					"invalid XPath expression");
#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableFetchRow
 *		Prepare the next "current" tuple for upcoming GetValue calls.
 *		Returns false if the row-filter expression returned no more rows.
 */
static bool
XmlTableFetchRow(TableFuncScanState *state)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableFetchRow");

	/* Propagate our own error context to libxml2 */
	xmlSetStructuredErrorFunc((void *) xtCxt->xmlerrcxt, xml_errorHandler);

	if (xtCxt->xpathobj == NULL)
	{
		xtCxt->xpathobj = xmlXPathCompiledEval(xtCxt->xpathcomp, xtCxt->xpathcxt);
		if (xtCxt->xpathobj == NULL || xtCxt->xmlerrcxt->err_occurred)
			xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"could not create XPath object");

		xtCxt->row_count = 0;
	}

	if (xtCxt->xpathobj->type == XPATH_NODESET)
	{
		if (xtCxt->xpathobj->nodesetval != NULL)
		{
			if (xtCxt->row_count++ < xtCxt->xpathobj->nodesetval->nodeNr)
				return true;
		}
	}

	return false;
#else
	NO_XML_SUPPORT();
	return false;
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableGetValue
 *		Return the value for column number 'colnum' for the current row.  If
 *		column -1 is requested, return representation of the whole row.
 *
 * This leaks memory, so be sure to reset often the context in which it's
 * called.
 */
static Datum
XmlTableGetValue(TableFuncScanState *state, int colnum,
				 Oid typid, int32 typmod, bool *isnull)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;
	Datum		result = (Datum) 0;
	xmlNodePtr	cur;
	char	   *cstr = NULL;
	volatile xmlXPathObjectPtr xpathobj = NULL;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableGetValue");

	Assert(xtCxt->xpathobj &&
		   xtCxt->xpathobj->type == XPATH_NODESET &&
		   xtCxt->xpathobj->nodesetval != NULL);

	/* Propagate our own error context to libxml2 */
	xmlSetStructuredErrorFunc((void *) xtCxt->xmlerrcxt, xml_errorHandler);

	*isnull = false;

	cur = xtCxt->xpathobj->nodesetval->nodeTab[xtCxt->row_count - 1];

	Assert(xtCxt->xpathscomp[colnum] != NULL);

	PG_TRY();
	{
		/* Set current node as entry point for XPath evaluation */
		xtCxt->xpathcxt->node = cur;

		/* Evaluate column path */
		xpathobj = xmlXPathCompiledEval(xtCxt->xpathscomp[colnum], xtCxt->xpathcxt);
		if (xpathobj == NULL || xtCxt->xmlerrcxt->err_occurred)
			xml_ereport(xtCxt->xmlerrcxt, ERROR, ERRCODE_INTERNAL_ERROR,
						"could not create XPath object");

		/*
		 * There are four possible cases, depending on the number of nodes
		 * returned by the XPath expression and the type of the target column:
		 * a) XPath returns no nodes.  b) The target type is XML (return all
		 * as XML).  For non-XML return types:  c) One node (return content).
		 * d) Multiple nodes (error).
		 */
		if (xpathobj->type == XPATH_NODESET)
		{
			int			count = 0;

			if (xpathobj->nodesetval != NULL)
				count = xpathobj->nodesetval->nodeNr;

			if (xpathobj->nodesetval == NULL || count == 0)
			{
				*isnull = true;
			}
			else
			{
				if (typid == XMLOID)
				{
					text	   *textstr;
					StringInfoData str;

					/* Concatenate serialized values */
					initStringInfo(&str);
					for (int i = 0; i < count; i++)
					{
						textstr =
							xml_xmlnodetoxmltype(xpathobj->nodesetval->nodeTab[i],
												 xtCxt->xmlerrcxt);

						appendStringInfoText(&str, textstr);
					}
					cstr = str.data;
				}
				else
				{
					xmlChar    *str;

					if (count > 1)
						ereport(ERROR,
								(errcode(ERRCODE_CARDINALITY_VIOLATION),
								 errmsg("more than one value returned by column XPath expression")));

					str = xmlXPathCastNodeSetToString(xpathobj->nodesetval);
					cstr = str ? xml_pstrdup_and_free(str) : "";
				}
			}
		}
		else if (xpathobj->type == XPATH_STRING)
		{
			/* Content should be escaped when target will be XML */
			if (typid == XMLOID)
				cstr = escape_xml((char *) xpathobj->stringval);
			else
				cstr = (char *) xpathobj->stringval;
		}
		else if (xpathobj->type == XPATH_BOOLEAN)
		{
			char		typcategory;
			bool		typispreferred;
			xmlChar    *str;

			/* Allow implicit casting from boolean to numbers */
			get_type_category_preferred(typid, &typcategory, &typispreferred);

			if (typcategory != TYPCATEGORY_NUMERIC)
				str = xmlXPathCastBooleanToString(xpathobj->boolval);
			else
				str = xmlXPathCastNumberToString(xmlXPathCastBooleanToNumber(xpathobj->boolval));

			cstr = xml_pstrdup_and_free(str);
		}
		else if (xpathobj->type == XPATH_NUMBER)
		{
			xmlChar    *str;

			str = xmlXPathCastNumberToString(xpathobj->floatval);
			cstr = xml_pstrdup_and_free(str);
		}
		else
			elog(ERROR, "unexpected XPath object type %u", xpathobj->type);

		/*
		 * By here, either cstr contains the result value, or the isnull flag
		 * has been set.
		 */
		Assert(cstr || *isnull);

		if (!*isnull)
			result = InputFunctionCall(&state->in_functions[colnum],
									   cstr,
									   state->typioparams[colnum],
									   typmod);
	}
	PG_FINALLY();
	{
		if (xpathobj != NULL)
			xmlXPathFreeObject(xpathobj);
	}
	PG_END_TRY();

	return result;
#else
	NO_XML_SUPPORT();
	return 0;
#endif							/* not USE_LIBXML */
}

/*
 * XmlTableDestroyOpaque
 *		Release all libxml2 resources
 */
static void
XmlTableDestroyOpaque(TableFuncScanState *state)
{
#ifdef USE_LIBXML
	XmlTableBuilderData *xtCxt;

	xtCxt = GetXmlTableBuilderPrivateData(state, "XmlTableDestroyOpaque");

	/* Propagate our own error context to libxml2 */
	xmlSetStructuredErrorFunc((void *) xtCxt->xmlerrcxt, xml_errorHandler);

	if (xtCxt->xpathscomp != NULL)
	{
		int			i;

		for (i = 0; i < xtCxt->natts; i++)
			if (xtCxt->xpathscomp[i] != NULL)
				xmlXPathFreeCompExpr(xtCxt->xpathscomp[i]);
	}

	if (xtCxt->xpathobj != NULL)
		xmlXPathFreeObject(xtCxt->xpathobj);
	if (xtCxt->xpathcomp != NULL)
		xmlXPathFreeCompExpr(xtCxt->xpathcomp);
	if (xtCxt->xpathcxt != NULL)
		xmlXPathFreeContext(xtCxt->xpathcxt);
	if (xtCxt->doc != NULL)
		xmlFreeDoc(xtCxt->doc);
	if (xtCxt->ctxt != NULL)
		xmlFreeParserCtxt(xtCxt->ctxt);

	pg_xml_done(xtCxt->xmlerrcxt, true);

	/* not valid anymore */
	xtCxt->magic = 0;
	state->opaque = NULL;

#else
	NO_XML_SUPPORT();
#endif							/* not USE_LIBXML */
}
