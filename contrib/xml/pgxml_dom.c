/* Parser interface for DOM-based parser (libxml) rather than
   stream-based SAX-type parser */

#include "postgres.h"
#include "fmgr.h"

/* libxml includes */

#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

/* declarations */

static void *pgxml_palloc(size_t size);
static void *pgxml_repalloc(void *ptr, size_t size);
static void pgxml_pfree(void *ptr);
static char *pgxml_pstrdup(const char *string);

static void pgxml_parser_init();

static xmlChar *pgxmlNodeSetToText(xmlNodeSetPtr nodeset, xmlDocPtr doc,
				   xmlChar * toptagname, xmlChar * septagname,
				   int format);

static xmlChar *pgxml_texttoxmlchar(text *textstring);


Datum		pgxml_parse(PG_FUNCTION_ARGS);
Datum		pgxml_xpath(PG_FUNCTION_ARGS);

/* memory handling passthrough functions (e.g. palloc, pstrdup are
   currently macros, and the others might become so...) */

static void *
pgxml_palloc(size_t size)
{
	return palloc(size);
}

static void *
pgxml_repalloc(void *ptr, size_t size)
{
	return repalloc(ptr, size);
}

static void
pgxml_pfree(void *ptr)
{
	return pfree(ptr);
}

static char *
pgxml_pstrdup(const char *string)
{
	return pstrdup(string);
}

static void
pgxml_parser_init()
{
	/*
	 * This code should also set parser settings from  user-supplied info.
	 * Quite how these settings are made is another matter :)
	 */

	xmlMemSetup(pgxml_pfree, pgxml_palloc, pgxml_repalloc, pgxml_pstrdup);
	xmlInitParser();

}


/* Returns true if document is well-formed */

PG_FUNCTION_INFO_V1(pgxml_parse);

Datum
pgxml_parse(PG_FUNCTION_ARGS)
{
	/* called as pgxml_parse(document) */
	xmlDocPtr	doctree;
	text	   *t = PG_GETARG_TEXT_P(0);		/* document buffer */
	int32		docsize = VARSIZE(t) - VARHDRSZ;

	pgxml_parser_init();

	doctree = xmlParseMemory((char *) VARDATA(t), docsize);
	if (doctree == NULL)
	{
		xmlCleanupParser();
		PG_RETURN_BOOL(false);	/* i.e. not well-formed */
	}
	xmlCleanupParser();
	xmlFreeDoc(doctree);
	PG_RETURN_BOOL(true);
}

static xmlChar
*
pgxmlNodeSetToText(xmlNodeSetPtr nodeset,
				   xmlDocPtr doc,
				   xmlChar * toptagname,
				   xmlChar * septagname,
				   int format)
{
	/* Function translates a nodeset into a text representation */

	/*
	 * iterates over each node in the set and calls xmlNodeDump to write
	 * it to an xmlBuffer -from which an xmlChar * string is returned.
	 */
	/* each representation is surrounded by <tagname> ... </tagname> */
	/* if format==0, add a newline between nodes?? */

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
			if ((septagname != NULL) && (xmlStrlen(septagname) > 0))
			{
				xmlBufferWriteChar(buf, "<");
				xmlBufferWriteCHAR(buf, septagname);
				xmlBufferWriteChar(buf, ">");
			}
			xmlNodeDump(buf, doc, nodeset->nodeTab[i], 1, (format == 2));

			if ((septagname != NULL) && (xmlStrlen(septagname) > 0))
			{
				xmlBufferWriteChar(buf, "</");
				xmlBufferWriteCHAR(buf, septagname);
				xmlBufferWriteChar(buf, ">");
			}
			if (format)
				xmlBufferWriteChar(buf, "\n");
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


PG_FUNCTION_INFO_V1(pgxml_xpath);

Datum
pgxml_xpath(PG_FUNCTION_ARGS)
{
	xmlDocPtr	doctree;
	xmlXPathContextPtr ctxt;
	xmlXPathObjectPtr res;
	xmlChar    *xpath,
			   *xpresstr,
			   *toptag,
			   *septag;
	xmlXPathCompExprPtr comppath;

	int32		docsize,
				ressize;
	text	   *t,
			   *xpres;

	t = PG_GETARG_TEXT_P(0);	/* document buffer */
	xpath = pgxml_texttoxmlchar(PG_GETARG_TEXT_P(1));	/* XPath expression */
	toptag = pgxml_texttoxmlchar(PG_GETARG_TEXT_P(2));
	septag = pgxml_texttoxmlchar(PG_GETARG_TEXT_P(3));

	docsize = VARSIZE(t) - VARHDRSZ;

	pgxml_parser_init();

	doctree = xmlParseMemory((char *) VARDATA(t), docsize);
	if (doctree == NULL)
	{							/* not well-formed */
		xmlCleanupParser();
		PG_RETURN_NULL();
	}

	ctxt = xmlXPathNewContext(doctree);
	ctxt->node = xmlDocGetRootElement(doctree);

	/* compile the path */
	comppath = xmlXPathCompile(xpath);
	if (comppath == NULL)
	{
		elog(WARNING, "XPath syntax error");
		xmlFreeDoc(doctree);
		pfree((void *) xpath);
		xmlCleanupParser();
		PG_RETURN_NULL();
	}

	/* Now evaluate the path expression. */
	res = xmlXPathCompiledEval(comppath, ctxt);
	xmlXPathFreeCompExpr(comppath);

	if (res == NULL)
	{
		xmlFreeDoc(doctree);
		pfree((void *) xpath);
		xmlCleanupParser();
		PG_RETURN_NULL();		/* seems appropriate */
	}
	/* now we dump this node, ?surrounding by tags? */
	/* To do this, we look first at the type */
	switch (res->type)
	{
		case XPATH_NODESET:
			xpresstr = pgxmlNodeSetToText(res->nodesetval,
										  doctree,
										  toptag, septag, 0);
			break;
		case XPATH_STRING:
			xpresstr = xmlStrdup(res->stringval);
			break;
		default:
			elog(WARNING, "Unsupported XQuery result: %d", res->type);
			xpresstr = xmlStrdup("<unsupported/>");
	}


	/* Now convert this result back to text */
	ressize = strlen(xpresstr);
	xpres = (text *) palloc(ressize + VARHDRSZ);
	memcpy(VARDATA(xpres), xpresstr, ressize);
	VARATT_SIZEP(xpres) = ressize + VARHDRSZ;

	/* Free various storage */
	xmlFreeDoc(doctree);
	pfree((void *) xpath);
	xmlFree(xpresstr);
	xmlCleanupParser();
	PG_RETURN_TEXT_P(xpres);
}
