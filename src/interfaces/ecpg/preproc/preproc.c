
/*  A Bison parser, made from preproc.y
 by  GNU Bison version 1.25.90
  */

#define YYBISON 1  /* Identify Bison output.  */

#define	SQL_AT	257
#define	SQL_BOOL	258
#define	SQL_BREAK	259
#define	SQL_CALL	260
#define	SQL_CONNECT	261
#define	SQL_CONNECTION	262
#define	SQL_CONTINUE	263
#define	SQL_DEALLOCATE	264
#define	SQL_DISCONNECT	265
#define	SQL_ENUM	266
#define	SQL_FOUND	267
#define	SQL_FREE	268
#define	SQL_GO	269
#define	SQL_GOTO	270
#define	SQL_IDENTIFIED	271
#define	SQL_IMMEDIATE	272
#define	SQL_INDICATOR	273
#define	SQL_INT	274
#define	SQL_LONG	275
#define	SQL_OPEN	276
#define	SQL_PREPARE	277
#define	SQL_RELEASE	278
#define	SQL_REFERENCE	279
#define	SQL_SECTION	280
#define	SQL_SEMI	281
#define	SQL_SHORT	282
#define	SQL_SIGNED	283
#define	SQL_SQLERROR	284
#define	SQL_SQLPRINT	285
#define	SQL_SQLWARNING	286
#define	SQL_START	287
#define	SQL_STOP	288
#define	SQL_STRUCT	289
#define	SQL_UNSIGNED	290
#define	SQL_VAR	291
#define	SQL_WHENEVER	292
#define	S_ANYTHING	293
#define	S_AUTO	294
#define	S_BOOL	295
#define	S_CHAR	296
#define	S_CONST	297
#define	S_DOUBLE	298
#define	S_ENUM	299
#define	S_EXTERN	300
#define	S_FLOAT	301
#define	S_INT	302
#define	S	303
#define	S_LONG	304
#define	S_REGISTER	305
#define	S_SHORT	306
#define	S_SIGNED	307
#define	S_STATIC	308
#define	S_STRUCT	309
#define	S_UNION	310
#define	S_UNSIGNED	311
#define	S_VARCHAR	312
#define	TYPECAST	313
#define	ABSOLUTE	314
#define	ACTION	315
#define	ADD	316
#define	ALL	317
#define	ALTER	318
#define	AND	319
#define	ANY	320
#define	AS	321
#define	ASC	322
#define	BEGIN_TRANS	323
#define	BETWEEN	324
#define	BOTH	325
#define	BY	326
#define	CASCADE	327
#define	CASE	328
#define	CAST	329
#define	CHAR	330
#define	CHARACTER	331
#define	CHECK	332
#define	CLOSE	333
#define	COALESCE	334
#define	COLLATE	335
#define	COLUMN	336
#define	COMMIT	337
#define	CONSTRAINT	338
#define	CREATE	339
#define	CROSS	340
#define	CURRENT	341
#define	CURRENT_DATE	342
#define	CURRENT_TIME	343
#define	CURRENT_TIMESTAMP	344
#define	CURRENT_USER	345
#define	CURSOR	346
#define	DAY_P	347
#define	DECIMAL	348
#define	DECLARE	349
#define	DEFAULT	350
#define	DELETE	351
#define	DESC	352
#define	DISTINCT	353
#define	DOUBLE	354
#define	DROP	355
#define	ELSE	356
#define	END_TRANS	357
#define	EXCEPT	358
#define	EXECUTE	359
#define	EXISTS	360
#define	EXTRACT	361
#define	FALSE_P	362
#define	FETCH	363
#define	FLOAT	364
#define	FOR	365
#define	FOREIGN	366
#define	FROM	367
#define	FULL	368
#define	GRANT	369
#define	GROUP	370
#define	HAVING	371
#define	HOUR_P	372
#define	IN	373
#define	INNER_P	374
#define	INSENSITIVE	375
#define	INSERT	376
#define	INTERSECT	377
#define	INTERVAL	378
#define	INTO	379
#define	IS	380
#define	ISOLATION	381
#define	JOIN	382
#define	KEY	383
#define	LANGUAGE	384
#define	LEADING	385
#define	LEFT	386
#define	LEVEL	387
#define	LIKE	388
#define	LOCAL	389
#define	MATCH	390
#define	MINUTE_P	391
#define	MONTH_P	392
#define	NAMES	393
#define	NATIONAL	394
#define	NATURAL	395
#define	NCHAR	396
#define	NEXT	397
#define	NO	398
#define	NOT	399
#define	NULLIF	400
#define	NULL_P	401
#define	NUMERIC	402
#define	OF	403
#define	ON	404
#define	ONLY	405
#define	OPTION	406
#define	OR	407
#define	ORDER	408
#define	OUTER_P	409
#define	PARTIAL	410
#define	POSITION	411
#define	PRECISION	412
#define	PRIMARY	413
#define	PRIOR	414
#define	PRIVILEGES	415
#define	PROCEDURE	416
#define	PUBLIC	417
#define	READ	418
#define	REFERENCES	419
#define	RELATIVE	420
#define	REVOKE	421
#define	RIGHT	422
#define	ROLLBACK	423
#define	SCROLL	424
#define	SECOND_P	425
#define	SELECT	426
#define	SET	427
#define	SUBSTRING	428
#define	TABLE	429
#define	TEMP	430
#define	THEN	431
#define	TIME	432
#define	TIMESTAMP	433
#define	TIMEZONE_HOUR	434
#define	TIMEZONE_MINUTE	435
#define	TO	436
#define	TRAILING	437
#define	TRANSACTION	438
#define	TRIM	439
#define	TRUE_P	440
#define	UNION	441
#define	UNIQUE	442
#define	UPDATE	443
#define	USER	444
#define	USING	445
#define	VALUES	446
#define	VARCHAR	447
#define	VARYING	448
#define	VIEW	449
#define	WHEN	450
#define	WHERE	451
#define	WITH	452
#define	WORK	453
#define	YEAR_P	454
#define	ZONE	455
#define	TRIGGER	456
#define	TYPE_P	457
#define	ABORT_TRANS	458
#define	AFTER	459
#define	AGGREGATE	460
#define	ANALYZE	461
#define	BACKWARD	462
#define	BEFORE	463
#define	BINARY	464
#define	CACHE	465
#define	CLUSTER	466
#define	COPY	467
#define	CREATEDB	468
#define	CREATEUSER	469
#define	CYCLE	470
#define	DATABASE	471
#define	DELIMITERS	472
#define	DO	473
#define	EACH	474
#define	ENCODING	475
#define	EXPLAIN	476
#define	EXTEND	477
#define	FORWARD	478
#define	FUNCTION	479
#define	HANDLER	480
#define	INCREMENT	481
#define	INDEX	482
#define	INHERITS	483
#define	INSTEAD	484
#define	ISNULL	485
#define	LANCOMPILER	486
#define	LIMIT	487
#define	LISTEN	488
#define	UNLISTEN	489
#define	LOAD	490
#define	LOCATION	491
#define	LOCK_P	492
#define	MAXVALUE	493
#define	MINVALUE	494
#define	MOVE	495
#define	NEW	496
#define	NOCREATEDB	497
#define	NOCREATEUSER	498
#define	NONE	499
#define	NOTHING	500
#define	NOTIFY	501
#define	NOTNULL	502
#define	OFFSET	503
#define	OIDS	504
#define	OPERATOR	505
#define	PASSWORD	506
#define	PROCEDURAL	507
#define	RECIPE	508
#define	RENAME	509
#define	RESET	510
#define	RETURNS	511
#define	ROW	512
#define	RULE	513
#define	SERIAL	514
#define	SEQUENCE	515
#define	SETOF	516
#define	SHOW	517
#define	START	518
#define	STATEMENT	519
#define	STDIN	520
#define	STDOUT	521
#define	TRUSTED	522
#define	UNTIL	523
#define	VACUUM	524
#define	VALID	525
#define	VERBOSE	526
#define	VERSION	527
#define	IDENT	528
#define	SCONST	529
#define	Op	530
#define	CSTRING	531
#define	CVARIABLE	532
#define	CPP_LINE	533
#define	ICONST	534
#define	PARAM	535
#define	FCONST	536
#define	OP	537
#define	UMINUS	538

#line 2 "preproc.y"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "catalog/catname.h"
#include "utils/numeric.h"

#include "extern.h"

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

#define STRUCT_DEPTH 128

/*
 * Variables containing simple states.
 */
int	struct_level = 0;
char	errortext[128];
static char	*connection = NULL;
static int      QueryIsRule = 0, ForUpdateNotAllowed = 0;
static struct this_type actual_type[STRUCT_DEPTH];
static char     *actual_storage[STRUCT_DEPTH];

/* temporarily store struct members while creating the data structure */
struct ECPGstruct_member *struct_member_list[STRUCT_DEPTH] = { NULL };

struct ECPGtype ecpg_no_indicator = {ECPGt_NO_INDICATOR, 0L, {NULL}};
struct variable no_indicator = {"no_indicator", &ecpg_no_indicator, 0, NULL};

struct ECPGtype ecpg_query = {ECPGt_char_variable, 0L, {NULL}};

/*
 * Handle the filename and line numbering.
 */
char * input_filename = NULL;

static void
output_line_number()
{
    if (input_filename)
       fprintf(yyout, "\n#line %d \"%s\"\n", yylineno, input_filename);
}

/*
 * store the whenever action here
 */
static struct when when_error, when_nf, when_warn;

static void
print_action(struct when *w)
{
	switch (w->code)
	{
		case W_SQLPRINT: fprintf(yyout, "sqlprint();");
                                 break;
		case W_GOTO:	 fprintf(yyout, "goto %s;", w->command);
				 break;
		case W_DO:	 fprintf(yyout, "%s;", w->command);
				 break;
		case W_STOP:	 fprintf(yyout, "exit (1);");
				 break;
		case W_BREAK:	 fprintf(yyout, "break;");
				 break;
		default:	 fprintf(yyout, "{/* %d not implemented yet */}", w->code);
				 break;
	}
}

static void
whenever_action(int mode)
{
	if (mode == 1 && when_nf.code != W_NOTHING)
	{
		output_line_number();
		fprintf(yyout, "\nif (sqlca.sqlcode == ECPG_NOT_FOUND) ");
		print_action(&when_nf);
	}
	if (when_warn.code != W_NOTHING)
        {
		output_line_number();
                fprintf(yyout, "\nif (sqlca.sqlwarn[0] == 'W') ");
		print_action(&when_warn);
        }
	if (when_error.code != W_NOTHING)
        {
		output_line_number();
                fprintf(yyout, "\nif (sqlca.sqlcode < 0) ");
		print_action(&when_error);
        }
	output_line_number();
}

/*
 * Handling of variables.
 */

/*
 * brace level counter
 */
int braces_open;

static struct variable * allvariables = NULL;

static struct variable *
new_variable(const char * name, struct ECPGtype * type)
{
    struct variable * p = (struct variable*) mm_alloc(sizeof(struct variable));

    p->name = mm_strdup(name);
    p->type = type;
    p->brace_level = braces_open;

    p->next = allvariables;
    allvariables = p;

    return(p);
}

static struct variable * find_variable(char * name);

static struct variable *
find_struct_member(char *name, char *str, struct ECPGstruct_member *members)
{
    char *next = strchr(++str, '.'), c = '\0';

    if (next != NULL)
    {
	c = *next;
	*next = '\0';
    }

    for (; members; members = members->next)
    {
        if (strcmp(members->name, str) == 0)
	{
		if (c == '\0')
		{
			/* found the end */
			switch (members->typ->typ)
			{
			   case ECPGt_array:
				return(new_variable(name, ECPGmake_array_type(members->typ->u.element, members->typ->size)));
			   case ECPGt_struct:
			   case ECPGt_union:
				return(new_variable(name, ECPGmake_struct_type(members->typ->u.members, members->typ->typ)));
			   default:
				return(new_variable(name, ECPGmake_simple_type(members->typ->typ, members->typ->size)));
			}
		}
		else
		{
			*next = c;
			if (c == '-')
			{
				next++;
				return(find_struct_member(name, next, members->typ->u.element->u.members));
			}
			else return(find_struct_member(name, next, members->typ->u.members));
		}
	}
    }

    return(NULL);
}

static struct variable *
find_struct(char * name, char *next)
{
    struct variable * p;
    char c = *next;

    /* first get the mother structure entry */
    *next = '\0';
    p = find_variable(name);

    /* restore the name, we will need it later on */
    *next = c;
    if (c == '-')
    {
	next++;
	return find_struct_member(name, next, p->type->u.element->u.members);
    }
    else return find_struct_member(name, next, p->type->u.members);
}

static struct variable *
find_simple(char * name)
{
    struct variable * p;

    for (p = allvariables; p; p = p->next)
    {
        if (strcmp(p->name, name) == 0)
	    return p;
    }

    return(NULL);
}

/* Note that this function will end the program in case of an unknown */
/* variable */
static struct variable *
find_variable(char * name)
{
    char * next;
    struct variable * p;

    if ((next = strchr(name, '.')) != NULL)
	p = find_struct(name, next);
    else if ((next = strstr(name, "->")) != NULL)
	p = find_struct(name, next);
    else
	p = find_simple(name);

    if (p == NULL)
    {
	sprintf(errortext, "The variable %s is not declared", name);
	yyerror(errortext);
    }

    return(p);
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

struct arguments * argsinsert = NULL;
struct arguments * argsresult = NULL;

static void
reset_variables(void)
{
    argsinsert = NULL;
    argsresult = NULL;
}


/* Add a variable to a request. */
static void
add_variable(struct arguments ** list, struct variable * var, struct variable * ind)
{
    struct arguments * p = (struct arguments *)mm_alloc(sizeof(struct arguments));
    p->variable = var;
    p->indicator = ind;
    p->next = *list;
    *list = p;
}


/* Dump out a list of all the variable on this list.
   This is a recursive function that works from the end of the list and
   deletes the list as we go on.
 */
static void
dump_variables(struct arguments * list, int mode)
{
    if (list == NULL)
    {
        return;
    }

    /* The list is build up from the beginning so lets first dump the
       end of the list:
     */

    dump_variables(list->next, mode);

    /* Then the current element and its indicator */
    ECPGdump_a_type(yyout, list->variable->name, list->variable->type,
	(list->indicator->type->typ != ECPGt_NO_INDICATOR) ? list->indicator->name : NULL,
	(list->indicator->type->typ != ECPGt_NO_INDICATOR) ? list->indicator->type : NULL, NULL, NULL);

    /* Then release the list element. */
    if (mode != 0)
    	free(list);
}

static void
check_indicator(struct ECPGtype *var)
{
	/* make sure this is a valid indicator variable */
	switch (var->typ)
	{
		struct ECPGstruct_member *p;

		case ECPGt_short:
		case ECPGt_int:
		case ECPGt_long:
		case ECPGt_unsigned_short:
		case ECPGt_unsigned_int:
		case ECPGt_unsigned_long:
			break;

		case ECPGt_struct:
		case ECPGt_union:
			for (p = var->u.members; p; p = p->next)
				check_indicator(p->typ);
			break;

		case ECPGt_array:
			check_indicator(var->u.element);
			break;
		default: 
			yyerror ("indicator variable must be integer type");
			break;
	}
}

static char *
make1_str(const char *str)
{
        char * res_str = (char *)mm_alloc(strlen(str) + 1);

	strcpy(res_str, str);
	return res_str;
}

static char *
make2_str(char *str1, char *str2)
{ 
	char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + 1);

	strcpy(res_str, str1);
	strcat(res_str, str2);
	free(str1);
	free(str2);
	return(res_str);
}

static char *
cat2_str(char *str1, char *str2)
{ 
	char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + 2);

	strcpy(res_str, str1);
	strcat(res_str, " ");
	strcat(res_str, str2);
	free(str1);
	free(str2);
	return(res_str);
}

static char *
make3_str(char *str1, char *str2, char * str3)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
	free(str1);
	free(str2);
	free(str3);
        return(res_str);
}    

static char *
cat3_str(char *str1, char *str2, char * str3)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + 3);
     
        strcpy(res_str, str1);
	strcat(res_str, " ");
        strcat(res_str, str2);
	strcat(res_str, " ");
	strcat(res_str, str3);
	free(str1);
	free(str2);
	free(str3);
        return(res_str);
}    

static char *
make4_str(char *str1, char *str2, char *str3, char *str4)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
	strcat(res_str, str4);
	free(str1);
	free(str2);
	free(str3);
	free(str4);
        return(res_str);
}

static char *
cat4_str(char *str1, char *str2, char *str3, char *str4)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + 4);
     
        strcpy(res_str, str1);
	strcat(res_str, " ");
        strcat(res_str, str2);
	strcat(res_str, " ");
	strcat(res_str, str3);
	strcat(res_str, " ");
	strcat(res_str, str4);
	free(str1);
	free(str2);
	free(str3);
	free(str4);
        return(res_str);
}

static char *
make5_str(char *str1, char *str2, char *str3, char *str4, char *str5)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + strlen(str5) + 1);
     
        strcpy(res_str, str1);
        strcat(res_str, str2);
	strcat(res_str, str3);
	strcat(res_str, str4);
	strcat(res_str, str5);
	free(str1);
	free(str2);
	free(str3);
	free(str4);
	free(str5);
        return(res_str);
}    

static char *
cat5_str(char *str1, char *str2, char *str3, char *str4, char *str5)
{    
        char * res_str  = (char *)mm_alloc(strlen(str1) + strlen(str2) + strlen(str3) + strlen(str4) + strlen(str5) + 5);
     
        strcpy(res_str, str1);
	strcat(res_str, " ");
        strcat(res_str, str2);
	strcat(res_str, " ");
	strcat(res_str, str3);
	strcat(res_str, " ");
	strcat(res_str, str4);
	strcat(res_str, " ");
	strcat(res_str, str5);
	free(str1);
	free(str2);
	free(str3);
	free(str4);
	free(str5);
        return(res_str);
}    

static char *
make_name(void)
{
	char * name = (char *)mm_alloc(yyleng + 1);

	strncpy(name, yytext, yyleng);
	name[yyleng] = '\0';
	return(name);
}

static void
output_statement(char * stmt, int mode)
{
	int i, j=strlen(stmt);

	fprintf(yyout, "ECPGdo(__LINE__, %s, \"", connection ? connection : "NULL");

	/* do this char by char as we have to filter '\"' */
	for (i = 0;i < j; i++)
		if (stmt[i] != '\"')
			fputc(stmt[i], yyout);
	fputs("\", ", yyout);

	/* dump variables to C file*/
	dump_variables(argsinsert, 1);
	fputs("ECPGt_EOIT, ", yyout);
	dump_variables(argsresult, 1);
	fputs("ECPGt_EORT);", yyout);
	whenever_action(mode);
	free(stmt);
	if (connection != NULL)
		free(connection);
}

static struct typedefs *
get_typedef(char *name)
{
	struct typedefs *this;

	for (this = types; this && strcmp(this->name, name); this = this->next);
	if (!this)
	{
		sprintf(errortext, "invalid datatype '%s'", name);
		yyerror(errortext);
	}

	return(this);
}

static void
adjust_array(enum ECPGttype type_enum, int *dimension, int *length, int type_dimension, int type_index, bool pointer)
{
	if (type_index >= 0) 
	{
		if (*length >= 0)
                      	yyerror("No multi-dimensional array support");

		*length = type_index;
	}
		       
	if (type_dimension >= 0)
	{
		if (*dimension >= 0 && *length >= 0)
			yyerror("No multi-dimensional array support");

		if (*dimension >= 0)
			*length = *dimension;

		*dimension = type_dimension;
	}

	switch (type_enum)
	{
	   case ECPGt_struct:
	   case ECPGt_union:
	        /* pointer has to get dimension 0 */
                if (pointer)
	        {
		    *length = *dimension;
                    *dimension = 0;
	        }

                if (*length >= 0)
                   yyerror("No multi-dimensional array support for structures");

                break;
           case ECPGt_varchar:
	        /* pointer has to get length 0 */
                if (pointer)
                    *length=0;

                /* one index is the string length */
                if (*length < 0)
                {
                   *length = *dimension;
                   *dimension = -1;
                }

                break;
           case ECPGt_char:
           case ECPGt_unsigned_char:
	        /* pointer has to get length 0 */
                if (pointer)
                    *length=0;

                /* one index is the string length */
                if (*length < 0)
                {
                   *length = (*dimension < 0) ? 1 : *dimension;
                   *dimension = -1;
                }

                break;
           default:
 	        /* a pointer has dimension = 0 */
                if (pointer) {
                    *length = *dimension;
		    *dimension = 0;
	        }

                if (*length >= 0)
                   yyerror("No multi-dimensional array support for simple data types");

                break;
	}
}


#line 609 "preproc.y"
typedef union {
	double                  dval;
        int                     ival;
	char *                  str;
	struct when             action;
	struct index		index;
	int			tagname;
	struct this_type	type;
	enum ECPGttype		type_enum;
} YYSTYPE;
#include <stdio.h>

#ifndef __cplusplus
#ifndef __STDC__
#define const
#endif
#endif



#define	YYFINAL		2398
#define	YYFLAG		-32768
#define	YYNTBASE	303

#define YYTRANSLATE(x) ((unsigned)(x) <= 538 ? yytranslate[x] : 662)

static const short yytranslate[] = {     0,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,   299,
   300,   289,   287,   298,   288,   295,   290,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,   292,   293,   285,
   284,   286,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
   296,     2,   297,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,   301,   291,   302,     2,     2,     2,     2,     2,
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
     2,     2,     2,     2,     2,     1,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    12,    13,    14,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    25,    26,
    27,    28,    29,    30,    31,    32,    33,    34,    35,    36,
    37,    38,    39,    40,    41,    42,    43,    44,    45,    46,
    47,    48,    49,    50,    51,    52,    53,    54,    55,    56,
    57,    58,    59,    60,    61,    62,    63,    64,    65,    66,
    67,    68,    69,    70,    71,    72,    73,    74,    75,    76,
    77,    78,    79,    80,    81,    82,    83,    84,    85,    86,
    87,    88,    89,    90,    91,    92,    93,    94,    95,    96,
    97,    98,    99,   100,   101,   102,   103,   104,   105,   106,
   107,   108,   109,   110,   111,   112,   113,   114,   115,   116,
   117,   118,   119,   120,   121,   122,   123,   124,   125,   126,
   127,   128,   129,   130,   131,   132,   133,   134,   135,   136,
   137,   138,   139,   140,   141,   142,   143,   144,   145,   146,
   147,   148,   149,   150,   151,   152,   153,   154,   155,   156,
   157,   158,   159,   160,   161,   162,   163,   164,   165,   166,
   167,   168,   169,   170,   171,   172,   173,   174,   175,   176,
   177,   178,   179,   180,   181,   182,   183,   184,   185,   186,
   187,   188,   189,   190,   191,   192,   193,   194,   195,   196,
   197,   198,   199,   200,   201,   202,   203,   204,   205,   206,
   207,   208,   209,   210,   211,   212,   213,   214,   215,   216,
   217,   218,   219,   220,   221,   222,   223,   224,   225,   226,
   227,   228,   229,   230,   231,   232,   233,   234,   235,   236,
   237,   238,   239,   240,   241,   242,   243,   244,   245,   246,
   247,   248,   249,   250,   251,   252,   253,   254,   255,   256,
   257,   258,   259,   260,   261,   262,   263,   264,   265,   266,
   267,   268,   269,   270,   271,   272,   273,   274,   275,   276,
   277,   278,   279,   280,   281,   282,   283,   294
};

#if YYDEBUG != 0
static const short yyprhs[] = {     0,
     0,     2,     3,     6,    11,    15,    17,    19,    21,    23,
    25,    28,    30,    32,    34,    36,    38,    40,    42,    44,
    46,    48,    50,    52,    54,    56,    58,    60,    62,    64,
    66,    68,    70,    72,    74,    76,    78,    80,    82,    84,
    86,    88,    90,    92,    94,    96,    98,   100,   102,   104,
   106,   108,   110,   112,   114,   116,   118,   120,   122,   124,
   126,   128,   130,   132,   134,   136,   138,   140,   142,   151,
   160,   164,   168,   169,   171,   173,   174,   176,   178,   179,
   183,   185,   189,   190,   194,   195,   200,   205,   210,   217,
   223,   227,   229,   231,   233,   235,   237,   240,   244,   249,
   252,   256,   261,   267,   271,   276,   280,   287,   293,   296,
   299,   307,   309,   311,   313,   315,   317,   319,   320,   323,
   324,   328,   329,   338,   340,   341,   345,   347,   348,   350,
   352,   356,   360,   362,   363,   366,   368,   371,   372,   376,
   378,   383,   386,   389,   392,   394,   397,   403,   407,   409,
   411,   414,   418,   422,   426,   430,   434,   438,   442,   445,
   448,   452,   459,   463,   467,   472,   476,   479,   482,   484,
   486,   491,   493,   498,   500,   502,   506,   508,   513,   518,
   524,   535,   539,   541,   543,   545,   547,   550,   554,   558,
   562,   566,   570,   574,   578,   581,   584,   588,   595,   599,
   603,   608,   612,   616,   621,   625,   629,   632,   635,   638,
   641,   645,   648,   653,   657,   661,   666,   671,   677,   684,
   690,   697,   701,   703,   705,   708,   711,   712,   715,   717,
   718,   722,   726,   729,   731,   734,   737,   742,   743,   751,
   755,   756,   760,   762,   764,   769,   772,   773,   776,   778,
   781,   784,   787,   790,   792,   794,   796,   799,   801,   804,
   814,   816,   817,   822,   837,   839,   841,   843,   847,   853,
   855,   857,   859,   863,   865,   866,   868,   870,   872,   876,
   877,   879,   881,   883,   885,   891,   895,   898,   900,   902,
   904,   906,   908,   910,   912,   914,   918,   920,   924,   928,
   930,   934,   936,   938,   940,   942,   945,   949,   953,   960,
   965,   967,   969,   971,   973,   974,   976,   979,   981,   983,
   985,   986,   989,   992,   993,  1001,  1004,  1006,  1008,  1010,
  1014,  1016,  1018,  1020,  1022,  1024,  1026,  1029,  1031,  1035,
  1036,  1043,  1055,  1057,  1058,  1061,  1062,  1064,  1066,  1070,
  1072,  1079,  1083,  1086,  1089,  1090,  1092,  1095,  1096,  1101,
  1105,  1117,  1120,  1121,  1125,  1128,  1130,  1134,  1137,  1139,
  1140,  1144,  1146,  1148,  1150,  1152,  1157,  1159,  1161,  1166,
  1173,  1175,  1177,  1179,  1181,  1183,  1185,  1187,  1189,  1191,
  1193,  1197,  1201,  1205,  1215,  1217,  1218,  1220,  1221,  1222,
  1236,  1238,  1240,  1242,  1246,  1250,  1252,  1254,  1257,  1261,
  1264,  1266,  1268,  1270,  1272,  1276,  1278,  1280,  1282,  1284,
  1286,  1288,  1289,  1292,  1295,  1298,  1301,  1304,  1307,  1310,
  1313,  1316,  1318,  1320,  1321,  1327,  1330,  1337,  1341,  1345,
  1346,  1350,  1351,  1353,  1355,  1356,  1358,  1360,  1361,  1365,
  1370,  1374,  1380,  1382,  1383,  1385,  1386,  1390,  1391,  1393,
  1397,  1401,  1403,  1405,  1407,  1409,  1411,  1413,  1418,  1423,
  1426,  1428,  1436,  1441,  1445,  1446,  1450,  1452,  1455,  1460,
  1464,  1473,  1481,  1488,  1490,  1491,  1498,  1506,  1508,  1510,
  1512,  1515,  1516,  1519,  1520,  1523,  1526,  1529,  1534,  1538,
  1540,  1544,  1549,  1554,  1563,  1568,  1571,  1572,  1574,  1575,
  1577,  1578,  1580,  1584,  1586,  1587,  1591,  1592,  1594,  1598,
  1601,  1604,  1607,  1610,  1612,  1614,  1615,  1620,  1625,  1628,
  1633,  1636,  1637,  1639,  1641,  1643,  1645,  1647,  1649,  1650,
  1652,  1654,  1658,  1662,  1663,  1666,  1667,  1670,  1675,  1676,
  1685,  1688,  1689,  1693,  1698,  1700,  1704,  1707,  1709,  1712,
  1715,  1718,  1721,  1723,  1725,  1727,  1728,  1730,  1731,  1736,
  1741,  1742,  1744,  1748,  1750,  1754,  1756,  1759,  1760,  1762,
  1765,  1769,  1774,  1775,  1779,  1784,  1785,  1788,  1790,  1793,
  1795,  1797,  1799,  1801,  1803,  1805,  1807,  1809,  1811,  1813,
  1815,  1817,  1819,  1821,  1823,  1825,  1827,  1829,  1831,  1833,
  1835,  1837,  1839,  1841,  1843,  1845,  1847,  1849,  1851,  1853,
  1855,  1857,  1859,  1861,  1863,  1865,  1867,  1870,  1873,  1876,
  1879,  1881,  1884,  1886,  1888,  1892,  1893,  1899,  1903,  1904,
  1910,  1914,  1915,  1920,  1922,  1927,  1930,  1932,  1936,  1939,
  1941,  1942,  1946,  1947,  1950,  1951,  1953,  1956,  1958,  1961,
  1963,  1965,  1967,  1969,  1971,  1973,  1977,  1978,  1980,  1984,
  1988,  1992,  1996,  2000,  2004,  2008,  2009,  2011,  2013,  2021,
  2030,  2039,  2047,  2055,  2059,  2061,  2063,  2065,  2067,  2069,
  2071,  2073,  2075,  2077,  2079,  2083,  2085,  2088,  2090,  2092,
  2094,  2097,  2101,  2105,  2109,  2113,  2117,  2121,  2125,  2128,
  2131,  2135,  2142,  2146,  2150,  2154,  2159,  2162,  2165,  2170,
  2174,  2179,  2181,  2183,  2188,  2190,  2195,  2197,  2199,  2204,
  2209,  2214,  2219,  2225,  2231,  2237,  2242,  2245,  2249,  2252,
  2257,  2261,  2266,  2270,  2275,  2281,  2288,  2294,  2301,  2307,
  2313,  2319,  2325,  2331,  2337,  2343,  2349,  2356,  2363,  2370,
  2377,  2384,  2391,  2398,  2405,  2412,  2419,  2426,  2433,  2440,
  2447,  2454,  2461,  2465,  2469,  2472,  2474,  2476,  2479,  2481,
  2483,  2486,  2490,  2494,  2498,  2502,  2505,  2508,  2512,  2519,
  2523,  2527,  2530,  2533,  2537,  2542,  2544,  2546,  2551,  2553,
  2558,  2560,  2562,  2567,  2572,  2578,  2584,  2590,  2595,  2597,
  2602,  2609,  2610,  2612,  2616,  2620,  2624,  2625,  2627,  2629,
  2631,  2633,  2637,  2638,  2641,  2643,  2646,  2650,  2654,  2658,
  2662,  2665,  2669,  2676,  2680,  2684,  2687,  2690,  2692,  2696,
  2701,  2706,  2711,  2717,  2723,  2729,  2734,  2738,  2739,  2742,
  2743,  2746,  2747,  2751,  2754,  2756,  2758,  2760,  2762,  2766,
  2768,  2770,  2772,  2776,  2782,  2789,  2794,  2797,  2799,  2804,
  2807,  2808,  2811,  2813,  2814,  2818,  2822,  2824,  2828,  2832,
  2836,  2838,  2840,  2845,  2848,  2852,  2856,  2858,  2862,  2864,
  2868,  2870,  2872,  2873,  2875,  2877,  2879,  2881,  2883,  2885,
  2887,  2889,  2891,  2893,  2895,  2897,  2899,  2901,  2904,  2906,
  2908,  2910,  2913,  2915,  2917,  2919,  2921,  2923,  2925,  2927,
  2929,  2931,  2933,  2935,  2937,  2939,  2941,  2943,  2945,  2947,
  2949,  2951,  2953,  2955,  2957,  2959,  2961,  2963,  2965,  2967,
  2969,  2971,  2973,  2975,  2977,  2979,  2981,  2983,  2985,  2987,
  2989,  2991,  2993,  2995,  2997,  2999,  3001,  3003,  3005,  3007,
  3009,  3011,  3013,  3015,  3017,  3019,  3021,  3023,  3025,  3027,
  3029,  3031,  3033,  3035,  3037,  3039,  3041,  3043,  3045,  3047,
  3049,  3051,  3053,  3055,  3057,  3059,  3061,  3063,  3065,  3067,
  3069,  3071,  3073,  3075,  3077,  3079,  3081,  3083,  3085,  3087,
  3089,  3091,  3093,  3095,  3097,  3099,  3101,  3103,  3105,  3107,
  3109,  3111,  3113,  3115,  3117,  3119,  3121,  3123,  3125,  3127,
  3129,  3131,  3133,  3135,  3137,  3139,  3141,  3143,  3145,  3147,
  3149,  3151,  3153,  3155,  3157,  3159,  3161,  3163,  3165,  3167,
  3169,  3171,  3173,  3175,  3177,  3179,  3181,  3183,  3185,  3187,
  3189,  3191,  3193,  3195,  3197,  3199,  3201,  3203,  3205,  3207,
  3209,  3211,  3213,  3219,  3223,  3226,  3230,  3237,  3239,  3241,
  3244,  3247,  3249,  3250,  3252,  3256,  3259,  3260,  3263,  3264,
  3267,  3268,  3270,  3274,  3279,  3283,  3285,  3287,  3289,  3291,
  3294,  3295,  3303,  3307,  3308,  3313,  3319,  3325,  3326,  3329,
  3330,  3331,  3338,  3340,  3342,  3344,  3346,  3348,  3350,  3351,
  3353,  3355,  3357,  3359,  3361,  3363,  3368,  3371,  3376,  3381,
  3384,  3387,  3388,  3390,  3392,  3395,  3397,  3400,  3402,  3405,
  3407,  3409,  3411,  3413,  3416,  3418,  3420,  3424,  3429,  3430,
  3433,  3434,  3436,  3440,  3443,  3445,  3447,  3449,  3450,  3452,
  3454,  3458,  3459,  3464,  3466,  3468,  3471,  3475,  3476,  3479,
  3481,  3485,  3490,  3493,  3497,  3504,  3508,  3512,  3517,  3522,
  3523,  3527,  3531,  3536,  3541,  3542,  3544,  3545,  3547,  3549,
  3551,  3553,  3556,  3558,  3561,  3564,  3566,  3569,  3572,  3575,
  3576,  3582,  3583,  3589,  3591,  3593,  3594,  3595,  3598,  3599,
  3604,  3606,  3610,  3614,  3621,  3625,  3630,  3634,  3636,  3638,
  3640,  3643,  3647,  3653,  3656,  3662,  3665,  3667,  3669,  3671,
  3674,  3678,  3682,  3686,  3690,  3694,  3698,  3702,  3705,  3708,
  3712,  3719,  3723,  3727,  3731,  3736,  3739,  3742,  3747,  3751,
  3756,  3758,  3760,  3765,  3767,  3772,  3774,  3779,  3784,  3789,
  3794,  3800,  3806,  3812,  3817,  3820,  3824,  3827,  3832,  3836,
  3841,  3845,  3850,  3856,  3863,  3869,  3876,  3882,  3888,  3894,
  3900,  3906,  3912,  3918,  3924,  3931,  3938,  3945,  3952,  3959,
  3966,  3973,  3980,  3987,  3994,  4001,  4008,  4015,  4022,  4029,
  4036,  4040,  4044,  4047,  4049,  4051,  4055,  4057,  4058,  4061,
  4063,  4066,  4069,  4072,  4074,  4076,  4077,  4079,  4082,  4085,
  4087,  4089,  4091,  4093,  4095,  4098,  4100,  4102,  4104,  4106,
  4108,  4110,  4112,  4114,  4116,  4118,  4120,  4122,  4124,  4126,
  4128,  4130,  4132,  4134,  4136,  4138,  4140,  4142,  4144,  4146,
  4148,  4150,  4152,  4154,  4156,  4158,  4160,  4162,  4164,  4166,
  4168,  4170,  4172,  4174,  4176,  4178,  4180,  4184,  4186
};

static const short yyrhs[] = {   304,
     0,     0,   304,   305,     0,   644,   306,   307,    27,     0,
   644,   307,    27,     0,   589,     0,   656,     0,   654,     0,
   660,     0,   661,     0,     3,   575,     0,   322,     0,   309,
     0,   324,     0,   325,     0,   331,     0,   354,     0,   358,
     0,   364,     0,   367,     0,   308,     0,   448,     0,   377,
     0,   385,     0,   366,     0,   376,     0,   310,     0,   406,
     0,   454,     0,   386,     0,   390,     0,   397,     0,   436,
     0,   437,     0,   462,     0,   408,     0,   407,     0,   416,
     0,   419,     0,   418,     0,   414,     0,   423,     0,   396,
     0,   455,     0,   426,     0,   438,     0,   440,     0,   441,
     0,   442,     0,   447,     0,   449,     0,   317,     0,   320,
     0,   321,     0,   574,     0,   587,     0,   588,     0,   612,
     0,   613,     0,   616,     0,   619,     0,   620,     0,   623,
     0,   624,     0,   625,     0,   626,     0,   639,     0,   640,
     0,    85,   190,   569,   311,   312,   313,   315,   316,     0,
    64,   190,   569,   311,   312,   313,   315,   316,     0,   101,
   190,   569,     0,   198,   252,   569,     0,     0,   214,     0,
   243,     0,     0,   215,     0,   244,     0,     0,   314,   298,
   569,     0,   569,     0,   119,   116,   314,     0,     0,   271,
   269,   568,     0,     0,   173,   571,   182,   318,     0,   173,
   571,   284,   318,     0,   173,   178,   201,   319,     0,   173,
   184,   127,   133,   164,   571,     0,   173,   184,   127,   133,
   571,     0,   173,   139,   446,     0,   568,     0,    96,     0,
   568,     0,    96,     0,   135,     0,   263,   571,     0,   263,
   178,   201,     0,   263,   184,   127,   133,     0,   256,   571,
     0,   256,   178,   201,     0,   256,   184,   127,   133,     0,
    64,   175,   554,   484,   323,     0,    62,   425,   335,     0,
    62,   299,   333,   300,     0,   101,   425,   571,     0,    64,
   425,   571,   173,    96,   342,     0,    64,   425,   571,   101,
    96,     0,    62,   344,     0,    79,   553,     0,   213,   328,
   554,   329,   326,   327,   330,     0,   182,     0,   113,     0,
   568,     0,   266,     0,   267,     0,   210,     0,     0,   198,
   250,     0,     0,   191,   218,   568,     0,     0,    85,   332,
   175,   554,   299,   333,   300,   353,     0,   176,     0,     0,
   333,   298,   334,     0,   334,     0,     0,   335,     0,   343,
     0,   571,   502,   336,     0,   571,   260,   338,     0,   337,
     0,     0,   337,   339,     0,   339,     0,   159,   129,     0,
     0,    84,   560,   340,     0,   340,     0,    78,   299,   346,
   300,     0,    96,   147,     0,    96,   342,     0,   145,   147,
     0,   188,     0,   159,   129,     0,   165,   571,   458,   349,
   350,     0,   341,   298,   342,     0,   342,     0,   564,     0,
   288,   342,     0,   342,   287,   342,     0,   342,   288,   342,
     0,   342,   290,   342,     0,   342,   289,   342,     0,   342,
   284,   342,     0,   342,   285,   342,     0,   342,   286,   342,
     0,   293,   342,     0,   291,   342,     0,   342,    59,   502,
     0,    75,   299,   342,    67,   502,   300,     0,   299,   342,
   300,     0,   561,   299,   300,     0,   561,   299,   341,   300,
     0,   342,   276,   342,     0,   276,   342,     0,   342,   276,
     0,    88,     0,    89,     0,    89,   299,   566,   300,     0,
    90,     0,    90,   299,   566,   300,     0,    91,     0,   190,
     0,    84,   560,   344,     0,   344,     0,    78,   299,   346,
   300,     0,   188,   299,   459,   300,     0,   159,   129,   299,
   459,   300,     0,   112,   129,   299,   459,   300,   165,   571,
   458,   349,   350,     0,   345,   298,   346,     0,   346,     0,
   564,     0,   147,     0,   571,     0,   288,   346,     0,   346,
   287,   346,     0,   346,   288,   346,     0,   346,   290,   346,
     0,   346,   289,   346,     0,   346,   284,   346,     0,   346,
   285,   346,     0,   346,   286,   346,     0,   293,   346,     0,
   291,   346,     0,   346,    59,   502,     0,    75,   299,   346,
    67,   502,   300,     0,   299,   346,   300,     0,   561,   299,
   300,     0,   561,   299,   345,   300,     0,   346,   276,   346,
     0,   346,   134,   346,     0,   346,   145,   134,   346,     0,
   346,    65,   346,     0,   346,   153,   346,     0,   145,   346,
     0,   276,   346,     0,   346,   276,     0,   346,   231,     0,
   346,   126,   147,     0,   346,   248,     0,   346,   126,   145,
   147,     0,   346,   126,   186,     0,   346,   126,   108,     0,
   346,   126,   145,   186,     0,   346,   126,   145,   108,     0,
   346,   119,   299,   347,   300,     0,   346,   145,   119,   299,
   347,   300,     0,   346,    70,   348,    65,   348,     0,   346,
   145,    70,   348,    65,   348,     0,   347,   298,   348,     0,
   348,     0,   564,     0,   136,   114,     0,   136,   156,     0,
     0,   351,   351,     0,   351,     0,     0,   150,    97,   352,
     0,   150,   189,   352,     0,   144,    61,     0,    73,     0,
   173,    96,     0,   173,   147,     0,   229,   299,   485,   300,
     0,     0,    85,   332,   175,   554,   355,    67,   472,     0,
   299,   356,   300,     0,     0,   356,   298,   357,     0,   357,
     0,   571,     0,    85,   261,   554,   359,     0,   359,   360,
     0,     0,   211,   363,     0,   216,     0,   227,   363,     0,
   239,   363,     0,   240,   363,     0,   264,   363,     0,   362,
     0,   363,     0,   567,     0,   288,   567,     0,   566,     0,
   288,   566,     0,    85,   365,   253,   130,   568,   226,   380,
   232,   568,     0,   268,     0,     0,   101,   253,   130,   568,
     0,    85,   202,   560,   368,   369,   150,   554,   371,   105,
   162,   560,   299,   374,   300,     0,   209,     0,   205,     0,
   370,     0,   370,   153,   370,     0,   370,   153,   370,   153,
   370,     0,   122,     0,    97,     0,   189,     0,   111,   372,
   373,     0,   220,     0,     0,   258,     0,   265,     0,   375,
     0,   374,   298,   375,     0,     0,   566,     0,   567,     0,
   568,     0,   652,     0,   101,   202,   560,   150,   554,     0,
    85,   379,   378,     0,   380,   381,     0,   251,     0,   203,
     0,   206,     0,   162,     0,   128,     0,   571,     0,   421,
     0,   276,     0,   299,   382,   300,     0,   383,     0,   382,
   298,   383,     0,   380,   284,   384,     0,   380,     0,    96,
   284,   384,     0,   571,     0,   420,     0,   361,     0,   568,
     0,   262,   571,     0,   101,   175,   485,     0,   101,   261,
   485,     0,   109,   387,   388,   389,   125,   643,     0,   241,
   387,   388,   389,     0,   224,     0,   208,     0,   166,     0,
    60,     0,     0,   566,     0,   288,   566,     0,    63,     0,
   143,     0,   160,     0,     0,   119,   560,     0,   113,   560,
     0,     0,   115,   391,   150,   485,   182,   394,   395,     0,
    63,   161,     0,    63,     0,   392,     0,   393,     0,   392,
   298,   393,     0,   172,     0,   122,     0,   189,     0,    97,
     0,   259,     0,   163,     0,   116,   571,     0,   571,     0,
   198,   115,   152,     0,     0,   167,   391,   150,   485,   113,
   394,     0,    85,   398,   228,   559,   150,   554,   399,   299,
   400,   300,   409,     0,   188,     0,     0,   191,   556,     0,
     0,   401,     0,   402,     0,   401,   298,   403,     0,   403,
     0,   561,   299,   486,   300,   404,   405,     0,   557,   404,
   405,     0,   292,   502,     0,   111,   502,     0,     0,   558,
     0,   191,   558,     0,     0,   223,   228,   559,   498,     0,
   105,   254,   563,     0,    85,   225,   561,   410,   257,   412,
   409,    67,   568,   130,   568,     0,   198,   381,     0,     0,
   299,   411,   300,     0,   299,   300,     0,   570,     0,   411,
   298,   570,     0,   413,   570,     0,   262,     0,     0,   101,
   415,   560,     0,   203,     0,   228,     0,   259,     0,   195,
     0,   101,   206,   560,   417,     0,   560,     0,   289,     0,
   101,   225,   561,   410,     0,   101,   251,   420,   299,   422,
   300,     0,   276,     0,   421,     0,   287,     0,   288,     0,
   289,     0,   290,     0,   285,     0,   286,     0,   284,     0,
   560,     0,   560,   298,   560,     0,   245,   298,   560,     0,
   560,   298,   245,     0,    64,   175,   554,   484,   255,   425,
   424,   182,   560,     0,   560,     0,     0,    82,     0,     0,
     0,    85,   259,   560,    67,   427,   150,   433,   182,   432,
   498,   219,   434,   428,     0,   246,     0,   470,     0,   431,
     0,   296,   429,   297,     0,   299,   429,   300,     0,   430,
     0,   431,     0,   430,   431,     0,   430,   431,   293,     0,
   431,   293,     0,   456,     0,   464,     0,   461,     0,   435,
     0,   554,   295,   557,     0,   554,     0,   172,     0,   189,
     0,    97,     0,   122,     0,   230,     0,     0,   247,   554,
     0,   234,   554,     0,   235,   554,     0,   235,   289,     0,
   204,   439,     0,    69,   439,     0,    83,   439,     0,   103,
   439,     0,   169,   439,     0,   199,     0,   184,     0,     0,
    85,   195,   560,    67,   470,     0,   236,   562,     0,    85,
   217,   555,   198,   443,   444,     0,    85,   217,   555,     0,
   237,   284,   445,     0,     0,   221,   284,   446,     0,     0,
   568,     0,    96,     0,     0,   568,     0,    96,     0,     0,
   101,   217,   555,     0,   212,   559,   150,   554,     0,   270,
   450,   451,     0,   270,   450,   451,   554,   452,     0,   272,
     0,     0,   207,     0,     0,   299,   453,   300,     0,     0,
   560,     0,   453,   298,   560,     0,   222,   450,   455,     0,
   470,     0,   465,     0,   464,     0,   456,     0,   435,     0,
   461,     0,   122,   125,   554,   457,     0,   192,   299,   551,
   300,     0,    96,   192,     0,   470,     0,   299,   459,   300,
   192,   299,   551,   300,     0,   299,   459,   300,   470,     0,
   299,   459,   300,     0,     0,   459,   298,   460,     0,   460,
     0,   571,   528,     0,    97,   113,   554,   498,     0,   238,
   474,   554,     0,   238,   474,   554,   119,   463,   258,   274,
   274,     0,   238,   474,   554,   119,   274,   274,   274,     0,
   238,   474,   554,   119,   274,   274,     0,   274,     0,     0,
   189,   554,   173,   549,   490,   498,     0,    95,   560,   466,
    92,   111,   470,   467,     0,   210,     0,   121,     0,   170,
     0,   121,   170,     0,     0,   111,   468,     0,     0,   164,
   151,     0,   189,   469,     0,   149,   459,     0,   471,   477,
   489,   481,     0,   299,   471,   300,     0,   472,     0,   471,
   104,   471,     0,   471,   187,   475,   471,     0,   471,   123,
   475,   471,     0,   172,   476,   551,   473,   490,   498,   487,
   488,     0,   125,   332,   474,   554,     0,   125,   643,     0,
     0,   175,     0,     0,    63,     0,     0,    99,     0,    99,
   150,   571,     0,    63,     0,     0,   154,    72,   478,     0,
     0,   479,     0,   478,   298,   479,     0,   526,   480,     0,
   191,   276,     0,   191,   285,     0,   191,   286,     0,    68,
     0,    98,     0,     0,   233,   482,   298,   483,     0,   233,
   482,   249,   483,     0,   233,   482,     0,   249,   483,   233,
   482,     0,   249,   483,     0,     0,   566,     0,    63,     0,
   281,     0,   566,     0,   281,     0,   289,     0,     0,   486,
     0,   560,     0,   486,   298,   560,     0,   116,    72,   529,
     0,     0,   117,   526,     0,     0,   111,   189,     0,   111,
   189,   149,   453,     0,     0,   113,   299,   499,   493,   128,
   499,   495,   300,     0,   113,   491,     0,     0,   491,   298,
   492,     0,   492,    86,   128,   492,     0,   492,     0,   499,
    67,   572,     0,   499,   571,     0,   499,     0,   141,   493,
     0,   114,   494,     0,   132,   494,     0,   168,   494,     0,
   155,     0,   120,     0,   187,     0,     0,   155,     0,     0,
   150,   299,   526,   300,     0,   191,   299,   496,   300,     0,
     0,   497,     0,   496,   298,   497,     0,   571,     0,   571,
   295,   571,     0,   566,     0,   197,   526,     0,     0,   554,
     0,   554,   289,     0,   296,   297,   501,     0,   296,   566,
   297,   501,     0,     0,   296,   297,   501,     0,   296,   566,
   297,   501,     0,     0,   503,   500,     0,   511,     0,   262,
   503,     0,   504,     0,   516,     0,   506,     0,   505,     0,
   652,     0,   203,     0,     3,     0,     4,     0,     5,     0,
     6,     0,     7,     0,     8,     0,     9,     0,    10,     0,
    11,     0,    13,     0,    15,     0,    16,     0,    17,     0,
    18,     0,    19,     0,    20,     0,    21,     0,    22,     0,
    23,     0,    24,     0,    26,     0,    28,     0,    29,     0,
    30,     0,    31,     0,    32,     0,    34,     0,    35,     0,
    36,     0,    37,     0,    38,     0,   110,   508,     0,   100,
   158,     0,    94,   510,     0,   148,   509,     0,   110,     0,
   100,   158,     0,    94,     0,   148,     0,   299,   566,   300,
     0,     0,   299,   566,   298,   566,   300,     0,   299,   566,
   300,     0,     0,   299,   566,   298,   566,   300,     0,   299,
   566,   300,     0,     0,   512,   299,   566,   300,     0,   512,
     0,    77,   513,   514,   515,     0,    76,   513,     0,   193,
     0,   140,    77,   513,     0,   142,   513,     0,   194,     0,
     0,    77,   173,   571,     0,     0,    81,   571,     0,     0,
   517,     0,   179,   518,     0,   178,     0,   124,   519,     0,
   200,     0,   138,     0,    93,     0,   118,     0,   137,     0,
   171,     0,   198,   178,   201,     0,     0,   517,     0,   200,
   182,   138,     0,    93,   182,   118,     0,    93,   182,   137,
     0,    93,   182,   171,     0,   118,   182,   137,     0,   137,
   182,   171,     0,   118,   182,   171,     0,     0,   526,     0,
   147,     0,   299,   522,   300,   119,   299,   472,   300,     0,
   299,   522,   300,   145,   119,   299,   472,   300,     0,   299,
   522,   300,   523,   524,   299,   472,   300,     0,   299,   522,
   300,   523,   299,   472,   300,     0,   299,   522,   300,   523,
   299,   522,   300,     0,   525,   298,   526,     0,   276,     0,
   285,     0,   284,     0,   286,     0,   287,     0,   288,     0,
   289,     0,   290,     0,    66,     0,    63,     0,   525,   298,
   526,     0,   526,     0,   547,   528,     0,   521,     0,   564,
     0,   571,     0,   288,   526,     0,   526,   287,   526,     0,
   526,   288,   526,     0,   526,   290,   526,     0,   526,   289,
   526,     0,   526,   285,   526,     0,   526,   286,   526,     0,
   526,   284,   526,     0,   293,   526,     0,   291,   526,     0,
   526,    59,   502,     0,    75,   299,   526,    67,   502,   300,
     0,   299,   520,   300,     0,   526,   276,   526,     0,   526,
   134,   526,     0,   526,   145,   134,   526,     0,   276,   526,
     0,   526,   276,     0,   561,   299,   289,   300,     0,   561,
   299,   300,     0,   561,   299,   529,   300,     0,    88,     0,
    89,     0,    89,   299,   566,   300,     0,    90,     0,    90,
   299,   566,   300,     0,    91,     0,   190,     0,   106,   299,
   472,   300,     0,   107,   299,   530,   300,     0,   157,   299,
   532,   300,     0,   174,   299,   534,   300,     0,   185,   299,
    71,   537,   300,     0,   185,   299,   131,   537,   300,     0,
   185,   299,   183,   537,   300,     0,   185,   299,   537,   300,
     0,   526,   231,     0,   526,   126,   147,     0,   526,   248,
     0,   526,   126,   145,   147,     0,   526,   126,   186,     0,
   526,   126,   145,   108,     0,   526,   126,   108,     0,   526,
   126,   145,   186,     0,   526,    70,   527,    65,   527,     0,
   526,   145,    70,   527,    65,   527,     0,   526,   119,   299,
   538,   300,     0,   526,   145,   119,   299,   540,   300,     0,
   526,   276,   299,   472,   300,     0,   526,   287,   299,   472,
   300,     0,   526,   288,   299,   472,   300,     0,   526,   290,
   299,   472,   300,     0,   526,   289,   299,   472,   300,     0,
   526,   285,   299,   472,   300,     0,   526,   286,   299,   472,
   300,     0,   526,   284,   299,   472,   300,     0,   526,   276,
    66,   299,   472,   300,     0,   526,   287,    66,   299,   472,
   300,     0,   526,   288,    66,   299,   472,   300,     0,   526,
   290,    66,   299,   472,   300,     0,   526,   289,    66,   299,
   472,   300,     0,   526,   285,    66,   299,   472,   300,     0,
   526,   286,    66,   299,   472,   300,     0,   526,   284,    66,
   299,   472,   300,     0,   526,   276,    63,   299,   472,   300,
     0,   526,   287,    63,   299,   472,   300,     0,   526,   288,
    63,   299,   472,   300,     0,   526,   290,    63,   299,   472,
   300,     0,   526,   289,    63,   299,   472,   300,     0,   526,
   285,    63,   299,   472,   300,     0,   526,   286,    63,   299,
   472,   300,     0,   526,   284,    63,   299,   472,   300,     0,
   526,    65,   526,     0,   526,   153,   526,     0,   145,   526,
     0,   542,     0,   648,     0,   547,   528,     0,   564,     0,
   571,     0,   288,   527,     0,   527,   287,   527,     0,   527,
   288,   527,     0,   527,   290,   527,     0,   527,   289,   527,
     0,   293,   527,     0,   291,   527,     0,   527,    59,   502,
     0,    75,   299,   527,    67,   502,   300,     0,   299,   526,
   300,     0,   527,   276,   527,     0,   276,   527,     0,   527,
   276,     0,   561,   299,   300,     0,   561,   299,   529,   300,
     0,    88,     0,    89,     0,    89,   299,   566,   300,     0,
    90,     0,    90,   299,   566,   300,     0,    91,     0,   190,
     0,   157,   299,   532,   300,     0,   174,   299,   534,   300,
     0,   185,   299,    71,   537,   300,     0,   185,   299,   131,
   537,   300,     0,   185,   299,   183,   537,   300,     0,   185,
   299,   537,   300,     0,   649,     0,   296,   642,   297,   528,
     0,   296,   642,   292,   642,   297,   528,     0,     0,   520,
     0,   529,   298,   520,     0,   529,   191,   526,     0,   531,
   113,   526,     0,     0,   648,     0,   517,     0,   180,     0,
   181,     0,   533,   119,   533,     0,     0,   547,   528,     0,
   564,     0,   288,   533,     0,   533,   287,   533,     0,   533,
   288,   533,     0,   533,   290,   533,     0,   533,   289,   533,
     0,   291,   533,     0,   533,    59,   502,     0,    75,   299,
   533,    67,   502,   300,     0,   299,   533,   300,     0,   533,
   276,   533,     0,   276,   533,     0,   533,   276,     0,   571,
     0,   561,   299,   300,     0,   561,   299,   529,   300,     0,
   157,   299,   532,   300,     0,   174,   299,   534,   300,     0,
   185,   299,    71,   537,   300,     0,   185,   299,   131,   537,
   300,     0,   185,   299,   183,   537,   300,     0,   185,   299,
   537,   300,     0,   529,   535,   536,     0,     0,   113,   529,
     0,     0,   111,   529,     0,     0,   526,   113,   529,     0,
   113,   529,     0,   529,     0,   472,     0,   539,     0,   564,
     0,   539,   298,   564,     0,   472,     0,   541,     0,   564,
     0,   541,   298,   564,     0,    74,   546,   543,   545,   103,
     0,   146,   299,   526,   298,   526,   300,     0,    80,   299,
   529,   300,     0,   543,   544,     0,   544,     0,   196,   526,
   177,   520,     0,   102,   520,     0,     0,   547,   528,     0,
   571,     0,     0,   554,   295,   548,     0,   565,   295,   548,
     0,   557,     0,   548,   295,   557,     0,   548,   295,   289,
     0,   549,   298,   550,     0,   550,     0,   289,     0,   571,
   528,   284,   520,     0,   547,   528,     0,   554,   295,   289,
     0,   551,   298,   552,     0,   552,     0,   520,    67,   572,
     0,   520,     0,   554,   295,   289,     0,   289,     0,   571,
     0,     0,   573,     0,   571,     0,   571,     0,   652,     0,
   571,     0,   652,     0,   571,     0,   571,     0,   571,     0,
   568,     0,   652,     0,   566,     0,   567,     0,   568,     0,
   502,   568,     0,   565,     0,   186,     0,   108,     0,   281,
   528,     0,   280,     0,   282,     0,   275,     0,   652,     0,
   571,     0,   507,     0,   512,     0,   652,     0,   517,     0,
    60,     0,    61,     0,   205,     0,   206,     0,   208,     0,
   209,     0,   211,     0,   214,     0,   215,     0,   216,     0,
   217,     0,   218,     0,   100,     0,   220,     0,   221,     0,
   224,     0,   225,     0,   226,     0,   227,     0,   228,     0,
   229,     0,   121,     0,   230,     0,   231,     0,   129,     0,
   130,     0,   232,     0,   237,     0,   136,     0,   239,     0,
   240,     0,   143,     0,   243,     0,   244,     0,   246,     0,
   248,     0,   149,     0,   250,     0,   151,     0,   251,     0,
   152,     0,   252,     0,   160,     0,   161,     0,   253,     0,
   164,     0,   254,     0,   166,     0,   255,     0,   257,     0,
   258,     0,   259,     0,   170,     0,   261,     0,   260,     0,
   264,     0,   265,     0,   266,     0,   267,     0,   178,     0,
   179,     0,   180,     0,   181,     0,   202,     0,   268,     0,
   203,     0,   271,     0,   273,     0,   201,     0,     3,     0,
     4,     0,     5,     0,     6,     0,     7,     0,     8,     0,
     9,     0,    10,     0,    11,     0,    13,     0,    15,     0,
    16,     0,    17,     0,    18,     0,    19,     0,    20,     0,
    21,     0,    22,     0,    23,     0,    24,     0,    26,     0,
    28,     0,    29,     0,    30,     0,    31,     0,    32,     0,
    34,     0,    35,     0,    36,     0,    37,     0,    38,     0,
   571,     0,   204,     0,   207,     0,   210,     0,    74,     0,
   212,     0,    80,     0,    84,     0,   213,     0,    86,     0,
    87,     0,   219,     0,   102,     0,   103,     0,   222,     0,
   223,     0,   108,     0,   112,     0,   116,     0,   234,     0,
   236,     0,   238,     0,   241,     0,   242,     0,   245,     0,
   146,     0,   154,     0,   157,     0,   158,     0,   256,     0,
   262,     0,   263,     0,   175,     0,   177,     0,   184,     0,
   186,     0,   270,     0,   272,     0,   196,     0,    87,     0,
   242,     0,     7,   182,   575,   581,   582,     0,     7,   182,
    96,     0,     7,   583,     0,   555,   578,   580,     0,   576,
   577,   580,   290,   555,   586,     0,   585,     0,   568,     0,
   652,   650,     0,   276,   579,     0,   577,     0,     0,   571,
     0,   571,   295,   579,     0,   292,   566,     0,     0,    67,
   575,     0,     0,   190,   583,     0,     0,   584,     0,   584,
   290,   571,     0,   584,    17,    72,   584,     0,   584,   191,
   584,     0,   569,     0,   585,     0,   275,     0,   650,     0,
   276,   571,     0,     0,    95,   560,   466,    92,   111,   652,
   467,     0,    10,    23,   652,     0,     0,   591,   590,   593,
   592,     0,   644,    69,    95,    26,    27,     0,   644,   103,
    95,    26,    27,     0,     0,   594,   593,     0,     0,     0,
   597,   595,   598,   596,   608,   293,     0,    46,     0,    54,
     0,    53,     0,    43,     0,    51,     0,    40,     0,     0,
   606,     0,   607,     0,   601,     0,   602,     0,   599,     0,
   653,     0,   600,   301,   655,   302,     0,    45,   605,     0,
   603,   301,   593,   302,     0,   604,   301,   593,   302,     0,
    55,   605,     0,    56,   605,     0,     0,   653,     0,    52,
     0,    57,    52,     0,    48,     0,    57,    48,     0,    50,
     0,    57,    50,     0,    47,     0,    44,     0,    41,     0,
    42,     0,    57,    42,     0,    58,     0,   609,     0,   608,
   298,   609,     0,   611,   653,   500,   610,     0,     0,   284,
   646,     0,     0,   289,     0,    95,   265,   652,     0,    11,
   614,     0,   615,     0,    87,     0,    63,     0,     0,   575,
     0,    96,     0,   105,    18,   618,     0,     0,   105,   652,
   617,   621,     0,   585,     0,   277,     0,    14,   652,     0,
    22,   560,   621,     0,     0,   191,   622,     0,   648,     0,
   648,   298,   622,     0,    23,   652,   113,   585,     0,   438,
    24,     0,   173,     8,   615,     0,   203,   653,   126,   630,
   627,   629,     0,   296,   297,   628,     0,   299,   300,   628,
     0,   296,   566,   297,   628,     0,   299,   566,   300,   628,
     0,     0,   296,   297,   628,     0,   299,   300,   628,     0,
   296,   566,   297,   628,     0,   299,   566,   300,   628,     0,
     0,    25,     0,     0,    76,     0,   193,     0,   110,     0,
   100,     0,   633,    20,     0,    12,     0,   633,    28,     0,
   633,    21,     0,     4,     0,    36,    20,     0,    36,    28,
     0,    36,    21,     0,     0,    35,   631,   301,   634,   302,
     0,     0,   187,   632,   301,   634,   302,     0,   653,     0,
    29,     0,     0,     0,   635,   634,     0,     0,   630,   636,
   637,    27,     0,   638,     0,   637,   298,   638,     0,   611,
   653,   500,     0,    37,   653,   126,   630,   627,   629,     0,
    38,    30,   641,     0,    38,   145,    13,   641,     0,    38,
    32,   641,     0,     9,     0,    31,     0,    34,     0,    16,
   560,     0,    15,   182,   560,     0,   219,   560,   299,   645,
   300,     0,   219,     5,     0,     6,   560,   299,   645,   300,
     0,   547,   528,     0,   521,     0,   564,     0,   571,     0,
   288,   642,     0,   526,   287,   642,     0,   526,   288,   642,
     0,   526,   290,   642,     0,   526,   289,   642,     0,   526,
   285,   642,     0,   526,   286,   642,     0,   526,   284,   642,
     0,   293,   642,     0,   291,   642,     0,   526,    59,   502,
     0,    75,   299,   526,    67,   502,   300,     0,   299,   520,
   300,     0,   526,   276,   642,     0,   526,   134,   642,     0,
   526,   145,   134,   642,     0,   276,   642,     0,   526,   276,
     0,   561,   299,   289,   300,     0,   561,   299,   300,     0,
   561,   299,   529,   300,     0,    88,     0,    89,     0,    89,
   299,   566,   300,     0,    90,     0,    90,   299,   566,   300,
     0,    91,     0,   106,   299,   472,   300,     0,   107,   299,
   530,   300,     0,   157,   299,   532,   300,     0,   174,   299,
   534,   300,     0,   185,   299,    71,   537,   300,     0,   185,
   299,   131,   537,   300,     0,   185,   299,   183,   537,   300,
     0,   185,   299,   537,   300,     0,   526,   231,     0,   526,
   126,   147,     0,   526,   248,     0,   526,   126,   145,   147,
     0,   526,   126,   186,     0,   526,   126,   145,   108,     0,
   526,   126,   108,     0,   526,   126,   145,   186,     0,   526,
    70,   527,    65,   527,     0,   526,   145,    70,   527,    65,
   527,     0,   526,   119,   299,   538,   300,     0,   526,   145,
   119,   299,   540,   300,     0,   526,   276,   299,   472,   300,
     0,   526,   287,   299,   472,   300,     0,   526,   288,   299,
   472,   300,     0,   526,   290,   299,   472,   300,     0,   526,
   289,   299,   472,   300,     0,   526,   285,   299,   472,   300,
     0,   526,   286,   299,   472,   300,     0,   526,   284,   299,
   472,   300,     0,   526,   276,    66,   299,   472,   300,     0,
   526,   287,    66,   299,   472,   300,     0,   526,   288,    66,
   299,   472,   300,     0,   526,   290,    66,   299,   472,   300,
     0,   526,   289,    66,   299,   472,   300,     0,   526,   285,
    66,   299,   472,   300,     0,   526,   286,    66,   299,   472,
   300,     0,   526,   284,    66,   299,   472,   300,     0,   526,
   276,    63,   299,   472,   300,     0,   526,   287,    63,   299,
   472,   300,     0,   526,   288,    63,   299,   472,   300,     0,
   526,   290,    63,   299,   472,   300,     0,   526,   289,    63,
   299,   472,   300,     0,   526,   285,    63,   299,   472,   300,
     0,   526,   286,    63,   299,   472,   300,     0,   526,   284,
    63,   299,   472,   300,     0,   526,    65,   642,     0,   526,
   153,   642,     0,   145,   642,     0,   649,     0,   647,     0,
   643,   298,   647,     0,    33,     0,     0,   645,   658,     0,
   659,     0,   646,   659,     0,   650,   651,     0,   650,   651,
     0,   650,     0,   278,     0,     0,   650,     0,    19,   650,
     0,    19,   560,     0,   274,     0,   277,     0,   274,     0,
   279,     0,   657,     0,   655,   657,     0,   657,     0,   293,
     0,   274,     0,   277,     0,   566,     0,   567,     0,   289,
     0,    40,     0,    41,     0,    42,     0,    43,     0,    44,
     0,    45,     0,    46,     0,    47,     0,    48,     0,    50,
     0,    51,     0,    52,     0,    53,     0,    54,     0,    55,
     0,    56,     0,    57,     0,    58,     0,    39,     0,   296,
     0,   297,     0,   299,     0,   300,     0,   284,     0,   298,
     0,   274,     0,   277,     0,   566,     0,   567,     0,   298,
     0,   274,     0,   277,     0,   566,     0,   567,     0,   301,
   655,   302,     0,   301,     0,   302,     0
};

#endif

#if YYDEBUG != 0
static const short yyrline[] = { 0,
   808,   810,   811,   813,   814,   815,   816,   817,   818,   819,
   821,   823,   824,   825,   826,   827,   828,   829,   830,   831,
   832,   833,   834,   835,   836,   837,   838,   839,   840,   841,
   842,   843,   844,   845,   846,   847,   848,   849,   850,   851,
   852,   853,   854,   855,   864,   865,   870,   871,   872,   873,
   874,   875,   876,   877,   878,   887,   891,   899,   903,   911,
   914,   919,   944,   952,   953,   961,   968,   975,   997,  1011,
  1025,  1031,  1032,  1035,  1039,  1043,  1046,  1050,  1054,  1057,
  1061,  1067,  1068,  1071,  1072,  1084,  1088,  1092,  1096,  1106,
  1116,  1126,  1127,  1130,  1131,  1132,  1135,  1139,  1143,  1149,
  1153,  1157,  1171,  1177,  1181,  1185,  1187,  1189,  1191,  1202,
  1217,  1223,  1225,  1234,  1235,  1236,  1239,  1240,  1243,  1244,
  1250,  1251,  1263,  1270,  1271,  1274,  1278,  1282,  1285,  1286,
  1289,  1293,  1299,  1300,  1303,  1304,  1307,  1311,  1317,  1322,
  1341,  1345,  1349,  1353,  1357,  1361,  1365,  1372,  1376,  1390,
  1392,  1394,  1396,  1398,  1400,  1402,  1404,  1406,  1412,  1414,
  1416,  1418,  1422,  1424,  1426,  1428,  1434,  1436,  1439,  1441,
  1443,  1449,  1451,  1457,  1459,  1467,  1471,  1475,  1479,  1483,
  1487,  1494,  1498,  1504,  1506,  1508,  1512,  1514,  1516,  1518,
  1520,  1522,  1524,  1526,  1532,  1534,  1536,  1540,  1544,  1546,
  1550,  1554,  1556,  1558,  1560,  1562,  1564,  1566,  1568,  1570,
  1572,  1574,  1576,  1578,  1580,  1582,  1584,  1586,  1588,  1590,
  1592,  1595,  1599,  1604,  1609,  1610,  1611,  1614,  1615,  1616,
  1619,  1620,  1623,  1624,  1625,  1626,  1629,  1630,  1633,  1639,
  1640,  1643,  1644,  1647,  1657,  1663,  1665,  1668,  1672,  1676,
  1680,  1684,  1688,  1694,  1695,  1697,  1701,  1708,  1712,  1726,
  1733,  1734,  1736,  1750,  1758,  1759,  1762,  1766,  1770,  1776,
  1777,  1778,  1781,  1787,  1788,  1791,  1792,  1795,  1797,  1799,
  1803,  1807,  1811,  1812,  1815,  1828,  1834,  1840,  1841,  1842,
  1845,  1846,  1847,  1848,  1849,  1852,  1855,  1856,  1859,  1862,
  1866,  1872,  1873,  1874,  1875,  1876,  1889,  1893,  1910,  1917,
  1923,  1924,  1925,  1926,  1931,  1934,  1935,  1936,  1937,  1938,
  1939,  1942,  1943,  1945,  1956,  1962,  1966,  1970,  1976,  1980,
  1986,  1990,  1994,  1998,  2002,  2008,  2012,  2016,  2022,  2026,
  2037,  2055,  2064,  2065,  2068,  2069,  2072,  2073,  2076,  2077,
  2080,  2086,  2092,  2093,  2094,  2103,  2104,  2105,  2115,  2129,
  2151,  2157,  2158,  2161,  2162,  2165,  2166,  2170,  2176,  2177,
  2198,  2204,  2205,  2206,  2207,  2211,  2217,  2218,  2222,  2229,
  2235,  2235,  2237,  2238,  2239,  2240,  2241,  2242,  2243,  2246,
  2250,  2252,  2254,  2267,  2274,  2275,  2278,  2279,  2292,  2294,
  2301,  2302,  2303,  2304,  2305,  2308,  2309,  2312,  2314,  2316,
  2320,  2321,  2322,  2323,  2326,  2330,  2337,  2338,  2339,  2340,
  2343,  2344,  2356,  2362,  2368,  2372,  2390,  2391,  2392,  2393,
  2394,  2396,  2397,  2398,  2408,  2422,  2436,  2446,  2452,  2453,
  2456,  2457,  2460,  2461,  2462,  2465,  2466,  2467,  2477,  2491,
  2505,  2509,  2517,  2518,  2521,  2522,  2525,  2526,  2529,  2531,
  2543,  2561,  2562,  2563,  2564,  2565,  2566,  2583,  2589,  2593,
  2597,  2601,  2605,  2611,  2612,  2615,  2618,  2622,  2636,  2643,
  2647,  2678,  2698,  2715,  2716,  2729,  2745,  2776,  2777,  2778,
  2779,  2780,  2783,  2784,  2788,  2789,  2795,  2808,  2825,  2829,
  2833,  2838,  2843,  2851,  2861,  2862,  2863,  2866,  2867,  2870,
  2871,  2874,  2875,  2876,  2877,  2880,  2881,  2884,  2885,  2888,
  2894,  2895,  2896,  2897,  2898,  2899,  2902,  2904,  2906,  2908,
  2910,  2912,  2916,  2917,  2918,  2921,  2922,  2932,  2933,  2936,
  2938,  2940,  2944,  2945,  2948,  2952,  2955,  2960,  2964,  2978,
  2982,  2983,  2986,  2988,  2990,  2994,  2998,  3002,  3008,  3009,
  3011,  3013,  3015,  3017,  3019,  3021,  3025,  3026,  3029,  3030,
  3031,  3034,  3035,  3038,  3042,  3046,  3052,  3053,  3056,  3061,
  3067,  3073,  3079,  3087,  3093,  3099,  3117,  3121,  3122,  3128,
  3129,  3130,  3133,  3139,  3140,  3141,  3142,  3143,  3144,  3145,
  3146,  3147,  3148,  3149,  3150,  3151,  3152,  3153,  3154,  3155,
  3156,  3157,  3158,  3159,  3160,  3161,  3162,  3163,  3164,  3165,
  3166,  3167,  3168,  3169,  3170,  3171,  3179,  3183,  3187,  3191,
  3197,  3199,  3201,  3203,  3207,  3215,  3221,  3233,  3241,  3247,
  3259,  3267,  3280,  3300,  3306,  3313,  3314,  3315,  3316,  3319,
  3320,  3323,  3324,  3327,  3328,  3331,  3335,  3339,  3343,  3349,
  3350,  3351,  3352,  3353,  3354,  3357,  3358,  3361,  3362,  3363,
  3364,  3365,  3366,  3367,  3368,  3369,  3379,  3381,  3396,  3400,
  3404,  3408,  3412,  3418,  3424,  3425,  3426,  3427,  3428,  3429,
  3430,  3431,  3434,  3435,  3439,  3443,  3458,  3462,  3464,  3466,
  3470,  3472,  3474,  3476,  3478,  3480,  3482,  3484,  3489,  3491,
  3493,  3497,  3501,  3503,  3505,  3507,  3509,  3511,  3513,  3517,
  3521,  3525,  3529,  3533,  3539,  3543,  3549,  3553,  3558,  3562,
  3566,  3570,  3575,  3579,  3583,  3587,  3591,  3593,  3595,  3597,
  3604,  3608,  3612,  3616,  3620,  3624,  3628,  3632,  3636,  3640,
  3644,  3648,  3652,  3656,  3660,  3664,  3668,  3672,  3676,  3680,
  3684,  3688,  3692,  3696,  3700,  3704,  3708,  3712,  3716,  3720,
  3724,  3728,  3732,  3734,  3736,  3738,  3740,  3749,  3753,  3755,
  3759,  3761,  3763,  3765,  3767,  3772,  3774,  3776,  3780,  3784,
  3786,  3788,  3790,  3792,  3796,  3800,  3804,  3808,  3814,  3818,
  3824,  3828,  3832,  3836,  3841,  3845,  3849,  3853,  3857,  3861,
  3865,  3869,  3873,  3875,  3877,  3881,  3885,  3887,  3891,  3892,
  3893,  3896,  3898,  3902,  3906,  3908,  3910,  3912,  3914,  3916,
  3918,  3920,  3924,  3928,  3930,  3932,  3934,  3936,  3940,  3944,
  3948,  3952,  3957,  3961,  3965,  3969,  3975,  3979,  3983,  3985,
  3991,  3993,  3997,  3999,  4001,  4005,  4009,  4013,  4015,  4019,
  4023,  4027,  4029,  4048,  4050,  4056,  4064,  4066,  4070,  4076,
  4077,  4080,  4084,  4088,  4092,  4096,  4102,  4104,  4106,  4117,
  4119,  4121,  4124,  4128,  4132,  4143,  4145,  4150,  4154,  4158,
  4162,  4168,  4169,  4172,  4176,  4189,  4190,  4191,  4192,  4193,
  4199,  4200,  4202,  4203,  4208,  4212,  4216,  4220,  4224,  4226,
  4230,  4236,  4242,  4243,  4244,  4252,  4259,  4261,  4263,  4274,
  4275,  4276,  4277,  4278,  4279,  4280,  4281,  4282,  4283,  4284,
  4285,  4286,  4287,  4288,  4289,  4290,  4291,  4292,  4293,  4294,
  4295,  4296,  4297,  4298,  4299,  4300,  4301,  4302,  4303,  4304,
  4305,  4306,  4307,  4308,  4309,  4310,  4311,  4312,  4313,  4314,
  4315,  4316,  4317,  4318,  4319,  4320,  4321,  4322,  4323,  4324,
  4325,  4326,  4327,  4328,  4329,  4330,  4331,  4332,  4333,  4334,
  4335,  4336,  4337,  4338,  4339,  4340,  4341,  4342,  4343,  4344,
  4345,  4346,  4347,  4348,  4349,  4350,  4351,  4352,  4353,  4354,
  4355,  4356,  4357,  4358,  4359,  4360,  4361,  4362,  4363,  4364,
  4365,  4366,  4367,  4368,  4369,  4370,  4371,  4372,  4373,  4374,
  4375,  4387,  4388,  4389,  4390,  4391,  4392,  4393,  4394,  4395,
  4396,  4397,  4398,  4399,  4400,  4401,  4402,  4403,  4404,  4405,
  4406,  4407,  4408,  4409,  4410,  4411,  4412,  4413,  4414,  4415,
  4416,  4417,  4418,  4419,  4420,  4421,  4422,  4423,  4424,  4425,
  4428,  4435,  4451,  4455,  4460,  4465,  4476,  4499,  4503,  4511,
  4528,  4539,  4540,  4542,  4543,  4545,  4546,  4548,  4549,  4551,
  4552,  4554,  4558,  4562,  4566,  4571,  4576,  4577,  4579,  4603,
  4616,  4622,  4665,  4670,  4675,  4682,  4684,  4686,  4690,  4695,
  4700,  4705,  4710,  4711,  4712,  4713,  4714,  4715,  4716,  4718,
  4725,  4732,  4739,  4746,  4753,  4765,  4770,  4772,  4779,  4786,
  4794,  4802,  4803,  4805,  4806,  4807,  4808,  4809,  4810,  4811,
  4812,  4813,  4814,  4815,  4817,  4819,  4823,  4828,  4901,  4902,
  4904,  4905,  4911,  4919,  4921,  4922,  4923,  4924,  4926,  4927,
  4932,  4945,  4957,  4961,  4961,  4968,  4973,  4977,  4978,  4983,
  4983,  4989,  4999,  5015,  5023,  5065,  5071,  5077,  5083,  5089,
  5097,  5103,  5109,  5115,  5121,  5128,  5129,  5131,  5138,  5145,
  5152,  5159,  5166,  5173,  5180,  5187,  5194,  5201,  5208,  5215,
  5220,  5228,  5233,  5241,  5252,  5252,  5254,  5258,  5264,  5270,
  5275,  5279,  5284,  5355,  5410,  5415,  5420,  5426,  5431,  5436,
  5441,  5446,  5451,  5456,  5461,  5468,  5472,  5474,  5476,  5480,
  5482,  5484,  5486,  5488,  5490,  5492,  5494,  5498,  5500,  5502,
  5506,  5510,  5512,  5514,  5516,  5518,  5520,  5522,  5526,  5530,
  5534,  5538,  5542,  5548,  5552,  5558,  5562,  5566,  5570,  5574,
  5579,  5583,  5587,  5591,  5595,  5597,  5599,  5601,  5608,  5612,
  5616,  5620,  5624,  5628,  5632,  5636,  5640,  5644,  5648,  5652,
  5656,  5660,  5664,  5668,  5672,  5676,  5680,  5684,  5688,  5692,
  5696,  5700,  5704,  5708,  5712,  5716,  5720,  5724,  5728,  5732,
  5736,  5738,  5740,  5742,  5746,  5746,  5748,  5750,  5751,  5753,
  5754,  5756,  5760,  5764,  5769,  5771,  5772,  5773,  5774,  5776,
  5777,  5782,  5784,  5786,  5787,  5792,  5792,  5794,  5795,  5796,
  5797,  5798,  5799,  5800,  5801,  5802,  5803,  5804,  5805,  5806,
  5807,  5808,  5809,  5810,  5811,  5812,  5813,  5814,  5815,  5816,
  5817,  5818,  5819,  5820,  5821,  5822,  5823,  5825,  5826,  5827,
  5828,  5829,  5831,  5832,  5833,  5834,  5835,  5837,  5842
};
#endif


#if YYDEBUG != 0 || defined (YYERROR_VERBOSE)

static const char * const yytname[] = {   "$","error","$undefined.","SQL_AT",
"SQL_BOOL","SQL_BREAK","SQL_CALL","SQL_CONNECT","SQL_CONNECTION","SQL_CONTINUE",
"SQL_DEALLOCATE","SQL_DISCONNECT","SQL_ENUM","SQL_FOUND","SQL_FREE","SQL_GO",
"SQL_GOTO","SQL_IDENTIFIED","SQL_IMMEDIATE","SQL_INDICATOR","SQL_INT","SQL_LONG",
"SQL_OPEN","SQL_PREPARE","SQL_RELEASE","SQL_REFERENCE","SQL_SECTION","SQL_SEMI",
"SQL_SHORT","SQL_SIGNED","SQL_SQLERROR","SQL_SQLPRINT","SQL_SQLWARNING","SQL_START",
"SQL_STOP","SQL_STRUCT","SQL_UNSIGNED","SQL_VAR","SQL_WHENEVER","S_ANYTHING",
"S_AUTO","S_BOOL","S_CHAR","S_CONST","S_DOUBLE","S_ENUM","S_EXTERN","S_FLOAT",
"S_INT","S","S_LONG","S_REGISTER","S_SHORT","S_SIGNED","S_STATIC","S_STRUCT",
"S_UNION","S_UNSIGNED","S_VARCHAR","TYPECAST","ABSOLUTE","ACTION","ADD","ALL",
"ALTER","AND","ANY","AS","ASC","BEGIN_TRANS","BETWEEN","BOTH","BY","CASCADE",
"CASE","CAST","CHAR","CHARACTER","CHECK","CLOSE","COALESCE","COLLATE","COLUMN",
"COMMIT","CONSTRAINT","CREATE","CROSS","CURRENT","CURRENT_DATE","CURRENT_TIME",
"CURRENT_TIMESTAMP","CURRENT_USER","CURSOR","DAY_P","DECIMAL","DECLARE","DEFAULT",
"DELETE","DESC","DISTINCT","DOUBLE","DROP","ELSE","END_TRANS","EXCEPT","EXECUTE",
"EXISTS","EXTRACT","FALSE_P","FETCH","FLOAT","FOR","FOREIGN","FROM","FULL","GRANT",
"GROUP","HAVING","HOUR_P","IN","INNER_P","INSENSITIVE","INSERT","INTERSECT",
"INTERVAL","INTO","IS","ISOLATION","JOIN","KEY","LANGUAGE","LEADING","LEFT",
"LEVEL","LIKE","LOCAL","MATCH","MINUTE_P","MONTH_P","NAMES","NATIONAL","NATURAL",
"NCHAR","NEXT","NO","NOT","NULLIF","NULL_P","NUMERIC","OF","ON","ONLY","OPTION",
"OR","ORDER","OUTER_P","PARTIAL","POSITION","PRECISION","PRIMARY","PRIOR","PRIVILEGES",
"PROCEDURE","PUBLIC","READ","REFERENCES","RELATIVE","REVOKE","RIGHT","ROLLBACK",
"SCROLL","SECOND_P","SELECT","SET","SUBSTRING","TABLE","TEMP","THEN","TIME",
"TIMESTAMP","TIMEZONE_HOUR","TIMEZONE_MINUTE","TO","TRAILING","TRANSACTION",
"TRIM","TRUE_P","UNION","UNIQUE","UPDATE","USER","USING","VALUES","VARCHAR",
"VARYING","VIEW","WHEN","WHERE","WITH","WORK","YEAR_P","ZONE","TRIGGER","TYPE_P",
"ABORT_TRANS","AFTER","AGGREGATE","ANALYZE","BACKWARD","BEFORE","BINARY","CACHE",
"CLUSTER","COPY","CREATEDB","CREATEUSER","CYCLE","DATABASE","DELIMITERS","DO",
"EACH","ENCODING","EXPLAIN","EXTEND","FORWARD","FUNCTION","HANDLER","INCREMENT",
"INDEX","INHERITS","INSTEAD","ISNULL","LANCOMPILER","LIMIT","LISTEN","UNLISTEN",
"LOAD","LOCATION","LOCK_P","MAXVALUE","MINVALUE","MOVE","NEW","NOCREATEDB","NOCREATEUSER",
"NONE","NOTHING","NOTIFY","NOTNULL","OFFSET","OIDS","OPERATOR","PASSWORD","PROCEDURAL",
"RECIPE","RENAME","RESET","RETURNS","ROW","RULE","SERIAL","SEQUENCE","SETOF",
"SHOW","START","STATEMENT","STDIN","STDOUT","TRUSTED","UNTIL","VACUUM","VALID",
"VERBOSE","VERSION","IDENT","SCONST","Op","CSTRING","CVARIABLE","CPP_LINE","ICONST",
"PARAM","FCONST","OP","'='","'<'","'>'","'+'","'-'","'*'","'/'","'|'","':'",
"';'","UMINUS","'.'","'['","']'","','","'('","')'","'{'","'}'","prog","statements",
"statement","opt_at","stmt","CreateUserStmt","AlterUserStmt","DropUserStmt",
"user_passwd_clause","user_createdb_clause","user_createuser_clause","user_group_list",
"user_group_clause","user_valid_clause","VariableSetStmt","var_value","zone_value",
"VariableShowStmt","VariableResetStmt","AddAttrStmt","alter_clause","ClosePortalStmt",
"CopyStmt","copy_dirn","copy_file_name","opt_binary","opt_with_copy","copy_delimiter",
"CreateStmt","OptTemp","OptTableElementList","OptTableElement","columnDef","ColQualifier",
"ColQualList","ColPrimaryKey","ColConstraint","ColConstraintElem","default_list",
"default_expr","TableConstraint","ConstraintElem","constraint_list","constraint_expr",
"c_list","c_expr","key_match","key_actions","key_action","key_reference","OptInherit",
"CreateAsStmt","OptCreateAs","CreateAsList","CreateAsElement","CreateSeqStmt",
"OptSeqList","OptSeqElem","NumericOnly","FloatOnly","IntegerOnly","CreatePLangStmt",
"PLangTrusted","DropPLangStmt","CreateTrigStmt","TriggerActionTime","TriggerEvents",
"TriggerOneEvent","TriggerForSpec","TriggerForOpt","TriggerForType","TriggerFuncArgs",
"TriggerFuncArg","DropTrigStmt","DefineStmt","def_rest","def_type","def_name",
"definition","def_list","def_elem","def_arg","DestroyStmt","FetchStmt","opt_direction",
"fetch_how_many","opt_portal_name","GrantStmt","privileges","operation_commalist",
"operation","grantee","opt_with_grant","RevokeStmt","IndexStmt","index_opt_unique",
"access_method_clause","index_params","index_list","func_index","index_elem",
"opt_type","opt_class","ExtendStmt","RecipeStmt","ProcedureStmt","opt_with",
"func_args","func_args_list","func_return","set_opt","RemoveStmt","remove_type",
"RemoveAggrStmt","aggr_argtype","RemoveFuncStmt","RemoveOperStmt","all_Op","MathOp",
"oper_argtypes","RenameStmt","opt_name","opt_column","RuleStmt","@1","RuleActionList",
"RuleActionBlock","RuleActionMulti","RuleActionStmt","event_object","event",
"opt_instead","NotifyStmt","ListenStmt","UnlistenStmt","TransactionStmt","opt_trans",
"ViewStmt","LoadStmt","CreatedbStmt","opt_database1","opt_database2","location",
"encoding","DestroydbStmt","ClusterStmt","VacuumStmt","opt_verbose","opt_analyze",
"opt_va_list","va_list","ExplainStmt","OptimizableStmt","InsertStmt","insert_rest",
"opt_column_list","columnList","columnElem","DeleteStmt","LockStmt","opt_lmode",
"UpdateStmt","CursorStmt","opt_cursor","cursor_clause","opt_readonly","opt_of",
"SelectStmt","select_w_o_sort","SubSelect","result","opt_table","opt_union",
"opt_unique","sort_clause","sortby_list","sortby","OptUseOp","opt_select_limit",
"select_limit_value","select_offset_value","opt_inh_star","relation_name_list",
"name_list","group_clause","having_clause","for_update_clause","from_clause",
"from_list","from_val","join_expr","join_outer","join_spec","join_list","join_using",
"where_clause","relation_expr","opt_array_bounds","nest_array_bounds","Typename",
"Array","Generic","generic","Numeric","numeric","opt_float","opt_numeric","opt_decimal",
"Character","character","opt_varying","opt_charset","opt_collate","Datetime",
"datetime","opt_timezone","opt_interval","a_expr_or_null","row_expr","row_descriptor",
"row_op","sub_type","row_list","a_expr","b_expr","opt_indirection","expr_list",
"extract_list","extract_arg","position_list","position_expr","substr_list","substr_from",
"substr_for","trim_list","in_expr","in_expr_nodes","not_in_expr","not_in_expr_nodes",
"case_expr","when_clause_list","when_clause","case_default","case_arg","attr",
"attrs","res_target_list","res_target_el","res_target_list2","res_target_el2",
"opt_id","relation_name","database_name","access_method","attr_name","class",
"index_name","name","func_name","file_name","recipe_name","AexprConst","ParamNo",
"Iconst","Fconst","Sconst","UserId","TypeId","ColId","ColLabel","SpecialRuleRelation",
"ECPGConnect","connection_target","db_prefix","server","opt_server","server_name",
"opt_port","opt_connection_name","opt_user","ora_user","user_name","char_variable",
"opt_options","ECPGCursorStmt","ECPGDeallocate","ECPGDeclaration","@2","sql_startdeclare",
"sql_enddeclare","variable_declarations","declaration","@3","@4","storage_clause",
"type","enum_type","s_enum","struct_type","union_type","s_struct","s_union",
"opt_symbol","simple_type","varchar_type","variable_list","variable","opt_initializer",
"opt_pointer","ECPGDeclare","ECPGDisconnect","dis_name","connection_object",
"ECPGExecute","@5","execstring","ECPGFree","ECPGOpen","opt_using","variablelist",
"ECPGPrepare","ECPGRelease","ECPGSetConnection","ECPGTypedef","opt_type_array_bounds",
"nest_type_array_bounds","opt_reference","ctype","@6","@7","opt_signed","sql_variable_declarations",
"sql_declaration","@8","sql_variable_list","sql_variable","ECPGVar","ECPGWhenever",
"action","ecpg_expr","into_list","ecpgstart","dotext","vartext","coutputvariable",
"cinputvariable","civariableonly","cvariable","indicator","ident","symbol","cpp_line",
"c_line","c_thing","c_anything","do_anything","var_anything","blockstart","blockend", NULL
};
#endif

static const short yyr1[] = {     0,
   303,   304,   304,   305,   305,   305,   305,   305,   305,   305,
   306,   307,   307,   307,   307,   307,   307,   307,   307,   307,
   307,   307,   307,   307,   307,   307,   307,   307,   307,   307,
   307,   307,   307,   307,   307,   307,   307,   307,   307,   307,
   307,   307,   307,   307,   307,   307,   307,   307,   307,   307,
   307,   307,   307,   307,   307,   307,   307,   307,   307,   307,
   307,   307,   307,   307,   307,   307,   307,   307,   308,   309,
   310,   311,   311,   312,   312,   312,   313,   313,   313,   314,
   314,   315,   315,   316,   316,   317,   317,   317,   317,   317,
   317,   318,   318,   319,   319,   319,   320,   320,   320,   321,
   321,   321,   322,   323,   323,   323,   323,   323,   323,   324,
   325,   326,   326,   327,   327,   327,   328,   328,   329,   329,
   330,   330,   331,   332,   332,   333,   333,   333,   334,   334,
   335,   335,   336,   336,   337,   337,   338,   338,   339,   339,
   340,   340,   340,   340,   340,   340,   340,   341,   341,   342,
   342,   342,   342,   342,   342,   342,   342,   342,   342,   342,
   342,   342,   342,   342,   342,   342,   342,   342,   342,   342,
   342,   342,   342,   342,   342,   343,   343,   344,   344,   344,
   344,   345,   345,   346,   346,   346,   346,   346,   346,   346,
   346,   346,   346,   346,   346,   346,   346,   346,   346,   346,
   346,   346,   346,   346,   346,   346,   346,   346,   346,   346,
   346,   346,   346,   346,   346,   346,   346,   346,   346,   346,
   346,   347,   347,   348,   349,   349,   349,   350,   350,   350,
   351,   351,   352,   352,   352,   352,   353,   353,   354,   355,
   355,   356,   356,   357,   358,   359,   359,   360,   360,   360,
   360,   360,   360,   361,   361,   362,   362,   363,   363,   364,
   365,   365,   366,   367,   368,   368,   369,   369,   369,   370,
   370,   370,   371,   372,   372,   373,   373,   374,   374,   374,
   375,   375,   375,   375,   376,   377,   378,   379,   379,   379,
   380,   380,   380,   380,   380,   381,   382,   382,   383,   383,
   383,   384,   384,   384,   384,   384,   385,   385,   386,   386,
   387,   387,   387,   387,   387,   388,   388,   388,   388,   388,
   388,   389,   389,   389,   390,   391,   391,   391,   392,   392,
   393,   393,   393,   393,   393,   394,   394,   394,   395,   395,
   396,   397,   398,   398,   399,   399,   400,   400,   401,   401,
   402,   403,   404,   404,   404,   405,   405,   405,   406,   407,
   408,   409,   409,   410,   410,   411,   411,   412,   413,   413,
   414,   415,   415,   415,   415,   416,   417,   417,   418,   419,
   420,   420,   421,   421,   421,   421,   421,   421,   421,   422,
   422,   422,   422,   423,   424,   424,   425,   425,   427,   426,
   428,   428,   428,   428,   428,   429,   429,   430,   430,   430,
   431,   431,   431,   431,   432,   432,   433,   433,   433,   433,
   434,   434,   435,   436,   437,   437,   438,   438,   438,   438,
   438,   439,   439,   439,   440,   441,   442,   442,   443,   443,
   444,   444,   445,   445,   445,   446,   446,   446,   447,   448,
   449,   449,   450,   450,   451,   451,   452,   452,   453,   453,
   454,   455,   455,   455,   455,   455,   455,   456,   457,   457,
   457,   457,   457,   458,   458,   459,   459,   460,   461,   462,
   462,   462,   462,   463,   463,   464,   465,   466,   466,   466,
   466,   466,   467,   467,   468,   468,   469,   470,   471,   471,
   471,   471,   471,   472,   473,   473,   473,   474,   474,   475,
   475,   476,   476,   476,   476,   477,   477,   478,   478,   479,
   480,   480,   480,   480,   480,   480,   481,   481,   481,   481,
   481,   481,   482,   482,   482,   483,   483,   484,   484,   485,
   486,   486,   487,   487,   488,   488,   489,   489,   489,   490,
   490,   490,   491,   491,   491,   492,   492,   492,   493,   493,
   493,   493,   493,   493,   493,   493,   494,   494,   495,   495,
   495,   496,   496,   497,   497,   497,   498,   498,   499,   499,
   500,   500,   500,   501,   501,   501,   502,   502,   502,   503,
   503,   503,   504,   505,   505,   505,   505,   505,   505,   505,
   505,   505,   505,   505,   505,   505,   505,   505,   505,   505,
   505,   505,   505,   505,   505,   505,   505,   505,   505,   505,
   505,   505,   505,   505,   505,   505,   506,   506,   506,   506,
   507,   507,   507,   507,   508,   508,   509,   509,   509,   510,
   510,   510,   511,   511,   512,   512,   512,   512,   512,   513,
   513,   514,   514,   515,   515,   516,   516,   516,   516,   517,
   517,   517,   517,   517,   517,   518,   518,   519,   519,   519,
   519,   519,   519,   519,   519,   519,   520,   520,   521,   521,
   521,   521,   521,   522,   523,   523,   523,   523,   523,   523,
   523,   523,   524,   524,   525,   525,   526,   526,   526,   526,
   526,   526,   526,   526,   526,   526,   526,   526,   526,   526,
   526,   526,   526,   526,   526,   526,   526,   526,   526,   526,
   526,   526,   526,   526,   526,   526,   526,   526,   526,   526,
   526,   526,   526,   526,   526,   526,   526,   526,   526,   526,
   526,   526,   526,   526,   526,   526,   526,   526,   526,   526,
   526,   526,   526,   526,   526,   526,   526,   526,   526,   526,
   526,   526,   526,   526,   526,   526,   526,   526,   526,   526,
   526,   526,   526,   526,   526,   526,   526,   527,   527,   527,
   527,   527,   527,   527,   527,   527,   527,   527,   527,   527,
   527,   527,   527,   527,   527,   527,   527,   527,   527,   527,
   527,   527,   527,   527,   527,   527,   527,   527,   527,   528,
   528,   528,   529,   529,   529,   530,   530,   530,   531,   531,
   531,   532,   532,   533,   533,   533,   533,   533,   533,   533,
   533,   533,   533,   533,   533,   533,   533,   533,   533,   533,
   533,   533,   533,   533,   533,   533,   534,   534,   535,   535,
   536,   536,   537,   537,   537,   538,   538,   539,   539,   540,
   540,   541,   541,   542,   542,   542,   543,   543,   544,   545,
   545,   546,   546,   546,   547,   547,   548,   548,   548,   549,
   549,   549,   550,   550,   550,   551,   551,   552,   552,   552,
   552,   553,   553,   554,   554,   555,   556,   557,   558,   559,
   560,   561,   562,   563,   564,   564,   564,   564,   564,   564,
   564,   565,   566,   567,   568,   569,   570,   570,   570,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   571,   571,   571,   571,   571,   571,   571,   571,   571,
   571,   572,   572,   572,   572,   572,   572,   572,   572,   572,
   572,   572,   572,   572,   572,   572,   572,   572,   572,   572,
   572,   572,   572,   572,   572,   572,   572,   572,   572,   572,
   572,   572,   572,   572,   572,   572,   572,   572,   572,   572,
   573,   573,   574,   574,   574,   575,   575,   575,   575,   576,
   577,   578,   578,   579,   579,   580,   580,   581,   581,   582,
   582,   583,   583,   583,   583,   584,   584,   584,   585,   586,
   586,   587,   588,   590,   589,   591,   592,   593,   593,   595,
   596,   594,   597,   597,   597,   597,   597,   597,   597,   598,
   598,   598,   598,   598,   598,   599,   600,   601,   602,   603,
   604,   605,   605,   606,   606,   606,   606,   606,   606,   606,
   606,   606,   606,   606,   607,   608,   608,   609,   610,   610,
   611,   611,   612,   613,   614,   614,   614,   614,   615,   615,
   616,   617,   616,   618,   618,   619,   620,   621,   621,   622,
   622,   623,   624,   625,   626,   627,   627,   627,   627,   627,
   628,   628,   628,   628,   628,   629,   629,   630,   630,   630,
   630,   630,   630,   630,   630,   630,   630,   630,   630,   631,
   630,   632,   630,   630,   633,   633,   634,   634,   636,   635,
   637,   637,   638,   639,   640,   640,   640,   641,   641,   641,
   641,   641,   641,   641,   641,   642,   642,   642,   642,   642,
   642,   642,   642,   642,   642,   642,   642,   642,   642,   642,
   642,   642,   642,   642,   642,   642,   642,   642,   642,   642,
   642,   642,   642,   642,   642,   642,   642,   642,   642,   642,
   642,   642,   642,   642,   642,   642,   642,   642,   642,   642,
   642,   642,   642,   642,   642,   642,   642,   642,   642,   642,
   642,   642,   642,   642,   642,   642,   642,   642,   642,   642,
   642,   642,   642,   642,   642,   642,   642,   642,   642,   642,
   642,   642,   642,   642,   643,   643,   644,   645,   645,   646,
   646,   647,   648,   649,   650,   651,   651,   651,   651,   652,
   652,   653,   654,   655,   655,   656,   656,   657,   657,   657,
   657,   657,   657,   657,   657,   657,   657,   657,   657,   657,
   657,   657,   657,   657,   657,   657,   657,   657,   657,   657,
   657,   657,   657,   657,   657,   657,   657,   658,   658,   658,
   658,   658,   659,   659,   659,   659,   659,   660,   661
};

static const short yyr2[] = {     0,
     1,     0,     2,     4,     3,     1,     1,     1,     1,     1,
     2,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     8,     8,
     3,     3,     0,     1,     1,     0,     1,     1,     0,     3,
     1,     3,     0,     3,     0,     4,     4,     4,     6,     5,
     3,     1,     1,     1,     1,     1,     2,     3,     4,     2,
     3,     4,     5,     3,     4,     3,     6,     5,     2,     2,
     7,     1,     1,     1,     1,     1,     1,     0,     2,     0,
     3,     0,     8,     1,     0,     3,     1,     0,     1,     1,
     3,     3,     1,     0,     2,     1,     2,     0,     3,     1,
     4,     2,     2,     2,     1,     2,     5,     3,     1,     1,
     2,     3,     3,     3,     3,     3,     3,     3,     2,     2,
     3,     6,     3,     3,     4,     3,     2,     2,     1,     1,
     4,     1,     4,     1,     1,     3,     1,     4,     4,     5,
    10,     3,     1,     1,     1,     1,     2,     3,     3,     3,
     3,     3,     3,     3,     2,     2,     3,     6,     3,     3,
     4,     3,     3,     4,     3,     3,     2,     2,     2,     2,
     3,     2,     4,     3,     3,     4,     4,     5,     6,     5,
     6,     3,     1,     1,     2,     2,     0,     2,     1,     0,
     3,     3,     2,     1,     2,     2,     4,     0,     7,     3,
     0,     3,     1,     1,     4,     2,     0,     2,     1,     2,
     2,     2,     2,     1,     1,     1,     2,     1,     2,     9,
     1,     0,     4,    14,     1,     1,     1,     3,     5,     1,
     1,     1,     3,     1,     0,     1,     1,     1,     3,     0,
     1,     1,     1,     1,     5,     3,     2,     1,     1,     1,
     1,     1,     1,     1,     1,     3,     1,     3,     3,     1,
     3,     1,     1,     1,     1,     2,     3,     3,     6,     4,
     1,     1,     1,     1,     0,     1,     2,     1,     1,     1,
     0,     2,     2,     0,     7,     2,     1,     1,     1,     3,
     1,     1,     1,     1,     1,     1,     2,     1,     3,     0,
     6,    11,     1,     0,     2,     0,     1,     1,     3,     1,
     6,     3,     2,     2,     0,     1,     2,     0,     4,     3,
    11,     2,     0,     3,     2,     1,     3,     2,     1,     0,
     3,     1,     1,     1,     1,     4,     1,     1,     4,     6,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     3,     3,     3,     9,     1,     0,     1,     0,     0,    13,
     1,     1,     1,     3,     3,     1,     1,     2,     3,     2,
     1,     1,     1,     1,     3,     1,     1,     1,     1,     1,
     1,     0,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     1,     1,     0,     5,     2,     6,     3,     3,     0,
     3,     0,     1,     1,     0,     1,     1,     0,     3,     4,
     3,     5,     1,     0,     1,     0,     3,     0,     1,     3,
     3,     1,     1,     1,     1,     1,     1,     4,     4,     2,
     1,     7,     4,     3,     0,     3,     1,     2,     4,     3,
     8,     7,     6,     1,     0,     6,     7,     1,     1,     1,
     2,     0,     2,     0,     2,     2,     2,     4,     3,     1,
     3,     4,     4,     8,     4,     2,     0,     1,     0,     1,
     0,     1,     3,     1,     0,     3,     0,     1,     3,     2,
     2,     2,     2,     1,     1,     0,     4,     4,     2,     4,
     2,     0,     1,     1,     1,     1,     1,     1,     0,     1,
     1,     3,     3,     0,     2,     0,     2,     4,     0,     8,
     2,     0,     3,     4,     1,     3,     2,     1,     2,     2,
     2,     2,     1,     1,     1,     0,     1,     0,     4,     4,
     0,     1,     3,     1,     3,     1,     2,     0,     1,     2,
     3,     4,     0,     3,     4,     0,     2,     1,     2,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     2,     2,     2,     2,
     1,     2,     1,     1,     3,     0,     5,     3,     0,     5,
     3,     0,     4,     1,     4,     2,     1,     3,     2,     1,
     0,     3,     0,     2,     0,     1,     2,     1,     2,     1,
     1,     1,     1,     1,     1,     3,     0,     1,     3,     3,
     3,     3,     3,     3,     3,     0,     1,     1,     7,     8,
     8,     7,     7,     3,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     3,     1,     2,     1,     1,     1,
     2,     3,     3,     3,     3,     3,     3,     3,     2,     2,
     3,     6,     3,     3,     3,     4,     2,     2,     4,     3,
     4,     1,     1,     4,     1,     4,     1,     1,     4,     4,
     4,     4,     5,     5,     5,     4,     2,     3,     2,     4,
     3,     4,     3,     4,     5,     6,     5,     6,     5,     5,
     5,     5,     5,     5,     5,     5,     6,     6,     6,     6,
     6,     6,     6,     6,     6,     6,     6,     6,     6,     6,
     6,     6,     3,     3,     2,     1,     1,     2,     1,     1,
     2,     3,     3,     3,     3,     2,     2,     3,     6,     3,
     3,     2,     2,     3,     4,     1,     1,     4,     1,     4,
     1,     1,     4,     4,     5,     5,     5,     4,     1,     4,
     6,     0,     1,     3,     3,     3,     0,     1,     1,     1,
     1,     3,     0,     2,     1,     2,     3,     3,     3,     3,
     2,     3,     6,     3,     3,     2,     2,     1,     3,     4,
     4,     4,     5,     5,     5,     4,     3,     0,     2,     0,
     2,     0,     3,     2,     1,     1,     1,     1,     3,     1,
     1,     1,     3,     5,     6,     4,     2,     1,     4,     2,
     0,     2,     1,     0,     3,     3,     1,     3,     3,     3,
     1,     1,     4,     2,     3,     3,     1,     3,     1,     3,
     1,     1,     0,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     2,     1,     1,
     1,     2,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     5,     3,     2,     3,     6,     1,     1,     2,
     2,     1,     0,     1,     3,     2,     0,     2,     0,     2,
     0,     1,     3,     4,     3,     1,     1,     1,     1,     2,
     0,     7,     3,     0,     4,     5,     5,     0,     2,     0,
     0,     6,     1,     1,     1,     1,     1,     1,     0,     1,
     1,     1,     1,     1,     1,     4,     2,     4,     4,     2,
     2,     0,     1,     1,     2,     1,     2,     1,     2,     1,
     1,     1,     1,     2,     1,     1,     3,     4,     0,     2,
     0,     1,     3,     2,     1,     1,     1,     0,     1,     1,
     3,     0,     4,     1,     1,     2,     3,     0,     2,     1,
     3,     4,     2,     3,     6,     3,     3,     4,     4,     0,
     3,     3,     4,     4,     0,     1,     0,     1,     1,     1,
     1,     2,     1,     2,     2,     1,     2,     2,     2,     0,
     5,     0,     5,     1,     1,     0,     0,     2,     0,     4,
     1,     3,     3,     6,     3,     4,     3,     1,     1,     1,
     2,     3,     5,     2,     5,     2,     1,     1,     1,     2,
     3,     3,     3,     3,     3,     3,     3,     2,     2,     3,
     6,     3,     3,     3,     4,     2,     2,     4,     3,     4,
     1,     1,     4,     1,     4,     1,     4,     4,     4,     4,
     5,     5,     5,     4,     2,     3,     2,     4,     3,     4,
     3,     4,     5,     6,     5,     6,     5,     5,     5,     5,
     5,     5,     5,     5,     6,     6,     6,     6,     6,     6,
     6,     6,     6,     6,     6,     6,     6,     6,     6,     6,
     3,     3,     2,     1,     1,     3,     1,     0,     2,     1,
     2,     2,     2,     1,     1,     0,     1,     2,     2,     1,
     1,     1,     1,     1,     2,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     3,     1,     1
};

static const short yydefact[] = {     2,
     1,  1297,  1341,  1323,  1324,  1325,  1326,  1327,  1328,  1329,
  1330,  1331,  1332,  1333,  1334,  1335,  1336,  1337,  1338,  1339,
  1340,  1318,  1319,  1313,   913,   914,  1346,  1322,  1317,  1342,
  1343,  1347,  1344,  1345,  1358,  1359,     3,  1320,  1321,     6,
  1094,     0,     8,     7,  1316,     9,    10,  1109,     0,     0,
     0,  1148,     0,     0,     0,     0,     0,     0,   434,   893,
   434,   125,     0,     0,     0,   434,     0,   315,     0,     0,
     0,   434,   515,     0,     0,     0,   434,     0,   118,   454,
     0,     0,     0,     0,   509,   315,     0,     0,     0,   454,
     0,     0,     0,    21,    13,    27,    52,    53,    54,    12,
    14,    15,    16,    17,    18,    19,    25,    20,    26,    23,
    24,    30,    31,    43,    32,    28,    37,    36,    41,    38,
    40,    39,    42,    45,   466,    33,    34,    46,    47,    48,
    49,    50,    22,    51,    29,    44,   465,   467,    35,   464,
   463,   462,   517,   500,    55,    56,    57,    58,    59,    60,
    61,    62,    63,    64,    65,    66,    67,    68,  1108,  1106,
  1103,  1107,  1105,  1104,     0,  1109,  1100,   991,   992,   993,
   994,   995,   996,   997,   998,   999,  1000,  1001,  1002,  1003,
  1004,  1005,  1006,  1007,  1008,  1009,  1010,  1011,  1012,  1013,
  1014,  1015,  1016,  1017,  1018,  1019,  1020,  1021,   922,   923,
   662,   934,   663,   943,   946,   947,   950,   664,   661,   953,
   958,   960,   962,   964,   965,   967,   969,   974,   665,   981,
   982,   983,   984,   660,   990,   985,   987,   924,   925,   926,
   927,   928,   929,   930,   931,   932,   933,   935,   936,   937,
   938,   939,   940,   941,   942,   944,   945,   948,   949,   951,
   952,   954,   955,   956,   957,   959,   961,   963,   966,   968,
   970,   971,   972,   973,   976,   975,   977,   978,   979,   980,
   986,   988,   989,  1310,   915,  1311,  1305,   921,  1073,  1069,
   896,    11,     0,  1068,  1089,   920,     0,  1088,  1086,  1065,
  1082,  1087,   916,     0,  1147,  1146,  1150,  1149,  1144,  1145,
  1156,  1158,   901,   920,     0,  1312,     0,     0,     0,     0,
     0,     0,     0,   433,   432,   428,   110,   892,   429,   124,
   343,     0,     0,     0,   289,   290,     0,     0,   288,     0,
     0,   261,     0,     0,     0,     0,   978,   492,     0,     0,
     0,   375,     0,   372,     0,     0,     0,   373,     0,     0,
   374,     0,     0,   430,     0,     0,  1152,   314,   313,   312,
   311,   321,   327,   334,   332,   331,   333,   335,     0,   328,
   329,     0,     0,   431,   514,   512,     0,   996,   448,   981,
     0,     0,  1061,  1062,     0,   895,   894,     0,   427,     0,
   900,   117,     0,   453,     0,     0,   424,   426,   425,   436,
   903,   508,     0,   321,   423,   981,     0,   100,   981,     0,
    97,   456,     0,   434,     0,     5,  1163,     0,   511,     0,
   511,   549,  1095,     0,  1099,     0,     0,  1072,  1077,  1077,
  1070,  1064,  1079,     0,     0,     0,  1093,     0,  1157,     0,
  1196,     0,  1208,     0,     0,  1209,  1210,     0,  1205,  1207,
     0,   539,    73,     0,    73,     0,     0,   438,     0,   902,
     0,   247,     0,     0,   292,   291,   295,   389,   387,   388,
   383,   384,   385,   386,   286,     0,   294,   293,     0,  1143,
   489,   490,   488,     0,   578,   307,   540,   541,    71,     0,
     0,   449,     0,   381,     0,   382,     0,   308,   371,  1155,
  1154,  1151,   360,   904,  1158,   318,   319,   320,     0,   324,
   316,   326,     0,     0,     0,     0,     0,   991,   992,   993,
   994,   995,   996,   997,   998,   999,  1000,  1001,  1002,  1003,
  1004,  1005,  1006,  1007,  1008,  1009,  1010,  1011,  1012,  1013,
  1014,  1015,  1016,  1017,  1018,  1019,  1020,  1021,   874,     0,
   651,   651,     0,   722,   723,   725,   727,   642,   934,     0,
     0,   911,   636,   676,     0,   651,     0,     0,   678,   639,
     0,     0,   981,   982,     0,   910,   728,   647,   987,     0,
     0,   812,     0,   891,     0,     0,     0,     0,   583,   590,
   593,   592,   588,   644,   591,   921,   889,   698,   677,   776,
   812,   507,   887,     0,     0,   699,   909,   905,   906,   907,
   700,   777,  1306,   920,  1164,   447,    91,   446,     0,     0,
     0,     0,     0,  1196,     0,   120,     0,   461,   578,   480,
   324,   101,     0,    98,     0,   455,   451,   499,     4,   501,
   510,     0,     0,     0,     0,   532,     0,  1132,  1133,  1131,
  1122,  1130,  1126,  1128,  1124,  1122,  1122,     0,  1135,  1101,
  1114,     0,  1112,  1113,     0,     0,  1110,  1111,  1115,  1074,
  1071,     0,  1066,     0,     0,  1081,     0,  1085,  1083,  1159,
  1160,  1162,  1186,  1183,  1195,  1190,     0,  1178,  1181,  1180,
  1192,  1179,  1170,     0,  1194,     0,     0,  1211,   993,     0,
  1206,   538,     0,     0,    76,  1096,    76,     0,   266,   265,
     0,   440,     0,     0,   399,   245,   241,     0,     0,   287,
     0,   491,     0,     0,   479,     0,     0,   378,   376,   377,
   379,     0,   263,  1153,   317,     0,     0,     0,     0,   330,
     0,     0,     0,   468,   471,     0,   513,     0,   812,     0,
     0,   873,     0,   650,   646,   653,     0,     0,     0,     0,
   629,   628,     0,   817,     0,   627,   662,   663,   664,   660,
   668,   659,   651,   649,   775,     0,     0,   630,   823,   848,
     0,   657,     0,   596,   597,   598,   599,   600,   601,   602,
   603,   604,   605,   606,   607,   608,   609,   610,   611,   612,
   613,   614,   615,   616,   617,   618,   619,   620,   621,   622,
   623,   624,   625,   626,     0,   658,   667,   595,   589,   656,
   594,   717,     0,   912,   701,   710,   709,     0,     0,     0,
   677,   908,     0,   587,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   737,   739,   718,     0,     0,     0,
     0,     0,     0,     0,   697,   125,     0,   552,     0,     0,
     0,     0,  1307,  1303,    95,    96,    88,    94,     0,    93,
    86,    92,    87,   882,   812,   552,   881,     0,   812,  1170,
   450,     0,     0,   492,   359,   485,   310,   102,    99,   458,
   503,   516,   518,   526,   502,   547,     0,     0,   498,     0,
  1117,  1123,  1120,  1121,  1134,  1127,  1129,  1125,  1141,     0,
  1109,  1109,     0,  1076,     0,  1078,     0,  1063,  1084,     0,
     0,  1187,  1189,  1188,     0,     0,     0,  1177,  1182,  1185,
  1184,  1298,  1212,  1298,   398,   398,   398,   398,   103,     0,
    74,    75,    79,    79,   435,   271,   270,   272,     0,   267,
     0,   442,   633,   934,   631,   634,   365,     0,   918,   919,
   366,   917,   370,     0,     0,   249,     0,     0,     0,     0,
   246,   128,     0,     0,     0,   300,     0,   297,     0,     0,
   577,   542,   285,     0,     0,   390,   323,   322,     0,     0,
   470,     0,     0,   477,   812,     0,     0,   871,   868,   872,
     0,     0,     0,   655,   813,     0,     0,     0,     0,     0,
   820,   821,   819,     0,     0,   818,     0,     0,     0,     0,
     0,   648,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   812,     0,   825,   838,   850,     0,
     0,     0,     0,     0,     0,   677,   855,     0,     0,   722,
   723,   725,   727,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   698,     0,   812,     0,   699,   700,
     0,  1294,  1306,   713,     0,     0,   586,     0,     0,  1026,
  1028,  1029,  1031,  1032,  1034,  1035,  1038,  1039,  1040,  1047,
  1048,  1049,  1050,  1054,  1055,  1056,  1057,  1060,  1023,  1024,
  1025,  1027,  1030,  1033,  1036,  1037,  1041,  1042,  1043,  1044,
  1045,  1046,  1051,  1052,  1053,  1058,  1059,  1022,   888,   711,
   773,     0,   796,   797,   799,   801,     0,     0,     0,   802,
     0,     0,     0,     0,     0,     0,   812,     0,   779,   780,
   809,  1304,     0,   743,     0,   738,   741,   715,     0,     0,
     0,   774,     0,     0,     0,   714,     0,     0,     0,   708,
     0,     0,     0,   706,     0,     0,     0,   707,     0,     0,
     0,   702,     0,     0,     0,   703,     0,     0,     0,   705,
     0,     0,     0,   704,   509,   506,  1295,  1306,   886,     0,
   578,   890,   875,   877,   898,     0,   720,     0,   876,  1309,
  1308,   967,    90,   884,     0,   578,     0,     0,  1177,   119,
   113,   112,     0,     0,   484,     0,     0,   452,     0,   524,
   525,     0,   520,     0,   534,   535,   529,   533,   537,   531,
   536,     0,  1142,     0,  1136,     0,     0,  1314,     0,     0,
  1075,  1091,  1080,  1161,  1196,  1196,  1175,     0,  1175,     0,
  1176,  1204,     0,     0,     0,   397,     0,     0,     0,   128,
   109,     0,     0,     0,   396,    72,    77,    78,    83,    83,
     0,     0,   445,     0,   437,   632,     0,   364,   369,   363,
     0,     0,     0,   248,   258,   250,   251,   252,   253,     0,
     0,   127,   129,   130,   177,     0,   243,   244,     0,     0,
     0,     0,     0,   296,   346,   494,   494,     0,   380,     0,
   309,     0,   336,   340,   338,     0,     0,     0,   478,   341,
     0,     0,   867,     0,     0,     0,     0,   645,     0,     0,
   866,   724,   726,     0,   641,   729,   730,     0,   635,   670,
   671,   672,   673,   675,   674,   669,     0,     0,   638,     0,
   823,   848,     0,   836,   826,   831,     0,   731,     0,     0,
   837,     0,     0,     0,     0,   824,     0,     0,   852,   732,
   666,     0,   854,     0,     0,     0,   736,     0,     0,     0,
     0,   817,   775,  1293,   823,   848,     0,   717,  1236,   701,
  1220,   710,  1229,   709,  1228,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   737,   739,   718,     0,     0,     0,
     0,     0,     0,     0,   697,     0,     0,   812,     0,     0,
   685,   687,   686,   688,   689,   690,   691,   692,     0,   684,
     0,   581,   586,   643,     0,     0,     0,   823,   848,     0,
   792,   781,   787,   786,     0,     0,     0,   793,     0,     0,
     0,     0,   778,     0,   856,     0,   857,   858,   909,   742,
   740,   744,     0,     0,   716,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1302,     0,   551,   555,   558,   579,   544,     0,   719,
   721,    89,   880,   486,   885,     0,  1165,   115,   116,   122,
   114,     0,   483,     0,     0,   459,   519,   521,   522,   523,
   548,     0,     0,     0,  1097,  1102,  1141,   583,  1116,  1315,
  1118,  1119,     0,  1067,  1199,     0,  1196,     0,     0,     0,
  1166,  1175,  1167,  1175,  1348,  1349,  1352,  1215,  1350,  1351,
  1299,  1213,     0,     0,     0,     0,     0,     0,   104,     0,
   106,     0,   395,     0,    85,    85,     0,   268,   444,   439,
   443,   448,   367,     0,     0,   368,   419,   420,   417,   418,
     0,   259,     0,     0,   238,     0,   240,   138,   134,   239,
     0,     0,   384,   304,   254,   255,   301,   303,   256,   305,
   302,   299,   298,     0,     0,     0,   487,  1092,   392,   393,
   391,   337,     0,   325,   469,   476,     0,   473,     0,   870,
   864,     0,   652,   654,   815,   814,     0,   816,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   834,   832,   822,
   835,   827,   828,   830,   829,   839,     0,   849,     0,   847,
   733,   734,   735,   853,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   713,   711,   773,  1291,     0,
     0,   743,     0,   738,   741,   715,  1234,     0,     0,     0,
   774,  1292,     0,     0,     0,   714,  1233,     0,     0,     0,
   708,  1227,     0,     0,     0,   706,  1225,     0,     0,     0,
   707,  1226,     0,     0,     0,   702,  1221,     0,     0,     0,
   703,  1222,     0,     0,     0,   705,  1224,     0,     0,     0,
   704,  1223,     0,   720,     0,     0,   810,     0,     0,   694,
   693,     0,     0,   586,     0,   582,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   790,   788,   745,   791,   782,
   783,   785,   784,   794,     0,   747,     0,     0,   860,     0,
   861,   862,     0,     0,   749,     0,     0,   756,     0,     0,
   754,     0,     0,   755,     0,     0,   750,     0,     0,   751,
     0,     0,   753,     0,     0,   752,   505,  1296,   566,     0,
     0,     0,   557,   580,     0,   546,   879,   878,   883,     0,
   111,     0,   482,     0,     0,   457,   528,   527,   530,  1137,
  1139,  1090,  1141,  1191,  1198,  1193,  1175,     0,  1175,     0,
  1168,  1169,     0,     0,   185,     0,     0,     0,     0,     0,
     0,     0,   184,   186,     0,     0,     0,   105,     0,     0,
     0,     0,     0,    70,    69,   275,     0,     0,   441,   362,
     0,     0,   176,   126,     0,   123,   242,   244,     0,   132,
     0,     0,     0,     0,     0,     0,   145,   131,   133,   136,
   140,     0,   306,   257,   345,   897,     0,     0,     0,   493,
     0,     0,   869,   712,   640,   865,   637,     0,   841,   842,
     0,     0,     0,   846,   840,   851,     0,   724,   726,   729,
   730,   731,   732,     0,     0,     0,   736,     0,     0,   742,
   740,   744,     0,     0,   716,  1235,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   719,   721,   812,     0,     0,     0,     0,   696,     0,   584,
   586,     0,   798,   800,   803,   804,     0,     0,     0,   808,
   795,   859,   746,   748,     0,   765,   757,   772,   764,   770,
   762,   771,   763,   766,   758,   767,   759,   769,   761,   768,
   760,   568,   564,   568,   566,   563,   568,   565,     0,   553,
     0,   556,     0,     0,   504,     0,   481,   460,     0,  1138,
     0,     0,  1201,  1171,  1175,  1172,  1175,     0,   207,   208,
   187,   196,   195,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   210,   212,   209,     0,     0,     0,     0,     0,
     0,     0,   178,     0,     0,     0,   179,   108,     0,   394,
    82,    81,     0,   274,     0,     0,   269,     0,   578,   416,
     0,   137,     0,     0,     0,   169,   170,   172,   174,   142,
   175,     0,     0,     0,     0,     0,   143,     0,   150,   144,
   146,   475,   135,   260,     0,   347,   348,   350,   355,     0,
   898,   495,     0,   496,   339,     0,     0,   843,   844,   845,
     0,   733,   734,   735,   745,   747,     0,     0,     0,     0,
   749,     0,     0,   756,     0,     0,   754,     0,     0,   755,
     0,     0,   750,     0,     0,   751,     0,     0,   753,     0,
     0,   752,   811,   679,     0,   682,   683,     0,   585,     0,
   805,   806,   807,   863,   567,   560,   561,   559,   562,     0,
   554,   543,   545,   121,  1353,  1354,     0,  1355,  1356,  1140,
  1300,   583,  1200,  1141,  1173,  1174,     0,   199,   197,   205,
     0,   224,     0,   215,     0,   211,   214,   203,     0,     0,
     0,   206,   202,   192,   193,   194,   188,   189,   191,   190,
   200,     0,   183,     0,   180,   107,     0,    84,   276,   277,
   273,     0,     0,     0,     0,     0,     0,   139,     0,     0,
     0,   167,   151,   160,   159,     0,     0,   168,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   227,   363,     0,
     0,     0,   358,     0,   497,   472,   833,   712,   746,   748,
   765,   757,   772,   764,   770,   762,   771,   763,   766,   758,
   767,   759,   769,   761,   768,   760,   680,   681,   789,   571,
     0,  1301,  1203,  1202,     0,     0,     0,   223,   217,   213,
   216,     0,     0,   204,     0,   201,     0,    80,     0,   361,
   422,   415,   237,   141,     0,     0,     0,   163,   161,   166,
   156,   157,   158,   152,   153,   155,   154,   164,     0,   149,
     0,     0,   230,   342,   349,   354,   353,     0,   352,   356,
   899,     0,     0,     0,     0,  1357,     0,   220,     0,   218,
     0,     0,   182,   475,   280,   421,     0,     0,   171,   173,
     0,   165,   474,   225,   226,     0,   147,   229,   357,   355,
     0,     0,   550,   198,   222,   221,   219,   227,     0,   278,
   281,   282,   283,   284,   401,     0,     0,   400,   403,   414,
   411,   413,   412,   402,     0,   148,     0,     0,   228,   358,
     0,     0,   572,   576,   574,   230,     0,   264,     0,   406,
   407,     0,   162,   234,     0,     0,   231,   232,   351,   569,
     0,   570,     0,   181,   279,   404,   408,   410,   405,   233,
   235,   236,   573,   575,   409,     0,     0,     0
};

static const short yydefgoto[] = {  2396,
     1,    37,    92,    93,    94,    95,    96,   705,   943,  1269,
  2041,  1565,  1844,    97,   871,   867,    98,    99,   100,   939,
   101,   102,  1213,  1510,   393,   883,  1801,   103,   333,  1291,
  1292,  1293,  1868,  1869,  1860,  1870,  1871,  2289,  2067,  1294,
  1295,  2182,  1831,  2257,  2258,  2293,  2327,  2328,  2377,  1856,
   104,   973,  1296,  1297,   105,   716,   971,  1594,  1595,  1596,
   106,   334,   107,   108,   711,   949,   950,  1847,  2045,  2191,
  2339,  2340,   109,   110,   475,   335,   976,   720,   977,   978,
  1597,   111,   112,   362,   510,   738,   113,   369,   370,   371,
  1314,  1614,   114,   115,   336,  1605,  2075,  2076,  2077,  2078,
  2223,  2299,   116,   117,   118,  1575,   714,   958,  1280,  1281,
   119,   353,   120,   729,   121,   122,  1598,   477,   985,   123,
  1562,  1262,   124,   964,  2348,  2369,  2370,  2371,  2049,  1581,
  2317,  2350,   126,   127,   128,   316,   129,   130,   131,   952,
  1275,  1570,   617,   132,   133,   134,   395,   637,  1218,  1515,
   135,   136,  2351,   744,  2218,   993,   994,  2352,   139,  1216,
  2353,   141,   484,  1607,  1880,  2084,   142,   143,   144,   858,
   403,   642,   377,   422,   892,   893,  1223,   899,  1227,  1230,
   703,   486,   487,  1796,  1995,   646,  1191,  1494,  1495,  1989,
  2136,  2305,  2362,  2363,   725,  1496,   834,  1432,   588,   589,
   590,   591,   592,   959,   766,   778,   761,   593,   594,   755,
  1004,  1328,   595,   596,   782,   772,  1005,   598,   829,  1429,
  1733,   830,   599,  1136,   824,  1047,  1014,  1015,  1033,  1034,
  1040,  1369,  1650,  1048,  1456,  1457,  1760,  1761,   600,   998,
   999,  1324,   748,   601,  1193,   876,   877,   602,   603,   317,
   750,   279,  1875,  1194,  2300,   390,   488,   605,   400,   503,
   606,   607,   608,   609,   610,   289,   961,   611,  1119,   387,
   145,   298,   283,   428,   429,   671,   673,   676,   918,   290,
   291,   284,  1534,   146,   147,    40,    48,    41,   423,   165,
   166,   426,   909,   167,   660,   661,   662,   663,   664,   665,
   666,   901,   667,   668,  1234,  1235,  2000,  1236,   148,   149,
   299,   300,   150,   505,   502,   151,   152,   439,   680,   153,
   154,   155,   156,   928,  1541,  1252,  1535,   921,   925,   694,
  1536,  1537,  1813,  2002,  2003,   157,   158,   449,  1071,  1186,
    42,  1253,  2150,  1187,   612,  1072,   613,   864,   614,   695,
    43,  1237,    44,  1238,  1551,  2151,    46,    47
};

static const short yypact[] = {-32768,
  4116,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,  5541,-32768,-32768,-32768,-32768,-32768,  1428, 24111,   572,
   126, 23283,   551, 27688,   551,   -97,    68,   273,    58, 27688,
   389,  2593, 27963,   125,  1865,   389,    90,    32,    20,    14,
    20,   389,   223, 25488, 25763,   -97,   389, 27688,    56,    10,
   114, 25763, 20946,    85,   225,    32, 25763, 26313, 26588,    10,
   -65,  2426,   476,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,   487,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,   527,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,   493,    33,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   262,-32768,
-32768,-32768,   262,-32768,-32768,   284, 23559,-32768,-32768,-32768,
    44,-32768,-32768,   551,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,   402,-32768,-32768,   543,-32768,   508,   169,   169,   652,
 25763,   551,   656,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,   551, 27688, 27688,-32768,-32768, 27688, 27688,-32768, 27688,
 25763,-32768,   513,   471, 20371,   537,   551,   -15, 25763, 27688,
   551,-32768, 27688,-32768, 27688, 27688, 27688,-32768,  1490,   688,
-32768, 27688, 27688,-32768,   440,   551,-32768,-32768,-32768,-32768,
-32768,   693,   621,-32768,-32768,-32768,-32768,-32768,   636,   528,
-32768, 25763,   690,-32768,-32768,   778, 10018, 23835,   -24,   751,
   881,   -79,-32768,-32768,   747,-32768,-32768,   828,-32768,   869,
-32768,-32768, 25763,-32768,   770, 27688,-32768,-32768,-32768,-32768,
-32768,-32768, 25763,   693,-32768,   838,   904,-32768,   848,   940,
-32768,   878,   349,   389,  1062,-32768,-32768,   -65,  1050,  1054,
  1050,  1019,-32768,  1053,-32768,   116, 27688,-32768,   871,   871,
-32768,-32768,  1108,  1106,  1115, 27688,-32768,   284,-32768,   284,
   594, 27688,-32768,   999, 27688,-32768,-32768, 28238,-32768,-32768,
   169,   899,   997,  1171,   997,  1147,   497,  1006,   944,-32768,
  1200,-32768, 25763,  1129,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,   979,-32768,-32768, 27688,-32768,
  1109,-32768,-32768,  1193,  1092,-32768,  1001,-32768,-32768,  1162,
 21221,-32768,   944,-32768,  1014,-32768,    85,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,   402,-32768,-32768,-32768,  1055,   387,
-32768,-32768, 27688,   367,    24, 27688, 27688,   293,   350,   358,
   372,   441,   520,   542,   549,   552,   556,   560,   591,   610,
   611,   644,   659,   669,   678,   707,   719,   730,   738,   750,
   772,   790,   808,   827,   833,   846,   862,   880, 22726,  1035,
  1149,  1149,  1087,-32768,  1100,  1107,-32768,  1114,  1191,  1116,
  1128,-32768,  1141,  1064,  1326,  1149, 15958,  1152,-32768,  1153,
  1156,  1164,   891,   227,  1174,-32768,-32768,-32768,   937,  6069,
 15958,  1118, 15958,-32768, 15958, 15958, 15067,    85,  1180,-32768,
-32768,-32768,-32768,  1181,-32768,   938,  1381,-32768,  5562,-32768,
  1118,   -36,-32768,  1188,  1206,-32768,  1195,-32768,-32768,-32768,
   -84,-32768,    36,   952,-32768,-32768,-32768,-32768,     6,  1359,
    -8,    -8, 20659,   594, 25763,  1330, 27688,-32768,  1092,  1389,
   387,-32768,  1407,-32768,  1410,-32768, 25763,-32768,-32768,-32768,
-32768,   -65, 15958,   -65,  1374,   383,  1492,-32768,-32768,-32768,
   -97,-32768,-32768,-32768,-32768,   -97,   -97,  1242,-32768,-32768,
-32768,  1313,-32768,-32768,  1317,  1325,-32768,-32768,-32768,  1312,
-32768,  1055,-32768,  1345, 24111,  1437,  1115,-32768,-32768,-32768,
  1349,-32768,-32768,-32768,-32768,-32768,   473,-32768,-32768,-32768,
-32768,-32768,   573,  1237,-32768,  1344, 27688,-32768,  1623,  1352,
-32768,-32768,   179,  1412,    15,-32768,    15,   -65,-32768,-32768,
    12,  1445,  8258,  1408,-32768,   978,  1390,    85, 20083,-32768,
  1544,-32768,  1585, 15958,-32768, 27688, 25763,-32768,-32768,-32768,
-32768, 26863,-32768,-32768,-32768, 27688, 27688,  1574,  1515,-32768,
  1508,  1402, 19510,-32768,-32768,  1589,-32768,  1507,  1118,  1414,
  1195,  1415, 15958,-32768,-32768,  1634, 15067,  1055,  1055,  1055,
-32768,-32768,  1540,  1475,  1055,-32768,  1531,  1533,  1534,  1535,
-32768,-32768,  1149,-32768,  2631, 15958,  1055,-32768, 18037, 15067,
  1545,-32768,  8533,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,  1191,-32768,  1526,-32768,-32768,-32768,
-32768,   554, 16255,-32768,  1666,  1666,  1666,  1426,  1427,  1431,
  1988,-32768,  -135,-32768,  1055, 24663, 28990, 15958, 16552,  1433,
   575, 15958,   444, 15958,-32768,-32768, 15364, 10315, 10612, 10909,
 11206, 11503, 11800, 12097,-32768,   -58, 10018,  1617, 21496,  6470,
 27688, 24387,-32768,-32768,-32768,-32768,-32768,-32768, 28513,-32768,
-32768,-32768,-32768,-32768,  1118,   -54,-32768,  1438,   559,   573,
-32768,  1484,   136,   -15,-32768,  1469,-32768,-32768,-32768,  1446,
-32768,  1448,-32768,  4695,-32768,  1595,    30,   655,-32768,  1721,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1459,  4670,
    97,    97, 27688,-32768, 27688,-32768,  1115,-32768,-32768,   284,
  1453,-32768,-32768,-32768,  1454,   260,   -86,  1724,-32768,-32768,
-32768,-32768,-32768,-32768,   362,  1674,  1674,  1674,-32768,   551,
-32768,-32768,   211,   211,-32768,-32768,-32768,-32768,  1607,  1605,
  1476,  1541,-32768,  1603,-32768,-32768,-32768,   401,-32768,-32768,
-32768,-32768,  1501,  1614,    98,-32768,    98,    98,    98,    98,
-32768, 25213,  1714,  1556,  1509,  1510,   443,-32768, 25763,   -67,
  5562,-32768,-32768,  1487,  1491,  1494,-32768,-32768,   284, 26038,
-32768, 10018,   584,-32768,  1118, 26038, 15958,    52,-32768,-32768,
 27688,  3013,  1625,  1718,-32768,   -81,  1500,  1502,   590,  1504,
-32768,-32768,-32768,  1505,  1694,-32768,  1511,   518,   315,  1637,
  1671,-32768,  3139,   702,  1514,  1516,  1517,  1518, 18037, 18037,
 18037, 18037,  1519,   521,  1118,  1522,-32768,   -84,   -39,  1525,
  1609, 12394, 15067, 12394, 12394,  4780,   -70,  1527,  1529,   153,
   974,   996,   304,  1530,  1532, 16255,  1536,  1537,  1539, 16255,
 16255, 16255, 16255, 15067,   492,  5605,  1118,  1542,   501,   746,
   625,-32768,    48,-32768,  1452, 15958,  1538,  1521,  1543,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
  2631,  1547,-32768,  1548,  1550,-32768,  1551,  1552,  1553,-32768,
 16552, 16552, 16552, 16552, 15958,   490,  1118,  1554,-32768,   -84,
-32768,-32768, 22321,-32768,   452,-32768,-32768,  1432, 16552,  1555,
 15958,  2097,  1557,  1559, 12691,   554,  1564,  1566, 12691,   811,
  1567,  1568, 12691,  3455,  1571,  1573, 12691,  3455,  1576,  1577,
 12691,     3,  1578,  1579, 12691,     3,  1580,  1581, 12691,  1666,
  1584,  1588, 12691,  1666,   225,  1546,-32768,    36,-32768, 19225,
  1092,-32768,  1560,-32768,-32768,  1569,-32768,   138,  1560,-32768,
-32768, 27688,-32768,-32768, 22726,  1092, 21771,  1549,  1724,-32768,
-32768,-32768,   409,  1722,  1558,  1582, 27688,-32768, 15958,-32768,
-32768,   894,-32768, 27688,-32768,-32768,  -119,-32768,-32768,  1597,
-32768,  1812,-32768,   371,-32768,   -97,  3624,-32768,  1586,  1587,
-32768,  1608,-32768,-32768,    46,    46,   595,  1593,   595,  1591,
-32768,-32768,   753,  1120,  1596,-32768,  1713,  1742,  1600, 25213,
-32768, 27688, 27688, 27688, 27688,-32768,-32768,-32768,  1754,  1754,
 25763,    12,    -6,  1612,-32768,-32768, 24938,-32768,-32768,  1704,
 24938,   327,  1055,-32768,-32768,-32768,-32768,-32768,-32768, 27688,
   949,-32768,-32768,-32768,-32768,   998,-32768, 28788,  1540, 20371,
 19795, 19795, 20083,-32768,  1712,  1793,  1793, 27688,-32768, 27138,
  1546, 27688,-32768,  1708,-32768,  1030, 27688,   -68,-32768,-32768,
  4056, 15067,-32768,  1805, 28990, 27688, 27688,-32768, 15958, 15067,
-32768,-32768,-32768,  1055,-32768,-32768,-32768, 15958,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768, 15958,  1055,-32768, 18037,
 18037, 15067,  8830,   708,  1853,  1853,   178,-32768, 28990, 18037,
 18334, 18037, 18037, 18037, 18037,-32768,  7066, 15067,  1803,-32768,
-32768,  1618,   -70,  1619,  1620, 15067,-32768, 15958,  1055,  1055,
  1540,  1475,  2966,-32768, 18037, 15067,  9127,   884,-32768,  1862,
-32768,  1862,-32768,  1862,-32768,  1624, 28990, 16255, 16552,  1626,
   712, 16255,   608, 16255,   654,   666,  9721, 12988, 13285, 13582,
 13879, 14176, 14473, 14770,   714,  6768, 16255,  1118,  1628,  1809,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,     5,  3329,
   268,-32768,  1538,-32768, 16552,  1055,  1055, 18037, 15067,  9424,
   774,  1870,  1870,  1870,   859, 28990, 16552, 16849, 16552, 16552,
 16552, 16552,-32768,  7364,-32768,  1630,  1633,-32768,-32768,-32768,
-32768,-32768,   516, 22321,  1432,  1540,  1540,  1632,  1540,  1540,
  1635,  1540,  1540,  1636,  1540,  1540,  1638,  1540,  1540,  1639,
  1540,  1540,  1640,  1540,  1540,  1643,  1540,  1540,  1644, 25763,
   284,-32768, 25763,  1647,  1847, 27413,  1648,  1830, 22046,-32768,
-32768,-32768,-32768,-32768,-32768, 15067,-32768,-32768,-32768,  1756,
-32768,  1839,  1678,  1679,  1048,-32768,-32768,-32768,-32768,-32768,
  1656,   655,   655,    30,-32768,-32768,  1459,  1180,-32768,-32768,
-32768,-32768, 27688,-32768,-32768,  1654,    46,  1655,   365,   142,
-32768,   595,-32768,   595,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768, 18631,  1659,  1660, 27688,  1063, 28788,-32768,    31,
-32768,  1778,-32768,  1845,  1691,  1691,  1854,  1811,-32768,-32768,
-32768,   -24,-32768,   979,  1900,-32768,-32768,-32768,-32768,-32768,
  1788,-32768,   329, 25213,  1745, 27688,-32768,  1816,  1038,-32768,
  1739, 27688,   628,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,   551,  1680,   316,-32768,-32768,-32768,-32768,
-32768,-32768,  1863,-32768,-32768,-32768,  1682,-32768, 15067,-32768,
-32768,  1677,-32768,-32768,  5562,-32768,  1683,  5562,  1066,  1684,
   626,  1693,  1695, 12394, 12394, 12394,  1696,-32768,-32768,   920,
   708,    19,    19,  1853,  1853,-32768,   198,   -70, 15067,-32768,
-32768,-32768,-32768,   -70,  4928,  1701,  1702,  1703,  1707,  1709,
  1710, 12394, 12394, 12394,  1717,   717,   836,  2966,-32768,   525,
 22321,   984,   605,  1034,  1040,  1572,-32768, 16552,  1715, 16255,
  2904,-32768,  1720,  1728, 12691,   884,-32768,  1729,  1731, 12691,
  1210,-32768,  1732,  1733, 12691,  3587,-32768,  1735,  1736, 12691,
  3587,-32768,  1737,  1738, 12691,    57,-32768,  1740,  1743, 12691,
    57,-32768,  1746,  1749, 12691,  1862,-32768,  1751,  1752, 12691,
  1862,-32768,  1741,  1068,   246,  1723,-32768,  1540,  1753,-32768,
-32768, 15661,  1755,  1538,  1747,-32768,   864,  1759,  1762,  1764,
  1765, 12394, 12394, 12394,  1769,-32768,-32768,  1029,   774,    60,
    60,  1870,  1870,-32768,   344,-32768, 22523, 16552,-32768,  1770,
  1774,-32768,  1773,  1775,-32768,  1776,  1777,-32768,  1779,  1780,
-32768,  1781,  1784,-32768,  1785,  1789,-32768,  1792,  1794,-32768,
  1795,  1797,-32768,  1798,  1799,-32768,-32768,-32768,  1346, 25763,
  1893, 24663,-32768,-32768,  1966,  1957,-32768,-32768,-32768,  1860,
-32768,   -65,-32768,  1814, 27688,-32768,-32768,-32768,-32768,-32768,
  1817,-32768,  1459,-32768,-32768,-32768,   595,  1813,   595,  1806,
-32768,-32768,  1810, 18631,-32768, 18631, 18631, 18631, 18631, 18631,
  1384,  1818,-32768,  1821, 27688, 27688,  1219,-32768,  2012,  2015,
 27688,   551,  1843,-32768,-32768,  1895,  2008,    12,-32768,-32768,
    85, 25763,-32768,-32768,  1822,-32768,-32768,-32768,  1994,-32768,
  1826, 27688, 17146,  1980,  1999, 27688,-32768,-32768,  1038,-32768,
-32768,    85,-32768,-32768,-32768,-32768, 27688,  1978,  1981,-32768,
  1982, 10018,-32768,-32768,-32768,-32768,-32768, 28990,-32768,-32768,
  1832,  1835,  1836,-32768,-32768,   -70, 28990,  1084,  1088,  1090,
  1104,  1124,  1125,  1837,  1838,  1840,  1131, 16552,  1842,  1134,
  1137,  1145,   830, 22321,  1572,-32768,  1540,  1540,  1844,  1540,
  1540,  1857,  1540,  1540,  1858,  1540,  1540,  1861,  1540,  1540,
  1864,  1540,  1540,  1878,  1540,  1540,  1879,  1540,  1540,  1881,
  1170,  1172,  1118,  1882,  1540,  1883,  1885,  5562,  1540,-32768,
  1538, 28990,-32768,-32768,-32768,-32768,  1886,  1887,  1888,-32768,
-32768,-32768,  1029,-32768, 22523,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,  1984,-32768,  1984,  1346,-32768,  1984,-32768,  2019,-32768,
 25763,-32768, 15067, 15958,-32768,    85,-32768,-32768,   706,-32768,
   -97,    26,-32768,-32768,   595,-32768,   595, 18631,  3712,  1157,
  2084,  2084,  2084,  1483, 28990, 18631, 22523,  1855,   715, 18631,
   793, 18631,-32768,-32768, 18928, 18631, 18631, 18631, 18631, 18631,
 18631, 18631,-32768,  7960,  1236,  1249,-32768,-32768, 17443,-32768,
  1867,-32768,    85,-32768,  -127,  1991,-32768,  2030,  1092,  1868,
 27688,-32768, 18631,   463,  1890,-32768,  1891,  1892,-32768,-32768,
-32768, 17443, 17443, 17443, 17443, 17443,   460,  1897,-32768,-32768,
-32768,  1899,-32768,-32768,  1894,  1903,-32768,-32768,   -31,  1904,
  1821,-32768, 27688,-32768,-32768,  1310,  1902,-32768,-32768,-32768,
  1906,  1173,  1192,  1212,   195,  1214, 16552,  1908,  1909,  1910,
  1215,  1911,  1912,  1223,  1913,  1914,  1234,  1915,  1917,  1258,
  1918,  1920,  1264,  1921,  1922,  1265,  1924,  1926,  1268,  1928,
  1929,  1275,-32768,-32768,  1930,-32768,-32768,  1932,-32768,  1933,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768, 25763,
-32768,   -70,  5562,-32768,-32768,-32768,  4670,-32768,-32768,   706,
-32768,  1180,-32768,  1459,-32768,-32768,  5539,-32768,-32768,  3712,
  2127,-32768, 22523,-32768,   813,-32768,-32768,  1815, 22523,  1905,
 18631,  3258,  1157,  2291,  3736,  3736,    76,    76,  2084,  2084,
-32768,  1341,  6066,  2069,-32768,   460,   551,-32768,-32768,-32768,
-32768, 27688,    85,  2016, 27688,  1937,  2755,-32768, 17443,  1055,
  1055,  1051,  2179,  2179,  2179,   143, 28990, 17740, 17443, 17443,
 17443, 17443, 17443, 17443, 17443,  7662, 27688,  2103,  1704, 27688,
 28990, 28990,   -22, 27688,  1942,-32768,-32768,  1278,   282,  1281,
  1284,  1285,  1288,  1291,  1294,  1298,  1304,  1306,  1307,  1308,
  1314,  1324,  1337,  1357,  1369,  1370,-32768,-32768,-32768,    -3,
  4174,-32768,-32768,-32768, 28990, 22523,  1378,-32768,-32768,-32768,
-32768,  2176, 22523,  1815, 18631,-32768, 27688,-32768,  1944,-32768,
  2014,-32768,-32768,-32768,   484,  1946,  1948,-32768,-32768,  1051,
   460,   -13,   -13,   131,   131,  2179,  2179,-32768,  1379,   460,
  1383,    94,  2099,-32768,-32768,-32768,-32768,   551,-32768,-32768,
-32768,  1387,  1952,  1955,  1956,-32768,  1958,-32768, 22523,-32768,
 22523,  1388,  6066,  1899,  1250,-32768,   865, 28990,-32768,-32768,
 17443,-32768,-32768,-32768,-32768,   148,-32768,  2099,-32768,   -31,
 15958, 23005,-32768,-32768,-32768,-32768,-32768,  2103,  1392,-32768,
-32768,-32768,-32768,-32768,-32768,   257,    96,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,  1959,   460,    92,    92,-32768,   -22,
  2834,  1395,-32768,-32768,  1960,  2099,  1250,-32768,  1963,   257,
  1968,  1962,-32768,-32768,  2204,    40,-32768,-32768,-32768,-32768,
 23005,-32768, 27688,-32768,-32768,-32768,  1973,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,  2268,  2269,-32768
};

static const short yypgoto[] = {-32768,
-32768,-32768,-32768,  2178,-32768,-32768,-32768,  1824,  1575,  1327,
-32768,  1010,   718,-32768,  1663,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1434,  1027,
   709,  1032,-32768,-32768,-32768,   422,   241,-32768, -1840,-32768,
  -911,-32768,  -950,    35, -1978,   -38,   -63,   -29,   -57,-32768,
-32768,-32768,-32768,   722,-32768,-32768,-32768,-32768,-32768,   339,
-32768,-32768,-32768,-32768,-32768,-32768, -1244,-32768,-32768,-32768,
-32768,   -62,-32768,-32768,-32768,-32768,  -328,   735,-32768,  1007,
  1002,-32768,-32768,  2225,  1916,  1681,-32768,  2244,-32768,  1804,
  1321,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,    99,
    -9,   -37,-32768,-32768,-32768,   103,  1831,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,  1976,  -331,-32768,-32768,
-32768,   292,-32768,-32768,-32768,   -21,-32768, -2184,-32768,-32768,
-32768,     9,-32768,-32768,-32768,  1179,-32768,-32768,-32768,-32768,
-32768,-32768,   755,-32768,-32768,-32768,  2239,-32768,-32768,  1110,
-32768,  1935,    21,-32768,    18, -1519,  1016,    22,-32768,-32768,
    23,-32768,  1451,  1031,-32768,-32768,  -511,   -90,  4523,-32768,
  1151,  1923,-32768,-32768,-32768,  1123,-32768,-32768,   815,  -449,
-32768,  -346,   113,-32768,-32768,-32768,  1470,-32768, -1721,   363,
  -860,-32768,-32768,   -30,  -624, -1466, -1492, -1404,  -783,  1767,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  -657,  -475,
-32768,-32768,-32768,  2654,-32768,-32768,  -131,  -679,   620,-32768,
-32768,-32768,  3424, -1037,  -560,  -749,   971,-32768, -1226,  -903,
  -873,-32768,-32768,  -687,   684,-32768,   442,-32768,-32768,-32768,
  1360,-32768,-32768,  3959,  1496,-32768,  1155,  -977,  1506,-32768,
   214,  -294,-32768, -1480,    64,  -273,   167,  3250,-32768,-32768,
  4118,   592,    -1,     1,   -27,  -301,  -526,   -40,   577,-32768,
-32768,   -23,-32768,  2081,-32768,  1457,  1941,-32768,-32768,  1450,
  -375,   -34,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  -149,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,   482,-32768,-32768,-32768,   845,-32768, -1769,-32768,-32768,
-32768,  1987,-32768,-32768,-32768,-32768,-32768,  1869,  1455,-32768,
-32768,-32768,-32768,  1497, -1217,  1167,  -371,-32768,-32768,-32768,
 -1201,-32768,-32768,-32768,   224,-32768,-32768,  -224,  -880,  1399,
  2214,  1456,-32768,   898,  -425,  -740,  2636,  1203,  1570,   -46,
-32768,   245,-32768,     2,-32768,   243,-32768,-32768
};


#define	YYLAST		29267


static const short yytable[] = {    38,
   413,    39,    45,   745,   885,   498,   476,  1006,   281,   307,
   453,   281,   681,   303,  1316,   292,   425,   496,  1798,   318,
   455,   280,   303,  1261,   280,   282,  1789,  1568,  1736,   388,
  1039,  1543,   458,   382,   386,  1811,  1837,   391,  2161,   489,
   855,   386,   386,  2001,  1538,  2207,   386,   408,   411,   683,
   125,   492,  2153,  1120,   862,   960,   401,   684,  1190,   678,
   434,   837,   137,   138,   140, -1098,   862,  1730,  1990,   693,
  1731,   616,   159,  1368,   685,   160,   756,  1359,   161,  2221,
   686,   687,   363,   162,   450,   163,   164,   870,   856,  1569,
   774,   358,  1225,  1441,  1442,  1443,  1444,   308,  1141,   309,
   125,   865,   621,    73,    73,   481,    73,   355,   946,  1329,
  1198,  1463,   137,   138,   140,  1397,   364,   320,  1446,   741,
  1329,   688,   629,  1617,  1632,  1354,  1355,  1356,  1357,  1522,
  2189,  1839,  2349,   947,  2015,  2391,   159,  2190,   372,   160,
   866,   365,   161,  1065,    25,   689,  2303,   162,   294,   163,
   164,  1329,   313,  1322,   482,   690,   648,   649,  1660,   650,
   651,  1077,   652,   653,  2374,   654,   739,   655,  2298,   746,
   656,   657,   658,   659,   442,  1384,   306,   443,  1523,  1389,
  1391,  1393,  1395,   444,   445,  2387,  2392,  2304,  1000,  2207,
  2262,   366,    64,    25,   483,    73,   945,   359,  2186,   446,
   948,  2207,   447,  1840,   622,   721,   274,  2324,   367,   276,
  -895,  1740,   310,  1249,  -902,   742,  1330,    70,  1331,   277,
   302,  2202,  2203,  2204,  2205,  2206,   701,  1330,   941,   338,
    91,    91,   691,    91,   435,  2375,  1359,   339,   692,   360,
   935,   314,   936,  1205,  2357,   597,   281,   997,  1211,  2325,
   275,   274,   880,  1446,   276,   361,   315,   942,  1330,   280,
  2222,   857,  2208,   433,  2376,   392,   275,    73,   275,  2141,
   386,-32768,-32768,  2212,  2213,  2214,  2215,  2308,   368,   937,
   275,   394,   303,   303,    75,   375,   281,   460,   385,   303,
   386,   853,   854,  1373,   478,   397,   399,  1022,   386,   303,
   405,   919,   303,  1732,   303,   281,   460,  1364,  1365,    25,
  1226,   303,   303,   277,  1204,  2035,  2036,  1212,  1208,   306,
   501,   376,   743,  2154,  1821,   277,  1822,   640,  1329,  1950,
  2335,   386,  2336,   436, -1098,  1815,  2358,   281,  1016, -1304,
  1446,   396,    87,   356, -1304,  1413,  1414, -1197,  1451,  1452,
   280,   618,   386,    64,  1372,   391,  1374,  1375,  2275,   275,
   511,  1670,   386,   274,  2031,  2032,   276,  2280,  2281,  2282,
  2283,  2284,  2285,  2286,  2287,  2290,  1065,    25,    70,   669,
  1065,  1065,  1065,  1065,  2001,  1283,   670,   448,  1329,   306,
  1141,  1141,  1141,  1141,    91,   679,  2079,  1737, -1098,   402,
   292,   303,   511,   125,   303,   682,  1255,   303,  1141,  1748,
  1749,  1750,  1751,  1752,  1753,   137,   138,   140,  2208,  2214,
  2215,    25,   386,  1577,   781,  1267,  2209,  2210,  2211,  2212,
  2213,  2214,  2215,   938,  1319,  1330,  1329,  1501,   391,  1255,
  1257,  1819,  2278,  1256, -1241,    75,  1631,   311,  1578, -1241,
   303,  1343,   418,  1361,  1268,   828,  1640,  1641,  1642,  1643,
  1644,  1645,   312,   364,  1362,  1363,  1364,  1365,  1306,   733,
  1448,   419,   303,  1257,  1366,   303,   747,  1638,  1633,  1878,
  2356,  1449,  1450,  1451,  1452,  1344, -1263,  1258,   365,   456,
   457, -1263,   922,   923,   681,  1330,   461,  1895,  1579,   736,
   924,  -667,   416,    87,  1879,   737,  1415,   735,   752,   490,
   417,   491,  1661,  1149,  1589,  1580,  1259,  1669,  2207,   499,
  1258,  1677,  -667,  1682,   452,     2,  1687,  1692,  1697,  1702,
  1707,  1712,  1717,  1722,  1329,   421,  1726,   427,   366,    25,
  1861,  1622,  2207,  1330,   462,  1942,  2129,    25,  1446,  1259,
  2318,   891,   485,   895,  1447,   367,  1247,  1448,  1863,  1460,
   832,   277,  1150,  2225,  1734,  1741,  1498,  -596,  1449,  1450,
  1451,  1452,   314, -1264,  1446,  1639,  1453,  1151, -1264,  1359,
  1758,  1504,   879,  1446,   386,   515,   303,   315,  -596,  1908,
   604,   868,   438,   872,   872, -1246,   386,   683,  1461,  2004,
 -1246,  2006,  1039,  2047,   902,   684,   626,  1864,   696,   902,
   902,   698,   837,  1667,   700,   897,   630,  1647,  1648,   960,
  1242,  1865,   685,   960,  -597,   368,  1654,  1866,   686,   687,
   418,   898,  -598,   441,   281,  1340,  1039,  1462,  1266,  1360,
  1913,  1330,   292,  1961,    25,  -597,  -599,   280,   638,   419,
  1867,   916,   413,  -598,  1341,   440,   303,   730,  1141,  2253,
  1260,  1817,  1747,  1526,   451,  1637,  1725,  -599,  1527,   688,
   914,  1853,   962,  2250,  1508,  1509,   717,  1678,   478,   841,
   420,   454,  1144,   275,  1359,   303,   386,   463,  1342,  1039,
   974,   303,  1888,   689,  1141,   303,   303,  2291,  1277,  1665,
  1278,   709,   995,   690,  1755,   710,  1141,  1141,  1141,  1141,
  1141,  1141,  1910,   421,  2272,  -600,   500,   277,  1065,  1145,
  1963,  1146,  1065,   464,  1065,   597,  1679,  1065,  1065,  1065,
  1065,  1065,  1065,  1065,  1065,  2208,  -600,  1065,  1038,  2079,
  1303,  1680,  1304,  2209,  2210,  2211,  2212,  2213,  2214,  2215,
  1573,  1911,  1745,   287,  1576,   506,  1007,  1008,  1009,  2208,
  1147,  1239,  1240,  1017,   479,  1448,  1359,  2209,  2210,  2211,
  2212,  2213,  2214,  2215,  1589,  1024,  1449,  1450,  1451,  1452,
   691,   512,  1070, -1217,   845,   513,   692,  2155, -1217,  2156,
  1912,  1448, -1218,   884,  -601,  1118,  1361, -1218,  1140,  1916,
  1448,   846,  1449,  1450,  1451,  1452,  1618,  1362,  1363,  1364,
  1365,  1449,  1450,  1451,  1452,  -601,  -602,   497,  1195,  1672,
  1195,   303,  2164,  -603,   274,   514,  -604,   276,  1203,-32768,
  -605,  1078,  1446,  1079,  -606,   507,   878,  -602,   881,   516,
   851,   852,   853,   854,  -603,   274,   288,  -604,   276,   277,
   890,  -605,   508,  -895,   823,  -606,  1673,  1727,  1674,  2165,
   597,  2166,  2169,   933,   627,  -607,    64,   306,   926,   837,
  2095,   927,   670,  2009,   281,  2010,  2011,  2012,  2013,  2014,
   839,  1317,   292,  1318,  -608,  -609,  -607,  1334,  1446,  1335,
  1539,    70,   982,  1540,  2097,  1228,  1231,  1675,   986,  1896,
  2167,  1361,   987,   988,  2086,  -608,  -609,    25,    38,    26,
    39,  2170,  1362,  1363,  1364,  1365,  1417,   837,  -610,   623,
  2259,  1418,  1446,   838,  1248,  1250,  2171,   517,   839,   840,
  1952,  1298,  1396,  -611,    25,  1229,   841,  1141,   386,  -610,
   983,    73,  1397,  -612,   842, -1255,  1891,  1892,  1893,  1315,
 -1255,   619,  -613,   624,  -611,  1315,  1016, -1257,    75,  2260,
  1195,    64, -1257,  1285,  -612,  1285,  1285,  1285,  1285,   496,
   496,  1591,    25,  -613,  1904,  1905,  1906,   840,  1359,  2145,
   509,  -614,  2146,-32768,   841,    25,    70,    26,  1038,  1038,
  1038,  1038,   842,  -615,  1362,  1363,  1364,  1365,  2261,  1348,
  1065,  1349,  -614,   843,  -616, -1216,  2147,   620, -1232,  1401,
 -1216,   844,  -617, -1232,  -615,  1070,    87,  1141,   625,  1070,
  1070,  1070,  1070,   828,  -618,  -616,  1545,   828,  1200,  1546,
   633,   828,    25,  -617,    26,   828,    73, -1219,   632,   828,
  -895,   845, -1219,   828,  -902,  -618,  -619,   828,   634,-32768,
  1547,   828,  1548,    75,  1957,  1958,  1959,  2157,   846,  2229,
  1449,  1450,  1451,  1452,  -620,  2160,   635,  -619,    91,  2168,
   604,  2172,  1807,  1808,  2173,  2174,  2175,  2176,  2177,  2178,
  2179,  2180,  -621,  2183,   636,  -620,   847,  1446,   639,   845,
  1140,  1140,  1140,  1140,   848,   849,   850,   851,   852,   853,
   854,  -622,  2197,  -621,  2087,  1448,   846,  -623,  1140,  2207,
  2345,    87,   641,  2091,  1405,  1861,  1449,  1450,  1451,  1452,
  -624,  1862,  -622,  2137,   837,   643,  2139, -1230,  -623,   645,
   838,  1406, -1230,  1863,   847,   839,  -625,   903,   904,  1448,
   751,  -624,   848,   849,   850,   851,   852,   853,   854,   386,
  1449,  1450,  1451,  1452,  -626,   647,   767,  -625,  1746,-32768,
  2346,  1502,   672,  2347,   879,  -658,  1195,  1141,  2130,  1518,
  1411,  1412,  1413,  1414,   675,  -626,   303,   677,  1519,  1520,
   697,   768,  1864,   303,   840,  1511,  -658,   702,   965,  1528,
  1620,   841,  1305,   966,   704,  1361,  1865,   706,  1626,   842,
   769,   209,  1866,   712,   967,   604,  1362,  1363,  1364,  1365,
   843,  -595,  -656,   708,   751,  2015,   968,   969,   844,  1558,
  2264,  1558,  1560,  1561,   303,  1867,  -594,  1263,  1264,  1265,
   386,  2159,  -595,  -656,   219,    38,   962,    39,  1530,   319,
   962,   970,   713,  2142,   354,  1571,  1584,  -594,  1585,   303,
   374,  1549,  1549,  1550,  1550,   389,   929,   930,   718,   478,
  1601,  1601,   478,   770,   931, -1242,   715,   303,  1397,   303,
 -1242,  1612,  1379,  1600,  1600, -1261,   995,   719,   722,  1399,
 -1261,  1582,  2019,   905,   723,  1623,  1624, -1244,   724,   906,
  1306,   907, -1244,   908,  1380,  1586,   845,  1587,   726,  1285,
  1285,  1599,  1599,  1284,  1448,  1286,  1287,  1288,  1289,  1038,
  1038,   727,   732,   846,  2313,  1449,  1450,  1451,  1452,  1038,
  1038,  1038,  1038,  1038,  1038, -1256,-32768,   857,  1400,  1615,
 -1256, -1259,  1627,   753,    25,  1401, -1259,  2212,  2213,  2214,
  2215,   847,   754,  1402,  1038,  1805,  1630,  1806,   762,   848,
   849,   850,   851,   852,   853,   854,  1141,  1070,  1140, -1239,
  1584,  1070,  1838,  1070, -1239,  1886,  1070,  1070,  1070,  1070,
  1070,  1070,  1070,  1070,  1799, -1243,  1070,  1656,  1657, -1245,
 -1243, -1247,  2123,  1516, -1245,   757, -1247,  2023,   274,   288,
  1516,   276,   277,  1545,  1140, -1248,  1546,  1038,   758,    25,
 -1248,    26,   773,  1497,  2024,   759,  1140,  1140,  1140,  1140,
  1140,  1140,   760,   823,   763, -1249, -1250,  1547,   878,  1552,
 -1249, -1250, -1254,  2279,  2194, -1260,   764, -1254, -1258,  1735,
 -1260,  1563,-32768, -1258,  1738,  1739, -1262,  2296,  2297,   765,
  1405, -1262,  2015,  2029,  2030,  2031,  2032,   836,  2016,   386,
   776,   777,   386,  2017,   779,  1793,  1583,  1406,  1195,  1982,
 -1098, -1238,   780, -1240, -1251,  1983, -1238,   159, -1240, -1251,
   160,  2307,   783,   161,  1609,   833,  1611,  1984,   162,   835,
   163,   164,   859, -1252,  1567,  1407,  1985,  1883, -1252,   861,
   837,   869,  1812,  1408,  1409,  1410,  1411,  1412,  1413,  1414,
  1986,   839,  2018, -1253,   860, -1265, -1267,   886, -1253,  2019,
 -1265, -1267,  1834,  1987, -1274,   995,  1317,  2020,  2037, -1274,
  1231,  1231,  1228,   274,   275, -1272,   276,   882,  2021,    25,
 -1272,    26,  1988,  1317,  2355,  2184,  2022,  1818,  1820,   888,
  2042,  2015,   889,  1558,   618,  1858,  1317,  2016,  2185, -1273,
   840,  1873,  2017,  1396, -1273, -1268, -1269,   841,  1396, -1271,
 -1268, -1269,   896,  1396, -1271,-32768, -1270,   201,  1396, -1231,
  1419, -1270, -1266,  1396, -1231, -1283, -1275, -1266,  1396, -1290,
 -1283, -1275, -1282,  1396, -1290, -1288,   900, -1282,  1396, -1280,
 -1288,  1582,   203,  1874, -1280, -1289,  1420, -1281, -1284, -1276,
 -1289,  2018, -1281, -1284, -1276, -1285,   913,   857,  2019,  2226,
 -1285,   208,   209,   910,  2023, -1277,  2020,   911,   286,   293,
 -1277,   286,   301,   304,   305,   912,   917,  2021, -1287,   304,
  1397,  2024,   304, -1287,   915,  2022,   357,  1140,  2265,  1070,
  2266,  1399,   932,   304,   304,   219,   920,   304, -1279, -1214,
   934,   304,   304, -1279,  1011,  1012,   304,   304,   304,  2025,
 -1286, -1278,   845,   940,   963, -1286, -1278,  2026,  2027,  2028,
  2029,  2030,  2031,  2032,   224,  2309,  2321,  2310,  2322,   846,
  1317,   951,  2323,  2033,   726,  2309,  2330,  2337,   972,  2367,
  1400,  2368,  2381,   979,  2382,   980,   990,  1401,   989,   991,
   992,   996,   997,  1787,  2196,-32768,  1497,   847,  1001,  -895,
  1003,    73,  1018,  2023,  1019,  1020,  1021,  1140,   851,   852,
   853,   854,  1041,   781,   837,  1074,  1075,  1421,  1076,  1190,
  2024,  1143,  1207,  1210,  1459,  1422,  1423,  1424,  1425,  1426,
  1427,  1428,  1215,  1224,  1217,  1219,  1232,  1233,  1251,   386,
   597,  1118,   277,  1245,  1246,  1256,  1271,  1272,  2025,  1273,
  1276,  1274,  1279,  1282,   303,   494,  2026,  2027,  2028,  2029,
  2030,  2031,  2032,   468,   469,   470,   471,   472,   473,   474,
  1299,  1300,  2158,  1834,  1308,  1834,  1834,  1834,  1834,  1834,
  1309,  1310,  1301,  1302,   995,   995,   751,  1326,  1327,  1332,
   303,  1333,  1405,  1336,  1337,  2354,  1338,  1345,  1346,  1371,
  1339,   386,  1350,  1512,  1351,  1352,  1353,  1433,  1358,  1406,
  1367,   303,   460,  2048,  1370,  2072,  1377,  1378,  1381,  1524,
  1382,  1513,  1506,  1431,  1385,  1386,  2081,  1387,  1525,  1514,
  1416,  1554,  1434,  1491,  2074,  1435,  1436,  1407,  1437,  1438,
  1439,  1440,  1454,  1464,  1499,  1466,   286,  1467,  1411,  1412,
  1413,  1414,  1469,   437,  1470,  1472,  1473,  1140,  1500,  1475,
  1555,  1476,  1564,  2015,  1478,  1479,  1481,  1482,  1484,  1485,
   304,   293,  1487,  1533,  2017,  2268,  1488,  1531,  1532,  1542,
  1544,   293,   304,   304,  1553,  1572,   304,   304,  1556,   304,
   304,  1574,  1604,  1606,   304,  1613,   480,  1621,   304,   304,
   293,  1359,   304,  1649,   304,   304,   304,  1651,  1652,  1653,
  1397,   304,   304,  1666,  1671,   504,  1728,  1729,  1446,  1756,
  1757,  1765,  1791,  2018,  1768,  1771,  1794,  1774,  1777,  1780,
  2019,   304,  1783,  1786,  1790,  1795,  1800,   286,-32768,  1802,
   386,  1803,  1804,  1805,  2152,  1814,  1816,  1835,  1836,  1841,
  1842,  1843,   304,  1848,  1846,   304,  1851,  1834,  2144,  1852,
  1872,  1998,   304,  1855,  1859,  1834,  1884,  1881,  1877,  1834,
  1882,  1834,  1885,  1887,  1834,  1834,  1834,  1834,  1834,  1834,
  1834,  1834,  1889,  1834,  1890,  1894,   304,  2148,   460,  2149,
  1898,  1899,  1900,  1497,   293,   304,  1901,  2040,  1902,  1903,
   303,   304,  1834,  1914,   304,  2188,  1907,   304,  1917,  1943,
  1991,   460,   460,   460,   460,   460,  1918,  1920,  2054,  1921,
  1923,  1924,   304,  1926,  1927,  1929,  1930,  1993,  1932,   340,
  1941,  1933,   995,  1951,  1935,  2023,   837,  1936,   304,  1938,
  1939,  1945,   838,  1949,   341,  1459,  1140,   839,  1953,   342,
   304,  1954,  2024,  1955,  1956,  2050,   343,   344,  1960,  1964,
   345,  1965,  1966,  1994,  1967,  1968,  1969,  1996,  1970,  1971,
  1972,   346,   304,  1973,  1974,   304,   304,  1997,  1975,   347,
  2025,  1976,   348,  1977,  1978,   604,  1979,  1980,  1981,   386,
  1999,  2029,  2030,  2031,  2032,  2007,   840,  2038,  2008,  2005,
  2039,  2043,  2046,   841,  2044,   349,  2034,   350,   304,  -902,
  2051,   842,  2052,   351,  2053,   352,  2070,  2071,  2082,  2083,
  1834,  2088,   843,  2085,  2089,  2090,  2092,  2093,  2135,  2094,
   844,  2096,  2015,  2101,  1459,    38,  2140,    39,  2148,   821,
  2149,   303,  2192,  2163,  1195,   837,  2104,  2107,   460,  2193,
  2110,   838,  2195,  2113,  2187,  2270,   839,   460,   460,   460,
   460,   460,   460,   460,   460,   460,   995,  2116,  2119,  1195,
  2122,  2124,  2126,   303,  2127,  2131,  2132,  2133,  2199,  2200,
  2201,  2256,   304,  2219,   304,  2216,   304,  2217,  2276,  2277,
  2220,  2227,  2224,  2263,  1497,  2228,   304,  2230,  2231,  2232,
  2233,  2234,  2235,  2236,  2237,   840,  2238,  2239,   845,  2240,
  2241,  2242,   841,  2243,  1834,  2244,  2314,  2245,  2246,  2247,
   842,  2248,  2249,  2267,  2271,   846,  2273,  2207,  2292,  1317,
  2311,   843,  2315,  2316,   286,  2319,   293,  2320,  2326,    38,
  2331,    39,  1530,  2332,  2383,  2333,   413,  2334,  2373,  2386,
  2388,  2389,  1459,   847,  2390,  2395,   304,  2397,  2398,   415,
  1270,   848,   849,   850,   851,   852,   853,   854,   707,  1566,
   460,   944,   304,  1845,   873,  -696,  1557,  2343,   304,  1185,
  2073,  2365,  1854,  1559,  2198,   304,   304,  2312,  2359,  2366,
  2378,   304,  2384,  1602,  2385,   304,   304,  1857,  1850,  1603,
   404,   887,   304,  2341,   373,  2342,  1320,   740,  2295,   631,
  2360,  2294,  2379,   731,   495,  2372,  1849,   845,   412,   628,
  2364,  2338,  1616,  1521,  1214,  1490,  2302,  1608,  1809,  2343,
  2365,  1517,  2394,   644,   846,  1206,   819,  2138,  1459,  2015,
  2393,  1947,  1659,  1497,  1909,  2098,  1199,  1323,  2269,  1503,
  2017,  2329,  1189,   430,   615,  2341,  1243,  2342,  1992,  1241,
   674,  1810,   847,   734,  1244,  1507,  1209,  2254,   424,  2364,
   848,   849,   850,   851,   852,   853,   854,  1311,  1788,  1254,
  1492,  2251,  2252,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   304,   821,     0,     0,  2018,
     0,     0,     0,     0,     0,  1459,  2019,  1459,  1459,  1459,
  1459,  1459,     0,     0,  2020,     0,     0,     0,   304,     0,
   304,   304,    50,     0,     0,    51,    52,     0,   304,    53,
     0,     0,     0,     0,     0,     0,     0,    54,    55,     0,
     0,     0,     0,     0,  1459,     0,     0,     0,     0,     0,
     0,     0,    56,    57,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   304,     0,   304,     0,   293,     0,     0,    58,
     0,     0,     0,     0,   414,     0,     0,     0,     0,     0,
     0,     0,     0,     0,    60,  1459,     0,     0,    61,   293,
    62,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    63,  2023,    64,     0,     0,     0,    65,     0,    66,     0,
    67,     0,     0,     0,    68,     0,     0,     0,  2024,     0,
    69,   304,     0,     0,     0,     0,     0,    70,   304,  1307,
     0,     0,     0,     0,     0,     0,  1459,     0,     0,   304,
     0,     0,     0,     0,     0,   304,  2025,     0,     0,     0,
   304,     0,     0,     0,  2026,  2027,  2028,  2029,  2030,  2031,
  2032,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,    71,     0,    72,     0,     0,    73,    74,  1459,
     0,     0,     0,     0,     0,     0,     0,  1459,  1459,     0,
     0,  1459,     0,  1459,    75,     0,  1459,  1459,  1459,  1459,
  1459,  1459,  1459,  1459,     0,  1459,     0,     0,    76,    77,
  1459,     0,     0,     0,     0,     0,     0,    78,    79,     0,
     0,     0,     0,     0,  1459,     0,     0,    80,    81,     0,
     0,     0,     0,  1459,  1459,  1459,  1459,  1459,     0,    82,
    83,    84,     0,    85,     0,     0,    86,     0,     0,     0,
     0,     0,    87,     0,     0,     0,     0,     0,     0,     0,
     0,    88,     0,     0,   285,   285,     0,   285,    89,   837,
     0,     0,     0,     0,     0,    90,     0,     0,     0,     0,
   839,     0,   278,     0,     0,   278,     0,   278,     0,     0,
     0,     0,   821,   278,     0,     0,   278,     0,     0,     0,
     0,     0,     0,     0,    91,     0,     0,   278,   278,     0,
     0,   278,     0,     0,     0,   278,   278,     0,     0,     0,
   278,   278,   278,     0,     0,     0,     0,     0,     0,   840,
     0,     0,     0,     0,  1459,     0,   841,     0,     0,   304,
  1459,     0,  1459,     0,   842,     0,     0,     0,   320,     0,
     0,   304,     0,     0,   304,   843,   304,     0,     0,     0,
   321,     0,   322,     0,     0,     0,   304,   323,     0,     0,
  1459,     0,     0,   304,   324,   325,     0,     0,   326,  1459,
  1459,  1459,  1459,  1459,  1459,  1459,  1459,  1459,     0,   327,
     0,     0,     0,  2015,     0,     0,     0,   328,     0,  2016,
  -344,     0,     0,     0,  2017,     0,     0,     0,     0,   304,
     0,   304,   304,   304,   304,     0,     0,     0,     0,     0,
   304,     0,     0,   329,     0,  -262,   304,  1459,     0,     0,
   304,   330,     0,   331,  1459,     0,  1459,     0,     0,   304,
   332,   845,     0,     0,     0,     0,     0,   821,     0,   304,
   304,   304,   304,  2018,     0,     0,     0,   304,   846,   304,
  2019,   304,     0,     0,     0,     0,   304,     0,  2020,     0,
     0,     0,   837,     0,   821,   304,   304,     0,   838,  2021,
  1459,     0,  1459,   839,     0,     0,   847,  2022,     0,     0,
     0,     0,  1459,     0,   848,   849,   850,   851,   852,   853,
   854,   431,   285,     0,     0,     0,     0,     0,   821,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   278,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   840,     0,     0,     0,     0,     0,     0,   841,
     0,     0,  1397,     0,   278,     0,   821,   842,  1398,     0,
     0,     0,     0,  1399,     0,     0,   278,   278,   843,     0,
   278,   278,     0,   278,   278,  2023,   844,     0,   278,     0,
   285,     0,   278,   278,     0,     0,   278,     0,   278,   278,
   278,     0,  2024,     0,     0,   278,   278,     0,     0,     0,
     0,     0,     0,   285,     0,   821,     0,     0,     0,     0,
     0,     0,  1400,     0,  1397,   278,     0,     0,     0,  1401,
  2025,   278,     0,   821,     0,  1399,     0,  1402,  2026,  2027,
  2028,  2029,  2030,  2031,  2032,     0,   278,     0,  1403,   278,
     0,     0,     0,     0,  2274,     0,   278,     0,     0,   304,
     0,     0,   304,     0,   845,   304,     0,     0,   304,     0,
   285,   837,     0,     0,     0,   285,     0,   838,     0,  1325,
   278,   846,   839,     0,  1400,     0,     0,     0,     0,   278,
     0,  1401,     0,     0,     0,   278,     0,     0,   278,  1402,
     0,   278,   304,     0,     0,     0,     0,     0,     0,   847,
  1403,     0,     0,     0,     0,     0,   278,   848,   849,   850,
   851,   852,   853,   854,     0,   304,     0,   821,     0,     0,
     0,   840,   278,  2380,  1405,     0,     0,     0,   841,     0,
     0,     0,     0,     0,   278,     0,   842,     0,     0,     0,
     0,  1406,     0,   304,     0,   304,     0,   843,     0,     0,
     0,   304,     0,     0,     0,   844,   278,     0,     0,   278,
   278,     0,     0,  1876,     0,     0,     0,     0,     0,  1407,
     0,     0,     0,     0,     0,     0,     0,  1408,  1409,  1410,
  1411,  1412,  1413,  1414,     0,     0,  1405,   837,     0,     0,
     0,     0,   278,   838,     0,     0,     0,     0,   839,     0,
     0,     0,     0,  1406,     0,     0,     0,   771,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   820,     0,     0,     0,     0,     0,     0,
   821,  1407,     0,   845,     0,     0,     0,     0,   863,  1408,
  1409,  1410,  1411,  1412,  1413,  1414,     0,   840,     0,     0,
   846,     0,     0,     0,   841,     0,     0,     0,     0,     0,
     0,     0,   842,     0,     0,     0,   278,     0,   278,     0,
   278,     0,     0,   843,     0,     0,     0,     0,   847,     0,
   278,   844,     0,     0,     0,     0,   848,   849,   850,   851,
   852,   853,   854,     0,     0,     0,     0,     0,     0,     0,
   285,     0,   285,     0,     0,     0,  2015,     0,     0,     0,
     0,     0,  2016,     0,     0,     0,   821,  2017,   278,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   278,     0,     0,     0,     0,     0,     0,     0,     0,   304,
     0,   304,     0,     0,     0,     0,   278,     0,     0,   845,
     0,     0,   278,     0,   304,     0,  2018,     0,     0,   278,
   278,     0,     0,  2019,     0,   278,   846,   837,     0,   278,
   278,  2020,     0,   838,     0,     0,   278,     0,   839,     0,
     0,     0,  2021,     0,   304,   304,     0,     0,     0,     0,
   304,   293,     0,     0,   847,     0,     0,  1013,     0,     0,
     0,   304,   848,   849,   850,   851,   852,   853,   854,     0,
     0,   304,     0,     0,     0,   304,  1347,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   304,   840,     0,     0,
     0,     0,     0,     0,   841,     0,     0,   821,  1073,     0,
     0,     0,   842,     0,     0,     0,   821,     0,     0,     0,
     0,     0,     0,   843,  1142,     0,     0,     0,     0,     0,
     0,   844,     0,   821,     0,     0,     0,     0,  2023,   278,
   820,  1188,     0,     0,     0,     0,     0,  1201,     0,     0,
     0,     0,     0,     0,     0,  2024,     0,     0,     0,     0,
     0,     0,   278,   837,   278,   278,     0,     0,     0,     0,
     0,   821,   278,     0,   839,     0,     0,     0,     0,     0,
     0,     0,     0,  2025,   821,     0,     0,     0,     0,     0,
     0,  2026,  2027,  2028,  2029,  2030,  2031,  2032,     0,     0,
     0,     0,   285,     0,     0,     0,     0,     0,     0,   845,
   304,     0,     0,     0,     0,     0,   278,     0,   278,     0,
     0,     0,     0,   840,     0,     0,   846,   459,     0,     0,
   841,     0,     0,     0,   821,     0,   821,     0,   842,     0,
     0,     0,     0,     0,     0,     0,   493,     0,     0,     0,
     0,     0,     0,     0,   847,     0,     0,     0,     0,     0,
     0,     0,   848,   849,   850,   851,   852,   853,   854,     0,
   304,     0,     0,     0,  1188,   278,  -695,     0,     0,     0,
     0,     0,   278,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   278,     0,  1397,     0,     0,     0,   278,
     0,     0,   304,     0,   278,     0,  1399,     0,     0,     0,
     0,     0,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    12,     0,    13,    14,    15,    16,    17,    18,    19,
    20,    21,     0,     0,     0,   845,     0,     0,     0,     0,
     0,  1073,     0,     0,     0,  1073,  1073,  1073,  1073,     0,
     0,     0,   846,     0,     0,  1400,     0,     0,   863,   304,
     0,     0,  1401,     0,     0,     0,     0,     0,     0,     0,
  1402,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   847,     0,   821,     0,     0,     0,     0,     0,   821,-32768,
-32768,   851,   852,   853,   854,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   293,     0,     0,     0,
     0,   304,     0,     0,   304,     0,  1142,  1142,  1142,  1142,
  2015,     0,     0,     0,     0,     0,   821,     0,     0,     0,
     0,  2017,     0,     0,  1142,     0,   304,     0,     0,   304,
   821,   821,  2301,   304,  2015,     0,   820,     0,     0,     0,
     0,     0,     0,     0,     0,  2017,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1405,     0,     0,
     0,     0,     0,   863,   821,   821,     0,     0,     0,     0,
  2018,     0,   821,     0,  1406,     0,   304,  2019,     0,     0,
     0,     0,     0,   278,     0,  2020,     0,     0,     0,     0,
     0,     0,     0,     0,  2018,   278,  2021,     0,   278,     0,
   278,  2019,  1407,     0,     0,     0,     0,  2301,     0,  2020,
   278,-32768,-32768,  1411,  1412,  1413,  1414,   278,   821,     0,
   821,     0,     0,     0,  2344,     0,     0,   821,     0,     0,
     0,     0,     0,     0,     0,     0,     0,    22,     0,     0,
    23,   304,     0,    25,     0,    26,     0,    27,     0,     0,
     0,     0,    28,   278,     0,   278,   278,   278,   278,    30,
    31,    32,    33,    34,   278,  1529,     0,     0,     0,  2301,
   278,     0,     0,     0,   278,     0,  2344,     0,     0,     0,
     0,     0,  2023,   278,     0,     0,     0,     0,     0,     0,
   304,   820,   304,   278,   278,   278,   278,     0,     0,  2024,
     0,   278,     0,   278,     0,   278,  2023,     0,     0,     0,
   278,     0,     0,     0,     0,     0,     0,     0,   820,   278,
   278,     0,     0,  2024,     0,     0,     0,  2025,     0,     0,
   775,     0,     0,     0,     0,  2026,  2027,  2028,  2029,  2030,
  2031,  2032,     0,     0,   822,     0,   825,     0,   826,   827,
   831,  2025,   820,     0,     0,     0,     0,     0,     0,     0,
-32768,-32768,  2029,  2030,  2031,  2032,     0,     0,  1036,     0,
     0,     0,     0,  1073,  1142,  1013,     0,  1073,     0,  1073,
     0,     0,  1073,  1073,  1073,  1073,  1073,  1073,  1073,  1073,
   820,     0,  1073,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   894,     0,     0,     0,
  1142,     0,  1068,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1142,  1142,  1142,  1142,  1142,  1142,  1138,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   820,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   837,     0,     0,   820,     0,     0,
   838,     0,     0,     0,     0,   839,  1188,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   278,     0,     0,   278,   981,     2,   278,
     0,     0,   278,     0,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    12,     0,    13,    14,    15,    16,    17,
    18,    19,    20,    21,   840,     0,  1002,     0,     0,     0,
     0,   841,     0,     0,     0,     0,   278,     0,     0,   842,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1023,
   843,     0,     0,     0,     0,     0,  1046,     0,   844,   278,
     0,   820,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    12,     0,    13,    14,    15,    16,    17,    18,    19,
    20,    21,  1619,     0,     0,     0,     0,   278,     0,   278,
     0,     0,     0,     0,     0,   278,  1066,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1121,     0,     0,     0,  1148,     0,  1152,     0,     0,
  1156,  1160,  1164,  1168,  1172,  1176,  1180,  1184,  1036,  1036,
  1036,  1036,     0,     0,     0,     0,   845,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   846,     0,  1068,     0,     0,     0,  1068,
  1068,  1068,  1068,  1142,     0,  1073,     0,     0,     0,     0,
     0,     0,     0,     0,   820,     0,     0,     0,     0,     0,
     0,   847,     0,     0,     0,     0,     0,     0,     0,   848,
   849,   850,   851,   852,   853,   854,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1138,  1138,  1138,  1138,     0,     0,     0,     0,     0,    22,
     0,     0,    23,  1142,    24,    25,     0,    26,  1138,    27,
     0,     0,     0,     0,    28,     0,     0,     0,    29,     0,
   820,    30,    31,    32,    33,    34,    35,    36,     0,     0,
  1321,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   278,     0,   278,     0,    22,     0,     0,
    23,     0,     0,    25,     0,    26,     0,    27,   278,     0,
     0,     0,    28,     0,     0,  1046,     0,  1046,  1046,    30,
    31,    32,    33,    34,     0,  2306,     0,     0,     0,  1383,
     0,     0,     0,  1388,  1390,  1392,  1394,   831,   278,   278,
     0,     0,     0,     0,   278,     0,     0,     0,     0,  1430,
     0,     0,     0,     0,     0,   278,     0,   749,     0,     0,
     0,     0,     0,     0,     0,   278,     0,     0,     0,   278,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   278,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   820,     0,  1142,     0,     0,     0,     0,     0,     0,
   820,     0,     0,     0,     0,     0,     0,     0,  1445,     0,
     0,     0,     0,     0,     0,     0,     0,   820,     0,     0,
     0,     0,     0,     0,  1465,     0,     0,     0,   831,     0,
     0,   875,   831,     0,     0,     0,   831,     0,     0,     0,
   831,     0,     0,     0,   831,     0,     0,     0,   831,  1036,
  1036,     0,   831,     0,     0,   820,   831,     0,     0,  1036,
  1036,  1036,  1036,  1036,  1036,     0,     0,     0,   820,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1036,     0,     0,     0,     0,     0,
     0,     0,   894,     0,   278,     0,     0,  1068,  1138,     0,
     0,  1068,     0,  1068,     0,     0,  1068,  1068,  1068,  1068,
  1068,  1068,  1068,  1068,     0,     0,  1068,     0,   820,     0,
   820,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1138,     0,     0,  1036,     0,     0,
     0,     0,     0,     0,     0,     0,  1138,  1138,  1138,  1138,
  1138,  1138,     0,     0,   278,     0,     0,     0,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    12,     0,    13,
    14,    15,    16,    17,    18,    19,    20,    21,     0,     0,
     0,     0,  1142,     0,     0,     0,   278,  1035,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1625,   837,     0,     0,     0,     0,     0,   838,
     0,  1628,  1220,     0,   839,     0,     0,     0,     0,     0,
  1629,     0,     0,     0,     0,     0,  1046,     0,     0,     0,
     0,  1067,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1221,   278,     0,     0,     0,  1137,     0,     0,
     0,  1655,  1832,     0,     0,     0,     0,     0,     0,     0,
  1046,     0,     0,   840,     0,     0,   820,     0,     0,     0,
   841,  1668,   820,     0,     0,  1676,     0,  1681,   842,     0,
  1686,  1691,  1696,  1701,  1706,  1711,  1716,  1721,   837,   843,
  1066,     0,     0,     0,   838,   278,     0,   844,   278,   839,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   820,     0,     0,  1046,     0,     0,     0,     0,     0,     0,
   278,     0,     0,   278,   820,   820,     0,   278,     0,     0,
     0,     0,     0,     0,     0,  1222,     0,     0,     0,     0,
     0,     0,  1376,     0,     0,     0,  1037,     0,   840,     0,
     0,     0,     0,     0,     0,   841,     0,     0,   820,   820,
     0,     0,     0,   842,     0,     0,   820,     0,     0,     0,
   278,     0,     0,     0,   843,   845,     0,  1138,     0,  1068,
     0,     0,   844,     0,     0,     0,     0,     0,     0,     0,
  1069,     0,   846,    22,     0,     0,    23,     0,     0,    25,
     0,    26,     0,    27,     0,     0,  1139,     0,    28,     0,
     0,     0,   820,     0,   820,    30,    31,    32,    33,    34,
   847,   820,     0,     0,     0,     0,     0,     0,   848,   849,
   850,   851,   852,   853,   854,   278,   837,  1035,  1035,  1035,
  1035,     0,   838,     0,  1897,     0,     0,   839,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1138,     0,     0,
   845,     0,     0,     0,  1067,     0,     0,     0,  1067,  1067,
  1067,  1067,     0,     0,     0,     0,     0,   846,     0,     0,
     0,     0,     0,     0,   278,     0,   278,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   840,     0,     0,     0,
     0,     0,     0,   841,     0,   847,     0,  1046,  1046,  1046,
     0,   842,     0,   848,   849,   850,   851,   852,   853,   854,
     0,     0,   843,  1832,     0,  1832,  1832,  1832,  1832,  1832,
   844,     0,     0,     0,     0,  1046,  1046,  1046,     0,  1137,
  1137,  1137,  1137,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1915,     0,     0,     0,  1137,   831,     0,
     0,     0,  2068,   831,     0,     0,     0,     0,   831,     0,
     0,     0,     0,   831,     0,     0,  2080,     0,   831,     0,
     0,     0,     0,   831,     0,     0,     0,     0,   831,     0,
     0,     0,     0,   831,     0,     0,  1037,  1037,  1037,  1037,
     0,     0,     0,     0,     0,  1948,     0,  1138,   845,     0,
     0,     0,     0,   875,     0,  1046,  1046,  1046,     0,     0,
     0,     0,     0,  1069,     0,   846,     0,  1069,  1069,  1069,
  1069,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   847,     0,     0,     0,     0,     0,     0,
     0,   848,   849,   850,   851,   852,   853,   854,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1139,  1139,
  1139,  1139,     0,     0,     0,     0,     0,  1832,     0,     0,
  1458,     0,     0,     0,     0,  1832,  1139,     0,     0,  1832,
     0,  1832,     0,     0,  1832,  1832,  1832,  1832,  1832,  1832,
  1832,  1832,     0,  1832,     0,  1010,     0,     0,  2068,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1832,     0,     0,     0,     0,     0,  1035,  1035,
     0,  2068,  2068,  2068,  2068,  2068,     0,     0,  1035,  1035,
  1035,  1035,  1035,  1035,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1035,     0,     0,  1138,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1067,  1137,     0,     0,
  1067,     0,  1067,     0,     0,  1067,  1067,  1067,  1067,  1067,
  1067,  1067,  1067,     0,     0,  1067,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1137,     0,     0,  1035,     0,     0,     0,
     0,     0,     0,     0,     0,  1137,  1137,  1137,  1137,  1137,
  1137,     0,     0,     0,     0,     0,     0,  2143,     0,     0,
  1832,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  2068,     0,
     0,     0,     0,     0,     0,     0,     0,  2068,  2068,  2068,
  2068,  2068,  2068,  2068,  2068,  2068,     0,  1037,  1037,     0,
     0,     0,     0,     0,     0,     0,     0,  1037,  1037,  1037,
  1037,  1037,  1037,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1037,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1832,  1069,  1139,     0,     0,  1069,
     0,  1069,     0,     0,  1069,  1069,  1069,  1069,  1069,  1069,
  1069,  1069,     0,     0,  1069,     0,     0,     0,     0,     0,
     0,     0,     0,    49,     0,     0,     0,    50,     0,     0,
    51,    52,  1139,     0,    53,  1037,     0,     0,     0,     0,
     0,     0,    54,    55,  1139,  1139,  1139,  1139,  1139,  1139,
  2068,     0,     0,     0,     0,     0,     0,    56,    57,     0,
     0,  1762,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2015,     0,     0,
     0,     0,     0,  2016,    58,  2255,     0,     0,  2017,    59,
     0,     0,     0,     0,     0,     0,     0,     0,     0,    60,
   837,     0,     0,    61,     0,    62,   838,     0,     0,     0,
     0,   839,     0,     0,     0,    63,  1137,    64,  1067,     0,
     0,    65,     0,    66,     0,    67,     0,     0,     0,    68,
     0,     0,     0,     0,     0,    69,     0,  2018,     0,     0,
     0,     0,    70,  1397,  2019,  1455,     0,     0,     0,  1398,
  1833,     0,  2020,     0,  1399,     0,     0,  1468,     0,     0,
   840,  1471,     0,  2021,     0,  1474,     0,   841,     0,  1477,
     0,  2022,     0,  1480,     0,   842,     0,  1483,     0,     0,
     0,  1486,     0,     0,     0,  1489,   843,    71,     0,    72,
     0,     0,    73,    74,   844,     0,  1137,     0,     0,     0,
     0,     0,     0,  1400,     0,     0,     0,     0,     0,    75,
  1401,     0,     0,     0,     0,     0,     0,     0,  1402,     0,
     0,     0,     0,    76,    77,     0,     0,     0,     0,  1403,
     0,     0,    78,    79,  2361,     0,     0,  1404,     0,     0,
     0,     0,    80,    81,     0,     0,     0,     0,     0,  2023,
     0,     0,     0,     0,    82,    83,    84,     0,    85,     0,
     0,    86,     0,     0,     0,     0,  2024,    87,  1458,     0,
     0,     0,   845,     0,     0,  1139,    88,  1069,     0,     0,
     0,     0,     0,    89,     0,     0,     0,     0,     0,   846,
    90,     0,     0,     0,  2025,     0,     0,     0,     0,     0,
     0,  1590,  2026,  2027,  2028,  2029,  2030,  2031,  2032,     0,
     0,     0,     0,     0,     0,  1405,     0,   847,     0,    91,
     0,     0,     0,     0,     0,   848,   849,   850,   851,   852,
   853,   854,  1406,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1137,     0,     0,     0,
     0,     0,     0,     0,  1962,  1139,     0,     0,     0,     0,
  1407,     0,     0,     0,     0,     0,     0,     0,  1408,  1409,
  1410,  1411,  1412,  1413,  1414,     0,     0,     0,     0,     0,
     0,     0,     0,  1658,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1833,     0,  1833,  1833,  1833,  1833,  1833,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2069,     0,     0,     0,     0,     0,  1759,     0,  1763,  1764,
     0,  1766,  1767,     0,  1769,  1770,     0,  1772,  1773,     0,
  1775,  1776,     0,  1778,  1779,     0,  1781,  1782,     0,  1784,
  1785,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1139,     0,     0,     0,     0,
     0,  1762,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1137,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   784,   785,   786,   787,   788,   789,   790,   791,   792,
     0,   793,  2134,   794,   795,   796,   797,   798,   799,   800,
   801,   802,   803,     0,   804,     0,   805,   806,   807,   808,
   809,     0,   810,   811,   812,   813,   814,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2015,  1833,     0,     0,     0,     0,
  2016,     0,     0,  1833,  2162,  2017,     0,  1833,     0,  1833,
     0,     0,  1833,  1833,  1833,  1833,  1833,  1833,  1833,  1833,
     0,  1833,     0,     0,     0,     0,  2069,     0,     0,     0,
     0,   201,   558,     0,     0,     0,     0,     0,   815,     0,
  1833,     0,     0,     0,     0,     0,     0,     0,   563,  2069,
  2069,  2069,  2069,  2069,  2018,     0,   203,     0,     0,     0,
     0,  2019,   564,  1455,     0,     0,     0,     0,     0,  2020,
     0,     0,     0,     0,     0,   208,   209,  1919,     0,     0,
  2021,     0,  1922,     0,  1139,     0,   570,  1925,  2022,     0,
     0,     0,  1928,     0,     0,     0,     0,  1931,     0,     0,
     0,     0,  1934,     0,     0,     0,     0,  1937,     0,   219,
     0,     0,  1940,     0,     0,     0,   816,   817,     0,     0,
  1944,     0,     0,     0,  1946,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   224,     0,
     0,   818,     0,     0,     0,     0,     0,     0,     0,     0,
  2162,     0,     0,     0,     0,     0,  2162,     0,  1833,     0,
     0,     0,     0,     0,     0,     0,  2023,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  2024,     0,     0,  2069,     0,     0,     0,
     0,     0,     0,     0,     0,  2069,  2069,  2069,  2069,  2069,
  2069,  2069,  2069,  2069,     0,     0,     0,     0,     0,     0,
     0,  2025,   274,     0,     0,   276,     0,     0,     0,  2026,
  2027,  2028,  2029,  2030,  2031,  2032,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  2162,     0,     0,     0,     0,     0,     0,
  2162,     0,  1833,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  2162,     0,  2162,     0,
     0,     0,     0,     0,     0,     0,  1759,     0,  2069,  2099,
  2100,     0,  2102,  2103,     0,  2105,  2106,     0,  2108,  2109,
     0,  2111,  2112,     0,  2114,  2115,     0,  2117,  2118,     0,
  2120,  2121,     0,     0,     0,     0,     0,  2125,     0,     0,
     0,  2128,   518,   519,   520,   521,   522,   523,   524,   525,
   526,     0,   527,     0,   528,   529,   530,   531,   532,   533,
   534,   535,   536,   537,     0,   538,     0,   539,   540,   541,
   542,   543,     0,   544,   545,   546,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   199,
   200,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   549,   550,   551,   552,     0,     0,   553,
     0,     0,     0,     0,     0,     0,   383,   554,   555,   556,
   557,     0,   201,   558,     0,     0,     0,     0,     0,   559,
     0,     0,     0,     0,     0,   560,   561,   562,     0,   563,
     0,     0,     0,     0,     0,     0,     0,   203,     0,     0,
   204,     0,     0,   564,     0,     0,     0,     0,   205,   206,
     0,     0,     0,     0,     0,   207,   208,   209,     0,   565,
     0,   566,   210,     0,   567,   568,   569,   570,   211,     0,
   212,   213,     0,     0,     0,     0,   571,     0,     0,   214,
   215,     0,     0,   216,     0,   217,     0,     0,     0,   218,
   219,     0,     0,   572,     0,     0,     0,   573,   574,   222,
   223,     0,     0,     0,   575,   576,     0,     0,     0,   577,
     0,     0,   578,     0,     0,     0,     0,     0,     0,   224,
   225,   226,   579,     0,   228,   229,     0,   230,   231,     0,
   232,     0,     0,   233,   234,   235,   236,   237,     0,   238,
   239,     0,     0,   240,   241,   242,   243,   244,   245,   246,
   247,   248,     0,     0,     0,     0,   249,     0,   250,   251,
     0,   384,   252,   253,     0,   254,     0,   255,     0,   256,
   257,   258,   259,   260,   261,     0,   262,   263,   264,   265,
   266,   580,     0,   267,   268,   269,   270,   271,     0,     0,
   272,     0,   273,   274,   275,   581,   276,   277,     0,    25,
   582,    26,     0,     0,     0,     0,     0,   583,  1196,     0,
   585,     0,   586,     0,     0,     0,     0,     0,   587,  1197,
   518,   519,   520,   521,   522,   523,   524,   525,   526,     0,
   527,     0,   528,   529,   530,   531,   532,   533,   534,   535,
   536,   537,     0,   538,     0,   539,   540,   541,   542,   543,
     0,   544,   545,   546,   547,   548,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   549,   550,   551,   552,     0,     0,   553,     0,     0,
     0,     0,     0,     0,   383,   554,   555,   556,   557,     0,
   201,   558,     0,     0,     0,     0,     0,   559,     0,     0,
     0,     0,     0,   560,   561,   562,     0,   563,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,   564,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,   565,     0,   566,
   210,     0,   567,   568,   569,   570,   211,     0,   212,   213,
     0,     0,     0,     0,   571,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,   572,     0,     0,     0,   573,   574,   222,   223,     0,
     0,     0,   575,   576,     0,     0,     0,   577,     0,     0,
   578,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   579,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,   384,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,   580,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,   275,   581,   276,   277,     0,    25,   582,    26,
     0,     0,     0,     0,     0,   583,  1723,     0,   585,     0,
   586,     0,     0,     0,     0,     0,   587,  1724,   518,   519,
   520,   521,   522,   523,   524,   525,   526,     0,   527,     0,
   528,   529,   530,   531,   532,   533,   534,   535,   536,   537,
     0,   538,     0,   539,   540,   541,   542,   543,     0,   544,
   545,   546,   547,   548,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   199,   200,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   549,
   550,   551,   552,     0,     0,   553,     0,     0,     0,     0,
     0,     0,   383,   554,   555,   556,   557,     0,   201,   558,
     0,     0,     0,     0,     0,   559,     0,     0,     0,     0,
     0,   560,   561,   562,     0,   563,     0,     0,     0,     0,
     0,     0,     0,   203,     0,     0,   204,     0,     0,   564,
     0,     0,     0,     0,   205,   206,     0,     0,     0,     0,
     0,   207,   208,   209,     0,   565,     0,   566,   210,     0,
   567,   568,   569,   570,   211,     0,   212,   213,     0,     0,
     0,     0,   571,     0,     0,   214,   215,     0,     0,   216,
     0,   217,     0,     0,     0,   218,   219,     0,     0,   572,
     0,     0,     0,   573,   574,   222,   223,     0,     0,     0,
   575,   576,     0,     0,     0,   577,     0,     0,   578,     0,
     0,     0,     0,     0,     0,   224,   225,   226,   579,     0,
   228,   229,     0,   230,   231,     0,   232,     0,     0,   233,
   234,   235,   236,   237,     0,   238,   239,     0,     0,   240,
   241,   242,   243,   244,   245,   246,   247,   248,     0,     0,
     0,     0,   249,     0,   250,   251,     0,   384,   252,   253,
     0,   254,     0,   255,     0,   256,   257,   258,   259,   260,
   261,     0,   262,   263,   264,   265,   266,   580,     0,   267,
   268,   269,   270,   271,     0,     0,   272,     0,   273,   274,
   275,   581,   276,   277,     0,    25,   582,    26,     0,     0,
     0,     0,     0,   583,     0,     0,   585,     0,   586,     0,
     0,     0,     0,     0,   587,  1646,   518,   519,   520,   521,
   522,   523,   524,   525,   526,     0,   527,     0,   528,   529,
   530,   531,   532,   533,   534,   535,   536,   537,     0,   538,
     0,   539,   540,   541,   542,   543,     0,   544,   545,   546,
   547,   548,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   199,   200,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   549,   550,   551,
   552,     0,     0,   553,     0,     0,     0,     0,     0,     0,
   383,   554,   555,   556,   557,     0,   201,   558,     0,     0,
     0,     0,     0,   559,     0,     0,     0,     0,     0,   560,
   561,   562,     0,   563,     0,     0,     0,     0,     0,     0,
     0,   203,     0,     0,   204,     0,     0,   564,     0,     0,
     0,     0,   205,   206,     0,     0,     0,     0,     0,   207,
   208,   209,     0,   565,     0,   566,   210,     0,   567,   568,
   569,   570,   211,     0,   212,   213,     0,     0,     0,     0,
   571,     0,     0,   214,   215,     0,     0,   216,     0,   217,
     0,     0,     0,   218,   219,     0,     0,   572,     0,     0,
     0,   573,   574,   222,   223,     0,     0,     0,   575,   576,
     0,     0,     0,   577,     0,     0,   578,     0,     0,     0,
     0,     0,     0,   224,   225,   226,   579,     0,   228,   229,
     0,   230,   231,     0,   232,     0,     0,   233,   234,   235,
   236,   237,     0,   238,   239,     0,     0,   240,   241,   242,
   243,   244,   245,   246,   247,   248,     0,     0,     0,     0,
   249,     0,   250,   251,     0,   384,   252,   253,     0,   254,
     0,   255,     0,   256,   257,   258,   259,   260,   261,     0,
   262,   263,   264,   265,   266,   580,     0,   267,   268,   269,
   270,   271,     0,     0,   272,     0,   273,   274,   275,   581,
   276,   277,     0,    25,   582,    26,     0,     0,     0,     0,
     0,   583,     0,     0,   585,     0,   586,     0,     0,     0,
     0,     0,   587,  1754,   518,   519,   520,   521,   522,   523,
   524,   525,   526,     0,   527,     0,   528,   529,   530,   531,
   532,   533,   534,   535,   536,   537,     0,   538,     0,   539,
   540,   541,   542,   543,     0,   544,   545,   546,   547,   548,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   199,   200,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  2055,   551,   552,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  2056,
  2057,  2058,  2059,     0,   201,   558,     0,     0,     0,     0,
     0,   559,     0,     0,     0,     0,     0,     0,     0,   562,
     0,   563,     0,     0,     0,     0,     0,     0,     0,   203,
     0,     0,   204,     0,     0,   564,     0,     0,     0,     0,
   205,   206,     0,     0,     0,     0,     0,   207,   208,   209,
     0,   565,     0,   566,   210,     0,     0,     0,     0,   570,
   211,     0,   212,   213,     0,     0,     0,     0,     0,     0,
     0,   214,   215,     0,     0,   216,     0,   217,     0,     0,
     0,   218,   219,     0,     0,     0,     0,     0,     0,   573,
   574,   222,   223,     0,     0,     0,     0,   576,     0,     0,
     0,  2061,     0,     0,   578,     0,     0,     0,     0,     0,
     0,   224,   225,   226,   579,     0,   228,   229,     0,   230,
   231,     0,   232,     0,     0,   233,   234,   235,   236,   237,
     0,   238,   239,     0,     0,   240,   241,   242,   243,   244,
   245,   246,   247,   248,     0,     0,     0,     0,   249,     0,
   250,   251,     0,     0,   252,   253,     0,   254,     0,   255,
     0,   256,   257,   258,   259,   260,   261,     0,   262,   263,
   264,   265,   266,   580,     0,   267,   268,   269,   270,   271,
     0,     0,   272,     0,   273,   274,   275,  2062,   276,     0,
     0,    25,   582,    26,     0,     0,     0,     0,     0,  2063,
     0,     0,  2064,     0,  2065,     0,     0,     0,     0,     0,
  2066,  2288,   518,   519,   520,   521,   522,   523,   524,   525,
   526,     0,   527,     0,   528,   529,   530,   531,   532,   533,
   534,   535,   536,   537,     0,   538,     0,   539,   540,   541,
   542,   543,     0,   544,   545,   546,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   199,
   200,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1823,   551,   552,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   201,   558,     0,     0,     0,     0,     0,   559,
     0,     0,     0,     0,     0,     0,     0,   562,     0,   563,
     0,     0,     0,     0,     0,     0,     0,   203,     0,     0,
   204,     0,     0,   564,     0,     0,     0,     0,   205,   206,
     0,     0,     0,     0,     0,   207,   208,   209,     0,   565,
     0,   566,   210,     0,  1824,     0,  1825,   570,   211,     0,
   212,   213,     0,     0,     0,     0,     0,     0,     0,   214,
   215,     0,     0,   216,     0,   217,     0,     0,     0,   218,
   219,     0,     0,     0,     0,     0,     0,   573,   574,   222,
   223,     0,     0,     0,     0,   576,     0,     0,     0,     0,
     0,     0,   578,     0,     0,     0,     0,     0,     0,   224,
   225,   226,   579,     0,   228,   229,     0,   230,   231,     0,
   232,     0,     0,   233,   234,   235,   236,   237,     0,   238,
   239,     0,     0,   240,   241,   242,   243,   244,   245,   246,
   247,   248,     0,     0,     0,     0,   249,     0,   250,   251,
     0,     0,   252,   253,     0,   254,     0,   255,     0,   256,
   257,   258,   259,   260,   261,     0,   262,   263,   264,   265,
   266,   580,     0,   267,   268,   269,   270,   271,     0,     0,
   272,     0,   273,   274,   275,  1826,   276,     0,     0,    25,
   582,    26,     0,     0,     0,     0,     0,  1827,     0,     0,
  1828,     0,  1829,     0,     0,     0,     0,     0,  1830,  2181,
   168,   169,   170,   171,   172,   173,   174,   175,   176,     0,
   177,     0,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,     0,   188,     0,   189,   190,   191,   192,   193,
     0,   194,   195,   196,   197,   198,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   551,   552,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,   953,     0,     0,     0,     0,     0,   954,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   955,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,     0,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,   565,     0,   566,
   210,     0,     0,     0,     0,   956,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   220,   221,   222,   223,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   578,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   227,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,     0,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,     0,     0,   276,   518,   519,   520,   521,   522,
   523,   524,   525,   526,     0,   527,     0,   528,   529,   530,
   531,   532,   533,   534,   535,   536,   537,   957,   538,     0,
   539,   540,   541,   542,   543,     0,   544,   545,   546,   547,
   548,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1042,     0,     0,   549,   550,   551,   552,
     0,     0,   553,     0,     0,     0,     0,     0,     0,   383,
   554,   555,   556,   557,     0,   201,   558,     0,     0,     0,
     0,     0,   559,     0,     0,     0,     0,     0,   560,   561,
   562,     0,   563,     0,     0,  1043,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,   564,     0,     0,     0,
     0,   205,   206,  1044,     0,     0,     0,     0,   207,   208,
   209,     0,   565,     0,   566,   210,     0,   567,   568,   569,
   570,   211,     0,   212,   213,     0,     0,     0,     0,   571,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,   572,     0,     0,     0,
   573,   574,   222,   223,     0,  1045,     0,   575,   576,     0,
     0,     0,   577,     0,     0,   578,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   579,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,   384,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,   580,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,   275,   581,   276,
   277,     0,    25,   582,    26,     0,     0,     0,     0,     0,
   583,     0,     0,   585,     0,   586,     0,     0,     0,     0,
     0,   587,   518,   519,   520,   521,   522,   523,   524,   525,
   526,     0,   527,     0,   528,   529,   530,   531,   532,   533,
   534,   535,   536,   537,     0,   538,     0,   539,   540,   541,
   542,   543,     0,   544,   545,   546,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   199,
   200,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1634,     0,     0,   549,   550,   551,   552,     0,     0,   553,
     0,     0,     0,     0,     0,     0,   383,   554,   555,   556,
   557,     0,   201,   558,     0,     0,     0,     0,     0,   559,
     0,     0,     0,     0,     0,   560,   561,   562,     0,   563,
     0,     0,  1043,     0,     0,     0,     0,   203,     0,     0,
   204,     0,     0,   564,     0,     0,     0,     0,   205,   206,
  1635,     0,     0,     0,     0,   207,   208,   209,     0,   565,
     0,   566,   210,     0,   567,   568,   569,   570,   211,     0,
   212,   213,     0,     0,     0,     0,   571,     0,     0,   214,
   215,     0,     0,   216,     0,   217,     0,     0,     0,   218,
   219,     0,     0,   572,     0,     0,     0,   573,   574,   222,
   223,     0,  1636,     0,   575,   576,     0,     0,     0,   577,
     0,     0,   578,     0,     0,     0,     0,     0,     0,   224,
   225,   226,   579,     0,   228,   229,     0,   230,   231,     0,
   232,     0,     0,   233,   234,   235,   236,   237,     0,   238,
   239,     0,     0,   240,   241,   242,   243,   244,   245,   246,
   247,   248,     0,     0,     0,     0,   249,     0,   250,   251,
     0,   384,   252,   253,     0,   254,     0,   255,     0,   256,
   257,   258,   259,   260,   261,     0,   262,   263,   264,   265,
   266,   580,     0,   267,   268,   269,   270,   271,     0,     0,
   272,     0,   273,   274,   275,   581,   276,   277,     0,    25,
   582,    26,     0,     0,     0,     0,     0,   583,     0,     0,
   585,     0,   586,     0,     0,     0,     0,     0,   587,   518,
   519,   520,   521,   522,   523,   524,   525,   526,     0,   527,
     0,   528,   529,   530,   531,   532,   533,   534,   535,   536,
   537,     0,   538,     0,   539,   540,   541,   542,   543,     0,
   544,   545,   546,   547,   548,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   199,   200,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1662,     0,     0,
   549,   550,   551,   552,     0,     0,   553,     0,     0,     0,
     0,     0,     0,   383,   554,   555,   556,   557,     0,   201,
   558,     0,     0,     0,     0,     0,   559,     0,     0,     0,
     0,     0,   560,   561,   562,     0,   563,     0,     0,  1043,
     0,     0,     0,     0,   203,     0,     0,   204,     0,     0,
   564,     0,     0,     0,     0,   205,   206,  1663,     0,     0,
     0,     0,   207,   208,   209,     0,   565,     0,   566,   210,
     0,   567,   568,   569,   570,   211,     0,   212,   213,     0,
     0,     0,     0,   571,     0,     0,   214,   215,     0,     0,
   216,     0,   217,     0,     0,     0,   218,   219,     0,     0,
   572,     0,     0,     0,   573,   574,   222,   223,     0,  1664,
     0,   575,   576,     0,     0,     0,   577,     0,     0,   578,
     0,     0,     0,     0,     0,     0,   224,   225,   226,   579,
     0,   228,   229,     0,   230,   231,     0,   232,     0,     0,
   233,   234,   235,   236,   237,     0,   238,   239,     0,     0,
   240,   241,   242,   243,   244,   245,   246,   247,   248,     0,
     0,     0,     0,   249,     0,   250,   251,     0,   384,   252,
   253,     0,   254,     0,   255,     0,   256,   257,   258,   259,
   260,   261,     0,   262,   263,   264,   265,   266,   580,     0,
   267,   268,   269,   270,   271,     0,     0,   272,     0,   273,
   274,   275,   581,   276,   277,     0,    25,   582,    26,     0,
     0,     0,     0,     0,   583,     0,     0,   585,     0,   586,
     0,     0,     0,     0,     0,   587,   518,   519,   520,   521,
   522,   523,   524,   525,   526,     0,   527,     0,   528,   529,
   530,   531,   532,   533,   534,   535,   536,   537,     0,   538,
     0,   539,   540,   541,   542,   543,     0,   544,   545,   546,
   547,   548,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   199,   200,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1742,     0,     0,   549,   550,   551,
   552,     0,     0,   553,     0,     0,     0,     0,     0,     0,
   383,   554,   555,   556,   557,     0,   201,   558,     0,     0,
     0,     0,     0,   559,     0,     0,     0,     0,     0,   560,
   561,   562,     0,   563,     0,     0,  1043,     0,     0,     0,
     0,   203,     0,     0,   204,     0,     0,   564,     0,     0,
     0,     0,   205,   206,  1743,     0,     0,     0,     0,   207,
   208,   209,     0,   565,     0,   566,   210,     0,   567,   568,
   569,   570,   211,     0,   212,   213,     0,     0,     0,     0,
   571,     0,     0,   214,   215,     0,     0,   216,     0,   217,
     0,     0,     0,   218,   219,     0,     0,   572,     0,     0,
     0,   573,   574,   222,   223,     0,  1744,     0,   575,   576,
     0,     0,     0,   577,     0,     0,   578,     0,     0,     0,
     0,     0,     0,   224,   225,   226,   579,     0,   228,   229,
     0,   230,   231,     0,   232,     0,     0,   233,   234,   235,
   236,   237,     0,   238,   239,     0,     0,   240,   241,   242,
   243,   244,   245,   246,   247,   248,     0,     0,     0,     0,
   249,     0,   250,   251,     0,   384,   252,   253,     0,   254,
     0,   255,     0,   256,   257,   258,   259,   260,   261,     0,
   262,   263,   264,   265,   266,   580,     0,   267,   268,   269,
   270,   271,     0,     0,   272,     0,   273,   274,   275,   581,
   276,   277,     0,    25,   582,    26,     0,     0,     0,     0,
     0,   583,     0,     0,   585,     0,   586,     0,     0,     0,
     0,     0,   587,   518,   519,   520,   521,   522,   523,   524,
   525,   526,     0,   527,     0,   528,   529,   530,   531,   532,
   533,   534,   535,   536,   537,     0,   538,     0,   539,   540,
   541,   542,   543,     0,   544,   545,   546,   547,   548,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   199,   200,     0,  1683,     0,     0,  1684,     0,     0,     0,
     0,     0,     0,     0,   549,  1049,   551,   552,     0,     0,
   553,     0,     0,     0,     0,     0,     0,   383,  1050,  1051,
  1052,  1053,     0,   201,   558,     0,     0,     0,     0,     0,
   559,     0,     0,     0,     0,     0,  1054,  1055,   562,     0,
   563,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,   204,     0,     0,   564,     0,     0,     0,     0,   205,
   206,     0,     0,     0,     0,     0,   207,   208,   209,     0,
   565,     0,   566,   210,     0,     0,   568,     0,   570,   211,
     0,   212,   213,     0,     0,     0,     0,  1057,     0,     0,
   214,   215,     0,     0,   216,     0,   217,     0,     0,     0,
   218,   219,     0,     0,  1058,     0,     0,     0,   573,   574,
   222,   223,     0,     0,     0,  1059,   576,     0,     0,     0,
   577,     0,     0,   578,     0,     0,     0,     0,     0,     0,
   224,   225,   226,   579,     0,   228,   229,     0,   230,   231,
     0,   232,     0,     0,   233,   234,   235,   236,   237,     0,
   238,   239,     0,     0,   240,   241,   242,   243,   244,   245,
   246,   247,   248,     0,     0,     0,     0,   249,     0,   250,
   251,     0,   384,   252,   253,     0,   254,     0,   255,     0,
   256,   257,   258,   259,   260,   261,     0,   262,   263,   264,
   265,   266,   580,     0,   267,   268,   269,   270,   271,     0,
     0,   272,     0,   273,   274,   275,-32768,   276,   277,     0,
    25,   582,    26,     0,     0,     0,     0,     0,  1061,     0,
     0,  1062, -1237,  1063,     0,     0,     0, -1237,     0,  1685,
   518,   519,   520,   521,   522,   523,   524,   525,   526,     0,
   527,     0,   528,   529,   530,   531,   532,   533,   534,   535,
   536,   537,     0,   538,     0,   539,   540,   541,   542,   543,
     0,   544,   545,   546,   547,   548,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   549,   550,   551,   552,     0,     0,   553,     0,     0,
     0,     0,     0,     0,   383,   554,   555,   556,   557,     0,
   201,   558,     0,     0,     0,     0,     0,   559,     0,     0,
     0,     0,     0,   560,   561,   562,     0,   563,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,   564,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,   565,     0,   566,
   210,     0,   567,   568,   569,   570,   211,     0,   212,   213,
     0,     0,     0,     0,   571,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,   572,     0,     0,     0,   573,   574,   222,   223,     0,
     0,     0,   575,   576,     0,     0,     0,   577,     0,     0,
   578,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   579,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,   384,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,   580,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,   275,   581,   276,   277,     0,    25,   582,    26,
     0,     0,     0,     0,     0,   583,   584,     0,   585,     0,
   586,     0,     0,     0,     0,     0,   587,   518,   519,   520,
   521,   522,   523,   524,   525,   526,     0,   527,     0,   528,
   529,   530,   531,   532,   533,   534,   535,   536,   537,     0,
   538,     0,   539,   540,   541,   542,   543,     0,   544,   545,
   546,   547,   548,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   199,   200,     0,  1157,     0,     0,
  1158,     0,     0,     0,     0,     0,     0,     0,   549,   550,
   551,   552,     0,     0,   553,     0,     0,     0,     0,     0,
     0,   383,   554,   555,   556,   557,     0,   201,   558,     0,
     0,     0,     0,     0,   559,     0,     0,     0,     0,     0,
   560,   561,   562,     0,   563,     0,     0,     0,     0,     0,
     0,     0,   203,     0,     0,   204,     0,     0,   564,     0,
     0,     0,     0,   205,   206,     0,     0,     0,     0,     0,
   207,   208,   209,     0,   565,     0,   566,   210,     0,   567,
   568,     0,   570,   211,     0,   212,   213,     0,     0,     0,
     0,   571,     0,     0,   214,   215,     0,     0,   216,     0,
   217,     0,     0,     0,   218,   219,     0,     0,   572,     0,
     0,     0,   573,   574,   222,   223,     0,     0,     0,   575,
   576,     0,     0,     0,   577,     0,     0,   578,     0,     0,
     0,     0,     0,     0,   224,   225,   226,   579,     0,   228,
   229,     0,   230,   231,     0,   232,     0,     0,   233,   234,
   235,   236,   237,     0,   238,   239,     0,     0,   240,   241,
   242,   243,   244,   245,   246,   247,   248,     0,     0,     0,
     0,   249,     0,   250,   251,     0,   384,   252,   253,     0,
   254,     0,   255,     0,   256,   257,   258,   259,   260,   261,
     0,   262,   263,   264,   265,   266,   580,     0,   267,   268,
   269,   270,   271,     0,     0,   272,     0,   273,   274,   275,
   581,   276,   277,     0,    25,   582,    26,     0,     0,     0,
     0,     0,   583,     0,     0,   585,     0,   586,     0,     0,
     0,     0,     0,  1159,   518,   519,   520,   521,   522,   523,
   524,   525,   526,     0,   527,     0,   528,   529,   530,   531,
   532,   533,   534,   535,   536,   537,     0,   538,     0,   539,
   540,   541,   542,   543,     0,   544,   545,   546,   547,   548,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   199,   200,     0,  1161,     0,     0,  1162,     0,     0,
     0,     0,     0,     0,     0,   549,   550,   551,   552,     0,
     0,   553,     0,     0,     0,     0,     0,     0,   383,   554,
   555,   556,   557,     0,   201,   558,     0,     0,     0,     0,
     0,   559,     0,     0,     0,     0,     0,   560,   561,   562,
     0,   563,     0,     0,     0,     0,     0,     0,     0,   203,
     0,     0,   204,     0,     0,   564,     0,     0,     0,     0,
   205,   206,     0,     0,     0,     0,     0,   207,   208,   209,
     0,   565,     0,   566,   210,     0,   567,   568,     0,   570,
   211,     0,   212,   213,     0,     0,     0,     0,   571,     0,
     0,   214,   215,     0,     0,   216,     0,   217,     0,     0,
     0,   218,   219,     0,     0,   572,     0,     0,     0,   573,
   574,   222,   223,     0,     0,     0,   575,   576,     0,     0,
     0,   577,     0,     0,   578,     0,     0,     0,     0,     0,
     0,   224,   225,   226,   579,     0,   228,   229,     0,   230,
   231,     0,   232,     0,     0,   233,   234,   235,   236,   237,
     0,   238,   239,     0,     0,   240,   241,   242,   243,   244,
   245,   246,   247,   248,     0,     0,     0,     0,   249,     0,
   250,   251,     0,   384,   252,   253,     0,   254,     0,   255,
     0,   256,   257,   258,   259,   260,   261,     0,   262,   263,
   264,   265,   266,   580,     0,   267,   268,   269,   270,   271,
     0,     0,   272,     0,   273,   274,   275,   581,   276,   277,
     0,    25,   582,    26,     0,     0,     0,     0,     0,   583,
     0,     0,   585,     0,   586,     0,     0,     0,     0,     0,
  1163,   518,   519,   520,   521,   522,   523,   524,   525,   526,
     0,   527,     0,   528,   529,   530,   531,   532,   533,   534,
   535,   536,   537,     0,   538,     0,   539,   540,   541,   542,
   543,     0,   544,   545,   546,   547,   548,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   199,   200,
     0,  1165,     0,     0,  1166,     0,     0,     0,     0,     0,
     0,     0,   549,   550,   551,   552,     0,     0,   553,     0,
     0,     0,     0,     0,     0,   383,   554,   555,   556,   557,
     0,   201,   558,     0,     0,     0,     0,     0,   559,     0,
     0,     0,     0,     0,   560,   561,   562,     0,   563,     0,
     0,     0,     0,     0,     0,     0,   203,     0,     0,   204,
     0,     0,   564,     0,     0,     0,     0,   205,   206,     0,
     0,     0,     0,     0,   207,   208,   209,     0,   565,     0,
   566,   210,     0,   567,   568,     0,   570,   211,     0,   212,
   213,     0,     0,     0,     0,   571,     0,     0,   214,   215,
     0,     0,   216,     0,   217,     0,     0,     0,   218,   219,
     0,     0,   572,     0,     0,     0,   573,   574,   222,   223,
     0,     0,     0,   575,   576,     0,     0,     0,   577,     0,
     0,   578,     0,     0,     0,     0,     0,     0,   224,   225,
   226,   579,     0,   228,   229,     0,   230,   231,     0,   232,
     0,     0,   233,   234,   235,   236,   237,     0,   238,   239,
     0,     0,   240,   241,   242,   243,   244,   245,   246,   247,
   248,     0,     0,     0,     0,   249,     0,   250,   251,     0,
   384,   252,   253,     0,   254,     0,   255,     0,   256,   257,
   258,   259,   260,   261,     0,   262,   263,   264,   265,   266,
   580,     0,   267,   268,   269,   270,   271,     0,     0,   272,
     0,   273,   274,   275,   581,   276,   277,     0,    25,   582,
    26,     0,     0,     0,     0,     0,   583,     0,     0,   585,
     0,   586,     0,     0,     0,     0,     0,  1167,   518,   519,
   520,   521,   522,   523,   524,   525,   526,     0,   527,     0,
   528,   529,   530,   531,   532,   533,   534,   535,   536,   537,
     0,   538,     0,   539,   540,   541,   542,   543,     0,   544,
   545,   546,   547,   548,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   199,   200,     0,  1169,     0,
     0,  1170,     0,     0,     0,     0,     0,     0,     0,   549,
   550,   551,   552,     0,     0,   553,     0,     0,     0,     0,
     0,     0,   383,   554,   555,   556,   557,     0,   201,   558,
     0,     0,     0,     0,     0,   559,     0,     0,     0,     0,
     0,   560,   561,   562,     0,   563,     0,     0,     0,     0,
     0,     0,     0,   203,     0,     0,   204,     0,     0,   564,
     0,     0,     0,     0,   205,   206,     0,     0,     0,     0,
     0,   207,   208,   209,     0,   565,     0,   566,   210,     0,
   567,   568,     0,   570,   211,     0,   212,   213,     0,     0,
     0,     0,   571,     0,     0,   214,   215,     0,     0,   216,
     0,   217,     0,     0,     0,   218,   219,     0,     0,   572,
     0,     0,     0,   573,   574,   222,   223,     0,     0,     0,
   575,   576,     0,     0,     0,   577,     0,     0,   578,     0,
     0,     0,     0,     0,     0,   224,   225,   226,   579,     0,
   228,   229,     0,   230,   231,     0,   232,     0,     0,   233,
   234,   235,   236,   237,     0,   238,   239,     0,     0,   240,
   241,   242,   243,   244,   245,   246,   247,   248,     0,     0,
     0,     0,   249,     0,   250,   251,     0,   384,   252,   253,
     0,   254,     0,   255,     0,   256,   257,   258,   259,   260,
   261,     0,   262,   263,   264,   265,   266,   580,     0,   267,
   268,   269,   270,   271,     0,     0,   272,     0,   273,   274,
   275,   581,   276,   277,     0,    25,   582,    26,     0,     0,
     0,     0,     0,   583,     0,     0,   585,     0,   586,     0,
     0,     0,     0,     0,  1171,   518,   519,   520,   521,   522,
   523,   524,   525,   526,     0,   527,     0,   528,   529,   530,
   531,   532,   533,   534,   535,   536,   537,     0,   538,     0,
   539,   540,   541,   542,   543,     0,   544,   545,   546,   547,
   548,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,  1173,     0,     0,  1174,     0,
     0,     0,     0,     0,     0,     0,   549,   550,   551,   552,
     0,     0,   553,     0,     0,     0,     0,     0,     0,   383,
   554,   555,   556,   557,     0,   201,   558,     0,     0,     0,
     0,     0,   559,     0,     0,     0,     0,     0,   560,   561,
   562,     0,   563,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,   564,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,   565,     0,   566,   210,     0,   567,   568,     0,
   570,   211,     0,   212,   213,     0,     0,     0,     0,   571,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,   572,     0,     0,     0,
   573,   574,   222,   223,     0,     0,     0,   575,   576,     0,
     0,     0,   577,     0,     0,   578,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   579,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,   384,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,   580,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,   275,   581,   276,
   277,     0,    25,   582,    26,     0,     0,     0,     0,     0,
   583,     0,     0,   585,     0,   586,     0,     0,     0,     0,
     0,  1175,   518,   519,   520,   521,   522,   523,   524,   525,
   526,     0,   527,     0,   528,   529,   530,   531,   532,   533,
   534,   535,   536,   537,     0,   538,     0,   539,   540,   541,
   542,   543,     0,   544,   545,   546,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   199,
   200,     0,  1177,     0,     0,  1178,     0,     0,     0,     0,
     0,     0,     0,   549,   550,   551,   552,     0,     0,   553,
     0,     0,     0,     0,     0,     0,   383,   554,   555,   556,
   557,     0,   201,   558,     0,     0,     0,     0,     0,   559,
     0,     0,     0,     0,     0,   560,   561,   562,     0,   563,
     0,     0,     0,     0,     0,     0,     0,   203,     0,     0,
   204,     0,     0,   564,     0,     0,     0,     0,   205,   206,
     0,     0,     0,     0,     0,   207,   208,   209,     0,   565,
     0,   566,   210,     0,   567,   568,     0,   570,   211,     0,
   212,   213,     0,     0,     0,     0,   571,     0,     0,   214,
   215,     0,     0,   216,     0,   217,     0,     0,     0,   218,
   219,     0,     0,   572,     0,     0,     0,   573,   574,   222,
   223,     0,     0,     0,   575,   576,     0,     0,     0,   577,
     0,     0,   578,     0,     0,     0,     0,     0,     0,   224,
   225,   226,   579,     0,   228,   229,     0,   230,   231,     0,
   232,     0,     0,   233,   234,   235,   236,   237,     0,   238,
   239,     0,     0,   240,   241,   242,   243,   244,   245,   246,
   247,   248,     0,     0,     0,     0,   249,     0,   250,   251,
     0,   384,   252,   253,     0,   254,     0,   255,     0,   256,
   257,   258,   259,   260,   261,     0,   262,   263,   264,   265,
   266,   580,     0,   267,   268,   269,   270,   271,     0,     0,
   272,     0,   273,   274,   275,   581,   276,   277,     0,    25,
   582,    26,     0,     0,     0,     0,     0,   583,     0,     0,
   585,     0,   586,     0,     0,     0,     0,     0,  1179,   518,
   519,   520,   521,   522,   523,   524,   525,   526,     0,   527,
     0,   528,   529,   530,   531,   532,   533,   534,   535,   536,
   537,     0,   538,     0,   539,   540,   541,   542,   543,     0,
   544,   545,   546,   547,   548,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   199,   200,     0,  1181,
     0,     0,  1182,     0,     0,     0,     0,     0,     0,     0,
   549,   550,   551,   552,     0,     0,   553,     0,     0,     0,
     0,     0,     0,   383,   554,   555,   556,   557,     0,   201,
   558,     0,     0,     0,     0,     0,   559,     0,     0,     0,
     0,     0,   560,   561,   562,     0,   563,     0,     0,     0,
     0,     0,     0,     0,   203,     0,     0,   204,     0,     0,
   564,     0,     0,     0,     0,   205,   206,     0,     0,     0,
     0,     0,   207,   208,   209,     0,   565,     0,   566,   210,
     0,   567,   568,     0,   570,   211,     0,   212,   213,     0,
     0,     0,     0,   571,     0,     0,   214,   215,     0,     0,
   216,     0,   217,     0,     0,     0,   218,   219,     0,     0,
   572,     0,     0,     0,   573,   574,   222,   223,     0,     0,
     0,   575,   576,     0,     0,     0,   577,     0,     0,   578,
     0,     0,     0,     0,     0,     0,   224,   225,   226,   579,
     0,   228,   229,     0,   230,   231,     0,   232,     0,     0,
   233,   234,   235,   236,   237,     0,   238,   239,     0,     0,
   240,   241,   242,   243,   244,   245,   246,   247,   248,     0,
     0,     0,     0,   249,     0,   250,   251,     0,   384,   252,
   253,     0,   254,     0,   255,     0,   256,   257,   258,   259,
   260,   261,     0,   262,   263,   264,   265,   266,   580,     0,
   267,   268,   269,   270,   271,     0,     0,   272,     0,   273,
   274,   275,   581,   276,   277,     0,    25,   582,    26,     0,
     0,     0,     0,     0,   583,     0,     0,   585,     0,   586,
     0,     0,     0,     0,     0,  1183,   518,   519,   520,   521,
   522,   523,   524,   525,   526,     0,   527,     0,   528,   529,
   530,   531,   532,   533,   534,   535,   536,   537,     0,   538,
     0,   539,   540,   541,   542,   543,     0,   544,   545,   546,
   547,   548,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   199,   200,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   549,   550,   551,
   552,     0,     0,   553,     0,     0,     0,     0,     0,     0,
   383,   554,   555,   556,   557,     0,   201,   558,     0,     0,
     0,     0,     0,   559,     0,     0,     0,     0,     0,   560,
   561,   562,     0,   563,     0,     0,  1043,     0,     0,     0,
     0,   203,     0,     0,   204,     0,     0,   564,     0,     0,
     0,     0,   205,   206,     0,     0,     0,     0,     0,   207,
   208,   209,     0,   565,     0,   566,   210,     0,   567,   568,
   569,   570,   211,     0,   212,   213,     0,     0,     0,     0,
   571,     0,     0,   214,   215,     0,     0,   216,     0,   217,
     0,     0,     0,   218,   219,     0,     0,   572,     0,     0,
     0,   573,   574,   222,   223,     0,     0,     0,   575,   576,
     0,     0,     0,   577,     0,     0,   578,     0,     0,     0,
     0,     0,     0,   224,   225,   226,   579,     0,   228,   229,
     0,   230,   231,     0,   232,     0,     0,   233,   234,   235,
   236,   237,     0,   238,   239,     0,     0,   240,   241,   242,
   243,   244,   245,   246,   247,   248,     0,     0,     0,     0,
   249,     0,   250,   251,     0,   384,   252,   253,     0,   254,
     0,   255,     0,   256,   257,   258,   259,   260,   261,     0,
   262,   263,   264,   265,   266,   580,     0,   267,   268,   269,
   270,   271,     0,     0,   272,     0,   273,   274,   275,   581,
   276,   277,     0,    25,   582,    26,     0,     0,     0,     0,
     0,   583,     0,     0,   585,     0,   586,     0,     0,     0,
     0,     0,   587,   518,   519,   520,   521,   522,   523,   524,
   525,   526,     0,   527,     0,   528,   529,   530,   531,   532,
   533,   534,   535,   536,   537,     0,   538,     0,   539,   540,
   541,   542,   543,     0,   544,   545,   546,   547,   548,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   199,   200,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   549,   550,   551,   552,     0,     0,
   553,     0,     0,     0,     0,     0,     0,   383,   554,   555,
   556,   557,     0,   201,   558,     0,     0,     0,     0,     0,
   559,     0,     0,     0,     0,     0,   560,   561,   562,     0,
   563,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,   204,     0,     0,   564,     0,     0,     0,     0,   205,
   206,     0,     0,     0,     0,     0,   207,   208,   209,     0,
   565,     0,   566,   210,     0,   567,   568,   569,   570,   211,
     0,   212,   213,     0,     0,     0,     0,   571,     0,     0,
   214,   215,     0,     0,   216,     0,   217,     0,     0,     0,
   218,   219,    73,     0,   572,     0,     0,     0,   573,   574,
   222,   223,     0,     0,     0,   575,   576,     0,     0,     0,
   577,     0,     0,   578,     0,     0,     0,     0,     0,     0,
   224,   225,   226,   579,     0,   228,   229,     0,   230,   231,
     0,   232,     0,     0,   233,   234,   235,   236,   237,     0,
   238,   239,     0,     0,   240,   241,   242,   243,   244,   245,
   246,   247,   248,     0,     0,     0,     0,   249,     0,   250,
   251,     0,   384,   252,   253,     0,   254,     0,   255,     0,
   256,   257,   258,   259,   260,   261,     0,   262,   263,   264,
   265,   266,   580,     0,   267,   268,   269,   270,   271,     0,
     0,   272,     0,   273,   274,   275,   581,   276,   277,     0,
    25,   582,    26,     0,     0,     0,     0,     0,   583,     0,
     0,   585,     0,   586,     0,     0,     0,     0,     0,   587,
   518,   519,   520,   521,   522,   523,   524,   525,   526,     0,
   527,     0,   528,   529,   530,   531,   532,   533,   534,   535,
   536,   537,     0,   538,     0,   539,   540,   541,   542,   543,
     0,   544,   545,   546,   547,   548,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
  1688,     0,     0,  1689,     0,     0,     0,     0,     0,     0,
     0,   549,  1049,   551,   552,     0,     0,   553,     0,     0,
     0,     0,     0,     0,   383,  1050,  1051,  1052,  1053,     0,
   201,   558,     0,     0,     0,     0,     0,   559,     0,     0,
     0,     0,     0,  1054,  1055,   562,     0,   563,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,   564,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,   565,     0,   566,
   210,     0,  1056,   568,     0,   570,   211,     0,   212,   213,
     0,     0,     0,     0,  1057,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,  1058,     0,     0,     0,   573,   574,   222,   223,     0,
     0,     0,  1059,   576,     0,     0,     0,   577,     0,     0,
   578,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   579,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,   384,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,   580,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,   275,  1060,   276,   277,     0,    25,   582,    26,
     0,     0,     0,     0,     0,  1061,     0,     0,  1062,     0,
  1063,     0,     0,     0,     0,     0,  1690,   518,   519,   520,
   521,   522,   523,   524,   525,   526,     0,   527,     0,   528,
   529,   530,   531,   532,   533,   534,   535,   536,   537,     0,
   538,     0,   539,   540,   541,   542,   543,     0,   544,   545,
   546,   547,   548,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   199,   200,     0,  1693,     0,     0,
  1694,     0,     0,     0,     0,     0,     0,     0,   549,  1049,
   551,   552,     0,     0,   553,     0,     0,     0,     0,     0,
     0,   383,  1050,  1051,  1052,  1053,     0,   201,   558,     0,
     0,     0,     0,     0,   559,     0,     0,     0,     0,     0,
  1054,  1055,   562,     0,   563,     0,     0,     0,     0,     0,
     0,     0,   203,     0,     0,   204,     0,     0,   564,     0,
     0,     0,     0,   205,   206,     0,     0,     0,     0,     0,
   207,   208,   209,     0,   565,     0,   566,   210,     0,  1056,
   568,     0,   570,   211,     0,   212,   213,     0,     0,     0,
     0,  1057,     0,     0,   214,   215,     0,     0,   216,     0,
   217,     0,     0,     0,   218,   219,     0,     0,  1058,     0,
     0,     0,   573,   574,   222,   223,     0,     0,     0,  1059,
   576,     0,     0,     0,   577,     0,     0,   578,     0,     0,
     0,     0,     0,     0,   224,   225,   226,   579,     0,   228,
   229,     0,   230,   231,     0,   232,     0,     0,   233,   234,
   235,   236,   237,     0,   238,   239,     0,     0,   240,   241,
   242,   243,   244,   245,   246,   247,   248,     0,     0,     0,
     0,   249,     0,   250,   251,     0,   384,   252,   253,     0,
   254,     0,   255,     0,   256,   257,   258,   259,   260,   261,
     0,   262,   263,   264,   265,   266,   580,     0,   267,   268,
   269,   270,   271,     0,     0,   272,     0,   273,   274,   275,
  1060,   276,   277,     0,    25,   582,    26,     0,     0,     0,
     0,     0,  1061,     0,     0,  1062,     0,  1063,     0,     0,
     0,     0,     0,  1695,   518,   519,   520,   521,   522,   523,
   524,   525,   526,     0,   527,     0,   528,   529,   530,   531,
   532,   533,   534,   535,   536,   537,     0,   538,     0,   539,
   540,   541,   542,   543,     0,   544,   545,   546,   547,   548,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   199,   200,     0,  1698,     0,     0,  1699,     0,     0,
     0,     0,     0,     0,     0,   549,  1049,   551,   552,     0,
     0,   553,     0,     0,     0,     0,     0,     0,   383,  1050,
  1051,  1052,  1053,     0,   201,   558,     0,     0,     0,     0,
     0,   559,     0,     0,     0,     0,     0,  1054,  1055,   562,
     0,   563,     0,     0,     0,     0,     0,     0,     0,   203,
     0,     0,   204,     0,     0,   564,     0,     0,     0,     0,
   205,   206,     0,     0,     0,     0,     0,   207,   208,   209,
     0,   565,     0,   566,   210,     0,  1056,   568,     0,   570,
   211,     0,   212,   213,     0,     0,     0,     0,  1057,     0,
     0,   214,   215,     0,     0,   216,     0,   217,     0,     0,
     0,   218,   219,     0,     0,  1058,     0,     0,     0,   573,
   574,   222,   223,     0,     0,     0,  1059,   576,     0,     0,
     0,   577,     0,     0,   578,     0,     0,     0,     0,     0,
     0,   224,   225,   226,   579,     0,   228,   229,     0,   230,
   231,     0,   232,     0,     0,   233,   234,   235,   236,   237,
     0,   238,   239,     0,     0,   240,   241,   242,   243,   244,
   245,   246,   247,   248,     0,     0,     0,     0,   249,     0,
   250,   251,     0,   384,   252,   253,     0,   254,     0,   255,
     0,   256,   257,   258,   259,   260,   261,     0,   262,   263,
   264,   265,   266,   580,     0,   267,   268,   269,   270,   271,
     0,     0,   272,     0,   273,   274,   275,  1060,   276,   277,
     0,    25,   582,    26,     0,     0,     0,     0,     0,  1061,
     0,     0,  1062,     0,  1063,     0,     0,     0,     0,     0,
  1700,   518,   519,   520,   521,   522,   523,   524,   525,   526,
     0,   527,     0,   528,   529,   530,   531,   532,   533,   534,
   535,   536,   537,     0,   538,     0,   539,   540,   541,   542,
   543,     0,   544,   545,   546,   547,   548,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   199,   200,
     0,  1703,     0,     0,  1704,     0,     0,     0,     0,     0,
     0,     0,   549,  1049,   551,   552,     0,     0,   553,     0,
     0,     0,     0,     0,     0,   383,  1050,  1051,  1052,  1053,
     0,   201,   558,     0,     0,     0,     0,     0,   559,     0,
     0,     0,     0,     0,  1054,  1055,   562,     0,   563,     0,
     0,     0,     0,     0,     0,     0,   203,     0,     0,   204,
     0,     0,   564,     0,     0,     0,     0,   205,   206,     0,
     0,     0,     0,     0,   207,   208,   209,     0,   565,     0,
   566,   210,     0,  1056,   568,     0,   570,   211,     0,   212,
   213,     0,     0,     0,     0,  1057,     0,     0,   214,   215,
     0,     0,   216,     0,   217,     0,     0,     0,   218,   219,
     0,     0,  1058,     0,     0,     0,   573,   574,   222,   223,
     0,     0,     0,  1059,   576,     0,     0,     0,   577,     0,
     0,   578,     0,     0,     0,     0,     0,     0,   224,   225,
   226,   579,     0,   228,   229,     0,   230,   231,     0,   232,
     0,     0,   233,   234,   235,   236,   237,     0,   238,   239,
     0,     0,   240,   241,   242,   243,   244,   245,   246,   247,
   248,     0,     0,     0,     0,   249,     0,   250,   251,     0,
   384,   252,   253,     0,   254,     0,   255,     0,   256,   257,
   258,   259,   260,   261,     0,   262,   263,   264,   265,   266,
   580,     0,   267,   268,   269,   270,   271,     0,     0,   272,
     0,   273,   274,   275,  1060,   276,   277,     0,    25,   582,
    26,     0,     0,     0,     0,     0,  1061,     0,     0,  1062,
     0,  1063,     0,     0,     0,     0,     0,  1705,   518,   519,
   520,   521,   522,   523,   524,   525,   526,     0,   527,     0,
   528,   529,   530,   531,   532,   533,   534,   535,   536,   537,
     0,   538,     0,   539,   540,   541,   542,   543,     0,   544,
   545,   546,   547,   548,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   199,   200,     0,  1708,     0,
     0,  1709,     0,     0,     0,     0,     0,     0,     0,   549,
  1049,   551,   552,     0,     0,   553,     0,     0,     0,     0,
     0,     0,   383,  1050,  1051,  1052,  1053,     0,   201,   558,
     0,     0,     0,     0,     0,   559,     0,     0,     0,     0,
     0,  1054,  1055,   562,     0,   563,     0,     0,     0,     0,
     0,     0,     0,   203,     0,     0,   204,     0,     0,   564,
     0,     0,     0,     0,   205,   206,     0,     0,     0,     0,
     0,   207,   208,   209,     0,   565,     0,   566,   210,     0,
  1056,   568,     0,   570,   211,     0,   212,   213,     0,     0,
     0,     0,  1057,     0,     0,   214,   215,     0,     0,   216,
     0,   217,     0,     0,     0,   218,   219,     0,     0,  1058,
     0,     0,     0,   573,   574,   222,   223,     0,     0,     0,
  1059,   576,     0,     0,     0,   577,     0,     0,   578,     0,
     0,     0,     0,     0,     0,   224,   225,   226,   579,     0,
   228,   229,     0,   230,   231,     0,   232,     0,     0,   233,
   234,   235,   236,   237,     0,   238,   239,     0,     0,   240,
   241,   242,   243,   244,   245,   246,   247,   248,     0,     0,
     0,     0,   249,     0,   250,   251,     0,   384,   252,   253,
     0,   254,     0,   255,     0,   256,   257,   258,   259,   260,
   261,     0,   262,   263,   264,   265,   266,   580,     0,   267,
   268,   269,   270,   271,     0,     0,   272,     0,   273,   274,
   275,  1060,   276,   277,     0,    25,   582,    26,     0,     0,
     0,     0,     0,  1061,     0,     0,  1062,     0,  1063,     0,
     0,     0,     0,     0,  1710,   518,   519,   520,   521,   522,
   523,   524,   525,   526,     0,   527,     0,   528,   529,   530,
   531,   532,   533,   534,   535,   536,   537,     0,   538,     0,
   539,   540,   541,   542,   543,     0,   544,   545,   546,   547,
   548,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,  1713,     0,     0,  1714,     0,
     0,     0,     0,     0,     0,     0,   549,  1049,   551,   552,
     0,     0,   553,     0,     0,     0,     0,     0,     0,   383,
  1050,  1051,  1052,  1053,     0,   201,   558,     0,     0,     0,
     0,     0,   559,     0,     0,     0,     0,     0,  1054,  1055,
   562,     0,   563,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,   564,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,   565,     0,   566,   210,     0,  1056,   568,     0,
   570,   211,     0,   212,   213,     0,     0,     0,     0,  1057,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,  1058,     0,     0,     0,
   573,   574,   222,   223,     0,     0,     0,  1059,   576,     0,
     0,     0,   577,     0,     0,   578,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   579,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,   384,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,   580,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,   275,  1060,   276,
   277,     0,    25,   582,    26,     0,     0,     0,     0,     0,
  1061,     0,     0,  1062,     0,  1063,     0,     0,     0,     0,
     0,  1715,   518,   519,   520,   521,   522,   523,   524,   525,
   526,     0,   527,     0,   528,   529,   530,   531,   532,   533,
   534,   535,   536,   537,     0,   538,     0,   539,   540,   541,
   542,   543,     0,   544,   545,   546,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   199,
   200,     0,  1718,     0,     0,  1719,     0,     0,     0,     0,
     0,     0,     0,   549,  1049,   551,   552,     0,     0,   553,
     0,     0,     0,     0,     0,     0,   383,  1050,  1051,  1052,
  1053,     0,   201,   558,     0,     0,     0,     0,     0,   559,
     0,     0,     0,     0,     0,  1054,  1055,   562,     0,   563,
     0,     0,     0,     0,     0,     0,     0,   203,     0,     0,
   204,     0,     0,   564,     0,     0,     0,     0,   205,   206,
     0,     0,     0,     0,     0,   207,   208,   209,     0,   565,
     0,   566,   210,     0,  1056,   568,     0,   570,   211,     0,
   212,   213,     0,     0,     0,     0,  1057,     0,     0,   214,
   215,     0,     0,   216,     0,   217,     0,     0,     0,   218,
   219,     0,     0,  1058,     0,     0,     0,   573,   574,   222,
   223,     0,     0,     0,  1059,   576,     0,     0,     0,   577,
     0,     0,   578,     0,     0,     0,     0,     0,     0,   224,
   225,   226,   579,     0,   228,   229,     0,   230,   231,     0,
   232,     0,     0,   233,   234,   235,   236,   237,     0,   238,
   239,     0,     0,   240,   241,   242,   243,   244,   245,   246,
   247,   248,     0,     0,     0,     0,   249,     0,   250,   251,
     0,   384,   252,   253,     0,   254,     0,   255,     0,   256,
   257,   258,   259,   260,   261,     0,   262,   263,   264,   265,
   266,   580,     0,   267,   268,   269,   270,   271,     0,     0,
   272,     0,   273,   274,   275,  1060,   276,   277,     0,    25,
   582,    26,     0,     0,     0,     0,     0,  1061,     0,     0,
  1062,     0,  1063,     0,     0,     0,     0,     0,  1720,   518,
   519,   520,   521,   522,   523,   524,   525,   526,     0,   527,
     0,   528,   529,   530,   531,   532,   533,   534,   535,   536,
   537,     0,   538,     0,   539,   540,   541,   542,   543,     0,
   544,   545,   546,   547,   548,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   199,   200,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   549,   550,   551,   552,     0,     0,   553,     0,     0,     0,
     0,     0,     0,   383,   554,   555,   556,   557,     0,   201,
   558,     0,     0,     0,     0,     0,   559,     0,     0,     0,
     0,     0,   560,   561,   562,     0,   563,     0,     0,     0,
     0,     0,     0,     0,   203,     0,     0,   204,     0,     0,
   564,     0,     0,     0,     0,   205,   206,     0,     0,     0,
     0,     0,   207,   208,   209,     0,   565,     0,   566,   210,
     0,   567,   568,   569,   570,   211,     0,   212,   213,     0,
     0,     0,     0,   571,     0,     0,   214,   215,     0,     0,
   216,     0,   217,     0,     0,     0,   218,   219,     0,     0,
   572,     0,     0,     0,   573,   574,   222,   223,     0,     0,
     0,   575,   576,     0,     0,     0,   577,     0,     0,   578,
     0,     0,     0,     0,     0,     0,   224,   225,   226,   579,
     0,   228,   229,     0,   230,   231,     0,   232,     0,     0,
   233,   234,   235,   236,   237,     0,   238,   239,     0,     0,
   240,   241,   242,   243,   244,   245,   246,   247,   248,     0,
     0,     0,     0,   249,     0,   250,   251,     0,   384,   252,
   253,     0,   254,     0,   255,     0,   256,   257,   258,   259,
   260,   261,     0,   262,   263,   264,   265,   266,   580,     0,
   267,   268,   269,   270,   271,     0,     0,   272,     0,   273,
   274,   275,   581,   276,   277,     0,    25,   582,    26,     0,
     0,     0,     0,     0,   583,     0,     0,   585,     0,   586,
     0,     0,     0,     0,     0,   587,   518,   519,   520,   521,
   522,   523,   524,   525,   526,     0,   527,     0,   528,   529,
   530,   531,   532,   533,   534,   535,   536,   537,     0,   538,
     0,   539,   540,   541,   542,   543,     0,   544,   545,   546,
   547,   548,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   199,   200,     0,  1153,     0,     0,  1154,
     0,     0,     0,     0,     0,     0,     0,   549,   550,   551,
   552,     0,     0,   553,     0,     0,     0,     0,     0,     0,
   383,   554,   555,   556,   557,     0,   201,   558,     0,     0,
     0,     0,     0,   559,     0,     0,     0,     0,     0,   560,
   561,   562,     0,   563,     0,     0,     0,     0,     0,     0,
     0,   203,     0,     0,   204,     0,     0,   564,     0,     0,
     0,     0,   205,   206,     0,     0,     0,     0,     0,   207,
   208,   209,     0,   565,     0,   566,   210,     0,     0,   568,
     0,   570,   211,     0,   212,   213,     0,     0,     0,     0,
   571,     0,     0,   214,   215,     0,     0,   216,     0,   217,
     0,     0,     0,   218,   219,     0,     0,   572,     0,     0,
     0,   573,   574,   222,   223,     0,     0,     0,   575,   576,
     0,     0,     0,   577,     0,     0,   578,     0,     0,     0,
     0,     0,     0,   224,   225,   226,   579,     0,   228,   229,
     0,   230,   231,     0,   232,     0,     0,   233,   234,   235,
   236,   237,     0,   238,   239,     0,     0,   240,   241,   242,
   243,   244,   245,   246,   247,   248,     0,     0,     0,     0,
   249,     0,   250,   251,     0,   384,   252,   253,     0,   254,
     0,   255,     0,   256,   257,   258,   259,   260,   261,     0,
   262,   263,   264,   265,   266,   580,     0,   267,   268,   269,
   270,   271,     0,     0,   272,     0,   273,   274,   275,-32768,
   276,   277,     0,    25,   582,    26,     0,     0,     0,     0,
     0,   583,     0,     0,   585,     0,   586,     0,     0,     0,
     0,     0,  1155,   518,   519,   520,   521,   522,   523,   524,
   525,   526,     0,   527,     0,   528,   529,   530,   531,   532,
   533,   534,   535,   536,   537,     0,   538,     0,   539,   540,
   541,   542,   543,     0,   544,   545,   546,   547,   548,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   199,   200,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   549,   550,   551,   552,     0,     0,
   553,     0,     0,     0,     0,     0,     0,   383,   554,   555,
   556,   557,     0,   201,   558,     0,     0,     0,     0,     0,
   559,     0,     0,     0,     0,     0,   560,   561,   562,     0,
   563,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,   204,     0,     0,   564,     0,     0,     0,     0,   205,
   206,     0,     0,     0,     0,     0,   207,   208,   209,     0,
   565,     0,   566,   210,     0,   567,   568,     0,   570,   211,
     0,   212,   213,     0,     0,     0,     0,   571,     0,     0,
   214,   215,     0,     0,   216,     0,   217,     0,     0,     0,
   218,   219,    73,     0,   572,     0,     0,     0,   573,   574,
   222,   223,     0,     0,     0,   575,   576,     0,     0,     0,
   577,     0,     0,   578,     0,     0,     0,     0,     0,     0,
   224,   225,   226,   579,     0,   228,   229,     0,   230,   231,
     0,   232,     0,     0,   233,   234,   235,   236,   237,     0,
   238,   239,     0,     0,   240,   241,   242,   243,   244,   245,
   246,   247,   248,     0,     0,     0,     0,   249,     0,   250,
   251,     0,   384,   252,   253,     0,   254,     0,   255,     0,
   256,   257,   258,   259,   260,   261,     0,   262,   263,   264,
   265,   266,   580,     0,   267,   268,   269,   270,   271,     0,
     0,   272,     0,   273,   274,   275,   581,   276,   277,     0,
    25,   582,    26,     0,     0,     0,     0,     0,   583,     0,
     0,   585,     0,   586,     0,     0,     0,     0,     0,   587,
   518,   519,   520,   521,   522,   523,   524,   525,   526,     0,
   527,     0,   528,   529,   530,   531,   532,   533,   534,   535,
   536,   537,     0,   538,     0,   539,   540,   541,   542,   543,
     0,   544,   545,   546,   547,   548,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   549,   550,   551,   552,     0,     0,   553,     0,     0,
     0,     0,     0,     0,   383,   554,   555,   556,   557,     0,
   201,   558,     0,     0,     0,     0,     0,   559,     0,     0,
     0,     0,     0,   560,   561,   562,     0,   563,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,   564,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,   565,     0,   566,
   210,     0,   567,   568,     0,   570,   211,     0,   212,   213,
     0,     0,     0,     0,   571,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,   572,     0,     0,     0,   573,   574,   222,   223,     0,
     0,     0,   575,   576,     0,     0,     0,   577,     0,     0,
   578,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   579,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,   384,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,   580,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,   275,   581,   276,   277,     0,    25,   582,    26,
     0,     0,     0,     0,     0,   583,     0,     0,   585,     0,
   586,     0,     0,     0,     0,     0,   587,   518,   519,   520,
   521,   522,   523,   524,   525,   526,     0,   527,     0,   528,
   529,   530,   531,   532,   533,   534,   535,   536,   537,     0,
   538,     0,   539,   540,   541,   542,   543,     0,   544,   545,
   546,   547,   548,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   199,   200,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   549,  1049,
   551,   552,     0,     0,   553,     0,     0,     0,     0,     0,
     0,   383,  1050,  1051,  1052,  1053,     0,   201,   558,     0,
     0,     0,     0,     0,   559,     0,     0,     0,     0,     0,
  1054,  1055,   562,     0,   563,     0,     0,     0,     0,     0,
     0,     0,   203,     0,     0,   204,     0,     0,   564,     0,
     0,     0,     0,   205,   206,     0,     0,     0,     0,     0,
   207,   208,   209,     0,   565,     0,   566,   210,     0,  1056,
   568,     0,   570,   211,     0,   212,   213,     0,     0,     0,
     0,  1057,     0,     0,   214,   215,     0,     0,   216,     0,
   217,     0,     0,     0,   218,   219,     0,     0,  1058,     0,
     0,     0,   573,   574,   222,   223,     0,     0,     0,  1059,
   576,     0,     0,     0,   577,     0,     0,   578,     0,     0,
     0,     0,     0,     0,   224,   225,   226,   579,     0,   228,
   229,     0,   230,   231,     0,   232,     0,     0,   233,   234,
   235,   236,   237,     0,   238,   239,     0,     0,   240,   241,
   242,   243,   244,   245,   246,   247,   248,     0,     0,     0,
     0,   249,     0,   250,   251,     0,   384,   252,   253,     0,
   254,     0,   255,     0,   256,   257,   258,   259,   260,   261,
     0,   262,   263,   264,   265,   266,   580,     0,   267,   268,
   269,   270,   271,     0,     0,   272,     0,   273,   274,   275,
  1060,   276,   277,     0,    25,   582,    26,     0,     0,     0,
     0,     0,  1061,     0,     0,  1062,     0,  1063,     0,     0,
     0,     0,     0,  1064,   518,   519,   520,   521,   522,   523,
   524,   525,   526,     0,   527,     0,   528,   529,   530,   531,
   532,   533,   534,   535,   536,   537,     0,   538,     0,   539,
   540,   541,   542,   543,     0,   544,   545,   546,   547,   548,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   199,   200,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1122,   551,   552,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   383,  1123,
  1124,  1125,  1126,     0,   201,   558,     0,     0,     0,     0,
     0,   559,     0,     0,     0,     0,     0,     0,     0,   562,
     0,   563,     0,     0,     0,     0,     0,     0,     0,   203,
     0,     0,   204,     0,     0,   564,     0,     0,     0,     0,
   205,   206,     0,     0,     0,     0,     0,   207,   208,   209,
     0,   565,     0,   566,   210,     0,     0,     0,     0,   570,
   211,     0,   212,   213,     0,     0,     0,     0,  1127,     0,
     0,   214,   215,     0,     0,   216,     0,   217,     0,     0,
     0,   218,   219,     0,     0,  1128,     0,     0,     0,   573,
   574,   222,   223,     0,     0,     0,  1129,   576,     0,     0,
     0,  1130,     0,     0,   578,     0,     0,     0,     0,     0,
     0,   224,   225,   226,   579,     0,   228,   229,     0,   230,
   231,     0,   232,     0,     0,   233,   234,   235,   236,   237,
     0,   238,   239,     0,     0,   240,   241,   242,   243,   244,
   245,   246,   247,   248,     0,     0,     0,     0,   249,     0,
   250,   251,     0,   384,   252,   253,     0,   254,     0,   255,
     0,   256,   257,   258,   259,   260,   261,     0,   262,   263,
   264,   265,   266,   580,     0,   267,   268,   269,   270,   271,
     0,     0,   272,     0,   273,   274,   275,  1131,   276,   277,
     0,    25,   582,    26,     0,     0,     0,     0,     0,  1132,
     0,     0,  1133,     0,  1134,     0,     0,     0,     0,     0,
  1135,   518,   519,   520,   521,   522,   523,   524,   525,   526,
     0,   527,     0,   528,   529,   530,   531,   532,   533,   534,
   535,   536,   537,     0,   538,     0,   539,   540,   541,   542,
   543,     0,   544,   545,   546,   547,   548,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   199,   200,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1122,   551,   552,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   383,  1123,  1124,  1125,  1126,
     0,   201,   558,     0,     0,     0,     0,     0,   559,     0,
     0,     0,     0,     0,     0,     0,   562,     0,   563,     0,
     0,     0,     0,     0,     0,     0,   203,     0,     0,   204,
     0,     0,   564,     0,     0,     0,     0,   205,   206,     0,
     0,     0,     0,     0,   207,   208,   209,     0,   565,     0,
   566,   210,     0,     0,     0,     0,   570,   211,     0,   212,
   213,     0,     0,     0,     0,  1127,     0,     0,   214,   215,
     0,     0,   216,     0,   217,     0,     0,     0,   218,   219,
     0,     0,  1128,     0,     0,     0,   573,   574,   222,   223,
     0,     0,     0,  1129,   576,     0,     0,     0,  1130,     0,
     0,   578,     0,     0,     0,     0,     0,     0,   224,   225,
   226,   579,     0,   228,   229,     0,   230,   231,     0,   232,
     0,     0,   233,   234,   235,   236,   237,     0,   238,   239,
     0,     0,   240,   241,   242,   243,   244,   245,   246,   247,
   248,     0,     0,     0,     0,   249,     0,   250,   251,     0,
   384,   252,   253,     0,   254,     0,   255,     0,   256,   257,
   258,   259,   260,   261,     0,   262,   263,   264,   265,   266,
   580,     0,   267,   268,   269,   270,   271,     0,     0,   272,
     0,   273,   274,   275,-32768,   276,   277,     0,    25,   582,
    26,     0,     0,     0,     0,     0,  1132,     0,     0,  1133,
     0,  1134,     0,     0,     0,     0,     0,  1135,   518,   519,
   520,   521,   522,   523,   524,   525,   526,     0,   527,     0,
   528,   529,   530,   531,   532,   533,   534,   535,   536,   537,
     0,   538,     0,   539,   540,   541,   542,   543,     0,   544,
   545,   546,   547,   548,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   199,   200,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2055,   551,   552,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  2056,  2057,  2058,  2059,     0,   201,   558,
     0,     0,     0,     0,     0,   559,     0,     0,     0,     0,
     0,     0,     0,   562,     0,   563,     0,     0,     0,     0,
     0,     0,     0,   203,     0,     0,   204,     0,     0,   564,
     0,     0,     0,     0,   205,   206,     0,     0,     0,     0,
     0,   207,   208,   209,     0,   565,     0,   566,   210,     0,
     0,     0,  2060,   570,   211,     0,   212,   213,     0,     0,
     0,     0,     0,     0,     0,   214,   215,     0,     0,   216,
     0,   217,     0,     0,     0,   218,   219,     0,     0,     0,
     0,     0,     0,   573,   574,   222,   223,     0,     0,     0,
     0,   576,     0,     0,     0,  2061,     0,     0,   578,     0,
     0,     0,     0,     0,     0,   224,   225,   226,   579,     0,
   228,   229,     0,   230,   231,     0,   232,     0,     0,   233,
   234,   235,   236,   237,     0,   238,   239,     0,     0,   240,
   241,   242,   243,   244,   245,   246,   247,   248,     0,     0,
     0,     0,   249,     0,   250,   251,     0,     0,   252,   253,
     0,   254,     0,   255,     0,   256,   257,   258,   259,   260,
   261,     0,   262,   263,   264,   265,   266,   580,     0,   267,
   268,   269,   270,   271,     0,     0,   272,     0,   273,   274,
   275,  2062,   276,     0,     0,    25,   582,    26,     0,     0,
     0,     0,     0,  2063,     0,     0,  2064,     0,  2065,     0,
     0,     0,     0,     0,  2066,   518,   519,   520,   521,   522,
   523,   524,   525,   526,     0,   527,     0,   528,   529,   530,
   531,   532,   533,   534,   535,   536,   537,     0,   538,     0,
   539,   540,   541,   542,   543,     0,   544,   545,   546,   547,
   548,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2055,   551,   552,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2056,  2057,  2058,  2059,     0,   201,   558,     0,     0,     0,
     0,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   562,     0,   563,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,   564,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,   565,     0,   566,   210,     0,     0,     0,     0,
   570,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   573,   574,   222,   223,     0,     0,     0,     0,   576,     0,
     0,     0,  2061,     0,     0,   578,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   579,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,   580,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,   275,  2062,   276,
     0,     0,    25,   582,    26,     0,     0,     0,     0,     0,
  2063,     0,     0,  2064,     0,  2065,     0,     0,     0,     0,
     0,  2066,   518,   519,   520,   521,   522,   523,   524,   525,
   526,     0,   527,     0,   528,   529,   530,   531,   532,   533,
   534,   535,   536,   537,     0,   538,     0,   539,   540,   541,
   542,   543,     0,   544,   545,   546,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   199,
   200,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2055,   551,   552,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2056,  2057,  2058,
  2059,     0,   201,   558,     0,     0,     0,     0,     0,   559,
     0,     0,     0,     0,     0,     0,     0,   562,     0,   563,
     0,     0,     0,     0,     0,     0,     0,   203,     0,     0,
   204,     0,     0,   564,     0,     0,     0,     0,   205,   206,
     0,     0,     0,     0,     0,   207,   208,   209,     0,   565,
     0,   566,   210,     0,     0,     0,     0,   570,   211,     0,
   212,   213,     0,     0,     0,     0,     0,     0,     0,   214,
   215,     0,     0,   216,     0,   217,     0,     0,     0,   218,
   219,     0,     0,     0,     0,     0,     0,   573,   574,   222,
   223,     0,     0,     0,     0,   576,     0,     0,     0,  2061,
     0,     0,   578,     0,     0,     0,     0,     0,     0,   224,
   225,   226,   579,     0,   228,   229,     0,   230,   231,     0,
   232,     0,     0,   233,   234,   235,   236,   237,     0,   238,
   239,     0,     0,   240,   241,   242,   243,   244,   245,   246,
   247,   248,     0,     0,     0,     0,   249,     0,   250,   251,
     0,     0,   252,   253,     0,   254,     0,   255,     0,   256,
   257,   258,   259,   260,   261,     0,   262,   263,   264,   265,
   266,   580,     0,   267,   268,   269,   270,   271,     0,     0,
   272,     0,   273,   274,   275,-32768,   276,     0,     0,    25,
   582,    26,     0,     0,     0,     0,     0,  2063,     0,     0,
  2064,     0,  2065,     0,     0,     0,     0,     0,  2066,   518,
   519,   520,   521,   522,   523,   524,   525,   526,     0,   527,
     0,   528,   529,   530,   531,   532,   533,   534,   535,   536,
   537,     0,   538,     0,   539,   540,   541,   542,   543,     0,
   544,   545,   546,   547,   548,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   199,   200,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1025,   551,   552,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   383,     0,     0,     0,     0,     0,   201,
   558,     0,     0,     0,     0,     0,   559,     0,     0,     0,
     0,     0,     0,     0,   562,     0,   563,     0,     0,     0,
     0,     0,     0,     0,   203,     0,     0,   204,     0,     0,
   564,     0,     0,     0,     0,   205,   206,     0,     0,     0,
     0,     0,   207,   208,   209,     0,   565,     0,   566,   210,
     0,     0,     0,     0,   570,   211,     0,   212,   213,     0,
     0,     0,     0,  1026,     0,     0,   214,   215,     0,     0,
   216,     0,   217,     0,     0,     0,   218,   219,     0,     0,
  1027,     0,     0,     0,   573,   574,   222,   223,     0,     0,
     0,  1028,   576,     0,     0,     0,     0,     0,     0,   578,
     0,     0,     0,     0,     0,     0,   224,   225,   226,   579,
     0,   228,   229,     0,   230,   231,     0,   232,     0,     0,
   233,   234,   235,   236,   237,     0,   238,   239,     0,     0,
   240,   241,   242,   243,   244,   245,   246,   247,   248,     0,
     0,     0,     0,   249,     0,   250,   251,     0,   384,   252,
   253,     0,   254,     0,   255,     0,   256,   257,   258,   259,
   260,   261,     0,   262,   263,   264,   265,   266,   580,     0,
   267,   268,   269,   270,   271,     0,     0,   272,     0,   273,
   274,   275,  1029,   276,     0,     0,    25,   582,    26,     0,
     0,     0,     0,     0,  1030,     0,     0,  1031,     0,     0,
     0,     0,     0,     0,     0,  1032,   518,   519,   520,   521,
   522,   523,   524,   525,   526,     0,   527,     0,   528,   529,
   530,   531,   532,   533,   534,   535,   536,   537,     0,   538,
     0,   539,   540,   541,   542,   543,     0,   544,   545,   546,
   547,   548,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   199,   200,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1025,   551,
   552,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   383,     0,     0,     0,     0,     0,   201,   558,     0,     0,
     0,     0,     0,   559,     0,     0,     0,     0,     0,     0,
     0,   562,     0,   563,     0,     0,     0,     0,     0,     0,
     0,   203,     0,     0,   204,     0,     0,   564,     0,     0,
     0,     0,   205,   206,     0,     0,     0,     0,     0,   207,
   208,   209,     0,   565,     0,   566,   210,     0,     0,     0,
     0,   570,   211,     0,   212,   213,     0,     0,     0,     0,
  1026,     0,     0,   214,   215,     0,     0,   216,     0,   217,
     0,     0,     0,   218,   219,     0,     0,  1027,     0,     0,
     0,   573,   574,   222,   223,     0,     0,     0,  1028,   576,
     0,     0,     0,     0,     0,     0,   578,     0,     0,     0,
     0,     0,     0,   224,   225,   226,   579,     0,   228,   229,
     0,   230,   231,     0,   232,     0,     0,   233,   234,   235,
   236,   237,     0,   238,   239,     0,     0,   240,   241,   242,
   243,   244,   245,   246,   247,   248,     0,     0,     0,     0,
   249,     0,   250,   251,     0,   384,   252,   253,     0,   254,
     0,   255,     0,   256,   257,   258,   259,   260,   261,     0,
   262,   263,   264,   265,   266,   580,     0,   267,   268,   269,
   270,   271,     0,     0,   272,     0,   273,   274,   275,-32768,
   276,     0,     0,    25,   582,    26,     0,     0,     0,     0,
     0,  1030,     0,     0,  1031,     0,     0,     0,     0,     0,
     0,     0,  1032,   518,   519,   520,   521,   522,   523,   524,
   525,   526,     0,   527,     0,   528,   529,   530,   531,   532,
   533,   534,   535,   536,   537,     0,   538,     0,   539,   540,
   541,   542,   543,     0,   544,   545,   546,   547,   548,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   199,   200,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1823,   551,   552,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   201,   558,     0,     0,     0,     0,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   562,     0,
   563,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,   204,     0,     0,   564,     0,     0,     0,     0,   205,
   206,     0,     0,     0,     0,     0,   207,   208,   209,     0,
   565,     0,   566,   210,     0,  1824,     0,  1825,   570,   211,
     0,   212,   213,     0,     0,     0,     0,     0,     0,     0,
   214,   215,     0,     0,   216,     0,   217,     0,     0,     0,
   218,   219,     0,     0,     0,     0,     0,     0,   573,   574,
   222,   223,     0,     0,     0,     0,   576,     0,     0,     0,
     0,     0,     0,   578,     0,     0,     0,     0,     0,     0,
   224,   225,   226,   579,     0,   228,   229,     0,   230,   231,
     0,   232,     0,     0,   233,   234,   235,   236,   237,     0,
   238,   239,     0,     0,   240,   241,   242,   243,   244,   245,
   246,   247,   248,     0,     0,     0,     0,   249,     0,   250,
   251,     0,     0,   252,   253,     0,   254,     0,   255,     0,
   256,   257,   258,   259,   260,   261,     0,   262,   263,   264,
   265,   266,   580,     0,   267,   268,   269,   270,   271,     0,
     0,   272,     0,   273,   274,   275,  1826,   276,     0,     0,
    25,   582,    26,     0,     0,     0,     0,     0,  1827,     0,
     0,  1828,     0,  1829,     0,     0,     0,     0,     0,  1830,
   518,   519,   520,   521,   522,   523,   524,   525,   526,     0,
   527,     0,   528,   529,   530,   531,   532,   533,   534,   535,
   536,   537,     0,   538,     0,   539,   540,   541,   542,   543,
     0,   544,   545,   546,   547,   548,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1823,   551,   552,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,   558,     0,     0,     0,     0,     0,   559,     0,     0,
     0,     0,     0,     0,     0,   562,     0,   563,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,   564,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,   565,     0,   566,
   210,     0,     0,     0,  1825,   570,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   573,   574,   222,   223,     0,
     0,     0,     0,   576,     0,     0,     0,     0,     0,     0,
   578,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   579,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,   580,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,   275,-32768,   276,     0,     0,    25,   582,    26,
     0,     0,     0,     0,     0,  1827,     0,     0,  1828,     0,
  1829,     0,     0,     0,     0,     0,  1830,   168,   169,   170,
   171,   172,   173,   174,   175,   176,     0,   177,     0,   178,
   179,   180,   181,   182,   183,   184,   185,   186,   187,     0,
   188,     0,   189,   190,   191,   192,   193,     0,   194,   195,
   196,   197,   198,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   199,   200,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   383,     0,     0,     0,     0,     0,   201,     0,     0,
     0,     0,     0,     0,   202,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   203,     0,     0,   204,     0,     0,     0,     0,
     0,     0,     0,   205,   206,     0,     0,     0,     0,     0,
   207,   208,   209,     0,     0,     0,     0,   210,     0,     0,
     0,     0,     0,   211,     0,   212,   213,     0,     0,     0,
     0,     0,     0,     0,   214,   215,     0,     0,   216,     0,
   217,     0,     0,     0,   218,   219,     0,     0,     0,     0,
     0,     0,   220,   221,   222,   223,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   224,   225,   226,   227,     0,   228,
   229,     0,   230,   231,     0,   232,     0,     0,   233,   234,
   235,   236,   237,     0,   238,   239,     0,     0,   240,   241,
   242,   243,   244,   245,   246,   247,   248,     0,     0,     0,
     0,   249,     0,   250,   251,     0,   384,   252,   253,     0,
   254,     0,   255,     0,   256,   257,   258,   259,   260,   261,
     0,   262,   263,   264,   265,   266,     0,     0,   267,   268,
   269,   270,   271,     0,     0,   272,     0,   273,   274,     0,
     0,   276,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   168,   169,   170,   171,   172,   173,   174,   175,
   176,     0,   177,  1493,   178,   179,   180,   181,   182,   183,
   184,   185,   186,   187,     0,   188,     0,   189,   190,   191,
   192,   193,     0,   194,   195,   196,   197,   198,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   199,
   200,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   201,     0,     0,     0,     0,     0,     0,   202,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   203,     0,     0,
   204,     0,     0,     0,     0,     0,     0,     0,   205,   206,
     0,     0,     0,     0,     0,   207,   208,   209,     0,     0,
     0,     0,   210,     0,     0,     0,     0,     0,   211,     0,
   212,   213,     0,     0,     0,     0,     0,     0,     0,   214,
   215,     0,     0,   216,     0,   217,     0,     0,     0,   218,
   219,    73,     0,     0,     0,     0,     0,   220,   221,   222,
   223,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   224,
   225,   226,   227,     0,   228,   229,     0,   230,   231,     0,
   232,     0,     0,   233,   234,   235,   236,   237,     0,   238,
   239,     0,     0,   240,   241,   242,   243,   244,   245,   246,
   247,   248,     0,     0,     0,     0,   249,     0,   250,   251,
     0,     0,   252,   253,     0,   254,     0,   255,     0,   256,
   257,   258,   259,   260,   261,     0,   262,   263,   264,   265,
   266,     0,     0,   267,   268,   269,   270,   271,     0,     0,
   272,     0,   273,   274,     0,     0,   276,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   168,   169,   170,
   171,   172,   173,   174,   175,   176,     0,   177,    91,   178,
   179,   180,   181,   182,   183,   184,   185,   186,   187,     0,
   188,     0,   189,   190,   191,   192,   193,     0,   194,   195,
   196,   197,   198,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   199,   200,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   201,     0,     0,
     0,     0,     0,     0,   202,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   203,     0,     0,   204,     0,     0,     0,     0,
     0,     0,     0,   205,   206,     0,     0,     0,     0,     0,
   207,   208,   209,     0,     0,     0,     0,   210,     0,     0,
     0,     0,     0,   211,     0,   212,   213,     0,     0,     0,
     0,     0,     0,     0,   214,   215,     0,     0,   216,     0,
   217,     0,     0,     0,   218,   219,     0,     0,     0,     0,
     0,     0,   220,   221,   222,   223,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   224,   225,   226,   227,     0,   228,
   229,     0,   230,   231,     0,   232,     0,     0,   233,   234,
   235,   236,   237,     0,   238,   239,     0,     0,   240,   241,
   242,   243,   244,   245,   246,   247,   248,     0,     0,     0,
     0,   249,     0,   250,   251,     0,     0,   252,   253,     0,
   254,     0,   255,     0,   256,   257,   258,   259,   260,   261,
     0,   262,   263,   264,   265,   266,  1592,     0,   267,   268,
   269,   270,   271,     0,     0,   272,     0,   273,   274,   275,
   494,   276,     0,     0,    25,     0,    26,     0,   468,   469,
   470,   471,  1593,   473,   474,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   201,     0,     0,   975,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
   465,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,   466,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   220,   221,   222,   223,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,     0,   467,   276,
     0,     0,     0,     0,     0,     0,   468,   469,   470,   471,
   472,   473,   474,   168,   169,   170,   171,   172,   173,   174,
   175,   176,     0,   177,     0,   178,   179,   180,   181,   182,
   183,   184,   185,   186,   187,     0,   188,     0,   189,   190,
   191,   192,   193,     0,   194,   195,   196,   197,   198,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   199,   200,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   201,     0,     0,     0,     0,     0,     0,
   202,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,   204,     0,     0,     0,     0,     0,     0,   465,   205,
   206,     0,     0,     0,     0,     0,   207,   208,   209,     0,
     0,     0,     0,   210,     0,     0,     0,     0,     0,   211,
     0,   212,   213,     0,     0,     0,     0,     0,     0,     0,
   214,   215,   466,     0,   216,     0,   217,     0,     0,     0,
   218,   219,     0,     0,     0,     0,     0,     0,   220,   221,
   222,   223,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   224,   225,   226,   227,     0,   228,   229,     0,   230,   231,
     0,   232,     0,     0,   233,   234,   235,   236,   237,     0,
   238,   239,     0,     0,   240,   241,   242,   243,   244,   245,
   246,   247,   248,     0,     0,     0,     0,   249,     0,   250,
   251,     0,     0,   252,   253,     0,   254,     0,   255,     0,
   256,   257,   258,   259,   260,   261,     0,   262,   263,   264,
   265,   266,     0,     0,   267,   268,   269,   270,   271,     0,
     0,   272,     0,   273,   274,     0,   467,   276,     0,     0,
     0,     0,     0,     0,   468,   469,   470,   471,   472,   473,
   474,   168,   169,   170,   171,   172,   173,   174,   175,   176,
     0,   177,     0,   178,   179,   180,   181,   182,   183,   184,
   185,   186,   187,     0,   188,     0,   189,   190,   191,   192,
   193,     0,   194,   195,   196,   197,   198,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   199,   200,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   383,     0,     0,     0,     0,
     0,   201,     0,     0,     0,     0,     0,     0,   202,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   203,     0,     0,   204,
     0,     0,     0,     0,     0,     0,     0,   205,   206,     0,
     0,     0,     0,     0,   207,   208,   209,     0,     0,     0,
     0,   210,     0,     0,     0,     0,     0,   211,     0,   212,
   213,     0,     0,     0,     0,     0,     0,     0,   214,   215,
     0,     0,   216,     0,   217,     0,     0,     0,   218,   219,
     0,     0,     0,     0,     0,     0,   220,   221,   222,   223,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   224,   225,
   226,   227,     0,   228,   229,     0,   230,   231,     0,   232,
     0,     0,   233,   234,   235,   236,   237,     0,   238,   239,
     0,     0,   240,   241,   242,   243,   244,   245,   246,   247,
   248,     0,     0,     0,     0,   249,     0,   250,   251,     0,
   384,   252,   253,     0,   254,     0,   255,     0,   256,   257,
   258,   259,   260,   261,     0,   262,   263,   264,   265,   266,
     0,     0,   267,   268,   269,   270,   271,     0,     0,   272,
     0,   273,   274,     0,     0,   276,     0,     0,     0,   582,
     0,     0,     0,     0,     0,     0,     0,   874,   168,   169,
   170,   171,   172,   173,   174,   175,   176,     0,   177,     0,
   178,   179,   180,   181,   182,   183,   184,   185,   186,   187,
     0,   188,     0,   189,   190,   191,   192,   193,     0,   194,
   195,   196,   197,   198,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   199,   200,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   383,     0,     0,     0,     0,     0,   201,     0,
     0,     0,     0,     0,     0,   202,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   203,     0,     0,   204,     0,     0,     0,
     0,     0,     0,     0,   205,   206,     0,     0,     0,     0,
     0,   207,   208,   209,     0,     0,     0,     0,   210,     0,
     0,     0,     0,     0,   211,     0,   212,   213,     0,     0,
     0,     0,     0,     0,     0,   214,   215,     0,     0,   216,
     0,   217,     0,     0,     0,   218,   219,     0,     0,     0,
     0,     0,     0,   220,   221,   222,   223,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   224,   225,   226,   227,     0,
   228,   229,     0,   230,   231,     0,   232,     0,     0,   233,
   234,   235,   236,   237,     0,   238,   239,     0,     0,   240,
   241,   242,   243,   244,   245,   246,   247,   248,     0,     0,
     0,     0,   249,     0,   250,   251,     0,   384,   252,   253,
     0,   254,     0,   255,     0,   256,   257,   258,   259,   260,
   261,     0,   262,   263,   264,   265,   266,     0,     0,   267,
   268,   269,   270,   271,     0,     0,   272,     0,   273,   274,
     0,     0,   276,   168,   169,   170,   171,   172,   173,   174,
   175,   176,     0,   177,   398,   178,   179,   180,   181,   182,
   183,   184,   185,   186,   187,     0,   188,     0,   189,   190,
   191,   192,   193,     0,   194,   195,   196,   197,   198,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   199,   200,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   201,     0,     0,     0,     0,     0,     0,
   202,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,   204,     0,     0,     0,     0,     0,     0,     0,   205,
   206,     0,     0,     0,     0,     0,   207,   208,   209,     0,
     0,     0,     0,   210,     0,     0,     0,     0,     0,   211,
     0,   212,   213,     0,     0,     0,     0,     0,     0,     0,
   214,   215,     0,     0,   216,     0,   217,     0,     0,     0,
   218,   219,     0,     0,     0,     0,     0,     0,   220,   221,
   222,   223,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   224,   225,   226,   227,     0,   228,   229,     0,   230,   231,
     0,   232,     0,     0,   233,   234,   235,   236,   237,     0,
   238,   239,     0,     0,   240,   241,   242,   243,   244,   245,
   246,   247,   248,     0,     0,     0,     0,   249,     0,   250,
   251,     0,     0,   252,   253,     0,   254,     0,   255,     0,
   256,   257,   258,   259,   260,   261,     0,   262,   263,   264,
   265,   266,     0,     0,   267,   268,   269,   270,   271,     0,
     0,   272,     0,   273,   274,     0,     0,   276,   168,   169,
   170,   171,   172,   173,   174,   175,   176,     0,   177,   728,
   178,   179,   180,   181,   182,   183,   184,   185,   186,   187,
     0,   188,     0,   189,   190,   191,   192,   193,     0,   194,
   195,   196,   197,   198,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   199,   200,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   201,     0,
     0,     0,     0,     0,     0,   202,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   203,     0,     0,   204,     0,     0,     0,
     0,     0,     0,     0,   205,   206,     0,     0,     0,     0,
     0,   207,   208,   209,     0,     0,     0,     0,   210,     0,
     0,     0,     0,     0,   211,     0,   212,   213,     0,     0,
     0,     0,     0,     0,     0,   214,   215,     0,     0,   216,
     0,   217,     0,     0,     0,   218,   219,     0,     0,     0,
     0,     0,     0,   220,   221,   222,   223,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   224,   225,   226,   227,     0,
   228,   229,     0,   230,   231,     0,   232,     0,     0,   233,
   234,   235,   236,   237,     0,   238,   239,     0,     0,   240,
   241,   242,   243,   244,   245,   246,   247,   248,     0,     0,
     0,     0,   249,     0,   250,   251,     0,     0,   252,   253,
     0,   254,     0,   255,     0,   256,   257,   258,   259,   260,
   261,     0,   262,   263,   264,   265,   266,     0,     0,   267,
   268,   269,   270,   271,     0,     0,   272,     0,   273,   274,
     0,     0,   276,   168,   169,   170,   171,   172,   173,   174,
   175,   176,     0,   177,  1192,   178,   179,   180,   181,   182,
   183,   184,   185,   186,   187,     0,   188,     0,   189,   190,
   191,   192,   193,     0,   194,   195,   196,   197,   198,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   199,   200,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   201,     0,     0,     0,     0,     0,     0,
   202,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,   204,     0,     0,     0,     0,     0,     0,     0,   205,
   206,     0,     0,     0,     0,     0,   207,   208,   209,     0,
     0,     0,     0,   210,     0,     0,     0,     0,     0,   211,
     0,   212,   213,     0,     0,     0,     0,     0,     0,     0,
   214,   215,     0,     0,   216,     0,   217,     0,     0,     0,
   218,   219,     0,     0,     0,     0,     0,     0,   220,   221,
   222,   223,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   224,   225,   226,   227,     0,   228,   229,     0,   230,   231,
     0,   232,     0,     0,   233,   234,   235,   236,   237,     0,
   238,   239,     0,     0,   240,   241,   242,   243,   244,   245,
   246,   247,   248,     0,     0,     0,     0,   249,     0,   250,
   251,     0,     0,   252,   253,     0,   254,     0,   255,     0,
   256,   257,   258,   259,   260,   261,     0,   262,   263,   264,
   265,   266,     0,     0,   267,   268,   269,   270,   271,     0,
     0,   272,     0,   273,   274,     0,     0,   276,   168,   169,
   170,   171,   172,   173,   174,   175,   176,     0,   177,  1505,
   178,   179,   180,   181,   182,   183,   184,   185,   186,   187,
     0,   188,     0,   189,   190,   191,   192,   193,     0,   194,
   195,   196,   197,   198,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   199,   200,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   201,     0,
     0,     0,     0,     0,     0,   202,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   203,     0,     0,   204,     0,     0,     0,
     0,     0,     0,     0,   205,   206,     0,     0,     0,     0,
     0,   207,   208,   209,     0,     0,     0,     0,   210,     0,
     0,     0,     0,     0,   211,     0,   212,   213,     0,     0,
     0,     0,     0,     0,     0,   214,   215,     0,     0,   216,
     0,   217,     0,     0,     0,   218,   219,     0,     0,     0,
     0,     0,     0,   220,   221,   222,   223,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   224,   225,   226,   227,     0,
   228,   229,     0,   230,   231,     0,   232,     0,     0,   233,
   234,   235,   236,   237,     0,   238,   239,     0,     0,   240,
   241,   242,   243,   244,   245,   246,   247,   248,     0,     0,
     0,     0,   249,     0,   250,   251,     0,     0,   252,   253,
     0,   254,     0,   255,     0,   256,   257,   258,   259,   260,
   261,     0,   262,   263,   264,   265,   266,     0,     0,   267,
   268,   269,   270,   271,     0,     0,   272,     0,   273,   274,
     0,     0,   276,   784,   785,   786,   787,   788,   789,   790,
   791,   792,     0,   793,  1797,   794,   795,   796,   797,   798,
   799,   800,   801,   802,   803,     0,   804,     0,   805,   806,
   807,   808,   809,     0,   810,   811,   812,   813,   814,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   551,   552,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   201,   558,     0,     0,     0,     0,     0,
   815,     0,     0,     0,     0,     0,     0,     0,   562,     0,
   563,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,     0,     0,     0,   564,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   208,   209,     0,
   565,     0,   566,     0,     0,     0,     0,     0,   570,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   219,    73,     0,     0,     0,     0,     0,   816,   817,
     0,     0,     0,     0,     0,     0,   576,     0,     0,     0,
     0,     0,     0,   578,     0,     0,     0,     0,     0,     0,
   224,     0,     0,   818,     0,   784,   785,   786,   787,   788,
   789,   790,   791,   792,     0,   793,     0,   794,   795,   796,
   797,   798,   799,   800,   801,   802,   803,     0,   804,     0,
   805,   806,   807,   808,   809,     0,   810,   811,   812,   813,
   814,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   580,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   274,   275,     0,   276,   551,   552,
    25,   582,    26,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   201,   558,     0,     0,     0,
     0,     0,   815,     0,     0,     0,     0,     0,     0,     0,
   562,     0,   563,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,     0,     0,     0,   564,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   208,
   209,     0,   565,     0,   566,     0,     0,     0,     0,     0,
   570,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   219,     0,     0,     0,     0,     0,     0,
   816,   817,     0,     0,     0,     0,     0,     0,   576,     0,
     0,     0,     0,     0,     0,   578,     0,     0,     0,     0,
     0,     0,   224,     0,     0,   818,     0,     0,   168,   169,
   170,   171,   172,   173,   174,   175,   176,     0,   177,     0,
   178,   179,   180,   181,   182,   183,   184,   185,   186,   187,
     0,   188,     0,   189,   190,   191,   192,   193,     0,   194,
   195,   196,   197,   198,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   580,   199,   200,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   274,   275,     0,   276,
     0,     0,    25,   582,    26,     0,     0,     0,     0,     0,
     0,     0,   383,     0,     0,     0,     0,     0,   201,     0,
     0,     0,     0,     0,     0,   202,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   203,     0,     0,   204,     0,     0,     0,
     0,     0,     0,     0,   205,   206,     0,     0,     0,     0,
     0,   207,   208,   209,     0,     0,     0,     0,   210,     0,
     0,     0,     0,     0,   211,     0,   212,   213,     0,     0,
     0,     0,     0,     0,     0,   214,   215,     0,     0,   216,
     0,   217,     0,     0,     0,   218,   219,     0,     0,     0,
     0,     0,     0,   220,   221,   222,   223,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   224,   225,   226,   227,     0,
   228,   229,     0,   230,   231,     0,   232,     0,     0,   233,
   234,   235,   236,   237,     0,   238,   239,     0,     0,   240,
   241,   242,   243,   244,   245,   246,   247,   248,     0,     0,
     0,     0,   249,     0,   250,   251,     0,   384,   252,   253,
     0,   254,     0,   255,     0,   256,   257,   258,   259,   260,
   261,     0,   262,   263,   264,   265,   266,     0,     0,   267,
   268,   269,   270,   271,     0,     0,   272,     0,   273,   274,
     0,     0,   276,     0,     0,     0,   582,   168,   169,   170,
   171,   172,   173,   174,   175,   176,     0,   177,     0,   178,
   179,   180,   181,   182,   183,   184,   185,   186,   187,     0,
   188,     0,   189,   190,   191,   192,   193,     0,   194,   195,
   196,   197,   198,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   199,   200,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   201,     0,     0,
     0,     0,     0,     0,   202,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   203,     0,     0,   204,     0,     0,     0,     0,
     0,     0,     0,   205,   206,     0,     0,     0,     0,     0,
   207,   208,   209,     0,     0,     0,     0,   210,     0,     0,
     0,     0,     0,   211,     0,   212,   213,     0,     0,     0,
     0,     0,     0,     0,   214,   215,     0,     0,   216,     0,
   217,     0,     0,     0,   218,   219,     0,     0,     0,     0,
     0,     0,   220,   221,   222,   223,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   224,   225,   226,   227,     0,   228,
   229,     0,   230,   231,     0,   232,     0,     0,   233,   234,
   235,   236,   237,     0,   238,   239,     0,     0,   240,   241,
   242,   243,   244,   245,   246,   247,   248,     0,     0,     0,
     0,   249,     0,   250,   251,     0,     0,   252,   253,     0,
   254,     0,   255,     0,   256,   257,   258,   259,   260,   261,
     0,   262,   263,   264,   265,   266,     0,     0,   267,   268,
   269,   270,   271,     0,     0,   272,     0,   273,   274,     0,
     0,   276,     0,     0,    25,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,   295,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   296,
     0,     0,     0,     0,     0,   201,     0,     0,   297,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   220,   221,   222,   223,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,   275,     0,   276,
   277,   168,   169,   170,   171,   172,   173,   174,   175,   176,
     0,   177,     0,   178,   179,   180,   181,   182,   183,   184,
   185,   186,   187,     0,   188,     0,   189,   190,   191,   192,
   193,     0,   194,   195,   196,   197,   198,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   199,   200,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   201,     0,     0,   432,     0,     0,     0,   202,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   203,     0,     0,   204,
     0,     0,     0,     0,     0,     0,     0,   205,   206,     0,
     0,     0,     0,     0,   207,   208,   209,     0,     0,     0,
     0,   210,     0,     0,     0,     0,     0,   211,     0,   212,
   213,     0,     0,     0,     0,     0,     0,     0,   214,   215,
     0,     0,   216,     0,   217,     0,     0,     0,   218,   219,
     0,     0,     0,     0,     0,     0,   220,   221,   222,   223,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   224,   225,
   226,   227,     0,   228,   229,     0,   230,   231,     0,   232,
     0,     0,   233,   234,   235,   236,   237,     0,   238,   239,
     0,     0,   240,   241,   242,   243,   244,   245,   246,   247,
   248,     0,     0,     0,     0,   249,     0,   250,   251,     0,
     0,   252,   253,     0,   254,     0,   255,     0,   256,   257,
   258,   259,   260,   261,     0,   262,   263,   264,   265,   266,
     0,     0,   267,   268,   269,   270,   271,     0,     0,   272,
     0,   273,   274,   275,     0,   276,   277,   168,   169,   170,
   171,   172,   173,   174,   175,   176,     0,   177,     0,   178,
   179,   180,   181,   182,   183,   184,   185,   186,   187,     0,
   188,     0,   189,   190,   191,   192,   193,     0,   194,   195,
   196,   197,   198,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   199,   200,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   201,     0,     0,
   297,     0,     0,     0,   202,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   203,     0,     0,   204,     0,     0,     0,     0,
     0,     0,     0,   205,   206,     0,     0,     0,     0,     0,
   207,   208,   209,     0,     0,     0,     0,   210,     0,     0,
     0,     0,     0,   211,     0,   212,   213,     0,     0,     0,
     0,     0,     0,     0,   214,   215,     0,     0,   216,     0,
   217,     0,     0,     0,   218,   219,     0,     0,     0,     0,
     0,     0,   220,   221,   222,   223,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   224,   225,   226,   227,     0,   228,
   229,     0,   230,   231,     0,   232,     0,     0,   233,   234,
   235,   236,   237,     0,   238,   239,     0,     0,   240,   241,
   242,   243,   244,   245,   246,   247,   248,     0,     0,     0,
     0,   249,     0,   250,   251,     0,     0,   252,   253,     0,
   254,     0,   255,     0,   256,   257,   258,   259,   260,   261,
     0,   262,   263,   264,   265,   266,     0,     0,   267,   268,
   269,   270,   271,     0,     0,   272,     0,   273,   274,   275,
     0,   276,   277,   168,   169,   170,   171,   172,   173,   174,
   175,   176,     0,   177,     0,   178,   179,   180,   181,   182,
   183,   184,   185,   186,   187,     0,   188,     0,   189,   190,
   191,   192,   193,     0,   194,   195,   196,   197,   198,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   199,   200,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   201,     0,     0,     0,     0,     0,     0,
   202,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   203,     0,
     0,   204,     0,     0,     0,     0,     0,     0,     0,   205,
   206,     0,     0,     0,     0,     0,   207,   208,   209,     0,
     0,     0,     0,   210,     0,     0,     0,     0,     0,   211,
     0,   212,   213,     0,     0,     0,     0,     0,     0,     0,
   214,   215,     0,     0,   216,     0,   217,     0,     0,     0,
   218,   219,     0,     0,     0,     0,     0,     0,   220,   221,
   222,   223,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   224,   225,   226,   227,     0,   228,   229,     0,   230,   231,
     0,   232,     0,     0,   233,   234,   235,   236,   237,     0,
   238,   239,     0,     0,   240,   241,   242,   243,   244,   245,
   246,   247,   248,     0,     0,     0,     0,   249,     0,   250,
   251,     0,     0,   252,   253,     0,   254,     0,   255,     0,
   256,   257,   258,   259,   260,   261,     0,   262,   263,   264,
   265,   266,     0,     0,   267,   268,   269,   270,   271,     0,
     0,   272,     0,   273,   274,   275,     0,   276,   277,   168,
   169,   170,   171,   172,   173,   174,   175,   176,     0,   177,
     0,   178,   179,   180,   181,   182,   183,   184,   185,   186,
   187,     0,   188,     0,   189,   190,   191,   192,   193,     0,
   194,   195,   196,   197,   198,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   199,   200,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   201,
     0,     0,     0,     0,     0,     0,   202,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   203,     0,     0,   204,     0,     0,
     0,     0,     0,     0,     0,   205,   206,     0,     0,     0,
     0,     0,   207,   208,   209,     0,     0,     0,     0,   210,
     0,     0,     0,     0,     0,   211,     0,   212,   213,     0,
     0,     0,     0,     0,     0,     0,   214,   215,     0,     0,
   216,     0,   217,     0,     0,     0,   218,   219,     0,     0,
     0,     0,     0,     0,   220,   221,   222,   223,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   224,   225,   226,   227,
     0,   228,   229,     0,   230,   231,     0,   232,     0,     0,
   233,   234,   235,   236,   237,     0,   238,   239,     0,     0,
   240,   241,   242,   243,   244,   245,   246,   247,   248,     0,
     0,     0,     0,   249,     0,   250,   251,     0,     0,   252,
   253,     0,   254,     0,   255,     0,   256,   257,   258,   259,
   260,   261,     0,   262,   263,   264,   265,   266,     0,     0,
   267,   268,   269,   270,   271,     0,     0,   272,     0,   273,
   274,     0,     0,   276,   277,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1080,     0,     0,     0,
     0,     0,  1081,     0,     0,     0,  1082,     0,  1083,  1084,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,   202,     0,  1085,  1086,     0,     0,     0,     0,
  1087,     0,     0,     0,  1088,     0,     0,     0,  1089,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,  1090,     0,
     0,   211,     0,   212,   213,     0,  1091,     0,     0,  1092,
  1093,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,  1094,     0,  1095,
   220,   221,   222,   223,     0,     0,  1096,     0,  1097,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1098,     0,
     0,     0,   224,   225,   226,   227,  1099,   228,   229,  1100,
   230,   231,  1101,   232,  1102,  1103,   233,   234,   235,   236,
   237,  1104,   238,   239,  1105,  1106,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,  1107,     0,  1108,   249,
  1109,   250,   251,  1110,  1111,   252,   253,  1112,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,  1113,   262,
   263,   264,   265,   266,  1114,  1115,   267,   268,   269,   270,
   271,     0,  1116,   272,  1117,   273,   274,     0,     0,   276,
   168,   169,   170,   171,   172,   173,   174,   175,   176,     0,
   177,     0,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,     0,   188,     0,   189,   190,   191,   192,   193,
     0,   194,   195,   196,   197,   198,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   551,   552,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,   953,     0,     0,     0,     0,     0,   954,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   955,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,     0,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,   565,     0,   566,
   210,     0,     0,     0,     0,   956,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   220,   221,   222,   223,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   578,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   227,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,     0,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,     0,     0,   276,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1255,     0,     0,     0,     0,     0,  1290,     0,     0,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1257,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,  1258,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   220,   221,   222,   223,     0,     0,     0,     0,     0,     0,
  1259,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,     0,     0,   276,
   168,   169,   170,   171,   172,   378,   174,   175,   176,     0,
   177,     0,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,     0,   188,     0,   189,   190,   191,   192,   193,
     0,   194,   195,   196,   197,   198,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,     0,     0,     0,     0,     0,     0,   202,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,     0,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,   379,     0,     0,     0,
   210,     0,     0,     0,     0,     0,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   380,   221,   222,   223,     0,
     0,   381,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   227,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,     0,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,     0,     0,   276,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   383,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   220,   221,   222,   223,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,   384,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,     0,     0,   276,
   168,   169,   170,   171,   172,   173,   174,   175,   176,     0,
   177,     0,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,     0,   188,     0,   189,   190,   191,   192,   193,
     0,   194,   195,   196,   197,   198,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,     0,     0,     0,     0,     0,     0,   202,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1312,     0,   203,     0,     0,   204,     0,
     0,     0,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,     0,     0,     0,
   210,     0,     0,     0,     0,     0,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
  1313,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   220,   221,   222,   223,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   227,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,     0,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,     0,     0,   276,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   406,   221,   222,   223,     0,     0,   407,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,     0,     0,   276,
   168,   169,   170,   171,   172,   173,   174,   175,   176,     0,
   177,     0,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,     0,   188,     0,   189,   190,   191,   192,   193,
     0,   194,   195,   196,   197,   198,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,     0,     0,     0,     0,     0,     0,   202,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,     0,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,     0,     0,     0,
   210,     0,     0,     0,     0,     0,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   409,   221,   222,   223,     0,
     0,   410,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   227,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,     0,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,     0,     0,   276,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   220,   221,   222,   223,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,   984,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,     0,     0,   276,
   168,   169,   170,   171,   172,   173,   174,   175,   176,     0,
   177,     0,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,     0,   188,     0,   189,   190,   191,   192,   193,
     0,   194,   195,   196,   197,   198,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,     0,     0,     0,     0,     0,     0,   202,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,     0,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,     0,     0,     0,
   210,     0,     0,     0,     0,     0,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   220,   221,   222,   223,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   227,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,  1610,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,     0,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,     0,     0,   276,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,  1792,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   220,   221,   222,   223,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,     0,     0,   276,
   168,   169,   170,   171,   172,   173,   174,   175,   176,     0,
   177,     0,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,     0,   188,     0,   189,   190,   191,   192,   193,
     0,   194,   195,   196,   197,   198,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,     0,     0,     0,     0,     0,     0,   202,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,     0,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,     0,     0,     0,
   210,     0,     0,     0,     0,     0,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   220,   221,   222,   223,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   227,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,     0,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,     0,     0,   276,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,     0,     0,   216,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   220,   221,   222,   223,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   337,   269,   270,
   271,     0,     0,   272,     0,   273,   274,     0,     0,   276,
   168,   169,   699,   171,   172,   173,   174,   175,   176,     0,
   177,     0,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,     0,   188,     0,   189,   190,   191,   192,   193,
     0,   194,   195,   196,   197,   198,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   199,   200,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,     0,     0,     0,     0,     0,     0,   202,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,   204,     0,
     0,     0,     0,     0,     0,     0,   205,   206,     0,     0,
     0,     0,     0,   207,   208,   209,     0,     0,     0,     0,
   210,     0,     0,     0,     0,     0,   211,     0,   212,   213,
     0,     0,     0,     0,     0,     0,     0,   214,   215,     0,
     0,   216,     0,   217,     0,     0,     0,   218,   219,     0,
     0,     0,     0,     0,     0,   220,   221,   222,   223,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   224,   225,   226,
   227,     0,   228,   229,     0,   230,   231,     0,   232,     0,
     0,   233,   234,   235,   236,   237,     0,   238,   239,     0,
     0,   240,   241,   242,   243,   244,   245,   246,   247,   248,
     0,     0,     0,     0,   249,     0,   250,   251,     0,     0,
   252,   253,     0,   254,     0,   255,     0,   256,   257,   258,
   259,   260,   261,     0,   262,   263,   264,   265,   266,     0,
     0,   267,   268,   269,   270,   271,     0,     0,   272,     0,
   273,   274,     0,     0,   276,   168,   169,   170,   171,   172,
   173,   174,   175,   176,     0,   177,     0,   178,   179,   180,
   181,   182,   183,   184,   185,   186,   187,     0,   188,     0,
   189,   190,   191,   192,   193,     0,   194,   195,   196,   197,
   198,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   199,   200,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   203,     0,     0,   204,     0,     0,     0,     0,     0,     0,
     0,   205,   206,     0,     0,     0,     0,     0,   207,   208,
   209,     0,     0,     0,     0,   210,     0,     0,     0,     0,
     0,   211,     0,   212,   213,     0,     0,     0,     0,     0,
     0,     0,   214,   215,     0,     0,  1202,     0,   217,     0,
     0,     0,   218,   219,     0,     0,     0,     0,     0,     0,
   220,   221,   222,   223,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   224,   225,   226,   227,     0,   228,   229,     0,
   230,   231,     0,   232,     0,     0,   233,   234,   235,   236,
   237,     0,   238,   239,     0,     0,   240,   241,   242,   243,
   244,   245,   246,   247,   248,     0,     0,     0,     0,   249,
     0,   250,   251,     0,     0,   252,   253,     0,   254,     0,
   255,     0,   256,   257,   258,   259,   260,   261,     0,   262,
   263,   264,   265,   266,     0,     0,   267,   268,   269,   270,
   271,     0,     0,   272,     0,   273,   274,     0,     0,   276,
   784,   785,   786,   787,   788,   789,   790,   791,   792,     0,
   793,     0,   794,   795,   796,   797,   798,   799,   800,   801,
   802,   803,     0,   804,     0,   805,   806,   807,   808,   809,
     0,   810,   811,   812,   813,   814,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   551,   552,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   201,   558,     0,     0,     0,     0,     0,   815,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   563,     0,     0,
     0,     0,     0,     0,     0,   203,     0,     0,     0,     0,
     0,   564,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   208,   209,     0,   565,     0,   566,
     0,     0,     0,     0,     0,   570,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   219,     0,
     0,     0,     0,     0,     0,   816,   817,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   578,     0,     0,     0,     0,     0,     0,   224,     0,     0,
   818,     0,   784,   785,   786,   787,   788,   789,   790,   791,
   792,     0,   793,     0,   794,   795,   796,   797,   798,   799,
   800,   801,   802,   803,     0,   804,     0,   805,   806,   807,
   808,   809,     0,   810,   811,   812,   813,   814,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1588,     0,   580,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   274,     0,     0,   276,   551,   552,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   201,   558,     0,     0,     0,     0,     0,   815,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   563,
     0,     0,     0,     0,     0,     0,     0,   203,     0,     0,
     0,     0,     0,   564,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   208,   209,     0,   565,
     0,   566,     0,     0,     0,     0,     0,   570,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   219,     0,     0,     0,     0,     0,     0,   816,   817,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   578,     0,     0,     0,     0,     0,     0,   224,
     0,     0,   818,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   580,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   274,     0,     0,   276
};

static const short yycheck[] = {     1,
    91,     1,     1,   515,   629,   352,   335,   757,    49,    56,
   312,    52,   438,    54,   992,    50,   166,   349,  1499,    60,
   322,    49,    63,   935,    52,    49,  1493,  1272,  1433,    76,
   780,  1249,   327,    74,    75,  1528,  1556,    78,  2017,   341,
   601,    82,    83,  1813,  1246,    59,    87,    88,    89,     4,
    42,   346,    27,   837,    19,   713,    84,    12,   113,   435,
    17,    59,    42,    42,    42,    33,    19,    63,  1790,   441,
    66,    96,    40,   113,    29,    43,   552,    59,    46,   111,
    35,    36,    63,    51,   309,    53,    54,    96,   125,    96,
   566,    60,    63,  1131,  1132,  1133,  1134,    30,   839,    32,
    92,    96,   182,   172,   172,   121,   172,    18,    97,   191,
   860,  1149,    92,    92,    92,    59,    97,   176,    59,    96,
   191,    76,   396,   192,  1351,  1029,  1030,  1031,  1032,   249,
   258,   101,  2317,   122,    59,    96,    40,   265,   125,    43,
   135,   122,    46,   823,   280,   100,   150,    51,    23,    53,
    54,   191,    95,   102,   170,   110,    41,    42,  1385,    44,
    45,   297,    47,    48,    73,    50,   513,    52,   191,   516,
    55,    56,    57,    58,     6,  1056,   274,     9,   298,  1060,
  1061,  1062,  1063,    15,    16,  2370,   147,   191,   749,    59,
  2169,   172,    97,   280,   210,   172,   708,   166,  2039,    31,
   189,    59,    34,   173,   284,   479,   274,   114,   189,   277,
   295,  1438,   145,   300,   299,   192,   298,   122,   300,   278,
    54,  2062,  2063,  2064,  2065,  2066,   451,   298,   214,    63,
   299,   299,   187,   299,   191,   144,    59,   113,   193,   208,
    62,   184,    64,   298,    97,   377,   287,   196,   113,   156,
   275,   274,   624,    59,   277,   224,   199,   243,   298,   287,
   292,   298,   276,   287,   173,   210,   275,   172,   275,  1991,
   311,   285,   286,   287,   288,   289,   290,  2256,   259,   101,
   275,   272,   323,   324,   189,    63,   327,   328,    75,   330,
   331,   289,   290,  1043,   335,    82,    83,   773,   339,   340,
    87,   677,   343,   299,   345,   346,   347,   289,   290,   280,
   281,   352,   353,   278,   875,  1835,  1836,   182,   879,   274,
   355,    99,   299,   298,  1542,   278,  1544,   418,   191,  1734,
  2309,   372,  2311,   290,   302,  1537,   189,   378,   764,   292,
    59,   228,   247,   254,   297,   289,   290,   302,   289,   290,
   378,   379,   393,    97,  1042,   396,  1044,  1045,  2199,   275,
   362,  1399,   403,   274,   289,   290,   277,  2208,  2209,  2210,
  2211,  2212,  2213,  2214,  2215,  2216,  1056,   280,   122,   426,
  1060,  1061,  1062,  1063,  2154,   288,   427,   219,   191,   274,
  1131,  1132,  1133,  1134,   299,   436,  1877,  1435,   302,   175,
   435,   442,   404,   395,   445,   440,    78,   448,  1149,  1447,
  1448,  1449,  1450,  1451,  1452,   395,   395,   395,   276,   289,
   290,   280,   463,    97,   198,   215,   284,   285,   286,   287,
   288,   289,   290,   255,   995,   298,   191,   300,   479,    78,
   112,   300,   300,    82,   292,   189,  1350,   175,   122,   297,
   491,   137,   104,   276,   244,   587,  1360,  1361,  1362,  1363,
  1364,  1365,   190,    97,   287,   288,   289,   290,   980,   497,
   276,   123,   513,   112,  1035,   516,   517,   300,  1352,   164,
  2321,   287,   288,   289,   290,   171,   292,   159,   122,   323,
   324,   297,    20,    21,   920,   298,   330,   300,   172,   113,
    28,   275,    27,   247,   189,   119,  1067,   509,   549,   343,
    24,   345,  1386,    70,  1298,   189,   188,  1398,    59,   353,
   159,  1402,   296,  1404,   311,    33,  1407,  1408,  1409,  1410,
  1411,  1412,  1413,  1414,   191,   187,  1417,   276,   172,   280,
    78,  1325,    59,   298,   331,   300,  1951,   280,    59,   188,
    67,   642,   339,   644,    65,   189,   297,   276,    96,   108,
   588,   278,   119,  2083,   297,  1439,  1191,   275,   287,   288,
   289,   290,   184,   292,    59,  1359,  1137,   134,   297,    59,
    65,  1206,   623,    59,   625,   372,   627,   199,   296,    65,
   377,   619,   191,   621,   622,   292,   637,     4,   147,  1817,
   297,  1819,  1352,  1848,   651,    12,   393,   145,   442,   656,
   657,   445,    59,  1397,   448,   233,   403,  1367,  1368,  1277,
   915,   159,    29,  1281,   275,   259,  1376,   165,    35,    36,
   104,   249,   275,   126,   675,   118,  1386,   186,   940,   119,
  1678,   298,   677,   300,   280,   296,   275,   675,   300,   123,
   188,   675,   743,   296,   137,   113,   697,   491,  1399,  2152,
   299,   297,  1446,   293,    13,  1353,  1416,   296,   298,    76,
   672,  1583,   713,  2140,   266,   267,   463,    70,   719,   126,
   154,    26,   108,   275,    59,   726,   727,   175,   171,  1439,
   718,   732,    67,   100,  1435,   736,   737,  2217,   298,  1387,
   300,   205,   743,   110,  1454,   209,  1447,  1448,  1449,  1450,
  1451,  1452,   108,   187,  2195,   275,   277,   278,  1398,   145,
  1758,   147,  1402,   253,  1404,   857,   119,  1407,  1408,  1409,
  1410,  1411,  1412,  1413,  1414,   276,   296,  1417,   779,  2220,
   298,   134,   300,   284,   285,   286,   287,   288,   289,   290,
  1277,   147,  1440,   182,  1281,    63,   758,   759,   760,   276,
   186,   911,   912,   765,   228,   276,    59,   284,   285,   286,
   287,   288,   289,   290,  1558,   777,   287,   288,   289,   290,
   187,   161,   823,   292,   231,   150,   193,  2005,   297,  2007,
   186,   276,   292,   627,   275,   836,   276,   297,   839,  1680,
   276,   248,   287,   288,   289,   290,  1318,   287,   288,   289,
   290,   287,   288,   289,   290,   296,   275,   130,   859,   108,
   861,   862,   108,   275,   274,   298,   275,   277,   869,   276,
   275,   833,    59,   835,   275,   143,   623,   296,   625,   150,
   287,   288,   289,   290,   296,   274,   275,   296,   277,   278,
   637,   296,   160,   295,   296,   296,   145,  1418,   147,   145,
   992,   147,    70,   697,    95,   275,    97,   274,   296,    59,
  1908,   299,   913,  1824,   915,  1826,  1827,  1828,  1829,  1830,
    70,   298,   917,   300,   275,   275,   296,   298,    59,   300,
   296,   122,   726,   299,    65,   897,   898,   186,   732,  1649,
   186,   276,   736,   737,  1882,   296,   296,   280,   910,   282,
   910,   119,   287,   288,   289,   290,   292,    59,   275,   173,
   108,   297,    59,    65,   926,   927,   134,   150,    70,   119,
    67,   972,  1064,   275,   280,   281,   126,  1678,   979,   296,
   727,   172,    59,   275,   134,   292,  1634,  1635,  1636,   990,
   297,   201,   275,   126,   296,   996,  1382,   292,   189,   147,
  1001,    97,   297,   965,   296,   967,   968,   969,   970,  1301,
  1302,  1300,   280,   296,  1662,  1663,  1664,   119,    59,   274,
   288,   275,   277,   276,   126,   280,   122,   282,  1029,  1030,
  1031,  1032,   134,   275,   287,   288,   289,   290,   186,   298,
  1680,   300,   296,   145,   275,   292,   301,   127,   292,   126,
   297,   153,   275,   297,   296,  1056,   247,  1758,   150,  1060,
  1061,  1062,  1063,  1155,   275,   296,   274,  1159,   862,   277,
   127,  1163,   280,   296,   282,  1167,   172,   292,   201,  1171,
   295,   231,   297,  1175,   299,   296,   275,  1179,   201,   276,
   298,  1183,   300,   189,  1742,  1743,  1744,  2008,   248,  2097,
   287,   288,   289,   290,   275,  2016,   127,   296,   299,  2020,
   857,  2022,  1522,  1523,  2025,  2026,  2027,  2028,  2029,  2030,
  2031,  2032,   275,  2034,   207,   296,   276,    59,    27,   231,
  1131,  1132,  1133,  1134,   284,   285,   286,   287,   288,   289,
   290,   275,  2053,   296,  1888,   276,   248,   275,  1149,    59,
   246,   247,    63,  1897,   231,    78,   287,   288,   289,   290,
   275,    84,   296,  1984,    59,    72,  1987,   292,   296,   111,
    65,   248,   297,    96,   276,    70,   275,   656,   657,   276,
   549,   296,   284,   285,   286,   287,   288,   289,   290,  1190,
   287,   288,   289,   290,   275,   103,    93,   296,   300,   276,
   296,  1202,   292,   299,  1205,   275,  1207,  1908,  1952,   276,
   287,   288,   289,   290,    67,   296,  1217,    72,   285,   286,
   182,   118,   145,  1224,   119,  1213,   296,   289,   211,  1236,
  1322,   126,   979,   216,   198,   276,   159,    27,  1330,   134,
   137,   138,   165,   198,   227,   992,   287,   288,   289,   290,
   145,   275,   275,    67,   623,    59,   239,   240,   153,  1260,
  2171,  1262,  1263,  1264,  1265,   188,   275,   936,   937,   938,
  1271,  2015,   296,   296,   171,  1237,  1277,  1237,  1237,    61,
  1281,   264,   299,  1993,    66,  1273,   298,   296,   300,  1290,
    72,  1253,  1254,  1253,  1254,    77,    20,    21,   130,  1300,
  1301,  1302,  1303,   200,    28,   292,    67,  1308,    59,  1310,
   297,  1312,   299,  1301,  1302,   292,  1317,   299,   170,    70,
   297,  1283,   126,    42,    92,  1326,  1327,   292,   197,    48,
  1802,    50,   297,    52,   299,   298,   231,   300,   298,  1301,
  1302,  1301,  1302,   965,   276,   967,   968,   969,   970,  1350,
  1351,   150,   299,   248,  2265,   287,   288,   289,   290,  1360,
  1361,  1362,  1363,  1364,  1365,   292,   276,   298,   119,   300,
   297,   292,  1334,   299,   280,   126,   297,   287,   288,   289,
   290,   276,   194,   134,  1385,   298,  1348,   300,   158,   284,
   285,   286,   287,   288,   289,   290,  2097,  1398,  1399,   292,
   298,  1402,   300,  1404,   297,   300,  1407,  1408,  1409,  1410,
  1411,  1412,  1413,  1414,  1506,   292,  1417,  1379,  1380,   292,
   297,   292,  1943,  1217,   297,   299,   297,   231,   274,   275,
  1224,   277,   278,   274,  1435,   292,   277,  1438,   299,   280,
   297,   282,    77,  1190,   248,   299,  1447,  1448,  1449,  1450,
  1451,  1452,   299,   296,   299,   292,   292,   298,  1205,   300,
   297,   297,   292,  2207,  2049,   292,   299,   297,   292,  1431,
   297,  1265,   276,   297,  1436,  1437,   292,  2221,  2222,   299,
   231,   297,    59,   287,   288,   289,   290,    67,    65,  1490,
   299,   299,  1493,    70,   299,  1496,  1290,   248,  1499,   114,
    33,   292,   299,   292,   292,   120,   297,    40,   297,   297,
    43,  2255,   299,    46,  1308,   296,  1310,   132,    51,   299,
    53,    54,   295,   292,  1271,   276,   141,  1619,   297,   295,
    59,   133,  1533,   284,   285,   286,   287,   288,   289,   290,
   155,    70,   119,   292,   299,   292,   292,   119,   297,   126,
   297,   297,  1553,   168,   292,  1556,   298,   134,   300,   297,
  1522,  1523,  1524,   274,   275,   292,   277,   198,   145,   280,
   297,   282,   187,   298,  2318,   300,   153,  1539,  1540,   133,
  1842,    59,   133,  1584,  1572,  1586,   298,    65,   300,   292,
   119,  1592,    70,  1685,   297,   292,   292,   126,  1690,   292,
   297,   297,   189,  1695,   297,   134,   292,    93,  1700,   292,
   119,   297,   292,  1705,   297,   292,   292,   297,  1710,   292,
   297,   297,   292,  1715,   297,   292,    95,   297,  1720,   292,
   297,  1593,   118,  1593,   297,   292,   145,   292,   292,   292,
   297,   119,   297,   297,   297,   292,   295,   298,   126,   300,
   297,   137,   138,   301,   231,   292,   134,   301,    49,    50,
   297,    52,    53,    54,    55,   301,   190,   145,   292,    60,
    59,   248,    63,   297,   290,   153,    67,  1678,   298,  1680,
   300,    70,   299,    74,    75,   171,   298,    78,   292,    27,
   299,    82,    83,   297,   180,   181,    87,    88,    89,   276,
   292,   292,   231,   252,   257,   297,   297,   284,   285,   286,
   287,   288,   289,   290,   200,   298,   298,   300,   300,   248,
   298,   237,   300,   300,   298,   298,   300,   300,   299,   298,
   119,   300,   298,   150,   300,   111,   182,   126,   125,   192,
   299,   113,   196,  1490,  2051,   134,  1493,   276,   295,   295,
    77,   172,   182,   231,   182,   182,   182,  1758,   287,   288,
   289,   290,   178,   198,    59,   300,   300,   276,   298,   113,
   248,   299,   295,   250,  1143,   284,   285,   286,   287,   288,
   289,   290,   274,   149,   299,   298,    26,   289,    25,  1790,
  1882,  1792,   278,   301,   301,    82,   150,   153,   276,   284,
   158,   221,   262,   150,  1805,   276,   284,   285,   286,   287,
   288,   289,   290,   284,   285,   286,   287,   288,   289,   290,
    67,   226,   300,  1824,   298,  1826,  1827,  1828,  1829,  1830,
   300,   298,   284,   284,  1835,  1836,  1205,   173,    81,   300,
  1841,   300,   231,   300,   300,  2317,   113,   171,   138,   201,
   300,  1852,   299,    92,   299,   299,   299,   297,   300,   248,
   299,  1862,  1863,  1851,   300,  1866,   300,   299,   299,   233,
   299,   274,   284,   296,   299,   299,  1877,   299,    27,   258,
   299,   129,   300,   298,  1872,   299,   299,   276,   299,   299,
   299,   299,   299,   299,   295,   299,   287,   299,   287,   288,
   289,   290,   299,   294,   299,   299,   299,  1908,   300,   299,
   129,   299,   119,    59,   299,   299,   299,   299,   299,   299,
   311,   312,   299,   276,    70,  2187,   299,   302,   302,   297,
   300,   322,   323,   324,   299,   284,   327,   328,   299,   330,
   331,   198,   191,   111,   335,   198,   337,   103,   339,   340,
   341,    59,   343,   111,   345,   346,   347,   300,   300,   300,
    59,   352,   353,   300,   299,   356,   299,   119,    59,   300,
   298,   300,    86,   119,   300,   300,   289,   300,   300,   300,
   126,   372,   300,   300,   298,   116,   191,   378,   134,   111,
  1991,   274,   274,   298,  2001,   302,   302,   299,   299,   182,
   116,   271,   393,   153,   111,   396,    67,  2008,  1996,   182,
   232,  1805,   403,   229,   159,  2016,   300,   115,   299,  2020,
   299,  2022,   300,   300,  2025,  2026,  2027,  2028,  2029,  2030,
  2031,  2032,   300,  2034,   300,   300,   427,  1999,  2039,  1999,
   300,   300,   300,  1790,   435,   436,   300,  1841,   300,   300,
  2051,   442,  2053,   299,   445,  2043,   300,   448,   299,   297,
   128,  2062,  2063,  2064,  2065,  2066,   299,   299,  1862,   299,
   299,   299,   463,   299,   299,   299,   299,    72,   299,   175,
   300,   299,  2083,   297,   299,   231,    59,   299,   479,   299,
   299,   299,    65,   299,   190,  1464,  2097,    70,   300,   195,
   491,   300,   248,   300,   300,  1852,   202,   203,   300,   300,
   206,   298,   300,   117,   300,   300,   300,   218,   300,   300,
   300,   217,   513,   300,   300,   516,   517,   274,   300,   225,
   276,   300,   228,   300,   300,  1882,   300,   300,   300,  2140,
   284,   287,   288,   289,   290,   300,   119,    96,   299,   297,
    96,   269,   105,   126,   220,   251,   299,   253,   549,   299,
   299,   134,   129,   259,   299,   261,   147,   129,   151,   149,
  2171,   300,   145,   152,   300,   300,   300,   300,   155,   300,
   153,   300,    59,   300,  1553,  2147,   128,  2147,  2150,   580,
  2150,  2192,   162,   299,  2195,    59,   300,   300,  2199,   130,
   300,    65,   295,   300,   298,  2193,    70,  2208,  2209,  2210,
  2211,  2212,  2213,  2214,  2215,  2216,  2217,   300,   300,  2220,
   300,   300,   300,  2224,   300,   300,   300,   300,   299,   299,
   299,    65,   623,   300,   625,   299,   627,   299,  2200,  2201,
   298,   300,   299,   299,  1991,   300,   637,   300,   300,   300,
   300,   300,   300,   300,   300,   119,   300,   300,   231,   300,
   300,   300,   126,   300,  2265,   300,  2267,   300,   300,   300,
   134,   300,   300,   165,   219,   248,   300,    59,   136,   298,
    65,   145,   299,   230,   675,   300,   677,   300,   150,  2251,
   299,  2251,  2251,   299,   295,   300,  2347,   300,   300,   297,
   293,   300,  1671,   276,    61,   293,   697,     0,     0,    92,
   944,   284,   285,   286,   287,   288,   289,   290,   455,  1270,
  2321,   707,   713,  1566,   622,   298,  1260,  2315,   719,   856,
  1869,  2332,  1584,  1262,  2054,   726,   727,  2263,  2328,  2338,
  2358,   732,  2366,  1302,  2367,   736,   737,  1586,  1574,  1303,
    86,   631,   743,  2315,    71,  2315,   996,   514,  2220,   404,
  2330,  2219,  2360,   493,   349,  2347,  1572,   231,    90,   395,
  2332,  2314,  1317,  1224,   884,  1185,  2224,  1307,  1524,  2367,
  2381,  1219,  2383,   421,   248,   876,   580,  1985,  1757,    59,
  2381,  1732,  1382,  2140,  1671,  1914,   861,   998,  2192,  1205,
    70,  2298,   857,   283,   378,  2367,   917,  2367,  1792,   913,
   430,  1527,   276,   505,   920,  1209,   880,  2154,   165,  2381,
   284,   285,   286,   287,   288,   289,   290,   989,  1491,   934,
  1188,  2147,  2150,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   836,   837,    -1,    -1,   119,
    -1,    -1,    -1,    -1,    -1,  1824,   126,  1826,  1827,  1828,
  1829,  1830,    -1,    -1,   134,    -1,    -1,    -1,   859,    -1,
   861,   862,     7,    -1,    -1,    10,    11,    -1,   869,    14,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    22,    23,    -1,
    -1,    -1,    -1,    -1,  1863,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   913,    -1,   915,    -1,   917,    -1,    -1,    64,
    -1,    -1,    -1,    -1,    69,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    79,  1914,    -1,    -1,    83,   940,
    85,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    95,   231,    97,    -1,    -1,    -1,   101,    -1,   103,    -1,
   105,    -1,    -1,    -1,   109,    -1,    -1,    -1,   248,    -1,
   115,   972,    -1,    -1,    -1,    -1,    -1,   122,   979,   980,
    -1,    -1,    -1,    -1,    -1,    -1,  1965,    -1,    -1,   990,
    -1,    -1,    -1,    -1,    -1,   996,   276,    -1,    -1,    -1,
  1001,    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,
   290,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   167,    -1,   169,    -1,    -1,   172,   173,  2008,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2016,  2017,    -1,
    -1,  2020,    -1,  2022,   189,    -1,  2025,  2026,  2027,  2028,
  2029,  2030,  2031,  2032,    -1,  2034,    -1,    -1,   203,   204,
  2039,    -1,    -1,    -1,    -1,    -1,    -1,   212,   213,    -1,
    -1,    -1,    -1,    -1,  2053,    -1,    -1,   222,   223,    -1,
    -1,    -1,    -1,  2062,  2063,  2064,  2065,  2066,    -1,   234,
   235,   236,    -1,   238,    -1,    -1,   241,    -1,    -1,    -1,
    -1,    -1,   247,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   256,    -1,    -1,    49,    50,    -1,    52,   263,    59,
    -1,    -1,    -1,    -1,    -1,   270,    -1,    -1,    -1,    -1,
    70,    -1,    49,    -1,    -1,    52,    -1,    54,    -1,    -1,
    -1,    -1,  1143,    60,    -1,    -1,    63,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   299,    -1,    -1,    74,    75,    -1,
    -1,    78,    -1,    -1,    -1,    82,    83,    -1,    -1,    -1,
    87,    88,    89,    -1,    -1,    -1,    -1,    -1,    -1,   119,
    -1,    -1,    -1,    -1,  2163,    -1,   126,    -1,    -1,  1190,
  2169,    -1,  2171,    -1,   134,    -1,    -1,    -1,   176,    -1,
    -1,  1202,    -1,    -1,  1205,   145,  1207,    -1,    -1,    -1,
   188,    -1,   190,    -1,    -1,    -1,  1217,   195,    -1,    -1,
  2199,    -1,    -1,  1224,   202,   203,    -1,    -1,   206,  2208,
  2209,  2210,  2211,  2212,  2213,  2214,  2215,  2216,    -1,   217,
    -1,    -1,    -1,    59,    -1,    -1,    -1,   225,    -1,    65,
   228,    -1,    -1,    -1,    70,    -1,    -1,    -1,    -1,  1260,
    -1,  1262,  1263,  1264,  1265,    -1,    -1,    -1,    -1,    -1,
  1271,    -1,    -1,   251,    -1,   253,  1277,  2256,    -1,    -1,
  1281,   259,    -1,   261,  2263,    -1,  2265,    -1,    -1,  1290,
   268,   231,    -1,    -1,    -1,    -1,    -1,  1298,    -1,  1300,
  1301,  1302,  1303,   119,    -1,    -1,    -1,  1308,   248,  1310,
   126,  1312,    -1,    -1,    -1,    -1,  1317,    -1,   134,    -1,
    -1,    -1,    59,    -1,  1325,  1326,  1327,    -1,    65,   145,
  2309,    -1,  2311,    70,    -1,    -1,   276,   153,    -1,    -1,
    -1,    -1,  2321,    -1,   284,   285,   286,   287,   288,   289,
   290,   286,   287,    -1,    -1,    -1,    -1,    -1,  1359,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   287,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   119,    -1,    -1,    -1,    -1,    -1,    -1,   126,
    -1,    -1,    59,    -1,   311,    -1,  1397,   134,    65,    -1,
    -1,    -1,    -1,    70,    -1,    -1,   323,   324,   145,    -1,
   327,   328,    -1,   330,   331,   231,   153,    -1,   335,    -1,
   355,    -1,   339,   340,    -1,    -1,   343,    -1,   345,   346,
   347,    -1,   248,    -1,    -1,   352,   353,    -1,    -1,    -1,
    -1,    -1,    -1,   378,    -1,  1446,    -1,    -1,    -1,    -1,
    -1,    -1,   119,    -1,    59,   372,    -1,    -1,    -1,   126,
   276,   378,    -1,  1464,    -1,    70,    -1,   134,   284,   285,
   286,   287,   288,   289,   290,    -1,   393,    -1,   145,   396,
    -1,    -1,    -1,    -1,   300,    -1,   403,    -1,    -1,  1490,
    -1,    -1,  1493,    -1,   231,  1496,    -1,    -1,  1499,    -1,
   435,    59,    -1,    -1,    -1,   440,    -1,    65,    -1,    67,
   427,   248,    70,    -1,   119,    -1,    -1,    -1,    -1,   436,
    -1,   126,    -1,    -1,    -1,   442,    -1,    -1,   445,   134,
    -1,   448,  1533,    -1,    -1,    -1,    -1,    -1,    -1,   276,
   145,    -1,    -1,    -1,    -1,    -1,   463,   284,   285,   286,
   287,   288,   289,   290,    -1,  1556,    -1,  1558,    -1,    -1,
    -1,   119,   479,   300,   231,    -1,    -1,    -1,   126,    -1,
    -1,    -1,    -1,    -1,   491,    -1,   134,    -1,    -1,    -1,
    -1,   248,    -1,  1584,    -1,  1586,    -1,   145,    -1,    -1,
    -1,  1592,    -1,    -1,    -1,   153,   513,    -1,    -1,   516,
   517,    -1,    -1,  1604,    -1,    -1,    -1,    -1,    -1,   276,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,   286,
   287,   288,   289,   290,    -1,    -1,   231,    59,    -1,    -1,
    -1,    -1,   549,    65,    -1,    -1,    -1,    -1,    70,    -1,
    -1,    -1,    -1,   248,    -1,    -1,    -1,   564,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   580,    -1,    -1,    -1,    -1,    -1,    -1,
  1671,   276,    -1,   231,    -1,    -1,    -1,    -1,   613,   284,
   285,   286,   287,   288,   289,   290,    -1,   119,    -1,    -1,
   248,    -1,    -1,    -1,   126,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   134,    -1,    -1,    -1,   623,    -1,   625,    -1,
   627,    -1,    -1,   145,    -1,    -1,    -1,    -1,   276,    -1,
   637,   153,    -1,    -1,    -1,    -1,   284,   285,   286,   287,
   288,   289,   290,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   675,    -1,   677,    -1,    -1,    -1,    59,    -1,    -1,    -1,
    -1,    -1,    65,    -1,    -1,    -1,  1757,    70,   675,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   697,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1790,
    -1,  1792,    -1,    -1,    -1,    -1,   713,    -1,    -1,   231,
    -1,    -1,   719,    -1,  1805,    -1,   119,    -1,    -1,   726,
   727,    -1,    -1,   126,    -1,   732,   248,    59,    -1,   736,
   737,   134,    -1,    65,    -1,    -1,   743,    -1,    70,    -1,
    -1,    -1,   145,    -1,  1835,  1836,    -1,    -1,    -1,    -1,
  1841,  1842,    -1,    -1,   276,    -1,    -1,   764,    -1,    -1,
    -1,  1852,   284,   285,   286,   287,   288,   289,   290,    -1,
    -1,  1862,    -1,    -1,    -1,  1866,   298,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1877,   119,    -1,    -1,
    -1,    -1,    -1,    -1,   126,    -1,    -1,  1888,   823,    -1,
    -1,    -1,   134,    -1,    -1,    -1,  1897,    -1,    -1,    -1,
    -1,    -1,    -1,   145,   839,    -1,    -1,    -1,    -1,    -1,
    -1,   153,    -1,  1914,    -1,    -1,    -1,    -1,   231,   836,
   837,   856,    -1,    -1,    -1,    -1,    -1,   862,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   248,    -1,    -1,    -1,    -1,
    -1,    -1,   859,    59,   861,   862,    -1,    -1,    -1,    -1,
    -1,  1952,   869,    -1,    70,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   276,  1965,    -1,    -1,    -1,    -1,    -1,
    -1,   284,   285,   286,   287,   288,   289,   290,    -1,    -1,
    -1,    -1,   917,    -1,    -1,    -1,    -1,    -1,    -1,   231,
  1991,    -1,    -1,    -1,    -1,    -1,   913,    -1,   915,    -1,
    -1,    -1,    -1,   119,    -1,    -1,   248,   328,    -1,    -1,
   126,    -1,    -1,    -1,  2015,    -1,  2017,    -1,   134,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   347,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   276,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   284,   285,   286,   287,   288,   289,   290,    -1,
  2051,    -1,    -1,    -1,   989,   972,   298,    -1,    -1,    -1,
    -1,    -1,   979,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   990,    -1,    59,    -1,    -1,    -1,   996,
    -1,    -1,  2083,    -1,  1001,    -1,    70,    -1,    -1,    -1,
    -1,    -1,    39,    40,    41,    42,    43,    44,    45,    46,
    47,    48,    -1,    50,    51,    52,    53,    54,    55,    56,
    57,    58,    -1,    -1,    -1,   231,    -1,    -1,    -1,    -1,
    -1,  1056,    -1,    -1,    -1,  1060,  1061,  1062,  1063,    -1,
    -1,    -1,   248,    -1,    -1,   119,    -1,    -1,  1073,  2140,
    -1,    -1,   126,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   134,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   276,    -1,  2163,    -1,    -1,    -1,    -1,    -1,  2169,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  2187,    -1,    -1,    -1,
    -1,  2192,    -1,    -1,  2195,    -1,  1131,  1132,  1133,  1134,
    59,    -1,    -1,    -1,    -1,    -1,  2207,    -1,    -1,    -1,
    -1,    70,    -1,    -1,  1149,    -1,  2217,    -1,    -1,  2220,
  2221,  2222,  2223,  2224,    59,    -1,  1143,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    70,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   231,    -1,    -1,
    -1,    -1,    -1,  1188,  2255,  2256,    -1,    -1,    -1,    -1,
   119,    -1,  2263,    -1,   248,    -1,  2267,   126,    -1,    -1,
    -1,    -1,    -1,  1190,    -1,   134,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   119,  1202,   145,    -1,  1205,    -1,
  1207,   126,   276,    -1,    -1,    -1,    -1,  2298,    -1,   134,
  1217,   285,   286,   287,   288,   289,   290,  1224,  2309,    -1,
  2311,    -1,    -1,    -1,  2315,    -1,    -1,  2318,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   274,    -1,    -1,
   277,  2332,    -1,   280,    -1,   282,    -1,   284,    -1,    -1,
    -1,    -1,   289,  1260,    -1,  1262,  1263,  1264,  1265,   296,
   297,   298,   299,   300,  1271,   302,    -1,    -1,    -1,  2360,
  1277,    -1,    -1,    -1,  1281,    -1,  2367,    -1,    -1,    -1,
    -1,    -1,   231,  1290,    -1,    -1,    -1,    -1,    -1,    -1,
  2381,  1298,  2383,  1300,  1301,  1302,  1303,    -1,    -1,   248,
    -1,  1308,    -1,  1310,    -1,  1312,   231,    -1,    -1,    -1,
  1317,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1325,  1326,
  1327,    -1,    -1,   248,    -1,    -1,    -1,   276,    -1,    -1,
   567,    -1,    -1,    -1,    -1,   284,   285,   286,   287,   288,
   289,   290,    -1,    -1,   581,    -1,   583,    -1,   585,   586,
   587,   276,  1359,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   285,   286,   287,   288,   289,   290,    -1,    -1,   779,    -1,
    -1,    -1,    -1,  1398,  1399,  1382,    -1,  1402,    -1,  1404,
    -1,    -1,  1407,  1408,  1409,  1410,  1411,  1412,  1413,  1414,
  1397,    -1,  1417,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   643,    -1,    -1,    -1,
  1435,    -1,   823,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1447,  1448,  1449,  1450,  1451,  1452,   839,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1446,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    59,    -1,    -1,  1464,    -1,    -1,
    65,    -1,    -1,    -1,    -1,    70,  1491,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1490,    -1,    -1,  1493,   724,    33,  1496,
    -1,    -1,  1499,    -1,    39,    40,    41,    42,    43,    44,
    45,    46,    47,    48,    -1,    50,    51,    52,    53,    54,
    55,    56,    57,    58,   119,    -1,   753,    -1,    -1,    -1,
    -1,   126,    -1,    -1,    -1,    -1,  1533,    -1,    -1,   134,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   776,
   145,    -1,    -1,    -1,    -1,    -1,   783,    -1,   153,  1556,
    -1,  1558,    39,    40,    41,    42,    43,    44,    45,    46,
    47,    48,    -1,    50,    51,    52,    53,    54,    55,    56,
    57,    58,   177,    -1,    -1,    -1,    -1,  1584,    -1,  1586,
    -1,    -1,    -1,    -1,    -1,  1592,   823,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   838,    -1,    -1,    -1,   842,    -1,   844,    -1,    -1,
   847,   848,   849,   850,   851,   852,   853,   854,  1029,  1030,
  1031,  1032,    -1,    -1,    -1,    -1,   231,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   248,    -1,  1056,    -1,    -1,    -1,  1060,
  1061,  1062,  1063,  1678,    -1,  1680,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1671,    -1,    -1,    -1,    -1,    -1,
    -1,   276,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   284,
   285,   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1131,  1132,  1133,  1134,    -1,    -1,    -1,    -1,    -1,   274,
    -1,    -1,   277,  1758,   279,   280,    -1,   282,  1149,   284,
    -1,    -1,    -1,    -1,   289,    -1,    -1,    -1,   293,    -1,
  1757,   296,   297,   298,   299,   300,   301,   302,    -1,    -1,
   997,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1790,    -1,  1792,    -1,   274,    -1,    -1,
   277,    -1,    -1,   280,    -1,   282,    -1,   284,  1805,    -1,
    -1,    -1,   289,    -1,    -1,  1042,    -1,  1044,  1045,   296,
   297,   298,   299,   300,    -1,   302,    -1,    -1,    -1,  1056,
    -1,    -1,    -1,  1060,  1061,  1062,  1063,  1064,  1835,  1836,
    -1,    -1,    -1,    -1,  1841,    -1,    -1,    -1,    -1,  1076,
    -1,    -1,    -1,    -1,    -1,  1852,    -1,   549,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1862,    -1,    -1,    -1,  1866,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1877,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1888,    -1,  1908,    -1,    -1,    -1,    -1,    -1,    -1,
  1897,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1135,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1914,    -1,    -1,
    -1,    -1,    -1,    -1,  1151,    -1,    -1,    -1,  1155,    -1,
    -1,   623,  1159,    -1,    -1,    -1,  1163,    -1,    -1,    -1,
  1167,    -1,    -1,    -1,  1171,    -1,    -1,    -1,  1175,  1350,
  1351,    -1,  1179,    -1,    -1,  1952,  1183,    -1,    -1,  1360,
  1361,  1362,  1363,  1364,  1365,    -1,    -1,    -1,  1965,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1385,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1219,    -1,  1991,    -1,    -1,  1398,  1399,    -1,
    -1,  1402,    -1,  1404,    -1,    -1,  1407,  1408,  1409,  1410,
  1411,  1412,  1413,  1414,    -1,    -1,  1417,    -1,  2015,    -1,
  2017,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1435,    -1,    -1,  1438,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1447,  1448,  1449,  1450,
  1451,  1452,    -1,    -1,  2051,    -1,    -1,    -1,    39,    40,
    41,    42,    43,    44,    45,    46,    47,    48,    -1,    50,
    51,    52,    53,    54,    55,    56,    57,    58,    -1,    -1,
    -1,    -1,  2097,    -1,    -1,    -1,  2083,   779,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1329,    59,    -1,    -1,    -1,    -1,    -1,    65,
    -1,  1338,    68,    -1,    70,    -1,    -1,    -1,    -1,    -1,
  1347,    -1,    -1,    -1,    -1,    -1,  1353,    -1,    -1,    -1,
    -1,   823,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    98,  2140,    -1,    -1,    -1,   839,    -1,    -1,
    -1,  1378,  1553,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1387,    -1,    -1,   119,    -1,    -1,  2163,    -1,    -1,    -1,
   126,  1398,  2169,    -1,    -1,  1402,    -1,  1404,   134,    -1,
  1407,  1408,  1409,  1410,  1411,  1412,  1413,  1414,    59,   145,
  1417,    -1,    -1,    -1,    65,  2192,    -1,   153,  2195,    70,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  2207,    -1,    -1,  1440,    -1,    -1,    -1,    -1,    -1,    -1,
  2217,    -1,    -1,  2220,  2221,  2222,    -1,  2224,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   191,    -1,    -1,    -1,    -1,
    -1,    -1,   113,    -1,    -1,    -1,   779,    -1,   119,    -1,
    -1,    -1,    -1,    -1,    -1,   126,    -1,    -1,  2255,  2256,
    -1,    -1,    -1,   134,    -1,    -1,  2263,    -1,    -1,    -1,
  2267,    -1,    -1,    -1,   145,   231,    -1,  1678,    -1,  1680,
    -1,    -1,   153,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   823,    -1,   248,   274,    -1,    -1,   277,    -1,    -1,   280,
    -1,   282,    -1,   284,    -1,    -1,   839,    -1,   289,    -1,
    -1,    -1,  2309,    -1,  2311,   296,   297,   298,   299,   300,
   276,  2318,    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,
   286,   287,   288,   289,   290,  2332,    59,  1029,  1030,  1031,
  1032,    -1,    65,    -1,    67,    -1,    -1,    70,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1758,    -1,    -1,
   231,    -1,    -1,    -1,  1056,    -1,    -1,    -1,  1060,  1061,
  1062,  1063,    -1,    -1,    -1,    -1,    -1,   248,    -1,    -1,
    -1,    -1,    -1,    -1,  2381,    -1,  2383,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   119,    -1,    -1,    -1,
    -1,    -1,    -1,   126,    -1,   276,    -1,  1634,  1635,  1636,
    -1,   134,    -1,   284,   285,   286,   287,   288,   289,   290,
    -1,    -1,   145,  1824,    -1,  1826,  1827,  1828,  1829,  1830,
   153,    -1,    -1,    -1,    -1,  1662,  1663,  1664,    -1,  1131,
  1132,  1133,  1134,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1680,    -1,    -1,    -1,  1149,  1685,    -1,
    -1,    -1,  1863,  1690,    -1,    -1,    -1,    -1,  1695,    -1,
    -1,    -1,    -1,  1700,    -1,    -1,  1877,    -1,  1705,    -1,
    -1,    -1,    -1,  1710,    -1,    -1,    -1,    -1,  1715,    -1,
    -1,    -1,    -1,  1720,    -1,    -1,  1029,  1030,  1031,  1032,
    -1,    -1,    -1,    -1,    -1,  1732,    -1,  1908,   231,    -1,
    -1,    -1,    -1,  1205,    -1,  1742,  1743,  1744,    -1,    -1,
    -1,    -1,    -1,  1056,    -1,   248,    -1,  1060,  1061,  1062,
  1063,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   276,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   284,   285,   286,   287,   288,   289,   290,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1131,  1132,
  1133,  1134,    -1,    -1,    -1,    -1,    -1,  2008,    -1,    -1,
  1143,    -1,    -1,    -1,    -1,  2016,  1149,    -1,    -1,  2020,
    -1,  2022,    -1,    -1,  2025,  2026,  2027,  2028,  2029,  2030,
  2031,  2032,    -1,  2034,    -1,   763,    -1,    -1,  2039,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  2053,    -1,    -1,    -1,    -1,    -1,  1350,  1351,
    -1,  2062,  2063,  2064,  2065,  2066,    -1,    -1,  1360,  1361,
  1362,  1363,  1364,  1365,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1385,    -1,    -1,  2097,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1398,  1399,    -1,    -1,
  1402,    -1,  1404,    -1,    -1,  1407,  1408,  1409,  1410,  1411,
  1412,  1413,  1414,    -1,    -1,  1417,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1435,    -1,    -1,  1438,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1447,  1448,  1449,  1450,  1451,
  1452,    -1,    -1,    -1,    -1,    -1,    -1,  1994,    -1,    -1,
  2171,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2199,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2208,  2209,  2210,
  2211,  2212,  2213,  2214,  2215,  2216,    -1,  1350,  1351,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1360,  1361,  1362,
  1363,  1364,  1365,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1385,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2265,  1398,  1399,    -1,    -1,  1402,
    -1,  1404,    -1,    -1,  1407,  1408,  1409,  1410,  1411,  1412,
  1413,  1414,    -1,    -1,  1417,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,     3,    -1,    -1,    -1,     7,    -1,    -1,
    10,    11,  1435,    -1,    14,  1438,    -1,    -1,    -1,    -1,
    -1,    -1,    22,    23,  1447,  1448,  1449,  1450,  1451,  1452,
  2321,    -1,    -1,    -1,    -1,    -1,    -1,    37,    38,    -1,
    -1,  1464,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    59,    -1,    -1,
    -1,    -1,    -1,    65,    64,    67,    -1,    -1,    70,    69,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    79,
    59,    -1,    -1,    83,    -1,    85,    65,    -1,    -1,    -1,
    -1,    70,    -1,    -1,    -1,    95,  1678,    97,  1680,    -1,
    -1,   101,    -1,   103,    -1,   105,    -1,    -1,    -1,   109,
    -1,    -1,    -1,    -1,    -1,   115,    -1,   119,    -1,    -1,
    -1,    -1,   122,    59,   126,  1143,    -1,    -1,    -1,    65,
  1553,    -1,   134,    -1,    70,    -1,    -1,  1155,    -1,    -1,
   119,  1159,    -1,   145,    -1,  1163,    -1,   126,    -1,  1167,
    -1,   153,    -1,  1171,    -1,   134,    -1,  1175,    -1,    -1,
    -1,  1179,    -1,    -1,    -1,  1183,   145,   167,    -1,   169,
    -1,    -1,   172,   173,   153,    -1,  1758,    -1,    -1,    -1,
    -1,    -1,    -1,   119,    -1,    -1,    -1,    -1,    -1,   189,
   126,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   134,    -1,
    -1,    -1,    -1,   203,   204,    -1,    -1,    -1,    -1,   145,
    -1,    -1,   212,   213,  2331,    -1,    -1,   153,    -1,    -1,
    -1,    -1,   222,   223,    -1,    -1,    -1,    -1,    -1,   231,
    -1,    -1,    -1,    -1,   234,   235,   236,    -1,   238,    -1,
    -1,   241,    -1,    -1,    -1,    -1,   248,   247,  1671,    -1,
    -1,    -1,   231,    -1,    -1,  1678,   256,  1680,    -1,    -1,
    -1,    -1,    -1,   263,    -1,    -1,    -1,    -1,    -1,   248,
   270,    -1,    -1,    -1,   276,    -1,    -1,    -1,    -1,    -1,
    -1,  1299,   284,   285,   286,   287,   288,   289,   290,    -1,
    -1,    -1,    -1,    -1,    -1,   231,    -1,   276,    -1,   299,
    -1,    -1,    -1,    -1,    -1,   284,   285,   286,   287,   288,
   289,   290,   248,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1908,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1757,  1758,    -1,    -1,    -1,    -1,
   276,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1381,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1824,    -1,  1826,  1827,  1828,  1829,  1830,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1863,    -1,    -1,    -1,    -1,    -1,  1464,    -1,  1466,  1467,
    -1,  1469,  1470,    -1,  1472,  1473,    -1,  1475,  1476,    -1,
  1478,  1479,    -1,  1481,  1482,    -1,  1484,  1485,    -1,  1487,
  1488,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1908,    -1,    -1,    -1,    -1,
    -1,  1914,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  2097,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,  1965,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    59,  2008,    -1,    -1,    -1,    -1,
    65,    -1,    -1,  2016,  2017,    70,    -1,  2020,    -1,  2022,
    -1,    -1,  2025,  2026,  2027,  2028,  2029,  2030,  2031,  2032,
    -1,  2034,    -1,    -1,    -1,    -1,  2039,    -1,    -1,    -1,
    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,
  2053,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   110,  2062,
  2063,  2064,  2065,  2066,   119,    -1,   118,    -1,    -1,    -1,
    -1,   126,   124,  1671,    -1,    -1,    -1,    -1,    -1,   134,
    -1,    -1,    -1,    -1,    -1,   137,   138,  1685,    -1,    -1,
   145,    -1,  1690,    -1,  2097,    -1,   148,  1695,   153,    -1,
    -1,    -1,  1700,    -1,    -1,    -1,    -1,  1705,    -1,    -1,
    -1,    -1,  1710,    -1,    -1,    -1,    -1,  1715,    -1,   171,
    -1,    -1,  1720,    -1,    -1,    -1,   178,   179,    -1,    -1,
  1728,    -1,    -1,    -1,  1732,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,    -1,
    -1,   203,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  2163,    -1,    -1,    -1,    -1,    -1,  2169,    -1,  2171,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   231,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   248,    -1,    -1,  2199,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  2208,  2209,  2210,  2211,  2212,
  2213,  2214,  2215,  2216,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   276,   274,    -1,    -1,   277,    -1,    -1,    -1,   284,
   285,   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  2256,    -1,    -1,    -1,    -1,    -1,    -1,
  2263,    -1,  2265,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  2309,    -1,  2311,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1914,    -1,  2321,  1917,
  1918,    -1,  1920,  1921,    -1,  1923,  1924,    -1,  1926,  1927,
    -1,  1929,  1930,    -1,  1932,  1933,    -1,  1935,  1936,    -1,
  1938,  1939,    -1,    -1,    -1,    -1,    -1,  1945,    -1,    -1,
    -1,  1949,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    74,    75,    76,    77,    -1,    -1,    80,
    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,
    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,   106,   107,   108,    -1,   110,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,
    -1,   142,   143,    -1,   145,   146,   147,   148,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,   190,
    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,   254,   255,    -1,   257,   258,   259,   260,
   261,   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,   275,   276,   277,   278,    -1,   280,
   281,   282,    -1,    -1,    -1,    -1,    -1,   288,   289,    -1,
   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,   300,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    74,    75,    76,    77,    -1,    -1,    80,    -1,    -1,
    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,   145,   146,   147,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,   262,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,   275,   276,   277,   278,    -1,   280,   281,   282,
    -1,    -1,    -1,    -1,    -1,   288,   289,    -1,   291,    -1,
   293,    -1,    -1,    -1,    -1,    -1,   299,   300,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,    -1,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,
    75,    76,    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,
    -1,    -1,    87,    88,    89,    90,    91,    -1,    93,    94,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,   106,   107,   108,    -1,   110,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,
   145,   146,   147,   148,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
   185,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,   254,
   255,    -1,   257,   258,   259,   260,   261,   262,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
   275,   276,   277,   278,    -1,   280,   281,   282,    -1,    -1,
    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,
    -1,    -1,    -1,    -1,   299,   300,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,    76,
    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,
    87,    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,
   107,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,
    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,
   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,   186,
    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,   254,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,
    -1,    -1,   299,   300,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    88,
    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,    -1,    -1,    -1,   148,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,    -1,   186,    -1,    -1,
    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,   254,   255,    -1,   257,   258,
   259,   260,   261,   262,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,    -1,
    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,
    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,
   299,   300,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    75,    76,    77,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,   110,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,
    -1,   142,   143,    -1,   145,    -1,   147,   148,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,    -1,   186,    -1,    -1,    -1,    -1,
    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,   254,   255,    -1,   257,   258,   259,   260,
   261,   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,   275,   276,   277,    -1,    -1,   280,
   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,
   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,   300,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    76,    77,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,    -1,    -1,    -1,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,   300,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    71,    -1,    -1,    74,    75,    76,    77,
    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,
    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,
   108,    -1,   110,    -1,    -1,   113,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,
    -1,   129,   130,   131,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,   147,
   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,   157,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,   174,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,   183,    -1,   185,   186,    -1,
    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,   262,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,
   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,
   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,
    -1,   299,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    71,    -1,    -1,    74,    75,    76,    77,    -1,    -1,    80,
    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,
    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,   106,   107,   108,    -1,   110,
    -1,    -1,   113,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,
   131,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,
    -1,   142,   143,    -1,   145,   146,   147,   148,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,   183,    -1,   185,   186,    -1,    -1,    -1,   190,
    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,   254,   255,    -1,   257,   258,   259,   260,
   261,   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,   275,   276,   277,   278,    -1,   280,
   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,
   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    71,    -1,    -1,
    74,    75,    76,    77,    -1,    -1,    80,    -1,    -1,    -1,
    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,    93,
    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,   113,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
   124,    -1,    -1,    -1,    -1,   129,   130,   131,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,
    -1,   145,   146,   147,   148,   149,    -1,   151,   152,    -1,
    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,   183,
    -1,   185,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
   254,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,   275,   276,   277,   278,    -1,   280,   281,   282,    -1,
    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,
    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    71,    -1,    -1,    74,    75,    76,
    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,
    87,    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,
   107,   108,    -1,   110,    -1,    -1,   113,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,
    -1,    -1,   129,   130,   131,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,
   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,   183,    -1,   185,   186,
    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,   254,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,
    -1,    -1,   299,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    63,    -1,    -1,    66,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    74,    75,    76,    77,    -1,    -1,
    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,
    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,   106,   107,   108,    -1,
   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
   140,    -1,   142,   143,    -1,    -1,   146,    -1,   148,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,   174,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,
   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,   254,   255,    -1,   257,   258,   259,
   260,   261,   262,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,   275,   276,   277,   278,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,
    -1,   291,   292,   293,    -1,    -1,    -1,   297,    -1,   299,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    74,    75,    76,    77,    -1,    -1,    80,    -1,    -1,
    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,   145,   146,   147,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,   262,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,   275,   276,   277,   278,    -1,   280,   281,   282,
    -1,    -1,    -1,    -1,    -1,   288,   289,    -1,   291,    -1,
   293,    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    63,    -1,    -1,
    66,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,
    76,    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,
    -1,    87,    88,    89,    90,    91,    -1,    93,    94,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
   106,   107,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,
   146,    -1,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,
   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,   254,   255,
    -1,   257,   258,   259,   260,   261,   262,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,
   276,   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,
    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,
    -1,    -1,    -1,   299,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    63,    -1,    -1,    66,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    74,    75,    76,    77,    -1,
    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,
    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,   108,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,   145,   146,    -1,   148,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,   174,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,
    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,   254,   255,    -1,   257,   258,
   259,   260,   261,   262,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,   278,
    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,
    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,
   299,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    63,    -1,    -1,    66,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    74,    75,    76,    77,    -1,    -1,    80,    -1,
    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,
    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,
    -1,    -1,    -1,    -1,   106,   107,   108,    -1,   110,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,
    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,
    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,
   142,   143,    -1,   145,   146,    -1,   148,   149,    -1,   151,
   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,
    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,
    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,
    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,   254,   255,    -1,   257,   258,   259,   260,   261,
   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,   275,   276,   277,   278,    -1,   280,   281,
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,
    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,    -1,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    63,    -1,
    -1,    66,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,
    75,    76,    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,
    -1,    -1,    87,    88,    89,    90,    91,    -1,    93,    94,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,   106,   107,   108,    -1,   110,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,
   145,   146,    -1,   148,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
   185,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,   254,
   255,    -1,   257,   258,   259,   260,   261,   262,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
   275,   276,   277,   278,    -1,   280,   281,   282,    -1,    -1,
    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,
    -1,    -1,    -1,    -1,   299,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    63,    -1,    -1,    66,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,    76,    77,
    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,
    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,
   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,    -1,
   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,   157,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,   174,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,   185,   186,    -1,
    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,   262,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,
   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,
   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,
    -1,   299,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    63,    -1,    -1,    66,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    74,    75,    76,    77,    -1,    -1,    80,
    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,
    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,   106,   107,   108,    -1,   110,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,
    -1,   142,   143,    -1,   145,   146,    -1,   148,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,   190,
    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,   254,   255,    -1,   257,   258,   259,   260,
   261,   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,   275,   276,   277,   278,    -1,   280,
   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,
   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    63,
    -1,    -1,    66,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    74,    75,    76,    77,    -1,    -1,    80,    -1,    -1,    -1,
    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,    93,
    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,
    -1,   145,   146,    -1,   148,   149,    -1,   151,   152,    -1,
    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,
    -1,   185,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
   254,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,   275,   276,   277,   278,    -1,   280,   281,   282,    -1,
    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,
    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,    76,
    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,
    87,    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,
   107,   108,    -1,   110,    -1,    -1,   113,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,
    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,
   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,   186,
    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,   254,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,
    -1,    -1,   299,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    74,    75,    76,    77,    -1,    -1,
    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,
    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,   106,   107,   108,    -1,
   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
   140,    -1,   142,   143,    -1,   145,   146,   147,   148,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,   172,    -1,   174,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,
   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,   254,   255,    -1,   257,   258,   259,
   260,   261,   262,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,   275,   276,   277,   278,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,
    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    63,    -1,    -1,    66,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    74,    75,    76,    77,    -1,    -1,    80,    -1,    -1,
    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,   145,   146,    -1,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,   262,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,   275,   276,   277,   278,    -1,   280,   281,   282,
    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,
   293,    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    63,    -1,    -1,
    66,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,
    76,    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,
    -1,    87,    88,    89,    90,    91,    -1,    93,    94,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
   106,   107,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,
   146,    -1,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,
   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,   254,   255,
    -1,   257,   258,   259,   260,   261,   262,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,
   276,   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,
    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,
    -1,    -1,    -1,   299,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    63,    -1,    -1,    66,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    74,    75,    76,    77,    -1,
    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,
    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,   108,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,   145,   146,    -1,   148,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,   174,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,
    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,   254,   255,    -1,   257,   258,
   259,   260,   261,   262,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,   278,
    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,
    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,
   299,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    63,    -1,    -1,    66,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    74,    75,    76,    77,    -1,    -1,    80,    -1,
    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,
    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,
    -1,    -1,    -1,    -1,   106,   107,   108,    -1,   110,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,
    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,
    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,
   142,   143,    -1,   145,   146,    -1,   148,   149,    -1,   151,
   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,
    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,
    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,
    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,   254,   255,    -1,   257,   258,   259,   260,   261,
   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,   275,   276,   277,   278,    -1,   280,   281,
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,
    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,    -1,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    63,    -1,
    -1,    66,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,
    75,    76,    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,
    -1,    -1,    87,    88,    89,    90,    91,    -1,    93,    94,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,   106,   107,   108,    -1,   110,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,
   145,   146,    -1,   148,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
   185,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,   254,
   255,    -1,   257,   258,   259,   260,   261,   262,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
   275,   276,   277,   278,    -1,   280,   281,   282,    -1,    -1,
    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,
    -1,    -1,    -1,    -1,   299,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    63,    -1,    -1,    66,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,    76,    77,
    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,
    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,
   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,    -1,
   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,   157,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,   174,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,   185,   186,    -1,
    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,   262,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,
   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,
   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,
    -1,   299,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    63,    -1,    -1,    66,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    74,    75,    76,    77,    -1,    -1,    80,
    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,
    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,   106,   107,   108,    -1,   110,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,
    -1,   142,   143,    -1,   145,   146,    -1,   148,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,   190,
    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,   254,   255,    -1,   257,   258,   259,   260,
   261,   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,   275,   276,   277,   278,    -1,   280,
   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,
   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    74,    75,    76,    77,    -1,    -1,    80,    -1,    -1,    -1,
    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,    93,
    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,
    -1,   145,   146,   147,   148,   149,    -1,   151,   152,    -1,
    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,
    -1,   185,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
   254,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,   275,   276,   277,   278,    -1,   280,   281,   282,    -1,
    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,
    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    63,    -1,    -1,    66,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,    76,
    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,
    87,    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,
   107,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,
    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,   140,    -1,   142,   143,    -1,    -1,   146,
    -1,   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,   186,
    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,   254,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,
    -1,    -1,   299,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    74,    75,    76,    77,    -1,    -1,
    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,
    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,   106,   107,   108,    -1,
   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
   140,    -1,   142,   143,    -1,   145,   146,    -1,   148,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,   172,    -1,   174,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,
   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,   254,   255,    -1,   257,   258,   259,
   260,   261,   262,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,   275,   276,   277,   278,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,
    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    74,    75,    76,    77,    -1,    -1,    80,    -1,    -1,
    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,   145,   146,    -1,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,   262,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,   275,   276,   277,   278,    -1,   280,   281,   282,
    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,
   293,    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,
    76,    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,
    -1,    87,    88,    89,    90,    91,    -1,    93,    94,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
   106,   107,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,
   146,    -1,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,
   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,   254,   255,
    -1,   257,   258,   259,   260,   261,   262,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,
   276,   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,
    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,
    -1,    -1,    -1,   299,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,
    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,    -1,    -1,    -1,   148,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,   174,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,
    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,   254,   255,    -1,   257,   258,
   259,   260,   261,   262,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,   278,
    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,
    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,
   299,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    75,    76,    77,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,
    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,   110,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,
    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,
    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,
   142,   143,    -1,    -1,    -1,    -1,   148,   149,    -1,   151,
   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,
    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,
    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,
    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,   254,   255,    -1,   257,   258,   259,   260,   261,
   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,   275,   276,   277,   278,    -1,   280,   281,
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,
    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,    -1,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    75,    76,    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    88,    89,    90,    91,    -1,    93,    94,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,
    -1,    -1,   147,   148,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
    -1,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,   254,
   255,    -1,   257,   258,   259,   260,   261,   262,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
   275,   276,   277,    -1,    -1,   280,   281,   282,    -1,    -1,
    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,
    -1,    -1,    -1,    -1,   299,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,   140,    -1,   142,   143,    -1,    -1,    -1,    -1,
   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,   186,    -1,
    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,   262,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,
    -1,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,
   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,
    -1,   299,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    75,    76,    77,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    88,    89,    90,
    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,   110,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,
    -1,   142,   143,    -1,    -1,    -1,    -1,   148,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,    -1,   186,    -1,    -1,    -1,   190,
    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,   254,   255,    -1,   257,   258,   259,   260,
   261,   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,   275,   276,   277,    -1,    -1,   280,
   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,
   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    75,    76,    77,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,
    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,
    -1,    -1,    -1,    -1,   148,   149,    -1,   151,   152,    -1,
    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,
    -1,   185,   186,    -1,    -1,    -1,    -1,    -1,    -1,   193,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
   254,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,   275,   276,   277,    -1,    -1,   280,   281,   282,    -1,
    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,
    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    87,    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,
    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,   140,    -1,   142,   143,    -1,    -1,    -1,
    -1,   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,   186,
    -1,    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,   254,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,    -1,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,   288,    -1,    -1,   291,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   299,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    75,    76,    77,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,
   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
   140,    -1,   142,   143,    -1,   145,    -1,   147,   148,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,    -1,   186,    -1,    -1,    -1,
    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,   254,   255,    -1,   257,   258,   259,
   260,   261,   262,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,   275,   276,   277,    -1,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,
    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,   299,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    75,    76,    77,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   108,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,    -1,    -1,   147,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,    -1,   186,    -1,    -1,    -1,    -1,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,   262,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,   275,   276,   277,    -1,    -1,   280,   281,   282,
    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,
   293,    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,
    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,   254,   255,
    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
    -1,   277,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,   299,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,    -1,
    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,   172,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,   254,   255,    -1,   257,   258,   259,   260,
   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,    -1,    -1,   277,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,   299,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,
    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,   254,   255,
    -1,   257,   258,   259,   260,   261,   262,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,
   276,   277,    -1,    -1,   280,    -1,   282,    -1,   284,   285,
   286,   287,   288,   289,   290,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    96,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
   128,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,   162,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,   276,   277,
    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,   286,   287,
   288,   289,   290,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,   128,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   160,   161,   162,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,   254,   255,    -1,   257,   258,   259,
   260,   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,    -1,   276,   277,    -1,    -1,
    -1,    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,
   290,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    87,    -1,    -1,    -1,    -1,
    -1,    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,
    -1,    -1,    -1,    -1,   136,   137,   138,    -1,    -1,    -1,
    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,
   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,
    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,
    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,   254,   255,    -1,   257,   258,   259,   260,   261,
    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,    -1,    -1,   277,    -1,    -1,    -1,   281,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   289,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,    -1,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,    -1,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,
    -1,    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,   254,
   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
    -1,    -1,   277,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,   289,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,   254,   255,    -1,   257,   258,   259,
   260,   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,   289,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    -1,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,
    -1,    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,   254,
   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
    -1,    -1,   277,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,   289,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,   254,   255,    -1,   257,   258,   259,
   260,   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,   289,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    -1,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,
    -1,    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,   254,
   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
    -1,    -1,   277,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,   289,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    76,    77,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,
   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,    -1,    -1,    -1,   124,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   137,   138,    -1,
   140,    -1,   142,    -1,    -1,    -1,    -1,    -1,   148,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   171,   172,    -1,    -1,    -1,    -1,    -1,   178,   179,
    -1,    -1,    -1,    -1,    -1,    -1,   186,    -1,    -1,    -1,
    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,    -1,    -1,   203,    -1,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   262,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   274,   275,    -1,   277,    76,    77,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,    -1,    -1,    -1,   124,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   137,
   138,    -1,   140,    -1,   142,    -1,    -1,    -1,    -1,    -1,
   148,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,    -1,    -1,    -1,    -1,    -1,    -1,   186,    -1,
    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,
    -1,    -1,   200,    -1,    -1,   203,    -1,    -1,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,    -1,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   262,    60,    61,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   274,   275,    -1,   277,
    -1,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,    -1,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,
    -1,    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,   254,
   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
    -1,    -1,   277,    -1,    -1,    -1,   281,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,
    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,   254,   255,
    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
    -1,   277,    -1,    -1,   280,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    63,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    87,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    96,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,   275,    -1,   277,
   278,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    93,    -1,    -1,    96,    -1,    -1,    -1,   100,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,
    -1,    -1,    -1,    -1,   136,   137,   138,    -1,    -1,    -1,
    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,
   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,
    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,
    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,   254,   255,    -1,   257,   258,   259,   260,   261,
    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,   275,    -1,   277,   278,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,
    96,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,
    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,   254,   255,
    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,
    -1,   277,   278,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,   254,   255,    -1,   257,   258,   259,
   260,   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,   275,    -1,   277,   278,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,
    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,   143,
    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
   254,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,   278,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    74,    -1,    -1,    -1,
    -1,    -1,    80,    -1,    -1,    -1,    84,    -1,    86,    87,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,
    -1,    -1,   100,    -1,   102,   103,    -1,    -1,    -1,    -1,
   108,    -1,    -1,    -1,   112,    -1,    -1,    -1,   116,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,   146,    -1,
    -1,   149,    -1,   151,   152,    -1,   154,    -1,    -1,   157,
   158,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,   175,    -1,   177,
   178,   179,   180,   181,    -1,    -1,   184,    -1,   186,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   196,    -1,
    -1,    -1,   200,   201,   202,   203,   204,   205,   206,   207,
   208,   209,   210,   211,   212,   213,   214,   215,   216,   217,
   218,   219,   220,   221,   222,   223,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,   234,    -1,   236,   237,
   238,   239,   240,   241,   242,   243,   244,   245,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,   256,   257,
   258,   259,   260,   261,   262,   263,   264,   265,   266,   267,
   268,    -1,   270,   271,   272,   273,   274,    -1,    -1,   277,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    76,    77,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,    -1,    -1,    -1,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    78,    -1,    -1,    -1,    -1,    -1,    84,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   112,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,   159,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,
   188,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,   139,    -1,    -1,    -1,
   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,   184,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    87,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   116,    -1,   118,    -1,    -1,   121,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,
   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
   163,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,   184,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,
   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,   184,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,   245,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,
   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,   245,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    67,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,
   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,
   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,   254,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,
    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,   254,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    76,    77,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,    -1,    -1,
    -1,   124,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   137,   138,    -1,   140,    -1,   142,
    -1,    -1,    -1,    -1,    -1,   148,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   171,    -1,
    -1,    -1,    -1,    -1,    -1,   178,   179,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,    -1,    -1,
   203,    -1,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   260,    -1,   262,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   274,    -1,    -1,   277,    76,    77,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   110,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
    -1,    -1,    -1,   124,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   137,   138,    -1,   140,
    -1,   142,    -1,    -1,    -1,    -1,    -1,   148,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,
    -1,    -1,   203,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   262,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   274,    -1,    -1,   277
};
/* -*-C-*-  Note some compilers choke on comments on `#line' lines.  */
#line 3 "/usr/share/misc/bison.simple"
/* This file comes from bison-1.25.90.  */

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

/* This is the parser code that is written into each bison parser
  when the %semantic_parser declaration is not specified in the grammar.
  It was written by Richard Stallman by simplifying the hairy parser
  used when %semantic_parser is specified.  */

#ifndef YYSTACK_USE_ALLOCA
#ifdef alloca
#define YYSTACK_USE_ALLOCA
#else /* alloca not defined */
#ifdef __GNUC__
#define YYSTACK_USE_ALLOCA
#define alloca __builtin_alloca
#else /* not GNU C.  */
#if (!defined (__STDC__) && defined (sparc)) || defined (__sparc__) || defined (__sparc) || defined (__sgi) || (defined (__sun) && defined (__i386))
#define YYSTACK_USE_ALLOCA
#include <alloca.h>
#else /* not sparc */
/* We think this test detects Watcom and Microsoft C.  */
/* This used to test MSDOS, but that is a bad idea
   since that symbol is in the user namespace.  */
#if (defined (_MSDOS) || defined (_MSDOS_)) && !defined (__TURBOC__)
#if 0 /* No need for malloc.h, which pollutes the namespace;
	 instead, just don't use alloca.  */
#include <malloc.h>
#endif
#else /* not MSDOS, or __TURBOC__ */
#if defined(_AIX)
/* I don't know what this was needed for, but it pollutes the namespace.
   So I turned it off.   rms, 2 May 1997.  */
/* #include <malloc.h>  */
 #pragma alloca
#define YYSTACK_USE_ALLOCA
#else /* not MSDOS, or __TURBOC__, or _AIX */
#if 0
#ifdef __hpux /* haible@ilog.fr says this works for HPUX 9.05 and up,
		 and on HPUX 10.  Eventually we can turn this on.  */
#define YYSTACK_USE_ALLOCA
#define alloca __builtin_alloca
#endif /* __hpux */
#endif
#endif /* not _AIX */
#endif /* not MSDOS, or __TURBOC__ */
#endif /* not sparc */
#endif /* not GNU C */
#endif /* alloca not defined */
#endif /* YYSTACK_USE_ALLOCA not defined */

#ifdef YYSTACK_USE_ALLOCA
#define YYSTACK_ALLOC alloca
#else
#define YYSTACK_ALLOC malloc
#endif

/* Note: there must be only one dollar sign in this file.
   It is replaced by the list of actions, each action
   as one case of the switch.  */

#define yyerrok		(yyerrstatus = 0)
#define yyclearin	(yychar = YYEMPTY)
#define YYEMPTY		-2
#define YYEOF		0
#define YYACCEPT	goto yyacceptlab
#define YYABORT 	goto yyabortlab
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

/* Define __yy_memcpy.  Note that the size argument
   should be passed with type unsigned int, because that is what the non-GCC
   definitions require.  With GCC, __builtin_memcpy takes an arg
   of type size_t, but it can handle unsigned int.  */

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
     unsigned int count;
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
__yy_memcpy (char *to, char *from, unsigned int count)
{
  register char *t = to;
  register char *f = from;
  register int i = count;

  while (i-- > 0)
    *t++ = *f++;
}

#endif
#endif

#line 216 "/usr/share/misc/bison.simple"

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

/* Prevent warning if -Wstrict-prototypes.  */
#ifdef __GNUC__
#ifdef YYPARSE_PARAM
int yyparse (void *);
#else
int yyparse (void);
#endif
#endif

int
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
  int yyfree_stacks = 0;

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
	  if (yyfree_stacks)
	    {
	      free (yyss);
	      free (yyvs);
#ifdef YYLSP_NEEDED
	      free (yyls);
#endif
	    }
	  return 2;
	}
      yystacksize *= 2;
      if (yystacksize > YYMAXDEPTH)
	yystacksize = YYMAXDEPTH;
#ifndef YYSTACK_USE_ALLOCA
      yyfree_stacks = 1;
#endif
      yyss = (short *) YYSTACK_ALLOC (yystacksize * sizeof (*yyssp));
      __yy_memcpy ((char *)yyss, (char *)yyss1,
		   size * (unsigned int) sizeof (*yyssp));
      yyvs = (YYSTYPE *) YYSTACK_ALLOC (yystacksize * sizeof (*yyvsp));
      __yy_memcpy ((char *)yyvs, (char *)yyvs1,
		   size * (unsigned int) sizeof (*yyvsp));
#ifdef YYLSP_NEEDED
      yyls = (YYLTYPE *) YYSTACK_ALLOC (yystacksize * sizeof (*yylsp));
      __yy_memcpy ((char *)yyls, (char *)yyls1,
		   size * (unsigned int) sizeof (*yylsp));
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

case 4:
#line 813 "preproc.y"
{ connection = NULL; ;
    break;}
case 7:
#line 816 "preproc.y"
{ fprintf(yyout, "%s", yyvsp[0].str); free(yyvsp[0].str); ;
    break;}
case 8:
#line 817 "preproc.y"
{ fprintf(yyout, "%s", yyvsp[0].str); free(yyvsp[0].str); ;
    break;}
case 9:
#line 818 "preproc.y"
{ fputs(yyvsp[0].str, yyout); free(yyvsp[0].str); ;
    break;}
case 10:
#line 819 "preproc.y"
{ fputs(yyvsp[0].str, yyout); free(yyvsp[0].str); ;
    break;}
case 11:
#line 821 "preproc.y"
{ connection = yyvsp[0].str; ;
    break;}
case 12:
#line 823 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 13:
#line 824 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 14:
#line 825 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 15:
#line 826 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 16:
#line 827 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 17:
#line 828 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 18:
#line 829 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 19:
#line 830 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 20:
#line 831 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 21:
#line 832 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 22:
#line 833 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 23:
#line 834 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 24:
#line 835 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 25:
#line 836 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 26:
#line 837 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 27:
#line 838 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 28:
#line 839 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 29:
#line 840 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 30:
#line 841 "preproc.y"
{ output_statement(yyvsp[0].str, 1); ;
    break;}
case 31:
#line 842 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 32:
#line 843 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 33:
#line 844 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 34:
#line 845 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 35:
#line 846 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 36:
#line 847 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 37:
#line 848 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 38:
#line 849 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 39:
#line 850 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 40:
#line 851 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 41:
#line 852 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 42:
#line 853 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 43:
#line 854 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 44:
#line 855 "preproc.y"
{
						if (strncmp(yyvsp[0].str, "/* " , sizeof("/* ")-1) == 0)
						{
							fputs(yyvsp[0].str, yyout);
							free(yyvsp[0].str);
						}
						else
							output_statement(yyvsp[0].str, 1);
					;
    break;}
case 45:
#line 864 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 46:
#line 865 "preproc.y"
{
						fprintf(yyout, "ECPGtrans(__LINE__, %s, \"%s\");", connection ? connection : "NULL", yyvsp[0].str);
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 47:
#line 870 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 48:
#line 871 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 49:
#line 872 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 50:
#line 873 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 51:
#line 874 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 52:
#line 875 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 53:
#line 876 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 54:
#line 877 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 55:
#line 878 "preproc.y"
{
						if (connection)
							yyerror("no at option for connect statement.\n");

						fprintf(yyout, "no_auto_trans = %d;\n", no_auto_trans);
						fprintf(yyout, "ECPGconnect(__LINE__, %s);", yyvsp[0].str);
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 56:
#line 887 "preproc.y"
{
						fputs(yyvsp[0].str, yyout);
                                                free(yyvsp[0].str); 
					;
    break;}
case 57:
#line 891 "preproc.y"
{
						if (connection)
							yyerror("no at option for connect statement.\n");

						fputs(yyvsp[0].str, yyout);
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 58:
#line 899 "preproc.y"
{
						fputs(yyvsp[0].str, yyout);
						free(yyvsp[0].str);
					;
    break;}
case 59:
#line 903 "preproc.y"
{
						if (connection)
							yyerror("no at option for disconnect statement.\n");

						fprintf(yyout, "ECPGdisconnect(__LINE__, \"%s\");", yyvsp[0].str); 
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 60:
#line 911 "preproc.y"
{
						output_statement(yyvsp[0].str, 0);
					;
    break;}
case 61:
#line 914 "preproc.y"
{
						fprintf(yyout, "ECPGdeallocate(__LINE__, %s, \"%s\");", connection ? connection : "NULL", yyvsp[0].str); 
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 62:
#line 919 "preproc.y"
{	
						struct cursor *ptr;
						 
						for (ptr = cur; ptr != NULL; ptr=ptr->next)
						{
					               if (strcmp(ptr->name, yyvsp[0].str) == 0)
						       		break;
						}
						
						if (ptr == NULL)
						{
							sprintf(errortext, "trying to open undeclared cursor %s\n", yyvsp[0].str);
							yyerror(errortext);
						}
                  
						fprintf(yyout, "ECPGdo(__LINE__, %s, \"%s\",", ptr->connection ? ptr->connection : "NULL", ptr->command);
						/* dump variables to C file*/
						dump_variables(ptr->argsinsert, 0);
						dump_variables(argsinsert, 0);
						fputs("ECPGt_EOIT, ", yyout);
						dump_variables(ptr->argsresult, 0);
						fputs("ECPGt_EORT);", yyout);
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 63:
#line 944 "preproc.y"
{
						if (connection)
							yyerror("no at option for set connection statement.\n");

						fprintf(yyout, "ECPGprepare(__LINE__, %s);", yyvsp[0].str); 
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 64:
#line 952 "preproc.y"
{ /* output already done */ ;
    break;}
case 65:
#line 953 "preproc.y"
{
						if (connection)
							yyerror("no at option for set connection statement.\n");

						fprintf(yyout, "ECPGsetconn(__LINE__, %s);", yyvsp[0].str);
						whenever_action(0);
                                       		free(yyvsp[0].str);
					;
    break;}
case 66:
#line 961 "preproc.y"
{
						if (connection)
							yyerror("no at option for typedef statement.\n");

						fputs(yyvsp[0].str, yyout);
                                                free(yyvsp[0].str);
					;
    break;}
case 67:
#line 968 "preproc.y"
{
						if (connection)
							yyerror("no at option for var statement.\n");

						fputs(yyvsp[0].str, yyout);
                                                free(yyvsp[0].str);
					;
    break;}
case 68:
#line 975 "preproc.y"
{
						if (connection)
							yyerror("no at option for whenever statement.\n");

						fputs(yyvsp[0].str, yyout);
						output_line_number();
						free(yyvsp[0].str);
					;
    break;}
case 69:
#line 999 "preproc.y"
{
					yyval.str = cat3_str(cat5_str(make1_str("create user"), yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 70:
#line 1013 "preproc.y"
{
					yyval.str = cat3_str(cat5_str(make1_str("alter user"), yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 71:
#line 1026 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop user"), yyvsp[0].str);
				;
    break;}
case 72:
#line 1031 "preproc.y"
{ yyval.str = cat2_str(make1_str("with password") , yyvsp[0].str); ;
    break;}
case 73:
#line 1032 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 74:
#line 1036 "preproc.y"
{
					yyval.str = make1_str("createdb");
				;
    break;}
case 75:
#line 1040 "preproc.y"
{
					yyval.str = make1_str("nocreatedb");
				;
    break;}
case 76:
#line 1043 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 77:
#line 1047 "preproc.y"
{
					yyval.str = make1_str("createuser");
				;
    break;}
case 78:
#line 1051 "preproc.y"
{
					yyval.str = make1_str("nocreateuser");
				;
    break;}
case 79:
#line 1054 "preproc.y"
{ yyval.str = NULL; ;
    break;}
case 80:
#line 1058 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 81:
#line 1062 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 82:
#line 1067 "preproc.y"
{ yyval.str = cat2_str(make1_str("in group"), yyvsp[0].str); ;
    break;}
case 83:
#line 1068 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 84:
#line 1071 "preproc.y"
{ yyval.str = cat2_str(make1_str("valid until"), yyvsp[0].str);; ;
    break;}
case 85:
#line 1072 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 86:
#line 1085 "preproc.y"
{
					yyval.str = cat4_str(make1_str("set"), yyvsp[-2].str, make1_str("to"), yyvsp[0].str);
				;
    break;}
case 87:
#line 1089 "preproc.y"
{
					yyval.str = cat4_str(make1_str("set"), yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 88:
#line 1093 "preproc.y"
{
					yyval.str = cat2_str(make1_str("set time zone"), yyvsp[0].str);
				;
    break;}
case 89:
#line 1097 "preproc.y"
{
					if (strcasecmp(yyvsp[0].str, "COMMITTED"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[0].str);
						yyerror(errortext);
					}

					yyval.str = cat2_str(make1_str("set transaction isolation level read"), yyvsp[0].str);
				;
    break;}
case 90:
#line 1107 "preproc.y"
{
					if (strcasecmp(yyvsp[0].str, "SERIALIZABLE"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[0].str);
                                                yyerror(errortext);
					}

					yyval.str = cat2_str(make1_str("set transaction isolation level read"), yyvsp[0].str);
				;
    break;}
case 91:
#line 1117 "preproc.y"
{
#ifdef MB
					yyval.str = cat2_str(make1_str("set names"), yyvsp[0].str);
#else
                                        yyerror("SET NAMES is not supported");
#endif
                                ;
    break;}
case 92:
#line 1126 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 93:
#line 1127 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 94:
#line 1130 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 95:
#line 1131 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 96:
#line 1132 "preproc.y"
{ yyval.str = make1_str("local"); ;
    break;}
case 97:
#line 1136 "preproc.y"
{
					yyval.str = cat2_str(make1_str("show"), yyvsp[0].str);
				;
    break;}
case 98:
#line 1140 "preproc.y"
{
					yyval.str = make1_str("show time zone");
				;
    break;}
case 99:
#line 1144 "preproc.y"
{
					yyval.str = make1_str("show transaction isolation level");
				;
    break;}
case 100:
#line 1150 "preproc.y"
{
					yyval.str = cat2_str(make1_str("reset"), yyvsp[0].str);
				;
    break;}
case 101:
#line 1154 "preproc.y"
{
					yyval.str = make1_str("reset time zone");
				;
    break;}
case 102:
#line 1158 "preproc.y"
{
					yyval.str = make1_str("reset transaction isolation level");
				;
    break;}
case 103:
#line 1172 "preproc.y"
{
					yyval.str = cat4_str(make1_str("alter table"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 104:
#line 1178 "preproc.y"
{
					yyval.str = cat3_str(make1_str("add"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 105:
#line 1182 "preproc.y"
{
					yyval.str = make3_str(make1_str("add("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 106:
#line 1186 "preproc.y"
{	yyerror("ALTER TABLE/DROP COLUMN not yet implemented"); ;
    break;}
case 107:
#line 1188 "preproc.y"
{	yyerror("ALTER TABLE/ALTER COLUMN/SET DEFAULT not yet implemented"); ;
    break;}
case 108:
#line 1190 "preproc.y"
{	yyerror("ALTER TABLE/ALTER COLUMN/DROP DEFAULT not yet implemented"); ;
    break;}
case 109:
#line 1192 "preproc.y"
{	yyerror("ALTER TABLE/ADD CONSTRAINT not yet implemented"); ;
    break;}
case 110:
#line 1203 "preproc.y"
{
					yyval.str = cat2_str(make1_str("close"), yyvsp[0].str);
				;
    break;}
case 111:
#line 1218 "preproc.y"
{
					yyval.str = cat3_str(cat5_str(make1_str("copy"), yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 112:
#line 1224 "preproc.y"
{ yyval.str = make1_str("to"); ;
    break;}
case 113:
#line 1226 "preproc.y"
{ yyval.str = make1_str("from"); ;
    break;}
case 114:
#line 1234 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 115:
#line 1235 "preproc.y"
{ yyval.str = make1_str("stdin"); ;
    break;}
case 116:
#line 1236 "preproc.y"
{ yyval.str = make1_str("stdout"); ;
    break;}
case 117:
#line 1239 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 118:
#line 1240 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 119:
#line 1243 "preproc.y"
{ yyval.str = make1_str("with oids"); ;
    break;}
case 120:
#line 1244 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 121:
#line 1250 "preproc.y"
{ yyval.str = cat2_str(make1_str("using delimiters"), yyvsp[0].str); ;
    break;}
case 122:
#line 1251 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 123:
#line 1265 "preproc.y"
{
					yyval.str = cat3_str(cat4_str(make1_str("create"), yyvsp[-6].str, make1_str("table"), yyvsp[-4].str), make3_str(make1_str("("), yyvsp[-2].str, make1_str(")")), yyvsp[0].str);
				;
    break;}
case 124:
#line 1270 "preproc.y"
{ yyval.str = make1_str("temp"); ;
    break;}
case 125:
#line 1271 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 126:
#line 1275 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 127:
#line 1279 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 128:
#line 1282 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 129:
#line 1285 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 130:
#line 1286 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 131:
#line 1290 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 132:
#line 1294 "preproc.y"
{
			yyval.str = make3_str(yyvsp[-2].str, make1_str(" serial "), yyvsp[0].str);
		;
    break;}
case 133:
#line 1299 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 134:
#line 1300 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 135:
#line 1303 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str,yyvsp[0].str); ;
    break;}
case 136:
#line 1304 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 137:
#line 1308 "preproc.y"
{
			yyval.str = make1_str("primary key");
                ;
    break;}
case 138:
#line 1312 "preproc.y"
{
			yyval.str = make1_str("");
		;
    break;}
case 139:
#line 1319 "preproc.y"
{
					yyval.str = cat3_str(make1_str("constraint"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 140:
#line 1323 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 141:
#line 1342 "preproc.y"
{
					yyval.str = make3_str(make1_str("check("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 142:
#line 1346 "preproc.y"
{
					yyval.str = make1_str("default null");
				;
    break;}
case 143:
#line 1350 "preproc.y"
{
					yyval.str = cat2_str(make1_str("default"), yyvsp[0].str);
				;
    break;}
case 144:
#line 1354 "preproc.y"
{
					yyval.str = make1_str("not null");
				;
    break;}
case 145:
#line 1358 "preproc.y"
{
					yyval.str = make1_str("unique");
				;
    break;}
case 146:
#line 1362 "preproc.y"
{
					yyval.str = make1_str("primary key");
				;
    break;}
case 147:
#line 1366 "preproc.y"
{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					yyval.str = make1_str("");
				;
    break;}
case 148:
#line 1373 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 149:
#line 1377 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 150:
#line 1391 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 151:
#line 1393 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 152:
#line 1395 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 153:
#line 1397 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 154:
#line 1399 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 155:
#line 1401 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 156:
#line 1403 "preproc.y"
{	yyerror("boolean expressions not supported in DEFAULT"); ;
    break;}
case 157:
#line 1405 "preproc.y"
{	yyerror("boolean expressions not supported in DEFAULT"); ;
    break;}
case 158:
#line 1407 "preproc.y"
{	yyerror("boolean expressions not supported in DEFAULT"); ;
    break;}
case 159:
#line 1413 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 160:
#line 1415 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 161:
#line 1417 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str); ;
    break;}
case 162:
#line 1419 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str) , make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 163:
#line 1423 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 164:
#line 1425 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); ;
    break;}
case 165:
#line 1427 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); ;
    break;}
case 166:
#line 1429 "preproc.y"
{
					if (!strcmp("<=", yyvsp[-1].str) || !strcmp(">=", yyvsp[-1].str))
						yyerror("boolean expressions not supported in DEFAULT");
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 167:
#line 1435 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 168:
#line 1437 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 169:
#line 1440 "preproc.y"
{	yyval.str = make1_str("current_date"); ;
    break;}
case 170:
#line 1442 "preproc.y"
{	yyval.str = make1_str("current_time"); ;
    break;}
case 171:
#line 1444 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr, "CURRENT_TIME(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = "current_time";
				;
    break;}
case 172:
#line 1450 "preproc.y"
{	yyval.str = make1_str("current_timestamp"); ;
    break;}
case 173:
#line 1452 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr, "CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = "current_timestamp";
				;
    break;}
case 174:
#line 1458 "preproc.y"
{	yyval.str = make1_str("current_user"); ;
    break;}
case 175:
#line 1460 "preproc.y"
{       yyval.str = make1_str("user"); ;
    break;}
case 176:
#line 1468 "preproc.y"
{
						yyval.str = cat3_str(make1_str("constraint"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 177:
#line 1472 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 178:
#line 1476 "preproc.y"
{
					yyval.str = make3_str(make1_str("check("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 179:
#line 1480 "preproc.y"
{
					yyval.str = make3_str(make1_str("unique("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 180:
#line 1484 "preproc.y"
{
					yyval.str = make3_str(make1_str("primary key("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 181:
#line 1488 "preproc.y"
{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					yyval.str = "";
				;
    break;}
case 182:
#line 1495 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 183:
#line 1499 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 184:
#line 1505 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 185:
#line 1507 "preproc.y"
{	yyval.str = make1_str("null"); ;
    break;}
case 186:
#line 1509 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 187:
#line 1513 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 188:
#line 1515 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 189:
#line 1517 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 190:
#line 1519 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 191:
#line 1521 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 192:
#line 1523 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 193:
#line 1525 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 194:
#line 1527 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 195:
#line 1533 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 196:
#line 1535 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 197:
#line 1537 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 198:
#line 1541 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 199:
#line 1545 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 200:
#line 1547 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); }
				;
    break;}
case 201:
#line 1551 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 202:
#line 1555 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 203:
#line 1557 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 204:
#line 1559 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 205:
#line 1561 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 206:
#line 1563 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 207:
#line 1565 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 208:
#line 1567 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 209:
#line 1569 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 210:
#line 1571 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 211:
#line 1573 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 212:
#line 1575 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 213:
#line 1577 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 214:
#line 1579 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); ;
    break;}
case 215:
#line 1581 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); ;
    break;}
case 216:
#line 1583 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); ;
    break;}
case 217:
#line 1585 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); ;
    break;}
case 218:
#line 1587 "preproc.y"
{	yyval.str = cat4_str(yyvsp[-4].str, make1_str("in ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 219:
#line 1589 "preproc.y"
{	yyval.str = cat4_str(yyvsp[-5].str, make1_str("not in ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 220:
#line 1591 "preproc.y"
{	yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 221:
#line 1593 "preproc.y"
{	yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 222:
#line 1596 "preproc.y"
{
		yyval.str = make3_str(yyvsp[-2].str, make1_str(", "), yyvsp[0].str);
	;
    break;}
case 223:
#line 1600 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 224:
#line 1605 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 225:
#line 1609 "preproc.y"
{ yyval.str = make1_str("match full"); ;
    break;}
case 226:
#line 1610 "preproc.y"
{ yyval.str = make1_str("match partial"); ;
    break;}
case 227:
#line 1611 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 228:
#line 1614 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 229:
#line 1615 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 230:
#line 1616 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 231:
#line 1619 "preproc.y"
{ yyval.str = cat2_str(make1_str("on delete"), yyvsp[0].str); ;
    break;}
case 232:
#line 1620 "preproc.y"
{ yyval.str = cat2_str(make1_str("on update"), yyvsp[0].str); ;
    break;}
case 233:
#line 1623 "preproc.y"
{ yyval.str = make1_str("no action"); ;
    break;}
case 234:
#line 1624 "preproc.y"
{ yyval.str = make1_str("cascade"); ;
    break;}
case 235:
#line 1625 "preproc.y"
{ yyval.str = make1_str("set default"); ;
    break;}
case 236:
#line 1626 "preproc.y"
{ yyval.str = make1_str("set null"); ;
    break;}
case 237:
#line 1629 "preproc.y"
{ yyval.str = make3_str(make1_str("inherits ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 238:
#line 1630 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 239:
#line 1634 "preproc.y"
{
			yyval.str = cat5_str(cat3_str(make1_str("create"), yyvsp[-5].str, make1_str("table")), yyvsp[-3].str, yyvsp[-2].str, make1_str("as"), yyvsp[0].str); 
		;
    break;}
case 240:
#line 1639 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 241:
#line 1640 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 242:
#line 1643 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 243:
#line 1644 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 244:
#line 1647 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 245:
#line 1658 "preproc.y"
{
					yyval.str = cat3_str(make1_str("create sequence"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 246:
#line 1664 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 247:
#line 1665 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 248:
#line 1669 "preproc.y"
{
					yyval.str = cat2_str(make1_str("cache"), yyvsp[0].str);
				;
    break;}
case 249:
#line 1673 "preproc.y"
{
					yyval.str = make1_str("cycle");
				;
    break;}
case 250:
#line 1677 "preproc.y"
{
					yyval.str = cat2_str(make1_str("increment"), yyvsp[0].str);
				;
    break;}
case 251:
#line 1681 "preproc.y"
{
					yyval.str = cat2_str(make1_str("maxvalue"), yyvsp[0].str);
				;
    break;}
case 252:
#line 1685 "preproc.y"
{
					yyval.str = cat2_str(make1_str("minvalue"), yyvsp[0].str);
				;
    break;}
case 253:
#line 1689 "preproc.y"
{
					yyval.str = cat2_str(make1_str("start"), yyvsp[0].str);
				;
    break;}
case 254:
#line 1694 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 255:
#line 1695 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 256:
#line 1698 "preproc.y"
{
                                       yyval.str = yyvsp[0].str;
                               ;
    break;}
case 257:
#line 1702 "preproc.y"
{
                                       yyval.str = cat2_str(make1_str("-"), yyvsp[0].str);
                               ;
    break;}
case 258:
#line 1709 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 259:
#line 1713 "preproc.y"
{
					yyval.str = cat2_str(make1_str("-"), yyvsp[0].str);
				;
    break;}
case 260:
#line 1728 "preproc.y"
{
				yyval.str = cat4_str(cat5_str(make1_str("create"), yyvsp[-7].str, make1_str("precedural language"), yyvsp[-4].str, make1_str("handler")), yyvsp[-2].str, make1_str("langcompiler"), yyvsp[0].str);
			;
    break;}
case 261:
#line 1733 "preproc.y"
{ yyval.str = make1_str("trusted"); ;
    break;}
case 262:
#line 1734 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 263:
#line 1737 "preproc.y"
{
				yyval.str = cat2_str(make1_str("drop procedural language"), yyvsp[0].str);
			;
    break;}
case 264:
#line 1753 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create trigger"), yyvsp[-11].str, yyvsp[-10].str, yyvsp[-9].str, make1_str("on")), yyvsp[-7].str, yyvsp[-6].str, make1_str("execute procedure"), yyvsp[-3].str), make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 265:
#line 1758 "preproc.y"
{ yyval.str = make1_str("before"); ;
    break;}
case 266:
#line 1759 "preproc.y"
{ yyval.str = make1_str("after"); ;
    break;}
case 267:
#line 1763 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 268:
#line 1767 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str);
				;
    break;}
case 269:
#line 1771 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("or"), yyvsp[-2].str, make1_str("or"), yyvsp[0].str);
				;
    break;}
case 270:
#line 1776 "preproc.y"
{ yyval.str = make1_str("insert"); ;
    break;}
case 271:
#line 1777 "preproc.y"
{ yyval.str = make1_str("delete"); ;
    break;}
case 272:
#line 1778 "preproc.y"
{ yyval.str = make1_str("update"); ;
    break;}
case 273:
#line 1782 "preproc.y"
{
					yyval.str = cat3_str(make1_str("for"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 274:
#line 1787 "preproc.y"
{ yyval.str = make1_str("each"); ;
    break;}
case 275:
#line 1788 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 276:
#line 1791 "preproc.y"
{ yyval.str = make1_str("row"); ;
    break;}
case 277:
#line 1792 "preproc.y"
{ yyval.str = make1_str("statement"); ;
    break;}
case 278:
#line 1796 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 279:
#line 1798 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 280:
#line 1800 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 281:
#line 1804 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 282:
#line 1808 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 283:
#line 1811 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 284:
#line 1812 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 285:
#line 1816 "preproc.y"
{
					yyval.str = cat4_str(make1_str("drop trigger"), yyvsp[-2].str, make1_str("on"), yyvsp[0].str);
				;
    break;}
case 286:
#line 1829 "preproc.y"
{
					yyval.str = cat3_str(make1_str("create"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 287:
#line 1835 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 288:
#line 1840 "preproc.y"
{ yyval.str = make1_str("operator"); ;
    break;}
case 289:
#line 1841 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 290:
#line 1842 "preproc.y"
{ yyval.str = make1_str("aggregate"); ;
    break;}
case 291:
#line 1845 "preproc.y"
{ yyval.str = make1_str("procedure"); ;
    break;}
case 292:
#line 1846 "preproc.y"
{ yyval.str = make1_str("join"); ;
    break;}
case 293:
#line 1847 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 294:
#line 1848 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 295:
#line 1849 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 296:
#line 1852 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 297:
#line 1855 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 298:
#line 1856 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 299:
#line 1859 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 300:
#line 1863 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 301:
#line 1867 "preproc.y"
{
					yyval.str = cat2_str(make1_str("default ="), yyvsp[0].str);
				;
    break;}
case 302:
#line 1872 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 303:
#line 1873 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 304:
#line 1874 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 305:
#line 1875 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 306:
#line 1877 "preproc.y"
{
					yyval.str = cat2_str(make1_str("setof"), yyvsp[0].str);
				;
    break;}
case 307:
#line 1890 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop table"), yyvsp[0].str);
				;
    break;}
case 308:
#line 1894 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop sequence"), yyvsp[0].str);
				;
    break;}
case 309:
#line 1911 "preproc.y"
{
					if (strncmp(yyvsp[-4].str, "relative", strlen("relative")) == 0 && atol(yyvsp[-3].str) == 0L)
						yyerror("FETCH/RELATIVE at current position is not supported");

					yyval.str = cat4_str(make1_str("fetch"), yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str);
				;
    break;}
case 310:
#line 1918 "preproc.y"
{
					yyval.str = cat4_str(make1_str("fetch"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 311:
#line 1923 "preproc.y"
{ yyval.str = make1_str("forward"); ;
    break;}
case 312:
#line 1924 "preproc.y"
{ yyval.str = make1_str("backward"); ;
    break;}
case 313:
#line 1925 "preproc.y"
{ yyval.str = make1_str("relative"); ;
    break;}
case 314:
#line 1927 "preproc.y"
{
					fprintf(stderr, "FETCH/ABSOLUTE not supported, using RELATIVE");
					yyval.str = make1_str("absolute");
				;
    break;}
case 315:
#line 1931 "preproc.y"
{ yyval.str = make1_str(""); /* default */ ;
    break;}
case 316:
#line 1934 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 317:
#line 1935 "preproc.y"
{ yyval.str = make2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 318:
#line 1936 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 319:
#line 1937 "preproc.y"
{ yyval.str = make1_str("next"); ;
    break;}
case 320:
#line 1938 "preproc.y"
{ yyval.str = make1_str("prior"); ;
    break;}
case 321:
#line 1939 "preproc.y"
{ yyval.str = make1_str(""); /*default*/ ;
    break;}
case 322:
#line 1942 "preproc.y"
{ yyval.str = cat2_str(make1_str("in"), yyvsp[0].str); ;
    break;}
case 323:
#line 1943 "preproc.y"
{ yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 324:
#line 1945 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 325:
#line 1957 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("grant"), yyvsp[-5].str, make1_str("on"), yyvsp[-3].str, make1_str("to")), yyvsp[-1].str);
				;
    break;}
case 326:
#line 1963 "preproc.y"
{
				 yyval.str = make1_str("all privileges");
				;
    break;}
case 327:
#line 1967 "preproc.y"
{
				 yyval.str = make1_str("all");
				;
    break;}
case 328:
#line 1971 "preproc.y"
{
				 yyval.str = yyvsp[0].str;
				;
    break;}
case 329:
#line 1977 "preproc.y"
{
						yyval.str = yyvsp[0].str;
				;
    break;}
case 330:
#line 1981 "preproc.y"
{
						yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 331:
#line 1987 "preproc.y"
{
						yyval.str = make1_str("select");
				;
    break;}
case 332:
#line 1991 "preproc.y"
{
						yyval.str = make1_str("insert");
				;
    break;}
case 333:
#line 1995 "preproc.y"
{
						yyval.str = make1_str("update");
				;
    break;}
case 334:
#line 1999 "preproc.y"
{
						yyval.str = make1_str("delete");
				;
    break;}
case 335:
#line 2003 "preproc.y"
{
						yyval.str = make1_str("rule");
				;
    break;}
case 336:
#line 2009 "preproc.y"
{
						yyval.str = make1_str("public");
				;
    break;}
case 337:
#line 2013 "preproc.y"
{
						yyval.str = cat2_str(make1_str("group"), yyvsp[0].str);
				;
    break;}
case 338:
#line 2017 "preproc.y"
{
						yyval.str = yyvsp[0].str;
				;
    break;}
case 339:
#line 2023 "preproc.y"
{
					yyerror("WITH GRANT OPTION is not supported.  Only relation owners can set privileges");
				 ;
    break;}
case 341:
#line 2038 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("revoke"), yyvsp[-4].str, make1_str("on"), yyvsp[-2].str, make1_str("from")), yyvsp[0].str);
				;
    break;}
case 342:
#line 2057 "preproc.y"
{
					/* should check that access_method is valid,
					   etc ... but doesn't */
					yyval.str = cat5_str(cat5_str(make1_str("create"), yyvsp[-9].str, make1_str("index"), yyvsp[-7].str, make1_str("on")), yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("("), yyvsp[-2].str, make1_str(")")), yyvsp[0].str);
				;
    break;}
case 343:
#line 2064 "preproc.y"
{ yyval.str = make1_str("unique"); ;
    break;}
case 344:
#line 2065 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 345:
#line 2068 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 346:
#line 2069 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 347:
#line 2072 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 348:
#line 2073 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 349:
#line 2076 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 350:
#line 2077 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 351:
#line 2081 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-5].str, make3_str(make1_str("("), yyvsp[-3].str, ")"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 352:
#line 2087 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 353:
#line 2092 "preproc.y"
{ yyval.str = cat2_str(make1_str(":"), yyvsp[0].str); ;
    break;}
case 354:
#line 2093 "preproc.y"
{ yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 355:
#line 2094 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 356:
#line 2103 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 357:
#line 2104 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 358:
#line 2105 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 359:
#line 2116 "preproc.y"
{
					yyval.str = cat3_str(make1_str("extend index"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 360:
#line 2130 "preproc.y"
{
					yyval.str = cat2_str(make1_str("execute recipe"), yyvsp[0].str);
				;
    break;}
case 361:
#line 2153 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create function"), yyvsp[-8].str, yyvsp[-7].str, make1_str("returns"), yyvsp[-5].str), yyvsp[-4].str, make1_str("as"), yyvsp[-2].str, make1_str("language")), yyvsp[0].str);
				;
    break;}
case 362:
#line 2157 "preproc.y"
{ yyval.str = cat2_str(make1_str("with"), yyvsp[0].str); ;
    break;}
case 363:
#line 2158 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 364:
#line 2161 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 365:
#line 2162 "preproc.y"
{ yyval.str = make1_str("()"); ;
    break;}
case 366:
#line 2165 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 367:
#line 2167 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 368:
#line 2171 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 369:
#line 2176 "preproc.y"
{ yyval.str = make1_str("setof"); ;
    break;}
case 370:
#line 2177 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 371:
#line 2199 "preproc.y"
{
					yyval.str = cat3_str(make1_str("drop"), yyvsp[-1].str, yyvsp[0].str);;
				;
    break;}
case 372:
#line 2204 "preproc.y"
{  yyval.str = make1_str("type"); ;
    break;}
case 373:
#line 2205 "preproc.y"
{  yyval.str = make1_str("index"); ;
    break;}
case 374:
#line 2206 "preproc.y"
{  yyval.str = make1_str("rule"); ;
    break;}
case 375:
#line 2207 "preproc.y"
{  yyval.str = make1_str("view"); ;
    break;}
case 376:
#line 2212 "preproc.y"
{
						yyval.str = cat3_str(make1_str("drop aggregate"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 377:
#line 2217 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 378:
#line 2218 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 379:
#line 2223 "preproc.y"
{
						yyval.str = cat3_str(make1_str("drop function"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 380:
#line 2230 "preproc.y"
{
					yyval.str = cat3_str(make1_str("drop operator"), yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 383:
#line 2237 "preproc.y"
{ yyval.str = make1_str("+"); ;
    break;}
case 384:
#line 2238 "preproc.y"
{ yyval.str = make1_str("-"); ;
    break;}
case 385:
#line 2239 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 386:
#line 2240 "preproc.y"
{ yyval.str = make1_str("/"); ;
    break;}
case 387:
#line 2241 "preproc.y"
{ yyval.str = make1_str("<"); ;
    break;}
case 388:
#line 2242 "preproc.y"
{ yyval.str = make1_str(">"); ;
    break;}
case 389:
#line 2243 "preproc.y"
{ yyval.str = make1_str("="); ;
    break;}
case 390:
#line 2247 "preproc.y"
{
				   yyerror("parser: argument type missing (use NONE for unary operators)");
				;
    break;}
case 391:
#line 2251 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 392:
#line 2253 "preproc.y"
{ yyval.str = cat2_str(make1_str("none,"), yyvsp[0].str); ;
    break;}
case 393:
#line 2255 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-2].str, make1_str(", none")); ;
    break;}
case 394:
#line 2269 "preproc.y"
{
					yyval.str = cat4_str(cat5_str(make1_str("alter table"), yyvsp[-6].str, yyvsp[-5].str, make1_str("rename"), yyvsp[-3].str), yyvsp[-2].str, make1_str("to"), yyvsp[0].str);
				;
    break;}
case 395:
#line 2274 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 396:
#line 2275 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 397:
#line 2278 "preproc.y"
{ yyval.str = make1_str("colmunn"); ;
    break;}
case 398:
#line 2279 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 399:
#line 2293 "preproc.y"
{ QueryIsRule=1; ;
    break;}
case 400:
#line 2296 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create rule"), yyvsp[-10].str, make1_str("as on"), yyvsp[-6].str, make1_str("to")), yyvsp[-4].str, yyvsp[-3].str, make1_str("do"), yyvsp[-1].str), yyvsp[0].str);
				;
    break;}
case 401:
#line 2301 "preproc.y"
{ yyval.str = make1_str("nothing"); ;
    break;}
case 402:
#line 2302 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 403:
#line 2303 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 404:
#line 2304 "preproc.y"
{ yyval.str = cat3_str(make1_str("["), yyvsp[-1].str, make1_str("]")); ;
    break;}
case 405:
#line 2305 "preproc.y"
{ yyval.str = cat3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 406:
#line 2308 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 407:
#line 2309 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 408:
#line 2313 "preproc.y"
{  yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 409:
#line 2315 "preproc.y"
{  yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, make1_str(";")); ;
    break;}
case 410:
#line 2317 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, make1_str(";")); ;
    break;}
case 415:
#line 2327 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 416:
#line 2331 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 417:
#line 2337 "preproc.y"
{ yyval.str = make1_str("select"); ;
    break;}
case 418:
#line 2338 "preproc.y"
{ yyval.str = make1_str("update"); ;
    break;}
case 419:
#line 2339 "preproc.y"
{ yyval.str = make1_str("delete"); ;
    break;}
case 420:
#line 2340 "preproc.y"
{ yyval.str = make1_str("insert"); ;
    break;}
case 421:
#line 2343 "preproc.y"
{ yyval.str = make1_str("instead"); ;
    break;}
case 422:
#line 2344 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 423:
#line 2357 "preproc.y"
{
					yyval.str = cat2_str(make1_str("notify"), yyvsp[0].str);
				;
    break;}
case 424:
#line 2363 "preproc.y"
{
					yyval.str = cat2_str(make1_str("listen"), yyvsp[0].str);
                                ;
    break;}
case 425:
#line 2369 "preproc.y"
{
					yyval.str = cat2_str(make1_str("unlisten"), yyvsp[0].str);
                                ;
    break;}
case 426:
#line 2373 "preproc.y"
{
					yyval.str = make1_str("unlisten *");
                                ;
    break;}
case 427:
#line 2390 "preproc.y"
{ yyval.str = make1_str("rollback"); ;
    break;}
case 428:
#line 2391 "preproc.y"
{ yyval.str = make1_str("begin transaction"); ;
    break;}
case 429:
#line 2392 "preproc.y"
{ yyval.str = make1_str("commit"); ;
    break;}
case 430:
#line 2393 "preproc.y"
{ yyval.str = make1_str("commit"); ;
    break;}
case 431:
#line 2394 "preproc.y"
{ yyval.str = make1_str("rollback"); ;
    break;}
case 432:
#line 2396 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 433:
#line 2397 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 434:
#line 2398 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 435:
#line 2409 "preproc.y"
{
					yyval.str = cat4_str(make1_str("create view"), yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
				;
    break;}
case 436:
#line 2423 "preproc.y"
{
					yyval.str = cat2_str(make1_str("load"), yyvsp[0].str);
				;
    break;}
case 437:
#line 2437 "preproc.y"
{
					if (strlen(yyvsp[-1].str) == 0 || strlen(yyvsp[0].str) == 0) 
						yyerror("CREATE DATABASE WITH requires at least an option");
#ifndef MULTIBYTE
					if (strlen(yyvsp[0].str) != 0)
						yyerror("WITH ENCODING is not supported");
#endif
					yyval.str = cat5_str(make1_str("create database"), yyvsp[-3].str, make1_str("with"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 438:
#line 2447 "preproc.y"
{
					yyval.str = cat2_str(make1_str("create database"), yyvsp[0].str);
				;
    break;}
case 439:
#line 2452 "preproc.y"
{ yyval.str = cat2_str(make1_str("location ="), yyvsp[0].str); ;
    break;}
case 440:
#line 2453 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 441:
#line 2456 "preproc.y"
{ yyval.str = cat2_str(make1_str("encoding ="), yyvsp[0].str); ;
    break;}
case 442:
#line 2457 "preproc.y"
{ yyval.str = NULL; ;
    break;}
case 443:
#line 2460 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 444:
#line 2461 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 445:
#line 2462 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 446:
#line 2465 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 447:
#line 2466 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 448:
#line 2467 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 449:
#line 2478 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop database"), yyvsp[0].str);
				;
    break;}
case 450:
#line 2492 "preproc.y"
{
				   yyval.str = cat4_str(make1_str("cluster"), yyvsp[-2].str, make1_str("on"), yyvsp[0].str);
				;
    break;}
case 451:
#line 2506 "preproc.y"
{
					yyval.str = cat3_str(make1_str("vacuum"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 452:
#line 2510 "preproc.y"
{
					if ( strlen(yyvsp[0].str) > 0 && strlen(yyvsp[-1].str) == 0 )
						yyerror("parser: syntax error at or near \"(\"");
					yyval.str = cat5_str(make1_str("vacuum"), yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 453:
#line 2517 "preproc.y"
{ yyval.str = make1_str("verbose"); ;
    break;}
case 454:
#line 2518 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 455:
#line 2521 "preproc.y"
{ yyval.str = make1_str("analyse"); ;
    break;}
case 456:
#line 2522 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 457:
#line 2525 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 458:
#line 2526 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 459:
#line 2530 "preproc.y"
{ yyval.str=yyvsp[0].str; ;
    break;}
case 460:
#line 2532 "preproc.y"
{ yyval.str=cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 461:
#line 2544 "preproc.y"
{
					yyval.str = cat3_str(make1_str("explain"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 468:
#line 2584 "preproc.y"
{
					yyval.str = cat3_str(make1_str("insert into"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 469:
#line 2590 "preproc.y"
{
					yyval.str = make3_str(make1_str("values("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 470:
#line 2594 "preproc.y"
{
					yyval.str = make1_str("default values");
				;
    break;}
case 471:
#line 2598 "preproc.y"
{
					yyval.str = yyvsp[0].str
				;
    break;}
case 472:
#line 2602 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-5].str, make1_str(") values ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 473:
#line 2606 "preproc.y"
{
					yyval.str = make4_str(make1_str("("), yyvsp[-2].str, make1_str(")"), yyvsp[0].str);
				;
    break;}
case 474:
#line 2611 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 475:
#line 2612 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 476:
#line 2617 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 477:
#line 2619 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 478:
#line 2623 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 479:
#line 2638 "preproc.y"
{
					yyval.str = cat3_str(make1_str("delete from"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 480:
#line 2644 "preproc.y"
{
					yyval.str = cat3_str(make1_str("lock"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 481:
#line 2648 "preproc.y"
{
					if (strcasecmp(yyvsp[0].str, "MODE"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[0].str);
						yyerror(errortext);
					}
					if (yyvsp[-3].str != NULL)
                                        {
                                                if (strcasecmp(yyvsp[-3].str, "SHARE"))
						{
                                                        sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[-3].str);
	                                                yyerror(errortext);
						}
                                                if (strcasecmp(yyvsp[-1].str, "EXCLUSIVE"))
						{
							sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[-1].str);
	                                                yyerror(errortext);
						}
					}
                                        else
                                        {
                                                if (strcasecmp(yyvsp[-1].str, "SHARE") && strcasecmp(yyvsp[-1].str, "EXCLUSIVE"))
						{
                                               		sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[-1].str);
	                                                yyerror(errortext);
						}
                                        }

					yyval.str=cat4_str(cat5_str(make1_str("lock"), yyvsp[-6].str, yyvsp[-5].str, make1_str("in"), yyvsp[-3].str), make1_str("row"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 482:
#line 2679 "preproc.y"
{
					if (strcasecmp(yyvsp[0].str, "MODE"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[0].str);
                                                yyerror(errortext);
					}                                
                                        if (strcasecmp(yyvsp[-2].str, "ACCESS"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[-2].str);
                                                yyerror(errortext);
					}
                                        if (strcasecmp(yyvsp[-1].str, "SHARE") && strcasecmp(yyvsp[-1].str, "EXCLUSIVE"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[-1].str);
                                                yyerror(errortext);
					}

					yyval.str=cat3_str(cat5_str(make1_str("lock"), yyvsp[-5].str, yyvsp[-4].str, make1_str("in"), yyvsp[-2].str), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 483:
#line 2699 "preproc.y"
{
					if (strcasecmp(yyvsp[0].str, "MODE"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[0].str);
						yyerror(errortext);
					}
                                        if (strcasecmp(yyvsp[-1].str, "SHARE") && strcasecmp(yyvsp[-1].str, "EXCLUSIVE"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[-1].str);
                                                yyerror(errortext);
					}

					yyval.str=cat2_str(cat5_str(make1_str("lock"), yyvsp[-4].str, yyvsp[-3].str, make1_str("in"), yyvsp[-1].str), yyvsp[0].str);
				;
    break;}
case 484:
#line 2715 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 485:
#line 2716 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 486:
#line 2733 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("update"), yyvsp[-4].str, make1_str("set"), yyvsp[-2].str, yyvsp[-1].str), yyvsp[0].str);
				;
    break;}
case 487:
#line 2746 "preproc.y"
{
					struct cursor *ptr, *this;
	
					for (ptr = cur; ptr != NULL; ptr = ptr->next)
					{
						if (strcmp(yyvsp[-5].str, ptr->name) == 0)
						{
						        /* re-definition is a bug */
							sprintf(errortext, "cursor %s already defined", yyvsp[-5].str);
							yyerror(errortext);
				                }
        				}
                        
        				this = (struct cursor *) mm_alloc(sizeof(struct cursor));

			        	/* initial definition */
				        this->next = cur;
				        this->name = yyvsp[-5].str;
					this->connection = connection;
				        this->command =  cat2_str(cat5_str(make1_str("declare"), mm_strdup(yyvsp[-5].str), yyvsp[-4].str, make1_str("cursor for"), yyvsp[-1].str), yyvsp[0].str);
					this->argsinsert = argsinsert;
					this->argsresult = argsresult;
					argsinsert = argsresult = NULL;
											
			        	cur = this;
					
					yyval.str = cat3_str(make1_str("/*"), mm_strdup(this->command), make1_str("*/"));
				;
    break;}
case 488:
#line 2776 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 489:
#line 2777 "preproc.y"
{ yyval.str = make1_str("insensitive"); ;
    break;}
case 490:
#line 2778 "preproc.y"
{ yyval.str = make1_str("scroll"); ;
    break;}
case 491:
#line 2779 "preproc.y"
{ yyval.str = make1_str("insensitive scroll"); ;
    break;}
case 492:
#line 2780 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 493:
#line 2783 "preproc.y"
{ yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 494:
#line 2784 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 495:
#line 2788 "preproc.y"
{ yyval.str = make1_str("read only"); ;
    break;}
case 496:
#line 2790 "preproc.y"
{
                               yyerror("DECLARE/UPDATE not supported; Cursors must be READ ONLY.");
                       ;
    break;}
case 497:
#line 2795 "preproc.y"
{ yyval.str = make2_str(make1_str("of"), yyvsp[0].str); ;
    break;}
case 498:
#line 2809 "preproc.y"
{
					if (strlen(yyvsp[-1].str) > 0 && ForUpdateNotAllowed != 0)
							yyerror("SELECT FOR UPDATE is not allowed in this context");

					ForUpdateNotAllowed = 0;
					yyval.str = cat4_str(yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 499:
#line 2826 "preproc.y"
{
                               yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); 
                        ;
    break;}
case 500:
#line 2830 "preproc.y"
{
                               yyval.str = yyvsp[0].str; 
                        ;
    break;}
case 501:
#line 2834 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-2].str, make1_str("except"), yyvsp[0].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 502:
#line 2839 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-3].str, make1_str("union"), yyvsp[-1].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 503:
#line 2844 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-3].str, make1_str("intersect"), yyvsp[-1].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 504:
#line 2854 "preproc.y"
{
					yyval.str = cat4_str(cat5_str(make1_str("select"), yyvsp[-6].str, yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
					if (strlen(yyvsp[-1].str) > 0 || strlen(yyvsp[0].str) > 0)
						ForUpdateNotAllowed = 1;
				;
    break;}
case 505:
#line 2861 "preproc.y"
{ yyval.str= cat4_str(make1_str("into"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 506:
#line 2862 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 507:
#line 2863 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 508:
#line 2866 "preproc.y"
{ yyval.str = make1_str("table"); ;
    break;}
case 509:
#line 2867 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 510:
#line 2870 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 511:
#line 2871 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 512:
#line 2874 "preproc.y"
{ yyval.str = make1_str("distinct"); ;
    break;}
case 513:
#line 2875 "preproc.y"
{ yyval.str = cat2_str(make1_str("distinct on"), yyvsp[0].str); ;
    break;}
case 514:
#line 2876 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 515:
#line 2877 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 516:
#line 2880 "preproc.y"
{ yyval.str = cat2_str(make1_str("order by"), yyvsp[0].str); ;
    break;}
case 517:
#line 2881 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 518:
#line 2884 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 519:
#line 2885 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 520:
#line 2889 "preproc.y"
{
					 yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 521:
#line 2894 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 522:
#line 2895 "preproc.y"
{ yyval.str = make1_str("using <"); ;
    break;}
case 523:
#line 2896 "preproc.y"
{ yyval.str = make1_str("using >"); ;
    break;}
case 524:
#line 2897 "preproc.y"
{ yyval.str = make1_str("asc"); ;
    break;}
case 525:
#line 2898 "preproc.y"
{ yyval.str = make1_str("desc"); ;
    break;}
case 526:
#line 2899 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 527:
#line 2903 "preproc.y"
{ yyval.str = cat4_str(make1_str("limit"), yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 528:
#line 2905 "preproc.y"
{ yyval.str = cat4_str(make1_str("limit"), yyvsp[-2].str, make1_str("offset"), yyvsp[0].str); ;
    break;}
case 529:
#line 2907 "preproc.y"
{ yyval.str = cat2_str(make1_str("limit"), yyvsp[0].str);; ;
    break;}
case 530:
#line 2909 "preproc.y"
{ yyval.str = cat4_str(make1_str("offset"), yyvsp[-2].str, make1_str("limit"), yyvsp[0].str); ;
    break;}
case 531:
#line 2911 "preproc.y"
{ yyval.str = cat2_str(make1_str("offset"), yyvsp[0].str); ;
    break;}
case 532:
#line 2913 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 533:
#line 2916 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 534:
#line 2917 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 535:
#line 2918 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 536:
#line 2921 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 537:
#line 2922 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 538:
#line 2932 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 539:
#line 2933 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 540:
#line 2936 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 541:
#line 2939 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 542:
#line 2941 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 543:
#line 2944 "preproc.y"
{ yyval.str = cat2_str(make1_str("groub by"), yyvsp[0].str); ;
    break;}
case 544:
#line 2945 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 545:
#line 2949 "preproc.y"
{
					yyval.str = cat2_str(make1_str("having"), yyvsp[0].str);
				;
    break;}
case 546:
#line 2952 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 547:
#line 2957 "preproc.y"
{
                                yyval.str = make1_str("for update");
                        ;
    break;}
case 548:
#line 2961 "preproc.y"
{
                                yyval.str = cat2_str(make1_str("for update of"), yyvsp[0].str);
                        ;
    break;}
case 549:
#line 2965 "preproc.y"
{
                                yyval.str = make1_str("");
                        ;
    break;}
case 550:
#line 2979 "preproc.y"
{
					yyerror("JOIN not yet implemented");
				;
    break;}
case 551:
#line 2982 "preproc.y"
{ yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 552:
#line 2983 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 553:
#line 2987 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 554:
#line 2989 "preproc.y"
{ yyerror("CROSS JOIN not yet implemented"); ;
    break;}
case 555:
#line 2991 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 556:
#line 2995 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
				;
    break;}
case 557:
#line 2999 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 558:
#line 3003 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 559:
#line 3008 "preproc.y"
{ yyval.str = cat2_str(make1_str("natural"), yyvsp[0].str); ;
    break;}
case 560:
#line 3010 "preproc.y"
{ yyerror("FULL OUTER JOIN not yet implemented"); ;
    break;}
case 561:
#line 3012 "preproc.y"
{ yyerror("LEFT OUTER JOIN not yet implemented"); ;
    break;}
case 562:
#line 3014 "preproc.y"
{ yyerror("RIGHT OUTER JOIN not yet implemented"); ;
    break;}
case 563:
#line 3016 "preproc.y"
{ yyerror("OUTER JOIN not yet implemented"); ;
    break;}
case 564:
#line 3018 "preproc.y"
{ yyerror("INNER JOIN not yet implemented"); ;
    break;}
case 565:
#line 3020 "preproc.y"
{ yyerror("UNION JOIN not yet implemented"); ;
    break;}
case 566:
#line 3022 "preproc.y"
{ yyerror("INNER JOIN not yet implemented"); ;
    break;}
case 567:
#line 3025 "preproc.y"
{ yyval.str = make1_str("outer"); ;
    break;}
case 568:
#line 3026 "preproc.y"
{ yyval.str = make1_str("");  /* no qualifiers */ ;
    break;}
case 569:
#line 3029 "preproc.y"
{ yyval.str = make3_str(make1_str("on ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 570:
#line 3030 "preproc.y"
{ yyval.str = make3_str(make1_str("using ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 571:
#line 3031 "preproc.y"
{ yyval.str = make1_str("");  /* no qualifiers */ ;
    break;}
case 572:
#line 3034 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 573:
#line 3035 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 574:
#line 3039 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 575:
#line 3043 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 576:
#line 3047 "preproc.y"
{
					yyval.str = yyvsp[0].str;;
				;
    break;}
case 577:
#line 3052 "preproc.y"
{ yyval.str = cat2_str(make1_str("where"), yyvsp[0].str); ;
    break;}
case 578:
#line 3053 "preproc.y"
{ yyval.str = make1_str("");  /* no qualifiers */ ;
    break;}
case 579:
#line 3057 "preproc.y"
{
					/* normal relations */
					yyval.str = yyvsp[0].str;
				;
    break;}
case 580:
#line 3062 "preproc.y"
{
					/* inheritance query */
					yyval.str = cat2_str(yyvsp[-1].str, make1_str("*"));
				;
    break;}
case 581:
#line 3068 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 582:
#line 3074 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 583:
#line 3080 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 584:
#line 3088 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 585:
#line 3094 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 586:
#line 3100 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 587:
#line 3118 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].index.str);
				;
    break;}
case 588:
#line 3121 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 589:
#line 3123 "preproc.y"
{
					yyval.str = cat2_str(make1_str("setof"), yyvsp[0].str);
				;
    break;}
case 591:
#line 3129 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 592:
#line 3130 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 593:
#line 3134 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 594:
#line 3139 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 595:
#line 3140 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 596:
#line 3141 "preproc.y"
{ yyval.str = make1_str("at"); ;
    break;}
case 597:
#line 3142 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 598:
#line 3143 "preproc.y"
{ yyval.str = make1_str("break"); ;
    break;}
case 599:
#line 3144 "preproc.y"
{ yyval.str = make1_str("call"); ;
    break;}
case 600:
#line 3145 "preproc.y"
{ yyval.str = make1_str("connect"); ;
    break;}
case 601:
#line 3146 "preproc.y"
{ yyval.str = make1_str("connection"); ;
    break;}
case 602:
#line 3147 "preproc.y"
{ yyval.str = make1_str("continue"); ;
    break;}
case 603:
#line 3148 "preproc.y"
{ yyval.str = make1_str("deallocate"); ;
    break;}
case 604:
#line 3149 "preproc.y"
{ yyval.str = make1_str("disconnect"); ;
    break;}
case 605:
#line 3150 "preproc.y"
{ yyval.str = make1_str("found"); ;
    break;}
case 606:
#line 3151 "preproc.y"
{ yyval.str = make1_str("go"); ;
    break;}
case 607:
#line 3152 "preproc.y"
{ yyval.str = make1_str("goto"); ;
    break;}
case 608:
#line 3153 "preproc.y"
{ yyval.str = make1_str("identified"); ;
    break;}
case 609:
#line 3154 "preproc.y"
{ yyval.str = make1_str("immediate"); ;
    break;}
case 610:
#line 3155 "preproc.y"
{ yyval.str = make1_str("indicator"); ;
    break;}
case 611:
#line 3156 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 612:
#line 3157 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 613:
#line 3158 "preproc.y"
{ yyval.str = make1_str("open"); ;
    break;}
case 614:
#line 3159 "preproc.y"
{ yyval.str = make1_str("prepare"); ;
    break;}
case 615:
#line 3160 "preproc.y"
{ yyval.str = make1_str("release"); ;
    break;}
case 616:
#line 3161 "preproc.y"
{ yyval.str = make1_str("section"); ;
    break;}
case 617:
#line 3162 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 618:
#line 3163 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 619:
#line 3164 "preproc.y"
{ yyval.str = make1_str("sqlerror"); ;
    break;}
case 620:
#line 3165 "preproc.y"
{ yyval.str = make1_str("sqlprint"); ;
    break;}
case 621:
#line 3166 "preproc.y"
{ yyval.str = make1_str("sqlwarning"); ;
    break;}
case 622:
#line 3167 "preproc.y"
{ yyval.str = make1_str("stop"); ;
    break;}
case 623:
#line 3168 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 624:
#line 3169 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 625:
#line 3170 "preproc.y"
{ yyval.str = make1_str("var"); ;
    break;}
case 626:
#line 3171 "preproc.y"
{ yyval.str = make1_str("whenever"); ;
    break;}
case 627:
#line 3180 "preproc.y"
{
					yyval.str = cat2_str(make1_str("float"), yyvsp[0].str);
				;
    break;}
case 628:
#line 3184 "preproc.y"
{
					yyval.str = make1_str("double precision");
				;
    break;}
case 629:
#line 3188 "preproc.y"
{
					yyval.str = cat2_str(make1_str("decimal"), yyvsp[0].str);
				;
    break;}
case 630:
#line 3192 "preproc.y"
{
					yyval.str = cat2_str(make1_str("numeric"), yyvsp[0].str);
				;
    break;}
case 631:
#line 3198 "preproc.y"
{	yyval.str = make1_str("float"); ;
    break;}
case 632:
#line 3200 "preproc.y"
{	yyval.str = make1_str("double precision"); ;
    break;}
case 633:
#line 3202 "preproc.y"
{	yyval.str = make1_str("decimal"); ;
    break;}
case 634:
#line 3204 "preproc.y"
{	yyval.str = make1_str("numeric"); ;
    break;}
case 635:
#line 3208 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1)
						yyerror("precision for FLOAT must be at least 1");
					else if (atol(yyvsp[-1].str) >= 16)
						yyerror("precision for FLOAT must be less than 16");
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 636:
#line 3216 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 637:
#line 3222 "preproc.y"
{
					if (atol(yyvsp[-3].str) < 1 || atol(yyvsp[-3].str) > NUMERIC_MAX_PRECISION) {
						sprintf(errortext, "NUMERIC precision %s must be between 1 and %d", yyvsp[-3].str, NUMERIC_MAX_PRECISION);
						yyerror(errortext);
					}
					if (atol(yyvsp[-1].str) < 0 || atol(yyvsp[-1].str) > atol(yyvsp[-3].str)) {
						sprintf(errortext, "NUMERIC scale %s must be between 0 and precision %s", yyvsp[-1].str, yyvsp[-3].str);
						yyerror(errortext);
					}
					yyval.str = cat3_str(make2_str(make1_str("("), yyvsp[-3].str), make1_str(","), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 638:
#line 3234 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1 || atol(yyvsp[-1].str) > NUMERIC_MAX_PRECISION) {
						sprintf(errortext, "NUMERIC precision %s must be between 1 and %d", yyvsp[-1].str, NUMERIC_MAX_PRECISION);
						yyerror(errortext);
					}
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 639:
#line 3242 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 640:
#line 3248 "preproc.y"
{
					if (atol(yyvsp[-3].str) < 1 || atol(yyvsp[-3].str) > NUMERIC_MAX_PRECISION) {
						sprintf(errortext, "NUMERIC precision %s must be between 1 and %d", yyvsp[-3].str, NUMERIC_MAX_PRECISION);
						yyerror(errortext);
					}
					if (atol(yyvsp[-1].str) < 0 || atol(yyvsp[-1].str) > atol(yyvsp[-3].str)) {
						sprintf(errortext, "NUMERIC scale %s must be between 0 and precision %s", yyvsp[-1].str, yyvsp[-3].str);
						yyerror(errortext);
					}
					yyval.str = cat3_str(make2_str(make1_str("("), yyvsp[-3].str), make1_str(","), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 641:
#line 3260 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1 || atol(yyvsp[-1].str) > NUMERIC_MAX_PRECISION) {
						sprintf(errortext, "NUMERIC precision %s must be between 1 and %d", yyvsp[-1].str, NUMERIC_MAX_PRECISION);
						yyerror(errortext);
					}
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 642:
#line 3268 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 643:
#line 3281 "preproc.y"
{
					if (strncasecmp(yyvsp[-3].str, "char", strlen("char")) && strncasecmp(yyvsp[-3].str, "varchar", strlen("varchar")))
						yyerror("internal parsing error; unrecognized character type");
					if (atol(yyvsp[-1].str) < 1) {
						sprintf(errortext, "length for '%s' type must be at least 1",yyvsp[-3].str);
						yyerror(errortext);
					}
					else if (atol(yyvsp[-1].str) > 4096) {
						/* we can store a char() of length up to the size
						 * of a page (8KB) - page headers and friends but
						 * just to be safe here...	- ay 6/95
						 * XXX note this hardcoded limit - thomas 1997-07-13
						 */
						sprintf(errortext, "length for type '%s' cannot exceed 4096",yyvsp[-3].str);
						yyerror(errortext);
					}

					yyval.str = cat2_str(yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 644:
#line 3301 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 645:
#line 3307 "preproc.y"
{
					if (strlen(yyvsp[0].str) > 0) 
						fprintf(stderr, "COLLATE %s not yet implemented",yyvsp[0].str);

					yyval.str = cat4_str(make1_str("character"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 646:
#line 3313 "preproc.y"
{ yyval.str = cat2_str(make1_str("char"), yyvsp[0].str); ;
    break;}
case 647:
#line 3314 "preproc.y"
{ yyval.str = make1_str("varchar"); ;
    break;}
case 648:
#line 3315 "preproc.y"
{ yyval.str = cat2_str(make1_str("national character"), yyvsp[0].str); ;
    break;}
case 649:
#line 3316 "preproc.y"
{ yyval.str = cat2_str(make1_str("nchar"), yyvsp[0].str); ;
    break;}
case 650:
#line 3319 "preproc.y"
{ yyval.str = make1_str("varying"); ;
    break;}
case 651:
#line 3320 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 652:
#line 3323 "preproc.y"
{ yyval.str = cat2_str(make1_str("character set"), yyvsp[0].str); ;
    break;}
case 653:
#line 3324 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 654:
#line 3327 "preproc.y"
{ yyval.str = cat2_str(make1_str("collate"), yyvsp[0].str); ;
    break;}
case 655:
#line 3328 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 656:
#line 3332 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 657:
#line 3336 "preproc.y"
{
					yyval.str = cat2_str(make1_str("timestamp"), yyvsp[0].str);
				;
    break;}
case 658:
#line 3340 "preproc.y"
{
					yyval.str = make1_str("time");
				;
    break;}
case 659:
#line 3344 "preproc.y"
{
					yyval.str = cat2_str(make1_str("interval"), yyvsp[0].str);
				;
    break;}
case 660:
#line 3349 "preproc.y"
{ yyval.str = make1_str("year"); ;
    break;}
case 661:
#line 3350 "preproc.y"
{ yyval.str = make1_str("month"); ;
    break;}
case 662:
#line 3351 "preproc.y"
{ yyval.str = make1_str("day"); ;
    break;}
case 663:
#line 3352 "preproc.y"
{ yyval.str = make1_str("hour"); ;
    break;}
case 664:
#line 3353 "preproc.y"
{ yyval.str = make1_str("minute"); ;
    break;}
case 665:
#line 3354 "preproc.y"
{ yyval.str = make1_str("second"); ;
    break;}
case 666:
#line 3357 "preproc.y"
{ yyval.str = make1_str("with time zone"); ;
    break;}
case 667:
#line 3358 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 668:
#line 3361 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 669:
#line 3362 "preproc.y"
{ yyval.str = make1_str("year to #month"); ;
    break;}
case 670:
#line 3363 "preproc.y"
{ yyval.str = make1_str("day to hour"); ;
    break;}
case 671:
#line 3364 "preproc.y"
{ yyval.str = make1_str("day to minute"); ;
    break;}
case 672:
#line 3365 "preproc.y"
{ yyval.str = make1_str("day to second"); ;
    break;}
case 673:
#line 3366 "preproc.y"
{ yyval.str = make1_str("hour to minute"); ;
    break;}
case 674:
#line 3367 "preproc.y"
{ yyval.str = make1_str("minute to second"); ;
    break;}
case 675:
#line 3368 "preproc.y"
{ yyval.str = make1_str("hour to second"); ;
    break;}
case 676:
#line 3369 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 677:
#line 3380 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 678:
#line 3382 "preproc.y"
{
					yyval.str = make1_str("null");
				;
    break;}
case 679:
#line 3397 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-5].str, make1_str(") in ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 680:
#line 3401 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-6].str, make1_str(") not in ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 681:
#line 3405 "preproc.y"
{
					yyval.str = make4_str(make5_str(make1_str("("), yyvsp[-6].str, make1_str(")"), yyvsp[-4].str, yyvsp[-3].str), make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 682:
#line 3409 "preproc.y"
{
					yyval.str = make3_str(make5_str(make1_str("("), yyvsp[-5].str, make1_str(")"), yyvsp[-3].str, make1_str("(")), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 683:
#line 3413 "preproc.y"
{
					yyval.str = cat3_str(make3_str(make1_str("("), yyvsp[-5].str, make1_str(")")), yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 684:
#line 3419 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 685:
#line 3424 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 686:
#line 3425 "preproc.y"
{ yyval.str = "<"; ;
    break;}
case 687:
#line 3426 "preproc.y"
{ yyval.str = "="; ;
    break;}
case 688:
#line 3427 "preproc.y"
{ yyval.str = ">"; ;
    break;}
case 689:
#line 3428 "preproc.y"
{ yyval.str = "+"; ;
    break;}
case 690:
#line 3429 "preproc.y"
{ yyval.str = "-"; ;
    break;}
case 691:
#line 3430 "preproc.y"
{ yyval.str = "*"; ;
    break;}
case 692:
#line 3431 "preproc.y"
{ yyval.str = "/"; ;
    break;}
case 693:
#line 3434 "preproc.y"
{ yyval.str = make1_str("ANY"); ;
    break;}
case 694:
#line 3435 "preproc.y"
{ yyval.str = make1_str("ALL"); ;
    break;}
case 695:
#line 3440 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 696:
#line 3444 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 697:
#line 3459 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 698:
#line 3463 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 699:
#line 3465 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 700:
#line 3467 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 701:
#line 3471 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 702:
#line 3473 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 703:
#line 3475 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 704:
#line 3477 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 705:
#line 3479 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 706:
#line 3481 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 707:
#line 3483 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 708:
#line 3485 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 709:
#line 3490 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 710:
#line 3492 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 711:
#line 3494 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 712:
#line 3498 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 713:
#line 3502 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 714:
#line 3504 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 715:
#line 3506 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 716:
#line 3508 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 717:
#line 3510 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 718:
#line 3512 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 719:
#line 3514 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make1_str("(*)")); 
				;
    break;}
case 720:
#line 3518 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 721:
#line 3522 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 722:
#line 3526 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 723:
#line 3530 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 724:
#line 3534 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 725:
#line 3540 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 726:
#line 3544 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 727:
#line 3550 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 728:
#line 3554 "preproc.y"
{
  		     		        yyval.str = make1_str("user");
			     	;
    break;}
case 729:
#line 3559 "preproc.y"
{
					yyval.str = make3_str(make1_str("exists("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 730:
#line 3563 "preproc.y"
{
					yyval.str = make3_str(make1_str("extract("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 731:
#line 3567 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 732:
#line 3571 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 733:
#line 3576 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 734:
#line 3580 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 735:
#line 3584 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 736:
#line 3588 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 737:
#line 3592 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 738:
#line 3594 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 739:
#line 3596 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 740:
#line 3598 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 741:
#line 3605 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); }
				;
    break;}
case 742:
#line 3609 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); }
				;
    break;}
case 743:
#line 3613 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); }
				;
    break;}
case 744:
#line 3617 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); }
				;
    break;}
case 745:
#line 3621 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 746:
#line 3625 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 747:
#line 3629 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 748:
#line 3633 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("not in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 749:
#line 3637 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-4].str, yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 750:
#line 3641 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("+("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 751:
#line 3645 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("-("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 752:
#line 3649 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("/("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 753:
#line 3653 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("*("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 754:
#line 3657 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("<("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 755:
#line 3661 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(">("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 756:
#line 3665 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("=("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 757:
#line 3669 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("any("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 758:
#line 3673 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 759:
#line 3677 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 760:
#line 3681 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 761:
#line 3685 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 762:
#line 3689 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 763:
#line 3693 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 764:
#line 3697 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 765:
#line 3701 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("all ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 766:
#line 3705 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 767:
#line 3709 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 768:
#line 3713 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 769:
#line 3717 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 770:
#line 3721 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 771:
#line 3725 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 772:
#line 3729 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 773:
#line 3733 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 774:
#line 3735 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 775:
#line 3737 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 776:
#line 3739 "preproc.y"
{       yyval.str = yyvsp[0].str; ;
    break;}
case 777:
#line 3741 "preproc.y"
{ yyval.str = make1_str(";;"); ;
    break;}
case 778:
#line 3750 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 779:
#line 3754 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 780:
#line 3756 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 781:
#line 3760 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 782:
#line 3762 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 783:
#line 3764 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 784:
#line 3766 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 785:
#line 3768 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 786:
#line 3773 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 787:
#line 3775 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 788:
#line 3777 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 789:
#line 3781 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 790:
#line 3785 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 791:
#line 3787 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 792:
#line 3789 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 793:
#line 3791 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 794:
#line 3793 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 795:
#line 3797 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 796:
#line 3801 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 797:
#line 3805 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 798:
#line 3809 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 799:
#line 3815 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 800:
#line 3819 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 801:
#line 3825 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 802:
#line 3829 "preproc.y"
{
					yyval.str = make1_str("user");
				;
    break;}
case 803:
#line 3833 "preproc.y"
{
					yyval.str = make3_str(make1_str("position ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 804:
#line 3837 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 805:
#line 3842 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 806:
#line 3846 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 807:
#line 3850 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 808:
#line 3854 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 809:
#line 3858 "preproc.y"
{ 	yyval.str = yyvsp[0].str; ;
    break;}
case 810:
#line 3862 "preproc.y"
{
					yyval.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].str);
				;
    break;}
case 811:
#line 3866 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("["), yyvsp[-4].str, make1_str(":"), yyvsp[-2].str, make1_str("]")), yyvsp[0].str);
				;
    break;}
case 812:
#line 3870 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 813:
#line 3874 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 814:
#line 3876 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 815:
#line 3878 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str("using"), yyvsp[0].str); ;
    break;}
case 816:
#line 3882 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("from"), yyvsp[0].str);
				;
    break;}
case 817:
#line 3886 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 818:
#line 3888 "preproc.y"
{ yyval.str = make1_str(";;"); ;
    break;}
case 819:
#line 3891 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 820:
#line 3892 "preproc.y"
{ yyval.str = make1_str("timezone_hour"); ;
    break;}
case 821:
#line 3893 "preproc.y"
{ yyval.str = make1_str("timezone_minute"); ;
    break;}
case 822:
#line 3897 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("in"), yyvsp[0].str); ;
    break;}
case 823:
#line 3899 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 824:
#line 3903 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 825:
#line 3907 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 826:
#line 3909 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 827:
#line 3911 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 828:
#line 3913 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 829:
#line 3915 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 830:
#line 3917 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 831:
#line 3919 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 832:
#line 3921 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 833:
#line 3925 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 834:
#line 3929 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 835:
#line 3931 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 836:
#line 3933 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 837:
#line 3935 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 838:
#line 3937 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 839:
#line 3941 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()"));
				;
    break;}
case 840:
#line 3945 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 841:
#line 3949 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 842:
#line 3953 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 843:
#line 3958 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 844:
#line 3962 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 845:
#line 3966 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 846:
#line 3970 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 847:
#line 3976 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 848:
#line 3980 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 849:
#line 3984 "preproc.y"
{	yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 850:
#line 3986 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 851:
#line 3992 "preproc.y"
{	yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 852:
#line 3994 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 853:
#line 3998 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str("from"), yyvsp[0].str); ;
    break;}
case 854:
#line 4000 "preproc.y"
{ yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 855:
#line 4002 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 856:
#line 4006 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 857:
#line 4010 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 858:
#line 4014 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 859:
#line 4016 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);;
    break;}
case 860:
#line 4020 "preproc.y"
{
					yyval.str = yyvsp[0].str; 
				;
    break;}
case 861:
#line 4024 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 862:
#line 4028 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 863:
#line 4030 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);;
    break;}
case 864:
#line 4049 "preproc.y"
{ yyval.str = cat5_str(make1_str("case"), yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, make1_str("end")); ;
    break;}
case 865:
#line 4051 "preproc.y"
{
					yyval.str = cat5_str(make1_str("nullif("), yyvsp[-3].str, make1_str(","), yyvsp[-1].str, make1_str(")"));

					fprintf(stderr, "NULLIF() not yet fully implemented");
                                ;
    break;}
case 866:
#line 4057 "preproc.y"
{
					yyval.str = cat3_str(make1_str("coalesce("), yyvsp[-1].str, make1_str(")"));

					fprintf(stderr, "COALESCE() not yet fully implemented");
				;
    break;}
case 867:
#line 4065 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 868:
#line 4067 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 869:
#line 4071 "preproc.y"
{
					yyval.str = cat4_str(make1_str("when"), yyvsp[-2].str, make1_str("then"), yyvsp[0].str);
                               ;
    break;}
case 870:
#line 4076 "preproc.y"
{ yyval.str = cat2_str(make1_str("else"), yyvsp[0].str); ;
    break;}
case 871:
#line 4077 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 872:
#line 4081 "preproc.y"
{
                                       yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                               ;
    break;}
case 873:
#line 4085 "preproc.y"
{
                                       yyval.str = yyvsp[0].str;
                               ;
    break;}
case 874:
#line 4089 "preproc.y"
{       yyval.str = make1_str(""); ;
    break;}
case 875:
#line 4093 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 876:
#line 4097 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 877:
#line 4103 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 878:
#line 4105 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str); ;
    break;}
case 879:
#line 4107 "preproc.y"
{ yyval.str = make2_str(yyvsp[-2].str, make1_str(".*")); ;
    break;}
case 880:
#line 4118 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","),yyvsp[0].str);  ;
    break;}
case 881:
#line 4120 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 882:
#line 4121 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 883:
#line 4125 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-3].str, yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 884:
#line 4129 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 885:
#line 4133 "preproc.y"
{
					yyval.str = make2_str(yyvsp[-2].str, make1_str(".*"));
				;
    break;}
case 886:
#line 4144 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);  ;
    break;}
case 887:
#line 4146 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 888:
#line 4151 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
				;
    break;}
case 889:
#line 4155 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 890:
#line 4159 "preproc.y"
{
					yyval.str = make2_str(yyvsp[-2].str, make1_str(".*"));
				;
    break;}
case 891:
#line 4163 "preproc.y"
{
					yyval.str = make1_str("*");
				;
    break;}
case 892:
#line 4168 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 893:
#line 4169 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 894:
#line 4173 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 895:
#line 4177 "preproc.y"
{
					/* disallow refs to variable system tables */
					if (strcmp(LogRelationName, yyvsp[0].str) == 0
					   || strcmp(VariableRelationName, yyvsp[0].str) == 0) {
						sprintf(errortext, make1_str("%s cannot be accessed by users"),yyvsp[0].str);
						yyerror(errortext);
					}
					else
						yyval.str = yyvsp[0].str;
				;
    break;}
case 896:
#line 4189 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 897:
#line 4190 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 898:
#line 4191 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 899:
#line 4192 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 900:
#line 4193 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 901:
#line 4199 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 902:
#line 4200 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 903:
#line 4202 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 904:
#line 4203 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 905:
#line 4209 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 906:
#line 4213 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 907:
#line 4217 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 908:
#line 4221 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 909:
#line 4225 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 910:
#line 4227 "preproc.y"
{
					yyval.str = make1_str("true");
				;
    break;}
case 911:
#line 4231 "preproc.y"
{
					yyval.str = make1_str("false");
				;
    break;}
case 912:
#line 4237 "preproc.y"
{
					yyval.str = cat2_str(make_name(), yyvsp[0].str);
				;
    break;}
case 913:
#line 4242 "preproc.y"
{ yyval.str = make_name();;
    break;}
case 914:
#line 4243 "preproc.y"
{ yyval.str = make_name();;
    break;}
case 915:
#line 4244 "preproc.y"
{
							yyval.str = (char *)mm_alloc(strlen(yyvsp[0].str) + 3);
							yyval.str[0]='\'';
				     		        strcpy(yyval.str+1, yyvsp[0].str);
							yyval.str[strlen(yyvsp[0].str)+2]='\0';
							yyval.str[strlen(yyvsp[0].str)+1]='\'';
							free(yyvsp[0].str);
						;
    break;}
case 916:
#line 4252 "preproc.y"
{ yyval.str = yyvsp[0].str;;
    break;}
case 917:
#line 4260 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 918:
#line 4262 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 919:
#line 4264 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 920:
#line 4274 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 921:
#line 4275 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 922:
#line 4276 "preproc.y"
{ yyval.str = make1_str("absolute"); ;
    break;}
case 923:
#line 4277 "preproc.y"
{ yyval.str = make1_str("action"); ;
    break;}
case 924:
#line 4278 "preproc.y"
{ yyval.str = make1_str("after"); ;
    break;}
case 925:
#line 4279 "preproc.y"
{ yyval.str = make1_str("aggregate"); ;
    break;}
case 926:
#line 4280 "preproc.y"
{ yyval.str = make1_str("backward"); ;
    break;}
case 927:
#line 4281 "preproc.y"
{ yyval.str = make1_str("before"); ;
    break;}
case 928:
#line 4282 "preproc.y"
{ yyval.str = make1_str("cache"); ;
    break;}
case 929:
#line 4283 "preproc.y"
{ yyval.str = make1_str("createdb"); ;
    break;}
case 930:
#line 4284 "preproc.y"
{ yyval.str = make1_str("createuser"); ;
    break;}
case 931:
#line 4285 "preproc.y"
{ yyval.str = make1_str("cycle"); ;
    break;}
case 932:
#line 4286 "preproc.y"
{ yyval.str = make1_str("database"); ;
    break;}
case 933:
#line 4287 "preproc.y"
{ yyval.str = make1_str("delimiters"); ;
    break;}
case 934:
#line 4288 "preproc.y"
{ yyval.str = make1_str("double"); ;
    break;}
case 935:
#line 4289 "preproc.y"
{ yyval.str = make1_str("each"); ;
    break;}
case 936:
#line 4290 "preproc.y"
{ yyval.str = make1_str("encoding"); ;
    break;}
case 937:
#line 4291 "preproc.y"
{ yyval.str = make1_str("forward"); ;
    break;}
case 938:
#line 4292 "preproc.y"
{ yyval.str = make1_str("function"); ;
    break;}
case 939:
#line 4293 "preproc.y"
{ yyval.str = make1_str("handler"); ;
    break;}
case 940:
#line 4294 "preproc.y"
{ yyval.str = make1_str("increment"); ;
    break;}
case 941:
#line 4295 "preproc.y"
{ yyval.str = make1_str("index"); ;
    break;}
case 942:
#line 4296 "preproc.y"
{ yyval.str = make1_str("inherits"); ;
    break;}
case 943:
#line 4297 "preproc.y"
{ yyval.str = make1_str("insensitive"); ;
    break;}
case 944:
#line 4298 "preproc.y"
{ yyval.str = make1_str("instead"); ;
    break;}
case 945:
#line 4299 "preproc.y"
{ yyval.str = make1_str("isnull"); ;
    break;}
case 946:
#line 4300 "preproc.y"
{ yyval.str = make1_str("key"); ;
    break;}
case 947:
#line 4301 "preproc.y"
{ yyval.str = make1_str("language"); ;
    break;}
case 948:
#line 4302 "preproc.y"
{ yyval.str = make1_str("lancompiler"); ;
    break;}
case 949:
#line 4303 "preproc.y"
{ yyval.str = make1_str("location"); ;
    break;}
case 950:
#line 4304 "preproc.y"
{ yyval.str = make1_str("match"); ;
    break;}
case 951:
#line 4305 "preproc.y"
{ yyval.str = make1_str("maxvalue"); ;
    break;}
case 952:
#line 4306 "preproc.y"
{ yyval.str = make1_str("minvalue"); ;
    break;}
case 953:
#line 4307 "preproc.y"
{ yyval.str = make1_str("next"); ;
    break;}
case 954:
#line 4308 "preproc.y"
{ yyval.str = make1_str("nocreatedb"); ;
    break;}
case 955:
#line 4309 "preproc.y"
{ yyval.str = make1_str("nocreateuser"); ;
    break;}
case 956:
#line 4310 "preproc.y"
{ yyval.str = make1_str("nothing"); ;
    break;}
case 957:
#line 4311 "preproc.y"
{ yyval.str = make1_str("notnull"); ;
    break;}
case 958:
#line 4312 "preproc.y"
{ yyval.str = make1_str("of"); ;
    break;}
case 959:
#line 4313 "preproc.y"
{ yyval.str = make1_str("oids"); ;
    break;}
case 960:
#line 4314 "preproc.y"
{ yyval.str = make1_str("only"); ;
    break;}
case 961:
#line 4315 "preproc.y"
{ yyval.str = make1_str("operator"); ;
    break;}
case 962:
#line 4316 "preproc.y"
{ yyval.str = make1_str("option"); ;
    break;}
case 963:
#line 4317 "preproc.y"
{ yyval.str = make1_str("password"); ;
    break;}
case 964:
#line 4318 "preproc.y"
{ yyval.str = make1_str("prior"); ;
    break;}
case 965:
#line 4319 "preproc.y"
{ yyval.str = make1_str("privileges"); ;
    break;}
case 966:
#line 4320 "preproc.y"
{ yyval.str = make1_str("procedural"); ;
    break;}
case 967:
#line 4321 "preproc.y"
{ yyval.str = make1_str("read"); ;
    break;}
case 968:
#line 4322 "preproc.y"
{ yyval.str = make1_str("recipe"); ;
    break;}
case 969:
#line 4323 "preproc.y"
{ yyval.str = make1_str("relative"); ;
    break;}
case 970:
#line 4324 "preproc.y"
{ yyval.str = make1_str("rename"); ;
    break;}
case 971:
#line 4325 "preproc.y"
{ yyval.str = make1_str("returns"); ;
    break;}
case 972:
#line 4326 "preproc.y"
{ yyval.str = make1_str("row"); ;
    break;}
case 973:
#line 4327 "preproc.y"
{ yyval.str = make1_str("rule"); ;
    break;}
case 974:
#line 4328 "preproc.y"
{ yyval.str = make1_str("scroll"); ;
    break;}
case 975:
#line 4329 "preproc.y"
{ yyval.str = make1_str("sequence"); ;
    break;}
case 976:
#line 4330 "preproc.y"
{ yyval.str = make1_str("serial"); ;
    break;}
case 977:
#line 4331 "preproc.y"
{ yyval.str = make1_str("start"); ;
    break;}
case 978:
#line 4332 "preproc.y"
{ yyval.str = make1_str("statement"); ;
    break;}
case 979:
#line 4333 "preproc.y"
{ yyval.str = make1_str("stdin"); ;
    break;}
case 980:
#line 4334 "preproc.y"
{ yyval.str = make1_str("stdout"); ;
    break;}
case 981:
#line 4335 "preproc.y"
{ yyval.str = make1_str("time"); ;
    break;}
case 982:
#line 4336 "preproc.y"
{ yyval.str = make1_str("timestamp"); ;
    break;}
case 983:
#line 4337 "preproc.y"
{ yyval.str = make1_str("timezone_hour"); ;
    break;}
case 984:
#line 4338 "preproc.y"
{ yyval.str = make1_str("timezone_minute"); ;
    break;}
case 985:
#line 4339 "preproc.y"
{ yyval.str = make1_str("trigger"); ;
    break;}
case 986:
#line 4340 "preproc.y"
{ yyval.str = make1_str("trusted"); ;
    break;}
case 987:
#line 4341 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 988:
#line 4342 "preproc.y"
{ yyval.str = make1_str("valid"); ;
    break;}
case 989:
#line 4343 "preproc.y"
{ yyval.str = make1_str("version"); ;
    break;}
case 990:
#line 4344 "preproc.y"
{ yyval.str = make1_str("zone"); ;
    break;}
case 991:
#line 4345 "preproc.y"
{ yyval.str = make1_str("at"); ;
    break;}
case 992:
#line 4346 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 993:
#line 4347 "preproc.y"
{ yyval.str = make1_str("break"); ;
    break;}
case 994:
#line 4348 "preproc.y"
{ yyval.str = make1_str("call"); ;
    break;}
case 995:
#line 4349 "preproc.y"
{ yyval.str = make1_str("connect"); ;
    break;}
case 996:
#line 4350 "preproc.y"
{ yyval.str = make1_str("connection"); ;
    break;}
case 997:
#line 4351 "preproc.y"
{ yyval.str = make1_str("continue"); ;
    break;}
case 998:
#line 4352 "preproc.y"
{ yyval.str = make1_str("deallocate"); ;
    break;}
case 999:
#line 4353 "preproc.y"
{ yyval.str = make1_str("disconnect"); ;
    break;}
case 1000:
#line 4354 "preproc.y"
{ yyval.str = make1_str("found"); ;
    break;}
case 1001:
#line 4355 "preproc.y"
{ yyval.str = make1_str("go"); ;
    break;}
case 1002:
#line 4356 "preproc.y"
{ yyval.str = make1_str("goto"); ;
    break;}
case 1003:
#line 4357 "preproc.y"
{ yyval.str = make1_str("identified"); ;
    break;}
case 1004:
#line 4358 "preproc.y"
{ yyval.str = make1_str("immediate"); ;
    break;}
case 1005:
#line 4359 "preproc.y"
{ yyval.str = make1_str("indicator"); ;
    break;}
case 1006:
#line 4360 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 1007:
#line 4361 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 1008:
#line 4362 "preproc.y"
{ yyval.str = make1_str("open"); ;
    break;}
case 1009:
#line 4363 "preproc.y"
{ yyval.str = make1_str("prepare"); ;
    break;}
case 1010:
#line 4364 "preproc.y"
{ yyval.str = make1_str("release"); ;
    break;}
case 1011:
#line 4365 "preproc.y"
{ yyval.str = make1_str("section"); ;
    break;}
case 1012:
#line 4366 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 1013:
#line 4367 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1014:
#line 4368 "preproc.y"
{ yyval.str = make1_str("sqlerror"); ;
    break;}
case 1015:
#line 4369 "preproc.y"
{ yyval.str = make1_str("sqlprint"); ;
    break;}
case 1016:
#line 4370 "preproc.y"
{ yyval.str = make1_str("sqlwarning"); ;
    break;}
case 1017:
#line 4371 "preproc.y"
{ yyval.str = make1_str("stop"); ;
    break;}
case 1018:
#line 4372 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 1019:
#line 4373 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 1020:
#line 4374 "preproc.y"
{ yyval.str = make1_str("var"); ;
    break;}
case 1021:
#line 4375 "preproc.y"
{ yyval.str = make1_str("whenever"); ;
    break;}
case 1022:
#line 4387 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1023:
#line 4388 "preproc.y"
{ yyval.str = make1_str("abort"); ;
    break;}
case 1024:
#line 4389 "preproc.y"
{ yyval.str = make1_str("analyze"); ;
    break;}
case 1025:
#line 4390 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 1026:
#line 4391 "preproc.y"
{ yyval.str = make1_str("case"); ;
    break;}
case 1027:
#line 4392 "preproc.y"
{ yyval.str = make1_str("cluster"); ;
    break;}
case 1028:
#line 4393 "preproc.y"
{ yyval.str = make1_str("coalesce"); ;
    break;}
case 1029:
#line 4394 "preproc.y"
{ yyval.str = make1_str("constraint"); ;
    break;}
case 1030:
#line 4395 "preproc.y"
{ yyval.str = make1_str("copy"); ;
    break;}
case 1031:
#line 4396 "preproc.y"
{ yyval.str = make1_str("cross"); ;
    break;}
case 1032:
#line 4397 "preproc.y"
{ yyval.str = make1_str("current"); ;
    break;}
case 1033:
#line 4398 "preproc.y"
{ yyval.str = make1_str("do"); ;
    break;}
case 1034:
#line 4399 "preproc.y"
{ yyval.str = make1_str("else"); ;
    break;}
case 1035:
#line 4400 "preproc.y"
{ yyval.str = make1_str("end"); ;
    break;}
case 1036:
#line 4401 "preproc.y"
{ yyval.str = make1_str("explain"); ;
    break;}
case 1037:
#line 4402 "preproc.y"
{ yyval.str = make1_str("extend"); ;
    break;}
case 1038:
#line 4403 "preproc.y"
{ yyval.str = make1_str("false"); ;
    break;}
case 1039:
#line 4404 "preproc.y"
{ yyval.str = make1_str("foreign"); ;
    break;}
case 1040:
#line 4405 "preproc.y"
{ yyval.str = make1_str("group"); ;
    break;}
case 1041:
#line 4406 "preproc.y"
{ yyval.str = make1_str("listen"); ;
    break;}
case 1042:
#line 4407 "preproc.y"
{ yyval.str = make1_str("load"); ;
    break;}
case 1043:
#line 4408 "preproc.y"
{ yyval.str = make1_str("lock"); ;
    break;}
case 1044:
#line 4409 "preproc.y"
{ yyval.str = make1_str("move"); ;
    break;}
case 1045:
#line 4410 "preproc.y"
{ yyval.str = make1_str("new"); ;
    break;}
case 1046:
#line 4411 "preproc.y"
{ yyval.str = make1_str("none"); ;
    break;}
case 1047:
#line 4412 "preproc.y"
{ yyval.str = make1_str("nullif"); ;
    break;}
case 1048:
#line 4413 "preproc.y"
{ yyval.str = make1_str("order"); ;
    break;}
case 1049:
#line 4414 "preproc.y"
{ yyval.str = make1_str("position"); ;
    break;}
case 1050:
#line 4415 "preproc.y"
{ yyval.str = make1_str("precision"); ;
    break;}
case 1051:
#line 4416 "preproc.y"
{ yyval.str = make1_str("reset"); ;
    break;}
case 1052:
#line 4417 "preproc.y"
{ yyval.str = make1_str("setof"); ;
    break;}
case 1053:
#line 4418 "preproc.y"
{ yyval.str = make1_str("show"); ;
    break;}
case 1054:
#line 4419 "preproc.y"
{ yyval.str = make1_str("table"); ;
    break;}
case 1055:
#line 4420 "preproc.y"
{ yyval.str = make1_str("then"); ;
    break;}
case 1056:
#line 4421 "preproc.y"
{ yyval.str = make1_str("transaction"); ;
    break;}
case 1057:
#line 4422 "preproc.y"
{ yyval.str = make1_str("true"); ;
    break;}
case 1058:
#line 4423 "preproc.y"
{ yyval.str = make1_str("vacuum"); ;
    break;}
case 1059:
#line 4424 "preproc.y"
{ yyval.str = make1_str("verbose"); ;
    break;}
case 1060:
#line 4425 "preproc.y"
{ yyval.str = make1_str("when"); ;
    break;}
case 1061:
#line 4429 "preproc.y"
{
					if (QueryIsRule)
						yyval.str = make1_str("current");
					else
						yyerror("CURRENT used in non-rule query");
				;
    break;}
case 1062:
#line 4436 "preproc.y"
{
					if (QueryIsRule)
						yyval.str = make1_str("new");
					else
						yyerror("NEW used in non-rule query");
				;
    break;}
case 1063:
#line 4452 "preproc.y"
{
			yyval.str = make5_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str, make1_str(","), yyvsp[-1].str);
                ;
    break;}
case 1064:
#line 4456 "preproc.y"
{
                	yyval.str = make1_str("NULL,NULL,NULL,\"DEFAULT\"");
                ;
    break;}
case 1065:
#line 4461 "preproc.y"
{
		       yyval.str = make3_str(make1_str("NULL,"), yyvsp[0].str, make1_str(",NULL"));
		;
    break;}
case 1066:
#line 4466 "preproc.y"
{
		  /* old style: dbname[@server][:port] */
		  if (strlen(yyvsp[-1].str) > 0 && *(yyvsp[-1].str) != '@')
		  {
		    sprintf(errortext, "parse error at or near '%s'", yyvsp[-1].str);
		    yyerror(errortext);
		  }

		  yyval.str = make5_str(make1_str("\""), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str, make1_str("\""));
		;
    break;}
case 1067:
#line 4477 "preproc.y"
{
		  /* new style: <tcp|unix>:postgresql://server[:port][/dbname] */
                  if (strncmp(yyvsp[-4].str, "://", 3) != 0)
		  {
		    sprintf(errortext, "parse error at or near '%s'", yyvsp[-4].str);
		    yyerror(errortext);
		  }

		  if (strncmp(yyvsp[-5].str, "unix", 4) == 0 && strncmp(yyvsp[-4].str + 3, "localhost", 9) != 0)
		  {
		    sprintf(errortext, "unix domain sockets only work on 'localhost' but not on '%9.9s'", yyvsp[-4].str);
                    yyerror(errortext);
		  }

		  if (strncmp(yyvsp[-5].str, "unix", 4) != 0 && strncmp(yyvsp[-5].str, "tcp", 3) != 0)
		  {
		    sprintf(errortext, "only protocols 'tcp' and 'unix' are supported");
                    yyerror(errortext);
		  }
	
		  yyval.str = make4_str(make5_str(make1_str("\""), yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str, make1_str("/")), yyvsp[-1].str, yyvsp[0].str, make1_str("\""));
		;
    break;}
case 1068:
#line 4500 "preproc.y"
{
		  yyval.str = yyvsp[0].str;
		;
    break;}
case 1069:
#line 4504 "preproc.y"
{
		  yyval.str = mm_strdup(yyvsp[0].str);
		  yyval.str[0] = '\"';
		  yyval.str[strlen(yyval.str) - 1] = '\"';
		  free(yyvsp[0].str);
		;
    break;}
case 1070:
#line 4512 "preproc.y"
{
		  if (strcmp(yyvsp[0].str, "postgresql") != 0 && strcmp(yyvsp[0].str, "postgres") != 0)
		  {
		    sprintf(errortext, "parse error at or near '%s'", yyvsp[0].str);
		    yyerror(errortext);	
		  }

		  if (strcmp(yyvsp[-1].str, "tcp") != 0 && strcmp(yyvsp[-1].str, "unix") != 0)
		  {
		    sprintf(errortext, "Illegal connection type %s", yyvsp[-1].str);
		    yyerror(errortext);
		  }

		  yyval.str = make3_str(yyvsp[-1].str, make1_str(":"), yyvsp[0].str);
		;
    break;}
case 1071:
#line 4529 "preproc.y"
{
		  if (strcmp(yyvsp[-1].str, "@") != 0 && strcmp(yyvsp[-1].str, "://") != 0)
		  {
		    sprintf(errortext, "parse error at or near '%s'", yyvsp[-1].str);
		    yyerror(errortext);
		  }

		  yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str);
	        ;
    break;}
case 1072:
#line 4539 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1073:
#line 4540 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1074:
#line 4542 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1075:
#line 4543 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str); ;
    break;}
case 1076:
#line 4545 "preproc.y"
{ yyval.str = make2_str(make1_str(":"), yyvsp[0].str); ;
    break;}
case 1077:
#line 4546 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1078:
#line 4548 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1079:
#line 4549 "preproc.y"
{ yyval.str = make1_str("NULL"); ;
    break;}
case 1080:
#line 4551 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1081:
#line 4552 "preproc.y"
{ yyval.str = make1_str("NULL,NULL"); ;
    break;}
case 1082:
#line 4555 "preproc.y"
{
                        yyval.str = make2_str(yyvsp[0].str, make1_str(",NULL"));
	        ;
    break;}
case 1083:
#line 4559 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1084:
#line 4563 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-3].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1085:
#line 4567 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1086:
#line 4571 "preproc.y"
{ if (yyvsp[0].str[0] == '\"')
				yyval.str = yyvsp[0].str;
			  else
				yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\""));
			;
    break;}
case 1087:
#line 4576 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1088:
#line 4577 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1089:
#line 4580 "preproc.y"
{ /* check if we have a char variable */
			struct variable *p = find_variable(yyvsp[0].str);
			enum ECPGttype typ = p->type->typ;

			/* if array see what's inside */
			if (typ == ECPGt_array)
				typ = p->type->u.element->typ;

                        switch (typ)
                        {
                            case ECPGt_char:
                            case ECPGt_unsigned_char:
                                yyval.str = yyvsp[0].str;
                                break;
                            case ECPGt_varchar:
                                yyval.str = make2_str(yyvsp[0].str, make1_str(".arr"));
                                break;
                            default:
                                yyerror("invalid datatype");
                                break;
                        }
		;
    break;}
case 1090:
#line 4604 "preproc.y"
{
			if (strlen(yyvsp[-1].str) == 0)
				yyerror("parse error");
				
			if (strcmp(yyvsp[-1].str, "?") != 0)
			{
				sprintf(errortext, "parse error at or near %s", yyvsp[-1].str);
				yyerror(errortext);
			}
			
			yyval.str = make2_str(make1_str("?"), yyvsp[0].str);
		;
    break;}
case 1091:
#line 4616 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1092:
#line 4623 "preproc.y"
{
					struct cursor *ptr, *this;
					struct variable *thisquery = (struct variable *)mm_alloc(sizeof(struct variable));

					for (ptr = cur; ptr != NULL; ptr = ptr->next)
					{
						if (strcmp(yyvsp[-5].str, ptr->name) == 0)
						{
						        /* re-definition is a bug */
							sprintf(errortext, "cursor %s already defined", yyvsp[-5].str);
							yyerror(errortext);
				                }
        				}

        				this = (struct cursor *) mm_alloc(sizeof(struct cursor));

			        	/* initial definition */
				        this->next = cur;
				        this->name = yyvsp[-5].str;
					this->connection = connection;
				        this->command =  cat5_str(make1_str("declare"), mm_strdup(yyvsp[-5].str), yyvsp[-4].str, make1_str("cursor for ;;"), yyvsp[0].str);
					this->argsresult = NULL;

					thisquery->type = &ecpg_query;
					thisquery->brace_level = 0;
					thisquery->next = NULL;
					thisquery->name = (char *) mm_alloc(sizeof("ECPGprepared_statement(\"\")") + strlen(yyvsp[-1].str));
					sprintf(thisquery->name, "ECPGprepared_statement(\"%s\")", yyvsp[-1].str);

					this->argsinsert = NULL;
					add_variable(&(this->argsinsert), thisquery, &no_indicator); 

			        	cur = this;
					
					yyval.str = cat3_str(make1_str("/*"), mm_strdup(this->command), make1_str("*/"));
				;
    break;}
case 1093:
#line 4665 "preproc.y"
{ yyval.str = make3_str(make1_str("ECPGdeallocate(__LINE__, \""), yyvsp[0].str, make1_str("\");")); ;
    break;}
case 1094:
#line 4671 "preproc.y"
{
		fputs("/* exec sql begin declare section */", yyout);
	        output_line_number();
	;
    break;}
case 1095:
#line 4676 "preproc.y"
{
		fprintf(yyout, "%s/* exec sql end declare section */", yyvsp[-1].str);
		free(yyvsp[-1].str);
		output_line_number();
	;
    break;}
case 1096:
#line 4682 "preproc.y"
{;
    break;}
case 1097:
#line 4684 "preproc.y"
{;
    break;}
case 1098:
#line 4687 "preproc.y"
{
		yyval.str = make1_str("");
	;
    break;}
case 1099:
#line 4691 "preproc.y"
{
		yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
	;
    break;}
case 1100:
#line 4696 "preproc.y"
{
		actual_storage[struct_level] = mm_strdup(yyvsp[0].str);
	;
    break;}
case 1101:
#line 4700 "preproc.y"
{
		actual_type[struct_level].type_enum = yyvsp[0].type.type_enum;
		actual_type[struct_level].type_dimension = yyvsp[0].type.type_dimension;
		actual_type[struct_level].type_index = yyvsp[0].type.type_index;
	;
    break;}
case 1102:
#line 4706 "preproc.y"
{
 		yyval.str = cat4_str(yyvsp[-5].str, yyvsp[-3].type.type_str, yyvsp[-1].str, make1_str(";\n"));
	;
    break;}
case 1103:
#line 4710 "preproc.y"
{ yyval.str = make1_str("extern"); ;
    break;}
case 1104:
#line 4711 "preproc.y"
{ yyval.str = make1_str("static"); ;
    break;}
case 1105:
#line 4712 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1106:
#line 4713 "preproc.y"
{ yyval.str = make1_str("const"); ;
    break;}
case 1107:
#line 4714 "preproc.y"
{ yyval.str = make1_str("register"); ;
    break;}
case 1108:
#line 4715 "preproc.y"
{ yyval.str = make1_str("auto"); ;
    break;}
case 1109:
#line 4716 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1110:
#line 4719 "preproc.y"
{
			yyval.type.type_enum = yyvsp[0].type_enum;
			yyval.type.type_str = mm_strdup(ECPGtype_name(yyvsp[0].type_enum));
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1111:
#line 4726 "preproc.y"
{
			yyval.type.type_enum = ECPGt_varchar;
			yyval.type.type_str = make1_str("");
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1112:
#line 4733 "preproc.y"
{
			yyval.type.type_enum = ECPGt_struct;
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1113:
#line 4740 "preproc.y"
{
			yyval.type.type_enum = ECPGt_union;
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1114:
#line 4747 "preproc.y"
{
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_enum = ECPGt_int;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1115:
#line 4754 "preproc.y"
{
			/* this is for typedef'ed types */
			struct typedefs *this = get_typedef(yyvsp[0].str);

			yyval.type.type_str = mm_strdup(this->name);
                        yyval.type.type_enum = this->type->type_enum;
			yyval.type.type_dimension = this->type->type_dimension;
  			yyval.type.type_index = this->type->type_index;
			struct_member_list[struct_level] = ECPGstruct_member_dup(this->struct_member_list);
		;
    break;}
case 1116:
#line 4766 "preproc.y"
{
		yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1117:
#line 4770 "preproc.y"
{ yyval.str = cat2_str(make1_str("enum"), yyvsp[0].str); ;
    break;}
case 1118:
#line 4773 "preproc.y"
{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1119:
#line 4780 "preproc.y"
{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1120:
#line 4787 "preproc.y"
{
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    yyval.str = cat2_str(make1_str("struct"), yyvsp[0].str);
	;
    break;}
case 1121:
#line 4795 "preproc.y"
{
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    yyval.str = cat2_str(make1_str("union"), yyvsp[0].str);
	;
    break;}
case 1122:
#line 4802 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1123:
#line 4803 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1124:
#line 4805 "preproc.y"
{ yyval.type_enum = ECPGt_short; ;
    break;}
case 1125:
#line 4806 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_short; ;
    break;}
case 1126:
#line 4807 "preproc.y"
{ yyval.type_enum = ECPGt_int; ;
    break;}
case 1127:
#line 4808 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_int; ;
    break;}
case 1128:
#line 4809 "preproc.y"
{ yyval.type_enum = ECPGt_long; ;
    break;}
case 1129:
#line 4810 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_long; ;
    break;}
case 1130:
#line 4811 "preproc.y"
{ yyval.type_enum = ECPGt_float; ;
    break;}
case 1131:
#line 4812 "preproc.y"
{ yyval.type_enum = ECPGt_double; ;
    break;}
case 1132:
#line 4813 "preproc.y"
{ yyval.type_enum = ECPGt_bool; ;
    break;}
case 1133:
#line 4814 "preproc.y"
{ yyval.type_enum = ECPGt_char; ;
    break;}
case 1134:
#line 4815 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_char; ;
    break;}
case 1135:
#line 4817 "preproc.y"
{ yyval.type_enum = ECPGt_varchar; ;
    break;}
case 1136:
#line 4820 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 1137:
#line 4824 "preproc.y"
{
		yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
	;
    break;}
case 1138:
#line 4829 "preproc.y"
{
			struct ECPGtype * type;
                        int dimension = yyvsp[-1].index.index1; /* dimension of array */
                        int length = yyvsp[-1].index.index2;    /* lenght of string */
                        char dim[14L], ascii_len[12];

			adjust_array(actual_type[struct_level].type_enum, &dimension, &length, actual_type[struct_level].type_dimension, actual_type[struct_level].type_index, strlen(yyvsp[-3].str));

			switch (actual_type[struct_level].type_enum)
			{
			   case ECPGt_struct:
			   case ECPGt_union:
                               if (dimension < 0)
                                   type = ECPGmake_struct_type(struct_member_list[struct_level], actual_type[struct_level].type_enum);
                               else
                                   type = ECPGmake_array_type(ECPGmake_struct_type(struct_member_list[struct_level], actual_type[struct_level].type_enum), dimension); 

                               yyval.str = make4_str(yyvsp[-3].str, mm_strdup(yyvsp[-2].str), yyvsp[-1].index.str, yyvsp[0].str);
                               break;
                           case ECPGt_varchar:
                               if (dimension == -1)
                                   type = ECPGmake_simple_type(actual_type[struct_level].type_enum, length);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, length), dimension);

                               switch(dimension)
                               {
                                  case 0:
                                      strcpy(dim, "[]");
                                      break;
				  case -1:
                                  case 1:
                                      *dim = '\0';
                                      break;
                                  default:
                                      sprintf(dim, "[%d]", dimension);
                                      break;
                               }
			       sprintf(ascii_len, "%d", length);

                               if (length > 0)
                                   yyval.str = make4_str(make5_str(mm_strdup(actual_storage[struct_level]), make1_str(" struct varchar_"), mm_strdup(yyvsp[-2].str), make1_str(" { int len; char arr["), mm_strdup(ascii_len)), make1_str("]; } "), mm_strdup(yyvsp[-2].str), mm_strdup(dim));
                               else
                                   yyval.str = make4_str(make3_str(mm_strdup(actual_storage[struct_level]), make1_str(" struct varchar_"), mm_strdup(yyvsp[-2].str)), make1_str(" { int len; char *arr; } "), mm_strdup(yyvsp[-2].str), mm_strdup(dim));
                               break;
                           case ECPGt_char:
                           case ECPGt_unsigned_char:
                               if (dimension == -1)
                                   type = ECPGmake_simple_type(actual_type[struct_level].type_enum, length);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, length), dimension);

			       yyval.str = make4_str(yyvsp[-3].str, mm_strdup(yyvsp[-2].str), yyvsp[-1].index.str, yyvsp[0].str);
                               break;
                           default:
                               if (dimension < 0)
                                   type = ECPGmake_simple_type(actual_type[struct_level].type_enum, 1);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, 1), dimension);

			       yyval.str = make4_str(yyvsp[-3].str, mm_strdup(yyvsp[-2].str), yyvsp[-1].index.str, yyvsp[0].str);
                               break;
			}

			if (struct_level == 0)
				new_variable(yyvsp[-2].str, type);
			else
				ECPGmake_struct_member(yyvsp[-2].str, type, &(struct_member_list[struct_level - 1]));

			free(yyvsp[-2].str);
		;
    break;}
case 1139:
#line 4901 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1140:
#line 4902 "preproc.y"
{ yyval.str = make2_str(make1_str("="), yyvsp[0].str); ;
    break;}
case 1141:
#line 4904 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1142:
#line 4905 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 1143:
#line 4912 "preproc.y"
{
		/* this is only supported for compatibility */
		yyval.str = cat3_str(make1_str("/* declare statement"), yyvsp[0].str, make1_str("*/"));
	;
    break;}
case 1144:
#line 4919 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1145:
#line 4921 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1146:
#line 4922 "preproc.y"
{ yyval.str = make1_str("CURRENT"); ;
    break;}
case 1147:
#line 4923 "preproc.y"
{ yyval.str = make1_str("ALL"); ;
    break;}
case 1148:
#line 4924 "preproc.y"
{ yyval.str = make1_str("CURRENT"); ;
    break;}
case 1149:
#line 4926 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1150:
#line 4927 "preproc.y"
{ yyval.str = make1_str("DEFAULT"); ;
    break;}
case 1151:
#line 4933 "preproc.y"
{ 
		struct variable *thisquery = (struct variable *)mm_alloc(sizeof(struct variable));

		thisquery->type = &ecpg_query;
		thisquery->brace_level = 0;
		thisquery->next = NULL;
		thisquery->name = yyvsp[0].str;

		add_variable(&argsinsert, thisquery, &no_indicator); 

		yyval.str = make1_str(";;");
	;
    break;}
case 1152:
#line 4946 "preproc.y"
{
		struct variable *thisquery = (struct variable *)mm_alloc(sizeof(struct variable));

		thisquery->type = &ecpg_query;
		thisquery->brace_level = 0;
		thisquery->next = NULL;
		thisquery->name = (char *) mm_alloc(sizeof("ECPGprepared_statement(\"\")") + strlen(yyvsp[0].str));
		sprintf(thisquery->name, "ECPGprepared_statement(\"%s\")", yyvsp[0].str);

		add_variable(&argsinsert, thisquery, &no_indicator); 
	;
    break;}
case 1153:
#line 4957 "preproc.y"
{
		yyval.str = make1_str(";;");
	;
    break;}
case 1155:
#line 4962 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1156:
#line 4968 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1157:
#line 4973 "preproc.y"
{
		yyval.str = yyvsp[-1].str;
;
    break;}
case 1158:
#line 4977 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1159:
#line 4978 "preproc.y"
{
					/* yyerror ("open cursor with variables not implemented yet"); */
					yyval.str = make1_str("");
				;
    break;}
case 1162:
#line 4990 "preproc.y"
{
		yyval.str = make4_str(make1_str("\""), yyvsp[-2].str, make1_str("\", "), yyvsp[0].str);
	;
    break;}
case 1163:
#line 5000 "preproc.y"
{
		if (strncmp(yyvsp[-1].str, "begin", 5) == 0)
                        yyerror("RELEASE does not make sense when beginning a transaction");

		fprintf(yyout, "ECPGtrans(__LINE__, %s, \"%s\");", connection, yyvsp[-1].str);
		whenever_action(0);
		fprintf(yyout, "ECPGdisconnect(\"\");"); 
		whenever_action(0);
		free(yyvsp[-1].str);
	;
    break;}
case 1164:
#line 5016 "preproc.y"
{
				yyval.str = yyvsp[0].str;
                        ;
    break;}
case 1165:
#line 5024 "preproc.y"
{
		/* add entry to list */
		struct typedefs *ptr, *this;
		int dimension = yyvsp[-1].index.index1;
		int length = yyvsp[-1].index.index2;

		for (ptr = types; ptr != NULL; ptr = ptr->next)
		{
			if (strcmp(yyvsp[-4].str, ptr->name) == 0)
			{
			        /* re-definition is a bug */
				sprintf(errortext, "type %s already defined", yyvsp[-4].str);
				yyerror(errortext);
	                }
		}

		adjust_array(yyvsp[-2].type.type_enum, &dimension, &length, yyvsp[-2].type.type_dimension, yyvsp[-2].type.type_index, strlen(yyvsp[0].str));

        	this = (struct typedefs *) mm_alloc(sizeof(struct typedefs));

        	/* initial definition */
	        this->next = types;
	        this->name = yyvsp[-4].str;
		this->type = (struct this_type *) mm_alloc(sizeof(struct this_type));
		this->type->type_enum = yyvsp[-2].type.type_enum;
		this->type->type_str = mm_strdup(yyvsp[-4].str);
		this->type->type_dimension = dimension; /* dimension of array */
		this->type->type_index = length;    /* lenght of string */
		this->struct_member_list = struct_member_list[struct_level];

		if (yyvsp[-2].type.type_enum != ECPGt_varchar &&
		    yyvsp[-2].type.type_enum != ECPGt_char &&
	            yyvsp[-2].type.type_enum != ECPGt_unsigned_char &&
		    this->type->type_index >= 0)
                            yyerror("No multi-dimensional array support for simple data types");

        	types = this;

		yyval.str = cat5_str(cat3_str(make1_str("/* exec sql type"), mm_strdup(yyvsp[-4].str), make1_str("is")), mm_strdup(yyvsp[-2].type.type_str), mm_strdup(yyvsp[-1].index.str), yyvsp[0].str, make1_str("*/"));
	;
    break;}
case 1166:
#line 5066 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1167:
#line 5072 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1168:
#line 5078 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1169:
#line 5084 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1170:
#line 5090 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 1171:
#line 5098 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1172:
#line 5104 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1173:
#line 5110 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1174:
#line 5116 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1175:
#line 5122 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 1176:
#line 5128 "preproc.y"
{ yyval.str = make1_str("reference"); ;
    break;}
case 1177:
#line 5129 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1178:
#line 5132 "preproc.y"
{
		yyval.type.type_str = make1_str("char");
                yyval.type.type_enum = ECPGt_char;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1179:
#line 5139 "preproc.y"
{
		yyval.type.type_str = make1_str("varchar");
                yyval.type.type_enum = ECPGt_varchar;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1180:
#line 5146 "preproc.y"
{
		yyval.type.type_str = make1_str("float");
                yyval.type.type_enum = ECPGt_float;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1181:
#line 5153 "preproc.y"
{
		yyval.type.type_str = make1_str("double");
                yyval.type.type_enum = ECPGt_double;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1182:
#line 5160 "preproc.y"
{
		yyval.type.type_str = make1_str("int");
       	        yyval.type.type_enum = ECPGt_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1183:
#line 5167 "preproc.y"
{
		yyval.type.type_str = make1_str("int");
       	        yyval.type.type_enum = ECPGt_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1184:
#line 5174 "preproc.y"
{
		yyval.type.type_str = make1_str("short");
       	        yyval.type.type_enum = ECPGt_short;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1185:
#line 5181 "preproc.y"
{
		yyval.type.type_str = make1_str("long");
       	        yyval.type.type_enum = ECPGt_long;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1186:
#line 5188 "preproc.y"
{
		yyval.type.type_str = make1_str("bool");
       	        yyval.type.type_enum = ECPGt_bool;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1187:
#line 5195 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned int");
       	        yyval.type.type_enum = ECPGt_unsigned_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1188:
#line 5202 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned short");
       	        yyval.type.type_enum = ECPGt_unsigned_short;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1189:
#line 5209 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned long");
       	        yyval.type.type_enum = ECPGt_unsigned_long;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1190:
#line 5216 "preproc.y"
{
		struct_member_list[struct_level++] = NULL;
		if (struct_level >= STRUCT_DEPTH)
        		yyerror("Too many levels in nested structure definition");
	;
    break;}
case 1191:
#line 5221 "preproc.y"
{
		ECPGfree_struct_member(struct_member_list[struct_level--]);
		yyval.type.type_str = cat3_str(make1_str("struct {"), yyvsp[-1].str, make1_str("}"));
		yyval.type.type_enum = ECPGt_struct;
                yyval.type.type_index = -1;
                yyval.type.type_dimension = -1;
	;
    break;}
case 1192:
#line 5229 "preproc.y"
{
		struct_member_list[struct_level++] = NULL;
		if (struct_level >= STRUCT_DEPTH)
        		yyerror("Too many levels in nested structure definition");
	;
    break;}
case 1193:
#line 5234 "preproc.y"
{
		ECPGfree_struct_member(struct_member_list[struct_level--]);
		yyval.type.type_str = cat3_str(make1_str("union {"), yyvsp[-1].str, make1_str("}"));
		yyval.type.type_enum = ECPGt_union;
                yyval.type.type_index = -1;
                yyval.type.type_dimension = -1;
	;
    break;}
case 1194:
#line 5242 "preproc.y"
{
		struct typedefs *this = get_typedef(yyvsp[0].str);

		yyval.type.type_str = mm_strdup(yyvsp[0].str);
		yyval.type.type_enum = this->type->type_enum;
		yyval.type.type_dimension = this->type->type_dimension;
		yyval.type.type_index = this->type->type_index;
		struct_member_list[struct_level] = this->struct_member_list;
	;
    break;}
case 1197:
#line 5255 "preproc.y"
{
		yyval.str = make1_str("");
	;
    break;}
case 1198:
#line 5259 "preproc.y"
{
		yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
	;
    break;}
case 1199:
#line 5265 "preproc.y"
{
		actual_type[struct_level].type_enum = yyvsp[0].type.type_enum;
		actual_type[struct_level].type_dimension = yyvsp[0].type.type_dimension;
		actual_type[struct_level].type_index = yyvsp[0].type.type_index;
	;
    break;}
case 1200:
#line 5271 "preproc.y"
{
		yyval.str = cat3_str(yyvsp[-3].type.type_str, yyvsp[-1].str, make1_str(";"));
	;
    break;}
case 1201:
#line 5276 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 1202:
#line 5280 "preproc.y"
{
		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
	;
    break;}
case 1203:
#line 5285 "preproc.y"
{
			int dimension = yyvsp[0].index.index1;
			int length = yyvsp[0].index.index2;
			struct ECPGtype * type;
                        char dim[14L];

			adjust_array(actual_type[struct_level].type_enum, &dimension, &length, actual_type[struct_level].type_dimension, actual_type[struct_level].type_index, strlen(yyvsp[-2].str));

			switch (actual_type[struct_level].type_enum)
			{
			   case ECPGt_struct:
			   case ECPGt_union:
                               if (dimension < 0)
                                   type = ECPGmake_struct_type(struct_member_list[struct_level], actual_type[struct_level].type_enum);
                               else
                                   type = ECPGmake_array_type(ECPGmake_struct_type(struct_member_list[struct_level], actual_type[struct_level].type_enum), dimension); 

                               break;
                           case ECPGt_varchar:
                               if (dimension == -1)
                                   type = ECPGmake_simple_type(actual_type[struct_level].type_enum, length);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, length), dimension);

                               switch(dimension)
                               {
                                  case 0:
                                      strcpy(dim, "[]");
                                      break;
				  case -1:
                                  case 1:
                                      *dim = '\0';
                                      break;
                                  default:
                                      sprintf(dim, "[%d]", dimension);
                                      break;
                                }

                               break;
                           case ECPGt_char:
                           case ECPGt_unsigned_char:
                               if (dimension == -1)
                                   type = ECPGmake_simple_type(actual_type[struct_level].type_enum, length);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, length), dimension);

                               break;
                           default:
			       if (length >= 0)
                	            yyerror("No multi-dimensional array support for simple data types");

                               if (dimension < 0)
                                   type = ECPGmake_simple_type(actual_type[struct_level].type_enum, 1);
                               else
                                   type = ECPGmake_array_type(ECPGmake_simple_type(actual_type[struct_level].type_enum, 1), dimension);

                               break;
			}

			if (struct_level == 0)
				new_variable(yyvsp[-1].str, type);
			else
				ECPGmake_struct_member(yyvsp[-1].str, type, &(struct_member_list[struct_level - 1]));

			yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].index.str);
		;
    break;}
case 1204:
#line 5356 "preproc.y"
{
		struct variable *p = find_variable(yyvsp[-4].str);
		int dimension = yyvsp[-1].index.index1;
		int length = yyvsp[-1].index.index2;
		struct ECPGtype * type;

		adjust_array(yyvsp[-2].type.type_enum, &dimension, &length, yyvsp[-2].type.type_dimension, yyvsp[-2].type.type_index, strlen(yyvsp[0].str));

		switch (yyvsp[-2].type.type_enum)
		{
		   case ECPGt_struct:
		   case ECPGt_union:
                        if (dimension < 0)
                            type = ECPGmake_struct_type(struct_member_list[struct_level], yyvsp[-2].type.type_enum);
                        else
                            type = ECPGmake_array_type(ECPGmake_struct_type(struct_member_list[struct_level], yyvsp[-2].type.type_enum), dimension); 
                        break;
                   case ECPGt_varchar:
                        if (dimension == -1)
                            type = ECPGmake_simple_type(yyvsp[-2].type.type_enum, length);
                        else
                            type = ECPGmake_array_type(ECPGmake_simple_type(yyvsp[-2].type.type_enum, length), dimension);

			break;
                   case ECPGt_char:
                   case ECPGt_unsigned_char:
                        if (dimension == -1)
                            type = ECPGmake_simple_type(yyvsp[-2].type.type_enum, length);
                        else
                            type = ECPGmake_array_type(ECPGmake_simple_type(yyvsp[-2].type.type_enum, length), dimension);

			break;
		   default:
			if (length >= 0)
                	    yyerror("No multi-dimensional array support for simple data types");

                        if (dimension < 0)
                            type = ECPGmake_simple_type(yyvsp[-2].type.type_enum, 1);
                        else
                            type = ECPGmake_array_type(ECPGmake_simple_type(yyvsp[-2].type.type_enum, 1), dimension);

			break;
		}	

		ECPGfree_type(p->type);
		p->type = type;

		yyval.str = cat5_str(cat3_str(make1_str("/* exec sql var"), mm_strdup(yyvsp[-4].str), make1_str("is")), mm_strdup(yyvsp[-2].type.type_str), mm_strdup(yyvsp[-1].index.str), yyvsp[0].str, make1_str("*/"));
	;
    break;}
case 1205:
#line 5410 "preproc.y"
{
	when_error.code = yyvsp[0].action.code;
	when_error.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever sqlerror "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1206:
#line 5415 "preproc.y"
{
	when_nf.code = yyvsp[0].action.code;
	when_nf.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever not found "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1207:
#line 5420 "preproc.y"
{
	when_warn.code = yyvsp[0].action.code;
	when_warn.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever sql_warning "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1208:
#line 5426 "preproc.y"
{
	yyval.action.code = W_NOTHING;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("continue");
;
    break;}
case 1209:
#line 5431 "preproc.y"
{
	yyval.action.code = W_SQLPRINT;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("sqlprint");
;
    break;}
case 1210:
#line 5436 "preproc.y"
{
	yyval.action.code = W_STOP;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("stop");
;
    break;}
case 1211:
#line 5441 "preproc.y"
{
        yyval.action.code = W_GOTO;
        yyval.action.command = strdup(yyvsp[0].str);
	yyval.action.str = cat2_str(make1_str("goto "), yyvsp[0].str);
;
    break;}
case 1212:
#line 5446 "preproc.y"
{
        yyval.action.code = W_GOTO;
        yyval.action.command = strdup(yyvsp[0].str);
	yyval.action.str = cat2_str(make1_str("goto "), yyvsp[0].str);
;
    break;}
case 1213:
#line 5451 "preproc.y"
{
	yyval.action.code = W_DO;
	yyval.action.command = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
	yyval.action.str = cat2_str(make1_str("do"), mm_strdup(yyval.action.command));
;
    break;}
case 1214:
#line 5456 "preproc.y"
{
        yyval.action.code = W_BREAK;
        yyval.action.command = NULL;
        yyval.action.str = make1_str("break");
;
    break;}
case 1215:
#line 5461 "preproc.y"
{
	yyval.action.code = W_DO;
	yyval.action.command = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
	yyval.action.str = cat2_str(make1_str("call"), mm_strdup(yyval.action.command));
;
    break;}
case 1216:
#line 5469 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 1217:
#line 5473 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 1218:
#line 5475 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 1219:
#line 5477 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 1220:
#line 5481 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 1221:
#line 5483 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 1222:
#line 5485 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 1223:
#line 5487 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 1224:
#line 5489 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 1225:
#line 5491 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 1226:
#line 5493 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 1227:
#line 5495 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 1228:
#line 5499 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 1229:
#line 5501 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 1230:
#line 5503 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 1231:
#line 5507 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 1232:
#line 5511 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 1233:
#line 5513 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 1234:
#line 5515 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 1235:
#line 5517 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 1236:
#line 5519 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1237:
#line 5521 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1238:
#line 5523 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make1_str("(*)")); 
				;
    break;}
case 1239:
#line 5527 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 1240:
#line 5531 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1241:
#line 5535 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 1242:
#line 5539 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 1243:
#line 5543 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 1244:
#line 5549 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 1245:
#line 5553 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 1246:
#line 5559 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 1247:
#line 5563 "preproc.y"
{
					yyval.str = make3_str(make1_str("exists("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1248:
#line 5567 "preproc.y"
{
					yyval.str = make3_str(make1_str("extract("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1249:
#line 5571 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1250:
#line 5575 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1251:
#line 5580 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1252:
#line 5584 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1253:
#line 5588 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1254:
#line 5592 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1255:
#line 5596 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 1256:
#line 5598 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 1257:
#line 5600 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 1258:
#line 5602 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 1259:
#line 5609 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); }
				;
    break;}
case 1260:
#line 5613 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); }
				;
    break;}
case 1261:
#line 5617 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); }
				;
    break;}
case 1262:
#line 5621 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); }
				;
    break;}
case 1263:
#line 5625 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 1264:
#line 5629 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 1265:
#line 5633 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1266:
#line 5637 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("not in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1267:
#line 5641 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-4].str, yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1268:
#line 5645 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("+("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1269:
#line 5649 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("-("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1270:
#line 5653 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("/("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1271:
#line 5657 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("*("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1272:
#line 5661 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("<("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1273:
#line 5665 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(">("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1274:
#line 5669 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("=("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1275:
#line 5673 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("any ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1276:
#line 5677 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1277:
#line 5681 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1278:
#line 5685 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1279:
#line 5689 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1280:
#line 5693 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1281:
#line 5697 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1282:
#line 5701 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1283:
#line 5705 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("all ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1284:
#line 5709 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1285:
#line 5713 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1286:
#line 5717 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1287:
#line 5721 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1288:
#line 5725 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1289:
#line 5729 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1290:
#line 5733 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1291:
#line 5737 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 1292:
#line 5739 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 1293:
#line 5741 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 1294:
#line 5743 "preproc.y"
{ 	yyval.str = yyvsp[0].str; ;
    break;}
case 1297:
#line 5748 "preproc.y"
{ reset_variables();;
    break;}
case 1298:
#line 5750 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1299:
#line 5751 "preproc.y"
{ yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1300:
#line 5753 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1301:
#line 5754 "preproc.y"
{ yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1302:
#line 5756 "preproc.y"
{
		add_variable(&argsresult, find_variable(yyvsp[-1].str), (yyvsp[0].str == NULL) ? &no_indicator : find_variable(yyvsp[0].str)); 
;
    break;}
case 1303:
#line 5760 "preproc.y"
{
		add_variable(&argsinsert, find_variable(yyvsp[-1].str), (yyvsp[0].str == NULL) ? &no_indicator : find_variable(yyvsp[0].str)); 
;
    break;}
case 1304:
#line 5764 "preproc.y"
{
		add_variable(&argsinsert, find_variable(yyvsp[0].str), &no_indicator); 
		yyval.str = make1_str(";;");
;
    break;}
case 1305:
#line 5769 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1306:
#line 5771 "preproc.y"
{ yyval.str = NULL; ;
    break;}
case 1307:
#line 5772 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1308:
#line 5773 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1309:
#line 5774 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1310:
#line 5776 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1311:
#line 5777 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1312:
#line 5782 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1313:
#line 5784 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1314:
#line 5786 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1315:
#line 5788 "preproc.y"
{
			yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str);
		;
    break;}
case 1317:
#line 5792 "preproc.y"
{ yyval.str = make1_str(";"); ;
    break;}
case 1318:
#line 5794 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1319:
#line 5795 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1320:
#line 5796 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1321:
#line 5797 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1322:
#line 5798 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 1323:
#line 5799 "preproc.y"
{ yyval.str = make1_str("auto"); ;
    break;}
case 1324:
#line 5800 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 1325:
#line 5801 "preproc.y"
{ yyval.str = make1_str("char"); ;
    break;}
case 1326:
#line 5802 "preproc.y"
{ yyval.str = make1_str("const"); ;
    break;}
case 1327:
#line 5803 "preproc.y"
{ yyval.str = make1_str("double"); ;
    break;}
case 1328:
#line 5804 "preproc.y"
{ yyval.str = make1_str("enum"); ;
    break;}
case 1329:
#line 5805 "preproc.y"
{ yyval.str = make1_str("extern"); ;
    break;}
case 1330:
#line 5806 "preproc.y"
{ yyval.str = make1_str("float"); ;
    break;}
case 1331:
#line 5807 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 1332:
#line 5808 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 1333:
#line 5809 "preproc.y"
{ yyval.str = make1_str("register"); ;
    break;}
case 1334:
#line 5810 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 1335:
#line 5811 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1336:
#line 5812 "preproc.y"
{ yyval.str = make1_str("static"); ;
    break;}
case 1337:
#line 5813 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 1338:
#line 5814 "preproc.y"
{ yyval.str = make1_str("union"); ;
    break;}
case 1339:
#line 5815 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 1340:
#line 5816 "preproc.y"
{ yyval.str = make1_str("varchar"); ;
    break;}
case 1341:
#line 5817 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 1342:
#line 5818 "preproc.y"
{ yyval.str = make1_str("["); ;
    break;}
case 1343:
#line 5819 "preproc.y"
{ yyval.str = make1_str("]"); ;
    break;}
case 1344:
#line 5820 "preproc.y"
{ yyval.str = make1_str("("); ;
    break;}
case 1345:
#line 5821 "preproc.y"
{ yyval.str = make1_str(")"); ;
    break;}
case 1346:
#line 5822 "preproc.y"
{ yyval.str = make1_str("="); ;
    break;}
case 1347:
#line 5823 "preproc.y"
{ yyval.str = make1_str(","); ;
    break;}
case 1348:
#line 5825 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1349:
#line 5826 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\""));;
    break;}
case 1350:
#line 5827 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1351:
#line 5828 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1352:
#line 5829 "preproc.y"
{ yyval.str = make1_str(","); ;
    break;}
case 1353:
#line 5831 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1354:
#line 5832 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1355:
#line 5833 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1356:
#line 5834 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1357:
#line 5835 "preproc.y"
{ yyval.str = make3_str(make1_str("{"), yyvsp[-1].str, make1_str("}")); ;
    break;}
case 1358:
#line 5837 "preproc.y"
{
    braces_open++;
    yyval.str = make1_str("{");
;
    break;}
case 1359:
#line 5842 "preproc.y"
{
    remove_variables(braces_open--);
    yyval.str = make1_str("}");
;
    break;}
}
   /* the action file gets copied in in place of this dollarsign */
#line 542 "/usr/share/misc/bison.simple"

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

 yyacceptlab:
  /* YYACCEPT comes here.  */
  if (yyfree_stacks)
    {
      free (yyss);
      free (yyvs);
#ifdef YYLSP_NEEDED
      free (yyls);
#endif
    }
  return 0;

 yyabortlab:
  /* YYABORT comes here.  */
  if (yyfree_stacks)
    {
      free (yyss);
      free (yyvs);
#ifdef YYLSP_NEEDED
      free (yyls);
#endif
    }
  return 1;
}
#line 5847 "preproc.y"


void yyerror(char * error)
{
    fprintf(stderr, "%s:%d: %s\n", input_filename, yylineno, error);
    exit(PARSE_ERROR);
}
