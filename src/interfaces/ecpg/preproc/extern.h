/* variables */

extern int	debugging,
			braces_open;
extern char *yytext;
extern int	yylineno,
			yyleng;
extern FILE *yyin,
		   *yyout;


/* functions */

extern void lex_init(void);
extern char *input_filename;
extern int	yyparse(void);
extern void *mm_alloc(size_t), *mm_realloc(void *, size_t);
