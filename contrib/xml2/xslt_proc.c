/* XSLT processing functions (requiring libxslt) */
/* John Gray, for Torchbox 2003-04-01 */

#include "postgres.h"
#include "fmgr.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"

/* libxml includes */

#include <libxml/xpath.h>
#include <libxml/tree.h>
#include <libxml/xmlmemory.h>

/* libxslt includes */

#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>


/* declarations to come from xpath.c */
extern void elog_error(int level, char *explain, int force);
extern void pgxml_parser_init();
extern xmlChar *pgxml_texttoxmlchar(text *textstring);

/* local defs */
static void parse_params(const char **params, text *paramstr);

Datum		xslt_process(PG_FUNCTION_ARGS);


#define MAXPARAMS 20

PG_FUNCTION_INFO_V1(xslt_process);

Datum
xslt_process(PG_FUNCTION_ARGS)
{
	text	   *doct = PG_GETARG_TEXT_P(0);
	text	   *ssheet = PG_GETARG_TEXT_P(1);
	text	   *paramstr;
	const char *params[MAXPARAMS + 1];	/* +1 for the terminator */
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
		doctree = xmlParseFile(text_to_cstring(doct));

	if (doctree == NULL)
	{
		xmlCleanupParser();
		elog_error(ERROR, "error parsing XML document", 0);

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
			xmlCleanupParser();
			elog_error(ERROR, "error parsing stylesheet as XML document", 0);
			PG_RETURN_NULL();
		}

		stylesheet = xsltParseStylesheetDoc(ssdoc);
	}
	else
		stylesheet = xsltParseStylesheetFile((xmlChar *) text_to_cstring(ssheet));


	if (stylesheet == NULL)
	{
		xmlFreeDoc(doctree);
		xsltCleanupGlobals();
		xmlCleanupParser();
		elog_error(ERROR, "failed to parse stylesheet", 0);
		PG_RETURN_NULL();
	}

	restree = xsltApplyStylesheet(stylesheet, doctree, params);
	resstat = xsltSaveResultToString(&resstr, &reslen, restree, stylesheet);

	xsltFreeStylesheet(stylesheet);
	xmlFreeDoc(restree);
	xmlFreeDoc(doctree);

	xsltCleanupGlobals();
	xmlCleanupParser();

	if (resstat < 0)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(cstring_to_text_with_len(resstr, reslen));
}


void
parse_params(const char **params, text *paramstr)
{
	char	   *pos;
	char	   *pstr;

	int			i;
	char	   *nvsep = "=";
	char	   *itsep = ",";

	pstr = text_to_cstring(paramstr);

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
			params[i] = NULL;
			break;
		}
		/* Value */
		i++;
		params[i] = pos;
		pos = strstr(pos, itsep);
		if (pos != NULL)
		{
			*pos = '\0';
			pos++;
		}
		else
			break;

	}
	if (i < MAXPARAMS)
		params[i + 1] = NULL;
}
