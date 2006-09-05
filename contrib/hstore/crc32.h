#ifndef _CRC32_H
#define _CRC32_H

/* Returns crc32 of data block */
extern unsigned int crc32_sz(char *buf, int size);

/* Returns crc32 of null-terminated string */
#define crc32(buf) crc32_sz((buf),strlen(buf))

#endif
