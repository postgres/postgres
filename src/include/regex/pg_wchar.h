/* $Id: pg_wchar.h,v 1.3 1998/06/16 07:29:43 momjian Exp $ */

#ifndef PG_WCHAR_H
#define PG_WCHAR_H

#include <sys/types.h>

#define EUC_JP 0	/* EUC for Japanese */
#define EUC_CN 1	/* EUC for Chinese */
#define EUC_KR 2	/* EUC for Korean */
#define EUC_TW 3	/* EUC for Taiwan */
#define UNICODE 4	/* Unicode UTF-8 */
#define MULE_INTERNAL 5	/* Mule internal code */
#define LATIN1 6	/* ISO-8859 Latin 1 */
#define LATIN2 7	/* ISO-8859 Latin 2 */
#define LATIN3 8	/* ISO-8859 Latin 3 */
#define LATIN4 9	/* ISO-8859 Latin 4 */
#define LATIN5 10	/* ISO-8859 Latin 5 */
/* followings are for client encoding only */
#define SJIS 16		/* Shift JIS */

#ifdef MB
# if LATIN1 <= MB && MB <= LATIN5
typedef unsigned char pg_wchar;
# else
typedef unsigned int pg_wchar;
# endif
#else
#define pg_wchar char
#endif

/*
 * various definitions for EUC
 */
#define SS2 0x8e	/* single shift 2 */
#define SS3 0x8f	/* single shift 3 */

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
#define	LC_ISO8859_1	0x81	/* ISO8859 Latin 1 */
#define	LC_ISO8859_2	0x82	/* ISO8859 Latin 2 */
#define	LC_ISO8859_3	0x83	/* ISO8859 Latin 3 */
#define	LC_ISO8859_4	0x84	/* ISO8859 Latin 4 */
#define	LC_ISO8859_5	0x8d	/* ISO8859 Latin 5 */
#define	LC_JISX0201K	0x89	/* Japanese 1 byte kana */
#define	LC_JISX0201R	0x90	/* Japanese 1 byte Roman */
#define	LC_GB2312_80	0x91	/* Chinese */
#define	LC_JISX0208	0x92	/* Japanese Kanji */
#define	LC_KS5601	0x93	/* Korean */
#define	LC_JISX0212	0x94	/* Japanese Kanji (JISX0212) */
#define	LC_CNS11643_1	0x95	/* CNS 11643-1992 Plane 1 */
#define	LC_CNS11643_2	0x96	/* CNS 11643-1992 Plane 2 */
#define	LC_CNS11643_3	0xf6	/* CNS 11643-1992 Plane 3 */
#define	LC_CNS11643_4	0xf7	/* CNS 11643-1992 Plane 4 */
#define	LC_CNS11643_5	0xf8	/* CNS 11643-1992 Plane 5 */
#define	LC_CNS11643_6	0xf9	/* CNS 11643-1992 Plane 6 */
#define	LC_CNS11643_7	0xfa	/* CNS 11643-1992 Plane 7 */

#ifdef MB
extern void pg_mb2wchar(const unsigned char *, pg_wchar *);
extern void pg_mb2wchar_with_len(const unsigned char *, pg_wchar *, int);
extern int pg_char_and_wchar_strcmp(const char *, const pg_wchar *);
extern int pg_wchar_strncmp(const pg_wchar *, const pg_wchar *, size_t);
extern int pg_char_and_wchar_strncmp(const char *, const pg_wchar *, size_t);
extern size_t pg_wchar_strlen(const pg_wchar *);
extern int pg_mblen(const unsigned char *);
extern int pg_encoding_mblen(int, const unsigned char *);
extern int pg_mic_mblen(const unsigned char *);
extern int pg_mbstrlen(const unsigned char *);
extern int pg_mbstrlen_with_len(const unsigned char *, int);
#endif

#endif
