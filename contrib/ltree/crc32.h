#ifndef _CRC32_H
#define _CRC32_H

/* $PostgreSQL: pgsql/contrib/ltree/crc32.h,v 1.3 2006/03/11 04:38:29 momjian Exp $ */

/* Returns crc32 of data block */
extern unsigned int ltree_crc32_sz(char *buf, int size);

/* Returns crc32 of null-terminated string */
#define crc32(buf) ltree_crc32_sz((buf),strlen(buf))

#endif
