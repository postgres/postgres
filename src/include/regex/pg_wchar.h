/* $Id: pg_wchar.h,v 1.1 1998/03/15 07:38:47 scrappy Exp $ */

#ifndef PG_WCHAR_H
#define PG_WCHAR_H

#include <sys/types.h>

#define EUC_JP 0	/* EUC for Japanese */
#define EUC_CN 1	/* EUC for Chinese */
#define EUC_KR 2	/* EUC for Korean */
#define EUC_TW 3	/* EUC for Taiwan */
#define UNICODE 4	/* Unicode UTF-8 */
#define MULE_INTERNAL 5	/* Mule internal code */

#ifdef MB
typedef unsigned int pg_wchar;
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

#ifdef MB
extern void pg_mb2wchar(const unsigned char *, pg_wchar *);
extern void pg_mb2wchar_with_len(const unsigned char *, pg_wchar *, int);
extern int pg_char_and_wchar_strcmp(const char *, const pg_wchar *);
extern int pg_wchar_strncmp(const pg_wchar *, const pg_wchar *, size_t);
extern int pg_char_and_wchar_strncmp(const char *, const pg_wchar *, size_t);
extern size_t pg_wchar_strlen(const pg_wchar *);
#endif

#endif
