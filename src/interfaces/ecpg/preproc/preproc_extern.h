/* src/interfaces/ecpg/preproc/preproc_extern.h */

#ifndef _ECPG_PREPROC_EXTERN_H
#define _ECPG_PREPROC_EXTERN_H

#include "common/keywords.h"
#include "type.h"

#ifndef CHAR_BIT
#include <limits.h>
#endif

/* defines */

#define STRUCT_DEPTH 128

/*
 * "Location tracking" support --- see ecpg.header for more comments.
 */
typedef const char *YYLTYPE;

#define YYLTYPE_IS_DECLARED 1

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
extern char *descriptor_name;
extern char *connection;
extern char *input_filename;
extern char *base_yytext,
		   *token_start;

#ifdef YYDEBUG
extern int	base_yydebug;
#endif
extern int	base_yylineno;
extern FILE *base_yyin,
		   *base_yyout;
extern char *output_filename;

extern struct _include_path *include_paths;
extern struct cursor *cur;
extern struct typedefs *types;
extern struct _defines *defines;
extern struct declared_list *g_declared_list;
extern struct ECPGtype ecpg_no_indicator;
extern struct variable no_indicator;
extern struct arguments *argsinsert;
extern struct arguments *argsresult;
extern struct when when_error,
			when_nf,
			when_warn;
extern struct ECPGstruct_member *struct_member_list[STRUCT_DEPTH];

/* Globals from keywords.c */
extern const uint16 SQLScanKeywordTokens[];

/* functions */

extern const char *get_dtype(enum ECPGdtype);
extern void lex_init(void);
extern void output_line_number(void);
extern void output_statement(const char *stmt, int whenever_mode, enum ECPG_statement_type st);
extern void output_prepare_statement(const char *name, const char *stmt);
extern void output_deallocate_prepare_statement(const char *name);
extern void output_simple_statement(const char *stmt, int whenever_mode);
extern char *hashline_number(void);
extern int	base_yyparse(void);
extern int	base_yylex(void);
extern void base_yyerror(const char *error);
extern void *mm_alloc(size_t size);
extern char *mm_strdup(const char *string);
extern void *loc_alloc(size_t size);
extern char *loc_strdup(const char *string);
extern void reclaim_local_storage(void);
extern char *cat2_str(const char *str1, const char *str2);
extern char *cat_str(int count,...);
extern char *make2_str(const char *str1, const char *str2);
extern char *make3_str(const char *str1, const char *str2, const char *str3);
extern void mmerror(int error_code, enum errortype type, const char *error,...) pg_attribute_printf(3, 4);
pg_noreturn extern void mmfatal(int error_code, const char *error,...) pg_attribute_printf(2, 3);
extern void output_get_descr_header(const char *desc_name);
extern void output_get_descr(const char *desc_name, const char *index);
extern void output_set_descr_header(const char *desc_name);
extern void output_set_descr(const char *desc_name, const char *index);
extern void push_assignment(const char *var, enum ECPGdtype value);
extern struct variable *find_variable(const char *name);
extern void whenever_action(int mode);
extern void add_descriptor(const char *name, const char *connection);
extern void drop_descriptor(const char *name, const char *connection);
extern struct descriptor *lookup_descriptor(const char *name, const char *connection);
extern struct variable *descriptor_variable(const char *name, int input);
extern struct variable *sqlda_variable(const char *name);
extern void add_variable_to_head(struct arguments **list,
								 struct variable *var,
								 struct variable *ind);
extern void add_variable_to_tail(struct arguments **list,
								 struct variable *var,
								 struct variable *ind);
extern void remove_variable_from_list(struct arguments **list, struct variable *var);
extern void dump_variables(struct arguments *list, int mode);
extern struct typedefs *get_typedef(const char *name, bool noerror);
extern void adjust_array(enum ECPGttype type_enum, const char **dimension,
						 const char **length, const char *type_dimension,
						 const char *type_index, int pointer_len,
						 bool type_definition);
extern void reset_variables(void);
extern void check_indicator(struct ECPGtype *var);
extern void remove_typedefs(int brace_level);
extern void remove_variables(int brace_level);
extern struct variable *new_variable(const char *name,
									 struct ECPGtype *type,
									 int brace_level);
extern int	ScanCKeywordLookup(const char *text);
extern int	ScanECPGKeywordLookup(const char *text);
extern void parser_init(void);
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
	ECPG_COMPAT_PGSQL = 0, ECPG_COMPAT_INFORMIX, ECPG_COMPAT_INFORMIX_SE, ECPG_COMPAT_ORACLE
};
extern enum COMPAT_MODE compat;

#define INFORMIX_MODE	(compat == ECPG_COMPAT_INFORMIX || compat == ECPG_COMPAT_INFORMIX_SE)
#define ORACLE_MODE (compat == ECPG_COMPAT_ORACLE)


#endif							/* _ECPG_PREPROC_EXTERN_H */
