
/*  A Bison parser, made from preproc.y
 by  GNU Bison version 1.25
  */

#define YYBISON 1  /* Identify Bison output.  */

#define	SQL_AT	258
#define	SQL_BOOL	259
#define	SQL_BREAK	260
#define	SQL_CALL	261
#define	SQL_CONNECT	262
#define	SQL_CONNECTION	263
#define	SQL_CONTINUE	264
#define	SQL_DEALLOCATE	265
#define	SQL_DISCONNECT	266
#define	SQL_ENUM	267
#define	SQL_FOUND	268
#define	SQL_FREE	269
#define	SQL_GO	270
#define	SQL_GOTO	271
#define	SQL_IDENTIFIED	272
#define	SQL_IMMEDIATE	273
#define	SQL_INDICATOR	274
#define	SQL_INT	275
#define	SQL_LONG	276
#define	SQL_OPEN	277
#define	SQL_PREPARE	278
#define	SQL_RELEASE	279
#define	SQL_REFERENCE	280
#define	SQL_SECTION	281
#define	SQL_SEMI	282
#define	SQL_SHORT	283
#define	SQL_SIGNED	284
#define	SQL_SQLERROR	285
#define	SQL_SQLPRINT	286
#define	SQL_SQLWARNING	287
#define	SQL_START	288
#define	SQL_STOP	289
#define	SQL_STRUCT	290
#define	SQL_UNSIGNED	291
#define	SQL_VAR	292
#define	SQL_WHENEVER	293
#define	S_ANYTHING	294
#define	S_AUTO	295
#define	S_BOOL	296
#define	S_CHAR	297
#define	S_CONST	298
#define	S_DOUBLE	299
#define	S_ENUM	300
#define	S_EXTERN	301
#define	S_FLOAT	302
#define	S_INT	303
#define	S	304
#define	S_LONG	305
#define	S_REGISTER	306
#define	S_SHORT	307
#define	S_SIGNED	308
#define	S_STATIC	309
#define	S_STRUCT	310
#define	S_UNION	311
#define	S_UNSIGNED	312
#define	S_VARCHAR	313
#define	TYPECAST	314
#define	ABSOLUTE	315
#define	ACTION	316
#define	ADD	317
#define	ALL	318
#define	ALTER	319
#define	AND	320
#define	ANY	321
#define	AS	322
#define	ASC	323
#define	BEGIN_TRANS	324
#define	BETWEEN	325
#define	BOTH	326
#define	BY	327
#define	CASCADE	328
#define	CASE	329
#define	CAST	330
#define	CHAR	331
#define	CHARACTER	332
#define	CHECK	333
#define	CLOSE	334
#define	COALESCE	335
#define	COLLATE	336
#define	COLUMN	337
#define	COMMIT	338
#define	CONSTRAINT	339
#define	CREATE	340
#define	CROSS	341
#define	CURRENT	342
#define	CURRENT_DATE	343
#define	CURRENT_TIME	344
#define	CURRENT_TIMESTAMP	345
#define	CURRENT_USER	346
#define	CURSOR	347
#define	DAY_P	348
#define	DECIMAL	349
#define	DECLARE	350
#define	DEFAULT	351
#define	DELETE	352
#define	DESC	353
#define	DISTINCT	354
#define	DOUBLE	355
#define	DROP	356
#define	ELSE	357
#define	END_TRANS	358
#define	EXCEPT	359
#define	EXECUTE	360
#define	EXISTS	361
#define	EXTRACT	362
#define	FALSE_P	363
#define	FETCH	364
#define	FLOAT	365
#define	FOR	366
#define	FOREIGN	367
#define	FROM	368
#define	FULL	369
#define	GRANT	370
#define	GROUP	371
#define	HAVING	372
#define	HOUR_P	373
#define	IN	374
#define	INNER_P	375
#define	INSENSITIVE	376
#define	INSERT	377
#define	INTERSECT	378
#define	INTERVAL	379
#define	INTO	380
#define	IS	381
#define	ISOLATION	382
#define	JOIN	383
#define	KEY	384
#define	LANGUAGE	385
#define	LEADING	386
#define	LEFT	387
#define	LEVEL	388
#define	LIKE	389
#define	LOCAL	390
#define	MATCH	391
#define	MINUTE_P	392
#define	MONTH_P	393
#define	NAMES	394
#define	NATIONAL	395
#define	NATURAL	396
#define	NCHAR	397
#define	NEXT	398
#define	NO	399
#define	NOT	400
#define	NULLIF	401
#define	NULL_P	402
#define	NUMERIC	403
#define	OF	404
#define	ON	405
#define	ONLY	406
#define	OPTION	407
#define	OR	408
#define	ORDER	409
#define	OUTER_P	410
#define	PARTIAL	411
#define	POSITION	412
#define	PRECISION	413
#define	PRIMARY	414
#define	PRIOR	415
#define	PRIVILEGES	416
#define	PROCEDURE	417
#define	PUBLIC	418
#define	READ	419
#define	REFERENCES	420
#define	RELATIVE	421
#define	REVOKE	422
#define	RIGHT	423
#define	ROLLBACK	424
#define	SCROLL	425
#define	SECOND_P	426
#define	SELECT	427
#define	SET	428
#define	SUBSTRING	429
#define	TABLE	430
#define	TEMP	431
#define	THEN	432
#define	TIME	433
#define	TIMESTAMP	434
#define	TIMEZONE_HOUR	435
#define	TIMEZONE_MINUTE	436
#define	TO	437
#define	TRAILING	438
#define	TRANSACTION	439
#define	TRIM	440
#define	TRUE_P	441
#define	UNION	442
#define	UNIQUE	443
#define	UPDATE	444
#define	USER	445
#define	USING	446
#define	VALUES	447
#define	VARCHAR	448
#define	VARYING	449
#define	VIEW	450
#define	WHEN	451
#define	WHERE	452
#define	WITH	453
#define	WORK	454
#define	YEAR_P	455
#define	ZONE	456
#define	TRIGGER	457
#define	TYPE_P	458
#define	ABORT_TRANS	459
#define	AFTER	460
#define	AGGREGATE	461
#define	ANALYZE	462
#define	BACKWARD	463
#define	BEFORE	464
#define	BINARY	465
#define	CACHE	466
#define	CLUSTER	467
#define	COPY	468
#define	CREATEDB	469
#define	CREATEUSER	470
#define	CYCLE	471
#define	DATABASE	472
#define	DELIMITERS	473
#define	DO	474
#define	EACH	475
#define	ENCODING	476
#define	EXPLAIN	477
#define	EXTEND	478
#define	FORWARD	479
#define	FUNCTION	480
#define	HANDLER	481
#define	INCREMENT	482
#define	INDEX	483
#define	INHERITS	484
#define	INSTEAD	485
#define	ISNULL	486
#define	LANCOMPILER	487
#define	LIMIT	488
#define	LISTEN	489
#define	UNLISTEN	490
#define	LOAD	491
#define	LOCATION	492
#define	LOCK_P	493
#define	MAXVALUE	494
#define	MINVALUE	495
#define	MOVE	496
#define	NEW	497
#define	NOCREATEDB	498
#define	NOCREATEUSER	499
#define	NONE	500
#define	NOTHING	501
#define	NOTIFY	502
#define	NOTNULL	503
#define	OFFSET	504
#define	OIDS	505
#define	OPERATOR	506
#define	PASSWORD	507
#define	PROCEDURAL	508
#define	RECIPE	509
#define	RENAME	510
#define	RESET	511
#define	RETURNS	512
#define	ROW	513
#define	RULE	514
#define	SERIAL	515
#define	SEQUENCE	516
#define	SETOF	517
#define	SHOW	518
#define	START	519
#define	STATEMENT	520
#define	STDIN	521
#define	STDOUT	522
#define	TRUSTED	523
#define	UNTIL	524
#define	VACUUM	525
#define	VALID	526
#define	VERBOSE	527
#define	VERSION	528
#define	IDENT	529
#define	SCONST	530
#define	Op	531
#define	CSTRING	532
#define	CVARIABLE	533
#define	CPP_LINE	534
#define	ICONST	535
#define	PARAM	536
#define	FCONST	537
#define	OP	538
#define	UMINUS	539

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

    if (c == '-')
    {
	if (p->type->typ != ECPGt_struct && p->type->typ != ECPGt_union)
        {
                sprintf(errortext, "variable %s is not a pointer", name);
                yyerror (errortext);
        }

	if (p->type->u.element->typ  != ECPGt_struct && p->type->u.element->typ != ECPGt_union)
        {
                sprintf(errortext, "variable %s is not a pointer to a structure or a union", name);
                yyerror (errortext);
        }

	/* restore the name, we will need it later on */
	*next = c;
	next++;

	return find_struct_member(name, next, p->type->u.element->u.members);
    }
    else
    {
	if (p->type->typ != ECPGt_struct && p->type->typ != ECPGt_union)
	{
		sprintf(errortext, "variable %s is neither a structure nor a union", name);
		yyerror (errortext);
	}

	/* restore the name, we will need it later on */
	*next = c;

	return find_struct_member(name, next, p->type->u.members);
    }
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

	if (*length >= 0 && *dimension >= 0 && pointer)
		yyerror("No multi-dimensional array support");

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
	        /* pointer has to get dimension 0 */
                if (pointer)
                    *dimension = 0;

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


#line 637 "preproc.y"
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



#define	YYFINAL		2400
#define	YYFLAG		-32768
#define	YYNTBASE	303

#define YYTRANSLATE(x) ((unsigned)(x) <= 539 ? yytranslate[x] : 666)

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
     2,     2,     2,     2,     2,     1,     2,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    12,    13,    14,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    25,
    26,    27,    28,    29,    30,    31,    32,    33,    34,    35,
    36,    37,    38,    39,    40,    41,    42,    43,    44,    45,
    46,    47,    48,    49,    50,    51,    52,    53,    54,    55,
    56,    57,    58,    59,    60,    61,    62,    63,    64,    65,
    66,    67,    68,    69,    70,    71,    72,    73,    74,    75,
    76,    77,    78,    79,    80,    81,    82,    83,    84,    85,
    86,    87,    88,    89,    90,    91,    92,    93,    94,    95,
    96,    97,    98,    99,   100,   101,   102,   103,   104,   105,
   106,   107,   108,   109,   110,   111,   112,   113,   114,   115,
   116,   117,   118,   119,   120,   121,   122,   123,   124,   125,
   126,   127,   128,   129,   130,   131,   132,   133,   134,   135,
   136,   137,   138,   139,   140,   141,   142,   143,   144,   145,
   146,   147,   148,   149,   150,   151,   152,   153,   154,   155,
   156,   157,   158,   159,   160,   161,   162,   163,   164,   165,
   166,   167,   168,   169,   170,   171,   172,   173,   174,   175,
   176,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,   187,   188,   189,   190,   191,   192,   193,   194,   195,
   196,   197,   198,   199,   200,   201,   202,   203,   204,   205,
   206,   207,   208,   209,   210,   211,   212,   213,   214,   215,
   216,   217,   218,   219,   220,   221,   222,   223,   224,   225,
   226,   227,   228,   229,   230,   231,   232,   233,   234,   235,
   236,   237,   238,   239,   240,   241,   242,   243,   244,   245,
   246,   247,   248,   249,   250,   251,   252,   253,   254,   255,
   256,   257,   258,   259,   260,   261,   262,   263,   264,   265,
   266,   267,   268,   269,   270,   271,   272,   273,   274,   275,
   276,   277,   278,   279,   280,   281,   282,   283,   294
};

#if YYDEBUG != 0
static const short yyprhs[] = {     0,
     0,     2,     3,     6,    11,    15,    17,    19,    21,    23,
    25,    28,    30,    32,    34,    36,    38,    40,    42,    44,
    46,    48,    50,    52,    54,    56,    58,    60,    62,    64,
    66,    68,    70,    72,    74,    76,    78,    80,    82,    84,
    86,    88,    90,    92,    94,    96,    98,   100,   102,   104,
   106,   108,   110,   112,   114,   116,   118,   120,   122,   124,
   126,   128,   130,   132,   134,   136,   138,   140,   149,   158,
   162,   166,   167,   169,   171,   172,   174,   176,   177,   181,
   183,   187,   188,   192,   193,   198,   203,   208,   215,   221,
   225,   227,   229,   231,   233,   235,   238,   242,   247,   250,
   254,   259,   265,   269,   274,   278,   285,   291,   294,   297,
   305,   307,   309,   311,   313,   315,   317,   318,   321,   322,
   326,   327,   336,   338,   339,   343,   345,   346,   348,   350,
   354,   358,   360,   361,   364,   366,   369,   370,   374,   376,
   381,   384,   387,   390,   392,   395,   401,   405,   407,   409,
   412,   416,   420,   424,   428,   432,   436,   440,   443,   446,
   450,   457,   461,   465,   470,   474,   477,   480,   482,   484,
   489,   491,   496,   498,   500,   504,   506,   511,   516,   522,
   533,   537,   539,   541,   543,   545,   548,   552,   556,   560,
   564,   568,   572,   576,   579,   582,   586,   593,   597,   601,
   606,   610,   614,   619,   623,   627,   630,   633,   636,   639,
   643,   646,   651,   655,   659,   664,   669,   675,   682,   688,
   695,   699,   701,   703,   706,   709,   710,   713,   715,   716,
   720,   724,   727,   729,   732,   735,   740,   741,   749,   753,
   754,   758,   760,   762,   767,   770,   771,   774,   776,   779,
   782,   785,   788,   790,   792,   794,   797,   799,   802,   812,
   814,   815,   820,   835,   837,   839,   841,   845,   851,   853,
   855,   857,   861,   863,   864,   866,   868,   870,   874,   875,
   877,   879,   881,   883,   889,   893,   896,   898,   900,   902,
   904,   906,   908,   910,   912,   916,   918,   922,   926,   928,
   932,   934,   936,   938,   940,   943,   947,   951,   958,   963,
   965,   967,   969,   971,   972,   974,   977,   979,   981,   983,
   984,   987,   990,   991,   999,  1002,  1004,  1006,  1008,  1012,
  1014,  1016,  1018,  1020,  1022,  1024,  1027,  1029,  1033,  1034,
  1041,  1053,  1055,  1056,  1059,  1060,  1062,  1064,  1068,  1070,
  1077,  1081,  1084,  1087,  1088,  1090,  1093,  1094,  1099,  1111,
  1114,  1115,  1119,  1122,  1124,  1128,  1131,  1133,  1134,  1138,
  1140,  1142,  1144,  1146,  1151,  1153,  1155,  1160,  1167,  1169,
  1171,  1173,  1175,  1177,  1179,  1181,  1183,  1185,  1187,  1191,
  1195,  1199,  1209,  1211,  1212,  1214,  1215,  1216,  1230,  1232,
  1234,  1236,  1240,  1244,  1246,  1248,  1251,  1255,  1258,  1260,
  1262,  1264,  1266,  1270,  1272,  1274,  1276,  1278,  1280,  1282,
  1283,  1286,  1289,  1292,  1295,  1298,  1301,  1304,  1307,  1310,
  1312,  1314,  1315,  1321,  1324,  1331,  1335,  1339,  1340,  1344,
  1345,  1347,  1349,  1350,  1352,  1354,  1355,  1359,  1364,  1368,
  1374,  1376,  1377,  1379,  1380,  1384,  1385,  1387,  1391,  1395,
  1397,  1399,  1401,  1403,  1405,  1407,  1412,  1417,  1420,  1422,
  1430,  1435,  1439,  1440,  1444,  1446,  1449,  1454,  1458,  1467,
  1475,  1482,  1484,  1485,  1492,  1500,  1502,  1504,  1506,  1509,
  1510,  1513,  1514,  1517,  1520,  1523,  1528,  1532,  1534,  1538,
  1543,  1548,  1557,  1562,  1565,  1566,  1568,  1569,  1571,  1572,
  1574,  1578,  1580,  1581,  1585,  1586,  1588,  1592,  1595,  1598,
  1601,  1604,  1606,  1608,  1609,  1614,  1619,  1622,  1627,  1630,
  1631,  1633,  1635,  1637,  1639,  1641,  1643,  1644,  1646,  1648,
  1652,  1656,  1657,  1660,  1661,  1665,  1666,  1669,  1670,  1673,
  1674,  1678,  1680,  1682,  1686,  1688,  1692,  1695,  1697,  1699,
  1704,  1707,  1710,  1712,  1717,  1722,  1726,  1729,  1732,  1735,
  1737,  1739,  1740,  1742,  1743,  1748,  1751,  1755,  1757,  1759,
  1762,  1763,  1765,  1768,  1772,  1777,  1778,  1782,  1787,  1788,
  1791,  1793,  1796,  1798,  1800,  1802,  1804,  1806,  1808,  1810,
  1812,  1814,  1816,  1818,  1820,  1822,  1824,  1826,  1828,  1830,
  1832,  1834,  1836,  1838,  1840,  1842,  1844,  1846,  1848,  1850,
  1852,  1854,  1856,  1858,  1860,  1862,  1864,  1866,  1868,  1870,
  1873,  1876,  1879,  1882,  1884,  1887,  1889,  1891,  1895,  1896,
  1902,  1906,  1907,  1913,  1917,  1918,  1923,  1925,  1930,  1933,
  1935,  1939,  1942,  1944,  1945,  1949,  1950,  1953,  1954,  1956,
  1959,  1961,  1964,  1966,  1968,  1970,  1972,  1974,  1976,  1980,
  1981,  1983,  1987,  1991,  1995,  1999,  2003,  2007,  2011,  2012,
  2014,  2016,  2024,  2033,  2042,  2050,  2058,  2062,  2064,  2066,
  2068,  2070,  2072,  2074,  2076,  2078,  2080,  2082,  2086,  2088,
  2091,  2093,  2095,  2097,  2100,  2104,  2108,  2112,  2116,  2120,
  2124,  2128,  2132,  2135,  2138,  2142,  2149,  2153,  2157,  2161,
  2166,  2169,  2172,  2177,  2181,  2186,  2188,  2190,  2195,  2197,
  2202,  2204,  2206,  2211,  2216,  2221,  2226,  2232,  2238,  2244,
  2249,  2252,  2256,  2259,  2264,  2268,  2273,  2277,  2282,  2288,
  2295,  2301,  2308,  2314,  2320,  2326,  2332,  2338,  2344,  2350,
  2356,  2363,  2370,  2377,  2384,  2391,  2398,  2405,  2412,  2419,
  2426,  2433,  2440,  2447,  2454,  2461,  2468,  2472,  2476,  2479,
  2481,  2483,  2486,  2488,  2490,  2493,  2497,  2501,  2505,  2509,
  2512,  2515,  2519,  2526,  2530,  2534,  2537,  2540,  2544,  2549,
  2551,  2553,  2558,  2560,  2565,  2567,  2569,  2574,  2579,  2585,
  2591,  2597,  2602,  2604,  2609,  2616,  2617,  2619,  2623,  2627,
  2631,  2632,  2634,  2636,  2638,  2640,  2644,  2645,  2648,  2650,
  2653,  2657,  2661,  2665,  2669,  2672,  2676,  2683,  2687,  2691,
  2694,  2697,  2699,  2703,  2708,  2713,  2718,  2724,  2730,  2736,
  2741,  2745,  2746,  2749,  2750,  2753,  2754,  2758,  2761,  2763,
  2765,  2767,  2769,  2773,  2775,  2777,  2779,  2783,  2789,  2796,
  2801,  2804,  2806,  2811,  2814,  2815,  2818,  2820,  2821,  2825,
  2829,  2831,  2835,  2839,  2843,  2845,  2847,  2852,  2855,  2859,
  2863,  2865,  2869,  2871,  2875,  2877,  2879,  2880,  2882,  2884,
  2886,  2888,  2890,  2892,  2894,  2896,  2898,  2900,  2902,  2904,
  2906,  2909,  2911,  2913,  2915,  2918,  2920,  2922,  2924,  2926,
  2928,  2930,  2932,  2934,  2936,  2938,  2940,  2942,  2944,  2946,
  2948,  2950,  2952,  2954,  2956,  2958,  2960,  2962,  2964,  2966,
  2968,  2970,  2972,  2974,  2976,  2978,  2980,  2982,  2984,  2986,
  2988,  2990,  2992,  2994,  2996,  2998,  3000,  3002,  3004,  3006,
  3008,  3010,  3012,  3014,  3016,  3018,  3020,  3022,  3024,  3026,
  3028,  3030,  3032,  3034,  3036,  3038,  3040,  3042,  3044,  3046,
  3048,  3050,  3052,  3054,  3056,  3058,  3060,  3062,  3064,  3066,
  3068,  3070,  3072,  3074,  3076,  3078,  3080,  3082,  3084,  3086,
  3088,  3090,  3092,  3094,  3096,  3098,  3100,  3102,  3104,  3106,
  3108,  3110,  3112,  3114,  3116,  3118,  3120,  3122,  3124,  3126,
  3128,  3130,  3132,  3134,  3136,  3138,  3140,  3142,  3144,  3146,
  3148,  3150,  3152,  3154,  3156,  3158,  3160,  3162,  3164,  3166,
  3168,  3170,  3172,  3174,  3176,  3178,  3180,  3182,  3184,  3186,
  3188,  3190,  3192,  3194,  3196,  3198,  3200,  3202,  3204,  3206,
  3208,  3210,  3212,  3214,  3220,  3224,  3227,  3231,  3238,  3240,
  3242,  3245,  3248,  3250,  3251,  3253,  3257,  3260,  3261,  3264,
  3265,  3268,  3269,  3271,  3275,  3280,  3284,  3286,  3288,  3290,
  3292,  3295,  3296,  3304,  3308,  3309,  3314,  3320,  3326,  3327,
  3330,  3331,  3332,  3339,  3341,  3343,  3345,  3347,  3349,  3351,
  3352,  3354,  3356,  3358,  3360,  3362,  3364,  3369,  3372,  3377,
  3382,  3385,  3388,  3389,  3391,  3393,  3396,  3398,  3401,  3403,
  3406,  3408,  3410,  3412,  3414,  3417,  3419,  3421,  3425,  3430,
  3431,  3434,  3435,  3437,  3441,  3444,  3446,  3448,  3450,  3451,
  3453,  3455,  3459,  3460,  3465,  3467,  3469,  3472,  3476,  3477,
  3480,  3482,  3486,  3491,  3494,  3498,  3505,  3509,  3513,  3518,
  3523,  3524,  3528,  3532,  3537,  3542,  3543,  3545,  3546,  3548,
  3550,  3552,  3554,  3557,  3559,  3562,  3565,  3567,  3570,  3573,
  3576,  3577,  3583,  3584,  3590,  3592,  3594,  3595,  3596,  3599,
  3600,  3605,  3607,  3611,  3615,  3622,  3626,  3631,  3635,  3637,
  3639,  3641,  3644,  3648,  3654,  3657,  3663,  3666,  3668,  3670,
  3672,  3675,  3679,  3683,  3687,  3691,  3695,  3699,  3703,  3706,
  3709,  3713,  3720,  3724,  3728,  3732,  3737,  3740,  3743,  3748,
  3752,  3757,  3759,  3761,  3766,  3768,  3773,  3775,  3780,  3785,
  3790,  3795,  3801,  3807,  3813,  3818,  3821,  3825,  3828,  3833,
  3837,  3842,  3846,  3851,  3857,  3864,  3870,  3877,  3883,  3889,
  3895,  3901,  3907,  3913,  3919,  3925,  3932,  3939,  3946,  3953,
  3960,  3967,  3974,  3981,  3988,  3995,  4002,  4009,  4016,  4023,
  4030,  4037,  4041,  4045,  4048,  4050,  4052,  4056,  4058,  4059,
  4062,  4064,  4067,  4070,  4073,  4075,  4077,  4078,  4080,  4083,
  4086,  4088,  4090,  4092,  4094,  4096,  4099,  4101,  4103,  4105,
  4107,  4109,  4111,  4113,  4115,  4117,  4119,  4121,  4123,  4125,
  4127,  4129,  4131,  4133,  4135,  4137,  4139,  4141,  4143,  4145,
  4147,  4149,  4151,  4153,  4155,  4157,  4159,  4161,  4163,  4165,
  4167,  4169,  4171,  4173,  4175,  4177,  4179,  4181,  4185,  4187
};

static const short yyrhs[] = {   304,
     0,     0,   304,   305,     0,   648,   306,   307,    27,     0,
   648,   307,    27,     0,   593,     0,   660,     0,   658,     0,
   664,     0,   665,     0,     3,   579,     0,   322,     0,   309,
     0,   324,     0,   325,     0,   331,     0,   354,     0,   358,
     0,   364,     0,   367,     0,   308,     0,   447,     0,   377,
     0,   385,     0,   366,     0,   376,     0,   310,     0,   406,
     0,   453,     0,   386,     0,   390,     0,   397,     0,   435,
     0,   436,     0,   461,     0,   407,     0,   415,     0,   418,
     0,   417,     0,   413,     0,   422,     0,   396,     0,   454,
     0,   425,     0,   437,     0,   439,     0,   440,     0,   441,
     0,   446,     0,   448,     0,   317,     0,   320,     0,   321,
     0,   578,     0,   591,     0,   592,     0,   616,     0,   617,
     0,   620,     0,   623,     0,   624,     0,   627,     0,   628,
     0,   629,     0,   630,     0,   643,     0,   644,     0,    85,
   190,   573,   311,   312,   313,   315,   316,     0,    64,   190,
   573,   311,   312,   313,   315,   316,     0,   101,   190,   573,
     0,   198,   252,   573,     0,     0,   214,     0,   243,     0,
     0,   215,     0,   244,     0,     0,   314,   298,   573,     0,
   573,     0,   119,   116,   314,     0,     0,   271,   269,   572,
     0,     0,   173,   575,   182,   318,     0,   173,   575,   284,
   318,     0,   173,   178,   201,   319,     0,   173,   184,   127,
   133,   164,   575,     0,   173,   184,   127,   133,   575,     0,
   173,   139,   445,     0,   572,     0,    96,     0,   572,     0,
    96,     0,   135,     0,   263,   575,     0,   263,   178,   201,
     0,   263,   184,   127,   133,     0,   256,   575,     0,   256,
   178,   201,     0,   256,   184,   127,   133,     0,    64,   175,
   559,   483,   323,     0,    62,   424,   335,     0,    62,   299,
   333,   300,     0,   101,   424,   575,     0,    64,   424,   575,
   173,    96,   342,     0,    64,   424,   575,   101,    96,     0,
    62,   344,     0,    79,   558,     0,   213,   328,   559,   329,
   326,   327,   330,     0,   182,     0,   113,     0,   572,     0,
   266,     0,   267,     0,   210,     0,     0,   198,   250,     0,
     0,   191,   218,   572,     0,     0,    85,   332,   175,   559,
   299,   333,   300,   353,     0,   176,     0,     0,   333,   298,
   334,     0,   334,     0,     0,   335,     0,   343,     0,   575,
   507,   336,     0,   575,   260,   338,     0,   337,     0,     0,
   337,   339,     0,   339,     0,   159,   129,     0,     0,    84,
   565,   340,     0,   340,     0,    78,   299,   346,   300,     0,
    96,   147,     0,    96,   342,     0,   145,   147,     0,   188,
     0,   159,   129,     0,   165,   575,   457,   349,   350,     0,
   341,   298,   342,     0,   342,     0,   568,     0,   288,   342,
     0,   342,   287,   342,     0,   342,   288,   342,     0,   342,
   290,   342,     0,   342,   289,   342,     0,   342,   284,   342,
     0,   342,   285,   342,     0,   342,   286,   342,     0,   293,
   342,     0,   291,   342,     0,   342,    59,   507,     0,    75,
   299,   342,    67,   507,   300,     0,   299,   342,   300,     0,
   566,   299,   300,     0,   566,   299,   341,   300,     0,   342,
   276,   342,     0,   276,   342,     0,   342,   276,     0,    88,
     0,    89,     0,    89,   299,   570,   300,     0,    90,     0,
    90,   299,   570,   300,     0,    91,     0,   190,     0,    84,
   565,   344,     0,   344,     0,    78,   299,   346,   300,     0,
   188,   299,   458,   300,     0,   159,   129,   299,   458,   300,
     0,   112,   129,   299,   458,   300,   165,   575,   457,   349,
   350,     0,   345,   298,   346,     0,   346,     0,   568,     0,
   147,     0,   575,     0,   288,   346,     0,   346,   287,   346,
     0,   346,   288,   346,     0,   346,   290,   346,     0,   346,
   289,   346,     0,   346,   284,   346,     0,   346,   285,   346,
     0,   346,   286,   346,     0,   293,   346,     0,   291,   346,
     0,   346,    59,   507,     0,    75,   299,   346,    67,   507,
   300,     0,   299,   346,   300,     0,   566,   299,   300,     0,
   566,   299,   345,   300,     0,   346,   276,   346,     0,   346,
   134,   346,     0,   346,   145,   134,   346,     0,   346,    65,
   346,     0,   346,   153,   346,     0,   145,   346,     0,   276,
   346,     0,   346,   276,     0,   346,   231,     0,   346,   126,
   147,     0,   346,   248,     0,   346,   126,   145,   147,     0,
   346,   126,   186,     0,   346,   126,   108,     0,   346,   126,
   145,   186,     0,   346,   126,   145,   108,     0,   346,   119,
   299,   347,   300,     0,   346,   145,   119,   299,   347,   300,
     0,   346,    70,   348,    65,   348,     0,   346,   145,    70,
   348,    65,   348,     0,   347,   298,   348,     0,   348,     0,
   568,     0,   136,   114,     0,   136,   156,     0,     0,   351,
   351,     0,   351,     0,     0,   150,    97,   352,     0,   150,
   189,   352,     0,   144,    61,     0,    73,     0,   173,    96,
     0,   173,   147,     0,   229,   299,   484,   300,     0,     0,
    85,   332,   175,   559,   355,    67,   471,     0,   299,   356,
   300,     0,     0,   356,   298,   357,     0,   357,     0,   575,
     0,    85,   261,   559,   359,     0,   359,   360,     0,     0,
   211,   363,     0,   216,     0,   227,   363,     0,   239,   363,
     0,   240,   363,     0,   264,   363,     0,   362,     0,   363,
     0,   571,     0,   288,   571,     0,   570,     0,   288,   570,
     0,    85,   365,   253,   130,   572,   226,   380,   232,   572,
     0,   268,     0,     0,   101,   253,   130,   572,     0,    85,
   202,   565,   368,   369,   150,   559,   371,   105,   162,   565,
   299,   374,   300,     0,   209,     0,   205,     0,   370,     0,
   370,   153,   370,     0,   370,   153,   370,   153,   370,     0,
   122,     0,    97,     0,   189,     0,   111,   372,   373,     0,
   220,     0,     0,   258,     0,   265,     0,   375,     0,   374,
   298,   375,     0,     0,   570,     0,   571,     0,   572,     0,
   656,     0,   101,   202,   565,   150,   559,     0,    85,   379,
   378,     0,   380,   381,     0,   251,     0,   203,     0,   206,
     0,   162,     0,   128,     0,   575,     0,   420,     0,   276,
     0,   299,   382,   300,     0,   383,     0,   382,   298,   383,
     0,   380,   284,   384,     0,   380,     0,    96,   284,   384,
     0,   575,     0,   419,     0,   361,     0,   572,     0,   262,
   575,     0,   101,   175,   484,     0,   101,   261,   484,     0,
   109,   387,   388,   389,   125,   647,     0,   241,   387,   388,
   389,     0,   224,     0,   208,     0,   166,     0,    60,     0,
     0,   570,     0,   288,   570,     0,    63,     0,   143,     0,
   160,     0,     0,   119,   565,     0,   113,   565,     0,     0,
   115,   391,   150,   484,   182,   394,   395,     0,    63,   161,
     0,    63,     0,   392,     0,   393,     0,   392,   298,   393,
     0,   172,     0,   122,     0,   189,     0,    97,     0,   259,
     0,   163,     0,   116,   575,     0,   575,     0,   198,   115,
   152,     0,     0,   167,   391,   150,   484,   113,   394,     0,
    85,   398,   228,   564,   150,   559,   399,   299,   400,   300,
   408,     0,   188,     0,     0,   191,   561,     0,     0,   401,
     0,   402,     0,   401,   298,   403,     0,   403,     0,   566,
   299,   485,   300,   404,   405,     0,   562,   404,   405,     0,
   292,   507,     0,   111,   507,     0,     0,   563,     0,   191,
   563,     0,     0,   223,   228,   564,   503,     0,    85,   225,
   566,   409,   257,   411,   408,    67,   572,   130,   572,     0,
   198,   381,     0,     0,   299,   410,   300,     0,   299,   300,
     0,   574,     0,   410,   298,   574,     0,   412,   574,     0,
   262,     0,     0,   101,   414,   565,     0,   203,     0,   228,
     0,   259,     0,   195,     0,   101,   206,   565,   416,     0,
   565,     0,   289,     0,   101,   225,   566,   409,     0,   101,
   251,   419,   299,   421,   300,     0,   276,     0,   420,     0,
   287,     0,   288,     0,   289,     0,   290,     0,   285,     0,
   286,     0,   284,     0,   565,     0,   565,   298,   565,     0,
   245,   298,   565,     0,   565,   298,   245,     0,    64,   175,
   559,   483,   255,   424,   423,   182,   565,     0,   565,     0,
     0,    82,     0,     0,     0,    85,   259,   565,    67,   426,
   150,   432,   182,   431,   503,   219,   433,   427,     0,   246,
     0,   469,     0,   430,     0,   296,   428,   297,     0,   299,
   428,   300,     0,   429,     0,   430,     0,   429,   430,     0,
   429,   430,   293,     0,   430,   293,     0,   455,     0,   463,
     0,   460,     0,   434,     0,   559,   295,   562,     0,   559,
     0,   172,     0,   189,     0,    97,     0,   122,     0,   230,
     0,     0,   247,   559,     0,   234,   559,     0,   235,   559,
     0,   235,   289,     0,   204,   438,     0,    69,   438,     0,
    83,   438,     0,   103,   438,     0,   169,   438,     0,   199,
     0,   184,     0,     0,    85,   195,   565,    67,   469,     0,
   236,   567,     0,    85,   217,   560,   198,   442,   443,     0,
    85,   217,   560,     0,   237,   284,   444,     0,     0,   221,
   284,   445,     0,     0,   572,     0,    96,     0,     0,   572,
     0,    96,     0,     0,   101,   217,   560,     0,   212,   564,
   150,   559,     0,   270,   449,   450,     0,   270,   449,   450,
   559,   451,     0,   272,     0,     0,   207,     0,     0,   299,
   452,   300,     0,     0,   565,     0,   452,   298,   565,     0,
   222,   449,   454,     0,   469,     0,   464,     0,   463,     0,
   455,     0,   434,     0,   460,     0,   122,   125,   559,   456,
     0,   192,   299,   556,   300,     0,    96,   192,     0,   469,
     0,   299,   458,   300,   192,   299,   556,   300,     0,   299,
   458,   300,   469,     0,   299,   458,   300,     0,     0,   458,
   298,   459,     0,   459,     0,   575,   533,     0,    97,   113,
   559,   503,     0,   238,   473,   559,     0,   238,   473,   559,
   119,   462,   258,   274,   274,     0,   238,   473,   559,   119,
   274,   274,   274,     0,   238,   473,   559,   119,   274,   274,
     0,   274,     0,     0,   189,   559,   173,   554,   490,   503,
     0,    95,   565,   465,    92,   111,   469,   466,     0,   210,
     0,   121,     0,   170,     0,   121,   170,     0,     0,   111,
   467,     0,     0,   164,   151,     0,   189,   468,     0,   149,
   458,     0,   470,   476,   488,   480,     0,   299,   470,   300,
     0,   471,     0,   470,   104,   470,     0,   470,   187,   474,
   470,     0,   470,   123,   474,   470,     0,   172,   475,   556,
   472,   490,   503,   486,   487,     0,   125,   332,   473,   559,
     0,   125,   647,     0,     0,   175,     0,     0,    63,     0,
     0,    99,     0,    99,   150,   575,     0,    63,     0,     0,
   154,    72,   477,     0,     0,   478,     0,   477,   298,   478,
     0,   531,   479,     0,   191,   276,     0,   191,   285,     0,
   191,   286,     0,    68,     0,    98,     0,     0,   233,   481,
   298,   482,     0,   233,   481,   249,   482,     0,   233,   481,
     0,   249,   482,   233,   481,     0,   249,   482,     0,     0,
   570,     0,    63,     0,   281,     0,   570,     0,   281,     0,
   289,     0,     0,   485,     0,   565,     0,   485,   298,   565,
     0,   116,    72,   534,     0,     0,   117,   531,     0,     0,
   111,   189,   489,     0,     0,   149,   452,     0,     0,   113,
   491,     0,     0,   299,   494,   300,     0,   495,     0,   492,
     0,   492,   298,   493,     0,   493,     0,   504,    67,   576,
     0,   504,   575,     0,   504,     0,   495,     0,   493,   187,
   128,   493,     0,   493,   496,     0,   496,   497,     0,   497,
     0,   498,   128,   493,   500,     0,   141,   498,   128,   493,
     0,    86,   128,   493,     0,   114,   499,     0,   132,   499,
     0,   168,   499,     0,   155,     0,   120,     0,     0,   155,
     0,     0,   191,   299,   501,   300,     0,   150,   531,     0,
   501,   298,   502,     0,   502,     0,   575,     0,   197,   531,
     0,     0,   559,     0,   559,   289,     0,   296,   297,   506,
     0,   296,   570,   297,   506,     0,     0,   296,   297,   506,
     0,   296,   570,   297,   506,     0,     0,   508,   505,     0,
   516,     0,   262,   508,     0,   509,     0,   521,     0,   511,
     0,   510,     0,   656,     0,   203,     0,     3,     0,     4,
     0,     5,     0,     6,     0,     7,     0,     8,     0,     9,
     0,    10,     0,    11,     0,    13,     0,    15,     0,    16,
     0,    17,     0,    18,     0,    19,     0,    20,     0,    21,
     0,    22,     0,    23,     0,    24,     0,    26,     0,    28,
     0,    29,     0,    30,     0,    31,     0,    32,     0,    34,
     0,    35,     0,    36,     0,    37,     0,    38,     0,   110,
   513,     0,   100,   158,     0,    94,   515,     0,   148,   514,
     0,   110,     0,   100,   158,     0,    94,     0,   148,     0,
   299,   570,   300,     0,     0,   299,   570,   298,   570,   300,
     0,   299,   570,   300,     0,     0,   299,   570,   298,   570,
   300,     0,   299,   570,   300,     0,     0,   517,   299,   570,
   300,     0,   517,     0,    77,   518,   519,   520,     0,    76,
   518,     0,   193,     0,   140,    77,   518,     0,   142,   518,
     0,   194,     0,     0,    77,   173,   575,     0,     0,    81,
   575,     0,     0,   522,     0,   179,   523,     0,   178,     0,
   124,   524,     0,   200,     0,   138,     0,    93,     0,   118,
     0,   137,     0,   171,     0,   198,   178,   201,     0,     0,
   522,     0,   200,   182,   138,     0,    93,   182,   118,     0,
    93,   182,   137,     0,    93,   182,   171,     0,   118,   182,
   137,     0,   137,   182,   171,     0,   118,   182,   171,     0,
     0,   531,     0,   147,     0,   299,   527,   300,   119,   299,
   471,   300,     0,   299,   527,   300,   145,   119,   299,   471,
   300,     0,   299,   527,   300,   528,   529,   299,   471,   300,
     0,   299,   527,   300,   528,   299,   471,   300,     0,   299,
   527,   300,   528,   299,   527,   300,     0,   530,   298,   531,
     0,   276,     0,   285,     0,   284,     0,   286,     0,   287,
     0,   288,     0,   289,     0,   290,     0,    66,     0,    63,
     0,   530,   298,   531,     0,   531,     0,   552,   533,     0,
   526,     0,   568,     0,   575,     0,   288,   531,     0,   531,
   287,   531,     0,   531,   288,   531,     0,   531,   290,   531,
     0,   531,   289,   531,     0,   531,   285,   531,     0,   531,
   286,   531,     0,   531,   284,   147,     0,   531,   284,   531,
     0,   293,   531,     0,   291,   531,     0,   531,    59,   507,
     0,    75,   299,   531,    67,   507,   300,     0,   299,   525,
   300,     0,   531,   276,   531,     0,   531,   134,   531,     0,
   531,   145,   134,   531,     0,   276,   531,     0,   531,   276,
     0,   566,   299,   289,   300,     0,   566,   299,   300,     0,
   566,   299,   534,   300,     0,    88,     0,    89,     0,    89,
   299,   570,   300,     0,    90,     0,    90,   299,   570,   300,
     0,    91,     0,   190,     0,   106,   299,   471,   300,     0,
   107,   299,   535,   300,     0,   157,   299,   537,   300,     0,
   174,   299,   539,   300,     0,   185,   299,    71,   542,   300,
     0,   185,   299,   131,   542,   300,     0,   185,   299,   183,
   542,   300,     0,   185,   299,   542,   300,     0,   531,   231,
     0,   531,   126,   147,     0,   531,   248,     0,   531,   126,
   145,   147,     0,   531,   126,   186,     0,   531,   126,   145,
   108,     0,   531,   126,   108,     0,   531,   126,   145,   186,
     0,   531,    70,   532,    65,   532,     0,   531,   145,    70,
   532,    65,   532,     0,   531,   119,   299,   543,   300,     0,
   531,   145,   119,   299,   545,   300,     0,   531,   276,   299,
   471,   300,     0,   531,   287,   299,   471,   300,     0,   531,
   288,   299,   471,   300,     0,   531,   290,   299,   471,   300,
     0,   531,   289,   299,   471,   300,     0,   531,   285,   299,
   471,   300,     0,   531,   286,   299,   471,   300,     0,   531,
   284,   299,   471,   300,     0,   531,   276,    66,   299,   471,
   300,     0,   531,   287,    66,   299,   471,   300,     0,   531,
   288,    66,   299,   471,   300,     0,   531,   290,    66,   299,
   471,   300,     0,   531,   289,    66,   299,   471,   300,     0,
   531,   285,    66,   299,   471,   300,     0,   531,   286,    66,
   299,   471,   300,     0,   531,   284,    66,   299,   471,   300,
     0,   531,   276,    63,   299,   471,   300,     0,   531,   287,
    63,   299,   471,   300,     0,   531,   288,    63,   299,   471,
   300,     0,   531,   290,    63,   299,   471,   300,     0,   531,
   289,    63,   299,   471,   300,     0,   531,   285,    63,   299,
   471,   300,     0,   531,   286,    63,   299,   471,   300,     0,
   531,   284,    63,   299,   471,   300,     0,   531,    65,   531,
     0,   531,   153,   531,     0,   145,   531,     0,   547,     0,
   652,     0,   552,   533,     0,   568,     0,   575,     0,   288,
   532,     0,   532,   287,   532,     0,   532,   288,   532,     0,
   532,   290,   532,     0,   532,   289,   532,     0,   293,   532,
     0,   291,   532,     0,   532,    59,   507,     0,    75,   299,
   532,    67,   507,   300,     0,   299,   531,   300,     0,   532,
   276,   532,     0,   276,   532,     0,   532,   276,     0,   566,
   299,   300,     0,   566,   299,   534,   300,     0,    88,     0,
    89,     0,    89,   299,   570,   300,     0,    90,     0,    90,
   299,   570,   300,     0,    91,     0,   190,     0,   157,   299,
   537,   300,     0,   174,   299,   539,   300,     0,   185,   299,
    71,   542,   300,     0,   185,   299,   131,   542,   300,     0,
   185,   299,   183,   542,   300,     0,   185,   299,   542,   300,
     0,   653,     0,   296,   646,   297,   533,     0,   296,   646,
   292,   646,   297,   533,     0,     0,   525,     0,   534,   298,
   525,     0,   534,   191,   531,     0,   536,   113,   531,     0,
     0,   652,     0,   522,     0,   180,     0,   181,     0,   538,
   119,   538,     0,     0,   552,   533,     0,   568,     0,   288,
   538,     0,   538,   287,   538,     0,   538,   288,   538,     0,
   538,   290,   538,     0,   538,   289,   538,     0,   291,   538,
     0,   538,    59,   507,     0,    75,   299,   538,    67,   507,
   300,     0,   299,   538,   300,     0,   538,   276,   538,     0,
   276,   538,     0,   538,   276,     0,   575,     0,   566,   299,
   300,     0,   566,   299,   534,   300,     0,   157,   299,   537,
   300,     0,   174,   299,   539,   300,     0,   185,   299,    71,
   542,   300,     0,   185,   299,   131,   542,   300,     0,   185,
   299,   183,   542,   300,     0,   185,   299,   542,   300,     0,
   534,   540,   541,     0,     0,   113,   534,     0,     0,   111,
   534,     0,     0,   531,   113,   534,     0,   113,   534,     0,
   534,     0,   471,     0,   544,     0,   568,     0,   544,   298,
   568,     0,   471,     0,   546,     0,   568,     0,   546,   298,
   568,     0,    74,   551,   548,   550,   103,     0,   146,   299,
   531,   298,   531,   300,     0,    80,   299,   534,   300,     0,
   548,   549,     0,   549,     0,   196,   531,   177,   525,     0,
   102,   525,     0,     0,   552,   533,     0,   575,     0,     0,
   559,   295,   553,     0,   569,   295,   553,     0,   562,     0,
   553,   295,   562,     0,   553,   295,   289,     0,   554,   298,
   555,     0,   555,     0,   289,     0,   575,   533,   284,   525,
     0,   552,   533,     0,   559,   295,   289,     0,   556,   298,
   557,     0,   557,     0,   525,    67,   576,     0,   525,     0,
   559,   295,   289,     0,   289,     0,   575,     0,     0,   577,
     0,   575,     0,   575,     0,   656,     0,   575,     0,   656,
     0,   575,     0,   575,     0,   575,     0,   572,     0,   570,
     0,   571,     0,   572,     0,   507,   572,     0,   569,     0,
   186,     0,   108,     0,   281,   533,     0,   280,     0,   282,
     0,   275,     0,   656,     0,   575,     0,   512,     0,   517,
     0,   656,     0,   522,     0,    60,     0,    61,     0,   205,
     0,   206,     0,   208,     0,   209,     0,   211,     0,   214,
     0,   215,     0,   216,     0,   217,     0,   218,     0,   100,
     0,   220,     0,   221,     0,   224,     0,   225,     0,   226,
     0,   227,     0,   228,     0,   229,     0,   121,     0,   230,
     0,   231,     0,   129,     0,   130,     0,   232,     0,   237,
     0,   136,     0,   239,     0,   240,     0,   143,     0,   243,
     0,   244,     0,   246,     0,   248,     0,   149,     0,   250,
     0,   151,     0,   251,     0,   152,     0,   252,     0,   160,
     0,   161,     0,   253,     0,   164,     0,   166,     0,   255,
     0,   257,     0,   258,     0,   259,     0,   170,     0,   261,
     0,   260,     0,   264,     0,   265,     0,   266,     0,   267,
     0,   178,     0,   179,     0,   180,     0,   181,     0,   202,
     0,   268,     0,   203,     0,   271,     0,   273,     0,   201,
     0,     3,     0,     4,     0,     5,     0,     6,     0,     7,
     0,     8,     0,     9,     0,    10,     0,    11,     0,    13,
     0,    15,     0,    16,     0,    17,     0,    18,     0,    19,
     0,    20,     0,    21,     0,    22,     0,    23,     0,    24,
     0,    26,     0,    28,     0,    29,     0,    30,     0,    31,
     0,    32,     0,    34,     0,    35,     0,    36,     0,    37,
     0,    38,     0,   575,     0,   204,     0,   207,     0,   210,
     0,    74,     0,   212,     0,    80,     0,    84,     0,   213,
     0,    87,     0,   219,     0,   102,     0,   103,     0,   222,
     0,   223,     0,   108,     0,   112,     0,   116,     0,   234,
     0,   236,     0,   238,     0,   241,     0,   242,     0,   245,
     0,   146,     0,   154,     0,   157,     0,   158,     0,   256,
     0,   262,     0,   263,     0,   175,     0,   177,     0,   184,
     0,   186,     0,   270,     0,   272,     0,   196,     0,    87,
     0,   242,     0,     7,   182,   579,   585,   586,     0,     7,
   182,    96,     0,     7,   587,     0,   560,   582,   584,     0,
   580,   581,   584,   290,   560,   590,     0,   589,     0,   572,
     0,   656,   654,     0,   276,   583,     0,   581,     0,     0,
   575,     0,   575,   295,   583,     0,   292,   570,     0,     0,
    67,   579,     0,     0,   190,   587,     0,     0,   588,     0,
   588,   290,   575,     0,   588,    17,    72,   588,     0,   588,
   191,   588,     0,   573,     0,   589,     0,   275,     0,   654,
     0,   276,   575,     0,     0,    95,   565,   465,    92,   111,
   656,   466,     0,    10,    23,   656,     0,     0,   595,   594,
   597,   596,     0,   648,    69,    95,    26,    27,     0,   648,
   103,    95,    26,    27,     0,     0,   598,   597,     0,     0,
     0,   601,   599,   602,   600,   612,   293,     0,    46,     0,
    54,     0,    53,     0,    43,     0,    51,     0,    40,     0,
     0,   610,     0,   611,     0,   605,     0,   606,     0,   603,
     0,   657,     0,   604,   301,   659,   302,     0,    45,   609,
     0,   607,   301,   597,   302,     0,   608,   301,   597,   302,
     0,    55,   609,     0,    56,   609,     0,     0,   657,     0,
    52,     0,    57,    52,     0,    48,     0,    57,    48,     0,
    50,     0,    57,    50,     0,    47,     0,    44,     0,    41,
     0,    42,     0,    57,    42,     0,    58,     0,   613,     0,
   612,   298,   613,     0,   615,   657,   505,   614,     0,     0,
   284,   650,     0,     0,   289,     0,    95,   265,   656,     0,
    11,   618,     0,   619,     0,    87,     0,    63,     0,     0,
   579,     0,    96,     0,   105,    18,   622,     0,     0,   105,
   656,   621,   625,     0,   589,     0,   277,     0,    14,   656,
     0,    22,   565,   625,     0,     0,   191,   626,     0,   652,
     0,   652,   298,   626,     0,    23,   656,   113,   589,     0,
   437,    24,     0,   173,     8,   619,     0,   203,   657,   126,
   634,   631,   633,     0,   296,   297,   632,     0,   299,   300,
   632,     0,   296,   570,   297,   632,     0,   299,   570,   300,
   632,     0,     0,   296,   297,   632,     0,   299,   300,   632,
     0,   296,   570,   297,   632,     0,   299,   570,   300,   632,
     0,     0,    25,     0,     0,    76,     0,   193,     0,   110,
     0,   100,     0,   637,    20,     0,    12,     0,   637,    28,
     0,   637,    21,     0,     4,     0,    36,    20,     0,    36,
    28,     0,    36,    21,     0,     0,    35,   635,   301,   638,
   302,     0,     0,   187,   636,   301,   638,   302,     0,   657,
     0,    29,     0,     0,     0,   639,   638,     0,     0,   634,
   640,   641,    27,     0,   642,     0,   641,   298,   642,     0,
   615,   657,   505,     0,    37,   657,   126,   634,   631,   633,
     0,    38,    30,   645,     0,    38,   145,    13,   645,     0,
    38,    32,   645,     0,     9,     0,    31,     0,    34,     0,
    16,   565,     0,    15,   182,   565,     0,   219,   565,   299,
   649,   300,     0,   219,     5,     0,     6,   565,   299,   649,
   300,     0,   552,   533,     0,   526,     0,   568,     0,   575,
     0,   288,   646,     0,   531,   287,   646,     0,   531,   288,
   646,     0,   531,   290,   646,     0,   531,   289,   646,     0,
   531,   285,   646,     0,   531,   286,   646,     0,   531,   284,
   646,     0,   293,   646,     0,   291,   646,     0,   531,    59,
   507,     0,    75,   299,   531,    67,   507,   300,     0,   299,
   525,   300,     0,   531,   276,   646,     0,   531,   134,   646,
     0,   531,   145,   134,   646,     0,   276,   646,     0,   531,
   276,     0,   566,   299,   289,   300,     0,   566,   299,   300,
     0,   566,   299,   534,   300,     0,    88,     0,    89,     0,
    89,   299,   570,   300,     0,    90,     0,    90,   299,   570,
   300,     0,    91,     0,   106,   299,   471,   300,     0,   107,
   299,   535,   300,     0,   157,   299,   537,   300,     0,   174,
   299,   539,   300,     0,   185,   299,    71,   542,   300,     0,
   185,   299,   131,   542,   300,     0,   185,   299,   183,   542,
   300,     0,   185,   299,   542,   300,     0,   531,   231,     0,
   531,   126,   147,     0,   531,   248,     0,   531,   126,   145,
   147,     0,   531,   126,   186,     0,   531,   126,   145,   108,
     0,   531,   126,   108,     0,   531,   126,   145,   186,     0,
   531,    70,   532,    65,   532,     0,   531,   145,    70,   532,
    65,   532,     0,   531,   119,   299,   543,   300,     0,   531,
   145,   119,   299,   545,   300,     0,   531,   276,   299,   471,
   300,     0,   531,   287,   299,   471,   300,     0,   531,   288,
   299,   471,   300,     0,   531,   290,   299,   471,   300,     0,
   531,   289,   299,   471,   300,     0,   531,   285,   299,   471,
   300,     0,   531,   286,   299,   471,   300,     0,   531,   284,
   299,   471,   300,     0,   531,   276,    66,   299,   471,   300,
     0,   531,   287,    66,   299,   471,   300,     0,   531,   288,
    66,   299,   471,   300,     0,   531,   290,    66,   299,   471,
   300,     0,   531,   289,    66,   299,   471,   300,     0,   531,
   285,    66,   299,   471,   300,     0,   531,   286,    66,   299,
   471,   300,     0,   531,   284,    66,   299,   471,   300,     0,
   531,   276,    63,   299,   471,   300,     0,   531,   287,    63,
   299,   471,   300,     0,   531,   288,    63,   299,   471,   300,
     0,   531,   290,    63,   299,   471,   300,     0,   531,   289,
    63,   299,   471,   300,     0,   531,   285,    63,   299,   471,
   300,     0,   531,   286,    63,   299,   471,   300,     0,   531,
   284,    63,   299,   471,   300,     0,   531,    65,   646,     0,
   531,   153,   646,     0,   145,   646,     0,   653,     0,   651,
     0,   647,   298,   651,     0,    33,     0,     0,   649,   662,
     0,   663,     0,   650,   663,     0,   654,   655,     0,   654,
   655,     0,   654,     0,   278,     0,     0,   654,     0,    19,
   654,     0,    19,   565,     0,   274,     0,   277,     0,   274,
     0,   279,     0,   661,     0,   659,   661,     0,   661,     0,
   293,     0,   274,     0,   277,     0,   570,     0,   571,     0,
   289,     0,    40,     0,    41,     0,    42,     0,    43,     0,
    44,     0,    45,     0,    46,     0,    47,     0,    48,     0,
    50,     0,    51,     0,    52,     0,    53,     0,    54,     0,
    55,     0,    56,     0,    57,     0,    58,     0,    39,     0,
   296,     0,   297,     0,   299,     0,   300,     0,   284,     0,
   298,     0,   274,     0,   277,     0,   570,     0,   571,     0,
   298,     0,   274,     0,   277,     0,   570,     0,   571,     0,
   301,   659,   302,     0,   301,     0,   302,     0
};

#endif

#if YYDEBUG != 0
static const short yyrline[] = { 0,
   837,   839,   840,   842,   843,   844,   845,   846,   847,   848,
   850,   852,   853,   854,   855,   856,   857,   858,   859,   860,
   861,   862,   863,   864,   865,   866,   867,   868,   869,   870,
   871,   872,   873,   874,   875,   876,   877,   878,   879,   880,
   881,   882,   883,   892,   893,   898,   899,   900,   901,   902,
   903,   904,   905,   906,   915,   919,   927,   931,   939,   942,
   947,   972,   980,   981,   989,   996,  1003,  1025,  1039,  1053,
  1059,  1060,  1063,  1067,  1071,  1074,  1078,  1082,  1085,  1089,
  1095,  1096,  1099,  1100,  1112,  1116,  1120,  1124,  1134,  1144,
  1154,  1155,  1158,  1159,  1160,  1163,  1167,  1171,  1177,  1181,
  1185,  1199,  1205,  1209,  1213,  1215,  1217,  1219,  1230,  1245,
  1251,  1253,  1262,  1263,  1264,  1267,  1268,  1271,  1272,  1278,
  1279,  1291,  1298,  1299,  1302,  1306,  1310,  1313,  1314,  1317,
  1321,  1327,  1328,  1331,  1332,  1335,  1339,  1345,  1350,  1364,
  1368,  1372,  1376,  1380,  1384,  1388,  1395,  1399,  1413,  1415,
  1417,  1419,  1421,  1423,  1425,  1427,  1429,  1435,  1437,  1439,
  1441,  1445,  1447,  1449,  1451,  1457,  1459,  1462,  1464,  1466,
  1472,  1474,  1480,  1482,  1490,  1494,  1498,  1502,  1506,  1510,
  1517,  1521,  1527,  1529,  1531,  1535,  1537,  1539,  1541,  1543,
  1545,  1547,  1549,  1555,  1557,  1559,  1563,  1567,  1569,  1573,
  1577,  1579,  1581,  1583,  1585,  1587,  1589,  1591,  1593,  1595,
  1597,  1599,  1601,  1603,  1605,  1607,  1609,  1611,  1613,  1615,
  1618,  1622,  1627,  1632,  1633,  1634,  1637,  1638,  1639,  1642,
  1643,  1646,  1647,  1648,  1649,  1652,  1653,  1656,  1662,  1663,
  1666,  1667,  1670,  1680,  1686,  1688,  1691,  1695,  1699,  1703,
  1707,  1711,  1717,  1718,  1720,  1724,  1731,  1735,  1749,  1756,
  1757,  1759,  1773,  1781,  1782,  1785,  1789,  1793,  1799,  1800,
  1801,  1804,  1810,  1811,  1814,  1815,  1818,  1820,  1822,  1826,
  1830,  1834,  1835,  1838,  1851,  1857,  1863,  1864,  1865,  1868,
  1869,  1870,  1871,  1872,  1875,  1878,  1879,  1882,  1885,  1889,
  1895,  1896,  1897,  1898,  1899,  1912,  1916,  1933,  1940,  1946,
  1947,  1948,  1949,  1954,  1957,  1958,  1959,  1960,  1961,  1962,
  1965,  1966,  1968,  1979,  1985,  1989,  1993,  1999,  2003,  2009,
  2013,  2017,  2021,  2025,  2031,  2035,  2039,  2045,  2049,  2060,
  2078,  2087,  2088,  2091,  2092,  2095,  2096,  2099,  2100,  2103,
  2109,  2115,  2116,  2117,  2126,  2127,  2128,  2138,  2174,  2180,
  2181,  2184,  2185,  2188,  2189,  2193,  2199,  2200,  2221,  2227,
  2228,  2229,  2230,  2234,  2240,  2241,  2245,  2252,  2258,  2258,
  2260,  2261,  2262,  2263,  2264,  2265,  2266,  2269,  2273,  2275,
  2277,  2290,  2297,  2298,  2301,  2302,  2315,  2317,  2324,  2325,
  2326,  2327,  2328,  2331,  2332,  2335,  2337,  2339,  2343,  2344,
  2345,  2346,  2349,  2353,  2360,  2361,  2362,  2363,  2366,  2367,
  2379,  2385,  2391,  2395,  2413,  2414,  2415,  2416,  2417,  2419,
  2420,  2421,  2431,  2445,  2459,  2469,  2475,  2476,  2479,  2480,
  2483,  2484,  2485,  2488,  2489,  2490,  2500,  2514,  2528,  2532,
  2540,  2541,  2544,  2545,  2548,  2549,  2552,  2554,  2566,  2584,
  2585,  2586,  2587,  2588,  2589,  2606,  2612,  2616,  2620,  2624,
  2628,  2634,  2635,  2638,  2641,  2645,  2659,  2666,  2670,  2701,
  2721,  2738,  2739,  2752,  2768,  2799,  2800,  2801,  2802,  2803,
  2806,  2807,  2811,  2812,  2818,  2834,  2851,  2855,  2859,  2864,
  2869,  2877,  2887,  2888,  2889,  2892,  2893,  2896,  2897,  2900,
  2901,  2902,  2903,  2906,  2907,  2910,  2911,  2914,  2920,  2921,
  2922,  2923,  2924,  2925,  2928,  2930,  2932,  2934,  2936,  2938,
  2942,  2943,  2944,  2947,  2948,  2958,  2959,  2962,  2964,  2966,
  2970,  2971,  2974,  2978,  2981,  2985,  2990,  2994,  3008,  3012,
  3018,  3020,  3022,  3026,  3028,  3032,  3036,  3040,  3050,  3052,
  3056,  3062,  3066,  3079,  3083,  3087,  3092,  3097,  3102,  3107,
  3112,  3116,  3122,  3123,  3134,  3135,  3138,  3139,  3142,  3148,
  3149,  3152,  3157,  3163,  3169,  3175,  3183,  3189,  3195,  3213,
  3217,  3218,  3224,  3225,  3226,  3229,  3235,  3236,  3237,  3238,
  3239,  3240,  3241,  3242,  3243,  3244,  3245,  3246,  3247,  3248,
  3249,  3250,  3251,  3252,  3253,  3254,  3255,  3256,  3257,  3258,
  3259,  3260,  3261,  3262,  3263,  3264,  3265,  3266,  3267,  3275,
  3279,  3283,  3287,  3293,  3295,  3297,  3299,  3303,  3311,  3317,
  3329,  3337,  3343,  3355,  3363,  3376,  3396,  3402,  3409,  3410,
  3411,  3412,  3415,  3416,  3419,  3420,  3423,  3424,  3427,  3431,
  3435,  3439,  3445,  3446,  3447,  3448,  3449,  3450,  3453,  3454,
  3457,  3458,  3459,  3460,  3461,  3462,  3463,  3464,  3465,  3475,
  3477,  3492,  3496,  3500,  3504,  3508,  3514,  3520,  3521,  3522,
  3523,  3524,  3525,  3526,  3527,  3530,  3531,  3535,  3539,  3554,
  3558,  3560,  3562,  3566,  3568,  3570,  3572,  3574,  3576,  3578,
  3580,  3582,  3587,  3589,  3591,  3595,  3599,  3601,  3603,  3605,
  3607,  3609,  3611,  3615,  3619,  3623,  3627,  3631,  3637,  3641,
  3647,  3651,  3656,  3660,  3664,  3668,  3673,  3677,  3681,  3685,
  3689,  3691,  3693,  3695,  3702,  3706,  3710,  3714,  3718,  3722,
  3726,  3730,  3734,  3738,  3742,  3746,  3750,  3754,  3758,  3762,
  3766,  3770,  3774,  3778,  3782,  3786,  3790,  3794,  3798,  3802,
  3806,  3810,  3814,  3818,  3822,  3826,  3830,  3832,  3834,  3836,
  3838,  3847,  3851,  3853,  3857,  3859,  3861,  3863,  3865,  3870,
  3872,  3874,  3878,  3882,  3884,  3886,  3888,  3890,  3894,  3898,
  3902,  3906,  3912,  3916,  3922,  3926,  3930,  3934,  3939,  3943,
  3947,  3951,  3955,  3959,  3963,  3967,  3971,  3973,  3975,  3979,
  3983,  3985,  3989,  3990,  3991,  3994,  3996,  4000,  4004,  4006,
  4008,  4010,  4012,  4014,  4016,  4018,  4022,  4026,  4028,  4030,
  4032,  4034,  4038,  4042,  4046,  4050,  4055,  4059,  4063,  4067,
  4073,  4077,  4081,  4083,  4089,  4091,  4095,  4097,  4099,  4103,
  4107,  4111,  4113,  4117,  4121,  4125,  4127,  4146,  4148,  4154,
  4160,  4162,  4166,  4172,  4173,  4176,  4180,  4184,  4188,  4192,
  4198,  4200,  4202,  4213,  4215,  4217,  4220,  4224,  4228,  4239,
  4241,  4246,  4250,  4254,  4258,  4264,  4265,  4268,  4272,  4285,
  4286,  4287,  4288,  4289,  4295,  4296,  4298,  4304,  4308,  4312,
  4316,  4320,  4322,  4326,  4332,  4338,  4339,  4340,  4348,  4355,
  4357,  4359,  4370,  4371,  4372,  4373,  4374,  4375,  4376,  4377,
  4378,  4379,  4380,  4381,  4382,  4383,  4384,  4385,  4386,  4387,
  4388,  4389,  4390,  4391,  4392,  4393,  4394,  4395,  4396,  4397,
  4398,  4399,  4400,  4401,  4402,  4403,  4404,  4405,  4406,  4407,
  4408,  4409,  4410,  4411,  4412,  4413,  4414,  4415,  4416,  4417,
  4419,  4420,  4421,  4422,  4423,  4424,  4425,  4426,  4427,  4428,
  4429,  4430,  4431,  4432,  4433,  4434,  4435,  4436,  4437,  4438,
  4439,  4440,  4441,  4442,  4443,  4444,  4445,  4446,  4447,  4448,
  4449,  4450,  4451,  4452,  4453,  4454,  4455,  4456,  4457,  4458,
  4459,  4460,  4461,  4462,  4463,  4464,  4465,  4466,  4467,  4468,
  4469,  4470,  4471,  4483,  4484,  4485,  4486,  4487,  4488,  4489,
  4490,  4491,  4492,  4493,  4494,  4495,  4496,  4497,  4498,  4499,
  4500,  4501,  4502,  4503,  4504,  4505,  4506,  4507,  4508,  4509,
  4510,  4511,  4512,  4513,  4514,  4515,  4516,  4517,  4518,  4519,
  4520,  4523,  4530,  4546,  4550,  4555,  4560,  4571,  4594,  4598,
  4606,  4623,  4634,  4635,  4637,  4638,  4640,  4641,  4643,  4644,
  4646,  4647,  4649,  4653,  4657,  4661,  4666,  4671,  4672,  4674,
  4698,  4711,  4717,  4760,  4765,  4770,  4777,  4779,  4781,  4785,
  4790,  4795,  4800,  4805,  4806,  4807,  4808,  4809,  4810,  4811,
  4813,  4820,  4827,  4834,  4841,  4849,  4861,  4866,  4868,  4875,
  4882,  4890,  4898,  4899,  4901,  4902,  4903,  4904,  4905,  4906,
  4907,  4908,  4909,  4910,  4911,  4913,  4915,  4919,  4924,  4999,
  5000,  5002,  5003,  5009,  5017,  5019,  5020,  5021,  5022,  5024,
  5025,  5030,  5043,  5055,  5059,  5059,  5066,  5071,  5075,  5076,
  5081,  5081,  5087,  5097,  5113,  5121,  5163,  5169,  5175,  5181,
  5187,  5195,  5201,  5207,  5213,  5219,  5226,  5227,  5229,  5236,
  5243,  5250,  5257,  5264,  5271,  5278,  5285,  5292,  5299,  5306,
  5313,  5318,  5326,  5331,  5339,  5350,  5350,  5352,  5356,  5362,
  5368,  5373,  5377,  5382,  5453,  5508,  5513,  5518,  5524,  5529,
  5534,  5539,  5544,  5549,  5554,  5559,  5566,  5570,  5572,  5574,
  5578,  5580,  5582,  5584,  5586,  5588,  5590,  5592,  5596,  5598,
  5600,  5604,  5608,  5610,  5612,  5614,  5616,  5618,  5620,  5624,
  5628,  5632,  5636,  5640,  5646,  5650,  5656,  5660,  5664,  5668,
  5672,  5677,  5681,  5685,  5689,  5693,  5695,  5697,  5699,  5706,
  5710,  5714,  5718,  5722,  5726,  5730,  5734,  5738,  5742,  5746,
  5750,  5754,  5758,  5762,  5766,  5770,  5774,  5778,  5782,  5786,
  5790,  5794,  5798,  5802,  5806,  5810,  5814,  5818,  5822,  5826,
  5830,  5834,  5836,  5838,  5840,  5844,  5844,  5846,  5848,  5849,
  5851,  5852,  5854,  5858,  5862,  5867,  5869,  5870,  5871,  5872,
  5874,  5875,  5880,  5882,  5884,  5885,  5890,  5890,  5892,  5893,
  5894,  5895,  5896,  5897,  5898,  5899,  5900,  5901,  5902,  5903,
  5904,  5905,  5906,  5907,  5908,  5909,  5910,  5911,  5912,  5913,
  5914,  5915,  5916,  5917,  5918,  5919,  5920,  5921,  5923,  5924,
  5925,  5926,  5927,  5929,  5930,  5931,  5932,  5933,  5935,  5940
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
"opt_type","opt_class","ExtendStmt","ProcedureStmt","opt_with","func_args","func_args_list",
"func_return","set_opt","RemoveStmt","remove_type","RemoveAggrStmt","aggr_argtype",
"RemoveFuncStmt","RemoveOperStmt","all_Op","MathOp","oper_argtypes","RenameStmt",
"opt_name","opt_column","RuleStmt","@1","RuleActionList","RuleActionBlock","RuleActionMulti",
"RuleActionStmt","event_object","event","opt_instead","NotifyStmt","ListenStmt",
"UnlistenStmt","TransactionStmt","opt_trans","ViewStmt","LoadStmt","CreatedbStmt",
"opt_database1","opt_database2","location","encoding","DestroydbStmt","ClusterStmt",
"VacuumStmt","opt_verbose","opt_analyze","opt_va_list","va_list","ExplainStmt",
"OptimizableStmt","InsertStmt","insert_rest","opt_column_list","columnList",
"columnElem","DeleteStmt","LockStmt","opt_lmode","UpdateStmt","CursorStmt","opt_cursor",
"cursor_clause","opt_readonly","opt_of","SelectStmt","select_clause","SubSelect",
"result","opt_table","opt_union","opt_unique","sort_clause","sortby_list","sortby",
"OptUseOp","opt_select_limit","select_limit_value","select_offset_value","opt_inh_star",
"relation_name_list","name_list","group_clause","having_clause","for_update_clause",
"update_list","from_clause","from_expr","table_list","table_expr","join_clause_with_union",
"join_clause","join_list","join_expr","join_type","join_outer","join_qual","using_list",
"using_expr","where_clause","relation_expr","opt_array_bounds","nest_array_bounds",
"Typename","Array","Generic","generic","Numeric","numeric","opt_float","opt_numeric",
"opt_decimal","Character","character","opt_varying","opt_charset","opt_collate",
"Datetime","datetime","opt_timezone","opt_interval","a_expr_or_null","row_expr",
"row_descriptor","row_op","sub_type","row_list","a_expr","b_expr","opt_indirection",
"expr_list","extract_list","extract_arg","position_list","position_expr","substr_list",
"substr_from","substr_for","trim_list","in_expr","in_expr_nodes","not_in_expr",
"not_in_expr_nodes","case_expr","when_clause_list","when_clause","case_default",
"case_arg","attr","attrs","res_target_list","res_target_el","res_target_list2",
"res_target_el2","opt_id","relation_name","database_name","access_method","attr_name",
"class","index_name","name","func_name","file_name","AexprConst","ParamNo","Iconst",
"Fconst","Sconst","UserId","TypeId","ColId","ColLabel","SpecialRuleRelation",
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
   307,   307,   307,   307,   307,   307,   307,   308,   309,   310,
   311,   311,   312,   312,   312,   313,   313,   313,   314,   314,
   315,   315,   316,   316,   317,   317,   317,   317,   317,   317,
   318,   318,   319,   319,   319,   320,   320,   320,   321,   321,
   321,   322,   323,   323,   323,   323,   323,   323,   324,   325,
   326,   326,   327,   327,   327,   328,   328,   329,   329,   330,
   330,   331,   332,   332,   333,   333,   333,   334,   334,   335,
   335,   336,   336,   337,   337,   338,   338,   339,   339,   340,
   340,   340,   340,   340,   340,   340,   341,   341,   342,   342,
   342,   342,   342,   342,   342,   342,   342,   342,   342,   342,
   342,   342,   342,   342,   342,   342,   342,   342,   342,   342,
   342,   342,   342,   342,   343,   343,   344,   344,   344,   344,
   345,   345,   346,   346,   346,   346,   346,   346,   346,   346,
   346,   346,   346,   346,   346,   346,   346,   346,   346,   346,
   346,   346,   346,   346,   346,   346,   346,   346,   346,   346,
   346,   346,   346,   346,   346,   346,   346,   346,   346,   346,
   347,   347,   348,   349,   349,   349,   350,   350,   350,   351,
   351,   352,   352,   352,   352,   353,   353,   354,   355,   355,
   356,   356,   357,   358,   359,   359,   360,   360,   360,   360,
   360,   360,   361,   361,   362,   362,   363,   363,   364,   365,
   365,   366,   367,   368,   368,   369,   369,   369,   370,   370,
   370,   371,   372,   372,   373,   373,   374,   374,   374,   375,
   375,   375,   375,   376,   377,   378,   379,   379,   379,   380,
   380,   380,   380,   380,   381,   382,   382,   383,   383,   383,
   384,   384,   384,   384,   384,   385,   385,   386,   386,   387,
   387,   387,   387,   387,   388,   388,   388,   388,   388,   388,
   389,   389,   389,   390,   391,   391,   391,   392,   392,   393,
   393,   393,   393,   393,   394,   394,   394,   395,   395,   396,
   397,   398,   398,   399,   399,   400,   400,   401,   401,   402,
   403,   404,   404,   404,   405,   405,   405,   406,   407,   408,
   408,   409,   409,   410,   410,   411,   412,   412,   413,   414,
   414,   414,   414,   415,   416,   416,   417,   418,   419,   419,
   420,   420,   420,   420,   420,   420,   420,   421,   421,   421,
   421,   422,   423,   423,   424,   424,   426,   425,   427,   427,
   427,   427,   427,   428,   428,   429,   429,   429,   430,   430,
   430,   430,   431,   431,   432,   432,   432,   432,   433,   433,
   434,   435,   436,   436,   437,   437,   437,   437,   437,   438,
   438,   438,   439,   440,   441,   441,   442,   442,   443,   443,
   444,   444,   444,   445,   445,   445,   446,   447,   448,   448,
   449,   449,   450,   450,   451,   451,   452,   452,   453,   454,
   454,   454,   454,   454,   454,   455,   456,   456,   456,   456,
   456,   457,   457,   458,   458,   459,   460,   461,   461,   461,
   461,   462,   462,   463,   464,   465,   465,   465,   465,   465,
   466,   466,   467,   467,   468,   469,   470,   470,   470,   470,
   470,   471,   472,   472,   472,   473,   473,   474,   474,   475,
   475,   475,   475,   476,   476,   477,   477,   478,   479,   479,
   479,   479,   479,   479,   480,   480,   480,   480,   480,   480,
   481,   481,   481,   482,   482,   483,   483,   484,   485,   485,
   486,   486,   487,   487,   488,   488,   489,   489,   490,   490,
   491,   491,   491,   492,   492,   493,   493,   493,   494,   494,
   495,   496,   496,   497,   497,   497,   498,   498,   498,   498,
   498,   498,   499,   499,   500,   500,   501,   501,   502,   503,
   503,   504,   504,   505,   505,   505,   506,   506,   506,   507,
   507,   507,   508,   508,   508,   509,   510,   510,   510,   510,
   510,   510,   510,   510,   510,   510,   510,   510,   510,   510,
   510,   510,   510,   510,   510,   510,   510,   510,   510,   510,
   510,   510,   510,   510,   510,   510,   510,   510,   510,   511,
   511,   511,   511,   512,   512,   512,   512,   513,   513,   514,
   514,   514,   515,   515,   515,   516,   516,   517,   517,   517,
   517,   517,   518,   518,   519,   519,   520,   520,   521,   521,
   521,   521,   522,   522,   522,   522,   522,   522,   523,   523,
   524,   524,   524,   524,   524,   524,   524,   524,   524,   525,
   525,   526,   526,   526,   526,   526,   527,   528,   528,   528,
   528,   528,   528,   528,   528,   529,   529,   530,   530,   531,
   531,   531,   531,   531,   531,   531,   531,   531,   531,   531,
   531,   531,   531,   531,   531,   531,   531,   531,   531,   531,
   531,   531,   531,   531,   531,   531,   531,   531,   531,   531,
   531,   531,   531,   531,   531,   531,   531,   531,   531,   531,
   531,   531,   531,   531,   531,   531,   531,   531,   531,   531,
   531,   531,   531,   531,   531,   531,   531,   531,   531,   531,
   531,   531,   531,   531,   531,   531,   531,   531,   531,   531,
   531,   531,   531,   531,   531,   531,   531,   531,   531,   531,
   531,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   533,   533,   533,   534,   534,   534,   535,
   535,   535,   536,   536,   536,   537,   537,   538,   538,   538,
   538,   538,   538,   538,   538,   538,   538,   538,   538,   538,
   538,   538,   538,   538,   538,   538,   538,   538,   538,   538,
   539,   539,   540,   540,   541,   541,   542,   542,   542,   543,
   543,   544,   544,   545,   545,   546,   546,   547,   547,   547,
   548,   548,   549,   550,   550,   551,   551,   551,   552,   552,
   553,   553,   553,   554,   554,   554,   555,   555,   555,   556,
   556,   557,   557,   557,   557,   558,   558,   559,   559,   560,
   561,   562,   563,   564,   565,   566,   567,   568,   568,   568,
   568,   568,   568,   568,   569,   570,   571,   572,   573,   574,
   574,   574,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   577,   577,   578,   578,   578,   579,   579,   579,   579,
   580,   581,   582,   582,   583,   583,   584,   584,   585,   585,
   586,   586,   587,   587,   587,   587,   588,   588,   588,   589,
   590,   590,   591,   592,   594,   593,   595,   596,   597,   597,
   599,   600,   598,   601,   601,   601,   601,   601,   601,   601,
   602,   602,   602,   602,   602,   602,   603,   604,   605,   606,
   607,   608,   609,   609,   610,   610,   610,   610,   610,   610,
   610,   610,   610,   610,   610,   611,   612,   612,   613,   614,
   614,   615,   615,   616,   617,   618,   618,   618,   618,   619,
   619,   620,   621,   620,   622,   622,   623,   624,   625,   625,
   626,   626,   627,   628,   629,   630,   631,   631,   631,   631,
   631,   632,   632,   632,   632,   632,   633,   633,   634,   634,
   634,   634,   634,   634,   634,   634,   634,   634,   634,   634,
   635,   634,   636,   634,   634,   637,   637,   638,   638,   640,
   639,   641,   641,   642,   643,   644,   644,   644,   645,   645,
   645,   645,   645,   645,   645,   645,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   647,   647,   648,   649,   649,
   650,   650,   651,   652,   653,   654,   655,   655,   655,   655,
   656,   656,   657,   658,   659,   659,   660,   660,   661,   661,
   661,   661,   661,   661,   661,   661,   661,   661,   661,   661,
   661,   661,   661,   661,   661,   661,   661,   661,   661,   661,
   661,   661,   661,   661,   661,   661,   661,   661,   662,   662,
   662,   662,   662,   663,   663,   663,   663,   663,   664,   665
};

static const short yyr2[] = {     0,
     1,     0,     2,     4,     3,     1,     1,     1,     1,     1,
     2,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     8,     8,     3,
     3,     0,     1,     1,     0,     1,     1,     0,     3,     1,
     3,     0,     3,     0,     4,     4,     4,     6,     5,     3,
     1,     1,     1,     1,     1,     2,     3,     4,     2,     3,
     4,     5,     3,     4,     3,     6,     5,     2,     2,     7,
     1,     1,     1,     1,     1,     1,     0,     2,     0,     3,
     0,     8,     1,     0,     3,     1,     0,     1,     1,     3,
     3,     1,     0,     2,     1,     2,     0,     3,     1,     4,
     2,     2,     2,     1,     2,     5,     3,     1,     1,     2,
     3,     3,     3,     3,     3,     3,     3,     2,     2,     3,
     6,     3,     3,     4,     3,     2,     2,     1,     1,     4,
     1,     4,     1,     1,     3,     1,     4,     4,     5,    10,
     3,     1,     1,     1,     1,     2,     3,     3,     3,     3,
     3,     3,     3,     2,     2,     3,     6,     3,     3,     4,
     3,     3,     4,     3,     3,     2,     2,     2,     2,     3,
     2,     4,     3,     3,     4,     4,     5,     6,     5,     6,
     3,     1,     1,     2,     2,     0,     2,     1,     0,     3,
     3,     2,     1,     2,     2,     4,     0,     7,     3,     0,
     3,     1,     1,     4,     2,     0,     2,     1,     2,     2,
     2,     2,     1,     1,     1,     2,     1,     2,     9,     1,
     0,     4,    14,     1,     1,     1,     3,     5,     1,     1,
     1,     3,     1,     0,     1,     1,     1,     3,     0,     1,
     1,     1,     1,     5,     3,     2,     1,     1,     1,     1,
     1,     1,     1,     1,     3,     1,     3,     3,     1,     3,
     1,     1,     1,     1,     2,     3,     3,     6,     4,     1,
     1,     1,     1,     0,     1,     2,     1,     1,     1,     0,
     2,     2,     0,     7,     2,     1,     1,     1,     3,     1,
     1,     1,     1,     1,     1,     2,     1,     3,     0,     6,
    11,     1,     0,     2,     0,     1,     1,     3,     1,     6,
     3,     2,     2,     0,     1,     2,     0,     4,    11,     2,
     0,     3,     2,     1,     3,     2,     1,     0,     3,     1,
     1,     1,     1,     4,     1,     1,     4,     6,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     3,     3,
     3,     9,     1,     0,     1,     0,     0,    13,     1,     1,
     1,     3,     3,     1,     1,     2,     3,     2,     1,     1,
     1,     1,     3,     1,     1,     1,     1,     1,     1,     0,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     1,
     1,     0,     5,     2,     6,     3,     3,     0,     3,     0,
     1,     1,     0,     1,     1,     0,     3,     4,     3,     5,
     1,     0,     1,     0,     3,     0,     1,     3,     3,     1,
     1,     1,     1,     1,     1,     4,     4,     2,     1,     7,
     4,     3,     0,     3,     1,     2,     4,     3,     8,     7,
     6,     1,     0,     6,     7,     1,     1,     1,     2,     0,
     2,     0,     2,     2,     2,     4,     3,     1,     3,     4,
     4,     8,     4,     2,     0,     1,     0,     1,     0,     1,
     3,     1,     0,     3,     0,     1,     3,     2,     2,     2,
     2,     1,     1,     0,     4,     4,     2,     4,     2,     0,
     1,     1,     1,     1,     1,     1,     0,     1,     1,     3,
     3,     0,     2,     0,     3,     0,     2,     0,     2,     0,
     3,     1,     1,     3,     1,     3,     2,     1,     1,     4,
     2,     2,     1,     4,     4,     3,     2,     2,     2,     1,
     1,     0,     1,     0,     4,     2,     3,     1,     1,     2,
     0,     1,     2,     3,     4,     0,     3,     4,     0,     2,
     1,     2,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     2,
     2,     2,     2,     1,     2,     1,     1,     3,     0,     5,
     3,     0,     5,     3,     0,     4,     1,     4,     2,     1,
     3,     2,     1,     0,     3,     0,     2,     0,     1,     2,
     1,     2,     1,     1,     1,     1,     1,     1,     3,     0,
     1,     3,     3,     3,     3,     3,     3,     3,     0,     1,
     1,     7,     8,     8,     7,     7,     3,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     3,     1,     2,
     1,     1,     1,     2,     3,     3,     3,     3,     3,     3,
     3,     3,     2,     2,     3,     6,     3,     3,     3,     4,
     2,     2,     4,     3,     4,     1,     1,     4,     1,     4,
     1,     1,     4,     4,     4,     4,     5,     5,     5,     4,
     2,     3,     2,     4,     3,     4,     3,     4,     5,     6,
     5,     6,     5,     5,     5,     5,     5,     5,     5,     5,
     6,     6,     6,     6,     6,     6,     6,     6,     6,     6,
     6,     6,     6,     6,     6,     6,     3,     3,     2,     1,
     1,     2,     1,     1,     2,     3,     3,     3,     3,     2,
     2,     3,     6,     3,     3,     2,     2,     3,     4,     1,
     1,     4,     1,     4,     1,     1,     4,     4,     5,     5,
     5,     4,     1,     4,     6,     0,     1,     3,     3,     3,
     0,     1,     1,     1,     1,     3,     0,     2,     1,     2,
     3,     3,     3,     3,     2,     3,     6,     3,     3,     2,
     2,     1,     3,     4,     4,     4,     5,     5,     5,     4,
     3,     0,     2,     0,     2,     0,     3,     2,     1,     1,
     1,     1,     3,     1,     1,     1,     3,     5,     6,     4,
     2,     1,     4,     2,     0,     2,     1,     0,     3,     3,
     1,     3,     3,     3,     1,     1,     4,     2,     3,     3,
     1,     3,     1,     3,     1,     1,     0,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     2,     1,     1,     1,     2,     1,     1,     1,     1,     1,
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
     1,     1,     1,     5,     3,     2,     3,     6,     1,     1,
     2,     2,     1,     0,     1,     3,     2,     0,     2,     0,
     2,     0,     1,     3,     4,     3,     1,     1,     1,     1,
     2,     0,     7,     3,     0,     4,     5,     5,     0,     2,
     0,     0,     6,     1,     1,     1,     1,     1,     1,     0,
     1,     1,     1,     1,     1,     1,     4,     2,     4,     4,
     2,     2,     0,     1,     1,     2,     1,     2,     1,     2,
     1,     1,     1,     1,     2,     1,     1,     3,     4,     0,
     2,     0,     1,     3,     2,     1,     1,     1,     0,     1,
     1,     3,     0,     4,     1,     1,     2,     3,     0,     2,
     1,     3,     4,     2,     3,     6,     3,     3,     4,     4,
     0,     3,     3,     4,     4,     0,     1,     0,     1,     1,
     1,     1,     2,     1,     2,     2,     1,     2,     2,     2,
     0,     5,     0,     5,     1,     1,     0,     0,     2,     0,
     4,     1,     3,     3,     6,     3,     4,     3,     1,     1,
     1,     2,     3,     5,     2,     5,     2,     1,     1,     1,
     2,     3,     3,     3,     3,     3,     3,     3,     2,     2,
     3,     6,     3,     3,     3,     4,     2,     2,     4,     3,
     4,     1,     1,     4,     1,     4,     1,     4,     4,     4,
     4,     5,     5,     5,     4,     2,     3,     2,     4,     3,
     4,     3,     4,     5,     6,     5,     6,     5,     5,     5,
     5,     5,     5,     5,     5,     6,     6,     6,     6,     6,
     6,     6,     6,     6,     6,     6,     6,     6,     6,     6,
     6,     3,     3,     2,     1,     1,     3,     1,     0,     2,
     1,     2,     2,     2,     1,     1,     0,     1,     2,     2,
     1,     1,     1,     1,     1,     2,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     3,     1,     1
};

static const short yydefact[] = {     2,
     1,  1298,  1342,  1324,  1325,  1326,  1327,  1328,  1329,  1330,
  1331,  1332,  1333,  1334,  1335,  1336,  1337,  1338,  1339,  1340,
  1341,  1319,  1320,  1314,   916,   917,  1347,  1323,  1318,  1343,
  1344,  1348,  1345,  1346,  1359,  1360,     3,  1321,  1322,     6,
  1095,     0,     8,     7,  1317,     9,    10,  1110,     0,     0,
     0,  1149,     0,     0,     0,     0,     0,     0,   432,   897,
   432,   124,     0,     0,     0,   432,     0,   314,     0,     0,
     0,   432,   513,     0,     0,     0,   432,     0,   117,   452,
     0,     0,     0,     0,   507,   314,     0,     0,     0,   452,
     0,     0,     0,    21,    13,    27,    51,    52,    53,    12,
    14,    15,    16,    17,    18,    19,    25,    20,    26,    23,
    24,    30,    31,    42,    32,    28,    36,    40,    37,    39,
    38,    41,    44,   464,    33,    34,    45,    46,    47,    48,
    49,    22,    50,    29,    43,   463,   465,    35,   462,   461,
   460,   515,   498,    54,    55,    56,    57,    58,    59,    60,
    61,    62,    63,    64,    65,    66,    67,  1109,  1107,  1104,
  1108,  1106,  1105,     0,  1110,  1101,   993,   994,   995,   996,
   997,   998,   999,  1000,  1001,  1002,  1003,  1004,  1005,  1006,
  1007,  1008,  1009,  1010,  1011,  1012,  1013,  1014,  1015,  1016,
  1017,  1018,  1019,  1020,  1021,  1022,  1023,   925,   926,   665,
   937,   666,   946,   949,   950,   953,   667,   664,   956,   961,
   963,   965,   967,   968,   970,   971,   976,   668,   983,   984,
   985,   986,   663,   992,   987,   989,   927,   928,   929,   930,
   931,   932,   933,   934,   935,   936,   938,   939,   940,   941,
   942,   943,   944,   945,   947,   948,   951,   952,   954,   955,
   957,   958,   959,   960,   962,   964,   966,   969,   972,   973,
   974,   975,   978,   977,   979,   980,   981,   982,   988,   990,
   991,  1311,   918,  1312,  1306,   924,  1074,  1070,   900,    11,
     0,  1069,  1090,   923,     0,  1089,  1087,  1066,  1083,  1088,
   919,     0,  1148,  1147,  1151,  1150,  1145,  1146,  1157,  1159,
   905,   923,     0,  1313,     0,     0,     0,     0,     0,     0,
     0,   431,   430,   426,   109,   896,   427,   123,   342,     0,
     0,     0,   288,   289,     0,     0,   287,     0,     0,   260,
     0,     0,     0,     0,   980,   490,     0,     0,     0,   373,
     0,   370,     0,     0,     0,   371,     0,     0,   372,     0,
     0,   428,     0,  1153,   313,   312,   311,   310,   320,   326,
   333,   331,   330,   332,   334,     0,   327,   328,     0,     0,
   429,   512,   510,     0,   998,   446,   983,     0,     0,  1062,
  1063,     0,   899,   898,     0,   425,     0,   904,   116,     0,
   451,     0,     0,   422,   424,   423,   434,   907,   506,     0,
   320,   421,   983,     0,    99,   983,     0,    96,   454,     0,
   432,     0,     5,  1164,     0,   509,     0,   509,   546,  1096,
     0,  1100,     0,     0,  1073,  1078,  1078,  1071,  1065,  1080,
     0,     0,     0,  1094,     0,  1158,     0,  1197,     0,  1209,
     0,     0,  1210,  1211,     0,  1206,  1208,     0,   537,    72,
     0,    72,     0,     0,   436,     0,   906,     0,   246,     0,
     0,   291,   290,   294,   387,   385,   386,   381,   382,   383,
   384,   285,     0,   293,   292,     0,  1144,   487,   488,   486,
     0,   581,   306,   538,   539,    70,     0,     0,   447,     0,
   379,     0,   380,     0,   307,   369,  1156,  1155,  1152,  1159,
   317,   318,   319,     0,   323,   315,   325,     0,     0,     0,
     0,     0,   993,   994,   995,   996,   997,   998,   999,  1000,
  1001,  1002,  1003,  1004,  1005,  1006,  1007,  1008,  1009,  1010,
  1011,  1012,  1013,  1014,  1015,  1016,  1017,  1018,  1019,  1020,
  1021,  1022,  1023,   878,     0,   654,   654,     0,   726,   727,
   729,   731,   645,   937,     0,     0,   914,   639,   679,     0,
   654,     0,     0,   681,   642,     0,     0,   983,   984,     0,
   913,   732,   650,   989,     0,     0,   816,     0,   895,     0,
     0,     0,     0,   586,   593,   596,   595,   591,   647,   594,
   924,   893,   701,   680,   780,   816,   505,   891,     0,     0,
   702,   912,   908,   909,   910,   703,   781,  1307,   923,  1165,
   445,    90,   444,     0,     0,     0,     0,     0,  1197,     0,
   119,     0,   459,   581,   478,   323,   100,     0,    97,     0,
   453,   449,   497,     4,   499,   508,     0,     0,     0,     0,
   530,     0,  1133,  1134,  1132,  1123,  1131,  1127,  1129,  1125,
  1123,  1123,     0,  1136,  1102,  1115,     0,  1113,  1114,     0,
     0,  1111,  1112,  1116,  1075,  1072,     0,  1067,     0,     0,
  1082,     0,  1086,  1084,  1160,  1161,  1163,  1187,  1184,  1196,
  1191,     0,  1179,  1182,  1181,  1193,  1180,  1171,     0,  1195,
     0,     0,  1212,   995,     0,  1207,   536,     0,     0,    75,
  1097,    75,     0,   265,   264,     0,   438,     0,     0,   397,
   244,   240,     0,     0,   286,     0,   489,     0,     0,   477,
     0,     0,   376,   374,   375,   377,     0,   262,  1154,   316,
     0,     0,     0,     0,   329,     0,     0,     0,   466,   469,
     0,   511,     0,   816,     0,     0,   877,     0,   653,   649,
   656,     0,     0,     0,     0,   632,   631,     0,   821,     0,
   630,   665,   666,   667,   663,   671,   662,   654,   652,   779,
     0,     0,   633,   827,   852,     0,   660,     0,   599,   600,
   601,   602,   603,   604,   605,   606,   607,   608,   609,   610,
   611,   612,   613,   614,   615,   616,   617,   618,   619,   620,
   621,   622,   623,   624,   625,   626,   627,   628,   629,     0,
   661,   670,   598,   592,   659,   597,   721,     0,   915,   704,
   714,   713,     0,     0,     0,   680,   911,     0,   590,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   741,
   743,   722,     0,     0,     0,     0,     0,     0,     0,   700,
   124,     0,   550,     0,     0,     0,     0,  1308,  1304,    94,
    95,    87,    93,     0,    92,    85,    91,    86,   886,   816,
   550,   885,     0,   816,  1171,   448,     0,     0,   490,   358,
   483,   309,   101,    98,   456,   501,   514,   516,   524,   500,
   548,     0,     0,   496,     0,  1118,  1124,  1121,  1122,  1135,
  1128,  1130,  1126,  1142,     0,  1110,  1110,     0,  1077,     0,
  1079,     0,  1064,  1085,     0,     0,  1188,  1190,  1189,     0,
     0,     0,  1178,  1183,  1186,  1185,  1299,  1213,  1299,   396,
   396,   396,   396,   102,     0,    73,    74,    78,    78,   433,
   270,   269,   271,     0,   266,     0,   440,   636,   937,   634,
   637,   363,     0,   921,   922,   364,   920,   368,     0,     0,
   248,     0,     0,     0,     0,   245,   127,     0,     0,     0,
   299,     0,   296,     0,     0,   580,   540,   284,     0,     0,
   388,   322,   321,     0,     0,   468,     0,     0,   475,   816,
     0,     0,   875,   872,   876,     0,     0,     0,   658,   817,
     0,     0,     0,     0,     0,   824,   825,   823,     0,     0,
   822,     0,     0,     0,     0,     0,   651,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   816,
     0,   829,   842,   854,     0,     0,     0,     0,     0,     0,
   680,   859,     0,     0,   726,   727,   729,   731,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   701,
     0,   816,     0,   702,   703,     0,  1295,  1307,   717,     0,
     0,   589,     0,     0,  1028,  1030,  1031,  1033,  1035,  1036,
  1039,  1040,  1041,  1048,  1049,  1050,  1051,  1055,  1056,  1057,
  1058,  1061,  1025,  1026,  1027,  1029,  1032,  1034,  1037,  1038,
  1042,  1043,  1044,  1045,  1046,  1047,  1052,  1053,  1054,  1059,
  1060,  1024,   892,   715,   777,     0,   800,   801,   803,   805,
     0,     0,     0,   806,     0,     0,     0,     0,     0,     0,
   816,     0,   783,   784,   813,  1305,     0,   747,     0,   742,
   745,   719,     0,     0,     0,   778,     0,     0,     0,   718,
     0,     0,   711,     0,   712,     0,     0,     0,   709,     0,
     0,     0,   710,     0,     0,     0,   705,     0,     0,     0,
   706,     0,     0,     0,   708,     0,     0,     0,   707,   507,
   504,  1296,  1307,   890,     0,   581,   894,   879,   881,   902,
     0,   724,     0,   880,  1310,  1309,   970,    89,   888,     0,
   581,     0,     0,  1178,   118,   112,   111,     0,     0,   482,
     0,     0,   450,     0,   522,   523,     0,   518,     0,   545,
   532,   533,   527,   531,   535,   529,   534,     0,  1143,     0,
  1137,     0,     0,  1315,     0,     0,  1076,  1092,  1081,  1162,
  1197,  1197,  1176,     0,  1176,     0,  1177,  1205,     0,     0,
     0,   395,     0,     0,     0,   127,   108,     0,     0,     0,
   394,    71,    76,    77,    82,    82,     0,     0,   443,     0,
   435,   635,     0,   362,   367,   361,     0,     0,     0,   247,
   257,   249,   250,   251,   252,     0,     0,   126,   128,   129,
   176,     0,   242,   243,     0,     0,     0,     0,     0,   295,
   345,   492,   492,     0,   378,     0,   308,     0,   335,   339,
   337,     0,     0,     0,   476,   340,     0,     0,   871,     0,
     0,     0,     0,   648,     0,     0,   870,   728,   730,     0,
   644,   733,   734,     0,   638,   673,   674,   675,   676,   678,
   677,   672,     0,     0,   641,     0,   827,   852,     0,   840,
   830,   835,     0,   735,     0,     0,   841,     0,     0,     0,
     0,   828,     0,     0,   856,   736,   669,     0,   858,     0,
     0,     0,   740,     0,     0,     0,     0,   821,   779,  1294,
   827,   852,     0,   721,  1237,   704,  1221,   714,  1230,   713,
  1229,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   741,   743,   722,     0,     0,     0,     0,     0,     0,     0,
   700,     0,     0,   816,     0,     0,   688,   690,   689,   691,
   692,   693,   694,   695,     0,   687,     0,   584,   589,   646,
     0,     0,     0,   827,   852,     0,   796,   785,   791,   790,
     0,     0,     0,   797,     0,     0,     0,     0,   782,     0,
   860,     0,   861,   862,   912,   746,   744,   748,     0,     0,
   720,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1303,     0,   549,
   553,   555,   552,   558,   582,   542,     0,   723,   725,    88,
   884,   484,   889,     0,  1166,   114,   115,   121,   113,     0,
   481,     0,     0,   457,   517,   519,   520,   521,   547,     0,
     0,     0,  1098,  1103,  1142,   586,  1117,  1316,  1119,  1120,
     0,  1068,  1200,     0,  1197,     0,     0,     0,  1167,  1176,
  1168,  1176,  1349,  1350,  1353,  1216,  1351,  1352,  1300,  1214,
     0,     0,     0,     0,     0,     0,   103,     0,   105,     0,
   393,     0,    84,    84,     0,   267,   442,   437,   441,   446,
   365,     0,     0,   366,   417,   418,   415,   416,     0,   258,
     0,     0,   237,     0,   239,   137,   133,   238,     0,     0,
   382,   303,   253,   254,   300,   302,   255,   304,   301,   298,
   297,     0,     0,     0,   485,  1093,   390,   391,   389,   336,
     0,   324,   467,   474,     0,   471,     0,   874,   868,     0,
   655,   657,   819,   818,     0,   820,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   838,   836,   826,   839,   831,
   832,   834,   833,   843,     0,   853,     0,   851,   737,   738,
   739,   857,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   717,   715,   777,  1292,     0,     0,   747,
     0,   742,   745,   719,  1235,     0,     0,     0,   778,  1293,
     0,     0,     0,   718,  1234,     0,     0,     0,   712,  1228,
     0,     0,     0,   709,  1226,     0,     0,     0,   710,  1227,
     0,     0,     0,   705,  1222,     0,     0,     0,   706,  1223,
     0,     0,     0,   708,  1225,     0,     0,     0,   707,  1224,
     0,   724,     0,     0,   814,     0,     0,   697,   696,     0,
     0,   589,     0,   585,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   794,   792,   749,   795,   786,   787,   789,
   788,   798,     0,   751,     0,     0,   864,     0,   865,   866,
     0,     0,   753,     0,     0,   760,     0,     0,   758,     0,
     0,   759,     0,     0,   754,     0,     0,   755,     0,     0,
   757,     0,     0,   756,   503,  1297,   572,     0,   559,     0,
     0,   574,   571,   574,   572,   570,   574,   561,   563,     0,
     0,   557,   583,     0,   544,   883,   882,   887,     0,   110,
     0,   480,     0,     0,   455,   526,   525,   528,  1138,  1140,
  1091,  1142,  1192,  1199,  1194,  1176,     0,  1176,     0,  1169,
  1170,     0,     0,   184,     0,     0,     0,     0,     0,     0,
     0,   183,   185,     0,     0,     0,   104,     0,     0,     0,
     0,     0,    69,    68,   274,     0,     0,   439,   360,     0,
     0,   175,   125,     0,   122,   241,   243,     0,   131,     0,
     0,     0,     0,     0,     0,   144,   130,   132,   135,   139,
     0,   305,   256,   344,   901,     0,     0,     0,   491,     0,
     0,   873,   716,   643,   869,   640,     0,   845,   846,     0,
     0,     0,   850,   844,   855,     0,   728,   730,   733,   734,
   735,   736,     0,     0,     0,   740,     0,     0,   746,   744,
   748,     0,     0,   720,  1236,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   723,
   725,   816,     0,     0,     0,     0,   699,     0,   587,   589,
     0,   802,   804,   807,   808,     0,     0,     0,   812,   799,
   863,   750,   752,     0,   769,   761,   776,   768,   774,   766,
   775,   767,   770,   762,   771,   763,   773,   765,   772,   764,
     0,   551,   554,     0,   573,   567,   568,     0,   569,   562,
     0,   556,     0,     0,   502,     0,   479,   458,     0,  1139,
     0,     0,  1202,  1172,  1176,  1173,  1176,     0,   206,   207,
   186,   195,   194,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   209,   211,   208,     0,     0,     0,     0,     0,
     0,     0,   177,     0,     0,     0,   178,   107,     0,   392,
    81,    80,     0,   273,     0,     0,   268,     0,   581,   414,
     0,   136,     0,     0,     0,   168,   169,   171,   173,   141,
   174,     0,     0,     0,     0,     0,   142,     0,   149,   143,
   145,   473,   134,   259,     0,   346,   347,   349,   354,     0,
   902,   493,     0,   494,   338,     0,     0,   847,   848,   849,
     0,   737,   738,   739,   749,   751,     0,     0,     0,     0,
   753,     0,     0,   760,     0,     0,   758,     0,     0,   759,
     0,     0,   754,     0,     0,   755,     0,     0,   757,     0,
     0,   756,   815,   682,     0,   685,   686,     0,   588,     0,
   809,   810,   811,   867,     0,   566,     0,     0,   541,   543,
   120,  1354,  1355,     0,  1356,  1357,  1141,  1301,   586,  1201,
  1142,  1174,  1175,     0,   198,   196,   204,     0,   223,     0,
   214,     0,   210,   213,   202,     0,     0,     0,   205,   201,
   191,   192,   193,   187,   188,   190,   189,   199,     0,   182,
     0,   179,   106,     0,    83,   275,   276,   272,     0,     0,
     0,     0,     0,     0,   138,     0,     0,     0,   166,   150,
   159,   158,     0,     0,   167,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   226,   361,     0,     0,     0,   357,
     0,   495,   470,   837,   716,   750,   752,   769,   761,   776,
   768,   774,   766,   775,   767,   770,   762,   771,   763,   773,
   765,   772,   764,   683,   684,   793,   560,   565,     0,     0,
   564,     0,  1302,  1204,  1203,     0,     0,     0,   222,   216,
   212,   215,     0,     0,   203,     0,   200,     0,    79,     0,
   359,   420,   413,   236,   140,     0,     0,     0,   162,   160,
   165,   155,   156,   157,   151,   152,   154,   153,   163,     0,
   148,     0,     0,   229,   341,   348,   353,   352,     0,   351,
   355,   903,     0,   576,     0,  1358,     0,   219,     0,   217,
     0,     0,   181,   473,   279,   419,     0,     0,   170,   172,
     0,   164,   472,   224,   225,     0,   146,   228,   356,   354,
     0,   578,   579,   197,   221,   220,   218,   226,     0,   277,
   280,   281,   282,   283,   399,     0,     0,   398,   401,   412,
   409,   411,   410,   400,     0,   147,     0,     0,   227,   357,
     0,   575,   229,     0,   263,     0,   404,   405,     0,   161,
   233,     0,     0,   230,   231,   350,   577,   180,   278,   402,
   406,   408,   403,   232,   234,   235,   407,     0,     0,     0
};

static const short yydefgoto[] = {  2398,
     1,    37,    92,    93,    94,    95,    96,   700,   938,  1265,
  2051,  1563,  1853,    97,   866,   862,    98,    99,   100,   934,
   101,   102,  1208,  1508,   390,   878,  1810,   103,   331,  1287,
  1288,  1289,  1877,  1878,  1869,  1879,  1880,  2300,  2077,  1290,
  1291,  2189,  1840,  2268,  2269,  2304,  2337,  2338,  2384,  1865,
   104,   968,  1292,  1293,   105,   711,   966,  1592,  1593,  1594,
   106,   332,   107,   108,   706,   944,   945,  1856,  2055,  2198,
  2349,  2350,   109,   110,   472,   333,   971,   715,   972,   973,
  1595,   111,   112,   359,   505,   733,   113,   366,   367,   368,
  1310,  1612,   114,   115,   334,  1603,  2085,  2086,  2087,  2088,
  2230,  2310,   116,   117,  1573,   709,   953,  1276,  1277,   118,
   351,   119,   724,   120,   121,  1596,   474,   980,   122,  1560,
  1258,   123,   959,  2358,  2376,  2377,  2378,  2059,  1579,  2327,
  2360,   125,   126,   127,   314,   128,   129,   130,   947,  1271,
  1568,   612,   131,   132,   133,   392,   632,  1213,  1513,   134,
   135,  2361,   739,  2225,   988,   989,  2362,   138,  1211,  2363,
   140,   481,  1605,  1889,  2094,   141,   142,   143,   853,   400,
   637,   374,   419,   887,   888,  1218,   894,  1223,  1226,   698,
   483,   484,  1805,  2005,   641,  1220,  1186,  1490,  1491,  1492,
  1788,  1493,  1798,  1799,  1800,  1996,  2261,  2341,  2342,   720,
  1494,   829,  1428,   583,   584,   585,   586,   587,   954,   761,
   773,   756,   588,   589,   750,   999,  1324,   590,   591,   777,
   767,  1000,   593,   824,  1425,  1731,   825,   594,  1130,   819,
  1042,  1009,  1010,  1028,  1029,  1035,  1365,  1648,  1043,  1452,
  1453,  1758,  1759,   595,   993,   994,  1320,   743,   596,  1188,
   871,   872,   597,   598,   315,   745,   277,  1884,  1189,  2311,
   387,   485,   600,   397,   601,   602,   603,   604,   605,   287,
   956,   606,  1113,   384,   144,   296,   281,   425,   426,   666,
   668,   671,   913,   288,   289,   282,  1532,   145,   146,    40,
    48,    41,   420,   164,   165,   423,   904,   166,   655,   656,
   657,   658,   659,   660,   661,   896,   662,   663,  1230,  1231,
  2010,  1232,   147,   148,   297,   298,   149,   500,   499,   150,
   151,   436,   675,   152,   153,   154,   155,   923,  1539,  1248,
  1533,   916,   920,   689,  1534,  1535,  1822,  2012,  2013,   156,
   157,   446,  1066,  1181,    42,  1249,  2157,  1182,   607,  1067,
   608,   859,   609,   690,    43,  1233,    44,  1234,  1549,  2158,
    46,    47
};

static const short yypact[] = {-32768,
  1651,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,  1545,-32768,-32768,-32768,-32768,-32768,  1161, 23905,   489,
   109, 23077,   601, 27482,   601,   -86,   103,   426,    50, 27482,
   474,  1918, 27757,   119,  1538,   474,    38,    53,   473,   129,
   473,   474,   263, 25282, 25557,   -86,   474, 27482,    82,   101,
   245, 25557, 21220,   120,   143,    53, 25557, 26107, 26382,   101,
   -64,  4203,   456,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,   505,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,    18,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,   468,    74,-32768,-32768,-32768,-32768,-32768,
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
-32768,-32768,-32768,-32768,-32768,-32768,   298,-32768,-32768,-32768,
   298,-32768,-32768,   337, 23353,-32768,-32768,-32768,    47,-32768,
-32768,   601,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   443,
-32768,-32768,   626,-32768,   631,   209,   209,   657, 25557,   601,
   705,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   601,
 27482, 27482,-32768,-32768, 27482, 27482,-32768, 27482, 25557,-32768,
   598,   541, 20645,   609,   601,    61, 25557, 27482,   601,-32768,
 27482,-32768, 27482, 27482, 27482,-32768,  1385,   696,-32768, 27482,
 27482,-32768,   675,-32768,-32768,-32768,-32768,-32768,   348,   683,
-32768,-32768,-32768,-32768,-32768,   702,   613,-32768, 25557,   769,
-32768,-32768,   787, 10886, 23629,   -15,   767,   849,   -48,-32768,
-32768,   825,-32768,-32768,   884,-32768,   892,-32768,-32768, 25557,
-32768,   624, 27482,-32768,-32768,-32768,-32768,-32768,-32768, 25557,
   348,-32768,   853,   947,-32768,   916,  1005,-32768,   937,     8,
   474,  1131,-32768,-32768,   -64,  1110,  1095,  1110,  1079,-32768,
  1093,-32768,   123, 27482,-32768,   908,   908,-32768,-32768,  1142,
  1150,  1338, 27482,-32768,   337,-32768,   337,   107, 27482,-32768,
  1048, 27482,-32768,-32768, 28032,-32768,-32768,   209,   964,  1063,
  1249,  1063,  1238,   808,  1121,  1028,-32768,  1275,-32768, 25557,
  1205,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,  1055,-32768,-32768, 27482,-32768,  1200,-32768,-32768,
  1287,  1193,-32768,  1101,-32768,-32768,  1257, 21495,-32768,  1028,
-32768,  1122,-32768,   120,-32768,-32768,-32768,-32768,-32768,   443,
-32768,-32768,-32768,  1135,   866,-32768,-32768, 27482,    32,   -34,
 27482, 27482,   -69,   220,   222,   223,   242,   270,   322,   333,
   342,   369,   472,   483,   522,   535,   544,   553,   557,   558,
   570,   573,   586,   602,   604,   605,   637,   647,   650,   651,
   661,   674,   700, 22798,  1138,  1244,  1244,  1144,-32768,  1149,
  1152,-32768,  1191,  1329,  1197,  1213,-32768,  1225,   862,  1383,
  1244, 16232,  1227,-32768,  1233,  1234,  1239,   720,   214,  1255,
-32768,-32768,-32768,   726,  4532, 16232,  1208, 16232,-32768, 16232,
 16232, 15341,   120,  1229,-32768,-32768,-32768,-32768,  1258,-32768,
   734,  1496,-32768,  4074,-32768,  1208,    19,-32768,  1309,  1311,
-32768,  1316,-32768,-32768,-32768,   819,-32768,    46,   749,-32768,
-32768,-32768,-32768,   -11,  1489,   -12,   -12, 20933,   107, 25557,
  1427, 27482,-32768,  1193,  1507,   866,-32768,  1494,-32768,  1505,
-32768, 25557,-32768,-32768,-32768,-32768,   -64, 16232,   -64,  1454,
   355,  1549,-32768,-32768,-32768,   -86,-32768,-32768,-32768,-32768,
   -86,   -86,  1027,-32768,-32768,-32768,  1377,-32768,-32768,  1378,
  1380,-32768,-32768,-32768,  1357,-32768,  1135,-32768,  1393, 23905,
  1495,  1338,-32768,-32768,-32768,  1388,-32768,-32768,-32768,-32768,
-32768,   582,-32768,-32768,-32768,-32768,-32768,   476,  1050,-32768,
  1389, 27482,-32768,  1662,  1401,-32768,-32768,    59,  1458,   -45,
-32768,   -45,   -64,-32768,-32768,   384,  1474,  8532,  1462,-32768,
   856,  1421,   120, 20357,-32768,  1571,-32768,  1611, 16232,-32768,
 27482, 25557,-32768,-32768,-32768,-32768, 26657,-32768,-32768,-32768,
 27482, 27482,  1598,  1542,-32768,  1533,  1428, 19784,-32768,-32768,
  1613,-32768,  1534,  1208,  1434,  1316,  1436, 16232,-32768,-32768,
  1655, 15341,  1135,  1135,  1135,-32768,-32768,  1563,  1151,  1135,
-32768,  1557,  1560,  1564,  1565,-32768,-32768,  1244,-32768,   771,
 16232,  1135,-32768, 18311, 15341,  1567,-32768,  8807,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1329,
-32768,  1554,-32768,-32768,-32768,-32768,  1146, 16529,-32768,  1694,
  1694,  1694,  1456,  1459,  1466,  2465,-32768,   -83,-32768,  1135,
 24457, 28784, 16232, 16826,  1461,   385, 16232,   542, 16232,-32768,
-32768, 15638,  9995, 11183, 11480, 11777, 12074, 12371, 12668,-32768,
   -84, 10886,  1652, 21770,  6744, 27482, 24181,-32768,-32768,-32768,
-32768,-32768,-32768, 28307,-32768,-32768,-32768,-32768,-32768,  1208,
   -39,-32768,  1476,   813,   476,-32768,  1519,    64,    61,-32768,
  1498,-32768,-32768,-32768,  1477,-32768,  1475,-32768,  2697,-32768,
  1626,   163,   906,-32768,  1751,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  1501,  2863,    37,    37, 27482,-32768, 27482,
-32768,  1338,-32768,-32768,   337,  1483,-32768,-32768,-32768,  1486,
    33,   262,  1753,-32768,-32768,-32768,-32768,-32768,-32768,    28,
  1711,  1711,  1711,-32768,   601,-32768,-32768,   183,   183,-32768,
-32768,-32768,-32768,  1644,  1642,  1514,  1581,-32768,  1645,-32768,
-32768,-32768,   728,-32768,-32768,-32768,-32768,  1550,  1657,   395,
-32768,   395,   395,   395,   395,-32768, 25007,  1746,  1588,  1532,
  1535,   913,-32768, 25557,   -24,  4074,-32768,-32768,  1520,  1517,
  1522,-32768,-32768,   337, 25832,-32768, 10886,   941,-32768,  1208,
 25832, 16232,    45,-32768,-32768, 27482,  2255,  1649,  1743,-32768,
   -98,  1525,  1526,   967,  1527,-32768,-32768,-32768,  1529,  1717,
-32768,  1537,   519,   235,  1661,  1701,-32768,  2815,   977,  1541,
  1543,  1544,  1548, 18311, 18311, 18311, 18311,  1551,   494,  1208,
  1559,-32768,   819,   -41,  1553,  1640, 12965, 15341, 12965, 12965,
  3461,   -76,  1556,  1562,   735,   523,   754,   842,  1568,  1569,
 16529,  1570,  1573,  1574, 16529, 16529, 16529, 16529, 15341,   844,
  4201,  1208,  1576,   869,  1009,   871,-32768,    44,-32768,  1285,
 16232,  1566,  1552,  1577,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,   771,  1579,-32768,  1580,  1582,-32768,
  1583,  1585,  1587,-32768, 16826, 16826, 16826, 16826, 16232,   446,
  1208,  1589,-32768,   819,-32768,-32768,  6089,-32768,   407,-32768,
-32768,   895, 16826,  1590, 16232,  1970,  1592,  1595, 13262,  1146,
  1597,  1599,-32768, 13262,  4859,  1600,  1601, 13262,  1221,  1609,
  1612, 13262,  1221,  1614,  1615, 13262,    43,  1616,  1619, 13262,
    43,  1620,  1623, 13262,  1694,  1624,  1625, 13262,  1694,   143,
  1561,-32768,    46,-32768, 19499,  1193,-32768,  1555,-32768,-32768,
  1610,-32768,   -97,  1555,-32768,-32768, 27482,-32768,-32768, 22798,
  1193, 22045,  1586,  1753,-32768,-32768,-32768,   774,  1771,  1602,
  1606, 27482,-32768, 16232,-32768,-32768,   438,-32768, 27482,-32768,
-32768,-32768,    30,-32768,-32768,  1647,-32768,  1860,-32768,  -147,
-32768,   -86,  2507,-32768,  1627,  1630,-32768,  1621,-32768,-32768,
   838,   838,   687,  1629,   687,  1636,-32768,-32768,  1013,  1307,
  1628,-32768,  1783,  1787,  1638, 25007,-32768, 27482, 27482, 27482,
 27482,-32768,-32768,-32768,  1819,  1819, 25557,   384,    -7,  1658,
-32768,-32768, 24732,-32768,-32768,  1741, 24732,   387,  1135,-32768,
-32768,-32768,-32768,-32768,-32768, 27482,   986,-32768,-32768,-32768,
-32768,   994,-32768, 28582,  1563, 20645, 20069, 20069, 20357,-32768,
  1750,  1832,  1832, 27482,-32768, 26932,  1561, 27482,-32768,  1748,
-32768,  1025, 27482,    20,-32768,-32768,  3428, 15341,-32768,  1852,
 28784, 27482, 27482,-32768, 16232, 15341,-32768,-32768,-32768,  1135,
-32768,-32768,-32768, 16232,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768, 16232,  1135,-32768, 18311, 18311, 15341,  9104,   498,
  1897,  1897,   188,-32768, 28784, 18311, 18608, 18311, 18311, 18311,
 18311,-32768,  7340, 15341,  1847,-32768,-32768,  1660,   -76,  1663,
  1664, 15341,-32768, 16232,  1135,  1135,  1563,  1151,  2888,-32768,
 18311, 15341,  9401,  1241,-32768,  1903,-32768,  1903,-32768,  1903,
-32768,  1665, 28784, 16529, 16826,  1667,   478, 16529,   550, 16529,
   896,   900, 10292, 10589, 13559, 13856, 14153, 14450, 14747, 15044,
   911,  7042, 16529,  1208,  1669,  1848,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,    10,  3187,   205,-32768,  1566,-32768,
 16826,  1135,  1135, 18311, 15341,  9698,   642,  1911,  1911,  1911,
  1072, 28784, 16826, 17123, 16826, 16826, 16826, 16826,-32768,  7638,
-32768,  1671,  1674,-32768,-32768,-32768,-32768,-32768,   620,  6089,
   895,  1563,  1563,  1673,  1563,  1563,  1675,  1563,  1563,  1677,
  1563,  1563,  1678,  1563,  1563,  1680,  1563,  1563,  1682,  1563,
  1563,  1683,  1563,  1563,  1686, 25557,   337,-32768, 25557,-32768,
  1690,  1333,-32768, 27207,  1700,  1874, 22320,-32768,-32768,-32768,
-32768,-32768,-32768, 15341,-32768,-32768,-32768,  1800,-32768,  1881,
  1719,  1720,  1071,-32768,-32768,-32768,-32768,-32768,  1697,   906,
   906,   163,-32768,-32768,  1501,  1229,-32768,-32768,-32768,-32768,
 27482,-32768,-32768,  1695,   838,  1696,   234,   644,-32768,   687,
-32768,   687,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
 18905,  1702,  1704, 27482,  1215, 28582,-32768,    17,-32768,  1814,
-32768,  1883,  1729,  1729,  1893,  1853,-32768,-32768,-32768,   -15,
-32768,  1055,  1938,-32768,-32768,-32768,-32768,-32768,  1825,-32768,
    96, 25007,  1789, 27482,-32768,  1864,   396,-32768,  1792, 27482,
  1204,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,   601,  1728,   297,-32768,-32768,-32768,-32768,-32768,-32768,
  1904,-32768,-32768,-32768,  1731,-32768, 15341,-32768,-32768,  1725,
-32768,-32768,  4074,-32768,  1732,  4074,  1347,  1733,    90,  1734,
  1736, 12965, 12965, 12965,  1737,-32768,-32768,   684,   498,    80,
    80,  1897,  1897,-32768,   -61,   -76, 15341,-32768,-32768,-32768,
-32768,   -76,  3666,  1738,  1739,  1744,  1745,  1747,  1749, 12965,
 12965, 12965,  1754,   948,   955,  2888,-32768,   652,  6089,   963,
   425,   982,  1017,  1076,-32768, 16826,  1742, 16529,  2999,-32768,
  1759,  1760, 13262,  1241,-32768,  1761,  1763, 13262,  4906,-32768,
  1764,  1765, 13262,  1726,-32768,  1766,  1767, 13262,  1726,-32768,
  1768,  1769, 13262,    93,-32768,  1770,  1772, 13262,    93,-32768,
  1773,  1774, 13262,  1903,-32768,  1775,  1776, 13262,  1903,-32768,
  1777,  1020,   250,  1779,-32768,  1563,  1780,-32768,-32768, 15935,
  1781,  1566,  1788,-32768,   835,  1778,  1784,  1786,  1790, 12965,
 12965, 12965,  1791,-32768,-32768,   776,   642,   125,   125,  1911,
  1911,-32768,   279,-32768, 22595, 16826,-32768,  1793,  1794,-32768,
  1795,  1797,-32768,  1799,  1802,-32768,  1803,  1809,-32768,  1810,
  1811,-32768,  1812,  1816,-32768,  1817,  1818,-32768,  1822,  1827,
-32768,  1828,  1829,-32768,-32768,-32768,  1668,  1831,-32768, 25557,
  1920,  1915,-32768,  1915,  1001,-32768,  1915,  1333,-32768,  1959,
 24457,-32768,-32768,  2016,  1983,-32768,-32768,-32768,  1896,-32768,
   -64,-32768,  1845, 27482,-32768,-32768,-32768,-32768,-32768,  1849,
-32768,  1501,-32768,-32768,-32768,   687,  1835,   687,  1834,-32768,
-32768,  1842, 18905,-32768, 18905, 18905, 18905, 18905, 18905,  1617,
  1843,-32768,  1851, 27482, 27482,  1247,-32768,  2048,  2051, 27482,
   601,  1879,-32768,-32768,  1929,  2046,   384,-32768,-32768,   120,
 25557,-32768,-32768,  1858,-32768,-32768,-32768,  2023,-32768,  1861,
 27482, 17420,  2017,  2034, 27482,-32768,-32768,   396,-32768,-32768,
   120,-32768,-32768,-32768,-32768, 27482,  2014,  2018,-32768,  2020,
 10886,-32768,-32768,-32768,-32768,-32768, 28784,-32768,-32768,  1866,
  1868,  1870,-32768,-32768,   -76, 28784,  1024,  1049,  1052,  1053,
  1088,  1089,  1875,  1876,  1878,  1090, 16826,  1880,  1100,  1103,
  1104,   747,  6089,  1076,-32768,  1563,  1563,  1882,  1563,  1563,
  1894,  1563,  1563,  1895,  1563,  1563,  1898,  1563,  1563,  1899,
  1563,  1563,  1900,  1563,  1563,  1902,  1563,  1563,  1908,  1113,
  1116,  1208,  1909,  1563,  1910,  1912,  4074,  1563,-32768,  1566,
 28784,-32768,-32768,-32768,-32768,  1913,  1914,  1917,-32768,-32768,
-32768,   776,-32768, 22595,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
  2053,-32768,-32768, 25557,-32768,-32768,-32768,  2055,-32768,-32768,
 25557,-32768, 15341, 16232,-32768,   120,-32768,-32768,   738,-32768,
   -86,    25,-32768,-32768,   687,-32768,   687, 18905,  4793,  1369,
  2134,  2134,  2134,  2146, 28784, 18905, 22595,  1905,   717, 18905,
   584, 18905,-32768,-32768, 19202, 18905, 18905, 18905, 18905, 18905,
 18905, 18905,-32768,  8234,  1266,  1279,-32768,-32768, 17717,-32768,
  1921,-32768,   120,-32768,   402,  2035,-32768,  2066,  1193,  1925,
 27482,-32768, 18905,   462,  1916,-32768,  1922,  1923,-32768,-32768,
-32768, 17717, 17717, 17717, 17717, 17717,   237,  1926,-32768,-32768,
-32768,  1930,-32768,-32768,  1924,  1932,-32768,-32768,   -50,  1933,
  1851,-32768, 27482,-32768,-32768,  1308,  1928,-32768,-32768,-32768,
  1931,  1117,  1119,  1126,    67,  1147, 16826,  1935,  1936,  1937,
  1148,  1939,  1940,  1158,  1941,  1942,  1167,  1943,  1944,  1170,
  1945,  1947,  1171,  1948,  1949,  1178,  1950,  1951,  1179,  1952,
  1962,  1185,-32768,-32768,  1966,-32768,-32768,  1973,-32768,  1974,
-32768,-32768,-32768,-32768, 25557,-32768, 25557,   299,   -76,  4074,
-32768,-32768,-32768,  2863,-32768,-32768,   738,-32768,  1229,-32768,
  1501,-32768,-32768,  3840,-32768,-32768,  4793,  2138,-32768, 22595,
-32768,   477,-32768,-32768,  1850, 22595,  1934, 18905,  4816,  1369,
  4990,  3362,  3362,   127,   127,  2134,  2134,-32768,  1320,  4617,
  2103,-32768,   237,   601,-32768,-32768,-32768,-32768, 27482,   120,
  2057, 27482,  1978,  2339,-32768, 17717,  1135,  1135,   840,  2211,
  2211,  2211,   420, 28784, 18014, 17717, 17717, 17717, 17717, 17717,
 17717, 17717,  7936, 27482,  2147,  1741, 27482, 28784, 28784,   269,
 27482,  1984,-32768,-32768,  1186,   150,  1188,  1201,  1202,  1226,
  1230,  1242,  1243,  1254,  1261,  1268,  1288,  1294,  1296,  1302,
  1304,  1305,  1306,-32768,-32768,-32768,-32768,-32768, 16232,  1985,
-32768,  3149,-32768,-32768,-32768, 28784, 22595,  1321,-32768,-32768,
-32768,-32768,  2221, 22595,  1850, 18905,-32768, 27482,-32768,  1993,
-32768,  2065,-32768,-32768,-32768,   817,  2000,  2002,-32768,-32768,
   840,   237,   169,   169,   134,   134,  2211,  2211,-32768,  1341,
   237,  1351,    77,  2153,-32768,-32768,-32768,-32768,   601,-32768,
-32768,-32768,  1355,  4074, 27482,-32768,  2007,-32768, 22595,-32768,
 22595,  1364,  4617,  1930,   949,-32768,   442, 28784,-32768,-32768,
 17717,-32768,-32768,-32768,-32768,    62,-32768,  2153,-32768,   -50,
  1365,-32768,-32768,-32768,-32768,-32768,-32768,  2147,  1368,-32768,
-32768,-32768,-32768,-32768,-32768,    40,   618,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,  2008,   237,   423,   423,-32768,   269,
 27482,-32768,  2153,   949,-32768,  2019,    40,  2022,  2021,-32768,
-32768,  2256,   115,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
  2030,-32768,-32768,-32768,-32768,-32768,-32768,  2327,  2328,-32768
};

static const short yypgoto[] = {-32768,
-32768,-32768,-32768,  2237,-32768,-32768,-32768,  1887,  1631,  1396,
-32768,  1066,   777,-32768,  1723,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1491,  1092,
   762,  1087,-32768,-32768,-32768,   471,   282,-32768, -1068,-32768,
  -901,-32768,  -950,    76, -1977,     7,   -17,    21,    -8,-32768,
-32768,-32768,-32768,   778,-32768,-32768,-32768,-32768,-32768,   374,
-32768,-32768,-32768,-32768,-32768,-32768, -1235,-32768,-32768,-32768,
-32768,   -10,-32768,-32768,-32768,-32768,  -327,   785,-32768,  1064,
  1068,-32768,-32768,  2281,  1969,  1762,-32768,  2298,-32768,  1862,
  1381,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   149,
    39,    16,-32768,-32768,   154,  1901,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,  2040,  -331,-32768,-32768,-32768,
   167,-32768,-32768,-32768,    36,-32768, -1942,-32768,-32768,-32768,
     4,-32768,-32768,-32768,  1098,-32768,-32768,-32768,-32768,-32768,
-32768,   820,-32768,-32768,-32768,  2305,-32768,-32768,  1177,-32768,
  2005,     9,-32768,    75, -1524,  1099,    11,-32768,-32768,    12,
-32768,  1523,  1102,-32768,-32768,  -493,   -90,  4679,-32768,  1223,
  1988,-32768,-32768,-32768,  1203,-32768,-32768,   889,  -344,-32768,
  -345,   184,-32768,-32768,-32768,-32768,  1547,-32768,-32768, -1450,
-32768,   925,-32768,   621,   625,  -713,-32768,-32768,    52,  -603,
-32768, -1500, -1385,  -787,  1841,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  -668,  -452,-32768,-32768,-32768,  3537,-32768,
-32768,  -315,  -605,   691,-32768,-32768,-32768,  4318, -1057,  -568,
  -670,  1046,-32768, -1158,  -927, -1152,-32768,-32768,  -884,   756,
-32768,   503,-32768,-32768,-32768,  1445,-32768,-32768,  1327,  1584,
-32768,  1245,  -976,  1575,-32768,   191,  -284,-32768, -1465,   130,
  -232,    24,  3564,-32768,  4522,  1465,    -1,     1,   -27,  -302,
  -229,  1968,   628,-32768,-32768,   -13,-32768,  2160,-32768,  1539,
  2015,-32768,-32768,  1531,  -365,   -19,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  -146,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,   773,-32768,-32768,-32768,   923,
-32768, -1767,-32768,-32768,-32768,  2074,-32768,-32768,-32768,-32768,
-32768,  1957,  1536,-32768,-32768,-32768,-32768,  1591, -1179,  1246,
  -363,-32768,-32768,-32768, -1184,-32768,-32768,-32768,   300,-32768,
-32768,  -228,  -416,  1478,  2295,  1540,-32768,   973,  -428,  -718,
  1453,  1280,   -40,   -52,-32768,   310,-32768,     2,-32768,   311,
-32768,-32768
};


#define	YYLAST		29061


static const short yytable[] = {    38,
   410,    39,    45,   305,   495,   473,   676,   450,   284,   291,
  1312,   284,   299,   302,   303,   493,   740,   452,   422,   302,
   880,   278,   302,   385,   278,  1820,   354,   850,  1257,  1846,
   290,  1807,  1566,   302,   302,   280,   486,   302,  1787,   955,
   455,   302,   302,  1734,  1114,   124,   302,   302,   302,  2168,
   136,  2160,   137,   139,  2011,   353,   398,  1536,   592,   489,
  2228,   736,   857,   431,   857,  1541,   673,  1437,  1438,  1439,
  1440,  1364,  1728,  1185,   688,  1729,   158,   300,   447,   159,
   611,  1001,   160,   865,   860,  1459,   336,   161,  1567,   162,
   163,   318,  1325,  1325,   751,   124,  1350,  1351,  1352,  1353,
   136,   832,   137,   139,  1034,  1251, -1099,    73,   769,  1252,
   678,   415,   355,   158,  1325,  1135,   159,  1848,   679,   160,
   930,   415,   931,   861,   161,  1442,   162,   163,   361,  1325,
   416,   292,   306,   616,   307,   680,    64,    73,  1355,  1253,
   416,   681,   682,   851,   311,  1524,  1318,    73,  1355,  1325,
  1525,  1393,  1368,   362,  1370,  1371,  1897,   737,  2367,   932,
   624,    70,   734,   643,   644,   741,   645,   646,   936,   647,
   648,   417,   649,  1251,   650,   995,  1206,   651,   652,   653,
   654,   478,   683,  1442,  1193,  2025,  1254,   304,  1630,  1849,
  2334,    73,  2214,   275,   418,  1631,    25,   937,  2273,  1326,
  1326,  1327,  1499,   363,   418,  -599,   684,  1253,  1442,   940,
  2395,  1615,  1060,  1072,   439,  1255,   685,   440,   356,   696,
   364,  1326,  1658,   441,   442,  1221,  -599,  2214,    75,  1659,
   479,   337,  2335,   312,    91,   617,  1326,   432,  1904,   443,
   992,  2229,   444,   716,   284,  1207,  1355,   308,   313,   272,
  2368,   434,   274,   369,  1254,   875,  1326,   278,  1200,   273,
   357,  2396,   273,   273,   738,   382,   823,   273,   302,   291,
   480,   430,   394,   396,    91,  1738,   358,   402,  1520,   291,
   302,   302,  1739,  1255,   302,   302,    87,   302,   302,  2318,
   365,   389,   302,   686,   477,  2214,   302,   302,   291,   687,
   302,  1199,   302,   302,   302,  1203,   914,   633,  1730,   302,
   302,   272,    25,   933,   274,  1017,   852,   399,    91,  2045,
  2046,   275,  2161,   275,   635,   372,  1256,  1521,   302,  1243,
  1011,   848,   849,   498,   284, -1305,   433,  1668, -1099,  1993,
 -1305,  2345,  1444,  2346,   453,   454,  1959,   278,   613,   302,
  1824,   458,   302,  1445,  1446,  1447,  1448,   506, -1264,   302,
  1830,   373,  1831, -1264,   487,  1357,   488,  1369,  1360,  1361,
   664,  1339,   391,  1735,   496, -1099,  1358,  1359,  1360,  1361,
   304,  1409,  1410,   302,  2359,  1746,  1747,  1748,  1749,  1750,
  1751,   291,   302,  2011,   273,   124,   304,  1263,   302,   506,
   136,   302,   137,   139,   302,  1340,  1135,  1135,  1135,  1135,
   501,   776,   290,  1447,  1448,  2041,  2042,   677,  1629,   302,
  2089,  1315,  2221,  2222,  1135,  1444,  1264,   445,  1638,  1639,
  1640,  1641,  1642,  1643,  2391,   302,  1445,  1446,  1447,  1448,
  1325, -1265,    25,  1222,  2215,  1060, -1265,   302,  2259,  1060,
  1060,  1060,  1060,-32768,-32768,  2219,  2220,  2221,  2222,  2309,
  1887,  1362,   691,  1357,  1635,   693,   728,   302,   695,  1325,
   302,   302,   393,  1870,  1358,  1359,  1360,  1361,  2214,  1871,
   941,  1302,   413,  1575,    25,  1888,   676,  1636,  -670,  2260,
   502,  1872,  1138,  1411,  -600,  2381,  -601,  -602,  1663,   449,
     2,  1732,   730,   302,  1442,   942,  1587,   503,  1576,  -670,
  1443,   725,  2215,    25,  1456,  -600,  -603,  -601,  -602,   459,
  2216,  2217,  2218,  2219,  2220,  2221,  2222,   482,   414,  1139,
  1826,  1140,  1919,  1620,   816,   360,   592,  -603,    64,  1870,
  1873,    25,   272,  2146,  -604,   274,   886,  1326,   890,  1951,
  2148,  1743,  1355,  1457,  1874,   827,  1355,  1872,  1577,   510,
  1875,  1245,  1449,    70,   599,  -604,  2382,  1637,  2232,   361,
  1141,  1920,   943,   424,  2139,  1578,  1326,   302,  1970,   302,
   621,   302,  1496,  1876,  2270,  1670,   863,   892,   867,   867,
   625,   302,  1458,   897,   362,  2383,  -605,  1502,   897,   897,
   309,   917,   918,   893,   955,  1665,  1873,  -606,   955,   919,
  1921,  1143,  1356,    73,   275,   310,  -607,  -605,  1922,  1676,
  1874,  2057,  1671,  2271,  1672,  1238,  1875,    25,  -606,   284,
    75,   291,  1262,   435,  1380,   504,  1336,  -607,  1385,  1387,
  1389,  1391,   278,  -608,   363,   879,  2014,   410,  2016,  1876,
   712,   302,   290,  2176,  1745,  1337,   911,   312,  2264,  2196,
  1144,   364,  2272,  1673,  -608,   909,  2197,   302,  1677,   448,
   285,   592,   313,   302,    25,  1145,  1135,  1034,  1442,  1862,
   302,   302,  1279,  1678,  1756,   969,   302,  2355,    87,  1338,
   302,   302,  1645,  1646,  2257,  2215,  2258,   302,  1972,  2302,
  1442,  1652,  2177,  2216,  2217,  2218,  2219,  2220,  2221,  2222,
  1442,  1034,  1135,  1516,    64,   928,  1917,  2178,   622,  2289,
    64,  1444,  1517,  1518,  1135,  1135,  1135,  1135,  1135,  1135,
   451,   365,  1445,  1446,  1447,  1448,  2283,  2356,   437,    70,
  2357,  1723,  1355,  1392,   977,    70,  -609,  1900,  1901,  1902,
   981,  1002,  1003,  1004,   982,   983,   438,  -610,  1012,  1235,
  1236,  2089,   272,   286,  1034,   274,   275,  -609,  1587,  1357,
  1019,   921,   460,-32768,   922,  1913,  1914,  1915,  -610,  1753,
  1358,  1359,  1360,  1361,  1358,  1359,  1360,  1361,  1060,    73,
   302,   816,  1060,   461,  1060,    73,  -611,  1060,  1060,  1060,
  1060,  1060,  1060,  1060,  1060,  1442,    75,  1060,   873,  -612,
   876,  2107,    75,   302, -1243,   302,   302,  -611,  -613, -1243,
  1616,  1375,   885,   302,  2171,   494,  1073,  -614,  1074,   832,
  -612,  -615,  -616,   823,  1442,  2162,   476,  2163,   823,  -613,
   834,   678,   823,   507,  -617,  1725,   823,  -618,  -614,   679,
   823,   508,  -615,  -616,   823,  1966,  1967,  1968,   823,  2105,
  -619,  2172,   823,  2173,    87,  -617,   680,   302,  -618,   302,
    87,   291,   681,   682,   272,  2214,  -620,   274,  -621,  -622,
  1195,  -619,  2019,  2328,  2020,  2021,  2022,  2023,  2024,   835,
  1224,  1227,   290,  1442,   291,  1444,   836,  -620,  2214,  -621,
  -622,  1961,  2174,    38,   837,    39,  1445,  1446,  1447,  1448,
   509,  -623,   978,   683,  2096,   838,    91,-32768,   511,  1244,
  1246,  -624,    91,    25,  -625,  -626,   302,  1444,  1445,  1446,
  1447,  1448,  -623,   302,  1303,  -627,   512,   684,  1445,  1446,
  1447,  1448,  -624,  1828,   302,  -625,  -626,   685,  -628,  1011,
   302,   497,   275,   832,   762,   302,  -627,  1135,  1281,  1357,
  1281,  1281,  1281,  1281,   834,   493,   493,   614,  1589,  -628,
  1358,  1359,  1360,  1361,  -629,   615,  1905,  1667,   731,   763,
  2193,  1675,  1537,  1680,   732,  1538,  1685,  1690,  1695,  1700,
  1705,  1710,  1715,  1720,  -661,  -629,  1724,   618,   764,   208,
  -598,   840,  1618,  2209,  2210,  2211,  2212,  2213,  -659,   619,
  1624,  2152,   704,   835,  2153,  -661,   705,    25,   841,    26,
   836,  -598,  1444,  -597,   686,  1273, -1242,  1274,-32768,  -659,
   687, -1242,   218,  1445,  1446,  1447,  1448,  1135,  2154,  1506,
  1507,   620,   599,  1571,  -597, -1245,   842,  1574,   273,  2236,
 -1245,  1444,  1376,   627,   843,   844,   845,   846,   847,   848,
   849,   765,  1445,  1446,  1447,  1448,   960,  2164,   900,   924,
   925,   961,  1060,   628,   901,  2167,   902,   926,   903,  2175,
  1997,  2179,   962,  1999,  2180,  2181,  2182,  2183,  2184,  2185,
  2186,  2187,  2215,  2190,   963,   964,   816,  1259,  1260,  1261,
  2216,  2217,  2218,  2219,  2220,  2221,  2222,  -899,   818,  2097,
  1444,   304,  2204,  -899,  1792,-32768,   629,  -906,  2101,   965,
  1793,  1445,  1446,  1447,  1448,   840,  2219,  2220,  2221,  2222,
   832,   630,  1794, -1247,  1393, -1218,   833,  2286, -1247, -1198,
 -1218,   834,   841,   631,   302,  1395,  2291,  2292,  2293,  2294,
  2295,  2296,  2297,  2298,  2301,  1796,   302,   634,   317,   302,
 -1219,   302,  1413,   352,  1301, -1219,   638,  1414,  1797,   371,
   842,   302,   636,  2140,   386,  1816,  1817,   599,   302,  1526,
  1509,   846,   847,   848,   849,    25,  1225, -1256,  1808,   640,
   835, -1258, -1256, -1099,  1396,   642, -1258,   836,  1135,   667,
   158,  1397, -1217,   159,   832,   837,   160, -1217,   670,-32768,
  1299,   161,  1300,   162,   163,   302,   838,   302,   302,   302,
   302,   672,   272,   273,   839,   274,   302,  2275,    25,   692,
    26,    38,   302,    39,  1528,  1514,   302,  2166,  1313, -1233,
  1314,  1569,  1514,   200, -1233,   302, -1231,  1547,  1547,  1548,
  1548, -1231,   697,   816, -1262,   302,   302,   302,   302, -1262,
   699,  1925,  2366,   302,  1330,   302,  1331,   302,   202,  1598,
  1598,   836,   302, -1257,  1344,   701,  1345,  1580, -1257,   832,
   816,   302,   302,  1582,  1561,  1583,  1543,   207,   208,  1544,
   834,  1584,    25,  1585,    26,  1281,  1281,  1597,  1597,  1393,
 -1220,  1892,   840,  -899,   703, -1220,  1401,  -906, -1260,  1581,
  1545, -1240,  1546, -1260,   816, -1244, -1240,  1302,   707,   841,
 -1244,   218,   852,  1402,  1613,  2323,   708,  1607,  1625,  1609,
  1006,  1007,  2149,  1280,   713,  1282,  1283,  1284,  1285,   835,
 -1246,   710,  1628, -1248, -1249, -1246,   836,   842, -1248, -1249,
   223,  1403,   816,   714,   837,   843,   844,   845,   846,   847,
   848,   849,  1407,  1408,  1409,  1410,  1397,  1392,  1814,   717,
  1815,  1744,  1392,  1654,  1655,  1495,   840,  1392,   718, -1250,
 -1251, -1255,  1392,  2133, -1250, -1251, -1255,  1392,  1135,   719,
   873, -1261,  1392,   841, -1259, -1263, -1261,  1392,   721, -1259,
 -1263,   816,  1392,  1415, -1239,   832,   722, -1241, -1252, -1239,
 -1253,   833, -1241, -1252,    25, -1253,   834, -1254,  1791,   816,
   727,-32768, -1254,   898,   899,  1733,  2290,  2025,   275,  1416,
  1736,  1737,   846,   847,   848,   849,   748,   749, -1266, -1268,
  2307,  2308,   752, -1266, -1268,   302,  1792,   753,   302, -1275,
   754,   840,  1793,   302, -1275,  2201,   302,  1565, -1273,   768,
  -572, -1274, -1269, -1273,  1794,   835, -1274, -1269,   841, -1270,
 -1272,  1401,   836,  1795, -1270, -1272, -1271, -1232,  2317, -1267,
   837, -1271, -1232,    25, -1267,    26,   757,  1796,  1402,   755,
   302,   838, -1284, -1276,  2029,   758,   842, -1284, -1276,   839,
  1797,   283,   283,   818,   283,-32768,-32768,   846,   847,   848,
   849,   759,  1582,   302,  1847,   816,-32768, -1291,  1227,  1227,
  1224, -1283, -1291,   760,   828,   771, -1283,  1407,  1408,  1409,
  1410,   772,   774, -1289, -1281,  1827,  1829,   775, -1289, -1281,
  2365,   302,   613,   302,  1313, -1290,  2047,    49,  2052,   302,
 -1290,    50, -1282,   778,    51,    52,   830, -1282,    53, -1285,
  1417,  1885,   831,  1313, -1285,  2191,    54,    55,  1418,  1419,
  1420,  1421,  1422,  1423,  1424,   592,  1313,   840,  2192, -1277,
  1543,    56,    57,  1544, -1277, -1286,    25, -1278,    26,  1580,
 -1286,  1883, -1278, -1288,   841, -1280, -1287, -1279, -1288,  2033,
 -1280, -1287, -1279,   854,  1545,   852,  1550,  2233,    58,   855,
   856,   272,   286,    59,   274,   275,  2034,  2276,  2319,  2277,
  2320,   864,   842,    60,   877,   881,   883,    61,   816,    62,
   843,   844,   845,   846,   847,   848,   849,   884,  2331,    63,
  2332,    64,   891,   895,-32768,    65,  1895,    66,  1313,    67,
  2333,   908,   721,    68,  2340,  2039,  2040,  2041,  2042,    69,
   491,  2319,  2371,  2347,  2372,  2374,    70,  2375,   465,   466,
   467,   468,   469,   470,   471,  2025,  1785,   905,   906,  1495,
   907,  2026,   910,     2,   912,   915,  2027,   927, -1215,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    12,   929,
    13,    14,    15,    16,    17,    18,    19,    20,    21,   935,
   946,    71,   338,    72,   816,  2203,    73,    74,   958,   967,
   974,   975,   984,   985,   986,   991,   987,   339,   996,   992,
  -899,   998,   340,    75,    73,  2028,   428,   283,  1013,   341,
   342,  1014,  2029,   343,  1036,  1015,  1016,    76,    77,   302,
  2030,   776,   832,  1791,   344,  1069,    78,    79,  1070,  1137,
   302,  2031,   345,  1071,  1185,   346,    80,    81,  1205,  2032,
  1202,  1210,  1214,   302,  1219,  1212,  1228,  1247,    82,    83,
    84,  1792,    85,  1241,  1393,    86,  1242,  1793,   347,  1229,
   348,    87,  1252,  1267,  1268,  1395,   349,  1269,   350,  1794,
    88,  1270,  1272,   302,   302,   283,  1278,    89,  1795,   302,
   291,  1275,  1295,  1296,    90,  1297,  1305,  1304,  1298,  1306,
   302,  1322,  1796,  1323,  1328,  1329,  1332,   283,  1333,  1334,
   302,  1341,  2058,  2364,   302,  1797,  1335,  2008,  1342,  1346,
  1367,  1347,  1348,    91,  1396,   302,  1349,  2033,  1429,  1497,
  1354,  1397,  1366,  2084,  1991,  1373,   816,  1363,  1487,  1398,
  1374,  1427,  1510,  1512,  2034,   816,  1377,  1378,  1381,  1504,
   744,  1382,  1383,  2050,  1412,  1511,  1430,  1431,  1432,  1522,
  1433,  1434,   816,  1435,   283,  1436,  1523,  1450,  1460,   283,
  1462,  2279,  2035,  1463,  2064,  1465,  1531,  1466,  1468,  1469,
  2036,  2037,  2038,  2039,  2040,  2041,  2042,  1471,  2025,  1498,
  1472,  1552,  1474,  1475,  1477,  1553,  2043,  1478,  1480,  2027,
   816,  1481,  1483,  1484,    22,  1540,  1551,    23,  1529,    24,
    25,  1530,    26,   816,    27,  1542,  1554,  1562,  1572,    28,
  1602,  1570,  1604,    29,   870,  1611,    30,    31,    32,    33,
    34,    35,    36,   302,  1619,  1355,  1401,  1647,  2159,  1649,
   302,  1393,  1650,  1651,  1664,  1669,  1727,  1726,  2028,  1442,
  1754,  1755,  1763,  1402,  1766,  2029,  1769,  1772,  2151,  1775,
  1495,  1778,  1781,-32768,   816,  1784,   816,  1790,  1803,  1804,
  1809,  1811,  1812,  1813,  1814,  1850,  1823,  1825,  1851,  1852,
  1844,  1403,  1845,  1855,  1860,  1857,  1861,  2155,   746,  2156,
-32768,-32768,  1407,  1408,  1409,  1410,   279,  1864,  1890,   279,
   302,   301,  1868,  1881,  1893,  2195,  1886,   316,   832,  1891,
   301,  1894,  1896,  1898,   833,  1899,  1903,  1907,  1908,   834,
  1923,   379,   383,  1909,  1910,   388,  1911,  1994,  1912,   383,
   383,  2060,   302,  1916,   383,   405,   408,  1926,  1927,  1929,
   858,  1930,  1932,  1933,  1935,  1936,  1938,  1939,  1941,  1995,
  1942,  1944,  1945,  1947,  1948,  1952,  1950,  1962,  1954,  1958,
  2033,   599,   746,  1963,  1960,  1964,  2001,  2003,   835,  1965,
  1969,  1974,  1973,   318,  1975,   836,  1976,  2034,  1977,  2004,
  1030,  1978,  1979,   837,   302,   319,   302,   320,  1980,  1981,
  1982,  1983,   321,  2006,   838,  1984,  1985,  1986,  2007,   322,
   323,  1987,   283,   324,   283,  2035,  1988,  1989,  1990,   816,
  1992,  2015,  2009,  2017,   325,   816,  2039,  2040,  2041,  2042,
  2018,  2044,   326,  2048,  1062,  -343,  2049,  2053,  2054,  -906,
  2056,  2062,    38,   291,    39,  2155,  2061,  2156,   302,  2063,
  1131,   302,  2081,  2080,  2092,  2098,  2093,  2099,   327,  2100,
  -261,  2095,  2281,   816,  2102,  2103,   328,  2104,   329,  2106,
  2145,  2111,  2147,   302,  1495,   330,   302,   816,   816,  2312,
   302,  1495,  2025,  2114,  2117,  2200,  2199,  2120,  2123,  2126,
   840,  2129,  2267,  2170,  2025,  2287,  2288,  2132,  2134,  2136,
  2026,  2137,  2141,  2142,  2206,  2027,  2143,   841,  2194,  2202,
  2207,  2208,  2280,  2226,  2223,   816,   816,  2234,  2224,  2227,
  2235,  2231,  2274,   816,  2237,  2238,  2239,   302,  2240,  2241,
  2242,  2243,  2244,  2245,  2246,   842,  2247,  2248,  2249,  2250,
  2251,  2252,   279,   843,   844,   845,   846,   847,   848,   849,
    38,  2253,    39,  1528,  2028,  2254,   410,  2278,  2312,  2214,
  1068,  2029,  2255,  2256,   302,  2282,   383,  2284,   816,  2030,
   816,  1313,  2303,  2315,  2354,  2321,  1136,   816,   301,   301,
  2031,  2325,   279,   457,  2326,   301,   383,  2353,  2032,  2329,
   475,  2330,  2336,  1183,   383,   301,  2344,  2380,   301,  1196,
   301,   279,   457,   832,  2392,  2390,  2394,   301,   301,   833,
  2393,  1321,  2397,  2351,   834,  2352,  2399,  2400,   412,  2312,
   302,  1564,   939,  2354,  1266,  1495,   383,  1495,   702,   868,
  1854,  1180,   279,  1863,  1557,  2205,  2353,  1555,  2083,  2322,
  1030,  1030,  1030,  1030,  2373,  2388,  1859,   383,  2369,  2385,
   388,  1866,  1601,  2389,   283,  1600,   401,   383,   370,   626,
   735,  1316,  2351,   835,  2352,  2306,  2033,  1062,  2370,  2305,
   836,  1062,  1062,  1062,  1062,  2386,   492,   882,   837,  1858,
   726,   665,  2379,  2034,   409,  1519,   623,  2025,  2348,   838,
   674,  1209,  1486,  2026,  1606,   639,   301,   839,  2027,   301,
  1818,  1614,   301,  1789,  2313,   814,  1515,  1201,  2000,  1998,
  1956,  2035,  2387,  1657,  1918,  2108,  1184,   383,  2002,  2036,
  2037,  2038,  2039,  2040,  2041,  2042,  1183,  1319,  2339,  1194,
   427,   669,  1239,   388,  1501,  2165,  1237,  1819,   610,  1505,
  1240,  1131,  1131,  1131,  1131,   301,   729,  2028,   421,  1786,
  2265,  1307,  1488,  2262,  2029,  1204,     0,  2263,  1250,  1131,
     0,     0,  2030,     0,     0,   301,     0,     0,   301,   742,
     0,     0,     0,  2031,     0,   840,     0,     0,     0,     0,
     0,  2032,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   841,  1068,     0,     0,     0,  1068,  1068,  1068,
  1068,   747,     0,     0,     0,     0,     0,     0,     0,     0,
   858,     0,     0,   832,     0,     0,   870,     0,     0,   833,
   842,     0,     0,     0,   834,     0,     0,     0,   843,   844,
   845,   846,   847,   848,   849,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    12,     0,    13,    14,    15,    16,
    17,    18,    19,    20,    21,     0,     0,     0,     0,  2033,
     0,     0,     0,     0,     0,     0,     0,  1136,  1136,  1136,
  1136,     0,     0,   835,     0,   874,  2034,   383,     0,   301,
   836,     0,     0,     0,     0,  1136,     0,     0,   837,   383,
     0,  1455,     0,     0,     0,     0,     0,     0,     0,   838,
     0,     0,     0,     0,  2035,     0,     0,   839,     0,     0,
     0,     0,  2036,  2037,  2038,  2039,  2040,  2041,  2042,     0,
     0,     0,     0,     0,     0,   858,     0,   279,  2285,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   301,
     0,     0,     0,     0,   746,     0,     0,     0,     0,     0,
     0,     0,  1030,  1030,     0,   957,     0,     0,     0,     0,
     0,   475,  1030,  1030,  1030,  1030,  1030,  1030,   301,   383,
     0,     0,     0,     0,   301,   840,     0,     0,   301,   301,
     0,     0,     0,     0,     0,   990,     0,  1030,     0,     0,
     0,     0,   841,     0,     0,     0,     0,     0,     0,     0,
  1062,  1131,     0,     0,  1062,     0,  1062,     0,     0,  1062,
  1062,  1062,  1062,  1062,  1062,  1062,  1062,     0,     0,  1062,
   842,  1033,     0,     0,     0,     0,     0,     0,   843,   844,
   845,   846,   847,   848,   849,   832,     0,  1131,     0,     0,
  1030,   833,  -699,     0,  1215,     0,   834,     0,     0,  1131,
  1131,  1131,  1131,  1131,  1131,     0,     0,     0,     0,     0,
    22,     0,     0,    23,     0,  1065,    25,     0,    26,     0,
    27,     0,     0,     0,  1216,    28,     0,     0,  1112,     0,
     0,  1134,    30,    31,    32,    33,    34,     0,  1527,     0,
     0,     0,     0,     0,     0,   835,     0,     0,     0,     0,
     0,  1190,   836,  1190,   301,     0,     0,     0,     0,     0,
   837,  1198,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   838,     0,     0,     0,     0,  1068,  1136,     0,   839,
  1068,     0,  1068,     0,     0,  1068,  1068,  1068,  1068,  1068,
  1068,  1068,  1068,     0,     0,  1068,     0,     0,     0,     0,
     0,     0,     0,   832,     0,   665,     0,   279,     0,   833,
     0,     0,     0,  1136,   834,     0,     0,  1217,     0,     0,
     0,     0,     0,     0,     0,  1136,  1136,  1136,  1136,  1136,
  1136,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    12,     0,    13,    14,    15,    16,    17,    18,    19,    20,
    21,     0,     0,     0,  1455,     0,     0,   840,     0,     0,
     0,     0,     0,   835,  1294,     0,     0,     0,     0,  1183,
   836,   383,     0,     0,   841,     0,  1393,     0,   837,     0,
     0,     0,  1311,     0,     0,     0,     0,  1395,  1311,   838,
     0,     0,     0,  1190,     0,     0,     0,   839,     0,     0,
     0,     0,   842,     0,     0,     0,     0,     0,     0,     0,
   843,   844,   845,   846,   847,   848,   849,     0,     0,     0,
     0,  1033,  1033,  1033,  1033,     0,     0,     0,     0,     0,
     0,     0,  1131,     0,  1062,     0,  1396,     0,     0,     0,
     0,     0,     0,  1397,     0,  1455,     0,     0,  1065,     0,
     0,  1398,  1065,  1065,  1065,  1065,     0,     0,     0,     0,
     0,     0,  1399,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   840,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1393,     0,     0,
     0,     0,   841,  1394,     0,     0,     0,     0,  1395,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1131,     0,     0,     0,     0,     0,     0,     0,
   842,     0,  1134,  1134,  1134,  1134,     0,     0,   843,   844,
   845,   846,   847,   848,   849,     0,     0,     0,     0,     0,
  1134,     0,  1343,     0,     0,     0,     0,  1396,  1401,     0,
     0,     0,     0,     0,  1397,     0,     0,     0,  1136,     0,
  1068,     0,  1398,  1455,     0,  1402,    22,     0,     0,    23,
     0,     0,    25,  1399,    26,     0,    27,     0,     0,     0,
     0,    28,   383,     0,     0,     0,     0,     0,    30,    31,
    32,    33,    34,  1403,  1500,     0,     0,   874,     0,  1190,
     0,  1404,  1405,  1406,  1407,  1408,  1409,  1410,     0,   301,
     0,     0,     0,     0,     0,     0,   301,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    12,     0,    13,    14,
    15,    16,    17,    18,    19,    20,    21,     0,  1136,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1455,
     0,     0,     0,  1556,     0,  1556,  1558,  1559,   301,  1401,
     0,     0,     0,     0,   383,     0,     0,     0,     0,     0,
   957,     0,     0,  1131,   957,   832,  1402,     0,     0,     0,
     0,   833,     0,   301,     0,     0,   834,     0,     0,     0,
     0,     0,     0,   475,  1599,  1599,   475,     0,     0,     0,
     0,   301,     0,   301,  1403,  1610,     0,     0,     0,     0,
   990,     0,  1404,  1405,  1406,  1407,  1408,  1409,  1410,  1621,
  1622,     0,     0,     0,     0,     0,     0,  1455,     0,  1455,
  1455,  1455,  1455,  1455,     0,   835,     0,     0,     0,     0,
     0,     0,   836,  1033,  1033,     0,     0,     0,     0,     0,
   837,     0,     0,  1033,  1033,  1033,  1033,  1033,  1033,     0,
     0,   838,     0,     0,     0,     0,  1455,     0,     0,   839,
     0,     0,     0,     0,     0,     0,     0,     0,  1033,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1065,  1134,     0,     0,  1065,     0,  1065,     0,  1136,
  1065,  1065,  1065,  1065,  1065,  1065,  1065,  1065,     0,     0,
  1065,     0,     0,     0,     0,     0,     0,  1455,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1134,     0,
     0,  1033,     0,     0,     0,     0,     0,     0,     0,     0,
  1134,  1134,  1134,  1134,  1134,  1134,     0,   840,     0,     0,
  2025,     0,    22,     0,     0,    23,     0,     0,    25,     0,
    26,  2027,    27,  1131,   841,     0,     0,    28,  1455,     0,
     0,     0,     0,     0,    30,    31,    32,    33,    34,     0,
  2316,     0,     0,   383,     0,     0,   383,     0,     0,     0,
     0,  1802,   842,     0,  1190,     0,     0,     0,     0,     0,
   843,   844,   845,   846,   847,   848,   849,     0,     0,     0,
  2028,     0,  1455,     0,  -698,     0,   832,  2029,     0,     0,
  1455,  1455,   833,     0,  1455,  2030,  1455,   834,  1821,  1455,
  1455,  1455,  1455,  1455,  1455,  1455,  1455,     0,  1455,     0,
     0,     0,     0,  1455,     0,     0,     0,     0,  1843,   832,
     0,   990,     0,     0,     0,   833,     0,  1455,     0,     0,
   834,     0,     0,     0,     0,     0,  1455,  1455,  1455,  1455,
  1455,     0,     0,     0,     0,     0,   835,     0,     0,  1556,
     0,  1867,     0,   836,     0,     0,     0,  1882,     0,  1136,
     0,   837,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   838,  1372,     0,     0,     0,     0,     0,   835,
   839,     0,     0,     0,     0,   276,   836,     0,   276,     0,
   276,     0,  2033,     0,   837,     0,   276,     0,     0,   276,
     0,     0,     0,     0,  1617,   838,     0,     0,     0,  2034,
   276,   276,     0,   839,   276,     0,     0,     0,   276,   276,
     0,     0,     0,   276,   276,   276,     0,     0,     0,     0,
     0,     0,     0,     0,  1455,     0,     0,  2035,     0,     0,
  1455,     0,  1455,  1134,     0,  1065,-32768,-32768,  2039,  2040,
  2041,  2042,     0,     0,     0,     0,     0,     0,   840,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1455,     0,     0,     0,     0,   841,     0,     0,     0,  1455,
  1455,  1455,  1455,  1455,  1455,  1455,  1455,  1455,     0,     0,
     0,   840,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   842,     0,     0,     0,     0,   841,     0,
     0,   843,   844,   845,   846,   847,   848,   849,     0,     0,
     0,     0,     0,  1134,   832,     0,     0,     0,     0,     0,
   833,  1455,  1906,     0,     0,   834,   842,     0,  1455,     0,
  1455,     0,     0,     0,   843,   844,   845,   846,   847,   848,
   849,     0,     0,     0,     0,     0,     0,   383,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1112,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   301,     0,  1455,   835,  1455,     0,     0,     0,     0,
     0,   836,     0,     0,     0,  1455,     0,     0,     0,   837,
  1843,     0,  1843,  1843,  1843,  1843,  1843,     0,     0,     0,
   838,   990,   990,     0,     0,     0,     0,   301,   839,     0,
     0,   276,     0,     0,     0,     0,     0,     0,   383,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   301,   457,
     0,     0,  2082,     0,     0,   276,     0,     0,     0,     0,
     0,     0,     0,  2091,     0,     0,     0,   276,   276,     0,
     0,   276,   276,     0,   276,   276,     0,     0,     0,   276,
     0,     0,     0,   276,   276,     0,     0,   276,     0,   276,
   276,   276,     0,     0,  1134,     0,   276,   276,     0,   456,
     0,     0,     0,     0,     0,     0,   840,     0,  2025,     0,
     0,     0,     0,     0,  2026,   276,  2266,     0,   490,  2027,
     0,   276,     0,   841,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   276,     0,     0,   276,
     0,     0,     0,     0,     0,     0,   276,     0,     0,     0,
     0,   842,     0,     0,     0,     0,     0,     0,     0,   843,
   844,   845,   846,   847,   848,   849,     0,     0,  2028,     0,
   276,   383,     0,     0,     0,  2029,     0,     0,   383,   276,
     0,     0,     0,  2030,     0,   276,     0,     0,   276,     0,
     0,   276,     0,     0,  2031,  1843,     0,     0,     0,     0,
     0,     0,  2032,  1843,     0,     0,   276,  1843,     0,  1843,
     0,     0,  1843,  1843,  1843,  1843,  1843,  1843,  1843,  1843,
     0,  1843,   276,     0,     0,     0,   457,     0,     0,     0,
     0,     0,     0,     0,   276,     0,     0,     0,   301,     0,
  1843,     0,     0,     0,     0,     0,     0,     0,     0,   457,
   457,   457,   457,   457,   276,     0,     0,   276,   276,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   990,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2033,     0,     0,     0,  1134,     0,     0,     0,     0,     0,
   276,     0,     0,     0,     0,     0,     0,  2034,     0,     0,
     0,     0,     0,     0,     0,   766,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   815,   383,     0,   383,  2035,     0,     0,     0,     0,
     0,     0,     0,  2036,  2037,  2038,  2039,  2040,  2041,  2042,
     0,     0,   832,     0,     0,     0,     0,     0,   833,     0,
     0,     0,     0,   834,     0,  1843,     0,     0,     0,     0,
     0,     0,     0,     0,   276,     0,   276,     0,   276,     0,
     0,     0,     0,     0,     0,     0,   301,     0,   276,  1190,
     0,     0,     0,   457,     0,     0,     0,     0,     0,     0,
     0,     0,   457,   457,   457,   457,   457,   457,   457,   457,
   457,   990,   835,     0,  1190,     0,     0,     0,   301,   836,
     0,     0,     0,     0,     0,     0,   276,   837,     0,    50,
     0,     0,    51,    52,     0,     0,    53,     0,   838,     0,
     0,     0,     0,     0,    54,    55,   839,     0,   276,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,    56,
    57,     0,     0,  1843,   276,  2324,     0,     0,     0,     0,
   276,     0,     0,     0,     0,     0,     0,   276,   276,  1393,
     0,     0,     0,   276,     0,  1394,    58,   276,   276,     0,
  1395,   411,     0,     0,   276,     0,     0,     0,     0,     0,
     0,    60,  2343,     0,     0,    61,     0,    62,     0,     0,
     0,     0,     0,     0,     0,  1008,     0,    63,   457,    64,
     0,     0,     0,    65,   840,    66,     0,    67,     0,     0,
     0,    68,     0,     0,     0,     0,     0,    69,     0,  1396,
     0,   841,     0,     0,    70,     0,  1397,     0,     0,     0,
     0,     0,     0,     0,  1398,     0,     0,  1031,  2343,     0,
     0,     0,     0,     0,     0,  1399,     0,     0,     0,   842,
     0,     0,     0,  1400,     0,     0,     0,   843,   844,   845,
   846,   847,   848,   849,     0,     0,     0,   276,   815,    71,
     0,    72,     0,     0,    73,    74,     0,     0,     0,     0,
     0,  1063,     0,     0,     0,     0,     0,     0,     0,     0,
   276,    75,   276,   276,     0,     0,     0,  1132,     0,     0,
   276,     0,     0,     0,     0,    76,    77,     0,     0,     0,
     0,     0,     0,     0,    78,    79,     0,     0,     0,     0,
     0,     0,     0,     0,    80,    81,     0,     0,     0,     0,
     0,  1401,     0,     0,     0,     0,    82,    83,    84,     0,
    85,     0,     0,    86,   276,     0,   276,     0,  1402,    87,
     0,     0,     0,     0,     0,     0,     0,     0,    88,     0,
     0,     0,     0,     0,     0,    89,     0,     0,     0,     0,
     0,     0,    90,     0,     0,     0,  1403,     0,     0,     0,
     0,     0,     0,     0,  1404,  1405,  1406,  1407,  1408,  1409,
  1410,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,    91,     0,   276,     0,     0,     0,     0,     0,     0,
   276,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   276,     0,     0,     0,     0,     0,   276,     0,     0,
     0,     0,   276,     0,   779,   780,   781,   782,   783,   784,
   785,   786,   787,     0,   788,     0,   789,   790,   791,   792,
   793,   794,   795,   796,   797,   798,     0,   799,     0,   800,
   801,   802,   803,   804,     0,   805,   806,   807,   808,   809,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1031,  1031,  1031,
  1031,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1063,     0,     0,     0,  1063,  1063,
  1063,  1063,     0,     0,   200,   553,     0,     0,     0,     0,
     0,   810,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,     0,     0,     0,   559,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   207,   208,
     0,     0,     0,   815,     0,  2025,     0,     0,     0,   565,
     0,  2026,     0,     0,     0,     0,  2027,     0,  1132,  1132,
  1132,  1132,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   218,     0,     0,     0,  1132,     0,     0,   811,
   812,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   276,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   223,     0,   276,   813,  2028,   276,     0,   276,     0,
     0,     0,  2029,     0,     0,     0,     0,     0,   276,     0,
  2030,     0,     0,     0,     0,   276,     0,     0,     0,     0,
     0,  2031,     0,     0,     0,     0,     0,     0,     0,  2032,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   276,     0,   276,   276,   276,   276,     0,     0,
     0,     0,     0,   276,     0,   272,     0,     0,   274,   276,
     0,     0,     0,   276,     0,     0,     0,     0,     0,     0,
     0,     0,   276,     0,     0,     0,     0,     0,     0,     0,
   815,     0,   276,   276,   276,   276,     0,     0,     0,     0,
   276,     0,   276,     0,   276,     0,     0,  2033,     0,   276,
     0,  2025,     0,     0,     0,     0,     0,   815,   276,   276,
     0,     0,  2027,     0,  2034,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2025,     0,     0,     0,     0,   770,
  2026,     0,     0,     0,     0,  2027,     0,     0,     0,     0,
     0,   815,  2035,   817,     0,   820,     0,   821,   822,   826,
  2036,  2037,  2038,  2039,  2040,  2041,  2042,     0,     0,  1031,
  1031,  2028,     0,     0,  1008,     0,     0,   832,  2029,  1031,
  1031,  1031,  1031,  1031,  1031,     0,  2030,     0,   834,   815,
     0,     0,     0,     0,  2028,     0,     0,  2031,     0,     0,
     0,  2029,     0,     0,  1031,     0,     0,     0,     0,  2030,
     0,     0,     0,     0,     0,   889,     0,  1063,  1132,     0,
  2031,  1063,     0,  1063,  1393,     0,  1063,  1063,  1063,  1063,
  1063,  1063,  1063,  1063,     0,  1395,  1063,   835,   815,     0,
     0,     0,     0,     0,   836,     0,     0,     0,     0,     0,
     0,     0,   837,     0,  1132,     0,   815,  1031,     0,     0,
     0,     0,     0,     0,     0,     0,  1132,  1132,  1132,  1132,
  1132,  1132,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   276,  2033,  1396,   276,     0,     0,     0,     0,
   276,  1397,     0,   276,     0,     0,   976,     0,     0,  1398,
  2034,     0,     0,     0,     0,     0,  2033,     0,  2025,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  2027,
     0,     0,     0,  2034,     0,   997,     0,   276,  2035,     0,
     0,     0,     0,     0,     0,     0,  2036,  2037,  2038,  2039,
  2040,  2041,  2042,     0,     0,     0,     0,     0,  1018,   840,
   276,  2035,   815,     0,     0,  1041,     0,     0,     0,  2036,
  2037,  2038,  2039,  2040,  2041,  2042,   841,     0,  2028,     0,
     0,     0,     0,     0,  1841,  2029,     0,     0,   276,     0,
   276,     0,     0,  2030,     0,     0,   276,     0,     0,     0,
     0,     0,     0,     0,   842,  1061,  1401,     0,     0,     0,
     0,     0,   843,   844,   845,   846,   847,   848,   849,     0,
  1115,     0,     0,  1402,  1142,     0,  1146,     0,     0,  1150,
  1155,  1159,  1163,  1167,  1171,  1175,  1179,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1403,     0,     0,     0,     0,     0,     0,     0,  1404,
  1405,  1406,  1407,  1408,  1409,  1410,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   815,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2033,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2034,     0,  1132,
     0,  1063,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  2035,     0,     0,     0,     0,
     0,     0,     0,  2036,  2037,  2038,  2039,  2040,  2041,  2042,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   815,     0,     0,     0,  1032,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1317,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1132,
     0,     0,     0,     0,     0,     0,   276,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   276,     0,  1064,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   276,     0,     0,     0,  1041,  1133,  1041,  1041,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1379,     0,
     0,     0,  1384,  1386,  1388,  1390,   826,     0,     0,     0,
   276,   276,     0,     0,     0,     0,   276,     0,  1426,     0,
     0,     0,     0,     0,     0,     0,  1841,   276,  1841,  1841,
  1841,  1841,  1841,     0,     0,     0,     0,   276,     0,     0,
     0,   276,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   276,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   815,     0,  2078,  1005,     0,     0,     0,
     0,     0,   815,     0,     0,     0,  1441,     0,     0,  2090,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   815,
     0,     0,  1461,     0,     0,     0,   826,     0,     0,     0,
     0,   826,     0,     0,     0,   826,     0,     0,     0,   826,
  1132,     0,     0,   826,     0,     0,     0,   826,     0,     0,
     0,   826,     0,     0,     0,   826,     0,   815,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   815,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   276,   889,     0,     0,     0,     0,     0,   276,     0,     0,
     0,     0,     0,     0,     0,  1032,  1032,  1032,  1032,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   815,     0,   815,     0,     0,     0,     0,     0,     0,
     0,     0,  1064,     0,     0,     0,  1064,  1064,  1064,  1064,
     0,  1841,     0,     0,     0,     0,     0,     0,     0,  1841,
     0,     0,     0,  1841,     0,  1841,     0,   276,  1841,  1841,
  1841,  1841,  1841,  1841,  1841,  1841,     0,  1841,     0,     0,
     0,     0,  2078,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1841,     0,     0,   276,
     0,     0,     0,     0,     0,  2078,  2078,  2078,  2078,  2078,
     0,     0,  1623,     0,     0,     0,  1133,  1133,  1133,  1133,
     0,  1626,     0,     0,     0,     0,     0,     0,  1454,     0,
  1627,     0,     0,     0,  1133,     0,  1041,     0,     0,     0,
  1132,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   276,     0,   276,     0,     0,     0,     0,     0,     0,
     0,  1653,     0,     0,     0,     0,     0,     0,     0,     0,
  1041,     0,     0,     0,     0,     0,   815,     0,     0,     0,
     0,  1666,   815,     0,     0,  1674,     0,  1679,     0,     0,
  1684,  1689,  1694,  1699,  1704,  1709,  1714,  1719,     0,     0,
  1061,     0,     0,     0,     0,   276,     0,     0,   276,     0,
     0,  1841,     0,     0,     0,     0,     0,     0,     0,     0,
   815,     0,     0,  1041,     0,     0,     0,     0,     0,     0,
   276,     0,     0,   276,   815,   815,     0,   276,     0,  2078,
     0,     0,     0,     0,     0,     0,     0,     0,  2078,  2078,
  2078,  2078,  2078,  2078,  2078,  2078,  2078,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   815,   815,     0,     0,     0,     0,     0,     0,
   815,     0,     0,     0,   276,  1451,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1464,     0,     0,
     0,     0,  1467,     0,     0,     0,  1470,     0,     0,  1841,
  1473,     0,     0,     0,  1476,     0,     0,     0,  1479,     0,
     0,   276,  1482,     0,     0,   815,  1485,   815,     0,     0,
     0,     0,     0,     0,   815,     0,     0,  1032,  1032,     0,
     0,     0,     0,     0,     0,     0,     0,  1032,  1032,  1032,
  1032,  1032,  1032,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2078,     0,     0,     0,     0,     0,
     0,     0,  1032,     0,     0,     0,     0,   276,     0,     0,
     0,     0,     0,     0,     0,  1064,  1133,     0,     0,  1064,
     0,  1064,     0,     0,  1064,  1064,  1064,  1064,  1064,  1064,
  1064,  1064,     0,     0,  1064,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1041,
  1041,  1041,  1133,     0,     0,  1032,     0,     0,     0,     0,
     0,     0,     0,     0,  1133,  1133,  1133,  1133,  1133,  1133,
     0,     0,     0,  1588,     0,     0,     0,  1041,  1041,  1041,
     0,  1760,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1924,     0,     0,     0,     0,
   826,     0,     0,     0,     0,   826,     0,     0,     0,     0,
   826,     0,     0,     0,     0,   826,     0,     0,     0,     0,
   826,     0,     0,     0,     0,   826,     0,     0,     0,     0,
   826,     0,     0,     0,     0,   826,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1957,     0,     0,
     0,     0,     0,     0,     0,  1656,     0,  1041,  1041,  1041,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1842,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   779,   780,   781,   782,   783,   784,   785,   786,   787,
     0,   788,     0,   789,   790,   791,   792,   793,   794,   795,
   796,   797,   798,     0,   799,     0,   800,   801,   802,   803,
   804,     0,   805,   806,   807,   808,   809,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1757,     0,
  1761,  1762,     0,  1764,  1765,     0,  1767,  1768,     0,  1770,
  1771,     0,  1773,  1774,     0,  1776,  1777,     0,  1779,  1780,
     0,  1782,  1783,     0,   546,   547,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   200,   553,     0,     0,     0,     0,     0,   810,     0,
  1454,     0,     0,     0,     0,     0,   557,  1133,   558,  1064,
     0,     0,     0,     0,     0,     0,   202,     0,     0,     0,
     0,     0,   559,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   207,   208,     0,   560,     0,
   561,     0,     0,     0,     0,     0,   565,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   218,
    73,     0,     0,     0,     0,     0,   811,   812,     0,     0,
     0,     0,     0,     0,   571,     0,  1971,  1133,     0,     0,
     0,   573,     0,     0,     0,     0,     0,     0,   223,     0,
     0,   813,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  2150,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1451,     0,     0,
   575,     0,     0,     0,  1842,     0,  1842,  1842,  1842,  1842,
  1842,  1928,   272,   273,     0,   274,  1931,     0,    25,   577,
    26,  1934,     0,     0,     0,     0,  1937,     0,     0,     0,
     0,  1940,     0,     0,     0,     0,  1943,     0,     0,     0,
     0,  1946,     0,  2079,     0,     0,  1949,     0,     0,     0,
     0,     0,     0,     0,  1953,     0,     0,     0,  1955,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1133,     0,
     0,     0,     0,     0,  1760,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  2144,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1842,
     0,     0,     0,     0,     0,     0,     0,  1842,  2169,     0,
     0,  1842,     0,  1842,     0,     0,  1842,  1842,  1842,  1842,
  1842,  1842,  1842,  1842,     0,  1842,     0,     0,     0,     0,
  2079,     0,     0,     0,     0,     0,  2314,     0,     0,     0,
     0,     0,     0,     0,  1842,     0,     0,     0,     0,     0,
     0,     0,     0,  2079,  2079,  2079,  2079,  2079,     0,     0,
     0,  1757,     0,     0,  2109,  2110,     0,  2112,  2113,     0,
  2115,  2116,     0,  2118,  2119,     0,  2121,  2122,     0,  2124,
  2125,     0,  2127,  2128,     0,  2130,  2131,     0,  1133,     0,
     0,     0,  2135,     0,     0,     0,  2138,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  2169,     0,     0,     0,     0,     0,  2169,     0,  1842,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2079,     0,     0,
     0,     0,     0,     0,     0,     0,  2079,  2079,  2079,  2079,
  2079,  2079,  2079,  2079,  2079,     0,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,  2169,     0,
     0,     0,     0,     0,     0,  2169,     0,  1842,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   544,   545,   546,
   547,     0,     0,   548,     0,     0,     0,     0,     0,     0,
   380,   549,   550,   551,   552,     0,   200,   553,     0,     0,
  2169,     0,  2169,   554,     0,     0,     0,     0,     0,   555,
   556,   557,  2079,   558,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,   562,   563,
   564,   565,   210,     0,   211,   212,     0,     0,     0,     0,
   566,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,   567,     0,     0,
     0,   568,   569,   221,   222,     0,     0,     0,   570,   571,
     0,     0,     0,   572,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   574,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,   381,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,   575,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,   576,
   274,   275,     0,    25,   577,    26,     0,     0,     0,     0,
     0,   578,  1191,     0,   580,     0,   581,     0,     0,     0,
     0,     0,   582,  1192,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   544,   545,   546,   547,     0,
     0,   548,     0,     0,     0,     0,     0,     0,   380,   549,
   550,   551,   552,     0,   200,   553,     0,     0,     0,     0,
     0,   554,     0,     0,     0,     0,     0,   555,   556,   557,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   559,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,   562,   563,   564,   565,
   210,     0,   211,   212,     0,     0,     0,     0,   566,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,   567,     0,     0,     0,   568,
   569,   221,   222,     0,     0,     0,   570,   571,     0,     0,
     0,   572,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   574,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,   381,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,   575,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,   273,   576,   274,   275,
     0,    25,   577,    26,     0,     0,     0,     0,     0,   578,
  1721,     0,   580,     0,   581,     0,     0,     0,     0,     0,
   582,  1722,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   544,   545,   546,   547,     0,     0,   548,
     0,     0,     0,     0,     0,     0,   380,   549,   550,   551,
   552,     0,   200,   553,     0,     0,     0,     0,     0,   554,
     0,     0,     0,     0,     0,   555,   556,   557,     0,   558,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,   559,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,   560,
     0,   561,   209,     0,   562,   563,   564,   565,   210,     0,
   211,   212,     0,     0,     0,     0,   566,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,   567,     0,     0,     0,   568,   569,   221,
   222,     0,     0,     0,   570,   571,     0,     0,     0,   572,
     0,     0,   573,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   574,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,   381,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,   575,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,   273,   576,   274,   275,     0,    25,
   577,    26,     0,     0,     0,     0,     0,   578,     0,     0,
   580,     0,   581,     0,     0,     0,     0,     0,   582,  1644,
   513,   514,   515,   516,   517,   518,   519,   520,   521,     0,
   522,     0,   523,   524,   525,   526,   527,   528,   529,   530,
   531,   532,     0,   533,     0,   534,   535,   536,   537,   538,
     0,   539,   540,   541,   542,   543,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   544,   545,   546,   547,     0,     0,   548,     0,     0,
     0,     0,     0,     0,   380,   549,   550,   551,   552,     0,
   200,   553,     0,     0,     0,     0,     0,   554,     0,     0,
     0,     0,     0,   555,   556,   557,     0,   558,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,   559,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,   562,   563,   564,   565,   210,     0,   211,   212,
     0,     0,     0,     0,   566,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,   567,     0,     0,     0,   568,   569,   221,   222,     0,
     0,     0,   570,   571,     0,     0,     0,   572,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   574,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,   381,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,   575,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,   273,   576,   274,   275,     0,    25,   577,    26,
     0,     0,     0,     0,     0,   578,     0,     0,   580,     0,
   581,     0,     0,     0,     0,     0,   582,  1752,   513,   514,
   515,   516,   517,   518,   519,   520,   521,     0,   522,     0,
   523,   524,   525,   526,   527,   528,   529,   530,   531,   532,
     0,   533,     0,   534,   535,   536,   537,   538,     0,   539,
   540,   541,   542,   543,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2065,   546,   547,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  2066,  2067,  2068,  2069,     0,   200,   553,
     0,     0,     0,     0,     0,   554,     0,     0,     0,     0,
     0,     0,     0,   557,     0,   558,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,   559,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,   560,     0,   561,   209,     0,
     0,     0,     0,   565,   210,     0,   211,   212,     0,     0,
     0,     0,     0,     0,     0,   213,   214,     0,     0,   215,
     0,   216,     0,     0,     0,   217,   218,     0,     0,     0,
     0,     0,     0,   568,   569,   221,   222,     0,     0,     0,
     0,   571,     0,     0,     0,  2071,     0,     0,   573,     0,
     0,     0,     0,     0,     0,   223,   224,   225,   574,     0,
   227,   228,     0,   229,   230,     0,   231,     0,     0,   232,
   233,   234,   235,   236,     0,   237,   238,     0,     0,   239,
   240,   241,   242,   243,   244,   245,   246,   247,     0,     0,
     0,     0,   248,     0,   249,   250,     0,     0,   251,   252,
     0,   253,     0,   254,     0,   255,   256,   257,   258,     0,
   259,     0,   260,   261,   262,   263,   264,   575,     0,   265,
   266,   267,   268,   269,     0,     0,   270,     0,   271,   272,
   273,  2072,   274,     0,     0,    25,   577,    26,     0,     0,
     0,     0,     0,  2073,     0,     0,  2074,     0,  2075,     0,
     0,     0,     0,     0,  2076,  2299,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1832,   546,
   547,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,     0,
     0,   557,     0,   558,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,  1833,     0,
  1834,   565,   210,     0,   211,   212,     0,     0,     0,     0,
     0,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,     0,     0,     0,
     0,   568,   569,   221,   222,     0,     0,     0,     0,   571,
     0,     0,     0,     0,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   574,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,     0,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,   575,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,  1835,
   274,     0,     0,    25,   577,    26,     0,     0,     0,     0,
     0,  1836,     0,     0,  1837,     0,  1838,     0,     0,     0,
     0,     0,  1839,  2188,   167,   168,   169,   170,   171,   172,
   173,   174,   175,     0,   176,     0,   177,   178,   179,   180,
   181,   182,   183,   184,   185,   186,     0,   187,     0,   188,
   189,   190,   191,   192,     0,   193,   194,   195,   196,   197,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   546,   547,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,   948,     0,     0,     0,     0,
     0,   949,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   950,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,     0,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,     0,     0,     0,   951,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   219,
   220,   221,   222,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   226,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,     0,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,     0,     0,   274,   513,
   514,   515,   516,   517,   518,   519,   520,   521,     0,   522,
     0,   523,   524,   525,   526,   527,   528,   529,   530,   531,
   532,   952,   533,     0,   534,   535,   536,   537,   538,     0,
   539,   540,   541,   542,   543,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1037,     0,     0,
   544,   545,   546,   547,     0,     0,   548,     0,     0,     0,
     0,     0,     0,   380,   549,   550,   551,   552,     0,   200,
   553,     0,     0,     0,     0,     0,   554,     0,     0,     0,
     0,     0,   555,   556,   557,     0,   558,     0,     0,  1038,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   559,     0,     0,     0,     0,   204,   205,  1039,     0,     0,
     0,     0,   206,   207,   208,     0,   560,     0,   561,   209,
     0,   562,   563,   564,   565,   210,     0,   211,   212,     0,
     0,     0,     0,   566,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
   567,     0,     0,     0,   568,   569,   221,   222,     0,  1040,
     0,   570,   571,     0,     0,     0,   572,     0,     0,   573,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   574,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   575,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,   576,   274,   275,     0,    25,   577,    26,     0,
     0,     0,     0,     0,   578,     0,     0,   580,     0,   581,
     0,     0,     0,     0,     0,   582,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1632,     0,     0,   544,   545,   546,
   547,     0,     0,   548,     0,     0,     0,     0,     0,     0,
   380,   549,   550,   551,   552,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,   555,
   556,   557,     0,   558,     0,     0,  1038,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,  1633,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,   562,   563,
   564,   565,   210,     0,   211,   212,     0,     0,     0,     0,
   566,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,   567,     0,     0,
     0,   568,   569,   221,   222,     0,  1634,     0,   570,   571,
     0,     0,     0,   572,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   574,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,   381,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,   575,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,   576,
   274,   275,     0,    25,   577,    26,     0,     0,     0,     0,
     0,   578,     0,     0,   580,     0,   581,     0,     0,     0,
     0,     0,   582,   513,   514,   515,   516,   517,   518,   519,
   520,   521,     0,   522,     0,   523,   524,   525,   526,   527,
   528,   529,   530,   531,   532,     0,   533,     0,   534,   535,
   536,   537,   538,     0,   539,   540,   541,   542,   543,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1660,     0,     0,   544,   545,   546,   547,     0,     0,
   548,     0,     0,     0,     0,     0,     0,   380,   549,   550,
   551,   552,     0,   200,   553,     0,     0,     0,     0,     0,
   554,     0,     0,     0,     0,     0,   555,   556,   557,     0,
   558,     0,     0,  1038,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   559,     0,     0,     0,     0,   204,
   205,  1661,     0,     0,     0,     0,   206,   207,   208,     0,
   560,     0,   561,   209,     0,   562,   563,   564,   565,   210,
     0,   211,   212,     0,     0,     0,     0,   566,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,   567,     0,     0,     0,   568,   569,
   221,   222,     0,  1662,     0,   570,   571,     0,     0,     0,
   572,     0,     0,   573,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   574,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   575,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,   576,   274,   275,     0,
    25,   577,    26,     0,     0,     0,     0,     0,   578,     0,
     0,   580,     0,   581,     0,     0,     0,     0,     0,   582,
   513,   514,   515,   516,   517,   518,   519,   520,   521,     0,
   522,     0,   523,   524,   525,   526,   527,   528,   529,   530,
   531,   532,     0,   533,     0,   534,   535,   536,   537,   538,
     0,   539,   540,   541,   542,   543,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1740,     0,
     0,   544,   545,   546,   547,     0,     0,   548,     0,     0,
     0,     0,     0,     0,   380,   549,   550,   551,   552,     0,
   200,   553,     0,     0,     0,     0,     0,   554,     0,     0,
     0,     0,     0,   555,   556,   557,     0,   558,     0,     0,
  1038,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,   559,     0,     0,     0,     0,   204,   205,  1741,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,   562,   563,   564,   565,   210,     0,   211,   212,
     0,     0,     0,     0,   566,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,   567,     0,     0,     0,   568,   569,   221,   222,     0,
  1742,     0,   570,   571,     0,     0,     0,   572,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   574,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,   381,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,   575,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,   273,   576,   274,   275,     0,    25,   577,    26,
     0,     0,     0,     0,     0,   578,     0,     0,   580,     0,
   581,     0,     0,     0,     0,     0,   582,   513,   514,   515,
   516,   517,   518,   519,   520,   521,     0,   522,     0,   523,
   524,   525,   526,   527,   528,   529,   530,   531,   532,     0,
   533,     0,   534,   535,   536,   537,   538,     0,   539,   540,
   541,   542,   543,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,  1151,     0,     0,
  1152,     0,     0,     0,     0,     0,     0,     0,   544,   545,
   546,   547,     0,     0,   548,     0,     0,     0,     0,     0,
     0,   380,   549,   550,   551,   552,     0,   200,   553,     0,
     0,     0,     0,     0,   554,     0,     0,     0,     0,     0,
   555,   556,   557,     0,   558,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   559,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   560,     0,   561,   209,     0,   562,
   563,  1153,   565,   210,     0,   211,   212,     0,     0,     0,
     0,   566,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,   567,     0,
     0,     0,   568,   569,   221,   222,     0,     0,     0,   570,
   571,     0,     0,     0,   572,     0,     0,   573,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   574,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   575,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   576,   274,   275,     0,    25,   577,    26,     0,     0,     0,
     0,     0,   578,     0,     0,   580,     0,   581,     0,     0,
     0,     0,     0,  1154,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,  1681,     0,     0,  1682,     0,     0,
     0,     0,     0,     0,     0,   544,  1044,   546,   547,     0,
     0,   548,     0,     0,     0,     0,     0,     0,   380,  1045,
  1046,  1047,  1048,     0,   200,   553,     0,     0,     0,     0,
     0,   554,     0,     0,     0,     0,     0,  1049,  1050,   557,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   559,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,     0,   563,     0,   565,
   210,     0,   211,   212,     0,     0,     0,     0,  1052,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,  1053,     0,     0,     0,   568,
   569,   221,   222,     0,     0,     0,  1054,   571,     0,     0,
     0,   572,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   574,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,   381,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,   575,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,   273,-32768,   274,   275,
     0,    25,   577,    26,     0,     0,     0,     0,     0,  1056,
     0,     0,  1057, -1238,  1058,     0,     0,     0, -1238,     0,
  1683,   513,   514,   515,   516,   517,   518,   519,   520,   521,
     0,   522,     0,   523,   524,   525,   526,   527,   528,   529,
   530,   531,   532,     0,   533,     0,   534,   535,   536,   537,
   538,     0,   539,   540,   541,   542,   543,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1686,     0,     0,  1687,     0,     0,     0,     0,     0,
     0,     0,   544,  1044,   546,   547,     0,     0,   548,     0,
     0,     0,     0,     0,     0,   380,  1045,  1046,  1047,  1048,
     0,   200,   553,     0,     0,     0,     0,     0,   554,     0,
     0,     0,     0,     0,  1049,  1050,   557,     0,   558,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   559,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   560,     0,
   561,   209,     0,  1051,   563,  1153,   565,   210,     0,   211,
   212,     0,     0,     0,     0,  1052,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,  1053,     0,     0,     0,   568,   569,   221,   222,
     0,     0,     0,  1054,   571,     0,     0,     0,   572,     0,
     0,   573,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   574,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   575,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,  1055,   274,   275,     0,    25,   577,
    26,     0,     0,     0,     0,     0,  1056,     0,     0,  1057,
     0,  1058,     0,     0,     0,     0,     0,  1688,   513,   514,
   515,   516,   517,   518,   519,   520,   521,     0,   522,     0,
   523,   524,   525,   526,   527,   528,   529,   530,   531,   532,
     0,   533,     0,   534,   535,   536,   537,   538,     0,   539,
   540,   541,   542,   543,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   544,
   545,   546,   547,     0,     0,   548,     0,     0,     0,     0,
     0,     0,   380,   549,   550,   551,   552,     0,   200,   553,
     0,     0,     0,     0,     0,   554,     0,     0,     0,     0,
     0,   555,   556,   557,     0,   558,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,   559,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,   560,     0,   561,   209,     0,
   562,   563,   564,   565,   210,     0,   211,   212,     0,     0,
     0,     0,   566,     0,     0,   213,   214,     0,     0,   215,
     0,   216,     0,     0,     0,   217,   218,     0,     0,   567,
     0,     0,     0,   568,   569,   221,   222,     0,     0,     0,
   570,   571,     0,     0,     0,   572,     0,     0,   573,     0,
     0,     0,     0,     0,     0,   223,   224,   225,   574,     0,
   227,   228,     0,   229,   230,     0,   231,     0,     0,   232,
   233,   234,   235,   236,     0,   237,   238,     0,     0,   239,
   240,   241,   242,   243,   244,   245,   246,   247,     0,     0,
     0,     0,   248,     0,   249,   250,     0,   381,   251,   252,
     0,   253,     0,   254,     0,   255,   256,   257,   258,     0,
   259,     0,   260,   261,   262,   263,   264,   575,     0,   265,
   266,   267,   268,   269,     0,     0,   270,     0,   271,   272,
   273,   576,   274,   275,     0,    25,   577,    26,     0,     0,
     0,     0,     0,   578,   579,     0,   580,     0,   581,     0,
     0,     0,     0,     0,   582,   513,   514,   515,   516,   517,
   518,   519,   520,   521,     0,   522,     0,   523,   524,   525,
   526,   527,   528,   529,   530,   531,   532,     0,   533,     0,
   534,   535,   536,   537,   538,     0,   539,   540,   541,   542,
   543,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,  1156,     0,     0,  1157,     0,
     0,     0,     0,     0,     0,     0,   544,   545,   546,   547,
     0,     0,   548,     0,     0,     0,     0,     0,     0,   380,
   549,   550,   551,   552,     0,   200,   553,     0,     0,     0,
     0,     0,   554,     0,     0,     0,     0,     0,   555,   556,
   557,     0,   558,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   559,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   560,     0,   561,   209,     0,   562,   563,     0,
   565,   210,     0,   211,   212,     0,     0,     0,     0,   566,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,   567,     0,     0,     0,
   568,   569,   221,   222,     0,     0,     0,   570,   571,     0,
     0,     0,   572,     0,     0,   573,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   574,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   575,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,   576,   274,
   275,     0,    25,   577,    26,     0,     0,     0,     0,     0,
   578,     0,     0,   580,     0,   581,     0,     0,     0,     0,
     0,  1158,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,  1160,     0,     0,  1161,     0,     0,     0,     0,
     0,     0,     0,   544,   545,   546,   547,     0,     0,   548,
     0,     0,     0,     0,     0,     0,   380,   549,   550,   551,
   552,     0,   200,   553,     0,     0,     0,     0,     0,   554,
     0,     0,     0,     0,     0,   555,   556,   557,     0,   558,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,   559,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,   560,
     0,   561,   209,     0,   562,   563,     0,   565,   210,     0,
   211,   212,     0,     0,     0,     0,   566,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,   567,     0,     0,     0,   568,   569,   221,
   222,     0,     0,     0,   570,   571,     0,     0,     0,   572,
     0,     0,   573,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   574,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,   381,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,   575,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,   273,   576,   274,   275,     0,    25,
   577,    26,     0,     0,     0,     0,     0,   578,     0,     0,
   580,     0,   581,     0,     0,     0,     0,     0,  1162,   513,
   514,   515,   516,   517,   518,   519,   520,   521,     0,   522,
     0,   523,   524,   525,   526,   527,   528,   529,   530,   531,
   532,     0,   533,     0,   534,   535,   536,   537,   538,     0,
   539,   540,   541,   542,   543,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,  1164,
     0,     0,  1165,     0,     0,     0,     0,     0,     0,     0,
   544,   545,   546,   547,     0,     0,   548,     0,     0,     0,
     0,     0,     0,   380,   549,   550,   551,   552,     0,   200,
   553,     0,     0,     0,     0,     0,   554,     0,     0,     0,
     0,     0,   555,   556,   557,     0,   558,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   559,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   560,     0,   561,   209,
     0,   562,   563,     0,   565,   210,     0,   211,   212,     0,
     0,     0,     0,   566,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
   567,     0,     0,     0,   568,   569,   221,   222,     0,     0,
     0,   570,   571,     0,     0,     0,   572,     0,     0,   573,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   574,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   575,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,   576,   274,   275,     0,    25,   577,    26,     0,
     0,     0,     0,     0,   578,     0,     0,   580,     0,   581,
     0,     0,     0,     0,     0,  1166,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,  1168,     0,     0,  1169,
     0,     0,     0,     0,     0,     0,     0,   544,   545,   546,
   547,     0,     0,   548,     0,     0,     0,     0,     0,     0,
   380,   549,   550,   551,   552,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,   555,
   556,   557,     0,   558,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,   562,   563,
     0,   565,   210,     0,   211,   212,     0,     0,     0,     0,
   566,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,   567,     0,     0,
     0,   568,   569,   221,   222,     0,     0,     0,   570,   571,
     0,     0,     0,   572,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   574,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,   381,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,   575,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,   576,
   274,   275,     0,    25,   577,    26,     0,     0,     0,     0,
     0,   578,     0,     0,   580,     0,   581,     0,     0,     0,
     0,     0,  1170,   513,   514,   515,   516,   517,   518,   519,
   520,   521,     0,   522,     0,   523,   524,   525,   526,   527,
   528,   529,   530,   531,   532,     0,   533,     0,   534,   535,
   536,   537,   538,     0,   539,   540,   541,   542,   543,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,  1172,     0,     0,  1173,     0,     0,     0,
     0,     0,     0,     0,   544,   545,   546,   547,     0,     0,
   548,     0,     0,     0,     0,     0,     0,   380,   549,   550,
   551,   552,     0,   200,   553,     0,     0,     0,     0,     0,
   554,     0,     0,     0,     0,     0,   555,   556,   557,     0,
   558,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   559,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   560,     0,   561,   209,     0,   562,   563,     0,   565,   210,
     0,   211,   212,     0,     0,     0,     0,   566,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,   567,     0,     0,     0,   568,   569,
   221,   222,     0,     0,     0,   570,   571,     0,     0,     0,
   572,     0,     0,   573,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   574,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   575,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,   576,   274,   275,     0,
    25,   577,    26,     0,     0,     0,     0,     0,   578,     0,
     0,   580,     0,   581,     0,     0,     0,     0,     0,  1174,
   513,   514,   515,   516,   517,   518,   519,   520,   521,     0,
   522,     0,   523,   524,   525,   526,   527,   528,   529,   530,
   531,   532,     0,   533,     0,   534,   535,   536,   537,   538,
     0,   539,   540,   541,   542,   543,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
  1176,     0,     0,  1177,     0,     0,     0,     0,     0,     0,
     0,   544,   545,   546,   547,     0,     0,   548,     0,     0,
     0,     0,     0,     0,   380,   549,   550,   551,   552,     0,
   200,   553,     0,     0,     0,     0,     0,   554,     0,     0,
     0,     0,     0,   555,   556,   557,     0,   558,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,   559,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,   562,   563,     0,   565,   210,     0,   211,   212,
     0,     0,     0,     0,   566,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,   567,     0,     0,     0,   568,   569,   221,   222,     0,
     0,     0,   570,   571,     0,     0,     0,   572,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   574,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,   381,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,   575,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,   273,   576,   274,   275,     0,    25,   577,    26,
     0,     0,     0,     0,     0,   578,     0,     0,   580,     0,
   581,     0,     0,     0,     0,     0,  1178,   513,   514,   515,
   516,   517,   518,   519,   520,   521,     0,   522,     0,   523,
   524,   525,   526,   527,   528,   529,   530,   531,   532,     0,
   533,     0,   534,   535,   536,   537,   538,     0,   539,   540,
   541,   542,   543,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   544,   545,
   546,   547,     0,     0,   548,     0,     0,     0,     0,     0,
     0,   380,   549,   550,   551,   552,     0,   200,   553,     0,
     0,     0,     0,     0,   554,     0,     0,     0,     0,     0,
   555,   556,   557,     0,   558,     0,     0,  1038,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   559,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   560,     0,   561,   209,     0,   562,
   563,   564,   565,   210,     0,   211,   212,     0,     0,     0,
     0,   566,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,   567,     0,
     0,     0,   568,   569,   221,   222,     0,     0,     0,   570,
   571,     0,     0,     0,   572,     0,     0,   573,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   574,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   575,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   576,   274,   275,     0,    25,   577,    26,     0,     0,     0,
     0,     0,   578,     0,     0,   580,     0,   581,     0,     0,
     0,     0,     0,   582,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   544,   545,   546,   547,     0,
     0,   548,     0,     0,     0,     0,     0,     0,   380,   549,
   550,   551,   552,     0,   200,   553,     0,     0,     0,     0,
     0,   554,     0,     0,     0,     0,     0,   555,   556,   557,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   559,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,   562,   563,   564,   565,
   210,     0,   211,   212,     0,     0,     0,     0,   566,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,    73,     0,   567,     0,     0,     0,   568,
   569,   221,   222,     0,     0,     0,   570,   571,     0,     0,
     0,   572,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   574,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,   381,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,   575,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,   273,   576,   274,   275,
     0,    25,   577,    26,     0,     0,     0,     0,     0,   578,
     0,     0,   580,     0,   581,     0,     0,     0,     0,     0,
   582,   513,   514,   515,   516,   517,   518,   519,   520,   521,
     0,   522,     0,   523,   524,   525,   526,   527,   528,   529,
   530,   531,   532,     0,   533,     0,   534,   535,   536,   537,
   538,     0,   539,   540,   541,   542,   543,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1691,     0,     0,  1692,     0,     0,     0,     0,     0,
     0,     0,   544,  1044,   546,   547,     0,     0,   548,     0,
     0,     0,     0,     0,     0,   380,  1045,  1046,  1047,  1048,
     0,   200,   553,     0,     0,     0,     0,     0,   554,     0,
     0,     0,     0,     0,  1049,  1050,   557,     0,   558,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   559,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   560,     0,
   561,   209,     0,  1051,   563,     0,   565,   210,     0,   211,
   212,     0,     0,     0,     0,  1052,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,  1053,     0,     0,     0,   568,   569,   221,   222,
     0,     0,     0,  1054,   571,     0,     0,     0,   572,     0,
     0,   573,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   574,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   575,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,  1055,   274,   275,     0,    25,   577,
    26,     0,     0,     0,     0,     0,  1056,     0,     0,  1057,
     0,  1058,     0,     0,     0,     0,     0,  1693,   513,   514,
   515,   516,   517,   518,   519,   520,   521,     0,   522,     0,
   523,   524,   525,   526,   527,   528,   529,   530,   531,   532,
     0,   533,     0,   534,   535,   536,   537,   538,     0,   539,
   540,   541,   542,   543,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,  1696,     0,
     0,  1697,     0,     0,     0,     0,     0,     0,     0,   544,
  1044,   546,   547,     0,     0,   548,     0,     0,     0,     0,
     0,     0,   380,  1045,  1046,  1047,  1048,     0,   200,   553,
     0,     0,     0,     0,     0,   554,     0,     0,     0,     0,
     0,  1049,  1050,   557,     0,   558,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,   559,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,   560,     0,   561,   209,     0,
  1051,   563,     0,   565,   210,     0,   211,   212,     0,     0,
     0,     0,  1052,     0,     0,   213,   214,     0,     0,   215,
     0,   216,     0,     0,     0,   217,   218,     0,     0,  1053,
     0,     0,     0,   568,   569,   221,   222,     0,     0,     0,
  1054,   571,     0,     0,     0,   572,     0,     0,   573,     0,
     0,     0,     0,     0,     0,   223,   224,   225,   574,     0,
   227,   228,     0,   229,   230,     0,   231,     0,     0,   232,
   233,   234,   235,   236,     0,   237,   238,     0,     0,   239,
   240,   241,   242,   243,   244,   245,   246,   247,     0,     0,
     0,     0,   248,     0,   249,   250,     0,   381,   251,   252,
     0,   253,     0,   254,     0,   255,   256,   257,   258,     0,
   259,     0,   260,   261,   262,   263,   264,   575,     0,   265,
   266,   267,   268,   269,     0,     0,   270,     0,   271,   272,
   273,  1055,   274,   275,     0,    25,   577,    26,     0,     0,
     0,     0,     0,  1056,     0,     0,  1057,     0,  1058,     0,
     0,     0,     0,     0,  1698,   513,   514,   515,   516,   517,
   518,   519,   520,   521,     0,   522,     0,   523,   524,   525,
   526,   527,   528,   529,   530,   531,   532,     0,   533,     0,
   534,   535,   536,   537,   538,     0,   539,   540,   541,   542,
   543,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,  1701,     0,     0,  1702,     0,
     0,     0,     0,     0,     0,     0,   544,  1044,   546,   547,
     0,     0,   548,     0,     0,     0,     0,     0,     0,   380,
  1045,  1046,  1047,  1048,     0,   200,   553,     0,     0,     0,
     0,     0,   554,     0,     0,     0,     0,     0,  1049,  1050,
   557,     0,   558,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   559,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   560,     0,   561,   209,     0,  1051,   563,     0,
   565,   210,     0,   211,   212,     0,     0,     0,     0,  1052,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,  1053,     0,     0,     0,
   568,   569,   221,   222,     0,     0,     0,  1054,   571,     0,
     0,     0,   572,     0,     0,   573,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   574,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   575,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,  1055,   274,
   275,     0,    25,   577,    26,     0,     0,     0,     0,     0,
  1056,     0,     0,  1057,     0,  1058,     0,     0,     0,     0,
     0,  1703,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,  1706,     0,     0,  1707,     0,     0,     0,     0,
     0,     0,     0,   544,  1044,   546,   547,     0,     0,   548,
     0,     0,     0,     0,     0,     0,   380,  1045,  1046,  1047,
  1048,     0,   200,   553,     0,     0,     0,     0,     0,   554,
     0,     0,     0,     0,     0,  1049,  1050,   557,     0,   558,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,   559,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,   560,
     0,   561,   209,     0,  1051,   563,     0,   565,   210,     0,
   211,   212,     0,     0,     0,     0,  1052,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,  1053,     0,     0,     0,   568,   569,   221,
   222,     0,     0,     0,  1054,   571,     0,     0,     0,   572,
     0,     0,   573,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   574,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,   381,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,   575,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,   273,  1055,   274,   275,     0,    25,
   577,    26,     0,     0,     0,     0,     0,  1056,     0,     0,
  1057,     0,  1058,     0,     0,     0,     0,     0,  1708,   513,
   514,   515,   516,   517,   518,   519,   520,   521,     0,   522,
     0,   523,   524,   525,   526,   527,   528,   529,   530,   531,
   532,     0,   533,     0,   534,   535,   536,   537,   538,     0,
   539,   540,   541,   542,   543,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,  1711,
     0,     0,  1712,     0,     0,     0,     0,     0,     0,     0,
   544,  1044,   546,   547,     0,     0,   548,     0,     0,     0,
     0,     0,     0,   380,  1045,  1046,  1047,  1048,     0,   200,
   553,     0,     0,     0,     0,     0,   554,     0,     0,     0,
     0,     0,  1049,  1050,   557,     0,   558,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   559,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   560,     0,   561,   209,
     0,  1051,   563,     0,   565,   210,     0,   211,   212,     0,
     0,     0,     0,  1052,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
  1053,     0,     0,     0,   568,   569,   221,   222,     0,     0,
     0,  1054,   571,     0,     0,     0,   572,     0,     0,   573,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   574,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   575,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,  1055,   274,   275,     0,    25,   577,    26,     0,
     0,     0,     0,     0,  1056,     0,     0,  1057,     0,  1058,
     0,     0,     0,     0,     0,  1713,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,  1716,     0,     0,  1717,
     0,     0,     0,     0,     0,     0,     0,   544,  1044,   546,
   547,     0,     0,   548,     0,     0,     0,     0,     0,     0,
   380,  1045,  1046,  1047,  1048,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,  1049,
  1050,   557,     0,   558,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,  1051,   563,
     0,   565,   210,     0,   211,   212,     0,     0,     0,     0,
  1052,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,  1053,     0,     0,
     0,   568,   569,   221,   222,     0,     0,     0,  1054,   571,
     0,     0,     0,   572,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   574,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,   381,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,   575,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,  1055,
   274,   275,     0,    25,   577,    26,     0,     0,     0,     0,
     0,  1056,     0,     0,  1057,     0,  1058,     0,     0,     0,
     0,     0,  1718,   513,   514,   515,   516,   517,   518,   519,
   520,   521,     0,   522,     0,   523,   524,   525,   526,   527,
   528,   529,   530,   531,   532,     0,   533,     0,   534,   535,
   536,   537,   538,     0,   539,   540,   541,   542,   543,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   544,   545,   546,   547,     0,     0,
   548,     0,     0,     0,     0,     0,     0,   380,   549,   550,
   551,   552,     0,   200,   553,     0,     0,     0,     0,     0,
   554,     0,     0,     0,     0,     0,   555,   556,   557,     0,
   558,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   559,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   560,     0,   561,   209,     0,   562,   563,   564,   565,   210,
     0,   211,   212,     0,     0,     0,     0,   566,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,   567,     0,     0,     0,   568,   569,
   221,   222,     0,     0,     0,   570,   571,     0,     0,     0,
   572,     0,     0,   573,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   574,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   575,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,   576,   274,   275,     0,
    25,   577,    26,     0,     0,     0,     0,     0,   578,     0,
     0,   580,     0,   581,     0,     0,     0,     0,     0,   582,
   513,   514,   515,   516,   517,   518,   519,   520,   521,     0,
   522,     0,   523,   524,   525,   526,   527,   528,   529,   530,
   531,   532,     0,   533,     0,   534,   535,   536,   537,   538,
     0,   539,   540,   541,   542,   543,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
  1147,     0,     0,  1148,     0,     0,     0,     0,     0,     0,
     0,   544,   545,   546,   547,     0,     0,   548,     0,     0,
     0,     0,     0,     0,   380,   549,   550,   551,   552,     0,
   200,   553,     0,     0,     0,     0,     0,   554,     0,     0,
     0,     0,     0,   555,   556,   557,     0,   558,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,   559,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,     0,   563,     0,   565,   210,     0,   211,   212,
     0,     0,     0,     0,   566,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,   567,     0,     0,     0,   568,   569,   221,   222,     0,
     0,     0,   570,   571,     0,     0,     0,   572,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   574,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,   381,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,   575,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,   273,-32768,   274,   275,     0,    25,   577,    26,
     0,     0,     0,     0,     0,   578,     0,     0,   580,     0,
   581,     0,     0,     0,     0,     0,  1149,   513,   514,   515,
   516,   517,   518,   519,   520,   521,     0,   522,     0,   523,
   524,   525,   526,   527,   528,   529,   530,   531,   532,     0,
   533,     0,   534,   535,   536,   537,   538,     0,   539,   540,
   541,   542,   543,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   544,   545,
   546,   547,     0,     0,   548,     0,     0,     0,     0,     0,
     0,   380,   549,   550,   551,   552,     0,   200,   553,     0,
     0,     0,     0,     0,   554,     0,     0,     0,     0,     0,
   555,   556,   557,     0,   558,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   559,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   560,     0,   561,   209,     0,   562,
   563,     0,   565,   210,     0,   211,   212,     0,     0,     0,
     0,   566,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,    73,     0,   567,     0,
     0,     0,   568,   569,   221,   222,     0,     0,     0,   570,
   571,     0,     0,     0,   572,     0,     0,   573,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   574,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   575,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   576,   274,   275,     0,    25,   577,    26,     0,     0,     0,
     0,     0,   578,     0,     0,   580,     0,   581,     0,     0,
     0,     0,     0,   582,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   544,   545,   546,   547,     0,
     0,   548,     0,     0,     0,     0,     0,     0,   380,   549,
   550,   551,   552,     0,   200,   553,     0,     0,     0,     0,
     0,   554,     0,     0,     0,     0,     0,   555,   556,   557,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   559,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,   562,   563,     0,   565,
   210,     0,   211,   212,     0,     0,     0,     0,   566,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,   567,     0,     0,     0,   568,
   569,   221,   222,     0,     0,     0,   570,   571,     0,     0,
     0,   572,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   574,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,   381,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,   575,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,   273,   576,   274,   275,
     0,    25,   577,    26,     0,     0,     0,     0,     0,   578,
     0,     0,   580,     0,   581,     0,     0,     0,     0,     0,
   582,   513,   514,   515,   516,   517,   518,   519,   520,   521,
     0,   522,     0,   523,   524,   525,   526,   527,   528,   529,
   530,   531,   532,     0,   533,     0,   534,   535,   536,   537,
   538,     0,   539,   540,   541,   542,   543,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   544,  1044,   546,   547,     0,     0,   548,     0,
     0,     0,     0,     0,     0,   380,  1045,  1046,  1047,  1048,
     0,   200,   553,     0,     0,     0,     0,     0,   554,     0,
     0,     0,     0,     0,  1049,  1050,   557,     0,   558,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   559,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   560,     0,
   561,   209,     0,  1051,   563,     0,   565,   210,     0,   211,
   212,     0,     0,     0,     0,  1052,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,  1053,     0,     0,     0,   568,   569,   221,   222,
     0,     0,     0,  1054,   571,     0,     0,     0,   572,     0,
     0,   573,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   574,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   575,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,  1055,   274,   275,     0,    25,   577,
    26,     0,     0,     0,     0,     0,  1056,     0,     0,  1057,
     0,  1058,     0,     0,     0,     0,     0,  1059,   513,   514,
   515,   516,   517,   518,   519,   520,   521,     0,   522,     0,
   523,   524,   525,   526,   527,   528,   529,   530,   531,   532,
     0,   533,     0,   534,   535,   536,   537,   538,     0,   539,
   540,   541,   542,   543,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1116,   546,   547,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   380,  1117,  1118,  1119,  1120,     0,   200,   553,
     0,     0,     0,     0,     0,   554,     0,     0,     0,     0,
     0,     0,     0,   557,     0,   558,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,   559,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,   560,     0,   561,   209,     0,
     0,     0,     0,   565,   210,     0,   211,   212,     0,     0,
     0,     0,  1121,     0,     0,   213,   214,     0,     0,   215,
     0,   216,     0,     0,     0,   217,   218,     0,     0,  1122,
     0,     0,     0,   568,   569,   221,   222,     0,     0,     0,
  1123,   571,     0,     0,     0,  1124,     0,     0,   573,     0,
     0,     0,     0,     0,     0,   223,   224,   225,   574,     0,
   227,   228,     0,   229,   230,     0,   231,     0,     0,   232,
   233,   234,   235,   236,     0,   237,   238,     0,     0,   239,
   240,   241,   242,   243,   244,   245,   246,   247,     0,     0,
     0,     0,   248,     0,   249,   250,     0,   381,   251,   252,
     0,   253,     0,   254,     0,   255,   256,   257,   258,     0,
   259,     0,   260,   261,   262,   263,   264,   575,     0,   265,
   266,   267,   268,   269,     0,     0,   270,     0,   271,   272,
   273,  1125,   274,   275,     0,    25,   577,    26,     0,     0,
     0,     0,     0,  1126,     0,     0,  1127,     0,  1128,     0,
     0,     0,     0,     0,  1129,   513,   514,   515,   516,   517,
   518,   519,   520,   521,     0,   522,     0,   523,   524,   525,
   526,   527,   528,   529,   530,   531,   532,     0,   533,     0,
   534,   535,   536,   537,   538,     0,   539,   540,   541,   542,
   543,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1116,   546,   547,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   380,
  1117,  1118,  1119,  1120,     0,   200,   553,     0,     0,     0,
     0,     0,   554,     0,     0,     0,     0,     0,     0,     0,
   557,     0,   558,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   559,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   560,     0,   561,   209,     0,     0,     0,     0,
   565,   210,     0,   211,   212,     0,     0,     0,     0,  1121,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,  1122,     0,     0,     0,
   568,   569,   221,   222,     0,     0,     0,  1123,   571,     0,
     0,     0,  1124,     0,     0,   573,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   574,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   575,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,-32768,   274,
   275,     0,    25,   577,    26,     0,     0,     0,     0,     0,
  1126,     0,     0,  1127,     0,  1128,     0,     0,     0,     0,
     0,  1129,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2065,   546,   547,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2066,  2067,  2068,
  2069,     0,   200,   553,     0,     0,     0,     0,     0,   554,
     0,     0,     0,     0,     0,     0,     0,   557,     0,   558,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,   559,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,   560,
     0,   561,   209,     0,     0,     0,  2070,   565,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   568,   569,   221,
   222,     0,     0,     0,     0,   571,     0,     0,     0,  2071,
     0,     0,   573,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   574,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,   575,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,   273,  2072,   274,     0,     0,    25,
   577,    26,     0,     0,     0,     0,     0,  2073,     0,     0,
  2074,     0,  2075,     0,     0,     0,     0,     0,  2076,   513,
   514,   515,   516,   517,   518,   519,   520,   521,     0,   522,
     0,   523,   524,   525,   526,   527,   528,   529,   530,   531,
   532,     0,   533,     0,   534,   535,   536,   537,   538,     0,
   539,   540,   541,   542,   543,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  2065,   546,   547,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2066,  2067,  2068,  2069,     0,   200,
   553,     0,     0,     0,     0,     0,   554,     0,     0,     0,
     0,     0,     0,     0,   557,     0,   558,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   559,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   560,     0,   561,   209,
     0,     0,     0,     0,   565,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   568,   569,   221,   222,     0,     0,
     0,     0,   571,     0,     0,     0,  2071,     0,     0,   573,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   574,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   575,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,  2072,   274,     0,     0,    25,   577,    26,     0,
     0,     0,     0,     0,  2073,     0,     0,  2074,     0,  2075,
     0,     0,     0,     0,     0,  2076,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  2065,   546,
   547,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  2066,  2067,  2068,  2069,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,     0,
     0,   557,     0,   558,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,     0,     0,
     0,   565,   210,     0,   211,   212,     0,     0,     0,     0,
     0,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,     0,     0,     0,
     0,   568,   569,   221,   222,     0,     0,     0,     0,   571,
     0,     0,     0,  2071,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   574,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,     0,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,   575,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,-32768,
   274,     0,     0,    25,   577,    26,     0,     0,     0,     0,
     0,  2073,     0,     0,  2074,     0,  2075,     0,     0,     0,
     0,     0,  2076,   513,   514,   515,   516,   517,   518,   519,
   520,   521,     0,   522,     0,   523,   524,   525,   526,   527,
   528,   529,   530,   531,   532,     0,   533,     0,   534,   535,
   536,   537,   538,     0,   539,   540,   541,   542,   543,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1020,   546,   547,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   380,     0,     0,
     0,     0,     0,   200,   553,     0,     0,     0,     0,     0,
   554,     0,     0,     0,     0,     0,     0,     0,   557,     0,
   558,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   559,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   560,     0,   561,   209,     0,     0,     0,     0,   565,   210,
     0,   211,   212,     0,     0,     0,     0,  1021,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,  1022,     0,     0,     0,   568,   569,
   221,   222,     0,     0,     0,  1023,   571,     0,     0,     0,
     0,     0,     0,   573,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   574,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   575,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,  1024,   274,     0,     0,
    25,   577,    26,     0,     0,     0,     0,     0,  1025,     0,
     0,  1026,     0,     0,     0,     0,     0,     0,     0,  1027,
   513,   514,   515,   516,   517,   518,   519,   520,   521,     0,
   522,     0,   523,   524,   525,   526,   527,   528,   529,   530,
   531,   532,     0,   533,     0,   534,   535,   536,   537,   538,
     0,   539,   540,   541,   542,   543,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1020,   546,   547,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   380,     0,     0,     0,     0,     0,
   200,   553,     0,     0,     0,     0,     0,   554,     0,     0,
     0,     0,     0,     0,     0,   557,     0,   558,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,   559,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,     0,     0,     0,   565,   210,     0,   211,   212,
     0,     0,     0,     0,  1021,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,  1022,     0,     0,     0,   568,   569,   221,   222,     0,
     0,     0,  1023,   571,     0,     0,     0,     0,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   574,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,   381,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,   575,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,   273,-32768,   274,     0,     0,    25,   577,    26,
     0,     0,     0,     0,     0,  1025,     0,     0,  1026,     0,
     0,     0,     0,     0,     0,     0,  1027,   513,   514,   515,
   516,   517,   518,   519,   520,   521,     0,   522,     0,   523,
   524,   525,   526,   527,   528,   529,   530,   531,   532,     0,
   533,     0,   534,   535,   536,   537,   538,     0,   539,   540,
   541,   542,   543,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1832,
   546,   547,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   200,   553,     0,
     0,     0,     0,     0,   554,     0,     0,     0,     0,     0,
     0,     0,   557,     0,   558,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   559,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   560,     0,   561,   209,     0,  1833,
     0,  1834,   565,   210,     0,   211,   212,     0,     0,     0,
     0,     0,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,     0,     0,
     0,     0,   568,   569,   221,   222,     0,     0,     0,     0,
   571,     0,     0,     0,     0,     0,     0,   573,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   574,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,     0,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   575,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
  1835,   274,     0,     0,    25,   577,    26,     0,     0,     0,
     0,     0,  1836,     0,     0,  1837,     0,  1838,     0,     0,
     0,     0,     0,  1839,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1832,   546,   547,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,   553,     0,     0,     0,     0,
     0,   554,     0,     0,     0,     0,     0,     0,     0,   557,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   559,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,     0,     0,  1834,   565,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   568,
   569,   221,   222,     0,     0,     0,     0,   571,     0,     0,
     0,     0,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   574,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,   575,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,   273,-32768,   274,     0,
     0,    25,   577,    26,     0,     0,     0,     0,     0,  1836,
     0,     0,  1837,     0,  1838,     0,     0,     0,     0,     0,
  1839,   167,   168,   169,   170,   171,   172,   173,   174,   175,
     0,   176,     0,   177,   178,   179,   180,   181,   182,   183,
   184,   185,   186,     0,   187,     0,   188,   189,   190,   191,
   192,     0,   193,   194,   195,   196,   197,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   380,     0,     0,     0,     0,
     0,   200,     0,     0,     0,     0,     0,     0,   201,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,     0,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,     0,     0,
     0,   209,     0,     0,     0,     0,     0,   210,     0,   211,
   212,     0,     0,     0,     0,     0,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,     0,     0,     0,     0,   219,   220,   221,   222,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   226,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
     0,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,     0,     0,   274,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   167,   168,   169,   170,
   171,   172,   173,   174,   175,     0,   176,  1489,   177,   178,
   179,   180,   181,   182,   183,   184,   185,   186,     0,   187,
     0,   188,   189,   190,   191,   192,     0,   193,   194,   195,
   196,   197,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   200,     0,     0,     0,
     0,     0,     0,   201,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,     0,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,     0,     0,     0,   209,     0,     0,     0,
     0,     0,   210,     0,   211,   212,     0,     0,     0,     0,
     0,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,    73,     0,     0,     0,     0,
     0,   219,   220,   221,   222,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   226,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,     0,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,     0,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,     0,     0,
   274,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   167,   168,   169,   170,   171,   172,   173,   174,   175,
     0,   176,    91,   177,   178,   179,   180,   181,   182,   183,
   184,   185,   186,     0,   187,     0,   188,   189,   190,   191,
   192,     0,   193,   194,   195,   196,   197,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   200,     0,     0,     0,     0,     0,     0,   201,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,     0,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,     0,     0,
     0,   209,     0,     0,     0,     0,     0,   210,     0,   211,
   212,     0,     0,     0,     0,     0,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,     0,     0,     0,     0,   219,   220,   221,   222,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   226,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
     0,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
  1590,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,   491,   274,     0,     0,    25,     0,
    26,     0,   465,   466,   467,   468,  1591,   470,   471,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   200,
     0,     0,   970,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,   462,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,   463,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   219,   220,   221,   222,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,   464,   274,     0,     0,     0,     0,     0,     0,
   465,   466,   467,   468,   469,   470,   471,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,     0,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   200,     0,     0,
     0,     0,     0,     0,   201,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,     0,     0,
     0,     0,   462,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,     0,     0,     0,   209,     0,     0,
     0,     0,     0,   210,     0,   211,   212,     0,     0,     0,
     0,     0,     0,     0,   213,   214,   463,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,     0,     0,
     0,     0,   219,   220,   221,   222,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   226,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,     0,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,     0,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,     0,
   464,   274,     0,     0,     0,     0,     0,     0,   465,   466,
   467,   468,   469,   470,   471,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   380,
     0,     0,     0,     0,     0,   200,     0,     0,     0,     0,
     0,     0,   201,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,     0,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,     0,     0,     0,   209,     0,     0,     0,     0,
     0,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,     0,     0,     0,     0,
   219,   220,   221,   222,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   226,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,     0,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
     0,     0,     0,   577,     0,     0,     0,     0,     0,     0,
     0,   869,   167,   168,   169,   170,   171,   172,   173,   174,
   175,     0,   176,     0,   177,   178,   179,   180,   181,   182,
   183,   184,   185,   186,     0,   187,     0,   188,   189,   190,
   191,   192,     0,   193,   194,   195,   196,   197,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   380,     0,     0,     0,
     0,     0,   200,     0,     0,     0,     0,     0,     0,   201,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,     0,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,     0,
     0,     0,   209,     0,     0,     0,     0,     0,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   219,   220,   221,
   222,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   226,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,   381,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,     0,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,     0,     0,   274,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,   395,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   200,     0,     0,
     0,     0,     0,     0,   201,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,     0,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,     0,     0,     0,   209,     0,     0,
     0,     0,     0,   210,     0,   211,   212,     0,     0,     0,
     0,     0,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,     0,     0,
     0,     0,   219,   220,   221,   222,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   226,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,     0,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,     0,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,     0,
     0,   274,   167,   168,   169,   170,   171,   172,   173,   174,
   175,     0,   176,   723,   177,   178,   179,   180,   181,   182,
   183,   184,   185,   186,     0,   187,     0,   188,   189,   190,
   191,   192,     0,   193,   194,   195,   196,   197,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   200,     0,     0,     0,     0,     0,     0,   201,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,     0,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,     0,
     0,     0,   209,     0,     0,     0,     0,     0,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   219,   220,   221,
   222,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   226,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,     0,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,     0,     0,   274,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,  1187,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   200,     0,     0,
     0,     0,     0,     0,   201,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,     0,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,     0,     0,     0,   209,     0,     0,
     0,     0,     0,   210,     0,   211,   212,     0,     0,     0,
     0,     0,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,     0,     0,
     0,     0,   219,   220,   221,   222,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   226,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,     0,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,     0,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,     0,
     0,   274,   167,   168,   169,   170,   171,   172,   173,   174,
   175,     0,   176,  1503,   177,   178,   179,   180,   181,   182,
   183,   184,   185,   186,     0,   187,     0,   188,   189,   190,
   191,   192,     0,   193,   194,   195,   196,   197,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   200,     0,     0,     0,     0,     0,     0,   201,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,     0,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,     0,
     0,     0,   209,     0,     0,     0,     0,     0,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   219,   220,   221,
   222,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   226,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,     0,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,     0,     0,   274,   779,   780,   781,
   782,   783,   784,   785,   786,   787,     0,   788,  1806,   789,
   790,   791,   792,   793,   794,   795,   796,   797,   798,     0,
   799,     0,   800,   801,   802,   803,   804,     0,   805,   806,
   807,   808,   809,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   546,   547,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   200,   553,     0,
     0,     0,     0,     0,   810,     0,     0,     0,     0,     0,
     0,     0,   557,     0,   558,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,   559,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   207,   208,     0,   560,     0,   561,     0,     0,     0,
     0,     0,   565,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   218,     0,     0,     0,     0,
     0,     0,   811,   812,     0,     0,     0,     0,     0,     0,
   571,     0,     0,     0,     0,     0,     0,   573,     0,     0,
     0,     0,     0,     0,   223,     0,     0,   813,     0,     0,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   575,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   272,   273,
     0,   274,     0,     0,    25,   577,    26,     0,     0,     0,
     0,     0,     0,     0,   380,     0,     0,     0,     0,     0,
   200,     0,     0,     0,     0,     0,     0,   201,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,     0,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,     0,     0,     0,
   209,     0,     0,     0,     0,     0,   210,     0,   211,   212,
     0,     0,     0,     0,     0,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,     0,     0,     0,     0,   219,   220,   221,   222,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   226,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,   381,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,     0,     0,     0,   577,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,   293,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   294,     0,     0,     0,     0,     0,   200,
     0,     0,   295,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   219,   220,   221,   222,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,     0,   274,   275,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   200,     0,     0,   429,     0,
     0,     0,   201,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,     0,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,     0,     0,     0,   209,     0,     0,     0,     0,
     0,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,     0,     0,     0,     0,
   219,   220,   221,   222,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   226,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,     0,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,     0,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,     0,   274,
   275,   167,   168,   169,   170,   171,   172,   173,   174,   175,
     0,   176,     0,   177,   178,   179,   180,   181,   182,   183,
   184,   185,   186,     0,   187,     0,   188,   189,   190,   191,
   192,     0,   193,   194,   195,   196,   197,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   200,     0,     0,   295,     0,     0,     0,   201,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,     0,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,     0,     0,
     0,   209,     0,     0,     0,     0,     0,   210,     0,   211,
   212,     0,     0,     0,     0,     0,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,     0,     0,     0,     0,   219,   220,   221,   222,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   226,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
     0,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
     0,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,     0,   274,   275,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,     0,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   200,     0,     0,
     0,     0,     0,     0,   201,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,     0,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,     0,     0,     0,   209,     0,     0,
     0,     0,     0,   210,     0,   211,   212,     0,     0,     0,
     0,     0,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,     0,     0,
     0,     0,   219,   220,   221,   222,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   226,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,     0,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,     0,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
     0,   274,   275,   167,   168,   169,   170,   171,   172,   173,
   174,   175,     0,   176,     0,   177,   178,   179,   180,   181,
   182,   183,   184,   185,   186,     0,   187,     0,   188,   189,
   190,   191,   192,     0,   193,   194,   195,   196,   197,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   200,     0,     0,     0,     0,     0,     0,
   201,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,     0,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
     0,     0,     0,   209,     0,     0,     0,     0,     0,   210,
     0,   211,   212,     0,     0,     0,     0,     0,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,     0,     0,     0,     0,   219,   220,
   221,   222,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   226,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,     0,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,     0,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,     0,     0,   274,   275,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1075,     0,     0,     0,     0,     0,  1076,     0,     0,     0,
  1077,     0,     0,  1078,     0,     0,     0,     0,     0,   200,
     0,     0,     0,     0,     0,     0,   201,     0,  1079,  1080,
     0,     0,     0,     0,  1081,     0,     0,     0,  1082,     0,
     0,     0,  1083,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,  1084,     0,     0,   210,     0,   211,   212,     0,
  1085,     0,     0,  1086,  1087,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,  1088,     0,  1089,   219,   220,   221,   222,     0,     0,
  1090,     0,  1091,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1092,     0,     0,     0,   223,   224,   225,   226,
  1093,   227,   228,  1094,   229,   230,  1095,   231,  1096,  1097,
   232,   233,   234,   235,   236,  1098,   237,   238,  1099,  1100,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
  1101,     0,  1102,   248,  1103,   249,   250,  1104,  1105,   251,
   252,  1106,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,  1107,   260,   261,   262,   263,   264,  1108,  1109,
   265,   266,   267,   268,   269,     0,  1110,   270,  1111,   271,
   272,     0,     0,   274,   167,   168,   169,   170,   171,   172,
   173,   174,   175,     0,   176,     0,   177,   178,   179,   180,
   181,   182,   183,   184,   185,   186,     0,   187,     0,   188,
   189,   190,   191,   192,     0,   193,   194,   195,   196,   197,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   546,   547,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,   948,     0,     0,     0,     0,
     0,   949,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   950,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,     0,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,     0,     0,     0,   951,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   219,
   220,   221,   222,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   226,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,     0,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,     0,     0,   274,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1251,     0,     0,     0,     0,     0,
  1286,     0,     0,     0,     0,     0,     0,     0,     0,   200,
     0,     0,     0,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1253,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,  1254,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   219,   220,   221,   222,     0,     0,
     0,     0,     0,     0,  1255,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,     0,   274,   167,   168,   169,   170,   171,   375,
   173,   174,   175,     0,   176,     0,   177,   178,   179,   180,
   181,   182,   183,   184,   185,   186,     0,   187,     0,   188,
   189,   190,   191,   192,     0,   193,   194,   195,   196,   197,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,     0,     0,     0,     0,     0,
     0,   201,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,     0,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
   376,     0,     0,     0,   209,     0,     0,     0,     0,     0,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   377,
   220,   221,   222,     0,     0,   378,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   226,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,     0,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,     0,     0,   274,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   380,     0,     0,     0,     0,     0,   200,
     0,     0,     0,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   219,   220,   221,   222,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,     0,   274,   167,   168,   169,   170,   171,   172,
   173,   174,   175,     0,   176,     0,   177,   178,   179,   180,
   181,   182,   183,   184,   185,   186,     0,   187,     0,   188,
   189,   190,   191,   192,     0,   193,   194,   195,   196,   197,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,     0,     0,     0,     0,     0,
     0,   201,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1308,     0,   202,
     0,     0,   203,     0,     0,     0,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,     0,     0,     0,   209,     0,     0,     0,     0,     0,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,  1309,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   219,
   220,   221,   222,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   226,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,     0,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,     0,     0,   274,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   200,
     0,     0,     0,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   403,   220,   221,   222,     0,     0,
   404,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,     0,   274,   167,   168,   169,   170,   171,   172,
   173,   174,   175,     0,   176,     0,   177,   178,   179,   180,
   181,   182,   183,   184,   185,   186,     0,   187,     0,   188,
   189,   190,   191,   192,     0,   193,   194,   195,   196,   197,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,     0,     0,     0,     0,     0,
     0,   201,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,     0,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,     0,     0,     0,   209,     0,     0,     0,     0,     0,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   406,
   220,   221,   222,     0,     0,   407,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   226,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,     0,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,     0,     0,   274,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   200,
     0,     0,     0,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   219,   220,   221,   222,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,   979,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,     0,   274,   167,   168,   169,   170,   171,   172,
   173,   174,   175,     0,   176,     0,   177,   178,   179,   180,
   181,   182,   183,   184,   185,   186,     0,   187,     0,   188,
   189,   190,   191,   192,     0,   193,   194,   195,   196,   197,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,     0,     0,     0,     0,     0,
     0,   201,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,     0,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,     0,     0,     0,   209,     0,     0,     0,     0,     0,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   219,
   220,   221,   222,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   226,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,  1608,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,     0,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,     0,     0,   274,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,  1801,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   200,
     0,     0,     0,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   219,   220,   221,   222,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,     0,   274,   167,   168,   169,   170,   171,   172,
   173,   174,   175,     0,   176,     0,   177,   178,   179,   180,
   181,   182,   183,   184,   185,   186,     0,   187,     0,   188,
   189,   190,   191,   192,     0,   193,   194,   195,   196,   197,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,     0,     0,     0,     0,     0,
     0,   201,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,     0,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,     0,     0,     0,   209,     0,     0,     0,     0,     0,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   219,
   220,   221,   222,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   226,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,     0,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,     0,     0,   274,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   200,
     0,     0,     0,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   219,   220,   221,   222,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   335,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,     0,   274,   167,   168,   694,   170,   171,   172,
   173,   174,   175,     0,   176,     0,   177,   178,   179,   180,
   181,   182,   183,   184,   185,   186,     0,   187,     0,   188,
   189,   190,   191,   192,     0,   193,   194,   195,   196,   197,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,     0,     0,     0,     0,     0,
     0,   201,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,     0,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,     0,     0,     0,   209,     0,     0,     0,     0,     0,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   219,
   220,   221,   222,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   226,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,     0,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,     0,     0,   274,   167,
   168,   169,   170,   171,   172,   173,   174,   175,     0,   176,
     0,   177,   178,   179,   180,   181,   182,   183,   184,   185,
   186,     0,   187,     0,   188,   189,   190,   191,   192,     0,
   193,   194,   195,   196,   197,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   200,
     0,     0,     0,     0,     0,     0,   201,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
     0,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,     0,     0,     0,   209,
     0,     0,     0,     0,     0,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
  1197,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   219,   220,   221,   222,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   226,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,     0,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,     0,   274,   779,   780,   781,   782,   783,   784,
   785,   786,   787,     0,   788,     0,   789,   790,   791,   792,
   793,   794,   795,   796,   797,   798,     0,   799,     0,   800,
   801,   802,   803,   804,     0,   805,   806,   807,   808,   809,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   546,   547,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   200,   553,     0,     0,     0,     0,
     0,   810,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,     0,     0,     0,   559,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   207,   208,
     0,   560,     0,   561,     0,     0,     0,     0,     0,   565,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   218,     0,     0,     0,     0,     0,     0,   811,
   812,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,     0,     0,   813,     0,   779,   780,   781,   782,
   783,   784,   785,   786,   787,     0,   788,     0,   789,   790,
   791,   792,   793,   794,   795,   796,   797,   798,     0,   799,
     0,   800,   801,   802,   803,   804,     0,   805,   806,   807,
   808,   809,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1586,     0,   575,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   272,     0,     0,   274,   546,
   547,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   200,   553,     0,     0,
     0,     0,     0,   810,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   558,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,     0,     0,     0,   559,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   207,   208,     0,   560,     0,   561,     0,     0,     0,     0,
     0,   565,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   218,     0,     0,     0,     0,     0,
     0,   811,   812,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,     0,     0,   813,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   575,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   272,     0,     0,
   274
};

static const short yycheck[] = {     1,
    91,     1,     1,    56,   350,   333,   435,   310,    49,    50,
   987,    52,    53,    54,    55,   347,   510,   320,   165,    60,
   624,    49,    63,    76,    52,  1526,    67,   596,   930,  1554,
    50,  1497,  1268,    74,    75,    49,   339,    78,  1489,   708,
   325,    82,    83,  1429,   832,    42,    87,    88,    89,  2027,
    42,    27,    42,    42,  1822,    18,    84,  1242,   374,   344,
   111,    96,    19,    17,    19,  1245,   432,  1125,  1126,  1127,
  1128,   113,    63,   113,   438,    66,    40,    54,   307,    43,
    96,   752,    46,    96,    96,  1143,    63,    51,    96,    53,
    54,   176,   191,   191,   547,    92,  1024,  1025,  1026,  1027,
    92,    59,    92,    92,   775,    78,    33,   172,   561,    82,
     4,   104,    60,    40,   191,   834,    43,   101,    12,    46,
    62,   104,    64,   135,    51,    59,    53,    54,    97,   191,
   123,    23,    30,   182,    32,    29,    97,   172,    59,   112,
   123,    35,    36,   125,    95,   293,   102,   172,    59,   191,
   298,    59,  1037,   122,  1039,  1040,    67,   192,    97,   101,
   393,   122,   508,    41,    42,   511,    44,    45,   214,    47,
    48,   154,    50,    78,    52,   744,   113,    55,    56,    57,
    58,   121,    76,    59,   855,    59,   159,   274,  1347,   173,
   114,   172,    59,   278,   187,  1348,   280,   243,  2176,   298,
   298,   300,   300,   172,   187,   275,   100,   112,    59,   703,
    96,   192,   818,   297,     6,   188,   110,     9,   166,   448,
   189,   298,  1381,    15,    16,    63,   296,    59,   189,  1382,
   170,   113,   156,   184,   299,   284,   298,   191,   300,    31,
   196,   292,    34,   476,   285,   182,    59,   145,   199,   274,
   189,   292,   277,   125,   159,   619,   298,   285,   298,   275,
   208,   147,   275,   275,   299,    75,   582,   275,   309,   310,
   210,   285,    82,    83,   299,  1434,   224,    87,   249,   320,
   321,   322,  1435,   188,   325,   326,   247,   328,   329,  2267,
   259,   210,   333,   187,   335,    59,   337,   338,   339,   193,
   341,   870,   343,   344,   345,   874,   672,   300,   299,   350,
   351,   274,   280,   255,   277,   768,   298,   175,   299,  1844,
  1845,   278,   298,   278,   415,    63,   299,   298,   369,   297,
   759,   289,   290,   353,   375,   292,   290,  1395,   302,  1790,
   297,  2319,   276,  2321,   321,   322,  1732,   375,   376,   390,
  1535,   328,   393,   287,   288,   289,   290,   359,   292,   400,
  1540,    99,  1542,   297,   341,   276,   343,  1038,   289,   290,
   423,   137,   272,  1431,   351,   302,   287,   288,   289,   290,
   274,   289,   290,   424,  2327,  1443,  1444,  1445,  1446,  1447,
  1448,   432,   433,  2161,   275,   392,   274,   215,   439,   401,
   392,   442,   392,   392,   445,   171,  1125,  1126,  1127,  1128,
    63,   198,   432,   289,   290,   289,   290,   437,  1346,   460,
  1886,   990,   289,   290,  1143,   276,   244,   219,  1356,  1357,
  1358,  1359,  1360,  1361,  2377,   476,   287,   288,   289,   290,
   191,   292,   280,   281,   276,  1051,   297,   488,   150,  1055,
  1056,  1057,  1058,   285,   286,   287,   288,   289,   290,   191,
   164,  1030,   439,   276,  1349,   442,   494,   508,   445,   191,
   511,   512,   228,    78,   287,   288,   289,   290,    59,    84,
    97,   975,    27,    97,   280,   189,   915,   300,   275,   191,
   143,    96,   108,  1062,   275,    73,   275,   275,  1383,   309,
    33,   297,   504,   544,    59,   122,  1294,   160,   122,   296,
    65,   488,   276,   280,   108,   296,   275,   296,   296,   329,
   284,   285,   286,   287,   288,   289,   290,   337,    24,   145,
   297,   147,   108,  1321,   575,    63,   852,   296,    97,    78,
   145,   280,   274,  1994,   275,   277,   637,   298,   639,   300,
  2001,  1436,    59,   147,   159,   583,    59,    96,   172,   369,
   165,   300,  1131,   122,   374,   296,   144,  1355,  2093,    97,
   186,   147,   189,   276,  1960,   189,   298,   618,   300,   620,
   390,   622,  1186,   188,   108,   108,   614,   233,   616,   617,
   400,   632,   186,   646,   122,   173,   275,  1201,   651,   652,
   175,    20,    21,   249,  1273,  1393,   145,   275,  1277,    28,
   186,    70,   119,   172,   278,   190,   275,   296,  1676,    70,
   159,  1857,   145,   147,   147,   910,   165,   280,   296,   670,
   189,   672,   935,   191,  1051,   288,   118,   296,  1055,  1056,
  1057,  1058,   670,   275,   172,   622,  1826,   738,  1828,   188,
   460,   692,   672,    70,  1442,   137,   670,   184,  2159,   258,
   119,   189,   186,   186,   296,   667,   265,   708,   119,    13,
   182,   987,   199,   714,   280,   134,  1395,  1348,    59,  1581,
   721,   722,   288,   134,    65,   713,   727,   246,   247,   171,
   731,   732,  1363,  1364,  2145,   276,  2147,   738,  1756,  2224,
    59,  1372,   119,   284,   285,   286,   287,   288,   289,   290,
    59,  1382,  1431,   276,    97,   692,    65,   134,    95,   300,
    97,   276,   285,   286,  1443,  1444,  1445,  1446,  1447,  1448,
    26,   259,   287,   288,   289,   290,  2202,   296,   113,   122,
   299,  1412,    59,  1059,   721,   122,   275,  1632,  1633,  1634,
   727,   753,   754,   755,   731,   732,   126,   275,   760,   906,
   907,  2227,   274,   275,  1435,   277,   278,   296,  1556,   276,
   772,   296,   175,   276,   299,  1660,  1661,  1662,   296,  1450,
   287,   288,   289,   290,   287,   288,   289,   290,  1394,   172,
   831,   832,  1398,   253,  1400,   172,   275,  1403,  1404,  1405,
  1406,  1407,  1408,  1409,  1410,    59,   189,  1413,   618,   275,
   620,    65,   189,   854,   292,   856,   857,   296,   275,   297,
  1314,   299,   632,   864,   108,   130,   828,   275,   830,    59,
   296,   275,   275,  1149,    59,  2015,   228,  2017,  1154,   296,
    70,     4,  1158,   161,   275,  1414,  1162,   275,   296,    12,
  1166,   150,   296,   296,  1170,  1740,  1741,  1742,  1174,  1917,
   275,   145,  1178,   147,   247,   296,    29,   908,   296,   910,
   247,   912,    35,    36,   274,    59,   275,   277,   275,   275,
   857,   296,  1833,    67,  1835,  1836,  1837,  1838,  1839,   119,
   892,   893,   912,    59,   935,   276,   126,   296,    59,   296,
   296,    67,   186,   905,   134,   905,   287,   288,   289,   290,
   298,   275,   722,    76,  1891,   145,   299,   276,   150,   921,
   922,   275,   299,   280,   275,   275,   967,   276,   287,   288,
   289,   290,   296,   974,   975,   275,   150,   100,   287,   288,
   289,   290,   296,   300,   985,   296,   296,   110,   275,  1378,
   991,   277,   278,    59,    93,   996,   296,  1676,   960,   276,
   962,   963,   964,   965,    70,  1297,  1298,   201,  1296,   296,
   287,   288,   289,   290,   275,   127,  1647,  1394,   113,   118,
  2049,  1398,   296,  1400,   119,   299,  1403,  1404,  1405,  1406,
  1407,  1408,  1409,  1410,   275,   296,  1413,   173,   137,   138,
   275,   231,  1318,  2072,  2073,  2074,  2075,  2076,   275,   126,
  1326,   274,   205,   119,   277,   296,   209,   280,   248,   282,
   126,   296,   276,   275,   187,   298,   292,   300,   134,   296,
   193,   297,   171,   287,   288,   289,   290,  1756,   301,   266,
   267,   150,   852,  1273,   296,   292,   276,  1277,   275,  2107,
   297,   276,   299,   201,   284,   285,   286,   287,   288,   289,
   290,   200,   287,   288,   289,   290,   211,  2018,    42,    20,
    21,   216,  1678,   127,    48,  2026,    50,    28,    52,  2030,
  1794,  2032,   227,  1797,  2035,  2036,  2037,  2038,  2039,  2040,
  2041,  2042,   276,  2044,   239,   240,  1137,   931,   932,   933,
   284,   285,   286,   287,   288,   289,   290,   295,   296,  1897,
   276,   274,  2063,   295,   114,   276,   201,   299,  1906,   264,
   120,   287,   288,   289,   290,   231,   287,   288,   289,   290,
    59,   127,   132,   292,    59,   292,    65,  2206,   297,   302,
   297,    70,   248,   207,  1185,    70,  2215,  2216,  2217,  2218,
  2219,  2220,  2221,  2222,  2223,   155,  1197,    27,    61,  1200,
   292,  1202,   292,    66,   974,   297,    72,   297,   168,    72,
   276,  1212,    63,  1961,    77,  1520,  1521,   987,  1219,  1232,
  1208,   287,   288,   289,   290,   280,   281,   292,  1504,   111,
   119,   292,   297,    33,   119,   103,   297,   126,  1917,   292,
    40,   126,   292,    43,    59,   134,    46,   297,    67,   134,
   298,    51,   300,    53,    54,  1256,   145,  1258,  1259,  1260,
  1261,    72,   274,   275,   153,   277,  1267,  2178,   280,   182,
   282,  1233,  1273,  1233,  1233,  1212,  1277,  2025,   298,   292,
   300,  1269,  1219,    93,   297,  1286,   292,  1249,  1250,  1249,
  1250,   297,   289,  1294,   292,  1296,  1297,  1298,  1299,   297,
   198,  1678,  2331,  1304,   298,  1306,   300,  1308,   118,  1297,
  1298,   126,  1313,   292,   298,    27,   300,  1279,   297,    59,
  1321,  1322,  1323,   298,  1261,   300,   274,   137,   138,   277,
    70,   298,   280,   300,   282,  1297,  1298,  1297,  1298,    59,
   292,  1617,   231,   295,    67,   297,   231,   299,   292,  1286,
   298,   292,   300,   297,  1355,   292,   297,  1811,   198,   248,
   297,   171,   298,   248,   300,  2276,   299,  1304,  1330,  1306,
   180,   181,  2003,   960,   130,   962,   963,   964,   965,   119,
   292,    67,  1344,   292,   292,   297,   126,   276,   297,   297,
   200,   276,  1393,   299,   134,   284,   285,   286,   287,   288,
   289,   290,   287,   288,   289,   290,   126,  1683,   298,   170,
   300,   300,  1688,  1375,  1376,  1185,   231,  1693,    92,   292,
   292,   292,  1698,  1952,   297,   297,   297,  1703,  2107,   197,
  1200,   292,  1708,   248,   292,   292,   297,  1713,   298,   297,
   297,  1442,  1718,   119,   292,    59,   150,   292,   292,   297,
   292,    65,   297,   297,   280,   297,    70,   292,    86,  1460,
   299,   276,   297,   651,   652,  1427,  2214,    59,   278,   145,
  1432,  1433,   287,   288,   289,   290,   299,   194,   292,   292,
  2228,  2229,   299,   297,   297,  1486,   114,   299,  1489,   292,
   299,   231,   120,  1494,   297,  2059,  1497,  1267,   292,    77,
   128,   292,   292,   297,   132,   119,   297,   297,   248,   292,
   292,   231,   126,   141,   297,   297,   292,   292,  2266,   292,
   134,   297,   297,   280,   297,   282,   158,   155,   248,   299,
  1531,   145,   292,   292,   126,   299,   276,   297,   297,   153,
   168,    49,    50,   296,    52,   285,   286,   287,   288,   289,
   290,   299,   298,  1554,   300,  1556,   276,   292,  1520,  1521,
  1522,   292,   297,   299,   296,   299,   297,   287,   288,   289,
   290,   299,   299,   292,   292,  1537,  1538,   299,   297,   297,
  2328,  1582,  1570,  1584,   298,   292,   300,     3,  1851,  1590,
   297,     7,   292,   299,    10,    11,   299,   297,    14,   292,
   276,  1602,    67,   298,   297,   300,    22,    23,   284,   285,
   286,   287,   288,   289,   290,  1891,   298,   231,   300,   292,
   274,    37,    38,   277,   297,   292,   280,   292,   282,  1591,
   297,  1591,   297,   292,   248,   292,   292,   292,   297,   231,
   297,   297,   297,   295,   298,   298,   300,   300,    64,   299,
   295,   274,   275,    69,   277,   278,   248,   298,   298,   300,
   300,   133,   276,    79,   198,   119,   133,    83,  1669,    85,
   284,   285,   286,   287,   288,   289,   290,   133,   298,    95,
   300,    97,   189,    95,   276,   101,   300,   103,   298,   105,
   300,   295,   298,   109,   300,   287,   288,   289,   290,   115,
   276,   298,   298,   300,   300,   298,   122,   300,   284,   285,
   286,   287,   288,   289,   290,    59,  1486,   301,   301,  1489,
   301,    65,   290,    33,   190,   298,    70,   299,    27,    39,
    40,    41,    42,    43,    44,    45,    46,    47,    48,   299,
    50,    51,    52,    53,    54,    55,    56,    57,    58,   252,
   237,   167,   175,   169,  1755,  2061,   172,   173,   257,   299,
   150,   111,   125,   182,   192,   113,   299,   190,   295,   196,
   295,    77,   195,   189,   172,   119,   284,   285,   182,   202,
   203,   182,   126,   206,   178,   182,   182,   203,   204,  1790,
   134,   198,    59,    86,   217,   300,   212,   213,   300,   299,
  1801,   145,   225,   298,   113,   228,   222,   223,   250,   153,
   295,   274,   298,  1814,   149,   299,    26,    25,   234,   235,
   236,   114,   238,   301,    59,   241,   301,   120,   251,   289,
   253,   247,    82,   150,   153,    70,   259,   284,   261,   132,
   256,   221,   158,  1844,  1845,   353,   150,   263,   141,  1850,
  1851,   262,    67,   226,   270,   284,   300,   298,   284,   298,
  1861,   173,   155,    81,   300,   300,   300,   375,   300,   113,
  1871,   171,  1860,  2327,  1875,   168,   300,  1814,   138,   299,
   201,   299,   299,   299,   119,  1886,   299,   231,   297,   295,
   300,   126,   300,  1881,   187,   300,  1897,   299,   298,   134,
   299,   296,    92,   258,   248,  1906,   299,   299,   299,   284,
   544,   299,   299,  1850,   299,   274,   300,   299,   299,   233,
   299,   299,  1923,   299,   432,   299,    27,   299,   299,   437,
   299,  2194,   276,   299,  1871,   299,   276,   299,   299,   299,
   284,   285,   286,   287,   288,   289,   290,   299,    59,   300,
   299,   129,   299,   299,   299,   129,   300,   299,   299,    70,
  1961,   299,   299,   299,   274,   297,   299,   277,   302,   279,
   280,   302,   282,  1974,   284,   300,   299,   119,   198,   289,
   191,   284,   111,   293,   618,   198,   296,   297,   298,   299,
   300,   301,   302,  1994,   103,    59,   231,   111,  2011,   300,
  2001,    59,   300,   300,   300,   299,   119,   299,   119,    59,
   300,   298,   300,   248,   300,   126,   300,   300,  2006,   300,
  1790,   300,   300,   134,  2025,   300,  2027,   298,   289,   116,
   191,   111,   274,   274,   298,   182,   302,   302,   116,   271,
   299,   276,   299,   111,    67,   153,   182,  2009,   544,  2009,
   285,   286,   287,   288,   289,   290,    49,   229,   115,    52,
  2061,    54,   159,   232,   300,  2053,   299,    60,    59,   299,
    63,   300,   300,   300,    65,   300,   300,   300,   300,    70,
   299,    74,    75,   300,   300,    78,   300,   128,   300,    82,
    83,  1861,  2093,   300,    87,    88,    89,   299,   299,   299,
   608,   299,   299,   299,   299,   299,   299,   299,   299,   155,
   299,   299,   299,   299,   299,   297,   300,   300,   299,   299,
   231,  1891,   618,   300,   297,   300,   128,    72,   119,   300,
   300,   298,   300,   176,   300,   126,   300,   248,   300,   117,
   774,   300,   300,   134,  2145,   188,  2147,   190,   300,   300,
   300,   300,   195,   218,   145,   300,   300,   300,   274,   202,
   203,   300,   670,   206,   672,   276,   300,   300,   300,  2170,
   300,   297,   284,   300,   217,  2176,   287,   288,   289,   290,
   299,   299,   225,    96,   818,   228,    96,   269,   220,   299,
   105,   129,  2154,  2194,  2154,  2157,   299,  2157,  2199,   299,
   834,  2202,   129,   147,   151,   300,   149,   300,   251,   300,
   253,   152,  2200,  2214,   300,   300,   259,   300,   261,   300,
   128,   300,   128,  2224,  1994,   268,  2227,  2228,  2229,  2230,
  2231,  2001,    59,   300,   300,   130,   162,   300,   300,   300,
   231,   300,    65,   299,    59,  2207,  2208,   300,   300,   300,
    65,   300,   300,   300,   299,    70,   300,   248,   298,   295,
   299,   299,  2199,   300,   299,  2266,  2267,   300,   299,   298,
   300,   299,   299,  2274,   300,   300,   300,  2278,   300,   300,
   300,   300,   300,   300,   300,   276,   300,   300,   300,   300,
   300,   300,   285,   284,   285,   286,   287,   288,   289,   290,
  2262,   300,  2262,  2262,   119,   300,  2357,   165,  2309,    59,
   818,   126,   300,   300,  2315,   219,   309,   300,  2319,   134,
  2321,   298,   136,   299,  2325,    65,   834,  2328,   321,   322,
   145,   299,   325,   326,   230,   328,   329,  2325,   153,   300,
   333,   300,   150,   851,   337,   338,   300,   300,   341,   857,
   343,   344,   345,    59,   293,   297,    61,   350,   351,    65,
   300,    67,   293,  2325,    70,  2325,     0,     0,    92,  2370,
  2371,  1266,   702,  2374,   939,  2145,   369,  2147,   452,   617,
  1564,   851,   375,  1582,  1258,  2064,  2374,  1256,  1878,  2274,
  1024,  1025,  1026,  1027,  2348,  2373,  1572,   390,  2338,  2368,
   393,  1584,  1299,  2374,   912,  1298,    86,   400,    71,   401,
   509,   991,  2374,   119,  2374,  2227,   231,  1051,  2340,  2226,
   126,  1055,  1056,  1057,  1058,  2370,   347,   626,   134,  1570,
   490,   424,  2357,   248,    90,  1219,   392,    59,  2324,   145,
   433,   879,  1180,    65,  1303,   418,   439,   153,    70,   442,
  1522,  1313,   445,  1489,  2231,   575,  1214,   871,  1798,  1795,
  1730,   276,  2371,  1378,  1669,  1923,   852,   460,  1801,   284,
   285,   286,   287,   288,   289,   290,   984,   993,  2309,   856,
   281,   427,   912,   476,  1200,   300,   908,  1525,   375,  1204,
   915,  1125,  1126,  1127,  1128,   488,   500,   119,   164,  1487,
  2161,   984,  1183,  2154,   126,   875,    -1,  2157,   929,  1143,
    -1,    -1,   134,    -1,    -1,   508,    -1,    -1,   511,   512,
    -1,    -1,    -1,   145,    -1,   231,    -1,    -1,    -1,    -1,
    -1,   153,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   248,  1051,    -1,    -1,    -1,  1055,  1056,  1057,
  1058,   544,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1068,    -1,    -1,    59,    -1,    -1,  1200,    -1,    -1,    65,
   276,    -1,    -1,    -1,    70,    -1,    -1,    -1,   284,   285,
   286,   287,   288,   289,   290,    39,    40,    41,    42,    43,
    44,    45,    46,    47,    48,    -1,    50,    51,    52,    53,
    54,    55,    56,    57,    58,    -1,    -1,    -1,    -1,   231,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1125,  1126,  1127,
  1128,    -1,    -1,   119,    -1,   618,   248,   620,    -1,   622,
   126,    -1,    -1,    -1,    -1,  1143,    -1,    -1,   134,   632,
    -1,  1137,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   145,
    -1,    -1,    -1,    -1,   276,    -1,    -1,   153,    -1,    -1,
    -1,    -1,   284,   285,   286,   287,   288,   289,   290,    -1,
    -1,    -1,    -1,    -1,    -1,  1183,    -1,   670,   300,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   692,
    -1,    -1,    -1,    -1,  1200,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1346,  1347,    -1,   708,    -1,    -1,    -1,    -1,
    -1,   714,  1356,  1357,  1358,  1359,  1360,  1361,   721,   722,
    -1,    -1,    -1,    -1,   727,   231,    -1,    -1,   731,   732,
    -1,    -1,    -1,    -1,    -1,   738,    -1,  1381,    -1,    -1,
    -1,    -1,   248,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1394,  1395,    -1,    -1,  1398,    -1,  1400,    -1,    -1,  1403,
  1404,  1405,  1406,  1407,  1408,  1409,  1410,    -1,    -1,  1413,
   276,   774,    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,
   286,   287,   288,   289,   290,    59,    -1,  1431,    -1,    -1,
  1434,    65,   298,    -1,    68,    -1,    70,    -1,    -1,  1443,
  1444,  1445,  1446,  1447,  1448,    -1,    -1,    -1,    -1,    -1,
   274,    -1,    -1,   277,    -1,   818,   280,    -1,   282,    -1,
   284,    -1,    -1,    -1,    98,   289,    -1,    -1,   831,    -1,
    -1,   834,   296,   297,   298,   299,   300,    -1,   302,    -1,
    -1,    -1,    -1,    -1,    -1,   119,    -1,    -1,    -1,    -1,
    -1,   854,   126,   856,   857,    -1,    -1,    -1,    -1,    -1,
   134,   864,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   145,    -1,    -1,    -1,    -1,  1394,  1395,    -1,   153,
  1398,    -1,  1400,    -1,    -1,  1403,  1404,  1405,  1406,  1407,
  1408,  1409,  1410,    -1,    -1,  1413,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    59,    -1,   908,    -1,   910,    -1,    65,
    -1,    -1,    -1,  1431,    70,    -1,    -1,   191,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1443,  1444,  1445,  1446,  1447,
  1448,    39,    40,    41,    42,    43,    44,    45,    46,    47,
    48,    -1,    50,    51,    52,    53,    54,    55,    56,    57,
    58,    -1,    -1,    -1,  1460,    -1,    -1,   231,    -1,    -1,
    -1,    -1,    -1,   119,   967,    -1,    -1,    -1,    -1,  1487,
   126,   974,    -1,    -1,   248,    -1,    59,    -1,   134,    -1,
    -1,    -1,   985,    -1,    -1,    -1,    -1,    70,   991,   145,
    -1,    -1,    -1,   996,    -1,    -1,    -1,   153,    -1,    -1,
    -1,    -1,   276,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   284,   285,   286,   287,   288,   289,   290,    -1,    -1,    -1,
    -1,  1024,  1025,  1026,  1027,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1676,    -1,  1678,    -1,   119,    -1,    -1,    -1,
    -1,    -1,    -1,   126,    -1,  1551,    -1,    -1,  1051,    -1,
    -1,   134,  1055,  1056,  1057,  1058,    -1,    -1,    -1,    -1,
    -1,    -1,   145,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   231,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    59,    -1,    -1,
    -1,    -1,   248,    65,    -1,    -1,    -1,    -1,    70,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1756,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   276,    -1,  1125,  1126,  1127,  1128,    -1,    -1,   284,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,    -1,
  1143,    -1,   298,    -1,    -1,    -1,    -1,   119,   231,    -1,
    -1,    -1,    -1,    -1,   126,    -1,    -1,    -1,  1676,    -1,
  1678,    -1,   134,  1669,    -1,   248,   274,    -1,    -1,   277,
    -1,    -1,   280,   145,   282,    -1,   284,    -1,    -1,    -1,
    -1,   289,  1185,    -1,    -1,    -1,    -1,    -1,   296,   297,
   298,   299,   300,   276,  1197,    -1,    -1,  1200,    -1,  1202,
    -1,   284,   285,   286,   287,   288,   289,   290,    -1,  1212,
    -1,    -1,    -1,    -1,    -1,    -1,  1219,    39,    40,    41,
    42,    43,    44,    45,    46,    47,    48,    -1,    50,    51,
    52,    53,    54,    55,    56,    57,    58,    -1,  1756,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1755,
    -1,    -1,    -1,  1256,    -1,  1258,  1259,  1260,  1261,   231,
    -1,    -1,    -1,    -1,  1267,    -1,    -1,    -1,    -1,    -1,
  1273,    -1,    -1,  1917,  1277,    59,   248,    -1,    -1,    -1,
    -1,    65,    -1,  1286,    -1,    -1,    70,    -1,    -1,    -1,
    -1,    -1,    -1,  1296,  1297,  1298,  1299,    -1,    -1,    -1,
    -1,  1304,    -1,  1306,   276,  1308,    -1,    -1,    -1,    -1,
  1313,    -1,   284,   285,   286,   287,   288,   289,   290,  1322,
  1323,    -1,    -1,    -1,    -1,    -1,    -1,  1833,    -1,  1835,
  1836,  1837,  1838,  1839,    -1,   119,    -1,    -1,    -1,    -1,
    -1,    -1,   126,  1346,  1347,    -1,    -1,    -1,    -1,    -1,
   134,    -1,    -1,  1356,  1357,  1358,  1359,  1360,  1361,    -1,
    -1,   145,    -1,    -1,    -1,    -1,  1872,    -1,    -1,   153,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1381,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1394,  1395,    -1,    -1,  1398,    -1,  1400,    -1,  1917,
  1403,  1404,  1405,  1406,  1407,  1408,  1409,  1410,    -1,    -1,
  1413,    -1,    -1,    -1,    -1,    -1,    -1,  1923,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1431,    -1,
    -1,  1434,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1443,  1444,  1445,  1446,  1447,  1448,    -1,   231,    -1,    -1,
    59,    -1,   274,    -1,    -1,   277,    -1,    -1,   280,    -1,
   282,    70,   284,  2107,   248,    -1,    -1,   289,  1974,    -1,
    -1,    -1,    -1,    -1,   296,   297,   298,   299,   300,    -1,
   302,    -1,    -1,  1486,    -1,    -1,  1489,    -1,    -1,    -1,
    -1,  1494,   276,    -1,  1497,    -1,    -1,    -1,    -1,    -1,
   284,   285,   286,   287,   288,   289,   290,    -1,    -1,    -1,
   119,    -1,  2018,    -1,   298,    -1,    59,   126,    -1,    -1,
  2026,  2027,    65,    -1,  2030,   134,  2032,    70,  1531,  2035,
  2036,  2037,  2038,  2039,  2040,  2041,  2042,    -1,  2044,    -1,
    -1,    -1,    -1,  2049,    -1,    -1,    -1,    -1,  1551,    59,
    -1,  1554,    -1,    -1,    -1,    65,    -1,  2063,    -1,    -1,
    70,    -1,    -1,    -1,    -1,    -1,  2072,  2073,  2074,  2075,
  2076,    -1,    -1,    -1,    -1,    -1,   119,    -1,    -1,  1582,
    -1,  1584,    -1,   126,    -1,    -1,    -1,  1590,    -1,  2107,
    -1,   134,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   145,   113,    -1,    -1,    -1,    -1,    -1,   119,
   153,    -1,    -1,    -1,    -1,    49,   126,    -1,    52,    -1,
    54,    -1,   231,    -1,   134,    -1,    60,    -1,    -1,    63,
    -1,    -1,    -1,    -1,   177,   145,    -1,    -1,    -1,   248,
    74,    75,    -1,   153,    78,    -1,    -1,    -1,    82,    83,
    -1,    -1,    -1,    87,    88,    89,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2170,    -1,    -1,   276,    -1,    -1,
  2176,    -1,  2178,  1676,    -1,  1678,   285,   286,   287,   288,
   289,   290,    -1,    -1,    -1,    -1,    -1,    -1,   231,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  2206,    -1,    -1,    -1,    -1,   248,    -1,    -1,    -1,  2215,
  2216,  2217,  2218,  2219,  2220,  2221,  2222,  2223,    -1,    -1,
    -1,   231,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   276,    -1,    -1,    -1,    -1,   248,    -1,
    -1,   284,   285,   286,   287,   288,   289,   290,    -1,    -1,
    -1,    -1,    -1,  1756,    59,    -1,    -1,    -1,    -1,    -1,
    65,  2267,    67,    -1,    -1,    70,   276,    -1,  2274,    -1,
  2276,    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,
   290,    -1,    -1,    -1,    -1,    -1,    -1,  1790,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1801,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1814,    -1,  2319,   119,  2321,    -1,    -1,    -1,    -1,
    -1,   126,    -1,    -1,    -1,  2331,    -1,    -1,    -1,   134,
  1833,    -1,  1835,  1836,  1837,  1838,  1839,    -1,    -1,    -1,
   145,  1844,  1845,    -1,    -1,    -1,    -1,  1850,   153,    -1,
    -1,   285,    -1,    -1,    -1,    -1,    -1,    -1,  1861,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1871,  1872,
    -1,    -1,  1875,    -1,    -1,   309,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1886,    -1,    -1,    -1,   321,   322,    -1,
    -1,   325,   326,    -1,   328,   329,    -1,    -1,    -1,   333,
    -1,    -1,    -1,   337,   338,    -1,    -1,   341,    -1,   343,
   344,   345,    -1,    -1,  1917,    -1,   350,   351,    -1,   326,
    -1,    -1,    -1,    -1,    -1,    -1,   231,    -1,    59,    -1,
    -1,    -1,    -1,    -1,    65,   369,    67,    -1,   345,    70,
    -1,   375,    -1,   248,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   390,    -1,    -1,   393,
    -1,    -1,    -1,    -1,    -1,    -1,   400,    -1,    -1,    -1,
    -1,   276,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   284,
   285,   286,   287,   288,   289,   290,    -1,    -1,   119,    -1,
   424,  1994,    -1,    -1,    -1,   126,    -1,    -1,  2001,   433,
    -1,    -1,    -1,   134,    -1,   439,    -1,    -1,   442,    -1,
    -1,   445,    -1,    -1,   145,  2018,    -1,    -1,    -1,    -1,
    -1,    -1,   153,  2026,    -1,    -1,   460,  2030,    -1,  2032,
    -1,    -1,  2035,  2036,  2037,  2038,  2039,  2040,  2041,  2042,
    -1,  2044,   476,    -1,    -1,    -1,  2049,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   488,    -1,    -1,    -1,  2061,    -1,
  2063,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2072,
  2073,  2074,  2075,  2076,   508,    -1,    -1,   511,   512,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  2093,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   231,    -1,    -1,    -1,  2107,    -1,    -1,    -1,    -1,    -1,
   544,    -1,    -1,    -1,    -1,    -1,    -1,   248,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   559,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   575,  2145,    -1,  2147,   276,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,   290,
    -1,    -1,    59,    -1,    -1,    -1,    -1,    -1,    65,    -1,
    -1,    -1,    -1,    70,    -1,  2178,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   618,    -1,   620,    -1,   622,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  2199,    -1,   632,  2202,
    -1,    -1,    -1,  2206,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  2215,  2216,  2217,  2218,  2219,  2220,  2221,  2222,
  2223,  2224,   119,    -1,  2227,    -1,    -1,    -1,  2231,   126,
    -1,    -1,    -1,    -1,    -1,    -1,   670,   134,    -1,     7,
    -1,    -1,    10,    11,    -1,    -1,    14,    -1,   145,    -1,
    -1,    -1,    -1,    -1,    22,    23,   153,    -1,   692,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    37,
    38,    -1,    -1,  2276,   708,  2278,    -1,    -1,    -1,    -1,
   714,    -1,    -1,    -1,    -1,    -1,    -1,   721,   722,    59,
    -1,    -1,    -1,   727,    -1,    65,    64,   731,   732,    -1,
    70,    69,    -1,    -1,   738,    -1,    -1,    -1,    -1,    -1,
    -1,    79,  2315,    -1,    -1,    83,    -1,    85,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   759,    -1,    95,  2331,    97,
    -1,    -1,    -1,   101,   231,   103,    -1,   105,    -1,    -1,
    -1,   109,    -1,    -1,    -1,    -1,    -1,   115,    -1,   119,
    -1,   248,    -1,    -1,   122,    -1,   126,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   134,    -1,    -1,   774,  2371,    -1,
    -1,    -1,    -1,    -1,    -1,   145,    -1,    -1,    -1,   276,
    -1,    -1,    -1,   153,    -1,    -1,    -1,   284,   285,   286,
   287,   288,   289,   290,    -1,    -1,    -1,   831,   832,   167,
    -1,   169,    -1,    -1,   172,   173,    -1,    -1,    -1,    -1,
    -1,   818,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   854,   189,   856,   857,    -1,    -1,    -1,   834,    -1,    -1,
   864,    -1,    -1,    -1,    -1,   203,   204,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   212,   213,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   222,   223,    -1,    -1,    -1,    -1,
    -1,   231,    -1,    -1,    -1,    -1,   234,   235,   236,    -1,
   238,    -1,    -1,   241,   908,    -1,   910,    -1,   248,   247,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   256,    -1,
    -1,    -1,    -1,    -1,    -1,   263,    -1,    -1,    -1,    -1,
    -1,    -1,   270,    -1,    -1,    -1,   276,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,
   290,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   299,    -1,   967,    -1,    -1,    -1,    -1,    -1,    -1,
   974,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   985,    -1,    -1,    -1,    -1,    -1,   991,    -1,    -1,
    -1,    -1,   996,    -1,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1024,  1025,  1026,
  1027,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1051,    -1,    -1,    -1,  1055,  1056,
  1057,  1058,    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,    -1,    -1,    -1,   124,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   137,   138,
    -1,    -1,    -1,  1137,    -1,    59,    -1,    -1,    -1,   148,
    -1,    65,    -1,    -1,    -1,    -1,    70,    -1,  1125,  1126,
  1127,  1128,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   171,    -1,    -1,    -1,  1143,    -1,    -1,   178,
   179,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1185,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   200,    -1,  1197,   203,   119,  1200,    -1,  1202,    -1,
    -1,    -1,   126,    -1,    -1,    -1,    -1,    -1,  1212,    -1,
   134,    -1,    -1,    -1,    -1,  1219,    -1,    -1,    -1,    -1,
    -1,   145,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   153,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1256,    -1,  1258,  1259,  1260,  1261,    -1,    -1,
    -1,    -1,    -1,  1267,    -1,   274,    -1,    -1,   277,  1273,
    -1,    -1,    -1,  1277,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1286,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1294,    -1,  1296,  1297,  1298,  1299,    -1,    -1,    -1,    -1,
  1304,    -1,  1306,    -1,  1308,    -1,    -1,   231,    -1,  1313,
    -1,    59,    -1,    -1,    -1,    -1,    -1,  1321,  1322,  1323,
    -1,    -1,    70,    -1,   248,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    59,    -1,    -1,    -1,    -1,   562,
    65,    -1,    -1,    -1,    -1,    70,    -1,    -1,    -1,    -1,
    -1,  1355,   276,   576,    -1,   578,    -1,   580,   581,   582,
   284,   285,   286,   287,   288,   289,   290,    -1,    -1,  1346,
  1347,   119,    -1,    -1,  1378,    -1,    -1,    59,   126,  1356,
  1357,  1358,  1359,  1360,  1361,    -1,   134,    -1,    70,  1393,
    -1,    -1,    -1,    -1,   119,    -1,    -1,   145,    -1,    -1,
    -1,   126,    -1,    -1,  1381,    -1,    -1,    -1,    -1,   134,
    -1,    -1,    -1,    -1,    -1,   638,    -1,  1394,  1395,    -1,
   145,  1398,    -1,  1400,    59,    -1,  1403,  1404,  1405,  1406,
  1407,  1408,  1409,  1410,    -1,    70,  1413,   119,  1442,    -1,
    -1,    -1,    -1,    -1,   126,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   134,    -1,  1431,    -1,  1460,  1434,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1443,  1444,  1445,  1446,
  1447,  1448,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1486,   231,   119,  1489,    -1,    -1,    -1,    -1,
  1494,   126,    -1,  1497,    -1,    -1,   719,    -1,    -1,   134,
   248,    -1,    -1,    -1,    -1,    -1,   231,    -1,    59,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    70,
    -1,    -1,    -1,   248,    -1,   748,    -1,  1531,   276,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,   286,   287,
   288,   289,   290,    -1,    -1,    -1,    -1,    -1,   771,   231,
  1554,   276,  1556,    -1,    -1,   778,    -1,    -1,    -1,   284,
   285,   286,   287,   288,   289,   290,   248,    -1,   119,    -1,
    -1,    -1,    -1,    -1,  1551,   126,    -1,    -1,  1582,    -1,
  1584,    -1,    -1,   134,    -1,    -1,  1590,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   276,   818,   231,    -1,    -1,    -1,
    -1,    -1,   284,   285,   286,   287,   288,   289,   290,    -1,
   833,    -1,    -1,   248,   837,    -1,   839,    -1,    -1,   842,
   843,   844,   845,   846,   847,   848,   849,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   276,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   284,
   285,   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1669,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   231,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   248,    -1,  1676,
    -1,  1678,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   276,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,   290,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1755,    -1,    -1,    -1,   774,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   992,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1756,
    -1,    -1,    -1,    -1,    -1,    -1,  1790,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1801,    -1,   818,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1814,    -1,    -1,    -1,  1037,   834,  1039,  1040,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1051,    -1,
    -1,    -1,  1055,  1056,  1057,  1058,  1059,    -1,    -1,    -1,
  1844,  1845,    -1,    -1,    -1,    -1,  1850,    -1,  1071,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1833,  1861,  1835,  1836,
  1837,  1838,  1839,    -1,    -1,    -1,    -1,  1871,    -1,    -1,
    -1,  1875,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1886,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1897,    -1,  1872,   758,    -1,    -1,    -1,
    -1,    -1,  1906,    -1,    -1,    -1,  1129,    -1,    -1,  1886,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1923,
    -1,    -1,  1145,    -1,    -1,    -1,  1149,    -1,    -1,    -1,
    -1,  1154,    -1,    -1,    -1,  1158,    -1,    -1,    -1,  1162,
  1917,    -1,    -1,  1166,    -1,    -1,    -1,  1170,    -1,    -1,
    -1,  1174,    -1,    -1,    -1,  1178,    -1,  1961,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1974,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1994,  1214,    -1,    -1,    -1,    -1,    -1,  2001,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1024,  1025,  1026,  1027,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2025,    -1,  2027,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1051,    -1,    -1,    -1,  1055,  1056,  1057,  1058,
    -1,  2018,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2026,
    -1,    -1,    -1,  2030,    -1,  2032,    -1,  2061,  2035,  2036,
  2037,  2038,  2039,  2040,  2041,  2042,    -1,  2044,    -1,    -1,
    -1,    -1,  2049,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  2063,    -1,    -1,  2093,
    -1,    -1,    -1,    -1,    -1,  2072,  2073,  2074,  2075,  2076,
    -1,    -1,  1325,    -1,    -1,    -1,  1125,  1126,  1127,  1128,
    -1,  1334,    -1,    -1,    -1,    -1,    -1,    -1,  1137,    -1,
  1343,    -1,    -1,    -1,  1143,    -1,  1349,    -1,    -1,    -1,
  2107,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2145,    -1,  2147,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1374,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1383,    -1,    -1,    -1,    -1,    -1,  2170,    -1,    -1,    -1,
    -1,  1394,  2176,    -1,    -1,  1398,    -1,  1400,    -1,    -1,
  1403,  1404,  1405,  1406,  1407,  1408,  1409,  1410,    -1,    -1,
  1413,    -1,    -1,    -1,    -1,  2199,    -1,    -1,  2202,    -1,
    -1,  2178,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  2214,    -1,    -1,  1436,    -1,    -1,    -1,    -1,    -1,    -1,
  2224,    -1,    -1,  2227,  2228,  2229,    -1,  2231,    -1,  2206,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2215,  2216,
  2217,  2218,  2219,  2220,  2221,  2222,  2223,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  2266,  2267,    -1,    -1,    -1,    -1,    -1,    -1,
  2274,    -1,    -1,    -1,  2278,  1137,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1149,    -1,    -1,
    -1,    -1,  1154,    -1,    -1,    -1,  1158,    -1,    -1,  2276,
  1162,    -1,    -1,    -1,  1166,    -1,    -1,    -1,  1170,    -1,
    -1,  2315,  1174,    -1,    -1,  2319,  1178,  2321,    -1,    -1,
    -1,    -1,    -1,    -1,  2328,    -1,    -1,  1346,  1347,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1356,  1357,  1358,
  1359,  1360,  1361,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2331,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1381,    -1,    -1,    -1,    -1,  2371,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1394,  1395,    -1,    -1,  1398,
    -1,  1400,    -1,    -1,  1403,  1404,  1405,  1406,  1407,  1408,
  1409,  1410,    -1,    -1,  1413,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1632,
  1633,  1634,  1431,    -1,    -1,  1434,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1443,  1444,  1445,  1446,  1447,  1448,
    -1,    -1,    -1,  1295,    -1,    -1,    -1,  1660,  1661,  1662,
    -1,  1460,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1678,    -1,    -1,    -1,    -1,
  1683,    -1,    -1,    -1,    -1,  1688,    -1,    -1,    -1,    -1,
  1693,    -1,    -1,    -1,    -1,  1698,    -1,    -1,    -1,    -1,
  1703,    -1,    -1,    -1,    -1,  1708,    -1,    -1,    -1,    -1,
  1713,    -1,    -1,    -1,    -1,  1718,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1730,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1377,    -1,  1740,  1741,  1742,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1551,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1460,    -1,
  1462,  1463,    -1,  1465,  1466,    -1,  1468,  1469,    -1,  1471,
  1472,    -1,  1474,  1475,    -1,  1477,  1478,    -1,  1480,  1481,
    -1,  1483,  1484,    -1,    76,    77,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,
  1669,    -1,    -1,    -1,    -1,    -1,   108,  1676,   110,  1678,
    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,    -1,
    -1,    -1,   124,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   137,   138,    -1,   140,    -1,
   142,    -1,    -1,    -1,    -1,    -1,   148,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   171,
   172,    -1,    -1,    -1,    -1,    -1,   178,   179,    -1,    -1,
    -1,    -1,    -1,    -1,   186,    -1,  1755,  1756,    -1,    -1,
    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,    -1,
    -1,   203,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2004,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1669,    -1,    -1,
   262,    -1,    -1,    -1,  1833,    -1,  1835,  1836,  1837,  1838,
  1839,  1683,   274,   275,    -1,   277,  1688,    -1,   280,   281,
   282,  1693,    -1,    -1,    -1,    -1,  1698,    -1,    -1,    -1,
    -1,  1703,    -1,    -1,    -1,    -1,  1708,    -1,    -1,    -1,
    -1,  1713,    -1,  1872,    -1,    -1,  1718,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1726,    -1,    -1,    -1,  1730,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1917,    -1,
    -1,    -1,    -1,    -1,  1923,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1974,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2018,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2026,  2027,    -1,
    -1,  2030,    -1,  2032,    -1,    -1,  2035,  2036,  2037,  2038,
  2039,  2040,  2041,  2042,    -1,  2044,    -1,    -1,    -1,    -1,
  2049,    -1,    -1,    -1,    -1,    -1,  2259,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2063,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  2072,  2073,  2074,  2075,  2076,    -1,    -1,
    -1,  1923,    -1,    -1,  1926,  1927,    -1,  1929,  1930,    -1,
  1932,  1933,    -1,  1935,  1936,    -1,  1938,  1939,    -1,  1941,
  1942,    -1,  1944,  1945,    -1,  1947,  1948,    -1,  2107,    -1,
    -1,    -1,  1954,    -1,    -1,    -1,  1958,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2170,    -1,    -1,    -1,    -1,    -1,  2176,    -1,  2178,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2206,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  2215,  2216,  2217,  2218,
  2219,  2220,  2221,  2222,  2223,    -1,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,  2267,    -1,
    -1,    -1,    -1,    -1,    -1,  2274,    -1,  2276,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,    76,
    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,
    87,    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,
  2319,    -1,  2321,   100,    -1,    -1,    -1,    -1,    -1,   106,
   107,   108,  2331,   110,    -1,    -1,    -1,    -1,    -1,    -1,
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
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,   288,   289,    -1,   291,    -1,   293,    -1,    -1,    -1,
    -1,    -1,   299,   300,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    74,    75,    76,    77,    -1,
    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,
    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,   108,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,   145,   146,   147,   148,
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
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,   262,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,   278,
    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,
   289,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,
   299,   300,     3,     4,     5,     6,     7,     8,     9,    10,
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
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
   261,   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,   275,   276,   277,   278,    -1,   280,
   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,   262,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,   275,   276,   277,   278,    -1,   280,   281,   282,
    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,
   293,    -1,    -1,    -1,    -1,    -1,   299,   300,     3,     4,
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
    -1,    -1,    -1,   148,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
    -1,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
   255,    -1,   257,   258,   259,   260,   261,   262,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
   275,   276,   277,    -1,    -1,   280,   281,   282,    -1,    -1,
    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,
    -1,    -1,    -1,    -1,   299,   300,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,
    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,
    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,    -1,
   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,   186,
    -1,    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,    -1,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,
    -1,    -1,   299,   300,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    76,    77,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,    -1,    -1,    -1,   148,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,    -1,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,   300,    26,    -1,    28,    29,    30,    31,    32,    -1,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
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
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
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
    -1,    71,    -1,    -1,    74,    75,    76,    77,    -1,    -1,
    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,
    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,   106,   107,   108,    -1,
   110,    -1,    -1,   113,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,
   130,   131,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
   140,    -1,   142,   143,    -1,   145,   146,   147,   148,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,   174,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,   183,    -1,   185,   186,    -1,    -1,    -1,
   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
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
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    71,    -1,
    -1,    74,    75,    76,    77,    -1,    -1,    80,    -1,    -1,
    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,
   113,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,   124,    -1,    -1,    -1,    -1,   129,   130,   131,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,   145,   146,   147,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
   183,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,   262,
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
   146,   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,
   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
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
    -1,   140,    -1,   142,   143,    -1,    -1,   146,    -1,   148,
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
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,   262,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,   278,
    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,
    -1,    -1,   291,   292,   293,    -1,    -1,    -1,   297,    -1,
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
   142,   143,    -1,   145,   146,   147,   148,   149,    -1,   151,
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
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
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
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
   255,    -1,   257,   258,   259,   260,   261,   262,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
   275,   276,   277,   278,    -1,   280,   281,   282,    -1,    -1,
    -1,    -1,    -1,   288,   289,    -1,   291,    -1,   293,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
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
   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,
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
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
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
   140,    -1,   142,   143,    -1,   145,   146,    -1,   148,   149,
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
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,   262,
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
   106,   107,   108,    -1,   110,    -1,    -1,   113,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,
   146,   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,
   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
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
    -1,    -1,    -1,    -1,    -1,    74,    75,    76,    77,    -1,
    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,
    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,   108,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,   145,   146,   147,   148,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,   172,    -1,   174,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,
    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
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
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
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
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
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
   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,
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
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
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
   170,   171,    -1,    -1,   174,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,
   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
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
   143,    -1,    -1,   146,    -1,   148,   149,    -1,   151,   152,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,   262,
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
   166,    -1,    -1,    -1,   170,   171,   172,    -1,   174,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,
   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
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
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
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
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
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
    -1,    -1,    87,    88,    89,    90,    91,    -1,    93,    94,
    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,
    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,
    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,
    -1,    -1,    -1,   148,   149,    -1,   151,   152,    -1,    -1,
    -1,    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,
    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,
    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,
   185,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,
    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,
   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,
   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,
   225,   226,   227,   228,   229,   230,   231,   232,    -1,    -1,
    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
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
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    87,
    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,   140,    -1,   142,   143,    -1,    -1,    -1,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
    -1,    -1,    -1,    -1,    75,    76,    77,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    88,    89,    90,
    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,   110,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,
    -1,   142,   143,    -1,    -1,    -1,   147,   148,   149,    -1,
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
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
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
    -1,    -1,    -1,    -1,    88,    89,    90,    91,    -1,    93,
    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,
    -1,    -1,    -1,    -1,   148,   149,    -1,   151,   152,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,
    -1,    -1,   186,    -1,    -1,    -1,   190,    -1,    -1,   193,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
    -1,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,   275,   276,   277,    -1,    -1,   280,   281,   282,    -1,
    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,   293,
    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,
    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,
    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,   140,    -1,   142,   143,    -1,    -1,    -1,
    -1,   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,   186,
    -1,    -1,    -1,   190,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,    -1,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,
    -1,    -1,   299,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    75,    76,    77,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    87,    -1,    -1,
    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,
   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
   140,    -1,   142,   143,    -1,    -1,    -1,    -1,   148,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,   174,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,
    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
   260,   261,   262,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,   275,   276,   277,    -1,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,
    -1,   291,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   299,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    75,    76,    77,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    87,    -1,    -1,    -1,    -1,    -1,
    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   108,    -1,   110,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,
    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,
    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,
   143,    -1,    -1,    -1,    -1,   148,   149,    -1,   151,   152,
    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,    -1,
    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,
    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,    -1,
    -1,    -1,   185,   186,    -1,    -1,    -1,    -1,    -1,    -1,
   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,
   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,
    -1,   214,   215,   216,   217,   218,    -1,   220,   221,    -1,
    -1,   224,   225,   226,   227,   228,   229,   230,   231,   232,
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,   262,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,   275,   276,   277,    -1,    -1,   280,   281,   282,
    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,   291,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   299,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,
    76,    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,
    -1,   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,
   186,    -1,    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
    -1,   257,   258,   259,   260,   261,   262,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,
   276,   277,    -1,    -1,   280,   281,   282,    -1,    -1,    -1,
    -1,    -1,   288,    -1,    -1,   291,    -1,   293,    -1,    -1,
    -1,    -1,    -1,   299,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,    -1,    -1,   147,   148,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,    -1,   186,    -1,    -1,
    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,   262,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,    -1,
    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,
    -1,    -1,   291,    -1,   293,    -1,    -1,    -1,    -1,    -1,
   299,     3,     4,     5,     6,     7,     8,     9,    10,    11,
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
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,    -1,    -1,   277,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,   299,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,
    -1,    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,   172,    -1,    -1,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
   257,   258,   259,   260,   261,    -1,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,
   277,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,   299,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
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
    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,   275,   276,   277,    -1,    -1,   280,    -1,
   282,    -1,   284,   285,   286,   287,   288,   289,   290,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,
    -1,    -1,    96,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
    -1,    -1,    -1,    -1,   128,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,   143,
    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,   162,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,   276,   277,    -1,    -1,    -1,    -1,    -1,    -1,
   284,   285,   286,   287,   288,   289,   290,     3,     4,     5,
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
    -1,    -1,   128,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,
    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   160,   161,   162,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
   276,   277,    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,
   286,   287,   288,   289,   290,     3,     4,     5,     6,     7,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
   258,   259,   260,   261,    -1,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,
    -1,    -1,    -1,   281,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   289,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    87,    -1,    -1,    -1,
    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,    -1,
    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,   289,    15,
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
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
    -1,   277,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,   289,    15,    16,    17,    18,    19,    20,
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
   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,   289,    15,
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
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
    -1,   277,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,   289,    15,    16,    17,    18,    19,    20,
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
   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,   289,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    76,    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,    -1,    -1,    -1,   124,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   137,   138,    -1,   140,    -1,   142,    -1,    -1,    -1,
    -1,    -1,   148,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   171,    -1,    -1,    -1,    -1,
    -1,    -1,   178,   179,    -1,    -1,    -1,    -1,    -1,    -1,
   186,    -1,    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,    -1,    -1,   203,    -1,    -1,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   262,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   274,   275,
    -1,   277,    -1,    -1,   280,   281,   282,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    87,    -1,    -1,    -1,    -1,    -1,
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
    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,
   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,
   273,   274,    -1,    -1,   277,    -1,    -1,    -1,   281,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    63,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,
    -1,    -1,    96,    -1,    -1,    -1,   100,    -1,    -1,    -1,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,   275,    -1,   277,   278,     3,     4,     5,     6,     7,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
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
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
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
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
   260,   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,    -1,    -1,   277,   278,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    74,    -1,    -1,    -1,    -1,    -1,    80,    -1,    -1,    -1,
    84,    -1,    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,
    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,   102,   103,
    -1,    -1,    -1,    -1,   108,    -1,    -1,    -1,   112,    -1,
    -1,    -1,   116,    -1,   118,    -1,    -1,   121,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,   143,
    -1,    -1,   146,    -1,    -1,   149,    -1,   151,   152,    -1,
   154,    -1,    -1,   157,   158,    -1,   160,   161,    -1,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
    -1,   175,    -1,   177,   178,   179,   180,   181,    -1,    -1,
   184,    -1,   186,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   196,    -1,    -1,    -1,   200,   201,   202,   203,
   204,   205,   206,   207,   208,   209,   210,   211,   212,   213,
   214,   215,   216,   217,   218,   219,   220,   221,   222,   223,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
   234,    -1,   236,   237,   238,   239,   240,   241,   242,   243,
   244,   245,   246,    -1,   248,    -1,   250,   251,   252,   253,
    -1,   255,   256,   257,   258,   259,   260,   261,   262,   263,
   264,   265,   266,   267,   268,    -1,   270,   271,   272,   273,
   274,    -1,    -1,   277,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    76,    77,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,   140,    -1,   142,   143,    -1,    -1,    -1,    -1,   148,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,    -1,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    78,    -1,    -1,    -1,    -1,    -1,
    84,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,
    -1,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   112,    -1,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,    -1,    -1,    -1,   143,
    -1,    -1,    -1,    -1,    -1,   149,    -1,   151,   152,    -1,
    -1,    -1,    -1,    -1,    -1,   159,   160,   161,    -1,    -1,
   164,    -1,   166,    -1,    -1,    -1,   170,   171,    -1,    -1,
    -1,    -1,    -1,    -1,   178,   179,   180,   181,    -1,    -1,
    -1,    -1,    -1,    -1,   188,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
   139,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,   184,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,    -1,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,
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
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,   242,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   116,    -1,   118,
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,   163,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,    -1,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,
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
   184,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,   202,   203,
    -1,   205,   206,    -1,   208,   209,    -1,   211,    -1,    -1,
   214,   215,   216,   217,   218,    -1,   220,   221,    -1,    -1,
   224,   225,   226,   227,   228,   229,   230,   231,   232,    -1,
    -1,    -1,    -1,   237,    -1,   239,   240,    -1,    -1,   243,
   244,    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,   184,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,    -1,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,
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
   244,   245,   246,    -1,   248,    -1,   250,   251,   252,   253,
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,   245,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,    -1,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    67,    -1,    -1,    -1,    -1,    -1,    -1,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,    -1,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,
    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,   180,   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,   208,
   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,
    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,   228,
   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,
   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
   259,   260,   261,    -1,    -1,   264,   265,   266,   267,   268,
    -1,    -1,   271,    -1,   273,   274,    -1,    -1,   277,     3,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    -1,    13,    -1,    15,    16,    17,    18,
    19,    20,    21,    22,    23,    24,    -1,    26,    -1,    28,
    29,    30,    31,    32,    -1,    34,    35,    36,    37,    38,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    76,    77,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,
    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,
    -1,    -1,    -1,    -1,    -1,   124,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   137,   138,
    -1,   140,    -1,   142,    -1,    -1,    -1,    -1,    -1,   148,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,
   179,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,
    -1,   200,    -1,    -1,   203,    -1,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   260,    -1,   262,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   274,    -1,    -1,   277,    76,
    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   118,    -1,    -1,    -1,    -1,    -1,   124,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   137,   138,    -1,   140,    -1,   142,    -1,    -1,    -1,    -1,
    -1,   148,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   171,    -1,    -1,    -1,    -1,    -1,
    -1,   178,   179,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,    -1,    -1,   203,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   262,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   274,    -1,    -1,
   277
};
/* -*-C-*-  Note some compilers choke on comments on `#line' lines.  */
#line 3 "/usr/local/bison/bison.simple"

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

/* Prevent warning if -Wstrict-prototypes.  */
#ifdef __GNUC__
int yyparse (void);
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

#line 196 "/usr/local/bison/bison.simple"

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

case 4:
#line 842 "preproc.y"
{ connection = NULL; ;
    break;}
case 7:
#line 845 "preproc.y"
{ fprintf(yyout, "%s", yyvsp[0].str); free(yyvsp[0].str); ;
    break;}
case 8:
#line 846 "preproc.y"
{ fprintf(yyout, "%s", yyvsp[0].str); free(yyvsp[0].str); ;
    break;}
case 9:
#line 847 "preproc.y"
{ fputs(yyvsp[0].str, yyout); free(yyvsp[0].str); ;
    break;}
case 10:
#line 848 "preproc.y"
{ fputs(yyvsp[0].str, yyout); free(yyvsp[0].str); ;
    break;}
case 11:
#line 850 "preproc.y"
{ connection = yyvsp[0].str; ;
    break;}
case 12:
#line 852 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 13:
#line 853 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 14:
#line 854 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 15:
#line 855 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 16:
#line 856 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 17:
#line 857 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 18:
#line 858 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 19:
#line 859 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 20:
#line 860 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 21:
#line 861 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 22:
#line 862 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 23:
#line 863 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 24:
#line 864 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 25:
#line 865 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 26:
#line 866 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 27:
#line 867 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 28:
#line 868 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 29:
#line 869 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 30:
#line 870 "preproc.y"
{ output_statement(yyvsp[0].str, 1); ;
    break;}
case 31:
#line 871 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 32:
#line 872 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 33:
#line 873 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 34:
#line 874 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 35:
#line 875 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 36:
#line 876 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 37:
#line 877 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 38:
#line 878 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 39:
#line 879 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 40:
#line 880 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 41:
#line 881 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 42:
#line 882 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 43:
#line 883 "preproc.y"
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
case 44:
#line 892 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 45:
#line 893 "preproc.y"
{
						fprintf(yyout, "ECPGtrans(__LINE__, %s, \"%s\");", connection ? connection : "NULL", yyvsp[0].str);
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 46:
#line 898 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 47:
#line 899 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 48:
#line 900 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 49:
#line 901 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 50:
#line 902 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 51:
#line 903 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 52:
#line 904 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 53:
#line 905 "preproc.y"
{ output_statement(yyvsp[0].str, 0); ;
    break;}
case 54:
#line 906 "preproc.y"
{
						if (connection)
							yyerror("no at option for connect statement.\n");

						fprintf(yyout, "no_auto_trans = %d;\n", no_auto_trans);
						fprintf(yyout, "ECPGconnect(__LINE__, %s);", yyvsp[0].str);
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 55:
#line 915 "preproc.y"
{
						fputs(yyvsp[0].str, yyout);
                                                free(yyvsp[0].str); 
					;
    break;}
case 56:
#line 919 "preproc.y"
{
						if (connection)
							yyerror("no at option for connect statement.\n");

						fputs(yyvsp[0].str, yyout);
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 57:
#line 927 "preproc.y"
{
						fputs(yyvsp[0].str, yyout);
						free(yyvsp[0].str);
					;
    break;}
case 58:
#line 931 "preproc.y"
{
						if (connection)
							yyerror("no at option for disconnect statement.\n");

						fprintf(yyout, "ECPGdisconnect(__LINE__, \"%s\");", yyvsp[0].str); 
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 59:
#line 939 "preproc.y"
{
						output_statement(yyvsp[0].str, 0);
					;
    break;}
case 60:
#line 942 "preproc.y"
{
						fprintf(yyout, "ECPGdeallocate(__LINE__, %s, \"%s\");", connection ? connection : "NULL", yyvsp[0].str); 
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 61:
#line 947 "preproc.y"
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
case 62:
#line 972 "preproc.y"
{
						if (connection)
							yyerror("no at option for set connection statement.\n");

						fprintf(yyout, "ECPGprepare(__LINE__, %s);", yyvsp[0].str); 
						whenever_action(0);
						free(yyvsp[0].str);
					;
    break;}
case 63:
#line 980 "preproc.y"
{ /* output already done */ ;
    break;}
case 64:
#line 981 "preproc.y"
{
						if (connection)
							yyerror("no at option for set connection statement.\n");

						fprintf(yyout, "ECPGsetconn(__LINE__, %s);", yyvsp[0].str);
						whenever_action(0);
                                       		free(yyvsp[0].str);
					;
    break;}
case 65:
#line 989 "preproc.y"
{
						if (connection)
							yyerror("no at option for typedef statement.\n");

						fputs(yyvsp[0].str, yyout);
                                                free(yyvsp[0].str);
					;
    break;}
case 66:
#line 996 "preproc.y"
{
						if (connection)
							yyerror("no at option for var statement.\n");

						fputs(yyvsp[0].str, yyout);
                                                free(yyvsp[0].str);
					;
    break;}
case 67:
#line 1003 "preproc.y"
{
						if (connection)
							yyerror("no at option for whenever statement.\n");

						fputs(yyvsp[0].str, yyout);
						output_line_number();
						free(yyvsp[0].str);
					;
    break;}
case 68:
#line 1027 "preproc.y"
{
					yyval.str = cat3_str(cat5_str(make1_str("create user"), yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 69:
#line 1041 "preproc.y"
{
					yyval.str = cat3_str(cat5_str(make1_str("alter user"), yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 70:
#line 1054 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop user"), yyvsp[0].str);
				;
    break;}
case 71:
#line 1059 "preproc.y"
{ yyval.str = cat2_str(make1_str("with password") , yyvsp[0].str); ;
    break;}
case 72:
#line 1060 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 73:
#line 1064 "preproc.y"
{
					yyval.str = make1_str("createdb");
				;
    break;}
case 74:
#line 1068 "preproc.y"
{
					yyval.str = make1_str("nocreatedb");
				;
    break;}
case 75:
#line 1071 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 76:
#line 1075 "preproc.y"
{
					yyval.str = make1_str("createuser");
				;
    break;}
case 77:
#line 1079 "preproc.y"
{
					yyval.str = make1_str("nocreateuser");
				;
    break;}
case 78:
#line 1082 "preproc.y"
{ yyval.str = NULL; ;
    break;}
case 79:
#line 1086 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 80:
#line 1090 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 81:
#line 1095 "preproc.y"
{ yyval.str = cat2_str(make1_str("in group"), yyvsp[0].str); ;
    break;}
case 82:
#line 1096 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 83:
#line 1099 "preproc.y"
{ yyval.str = cat2_str(make1_str("valid until"), yyvsp[0].str); ;
    break;}
case 84:
#line 1100 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 85:
#line 1113 "preproc.y"
{
					yyval.str = cat4_str(make1_str("set"), yyvsp[-2].str, make1_str("to"), yyvsp[0].str);
				;
    break;}
case 86:
#line 1117 "preproc.y"
{
					yyval.str = cat4_str(make1_str("set"), yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 87:
#line 1121 "preproc.y"
{
					yyval.str = cat2_str(make1_str("set time zone"), yyvsp[0].str);
				;
    break;}
case 88:
#line 1125 "preproc.y"
{
					if (strcasecmp(yyvsp[0].str, "COMMITTED"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[0].str);
						yyerror(errortext);
					}

					yyval.str = cat2_str(make1_str("set transaction isolation level read"), yyvsp[0].str);
				;
    break;}
case 89:
#line 1135 "preproc.y"
{
					if (strcasecmp(yyvsp[0].str, "SERIALIZABLE"))
					{
                                                sprintf(errortext, "syntax error at or near \"%s\"", yyvsp[0].str);
                                                yyerror(errortext);
					}

					yyval.str = cat2_str(make1_str("set transaction isolation level read"), yyvsp[0].str);
				;
    break;}
case 90:
#line 1145 "preproc.y"
{
#ifdef MB
					yyval.str = cat2_str(make1_str("set names"), yyvsp[0].str);
#else
                                        yyerror("SET NAMES is not supported");
#endif
                                ;
    break;}
case 91:
#line 1154 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 92:
#line 1155 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 93:
#line 1158 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 94:
#line 1159 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 95:
#line 1160 "preproc.y"
{ yyval.str = make1_str("local"); ;
    break;}
case 96:
#line 1164 "preproc.y"
{
					yyval.str = cat2_str(make1_str("show"), yyvsp[0].str);
				;
    break;}
case 97:
#line 1168 "preproc.y"
{
					yyval.str = make1_str("show time zone");
				;
    break;}
case 98:
#line 1172 "preproc.y"
{
					yyval.str = make1_str("show transaction isolation level");
				;
    break;}
case 99:
#line 1178 "preproc.y"
{
					yyval.str = cat2_str(make1_str("reset"), yyvsp[0].str);
				;
    break;}
case 100:
#line 1182 "preproc.y"
{
					yyval.str = make1_str("reset time zone");
				;
    break;}
case 101:
#line 1186 "preproc.y"
{
					yyval.str = make1_str("reset transaction isolation level");
				;
    break;}
case 102:
#line 1200 "preproc.y"
{
					yyval.str = cat4_str(make1_str("alter table"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 103:
#line 1206 "preproc.y"
{
					yyval.str = cat3_str(make1_str("add"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 104:
#line 1210 "preproc.y"
{
					yyval.str = make3_str(make1_str("add("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 105:
#line 1214 "preproc.y"
{	yyerror("ALTER TABLE/DROP COLUMN not yet implemented"); ;
    break;}
case 106:
#line 1216 "preproc.y"
{	yyerror("ALTER TABLE/ALTER COLUMN/SET DEFAULT not yet implemented"); ;
    break;}
case 107:
#line 1218 "preproc.y"
{	yyerror("ALTER TABLE/ALTER COLUMN/DROP DEFAULT not yet implemented"); ;
    break;}
case 108:
#line 1220 "preproc.y"
{	yyerror("ALTER TABLE/ADD CONSTRAINT not yet implemented"); ;
    break;}
case 109:
#line 1231 "preproc.y"
{
					yyval.str = cat2_str(make1_str("close"), yyvsp[0].str);
				;
    break;}
case 110:
#line 1246 "preproc.y"
{
					yyval.str = cat3_str(cat5_str(make1_str("copy"), yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 111:
#line 1252 "preproc.y"
{ yyval.str = make1_str("to"); ;
    break;}
case 112:
#line 1254 "preproc.y"
{ yyval.str = make1_str("from"); ;
    break;}
case 113:
#line 1262 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 114:
#line 1263 "preproc.y"
{ yyval.str = make1_str("stdin"); ;
    break;}
case 115:
#line 1264 "preproc.y"
{ yyval.str = make1_str("stdout"); ;
    break;}
case 116:
#line 1267 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 117:
#line 1268 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 118:
#line 1271 "preproc.y"
{ yyval.str = make1_str("with oids"); ;
    break;}
case 119:
#line 1272 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 120:
#line 1278 "preproc.y"
{ yyval.str = cat2_str(make1_str("using delimiters"), yyvsp[0].str); ;
    break;}
case 121:
#line 1279 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 122:
#line 1293 "preproc.y"
{
					yyval.str = cat3_str(cat4_str(make1_str("create"), yyvsp[-6].str, make1_str("table"), yyvsp[-4].str), make3_str(make1_str("("), yyvsp[-2].str, make1_str(")")), yyvsp[0].str);
				;
    break;}
case 123:
#line 1298 "preproc.y"
{ yyval.str = make1_str("temp"); ;
    break;}
case 124:
#line 1299 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 125:
#line 1303 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 126:
#line 1307 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 127:
#line 1310 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 128:
#line 1313 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 129:
#line 1314 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 130:
#line 1318 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 131:
#line 1322 "preproc.y"
{
			yyval.str = make3_str(yyvsp[-2].str, make1_str(" serial "), yyvsp[0].str);
		;
    break;}
case 132:
#line 1327 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 133:
#line 1328 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 134:
#line 1331 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str,yyvsp[0].str); ;
    break;}
case 135:
#line 1332 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 136:
#line 1336 "preproc.y"
{
			yyval.str = make1_str("primary key");
                ;
    break;}
case 137:
#line 1340 "preproc.y"
{
			yyval.str = make1_str("");
		;
    break;}
case 138:
#line 1347 "preproc.y"
{
					yyval.str = cat3_str(make1_str("constraint"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 139:
#line 1351 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 140:
#line 1365 "preproc.y"
{
					yyval.str = make3_str(make1_str("check("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 141:
#line 1369 "preproc.y"
{
					yyval.str = make1_str("default null");
				;
    break;}
case 142:
#line 1373 "preproc.y"
{
					yyval.str = cat2_str(make1_str("default"), yyvsp[0].str);
				;
    break;}
case 143:
#line 1377 "preproc.y"
{
					yyval.str = make1_str("not null");
				;
    break;}
case 144:
#line 1381 "preproc.y"
{
					yyval.str = make1_str("unique");
				;
    break;}
case 145:
#line 1385 "preproc.y"
{
					yyval.str = make1_str("primary key");
				;
    break;}
case 146:
#line 1389 "preproc.y"
{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					yyval.str = make1_str("");
				;
    break;}
case 147:
#line 1396 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 148:
#line 1400 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 149:
#line 1414 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 150:
#line 1416 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 151:
#line 1418 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 152:
#line 1420 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 153:
#line 1422 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 154:
#line 1424 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 155:
#line 1426 "preproc.y"
{	yyerror("boolean expressions not supported in DEFAULT"); ;
    break;}
case 156:
#line 1428 "preproc.y"
{	yyerror("boolean expressions not supported in DEFAULT"); ;
    break;}
case 157:
#line 1430 "preproc.y"
{	yyerror("boolean expressions not supported in DEFAULT"); ;
    break;}
case 158:
#line 1436 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 159:
#line 1438 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 160:
#line 1440 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str); ;
    break;}
case 161:
#line 1442 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str) , make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 162:
#line 1446 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 163:
#line 1448 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); ;
    break;}
case 164:
#line 1450 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); ;
    break;}
case 165:
#line 1452 "preproc.y"
{
					if (!strcmp("<=", yyvsp[-1].str) || !strcmp(">=", yyvsp[-1].str))
						yyerror("boolean expressions not supported in DEFAULT");
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 166:
#line 1458 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 167:
#line 1460 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 168:
#line 1463 "preproc.y"
{	yyval.str = make1_str("current_date"); ;
    break;}
case 169:
#line 1465 "preproc.y"
{	yyval.str = make1_str("current_time"); ;
    break;}
case 170:
#line 1467 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr, "CURRENT_TIME(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = "current_time";
				;
    break;}
case 171:
#line 1473 "preproc.y"
{	yyval.str = make1_str("current_timestamp"); ;
    break;}
case 172:
#line 1475 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr, "CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = "current_timestamp";
				;
    break;}
case 173:
#line 1481 "preproc.y"
{	yyval.str = make1_str("current_user"); ;
    break;}
case 174:
#line 1483 "preproc.y"
{       yyval.str = make1_str("user"); ;
    break;}
case 175:
#line 1491 "preproc.y"
{
						yyval.str = cat3_str(make1_str("constraint"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 176:
#line 1495 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 177:
#line 1499 "preproc.y"
{
					yyval.str = make3_str(make1_str("check("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 178:
#line 1503 "preproc.y"
{
					yyval.str = make3_str(make1_str("unique("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 179:
#line 1507 "preproc.y"
{
					yyval.str = make3_str(make1_str("primary key("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 180:
#line 1511 "preproc.y"
{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					yyval.str = "";
				;
    break;}
case 181:
#line 1518 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 182:
#line 1522 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 183:
#line 1528 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 184:
#line 1530 "preproc.y"
{	yyval.str = make1_str("null"); ;
    break;}
case 185:
#line 1532 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 186:
#line 1536 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 187:
#line 1538 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 188:
#line 1540 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 189:
#line 1542 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 190:
#line 1544 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 191:
#line 1546 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 192:
#line 1548 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 193:
#line 1550 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 194:
#line 1556 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 195:
#line 1558 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 196:
#line 1560 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 197:
#line 1564 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 198:
#line 1568 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 199:
#line 1570 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); }
				;
    break;}
case 200:
#line 1574 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 201:
#line 1578 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 202:
#line 1580 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 203:
#line 1582 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 204:
#line 1584 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 205:
#line 1586 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 206:
#line 1588 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 207:
#line 1590 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 208:
#line 1592 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 209:
#line 1594 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 210:
#line 1596 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 211:
#line 1598 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 212:
#line 1600 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 213:
#line 1602 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); ;
    break;}
case 214:
#line 1604 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); ;
    break;}
case 215:
#line 1606 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); ;
    break;}
case 216:
#line 1608 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); ;
    break;}
case 217:
#line 1610 "preproc.y"
{	yyval.str = cat4_str(yyvsp[-4].str, make1_str("in ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 218:
#line 1612 "preproc.y"
{	yyval.str = cat4_str(yyvsp[-5].str, make1_str("not in ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 219:
#line 1614 "preproc.y"
{	yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 220:
#line 1616 "preproc.y"
{	yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 221:
#line 1619 "preproc.y"
{
		yyval.str = make3_str(yyvsp[-2].str, make1_str(", "), yyvsp[0].str);
	;
    break;}
case 222:
#line 1623 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 223:
#line 1628 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 224:
#line 1632 "preproc.y"
{ yyval.str = make1_str("match full"); ;
    break;}
case 225:
#line 1633 "preproc.y"
{ yyval.str = make1_str("match partial"); ;
    break;}
case 226:
#line 1634 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 227:
#line 1637 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 228:
#line 1638 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 229:
#line 1639 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 230:
#line 1642 "preproc.y"
{ yyval.str = cat2_str(make1_str("on delete"), yyvsp[0].str); ;
    break;}
case 231:
#line 1643 "preproc.y"
{ yyval.str = cat2_str(make1_str("on update"), yyvsp[0].str); ;
    break;}
case 232:
#line 1646 "preproc.y"
{ yyval.str = make1_str("no action"); ;
    break;}
case 233:
#line 1647 "preproc.y"
{ yyval.str = make1_str("cascade"); ;
    break;}
case 234:
#line 1648 "preproc.y"
{ yyval.str = make1_str("set default"); ;
    break;}
case 235:
#line 1649 "preproc.y"
{ yyval.str = make1_str("set null"); ;
    break;}
case 236:
#line 1652 "preproc.y"
{ yyval.str = make3_str(make1_str("inherits ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 237:
#line 1653 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 238:
#line 1657 "preproc.y"
{
			yyval.str = cat5_str(cat3_str(make1_str("create"), yyvsp[-5].str, make1_str("table")), yyvsp[-3].str, yyvsp[-2].str, make1_str("as"), yyvsp[0].str); 
		;
    break;}
case 239:
#line 1662 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 240:
#line 1663 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 241:
#line 1666 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 242:
#line 1667 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 243:
#line 1670 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 244:
#line 1681 "preproc.y"
{
					yyval.str = cat3_str(make1_str("create sequence"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 245:
#line 1687 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 246:
#line 1688 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 247:
#line 1692 "preproc.y"
{
					yyval.str = cat2_str(make1_str("cache"), yyvsp[0].str);
				;
    break;}
case 248:
#line 1696 "preproc.y"
{
					yyval.str = make1_str("cycle");
				;
    break;}
case 249:
#line 1700 "preproc.y"
{
					yyval.str = cat2_str(make1_str("increment"), yyvsp[0].str);
				;
    break;}
case 250:
#line 1704 "preproc.y"
{
					yyval.str = cat2_str(make1_str("maxvalue"), yyvsp[0].str);
				;
    break;}
case 251:
#line 1708 "preproc.y"
{
					yyval.str = cat2_str(make1_str("minvalue"), yyvsp[0].str);
				;
    break;}
case 252:
#line 1712 "preproc.y"
{
					yyval.str = cat2_str(make1_str("start"), yyvsp[0].str);
				;
    break;}
case 253:
#line 1717 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 254:
#line 1718 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 255:
#line 1721 "preproc.y"
{
                                       yyval.str = yyvsp[0].str;
                               ;
    break;}
case 256:
#line 1725 "preproc.y"
{
                                       yyval.str = cat2_str(make1_str("-"), yyvsp[0].str);
                               ;
    break;}
case 257:
#line 1732 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 258:
#line 1736 "preproc.y"
{
					yyval.str = cat2_str(make1_str("-"), yyvsp[0].str);
				;
    break;}
case 259:
#line 1751 "preproc.y"
{
				yyval.str = cat4_str(cat5_str(make1_str("create"), yyvsp[-7].str, make1_str("precedural language"), yyvsp[-4].str, make1_str("handler")), yyvsp[-2].str, make1_str("langcompiler"), yyvsp[0].str);
			;
    break;}
case 260:
#line 1756 "preproc.y"
{ yyval.str = make1_str("trusted"); ;
    break;}
case 261:
#line 1757 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 262:
#line 1760 "preproc.y"
{
				yyval.str = cat2_str(make1_str("drop procedural language"), yyvsp[0].str);
			;
    break;}
case 263:
#line 1776 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create trigger"), yyvsp[-11].str, yyvsp[-10].str, yyvsp[-9].str, make1_str("on")), yyvsp[-7].str, yyvsp[-6].str, make1_str("execute procedure"), yyvsp[-3].str), make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 264:
#line 1781 "preproc.y"
{ yyval.str = make1_str("before"); ;
    break;}
case 265:
#line 1782 "preproc.y"
{ yyval.str = make1_str("after"); ;
    break;}
case 266:
#line 1786 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 267:
#line 1790 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str);
				;
    break;}
case 268:
#line 1794 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("or"), yyvsp[-2].str, make1_str("or"), yyvsp[0].str);
				;
    break;}
case 269:
#line 1799 "preproc.y"
{ yyval.str = make1_str("insert"); ;
    break;}
case 270:
#line 1800 "preproc.y"
{ yyval.str = make1_str("delete"); ;
    break;}
case 271:
#line 1801 "preproc.y"
{ yyval.str = make1_str("update"); ;
    break;}
case 272:
#line 1805 "preproc.y"
{
					yyval.str = cat3_str(make1_str("for"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 273:
#line 1810 "preproc.y"
{ yyval.str = make1_str("each"); ;
    break;}
case 274:
#line 1811 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 275:
#line 1814 "preproc.y"
{ yyval.str = make1_str("row"); ;
    break;}
case 276:
#line 1815 "preproc.y"
{ yyval.str = make1_str("statement"); ;
    break;}
case 277:
#line 1819 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 278:
#line 1821 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 279:
#line 1823 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 280:
#line 1827 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 281:
#line 1831 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 282:
#line 1834 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 283:
#line 1835 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 284:
#line 1839 "preproc.y"
{
					yyval.str = cat4_str(make1_str("drop trigger"), yyvsp[-2].str, make1_str("on"), yyvsp[0].str);
				;
    break;}
case 285:
#line 1852 "preproc.y"
{
					yyval.str = cat3_str(make1_str("create"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 286:
#line 1858 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 287:
#line 1863 "preproc.y"
{ yyval.str = make1_str("operator"); ;
    break;}
case 288:
#line 1864 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 289:
#line 1865 "preproc.y"
{ yyval.str = make1_str("aggregate"); ;
    break;}
case 290:
#line 1868 "preproc.y"
{ yyval.str = make1_str("procedure"); ;
    break;}
case 291:
#line 1869 "preproc.y"
{ yyval.str = make1_str("join"); ;
    break;}
case 292:
#line 1870 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 293:
#line 1871 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 294:
#line 1872 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 295:
#line 1875 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 296:
#line 1878 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 297:
#line 1879 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 298:
#line 1882 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 299:
#line 1886 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 300:
#line 1890 "preproc.y"
{
					yyval.str = cat2_str(make1_str("default ="), yyvsp[0].str);
				;
    break;}
case 301:
#line 1895 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 302:
#line 1896 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 303:
#line 1897 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 304:
#line 1898 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 305:
#line 1900 "preproc.y"
{
					yyval.str = cat2_str(make1_str("setof"), yyvsp[0].str);
				;
    break;}
case 306:
#line 1913 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop table"), yyvsp[0].str);
				;
    break;}
case 307:
#line 1917 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop sequence"), yyvsp[0].str);
				;
    break;}
case 308:
#line 1934 "preproc.y"
{
					if (strncmp(yyvsp[-4].str, "relative", strlen("relative")) == 0 && atol(yyvsp[-3].str) == 0L)
						yyerror("FETCH/RELATIVE at current position is not supported");

					yyval.str = cat4_str(make1_str("fetch"), yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str);
				;
    break;}
case 309:
#line 1941 "preproc.y"
{
					yyval.str = cat4_str(make1_str("fetch"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 310:
#line 1946 "preproc.y"
{ yyval.str = make1_str("forward"); ;
    break;}
case 311:
#line 1947 "preproc.y"
{ yyval.str = make1_str("backward"); ;
    break;}
case 312:
#line 1948 "preproc.y"
{ yyval.str = make1_str("relative"); ;
    break;}
case 313:
#line 1950 "preproc.y"
{
					fprintf(stderr, "FETCH/ABSOLUTE not supported, using RELATIVE");
					yyval.str = make1_str("absolute");
				;
    break;}
case 314:
#line 1954 "preproc.y"
{ yyval.str = make1_str(""); /* default */ ;
    break;}
case 315:
#line 1957 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 316:
#line 1958 "preproc.y"
{ yyval.str = make2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 317:
#line 1959 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 318:
#line 1960 "preproc.y"
{ yyval.str = make1_str("next"); ;
    break;}
case 319:
#line 1961 "preproc.y"
{ yyval.str = make1_str("prior"); ;
    break;}
case 320:
#line 1962 "preproc.y"
{ yyval.str = make1_str(""); /*default*/ ;
    break;}
case 321:
#line 1965 "preproc.y"
{ yyval.str = cat2_str(make1_str("in"), yyvsp[0].str); ;
    break;}
case 322:
#line 1966 "preproc.y"
{ yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 323:
#line 1968 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 324:
#line 1980 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("grant"), yyvsp[-5].str, make1_str("on"), yyvsp[-3].str, make1_str("to")), yyvsp[-1].str);
				;
    break;}
case 325:
#line 1986 "preproc.y"
{
				 yyval.str = make1_str("all privileges");
				;
    break;}
case 326:
#line 1990 "preproc.y"
{
				 yyval.str = make1_str("all");
				;
    break;}
case 327:
#line 1994 "preproc.y"
{
				 yyval.str = yyvsp[0].str;
				;
    break;}
case 328:
#line 2000 "preproc.y"
{
						yyval.str = yyvsp[0].str;
				;
    break;}
case 329:
#line 2004 "preproc.y"
{
						yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 330:
#line 2010 "preproc.y"
{
						yyval.str = make1_str("select");
				;
    break;}
case 331:
#line 2014 "preproc.y"
{
						yyval.str = make1_str("insert");
				;
    break;}
case 332:
#line 2018 "preproc.y"
{
						yyval.str = make1_str("update");
				;
    break;}
case 333:
#line 2022 "preproc.y"
{
						yyval.str = make1_str("delete");
				;
    break;}
case 334:
#line 2026 "preproc.y"
{
						yyval.str = make1_str("rule");
				;
    break;}
case 335:
#line 2032 "preproc.y"
{
						yyval.str = make1_str("public");
				;
    break;}
case 336:
#line 2036 "preproc.y"
{
						yyval.str = cat2_str(make1_str("group"), yyvsp[0].str);
				;
    break;}
case 337:
#line 2040 "preproc.y"
{
						yyval.str = yyvsp[0].str;
				;
    break;}
case 338:
#line 2046 "preproc.y"
{
					yyerror("WITH GRANT OPTION is not supported.  Only relation owners can set privileges");
				 ;
    break;}
case 340:
#line 2061 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("revoke"), yyvsp[-4].str, make1_str("on"), yyvsp[-2].str, make1_str("from")), yyvsp[0].str);
				;
    break;}
case 341:
#line 2080 "preproc.y"
{
					/* should check that access_method is valid,
					   etc ... but doesn't */
					yyval.str = cat5_str(cat5_str(make1_str("create"), yyvsp[-9].str, make1_str("index"), yyvsp[-7].str, make1_str("on")), yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("("), yyvsp[-2].str, make1_str(")")), yyvsp[0].str);
				;
    break;}
case 342:
#line 2087 "preproc.y"
{ yyval.str = make1_str("unique"); ;
    break;}
case 343:
#line 2088 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 344:
#line 2091 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 345:
#line 2092 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 346:
#line 2095 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 347:
#line 2096 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 348:
#line 2099 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 349:
#line 2100 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 350:
#line 2104 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-5].str, make3_str(make1_str("("), yyvsp[-3].str, ")"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 351:
#line 2110 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 352:
#line 2115 "preproc.y"
{ yyval.str = cat2_str(make1_str(":"), yyvsp[0].str); ;
    break;}
case 353:
#line 2116 "preproc.y"
{ yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 354:
#line 2117 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 355:
#line 2126 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 356:
#line 2127 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 357:
#line 2128 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 358:
#line 2139 "preproc.y"
{
					yyval.str = cat3_str(make1_str("extend index"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 359:
#line 2176 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create function"), yyvsp[-8].str, yyvsp[-7].str, make1_str("returns"), yyvsp[-5].str), yyvsp[-4].str, make1_str("as"), yyvsp[-2].str, make1_str("language")), yyvsp[0].str);
				;
    break;}
case 360:
#line 2180 "preproc.y"
{ yyval.str = cat2_str(make1_str("with"), yyvsp[0].str); ;
    break;}
case 361:
#line 2181 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 362:
#line 2184 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 363:
#line 2185 "preproc.y"
{ yyval.str = make1_str("()"); ;
    break;}
case 364:
#line 2188 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 365:
#line 2190 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 366:
#line 2194 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 367:
#line 2199 "preproc.y"
{ yyval.str = make1_str("setof"); ;
    break;}
case 368:
#line 2200 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 369:
#line 2222 "preproc.y"
{
					yyval.str = cat3_str(make1_str("drop"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 370:
#line 2227 "preproc.y"
{  yyval.str = make1_str("type"); ;
    break;}
case 371:
#line 2228 "preproc.y"
{  yyval.str = make1_str("index"); ;
    break;}
case 372:
#line 2229 "preproc.y"
{  yyval.str = make1_str("rule"); ;
    break;}
case 373:
#line 2230 "preproc.y"
{  yyval.str = make1_str("view"); ;
    break;}
case 374:
#line 2235 "preproc.y"
{
						yyval.str = cat3_str(make1_str("drop aggregate"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 375:
#line 2240 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 376:
#line 2241 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 377:
#line 2246 "preproc.y"
{
						yyval.str = cat3_str(make1_str("drop function"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 378:
#line 2253 "preproc.y"
{
					yyval.str = cat3_str(make1_str("drop operator"), yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 381:
#line 2260 "preproc.y"
{ yyval.str = make1_str("+"); ;
    break;}
case 382:
#line 2261 "preproc.y"
{ yyval.str = make1_str("-"); ;
    break;}
case 383:
#line 2262 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 384:
#line 2263 "preproc.y"
{ yyval.str = make1_str("/"); ;
    break;}
case 385:
#line 2264 "preproc.y"
{ yyval.str = make1_str("<"); ;
    break;}
case 386:
#line 2265 "preproc.y"
{ yyval.str = make1_str(">"); ;
    break;}
case 387:
#line 2266 "preproc.y"
{ yyval.str = make1_str("="); ;
    break;}
case 388:
#line 2270 "preproc.y"
{
				   yyerror("parser: argument type missing (use NONE for unary operators)");
				;
    break;}
case 389:
#line 2274 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 390:
#line 2276 "preproc.y"
{ yyval.str = cat2_str(make1_str("none,"), yyvsp[0].str); ;
    break;}
case 391:
#line 2278 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-2].str, make1_str(", none")); ;
    break;}
case 392:
#line 2292 "preproc.y"
{
					yyval.str = cat4_str(cat5_str(make1_str("alter table"), yyvsp[-6].str, yyvsp[-5].str, make1_str("rename"), yyvsp[-3].str), yyvsp[-2].str, make1_str("to"), yyvsp[0].str);
				;
    break;}
case 393:
#line 2297 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 394:
#line 2298 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 395:
#line 2301 "preproc.y"
{ yyval.str = make1_str("colmunn"); ;
    break;}
case 396:
#line 2302 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 397:
#line 2316 "preproc.y"
{ QueryIsRule=1; ;
    break;}
case 398:
#line 2319 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create rule"), yyvsp[-10].str, make1_str("as on"), yyvsp[-6].str, make1_str("to")), yyvsp[-4].str, yyvsp[-3].str, make1_str("do"), yyvsp[-1].str), yyvsp[0].str);
				;
    break;}
case 399:
#line 2324 "preproc.y"
{ yyval.str = make1_str("nothing"); ;
    break;}
case 400:
#line 2325 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 401:
#line 2326 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 402:
#line 2327 "preproc.y"
{ yyval.str = cat3_str(make1_str("["), yyvsp[-1].str, make1_str("]")); ;
    break;}
case 403:
#line 2328 "preproc.y"
{ yyval.str = cat3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 404:
#line 2331 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 405:
#line 2332 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 406:
#line 2336 "preproc.y"
{  yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 407:
#line 2338 "preproc.y"
{  yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, make1_str(";")); ;
    break;}
case 408:
#line 2340 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, make1_str(";")); ;
    break;}
case 413:
#line 2350 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 414:
#line 2354 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 415:
#line 2360 "preproc.y"
{ yyval.str = make1_str("select"); ;
    break;}
case 416:
#line 2361 "preproc.y"
{ yyval.str = make1_str("update"); ;
    break;}
case 417:
#line 2362 "preproc.y"
{ yyval.str = make1_str("delete"); ;
    break;}
case 418:
#line 2363 "preproc.y"
{ yyval.str = make1_str("insert"); ;
    break;}
case 419:
#line 2366 "preproc.y"
{ yyval.str = make1_str("instead"); ;
    break;}
case 420:
#line 2367 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 421:
#line 2380 "preproc.y"
{
					yyval.str = cat2_str(make1_str("notify"), yyvsp[0].str);
				;
    break;}
case 422:
#line 2386 "preproc.y"
{
					yyval.str = cat2_str(make1_str("listen"), yyvsp[0].str);
                                ;
    break;}
case 423:
#line 2392 "preproc.y"
{
					yyval.str = cat2_str(make1_str("unlisten"), yyvsp[0].str);
                                ;
    break;}
case 424:
#line 2396 "preproc.y"
{
					yyval.str = make1_str("unlisten *");
                                ;
    break;}
case 425:
#line 2413 "preproc.y"
{ yyval.str = make1_str("rollback"); ;
    break;}
case 426:
#line 2414 "preproc.y"
{ yyval.str = make1_str("begin transaction"); ;
    break;}
case 427:
#line 2415 "preproc.y"
{ yyval.str = make1_str("commit"); ;
    break;}
case 428:
#line 2416 "preproc.y"
{ yyval.str = make1_str("commit"); ;
    break;}
case 429:
#line 2417 "preproc.y"
{ yyval.str = make1_str("rollback"); ;
    break;}
case 430:
#line 2419 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 431:
#line 2420 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 432:
#line 2421 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 433:
#line 2432 "preproc.y"
{
					yyval.str = cat4_str(make1_str("create view"), yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
				;
    break;}
case 434:
#line 2446 "preproc.y"
{
					yyval.str = cat2_str(make1_str("load"), yyvsp[0].str);
				;
    break;}
case 435:
#line 2460 "preproc.y"
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
case 436:
#line 2470 "preproc.y"
{
					yyval.str = cat2_str(make1_str("create database"), yyvsp[0].str);
				;
    break;}
case 437:
#line 2475 "preproc.y"
{ yyval.str = cat2_str(make1_str("location ="), yyvsp[0].str); ;
    break;}
case 438:
#line 2476 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 439:
#line 2479 "preproc.y"
{ yyval.str = cat2_str(make1_str("encoding ="), yyvsp[0].str); ;
    break;}
case 440:
#line 2480 "preproc.y"
{ yyval.str = NULL; ;
    break;}
case 441:
#line 2483 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 442:
#line 2484 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 443:
#line 2485 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 444:
#line 2488 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 445:
#line 2489 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 446:
#line 2490 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 447:
#line 2501 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop database"), yyvsp[0].str);
				;
    break;}
case 448:
#line 2515 "preproc.y"
{
				   yyval.str = cat4_str(make1_str("cluster"), yyvsp[-2].str, make1_str("on"), yyvsp[0].str);
				;
    break;}
case 449:
#line 2529 "preproc.y"
{
					yyval.str = cat3_str(make1_str("vacuum"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 450:
#line 2533 "preproc.y"
{
					if ( strlen(yyvsp[0].str) > 0 && strlen(yyvsp[-1].str) == 0 )
						yyerror("parser: syntax error at or near \"(\"");
					yyval.str = cat5_str(make1_str("vacuum"), yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 451:
#line 2540 "preproc.y"
{ yyval.str = make1_str("verbose"); ;
    break;}
case 452:
#line 2541 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 453:
#line 2544 "preproc.y"
{ yyval.str = make1_str("analyse"); ;
    break;}
case 454:
#line 2545 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 455:
#line 2548 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 456:
#line 2549 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 457:
#line 2553 "preproc.y"
{ yyval.str=yyvsp[0].str; ;
    break;}
case 458:
#line 2555 "preproc.y"
{ yyval.str=cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 459:
#line 2567 "preproc.y"
{
					yyval.str = cat3_str(make1_str("explain"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 466:
#line 2607 "preproc.y"
{
					yyval.str = cat3_str(make1_str("insert into"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 467:
#line 2613 "preproc.y"
{
					yyval.str = make3_str(make1_str("values("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 468:
#line 2617 "preproc.y"
{
					yyval.str = make1_str("default values");
				;
    break;}
case 469:
#line 2621 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 470:
#line 2625 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-5].str, make1_str(") values ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 471:
#line 2629 "preproc.y"
{
					yyval.str = make4_str(make1_str("("), yyvsp[-2].str, make1_str(")"), yyvsp[0].str);
				;
    break;}
case 472:
#line 2634 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 473:
#line 2635 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 474:
#line 2640 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 475:
#line 2642 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 476:
#line 2646 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 477:
#line 2661 "preproc.y"
{
					yyval.str = cat3_str(make1_str("delete from"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 478:
#line 2667 "preproc.y"
{
					yyval.str = cat3_str(make1_str("lock"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 479:
#line 2671 "preproc.y"
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
case 480:
#line 2702 "preproc.y"
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
case 481:
#line 2722 "preproc.y"
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
case 482:
#line 2738 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 483:
#line 2739 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 484:
#line 2756 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("update"), yyvsp[-4].str, make1_str("set"), yyvsp[-2].str, yyvsp[-1].str), yyvsp[0].str);
				;
    break;}
case 485:
#line 2769 "preproc.y"
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
case 486:
#line 2799 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 487:
#line 2800 "preproc.y"
{ yyval.str = make1_str("insensitive"); ;
    break;}
case 488:
#line 2801 "preproc.y"
{ yyval.str = make1_str("scroll"); ;
    break;}
case 489:
#line 2802 "preproc.y"
{ yyval.str = make1_str("insensitive scroll"); ;
    break;}
case 490:
#line 2803 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 491:
#line 2806 "preproc.y"
{ yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 492:
#line 2807 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 493:
#line 2811 "preproc.y"
{ yyval.str = make1_str("read only"); ;
    break;}
case 494:
#line 2813 "preproc.y"
{
                               yyerror("DECLARE/UPDATE not supported; Cursors must be READ ONLY.");
                       ;
    break;}
case 495:
#line 2818 "preproc.y"
{ yyval.str = make2_str(make1_str("of"), yyvsp[0].str); ;
    break;}
case 496:
#line 2835 "preproc.y"
{
					if (strlen(yyvsp[-1].str) > 0 && ForUpdateNotAllowed != 0)
							yyerror("SELECT FOR UPDATE is not allowed in this context");

					ForUpdateNotAllowed = 0;
					yyval.str = cat4_str(yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 497:
#line 2852 "preproc.y"
{
                               yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); 
                        ;
    break;}
case 498:
#line 2856 "preproc.y"
{
                               yyval.str = yyvsp[0].str; 
                        ;
    break;}
case 499:
#line 2860 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-2].str, make1_str("except"), yyvsp[0].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 500:
#line 2865 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-3].str, make1_str("union"), yyvsp[-1].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 501:
#line 2870 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-3].str, make1_str("intersect"), yyvsp[-1].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 502:
#line 2880 "preproc.y"
{
					yyval.str = cat4_str(cat5_str(make1_str("select"), yyvsp[-6].str, yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
					if (strlen(yyvsp[-1].str) > 0 || strlen(yyvsp[0].str) > 0)
						ForUpdateNotAllowed = 1;
				;
    break;}
case 503:
#line 2887 "preproc.y"
{ yyval.str= cat4_str(make1_str("into"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 504:
#line 2888 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 505:
#line 2889 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 506:
#line 2892 "preproc.y"
{ yyval.str = make1_str("table"); ;
    break;}
case 507:
#line 2893 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 508:
#line 2896 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 509:
#line 2897 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 510:
#line 2900 "preproc.y"
{ yyval.str = make1_str("distinct"); ;
    break;}
case 511:
#line 2901 "preproc.y"
{ yyval.str = cat2_str(make1_str("distinct on"), yyvsp[0].str); ;
    break;}
case 512:
#line 2902 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 513:
#line 2903 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 514:
#line 2906 "preproc.y"
{ yyval.str = cat2_str(make1_str("order by"), yyvsp[0].str); ;
    break;}
case 515:
#line 2907 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 516:
#line 2910 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 517:
#line 2911 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 518:
#line 2915 "preproc.y"
{
					 yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 519:
#line 2920 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 520:
#line 2921 "preproc.y"
{ yyval.str = make1_str("using <"); ;
    break;}
case 521:
#line 2922 "preproc.y"
{ yyval.str = make1_str("using >"); ;
    break;}
case 522:
#line 2923 "preproc.y"
{ yyval.str = make1_str("asc"); ;
    break;}
case 523:
#line 2924 "preproc.y"
{ yyval.str = make1_str("desc"); ;
    break;}
case 524:
#line 2925 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 525:
#line 2929 "preproc.y"
{ yyval.str = cat4_str(make1_str("limit"), yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 526:
#line 2931 "preproc.y"
{ yyval.str = cat4_str(make1_str("limit"), yyvsp[-2].str, make1_str("offset"), yyvsp[0].str); ;
    break;}
case 527:
#line 2933 "preproc.y"
{ yyval.str = cat2_str(make1_str("limit"), yyvsp[0].str); ;
    break;}
case 528:
#line 2935 "preproc.y"
{ yyval.str = cat4_str(make1_str("offset"), yyvsp[-2].str, make1_str("limit"), yyvsp[0].str); ;
    break;}
case 529:
#line 2937 "preproc.y"
{ yyval.str = cat2_str(make1_str("offset"), yyvsp[0].str); ;
    break;}
case 530:
#line 2939 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 531:
#line 2942 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 532:
#line 2943 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 533:
#line 2944 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 534:
#line 2947 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 535:
#line 2948 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 536:
#line 2958 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 537:
#line 2959 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 538:
#line 2962 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 539:
#line 2965 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 540:
#line 2967 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 541:
#line 2970 "preproc.y"
{ yyval.str = cat2_str(make1_str("groub by"), yyvsp[0].str); ;
    break;}
case 542:
#line 2971 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 543:
#line 2975 "preproc.y"
{
					yyval.str = cat2_str(make1_str("having"), yyvsp[0].str);
				;
    break;}
case 544:
#line 2978 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 545:
#line 2982 "preproc.y"
{
                	yyval.str = make1_str("for update"); 
		;
    break;}
case 546:
#line 2986 "preproc.y"
{
                        yyval.str = make1_str("");
                ;
    break;}
case 547:
#line 2991 "preproc.y"
{
			yyval.str = cat2_str(make1_str("of"), yyvsp[0].str);
	      ;
    break;}
case 548:
#line 2995 "preproc.y"
{
                        yyval.str = make1_str("");
              ;
    break;}
case 549:
#line 3009 "preproc.y"
{
			yyval.str = cat2_str(make1_str("from"), yyvsp[0].str);
		;
    break;}
case 550:
#line 3013 "preproc.y"
{
			yyval.str = make1_str("");
		;
    break;}
case 551:
#line 3019 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 552:
#line 3021 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 553:
#line 3023 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 554:
#line 3027 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 555:
#line 3029 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 556:
#line 3033 "preproc.y"
{
                                        yyval.str = cat3_str(yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
                                ;
    break;}
case 557:
#line 3037 "preproc.y"
{
                                        yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 558:
#line 3041 "preproc.y"
{
                                        yyval.str = yyvsp[0].str;
                                ;
    break;}
case 559:
#line 3051 "preproc.y"
{       yyval.str = yyvsp[0].str; ;
    break;}
case 560:
#line 3053 "preproc.y"
{       yyerror("UNION JOIN not yet implemented"); ;
    break;}
case 561:
#line 3057 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 562:
#line 3063 "preproc.y"
{
                                        yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 563:
#line 3067 "preproc.y"
{
                                        yyval.str = yyvsp[0].str;
                                ;
    break;}
case 564:
#line 3080 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-3].str, make1_str("join"), yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 565:
#line 3084 "preproc.y"
{
					yyval.str = cat4_str(make1_str("natural"), yyvsp[-2].str, make1_str("join"), yyvsp[0].str);
                                ;
    break;}
case 566:
#line 3088 "preproc.y"
{ 	yyval.str = cat2_str(make1_str("cross join"), yyvsp[0].str); ;
    break;}
case 567:
#line 3093 "preproc.y"
{
                                        yyval.str = cat2_str(make1_str("full"), yyvsp[0].str);
                                        fprintf(stderr,"FULL OUTER JOIN not yet implemented\n");
                                ;
    break;}
case 568:
#line 3098 "preproc.y"
{
                                        yyval.str = cat2_str(make1_str("left"), yyvsp[0].str);
                                        fprintf(stderr,"LEFT OUTER JOIN not yet implemented\n");
                                ;
    break;}
case 569:
#line 3103 "preproc.y"
{
                                        yyval.str = cat2_str(make1_str("right"), yyvsp[0].str);
                                        fprintf(stderr,"RIGHT OUTER JOIN not yet implemented\n");
                                ;
    break;}
case 570:
#line 3108 "preproc.y"
{
                                        yyval.str = make1_str("outer");
                                        fprintf(stderr,"OUTER JOIN not yet implemented\n");
                                ;
    break;}
case 571:
#line 3113 "preproc.y"
{
                                        yyval.str = make1_str("inner");
				;
    break;}
case 572:
#line 3117 "preproc.y"
{
                                        yyval.str = make1_str("");
				;
    break;}
case 573:
#line 3122 "preproc.y"
{ yyval.str = make1_str("outer"); ;
    break;}
case 574:
#line 3123 "preproc.y"
{ yyval.str = make1_str("");  /* no qualifiers */ ;
    break;}
case 575:
#line 3134 "preproc.y"
{ yyval.str = make3_str(make1_str("using ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 576:
#line 3135 "preproc.y"
{ yyval.str = cat2_str(make1_str("on"), yyvsp[0].str); ;
    break;}
case 577:
#line 3138 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 578:
#line 3139 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 579:
#line 3143 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 580:
#line 3148 "preproc.y"
{ yyval.str = cat2_str(make1_str("where"), yyvsp[0].str); ;
    break;}
case 581:
#line 3149 "preproc.y"
{ yyval.str = make1_str("");  /* no qualifiers */ ;
    break;}
case 582:
#line 3153 "preproc.y"
{
					/* normal relations */
					yyval.str = yyvsp[0].str;
				;
    break;}
case 583:
#line 3158 "preproc.y"
{
					/* inheritance query */
					yyval.str = cat2_str(yyvsp[-1].str, make1_str("*"));
				;
    break;}
case 584:
#line 3164 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 585:
#line 3170 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 586:
#line 3176 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 587:
#line 3184 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 588:
#line 3190 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 589:
#line 3196 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 590:
#line 3214 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].index.str);
				;
    break;}
case 591:
#line 3217 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 592:
#line 3219 "preproc.y"
{
					yyval.str = cat2_str(make1_str("setof"), yyvsp[0].str);
				;
    break;}
case 594:
#line 3225 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 595:
#line 3226 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 596:
#line 3230 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 597:
#line 3235 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 598:
#line 3236 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 599:
#line 3237 "preproc.y"
{ yyval.str = make1_str("at"); ;
    break;}
case 600:
#line 3238 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 601:
#line 3239 "preproc.y"
{ yyval.str = make1_str("break"); ;
    break;}
case 602:
#line 3240 "preproc.y"
{ yyval.str = make1_str("call"); ;
    break;}
case 603:
#line 3241 "preproc.y"
{ yyval.str = make1_str("connect"); ;
    break;}
case 604:
#line 3242 "preproc.y"
{ yyval.str = make1_str("connection"); ;
    break;}
case 605:
#line 3243 "preproc.y"
{ yyval.str = make1_str("continue"); ;
    break;}
case 606:
#line 3244 "preproc.y"
{ yyval.str = make1_str("deallocate"); ;
    break;}
case 607:
#line 3245 "preproc.y"
{ yyval.str = make1_str("disconnect"); ;
    break;}
case 608:
#line 3246 "preproc.y"
{ yyval.str = make1_str("found"); ;
    break;}
case 609:
#line 3247 "preproc.y"
{ yyval.str = make1_str("go"); ;
    break;}
case 610:
#line 3248 "preproc.y"
{ yyval.str = make1_str("goto"); ;
    break;}
case 611:
#line 3249 "preproc.y"
{ yyval.str = make1_str("identified"); ;
    break;}
case 612:
#line 3250 "preproc.y"
{ yyval.str = make1_str("immediate"); ;
    break;}
case 613:
#line 3251 "preproc.y"
{ yyval.str = make1_str("indicator"); ;
    break;}
case 614:
#line 3252 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 615:
#line 3253 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 616:
#line 3254 "preproc.y"
{ yyval.str = make1_str("open"); ;
    break;}
case 617:
#line 3255 "preproc.y"
{ yyval.str = make1_str("prepare"); ;
    break;}
case 618:
#line 3256 "preproc.y"
{ yyval.str = make1_str("release"); ;
    break;}
case 619:
#line 3257 "preproc.y"
{ yyval.str = make1_str("section"); ;
    break;}
case 620:
#line 3258 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 621:
#line 3259 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 622:
#line 3260 "preproc.y"
{ yyval.str = make1_str("sqlerror"); ;
    break;}
case 623:
#line 3261 "preproc.y"
{ yyval.str = make1_str("sqlprint"); ;
    break;}
case 624:
#line 3262 "preproc.y"
{ yyval.str = make1_str("sqlwarning"); ;
    break;}
case 625:
#line 3263 "preproc.y"
{ yyval.str = make1_str("stop"); ;
    break;}
case 626:
#line 3264 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 627:
#line 3265 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 628:
#line 3266 "preproc.y"
{ yyval.str = make1_str("var"); ;
    break;}
case 629:
#line 3267 "preproc.y"
{ yyval.str = make1_str("whenever"); ;
    break;}
case 630:
#line 3276 "preproc.y"
{
					yyval.str = cat2_str(make1_str("float"), yyvsp[0].str);
				;
    break;}
case 631:
#line 3280 "preproc.y"
{
					yyval.str = make1_str("double precision");
				;
    break;}
case 632:
#line 3284 "preproc.y"
{
					yyval.str = cat2_str(make1_str("decimal"), yyvsp[0].str);
				;
    break;}
case 633:
#line 3288 "preproc.y"
{
					yyval.str = cat2_str(make1_str("numeric"), yyvsp[0].str);
				;
    break;}
case 634:
#line 3294 "preproc.y"
{	yyval.str = make1_str("float"); ;
    break;}
case 635:
#line 3296 "preproc.y"
{	yyval.str = make1_str("double precision"); ;
    break;}
case 636:
#line 3298 "preproc.y"
{	yyval.str = make1_str("decimal"); ;
    break;}
case 637:
#line 3300 "preproc.y"
{	yyval.str = make1_str("numeric"); ;
    break;}
case 638:
#line 3304 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1)
						yyerror("precision for FLOAT must be at least 1");
					else if (atol(yyvsp[-1].str) >= 16)
						yyerror("precision for FLOAT must be less than 16");
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 639:
#line 3312 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 640:
#line 3318 "preproc.y"
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
#line 3330 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1 || atol(yyvsp[-1].str) > NUMERIC_MAX_PRECISION) {
						sprintf(errortext, "NUMERIC precision %s must be between 1 and %d", yyvsp[-1].str, NUMERIC_MAX_PRECISION);
						yyerror(errortext);
					}
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 642:
#line 3338 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 643:
#line 3344 "preproc.y"
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
case 644:
#line 3356 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1 || atol(yyvsp[-1].str) > NUMERIC_MAX_PRECISION) {
						sprintf(errortext, "NUMERIC precision %s must be between 1 and %d", yyvsp[-1].str, NUMERIC_MAX_PRECISION);
						yyerror(errortext);
					}
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 645:
#line 3364 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 646:
#line 3377 "preproc.y"
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
case 647:
#line 3397 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 648:
#line 3403 "preproc.y"
{
					if (strlen(yyvsp[0].str) > 0) 
						fprintf(stderr, "COLLATE %s not yet implemented",yyvsp[0].str);

					yyval.str = cat4_str(make1_str("character"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 649:
#line 3409 "preproc.y"
{ yyval.str = cat2_str(make1_str("char"), yyvsp[0].str); ;
    break;}
case 650:
#line 3410 "preproc.y"
{ yyval.str = make1_str("varchar"); ;
    break;}
case 651:
#line 3411 "preproc.y"
{ yyval.str = cat2_str(make1_str("national character"), yyvsp[0].str); ;
    break;}
case 652:
#line 3412 "preproc.y"
{ yyval.str = cat2_str(make1_str("nchar"), yyvsp[0].str); ;
    break;}
case 653:
#line 3415 "preproc.y"
{ yyval.str = make1_str("varying"); ;
    break;}
case 654:
#line 3416 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 655:
#line 3419 "preproc.y"
{ yyval.str = cat2_str(make1_str("character set"), yyvsp[0].str); ;
    break;}
case 656:
#line 3420 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 657:
#line 3423 "preproc.y"
{ yyval.str = cat2_str(make1_str("collate"), yyvsp[0].str); ;
    break;}
case 658:
#line 3424 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 659:
#line 3428 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 660:
#line 3432 "preproc.y"
{
					yyval.str = cat2_str(make1_str("timestamp"), yyvsp[0].str);
				;
    break;}
case 661:
#line 3436 "preproc.y"
{
					yyval.str = make1_str("time");
				;
    break;}
case 662:
#line 3440 "preproc.y"
{
					yyval.str = cat2_str(make1_str("interval"), yyvsp[0].str);
				;
    break;}
case 663:
#line 3445 "preproc.y"
{ yyval.str = make1_str("year"); ;
    break;}
case 664:
#line 3446 "preproc.y"
{ yyval.str = make1_str("month"); ;
    break;}
case 665:
#line 3447 "preproc.y"
{ yyval.str = make1_str("day"); ;
    break;}
case 666:
#line 3448 "preproc.y"
{ yyval.str = make1_str("hour"); ;
    break;}
case 667:
#line 3449 "preproc.y"
{ yyval.str = make1_str("minute"); ;
    break;}
case 668:
#line 3450 "preproc.y"
{ yyval.str = make1_str("second"); ;
    break;}
case 669:
#line 3453 "preproc.y"
{ yyval.str = make1_str("with time zone"); ;
    break;}
case 670:
#line 3454 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 671:
#line 3457 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 672:
#line 3458 "preproc.y"
{ yyval.str = make1_str("year to #month"); ;
    break;}
case 673:
#line 3459 "preproc.y"
{ yyval.str = make1_str("day to hour"); ;
    break;}
case 674:
#line 3460 "preproc.y"
{ yyval.str = make1_str("day to minute"); ;
    break;}
case 675:
#line 3461 "preproc.y"
{ yyval.str = make1_str("day to second"); ;
    break;}
case 676:
#line 3462 "preproc.y"
{ yyval.str = make1_str("hour to minute"); ;
    break;}
case 677:
#line 3463 "preproc.y"
{ yyval.str = make1_str("minute to second"); ;
    break;}
case 678:
#line 3464 "preproc.y"
{ yyval.str = make1_str("hour to second"); ;
    break;}
case 679:
#line 3465 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 680:
#line 3476 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 681:
#line 3478 "preproc.y"
{
					yyval.str = make1_str("null");
				;
    break;}
case 682:
#line 3493 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-5].str, make1_str(") in ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 683:
#line 3497 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-6].str, make1_str(") not in ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 684:
#line 3501 "preproc.y"
{
					yyval.str = make4_str(make5_str(make1_str("("), yyvsp[-6].str, make1_str(")"), yyvsp[-4].str, yyvsp[-3].str), make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 685:
#line 3505 "preproc.y"
{
					yyval.str = make3_str(make5_str(make1_str("("), yyvsp[-5].str, make1_str(")"), yyvsp[-3].str, make1_str("(")), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 686:
#line 3509 "preproc.y"
{
					yyval.str = cat3_str(make3_str(make1_str("("), yyvsp[-5].str, make1_str(")")), yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 687:
#line 3515 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 688:
#line 3520 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 689:
#line 3521 "preproc.y"
{ yyval.str = "<"; ;
    break;}
case 690:
#line 3522 "preproc.y"
{ yyval.str = "="; ;
    break;}
case 691:
#line 3523 "preproc.y"
{ yyval.str = ">"; ;
    break;}
case 692:
#line 3524 "preproc.y"
{ yyval.str = "+"; ;
    break;}
case 693:
#line 3525 "preproc.y"
{ yyval.str = "-"; ;
    break;}
case 694:
#line 3526 "preproc.y"
{ yyval.str = "*"; ;
    break;}
case 695:
#line 3527 "preproc.y"
{ yyval.str = "/"; ;
    break;}
case 696:
#line 3530 "preproc.y"
{ yyval.str = make1_str("ANY"); ;
    break;}
case 697:
#line 3531 "preproc.y"
{ yyval.str = make1_str("ALL"); ;
    break;}
case 698:
#line 3536 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 699:
#line 3540 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 700:
#line 3555 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 701:
#line 3559 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 702:
#line 3561 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 703:
#line 3563 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 704:
#line 3567 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 705:
#line 3569 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 706:
#line 3571 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 707:
#line 3573 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 708:
#line 3575 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 709:
#line 3577 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 710:
#line 3579 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 711:
#line 3581 "preproc.y"
{       yyval.str = cat2_str(yyvsp[-2].str, make1_str("= NULL")); ;
    break;}
case 712:
#line 3583 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 713:
#line 3588 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 714:
#line 3590 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 715:
#line 3592 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 716:
#line 3596 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 717:
#line 3600 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 718:
#line 3602 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 719:
#line 3604 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 720:
#line 3606 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 721:
#line 3608 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 722:
#line 3610 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 723:
#line 3612 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make1_str("(*)")); 
				;
    break;}
case 724:
#line 3616 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 725:
#line 3620 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 726:
#line 3624 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 727:
#line 3628 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 728:
#line 3632 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 729:
#line 3638 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 730:
#line 3642 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 731:
#line 3648 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 732:
#line 3652 "preproc.y"
{
  		     		        yyval.str = make1_str("user");
			     	;
    break;}
case 733:
#line 3657 "preproc.y"
{
					yyval.str = make3_str(make1_str("exists("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 734:
#line 3661 "preproc.y"
{
					yyval.str = make3_str(make1_str("extract("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 735:
#line 3665 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 736:
#line 3669 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 737:
#line 3674 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 738:
#line 3678 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 739:
#line 3682 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 740:
#line 3686 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 741:
#line 3690 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 742:
#line 3692 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 743:
#line 3694 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 744:
#line 3696 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 745:
#line 3703 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); }
				;
    break;}
case 746:
#line 3707 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); }
				;
    break;}
case 747:
#line 3711 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); }
				;
    break;}
case 748:
#line 3715 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); }
				;
    break;}
case 749:
#line 3719 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 750:
#line 3723 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 751:
#line 3727 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(" in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 752:
#line 3731 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(" not in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 753:
#line 3735 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-4].str, yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 754:
#line 3739 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("+("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 755:
#line 3743 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("-("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 756:
#line 3747 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("/("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 757:
#line 3751 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("*("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 758:
#line 3755 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("<("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 759:
#line 3759 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(">("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 760:
#line 3763 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("=("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 761:
#line 3767 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("any("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 762:
#line 3771 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 763:
#line 3775 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 764:
#line 3779 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 765:
#line 3783 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 766:
#line 3787 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 767:
#line 3791 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 768:
#line 3795 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 769:
#line 3799 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("all ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 770:
#line 3803 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 771:
#line 3807 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 772:
#line 3811 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 773:
#line 3815 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 774:
#line 3819 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 775:
#line 3823 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 776:
#line 3827 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 777:
#line 3831 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 778:
#line 3833 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 779:
#line 3835 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 780:
#line 3837 "preproc.y"
{       yyval.str = yyvsp[0].str; ;
    break;}
case 781:
#line 3839 "preproc.y"
{ yyval.str = make1_str("?"); ;
    break;}
case 782:
#line 3848 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 783:
#line 3852 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 784:
#line 3854 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 785:
#line 3858 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 786:
#line 3860 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 787:
#line 3862 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 788:
#line 3864 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 789:
#line 3866 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 790:
#line 3871 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 791:
#line 3873 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 792:
#line 3875 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 793:
#line 3879 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 794:
#line 3883 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 795:
#line 3885 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 796:
#line 3887 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 797:
#line 3889 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 798:
#line 3891 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 799:
#line 3895 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 800:
#line 3899 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 801:
#line 3903 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 802:
#line 3907 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 803:
#line 3913 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 804:
#line 3917 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 805:
#line 3923 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 806:
#line 3927 "preproc.y"
{
					yyval.str = make1_str("user");
				;
    break;}
case 807:
#line 3931 "preproc.y"
{
					yyval.str = make3_str(make1_str("position ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 808:
#line 3935 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 809:
#line 3940 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 810:
#line 3944 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 811:
#line 3948 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 812:
#line 3952 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 813:
#line 3956 "preproc.y"
{ 	yyval.str = yyvsp[0].str; ;
    break;}
case 814:
#line 3960 "preproc.y"
{
					yyval.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].str);
				;
    break;}
case 815:
#line 3964 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("["), yyvsp[-4].str, make1_str(":"), yyvsp[-2].str, make1_str("]")), yyvsp[0].str);
				;
    break;}
case 816:
#line 3968 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 817:
#line 3972 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 818:
#line 3974 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 819:
#line 3976 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str("using"), yyvsp[0].str); ;
    break;}
case 820:
#line 3980 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("from"), yyvsp[0].str);
				;
    break;}
case 821:
#line 3984 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 822:
#line 3986 "preproc.y"
{ yyval.str = make1_str("?"); ;
    break;}
case 823:
#line 3989 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 824:
#line 3990 "preproc.y"
{ yyval.str = make1_str("timezone_hour"); ;
    break;}
case 825:
#line 3991 "preproc.y"
{ yyval.str = make1_str("timezone_minute"); ;
    break;}
case 826:
#line 3995 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("in"), yyvsp[0].str); ;
    break;}
case 827:
#line 3997 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 828:
#line 4001 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 829:
#line 4005 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 830:
#line 4007 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 831:
#line 4009 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 832:
#line 4011 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 833:
#line 4013 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 834:
#line 4015 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 835:
#line 4017 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 836:
#line 4019 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 837:
#line 4023 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 838:
#line 4027 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 839:
#line 4029 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 840:
#line 4031 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 841:
#line 4033 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 842:
#line 4035 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 843:
#line 4039 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()"));
				;
    break;}
case 844:
#line 4043 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 845:
#line 4047 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 846:
#line 4051 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 847:
#line 4056 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 848:
#line 4060 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 849:
#line 4064 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 850:
#line 4068 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 851:
#line 4074 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 852:
#line 4078 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 853:
#line 4082 "preproc.y"
{	yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 854:
#line 4084 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 855:
#line 4090 "preproc.y"
{	yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 856:
#line 4092 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 857:
#line 4096 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str("from"), yyvsp[0].str); ;
    break;}
case 858:
#line 4098 "preproc.y"
{ yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 859:
#line 4100 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 860:
#line 4104 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 861:
#line 4108 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 862:
#line 4112 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 863:
#line 4114 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);;
    break;}
case 864:
#line 4118 "preproc.y"
{
					yyval.str = yyvsp[0].str; 
				;
    break;}
case 865:
#line 4122 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 866:
#line 4126 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 867:
#line 4128 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);;
    break;}
case 868:
#line 4147 "preproc.y"
{ yyval.str = cat5_str(make1_str("case"), yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, make1_str("end")); ;
    break;}
case 869:
#line 4149 "preproc.y"
{
					yyval.str = cat5_str(make1_str("nullif("), yyvsp[-3].str, make1_str(","), yyvsp[-1].str, make1_str(")"));

					fprintf(stderr, "NULLIF() not yet fully implemented");
                                ;
    break;}
case 870:
#line 4155 "preproc.y"
{
					yyval.str = cat3_str(make1_str("coalesce("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 871:
#line 4161 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 872:
#line 4163 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 873:
#line 4167 "preproc.y"
{
					yyval.str = cat4_str(make1_str("when"), yyvsp[-2].str, make1_str("then"), yyvsp[0].str);
                               ;
    break;}
case 874:
#line 4172 "preproc.y"
{ yyval.str = cat2_str(make1_str("else"), yyvsp[0].str); ;
    break;}
case 875:
#line 4173 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 876:
#line 4177 "preproc.y"
{
                                       yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                               ;
    break;}
case 877:
#line 4181 "preproc.y"
{
                                       yyval.str = yyvsp[0].str;
                               ;
    break;}
case 878:
#line 4185 "preproc.y"
{       yyval.str = make1_str(""); ;
    break;}
case 879:
#line 4189 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 880:
#line 4193 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 881:
#line 4199 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 882:
#line 4201 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str); ;
    break;}
case 883:
#line 4203 "preproc.y"
{ yyval.str = make2_str(yyvsp[-2].str, make1_str(".*")); ;
    break;}
case 884:
#line 4214 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","),yyvsp[0].str);  ;
    break;}
case 885:
#line 4216 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 886:
#line 4217 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 887:
#line 4221 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-3].str, yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 888:
#line 4225 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 889:
#line 4229 "preproc.y"
{
					yyval.str = make2_str(yyvsp[-2].str, make1_str(".*"));
				;
    break;}
case 890:
#line 4240 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);  ;
    break;}
case 891:
#line 4242 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 892:
#line 4247 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
				;
    break;}
case 893:
#line 4251 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 894:
#line 4255 "preproc.y"
{
					yyval.str = make2_str(yyvsp[-2].str, make1_str(".*"));
				;
    break;}
case 895:
#line 4259 "preproc.y"
{
					yyval.str = make1_str("*");
				;
    break;}
case 896:
#line 4264 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 897:
#line 4265 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 898:
#line 4269 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 899:
#line 4273 "preproc.y"
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
case 900:
#line 4285 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 901:
#line 4286 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 902:
#line 4287 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 903:
#line 4288 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 904:
#line 4289 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 905:
#line 4295 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 906:
#line 4296 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 907:
#line 4298 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 908:
#line 4305 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 909:
#line 4309 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 910:
#line 4313 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 911:
#line 4317 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 912:
#line 4321 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 913:
#line 4323 "preproc.y"
{
					yyval.str = make1_str("true");
				;
    break;}
case 914:
#line 4327 "preproc.y"
{
					yyval.str = make1_str("false");
				;
    break;}
case 915:
#line 4333 "preproc.y"
{
					yyval.str = cat2_str(make_name(), yyvsp[0].str);
				;
    break;}
case 916:
#line 4338 "preproc.y"
{ yyval.str = make_name();;
    break;}
case 917:
#line 4339 "preproc.y"
{ yyval.str = make_name();;
    break;}
case 918:
#line 4340 "preproc.y"
{
							yyval.str = (char *)mm_alloc(strlen(yyvsp[0].str) + 3);
							yyval.str[0]='\'';
				     		        strcpy(yyval.str+1, yyvsp[0].str);
							yyval.str[strlen(yyvsp[0].str)+2]='\0';
							yyval.str[strlen(yyvsp[0].str)+1]='\'';
							free(yyvsp[0].str);
						;
    break;}
case 919:
#line 4348 "preproc.y"
{ yyval.str = yyvsp[0].str;;
    break;}
case 920:
#line 4356 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 921:
#line 4358 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 922:
#line 4360 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 923:
#line 4370 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 924:
#line 4371 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 925:
#line 4372 "preproc.y"
{ yyval.str = make1_str("absolute"); ;
    break;}
case 926:
#line 4373 "preproc.y"
{ yyval.str = make1_str("action"); ;
    break;}
case 927:
#line 4374 "preproc.y"
{ yyval.str = make1_str("after"); ;
    break;}
case 928:
#line 4375 "preproc.y"
{ yyval.str = make1_str("aggregate"); ;
    break;}
case 929:
#line 4376 "preproc.y"
{ yyval.str = make1_str("backward"); ;
    break;}
case 930:
#line 4377 "preproc.y"
{ yyval.str = make1_str("before"); ;
    break;}
case 931:
#line 4378 "preproc.y"
{ yyval.str = make1_str("cache"); ;
    break;}
case 932:
#line 4379 "preproc.y"
{ yyval.str = make1_str("createdb"); ;
    break;}
case 933:
#line 4380 "preproc.y"
{ yyval.str = make1_str("createuser"); ;
    break;}
case 934:
#line 4381 "preproc.y"
{ yyval.str = make1_str("cycle"); ;
    break;}
case 935:
#line 4382 "preproc.y"
{ yyval.str = make1_str("database"); ;
    break;}
case 936:
#line 4383 "preproc.y"
{ yyval.str = make1_str("delimiters"); ;
    break;}
case 937:
#line 4384 "preproc.y"
{ yyval.str = make1_str("double"); ;
    break;}
case 938:
#line 4385 "preproc.y"
{ yyval.str = make1_str("each"); ;
    break;}
case 939:
#line 4386 "preproc.y"
{ yyval.str = make1_str("encoding"); ;
    break;}
case 940:
#line 4387 "preproc.y"
{ yyval.str = make1_str("forward"); ;
    break;}
case 941:
#line 4388 "preproc.y"
{ yyval.str = make1_str("function"); ;
    break;}
case 942:
#line 4389 "preproc.y"
{ yyval.str = make1_str("handler"); ;
    break;}
case 943:
#line 4390 "preproc.y"
{ yyval.str = make1_str("increment"); ;
    break;}
case 944:
#line 4391 "preproc.y"
{ yyval.str = make1_str("index"); ;
    break;}
case 945:
#line 4392 "preproc.y"
{ yyval.str = make1_str("inherits"); ;
    break;}
case 946:
#line 4393 "preproc.y"
{ yyval.str = make1_str("insensitive"); ;
    break;}
case 947:
#line 4394 "preproc.y"
{ yyval.str = make1_str("instead"); ;
    break;}
case 948:
#line 4395 "preproc.y"
{ yyval.str = make1_str("isnull"); ;
    break;}
case 949:
#line 4396 "preproc.y"
{ yyval.str = make1_str("key"); ;
    break;}
case 950:
#line 4397 "preproc.y"
{ yyval.str = make1_str("language"); ;
    break;}
case 951:
#line 4398 "preproc.y"
{ yyval.str = make1_str("lancompiler"); ;
    break;}
case 952:
#line 4399 "preproc.y"
{ yyval.str = make1_str("location"); ;
    break;}
case 953:
#line 4400 "preproc.y"
{ yyval.str = make1_str("match"); ;
    break;}
case 954:
#line 4401 "preproc.y"
{ yyval.str = make1_str("maxvalue"); ;
    break;}
case 955:
#line 4402 "preproc.y"
{ yyval.str = make1_str("minvalue"); ;
    break;}
case 956:
#line 4403 "preproc.y"
{ yyval.str = make1_str("next"); ;
    break;}
case 957:
#line 4404 "preproc.y"
{ yyval.str = make1_str("nocreatedb"); ;
    break;}
case 958:
#line 4405 "preproc.y"
{ yyval.str = make1_str("nocreateuser"); ;
    break;}
case 959:
#line 4406 "preproc.y"
{ yyval.str = make1_str("nothing"); ;
    break;}
case 960:
#line 4407 "preproc.y"
{ yyval.str = make1_str("notnull"); ;
    break;}
case 961:
#line 4408 "preproc.y"
{ yyval.str = make1_str("of"); ;
    break;}
case 962:
#line 4409 "preproc.y"
{ yyval.str = make1_str("oids"); ;
    break;}
case 963:
#line 4410 "preproc.y"
{ yyval.str = make1_str("only"); ;
    break;}
case 964:
#line 4411 "preproc.y"
{ yyval.str = make1_str("operator"); ;
    break;}
case 965:
#line 4412 "preproc.y"
{ yyval.str = make1_str("option"); ;
    break;}
case 966:
#line 4413 "preproc.y"
{ yyval.str = make1_str("password"); ;
    break;}
case 967:
#line 4414 "preproc.y"
{ yyval.str = make1_str("prior"); ;
    break;}
case 968:
#line 4415 "preproc.y"
{ yyval.str = make1_str("privileges"); ;
    break;}
case 969:
#line 4416 "preproc.y"
{ yyval.str = make1_str("procedural"); ;
    break;}
case 970:
#line 4417 "preproc.y"
{ yyval.str = make1_str("read"); ;
    break;}
case 971:
#line 4419 "preproc.y"
{ yyval.str = make1_str("relative"); ;
    break;}
case 972:
#line 4420 "preproc.y"
{ yyval.str = make1_str("rename"); ;
    break;}
case 973:
#line 4421 "preproc.y"
{ yyval.str = make1_str("returns"); ;
    break;}
case 974:
#line 4422 "preproc.y"
{ yyval.str = make1_str("row"); ;
    break;}
case 975:
#line 4423 "preproc.y"
{ yyval.str = make1_str("rule"); ;
    break;}
case 976:
#line 4424 "preproc.y"
{ yyval.str = make1_str("scroll"); ;
    break;}
case 977:
#line 4425 "preproc.y"
{ yyval.str = make1_str("sequence"); ;
    break;}
case 978:
#line 4426 "preproc.y"
{ yyval.str = make1_str("serial"); ;
    break;}
case 979:
#line 4427 "preproc.y"
{ yyval.str = make1_str("start"); ;
    break;}
case 980:
#line 4428 "preproc.y"
{ yyval.str = make1_str("statement"); ;
    break;}
case 981:
#line 4429 "preproc.y"
{ yyval.str = make1_str("stdin"); ;
    break;}
case 982:
#line 4430 "preproc.y"
{ yyval.str = make1_str("stdout"); ;
    break;}
case 983:
#line 4431 "preproc.y"
{ yyval.str = make1_str("time"); ;
    break;}
case 984:
#line 4432 "preproc.y"
{ yyval.str = make1_str("timestamp"); ;
    break;}
case 985:
#line 4433 "preproc.y"
{ yyval.str = make1_str("timezone_hour"); ;
    break;}
case 986:
#line 4434 "preproc.y"
{ yyval.str = make1_str("timezone_minute"); ;
    break;}
case 987:
#line 4435 "preproc.y"
{ yyval.str = make1_str("trigger"); ;
    break;}
case 988:
#line 4436 "preproc.y"
{ yyval.str = make1_str("trusted"); ;
    break;}
case 989:
#line 4437 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 990:
#line 4438 "preproc.y"
{ yyval.str = make1_str("valid"); ;
    break;}
case 991:
#line 4439 "preproc.y"
{ yyval.str = make1_str("version"); ;
    break;}
case 992:
#line 4440 "preproc.y"
{ yyval.str = make1_str("zone"); ;
    break;}
case 993:
#line 4441 "preproc.y"
{ yyval.str = make1_str("at"); ;
    break;}
case 994:
#line 4442 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 995:
#line 4443 "preproc.y"
{ yyval.str = make1_str("break"); ;
    break;}
case 996:
#line 4444 "preproc.y"
{ yyval.str = make1_str("call"); ;
    break;}
case 997:
#line 4445 "preproc.y"
{ yyval.str = make1_str("connect"); ;
    break;}
case 998:
#line 4446 "preproc.y"
{ yyval.str = make1_str("connection"); ;
    break;}
case 999:
#line 4447 "preproc.y"
{ yyval.str = make1_str("continue"); ;
    break;}
case 1000:
#line 4448 "preproc.y"
{ yyval.str = make1_str("deallocate"); ;
    break;}
case 1001:
#line 4449 "preproc.y"
{ yyval.str = make1_str("disconnect"); ;
    break;}
case 1002:
#line 4450 "preproc.y"
{ yyval.str = make1_str("found"); ;
    break;}
case 1003:
#line 4451 "preproc.y"
{ yyval.str = make1_str("go"); ;
    break;}
case 1004:
#line 4452 "preproc.y"
{ yyval.str = make1_str("goto"); ;
    break;}
case 1005:
#line 4453 "preproc.y"
{ yyval.str = make1_str("identified"); ;
    break;}
case 1006:
#line 4454 "preproc.y"
{ yyval.str = make1_str("immediate"); ;
    break;}
case 1007:
#line 4455 "preproc.y"
{ yyval.str = make1_str("indicator"); ;
    break;}
case 1008:
#line 4456 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 1009:
#line 4457 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 1010:
#line 4458 "preproc.y"
{ yyval.str = make1_str("open"); ;
    break;}
case 1011:
#line 4459 "preproc.y"
{ yyval.str = make1_str("prepare"); ;
    break;}
case 1012:
#line 4460 "preproc.y"
{ yyval.str = make1_str("release"); ;
    break;}
case 1013:
#line 4461 "preproc.y"
{ yyval.str = make1_str("section"); ;
    break;}
case 1014:
#line 4462 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 1015:
#line 4463 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1016:
#line 4464 "preproc.y"
{ yyval.str = make1_str("sqlerror"); ;
    break;}
case 1017:
#line 4465 "preproc.y"
{ yyval.str = make1_str("sqlprint"); ;
    break;}
case 1018:
#line 4466 "preproc.y"
{ yyval.str = make1_str("sqlwarning"); ;
    break;}
case 1019:
#line 4467 "preproc.y"
{ yyval.str = make1_str("stop"); ;
    break;}
case 1020:
#line 4468 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 1021:
#line 4469 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 1022:
#line 4470 "preproc.y"
{ yyval.str = make1_str("var"); ;
    break;}
case 1023:
#line 4471 "preproc.y"
{ yyval.str = make1_str("whenever"); ;
    break;}
case 1024:
#line 4483 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1025:
#line 4484 "preproc.y"
{ yyval.str = make1_str("abort"); ;
    break;}
case 1026:
#line 4485 "preproc.y"
{ yyval.str = make1_str("analyze"); ;
    break;}
case 1027:
#line 4486 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 1028:
#line 4487 "preproc.y"
{ yyval.str = make1_str("case"); ;
    break;}
case 1029:
#line 4488 "preproc.y"
{ yyval.str = make1_str("cluster"); ;
    break;}
case 1030:
#line 4489 "preproc.y"
{ yyval.str = make1_str("coalesce"); ;
    break;}
case 1031:
#line 4490 "preproc.y"
{ yyval.str = make1_str("constraint"); ;
    break;}
case 1032:
#line 4491 "preproc.y"
{ yyval.str = make1_str("copy"); ;
    break;}
case 1033:
#line 4492 "preproc.y"
{ yyval.str = make1_str("current"); ;
    break;}
case 1034:
#line 4493 "preproc.y"
{ yyval.str = make1_str("do"); ;
    break;}
case 1035:
#line 4494 "preproc.y"
{ yyval.str = make1_str("else"); ;
    break;}
case 1036:
#line 4495 "preproc.y"
{ yyval.str = make1_str("end"); ;
    break;}
case 1037:
#line 4496 "preproc.y"
{ yyval.str = make1_str("explain"); ;
    break;}
case 1038:
#line 4497 "preproc.y"
{ yyval.str = make1_str("extend"); ;
    break;}
case 1039:
#line 4498 "preproc.y"
{ yyval.str = make1_str("false"); ;
    break;}
case 1040:
#line 4499 "preproc.y"
{ yyval.str = make1_str("foreign"); ;
    break;}
case 1041:
#line 4500 "preproc.y"
{ yyval.str = make1_str("group"); ;
    break;}
case 1042:
#line 4501 "preproc.y"
{ yyval.str = make1_str("listen"); ;
    break;}
case 1043:
#line 4502 "preproc.y"
{ yyval.str = make1_str("load"); ;
    break;}
case 1044:
#line 4503 "preproc.y"
{ yyval.str = make1_str("lock"); ;
    break;}
case 1045:
#line 4504 "preproc.y"
{ yyval.str = make1_str("move"); ;
    break;}
case 1046:
#line 4505 "preproc.y"
{ yyval.str = make1_str("new"); ;
    break;}
case 1047:
#line 4506 "preproc.y"
{ yyval.str = make1_str("none"); ;
    break;}
case 1048:
#line 4507 "preproc.y"
{ yyval.str = make1_str("nullif"); ;
    break;}
case 1049:
#line 4508 "preproc.y"
{ yyval.str = make1_str("order"); ;
    break;}
case 1050:
#line 4509 "preproc.y"
{ yyval.str = make1_str("position"); ;
    break;}
case 1051:
#line 4510 "preproc.y"
{ yyval.str = make1_str("precision"); ;
    break;}
case 1052:
#line 4511 "preproc.y"
{ yyval.str = make1_str("reset"); ;
    break;}
case 1053:
#line 4512 "preproc.y"
{ yyval.str = make1_str("setof"); ;
    break;}
case 1054:
#line 4513 "preproc.y"
{ yyval.str = make1_str("show"); ;
    break;}
case 1055:
#line 4514 "preproc.y"
{ yyval.str = make1_str("table"); ;
    break;}
case 1056:
#line 4515 "preproc.y"
{ yyval.str = make1_str("then"); ;
    break;}
case 1057:
#line 4516 "preproc.y"
{ yyval.str = make1_str("transaction"); ;
    break;}
case 1058:
#line 4517 "preproc.y"
{ yyval.str = make1_str("true"); ;
    break;}
case 1059:
#line 4518 "preproc.y"
{ yyval.str = make1_str("vacuum"); ;
    break;}
case 1060:
#line 4519 "preproc.y"
{ yyval.str = make1_str("verbose"); ;
    break;}
case 1061:
#line 4520 "preproc.y"
{ yyval.str = make1_str("when"); ;
    break;}
case 1062:
#line 4524 "preproc.y"
{
					if (QueryIsRule)
						yyval.str = make1_str("current");
					else
						yyerror("CURRENT used in non-rule query");
				;
    break;}
case 1063:
#line 4531 "preproc.y"
{
					if (QueryIsRule)
						yyval.str = make1_str("new");
					else
						yyerror("NEW used in non-rule query");
				;
    break;}
case 1064:
#line 4547 "preproc.y"
{
			yyval.str = make5_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str, make1_str(","), yyvsp[-1].str);
                ;
    break;}
case 1065:
#line 4551 "preproc.y"
{
                	yyval.str = make1_str("NULL,NULL,NULL,\"DEFAULT\"");
                ;
    break;}
case 1066:
#line 4556 "preproc.y"
{
		       yyval.str = make3_str(make1_str("NULL,"), yyvsp[0].str, make1_str(",NULL"));
		;
    break;}
case 1067:
#line 4561 "preproc.y"
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
case 1068:
#line 4572 "preproc.y"
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
case 1069:
#line 4595 "preproc.y"
{
		  yyval.str = yyvsp[0].str;
		;
    break;}
case 1070:
#line 4599 "preproc.y"
{
		  yyval.str = mm_strdup(yyvsp[0].str);
		  yyval.str[0] = '\"';
		  yyval.str[strlen(yyval.str) - 1] = '\"';
		  free(yyvsp[0].str);
		;
    break;}
case 1071:
#line 4607 "preproc.y"
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
case 1072:
#line 4624 "preproc.y"
{
		  if (strcmp(yyvsp[-1].str, "@") != 0 && strcmp(yyvsp[-1].str, "://") != 0)
		  {
		    sprintf(errortext, "parse error at or near '%s'", yyvsp[-1].str);
		    yyerror(errortext);
		  }

		  yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str);
	        ;
    break;}
case 1073:
#line 4634 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1074:
#line 4635 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1075:
#line 4637 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1076:
#line 4638 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str); ;
    break;}
case 1077:
#line 4640 "preproc.y"
{ yyval.str = make2_str(make1_str(":"), yyvsp[0].str); ;
    break;}
case 1078:
#line 4641 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1079:
#line 4643 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1080:
#line 4644 "preproc.y"
{ yyval.str = make1_str("NULL"); ;
    break;}
case 1081:
#line 4646 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1082:
#line 4647 "preproc.y"
{ yyval.str = make1_str("NULL,NULL"); ;
    break;}
case 1083:
#line 4650 "preproc.y"
{
                        yyval.str = make2_str(yyvsp[0].str, make1_str(",NULL"));
	        ;
    break;}
case 1084:
#line 4654 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1085:
#line 4658 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-3].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1086:
#line 4662 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1087:
#line 4666 "preproc.y"
{ if (yyvsp[0].str[0] == '\"')
				yyval.str = yyvsp[0].str;
			  else
				yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\""));
			;
    break;}
case 1088:
#line 4671 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1089:
#line 4672 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1090:
#line 4675 "preproc.y"
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
case 1091:
#line 4699 "preproc.y"
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
case 1092:
#line 4711 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1093:
#line 4718 "preproc.y"
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
				        this->command =  cat5_str(make1_str("declare"), mm_strdup(yyvsp[-5].str), yyvsp[-4].str, make1_str("cursor for ?"), yyvsp[0].str);
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
case 1094:
#line 4760 "preproc.y"
{ yyval.str = make3_str(make1_str("ECPGdeallocate(__LINE__, \""), yyvsp[0].str, make1_str("\");")); ;
    break;}
case 1095:
#line 4766 "preproc.y"
{
		fputs("/* exec sql begin declare section */", yyout);
	        output_line_number();
	;
    break;}
case 1096:
#line 4771 "preproc.y"
{
		fprintf(yyout, "%s/* exec sql end declare section */", yyvsp[-1].str);
		free(yyvsp[-1].str);
		output_line_number();
	;
    break;}
case 1097:
#line 4777 "preproc.y"
{;
    break;}
case 1098:
#line 4779 "preproc.y"
{;
    break;}
case 1099:
#line 4782 "preproc.y"
{
		yyval.str = make1_str("");
	;
    break;}
case 1100:
#line 4786 "preproc.y"
{
		yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
	;
    break;}
case 1101:
#line 4791 "preproc.y"
{
		actual_storage[struct_level] = mm_strdup(yyvsp[0].str);
	;
    break;}
case 1102:
#line 4795 "preproc.y"
{
		actual_type[struct_level].type_enum = yyvsp[0].type.type_enum;
		actual_type[struct_level].type_dimension = yyvsp[0].type.type_dimension;
		actual_type[struct_level].type_index = yyvsp[0].type.type_index;
	;
    break;}
case 1103:
#line 4801 "preproc.y"
{
 		yyval.str = cat4_str(yyvsp[-5].str, yyvsp[-3].type.type_str, yyvsp[-1].str, make1_str(";\n"));
	;
    break;}
case 1104:
#line 4805 "preproc.y"
{ yyval.str = make1_str("extern"); ;
    break;}
case 1105:
#line 4806 "preproc.y"
{ yyval.str = make1_str("static"); ;
    break;}
case 1106:
#line 4807 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1107:
#line 4808 "preproc.y"
{ yyval.str = make1_str("const"); ;
    break;}
case 1108:
#line 4809 "preproc.y"
{ yyval.str = make1_str("register"); ;
    break;}
case 1109:
#line 4810 "preproc.y"
{ yyval.str = make1_str("auto"); ;
    break;}
case 1110:
#line 4811 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1111:
#line 4814 "preproc.y"
{
			yyval.type.type_enum = yyvsp[0].type_enum;
			yyval.type.type_str = mm_strdup(ECPGtype_name(yyvsp[0].type_enum));
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1112:
#line 4821 "preproc.y"
{
			yyval.type.type_enum = ECPGt_varchar;
			yyval.type.type_str = make1_str("");
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1113:
#line 4828 "preproc.y"
{
			yyval.type.type_enum = ECPGt_struct;
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1114:
#line 4835 "preproc.y"
{
			yyval.type.type_enum = ECPGt_union;
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1115:
#line 4842 "preproc.y"
{
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_enum = ECPGt_int;
		
	yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1116:
#line 4850 "preproc.y"
{
			/* this is for typedef'ed types */
			struct typedefs *this = get_typedef(yyvsp[0].str);

			yyval.type.type_str = (this->type->type_enum == ECPGt_varchar) ? make1_str("") : mm_strdup(this->name);
                        yyval.type.type_enum = this->type->type_enum;
			yyval.type.type_dimension = this->type->type_dimension;
  			yyval.type.type_index = this->type->type_index;
			struct_member_list[struct_level] = ECPGstruct_member_dup(this->struct_member_list);
		;
    break;}
case 1117:
#line 4862 "preproc.y"
{
		yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1118:
#line 4866 "preproc.y"
{ yyval.str = cat2_str(make1_str("enum"), yyvsp[0].str); ;
    break;}
case 1119:
#line 4869 "preproc.y"
{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1120:
#line 4876 "preproc.y"
{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1121:
#line 4883 "preproc.y"
{
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    yyval.str = cat2_str(make1_str("struct"), yyvsp[0].str);
	;
    break;}
case 1122:
#line 4891 "preproc.y"
{
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    yyval.str = cat2_str(make1_str("union"), yyvsp[0].str);
	;
    break;}
case 1123:
#line 4898 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1124:
#line 4899 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1125:
#line 4901 "preproc.y"
{ yyval.type_enum = ECPGt_short; ;
    break;}
case 1126:
#line 4902 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_short; ;
    break;}
case 1127:
#line 4903 "preproc.y"
{ yyval.type_enum = ECPGt_int; ;
    break;}
case 1128:
#line 4904 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_int; ;
    break;}
case 1129:
#line 4905 "preproc.y"
{ yyval.type_enum = ECPGt_long; ;
    break;}
case 1130:
#line 4906 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_long; ;
    break;}
case 1131:
#line 4907 "preproc.y"
{ yyval.type_enum = ECPGt_float; ;
    break;}
case 1132:
#line 4908 "preproc.y"
{ yyval.type_enum = ECPGt_double; ;
    break;}
case 1133:
#line 4909 "preproc.y"
{ yyval.type_enum = ECPGt_bool; ;
    break;}
case 1134:
#line 4910 "preproc.y"
{ yyval.type_enum = ECPGt_char; ;
    break;}
case 1135:
#line 4911 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_char; ;
    break;}
case 1136:
#line 4913 "preproc.y"
{ yyval.type_enum = ECPGt_varchar; ;
    break;}
case 1137:
#line 4916 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 1138:
#line 4920 "preproc.y"
{
		yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
	;
    break;}
case 1139:
#line 4925 "preproc.y"
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
				  case -1:
                                  case 1:
                                      *dim = '\0';
                                      break;
                                  default:
                                      sprintf(dim, "[%d]", dimension);
                                      break;
                               }
			       sprintf(ascii_len, "%d", length);

                               if (length == 0)
				   yyerror ("pointer to varchar are not implemented");

			       if (dimension == 0)
				   yyval.str = make4_str(make5_str(mm_strdup(actual_storage[struct_level]), make1_str(" struct varchar_"), mm_strdup(yyvsp[-2].str), make1_str(" { int len; char arr["), mm_strdup(ascii_len)), make1_str("]; } *"), mm_strdup(yyvsp[-2].str), yyvsp[0].str);
			       else
                                   yyval.str = make5_str(make5_str(mm_strdup(actual_storage[struct_level]), make1_str(" struct varchar_"), mm_strdup(yyvsp[-2].str), make1_str(" { int len; char arr["), mm_strdup(ascii_len)), make1_str("]; } "), mm_strdup(yyvsp[-2].str), mm_strdup(dim), yyvsp[0].str);

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
case 1140:
#line 4999 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1141:
#line 5000 "preproc.y"
{ yyval.str = make2_str(make1_str("="), yyvsp[0].str); ;
    break;}
case 1142:
#line 5002 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1143:
#line 5003 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 1144:
#line 5010 "preproc.y"
{
		/* this is only supported for compatibility */
		yyval.str = cat3_str(make1_str("/* declare statement"), yyvsp[0].str, make1_str("*/"));
	;
    break;}
case 1145:
#line 5017 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1146:
#line 5019 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1147:
#line 5020 "preproc.y"
{ yyval.str = make1_str("CURRENT"); ;
    break;}
case 1148:
#line 5021 "preproc.y"
{ yyval.str = make1_str("ALL"); ;
    break;}
case 1149:
#line 5022 "preproc.y"
{ yyval.str = make1_str("CURRENT"); ;
    break;}
case 1150:
#line 5024 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1151:
#line 5025 "preproc.y"
{ yyval.str = make1_str("DEFAULT"); ;
    break;}
case 1152:
#line 5031 "preproc.y"
{ 
		struct variable *thisquery = (struct variable *)mm_alloc(sizeof(struct variable));

		thisquery->type = &ecpg_query;
		thisquery->brace_level = 0;
		thisquery->next = NULL;
		thisquery->name = yyvsp[0].str;

		add_variable(&argsinsert, thisquery, &no_indicator); 

		yyval.str = make1_str("?");
	;
    break;}
case 1153:
#line 5044 "preproc.y"
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
case 1154:
#line 5055 "preproc.y"
{
		yyval.str = make1_str("?");
	;
    break;}
case 1156:
#line 5060 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1157:
#line 5066 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1158:
#line 5071 "preproc.y"
{
		yyval.str = yyvsp[-1].str;
;
    break;}
case 1159:
#line 5075 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1160:
#line 5076 "preproc.y"
{
					/* yyerror ("open cursor with variables not implemented yet"); */
					yyval.str = make1_str("");
				;
    break;}
case 1163:
#line 5088 "preproc.y"
{
		yyval.str = make4_str(make1_str("\""), yyvsp[-2].str, make1_str("\", "), yyvsp[0].str);
	;
    break;}
case 1164:
#line 5098 "preproc.y"
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
case 1165:
#line 5114 "preproc.y"
{
				yyval.str = yyvsp[0].str;
                        ;
    break;}
case 1166:
#line 5122 "preproc.y"
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
case 1167:
#line 5164 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1168:
#line 5170 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1169:
#line 5176 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1170:
#line 5182 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1171:
#line 5188 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 1172:
#line 5196 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1173:
#line 5202 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1174:
#line 5208 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1175:
#line 5214 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1176:
#line 5220 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 1177:
#line 5226 "preproc.y"
{ yyval.str = make1_str("reference"); ;
    break;}
case 1178:
#line 5227 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1179:
#line 5230 "preproc.y"
{
		yyval.type.type_str = make1_str("char");
                yyval.type.type_enum = ECPGt_char;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1180:
#line 5237 "preproc.y"
{
		yyval.type.type_str = make1_str("varchar");
                yyval.type.type_enum = ECPGt_varchar;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1181:
#line 5244 "preproc.y"
{
		yyval.type.type_str = make1_str("float");
                yyval.type.type_enum = ECPGt_float;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1182:
#line 5251 "preproc.y"
{
		yyval.type.type_str = make1_str("double");
                yyval.type.type_enum = ECPGt_double;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1183:
#line 5258 "preproc.y"
{
		yyval.type.type_str = make1_str("int");
       	        yyval.type.type_enum = ECPGt_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1184:
#line 5265 "preproc.y"
{
		yyval.type.type_str = make1_str("int");
       	        yyval.type.type_enum = ECPGt_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1185:
#line 5272 "preproc.y"
{
		yyval.type.type_str = make1_str("short");
       	        yyval.type.type_enum = ECPGt_short;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1186:
#line 5279 "preproc.y"
{
		yyval.type.type_str = make1_str("long");
       	        yyval.type.type_enum = ECPGt_long;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1187:
#line 5286 "preproc.y"
{
		yyval.type.type_str = make1_str("bool");
       	        yyval.type.type_enum = ECPGt_bool;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1188:
#line 5293 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned int");
       	        yyval.type.type_enum = ECPGt_unsigned_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1189:
#line 5300 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned short");
       	        yyval.type.type_enum = ECPGt_unsigned_short;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1190:
#line 5307 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned long");
       	        yyval.type.type_enum = ECPGt_unsigned_long;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1191:
#line 5314 "preproc.y"
{
		struct_member_list[struct_level++] = NULL;
		if (struct_level >= STRUCT_DEPTH)
        		yyerror("Too many levels in nested structure definition");
	;
    break;}
case 1192:
#line 5319 "preproc.y"
{
		ECPGfree_struct_member(struct_member_list[struct_level--]);
		yyval.type.type_str = cat3_str(make1_str("struct {"), yyvsp[-1].str, make1_str("}"));
		yyval.type.type_enum = ECPGt_struct;
                yyval.type.type_index = -1;
                yyval.type.type_dimension = -1;
	;
    break;}
case 1193:
#line 5327 "preproc.y"
{
		struct_member_list[struct_level++] = NULL;
		if (struct_level >= STRUCT_DEPTH)
        		yyerror("Too many levels in nested structure definition");
	;
    break;}
case 1194:
#line 5332 "preproc.y"
{
		ECPGfree_struct_member(struct_member_list[struct_level--]);
		yyval.type.type_str = cat3_str(make1_str("union {"), yyvsp[-1].str, make1_str("}"));
		yyval.type.type_enum = ECPGt_union;
                yyval.type.type_index = -1;
                yyval.type.type_dimension = -1;
	;
    break;}
case 1195:
#line 5340 "preproc.y"
{
		struct typedefs *this = get_typedef(yyvsp[0].str);

		yyval.type.type_str = mm_strdup(yyvsp[0].str);
		yyval.type.type_enum = this->type->type_enum;
		yyval.type.type_dimension = this->type->type_dimension;
		yyval.type.type_index = this->type->type_index;
		struct_member_list[struct_level] = this->struct_member_list;
	;
    break;}
case 1198:
#line 5353 "preproc.y"
{
		yyval.str = make1_str("");
	;
    break;}
case 1199:
#line 5357 "preproc.y"
{
		yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
	;
    break;}
case 1200:
#line 5363 "preproc.y"
{
		actual_type[struct_level].type_enum = yyvsp[0].type.type_enum;
		actual_type[struct_level].type_dimension = yyvsp[0].type.type_dimension;
		actual_type[struct_level].type_index = yyvsp[0].type.type_index;
	;
    break;}
case 1201:
#line 5369 "preproc.y"
{
		yyval.str = cat3_str(yyvsp[-3].type.type_str, yyvsp[-1].str, make1_str(";"));
	;
    break;}
case 1202:
#line 5374 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 1203:
#line 5378 "preproc.y"
{
		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
	;
    break;}
case 1204:
#line 5383 "preproc.y"
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
case 1205:
#line 5454 "preproc.y"
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
case 1206:
#line 5508 "preproc.y"
{
	when_error.code = yyvsp[0].action.code;
	when_error.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever sqlerror "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1207:
#line 5513 "preproc.y"
{
	when_nf.code = yyvsp[0].action.code;
	when_nf.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever not found "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1208:
#line 5518 "preproc.y"
{
	when_warn.code = yyvsp[0].action.code;
	when_warn.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever sql_warning "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1209:
#line 5524 "preproc.y"
{
	yyval.action.code = W_NOTHING;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("continue");
;
    break;}
case 1210:
#line 5529 "preproc.y"
{
	yyval.action.code = W_SQLPRINT;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("sqlprint");
;
    break;}
case 1211:
#line 5534 "preproc.y"
{
	yyval.action.code = W_STOP;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("stop");
;
    break;}
case 1212:
#line 5539 "preproc.y"
{
        yyval.action.code = W_GOTO;
        yyval.action.command = strdup(yyvsp[0].str);
	yyval.action.str = cat2_str(make1_str("goto "), yyvsp[0].str);
;
    break;}
case 1213:
#line 5544 "preproc.y"
{
        yyval.action.code = W_GOTO;
        yyval.action.command = strdup(yyvsp[0].str);
	yyval.action.str = cat2_str(make1_str("goto "), yyvsp[0].str);
;
    break;}
case 1214:
#line 5549 "preproc.y"
{
	yyval.action.code = W_DO;
	yyval.action.command = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
	yyval.action.str = cat2_str(make1_str("do"), mm_strdup(yyval.action.command));
;
    break;}
case 1215:
#line 5554 "preproc.y"
{
        yyval.action.code = W_BREAK;
        yyval.action.command = NULL;
        yyval.action.str = make1_str("break");
;
    break;}
case 1216:
#line 5559 "preproc.y"
{
	yyval.action.code = W_DO;
	yyval.action.command = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
	yyval.action.str = cat2_str(make1_str("call"), mm_strdup(yyval.action.command));
;
    break;}
case 1217:
#line 5567 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 1218:
#line 5571 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 1219:
#line 5573 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 1220:
#line 5575 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 1221:
#line 5579 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 1222:
#line 5581 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 1223:
#line 5583 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 1224:
#line 5585 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 1225:
#line 5587 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 1226:
#line 5589 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 1227:
#line 5591 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 1228:
#line 5593 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 1229:
#line 5597 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 1230:
#line 5599 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 1231:
#line 5601 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 1232:
#line 5605 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 1233:
#line 5609 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 1234:
#line 5611 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 1235:
#line 5613 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 1236:
#line 5615 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 1237:
#line 5617 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1238:
#line 5619 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1239:
#line 5621 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make1_str("(*)")); 
				;
    break;}
case 1240:
#line 5625 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 1241:
#line 5629 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1242:
#line 5633 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 1243:
#line 5637 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 1244:
#line 5641 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 1245:
#line 5647 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 1246:
#line 5651 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 1247:
#line 5657 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 1248:
#line 5661 "preproc.y"
{
					yyval.str = make3_str(make1_str("exists("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1249:
#line 5665 "preproc.y"
{
					yyval.str = make3_str(make1_str("extract("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1250:
#line 5669 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1251:
#line 5673 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1252:
#line 5678 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1253:
#line 5682 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1254:
#line 5686 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1255:
#line 5690 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1256:
#line 5694 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 1257:
#line 5696 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 1258:
#line 5698 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 1259:
#line 5700 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 1260:
#line 5707 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); }
				;
    break;}
case 1261:
#line 5711 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); }
				;
    break;}
case 1262:
#line 5715 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); }
				;
    break;}
case 1263:
#line 5719 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); }
				;
    break;}
case 1264:
#line 5723 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 1265:
#line 5727 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 1266:
#line 5731 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1267:
#line 5735 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("not in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1268:
#line 5739 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-4].str, yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1269:
#line 5743 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("+("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1270:
#line 5747 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("-("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1271:
#line 5751 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("/("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1272:
#line 5755 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("*("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1273:
#line 5759 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("<("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1274:
#line 5763 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(">("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1275:
#line 5767 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("=("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1276:
#line 5771 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("any ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1277:
#line 5775 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1278:
#line 5779 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1279:
#line 5783 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1280:
#line 5787 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1281:
#line 5791 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1282:
#line 5795 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1283:
#line 5799 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1284:
#line 5803 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("all ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1285:
#line 5807 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1286:
#line 5811 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1287:
#line 5815 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1288:
#line 5819 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1289:
#line 5823 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1290:
#line 5827 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1291:
#line 5831 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1292:
#line 5835 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 1293:
#line 5837 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 1294:
#line 5839 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 1295:
#line 5841 "preproc.y"
{ 	yyval.str = yyvsp[0].str; ;
    break;}
case 1298:
#line 5846 "preproc.y"
{ reset_variables();;
    break;}
case 1299:
#line 5848 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1300:
#line 5849 "preproc.y"
{ yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1301:
#line 5851 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1302:
#line 5852 "preproc.y"
{ yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1303:
#line 5854 "preproc.y"
{
		add_variable(&argsresult, find_variable(yyvsp[-1].str), (yyvsp[0].str == NULL) ? &no_indicator : find_variable(yyvsp[0].str)); 
;
    break;}
case 1304:
#line 5858 "preproc.y"
{
		add_variable(&argsinsert, find_variable(yyvsp[-1].str), (yyvsp[0].str == NULL) ? &no_indicator : find_variable(yyvsp[0].str)); 
;
    break;}
case 1305:
#line 5862 "preproc.y"
{
		add_variable(&argsinsert, find_variable(yyvsp[0].str), &no_indicator); 
		yyval.str = make1_str("?");
;
    break;}
case 1306:
#line 5867 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1307:
#line 5869 "preproc.y"
{ yyval.str = NULL; ;
    break;}
case 1308:
#line 5870 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1309:
#line 5871 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1310:
#line 5872 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1311:
#line 5874 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1312:
#line 5875 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1313:
#line 5880 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1314:
#line 5882 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1315:
#line 5884 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1316:
#line 5886 "preproc.y"
{
			yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str);
		;
    break;}
case 1318:
#line 5890 "preproc.y"
{ yyval.str = make1_str(";"); ;
    break;}
case 1319:
#line 5892 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1320:
#line 5893 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1321:
#line 5894 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1322:
#line 5895 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1323:
#line 5896 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 1324:
#line 5897 "preproc.y"
{ yyval.str = make1_str("auto"); ;
    break;}
case 1325:
#line 5898 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 1326:
#line 5899 "preproc.y"
{ yyval.str = make1_str("char"); ;
    break;}
case 1327:
#line 5900 "preproc.y"
{ yyval.str = make1_str("const"); ;
    break;}
case 1328:
#line 5901 "preproc.y"
{ yyval.str = make1_str("double"); ;
    break;}
case 1329:
#line 5902 "preproc.y"
{ yyval.str = make1_str("enum"); ;
    break;}
case 1330:
#line 5903 "preproc.y"
{ yyval.str = make1_str("extern"); ;
    break;}
case 1331:
#line 5904 "preproc.y"
{ yyval.str = make1_str("float"); ;
    break;}
case 1332:
#line 5905 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 1333:
#line 5906 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 1334:
#line 5907 "preproc.y"
{ yyval.str = make1_str("register"); ;
    break;}
case 1335:
#line 5908 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 1336:
#line 5909 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1337:
#line 5910 "preproc.y"
{ yyval.str = make1_str("static"); ;
    break;}
case 1338:
#line 5911 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 1339:
#line 5912 "preproc.y"
{ yyval.str = make1_str("union"); ;
    break;}
case 1340:
#line 5913 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 1341:
#line 5914 "preproc.y"
{ yyval.str = make1_str("varchar"); ;
    break;}
case 1342:
#line 5915 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 1343:
#line 5916 "preproc.y"
{ yyval.str = make1_str("["); ;
    break;}
case 1344:
#line 5917 "preproc.y"
{ yyval.str = make1_str("]"); ;
    break;}
case 1345:
#line 5918 "preproc.y"
{ yyval.str = make1_str("("); ;
    break;}
case 1346:
#line 5919 "preproc.y"
{ yyval.str = make1_str(")"); ;
    break;}
case 1347:
#line 5920 "preproc.y"
{ yyval.str = make1_str("="); ;
    break;}
case 1348:
#line 5921 "preproc.y"
{ yyval.str = make1_str(","); ;
    break;}
case 1349:
#line 5923 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1350:
#line 5924 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\""));;
    break;}
case 1351:
#line 5925 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1352:
#line 5926 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1353:
#line 5927 "preproc.y"
{ yyval.str = make1_str(","); ;
    break;}
case 1354:
#line 5929 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1355:
#line 5930 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1356:
#line 5931 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1357:
#line 5932 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1358:
#line 5933 "preproc.y"
{ yyval.str = make3_str(make1_str("{"), yyvsp[-1].str, make1_str("}")); ;
    break;}
case 1359:
#line 5935 "preproc.y"
{
    braces_open++;
    yyval.str = make1_str("{");
;
    break;}
case 1360:
#line 5940 "preproc.y"
{
    remove_variables(braces_open--);
    yyval.str = make1_str("}");
;
    break;}
}
   /* the action file gets copied in in place of this dollarsign */
#line 498 "/usr/local/bison/bison.simple"

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
#line 5945 "preproc.y"


void yyerror(char * error)
{
    fprintf(stderr, "%s:%d: %s\n", input_filename, yylineno, error);
    exit(PARSE_ERROR);
}
