/* $Id: pg_wchar.h,v 1.36 2001/10/28 06:26:07 momjian Exp $ */

#ifndef PG_WCHAR_H
#define PG_WCHAR_H

#include <sys/types.h>

#ifdef FRONTEND
#undef palloc
#define palloc malloc
#undef pfree
#define pfree free
#endif

/*
 * The pg_wchar
 */
#ifdef MULTIBYTE
typedef unsigned int pg_wchar;

#else
#define pg_wchar char
#endif

/*
 * various definitions for EUC
 */
#define SS2 0x8e				/* single shift 2 (JIS0201) */
#define SS3 0x8f				/* single shift 3 (JIS0212) */

/*
 * Leading byte types or leading prefix byte for MULE internal code.
 * See http://www.xemacs.org for more details.	(there is a doc titled
 * "XEmacs Internals Manual", "MULE Character Sets and Encodings"
 * section.
 */
/*
 * Is a leading byte for "official" single byte encodings?
 */
#define IS_LC1(c)	((unsigned char)(c) >= 0x81 && (unsigned char)(c) <= 0x8d)
/*
 * Is a prefix byte for "private" single byte encodings?
 */
#define IS_LCPRV1(c)	((unsigned char)(c) == 0x9a || (unsigned char)(c) == 0x9b)
/*
 * Is a leading byte for "official" multi byte encodings?
 */
#define IS_LC2(c)	((unsigned char)(c) >= 0x90 && (unsigned char)(c) <= 0x99)
/*
 * Is a prefix byte for "private" multi byte encodings?
 */
#define IS_LCPRV2(c)	((unsigned char)(c) == 0x9c || (unsigned char)(c) == 0x9d)

/*----------------------------------------------------
 * leading characters
 *----------------------------------------------------
 */

/*
 * Official single byte encodings (0x81-0x8e)
 */
#define LC_ISO8859_1	0x81	/* ISO8859 Latin 1 */
#define LC_ISO8859_2	0x82	/* ISO8859 Latin 2 */
#define LC_ISO8859_3	0x83	/* ISO8859 Latin 3 */
#define LC_ISO8859_4	0x84	/* ISO8859 Latin 4 */
#define LC_TIS620	0x85		/* Thai (not supported yet) */
#define LC_ISO8859_7	0x86	/* Greek (not supported yet) */
#define LC_ISO8859_6	0x87	/* Arabic (not supported yet) */
#define LC_ISO8859_8	0x88	/* Hebrew (not supported yet) */
#define LC_JISX0201K	0x89	/* Japanese 1 byte kana */
#define LC_JISX0201R	0x8a	/* Japanese 1 byte Roman */
/* Note that 0x8b seems to be unused in as of Emacs 20.7.
 * However, there might be a chance that 0x8b could be used
 * in later version of Emacs.
 */
#define LC_KOI8_R	0x8b		/* Cyrillic KOI8-R */
#define LC_KOI8_U	0x8b		/* Cyrillic KOI8-U */
#define LC_ISO8859_5	0x8c	/* ISO8859 Cyrillic */
#define LC_ISO8859_9	0x8d	/* ISO8859 Latin 5 (not supported yet) */
/* #define FREE		0x8e	free (unused) */

/*
 * Unused
 */
#define CONTROL_1	0x8f		/* control characters (unused) */

/*
 * Official multibyte byte encodings (0x90-0x99)
 * 0x9a-0x9d are free. 0x9e and 0x9f are reserved.
 */
#define LC_JISX0208_1978	0x90	/* Japanese Kanji, old JIS (not supported) */
/* #define FREE		0x90	free (unused) */
#define LC_GB2312_80	0x91	/* Chinese */
#define LC_JISX0208 0x92		/* Japanese Kanji (JIS X 0208) */
#define LC_KS5601	0x93		/* Korean */
#define LC_JISX0212 0x94		/* Japanese Kanji (JIS X 0212) */
#define LC_CNS11643_1	0x95	/* CNS 11643-1992 Plane 1 */
#define LC_CNS11643_2	0x96	/* CNS 11643-1992 Plane 2 */
/* #define FREE		0x97	free (unused) */
#define LC_BIG5_1	0x98		/* Plane 1 Chinese traditional (not
								 * supported) */
#define LC_BIG5_2	0x99		/* Plane 1 Chinese traditional (not
								 * supported) */

/*
 * Private single byte encodings (0xa0-0xef)
 */
#define LC_SISHENG	0xa0		/* Chinese SiSheng characters for
								 * PinYin/ZhuYin (not supported) */
#define LC_IPA		0xa1		/* IPA (International Phonetic
								 * Association) (not supported) */
#define LC_VISCII_LOWER 0xa2	/* Vietnamese VISCII1.1 lower-case (not
								 * supported) */
#define LC_VISCII_UPPER 0xa3	/* Vietnamese VISCII1.1 upper-case (not
								 * supported) */
#define LC_ARABIC_DIGIT 0xa4	/* Arabic digit (not supported) */
#define LC_ARABIC_1_COLUMN	0xa5	/* Arabic 1-column (not supported) */
#define LC_ASCII_RIGHT_TO_LEFT	0xa6	/* ASCII (left half of ISO8859-1)
										 * with right-to-left direction
										 * (not supported) */
#define LC_LAO		0xa7		/* Lao characters (ISO10646 0E80..0EDF)
								 * (not supported) */
#define LC_ARABIC_2_COLUMN	0xa8	/* Arabic 1-column (not supported) */

/*
 * Private multi byte encodings (0xf0-0xff)
 */
#define LC_INDIAN_1_COLUMN	0xf0/* Indian charset for 1-column width
								 * glypps (not supported) */
#define LC_TIBETAN_1_COLUMN 0xf1	/* Tibetan 1 column glyph (not supported) */
#define LC_ETHIOPIC 0xf5		/* Ethiopic characters (not supported) */
#define LC_CNS11643_3	0xf6	/* CNS 11643-1992 Plane 3 */
#define LC_CNS11643_4	0xf7	/* CNS 11643-1992 Plane 4 */
#define LC_CNS11643_5	0xf8	/* CNS 11643-1992 Plane 5 */
#define LC_CNS11643_6	0xf9	/* CNS 11643-1992 Plane 6 */
#define LC_CNS11643_7	0xfa	/* CNS 11643-1992 Plane 7 */
#define LC_INDIAN_2_COLUMN	0xfb/* Indian charset for 2-column width
								 * glypps (not supported) */
#define LC_TIBETAN	0xfc		/* Tibetan (not supported) */
/* #define FREE		0xfd	free (unused) */
/* #define FREE		0xfe	free (unused) */
/* #define FREE		0xff	free (unused) */

/*
 * Encoding numeral identificators
 *
 * WARNING: the order of this table must be same as order
 *			in the pg_enconv[] (mb/conv.c) and pg_enc2name[] (mb/encnames.c) array!
 *
 *			If you add some encoding don'y forget check
 *			PG_ENCODING_[BE|FE]_LAST macros.
 *
 *		The PG_SQL_ASCII is default encoding and must be = 0.
 */
typedef enum pg_enc
{
	PG_SQL_ASCII = 0,			/* SQL/ASCII */
	PG_EUC_JP,					/* EUC for Japanese */
	PG_EUC_CN,					/* EUC for Chinese */
	PG_EUC_KR,					/* EUC for Korean */
	PG_EUC_TW,					/* EUC for Taiwan */
	PG_UTF8,					/* Unicode UTF-8 */
	PG_MULE_INTERNAL,			/* Mule internal code */
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
	PG_KOI8R,					/* KOI8-R */
	PG_WIN1251,					/* windows-1251 (was: WIN) */
	PG_ALT,						/* (MS-DOS CP866) */
	PG_ISO_8859_5,				/* ISO-8859-5 */
	PG_ISO_8859_6,				/* ISO-8859-6 */
	PG_ISO_8859_7,				/* ISO-8859-7 */
	PG_ISO_8859_8,				/* ISO-8859-8 */

	/* followings are for client encoding only */
	PG_SJIS,					/* Shift JIS */
	PG_BIG5,					/* Big5 */
	PG_WIN1250,					/* windows-1250 */

	_PG_LAST_ENCODING_			/* mark only */

} pg_enc;

#define PG_ENCODING_BE_LAST PG_ISO_8859_8
#define PG_ENCODING_FE_LAST PG_WIN1250


#ifdef MULTIBYTE

/*
 * Please use these tests before access to pg_encconv_tbl[]
 * or to other places...
 */
#define PG_VALID_BE_ENCODING(_enc) \
		((_enc) >= 0 && (_enc) <= PG_ENCODING_BE_LAST)

#define PG_ENCODING_IS_CLIEN_ONLY(_enc) \
		(((_enc) > PG_ENCODING_BE_LAST && (_enc) <= PG_ENCODING_FE_LAST)

#define PG_VALID_ENCODING(_enc) \
		((_enc) >= 0 && (_enc) < _PG_LAST_ENCODING_)

/* On FE are possible all encodings
 */
#define PG_VALID_FE_ENCODING(_enc)	PG_VALID_ENCODING(_enc)

/*
 * Encoding names with all aliases
 */
typedef struct pg_encname
{
	char	   *name;
	pg_enc		encoding;
} pg_encname;

extern pg_encname pg_encname_tbl[];
extern unsigned int pg_encname_tbl_sz;

/*
 * Careful:
 *
 * if (PG_VALID_ENCODING(encoding))
 *		pg_enc2name_tbl[ encoding ];
 */
typedef struct pg_enc2name
{
	char	   *name;
	pg_enc		encoding;
} pg_enc2name;

extern pg_enc2name pg_enc2name_tbl[];

extern pg_encname *pg_char_to_encname_struct(const char *name);

extern int	pg_char_to_encoding(const char *s);
extern const char *pg_encoding_to_char(int encoding);

typedef void (*to_mic_converter) (unsigned char *l, unsigned char *p, int len);
typedef void (*from_mic_converter) (unsigned char *mic, unsigned char *p, int len);

/*
 * The backend encoding conversion routines
 * Careful:
 *
 *	if (PG_VALID_ENCODING(enc))
 *		pg_encconv_tbl[ enc ]->foo
 */
#ifndef FRONTEND
typedef struct pg_enconv
{
	pg_enc		encoding;		/* encoding identifier */
	to_mic_converter to_mic;	/* client encoding to MIC */
	from_mic_converter from_mic;	/* MIC to client encoding */
	to_mic_converter to_unicode;	/* client encoding to UTF-8 */
	from_mic_converter from_unicode;	/* UTF-8 to client encoding */
} pg_enconv;

extern pg_enconv pg_enconv_tbl[];
extern pg_enconv *pg_get_enconv_by_encoding(int encoding);
#endif	 /* FRONTEND */

/*
 * pg_wchar stuff
 */
typedef int (*mb2wchar_with_len_converter) (const unsigned char *from,
														pg_wchar *to,
														int len);
typedef int (*mblen_converter) (const unsigned char *mbstr);

typedef struct
{
	mb2wchar_with_len_converter mb2wchar_with_len;		/* convert a multi-byte
														 * string to a wchar */
	mblen_converter mblen;		/* returns the length of a multi-byte char */
	int			maxmblen;		/* max bytes for a char in this charset */
} pg_wchar_tbl;

extern pg_wchar_tbl pg_wchar_table[];

/*
 * UTF-8 to local code conversion map
 */
typedef struct
{
	unsigned int utf;			/* UTF-8 */
	unsigned int code;			/* local code */
} pg_utf_to_local;

/*
 * local code to UTF-8 conversion map
 */
typedef struct
{
	unsigned int code;			/* local code */
	unsigned int utf;			/* UTF-8 */
} pg_local_to_utf;

extern int	pg_mb2wchar(const unsigned char *, pg_wchar *);
extern int	pg_mb2wchar_with_len(const unsigned char *, pg_wchar *, int);
extern int	pg_char_and_wchar_strcmp(const char *, const pg_wchar *);
extern int	pg_wchar_strncmp(const pg_wchar *, const pg_wchar *, size_t);
extern int	pg_char_and_wchar_strncmp(const char *, const pg_wchar *, size_t);
extern size_t pg_wchar_strlen(const pg_wchar *);
extern int	pg_mblen(const unsigned char *);
extern int	pg_encoding_mblen(int, const unsigned char *);
extern int	pg_mule_mblen(const unsigned char *);
extern int	pg_mic_mblen(const unsigned char *);
extern int	pg_mbstrlen(const unsigned char *);
extern int	pg_mbstrlen_with_len(const unsigned char *, int);
extern int	pg_mbcliplen(const unsigned char *, int, int);
extern int	pg_mbcharcliplen(const unsigned char *, int, int);
extern int	pg_encoding_max_length(int);
extern int	pg_database_encoding_max_length(void);

extern int	pg_set_client_encoding(int);
extern int	pg_get_client_encoding(void);
extern const char *pg_get_client_encoding_name(void);

extern void SetDatabaseEncoding(int);
extern int	GetDatabaseEncoding(void);
extern const char *GetDatabaseEncodingName(void);

extern int	pg_valid_client_encoding(const char *name);
extern int	pg_valid_server_encoding(const char *name);

extern int	pg_utf_mblen(const unsigned char *);
extern int pg_find_encoding_converters(int src, int dest,
							to_mic_converter *src_to_mic,
							from_mic_converter *dest_from_mic);
extern unsigned char *pg_do_encoding_conversion(unsigned char *src, int len,
						  to_mic_converter src_to_mic,
						  from_mic_converter dest_from_mic);

extern unsigned char *pg_client_to_server(unsigned char *, int);
extern unsigned char *pg_server_to_client(unsigned char *, int);

extern unsigned short BIG5toCNS(unsigned short, unsigned char *);
extern unsigned short CNStoBIG5(unsigned short, unsigned char);

char	   *pg_verifymbstr(const unsigned char *, int);
#endif	 /* MULTIBYTE */

#endif	 /* PG_WCHAR_H */
