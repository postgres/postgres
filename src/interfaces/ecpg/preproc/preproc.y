/* Copyright comment */
%{
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "type.h"
#include "extern.h"

static void yyerror(char *);

/*
 * Variables containing simple states.
 */
int	debugging = 0;
static int	struct_level = 0;

/* temporarily store record members while creating the data structure */
struct ECPGrecord_member *record_member_list[128] = { NULL };

/*
 * Handle the filename and line numbering.
 */
char * input_filename = NULL;

void
output_line_number()
{
    if (input_filename)
       fprintf(yyout, "\n#line %d \"%s\"\n", yylineno, input_filename);
}

/*
 * Handling of the variables.
 */

/*
 * brace level counter
 */
int braces_open;

/* This is a linked list of the variable names and types. */
struct variable
{
    char * name;
    struct ECPGtype * type;
    int brace_level;
    struct variable * next;
};

static struct variable * allvariables = NULL;

static struct variable *
find_variable(char * name)
{
    struct variable * p;

    for (p = allvariables; p; p = p->next)
    {
        if (strcmp(p->name, name) == 0)
	    return p;
    }

    {
	char * errorstring = (char *) malloc(strlen(name) + 100);

	sprintf(errorstring, "The variable :%s is not declared.", name);

	yyerror(errorstring);
    }
    return NULL;
}


static void
new_variable(const char * name, struct ECPGtype * type)
{
    struct variable * p = (struct variable*) malloc(sizeof(struct variable));

    p->name = strdup(name);
    p->type = type;
    p->brace_level = braces_open;

    p->next = allvariables;
    allvariables = p;
}

static void
remove_variables(int brace_level)
{
    struct variable * p, *prev;

    for (p = prev = allvariables; p; p = p ? p->next : NULL)
    {
	if (p->brace_level >= brace_level)
	{
	    /* remove it */
	    if (p == allvariables)
		prev = allvariables = p->next;
	    else
		prev->next = p->next;

	    ECPGfree_type(p->type);
	    free(p->name);
	    free(p);
	    p = prev;
	}
	else
	    prev = p;
    }
}


/*
 * Here are the variables that need to be handled on every request.
 * These are of two kinds: input and output.
 * I will make two lists for them.
 */
struct arguments {
    struct variable * variable;
    struct arguments * next;
};


static struct arguments * argsinsert = NULL;
static struct arguments * argsresult = NULL;

void
reset_variables()
{
    argsinsert = NULL;
    argsresult = NULL;
}


/* Add a variable to a request. */
void
add_variable(struct arguments ** list, struct variable * var)
{
    struct arguments * p = (struct arguments *)malloc(sizeof(struct arguments));
    p->variable = var;
    p->next = *list;
    *list = p;
}


/* Dump out a list of all the variable on this list.
   This is a recursive function that works from the end of the list and
   deletes the list as we go on.
 */
void
dump_variables(struct arguments * list)
{
    if (list == NULL)
    {
        return;
    }

    /* The list is build up from the beginning so lets first dump the
       end of the list:
     */

    dump_variables(list->next);

    /* Then the current element. */
    ECPGdump_a_type(yyout, list->variable->name, list->variable->type, NULL);

    /* Then release the list element. */
    free(list);
}
%}

%union {
    int				tagname;
    struct ECPGtemp_type	type;
    char *			symbolname;
    int				indexsize;
    enum ECPGttype		type_enum;
}

%token <tagname> SQL_START SQL_SEMI SQL_STRING SQL_INTO
%token <tagname> SQL_BEGIN SQL_END SQL_DECLARE SQL_SECTION SQL_INCLUDE 
%token <tagname> SQL_CONNECT SQL_OPEN
%token <tagname> SQL_COMMIT SQL_ROLLBACK

%token <tagname> S_SYMBOL S_LENGTH S_ANYTHING
%token <tagname> S_VARCHAR S_VARCHAR2
%token <tagname> S_EXTERN S_STATIC S_AUTO S_CONST S_REGISTER S_STRUCT
%token <tagname> S_UNSIGNED S_SIGNED
%token <tagname> S_LONG S_SHORT S_INT S_CHAR S_FLOAT S_DOUBLE S_BOOL
%token <tagname> '[' ']' ';' ',' '{' '}'

%type <type> type type_detailed varchar_type simple_type array_type struct_type
%type <symbolname> symbol
%type <tagname> maybe_storage_clause varchar_tag
%type <type_enum> simple_tag
%type <indexsize> index length
%type <tagname> canything sqlanything both_anything


%%
prog : statements;

statements : /* empty */
	   | statements statement;

statement : sqldeclaration
	  | sqlinclude
	  | sqlconnect
	  | sqlopen
	  | sqlcommit
	  | sqlrollback
	  | sqlstatement
	  | cthing
	  | blockstart
	  | blockend;

sqldeclaration : sql_startdeclare
		 variable_declarations
		 sql_enddeclare;

sql_startdeclare : SQL_START SQL_BEGIN SQL_DECLARE SQL_SECTION SQL_SEMI	{
    fprintf(yyout, "/* exec sql begin declare section */\n"); 
    output_line_number();
};
sql_enddeclare : SQL_START SQL_END SQL_DECLARE SQL_SECTION SQL_SEMI {
    fprintf(yyout,"/* exec sql end declare section */\n"); 
    output_line_number();
};

variable_declarations : /* empty */
		      | variable_declarations variable_declaration ;

/* Here is where we can enter support for typedef. */
variable_declaration : type ';'	{ 
    /* don't worry about our list when we're working on a struct */
    if (struct_level == 0)
    {
	new_variable($<type>1.name, $<type>1.typ);
	free($<type>1.name);
    }
    fprintf(yyout, ";"); 
}

symbol : S_SYMBOL { 
    char * name = (char *)malloc(yyleng + 1);

    strncpy(name, yytext, yyleng);
    name[yyleng] = '\0';

    $<symbolname>$ = name;
}

type : maybe_storage_clause type_detailed { $<type>$ = $<type>2; };
type_detailed : varchar_type { $<type>$ = $<type>1; }
	      | simple_type { $<type>$ = $<type>1; }
	      | array_type {$<type>$ = $<type>1; }
	      | struct_type {$<type>$ = $<type>1; };

varchar_type : varchar_tag symbol index {
    fprintf(yyout, "struct varchar_%s { int len; char arr[%d]; } %s", $<symbolname>2, $<indexsize>3, $<symbolname>2);
    if (struct_level == 0)
    {
	$<type>$.name = $<symbolname>2;
	$<type>$.typ = ECPGmake_varchar_type(ECPGt_varchar, $<indexsize>3);
    }
    else
	ECPGmake_record_member($<symbolname>2, ECPGmake_varchar_type(ECPGt_varchar, $<indexsize>3), &(record_member_list[struct_level-1]));
}

varchar_tag : S_VARCHAR { $<tagname>$ = $<tagname>1; }
            | S_VARCHAR2 { $<tagname>$ = $<tagname>1; };

simple_type : simple_tag symbol {
    fprintf(yyout, "%s %s", ECPGtype_name($<type_enum>1), $<symbolname>2);
    if (struct_level == 0)
    {
	$<type>$.name = $<symbolname>2;
	$<type>$.typ = ECPGmake_simple_type($<type_enum>1);
    }
    else
        ECPGmake_record_member($<symbolname>2, ECPGmake_simple_type($<type_enum>1), &(record_member_list[struct_level-1]));
}

array_type : simple_tag symbol index {
    fprintf(yyout, "%s %s [%d]", ECPGtype_name($<type_enum>1), $<symbolname>2, $<indexsize>3);
    if (struct_level == 0)
    {
	$<type>$.name = $<symbolname>2;
	$<type>$.typ = ECPGmake_array_type(ECPGmake_simple_type($<type_enum>1), $<indexsize>3);
    }
    else
	ECPGmake_record_member($<symbolname>2, ECPGmake_array_type(ECPGmake_simple_type($<type_enum>1), $<indexsize>3), &(record_member_list[struct_level-1]));
}

s_struct : S_STRUCT symbol {
    struct_level++;
    fprintf(yyout, "struct %s {", $<symbolname>2);
}

struct_type : s_struct '{' variable_declarations '}' symbol {
    struct_level--;
    if (struct_level == 0)
    {
	$<type>$.name = $<symbolname>5;
	$<type>$.typ = ECPGmake_record_type(record_member_list[struct_level]);
    }
    else
	ECPGmake_record_member($<symbolname>5, ECPGmake_record_type(record_member_list[struct_level]), &(record_member_list[struct_level-1])); 
    fprintf(yyout, "} %s", $<symbolname>5);
    record_member_list[struct_level] = NULL;
}

simple_tag : S_CHAR { $<type_enum>$ = ECPGt_char; }
           | S_UNSIGNED S_CHAR { $<type_enum>$ = ECPGt_unsigned_char; }
	   | S_SHORT { $<type_enum>$ = ECPGt_short; }
           | S_UNSIGNED S_SHORT { $<type_enum>$ = ECPGt_unsigned_short; }
	   | S_INT { $<type_enum>$ = ECPGt_int; }
           | S_UNSIGNED S_INT { $<type_enum>$ = ECPGt_unsigned_int; }
	   | S_LONG { $<type_enum>$ = ECPGt_long; }
           | S_UNSIGNED S_LONG { $<type_enum>$ = ECPGt_unsigned_long; }
           | S_FLOAT { $<type_enum>$ = ECPGt_float; }
           | S_DOUBLE { $<type_enum>$ = ECPGt_double; }
	   | S_BOOL { $<type_enum>$ = ECPGt_bool; };

maybe_storage_clause : S_EXTERN { fwrite(yytext, yyleng, 1, yyout); }
		       | S_STATIC { fwrite(yytext, yyleng, 1, yyout); }
		       | S_CONST { fwrite(yytext, yyleng, 1, yyout); }
		       | S_REGISTER { fwrite(yytext, yyleng, 1, yyout); }
		       | S_AUTO { fwrite(yytext, yyleng, 1, yyout); }
                       | /* empty */ { };
  	 
index : '[' length ']' {
    $<indexsize>$ = $<indexsize>2; 
};

length : S_LENGTH { $<indexsize>$ = atoi(yytext); }

sqlinclude : SQL_START SQL_INCLUDE { fprintf(yyout, "#include \""); }
	filename SQL_SEMI { fprintf(yyout, ".h\""); output_line_number(); };

filename : cthing
	 | filename cthing;

sqlconnect : SQL_START SQL_CONNECT { fprintf(yyout, "ECPGconnect(\""); }
	SQL_STRING { fwrite(yytext + 1, yyleng - 2, 1, yyout); }
	SQL_SEMI { fprintf(yyout, "\");"); output_line_number(); };

/* Open is an open cursor. Removed. */
sqlopen : SQL_START SQL_OPEN sqlgarbage SQL_SEMI { output_line_number(); };

sqlgarbage : /* Empty */
	   | sqlgarbage sqlanything;
	    

sqlcommit : SQL_START SQL_COMMIT SQL_SEMI {
    fprintf(yyout, "ECPGcommit(__LINE__);"); 
    output_line_number();
};
sqlrollback : SQL_START SQL_ROLLBACK SQL_SEMI {
    fprintf(yyout, "ECPGrollback(__LINE__);");
    output_line_number();
};

sqlstatement : SQL_START { /* Reset stack */
    reset_variables();
    fprintf(yyout, "ECPGdo(__LINE__, \"");
}
               sqlstatement_words
	       SQL_SEMI {  
    /* Dump */
    fprintf(yyout, "\", ");		   
    dump_variables(argsinsert);
    fprintf(yyout, "ECPGt_EOIT, ");
    dump_variables(argsresult);
    fprintf(yyout, "ECPGt_EORT );");
    output_line_number();
};

sqlstatement_words : sqlstatement_word
		   | sqlstatement_words sqlstatement_word;
	
sqlstatement_word : ':' symbol 
                  {
		      add_variable(&argsinsert, find_variable($2));
		      fprintf(yyout, " ;; ");
		  }
		  | SQL_INTO into_list { }
		  | sqlanything 
                  { 
		      fwrite(yytext, yyleng, 1, yyout);
		      fwrite(" ", 1, 1, yyout);
		  }
		  | SQL_INTO sqlanything 
		  {
		      fprintf(yyout, " into ");
		      fwrite(yytext, yyleng, 1, yyout);
		      fwrite(" ", 1, 1, yyout);
		  };

into_list : ':' symbol {
    add_variable(&argsresult, find_variable($2)); 
}
	  | into_list ',' ':' symbol{
    add_variable(&argsresult, find_variable($4)); 
};

cthing : canything {
    fwrite(yytext, yyleng, 1, yyout);
}

canything : both_anything
	  | SQL_INTO
	  | ';';

sqlanything : both_anything;

both_anything : S_LENGTH | S_VARCHAR | S_VARCHAR2 
	  | S_LONG | S_SHORT | S_INT | S_CHAR | S_FLOAT | S_DOUBLE | S_BOOL
	  | SQL_OPEN | SQL_CONNECT
	  | SQL_STRING
	  | SQL_BEGIN | SQL_END 
	  | SQL_DECLARE | SQL_SECTION 
	  | SQL_INCLUDE 
	  | S_SYMBOL
	  | S_STATIC | S_EXTERN | S_AUTO | S_CONST | S_REGISTER | S_STRUCT
	  | '[' | ']' | ','
	  | S_ANYTHING;

blockstart : '{' {
    braces_open++;
    fwrite(yytext, yyleng, 1, yyout);
}

blockend : '}' {
    remove_variables(braces_open--);
    fwrite(yytext, yyleng, 1, yyout);
}
%%
static void yyerror(char * error)
{
    fprintf(stderr, "%s\n", error);
    exit(1);
}
