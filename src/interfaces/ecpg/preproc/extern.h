/* variables */

extern int	debugging,
			braces_open;
extern char *yytext;
extern int	yylineno,
			yyleng;
extern FILE *yyin,
		   *yyout;

struct _include_path {  char * path;
                        struct _include_path * next;
                     };

extern struct _include_path *include_paths;

/* functions */

extern void lex_init(void);
extern char *input_filename;
extern int	yyparse(void);
extern void *mm_alloc(size_t), *mm_realloc(void *, size_t);
