
/*  A Bison parser, made from gram.y
 by  GNU Bison version 1.25
  */

#define YYBISON 1  /* Identify Bison output.  */

#define	K_ALIAS	258
#define	K_ASSIGN	259
#define	K_BEGIN	260
#define	K_CONSTANT	261
#define	K_DEBUG	262
#define	K_DECLARE	263
#define	K_DEFAULT	264
#define	K_DOTDOT	265
#define	K_ELSE	266
#define	K_END	267
#define	K_EXCEPTION	268
#define	K_EXIT	269
#define	K_FOR	270
#define	K_FROM	271
#define	K_IF	272
#define	K_IN	273
#define	K_INTO	274
#define	K_LOOP	275
#define	K_NOT	276
#define	K_NOTICE	277
#define	K_NULL	278
#define	K_PERFORM	279
#define	K_RAISE	280
#define	K_RECORD	281
#define	K_RENAME	282
#define	K_RETURN	283
#define	K_REVERSE	284
#define	K_SELECT	285
#define	K_THEN	286
#define	K_TO	287
#define	K_TYPE	288
#define	K_WHEN	289
#define	K_WHILE	290
#define	T_FUNCTION	291
#define	T_TRIGGER	292
#define	T_CHAR	293
#define	T_BPCHAR	294
#define	T_VARCHAR	295
#define	T_LABEL	296
#define	T_STRING	297
#define	T_VARIABLE	298
#define	T_ROW	299
#define	T_ROWTYPE	300
#define	T_RECORD	301
#define	T_RECFIELD	302
#define	T_TGARGV	303
#define	T_DTYPE	304
#define	T_WORD	305
#define	T_NUMBER	306
#define	T_ERROR	307
#define	O_OPTION	308
#define	O_DUMP	309

#line 1 "gram.y"

/**********************************************************************
 * gram.y		- Parser for the PL/pgSQL
 *			  procedural language
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/pl/plpgsql/src/Attic/gram.c,v 1.3 1999/01/28 11:50:41 wieck Exp $
 *
 *    This software is copyrighted by Jan Wieck - Hamburg.
 *
 *    The author hereby grants permission  to  use,  copy,  modify,
 *    distribute,  and  license this software and its documentation
 *    for any purpose, provided that existing copyright notices are
 *    retained  in  all  copies  and  that  this notice is included
 *    verbatim in any distributions. No written agreement, license,
 *    or  royalty  fee  is required for any of the authorized uses.
 *    Modifications to this software may be  copyrighted  by  their
 *    author  and  need  not  follow  the licensing terms described
 *    here, provided that the new terms are  clearly  indicated  on
 *    the first page of each file where they apply.
 *
 *    IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY
 *    PARTY  FOR  DIRECT,   INDIRECT,   SPECIAL,   INCIDENTAL,   OR
 *    CONSEQUENTIAL   DAMAGES  ARISING  OUT  OF  THE  USE  OF  THIS
 *    SOFTWARE, ITS DOCUMENTATION, OR ANY DERIVATIVES THEREOF, EVEN
 *    IF  THE  AUTHOR  HAVE BEEN ADVISED OF THE POSSIBILITY OF SUCH
 *    DAMAGE.
 *
 *    THE  AUTHOR  AND  DISTRIBUTORS  SPECIFICALLY   DISCLAIM   ANY
 *    WARRANTIES,  INCLUDING,  BUT  NOT  LIMITED  TO,  THE  IMPLIED
 *    WARRANTIES  OF  MERCHANTABILITY,  FITNESS  FOR  A  PARTICULAR
 *    PURPOSE,  AND NON-INFRINGEMENT.  THIS SOFTWARE IS PROVIDED ON
 *    AN "AS IS" BASIS, AND THE AUTHOR  AND  DISTRIBUTORS  HAVE  NO
 *    OBLIGATION   TO   PROVIDE   MAINTENANCE,   SUPPORT,  UPDATES,
 *    ENHANCEMENTS, OR MODIFICATIONS.
 *
 **********************************************************************/

#include "stdio.h"
#include "string.h"
#include "plpgsql.h"

extern	int	yylineno;
extern	char	yytext[];

static	PLpgSQL_expr	*read_sqlstmt(int until, char *s, char *sqlstart);
static	PLpgSQL_stmt	*make_select_stmt(void);
static	PLpgSQL_expr	*make_tupret_expr(PLpgSQL_row *row);


#line 52 "gram.y"
typedef union {
	int32			ival;
	char			*str;
	struct {
	    char *name;
	    int  lineno;
	}			varname;
	struct {
	    int  nalloc;
	    int	 nused;
	    int	 *dtnums;
	}			dtlist;
	struct {
	    int  reverse;
	    PLpgSQL_expr *expr;
	}			forilow;
	struct {
	    char *label;
	    int  n_initvars;
	    int  *initvarnos;
	}			declhdr;
	PLpgSQL_type		*dtype;
	PLpgSQL_var		*var;
	PLpgSQL_row		*row;
	PLpgSQL_rec		*rec;
	PLpgSQL_recfield	*recfield;
	PLpgSQL_trigarg		*trigarg;
	PLpgSQL_expr		*expr;
	PLpgSQL_stmt		*stmt;
	PLpgSQL_stmts		*stmts;
	PLpgSQL_stmt_block	*program;
	PLpgSQL_nsitem		*nsitem;
} YYSTYPE;
#include <stdio.h>

#ifndef __cplusplus
#ifndef __STDC__
#define const
#endif
#endif



#define	YYFINAL		179
#define	YYFLAG		-32768
#define	YYNTBASE	61

#define YYTRANSLATE(x) ((unsigned)(x) <= 309 ? yytranslate[x] : 118)

static const char yytranslate[] = {     0,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,    58,
    59,     2,     2,    60,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,    55,    56,
     2,    57,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     1,     2,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    12,    13,    14,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    25,
    26,    27,    28,    29,    30,    31,    32,    33,    34,    35,
    36,    37,    38,    39,    40,    41,    42,    43,    44,    45,
    46,    47,    48,    49,    50,    51,    52,    53,    54
};

#if YYDEBUG != 0
static const short yyprhs[] = {     0,
     0,     4,     8,     9,    11,    14,    16,    19,    26,    28,
    31,    35,    37,    40,    42,    48,    50,    52,    58,    62,
    66,    72,    78,    80,    82,    84,    86,    87,    89,    91,
    93,    96,    99,   104,   105,   109,   111,   112,   115,   117,
   119,   121,   123,   124,   126,   129,   131,   133,   135,   137,
   139,   141,   143,   145,   147,   149,   151,   153,   155,   157,
   161,   166,   168,   170,   179,   180,   183,   188,   194,   203,
   205,   207,   209,   210,   219,   221,   223,   226,   231,   234,
   241,   247,   249,   251,   253,   255,   258,   260,   263,   266,
   269,   274,   277,   279,   281,   282,   283,   284,   285,   291,
   292,   294,   296,   299,   301
};

static const short yyrhs[] = {    36,
    62,    65,     0,    37,    62,    65,     0,     0,    63,     0,
    63,    64,     0,    64,     0,    53,    54,     0,    66,     5,
   117,    83,    12,    55,     0,   113,     0,   113,    67,     0,
   113,    67,    68,     0,     8,     0,    68,    69,     0,    69,
     0,    56,    56,   116,    57,    57,     0,     8,     0,    70,
     0,    73,    75,    76,    80,    81,     0,    73,    26,    55,
     0,    73,    72,    55,     0,    73,     3,    15,    71,    55,
     0,    27,    74,    32,    74,    55,     0,    50,     0,    44,
     0,    50,     0,    50,     0,     0,     6,     0,    77,     0,
    49,     0,    38,    78,     0,    40,    78,     0,    39,    58,
    79,    59,     0,     0,    58,    79,    59,     0,    51,     0,
     0,    21,    23,     0,    55,     0,    82,     0,     4,     0,
     9,     0,     0,    84,     0,    84,    85,     0,    85,     0,
    65,     0,    87,     0,    89,     0,    91,     0,    92,     0,
    93,     0,    97,     0,    99,     0,   100,     0,   101,     0,
   102,     0,   108,     0,    86,     0,    24,   117,   110,     0,
    88,   117,     4,   110,     0,    43,     0,    47,     0,    17,
   117,   111,    83,    90,    12,    17,    55,     0,     0,    11,
    83,     0,   113,    20,   117,   107,     0,   113,    35,   117,
   112,   107,     0,   113,    15,   117,    94,    18,    96,   112,
   107,     0,    95,     0,    43,     0,    50,     0,     0,   113,
    15,   117,    98,    18,    30,   112,   107,     0,    46,     0,
    44,     0,    30,   117,     0,    14,   117,   114,   115,     0,
    28,   117,     0,    25,   117,   104,   103,   105,    55,     0,
    25,   117,   104,   103,    55,     0,    42,     0,    13,     0,
    22,     0,     7,     0,   105,   106,     0,   106,     0,    60,
    43,     0,    60,    47,     0,    60,    48,     0,    83,    12,
    20,    55,     0,   109,   117,     0,    50,     0,    52,     0,
     0,     0,     0,     0,    56,    56,   116,    57,    57,     0,
     0,    41,     0,    55,     0,    34,   110,     0,    50,     0,
     0
};

#endif

#if YYDEBUG != 0
static const short yyrline[] = { 0,
   184,   188,   194,   195,   198,   199,   202,   208,   229,   237,
   245,   257,   263,   267,   273,   277,   281,   287,   306,   320,
   330,   335,   341,   362,   368,   375,   381,   383,   387,   393,
   397,   408,   414,   422,   426,   432,   438,   440,   444,   446,
   502,   503,   505,   513,   519,   529,   545,   547,   549,   551,
   553,   555,   557,   559,   561,   563,   565,   567,   569,   573,
   589,   605,   613,   619,   636,   644,   648,   666,   685,   707,
   734,   739,   746,   816,   845,   849,   855,   862,   878,   924,
   940,   957,   963,   967,   971,   977,   989,   998,  1002,  1006,
  1012,  1016,  1029,  1031,  1035,  1039,  1043,  1047,  1052,  1059,
  1061,  1065,  1067,  1071,  1075
};
#endif


#if YYDEBUG != 0 || defined (YYERROR_VERBOSE)

static const char * const yytname[] = {   "$","error","$undefined.","K_ALIAS",
"K_ASSIGN","K_BEGIN","K_CONSTANT","K_DEBUG","K_DECLARE","K_DEFAULT","K_DOTDOT",
"K_ELSE","K_END","K_EXCEPTION","K_EXIT","K_FOR","K_FROM","K_IF","K_IN","K_INTO",
"K_LOOP","K_NOT","K_NOTICE","K_NULL","K_PERFORM","K_RAISE","K_RECORD","K_RENAME",
"K_RETURN","K_REVERSE","K_SELECT","K_THEN","K_TO","K_TYPE","K_WHEN","K_WHILE",
"T_FUNCTION","T_TRIGGER","T_CHAR","T_BPCHAR","T_VARCHAR","T_LABEL","T_STRING",
"T_VARIABLE","T_ROW","T_ROWTYPE","T_RECORD","T_RECFIELD","T_TGARGV","T_DTYPE",
"T_WORD","T_NUMBER","T_ERROR","O_OPTION","O_DUMP","';'","'<'","'>'","'('","')'",
"','","pl_function","comp_optsect","comp_options","comp_option","pl_block","decl_sect",
"decl_start","decl_stmts","decl_stmt","decl_statement","decl_aliasitem","decl_rowtype",
"decl_varname","decl_renname","decl_const","decl_datatype","decl_dtypename",
"decl_atttypmod","decl_atttypmodval","decl_notnull","decl_defval","decl_defkey",
"proc_sect","proc_stmts","proc_stmt","stmt_perform","stmt_assign","assign_var",
"stmt_if","stmt_else","stmt_loop","stmt_while","stmt_fori","fori_var","fori_varname",
"fori_lower","stmt_fors","fors_target","stmt_select","stmt_exit","stmt_return",
"stmt_raise","raise_msg","raise_level","raise_params","raise_param","loop_body",
"stmt_execsql","execsql_start","expr_until_semi","expr_until_then","expr_until_loop",
"opt_label","opt_exitlabel","opt_exitcond","opt_lblname","lno", NULL
};
#endif

static const short yyr1[] = {     0,
    61,    61,    62,    62,    63,    63,    64,    65,    66,    66,
    66,    67,    68,    68,    69,    69,    69,    70,    70,    70,
    70,    70,    71,    72,    73,    74,    75,    75,    76,    77,
    77,    77,    77,    78,    78,    79,    80,    80,    81,    81,
    82,    82,    83,    83,    84,    84,    85,    85,    85,    85,
    85,    85,    85,    85,    85,    85,    85,    85,    85,    86,
    87,    88,    88,    89,    90,    90,    91,    92,    93,    94,
    95,    95,    96,    97,    98,    98,    99,   100,   101,   102,
   102,   103,   104,   104,   104,   105,   105,   106,   106,   106,
   107,   108,   109,   109,   110,   111,   112,   113,   113,   114,
   114,   115,   115,   116,   117
};

static const short yyr2[] = {     0,
     3,     3,     0,     1,     2,     1,     2,     6,     1,     2,
     3,     1,     2,     1,     5,     1,     1,     5,     3,     3,
     5,     5,     1,     1,     1,     1,     0,     1,     1,     1,
     2,     2,     4,     0,     3,     1,     0,     2,     1,     1,
     1,     1,     0,     1,     2,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     3,
     4,     1,     1,     8,     0,     2,     4,     5,     8,     1,
     1,     1,     0,     8,     1,     1,     2,     4,     2,     6,
     5,     1,     1,     1,     1,     2,     1,     2,     2,     2,
     4,     2,     1,     1,     0,     0,     0,     0,     5,     0,
     1,     1,     2,     1,     0
};

static const short yydefact[] = {     0,
     3,     3,     0,    98,     4,     6,    98,     7,     0,     1,
     0,     9,     5,     2,     0,   105,    12,    10,   104,     0,
    98,    16,     0,    25,     0,    11,    14,    17,    27,     0,
   105,   105,   105,   105,   105,   105,    62,    63,    93,    94,
    47,     0,    98,    46,    59,    48,   105,    49,    50,    51,
    52,    53,    54,    55,    56,    57,    58,   105,     9,    26,
     0,     0,    13,     0,    28,     0,    24,     0,     0,    99,
   100,    96,    95,     0,    79,    77,     0,    45,     0,    92,
   105,   105,   105,     0,     0,     0,    19,    20,    34,     0,
    34,    30,    37,    29,   101,     0,    98,    60,    85,    83,
    84,     0,     8,    95,     0,    98,    97,     0,     0,    23,
     0,     0,    31,     0,    32,     0,     0,    95,   102,    78,
    65,    82,     0,    61,    71,    76,    75,    72,     0,    70,
     0,     0,    67,    98,    22,    15,    21,    36,     0,     0,
    38,    41,    42,    39,    18,    40,   103,    98,     0,    81,
     0,     0,    87,    73,     0,     0,    68,    35,    33,    66,
     0,    88,    89,    90,    80,    86,    97,    97,     0,     0,
    98,    98,    91,    64,    69,    74,     0,     0,     0
};

static const short yydefgoto[] = {   177,
     4,     5,     6,    41,    11,    18,    26,    27,    28,   111,
    68,    29,    61,    69,    93,    94,   113,   139,   117,   145,
   146,   132,    43,    44,    45,    46,    47,    48,   149,    49,
    50,    51,   129,   130,   167,    52,   131,    53,    54,    55,
    56,   123,   102,   152,   153,   133,    57,    58,    98,    97,
   134,    59,    96,   120,    20,    21
};

static const short yypact[] = {   -15,
   -40,   -40,   -39,   -13,   -40,-32768,   -13,-32768,    -2,-32768,
    36,    59,-32768,-32768,    19,-32768,-32768,     0,-32768,    21,
    70,-32768,    22,-32768,    25,     0,-32768,-32768,    13,    26,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,    76,    12,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,    10,-32768,
    57,    19,-32768,    87,-32768,    48,-32768,    51,    69,-32768,
    63,-32768,-32768,    58,-32768,-32768,    55,-32768,   107,-32768,
-32768,-32768,-32768,    22,    62,    64,-32768,-32768,    54,    65,
    54,-32768,    94,-32768,-32768,   -24,    49,-32768,-32768,-32768,
-32768,    74,-32768,-32768,    47,    70,-32768,    66,    67,-32768,
    73,    78,-32768,    78,-32768,   102,     3,-32768,-32768,-32768,
   119,-32768,   -22,-32768,-32768,-32768,-32768,-32768,   113,-32768,
   114,   121,-32768,    70,-32768,-32768,-32768,-32768,    75,    77,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,    70,   123,-32768,
     1,    15,-32768,-32768,   108,   117,-32768,-32768,-32768,-32768,
   122,-32768,-32768,-32768,-32768,-32768,-32768,-32768,    85,    86,
    70,    70,-32768,-32768,-32768,-32768,   142,   143,-32768
};

static const short yypgoto[] = {-32768,
   144,-32768,   139,     7,-32768,-32768,-32768,   124,-32768,-32768,
-32768,-32768,    61,-32768,-32768,-32768,    56,    34,-32768,-32768,
-32768,   -21,-32768,   106,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,    -1,  -125,-32768,-32768,   -84,-32768,
   -82,    28,-32768,-32768,    90,   -30
};


#define	YYLAST		152


static const short yytable[] = {    42,
    71,    72,    73,    74,    75,    76,   142,    22,   157,   118,
    10,   143,     3,    14,     8,    64,    79,    17,    65,   124,
     1,     2,   -44,   -44,    81,    31,    23,    80,    32,    82,
   119,    12,   150,   147,    12,    33,    34,   151,    66,    35,
    16,    36,     9,   162,    83,   175,   176,   163,   164,    24,
   105,   106,   107,    15,    37,    25,    67,   144,    38,   -43,
   -43,    39,    31,    40,    99,    32,    17,     9,    19,   165,
   100,    60,    33,    34,   151,   121,    35,    30,    36,   101,
    62,   -43,    70,    31,   171,   172,    32,    77,    84,   125,
   126,    37,   127,    33,    34,    38,   128,    35,    39,    36,
    40,    86,    87,    95,     9,    88,    89,    90,    91,   103,
   104,   112,    37,   110,   116,   122,    38,    92,   109,    39,
   135,    40,   114,   136,   141,     9,   160,   137,   138,   148,
   154,   155,   156,   158,   161,   159,   169,   168,   170,   173,
   174,   178,   179,    13,   108,     7,   115,   140,    78,    63,
   166,    85
};

static const short yycheck[] = {    21,
    31,    32,    33,    34,    35,    36,     4,     8,   134,    34,
     4,     9,    53,     7,    54,     3,    47,     8,     6,   104,
    36,    37,    11,    12,    15,    14,    27,    58,    17,    20,
    55,     4,    55,   118,     7,    24,    25,    60,    26,    28,
     5,    30,    56,    43,    35,   171,   172,    47,    48,    50,
    81,    82,    83,    56,    43,    56,    44,    55,    47,    11,
    12,    50,    14,    52,     7,    17,     8,    56,    50,    55,
    13,    50,    24,    25,    60,    97,    28,    57,    30,    22,
    56,    12,    57,    14,   167,   168,    17,    12,    32,    43,
    44,    43,    46,    24,    25,    47,    50,    28,    50,    30,
    52,    15,    55,    41,    56,    55,    38,    39,    40,    55,
     4,    58,    43,    50,    21,    42,    47,    49,    57,    50,
    55,    52,    58,    57,    23,    56,   148,    55,    51,    11,
    18,    18,    12,    59,    12,    59,    20,    30,    17,    55,
    55,     0,     0,     5,    84,     2,    91,   114,    43,    26,
   152,    62
};
/* -*-C-*-  Note some compilers choke on comments on `#line' lines.  */
#line 3 "/usr/share/bison.simple"

/* Skeleton output parser for bison,
   Copyright (C) 1984, 1989, 1990 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* As a special exception, when this file is copied by Bison into a
   Bison output file, you may use that output file without restriction.
   This special exception was added by the Free Software Foundation
   in version 1.24 of Bison.  */

#ifndef alloca
#ifdef __GNUC__
#define alloca __builtin_alloca
#else /* not GNU C.  */
#if (!defined (__STDC__) && defined (sparc)) || defined (__sparc__) || defined (__sparc) || defined (__sgi)
#include <alloca.h>
#else /* not sparc */
#if defined (MSDOS) && !defined (__TURBOC__)
#include <malloc.h>
#else /* not MSDOS, or __TURBOC__ */
#if defined(_AIX)
#include <malloc.h>
 #pragma alloca
#else /* not MSDOS, __TURBOC__, or _AIX */
#ifdef __hpux
#ifdef __cplusplus
extern "C" {
void *alloca (unsigned int);
};
#else /* not __cplusplus */
void *alloca ();
#endif /* not __cplusplus */
#endif /* __hpux */
#endif /* not _AIX */
#endif /* not MSDOS, or __TURBOC__ */
#endif /* not sparc.  */
#endif /* not GNU C.  */
#endif /* alloca not defined.  */

#ifdef __cplusplus
extern "C" {
  void yyerror(char *);
  int yylex();
};
#else
  extern void yyerror(char *);
  extern int yylex();
#endif

/* This is the parser code that is written into each bison parser
  when the %semantic_parser declaration is not specified in the grammar.
  It was written by Richard Stallman by simplifying the hairy parser
  used when %semantic_parser is specified.  */

/* Note: there must be only one dollar sign in this file.
   It is replaced by the list of actions, each action
   as one case of the switch.  */

#define yyerrok		(yyerrstatus = 0)
#define yyclearin	(yychar = YYEMPTY)
#define YYEMPTY		-2
#define YYEOF		0
#define YYACCEPT	return(0)
#define YYABORT 	return(1)
#define YYERROR		goto yyerrlab1
/* Like YYERROR except do call yyerror.
   This remains here temporarily to ease the
   transition to the new meaning of YYERROR, for GCC.
   Once GCC version 2 has supplanted version 1, this can go.  */
#define YYFAIL		goto yyerrlab
#define YYRECOVERING()  (!!yyerrstatus)
#define YYBACKUP(token, value) \
do								\
  if (yychar == YYEMPTY && yylen == 1)				\
    { yychar = (token), yylval = (value);			\
      yychar1 = YYTRANSLATE (yychar);				\
      YYPOPSTACK;						\
      goto yybackup;						\
    }								\
  else								\
    { yyerror ("syntax error: cannot back up"); YYERROR; }	\
while (0)

#define YYTERROR	1
#define YYERRCODE	256

#ifndef YYPURE
#define YYLEX		yylex()
#endif

#ifdef YYPURE
#ifdef YYLSP_NEEDED
#ifdef YYLEX_PARAM
#define YYLEX		yylex(&yylval, &yylloc, YYLEX_PARAM)
#else
#define YYLEX		yylex(&yylval, &yylloc)
#endif
#else /* not YYLSP_NEEDED */
#ifdef YYLEX_PARAM
#define YYLEX		yylex(&yylval, YYLEX_PARAM)
#else
#define YYLEX		yylex(&yylval)
#endif
#endif /* not YYLSP_NEEDED */
#endif

/* If nonreentrant, generate the variables here */

#ifndef YYPURE

int	yychar;			/*  the lookahead symbol		*/
YYSTYPE	yylval;			/*  the semantic value of the		*/
				/*  lookahead symbol			*/

#ifdef YYLSP_NEEDED
YYLTYPE yylloc;			/*  location data for the lookahead	*/
				/*  symbol				*/
#endif

int yynerrs;			/*  number of parse errors so far       */
#endif  /* not YYPURE */

#if YYDEBUG != 0
int yydebug;			/*  nonzero means print parse trace	*/
/* Since this is uninitialized, it does not stop multiple parsers
   from coexisting.  */
#endif

/*  YYINITDEPTH indicates the initial size of the parser's stacks	*/

#ifndef	YYINITDEPTH
#define YYINITDEPTH 200
#endif

/*  YYMAXDEPTH is the maximum size the stacks can grow to
    (effective only if the built-in stack extension method is used).  */

#if YYMAXDEPTH == 0
#undef YYMAXDEPTH
#endif

#ifndef YYMAXDEPTH
#define YYMAXDEPTH 10000
#endif

#ifndef YYPARSE_RETURN_TYPE
#define YYPARSE_RETURN_TYPE int
#endif

/* Prevent warning if -Wstrict-prototypes.  */
#ifdef __GNUC__
YYPARSE_RETURN_TYPE yyparse (void);
#endif

#if __GNUC__ > 1		/* GNU C and GNU C++ define this.  */
#define __yy_memcpy(TO,FROM,COUNT)	__builtin_memcpy(TO,FROM,COUNT)
#else				/* not GNU C or C++ */
#ifndef __cplusplus

/* This is the most reliable way to avoid incompatibilities
   in available built-in functions on various systems.  */
static void
__yy_memcpy (to, from, count)
     char *to;
     char *from;
     int count;
{
  register char *f = from;
  register char *t = to;
  register int i = count;

  while (i-- > 0)
    *t++ = *f++;
}

#else /* __cplusplus */

/* This is the most reliable way to avoid incompatibilities
   in available built-in functions on various systems.  */
static void
__yy_memcpy (char *to, char *from, int count)
{
  register char *f = from;
  register char *t = to;
  register int i = count;

  while (i-- > 0)
    *t++ = *f++;
}

#endif
#endif

#line 196 "/usr/share/bison.simple"

/* The user can define YYPARSE_PARAM as the name of an argument to be passed
   into yyparse.  The argument should have type void *.
   It should actually point to an object.
   Grammar actions can access the variable by casting it
   to the proper pointer type.  */

#ifdef YYPARSE_PARAM
#ifdef __cplusplus
#define YYPARSE_PARAM_ARG void *YYPARSE_PARAM
#define YYPARSE_PARAM_DECL
#else /* not __cplusplus */
#define YYPARSE_PARAM_ARG YYPARSE_PARAM
#define YYPARSE_PARAM_DECL void *YYPARSE_PARAM;
#endif /* not __cplusplus */
#else /* not YYPARSE_PARAM */
#define YYPARSE_PARAM_ARG
#define YYPARSE_PARAM_DECL
#endif /* not YYPARSE_PARAM */

YYPARSE_RETURN_TYPE
yyparse(YYPARSE_PARAM_ARG)
     YYPARSE_PARAM_DECL
{
  register int yystate;
  register int yyn;
  register short *yyssp;
  register YYSTYPE *yyvsp;
  int yyerrstatus;	/*  number of tokens to shift before error messages enabled */
  int yychar1 = 0;		/*  lookahead token as an internal (translated) token number */

  short	yyssa[YYINITDEPTH];	/*  the state stack			*/
  YYSTYPE yyvsa[YYINITDEPTH];	/*  the semantic value stack		*/

  short *yyss = yyssa;		/*  refer to the stacks thru separate pointers */
  YYSTYPE *yyvs = yyvsa;	/*  to allow yyoverflow to reallocate them elsewhere */

#ifdef YYLSP_NEEDED
  YYLTYPE yylsa[YYINITDEPTH];	/*  the location stack			*/
  YYLTYPE *yyls = yylsa;
  YYLTYPE *yylsp;

#define YYPOPSTACK   (yyvsp--, yyssp--, yylsp--)
#else
#define YYPOPSTACK   (yyvsp--, yyssp--)
#endif

  int yystacksize = YYINITDEPTH;

#ifdef YYPURE
  int yychar;
  YYSTYPE yylval;
  int yynerrs;
#ifdef YYLSP_NEEDED
  YYLTYPE yylloc;
#endif
#endif

  YYSTYPE yyval;		/*  the variable used to return		*/
				/*  semantic values from the action	*/
				/*  routines				*/

  int yylen;

#if YYDEBUG != 0
  if (yydebug)
    fprintf(stderr, "Starting parse\n");
#endif

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY;		/* Cause a token to be read.  */

  /* Initialize stack pointers.
     Waste one element of value and location stack
     so that they stay on the same level as the state stack.
     The wasted elements are never initialized.  */

  yyssp = yyss - 1;
  yyvsp = yyvs;
#ifdef YYLSP_NEEDED
  yylsp = yyls;
#endif

/* Push a new state, which is found in  yystate  .  */
/* In all cases, when you get here, the value and location stacks
   have just been pushed. so pushing a state here evens the stacks.  */
yynewstate:

  *++yyssp = yystate;

  if (yyssp >= yyss + yystacksize - 1)
    {
      /* Give user a chance to reallocate the stack */
      /* Use copies of these so that the &'s don't force the real ones into memory. */
      YYSTYPE *yyvs1 = yyvs;
      short *yyss1 = yyss;
#ifdef YYLSP_NEEDED
      YYLTYPE *yyls1 = yyls;
#endif

      /* Get the current used size of the three stacks, in elements.  */
      int size = yyssp - yyss + 1;

#ifdef yyoverflow
      /* Each stack pointer address is followed by the size of
	 the data in use in that stack, in bytes.  */
#ifdef YYLSP_NEEDED
      /* This used to be a conditional around just the two extra args,
	 but that might be undefined if yyoverflow is a macro.  */
      yyoverflow("parser stack overflow",
		 &yyss1, size * sizeof (*yyssp),
		 &yyvs1, size * sizeof (*yyvsp),
		 &yyls1, size * sizeof (*yylsp),
		 &yystacksize);
#else
      yyoverflow("parser stack overflow",
		 &yyss1, size * sizeof (*yyssp),
		 &yyvs1, size * sizeof (*yyvsp),
		 &yystacksize);
#endif

      yyss = yyss1; yyvs = yyvs1;
#ifdef YYLSP_NEEDED
      yyls = yyls1;
#endif
#else /* no yyoverflow */
      /* Extend the stack our own way.  */
      if (yystacksize >= YYMAXDEPTH)
	{
	  yyerror("parser stack overflow");
	  return 2;
	}
      yystacksize *= 2;
      if (yystacksize > YYMAXDEPTH)
	yystacksize = YYMAXDEPTH;
      yyss = (short *) alloca (yystacksize * sizeof (*yyssp));
      __yy_memcpy ((char *)yyss, (char *)yyss1, size * sizeof (*yyssp));
      yyvs = (YYSTYPE *) alloca (yystacksize * sizeof (*yyvsp));
      __yy_memcpy ((char *)yyvs, (char *)yyvs1, size * sizeof (*yyvsp));
#ifdef YYLSP_NEEDED
      yyls = (YYLTYPE *) alloca (yystacksize * sizeof (*yylsp));
      __yy_memcpy ((char *)yyls, (char *)yyls1, size * sizeof (*yylsp));
#endif
#endif /* no yyoverflow */

      yyssp = yyss + size - 1;
      yyvsp = yyvs + size - 1;
#ifdef YYLSP_NEEDED
      yylsp = yyls + size - 1;
#endif

#if YYDEBUG != 0
      if (yydebug)
	fprintf(stderr, "Stack size increased to %d\n", yystacksize);
#endif

      if (yyssp >= yyss + yystacksize - 1)
	YYABORT;
    }

#if YYDEBUG != 0
  if (yydebug)
    fprintf(stderr, "Entering state %d\n", yystate);
#endif

  goto yybackup;
 yybackup:

/* Do appropriate processing given the current state.  */
/* Read a lookahead token if we need one and don't already have one.  */
/* yyresume: */

  /* First try to decide what to do without reference to lookahead token.  */

  yyn = yypact[yystate];
  if (yyn == YYFLAG)
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* yychar is either YYEMPTY or YYEOF
     or a valid token in external form.  */

  if (yychar == YYEMPTY)
    {
#if YYDEBUG != 0
      if (yydebug)
	fprintf(stderr, "Reading a token: ");
#endif
      yychar = YYLEX;
    }

  /* Convert token to internal form (in yychar1) for indexing tables with */

  if (yychar <= 0)		/* This means end of input. */
    {
      yychar1 = 0;
      yychar = YYEOF;		/* Don't call YYLEX any more */

#if YYDEBUG != 0
      if (yydebug)
	fprintf(stderr, "Now at end of input.\n");
#endif
    }
  else
    {
      yychar1 = YYTRANSLATE(yychar);

#if YYDEBUG != 0
      if (yydebug)
	{
	  fprintf (stderr, "Next token is %d (%s", yychar, yytname[yychar1]);
	  /* Give the individual parser a way to print the precise meaning
	     of a token, for further debugging info.  */
#ifdef YYPRINT
	  YYPRINT (stderr, yychar, yylval);
#endif
	  fprintf (stderr, ")\n");
	}
#endif
    }

  yyn += yychar1;
  if (yyn < 0 || yyn > YYLAST || yycheck[yyn] != yychar1)
    goto yydefault;

  yyn = yytable[yyn];

  /* yyn is what to do for this token type in this state.
     Negative => reduce, -yyn is rule number.
     Positive => shift, yyn is new state.
       New state is final state => don't bother to shift,
       just return success.
     0, or most negative number => error.  */

  if (yyn < 0)
    {
      if (yyn == YYFLAG)
	goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }
  else if (yyn == 0)
    goto yyerrlab;

  if (yyn == YYFINAL)
    YYACCEPT;

  /* Shift the lookahead token.  */

#if YYDEBUG != 0
  if (yydebug)
    fprintf(stderr, "Shifting token %d (%s), ", yychar, yytname[yychar1]);
#endif

  /* Discard the token being shifted unless it is eof.  */
  if (yychar != YYEOF)
    yychar = YYEMPTY;

  *++yyvsp = yylval;
#ifdef YYLSP_NEEDED
  *++yylsp = yylloc;
#endif

  /* count tokens shifted since error; after three, turn off error status.  */
  if (yyerrstatus) yyerrstatus--;

  yystate = yyn;
  goto yynewstate;

/* Do the default action for the current state.  */
yydefault:

  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;

/* Do a reduction.  yyn is the number of a rule to reduce with.  */
yyreduce:
  yylen = yyr2[yyn];
  if (yylen > 0)
    yyval = yyvsp[1-yylen]; /* implement default value of the action */

#if YYDEBUG != 0
  if (yydebug)
    {
      int i;

      fprintf (stderr, "Reducing via rule %d (line %d), ",
	       yyn, yyrline[yyn]);

      /* Print the symbols being reduced, and their result.  */
      for (i = yyprhs[yyn]; yyrhs[i] > 0; i++)
	fprintf (stderr, "%s ", yytname[yyrhs[i]]);
      fprintf (stderr, " -> %s\n", yytname[yyr1[yyn]]);
    }
#endif


  switch (yyn) {

case 1:
#line 185 "gram.y"
{
			yylval.program = (PLpgSQL_stmt_block *)yyvsp[0].stmt;
		    ;
    break;}
case 2:
#line 189 "gram.y"
{
			yylval.program = (PLpgSQL_stmt_block *)yyvsp[0].stmt;
		    ;
    break;}
case 7:
#line 203 "gram.y"
{
		        plpgsql_DumpExecTree = 1;
		    ;
    break;}
case 8:
#line 209 "gram.y"
{
		        PLpgSQL_stmt_block *new;

			new = malloc(sizeof(PLpgSQL_stmt_block));
			memset(new, 0, sizeof(PLpgSQL_stmt_block));

			new->cmd_type   = PLPGSQL_STMT_BLOCK;
			new->lineno     = yyvsp[-3].ival;
			new->label      = yyvsp[-5].declhdr.label;
			new->n_initvars = yyvsp[-5].declhdr.n_initvars;
			new->initvarnos = yyvsp[-5].declhdr.initvarnos;
			new->body       = yyvsp[-2].stmts;

			plpgsql_ns_pop();

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 9:
#line 230 "gram.y"
{
		        plpgsql_ns_setlocal(false);
			yyval.declhdr.label      = yyvsp[0].str;
			yyval.declhdr.n_initvars = 0;
			yyval.declhdr.initvarnos = NULL;
			plpgsql_add_initdatums(NULL);
		    ;
    break;}
case 10:
#line 238 "gram.y"
{
		        plpgsql_ns_setlocal(false);
			yyval.declhdr.label      = yyvsp[-1].str;
			yyval.declhdr.n_initvars = 0;
			yyval.declhdr.initvarnos = NULL;
			plpgsql_add_initdatums(NULL);
		    ;
    break;}
case 11:
#line 246 "gram.y"
{
		        plpgsql_ns_setlocal(false);
			if (yyvsp[0].str != NULL) {
			    yyval.declhdr.label = yyvsp[0].str;
			} else {
			    yyval.declhdr.label = yyvsp[-2].str;
			}
			yyval.declhdr.n_initvars = plpgsql_add_initdatums(&(yyval.declhdr.initvarnos));
		    ;
    break;}
case 12:
#line 258 "gram.y"
{
		        plpgsql_ns_setlocal(true);
		    ;
    break;}
case 13:
#line 264 "gram.y"
{
		        yyval.str = yyvsp[0].str;
		    ;
    break;}
case 14:
#line 268 "gram.y"
{
		        yyval.str = yyvsp[0].str;
		    ;
    break;}
case 15:
#line 274 "gram.y"
{
			yyval.str = yyvsp[-2].str;
		    ;
    break;}
case 16:
#line 278 "gram.y"
{
		        yyval.str = NULL;
		    ;
    break;}
case 17:
#line 282 "gram.y"
{
		        yyval.str = NULL;
		    ;
    break;}
case 18:
#line 288 "gram.y"
{
		        PLpgSQL_var	*new;

			new = malloc(sizeof(PLpgSQL_var));

			new->dtype	= PLPGSQL_DTYPE_VAR;
			new->refname	= yyvsp[-4].varname.name;
			new->lineno	= yyvsp[-4].varname.lineno;

			new->datatype	= yyvsp[-2].dtype;
			new->isconst	= yyvsp[-3].ival;
			new->notnull	= yyvsp[-1].ival;
			new->default_val = yyvsp[0].expr;

			plpgsql_adddatum((PLpgSQL_datum *)new);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, new->varno,
						yyvsp[-4].varname.name);
		    ;
    break;}
case 19:
#line 307 "gram.y"
{
		        PLpgSQL_rec	*new;

			new = malloc(sizeof(PLpgSQL_var));

			new->dtype	= PLPGSQL_DTYPE_REC;
			new->refname	= yyvsp[-2].varname.name;
			new->lineno	= yyvsp[-2].varname.lineno;

			plpgsql_adddatum((PLpgSQL_datum *)new);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_REC, new->recno,
						yyvsp[-2].varname.name);
		    ;
    break;}
case 20:
#line 321 "gram.y"
{
			yyvsp[-1].row->dtype	= PLPGSQL_DTYPE_ROW;
			yyvsp[-1].row->refname	= yyvsp[-2].varname.name;
			yyvsp[-1].row->lineno	= yyvsp[-2].varname.lineno;

			plpgsql_adddatum((PLpgSQL_datum *)yyvsp[-1].row);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_ROW, yyvsp[-1].row->rowno,
						yyvsp[-2].varname.name);
		    ;
    break;}
case 21:
#line 331 "gram.y"
{
		        plpgsql_ns_additem(yyvsp[-1].nsitem->itemtype,
					yyvsp[-1].nsitem->itemno, yyvsp[-4].varname.name);
		    ;
    break;}
case 22:
#line 336 "gram.y"
{
		        plpgsql_ns_rename(yyvsp[-3].str, yyvsp[-1].str);
		    ;
    break;}
case 23:
#line 342 "gram.y"
{
		        PLpgSQL_nsitem *nsi;
			char	*name;

			plpgsql_ns_setlocal(false);
			name = plpgsql_tolower(yytext);
			if (name[0] != '$') {
			    elog(ERROR, "can only alias positional parameters");
			}
			nsi = plpgsql_ns_lookup(name, NULL);
			if (nsi == NULL) {
			    elog(ERROR, "function has no parameter %s", name);
			}

			plpgsql_ns_setlocal(true);

			yyval.nsitem = nsi;
		    ;
    break;}
case 24:
#line 363 "gram.y"
{
		        yyval.row = yylval.row;
		    ;
    break;}
case 25:
#line 369 "gram.y"
{
		        yyval.varname.name = strdup(yytext);
			yyval.varname.lineno  = yylineno;
		    ;
    break;}
case 26:
#line 376 "gram.y"
{
		        yyval.str = plpgsql_tolower(yytext);
		    ;
    break;}
case 27:
#line 382 "gram.y"
{ yyval.ival = 0; ;
    break;}
case 28:
#line 384 "gram.y"
{ yyval.ival = 1; ;
    break;}
case 29:
#line 388 "gram.y"
{
		        yyval.dtype = yyvsp[0].dtype;
		    ;
    break;}
case 30:
#line 394 "gram.y"
{
			yyval.dtype = yylval.dtype;
		    ;
    break;}
case 31:
#line 398 "gram.y"
{
		        if (yyvsp[0].ival < 0) {
			    plpgsql_parse_word("char");
			    yyval.dtype = yylval.dtype;
			} else {
			    plpgsql_parse_word("bpchar");
			    yyval.dtype = yylval.dtype;
			    yyval.dtype->atttypmod = yyvsp[0].ival;
			}
		    ;
    break;}
case 32:
#line 409 "gram.y"
{
		        plpgsql_parse_word("varchar");
			yyval.dtype = yylval.dtype;
			yyval.dtype->atttypmod = yyvsp[0].ival;
		    ;
    break;}
case 33:
#line 415 "gram.y"
{
		        plpgsql_parse_word("bpchar");
			yyval.dtype = yylval.dtype;
			yyval.dtype->atttypmod = yyvsp[-1].ival;
		    ;
    break;}
case 34:
#line 423 "gram.y"
{
		        yyval.ival = -1;
		    ;
    break;}
case 35:
#line 427 "gram.y"
{
		        yyval.ival = yyvsp[-1].ival;
		    ;
    break;}
case 36:
#line 433 "gram.y"
{
		        yyval.ival = int2in(yytext) + VARHDRSZ;
		    ;
    break;}
case 37:
#line 439 "gram.y"
{ yyval.ival = 0; ;
    break;}
case 38:
#line 441 "gram.y"
{ yyval.ival = 1; ;
    break;}
case 39:
#line 445 "gram.y"
{ yyval.expr = NULL; ;
    break;}
case 40:
#line 447 "gram.y"
{
			int		tok;
			int		lno;
		        PLpgSQL_dstring	ds;
			PLpgSQL_expr	*expr;

			lno = yylineno;
			expr = malloc(sizeof(PLpgSQL_expr));
			plpgsql_dstring_init(&ds);
			plpgsql_dstring_append(&ds, "SELECT ");

			expr->dtype   = PLPGSQL_DTYPE_EXPR;
			expr->plan    = NULL;
			expr->nparams = 0;

			tok = yylex();
			switch (tok) {
			    case 0:
				plpgsql_error_lineno = lno;
				plpgsql_comperrinfo();
			    	elog(ERROR, "unexpected end of file");
			    case K_NULL:
			        if (yylex() != ';') {
				    plpgsql_error_lineno = lno;
				    plpgsql_comperrinfo();
				    elog(ERROR, "expectec ; after NULL");
				}
				free(expr);
				plpgsql_dstring_free(&ds);

				yyval.expr = NULL;
				break;

			    default:
				plpgsql_dstring_append(&ds, yytext);
				while ((tok = yylex()) != ';') {
				    if (tok == 0) {
					plpgsql_error_lineno = lno;
					plpgsql_comperrinfo();
					elog(ERROR, "unterminated default value");
				    }
				    if (plpgsql_SpaceScanned) {
					plpgsql_dstring_append(&ds, " ");
				    }
				    plpgsql_dstring_append(&ds, yytext);
				}
				expr->query = strdup(plpgsql_dstring_get(&ds));
				plpgsql_dstring_free(&ds);

				yyval.expr = expr;
				break;
			}
		    ;
    break;}
case 43:
#line 506 "gram.y"
{
				PLpgSQL_stmts	*new;

				new = malloc(sizeof(PLpgSQL_stmts));
				memset(new, 0, sizeof(PLpgSQL_stmts));
				yyval.stmts = new;
			;
    break;}
case 44:
#line 514 "gram.y"
{
				yyval.stmts = yyvsp[0].stmts;
			;
    break;}
case 45:
#line 520 "gram.y"
{
				if (yyvsp[-1].stmts->stmts_used == yyvsp[-1].stmts->stmts_alloc) {
				    yyvsp[-1].stmts->stmts_alloc *= 2;
				    yyvsp[-1].stmts->stmts = realloc(yyvsp[-1].stmts->stmts, sizeof(PLpgSQL_stmt *) * yyvsp[-1].stmts->stmts_alloc);
				}
				yyvsp[-1].stmts->stmts[yyvsp[-1].stmts->stmts_used++] = (struct PLpgSQL_stmt *)yyvsp[0].stmt;

				yyval.stmts = yyvsp[-1].stmts;
			;
    break;}
case 46:
#line 530 "gram.y"
{
				PLpgSQL_stmts	*new;

				new = malloc(sizeof(PLpgSQL_stmts));
				memset(new, 0, sizeof(PLpgSQL_stmts));

				new->stmts_alloc = 64;
				new->stmts_used  = 1;
				new->stmts = malloc(sizeof(PLpgSQL_stmt *) * new->stmts_alloc);
				new->stmts[0] = (struct PLpgSQL_stmt *)yyvsp[0].stmt;

				yyval.stmts = new;
			;
    break;}
case 47:
#line 546 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 48:
#line 548 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 49:
#line 550 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 50:
#line 552 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 51:
#line 554 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 52:
#line 556 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 53:
#line 558 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 54:
#line 560 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 55:
#line 562 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 56:
#line 564 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 57:
#line 566 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 58:
#line 568 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 59:
#line 570 "gram.y"
{ yyval.stmt = yyvsp[0].stmt; ;
    break;}
case 60:
#line 574 "gram.y"
{
		    	PLpgSQL_stmt_assign *new;

			new = malloc(sizeof(PLpgSQL_stmt_assign));
			memset(new, 0, sizeof(PLpgSQL_stmt_assign));

			new->cmd_type = PLPGSQL_STMT_ASSIGN;
			new->lineno   = yyvsp[-1].ival;
			new->varno = -1;
			new->expr  = yyvsp[0].expr;

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 61:
#line 590 "gram.y"
{
			PLpgSQL_stmt_assign *new;

			new = malloc(sizeof(PLpgSQL_stmt_assign));
			memset(new, 0, sizeof(PLpgSQL_stmt_assign));

			new->cmd_type = PLPGSQL_STMT_ASSIGN;
			new->lineno   = yyvsp[-2].ival;
			new->varno = yyvsp[-3].ival;
			new->expr  = yyvsp[0].expr;

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 62:
#line 606 "gram.y"
{
			if (yylval.var->isconst) {
			    plpgsql_comperrinfo();
			    elog(ERROR, "%s is declared CONSTANT", yylval.var->refname);
			}
		        yyval.ival = yylval.var->varno;
		    ;
    break;}
case 63:
#line 614 "gram.y"
{
		        yyval.ival = yylval.recfield->rfno;
		    ;
    break;}
case 64:
#line 620 "gram.y"
{
			PLpgSQL_stmt_if *new;

			new = malloc(sizeof(PLpgSQL_stmt_if));
			memset(new, 0, sizeof(PLpgSQL_stmt_if));

			new->cmd_type   = PLPGSQL_STMT_IF;
			new->lineno     = yyvsp[-6].ival;
			new->cond       = yyvsp[-5].expr;
			new->true_body  = yyvsp[-4].stmts;
			new->false_body = yyvsp[-3].stmts;

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 65:
#line 637 "gram.y"
{
				PLpgSQL_stmts	*new;

				new = malloc(sizeof(PLpgSQL_stmts));
				memset(new, 0, sizeof(PLpgSQL_stmts));
				yyval.stmts = new;
			;
    break;}
case 66:
#line 645 "gram.y"
{ yyval.stmts = yyvsp[0].stmts; ;
    break;}
case 67:
#line 649 "gram.y"
{
			PLpgSQL_stmt_loop *new;

			new = malloc(sizeof(PLpgSQL_stmt_loop));
			memset(new, 0, sizeof(PLpgSQL_stmt_loop));

			new->cmd_type = PLPGSQL_STMT_LOOP;
			new->lineno   = yyvsp[-1].ival;
			new->label    = yyvsp[-3].str;
			new->body     = yyvsp[0].stmts;

			plpgsql_ns_pop();

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 68:
#line 667 "gram.y"
{
			PLpgSQL_stmt_while *new;

			new = malloc(sizeof(PLpgSQL_stmt_while));
			memset(new, 0, sizeof(PLpgSQL_stmt_while));

			new->cmd_type = PLPGSQL_STMT_WHILE;
			new->lineno   = yyvsp[-2].ival;
			new->label    = yyvsp[-4].str;
			new->cond     = yyvsp[-1].expr;
			new->body     = yyvsp[0].stmts;

			plpgsql_ns_pop();

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 69:
#line 686 "gram.y"
{
			PLpgSQL_stmt_fori	*new;

			new = malloc(sizeof(PLpgSQL_stmt_fori));
			memset(new, 0, sizeof(PLpgSQL_stmt_fori));

			new->cmd_type = PLPGSQL_STMT_FORI;
			new->lineno   = yyvsp[-5].ival;
			new->label    = yyvsp[-7].str;
			new->var      = yyvsp[-4].var;
			new->reverse  = yyvsp[-2].forilow.reverse;
			new->lower    = yyvsp[-2].forilow.expr;
			new->upper    = yyvsp[-1].expr;
			new->body     = yyvsp[0].stmts;

			plpgsql_ns_pop();

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 70:
#line 708 "gram.y"
{
		        PLpgSQL_var	*new;

			new = malloc(sizeof(PLpgSQL_var));

			new->dtype	= PLPGSQL_DTYPE_VAR;
			new->refname	= yyvsp[0].varname.name;
			new->lineno	= yyvsp[0].varname.lineno;

			plpgsql_parse_word("integer");

			new->datatype	= yylval.dtype;
			new->isconst	= false;
			new->notnull	= false;
			new->default_val = NULL;

			plpgsql_adddatum((PLpgSQL_datum *)new);
			plpgsql_ns_additem(PLPGSQL_NSTYPE_VAR, new->varno,
						yyvsp[0].varname.name);

			plpgsql_add_initdatums(NULL);

		        yyval.var = new;
		    ;
    break;}
case 71:
#line 735 "gram.y"
{
		        yyval.varname.name = strdup(yytext);
			yyval.varname.lineno = yylineno;
		    ;
    break;}
case 72:
#line 740 "gram.y"
{
		        yyval.varname.name = strdup(yytext);
			yyval.varname.lineno = yylineno;
		    ;
    break;}
case 73:
#line 747 "gram.y"
{
			int			tok;
			int			lno;
			PLpgSQL_dstring	ds;
			int			nparams = 0;
			int			params[1024];
			char		buf[32];
			PLpgSQL_expr	*expr;
			int			firsttok = 1;

			lno = yylineno;
			plpgsql_dstring_init(&ds);
			plpgsql_dstring_append(&ds, "SELECT ");

			yyval.forilow.reverse = 0;
			while((tok = yylex()) != K_DOTDOT) {
			    if (firsttok) {
				firsttok = 0;
				if (tok == K_REVERSE) {
				    yyval.forilow.reverse = 1;
				    continue;
				}
			    }
			    if (tok == ';') break;
			    if (plpgsql_SpaceScanned) {
				plpgsql_dstring_append(&ds, " ");
			    }
			    switch (tok) {
				case T_VARIABLE:
				    params[nparams] = yylval.var->varno;
				    sprintf(buf, "$%d", ++nparams);
				    plpgsql_dstring_append(&ds, buf);
				    break;
				    
				case T_RECFIELD:
				    params[nparams] = yylval.recfield->rfno;
				    sprintf(buf, "$%d", ++nparams);
				    plpgsql_dstring_append(&ds, buf);
				    break;
				    
				case T_TGARGV:
				    params[nparams] = yylval.trigarg->dno;
				    sprintf(buf, "$%d", ++nparams);
				    plpgsql_dstring_append(&ds, buf);
				    break;
				    
				default:
				    if (tok == 0) {
					plpgsql_error_lineno = lno;
					plpgsql_comperrinfo();
					elog(ERROR, "missing .. to terminate lower bound of for loop");
				    }
				    plpgsql_dstring_append(&ds, yytext);
				    break;
			    }
			}

			expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * nparams - 1);
			expr->dtype		= PLPGSQL_DTYPE_EXPR;
			expr->query		= strdup(plpgsql_dstring_get(&ds));
			expr->plan		= NULL;
			expr->nparams	= nparams;
			while(nparams-- > 0) {
			    expr->params[nparams] = params[nparams];
			}
			plpgsql_dstring_free(&ds);
			yyval.forilow.expr = expr;
		    ;
    break;}
case 74:
#line 817 "gram.y"
{
			PLpgSQL_stmt_fors	*new;

			new = malloc(sizeof(PLpgSQL_stmt_fors));
			memset(new, 0, sizeof(PLpgSQL_stmt_fors));

			new->cmd_type = PLPGSQL_STMT_FORS;
			new->lineno   = yyvsp[-5].ival;
			new->label    = yyvsp[-7].str;
			switch (yyvsp[-4].rec->dtype) {
			    case PLPGSQL_DTYPE_REC:
			        new->rec = yyvsp[-4].rec;
				break;
			    case PLPGSQL_DTYPE_ROW:
			        new->row = (PLpgSQL_row *)yyvsp[-4].rec;
				break;
			    default:
				plpgsql_comperrinfo();
			        elog(ERROR, "unknown dtype %d in stmt_fors", yyvsp[-4].rec->dtype);
			}
			new->query = yyvsp[-1].expr;
			new->body  = yyvsp[0].stmts;

			plpgsql_ns_pop();

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 75:
#line 846 "gram.y"
{
		        yyval.rec = yylval.rec;
		    ;
    break;}
case 76:
#line 850 "gram.y"
{
		    	yyval.rec = (PLpgSQL_rec *)(yylval.row);
		    ;
    break;}
case 77:
#line 856 "gram.y"
{
		    	yyval.stmt = make_select_stmt();
			yyval.stmt->lineno = yyvsp[0].ival;
		    ;
    break;}
case 78:
#line 863 "gram.y"
{
			PLpgSQL_stmt_exit *new;

			new = malloc(sizeof(PLpgSQL_stmt_exit));
			memset(new, 0, sizeof(PLpgSQL_stmt_exit));

			new->cmd_type = PLPGSQL_STMT_EXIT;
			new->lineno   = yyvsp[-2].ival;
			new->label    = yyvsp[-1].str;
			new->cond     = yyvsp[0].expr;

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 79:
#line 879 "gram.y"
{
			PLpgSQL_stmt_return *new;
			PLpgSQL_expr	*expr = NULL;
			int		tok;

			new = malloc(sizeof(PLpgSQL_stmt_return));
			memset(new, 0, sizeof(PLpgSQL_stmt_return));

			if (plpgsql_curr_compile->fn_retistuple) {
			    new->retistuple = true;
			    new->retrecno   = -1;
			    switch (tok = yylex()) {
			        case K_NULL:
				    expr = NULL;
				    break;

			        case T_ROW:
				    expr = make_tupret_expr(yylval.row);
				    break;

				case T_RECORD:
				    new->retrecno = yylval.rec->recno;
				    expr = NULL;
				    break;

				default:
				    yyerror("return type mismatch in function returning table row");
				    break;
			    }
			    if (yylex() != ';') {
			        yyerror("expected ';'");
			    }
			} else {
			    new->retistuple = false;
			    expr = plpgsql_read_expression(';', ";");
			}

			new->cmd_type = PLPGSQL_STMT_RETURN;
			new->lineno   = yyvsp[0].ival;
			new->expr     = expr;

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 80:
#line 925 "gram.y"
{
		        PLpgSQL_stmt_raise	*new;

			new = malloc(sizeof(PLpgSQL_stmt_raise));

			new->cmd_type	= PLPGSQL_STMT_RAISE;
			new->lineno     = yyvsp[-4].ival;
			new->elog_level	= yyvsp[-3].ival;
			new->message	= yyvsp[-2].str;
			new->nparams	= yyvsp[-1].dtlist.nused;
			new->params	= malloc(sizeof(int) * yyvsp[-1].dtlist.nused);
			memcpy(new->params, yyvsp[-1].dtlist.dtnums, sizeof(int) * yyvsp[-1].dtlist.nused);

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 81:
#line 941 "gram.y"
{
		        PLpgSQL_stmt_raise	*new;

			new = malloc(sizeof(PLpgSQL_stmt_raise));

			new->cmd_type	= PLPGSQL_STMT_RAISE;
			new->lineno     = yyvsp[-3].ival;
			new->elog_level	= yyvsp[-2].ival;
			new->message	= yyvsp[-1].str;
			new->nparams	= 0;
			new->params	= NULL;

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 82:
#line 958 "gram.y"
{
		        yyval.str = strdup(yytext);
		    ;
    break;}
case 83:
#line 964 "gram.y"
{
		        yyval.ival = ERROR;
		    ;
    break;}
case 84:
#line 968 "gram.y"
{
		        yyval.ival = NOTICE;
		    ;
    break;}
case 85:
#line 972 "gram.y"
{
		        yyval.ival = DEBUG;
		    ;
    break;}
case 86:
#line 978 "gram.y"
{
		        if (yyvsp[-1].dtlist.nused == yyvsp[-1].dtlist.nalloc) {
			    yyvsp[-1].dtlist.nalloc *= 2;
			    yyvsp[-1].dtlist.dtnums = repalloc(yyvsp[-1].dtlist.dtnums, sizeof(int) * yyvsp[-1].dtlist.nalloc);
			}
			yyvsp[-1].dtlist.dtnums[yyvsp[-1].dtlist.nused++] = yyvsp[0].ival;

			yyval.dtlist.nalloc = yyvsp[-1].dtlist.nalloc;
			yyval.dtlist.nused  = yyvsp[-1].dtlist.nused;
			yyval.dtlist.dtnums = yyvsp[-1].dtlist.dtnums;
		    ;
    break;}
case 87:
#line 990 "gram.y"
{
		        yyval.dtlist.nalloc = 1;
			yyval.dtlist.nused  = 1;
			yyval.dtlist.dtnums = palloc(sizeof(int) * yyval.dtlist.nalloc);
			yyval.dtlist.dtnums[0] = yyvsp[0].ival;
		    ;
    break;}
case 88:
#line 999 "gram.y"
{
		        yyval.ival = yylval.var->varno;
		    ;
    break;}
case 89:
#line 1003 "gram.y"
{
		        yyval.ival = yylval.recfield->rfno;
		    ;
    break;}
case 90:
#line 1007 "gram.y"
{
		        yyval.ival = yylval.trigarg->dno;
		    ;
    break;}
case 91:
#line 1013 "gram.y"
{ yyval.stmts = yyvsp[-3].stmts; ;
    break;}
case 92:
#line 1017 "gram.y"
{
		        PLpgSQL_stmt_execsql	*new;

			new = malloc(sizeof(PLpgSQL_stmt_execsql));
			new->cmd_type = PLPGSQL_STMT_EXECSQL;
			new->lineno   = yyvsp[0].ival;
			new->sqlstmt  = read_sqlstmt(';', ";", yyvsp[-1].str);

			yyval.stmt = (PLpgSQL_stmt *)new;
		    ;
    break;}
case 93:
#line 1030 "gram.y"
{ yyval.str = strdup(yytext); ;
    break;}
case 94:
#line 1032 "gram.y"
{ yyval.str = strdup(yytext); ;
    break;}
case 95:
#line 1036 "gram.y"
{ yyval.expr = plpgsql_read_expression(';', ";"); ;
    break;}
case 96:
#line 1040 "gram.y"
{ yyval.expr = plpgsql_read_expression(K_THEN, "THEN"); ;
    break;}
case 97:
#line 1044 "gram.y"
{ yyval.expr = plpgsql_read_expression(K_LOOP, "LOOP"); ;
    break;}
case 98:
#line 1048 "gram.y"
{
			plpgsql_ns_push(NULL);
			yyval.str = NULL;
		    ;
    break;}
case 99:
#line 1053 "gram.y"
{
			plpgsql_ns_push(yyvsp[-2].str);
			yyval.str = yyvsp[-2].str;
		    ;
    break;}
case 100:
#line 1060 "gram.y"
{ yyval.str = NULL; ;
    break;}
case 101:
#line 1062 "gram.y"
{ yyval.str = strdup(yytext); ;
    break;}
case 102:
#line 1066 "gram.y"
{ yyval.expr = NULL; ;
    break;}
case 103:
#line 1068 "gram.y"
{ yyval.expr = yyvsp[0].expr; ;
    break;}
case 104:
#line 1072 "gram.y"
{ yyval.str = strdup(yytext); ;
    break;}
case 105:
#line 1076 "gram.y"
{
			plpgsql_error_lineno = yylineno;
		        yyval.ival = yylineno;
		    ;
    break;}
}
   /* the action file gets copied in in place of this dollarsign */
#line 498 "/usr/share/bison.simple"

  yyvsp -= yylen;
  yyssp -= yylen;
#ifdef YYLSP_NEEDED
  yylsp -= yylen;
#endif

#if YYDEBUG != 0
  if (yydebug)
    {
      short *ssp1 = yyss - 1;
      fprintf (stderr, "state stack now");
      while (ssp1 != yyssp)
	fprintf (stderr, " %d", *++ssp1);
      fprintf (stderr, "\n");
    }
#endif

  *++yyvsp = yyval;

#ifdef YYLSP_NEEDED
  yylsp++;
  if (yylen == 0)
    {
      yylsp->first_line = yylloc.first_line;
      yylsp->first_column = yylloc.first_column;
      yylsp->last_line = (yylsp-1)->last_line;
      yylsp->last_column = (yylsp-1)->last_column;
      yylsp->text = 0;
    }
  else
    {
      yylsp->last_line = (yylsp+yylen-1)->last_line;
      yylsp->last_column = (yylsp+yylen-1)->last_column;
    }
#endif

  /* Now "shift" the result of the reduction.
     Determine what state that goes to,
     based on the state we popped back to
     and the rule number reduced by.  */

  yyn = yyr1[yyn];

  yystate = yypgoto[yyn - YYNTBASE] + *yyssp;
  if (yystate >= 0 && yystate <= YYLAST && yycheck[yystate] == *yyssp)
    yystate = yytable[yystate];
  else
    yystate = yydefgoto[yyn - YYNTBASE];

  goto yynewstate;

yyerrlab:   /* here on detecting error */

  if (! yyerrstatus)
    /* If not already recovering from an error, report this error.  */
    {
      ++yynerrs;

#ifdef YYERROR_VERBOSE
      yyn = yypact[yystate];

      if (yyn > YYFLAG && yyn < YYLAST)
	{
	  int size = 0;
	  char *msg;
	  int x, count;

	  count = 0;
	  /* Start X at -yyn if nec to avoid negative indexes in yycheck.  */
	  for (x = (yyn < 0 ? -yyn : 0);
	       x < (sizeof(yytname) / sizeof(char *)); x++)
	    if (yycheck[x + yyn] == x)
	      size += strlen(yytname[x]) + 15, count++;
	  msg = (char *) malloc(size + 15);
	  if (msg != 0)
	    {
	      strcpy(msg, "parse error");

	      if (count < 5)
		{
		  count = 0;
		  for (x = (yyn < 0 ? -yyn : 0);
		       x < (sizeof(yytname) / sizeof(char *)); x++)
		    if (yycheck[x + yyn] == x)
		      {
			strcat(msg, count == 0 ? ", expecting `" : " or `");
			strcat(msg, yytname[x]);
			strcat(msg, "'");
			count++;
		      }
		}
	      yyerror(msg);
	      free(msg);
	    }
	  else
	    yyerror ("parse error; also virtual memory exceeded");
	}
      else
#endif /* YYERROR_VERBOSE */
	yyerror("parse error");
    }

  goto yyerrlab1;
yyerrlab1:   /* here on error raised explicitly by an action */

  if (yyerrstatus == 3)
    {
      /* if just tried and failed to reuse lookahead token after an error, discard it.  */

      /* return failure if at end of input */
      if (yychar == YYEOF)
	YYABORT;

#if YYDEBUG != 0
      if (yydebug)
	fprintf(stderr, "Discarding token %d (%s).\n", yychar, yytname[yychar1]);
#endif

      yychar = YYEMPTY;
    }

  /* Else will try to reuse lookahead token
     after shifting the error token.  */

  yyerrstatus = 3;		/* Each real token shifted decrements this */

  goto yyerrhandle;

yyerrdefault:  /* current state does not do anything special for the error token. */

#if 0
  /* This is wrong; only states that explicitly want error tokens
     should shift them.  */
  yyn = yydefact[yystate];  /* If its default is to accept any token, ok.  Otherwise pop it.*/
  if (yyn) goto yydefault;
#endif

yyerrpop:   /* pop the current state because it cannot handle the error token */

  if (yyssp == yyss) YYABORT;
  yyvsp--;
  yystate = *--yyssp;
#ifdef YYLSP_NEEDED
  yylsp--;
#endif

#if YYDEBUG != 0
  if (yydebug)
    {
      short *ssp1 = yyss - 1;
      fprintf (stderr, "Error: state stack now");
      while (ssp1 != yyssp)
	fprintf (stderr, " %d", *++ssp1);
      fprintf (stderr, "\n");
    }
#endif

yyerrhandle:

  yyn = yypact[yystate];
  if (yyn == YYFLAG)
    goto yyerrdefault;

  yyn += YYTERROR;
  if (yyn < 0 || yyn > YYLAST || yycheck[yyn] != YYTERROR)
    goto yyerrdefault;

  yyn = yytable[yyn];
  if (yyn < 0)
    {
      if (yyn == YYFLAG)
	goto yyerrpop;
      yyn = -yyn;
      goto yyreduce;
    }
  else if (yyn == 0)
    goto yyerrpop;

  if (yyn == YYFINAL)
    YYACCEPT;

#if YYDEBUG != 0
  if (yydebug)
    fprintf(stderr, "Shifting error token, ");
#endif

  *++yyvsp = yylval;
#ifdef YYLSP_NEEDED
  *++yylsp = yylloc;
#endif

  yystate = yyn;
  goto yynewstate;
}
#line 1082 "gram.y"


PLpgSQL_expr *
plpgsql_read_expression (int until, char *s)
{
    return read_sqlstmt(until, s, "SELECT ");
}


static PLpgSQL_expr *
read_sqlstmt (int until, char *s, char *sqlstart)
{
    int			tok;
    int			lno;
    PLpgSQL_dstring	ds;
    int			nparams = 0;
    int			params[1024];
    char		buf[32];
    PLpgSQL_expr	*expr;

    lno = yylineno;
    plpgsql_dstring_init(&ds);
    plpgsql_dstring_append(&ds, sqlstart);

    while((tok = yylex()) != until) {
	if (tok == ';') break;
	if (plpgsql_SpaceScanned) {
	    plpgsql_dstring_append(&ds, " ");
	}
        switch (tok) {
	    case T_VARIABLE:
		params[nparams] = yylval.var->varno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    case T_RECFIELD:
		params[nparams] = yylval.recfield->rfno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    case T_TGARGV:
		params[nparams] = yylval.trigarg->dno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    default:
		if (tok == 0) {
		    plpgsql_error_lineno = lno;
		    plpgsql_comperrinfo();
		    elog(ERROR, "missing %s at end of SQL statement", s);
		}
		plpgsql_dstring_append(&ds, yytext);
		break;
        }
    }

    expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * nparams - 1);
    expr->dtype		= PLPGSQL_DTYPE_EXPR;
    expr->query		= strdup(plpgsql_dstring_get(&ds));
    expr->plan		= NULL;
    expr->nparams	= nparams;
    while(nparams-- > 0) {
        expr->params[nparams] = params[nparams];
    }
    plpgsql_dstring_free(&ds);
    
    return expr;
}


static PLpgSQL_stmt *
make_select_stmt()
{
    int			tok;
    int			lno;
    PLpgSQL_dstring	ds;
    int			nparams = 0;
    int			params[1024];
    char		buf[32];
    PLpgSQL_expr	*expr;
    PLpgSQL_row		*row = NULL;
    PLpgSQL_rec		*rec = NULL;
    PLpgSQL_stmt_select	*select;
    int			have_nexttok = 0;

    lno = yylineno;
    plpgsql_dstring_init(&ds);
    plpgsql_dstring_append(&ds, "SELECT ");

    while((tok = yylex()) != K_INTO) {
	if (tok == ';') {
	    PLpgSQL_stmt_execsql	*execsql;

	    expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * nparams - 1);
	    expr->dtype		= PLPGSQL_DTYPE_EXPR;
	    expr->query		= strdup(plpgsql_dstring_get(&ds));
	    expr->plan		= NULL;
	    expr->nparams	= nparams;
	    while(nparams-- > 0) {
		expr->params[nparams] = params[nparams];
	    }
	    plpgsql_dstring_free(&ds);

	    execsql = malloc(sizeof(PLpgSQL_stmt_execsql));
	    execsql->cmd_type = PLPGSQL_STMT_EXECSQL;
	    execsql->sqlstmt  = expr;

	    return (PLpgSQL_stmt *)execsql;
	}

	if (plpgsql_SpaceScanned) {
	    plpgsql_dstring_append(&ds, " ");
	}
        switch (tok) {
	    case T_VARIABLE:
		params[nparams] = yylval.var->varno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    case T_RECFIELD:
		params[nparams] = yylval.recfield->rfno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    case T_TGARGV:
		params[nparams] = yylval.trigarg->dno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
	        
	    default:
		if (tok == 0) {
		    plpgsql_error_lineno = yylineno;
		    plpgsql_comperrinfo();
		    elog(ERROR, "unexpected end of file");
		}
		plpgsql_dstring_append(&ds, yytext);
		break;
        }
    }

    tok = yylex();
    switch (tok) {
        case T_ROW:
	    row = yylval.row;
	    break;

        case T_RECORD:
	    rec = yylval.rec;
	    break;

	case T_VARIABLE:
	case T_RECFIELD:
	    {
		PLpgSQL_var	*var;
		PLpgSQL_recfield *recfield;
		int		nfields = 1;
		char		*fieldnames[1024];
		int		varnos[1024];

		switch (tok) {
		    case T_VARIABLE:
			var = yylval.var;
			fieldnames[0] = strdup(yytext);
			varnos[0]     = var->varno;
			break;
		    
		    case T_RECFIELD:
			recfield = yylval.recfield;
			fieldnames[0] = strdup(yytext);
			varnos[0]     = recfield->rfno;
			break;
		}

		while ((tok = yylex()) == ',') {
		    tok = yylex();
		    switch(tok) {
			case T_VARIABLE:
			    var = yylval.var;
			    fieldnames[nfields] = strdup(yytext);
			    varnos[nfields++]   = var->varno;
			    break;

			case T_RECFIELD:
			    recfield = yylval.recfield;
			    fieldnames[0] = strdup(yytext);
			    varnos[0]     = recfield->rfno;
			    break;

			default:
			    elog(ERROR, "plpgsql: %s is not a variable or record field", yytext);
		    }
		}
		row = malloc(sizeof(PLpgSQL_row));
		row->dtype = PLPGSQL_DTYPE_ROW;
		row->refname = strdup("*internal*");
		row->lineno = yylineno;
		row->rowtypeclass = InvalidOid;
		row->nfields = nfields;
		row->fieldnames = malloc(sizeof(char *) * nfields);
		row->varnos = malloc(sizeof(int) * nfields);
		while (--nfields >= 0) {
		    row->fieldnames[nfields] = fieldnames[nfields];
		    row->varnos[nfields] = varnos[nfields];
		}

		plpgsql_adddatum((PLpgSQL_datum *)row);

		have_nexttok = 1;
	    }
	    break;

        default:
	    {
		if (plpgsql_SpaceScanned) {
		    plpgsql_dstring_append(&ds, " ");
		}
		plpgsql_dstring_append(&ds, yytext);

		while(1) {
		    tok = yylex();
		    if (tok == ';') {
			PLpgSQL_stmt_execsql	*execsql;

			expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * nparams - 1);
			expr->dtype		= PLPGSQL_DTYPE_EXPR;
			expr->query		= strdup(plpgsql_dstring_get(&ds));
			expr->plan		= NULL;
			expr->nparams	= nparams;
			while(nparams-- > 0) {
			    expr->params[nparams] = params[nparams];
			}
			plpgsql_dstring_free(&ds);

			execsql = malloc(sizeof(PLpgSQL_stmt_execsql));
			execsql->cmd_type = PLPGSQL_STMT_EXECSQL;
			execsql->sqlstmt  = expr;

			return (PLpgSQL_stmt *)execsql;
		    }

		    if (plpgsql_SpaceScanned) {
			plpgsql_dstring_append(&ds, " ");
		    }
		    switch (tok) {
			case T_VARIABLE:
			    params[nparams] = yylval.var->varno;
			    sprintf(buf, "$%d", ++nparams);
			    plpgsql_dstring_append(&ds, buf);
			    break;
			    
			case T_RECFIELD:
			    params[nparams] = yylval.recfield->rfno;
			    sprintf(buf, "$%d", ++nparams);
			    plpgsql_dstring_append(&ds, buf);
			    break;
			    
			case T_TGARGV:
			    params[nparams] = yylval.trigarg->dno;
			    sprintf(buf, "$%d", ++nparams);
			    plpgsql_dstring_append(&ds, buf);
			    break;
			    
			default:
			    if (tok == 0) {
				plpgsql_error_lineno = yylineno;
				plpgsql_comperrinfo();
				elog(ERROR, "unexpected end of file");
			    }
			    plpgsql_dstring_append(&ds, yytext);
			    break;
		    }
		}
	    }
    }

    /************************************************************
     * Eat up the rest of the statement after the target fields
     ************************************************************/
    while(1) {
	if (!have_nexttok) {
	    tok = yylex();
	}
	have_nexttok = 0;
	if (tok == ';') {
	    break;
	}

	if (plpgsql_SpaceScanned) {
	    plpgsql_dstring_append(&ds, " ");
	}
	switch (tok) {
	    case T_VARIABLE:
		params[nparams] = yylval.var->varno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
		
	    case T_RECFIELD:
		params[nparams] = yylval.recfield->rfno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
		
	    case T_TGARGV:
		params[nparams] = yylval.trigarg->dno;
		sprintf(buf, "$%d", ++nparams);
		plpgsql_dstring_append(&ds, buf);
		break;
		
	    default:
		if (tok == 0) {
		    plpgsql_error_lineno = yylineno;
		    plpgsql_comperrinfo();
		    elog(ERROR, "unexpected end of file");
		}
		plpgsql_dstring_append(&ds, yytext);
		break;
	}
    }

    expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * (nparams - 1));
    expr->dtype		= PLPGSQL_DTYPE_EXPR;
    expr->query		= strdup(plpgsql_dstring_get(&ds));
    expr->plan		= NULL;
    expr->nparams	= nparams;
    while(nparams-- > 0) {
        expr->params[nparams] = params[nparams];
    }
    plpgsql_dstring_free(&ds);

    select = malloc(sizeof(PLpgSQL_stmt_select));
    memset(select, 0, sizeof(PLpgSQL_stmt_select));
    select->cmd_type = PLPGSQL_STMT_SELECT;
    select->rec      = rec;
    select->row      = row;
    select->query    = expr;
    
    return (PLpgSQL_stmt *)select;
}


static PLpgSQL_expr *
make_tupret_expr(PLpgSQL_row *row)
{
    PLpgSQL_dstring	ds;
    PLpgSQL_expr	*expr;
    int			i;
    char		buf[16];

    expr = malloc(sizeof(PLpgSQL_expr) + sizeof(int) * (row->nfields - 1));
    expr->dtype		= PLPGSQL_DTYPE_EXPR;

    plpgsql_dstring_init(&ds);
    plpgsql_dstring_append(&ds, "SELECT ");

    for (i = 0; i < row->nfields; i++) {
        sprintf(buf, "%s$%d", (i > 0) ? "," : "", i + 1);
	plpgsql_dstring_append(&ds, buf);
	expr->params[i] = row->varnos[i];
    }

    expr->query         = strdup(plpgsql_dstring_get(&ds));
    expr->plan          = NULL;
    expr->plan_argtypes = NULL;
    expr->nparams       = row->nfields;

    plpgsql_dstring_free(&ds);
    return expr;
}



#include "pl_scan.c"
