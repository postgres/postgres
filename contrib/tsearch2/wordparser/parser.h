#ifndef __PARSER_H__
#define __PARSER_H__

extern char	   *token;
extern int			tokenlen;
int			tsearch2_yylex(void);
void		tsearch2_start_parse_str(char *, int);
void		tsearch2_end_parse(void);

#endif
