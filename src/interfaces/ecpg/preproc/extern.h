#ifndef _ECPG_PREPROC_EXTERN_H
#define _ECPG_PREPROC_EXTERN_H

#include "type.h"

#include <errno.h>

/* defines */

#define STRUCT_DEPTH 128
#define EMPTY make_str("")

/* variables */

extern int	braces_open,
			autocommit,
			auto_create_c,
			system_includes,
			force_indicator,
			ret_value,
			struct_level,
			ecpg_informix_var;
extern char *descriptor_index;
extern char *descriptor_name;
extern char *connection;
extern char *input_filename;
extern char *yytext,
		   *token_start,
			errortext[128];

#ifdef YYDEBUG
extern int	yydebug;
#endif
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
extern struct when when_error,
			when_nf,
			when_warn;
extern struct ECPGstruct_member *struct_member_list[STRUCT_DEPTH];

/* functions */

extern const char *get_dtype(enum ECPGdtype);
extern void lex_init(void);
extern char *make_str(const char *);
extern void output_line_number(void);
extern void output_statement(char *, int, char *);
extern void output_simple_statement(char *);
extern char *hashline_number(void);
extern int	yyparse(void);
extern int	yylex(void);
extern void yyerror(char *);
extern void *mm_alloc(size_t), *mm_realloc(void *, size_t);
extern char *mm_strdup(const char *);
extern void mmerror(int, enum errortype, char *);
extern ScanKeyword *ScanECPGKeywordLookup(char *);
extern ScanKeyword *ScanCKeywordLookup(char *);
extern void output_get_descr_header(char *);
extern void output_get_descr(char *, char *);
extern void push_assignment(char *, enum ECPGdtype);
extern struct variable *find_variable(char *);
extern void whenever_action(int);
extern void add_descriptor(char *, char *);
extern void drop_descriptor(char *, char *);
extern struct descriptor *lookup_descriptor(char *, char *);
extern struct variable *descriptor_variable(const char *name, int input);
extern void add_variable_to_head(struct arguments **, struct variable *, struct variable *);
extern void add_variable_to_tail(struct arguments **, struct variable *, struct variable *);
extern void dump_variables(struct arguments *, int);
extern struct typedefs *get_typedef(char *);
extern void adjust_array(enum ECPGttype, char **, char **, char *, char *, int, bool);
extern void reset_variables(void);
extern void check_indicator(struct ECPGtype *);
extern void remove_typedefs(int);
extern void remove_variables(int);
extern struct variable *new_variable(const char *, struct ECPGtype *, int);
extern ScanKeyword *ScanKeywordLookup(char *text);

/* return codes */

#define ILLEGAL_OPTION		1
#define NO_INCLUDE_FILE		2
#define PARSE_ERROR			3
#define INDICATOR_NOT_ARRAY 4
#define OUT_OF_MEMORY		5
#define INDICATOR_NOT_STRUCT	6
#define INDICATOR_NOT_SIMPLE	7

enum COMPAT_MODE
{
	ECPG_COMPAT_PGSQL = 0, ECPG_COMPAT_INFORMIX, ECPG_COMPAT_INFORMIX_SE
};
extern enum COMPAT_MODE compat;

#define INFORMIX_MODE	(compat == ECPG_COMPAT_INFORMIX || compat == ECPG_COMPAT_INFORMIX_SE)

#endif   /* _ECPG_PREPROC_EXTERN_H */
