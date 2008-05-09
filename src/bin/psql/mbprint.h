/* $PostgreSQL: pgsql/src/bin/psql/mbprint.h,v 1.12 2008/05/09 05:25:04 tgl Exp $ */
#ifndef MBPRINT_H
#define MBPRINT_H


struct lineptr
{
	unsigned char *ptr;
	int			width;
};

extern unsigned char *mbvalidate(unsigned char *pwcs, int encoding);

extern int	pg_wcswidth(const unsigned char *pwcs, size_t len, int encoding);
extern void pg_wcsformat(unsigned char *pwcs, size_t len, int encoding, struct lineptr * lines, int count);
extern void pg_wcssize(unsigned char *pwcs, size_t len, int encoding,
					   int *width, int *height, int *format_size);

#endif   /* MBPRINT_H */
