/* Parser interface for DOM-based parser (libxml) rather than
   stream-based SAX-type parser */

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"

/* libxml includes */

#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlerror.h>
#include <libxml/parserInternals.h>


PG_MODULE_MAGIC;

/* externally accessible functions */

Datum		xml_is_well_formed(PG_FUNCTION_ARGS);
Datum		xml_encode_special_chars(PG_FUNCTION_ARGS);
Datum		xpath_nodeset(PG_FUNCTION_ARGS);
Datum		xpath_string(PG_FUNCTION_ARGS);
Datum		xpath_number(PG_FUNCTION_ARGS);
Datum		xpath_bool(PG_FUNCTION_ARGS);
Datum		xpath_list(PG_FUNCTION_ARGS);
Datum		xpath_table(PG_FUNCTION_ARGS);

/* these are exported for use by xslt_proc.c */

void		elog_error(const char *explain, bool force);
void		pgxml_parser_init(void);

/* local declarations */

static void pgxml_errorHandler(void *ctxt, const char *msg,...);

static xmlChar *pgxmlNodeSetToText(xmlNodeSetPtr nodeset,
				   xmlChar *toptagname, xmlChar *septagname,
				   xmlChar *plainsep);

static text *pgxml_result_to_text(xmlXPathObjectPtr res, xmlChar *toptag,
					 xmlChar *septag, xmlChar *plainsep);

static xmlChar *pgxml_texttoxmlchar(text *textstring);

static xmlXPathObjectPtr pgxml_xpath(text *document, xmlChar *xpath);

/* Global variables */
static char *pgxml_errorMsg = NULL;		/* overall error message */

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))


/*
 * The error handling function. This formats an error message and sets
 * a flag - an ereport will be issued prior to return
 */
static void
pgxml_errorHandler(void *ctxt, const char *msg,...)
{
	char		errbuf[1024];				/* per line error buffer */
	va_list		args;

	/* Format the message */
	va_start(args, msg);
	vsnprintf(errbuf, sizeof(errbuf), msg, args);
	va_end(args);
	/* Store in, or append to, pgxml_errorMsg */
	if (pgxml_errorMsg == NULL)
		pgxml_errorMsg = pstrdup(errbuf);
	else
	{
		size_t	oldsize = strlen(pgxml_errorMsg);
		size_t	newsize = strlen(errbuf);

		/*
		 * We intentionally discard the last char of the existing message,
		 * which should be a carriage return.  (XXX wouldn't it be saner
		 * to keep it?)
		 */
		pgxml_errorMsg = repalloc(pgxml_errorMsg, oldsize + newsize);
		memcpy(&pgxml_errorMsg[oldsize - 1], errbuf, newsize);
		pgxml_errorMsg[oldsize + newsize - 1] = '\0';
	}
}

/*
 * This function ereports the current message if any.  If force is true
 * then an error is thrown even if pgxml_errorMsg hasn't been set.
 */
void
elog_error(const char *explain, bool force)
{
	if (force || pgxml_errorMsg != NULL)
	{
		if (pgxml_errorMsg == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("%s", explain)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("%s: %s", explain, pgxml_errorMsg)));
	}
}

/*
 * Initialize for xml parsing.
 */
void
pgxml_parser_init(void)
{
	/* Set up error handling */
	pgxml_errorMsg = NULL;
	xmlSetGenericErrorFunc(NULL, pgxml_errorHandler);

	/* Initialize libxml */
	xmlInitParser();

	xmlSubstituteEntitiesDefault(1);
	xmlLoadExtDtdDefaultValue = 1;
}


/* Returns true if document is well-formed */

PG_FUNCTION_INFO_V1(xml_is_well_formed);

Datum
xml_is_well_formed(PG_FUNCTION_ARGS)
{
	text	   *t = PG_GETARG_TEXT_P(0);		/* document buffer */
	int32		docsize = VARSIZE(t) - VARHDRSZ;
	xmlDocPtr	doctree;

	pgxml_parser_init();

	doctree = xmlParseMemory((char *) VARDATA(t), docsize);
	if (doctree == NULL)
	{
		xmlCleanupParser();
		PG_RETURN_BOOL(false);	/* i.e. not well-formed */
	}
	xmlFreeDoc(doctree);
	xmlCleanupParser();
	PG_RETURN_BOOL(true);
}


/* Encodes special characters (<, >, &, " and \r) as XML entities */

PG_FUNCTION_INFO_V1(xml_encode_special_chars);

Datum
xml_encode_special_chars(PG_FUNCTION_ARGS)
{
	text	   *tin = PG_GETARG_TEXT_P(0);
	text	   *tout;
	int32		ressize;
	xmlChar    *ts,
			   *tt;

	ts = pgxml_texttoxmlchar(tin);

	tt = xmlEncodeSpecialChars(NULL, ts);

	pfree(ts);

	ressize = strlen((char *) tt);
	tout = (text *) palloc(ressize + VARHDRSZ);
	memcpy(VARDATA(tout), tt, ressize);
	SET_VARSIZE(tout, ressize + VARHDRSZ);

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
	xmlChar    *res;
	int32		txsize;

	txsize = VARSIZE(textstring) - VARHDRSZ;
	res = (xmlChar *) palloc(txsize + 1);
	memcpy((char *) res, VARDATA(textstring), txsize);
	res[txsize] = '\0';
	return res;
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
	xmlChar    *xpath,
			   *toptag,
			   *septag;
	int32		pathsize;
	text	   *xpathsupp,
			   *xpres;

	/* PG_GETARG_TEXT_P(0) is document buffer */
	xpathsupp = PG_GETARG_TEXT_P(1);	/* XPath expression */

	toptag = pgxml_texttoxmlchar(PG_GETARG_TEXT_P(2));
	septag = pgxml_texttoxmlchar(PG_GETARG_TEXT_P(3));

	pathsize = VARSIZE(xpathsupp) - VARHDRSZ;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	xpres = pgxml_result_to_text(pgxml_xpath(PG_GETARG_TEXT_P(0), xpath),
								 toptag, septag, NULL);

	/* xmlCleanupParser(); done by result_to_text routine */
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
	xmlChar    *xpath,
			   *plainsep;
	int32		pathsize;
	text	   *xpathsupp,
			   *xpres;

	/* PG_GETARG_TEXT_P(0) is document buffer */
	xpathsupp = PG_GETARG_TEXT_P(1);	/* XPath expression */

	plainsep = pgxml_texttoxmlchar(PG_GETARG_TEXT_P(2));

	pathsize = VARSIZE(xpathsupp) - VARHDRSZ;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	xpres = pgxml_result_to_text(pgxml_xpath(PG_GETARG_TEXT_P(0), xpath),
								 NULL, NULL, plainsep);

	/* xmlCleanupParser(); done by result_to_text routine */
	pfree(xpath);

	if (xpres == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(xpres);
}


PG_FUNCTION_INFO_V1(xpath_string);

Datum
xpath_string(PG_FUNCTION_ARGS)
{
	xmlChar    *xpath;
	int32		pathsize;
	text	   *xpathsupp,
			   *xpres;

	/* PG_GETARG_TEXT_P(0) is document buffer */
	xpathsupp = PG_GETARG_TEXT_P(1);	/* XPath expression */

	pathsize = VARSIZE(xpathsupp) - VARHDRSZ;

	/*
	 * We encapsulate the supplied path with "string()" = 8 chars + 1 for NUL
	 * at end
	 */
	/* We could try casting to string using the libxml function? */

	xpath = (xmlChar *) palloc(pathsize + 9);
	memcpy((char *) (xpath + 7), VARDATA(xpathsupp), pathsize);
	strncpy((char *) xpath, "string(", 7);
	xpath[pathsize + 7] = ')';
	xpath[pathsize + 8] = '\0';

	xpres = pgxml_result_to_text(pgxml_xpath(PG_GETARG_TEXT_P(0), xpath),
								 NULL, NULL, NULL);

	xmlCleanupParser();
	pfree(xpath);

	if (xpres == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(xpres);
}


PG_FUNCTION_INFO_V1(xpath_number);

Datum
xpath_number(PG_FUNCTION_ARGS)
{
	xmlChar    *xpath;
	int32		pathsize;
	text	   *xpathsupp;
	float4		fRes;

	xmlXPathObjectPtr res;

	/* PG_GETARG_TEXT_P(0) is document buffer */
	xpathsupp = PG_GETARG_TEXT_P(1);	/* XPath expression */

	pathsize = VARSIZE(xpathsupp) - VARHDRSZ;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(PG_GETARG_TEXT_P(0), xpath);
	pfree(xpath);

	if (res == NULL)
	{
		xmlCleanupParser();
		PG_RETURN_NULL();
	}

	fRes = xmlXPathCastToNumber(res);
	xmlCleanupParser();
	if (xmlXPathIsNaN(fRes))
		PG_RETURN_NULL();

	PG_RETURN_FLOAT4(fRes);
}


PG_FUNCTION_INFO_V1(xpath_bool);

Datum
xpath_bool(PG_FUNCTION_ARGS)
{
	xmlChar    *xpath;
	int32		pathsize;
	text	   *xpathsupp;
	int			bRes;

	xmlXPathObjectPtr res;

	/* PG_GETARG_TEXT_P(0) is document buffer */
	xpathsupp = PG_GETARG_TEXT_P(1);	/* XPath expression */

	pathsize = VARSIZE(xpathsupp) - VARHDRSZ;

	xpath = pgxml_texttoxmlchar(xpathsupp);

	res = pgxml_xpath(PG_GETARG_TEXT_P(0), xpath);
	pfree(xpath);

	if (res == NULL)
	{
		xmlCleanupParser();
		PG_RETURN_BOOL(false);
	}

	bRes = xmlXPathCastToBoolean(res);
	xmlCleanupParser();
	PG_RETURN_BOOL(bRes);
}



/* Core function to evaluate XPath query */

static xmlXPathObjectPtr
pgxml_xpath(text *document, xmlChar *xpath)
{
	xmlDocPtr	doctree;
	xmlXPathContextPtr ctxt;
	xmlXPathObjectPtr res;
	xmlXPathCompExprPtr comppath;
	int32		docsize;

	docsize = VARSIZE(document) - VARHDRSZ;

	pgxml_parser_init();

	doctree = xmlParseMemory((char *) VARDATA(document), docsize);
	if (doctree == NULL)
	{							/* not well-formed */
		return NULL;
	}

	ctxt = xmlXPathNewContext(doctree);
	ctxt->node = xmlDocGetRootElement(doctree);

	/* compile the path */
	comppath = xmlXPathCompile(xpath);
	if (comppath == NULL)
	{
		xmlCleanupParser();
		xmlFreeDoc(doctree);
		elog_error("XPath Syntax Error", true);
	}

	/* Now evaluate the path expression. */
	res = xmlXPathCompiledEval(comppath, ctxt);
	xmlXPathFreeCompExpr(comppath);

	if (res == NULL)
	{
		xmlXPathFreeContext(ctxt);
		/* xmlCleanupParser(); */
		xmlFreeDoc(doctree);

		return NULL;
	}
	/* xmlFreeDoc(doctree); */
	return res;
}

static text *
pgxml_result_to_text(xmlXPathObjectPtr res,
					 xmlChar *toptag,
					 xmlChar *septag,
					 xmlChar *plainsep)
{
	xmlChar    *xpresstr;
	int32		ressize;
	text	   *xpres;

	if (res == NULL)
	{
		xmlCleanupParser();
		return NULL;
	}
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
	ressize = strlen((char *) xpresstr);
	xpres = (text *) palloc(ressize + VARHDRSZ);
	memcpy(VARDATA(xpres), xpresstr, ressize);
	SET_VARSIZE(xpres, ressize + VARHDRSZ);

	/* Free various storage */
	xmlCleanupParser();
	/* xmlFreeDoc(doctree);  -- will die at end of tuple anyway */

	xmlFree(xpresstr);

	elog_error("XPath error", false);

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
	char	   *pkeyfield = GET_STR(PG_GETARG_TEXT_P(0));
	char	   *xmlfield = GET_STR(PG_GETARG_TEXT_P(1));
	char	   *relname = GET_STR(PG_GETARG_TEXT_P(2));
	char	   *xpathset = GET_STR(PG_GETARG_TEXT_P(3));
	char	   *condition = GET_STR(PG_GETARG_TEXT_P(4));

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
	int			proc;
	int			i;
	int			j;
	int			rownr;			/* For issuing multiple rows from one original
								 * document */
	bool		had_values;		/* To determine end of nodeset results */
	StringInfoData query_buf;

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
	tupstore = tuplestore_begin_heap(true, false, work_mem);

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
	 * XPath specififed (i.e. we trust the caller). It's not fatal if they get
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
	/* elog(DEBUG1,"xpath_table: SPI returned %d rows",proc); */
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
	 * Setup the parser. Beware that this must happen in the same context as
	 * the cleanup - which means that any error from here on must do cleanup
	 * to ensure that the entity table doesn't get freed by being out of
	 * context.
	 */
	pgxml_parser_init();

	/* For each row i.e. document returned from SPI */
	for (i = 0; i < proc; i++)
	{
		char	   *pkey;
		char	   *xmldoc;
		xmlDocPtr	doctree;
		xmlXPathContextPtr ctxt;
		xmlXPathObjectPtr res;
		xmlChar    *resstr;
		xmlXPathCompExprPtr comppath;

		/* Extract the row data as C Strings */
		spi_tuple = tuptable->vals[i];
		pkey = SPI_getvalue(spi_tuple, spi_tupdesc, 1);
		xmldoc = SPI_getvalue(spi_tuple, spi_tupdesc, 2);

		/*
		 * Clear the values array, so that not-well-formed documents return
		 * NULL in all columns.  Note that this also means that spare columns
		 * will be NULL.
		 */
		for (j = 0; j < ret_tupdesc->natts; j++)
			values[j] = NULL;

		/* Insert primary key */
		values[0] = pkey;

		/* Parse the document */
		if (xmldoc)
			doctree = xmlParseMemory(xmldoc, strlen(xmldoc));
		else					/* treat NULL as not well-formed */
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
					xmlSetGenericErrorFunc(ctxt, pgxml_errorHandler);

					/* compile the path */
					comppath = xmlXPathCompile(xpaths[j]);
					if (comppath == NULL)
					{
						xmlCleanupParser();
						xmlFreeDoc(doctree);
						elog_error("XPath Syntax Error", true);
					}

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
									resstr =
										xmlXPathCastNodeToString(res->nodesetval->nodeTab[rownr]);
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

		xmlFreeDoc(doctree);

		if (pkey)
			pfree(pkey);
		if (xmldoc)
			pfree(xmldoc);
	}

	xmlCleanupParser();

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
