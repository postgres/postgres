#include "parser/keywords.h"
#include "type.h"
#include <errno.h>

/* defines */

#define STRUCT_DEPTH 128
#define EMPTY make_str("")

/* variables */

extern int	braces_open,
		autocommit,
		ret_value,
		struct_level;
extern char *descriptor_index;
extern char *descriptor_name;
extern char *connection;
extern char 	*input_filename;;
extern char 	*yytext, errortext[128];
extern int	yylineno, yyleng;
extern FILE *yyin, *yyout;

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
extern struct descriptor *descriptors;

/* functions */

extern const char *get_dtype(enum ECPGdtype);
extern void lex_init(void);
extern char *make_str(const char *);
extern void output_line_number(void);
extern void output_statement(char *, int, char *, char *, struct arguments *, struct arguments *);
extern void output_simple_statement(char *);
extern char *hashline_number(void);
extern int	yyparse(void);
extern int	yylex(void);
extern void yyerror(char *);
extern void *mm_alloc(size_t), *mm_realloc(void *, size_t);
extern char *mm_strdup(const char *);
extern void mmerror(enum errortype, char * );
extern ScanKeyword *ScanECPGKeywordLookup(char *);
extern ScanKeyword *ScanCKeywordLookup(char *);
extern void output_get_descr_header(char *);
extern void output_get_descr(char *, char *);
extern void push_assignment(char *, enum ECPGdtype);
extern struct variable * find_variable(char *);
extern void whenever_action(int);
extern void add_descriptor(char *,char *);
extern void drop_descriptor(char *,char *);
extern struct descriptor *lookup_descriptor(char *,char *);
extern void add_variable(struct arguments ** , struct variable * , struct variable *);
extern void dump_variables(struct arguments *, int);
extern struct typedefs *get_typedef(char *);
extern void adjust_array(enum ECPGttype, int *, int *, int, int, bool);
extern void reset_variables(void);
extern void check_indicator(struct ECPGtype *);
extern void remove_variables(int);
extern struct variable *new_variable(const char *, struct ECPGtype *);
 
/* return codes */

#define OK			 0
#define PARSE_ERROR		-1
#define ILLEGAL_OPTION		-2
#define INDICATOR_NOT_ARRAY	-3

#define NO_INCLUDE_FILE		ENOENT
#define OUT_OF_MEMORY		ENOMEM
