/*-------------------------------------------------------------------------
 *
 *	  Utility functions for conversion procs.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mb/conv.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "mb/pg_wchar.h"


/*
 * local2local: a generic single byte charset encoding
 * conversion between two ASCII-superset encodings.
 *
 * l points to the source string of length len
 * p is the output area (must be large enough!)
 * src_encoding is the PG identifier for the source encoding
 * dest_encoding is the PG identifier for the target encoding
 * tab holds conversion entries for the source charset
 * starting from 128 (0x80). each entry in the table holds the corresponding
 * code point for the target charset, or 0 if there is no equivalent code.
 *
 * Returns the number of input bytes consumed.  If noError is true, this can
 * be less than 'len'.
 */
int
local2local(const unsigned char *l,
			unsigned char *p,
			int len,
			int src_encoding,
			int dest_encoding,
			const unsigned char *tab,
			bool noError)
{
	const unsigned char *start = l;
	unsigned char c1,
				c2;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(src_encoding, (const char *) l, len);
		}
		if (!IS_HIGHBIT_SET(c1))
			*p++ = c1;
		else
		{
			c2 = tab[c1 - HIGHBIT];
			if (c2)
				*p++ = c2;
			else
			{
				if (noError)
					break;
				report_untranslatable_char(src_encoding, dest_encoding,
										   (const char *) l, len);
			}
		}
		l++;
		len--;
	}
	*p = '\0';

	return l - start;
}

/*
 * LATINn ---> MIC when the charset's local codes map directly to MIC
 *
 * l points to the source string of length len
 * p is the output area (must be large enough!)
 * lc is the mule character set id for the local encoding
 * encoding is the PG identifier for the local encoding
 *
 * Returns the number of input bytes consumed.  If noError is true, this can
 * be less than 'len'.
 */
int
latin2mic(const unsigned char *l, unsigned char *p, int len,
		  int lc, int encoding, bool noError)
{
	const unsigned char *start = l;
	int			c1;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(encoding, (const char *) l, len);
		}
		if (IS_HIGHBIT_SET(c1))
			*p++ = lc;
		*p++ = c1;
		l++;
		len--;
	}
	*p = '\0';

	return l - start;
}

/*
 * MIC ---> LATINn when the charset's local codes map directly to MIC
 *
 * mic points to the source string of length len
 * p is the output area (must be large enough!)
 * lc is the mule character set id for the local encoding
 * encoding is the PG identifier for the local encoding
 *
 * Returns the number of input bytes consumed.  If noError is true, this can
 * be less than 'len'.
 */
int
mic2latin(const unsigned char *mic, unsigned char *p, int len,
		  int lc, int encoding, bool noError)
{
	const unsigned char *start = mic;
	int			c1;

	while (len > 0)
	{
		c1 = *mic;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic, len);
		}
		if (!IS_HIGHBIT_SET(c1))
		{
			/* easy for ASCII */
			*p++ = c1;
			mic++;
			len--;
		}
		else
		{
			int			l = pg_mule_mblen(mic);

			if (len < l)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic,
										len);
			}
			if (l != 2 || c1 != lc || !IS_HIGHBIT_SET(mic[1]))
			{
				if (noError)
					break;
				report_untranslatable_char(PG_MULE_INTERNAL, encoding,
										   (const char *) mic, len);
			}
			*p++ = mic[1];
			mic += 2;
			len -= 2;
		}
	}
	*p = '\0';

	return mic - start;
}


/*
 * latin2mic_with_table: a generic single byte charset encoding
 * conversion from a local charset to the mule internal code.
 *
 * l points to the source string of length len
 * p is the output area (must be large enough!)
 * lc is the mule character set id for the local encoding
 * encoding is the PG identifier for the local encoding
 * tab holds conversion entries for the local charset
 * starting from 128 (0x80). each entry in the table holds the corresponding
 * code point for the mule encoding, or 0 if there is no equivalent code.
 *
 * Returns the number of input bytes consumed.  If noError is true, this can
 * be less than 'len'.
 */
int
latin2mic_with_table(const unsigned char *l,
					 unsigned char *p,
					 int len,
					 int lc,
					 int encoding,
					 const unsigned char *tab,
					 bool noError)
{
	const unsigned char *start = l;
	unsigned char c1,
				c2;

	while (len > 0)
	{
		c1 = *l;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(encoding, (const char *) l, len);
		}
		if (!IS_HIGHBIT_SET(c1))
			*p++ = c1;
		else
		{
			c2 = tab[c1 - HIGHBIT];
			if (c2)
			{
				*p++ = lc;
				*p++ = c2;
			}
			else
			{
				if (noError)
					break;
				report_untranslatable_char(encoding, PG_MULE_INTERNAL,
										   (const char *) l, len);
			}
		}
		l++;
		len--;
	}
	*p = '\0';

	return l - start;
}

/*
 * mic2latin_with_table: a generic single byte charset encoding
 * conversion from the mule internal code to a local charset.
 *
 * mic points to the source string of length len
 * p is the output area (must be large enough!)
 * lc is the mule character set id for the local encoding
 * encoding is the PG identifier for the local encoding
 * tab holds conversion entries for the mule internal code's second byte,
 * starting from 128 (0x80). each entry in the table holds the corresponding
 * code point for the local charset, or 0 if there is no equivalent code.
 *
 * Returns the number of input bytes consumed.  If noError is true, this can
 * be less than 'len'.
 */
int
mic2latin_with_table(const unsigned char *mic,
					 unsigned char *p,
					 int len,
					 int lc,
					 int encoding,
					 const unsigned char *tab,
					 bool noError)
{
	const unsigned char *start = mic;
	unsigned char c1,
				c2;

	while (len > 0)
	{
		c1 = *mic;
		if (c1 == 0)
		{
			if (noError)
				break;
			report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic, len);
		}
		if (!IS_HIGHBIT_SET(c1))
		{
			/* easy for ASCII */
			*p++ = c1;
			mic++;
			len--;
		}
		else
		{
			int			l = pg_mule_mblen(mic);

			if (len < l)
			{
				if (noError)
					break;
				report_invalid_encoding(PG_MULE_INTERNAL, (const char *) mic,
										len);
			}
			if (l != 2 || c1 != lc || !IS_HIGHBIT_SET(mic[1]) ||
				(c2 = tab[mic[1] - HIGHBIT]) == 0)
			{
				if (noError)
					break;
				report_untranslatable_char(PG_MULE_INTERNAL, encoding,
										   (const char *) mic, len);
				break;			/* keep compiler quiet */
			}
			*p++ = c2;
			mic += 2;
			len -= 2;
		}
	}
	*p = '\0';

	return mic - start;
}

/*
 * comparison routine for bsearch()
 * this routine is intended for combined UTF8 -> local code
 */
static int
compare3(const void *p1, const void *p2)
{
	uint32		s1,
				s2,
				d1,
				d2;

	s1 = *(const uint32 *) p1;
	s2 = *((const uint32 *) p1 + 1);
	d1 = ((const pg_utf_to_local_combined *) p2)->utf1;
	d2 = ((const pg_utf_to_local_combined *) p2)->utf2;
	return (s1 > d1 || (s1 == d1 && s2 > d2)) ? 1 : ((s1 == d1 && s2 == d2) ? 0 : -1);
}

/*
 * comparison routine for bsearch()
 * this routine is intended for local code -> combined UTF8
 */
static int
compare4(const void *p1, const void *p2)
{
	uint32		v1,
				v2;

	v1 = *(const uint32 *) p1;
	v2 = ((const pg_local_to_utf_combined *) p2)->code;
	return (v1 > v2) ? 1 : ((v1 == v2) ? 0 : -1);
}

/*
 * store 32bit character representation into multibyte stream
 */
static inline unsigned char *
store_coded_char(unsigned char *dest, uint32 code)
{
	if (code & 0xff000000)
		*dest++ = code >> 24;
	if (code & 0x00ff0000)
		*dest++ = code >> 16;
	if (code & 0x0000ff00)
		*dest++ = code >> 8;
	if (code & 0x000000ff)
		*dest++ = code;
	return dest;
}

/*
 * Convert a character using a conversion radix tree.
 *
 * 'l' is the length of the input character in bytes, and b1-b4 are
 * the input character's bytes.
 */
static inline uint32
pg_mb_radix_conv(const pg_mb_radix_tree *rt,
				 int l,
				 unsigned char b1,
				 unsigned char b2,
				 unsigned char b3,
				 unsigned char b4)
{
	if (l == 4)
	{
		/* 4-byte code */

		/* check code validity */
		if (b1 < rt->b4_1_lower || b1 > rt->b4_1_upper ||
			b2 < rt->b4_2_lower || b2 > rt->b4_2_upper ||
			b3 < rt->b4_3_lower || b3 > rt->b4_3_upper ||
			b4 < rt->b4_4_lower || b4 > rt->b4_4_upper)
			return 0;

		/* perform lookup */
		if (rt->chars32)
		{
			uint32		idx = rt->b4root;

			idx = rt->chars32[b1 + idx - rt->b4_1_lower];
			idx = rt->chars32[b2 + idx - rt->b4_2_lower];
			idx = rt->chars32[b3 + idx - rt->b4_3_lower];
			return rt->chars32[b4 + idx - rt->b4_4_lower];
		}
		else
		{
			uint16		idx = rt->b4root;

			idx = rt->chars16[b1 + idx - rt->b4_1_lower];
			idx = rt->chars16[b2 + idx - rt->b4_2_lower];
			idx = rt->chars16[b3 + idx - rt->b4_3_lower];
			return rt->chars16[b4 + idx - rt->b4_4_lower];
		}
	}
	else if (l == 3)
	{
		/* 3-byte code */

		/* check code validity */
		if (b2 < rt->b3_1_lower || b2 > rt->b3_1_upper ||
			b3 < rt->b3_2_lower || b3 > rt->b3_2_upper ||
			b4 < rt->b3_3_lower || b4 > rt->b3_3_upper)
			return 0;

		/* perform lookup */
		if (rt->chars32)
		{
			uint32		idx = rt->b3root;

			idx = rt->chars32[b2 + idx - rt->b3_1_lower];
			idx = rt->chars32[b3 + idx - rt->b3_2_lower];
			return rt->chars32[b4 + idx - rt->b3_3_lower];
		}
		else
		{
			uint16		idx = rt->b3root;

			idx = rt->chars16[b2 + idx - rt->b3_1_lower];
			idx = rt->chars16[b3 + idx - rt->b3_2_lower];
			return rt->chars16[b4 + idx - rt->b3_3_lower];
		}
	}
	else if (l == 2)
	{
		/* 2-byte code */

		/* check code validity - first byte */
		if (b3 < rt->b2_1_lower || b3 > rt->b2_1_upper ||
			b4 < rt->b2_2_lower || b4 > rt->b2_2_upper)
			return 0;

		/* perform lookup */
		if (rt->chars32)
		{
			uint32		idx = rt->b2root;

			idx = rt->chars32[b3 + idx - rt->b2_1_lower];
			return rt->chars32[b4 + idx - rt->b2_2_lower];
		}
		else
		{
			uint16		idx = rt->b2root;

			idx = rt->chars16[b3 + idx - rt->b2_1_lower];
			return rt->chars16[b4 + idx - rt->b2_2_lower];
		}
	}
	else if (l == 1)
	{
		/* 1-byte code */

		/* check code validity - first byte */
		if (b4 < rt->b1_lower || b4 > rt->b1_upper)
			return 0;

		/* perform lookup */
		if (rt->chars32)
			return rt->chars32[b4 + rt->b1root - rt->b1_lower];
		else
			return rt->chars16[b4 + rt->b1root - rt->b1_lower];
	}
	return 0;					/* shouldn't happen */
}

/*
 * UTF8 ---> local code
 *
 * utf: input string in UTF8 encoding (need not be null-terminated)
 * len: length of input string (in bytes)
 * iso: pointer to the output area (must be large enough!)
		  (output string will be null-terminated)
 * map: conversion map for single characters
 * cmap: conversion map for combined characters
 *		  (optional, pass NULL if none)
 * cmapsize: number of entries in the conversion map for combined characters
 *		  (optional, pass 0 if none)
 * conv_func: algorithmic encoding conversion function
 *		  (optional, pass NULL if none)
 * encoding: PG identifier for the local encoding
 *
 * For each character, the cmap (if provided) is consulted first; if no match,
 * the map is consulted next; if still no match, the conv_func (if provided)
 * is applied.  An error is raised if no match is found.
 *
 * See pg_wchar.h for more details about the data structures used here.
 *
 * Returns the number of input bytes consumed.  If noError is true, this can
 * be less than 'len'.
 */
int
UtfToLocal(const unsigned char *utf, int len,
		   unsigned char *iso,
		   const pg_mb_radix_tree *map,
		   const pg_utf_to_local_combined *cmap, int cmapsize,
		   utf_local_conversion_func conv_func,
		   int encoding, bool noError)
{
	uint32		iutf;
	int			l;
	const pg_utf_to_local_combined *cp;
	const unsigned char *start = utf;

	if (!PG_VALID_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding number: %d", encoding)));

	for (; len > 0; len -= l)
	{
		unsigned char b1 = 0;
		unsigned char b2 = 0;
		unsigned char b3 = 0;
		unsigned char b4 = 0;

		/* "break" cases all represent errors */
		if (*utf == '\0')
			break;

		l = pg_utf_mblen(utf);
		if (len < l)
			break;

		if (!pg_utf8_islegal(utf, l))
			break;

		if (l == 1)
		{
			/* ASCII case is easy, assume it's one-to-one conversion */
			*iso++ = *utf++;
			continue;
		}

		/* collect coded char of length l */
		if (l == 2)
		{
			b3 = *utf++;
			b4 = *utf++;
		}
		else if (l == 3)
		{
			b2 = *utf++;
			b3 = *utf++;
			b4 = *utf++;
		}
		else if (l == 4)
		{
			b1 = *utf++;
			b2 = *utf++;
			b3 = *utf++;
			b4 = *utf++;
		}
		else
		{
			elog(ERROR, "unsupported character length %d", l);
			iutf = 0;			/* keep compiler quiet */
		}
		iutf = (b1 << 24 | b2 << 16 | b3 << 8 | b4);

		/* First, try with combined map if possible */
		if (cmap && len > l)
		{
			const unsigned char *utf_save = utf;
			int			len_save = len;
			int			l_save = l;

			/* collect next character, same as above */
			len -= l;

			l = pg_utf_mblen(utf);
			if (len < l)
			{
				/* need more data to decide if this is a combined char */
				utf -= l_save;
				break;
			}

			if (!pg_utf8_islegal(utf, l))
			{
				if (!noError)
					report_invalid_encoding(PG_UTF8, (const char *) utf, len);
				utf -= l_save;
				break;
			}

			/* We assume ASCII character cannot be in combined map */
			if (l > 1)
			{
				uint32		iutf2;
				uint32		cutf[2];

				if (l == 2)
				{
					iutf2 = *utf++ << 8;
					iutf2 |= *utf++;
				}
				else if (l == 3)
				{
					iutf2 = *utf++ << 16;
					iutf2 |= *utf++ << 8;
					iutf2 |= *utf++;
				}
				else if (l == 4)
				{
					iutf2 = *utf++ << 24;
					iutf2 |= *utf++ << 16;
					iutf2 |= *utf++ << 8;
					iutf2 |= *utf++;
				}
				else
				{
					elog(ERROR, "unsupported character length %d", l);
					iutf2 = 0;	/* keep compiler quiet */
				}

				cutf[0] = iutf;
				cutf[1] = iutf2;

				cp = bsearch(cutf, cmap, cmapsize,
							 sizeof(pg_utf_to_local_combined), compare3);

				if (cp)
				{
					iso = store_coded_char(iso, cp->code);
					continue;
				}
			}

			/* fail, so back up to reprocess second character next time */
			utf = utf_save;
			len = len_save;
			l = l_save;
		}

		/* Now check ordinary map */
		if (map)
		{
			uint32		converted = pg_mb_radix_conv(map, l, b1, b2, b3, b4);

			if (converted)
			{
				iso = store_coded_char(iso, converted);
				continue;
			}
		}

		/* if there's a conversion function, try that */
		if (conv_func)
		{
			uint32		converted = (*conv_func) (iutf);

			if (converted)
			{
				iso = store_coded_char(iso, converted);
				continue;
			}
		}

		/* failed to translate this character */
		utf -= l;
		if (noError)
			break;
		report_untranslatable_char(PG_UTF8, encoding,
								   (const char *) utf, len);
	}

	/* if we broke out of loop early, must be invalid input */
	if (len > 0 && !noError)
		report_invalid_encoding(PG_UTF8, (const char *) utf, len);

	*iso = '\0';

	return utf - start;
}

/*
 * local code ---> UTF8
 *
 * iso: input string in local encoding (need not be null-terminated)
 * len: length of input string (in bytes)
 * utf: pointer to the output area (must be large enough!)
		  (output string will be null-terminated)
 * map: conversion map for single characters
 * cmap: conversion map for combined characters
 *		  (optional, pass NULL if none)
 * cmapsize: number of entries in the conversion map for combined characters
 *		  (optional, pass 0 if none)
 * conv_func: algorithmic encoding conversion function
 *		  (optional, pass NULL if none)
 * encoding: PG identifier for the local encoding
 *
 * For each character, the map is consulted first; if no match, the cmap
 * (if provided) is consulted next; if still no match, the conv_func
 * (if provided) is applied.  An error is raised if no match is found.
 *
 * See pg_wchar.h for more details about the data structures used here.
 *
 * Returns the number of input bytes consumed.  If noError is true, this can
 * be less than 'len'.
 */
int
LocalToUtf(const unsigned char *iso, int len,
		   unsigned char *utf,
		   const pg_mb_radix_tree *map,
		   const pg_local_to_utf_combined *cmap, int cmapsize,
		   utf_local_conversion_func conv_func,
		   int encoding,
		   bool noError)
{
	uint32		iiso;
	int			l;
	const pg_local_to_utf_combined *cp;
	const unsigned char *start = iso;

	if (!PG_VALID_ENCODING(encoding))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding number: %d", encoding)));

	for (; len > 0; len -= l)
	{
		unsigned char b1 = 0;
		unsigned char b2 = 0;
		unsigned char b3 = 0;
		unsigned char b4 = 0;

		/* "break" cases all represent errors */
		if (*iso == '\0')
			break;

		if (!IS_HIGHBIT_SET(*iso))
		{
			/* ASCII case is easy, assume it's one-to-one conversion */
			*utf++ = *iso++;
			l = 1;
			continue;
		}

		l = pg_encoding_verifymbchar(encoding, (const char *) iso, len);
		if (l < 0)
			break;

		/* collect coded char of length l */
		if (l == 1)
			b4 = *iso++;
		else if (l == 2)
		{
			b3 = *iso++;
			b4 = *iso++;
		}
		else if (l == 3)
		{
			b2 = *iso++;
			b3 = *iso++;
			b4 = *iso++;
		}
		else if (l == 4)
		{
			b1 = *iso++;
			b2 = *iso++;
			b3 = *iso++;
			b4 = *iso++;
		}
		else
		{
			elog(ERROR, "unsupported character length %d", l);
			iiso = 0;			/* keep compiler quiet */
		}
		iiso = (b1 << 24 | b2 << 16 | b3 << 8 | b4);

		if (map)
		{
			uint32		converted = pg_mb_radix_conv(map, l, b1, b2, b3, b4);

			if (converted)
			{
				utf = store_coded_char(utf, converted);
				continue;
			}

			/* If there's a combined character map, try that */
			if (cmap)
			{
				cp = bsearch(&iiso, cmap, cmapsize,
							 sizeof(pg_local_to_utf_combined), compare4);

				if (cp)
				{
					utf = store_coded_char(utf, cp->utf1);
					utf = store_coded_char(utf, cp->utf2);
					continue;
				}
			}
		}

		/* if there's a conversion function, try that */
		if (conv_func)
		{
			uint32		converted = (*conv_func) (iiso);

			if (converted)
			{
				utf = store_coded_char(utf, converted);
				continue;
			}
		}

		/* failed to translate this character */
		iso -= l;
		if (noError)
			break;
		report_untranslatable_char(encoding, PG_UTF8,
								   (const char *) iso, len);
	}

	/* if we broke out of loop early, must be invalid input */
	if (len > 0 && !noError)
		report_invalid_encoding(encoding, (const char *) iso, len);

	*utf = '\0';

	return iso - start;
}
