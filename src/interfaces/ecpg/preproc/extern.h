/* src/interfaces/ecpg/preproc/extern.h */

#ifndef _ECPG_PREPROC_EXTERN_H
#define _ECPG_PREPROC_EXTERN_H

#include "type.h"
#include "parser/keywords.h"

#include <errno.h>
#ifndef CHAR_BIT
#include <limits.h>
#endif

/* defines */

#define STRUCT_DEPTH 128
#define EMPTY mm_strdup("")

/* variables */

extern bool autocommit,
			auto_create_c,
			system_includes,
			force_indicator,
			questionmarks,
			regression_mode,
			auto_prepare;
extern int	braces_open,
			ret_value,
			struct_level,
			ecpg_internal_var;
extern char *current_function;
extern char *descriptor_index;
extern char *descriptor_name;
extern char *connection;
extern char *input_filename;
extern char *yytext,
		   *token_start;

#ifdef YYDEBUG
extern int	yydebug;
#endif
extern int	yylineno;
extern FILE *yyin,
		   *yyout;
extern char *output_filename;

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

/* Globals from keywords.c */
extern const ScanKeyword SQLScanKeywords[];
extern const int NumSQLScanKeywords;

/* functions */

extern const char *get_dtype(enum ECPGdtype);
extern void lex_init(void);
extern void output_line_number(void);
extern void output_statement(char *, int, enum ECPG_statement_type);
extern void output_prepare_statement(char *, char *);
extern void output_deallocate_prepare_statement(char *);
extern void output_simple_statement(char *);
extern char *hashline_number(void);
extern int	base_yyparse(void);
extern int	base_yylex(void);
extern void base_yyerror(const char *);
extern void *mm_alloc(size_t), *mm_realloc(void *, size_t);
extern char *mm_strdup(const char *);
extern void mmerror(int errorcode, enum errortype type, const char *error,...) __attribute__((format(PG_PRINTF_ATTRIBUTE, 3, 4)));
extern void mmfatal(int errorcode, const char *error,...) __attribute__((format(PG_PRINTF_ATTRIBUTE, 2, 3), noreturn));
extern void output_get_descr_header(char *);
extern void output_get_descr(char *, char *);
extern void output_set_descr_header(char *);
extern void output_set_descr(char *, char *);
extern void push_assignment(char *, enum ECPGdtype);
extern struct variable *find_variable(char *);
extern void whenever_action(int);
extern void add_descriptor(char *, char *);
extern void drop_descriptor(char *, char *);
extern struct descriptor *lookup_descriptor(char *, char *);
extern struct variable *descriptor_variable(const char *name, int input);
extern struct variable *sqlda_variable(const char *name);
extern void add_variable_to_head(struct arguments **, struct variable *, struct variable *);
extern void add_variable_to_tail(struct arguments **, struct variable *, struct variable *);
extern void remove_variable_from_list(struct arguments ** list, struct variable * var);
extern void dump_variables(struct arguments *, int);
extern struct typedefs *get_typedef(char *);
extern void adjust_array(enum ECPGttype, char **, char **, char *, char *, int, bool);
extern void reset_variables(void);
extern void check_indicator(struct ECPGtype *);
extern void remove_typedefs(int);
extern void remove_variables(int);
extern struct variable *new_variable(const char *, struct ECPGtype *, int);
extern const ScanKeyword *ScanCKeywordLookup(const char *);
extern const ScanKeyword *ScanECPGKeywordLookup(const char *text);
extern void scanner_init(const char *);
extern void parser_init(void);
extern void scanner_finish(void);
extern int	filtered_base_yylex(void);

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
