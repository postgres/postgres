/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2003, PostgreSQL Global Development Group
 *
 * $Header: /cvsroot/pgsql/src/bin/psql/mbprint.c,v 1.12 2003/09/12 02:40:09 momjian Exp $
 */

#include "postgres_fe.h"
#ifndef PGSCRIPTS
#include "settings.h"
#endif
#include "mbprint.h"

#include "mb/pg_wchar.h"

#ifdef WIN32
#include <windows.h>
#endif

/*
 * This is an implementation of wcwidth() and wcswidth() as defined in
 * "The Single UNIX Specification, Version 2, The Open Group, 1997"
 * <http://www.UNIX-systems.org/online.html>
 *
 * Markus Kuhn -- 2001-09-08 -- public domain
 *
 * customised for PostgreSQL
 *
 * original available at : http://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c
 */

struct mbinterval
{
	unsigned short first;
	unsigned short last;
};

/* auxiliary function for binary search in interval table */
static int
mbbisearch(pg_wchar ucs, const struct mbinterval * table, int max)
{
	int			min = 0;
	int			mid;

	if (ucs < table[0].first || ucs > table[max].last)
		return 0;
	while (max >= min)
	{
		mid = (min + max) / 2;
		if (ucs > table[mid].last)
			min = mid + 1;
		else if (ucs < table[mid].first)
			max = mid - 1;
		else
			return 1;
	}

	return 0;
}


/* The following functions define the column width of an ISO 10646
 * character as follows:
 *
 *	  - The null character (U+0000) has a column width of 0.
 *
 *	  - Other C0/C1 control characters and DEL will lead to a return
 *		value of -1.
 *
 *	  - Non-spacing and enclosing combining characters (general
 *		category code Mn or Me in the Unicode database) have a
 *		column width of 0.
 *
 *	  - Other format characters (general category code Cf in the Unicode
 *		database) and ZERO WIDTH SPACE (U+200B) have a column width of 0.
 *
 *	  - Hangul Jamo medial vowels and final consonants (U+1160-U+11FF)
 *		have a column width of 0.
 *
 *	  - Spacing characters in the East Asian Wide (W) or East Asian
 *		FullWidth (F) category as defined in Unicode Technical
 *		Report #11 have a column width of 2.
 *
 *	  - All remaining characters (including all printable
 *		ISO 8859-1 and WGL4 characters, Unicode control characters,
 *		etc.) have a column width of 1.
 *
 * This implementation assumes that wchar_t characters are encoded
 * in ISO 10646.
 */

static int
ucs_wcwidth(pg_wchar ucs)
{
	/* sorted list of non-overlapping intervals of non-spacing characters */
	static const struct mbinterval combining[] = {
		{0x0300, 0x034E}, {0x0360, 0x0362}, {0x0483, 0x0486},
		{0x0488, 0x0489}, {0x0591, 0x05A1}, {0x05A3, 0x05B9},
		{0x05BB, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
		{0x05C4, 0x05C4}, {0x064B, 0x0655}, {0x0670, 0x0670},
		{0x06D6, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED},
		{0x070F, 0x070F}, {0x0711, 0x0711}, {0x0730, 0x074A},
		{0x07A6, 0x07B0}, {0x0901, 0x0902}, {0x093C, 0x093C},
		{0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0954},
		{0x0962, 0x0963}, {0x0981, 0x0981}, {0x09BC, 0x09BC},
		{0x09C1, 0x09C4}, {0x09CD, 0x09CD}, {0x09E2, 0x09E3},
		{0x0A02, 0x0A02}, {0x0A3C, 0x0A3C}, {0x0A41, 0x0A42},
		{0x0A47, 0x0A48}, {0x0A4B, 0x0A4D}, {0x0A70, 0x0A71},
		{0x0A81, 0x0A82}, {0x0ABC, 0x0ABC}, {0x0AC1, 0x0AC5},
		{0x0AC7, 0x0AC8}, {0x0ACD, 0x0ACD}, {0x0B01, 0x0B01},
		{0x0B3C, 0x0B3C}, {0x0B3F, 0x0B3F}, {0x0B41, 0x0B43},
		{0x0B4D, 0x0B4D}, {0x0B56, 0x0B56}, {0x0B82, 0x0B82},
		{0x0BC0, 0x0BC0}, {0x0BCD, 0x0BCD}, {0x0C3E, 0x0C40},
		{0x0C46, 0x0C48}, {0x0C4A, 0x0C4D}, {0x0C55, 0x0C56},
		{0x0CBF, 0x0CBF}, {0x0CC6, 0x0CC6}, {0x0CCC, 0x0CCD},
		{0x0D41, 0x0D43}, {0x0D4D, 0x0D4D}, {0x0DCA, 0x0DCA},
		{0x0DD2, 0x0DD4}, {0x0DD6, 0x0DD6}, {0x0E31, 0x0E31},
		{0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1},
		{0x0EB4, 0x0EB9}, {0x0EBB, 0x0EBC}, {0x0EC8, 0x0ECD},
		{0x0F18, 0x0F19}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37},
		{0x0F39, 0x0F39}, {0x0F71, 0x0F7E}, {0x0F80, 0x0F84},
		{0x0F86, 0x0F87}, {0x0F90, 0x0F97}, {0x0F99, 0x0FBC},
		{0x0FC6, 0x0FC6}, {0x102D, 0x1030}, {0x1032, 0x1032},
		{0x1036, 0x1037}, {0x1039, 0x1039}, {0x1058, 0x1059},
		{0x1160, 0x11FF}, {0x17B7, 0x17BD}, {0x17C6, 0x17C6},
		{0x17C9, 0x17D3}, {0x180B, 0x180E}, {0x18A9, 0x18A9},
		{0x200B, 0x200F}, {0x202A, 0x202E}, {0x206A, 0x206F},
		{0x20D0, 0x20E3}, {0x302A, 0x302F}, {0x3099, 0x309A},
		{0xFB1E, 0xFB1E}, {0xFE20, 0xFE23}, {0xFEFF, 0xFEFF},
		{0xFFF9, 0xFFFB}
	};

	/* test for 8-bit control characters */
	if (ucs == 0)
		return 0;

	if (ucs < 32 || (ucs >= 0x7f && ucs < 0xa0) || ucs > 0x0010ffff)
		return -1;

	/* binary search in table of non-spacing characters */
	if (mbbisearch(ucs, combining,
				   sizeof(combining) / sizeof(struct mbinterval) - 1))
		return 0;

	/*
	 * if we arrive here, ucs is not a combining or C0/C1 control
	 * character
	 */

	return 1 +
		(ucs >= 0x1100 &&
		 (ucs <= 0x115f ||		/* Hangul Jamo init. consonants */
		  (ucs >= 0x2e80 && ucs <= 0xa4cf && (ucs & ~0x0011) != 0x300a &&
		   ucs != 0x303f) ||	/* CJK ... Yi */
		  (ucs >= 0xac00 && ucs <= 0xd7a3) ||	/* Hangul Syllables */
		  (ucs >= 0xf900 && ucs <= 0xfaff) ||	/* CJK Compatibility
												 * Ideographs */
		  (ucs >= 0xfe30 && ucs <= 0xfe6f) ||	/* CJK Compatibility Forms */
		  (ucs >= 0xff00 && ucs <= 0xff5f) ||	/* Fullwidth Forms */
		  (ucs >= 0xffe0 && ucs <= 0xffe6) ||
		  (ucs >= 0x20000 && ucs <= 0x2ffff)));
}

pg_wchar
utf2ucs(const unsigned char *c)
{
	/*
	 * one char version of pg_utf2wchar_with_len. no control here, c must
	 * point to a large enough string
	 */
	if ((*c & 0x80) == 0)
		return (pg_wchar) c[0];
	else if ((*c & 0xe0) == 0xc0)
	{
		return (pg_wchar) (((c[0] & 0x1f) << 6) |
						   (c[1] & 0x3f));
	}
	else if ((*c & 0xf0) == 0xe0)
	{
		return (pg_wchar) (((c[0] & 0x0f) << 12) |
						   ((c[1] & 0x3f) << 6) |
						   (c[2] & 0x3f));
	}
	else if ((*c & 0xf0) == 0xf0)
	{
		return (pg_wchar) (((c[0] & 0x07) << 18) |
						   ((c[1] & 0x3f) << 12) |
						   ((c[2] & 0x3f) << 6) |
						   (c[3] & 0x3f));
	}
	else
	{
		/* that is an invalid code on purpose */
		return 0xffffffff;
	}
}

/* mb_utf_wcwidth : calculate column length for the utf8 string pwcs
 */
static int
mb_utf_wcswidth(unsigned char *pwcs, size_t len)
{
	int			w,
				l = 0;
	int			width = 0;

	for (; *pwcs && len > 0; pwcs += l)
	{
		l = pg_utf_mblen(pwcs);
		if ((len < (size_t) l) || ((w = ucs_wcwidth(utf2ucs(pwcs))) < 0))
			return width;
		len -= l;
		width += w;
	}
	return width;
}

static int
utf_charcheck(const unsigned char *c)
{
	/*
	 * Unicode 3.1 compliant validation : for each category, it checks the
	 * combination of each byte to make sur it maps to a valid range. It
	 * also returns -1 for the following UCS values: ucs > 0x10ffff ucs &
	 * 0xfffe = 0xfffe 0xfdd0 < ucs < 0xfdef ucs & 0xdb00 = 0xd800
	 * (surrogates)
	 */
	if ((*c & 0x80) == 0)
		return 1;
	else if ((*c & 0xe0) == 0xc0)
	{
		/* two-byte char */
		if (((c[1] & 0xc0) == 0x80) && ((c[0] & 0x1f) > 0x01))
			return 2;
		return -1;
	}
	else if ((*c & 0xf0) == 0xe0)
	{
		/* three-byte char */
		if (((c[1] & 0xc0) == 0x80) &&
			(((c[0] & 0x0f) != 0x00) || ((c[1] & 0x20) == 0x20)) &&
			((c[2] & 0xc0) == 0x80))
		{
			int			z = c[0] & 0x0f;
			int			yx = ((c[1] & 0x3f) << 6) | (c[0] & 0x3f);
			int			lx = yx & 0x7f;

			/* check 0xfffe/0xffff, 0xfdd0..0xfedf range, surrogates */
			if (((z == 0x0f) &&
				 (((yx & 0xffe) == 0xffe) ||
			(((yx & 0xf80) == 0xd80) && (lx >= 0x30) && (lx <= 0x4f)))) ||
				((z == 0x0d) && ((yx & 0xb00) == 0x800)))
				return -1;
			return 3;
		}
		return -1;
	}
	else if ((*c & 0xf8) == 0xf0)
	{
		int			u = ((c[0] & 0x07) << 2) | ((c[1] & 0x30) >> 4);

		/* four-byte char */
		if (((c[1] & 0xc0) == 0x80) &&
			(u > 0x00) && (u <= 0x10) &&
			((c[2] & 0xc0) == 0x80) && ((c[3] & 0xc0) == 0x80))
		{
			/* test for 0xzzzzfffe/0xzzzzfffff */
			if (((c[1] & 0x0f) == 0x0f) && ((c[2] & 0x3f) == 0x3f) &&
				((c[3] & 0x3e) == 0x3e))
				return -1;
			return 4;
		}
		return -1;
	}
	return -1;
}

static unsigned char *
mb_utf_validate(unsigned char *pwcs)
{
	int			l = 0;
	unsigned char *p = pwcs;
	unsigned char *p0 = pwcs;

	while (*pwcs)
	{
		if ((l = utf_charcheck(pwcs)) > 0)
		{
			if (p != pwcs)
			{
				int			i;

				for (i = 0; i < l; i++)
					*p++ = *pwcs++;
			}
			else
			{
				pwcs += l;
				p += l;
			}
		}
		else
		{
			/* we skip the char */
			pwcs++;
		}
	}
	if (p != pwcs)
		*p = '\0';
	return p0;
}

/*
 * public functions : wcswidth and mbvalidate
 */

int
pg_wcswidth(unsigned char *pwcs, size_t len, int encoding)
{
	if (encoding == PG_UTF8)
		return mb_utf_wcswidth(pwcs, len);
	else
	{
		/*
		 * obviously, other encodings may want to fix this, but I don't
		 * know them myself, unfortunately.
		 */
		return len;
	}
}

unsigned char *
mbvalidate(unsigned char *pwcs, int encoding)
{
	if (encoding == PG_UTF8)
		return mb_utf_validate(pwcs);
	else
	{
		/*
		 * other encodings needing validation should add their own
		 * routines here
		 */
		return pwcs;
	}
}
