/********************************************************
 * Interface code to parse an XML document using expat
 ********************************************************/

#include "postgres.h"
#include "fmgr.h"

#include "expat.h"
#include "pgxml.h"

/* Memory management - we make expat use standard pg MM */

XML_Memory_Handling_Suite mhs;

/* passthrough functions (palloc is a macro) */

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

static void
pgxml_mhs_init()
{
	mhs.malloc_fcn = pgxml_palloc;
	mhs.realloc_fcn = pgxml_repalloc;
	mhs.free_fcn = pgxml_pfree;
}

static void
pgxml_handler_init()
{
	/*
	 * This code should set up the relevant handlers from  user-supplied
	 * settings. Quite how these settings are made is another matter :)
	 */
}

/* Returns true if document is well-formed */

PG_FUNCTION_INFO_V1(pgxml_parse);

Datum
pgxml_parse(PG_FUNCTION_ARGS)
{
	/* called as pgxml_parse(document) */
	XML_Parser	p;
	text	   *t = PG_GETARG_TEXT_P(0);		/* document buffer */
	int32		docsize = VARSIZE(t) - VARHDRSZ;

	pgxml_mhs_init();

	pgxml_handler_init();

	p = XML_ParserCreate_MM(NULL, &mhs, NULL);
	if (!p)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not create expat parser")));
		PG_RETURN_NULL();		/* seems appropriate if we couldn't parse */
	}

	if (!XML_Parse(p, (char *) VARDATA(t), docsize, 1))
	{
		/*
		 * elog(WARNING, "Parse error at line %d:%s",
		 * XML_GetCurrentLineNumber(p),
		 * XML_ErrorString(XML_GetErrorCode(p)));
		 */
		XML_ParserFree(p);
		PG_RETURN_BOOL(false);
	}

	XML_ParserFree(p);
	PG_RETURN_BOOL(true);
}

/* XPath handling functions */

/* XPath support here is for a very skeletal kind of XPath!
   It was easy to program though... */

/* This first is the core function that builds a result set. The
   actual functions called by the user manipulate that result set
   in various ways.
*/

static XPath_Results *
build_xpath_results(text *doc, text *pathstr)
{
	XPath_Results *xpr;
	char	   *res;
	pgxml_udata *udata;
	XML_Parser	p;
	int32		docsize;

	xpr = (XPath_Results *) palloc((sizeof(XPath_Results)));
	memset((void *) xpr, 0, sizeof(XPath_Results));
	xpr->rescount = 0;

	docsize = VARSIZE(doc) - VARHDRSZ;

	/* res isn't going to be the real return type, it is just a buffer */

	res = (char *) palloc(docsize);
	memset((void *) res, 0, docsize);

	xpr->resbuf = res;

	udata = (pgxml_udata *) palloc((sizeof(pgxml_udata)));
	memset((void *) udata, 0, sizeof(pgxml_udata));

	udata->currentpath[0] = '\0';
	udata->textgrab = 0;

	udata->path = (char *) palloc(VARSIZE(pathstr));
	memcpy(udata->path, VARDATA(pathstr), VARSIZE(pathstr) - VARHDRSZ);

	udata->path[VARSIZE(pathstr) - VARHDRSZ] = '\0';

	udata->resptr = res;
	udata->reslen = 0;

	udata->xpres = xpr;

	/* Now fire up the parser */
	pgxml_mhs_init();

	p = XML_ParserCreate_MM(NULL, &mhs, NULL);
	if (!p)
	{
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not create expat parser")));
		pfree(xpr);
		pfree(udata->path);
		pfree(udata);
		pfree(res);
		return NULL;
	}
	XML_SetUserData(p, (void *) udata);

	/* Set the handlers */

	XML_SetElementHandler(p, pgxml_starthandler, pgxml_endhandler);
	XML_SetCharacterDataHandler(p, pgxml_charhandler);

	if (!XML_Parse(p, (char *) VARDATA(doc), docsize, 1))
	{
		/*
		 * elog(WARNING, "Parse error at line %d:%s",
		 * XML_GetCurrentLineNumber(p),
		 * XML_ErrorString(XML_GetErrorCode(p)));
		 */
		XML_ParserFree(p);
		pfree(xpr);
		pfree(udata->path);
		pfree(udata);

		return NULL;
	}

	pfree(udata->path);
	pfree(udata);
	XML_ParserFree(p);
	return xpr;
}


PG_FUNCTION_INFO_V1(pgxml_xpath);

Datum
pgxml_xpath(PG_FUNCTION_ARGS)
{
	/* called as pgxml_xpath(document,pathstr, index) for the moment */

	XPath_Results *xpresults;
	text	   *restext;

	text	   *t = PG_GETARG_TEXT_P(0);		/* document buffer */
	text	   *t2 = PG_GETARG_TEXT_P(1);
	int32		ind = PG_GETARG_INT32(2) - 1;

	xpresults = build_xpath_results(t, t2);

	/*
	 * This needs to be changed depending on the mechanism for returning
	 * our set of results.
	 */

	if (xpresults == NULL)		/* parse error (not WF or parser failure) */
		PG_RETURN_NULL();

	if (ind >= (xpresults->rescount))
		PG_RETURN_NULL();

	restext = (text *) palloc(xpresults->reslens[ind] + VARHDRSZ);
	memcpy(VARDATA(restext), xpresults->results[ind], xpresults->reslens[ind]);

	VARATT_SIZEP(restext) = xpresults->reslens[ind] + VARHDRSZ;

	pfree(xpresults->resbuf);
	pfree(xpresults);

	PG_RETURN_TEXT_P(restext);
}


static void
pgxml_pathcompare(void *userData)
{
	char	   *matchpos;

	matchpos = strstr(UD->currentpath, UD->path);

	if (matchpos == NULL)
	{							/* Should we have more logic here ? */
		if (UD->textgrab)
		{
			UD->textgrab = 0;
			pgxml_finalisegrabbedtext(userData);
		}
		return;
	}

	/*
	 * OK, we have a match of some sort. Now we need to check that our
	 * match is anchored to the *end* of the string AND that it is
	 * immediately preceded by a '/'
	 */

	/*
	 * This test wouldn't work if strlen (UD->path) overran the length of
	 * the currentpath, but that's not possible because we got a match!
	 */

	if ((matchpos + strlen(UD->path))[0] == '\0')
	{
		if ((UD->path)[0] == '/')
		{
			if (matchpos == UD->currentpath)
				UD->textgrab = 1;
		}
		else
		{
			if ((matchpos - 1)[0] == '/')
				UD->textgrab = 1;
		}
	}
}

static void
pgxml_starthandler(void *userData, const XML_Char * name,
				   const XML_Char ** atts)
{

	char		sepstr[] = "/";

	if ((strlen(name) + strlen(UD->currentpath)) > MAXPATHLENGTH - 2)
		elog(WARNING, "path too long");
	else
	{
		strncat(UD->currentpath, sepstr, 1);
		strcat(UD->currentpath, name);
	}
	if (UD->textgrab)
	{
		/*
		 * Depending on user preference, should we "reconstitute" the
		 * element into the result text?
		 */
	}
	else
		pgxml_pathcompare(userData);
}

static void
pgxml_endhandler(void *userData, const XML_Char * name)
{
	/*
	 * Start by removing the current element off the end of the
	 * currentpath
	 */

	char	   *sepptr;

	sepptr = strrchr(UD->currentpath, '/');
	if (sepptr == NULL)
	{
		/* internal error */
		elog(ERROR, "did not find '/'");
		sepptr = UD->currentpath;
	}
	if (strcmp(name, sepptr + 1) != 0)
	{
		elog(WARNING, "wanted [%s], got [%s]", sepptr, name);
		/* unmatched entry, so do nothing */
	}
	else
	{
		sepptr[0] = '\0';		/* Chop that element off the end */
	}

	if (UD->textgrab)
		pgxml_pathcompare(userData);

}

static void
pgxml_charhandler(void *userData, const XML_Char * s, int len)
{
	if (UD->textgrab)
	{
		if (len > 0)
		{
			memcpy(UD->resptr, s, len);
			UD->resptr += len;
			UD->reslen += len;
		}
	}
}

/* Should I be using PG list types here? */

static void
pgxml_finalisegrabbedtext(void *userData)
{
	/* In res/reslen, we have a single result. */
	UD->xpres->results[UD->xpres->rescount] = UD->resptr - UD->reslen;
	UD->xpres->reslens[UD->xpres->rescount] = UD->reslen;
	UD->reslen = 0;
	UD->xpres->rescount++;

	/*
	 * This effectively concatenates all the results together but we do
	 * know where one ends and the next begins
	 */
}
