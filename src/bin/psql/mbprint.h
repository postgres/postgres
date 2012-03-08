/* src/bin/psql/mbprint.h */
#ifndef MBPRINT_H
#define MBPRINT_H


struct lineptr
{
	unsigned char *ptr;
	int			width;
};

extern unsigned char *mbvalidate(unsigned char *pwcs, int encoding);
extern int	pg_wcswidth(const char *pwcs, size_t len, int encoding);
extern void pg_wcsformat(const unsigned char *pwcs, size_t len, int encoding, struct lineptr * lines, int count);
extern void pg_wcssize(const unsigned char *pwcs, size_t len, int encoding,
		   int *width, int *height, int *format_size);

#endif   /* MBPRINT_H */
