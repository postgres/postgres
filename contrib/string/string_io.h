#ifndef STRING_IO_H
#define STRING_IO_H

char	   *string_output(char *data, int size);
char	   *string_input(char *str, int size, int hdrsize, int *rtn_size);
char	   *c_charout(int32 c);
char	   *c_char2out(uint16 s);
char	   *c_char4out(uint32 s);
char	   *c_char8out(char *s);
char	   *c_char16out(char *s);
char	   *c_textout(struct varlena * vlena);
char	   *c_varcharout(char *s);

#if 0
struct varlena *c_textin(char *str);
char	   *c_char16in(char *str);

#endif

#endif

/*
 * Local variables:
 *	tab-width: 4
 *	c-indent-level: 4
 *	c-basic-offset: 4
 * End:
 */
