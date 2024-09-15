/*
 * contrib/xml2/xpath.c
 *
 * Parser interface for DOM-based parser (libxml) rather than
 * stream-based SAX-type parser
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/xml.h"

/* libxml includes */

#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlerror.h>
#include <libxml/parserInternals.h>

PG_MODULE_MAGIC;

/* exported for use by xslt_proc.c */

PgXmlErrorContext *pgxml_parser_init(PgXmlStrictness strictness);

/* workspace for pgxml_xpath() */

typedef struct
{
	xmlDocPtr	doctree;
	xmlXPathContextPtr ctxt;
	xmlXPathObjectPtr res;
} xpath_workspace;

/* local declarations */

static xmlChar *pgxmlNodeSetToText(xmlNodeSetPtr nodeset,
								   xmlChar *toptagname, xmlChar *septagname,
								   xmlChar *plainsep);

static text *pgxml_result_to_text(xmlXPathObjectPtr res, xmlChar *toptag,
								  xmlChar *septag, xmlChar *plainsep);

static xmlChar *pgxml_texttoxmlchar(text *textstring);

static xmlXPathObjectPtr pgxml_xpath(text *document, xmlChar *xpath,
									 xpath_workspace *workspace);

static void cleanup_workspace(xpath_workspace *workspace);


/*
 * Initialize for xml parsing.
 *
 * As with the underlying pg_xml_init function, calls to this MUST be followed
 * by a PG_TRY block that guarantees that pg_xml_done is called.
 */
PgXmlErrorContext *
pgxml_parser_init(PgXmlStrictness strictness)
{
	PgXmlErrorContext *xmlerrcxt;

	/* Set up error handling (we share the core's error handler) */
	xmlerrcxt = pg_xml_init(strictness);

	/* Note: we're assuming an elog cannot be thrown by the following calls */

	/* Initialize libxml */
	xmlInitParser();

	return xmlerrcxt;
}


/*
 * Returns true if document is well-formed
 *
 * Note: this has been superseded by a core function.  We still have to
 * have it in the contrib module so that existing SQL-level references
 * to the function won't fail; but in normal usage with up-to-date SQL
 * definitions for the contrib module, this won't be called.
 */

PG_FUNCTION_INFO_V1(xml_is_well_formed);

Datum
xml_is_well_formed(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_PP(0);	/* document buffer */
	bool		result = false;
	int32		docsize = VARSIZE_ANY_EXHDR(t);
	xmlDocPtr	doctree;
	PgXmlErrorContext *xmlerrcxt;

	xmlerrcxt = pgxml_parser_init(PG_XML_STRICTNESS_LEGACY);

	PG_TRY();
	{
		doctree = xmlReadMemory((char *) VARDATA_ANY(t), docsize,
								NULL, NULL, XML_PARSE_NOENT);

		result = (doctree != NULL);

		if (doctree != NULL)
			xmlFreeDoc(doctree);
	}
	PG_CATCH();
	{
		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	pg_xml_done(xmlerrcxt, false);

	PG_RETURN_BOOL(result);
}


/* Encodes special characters (<, >, &, " and \r) as XML entities */

PG_FUNCTION_INFO_V1(xml_encode_special_chars);

Datum
xml_encode_special_chars(PG_FUNCTION_ARGS)
{
	text	   *tin = PG_GETARG_TEXT_PP(0);
	text	   *tout;
	xmlChar    *ts,
			   *tt;

	ts = pgxml_texttoxmlchar(tin);

	tt = xmlEncodeSpecialChars(NULL, ts);

	pfree(ts);

	tout = cstring_to_text((char *) tt);

	xmlFree(tt);

	PG_RETURN_TEXT_P(tout);
}

/*
 * Function translates a nodeset into a text representation
 *
 * iterates over each node in the set and calls xmlNodeDump to write it to
 * an xmlBuffer -from which an xmlChar * string is returned.
 *
 * each representation is surrounded by <tagname> ... </tagname>
 *
 * plainsep is an ordinary (not tag) separator - if used, then nodes are
 * cast to string as output method
 */
static xmlChar *
pgxmlNodeSetToText(xmlNodeSetPtr nodeset,
				   xmlChar *toptagname,
				   xmlChar *septagname,
				   xmlChar *plainsep)
{
	xmlBufferPtr buf;
	xmlChar    *result;
	int			i;

	buf = xmlBufferCreate();

	if ((toptagname != NULL) && (xmlStrlen(toptagname) > 0))
	{
		xmlBufferWriteChar(buf, "<");
		xmlBufferWriteCHAR(buf, toptagname);
		xmlBufferWriteChar(buf, ">");
	}
	if (nodeset != NULL)
	{
		for (i = 0; i < nodeset->nodeNr; i++)
		{
			if (plainsep != NULL)
			{
				xmlBufferWriteCHAR(buf,
								   xmlXPathCastNodeToString(nodeset->nodeTab[i]));

				/* If this isn't the last entry, write the plain sep. */
				if (i < (nodeset->nodeNr) - 1)
					xmlBufferWriteChar(buf, (char *) plainsep);
			}
			else
			{
				if ((septagname != NULL) && (xmlStrlen(septagname) > 0))
				{
					xmlBufferWriteChar(buf, "<");
					xmlBufferWriteCHAR(buf, septagname);
					xmlBufferWriteChar(buf, ">");
				}
				xmlNodeDump(buf,
							nodeset->nodeTab[i]->doc,
							nodeset->nodeTab[i],
							1, 0);

				if ((septagname != NULL) && (xmlStrlen(septagname) > 0))
				{
					xmlBufferWriteChar(buf, "</");
					xmlBufferWriteCHAR(buf, septagname);
					xmlBufferWriteChar(buf, ">");
				}
			}
		}
	}

	if ((toptagname != NULL) && (xmlStrlen(toptagname) > 0))
	{
		xmlBufferWriteChar(buf, "</");
		xmlBufferWriteCHAR(buf, toptagname);
		xmlBufferWriteChar(buf, ">");
	}
	result = xmlStrdup(buf->content);
	xmlBufferFree(buf);
	return result;
}


/* Translate a PostgreSQL "varlena" -i.e. a variable length parameter
 * into the libxml2 representation
 */
static xmlChar *
pgxml_texttoxmlchar(text *textstring)
{
	return (xmlChar *) text_to_cstring(textstring);
}

/* Publicly visible XPath functions */

/*
 * This is a "raw" xpath function. Check that it returns child elements
 * properly
 */
PG_FUNCTION_INFO_V1(xpath_nodeset);

Datum
xpath_nodeset(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *toptag = pgxml_texttoxmlchar(PG_GETARG_TEXT_PP(2));
	xmlChar    *septag = pgxml_texttoxmlchar(PG_GETARG_TEXT_PP(3));
	xmlChar    *xpath;
	text	   *xpres;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(document, xpath, &workspace);

	xpres = pgxml_result_to_text(res, toptag, septag, NULL);

	cleanup_workspace(&workspace);

	pfree(xpath);

	if (xpres == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(xpres);
}

/*
 * The following function is almost identical, but returns the elements in
 * a list.
 */
PG_FUNCTION_INFO_V1(xpath_list);

Datum
xpath_list(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *plainsep = pgxml_texttoxmlchar(PG_GETARG_TEXT_PP(2));
	xmlChar    *xpath;
	text	   *xpres;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(document, xpath, &workspace);

	xpres = pgxml_result_to_text(res, NULL, NULL, plainsep);

	cleanup_workspace(&workspace);

	pfree(xpath);

	if (xpres == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(xpres);
}


PG_FUNCTION_INFO_V1(xpath_string);

Datum
xpath_string(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *xpath;
	int32		pathsize;
	text	   *xpres;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	pathsize = VARSIZE_ANY_EXHDR(xpathsupp);

	/*
	 * We encapsulate the supplied path with "string()" = 8 chars + 1 for NUL
	 * at end
	 */
	/* We could try casting to string using the libxml function? */

	xpath = (xmlChar *) palloc(pathsize + 9);
	memcpy((char *) xpath, "string(", 7);
	memcpy((char *) (xpath + 7), VARDATA_ANY(xpathsupp), pathsize);
	xpath[pathsize + 7] = ')';
	xpath[pathsize + 8] = '\0';

	res = pgxml_xpath(document, xpath, &workspace);

	xpres = pgxml_result_to_text(res, NULL, NULL, NULL);

	cleanup_workspace(&workspace);

	pfree(xpath);

	if (xpres == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(xpres);
}


PG_FUNCTION_INFO_V1(xpath_number);

Datum
xpath_number(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *xpath;
	float4		fRes;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(document, xpath, &workspace);

	pfree(xpath);

	if (res == NULL)
		PG_RETURN_NULL();

	fRes = xmlXPathCastToNumber(res);

	cleanup_workspace(&workspace);

	if (xmlXPathIsNaN(fRes))
		PG_RETURN_NULL();

	PG_RETURN_FLOAT4(fRes);
}


PG_FUNCTION_INFO_V1(xpath_bool);

Datum
xpath_bool(PG_FUNCTION_ARGS)
{
	text	   *document = PG_GETARG_TEXT_PP(0);
	text	   *xpathsupp = PG_GETARG_TEXT_PP(1);	/* XPath expression */
	xmlChar    *xpath;
	int			bRes;
	xmlXPathObjectPtr res;
	xpath_workspace workspace;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(document, xpath, &workspace);

	pfree(xpath);

	if (res == NULL)
		PG_RETURN_BOOL(false);

	bRes = xmlXPathCastToBoolean(res);

	cleanup_workspace(&workspace);

	PG_RETURN_BOOL(bRes);
}



/* Core function to evaluate XPath query */

static xmlXPathObjectPtr
pgxml_xpath(text *document, xmlChar *xpath, xpath_workspace *workspace)
{
	int32		docsize = VARSIZE_ANY_EXHDR(document);
	PgXmlErrorContext *xmlerrcxt;
	xmlXPathCompExprPtr comppath;

	workspace->doctree = NULL;
	workspace->ctxt = NULL;
	workspace->res = NULL;

	xmlerrcxt = pgxml_parser_init(PG_XML_STRICTNESS_LEGACY);

	PG_TRY();
	{
		workspace->doctree = xmlReadMemory((char *) VARDATA_ANY(document),
										   docsize, NULL, NULL,
										   XML_PARSE_NOENT);
		if (workspace->doctree != NULL)
		{
			workspace->ctxt = xmlXPathNewContext(workspace->doctree);
			workspace->ctxt->node = xmlDocGetRootElement(workspace->doctree);

			/* compile the path */
			comppath = xmlXPathCtxtCompile(workspace->ctxt, xpath);
			if (comppath == NULL)
				xml_ereport(xmlerrcxt, ERROR, ERRCODE_EXTERNAL_ROUTINE_EXCEPTION,
							"XPath Syntax Error");

			/* Now evaluate the path expression. */
			workspace->res = xmlXPathCompiledEval(comppath, workspace->ctxt);

			xmlXPathFreeCompExpr(comppath);
		}
	}
	PG_CATCH();
	{
		cleanup_workspace(workspace);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (workspace->res == NULL)
		cleanup_workspace(workspace);

	pg_xml_done(xmlerrcxt, false);

	return workspace->res;
}

/* Clean up after processing the result of pgxml_xpath() */
static void
cleanup_workspace(xpath_workspace *workspace)
{
	if (workspace->res)
		xmlXPathFreeObject(workspace->res);
	workspace->res = NULL;
	if (workspace->ctxt)
		xmlXPathFreeContext(workspace->ctxt);
	workspace->ctxt = NULL;
	if (workspace->doctree)
		xmlFreeDoc(workspace->doctree);
	workspace->doctree = NULL;
}

static text *
pgxml_result_to_text(xmlXPathObjectPtr res,
					 xmlChar *toptag,
					 xmlChar *septag,
					 xmlChar *plainsep)
{
	xmlChar    *xpresstr;
	text	   *xpres;

	if (res == NULL)
		return NULL;

	switch (res->type)
	{
		case XPATH_NODESET:
			xpresstr = pgxmlNodeSetToText(res->nodesetval,
										  toptag,
										  septag, plainsep);
			break;

		case XPATH_STRING:
			xpresstr = xmlStrdup(res->stringval);
			break;

		default:
			elog(NOTICE, "unsupported XQuery result: %d", res->type);
			xpresstr = xmlStrdup((const xmlChar *) "<unsupported/>");
	}

	/* Now convert this result back to text */
	xpres = cstring_to_text((char *) xpresstr);

	/* Free various storage */
	xmlFree(xpresstr);

	return xpres;
}

/*
 * xpath_table is a table function. It needs some tidying (as do the
 * other functions here!
 */
PG_FUNCTION_INFO_V1(xpath_table);

Datum
xpath_table(PG_FUNCTION_ARGS)
{
	/* Function parameters */
	char	   *pkeyfield = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *xmlfield = text_to_cstring(PG_GETARG_TEXT_PP(1));
	char	   *relname = text_to_cstring(PG_GETARG_TEXT_PP(2));
	char	   *xpathset = text_to_cstring(PG_GETARG_TEXT_PP(3));
	char	   *condition = text_to_cstring(PG_GETARG_TEXT_PP(4));

	/* SPI (input tuple) support */
	SPITupleTable *tuptable;
	HeapTuple	spi_tuple;
	TupleDesc	spi_tupdesc;

	/* Output tuple (tuplestore) support */
	Tuplestorestate *tupstore = NULL;
	TupleDesc	ret_tupdesc;
	HeapTuple	ret_tuple;

	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	AttInMetadata *attinmeta;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	char	  **values;
	xmlChar   **xpaths;
	char	   *pos;
	const char *pathsep = "|";

	int			numpaths;
	int			ret;
	uint64		proc;
	int			j;
	int			rownr;			/* For issuing multiple rows from one original
								 * document */
	bool		had_values;		/* To determine end of nodeset results */
	StringInfoData query_buf;
	PgXmlErrorContext *xmlerrcxt;
	volatile xmlDocPtr doctree = NULL;

	/* We only have a valid tuple description in table function mode */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (rsinfo->expectedDesc == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("xpath_table must be called as a table function")));

	/*
	 * We want to materialise because it means that we don't have to carry
	 * libxml2 parser state between invocations of this function
	 */
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("xpath_table requires Materialize mode, but it is not "
						"allowed in this context")));

	/*
	 * The tuplestore must exist in a higher context than this function call
	 * (per_query_ctx is used)
	 */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/*
	 * Create the tuplestore - work_mem is the max in-memory size before a
	 * file is created on disk to hold it.
	 */
	tupstore =
		tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	MemoryContextSwitchTo(oldcontext);

	/* get the requested return tuple description */
	ret_tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);

	/* must have at least one output column (for the pkey) */
	if (ret_tupdesc->natts < 1)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("xpath_table must have at least one output column")));

	/*
	 * At the moment we assume that the returned attributes make sense for the
	 * XPath specified (i.e. we trust the caller). It's not fatal if they get
	 * it wrong - the input function for the column type will raise an error
	 * if the path result can't be converted into the correct binary
	 * representation.
	 */

	attinmeta = TupleDescGetAttInMetadata(ret_tupdesc);

	/* Set return mode and allocate value space. */
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setDesc = ret_tupdesc;

	values = (char **) palloc(ret_tupdesc->natts * sizeof(char *));
	xpaths = (xmlChar **) palloc(ret_tupdesc->natts * sizeof(xmlChar *));

	/*
	 * Split XPaths. xpathset is a writable CString.
	 *
	 * Note that we stop splitting once we've done all needed for tupdesc
	 */
	numpaths = 0;
	pos = xpathset;
	while (numpaths < (ret_tupdesc->natts - 1))
	{
		xpaths[numpaths++] = (xmlChar *) pos;
		pos = strstr(pos, pathsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
			break;
	}

	/* Now build query */
	initStringInfo(&query_buf);

	/* Build initial sql statement */
	appendStringInfo(&query_buf, "SELECT %s, %s FROM %s WHERE %s",
					 pkeyfield,
					 xmlfield,
					 relname,
					 condition);

	if ((ret = SPI_connect()) < 0)
		elog(ERROR, "xpath_table: SPI_connect returned %d", ret);

	if ((ret = SPI_exec(query_buf.data, 0)) != SPI_OK_SELECT)
		elog(ERROR, "xpath_table: SPI execution failed for query %s",
			 query_buf.data);

	proc = SPI_processed;
	tuptable = SPI_tuptable;
	spi_tupdesc = tuptable->tupdesc;

	/* Switch out of SPI context */
	MemoryContextSwitchTo(oldcontext);

	/*
	 * Check that SPI returned correct result. If you put a comma into one of
	 * the function parameters, this will catch it when the SPI query returns
	 * e.g. 3 columns.
	 */
	if (spi_tupdesc->natts != 2)
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("expression returning multiple columns is not valid in parameter list"),
						errdetail("Expected two columns in SPI result, got %d.", spi_tupdesc->natts)));
	}

	/*
	 * Setup the parser.  This should happen after we are done evaluating the
	 * query, in case it calls functions that set up libxml differently.
	 */
	xmlerrcxt = pgxml_parser_init(PG_XML_STRICTNESS_LEGACY);

	PG_TRY();
	{
		/* For each row i.e. document returned from SPI */
		uint64		i;

		for (i = 0; i < proc; i++)
		{
			char	   *pkey;
			char	   *xmldoc;
			xmlXPathContextPtr ctxt;
			xmlXPathObjectPtr res;
			xmlChar    *resstr;
			xmlXPathCompExprPtr comppath;

			/* Extract the row data as C Strings */
			spi_tuple = tuptable->vals[i];
			pkey = SPI_getvalue(spi_tuple, spi_tupdesc, 1);
			xmldoc = SPI_getvalue(spi_tuple, spi_tupdesc, 2);

			/*
			 * Clear the values array, so that not-well-formed documents
			 * return NULL in all columns.  Note that this also means that
			 * spare columns will be NULL.
			 */
			for (j = 0; j < ret_tupdesc->natts; j++)
				values[j] = NULL;

			/* Insert primary key */
			values[0] = pkey;

			/* Parse the document */
			if (xmldoc)
				doctree = xmlReadMemory(xmldoc, strlen(xmldoc),
										NULL, NULL,
										XML_PARSE_NOENT);
			else				/* treat NULL as not well-formed */
				doctree = NULL;

			if (doctree == NULL)
			{
				/* not well-formed, so output all-NULL tuple */
				ret_tuple = BuildTupleFromCStrings(attinmeta, values);
				tuplestore_puttuple(tupstore, ret_tuple);
				heap_freetuple(ret_tuple);
			}
			else
			{
				/* New loop here - we have to deal with nodeset results */
				rownr = 0;

				do
				{
					/* Now evaluate the set of xpaths. */
					had_values = false;
					for (j = 0; j < numpaths; j++)
					{
						ctxt = xmlXPathNewContext(doctree);
						ctxt->node = xmlDocGetRootElement(doctree);

						/* compile the path */
						comppath = xmlXPathCtxtCompile(ctxt, xpaths[j]);
						if (comppath == NULL)
							xml_ereport(xmlerrcxt, ERROR,
										ERRCODE_EXTERNAL_ROUTINE_EXCEPTION,
										"XPath Syntax Error");

						/* Now evaluate the path expression. */
						res = xmlXPathCompiledEval(comppath, ctxt);
						xmlXPathFreeCompExpr(comppath);

						if (res != NULL)
						{
							switch (res->type)
							{
								case XPATH_NODESET:
									/* We see if this nodeset has enough nodes */
									if (res->nodesetval != NULL &&
										rownr < res->nodesetval->nodeNr)
									{
										resstr = xmlXPathCastNodeToString(res->nodesetval->nodeTab[rownr]);
										had_values = true;
									}
									else
										resstr = NULL;

									break;

								case XPATH_STRING:
									resstr = xmlStrdup(res->stringval);
									break;

								default:
									elog(NOTICE, "unsupported XQuery result: %d", res->type);
									resstr = xmlStrdup((const xmlChar *) "<unsupported/>");
							}

							/*
							 * Insert this into the appropriate column in the
							 * result tuple.
							 */
							values[j + 1] = (char *) resstr;
						}
						xmlXPathFreeContext(ctxt);
					}

					/* Now add the tuple to the output, if there is one. */
					if (had_values)
					{
						ret_tuple = BuildTupleFromCStrings(attinmeta, values);
						tuplestore_puttuple(tupstore, ret_tuple);
						heap_freetuple(ret_tuple);
					}

					rownr++;
				} while (had_values);
			}

			if (doctree != NULL)
				xmlFreeDoc(doctree);
			doctree = NULL;

			if (pkey)
				pfree(pkey);
			if (xmldoc)
				pfree(xmldoc);
		}
	}
	PG_CATCH();
	{
		if (doctree != NULL)
			xmlFreeDoc(doctree);

		pg_xml_done(xmlerrcxt, true);

		PG_RE_THROW();
	}
	PG_END_TRY();

	if (doctree != NULL)
		xmlFreeDoc(doctree);

	pg_xml_done(xmlerrcxt, false);

	tuplestore_donestoring(tupstore);

	SPI_finish();

	rsinfo->setResult = tupstore;

	/*
	 * SFRM_Materialize mode expects us to return a NULL Datum. The actual
	 * tuples are in our tuplestore and passed back through rsinfo->setResult.
	 * rsinfo->setDesc is set to the tuple description that we actually used
	 * to build our tuples with, so the caller can verify we did what it was
	 * expecting.
	 */
	return (Datum) 0;
}
