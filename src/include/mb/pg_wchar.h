/*-------------------------------------------------------------------------
 *
 * pg_wchar.h
 *	  multibyte-character support
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/mb/pg_wchar.h
 *
 *	NOTES
 *		This is used both by the backend and by frontends, but should not be
 *		included by libpq client programs.  In particular, a libpq client
 *		should not assume that the encoding IDs used by the version of libpq
 *		it's linked to match up with the IDs declared here.
 *		To help prevent mistakes, relevant functions that are exported by
 *		libpq have a physically different name when being referenced
 *		statically.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_WCHAR_H
#define PG_WCHAR_H

/*
 * The pg_wchar type
 */
typedef unsigned int pg_wchar;

/*
 * Maximum byte length of multibyte characters in any backend encoding
 */
#define MAX_MULTIBYTE_CHAR_LEN	4

/*
 * various definitions for EUC
 */
#define SS2 0x8e				/* single shift 2 (JIS0201) */
#define SS3 0x8f				/* single shift 3 (JIS0212) */

/*
 * EUC_TW planes
 */
#define LC_CNS11643_1		0x95	/* CNS 11643-1992 Plane 1 */
#define LC_CNS11643_2		0x96	/* CNS 11643-1992 Plane 2 */
#define LC_CNS11643_3		0xf6	/* CNS 11643-1992 Plane 3 */
#define LC_CNS11643_4		0xf7	/* CNS 11643-1992 Plane 4 */
#define LC_CNS11643_5		0xf8	/* CNS 11643-1992 Plane 5 */
#define LC_CNS11643_6		0xf9	/* CNS 11643-1992 Plane 6 */
#define LC_CNS11643_7		0xfa	/* CNS 11643-1992 Plane 7 */

/*
 * SJIS validation macros
 */
#define ISSJISHEAD(c) (((c) >= 0x81 && (c) <= 0x9f) || ((c) >= 0xe0 && (c) <= 0xfc))
#define ISSJISTAIL(c) (((c) >= 0x40 && (c) <= 0x7e) || ((c) >= 0x80 && (c) <= 0xfc))

/*
 * PostgreSQL encoding identifiers
 *
 * WARNING: If you add some encoding don't forget to update
 *			the pg_enc2name_tbl[] array (in src/common/encnames.c),
 *			the pg_enc2gettext_tbl[] array (in src/common/encnames.c) and
 *			the pg_wchar_table[] array (in src/common/wchar.c) and to check
 *			PG_ENCODING_BE_LAST macro.
 *
 * PG_SQL_ASCII is default encoding and must be = 0.
 *
 * XXX	We must avoid renumbering any backend encoding until libpq's major
 * version number is increased beyond 5; it turns out that the backend
 * encoding IDs are effectively part of libpq's ABI as far as 8.2 initdb and
 * psql are concerned.
 */
typedef enum pg_enc
{
	PG_SQL_ASCII = 0,			/* SQL/ASCII */
	PG_EUC_JP,					/* EUC for Japanese */
	PG_EUC_CN,					/* EUC for Chinese */
	PG_EUC_KR,					/* EUC for Korean */
	PG_EUC_TW,					/* EUC for Taiwan */
	PG_EUC_JIS_2004,			/* EUC-JIS-2004 */
	PG_UTF8,					/* Unicode UTF8 */
	PG_UNUSED_1,				/* (Was Mule internal code) */
	PG_LATIN1,					/* ISO-8859-1 Latin 1 */
	PG_LATIN2,					/* ISO-8859-2 Latin 2 */
	PG_LATIN3,					/* ISO-8859-3 Latin 3 */
	PG_LATIN4,					/* ISO-8859-4 Latin 4 */
	PG_LATIN5,					/* ISO-8859-9 Latin 5 */
	PG_LATIN6,					/* ISO-8859-10 Latin6 */
	PG_LATIN7,					/* ISO-8859-13 Latin7 */
	PG_LATIN8,					/* ISO-8859-14 Latin8 */
	PG_LATIN9,					/* ISO-8859-15 Latin9 */
	PG_LATIN10,					/* ISO-8859-16 Latin10 */
	PG_WIN1256,					/* windows-1256 */
	PG_WIN1258,					/* Windows-1258 */
	PG_WIN866,					/* (MS-DOS CP866) */
	PG_WIN874,					/* windows-874 */
	PG_KOI8R,					/* KOI8-R */
	PG_WIN1251,					/* windows-1251 */
	PG_WIN1252,					/* windows-1252 */
	PG_ISO_8859_5,				/* ISO-8859-5 */
	PG_ISO_8859_6,				/* ISO-8859-6 */
	PG_ISO_8859_7,				/* ISO-8859-7 */
	PG_ISO_8859_8,				/* ISO-8859-8 */
	PG_WIN1250,					/* windows-1250 */
	PG_WIN1253,					/* windows-1253 */
	PG_WIN1254,					/* windows-1254 */
	PG_WIN1255,					/* windows-1255 */
	PG_WIN1257,					/* windows-1257 */
	PG_KOI8U,					/* KOI8-U */
	/* PG_ENCODING_BE_LAST points to the above entry */

	/* followings are for client encoding only */
	PG_SJIS,					/* Shift JIS (Windows-932) */
	PG_BIG5,					/* Big5 (Windows-950) */
	PG_GBK,						/* GBK (Windows-936) */
	PG_UHC,						/* UHC (Windows-949) */
	PG_GB18030,					/* GB18030 */
	PG_JOHAB,					/* EUC for Korean JOHAB */
	PG_SHIFT_JIS_2004,			/* Shift-JIS-2004 */
	_PG_LAST_ENCODING_			/* mark only */

} pg_enc;

#define PG_ENCODING_BE_LAST PG_KOI8U

#define PG_UNUSED_ENCODING(_enc) \
	((_enc) == PG_UNUSED_1)

/*
 * Please use these tests before access to pg_enc2name_tbl[]
 * or to other places...
 */
#define PG_VALID_BE_ENCODING(_enc) \
		((_enc) >= 0 && (_enc) <= PG_ENCODING_BE_LAST && !PG_UNUSED_ENCODING(_enc))

#define PG_ENCODING_IS_CLIENT_ONLY(_enc) \
		((_enc) > PG_ENCODING_BE_LAST && (_enc) < _PG_LAST_ENCODING_)

#define PG_VALID_ENCODING(_enc) \
		((_enc) >= 0 && (_enc) < _PG_LAST_ENCODING_ && !PG_UNUSED_ENCODING(_enc))

/* On FE are possible all encodings */
#define PG_VALID_FE_ENCODING(_enc)	PG_VALID_ENCODING(_enc)

/*
 * When converting strings between different encodings, we assume that space
 * for converted result is 4-to-1 growth in the worst case.  The rate for
 * currently supported encoding pairs are within 3 (SJIS JIS X0201 half width
 * kana -> UTF8 is the worst case).  So "4" should be enough for the moment.
 *
 * Note that this is not the same as the maximum character width in any
 * particular encoding.
 */
#define MAX_CONVERSION_GROWTH  4

/*
 * Maximum byte length of a string that's required in any encoding to convert
 * at least one character to any other encoding.  In other words, if you feed
 * MAX_CONVERSION_INPUT_LENGTH bytes to any encoding conversion function, it
 * is guaranteed to be able to convert something without needing more input
 * (assuming the input is valid).
 *
 * Currently, the maximum case is the conversion UTF8 -> SJIS JIS X0201 half
 * width kana, where a pair of UTF-8 characters is converted into a single
 * SHIFT_JIS_2004 character (the reverse of the worst case for
 * MAX_CONVERSION_GROWTH).  It needs 6 bytes of input.  In theory, a
 * user-defined conversion function might have more complicated cases, although
 * for the reverse mapping you would probably also need to bump up
 * MAX_CONVERSION_GROWTH.  But there is no need to be stingy here, so make it
 * generous.
 */
#define MAX_CONVERSION_INPUT_LENGTH	16

/*
 * Maximum byte length of the string equivalent to any one Unicode code point,
 * in any backend encoding.  The current value assumes that a 4-byte UTF-8
 * character might expand by MAX_CONVERSION_GROWTH, which is a huge
 * overestimate.  But in current usage we don't allocate large multiples of
 * this, so there's little point in being stingy.
 */
#define MAX_UNICODE_EQUIVALENT_STRING	16

/*
 * Table for mapping an encoding number to official encoding name and
 * possibly other subsidiary data.  Be careful to check encoding number
 * before accessing a table entry!
 *
 * if (PG_VALID_ENCODING(encoding))
 *		pg_enc2name_tbl[ encoding ];
 */
typedef struct pg_enc2name
{
	const char *name;
	pg_enc		encoding;
#ifdef WIN32
	unsigned	codepage;		/* codepage for WIN32 */
#endif
} pg_enc2name;

extern PGDLLIMPORT const pg_enc2name pg_enc2name_tbl[];

/*
 * Encoding names for gettext
 */
extern PGDLLIMPORT const char *pg_enc2gettext_tbl[];

/*
 * pg_wchar stuff
 */
typedef int (*mb2wchar_with_len_converter) (const unsigned char *from,
											pg_wchar *to,
											int len);

typedef int (*wchar2mb_with_len_converter) (const pg_wchar *from,
											unsigned char *to,
											int len);

typedef int (*mblen_converter) (const unsigned char *mbstr);

typedef int (*mbdisplaylen_converter) (const unsigned char *mbstr);

typedef bool (*mbcharacter_incrementer) (unsigned char *mbstr, int len);

typedef int (*mbchar_verifier) (const unsigned char *mbstr, int len);

typedef int (*mbstr_verifier) (const unsigned char *mbstr, int len);

typedef struct
{
	mb2wchar_with_len_converter mb2wchar_with_len;	/* convert a multibyte
													 * string to a wchar */
	wchar2mb_with_len_converter wchar2mb_with_len;	/* convert a wchar string
													 * to a multibyte */
	mblen_converter mblen;		/* get byte length of a char */
	mbdisplaylen_converter dsplen;	/* get display width of a char */
	mbchar_verifier mbverifychar;	/* verify multibyte character */
	mbstr_verifier mbverifystr; /* verify multibyte string */
	int			maxmblen;		/* max bytes for a char in this encoding */
} pg_wchar_tbl;

extern PGDLLIMPORT const pg_wchar_tbl pg_wchar_table[];

/*
 * Data structures for conversions between UTF-8 and other encodings
 * (UtfToLocal() and LocalToUtf()).  In these data structures, characters of
 * either encoding are represented by uint32 words; hence we can only support
 * characters up to 4 bytes long.  For example, the byte sequence 0xC2 0x89
 * would be represented by 0x0000C289, and 0xE8 0xA2 0xB4 by 0x00E8A2B4.
 *
 * There are three possible ways a character can be mapped:
 *
 * 1. Using a radix tree, from source to destination code.
 * 2. Using a sorted array of source -> destination code pairs. This
 *	  method is used for "combining" characters. There are so few of
 *	  them that building a radix tree would be wasteful.
 * 3. Using a conversion function.
 */

/*
 * Radix tree for character conversion.
 *
 * Logically, this is actually four different radix trees, for 1-byte,
 * 2-byte, 3-byte and 4-byte inputs. The 1-byte tree is a simple lookup
 * table from source to target code. The 2-byte tree consists of two levels:
 * one lookup table for the first byte, where the value in the lookup table
 * points to a lookup table for the second byte. And so on.
 *
 * Physically, all the trees are stored in one big array, in 'chars16' or
 * 'chars32', depending on the maximum value that needs to be represented. For
 * each level in each tree, we also store lower and upper bound of allowed
 * values - values outside those bounds are considered invalid, and are left
 * out of the tables.
 *
 * In the intermediate levels of the trees, the values stored are offsets
 * into the chars[16|32] array.
 *
 * In the beginning of the chars[16|32] array, there is always a number of
 * zeros, so that you safely follow an index from an intermediate table
 * without explicitly checking for a zero. Following a zero any number of
 * times will always bring you to the dummy, all-zeros table in the
 * beginning. This helps to shave some cycles when looking up values.
 */
typedef struct
{
	/*
	 * Array containing all the values. Only one of chars16 or chars32 is
	 * used, depending on how wide the values we need to represent are.
	 */
	const uint16 *chars16;
	const uint32 *chars32;

	/* Radix tree for 1-byte inputs */
	uint32		b1root;			/* offset of table in the chars[16|32] array */
	uint8		b1_lower;		/* min allowed value for a single byte input */
	uint8		b1_upper;		/* max allowed value for a single byte input */

	/* Radix tree for 2-byte inputs */
	uint32		b2root;			/* offset of 1st byte's table */
	uint8		b2_1_lower;		/* min/max allowed value for 1st input byte */
	uint8		b2_1_upper;
	uint8		b2_2_lower;		/* min/max allowed value for 2nd input byte */
	uint8		b2_2_upper;

	/* Radix tree for 3-byte inputs */
	uint32		b3root;			/* offset of 1st byte's table */
	uint8		b3_1_lower;		/* min/max allowed value for 1st input byte */
	uint8		b3_1_upper;
	uint8		b3_2_lower;		/* min/max allowed value for 2nd input byte */
	uint8		b3_2_upper;
	uint8		b3_3_lower;		/* min/max allowed value for 3rd input byte */
	uint8		b3_3_upper;

	/* Radix tree for 4-byte inputs */
	uint32		b4root;			/* offset of 1st byte's table */
	uint8		b4_1_lower;		/* min/max allowed value for 1st input byte */
	uint8		b4_1_upper;
	uint8		b4_2_lower;		/* min/max allowed value for 2nd input byte */
	uint8		b4_2_upper;
	uint8		b4_3_lower;		/* min/max allowed value for 3rd input byte */
	uint8		b4_3_upper;
	uint8		b4_4_lower;		/* min/max allowed value for 4th input byte */
	uint8		b4_4_upper;

} pg_mb_radix_tree;

/*
 * UTF-8 to local code conversion map (for combined characters)
 */
typedef struct
{
	uint32		utf1;			/* UTF-8 code 1 */
	uint32		utf2;			/* UTF-8 code 2 */
	uint32		code;			/* local code */
} pg_utf_to_local_combined;

/*
 * local code to UTF-8 conversion map (for combined characters)
 */
typedef struct
{
	uint32		code;			/* local code */
	uint32		utf1;			/* UTF-8 code 1 */
	uint32		utf2;			/* UTF-8 code 2 */
} pg_local_to_utf_combined;

/*
 * callback function for algorithmic encoding conversions (in either direction)
 *
 * if function returns zero, it does not know how to convert the code
 */
typedef uint32 (*utf_local_conversion_func) (uint32 code);

/*
 * Support macro for encoding conversion functions to validate their
 * arguments.  (This could be made more compact if we included fmgr.h
 * here, but we don't want to do that because this header file is also
 * used by frontends.)
 */
#define CHECK_ENCODING_CONVERSION_ARGS(srcencoding,destencoding) \
	check_encoding_conversion_args(PG_GETARG_INT32(0), \
								   PG_GETARG_INT32(1), \
								   PG_GETARG_INT32(4), \
								   (srcencoding), \
								   (destencoding))


/*
 * Some handy functions for Unicode-specific tests.
 */
static inline bool
is_valid_unicode_codepoint(char32_t c)
{
	return (c > 0 && c <= 0x10FFFF);
}

static inline bool
is_utf16_surrogate_first(char32_t c)
{
	return (c >= 0xD800 && c <= 0xDBFF);
}

static inline bool
is_utf16_surrogate_second(char32_t c)
{
	return (c >= 0xDC00 && c <= 0xDFFF);
}

static inline char32_t
surrogate_pair_to_codepoint(char16_t first, char16_t second)
{
	return ((first & 0x3FF) << 10) + 0x10000 + (second & 0x3FF);
}

/*
 * Convert a UTF-8 character to a Unicode code point.
 * This is a one-character version of pg_utf2wchar_with_len.
 *
 * No error checks here, c must point to a long-enough string.
 */
static inline char32_t
utf8_to_unicode(const unsigned char *c)
{
	if ((*c & 0x80) == 0)
		return (char32_t) c[0];
	else if ((*c & 0xe0) == 0xc0)
		return (char32_t) (((c[0] & 0x1f) << 6) |
						   (c[1] & 0x3f));
	else if ((*c & 0xf0) == 0xe0)
		return (char32_t) (((c[0] & 0x0f) << 12) |
						   ((c[1] & 0x3f) << 6) |
						   (c[2] & 0x3f));
	else if ((*c & 0xf8) == 0xf0)
		return (char32_t) (((c[0] & 0x07) << 18) |
						   ((c[1] & 0x3f) << 12) |
						   ((c[2] & 0x3f) << 6) |
						   (c[3] & 0x3f));
	else
		/* that is an invalid code on purpose */
		return 0xffffffff;
}

/*
 * Map a Unicode code point to UTF-8.  utf8string must have at least
 * unicode_utf8len(c) bytes available.
 */
static inline unsigned char *
unicode_to_utf8(char32_t c, unsigned char *utf8string)
{
	if (c <= 0x7F)
	{
		utf8string[0] = c;
	}
	else if (c <= 0x7FF)
	{
		utf8string[0] = 0xC0 | ((c >> 6) & 0x1F);
		utf8string[1] = 0x80 | (c & 0x3F);
	}
	else if (c <= 0xFFFF)
	{
		utf8string[0] = 0xE0 | ((c >> 12) & 0x0F);
		utf8string[1] = 0x80 | ((c >> 6) & 0x3F);
		utf8string[2] = 0x80 | (c & 0x3F);
	}
	else
	{
		utf8string[0] = 0xF0 | ((c >> 18) & 0x07);
		utf8string[1] = 0x80 | ((c >> 12) & 0x3F);
		utf8string[2] = 0x80 | ((c >> 6) & 0x3F);
		utf8string[3] = 0x80 | (c & 0x3F);
	}

	return utf8string;
}

/*
 * Number of bytes needed to represent the given char in UTF8.
 */
static inline int
unicode_utf8len(char32_t c)
{
	if (c <= 0x7F)
		return 1;
	else if (c <= 0x7FF)
		return 2;
	else if (c <= 0xFFFF)
		return 3;
	else
		return 4;
}

/*
 * The functions in this list are exported by libpq, and we need to be sure
 * that we know which calls are satisfied by libpq and which are satisfied
 * by static linkage to libpgcommon.  (This is because we might be using a
 * libpq.so that's of a different major version and has encoding IDs that
 * differ from the current version's.)  The nominal function names are what
 * are actually used in and exported by libpq, while the names exported by
 * libpgcommon.a and libpgcommon_srv.a end in "_private".
 */
#if defined(USE_PRIVATE_ENCODING_FUNCS) || !defined(FRONTEND)
#define pg_char_to_encoding			pg_char_to_encoding_private
#define pg_encoding_to_char			pg_encoding_to_char_private
#define pg_valid_server_encoding	pg_valid_server_encoding_private
#define pg_valid_server_encoding_id	pg_valid_server_encoding_id_private
#define pg_utf_mblen				pg_utf_mblen_private
#endif

/*
 * These functions are considered part of libpq's exported API and
 * are also declared in libpq-fe.h.
 */
extern int	pg_char_to_encoding(const char *name);
extern const char *pg_encoding_to_char(int encoding);
extern int	pg_valid_server_encoding_id(int encoding);

/*
 * These functions are available to frontend code that links with libpgcommon
 * (in addition to the ones just above).  The constant tables declared
 * earlier in this file are also available from libpgcommon.
 */
extern void pg_encoding_set_invalid(int encoding, char *dst);
extern int	pg_encoding_mblen(int encoding, const char *mbstr);
extern int	pg_encoding_mblen_or_incomplete(int encoding, const char *mbstr,
											size_t remaining);
extern int	pg_encoding_mblen_bounded(int encoding, const char *mbstr);
extern int	pg_encoding_dsplen(int encoding, const char *mbstr);
extern int	pg_encoding_verifymbchar(int encoding, const char *mbstr, int len);
extern int	pg_encoding_verifymbstr(int encoding, const char *mbstr, int len);
extern int	pg_encoding_max_length(int encoding);
extern int	pg_valid_client_encoding(const char *name);
extern int	pg_valid_server_encoding(const char *name);
extern bool is_encoding_supported_by_icu(int encoding);
extern const char *get_encoding_name_for_icu(int encoding);

extern bool pg_utf8_islegal(const unsigned char *source, int length);
extern int	pg_utf_mblen(const unsigned char *s);

/*
 * The remaining functions are backend-only.
 */
extern int	pg_mb2wchar(const char *from, pg_wchar *to);
extern int	pg_mb2wchar_with_len(const char *from, pg_wchar *to, int len);
extern int	pg_encoding_mb2wchar_with_len(int encoding,
										  const char *from, pg_wchar *to, int len);
extern int	pg_wchar2mb(const pg_wchar *from, char *to);
extern int	pg_wchar2mb_with_len(const pg_wchar *from, char *to, int len);
extern int	pg_encoding_wchar2mb_with_len(int encoding,
										  const pg_wchar *from, char *to, int len);
extern int	pg_char_and_wchar_strcmp(const char *s1, const pg_wchar *s2);
extern int	pg_wchar_strncmp(const pg_wchar *s1, const pg_wchar *s2, size_t n);
extern int	pg_char_and_wchar_strncmp(const char *s1, const pg_wchar *s2, size_t n);
extern size_t pg_wchar_strlen(const pg_wchar *str);
extern int	pg_mblen_cstr(const char *mbstr);
extern int	pg_mblen_range(const char *mbstr, const char *end);
extern int	pg_mblen_with_len(const char *mbstr, int limit);
extern int	pg_mblen_unbounded(const char *mbstr);

/* deprecated */
extern int	pg_mblen(const char *mbstr);

extern int	pg_dsplen(const char *mbstr);
extern int	pg_mbstrlen(const char *mbstr);
extern int	pg_mbstrlen_with_len(const char *mbstr, int limit);
extern int	pg_mbcliplen(const char *mbstr, int len, int limit);
extern int	pg_encoding_mbcliplen(int encoding, const char *mbstr,
								  int len, int limit);
extern int	pg_mbcharcliplen(const char *mbstr, int len, int limit);
extern int	pg_database_encoding_max_length(void);
extern mbcharacter_incrementer pg_database_encoding_character_incrementer(void);

extern int	PrepareClientEncoding(int encoding);
extern int	SetClientEncoding(int encoding);
extern void InitializeClientEncoding(void);
extern int	pg_get_client_encoding(void);
extern const char *pg_get_client_encoding_name(void);

extern void SetDatabaseEncoding(int encoding);
extern int	GetDatabaseEncoding(void);
extern const char *GetDatabaseEncodingName(void);
extern void SetMessageEncoding(int encoding);
extern int	GetMessageEncoding(void);

#ifdef ENABLE_NLS
extern int	pg_bind_textdomain_codeset(const char *domainname);
#endif

extern unsigned char *pg_do_encoding_conversion(unsigned char *src, int len,
												int src_encoding,
												int dest_encoding);
extern int	pg_do_encoding_conversion_buf(Oid proc,
										  int src_encoding,
										  int dest_encoding,
										  unsigned char *src, int srclen,
										  unsigned char *dest, int destlen,
										  bool noError);

extern char *pg_client_to_server(const char *s, int len);
extern char *pg_server_to_client(const char *s, int len);
extern char *pg_any_to_server(const char *s, int len, int encoding);
extern char *pg_server_to_any(const char *s, int len, int encoding);

extern void pg_unicode_to_server(char32_t c, unsigned char *s);
extern bool pg_unicode_to_server_noerror(char32_t c, unsigned char *s);

extern unsigned short BIG5toCNS(unsigned short big5, unsigned char *lc);
extern unsigned short CNStoBIG5(unsigned short cns, unsigned char lc);

extern int	UtfToLocal(const unsigned char *utf, int len,
					   unsigned char *iso,
					   const pg_mb_radix_tree *map,
					   const pg_utf_to_local_combined *cmap, int cmapsize,
					   utf_local_conversion_func conv_func,
					   int encoding, bool noError);
extern int	LocalToUtf(const unsigned char *iso, int len,
					   unsigned char *utf,
					   const pg_mb_radix_tree *map,
					   const pg_local_to_utf_combined *cmap, int cmapsize,
					   utf_local_conversion_func conv_func,
					   int encoding, bool noError);

extern bool pg_verifymbstr(const char *mbstr, int len, bool noError);
extern bool pg_verify_mbstr(int encoding, const char *mbstr, int len,
							bool noError);
extern int	pg_verify_mbstr_len(int encoding, const char *mbstr, int len,
								bool noError);

extern void check_encoding_conversion_args(int src_encoding,
										   int dest_encoding,
										   int len,
										   int expected_src_encoding,
										   int expected_dest_encoding);

pg_noreturn extern void report_invalid_encoding(int encoding, const char *mbstr, int len);
pg_noreturn extern void report_untranslatable_char(int src_encoding, int dest_encoding,
												   const char *mbstr, int len);

extern int	local2local(const unsigned char *l, unsigned char *p, int len,
						int src_encoding, int dest_encoding,
						const unsigned char *tab, bool noError);

#ifdef WIN32
extern WCHAR *pgwin32_message_to_UTF16(const char *str, int len, int *utf16len);
#endif

#endif							/* PG_WCHAR_H */
