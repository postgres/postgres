/* $Id: pg_wchar.h,v 1.30 2001/09/11 04:50:36 ishii Exp $ */

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
 * various definitions for mule internal code
 */
#define IS_LC1(c)	((unsigned char)(c) >= 0x81 && (unsigned char)(c) <= 0x8f)
#define IS_LCPRV1(c)	((unsigned char)(c) == 0x9a || (unsigned char)(c) == 0x9b)
#define IS_LC2(c)	((unsigned char)(c) >= 0x90 && (unsigned char)(c) <= 0x99)
#define IS_LCPRV2(c)	((unsigned char)(c) == 0x9c || (unsigned char)(c) == 0x9d)

/*
 * leading characters
 */
#define LC_ISO8859_1	0x81	/* ISO8859 Latin 1 */
#define LC_ISO8859_2	0x82	/* ISO8859 Latin 2 */
#define LC_ISO8859_3	0x83	/* ISO8859 Latin 3 */
#define LC_ISO8859_4	0x84	/* ISO8859 Latin 4 */
#define LC_ISO8859_5	0x8d	/* ISO8859 Latin 5 */
#define LC_JISX0201K	0x89	/* Japanese 1 byte kana */
#define LC_JISX0201R	0x8a	/* Japanese 1 byte Roman */
#define LC_KOI8_R	0x8c	/* Cyrillic KOI8-R */
#define LC_KOI8_U	0x8c	/* Cyrillic KOI8-U */
#define LC_GB2312_80	0x91	/* Chinese */
#define LC_JISX0208	0x92	/* Japanese Kanji */
#define LC_KS5601	0x93	/* Korean */
#define LC_JISX0212	0x94	/* Japanese Kanji (JISX0212) */
#define LC_CNS11643_1	0x95	/* CNS 11643-1992 Plane 1 */
#define LC_CNS11643_2	0x96	/* CNS 11643-1992 Plane 2 */
#define LC_CNS11643_3	0xf6	/* CNS 11643-1992 Plane 3 */
#define LC_CNS11643_4	0xf7	/* CNS 11643-1992 Plane 4 */
#define LC_CNS11643_5	0xf8	/* CNS 11643-1992 Plane 5 */
#define LC_CNS11643_6	0xf9	/* CNS 11643-1992 Plane 6 */
#define LC_CNS11643_7	0xfa	/* CNS 11643-1992 Plane 7 */

/*
 * Encoding numeral identificators
 *
 * WARNING: the order of this table must be same as order 
 *          in the pg_enconv[] (mb/conv.c) and pg_enc2name[] (mb/names.c) array!
 *
 *          If you add some encoding don'y forget check  
 *          PG_ENCODING_[BE|FE]_LAST macros.
 *
 *	    The PG_SQL_ASCII is default encoding and must be = 0.
 */
typedef enum pg_enc
{
	PG_SQL_ASCII = 0,			/* SQL/ASCII */
	PG_EUC_JP,				/* EUC for Japanese */
	PG_EUC_CN,				/* EUC for Chinese */
	PG_EUC_KR,				/* EUC for Korean */
	PG_EUC_TW,				/* EUC for Taiwan */
	PG_UTF8,				/* Unicode UTF-8 */
	PG_MULE_INTERNAL,			/* Mule internal code */
	PG_LATIN1,				/* ISO-8859 Latin 1 */
	PG_LATIN2,				/* ISO-8859 Latin 2 */
	PG_LATIN3,				/* ISO-8859 Latin 3 */
	PG_LATIN4,				/* ISO-8859 Latin 4 */
	PG_LATIN5,				/* ISO-8859 Latin 5 */
	PG_KOI8R,				/* KOI8-R */
	PG_WIN1251,				/* windows-1251 (was: WIN) */
	PG_ALT,					/* (MS-DOS CP866) */
	
	/* followings are for client encoding only */
	PG_SJIS,				/* Shift JIS */
	PG_BIG5,				/* Big5 */
	PG_WIN1250,				/* windows-1250 */

	_PG_LAST_ENCODING_			/* mark only */

} pg_enc;

#define PG_ENCODING_BE_LAST	PG_ALT
#define PG_ENCODING_FE_LAST	PG_WIN1250


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
#define PG_VALID_FE_ENCODING(_enc) 	PG_VALID_ENCODING(_enc)

/* 
 * Encoding names with all aliases
 */
typedef struct pg_encname
{
	char	*name;
	pg_enc	encoding;
} pg_encname;

extern pg_encname	pg_encname_tbl[];
extern unsigned	int	pg_encname_tbl_sz;

/*
 * Careful:
 *
 * if (PG_VALID_ENCODING(encoding))
 *		pg_enc2name_tbl[ encoding ];
 */
typedef struct pg_enc2name
{
	char	*name;
	pg_enc	encoding;
} pg_enc2name;

extern pg_enc2name	pg_enc2name_tbl[];

extern pg_encname	*pg_char_to_encname_struct(const char *name);

extern int		pg_char_to_encoding(const char *s);
extern const char	*pg_encoding_to_char(int encoding);

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
	pg_enc		encoding;		/* encoding identificator */
	void		(*to_mic) ();		/* client encoding to MIC */
	void		(*from_mic) (); 	/* MIC to client encoding */
	void		(*to_unicode) ();	/* client encoding to UTF-8 */
	void		(*from_unicode) ();	/* UTF-8 to client encoding */
} pg_enconv;

extern pg_enconv pg_enconv_tbl[];
extern pg_enconv *pg_get_enconv_by_encoding(int encoding);

#endif	/* FRONTEND */

/*
 * pg_wchar stuff
 */
typedef struct
{
	int		(*mb2wchar_with_len) ();	/* convert a multi-byte	
							 * string to a wchar */
	int		(*mblen) ();			/* returns the length of a multi-byte word */
	int		maxmblen;			/* max bytes for a letter in this charset */

} pg_wchar_tbl;

extern pg_wchar_tbl pg_wchar_table[];

/*
 * UTF-8 to local code conversion map
 */
typedef struct
{
	unsigned int	utf;			/* UTF-8 */
	unsigned int	code;			/* local code */
} pg_utf_to_local;

/*
 * local code to UTF-8 conversion map
 */
typedef struct
{
	unsigned int	code;			/* local code */
	unsigned int	utf;			/* UTF-8 */
} pg_local_to_utf;

extern int	pg_mb2wchar(const unsigned char *, pg_wchar *);
extern int	pg_mb2wchar_with_len(const unsigned char *, pg_wchar *, int);
extern int	pg_char_and_wchar_strcmp(const char *, const pg_wchar *);
extern int	pg_wchar_strncmp(const pg_wchar *, const pg_wchar *, size_t);
extern int	pg_char_and_wchar_strncmp(const char *, const pg_wchar *, size_t);
extern size_t	pg_wchar_strlen(const pg_wchar *);
extern int	pg_mblen(const unsigned char *);
extern int	pg_encoding_mblen(int, const unsigned char *);
extern int	pg_mule_mblen(const unsigned char *);
extern int	pg_mic_mblen(const unsigned char *);
extern int	pg_mbstrlen(const unsigned char *);
extern int	pg_mbstrlen_with_len(const unsigned char *, int);
extern int	pg_mbcliplen(const unsigned char *, int, int);
extern int	pg_mbcharcliplen(const unsigned char *, int, int);

extern int		pg_set_client_encoding(int);
extern int		pg_get_client_encoding(void);
extern const char	*pg_get_client_encoding_name(void);

extern void		SetDatabaseEncoding(int);
extern int		GetDatabaseEncoding(void);
extern const char	*GetDatabaseEncodingName(void);

extern int	pg_valid_client_encoding(const char *name);
extern int	pg_valid_server_encoding(const char *name);

extern int	pg_utf_mblen(const unsigned char *);
extern int	pg_find_encoding_converters(int, int, void (**)(), void (**)());
extern unsigned char *pg_do_encoding_conversion(unsigned char *, int, void (*)(), void (*)());

extern unsigned char *pg_client_to_server(unsigned char *, int);
extern unsigned char *pg_server_to_client(unsigned char *, int);

extern unsigned short BIG5toCNS(unsigned short, unsigned char *);
extern unsigned short CNStoBIG5(unsigned short, unsigned char);

char *pg_verifymbstr(const unsigned char *, int);

#endif	 /* MULTIBYTE */

#endif	 /* PG_WCHAR_H */
