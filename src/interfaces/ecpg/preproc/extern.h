#include "parser/keywords.h"

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

struct cursor {	char *name;
		char *command;
		struct cursor *next;
	      };

extern struct cursor *cur;

/* functions */

extern void lex_init(void);
extern char *input_filename;
extern int	yyparse(void);
extern void *mm_alloc(size_t), *mm_realloc(void *, size_t);
ScanKeyword * ScanECPGKeywordLookup(char *);
ScanKeyword * ScanCKeywordLookup(char *);
extern void yyerror(char *);
