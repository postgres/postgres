#ifndef STRING_IO_H
#define STRING_IO_H

unsigned char*	string_output(unsigned char *data, int size);
unsigned char*	string_input(unsigned char *str, int size, int hdrsize,
							  int *rtn_size);
unsigned char*	c_charout(int32 c);
unsigned char*	c_textout(struct varlena * vlena);
unsigned char*	c_varcharout(unsigned char *s);

#if 0
struct varlena*	c_textin(unsigned char *str);
int32*			c_charin(unsigned char *str)
#endif

#endif

/*
 * Local Variables:
 *  tab-width: 4
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 */
