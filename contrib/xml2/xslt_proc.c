/* XSLT processing functions (requiring libxslt) */
/* John Gray, for Torchbox 2003-04-01 */

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"

#ifdef USE_LIBXSLT

/* libxml includes */

#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

/* libxslt includes */

#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>

#endif /* USE_LIBXSLT */


/* externally accessible functions */

Datum		xslt_process(PG_FUNCTION_ARGS);

#ifdef USE_LIBXSLT

/* declarations to come from xpath.c */
extern void elog_error(const char *explain, bool force);
extern void pgxml_parser_init(void);

/* local defs */
static void parse_params(const char **params, text *paramstr);

#define MAXPARAMS 20			/* must be even, see parse_params() */

#define GET_STR(textp) DatumGetCString(DirectFunctionCall1(textout, PointerGetDatum(textp)))

#endif /* USE_LIBXSLT */


PG_FUNCTION_INFO_V1(xslt_process);

Datum
xslt_process(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXSLT

	const char *params[MAXPARAMS + 1];	/* +1 for the terminator */
	xsltStylesheetPtr stylesheet = NULL;
	xmlDocPtr	doctree;
	xmlDocPtr	restree;
	xmlDocPtr	ssdoc = NULL;
	xmlChar    *resstr;
	int			resstat;
	int			reslen;

	text	   *doct = PG_GETARG_TEXT_P(0);
	text	   *ssheet = PG_GETARG_TEXT_P(1);
	text	   *paramstr;
	text	   *tres;

	if (fcinfo->nargs == 3)
	{
		paramstr = PG_GETARG_TEXT_P(2);
		parse_params(params, paramstr);
	}
	else
		/* No parameters */
		params[0] = NULL;

	/* Setup parser */
	pgxml_parser_init();

	/* Check to see if document is a file or a literal */

	if (VARDATA(doct)[0] == '<')
		doctree = xmlParseMemory((char *) VARDATA(doct), VARSIZE(doct) - VARHDRSZ);
	else
		doctree = xmlParseFile(GET_STR(doct));

	if (doctree == NULL)
	{
		elog_error("error parsing XML document", false);

		PG_RETURN_NULL();
	}

	/* Same for stylesheet */
	if (VARDATA(ssheet)[0] == '<')
	{
		ssdoc = xmlParseMemory((char *) VARDATA(ssheet),
							   VARSIZE(ssheet) - VARHDRSZ);
		if (ssdoc == NULL)
		{
			xmlFreeDoc(doctree);
			elog_error("error parsing stylesheet as XML document", false);
			PG_RETURN_NULL();
		}

		stylesheet = xsltParseStylesheetDoc(ssdoc);
	}
	else
		stylesheet = xsltParseStylesheetFile((xmlChar *) GET_STR(ssheet));


	if (stylesheet == NULL)
	{
		xmlFreeDoc(doctree);
		xsltCleanupGlobals();
		elog_error("failed to parse stylesheet", false);
		PG_RETURN_NULL();
	}

	restree = xsltApplyStylesheet(stylesheet, doctree, params);
	resstat = xsltSaveResultToString(&resstr, &reslen, restree, stylesheet);

	xsltFreeStylesheet(stylesheet);
	xmlFreeDoc(restree);
	xmlFreeDoc(doctree);

	xsltCleanupGlobals();

	if (resstat < 0)
		PG_RETURN_NULL();

	tres = palloc(reslen + VARHDRSZ);
	memcpy(VARDATA(tres), resstr, reslen);
	SET_VARSIZE(tres, reslen + VARHDRSZ);

	PG_RETURN_TEXT_P(tres);

#else /* !USE_LIBXSLT */

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("xslt_process() is not available without libxslt")));
	PG_RETURN_NULL();

#endif /* USE_LIBXSLT */
}

#ifdef USE_LIBXSLT

static void
parse_params(const char **params, text *paramstr)
{
	char	   *pos;
	char	   *pstr;
	int			i;
	char	   *nvsep = "=";
	char	   *itsep = ",";

	pstr = GET_STR(paramstr);

	pos = pstr;

	for (i = 0; i < MAXPARAMS; i++)
	{
		params[i] = pos;
		pos = strstr(pos, nvsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
		{
			/* No equal sign, so ignore this "parameter" */
			/* We'll reset params[i] to NULL below the loop */
			break;
		}
		/* Value */
		i++;
		/* since MAXPARAMS is even, we still have i < MAXPARAMS */
		params[i] = pos;
		pos = strstr(pos, itsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
		{
			i++;
			break;
		}
	}

	params[i] = NULL;
}

#endif /* USE_LIBXSLT */
