/*
 * regc_locale.c --
 *
 *	This file contains locale-specific regexp routines.
 *	This file is #included by regcomp.c.
 *
 * Copyright (c) 1998 by Scriptics Corporation.
 *
 * This software is copyrighted by the Regents of the University of
 * California, Sun Microsystems, Inc., Scriptics Corporation, ActiveState
 * Corporation and other parties.  The following terms apply to all files
 * associated with the software unless explicitly disclaimed in
 * individual files.
 *
 * The authors hereby grant permission to use, copy, modify, distribute,
 * and license this software and its documentation for any purpose, provided
 * that existing copyright notices are retained in all copies and that this
 * notice is included verbatim in any distributions. No written agreement,
 * license, or royalty fee is required for any of the authorized uses.
 * Modifications to this software may be copyrighted by their authors
 * and need not follow the licensing terms described here, provided that
 * the new terms are clearly indicated on the first page of each file where
 * they apply.
 *
 * IN NO EVENT SHALL THE AUTHORS OR DISTRIBUTORS BE LIABLE TO ANY PARTY
 * FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE, ITS DOCUMENTATION, OR ANY
 * DERIVATIVES THEREOF, EVEN IF THE AUTHORS HAVE BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE AUTHORS AND DISTRIBUTORS SPECIFICALLY DISCLAIM ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.	THIS SOFTWARE
 * IS PROVIDED ON AN "AS IS" BASIS, AND THE AUTHORS AND DISTRIBUTORS HAVE
 * NO OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
 * MODIFICATIONS.
 *
 * GOVERNMENT USE: If you are acquiring this software on behalf of the
 * U.S. government, the Government shall have only "Restricted Rights"
 * in the software and related documentation as defined in the Federal
 * Acquisition Regulations (FARs) in Clause 52.227.19 (c) (2).	If you
 * are acquiring the software on behalf of the Department of Defense, the
 * software shall be classified as "Commercial Computer Software" and the
 * Government shall have only "Restricted Rights" as defined in Clause
 * 252.227-7013 (c) (1) of DFARs.  Notwithstanding the foregoing, the
 * authors grant the U.S. Government and others acting in its behalf
 * permission to use and distribute the software in accordance with the
 * terms specified in this license.
 *
 * $PostgreSQL: pgsql/src/backend/regex/regc_locale.c,v 1.8 2005/11/22 18:17:19 momjian Exp $
 */

/* ASCII character-name table */

static struct cname
{
	char	   *name;
	char		code;
}	cnames[] =

{
	{
		"NUL", '\0'
	},
	{
		"SOH", '\001'
	},
	{
		"STX", '\002'
	},
	{
		"ETX", '\003'
	},
	{
		"EOT", '\004'
	},
	{
		"ENQ", '\005'
	},
	{
		"ACK", '\006'
	},
	{
		"BEL", '\007'
	},
	{
		"alert", '\007'
	},
	{
		"BS", '\010'
	},
	{
		"backspace", '\b'
	},
	{
		"HT", '\011'
	},
	{
		"tab", '\t'
	},
	{
		"LF", '\012'
	},
	{
		"newline", '\n'
	},
	{
		"VT", '\013'
	},
	{
		"vertical-tab", '\v'
	},
	{
		"FF", '\014'
	},
	{
		"form-feed", '\f'
	},
	{
		"CR", '\015'
	},
	{
		"carriage-return", '\r'
	},
	{
		"SO", '\016'
	},
	{
		"SI", '\017'
	},
	{
		"DLE", '\020'
	},
	{
		"DC1", '\021'
	},
	{
		"DC2", '\022'
	},
	{
		"DC3", '\023'
	},
	{
		"DC4", '\024'
	},
	{
		"NAK", '\025'
	},
	{
		"SYN", '\026'
	},
	{
		"ETB", '\027'
	},
	{
		"CAN", '\030'
	},
	{
		"EM", '\031'
	},
	{
		"SUB", '\032'
	},
	{
		"ESC", '\033'
	},
	{
		"IS4", '\034'
	},
	{
		"FS", '\034'
	},
	{
		"IS3", '\035'
	},
	{
		"GS", '\035'
	},
	{
		"IS2", '\036'
	},
	{
		"RS", '\036'
	},
	{
		"IS1", '\037'
	},
	{
		"US", '\037'
	},
	{
		"space", ' '
	},
	{
		"exclamation-mark", '!'
	},
	{
		"quotation-mark", '"'
	},
	{
		"number-sign", '#'
	},
	{
		"dollar-sign", '$'
	},
	{
		"percent-sign", '%'
	},
	{
		"ampersand", '&'
	},
	{
		"apostrophe", '\''
	},
	{
		"left-parenthesis", '('
	},
	{
		"right-parenthesis", ')'
	},
	{
		"asterisk", '*'
	},
	{
		"plus-sign", '+'
	},
	{
		"comma", ','
	},
	{
		"hyphen", '-'
	},
	{
		"hyphen-minus", '-'
	},
	{
		"period", '.'
	},
	{
		"full-stop", '.'
	},
	{
		"slash", '/'
	},
	{
		"solidus", '/'
	},
	{
		"zero", '0'
	},
	{
		"one", '1'
	},
	{
		"two", '2'
	},
	{
		"three", '3'
	},
	{
		"four", '4'
	},
	{
		"five", '5'
	},
	{
		"six", '6'
	},
	{
		"seven", '7'
	},
	{
		"eight", '8'
	},
	{
		"nine", '9'
	},
	{
		"colon", ':'
	},
	{
		"semicolon", ';'
	},
	{
		"less-than-sign", '<'
	},
	{
		"equals-sign", '='
	},
	{
		"greater-than-sign", '>'
	},
	{
		"question-mark", '?'
	},
	{
		"commercial-at", '@'
	},
	{
		"left-square-bracket", '['
	},
	{
		"backslash", '\\'
	},
	{
		"reverse-solidus", '\\'
	},
	{
		"right-square-bracket", ']'
	},
	{
		"circumflex", '^'
	},
	{
		"circumflex-accent", '^'
	},
	{
		"underscore", '_'
	},
	{
		"low-line", '_'
	},
	{
		"grave-accent", '`'
	},
	{
		"left-brace", '{'
	},
	{
		"left-curly-bracket", '{'
	},
	{
		"vertical-line", '|'
	},
	{
		"right-brace", '}'
	},
	{
		"right-curly-bracket", '}'
	},
	{
		"tilde", '~'
	},
	{
		"DEL", '\177'
	},
	{
		NULL, 0
	}
};

/*
 * some ctype functions with non-ascii-char guard
 */
static int
pg_wc_isdigit(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && isdigit((unsigned char) c));
}

static int
pg_wc_isalpha(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && isalpha((unsigned char) c));
}

static int
pg_wc_isalnum(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && isalnum((unsigned char) c));
}

static int
pg_wc_isupper(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && isupper((unsigned char) c));
}

static int
pg_wc_islower(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && islower((unsigned char) c));
}

static int
pg_wc_isgraph(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && isgraph((unsigned char) c));
}

static int
pg_wc_isprint(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && isprint((unsigned char) c));
}

static int
pg_wc_ispunct(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && ispunct((unsigned char) c));
}

static int
pg_wc_isspace(pg_wchar c)
{
	return (c >= 0 && c <= UCHAR_MAX && isspace((unsigned char) c));
}

static pg_wchar
pg_wc_toupper(pg_wchar c)
{
	if (c >= 0 && c <= UCHAR_MAX)
		return toupper((unsigned char) c);
	return c;
}

static pg_wchar
pg_wc_tolower(pg_wchar c)
{
	if (c >= 0 && c <= UCHAR_MAX)
		return tolower((unsigned char) c);
	return c;
}


/*
 * nmcces - how many distinct MCCEs are there?
 */
static int
nmcces(struct vars * v)
{
	/*
	 * No multi-character collating elements defined at the moment.
	 */
	return 0;
}

/*
 * nleaders - how many chrs can be first chrs of MCCEs?
 */
static int
nleaders(struct vars * v)
{
	return 0;
}

/*
 * allmcces - return a cvec with all the MCCEs of the locale
 */
static struct cvec *
allmcces(struct vars * v,		/* context */
		 struct cvec * cv)		/* this is supposed to have enough room */
{
	return clearcvec(cv);
}

/*
 * element - map collating-element name to celt
 */
static celt
element(struct vars * v,		/* context */
		chr *startp,			/* points to start of name */
		chr *endp)				/* points just past end of name */
{
	struct cname *cn;
	size_t		len;

	/* generic:  one-chr names stand for themselves */
	assert(startp < endp);
	len = endp - startp;
	if (len == 1)
		return *startp;

	NOTE(REG_ULOCALE);

	/* search table */
	for (cn = cnames; cn->name != NULL; cn++)
	{
		if (strlen(cn->name) == len &&
			pg_char_and_wchar_strncmp(cn->name, startp, len) == 0)
		{
			break;				/* NOTE BREAK OUT */
		}
	}
	if (cn->name != NULL)
		return CHR(cn->code);

	/* couldn't find it */
	ERR(REG_ECOLLATE);
	return 0;
}

/*
 * range - supply cvec for a range, including legality check
 */
static struct cvec *
range(struct vars * v,			/* context */
	  celt a,					/* range start */
	  celt b,					/* range end, might equal a */
	  int cases)				/* case-independent? */
{
	int			nchrs;
	struct cvec *cv;
	celt		c,
				lc,
				uc;

	if (a != b && !before(a, b))
	{
		ERR(REG_ERANGE);
		return NULL;
	}

	if (!cases)
	{							/* easy version */
		cv = getcvec(v, 0, 1, 0);
		NOERRN();
		addrange(cv, a, b);
		return cv;
	}

	/*
	 * When case-independent, it's hard to decide when cvec ranges are usable,
	 * so for now at least, we won't try.  We allocate enough space for two
	 * case variants plus a little extra for the two title case variants.
	 */

	nchrs = (b - a + 1) * 2 + 4;

	cv = getcvec(v, nchrs, 0, 0);
	NOERRN();

	for (c = a; c <= b; c++)
	{
		addchr(cv, c);
		lc = pg_wc_tolower((chr) c);
		if (c != lc)
			addchr(cv, lc);
		uc = pg_wc_toupper((chr) c);
		if (c != uc)
			addchr(cv, uc);
	}

	return cv;
}

/*
 * before - is celt x before celt y, for purposes of range legality?
 */
static int						/* predicate */
before(celt x, celt y)
{
	/* trivial because no MCCEs */
	if (x < y)
		return 1;
	return 0;
}

/*
 * eclass - supply cvec for an equivalence class
 * Must include case counterparts on request.
 */
static struct cvec *
eclass(struct vars * v,			/* context */
	   celt c,					/* Collating element representing the
								 * equivalence class. */
	   int cases)				/* all cases? */
{
	struct cvec *cv;

	/* crude fake equivalence class for testing */
	if ((v->cflags & REG_FAKE) && c == 'x')
	{
		cv = getcvec(v, 4, 0, 0);
		addchr(cv, (chr) 'x');
		addchr(cv, (chr) 'y');
		if (cases)
		{
			addchr(cv, (chr) 'X');
			addchr(cv, (chr) 'Y');
		}
		return cv;
	}

	/* otherwise, none */
	if (cases)
		return allcases(v, c);
	cv = getcvec(v, 1, 0, 0);
	assert(cv != NULL);
	addchr(cv, (chr) c);
	return cv;
}

/*
 * cclass - supply cvec for a character class
 *
 * Must include case counterparts on request.
 */
static struct cvec *
cclass(struct vars * v,			/* context */
	   chr *startp,				/* where the name starts */
	   chr *endp,				/* just past the end of the name */
	   int cases)				/* case-independent? */
{
	size_t		len;
	struct cvec *cv = NULL;
	char	  **namePtr;
	int			i,
				index;

	/*
	 * The following arrays define the valid character class names.
	 */

	static char *classNames[] = {
		"alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph",
		"lower", "print", "punct", "space", "upper", "xdigit", NULL
	};

	enum classes
	{
		CC_ALNUM, CC_ALPHA, CC_ASCII, CC_BLANK, CC_CNTRL, CC_DIGIT, CC_GRAPH,
		CC_LOWER, CC_PRINT, CC_PUNCT, CC_SPACE, CC_UPPER, CC_XDIGIT
	};

	/*
	 * Map the name to the corresponding enumerated value.
	 */
	len = endp - startp;
	index = -1;
	for (namePtr = classNames, i = 0; *namePtr != NULL; namePtr++, i++)
	{
		if (strlen(*namePtr) == len &&
			pg_char_and_wchar_strncmp(*namePtr, startp, len) == 0)
		{
			index = i;
			break;
		}
	}
	if (index == -1)
	{
		ERR(REG_ECTYPE);
		return NULL;
	}

	/*
	 * Remap lower and upper to alpha if the match is case insensitive.
	 */

	if (cases &&
		((enum classes) index == CC_LOWER ||
		 (enum classes) index == CC_UPPER))
		index = (int) CC_ALPHA;

	/*
	 * Now compute the character class contents.
	 *
	 * For the moment, assume that only char codes < 256 can be in these
	 * classes.
	 */

	switch ((enum classes) index)
	{
		case CC_PRINT:
			cv = getcvec(v, UCHAR_MAX, 0, 0);
			if (cv)
			{
				for (i = 0; i <= UCHAR_MAX; i++)
				{
					if (pg_wc_isprint((chr) i))
						addchr(cv, (chr) i);
				}
			}
			break;
		case CC_ALNUM:
			cv = getcvec(v, UCHAR_MAX, 0, 0);
			if (cv)
			{
				for (i = 0; i <= UCHAR_MAX; i++)
				{
					if (pg_wc_isalnum((chr) i))
						addchr(cv, (chr) i);
				}
			}
			break;
		case CC_ALPHA:
			cv = getcvec(v, UCHAR_MAX, 0, 0);
			if (cv)
			{
				for (i = 0; i <= UCHAR_MAX; i++)
				{
					if (pg_wc_isalpha((chr) i))
						addchr(cv, (chr) i);
				}
			}
			break;
		case CC_ASCII:
			cv = getcvec(v, 0, 1, 0);
			if (cv)
				addrange(cv, 0, 0x7f);
			break;
		case CC_BLANK:
			cv = getcvec(v, 2, 0, 0);
			addchr(cv, '\t');
			addchr(cv, ' ');
			break;
		case CC_CNTRL:
			cv = getcvec(v, 0, 2, 0);
			addrange(cv, 0x0, 0x1f);
			addrange(cv, 0x7f, 0x9f);
			break;
		case CC_DIGIT:
			cv = getcvec(v, 0, 1, 0);
			if (cv)
				addrange(cv, (chr) '0', (chr) '9');
			break;
		case CC_PUNCT:
			cv = getcvec(v, UCHAR_MAX, 0, 0);
			if (cv)
			{
				for (i = 0; i <= UCHAR_MAX; i++)
				{
					if (pg_wc_ispunct((chr) i))
						addchr(cv, (chr) i);
				}
			}
			break;
		case CC_XDIGIT:
			cv = getcvec(v, 0, 3, 0);
			if (cv)
			{
				addrange(cv, '0', '9');
				addrange(cv, 'a', 'f');
				addrange(cv, 'A', 'F');
			}
			break;
		case CC_SPACE:
			cv = getcvec(v, UCHAR_MAX, 0, 0);
			if (cv)
			{
				for (i = 0; i <= UCHAR_MAX; i++)
				{
					if (pg_wc_isspace((chr) i))
						addchr(cv, (chr) i);
				}
			}
			break;
		case CC_LOWER:
			cv = getcvec(v, UCHAR_MAX, 0, 0);
			if (cv)
			{
				for (i = 0; i <= UCHAR_MAX; i++)
				{
					if (pg_wc_islower((chr) i))
						addchr(cv, (chr) i);
				}
			}
			break;
		case CC_UPPER:
			cv = getcvec(v, UCHAR_MAX, 0, 0);
			if (cv)
			{
				for (i = 0; i <= UCHAR_MAX; i++)
				{
					if (pg_wc_isupper((chr) i))
						addchr(cv, (chr) i);
				}
			}
			break;
		case CC_GRAPH:
			cv = getcvec(v, UCHAR_MAX, 0, 0);
			if (cv)
			{
				for (i = 0; i <= UCHAR_MAX; i++)
				{
					if (pg_wc_isgraph((chr) i))
						addchr(cv, (chr) i);
				}
			}
			break;
	}
	if (cv == NULL)
		ERR(REG_ESPACE);
	return cv;
}

/*
 * allcases - supply cvec for all case counterparts of a chr (including itself)
 *
 * This is a shortcut, preferably an efficient one, for simple characters;
 * messy cases are done via range().
 */
static struct cvec *
allcases(struct vars * v,		/* context */
		 chr pc)				/* character to get case equivs of */
{
	struct cvec *cv;
	chr			c = (chr) pc;
	chr			lc,
				uc;

	lc = pg_wc_tolower((chr) c);
	uc = pg_wc_toupper((chr) c);

	cv = getcvec(v, 2, 0, 0);
	addchr(cv, lc);
	if (lc != uc)
		addchr(cv, uc);
	return cv;
}

/*
 * cmp - chr-substring compare
 *
 * Backrefs need this.	It should preferably be efficient.
 * Note that it does not need to report anything except equal/unequal.
 * Note also that the length is exact, and the comparison should not
 * stop at embedded NULs!
 */
static int						/* 0 for equal, nonzero for unequal */
cmp(const chr *x, const chr *y, /* strings to compare */
	size_t len)					/* exact length of comparison */
{
	return memcmp(VS(x), VS(y), len * sizeof(chr));
}

/*
 * casecmp - case-independent chr-substring compare
 *
 * REG_ICASE backrefs need this.  It should preferably be efficient.
 * Note that it does not need to report anything except equal/unequal.
 * Note also that the length is exact, and the comparison should not
 * stop at embedded NULs!
 */
static int						/* 0 for equal, nonzero for unequal */
casecmp(const chr *x, const chr *y,		/* strings to compare */
		size_t len)				/* exact length of comparison */
{
	for (; len > 0; len--, x++, y++)
	{
		if ((*x != *y) && (pg_wc_tolower(*x) != pg_wc_tolower(*y)))
			return 1;
	}
	return 0;
}
