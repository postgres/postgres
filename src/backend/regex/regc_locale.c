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
 * FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.  THIS SOFTWARE
 * IS PROVIDED ON AN "AS IS" BASIS, AND THE AUTHORS AND DISTRIBUTORS HAVE
 * NO OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
 * MODIFICATIONS.
 *
 * GOVERNMENT USE: If you are acquiring this software on behalf of the
 * U.S. government, the Government shall have only "Restricted Rights"
 * in the software and related documentation as defined in the Federal
 * Acquisition Regulations (FARs) in Clause 52.227.19 (c) (2).  If you
 * are acquiring the software on behalf of the Department of Defense, the
 * software shall be classified as "Commercial Computer Software" and the
 * Government shall have only "Restricted Rights" as defined in Clause
 * 252.227-7013 (c) (1) of DFARs.  Notwithstanding the foregoing, the
 * authors grant the U.S. Government and others acting in its behalf
 * permission to use and distribute the software in accordance with the
 * terms specified in this license.
 *
 * src/backend/regex/regc_locale.c
 */

/* ASCII character-name table */

static const struct cname
{
	const char *name;
	const char	code;
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
 * We do not use the hard-wired Unicode classification tables that Tcl does.
 * This is because (a) we need to deal with other encodings besides Unicode,
 * and (b) we want to track the behavior of the libc locale routines as
 * closely as possible.  For example, it wouldn't be unreasonable for a
 * locale to not consider every Unicode letter as a letter.  So we build
 * character classification cvecs by asking libc, even for Unicode.
 */


/*
 * element - map collating-element name to celt
 */
static celt
element(struct vars * v,		/* context */
		const chr *startp,		/* points to start of name */
		const chr *endp)		/* points just past end of name */
{
	const struct cname *cn;
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
				cc;

	if (a != b && !before(a, b))
	{
		ERR(REG_ERANGE);
		return NULL;
	}

	if (!cases)
	{							/* easy version */
		cv = getcvec(v, 0, 1);
		NOERRN();
		addrange(cv, a, b);
		return cv;
	}

	/*
	 * When case-independent, it's hard to decide when cvec ranges are usable,
	 * so for now at least, we won't try.  We use a range for the originally
	 * specified chrs and then add on any case-equivalents that are outside
	 * that range as individual chrs.
	 *
	 * To ensure sane behavior if someone specifies a very large range, limit
	 * the allocation size to 100000 chrs (arbitrary) and check for overrun
	 * inside the loop below.
	 */
	nchrs = b - a + 1;
	if (nchrs <= 0 || nchrs > 100000)
		nchrs = 100000;

	cv = getcvec(v, nchrs, 1);
	NOERRN();
	addrange(cv, a, b);

	for (c = a; c <= b; c++)
	{
		cc = pg_wc_tolower((chr) c);
		if (cc != c &&
			(before(cc, a) || before(b, cc)))
		{
			if (cv->nchrs >= cv->chrspace)
			{
				ERR(REG_ETOOBIG);
				return NULL;
			}
			addchr(cv, cc);
		}
		cc = pg_wc_toupper((chr) c);
		if (cc != c &&
			(before(cc, a) || before(b, cc)))
		{
			if (cv->nchrs >= cv->chrspace)
			{
				ERR(REG_ETOOBIG);
				return NULL;
			}
			addchr(cv, cc);
		}
		if (CANCEL_REQUESTED(v->re))
		{
			ERR(REG_CANCEL);
			return NULL;
		}
	}

	return cv;
}

/*
 * before - is celt x before celt y, for purposes of range legality?
 */
static int						/* predicate */
before(celt x, celt y)
{
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
		cv = getcvec(v, 4, 0);
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
	cv = getcvec(v, 1, 0);
	assert(cv != NULL);
	addchr(cv, (chr) c);
	return cv;
}

/*
 * cclass - supply cvec for a character class
 *
 * Must include case counterparts if "cases" is true.
 *
 * The returned cvec might be either a transient cvec gotten from getcvec(),
 * or a permanently cached one from pg_ctype_get_cache().  This is okay
 * because callers are not supposed to explicitly free the result either way.
 */
static struct cvec *
cclass(struct vars * v,			/* context */
	   const chr *startp,		/* where the name starts */
	   const chr *endp,			/* just past the end of the name */
	   int cases)				/* case-independent? */
{
	size_t		len;
	struct cvec *cv = NULL;
	const char *const * namePtr;
	int			i,
				index;

	/*
	 * The following arrays define the valid character class names.
	 */

	static const char *const classNames[] = {
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
	 * Now compute the character class contents.  For classes that are based
	 * on the behavior of a <wctype.h> or <ctype.h> function, we use
	 * pg_ctype_get_cache so that we can cache the results.  Other classes
	 * have definitions that are hard-wired here, and for those we just
	 * construct a transient cvec on the fly.
	 */

	switch ((enum classes) index)
	{
		case CC_PRINT:
			cv = pg_ctype_get_cache(pg_wc_isprint);
			break;
		case CC_ALNUM:
			cv = pg_ctype_get_cache(pg_wc_isalnum);
			break;
		case CC_ALPHA:
			cv = pg_ctype_get_cache(pg_wc_isalpha);
			break;
		case CC_ASCII:
			/* hard-wired meaning */
			cv = getcvec(v, 0, 1);
			if (cv)
				addrange(cv, 0, 0x7f);
			break;
		case CC_BLANK:
			/* hard-wired meaning */
			cv = getcvec(v, 2, 0);
			addchr(cv, '\t');
			addchr(cv, ' ');
			break;
		case CC_CNTRL:
			/* hard-wired meaning */
			cv = getcvec(v, 0, 2);
			addrange(cv, 0x0, 0x1f);
			addrange(cv, 0x7f, 0x9f);
			break;
		case CC_DIGIT:
			cv = pg_ctype_get_cache(pg_wc_isdigit);
			break;
		case CC_PUNCT:
			cv = pg_ctype_get_cache(pg_wc_ispunct);
			break;
		case CC_XDIGIT:

			/*
			 * It's not clear how to define this in non-western locales, and
			 * even less clear that there's any particular use in trying. So
			 * just hard-wire the meaning.
			 */
			cv = getcvec(v, 0, 3);
			if (cv)
			{
				addrange(cv, '0', '9');
				addrange(cv, 'a', 'f');
				addrange(cv, 'A', 'F');
			}
			break;
		case CC_SPACE:
			cv = pg_ctype_get_cache(pg_wc_isspace);
			break;
		case CC_LOWER:
			cv = pg_ctype_get_cache(pg_wc_islower);
			break;
		case CC_UPPER:
			cv = pg_ctype_get_cache(pg_wc_isupper);
			break;
		case CC_GRAPH:
			cv = pg_ctype_get_cache(pg_wc_isgraph);
			break;
	}

	/* If cv is NULL now, the reason must be "out of memory" */
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

	cv = getcvec(v, 2, 0);
	addchr(cv, lc);
	if (lc != uc)
		addchr(cv, uc);
	return cv;
}

/*
 * cmp - chr-substring compare
 *
 * Backrefs need this.  It should preferably be efficient.
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
