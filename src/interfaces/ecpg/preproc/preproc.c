
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



#define	YYFINAL		2399
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
  2124,  2128,  2131,  2134,  2138,  2145,  2149,  2153,  2157,  2162,
  2165,  2168,  2173,  2177,  2182,  2184,  2186,  2191,  2193,  2198,
  2200,  2202,  2207,  2212,  2217,  2222,  2228,  2234,  2240,  2245,
  2248,  2252,  2255,  2260,  2264,  2269,  2273,  2278,  2284,  2291,
  2297,  2304,  2310,  2316,  2322,  2328,  2334,  2340,  2346,  2352,
  2359,  2366,  2373,  2380,  2387,  2394,  2401,  2408,  2415,  2422,
  2429,  2436,  2443,  2450,  2457,  2464,  2468,  2472,  2475,  2477,
  2479,  2482,  2484,  2486,  2489,  2493,  2497,  2501,  2505,  2508,
  2511,  2515,  2522,  2526,  2530,  2533,  2536,  2540,  2545,  2547,
  2549,  2554,  2556,  2561,  2563,  2565,  2570,  2575,  2581,  2587,
  2593,  2598,  2600,  2605,  2612,  2613,  2615,  2619,  2623,  2627,
  2628,  2630,  2632,  2634,  2636,  2640,  2641,  2644,  2646,  2649,
  2653,  2657,  2661,  2665,  2668,  2672,  2679,  2683,  2687,  2690,
  2693,  2695,  2699,  2704,  2709,  2714,  2720,  2726,  2732,  2737,
  2741,  2742,  2745,  2746,  2749,  2750,  2754,  2757,  2759,  2761,
  2763,  2765,  2769,  2771,  2773,  2775,  2779,  2785,  2792,  2797,
  2800,  2802,  2807,  2810,  2811,  2814,  2816,  2817,  2821,  2825,
  2827,  2831,  2835,  2839,  2841,  2843,  2848,  2851,  2855,  2859,
  2861,  2865,  2867,  2871,  2873,  2875,  2876,  2878,  2880,  2882,
  2884,  2886,  2888,  2890,  2892,  2894,  2896,  2898,  2900,  2902,
  2905,  2907,  2909,  2911,  2914,  2916,  2918,  2920,  2922,  2924,
  2926,  2928,  2930,  2932,  2934,  2936,  2938,  2940,  2942,  2944,
  2946,  2948,  2950,  2952,  2954,  2956,  2958,  2960,  2962,  2964,
  2966,  2968,  2970,  2972,  2974,  2976,  2978,  2980,  2982,  2984,
  2986,  2988,  2990,  2992,  2994,  2996,  2998,  3000,  3002,  3004,
  3006,  3008,  3010,  3012,  3014,  3016,  3018,  3020,  3022,  3024,
  3026,  3028,  3030,  3032,  3034,  3036,  3038,  3040,  3042,  3044,
  3046,  3048,  3050,  3052,  3054,  3056,  3058,  3060,  3062,  3064,
  3066,  3068,  3070,  3072,  3074,  3076,  3078,  3080,  3082,  3084,
  3086,  3088,  3090,  3092,  3094,  3096,  3098,  3100,  3102,  3104,
  3106,  3108,  3110,  3112,  3114,  3116,  3118,  3120,  3122,  3124,
  3126,  3128,  3130,  3132,  3134,  3136,  3138,  3140,  3142,  3144,
  3146,  3148,  3150,  3152,  3154,  3156,  3158,  3160,  3162,  3164,
  3166,  3168,  3170,  3172,  3174,  3176,  3178,  3180,  3182,  3184,
  3186,  3188,  3190,  3192,  3194,  3196,  3198,  3200,  3202,  3204,
  3206,  3208,  3210,  3216,  3220,  3223,  3227,  3234,  3236,  3238,
  3241,  3244,  3246,  3247,  3249,  3253,  3256,  3257,  3260,  3261,
  3264,  3265,  3267,  3271,  3276,  3280,  3282,  3284,  3286,  3288,
  3291,  3292,  3300,  3304,  3305,  3310,  3316,  3322,  3323,  3326,
  3327,  3328,  3335,  3337,  3339,  3341,  3343,  3345,  3347,  3348,
  3350,  3352,  3354,  3356,  3358,  3360,  3365,  3368,  3373,  3378,
  3381,  3384,  3385,  3387,  3389,  3392,  3394,  3397,  3399,  3402,
  3404,  3406,  3408,  3410,  3413,  3415,  3417,  3421,  3426,  3427,
  3430,  3431,  3433,  3437,  3440,  3442,  3444,  3446,  3447,  3449,
  3451,  3455,  3456,  3461,  3463,  3465,  3468,  3472,  3473,  3476,
  3478,  3482,  3487,  3490,  3494,  3501,  3505,  3509,  3514,  3519,
  3520,  3524,  3528,  3533,  3538,  3539,  3541,  3542,  3544,  3546,
  3548,  3550,  3553,  3555,  3558,  3561,  3563,  3566,  3569,  3572,
  3573,  3579,  3580,  3586,  3588,  3590,  3591,  3592,  3595,  3596,
  3601,  3603,  3607,  3611,  3618,  3622,  3627,  3631,  3633,  3635,
  3637,  3640,  3644,  3650,  3653,  3659,  3662,  3664,  3666,  3668,
  3671,  3675,  3679,  3683,  3687,  3691,  3695,  3699,  3702,  3705,
  3709,  3716,  3720,  3724,  3728,  3733,  3736,  3739,  3744,  3748,
  3753,  3755,  3757,  3762,  3764,  3769,  3771,  3776,  3781,  3786,
  3791,  3797,  3803,  3809,  3814,  3817,  3821,  3824,  3829,  3833,
  3838,  3842,  3847,  3853,  3860,  3866,  3873,  3879,  3885,  3891,
  3897,  3903,  3909,  3915,  3921,  3928,  3935,  3942,  3949,  3956,
  3963,  3970,  3977,  3984,  3991,  3998,  4005,  4012,  4019,  4026,
  4033,  4037,  4041,  4044,  4046,  4048,  4052,  4054,  4055,  4058,
  4060,  4063,  4066,  4069,  4071,  4073,  4074,  4076,  4079,  4082,
  4084,  4086,  4088,  4090,  4092,  4095,  4097,  4099,  4101,  4103,
  4105,  4107,  4109,  4111,  4113,  4115,  4117,  4119,  4121,  4123,
  4125,  4127,  4129,  4131,  4133,  4135,  4137,  4139,  4141,  4143,
  4145,  4147,  4149,  4151,  4153,  4155,  4157,  4159,  4161,  4163,
  4165,  4167,  4169,  4171,  4173,  4175,  4177,  4181,  4183
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
   286,   531,     0,   531,   284,   531,     0,   293,   531,     0,
   291,   531,     0,   531,    59,   507,     0,    75,   299,   531,
    67,   507,   300,     0,   299,   525,   300,     0,   531,   276,
   531,     0,   531,   134,   531,     0,   531,   145,   134,   531,
     0,   276,   531,     0,   531,   276,     0,   566,   299,   289,
   300,     0,   566,   299,   300,     0,   566,   299,   534,   300,
     0,    88,     0,    89,     0,    89,   299,   570,   300,     0,
    90,     0,    90,   299,   570,   300,     0,    91,     0,   190,
     0,   106,   299,   471,   300,     0,   107,   299,   535,   300,
     0,   157,   299,   537,   300,     0,   174,   299,   539,   300,
     0,   185,   299,    71,   542,   300,     0,   185,   299,   131,
   542,   300,     0,   185,   299,   183,   542,   300,     0,   185,
   299,   542,   300,     0,   531,   231,     0,   531,   126,   147,
     0,   531,   248,     0,   531,   126,   145,   147,     0,   531,
   126,   186,     0,   531,   126,   145,   108,     0,   531,   126,
   108,     0,   531,   126,   145,   186,     0,   531,    70,   532,
    65,   532,     0,   531,   145,    70,   532,    65,   532,     0,
   531,   119,   299,   543,   300,     0,   531,   145,   119,   299,
   545,   300,     0,   531,   276,   299,   471,   300,     0,   531,
   287,   299,   471,   300,     0,   531,   288,   299,   471,   300,
     0,   531,   290,   299,   471,   300,     0,   531,   289,   299,
   471,   300,     0,   531,   285,   299,   471,   300,     0,   531,
   286,   299,   471,   300,     0,   531,   284,   299,   471,   300,
     0,   531,   276,    66,   299,   471,   300,     0,   531,   287,
    66,   299,   471,   300,     0,   531,   288,    66,   299,   471,
   300,     0,   531,   290,    66,   299,   471,   300,     0,   531,
   289,    66,   299,   471,   300,     0,   531,   285,    66,   299,
   471,   300,     0,   531,   286,    66,   299,   471,   300,     0,
   531,   284,    66,   299,   471,   300,     0,   531,   276,    63,
   299,   471,   300,     0,   531,   287,    63,   299,   471,   300,
     0,   531,   288,    63,   299,   471,   300,     0,   531,   290,
    63,   299,   471,   300,     0,   531,   289,    63,   299,   471,
   300,     0,   531,   285,    63,   299,   471,   300,     0,   531,
   286,    63,   299,   471,   300,     0,   531,   284,    63,   299,
   471,   300,     0,   531,    65,   531,     0,   531,   153,   531,
     0,   145,   531,     0,   547,     0,   652,     0,   552,   533,
     0,   568,     0,   575,     0,   288,   532,     0,   532,   287,
   532,     0,   532,   288,   532,     0,   532,   290,   532,     0,
   532,   289,   532,     0,   293,   532,     0,   291,   532,     0,
   532,    59,   507,     0,    75,   299,   532,    67,   507,   300,
     0,   299,   531,   300,     0,   532,   276,   532,     0,   276,
   532,     0,   532,   276,     0,   566,   299,   300,     0,   566,
   299,   534,   300,     0,    88,     0,    89,     0,    89,   299,
   570,   300,     0,    90,     0,    90,   299,   570,   300,     0,
    91,     0,   190,     0,   157,   299,   537,   300,     0,   174,
   299,   539,   300,     0,   185,   299,    71,   542,   300,     0,
   185,   299,   131,   542,   300,     0,   185,   299,   183,   542,
   300,     0,   185,   299,   542,   300,     0,   653,     0,   296,
   646,   297,   533,     0,   296,   646,   292,   646,   297,   533,
     0,     0,   525,     0,   534,   298,   525,     0,   534,   191,
   531,     0,   536,   113,   531,     0,     0,   652,     0,   522,
     0,   180,     0,   181,     0,   538,   119,   538,     0,     0,
   552,   533,     0,   568,     0,   288,   538,     0,   538,   287,
   538,     0,   538,   288,   538,     0,   538,   290,   538,     0,
   538,   289,   538,     0,   291,   538,     0,   538,    59,   507,
     0,    75,   299,   538,    67,   507,   300,     0,   299,   538,
   300,     0,   538,   276,   538,     0,   276,   538,     0,   538,
   276,     0,   575,     0,   566,   299,   300,     0,   566,   299,
   534,   300,     0,   157,   299,   537,   300,     0,   174,   299,
   539,   300,     0,   185,   299,    71,   542,   300,     0,   185,
   299,   131,   542,   300,     0,   185,   299,   183,   542,   300,
     0,   185,   299,   542,   300,     0,   534,   540,   541,     0,
     0,   113,   534,     0,     0,   111,   534,     0,     0,   531,
   113,   534,     0,   113,   534,     0,   534,     0,   471,     0,
   544,     0,   568,     0,   544,   298,   568,     0,   471,     0,
   546,     0,   568,     0,   546,   298,   568,     0,    74,   551,
   548,   550,   103,     0,   146,   299,   531,   298,   531,   300,
     0,    80,   299,   534,   300,     0,   548,   549,     0,   549,
     0,   196,   531,   177,   525,     0,   102,   525,     0,     0,
   552,   533,     0,   575,     0,     0,   559,   295,   553,     0,
   569,   295,   553,     0,   562,     0,   553,   295,   562,     0,
   553,   295,   289,     0,   554,   298,   555,     0,   555,     0,
   289,     0,   575,   533,   284,   525,     0,   552,   533,     0,
   559,   295,   289,     0,   556,   298,   557,     0,   557,     0,
   525,    67,   576,     0,   525,     0,   559,   295,   289,     0,
   289,     0,   575,     0,     0,   577,     0,   575,     0,   575,
     0,   656,     0,   575,     0,   656,     0,   575,     0,   575,
     0,   575,     0,   572,     0,   570,     0,   571,     0,   572,
     0,   507,   572,     0,   569,     0,   186,     0,   108,     0,
   281,   533,     0,   280,     0,   282,     0,   275,     0,   656,
     0,   575,     0,   512,     0,   517,     0,   656,     0,   522,
     0,    60,     0,    61,     0,   205,     0,   206,     0,   208,
     0,   209,     0,   211,     0,   214,     0,   215,     0,   216,
     0,   217,     0,   218,     0,   100,     0,   220,     0,   221,
     0,   224,     0,   225,     0,   226,     0,   227,     0,   228,
     0,   229,     0,   121,     0,   230,     0,   231,     0,   129,
     0,   130,     0,   232,     0,   237,     0,   136,     0,   239,
     0,   240,     0,   143,     0,   243,     0,   244,     0,   246,
     0,   248,     0,   149,     0,   250,     0,   151,     0,   251,
     0,   152,     0,   252,     0,   160,     0,   161,     0,   253,
     0,   164,     0,   166,     0,   255,     0,   257,     0,   258,
     0,   259,     0,   170,     0,   261,     0,   260,     0,   264,
     0,   265,     0,   266,     0,   267,     0,   178,     0,   179,
     0,   180,     0,   181,     0,   202,     0,   268,     0,   203,
     0,   271,     0,   273,     0,   201,     0,     3,     0,     4,
     0,     5,     0,     6,     0,     7,     0,     8,     0,     9,
     0,    10,     0,    11,     0,    13,     0,    15,     0,    16,
     0,    17,     0,    18,     0,    19,     0,    20,     0,    21,
     0,    22,     0,    23,     0,    24,     0,    26,     0,    28,
     0,    29,     0,    30,     0,    31,     0,    32,     0,    34,
     0,    35,     0,    36,     0,    37,     0,    38,     0,   575,
     0,   204,     0,   207,     0,   210,     0,    74,     0,   212,
     0,    80,     0,    84,     0,   213,     0,    87,     0,   219,
     0,   102,     0,   103,     0,   222,     0,   223,     0,   108,
     0,   112,     0,   116,     0,   234,     0,   236,     0,   238,
     0,   241,     0,   242,     0,   245,     0,   146,     0,   154,
     0,   157,     0,   158,     0,   256,     0,   262,     0,   263,
     0,   175,     0,   177,     0,   184,     0,   186,     0,   270,
     0,   272,     0,   196,     0,    87,     0,   242,     0,     7,
   182,   579,   585,   586,     0,     7,   182,    96,     0,     7,
   587,     0,   560,   582,   584,     0,   580,   581,   584,   290,
   560,   590,     0,   589,     0,   572,     0,   656,   654,     0,
   276,   583,     0,   581,     0,     0,   575,     0,   575,   295,
   583,     0,   292,   570,     0,     0,    67,   579,     0,     0,
   190,   587,     0,     0,   588,     0,   588,   290,   575,     0,
   588,    17,    72,   588,     0,   588,   191,   588,     0,   573,
     0,   589,     0,   275,     0,   654,     0,   276,   575,     0,
     0,    95,   565,   465,    92,   111,   656,   466,     0,    10,
    23,   656,     0,     0,   595,   594,   597,   596,     0,   648,
    69,    95,    26,    27,     0,   648,   103,    95,    26,    27,
     0,     0,   598,   597,     0,     0,     0,   601,   599,   602,
   600,   612,   293,     0,    46,     0,    54,     0,    53,     0,
    43,     0,    51,     0,    40,     0,     0,   610,     0,   611,
     0,   605,     0,   606,     0,   603,     0,   657,     0,   604,
   301,   659,   302,     0,    45,   609,     0,   607,   301,   597,
   302,     0,   608,   301,   597,   302,     0,    55,   609,     0,
    56,   609,     0,     0,   657,     0,    52,     0,    57,    52,
     0,    48,     0,    57,    48,     0,    50,     0,    57,    50,
     0,    47,     0,    44,     0,    41,     0,    42,     0,    57,
    42,     0,    58,     0,   613,     0,   612,   298,   613,     0,
   615,   657,   505,   614,     0,     0,   284,   650,     0,     0,
   289,     0,    95,   265,   656,     0,    11,   618,     0,   619,
     0,    87,     0,    63,     0,     0,   579,     0,    96,     0,
   105,    18,   622,     0,     0,   105,   656,   621,   625,     0,
   589,     0,   277,     0,    14,   656,     0,    22,   565,   625,
     0,     0,   191,   626,     0,   652,     0,   652,   298,   626,
     0,    23,   656,   113,   589,     0,   437,    24,     0,   173,
     8,   619,     0,   203,   657,   126,   634,   631,   633,     0,
   296,   297,   632,     0,   299,   300,   632,     0,   296,   570,
   297,   632,     0,   299,   570,   300,   632,     0,     0,   296,
   297,   632,     0,   299,   300,   632,     0,   296,   570,   297,
   632,     0,   299,   570,   300,   632,     0,     0,    25,     0,
     0,    76,     0,   193,     0,   110,     0,   100,     0,   637,
    20,     0,    12,     0,   637,    28,     0,   637,    21,     0,
     4,     0,    36,    20,     0,    36,    28,     0,    36,    21,
     0,     0,    35,   635,   301,   638,   302,     0,     0,   187,
   636,   301,   638,   302,     0,   657,     0,    29,     0,     0,
     0,   639,   638,     0,     0,   634,   640,   641,    27,     0,
   642,     0,   641,   298,   642,     0,   615,   657,   505,     0,
    37,   657,   126,   634,   631,   633,     0,    38,    30,   645,
     0,    38,   145,    13,   645,     0,    38,    32,   645,     0,
     9,     0,    31,     0,    34,     0,    16,   565,     0,    15,
   182,   565,     0,   219,   565,   299,   649,   300,     0,   219,
     5,     0,     6,   565,   299,   649,   300,     0,   552,   533,
     0,   526,     0,   568,     0,   575,     0,   288,   646,     0,
   531,   287,   646,     0,   531,   288,   646,     0,   531,   290,
   646,     0,   531,   289,   646,     0,   531,   285,   646,     0,
   531,   286,   646,     0,   531,   284,   646,     0,   293,   646,
     0,   291,   646,     0,   531,    59,   507,     0,    75,   299,
   531,    67,   507,   300,     0,   299,   525,   300,     0,   531,
   276,   646,     0,   531,   134,   646,     0,   531,   145,   134,
   646,     0,   276,   646,     0,   531,   276,     0,   566,   299,
   289,   300,     0,   566,   299,   300,     0,   566,   299,   534,
   300,     0,    88,     0,    89,     0,    89,   299,   570,   300,
     0,    90,     0,    90,   299,   570,   300,     0,    91,     0,
   106,   299,   471,   300,     0,   107,   299,   535,   300,     0,
   157,   299,   537,   300,     0,   174,   299,   539,   300,     0,
   185,   299,    71,   542,   300,     0,   185,   299,   131,   542,
   300,     0,   185,   299,   183,   542,   300,     0,   185,   299,
   542,   300,     0,   531,   231,     0,   531,   126,   147,     0,
   531,   248,     0,   531,   126,   145,   147,     0,   531,   126,
   186,     0,   531,   126,   145,   108,     0,   531,   126,   108,
     0,   531,   126,   145,   186,     0,   531,    70,   532,    65,
   532,     0,   531,   145,    70,   532,    65,   532,     0,   531,
   119,   299,   543,   300,     0,   531,   145,   119,   299,   545,
   300,     0,   531,   276,   299,   471,   300,     0,   531,   287,
   299,   471,   300,     0,   531,   288,   299,   471,   300,     0,
   531,   290,   299,   471,   300,     0,   531,   289,   299,   471,
   300,     0,   531,   285,   299,   471,   300,     0,   531,   286,
   299,   471,   300,     0,   531,   284,   299,   471,   300,     0,
   531,   276,    66,   299,   471,   300,     0,   531,   287,    66,
   299,   471,   300,     0,   531,   288,    66,   299,   471,   300,
     0,   531,   290,    66,   299,   471,   300,     0,   531,   289,
    66,   299,   471,   300,     0,   531,   285,    66,   299,   471,
   300,     0,   531,   286,    66,   299,   471,   300,     0,   531,
   284,    66,   299,   471,   300,     0,   531,   276,    63,   299,
   471,   300,     0,   531,   287,    63,   299,   471,   300,     0,
   531,   288,    63,   299,   471,   300,     0,   531,   290,    63,
   299,   471,   300,     0,   531,   289,    63,   299,   471,   300,
     0,   531,   285,    63,   299,   471,   300,     0,   531,   286,
    63,   299,   471,   300,     0,   531,   284,    63,   299,   471,
   300,     0,   531,    65,   646,     0,   531,   153,   646,     0,
   145,   646,     0,   653,     0,   651,     0,   647,   298,   651,
     0,    33,     0,     0,   649,   662,     0,   663,     0,   650,
   663,     0,   654,   655,     0,   654,   655,     0,   654,     0,
   278,     0,     0,   654,     0,    19,   654,     0,    19,   565,
     0,   274,     0,   277,     0,   274,     0,   279,     0,   661,
     0,   659,   661,     0,   661,     0,   293,     0,   274,     0,
   277,     0,   570,     0,   571,     0,   289,     0,    40,     0,
    41,     0,    42,     0,    43,     0,    44,     0,    45,     0,
    46,     0,    47,     0,    48,     0,    50,     0,    51,     0,
    52,     0,    53,     0,    54,     0,    55,     0,    56,     0,
    57,     0,    58,     0,    39,     0,   296,     0,   297,     0,
   299,     0,   300,     0,   284,     0,   298,     0,   274,     0,
   277,     0,   570,     0,   571,     0,   298,     0,   274,     0,
   277,     0,   570,     0,   571,     0,   301,   659,   302,     0,
   301,     0,   302,     0
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
  3580,  3585,  3587,  3589,  3593,  3597,  3599,  3601,  3603,  3605,
  3607,  3609,  3613,  3617,  3621,  3625,  3629,  3635,  3639,  3645,
  3649,  3654,  3658,  3662,  3666,  3671,  3675,  3679,  3683,  3687,
  3689,  3691,  3693,  3700,  3704,  3708,  3712,  3716,  3720,  3724,
  3728,  3732,  3736,  3740,  3744,  3748,  3752,  3756,  3760,  3764,
  3768,  3772,  3776,  3780,  3784,  3788,  3792,  3796,  3800,  3804,
  3808,  3812,  3816,  3820,  3824,  3828,  3830,  3832,  3834,  3836,
  3845,  3849,  3851,  3855,  3857,  3859,  3861,  3863,  3868,  3870,
  3872,  3876,  3880,  3882,  3884,  3886,  3888,  3892,  3896,  3900,
  3904,  3910,  3914,  3920,  3924,  3928,  3932,  3937,  3941,  3945,
  3949,  3953,  3957,  3961,  3965,  3969,  3971,  3973,  3977,  3981,
  3983,  3987,  3988,  3989,  3992,  3994,  3998,  4002,  4004,  4006,
  4008,  4010,  4012,  4014,  4016,  4020,  4024,  4026,  4028,  4030,
  4032,  4036,  4040,  4044,  4048,  4053,  4057,  4061,  4065,  4071,
  4075,  4079,  4081,  4087,  4089,  4093,  4095,  4097,  4101,  4105,
  4109,  4111,  4115,  4119,  4123,  4125,  4144,  4146,  4152,  4158,
  4160,  4164,  4170,  4171,  4174,  4178,  4182,  4186,  4190,  4196,
  4198,  4200,  4211,  4213,  4215,  4218,  4222,  4226,  4237,  4239,
  4244,  4248,  4252,  4256,  4262,  4263,  4266,  4270,  4283,  4284,
  4285,  4286,  4287,  4293,  4294,  4296,  4302,  4306,  4310,  4314,
  4318,  4320,  4324,  4330,  4336,  4337,  4338,  4346,  4353,  4355,
  4357,  4368,  4369,  4370,  4371,  4372,  4373,  4374,  4375,  4376,
  4377,  4378,  4379,  4380,  4381,  4382,  4383,  4384,  4385,  4386,
  4387,  4388,  4389,  4390,  4391,  4392,  4393,  4394,  4395,  4396,
  4397,  4398,  4399,  4400,  4401,  4402,  4403,  4404,  4405,  4406,
  4407,  4408,  4409,  4410,  4411,  4412,  4413,  4414,  4415,  4417,
  4418,  4419,  4420,  4421,  4422,  4423,  4424,  4425,  4426,  4427,
  4428,  4429,  4430,  4431,  4432,  4433,  4434,  4435,  4436,  4437,
  4438,  4439,  4440,  4441,  4442,  4443,  4444,  4445,  4446,  4447,
  4448,  4449,  4450,  4451,  4452,  4453,  4454,  4455,  4456,  4457,
  4458,  4459,  4460,  4461,  4462,  4463,  4464,  4465,  4466,  4467,
  4468,  4469,  4481,  4482,  4483,  4484,  4485,  4486,  4487,  4488,
  4489,  4490,  4491,  4492,  4493,  4494,  4495,  4496,  4497,  4498,
  4499,  4500,  4501,  4502,  4503,  4504,  4505,  4506,  4507,  4508,
  4509,  4510,  4511,  4512,  4513,  4514,  4515,  4516,  4517,  4518,
  4521,  4528,  4544,  4548,  4553,  4558,  4569,  4592,  4596,  4604,
  4621,  4632,  4633,  4635,  4636,  4638,  4639,  4641,  4642,  4644,
  4645,  4647,  4651,  4655,  4659,  4664,  4669,  4670,  4672,  4696,
  4709,  4715,  4758,  4763,  4768,  4775,  4777,  4779,  4783,  4788,
  4793,  4798,  4803,  4804,  4805,  4806,  4807,  4808,  4809,  4811,
  4818,  4825,  4832,  4839,  4847,  4859,  4864,  4866,  4873,  4880,
  4888,  4896,  4897,  4899,  4900,  4901,  4902,  4903,  4904,  4905,
  4906,  4907,  4908,  4909,  4911,  4913,  4917,  4922,  4997,  4998,
  5000,  5001,  5007,  5015,  5017,  5018,  5019,  5020,  5022,  5023,
  5028,  5041,  5053,  5057,  5057,  5064,  5069,  5073,  5074,  5079,
  5079,  5085,  5095,  5111,  5119,  5161,  5167,  5173,  5179,  5185,
  5193,  5199,  5205,  5211,  5217,  5224,  5225,  5227,  5234,  5241,
  5248,  5255,  5262,  5269,  5276,  5283,  5290,  5297,  5304,  5311,
  5316,  5324,  5329,  5337,  5348,  5348,  5350,  5354,  5360,  5366,
  5371,  5375,  5380,  5451,  5506,  5511,  5516,  5522,  5527,  5532,
  5537,  5542,  5547,  5552,  5557,  5564,  5568,  5570,  5572,  5576,
  5578,  5580,  5582,  5584,  5586,  5588,  5590,  5594,  5596,  5598,
  5602,  5606,  5608,  5610,  5612,  5614,  5616,  5618,  5622,  5626,
  5630,  5634,  5638,  5644,  5648,  5654,  5658,  5662,  5666,  5670,
  5675,  5679,  5683,  5687,  5691,  5693,  5695,  5697,  5704,  5708,
  5712,  5716,  5720,  5724,  5728,  5732,  5736,  5740,  5744,  5748,
  5752,  5756,  5760,  5764,  5768,  5772,  5776,  5780,  5784,  5788,
  5792,  5796,  5800,  5804,  5808,  5812,  5816,  5820,  5824,  5828,
  5832,  5834,  5836,  5838,  5842,  5842,  5844,  5846,  5847,  5849,
  5850,  5852,  5856,  5860,  5865,  5867,  5868,  5869,  5870,  5872,
  5873,  5878,  5880,  5882,  5883,  5888,  5888,  5890,  5891,  5892,
  5893,  5894,  5895,  5896,  5897,  5898,  5899,  5900,  5901,  5902,
  5903,  5904,  5905,  5906,  5907,  5908,  5909,  5910,  5911,  5912,
  5913,  5914,  5915,  5916,  5917,  5918,  5919,  5921,  5922,  5923,
  5924,  5925,  5927,  5928,  5929,  5930,  5931,  5933,  5938
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
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   533,   533,   533,   534,   534,   534,   535,   535,
   535,   536,   536,   536,   537,   537,   538,   538,   538,   538,
   538,   538,   538,   538,   538,   538,   538,   538,   538,   538,
   538,   538,   538,   538,   538,   538,   538,   538,   538,   539,
   539,   540,   540,   541,   541,   542,   542,   542,   543,   543,
   544,   544,   545,   545,   546,   546,   547,   547,   547,   548,
   548,   549,   550,   550,   551,   551,   551,   552,   552,   553,
   553,   553,   554,   554,   554,   555,   555,   555,   556,   556,
   557,   557,   557,   557,   558,   558,   559,   559,   560,   561,
   562,   563,   564,   565,   566,   567,   568,   568,   568,   568,
   568,   568,   568,   569,   570,   571,   572,   573,   574,   574,
   574,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   575,   575,   575,   575,   575,   575,   575,   575,
   575,   575,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   577,   577,   578,   578,   578,   579,   579,   579,   579,   580,
   581,   582,   582,   583,   583,   584,   584,   585,   585,   586,
   586,   587,   587,   587,   587,   588,   588,   588,   589,   590,
   590,   591,   592,   594,   593,   595,   596,   597,   597,   599,
   600,   598,   601,   601,   601,   601,   601,   601,   601,   602,
   602,   602,   602,   602,   602,   603,   604,   605,   606,   607,
   608,   609,   609,   610,   610,   610,   610,   610,   610,   610,
   610,   610,   610,   610,   611,   612,   612,   613,   614,   614,
   615,   615,   616,   617,   618,   618,   618,   618,   619,   619,
   620,   621,   620,   622,   622,   623,   624,   625,   625,   626,
   626,   627,   628,   629,   630,   631,   631,   631,   631,   631,
   632,   632,   632,   632,   632,   633,   633,   634,   634,   634,
   634,   634,   634,   634,   634,   634,   634,   634,   634,   635,
   634,   636,   634,   634,   637,   637,   638,   638,   640,   639,
   641,   641,   642,   643,   644,   644,   644,   645,   645,   645,
   645,   645,   645,   645,   645,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   646,   646,   646,   646,   646,   646,
   646,   646,   646,   646,   647,   647,   648,   649,   649,   650,
   650,   651,   652,   653,   654,   655,   655,   655,   655,   656,
   656,   657,   658,   659,   659,   660,   660,   661,   661,   661,
   661,   661,   661,   661,   661,   661,   661,   661,   661,   661,
   661,   661,   661,   661,   661,   661,   661,   661,   661,   661,
   661,   661,   661,   661,   661,   661,   661,   662,   662,   662,
   662,   662,   663,   663,   663,   663,   663,   664,   665
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
     3,     2,     2,     3,     6,     3,     3,     3,     4,     2,
     2,     4,     3,     4,     1,     1,     4,     1,     4,     1,
     1,     4,     4,     4,     4,     5,     5,     5,     4,     2,
     3,     2,     4,     3,     4,     3,     4,     5,     6,     5,
     6,     5,     5,     5,     5,     5,     5,     5,     5,     6,
     6,     6,     6,     6,     6,     6,     6,     6,     6,     6,
     6,     6,     6,     6,     6,     3,     3,     2,     1,     1,
     2,     1,     1,     2,     3,     3,     3,     3,     2,     2,
     3,     6,     3,     3,     2,     2,     3,     4,     1,     1,
     4,     1,     4,     1,     1,     4,     4,     5,     5,     5,
     4,     1,     4,     6,     0,     1,     3,     3,     3,     0,
     1,     1,     1,     1,     3,     0,     2,     1,     2,     3,
     3,     3,     3,     2,     3,     6,     3,     3,     2,     2,
     1,     3,     4,     4,     4,     5,     5,     5,     4,     3,
     0,     2,     0,     2,     0,     3,     2,     1,     1,     1,
     1,     3,     1,     1,     1,     3,     5,     6,     4,     2,
     1,     4,     2,     0,     2,     1,     0,     3,     3,     1,
     3,     3,     3,     1,     1,     4,     2,     3,     3,     1,
     3,     1,     3,     1,     1,     0,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     2,
     1,     1,     1,     2,     1,     1,     1,     1,     1,     1,
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
  1340,  1318,  1319,  1313,   915,   916,  1346,  1322,  1317,  1342,
  1343,  1347,  1344,  1345,  1358,  1359,     3,  1320,  1321,     6,
  1094,     0,     8,     7,  1316,     9,    10,  1109,     0,     0,
     0,  1148,     0,     0,     0,     0,     0,     0,   432,   896,
   432,   124,     0,     0,     0,   432,     0,   314,     0,     0,
     0,   432,   513,     0,     0,     0,   432,     0,   117,   452,
     0,     0,     0,     0,   507,   314,     0,     0,     0,   452,
     0,     0,     0,    21,    13,    27,    51,    52,    53,    12,
    14,    15,    16,    17,    18,    19,    25,    20,    26,    23,
    24,    30,    31,    42,    32,    28,    36,    40,    37,    39,
    38,    41,    44,   464,    33,    34,    45,    46,    47,    48,
    49,    22,    50,    29,    43,   463,   465,    35,   462,   461,
   460,   515,   498,    54,    55,    56,    57,    58,    59,    60,
    61,    62,    63,    64,    65,    66,    67,  1108,  1106,  1103,
  1107,  1105,  1104,     0,  1109,  1100,   992,   993,   994,   995,
   996,   997,   998,   999,  1000,  1001,  1002,  1003,  1004,  1005,
  1006,  1007,  1008,  1009,  1010,  1011,  1012,  1013,  1014,  1015,
  1016,  1017,  1018,  1019,  1020,  1021,  1022,   924,   925,   665,
   936,   666,   945,   948,   949,   952,   667,   664,   955,   960,
   962,   964,   966,   967,   969,   970,   975,   668,   982,   983,
   984,   985,   663,   991,   986,   988,   926,   927,   928,   929,
   930,   931,   932,   933,   934,   935,   937,   938,   939,   940,
   941,   942,   943,   944,   946,   947,   950,   951,   953,   954,
   956,   957,   958,   959,   961,   963,   965,   968,   971,   972,
   973,   974,   977,   976,   978,   979,   980,   981,   987,   989,
   990,  1310,   917,  1311,  1305,   923,  1073,  1069,   899,    11,
     0,  1068,  1089,   922,     0,  1088,  1086,  1065,  1082,  1087,
   918,     0,  1147,  1146,  1150,  1149,  1144,  1145,  1156,  1158,
   904,   922,     0,  1312,     0,     0,     0,     0,     0,     0,
     0,   431,   430,   426,   109,   895,   427,   123,   342,     0,
     0,     0,   288,   289,     0,     0,   287,     0,     0,   260,
     0,     0,     0,     0,   979,   490,     0,     0,     0,   373,
     0,   370,     0,     0,     0,   371,     0,     0,   372,     0,
     0,   428,     0,  1152,   313,   312,   311,   310,   320,   326,
   333,   331,   330,   332,   334,     0,   327,   328,     0,     0,
   429,   512,   510,     0,   997,   446,   982,     0,     0,  1061,
  1062,     0,   898,   897,     0,   425,     0,   903,   116,     0,
   451,     0,     0,   422,   424,   423,   434,   906,   506,     0,
   320,   421,   982,     0,    99,   982,     0,    96,   454,     0,
   432,     0,     5,  1163,     0,   509,     0,   509,   546,  1095,
     0,  1099,     0,     0,  1072,  1077,  1077,  1070,  1064,  1079,
     0,     0,     0,  1093,     0,  1157,     0,  1196,     0,  1208,
     0,     0,  1209,  1210,     0,  1205,  1207,     0,   537,    72,
     0,    72,     0,     0,   436,     0,   905,     0,   246,     0,
     0,   291,   290,   294,   387,   385,   386,   381,   382,   383,
   384,   285,     0,   293,   292,     0,  1143,   487,   488,   486,
     0,   581,   306,   538,   539,    70,     0,     0,   447,     0,
   379,     0,   380,     0,   307,   369,  1155,  1154,  1151,  1158,
   317,   318,   319,     0,   323,   315,   325,     0,     0,     0,
     0,     0,   992,   993,   994,   995,   996,   997,   998,   999,
  1000,  1001,  1002,  1003,  1004,  1005,  1006,  1007,  1008,  1009,
  1010,  1011,  1012,  1013,  1014,  1015,  1016,  1017,  1018,  1019,
  1020,  1021,  1022,   877,     0,   654,   654,     0,   725,   726,
   728,   730,   645,   936,     0,     0,   913,   639,   679,     0,
   654,     0,     0,   681,   642,     0,     0,   982,   983,     0,
   912,   731,   650,   988,     0,     0,   815,     0,   894,     0,
     0,     0,     0,   586,   593,   596,   595,   591,   647,   594,
   923,   892,   701,   680,   779,   815,   505,   890,     0,     0,
   702,   911,   907,   908,   909,   703,   780,  1306,   922,  1164,
   445,    90,   444,     0,     0,     0,     0,     0,  1196,     0,
   119,     0,   459,   581,   478,   323,   100,     0,    97,     0,
   453,   449,   497,     4,   499,   508,     0,     0,     0,     0,
   530,     0,  1132,  1133,  1131,  1122,  1130,  1126,  1128,  1124,
  1122,  1122,     0,  1135,  1101,  1114,     0,  1112,  1113,     0,
     0,  1110,  1111,  1115,  1074,  1071,     0,  1066,     0,     0,
  1081,     0,  1085,  1083,  1159,  1160,  1162,  1186,  1183,  1195,
  1190,     0,  1178,  1181,  1180,  1192,  1179,  1170,     0,  1194,
     0,     0,  1211,   994,     0,  1206,   536,     0,     0,    75,
  1096,    75,     0,   265,   264,     0,   438,     0,     0,   397,
   244,   240,     0,     0,   286,     0,   489,     0,     0,   477,
     0,     0,   376,   374,   375,   377,     0,   262,  1153,   316,
     0,     0,     0,     0,   329,     0,     0,     0,   466,   469,
     0,   511,     0,   815,     0,     0,   876,     0,   653,   649,
   656,     0,     0,     0,     0,   632,   631,     0,   820,     0,
   630,   665,   666,   667,   663,   671,   662,   654,   652,   778,
     0,     0,   633,   826,   851,     0,   660,     0,   599,   600,
   601,   602,   603,   604,   605,   606,   607,   608,   609,   610,
   611,   612,   613,   614,   615,   616,   617,   618,   619,   620,
   621,   622,   623,   624,   625,   626,   627,   628,   629,     0,
   661,   670,   598,   592,   659,   597,   720,     0,   914,   704,
   713,   712,     0,     0,     0,   680,   910,     0,   590,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   740,
   742,   721,     0,     0,     0,     0,     0,     0,     0,   700,
   124,     0,   550,     0,     0,     0,     0,  1307,  1303,    94,
    95,    87,    93,     0,    92,    85,    91,    86,   885,   815,
   550,   884,     0,   815,  1170,   448,     0,     0,   490,   358,
   483,   309,   101,    98,   456,   501,   514,   516,   524,   500,
   548,     0,     0,   496,     0,  1117,  1123,  1120,  1121,  1134,
  1127,  1129,  1125,  1141,     0,  1109,  1109,     0,  1076,     0,
  1078,     0,  1063,  1084,     0,     0,  1187,  1189,  1188,     0,
     0,     0,  1177,  1182,  1185,  1184,  1298,  1212,  1298,   396,
   396,   396,   396,   102,     0,    73,    74,    78,    78,   433,
   270,   269,   271,     0,   266,     0,   440,   636,   936,   634,
   637,   363,     0,   920,   921,   364,   919,   368,     0,     0,
   248,     0,     0,     0,     0,   245,   127,     0,     0,     0,
   299,     0,   296,     0,     0,   580,   540,   284,     0,     0,
   388,   322,   321,     0,     0,   468,     0,     0,   475,   815,
     0,     0,   874,   871,   875,     0,     0,     0,   658,   816,
     0,     0,     0,     0,     0,   823,   824,   822,     0,     0,
   821,     0,     0,     0,     0,     0,   651,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   815,
     0,   828,   841,   853,     0,     0,     0,     0,     0,     0,
   680,   858,     0,     0,   725,   726,   728,   730,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   701,
     0,   815,     0,   702,   703,     0,  1294,  1306,   716,     0,
     0,   589,     0,     0,  1027,  1029,  1030,  1032,  1034,  1035,
  1038,  1039,  1040,  1047,  1048,  1049,  1050,  1054,  1055,  1056,
  1057,  1060,  1024,  1025,  1026,  1028,  1031,  1033,  1036,  1037,
  1041,  1042,  1043,  1044,  1045,  1046,  1051,  1052,  1053,  1058,
  1059,  1023,   891,   714,   776,     0,   799,   800,   802,   804,
     0,     0,     0,   805,     0,     0,     0,     0,     0,     0,
   815,     0,   782,   783,   812,  1304,     0,   746,     0,   741,
   744,   718,     0,     0,     0,   777,     0,     0,     0,   717,
     0,     0,     0,   711,     0,     0,     0,   709,     0,     0,
     0,   710,     0,     0,     0,   705,     0,     0,     0,   706,
     0,     0,     0,   708,     0,     0,     0,   707,   507,   504,
  1295,  1306,   889,     0,   581,   893,   878,   880,   901,     0,
   723,     0,   879,  1309,  1308,   969,    89,   887,     0,   581,
     0,     0,  1177,   118,   112,   111,     0,     0,   482,     0,
     0,   450,     0,   522,   523,     0,   518,     0,   545,   532,
   533,   527,   531,   535,   529,   534,     0,  1142,     0,  1136,
     0,     0,  1314,     0,     0,  1075,  1091,  1080,  1161,  1196,
  1196,  1175,     0,  1175,     0,  1176,  1204,     0,     0,     0,
   395,     0,     0,     0,   127,   108,     0,     0,     0,   394,
    71,    76,    77,    82,    82,     0,     0,   443,     0,   435,
   635,     0,   362,   367,   361,     0,     0,     0,   247,   257,
   249,   250,   251,   252,     0,     0,   126,   128,   129,   176,
     0,   242,   243,     0,     0,     0,     0,     0,   295,   345,
   492,   492,     0,   378,     0,   308,     0,   335,   339,   337,
     0,     0,     0,   476,   340,     0,     0,   870,     0,     0,
     0,     0,   648,     0,     0,   869,   727,   729,     0,   644,
   732,   733,     0,   638,   673,   674,   675,   676,   678,   677,
   672,     0,     0,   641,     0,   826,   851,     0,   839,   829,
   834,     0,   734,     0,     0,   840,     0,     0,     0,     0,
   827,     0,     0,   855,   735,   669,     0,   857,     0,     0,
     0,   739,     0,     0,     0,     0,   820,   778,  1293,   826,
   851,     0,   720,  1236,   704,  1220,   713,  1229,   712,  1228,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   740,
   742,   721,     0,     0,     0,     0,     0,     0,     0,   700,
     0,     0,   815,     0,     0,   688,   690,   689,   691,   692,
   693,   694,   695,     0,   687,     0,   584,   589,   646,     0,
     0,     0,   826,   851,     0,   795,   784,   790,   789,     0,
     0,     0,   796,     0,     0,     0,     0,   781,     0,   859,
     0,   860,   861,   911,   745,   743,   747,     0,     0,   719,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1302,     0,   549,   553,
   555,   552,   558,   582,   542,     0,   722,   724,    88,   883,
   484,   888,     0,  1165,   114,   115,   121,   113,     0,   481,
     0,     0,   457,   517,   519,   520,   521,   547,     0,     0,
     0,  1097,  1102,  1141,   586,  1116,  1315,  1118,  1119,     0,
  1067,  1199,     0,  1196,     0,     0,     0,  1166,  1175,  1167,
  1175,  1348,  1349,  1352,  1215,  1350,  1351,  1299,  1213,     0,
     0,     0,     0,     0,     0,   103,     0,   105,     0,   393,
     0,    84,    84,     0,   267,   442,   437,   441,   446,   365,
     0,     0,   366,   417,   418,   415,   416,     0,   258,     0,
     0,   237,     0,   239,   137,   133,   238,     0,     0,   382,
   303,   253,   254,   300,   302,   255,   304,   301,   298,   297,
     0,     0,     0,   485,  1092,   390,   391,   389,   336,     0,
   324,   467,   474,     0,   471,     0,   873,   867,     0,   655,
   657,   818,   817,     0,   819,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   837,   835,   825,   838,   830,   831,
   833,   832,   842,     0,   852,     0,   850,   736,   737,   738,
   856,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   716,   714,   776,  1291,     0,     0,   746,     0,
   741,   744,   718,  1234,     0,     0,     0,   777,  1292,     0,
     0,     0,   717,  1233,     0,     0,     0,   711,  1227,     0,
     0,     0,   709,  1225,     0,     0,     0,   710,  1226,     0,
     0,     0,   705,  1221,     0,     0,     0,   706,  1222,     0,
     0,     0,   708,  1224,     0,     0,     0,   707,  1223,     0,
   723,     0,     0,   813,     0,     0,   697,   696,     0,     0,
   589,     0,   585,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   793,   791,   748,   794,   785,   786,   788,   787,
   797,     0,   750,     0,     0,   863,     0,   864,   865,     0,
     0,   752,     0,     0,   759,     0,     0,   757,     0,     0,
   758,     0,     0,   753,     0,     0,   754,     0,     0,   756,
     0,     0,   755,   503,  1296,   572,     0,   559,     0,     0,
   574,   571,   574,   572,   570,   574,   561,   563,     0,     0,
   557,   583,     0,   544,   882,   881,   886,     0,   110,     0,
   480,     0,     0,   455,   526,   525,   528,  1137,  1139,  1090,
  1141,  1191,  1198,  1193,  1175,     0,  1175,     0,  1168,  1169,
     0,     0,   184,     0,     0,     0,     0,     0,     0,     0,
   183,   185,     0,     0,     0,   104,     0,     0,     0,     0,
     0,    69,    68,   274,     0,     0,   439,   360,     0,     0,
   175,   125,     0,   122,   241,   243,     0,   131,     0,     0,
     0,     0,     0,     0,   144,   130,   132,   135,   139,     0,
   305,   256,   344,   900,     0,     0,     0,   491,     0,     0,
   872,   715,   643,   868,   640,     0,   844,   845,     0,     0,
     0,   849,   843,   854,     0,   727,   729,   732,   733,   734,
   735,     0,     0,     0,   739,     0,     0,   745,   743,   747,
     0,     0,   719,  1235,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   722,   724,
   815,     0,     0,     0,     0,   699,     0,   587,   589,     0,
   801,   803,   806,   807,     0,     0,     0,   811,   798,   862,
   749,   751,     0,   768,   760,   775,   767,   773,   765,   774,
   766,   769,   761,   770,   762,   772,   764,   771,   763,     0,
   551,   554,     0,   573,   567,   568,     0,   569,   562,     0,
   556,     0,     0,   502,     0,   479,   458,     0,  1138,     0,
     0,  1201,  1171,  1175,  1172,  1175,     0,   206,   207,   186,
   195,   194,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   209,   211,   208,     0,     0,     0,     0,     0,     0,
     0,   177,     0,     0,     0,   178,   107,     0,   392,    81,
    80,     0,   273,     0,     0,   268,     0,   581,   414,     0,
   136,     0,     0,     0,   168,   169,   171,   173,   141,   174,
     0,     0,     0,     0,     0,   142,     0,   149,   143,   145,
   473,   134,   259,     0,   346,   347,   349,   354,     0,   901,
   493,     0,   494,   338,     0,     0,   846,   847,   848,     0,
   736,   737,   738,   748,   750,     0,     0,     0,     0,   752,
     0,     0,   759,     0,     0,   757,     0,     0,   758,     0,
     0,   753,     0,     0,   754,     0,     0,   756,     0,     0,
   755,   814,   682,     0,   685,   686,     0,   588,     0,   808,
   809,   810,   866,     0,   566,     0,     0,   541,   543,   120,
  1353,  1354,     0,  1355,  1356,  1140,  1300,   586,  1200,  1141,
  1173,  1174,     0,   198,   196,   204,     0,   223,     0,   214,
     0,   210,   213,   202,     0,     0,     0,   205,   201,   191,
   192,   193,   187,   188,   190,   189,   199,     0,   182,     0,
   179,   106,     0,    83,   275,   276,   272,     0,     0,     0,
     0,     0,     0,   138,     0,     0,     0,   166,   150,   159,
   158,     0,     0,   167,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   226,   361,     0,     0,     0,   357,     0,
   495,   470,   836,   715,   749,   751,   768,   760,   775,   767,
   773,   765,   774,   766,   769,   761,   770,   762,   772,   764,
   771,   763,   683,   684,   792,   560,   565,     0,     0,   564,
     0,  1301,  1203,  1202,     0,     0,     0,   222,   216,   212,
   215,     0,     0,   203,     0,   200,     0,    79,     0,   359,
   420,   413,   236,   140,     0,     0,     0,   162,   160,   165,
   155,   156,   157,   151,   152,   154,   153,   163,     0,   148,
     0,     0,   229,   341,   348,   353,   352,     0,   351,   355,
   902,     0,   576,     0,  1357,     0,   219,     0,   217,     0,
     0,   181,   473,   279,   419,     0,     0,   170,   172,     0,
   164,   472,   224,   225,     0,   146,   228,   356,   354,     0,
   578,   579,   197,   221,   220,   218,   226,     0,   277,   280,
   281,   282,   283,   399,     0,     0,   398,   401,   412,   409,
   411,   410,   400,     0,   147,     0,     0,   227,   357,     0,
   575,   229,     0,   263,     0,   404,   405,     0,   161,   233,
     0,     0,   230,   231,   350,   577,   180,   278,   402,   406,
   408,   403,   232,   234,   235,   407,     0,     0,     0
};

static const short yydefgoto[] = {  2397,
     1,    37,    92,    93,    94,    95,    96,   700,   938,  1264,
  2050,  1562,  1852,    97,   866,   862,    98,    99,   100,   934,
   101,   102,  1207,  1507,   390,   878,  1809,   103,   331,  1286,
  1287,  1288,  1876,  1877,  1868,  1878,  1879,  2299,  2076,  1289,
  1290,  2188,  1839,  2267,  2268,  2303,  2336,  2337,  2383,  1864,
   104,   968,  1291,  1292,   105,   711,   966,  1591,  1592,  1593,
   106,   332,   107,   108,   706,   944,   945,  1855,  2054,  2197,
  2348,  2349,   109,   110,   472,   333,   971,   715,   972,   973,
  1594,   111,   112,   359,   505,   733,   113,   366,   367,   368,
  1309,  1611,   114,   115,   334,  1602,  2084,  2085,  2086,  2087,
  2229,  2309,   116,   117,  1572,   709,   953,  1275,  1276,   118,
   351,   119,   724,   120,   121,  1595,   474,   980,   122,  1559,
  1257,   123,   959,  2357,  2375,  2376,  2377,  2058,  1578,  2326,
  2359,   125,   126,   127,   314,   128,   129,   130,   947,  1270,
  1567,   612,   131,   132,   133,   392,   632,  1212,  1512,   134,
   135,  2360,   739,  2224,   988,   989,  2361,   138,  1210,  2362,
   140,   481,  1604,  1888,  2093,   141,   142,   143,   853,   400,
   637,   374,   419,   887,   888,  1217,   894,  1222,  1225,   698,
   483,   484,  1804,  2004,   641,  1219,  1185,  1489,  1490,  1491,
  1787,  1492,  1797,  1798,  1799,  1995,  2260,  2340,  2341,   720,
  1493,   829,  1427,   583,   584,   585,   586,   587,   954,   761,
   773,   756,   588,   589,   750,   999,  1323,   590,   591,   777,
   767,  1000,   593,   824,  1424,  1730,   825,   594,  1130,   819,
  1042,  1009,  1010,  1028,  1029,  1035,  1364,  1647,  1043,  1451,
  1452,  1757,  1758,   595,   993,   994,  1319,   743,   596,  1187,
   871,   872,   597,   598,   315,   745,   277,  1883,  1188,  2310,
   387,   485,   600,   397,   601,   602,   603,   604,   605,   287,
   956,   606,  1113,   384,   144,   296,   281,   425,   426,   666,
   668,   671,   913,   288,   289,   282,  1531,   145,   146,    40,
    48,    41,   420,   164,   165,   423,   904,   166,   655,   656,
   657,   658,   659,   660,   661,   896,   662,   663,  1229,  1230,
  2009,  1231,   147,   148,   297,   298,   149,   500,   499,   150,
   151,   436,   675,   152,   153,   154,   155,   923,  1538,  1247,
  1532,   916,   920,   689,  1533,  1534,  1821,  2011,  2012,   156,
   157,   446,  1066,  1180,    42,  1248,  2156,  1181,   607,  1067,
   608,   859,   609,   690,    43,  1232,    44,  1233,  1548,  2157,
    46,    47
};

static const short yypact[] = {-32768,
  2491,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,  1500,-32768,-32768,-32768,-32768,-32768,  1578, 23971,   280,
   122, 23143,   177, 27548,   177,  -167,    84,    89,    35, 27548,
   547,  1555, 27823,   120,  2377,   547,    62,    30,   875,   116,
   875,   547,   121, 25348, 25623,  -167,   547, 27548,    82,    79,
   186, 25623, 21286,   107,   220,    30, 25623, 26173, 26448,    79,
   -88,  4311,   394,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,   512,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,   448,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,   459,   101,-32768,-32768,-32768,-32768,-32768,
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
-32768,-32768,-32768,-32768,-32768,-32768,   242,-32768,-32768,-32768,
   242,-32768,-32768,   272, 23419,-32768,-32768,-32768,    48,-32768,
-32768,   177,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   372,
-32768,-32768,   473,-32768,   505,   193,   193,   632, 25623,   177,
   630,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   177,
 27548, 27548,-32768,-32768, 27548, 27548,-32768, 27548, 25623,-32768,
   501,   431, 20711,   462,   177,    37, 25623, 27548,   177,-32768,
 27548,-32768, 27548, 27548, 27548,-32768,  1390,   566,-32768, 27548,
 27548,-32768,   526,-32768,-32768,-32768,-32768,-32768,   106,   560,
-32768,-32768,-32768,-32768,-32768,   599,   463,-32768, 25623,   639,
-32768,-32768,   643, 10358, 23695,    -2,   654,   710,   -79,-32768,
-32768,   690,-32768,-32768,   752,-32768,   740,-32768,-32768, 25623,
-32768,   675, 27548,-32768,-32768,-32768,-32768,-32768,-32768, 25623,
   106,-32768,   727,   767,-32768,   741,   819,-32768,   743,    74,
   547,   942,-32768,-32768,   -88,   913,   949,   913,   928,-32768,
   941,-32768,   135, 27548,-32768,   773,   773,-32768,-32768,  1022,
  1014,  1383, 27548,-32768,   272,-32768,   272,   831, 27548,-32768,
   966, 27548,-32768,-32768, 28098,-32768,-32768,   193,   834,   972,
  1160,   972,  1171,   399,  1004,   945,-32768,  1185,-32768, 25623,
  1124,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,   961,-32768,-32768, 27548,-32768,  1109,-32768,-32768,
  1176,  1074,-32768,   987,-32768,-32768,  1114, 21561,-32768,   945,
-32768,   993,-32768,   107,-32768,-32768,-32768,-32768,-32768,   372,
-32768,-32768,-32768,  1035,   472,-32768,-32768, 27548,   895,   -21,
 27548, 27548,   174,   236,   252,   263,   320,   359,   369,   379,
   414,   436,   440,   466,   468,   482,   494,   530,   540,   543,
   544,   548,   555,   556,   557,   581,   614,   634,   657,   658,
   662,   664,   669, 22864,  1000,  1130,  1130,  1048,-32768,  1064,
  1070,-32768,  1097,  1235,  1106,  1138,-32768,  1153,   950,  1257,
  1130, 16298,  1156,-32768,  1173,  1198,  1210,   708,   -61,  1218,
-32768,-32768,-32768,   724,  6465, 16298,  1115, 16298,-32768, 16298,
 16298, 15407,   107,  1166,-32768,-32768,-32768,-32768,  1225,-32768,
   726,  1273,-32768,  4665,-32768,  1115,   -55,-32768,  1189,  1228,
-32768,  1213,-32768,-32768,-32768,   480,-32768,    54,   744,-32768,
-32768,-32768,-32768,    21,  1347,    65,    65, 20999,   831, 25623,
  1330, 27548,-32768,  1074,  1412,   472,-32768,  1401,-32768,  1407,
-32768, 25623,-32768,-32768,-32768,-32768,   -88, 16298,   -88,  1374,
   173,  1482,-32768,-32768,-32768,  -167,-32768,-32768,-32768,-32768,
  -167,  -167,  1087,-32768,-32768,-32768,  1279,-32768,-32768,  1283,
  1293,-32768,-32768,-32768,  1309,-32768,  1035,-32768,  1326, 23971,
  1420,  1383,-32768,-32768,-32768,  1321,-32768,-32768,-32768,-32768,
-32768,   800,-32768,-32768,-32768,-32768,-32768,   315,   859,-32768,
  1324, 27548,-32768,  1599,  1331,-32768,-32768,    17,  1382,   -83,
-32768,   -83,   -88,-32768,-32768,   409,  1406,  8598,  1389,-32768,
  1067,  1360,   107, 20423,-32768,  1502,-32768,  1544, 16298,-32768,
 27548, 25623,-32768,-32768,-32768,-32768, 26723,-32768,-32768,-32768,
 27548, 27548,  1539,  1483,-32768,  1489,  1395, 19850,-32768,-32768,
  1582,-32768,  1491,  1115,  1396,  1213,  1402, 16298,-32768,-32768,
  1619, 15407,  1035,  1035,  1035,-32768,-32768,  1526,  1059,  1035,
-32768,  1517,  1519,  1520,  1523,-32768,-32768,  1130,-32768,  1574,
 16298,  1035,-32768, 18377, 15407,  1528,-32768,  8873,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1235,
-32768,  1509,-32768,-32768,-32768,-32768,   139, 16595,-32768,  1650,
  1650,  1650,  1410,  1411,  1418,  2970,-32768,   -57,-32768,  1035,
 24523, 28850, 16298, 16892,  1421,   420, 16298,   346, 16298,-32768,
-32768, 15704, 10655, 10952, 11249, 11546, 11843, 12140, 12437,-32768,
   -48, 10358,  1604, 21836,  6810, 27548, 24247,-32768,-32768,-32768,
-32768,-32768,-32768, 28373,-32768,-32768,-32768,-32768,-32768,  1115,
   -47,-32768,  1423,   862,   315,-32768,  1475,    60,    37,-32768,
  1454,-32768,-32768,-32768,  1430,-32768,  1432,-32768,  3664,-32768,
  1584,    28,   912,-32768,  1711,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  1450,  2932,    59,    59, 27548,-32768, 27548,
-32768,  1383,-32768,-32768,   272,  1439,-32768,-32768,-32768,  1441,
   127,  -112,  1719,-32768,-32768,-32768,-32768,-32768,-32768,    58,
  1664,  1664,  1664,-32768,   177,-32768,-32768,   153,   153,-32768,
-32768,-32768,-32768,  1598,  1600,  1467,  1531,-32768,  1596,-32768,
-32768,-32768,   439,-32768,-32768,-32768,-32768,  1493,  1609,   317,
-32768,   317,   317,   317,   317,-32768, 25073,  1695,  1538,  1481,
  1485,   715,-32768, 25623,   -74,  4665,-32768,-32768,  1468,  1471,
  1469,-32768,-32768,   272, 25898,-32768, 10358,   785,-32768,  1115,
 25898, 16298,    -7,-32768,-32768, 27548,  3838,  1601,  1694,-32768,
   -94,  1476,  1477,   811,  1478,-32768,-32768,-32768,  1479,  1669,
-32768,  1484,   478,   261,  1615,  1649,-32768,  3273,   849,  1490,
  1492,  1494,  1495, 18377, 18377, 18377, 18377,  1497,   254,  1115,
  1496,-32768,   480,   -45,  1498,  1589, 12734, 15407, 12734, 12734,
  4017,  -103,  1501,  1503,   248,   911,   915,   325,  1512,  1513,
 16595,  1518,  1529,  1530, 16595, 16595, 16595, 16595, 15407,   350,
  4717,  1115,  1532,   484,   841,   611,-32768,    36,-32768,  1269,
 16298,  1504,  1521,  1541,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,  1574,  1540,-32768,  1543,  1545,-32768,
  1548,  1549,  1550,-32768, 16892, 16892, 16892, 16892, 16298,   636,
  1115,  1553,-32768,   480,-32768,-32768,  6084,-32768,   415,-32768,
-32768,  1056, 16892,  1556, 16298,  1969,  1558,  1567, 13031,   139,
  1568,  1569, 13031,  1072,  1570,  1571, 13031,   877,  1572,  1575,
 13031,   877,  1576,  1577, 13031,    26,  1579,  1580, 13031,    26,
  1581,  1585, 13031,  1650,  1586,  1587, 13031,  1650,   220,  1583,
-32768,    54,-32768, 19565,  1074,-32768,  1524,-32768,-32768,  1554,
-32768,   -72,  1524,-32768,-32768, 27548,-32768,-32768, 22864,  1074,
 22111,  1508,  1719,-32768,-32768,-32768,   392,  1746,  1603,  1629,
 27548,-32768, 16298,-32768,-32768,   774,-32768, 27548,-32768,-32768,
-32768,  -149,-32768,-32768,  1610,-32768,  1862,-32768,   659,-32768,
  -167,  2568,-32768,  1588,  1590,-32768,  1620,-32768,-32768,    57,
    57,   644,  1602,   644,  1595,-32768,-32768,  1126,  1288,  1605,
-32768,  1768,  1769,  1607, 25073,-32768, 27548, 27548, 27548, 27548,
-32768,-32768,-32768,  1782,  1782, 25623,   409,    66,  1618,-32768,
-32768, 24798,-32768,-32768,  1705, 24798,   134,  1035,-32768,-32768,
-32768,-32768,-32768,-32768, 27548,   923,-32768,-32768,-32768,-32768,
  1023,-32768, 28648,  1526, 20711, 20135, 20135, 20423,-32768,  1717,
  1798,  1798, 27548,-32768, 26998,  1583, 27548,-32768,  1714,-32768,
  1053, 27548,   -63,-32768,-32768,  4205, 15407,-32768,  1810, 28850,
 27548, 27548,-32768, 16298, 15407,-32768,-32768,-32768,  1035,-32768,
-32768,-32768, 16298,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768, 16298,  1035,-32768, 18377, 18377, 15407,  9170,   374,  1855,
  1855,   200,-32768, 28850, 18377, 18674, 18377, 18377, 18377, 18377,
-32768,  7406, 15407,  1805,-32768,-32768,  1617,  -103,  1621,  1622,
 15407,-32768, 16298,  1035,  1035,  1526,  1059,  3345,-32768, 18377,
 15407,  9467,   285,-32768,  1859,-32768,  1859,-32768,  1859,-32768,
  1623, 28850, 16595, 16892,  1625,   655, 16595,   391, 16595,   717,
   820, 10061, 13328, 13625, 13922, 14219, 14516, 14813, 15110,   854,
  7108, 16595,  1115,  1626,  1800,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,     8,  3380,   184,-32768,  1504,-32768, 16892,
  1035,  1035, 18377, 15407,  9764,   522,  1868,  1868,  1868,  1351,
 28850, 16892, 17189, 16892, 16892, 16892, 16892,-32768,  7704,-32768,
  1628,  1631,-32768,-32768,-32768,-32768,-32768,   718,  6084,  1056,
  1526,  1526,  1630,  1526,  1526,  1632,  1526,  1526,  1635,  1526,
  1526,  1636,  1526,  1526,  1637,  1526,  1526,  1639,  1526,  1526,
  1640,  1526,  1526,  1641, 25623,   272,-32768, 25623,-32768,  1633,
  1361,-32768, 27273,  1653,  1827, 22386,-32768,-32768,-32768,-32768,
-32768,-32768, 15407,-32768,-32768,-32768,  1753,-32768,  1834,  1680,
  1681,  1066,-32768,-32768,-32768,-32768,-32768,  1658,   912,   912,
    28,-32768,-32768,  1450,  1166,-32768,-32768,-32768,-32768, 27548,
-32768,-32768,  1655,    57,  1657,   194,   234,-32768,   644,-32768,
   644,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768, 18971,
  1665,  1666, 27548,  1140, 28648,-32768,    42,-32768,  1779,-32768,
  1847,  1696,  1696,  1857,  1816,-32768,-32768,-32768,    -2,-32768,
   961,  1903,-32768,-32768,-32768,-32768,-32768,  1789,-32768,   159,
 25073,  1743, 27548,-32768,  1814,  1036,-32768,  1742, 27548,   894,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
   177,  1676,   113,-32768,-32768,-32768,-32768,-32768,-32768,  1861,
-32768,-32768,-32768,  1678,-32768, 15407,-32768,-32768,  1679,-32768,
-32768,  4665,-32768,  1682,  4665,  1662,  1683,   790,  1685,  1687,
 12734, 12734, 12734,  1688,-32768,-32768,   896,   374,    73,    73,
  1855,  1855,-32768,   -43,  -103, 15407,-32768,-32768,-32768,-32768,
  -103,  4306,  1689,  1690,  1691,  1692,  1693,  1697, 12734, 12734,
 12734,  1698,   863,   876,  3345,-32768,   806,  6084,   897,   561,
   907,   919,  1537,-32768, 16892,  1700, 16595,  4993,-32768,  1701,
  1702, 13031,   285,-32768,  1703,  1704, 13031,  2355,-32768,  1706,
  1707, 13031,  2894,-32768,  1709,  1712, 13031,  2894,-32768,  1715,
  1716, 13031,    91,-32768,  1718,  1723, 13031,    91,-32768,  1724,
  1725, 13031,  1859,-32768,  1727,  1732, 13031,  1859,-32768,  1710,
   930,   -38,  1684,-32768,  1526,  1733,-32768,-32768, 16001,  1734,
  1504,  1699,-32768,  1146,  1713,  1735,  1736,  1737, 12734, 12734,
 12734,  1740,-32768,-32768,   986,   522,   154,   154,  1868,  1868,
-32768,    85,-32768, 22661, 16892,-32768,  1744,  1720,-32768,  1747,
  1748,-32768,  1751,  1758,-32768,  1759,  1760,-32768,  1761,  1762,
-32768,  1763,  1764,-32768,  1772,  1773,-32768,  1774,  1775,-32768,
  1776,  1777,-32768,-32768,-32768,  1419,  1778,-32768, 25623,  1852,
  1839,-32768,  1839,   693,-32768,  1839,  1361,-32768,  1867, 24523,
-32768,-32768,  1932,  1926,-32768,-32768,-32768,  1828,-32768,   -88,
-32768,  1806, 27548,-32768,-32768,-32768,-32768,-32768,  1795,-32768,
  1450,-32768,-32768,-32768,   644,  1756,   644,  1783,-32768,-32768,
  1785, 18971,-32768, 18971, 18971, 18971, 18971, 18971,  1781,  1786,
-32768,  1787, 27548, 27548,  1151,-32768,  1986,  1991, 27548,   177,
  1820,-32768,-32768,  1870,  1987,   409,-32768,-32768,   107, 25623,
-32768,-32768,  1792,-32768,-32768,-32768,  1964,-32768,  1797, 27548,
 17486,  1947,  1968, 27548,-32768,-32768,  1036,-32768,-32768,   107,
-32768,-32768,-32768,-32768, 27548,  1948,  1949,-32768,  1950, 10358,
-32768,-32768,-32768,-32768,-32768, 28850,-32768,-32768,  1801,  1807,
  1808,-32768,-32768,  -103, 28850,   954,   969,  1013,  1016,  1019,
  1020,  1813,  1815,  1817,  1033, 16892,  1818,  1044,  1057,  1058,
  1178,  6084,  1537,-32768,  1526,  1526,  1819,  1526,  1526,  1821,
  1526,  1526,  1823,  1526,  1526,  1824,  1526,  1526,  1825,  1526,
  1526,  1826,  1526,  1526,  1830,  1526,  1526,  1831,  1073,  1092,
  1115,  1832,  1526,  1833,  1836,  4665,  1526,-32768,  1504, 28850,
-32768,-32768,-32768,-32768,  1838,  1840,  1841,-32768,-32768,-32768,
   986,-32768, 22661,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1972,
-32768,-32768, 25623,-32768,-32768,-32768,  1988,-32768,-32768, 25623,
-32768, 15407, 16298,-32768,   107,-32768,-32768,   842,-32768,  -167,
    29,-32768,-32768,   644,-32768,   644, 18971,  5298,   747,  2061,
  2061,  2061,  2097, 28850, 18971, 22661,  1829,   983, 18971,   445,
 18971,-32768,-32768, 19268, 18971, 18971, 18971, 18971, 18971, 18971,
 18971,-32768,  8300,  1163,  1200,-32768,-32768, 17783,-32768,  1844,
-32768,   107,-32768,  -123,  1965,-32768,  2004,  1074,  1849, 27548,
-32768, 18971,   904,  1846,-32768,  1848,  1850,-32768,-32768,-32768,
 17783, 17783, 17783, 17783, 17783,   629,  1851,-32768,-32768,-32768,
  1860,-32768,-32768,  1863,  1853,-32768,-32768,   -44,  1865,  1787,
-32768, 27548,-32768,-32768,  1246,  1866,-32768,-32768,-32768,  1869,
  1094,  1098,  1102,   232,  1112, 16892,  1871,  1874,  1875,  1123,
  1876,  1877,  1131,  1878,  1884,  1135,  1885,  1891,  1147,  1892,
  1893,  1172,  1894,  1895,  1179,  1896,  1898,  1181,  1899,  1901,
  1182,-32768,-32768,  1902,-32768,-32768,  1907,-32768,  1909,-32768,
-32768,-32768,-32768, 25623,-32768, 25623,   284,  -103,  4665,-32768,
-32768,-32768,  2932,-32768,-32768,   842,-32768,  1166,-32768,  1450,
-32768,-32768,  4618,-32768,-32768,  5298,  2074,-32768, 22661,-32768,
   572,-32768,-32768,  3068, 22661,  1904, 18971,  5112,   747,  4695,
  3005,  3005,   157,   157,  2061,  2061,-32768,  1252,  4930,  1981,
-32768,   629,   177,-32768,-32768,-32768,-32768, 27548,   107,  1929,
 27548,  1911,  2145,-32768, 17783,  1035,  1035,  1170,  2101,  2101,
  2101,   209, 28850, 18080, 17783, 17783, 17783, 17783, 17783, 17783,
 17783,  8002, 27548,  2029,  1705, 27548, 28850, 28850,    10, 27548,
  1872,-32768,-32768,  1190,   331,  1191,  1202,  1209,  1229,  1233,
  1274,  1275,  1281,  1284,  1301,  1316,  1320,  1328,  1350,  1353,
  1356,  1357,-32768,-32768,-32768,-32768,-32768, 16298,  1913,-32768,
  2875,-32768,-32768,-32768, 28850, 22661,  1292,-32768,-32768,-32768,
-32768,  2103, 22661,  3068, 18971,-32768, 27548,-32768,  1914,-32768,
  1984,-32768,-32768,-32768,   612,  1918,  1919,-32768,-32768,  1170,
   629,   786,   786,   163,   163,  2101,  2101,-32768,  1302,   629,
  1370,   118,  2070,-32768,-32768,-32768,-32768,   177,-32768,-32768,
-32768,  1384,  4665, 27548,-32768,  1921,-32768, 22661,-32768, 22661,
  1385,  4930,  1860,  1101,-32768,  1196, 28850,-32768,-32768, 17783,
-32768,-32768,-32768,-32768,    -8,-32768,  2070,-32768,   -44,  1388,
-32768,-32768,-32768,-32768,-32768,-32768,  2029,  1392,-32768,-32768,
-32768,-32768,-32768,-32768,   380,   404,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  1924,   629,   244,   244,-32768,    10, 27548,
-32768,  2070,  1101,-32768,  1930,   380,  1935,  1939,-32768,-32768,
  2168,    63,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1937,
-32768,-32768,-32768,-32768,-32768,-32768,  2232,  2235,-32768
};

static const short yypgoto[] = {-32768,
-32768,-32768,-32768,  2148,-32768,-32768,-32768,  1791,  1542,  1307,
-32768,   976,   685,-32768,  1634,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1398,   992,
   680,  1008,-32768,-32768,-32768,   390,   206,-32768, -1189,-32768,
  -897,-32768, -1050,    -3, -1954,   -75,   -99,   -62,   -86,-32768,
-32768,-32768,-32768,   694,-32768,-32768,-32768,-32768,-32768,   326,
-32768,-32768,-32768,-32768,-32768,-32768, -1231,-32768,-32768,-32768,
-32768,   -91,-32768,-32768,-32768,-32768,  -322,   712,-32768,   988,
   994,-32768,-32768,  2199,  1900,  1668,-32768,  2228,-32768,  1793,
  1312,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,    80,
   -32,   -60,-32768,-32768,    88,  1837,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,  1973,  -326,-32768,-32768,-32768,
   167,-32768,-32768,-32768,   -42,-32768, -2211,-32768,-32768,-32768,
     4,-32768,-32768,-32768,  1041,-32768,-32768,-32768,-32768,-32768,
-32768,   746,-32768,-32768,-32768,  2226,-32768,-32768,  1103,-32768,
  1927,     9,-32768,     3, -1489,  1010,    12,-32768,-32768,    16,
-32768,  1445,  1029,-32768,-32768,  -491,   -90,  4603,-32768,  1155,
  1917,-32768,-32768,-32768,  1119,-32768,-32768,   816,  -140,-32768,
  -345,   108,-32768,-32768,-32768,-32768,  1472,-32768,-32768, -1447,
-32768,   851,-32768,   550,   546,  -755,-32768,-32768,   -26,  -617,
-32768, -1485, -1375,  -815,  1780,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  -663,  -485,-32768,-32768,-32768,  3535,-32768,
-32768,  -315,  -679,   620,-32768,-32768,-32768,  4197, -1005,  -580,
  -669,   973,-32768,  -873,  -900,  -868,-32768,-32768,  -865,   683,
-32768,   432,-32768,-32768,-32768,  1363,-32768,-32768,  2294,  1505,
-32768,  1154,  -961,  1506,-32768,   348,  -281,-32768, -1466,    51,
  -282,    24,  3259,-32768,  4589,   684,    -1,     1,   -27,  -302,
  -563,  1967,   562,-32768,-32768,   -10,-32768,  2082,-32768,  1456,
  1938,-32768,-32768,  1457,  -372,   -19,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  -133,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,   766,-32768,-32768,-32768,   844,
-32768, -1769,-32768,-32768,-32768,  1995,-32768,-32768,-32768,-32768,
-32768,  1879,  1451,-32768,-32768,-32768,-32768,  1514, -1215,  1168,
  -356,-32768,-32768,-32768, -1191,-32768,-32768,-32768,   215,-32768,
-32768,  -230,  -351,  1393,  2214,  1459,-32768,   906,  -429,  -806,
  1054,  1208,   -40,   -52,-32768,   227,-32768,     2,-32768,   238,
-32768,-32768
};


#define	YYLAST		29127


static const short yytable[] = {    38,
   410,    39,    45,   305,   495,   676,   880,   450,   284,   291,
   473,   284,   299,   302,   303,   850,  1114,   452,   740,   302,
   493,   278,   302,   385,   278,  1311,   354,  1135,  1540,  1806,
   290,   422,  1256,   302,   302,  1565,   486,   302,   280,  1819,
  1786,   302,   302,   455,   955,   124,   302,   302,   302,  1535,
   136,  2010,  1733,   137,   857,  2159,   398,   139,   592,   673,
   678,   751,   489,  1845,   431,  1184,  2227,  1363,   679,   851,
  1727,  2167,   857,  1728,   736,   769,   447,   300,   930,   353,
   931,   688,  1001,    73,   832,   680,   336,  1324,  2366,   355,
  1220,   681,   682,   611,  1317,   124,  1324,    73,   158,  1519,
   136,   159,   616,   137,   160,  1034,   304,   139,    73,   161,
   624,   162,   163,   306,  2358,   307,   860,   932,  1324,  1436,
  1437,  1438,  1439,  1349,  1350,  1351,  1352,   318,  1614,   311,
   936,  1354,   683, -1098,  2195,  1250,   776,  1458,  1060,  1251,
   158,  2196,  1847,   159,   292,  1324,   160,  1324,  1520,  1392,
    73,   161,  1324,   162,   163,   861,   684,   478,  2394,   937,
   865,  1566,   734,   995,  2390,   741,   685,    25,   501,  1252,
   737,  1367,  1205,  1369,  1370,   643,   644,   415,   645,   646,
  2367,   647,   648,   372,   649,  1192,   650,  1244,   992,   651,
   652,   653,   654,   716,  1325,   356,   416,   832,   439,   272,
  2308,   440,   274,  1325,   617,  1326,   479,   441,   442,  2395,
    91,   940,  1441,  -670,  1848,  2024,  1253,   696,   312,   373,
  2272,  2213,    25,   443,    91,  1325,   444,  1498,   308,   275,
  1574,  2333,   337,   313,  -670,    91,  1250,   357,   432,  1072,
   369,  1206,   852,   686,   284,  1254,   480,  2228,   502,   687,
  1199,   434,  1325,   358,  1325,  1575,  1903,   278,  1354,  1325,
   418,  1950,   875,   309,   836,   503,   823,  2213,   302,   291,
  1252,   933,   273,  2334,   430,  1324,  1886,   738,   310,   291,
   302,   302,  1017,   272,   302,   302,   274,   302,   302,  1198,
  1441,   389,   302,  1202,   477,   273,   302,   302,   291,   914,
   302,  1887,   302,   302,   302,  1576,  1729,    25,  1221,   302,
   302,  2317,  1354,   275,   848,   849,  2380,  1253,  1135,  1135,
  1135,  1135,  1577,  1829,   635,  1830,  2160, -1304,   302,  1011,
   304,   275, -1304,   498,   284,   272,  1135,   433,   274,   273,
   273,  1992,  1823,  1392,   453,   454,  1254,   278,   613,   302,
   391,   458,   302,  2044,  2045,  1958,  1255,   506, -1197,   302,
 -1098,  1359,  1360,  2344,   487,  2345,   488,  1262,  1368,   840,
   664,  1060,  1355,   633,   496,  1060,  1060,  1060,  1060,  1408,
  1409,   273,  1325,   302,  1969,    25,   841,  2381,  1667,  1441,
  2010,   291,   302,   504,   399,   124,  1263,  1338,   302,   506,
   136,   302, -1098,   137,   302,   892,    25,   139,   304,  1314,
  1396,   445,   290,   393,-32768,  1143,  2382,   677,  2088,   302,
   413,   893,   382,  1242,  1734,   846,   847,   848,   849,   394,
   396,  1339,  1354,  2258,   402,   302,  1745,  1746,  1747,  1748,
  1749,  1750,  1446,  1447,  1628,  2040,  2041,   302,  -599,  1361,
   272,  2220,  2221,   274,  1637,  1638,  1639,  1640,  1641,  1642,
  1675,   285,   691,    25,  1144,   693,   728,   302,   695,  -599,
   302,   302,  1629,    25,  2259,  1356,    64,  1586,  1630,  1145,
  1731,  1410,  1634,  1301,  2214,   676,  1357,  1358,  1359,  1360,
  1825,     2,  2215,  2216,  2217,  2218,  2219,  2220,  2221,  1635,
    64,    70,   730,   302,  1619,   941,  1657,  1443,  2288,  1676,
  -600,   725,  1658,    25,  2175,  1400,  1662,   424,  1444,  1445,
  1446,  1447,  1455, -1263,  1677,    70,  -601,  1138, -1263,  1356,
   942,  -600,  1401,  1827,   816,   414,   592,  -602,  1636, -1241,
  1357,  1358,  1359,  1360, -1241,  2145,   886,  -601,   890,   275,
  1448,   415,  2147,   272,   286,   827,   274,   275,  -602,  1737,
-32768,  1456,   435,  2176,  1139,  1738,  1140,  1495,    75,  1742,
   416,  1406,  1407,  1408,  1409,    73,  1664,   302,  2177,   302,
  1441,   302,  1501,  2138,   731,   437,   863,  1135,   867,   867,
   732,   302,    75,   897,  -603,  1335,    25,   943,   897,   897,
  1457,   417,  2231,   704,  1278,  1141,  1443,   705,   955,  2013,
   921,  2015,   955,   922,  1336,  -603, -1246,  1444,  1445,  1446,
  1447, -1246, -1264,  1135,  2056,  1744,    87, -1264,  1237,   284,
   438,   291,  1261,  -604,   418,  1135,  1135,  1135,  1135,  1135,
  1135, -1217,   278,  -605,   448,   879, -1217,   410,  1337,-32768,
    87,   302,   290,  -606,  -604,   451,   449,  1505,  1506,   911,
  1357,  1358,  1359,  1360,  -605,   909,   273,   302,  1918,  1921,
  2213,   592,  2263,   302,  -606,   460,   459,  1034,  2327,  2269,
   302,   302,  1861,   461,   482,   969,   302,  2213,  -607,   476,
   302,   302,  1644,  1645,  1441,   494,  2256,   302,  2257,  1379,
  1442,  1651,    91,  1384,  1386,  1388,  1390,  1919,  1570,  -607,
  -608,  1034,  1573,  1060,  -609,   928,   510,  1060,  2270,  1060,
   507,   599,  1060,  1060,  1060,  1060,  1060,  1060,  1060,  1060,
   312,  -608,  1060,  2301,  2282,  -609,  1272,   621,  1273,  1586,
  -610,  1722,  -611,  1391,   977,   313,  1920,   625,   508,  1971,
   981,  1002,  1003,  1004,   982,   983,  -612,  2271,  1012,  2088,
   509,  -610,  1669,  -611,  1034,  1899,  1900,  1901,  -613,   622,
  1019,    64,  1234,  1235,  -898, -1218,  1441,  -612,  -905,  1752,
 -1218,  2018,  1755,  2019,  2020,  2021,  2022,  2023,   511,  -613,
   302,   816,   512,  1912,  1913,  1914,    70,-32768,  2161,  1670,
  2162,  1671,   497,   275,  -614,  2024,  1791,   712,  1444,  1445,
  1446,  1447,  1792,   302,  -615,   302,   302,  -616,  -617,   917,
   918,  1615,  -618,   302,  1793,  -614,  1073,   919,  1074,  -619,
  -620,  -621,  1724,   823,   678,  -615,   615,   823,  -616,  -617,
  1672,   823,   679,  -618,  2213,   823,    73,  1795,  1354,   823,
  -619,  -620,  -621,   823,   614,  -622,  1896,   823,  2192,   680,
  1796,   823,   618,    75,  1441,   681,   682,   302,  1135,   302,
  1916,   291,  2028,  1965,  1966,  1967,  -622,   619,   924,   925,
  1194,  2208,  2209,  2210,  2211,  2212,   926,  2214,  -623,   620,
  1223,  1226,   290,   628,   291,  2215,  2216,  2217,  2218,  2219,
  2220,  2221,  1412,    38,  2214,    39,   683,  1413,  -624,  -623,
  2104,  1443,  2215,  2216,  2217,  2218,  2219,  2220,  2221,  1243,
  1245,    87,  1444,  1445,  1446,  1447,   302,   627,  2095,  -624,
   684,  -625,  -626,   302,  1302,   832,  -627,   360,  -628,  1536,
   685,   629,  1537,  -629,   302,   630,   834,  1011,  1135,   631,
   302,  1523,  -625,  -626,  1354,   302,  1524,  -627,  1280,  -628,
  1280,  1280,  1280,  1280,  -629,   873,  2163,   876,   634,   493,
   493,   361,  1588,    91,  2166,   636,  1904,  2032,  2174,   885,
  2178,  1869,  -661,  2179,  2180,  2181,  2182,  2183,  2184,  2185,
  2186,   361,  2189,  1443,  2033,   835,   362,  1060,  -598,  1871,
  -659,  1617,   836,  -661,  1444,  1445,  1446,  1447, -1255,  1623,
   837,  2203,  1298, -1255,  1299,  2285,   362,   686,  -597,  -598,
   638,  -659,-32768,   687,  2290,  2291,  2292,  2293,  2294,  2295,
  2296,  2297,  2300,  2038,  2039,  2040,  2041,  1996,   640,  -597,
  1998,  1666,   762,   642,  1441,  1674,   363,  1679,  1872,  1515,
  1684,  1689,  1694,  1699,  1704,  1709,  1714,  1719,  1516,  1517,
  1723,  2214,  1873,   364,   667,  1356,   363,   763,  1874,   978,
-32768,-32768,  2218,  2219,  2220,  2221,  1357,  1358,  1359,  1360,
  2096,  1443,  1312,   364,  1313,   672,   764,   208,   670,  2100,
  2170,  1875,  1444,  1445,  1446,  1447,   816,  1258,  1259,  1260,
  2235,   317,   283,   283,   304,   283,   352,   840,  1329,  1135,
  1330, -1257,   371,  1869,   832,  2151, -1257,   386,  2152,  1870,
   218,    25,   697,    26,   841,   834,  2274,  2171,   900,  2172,
   832,  1871, -1219,   365,   901,  -898,   902, -1219,   903,  -905,
  2365,   834,  2153,   302,  2139, -1216,  1343,   692,  1344,   765,
 -1216,   200,   842,   365, -1232,   302,  -898,   818,   302, -1232,
   302,-32768,-32768,   846,   847,   848,   849, -1230,  2173,   699,
   302,  1356, -1230,    25,   835,    26,   202,   302,  1525,  1508,
  1872,   836,  1357,  1358,  1359,  1360,   701,  1807, -1261,-32768,
   835,    25,  1224, -1261,  1873,   207,   208,   836, -1256,   599,
  1874,   707, -1242, -1256,  1441,   837, -1244, -1242,  2165,  1374,
 -1259, -1244,  1960,  1375,   302, -1259,   302,   302,   302,   302,
  1581, -1239,  1582,  1875,  2322,   302, -1239,   746,  2213,   218,
    38,   302,    39,  1527,  1513,   302,  1441,   703,  1006,  1007,
  1568,  1513,  2106,   708,   302, -1243,  1546,  1546,  1547,  1547,
 -1243,   710,   816,   713,   302,   302,   302,   302,   223,   714,
 -1245,  1443,   302,   722,   302, -1245,   302,   718,  1597,  1597,
   719,   302,  1444,  1445,  1446,  1447,  1579,   960,   717,   816,
   302,   302,   961,  1560,   721,  1279,   840,  1281,  1282,  1283,
  1284,   727,    64,   962,  1280,  1280,  1596,  1596,   748,  1135,
  1891,   746,   840,   841, -1247,   963,   964, -1248,  1580, -1247,
 -1249, -1250, -1248,   816,    25, -1249, -1250,    70,  1301,   841,
  1583,  1300,  1584,   749, -1254,  1924,  1606,  1624,  1608, -1254,
   965,   842,  2148,   768,   599, -1260,   275,   428,   283,   831,
 -1260,  1627,   846,   847,   848,   849,   752,   842, -1258, -1262,
   852,   816,  1612, -1258, -1262,   843,   844,   845,   846,   847,
   848,   849,   753,  1813, -1238,  1814,  1391,    73,   754, -1238,
  2132,  1391,  1653,  1654,   272,   273,  1391,   274,  1815,  1816,
    25,  1391,    26, -1240,    75, -1251,  1391,  1414, -1240, -1252,
 -1251,  1391,   757, -1253, -1252,   755,  1391,  2289, -1253,  1542,
   816,  1391,  1543, -1265,   758,    25,   283,    26, -1265,   832,
   818,  2306,  2307,  1415, -1267,   833,   898,   899,   816, -1267,
   834,  1443, -1274,  1544,  1732,  1545, -1272, -1274,   283,  1735,
  1736, -1272,  1444,  1445,  1446,  1447,   759,  1581, -1273,  1846,
  2200,  2354,    87, -1273,   302,-32768,  1790,   302,  1312,  2316,
  2046,   760,   302,  1443,   771,   302,  2218,  2219,  2220,  2221,
  1312,   828,  2190, -1268,  1444,  1445,  1446,  1447, -1268,   835,
 -1269,   772, -1271, -1270,  1791, -1269,   836, -1271, -1270,   864,
  1792, -1231, -1266,   854,   837,   283, -1231, -1266,  -572,   302,
   283,  2355,  1793, -1283,  2356,   838,   774,  1312, -1283,  2191,
 -1275,  1794,    49,   839,  1790, -1275,    50,   856,   775,    51,
    52,  2364,   302,    53,   816,  1795,   778,  1226,  1226,  1223,
 -1290,    54,    55,   830, -1282, -1290,   855,   877,  1796, -1282,
   881,  1494,  1791,   883,  1826,  1828,    56,    57,  1792,   884,
   302,   613,   302,   852,  1416,  2232,   873,  2051,   302,  2275,
  1793,  2276,  1417,  1418,  1419,  1420,  1421,  1422,  1423,  1794,
  1884,  1542,   891,    58,  1543, -1288, -1280,    25,    59,    26,
 -1288, -1280, -1289,  1795,   592, -1281,   895, -1289,    60,   905,
 -1281,   840,    61,   906,    62,  1544,  1796,  1549,  1579,  2318,
  1882,  2319, -1284,   907,    63,  1392,    64, -1284,   841,  2330,
    65,  2331,    66,   908,    67,  1990,  1394, -1276,    68,   912,
 -1098, -1285, -1276,  1564,    69,   910, -1285,   158,   915, -1277,
   159,    70,   927,   160, -1277, -1214,   842,   816,   161,   929,
   162,   163,   832,   935,   843,   844,   845,   846,   847,   848,
   849, -1287,   946,   834, -1279,   958, -1287, -1286, -1278, -1279,
  1743,   974, -1286, -1278,   975,  1395,   272,   286,   967,   274,
   275,   858,  1396,   984,   985,   491,    71,  1312,    72,  2332,
-32768,    73,    74,   465,   466,   467,   468,   469,   470,   471,
   986,   721,  2318,  2339,  2346,  2370,   992,  2371,    75,  2373,
   996,  2374,   835,   987,   991,   998,  -898,    73,  1013,   836,
  1014,  1015,    76,    77,  1016,  1036,   776,   837,   832,  1069,
  1070,    78,    79,   816,  2202,  1071,  1184,  1201,   838,  1137,
   832,    80,    81,   283,  1204,   283,   833,  1209,  1211,  1213,
   318,   834,  1218,    82,    83,    84,  1227,    85,  1228,  1240,
    86,  1241,   319,  1246,   320,  1251,    87,  1266,   302,   321,
  1268,  1269,  1267,  1271,  1274,    88,   322,   323,  1277,   302,
   324,  1294,    89,  1295,  1296,  1303,  1305,  1400,  1297,    90,
  1304,   325,   302,  1321,  1322,  1327,  1328,  1331,  1332,   326,
   835,  1333,  -343,  1334,  1401,  1340,  1341,   836,  1345,  1366,
  1346,  1503,  1347,  1348,  1362,   837,  1353,  1365,    91,  1426,
  1372,  1373,   302,   302,   840,   327,   838,  -261,   302,   291,
  1376,  1377,  1402,   328,   839,   329,  1380,  1428,  1496,   302,
  1454,   841,   330,  1406,  1407,  1408,  1409,  1381,  1382,   302,
  1411,  2057,  1784,   302,  2363,  1494,  2007,  1509,  1430,  2024,
  1429,  1431,  1521,  1432,   302,  2025,  1433,  1434,  1435,   842,
  2026,  1449,  2083,  1497,  1459,   816,  1461,   843,   844,   845,
   846,   847,   848,   849,   816,  1462,  1464,  1465,  1467,  1468,
  1470,  1068,  2049,  1471,  1473,  1474,  1510,  1476,  1477,  1479,
  1486,   816,   746,  1480,  1482,  1483,  1511,  1136,  1522,  1528,
  2278,  1529,   840,  2063,  1541,  1530,  1551,  1552,  1539,  2027,
  1561,  1569,  1571,  1550,  1182,  1553,  2028,  1601,  1603,   841,
  1195,  1610,  1618,  1354,  2029,  1646,  1648,  1392,  1726,   816,
  1649,  1650,  1663,  1668,  1725,  2030,  1441,  1753,  1754,  1762,
  1789,  1765,   816,  2031,  1768,  1771,  1774,   842,  1777,  1780,
  1783,  1802,  1803,  1808,  1810,   843,   844,   845,   846,   847,
   848,   849,   302,  1811,  1812,  1813,  1822,  2158,  1824,   302,
  1849,  1894,  1850,  1843,  1844,   283,  1851,  1854,  1856,  1859,
  1860,  1863,  1867,  1880,  1885,  1889,  1890,  2150,  1892,  1993,
  1951,  1893,  1895,   816,  1897,   816,  1898,  1902,  1906,  1907,
  1908,  1909,  1910,  1994,  2000,  1959,  1911,  1915,  1922,  1925,
  1926,  1928,  1929,  2002,  1931,  1932,  2154,  1934,  2155,  1949,
  1935,  2032,  1961,  1937,  1938,   279,  1940,  1973,   279,   302,
   301,  1941,  1943,  1944,  2194,  1946,   316,   832,  2033,   301,
  1947,  1953,  1957,   833,  1962,  1963,  1964,  1182,   834,  1968,
   379,   383,  2003,  1972,   388,  2005,  1974,  1975,   383,   383,
  1976,   302,  2014,   383,   405,   408,  2034,  1977,  1978,  1979,
  1980,  1981,  1982,  1983,  2035,  2036,  2037,  2038,  2039,  2040,
  2041,  1984,  1985,  1986,  1987,  1988,  1989,  1991,  2008,  2006,
  2042,  2047,  2016,  2017,  2043,  -905,  2048,   835,  2052,  2053,
  2060,  2055,  2061,  2079,   836,  2062,  2080,  2092,  2091,  2144,
  2097,  2094,   837,   302,  1068,   302,  2098,  2099,  1068,  1068,
  1068,  1068,  2101,   838,  2102,  2146,  2103,  2105,  2110,  2024,
  2113,   858,  2116,  2119,  2122,  2125,  2198,  2169,   816,  2128,
  2131,  2133,  2135,  2199,   816,  2136,  1494,  2140,  2266,  2141,
  2142,  2193,  1454,  2201,  2205,  2277,  2206,  2281,  2207,  2222,
  2226,    38,   291,    39,  2154,  2024,  2155,   302,  2223,  2213,
   302,  2025,  2225,  2230,  2302,  2233,  2026,  2320,  2234,  1312,
  2236,  2280,   816,  2237,  2238,  2239,  2240,  2241,  1136,  1136,
  1136,  1136,   302,  2242,  2243,   302,   816,   816,  2311,   302,
  2244,  2245,  2246,  2247,  2248,  2249,  1136,  2250,  2251,   840,
  2252,  2253,  2273,  2024,  2286,  2287,  2254,  2059,  2255,  2025,
  2283,  2314,  2324,  2325,  2026,  2027,   841,  2328,  2329,  2335,
  2343,  2279,  2028,  2379,   816,   816,  2389,  2391,  2393,  2396,
  2029,  2398,   816,  1454,  2399,   858,   302,   599,  2392,   412,
  1563,  2030,   702,   939,   842,  1265,  1554,  1853,  1179,  2031,
   868,   279,   843,   844,   845,   846,   847,   848,   849,    38,
  1862,    39,  1527,  2027,  1556,   410,  2082,  2311,  2204,  2321,
  2028,  2372,  2387,   302,  2368,   383,  1865,   816,  2029,   816,
  2384,  2388,  1858,  2353,   401,  1600,   816,   301,   301,  2030,
  1599,   279,   457,   882,   301,   383,  2352,  2031,   370,   475,
   626,   735,  1315,   383,   301,  2305,  2369,   301,  2385,   301,
   279,   457,  2304,  2378,  1857,   409,   301,   301,   623,   492,
  1518,  1613,  2350,  1208,  2351,  2347,   726,  2032,  2311,   302,
  1605,  1514,  2353,  1485,   639,   383,  1817,  2312,  1788,  1997,
  1494,   279,  1200,  2386,  2033,  2352,  1999,  1494,  1955,  1656,
  1917,  1454,  1500,  2107,   814,  1318,   383,  1183,  2338,   388,
  1193,  2001,   427,  1236,   669,  1239,   383,  1818,  1238,   610,
  1504,  2350,  2034,  2351,  2264,  2032,  1306,   421,   729,  2261,
  2035,  2036,  2037,  2038,  2039,  2040,  2041,  1249,  1203,  1487,
   665,  1785,  2033,  2262,     0,     0,  2164,     0,     0,   674,
     0,     0,     0,     0,     0,   301,     0,     0,   301,     0,
     0,   301,     0,  1392,     0,     0,     0,     0,     0,     0,
  2034,     0,     0,     0,  1394,     0,   383,     0,  2035,  2036,
  2037,  2038,  2039,  2040,  2041,     0,     0,  1454,     0,     0,
     0,     0,   388,     0,  2284,     0,  1068,  1136,     0,     0,
  1068,     0,  1068,     0,   301,  1068,  1068,  1068,  1068,  1068,
  1068,  1068,  1068,     0,     0,  1068,     0,     0,     0,     0,
     0,     0,     0,  1395,   301,     0,     0,   301,   742,     0,
  1396,     0,     0,  1136,     0,     0,     0,     0,  1397,     0,
     0,  1494,     0,  1494,     0,  1136,  1136,  1136,  1136,  1136,
  1136,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   747,     0,     0,     0,     0,  1454,     0,  1454,  1454,  1454,
  1454,  1454,     0,     2,     0,     0,     0,     0,     0,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    12,  1182,
    13,    14,    15,    16,    17,    18,    19,    20,    21,     0,
     0,   338,     0,     0,  1454,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   339,     0,     0,     0,
     0,   340,     0,     0,     0,     0,     0,     0,   341,   342,
     0,     0,   343,     0,   874,  1400,   383,     0,   301,     0,
     0,     0,     0,   344,     0,     0,     0,     0,   383,     0,
     0,   345,  1401,     0,   346,  1454,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    12,     0,    13,    14,    15,
    16,    17,    18,    19,    20,    21,     0,   347,     0,   348,
  1402,     0,     0,     0,     0,   349,   279,   350,  1403,  1404,
  1405,  1406,  1407,  1408,  1409,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1454,     0,   301,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   957,     0,     0,     0,     0,     0,
   475,     0,     0,     0,     0,     0,     0,   301,   383,     0,
     0,     0,     0,   301,     0,     0,     0,   301,   301,     0,
  1454,     0,     0,     0,   990,     0,     0,     0,  1454,  1454,
     0,     0,  1454,     0,  1454,     0,     0,  1454,  1454,  1454,
  1454,  1454,  1454,  1454,  1454,     0,  1454,     0,  1136,     0,
  1068,  1454,     0,     0,     0,     0,     0,     0,     0,     0,
  1033,     0,     0,     0,     0,  1454,     0,     0,     0,     0,
     0,     0,     0,     0,  1454,  1454,  1454,  1454,  1454,     0,
     0,     0,     0,     0,    22,     0,     0,    23,     0,    24,
    25,     0,    26,     0,    27,     0,     0,     0,     0,    28,
     0,     0,     0,    29,  1065,     0,    30,    31,    32,    33,
    34,    35,    36,     0,     0,     0,     0,  1112,     0,     0,
  1134,     0,     0,     0,     0,     0,     0,     0,  1136,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1189,     0,  1189,   301,     0,     0,     0,     0,     0,     0,
  1197,     0,     0,     0,     0,     0,     0,   744,     0,     0,
     0,    22,     0,     0,    23,     0,     0,    25,     0,    26,
     0,    27,  1454,     0,     0,     0,    28,     0,  1454,     0,
  1454,     0,     0,    30,    31,    32,    33,    34,     0,  1526,
     0,     0,     0,     0,   665,     0,   279,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1454,     0,
     0,     0,     0,     0,     0,     0,     0,  1454,  1454,  1454,
  1454,  1454,  1454,  1454,  1454,  1454,     0,     0,     0,     0,
     0,   870,     0,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    12,     0,    13,    14,    15,    16,    17,    18,
    19,    20,    21,  1293,     0,     0,     0,     0,     0,     0,
   383,     0,     0,     0,     0,     0,     0,     0,     0,  1454,
     0,  1310,  1392,     0,     0,     0,  1454,  1310,  1454,     0,
     0,     0,  1189,  1394,     0,     0,     0,     0,     0,  1136,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    12,
     0,    13,    14,    15,    16,    17,    18,    19,    20,    21,
  1033,  1033,  1033,  1033,     0,     0,     0,     0,     0,     0,
     0,  1454,     0,  1454,     0,     0,     0,     0,     0,     0,
     0,     0,  1395,  1454,     0,     0,     0,  1065,     0,  1396,
     0,  1065,  1065,  1065,  1065,     0,     0,  1397,   832,     0,
     0,     0,     0,     0,   833,     0,     0,     0,     0,   834,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  2024,     0,     0,     0,  1030,     0,     0,
     0,     0,     0,     0,  2026,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   835,     0,
     0,  1134,  1134,  1134,  1134,   836,     0,     0,     0,     0,
     0,     0,     0,   837,     0,     0,     0,     0,     0,  1134,
     0,  1062,     0,     0,   838,     0,     0,     0,     0,     0,
     0,     0,   839,  2027,  1400,     0,  2024,  1131,     0,     0,
  2028,     0,     0,     0,     0,     0,     0,  2026,  2029,     0,
     0,  1401,     0,     0,     0,     0,     0,     0,    22,     0,
   383,    23,     0,     0,    25,     0,    26,     0,    27,  1136,
     0,     0,  1499,    28,     0,   874,     0,  1189,     0,  1402,
    30,    31,    32,    33,    34,     0,  2315,   301,-32768,-32768,
  1406,  1407,  1408,  1409,   301,     0,  2027,     0,     0,     0,
     0,     0,     0,  2028,     0,     0,     0,     0,     0,     0,
   840,-32768,     0,     0,     0,    22,     0,     0,    23,     0,
     0,    25,     0,    26,     0,    27,     0,   841,     0,     0,
    28,  1555,     0,  1555,  1557,  1558,   301,    30,    31,    32,
    33,    34,   383,     0,     0,  2032,     0,     0,   957,     0,
     0,     0,   957,     0,     0,   842,     0,     0,     0,     0,
     0,   301,  2033,   843,   844,   845,   846,   847,   848,   849,
     0,   475,  1598,  1598,   475,     0,     0,  -699,     0,   301,
     0,   301,     0,  1609,     0,     0,     0,     0,   990,     0,
  2034,     0,     0,     0,     0,     0,     0,  1620,  1621,-32768,
-32768,  2038,  2039,  2040,  2041,     0,     0,     0,  2032,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1033,  1033,     0,     0,  2033,     0,  1030,  1030,  1030,
  1030,  1033,  1033,  1033,  1033,  1033,  1033,     0,     0,     0,
     0,   832,     0,     0,     0,     0,     0,   833,     0,     0,
     0,     0,   834,  2034,  1062,     0,  1033,     0,  1062,  1062,
  1062,  1062,     0,     0,  2038,  2039,  2040,  2041,     0,  1065,
  1134,     0,     0,  1065,     0,  1065,     0,     0,  1065,  1065,
  1065,  1065,  1065,  1065,  1065,  1065,     0,     0,  1065,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   835,     0,     0,     0,     0,  1134,     0,   836,  1033,
     0,     0,     0,  1392,     0,     0,   837,     0,  1134,  1134,
  1134,  1134,  1134,  1134,  1394,     0,     0,   838,  1131,  1131,
  1131,  1131,     0,     0,     0,   839,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1131,     0,   832,     0,
     0,     0,     0,     0,   833,     0,     0,     0,     0,   834,
     0,   383,     0,     0,   383,     0,     0,     0,     0,  1801,
     0,     0,  1189,  1395,     0,     0,     0,     0,     0,     0,
  1396,     0,     0,     0,     0,     0,     0,     0,  1397,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1398,
     0,     0,   870,     0,     0,     0,  1820,     0,   835,     0,
     0,     0,     0,   840,     0,   836,     0,     0,     0,     0,
     0,     0,     0,   837,     0,     0,  1842,     0,     0,   990,
   841,     0,     0,     0,   838,     0,     0,     0,     0,     0,
     0,     0,   839,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1555,   842,  1866,
     0,     0,     0,     0,     0,  1881,   843,   844,   845,   846,
   847,   848,   849,     0,     0,     0,     0,     0,     0,     0,
  1342,     0,     0,     0,     0,  1400,     0,     0,     0,     0,
     0,     0,     0,   276,   456,     0,   276,     0,   276,     0,
     0,     0,  1401,     0,   276,     0,     0,   276,     0,     0,
     0,     0,     0,   490,     0,     0,     0,     0,   276,   276,
   840,     0,   276,     0,     0,     0,   276,   276,     0,     0,
  1402,   276,   276,   276,     0,     0,     0,   841,  1403,  1404,
  1405,  1406,  1407,  1408,  1409,     0,     0,     0,  1030,  1030,
     0,  1134,     0,  1065,     0,     0,     0,     0,  1030,  1030,
  1030,  1030,  1030,  1030,     0,   842,     0,     0,     0,     0,
     0,     0,     0,   843,   844,   845,   846,   847,   848,   849,
     0,     0,     0,  1030,     0,     0,     0,  -698,     0,     0,
     0,     0,     0,     0,     0,     0,  1062,  1131,     0,     0,
  1062,     0,  1062,     0,     0,  1062,  1062,  1062,  1062,  1062,
  1062,  1062,  1062,     0,     0,  1062,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1134,   832,  1131,     0,     0,  1030,     0,   833,     0,
     0,  1214,     0,   834,     0,  1131,  1131,  1131,  1131,  1131,
  1131,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   383,     0,     0,     0,     0,
     0,  1215,     0,     0,     0,     0,  1112,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   301,
     0,     0,   835,     0,     0,     0,     0,     0,     0,   836,
     0,     0,     0,     0,     0,     0,     0,   837,  1842,     0,
  1842,  1842,  1842,  1842,  1842,     0,     0,     0,   838,   990,
   990,     0,     0,     0,     0,   301,   839,     0,     0,   276,
     0,     0,     0,     0,     0,     0,   383,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   301,   457,     0,     0,
  2081,     0,     0,   276,     0,     0,     0,     0,     0,     0,
     0,  2090,     0,     0,  1216,   276,   276,     0,     0,   276,
   276,     0,   276,   276,     0,     0,     0,   276,     0,     0,
     0,   276,   276,     0,     0,   276,     0,   276,   276,   276,
     0,     0,  1134,     0,   276,   276,     0,     0,     0,     0,
     0,     0,     0,     0,   840,     0,   832,     0,     0,     0,
     0,     0,   833,   276,  1320,     0,     0,   834,     0,   276,
     0,   841,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   276,     0,     0,   276,     0,     0,
     0,     0,     0,     0,   276,     0,     0,     0,     0,   842,
     0,     0,     0,     0,     0,     0,     0,   843,   844,   845,
   846,   847,   848,   849,     0,     0,   835,     0,   276,   383,
     0,     0,     0,   836,     0,     0,   383,   276,  1131,     0,
  1062,   837,     0,   276,     0,     0,   276,     0,     0,   276,
     0,     0,   838,  1842,     0,     0,     0,     0,     0,     0,
   839,  1842,     0,     0,   276,  1842,     0,  1842,     0,     0,
  1842,  1842,  1842,  1842,  1842,  1842,  1842,  1842,     0,  1842,
   276,     0,     0,     0,   457,     0,     0,     0,     0,     0,
     0,     0,   276,     0,     0,     0,   301,     0,  1842,     0,
     0,     0,  1031,     0,     0,     0,     0,   457,   457,   457,
   457,   457,   276,     0,     0,   276,   276,     0,  1131,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   990,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   840,     0,
     0,     0,  1134,     0,     0,   832,  1063,     0,   276,     0,
     0,   833,     0,     0,     0,   841,   834,     0,     0,     0,
     0,     0,  1132,   766,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   815,
   383,     0,   383,   842,     0,     0,     0,     0,     0,     0,
     0,   843,   844,   845,   846,   847,   848,   849,     0,  1371,
     0,     0,     0,     0,     0,   835,     0,     0,     0,     0,
     0,     0,   836,  1842,     0,     0,     0,     0,     0,     0,
   837,     0,   276,     0,   276,     0,   276,     0,     0,     0,
     0,   838,     0,     0,   301,     0,   276,  1189,     0,   839,
     0,   457,     0,     0,     0,     0,     0,     0,     0,     0,
   457,   457,   457,   457,   457,   457,   457,   457,   457,   990,
     0,     0,  1189,     0,     0,     0,   301,     0,     0,     0,
     0,     0,     0,     0,   276,     0,     0,     0,     0,  1131,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   276,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1842,   276,  2323,     0,     0,     0,   840,   276,     0,
     0,     0,     0,     0,     0,   276,   276,     0,     0,     0,
     0,   276,     0,   832,   841,   276,   276,     0,     0,   833,
     0,     0,   276,     0,   834,     0,     0,     0,     0,     0,
  2342,     0,  1031,  1031,  1031,  1031,     0,     0,     0,     0,
     0,     0,   842,  1008,     0,     0,   457,     0,     0,     0,
   843,   844,   845,   846,   847,   848,   849,     0,     0,  1063,
     0,     0,     0,  1063,  1063,  1063,  1063,    50,     0,     0,
    51,    52,     0,   835,    53,     0,     0,     0,     0,     0,
   836,     0,    54,    55,     0,     0,  2342,     0,   837,     0,
     0,     0,     0,     0,     0,     0,     0,    56,    57,   838,
     0,     0,     0,     0,     0,     0,     0,   839,     0,     0,
     0,     0,     0,     0,   832,   276,   815,     0,     0,     0,
   833,     0,  1905,     0,    58,   834,     0,     0,     0,   411,
     0,  1616,     0,  1132,  1132,  1132,  1132,     0,   276,    60,
   276,   276,     0,    61,     0,    62,     0,     0,   276,  1131,
     0,  1132,     0,     0,     0,    63,     0,    64,     0,     0,
     0,    65,     0,    66,     0,    67,     0,     0,     0,    68,
     0,     0,     0,     0,   835,    69,     0,     0,     0,     0,
     0,   836,    70,     0,     0,   840,     0,     0,     0,   837,
     0,     0,   276,     0,   276,     0,     0,     0,     0,     0,
   838,     0,   841,     0,     0,     0,     0,     0,   839,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,    71,     0,    72,
   842,     0,    73,    74,     0,     0,     0,     0,   843,   844,
   845,   846,   847,   848,   849,     0,     0,     0,     0,    75,
     0,   276,     0,     0,     0,     0,     0,     0,   276,     0,
     0,     0,     0,    76,    77,     0,     0,     0,     0,   276,
     0,     0,    78,    79,     0,   276,     0,     0,     0,     0,
   276,     0,    80,    81,     0,     0,   840,     0,     0,     0,
     0,     0,     0,     0,    82,    83,    84,     0,    85,     0,
     0,    86,     0,   841,     0,     0,     0,    87,     0,     0,
     0,     0,     0,     0,     0,     0,    88,     0,     0,     0,
     0,     0,     0,    89,     0,     0,     0,     0,     0,     0,
    90,   842,     0,     0,     0,     0,     0,     0,     0,   843,
   844,   845,   846,   847,   848,   849,     0,     0,     0,     0,
     0,     0,     0,  1031,  1031,     0,     0,     0,     0,    91,
     0,     0,     0,  1031,  1031,  1031,  1031,  1031,  1031,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1031,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1063,  1132,     0,     0,  1063,     0,  1063,     0,     0,
  1063,  1063,  1063,  1063,  1063,  1063,  1063,  1063,     0,     0,
  1063,   815,     0,     0,     0,     0,  2024,     0,     0,     0,
     0,     0,  2025,     0,  2265,     0,     0,  2026,  1132,     0,
     0,  1031,     0,     0,     0,     0,     0,     0,     0,     0,
  1132,  1132,  1132,  1132,  1132,  1132,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   276,     0,
     0,     0,     0,   832,     0,     0,     0,     0,     0,   833,
   276,     0,     0,   276,   834,   276,  2027,     0,     0,     0,
     0,     0,     0,  2028,     0,   276,     0,     0,     0,     0,
     0,  2029,   276,  2024,     0,     0,     0,     0,   770,     0,
     0,     0,  2030,     0,  2026,     0,     0,     0,     0,     0,
  2031,     0,   817,     0,   820,  1392,   821,   822,   826,     0,
     0,  1393,     0,   835,     0,     0,  1394,     0,     0,   276,
   836,   276,   276,   276,   276,     0,     0,     0,   837,     0,
   276,     0,     0,     0,     0,     0,   276,     0,  1840,   838,
   276,     0,     0,  2027,     0,     0,     0,   839,     0,   276,
  2028,     0,     0,     0,     0,     0,     0,   815,  2029,   276,
   276,   276,   276,     0,   889,  1395,     0,   276,     0,   276,
     0,   276,  1396,     0,     0,     0,   276,     0,  2032,     0,
  1397,     0,     0,     0,   815,   276,   276,     0,     0,     0,
     0,  1398,     0,     0,     0,  2033,     0,     0,     0,  1399,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   815,     0,
     0,     0,     0,  2034,     0,   840,     0,     0,     0,     0,
     0,  2035,  2036,  2037,  2038,  2039,  2040,  2041,     0,     0,
     0,  1008,   841,     0,     0,   976,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  2032,   815,     0,     0,     0,
     0,     0,     0,  1132,     0,  1063,     0,     0,     0,     0,
   842,     0,  2033,     0,   997,     0,     0,  1400,   843,   844,
   845,   846,   847,   848,   849,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1401,     0,     0,  1018,     0,     0,
  2034,     0,     0,     0,  1041,   815,     0,     0,  2035,  2036,
  2037,  2038,  2039,  2040,  2041,     0,     0,     0,  2024,     0,
     0,     0,  1402,   815,  2025,     0,     0,     0,     0,  2026,
  1403,  1404,  1405,  1406,  1407,  1408,  1409,     0,     0,     0,
     0,     0,     0,  1132,  1061,     0,     0,     0,     0,   276,
     0,     0,   276,     0,     0,     0,     0,   276,     0,  1115,
   276,     0,     0,  1142,     0,  1146,     0,     0,  1150,  1154,
  1158,  1162,  1166,  1170,  1174,  1178,     0,     0,  2027,     0,
     0,  1392,     0,     0,     0,  2028,     0,  1393,     0,     0,
     0,     0,  1394,  2029,   276,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2030,     0,     0,     0,     0,     0,
     0,     0,  2031,     0,     0,     0,     0,   276,     0,   815,
  1840,     0,  1840,  1840,  1840,  1840,  1840,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1395,     0,     0,     0,   276,     0,   276,  1396,     0,
     0,     0,     0,   276,     0,     0,  1397,     0,     0,  2077,
     0,     0,     0,     0,     0,     0,     0,  1398,     0,     0,
     0,     0,     0,  2089,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2032,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2024,     0,     0,     0,  1132,     0,  2025,  2033,     0,     0,
     0,  2026,     0,     0,     0,     0,     0,     0,  1316,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   815,     0,     0,  2034,     0,     0,     0,     0,
     0,     0,     0,  2035,  2036,  2037,  2038,  2039,  2040,  2041,
     0,     0,     0,  1400,     0,     0,     0,     0,     0,     0,
  2027,     0,     0,  1041,     0,  1041,  1041,  2028,     0,     0,
  1401,     0,     0,     0,     0,  2029,     0,  1378,     0,     0,
     0,  1383,  1385,  1387,  1389,   826,  2030,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1425,  1402,     0,
     0,     0,     0,     0,     0,  1840,  1403,  1404,  1405,  1406,
  1407,  1408,  1409,  1840,     0,     0,     0,  1840,   815,  1840,
     0,     0,  1840,  1840,  1840,  1840,  1840,  1840,  1840,  1840,
     0,  1840,     0,     0,     0,     0,  2077,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1840,     0,     0,   276,     0,  1440,     0,     0,     0,  2077,
  2077,  2077,  2077,  2077,   276,     0,     0,     0,     0,     0,
     0,  1460,  2032,     0,     0,   826,     0,   276,     0,   826,
     0,     0,     0,   826,     0,     0,  2024,   826,     0,  2033,
  1005,   826,  1032,     0,  1132,   826,     0,  2026,     0,   826,
     0,     0,     0,   826,     0,     0,     0,   276,   276,     0,
     0,     0,     0,   276,     0,     0,     0,  2034,     0,     0,
     0,     0,     0,     0,   276,  2035,  2036,  2037,  2038,  2039,
  2040,  2041,     0,     0,   276,     0,  1064,     0,   276,   889,
     0,     0,     0,     0,     0,     0,  2027,     0,     0,   276,
     0,     0,  1133,  2028,     0,     0,     0,     0,     0,     0,
   815,  2029,     0,     0,     0,  1840,     0,     0,     0,   815,
     0,     0,  2030,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   815,     0,     0,     0,
     0,     0,     0,  2077,     0,     0,     0,     0,     0,     0,
     0,     0,  2077,  2077,  2077,  2077,  2077,  2077,  2077,  2077,
  2077,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   815,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   815,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1622,     0,     0,     0,     0,     0,     0,   276,  2032,  1625,
     0,     0,     0,  1840,   276,     0,     0,     0,  1626,     0,
     0,     0,     0,     0,  1041,  2033,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   815,     0,
   815,     0,     0,     0,     0,     0,     0,     0,     0,  1652,
     0,     0,     0,  2034,     0,     0,     0,     0,  1041,     0,
     0,  2035,  2036,  2037,  2038,  2039,  2040,  2041,  2077,  1665,
     0,     0,     0,  1673,   276,  1678,     0,     0,  1683,  1688,
  1693,  1698,  1703,  1708,  1713,  1718,     0,     0,  1061,     0,
     0,     0,  1032,  1032,  1032,  1032,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   276,     0,     0,     0,
     0,  1041,     0,     0,     0,     0,     0,     0,     0,  1064,
     0,     0,     0,  1064,  1064,  1064,  1064,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   276,     0,
   276,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   815,     0,     0,     0,     0,     0,   815,
     0,     0,     0,  1133,  1133,  1133,  1133,     0,     0,     0,
     0,     0,     0,     0,     0,  1453,     0,     0,     0,     0,
     0,  1133,   276,     0,     0,   276,     0,     0,     0,  1450,
     0,     0,     0,     0,     0,     0,     0,   815,     0,     0,
     0,  1463,     0,     0,     0,  1466,     0,   276,     0,  1469,
   276,   815,   815,  1472,   276,     0,     0,  1475,     0,     0,
     0,  1478,     0,     0,     0,  1481,     0,     0,     0,  1484,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   815,
   815,     0,     0,     0,     0,     0,     0,   815,     0,     0,
     0,   276,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1041,  1041,  1041,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   276,     0,
     0,     0,   815,     0,   815,  1041,  1041,  1041,     0,     0,
     0,   815,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1923,     0,     0,     0,     0,   826,     0,
     0,     0,     0,   826,     0,     0,     0,     0,   826,     0,
     0,     0,     0,   826,     0,     0,  1587,     0,   826,     0,
     0,     0,     0,   826,   276,     0,     0,     0,   826,     0,
     0,     0,     0,   826,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1956,     0,     0,     0,     0,
     0,     0,     0,  1032,  1032,  1041,  1041,  1041,     0,     0,
     0,     0,     0,  1032,  1032,  1032,  1032,  1032,  1032,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1032,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1655,     0,
     0,  1064,  1133,     0,     0,  1064,     0,  1064,     0,     0,
  1064,  1064,  1064,  1064,  1064,  1064,  1064,  1064,     0,     0,
  1064,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1133,     0,
     0,  1032,     0,     0,     0,     0,     0,     0,     0,     0,
  1133,  1133,  1133,  1133,  1133,  1133,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1759,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1756,     0,  1760,  1761,     0,  1763,  1764,     0,  1766,
  1767,     0,  1769,  1770,     0,  1772,  1773,     0,  1775,  1776,
     0,  1778,  1779,     0,  1781,  1782,   779,   780,   781,   782,
   783,   784,   785,   786,   787,     0,   788,     0,   789,   790,
   791,   792,   793,   794,   795,   796,   797,   798,     0,   799,
     0,   800,   801,   802,   803,   804,     0,   805,   806,   807,
   808,   809,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1841,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   546,
   547,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   200,   553,     0,     0,
     0,     0,     0,   810,     0,     0,     0,     0,     0,     0,
     0,   557,     0,   558,     0,     0,     0,     0,     0,  2149,
     0,   202,     0,     0,     0,     0,     0,   559,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   207,   208,     0,   560,     0,   561,     0,     0,     0,     0,
     0,   565,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   218,    73,  1453,     0,     0,     0,
     0,   811,   812,  1133,     0,  1064,     0,     0,     0,   571,
  1450,     0,     0,     0,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,  1927,     0,   813,     0,     0,  1930,
     0,     0,     0,     0,  1933,     0,     0,     0,     0,  1936,
     0,     0,     0,     0,  1939,     0,     0,     0,     0,  1942,
     0,     0,     0,     0,  1945,     0,     0,     0,     0,  1948,
     0,     0,     0,     0,     0,     0,     0,  1952,     0,     0,
     0,  1954,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1970,  1133,     0,   575,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   272,   273,     0,
   274,     0,     0,    25,   577,    26,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1841,     0,  1841,  1841,  1841,  1841,  1841,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2313,     0,     0,     0,     0,  2078,
     0,     0,     0,     0,     0,     0,     0,   779,   780,   781,
   782,   783,   784,   785,   786,   787,     0,   788,     0,   789,
   790,   791,   792,   793,   794,   795,   796,   797,   798,     0,
   799,     0,   800,   801,   802,   803,   804,     0,   805,   806,
   807,   808,   809,     0,  1133,     0,     0,     0,     0,     0,
  1759,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1756,     0,     0,  2108,  2109,     0,
  2111,  2112,     0,  2114,  2115,     0,  2117,  2118,     0,  2120,
  2121,     0,  2123,  2124,     0,  2126,  2127,     0,  2129,  2130,
     0,     0,     0,     0,     0,  2134,     0,   200,   553,  2137,
     0,  2143,     0,     0,   810,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   558,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,     0,     0,     0,   559,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   207,   208,     0,     0,  1841,     0,     0,     0,     0,
     0,     0,   565,  1841,  2168,     0,     0,  1841,     0,  1841,
     0,     0,  1841,  1841,  1841,  1841,  1841,  1841,  1841,  1841,
     0,  1841,     0,     0,     0,   218,  2078,     0,     0,     0,
     0,     0,   811,   812,     0,     0,     0,     0,     0,     0,
  1841,     0,     0,     0,     0,     0,     0,     0,     0,  2078,
  2078,  2078,  2078,  2078,   223,     0,     0,   813,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1133,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   272,     0,
     0,   274,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2168,     0,     0,
     0,     0,     0,  2168,     0,  1841,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  2078,     0,     0,     0,     0,     0,     0,
     0,     0,  2078,  2078,  2078,  2078,  2078,  2078,  2078,  2078,
  2078,     0,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,  2168,     0,     0,     0,     0,     0,
     0,  2168,     0,  1841,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   544,   545,   546,   547,     0,     0,   548,
     0,     0,     0,     0,     0,     0,   380,   549,   550,   551,
   552,     0,   200,   553,     0,     0,  2168,     0,  2168,   554,
     0,     0,     0,     0,     0,   555,   556,   557,  2078,   558,
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
   577,    26,     0,     0,     0,     0,     0,   578,  1190,     0,
   580,     0,   581,     0,     0,     0,     0,     0,   582,  1191,
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
     0,     0,     0,     0,     0,   578,  1720,     0,   580,     0,
   581,     0,     0,     0,     0,     0,   582,  1721,   513,   514,
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
     0,     0,     0,   578,     0,     0,   580,     0,   581,     0,
     0,     0,     0,     0,   582,  1643,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   544,   545,   546,
   547,     0,     0,   548,     0,     0,     0,     0,     0,     0,
   380,   549,   550,   551,   552,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,   555,
   556,   557,     0,   558,     0,     0,     0,     0,     0,     0,
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
     0,   578,     0,     0,   580,     0,   581,     0,     0,     0,
     0,     0,   582,  1751,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  2064,   546,   547,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  2065,
  2066,  2067,  2068,     0,   200,   553,     0,     0,     0,     0,
     0,   554,     0,     0,     0,     0,     0,     0,     0,   557,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   559,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,     0,     0,     0,   565,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,     0,     0,     0,     0,   568,
   569,   221,   222,     0,     0,     0,     0,   571,     0,     0,
     0,  2070,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   574,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,     0,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,   575,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,   273,  2071,   274,     0,
     0,    25,   577,    26,     0,     0,     0,     0,     0,  2072,
     0,     0,  2073,     0,  2074,     0,     0,     0,     0,     0,
  2075,  2298,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1831,   546,   547,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   200,   553,     0,     0,     0,     0,     0,   554,
     0,     0,     0,     0,     0,     0,     0,   557,     0,   558,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,   559,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,   560,
     0,   561,   209,     0,  1832,     0,  1833,   565,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   568,   569,   221,
   222,     0,     0,     0,     0,   571,     0,     0,     0,     0,
     0,     0,   573,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   574,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,   575,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,   273,  1834,   274,     0,     0,    25,
   577,    26,     0,     0,     0,     0,     0,  1835,     0,     0,
  1836,     0,  1837,     0,     0,     0,     0,     0,  1838,  2187,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   546,   547,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   200,   948,     0,     0,     0,     0,     0,   949,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   950,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,     0,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,     0,     0,     0,   951,   210,     0,   211,   212,
     0,     0,     0,     0,     0,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,     0,     0,     0,     0,   219,   220,   221,   222,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   226,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,   513,   514,   515,   516,   517,
   518,   519,   520,   521,     0,   522,     0,   523,   524,   525,
   526,   527,   528,   529,   530,   531,   532,   952,   533,     0,
   534,   535,   536,   537,   538,     0,   539,   540,   541,   542,
   543,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1037,     0,     0,   544,   545,   546,   547,
     0,     0,   548,     0,     0,     0,     0,     0,     0,   380,
   549,   550,   551,   552,     0,   200,   553,     0,     0,     0,
     0,     0,   554,     0,     0,     0,     0,     0,   555,   556,
   557,     0,   558,     0,     0,  1038,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   559,     0,     0,     0,
     0,   204,   205,  1039,     0,     0,     0,     0,   206,   207,
   208,     0,   560,     0,   561,   209,     0,   562,   563,   564,
   565,   210,     0,   211,   212,     0,     0,     0,     0,   566,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,   567,     0,     0,     0,
   568,   569,   221,   222,     0,  1040,     0,   570,   571,     0,
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
     0,   582,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1631,     0,     0,   544,   545,   546,   547,     0,     0,   548,
     0,     0,     0,     0,     0,     0,   380,   549,   550,   551,
   552,     0,   200,   553,     0,     0,     0,     0,     0,   554,
     0,     0,     0,     0,     0,   555,   556,   557,     0,   558,
     0,     0,  1038,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,   559,     0,     0,     0,     0,   204,   205,
  1632,     0,     0,     0,     0,   206,   207,   208,     0,   560,
     0,   561,   209,     0,   562,   563,   564,   565,   210,     0,
   211,   212,     0,     0,     0,     0,   566,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,   567,     0,     0,     0,   568,   569,   221,
   222,     0,  1633,     0,   570,   571,     0,     0,     0,   572,
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
   580,     0,   581,     0,     0,     0,     0,     0,   582,   513,
   514,   515,   516,   517,   518,   519,   520,   521,     0,   522,
     0,   523,   524,   525,   526,   527,   528,   529,   530,   531,
   532,     0,   533,     0,   534,   535,   536,   537,   538,     0,
   539,   540,   541,   542,   543,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1659,     0,     0,
   544,   545,   546,   547,     0,     0,   548,     0,     0,     0,
     0,     0,     0,   380,   549,   550,   551,   552,     0,   200,
   553,     0,     0,     0,     0,     0,   554,     0,     0,     0,
     0,     0,   555,   556,   557,     0,   558,     0,     0,  1038,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   559,     0,     0,     0,     0,   204,   205,  1660,     0,     0,
     0,     0,   206,   207,   208,     0,   560,     0,   561,   209,
     0,   562,   563,   564,   565,   210,     0,   211,   212,     0,
     0,     0,     0,   566,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
   567,     0,     0,     0,   568,   569,   221,   222,     0,  1661,
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
     0,     0,     0,     0,  1739,     0,     0,   544,   545,   546,
   547,     0,     0,   548,     0,     0,     0,     0,     0,     0,
   380,   549,   550,   551,   552,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,   555,
   556,   557,     0,   558,     0,     0,  1038,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,  1740,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,   562,   563,
   564,   565,   210,     0,   211,   212,     0,     0,     0,     0,
   566,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,   567,     0,     0,
     0,   568,   569,   221,   222,     0,  1741,     0,   570,   571,
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
   198,   199,     0,  1680,     0,     0,  1681,     0,     0,     0,
     0,     0,     0,     0,   544,  1044,   546,   547,     0,     0,
   548,     0,     0,     0,     0,     0,     0,   380,  1045,  1046,
  1047,  1048,     0,   200,   553,     0,     0,     0,     0,     0,
   554,     0,     0,     0,     0,     0,  1049,  1050,   557,     0,
   558,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   559,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   560,     0,   561,   209,     0,     0,   563,     0,   565,   210,
     0,   211,   212,     0,     0,     0,     0,  1052,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,  1053,     0,     0,     0,   568,   569,
   221,   222,     0,     0,     0,  1054,   571,     0,     0,     0,
   572,     0,     0,   573,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   574,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   575,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,-32768,   274,   275,     0,
    25,   577,    26,     0,     0,     0,     0,     0,  1056,     0,
     0,  1057, -1237,  1058,     0,     0,     0, -1237,     0,  1682,
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
     0,     0,     0,     0,     0,   578,   579,     0,   580,     0,
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
   563,     0,   565,   210,     0,   211,   212,     0,     0,     0,
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
     0,     0,     0,  1153,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,  1155,     0,     0,  1156,     0,     0,
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
  1157,   513,   514,   515,   516,   517,   518,   519,   520,   521,
     0,   522,     0,   523,   524,   525,   526,   527,   528,   529,
   530,   531,   532,     0,   533,     0,   534,   535,   536,   537,
   538,     0,   539,   540,   541,   542,   543,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1159,     0,     0,  1160,     0,     0,     0,     0,     0,
     0,     0,   544,   545,   546,   547,     0,     0,   548,     0,
     0,     0,     0,     0,     0,   380,   549,   550,   551,   552,
     0,   200,   553,     0,     0,     0,     0,     0,   554,     0,
     0,     0,     0,     0,   555,   556,   557,     0,   558,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   559,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   560,     0,
   561,   209,     0,   562,   563,     0,   565,   210,     0,   211,
   212,     0,     0,     0,     0,   566,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,   567,     0,     0,     0,   568,   569,   221,   222,
     0,     0,     0,   570,   571,     0,     0,     0,   572,     0,
     0,   573,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   574,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   575,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,   576,   274,   275,     0,    25,   577,
    26,     0,     0,     0,     0,     0,   578,     0,     0,   580,
     0,   581,     0,     0,     0,     0,     0,  1161,   513,   514,
   515,   516,   517,   518,   519,   520,   521,     0,   522,     0,
   523,   524,   525,   526,   527,   528,   529,   530,   531,   532,
     0,   533,     0,   534,   535,   536,   537,   538,     0,   539,
   540,   541,   542,   543,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,  1163,     0,
     0,  1164,     0,     0,     0,     0,     0,     0,     0,   544,
   545,   546,   547,     0,     0,   548,     0,     0,     0,     0,
     0,     0,   380,   549,   550,   551,   552,     0,   200,   553,
     0,     0,     0,     0,     0,   554,     0,     0,     0,     0,
     0,   555,   556,   557,     0,   558,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,   559,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,   560,     0,   561,   209,     0,
   562,   563,     0,   565,   210,     0,   211,   212,     0,     0,
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
     0,     0,     0,   578,     0,     0,   580,     0,   581,     0,
     0,     0,     0,     0,  1165,   513,   514,   515,   516,   517,
   518,   519,   520,   521,     0,   522,     0,   523,   524,   525,
   526,   527,   528,   529,   530,   531,   532,     0,   533,     0,
   534,   535,   536,   537,   538,     0,   539,   540,   541,   542,
   543,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,  1167,     0,     0,  1168,     0,
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
     0,  1169,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,  1171,     0,     0,  1172,     0,     0,     0,     0,
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
   580,     0,   581,     0,     0,     0,     0,     0,  1173,   513,
   514,   515,   516,   517,   518,   519,   520,   521,     0,   522,
     0,   523,   524,   525,   526,   527,   528,   529,   530,   531,
   532,     0,   533,     0,   534,   535,   536,   537,   538,     0,
   539,   540,   541,   542,   543,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,  1175,
     0,     0,  1176,     0,     0,     0,     0,     0,     0,     0,
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
     0,     0,     0,     0,     0,  1177,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   544,   545,   546,
   547,     0,     0,   548,     0,     0,     0,     0,     0,     0,
   380,   549,   550,   551,   552,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,   555,
   556,   557,     0,   558,     0,     0,  1038,     0,     0,     0,
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
     0,   578,     0,     0,   580,     0,   581,     0,     0,     0,
     0,     0,   582,   513,   514,   515,   516,   517,   518,   519,
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
   217,   218,    73,     0,   567,     0,     0,     0,   568,   569,
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
  1685,     0,     0,  1686,     0,     0,     0,     0,     0,     0,
     0,   544,  1044,   546,   547,     0,     0,   548,     0,     0,
     0,     0,     0,     0,   380,  1045,  1046,  1047,  1048,     0,
   200,   553,     0,     0,     0,     0,     0,   554,     0,     0,
     0,     0,     0,  1049,  1050,   557,     0,   558,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,   559,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,  1051,   563,     0,   565,   210,     0,   211,   212,
     0,     0,     0,     0,  1052,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,  1053,     0,     0,     0,   568,   569,   221,   222,     0,
     0,     0,  1054,   571,     0,     0,     0,   572,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   574,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,   381,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,   575,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,   273,  1055,   274,   275,     0,    25,   577,    26,
     0,     0,     0,     0,     0,  1056,     0,     0,  1057,     0,
  1058,     0,     0,     0,     0,     0,  1687,   513,   514,   515,
   516,   517,   518,   519,   520,   521,     0,   522,     0,   523,
   524,   525,   526,   527,   528,   529,   530,   531,   532,     0,
   533,     0,   534,   535,   536,   537,   538,     0,   539,   540,
   541,   542,   543,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,  1690,     0,     0,
  1691,     0,     0,     0,     0,     0,     0,     0,   544,  1044,
   546,   547,     0,     0,   548,     0,     0,     0,     0,     0,
     0,   380,  1045,  1046,  1047,  1048,     0,   200,   553,     0,
     0,     0,     0,     0,   554,     0,     0,     0,     0,     0,
  1049,  1050,   557,     0,   558,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   559,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   560,     0,   561,   209,     0,  1051,
   563,     0,   565,   210,     0,   211,   212,     0,     0,     0,
     0,  1052,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,  1053,     0,
     0,     0,   568,   569,   221,   222,     0,     0,     0,  1054,
   571,     0,     0,     0,   572,     0,     0,   573,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   574,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   575,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
  1055,   274,   275,     0,    25,   577,    26,     0,     0,     0,
     0,     0,  1056,     0,     0,  1057,     0,  1058,     0,     0,
     0,     0,     0,  1692,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,  1695,     0,     0,  1696,     0,     0,
     0,     0,     0,     0,     0,   544,  1044,   546,   547,     0,
     0,   548,     0,     0,     0,     0,     0,     0,   380,  1045,
  1046,  1047,  1048,     0,   200,   553,     0,     0,     0,     0,
     0,   554,     0,     0,     0,     0,     0,  1049,  1050,   557,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   559,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,  1051,   563,     0,   565,
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
     0,     0,   270,     0,   271,   272,   273,  1055,   274,   275,
     0,    25,   577,    26,     0,     0,     0,     0,     0,  1056,
     0,     0,  1057,     0,  1058,     0,     0,     0,     0,     0,
  1697,   513,   514,   515,   516,   517,   518,   519,   520,   521,
     0,   522,     0,   523,   524,   525,   526,   527,   528,   529,
   530,   531,   532,     0,   533,     0,   534,   535,   536,   537,
   538,     0,   539,   540,   541,   542,   543,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1700,     0,     0,  1701,     0,     0,     0,     0,     0,
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
     0,  1058,     0,     0,     0,     0,     0,  1702,   513,   514,
   515,   516,   517,   518,   519,   520,   521,     0,   522,     0,
   523,   524,   525,   526,   527,   528,   529,   530,   531,   532,
     0,   533,     0,   534,   535,   536,   537,   538,     0,   539,
   540,   541,   542,   543,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,  1705,     0,
     0,  1706,     0,     0,     0,     0,     0,     0,     0,   544,
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
     0,     0,     0,     0,  1707,   513,   514,   515,   516,   517,
   518,   519,   520,   521,     0,   522,     0,   523,   524,   525,
   526,   527,   528,   529,   530,   531,   532,     0,   533,     0,
   534,   535,   536,   537,   538,     0,   539,   540,   541,   542,
   543,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,  1710,     0,     0,  1711,     0,
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
     0,  1712,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,  1715,     0,     0,  1716,     0,     0,     0,     0,
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
  1057,     0,  1058,     0,     0,     0,     0,     0,  1717,   513,
   514,   515,   516,   517,   518,   519,   520,   521,     0,   522,
     0,   523,   524,   525,   526,   527,   528,   529,   530,   531,
   532,     0,   533,     0,   534,   535,   536,   537,   538,     0,
   539,   540,   541,   542,   543,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   544,   545,   546,   547,     0,     0,   548,     0,     0,     0,
     0,     0,     0,   380,   549,   550,   551,   552,     0,   200,
   553,     0,     0,     0,     0,     0,   554,     0,     0,     0,
     0,     0,   555,   556,   557,     0,   558,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   559,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   560,     0,   561,   209,
     0,   562,   563,   564,   565,   210,     0,   211,   212,     0,
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
     0,     0,     0,     0,     0,   582,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,  1147,     0,     0,  1148,
     0,     0,     0,     0,     0,     0,     0,   544,   545,   546,
   547,     0,     0,   548,     0,     0,     0,     0,     0,     0,
   380,   549,   550,   551,   552,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,   555,
   556,   557,     0,   558,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,     0,   563,
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
   268,   269,     0,     0,   270,     0,   271,   272,   273,-32768,
   274,   275,     0,    25,   577,    26,     0,     0,     0,     0,
     0,   578,     0,     0,   580,     0,   581,     0,     0,     0,
     0,     0,  1149,   513,   514,   515,   516,   517,   518,   519,
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
   560,     0,   561,   209,     0,   562,   563,     0,   565,   210,
     0,   211,   212,     0,     0,     0,     0,   566,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,    73,     0,   567,     0,     0,     0,   568,   569,
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
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
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
   581,     0,     0,     0,     0,     0,   582,   513,   514,   515,
   516,   517,   518,   519,   520,   521,     0,   522,     0,   523,
   524,   525,   526,   527,   528,   529,   530,   531,   532,     0,
   533,     0,   534,   535,   536,   537,   538,     0,   539,   540,
   541,   542,   543,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   544,  1044,
   546,   547,     0,     0,   548,     0,     0,     0,     0,     0,
     0,   380,  1045,  1046,  1047,  1048,     0,   200,   553,     0,
     0,     0,     0,     0,   554,     0,     0,     0,     0,     0,
  1049,  1050,   557,     0,   558,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   559,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   560,     0,   561,   209,     0,  1051,
   563,     0,   565,   210,     0,   211,   212,     0,     0,     0,
     0,  1052,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,  1053,     0,
     0,     0,   568,   569,   221,   222,     0,     0,     0,  1054,
   571,     0,     0,     0,   572,     0,     0,   573,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   574,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   575,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
  1055,   274,   275,     0,    25,   577,    26,     0,     0,     0,
     0,     0,  1056,     0,     0,  1057,     0,  1058,     0,     0,
     0,     0,     0,  1059,   513,   514,   515,   516,   517,   518,
   519,   520,   521,     0,   522,     0,   523,   524,   525,   526,
   527,   528,   529,   530,   531,   532,     0,   533,     0,   534,
   535,   536,   537,   538,     0,   539,   540,   541,   542,   543,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1116,   546,   547,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   380,  1117,
  1118,  1119,  1120,     0,   200,   553,     0,     0,     0,     0,
     0,   554,     0,     0,     0,     0,     0,     0,     0,   557,
     0,   558,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   559,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   560,     0,   561,   209,     0,     0,     0,     0,   565,
   210,     0,   211,   212,     0,     0,     0,     0,  1121,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,  1122,     0,     0,     0,   568,
   569,   221,   222,     0,     0,     0,  1123,   571,     0,     0,
     0,  1124,     0,     0,   573,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   574,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,   381,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,   575,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,   273,  1125,   274,   275,
     0,    25,   577,    26,     0,     0,     0,     0,     0,  1126,
     0,     0,  1127,     0,  1128,     0,     0,     0,     0,     0,
  1129,   513,   514,   515,   516,   517,   518,   519,   520,   521,
     0,   522,     0,   523,   524,   525,   526,   527,   528,   529,
   530,   531,   532,     0,   533,     0,   534,   535,   536,   537,
   538,     0,   539,   540,   541,   542,   543,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1116,   546,   547,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   380,  1117,  1118,  1119,  1120,
     0,   200,   553,     0,     0,     0,     0,     0,   554,     0,
     0,     0,     0,     0,     0,     0,   557,     0,   558,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   559,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   560,     0,
   561,   209,     0,     0,     0,     0,   565,   210,     0,   211,
   212,     0,     0,     0,     0,  1121,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,  1122,     0,     0,     0,   568,   569,   221,   222,
     0,     0,     0,  1123,   571,     0,     0,     0,  1124,     0,
     0,   573,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   574,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   575,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,-32768,   274,   275,     0,    25,   577,
    26,     0,     0,     0,     0,     0,  1126,     0,     0,  1127,
     0,  1128,     0,     0,     0,     0,     0,  1129,   513,   514,
   515,   516,   517,   518,   519,   520,   521,     0,   522,     0,
   523,   524,   525,   526,   527,   528,   529,   530,   531,   532,
     0,   533,     0,   534,   535,   536,   537,   538,     0,   539,
   540,   541,   542,   543,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2064,   546,   547,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  2065,  2066,  2067,  2068,     0,   200,   553,
     0,     0,     0,     0,     0,   554,     0,     0,     0,     0,
     0,     0,     0,   557,     0,   558,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,   559,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,   560,     0,   561,   209,     0,
     0,     0,  2069,   565,   210,     0,   211,   212,     0,     0,
     0,     0,     0,     0,     0,   213,   214,     0,     0,   215,
     0,   216,     0,     0,     0,   217,   218,     0,     0,     0,
     0,     0,     0,   568,   569,   221,   222,     0,     0,     0,
     0,   571,     0,     0,     0,  2070,     0,     0,   573,     0,
     0,     0,     0,     0,     0,   223,   224,   225,   574,     0,
   227,   228,     0,   229,   230,     0,   231,     0,     0,   232,
   233,   234,   235,   236,     0,   237,   238,     0,     0,   239,
   240,   241,   242,   243,   244,   245,   246,   247,     0,     0,
     0,     0,   248,     0,   249,   250,     0,     0,   251,   252,
     0,   253,     0,   254,     0,   255,   256,   257,   258,     0,
   259,     0,   260,   261,   262,   263,   264,   575,     0,   265,
   266,   267,   268,   269,     0,     0,   270,     0,   271,   272,
   273,  2071,   274,     0,     0,    25,   577,    26,     0,     0,
     0,     0,     0,  2072,     0,     0,  2073,     0,  2074,     0,
     0,     0,     0,     0,  2075,   513,   514,   515,   516,   517,
   518,   519,   520,   521,     0,   522,     0,   523,   524,   525,
   526,   527,   528,   529,   530,   531,   532,     0,   533,     0,
   534,   535,   536,   537,   538,     0,   539,   540,   541,   542,
   543,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2064,   546,   547,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2065,  2066,  2067,  2068,     0,   200,   553,     0,     0,     0,
     0,     0,   554,     0,     0,     0,     0,     0,     0,     0,
   557,     0,   558,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   559,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   560,     0,   561,   209,     0,     0,     0,     0,
   565,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,     0,     0,     0,     0,
   568,   569,   221,   222,     0,     0,     0,     0,   571,     0,
     0,     0,  2070,     0,     0,   573,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   574,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,     0,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   575,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,  2071,   274,
     0,     0,    25,   577,    26,     0,     0,     0,     0,     0,
  2072,     0,     0,  2073,     0,  2074,     0,     0,     0,     0,
     0,  2075,   513,   514,   515,   516,   517,   518,   519,   520,
   521,     0,   522,     0,   523,   524,   525,   526,   527,   528,
   529,   530,   531,   532,     0,   533,     0,   534,   535,   536,
   537,   538,     0,   539,   540,   541,   542,   543,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2064,   546,   547,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2065,  2066,  2067,
  2068,     0,   200,   553,     0,     0,     0,     0,     0,   554,
     0,     0,     0,     0,     0,     0,     0,   557,     0,   558,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,   559,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,   560,
     0,   561,   209,     0,     0,     0,     0,   565,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   568,   569,   221,
   222,     0,     0,     0,     0,   571,     0,     0,     0,  2070,
     0,     0,   573,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   574,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,   575,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,   273,-32768,   274,     0,     0,    25,
   577,    26,     0,     0,     0,     0,     0,  2072,     0,     0,
  2073,     0,  2074,     0,     0,     0,     0,     0,  2075,   513,
   514,   515,   516,   517,   518,   519,   520,   521,     0,   522,
     0,   523,   524,   525,   526,   527,   528,   529,   530,   531,
   532,     0,   533,     0,   534,   535,   536,   537,   538,     0,
   539,   540,   541,   542,   543,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1020,   546,   547,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   380,     0,     0,     0,     0,     0,   200,
   553,     0,     0,     0,     0,     0,   554,     0,     0,     0,
     0,     0,     0,     0,   557,     0,   558,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   559,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   560,     0,   561,   209,
     0,     0,     0,     0,   565,   210,     0,   211,   212,     0,
     0,     0,     0,  1021,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
  1022,     0,     0,     0,   568,   569,   221,   222,     0,     0,
     0,  1023,   571,     0,     0,     0,     0,     0,     0,   573,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   574,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   575,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,  1024,   274,     0,     0,    25,   577,    26,     0,
     0,     0,     0,     0,  1025,     0,     0,  1026,     0,     0,
     0,     0,     0,     0,     0,  1027,   513,   514,   515,   516,
   517,   518,   519,   520,   521,     0,   522,     0,   523,   524,
   525,   526,   527,   528,   529,   530,   531,   532,     0,   533,
     0,   534,   535,   536,   537,   538,     0,   539,   540,   541,
   542,   543,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1020,   546,
   547,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   380,     0,     0,     0,     0,     0,   200,   553,     0,     0,
     0,     0,     0,   554,     0,     0,     0,     0,     0,     0,
     0,   557,     0,   558,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   559,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,   560,     0,   561,   209,     0,     0,     0,
     0,   565,   210,     0,   211,   212,     0,     0,     0,     0,
  1021,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,  1022,     0,     0,
     0,   568,   569,   221,   222,     0,     0,     0,  1023,   571,
     0,     0,     0,     0,     0,     0,   573,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   574,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,   381,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,   575,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,-32768,
   274,     0,     0,    25,   577,    26,     0,     0,     0,     0,
     0,  1025,     0,     0,  1026,     0,     0,     0,     0,     0,
     0,     0,  1027,   513,   514,   515,   516,   517,   518,   519,
   520,   521,     0,   522,     0,   523,   524,   525,   526,   527,
   528,   529,   530,   531,   532,     0,   533,     0,   534,   535,
   536,   537,   538,     0,   539,   540,   541,   542,   543,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1831,   546,   547,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   200,   553,     0,     0,     0,     0,     0,
   554,     0,     0,     0,     0,     0,     0,     0,   557,     0,
   558,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   559,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   560,     0,   561,   209,     0,  1832,     0,  1833,   565,   210,
     0,   211,   212,     0,     0,     0,     0,     0,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,     0,     0,     0,     0,   568,   569,
   221,   222,     0,     0,     0,     0,   571,     0,     0,     0,
     0,     0,     0,   573,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   574,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,     0,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   575,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,  1834,   274,     0,     0,
    25,   577,    26,     0,     0,     0,     0,     0,  1835,     0,
     0,  1836,     0,  1837,     0,     0,     0,     0,     0,  1838,
   513,   514,   515,   516,   517,   518,   519,   520,   521,     0,
   522,     0,   523,   524,   525,   526,   527,   528,   529,   530,
   531,   532,     0,   533,     0,   534,   535,   536,   537,   538,
     0,   539,   540,   541,   542,   543,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1831,   546,   547,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   200,   553,     0,     0,     0,     0,     0,   554,     0,     0,
     0,     0,     0,     0,     0,   557,     0,   558,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,   559,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,     0,     0,  1833,   565,   210,     0,   211,   212,
     0,     0,     0,     0,     0,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,     0,     0,     0,     0,   568,   569,   221,   222,     0,
     0,     0,     0,   571,     0,     0,     0,     0,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   574,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,   575,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,   273,-32768,   274,     0,     0,    25,   577,    26,
     0,     0,     0,     0,     0,  1835,     0,     0,  1836,     0,
  1837,     0,     0,     0,     0,     0,  1838,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,     0,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   380,     0,     0,     0,     0,     0,   200,     0,     0,
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
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,     0,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,     0,
     0,   274,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   167,   168,   169,   170,   171,   172,   173,   174,
   175,     0,   176,  1488,   177,   178,   179,   180,   181,   182,
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
   218,    73,     0,     0,     0,     0,     0,   219,   220,   221,
   222,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   226,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,     0,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,     0,     0,   274,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,    91,   177,
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
     0,   260,   261,   262,   263,   264,  1589,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   491,   274,     0,     0,    25,     0,    26,     0,   465,   466,
   467,   468,  1590,   470,   471,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   200,     0,     0,   970,     0,
     0,     0,   201,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,     0,     0,     0,     0,
   462,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,     0,     0,     0,   209,     0,     0,     0,     0,
     0,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,     0,   213,   214,   463,     0,   215,     0,   216,     0,
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
   269,     0,     0,   270,     0,   271,   272,     0,   464,   274,
     0,     0,     0,     0,     0,     0,   465,   466,   467,   468,
   469,   470,   471,   167,   168,   169,   170,   171,   172,   173,
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
     0,   203,     0,     0,     0,     0,     0,     0,   462,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
     0,     0,     0,   209,     0,     0,     0,     0,     0,   210,
     0,   211,   212,     0,     0,     0,     0,     0,     0,     0,
   213,   214,   463,     0,   215,     0,   216,     0,     0,     0,
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
     0,   270,     0,   271,   272,     0,   464,   274,     0,     0,
     0,     0,     0,     0,   465,   466,   467,   468,   469,   470,
   471,   167,   168,   169,   170,   171,   172,   173,   174,   175,
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
     0,   271,   272,     0,     0,   274,     0,     0,     0,   577,
     0,     0,     0,     0,     0,     0,     0,   869,   167,   168,
   169,   170,   171,   172,   173,   174,   175,     0,   176,     0,
   177,   178,   179,   180,   181,   182,   183,   184,   185,   186,
     0,   187,     0,   188,   189,   190,   191,   192,     0,   193,
   194,   195,   196,   197,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   380,     0,     0,     0,     0,     0,   200,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,     0,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,     0,     0,     0,   209,     0,
     0,     0,     0,     0,   210,     0,   211,   212,     0,     0,
     0,     0,     0,     0,     0,   213,   214,     0,     0,   215,
     0,   216,     0,     0,     0,   217,   218,     0,     0,     0,
     0,     0,     0,   219,   220,   221,   222,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   223,   224,   225,   226,     0,
   227,   228,     0,   229,   230,     0,   231,     0,     0,   232,
   233,   234,   235,   236,     0,   237,   238,     0,     0,   239,
   240,   241,   242,   243,   244,   245,   246,   247,     0,     0,
     0,     0,   248,     0,   249,   250,     0,   381,   251,   252,
     0,   253,     0,   254,     0,   255,   256,   257,   258,     0,
   259,     0,   260,   261,   262,   263,   264,     0,     0,   265,
   266,   267,   268,   269,     0,     0,   270,     0,   271,   272,
     0,     0,   274,   167,   168,   169,   170,   171,   172,   173,
   174,   175,     0,   176,   395,   177,   178,   179,   180,   181,
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
     0,   270,     0,   271,   272,     0,     0,   274,   167,   168,
   169,   170,   171,   172,   173,   174,   175,     0,   176,   723,
   177,   178,   179,   180,   181,   182,   183,   184,   185,   186,
     0,   187,     0,   188,   189,   190,   191,   192,     0,   193,
   194,   195,   196,   197,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   200,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,     0,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,     0,     0,     0,   209,     0,
     0,     0,     0,     0,   210,     0,   211,   212,     0,     0,
     0,     0,     0,     0,     0,   213,   214,     0,     0,   215,
     0,   216,     0,     0,     0,   217,   218,     0,     0,     0,
     0,     0,     0,   219,   220,   221,   222,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   223,   224,   225,   226,     0,
   227,   228,     0,   229,   230,     0,   231,     0,     0,   232,
   233,   234,   235,   236,     0,   237,   238,     0,     0,   239,
   240,   241,   242,   243,   244,   245,   246,   247,     0,     0,
     0,     0,   248,     0,   249,   250,     0,     0,   251,   252,
     0,   253,     0,   254,     0,   255,   256,   257,   258,     0,
   259,     0,   260,   261,   262,   263,   264,     0,     0,   265,
   266,   267,   268,   269,     0,     0,   270,     0,   271,   272,
     0,     0,   274,   167,   168,   169,   170,   171,   172,   173,
   174,   175,     0,   176,  1186,   177,   178,   179,   180,   181,
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
     0,   270,     0,   271,   272,     0,     0,   274,   167,   168,
   169,   170,   171,   172,   173,   174,   175,     0,   176,  1502,
   177,   178,   179,   180,   181,   182,   183,   184,   185,   186,
     0,   187,     0,   188,   189,   190,   191,   192,     0,   193,
   194,   195,   196,   197,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   198,   199,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   200,     0,
     0,     0,     0,     0,     0,   201,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,   203,     0,     0,     0,
     0,     0,     0,     0,   204,   205,     0,     0,     0,     0,
     0,   206,   207,   208,     0,     0,     0,     0,   209,     0,
     0,     0,     0,     0,   210,     0,   211,   212,     0,     0,
     0,     0,     0,     0,     0,   213,   214,     0,     0,   215,
     0,   216,     0,     0,     0,   217,   218,     0,     0,     0,
     0,     0,     0,   219,   220,   221,   222,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   223,   224,   225,   226,     0,
   227,   228,     0,   229,   230,     0,   231,     0,     0,   232,
   233,   234,   235,   236,     0,   237,   238,     0,     0,   239,
   240,   241,   242,   243,   244,   245,   246,   247,     0,     0,
     0,     0,   248,     0,   249,   250,     0,     0,   251,   252,
     0,   253,     0,   254,     0,   255,   256,   257,   258,     0,
   259,     0,   260,   261,   262,   263,   264,     0,     0,   265,
   266,   267,   268,   269,     0,     0,   270,     0,   271,   272,
     0,     0,   274,   779,   780,   781,   782,   783,   784,   785,
   786,   787,     0,   788,  1805,   789,   790,   791,   792,   793,
   794,   795,   796,   797,   798,     0,   799,     0,   800,   801,
   802,   803,   804,     0,   805,   806,   807,   808,   809,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   546,   547,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   200,   553,     0,     0,     0,     0,     0,
   810,     0,     0,     0,     0,     0,     0,     0,   557,     0,
   558,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,     0,     0,     0,   559,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   207,   208,     0,
   560,     0,   561,     0,     0,     0,     0,     0,   565,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   218,     0,     0,     0,     0,     0,     0,   811,   812,
     0,     0,     0,     0,     0,     0,   571,     0,     0,     0,
     0,     0,     0,   573,     0,     0,     0,     0,     0,     0,
   223,     0,     0,   813,     0,     0,   167,   168,   169,   170,
   171,   172,   173,   174,   175,     0,   176,     0,   177,   178,
   179,   180,   181,   182,   183,   184,   185,   186,     0,   187,
     0,   188,   189,   190,   191,   192,     0,   193,   194,   195,
   196,   197,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   575,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   272,   273,     0,   274,     0,     0,
    25,   577,    26,     0,     0,     0,     0,     0,     0,     0,
   380,     0,     0,     0,     0,     0,   200,     0,     0,     0,
     0,     0,     0,   201,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,     0,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,     0,     0,     0,   209,     0,     0,     0,
     0,     0,   210,     0,   211,   212,     0,     0,     0,     0,
     0,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,     0,     0,     0,
     0,   219,   220,   221,   222,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   226,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,   381,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,     0,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,     0,     0,
   274,     0,     0,     0,   577,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,   293,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   294,
     0,     0,     0,     0,     0,   200,     0,     0,   295,     0,
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
     0,   200,     0,     0,   429,     0,     0,     0,   201,     0,
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
   295,     0,     0,     0,   201,     0,     0,     0,     0,     0,
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
     0,   270,     0,   271,   272,   273,     0,   274,   275,   167,
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
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,     0,     0,   274,   275,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1075,     0,     0,     0,
     0,     0,  1076,     0,     0,     0,  1077,     0,     0,  1078,
     0,     0,     0,     0,     0,   200,     0,     0,     0,     0,
     0,     0,   201,     0,  1079,  1080,     0,     0,     0,     0,
  1081,     0,     0,     0,  1082,     0,     0,     0,  1083,     0,
   202,     0,     0,   203,     0,     0,     0,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,     0,     0,     0,   209,     0,     0,  1084,     0,
     0,   210,     0,   211,   212,     0,  1085,     0,     0,  1086,
  1087,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,     0,  1088,     0,  1089,
   219,   220,   221,   222,     0,     0,  1090,     0,  1091,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1092,     0,
     0,     0,   223,   224,   225,   226,  1093,   227,   228,  1094,
   229,   230,  1095,   231,  1096,  1097,   232,   233,   234,   235,
   236,  1098,   237,   238,  1099,  1100,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,  1101,     0,  1102,   248,
  1103,   249,   250,  1104,  1105,   251,   252,  1106,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,  1107,   260,
   261,   262,   263,   264,  1108,  1109,   265,   266,   267,   268,
   269,     0,  1110,   270,  1111,   271,   272,     0,     0,   274,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   546,   547,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   200,   948,     0,     0,     0,     0,     0,   949,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   950,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,     0,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,   560,     0,   561,
   209,     0,     0,     0,     0,   951,   210,     0,   211,   212,
     0,     0,     0,     0,     0,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,     0,     0,     0,     0,   219,   220,   221,   222,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   226,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1250,     0,     0,     0,     0,     0,  1285,     0,     0,     0,
     0,     0,     0,     0,     0,   200,     0,     0,     0,     0,
     0,     0,   201,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1252,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,     0,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,     0,     0,     0,   209,     0,     0,     0,     0,
     0,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,  1253,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,     0,     0,     0,     0,
   219,   220,   221,   222,     0,     0,     0,     0,     0,     0,
  1254,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   226,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,     0,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,     0,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
   167,   168,   169,   170,   171,   375,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   200,     0,     0,     0,     0,     0,     0,   201,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,     0,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,   376,     0,     0,     0,
   209,     0,     0,     0,     0,     0,   210,     0,   211,   212,
     0,     0,     0,     0,     0,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,     0,     0,     0,     0,   377,   220,   221,   222,     0,
     0,   378,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   226,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,   167,   168,   169,   170,   171,
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
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   200,     0,     0,     0,     0,     0,     0,   201,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1307,     0,   202,     0,     0,   203,     0,
     0,     0,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,     0,     0,     0,
   209,     0,     0,     0,     0,     0,   210,     0,   211,   212,
     0,     0,     0,     0,     0,     0,     0,   213,   214,     0,
  1308,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,     0,     0,     0,     0,   219,   220,   221,   222,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   226,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   200,     0,     0,     0,     0,
     0,     0,   201,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,     0,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,     0,     0,     0,   209,     0,     0,     0,     0,
     0,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,     0,     0,     0,     0,
   403,   220,   221,   222,     0,     0,   404,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   226,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,     0,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,     0,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   200,     0,     0,     0,     0,     0,     0,   201,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,   203,     0,
     0,     0,     0,     0,     0,     0,   204,   205,     0,     0,
     0,     0,     0,   206,   207,   208,     0,     0,     0,     0,
   209,     0,     0,     0,     0,     0,   210,     0,   211,   212,
     0,     0,     0,     0,     0,     0,     0,   213,   214,     0,
     0,   215,     0,   216,     0,     0,     0,   217,   218,     0,
     0,     0,     0,     0,     0,   406,   220,   221,   222,     0,
     0,   407,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   223,   224,   225,
   226,     0,   227,   228,     0,   229,   230,     0,   231,     0,
     0,   232,   233,   234,   235,   236,     0,   237,   238,     0,
     0,   239,   240,   241,   242,   243,   244,   245,   246,   247,
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
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
     0,   249,   250,     0,     0,   251,   252,   979,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,     0,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
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
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,  1607,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,  1800,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
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
     0,   249,   250,     0,     0,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,     0,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
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
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
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
     0,   249,   250,     0,     0,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,     0,     0,   265,   335,   267,   268,
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
   167,   168,   694,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
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
     0,     0,     0,     0,   248,     0,   249,   250,     0,     0,
   251,   252,     0,   253,     0,   254,     0,   255,   256,   257,
   258,     0,   259,     0,   260,   261,   262,   263,   264,     0,
     0,   265,   266,   267,   268,   269,     0,     0,   270,     0,
   271,   272,     0,     0,   274,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   200,     0,     0,     0,     0,
     0,     0,   201,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,     0,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,     0,     0,     0,   209,     0,     0,     0,     0,
     0,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,     0,   213,   214,     0,     0,  1196,     0,   216,     0,
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
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
   779,   780,   781,   782,   783,   784,   785,   786,   787,     0,
   788,     0,   789,   790,   791,   792,   793,   794,   795,   796,
   797,   798,     0,   799,     0,   800,   801,   802,   803,   804,
     0,   805,   806,   807,   808,   809,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   546,   547,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   200,   553,     0,     0,     0,     0,     0,   810,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   558,     0,     0,
     0,     0,     0,     0,     0,   202,     0,     0,     0,     0,
     0,   559,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   207,   208,     0,   560,     0,   561,
     0,     0,     0,     0,     0,   565,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   218,     0,
     0,     0,     0,     0,     0,   811,   812,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   573,     0,     0,     0,     0,     0,     0,   223,     0,     0,
   813,     0,   779,   780,   781,   782,   783,   784,   785,   786,
   787,     0,   788,     0,   789,   790,   791,   792,   793,   794,
   795,   796,   797,   798,     0,   799,     0,   800,   801,   802,
   803,   804,     0,   805,   806,   807,   808,   809,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1585,     0,   575,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   272,     0,     0,   274,   546,   547,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   200,   553,     0,     0,     0,     0,     0,   810,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   558,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
     0,     0,     0,   559,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   207,   208,     0,   560,
     0,   561,     0,     0,     0,     0,     0,   565,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   218,     0,     0,     0,     0,     0,     0,   811,   812,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   573,     0,     0,     0,     0,     0,     0,   223,
     0,     0,   813,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   575,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   272,     0,     0,   274
};

static const short yycheck[] = {     1,
    91,     1,     1,    56,   350,   435,   624,   310,    49,    50,
   333,    52,    53,    54,    55,   596,   832,   320,   510,    60,
   347,    49,    63,    76,    52,   987,    67,   834,  1244,  1496,
    50,   165,   930,    74,    75,  1267,   339,    78,    49,  1525,
  1488,    82,    83,   325,   708,    42,    87,    88,    89,  1241,
    42,  1821,  1428,    42,    19,    27,    84,    42,   374,   432,
     4,   547,   344,  1553,    17,   113,   111,   113,    12,   125,
    63,  2026,    19,    66,    96,   561,   307,    54,    62,    18,
    64,   438,   752,   172,    59,    29,    63,   191,    97,    60,
    63,    35,    36,    96,   102,    92,   191,   172,    40,   249,
    92,    43,   182,    92,    46,   775,   274,    92,   172,    51,
   393,    53,    54,    30,  2326,    32,    96,   101,   191,  1125,
  1126,  1127,  1128,  1024,  1025,  1026,  1027,   176,   192,    95,
   214,    59,    76,    33,   258,    78,   198,  1143,   818,    82,
    40,   265,   101,    43,    23,   191,    46,   191,   298,    59,
   172,    51,   191,    53,    54,   135,   100,   121,    96,   243,
    96,    96,   508,   744,  2376,   511,   110,   280,    63,   112,
   192,  1037,   113,  1039,  1040,    41,    42,   104,    44,    45,
   189,    47,    48,    63,    50,   855,    52,   300,   196,    55,
    56,    57,    58,   476,   298,   166,   123,    59,     6,   274,
   191,     9,   277,   298,   284,   300,   170,    15,    16,   147,
   299,   703,    59,   275,   173,    59,   159,   448,   184,    99,
  2175,    59,   280,    31,   299,   298,    34,   300,   145,   278,
    97,   114,   113,   199,   296,   299,    78,   208,   191,   297,
   125,   182,   298,   187,   285,   188,   210,   292,   143,   193,
   298,   292,   298,   224,   298,   122,   300,   285,    59,   298,
   187,   300,   619,   175,   126,   160,   582,    59,   309,   310,
   112,   255,   275,   156,   285,   191,   164,   299,   190,   320,
   321,   322,   768,   274,   325,   326,   277,   328,   329,   870,
    59,   210,   333,   874,   335,   275,   337,   338,   339,   672,
   341,   189,   343,   344,   345,   172,   299,   280,   281,   350,
   351,  2266,    59,   278,   289,   290,    73,   159,  1125,  1126,
  1127,  1128,   189,  1539,   415,  1541,   298,   292,   369,   759,
   274,   278,   297,   353,   375,   274,  1143,   290,   277,   275,
   275,  1789,  1534,    59,   321,   322,   188,   375,   376,   390,
   272,   328,   393,  1843,  1844,  1731,   299,   359,   302,   400,
   302,   289,   290,  2318,   341,  2320,   343,   215,  1038,   231,
   423,  1051,   119,   300,   351,  1055,  1056,  1057,  1058,   289,
   290,   275,   298,   424,   300,   280,   248,   144,  1394,    59,
  2160,   432,   433,   288,   175,   392,   244,   137,   439,   401,
   392,   442,   302,   392,   445,   233,   280,   392,   274,   990,
   126,   219,   432,   228,   276,    70,   173,   437,  1885,   460,
    27,   249,    75,   297,  1430,   287,   288,   289,   290,    82,
    83,   171,    59,   150,    87,   476,  1442,  1443,  1444,  1445,
  1446,  1447,   289,   290,  1345,   289,   290,   488,   275,  1030,
   274,   289,   290,   277,  1355,  1356,  1357,  1358,  1359,  1360,
    70,   182,   439,   280,   119,   442,   494,   508,   445,   296,
   511,   512,  1346,   280,   191,   276,    97,  1293,  1347,   134,
   297,  1062,  1348,   975,   276,   915,   287,   288,   289,   290,
   297,    33,   284,   285,   286,   287,   288,   289,   290,   300,
    97,   122,   504,   544,  1320,    97,  1380,   276,   300,   119,
   275,   488,  1381,   280,    70,   231,  1382,   276,   287,   288,
   289,   290,   108,   292,   134,   122,   275,   108,   297,   276,
   122,   296,   248,   300,   575,    24,   852,   275,  1354,   292,
   287,   288,   289,   290,   297,  1993,   637,   296,   639,   278,
  1131,   104,  2000,   274,   275,   583,   277,   278,   296,  1433,
   276,   147,   191,   119,   145,  1434,   147,  1185,   189,  1435,
   123,   287,   288,   289,   290,   172,  1392,   618,   134,   620,
    59,   622,  1200,  1959,   113,   113,   614,  1394,   616,   617,
   119,   632,   189,   646,   275,   118,   280,   189,   651,   652,
   186,   154,  2092,   205,   288,   186,   276,   209,  1272,  1825,
   296,  1827,  1276,   299,   137,   296,   292,   287,   288,   289,
   290,   297,   292,  1430,  1856,  1441,   247,   297,   910,   670,
   126,   672,   935,   275,   187,  1442,  1443,  1444,  1445,  1446,
  1447,   292,   670,   275,    13,   622,   297,   738,   171,   276,
   247,   692,   672,   275,   296,    26,   309,   266,   267,   670,
   287,   288,   289,   290,   296,   667,   275,   708,   108,  1675,
    59,   987,  2158,   714,   296,   175,   329,  1347,    67,   108,
   721,   722,  1580,   253,   337,   713,   727,    59,   275,   228,
   731,   732,  1362,  1363,    59,   130,  2144,   738,  2146,  1051,
    65,  1371,   299,  1055,  1056,  1057,  1058,   147,  1272,   296,
   275,  1381,  1276,  1393,   275,   692,   369,  1397,   147,  1399,
   161,   374,  1402,  1403,  1404,  1405,  1406,  1407,  1408,  1409,
   184,   296,  1412,  2223,  2201,   296,   298,   390,   300,  1555,
   275,  1411,   275,  1059,   721,   199,   186,   400,   150,  1755,
   727,   753,   754,   755,   731,   732,   275,   186,   760,  2226,
   298,   296,   108,   296,  1434,  1631,  1632,  1633,   275,    95,
   772,    97,   906,   907,   295,   292,    59,   296,   299,  1449,
   297,  1832,    65,  1834,  1835,  1836,  1837,  1838,   150,   296,
   831,   832,   150,  1659,  1660,  1661,   122,   276,  2014,   145,
  2016,   147,   277,   278,   275,    59,   114,   460,   287,   288,
   289,   290,   120,   854,   275,   856,   857,   275,   275,    20,
    21,  1313,   275,   864,   132,   296,   828,    28,   830,   275,
   275,   275,  1413,  1149,     4,   296,   127,  1153,   296,   296,
   186,  1157,    12,   296,    59,  1161,   172,   155,    59,  1165,
   296,   296,   296,  1169,   201,   275,    67,  1173,  2048,    29,
   168,  1177,   173,   189,    59,    35,    36,   908,  1675,   910,
    65,   912,   126,  1739,  1740,  1741,   296,   126,    20,    21,
   857,  2071,  2072,  2073,  2074,  2075,    28,   276,   275,   150,
   892,   893,   912,   127,   935,   284,   285,   286,   287,   288,
   289,   290,   292,   905,   276,   905,    76,   297,   275,   296,
  1916,   276,   284,   285,   286,   287,   288,   289,   290,   921,
   922,   247,   287,   288,   289,   290,   967,   201,  1890,   296,
   100,   275,   275,   974,   975,    59,   275,    63,   275,   296,
   110,   201,   299,   275,   985,   127,    70,  1377,  1755,   207,
   991,   293,   296,   296,    59,   996,   298,   296,   960,   296,
   962,   963,   964,   965,   296,   618,  2017,   620,    27,  1296,
  1297,    97,  1295,   299,  2025,    63,  1646,   231,  2029,   632,
  2031,    78,   275,  2034,  2035,  2036,  2037,  2038,  2039,  2040,
  2041,    97,  2043,   276,   248,   119,   122,  1677,   275,    96,
   275,  1317,   126,   296,   287,   288,   289,   290,   292,  1325,
   134,  2062,   298,   297,   300,  2205,   122,   187,   275,   296,
    72,   296,   276,   193,  2214,  2215,  2216,  2217,  2218,  2219,
  2220,  2221,  2222,   287,   288,   289,   290,  1793,   111,   296,
  1796,  1393,    93,   103,    59,  1397,   172,  1399,   145,   276,
  1402,  1403,  1404,  1405,  1406,  1407,  1408,  1409,   285,   286,
  1412,   276,   159,   189,   292,   276,   172,   118,   165,   722,
   285,   286,   287,   288,   289,   290,   287,   288,   289,   290,
  1896,   276,   298,   189,   300,    72,   137,   138,    67,  1905,
   108,   188,   287,   288,   289,   290,  1137,   931,   932,   933,
  2106,    61,    49,    50,   274,    52,    66,   231,   298,  1916,
   300,   292,    72,    78,    59,   274,   297,    77,   277,    84,
   171,   280,   289,   282,   248,    70,  2177,   145,    42,   147,
    59,    96,   292,   259,    48,   295,    50,   297,    52,   299,
  2330,    70,   301,  1184,  1960,   292,   298,   182,   300,   200,
   297,    93,   276,   259,   292,  1196,   295,   296,  1199,   297,
  1201,   285,   286,   287,   288,   289,   290,   292,   186,   198,
  1211,   276,   297,   280,   119,   282,   118,  1218,  1231,  1207,
   145,   126,   287,   288,   289,   290,    27,  1503,   292,   134,
   119,   280,   281,   297,   159,   137,   138,   126,   292,   852,
   165,   198,   292,   297,    59,   134,   292,   297,  2024,   299,
   292,   297,    67,   299,  1255,   297,  1257,  1258,  1259,  1260,
   298,   292,   300,   188,  2275,  1266,   297,   544,    59,   171,
  1232,  1272,  1232,  1232,  1211,  1276,    59,    67,   180,   181,
  1268,  1218,    65,   299,  1285,   292,  1248,  1249,  1248,  1249,
   297,    67,  1293,   130,  1295,  1296,  1297,  1298,   200,   299,
   292,   276,  1303,   150,  1305,   297,  1307,    92,  1296,  1297,
   197,  1312,   287,   288,   289,   290,  1278,   211,   170,  1320,
  1321,  1322,   216,  1260,   298,   960,   231,   962,   963,   964,
   965,   299,    97,   227,  1296,  1297,  1296,  1297,   299,  2106,
  1616,   618,   231,   248,   292,   239,   240,   292,  1285,   297,
   292,   292,   297,  1354,   280,   297,   297,   122,  1810,   248,
   298,   974,   300,   194,   292,  1677,  1303,  1329,  1305,   297,
   264,   276,  2002,    77,   987,   292,   278,   284,   285,    67,
   297,  1343,   287,   288,   289,   290,   299,   276,   292,   292,
   298,  1392,   300,   297,   297,   284,   285,   286,   287,   288,
   289,   290,   299,   298,   292,   300,  1682,   172,   299,   297,
  1951,  1687,  1374,  1375,   274,   275,  1692,   277,  1519,  1520,
   280,  1697,   282,   292,   189,   292,  1702,   119,   297,   292,
   297,  1707,   158,   292,   297,   299,  1712,  2213,   297,   274,
  1441,  1717,   277,   292,   299,   280,   353,   282,   297,    59,
   296,  2227,  2228,   145,   292,    65,   651,   652,  1459,   297,
    70,   276,   292,   298,  1426,   300,   292,   297,   375,  1431,
  1432,   297,   287,   288,   289,   290,   299,   298,   292,   300,
  2058,   246,   247,   297,  1485,   276,    86,  1488,   298,  2265,
   300,   299,  1493,   276,   299,  1496,   287,   288,   289,   290,
   298,   296,   300,   292,   287,   288,   289,   290,   297,   119,
   292,   299,   292,   292,   114,   297,   126,   297,   297,   133,
   120,   292,   292,   295,   134,   432,   297,   297,   128,  1530,
   437,   296,   132,   292,   299,   145,   299,   298,   297,   300,
   292,   141,     3,   153,    86,   297,     7,   295,   299,    10,
    11,  2327,  1553,    14,  1555,   155,   299,  1519,  1520,  1521,
   292,    22,    23,   299,   292,   297,   299,   198,   168,   297,
   119,  1184,   114,   133,  1536,  1537,    37,    38,   120,   133,
  1581,  1569,  1583,   298,   276,   300,  1199,  1850,  1589,   298,
   132,   300,   284,   285,   286,   287,   288,   289,   290,   141,
  1601,   274,   189,    64,   277,   292,   292,   280,    69,   282,
   297,   297,   292,   155,  1890,   292,    95,   297,    79,   301,
   297,   231,    83,   301,    85,   298,   168,   300,  1590,   298,
  1590,   300,   292,   301,    95,    59,    97,   297,   248,   298,
   101,   300,   103,   295,   105,   187,    70,   292,   109,   190,
    33,   292,   297,  1266,   115,   290,   297,    40,   298,   292,
    43,   122,   299,    46,   297,    27,   276,  1668,    51,   299,
    53,    54,    59,   252,   284,   285,   286,   287,   288,   289,
   290,   292,   237,    70,   292,   257,   297,   292,   292,   297,
   300,   150,   297,   297,   111,   119,   274,   275,   299,   277,
   278,   608,   126,   125,   182,   276,   167,   298,   169,   300,
   134,   172,   173,   284,   285,   286,   287,   288,   289,   290,
   192,   298,   298,   300,   300,   298,   196,   300,   189,   298,
   295,   300,   119,   299,   113,    77,   295,   172,   182,   126,
   182,   182,   203,   204,   182,   178,   198,   134,    59,   300,
   300,   212,   213,  1754,  2060,   298,   113,   295,   145,   299,
    59,   222,   223,   670,   250,   672,    65,   274,   299,   298,
   176,    70,   149,   234,   235,   236,    26,   238,   289,   301,
   241,   301,   188,    25,   190,    82,   247,   150,  1789,   195,
   284,   221,   153,   158,   262,   256,   202,   203,   150,  1800,
   206,    67,   263,   226,   284,   298,   298,   231,   284,   270,
   300,   217,  1813,   173,    81,   300,   300,   300,   300,   225,
   119,   113,   228,   300,   248,   171,   138,   126,   299,   201,
   299,   284,   299,   299,   299,   134,   300,   300,   299,   296,
   300,   299,  1843,  1844,   231,   251,   145,   253,  1849,  1850,
   299,   299,   276,   259,   153,   261,   299,   297,   295,  1860,
  1137,   248,   268,   287,   288,   289,   290,   299,   299,  1870,
   299,  1859,  1485,  1874,  2326,  1488,  1813,    92,   299,    59,
   300,   299,   233,   299,  1885,    65,   299,   299,   299,   276,
    70,   299,  1880,   300,   299,  1896,   299,   284,   285,   286,
   287,   288,   289,   290,  1905,   299,   299,   299,   299,   299,
   299,   818,  1849,   299,   299,   299,   274,   299,   299,   299,
   298,  1922,  1199,   299,   299,   299,   258,   834,    27,   302,
  2193,   302,   231,  1870,   300,   276,   129,   129,   297,   119,
   119,   284,   198,   299,   851,   299,   126,   191,   111,   248,
   857,   198,   103,    59,   134,   111,   300,    59,   119,  1960,
   300,   300,   300,   299,   299,   145,    59,   300,   298,   300,
   298,   300,  1973,   153,   300,   300,   300,   276,   300,   300,
   300,   289,   116,   191,   111,   284,   285,   286,   287,   288,
   289,   290,  1993,   274,   274,   298,   302,  2010,   302,  2000,
   182,   300,   116,   299,   299,   912,   271,   111,   153,    67,
   182,   229,   159,   232,   299,   115,   299,  2005,   300,   128,
   297,   300,   300,  2024,   300,  2026,   300,   300,   300,   300,
   300,   300,   300,   155,   128,   297,   300,   300,   299,   299,
   299,   299,   299,    72,   299,   299,  2008,   299,  2008,   300,
   299,   231,   300,   299,   299,    49,   299,   298,    52,  2060,
    54,   299,   299,   299,  2052,   299,    60,    59,   248,    63,
   299,   299,   299,    65,   300,   300,   300,   984,    70,   300,
    74,    75,   117,   300,    78,   218,   300,   300,    82,    83,
   300,  2092,   297,    87,    88,    89,   276,   300,   300,   300,
   300,   300,   300,   300,   284,   285,   286,   287,   288,   289,
   290,   300,   300,   300,   300,   300,   300,   300,   284,   274,
   300,    96,   300,   299,   299,   299,    96,   119,   269,   220,
   299,   105,   129,   147,   126,   299,   129,   149,   151,   128,
   300,   152,   134,  2144,  1051,  2146,   300,   300,  1055,  1056,
  1057,  1058,   300,   145,   300,   128,   300,   300,   300,    59,
   300,  1068,   300,   300,   300,   300,   162,   299,  2169,   300,
   300,   300,   300,   130,  2175,   300,  1789,   300,    65,   300,
   300,   298,  1459,   295,   299,   165,   299,   219,   299,   299,
   298,  2153,  2193,  2153,  2156,    59,  2156,  2198,   299,    59,
  2201,    65,   300,   299,   136,   300,    70,    65,   300,   298,
   300,  2199,  2213,   300,   300,   300,   300,   300,  1125,  1126,
  1127,  1128,  2223,   300,   300,  2226,  2227,  2228,  2229,  2230,
   300,   300,   300,   300,   300,   300,  1143,   300,   300,   231,
   300,   300,   299,    59,  2206,  2207,   300,  1860,   300,    65,
   300,   299,   299,   230,    70,   119,   248,   300,   300,   150,
   300,  2198,   126,   300,  2265,  2266,   297,   293,    61,   293,
   134,     0,  2273,  1550,     0,  1182,  2277,  1890,   300,    92,
  1265,   145,   452,   702,   276,   939,  1255,  1563,   851,   153,
   617,   285,   284,   285,   286,   287,   288,   289,   290,  2261,
  1581,  2261,  2261,   119,  1257,  2356,  1877,  2308,  2063,  2273,
   126,  2347,  2372,  2314,  2337,   309,  1583,  2318,   134,  2320,
  2367,  2373,  1571,  2324,    86,  1298,  2327,   321,   322,   145,
  1297,   325,   326,   626,   328,   329,  2324,   153,    71,   333,
   401,   509,   991,   337,   338,  2226,  2339,   341,  2369,   343,
   344,   345,  2225,  2356,  1569,    90,   350,   351,   392,   347,
  1218,  1312,  2324,   879,  2324,  2323,   490,   231,  2369,  2370,
  1302,  1213,  2373,  1179,   418,   369,  1521,  2230,  1488,  1794,
  1993,   375,   871,  2370,   248,  2373,  1797,  2000,  1729,  1377,
  1668,  1668,  1199,  1922,   575,   993,   390,   852,  2308,   393,
   856,  1800,   281,   908,   427,   915,   400,  1524,   912,   375,
  1203,  2373,   276,  2373,  2160,   231,   984,   164,   500,  2153,
   284,   285,   286,   287,   288,   289,   290,   929,   875,  1182,
   424,  1486,   248,  2156,    -1,    -1,   300,    -1,    -1,   433,
    -1,    -1,    -1,    -1,    -1,   439,    -1,    -1,   442,    -1,
    -1,   445,    -1,    59,    -1,    -1,    -1,    -1,    -1,    -1,
   276,    -1,    -1,    -1,    70,    -1,   460,    -1,   284,   285,
   286,   287,   288,   289,   290,    -1,    -1,  1754,    -1,    -1,
    -1,    -1,   476,    -1,   300,    -1,  1393,  1394,    -1,    -1,
  1397,    -1,  1399,    -1,   488,  1402,  1403,  1404,  1405,  1406,
  1407,  1408,  1409,    -1,    -1,  1412,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   119,   508,    -1,    -1,   511,   512,    -1,
   126,    -1,    -1,  1430,    -1,    -1,    -1,    -1,   134,    -1,
    -1,  2144,    -1,  2146,    -1,  1442,  1443,  1444,  1445,  1446,
  1447,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   544,    -1,    -1,    -1,    -1,  1832,    -1,  1834,  1835,  1836,
  1837,  1838,    -1,    33,    -1,    -1,    -1,    -1,    -1,    39,
    40,    41,    42,    43,    44,    45,    46,    47,    48,  1486,
    50,    51,    52,    53,    54,    55,    56,    57,    58,    -1,
    -1,   175,    -1,    -1,  1871,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   190,    -1,    -1,    -1,
    -1,   195,    -1,    -1,    -1,    -1,    -1,    -1,   202,   203,
    -1,    -1,   206,    -1,   618,   231,   620,    -1,   622,    -1,
    -1,    -1,    -1,   217,    -1,    -1,    -1,    -1,   632,    -1,
    -1,   225,   248,    -1,   228,  1922,    39,    40,    41,    42,
    43,    44,    45,    46,    47,    48,    -1,    50,    51,    52,
    53,    54,    55,    56,    57,    58,    -1,   251,    -1,   253,
   276,    -1,    -1,    -1,    -1,   259,   670,   261,   284,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1973,    -1,   692,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   708,    -1,    -1,    -1,    -1,    -1,
   714,    -1,    -1,    -1,    -1,    -1,    -1,   721,   722,    -1,
    -1,    -1,    -1,   727,    -1,    -1,    -1,   731,   732,    -1,
  2017,    -1,    -1,    -1,   738,    -1,    -1,    -1,  2025,  2026,
    -1,    -1,  2029,    -1,  2031,    -1,    -1,  2034,  2035,  2036,
  2037,  2038,  2039,  2040,  2041,    -1,  2043,    -1,  1675,    -1,
  1677,  2048,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   774,    -1,    -1,    -1,    -1,  2062,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2071,  2072,  2073,  2074,  2075,    -1,
    -1,    -1,    -1,    -1,   274,    -1,    -1,   277,    -1,   279,
   280,    -1,   282,    -1,   284,    -1,    -1,    -1,    -1,   289,
    -1,    -1,    -1,   293,   818,    -1,   296,   297,   298,   299,
   300,   301,   302,    -1,    -1,    -1,    -1,   831,    -1,    -1,
   834,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1755,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   854,    -1,   856,   857,    -1,    -1,    -1,    -1,    -1,    -1,
   864,    -1,    -1,    -1,    -1,    -1,    -1,   544,    -1,    -1,
    -1,   274,    -1,    -1,   277,    -1,    -1,   280,    -1,   282,
    -1,   284,  2169,    -1,    -1,    -1,   289,    -1,  2175,    -1,
  2177,    -1,    -1,   296,   297,   298,   299,   300,    -1,   302,
    -1,    -1,    -1,    -1,   908,    -1,   910,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2205,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2214,  2215,  2216,
  2217,  2218,  2219,  2220,  2221,  2222,    -1,    -1,    -1,    -1,
    -1,   618,    -1,    39,    40,    41,    42,    43,    44,    45,
    46,    47,    48,    -1,    50,    51,    52,    53,    54,    55,
    56,    57,    58,   967,    -1,    -1,    -1,    -1,    -1,    -1,
   974,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2266,
    -1,   985,    59,    -1,    -1,    -1,  2273,   991,  2275,    -1,
    -1,    -1,   996,    70,    -1,    -1,    -1,    -1,    -1,  1916,
    39,    40,    41,    42,    43,    44,    45,    46,    47,    48,
    -1,    50,    51,    52,    53,    54,    55,    56,    57,    58,
  1024,  1025,  1026,  1027,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2318,    -1,  2320,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   119,  2330,    -1,    -1,    -1,  1051,    -1,   126,
    -1,  1055,  1056,  1057,  1058,    -1,    -1,   134,    59,    -1,
    -1,    -1,    -1,    -1,    65,    -1,    -1,    -1,    -1,    70,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    59,    -1,    -1,    -1,   774,    -1,    -1,
    -1,    -1,    -1,    -1,    70,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   119,    -1,
    -1,  1125,  1126,  1127,  1128,   126,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   134,    -1,    -1,    -1,    -1,    -1,  1143,
    -1,   818,    -1,    -1,   145,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   153,   119,   231,    -1,    59,   834,    -1,    -1,
   126,    -1,    -1,    -1,    -1,    -1,    -1,    70,   134,    -1,
    -1,   248,    -1,    -1,    -1,    -1,    -1,    -1,   274,    -1,
  1184,   277,    -1,    -1,   280,    -1,   282,    -1,   284,  2106,
    -1,    -1,  1196,   289,    -1,  1199,    -1,  1201,    -1,   276,
   296,   297,   298,   299,   300,    -1,   302,  1211,   285,   286,
   287,   288,   289,   290,  1218,    -1,   119,    -1,    -1,    -1,
    -1,    -1,    -1,   126,    -1,    -1,    -1,    -1,    -1,    -1,
   231,   134,    -1,    -1,    -1,   274,    -1,    -1,   277,    -1,
    -1,   280,    -1,   282,    -1,   284,    -1,   248,    -1,    -1,
   289,  1255,    -1,  1257,  1258,  1259,  1260,   296,   297,   298,
   299,   300,  1266,    -1,    -1,   231,    -1,    -1,  1272,    -1,
    -1,    -1,  1276,    -1,    -1,   276,    -1,    -1,    -1,    -1,
    -1,  1285,   248,   284,   285,   286,   287,   288,   289,   290,
    -1,  1295,  1296,  1297,  1298,    -1,    -1,   298,    -1,  1303,
    -1,  1305,    -1,  1307,    -1,    -1,    -1,    -1,  1312,    -1,
   276,    -1,    -1,    -1,    -1,    -1,    -1,  1321,  1322,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,   231,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1345,  1346,    -1,    -1,   248,    -1,  1024,  1025,  1026,
  1027,  1355,  1356,  1357,  1358,  1359,  1360,    -1,    -1,    -1,
    -1,    59,    -1,    -1,    -1,    -1,    -1,    65,    -1,    -1,
    -1,    -1,    70,   276,  1051,    -1,  1380,    -1,  1055,  1056,
  1057,  1058,    -1,    -1,   287,   288,   289,   290,    -1,  1393,
  1394,    -1,    -1,  1397,    -1,  1399,    -1,    -1,  1402,  1403,
  1404,  1405,  1406,  1407,  1408,  1409,    -1,    -1,  1412,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   119,    -1,    -1,    -1,    -1,  1430,    -1,   126,  1433,
    -1,    -1,    -1,    59,    -1,    -1,   134,    -1,  1442,  1443,
  1444,  1445,  1446,  1447,    70,    -1,    -1,   145,  1125,  1126,
  1127,  1128,    -1,    -1,    -1,   153,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1143,    -1,    59,    -1,
    -1,    -1,    -1,    -1,    65,    -1,    -1,    -1,    -1,    70,
    -1,  1485,    -1,    -1,  1488,    -1,    -1,    -1,    -1,  1493,
    -1,    -1,  1496,   119,    -1,    -1,    -1,    -1,    -1,    -1,
   126,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   134,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   145,
    -1,    -1,  1199,    -1,    -1,    -1,  1530,    -1,   119,    -1,
    -1,    -1,    -1,   231,    -1,   126,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   134,    -1,    -1,  1550,    -1,    -1,  1553,
   248,    -1,    -1,    -1,   145,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   153,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1581,   276,  1583,
    -1,    -1,    -1,    -1,    -1,  1589,   284,   285,   286,   287,
   288,   289,   290,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   298,    -1,    -1,    -1,    -1,   231,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    49,   326,    -1,    52,    -1,    54,    -1,
    -1,    -1,   248,    -1,    60,    -1,    -1,    63,    -1,    -1,
    -1,    -1,    -1,   345,    -1,    -1,    -1,    -1,    74,    75,
   231,    -1,    78,    -1,    -1,    -1,    82,    83,    -1,    -1,
   276,    87,    88,    89,    -1,    -1,    -1,   248,   284,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,  1345,  1346,
    -1,  1675,    -1,  1677,    -1,    -1,    -1,    -1,  1355,  1356,
  1357,  1358,  1359,  1360,    -1,   276,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,   290,
    -1,    -1,    -1,  1380,    -1,    -1,    -1,   298,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1393,  1394,    -1,    -1,
  1397,    -1,  1399,    -1,    -1,  1402,  1403,  1404,  1405,  1406,
  1407,  1408,  1409,    -1,    -1,  1412,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1755,    59,  1430,    -1,    -1,  1433,    -1,    65,    -1,
    -1,    68,    -1,    70,    -1,  1442,  1443,  1444,  1445,  1446,
  1447,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1789,    -1,    -1,    -1,    -1,
    -1,    98,    -1,    -1,    -1,    -1,  1800,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1813,
    -1,    -1,   119,    -1,    -1,    -1,    -1,    -1,    -1,   126,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   134,  1832,    -1,
  1834,  1835,  1836,  1837,  1838,    -1,    -1,    -1,   145,  1843,
  1844,    -1,    -1,    -1,    -1,  1849,   153,    -1,    -1,   285,
    -1,    -1,    -1,    -1,    -1,    -1,  1860,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1870,  1871,    -1,    -1,
  1874,    -1,    -1,   309,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1885,    -1,    -1,   191,   321,   322,    -1,    -1,   325,
   326,    -1,   328,   329,    -1,    -1,    -1,   333,    -1,    -1,
    -1,   337,   338,    -1,    -1,   341,    -1,   343,   344,   345,
    -1,    -1,  1916,    -1,   350,   351,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   231,    -1,    59,    -1,    -1,    -1,
    -1,    -1,    65,   369,    67,    -1,    -1,    70,    -1,   375,
    -1,   248,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   390,    -1,    -1,   393,    -1,    -1,
    -1,    -1,    -1,    -1,   400,    -1,    -1,    -1,    -1,   276,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,   286,
   287,   288,   289,   290,    -1,    -1,   119,    -1,   424,  1993,
    -1,    -1,    -1,   126,    -1,    -1,  2000,   433,  1675,    -1,
  1677,   134,    -1,   439,    -1,    -1,   442,    -1,    -1,   445,
    -1,    -1,   145,  2017,    -1,    -1,    -1,    -1,    -1,    -1,
   153,  2025,    -1,    -1,   460,  2029,    -1,  2031,    -1,    -1,
  2034,  2035,  2036,  2037,  2038,  2039,  2040,  2041,    -1,  2043,
   476,    -1,    -1,    -1,  2048,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   488,    -1,    -1,    -1,  2060,    -1,  2062,    -1,
    -1,    -1,   774,    -1,    -1,    -1,    -1,  2071,  2072,  2073,
  2074,  2075,   508,    -1,    -1,   511,   512,    -1,  1755,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2092,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   231,    -1,
    -1,    -1,  2106,    -1,    -1,    59,   818,    -1,   544,    -1,
    -1,    65,    -1,    -1,    -1,   248,    70,    -1,    -1,    -1,
    -1,    -1,   834,   559,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   575,
  2144,    -1,  2146,   276,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   284,   285,   286,   287,   288,   289,   290,    -1,   113,
    -1,    -1,    -1,    -1,    -1,   119,    -1,    -1,    -1,    -1,
    -1,    -1,   126,  2177,    -1,    -1,    -1,    -1,    -1,    -1,
   134,    -1,   618,    -1,   620,    -1,   622,    -1,    -1,    -1,
    -1,   145,    -1,    -1,  2198,    -1,   632,  2201,    -1,   153,
    -1,  2205,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  2214,  2215,  2216,  2217,  2218,  2219,  2220,  2221,  2222,  2223,
    -1,    -1,  2226,    -1,    -1,    -1,  2230,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   670,    -1,    -1,    -1,    -1,  1916,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   692,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2275,   708,  2277,    -1,    -1,    -1,   231,   714,    -1,
    -1,    -1,    -1,    -1,    -1,   721,   722,    -1,    -1,    -1,
    -1,   727,    -1,    59,   248,   731,   732,    -1,    -1,    65,
    -1,    -1,   738,    -1,    70,    -1,    -1,    -1,    -1,    -1,
  2314,    -1,  1024,  1025,  1026,  1027,    -1,    -1,    -1,    -1,
    -1,    -1,   276,   759,    -1,    -1,  2330,    -1,    -1,    -1,
   284,   285,   286,   287,   288,   289,   290,    -1,    -1,  1051,
    -1,    -1,    -1,  1055,  1056,  1057,  1058,     7,    -1,    -1,
    10,    11,    -1,   119,    14,    -1,    -1,    -1,    -1,    -1,
   126,    -1,    22,    23,    -1,    -1,  2370,    -1,   134,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    37,    38,   145,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   153,    -1,    -1,
    -1,    -1,    -1,    -1,    59,   831,   832,    -1,    -1,    -1,
    65,    -1,    67,    -1,    64,    70,    -1,    -1,    -1,    69,
    -1,   177,    -1,  1125,  1126,  1127,  1128,    -1,   854,    79,
   856,   857,    -1,    83,    -1,    85,    -1,    -1,   864,  2106,
    -1,  1143,    -1,    -1,    -1,    95,    -1,    97,    -1,    -1,
    -1,   101,    -1,   103,    -1,   105,    -1,    -1,    -1,   109,
    -1,    -1,    -1,    -1,   119,   115,    -1,    -1,    -1,    -1,
    -1,   126,   122,    -1,    -1,   231,    -1,    -1,    -1,   134,
    -1,    -1,   908,    -1,   910,    -1,    -1,    -1,    -1,    -1,
   145,    -1,   248,    -1,    -1,    -1,    -1,    -1,   153,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   167,    -1,   169,
   276,    -1,   172,   173,    -1,    -1,    -1,    -1,   284,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,   189,
    -1,   967,    -1,    -1,    -1,    -1,    -1,    -1,   974,    -1,
    -1,    -1,    -1,   203,   204,    -1,    -1,    -1,    -1,   985,
    -1,    -1,   212,   213,    -1,   991,    -1,    -1,    -1,    -1,
   996,    -1,   222,   223,    -1,    -1,   231,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   234,   235,   236,    -1,   238,    -1,
    -1,   241,    -1,   248,    -1,    -1,    -1,   247,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   256,    -1,    -1,    -1,
    -1,    -1,    -1,   263,    -1,    -1,    -1,    -1,    -1,    -1,
   270,   276,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   284,
   285,   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1345,  1346,    -1,    -1,    -1,    -1,   299,
    -1,    -1,    -1,  1355,  1356,  1357,  1358,  1359,  1360,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1380,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1393,  1394,    -1,    -1,  1397,    -1,  1399,    -1,    -1,
  1402,  1403,  1404,  1405,  1406,  1407,  1408,  1409,    -1,    -1,
  1412,  1137,    -1,    -1,    -1,    -1,    59,    -1,    -1,    -1,
    -1,    -1,    65,    -1,    67,    -1,    -1,    70,  1430,    -1,
    -1,  1433,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1442,  1443,  1444,  1445,  1446,  1447,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1184,    -1,
    -1,    -1,    -1,    59,    -1,    -1,    -1,    -1,    -1,    65,
  1196,    -1,    -1,  1199,    70,  1201,   119,    -1,    -1,    -1,
    -1,    -1,    -1,   126,    -1,  1211,    -1,    -1,    -1,    -1,
    -1,   134,  1218,    59,    -1,    -1,    -1,    -1,   562,    -1,
    -1,    -1,   145,    -1,    70,    -1,    -1,    -1,    -1,    -1,
   153,    -1,   576,    -1,   578,    59,   580,   581,   582,    -1,
    -1,    65,    -1,   119,    -1,    -1,    70,    -1,    -1,  1255,
   126,  1257,  1258,  1259,  1260,    -1,    -1,    -1,   134,    -1,
  1266,    -1,    -1,    -1,    -1,    -1,  1272,    -1,  1550,   145,
  1276,    -1,    -1,   119,    -1,    -1,    -1,   153,    -1,  1285,
   126,    -1,    -1,    -1,    -1,    -1,    -1,  1293,   134,  1295,
  1296,  1297,  1298,    -1,   638,   119,    -1,  1303,    -1,  1305,
    -1,  1307,   126,    -1,    -1,    -1,  1312,    -1,   231,    -1,
   134,    -1,    -1,    -1,  1320,  1321,  1322,    -1,    -1,    -1,
    -1,   145,    -1,    -1,    -1,   248,    -1,    -1,    -1,   153,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1354,    -1,
    -1,    -1,    -1,   276,    -1,   231,    -1,    -1,    -1,    -1,
    -1,   284,   285,   286,   287,   288,   289,   290,    -1,    -1,
    -1,  1377,   248,    -1,    -1,   719,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   231,  1392,    -1,    -1,    -1,
    -1,    -1,    -1,  1675,    -1,  1677,    -1,    -1,    -1,    -1,
   276,    -1,   248,    -1,   748,    -1,    -1,   231,   284,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   248,    -1,    -1,   771,    -1,    -1,
   276,    -1,    -1,    -1,   778,  1441,    -1,    -1,   284,   285,
   286,   287,   288,   289,   290,    -1,    -1,    -1,    59,    -1,
    -1,    -1,   276,  1459,    65,    -1,    -1,    -1,    -1,    70,
   284,   285,   286,   287,   288,   289,   290,    -1,    -1,    -1,
    -1,    -1,    -1,  1755,   818,    -1,    -1,    -1,    -1,  1485,
    -1,    -1,  1488,    -1,    -1,    -1,    -1,  1493,    -1,   833,
  1496,    -1,    -1,   837,    -1,   839,    -1,    -1,   842,   843,
   844,   845,   846,   847,   848,   849,    -1,    -1,   119,    -1,
    -1,    59,    -1,    -1,    -1,   126,    -1,    65,    -1,    -1,
    -1,    -1,    70,   134,  1530,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   145,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   153,    -1,    -1,    -1,    -1,  1553,    -1,  1555,
  1832,    -1,  1834,  1835,  1836,  1837,  1838,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   119,    -1,    -1,    -1,  1581,    -1,  1583,   126,    -1,
    -1,    -1,    -1,  1589,    -1,    -1,   134,    -1,    -1,  1871,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   145,    -1,    -1,
    -1,    -1,    -1,  1885,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   231,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    59,    -1,    -1,    -1,  1916,    -1,    65,   248,    -1,    -1,
    -1,    70,    -1,    -1,    -1,    -1,    -1,    -1,   992,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1668,    -1,    -1,   276,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,   290,
    -1,    -1,    -1,   231,    -1,    -1,    -1,    -1,    -1,    -1,
   119,    -1,    -1,  1037,    -1,  1039,  1040,   126,    -1,    -1,
   248,    -1,    -1,    -1,    -1,   134,    -1,  1051,    -1,    -1,
    -1,  1055,  1056,  1057,  1058,  1059,   145,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1071,   276,    -1,
    -1,    -1,    -1,    -1,    -1,  2017,   284,   285,   286,   287,
   288,   289,   290,  2025,    -1,    -1,    -1,  2029,  1754,  2031,
    -1,    -1,  2034,  2035,  2036,  2037,  2038,  2039,  2040,  2041,
    -1,  2043,    -1,    -1,    -1,    -1,  2048,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  2062,    -1,    -1,  1789,    -1,  1129,    -1,    -1,    -1,  2071,
  2072,  2073,  2074,  2075,  1800,    -1,    -1,    -1,    -1,    -1,
    -1,  1145,   231,    -1,    -1,  1149,    -1,  1813,    -1,  1153,
    -1,    -1,    -1,  1157,    -1,    -1,    59,  1161,    -1,   248,
   758,  1165,   774,    -1,  2106,  1169,    -1,    70,    -1,  1173,
    -1,    -1,    -1,  1177,    -1,    -1,    -1,  1843,  1844,    -1,
    -1,    -1,    -1,  1849,    -1,    -1,    -1,   276,    -1,    -1,
    -1,    -1,    -1,    -1,  1860,   284,   285,   286,   287,   288,
   289,   290,    -1,    -1,  1870,    -1,   818,    -1,  1874,  1213,
    -1,    -1,    -1,    -1,    -1,    -1,   119,    -1,    -1,  1885,
    -1,    -1,   834,   126,    -1,    -1,    -1,    -1,    -1,    -1,
  1896,   134,    -1,    -1,    -1,  2177,    -1,    -1,    -1,  1905,
    -1,    -1,   145,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1922,    -1,    -1,    -1,
    -1,    -1,    -1,  2205,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  2214,  2215,  2216,  2217,  2218,  2219,  2220,  2221,
  2222,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1960,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1973,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1324,    -1,    -1,    -1,    -1,    -1,    -1,  1993,   231,  1333,
    -1,    -1,    -1,  2275,  2000,    -1,    -1,    -1,  1342,    -1,
    -1,    -1,    -1,    -1,  1348,   248,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2024,    -1,
  2026,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1373,
    -1,    -1,    -1,   276,    -1,    -1,    -1,    -1,  1382,    -1,
    -1,   284,   285,   286,   287,   288,   289,   290,  2330,  1393,
    -1,    -1,    -1,  1397,  2060,  1399,    -1,    -1,  1402,  1403,
  1404,  1405,  1406,  1407,  1408,  1409,    -1,    -1,  1412,    -1,
    -1,    -1,  1024,  1025,  1026,  1027,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  2092,    -1,    -1,    -1,
    -1,  1435,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1051,
    -1,    -1,    -1,  1055,  1056,  1057,  1058,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2144,    -1,
  2146,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  2169,    -1,    -1,    -1,    -1,    -1,  2175,
    -1,    -1,    -1,  1125,  1126,  1127,  1128,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1137,    -1,    -1,    -1,    -1,
    -1,  1143,  2198,    -1,    -1,  2201,    -1,    -1,    -1,  1137,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2213,    -1,    -1,
    -1,  1149,    -1,    -1,    -1,  1153,    -1,  2223,    -1,  1157,
  2226,  2227,  2228,  1161,  2230,    -1,    -1,  1165,    -1,    -1,
    -1,  1169,    -1,    -1,    -1,  1173,    -1,    -1,    -1,  1177,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2265,
  2266,    -1,    -1,    -1,    -1,    -1,    -1,  2273,    -1,    -1,
    -1,  2277,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1631,  1632,  1633,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2314,    -1,
    -1,    -1,  2318,    -1,  2320,  1659,  1660,  1661,    -1,    -1,
    -1,  2327,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1677,    -1,    -1,    -1,    -1,  1682,    -1,
    -1,    -1,    -1,  1687,    -1,    -1,    -1,    -1,  1692,    -1,
    -1,    -1,    -1,  1697,    -1,    -1,  1294,    -1,  1702,    -1,
    -1,    -1,    -1,  1707,  2370,    -1,    -1,    -1,  1712,    -1,
    -1,    -1,    -1,  1717,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1729,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1345,  1346,  1739,  1740,  1741,    -1,    -1,
    -1,    -1,    -1,  1355,  1356,  1357,  1358,  1359,  1360,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1380,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1376,    -1,
    -1,  1393,  1394,    -1,    -1,  1397,    -1,  1399,    -1,    -1,
  1402,  1403,  1404,  1405,  1406,  1407,  1408,  1409,    -1,    -1,
  1412,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1430,    -1,
    -1,  1433,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1442,  1443,  1444,  1445,  1446,  1447,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1459,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1459,    -1,  1461,  1462,    -1,  1464,  1465,    -1,  1467,
  1468,    -1,  1470,  1471,    -1,  1473,  1474,    -1,  1476,  1477,
    -1,  1479,  1480,    -1,  1482,  1483,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1550,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    76,
    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,  2003,
    -1,   118,    -1,    -1,    -1,    -1,    -1,   124,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   137,   138,    -1,   140,    -1,   142,    -1,    -1,    -1,    -1,
    -1,   148,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   171,   172,  1668,    -1,    -1,    -1,
    -1,   178,   179,  1675,    -1,  1677,    -1,    -1,    -1,   186,
  1668,    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,
    -1,    -1,    -1,   200,  1682,    -1,   203,    -1,    -1,  1687,
    -1,    -1,    -1,    -1,  1692,    -1,    -1,    -1,    -1,  1697,
    -1,    -1,    -1,    -1,  1702,    -1,    -1,    -1,    -1,  1707,
    -1,    -1,    -1,    -1,  1712,    -1,    -1,    -1,    -1,  1717,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1725,    -1,    -1,
    -1,  1729,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1754,  1755,    -1,   262,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   274,   275,    -1,
   277,    -1,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1832,    -1,  1834,  1835,  1836,  1837,  1838,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2258,    -1,    -1,    -1,    -1,  1871,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,  1916,    -1,    -1,    -1,    -1,    -1,
  1922,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1922,    -1,    -1,  1925,  1926,    -1,
  1928,  1929,    -1,  1931,  1932,    -1,  1934,  1935,    -1,  1937,
  1938,    -1,  1940,  1941,    -1,  1943,  1944,    -1,  1946,  1947,
    -1,    -1,    -1,    -1,    -1,  1953,    -1,    93,    94,  1957,
    -1,  1973,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   110,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,    -1,    -1,    -1,   124,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   137,   138,    -1,    -1,  2017,    -1,    -1,    -1,    -1,
    -1,    -1,   148,  2025,  2026,    -1,    -1,  2029,    -1,  2031,
    -1,    -1,  2034,  2035,  2036,  2037,  2038,  2039,  2040,  2041,
    -1,  2043,    -1,    -1,    -1,   171,  2048,    -1,    -1,    -1,
    -1,    -1,   178,   179,    -1,    -1,    -1,    -1,    -1,    -1,
  2062,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2071,
  2072,  2073,  2074,  2075,   200,    -1,    -1,   203,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2106,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   274,    -1,
    -1,   277,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2169,    -1,    -1,
    -1,    -1,    -1,  2175,    -1,  2177,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  2205,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  2214,  2215,  2216,  2217,  2218,  2219,  2220,  2221,
  2222,    -1,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,  2266,    -1,    -1,    -1,    -1,    -1,
    -1,  2273,    -1,  2275,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    74,    75,    76,    77,    -1,    -1,    80,
    -1,    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,
    91,    -1,    93,    94,    -1,    -1,  2318,    -1,  2320,   100,
    -1,    -1,    -1,    -1,    -1,   106,   107,   108,  2330,   110,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,   262,
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
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
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
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
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
    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,   258,
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
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
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
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,   262,
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
   170,   171,   172,    -1,   174,    -1,    -1,    -1,   178,   179,
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
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,   262,    -1,
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
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
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
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,   262,
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
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
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
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
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
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
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
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
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
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
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
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
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
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
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
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
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
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
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
    -1,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,
    -1,    -1,    -1,    -1,    -1,    -1,   186,    -1,    -1,    -1,
    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,    -1,    -1,   203,    -1,    -1,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   262,    60,    61,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   274,   275,    -1,   277,    -1,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    87,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,    -1,
    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,
   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,
    -1,    -1,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,
    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
   257,   258,   259,   260,   261,    -1,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,    -1,
   277,    -1,    -1,    -1,   281,     3,     4,     5,     6,     7,
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
    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,
   264,   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,
   274,    -1,    -1,   277,   278,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    74,    -1,    -1,    -1,
    -1,    -1,    80,    -1,    -1,    -1,    84,    -1,    -1,    87,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,   256,   257,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
   253,    -1,   255,    -1,   257,   258,   259,   260,   261,    -1,
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
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
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
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 712:
#line 3586 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 713:
#line 3588 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 714:
#line 3590 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 715:
#line 3594 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 716:
#line 3598 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 717:
#line 3600 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 718:
#line 3602 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 719:
#line 3604 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 720:
#line 3606 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 721:
#line 3608 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 722:
#line 3610 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make1_str("(*)")); 
				;
    break;}
case 723:
#line 3614 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 724:
#line 3618 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 725:
#line 3622 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 726:
#line 3626 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 727:
#line 3630 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 728:
#line 3636 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 729:
#line 3640 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 730:
#line 3646 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 731:
#line 3650 "preproc.y"
{
  		     		        yyval.str = make1_str("user");
			     	;
    break;}
case 732:
#line 3655 "preproc.y"
{
					yyval.str = make3_str(make1_str("exists("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 733:
#line 3659 "preproc.y"
{
					yyval.str = make3_str(make1_str("extract("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 734:
#line 3663 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 735:
#line 3667 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 736:
#line 3672 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 737:
#line 3676 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 738:
#line 3680 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 739:
#line 3684 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 740:
#line 3688 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 741:
#line 3690 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 742:
#line 3692 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 743:
#line 3694 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 744:
#line 3701 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); }
				;
    break;}
case 745:
#line 3705 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); }
				;
    break;}
case 746:
#line 3709 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); }
				;
    break;}
case 747:
#line 3713 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); }
				;
    break;}
case 748:
#line 3717 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 749:
#line 3721 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 750:
#line 3725 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(" in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 751:
#line 3729 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(" not in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 752:
#line 3733 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-4].str, yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 753:
#line 3737 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("+("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 754:
#line 3741 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("-("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 755:
#line 3745 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("/("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 756:
#line 3749 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("*("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 757:
#line 3753 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("<("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 758:
#line 3757 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(">("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 759:
#line 3761 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("=("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 760:
#line 3765 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("any("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 761:
#line 3769 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 762:
#line 3773 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 763:
#line 3777 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 764:
#line 3781 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 765:
#line 3785 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 766:
#line 3789 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 767:
#line 3793 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 768:
#line 3797 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("all ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 769:
#line 3801 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 770:
#line 3805 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 771:
#line 3809 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 772:
#line 3813 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 773:
#line 3817 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 774:
#line 3821 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 775:
#line 3825 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 776:
#line 3829 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 777:
#line 3831 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 778:
#line 3833 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 779:
#line 3835 "preproc.y"
{       yyval.str = yyvsp[0].str; ;
    break;}
case 780:
#line 3837 "preproc.y"
{ yyval.str = make1_str("?"); ;
    break;}
case 781:
#line 3846 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 782:
#line 3850 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 783:
#line 3852 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 784:
#line 3856 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 785:
#line 3858 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 786:
#line 3860 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 787:
#line 3862 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 788:
#line 3864 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 789:
#line 3869 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 790:
#line 3871 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 791:
#line 3873 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 792:
#line 3877 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 793:
#line 3881 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 794:
#line 3883 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 795:
#line 3885 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 796:
#line 3887 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 797:
#line 3889 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 798:
#line 3893 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 799:
#line 3897 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 800:
#line 3901 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 801:
#line 3905 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 802:
#line 3911 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 803:
#line 3915 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 804:
#line 3921 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 805:
#line 3925 "preproc.y"
{
					yyval.str = make1_str("user");
				;
    break;}
case 806:
#line 3929 "preproc.y"
{
					yyval.str = make3_str(make1_str("position ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 807:
#line 3933 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 808:
#line 3938 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 809:
#line 3942 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 810:
#line 3946 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 811:
#line 3950 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 812:
#line 3954 "preproc.y"
{ 	yyval.str = yyvsp[0].str; ;
    break;}
case 813:
#line 3958 "preproc.y"
{
					yyval.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].str);
				;
    break;}
case 814:
#line 3962 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("["), yyvsp[-4].str, make1_str(":"), yyvsp[-2].str, make1_str("]")), yyvsp[0].str);
				;
    break;}
case 815:
#line 3966 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 816:
#line 3970 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 817:
#line 3972 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 818:
#line 3974 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str("using"), yyvsp[0].str); ;
    break;}
case 819:
#line 3978 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("from"), yyvsp[0].str);
				;
    break;}
case 820:
#line 3982 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 821:
#line 3984 "preproc.y"
{ yyval.str = make1_str("?"); ;
    break;}
case 822:
#line 3987 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 823:
#line 3988 "preproc.y"
{ yyval.str = make1_str("timezone_hour"); ;
    break;}
case 824:
#line 3989 "preproc.y"
{ yyval.str = make1_str("timezone_minute"); ;
    break;}
case 825:
#line 3993 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("in"), yyvsp[0].str); ;
    break;}
case 826:
#line 3995 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 827:
#line 3999 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 828:
#line 4003 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 829:
#line 4005 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 830:
#line 4007 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 831:
#line 4009 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 832:
#line 4011 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 833:
#line 4013 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 834:
#line 4015 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 835:
#line 4017 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 836:
#line 4021 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 837:
#line 4025 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 838:
#line 4027 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 839:
#line 4029 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 840:
#line 4031 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 841:
#line 4033 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 842:
#line 4037 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()"));
				;
    break;}
case 843:
#line 4041 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 844:
#line 4045 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 845:
#line 4049 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 846:
#line 4054 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 847:
#line 4058 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 848:
#line 4062 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 849:
#line 4066 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 850:
#line 4072 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 851:
#line 4076 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 852:
#line 4080 "preproc.y"
{	yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 853:
#line 4082 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 854:
#line 4088 "preproc.y"
{	yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 855:
#line 4090 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 856:
#line 4094 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str("from"), yyvsp[0].str); ;
    break;}
case 857:
#line 4096 "preproc.y"
{ yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 858:
#line 4098 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 859:
#line 4102 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 860:
#line 4106 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 861:
#line 4110 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 862:
#line 4112 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);;
    break;}
case 863:
#line 4116 "preproc.y"
{
					yyval.str = yyvsp[0].str; 
				;
    break;}
case 864:
#line 4120 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 865:
#line 4124 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 866:
#line 4126 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);;
    break;}
case 867:
#line 4145 "preproc.y"
{ yyval.str = cat5_str(make1_str("case"), yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, make1_str("end")); ;
    break;}
case 868:
#line 4147 "preproc.y"
{
					yyval.str = cat5_str(make1_str("nullif("), yyvsp[-3].str, make1_str(","), yyvsp[-1].str, make1_str(")"));

					fprintf(stderr, "NULLIF() not yet fully implemented");
                                ;
    break;}
case 869:
#line 4153 "preproc.y"
{
					yyval.str = cat3_str(make1_str("coalesce("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 870:
#line 4159 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 871:
#line 4161 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 872:
#line 4165 "preproc.y"
{
					yyval.str = cat4_str(make1_str("when"), yyvsp[-2].str, make1_str("then"), yyvsp[0].str);
                               ;
    break;}
case 873:
#line 4170 "preproc.y"
{ yyval.str = cat2_str(make1_str("else"), yyvsp[0].str); ;
    break;}
case 874:
#line 4171 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 875:
#line 4175 "preproc.y"
{
                                       yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                               ;
    break;}
case 876:
#line 4179 "preproc.y"
{
                                       yyval.str = yyvsp[0].str;
                               ;
    break;}
case 877:
#line 4183 "preproc.y"
{       yyval.str = make1_str(""); ;
    break;}
case 878:
#line 4187 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 879:
#line 4191 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 880:
#line 4197 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 881:
#line 4199 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str); ;
    break;}
case 882:
#line 4201 "preproc.y"
{ yyval.str = make2_str(yyvsp[-2].str, make1_str(".*")); ;
    break;}
case 883:
#line 4212 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","),yyvsp[0].str);  ;
    break;}
case 884:
#line 4214 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 885:
#line 4215 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 886:
#line 4219 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-3].str, yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 887:
#line 4223 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 888:
#line 4227 "preproc.y"
{
					yyval.str = make2_str(yyvsp[-2].str, make1_str(".*"));
				;
    break;}
case 889:
#line 4238 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);  ;
    break;}
case 890:
#line 4240 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 891:
#line 4245 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
				;
    break;}
case 892:
#line 4249 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 893:
#line 4253 "preproc.y"
{
					yyval.str = make2_str(yyvsp[-2].str, make1_str(".*"));
				;
    break;}
case 894:
#line 4257 "preproc.y"
{
					yyval.str = make1_str("*");
				;
    break;}
case 895:
#line 4262 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 896:
#line 4263 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 897:
#line 4267 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 898:
#line 4271 "preproc.y"
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
case 899:
#line 4283 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 900:
#line 4284 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 901:
#line 4285 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 902:
#line 4286 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 903:
#line 4287 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 904:
#line 4293 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 905:
#line 4294 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 906:
#line 4296 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 907:
#line 4303 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 908:
#line 4307 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 909:
#line 4311 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 910:
#line 4315 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 911:
#line 4319 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 912:
#line 4321 "preproc.y"
{
					yyval.str = make1_str("true");
				;
    break;}
case 913:
#line 4325 "preproc.y"
{
					yyval.str = make1_str("false");
				;
    break;}
case 914:
#line 4331 "preproc.y"
{
					yyval.str = cat2_str(make_name(), yyvsp[0].str);
				;
    break;}
case 915:
#line 4336 "preproc.y"
{ yyval.str = make_name();;
    break;}
case 916:
#line 4337 "preproc.y"
{ yyval.str = make_name();;
    break;}
case 917:
#line 4338 "preproc.y"
{
							yyval.str = (char *)mm_alloc(strlen(yyvsp[0].str) + 3);
							yyval.str[0]='\'';
				     		        strcpy(yyval.str+1, yyvsp[0].str);
							yyval.str[strlen(yyvsp[0].str)+2]='\0';
							yyval.str[strlen(yyvsp[0].str)+1]='\'';
							free(yyvsp[0].str);
						;
    break;}
case 918:
#line 4346 "preproc.y"
{ yyval.str = yyvsp[0].str;;
    break;}
case 919:
#line 4354 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
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
#line 4368 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 923:
#line 4369 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 924:
#line 4370 "preproc.y"
{ yyval.str = make1_str("absolute"); ;
    break;}
case 925:
#line 4371 "preproc.y"
{ yyval.str = make1_str("action"); ;
    break;}
case 926:
#line 4372 "preproc.y"
{ yyval.str = make1_str("after"); ;
    break;}
case 927:
#line 4373 "preproc.y"
{ yyval.str = make1_str("aggregate"); ;
    break;}
case 928:
#line 4374 "preproc.y"
{ yyval.str = make1_str("backward"); ;
    break;}
case 929:
#line 4375 "preproc.y"
{ yyval.str = make1_str("before"); ;
    break;}
case 930:
#line 4376 "preproc.y"
{ yyval.str = make1_str("cache"); ;
    break;}
case 931:
#line 4377 "preproc.y"
{ yyval.str = make1_str("createdb"); ;
    break;}
case 932:
#line 4378 "preproc.y"
{ yyval.str = make1_str("createuser"); ;
    break;}
case 933:
#line 4379 "preproc.y"
{ yyval.str = make1_str("cycle"); ;
    break;}
case 934:
#line 4380 "preproc.y"
{ yyval.str = make1_str("database"); ;
    break;}
case 935:
#line 4381 "preproc.y"
{ yyval.str = make1_str("delimiters"); ;
    break;}
case 936:
#line 4382 "preproc.y"
{ yyval.str = make1_str("double"); ;
    break;}
case 937:
#line 4383 "preproc.y"
{ yyval.str = make1_str("each"); ;
    break;}
case 938:
#line 4384 "preproc.y"
{ yyval.str = make1_str("encoding"); ;
    break;}
case 939:
#line 4385 "preproc.y"
{ yyval.str = make1_str("forward"); ;
    break;}
case 940:
#line 4386 "preproc.y"
{ yyval.str = make1_str("function"); ;
    break;}
case 941:
#line 4387 "preproc.y"
{ yyval.str = make1_str("handler"); ;
    break;}
case 942:
#line 4388 "preproc.y"
{ yyval.str = make1_str("increment"); ;
    break;}
case 943:
#line 4389 "preproc.y"
{ yyval.str = make1_str("index"); ;
    break;}
case 944:
#line 4390 "preproc.y"
{ yyval.str = make1_str("inherits"); ;
    break;}
case 945:
#line 4391 "preproc.y"
{ yyval.str = make1_str("insensitive"); ;
    break;}
case 946:
#line 4392 "preproc.y"
{ yyval.str = make1_str("instead"); ;
    break;}
case 947:
#line 4393 "preproc.y"
{ yyval.str = make1_str("isnull"); ;
    break;}
case 948:
#line 4394 "preproc.y"
{ yyval.str = make1_str("key"); ;
    break;}
case 949:
#line 4395 "preproc.y"
{ yyval.str = make1_str("language"); ;
    break;}
case 950:
#line 4396 "preproc.y"
{ yyval.str = make1_str("lancompiler"); ;
    break;}
case 951:
#line 4397 "preproc.y"
{ yyval.str = make1_str("location"); ;
    break;}
case 952:
#line 4398 "preproc.y"
{ yyval.str = make1_str("match"); ;
    break;}
case 953:
#line 4399 "preproc.y"
{ yyval.str = make1_str("maxvalue"); ;
    break;}
case 954:
#line 4400 "preproc.y"
{ yyval.str = make1_str("minvalue"); ;
    break;}
case 955:
#line 4401 "preproc.y"
{ yyval.str = make1_str("next"); ;
    break;}
case 956:
#line 4402 "preproc.y"
{ yyval.str = make1_str("nocreatedb"); ;
    break;}
case 957:
#line 4403 "preproc.y"
{ yyval.str = make1_str("nocreateuser"); ;
    break;}
case 958:
#line 4404 "preproc.y"
{ yyval.str = make1_str("nothing"); ;
    break;}
case 959:
#line 4405 "preproc.y"
{ yyval.str = make1_str("notnull"); ;
    break;}
case 960:
#line 4406 "preproc.y"
{ yyval.str = make1_str("of"); ;
    break;}
case 961:
#line 4407 "preproc.y"
{ yyval.str = make1_str("oids"); ;
    break;}
case 962:
#line 4408 "preproc.y"
{ yyval.str = make1_str("only"); ;
    break;}
case 963:
#line 4409 "preproc.y"
{ yyval.str = make1_str("operator"); ;
    break;}
case 964:
#line 4410 "preproc.y"
{ yyval.str = make1_str("option"); ;
    break;}
case 965:
#line 4411 "preproc.y"
{ yyval.str = make1_str("password"); ;
    break;}
case 966:
#line 4412 "preproc.y"
{ yyval.str = make1_str("prior"); ;
    break;}
case 967:
#line 4413 "preproc.y"
{ yyval.str = make1_str("privileges"); ;
    break;}
case 968:
#line 4414 "preproc.y"
{ yyval.str = make1_str("procedural"); ;
    break;}
case 969:
#line 4415 "preproc.y"
{ yyval.str = make1_str("read"); ;
    break;}
case 970:
#line 4417 "preproc.y"
{ yyval.str = make1_str("relative"); ;
    break;}
case 971:
#line 4418 "preproc.y"
{ yyval.str = make1_str("rename"); ;
    break;}
case 972:
#line 4419 "preproc.y"
{ yyval.str = make1_str("returns"); ;
    break;}
case 973:
#line 4420 "preproc.y"
{ yyval.str = make1_str("row"); ;
    break;}
case 974:
#line 4421 "preproc.y"
{ yyval.str = make1_str("rule"); ;
    break;}
case 975:
#line 4422 "preproc.y"
{ yyval.str = make1_str("scroll"); ;
    break;}
case 976:
#line 4423 "preproc.y"
{ yyval.str = make1_str("sequence"); ;
    break;}
case 977:
#line 4424 "preproc.y"
{ yyval.str = make1_str("serial"); ;
    break;}
case 978:
#line 4425 "preproc.y"
{ yyval.str = make1_str("start"); ;
    break;}
case 979:
#line 4426 "preproc.y"
{ yyval.str = make1_str("statement"); ;
    break;}
case 980:
#line 4427 "preproc.y"
{ yyval.str = make1_str("stdin"); ;
    break;}
case 981:
#line 4428 "preproc.y"
{ yyval.str = make1_str("stdout"); ;
    break;}
case 982:
#line 4429 "preproc.y"
{ yyval.str = make1_str("time"); ;
    break;}
case 983:
#line 4430 "preproc.y"
{ yyval.str = make1_str("timestamp"); ;
    break;}
case 984:
#line 4431 "preproc.y"
{ yyval.str = make1_str("timezone_hour"); ;
    break;}
case 985:
#line 4432 "preproc.y"
{ yyval.str = make1_str("timezone_minute"); ;
    break;}
case 986:
#line 4433 "preproc.y"
{ yyval.str = make1_str("trigger"); ;
    break;}
case 987:
#line 4434 "preproc.y"
{ yyval.str = make1_str("trusted"); ;
    break;}
case 988:
#line 4435 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 989:
#line 4436 "preproc.y"
{ yyval.str = make1_str("valid"); ;
    break;}
case 990:
#line 4437 "preproc.y"
{ yyval.str = make1_str("version"); ;
    break;}
case 991:
#line 4438 "preproc.y"
{ yyval.str = make1_str("zone"); ;
    break;}
case 992:
#line 4439 "preproc.y"
{ yyval.str = make1_str("at"); ;
    break;}
case 993:
#line 4440 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 994:
#line 4441 "preproc.y"
{ yyval.str = make1_str("break"); ;
    break;}
case 995:
#line 4442 "preproc.y"
{ yyval.str = make1_str("call"); ;
    break;}
case 996:
#line 4443 "preproc.y"
{ yyval.str = make1_str("connect"); ;
    break;}
case 997:
#line 4444 "preproc.y"
{ yyval.str = make1_str("connection"); ;
    break;}
case 998:
#line 4445 "preproc.y"
{ yyval.str = make1_str("continue"); ;
    break;}
case 999:
#line 4446 "preproc.y"
{ yyval.str = make1_str("deallocate"); ;
    break;}
case 1000:
#line 4447 "preproc.y"
{ yyval.str = make1_str("disconnect"); ;
    break;}
case 1001:
#line 4448 "preproc.y"
{ yyval.str = make1_str("found"); ;
    break;}
case 1002:
#line 4449 "preproc.y"
{ yyval.str = make1_str("go"); ;
    break;}
case 1003:
#line 4450 "preproc.y"
{ yyval.str = make1_str("goto"); ;
    break;}
case 1004:
#line 4451 "preproc.y"
{ yyval.str = make1_str("identified"); ;
    break;}
case 1005:
#line 4452 "preproc.y"
{ yyval.str = make1_str("immediate"); ;
    break;}
case 1006:
#line 4453 "preproc.y"
{ yyval.str = make1_str("indicator"); ;
    break;}
case 1007:
#line 4454 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 1008:
#line 4455 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 1009:
#line 4456 "preproc.y"
{ yyval.str = make1_str("open"); ;
    break;}
case 1010:
#line 4457 "preproc.y"
{ yyval.str = make1_str("prepare"); ;
    break;}
case 1011:
#line 4458 "preproc.y"
{ yyval.str = make1_str("release"); ;
    break;}
case 1012:
#line 4459 "preproc.y"
{ yyval.str = make1_str("section"); ;
    break;}
case 1013:
#line 4460 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 1014:
#line 4461 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1015:
#line 4462 "preproc.y"
{ yyval.str = make1_str("sqlerror"); ;
    break;}
case 1016:
#line 4463 "preproc.y"
{ yyval.str = make1_str("sqlprint"); ;
    break;}
case 1017:
#line 4464 "preproc.y"
{ yyval.str = make1_str("sqlwarning"); ;
    break;}
case 1018:
#line 4465 "preproc.y"
{ yyval.str = make1_str("stop"); ;
    break;}
case 1019:
#line 4466 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 1020:
#line 4467 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 1021:
#line 4468 "preproc.y"
{ yyval.str = make1_str("var"); ;
    break;}
case 1022:
#line 4469 "preproc.y"
{ yyval.str = make1_str("whenever"); ;
    break;}
case 1023:
#line 4481 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1024:
#line 4482 "preproc.y"
{ yyval.str = make1_str("abort"); ;
    break;}
case 1025:
#line 4483 "preproc.y"
{ yyval.str = make1_str("analyze"); ;
    break;}
case 1026:
#line 4484 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 1027:
#line 4485 "preproc.y"
{ yyval.str = make1_str("case"); ;
    break;}
case 1028:
#line 4486 "preproc.y"
{ yyval.str = make1_str("cluster"); ;
    break;}
case 1029:
#line 4487 "preproc.y"
{ yyval.str = make1_str("coalesce"); ;
    break;}
case 1030:
#line 4488 "preproc.y"
{ yyval.str = make1_str("constraint"); ;
    break;}
case 1031:
#line 4489 "preproc.y"
{ yyval.str = make1_str("copy"); ;
    break;}
case 1032:
#line 4490 "preproc.y"
{ yyval.str = make1_str("current"); ;
    break;}
case 1033:
#line 4491 "preproc.y"
{ yyval.str = make1_str("do"); ;
    break;}
case 1034:
#line 4492 "preproc.y"
{ yyval.str = make1_str("else"); ;
    break;}
case 1035:
#line 4493 "preproc.y"
{ yyval.str = make1_str("end"); ;
    break;}
case 1036:
#line 4494 "preproc.y"
{ yyval.str = make1_str("explain"); ;
    break;}
case 1037:
#line 4495 "preproc.y"
{ yyval.str = make1_str("extend"); ;
    break;}
case 1038:
#line 4496 "preproc.y"
{ yyval.str = make1_str("false"); ;
    break;}
case 1039:
#line 4497 "preproc.y"
{ yyval.str = make1_str("foreign"); ;
    break;}
case 1040:
#line 4498 "preproc.y"
{ yyval.str = make1_str("group"); ;
    break;}
case 1041:
#line 4499 "preproc.y"
{ yyval.str = make1_str("listen"); ;
    break;}
case 1042:
#line 4500 "preproc.y"
{ yyval.str = make1_str("load"); ;
    break;}
case 1043:
#line 4501 "preproc.y"
{ yyval.str = make1_str("lock"); ;
    break;}
case 1044:
#line 4502 "preproc.y"
{ yyval.str = make1_str("move"); ;
    break;}
case 1045:
#line 4503 "preproc.y"
{ yyval.str = make1_str("new"); ;
    break;}
case 1046:
#line 4504 "preproc.y"
{ yyval.str = make1_str("none"); ;
    break;}
case 1047:
#line 4505 "preproc.y"
{ yyval.str = make1_str("nullif"); ;
    break;}
case 1048:
#line 4506 "preproc.y"
{ yyval.str = make1_str("order"); ;
    break;}
case 1049:
#line 4507 "preproc.y"
{ yyval.str = make1_str("position"); ;
    break;}
case 1050:
#line 4508 "preproc.y"
{ yyval.str = make1_str("precision"); ;
    break;}
case 1051:
#line 4509 "preproc.y"
{ yyval.str = make1_str("reset"); ;
    break;}
case 1052:
#line 4510 "preproc.y"
{ yyval.str = make1_str("setof"); ;
    break;}
case 1053:
#line 4511 "preproc.y"
{ yyval.str = make1_str("show"); ;
    break;}
case 1054:
#line 4512 "preproc.y"
{ yyval.str = make1_str("table"); ;
    break;}
case 1055:
#line 4513 "preproc.y"
{ yyval.str = make1_str("then"); ;
    break;}
case 1056:
#line 4514 "preproc.y"
{ yyval.str = make1_str("transaction"); ;
    break;}
case 1057:
#line 4515 "preproc.y"
{ yyval.str = make1_str("true"); ;
    break;}
case 1058:
#line 4516 "preproc.y"
{ yyval.str = make1_str("vacuum"); ;
    break;}
case 1059:
#line 4517 "preproc.y"
{ yyval.str = make1_str("verbose"); ;
    break;}
case 1060:
#line 4518 "preproc.y"
{ yyval.str = make1_str("when"); ;
    break;}
case 1061:
#line 4522 "preproc.y"
{
					if (QueryIsRule)
						yyval.str = make1_str("current");
					else
						yyerror("CURRENT used in non-rule query");
				;
    break;}
case 1062:
#line 4529 "preproc.y"
{
					if (QueryIsRule)
						yyval.str = make1_str("new");
					else
						yyerror("NEW used in non-rule query");
				;
    break;}
case 1063:
#line 4545 "preproc.y"
{
			yyval.str = make5_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str, make1_str(","), yyvsp[-1].str);
                ;
    break;}
case 1064:
#line 4549 "preproc.y"
{
                	yyval.str = make1_str("NULL,NULL,NULL,\"DEFAULT\"");
                ;
    break;}
case 1065:
#line 4554 "preproc.y"
{
		       yyval.str = make3_str(make1_str("NULL,"), yyvsp[0].str, make1_str(",NULL"));
		;
    break;}
case 1066:
#line 4559 "preproc.y"
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
#line 4570 "preproc.y"
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
#line 4593 "preproc.y"
{
		  yyval.str = yyvsp[0].str;
		;
    break;}
case 1069:
#line 4597 "preproc.y"
{
		  yyval.str = mm_strdup(yyvsp[0].str);
		  yyval.str[0] = '\"';
		  yyval.str[strlen(yyval.str) - 1] = '\"';
		  free(yyvsp[0].str);
		;
    break;}
case 1070:
#line 4605 "preproc.y"
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
#line 4622 "preproc.y"
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
#line 4632 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1073:
#line 4633 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1074:
#line 4635 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1075:
#line 4636 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str); ;
    break;}
case 1076:
#line 4638 "preproc.y"
{ yyval.str = make2_str(make1_str(":"), yyvsp[0].str); ;
    break;}
case 1077:
#line 4639 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1078:
#line 4641 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1079:
#line 4642 "preproc.y"
{ yyval.str = make1_str("NULL"); ;
    break;}
case 1080:
#line 4644 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1081:
#line 4645 "preproc.y"
{ yyval.str = make1_str("NULL,NULL"); ;
    break;}
case 1082:
#line 4648 "preproc.y"
{
                        yyval.str = make2_str(yyvsp[0].str, make1_str(",NULL"));
	        ;
    break;}
case 1083:
#line 4652 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1084:
#line 4656 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-3].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1085:
#line 4660 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1086:
#line 4664 "preproc.y"
{ if (yyvsp[0].str[0] == '\"')
				yyval.str = yyvsp[0].str;
			  else
				yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\""));
			;
    break;}
case 1087:
#line 4669 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1088:
#line 4670 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1089:
#line 4673 "preproc.y"
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
#line 4697 "preproc.y"
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
#line 4709 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1092:
#line 4716 "preproc.y"
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
case 1093:
#line 4758 "preproc.y"
{ yyval.str = make3_str(make1_str("ECPGdeallocate(__LINE__, \""), yyvsp[0].str, make1_str("\");")); ;
    break;}
case 1094:
#line 4764 "preproc.y"
{
		fputs("/* exec sql begin declare section */", yyout);
	        output_line_number();
	;
    break;}
case 1095:
#line 4769 "preproc.y"
{
		fprintf(yyout, "%s/* exec sql end declare section */", yyvsp[-1].str);
		free(yyvsp[-1].str);
		output_line_number();
	;
    break;}
case 1096:
#line 4775 "preproc.y"
{;
    break;}
case 1097:
#line 4777 "preproc.y"
{;
    break;}
case 1098:
#line 4780 "preproc.y"
{
		yyval.str = make1_str("");
	;
    break;}
case 1099:
#line 4784 "preproc.y"
{
		yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
	;
    break;}
case 1100:
#line 4789 "preproc.y"
{
		actual_storage[struct_level] = mm_strdup(yyvsp[0].str);
	;
    break;}
case 1101:
#line 4793 "preproc.y"
{
		actual_type[struct_level].type_enum = yyvsp[0].type.type_enum;
		actual_type[struct_level].type_dimension = yyvsp[0].type.type_dimension;
		actual_type[struct_level].type_index = yyvsp[0].type.type_index;
	;
    break;}
case 1102:
#line 4799 "preproc.y"
{
 		yyval.str = cat4_str(yyvsp[-5].str, yyvsp[-3].type.type_str, yyvsp[-1].str, make1_str(";\n"));
	;
    break;}
case 1103:
#line 4803 "preproc.y"
{ yyval.str = make1_str("extern"); ;
    break;}
case 1104:
#line 4804 "preproc.y"
{ yyval.str = make1_str("static"); ;
    break;}
case 1105:
#line 4805 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1106:
#line 4806 "preproc.y"
{ yyval.str = make1_str("const"); ;
    break;}
case 1107:
#line 4807 "preproc.y"
{ yyval.str = make1_str("register"); ;
    break;}
case 1108:
#line 4808 "preproc.y"
{ yyval.str = make1_str("auto"); ;
    break;}
case 1109:
#line 4809 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1110:
#line 4812 "preproc.y"
{
			yyval.type.type_enum = yyvsp[0].type_enum;
			yyval.type.type_str = mm_strdup(ECPGtype_name(yyvsp[0].type_enum));
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1111:
#line 4819 "preproc.y"
{
			yyval.type.type_enum = ECPGt_varchar;
			yyval.type.type_str = make1_str("");
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1112:
#line 4826 "preproc.y"
{
			yyval.type.type_enum = ECPGt_struct;
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1113:
#line 4833 "preproc.y"
{
			yyval.type.type_enum = ECPGt_union;
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1114:
#line 4840 "preproc.y"
{
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_enum = ECPGt_int;
		
	yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1115:
#line 4848 "preproc.y"
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
case 1116:
#line 4860 "preproc.y"
{
		yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1117:
#line 4864 "preproc.y"
{ yyval.str = cat2_str(make1_str("enum"), yyvsp[0].str); ;
    break;}
case 1118:
#line 4867 "preproc.y"
{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1119:
#line 4874 "preproc.y"
{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1120:
#line 4881 "preproc.y"
{
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    yyval.str = cat2_str(make1_str("struct"), yyvsp[0].str);
	;
    break;}
case 1121:
#line 4889 "preproc.y"
{
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    yyval.str = cat2_str(make1_str("union"), yyvsp[0].str);
	;
    break;}
case 1122:
#line 4896 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1123:
#line 4897 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1124:
#line 4899 "preproc.y"
{ yyval.type_enum = ECPGt_short; ;
    break;}
case 1125:
#line 4900 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_short; ;
    break;}
case 1126:
#line 4901 "preproc.y"
{ yyval.type_enum = ECPGt_int; ;
    break;}
case 1127:
#line 4902 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_int; ;
    break;}
case 1128:
#line 4903 "preproc.y"
{ yyval.type_enum = ECPGt_long; ;
    break;}
case 1129:
#line 4904 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_long; ;
    break;}
case 1130:
#line 4905 "preproc.y"
{ yyval.type_enum = ECPGt_float; ;
    break;}
case 1131:
#line 4906 "preproc.y"
{ yyval.type_enum = ECPGt_double; ;
    break;}
case 1132:
#line 4907 "preproc.y"
{ yyval.type_enum = ECPGt_bool; ;
    break;}
case 1133:
#line 4908 "preproc.y"
{ yyval.type_enum = ECPGt_char; ;
    break;}
case 1134:
#line 4909 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_char; ;
    break;}
case 1135:
#line 4911 "preproc.y"
{ yyval.type_enum = ECPGt_varchar; ;
    break;}
case 1136:
#line 4914 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 1137:
#line 4918 "preproc.y"
{
		yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
	;
    break;}
case 1138:
#line 4923 "preproc.y"
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
case 1139:
#line 4997 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1140:
#line 4998 "preproc.y"
{ yyval.str = make2_str(make1_str("="), yyvsp[0].str); ;
    break;}
case 1141:
#line 5000 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1142:
#line 5001 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 1143:
#line 5008 "preproc.y"
{
		/* this is only supported for compatibility */
		yyval.str = cat3_str(make1_str("/* declare statement"), yyvsp[0].str, make1_str("*/"));
	;
    break;}
case 1144:
#line 5015 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1145:
#line 5017 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1146:
#line 5018 "preproc.y"
{ yyval.str = make1_str("CURRENT"); ;
    break;}
case 1147:
#line 5019 "preproc.y"
{ yyval.str = make1_str("ALL"); ;
    break;}
case 1148:
#line 5020 "preproc.y"
{ yyval.str = make1_str("CURRENT"); ;
    break;}
case 1149:
#line 5022 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1150:
#line 5023 "preproc.y"
{ yyval.str = make1_str("DEFAULT"); ;
    break;}
case 1151:
#line 5029 "preproc.y"
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
case 1152:
#line 5042 "preproc.y"
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
#line 5053 "preproc.y"
{
		yyval.str = make1_str("?");
	;
    break;}
case 1155:
#line 5058 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1156:
#line 5064 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1157:
#line 5069 "preproc.y"
{
		yyval.str = yyvsp[-1].str;
;
    break;}
case 1158:
#line 5073 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1159:
#line 5074 "preproc.y"
{
					/* yyerror ("open cursor with variables not implemented yet"); */
					yyval.str = make1_str("");
				;
    break;}
case 1162:
#line 5086 "preproc.y"
{
		yyval.str = make4_str(make1_str("\""), yyvsp[-2].str, make1_str("\", "), yyvsp[0].str);
	;
    break;}
case 1163:
#line 5096 "preproc.y"
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
#line 5112 "preproc.y"
{
				yyval.str = yyvsp[0].str;
                        ;
    break;}
case 1165:
#line 5120 "preproc.y"
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
#line 5162 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1167:
#line 5168 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1168:
#line 5174 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1169:
#line 5180 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1170:
#line 5186 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 1171:
#line 5194 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1172:
#line 5200 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1173:
#line 5206 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1174:
#line 5212 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1175:
#line 5218 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 1176:
#line 5224 "preproc.y"
{ yyval.str = make1_str("reference"); ;
    break;}
case 1177:
#line 5225 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1178:
#line 5228 "preproc.y"
{
		yyval.type.type_str = make1_str("char");
                yyval.type.type_enum = ECPGt_char;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1179:
#line 5235 "preproc.y"
{
		yyval.type.type_str = make1_str("varchar");
                yyval.type.type_enum = ECPGt_varchar;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1180:
#line 5242 "preproc.y"
{
		yyval.type.type_str = make1_str("float");
                yyval.type.type_enum = ECPGt_float;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1181:
#line 5249 "preproc.y"
{
		yyval.type.type_str = make1_str("double");
                yyval.type.type_enum = ECPGt_double;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1182:
#line 5256 "preproc.y"
{
		yyval.type.type_str = make1_str("int");
       	        yyval.type.type_enum = ECPGt_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1183:
#line 5263 "preproc.y"
{
		yyval.type.type_str = make1_str("int");
       	        yyval.type.type_enum = ECPGt_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1184:
#line 5270 "preproc.y"
{
		yyval.type.type_str = make1_str("short");
       	        yyval.type.type_enum = ECPGt_short;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1185:
#line 5277 "preproc.y"
{
		yyval.type.type_str = make1_str("long");
       	        yyval.type.type_enum = ECPGt_long;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1186:
#line 5284 "preproc.y"
{
		yyval.type.type_str = make1_str("bool");
       	        yyval.type.type_enum = ECPGt_bool;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1187:
#line 5291 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned int");
       	        yyval.type.type_enum = ECPGt_unsigned_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1188:
#line 5298 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned short");
       	        yyval.type.type_enum = ECPGt_unsigned_short;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1189:
#line 5305 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned long");
       	        yyval.type.type_enum = ECPGt_unsigned_long;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1190:
#line 5312 "preproc.y"
{
		struct_member_list[struct_level++] = NULL;
		if (struct_level >= STRUCT_DEPTH)
        		yyerror("Too many levels in nested structure definition");
	;
    break;}
case 1191:
#line 5317 "preproc.y"
{
		ECPGfree_struct_member(struct_member_list[struct_level--]);
		yyval.type.type_str = cat3_str(make1_str("struct {"), yyvsp[-1].str, make1_str("}"));
		yyval.type.type_enum = ECPGt_struct;
                yyval.type.type_index = -1;
                yyval.type.type_dimension = -1;
	;
    break;}
case 1192:
#line 5325 "preproc.y"
{
		struct_member_list[struct_level++] = NULL;
		if (struct_level >= STRUCT_DEPTH)
        		yyerror("Too many levels in nested structure definition");
	;
    break;}
case 1193:
#line 5330 "preproc.y"
{
		ECPGfree_struct_member(struct_member_list[struct_level--]);
		yyval.type.type_str = cat3_str(make1_str("union {"), yyvsp[-1].str, make1_str("}"));
		yyval.type.type_enum = ECPGt_union;
                yyval.type.type_index = -1;
                yyval.type.type_dimension = -1;
	;
    break;}
case 1194:
#line 5338 "preproc.y"
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
#line 5351 "preproc.y"
{
		yyval.str = make1_str("");
	;
    break;}
case 1198:
#line 5355 "preproc.y"
{
		yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
	;
    break;}
case 1199:
#line 5361 "preproc.y"
{
		actual_type[struct_level].type_enum = yyvsp[0].type.type_enum;
		actual_type[struct_level].type_dimension = yyvsp[0].type.type_dimension;
		actual_type[struct_level].type_index = yyvsp[0].type.type_index;
	;
    break;}
case 1200:
#line 5367 "preproc.y"
{
		yyval.str = cat3_str(yyvsp[-3].type.type_str, yyvsp[-1].str, make1_str(";"));
	;
    break;}
case 1201:
#line 5372 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 1202:
#line 5376 "preproc.y"
{
		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
	;
    break;}
case 1203:
#line 5381 "preproc.y"
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
#line 5452 "preproc.y"
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
#line 5506 "preproc.y"
{
	when_error.code = yyvsp[0].action.code;
	when_error.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever sqlerror "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1206:
#line 5511 "preproc.y"
{
	when_nf.code = yyvsp[0].action.code;
	when_nf.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever not found "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1207:
#line 5516 "preproc.y"
{
	when_warn.code = yyvsp[0].action.code;
	when_warn.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever sql_warning "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1208:
#line 5522 "preproc.y"
{
	yyval.action.code = W_NOTHING;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("continue");
;
    break;}
case 1209:
#line 5527 "preproc.y"
{
	yyval.action.code = W_SQLPRINT;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("sqlprint");
;
    break;}
case 1210:
#line 5532 "preproc.y"
{
	yyval.action.code = W_STOP;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("stop");
;
    break;}
case 1211:
#line 5537 "preproc.y"
{
        yyval.action.code = W_GOTO;
        yyval.action.command = strdup(yyvsp[0].str);
	yyval.action.str = cat2_str(make1_str("goto "), yyvsp[0].str);
;
    break;}
case 1212:
#line 5542 "preproc.y"
{
        yyval.action.code = W_GOTO;
        yyval.action.command = strdup(yyvsp[0].str);
	yyval.action.str = cat2_str(make1_str("goto "), yyvsp[0].str);
;
    break;}
case 1213:
#line 5547 "preproc.y"
{
	yyval.action.code = W_DO;
	yyval.action.command = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
	yyval.action.str = cat2_str(make1_str("do"), mm_strdup(yyval.action.command));
;
    break;}
case 1214:
#line 5552 "preproc.y"
{
        yyval.action.code = W_BREAK;
        yyval.action.command = NULL;
        yyval.action.str = make1_str("break");
;
    break;}
case 1215:
#line 5557 "preproc.y"
{
	yyval.action.code = W_DO;
	yyval.action.command = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
	yyval.action.str = cat2_str(make1_str("call"), mm_strdup(yyval.action.command));
;
    break;}
case 1216:
#line 5565 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 1217:
#line 5569 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 1218:
#line 5571 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 1219:
#line 5573 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 1220:
#line 5577 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 1221:
#line 5579 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 1222:
#line 5581 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 1223:
#line 5583 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 1224:
#line 5585 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 1225:
#line 5587 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 1226:
#line 5589 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 1227:
#line 5591 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 1228:
#line 5595 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 1229:
#line 5597 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 1230:
#line 5599 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 1231:
#line 5603 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 1232:
#line 5607 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 1233:
#line 5609 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 1234:
#line 5611 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 1235:
#line 5613 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 1236:
#line 5615 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1237:
#line 5617 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1238:
#line 5619 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make1_str("(*)")); 
				;
    break;}
case 1239:
#line 5623 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 1240:
#line 5627 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1241:
#line 5631 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 1242:
#line 5635 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 1243:
#line 5639 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 1244:
#line 5645 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 1245:
#line 5649 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 1246:
#line 5655 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 1247:
#line 5659 "preproc.y"
{
					yyval.str = make3_str(make1_str("exists("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1248:
#line 5663 "preproc.y"
{
					yyval.str = make3_str(make1_str("extract("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1249:
#line 5667 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1250:
#line 5671 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1251:
#line 5676 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1252:
#line 5680 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1253:
#line 5684 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1254:
#line 5688 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1255:
#line 5692 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 1256:
#line 5694 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 1257:
#line 5696 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 1258:
#line 5698 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 1259:
#line 5705 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); }
				;
    break;}
case 1260:
#line 5709 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); }
				;
    break;}
case 1261:
#line 5713 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); }
				;
    break;}
case 1262:
#line 5717 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); }
				;
    break;}
case 1263:
#line 5721 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 1264:
#line 5725 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 1265:
#line 5729 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1266:
#line 5733 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("not in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1267:
#line 5737 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-4].str, yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1268:
#line 5741 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("+("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1269:
#line 5745 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("-("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1270:
#line 5749 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("/("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1271:
#line 5753 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("*("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1272:
#line 5757 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("<("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1273:
#line 5761 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(">("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1274:
#line 5765 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("=("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1275:
#line 5769 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("any ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1276:
#line 5773 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1277:
#line 5777 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1278:
#line 5781 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1279:
#line 5785 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1280:
#line 5789 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1281:
#line 5793 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1282:
#line 5797 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1283:
#line 5801 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("all ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1284:
#line 5805 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1285:
#line 5809 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("-all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1286:
#line 5813 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1287:
#line 5817 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("*all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1288:
#line 5821 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("<all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1289:
#line 5825 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(">all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1290:
#line 5829 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1291:
#line 5833 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 1292:
#line 5835 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 1293:
#line 5837 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 1294:
#line 5839 "preproc.y"
{ 	yyval.str = yyvsp[0].str; ;
    break;}
case 1297:
#line 5844 "preproc.y"
{ reset_variables();;
    break;}
case 1298:
#line 5846 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1299:
#line 5847 "preproc.y"
{ yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1300:
#line 5849 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1301:
#line 5850 "preproc.y"
{ yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1302:
#line 5852 "preproc.y"
{
		add_variable(&argsresult, find_variable(yyvsp[-1].str), (yyvsp[0].str == NULL) ? &no_indicator : find_variable(yyvsp[0].str)); 
;
    break;}
case 1303:
#line 5856 "preproc.y"
{
		add_variable(&argsinsert, find_variable(yyvsp[-1].str), (yyvsp[0].str == NULL) ? &no_indicator : find_variable(yyvsp[0].str)); 
;
    break;}
case 1304:
#line 5860 "preproc.y"
{
		add_variable(&argsinsert, find_variable(yyvsp[0].str), &no_indicator); 
		yyval.str = make1_str("?");
;
    break;}
case 1305:
#line 5865 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1306:
#line 5867 "preproc.y"
{ yyval.str = NULL; ;
    break;}
case 1307:
#line 5868 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1308:
#line 5869 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1309:
#line 5870 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1310:
#line 5872 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1311:
#line 5873 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1312:
#line 5878 "preproc.y"
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
{
			yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str);
		;
    break;}
case 1317:
#line 5888 "preproc.y"
{ yyval.str = make1_str(";"); ;
    break;}
case 1318:
#line 5890 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1319:
#line 5891 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1320:
#line 5892 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1321:
#line 5893 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1322:
#line 5894 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 1323:
#line 5895 "preproc.y"
{ yyval.str = make1_str("auto"); ;
    break;}
case 1324:
#line 5896 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 1325:
#line 5897 "preproc.y"
{ yyval.str = make1_str("char"); ;
    break;}
case 1326:
#line 5898 "preproc.y"
{ yyval.str = make1_str("const"); ;
    break;}
case 1327:
#line 5899 "preproc.y"
{ yyval.str = make1_str("double"); ;
    break;}
case 1328:
#line 5900 "preproc.y"
{ yyval.str = make1_str("enum"); ;
    break;}
case 1329:
#line 5901 "preproc.y"
{ yyval.str = make1_str("extern"); ;
    break;}
case 1330:
#line 5902 "preproc.y"
{ yyval.str = make1_str("float"); ;
    break;}
case 1331:
#line 5903 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 1332:
#line 5904 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 1333:
#line 5905 "preproc.y"
{ yyval.str = make1_str("register"); ;
    break;}
case 1334:
#line 5906 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 1335:
#line 5907 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1336:
#line 5908 "preproc.y"
{ yyval.str = make1_str("static"); ;
    break;}
case 1337:
#line 5909 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 1338:
#line 5910 "preproc.y"
{ yyval.str = make1_str("union"); ;
    break;}
case 1339:
#line 5911 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 1340:
#line 5912 "preproc.y"
{ yyval.str = make1_str("varchar"); ;
    break;}
case 1341:
#line 5913 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 1342:
#line 5914 "preproc.y"
{ yyval.str = make1_str("["); ;
    break;}
case 1343:
#line 5915 "preproc.y"
{ yyval.str = make1_str("]"); ;
    break;}
case 1344:
#line 5916 "preproc.y"
{ yyval.str = make1_str("("); ;
    break;}
case 1345:
#line 5917 "preproc.y"
{ yyval.str = make1_str(")"); ;
    break;}
case 1346:
#line 5918 "preproc.y"
{ yyval.str = make1_str("="); ;
    break;}
case 1347:
#line 5919 "preproc.y"
{ yyval.str = make1_str(","); ;
    break;}
case 1348:
#line 5921 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1349:
#line 5922 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\""));;
    break;}
case 1350:
#line 5923 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1351:
#line 5924 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1352:
#line 5925 "preproc.y"
{ yyval.str = make1_str(","); ;
    break;}
case 1353:
#line 5927 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1354:
#line 5928 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1355:
#line 5929 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1356:
#line 5930 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1357:
#line 5931 "preproc.y"
{ yyval.str = make3_str(make1_str("{"), yyvsp[-1].str, make1_str("}")); ;
    break;}
case 1358:
#line 5933 "preproc.y"
{
    braces_open++;
    yyval.str = make1_str("{");
;
    break;}
case 1359:
#line 5938 "preproc.y"
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
#line 5943 "preproc.y"


void yyerror(char * error)
{
    fprintf(stderr, "%s:%d: %s\n", input_filename, yylineno, error);
    exit(PARSE_ERROR);
}
