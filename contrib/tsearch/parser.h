#ifndef __PARSER_H__
#define __PARSER_H__

char	   *token;
int			tokenlen;
int			tsearch_yylex(void);
void		start_parse_str(char *, int);
void		end_parse(void);

#endif
