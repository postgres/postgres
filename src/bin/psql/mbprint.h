/* $Id: mbprint.h,v 1.5 2002/08/27 20:16:48 petere Exp $ */
#ifndef MBPRINT_H
#define MBPRINT_H

#include "mb/pg_wchar.h"

pg_wchar	utf2ucs(const unsigned char *c);

unsigned char *mbvalidate(unsigned char *pwcs);

int			pg_wcswidth(unsigned char *pwcs, size_t len);

#endif   /* MBPRINT_H */
