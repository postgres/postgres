/* $Id: mbprint.h,v 1.3 2001/10/28 06:25:58 momjian Exp $ */
#ifndef MBPRINT_H
#define MBPRINT_H


#ifdef MULTIBYTE

#include "mb/pg_wchar.h"

pg_wchar	utf2ucs(const unsigned char *c);

unsigned char *mbvalidate(unsigned char *pwcs);
#endif	 /* MULTIBYTE */

int			pg_wcswidth(unsigned char *pwcs, int len);

#endif	 /* MBPRINT_H */
