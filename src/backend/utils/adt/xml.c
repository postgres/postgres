/*-------------------------------------------------------------------------
 *
 * xml.c
 *	  XML data type support.
 *
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/utils/adt/xml.c,v 1.23 2007/01/27 11:48:31 petere Exp $
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

#include "postgres.h"

#ifdef USE_LIBXML
#include <libxml/chvalid.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlsave.h>
#include <libxml/xmlwriter.h>
#endif /* USE_LIBXML */

#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "nodes/execnodes.h"
#include "parser/parse_expr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/xml.h"


#ifdef USE_LIBXML

static StringInfo xml_err_buf = NULL;

static void 	xml_init(void);
#ifdef NOT_USED
static void    *xml_palloc(size_t size);
static void    *xml_repalloc(void *ptr, size_t size);
static void 	xml_pfree(void *ptr);
static char    *xml_pstrdup(const char *string);
#endif
static void 	xml_ereport(int level, int sqlcode,
							const char *msg);
static void 	xml_errorHandler(void *ctxt, const char *msg, ...);
static void 	xml_ereport_by_code(int level, int sqlcode,
									const char *msg, int errcode);
static xmlChar *xml_text2xmlChar(text *in);
static int		parse_xml_decl(const xmlChar *str, size_t *lenp, xmlChar **version, xmlChar **encoding, int *standalone);
static bool		print_xml_decl(StringInfo buf, const xmlChar *version, pg_enc encoding, int standalone);
static xmlDocPtr xml_parse(text *data, bool is_document, bool preserve_whitespace, xmlChar *encoding);

#endif /* USE_LIBXML */

XmlBinaryType xmlbinary;
XmlOptionType xmloption;


#define NO_XML_SUPPORT() \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("no XML support in this installation")))


Datum
xml_in(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	char		*s = PG_GETARG_CSTRING(0);
	size_t		len;
	xmltype		*vardata;
	xmlDocPtr	 doc;

	len = strlen(s);
	vardata = palloc(len + VARHDRSZ);
	VARATT_SIZEP(vardata) = len + VARHDRSZ;
	memcpy(VARDATA(vardata), s, len);

	/*
	 * Parse the data to check if it is well-formed XML data.  Assume
	 * that ERROR occurred if parsing failed.
	 */
	doc = xml_parse(vardata, (xmloption == XMLOPTION_DOCUMENT), true, NULL);
	xmlFreeDoc(doc);

	PG_RETURN_XML_P(vardata);
#else
	NO_XML_SUPPORT();
	return 0;
#endif
}


#define PG_XML_DEFAULT_VERSION "1.0"


static char *
xml_out_internal(xmltype *x, pg_enc target_encoding)
{
	char		*str;
	size_t		len;
#ifdef USE_LIBXML
	xmlChar		*version;
	xmlChar		*encoding;
	int			standalone;
	int			res_code;
#endif

	len = VARSIZE(x) - VARHDRSZ;
	str = palloc(len + 1);
	memcpy(str, VARDATA(x), len);
	str[len] = '\0';

#ifdef USE_LIBXML
	if ((res_code = parse_xml_decl((xmlChar *) str, &len, &version, &encoding, &standalone)) == 0)
	{
		StringInfoData buf;

		initStringInfo(&buf);

		if (!print_xml_decl(&buf, version, target_encoding, standalone))
		{
			/*
			 * If we are not going to produce an XML declaration, eat
			 * a single newline in the original string to prevent
			 * empty first lines in the output.
			 */
			if (*(str + len) == '\n')
				len += 1;
		}
		appendStringInfoString(&buf, str + len);

		return buf.data;
	}

	xml_ereport_by_code(WARNING, ERRCODE_INTERNAL_ERROR,
						"could not parse XML declaration in stored value", res_code);
#endif
	return str;
}


Datum
xml_out(PG_FUNCTION_ARGS)
{
	xmltype	   *x = PG_GETARG_XML_P(0);

	/*
	 * xml_out removes the encoding property in all cases.  This is
	 * because we cannot control from here whether the datum will be
	 * converted to a different client encoding, so we'd do more harm
	 * than good by including it.
	 */
	PG_RETURN_CSTRING(xml_out_internal(x, 0));
}


Datum
xml_recv(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	xmltype	   *result;
	char	   *str;
	char	   *newstr;
	int			nbytes;
	xmlDocPtr	doc;
	xmlChar	   *encoding = NULL;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);

	result = palloc(nbytes + VARHDRSZ);
	VARATT_SIZEP(result) = nbytes + VARHDRSZ;
	memcpy(VARDATA(result), str, nbytes);

	parse_xml_decl((xmlChar *) str, NULL, NULL, &encoding, NULL);

	/*
	 * Parse the data to check if it is well-formed XML data.  Assume
	 * that ERROR occurred if parsing failed.
	 */
	doc = xml_parse(result, (xmloption == XMLOPTION_DOCUMENT), true, encoding);
	xmlFreeDoc(doc);

	newstr = (char *) pg_do_encoding_conversion((unsigned char *) str,
												nbytes,
												encoding ? pg_char_to_encoding((char *) encoding) : PG_UTF8,
												GetDatabaseEncoding());

	pfree(str);

	if (newstr != str)
	{
		free(result);

		nbytes = strlen(newstr);

		result = palloc(nbytes + VARHDRSZ);
		VARATT_SIZEP(result) = nbytes + VARHDRSZ;
		memcpy(VARDATA(result), newstr, nbytes);
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
	xmltype	   *x = PG_GETARG_XML_P(0);
	char	   *outval = xml_out_internal(x, pg_get_client_encoding());
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendstring(&buf, outval);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


#ifdef USE_LIBXML
static void
appendStringInfoText(StringInfo str, const text *t)
{
	appendBinaryStringInfo(str, VARDATA(t), VARSIZE(t) - VARHDRSZ);
}


static xmltype *
stringinfo_to_xmltype(StringInfo buf)
{
	int32 len;
	xmltype *result;

	len = buf->len + VARHDRSZ;
	result = palloc(len);
	VARATT_SIZEP(result) = len;
	memcpy(VARDATA(result), buf->data, buf->len);

	return result;
}


#ifdef NOT_USED
static xmltype *
cstring_to_xmltype(const char *string)
{
	int32		len;
	xmltype	   *result;

	len = strlen(string) + VARHDRSZ;
	result = palloc(len);
	VARATT_SIZEP(result) = len;
	memcpy(VARDATA(result), string, len - VARHDRSZ);

	return result;
}
#endif


static xmltype *
xmlBuffer_to_xmltype(xmlBufferPtr buf)
{
	int32		len;
	xmltype	   *result;

	len = xmlBufferLength(buf) + VARHDRSZ;
	result = palloc(len);
	VARATT_SIZEP(result) = len;
	memcpy(VARDATA(result), xmlBufferContent(buf), len - VARHDRSZ);

	return result;
}
#endif


Datum
xmlcomment(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text *arg = PG_GETARG_TEXT_P(0);
	int len =  VARSIZE(arg) - VARHDRSZ;
	StringInfoData buf;
	int i;

	/* check for "--" in string or "-" at the end */
	for (i = 1; i < len; i++)
		if ((VARDATA(arg)[i] == '-' && VARDATA(arg)[i - 1] == '-')
			|| (VARDATA(arg)[i] == '-' && i == len - 1))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_XML_COMMENT),
							 errmsg("invalid XML comment")));

	initStringInfo(&buf);
	appendStringInfo(&buf, "<!--");
	appendStringInfoText(&buf, arg);
	appendStringInfo(&buf, "-->");

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
	StringInfoData buf;
	ListCell   *v;

	int			global_standalone = 1;
	xmlChar	   *global_version = NULL;
	bool		global_version_no_value = false;

	initStringInfo(&buf);
	foreach(v, args)
	{
		size_t		len;
		xmlChar	   *version;
		int			standalone;
		xmltype	   *x = DatumGetXmlP(PointerGetDatum(lfirst(v)));
		char	   *str;

		len = VARSIZE(x) - VARHDRSZ;
		str = palloc(len + 1);
		memcpy(str, VARDATA(x), len);
		str[len] = '\0';

		parse_xml_decl((xmlChar *) str, &len, &version, NULL, &standalone);

		if (standalone == 0 && global_standalone == 1)
			global_standalone = 0;
		if (standalone < 0)
			global_standalone = -1;

		if (!version)
			global_version_no_value = true;
		else if (!global_version)
			global_version = xmlStrdup(version);
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
					   (!global_version_no_value && global_version) ? global_version : NULL,
					   0,
					   global_standalone);

		appendStringInfoString(&buf2, buf.data);
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
		PG_RETURN_XML_P(xmlconcat(list_make2(PG_GETARG_XML_P(0), PG_GETARG_XML_P(1))));
}


Datum
texttoxml(PG_FUNCTION_ARGS)
{
	text	   *data = PG_GETARG_TEXT_P(0);

	PG_RETURN_XML_P(xmlparse(data, (xmloption == XMLOPTION_DOCUMENT), true));
}


xmltype *
xmlelement(XmlExprState *xmlExpr, ExprContext *econtext)
{
#ifdef USE_LIBXML
	XmlExpr	   *xexpr = (XmlExpr *) xmlExpr->xprstate.expr;
	int			i;
	ListCell   *arg;
	ListCell   *narg;
	bool		isnull;
	xmltype	   *result;
	Datum		value;
	char	   *str;

	xmlBufferPtr buf;
	xmlTextWriterPtr writer;

	buf = xmlBufferCreate();
	writer = xmlNewTextWriterMemory(buf, 0);

	xmlTextWriterStartElement(writer, (xmlChar *) xexpr->name);

	i = 0;
	forboth(arg, xmlExpr->named_args, narg, xexpr->arg_names)
	{
		ExprState 	*e = (ExprState *) lfirst(arg);
		char	*argname = strVal(lfirst(narg));

		value = ExecEvalExpr(e, econtext, &isnull, NULL);
		if (!isnull)
		{
			str = OutputFunctionCall(&xmlExpr->named_outfuncs[i], value);
			xmlTextWriterWriteAttribute(writer, (xmlChar *) argname, (xmlChar *) str);
			pfree(str);
		}
		i++;
	}

	foreach(arg, xmlExpr->args)
	{
		ExprState 	*e = (ExprState *) lfirst(arg);

		value = ExecEvalExpr(e, econtext, &isnull, NULL);
		if (!isnull)
			xmlTextWriterWriteRaw(writer, (xmlChar *) map_sql_value_to_xml_value(value, exprType((Node *) e->expr)));
	}

	xmlTextWriterEndElement(writer);
	xmlFreeTextWriter(writer);

	result = xmlBuffer_to_xmltype(buf);
	xmlBufferFree(buf);
	return result;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlparse(text *data, bool is_document, bool preserve_whitespace)
{
#ifdef USE_LIBXML
	xmlDocPtr	doc;

	doc = xml_parse(data, is_document, preserve_whitespace, NULL);
	xmlFreeDoc(doc);

	return (xmltype *) data;
#else
	NO_XML_SUPPORT();
	return NULL;
#endif
}


xmltype *
xmlpi(char *target, text *arg, bool arg_is_null, bool *result_is_null)
{
#ifdef USE_LIBXML
	xmltype *result;
	StringInfoData buf;

	if (pg_strncasecmp(target, "xml", 3) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),	/* really */
				 errmsg("invalid XML processing instruction"),
				 errdetail("XML processing instruction target name cannot start with \"xml\".")));

	/*
	 * Following the SQL standard, the null check comes after the
	 * syntax check above.
	 */
	*result_is_null = arg_is_null;
	if (*result_is_null)
		return NULL;		

	initStringInfo(&buf);

	appendStringInfo(&buf, "<?%s", target);

	if (arg != NULL)
	{
		char *string;

		string = DatumGetCString(DirectFunctionCall1(textout,
													 PointerGetDatum(arg)));
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
	xmlChar	   *orig_version;
	int			orig_standalone;
	StringInfoData buf;

	len = VARSIZE(data) - VARHDRSZ;
	str = palloc(len + 1);
	memcpy(str, VARDATA(data), len);
	str[len] = '\0';

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
 * TODO !!! use text instead of cstring for second arg
 * TODO allow passing DTD as a string value (not only as an URI)
 * TODO redesign (see comment with '!!!' below)
 */
Datum
xmlvalidate(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXML
	text				*data = PG_GETARG_TEXT_P(0);
	text				*dtdOrUri = PG_GETARG_TEXT_P(1);
	bool				result = false;
	xmlParserCtxtPtr	ctxt = NULL;
	xmlDocPtr 			doc = NULL;
	xmlDtdPtr			dtd = NULL;

	xml_init();

	/* We use a PG_TRY block to ensure libxml is cleaned up on error */
	PG_TRY();
	{
		ctxt = xmlNewParserCtxt();
		if (ctxt == NULL)
			xml_ereport(ERROR, ERRCODE_INTERNAL_ERROR,
						"could not allocate parser context");

		doc = xmlCtxtReadMemory(ctxt, (char *) VARDATA(data),
								VARSIZE(data) - VARHDRSZ,
								NULL, NULL, 0);
		if (doc == NULL)
			xml_ereport(ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"could not parse XML data");

#if 0
		uri = xmlCreateURI();
		elog(NOTICE, "dtd - %s", dtdOrUri);
		dtd = palloc(sizeof(xmlDtdPtr));
		uri = xmlParseURI(dtdOrUri);
		if (uri == NULL)
			xml_ereport(ERROR, ERRCODE_INTERNAL_ERROR,
						"not implemented yet... (TODO)");
		else
#endif
			dtd = xmlParseDTD(NULL, xml_text2xmlChar(dtdOrUri));

		if (dtd == NULL)
			xml_ereport(ERROR, ERRCODE_INVALID_XML_DOCUMENT,
						"could not load DTD");

		if (xmlValidateDtd(xmlNewValidCtxt(), doc, dtd) == 1)
			result = true;

		if (!result)
			xml_ereport(NOTICE, ERRCODE_INVALID_XML_DOCUMENT,
						"validation against DTD failed");

#if 0
		if (uri)
			xmlFreeURI(uri);
#endif
		if (dtd)
			xmlFreeDtd(dtd);
		if (doc)
			xmlFreeDoc(doc);
		if (ctxt)
			xmlFreeParserCtxt(ctxt);
		xmlCleanupParser();
	}
	PG_CATCH();
	{
#if 0
		if (uri)
			xmlFreeURI(uri);
#endif
		if (dtd)
			xmlFreeDtd(dtd);
		if (doc)
			xmlFreeDoc(doc);
		if (ctxt)
			xmlFreeParserCtxt(ctxt);
		xmlCleanupParser();

		PG_RE_THROW();
	}
	PG_END_TRY();

	PG_RETURN_BOOL(result);
#else /* not USE_LIBXML */
	NO_XML_SUPPORT();
	return 0;
#endif /* not USE_LIBXML */
}


bool
xml_is_document(xmltype *arg)
{
#ifdef USE_LIBXML
	bool		result;
	xmlDocPtr	doc = NULL;
	MemoryContext ccxt = CurrentMemoryContext;

	PG_TRY();
	{
		doc = xml_parse((text *) arg, true, true, NULL);
		result = true;
	}
	PG_CATCH();
	{
		ErrorData *errdata;
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
#else /* not USE_LIBXML */
	NO_XML_SUPPORT();
	return false;
#endif /* not USE_LIBXML */
}


#ifdef USE_LIBXML

/*
 * Container for some init stuff (not good design!)
 * TODO xmlChar is utf8-char, make proper tuning (initdb with enc!=utf8 and check)
 */
static void
xml_init(void)
{
	/*
	 * Currently, we have no pure UTF-8 support for internals -- check
	 * if we can work.
	 */
	if (sizeof (char) != sizeof (xmlChar))
		ereport(ERROR,
				(errmsg("could not initialize XML library"),
				 errdetail("libxml2 has incompatible char type: sizeof(char)=%u, sizeof(xmlChar)=%u.",
						   (int) sizeof(char), (int) sizeof(xmlChar))));

	if (xml_err_buf == NULL)
	{
		/* First time through: create error buffer in permanent context */
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		xml_err_buf = makeStringInfo();
		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		/* Reset pre-existing buffer to empty */
		xml_err_buf->data[0] = '\0';
		xml_err_buf->len = 0;
	}
	/* Now that xml_err_buf exists, safe to call xml_errorHandler */
	xmlSetGenericErrorFunc(NULL, xml_errorHandler);

#ifdef NOT_USED
	/*
	 * FIXME: This doesn't work because libxml assumes that whatever
	 * libxml allocates, only libxml will free, so we can't just drop
	 * memory contexts behind it.  This needs to be refined.
	 */
	xmlMemSetup(xml_pfree, xml_palloc, xml_repalloc, xml_pstrdup);
#endif
	xmlInitParser();
	LIBXML_TEST_VERSION;
}


/*
 * SQL/XML allows storing "XML documents" or "XML content".  "XML
 * documents" are specified by the XML specification and are parsed
 * easily by libxml.  "XML content" is specified by SQL/XML as the
 * production "XMLDecl? content".  But libxml can only parse the
 * "content" part, so we have to parse the XML declaration ourselves
 * to complete this.
 */

#define CHECK_XML_SPACE(p) if (!xmlIsBlank_ch(*(p))) return XML_ERR_SPACE_REQUIRED
#define SKIP_XML_SPACE(p) while (xmlIsBlank_ch(*(p))) (p)++

static int
parse_xml_decl(const xmlChar *str, size_t *lenp, xmlChar **version, xmlChar **encoding, int *standalone)
{
	const xmlChar *p;
	const xmlChar *save_p;
	size_t		len;

	p = str;

	if (version)
		*version = NULL;
	if (encoding)
		*encoding = NULL;
	if (standalone)
		*standalone = -1;

	if (xmlStrncmp(p, (xmlChar *)"<?xml", 5) != 0)
		goto finished;

	p += 5;

	/* version */
	CHECK_XML_SPACE(p);
	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *)"version", 7) != 0)
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
			*version = xmlStrndup(p + 1, q - p - 1);
		p = q + 1;
	}
	else
		return XML_ERR_VERSION_MISSING;

	/* encoding */
	save_p = p;
	SKIP_XML_SPACE(p);
	if (xmlStrncmp(p, (xmlChar *)"encoding", 8) == 0)
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
			*encoding = xmlStrndup(p + 1, q - p - 1);
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
	if (xmlStrncmp(p, (xmlChar *)"standalone", 10) == 0)
	{
		CHECK_XML_SPACE(save_p);
		p += 10;
		SKIP_XML_SPACE(p);
		if (*p != '=')
			return XML_ERR_STANDALONE_VALUE;
		p += 1;
		SKIP_XML_SPACE(p);
		if (xmlStrncmp(p, (xmlChar *)"'yes'", 5) == 0 || xmlStrncmp(p, (xmlChar *)"\"yes\"", 5) == 0)
		{
			*standalone = 1;
			p += 5;
		}
		else if (xmlStrncmp(p, (xmlChar *)"'no'", 4) == 0 || xmlStrncmp(p, (xmlChar *)"\"no\"", 4) == 0)
		{
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
	if (xmlStrncmp(p, (xmlChar *)"?>", 2) != 0)
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
print_xml_decl(StringInfo buf, const xmlChar *version, pg_enc encoding, int standalone)
{
	if ((version && strcmp((char *) version, PG_XML_DEFAULT_VERSION) != 0)
		|| (encoding && encoding != PG_UTF8)
		|| standalone != -1)
	{
		appendStringInfoString(buf, "<?xml");

		if (version)
			appendStringInfo(buf, " version=\"%s\"", version);
		else
			appendStringInfo(buf, " version=\"%s\"", PG_XML_DEFAULT_VERSION);

		if (encoding && encoding != PG_UTF8)
			/* XXX might be useful to convert this to IANA names
			 * (ISO-8859-1 instead of LATIN1 etc.); needs field
			 * experience */
			appendStringInfo(buf, " encoding=\"%s\"", pg_encoding_to_char(encoding));

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
 * Convert a C string to XML internal representation
 *
 * TODO maybe, libxml2's xmlreader is better? (do not construct DOM, yet do not use SAX - see xml_reader.c)
 */
static xmlDocPtr
xml_parse(text *data, bool is_document, bool preserve_whitespace, xmlChar *encoding)
{
	int32				len;
	xmlChar				*string;
	xmlChar				*utf8string;
	xmlParserCtxtPtr 	ctxt = NULL;
	xmlDocPtr 			doc = NULL;

	len = VARSIZE(data) - VARHDRSZ; /* will be useful later */
	string = xml_text2xmlChar(data);

	utf8string = pg_do_encoding_conversion(string,
										   len,
										   encoding
										   ? pg_char_to_encoding((char *) encoding)
										   : GetDatabaseEncoding(),
										   PG_UTF8);

	xml_init();

	/* We use a PG_TRY block to ensure libxml is cleaned up on error */
	PG_TRY();
	{
		ctxt = xmlNewParserCtxt();
		if (ctxt == NULL)
			xml_ereport(ERROR, ERRCODE_INTERNAL_ERROR,
						"could not allocate parser context");

		if (is_document)
		{
			/*
			 * Note, that here we try to apply DTD defaults
			 * (XML_PARSE_DTDATTR) according to SQL/XML:10.16.7.d:
			 * 'Default valies defined by internal DTD are applied'.
			 * As for external DTDs, we try to support them too, (see
			 * SQL/XML:10.16.7.e)
			 */
			doc = xmlCtxtReadDoc(ctxt, utf8string,
								 NULL,
								 "UTF-8",
								 XML_PARSE_NOENT | XML_PARSE_DTDATTR
								 | (preserve_whitespace ? 0 : XML_PARSE_NOBLANKS));
			if (doc == NULL)
				xml_ereport(ERROR, ERRCODE_INVALID_XML_DOCUMENT,
							"invalid XML document");
		}
		else
		{
			int			res_code;
			size_t count;
			xmlChar	   *version = NULL;
			int standalone = -1;

			doc = xmlNewDoc(NULL);

			res_code = parse_xml_decl(utf8string, &count, &version, NULL, &standalone);
			if (res_code != 0)
				xml_ereport_by_code(ERROR, ERRCODE_INVALID_XML_CONTENT,
									"invalid XML content: invalid XML declaration", res_code);

			res_code = xmlParseBalancedChunkMemory(doc, NULL, NULL, 0, utf8string + count, NULL);
			if (res_code != 0)
				xml_ereport(ERROR, ERRCODE_INVALID_XML_CONTENT,
							"invalid XML content");

			doc->version = xmlStrdup(version);
			doc->encoding = xmlStrdup((xmlChar *) "UTF-8");
			doc->standalone = standalone;
		}

		if (ctxt)
			xmlFreeParserCtxt(ctxt);
		xmlCleanupParser();
	}
	PG_CATCH();
	{
		if (doc)
			xmlFreeDoc(doc);
		doc = NULL;
		if (ctxt)
			xmlFreeParserCtxt(ctxt);
		xmlCleanupParser();

		PG_RE_THROW();
	}
	PG_END_TRY();

	return doc;
}


/*
 * xmlChar<->text convertions
 */
static xmlChar *
xml_text2xmlChar(text *in)
{
	int32 		len = VARSIZE(in) - VARHDRSZ;
	xmlChar		*res;

	res = palloc(len + 1);
	memcpy(res, VARDATA(in), len);
	res[len] = '\0';

	return(res);
}


#ifdef NOT_USED
/*
 * Wrappers for memory management functions
 */
static void *
xml_palloc(size_t size)
{
	return palloc(size);
}


static void *
xml_repalloc(void *ptr, size_t size)
{
	return repalloc(ptr, size);
}


static void
xml_pfree(void *ptr)
{
	pfree(ptr);
}


static char *
xml_pstrdup(const char *string)
{
	return pstrdup(string);
}
#endif /* NOT_USED */


/*
 * Wrapper for "ereport" function for XML-related errors.  The "msg"
 * is the SQL-level message; some can be adopted from the SQL/XML
 * standard.  This function adds libxml's native error messages, if
 * any, as detail.
 */
static void
xml_ereport(int level, int sqlcode,
			const char *msg)
{
	char *detail;

	if (xml_err_buf->len > 0)
	{
		detail = pstrdup(xml_err_buf->data);
		xml_err_buf->data[0] = '\0';
		xml_err_buf->len = 0;
	}
	else
		detail = NULL;

	/* libxml error messages end in '\n'; get rid of it */
	if (detail)
	{
		size_t len;

		len = strlen(detail);
		if (len > 0 && detail[len-1] == '\n')
			detail[len-1] = '\0';

		ereport(level,
				(errcode(sqlcode),
				 errmsg("%s", msg),
				 errdetail("%s", detail)));
	}
	else
	{
		ereport(level,
				(errcode(sqlcode),
				 errmsg("%s", msg)));
	}
}


/*
 * Error handler for libxml error messages
 */
static void
xml_errorHandler(void *ctxt, const char *msg,...)
{
	/* Append the formatted text to xml_err_buf */
	for (;;)
	{
		va_list		args;
		bool		success;

		/* Try to format the data. */
		va_start(args, msg);
		success = appendStringInfoVA(xml_err_buf, msg, args);
		va_end(args);

		if (success)
			break;

		/* Double the buffer size and try again. */
		enlargeStringInfo(xml_err_buf, xml_err_buf->maxlen);
	}
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
			det = "Invalid character value";
			break;
		case XML_ERR_SPACE_REQUIRED:
			det = "Space required";
			break;
		case XML_ERR_STANDALONE_VALUE:
			det = "standalone accepts only 'yes' or 'no'";
			break;
		case XML_ERR_VERSION_MISSING:
			det = "Malformed declaration expecting version";
			break;
		case XML_ERR_MISSING_ENCODING:
			det = "Missing encoding in text declaration";
			break;
		case XML_ERR_XMLDECL_NOT_FINISHED:
			det = "Parsing XML declaration: '?>' expected";
			break;
        default:
            det = "Unrecognized libxml error code: %d";
			break;
	}

	ereport(level,
			(errcode(sqlcode),
			 errmsg("%s", msg),
			 errdetail(det, code)));
}


/*
 * Convert one char in the current server encoding to a Unicode codepoint.
 */
static pg_wchar
sqlchar_to_unicode(char *s)
{
	char *utf8string;
	pg_wchar ret[2];			/* need space for trailing zero */

	utf8string = (char *) pg_do_encoding_conversion((unsigned char *) s,
													pg_mblen(s),
													GetDatabaseEncoding(),
													PG_UTF8);

	pg_encoding_mb2wchar_with_len(PG_UTF8, utf8string, ret, pg_mblen(s));

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
#endif /* USE_LIBXML */


/*
 * Map SQL identifier to XML name; see SQL/XML:2003 section 9.1.
 */
char *
map_sql_identifier_to_xml_name(char *ident, bool fully_escaped)
{
#ifdef USE_LIBXML
	StringInfoData buf;
	char *p;

	initStringInfo(&buf);

	for (p = ident; *p; p += pg_mblen(p))
	{
		if (*p == ':' && (p == ident || fully_escaped))
			appendStringInfo(&buf, "_x003A_");
		else if (*p == '_' && *(p+1) == 'x')
			appendStringInfo(&buf, "_x005F_");
		else if (fully_escaped && p == ident &&
				 pg_strncasecmp(p, "xml", 3) == 0)
		{
			if (*p == 'x')
				appendStringInfo(&buf, "_x0078_");
			else
				appendStringInfo(&buf, "_x0058_");
		}
		else
		{
			pg_wchar u = sqlchar_to_unicode(p);

			if ((p == ident)
				? !is_valid_xml_namefirst(u)
				: !is_valid_xml_namechar(u))
				appendStringInfo(&buf, "_x%04X_", (unsigned int) u);
			else
				appendBinaryStringInfo(&buf, p, pg_mblen(p));
		}
	}

	return buf.data;
#else /* not USE_LIBXML */
	NO_XML_SUPPORT();
	return NULL;
#endif /* not USE_LIBXML */
}


/*
 * Map a Unicode codepoint into the current server encoding.
 */
static char *
unicode_to_sqlchar(pg_wchar c)
{
	static unsigned char utf8string[5];	/* need trailing zero */

	if (c <= 0x7F)
	{
		utf8string[0] = c;
	}
	else if (c <= 0x7FF)
	{
		utf8string[0] = 0xC0 | ((c >> 6) & 0x1F);
		utf8string[1] = 0x80 | (c & 0x3F);
	}
	else if (c <= 0xFFFF)
	{
		utf8string[0] = 0xE0 | ((c >> 12) & 0x0F);
		utf8string[1] = 0x80 | ((c >> 6) & 0x3F);
		utf8string[2] = 0x80 | (c & 0x3F);
	}
	else
	{
		utf8string[0] = 0xF0 | ((c >> 18) & 0x07);
		utf8string[1] = 0x80 | ((c >> 12) & 0x3F);
		utf8string[2] = 0x80 | ((c >> 6) & 0x3F);
		utf8string[3] = 0x80 | (c & 0x3F);
	}

	return (char *) pg_do_encoding_conversion(utf8string,
											  pg_mblen((char *) utf8string),
											  PG_UTF8,
											  GetDatabaseEncoding());
}


/*
 * Map XML name to SQL identifier; see SQL/XML:2003 section 9.17.
 */
char *
map_xml_name_to_sql_identifier(char *name)
{
	StringInfoData buf;
	char *p;

	initStringInfo(&buf);

	for (p = name; *p; p += pg_mblen(p))
	{
		if (*p == '_' && *(p+1) == 'x'
			&& isxdigit((unsigned char) *(p+2))
			&& isxdigit((unsigned char) *(p+3))
			&& isxdigit((unsigned char) *(p+4))
			&& isxdigit((unsigned char) *(p+5))
			&& *(p+6) == '_')
		{
			unsigned int u;

			sscanf(p + 2, "%X", &u);
			appendStringInfoString(&buf, unicode_to_sqlchar(u));
			p += 6;
		}
		else
			appendBinaryStringInfo(&buf, p, pg_mblen(p));
	}

	return buf.data;
}


/*
 * Map SQL value to XML value; see SQL/XML:2003 section 9.16.
 */
char *
map_sql_value_to_xml_value(Datum value, Oid type)
{
	StringInfoData buf;

	initStringInfo(&buf);

	if (is_array_type(type))
	{
		int i;
		ArrayType *array;
		Oid elmtype;
		int16 elmlen;
		bool elmbyval;
		char elmalign;

		array = DatumGetArrayTypeP(value);

		/* TODO: need some code-fu here to remove this limitation */
		if (ARR_NDIM(array) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only supported for one-dimensional array")));

		elmtype = ARR_ELEMTYPE(array);
		get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

		for (i = ARR_LBOUND(array)[0];
			 i < ARR_LBOUND(array)[0] + ARR_DIMS(array)[0];
			 i++)
		{
			Datum subval;
			bool isnull;

			subval = array_ref(array, 1, &i, -1, elmlen, elmbyval, elmalign, &isnull);
			appendStringInfoString(&buf, "<element>");
			appendStringInfoString(&buf, map_sql_value_to_xml_value(subval, elmtype));
			appendStringInfoString(&buf, "</element>");
		}
	}
	else
	{
		Oid typeOut;
		bool isvarlena;
		char *p, *str;

		getTypeOutputInfo(type, &typeOut, &isvarlena);
		str = OidOutputFunctionCall(typeOut, value);

		if (type == XMLOID)
			return str;

#ifdef USE_LIBXML
		if (type == BYTEAOID)
		{
			xmlBufferPtr buf;
			xmlTextWriterPtr writer;
			char *result;

			buf = xmlBufferCreate();
			writer = xmlNewTextWriterMemory(buf, 0);

			if (xmlbinary == XMLBINARY_BASE64)
				xmlTextWriterWriteBase64(writer, VARDATA(value), 0, VARSIZE(value) - VARHDRSZ);
			else
				xmlTextWriterWriteBinHex(writer, VARDATA(value), 0, VARSIZE(value) - VARHDRSZ);

			xmlFreeTextWriter(writer);
			result = pstrdup((const char *) xmlBufferContent(buf));
			xmlBufferFree(buf);
			return result;
		}
#endif /* USE_LIBXML */

		for (p = str; *p; p += pg_mblen(p))
		{
			switch (*p)
			{
				case '&':
					appendStringInfo(&buf, "&amp;");
					break;
				case '<':
					appendStringInfo(&buf, "&lt;");
					break;
				case '>':
					appendStringInfo(&buf, "&gt;");
					break;
				case '\r':
					appendStringInfo(&buf, "&#x0d;");
					break;
				default:
					appendBinaryStringInfo(&buf, p, pg_mblen(p));
					break;
			}
		}
	}

	return buf.data;
}
