/* $Id: mbprint.h,v 1.6 2003/03/18 22:15:44 petere Exp $ */
#ifndef MBPRINT_H
#define MBPRINT_H

#include "mb/pg_wchar.h"

pg_wchar	utf2ucs(const unsigned char *c);

unsigned char *mbvalidate(unsigned char *pwcs, int encoding);

int			pg_wcswidth(unsigned char *pwcs, size_t len, int encoding);

#endif   /* MBPRINT_H */
