/* $Id: pg_wchar.h,v 1.4 1998/09/01 04:36:34 momjian Exp $ */

#ifndef PG_WCHAR_H
#define PG_WCHAR_H

#include <sys/types.h>
#include "postgres.h"
#include "miscadmin.h"			/* for getdatabaseencoding() */

#define SQL_ASCII 0				/* SQL/ASCII */
#define EUC_JP 1				/* EUC for Japanese */
#define EUC_CN 2				/* EUC for Chinese */
#define EUC_KR 3				/* EUC for Korean */
#define EUC_TW 3				/* EUC for Taiwan */
#define UNICODE 5				/* Unicode UTF-8 */
#define MULE_INTERNAL 6			/* Mule internal code */
#define LATIN1 7				/* ISO-8859 Latin 1 */
#define LATIN2 8				/* ISO-8859 Latin 2 */
#define LATIN3 9				/* ISO-8859 Latin 3 */
#define LATIN4 10				/* ISO-8859 Latin 4 */
#define LATIN5 11				/* ISO-8859 Latin 5 */
#define LATIN6 12				/* ISO-8859 Latin 6 */
#define LATIN7 13				/* ISO-8859 Latin 7 */
#define LATIN8 14				/* ISO-8859 Latin 8 */
#define LATIN9 15				/* ISO-8859 Latin 9 */
/* followings are for client encoding only */
#define SJIS 32					/* Shift JIS */

#ifdef MULTIBYTE
typedef unsigned int pg_wchar;

#else
#define pg_wchar char
#endif

/*
 * various definitions for EUC
 */
#define SS2 0x8e				/* single shift 2 */
#define SS3 0x8f				/* single shift 3 */

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
#define LC_JISX0201R	0x90	/* Japanese 1 byte Roman */
#define LC_GB2312_80	0x91	/* Chinese */
#define LC_JISX0208 0x92		/* Japanese Kanji */
#define LC_KS5601	0x93		/* Korean */
#define LC_JISX0212 0x94		/* Japanese Kanji (JISX0212) */
#define LC_CNS11643_1	0x95	/* CNS 11643-1992 Plane 1 */
#define LC_CNS11643_2	0x96	/* CNS 11643-1992 Plane 2 */
#define LC_CNS11643_3	0xf6	/* CNS 11643-1992 Plane 3 */
#define LC_CNS11643_4	0xf7	/* CNS 11643-1992 Plane 4 */
#define LC_CNS11643_5	0xf8	/* CNS 11643-1992 Plane 5 */
#define LC_CNS11643_6	0xf9	/* CNS 11643-1992 Plane 6 */
#define LC_CNS11643_7	0xfa	/* CNS 11643-1992 Plane 7 */

#ifdef MULTIBYTE
typedef struct
{
	int			encoding;		/* encoding symbol value */
	char	   *name;			/* encoding name */
	int			is_client_only; /* 0: server/client bothg supported 1:
								 * client only */
	void		(*to_mic) ();	/* client encoding to MIC */
	void		(*from_mic) (); /* MIC to client encoding */
}			pg_encoding_conv_tbl;

extern pg_encoding_conv_tbl pg_conv_tbl[];

typedef struct
{
	void		(*mb2wchar_with_len) ();		/* convert a multi-byte
												 * string to a wchar */
	int			(*mblen) ();	/* returns the length of a multi-byte word */
}			pg_wchar_tbl;

extern pg_wchar_tbl pg_wchar_table[];

extern void pg_mb2wchar(const unsigned char *, pg_wchar *);
extern void pg_mb2wchar_with_len(const unsigned char *, pg_wchar *, int);
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
extern pg_encoding_conv_tbl *pg_get_encent_by_encoding(int);
extern bool show_client_encoding(void);
extern bool reset_client_encoding(void);
extern bool parse_client_encoding(const char *);
extern bool show_server_encoding(void);
extern bool reset_server_encoding(void);
extern bool parse_server_encoding(const char *);
extern int	pg_set_client_encoding(int);
extern int	pg_get_client_encoding(void);
extern unsigned char *pg_client_to_server(unsigned char *, int);
extern unsigned char *pg_server_to_client(unsigned char *, int);
extern int	pg_valid_client_encoding(const char *);
extern const char *pg_encoding_to_char(int);
extern int	pg_char_to_encoding(const char *);
extern int	GetDatabaseEncoding(void);
extern void SetDatabaseEncoding(int);
extern void SetTemplateEncoding(int);
extern int	GetTemplateEncoding(void);

#endif	 /* MULTIBYTE */

#endif	 /* PG_WCHAR_H */
