#include "parser/keywords.h"
#include <errno.h>

/* variables */

extern int	braces_open,
			no_auto_trans;
extern char *yytext;
extern int	yylineno,
			yyleng;
extern FILE *yyin,
		   *yyout;

struct _include_path
{
	char	   *path;
	struct _include_path *next;
};

extern struct _include_path *include_paths;

struct cursor
{
	char	   *name;
	char	   *command;
	struct arguments *argsinsert;
	struct arguments *argsresult;
	struct cursor *next;
};

extern struct cursor *cur;

struct _defines
{
	char	   *old;
	char	   *new;
	struct _defines *next;
};

extern struct _defines *defines;

/* This is a linked list of the variable names and types. */
struct variable
{
	char	   *name;
	struct ECPGtype *type;
	int			brace_level;
	struct variable *next;
};

extern struct ECPGtype ecpg_no_indicator;
extern struct variable no_indicator;

struct arguments
{
	struct variable *variable;
	struct variable *indicator;
	struct arguments *next;
};

extern struct arguments *argsinsert;
extern struct arguments *argsresult;

/* functions */

extern void lex_init(void);
extern char *input_filename;
extern int	yyparse(void);
extern void *mm_alloc(size_t), *mm_realloc(void *, size_t);
extern char *mm_strdup(const char *);
ScanKeyword *ScanECPGKeywordLookup(char *);
ScanKeyword *ScanCKeywordLookup(char *);
extern void yyerror(char *);

/* return codes */

#define OK		0
#define PARSE_ERROR -1
#define ILLEGAL_OPTION	-2

#define NO_INCLUDE_FILE ENOENT
#define OUT_OF_MEMORY	ENOMEM
