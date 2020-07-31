/*-------------------------------------------------------------------------
 *
 * wparser_def.c
 *		Default text search parser
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/tsearch/wparser_def.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "tsearch/ts_locale.h"
#include "tsearch/ts_public.h"
#include "tsearch/ts_type.h"
#include "tsearch/ts_utils.h"
#include "utils/builtins.h"


/* Define me to enable tracing of parser behavior */
/* #define WPARSER_TRACE */


/* Output token categories */

#define ASCIIWORD		1
#define WORD_T			2
#define NUMWORD			3
#define EMAIL			4
#define URL_T			5
#define HOST			6
#define SCIENTIFIC		7
#define VERSIONNUMBER	8
#define NUMPARTHWORD	9
#define PARTHWORD		10
#define ASCIIPARTHWORD	11
#define SPACE			12
#define TAG_T			13
#define PROTOCOL		14
#define NUMHWORD		15
#define ASCIIHWORD		16
#define HWORD			17
#define URLPATH			18
#define FILEPATH		19
#define DECIMAL_T		20
#define SIGNEDINT		21
#define UNSIGNEDINT		22
#define XMLENTITY		23

#define LASTNUM			23

static const char *const tok_alias[] = {
	"",
	"asciiword",
	"word",
	"numword",
	"email",
	"url",
	"host",
	"sfloat",
	"version",
	"hword_numpart",
	"hword_part",
	"hword_asciipart",
	"blank",
	"tag",
	"protocol",
	"numhword",
	"asciihword",
	"hword",
	"url_path",
	"file",
	"float",
	"int",
	"uint",
	"entity"
};

static const char *const lex_descr[] = {
	"",
	"Word, all ASCII",
	"Word, all letters",
	"Word, letters and digits",
	"Email address",
	"URL",
	"Host",
	"Scientific notation",
	"Version number",
	"Hyphenated word part, letters and digits",
	"Hyphenated word part, all letters",
	"Hyphenated word part, all ASCII",
	"Space symbols",
	"XML tag",
	"Protocol head",
	"Hyphenated word, letters and digits",
	"Hyphenated word, all ASCII",
	"Hyphenated word, all letters",
	"URL path",
	"File or path name",
	"Decimal notation",
	"Signed integer",
	"Unsigned integer",
	"XML entity"
};


/* Parser states */

typedef enum
{
	TPS_Base = 0,
	TPS_InNumWord,
	TPS_InAsciiWord,
	TPS_InWord,
	TPS_InUnsignedInt,
	TPS_InSignedIntFirst,
	TPS_InSignedInt,
	TPS_InSpace,
	TPS_InUDecimalFirst,
	TPS_InUDecimal,
	TPS_InDecimalFirst,
	TPS_InDecimal,
	TPS_InVerVersion,
	TPS_InSVerVersion,
	TPS_InVersionFirst,
	TPS_InVersion,
	TPS_InMantissaFirst,
	TPS_InMantissaSign,
	TPS_InMantissa,
	TPS_InXMLEntityFirst,
	TPS_InXMLEntity,
	TPS_InXMLEntityNumFirst,
	TPS_InXMLEntityNum,
	TPS_InXMLEntityHexNumFirst,
	TPS_InXMLEntityHexNum,
	TPS_InXMLEntityEnd,
	TPS_InTagFirst,
	TPS_InXMLBegin,
	TPS_InTagCloseFirst,
	TPS_InTagName,
	TPS_InTagBeginEnd,
	TPS_InTag,
	TPS_InTagEscapeK,
	TPS_InTagEscapeKK,
	TPS_InTagBackSleshed,
	TPS_InTagEnd,
	TPS_InCommentFirst,
	TPS_InCommentLast,
	TPS_InComment,
	TPS_InCloseCommentFirst,
	TPS_InCloseCommentLast,
	TPS_InCommentEnd,
	TPS_InHostFirstDomain,
	TPS_InHostDomainSecond,
	TPS_InHostDomain,
	TPS_InPortFirst,
	TPS_InPort,
	TPS_InHostFirstAN,
	TPS_InHost,
	TPS_InEmail,
	TPS_InFileFirst,
	TPS_InFileTwiddle,
	TPS_InPathFirst,
	TPS_InPathFirstFirst,
	TPS_InPathSecond,
	TPS_InFile,
	TPS_InFileNext,
	TPS_InURLPathFirst,
	TPS_InURLPathStart,
	TPS_InURLPath,
	TPS_InFURL,
	TPS_InProtocolFirst,
	TPS_InProtocolSecond,
	TPS_InProtocolEnd,
	TPS_InHyphenAsciiWordFirst,
	TPS_InHyphenAsciiWord,
	TPS_InHyphenWordFirst,
	TPS_InHyphenWord,
	TPS_InHyphenNumWordFirst,
	TPS_InHyphenNumWord,
	TPS_InHyphenDigitLookahead,
	TPS_InParseHyphen,
	TPS_InParseHyphenHyphen,
	TPS_InHyphenWordPart,
	TPS_InHyphenAsciiWordPart,
	TPS_InHyphenNumWordPart,
	TPS_InHyphenUnsignedInt,
	TPS_Null					/* last state (fake value) */
} TParserState;

/* forward declaration */
struct TParser;

typedef int (*TParserCharTest) (struct TParser *);	/* any p_is* functions
													 * except p_iseq */
typedef void (*TParserSpecial) (struct TParser *);	/* special handler for
													 * special cases... */

typedef struct
{
	TParserCharTest isclass;
	char		c;
	uint16		flags;
	TParserState tostate;
	int			type;
	TParserSpecial special;
} TParserStateActionItem;

/* Flag bits in TParserStateActionItem.flags */
#define A_NEXT		0x0000
#define A_BINGO		0x0001
#define A_POP		0x0002
#define A_PUSH		0x0004
#define A_RERUN		0x0008
#define A_CLEAR		0x0010
#define A_MERGE		0x0020
#define A_CLRALL	0x0040

typedef struct TParserPosition
{
	int			posbyte;		/* position of parser in bytes */
	int			poschar;		/* position of parser in characters */
	int			charlen;		/* length of current char */
	int			lenbytetoken;	/* length of token-so-far in bytes */
	int			lenchartoken;	/* and in chars */
	TParserState state;
	struct TParserPosition *prev;
	const TParserStateActionItem *pushedAtAction;
} TParserPosition;

typedef struct TParser
{
	/* string and position information */
	char	   *str;			/* multibyte string */
	int			lenstr;			/* length of mbstring */
	wchar_t    *wstr;			/* wide character string */
	pg_wchar   *pgwstr;			/* wide character string for C-locale */
	bool		usewide;

	/* State of parse */
	int			charmaxlen;
	TParserPosition *state;
	bool		ignore;
	bool		wanthost;

	/* silly char */
	char		c;

	/* out */
	char	   *token;
	int			lenbytetoken;
	int			lenchartoken;
	int			type;
} TParser;


/* forward decls here */
static bool TParserGet(TParser *prs);


static TParserPosition *
newTParserPosition(TParserPosition *prev)
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

static TParser *
TParserInit(char *str, int len)
{
	TParser    *prs = (TParser *) palloc0(sizeof(TParser));

	prs->charmaxlen = pg_database_encoding_max_length();
	prs->str = str;
	prs->lenstr = len;

	/*
	 * Use wide char code only when max encoding length > 1.
	 */
	if (prs->charmaxlen > 1)
	{
		Oid			collation = DEFAULT_COLLATION_OID;	/* TODO */
		pg_locale_t mylocale = 0;	/* TODO */

		prs->usewide = true;
		if (lc_ctype_is_c(collation))
		{
			/*
			 * char2wchar doesn't work for C-locale and sizeof(pg_wchar) could
			 * be different from sizeof(wchar_t)
			 */
			prs->pgwstr = (pg_wchar *) palloc(sizeof(pg_wchar) * (prs->lenstr + 1));
			pg_mb2wchar_with_len(prs->str, prs->pgwstr, prs->lenstr);
		}
		else
		{
			prs->wstr = (wchar_t *) palloc(sizeof(wchar_t) * (prs->lenstr + 1));
			char2wchar(prs->wstr, prs->lenstr + 1, prs->str, prs->lenstr,
					   mylocale);
		}
	}
	else
		prs->usewide = false;

	prs->state = newTParserPosition(NULL);
	prs->state->state = TPS_Base;

#ifdef WPARSER_TRACE

	/*
	 * Use of %.*s here is a bit risky since it can misbehave if the data is
	 * not in what libc thinks is the prevailing encoding.  However, since
	 * this is just a debugging aid, we choose to live with that.
	 */
	fprintf(stderr, "parsing \"%.*s\"\n", len, str);
#endif

	return prs;
}

/*
 * As an alternative to a full TParserInit one can create a
 * TParserCopy which basically is a regular TParser without a private
 * copy of the string - instead it uses the one from another TParser.
 * This is useful because at some places TParsers are created
 * recursively and the repeated copying around of the strings can
 * cause major inefficiency if the source string is long.
 * The new parser starts parsing at the original's current position.
 *
 * Obviously one must not close the original TParser before the copy.
 */
static TParser *
TParserCopyInit(const TParser *orig)
{
	TParser    *prs = (TParser *) palloc0(sizeof(TParser));

	prs->charmaxlen = orig->charmaxlen;
	prs->str = orig->str + orig->state->posbyte;
	prs->lenstr = orig->lenstr - orig->state->posbyte;
	prs->usewide = orig->usewide;

	if (orig->pgwstr)
		prs->pgwstr = orig->pgwstr + orig->state->poschar;
	if (orig->wstr)
		prs->wstr = orig->wstr + orig->state->poschar;

	prs->state = newTParserPosition(NULL);
	prs->state->state = TPS_Base;

#ifdef WPARSER_TRACE
	/* See note above about %.*s */
	fprintf(stderr, "parsing copy of \"%.*s\"\n", prs->lenstr, prs->str);
#endif

	return prs;
}


static void
TParserClose(TParser *prs)
{
	while (prs->state)
	{
		TParserPosition *ptr = prs->state->prev;

		pfree(prs->state);
		prs->state = ptr;
	}

	if (prs->wstr)
		pfree(prs->wstr);
	if (prs->pgwstr)
		pfree(prs->pgwstr);

#ifdef WPARSER_TRACE
	fprintf(stderr, "closing parser\n");
#endif
	pfree(prs);
}

/*
 * Close a parser created with TParserCopyInit
 */
static void
TParserCopyClose(TParser *prs)
{
	while (prs->state)
	{
		TParserPosition *ptr = prs->state->prev;

		pfree(prs->state);
		prs->state = ptr;
	}

#ifdef WPARSER_TRACE
	fprintf(stderr, "closing parser copy\n");
#endif
	pfree(prs);
}


/*
 * Character-type support functions, equivalent to is* macros, but
 * working with any possible encodings and locales. Notes:
 *	- with multibyte encoding and C-locale isw* function may fail
 *	  or give wrong result.
 *	- multibyte encoding and C-locale often are used for
 *	  Asian languages.
 *	- if locale is C then we use pgwstr instead of wstr.
 */

#define p_iswhat(type, nonascii)											\
																			\
static int																	\
p_is##type(TParser *prs)													\
{																			\
	Assert(prs->state);														\
	if (prs->usewide)														\
	{																		\
		if (prs->pgwstr)													\
		{																	\
			unsigned int c = *(prs->pgwstr + prs->state->poschar);			\
			if (c > 0x7f)													\
				return nonascii;											\
			return is##type(c);												\
		}																	\
		return isw##type(*(prs->wstr + prs->state->poschar));				\
	}																		\
	return is##type(*(unsigned char *) (prs->str + prs->state->posbyte));	\
}																			\
																			\
static int																	\
p_isnot##type(TParser *prs)													\
{																			\
	return !p_is##type(prs);												\
}

/*
 * In C locale with a multibyte encoding, any non-ASCII symbol is considered
 * an alpha character, but not a member of other char classes.
 */
p_iswhat(alnum, 1)
p_iswhat(alpha, 1)
p_iswhat(digit, 0)
p_iswhat(lower, 0)
p_iswhat(print, 0)
p_iswhat(punct, 0)
p_iswhat(space, 0)
p_iswhat(upper, 0)
p_iswhat(xdigit, 0)

/* p_iseq should be used only for ascii symbols */

static int
p_iseq(TParser *prs, char c)
{
	Assert(prs->state);
	return ((prs->state->charlen == 1 && *(prs->str + prs->state->posbyte) == c)) ? 1 : 0;
}

static int
p_isEOF(TParser *prs)
{
	Assert(prs->state);
	return (prs->state->posbyte == prs->lenstr || prs->state->charlen == 0) ? 1 : 0;
}

static int
p_iseqC(TParser *prs)
{
	return p_iseq(prs, prs->c);
}

static int
p_isneC(TParser *prs)
{
	return !p_iseq(prs, prs->c);
}

static int
p_isascii(TParser *prs)
{
	return (prs->state->charlen == 1 && isascii((unsigned char) *(prs->str + prs->state->posbyte))) ? 1 : 0;
}

static int
p_isasclet(TParser *prs)
{
	return (p_isascii(prs) && p_isalpha(prs)) ? 1 : 0;
}

static int
p_isurlchar(TParser *prs)
{
	char		ch;

	/* no non-ASCII need apply */
	if (prs->state->charlen != 1)
		return 0;
	ch = *(prs->str + prs->state->posbyte);
	/* no spaces or control characters */
	if (ch <= 0x20 || ch >= 0x7F)
		return 0;
	/* reject characters disallowed by RFC 3986 */
	switch (ch)
	{
		case '"':
		case '<':
		case '>':
		case '\\':
		case '^':
		case '`':
		case '{':
		case '|':
		case '}':
			return 0;
	}
	return 1;
}


/* deliberately suppress unused-function complaints for the above */
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
SpecialTags(TParser *prs)
{
	switch (prs->state->lenchartoken)
	{
		case 8:					/* </script */
			if (pg_strncasecmp(prs->token, "</script", 8) == 0)
				prs->ignore = false;
			break;
		case 7:					/* <script || </style */
			if (pg_strncasecmp(prs->token, "</style", 7) == 0)
				prs->ignore = false;
			else if (pg_strncasecmp(prs->token, "<script", 7) == 0)
				prs->ignore = true;
			break;
		case 6:					/* <style */
			if (pg_strncasecmp(prs->token, "<style", 6) == 0)
				prs->ignore = true;
			break;
		default:
			break;
	}
}

static void
SpecialFURL(TParser *prs)
{
	prs->wanthost = true;
	prs->state->posbyte -= prs->state->lenbytetoken;
	prs->state->poschar -= prs->state->lenchartoken;
}

static void
SpecialHyphen(TParser *prs)
{
	prs->state->posbyte -= prs->state->lenbytetoken;
	prs->state->poschar -= prs->state->lenchartoken;
}

static void
SpecialVerVersion(TParser *prs)
{
	prs->state->posbyte -= prs->state->lenbytetoken;
	prs->state->poschar -= prs->state->lenchartoken;
	prs->state->lenbytetoken = 0;
	prs->state->lenchartoken = 0;
}

static int
p_isstophost(TParser *prs)
{
	if (prs->wanthost)
	{
		prs->wanthost = false;
		return 1;
	}
	return 0;
}

static int
p_isignore(TParser *prs)
{
	return (prs->ignore) ? 1 : 0;
}

static int
p_ishost(TParser *prs)
{
	TParser    *tmpprs = TParserCopyInit(prs);
	int			res = 0;

	tmpprs->wanthost = true;

	if (TParserGet(tmpprs) && tmpprs->type == HOST)
	{
		prs->state->posbyte += tmpprs->lenbytetoken;
		prs->state->poschar += tmpprs->lenchartoken;
		prs->state->lenbytetoken += tmpprs->lenbytetoken;
		prs->state->lenchartoken += tmpprs->lenchartoken;
		prs->state->charlen = tmpprs->state->charlen;
		res = 1;
	}
	TParserCopyClose(tmpprs);

	return res;
}

static int
p_isURLPath(TParser *prs)
{
	TParser    *tmpprs = TParserCopyInit(prs);
	int			res = 0;

	tmpprs->state = newTParserPosition(tmpprs->state);
	tmpprs->state->state = TPS_InURLPathFirst;

	if (TParserGet(tmpprs) && tmpprs->type == URLPATH)
	{
		prs->state->posbyte += tmpprs->lenbytetoken;
		prs->state->poschar += tmpprs->lenchartoken;
		prs->state->lenbytetoken += tmpprs->lenbytetoken;
		prs->state->lenchartoken += tmpprs->lenchartoken;
		prs->state->charlen = tmpprs->state->charlen;
		res = 1;
	}
	TParserCopyClose(tmpprs);

	return res;
}

/*
 * returns true if current character has zero display length or
 * it's a special sign in several languages. Such characters
 * aren't a word-breaker although they aren't an isalpha.
 * In beginning of word they aren't a part of it.
 */
static int
p_isspecial(TParser *prs)
{
	/*
	 * pg_dsplen could return -1 which means error or control character
	 */
	if (pg_dsplen(prs->str + prs->state->posbyte) == 0)
		return 1;

	/*
	 * Unicode Characters in the 'Mark, Spacing Combining' Category That
	 * characters are not alpha although they are not breakers of word too.
	 * Check that only in utf encoding, because other encodings aren't
	 * supported by postgres or even exists.
	 */
	if (GetDatabaseEncoding() == PG_UTF8 && prs->usewide)
	{
		static const pg_wchar strange_letter[] = {
			/*
			 * use binary search, so elements should be ordered
			 */
			0x0903,				/* DEVANAGARI SIGN VISARGA */
			0x093E,				/* DEVANAGARI VOWEL SIGN AA */
			0x093F,				/* DEVANAGARI VOWEL SIGN I */
			0x0940,				/* DEVANAGARI VOWEL SIGN II */
			0x0949,				/* DEVANAGARI VOWEL SIGN CANDRA O */
			0x094A,				/* DEVANAGARI VOWEL SIGN SHORT O */
			0x094B,				/* DEVANAGARI VOWEL SIGN O */
			0x094C,				/* DEVANAGARI VOWEL SIGN AU */
			0x0982,				/* BENGALI SIGN ANUSVARA */
			0x0983,				/* BENGALI SIGN VISARGA */
			0x09BE,				/* BENGALI VOWEL SIGN AA */
			0x09BF,				/* BENGALI VOWEL SIGN I */
			0x09C0,				/* BENGALI VOWEL SIGN II */
			0x09C7,				/* BENGALI VOWEL SIGN E */
			0x09C8,				/* BENGALI VOWEL SIGN AI */
			0x09CB,				/* BENGALI VOWEL SIGN O */
			0x09CC,				/* BENGALI VOWEL SIGN AU */
			0x09D7,				/* BENGALI AU LENGTH MARK */
			0x0A03,				/* GURMUKHI SIGN VISARGA */
			0x0A3E,				/* GURMUKHI VOWEL SIGN AA */
			0x0A3F,				/* GURMUKHI VOWEL SIGN I */
			0x0A40,				/* GURMUKHI VOWEL SIGN II */
			0x0A83,				/* GUJARATI SIGN VISARGA */
			0x0ABE,				/* GUJARATI VOWEL SIGN AA */
			0x0ABF,				/* GUJARATI VOWEL SIGN I */
			0x0AC0,				/* GUJARATI VOWEL SIGN II */
			0x0AC9,				/* GUJARATI VOWEL SIGN CANDRA O */
			0x0ACB,				/* GUJARATI VOWEL SIGN O */
			0x0ACC,				/* GUJARATI VOWEL SIGN AU */
			0x0B02,				/* ORIYA SIGN ANUSVARA */
			0x0B03,				/* ORIYA SIGN VISARGA */
			0x0B3E,				/* ORIYA VOWEL SIGN AA */
			0x0B40,				/* ORIYA VOWEL SIGN II */
			0x0B47,				/* ORIYA VOWEL SIGN E */
			0x0B48,				/* ORIYA VOWEL SIGN AI */
			0x0B4B,				/* ORIYA VOWEL SIGN O */
			0x0B4C,				/* ORIYA VOWEL SIGN AU */
			0x0B57,				/* ORIYA AU LENGTH MARK */
			0x0BBE,				/* TAMIL VOWEL SIGN AA */
			0x0BBF,				/* TAMIL VOWEL SIGN I */
			0x0BC1,				/* TAMIL VOWEL SIGN U */
			0x0BC2,				/* TAMIL VOWEL SIGN UU */
			0x0BC6,				/* TAMIL VOWEL SIGN E */
			0x0BC7,				/* TAMIL VOWEL SIGN EE */
			0x0BC8,				/* TAMIL VOWEL SIGN AI */
			0x0BCA,				/* TAMIL VOWEL SIGN O */
			0x0BCB,				/* TAMIL VOWEL SIGN OO */
			0x0BCC,				/* TAMIL VOWEL SIGN AU */
			0x0BD7,				/* TAMIL AU LENGTH MARK */
			0x0C01,				/* TELUGU SIGN CANDRABINDU */
			0x0C02,				/* TELUGU SIGN ANUSVARA */
			0x0C03,				/* TELUGU SIGN VISARGA */
			0x0C41,				/* TELUGU VOWEL SIGN U */
			0x0C42,				/* TELUGU VOWEL SIGN UU */
			0x0C43,				/* TELUGU VOWEL SIGN VOCALIC R */
			0x0C44,				/* TELUGU VOWEL SIGN VOCALIC RR */
			0x0C82,				/* KANNADA SIGN ANUSVARA */
			0x0C83,				/* KANNADA SIGN VISARGA */
			0x0CBE,				/* KANNADA VOWEL SIGN AA */
			0x0CC0,				/* KANNADA VOWEL SIGN II */
			0x0CC1,				/* KANNADA VOWEL SIGN U */
			0x0CC2,				/* KANNADA VOWEL SIGN UU */
			0x0CC3,				/* KANNADA VOWEL SIGN VOCALIC R */
			0x0CC4,				/* KANNADA VOWEL SIGN VOCALIC RR */
			0x0CC7,				/* KANNADA VOWEL SIGN EE */
			0x0CC8,				/* KANNADA VOWEL SIGN AI */
			0x0CCA,				/* KANNADA VOWEL SIGN O */
			0x0CCB,				/* KANNADA VOWEL SIGN OO */
			0x0CD5,				/* KANNADA LENGTH MARK */
			0x0CD6,				/* KANNADA AI LENGTH MARK */
			0x0D02,				/* MALAYALAM SIGN ANUSVARA */
			0x0D03,				/* MALAYALAM SIGN VISARGA */
			0x0D3E,				/* MALAYALAM VOWEL SIGN AA */
			0x0D3F,				/* MALAYALAM VOWEL SIGN I */
			0x0D40,				/* MALAYALAM VOWEL SIGN II */
			0x0D46,				/* MALAYALAM VOWEL SIGN E */
			0x0D47,				/* MALAYALAM VOWEL SIGN EE */
			0x0D48,				/* MALAYALAM VOWEL SIGN AI */
			0x0D4A,				/* MALAYALAM VOWEL SIGN O */
			0x0D4B,				/* MALAYALAM VOWEL SIGN OO */
			0x0D4C,				/* MALAYALAM VOWEL SIGN AU */
			0x0D57,				/* MALAYALAM AU LENGTH MARK */
			0x0D82,				/* SINHALA SIGN ANUSVARAYA */
			0x0D83,				/* SINHALA SIGN VISARGAYA */
			0x0DCF,				/* SINHALA VOWEL SIGN AELA-PILLA */
			0x0DD0,				/* SINHALA VOWEL SIGN KETTI AEDA-PILLA */
			0x0DD1,				/* SINHALA VOWEL SIGN DIGA AEDA-PILLA */
			0x0DD8,				/* SINHALA VOWEL SIGN GAETTA-PILLA */
			0x0DD9,				/* SINHALA VOWEL SIGN KOMBUVA */
			0x0DDA,				/* SINHALA VOWEL SIGN DIGA KOMBUVA */
			0x0DDB,				/* SINHALA VOWEL SIGN KOMBU DEKA */
			0x0DDC,				/* SINHALA VOWEL SIGN KOMBUVA HAA AELA-PILLA */
			0x0DDD,				/* SINHALA VOWEL SIGN KOMBUVA HAA DIGA
								 * AELA-PILLA */
			0x0DDE,				/* SINHALA VOWEL SIGN KOMBUVA HAA GAYANUKITTA */
			0x0DDF,				/* SINHALA VOWEL SIGN GAYANUKITTA */
			0x0DF2,				/* SINHALA VOWEL SIGN DIGA GAETTA-PILLA */
			0x0DF3,				/* SINHALA VOWEL SIGN DIGA GAYANUKITTA */
			0x0F3E,				/* TIBETAN SIGN YAR TSHES */
			0x0F3F,				/* TIBETAN SIGN MAR TSHES */
			0x0F7F,				/* TIBETAN SIGN RNAM BCAD */
			0x102B,				/* MYANMAR VOWEL SIGN TALL AA */
			0x102C,				/* MYANMAR VOWEL SIGN AA */
			0x1031,				/* MYANMAR VOWEL SIGN E */
			0x1038,				/* MYANMAR SIGN VISARGA */
			0x103B,				/* MYANMAR CONSONANT SIGN MEDIAL YA */
			0x103C,				/* MYANMAR CONSONANT SIGN MEDIAL RA */
			0x1056,				/* MYANMAR VOWEL SIGN VOCALIC R */
			0x1057,				/* MYANMAR VOWEL SIGN VOCALIC RR */
			0x1062,				/* MYANMAR VOWEL SIGN SGAW KAREN EU */
			0x1063,				/* MYANMAR TONE MARK SGAW KAREN HATHI */
			0x1064,				/* MYANMAR TONE MARK SGAW KAREN KE PHO */
			0x1067,				/* MYANMAR VOWEL SIGN WESTERN PWO KAREN EU */
			0x1068,				/* MYANMAR VOWEL SIGN WESTERN PWO KAREN UE */
			0x1069,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-1 */
			0x106A,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-2 */
			0x106B,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-3 */
			0x106C,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-4 */
			0x106D,				/* MYANMAR SIGN WESTERN PWO KAREN TONE-5 */
			0x1083,				/* MYANMAR VOWEL SIGN SHAN AA */
			0x1084,				/* MYANMAR VOWEL SIGN SHAN E */
			0x1087,				/* MYANMAR SIGN SHAN TONE-2 */
			0x1088,				/* MYANMAR SIGN SHAN TONE-3 */
			0x1089,				/* MYANMAR SIGN SHAN TONE-5 */
			0x108A,				/* MYANMAR SIGN SHAN TONE-6 */
			0x108B,				/* MYANMAR SIGN SHAN COUNCIL TONE-2 */
			0x108C,				/* MYANMAR SIGN SHAN COUNCIL TONE-3 */
			0x108F,				/* MYANMAR SIGN RUMAI PALAUNG TONE-5 */
			0x17B6,				/* KHMER VOWEL SIGN AA */
			0x17BE,				/* KHMER VOWEL SIGN OE */
			0x17BF,				/* KHMER VOWEL SIGN YA */
			0x17C0,				/* KHMER VOWEL SIGN IE */
			0x17C1,				/* KHMER VOWEL SIGN E */
			0x17C2,				/* KHMER VOWEL SIGN AE */
			0x17C3,				/* KHMER VOWEL SIGN AI */
			0x17C4,				/* KHMER VOWEL SIGN OO */
			0x17C5,				/* KHMER VOWEL SIGN AU */
			0x17C7,				/* KHMER SIGN REAHMUK */
			0x17C8,				/* KHMER SIGN YUUKALEAPINTU */
			0x1923,				/* LIMBU VOWEL SIGN EE */
			0x1924,				/* LIMBU VOWEL SIGN AI */
			0x1925,				/* LIMBU VOWEL SIGN OO */
			0x1926,				/* LIMBU VOWEL SIGN AU */
			0x1929,				/* LIMBU SUBJOINED LETTER YA */
			0x192A,				/* LIMBU SUBJOINED LETTER RA */
			0x192B,				/* LIMBU SUBJOINED LETTER WA */
			0x1930,				/* LIMBU SMALL LETTER KA */
			0x1931,				/* LIMBU SMALL LETTER NGA */
			0x1933,				/* LIMBU SMALL LETTER TA */
			0x1934,				/* LIMBU SMALL LETTER NA */
			0x1935,				/* LIMBU SMALL LETTER PA */
			0x1936,				/* LIMBU SMALL LETTER MA */
			0x1937,				/* LIMBU SMALL LETTER RA */
			0x1938,				/* LIMBU SMALL LETTER LA */
			0x19B0,				/* NEW TAI LUE VOWEL SIGN VOWEL SHORTENER */
			0x19B1,				/* NEW TAI LUE VOWEL SIGN AA */
			0x19B2,				/* NEW TAI LUE VOWEL SIGN II */
			0x19B3,				/* NEW TAI LUE VOWEL SIGN U */
			0x19B4,				/* NEW TAI LUE VOWEL SIGN UU */
			0x19B5,				/* NEW TAI LUE VOWEL SIGN E */
			0x19B6,				/* NEW TAI LUE VOWEL SIGN AE */
			0x19B7,				/* NEW TAI LUE VOWEL SIGN O */
			0x19B8,				/* NEW TAI LUE VOWEL SIGN OA */
			0x19B9,				/* NEW TAI LUE VOWEL SIGN UE */
			0x19BA,				/* NEW TAI LUE VOWEL SIGN AY */
			0x19BB,				/* NEW TAI LUE VOWEL SIGN AAY */
			0x19BC,				/* NEW TAI LUE VOWEL SIGN UY */
			0x19BD,				/* NEW TAI LUE VOWEL SIGN OY */
			0x19BE,				/* NEW TAI LUE VOWEL SIGN OAY */
			0x19BF,				/* NEW TAI LUE VOWEL SIGN UEY */
			0x19C0,				/* NEW TAI LUE VOWEL SIGN IY */
			0x19C8,				/* NEW TAI LUE TONE MARK-1 */
			0x19C9,				/* NEW TAI LUE TONE MARK-2 */
			0x1A19,				/* BUGINESE VOWEL SIGN E */
			0x1A1A,				/* BUGINESE VOWEL SIGN O */
			0x1A1B,				/* BUGINESE VOWEL SIGN AE */
			0x1B04,				/* BALINESE SIGN BISAH */
			0x1B35,				/* BALINESE VOWEL SIGN TEDUNG */
			0x1B3B,				/* BALINESE VOWEL SIGN RA REPA TEDUNG */
			0x1B3D,				/* BALINESE VOWEL SIGN LA LENGA TEDUNG */
			0x1B3E,				/* BALINESE VOWEL SIGN TALING */
			0x1B3F,				/* BALINESE VOWEL SIGN TALING REPA */
			0x1B40,				/* BALINESE VOWEL SIGN TALING TEDUNG */
			0x1B41,				/* BALINESE VOWEL SIGN TALING REPA TEDUNG */
			0x1B43,				/* BALINESE VOWEL SIGN PEPET TEDUNG */
			0x1B44,				/* BALINESE ADEG ADEG */
			0x1B82,				/* SUNDANESE SIGN PANGWISAD */
			0x1BA1,				/* SUNDANESE CONSONANT SIGN PAMINGKAL */
			0x1BA6,				/* SUNDANESE VOWEL SIGN PANAELAENG */
			0x1BA7,				/* SUNDANESE VOWEL SIGN PANOLONG */
			0x1BAA,				/* SUNDANESE SIGN PAMAAEH */
			0x1C24,				/* LEPCHA SUBJOINED LETTER YA */
			0x1C25,				/* LEPCHA SUBJOINED LETTER RA */
			0x1C26,				/* LEPCHA VOWEL SIGN AA */
			0x1C27,				/* LEPCHA VOWEL SIGN I */
			0x1C28,				/* LEPCHA VOWEL SIGN O */
			0x1C29,				/* LEPCHA VOWEL SIGN OO */
			0x1C2A,				/* LEPCHA VOWEL SIGN U */
			0x1C2B,				/* LEPCHA VOWEL SIGN UU */
			0x1C34,				/* LEPCHA CONSONANT SIGN NYIN-DO */
			0x1C35,				/* LEPCHA CONSONANT SIGN KANG */
			0xA823,				/* SYLOTI NAGRI VOWEL SIGN A */
			0xA824,				/* SYLOTI NAGRI VOWEL SIGN I */
			0xA827,				/* SYLOTI NAGRI VOWEL SIGN OO */
			0xA880,				/* SAURASHTRA SIGN ANUSVARA */
			0xA881,				/* SAURASHTRA SIGN VISARGA */
			0xA8B4,				/* SAURASHTRA CONSONANT SIGN HAARU */
			0xA8B5,				/* SAURASHTRA VOWEL SIGN AA */
			0xA8B6,				/* SAURASHTRA VOWEL SIGN I */
			0xA8B7,				/* SAURASHTRA VOWEL SIGN II */
			0xA8B8,				/* SAURASHTRA VOWEL SIGN U */
			0xA8B9,				/* SAURASHTRA VOWEL SIGN UU */
			0xA8BA,				/* SAURASHTRA VOWEL SIGN VOCALIC R */
			0xA8BB,				/* SAURASHTRA VOWEL SIGN VOCALIC RR */
			0xA8BC,				/* SAURASHTRA VOWEL SIGN VOCALIC L */
			0xA8BD,				/* SAURASHTRA VOWEL SIGN VOCALIC LL */
			0xA8BE,				/* SAURASHTRA VOWEL SIGN E */
			0xA8BF,				/* SAURASHTRA VOWEL SIGN EE */
			0xA8C0,				/* SAURASHTRA VOWEL SIGN AI */
			0xA8C1,				/* SAURASHTRA VOWEL SIGN O */
			0xA8C2,				/* SAURASHTRA VOWEL SIGN OO */
			0xA8C3,				/* SAURASHTRA VOWEL SIGN AU */
			0xA952,				/* REJANG CONSONANT SIGN H */
			0xA953,				/* REJANG VIRAMA */
			0xAA2F,				/* CHAM VOWEL SIGN O */
			0xAA30,				/* CHAM VOWEL SIGN AI */
			0xAA33,				/* CHAM CONSONANT SIGN YA */
			0xAA34,				/* CHAM CONSONANT SIGN RA */
			0xAA4D				/* CHAM CONSONANT SIGN FINAL H */
		};
		const pg_wchar *StopLow = strange_letter,
				   *StopHigh = strange_letter + lengthof(strange_letter),
				   *StopMiddle;
		pg_wchar	c;

		if (prs->pgwstr)
			c = *(prs->pgwstr + prs->state->poschar);
		else
			c = (pg_wchar) *(prs->wstr + prs->state->poschar);

		while (StopLow < StopHigh)
		{
			StopMiddle = StopLow + ((StopHigh - StopLow) >> 1);
			if (*StopMiddle == c)
				return 1;
			else if (*StopMiddle < c)
				StopLow = StopMiddle + 1;
			else
				StopHigh = StopMiddle;
		}
	}

	return 0;
}

/*
 * Table of state/action of parser
 */

static const TParserStateActionItem actionTPS_Base[] = {
	{p_isEOF, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '<', A_PUSH, TPS_InTagFirst, 0, NULL},
	{p_isignore, 0, A_NEXT, TPS_InSpace, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InAsciiWord, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InUnsignedInt, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InSignedIntFirst, 0, NULL},
	{p_iseqC, '+', A_PUSH, TPS_InSignedIntFirst, 0, NULL},
	{p_iseqC, '&', A_PUSH, TPS_InXMLEntityFirst, 0, NULL},
	{p_iseqC, '~', A_PUSH, TPS_InFileTwiddle, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InPathFirstFirst, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InSpace, 0, NULL}
};


static const TParserStateActionItem actionTPS_InNumWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, NUMWORD, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenNumWordFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, NUMWORD, NULL}
};

static const TParserStateActionItem actionTPS_InAsciiWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, ASCIIWORD, NULL},
	{p_isasclet, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenAsciiWordFirst, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, ':', A_PUSH, TPS_InProtocolFirst, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InWord, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, ASCIIWORD, NULL}
};

static const TParserStateActionItem actionTPS_InWord[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, WORD_T, NULL},
	{p_isalpha, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenWordFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, WORD_T, NULL}
};

static const TParserStateActionItem actionTPS_InUnsignedInt[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, UNSIGNEDINT, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InUDecimalFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_isasclet, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InNumWord, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, UNSIGNEDINT, NULL}
};

static const TParserStateActionItem actionTPS_InSignedIntFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT | A_CLEAR, TPS_InSignedInt, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InSignedInt[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, SIGNEDINT, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InDecimalFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, SIGNEDINT, NULL}
};

static const TParserStateActionItem actionTPS_InSpace[] = {
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

static const TParserStateActionItem actionTPS_InUDecimalFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InUDecimal, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InUDecimal[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, DECIMAL_T, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InUDecimal, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVersionFirst, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, DECIMAL_T, NULL}
};

static const TParserStateActionItem actionTPS_InDecimalFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InDecimal, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InDecimal[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, DECIMAL_T, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InDecimal, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVerVersion, 0, NULL},
	{p_iseqC, 'e', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{p_iseqC, 'E', A_PUSH, TPS_InMantissaFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, DECIMAL_T, NULL}
};

static const TParserStateActionItem actionTPS_InVerVersion[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_RERUN, TPS_InSVerVersion, 0, SpecialVerVersion},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InSVerVersion[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_BINGO | A_CLRALL, TPS_InUnsignedInt, SPACE, NULL},
	{NULL, 0, A_NEXT, TPS_Null, 0, NULL}
};


static const TParserStateActionItem actionTPS_InVersionFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InVersion, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InVersion[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, VERSIONNUMBER, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InVersion, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InVersionFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, VERSIONNUMBER, NULL}
};

static const TParserStateActionItem actionTPS_InMantissaFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InMantissa, 0, NULL},
	{p_iseqC, '+', A_NEXT, TPS_InMantissaSign, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InMantissaSign, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InMantissaSign[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InMantissa, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InMantissa[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, SCIENTIFIC, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InMantissa, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, SCIENTIFIC, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '#', A_NEXT, TPS_InXMLEntityNumFirst, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, ':', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntity[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, ':', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InXMLEntity, 0, NULL},
	{p_iseqC, ';', A_NEXT, TPS_InXMLEntityEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityNumFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, 'x', A_NEXT, TPS_InXMLEntityHexNumFirst, 0, NULL},
	{p_iseqC, 'X', A_NEXT, TPS_InXMLEntityHexNumFirst, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InXMLEntityNum, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityHexNumFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isxdigit, 0, A_NEXT, TPS_InXMLEntityHexNum, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityNum[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InXMLEntityNum, 0, NULL},
	{p_iseqC, ';', A_NEXT, TPS_InXMLEntityEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityHexNum[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isxdigit, 0, A_NEXT, TPS_InXMLEntityHexNum, 0, NULL},
	{p_iseqC, ';', A_NEXT, TPS_InXMLEntityEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLEntityEnd[] = {
	{NULL, 0, A_BINGO | A_CLEAR, TPS_Base, XMLENTITY, NULL}
};

static const TParserStateActionItem actionTPS_InTagFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InTagCloseFirst, 0, NULL},
	{p_iseqC, '!', A_PUSH, TPS_InCommentFirst, 0, NULL},
	{p_iseqC, '?', A_PUSH, TPS_InXMLBegin, 0, NULL},
	{p_isasclet, 0, A_PUSH, TPS_InTagName, 0, NULL},
	{p_iseqC, ':', A_PUSH, TPS_InTagName, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InTagName, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InXMLBegin[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	/* <?xml ... */
	/* XXX do we wants states for the m and l ?  Right now this accepts <?xZ */
	{p_iseqC, 'x', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagCloseFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InTagName, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagName[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	/* <br/> case */
	{p_iseqC, '/', A_NEXT, TPS_InTagBeginEnd, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, SpecialTags},
	{p_isspace, 0, A_NEXT, TPS_InTag, 0, SpecialTags},
	{p_isalnum, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, ':', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_Null, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagBeginEnd[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTag[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InTagEnd, 0, SpecialTags},
	{p_iseqC, '\'', A_NEXT, TPS_InTagEscapeK, 0, NULL},
	{p_iseqC, '"', A_NEXT, TPS_InTagEscapeKK, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '=', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_Null, 0, NULL},
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

static const TParserStateActionItem actionTPS_InTagEscapeK[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '\\', A_PUSH, TPS_InTagBackSleshed, 0, NULL},
	{p_iseqC, '\'', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InTagEscapeK, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagEscapeKK[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '\\', A_PUSH, TPS_InTagBackSleshed, 0, NULL},
	{p_iseqC, '"', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InTagEscapeKK, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagBackSleshed[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{NULL, 0, A_MERGE, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InTagEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, TAG_T, NULL}
};

static const TParserStateActionItem actionTPS_InCommentFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCommentLast, 0, NULL},
	/* <!DOCTYPE ...> */
	{p_iseqC, 'D', A_NEXT, TPS_InTag, 0, NULL},
	{p_iseqC, 'd', A_NEXT, TPS_InTag, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InCommentLast[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InComment, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InComment[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCloseCommentFirst, 0, NULL},
	{NULL, 0, A_NEXT, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InCloseCommentFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InCloseCommentLast, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InComment, 0, NULL}
};

static const TParserStateActionItem actionTPS_InCloseCommentLast[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_Null, 0, NULL},
	{p_iseqC, '>', A_NEXT, TPS_InCommentEnd, 0, NULL},
	{NULL, 0, A_NEXT, TPS_InComment, 0, NULL}
};

static const TParserStateActionItem actionTPS_InCommentEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, TAG_T, NULL}
};

static const TParserStateActionItem actionTPS_InHostFirstDomain[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHostDomainSecond, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHostDomainSecond[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHostDomain, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHostDomain[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHostDomain, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHost, 0, NULL},
	{p_iseqC, ':', A_PUSH, TPS_InPortFirst, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_isdigit, 0, A_POP, TPS_Null, 0, NULL},
	{p_isstophost, 0, A_BINGO | A_CLRALL, TPS_InURLPathStart, HOST, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFURL, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL}
};

static const TParserStateActionItem actionTPS_InPortFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InPort, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InPort[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InPort, 0, NULL},
	{p_isstophost, 0, A_BINGO | A_CLRALL, TPS_InURLPathStart, HOST, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFURL, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, HOST, NULL}
};

static const TParserStateActionItem actionTPS_InHostFirstAN[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHost, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHost[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHost, 0, NULL},
	{p_iseqC, '@', A_PUSH, TPS_InEmail, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InHostFirstDomain, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{p_iseqC, '_', A_PUSH, TPS_InHostFirstAN, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InEmail[] = {
	{p_isstophost, 0, A_POP, TPS_Null, 0, NULL},
	{p_ishost, 0, A_BINGO | A_CLRALL, TPS_Base, EMAIL, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InFileFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathFirst, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '~', A_PUSH, TPS_InFileTwiddle, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InFileTwiddle[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InPathFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathSecond, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InPathFirstFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '.', A_NEXT, TPS_InPathSecond, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InPathSecond[] = {
	{p_isEOF, 0, A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{p_iseqC, '/', A_NEXT | A_PUSH, TPS_InFileFirst, 0, NULL},
	{p_iseqC, '/', A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{p_isspace, 0, A_BINGO | A_CLEAR, TPS_Base, FILEPATH, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InFile[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, FILEPATH, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '.', A_PUSH, TPS_InFileNext, 0, NULL},
	{p_iseqC, '_', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '-', A_NEXT, TPS_InFile, 0, NULL},
	{p_iseqC, '/', A_PUSH, TPS_InFileFirst, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, FILEPATH, NULL}
};

static const TParserStateActionItem actionTPS_InFileNext[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_CLEAR, TPS_InFile, 0, NULL},
	{p_isdigit, 0, A_CLEAR, TPS_InFile, 0, NULL},
	{p_iseqC, '_', A_CLEAR, TPS_InFile, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InURLPathFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isurlchar, 0, A_NEXT, TPS_InURLPath, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL},
};

static const TParserStateActionItem actionTPS_InURLPathStart[] = {
	{NULL, 0, A_NEXT, TPS_InURLPath, 0, NULL}
};

static const TParserStateActionItem actionTPS_InURLPath[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, URLPATH, NULL},
	{p_isurlchar, 0, A_NEXT, TPS_InURLPath, 0, NULL},
	{NULL, 0, A_BINGO, TPS_Base, URLPATH, NULL}
};

static const TParserStateActionItem actionTPS_InFURL[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isURLPath, 0, A_BINGO | A_CLRALL, TPS_Base, URL_T, SpecialFURL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InProtocolFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InProtocolSecond, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InProtocolSecond[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_iseqC, '/', A_NEXT, TPS_InProtocolEnd, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InProtocolEnd[] = {
	{NULL, 0, A_BINGO | A_CLRALL, TPS_Base, PROTOCOL, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenAsciiWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHyphenAsciiWord, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenDigitLookahead, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenAsciiWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, ASCIIHWORD, SpecialHyphen},
	{p_isasclet, 0, A_NEXT, TPS_InHyphenAsciiWord, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenAsciiWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, ASCIIHWORD, SpecialHyphen}
};

static const TParserStateActionItem actionTPS_InHyphenWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenDigitLookahead, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HWORD, SpecialHyphen},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, HWORD, SpecialHyphen}
};

static const TParserStateActionItem actionTPS_InHyphenNumWordFirst[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenDigitLookahead, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenNumWord[] = {
	{p_isEOF, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, NUMHWORD, SpecialHyphen},
	{p_isalnum, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InHyphenNumWordFirst, 0, NULL},
	{NULL, 0, A_BINGO | A_CLRALL, TPS_InParseHyphen, NUMHWORD, SpecialHyphen}
};

static const TParserStateActionItem actionTPS_InHyphenDigitLookahead[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenDigitLookahead, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenNumWord, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InParseHyphen[] = {
	{p_isEOF, 0, A_RERUN, TPS_Base, 0, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHyphenAsciiWordPart, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isdigit, 0, A_PUSH, TPS_InHyphenUnsignedInt, 0, NULL},
	{p_iseqC, '-', A_PUSH, TPS_InParseHyphenHyphen, 0, NULL},
	{NULL, 0, A_RERUN, TPS_Base, 0, NULL}
};

static const TParserStateActionItem actionTPS_InParseHyphenHyphen[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isalnum, 0, A_BINGO | A_CLEAR, TPS_InParseHyphen, SPACE, NULL},
	{p_isspecial, 0, A_BINGO | A_CLEAR, TPS_InParseHyphen, SPACE, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, PARTHWORD, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenNumWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, PARTHWORD, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenAsciiWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, ASCIIPARTHWORD, NULL},
	{p_isasclet, 0, A_NEXT, TPS_InHyphenAsciiWordPart, 0, NULL},
	{p_isalpha, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenWordPart, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_InHyphenNumWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, ASCIIPARTHWORD, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenNumWordPart[] = {
	{p_isEOF, 0, A_BINGO, TPS_Base, NUMPARTHWORD, NULL},
	{p_isalnum, 0, A_NEXT, TPS_InHyphenNumWordPart, 0, NULL},
	{p_isspecial, 0, A_NEXT, TPS_InHyphenNumWordPart, 0, NULL},
	{NULL, 0, A_BINGO, TPS_InParseHyphen, NUMPARTHWORD, NULL}
};

static const TParserStateActionItem actionTPS_InHyphenUnsignedInt[] = {
	{p_isEOF, 0, A_POP, TPS_Null, 0, NULL},
	{p_isdigit, 0, A_NEXT, TPS_Null, 0, NULL},
	{p_isalpha, 0, A_CLEAR, TPS_InHyphenNumWordPart, 0, NULL},
	{p_isspecial, 0, A_CLEAR, TPS_InHyphenNumWordPart, 0, NULL},
	{NULL, 0, A_POP, TPS_Null, 0, NULL}
};


/*
 * main table of per-state parser actions
 */
typedef struct
{
	const TParserStateActionItem *action;	/* the actual state info */
	TParserState state;			/* only for Assert crosscheck */
#ifdef WPARSER_TRACE
	const char *state_name;		/* only for debug printout */
#endif
} TParserStateAction;

#ifdef WPARSER_TRACE
#define TPARSERSTATEACTION(state) \
	{ CppConcat(action,state), state, CppAsString(state) }
#else
#define TPARSERSTATEACTION(state) \
	{ CppConcat(action,state), state }
#endif

/*
 * order must be the same as in typedef enum {} TParserState!!
 */

static const TParserStateAction Actions[] = {
	TPARSERSTATEACTION(TPS_Base),
	TPARSERSTATEACTION(TPS_InNumWord),
	TPARSERSTATEACTION(TPS_InAsciiWord),
	TPARSERSTATEACTION(TPS_InWord),
	TPARSERSTATEACTION(TPS_InUnsignedInt),
	TPARSERSTATEACTION(TPS_InSignedIntFirst),
	TPARSERSTATEACTION(TPS_InSignedInt),
	TPARSERSTATEACTION(TPS_InSpace),
	TPARSERSTATEACTION(TPS_InUDecimalFirst),
	TPARSERSTATEACTION(TPS_InUDecimal),
	TPARSERSTATEACTION(TPS_InDecimalFirst),
	TPARSERSTATEACTION(TPS_InDecimal),
	TPARSERSTATEACTION(TPS_InVerVersion),
	TPARSERSTATEACTION(TPS_InSVerVersion),
	TPARSERSTATEACTION(TPS_InVersionFirst),
	TPARSERSTATEACTION(TPS_InVersion),
	TPARSERSTATEACTION(TPS_InMantissaFirst),
	TPARSERSTATEACTION(TPS_InMantissaSign),
	TPARSERSTATEACTION(TPS_InMantissa),
	TPARSERSTATEACTION(TPS_InXMLEntityFirst),
	TPARSERSTATEACTION(TPS_InXMLEntity),
	TPARSERSTATEACTION(TPS_InXMLEntityNumFirst),
	TPARSERSTATEACTION(TPS_InXMLEntityNum),
	TPARSERSTATEACTION(TPS_InXMLEntityHexNumFirst),
	TPARSERSTATEACTION(TPS_InXMLEntityHexNum),
	TPARSERSTATEACTION(TPS_InXMLEntityEnd),
	TPARSERSTATEACTION(TPS_InTagFirst),
	TPARSERSTATEACTION(TPS_InXMLBegin),
	TPARSERSTATEACTION(TPS_InTagCloseFirst),
	TPARSERSTATEACTION(TPS_InTagName),
	TPARSERSTATEACTION(TPS_InTagBeginEnd),
	TPARSERSTATEACTION(TPS_InTag),
	TPARSERSTATEACTION(TPS_InTagEscapeK),
	TPARSERSTATEACTION(TPS_InTagEscapeKK),
	TPARSERSTATEACTION(TPS_InTagBackSleshed),
	TPARSERSTATEACTION(TPS_InTagEnd),
	TPARSERSTATEACTION(TPS_InCommentFirst),
	TPARSERSTATEACTION(TPS_InCommentLast),
	TPARSERSTATEACTION(TPS_InComment),
	TPARSERSTATEACTION(TPS_InCloseCommentFirst),
	TPARSERSTATEACTION(TPS_InCloseCommentLast),
	TPARSERSTATEACTION(TPS_InCommentEnd),
	TPARSERSTATEACTION(TPS_InHostFirstDomain),
	TPARSERSTATEACTION(TPS_InHostDomainSecond),
	TPARSERSTATEACTION(TPS_InHostDomain),
	TPARSERSTATEACTION(TPS_InPortFirst),
	TPARSERSTATEACTION(TPS_InPort),
	TPARSERSTATEACTION(TPS_InHostFirstAN),
	TPARSERSTATEACTION(TPS_InHost),
	TPARSERSTATEACTION(TPS_InEmail),
	TPARSERSTATEACTION(TPS_InFileFirst),
	TPARSERSTATEACTION(TPS_InFileTwiddle),
	TPARSERSTATEACTION(TPS_InPathFirst),
	TPARSERSTATEACTION(TPS_InPathFirstFirst),
	TPARSERSTATEACTION(TPS_InPathSecond),
	TPARSERSTATEACTION(TPS_InFile),
	TPARSERSTATEACTION(TPS_InFileNext),
	TPARSERSTATEACTION(TPS_InURLPathFirst),
	TPARSERSTATEACTION(TPS_InURLPathStart),
	TPARSERSTATEACTION(TPS_InURLPath),
	TPARSERSTATEACTION(TPS_InFURL),
	TPARSERSTATEACTION(TPS_InProtocolFirst),
	TPARSERSTATEACTION(TPS_InProtocolSecond),
	TPARSERSTATEACTION(TPS_InProtocolEnd),
	TPARSERSTATEACTION(TPS_InHyphenAsciiWordFirst),
	TPARSERSTATEACTION(TPS_InHyphenAsciiWord),
	TPARSERSTATEACTION(TPS_InHyphenWordFirst),
	TPARSERSTATEACTION(TPS_InHyphenWord),
	TPARSERSTATEACTION(TPS_InHyphenNumWordFirst),
	TPARSERSTATEACTION(TPS_InHyphenNumWord),
	TPARSERSTATEACTION(TPS_InHyphenDigitLookahead),
	TPARSERSTATEACTION(TPS_InParseHyphen),
	TPARSERSTATEACTION(TPS_InParseHyphenHyphen),
	TPARSERSTATEACTION(TPS_InHyphenWordPart),
	TPARSERSTATEACTION(TPS_InHyphenAsciiWordPart),
	TPARSERSTATEACTION(TPS_InHyphenNumWordPart),
	TPARSERSTATEACTION(TPS_InHyphenUnsignedInt)
};


static bool
TParserGet(TParser *prs)
{
	const TParserStateActionItem *item = NULL;

	Assert(prs->state);

	if (prs->state->posbyte >= prs->lenstr)
		return false;

	prs->token = prs->str + prs->state->posbyte;
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

		if (prs->state->pushedAtAction)
		{
			/* After a POP, pick up at the next test */
			item = prs->state->pushedAtAction + 1;
			prs->state->pushedAtAction = NULL;
		}
		else
		{
			item = Actions[prs->state->state].action;
			Assert(item != NULL);
		}

		/* find action by character class */
		while (item->isclass)
		{
			prs->c = item->c;
			if (item->isclass(prs) != 0)
				break;
			item++;
		}

#ifdef WPARSER_TRACE
		{
			TParserPosition *ptr;

			fprintf(stderr, "state ");
			/* indent according to stack depth */
			for (ptr = prs->state->prev; ptr; ptr = ptr->prev)
				fprintf(stderr, "  ");
			fprintf(stderr, "%s ", Actions[prs->state->state].state_name);
			if (prs->state->posbyte < prs->lenstr)
				fprintf(stderr, "at %c", *(prs->str + prs->state->posbyte));
			else
				fprintf(stderr, "at EOF");
			fprintf(stderr, " matched rule %d flags%s%s%s%s%s%s%s%s%s%s%s\n",
					(int) (item - Actions[prs->state->state].action),
					(item->flags & A_BINGO) ? " BINGO" : "",
					(item->flags & A_POP) ? " POP" : "",
					(item->flags & A_PUSH) ? " PUSH" : "",
					(item->flags & A_RERUN) ? " RERUN" : "",
					(item->flags & A_CLEAR) ? " CLEAR" : "",
					(item->flags & A_MERGE) ? " MERGE" : "",
					(item->flags & A_CLRALL) ? " CLRALL" : "",
					(item->tostate != TPS_Null) ? " tostate " : "",
					(item->tostate != TPS_Null) ? Actions[item->tostate].state_name : "",
					(item->type > 0) ? " type " : "",
					tok_alias[item->type]);
		}
#endif

		/* call special handler if exists */
		if (item->special)
			item->special(prs);

		/* BINGO, token is found */
		if (item->flags & A_BINGO)
		{
			Assert(item->type > 0);
			prs->lenbytetoken = prs->state->lenbytetoken;
			prs->lenchartoken = prs->state->lenchartoken;
			prs->state->lenbytetoken = prs->state->lenchartoken = 0;
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
			prs->state->lenbytetoken = ptr->lenbytetoken;
			prs->state->lenchartoken = ptr->lenchartoken;
			pfree(ptr);
		}

		/* set new state if pointed */
		if (item->tostate != TPS_Null)
			prs->state->state = item->tostate;

		/* check for go away */
		if ((item->flags & A_BINGO) ||
			(prs->state->posbyte >= prs->lenstr &&
			 (item->flags & A_RERUN) == 0))
			break;

		/* go to beginning of loop if we should rerun or we just restore state */
		if (item->flags & (A_RERUN | A_POP))
			continue;

		/* move forward */
		if (prs->state->charlen)
		{
			prs->state->posbyte += prs->state->charlen;
			prs->state->lenbytetoken += prs->state->charlen;
			prs->state->poschar++;
			prs->state->lenchartoken++;
		}
	}

	return (item && (item->flags & A_BINGO)) ? true : false;
}

Datum
prsd_lextype(PG_FUNCTION_ARGS)
{
	LexDescr   *descr = (LexDescr *) palloc(sizeof(LexDescr) * (LASTNUM + 1));
	int			i;

	for (i = 1; i <= LASTNUM; i++)
	{
		descr[i - 1].lexid = i;
		descr[i - 1].alias = pstrdup(tok_alias[i]);
		descr[i - 1].descr = pstrdup(lex_descr[i]);
	}

	descr[LASTNUM].lexid = 0;

	PG_RETURN_POINTER(descr);
}

Datum
prsd_start(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(TParserInit((char *) PG_GETARG_POINTER(0), PG_GETARG_INT32(1)));
}

Datum
prsd_nexttoken(PG_FUNCTION_ARGS)
{
	TParser    *p = (TParser *) PG_GETARG_POINTER(0);
	char	  **t = (char **) PG_GETARG_POINTER(1);
	int		   *tlen = (int *) PG_GETARG_POINTER(2);

	if (!TParserGet(p))
		PG_RETURN_INT32(0);

	*t = p->token;
	*tlen = p->lenbytetoken;

	PG_RETURN_INT32(p->type);
}

Datum
prsd_end(PG_FUNCTION_ARGS)
{
	TParser    *p = (TParser *) PG_GETARG_POINTER(0);

	TParserClose(p);
	PG_RETURN_VOID();
}


/*
 * ts_headline support begins here
 */

/* token type classification macros */
#define LEAVETOKEN(x)	( (x)==SPACE )
#define COMPLEXTOKEN(x) ( (x)==URL_T || (x)==NUMHWORD || (x)==ASCIIHWORD || (x)==HWORD )
#define ENDPUNCTOKEN(x) ( (x)==SPACE )

#define TS_IDIGNORE(x)	( (x)==TAG_T || (x)==PROTOCOL || (x)==SPACE || (x)==XMLENTITY )
#define HLIDREPLACE(x)	( (x)==TAG_T )
#define HLIDSKIP(x)		( (x)==URL_T || (x)==NUMHWORD || (x)==ASCIIHWORD || (x)==HWORD )
#define XMLHLIDSKIP(x)	( (x)==URL_T || (x)==NUMHWORD || (x)==ASCIIHWORD || (x)==HWORD )
#define NONWORDTOKEN(x) ( (x)==SPACE || HLIDREPLACE(x) || HLIDSKIP(x) )
#define NOENDTOKEN(x)	( NONWORDTOKEN(x) || (x)==SCIENTIFIC || (x)==VERSIONNUMBER || (x)==DECIMAL_T || (x)==SIGNEDINT || (x)==UNSIGNEDINT || TS_IDIGNORE(x) )

/*
 * Macros useful in headline selection.  These rely on availability of
 * "HeadlineParsedText *prs" describing some text, and "int shortword"
 * describing the "short word" length parameter.
 */

/* Interesting words are non-repeated search terms */
#define INTERESTINGWORD(j) \
	(prs->words[j].item && !prs->words[j].repeated)

/* Don't want to end at a non-word or a short word, unless interesting */
#define BADENDPOINT(j) \
	((NOENDTOKEN(prs->words[j].type) || prs->words[j].len <= shortword) && \
	 !INTERESTINGWORD(j))

typedef struct
{
	/* one cover (well, really one fragment) for mark_hl_fragments */
	int32		startpos;		/* fragment's starting word index */
	int32		endpos;			/* ending word index (inclusive) */
	int32		poslen;			/* number of interesting words */
	int32		curlen;			/* total number of words */
	bool		chosen;			/* chosen? */
	bool		excluded;		/* excluded? */
} CoverPos;

typedef struct
{
	/* callback data for checkcondition_HL */
	HeadlineWordEntry *words;
	int			len;
} hlCheck;


/*
 * TS_execute callback for matching a tsquery operand to headline words
 */
static TSTernaryValue
checkcondition_HL(void *opaque, QueryOperand *val, ExecPhraseData *data)
{
	hlCheck    *checkval = (hlCheck *) opaque;
	int			i;

	/* scan words array for marching items */
	for (i = 0; i < checkval->len; i++)
	{
		if (checkval->words[i].item == val)
		{
			/* if data == NULL, don't need to report positions */
			if (!data)
				return TS_YES;

			if (!data->pos)
			{
				data->pos = palloc(sizeof(WordEntryPos) * checkval->len);
				data->allocated = true;
				data->npos = 1;
				data->pos[0] = checkval->words[i].pos;
			}
			else if (data->pos[data->npos - 1] < checkval->words[i].pos)
			{
				data->pos[data->npos++] = checkval->words[i].pos;
			}
		}
	}

	if (data && data->npos > 0)
		return TS_YES;

	return TS_NO;
}

/*
 * hlFirstIndex: find first index >= pos containing any word used in query
 *
 * Returns -1 if no such index
 */
static int
hlFirstIndex(HeadlineParsedText *prs, int pos)
{
	int			i;

	for (i = pos; i < prs->curwords; i++)
	{
		if (prs->words[i].item != NULL)
			return i;
	}
	return -1;
}

/*
 * hlCover: try to find a substring of prs' word list that satisfies query
 *
 * At entry, *p must be the first word index to consider (initialize this
 * to zero, or to the next index after a previous successful search).
 * We will consider all substrings starting at or after that word, and
 * containing no more than max_cover words.  (We need a length limit to
 * keep this from taking O(N^2) time for a long document with many query
 * words but few complete matches.  Actually, since checkcondition_HL is
 * roughly O(N) in the length of the substring being checked, it's even
 * worse than that.)
 *
 * On success, sets *p to first word index and *q to last word index of the
 * cover substring, and returns true.
 *
 * The result is a minimal cover, in the sense that both *p and *q will be
 * words used in the query.
 */
static bool
hlCover(HeadlineParsedText *prs, TSQuery query, int max_cover,
		int *p, int *q)
{
	int			pmin,
				pmax,
				nextpmin,
				nextpmax;
	hlCheck		ch;

	/*
	 * We look for the earliest, shortest substring of prs->words that
	 * satisfies the query.  Both the pmin and pmax indices must be words
	 * appearing in the query; there's no point in trying endpoints in between
	 * such points.
	 */
	pmin = hlFirstIndex(prs, *p);
	while (pmin >= 0)
	{
		/* This useless assignment just keeps stupider compilers quiet */
		nextpmin = -1;
		/* Consider substrings starting at pmin */
		ch.words = &(prs->words[pmin]);
		/* Consider the length-one substring first, then longer substrings */
		pmax = pmin;
		do
		{
			/* Try to match query against pmin .. pmax substring */
			ch.len = pmax - pmin + 1;
			if (TS_execute(GETQUERY(query), &ch,
						   TS_EXEC_EMPTY, checkcondition_HL))
			{
				*p = pmin;
				*q = pmax;
				return true;
			}
			/* Nope, so advance pmax to next feasible endpoint */
			nextpmax = hlFirstIndex(prs, pmax + 1);

			/*
			 * If this is our first advance past pmin, then the result is also
			 * the next feasible value of pmin; remember it to save a
			 * redundant search.
			 */
			if (pmax == pmin)
				nextpmin = nextpmax;
			pmax = nextpmax;
		}
		while (pmax >= 0 && pmax - pmin < max_cover);
		/* No luck here, so try next feasible startpoint */
		pmin = nextpmin;
	}
	return false;
}

/*
 * Apply suitable highlight marking to words selected by headline selector
 *
 * The words from startpos to endpos inclusive are marked per highlightall
 */
static void
mark_fragment(HeadlineParsedText *prs, bool highlightall,
			  int startpos, int endpos)
{
	int			i;

	for (i = startpos; i <= endpos; i++)
	{
		if (prs->words[i].item)
			prs->words[i].selected = 1;
		if (!highlightall)
		{
			if (HLIDREPLACE(prs->words[i].type))
				prs->words[i].replace = 1;
			else if (HLIDSKIP(prs->words[i].type))
				prs->words[i].skip = 1;
		}
		else
		{
			if (XMLHLIDSKIP(prs->words[i].type))
				prs->words[i].skip = 1;
		}

		prs->words[i].in = (prs->words[i].repeated) ? 0 : 1;
	}
}

/*
 * split a cover substring into fragments not longer than max_words
 *
 * At entry, *startpos and *endpos are the (remaining) bounds of the cover
 * substring.  They are updated to hold the bounds of the next fragment.
 *
 * *curlen and *poslen are set to the fragment's length, in words and
 * interesting words respectively.
 */
static void
get_next_fragment(HeadlineParsedText *prs, int *startpos, int *endpos,
				  int *curlen, int *poslen, int max_words)
{
	int			i;

	/*
	 * Objective: select a fragment of words between startpos and endpos such
	 * that it has at most max_words and both ends have query words. If the
	 * startpos and endpos are the endpoints of the cover and the cover has
	 * fewer words than max_words, then this function should just return the
	 * cover
	 */
	/* first move startpos to an item */
	for (i = *startpos; i <= *endpos; i++)
	{
		*startpos = i;
		if (INTERESTINGWORD(i))
			break;
	}
	/* cut endpos to have only max_words */
	*curlen = 0;
	*poslen = 0;
	for (i = *startpos; i <= *endpos && *curlen < max_words; i++)
	{
		if (!NONWORDTOKEN(prs->words[i].type))
			*curlen += 1;
		if (INTERESTINGWORD(i))
			*poslen += 1;
	}
	/* if the cover was cut then move back endpos to a query item */
	if (*endpos > i)
	{
		*endpos = i;
		for (i = *endpos; i >= *startpos; i--)
		{
			*endpos = i;
			if (INTERESTINGWORD(i))
				break;
			if (!NONWORDTOKEN(prs->words[i].type))
				*curlen -= 1;
		}
	}
}

/*
 * Headline selector used when MaxFragments > 0
 *
 * Note: in this mode, highlightall is disregarded for phrase selection;
 * it only controls presentation details.
 */
static void
mark_hl_fragments(HeadlineParsedText *prs, TSQuery query, bool highlightall,
				  int shortword, int min_words,
				  int max_words, int max_fragments, int max_cover)
{
	int32		poslen,
				curlen,
				i,
				f,
				num_f = 0;
	int32		stretch,
				maxstretch,
				posmarker;

	int32		startpos = 0,
				endpos = 0,
				p = 0,
				q = 0;

	int32		numcovers = 0,
				maxcovers = 32;

	int32		minI,
				minwords,
				maxitems;
	CoverPos   *covers;

	covers = palloc(maxcovers * sizeof(CoverPos));

	/* get all covers */
	while (hlCover(prs, query, max_cover, &p, &q))
	{
		startpos = p;
		endpos = q;

		/*
		 * Break the cover into smaller fragments such that each fragment has
		 * at most max_words. Also ensure that each end of each fragment is a
		 * query word. This will allow us to stretch the fragment in either
		 * direction
		 */

		while (startpos <= endpos)
		{
			get_next_fragment(prs, &startpos, &endpos, &curlen, &poslen, max_words);
			if (numcovers >= maxcovers)
			{
				maxcovers *= 2;
				covers = repalloc(covers, sizeof(CoverPos) * maxcovers);
			}
			covers[numcovers].startpos = startpos;
			covers[numcovers].endpos = endpos;
			covers[numcovers].curlen = curlen;
			covers[numcovers].poslen = poslen;
			covers[numcovers].chosen = false;
			covers[numcovers].excluded = false;
			numcovers++;
			startpos = endpos + 1;
			endpos = q;
		}

		/* move p to generate the next cover */
		p++;
	}

	/* choose best covers */
	for (f = 0; f < max_fragments; f++)
	{
		maxitems = 0;
		minwords = PG_INT32_MAX;
		minI = -1;

		/*
		 * Choose the cover that contains max items. In case of tie choose the
		 * one with smaller number of words.
		 */
		for (i = 0; i < numcovers; i++)
		{
			if (!covers[i].chosen && !covers[i].excluded &&
				(maxitems < covers[i].poslen ||
				 (maxitems == covers[i].poslen &&
				  minwords > covers[i].curlen)))
			{
				maxitems = covers[i].poslen;
				minwords = covers[i].curlen;
				minI = i;
			}
		}
		/* if a cover was found mark it */
		if (minI >= 0)
		{
			covers[minI].chosen = true;
			/* adjust the size of cover */
			startpos = covers[minI].startpos;
			endpos = covers[minI].endpos;
			curlen = covers[minI].curlen;
			/* stretch the cover if cover size is lower than max_words */
			if (curlen < max_words)
			{
				/* divide the stretch on both sides of cover */
				maxstretch = (max_words - curlen) / 2;

				/*
				 * first stretch the startpos stop stretching if 1. we hit the
				 * beginning of document 2. exceed maxstretch 3. we hit an
				 * already marked fragment
				 */
				stretch = 0;
				posmarker = startpos;
				for (i = startpos - 1; i >= 0 && stretch < maxstretch && !prs->words[i].in; i--)
				{
					if (!NONWORDTOKEN(prs->words[i].type))
					{
						curlen++;
						stretch++;
					}
					posmarker = i;
				}
				/* cut back startpos till we find a good endpoint */
				for (i = posmarker; i < startpos && BADENDPOINT(i); i++)
				{
					if (!NONWORDTOKEN(prs->words[i].type))
						curlen--;
				}
				startpos = i;
				/* now stretch the endpos as much as possible */
				posmarker = endpos;
				for (i = endpos + 1; i < prs->curwords && curlen < max_words && !prs->words[i].in; i++)
				{
					if (!NONWORDTOKEN(prs->words[i].type))
						curlen++;
					posmarker = i;
				}
				/* cut back endpos till we find a good endpoint */
				for (i = posmarker; i > endpos && BADENDPOINT(i); i--)
				{
					if (!NONWORDTOKEN(prs->words[i].type))
						curlen--;
				}
				endpos = i;
			}
			covers[minI].startpos = startpos;
			covers[minI].endpos = endpos;
			covers[minI].curlen = curlen;
			/* Mark the chosen fragments (covers) */
			mark_fragment(prs, highlightall, startpos, endpos);
			num_f++;
			/* Exclude covers overlapping this one from future consideration */
			for (i = 0; i < numcovers; i++)
			{
				if (i != minI &&
					((covers[i].startpos >= startpos &&
					  covers[i].startpos <= endpos) ||
					 (covers[i].endpos >= startpos &&
					  covers[i].endpos <= endpos) ||
					 (covers[i].startpos < startpos &&
					  covers[i].endpos > endpos)))
					covers[i].excluded = true;
			}
		}
		else
			break;				/* no selectable covers remain */
	}

	/* show the first min_words words if we have not marked anything */
	if (num_f <= 0)
	{
		startpos = endpos = curlen = 0;
		for (i = 0; i < prs->curwords && curlen < min_words; i++)
		{
			if (!NONWORDTOKEN(prs->words[i].type))
				curlen++;
			endpos = i;
		}
		mark_fragment(prs, highlightall, startpos, endpos);
	}

	pfree(covers);
}

/*
 * Headline selector used when MaxFragments == 0
 */
static void
mark_hl_words(HeadlineParsedText *prs, TSQuery query, bool highlightall,
			  int shortword, int min_words, int max_words, int max_cover)
{
	int			p = 0,
				q = 0;
	int			bestb = -1,
				beste = -1;
	int			bestlen = -1;
	bool		bestcover = false;
	int			pose,
				posb,
				poslen,
				curlen;
	bool		poscover;
	int			i;

	if (!highlightall)
	{
		/* examine all covers, select a headline using the best one */
		while (hlCover(prs, query, max_cover, &p, &q))
		{
			/*
			 * Count words (curlen) and interesting words (poslen) within
			 * cover, but stop once we reach max_words.  This step doesn't
			 * consider whether that's a good stopping point.  posb and pose
			 * are set to the start and end indexes of the possible headline.
			 */
			curlen = 0;
			poslen = 0;
			posb = pose = p;
			for (i = p; i <= q && curlen < max_words; i++)
			{
				if (!NONWORDTOKEN(prs->words[i].type))
					curlen++;
				if (INTERESTINGWORD(i))
					poslen++;
				pose = i;
			}

			if (curlen < max_words)
			{
				/*
				 * We have room to lengthen the headline, so search forward
				 * until it's full or we find a good stopping point.  We'll
				 * reconsider the word at "q", then move forward.
				 */
				for (i = i - 1; i < prs->curwords && curlen < max_words; i++)
				{
					if (i > q)
					{
						if (!NONWORDTOKEN(prs->words[i].type))
							curlen++;
						if (INTERESTINGWORD(i))
							poslen++;
					}
					pose = i;
					if (BADENDPOINT(i))
						continue;
					if (curlen >= min_words)
						break;
				}
				if (curlen < min_words)
				{
					/*
					 * Reached end of text and our headline is still shorter
					 * than min_words, so try to extend it to the left.
					 */
					for (i = p - 1; i >= 0; i--)
					{
						if (!NONWORDTOKEN(prs->words[i].type))
							curlen++;
						if (INTERESTINGWORD(i))
							poslen++;
						if (curlen >= max_words)
							break;
						if (BADENDPOINT(i))
							continue;
						if (curlen >= min_words)
							break;
					}
					posb = (i >= 0) ? i : 0;
				}
			}
			else
			{
				/*
				 * Can't make headline longer, so consider making it shorter
				 * if needed to avoid a bad endpoint.
				 */
				if (i > q)
					i = q;
				for (; curlen > min_words; i--)
				{
					if (!BADENDPOINT(i))
						break;
					if (!NONWORDTOKEN(prs->words[i].type))
						curlen--;
					if (INTERESTINGWORD(i))
						poslen--;
					pose = i - 1;
				}
			}

			/*
			 * Check whether the proposed headline includes the original
			 * cover; it might not if we trimmed it due to max_words.
			 */
			poscover = (posb <= p && pose >= q);

			/*
			 * Adopt this headline if it's better than the last one, giving
			 * highest priority to headlines including the cover, then to
			 * headlines with more interesting words, then to headlines with
			 * good stopping points.  (Since bestlen is initially -1, we will
			 * certainly adopt the first headline.)
			 */
			if (poscover > bestcover ||
				(poscover == bestcover && poslen > bestlen) ||
				(poscover == bestcover && poslen == bestlen &&
				 !BADENDPOINT(pose) && BADENDPOINT(beste)))
			{
				bestb = posb;
				beste = pose;
				bestlen = poslen;
				bestcover = poscover;
			}

			/* move p to generate the next cover */
			p++;
		}

		/*
		 * If we found nothing acceptable, select min_words words starting at
		 * the beginning.
		 */
		if (bestlen < 0)
		{
			curlen = 0;
			pose = 0;
			for (i = 0; i < prs->curwords && curlen < min_words; i++)
			{
				if (!NONWORDTOKEN(prs->words[i].type))
					curlen++;
				pose = i;
			}
			bestb = 0;
			beste = pose;
		}
	}
	else
	{
		/* highlightall mode: headline is whole document */
		bestb = 0;
		beste = prs->curwords - 1;
	}

	mark_fragment(prs, highlightall, bestb, beste);
}

/*
 * Default parser's prsheadline function
 */
Datum
prsd_headline(PG_FUNCTION_ARGS)
{
	HeadlineParsedText *prs = (HeadlineParsedText *) PG_GETARG_POINTER(0);
	List	   *prsoptions = (List *) PG_GETARG_POINTER(1);
	TSQuery		query = PG_GETARG_TSQUERY(2);

	/* default option values: */
	int			min_words = 15;
	int			max_words = 35;
	int			shortword = 3;
	int			max_fragments = 0;
	bool		highlightall = false;
	int			max_cover;
	ListCell   *l;

	/* Extract configuration option values */
	prs->startsel = NULL;
	prs->stopsel = NULL;
	prs->fragdelim = NULL;
	foreach(l, prsoptions)
	{
		DefElem    *defel = (DefElem *) lfirst(l);
		char	   *val = defGetString(defel);

		if (pg_strcasecmp(defel->defname, "MaxWords") == 0)
			max_words = pg_strtoint32(val);
		else if (pg_strcasecmp(defel->defname, "MinWords") == 0)
			min_words = pg_strtoint32(val);
		else if (pg_strcasecmp(defel->defname, "ShortWord") == 0)
			shortword = pg_strtoint32(val);
		else if (pg_strcasecmp(defel->defname, "MaxFragments") == 0)
			max_fragments = pg_strtoint32(val);
		else if (pg_strcasecmp(defel->defname, "StartSel") == 0)
			prs->startsel = pstrdup(val);
		else if (pg_strcasecmp(defel->defname, "StopSel") == 0)
			prs->stopsel = pstrdup(val);
		else if (pg_strcasecmp(defel->defname, "FragmentDelimiter") == 0)
			prs->fragdelim = pstrdup(val);
		else if (pg_strcasecmp(defel->defname, "HighlightAll") == 0)
			highlightall = (pg_strcasecmp(val, "1") == 0 ||
							pg_strcasecmp(val, "on") == 0 ||
							pg_strcasecmp(val, "true") == 0 ||
							pg_strcasecmp(val, "t") == 0 ||
							pg_strcasecmp(val, "y") == 0 ||
							pg_strcasecmp(val, "yes") == 0);
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized headline parameter: \"%s\"",
							defel->defname)));
	}

	/*
	 * We might eventually make max_cover a user-settable parameter, but for
	 * now, just compute a reasonable value based on max_words and
	 * max_fragments.
	 */
	max_cover = Max(max_words * 10, 100);
	if (max_fragments > 0)
		max_cover *= max_fragments;

	/* in HighlightAll mode these parameters are ignored */
	if (!highlightall)
	{
		if (min_words >= max_words)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("MinWords should be less than MaxWords")));
		if (min_words <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("MinWords should be positive")));
		if (shortword < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("ShortWord should be >= 0")));
		if (max_fragments < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("MaxFragments should be >= 0")));
	}

	/* Apply appropriate headline selector */
	if (max_fragments == 0)
		mark_hl_words(prs, query, highlightall, shortword,
					  min_words, max_words, max_cover);
	else
		mark_hl_fragments(prs, query, highlightall, shortword,
						  min_words, max_words, max_fragments, max_cover);

	/* Fill in default values for string options */
	if (!prs->startsel)
		prs->startsel = pstrdup("<b>");
	if (!prs->stopsel)
		prs->stopsel = pstrdup("</b>");
	if (!prs->fragdelim)
		prs->fragdelim = pstrdup(" ... ");

	/* Caller will need these lengths, too */
	prs->startsellen = strlen(prs->startsel);
	prs->stopsellen = strlen(prs->stopsel);
	prs->fragdelimlen = strlen(prs->fragdelim);

	PG_RETURN_POINTER(prs);
}
