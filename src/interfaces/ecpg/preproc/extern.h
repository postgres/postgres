#include "parser/keywords.h"
#include "type.h"
#include <errno.h>

/* defines */

#define STRUCT_DEPTH 128

/* variables */

extern int	braces_open,
		autocommit,
		ret_value,
		struct_level;
extern char *yytext,
			errortext[128];
extern int	yylineno,
			yyleng;
extern FILE *yyin,
		   *yyout;

extern struct _include_path *include_paths;
extern struct cursor *cur;
extern struct typedefs *types;
extern struct _defines *defines;
extern struct ECPGtype ecpg_no_indicator;
extern struct variable no_indicator;
extern struct arguments *argsinsert;
extern struct arguments *argsresult;
extern struct when when_error, when_nf, when_warn;
extern struct ECPGstruct_member *struct_member_list[STRUCT_DEPTH];

/* functions */

extern void output_line_number(void);
extern void lex_init(void);
extern char *input_filename;
extern int	yyparse(void);
extern int	yylex(void);
extern void yyerror(const char * error);
extern void *mm_alloc(size_t), *mm_realloc(void *, size_t);
extern char *mm_strdup(const char *);
extern void mmerror(enum errortype, char * );
ScanKeyword *ScanECPGKeywordLookup(char *);
ScanKeyword *ScanCKeywordLookup(char *);

/* return codes */

#define OK			 0
#define PARSE_ERROR		-1
#define ILLEGAL_OPTION		-2
#define INDICATOR_NOT_ARRAY	-3

#define NO_INCLUDE_FILE		ENOENT
#define OUT_OF_MEMORY		ENOMEM
