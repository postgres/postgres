/* Header for pg xml parser interface */

static void *pgxml_palloc(size_t size);
static void *pgxml_repalloc(void *ptr, size_t size);
static void pgxml_pfree(void *ptr);
static void pgxml_mhs_init();
static void pgxml_handler_init();
Datum		pgxml_parse(PG_FUNCTION_ARGS);
Datum		pgxml_xpath(PG_FUNCTION_ARGS);
static void pgxml_starthandler(void *userData, const XML_Char * name,
				   const XML_Char ** atts);
static void pgxml_endhandler(void *userData, const XML_Char * name);
static void pgxml_charhandler(void *userData, const XML_Char * s, int len);
static void pgxml_pathcompare(void *userData);
static void pgxml_finalisegrabbedtext(void *userData);

#define MAXPATHLENGTH 512
#define MAXRESULTS 100


typedef struct
{
	int			rescount;
	char	   *results[MAXRESULTS];
	int32		reslens[MAXRESULTS];
	char	   *resbuf;			/* pointer to the result buffer for pfree */
}	XPath_Results;



typedef struct
{
	char		currentpath[MAXPATHLENGTH];
	char	   *path;
	int			textgrab;
	char	   *resptr;
	int32		reslen;
	XPath_Results *xpres;
}	pgxml_udata;


#define UD ((pgxml_udata *) userData)
