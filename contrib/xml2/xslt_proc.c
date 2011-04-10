/*
 * contrib/xml2/xslt_proc.c
 *
 * XSLT processing functions (requiring libxslt)
 *
 * John Gray, for Torchbox 2003-04-01
 */
#include "postgres.h"

#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/xml.h"

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
#endif   /* USE_LIBXSLT */


/* externally accessible functions */

Datum		xslt_process(PG_FUNCTION_ARGS);

#ifdef USE_LIBXSLT

/* declarations to come from xpath.c */
extern void pgxml_parser_init(void);

/* local defs */
static const char **parse_params(text *paramstr);
#endif   /* USE_LIBXSLT */


PG_FUNCTION_INFO_V1(xslt_process);

Datum
xslt_process(PG_FUNCTION_ARGS)
{
#ifdef USE_LIBXSLT

	text	   *doct = PG_GETARG_TEXT_P(0);
	text	   *ssheet = PG_GETARG_TEXT_P(1);
	text	   *paramstr;
	const char **params;
	xsltStylesheetPtr stylesheet = NULL;
	xmlDocPtr	doctree;
	xmlDocPtr	restree;
	xmlDocPtr	ssdoc = NULL;
	xmlChar    *resstr;
	int			resstat;
	int			reslen;

	if (fcinfo->nargs == 3)
	{
		paramstr = PG_GETARG_TEXT_P(2);
		params = parse_params(paramstr);
	}
	else
	{
		/* No parameters */
		params = (const char **) palloc(sizeof(char *));
		params[0] = NULL;
	}

	/* Setup parser */
	pgxml_parser_init();

	/* Check to see if document is a file or a literal */

	if (VARDATA(doct)[0] == '<')
		doctree = xmlParseMemory((char *) VARDATA(doct), VARSIZE(doct) - VARHDRSZ);
	else
		doctree = xmlParseFile(text_to_cstring(doct));

	if (doctree == NULL)
		xml_ereport(ERROR, ERRCODE_EXTERNAL_ROUTINE_EXCEPTION,
					"error parsing XML document");

	/* Same for stylesheet */
	if (VARDATA(ssheet)[0] == '<')
	{
		ssdoc = xmlParseMemory((char *) VARDATA(ssheet),
							   VARSIZE(ssheet) - VARHDRSZ);
		if (ssdoc == NULL)
		{
			xmlFreeDoc(doctree);
			xml_ereport(ERROR, ERRCODE_EXTERNAL_ROUTINE_EXCEPTION,
						"error parsing stylesheet as XML document");
		}

		stylesheet = xsltParseStylesheetDoc(ssdoc);
	}
	else
		stylesheet = xsltParseStylesheetFile((xmlChar *) text_to_cstring(ssheet));


	if (stylesheet == NULL)
	{
		xmlFreeDoc(doctree);
		xsltCleanupGlobals();
		xml_ereport(ERROR, ERRCODE_EXTERNAL_ROUTINE_EXCEPTION,
					"failed to parse stylesheet");
	}

	restree = xsltApplyStylesheet(stylesheet, doctree, params);
	resstat = xsltSaveResultToString(&resstr, &reslen, restree, stylesheet);

	xsltFreeStylesheet(stylesheet);
	xmlFreeDoc(restree);
	xmlFreeDoc(doctree);

	xsltCleanupGlobals();

	if (resstat < 0)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text_with_len((char *) resstr, reslen));
#else							/* !USE_LIBXSLT */

	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("xslt_process() is not available without libxslt")));
	PG_RETURN_NULL();
#endif   /* USE_LIBXSLT */
}

#ifdef USE_LIBXSLT

static const char **
parse_params(text *paramstr)
{
	char	   *pos;
	char	   *pstr;
	char	   *nvsep = "=";
	char	   *itsep = ",";
	const char **params;
	int			max_params;
	int			nparams;

	pstr = text_to_cstring(paramstr);

	max_params = 20;			/* must be even! */
	params = (const char **) palloc((max_params + 1) * sizeof(char *));
	nparams = 0;

	pos = pstr;

	while (*pos != '\0')
	{
		if (nparams >= max_params)
		{
			max_params *= 2;
			params = (const char **) repalloc(params,
										  (max_params + 1) * sizeof(char *));
		}
		params[nparams++] = pos;
		pos = strstr(pos, nvsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
		{
			/* No equal sign, so ignore this "parameter" */
			nparams--;
			break;
		}

		/* since max_params is even, we still have nparams < max_params */
		params[nparams++] = pos;
		pos = strstr(pos, itsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
			break;
	}

	/* Add the terminator marker; we left room for it in the palloc's */
	params[nparams] = NULL;

	return params;
}

#endif   /* USE_LIBXSLT */
