/* $PostgreSQL: pgsql/contrib/tsearch2/wordparser/parser.h,v 1.11 2006/03/11 04:38:30 momjian Exp $ */

#ifndef __PARSER_H__
#define __PARSER_H__

#include <ctype.h>
#include <limits.h>
#include "ts_locale.h"

typedef enum
{
	TPS_Base = 0,
	TPS_InUWord,
	TPS_InLatWord,
	TPS_InCyrWord,
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
	TPS_InHTMLEntityFirst,
	TPS_InHTMLEntity,
	TPS_InHTMLEntityNumFirst,
	TPS_InHTMLEntityNum,
	TPS_InHTMLEntityEnd,
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
	TPS_InURIFirst,
	TPS_InURIStart,
	TPS_InURI,
	TPS_InFURL,
	TPS_InProtocolFirst,
	TPS_InProtocolSecond,
	TPS_InProtocolEnd,
	TPS_InHyphenLatWordFirst,
	TPS_InHyphenLatWord,
	TPS_InHyphenCyrWordFirst,
	TPS_InHyphenCyrWord,
	TPS_InHyphenUWordFirst,
	TPS_InHyphenUWord,
	TPS_InHyphenValueFirst,
	TPS_InHyphenValue,
	TPS_InHyphenValueExact,
	TPS_InParseHyphen,
	TPS_InParseHyphenHyphen,
	TPS_InHyphenCyrWordPart,
	TPS_InHyphenLatWordPart,
	TPS_InHyphenUWordPart,
	TPS_InHyphenUnsignedInt,
	TPS_InHDecimalPartFirst,
	TPS_InHDecimalPart,
	TPS_InHVersionPartFirst,
	TPS_InHVersionPart,
	TPS_Null					/* last state (fake value) */
}	TParserState;

/* forward declaration */
struct TParser;


typedef int (*TParserCharTest) (struct TParser *);		/* any p_is* functions
														 * except p_iseq */
typedef void (*TParserSpecial) (struct TParser *);		/* special handler for
														 * special cases... */

typedef struct
{
	TParserCharTest isclass;
	char		c;
	uint16		flags;
	TParserState tostate;
	int			type;
	TParserSpecial special;
}	TParserStateActionItem;

typedef struct
{
	TParserState state;
	TParserStateActionItem *action;
}	TParserStateAction;

typedef struct TParserPosition
{
	int			posbyte;		/* position of parser in bytes */
	int			poschar;		/* osition of parser in characters */
	int			charlen;		/* length of current char */
	int			lenbytelexeme;
	int			lencharlexeme;
	TParserState state;
	struct TParserPosition *prev;
	int			flags;
	TParserStateActionItem *pushedAtAction;
}	TParserPosition;

typedef struct TParser
{
	/* string and position information */
	char	   *str;			/* multibyte string */
	int			lenstr;			/* length of mbstring */
#ifdef TS_USE_WIDE
	wchar_t    *wstr;			/* wide character string */
	int			lenwstr;		/* length of wsting */
#endif

	/* State of parse */
	int			charmaxlen;
	bool		usewide;
	TParserPosition *state;
	bool		ignore;
	bool		wanthost;

	/* silly char */
	char		c;

	/* out */
	char	   *lexeme;
	int			lenbytelexeme;
	int			lencharlexeme;
	int			type;

}	TParser;


TParser    *TParserInit(char *, int);
bool		TParserGet(TParser *);
void		TParserClose(TParser *);

#endif
