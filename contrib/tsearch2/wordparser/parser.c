/* $PostgreSQL: pgsql/contrib/tsearch2/wordparser/parser.c,v 1.11.2.2 2007/03/22 15:59:09 teodor Exp $ */

#include "postgres.h"

#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "mb/pg_wchar.h"

#include "deflex.h"
#include "parser.h"
#include "ts_locale.h"


static TParserPosition *
newTParserPosition(TParserPosition * prev)
{
	TParserPosition *res = (TParserPosition *) palloc(sizeof(TParserPosition));

	if (prev)
		memcpy(res, prev, sizeof(TParserPosition));
	else
		memset(res, 0, sizeof(TParserPosition));

	res->prev = prev;

	res->pushedAtAction = NULL;

	return res;
}

TParser *
TParserInit(char *str, int len)
{
	TParser    *prs = (TParser *) palloc0(sizeof(TParser));

	prs->charmaxlen = pg_database_encoding_max_length();
	prs->str = str;
	prs->lenstr = len;

#ifdef TS_USE_WIDE

	/*
	 * Use wide char code only when max encoding length > 1.
	 */

	if (prs->charmaxlen > 1)
	{
		prs->usewide = true;
		prs->wstr = (wchar_t *) palloc(sizeof(wchar_t) * (prs->lenstr+1));
		prs->lenwstr = char2wchar(prs->wstr, prs->str, prs->lenstr);
	}
	else
#endif
		prs->usewide = false;

	prs->state = newTParserPosition(NULL);
	prs->state->state = TPS_Base;

	return prs;
}

void
TParserClose(TParser * prs)
{
	while (prs->state)
	{
		TParserPosition *ptr = prs->state->prev;

		pfree(prs->state);
		prs->state = ptr;
	}

#ifdef TS_USE_WIDE
	if (prs->wstr)
		pfree(prs->wstr);
#endif

	pfree(prs);
}

/*
 * defining support function, equvalent is* macroses, but
 * working with any possible encodings and locales. Note,
 * that with multibyte encoding and C-locale isw* function may fail
 * or give wrong result. Note 2: multibyte encoding and C-locale 
 * often are used for Asian languages.
 */

#ifdef TS_USE_WIDE

#define p_iswhat(type)														\
static int																	\
p_is##type(TParser *prs) {													\
	Assert( prs->state );													\
	if ( prs->usewide )														\
	{																		\
		if ( lc_ctype_is_c() )												\
			return is##type( 0xff & *( prs->wstr + prs->state->poschar) );	\
																			\
		return isw##type( *(wint_t*)( prs->wstr + prs->state->poschar ) );	\
	}																		\
																			\
	return is##type( *(unsigned char*)( prs->str + prs->state->posbyte ) );	\
}																			\
																			\
static int																	\
p_isnot##type(TParser *prs) {												\
	return !p_is##type(prs);												\
}

static int 
p_isalnum(TParser *prs)
{
	Assert( prs->state );

	if (prs->usewide)
	{
		if (lc_ctype_is_c())
		{
			unsigned int c = *(prs->wstr + prs->state->poschar);

			/*
			 * any non-ascii symbol with multibyte encoding
			 * with C-locale is an alpha character
			 */
			if ( c > 0x7f )
				return 1;

			return isalnum(0xff & c);
		}

		return iswalnum( (wint_t)*( prs->wstr + prs->state->poschar));
	}

	return isalnum( *(unsigned char*)( prs->str + prs->state->posbyte ));
}

static int
p_isnotalnum(TParser *prs)
{
	return !p_isalnum(prs);
}

static int 
p_isalpha(TParser *prs)
{
	Assert( prs->state );

	if (prs->usewide)
	{
		if (lc_ctype_is_c())
		{
			unsigned int c = *(prs->wstr + prs->state->poschar);

			/*
			 * any non-ascii symbol with multibyte encoding
			 * with C-locale is an alpha character
			 */
			if ( c > 0x7f )
				return 1;

			return isalpha(0xff & c);
		}

		return iswalpha( (wint_t)*( prs->wstr + prs->state->poschar));
	}

	return isalpha( *(unsigned char*)( prs->str + prs->state->posbyte ));
}

static int
p_isnotalpha(TParser *prs)
{
	return !p_isalpha(prs);
}

/* p_iseq should be used only for ascii symbols */

static int
p_iseq(TParser * prs, char c)
{
	Assert(prs->state);
	return ((prs->state->charlen == 1 && *(prs->str + prs->state->posbyte) == c)) ? 1 : 0;
}

#else							/* TS_USE_WIDE */

#define p_iswhat(type)														\
static int																	\
p_is##type(TParser *prs) {													\
	Assert( prs->state );													\
	return is##type( (unsigned char)*( prs->str + prs->state->posbyte ) );	\
}																			\
																			\
static int																	\
p_isnot##type(TParser *prs) {												\
	return !p_is##type(prs);												\
}


static int
p_iseq(TParser * prs, char c)
{
	Assert(prs->state);
	return (*(prs->str + prs->state->posbyte) == c) ? 1 : 0;
}

p_iswhat(alnum)
p_iswhat(alpha)

#endif   /* TS_USE_WIDE */

p_iswhat(digit)
p_iswhat(lower)
p_iswhat(print)
p_iswhat(punct)
p_iswhat(space)
p_iswhat(upper)
p_iswhat(xdigit)

static int
p_isEOF(TParser * prs)
{
	Assert(prs->state);
	return (prs->state->posbyte == prs->lenstr || prs->state->charlen == 0) ? 1 : 0;
}

static int
p_iseqC(TParser * prs)
{
	return p_iseq(prs, prs->c);
}

static int
p_isneC(TParser * prs)
{
	return !p_iseq(prs, prs->c);
}

static int
p_isascii(TParser * prs)
{
	return (prs->state->charlen == 1 && isascii((unsigned char) *(prs->str + prs->state->posbyte))) ? 1 : 0;
}

static int
p_islatin(TParser * prs)
{
	return (p_isalpha(prs) && p_isascii(prs)) ? 1 : 0;
}

static int
p_isnonlatin(TParser * prs)
{
	return (p_isalpha(prs) && !p_isascii(prs)) ? 1 : 0;
}

void		_make_compiler_happy(void);
void
_make_compiler_happy(void)
{
	p_isalnum(NULL);
	p_isnotalnum(NULL);
	p_isalpha(NULL);
	p_isnotalpha(NULL);
	p_isdigit(NULL);
	p_isnotdigit(NULL);
	p_islower(NULL);
	p_isnotlower(NULL);
	p_isprint(NULL);
	p_isnotprint(NULL);
	p_ispunct(NULL);
	p_isnotpunct(NULL);
	p_isspace(NULL);
	p_isnotspace(NULL);
	p_isupper(NULL);
	p_isnotupper(NULL);
	p_isxdigit(NULL);
	p_isnotxdigit(NULL);
	p_isEOF(NULL);
	p_iseqC(NULL);
	p_isneC(NULL);
}


static void
SpecialTags(TParser * prs)
{
	switch (prs->state->lencharlexeme)
	{
		case 8:			/* </script */
			if (pg_strncasecmp(prs->lexeme, "</script", 8) == 0)
				prs->ignore = false;
			break;
		case 7:			/* <script || </style */
			if (pg_strncasecmp(prs->lexeme, "</style", 7) == 0)
				prs->ignore = false;
			else if (pg_strncasecmp(prs->lexeme, "<script", 7) == 0)
				prs->ignore = true;
			break;
		case 6:			/* <style */
			if (pg_strncasecmp(prs->lexeme, "<style", 6) == 0)
				prs->ignore = true;
			break;
		default:
			break;
	}
}

static void
SpecialFURL(TParser * prs)
{
	prs->wanthost = true;
	prs->state->posbyte -= prs->state->lenbytelexeme;
	prs->state->poschar -= prs->state->lencharlexeme;
}

static void
SpecialHyphen(TParser * prs)
{
	prs->state->posbyte -= prs->state->lenbytelexeme;
	prs->state->poschar -= prs->state->lencharlexeme;
}

static void
SpecialVerVersion(TParser * prs)
{
	prs->state->posbyte -= prs->state->lenbytelexeme;
	prs->state->poschar -= prs->state->lencharlexeme;
	prs->state->lenbytelexeme = 0;
	prs->state->lencharlexeme = 0;
}

static int
p_isstophost(TParser * prs)
{
	if (prs->wanthost)
	{
		prs->wanthost = false;
		return 1;
	}
	return 0;
}

static int
p_isignore(TParser * prs)
{
	return (prs->ignore) ? 1 : 0;
}

static int
p_ishost(TParser * prs)
{
	TParser    *tmpprs = TParserInit(prs->str + prs->state->posbyte, prs->lenstr - prs->state->posbyte);
	int			res = 0;

	if (TParserGet(tmpprs) && tmpprs->type == HOST)
	{
		prs->state->posbyte += tmpprs->lenbytelexeme;
		prs->state->poschar += tmpprs->lencharlexeme;
		prs->state->lenbytelexeme += tmpprs->lenbytelexeme;
		prs->state->lencharlexeme += tmpprs->lencharlexeme;
		prs->state->charlen = tmpprs->state->charlen;
		res = 1;
	}
	TParserClose(tmpprs);

	return res;
}

static int
p_isURI(TParser * prs)
{
	TParser    *tmpprs = TParserInit(prs->str + prs->state->posbyte, prs->lenstr - prs->state->posbyte);
	int			res = 0;

	tmpprs->state = newTParserPosition(tmpprs->state);
	tmpprs->state->state = TPS_InFileFirst;

	if (TParserGet(tmpprs) && (tmpprs->type == URI || tmpprs->type == FILEPATH))
	{
		prs->state->posbyte += tmpprs->lenbytelexeme;
		prs->state->poschar += tmpprs->lencharlexeme;
		prs->state->lenbytelexeme += tmpprs->lenbytelexeme;
		prs->state->lencharlexeme += tmpprs->lencharlexeme;
		prs->state->charlen = tmpprs->state->charlen;
		res = 1;
	}
	TParserClose(tmpprs);

	return res;
}

/*
 * Table of state/action of parser
 */

#define A_NEXT		0x0000
#define A_BINGO		0x0001
#define A_POP		0x0002
#define A_PUSH		0x0004
#define A_RERUN		0x0008
#define A_CLEAR		0x0010
#define A_MERGE		0x0020
#define A_CLRALL	0x0040

static TParserStateActionItem actionTPS_Base[] = {
	{p_isEOF, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '<', A_PUSH, TPS_InTagFirst, 0, NULL},
	{p_isignore, 0, A_NEXT, TPS_InSpace, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InLatWord, 0, NULL},
	{p_isnonlatin, 0, A_NEXT, TPS_InCyrWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InUnsignedInt, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InSignedIntFirst, 0, NULL},
	{p_iseqC, '+', A_PUSH, TPS_InSignedIntFirst, 0, NULL},
	{p_iseqC, '&', A_PUSH, TPS_InHTMLEntityFirst, 0, NULL},
	{p_iseqC, '~', A_PUSH, TPS_InFileTwiddle, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InPathFirstFirst, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InSpace, 0, NULL}
};


static TParserStateActionItem actionTPS_InUWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, UWORD, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InUWord, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenUWordFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, UWORD, NULL}
};

static TParserStateActionItem actionTPS_InLatWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, LATWORD, NULL},
	{p_islatin, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenLatWordFirst, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, ':', A_PUSH, TPS_InProtocolFirst, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InUWord, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, LATWORD, NULL}
};

static TParserStateActionItem actionTPS_InCyrWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, CYRWORD, NULL},
	{p_isnonlatin, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InUWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenCyrWordFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, CYRWORD, NULL}
};

static TParserStateActionItem actionTPS_InUnsignedInt[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, UNSIGNEDINT, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InUDecimalFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_islatin, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InUWord, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, UNSIGNEDINT, NULL}
};

static TParserStateActionItem actionTPS_InSignedIntFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT | A_CLEAR, TPS_InSignedInt, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InSignedInt[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, SIGNEDINT, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InDecimalFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, SIGNEDINT, NULL}
};

static TParserStateActionItem actionTPS_InSpace[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, SPACE, NULL},
	{p_iseqC, '<', A_BINGO, TPS_Base, SPACE, NULL},
	{p_isignore, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_BINGO, TPS_Base, SPACE, NULL},
	{p_iseqC, '+', A_BINGO, TPS_Base, SPACE, NULL},
	{p_iseqC, '&', A_BINGO, TPS_Base, SPACE, NULL},
	{p_iseqC, '/', A_BINGO, TPS_Base, SPACE, NULL},
	{p_isnotalnum, 0, A_NEXT, TPS_InSpace, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, SPACE, NULL}
};

static TParserStateActionItem actionTPS_InUDecimalFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InUDecimal, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InUDecimal[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, DECIMAL, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InUDecimal, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVersionFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, DECIMAL, NULL}
};

static TParserStateActionItem actionTPS_InDecimalFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InDecimal, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InDecimal[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, DECIMAL, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InDecimal, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVerVersion, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, DECIMAL, NULL}
};

static TParserStateActionItem actionTPS_InVerVersion[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_RERUN, TPS_InSVerVersion, 0, SpecialVerVersion},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InSVerVersion[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_BINGO | A_CLRALL, TPS_InUnsignedInt, SPACE, NULL},
	{NULL, 0, A_NEXT, TPS_Null, 0, NULL}
};


static TParserStateActionItem actionTPS_InVersionFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InVersion, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InVersion[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, VERSIONNUMBER, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InVersion, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVersionFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, VERSIONNUMBER, NULL}
};

static TParserStateActionItem actionTPS_InMantissaFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InMantissa, 0, NULL},
	{p_iseqC, '+', A_NEXT, TPS_InMantissaSign, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InMantissaSign, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InMantissaSign[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InMantissa, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InMantissa[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, SCIENTIFIC, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InMantissa, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, SCIENTIFIC, NULL}
};

static TParserStateActionItem actionTPS_InHTMLEntityFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '#', A_NEXT, TPS_InHTMLEntityNumFirst, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHTMLEntity, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHTMLEntity[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHTMLEntity, 0, NULL},
	{p_iseqC, ';', A_NEXT, TPS_InHTMLEntityEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHTMLEntityNumFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHTMLEntityNum, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHTMLEntityNum[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHTMLEntityNum, 0, NULL},
	{p_iseqC, ';', A_NEXT, TPS_InHTMLEntityEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHTMLEntityEnd[] = {
	{NULL, 0, A_BINGO | A_CLEAR, TPS_Base, HTMLENTITY, NULL}
};

static TParserStateActionItem actionTPS_InTagFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InTagCloseFirst, 0, NULL},
	{p_iseqC, '!', A_PUSH, TPS_InCommentFirst, 0, NULL},
	{p_iseqC, '?', A_PUSH, TPS_InXMLBegin, 0, NULL},
	{p_islatin, 0, A_PUSH, TPS_InTagName, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InXMLBegin[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	/* <?xml ... */
	{p_iseqC, 'x', A_NEXT, TPS_InTag, 0, NULL},
	{p_iseqC, 'X', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InTagCloseFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InTagName, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InTagName[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	/* <br/> case */
	{p_iseqC, '/', A_NEXT, TPS_InTagBeginEnd, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, SpecialTags},
	{p_isspace, 0, A_NEXT, TPS_InTag, 0, SpecialTags},
	{p_islatin, 0, A_NEXT, TPS_Null, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InTagBeginEnd[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InTag[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, SpecialTags},
	{p_iseqC, '\'', A_NEXT, TPS_InTagEscapeK, 0, NULL},
	{p_iseqC, '"', A_NEXT, TPS_InTagEscapeKK, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '=', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '#', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, ':', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '&', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '?', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '%', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '~', A_NEXT, TPS_Null, 0, NULL},
	{p_isspace, 0, A_NEXT, TPS_Null, 0, SpecialTags},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InTagEscapeK[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '\\', A_PUSH, TPS_InTagBackSleshed, 0, NULL},
	{p_iseqC, '\'', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InTagEscapeK, 0, NULL}
};

static TParserStateActionItem actionTPS_InTagEscapeKK[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '\\', A_PUSH, TPS_InTagBackSleshed, 0, NULL},
	{p_iseqC, '"', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InTagEscapeKK, 0, NULL}
};

static TParserStateActionItem actionTPS_InTagBackSleshed[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{NULL, 0, A_MERGE, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InTagEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, TAG, NULL}
};

static TParserStateActionItem actionTPS_InCommentFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCommentLast, 0, NULL},
	/* <!DOCTYPE ...> */
	{p_iseqC, 'D', A_NEXT, TPS_InTag, 0, NULL},
	{p_iseqC, 'd', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InCommentLast[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InComment, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InComment[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCloseCommentFirst, 0, NULL},
	{NULL, 0, A_NEXT, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InCloseCommentFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCloseCommentLast, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InComment, 0, NULL}
};

static TParserStateActionItem actionTPS_InCloseCommentLast[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InCommentEnd, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InComment, 0, NULL}
};

static TParserStateActionItem actionTPS_InCommentEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, TAG, NULL}
};

static TParserStateActionItem actionTPS_InHostFirstDomain[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHostDomainSecond, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHostDomainSecond[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHostDomain, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHostDomain[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHostDomain, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_iseqC, ':', A_PUSH, TPS_InPortFirst, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_isdigit, 0, A_POP, TPS_Null, 0, NULL},
	{p_isstophost, 0, A_BINGO | A_CLRALL, TPS_InURIStart, HOST, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFURL, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL}
};

static TParserStateActionItem actionTPS_InPortFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InPort, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InPort[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InPort, 0, NULL},
	{p_isstophost, 0, A_BINGO | A_CLRALL, TPS_InURIStart, HOST, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFURL, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL}
};

static TParserStateActionItem actionTPS_InHostFirstAN[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHost, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHost[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InEmail[] = {
	{p_ishost, 0, A_BINGO | A_CLRALL, TPS_Base, EMAIL, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InFileFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathFirst, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '?', A_PUSH, TPS_InURIFirst, 0, NULL},
	{p_iseqC, '~', A_PUSH, TPS_InFileTwiddle, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InFileTwiddle[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InPathFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathSecond, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InPathFirstFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathSecond, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InPathSecond[] = {
	{p_isEOF, 0, A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{p_iseqC, '/', A_NEXT | A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '/', A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{p_isspace, 0, A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InFile[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, FILEPATH, NULL},
	{p_islatin, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '?', A_PUSH, TPS_InURIFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, FILEPATH, NULL}
};

static TParserStateActionItem actionTPS_InFileNext[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_CLEAR, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_CLEAR, TPS_InFile, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InURIFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '"', A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '\'', A_POP, TPS_Null, 0, NULL},
	{p_isnotspace, 0, A_CLEAR, TPS_InURI, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL},
};

static TParserStateActionItem actionTPS_InURIStart[] = {
	{NULL, 0, A_NEXT, TPS_InURI, 0, NULL}
};

static TParserStateActionItem actionTPS_InURI[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, URI, NULL},
	{p_iseqC, '"', A_BINGO, TPS_Base, URI, NULL},
	{p_iseqC, '\'', A_BINGO, TPS_Base, URI, NULL},
	{p_isnotspace, 0, A_NEXT, TPS_InURI, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, URI, NULL}
};

static TParserStateActionItem actionTPS_InFURL[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isURI, 0, A_BINGO | A_CLRALL, TPS_Base, FURL, SpecialFURL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InProtocolFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InProtocolSecond, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InProtocolSecond[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InProtocolEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InProtocolEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, PROTOCOL, NULL}
};

static TParserStateActionItem actionTPS_InHyphenLatWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHyphenLatWord, 0, NULL},
	{p_isnonlatin, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenValue, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHyphenLatWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, LATHYPHENWORD, SpecialHyphen},
	{p_islatin, 0, A_NEXT, TPS_InHyphenLatWord, 0, NULL},
	{p_isnonlatin, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenLatWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, LATHYPHENWORD, SpecialHyphen}
};

static TParserStateActionItem actionTPS_InHyphenCyrWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isnonlatin, 0, A_NEXT, TPS_InHyphenCyrWord, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenValue, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHyphenCyrWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, CYRHYPHENWORD, SpecialHyphen},
	{p_isnonlatin, 0, A_NEXT, TPS_InHyphenCyrWord, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenCyrWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, CYRHYPHENWORD, SpecialHyphen}
};

static TParserStateActionItem actionTPS_InHyphenUWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenValue, 0, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHyphenUWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HYPHENWORD, SpecialHyphen},
	{p_isalnum, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenUWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HYPHENWORD, SpecialHyphen}
};

static TParserStateActionItem actionTPS_InHyphenValueFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenValueExact, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHyphenValue[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HYPHENWORD, SpecialHyphen},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenValue, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHyphenValueFirst, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenUWordFirst, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenUWord, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HYPHENWORD, SpecialHyphen}
};

static TParserStateActionItem actionTPS_InHyphenValueExact[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HYPHENWORD, SpecialHyphen},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenValueExact, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHyphenValueFirst, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenUWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HYPHENWORD, SpecialHyphen}
};

static TParserStateActionItem actionTPS_InParseHyphen[] = {
	{p_isEOF, 0, A_RERUN, TPS_Base, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHyphenLatWordPart, 0, NULL},
	{p_isnonlatin, 0, A_NEXT, TPS_InHyphenCyrWordPart, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenUnsignedInt, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InParseHyphenHyphen, 0, NULL},
	{NULL, 0, A_RERUN, TPS_Base, 0, NULL}
};

static TParserStateActionItem actionTPS_InParseHyphenHyphen[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalnum, 0, A_BINGO | A_CLEAR, TPS_InParseHyphen, SPACE, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHyphenCyrWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, CYRPARTHYPHENWORD, NULL},
	{p_isnonlatin, 0, A_NEXT, TPS_InHyphenCyrWordPart, 0, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHyphenUWordPart, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenUWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, CYRPARTHYPHENWORD, NULL}
};

static TParserStateActionItem actionTPS_InHyphenLatWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, LATPARTHYPHENWORD, NULL},
	{p_islatin, 0, A_NEXT, TPS_InHyphenLatWordPart, 0, NULL},
	{p_isnonlatin, 0, A_NEXT, TPS_InHyphenUWordPart, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenUWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, LATPARTHYPHENWORD, NULL}
};

static TParserStateActionItem actionTPS_InHyphenUWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, PARTHYPHENWORD, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InHyphenUWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, PARTHYPHENWORD, NULL}
};

static TParserStateActionItem actionTPS_InHyphenUnsignedInt[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, UNSIGNEDINT, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenUnsignedInt, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenUWordPart, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHDecimalPartFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, UNSIGNEDINT, NULL}
};

static TParserStateActionItem actionTPS_InHDecimalPartFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InHDecimalPart, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHDecimalPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, DECIMAL, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHDecimalPart, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHVersionPartFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, DECIMAL, NULL}
};

static TParserStateActionItem actionTPS_InHVersionPartFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InHVersionPart, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static TParserStateActionItem actionTPS_InHVersionPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, VERSIONNUMBER, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHVersionPart, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHVersionPartFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, VERSIONNUMBER, NULL}
};

/*
 * order should be the same as in typedef enum {} TParserState!!
 */

static const TParserStateAction Actions[] = {
	{TPS_Base, actionTPS_Base},
	{TPS_InUWord, actionTPS_InUWord},
	{TPS_InLatWord, actionTPS_InLatWord},
	{TPS_InCyrWord, actionTPS_InCyrWord},
	{TPS_InUnsignedInt, actionTPS_InUnsignedInt},
	{TPS_InSignedIntFirst, actionTPS_InSignedIntFirst},
	{TPS_InSignedInt, actionTPS_InSignedInt},
	{TPS_InSpace, actionTPS_InSpace},
	{TPS_InUDecimalFirst, actionTPS_InUDecimalFirst},
	{TPS_InUDecimal, actionTPS_InUDecimal},
	{TPS_InDecimalFirst, actionTPS_InDecimalFirst},
	{TPS_InDecimal, actionTPS_InDecimal},
	{TPS_InVerVersion, actionTPS_InVerVersion},
	{TPS_InSVerVersion, actionTPS_InSVerVersion},
	{TPS_InVersionFirst, actionTPS_InVersionFirst},
	{TPS_InVersion, actionTPS_InVersion},
	{TPS_InMantissaFirst, actionTPS_InMantissaFirst},
	{TPS_InMantissaSign, actionTPS_InMantissaSign},
	{TPS_InMantissa, actionTPS_InMantissa},
	{TPS_InHTMLEntityFirst, actionTPS_InHTMLEntityFirst},
	{TPS_InHTMLEntity, actionTPS_InHTMLEntity},
	{TPS_InHTMLEntityNumFirst, actionTPS_InHTMLEntityNumFirst},
	{TPS_InHTMLEntityNum, actionTPS_InHTMLEntityNum},
	{TPS_InHTMLEntityEnd, actionTPS_InHTMLEntityEnd},
	{TPS_InTagFirst, actionTPS_InTagFirst},
	{TPS_InXMLBegin, actionTPS_InXMLBegin},
	{TPS_InTagCloseFirst, actionTPS_InTagCloseFirst},
	{TPS_InTagName, actionTPS_InTagName},
	{TPS_InTagBeginEnd, actionTPS_InTagBeginEnd},
	{TPS_InTag, actionTPS_InTag},
	{TPS_InTagEscapeK, actionTPS_InTagEscapeK},
	{TPS_InTagEscapeKK, actionTPS_InTagEscapeKK},
	{TPS_InTagBackSleshed, actionTPS_InTagBackSleshed},
	{TPS_InTagEnd, actionTPS_InTagEnd},
	{TPS_InCommentFirst, actionTPS_InCommentFirst},
	{TPS_InCommentLast, actionTPS_InCommentLast},
	{TPS_InComment, actionTPS_InComment},
	{TPS_InCloseCommentFirst, actionTPS_InCloseCommentFirst},
	{TPS_InCloseCommentLast, actionTPS_InCloseCommentLast},
	{TPS_InCommentEnd, actionTPS_InCommentEnd},
	{TPS_InHostFirstDomain, actionTPS_InHostFirstDomain},
	{TPS_InHostDomainSecond, actionTPS_InHostDomainSecond},
	{TPS_InHostDomain, actionTPS_InHostDomain},
	{TPS_InPortFirst, actionTPS_InPortFirst},
	{TPS_InPort, actionTPS_InPort},
	{TPS_InHostFirstAN, actionTPS_InHostFirstAN},
	{TPS_InHost, actionTPS_InHost},
	{TPS_InEmail, actionTPS_InEmail},
	{TPS_InFileFirst, actionTPS_InFileFirst},
	{TPS_InFileTwiddle, actionTPS_InFileTwiddle},
	{TPS_InPathFirst, actionTPS_InPathFirst},
	{TPS_InPathFirstFirst, actionTPS_InPathFirstFirst},
	{TPS_InPathSecond, actionTPS_InPathSecond},
	{TPS_InFile, actionTPS_InFile},
	{TPS_InFileNext, actionTPS_InFileNext},
	{TPS_InURIFirst, actionTPS_InURIFirst},
	{TPS_InURIStart, actionTPS_InURIStart},
	{TPS_InURI, actionTPS_InURI},
	{TPS_InFURL, actionTPS_InFURL},
	{TPS_InProtocolFirst, actionTPS_InProtocolFirst},
	{TPS_InProtocolSecond, actionTPS_InProtocolSecond},
	{TPS_InProtocolEnd, actionTPS_InProtocolEnd},
	{TPS_InHyphenLatWordFirst, actionTPS_InHyphenLatWordFirst},
	{TPS_InHyphenLatWord, actionTPS_InHyphenLatWord},
	{TPS_InHyphenCyrWordFirst, actionTPS_InHyphenCyrWordFirst},
	{TPS_InHyphenCyrWord, actionTPS_InHyphenCyrWord},
	{TPS_InHyphenUWordFirst, actionTPS_InHyphenUWordFirst},
	{TPS_InHyphenUWord, actionTPS_InHyphenUWord},
	{TPS_InHyphenValueFirst, actionTPS_InHyphenValueFirst},
	{TPS_InHyphenValue, actionTPS_InHyphenValue},
	{TPS_InHyphenValueExact, actionTPS_InHyphenValueExact},
	{TPS_InParseHyphen, actionTPS_InParseHyphen},
	{TPS_InParseHyphenHyphen, actionTPS_InParseHyphenHyphen},
	{TPS_InHyphenCyrWordPart, actionTPS_InHyphenCyrWordPart},
	{TPS_InHyphenLatWordPart, actionTPS_InHyphenLatWordPart},
	{TPS_InHyphenUWordPart, actionTPS_InHyphenUWordPart},
	{TPS_InHyphenUnsignedInt, actionTPS_InHyphenUnsignedInt},
	{TPS_InHDecimalPartFirst, actionTPS_InHDecimalPartFirst},
	{TPS_InHDecimalPart, actionTPS_InHDecimalPart},
	{TPS_InHVersionPartFirst, actionTPS_InHVersionPartFirst},
	{TPS_InHVersionPart, actionTPS_InHVersionPart},
	{TPS_Null, NULL}
};


bool
TParserGet(TParser * prs)
{
	TParserStateActionItem *item = NULL;

	if (prs->state->posbyte >= prs->lenstr)
		return false;

	Assert(prs->state);
	prs->lexeme = prs->str + prs->state->posbyte;
	prs->state->pushedAtAction = NULL;

	/* look at string */
	while (prs->state->posbyte <= prs->lenstr)
	{
		if (prs->state->posbyte == prs->lenstr)
			prs->state->charlen = 0;
		else
			prs->state->charlen = (prs->charmaxlen == 1) ? prs->charmaxlen :
				pg_mblen(prs->str + prs->state->posbyte);

		Assert(prs->state->posbyte + prs->state->charlen <= prs->lenstr);
		Assert(prs->state->state >= TPS_Base && prs->state->state < TPS_Null);
		Assert(Actions[prs->state->state].state == prs->state->state);

		item = Actions[prs->state->state].action;
		Assert(item != NULL);

		if (item < prs->state->pushedAtAction)
			item = prs->state->pushedAtAction;

		/* find action by character class */
		while (item->isclass)
		{
			prs->c = item->c;
			if (item->isclass(prs) != 0)
			{
				if (item > prs->state->pushedAtAction)	/* remember: after
														 * pushing we were by
														 * false way */
					break;
			}
			item++;
		}

		prs->state->pushedAtAction = NULL;

		/* call special handler if exists */
		if (item->special)
			item->special(prs);

		/* BINGO, lexeme is found */
		if (item->flags & A_BINGO)
		{
			Assert(item->type > 0);
			prs->lenbytelexeme = prs->state->lenbytelexeme;
			prs->lencharlexeme = prs->state->lencharlexeme;
			prs->state->lenbytelexeme = prs->state->lencharlexeme = 0;
			prs->type = item->type;
		}

		/* do various actions by flags */
		if (item->flags & A_POP)
		{						/* pop stored state in stack */
			TParserPosition *ptr = prs->state->prev;

			pfree(prs->state);
			prs->state = ptr;
			Assert(prs->state);
		}
		else if (item->flags & A_PUSH)
		{						/* push (store) state in stack */
			prs->state->pushedAtAction = item;	/* remember where we push */
			prs->state = newTParserPosition(prs->state);
		}
		else if (item->flags & A_CLEAR)
		{						/* clear previous pushed state */
			TParserPosition *ptr;

			Assert(prs->state->prev);
			ptr = prs->state->prev->prev;
			pfree(prs->state->prev);
			prs->state->prev = ptr;
		}
		else if (item->flags & A_CLRALL)
		{						/* clear all previous pushed state */
			TParserPosition *ptr;

			while (prs->state->prev)
			{
				ptr = prs->state->prev->prev;
				pfree(prs->state->prev);
				prs->state->prev = ptr;
			}
		}
		else if (item->flags & A_MERGE)
		{						/* merge posinfo with current and pushed state */
			TParserPosition *ptr = prs->state;

			Assert(prs->state->prev);
			prs->state = prs->state->prev;

			prs->state->posbyte = ptr->posbyte;
			prs->state->poschar = ptr->poschar;
			prs->state->charlen = ptr->charlen;
			prs->state->lenbytelexeme = ptr->lenbytelexeme;
			prs->state->lencharlexeme = ptr->lencharlexeme;
			pfree(ptr);
		}

		/* set new state if pointed */
		if (item->tostate != TPS_Null)
			prs->state->state = item->tostate;

		/* check for go away */
		if ((item->flags & A_BINGO) || (prs->state->posbyte >= prs->lenstr && (item->flags & A_RERUN) == 0))
			break;

		/* go to begining of loop if we should rerun or we just restore state */
		if (item->flags & (A_RERUN | A_POP))
			continue;

		/* move forward */
		if (prs->state->charlen)
		{
			prs->state->posbyte += prs->state->charlen;
			prs->state->lenbytelexeme += prs->state->charlen;
			prs->state->poschar++;
			prs->state->lencharlexeme++;
		}
	}

	return (item && (item->flags & A_BINGO)) ? true : false;
}
