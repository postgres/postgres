
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



#define	YYFINAL		2437
#define	YYFLAG		-32768
#define	YYNTBASE	304

#define YYTRANSLATE(x) ((unsigned)(x) <= 539 ? yytranslate[x] : 667)

static const short yytranslate[] = {     0,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,   291,     2,     2,   300,
   301,   289,   287,   299,   288,   296,   290,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,   293,   294,   285,
   284,   286,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
   297,     2,   298,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,   302,   292,   303,     2,     2,     2,     2,     2,
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
   276,   277,   278,   279,   280,   281,   282,   283,   295
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
   412,   416,   420,   424,   428,   432,   436,   440,   444,   447,
   450,   454,   461,   465,   469,   474,   478,   481,   484,   486,
   488,   493,   495,   500,   502,   504,   508,   510,   515,   520,
   526,   537,   541,   543,   545,   547,   549,   552,   556,   560,
   564,   568,   572,   576,   580,   584,   587,   590,   594,   601,
   605,   609,   614,   618,   622,   627,   631,   635,   638,   641,
   644,   647,   651,   654,   659,   663,   667,   672,   677,   683,
   690,   696,   703,   707,   709,   711,   714,   717,   718,   721,
   723,   724,   728,   732,   735,   737,   740,   743,   748,   749,
   757,   761,   762,   766,   768,   770,   775,   778,   779,   782,
   784,   787,   790,   793,   796,   798,   800,   802,   805,   807,
   810,   820,   822,   823,   828,   843,   845,   847,   849,   853,
   859,   861,   863,   865,   869,   871,   872,   874,   876,   878,
   882,   883,   885,   887,   889,   891,   897,   901,   904,   906,
   908,   910,   912,   914,   916,   918,   920,   924,   926,   930,
   934,   936,   940,   942,   944,   946,   948,   951,   955,   959,
   966,   971,   973,   975,   977,   979,   980,   982,   985,   987,
   989,   991,   992,   995,   998,   999,  1007,  1010,  1012,  1014,
  1016,  1020,  1022,  1024,  1026,  1028,  1030,  1032,  1035,  1037,
  1041,  1042,  1049,  1061,  1063,  1064,  1067,  1068,  1070,  1072,
  1076,  1078,  1085,  1089,  1092,  1095,  1096,  1098,  1101,  1102,
  1107,  1119,  1122,  1123,  1127,  1130,  1132,  1136,  1139,  1141,
  1142,  1146,  1148,  1150,  1152,  1154,  1159,  1161,  1163,  1168,
  1175,  1177,  1179,  1181,  1183,  1185,  1187,  1189,  1191,  1193,
  1195,  1197,  1201,  1205,  1209,  1219,  1221,  1222,  1224,  1225,
  1226,  1240,  1242,  1244,  1246,  1250,  1254,  1256,  1258,  1261,
  1265,  1268,  1270,  1272,  1274,  1276,  1280,  1282,  1284,  1286,
  1288,  1290,  1292,  1293,  1296,  1299,  1302,  1305,  1308,  1311,
  1314,  1317,  1320,  1322,  1324,  1325,  1331,  1334,  1341,  1345,
  1349,  1350,  1354,  1355,  1357,  1359,  1360,  1362,  1364,  1365,
  1369,  1374,  1378,  1384,  1386,  1387,  1389,  1390,  1394,  1395,
  1397,  1401,  1405,  1407,  1409,  1411,  1413,  1415,  1417,  1422,
  1427,  1430,  1432,  1440,  1445,  1449,  1450,  1454,  1456,  1459,
  1464,  1468,  1477,  1485,  1492,  1494,  1495,  1502,  1510,  1512,
  1514,  1516,  1519,  1520,  1523,  1524,  1527,  1530,  1533,  1538,
  1542,  1544,  1548,  1553,  1558,  1567,  1572,  1575,  1576,  1578,
  1579,  1581,  1582,  1584,  1588,  1590,  1591,  1595,  1596,  1598,
  1602,  1605,  1608,  1611,  1614,  1616,  1618,  1619,  1624,  1629,
  1632,  1637,  1640,  1641,  1643,  1645,  1647,  1649,  1651,  1653,
  1654,  1656,  1658,  1662,  1666,  1667,  1670,  1671,  1675,  1676,
  1679,  1680,  1683,  1684,  1688,  1690,  1692,  1696,  1698,  1702,
  1705,  1707,  1709,  1714,  1717,  1720,  1722,  1727,  1732,  1736,
  1739,  1742,  1745,  1747,  1749,  1750,  1752,  1753,  1758,  1761,
  1765,  1767,  1769,  1772,  1773,  1775,  1778,  1782,  1787,  1788,
  1792,  1797,  1798,  1801,  1803,  1806,  1808,  1810,  1812,  1814,
  1816,  1818,  1820,  1822,  1824,  1826,  1828,  1830,  1832,  1834,
  1836,  1838,  1840,  1842,  1844,  1846,  1848,  1850,  1852,  1854,
  1856,  1858,  1860,  1862,  1864,  1866,  1868,  1870,  1872,  1874,
  1876,  1878,  1880,  1883,  1886,  1889,  1892,  1894,  1897,  1899,
  1901,  1905,  1906,  1912,  1916,  1917,  1923,  1927,  1928,  1933,
  1935,  1940,  1943,  1945,  1949,  1952,  1954,  1955,  1959,  1960,
  1963,  1964,  1966,  1969,  1971,  1974,  1976,  1978,  1980,  1982,
  1984,  1986,  1990,  1991,  1993,  1997,  2001,  2005,  2009,  2013,
  2017,  2021,  2022,  2024,  2026,  2034,  2043,  2052,  2060,  2068,
  2072,  2074,  2076,  2078,  2080,  2082,  2084,  2086,  2088,  2090,
  2092,  2094,  2098,  2100,  2103,  2105,  2107,  2109,  2112,  2116,
  2120,  2124,  2128,  2132,  2136,  2140,  2144,  2148,  2151,  2154,
  2158,  2165,  2169,  2173,  2177,  2182,  2185,  2188,  2193,  2197,
  2202,  2204,  2206,  2211,  2213,  2218,  2220,  2222,  2227,  2232,
  2237,  2242,  2248,  2254,  2260,  2265,  2268,  2272,  2275,  2280,
  2284,  2289,  2293,  2298,  2304,  2311,  2317,  2324,  2330,  2336,
  2342,  2348,  2354,  2360,  2366,  2372,  2378,  2385,  2392,  2399,
  2406,  2413,  2420,  2427,  2434,  2441,  2448,  2455,  2462,  2469,
  2476,  2483,  2490,  2497,  2504,  2508,  2512,  2515,  2517,  2519,
  2522,  2524,  2526,  2529,  2533,  2537,  2541,  2545,  2549,  2552,
  2555,  2559,  2566,  2570,  2574,  2577,  2580,  2584,  2589,  2591,
  2593,  2598,  2600,  2605,  2607,  2609,  2614,  2619,  2625,  2631,
  2637,  2642,  2644,  2649,  2656,  2657,  2659,  2663,  2667,  2671,
  2672,  2674,  2676,  2678,  2680,  2684,  2685,  2688,  2690,  2693,
  2697,  2701,  2705,  2709,  2713,  2716,  2720,  2727,  2731,  2735,
  2738,  2741,  2743,  2747,  2752,  2757,  2762,  2768,  2774,  2780,
  2785,  2789,  2790,  2793,  2794,  2797,  2798,  2802,  2805,  2807,
  2809,  2811,  2813,  2817,  2819,  2821,  2823,  2827,  2833,  2840,
  2845,  2848,  2850,  2855,  2858,  2859,  2862,  2864,  2865,  2869,
  2873,  2875,  2879,  2883,  2887,  2889,  2891,  2896,  2899,  2903,
  2907,  2909,  2913,  2915,  2919,  2921,  2923,  2924,  2926,  2928,
  2930,  2932,  2934,  2936,  2938,  2940,  2942,  2944,  2946,  2948,
  2950,  2953,  2955,  2957,  2959,  2962,  2964,  2966,  2968,  2970,
  2972,  2974,  2976,  2978,  2980,  2982,  2984,  2986,  2988,  2990,
  2992,  2994,  2996,  2998,  3000,  3002,  3004,  3006,  3008,  3010,
  3012,  3014,  3016,  3018,  3020,  3022,  3024,  3026,  3028,  3030,
  3032,  3034,  3036,  3038,  3040,  3042,  3044,  3046,  3048,  3050,
  3052,  3054,  3056,  3058,  3060,  3062,  3064,  3066,  3068,  3070,
  3072,  3074,  3076,  3078,  3080,  3082,  3084,  3086,  3088,  3090,
  3092,  3094,  3096,  3098,  3100,  3102,  3104,  3106,  3108,  3110,
  3112,  3114,  3116,  3118,  3120,  3122,  3124,  3126,  3128,  3130,
  3132,  3134,  3136,  3138,  3140,  3142,  3144,  3146,  3148,  3150,
  3152,  3154,  3156,  3158,  3160,  3162,  3164,  3166,  3168,  3170,
  3172,  3174,  3176,  3178,  3180,  3182,  3184,  3186,  3188,  3190,
  3192,  3194,  3196,  3198,  3200,  3202,  3204,  3206,  3208,  3210,
  3212,  3214,  3216,  3218,  3220,  3222,  3224,  3226,  3228,  3230,
  3232,  3234,  3236,  3238,  3240,  3242,  3244,  3246,  3248,  3250,
  3252,  3254,  3256,  3258,  3264,  3268,  3271,  3275,  3282,  3284,
  3286,  3289,  3292,  3294,  3295,  3297,  3301,  3304,  3305,  3308,
  3309,  3312,  3313,  3315,  3319,  3324,  3328,  3330,  3332,  3334,
  3336,  3339,  3340,  3348,  3352,  3353,  3358,  3364,  3370,  3371,
  3374,  3375,  3376,  3383,  3385,  3387,  3389,  3391,  3393,  3395,
  3396,  3398,  3400,  3402,  3404,  3406,  3408,  3413,  3416,  3421,
  3426,  3429,  3432,  3433,  3435,  3437,  3440,  3442,  3445,  3447,
  3450,  3452,  3454,  3456,  3458,  3461,  3463,  3465,  3469,  3474,
  3475,  3478,  3479,  3481,  3485,  3488,  3490,  3492,  3494,  3495,
  3497,  3499,  3503,  3504,  3509,  3511,  3513,  3516,  3520,  3521,
  3524,  3526,  3530,  3535,  3538,  3542,  3549,  3553,  3557,  3562,
  3567,  3568,  3572,  3576,  3581,  3586,  3587,  3589,  3590,  3592,
  3594,  3596,  3598,  3601,  3603,  3606,  3609,  3611,  3614,  3617,
  3620,  3621,  3627,  3628,  3634,  3636,  3638,  3639,  3640,  3643,
  3644,  3649,  3651,  3655,  3659,  3666,  3670,  3675,  3679,  3681,
  3683,  3685,  3688,  3692,  3698,  3701,  3707,  3710,  3712,  3714,
  3716,  3719,  3723,  3727,  3731,  3735,  3739,  3743,  3747,  3751,
  3754,  3757,  3761,  3768,  3772,  3776,  3780,  3785,  3788,  3791,
  3796,  3800,  3805,  3807,  3809,  3814,  3816,  3821,  3823,  3828,
  3833,  3838,  3843,  3849,  3855,  3861,  3866,  3869,  3873,  3876,
  3881,  3885,  3890,  3894,  3899,  3905,  3912,  3918,  3925,  3931,
  3937,  3943,  3949,  3955,  3961,  3967,  3973,  3979,  3986,  3993,
  4000,  4007,  4014,  4021,  4028,  4035,  4042,  4049,  4056,  4063,
  4070,  4077,  4084,  4091,  4098,  4105,  4109,  4113,  4116,  4118,
  4120,  4124,  4126,  4127,  4130,  4132,  4135,  4138,  4141,  4143,
  4145,  4146,  4148,  4151,  4154,  4156,  4158,  4160,  4162,  4164,
  4167,  4169,  4171,  4173,  4175,  4177,  4179,  4181,  4183,  4185,
  4187,  4189,  4191,  4193,  4195,  4197,  4199,  4201,  4203,  4205,
  4207,  4209,  4211,  4213,  4215,  4217,  4219,  4221,  4223,  4225,
  4227,  4229,  4231,  4233,  4235,  4237,  4239,  4241,  4243,  4245,
  4247,  4249,  4253,  4255
};

static const short yyrhs[] = {   305,
     0,     0,   305,   306,     0,   649,   307,   308,    27,     0,
   649,   308,    27,     0,   594,     0,   661,     0,   659,     0,
   665,     0,   666,     0,     3,   580,     0,   323,     0,   310,
     0,   325,     0,   326,     0,   332,     0,   355,     0,   359,
     0,   365,     0,   368,     0,   309,     0,   448,     0,   378,
     0,   386,     0,   367,     0,   377,     0,   311,     0,   407,
     0,   454,     0,   387,     0,   391,     0,   398,     0,   436,
     0,   437,     0,   462,     0,   408,     0,   416,     0,   419,
     0,   418,     0,   414,     0,   423,     0,   397,     0,   455,
     0,   426,     0,   438,     0,   440,     0,   441,     0,   442,
     0,   447,     0,   449,     0,   318,     0,   321,     0,   322,
     0,   579,     0,   592,     0,   593,     0,   617,     0,   618,
     0,   621,     0,   624,     0,   625,     0,   628,     0,   629,
     0,   630,     0,   631,     0,   644,     0,   645,     0,    85,
   190,   574,   312,   313,   314,   316,   317,     0,    64,   190,
   574,   312,   313,   314,   316,   317,     0,   101,   190,   574,
     0,   198,   252,   574,     0,     0,   214,     0,   243,     0,
     0,   215,     0,   244,     0,     0,   315,   299,   574,     0,
   574,     0,   119,   116,   315,     0,     0,   271,   269,   573,
     0,     0,   173,   576,   182,   319,     0,   173,   576,   284,
   319,     0,   173,   178,   201,   320,     0,   173,   184,   127,
   133,   164,   576,     0,   173,   184,   127,   133,   576,     0,
   173,   139,   446,     0,   573,     0,    96,     0,   573,     0,
    96,     0,   135,     0,   263,   576,     0,   263,   178,   201,
     0,   263,   184,   127,   133,     0,   256,   576,     0,   256,
   178,   201,     0,   256,   184,   127,   133,     0,    64,   175,
   560,   484,   324,     0,    62,   425,   336,     0,    62,   300,
   334,   301,     0,   101,   425,   576,     0,    64,   425,   576,
   173,    96,   343,     0,    64,   425,   576,   101,    96,     0,
    62,   345,     0,    79,   559,     0,   213,   329,   560,   330,
   327,   328,   331,     0,   182,     0,   113,     0,   573,     0,
   266,     0,   267,     0,   210,     0,     0,   198,   250,     0,
     0,   191,   218,   573,     0,     0,    85,   333,   175,   560,
   300,   334,   301,   354,     0,   176,     0,     0,   334,   299,
   335,     0,   335,     0,     0,   336,     0,   344,     0,   576,
   508,   337,     0,   576,   260,   339,     0,   338,     0,     0,
   338,   340,     0,   340,     0,   159,   129,     0,     0,    84,
   566,   341,     0,   341,     0,    78,   300,   347,   301,     0,
    96,   147,     0,    96,   343,     0,   145,   147,     0,   188,
     0,   159,   129,     0,   165,   576,   458,   350,   351,     0,
   342,   299,   343,     0,   343,     0,   569,     0,   288,   343,
     0,   343,   287,   343,     0,   343,   288,   343,     0,   343,
   290,   343,     0,   343,   291,   343,     0,   343,   289,   343,
     0,   343,   284,   343,     0,   343,   285,   343,     0,   343,
   286,   343,     0,   294,   343,     0,   292,   343,     0,   343,
    59,   508,     0,    75,   300,   343,    67,   508,   301,     0,
   300,   343,   301,     0,   567,   300,   301,     0,   567,   300,
   342,   301,     0,   343,   276,   343,     0,   276,   343,     0,
   343,   276,     0,    88,     0,    89,     0,    89,   300,   571,
   301,     0,    90,     0,    90,   300,   571,   301,     0,    91,
     0,   190,     0,    84,   566,   345,     0,   345,     0,    78,
   300,   347,   301,     0,   188,   300,   459,   301,     0,   159,
   129,   300,   459,   301,     0,   112,   129,   300,   459,   301,
   165,   576,   458,   350,   351,     0,   346,   299,   347,     0,
   347,     0,   569,     0,   147,     0,   576,     0,   288,   347,
     0,   347,   287,   347,     0,   347,   288,   347,     0,   347,
   290,   347,     0,   347,   291,   347,     0,   347,   289,   347,
     0,   347,   284,   347,     0,   347,   285,   347,     0,   347,
   286,   347,     0,   294,   347,     0,   292,   347,     0,   347,
    59,   508,     0,    75,   300,   347,    67,   508,   301,     0,
   300,   347,   301,     0,   567,   300,   301,     0,   567,   300,
   346,   301,     0,   347,   276,   347,     0,   347,   134,   347,
     0,   347,   145,   134,   347,     0,   347,    65,   347,     0,
   347,   153,   347,     0,   145,   347,     0,   276,   347,     0,
   347,   276,     0,   347,   231,     0,   347,   126,   147,     0,
   347,   248,     0,   347,   126,   145,   147,     0,   347,   126,
   186,     0,   347,   126,   108,     0,   347,   126,   145,   186,
     0,   347,   126,   145,   108,     0,   347,   119,   300,   348,
   301,     0,   347,   145,   119,   300,   348,   301,     0,   347,
    70,   349,    65,   349,     0,   347,   145,    70,   349,    65,
   349,     0,   348,   299,   349,     0,   349,     0,   569,     0,
   136,   114,     0,   136,   156,     0,     0,   352,   352,     0,
   352,     0,     0,   150,    97,   353,     0,   150,   189,   353,
     0,   144,    61,     0,    73,     0,   173,    96,     0,   173,
   147,     0,   229,   300,   485,   301,     0,     0,    85,   333,
   175,   560,   356,    67,   472,     0,   300,   357,   301,     0,
     0,   357,   299,   358,     0,   358,     0,   576,     0,    85,
   261,   560,   360,     0,   360,   361,     0,     0,   211,   364,
     0,   216,     0,   227,   364,     0,   239,   364,     0,   240,
   364,     0,   264,   364,     0,   363,     0,   364,     0,   572,
     0,   288,   572,     0,   571,     0,   288,   571,     0,    85,
   366,   253,   130,   573,   226,   381,   232,   573,     0,   268,
     0,     0,   101,   253,   130,   573,     0,    85,   202,   566,
   369,   370,   150,   560,   372,   105,   162,   566,   300,   375,
   301,     0,   209,     0,   205,     0,   371,     0,   371,   153,
   371,     0,   371,   153,   371,   153,   371,     0,   122,     0,
    97,     0,   189,     0,   111,   373,   374,     0,   220,     0,
     0,   258,     0,   265,     0,   376,     0,   375,   299,   376,
     0,     0,   571,     0,   572,     0,   573,     0,   657,     0,
   101,   202,   566,   150,   560,     0,    85,   380,   379,     0,
   381,   382,     0,   251,     0,   203,     0,   206,     0,   162,
     0,   128,     0,   576,     0,   421,     0,   276,     0,   300,
   383,   301,     0,   384,     0,   383,   299,   384,     0,   381,
   284,   385,     0,   381,     0,    96,   284,   385,     0,   576,
     0,   420,     0,   362,     0,   573,     0,   262,   576,     0,
   101,   175,   485,     0,   101,   261,   485,     0,   109,   388,
   389,   390,   125,   648,     0,   241,   388,   389,   390,     0,
   224,     0,   208,     0,   166,     0,    60,     0,     0,   571,
     0,   288,   571,     0,    63,     0,   143,     0,   160,     0,
     0,   119,   566,     0,   113,   566,     0,     0,   115,   392,
   150,   485,   182,   395,   396,     0,    63,   161,     0,    63,
     0,   393,     0,   394,     0,   393,   299,   394,     0,   172,
     0,   122,     0,   189,     0,    97,     0,   259,     0,   163,
     0,   116,   576,     0,   576,     0,   198,   115,   152,     0,
     0,   167,   392,   150,   485,   113,   395,     0,    85,   399,
   228,   565,   150,   560,   400,   300,   401,   301,   409,     0,
   188,     0,     0,   191,   562,     0,     0,   402,     0,   403,
     0,   402,   299,   404,     0,   404,     0,   567,   300,   486,
   301,   405,   406,     0,   563,   405,   406,     0,   293,   508,
     0,   111,   508,     0,     0,   564,     0,   191,   564,     0,
     0,   223,   228,   565,   504,     0,    85,   225,   567,   410,
   257,   412,   409,    67,   573,   130,   573,     0,   198,   382,
     0,     0,   300,   411,   301,     0,   300,   301,     0,   575,
     0,   411,   299,   575,     0,   413,   575,     0,   262,     0,
     0,   101,   415,   566,     0,   203,     0,   228,     0,   259,
     0,   195,     0,   101,   206,   566,   417,     0,   566,     0,
   289,     0,   101,   225,   567,   410,     0,   101,   251,   420,
   300,   422,   301,     0,   276,     0,   421,     0,   287,     0,
   288,     0,   289,     0,   291,     0,   290,     0,   285,     0,
   286,     0,   284,     0,   566,     0,   566,   299,   566,     0,
   245,   299,   566,     0,   566,   299,   245,     0,    64,   175,
   560,   484,   255,   425,   424,   182,   566,     0,   566,     0,
     0,    82,     0,     0,     0,    85,   259,   566,    67,   427,
   150,   433,   182,   432,   504,   219,   434,   428,     0,   246,
     0,   470,     0,   431,     0,   297,   429,   298,     0,   300,
   429,   301,     0,   430,     0,   431,     0,   430,   431,     0,
   430,   431,   294,     0,   431,   294,     0,   456,     0,   464,
     0,   461,     0,   435,     0,   560,   296,   563,     0,   560,
     0,   172,     0,   189,     0,    97,     0,   122,     0,   230,
     0,     0,   247,   560,     0,   234,   560,     0,   235,   560,
     0,   235,   289,     0,   204,   439,     0,    69,   439,     0,
    83,   439,     0,   103,   439,     0,   169,   439,     0,   199,
     0,   184,     0,     0,    85,   195,   566,    67,   470,     0,
   236,   568,     0,    85,   217,   561,   198,   443,   444,     0,
    85,   217,   561,     0,   237,   284,   445,     0,     0,   221,
   284,   446,     0,     0,   573,     0,    96,     0,     0,   573,
     0,    96,     0,     0,   101,   217,   561,     0,   212,   565,
   150,   560,     0,   270,   450,   451,     0,   270,   450,   451,
   560,   452,     0,   272,     0,     0,   207,     0,     0,   300,
   453,   301,     0,     0,   566,     0,   453,   299,   566,     0,
   222,   450,   455,     0,   470,     0,   465,     0,   464,     0,
   456,     0,   435,     0,   461,     0,   122,   125,   560,   457,
     0,   192,   300,   557,   301,     0,    96,   192,     0,   470,
     0,   300,   459,   301,   192,   300,   557,   301,     0,   300,
   459,   301,   470,     0,   300,   459,   301,     0,     0,   459,
   299,   460,     0,   460,     0,   576,   534,     0,    97,   113,
   560,   504,     0,   238,   474,   560,     0,   238,   474,   560,
   119,   463,   258,   274,   274,     0,   238,   474,   560,   119,
   274,   274,   274,     0,   238,   474,   560,   119,   274,   274,
     0,   274,     0,     0,   189,   560,   173,   555,   491,   504,
     0,    95,   566,   466,    92,   111,   470,   467,     0,   210,
     0,   121,     0,   170,     0,   121,   170,     0,     0,   111,
   468,     0,     0,   164,   151,     0,   189,   469,     0,   149,
   459,     0,   471,   477,   489,   481,     0,   300,   471,   301,
     0,   472,     0,   471,   104,   471,     0,   471,   187,   475,
   471,     0,   471,   123,   475,   471,     0,   172,   476,   557,
   473,   491,   504,   487,   488,     0,   125,   333,   474,   560,
     0,   125,   648,     0,     0,   175,     0,     0,    63,     0,
     0,    99,     0,    99,   150,   576,     0,    63,     0,     0,
   154,    72,   478,     0,     0,   479,     0,   478,   299,   479,
     0,   532,   480,     0,   191,   276,     0,   191,   285,     0,
   191,   286,     0,    68,     0,    98,     0,     0,   233,   482,
   299,   483,     0,   233,   482,   249,   483,     0,   233,   482,
     0,   249,   483,   233,   482,     0,   249,   483,     0,     0,
   571,     0,    63,     0,   281,     0,   571,     0,   281,     0,
   289,     0,     0,   486,     0,   566,     0,   486,   299,   566,
     0,   116,    72,   535,     0,     0,   117,   532,     0,     0,
   111,   189,   490,     0,     0,   149,   453,     0,     0,   113,
   492,     0,     0,   300,   495,   301,     0,   496,     0,   493,
     0,   493,   299,   494,     0,   494,     0,   505,    67,   577,
     0,   505,   576,     0,   505,     0,   496,     0,   494,   187,
   128,   494,     0,   494,   497,     0,   497,   498,     0,   498,
     0,   499,   128,   494,   501,     0,   141,   499,   128,   494,
     0,    86,   128,   494,     0,   114,   500,     0,   132,   500,
     0,   168,   500,     0,   155,     0,   120,     0,     0,   155,
     0,     0,   191,   300,   502,   301,     0,   150,   532,     0,
   502,   299,   503,     0,   503,     0,   576,     0,   197,   532,
     0,     0,   560,     0,   560,   289,     0,   297,   298,   507,
     0,   297,   571,   298,   507,     0,     0,   297,   298,   507,
     0,   297,   571,   298,   507,     0,     0,   509,   506,     0,
   517,     0,   262,   509,     0,   510,     0,   522,     0,   512,
     0,   511,     0,   657,     0,   203,     0,     3,     0,     4,
     0,     5,     0,     6,     0,     7,     0,     8,     0,     9,
     0,    10,     0,    11,     0,    13,     0,    15,     0,    16,
     0,    17,     0,    18,     0,    19,     0,    20,     0,    21,
     0,    22,     0,    23,     0,    24,     0,    26,     0,    28,
     0,    29,     0,    30,     0,    31,     0,    32,     0,    34,
     0,    35,     0,    36,     0,    37,     0,    38,     0,   110,
   514,     0,   100,   158,     0,    94,   516,     0,   148,   515,
     0,   110,     0,   100,   158,     0,    94,     0,   148,     0,
   300,   571,   301,     0,     0,   300,   571,   299,   571,   301,
     0,   300,   571,   301,     0,     0,   300,   571,   299,   571,
   301,     0,   300,   571,   301,     0,     0,   518,   300,   571,
   301,     0,   518,     0,    77,   519,   520,   521,     0,    76,
   519,     0,   193,     0,   140,    77,   519,     0,   142,   519,
     0,   194,     0,     0,    77,   173,   576,     0,     0,    81,
   576,     0,     0,   523,     0,   179,   524,     0,   178,     0,
   124,   525,     0,   200,     0,   138,     0,    93,     0,   118,
     0,   137,     0,   171,     0,   198,   178,   201,     0,     0,
   523,     0,   200,   182,   138,     0,    93,   182,   118,     0,
    93,   182,   137,     0,    93,   182,   171,     0,   118,   182,
   137,     0,   137,   182,   171,     0,   118,   182,   171,     0,
     0,   532,     0,   147,     0,   300,   528,   301,   119,   300,
   472,   301,     0,   300,   528,   301,   145,   119,   300,   472,
   301,     0,   300,   528,   301,   529,   530,   300,   472,   301,
     0,   300,   528,   301,   529,   300,   472,   301,     0,   300,
   528,   301,   529,   300,   528,   301,     0,   531,   299,   532,
     0,   276,     0,   285,     0,   284,     0,   286,     0,   287,
     0,   288,     0,   289,     0,   291,     0,   290,     0,    66,
     0,    63,     0,   531,   299,   532,     0,   532,     0,   553,
   534,     0,   527,     0,   569,     0,   576,     0,   288,   532,
     0,   532,   287,   532,     0,   532,   288,   532,     0,   532,
   290,   532,     0,   532,   291,   532,     0,   532,   289,   532,
     0,   532,   285,   532,     0,   532,   286,   532,     0,   532,
   284,   147,     0,   532,   284,   532,     0,   294,   532,     0,
   292,   532,     0,   532,    59,   508,     0,    75,   300,   532,
    67,   508,   301,     0,   300,   526,   301,     0,   532,   276,
   532,     0,   532,   134,   532,     0,   532,   145,   134,   532,
     0,   276,   532,     0,   532,   276,     0,   567,   300,   289,
   301,     0,   567,   300,   301,     0,   567,   300,   535,   301,
     0,    88,     0,    89,     0,    89,   300,   571,   301,     0,
    90,     0,    90,   300,   571,   301,     0,    91,     0,   190,
     0,   106,   300,   472,   301,     0,   107,   300,   536,   301,
     0,   157,   300,   538,   301,     0,   174,   300,   540,   301,
     0,   185,   300,    71,   543,   301,     0,   185,   300,   131,
   543,   301,     0,   185,   300,   183,   543,   301,     0,   185,
   300,   543,   301,     0,   532,   231,     0,   532,   126,   147,
     0,   532,   248,     0,   532,   126,   145,   147,     0,   532,
   126,   186,     0,   532,   126,   145,   108,     0,   532,   126,
   108,     0,   532,   126,   145,   186,     0,   532,    70,   533,
    65,   533,     0,   532,   145,    70,   533,    65,   533,     0,
   532,   119,   300,   544,   301,     0,   532,   145,   119,   300,
   546,   301,     0,   532,   276,   300,   472,   301,     0,   532,
   287,   300,   472,   301,     0,   532,   288,   300,   472,   301,
     0,   532,   290,   300,   472,   301,     0,   532,   291,   300,
   472,   301,     0,   532,   289,   300,   472,   301,     0,   532,
   285,   300,   472,   301,     0,   532,   286,   300,   472,   301,
     0,   532,   284,   300,   472,   301,     0,   532,   276,    66,
   300,   472,   301,     0,   532,   287,    66,   300,   472,   301,
     0,   532,   288,    66,   300,   472,   301,     0,   532,   290,
    66,   300,   472,   301,     0,   532,   291,    66,   300,   472,
   301,     0,   532,   289,    66,   300,   472,   301,     0,   532,
   285,    66,   300,   472,   301,     0,   532,   286,    66,   300,
   472,   301,     0,   532,   284,    66,   300,   472,   301,     0,
   532,   276,    63,   300,   472,   301,     0,   532,   287,    63,
   300,   472,   301,     0,   532,   288,    63,   300,   472,   301,
     0,   532,   290,    63,   300,   472,   301,     0,   532,   291,
    63,   300,   472,   301,     0,   532,   289,    63,   300,   472,
   301,     0,   532,   285,    63,   300,   472,   301,     0,   532,
   286,    63,   300,   472,   301,     0,   532,   284,    63,   300,
   472,   301,     0,   532,    65,   532,     0,   532,   153,   532,
     0,   145,   532,     0,   548,     0,   653,     0,   553,   534,
     0,   569,     0,   576,     0,   288,   533,     0,   533,   287,
   533,     0,   533,   288,   533,     0,   533,   290,   533,     0,
   533,   291,   533,     0,   533,   289,   533,     0,   294,   533,
     0,   292,   533,     0,   533,    59,   508,     0,    75,   300,
   533,    67,   508,   301,     0,   300,   532,   301,     0,   533,
   276,   533,     0,   276,   533,     0,   533,   276,     0,   567,
   300,   301,     0,   567,   300,   535,   301,     0,    88,     0,
    89,     0,    89,   300,   571,   301,     0,    90,     0,    90,
   300,   571,   301,     0,    91,     0,   190,     0,   157,   300,
   538,   301,     0,   174,   300,   540,   301,     0,   185,   300,
    71,   543,   301,     0,   185,   300,   131,   543,   301,     0,
   185,   300,   183,   543,   301,     0,   185,   300,   543,   301,
     0,   654,     0,   297,   647,   298,   534,     0,   297,   647,
   293,   647,   298,   534,     0,     0,   526,     0,   535,   299,
   526,     0,   535,   191,   532,     0,   537,   113,   532,     0,
     0,   653,     0,   523,     0,   180,     0,   181,     0,   539,
   119,   539,     0,     0,   553,   534,     0,   569,     0,   288,
   539,     0,   539,   287,   539,     0,   539,   288,   539,     0,
   539,   290,   539,     0,   539,   291,   539,     0,   539,   289,
   539,     0,   292,   539,     0,   539,    59,   508,     0,    75,
   300,   539,    67,   508,   301,     0,   300,   539,   301,     0,
   539,   276,   539,     0,   276,   539,     0,   539,   276,     0,
   576,     0,   567,   300,   301,     0,   567,   300,   535,   301,
     0,   157,   300,   538,   301,     0,   174,   300,   540,   301,
     0,   185,   300,    71,   543,   301,     0,   185,   300,   131,
   543,   301,     0,   185,   300,   183,   543,   301,     0,   185,
   300,   543,   301,     0,   535,   541,   542,     0,     0,   113,
   535,     0,     0,   111,   535,     0,     0,   532,   113,   535,
     0,   113,   535,     0,   535,     0,   472,     0,   545,     0,
   569,     0,   545,   299,   569,     0,   472,     0,   547,     0,
   569,     0,   547,   299,   569,     0,    74,   552,   549,   551,
   103,     0,   146,   300,   532,   299,   532,   301,     0,    80,
   300,   535,   301,     0,   549,   550,     0,   550,     0,   196,
   532,   177,   526,     0,   102,   526,     0,     0,   553,   534,
     0,   576,     0,     0,   560,   296,   554,     0,   570,   296,
   554,     0,   563,     0,   554,   296,   563,     0,   554,   296,
   289,     0,   555,   299,   556,     0,   556,     0,   289,     0,
   576,   534,   284,   526,     0,   553,   534,     0,   560,   296,
   289,     0,   557,   299,   558,     0,   558,     0,   526,    67,
   577,     0,   526,     0,   560,   296,   289,     0,   289,     0,
   576,     0,     0,   578,     0,   576,     0,   576,     0,   657,
     0,   576,     0,   657,     0,   576,     0,   576,     0,   576,
     0,   573,     0,   571,     0,   572,     0,   573,     0,   508,
   573,     0,   570,     0,   186,     0,   108,     0,   281,   534,
     0,   280,     0,   282,     0,   275,     0,   657,     0,   576,
     0,   513,     0,   518,     0,   657,     0,   523,     0,    60,
     0,    61,     0,   205,     0,   206,     0,   208,     0,   209,
     0,   211,     0,   214,     0,   215,     0,   216,     0,   217,
     0,   218,     0,   100,     0,   220,     0,   221,     0,   224,
     0,   225,     0,   226,     0,   227,     0,   228,     0,   229,
     0,   121,     0,   230,     0,   231,     0,   129,     0,   130,
     0,   232,     0,   237,     0,   136,     0,   239,     0,   240,
     0,   143,     0,   243,     0,   244,     0,   246,     0,   248,
     0,   149,     0,   250,     0,   151,     0,   251,     0,   152,
     0,   252,     0,   160,     0,   161,     0,   253,     0,   164,
     0,   166,     0,   255,     0,   257,     0,   258,     0,   259,
     0,   170,     0,   261,     0,   260,     0,   264,     0,   265,
     0,   266,     0,   267,     0,   178,     0,   179,     0,   180,
     0,   181,     0,   202,     0,   268,     0,   203,     0,   271,
     0,   273,     0,   201,     0,     3,     0,     4,     0,     5,
     0,     6,     0,     7,     0,     8,     0,     9,     0,    10,
     0,    11,     0,    13,     0,    15,     0,    16,     0,    17,
     0,    18,     0,    19,     0,    20,     0,    21,     0,    22,
     0,    23,     0,    24,     0,    26,     0,    28,     0,    29,
     0,    30,     0,    31,     0,    32,     0,    34,     0,    35,
     0,    36,     0,    37,     0,    38,     0,   576,     0,   204,
     0,   207,     0,   210,     0,    74,     0,   212,     0,    80,
     0,    84,     0,   213,     0,    87,     0,   219,     0,   102,
     0,   103,     0,   222,     0,   223,     0,   108,     0,   112,
     0,   116,     0,   234,     0,   236,     0,   238,     0,   241,
     0,   242,     0,   245,     0,   146,     0,   154,     0,   157,
     0,   158,     0,   256,     0,   262,     0,   263,     0,   175,
     0,   177,     0,   184,     0,   186,     0,   270,     0,   272,
     0,   196,     0,    87,     0,   242,     0,     7,   182,   580,
   586,   587,     0,     7,   182,    96,     0,     7,   588,     0,
   561,   583,   585,     0,   581,   582,   585,   290,   561,   591,
     0,   590,     0,   573,     0,   657,   655,     0,   276,   584,
     0,   582,     0,     0,   576,     0,   576,   296,   584,     0,
   293,   571,     0,     0,    67,   580,     0,     0,   190,   588,
     0,     0,   589,     0,   589,   290,   576,     0,   589,    17,
    72,   589,     0,   589,   191,   589,     0,   574,     0,   590,
     0,   275,     0,   655,     0,   276,   576,     0,     0,    95,
   566,   466,    92,   111,   657,   467,     0,    10,    23,   657,
     0,     0,   596,   595,   598,   597,     0,   649,    69,    95,
    26,    27,     0,   649,   103,    95,    26,    27,     0,     0,
   599,   598,     0,     0,     0,   602,   600,   603,   601,   613,
   294,     0,    46,     0,    54,     0,    53,     0,    43,     0,
    51,     0,    40,     0,     0,   611,     0,   612,     0,   606,
     0,   607,     0,   604,     0,   658,     0,   605,   302,   660,
   303,     0,    45,   610,     0,   608,   302,   598,   303,     0,
   609,   302,   598,   303,     0,    55,   610,     0,    56,   610,
     0,     0,   658,     0,    52,     0,    57,    52,     0,    48,
     0,    57,    48,     0,    50,     0,    57,    50,     0,    47,
     0,    44,     0,    41,     0,    42,     0,    57,    42,     0,
    58,     0,   614,     0,   613,   299,   614,     0,   616,   658,
   506,   615,     0,     0,   284,   651,     0,     0,   289,     0,
    95,   265,   657,     0,    11,   619,     0,   620,     0,    87,
     0,    63,     0,     0,   580,     0,    96,     0,   105,    18,
   623,     0,     0,   105,   657,   622,   626,     0,   590,     0,
   277,     0,    14,   657,     0,    22,   566,   626,     0,     0,
   191,   627,     0,   653,     0,   653,   299,   627,     0,    23,
   657,   113,   590,     0,   438,    24,     0,   173,     8,   620,
     0,   203,   658,   126,   635,   632,   634,     0,   297,   298,
   633,     0,   300,   301,   633,     0,   297,   571,   298,   633,
     0,   300,   571,   301,   633,     0,     0,   297,   298,   633,
     0,   300,   301,   633,     0,   297,   571,   298,   633,     0,
   300,   571,   301,   633,     0,     0,    25,     0,     0,    76,
     0,   193,     0,   110,     0,   100,     0,   638,    20,     0,
    12,     0,   638,    28,     0,   638,    21,     0,     4,     0,
    36,    20,     0,    36,    28,     0,    36,    21,     0,     0,
    35,   636,   302,   639,   303,     0,     0,   187,   637,   302,
   639,   303,     0,   658,     0,    29,     0,     0,     0,   640,
   639,     0,     0,   635,   641,   642,    27,     0,   643,     0,
   642,   299,   643,     0,   616,   658,   506,     0,    37,   658,
   126,   635,   632,   634,     0,    38,    30,   646,     0,    38,
   145,    13,   646,     0,    38,    32,   646,     0,     9,     0,
    31,     0,    34,     0,    16,   566,     0,    15,   182,   566,
     0,   219,   566,   300,   650,   301,     0,   219,     5,     0,
     6,   566,   300,   650,   301,     0,   553,   534,     0,   527,
     0,   569,     0,   576,     0,   288,   647,     0,   532,   287,
   647,     0,   532,   288,   647,     0,   532,   290,   647,     0,
   532,   291,   647,     0,   532,   289,   647,     0,   532,   285,
   647,     0,   532,   286,   647,     0,   532,   284,   647,     0,
   294,   647,     0,   292,   647,     0,   532,    59,   508,     0,
    75,   300,   532,    67,   508,   301,     0,   300,   526,   301,
     0,   532,   276,   647,     0,   532,   134,   647,     0,   532,
   145,   134,   647,     0,   276,   647,     0,   532,   276,     0,
   567,   300,   289,   301,     0,   567,   300,   301,     0,   567,
   300,   535,   301,     0,    88,     0,    89,     0,    89,   300,
   571,   301,     0,    90,     0,    90,   300,   571,   301,     0,
    91,     0,   106,   300,   472,   301,     0,   107,   300,   536,
   301,     0,   157,   300,   538,   301,     0,   174,   300,   540,
   301,     0,   185,   300,    71,   543,   301,     0,   185,   300,
   131,   543,   301,     0,   185,   300,   183,   543,   301,     0,
   185,   300,   543,   301,     0,   532,   231,     0,   532,   126,
   147,     0,   532,   248,     0,   532,   126,   145,   147,     0,
   532,   126,   186,     0,   532,   126,   145,   108,     0,   532,
   126,   108,     0,   532,   126,   145,   186,     0,   532,    70,
   533,    65,   533,     0,   532,   145,    70,   533,    65,   533,
     0,   532,   119,   300,   544,   301,     0,   532,   145,   119,
   300,   546,   301,     0,   532,   276,   300,   472,   301,     0,
   532,   287,   300,   472,   301,     0,   532,   288,   300,   472,
   301,     0,   532,   290,   300,   472,   301,     0,   532,   291,
   300,   472,   301,     0,   532,   289,   300,   472,   301,     0,
   532,   285,   300,   472,   301,     0,   532,   286,   300,   472,
   301,     0,   532,   284,   300,   472,   301,     0,   532,   276,
    66,   300,   472,   301,     0,   532,   287,    66,   300,   472,
   301,     0,   532,   288,    66,   300,   472,   301,     0,   532,
   290,    66,   300,   472,   301,     0,   532,   291,    66,   300,
   472,   301,     0,   532,   289,    66,   300,   472,   301,     0,
   532,   285,    66,   300,   472,   301,     0,   532,   286,    66,
   300,   472,   301,     0,   532,   284,    66,   300,   472,   301,
     0,   532,   276,    63,   300,   472,   301,     0,   532,   287,
    63,   300,   472,   301,     0,   532,   288,    63,   300,   472,
   301,     0,   532,   290,    63,   300,   472,   301,     0,   532,
   291,    63,   300,   472,   301,     0,   532,   289,    63,   300,
   472,   301,     0,   532,   285,    63,   300,   472,   301,     0,
   532,   286,    63,   300,   472,   301,     0,   532,   284,    63,
   300,   472,   301,     0,   532,    65,   647,     0,   532,   153,
   647,     0,   145,   647,     0,   654,     0,   652,     0,   648,
   299,   652,     0,    33,     0,     0,   650,   663,     0,   664,
     0,   651,   664,     0,   655,   656,     0,   655,   656,     0,
   655,     0,   278,     0,     0,   655,     0,    19,   655,     0,
    19,   566,     0,   274,     0,   277,     0,   274,     0,   279,
     0,   662,     0,   660,   662,     0,   662,     0,   294,     0,
   274,     0,   277,     0,   571,     0,   572,     0,   289,     0,
    40,     0,    41,     0,    42,     0,    43,     0,    44,     0,
    45,     0,    46,     0,    47,     0,    48,     0,    50,     0,
    51,     0,    52,     0,    53,     0,    54,     0,    55,     0,
    56,     0,    57,     0,    58,     0,    39,     0,   297,     0,
   298,     0,   300,     0,   301,     0,   284,     0,   299,     0,
   274,     0,   277,     0,   571,     0,   572,     0,   299,     0,
   274,     0,   277,     0,   571,     0,   572,     0,   302,   660,
   303,     0,   302,     0,   303,     0
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
  1417,  1419,  1421,  1423,  1425,  1427,  1429,  1431,  1437,  1439,
  1441,  1443,  1447,  1449,  1451,  1453,  1459,  1461,  1464,  1466,
  1468,  1474,  1476,  1482,  1484,  1492,  1496,  1500,  1504,  1508,
  1512,  1519,  1523,  1529,  1531,  1533,  1537,  1539,  1541,  1543,
  1545,  1547,  1549,  1551,  1553,  1559,  1561,  1563,  1567,  1571,
  1573,  1577,  1581,  1583,  1585,  1587,  1589,  1591,  1593,  1595,
  1597,  1599,  1601,  1603,  1605,  1607,  1609,  1611,  1613,  1615,
  1617,  1619,  1622,  1626,  1631,  1636,  1637,  1638,  1641,  1642,
  1643,  1646,  1647,  1650,  1651,  1652,  1653,  1656,  1657,  1660,
  1666,  1667,  1670,  1671,  1674,  1684,  1690,  1692,  1695,  1699,
  1703,  1707,  1711,  1715,  1721,  1722,  1724,  1728,  1735,  1739,
  1753,  1760,  1761,  1763,  1777,  1785,  1786,  1789,  1793,  1797,
  1803,  1804,  1805,  1808,  1814,  1815,  1818,  1819,  1822,  1824,
  1826,  1830,  1834,  1838,  1839,  1842,  1855,  1861,  1867,  1868,
  1869,  1872,  1873,  1874,  1875,  1876,  1879,  1882,  1883,  1886,
  1889,  1893,  1899,  1900,  1901,  1902,  1903,  1916,  1920,  1937,
  1944,  1950,  1951,  1952,  1953,  1958,  1961,  1962,  1963,  1964,
  1965,  1966,  1969,  1970,  1972,  1983,  1989,  1993,  1997,  2003,
  2007,  2013,  2017,  2021,  2025,  2029,  2035,  2039,  2043,  2049,
  2053,  2064,  2082,  2091,  2092,  2095,  2096,  2099,  2100,  2103,
  2104,  2107,  2113,  2119,  2120,  2121,  2130,  2131,  2132,  2142,
  2178,  2184,  2185,  2188,  2189,  2192,  2193,  2197,  2203,  2204,
  2225,  2231,  2232,  2233,  2234,  2238,  2244,  2245,  2249,  2256,
  2262,  2262,  2264,  2265,  2266,  2267,  2268,  2269,  2270,  2271,
  2274,  2278,  2280,  2282,  2295,  2302,  2303,  2306,  2307,  2320,
  2322,  2329,  2330,  2331,  2332,  2333,  2336,  2337,  2340,  2342,
  2344,  2348,  2349,  2350,  2351,  2354,  2358,  2365,  2366,  2367,
  2368,  2371,  2372,  2384,  2390,  2396,  2400,  2418,  2419,  2420,
  2421,  2422,  2424,  2425,  2426,  2436,  2450,  2464,  2474,  2480,
  2481,  2484,  2485,  2488,  2489,  2490,  2493,  2494,  2495,  2505,
  2519,  2533,  2537,  2545,  2546,  2549,  2550,  2553,  2554,  2557,
  2559,  2571,  2589,  2590,  2591,  2592,  2593,  2594,  2611,  2617,
  2621,  2625,  2629,  2633,  2639,  2640,  2643,  2646,  2650,  2664,
  2671,  2675,  2706,  2726,  2743,  2744,  2757,  2773,  2804,  2805,
  2806,  2807,  2808,  2811,  2812,  2816,  2817,  2823,  2839,  2856,
  2860,  2864,  2869,  2874,  2882,  2892,  2893,  2894,  2897,  2898,
  2901,  2902,  2905,  2906,  2907,  2908,  2911,  2912,  2915,  2916,
  2919,  2925,  2926,  2927,  2928,  2929,  2930,  2933,  2935,  2937,
  2939,  2941,  2943,  2947,  2948,  2949,  2952,  2953,  2963,  2964,
  2967,  2969,  2971,  2975,  2976,  2979,  2983,  2986,  2990,  2995,
  2999,  3013,  3017,  3023,  3025,  3027,  3031,  3033,  3037,  3041,
  3045,  3055,  3057,  3061,  3067,  3071,  3084,  3088,  3092,  3097,
  3102,  3107,  3112,  3117,  3121,  3127,  3128,  3139,  3140,  3143,
  3144,  3147,  3153,  3154,  3157,  3162,  3168,  3174,  3180,  3188,
  3194,  3200,  3218,  3222,  3223,  3229,  3230,  3231,  3234,  3240,
  3241,  3242,  3243,  3244,  3245,  3246,  3247,  3248,  3249,  3250,
  3251,  3252,  3253,  3254,  3255,  3256,  3257,  3258,  3259,  3260,
  3261,  3262,  3263,  3264,  3265,  3266,  3267,  3268,  3269,  3270,
  3271,  3272,  3280,  3284,  3288,  3292,  3298,  3300,  3302,  3304,
  3308,  3316,  3322,  3334,  3342,  3348,  3360,  3368,  3381,  3401,
  3407,  3414,  3415,  3416,  3417,  3420,  3421,  3424,  3425,  3428,
  3429,  3432,  3436,  3440,  3444,  3450,  3451,  3452,  3453,  3454,
  3455,  3458,  3459,  3462,  3463,  3464,  3465,  3466,  3467,  3468,
  3469,  3470,  3480,  3482,  3497,  3501,  3505,  3509,  3513,  3519,
  3525,  3526,  3527,  3528,  3529,  3530,  3531,  3532,  3533,  3536,
  3537,  3541,  3545,  3560,  3564,  3566,  3568,  3572,  3574,  3576,
  3578,  3580,  3582,  3584,  3586,  3588,  3590,  3595,  3597,  3599,
  3603,  3607,  3609,  3611,  3613,  3615,  3617,  3619,  3623,  3627,
  3631,  3635,  3639,  3645,  3649,  3655,  3659,  3664,  3668,  3672,
  3676,  3681,  3685,  3689,  3693,  3697,  3699,  3701,  3703,  3710,
  3714,  3718,  3722,  3726,  3730,  3734,  3738,  3742,  3746,  3750,
  3754,  3758,  3762,  3766,  3770,  3774,  3778,  3782,  3786,  3790,
  3794,  3798,  3802,  3806,  3810,  3814,  3818,  3822,  3826,  3830,
  3834,  3838,  3842,  3846,  3850,  3852,  3854,  3856,  3858,  3867,
  3871,  3873,  3877,  3879,  3881,  3883,  3885,  3887,  3892,  3894,
  3896,  3900,  3904,  3906,  3908,  3910,  3912,  3916,  3920,  3924,
  3928,  3934,  3938,  3944,  3948,  3952,  3956,  3961,  3965,  3969,
  3973,  3977,  3981,  3985,  3989,  3993,  3995,  3997,  4001,  4005,
  4007,  4011,  4012,  4013,  4016,  4018,  4022,  4026,  4028,  4030,
  4032,  4034,  4036,  4038,  4040,  4042,  4046,  4050,  4052,  4054,
  4056,  4058,  4062,  4066,  4070,  4074,  4079,  4083,  4087,  4091,
  4097,  4101,  4105,  4107,  4113,  4115,  4119,  4121,  4123,  4127,
  4131,  4135,  4137,  4141,  4145,  4149,  4151,  4170,  4172,  4178,
  4184,  4186,  4190,  4196,  4197,  4200,  4204,  4208,  4212,  4216,
  4222,  4224,  4226,  4237,  4239,  4241,  4244,  4248,  4252,  4263,
  4265,  4270,  4274,  4278,  4282,  4288,  4289,  4292,  4296,  4309,
  4310,  4311,  4312,  4313,  4319,  4320,  4322,  4328,  4332,  4336,
  4340,  4344,  4346,  4350,  4356,  4362,  4363,  4364,  4372,  4379,
  4381,  4383,  4394,  4395,  4396,  4397,  4398,  4399,  4400,  4401,
  4402,  4403,  4404,  4405,  4406,  4407,  4408,  4409,  4410,  4411,
  4412,  4413,  4414,  4415,  4416,  4417,  4418,  4419,  4420,  4421,
  4422,  4423,  4424,  4425,  4426,  4427,  4428,  4429,  4430,  4431,
  4432,  4433,  4434,  4435,  4436,  4437,  4438,  4439,  4440,  4441,
  4443,  4444,  4445,  4446,  4447,  4448,  4449,  4450,  4451,  4452,
  4453,  4454,  4455,  4456,  4457,  4458,  4459,  4460,  4461,  4462,
  4463,  4464,  4465,  4466,  4467,  4468,  4469,  4470,  4471,  4472,
  4473,  4474,  4475,  4476,  4477,  4478,  4479,  4480,  4481,  4482,
  4483,  4484,  4485,  4486,  4487,  4488,  4489,  4490,  4491,  4492,
  4493,  4494,  4495,  4507,  4508,  4509,  4510,  4511,  4512,  4513,
  4514,  4515,  4516,  4517,  4518,  4519,  4520,  4521,  4522,  4523,
  4524,  4525,  4526,  4527,  4528,  4529,  4530,  4531,  4532,  4533,
  4534,  4535,  4536,  4537,  4538,  4539,  4540,  4541,  4542,  4543,
  4544,  4547,  4554,  4570,  4574,  4579,  4584,  4595,  4618,  4622,
  4630,  4647,  4658,  4659,  4661,  4662,  4664,  4665,  4667,  4668,
  4670,  4671,  4673,  4677,  4681,  4685,  4690,  4695,  4696,  4698,
  4722,  4735,  4741,  4784,  4789,  4794,  4801,  4803,  4805,  4809,
  4814,  4819,  4824,  4829,  4830,  4831,  4832,  4833,  4834,  4835,
  4837,  4844,  4851,  4858,  4865,  4873,  4885,  4890,  4892,  4899,
  4906,  4914,  4922,  4923,  4925,  4926,  4927,  4928,  4929,  4930,
  4931,  4932,  4933,  4934,  4935,  4937,  4939,  4943,  4948,  5023,
  5024,  5026,  5027,  5033,  5041,  5043,  5044,  5045,  5046,  5048,
  5049,  5054,  5067,  5079,  5083,  5083,  5090,  5095,  5099,  5100,
  5105,  5105,  5111,  5121,  5137,  5145,  5187,  5193,  5199,  5205,
  5211,  5219,  5225,  5231,  5237,  5243,  5250,  5251,  5253,  5260,
  5267,  5274,  5281,  5288,  5295,  5302,  5309,  5316,  5323,  5330,
  5337,  5342,  5350,  5355,  5363,  5374,  5374,  5376,  5380,  5386,
  5392,  5397,  5401,  5406,  5477,  5532,  5537,  5542,  5548,  5553,
  5558,  5563,  5568,  5573,  5578,  5583,  5590,  5594,  5596,  5598,
  5602,  5604,  5606,  5608,  5610,  5612,  5614,  5616,  5618,  5622,
  5624,  5626,  5630,  5634,  5636,  5638,  5640,  5642,  5644,  5646,
  5650,  5654,  5658,  5662,  5666,  5672,  5676,  5682,  5686,  5690,
  5694,  5698,  5703,  5707,  5711,  5715,  5719,  5721,  5723,  5725,
  5732,  5736,  5740,  5744,  5748,  5752,  5756,  5760,  5764,  5768,
  5772,  5776,  5780,  5784,  5788,  5792,  5796,  5800,  5804,  5808,
  5812,  5816,  5820,  5824,  5828,  5832,  5836,  5840,  5844,  5848,
  5852,  5856,  5860,  5864,  5868,  5872,  5874,  5876,  5878,  5882,
  5882,  5884,  5886,  5887,  5889,  5890,  5892,  5896,  5900,  5905,
  5907,  5908,  5909,  5910,  5912,  5913,  5918,  5920,  5922,  5923,
  5928,  5928,  5930,  5931,  5932,  5933,  5934,  5935,  5936,  5937,
  5938,  5939,  5940,  5941,  5942,  5943,  5944,  5945,  5946,  5947,
  5948,  5949,  5950,  5951,  5952,  5953,  5954,  5955,  5956,  5957,
  5958,  5959,  5961,  5962,  5963,  5964,  5965,  5967,  5968,  5969,
  5970,  5971,  5973,  5978
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
"PARAM","FCONST","OP","'='","'<'","'>'","'+'","'-'","'*'","'/'","'%'","'|'",
"':'","';'","UMINUS","'.'","'['","']'","','","'('","')'","'{'","'}'","prog",
"statements","statement","opt_at","stmt","CreateUserStmt","AlterUserStmt","DropUserStmt",
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
   304,   305,   305,   306,   306,   306,   306,   306,   306,   306,
   307,   308,   308,   308,   308,   308,   308,   308,   308,   308,
   308,   308,   308,   308,   308,   308,   308,   308,   308,   308,
   308,   308,   308,   308,   308,   308,   308,   308,   308,   308,
   308,   308,   308,   308,   308,   308,   308,   308,   308,   308,
   308,   308,   308,   308,   308,   308,   308,   308,   308,   308,
   308,   308,   308,   308,   308,   308,   308,   309,   310,   311,
   312,   312,   313,   313,   313,   314,   314,   314,   315,   315,
   316,   316,   317,   317,   318,   318,   318,   318,   318,   318,
   319,   319,   320,   320,   320,   321,   321,   321,   322,   322,
   322,   323,   324,   324,   324,   324,   324,   324,   325,   326,
   327,   327,   328,   328,   328,   329,   329,   330,   330,   331,
   331,   332,   333,   333,   334,   334,   334,   335,   335,   336,
   336,   337,   337,   338,   338,   339,   339,   340,   340,   341,
   341,   341,   341,   341,   341,   341,   342,   342,   343,   343,
   343,   343,   343,   343,   343,   343,   343,   343,   343,   343,
   343,   343,   343,   343,   343,   343,   343,   343,   343,   343,
   343,   343,   343,   343,   343,   344,   344,   345,   345,   345,
   345,   346,   346,   347,   347,   347,   347,   347,   347,   347,
   347,   347,   347,   347,   347,   347,   347,   347,   347,   347,
   347,   347,   347,   347,   347,   347,   347,   347,   347,   347,
   347,   347,   347,   347,   347,   347,   347,   347,   347,   347,
   347,   347,   348,   348,   349,   350,   350,   350,   351,   351,
   351,   352,   352,   353,   353,   353,   353,   354,   354,   355,
   356,   356,   357,   357,   358,   359,   360,   360,   361,   361,
   361,   361,   361,   361,   362,   362,   363,   363,   364,   364,
   365,   366,   366,   367,   368,   369,   369,   370,   370,   370,
   371,   371,   371,   372,   373,   373,   374,   374,   375,   375,
   375,   376,   376,   376,   376,   377,   378,   379,   380,   380,
   380,   381,   381,   381,   381,   381,   382,   383,   383,   384,
   384,   384,   385,   385,   385,   385,   385,   386,   386,   387,
   387,   388,   388,   388,   388,   388,   389,   389,   389,   389,
   389,   389,   390,   390,   390,   391,   392,   392,   392,   393,
   393,   394,   394,   394,   394,   394,   395,   395,   395,   396,
   396,   397,   398,   399,   399,   400,   400,   401,   401,   402,
   402,   403,   404,   405,   405,   405,   406,   406,   406,   407,
   408,   409,   409,   410,   410,   411,   411,   412,   413,   413,
   414,   415,   415,   415,   415,   416,   417,   417,   418,   419,
   420,   420,   421,   421,   421,   421,   421,   421,   421,   421,
   422,   422,   422,   422,   423,   424,   424,   425,   425,   427,
   426,   428,   428,   428,   428,   428,   429,   429,   430,   430,
   430,   431,   431,   431,   431,   432,   432,   433,   433,   433,
   433,   434,   434,   435,   436,   437,   437,   438,   438,   438,
   438,   438,   439,   439,   439,   440,   441,   442,   442,   443,
   443,   444,   444,   445,   445,   445,   446,   446,   446,   447,
   448,   449,   449,   450,   450,   451,   451,   452,   452,   453,
   453,   454,   455,   455,   455,   455,   455,   455,   456,   457,
   457,   457,   457,   457,   458,   458,   459,   459,   460,   461,
   462,   462,   462,   462,   463,   463,   464,   465,   466,   466,
   466,   466,   466,   467,   467,   468,   468,   469,   470,   471,
   471,   471,   471,   471,   472,   473,   473,   473,   474,   474,
   475,   475,   476,   476,   476,   476,   477,   477,   478,   478,
   479,   480,   480,   480,   480,   480,   480,   481,   481,   481,
   481,   481,   481,   482,   482,   482,   483,   483,   484,   484,
   485,   486,   486,   487,   487,   488,   488,   489,   489,   490,
   490,   491,   491,   492,   492,   492,   493,   493,   494,   494,
   494,   495,   495,   496,   497,   497,   498,   498,   498,   499,
   499,   499,   499,   499,   499,   500,   500,   501,   501,   502,
   502,   503,   504,   504,   505,   505,   506,   506,   506,   507,
   507,   507,   508,   508,   508,   509,   509,   509,   510,   511,
   511,   511,   511,   511,   511,   511,   511,   511,   511,   511,
   511,   511,   511,   511,   511,   511,   511,   511,   511,   511,
   511,   511,   511,   511,   511,   511,   511,   511,   511,   511,
   511,   511,   512,   512,   512,   512,   513,   513,   513,   513,
   514,   514,   515,   515,   515,   516,   516,   516,   517,   517,
   518,   518,   518,   518,   518,   519,   519,   520,   520,   521,
   521,   522,   522,   522,   522,   523,   523,   523,   523,   523,
   523,   524,   524,   525,   525,   525,   525,   525,   525,   525,
   525,   525,   526,   526,   527,   527,   527,   527,   527,   528,
   529,   529,   529,   529,   529,   529,   529,   529,   529,   530,
   530,   531,   531,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   532,
   532,   532,   532,   532,   532,   532,   532,   532,   532,   533,
   533,   533,   533,   533,   533,   533,   533,   533,   533,   533,
   533,   533,   533,   533,   533,   533,   533,   533,   533,   533,
   533,   533,   533,   533,   533,   533,   533,   533,   533,   533,
   533,   533,   534,   534,   534,   535,   535,   535,   536,   536,
   536,   537,   537,   537,   538,   538,   539,   539,   539,   539,
   539,   539,   539,   539,   539,   539,   539,   539,   539,   539,
   539,   539,   539,   539,   539,   539,   539,   539,   539,   539,
   540,   540,   541,   541,   542,   542,   543,   543,   543,   544,
   544,   545,   545,   546,   546,   547,   547,   548,   548,   548,
   549,   549,   550,   551,   551,   552,   552,   552,   553,   553,
   554,   554,   554,   555,   555,   555,   556,   556,   556,   557,
   557,   558,   558,   558,   558,   559,   559,   560,   560,   561,
   562,   563,   564,   565,   566,   567,   568,   569,   569,   569,
   569,   569,   569,   569,   570,   571,   572,   573,   574,   575,
   575,   575,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   576,   576,   576,   576,   576,   576,   576,
   576,   576,   576,   577,   577,   577,   577,   577,   577,   577,
   577,   577,   577,   577,   577,   577,   577,   577,   577,   577,
   577,   577,   577,   577,   577,   577,   577,   577,   577,   577,
   577,   577,   577,   577,   577,   577,   577,   577,   577,   577,
   577,   578,   578,   579,   579,   579,   580,   580,   580,   580,
   581,   582,   583,   583,   584,   584,   585,   585,   586,   586,
   587,   587,   588,   588,   588,   588,   589,   589,   589,   590,
   591,   591,   592,   593,   595,   594,   596,   597,   598,   598,
   600,   601,   599,   602,   602,   602,   602,   602,   602,   602,
   603,   603,   603,   603,   603,   603,   604,   605,   606,   607,
   608,   609,   610,   610,   611,   611,   611,   611,   611,   611,
   611,   611,   611,   611,   611,   612,   613,   613,   614,   615,
   615,   616,   616,   617,   618,   619,   619,   619,   619,   620,
   620,   621,   622,   621,   623,   623,   624,   625,   626,   626,
   627,   627,   628,   629,   630,   631,   632,   632,   632,   632,
   632,   633,   633,   633,   633,   633,   634,   634,   635,   635,
   635,   635,   635,   635,   635,   635,   635,   635,   635,   635,
   636,   635,   637,   635,   635,   638,   638,   639,   639,   641,
   640,   642,   642,   643,   644,   645,   645,   645,   646,   646,
   646,   646,   646,   646,   646,   646,   647,   647,   647,   647,
   647,   647,   647,   647,   647,   647,   647,   647,   647,   647,
   647,   647,   647,   647,   647,   647,   647,   647,   647,   647,
   647,   647,   647,   647,   647,   647,   647,   647,   647,   647,
   647,   647,   647,   647,   647,   647,   647,   647,   647,   647,
   647,   647,   647,   647,   647,   647,   647,   647,   647,   647,
   647,   647,   647,   647,   647,   647,   647,   647,   647,   647,
   647,   647,   647,   647,   647,   647,   647,   647,   647,   647,
   647,   647,   647,   647,   647,   647,   647,   647,   647,   648,
   648,   649,   650,   650,   651,   651,   652,   653,   654,   655,
   656,   656,   656,   656,   657,   657,   658,   659,   660,   660,
   661,   661,   662,   662,   662,   662,   662,   662,   662,   662,
   662,   662,   662,   662,   662,   662,   662,   662,   662,   662,
   662,   662,   662,   662,   662,   662,   662,   662,   662,   662,
   662,   662,   663,   663,   663,   663,   663,   664,   664,   664,
   664,   664,   665,   666
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
     3,     3,     3,     3,     3,     3,     3,     3,     2,     2,
     3,     6,     3,     3,     4,     3,     2,     2,     1,     1,
     4,     1,     4,     1,     1,     3,     1,     4,     4,     5,
    10,     3,     1,     1,     1,     1,     2,     3,     3,     3,
     3,     3,     3,     3,     3,     2,     2,     3,     6,     3,
     3,     4,     3,     3,     4,     3,     3,     2,     2,     2,
     2,     3,     2,     4,     3,     3,     4,     4,     5,     6,
     5,     6,     3,     1,     1,     2,     2,     0,     2,     1,
     0,     3,     3,     2,     1,     2,     2,     4,     0,     7,
     3,     0,     3,     1,     1,     4,     2,     0,     2,     1,
     2,     2,     2,     2,     1,     1,     1,     2,     1,     2,
     9,     1,     0,     4,    14,     1,     1,     1,     3,     5,
     1,     1,     1,     3,     1,     0,     1,     1,     1,     3,
     0,     1,     1,     1,     1,     5,     3,     2,     1,     1,
     1,     1,     1,     1,     1,     1,     3,     1,     3,     3,
     1,     3,     1,     1,     1,     1,     2,     3,     3,     6,
     4,     1,     1,     1,     1,     0,     1,     2,     1,     1,
     1,     0,     2,     2,     0,     7,     2,     1,     1,     1,
     3,     1,     1,     1,     1,     1,     1,     2,     1,     3,
     0,     6,    11,     1,     0,     2,     0,     1,     1,     3,
     1,     6,     3,     2,     2,     0,     1,     2,     0,     4,
    11,     2,     0,     3,     2,     1,     3,     2,     1,     0,
     3,     1,     1,     1,     1,     4,     1,     1,     4,     6,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     3,     3,     3,     9,     1,     0,     1,     0,     0,
    13,     1,     1,     1,     3,     3,     1,     1,     2,     3,
     2,     1,     1,     1,     1,     3,     1,     1,     1,     1,
     1,     1,     0,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     1,     1,     0,     5,     2,     6,     3,     3,
     0,     3,     0,     1,     1,     0,     1,     1,     0,     3,
     4,     3,     5,     1,     0,     1,     0,     3,     0,     1,
     3,     3,     1,     1,     1,     1,     1,     1,     4,     4,
     2,     1,     7,     4,     3,     0,     3,     1,     2,     4,
     3,     8,     7,     6,     1,     0,     6,     7,     1,     1,
     1,     2,     0,     2,     0,     2,     2,     2,     4,     3,
     1,     3,     4,     4,     8,     4,     2,     0,     1,     0,
     1,     0,     1,     3,     1,     0,     3,     0,     1,     3,
     2,     2,     2,     2,     1,     1,     0,     4,     4,     2,
     4,     2,     0,     1,     1,     1,     1,     1,     1,     0,
     1,     1,     3,     3,     0,     2,     0,     3,     0,     2,
     0,     2,     0,     3,     1,     1,     3,     1,     3,     2,
     1,     1,     4,     2,     2,     1,     4,     4,     3,     2,
     2,     2,     1,     1,     0,     1,     0,     4,     2,     3,
     1,     1,     2,     0,     1,     2,     3,     4,     0,     3,
     4,     0,     2,     1,     2,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     2,     2,     2,     2,     1,     2,     1,     1,
     3,     0,     5,     3,     0,     5,     3,     0,     4,     1,
     4,     2,     1,     3,     2,     1,     0,     3,     0,     2,
     0,     1,     2,     1,     2,     1,     1,     1,     1,     1,
     1,     3,     0,     1,     3,     3,     3,     3,     3,     3,
     3,     0,     1,     1,     7,     8,     8,     7,     7,     3,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     3,     1,     2,     1,     1,     1,     2,     3,     3,
     3,     3,     3,     3,     3,     3,     3,     2,     2,     3,
     6,     3,     3,     3,     4,     2,     2,     4,     3,     4,
     1,     1,     4,     1,     4,     1,     1,     4,     4,     4,
     4,     5,     5,     5,     4,     2,     3,     2,     4,     3,
     4,     3,     4,     5,     6,     5,     6,     5,     5,     5,
     5,     5,     5,     5,     5,     5,     6,     6,     6,     6,
     6,     6,     6,     6,     6,     6,     6,     6,     6,     6,
     6,     6,     6,     6,     3,     3,     2,     1,     1,     2,
     1,     1,     2,     3,     3,     3,     3,     3,     2,     2,
     3,     6,     3,     3,     2,     2,     3,     4,     1,     1,
     4,     1,     4,     1,     1,     4,     4,     5,     5,     5,
     4,     1,     4,     6,     0,     1,     3,     3,     3,     0,
     1,     1,     1,     1,     3,     0,     2,     1,     2,     3,
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
     2,     3,     3,     3,     3,     3,     3,     3,     3,     2,
     2,     3,     6,     3,     3,     3,     4,     2,     2,     4,
     3,     4,     1,     1,     4,     1,     4,     1,     4,     4,
     4,     4,     5,     5,     5,     4,     2,     3,     2,     4,
     3,     4,     3,     4,     5,     6,     5,     6,     5,     5,
     5,     5,     5,     5,     5,     5,     5,     6,     6,     6,
     6,     6,     6,     6,     6,     6,     6,     6,     6,     6,
     6,     6,     6,     6,     6,     3,     3,     2,     1,     1,
     3,     1,     0,     2,     1,     2,     2,     2,     1,     1,
     0,     1,     2,     2,     1,     1,     1,     1,     1,     2,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     3,     1,     1
};

static const short yydefact[] = {     2,
     1,  1312,  1356,  1338,  1339,  1340,  1341,  1342,  1343,  1344,
  1345,  1346,  1347,  1348,  1349,  1350,  1351,  1352,  1353,  1354,
  1355,  1333,  1334,  1328,   926,   927,  1361,  1337,  1332,  1357,
  1358,  1362,  1359,  1360,  1373,  1374,     3,  1335,  1336,     6,
  1105,     0,     8,     7,  1331,     9,    10,  1120,     0,     0,
     0,  1159,     0,     0,     0,     0,     0,     0,   435,   907,
   435,   124,     0,     0,     0,   435,     0,   316,     0,     0,
     0,   435,   516,     0,     0,     0,   435,     0,   117,   455,
     0,     0,     0,     0,   510,   316,     0,     0,     0,   455,
     0,     0,     0,    21,    13,    27,    51,    52,    53,    12,
    14,    15,    16,    17,    18,    19,    25,    20,    26,    23,
    24,    30,    31,    42,    32,    28,    36,    40,    37,    39,
    38,    41,    44,   467,    33,    34,    45,    46,    47,    48,
    49,    22,    50,    29,    43,   466,   468,    35,   465,   464,
   463,   518,   501,    54,    55,    56,    57,    58,    59,    60,
    61,    62,    63,    64,    65,    66,    67,  1119,  1117,  1114,
  1118,  1116,  1115,     0,  1120,  1111,  1003,  1004,  1005,  1006,
  1007,  1008,  1009,  1010,  1011,  1012,  1013,  1014,  1015,  1016,
  1017,  1018,  1019,  1020,  1021,  1022,  1023,  1024,  1025,  1026,
  1027,  1028,  1029,  1030,  1031,  1032,  1033,   935,   936,   668,
   947,   669,   956,   959,   960,   963,   670,   667,   966,   971,
   973,   975,   977,   978,   980,   981,   986,   671,   993,   994,
   995,   996,   666,  1002,   997,   999,   937,   938,   939,   940,
   941,   942,   943,   944,   945,   946,   948,   949,   950,   951,
   952,   953,   954,   955,   957,   958,   961,   962,   964,   965,
   967,   968,   969,   970,   972,   974,   976,   979,   982,   983,
   984,   985,   988,   987,   989,   990,   991,   992,   998,  1000,
  1001,  1325,   928,  1326,  1320,   934,  1084,  1080,   910,    11,
     0,  1079,  1100,   933,     0,  1099,  1097,  1076,  1093,  1098,
   929,     0,  1158,  1157,  1161,  1160,  1155,  1156,  1167,  1169,
   915,   933,     0,  1327,     0,     0,     0,     0,     0,     0,
     0,   434,   433,   429,   109,   906,   430,   123,   344,     0,
     0,     0,   290,   291,     0,     0,   289,     0,     0,   262,
     0,     0,     0,     0,   990,   493,     0,     0,     0,   375,
     0,   372,     0,     0,     0,   373,     0,     0,   374,     0,
     0,   431,     0,  1163,   315,   314,   313,   312,   322,   328,
   335,   333,   332,   334,   336,     0,   329,   330,     0,     0,
   432,   515,   513,     0,  1008,   449,   993,     0,     0,  1072,
  1073,     0,   909,   908,     0,   428,     0,   914,   116,     0,
   454,     0,     0,   425,   427,   426,   437,   917,   509,     0,
   322,   424,   993,     0,    99,   993,     0,    96,   457,     0,
   435,     0,     5,  1174,     0,   512,     0,   512,   549,  1106,
     0,  1110,     0,     0,  1083,  1088,  1088,  1081,  1075,  1090,
     0,     0,     0,  1104,     0,  1168,     0,  1207,     0,  1219,
     0,     0,  1220,  1221,     0,  1216,  1218,     0,   540,    72,
     0,    72,     0,     0,   439,     0,   916,     0,   248,     0,
     0,   293,   292,   296,   390,   388,   389,   383,   384,   385,
   387,   386,   287,     0,   295,   294,     0,  1154,   490,   491,
   489,     0,   584,   308,   541,   542,    70,     0,     0,   450,
     0,   381,     0,   382,     0,   309,   371,  1166,  1165,  1162,
  1169,   319,   320,   321,     0,   325,   317,   327,     0,     0,
     0,     0,     0,  1003,  1004,  1005,  1006,  1007,  1008,  1009,
  1010,  1011,  1012,  1013,  1014,  1015,  1016,  1017,  1018,  1019,
  1020,  1021,  1022,  1023,  1024,  1025,  1026,  1027,  1028,  1029,
  1030,  1031,  1032,  1033,   888,     0,   657,   657,     0,   731,
   732,   734,   736,   648,   947,     0,     0,   924,   642,   682,
     0,   657,     0,     0,   684,   645,     0,     0,   993,   994,
     0,   923,   737,   653,   999,     0,     0,   825,     0,   905,
     0,     0,     0,     0,   589,   596,   599,   598,   594,   650,
   597,   934,   903,   705,   683,   788,   825,   508,   901,     0,
     0,   706,   922,   918,   919,   920,   707,   789,  1321,   933,
  1175,   448,    90,   447,     0,     0,     0,     0,     0,  1207,
     0,   119,     0,   462,   584,   481,   325,   100,     0,    97,
     0,   456,   452,   500,     4,   502,   511,     0,     0,     0,
     0,   533,     0,  1143,  1144,  1142,  1133,  1141,  1137,  1139,
  1135,  1133,  1133,     0,  1146,  1112,  1125,     0,  1123,  1124,
     0,     0,  1121,  1122,  1126,  1085,  1082,     0,  1077,     0,
     0,  1092,     0,  1096,  1094,  1170,  1171,  1173,  1197,  1194,
  1206,  1201,     0,  1189,  1192,  1191,  1203,  1190,  1181,     0,
  1205,     0,     0,  1222,  1005,     0,  1217,   539,     0,     0,
    75,  1107,    75,     0,   267,   266,     0,   441,     0,     0,
   400,   246,   242,     0,     0,   288,     0,   492,     0,     0,
   480,     0,     0,   378,   376,   377,   379,     0,   264,  1164,
   318,     0,     0,     0,     0,   331,     0,     0,     0,   469,
   472,     0,   514,     0,   825,     0,     0,   887,     0,   656,
   652,   659,     0,     0,     0,     0,   635,   634,     0,   830,
     0,   633,   668,   669,   670,   666,   674,   665,   657,   655,
   787,     0,     0,   636,   836,   862,     0,   663,     0,   602,
   603,   604,   605,   606,   607,   608,   609,   610,   611,   612,
   613,   614,   615,   616,   617,   618,   619,   620,   621,   622,
   623,   624,   625,   626,   627,   628,   629,   630,   631,   632,
     0,   664,   673,   601,   595,   662,   600,   726,     0,   925,
   708,   719,   718,     0,     0,     0,   683,   921,     0,   593,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   746,   748,   727,     0,     0,     0,     0,     0,     0,     0,
     0,   704,   124,     0,   553,     0,     0,     0,     0,  1322,
  1318,    94,    95,    87,    93,     0,    92,    85,    91,    86,
   896,   825,   553,   895,     0,   825,  1181,   451,     0,     0,
   493,   360,   486,   311,   101,    98,   459,   504,   517,   519,
   527,   503,   551,     0,     0,   499,     0,  1128,  1134,  1131,
  1132,  1145,  1138,  1140,  1136,  1152,     0,  1120,  1120,     0,
  1087,     0,  1089,     0,  1074,  1095,     0,     0,  1198,  1200,
  1199,     0,     0,     0,  1188,  1193,  1196,  1195,  1313,  1223,
  1313,   399,   399,   399,   399,   102,     0,    73,    74,    78,
    78,   436,   272,   271,   273,     0,   268,     0,   443,   639,
   947,   637,   640,   365,     0,   931,   932,   366,   930,   370,
     0,     0,   250,     0,     0,     0,     0,   247,   127,     0,
     0,     0,   301,     0,   298,     0,     0,   583,   543,   286,
     0,     0,   391,   324,   323,     0,     0,   471,     0,     0,
   478,   825,     0,     0,   885,   882,   886,     0,     0,     0,
   661,   826,     0,     0,     0,     0,     0,   833,   834,   832,
     0,     0,   831,     0,     0,     0,     0,     0,   654,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   825,     0,   838,   852,   864,     0,     0,     0,     0,
     0,     0,   683,   869,     0,     0,   731,   732,   734,   736,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   705,     0,   825,     0,   706,   707,     0,  1309,  1321,
   722,     0,     0,   592,     0,     0,  1038,  1040,  1041,  1043,
  1045,  1046,  1049,  1050,  1051,  1058,  1059,  1060,  1061,  1065,
  1066,  1067,  1068,  1071,  1035,  1036,  1037,  1039,  1042,  1044,
  1047,  1048,  1052,  1053,  1054,  1055,  1056,  1057,  1062,  1063,
  1064,  1069,  1070,  1034,   902,   720,   785,     0,   809,   810,
   812,   814,     0,     0,     0,   815,     0,     0,     0,     0,
     0,     0,   825,     0,   791,   792,   822,  1319,     0,   752,
     0,   747,   750,   724,     0,     0,     0,   786,     0,     0,
     0,   723,     0,     0,   716,     0,   717,     0,     0,     0,
   714,     0,     0,     0,   715,     0,     0,     0,   709,     0,
     0,     0,   710,     0,     0,     0,   713,     0,     0,     0,
   711,     0,     0,     0,   712,   510,   507,  1310,  1321,   900,
     0,   584,   904,   889,   891,   912,     0,   729,     0,   890,
  1324,  1323,   980,    89,   898,     0,   584,     0,     0,  1188,
   118,   112,   111,     0,     0,   485,     0,     0,   453,     0,
   525,   526,     0,   521,     0,   548,   535,   536,   530,   534,
   538,   532,   537,     0,  1153,     0,  1147,     0,     0,  1329,
     0,     0,  1086,  1102,  1091,  1172,  1207,  1207,  1186,     0,
  1186,     0,  1187,  1215,     0,     0,     0,   398,     0,     0,
     0,   127,   108,     0,     0,     0,   397,    71,    76,    77,
    82,    82,     0,     0,   446,     0,   438,   638,     0,   364,
   369,   363,     0,     0,     0,   249,   259,   251,   252,   253,
   254,     0,     0,   126,   128,   129,   177,     0,   244,   245,
     0,     0,     0,     0,     0,   297,   347,   495,   495,     0,
   380,     0,   310,     0,   337,   341,   339,     0,     0,     0,
   479,   342,     0,     0,   881,     0,     0,     0,     0,   651,
     0,     0,   880,   733,   735,     0,   647,   738,   739,     0,
   641,   676,   677,   678,   679,   681,   680,   675,     0,     0,
   644,     0,   836,   862,     0,   850,   839,   845,     0,   740,
     0,     0,   851,     0,     0,     0,     0,     0,   837,     0,
     0,   866,   741,   672,     0,   868,     0,     0,     0,   745,
     0,     0,     0,     0,   830,   787,  1308,   836,   862,     0,
   726,  1248,   708,  1231,   719,  1241,   718,  1240,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   746,   748,   727,
     0,     0,     0,     0,     0,     0,     0,     0,   704,     0,
     0,   825,     0,     0,   691,   693,   692,   694,   695,   696,
   697,   699,   698,     0,   690,     0,   587,   592,   649,     0,
     0,     0,   836,   862,     0,   805,   793,   800,   799,     0,
     0,     0,   806,     0,     0,     0,     0,     0,   790,     0,
   870,     0,   871,   872,   922,   751,   749,   753,     0,     0,
   725,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1317,     0,   552,   556,   558,   555,   561,   585,   545,     0,
   728,   730,    88,   894,   487,   899,     0,  1176,   114,   115,
   121,   113,     0,   484,     0,     0,   460,   520,   522,   523,
   524,   550,     0,     0,     0,  1108,  1113,  1152,   589,  1127,
  1330,  1129,  1130,     0,  1078,  1210,     0,  1207,     0,     0,
     0,  1177,  1186,  1178,  1186,  1363,  1364,  1367,  1226,  1365,
  1366,  1314,  1224,     0,     0,     0,     0,     0,     0,   103,
     0,   105,     0,   396,     0,    84,    84,     0,   269,   445,
   440,   444,   449,   367,     0,     0,   368,   420,   421,   418,
   419,     0,   260,     0,     0,   239,     0,   241,   137,   133,
   240,     0,     0,   384,   305,   255,   256,   302,   304,   257,
   306,   303,   300,   299,     0,     0,     0,   488,  1103,   393,
   394,   392,   338,     0,   326,   470,   477,     0,   474,     0,
   884,   878,     0,   658,   660,   828,   827,     0,   829,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   848,   846,
   835,   849,   840,   841,   844,   842,   843,   853,     0,   863,
     0,   861,   742,   743,   744,   867,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   722,   720,   785,
  1306,     0,     0,   752,     0,   747,   750,   724,  1246,     0,
     0,     0,   786,  1307,     0,     0,     0,   723,  1245,     0,
     0,     0,   717,  1239,     0,     0,     0,   714,  1237,     0,
     0,     0,   715,  1238,     0,     0,     0,   709,  1232,     0,
     0,     0,   710,  1233,     0,     0,     0,   713,  1236,     0,
     0,     0,   711,  1234,     0,     0,     0,   712,  1235,     0,
   729,     0,     0,   823,     0,     0,   701,   700,     0,     0,
   592,     0,   588,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   803,   801,   754,   804,   794,   795,   798,   796,
   797,   807,     0,   756,     0,     0,   874,     0,   875,   876,
     0,     0,   758,     0,     0,   766,     0,     0,   764,     0,
     0,   765,     0,     0,   759,     0,     0,   760,     0,     0,
   763,     0,     0,   761,     0,     0,   762,   506,  1311,   575,
     0,   562,     0,     0,   577,   574,   577,   575,   573,   577,
   564,   566,     0,     0,   560,   586,     0,   547,   893,   892,
   897,     0,   110,     0,   483,     0,     0,   458,   529,   528,
   531,  1148,  1150,  1101,  1152,  1202,  1209,  1204,  1186,     0,
  1186,     0,  1179,  1180,     0,     0,   185,     0,     0,     0,
     0,     0,     0,     0,   184,   186,     0,     0,     0,   104,
     0,     0,     0,     0,     0,    69,    68,   276,     0,     0,
   442,   362,     0,     0,   176,   125,     0,   122,   243,   245,
     0,   131,     0,     0,     0,     0,     0,     0,   144,   130,
   132,   135,   139,     0,   307,   258,   346,   911,     0,     0,
     0,   494,     0,     0,   883,   721,   646,   879,   643,     0,
   855,   856,     0,     0,     0,   860,   854,   865,     0,   733,
   735,   738,   739,   740,   741,     0,     0,     0,   745,     0,
     0,   751,   749,   753,     0,     0,   725,  1247,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   728,   730,   825,     0,     0,
     0,     0,   703,     0,   590,   592,     0,   811,   813,   816,
   817,     0,     0,     0,   821,   808,   873,   755,   757,     0,
   776,   767,   784,   775,   782,   773,   783,   774,   777,   768,
   778,   769,   781,   772,   779,   770,   780,   771,     0,   554,
   557,     0,   576,   570,   571,     0,   572,   565,     0,   559,
     0,     0,   505,     0,   482,   461,     0,  1149,     0,     0,
  1212,  1182,  1186,  1183,  1186,     0,   208,   209,   187,   197,
   196,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   211,   213,   210,     0,     0,     0,     0,     0,     0,     0,
     0,   178,     0,     0,     0,   179,   107,     0,   395,    81,
    80,     0,   275,     0,     0,   270,     0,   584,   417,     0,
   136,     0,     0,     0,   169,   170,   172,   174,   141,   175,
     0,     0,     0,     0,     0,   142,     0,   149,   143,   145,
   476,   134,   261,     0,   348,   349,   351,   356,     0,   912,
   496,     0,   497,   340,     0,     0,   857,   858,   859,     0,
   742,   743,   744,   754,   756,     0,     0,     0,     0,   758,
     0,     0,   766,     0,     0,   764,     0,     0,   765,     0,
     0,   759,     0,     0,   760,     0,     0,   763,     0,     0,
   761,     0,     0,   762,   824,   685,     0,   688,   689,     0,
   591,     0,   818,   819,   820,   877,     0,   569,     0,     0,
   544,   546,   120,  1368,  1369,     0,  1370,  1371,  1151,  1315,
   589,  1211,  1152,  1184,  1185,     0,   200,   198,   206,     0,
   225,     0,   216,     0,   212,   215,   204,     0,     0,     0,
   207,   203,   193,   194,   195,   188,   189,   192,   190,   191,
   201,     0,   183,     0,   180,   106,     0,    83,   277,   278,
   274,     0,     0,     0,     0,     0,     0,   138,     0,     0,
     0,   167,   150,   160,   159,     0,     0,   168,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   228,   363,
     0,     0,     0,   359,     0,   498,   473,   847,   721,   755,
   757,   776,   767,   784,   775,   782,   773,   783,   774,   777,
   768,   778,   769,   781,   772,   779,   770,   780,   771,   686,
   687,   802,   563,   568,     0,     0,   567,     0,  1316,  1214,
  1213,     0,     0,     0,   224,   218,   214,   217,     0,     0,
   205,     0,   202,     0,    79,     0,   361,   423,   416,   238,
   140,     0,     0,     0,   163,   161,   166,   156,   157,   158,
   151,   152,   155,   153,   154,   164,     0,   148,     0,     0,
   231,   343,   350,   355,   354,     0,   353,   357,   913,     0,
   579,     0,  1372,     0,   221,     0,   219,     0,     0,   182,
   476,   281,   422,     0,     0,   171,   173,     0,   165,   475,
   226,   227,     0,   146,   230,   358,   356,     0,   581,   582,
   199,   223,   222,   220,   228,     0,   279,   282,   283,   284,
   285,   402,     0,     0,   401,   404,   415,   412,   414,   413,
   403,     0,   147,     0,     0,   229,   359,     0,   578,   231,
     0,   265,     0,   407,   408,     0,   162,   235,     0,     0,
   232,   233,   352,   580,   181,   280,   405,   409,   411,   406,
   234,   236,   237,   410,     0,     0,     0
};

static const short yydefgoto[] = {  2435,
     1,    37,    92,    93,    94,    95,    96,   701,   940,  1271,
  2080,  1576,  1876,    97,   868,   864,    98,    99,   100,   936,
   101,   102,  1214,  1521,   390,   880,  1833,   103,   331,  1293,
  1294,  1295,  1900,  1901,  1892,  1902,  1903,  2337,  2106,  1296,
  1297,  2222,  1863,  2304,  2305,  2341,  2374,  2375,  2421,  1888,
   104,   970,  1298,  1299,   105,   712,   968,  1605,  1606,  1607,
   106,   332,   107,   108,   707,   946,   947,  1879,  2084,  2231,
  2386,  2387,   109,   110,   473,   333,   973,   716,   974,   975,
  1608,   111,   112,   359,   506,   734,   113,   366,   367,   368,
  1316,  1625,   114,   115,   334,  1616,  2114,  2115,  2116,  2117,
  2264,  2347,   116,   117,  1586,   710,   955,  1282,  1283,   118,
   351,   119,   725,   120,   121,  1609,   475,   982,   122,  1573,
  1264,   123,   961,  2395,  2413,  2414,  2415,  2088,  1592,  2364,
  2397,   125,   126,   127,   314,   128,   129,   130,   949,  1277,
  1581,   613,   131,   132,   133,   392,   633,  1219,  1526,   134,
   135,  2398,   740,  2259,   990,   991,  2399,   138,  1217,  2400,
   140,   482,  1618,  1912,  2123,   141,   142,   143,   855,   400,
   638,   374,   419,   889,   890,  1224,   896,  1229,  1232,   699,
   484,   485,  1828,  2033,   642,  1226,  1192,  1503,  1504,  1505,
  1811,  1506,  1821,  1822,  1823,  2024,  2297,  2378,  2379,   721,
  1507,   830,  1437,   584,   585,   586,   587,   588,   956,   762,
   774,   757,   589,   590,   751,  1001,  1330,   591,   592,   778,
   768,  1002,   594,   825,  1434,  1750,   826,   595,  1132,   820,
  1044,  1011,  1012,  1030,  1031,  1037,  1372,  1662,  1045,  1462,
  1463,  1778,  1779,   596,   995,   996,  1326,   744,   597,  1194,
   873,   874,   598,   599,   315,   746,   277,  1907,  1195,  2348,
   387,   486,   601,   397,   602,   603,   604,   605,   606,   287,
   958,   607,  1115,   384,   144,   296,   281,   425,   426,   667,
   669,   672,   915,   288,   289,   282,  1545,   145,   146,    40,
    48,    41,   420,   164,   165,   423,   906,   166,   656,   657,
   658,   659,   660,   661,   662,   898,   663,   664,  1236,  1237,
  2038,  1238,   147,   148,   297,   298,   149,   501,   500,   150,
   151,   436,   676,   152,   153,   154,   155,   925,  1552,  1254,
  1546,   918,   922,   690,  1547,  1548,  1845,  2040,  2041,   156,
   157,   446,  1068,  1187,    42,  1255,  2189,  1188,   608,  1069,
   609,   861,   610,   691,    43,  1239,    44,  1240,  1562,  2190,
    46,    47
};

static const short yypact[] = {-32768,
  2622,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,  3138,-32768,-32768,-32768,-32768,-32768,  1613, 25243,   391,
   127, 24415,   531, 28820,   531,   -85,    84,   266,   -17, 28820,
   296,  2057, 29095,   159,  1751,   296,    27,   358,   379,   243,
   379,   296,    62, 26620, 26895,   -85,   296, 28820,    99,   140,
   208, 26895, 22558,   234,   309,   358, 26895, 27445, 27720,   140,
   -79,  4243,   581,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,   578,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,   354,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,   589,   141,-32768,-32768,-32768,-32768,-32768,
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
-32768,-32768,-32768,-32768,-32768,-32768,   360,-32768,-32768,-32768,
   360,-32768,-32768,   392, 24691,-32768,-32768,-32768,    21,-32768,
-32768,   531,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   522,
-32768,-32768,   572,-32768,   596,  1305,  1305,   753, 26895,   531,
   768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   531,
 28820, 28820,-32768,-32768, 28820, 28820,-32768, 28820, 26895,-32768,
   598,   553, 21982,   591,   531,    10, 26895, 28820,   531,-32768,
 28820,-32768, 28820, 28820, 28820,-32768,  1384,   701,-32768, 28820,
 28820,-32768,   480,-32768,-32768,-32768,-32768,-32768,   -15,   683,
-32768,-32768,-32768,-32768,-32768,   736,   593,-32768, 26895,   766,
-32768,-32768,   810, 11593, 24967,   -22,   771,   849,   -94,-32768,
-32768,   815,-32768,-32768,   865,-32768,   869,-32768,-32768, 26895,
-32768,   670, 28820,-32768,-32768,-32768,-32768,-32768,-32768, 26895,
   -15,-32768,   833,   916,-32768,   859,   947,-32768,   873,     9,
   296,  1065,-32768,-32768,   -79,  1060,  1023,  1060,  1029,-32768,
  1038,-32768,   161, 28820,-32768,   875,   875,-32768,-32768,  1102,
  1080,  1424, 28820,-32768,   392,-32768,   392,   920, 28820,-32768,
  1003, 28820,-32768,-32768, 29370,-32768,-32768,  1305,   912,  1011,
  1202,  1011,  1143,    39,  1044,   930,-32768,  1178,-32768, 26895,
  1121,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,   952,-32768,-32768, 28820,-32768,  1092,-32768,
-32768,  1171,  1104,-32768,   975,-32768,-32768,  1149, 22833,-32768,
   930,-32768,   989,-32768,   234,-32768,-32768,-32768,-32768,-32768,
   522,-32768,-32768,-32768,   997,   430,-32768,-32768, 28820,   -20,
     7, 28820, 28820,  -171,  -140,   195,   284,   314,   350,   453,
   518,   532,   538,   542,   559,   575,   629,   640,   654,   656,
   696,   698,   702,   741,   747,   757,   762,   780,   787,   801,
   804,   829,   831,   835, 24136,  1045,  1138,  1138,  1069,-32768,
  1070,  1086,-32768,  1095,  1186,  1103,  1110,-32768,  1122,   609,
  1289,  1138, 17553,  1136,-32768,  1148,  1152,  1167,   856,   -97,
  1168,-32768,-32768,-32768,   863,  1719, 17553,  1163, 17553,-32768,
 17553, 17553, 16957,   234,  1177,-32768,-32768,-32768,-32768,  1182,
-32768,   864,  1310,-32768,  5432,-32768,  1163,   -29,-32768,  1179,
  1189,-32768,  1187,-32768,-32768,-32768,   186,-32768,    20,   867,
-32768,-32768,-32768,-32768,   -41,  1357,    -4,    -4, 22271,   920,
 26895,  1301, 28820,-32768,  1104,  1390,   430,-32768,  1394,-32768,
  1404,-32768, 26895,-32768,-32768,-32768,-32768,   -79, 17553,   -79,
  1339,   306,  1427,-32768,-32768,-32768,   -85,-32768,-32768,-32768,
-32768,   -85,   -85,  1085,-32768,-32768,-32768,  1236,-32768,-32768,
  1244,  1252,-32768,-32768,-32768,  1267,-32768,   997,-32768,  1270,
 25243,  1396,  1424,-32768,-32768,-32768,  1275,-32768,-32768,-32768,
-32768,-32768,   704,-32768,-32768,-32768,-32768,-32768,   628,  1007,
-32768,  1316, 28820,-32768,  1562,  1317,-32768,-32768,    85,  1341,
   128,-32768,   128,   -79,-32768,-32768,    61,  1387,  9529,  1371,
-32768,   650,  1334,   234, 21693,-32768,  1485,-32768,  1525, 17553,
-32768, 28820, 26895,-32768,-32768,-32768,-32768, 27995,-32768,-32768,
-32768, 28820, 28820,  1522,  1466,-32768,  1469,  1351, 21129,-32768,
-32768,  1549,-32768,  1481,  1163,  1369,  1187,  1400, 17553,-32768,
-32768,  1623, 16957,   997,   997,   997,-32768,-32768,  1534,  1247,
   997,-32768,  1537,  1538,  1570,  1587,-32768,-32768,  1138,-32768,
  1560, 17553,   997,-32768, 19639, 16957,  1566,-32768,  9805,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
  1186,-32768,  1518,-32768,-32768,-32768,-32768,   564, 17851,-32768,
  1714,  1714,  1714,  1474,  1475,  1478,  3553,-32768,    71,-32768,
   997, 25795,  5247, 17553, 18447,  1483,   381, 17553,   494, 17553,
-32768,-32768, 18149, 10997, 11891, 12189, 12487, 12785, 13083, 13381,
 13679,-32768,   -42, 11593,  1671, 23108,  7735, 28820, 25519,-32768,
-32768,-32768,-32768,-32768,-32768, 29645,-32768,-32768,-32768,-32768,
-32768,  1163,   -54,-32768,  1489,    58,   628,-32768,  1539,   130,
    10,-32768,  1513,-32768,-32768,-32768,  1488,-32768,  1491,-32768,
  4399,-32768,  1643,     4,   660,-32768,  1768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,  1506,  3015,    11,    11, 28820,
-32768, 28820,-32768,  1424,-32768,-32768,   392,  1495,-32768,-32768,
-32768,  1496,   231,   398,  1775,-32768,-32768,-32768,-32768,-32768,
-32768,   213,  1720,  1720,  1720,-32768,   531,-32768,-32768,   184,
   184,-32768,-32768,-32768,-32768,  1653,  1651,  1523,  1588,-32768,
  1657,-32768,-32768,-32768,   887,-32768,-32768,-32768,-32768,  1554,
  1661,  -144,-32768,  -144,  -144,  -144,  -144,-32768, 26345,  1750,
  1592,  1540,  1541,   896,-32768, 26895,   -67,  5432,-32768,-32768,
  1524,  1520,  1527,-32768,-32768,   392, 27170,-32768, 11593,   927,
-32768,  1163, 27170, 17553,   -11,-32768,-32768, 28820,  4383,  1649,
  1746,-32768,   -74,  1529,  1530,   942,  1532,-32768,-32768,-32768,
  1533,  1725,-32768,  1552,   -18,   274,  1668,  1704,-32768,  3600,
   979,  1561,  1564,  1565,  1569, 19639, 19639, 19639, 19639,  1553,
   511,  1163,  1572,-32768,   186,   -64,  1559,  1669, 13977, 16957,
 13977, 13977,  4907,   -84,  1574,  1578,   265,   659,   898,   539,
  1580,  1581, 17851,  1582,  1583,  1584, 17851, 17851, 17851, 17851,
 16957,   580,  5823,  1163,  1586,   583,   971,   682,-32768,    15,
-32768,  1229, 17553,  1590,  1591,  1593,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,  1560,  1595,-32768,  1599,
  1600,-32768,  1602,  1603,  1604,-32768, 18447, 18447, 18447, 18447,
 17553,   329,  1163,  1605,-32768,   186,-32768,-32768,  7006,-32768,
   424,-32768,-32768,   860, 18447,  1608, 17553,  4494,  1609,  1610,
 14275,   564,  1611,  1614,-32768, 14275,  3249,  1615,  1616, 14275,
  1353,  1618,  1620, 14275,  1353,  1627,  1628, 14275,    70,  1629,
  1630, 14275,    70,  1631,  1640, 14275,  1714,  1642,  1644, 14275,
  1714,  1645,  1647, 14275,  1714,   309,  1563,-32768,    20,-32768,
  7374,  1104,-32768,  1617,-32768,-32768,  1622,-32768,   -73,  1617,
-32768,-32768, 28820,-32768,-32768, 24136,  1104, 23383,  1607,  1775,
-32768,-32768,-32768,   414,  1796,  1674,  1692, 28820,-32768, 17553,
-32768,-32768,   814,-32768, 28820,-32768,-32768,-32768,  -143,-32768,
-32768,  1718,-32768,  1925,-32768,   649,-32768,   -85,  2652,-32768,
  1655,  1656,-32768,  1684,-32768,-32768,    54,    54,   642,  1665,
   642,  1663,-32768,-32768,  1279,  1293,  1667,-32768,  1836,  1840,
  1670, 26345,-32768, 28820, 28820, 28820, 28820,-32768,-32768,-32768,
  1855,  1855, 26895,    61,    77,  1691,-32768,-32768, 26070,-32768,
-32768,  1779, 26070,   333,   997,-32768,-32768,-32768,-32768,-32768,
-32768, 28820,   996,-32768,-32768,-32768,-32768,  1008,-32768,  4836,
  1534, 21982, 21404, 21404, 21693,-32768,  1787,  1869,  1869, 28820,
-32768, 28270,  1563, 28820,-32768,  1785,-32768,  1061, 28820,   -77,
-32768,-32768,  5283, 16957,-32768,  1881,  5247, 28820, 28820,-32768,
 17553, 16957,-32768,-32768,-32768,   997,-32768,-32768,-32768, 17553,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768, 17553,   997,
-32768, 19639, 19639, 16957, 10103,   447,  1926,  1926,   178,-32768,
  5247, 19639, 20235, 19639, 19639, 19639, 19639, 19639,-32768,  8333,
 16957,  1876,-32768,-32768,  1688,   -84,  1690,  1693, 16957,-32768,
 17553,   997,   997,  1534,  1247,  1847,-32768, 19639, 16957, 10401,
   915,-32768,  1936,-32768,  1936,-32768,  1936,-32768,  1697,  5247,
 17851, 18447,  1699,   501, 17851,   699, 17851,   796,   836, 14573,
 11295, 14871, 15169, 15467, 15765, 16063, 16361, 16659,   894,  8034,
 17851,  1163,  1701,  1886,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,    -3,  3788,   276,-32768,  1590,-32768, 18447,
   997,   997, 19639, 16957, 10699,   488,  1941,  1941,  1941,  1324,
  5247, 18447, 18745, 18447, 18447, 18447, 18447, 18447,-32768,  8632,
-32768,  1705,  1708,-32768,-32768,-32768,-32768,-32768,   868,  7006,
   860,  1534,  1534,  1710,  1534,  1534,  1712,  1534,  1534,  1713,
  1534,  1534,  1717,  1534,  1534,  1722,  1534,  1534,  1731,  1534,
  1534,  1732,  1534,  1534,  1734,  1534,  1534,  1736, 26895,   392,
-32768, 26895,-32768,  1721,  1700,-32768, 28545,  1730,  1893, 23658,
-32768,-32768,-32768,-32768,-32768,-32768, 16957,-32768,-32768,-32768,
  1830,-32768,  1928,  1766,  1769,  1077,-32768,-32768,-32768,-32768,
-32768,  1743,   660,   660,     4,-32768,-32768,  1506,  1177,-32768,
-32768,-32768,-32768, 28820,-32768,-32768,  1741,    54,  1742,   308,
   417,-32768,   642,-32768,   642,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768, 20533,  1747,  1748, 28820,  1098,  4836,-32768,
   123,-32768,  1864,-32768,  1933,  1783,  1783,  1939,  1902,-32768,
-32768,-32768,   -22,-32768,   952,  1990,-32768,-32768,-32768,-32768,
-32768,  1877,-32768,    81, 26345,  1831, 28820,-32768,  1911,   937,
-32768,  1839, 28820,   781,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,   531,  1773,   192,-32768,-32768,-32768,
-32768,-32768,-32768,  1959,-32768,-32768,-32768,  1776,-32768, 16957,
-32768,-32768,  1778,-32768,-32768,  5432,-32768,  1780,  5432,  1648,
  1781,   227,  1782,  1784, 13977, 13977, 13977,  1786,-32768,-32768,
   495,   447,    74,    74,  1926,  1926,  1926,-32768,   328,   -84,
 16957,-32768,-32768,-32768,-32768,   -84,  5308,  1788,  1789,  1791,
  1792,  1793,  1795, 13977, 13977, 13977,  1797,   939,   960,  1847,
-32768,   924,  7006,   977,   438,   988,   994,   959,-32768, 18447,
  1777, 17851,  5839,-32768,  1799,  1800, 14275,   915,-32768,  1801,
  1802, 14275,  3741,-32768,  1811,  1812, 14275,  3680,-32768,  1813,
  1814, 14275,  3680,-32768,  1815,  1816, 14275,    87,-32768,  1818,
  1819, 14275,    87,-32768,  1820,  1821, 14275,  1936,-32768,  1822,
  1824, 14275,  1936,-32768,  1825,  1826, 14275,  1936,-32768,  1827,
  1000,   340,  1829,-32768,  1534,  1842,-32768,-32768, 17255,  1844,
  1590,  1832,-32768,   891,  1828,  1838,  1845,  1846, 13977, 13977,
 13977,  1848,-32768,-32768,  1175,   488,   101,   101,  1941,  1941,
  1941,-32768,   351,-32768, 23933, 18447,-32768,  1849,  1798,-32768,
  1851,  1852,-32768,  1853,  1854,-32768,  1856,  1857,-32768,  1858,
  1859,-32768,  1860,  1861,-32768,  1862,  1863,-32768,  1865,  1866,
-32768,  1867,  1868,-32768,  1870,  1872,-32768,-32768,-32768,  1465,
  1873,-32768, 26895,  1952,  1929,-32768,  1929,   750,-32768,  1929,
  1700,-32768,  1958, 25795,-32768,-32768,  2068,  2031,-32768,-32768,
-32768,  1947,-32768,   -79,-32768,  1901, 28820,-32768,-32768,-32768,
-32768,-32768,  1892,-32768,  1506,-32768,-32768,-32768,   642,  1879,
   642,  1878,-32768,-32768,  1880, 20533,-32768, 20533, 20533, 20533,
 20533, 20533,  1740,  1882,-32768,  1883, 28820, 28820,  1160,-32768,
  2085,  2088, 28820,   531,  1917,-32768,-32768,  1971,  2090,    61,
-32768,-32768,   234, 26895,-32768,-32768,  1896,-32768,-32768,-32768,
  2064,-32768,  1897, 28820, 19043,  2051,  2070, 28820,-32768,-32768,
   937,-32768,-32768,   234,-32768,-32768,-32768,-32768, 28820,  2050,
  2055,-32768,  2054, 11593,-32768,-32768,-32768,-32768,-32768,  5247,
-32768,-32768,  1907,  1908,  1910,-32768,-32768,   -84,  5247,  1032,
  1033,  1036,  1040,  1049,  1053,  1912,  1915,  1916,  1066, 18447,
  1918,  1075,  1108,  1127,  1166,  7006,   959,-32768,  1534,  1534,
  1920,  1534,  1534,  1923,  1534,  1534,  1924,  1534,  1534,  1931,
  1534,  1534,  1934,  1534,  1534,  1935,  1534,  1534,  1937,  1534,
  1534,  1940,  1534,  1534,  1942,  1128,  1139,  1163,  1943,  1534,
  1945,  1948,  5432,  1534,-32768,  1590,  5247,-32768,-32768,-32768,
-32768,  1949,  1950,  1953,-32768,-32768,-32768,  1175,-32768, 23933,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  2098,-32768,
-32768, 26895,-32768,-32768,-32768,  2106,-32768,-32768, 26895,-32768,
 16957, 17553,-32768,   234,-32768,-32768,   566,-32768,   -85,    16,
-32768,-32768,   642,-32768,   642, 20533,  2086,  1028,  2178,  2178,
  2178,  2262,  5247, 20533, 23933,  1955,   565, 20533,   726, 20533,
-32768,-32768, 20831, 20533, 20533, 20533, 20533, 20533, 20533, 20533,
 20533,-32768,  9230,  1246,  1379,-32768,-32768, 19341,-32768,  1954,
-32768,   234,-32768,   491,  2080,-32768,  2118,  1104,  1960, 28820,
-32768, 20533,  1335,  1957,-32768,  1961,  1962,-32768,-32768,-32768,
 19341, 19341, 19341, 19341, 19341,   782,  1964,-32768,-32768,-32768,
  1966,-32768,-32768,  1967,  1970,-32768,-32768,   -55,  1972,  1883,
-32768, 28820,-32768,-32768,  1413,  1969,-32768,-32768,-32768,  1974,
  1140,  1180,  1183,   209,  1198, 18447,  1975,  1976,  1977,  1200,
  1978,  1979,  1204,  1980,  1982,  1210,  1983,  1985,  1213,  1986,
  1987,  1214,  1988,  1991,  1228,  1992,  1993,  1237,  1994,  1999,
  1238,  2001,  2002,  1264,-32768,-32768,  2003,-32768,-32768,  2004,
-32768,  2006,-32768,-32768,-32768,-32768, 26895,-32768, 26895,   179,
   -84,  5432,-32768,-32768,-32768,  3015,-32768,-32768,   566,-32768,
  1177,-32768,  1506,-32768,-32768,  5400,-32768,-32768,  2086,  2193,
-32768, 23933,-32768,   449,-32768,-32768,  1253, 23933,  1973, 20533,
  6100,  1028,  3812,  4315,  4315,   171,   171,  2178,  2178,  2178,
-32768,  1416,  6030,  2126,-32768,   782,   531,-32768,-32768,-32768,
-32768, 28820,   234,  2052, 28820,  2008,  3507,-32768, 19341,   997,
   997,   535,  2252,  2252,  2252,    -9,  5247, 19937, 19341, 19341,
 19341, 19341, 19341, 19341, 19341, 19341,  8931, 28820,  2176,  1779,
 28820,  5247,  5247,   302, 28820,  2014,-32768,-32768,  1271,   247,
  1273,  1283,  1284,  1290,  1298,  1306,  1309,  1325,  1329,  1333,
  1352,  1356,  1365,  1383,  1389,  1391,  1395,  1397,  1399,-32768,
-32768,-32768,-32768,-32768, 17553,  2019,-32768,  2974,-32768,-32768,
-32768,  5247, 23933,  1432,-32768,-32768,-32768,-32768,  2249, 23933,
  1253, 20533,-32768, 28820,-32768,  2022,-32768,  2094,-32768,-32768,
-32768,   612,  2025,  2027,-32768,-32768,   535,   782,   886,   886,
   183,   183,  2252,  2252,  2252,-32768,  1459,   782,  1460,   118,
  2179,-32768,-32768,-32768,-32768,   531,-32768,-32768,-32768,  1463,
  5432, 28820,-32768,  2029,-32768, 23933,-32768, 23933,  1464,  6030,
  1966,   710,-32768,  1111,  5247,-32768,-32768, 19341,-32768,-32768,
-32768,-32768,     2,-32768,  2179,-32768,   -55,  1467,-32768,-32768,
-32768,-32768,-32768,-32768,  2176,  1471,-32768,-32768,-32768,-32768,
-32768,-32768,   100,  1096,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,  2032,   782,   251,   251,-32768,   302, 28820,-32768,  2179,
   710,-32768,  2038,   100,  2043,  2037,-32768,-32768,  2278,   153,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,  2046,-32768,-32768,
-32768,-32768,-32768,-32768,  2342,  2344,-32768
};

static const short yypgoto[] = {-32768,
-32768,-32768,-32768,  2253,-32768,-32768,-32768,  1895,  1652,  1407,
-32768,  1082,   779,-32768,  1739,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,  1505,  1097,
   765,  1100,-32768,-32768,-32768,   464,   273,-32768, -1993,-32768,
  -922,-32768,  -949,    57, -2020,   -16,   -32,     8,   -37,-32768,
-32768,-32768,-32768,   785,-32768,-32768,-32768,-32768,-32768,   255,
-32768,-32768,-32768,-32768,-32768,-32768, -1248,-32768,-32768,-32768,
-32768,   -27,-32768,-32768,-32768,-32768,  -324,   794,-32768,  1081,
  1083,-32768,-32768,  2299,  1996,  1762,-32768,  2321,-32768,  1884,
  1402,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   132,
    22,     6,-32768,-32768,   143,  1909,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,  2058,  -336,-32768,-32768,-32768,
  -342,-32768,-32768,-32768,    12,-32768, -2048,-32768,-32768,-32768,
   -13,-32768,-32768,-32768,   808,-32768,-32768,-32768,-32768,-32768,
-32768,   818,-32768,-32768,-32768,  2314,-32768,-32768,  1184,-32768,
  2016,   -10,-32768,    50, -1550,  1099,    -6,-32768,-32768,    -5,
-32768,  1535,  1105,-32768,-32768,  -506,   -88,  5753,-32768,  1231,
  2005,-32768,-32768,-32768,  1199,-32768,-32768,   889,  -510,-32768,
  -344,   156,-32768,-32768,-32768,-32768,  1555,-32768,-32768, -1460,
-32768,   928,-32768,   601,   608,  -955,-32768,-32768,    23,  -617,
-32768, -1517, -1408,  -817,  1871,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  -684,  -464,-32768,-32768,-32768,  3435,-32768,
-32768,  -293,  -404,   680,-32768,-32768,-32768,  4922, -1006,  -573,
  -656,  1047,-32768, -1122,  -623,  -864,-32768,-32768,  -970,   752,
-32768,   487,-32768,-32768,-32768,  1441,-32768,-32768,  5373,  1579,
-32768,  1232,  -976,  1585,-32768,    -7,  -297,-32768, -1487,    95,
  -189,    -2,  3764,-32768,  5006,   737,    -1,     1,   -31,  -306,
  -597,  2140,   618,-32768,-32768,   -34,-32768,  2162,-32768,  1542,
  2018,-32768,-32768,  1543,  -386,   -30,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,  -146,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,   513,-32768,-32768,-32768,   910,
-32768, -1804,-32768,-32768,-32768,  2076,-32768,-32768,-32768,-32768,
-32768,  1963,  1546,-32768,-32768,-32768,-32768,  1577, -1207,  1245,
  -368,-32768,-32768,-32768, -1217,-32768,-32768,-32768,   263,-32768,
-32768,  -234,  -365,  1472,  2295,  1536,-32768,   970,  -423,  -795,
   651,  1282,    88,   -49,-32768,   286,-32768,     0,-32768,   271,
-32768,-32768
};


#define	YYLAST		29922


static const short yytable[] = {    38,
    45,    39,   410,   450,   741,   496,   305,   882,   474,  1263,
   494,   677,  1318,   452,   280,  1116,  1869,   278,   422,   290,
   278,  1843,  1830,   852,   957,  1579,   385,   455,   124,  1753,
  1549,   136,   487,   859,  2200,   137,   139,   431,   859,  1137,
  2039,  1810,  2192,  1554,   353,   674,   490,   502,  1371,  2247,
   158,   300,   398,   159,   862,  2262,   160,   679,  1191,  1747,
   336,   161,  1748,   162,   163,   680,  1227,   382,  1375,   689,
  1377,  1378,   447,   612,   394,   396,   361,   311,   124,   402,
   593,   136,   681,   752,  2226,   137,   139,   617,   682,   683,
  1324,   867,    73,   863,    73,   853,  1003,   770,  2404,  1342,
   777,   362,   737,  -602,    73,  1533,  1331,  2242,  2243,  2244,
  2245,  2246,   415,   306,  1628,   307,  1331,  1331,  1343,  1036,
  1446,  1447,  1448,  1449,   372,  -602,  1331,   503,   833,   684,
   479,   416,  1361,   318,  -603,    25,   284,   291,  1469,   284,
   299,   302,   303,  1285,   504,  1400,   932,   302,   933,   292,
   302,   363,  1344,   685,   354,  1534,  -603,   943,  1257,  1451,
   373,   302,   302,   686,   735,   302,   312,   742,   364,   302,
   302,   997,  1580, -1109,   302,   302,   302,  -673,    73,   480,
   158,   313,   944,   159,   994,   934,   160,  2309,   304,   618,
  2405,   161,  1259,   162,   163,   418,    64,   942,   738,  -673,
  1199,   644,   645,   625,   646,   647,   272,   648,   649,   274,
   650,   432,   651,   697,  1332,   652,   653,   654,   655,   481,
    91,    70,    91,  1871,  1332,  1332,  1333,  1512,   308,  2053,
  1643,  2371,    91,   273,  1332,   275,  1361,  2263,   365,  1260,
   687,  2247,  1212,   705,  1206,  2322,   688,   706,  2432,   945,
   430,   877,   273,   278,  2327,  2328,  2329,  2330,  2331,  2332,
  2333,  2334,  2335,  2338,    25,  1672,  2248,  1451,  1261,   854,
   273,   337,   505,  2372,  2249,  2250,  2251,  2252,  2253,  2254,
  2255,  2256,  2355,    25,  1228,  1361,   916,   717,    75,   824,
  1257,  2325,   275,  1920,  1258,  1872,  1749,   275,  1205,  2433,
   272,   449,  1209,   274,  1019,  1451,   739, -1319,   389,   634,
   433,  1213, -1319, -1109,  2193,  2396,  2074,  2075,   453,   454,
  1757,   459,   499,  2418,  1259,   458,   636,   304,  2295,   483,
  1847,  1137,  1137,  1137,  1137,  2382,  1013,  2383,   488,   935,
   489,   938,  1985,   278,   614,  1853,    87,  1854,   497,  1137,
    25,   273,  2021,  -909,   819,  1910, -1208,   507,   849,   850,
   851,   511,  1366,  1367,  1368,  2428,   600,   369,  1074,  2296,
   939,  1260,   284,   665,  2403,  1416,  1417,  1418,   124,   434,
  1911,   136,   622,  1376,  1648,   137,   139,  1451,  2039,  1456,
  1457,  1458,   626,  1452,  2419,  1682,   302,   291,  1269,   507,
  1261,   290,  1356,  1357,  1358,  1359,   678,   291,   302,   302,
  1345,   391,   302,   302,  1062,   302,   302,   355,  1321,  1677,
   302,  2118,   478,  2420,   302,   302,   291,  1270,   302,  1588,
   302,   302,   302,  1754,   304,   393,   692,   302,   302,   694,
   309,   360,   696, -1109,  1346,  1765,  1766,  1767,  1768,  1769,
  1770,  1771,   713,  1363,  1589,   310,   302,   415,  1369,  2069,
  2070,  2071,   284,   729,  1364,  1365,  1366,  1367,  1368,  -604,
  1308,  2254,  2255,  2256,  1762,   361,   416,   302,  1649,   312,
   302,  -909,  1600,   399,  1453,  -916,   726,   302,  1140,  1644,
  1419,  -604,  2346,   677,   313,  1454,  1455,  1456,  1457,  1458,
   362, -1275,  1363,   731,  1590,  1361, -1275,   417,   273,  1633,
    25,   302,  1262,  1364,  1365,  1366,  1367,  1368,  1331,   291,
   302,  1591,  1453,   356,  1673,  1141,   302,  1142,  1249,   302,
  1331,  1466,   302,  1454,  1455,  1456,  1457,  1458,   894, -1276,
   418,  1331,   732,  1650, -1276,  1942,  1451,   302,   733,   888,
   363,   892,   828,  1361,   895,    25,  2306, -1253,  -605,  1459,
   593,  2178, -1253,  1145,   302,   357,  1143,   364,  2180,  1361,
  1467,  2266,   285,  1751,  1509,   272,   302,  2171,   274,  1758,
  -605,   358,  1679,   865,  1943,   869,   869,    25,  -606,  1515,
  1265,  1266,  1267,  2247,   957,  2307,   302,   899,   957,   302,
   302,   414,   899,   899,  1453,  1849,  1137,   413,  1684,  1468,
  -606,   875,  1146,   878,  1244,  1454,  1455,  1456,  1457,  1458,
   881,     2,   833,  1944,  -607,   887,  1332,  1147,  1927,  1362,
  1268,  2086,   302,  1764,  2308,   424,   913,   365,  1332,   278,
  1977,  2042,   290,  2044,  1137,  1685,  -607,  1686,  1062,  1332,
   410,  1996,  1062,  1062,  1062,  1062,  1137,  1137,  1137,  1137,
  1137,  1137,  1137,   817,   272,   286,   911,   274,   275,   275,
  2247,  1885,  2203,  2300,  1923,  1924,  1925,    25,  2365,  1519,
  1520,  1584,   971,  1945,   437,  1587,  1687,  1387,   273,   837,
   930,  1392,  1394,  1396,  1398,   593,    25,  1036,  1251,   283,
   283,   763,   283,  1936,  1937,  1938,   302,  2339,   302,  2204,
   302,  2205,   435,  1659,  1660,   980,  2293,  1851,  2294,   979,
   302,   438,  1666,   919,   920,   983,   764,  -608,  1642,   984,
   985,   921,  1036,  1364,  1365,  1366,  1367,  1368,  1651,  1652,
  1653,  1654,  1655,  1656,  1657,   765,   208,  2319,  2229,  -608,
  2206,  1600,  1004,  1005,  1006,  2230,   498,   275,   284,  1014,
   291,  1241,  1242,  1742,   623,   448,    64,  1399,  1690,  1998,
  1363,  1021,   460,  2118,  1454,  1455,  1456,  1457,  1458,   218,
   302,  1364,  1365,  1366,  1367,  1368,  1363,  1036,  1992,  1993,
  1994,    70,  -609,   451,   841,  2208,   302,  1364,  1365,  1366,
  1367,  1368,   302,  1773,   272,   461,  -610,   274,   766,   302,
   302,   842,  -611,  1629,  -609,   302,  -612,  1691,   477,   302,
   302,  2252,  2253,  2254,  2255,  2256,   302,  1075,  -610,  1076,
   495, -1258,  1692,  -613,  -611,  2194, -1258,  2195,  -612,  2184,
  2247,    73,  2185,   508,  2209,    25,   600,    26,  1744,  -614,
   847,   848,   849,   850,   851,  -613,  1201,   824,    75,  2210,
   962,  2025,   824,  1815,  2027,   963,   824,  2186,   317,  1816,
   824,  -614, -1228,   352,   824, -1229,   964, -1228,   824,   371,
 -1229,  1817,   824,   290,   386,   509,   824,  2248,   965,   966,
   824,   510,  1230,  1233,  1137,  2249,  2250,  2251,  2252,  2253,
  2254,  2255,  2256,  -615,  1819,    38,  2047,    39,  2048,  2049,
  2050,  2051,  2052,   967,  -616,   512,    87,  1820,   833,   302,
   817,  1250,  1252,   679,   923,  -615,  1451,   924,  -617,   835,
  -618,   680,  1776,  2134,   428,   283,  -616,  2125,  1550,    25,
  1231,  1551,  1537,   302,  2247,   302,   302,  1538,   681,  1451,
  -617, -1254,  -618,   302,   682,   683, -1254,  1987,  1382,   513,
  1287,  1013,  1287,  1287,  1287,  1287,   494,   494,  1307,    91,
  -619,   615,  -620,  1400,  1421,   616,  -621,  1602,   836,  1422,
  1137,   600,  1451,   272,   273,   837,   274,   619,  1940,    25,
   620,    26,  -619,-32768,  -620,   684,  1062,   302,  -621,   302,
  1062,   291,  1062,   283,  1928,  1062,  1062,  1062,  1062,  1062,
  1062,  1062,  1062,  1062,  1893,  -622,  1062,  1400,   621,   685,
  1894,  -623,  1839,  1840,   291,   283,   926,   927,  1402,   686,
  1631,  -624,  1895,   628,   928,  1681,  -625,  -622,  1637,  1689,
  1404,  1694,   629,  -623,  1699,  1704,  1709,  1714,  1719,  1724,
  1729,  1734,  1739,  -624,  -626,  1743,   302,  2248,  -625,   630,
    25,  -627,    26,   302,  1309,  2249,  2250,  2251,  2252,  2253,
  2254,  2255,  2256,   631,   302,  -628,  -626,  1403,  -629,   632,
   302,  1896,   283,  -627,  1404,   302,  2053,   283, -1267,  1529,
   841,   635,-32768, -1267,   639,  1897,  2196,  -628,  1530,  1531,
  -629,  1898,  2126,  -630,  2199,  -631,   687,   842,  2207,  -632,
  2211,  2130,   688,  2212,  2213,  2214,  2215,  2216,  2217,  2218,
  2219,  2220,   637,  2223,  1899,  -630,   902,  -631, -1269,  2270,
  -664,  -632,   903, -1269,   904,   843,   905,  -601,  -662,   641,
   643,  -600,  2237,  1453,  1137,  1408,   847,   848,   849,   850,
   851,   673,  -664,  2057,  1454,  1455,  1456,  1457,  1458,  -601,
  -662,  2248,  1409,  -600,   900,   901,  1453,   668,   671,  2172,
-32768,-32768,  2252,  2253,  2254,  2255,  2256,  1454,  1455,  1456,
  1457,  1458,  1522,  1508,   693,  1279, -1227,  1280,  1539,  1408,
 -1256, -1227,    64,   304,  1305, -1256,  1306,  1383,   875,  1453,
   698,  1414,  1415,  1416,  1417,  1418,  1409,    64,   700,   704,
  1454,  1455,  1456,  1457,  1458,  1527,  1286,    70,  1288,  1289,
  1290,  1291,  1527,  1831,  1451,  1319,   817,  1320,   702,   709,
  2136, -1244,    70,  1451,  1410,  2198, -1244,    38,  1541,    39,
  1336,   708,  1337,  1582,   711,  1414,  1415,  1416,  1417,  1418,
   714,   715, -1242,  1560,  1560,  1561,  1561, -1242,  2061,   860,
  2311,   718,   719, -1230,  1574,  1578,  -909,    73, -1230, -1273,
  -916,  1611,  1611,   722, -1273,  2062,    25,  1350,   302,  1351,
 -1268,   747,    73,  1593,    75, -1268, -1271,  1062,   728,  1594,
   302, -1271, -1251,   302,  1595,   302,  1596, -1251,   723,    75,
   720,  1287,  1287,  1610,  1610,   302,  1597,  1620,  1598,  1622,
   439,  2053,   302,   440,  2067,  2068,  2069,  2070,  2071,   441,
   442,   283,  2055,   283, -1255, -1257,  1948,  1308, -1259, -1255,
 -1257,   750, -1260, -1259,  1638,   443,  1915, -1260,   444,   200,
  1137, -1261,    87,   758,   749, -1262, -1261,  1423,  1641,   302,
 -1262,   302,   302,   302,   302,   747,  2392,    87, -1266,   854,
   302,  1626,  2360, -1266,   202,   769,   302, -1272,   753,   754,
   302,  2056, -1272,  1424,  2181,  1837,   832,  1838,  2057,   302,
  1668,  1669,   833,   207,   208,   755,-32768,   817,   834,   302,
   302,   302,   302,   835,   756,    91,  1595,   302,  1870,   302,
 -1270,   302,   759,  1399,  2165, -1270,   302,  2393,  1399,   760,
  2394,   833,  1893,  1399,   817,   302,   302,   218,  1399, -1274,
 -1250,   761,   835,  1399, -1274, -1250,  1008,  1009,  1399,  2326,
  1895, -1252, -1263,  1399,  1752,   772, -1252, -1263,  1399,  1755,
  1756,  1453,   836,  1399,  2344,  2345,   223,   773,   817,   837,
  1453,   775,  1454,  1455,  1456,  1457,  1458,   838,  1319,   819,
  2076,  1454,  1455,  1456,  1457,  1458,   776,   779,   839,  1070,
  2234,   836, -1264,   829,   856, -1265,   840, -1264,   837,  1896,
 -1265,   831,   858,  2061,  2354,  1138,   838,   817,   857,   866,
 -1277,  1808, -1279,  1897,  1508, -1277, -1287, -1279,   879,  1898,
  2062, -1287, -1285,  1189,  1425, -1286, -1280, -1285,   883,  1202,
 -1286, -1280,  1426,  1427,  1428,  1429,  1430,  1431,  1432,  1433,
 -1281,   897,  1899,   445,   275, -1281,   885,   893,  2063, -1284,
 -1282,  1233,  1233,  1230, -1284, -1282,   886,   907,   817,  2067,
  2068,  2069,  2070,  2071,  1319,   908,  2224,  2402,  1850,  1852,
  1814,   614,  1556,   909,   841,  1557, -1283,   817,    25,   912,
    26, -1283,   910, -1243,   283, -1278,  1556,  2081, -1243,  1557,
 -1278,   842,    25,   917,    26, -1297, -1288,  1558,  1815,  1559,
 -1297, -1288, -1305,   841,  1816,   914,   302, -1305, -1225,   302,
 -1296,  1558,   937,  1563,   302, -1296,  1817,   302, -1303,   843,
   842, -1294,  1593, -1303,  1906,  1818, -1294,   844,   845,   846,
   847,   848,   849,   850,   851,   929,   931, -1304,   833,  1819,
   593, -1295, -1304,   948,  1763, -1298, -1295,   960,   843,   835,
 -1298,   302,  1820,   969,   976,   977,  1189,-32768,-32768,   847,
   848,   849,   850,   851, -1289, -1109,   986,   987, -1299, -1289,
   989,  2019,   158, -1299,   302,   159,   817, -1290,   160,   492,
   988,   993, -1290,   161,   998,   162,   163,   465,   466,   467,
   468,   469,   470,   471,   472, -1302,   994,  1319,   836,  2225,
 -1302, -1293,   302, -1300,   302,   837, -1293, -1291, -1300, -1301,
   302, -1292, -1291,   838, -1301,  -909, -1292,   272,   286,  1000,
   274,   275,  1908,  1070,   839,    73,   833,  1070,  1070,  1070,
  1070,   854,   834,  2267,  2312,   777,  2313,   835,  1015,  1016,
   860,   780,   781,   782,   783,   784,   785,   786,   787,   788,
  2356,   789,  2357,   790,   791,   792,   793,   794,   795,   796,
   797,   798,   799,  1038,   800,  2236,   801,   802,   803,   804,
   805,  1017,   806,   807,   808,   809,   810,  2368,  1319,  2369,
  2370,   722,  2356,  2377,  2384,  2408,   836,  2409,  1018,  2411,
   817,  2412,   833,   837,  1071,  1072,  1073,  1138,  1138,  1138,
  1138,   838,  1139,  1191,  1208,  1814,  1216,  1218,  1211,  1220,
   841,  1225,   839,  1234,  1235,  1138,  1247,  1248,  2053,  1253,
   840,  1258,  1273,  1274,  2054,  1508,  1275,   842,  1276,  2055,
  1284,   200,   554,  1815,  1278,  1281,  1301,  1302,   811,  1816,
  1311,  1328,  1310,  1303,  1304,  1312,  1329,  -575,   559,  1334,
  1335,  1817,  1338,  1339,  2036,   843,   202,  1340,  1347,   860,
  1818,  1348,   560,   844,   845,   846,   847,   848,   849,   850,
   851,  2087,  1341,  1360,  1819,   207,   208,  2401,  2056,  1373,
  1352,  1500,   817,  1353,  1354,  2057,   566,  1820,  1355,  1374,
  2079,  1370,  2113,  2058,  1380,  1465,  2089,  1381,   841,  1384,
  1385,  1388,  1389,  1390,  2059,  1420,  1436,  1523,  1438,   218,
  1517,  2093,  2060,  1439,  1440,   842,   812,   813,  1441,  1442,
   302,  1443,  1444,  1445,  1460,  1400,   600,  1470,  1472,  1473,
  1475,   302,  1510,  1476,  1478,  1479,  1402,  1481,   223,  1482,
  2315,   814,  1511,   843,   302,   338,  1484,  1485,  1487,  1488,
  1490,   844,   845,   846,   847,   848,   849,   850,   851,  1491,
   339,  1493,   747,  1494,  1496,   340,  1497,  1524,  1918,  1525,
  1535,  1536,   341,   342,   302,   302,   343,  1542,  1543,  1544,
   302,   291,  1553,  1555,  1565,  1403,  1564,   344,  1566,  1567,
  2061,   302,  1404,  1575,  1583,   345,  1585,  1615,   346,  1617,
  1405,   302,  1624,  1632,  1361,   302,  1661,  2062,  1663,  2191,
  1664,  1406,   272,  1665,  1400,   274,   302,  1678,  1683,  1451,
  1745,   347,  2183,   348,  1746,  1774,  1775,   817,  1827,   349,
  1783,   350,  1786,  1789,  1508,  2063,   817,  1792,  1826,  1813,
  1832,  1508,  1795,  2064,  2065,  2066,  2067,  2068,  2069,  2070,
  2071,  1798,  1801,   817,  1804,  2187,  1807,  2188,  1834,  1835,
  2072,  1837,  1836,  1846,  1848,  1873,  1867,  1868,  1874,  1878,
  2228,  1070,  1138,  1875,  1880,  1070,  1883,  1070,  1884,  1887,
  1070,  1070,  1070,  1070,  1070,  1070,  1070,  1070,  1070,  1891,
  1904,  1070,  1909,  1913,   817,  1914,  1946,  1408,  1916,  2022,
  1917,  1919,  1921,  2023,  1922,  2029,  1926,   817,  1930,  1931,
  1138,  1932,  1933,  1934,  1409,  1935,  2000,  1939,  1949,  1950,
  1952,  1953,  1138,  1138,  1138,  1138,  1138,  1138,  1138,   302,
  1955,  1956,  1958,  1959,  1961,  1962,   302,  1964,  1965,  1967,
  1968,  1970,  1410,  1971,  1973,  1974,  1978,  1976,  1988,  1986,
  1411,  1412,  1413,  1414,  1415,  1416,  1417,  1418,  1989,  2031,
   817,  1980,   817,  1984,  2053,  1990,  1991,  2032,  1995,  1999,
  1189,  2001,  2002,  2003,  2004,  2055,  2005,  2006,  2007,  2008,
  2009,  2010,  2011,  2012,  2034,  2013,  2014,  2015,  2016,  1508,
  2017,  1508,  2018,  2020,  2035,  2037,  2043,   302,  2045,  2046,
  2077,  2073,  -916,  2078,    38,  2082,    39,  2187,   279,  2188,
  2083,   279,  2091,   301,  2085,  2090,  2092,  2109,  2110,   316,
  2121,  2317,   301,  2122,  2056,  2124,  1465,  2127,  2128,   302,
  2129,  2057,  2131,   379,   383,  2132,  2133,   388,  2135,  2058,
  2140,   383,   383,  2143,  2146,  2177,   383,   405,   408,  2316,
  2059,  2149,   318,  2179,  2152,  2155,  2053,  2158,  2323,  2324,
  2161,  2232,  2164,  2166,   319,  2168,   320,  2233,  2169,  2173,
  2174,   321,  2227,  2175,  2202,  2235,  2239,  2303,   322,   323,
  2240,  2241,   324,  2257,   302,  2258,   302,  2260,  2261,  2268,
  2318,  2265,  2310,   325,  2269,  2271,  2272,  2273,  2274,  2275,
  2276,   326,  2277,  2278,  -345,  2279,  2280,  2281,  2282,   817,
  2314,  2283,  2284,  2285,  2286,   817,    38,  1541,    39,  2287,
  1465,  2288,  2289,  2290,  2291,   410,  2292,   327,  2320,  -263,
  2247,  2340,  1319,  2358,   291,   328,  2061,   329,  2352,   302,
  2053,  2362,   302,  2363,   330,  2366,  2054,  2367,  2373,  2381,
  2390,  2055,  2417,  2062,   817,  2427,  2429,  2430,  2431,  2434,
  1138,  2436,  1070,  2437,   412,   302,   703,  1272,   302,   817,
   817,  2349,   302,  1577,   941,  1877,   870,  1186,  1568,  1886,
  2388,  2063,  2389,  1570,  2112,  2238,  2359,  2422,  2410,  2064,
  2065,  2066,  2067,  2068,  2069,  2070,  2071,  2425,  1882,  2390,
  2056,  1889,  2406,  2426,   401,  1614,  1613,  2057,   884,   817,
   817,   370,  2343,   736,  1322,  2058,   627,   817,  2407,   727,
  1881,   302,  2342,   409,   493,  2416,  2059,   624,  1532,  2388,
  2385,  2389,  2423,  1619,  2060,  1215,  1499,  1627,  1528,  1465,
  2350,  2028,   640,  1841,   279,  2026,  1138,  1207,  1982,  1812,
  2424,  1671,  2137,  2349,  1941,  1325,  1200,  1514,  1190,   302,
  2376,  2030,   427,   817,   670,   817,   815,  1842,   383,  2391,
   611,  1243,   817,  1210,  1518,  2301,  1245,  1313,   421,  2299,
   301,   301,  1246,   730,   279,   457,  1256,   301,   383,  1809,
  1501,  2298,   476,     0,     0,     0,   383,   301,     0,     0,
   301,     0,   301,   279,   457,     0,     0,     0,     0,   301,
   301,     0,  2061,     0,  2349,   302,     0,     0,  2391,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   383,  2062,
     0,  1465,     0,     0,   279,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   383,
     0,     0,   388,     0,     0,     0,     0,  2063,     0,   383,
     0,     0,     0,     0,     0,  2064,  2065,  2066,  2067,  2068,
  2069,  2070,  2071,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  2197,   666,     0,     0,     0,     0,     0,     0,
     0,     0,   675,     0,     0,     0,     0,     0,   301,     0,
     0,   301,     0,     0,   301,     0,     0,     0,     0,     0,
  1138,     0,  1465,     0,  1465,  1465,  1465,  1465,  1465,   383,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   388,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   301,     0,
     0,  1465,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   301,     0,
     0,   301,   743,     0,     2,     0,     0,     0,     0,     0,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    12,
     0,    13,    14,    15,    16,    17,    18,    19,    20,    21,
     0,     0,  1465,     0,   748,     0,     0,     0,     0,     0,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    12,
     0,    13,    14,    15,    16,    17,    18,    19,    20,    21,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1465,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   876,     0,
   383,     0,   301,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   383,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1465,     0,     0,     0,  1138,     0,     0,     0,
  1465,  1465,     0,     0,  1465,     0,  1465,     0,     0,  1465,
  1465,  1465,  1465,  1465,  1465,  1465,  1465,  1465,     0,  1465,
   279,     0,     0,     0,  1465,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1465,     0,
     0,     0,   301,     0,     0,     0,     0,  1465,  1465,  1465,
  1465,  1465,     0,     0,     0,     0,     0,     0,   959,     0,
     0,     0,     0,     0,   476,     0,     0,     0,     0,     0,
     0,   301,   383,     0,     0,     0,     0,   301,     0,     0,
     0,   301,   301,     0,     0,     0,     0,     0,   992,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,    22,     0,     0,    23,     0,
    24,    25,     0,    26,     0,    27,     0,     0,     0,     0,
    28,     0,     0,     0,  1035,    29,     0,     0,    30,    31,
    32,    33,    34,    35,    36,    22,     0,     0,    23,     0,
     0,    25,     0,    26,     0,    27,     0,     0,  1465,     0,
    28,     0,     0,     0,  1465,     0,  1465,     0,    30,    31,
    32,    33,    34,     0,  1540,     0,     0,     0,  1067,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1114,     0,     0,  1136,  1465,     0,     0,     0,     0,
     0,     0,     0,     0,  1465,  1465,  1465,  1465,  1465,  1465,
  1465,  1465,  1465,  1465,     0,  1196,     0,  1196,   301,     0,
     0,     0,     0,     0,     0,  1204,     0,     0,     0,     0,
     0,     0,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    12,     0,    13,    14,    15,    16,    17,    18,    19,
    20,    21,     0,     0,     0,     0,     0,     0,     0,  1465,
     0,     0,     0,     0,     0,     0,  1465,     0,  1465,   666,
     0,   279,     0,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    12,     0,    13,    14,    15,    16,    17,    18,
    19,    20,    21,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1465,     0,  1465,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1465,     0,     0,     0,  1300,     0,
     0,     0,     0,     0,     0,   383,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1317,     0,     0,     0,
     0,     0,  1317,     0,     0,     0,     0,  1196,     0,     0,
    49,     0,     0,     0,    50,     0,     0,    51,    52,     0,
     0,    53,     0,     0,     0,     0,     0,     0,     0,    54,
    55,     0,     0,     0,     0,  1035,  1035,  1035,  1035,     0,
     0,     0,     0,     0,    56,    57,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1067,     0,     0,     0,  1067,  1067,  1067,  1067,
     0,    58,     0,     0,     0,     0,    59,     0,     0,     0,
     0,     0,     0,     0,     0,     0,    60,     0,     0,     0,
    61,     0,    62,     0,     0,     0,     0,     0,     0,     0,
     0,     0,    63,     0,    64,     0,     0,     0,    65,     0,
    66,     0,    67,     0,     0,     0,    68,    22,     0,     0,
    23,     0,    69,    25,     0,    26,     0,    27,     0,    70,
     0,     0,    28,     0,     0,     0,  1136,  1136,  1136,  1136,
    30,    31,    32,    33,    34,     0,  2353,     0,     0,     0,
     0,     0,     0,     0,  1136,     0,     0,     0,    22,     0,
     0,    23,     0,     0,    25,     0,    26,     0,    27,     0,
     0,     0,     0,    28,    71,     0,    72,   833,     0,    73,
    74,    30,    31,    32,    33,    34,     0,     0,   835,     0,
     0,     0,     0,     0,     0,     0,    75,     0,     0,     0,
   383,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    76,    77,  1513,     0,     0,   876,     0,  1196,     0,    78,
    79,     0,     0,     0,     0,     0,     0,   301,     0,    80,
    81,     0,     0,     0,   301,     0,     0,   836,     0,     0,
     0,    82,    83,    84,   837,    85,     0,     0,    86,     0,
     0,     0,   838,     0,    87,     0,     0,     0,     0,     0,
     0,     0,     0,    88,     0,     0,     0,     0,     0,     0,
    89,  1569,     0,  1569,  1571,  1572,   301,    90,     0,     0,
     0,     0,   383,     0,     0,     0,     0,     0,   959,     0,
     0,     0,   959,     0,     0,     0,     0,     0,     0,     0,
     0,   301,     0,     0,     0,     0,     0,    91,     0,     0,
     0,   476,  1612,  1612,   476,     0,     0,     0,     0,   301,
     0,   301,     0,  1623,     0,     0,     0,     0,   992,     0,
     0,     0,     0,     0,     0,     0,     0,  1634,  1635,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   841,
     0,     0,     0,   276,     0,     0,   276,     0,   276,     0,
     0,  1035,  1035,     0,   276,     0,   842,   276,     0,     0,
     0,  1035,  1035,  1035,  1035,  1035,  1035,  1035,   276,   276,
     0,     0,   276,     0,     0,     0,   276,   276,     0,     0,
     0,   276,   276,   276,   843,     0,     0,  1035,     0,     0,
     0,     0,   844,   845,   846,   847,   848,   849,   850,   851,
  1067,  1136,     0,     0,  1067,     0,  1067,     0,     0,  1067,
  1067,  1067,  1067,  1067,  1067,  1067,  1067,  1067,     0,     0,
  1067,     0,     0,     0,     0,  2053,     0,     0,     0,     0,
     0,  2054,     0,     0,     0,     0,  2055,     0,     0,  1136,
     0,     0,  1035,     0,     0,     0,     0,     0,     0,     0,
     0,  1136,  1136,  1136,  1136,  1136,  1136,  1136,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   833,     0,     0,     0,     0,     0,   834,     0,     0,
     0,     0,   835,     0,     0,  2056,     0,     0,     0,     0,
     0,     0,  2057,     0,     0,     0,     0,     0,   383,     0,
  2058,   383,     0,     0,     0,     0,  1825,     0,     0,  1196,
     0,  2059,     0,     0,     0,     0,     0,     0,   833,  2060,
     0,     0,     0,     0,   834,     0,     0,     0,     0,   835,
     0,   836,     0,     0,     0,     0,     0,     0,   837,     0,
     0,     0,     0,  1844,     0,     0,   838,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   839,     0,     0,
     0,     0,     0,  1866,     0,   840,   992,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   836,   276,
     0,     0,     0,     0,     0,   837,     0,     0,     0,     0,
     0,     0,     0,   838,  1569,     0,  1890,  2061,  1400,     0,
     0,     0,  1905,   276,   839,     0,     0,     0,     0,  1402,
     0,     0,   840,     0,  2062,   276,   276,     0,     0,   276,
   276,     0,   276,   276,     0,     0,     0,   276,     0,     0,
     0,   276,   276,     0,     0,   276,     0,   276,   276,   276,
     0,     0,  2063,   841,   276,   276,     0,     0,     0,     0,
  2064,  2065,  2066,  2067,  2068,  2069,  2070,  2071,  1403,  1400,
   842,     0,     0,   276,     0,  1404,     0,  2321,     0,   276,
  1402,     0,     0,  1405,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   276,     0,     0,   276,   843,  1136,
   841,  1067,     0,     0,   276,     0,   844,   845,   846,   847,
   848,   849,   850,   851,     0,     0,   833,   842,     0,     0,
     0,  -703,   834,     0,     0,     0,     0,   835,   276,  1403,
     0,     0,     0,     0,     0,     0,  1404,   276,     0,     0,
  2053,     0,     0,   276,  1405,   843,   276,     0,     0,   276,
     0,  2055,     0,   844,   845,   846,   847,   848,   849,   850,
   851,     0,     0,     0,   276,     0,     0,     0,  1349,     0,
     0,     0,     0,     0,     0,     0,   836,     0,     0,     0,
  1408,   276,     0,   837,     0,  1136,     0,     0,     0,     0,
     0,   838,     0,   276,     0,     0,     0,  1409,     0,     0,
  2056,     0,   839,     0,     0,     0,     0,  2057,     0,     0,
   840,     0,     0,   276,     0,  2058,   276,   276,     0,     0,
     0,     0,   383,     0,     0,  1410,     0,     0,     0,     0,
     0,     0,     0,  1114,-32768,-32768,  1414,  1415,  1416,  1417,
  1418,  1408,     0,     0,     0,     0,   301,     0,     0,   276,
     0,     0,     0,     0,     0,     0,     0,     0,  1409,     0,
     0,     0,     0,     0,   767,  1866,     0,  1866,  1866,  1866,
  1866,  1866,     0,     0,     0,     0,   992,   992,     0,     0,
   816,     0,   301,     0,     0,     0,  1410,     0,   841,     0,
     0,     0,     0,   383,  1411,  1412,  1413,  1414,  1415,  1416,
  1417,  1418,     0,   301,   457,   842,     0,  2111,     0,     0,
     0,     0,  2061,     0,     0,     0,     0,     0,  2120,     0,
     0,     0,     0,   276,     0,   276,     0,   276,     0,  2062,
     0,     0,     0,   843,     0,     0,     0,   276,     0,     0,
     0,   844,   845,   846,   847,   848,   849,   850,   851,  1136,
     0,     0,     0,     0,     0,     0,  -702,  2063,     0,   456,
     0,     0,     0,     0,     0,  2064,  2065,  2066,  2067,  2068,
  2069,  2070,  2071,     0,     0,   276,     0,     0,   491,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   276,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   276,     0,     0,     0,     0,     0,   276,
     0,     0,     0,     0,     0,     0,   276,   276,     0,     0,
     0,   383,   276,     0,     0,     0,   276,   276,   383,     0,
     0,     0,     0,   276,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1866,     0,     0,     0,     0,
     0,     0,     0,  1866,  1010,     0,     0,  1866,     0,  1866,
     0,     0,  1866,  1866,  1866,  1866,  1866,  1866,  1866,  1866,
  1866,     0,  1866,     0,     0,     0,     0,   457,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   301,
     0,  1866,     0,     0,     0,     0,     0,     0,     0,     0,
   457,   457,   457,   457,   457,     0,     0,     0,     0,    50,
     0,     0,    51,    52,     0,     0,    53,     0,     0,     0,
     0,   992,     0,     0,    54,    55,   276,   816,     0,     0,
     0,     0,     0,     0,     0,  1136,     0,     0,     0,    56,
    57,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   276,     0,   276,   276,     0,     0,     0,     0,     0,     0,
   276,     0,     0,     0,     0,     0,    58,     0,     0,     0,
     0,   411,     0,     0,     0,     0,   383,     0,   383,     0,
     0,    60,     0,     0,     0,    61,     0,    62,     0,     0,
     0,     0,     0,     0,     0,     0,     0,    63,     0,    64,
     0,     0,     0,    65,   276,    66,   276,    67,     0,  1866,
     0,    68,     0,     0,     0,     0,     0,    69,     0,     0,
     0,     0,     0,     0,    70,     0,     0,     0,     0,     0,
     0,   301,     0,  2053,  1196,     0,     0,     0,   457,     0,
     0,     0,     0,     0,  2055,     0,     0,   457,   457,   457,
   457,   457,   457,   457,   457,   457,   457,   992,     0,     0,
  1196,     0,     0,   276,   301,     0,     0,     0,     0,    71,
   276,    72,     0,     0,    73,    74,     0,     0,     0,     0,
     0,   276,     0,     0,     0,     0,     0,   276,     0,     0,
     0,    75,   276,  2056,     0,     0,     0,     0,     0,     0,
  2057,   833,     0,     0,     0,    76,    77,   834,  2058,  1327,
     0,  1866,   835,  2361,    78,    79,     0,   833,     0,     0,
     0,     0,     0,   834,    80,    81,  1221,     0,   835,     0,
     0,     0,     0,     0,     0,     0,    82,    83,    84,     0,
    85,     0,     0,    86,     0,     0,     0,     0,     0,    87,
     0,  2380,     0,     0,     0,     0,  1222,     0,    88,     0,
     0,   836,     0,     0,     0,    89,     0,   457,   837,     0,
     0,     0,    90,     0,     0,     0,   838,   836,     0,     0,
     0,     0,     0,     0,   837,     0,     0,   839,     0,     0,
     0,     0,   838,     0,     0,   840,     0,     0,  1033,     0,
     0,     0,    91,   839,     0,  2061,     0,  2380,     0,     0,
     0,   840,   833,     0,     0,     0,     0,     0,   834,     0,
     0,     0,  2062,   835,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   816,     0,     0,     0,     0,     0,     0,
     0,     0,  1065,     0,     0,     0,     0,     0,     0,  1223,
  2063,     0,     0,     0,     0,     0,     0,     0,  1134,-32768,
-32768,  2067,  2068,  2069,  2070,  2071,     0,     0,     0,     0,
     0,     0,   836,   841,     0,     0,     0,     0,     0,   837,
     0,     0,     0,     0,     0,   276,     0,   838,     0,   841,
   842,     0,     0,     0,     0,     0,     0,   276,   839,     0,
   276,     0,   276,     0,     0,     0,   842,     0,     0,     0,
     0,     0,   276,     0,     0,     0,     0,     0,   843,   276,
     0,     0,     0,     0,     0,     0,   844,   845,   846,   847,
   848,   849,   850,   851,   843,     0,     0,     0,     0,     0,
     0,     0,   844,   845,   846,   847,   848,   849,   850,   851,
     0,     0,     0,     0,     0,     0,   276,     0,   276,   276,
   276,   276,     0,     0,     0,     0,     0,   276,     0,     0,
     0,     0,     0,   276,     0,     0,     0,   276,     0,     0,
     0,     0,     0,     0,   841,     0,   276,     0,     0,     0,
     0,     0,     0,     0,   816,     0,   276,   276,   276,   276,
     0,   842,     0,     0,   276,     0,   276,     0,   276,     0,
     0,     0,     0,   276,     0,     0,     0,     0,     0,     0,
     0,   816,   276,   276,     0,     0,     0,     0,     0,   843,
     0,     0,     0,     0,     0,     0,     0,   844,   845,   846,
   847,   848,   849,   850,   851,     0,     0,     0,     0,  1033,
  1033,  1033,  1033,     0,     0,   816,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1065,     0,     0,  1010,
  1065,  1065,  1065,  1065,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   816,     0,     0,     0,   780,   781,
   782,   783,   784,   785,   786,   787,   788,     0,   789,     0,
   790,   791,   792,   793,   794,   795,   796,   797,   798,   799,
     0,   800,     0,   801,   802,   803,   804,   805,     0,   806,
   807,   808,   809,   810,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   816,     0,     0,     0,     0,
  1134,  1134,  1134,  1134,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   816,     0,     0,     0,  1134,     0,
     0,   547,   548,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   200,   554,
     0,     0,     0,   276,     0,   811,   276,     0,     0,     0,
     0,   276,     0,     0,   276,   559,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,     0,     0,     0,   560,
     0,     0,     0,     0,     0,   833,     0,     0,     0,     0,
     0,   834,   207,   208,     0,   561,   835,   562,   276,     0,
     0,     0,     0,   566,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   276,     0,   816,     0,     0,   218,     0,     0,     0,
     0,     0,     0,   812,   813,     0,     0,     0,     0,  1379,
     0,     0,     0,     0,     0,   836,     0,     0,   574,   276,
     0,   276,   837,     0,     0,   223,     0,   276,   814,     0,
   838,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   839,     0,     0,     0,     0,     0,     0,     0,   840,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1599,     0,   576,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   272,
     0,     0,   274,     0,     0,  1033,  1033,   816,     0,     0,
     0,     0,     0,     0,     0,  1033,  1033,  1033,  1033,  1033,
  1033,  1033,     0,     0,     0,     0,     0,   841,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1033,     0,     0,   842,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1065,  1134,     0,     0,  1065,     0,
  1065,     0,     0,  1065,  1065,  1065,  1065,  1065,  1065,  1065,
  1065,  1065,   843,     0,  1065,     0,     0,     0,     0,     0,
   844,   845,   846,   847,   848,   849,   850,   851,     0,     0,
     0,     0,     0,  1134,     0,     0,  1033,     0,     0,   816,
     0,     0,     0,     0,     0,  1134,  1134,  1134,  1134,  1134,
  1134,  1134,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   276,     0,   780,
   781,   782,   783,   784,   785,   786,   787,   788,   276,   789,
     0,   790,   791,   792,   793,   794,   795,   796,   797,   798,
   799,   276,   800,     0,   801,   802,   803,   804,   805,     0,
   806,   807,   808,   809,   810,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   276,   276,     0,     0,     0,     0,   276,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   276,     0,
     0,     0,   547,   548,     0,     0,     0,  1864,   276,     0,
     0,     0,   276,     0,     0,     0,     0,     0,     0,   200,
   554,   833,     0,   276,     0,     0,   811,   834,     0,     0,
     0,     0,   835,     0,   816,     0,   559,     0,     0,     0,
     0,     0,     0,   816,   202,     0,   833,     0,     0,     0,
   560,     0,   834,     0,  1929,     0,     0,   835,     0,     0,
   816,     0,     0,   207,   208,     0,   561,     0,   562,     0,
     0,     0,     0,     0,   566,     0,     0,     0,     0,     0,
     0,   836,     0,     0,     0,     0,     0,     0,   837,     0,
     0,     0,     0,     0,     0,     0,   838,   218,     0,     0,
     0,   816,     0,     0,   812,   813,   836,   839,     0,     0,
     0,     0,     0,   837,   816,   840,     0,     0,     0,   574,
     0,   838,     0,     0,     0,     0,   223,     0,     0,   814,
     0,     0,   839,  1134,     0,  1065,   276,     0,  2053,  1630,
   840,     0,     0,   276,  2054,     0,  2302,     0,     0,  2055,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   771,     0,     0,   816,     0,   816,
   833,     0,     0,     0,     0,     0,   834,     0,   818,     0,
   821,   835,   822,   823,   827,     0,     0,     0,   576,     0,
     0,     0,     0,   841,     0,     0,     0,     0,  2056,     0,
   272,     0,     0,   274,   276,  2057,     0,     0,     0,     0,
   842,     0,     0,  2058,     0,     0,     0,     0,   841,  1134,
     0,     0,     0,     0,  2059,     0,     0,     0,     0,     0,
   836,     0,  2060,     0,     0,   842,   276,   837,   843,     0,
   891,     0,     0,     0,     0,   838,   844,   845,   846,   847,
   848,   849,   850,   851,     0,     0,   839,     0,     0,     0,
     0,     0,     0,   843,   840,     0,     0,     0,     0,     0,
     0,   844,   845,   846,   847,   848,   849,   850,   851,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   276,     0,   276,     0,     0,     0,     0,     0,  1864,
     0,  1864,  1864,  1864,  1864,  1864,     0,     0,     0,     0,
  2061,     0,     0,     0,     0,     0,   816,     0,     0,     0,
     0,   978,   816,     0,     0,     0,     0,  2062,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  2107,     0,
     0,     0,   841,     0,     0,     0,   276,     0,     0,   276,
   999,     0,  2119,     0,     0,  2063,     0,     0,     0,   842,
     0,   816,     0,  2064,  2065,  2066,  2067,  2068,  2069,  2070,
  2071,     0,   276,  1020,     0,   276,   816,   816,     0,   276,
  1043,     0,     0,  1134,     0,     0,     0,   843,     0,     0,
     0,     0,     0,     0,     0,   844,   845,   846,   847,   848,
   849,   850,   851,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   816,   816,     0,     0,
  1063,     0,     0,     0,   816,     0,     0,     0,   276,     0,
     0,     0,     0,     0,     0,  1117,     0,     0,     0,  1144,
     0,  1148,     0,     0,  1152,  1157,  1161,  1165,  1169,  1173,
  1177,  1181,  1185,     0,     0,     0,     0,     0,     0,     0,
  1034,     0,     0,     0,     0,     0,   276,     0,     0,     0,
   816,     0,   816,     0,     0,     0,     0,     0,     0,   816,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1864,
     0,     0,     0,     0,     0,     0,     0,  1864,     0,     0,
     0,  1864,     0,  1864,  1066,     0,  1864,  1864,  1864,  1864,
  1864,  1864,  1864,  1864,  1864,     0,  1864,     0,     0,     0,
  1135,  2107,   276,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1864,     0,     0,     0,     0,
     0,     0,     0,     0,  2107,  2107,  2107,  2107,  2107,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1400,     0,     0,     0,     0,     0,  1401,     0,     0,
     0,     0,  1402,     0,     0,     0,     0,  1400,     0,  1134,
     0,     0,     0,  1401,     0,     0,     0,     0,  1402,     0,
     0,     0,     0,     0,     0,  1323,     0,   745,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1403,     0,     0,     0,     0,     0,     0,  1404,     0,
     0,     0,     0,     0,     0,     0,  1405,  1403,     0,     0,
  1043,     0,  1043,  1043,  1404,     0,     0,  1406,     0,     0,
     0,     0,  1405,  1864,  1386,  1407,     0,     0,  1391,  1393,
  1395,  1397,   827,  1406,     0,     0,     0,     0,     0,     0,
     0,   872,     0,     0,  1435,     0,     0,     0,     0,     0,
     0,     0,  2107,     0,     0,     0,     0,     0,     0,     0,
     0,  2107,  2107,  2107,  2107,  2107,  2107,  2107,  2107,  2107,
  2107,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1034,  1034,  1034,  1034,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1450,  1408,     0,     0,     0,     0,  1066,     0,
     0,     0,  1066,  1066,  1066,  1066,     0,     0,  1471,  1408,
  1409,     0,   827,     0,     0,  1864,     0,   827,     0,     0,
     0,   827,     0,     0,     0,   827,  1409,     0,  2053,   827,
     0,     0,     0,   827,  2054,     0,     0,   827,  1410,  2055,
     0,   827,     0,     0,     0,   827,  1411,  1412,  1413,  1414,
  1415,  1416,  1417,  1418,  1410,     0,     0,     0,     0,     0,
     0,     0,  1411,  1412,  1413,  1414,  1415,  1416,  1417,  1418,
     0,  2107,  1135,  1135,  1135,  1135,     0,     0,     0,     0,
     0,   891,     0,     0,  1464,     0,     0,  1032,  2056,     0,
  1135,     0,     0,     0,     0,  2057,     0,     0,  2053,     0,
     0,     0,     0,  2058,  2054,     0,     0,     0,     0,  2055,
     0,     0,     0,     0,  2059,     0,     0,     0,     0,     0,
     0,     0,  2060,     0,     0,     0,     0,     0,     0,     0,
     0,  1064,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1133,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  2056,     0,
     0,     0,     0,     0,     0,  2057,     0,     0,     0,     0,
     0,     0,     0,  2058,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2059,     0,     0,     0,     0,     0,
     0,     0,  1636,     0,     0,     0,     0,     0,     0,     0,
  2061,  1639,     0,     0,     0,     0,     0,     0,     0,     0,
  1640,     0,     0,     0,     0,     0,  1043,  2062,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1667,     0,     0,  2063,     0,     0,     0,     0,
     0,  1043,     0,  2064,  2065,  2066,  2067,  2068,  2069,  2070,
  2071,     0,  1680,     0,     0,     0,  1688,     0,  1693,     0,
  2061,  1698,  1703,  1708,  1713,  1718,  1723,  1728,  1733,  1738,
     0,     0,  1063,     0,     0,     0,     0,  2062,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1034,  1034,     0,
     0,     0,     0,     0,     0,     0,  1043,  1034,  1034,  1034,
  1034,  1034,  1034,  1034,     0,  2063,     0,     0,     0,     0,
     0,     0,     0,  2064,  2065,  2066,  2067,  2068,  2069,  2070,
  2071,     0,     0,  1034,     0,     0,     0,     0,  1032,  1032,
  1032,  1032,     0,     0,     0,     0,  1066,  1135,     0,     0,
  1066,     0,  1066,     0,     0,  1066,  1066,  1066,  1066,  1066,
  1066,  1066,  1066,  1066,     0,  1064,  1066,     0,     0,  1064,
  1064,  1064,  1064,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1135,     0,     0,  1034,     0,
     0,     0,     0,     0,     0,     0,     0,  1135,  1135,  1135,
  1135,  1135,  1135,  1135,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1780,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1133,
  1133,  1133,  1133,     0,     0,     0,     0,     0,     0,     0,
     0,  1007,     0,     0,     0,     0,     0,  1133,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1043,  1043,  1043,  1865,
     0,     0,     0,     0,     0,     0,     0,     0,   872,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1043,  1043,  1043,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1947,     0,     0,     0,     0,   827,     0,
     0,     0,     0,   827,     0,     0,     0,     0,   827,     0,
     0,     0,     0,   827,     0,     0,     0,     0,   827,     0,
     0,     0,     0,   827,     0,     0,     0,     0,   827,     0,
     0,     0,     0,   827,     0,     0,     0,     0,   827,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1983,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1043,  1043,  1043,     0,     0,     0,     0,     0,  1464,     0,
     0,     0,     0,     0,     0,  1135,     0,  1066,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1032,  1032,     0,     0,     0,     0,
     0,     0,     0,     0,  1032,  1032,  1032,  1032,  1032,  1032,
  1032,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1032,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1064,  1133,     0,     0,  1064,     0,  1064,
  1997,  1135,  1064,  1064,  1064,  1064,  1064,  1064,  1064,  1064,
  1064,     0,     0,  1064,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1133,     0,     0,  1032,     0,     0,     0,     0,
     0,     0,     0,     0,  1133,  1133,  1133,  1133,  1133,  1133,
  1133,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1865,     0,  1865,  1865,  1865,  1865,  1865,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1461,     0,     0,     0,     0,     0,     0,     0,     0,
  2108,     0,     0,  1474,     0,     0,     0,     0,  1477,     0,
     0,     0,  1480,     0,     0,     0,  1483,     0,     0,     0,
  1486,     0,     0,     0,  1489,     0,     0,     0,  1492,     0,
     0,     0,  1495,     0,     0,     0,  1498,     0,     0,     0,
     0,     0,     0,     0,     0,  1135,     0,     0,     0,     0,
     0,  1780,     0,  2182,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  2176,     0,     0,   780,   781,
   782,   783,   784,   785,   786,   787,   788,     0,   789,     0,
   790,   791,   792,   793,   794,   795,   796,   797,   798,   799,
     0,   800,     0,   801,   802,   803,   804,   805,     0,   806,
   807,   808,   809,   810,     0,     0,     0,     0,     0,     0,
     0,  1865,     0,  1601,     0,     0,     0,     0,     0,  1865,
  2201,     0,  1133,  1865,  1064,  1865,     0,     0,  1865,  1865,
  1865,  1865,  1865,  1865,  1865,  1865,  1865,     0,  1865,     0,
     0,   547,   548,  2108,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1865,   200,   554,
     0,     0,     0,     0,     0,   811,  2108,  2108,  2108,  2108,
  2108,     0,     0,   558,     0,   559,     0,     0,     0,     0,
     0,     0,     0,   202,     0,     0,     0,     0,     0,   560,
     0,     0,     0,     0,     0,     0,  1670,     0,     0,     0,
     0,  1135,   207,   208,     0,   561,     0,   562,  1133,     0,
     0,     0,     0,   566,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   218,    73,     0,     0,
     0,     0,     0,   812,   813,     0,     0,     0,     0,     0,
     0,   572,     0,     0,     0,     0,     0,     0,   574,     0,
     0,     0,     0,     0,     0,   223,     0,  2201,   814,     0,
     0,     0,     0,  2201,     0,  1865,  2351,     0,     0,     0,
     0,     0,  1777,     0,  1781,  1782,     0,  1784,  1785,     0,
  1787,  1788,     0,  1790,  1791,     0,  1793,  1794,     0,  1796,
  1797,     0,  1799,  1800,  2108,  1802,  1803,     0,  1805,  1806,
     0,     0,     0,  2108,  2108,  2108,  2108,  2108,  2108,  2108,
  2108,  2108,  2108,     0,     0,     0,     0,   576,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   272,
   273,     0,   274,     0,     0,    25,   578,    26,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  2201,     0,
     0,     0,  1133,     0,     0,  2201,     0,  1865,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  2201,     0,  2201,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  2108,     0,     0,   167,   168,   169,   170,
   171,   172,   173,   174,   175,     0,   176,     0,   177,   178,
   179,   180,   181,   182,   183,   184,   185,   186,     0,   187,
     0,   188,   189,   190,   191,   192,     0,   193,   194,   195,
   196,   197,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,  1461,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1951,
     0,     0,     0,     0,  1954,     0,     0,     0,     0,  1957,
   380,     0,     0,     0,  1960,     0,   200,     0,     0,  1963,
     0,     0,     0,   201,  1966,     0,     0,     0,     0,  1969,
     0,     0,     0,     0,  1972,     0,     0,     0,     0,  1975,
     0,   202,     0,     0,   203,     0,     0,  1979,     0,     0,
     0,  1981,   204,   205,     0,     0,     0,     0,  1133,   206,
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
   274,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1502,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1777,     0,
     0,  2138,  2139,     0,  2141,  2142,     0,  2144,  2145,     0,
  2147,  2148,     0,  2150,  2151,     0,  2153,  2154,     0,  2156,
  2157,     0,  2159,  2160,     0,  2162,  2163,     0,     0,     0,
     0,     0,  2167,     0,     0,     0,  2170,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,     0,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   545,   546,
   547,   548,     0,     0,   549,     0,     0,     0,     0,     0,
     0,   380,   550,   551,   552,   553,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
   556,   557,   558,     0,   559,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,   563,
   564,   565,   566,   210,     0,   211,   212,     0,     0,     0,
     0,   567,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,   568,     0,
     0,     0,   569,   570,   221,   222,     0,     0,     0,   571,
   572,     0,     0,     0,   573,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   577,   274,   275,     0,    25,   578,    26,     0,     0,     0,
     0,     0,   579,  1197,     0,     0,   581,     0,   582,     0,
     0,     0,     0,     0,   583,  1198,   514,   515,   516,   517,
   518,   519,   520,   521,   522,     0,   523,     0,   524,   525,
   526,   527,   528,   529,   530,   531,   532,   533,     0,   534,
     0,   535,   536,   537,   538,   539,     0,   540,   541,   542,
   543,   544,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   198,   199,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   545,   546,   547,
   548,     0,     0,   549,     0,     0,     0,     0,     0,     0,
   380,   550,   551,   552,   553,     0,   200,   554,     0,     0,
     0,     0,     0,   555,     0,     0,     0,     0,     0,   556,
   557,   558,     0,   559,     0,     0,     0,     0,     0,     0,
     0,   202,     0,     0,   203,     0,     0,   560,     0,     0,
     0,     0,   204,   205,     0,     0,     0,     0,     0,   206,
   207,   208,     0,   561,     0,   562,   209,     0,   563,   564,
   565,   566,   210,     0,   211,   212,     0,     0,     0,     0,
   567,     0,     0,   213,   214,     0,     0,   215,     0,   216,
     0,     0,     0,   217,   218,     0,     0,   568,     0,     0,
     0,   569,   570,   221,   222,     0,     0,     0,   571,   572,
     0,     0,     0,   573,     0,     0,   574,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   575,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,   381,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,   576,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,   577,
   274,   275,     0,    25,   578,    26,     0,     0,     0,     0,
     0,   579,  1740,     0,     0,   581,     0,   582,     0,     0,
     0,     0,     0,   583,  1741,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   545,   546,   547,   548,
     0,     0,   549,     0,     0,     0,     0,     0,     0,   380,
   550,   551,   552,   553,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,   556,   557,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,   563,   564,   565,
   566,   210,     0,   211,   212,     0,     0,     0,     0,   567,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,   568,     0,     0,     0,
   569,   570,   221,   222,     0,     0,     0,   571,   572,     0,
     0,     0,   573,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,   577,   274,
   275,     0,    25,   578,    26,     0,     0,     0,     0,     0,
   579,     0,     0,     0,   581,     0,   582,     0,     0,     0,
     0,     0,   583,  1658,   514,   515,   516,   517,   518,   519,
   520,   521,   522,     0,   523,     0,   524,   525,   526,   527,
   528,   529,   530,   531,   532,   533,     0,   534,     0,   535,
   536,   537,   538,   539,     0,   540,   541,   542,   543,   544,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   545,   546,   547,   548,     0,
     0,   549,     0,     0,     0,     0,     0,     0,   380,   550,
   551,   552,   553,     0,   200,   554,     0,     0,     0,     0,
     0,   555,     0,     0,     0,     0,     0,   556,   557,   558,
     0,   559,     0,     0,     0,     0,     0,     0,     0,   202,
     0,     0,   203,     0,     0,   560,     0,     0,     0,     0,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,   561,     0,   562,   209,     0,   563,   564,   565,   566,
   210,     0,   211,   212,     0,     0,     0,     0,   567,     0,
     0,   213,   214,     0,     0,   215,     0,   216,     0,     0,
     0,   217,   218,     0,     0,   568,     0,     0,     0,   569,
   570,   221,   222,     0,     0,     0,   571,   572,     0,     0,
     0,   573,     0,     0,   574,     0,     0,     0,     0,     0,
     0,   223,   224,   225,   575,     0,   227,   228,     0,   229,
   230,     0,   231,     0,     0,   232,   233,   234,   235,   236,
     0,   237,   238,     0,     0,   239,   240,   241,   242,   243,
   244,   245,   246,   247,     0,     0,     0,     0,   248,     0,
   249,   250,     0,   381,   251,   252,     0,   253,     0,   254,
     0,   255,   256,   257,   258,     0,   259,     0,   260,   261,
   262,   263,   264,   576,     0,   265,   266,   267,   268,   269,
     0,     0,   270,     0,   271,   272,   273,   577,   274,   275,
     0,    25,   578,    26,     0,     0,     0,     0,     0,   579,
     0,     0,     0,   581,     0,   582,     0,     0,     0,     0,
     0,   583,  1772,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  2094,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  2095,  2096,
  2097,  2098,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,     0,     0,   558,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,     0,     0,     0,   566,   210,
     0,   211,   212,     0,     0,     0,     0,     0,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,     0,     0,     0,     0,   569,   570,
   221,   222,     0,     0,     0,     0,   572,     0,     0,     0,
  2100,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,     0,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,  2101,   274,     0,     0,
    25,   578,    26,     0,     0,     0,     0,     0,  2102,     0,
     0,     0,  2103,     0,  2104,     0,     0,     0,     0,     0,
  2105,  2336,   514,   515,   516,   517,   518,   519,   520,   521,
   522,     0,   523,     0,   524,   525,   526,   527,   528,   529,
   530,   531,   532,   533,     0,   534,     0,   535,   536,   537,
   538,   539,     0,   540,   541,   542,   543,   544,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  1855,   547,   548,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   200,   554,     0,     0,     0,     0,     0,   555,
     0,     0,     0,     0,     0,     0,     0,   558,     0,   559,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,   560,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,   561,
     0,   562,   209,     0,  1856,     0,  1857,   566,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   569,   570,   221,
   222,     0,     0,     0,     0,   572,     0,     0,     0,     0,
     0,     0,   574,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   575,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,   576,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,   273,  1858,   274,     0,     0,    25,
   578,    26,     0,     0,     0,     0,     0,  1859,     0,     0,
     0,  1860,     0,  1861,     0,     0,     0,     0,     0,  1862,
  2221,   167,   168,   169,   170,   171,   172,   173,   174,   175,
     0,   176,     0,   177,   178,   179,   180,   181,   182,   183,
   184,   185,   186,     0,   187,     0,   188,   189,   190,   191,
   192,     0,   193,   194,   195,   196,   197,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   547,   548,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   200,   950,     0,     0,     0,     0,     0,   951,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   952,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,     0,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   561,     0,
   562,   209,     0,     0,     0,     0,   953,   210,     0,   211,
   212,     0,     0,     0,     0,     0,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,     0,     0,     0,     0,   219,   220,   221,   222,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   574,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   226,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
     0,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
     0,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,     0,     0,   274,     0,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,   954,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1039,     0,     0,   545,   546,
   547,   548,     0,     0,   549,     0,     0,     0,     0,     0,
     0,   380,   550,   551,   552,   553,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
   556,   557,   558,     0,   559,     0,     0,  1040,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,  1041,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,   563,
   564,   565,   566,   210,     0,   211,   212,     0,     0,     0,
     0,   567,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,   568,     0,
     0,     0,   569,   570,   221,   222,     0,  1042,     0,   571,
   572,     0,     0,     0,   573,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   577,   274,   275,     0,    25,   578,    26,     0,     0,     0,
     0,     0,   579,     0,     0,     0,   581,     0,   582,     0,
     0,     0,     0,     0,   583,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1645,     0,     0,   545,   546,   547,   548,
     0,     0,   549,     0,     0,     0,     0,     0,     0,   380,
   550,   551,   552,   553,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,   556,   557,
   558,     0,   559,     0,     0,  1040,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,  1646,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,   563,   564,   565,
   566,   210,     0,   211,   212,     0,     0,     0,     0,   567,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,   568,     0,     0,     0,
   569,   570,   221,   222,     0,  1647,     0,   571,   572,     0,
     0,     0,   573,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,   577,   274,
   275,     0,    25,   578,    26,     0,     0,     0,     0,     0,
   579,     0,     0,     0,   581,     0,   582,     0,     0,     0,
     0,     0,   583,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1674,     0,     0,   545,   546,   547,   548,     0,     0,
   549,     0,     0,     0,     0,     0,     0,   380,   550,   551,
   552,   553,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,   556,   557,   558,     0,
   559,     0,     0,  1040,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,  1675,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,   563,   564,   565,   566,   210,
     0,   211,   212,     0,     0,     0,     0,   567,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,   568,     0,     0,     0,   569,   570,
   221,   222,     0,  1676,     0,   571,   572,     0,     0,     0,
   573,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,   577,   274,   275,     0,
    25,   578,    26,     0,     0,     0,     0,     0,   579,     0,
     0,     0,   581,     0,   582,     0,     0,     0,     0,     0,
   583,   514,   515,   516,   517,   518,   519,   520,   521,   522,
     0,   523,     0,   524,   525,   526,   527,   528,   529,   530,
   531,   532,   533,     0,   534,     0,   535,   536,   537,   538,
   539,     0,   540,   541,   542,   543,   544,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1759,
     0,     0,   545,   546,   547,   548,     0,     0,   549,     0,
     0,     0,     0,     0,     0,   380,   550,   551,   552,   553,
     0,   200,   554,     0,     0,     0,     0,     0,   555,     0,
     0,     0,     0,     0,   556,   557,   558,     0,   559,     0,
     0,  1040,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   560,     0,     0,     0,     0,   204,   205,  1760,
     0,     0,     0,     0,   206,   207,   208,     0,   561,     0,
   562,   209,     0,   563,   564,   565,   566,   210,     0,   211,
   212,     0,     0,     0,     0,   567,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,   568,     0,     0,     0,   569,   570,   221,   222,
     0,  1761,     0,   571,   572,     0,     0,     0,   573,     0,
     0,   574,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   575,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   576,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,   577,   274,   275,     0,    25,   578,
    26,     0,     0,     0,     0,     0,   579,     0,     0,     0,
   581,     0,   582,     0,     0,     0,     0,     0,   583,   514,
   515,   516,   517,   518,   519,   520,   521,   522,     0,   523,
     0,   524,   525,   526,   527,   528,   529,   530,   531,   532,
   533,     0,   534,     0,   535,   536,   537,   538,   539,     0,
   540,   541,   542,   543,   544,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,  1153,
     0,     0,  1154,     0,     0,     0,     0,     0,     0,     0,
   545,   546,   547,   548,     0,     0,   549,     0,     0,     0,
     0,     0,     0,   380,   550,   551,   552,   553,     0,   200,
   554,     0,     0,     0,     0,     0,   555,     0,     0,     0,
     0,     0,   556,   557,   558,     0,   559,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   560,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   561,     0,   562,   209,
     0,   563,   564,  1155,   566,   210,     0,   211,   212,     0,
     0,     0,     0,   567,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
   568,     0,     0,     0,   569,   570,   221,   222,     0,     0,
     0,   571,   572,     0,     0,     0,   573,     0,     0,   574,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   575,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   576,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,   577,   274,   275,     0,    25,   578,    26,     0,
     0,     0,     0,     0,   579,     0,     0,     0,   581,     0,
   582,     0,     0,     0,     0,     0,  1156,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,     0,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,  1700,     0,     0,
  1701,     0,     0,     0,     0,     0,     0,     0,   545,  1046,
   547,   548,     0,     0,   549,     0,     0,     0,     0,     0,
     0,   380,  1047,  1048,  1049,  1050,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
  1051,  1052,   558,     0,   559,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,  1053,
   564,  1155,   566,   210,     0,   211,   212,     0,     0,     0,
     0,  1054,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,  1055,     0,
     0,     0,   569,   570,   221,   222,     0,     0,     0,  1056,
   572,     0,     0,     0,   573,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
  1057,   274,   275,     0,    25,   578,    26,     0,     0,     0,
     0,     0,  1058,     0,     0,     0,  1059,     0,  1060,     0,
     0,     0,     0,     0,  1702,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   545,   546,   547,   548,
     0,     0,   549,     0,     0,     0,     0,     0,     0,   380,
   550,   551,   552,   553,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,   556,   557,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,   563,   564,   565,
   566,   210,     0,   211,   212,     0,     0,     0,     0,   567,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,   568,     0,     0,     0,
   569,   570,   221,   222,     0,     0,     0,   571,   572,     0,
     0,     0,   573,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,   577,   274,
   275,     0,    25,   578,    26,     0,     0,     0,     0,     0,
   579,   580,     0,     0,   581,     0,   582,     0,     0,     0,
     0,     0,   583,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,  1158,     0,     0,  1159,     0,     0,     0,
     0,     0,     0,     0,   545,   546,   547,   548,     0,     0,
   549,     0,     0,     0,     0,     0,     0,   380,   550,   551,
   552,   553,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,   556,   557,   558,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,   563,   564,     0,   566,   210,
     0,   211,   212,     0,     0,     0,     0,   567,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,   568,     0,     0,     0,   569,   570,
   221,   222,     0,     0,     0,   571,   572,     0,     0,     0,
   573,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,   577,   274,   275,     0,
    25,   578,    26,     0,     0,     0,     0,     0,   579,     0,
     0,     0,   581,     0,   582,     0,     0,     0,     0,     0,
  1160,   514,   515,   516,   517,   518,   519,   520,   521,   522,
     0,   523,     0,   524,   525,   526,   527,   528,   529,   530,
   531,   532,   533,     0,   534,     0,   535,   536,   537,   538,
   539,     0,   540,   541,   542,   543,   544,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1162,     0,     0,  1163,     0,     0,     0,     0,     0,
     0,     0,   545,   546,   547,   548,     0,     0,   549,     0,
     0,     0,     0,     0,     0,   380,   550,   551,   552,   553,
     0,   200,   554,     0,     0,     0,     0,     0,   555,     0,
     0,     0,     0,     0,   556,   557,   558,     0,   559,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   560,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   561,     0,
   562,   209,     0,   563,   564,     0,   566,   210,     0,   211,
   212,     0,     0,     0,     0,   567,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,   568,     0,     0,     0,   569,   570,   221,   222,
     0,     0,     0,   571,   572,     0,     0,     0,   573,     0,
     0,   574,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   575,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   576,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,   577,   274,   275,     0,    25,   578,
    26,     0,     0,     0,     0,     0,   579,     0,     0,     0,
   581,     0,   582,     0,     0,     0,     0,     0,  1164,   514,
   515,   516,   517,   518,   519,   520,   521,   522,     0,   523,
     0,   524,   525,   526,   527,   528,   529,   530,   531,   532,
   533,     0,   534,     0,   535,   536,   537,   538,   539,     0,
   540,   541,   542,   543,   544,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,  1166,
     0,     0,  1167,     0,     0,     0,     0,     0,     0,     0,
   545,   546,   547,   548,     0,     0,   549,     0,     0,     0,
     0,     0,     0,   380,   550,   551,   552,   553,     0,   200,
   554,     0,     0,     0,     0,     0,   555,     0,     0,     0,
     0,     0,   556,   557,   558,     0,   559,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   560,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   561,     0,   562,   209,
     0,   563,   564,     0,   566,   210,     0,   211,   212,     0,
     0,     0,     0,   567,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
   568,     0,     0,     0,   569,   570,   221,   222,     0,     0,
     0,   571,   572,     0,     0,     0,   573,     0,     0,   574,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   575,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   576,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,   577,   274,   275,     0,    25,   578,    26,     0,
     0,     0,     0,     0,   579,     0,     0,     0,   581,     0,
   582,     0,     0,     0,     0,     0,  1168,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,     0,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,  1170,     0,     0,
  1171,     0,     0,     0,     0,     0,     0,     0,   545,   546,
   547,   548,     0,     0,   549,     0,     0,     0,     0,     0,
     0,   380,   550,   551,   552,   553,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
   556,   557,   558,     0,   559,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,   563,
   564,     0,   566,   210,     0,   211,   212,     0,     0,     0,
     0,   567,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,   568,     0,
     0,     0,   569,   570,   221,   222,     0,     0,     0,   571,
   572,     0,     0,     0,   573,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   577,   274,   275,     0,    25,   578,    26,     0,     0,     0,
     0,     0,   579,     0,     0,     0,   581,     0,   582,     0,
     0,     0,     0,     0,  1172,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,  1174,     0,     0,  1175,     0,
     0,     0,     0,     0,     0,     0,   545,   546,   547,   548,
     0,     0,   549,     0,     0,     0,     0,     0,     0,   380,
   550,   551,   552,   553,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,   556,   557,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,   563,   564,     0,
   566,   210,     0,   211,   212,     0,     0,     0,     0,   567,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,   568,     0,     0,     0,
   569,   570,   221,   222,     0,     0,     0,   571,   572,     0,
     0,     0,   573,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,   577,   274,
   275,     0,    25,   578,    26,     0,     0,     0,     0,     0,
   579,     0,     0,     0,   581,     0,   582,     0,     0,     0,
     0,     0,  1176,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,  1178,     0,     0,  1179,     0,     0,     0,
     0,     0,     0,     0,   545,   546,   547,   548,     0,     0,
   549,     0,     0,     0,     0,     0,     0,   380,   550,   551,
   552,   553,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,   556,   557,   558,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,   563,   564,     0,   566,   210,
     0,   211,   212,     0,     0,     0,     0,   567,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,   568,     0,     0,     0,   569,   570,
   221,   222,     0,     0,     0,   571,   572,     0,     0,     0,
   573,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,   577,   274,   275,     0,
    25,   578,    26,     0,     0,     0,     0,     0,   579,     0,
     0,     0,   581,     0,   582,     0,     0,     0,     0,     0,
  1180,   514,   515,   516,   517,   518,   519,   520,   521,   522,
     0,   523,     0,   524,   525,   526,   527,   528,   529,   530,
   531,   532,   533,     0,   534,     0,   535,   536,   537,   538,
   539,     0,   540,   541,   542,   543,   544,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1182,     0,     0,  1183,     0,     0,     0,     0,     0,
     0,     0,   545,   546,   547,   548,     0,     0,   549,     0,
     0,     0,     0,     0,     0,   380,   550,   551,   552,   553,
     0,   200,   554,     0,     0,     0,     0,     0,   555,     0,
     0,     0,     0,     0,   556,   557,   558,     0,   559,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   560,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   561,     0,
   562,   209,     0,   563,   564,     0,   566,   210,     0,   211,
   212,     0,     0,     0,     0,   567,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,   568,     0,     0,     0,   569,   570,   221,   222,
     0,     0,     0,   571,   572,     0,     0,     0,   573,     0,
     0,   574,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   575,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   576,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,   577,   274,   275,     0,    25,   578,
    26,     0,     0,     0,     0,     0,   579,     0,     0,     0,
   581,     0,   582,     0,     0,     0,     0,     0,  1184,   514,
   515,   516,   517,   518,   519,   520,   521,   522,     0,   523,
     0,   524,   525,   526,   527,   528,   529,   530,   531,   532,
   533,     0,   534,     0,   535,   536,   537,   538,   539,     0,
   540,   541,   542,   543,   544,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   545,   546,   547,   548,     0,     0,   549,     0,     0,     0,
     0,     0,     0,   380,   550,   551,   552,   553,     0,   200,
   554,     0,     0,     0,     0,     0,   555,     0,     0,     0,
     0,     0,   556,   557,   558,     0,   559,     0,     0,  1040,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   560,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   561,     0,   562,   209,
     0,   563,   564,   565,   566,   210,     0,   211,   212,     0,
     0,     0,     0,   567,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
   568,     0,     0,     0,   569,   570,   221,   222,     0,     0,
     0,   571,   572,     0,     0,     0,   573,     0,     0,   574,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   575,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   576,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,   577,   274,   275,     0,    25,   578,    26,     0,
     0,     0,     0,     0,   579,     0,     0,     0,   581,     0,
   582,     0,     0,     0,     0,     0,   583,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,     0,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   545,   546,
   547,   548,     0,     0,   549,     0,     0,     0,     0,     0,
     0,   380,   550,   551,   552,   553,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
   556,   557,   558,     0,   559,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,   563,
   564,   565,   566,   210,     0,   211,   212,     0,     0,     0,
     0,   567,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,    73,     0,   568,     0,
     0,     0,   569,   570,   221,   222,     0,     0,     0,   571,
   572,     0,     0,     0,   573,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   577,   274,   275,     0,    25,   578,    26,     0,     0,     0,
     0,     0,   579,     0,     0,     0,   581,     0,   582,     0,
     0,     0,     0,     0,   583,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,  1695,     0,     0,  1696,     0,
     0,     0,     0,     0,     0,     0,   545,  1046,   547,   548,
     0,     0,   549,     0,     0,     0,     0,     0,     0,   380,
  1047,  1048,  1049,  1050,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,  1051,  1052,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,     0,   564,     0,
   566,   210,     0,   211,   212,     0,     0,     0,     0,  1054,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,  1055,     0,     0,     0,
   569,   570,   221,   222,     0,     0,     0,  1056,   572,     0,
     0,     0,   573,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,     0,   274,
   275,     0,    25,   578,    26,     0,     0,     0,     0,     0,
  1058,     0,     0,     0,  1059, -1249,  1060,     0,     0,     0,
 -1249,     0,  1697,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,  1705,     0,     0,  1706,     0,     0,     0,
     0,     0,     0,     0,   545,  1046,   547,   548,     0,     0,
   549,     0,     0,     0,     0,     0,     0,   380,  1047,  1048,
  1049,  1050,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,  1051,  1052,   558,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,  1053,   564,     0,   566,   210,
     0,   211,   212,     0,     0,     0,     0,  1054,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,  1055,     0,     0,     0,   569,   570,
   221,   222,     0,     0,     0,  1056,   572,     0,     0,     0,
   573,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,  1057,   274,   275,     0,
    25,   578,    26,     0,     0,     0,     0,     0,  1058,     0,
     0,     0,  1059,     0,  1060,     0,     0,     0,     0,     0,
  1707,   514,   515,   516,   517,   518,   519,   520,   521,   522,
     0,   523,     0,   524,   525,   526,   527,   528,   529,   530,
   531,   532,   533,     0,   534,     0,   535,   536,   537,   538,
   539,     0,   540,   541,   542,   543,   544,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1710,     0,     0,  1711,     0,     0,     0,     0,     0,
     0,     0,   545,  1046,   547,   548,     0,     0,   549,     0,
     0,     0,     0,     0,     0,   380,  1047,  1048,  1049,  1050,
     0,   200,   554,     0,     0,     0,     0,     0,   555,     0,
     0,     0,     0,     0,  1051,  1052,   558,     0,   559,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   560,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   561,     0,
   562,   209,     0,  1053,   564,     0,   566,   210,     0,   211,
   212,     0,     0,     0,     0,  1054,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,  1055,     0,     0,     0,   569,   570,   221,   222,
     0,     0,     0,  1056,   572,     0,     0,     0,   573,     0,
     0,   574,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   575,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   576,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,  1057,   274,   275,     0,    25,   578,
    26,     0,     0,     0,     0,     0,  1058,     0,     0,     0,
  1059,     0,  1060,     0,     0,     0,     0,     0,  1712,   514,
   515,   516,   517,   518,   519,   520,   521,   522,     0,   523,
     0,   524,   525,   526,   527,   528,   529,   530,   531,   532,
   533,     0,   534,     0,   535,   536,   537,   538,   539,     0,
   540,   541,   542,   543,   544,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,  1715,
     0,     0,  1716,     0,     0,     0,     0,     0,     0,     0,
   545,  1046,   547,   548,     0,     0,   549,     0,     0,     0,
     0,     0,     0,   380,  1047,  1048,  1049,  1050,     0,   200,
   554,     0,     0,     0,     0,     0,   555,     0,     0,     0,
     0,     0,  1051,  1052,   558,     0,   559,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   560,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   561,     0,   562,   209,
     0,  1053,   564,     0,   566,   210,     0,   211,   212,     0,
     0,     0,     0,  1054,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
  1055,     0,     0,     0,   569,   570,   221,   222,     0,     0,
     0,  1056,   572,     0,     0,     0,   573,     0,     0,   574,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   575,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   576,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,  1057,   274,   275,     0,    25,   578,    26,     0,
     0,     0,     0,     0,  1058,     0,     0,     0,  1059,     0,
  1060,     0,     0,     0,     0,     0,  1717,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,     0,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,  1720,     0,     0,
  1721,     0,     0,     0,     0,     0,     0,     0,   545,  1046,
   547,   548,     0,     0,   549,     0,     0,     0,     0,     0,
     0,   380,  1047,  1048,  1049,  1050,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
  1051,  1052,   558,     0,   559,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,  1053,
   564,     0,   566,   210,     0,   211,   212,     0,     0,     0,
     0,  1054,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,  1055,     0,
     0,     0,   569,   570,   221,   222,     0,     0,     0,  1056,
   572,     0,     0,     0,   573,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
  1057,   274,   275,     0,    25,   578,    26,     0,     0,     0,
     0,     0,  1058,     0,     0,     0,  1059,     0,  1060,     0,
     0,     0,     0,     0,  1722,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,  1725,     0,     0,  1726,     0,
     0,     0,     0,     0,     0,     0,   545,  1046,   547,   548,
     0,     0,   549,     0,     0,     0,     0,     0,     0,   380,
  1047,  1048,  1049,  1050,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,  1051,  1052,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,  1053,   564,     0,
   566,   210,     0,   211,   212,     0,     0,     0,     0,  1054,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,  1055,     0,     0,     0,
   569,   570,   221,   222,     0,     0,     0,  1056,   572,     0,
     0,     0,   573,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,  1057,   274,
   275,     0,    25,   578,    26,     0,     0,     0,     0,     0,
  1058,     0,     0,     0,  1059,     0,  1060,     0,     0,     0,
     0,     0,  1727,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,  1730,     0,     0,  1731,     0,     0,     0,
     0,     0,     0,     0,   545,  1046,   547,   548,     0,     0,
   549,     0,     0,     0,     0,     0,     0,   380,  1047,  1048,
  1049,  1050,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,  1051,  1052,   558,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,  1053,   564,     0,   566,   210,
     0,   211,   212,     0,     0,     0,     0,  1054,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,  1055,     0,     0,     0,   569,   570,
   221,   222,     0,     0,     0,  1056,   572,     0,     0,     0,
   573,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,  1057,   274,   275,     0,
    25,   578,    26,     0,     0,     0,     0,     0,  1058,     0,
     0,     0,  1059,     0,  1060,     0,     0,     0,     0,     0,
  1732,   514,   515,   516,   517,   518,   519,   520,   521,   522,
     0,   523,     0,   524,   525,   526,   527,   528,   529,   530,
   531,   532,   533,     0,   534,     0,   535,   536,   537,   538,
   539,     0,   540,   541,   542,   543,   544,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1735,     0,     0,  1736,     0,     0,     0,     0,     0,
     0,     0,   545,  1046,   547,   548,     0,     0,   549,     0,
     0,     0,     0,     0,     0,   380,  1047,  1048,  1049,  1050,
     0,   200,   554,     0,     0,     0,     0,     0,   555,     0,
     0,     0,     0,     0,  1051,  1052,   558,     0,   559,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   560,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   561,     0,
   562,   209,     0,  1053,   564,     0,   566,   210,     0,   211,
   212,     0,     0,     0,     0,  1054,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,  1055,     0,     0,     0,   569,   570,   221,   222,
     0,     0,     0,  1056,   572,     0,     0,     0,   573,     0,
     0,   574,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   575,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   576,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,  1057,   274,   275,     0,    25,   578,
    26,     0,     0,     0,     0,     0,  1058,     0,     0,     0,
  1059,     0,  1060,     0,     0,     0,     0,     0,  1737,   514,
   515,   516,   517,   518,   519,   520,   521,   522,     0,   523,
     0,   524,   525,   526,   527,   528,   529,   530,   531,   532,
   533,     0,   534,     0,   535,   536,   537,   538,   539,     0,
   540,   541,   542,   543,   544,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   545,   546,   547,   548,     0,     0,   549,     0,     0,     0,
     0,     0,     0,   380,   550,   551,   552,   553,     0,   200,
   554,     0,     0,     0,     0,     0,   555,     0,     0,     0,
     0,     0,   556,   557,   558,     0,   559,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   560,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   561,     0,   562,   209,
     0,   563,   564,   565,   566,   210,     0,   211,   212,     0,
     0,     0,     0,   567,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
   568,     0,     0,     0,   569,   570,   221,   222,     0,     0,
     0,   571,   572,     0,     0,     0,   573,     0,     0,   574,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   575,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   576,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,   577,   274,   275,     0,    25,   578,    26,     0,
     0,     0,     0,     0,   579,     0,     0,     0,   581,     0,
   582,     0,     0,     0,     0,     0,   583,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,     0,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   545,   546,
   547,   548,     0,     0,   549,     0,     0,     0,     0,     0,
     0,   380,   550,   551,   552,   553,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
   556,   557,   558,     0,   559,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,   563,
   564,     0,   566,   210,     0,   211,   212,     0,     0,     0,
     0,   567,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,    73,     0,   568,     0,
     0,     0,   569,   570,   221,   222,     0,     0,     0,   571,
   572,     0,     0,     0,   573,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
   577,   274,   275,     0,    25,   578,    26,     0,     0,     0,
     0,     0,   579,     0,     0,     0,   581,     0,   582,     0,
     0,     0,     0,     0,   583,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   545,   546,   547,   548,
     0,     0,   549,     0,     0,     0,     0,     0,     0,   380,
   550,   551,   552,   553,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,   556,   557,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,   563,   564,     0,
   566,   210,     0,   211,   212,     0,     0,     0,     0,   567,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,   568,     0,     0,     0,
   569,   570,   221,   222,     0,     0,     0,   571,   572,     0,
     0,     0,   573,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,   381,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,   577,   274,
   275,     0,    25,   578,    26,     0,     0,     0,     0,     0,
   579,     0,     0,     0,   581,     0,   582,     0,     0,     0,
     0,     0,   583,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   545,  1046,   547,   548,     0,     0,
   549,     0,     0,     0,     0,     0,     0,   380,  1047,  1048,
  1049,  1050,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,  1051,  1052,   558,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,  1053,   564,     0,   566,   210,
     0,   211,   212,     0,     0,     0,     0,  1054,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,  1055,     0,     0,     0,   569,   570,
   221,   222,     0,     0,     0,  1056,   572,     0,     0,     0,
   573,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,  1057,   274,   275,     0,
    25,   578,    26,     0,     0,     0,     0,     0,  1058,     0,
     0,     0,  1059,     0,  1060,     0,     0,     0,     0,     0,
  1061,   514,   515,   516,   517,   518,   519,   520,   521,   522,
     0,   523,     0,   524,   525,   526,   527,   528,   529,   530,
   531,   532,   533,     0,   534,     0,   535,   536,   537,   538,
   539,     0,   540,   541,   542,   543,   544,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,  1149,     0,     0,  1150,     0,     0,     0,     0,     0,
     0,     0,   545,   546,   547,   548,     0,     0,   549,     0,
     0,     0,     0,     0,     0,   380,   550,   551,   552,   553,
     0,   200,   554,     0,     0,     0,     0,     0,   555,     0,
     0,     0,     0,     0,   556,   557,   558,     0,   559,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   560,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   561,     0,
   562,   209,     0,     0,   564,     0,   566,   210,     0,   211,
   212,     0,     0,     0,     0,   567,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,   568,     0,     0,     0,   569,   570,   221,   222,
     0,     0,     0,   571,   572,     0,     0,     0,   573,     0,
     0,   574,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   575,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   576,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,     0,   274,   275,     0,    25,   578,
    26,     0,     0,     0,     0,     0,   579,     0,     0,     0,
   581,     0,   582,     0,     0,     0,     0,     0,  1151,   514,
   515,   516,   517,   518,   519,   520,   521,   522,     0,   523,
     0,   524,   525,   526,   527,   528,   529,   530,   531,   532,
   533,     0,   534,     0,   535,   536,   537,   538,   539,     0,
   540,   541,   542,   543,   544,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  1118,   547,   548,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   380,  1119,  1120,  1121,  1122,     0,   200,
   554,     0,     0,     0,     0,     0,   555,     0,     0,     0,
     0,     0,     0,     0,   558,     0,   559,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   560,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   561,     0,   562,   209,
     0,     0,     0,     0,   566,   210,     0,   211,   212,     0,
     0,     0,     0,  1123,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
  1124,     0,     0,     0,   569,   570,   221,   222,     0,     0,
     0,  1125,   572,     0,     0,     0,  1126,     0,     0,   574,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   575,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,   381,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   576,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,  1127,   274,   275,     0,    25,   578,    26,     0,
     0,     0,     0,     0,  1128,     0,     0,     0,  1129,     0,
  1130,     0,     0,     0,     0,     0,  1131,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,     0,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1118,
   547,   548,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   380,  1119,  1120,  1121,  1122,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
     0,     0,   558,     0,   559,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,     0,
     0,     0,   566,   210,     0,   211,   212,     0,     0,     0,
     0,  1123,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,  1124,     0,
     0,     0,   569,   570,   221,   222,     0,     0,     0,  1125,
   572,     0,     0,     0,  1126,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
     0,   274,   275,     0,    25,   578,    26,     0,     0,     0,
     0,     0,  1128,     0,     0,     0,  1129,     0,  1130,     0,
     0,     0,     0,     0,  1131,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  2094,   547,   548,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  2095,  2096,  2097,  2098,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,     0,     0,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,     0,     0,  2099,
   566,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,     0,     0,     0,     0,
   569,   570,   221,   222,     0,     0,     0,     0,   572,     0,
     0,     0,  2100,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,     0,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,  2101,   274,
     0,     0,    25,   578,    26,     0,     0,     0,     0,     0,
  2102,     0,     0,     0,  2103,     0,  2104,     0,     0,     0,
     0,     0,  2105,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  2094,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  2095,  2096,
  2097,  2098,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,     0,     0,   558,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,     0,     0,     0,   566,   210,
     0,   211,   212,     0,     0,     0,     0,     0,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,     0,     0,     0,     0,   569,   570,
   221,   222,     0,     0,     0,     0,   572,     0,     0,     0,
  2100,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,     0,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,  2101,   274,     0,     0,
    25,   578,    26,     0,     0,     0,     0,     0,  2102,     0,
     0,     0,  2103,     0,  2104,     0,     0,     0,     0,     0,
  2105,   514,   515,   516,   517,   518,   519,   520,   521,   522,
     0,   523,     0,   524,   525,   526,   527,   528,   529,   530,
   531,   532,   533,     0,   534,     0,   535,   536,   537,   538,
   539,     0,   540,   541,   542,   543,   544,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   198,   199,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,  1022,   547,   548,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   380,     0,     0,     0,     0,
     0,   200,   554,     0,     0,     0,     0,     0,   555,     0,
     0,     0,     0,     0,     0,     0,   558,     0,   559,     0,
     0,     0,     0,     0,     0,     0,   202,     0,     0,   203,
     0,     0,   560,     0,     0,     0,     0,   204,   205,     0,
     0,     0,     0,     0,   206,   207,   208,     0,   561,     0,
   562,   209,     0,     0,     0,     0,   566,   210,     0,   211,
   212,     0,     0,     0,     0,  1023,     0,     0,   213,   214,
     0,     0,   215,     0,   216,     0,     0,     0,   217,   218,
     0,     0,  1024,     0,     0,     0,   569,   570,   221,   222,
     0,     0,     0,  1025,   572,     0,     0,     0,     0,     0,
     0,   574,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   575,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
   381,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
   576,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,   273,  1026,   274,     0,     0,    25,   578,
    26,     0,     0,     0,     0,     0,  1027,     0,     0,     0,
  1028,     0,     0,     0,     0,     0,     0,     0,  1029,   514,
   515,   516,   517,   518,   519,   520,   521,   522,     0,   523,
     0,   524,   525,   526,   527,   528,   529,   530,   531,   532,
   533,     0,   534,     0,   535,   536,   537,   538,   539,     0,
   540,   541,   542,   543,   544,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   198,   199,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,  2094,   547,   548,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,  2095,  2096,  2097,  2098,     0,   200,
   554,     0,     0,     0,     0,     0,   555,     0,     0,     0,
     0,     0,     0,     0,   558,     0,   559,     0,     0,     0,
     0,     0,     0,     0,   202,     0,     0,   203,     0,     0,
   560,     0,     0,     0,     0,   204,   205,     0,     0,     0,
     0,     0,   206,   207,   208,     0,   561,     0,   562,   209,
     0,     0,     0,     0,   566,   210,     0,   211,   212,     0,
     0,     0,     0,     0,     0,     0,   213,   214,     0,     0,
   215,     0,   216,     0,     0,     0,   217,   218,     0,     0,
     0,     0,     0,     0,   569,   570,   221,   222,     0,     0,
     0,     0,   572,     0,     0,     0,  2100,     0,     0,   574,
     0,     0,     0,     0,     0,     0,   223,   224,   225,   575,
     0,   227,   228,     0,   229,   230,     0,   231,     0,     0,
   232,   233,   234,   235,   236,     0,   237,   238,     0,     0,
   239,   240,   241,   242,   243,   244,   245,   246,   247,     0,
     0,     0,     0,   248,     0,   249,   250,     0,     0,   251,
   252,     0,   253,     0,   254,     0,   255,   256,   257,   258,
     0,   259,     0,   260,   261,   262,   263,   264,   576,     0,
   265,   266,   267,   268,   269,     0,     0,   270,     0,   271,
   272,   273,     0,   274,     0,     0,    25,   578,    26,     0,
     0,     0,     0,     0,  2102,     0,     0,     0,  2103,     0,
  2104,     0,     0,     0,     0,     0,  2105,   514,   515,   516,
   517,   518,   519,   520,   521,   522,     0,   523,     0,   524,
   525,   526,   527,   528,   529,   530,   531,   532,   533,     0,
   534,     0,   535,   536,   537,   538,   539,     0,   540,   541,
   542,   543,   544,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,  1022,
   547,   548,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   380,     0,     0,     0,     0,     0,   200,   554,     0,
     0,     0,     0,     0,   555,     0,     0,     0,     0,     0,
     0,     0,   558,     0,   559,     0,     0,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,   560,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,   561,     0,   562,   209,     0,     0,
     0,     0,   566,   210,     0,   211,   212,     0,     0,     0,
     0,  1023,     0,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,  1024,     0,
     0,     0,   569,   570,   221,   222,     0,     0,     0,  1025,
   572,     0,     0,     0,     0,     0,     0,   574,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   575,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,   381,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,   576,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,   273,
     0,   274,     0,     0,    25,   578,    26,     0,     0,     0,
     0,     0,  1027,     0,     0,     0,  1028,     0,     0,     0,
     0,     0,     0,     0,  1029,   514,   515,   516,   517,   518,
   519,   520,   521,   522,     0,   523,     0,   524,   525,   526,
   527,   528,   529,   530,   531,   532,   533,     0,   534,     0,
   535,   536,   537,   538,   539,     0,   540,   541,   542,   543,
   544,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,  1855,   547,   548,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   200,   554,     0,     0,     0,
     0,     0,   555,     0,     0,     0,     0,     0,     0,     0,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,   203,     0,     0,   560,     0,     0,     0,
     0,   204,   205,     0,     0,     0,     0,     0,   206,   207,
   208,     0,   561,     0,   562,   209,     0,  1856,     0,  1857,
   566,   210,     0,   211,   212,     0,     0,     0,     0,     0,
     0,     0,   213,   214,     0,     0,   215,     0,   216,     0,
     0,     0,   217,   218,     0,     0,     0,     0,     0,     0,
   569,   570,   221,   222,     0,     0,     0,     0,   572,     0,
     0,     0,     0,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,   224,   225,   575,     0,   227,   228,     0,
   229,   230,     0,   231,     0,     0,   232,   233,   234,   235,
   236,     0,   237,   238,     0,     0,   239,   240,   241,   242,
   243,   244,   245,   246,   247,     0,     0,     0,     0,   248,
     0,   249,   250,     0,     0,   251,   252,     0,   253,     0,
   254,     0,   255,   256,   257,   258,     0,   259,     0,   260,
   261,   262,   263,   264,   576,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,   273,  1858,   274,
     0,     0,    25,   578,    26,     0,     0,     0,     0,     0,
  1859,     0,     0,     0,  1860,     0,  1861,     0,     0,     0,
     0,     0,  1862,   514,   515,   516,   517,   518,   519,   520,
   521,   522,     0,   523,     0,   524,   525,   526,   527,   528,
   529,   530,   531,   532,   533,     0,   534,     0,   535,   536,
   537,   538,   539,     0,   540,   541,   542,   543,   544,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1855,   547,   548,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   200,   554,     0,     0,     0,     0,     0,
   555,     0,     0,     0,     0,     0,     0,     0,   558,     0,
   559,     0,     0,     0,     0,     0,     0,     0,   202,     0,
     0,   203,     0,     0,   560,     0,     0,     0,     0,   204,
   205,     0,     0,     0,     0,     0,   206,   207,   208,     0,
   561,     0,   562,   209,     0,     0,     0,  1857,   566,   210,
     0,   211,   212,     0,     0,     0,     0,     0,     0,     0,
   213,   214,     0,     0,   215,     0,   216,     0,     0,     0,
   217,   218,     0,     0,     0,     0,     0,     0,   569,   570,
   221,   222,     0,     0,     0,     0,   572,     0,     0,     0,
     0,     0,     0,   574,     0,     0,     0,     0,     0,     0,
   223,   224,   225,   575,     0,   227,   228,     0,   229,   230,
     0,   231,     0,     0,   232,   233,   234,   235,   236,     0,
   237,   238,     0,     0,   239,   240,   241,   242,   243,   244,
   245,   246,   247,     0,     0,     0,     0,   248,     0,   249,
   250,     0,     0,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,   576,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,   273,     0,   274,     0,     0,
    25,   578,    26,     0,     0,     0,     0,     0,  1859,     0,
     0,     0,  1860,     0,  1861,     0,     0,     0,     0,     0,
  1862,   167,   168,   169,   170,   171,   172,   173,   174,   175,
     0,   176,     0,   177,   178,   179,   180,   181,   182,   183,
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
    73,     0,     0,     0,     0,     0,   219,   220,   221,   222,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   223,   224,
   225,   226,     0,   227,   228,     0,   229,   230,     0,   231,
     0,     0,   232,   233,   234,   235,   236,     0,   237,   238,
     0,     0,   239,   240,   241,   242,   243,   244,   245,   246,
   247,     0,     0,     0,     0,   248,     0,   249,   250,     0,
     0,   251,   252,     0,   253,     0,   254,     0,   255,   256,
   257,   258,     0,   259,     0,   260,   261,   262,   263,   264,
     0,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,     0,     0,   274,   167,   168,   169,   170,
   171,   172,   173,   174,   175,     0,   176,     0,   177,   178,
   179,   180,   181,   182,   183,   184,   185,   186,    91,   187,
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
     0,     0,     0,   217,   218,     0,     0,     0,     0,     0,
     0,   219,   220,   221,   222,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   223,   224,   225,   226,     0,   227,   228,
     0,   229,   230,     0,   231,     0,     0,   232,   233,   234,
   235,   236,     0,   237,   238,     0,     0,   239,   240,   241,
   242,   243,   244,   245,   246,   247,     0,     0,     0,     0,
   248,     0,   249,   250,     0,     0,   251,   252,     0,   253,
     0,   254,     0,   255,   256,   257,   258,     0,   259,     0,
   260,   261,   262,   263,   264,  1603,     0,   265,   266,   267,
   268,   269,     0,     0,   270,     0,   271,   272,   273,   492,
   274,     0,     0,    25,     0,    26,     0,   465,   466,   467,
   468,  1604,   470,   471,   472,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,     0,   177,   178,   179,
   180,   181,   182,   183,   184,   185,   186,     0,   187,     0,
   188,   189,   190,   191,   192,     0,   193,   194,   195,   196,
   197,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   200,     0,     0,   972,     0,
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
   469,   470,   471,   472,   167,   168,   169,   170,   171,   172,
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
     0,     0,   203,     0,     0,     0,     0,     0,     0,   462,
   204,   205,     0,     0,     0,     0,     0,   206,   207,   208,
     0,     0,     0,     0,   209,     0,     0,     0,     0,     0,
   210,     0,   211,   212,     0,     0,     0,     0,     0,     0,
     0,   213,   214,   463,     0,   215,     0,   216,     0,     0,
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
     0,     0,   270,     0,   271,   272,     0,   464,   274,     0,
     0,     0,     0,     0,     0,   465,   466,   467,   468,   469,
   470,   471,   472,   167,   168,   169,   170,   171,   172,   173,
   174,   175,     0,   176,     0,   177,   178,   179,   180,   181,
   182,   183,   184,   185,   186,     0,   187,     0,   188,   189,
   190,   191,   192,     0,   193,   194,   195,   196,   197,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
   198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   380,     0,     0,
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
   250,     0,   381,   251,   252,     0,   253,     0,   254,     0,
   255,   256,   257,   258,     0,   259,     0,   260,   261,   262,
   263,   264,     0,     0,   265,   266,   267,   268,   269,     0,
     0,   270,     0,   271,   272,     0,     0,   274,     0,     0,
     0,   578,     0,     0,     0,     0,     0,     0,     0,   871,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,     0,   177,   178,   179,   180,   181,   182,   183,   184,
   185,   186,     0,   187,     0,   188,   189,   190,   191,   192,
     0,   193,   194,   195,   196,   197,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,   198,   199,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
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
   271,   272,     0,     0,   274,   167,   168,   169,   170,   171,
   172,   173,   174,   175,     0,   176,   395,   177,   178,   179,
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
   261,   262,   263,   264,     0,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,   724,   177,   178,   179,   180,   181,   182,   183,   184,
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
   172,   173,   174,   175,     0,   176,  1193,   177,   178,   179,
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
   261,   262,   263,   264,     0,     0,   265,   266,   267,   268,
   269,     0,     0,   270,     0,   271,   272,     0,     0,   274,
   167,   168,   169,   170,   171,   172,   173,   174,   175,     0,
   176,  1516,   177,   178,   179,   180,   181,   182,   183,   184,
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
   271,   272,     0,     0,   274,   780,   781,   782,   783,   784,
   785,   786,   787,   788,     0,   789,  1829,   790,   791,   792,
   793,   794,   795,   796,   797,   798,   799,     0,   800,     0,
   801,   802,   803,   804,   805,     0,   806,   807,   808,   809,
   810,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,   547,   548,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   200,   554,     0,     0,     0,
     0,     0,   811,     0,     0,     0,     0,     0,     0,     0,
   558,     0,   559,     0,     0,     0,     0,     0,     0,     0,
   202,     0,     0,     0,     0,     0,   560,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   207,
   208,     0,   561,     0,   562,     0,     0,     0,     0,     0,
   566,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,   218,     0,     0,     0,     0,     0,     0,
   812,   813,     0,     0,     0,     0,     0,     0,   572,     0,
     0,     0,     0,     0,     0,   574,     0,     0,     0,     0,
     0,     0,   223,     0,     0,   814,     0,     0,   167,   168,
   169,   170,   171,   172,   173,   174,   175,     0,   176,     0,
   177,   178,   179,   180,   181,   182,   183,   184,   185,   186,
     0,   187,     0,   188,   189,   190,   191,   192,     0,   193,
   194,   195,   196,   197,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   576,   198,   199,     0,     0,     0,
     0,     0,     0,     0,     0,     0,   272,   273,     0,   274,
     0,     0,    25,   578,    26,     0,     0,     0,     0,     0,
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
     0,     0,   274,     0,     0,     0,   578,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,     0,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,   293,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,   294,     0,     0,     0,     0,     0,   200,     0,     0,
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
     0,     0,     0,   200,     0,     0,   429,     0,     0,     0,
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
     0,     0,   265,   266,   267,   268,   269,     0,     0,   270,
     0,   271,   272,     0,     0,   274,   275,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,     0,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,  1077,     0,
     0,     0,     0,     0,  1078,     0,     0,     0,  1079,     0,
     0,  1080,     0,     0,     0,     0,     0,   200,     0,     0,
     0,     0,     0,     0,   201,     0,  1081,  1082,     0,     0,
     0,     0,  1083,     0,     0,     0,  1084,     0,     0,     0,
  1085,     0,   202,     0,     0,   203,     0,     0,     0,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,     0,     0,     0,   209,     0,     0,
  1086,     0,     0,   210,     0,   211,   212,     0,  1087,     0,
     0,  1088,  1089,     0,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,     0,  1090,
     0,  1091,   219,   220,   221,   222,     0,     0,  1092,     0,
  1093,     0,     0,     0,     0,     0,     0,     0,     0,     0,
  1094,     0,     0,     0,   223,   224,   225,   226,  1095,   227,
   228,  1096,   229,   230,  1097,   231,  1098,  1099,   232,   233,
   234,   235,   236,  1100,   237,   238,  1101,  1102,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,  1103,     0,
  1104,   248,  1105,   249,   250,  1106,  1107,   251,   252,  1108,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
  1109,   260,   261,   262,   263,   264,  1110,  1111,   265,   266,
   267,   268,   269,     0,  1112,   270,  1113,   271,   272,     0,
     0,   274,   167,   168,   169,   170,   171,   172,   173,   174,
   175,     0,   176,     0,   177,   178,   179,   180,   181,   182,
   183,   184,   185,   186,     0,   187,     0,   188,   189,   190,
   191,   192,     0,   193,   194,   195,   196,   197,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,   547,   548,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   200,   950,     0,     0,     0,     0,     0,   951,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   952,
     0,     0,     0,     0,     0,     0,     0,   202,     0,     0,
   203,     0,     0,     0,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,   561,
     0,   562,   209,     0,     0,     0,     0,   953,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   219,   220,   221,
   222,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   574,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   226,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,     0,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,     0,     0,   274,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,     0,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,  1257,     0,     0,     0,     0,     0,  1292,     0,
     0,     0,     0,     0,     0,     0,     0,   200,     0,     0,
     0,     0,     0,     0,   201,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,  1259,     0,     0,     0,
     0,     0,   202,     0,     0,   203,     0,     0,     0,     0,
     0,     0,     0,   204,   205,     0,     0,     0,     0,     0,
   206,   207,   208,     0,     0,     0,     0,   209,     0,     0,
     0,     0,     0,   210,     0,   211,   212,     0,     0,     0,
     0,     0,     0,  1260,   213,   214,     0,     0,   215,     0,
   216,     0,     0,     0,   217,   218,     0,     0,     0,     0,
     0,     0,   219,   220,   221,   222,     0,     0,     0,     0,
     0,     0,  1261,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   223,   224,   225,   226,     0,   227,
   228,     0,   229,   230,     0,   231,     0,     0,   232,   233,
   234,   235,   236,     0,   237,   238,     0,     0,   239,   240,
   241,   242,   243,   244,   245,   246,   247,     0,     0,     0,
     0,   248,     0,   249,   250,     0,     0,   251,   252,     0,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,     0,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,     0,
     0,   274,   167,   168,   169,   170,   171,   375,   173,   174,
   175,     0,   176,     0,   177,   178,   179,   180,   181,   182,
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
     0,     0,     0,     0,     0,   206,   207,   208,   376,     0,
     0,     0,   209,     0,     0,     0,     0,     0,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,     0,   215,     0,   216,     0,     0,     0,   217,
   218,     0,     0,     0,     0,     0,     0,   377,   220,   221,
   222,     0,     0,   378,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   226,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,     0,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,     0,     0,   274,   167,   168,   169,
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
     0,   274,   167,   168,   169,   170,   171,   172,   173,   174,
   175,     0,   176,     0,   177,   178,   179,   180,   181,   182,
   183,   184,   185,   186,     0,   187,     0,   188,   189,   190,
   191,   192,     0,   193,   194,   195,   196,   197,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   198,
   199,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,   200,     0,     0,     0,     0,     0,     0,   201,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,  1314,     0,   202,     0,     0,
   203,     0,     0,     0,     0,     0,     0,     0,   204,   205,
     0,     0,     0,     0,     0,   206,   207,   208,     0,     0,
     0,     0,   209,     0,     0,     0,     0,     0,   210,     0,
   211,   212,     0,     0,     0,     0,     0,     0,     0,   213,
   214,     0,  1315,   215,     0,   216,     0,     0,     0,   217,
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
     0,     0,   403,   220,   221,   222,     0,     0,   404,     0,
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
   175,     0,   176,     0,   177,   178,   179,   180,   181,   182,
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
   218,     0,     0,     0,     0,     0,     0,   406,   220,   221,
   222,     0,     0,   407,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,   223,
   224,   225,   226,     0,   227,   228,     0,   229,   230,     0,
   231,     0,     0,   232,   233,   234,   235,   236,     0,   237,
   238,     0,     0,   239,   240,   241,   242,   243,   244,   245,
   246,   247,     0,     0,     0,     0,   248,     0,   249,   250,
     0,     0,   251,   252,     0,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,     0,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,     0,     0,   274,   167,   168,   169,
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
     0,   248,     0,   249,   250,     0,     0,   251,   252,   981,
   253,     0,   254,     0,   255,   256,   257,   258,     0,   259,
     0,   260,   261,   262,   263,   264,     0,     0,   265,   266,
   267,   268,   269,     0,     0,   270,     0,   271,   272,     0,
     0,   274,   167,   168,   169,   170,   171,   172,   173,   174,
   175,     0,   176,     0,   177,   178,   179,   180,   181,   182,
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
     0,     0,   251,   252,  1621,   253,     0,   254,     0,   255,
   256,   257,   258,     0,   259,     0,   260,   261,   262,   263,
   264,     0,     0,   265,   266,   267,   268,   269,     0,     0,
   270,     0,   271,   272,     0,     0,   274,   167,   168,   169,
   170,   171,   172,   173,   174,   175,     0,   176,     0,   177,
   178,   179,   180,   181,   182,   183,   184,   185,   186,     0,
   187,     0,   188,   189,   190,   191,   192,     0,   193,   194,
   195,   196,   197,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,   198,   199,     0,     0,     0,     0,
     0,  1824,     0,     0,     0,     0,     0,     0,     0,     0,
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
   175,     0,   176,     0,   177,   178,   179,   180,   181,   182,
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
     0,   260,   261,   262,   263,   264,     0,     0,   265,   335,
   267,   268,   269,     0,     0,   270,     0,   271,   272,     0,
     0,   274,   167,   168,   695,   170,   171,   172,   173,   174,
   175,     0,   176,     0,   177,   178,   179,   180,   181,   182,
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
     0,     0,     0,     0,   213,   214,     0,     0,  1203,     0,
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
     0,   274
};

static const short yycheck[] = {     1,
     1,     1,    91,   310,   511,   350,    56,   625,   333,   932,
   347,   435,   989,   320,    49,   833,  1567,    49,   165,    50,
    52,  1539,  1510,   597,   709,  1274,    76,   325,    42,  1438,
  1248,    42,   339,    19,  2055,    42,    42,    17,    19,   835,
  1845,  1502,    27,  1251,    18,   432,   344,    63,   113,    59,
    40,    54,    84,    43,    96,   111,    46,     4,   113,    63,
    63,    51,    66,    53,    54,    12,    63,    75,  1039,   438,
  1041,  1042,   307,    96,    82,    83,    97,    95,    92,    87,
   374,    92,    29,   548,  2078,    92,    92,   182,    35,    36,
   102,    96,   172,   135,   172,   125,   753,   562,    97,   118,
   198,   122,    96,   275,   172,   249,   191,  2101,  2102,  2103,
  2104,  2105,   104,    30,   192,    32,   191,   191,   137,   776,
  1127,  1128,  1129,  1130,    63,   297,   191,   143,    59,    76,
   121,   123,    59,   176,   275,   280,    49,    50,  1145,    52,
    53,    54,    55,   288,   160,    59,    62,    60,    64,    23,
    63,   172,   171,   100,    67,   299,   297,    97,    78,    59,
    99,    74,    75,   110,   509,    78,   184,   512,   189,    82,
    83,   745,    96,    33,    87,    88,    89,   275,   172,   170,
    40,   199,   122,    43,   196,   101,    46,  2208,   274,   284,
   189,    51,   112,    53,    54,   187,    97,   704,   192,   297,
   857,    41,    42,   393,    44,    45,   274,    47,    48,   277,
    50,   191,    52,   448,   299,    55,    56,    57,    58,   210,
   300,   122,   300,   101,   299,   299,   301,   301,   145,    59,
  1353,   114,   300,   275,   299,   278,    59,   293,   259,   159,
   187,    59,   113,   205,   299,  2239,   193,   209,    96,   189,
   285,   620,   275,   285,  2248,  2249,  2250,  2251,  2252,  2253,
  2254,  2255,  2256,  2257,   280,  1388,   276,    59,   188,   299,
   275,   113,   288,   156,   284,   285,   286,   287,   288,   289,
   290,   291,  2303,   280,   281,    59,   673,   477,   189,   583,
    78,   301,   278,    67,    82,   173,   300,   278,   872,   147,
   274,   309,   876,   277,   769,    59,   300,   293,   210,   301,
   290,   182,   298,   303,   299,  2364,  1867,  1868,   321,   322,
  1443,   329,   353,    73,   112,   328,   415,   274,   150,   337,
  1548,  1127,  1128,  1129,  1130,  2356,   760,  2358,   341,   255,
   343,   214,  1751,   375,   376,  1553,   247,  1555,   351,  1145,
   280,   275,  1813,   296,   297,   164,   303,   359,   289,   290,
   291,   369,   289,   290,   291,  2414,   374,   125,   298,   191,
   243,   159,   285,   423,  2368,   289,   290,   291,   392,   292,
   189,   392,   390,  1040,  1355,   392,   392,    59,  2193,   289,
   290,   291,   400,    65,   144,  1402,   309,   310,   215,   401,
   188,   432,  1026,  1027,  1028,  1029,   437,   320,   321,   322,
   137,   272,   325,   326,   819,   328,   329,    60,   992,  1390,
   333,  1909,   335,   173,   337,   338,   339,   244,   341,    97,
   343,   344,   345,  1440,   274,   228,   439,   350,   351,   442,
   175,    63,   445,   303,   171,  1452,  1453,  1454,  1455,  1456,
  1457,  1458,   460,   276,   122,   190,   369,   104,  1032,   289,
   290,   291,   375,   495,   287,   288,   289,   290,   291,   275,
   977,   289,   290,   291,  1445,    97,   123,   390,   301,   184,
   393,   296,  1300,   175,   276,   300,   489,   400,   108,  1354,
  1064,   297,   191,   917,   199,   287,   288,   289,   290,   291,
   122,   293,   276,   505,   172,    59,   298,   154,   275,  1327,
   280,   424,   300,   287,   288,   289,   290,   291,   191,   432,
   433,   189,   276,   166,  1389,   145,   439,   147,   298,   442,
   191,   108,   445,   287,   288,   289,   290,   291,   233,   293,
   187,   191,   113,  1361,   298,   108,    59,   460,   119,   638,
   172,   640,   584,    59,   249,   280,   108,   293,   275,  1133,
   854,  2022,   298,    70,   477,   208,   186,   189,  2029,    59,
   147,  2122,   182,   298,  1192,   274,   489,  1986,   277,  1444,
   297,   224,  1400,   615,   147,   617,   618,   280,   275,  1207,
   933,   934,   935,    59,  1279,   147,   509,   647,  1283,   512,
   513,    24,   652,   653,   276,   298,  1402,    27,   108,   186,
   297,   619,   119,   621,   912,   287,   288,   289,   290,   291,
   623,    33,    59,   186,   275,   633,   299,   134,   301,   119,
   937,  1880,   545,  1451,   186,   276,   671,   259,   299,   671,
   301,  1849,   673,  1851,  1440,   145,   297,   147,  1053,   299,
   739,   301,  1057,  1058,  1059,  1060,  1452,  1453,  1454,  1455,
  1456,  1457,  1458,   576,   274,   275,   668,   277,   278,   278,
    59,  1594,   108,  2191,  1645,  1646,  1647,   280,    67,   266,
   267,  1279,   714,  1690,   113,  1283,   186,  1053,   275,   126,
   693,  1057,  1058,  1059,  1060,   989,   280,  1354,   301,    49,
    50,    93,    52,  1674,  1675,  1676,   619,  2258,   621,   145,
   623,   147,   191,  1370,  1371,   723,  2177,   301,  2179,   722,
   633,   126,  1379,    20,    21,   728,   118,   275,  1352,   732,
   733,    28,  1389,   287,   288,   289,   290,   291,  1362,  1363,
  1364,  1365,  1366,  1367,  1368,   137,   138,  2235,   258,   297,
   186,  1569,   754,   755,   756,   265,   277,   278,   671,   761,
   673,   908,   909,  1420,    95,    13,    97,  1061,    70,  1776,
   276,   773,   175,  2261,   287,   288,   289,   290,   291,   171,
   693,   287,   288,   289,   290,   291,   276,  1444,  1759,  1760,
  1761,   122,   275,    26,   231,    70,   709,   287,   288,   289,
   290,   291,   715,  1460,   274,   253,   275,   277,   200,   722,
   723,   248,   275,  1320,   297,   728,   275,   119,   228,   732,
   733,   287,   288,   289,   290,   291,   739,   829,   297,   831,
   130,   293,   134,   275,   297,  2043,   298,  2045,   297,   274,
    59,   172,   277,   161,   119,   280,   854,   282,  1422,   275,
   287,   288,   289,   290,   291,   297,   859,  1151,   189,   134,
   211,  1817,  1156,   114,  1820,   216,  1160,   302,    61,   120,
  1164,   297,   293,    66,  1168,   293,   227,   298,  1172,    72,
   298,   132,  1176,   914,    77,   150,  1180,   276,   239,   240,
  1184,   299,   894,   895,  1690,   284,   285,   286,   287,   288,
   289,   290,   291,   275,   155,   907,  1856,   907,  1858,  1859,
  1860,  1861,  1862,   264,   275,   150,   247,   168,    59,   832,
   833,   923,   924,     4,   297,   297,    59,   300,   275,    70,
   275,    12,    65,  1940,   284,   285,   297,  1914,   297,   280,
   281,   300,   294,   856,    59,   858,   859,   299,    29,    59,
   297,   293,   297,   866,    35,    36,   298,    67,   300,   150,
   962,  1385,   964,   965,   966,   967,  1303,  1304,   976,   300,
   275,   201,   275,    59,   293,   127,   275,  1302,   119,   298,
  1776,   989,    59,   274,   275,   126,   277,   173,    65,   280,
   126,   282,   297,   134,   297,    76,  1401,   910,   297,   912,
  1405,   914,  1407,   353,  1661,  1410,  1411,  1412,  1413,  1414,
  1415,  1416,  1417,  1418,    78,   275,  1421,    59,   150,   100,
    84,   275,  1533,  1534,   937,   375,    20,    21,    70,   110,
  1324,   275,    96,   201,    28,  1401,   275,   297,  1332,  1405,
   126,  1407,   127,   297,  1410,  1411,  1412,  1413,  1414,  1415,
  1416,  1417,  1418,   297,   275,  1421,   969,   276,   297,   201,
   280,   275,   282,   976,   977,   284,   285,   286,   287,   288,
   289,   290,   291,   127,   987,   275,   297,   119,   275,   207,
   993,   145,   432,   297,   126,   998,    59,   437,   293,   276,
   231,    27,   134,   298,    72,   159,  2046,   297,   285,   286,
   297,   165,  1920,   275,  2054,   275,   187,   248,  2058,   275,
  2060,  1929,   193,  2063,  2064,  2065,  2066,  2067,  2068,  2069,
  2070,  2071,    63,  2073,   188,   297,    42,   297,   293,  2136,
   275,   297,    48,   298,    50,   276,    52,   275,   275,   111,
   103,   275,  2092,   276,  1940,   231,   287,   288,   289,   290,
   291,    72,   297,   126,   287,   288,   289,   290,   291,   297,
   297,   276,   248,   297,   652,   653,   276,   293,    67,  1987,
   285,   286,   287,   288,   289,   290,   291,   287,   288,   289,
   290,   291,  1214,  1191,   182,   299,   293,   301,  1238,   231,
   293,   298,    97,   274,   299,   298,   301,   300,  1206,   276,
   289,   287,   288,   289,   290,   291,   248,    97,   198,    67,
   287,   288,   289,   290,   291,  1218,   962,   122,   964,   965,
   966,   967,  1225,  1517,    59,   299,  1139,   301,    27,   300,
    65,   293,   122,    59,   276,  2053,   298,  1239,  1239,  1239,
   299,   198,   301,  1275,    67,   287,   288,   289,   290,   291,
   130,   300,   293,  1255,  1256,  1255,  1256,   298,   231,   609,
  2210,   170,    92,   293,  1267,  1273,   296,   172,   298,   293,
   300,  1303,  1304,   299,   298,   248,   280,   299,  1191,   301,
   293,   545,   172,  1285,   189,   298,   293,  1692,   300,  1292,
  1203,   298,   293,  1206,   299,  1208,   301,   298,   150,   189,
   197,  1303,  1304,  1303,  1304,  1218,   299,  1310,   301,  1312,
     6,    59,  1225,     9,   287,   288,   289,   290,   291,    15,
    16,   671,    70,   673,   293,   293,  1692,  1834,   293,   298,
   298,   194,   293,   298,  1336,    31,  1630,   298,    34,    93,
  2136,   293,   247,   158,   300,   293,   298,   119,  1350,  1262,
   298,  1264,  1265,  1266,  1267,   619,   246,   247,   293,   299,
  1273,   301,  2312,   298,   118,    77,  1279,   293,   300,   300,
  1283,   119,   298,   145,  2031,   299,    67,   301,   126,  1292,
  1382,  1383,    59,   137,   138,   300,   134,  1300,    65,  1302,
  1303,  1304,  1305,    70,   300,   300,   299,  1310,   301,  1312,
   293,  1314,   300,  1697,  1978,   298,  1319,   297,  1702,   300,
   300,    59,    78,  1707,  1327,  1328,  1329,   171,  1712,   293,
   293,   300,    70,  1717,   298,   298,   180,   181,  1722,  2247,
    96,   293,   293,  1727,  1436,   300,   298,   298,  1732,  1441,
  1442,   276,   119,  1737,  2262,  2263,   200,   300,  1361,   126,
   276,   300,   287,   288,   289,   290,   291,   134,   299,   297,
   301,   287,   288,   289,   290,   291,   300,   300,   145,   819,
  2088,   119,   293,   297,   296,   293,   153,   298,   126,   145,
   298,   300,   296,   231,  2302,   835,   134,  1400,   300,   133,
   293,  1499,   293,   159,  1502,   298,   293,   298,   198,   165,
   248,   298,   293,   853,   276,   293,   293,   298,   119,   859,
   298,   298,   284,   285,   286,   287,   288,   289,   290,   291,
   293,    95,   188,   219,   278,   298,   133,   189,   276,   293,
   293,  1533,  1534,  1535,   298,   298,   133,   302,  1451,   287,
   288,   289,   290,   291,   299,   302,   301,  2365,  1550,  1551,
    86,  1583,   274,   302,   231,   277,   293,  1470,   280,   290,
   282,   298,   296,   293,   914,   293,   274,  1874,   298,   277,
   298,   248,   280,   299,   282,   293,   293,   299,   114,   301,
   298,   298,   293,   231,   120,   190,  1499,   298,    27,  1502,
   293,   299,   252,   301,  1507,   298,   132,  1510,   293,   276,
   248,   293,  1604,   298,  1604,   141,   298,   284,   285,   286,
   287,   288,   289,   290,   291,   300,   300,   293,    59,   155,
  1914,   293,   298,   237,   301,   293,   298,   257,   276,    70,
   298,  1544,   168,   300,   150,   111,   986,   285,   286,   287,
   288,   289,   290,   291,   293,    33,   125,   182,   293,   298,
   300,   187,    40,   298,  1567,    43,  1569,   293,    46,   276,
   192,   113,   298,    51,   296,    53,    54,   284,   285,   286,
   287,   288,   289,   290,   291,   293,   196,   299,   119,   301,
   298,   293,  1595,   293,  1597,   126,   298,   293,   298,   293,
  1603,   293,   298,   134,   298,   296,   298,   274,   275,    77,
   277,   278,  1615,  1053,   145,   172,    59,  1057,  1058,  1059,
  1060,   299,    65,   301,   299,   198,   301,    70,   182,   182,
  1070,     3,     4,     5,     6,     7,     8,     9,    10,    11,
   299,    13,   301,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,   178,    26,  2090,    28,    29,    30,    31,
    32,   182,    34,    35,    36,    37,    38,   299,   299,   301,
   301,   299,   299,   301,   301,   299,   119,   301,   182,   299,
  1683,   301,    59,   126,   301,   301,   299,  1127,  1128,  1129,
  1130,   134,   300,   113,   296,    86,   274,   300,   250,   299,
   231,   149,   145,    26,   289,  1145,   302,   302,    59,    25,
   153,    82,   150,   153,    65,  1813,   284,   248,   221,    70,
   150,    93,    94,   114,   158,   262,    67,   226,   100,   120,
   301,   173,   299,   284,   284,   299,    81,   128,   110,   301,
   301,   132,   301,   301,  1837,   276,   118,   113,   171,  1189,
   141,   138,   124,   284,   285,   286,   287,   288,   289,   290,
   291,  1883,   301,   301,   155,   137,   138,  2364,   119,   301,
   300,   299,  1775,   300,   300,   126,   148,   168,   300,   201,
  1873,   300,  1904,   134,   301,  1139,  1884,   300,   231,   300,
   300,   300,   300,   300,   145,   300,   297,    92,   298,   171,
   284,  1894,   153,   301,   300,   248,   178,   179,   300,   300,
  1813,   300,   300,   300,   300,    59,  1914,   300,   300,   300,
   300,  1824,   296,   300,   300,   300,    70,   300,   200,   300,
  2227,   203,   301,   276,  1837,   175,   300,   300,   300,   300,
   300,   284,   285,   286,   287,   288,   289,   290,   291,   300,
   190,   300,  1206,   300,   300,   195,   300,   274,   301,   258,
   233,    27,   202,   203,  1867,  1868,   206,   303,   303,   276,
  1873,  1874,   298,   301,   129,   119,   300,   217,   129,   300,
   231,  1884,   126,   119,   284,   225,   198,   191,   228,   111,
   134,  1894,   198,   103,    59,  1898,   111,   248,   301,  2039,
   301,   145,   274,   301,    59,   277,  1909,   301,   300,    59,
   300,   251,  2034,   253,   119,   301,   299,  1920,   116,   259,
   301,   261,   301,   301,  2022,   276,  1929,   301,   289,   299,
   191,  2029,   301,   284,   285,   286,   287,   288,   289,   290,
   291,   301,   301,  1946,   301,  2037,   301,  2037,   111,   274,
   301,   299,   274,   303,   303,   182,   300,   300,   116,   111,
  2082,  1401,  1402,   271,   153,  1405,    67,  1407,   182,   229,
  1410,  1411,  1412,  1413,  1414,  1415,  1416,  1417,  1418,   159,
   232,  1421,   300,   115,  1987,   300,   300,   231,   301,   128,
   301,   301,   301,   155,   301,   128,   301,  2000,   301,   301,
  1440,   301,   301,   301,   248,   301,   299,   301,   300,   300,
   300,   300,  1452,  1453,  1454,  1455,  1456,  1457,  1458,  2022,
   300,   300,   300,   300,   300,   300,  2029,   300,   300,   300,
   300,   300,   276,   300,   300,   300,   298,   301,   301,   298,
   284,   285,   286,   287,   288,   289,   290,   291,   301,    72,
  2053,   300,  2055,   300,    59,   301,   301,   117,   301,   301,
  1500,   301,   301,   301,   301,    70,   301,   301,   301,   301,
   301,   301,   301,   301,   218,   301,   301,   301,   301,  2177,
   301,  2179,   301,   301,   274,   284,   298,  2090,   301,   300,
    96,   300,   300,    96,  2186,   269,  2186,  2189,    49,  2189,
   220,    52,   129,    54,   105,   300,   300,   147,   129,    60,
   151,  2233,    63,   149,   119,   152,  1470,   301,   301,  2122,
   301,   126,   301,    74,    75,   301,   301,    78,   301,   134,
   301,    82,    83,   301,   301,   128,    87,    88,    89,  2232,
   145,   301,   176,   128,   301,   301,    59,   301,  2240,  2241,
   301,   162,   301,   301,   188,   301,   190,   130,   301,   301,
   301,   195,   299,   301,   300,   296,   300,    65,   202,   203,
   300,   300,   206,   300,  2177,   300,  2179,   301,   299,   301,
   219,   300,   300,   217,   301,   301,   301,   301,   301,   301,
   301,   225,   301,   301,   228,   301,   301,   301,   301,  2202,
   165,   301,   301,   301,   301,  2208,  2298,  2298,  2298,   301,
  1564,   301,   301,   301,   301,  2394,   301,   251,   301,   253,
    59,   136,   299,    65,  2227,   259,   231,   261,   300,  2232,
    59,   300,  2235,   230,   268,   301,    65,   301,   150,   301,
  2362,    70,   301,   248,  2247,   298,   294,   301,    61,   294,
  1690,     0,  1692,     0,    92,  2258,   452,   941,  2261,  2262,
  2263,  2264,  2265,  1272,   703,  1577,   618,   853,  1262,  1595,
  2362,   276,  2362,  1264,  1901,  2093,  2310,  2405,  2385,   284,
   285,   286,   287,   288,   289,   290,   291,  2410,  1585,  2411,
   119,  1597,  2375,  2411,    86,  1305,  1304,   126,   627,  2302,
  2303,    71,  2261,   510,   993,   134,   401,  2310,  2377,   491,
  1583,  2314,  2260,    90,   347,  2394,   145,   392,  1225,  2411,
  2361,  2411,  2407,  1309,   153,   881,  1186,  1319,  1220,  1683,
  2265,  1821,   418,  1535,   285,  1818,  1776,   873,  1749,  1502,
  2408,  1385,  1946,  2346,  1683,   995,   858,  1206,   854,  2352,
  2346,  1824,   281,  2356,   427,  2358,   576,  1538,   309,  2362,
   375,   910,  2365,   877,  1210,  2193,   914,   986,   164,  2189,
   321,   322,   917,   501,   325,   326,   931,   328,   329,  1500,
  1189,  2186,   333,    -1,    -1,    -1,   337,   338,    -1,    -1,
   341,    -1,   343,   344,   345,    -1,    -1,    -1,    -1,   350,
   351,    -1,   231,    -1,  2407,  2408,    -1,    -1,  2411,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   369,   248,
    -1,  1775,    -1,    -1,   375,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   390,
    -1,    -1,   393,    -1,    -1,    -1,    -1,   276,    -1,   400,
    -1,    -1,    -1,    -1,    -1,   284,   285,   286,   287,   288,
   289,   290,   291,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   301,   424,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   433,    -1,    -1,    -1,    -1,    -1,   439,    -1,
    -1,   442,    -1,    -1,   445,    -1,    -1,    -1,    -1,    -1,
  1940,    -1,  1856,    -1,  1858,  1859,  1860,  1861,  1862,   460,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   477,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   489,    -1,
    -1,  1895,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   509,    -1,
    -1,   512,   513,    -1,    33,    -1,    -1,    -1,    -1,    -1,
    39,    40,    41,    42,    43,    44,    45,    46,    47,    48,
    -1,    50,    51,    52,    53,    54,    55,    56,    57,    58,
    -1,    -1,  1946,    -1,   545,    -1,    -1,    -1,    -1,    -1,
    39,    40,    41,    42,    43,    44,    45,    46,    47,    48,
    -1,    50,    51,    52,    53,    54,    55,    56,    57,    58,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  2000,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   619,    -1,
   621,    -1,   623,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   633,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  2046,    -1,    -1,    -1,  2136,    -1,    -1,    -1,
  2054,  2055,    -1,    -1,  2058,    -1,  2060,    -1,    -1,  2063,
  2064,  2065,  2066,  2067,  2068,  2069,  2070,  2071,    -1,  2073,
   671,    -1,    -1,    -1,  2078,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2092,    -1,
    -1,    -1,   693,    -1,    -1,    -1,    -1,  2101,  2102,  2103,
  2104,  2105,    -1,    -1,    -1,    -1,    -1,    -1,   709,    -1,
    -1,    -1,    -1,    -1,   715,    -1,    -1,    -1,    -1,    -1,
    -1,   722,   723,    -1,    -1,    -1,    -1,   728,    -1,    -1,
    -1,   732,   733,    -1,    -1,    -1,    -1,    -1,   739,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   274,    -1,    -1,   277,    -1,
   279,   280,    -1,   282,    -1,   284,    -1,    -1,    -1,    -1,
   289,    -1,    -1,    -1,   775,   294,    -1,    -1,   297,   298,
   299,   300,   301,   302,   303,   274,    -1,    -1,   277,    -1,
    -1,   280,    -1,   282,    -1,   284,    -1,    -1,  2202,    -1,
   289,    -1,    -1,    -1,  2208,    -1,  2210,    -1,   297,   298,
   299,   300,   301,    -1,   303,    -1,    -1,    -1,   819,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   832,    -1,    -1,   835,  2239,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2248,  2249,  2250,  2251,  2252,  2253,
  2254,  2255,  2256,  2257,    -1,   856,    -1,   858,   859,    -1,
    -1,    -1,    -1,    -1,    -1,   866,    -1,    -1,    -1,    -1,
    -1,    -1,    39,    40,    41,    42,    43,    44,    45,    46,
    47,    48,    -1,    50,    51,    52,    53,    54,    55,    56,
    57,    58,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2303,
    -1,    -1,    -1,    -1,    -1,    -1,  2310,    -1,  2312,   910,
    -1,   912,    -1,    39,    40,    41,    42,    43,    44,    45,
    46,    47,    48,    -1,    50,    51,    52,    53,    54,    55,
    56,    57,    58,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  2356,    -1,  2358,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2368,    -1,    -1,    -1,   969,    -1,
    -1,    -1,    -1,    -1,    -1,   976,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   987,    -1,    -1,    -1,
    -1,    -1,   993,    -1,    -1,    -1,    -1,   998,    -1,    -1,
     3,    -1,    -1,    -1,     7,    -1,    -1,    10,    11,    -1,
    -1,    14,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    22,
    23,    -1,    -1,    -1,    -1,  1026,  1027,  1028,  1029,    -1,
    -1,    -1,    -1,    -1,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1053,    -1,    -1,    -1,  1057,  1058,  1059,  1060,
    -1,    64,    -1,    -1,    -1,    -1,    69,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    79,    -1,    -1,    -1,
    83,    -1,    85,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    95,    -1,    97,    -1,    -1,    -1,   101,    -1,
   103,    -1,   105,    -1,    -1,    -1,   109,   274,    -1,    -1,
   277,    -1,   115,   280,    -1,   282,    -1,   284,    -1,   122,
    -1,    -1,   289,    -1,    -1,    -1,  1127,  1128,  1129,  1130,
   297,   298,   299,   300,   301,    -1,   303,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1145,    -1,    -1,    -1,   274,    -1,
    -1,   277,    -1,    -1,   280,    -1,   282,    -1,   284,    -1,
    -1,    -1,    -1,   289,   167,    -1,   169,    59,    -1,   172,
   173,   297,   298,   299,   300,   301,    -1,    -1,    70,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   189,    -1,    -1,    -1,
  1191,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   203,   204,  1203,    -1,    -1,  1206,    -1,  1208,    -1,   212,
   213,    -1,    -1,    -1,    -1,    -1,    -1,  1218,    -1,   222,
   223,    -1,    -1,    -1,  1225,    -1,    -1,   119,    -1,    -1,
    -1,   234,   235,   236,   126,   238,    -1,    -1,   241,    -1,
    -1,    -1,   134,    -1,   247,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   256,    -1,    -1,    -1,    -1,    -1,    -1,
   263,  1262,    -1,  1264,  1265,  1266,  1267,   270,    -1,    -1,
    -1,    -1,  1273,    -1,    -1,    -1,    -1,    -1,  1279,    -1,
    -1,    -1,  1283,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1292,    -1,    -1,    -1,    -1,    -1,   300,    -1,    -1,
    -1,  1302,  1303,  1304,  1305,    -1,    -1,    -1,    -1,  1310,
    -1,  1312,    -1,  1314,    -1,    -1,    -1,    -1,  1319,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1328,  1329,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   231,
    -1,    -1,    -1,    49,    -1,    -1,    52,    -1,    54,    -1,
    -1,  1352,  1353,    -1,    60,    -1,   248,    63,    -1,    -1,
    -1,  1362,  1363,  1364,  1365,  1366,  1367,  1368,    74,    75,
    -1,    -1,    78,    -1,    -1,    -1,    82,    83,    -1,    -1,
    -1,    87,    88,    89,   276,    -1,    -1,  1388,    -1,    -1,
    -1,    -1,   284,   285,   286,   287,   288,   289,   290,   291,
  1401,  1402,    -1,    -1,  1405,    -1,  1407,    -1,    -1,  1410,
  1411,  1412,  1413,  1414,  1415,  1416,  1417,  1418,    -1,    -1,
  1421,    -1,    -1,    -1,    -1,    59,    -1,    -1,    -1,    -1,
    -1,    65,    -1,    -1,    -1,    -1,    70,    -1,    -1,  1440,
    -1,    -1,  1443,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1452,  1453,  1454,  1455,  1456,  1457,  1458,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    59,    -1,    -1,    -1,    -1,    -1,    65,    -1,    -1,
    -1,    -1,    70,    -1,    -1,   119,    -1,    -1,    -1,    -1,
    -1,    -1,   126,    -1,    -1,    -1,    -1,    -1,  1499,    -1,
   134,  1502,    -1,    -1,    -1,    -1,  1507,    -1,    -1,  1510,
    -1,   145,    -1,    -1,    -1,    -1,    -1,    -1,    59,   153,
    -1,    -1,    -1,    -1,    65,    -1,    -1,    -1,    -1,    70,
    -1,   119,    -1,    -1,    -1,    -1,    -1,    -1,   126,    -1,
    -1,    -1,    -1,  1544,    -1,    -1,   134,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   145,    -1,    -1,
    -1,    -1,    -1,  1564,    -1,   153,  1567,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   119,   285,
    -1,    -1,    -1,    -1,    -1,   126,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   134,  1595,    -1,  1597,   231,    59,    -1,
    -1,    -1,  1603,   309,   145,    -1,    -1,    -1,    -1,    70,
    -1,    -1,   153,    -1,   248,   321,   322,    -1,    -1,   325,
   326,    -1,   328,   329,    -1,    -1,    -1,   333,    -1,    -1,
    -1,   337,   338,    -1,    -1,   341,    -1,   343,   344,   345,
    -1,    -1,   276,   231,   350,   351,    -1,    -1,    -1,    -1,
   284,   285,   286,   287,   288,   289,   290,   291,   119,    59,
   248,    -1,    -1,   369,    -1,   126,    -1,   301,    -1,   375,
    70,    -1,    -1,   134,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   390,    -1,    -1,   393,   276,  1690,
   231,  1692,    -1,    -1,   400,    -1,   284,   285,   286,   287,
   288,   289,   290,   291,    -1,    -1,    59,   248,    -1,    -1,
    -1,   299,    65,    -1,    -1,    -1,    -1,    70,   424,   119,
    -1,    -1,    -1,    -1,    -1,    -1,   126,   433,    -1,    -1,
    59,    -1,    -1,   439,   134,   276,   442,    -1,    -1,   445,
    -1,    70,    -1,   284,   285,   286,   287,   288,   289,   290,
   291,    -1,    -1,    -1,   460,    -1,    -1,    -1,   299,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   119,    -1,    -1,    -1,
   231,   477,    -1,   126,    -1,  1776,    -1,    -1,    -1,    -1,
    -1,   134,    -1,   489,    -1,    -1,    -1,   248,    -1,    -1,
   119,    -1,   145,    -1,    -1,    -1,    -1,   126,    -1,    -1,
   153,    -1,    -1,   509,    -1,   134,   512,   513,    -1,    -1,
    -1,    -1,  1813,    -1,    -1,   276,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1824,   285,   286,   287,   288,   289,   290,
   291,   231,    -1,    -1,    -1,    -1,  1837,    -1,    -1,   545,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   248,    -1,
    -1,    -1,    -1,    -1,   560,  1856,    -1,  1858,  1859,  1860,
  1861,  1862,    -1,    -1,    -1,    -1,  1867,  1868,    -1,    -1,
   576,    -1,  1873,    -1,    -1,    -1,   276,    -1,   231,    -1,
    -1,    -1,    -1,  1884,   284,   285,   286,   287,   288,   289,
   290,   291,    -1,  1894,  1895,   248,    -1,  1898,    -1,    -1,
    -1,    -1,   231,    -1,    -1,    -1,    -1,    -1,  1909,    -1,
    -1,    -1,    -1,   619,    -1,   621,    -1,   623,    -1,   248,
    -1,    -1,    -1,   276,    -1,    -1,    -1,   633,    -1,    -1,
    -1,   284,   285,   286,   287,   288,   289,   290,   291,  1940,
    -1,    -1,    -1,    -1,    -1,    -1,   299,   276,    -1,   326,
    -1,    -1,    -1,    -1,    -1,   284,   285,   286,   287,   288,
   289,   290,   291,    -1,    -1,   671,    -1,    -1,   345,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   693,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   709,    -1,    -1,    -1,    -1,    -1,   715,
    -1,    -1,    -1,    -1,    -1,    -1,   722,   723,    -1,    -1,
    -1,  2022,   728,    -1,    -1,    -1,   732,   733,  2029,    -1,
    -1,    -1,    -1,   739,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  2046,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  2054,   760,    -1,    -1,  2058,    -1,  2060,
    -1,    -1,  2063,  2064,  2065,  2066,  2067,  2068,  2069,  2070,
  2071,    -1,  2073,    -1,    -1,    -1,    -1,  2078,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2090,
    -1,  2092,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  2101,  2102,  2103,  2104,  2105,    -1,    -1,    -1,    -1,     7,
    -1,    -1,    10,    11,    -1,    -1,    14,    -1,    -1,    -1,
    -1,  2122,    -1,    -1,    22,    23,   832,   833,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  2136,    -1,    -1,    -1,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   856,    -1,   858,   859,    -1,    -1,    -1,    -1,    -1,    -1,
   866,    -1,    -1,    -1,    -1,    -1,    64,    -1,    -1,    -1,
    -1,    69,    -1,    -1,    -1,    -1,  2177,    -1,  2179,    -1,
    -1,    79,    -1,    -1,    -1,    83,    -1,    85,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    95,    -1,    97,
    -1,    -1,    -1,   101,   910,   103,   912,   105,    -1,  2210,
    -1,   109,    -1,    -1,    -1,    -1,    -1,   115,    -1,    -1,
    -1,    -1,    -1,    -1,   122,    -1,    -1,    -1,    -1,    -1,
    -1,  2232,    -1,    59,  2235,    -1,    -1,    -1,  2239,    -1,
    -1,    -1,    -1,    -1,    70,    -1,    -1,  2248,  2249,  2250,
  2251,  2252,  2253,  2254,  2255,  2256,  2257,  2258,    -1,    -1,
  2261,    -1,    -1,   969,  2265,    -1,    -1,    -1,    -1,   167,
   976,   169,    -1,    -1,   172,   173,    -1,    -1,    -1,    -1,
    -1,   987,    -1,    -1,    -1,    -1,    -1,   993,    -1,    -1,
    -1,   189,   998,   119,    -1,    -1,    -1,    -1,    -1,    -1,
   126,    59,    -1,    -1,    -1,   203,   204,    65,   134,    67,
    -1,  2312,    70,  2314,   212,   213,    -1,    59,    -1,    -1,
    -1,    -1,    -1,    65,   222,   223,    68,    -1,    70,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   234,   235,   236,    -1,
   238,    -1,    -1,   241,    -1,    -1,    -1,    -1,    -1,   247,
    -1,  2352,    -1,    -1,    -1,    -1,    98,    -1,   256,    -1,
    -1,   119,    -1,    -1,    -1,   263,    -1,  2368,   126,    -1,
    -1,    -1,   270,    -1,    -1,    -1,   134,   119,    -1,    -1,
    -1,    -1,    -1,    -1,   126,    -1,    -1,   145,    -1,    -1,
    -1,    -1,   134,    -1,    -1,   153,    -1,    -1,   775,    -1,
    -1,    -1,   300,   145,    -1,   231,    -1,  2408,    -1,    -1,
    -1,   153,    59,    -1,    -1,    -1,    -1,    -1,    65,    -1,
    -1,    -1,   248,    70,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1139,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   819,    -1,    -1,    -1,    -1,    -1,    -1,   191,
   276,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   835,   285,
   286,   287,   288,   289,   290,   291,    -1,    -1,    -1,    -1,
    -1,    -1,   119,   231,    -1,    -1,    -1,    -1,    -1,   126,
    -1,    -1,    -1,    -1,    -1,  1191,    -1,   134,    -1,   231,
   248,    -1,    -1,    -1,    -1,    -1,    -1,  1203,   145,    -1,
  1206,    -1,  1208,    -1,    -1,    -1,   248,    -1,    -1,    -1,
    -1,    -1,  1218,    -1,    -1,    -1,    -1,    -1,   276,  1225,
    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,   286,   287,
   288,   289,   290,   291,   276,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   284,   285,   286,   287,   288,   289,   290,   291,
    -1,    -1,    -1,    -1,    -1,    -1,  1262,    -1,  1264,  1265,
  1266,  1267,    -1,    -1,    -1,    -1,    -1,  1273,    -1,    -1,
    -1,    -1,    -1,  1279,    -1,    -1,    -1,  1283,    -1,    -1,
    -1,    -1,    -1,    -1,   231,    -1,  1292,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1300,    -1,  1302,  1303,  1304,  1305,
    -1,   248,    -1,    -1,  1310,    -1,  1312,    -1,  1314,    -1,
    -1,    -1,    -1,  1319,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1327,  1328,  1329,    -1,    -1,    -1,    -1,    -1,   276,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   284,   285,   286,
   287,   288,   289,   290,   291,    -1,    -1,    -1,    -1,  1026,
  1027,  1028,  1029,    -1,    -1,  1361,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1053,    -1,    -1,  1385,
  1057,  1058,  1059,  1060,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1400,    -1,    -1,    -1,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,    -1,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1451,    -1,    -1,    -1,    -1,
  1127,  1128,  1129,  1130,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1470,    -1,    -1,    -1,  1145,    -1,
    -1,    76,    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    94,
    -1,    -1,    -1,  1499,    -1,   100,  1502,    -1,    -1,    -1,
    -1,  1507,    -1,    -1,  1510,   110,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,    -1,    -1,    -1,   124,
    -1,    -1,    -1,    -1,    -1,    59,    -1,    -1,    -1,    -1,
    -1,    65,   137,   138,    -1,   140,    70,   142,  1544,    -1,
    -1,    -1,    -1,   148,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1567,    -1,  1569,    -1,    -1,   171,    -1,    -1,    -1,
    -1,    -1,    -1,   178,   179,    -1,    -1,    -1,    -1,   113,
    -1,    -1,    -1,    -1,    -1,   119,    -1,    -1,   193,  1595,
    -1,  1597,   126,    -1,    -1,   200,    -1,  1603,   203,    -1,
   134,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   145,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   153,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   260,    -1,   262,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   274,
    -1,    -1,   277,    -1,    -1,  1352,  1353,  1683,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1362,  1363,  1364,  1365,  1366,
  1367,  1368,    -1,    -1,    -1,    -1,    -1,   231,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1388,    -1,    -1,   248,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1401,  1402,    -1,    -1,  1405,    -1,
  1407,    -1,    -1,  1410,  1411,  1412,  1413,  1414,  1415,  1416,
  1417,  1418,   276,    -1,  1421,    -1,    -1,    -1,    -1,    -1,
   284,   285,   286,   287,   288,   289,   290,   291,    -1,    -1,
    -1,    -1,    -1,  1440,    -1,    -1,  1443,    -1,    -1,  1775,
    -1,    -1,    -1,    -1,    -1,  1452,  1453,  1454,  1455,  1456,
  1457,  1458,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1813,    -1,     3,
     4,     5,     6,     7,     8,     9,    10,    11,  1824,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,  1837,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1867,  1868,    -1,    -1,    -1,    -1,  1873,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1884,    -1,
    -1,    -1,    76,    77,    -1,    -1,    -1,  1564,  1894,    -1,
    -1,    -1,  1898,    -1,    -1,    -1,    -1,    -1,    -1,    93,
    94,    59,    -1,  1909,    -1,    -1,   100,    65,    -1,    -1,
    -1,    -1,    70,    -1,  1920,    -1,   110,    -1,    -1,    -1,
    -1,    -1,    -1,  1929,   118,    -1,    59,    -1,    -1,    -1,
   124,    -1,    65,    -1,    67,    -1,    -1,    70,    -1,    -1,
  1946,    -1,    -1,   137,   138,    -1,   140,    -1,   142,    -1,
    -1,    -1,    -1,    -1,   148,    -1,    -1,    -1,    -1,    -1,
    -1,   119,    -1,    -1,    -1,    -1,    -1,    -1,   126,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   134,   171,    -1,    -1,
    -1,  1987,    -1,    -1,   178,   179,   119,   145,    -1,    -1,
    -1,    -1,    -1,   126,  2000,   153,    -1,    -1,    -1,   193,
    -1,   134,    -1,    -1,    -1,    -1,   200,    -1,    -1,   203,
    -1,    -1,   145,  1690,    -1,  1692,  2022,    -1,    59,   177,
   153,    -1,    -1,  2029,    65,    -1,    67,    -1,    -1,    70,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   563,    -1,    -1,  2053,    -1,  2055,
    59,    -1,    -1,    -1,    -1,    -1,    65,    -1,   577,    -1,
   579,    70,   581,   582,   583,    -1,    -1,    -1,   262,    -1,
    -1,    -1,    -1,   231,    -1,    -1,    -1,    -1,   119,    -1,
   274,    -1,    -1,   277,  2090,   126,    -1,    -1,    -1,    -1,
   248,    -1,    -1,   134,    -1,    -1,    -1,    -1,   231,  1776,
    -1,    -1,    -1,    -1,   145,    -1,    -1,    -1,    -1,    -1,
   119,    -1,   153,    -1,    -1,   248,  2122,   126,   276,    -1,
   639,    -1,    -1,    -1,    -1,   134,   284,   285,   286,   287,
   288,   289,   290,   291,    -1,    -1,   145,    -1,    -1,    -1,
    -1,    -1,    -1,   276,   153,    -1,    -1,    -1,    -1,    -1,
    -1,   284,   285,   286,   287,   288,   289,   290,   291,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2177,    -1,  2179,    -1,    -1,    -1,    -1,    -1,  1856,
    -1,  1858,  1859,  1860,  1861,  1862,    -1,    -1,    -1,    -1,
   231,    -1,    -1,    -1,    -1,    -1,  2202,    -1,    -1,    -1,
    -1,   720,  2208,    -1,    -1,    -1,    -1,   248,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1895,    -1,
    -1,    -1,   231,    -1,    -1,    -1,  2232,    -1,    -1,  2235,
   749,    -1,  1909,    -1,    -1,   276,    -1,    -1,    -1,   248,
    -1,  2247,    -1,   284,   285,   286,   287,   288,   289,   290,
   291,    -1,  2258,   772,    -1,  2261,  2262,  2263,    -1,  2265,
   779,    -1,    -1,  1940,    -1,    -1,    -1,   276,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   284,   285,   286,   287,   288,
   289,   290,   291,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  2302,  2303,    -1,    -1,
   819,    -1,    -1,    -1,  2310,    -1,    -1,    -1,  2314,    -1,
    -1,    -1,    -1,    -1,    -1,   834,    -1,    -1,    -1,   838,
    -1,   840,    -1,    -1,   843,   844,   845,   846,   847,   848,
   849,   850,   851,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   775,    -1,    -1,    -1,    -1,    -1,  2352,    -1,    -1,    -1,
  2356,    -1,  2358,    -1,    -1,    -1,    -1,    -1,    -1,  2365,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2046,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2054,    -1,    -1,
    -1,  2058,    -1,  2060,   819,    -1,  2063,  2064,  2065,  2066,
  2067,  2068,  2069,  2070,  2071,    -1,  2073,    -1,    -1,    -1,
   835,  2078,  2408,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  2092,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  2101,  2102,  2103,  2104,  2105,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    59,    -1,    -1,    -1,    -1,    -1,    65,    -1,    -1,
    -1,    -1,    70,    -1,    -1,    -1,    -1,    59,    -1,  2136,
    -1,    -1,    -1,    65,    -1,    -1,    -1,    -1,    70,    -1,
    -1,    -1,    -1,    -1,    -1,   994,    -1,   545,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   119,    -1,    -1,    -1,    -1,    -1,    -1,   126,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   134,   119,    -1,    -1,
  1039,    -1,  1041,  1042,   126,    -1,    -1,   145,    -1,    -1,
    -1,    -1,   134,  2210,  1053,   153,    -1,    -1,  1057,  1058,
  1059,  1060,  1061,   145,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   619,    -1,    -1,  1073,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  2239,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2248,  2249,  2250,  2251,  2252,  2253,  2254,  2255,  2256,
  2257,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1026,  1027,  1028,  1029,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1131,   231,    -1,    -1,    -1,    -1,  1053,    -1,
    -1,    -1,  1057,  1058,  1059,  1060,    -1,    -1,  1147,   231,
   248,    -1,  1151,    -1,    -1,  2312,    -1,  1156,    -1,    -1,
    -1,  1160,    -1,    -1,    -1,  1164,   248,    -1,    59,  1168,
    -1,    -1,    -1,  1172,    65,    -1,    -1,  1176,   276,    70,
    -1,  1180,    -1,    -1,    -1,  1184,   284,   285,   286,   287,
   288,   289,   290,   291,   276,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   284,   285,   286,   287,   288,   289,   290,   291,
    -1,  2368,  1127,  1128,  1129,  1130,    -1,    -1,    -1,    -1,
    -1,  1220,    -1,    -1,  1139,    -1,    -1,   775,   119,    -1,
  1145,    -1,    -1,    -1,    -1,   126,    -1,    -1,    59,    -1,
    -1,    -1,    -1,   134,    65,    -1,    -1,    -1,    -1,    70,
    -1,    -1,    -1,    -1,   145,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   153,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   819,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   835,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   119,    -1,
    -1,    -1,    -1,    -1,    -1,   126,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   134,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   145,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1331,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   231,  1340,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1349,    -1,    -1,    -1,    -1,    -1,  1355,   248,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1381,    -1,    -1,   276,    -1,    -1,    -1,    -1,
    -1,  1390,    -1,   284,   285,   286,   287,   288,   289,   290,
   291,    -1,  1401,    -1,    -1,    -1,  1405,    -1,  1407,    -1,
   231,  1410,  1411,  1412,  1413,  1414,  1415,  1416,  1417,  1418,
    -1,    -1,  1421,    -1,    -1,    -1,    -1,   248,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1352,  1353,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1445,  1362,  1363,  1364,
  1365,  1366,  1367,  1368,    -1,   276,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   284,   285,   286,   287,   288,   289,   290,
   291,    -1,    -1,  1388,    -1,    -1,    -1,    -1,  1026,  1027,
  1028,  1029,    -1,    -1,    -1,    -1,  1401,  1402,    -1,    -1,
  1405,    -1,  1407,    -1,    -1,  1410,  1411,  1412,  1413,  1414,
  1415,  1416,  1417,  1418,    -1,  1053,  1421,    -1,    -1,  1057,
  1058,  1059,  1060,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1440,    -1,    -1,  1443,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1452,  1453,  1454,
  1455,  1456,  1457,  1458,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1470,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1127,
  1128,  1129,  1130,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   759,    -1,    -1,    -1,    -1,    -1,  1145,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,  1645,  1646,  1647,  1564,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1206,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1674,  1675,  1676,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1692,    -1,    -1,    -1,    -1,  1697,    -1,
    -1,    -1,    -1,  1702,    -1,    -1,    -1,    -1,  1707,    -1,
    -1,    -1,    -1,  1712,    -1,    -1,    -1,    -1,  1717,    -1,
    -1,    -1,    -1,  1722,    -1,    -1,    -1,    -1,  1727,    -1,
    -1,    -1,    -1,  1732,    -1,    -1,    -1,    -1,  1737,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1749,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1759,  1760,  1761,    -1,    -1,    -1,    -1,    -1,  1683,    -1,
    -1,    -1,    -1,    -1,    -1,  1690,    -1,  1692,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1352,  1353,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1362,  1363,  1364,  1365,  1366,  1367,
  1368,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1388,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  1401,  1402,    -1,    -1,  1405,    -1,  1407,
  1775,  1776,  1410,  1411,  1412,  1413,  1414,  1415,  1416,  1417,
  1418,    -1,    -1,  1421,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,  1440,    -1,    -1,  1443,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,  1452,  1453,  1454,  1455,  1456,  1457,
  1458,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1856,    -1,  1858,  1859,  1860,  1861,  1862,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  1139,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
  1895,    -1,    -1,  1151,    -1,    -1,    -1,    -1,  1156,    -1,
    -1,    -1,  1160,    -1,    -1,    -1,  1164,    -1,    -1,    -1,
  1168,    -1,    -1,    -1,  1172,    -1,    -1,    -1,  1176,    -1,
    -1,    -1,  1180,    -1,    -1,    -1,  1184,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  1940,    -1,    -1,    -1,    -1,
    -1,  1946,    -1,  2032,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,  2000,    -1,    -1,     3,     4,
     5,     6,     7,     8,     9,    10,    11,    -1,    13,    -1,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,    34,
    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2046,    -1,  1301,    -1,    -1,    -1,    -1,    -1,  2054,
  2055,    -1,  1690,  2058,  1692,  2060,    -1,    -1,  2063,  2064,
  2065,  2066,  2067,  2068,  2069,  2070,  2071,    -1,  2073,    -1,
    -1,    76,    77,  2078,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2092,    93,    94,
    -1,    -1,    -1,    -1,    -1,   100,  2101,  2102,  2103,  2104,
  2105,    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   118,    -1,    -1,    -1,    -1,    -1,   124,
    -1,    -1,    -1,    -1,    -1,    -1,  1384,    -1,    -1,    -1,
    -1,  2136,   137,   138,    -1,   140,    -1,   142,  1776,    -1,
    -1,    -1,    -1,   148,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   171,   172,    -1,    -1,
    -1,    -1,    -1,   178,   179,    -1,    -1,    -1,    -1,    -1,
    -1,   186,    -1,    -1,    -1,    -1,    -1,    -1,   193,    -1,
    -1,    -1,    -1,    -1,    -1,   200,    -1,  2202,   203,    -1,
    -1,    -1,    -1,  2208,    -1,  2210,  2295,    -1,    -1,    -1,
    -1,    -1,  1470,    -1,  1472,  1473,    -1,  1475,  1476,    -1,
  1478,  1479,    -1,  1481,  1482,    -1,  1484,  1485,    -1,  1487,
  1488,    -1,  1490,  1491,  2239,  1493,  1494,    -1,  1496,  1497,
    -1,    -1,    -1,  2248,  2249,  2250,  2251,  2252,  2253,  2254,
  2255,  2256,  2257,    -1,    -1,    -1,    -1,   262,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   274,
   275,    -1,   277,    -1,    -1,   280,   281,   282,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  2303,    -1,
    -1,    -1,  1940,    -1,    -1,  2310,    -1,  2312,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,  2356,    -1,  2358,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,  2368,    -1,    -1,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    -1,    26,
    -1,    28,    29,    30,    31,    32,    -1,    34,    35,    36,
    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    60,    61,  1683,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1697,
    -1,    -1,    -1,    -1,  1702,    -1,    -1,    -1,    -1,  1707,
    87,    -1,    -1,    -1,  1712,    -1,    93,    -1,    -1,  1717,
    -1,    -1,    -1,   100,  1722,    -1,    -1,    -1,    -1,  1727,
    -1,    -1,    -1,    -1,  1732,    -1,    -1,    -1,    -1,  1737,
    -1,   118,    -1,    -1,   121,    -1,    -1,  1745,    -1,    -1,
    -1,  1749,   129,   130,    -1,    -1,    -1,    -1,  2136,   136,
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
   277,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   300,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,  1946,    -1,
    -1,  1949,  1950,    -1,  1952,  1953,    -1,  1955,  1956,    -1,
  1958,  1959,    -1,  1961,  1962,    -1,  1964,  1965,    -1,  1967,
  1968,    -1,  1970,  1971,    -1,  1973,  1974,    -1,    -1,    -1,
    -1,    -1,  1980,    -1,    -1,    -1,  1984,     3,     4,     5,
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
    -1,    -1,   288,   289,    -1,    -1,   292,    -1,   294,    -1,
    -1,    -1,    -1,    -1,   300,   301,     3,     4,     5,     6,
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
    -1,   288,   289,    -1,    -1,   292,    -1,   294,    -1,    -1,
    -1,    -1,    -1,   300,   301,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,    76,    77,
    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,
    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,
   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,   147,
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
   288,    -1,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,
    -1,    -1,   300,   301,     3,     4,     5,     6,     7,     8,
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
    -1,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,
    -1,   300,   301,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    75,    76,    77,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    88,    89,
    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,
   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
   140,    -1,   142,   143,    -1,    -1,    -1,    -1,   148,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,    -1,   186,    -1,    -1,    -1,
   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
   260,   261,   262,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,   275,   276,   277,    -1,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,   301,     3,     4,     5,     6,     7,     8,     9,    10,
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
    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,   300,
   301,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    76,    77,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   110,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,    -1,
    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,
   142,   143,    -1,    -1,    -1,    -1,   148,   149,    -1,   151,
   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,   161,
    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,
    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,    -1,    -1,   277,    -1,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,   301,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    71,    -1,    -1,    74,    75,
    76,    77,    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,
    -1,    87,    88,    89,    90,    91,    -1,    93,    94,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
   106,   107,   108,    -1,   110,    -1,    -1,   113,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,
    -1,    -1,    -1,   129,   130,   131,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,   145,
   146,   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,   183,    -1,   185,
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
    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,   294,    -1,
    -1,    -1,    -1,    -1,   300,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
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
   288,    -1,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,
    -1,    -1,   300,     3,     4,     5,     6,     7,     8,     9,
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
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    71,
    -1,    -1,    74,    75,    76,    77,    -1,    -1,    80,    -1,
    -1,    -1,    -1,    -1,    -1,    87,    88,    89,    90,    91,
    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,
    -1,    -1,    -1,    -1,   106,   107,   108,    -1,   110,    -1,
    -1,   113,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,
    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,   131,
    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,
   142,   143,    -1,   145,   146,   147,   148,   149,    -1,   151,
   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,
    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,
    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,   183,    -1,   185,   186,    -1,    -1,    -1,   190,    -1,
    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,   275,   276,   277,   278,    -1,   280,   281,
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,
   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,   300,     3,
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
    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,
   294,    -1,    -1,    -1,    -1,    -1,   300,     3,     4,     5,
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
    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,   294,    -1,
    -1,    -1,    -1,    -1,   300,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    74,    75,    76,    77,
    -1,    -1,    80,    -1,    -1,    -1,    -1,    -1,    -1,    87,
    88,    89,    90,    91,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,   106,   107,
   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,   140,    -1,   142,   143,    -1,   145,   146,   147,
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
   288,   289,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,
    -1,    -1,   300,     3,     4,     5,     6,     7,     8,     9,
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
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,     3,     4,     5,     6,     7,     8,     9,    10,    11,
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
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,
   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,   300,     3,
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
    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,
   294,    -1,    -1,    -1,    -1,    -1,   300,     3,     4,     5,
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
    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,   294,    -1,
    -1,    -1,    -1,    -1,   300,     3,     4,     5,     6,     7,
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
   288,    -1,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,
    -1,    -1,   300,     3,     4,     5,     6,     7,     8,     9,
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
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,     3,     4,     5,     6,     7,     8,     9,    10,    11,
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
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,
   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,   300,     3,
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
    -1,    -1,   106,   107,   108,    -1,   110,    -1,    -1,   113,
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
    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,
   294,    -1,    -1,    -1,    -1,    -1,   300,     3,     4,     5,
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
   146,   147,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
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
    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,   294,    -1,
    -1,    -1,    -1,    -1,   300,     3,     4,     5,     6,     7,
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
   138,    -1,   140,    -1,   142,   143,    -1,    -1,   146,    -1,
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
   268,    -1,    -1,   271,    -1,   273,   274,   275,    -1,   277,
   278,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,
   288,    -1,    -1,    -1,   292,   293,   294,    -1,    -1,    -1,
   298,    -1,   300,     3,     4,     5,     6,     7,     8,     9,
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
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,     3,     4,     5,     6,     7,     8,     9,    10,    11,
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
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,
   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,   300,     3,
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
    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,
   294,    -1,    -1,    -1,    -1,    -1,   300,     3,     4,     5,
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
    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,   294,    -1,
    -1,    -1,    -1,    -1,   300,     3,     4,     5,     6,     7,
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
   288,    -1,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,
    -1,    -1,   300,     3,     4,     5,     6,     7,     8,     9,
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
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,     3,     4,     5,     6,     7,     8,     9,    10,    11,
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
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,
   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,   300,     3,
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
    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,
   294,    -1,    -1,    -1,    -1,    -1,   300,     3,     4,     5,
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
    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,   294,    -1,
    -1,    -1,    -1,    -1,   300,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
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
   288,    -1,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,
    -1,    -1,   300,     3,     4,     5,     6,     7,     8,     9,
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
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,     3,     4,     5,     6,     7,     8,     9,    10,    11,
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
   142,   143,    -1,    -1,   146,    -1,   148,   149,    -1,   151,
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
    -1,   273,   274,   275,    -1,   277,   278,    -1,   280,   281,
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,
   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,   300,     3,
     4,     5,     6,     7,     8,     9,    10,    11,    -1,    13,
    -1,    15,    16,    17,    18,    19,    20,    21,    22,    23,
    24,    -1,    26,    -1,    28,    29,    30,    31,    32,    -1,
    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    75,    76,    77,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    87,    88,    89,    90,    91,    -1,    93,
    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,
   124,    -1,    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,
    -1,    -1,   136,   137,   138,    -1,   140,    -1,   142,   143,
    -1,    -1,    -1,    -1,   148,   149,    -1,   151,   152,    -1,
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
    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,
   294,    -1,    -1,    -1,    -1,    -1,   300,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,
    76,    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    87,    88,    89,    90,    91,    -1,    93,    94,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,    -1,
    -1,    -1,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
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
    -1,   277,   278,    -1,   280,   281,   282,    -1,    -1,    -1,
    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,   294,    -1,
    -1,    -1,    -1,    -1,   300,     3,     4,     5,     6,     7,
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
   138,    -1,   140,    -1,   142,   143,    -1,    -1,    -1,   147,
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
   288,    -1,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,
    -1,    -1,   300,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    75,    76,    77,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    88,    89,
    90,    91,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,
   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,
   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,
    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,
   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,
   140,    -1,   142,   143,    -1,    -1,    -1,    -1,   148,   149,
    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   160,   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,
   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,
   180,   181,    -1,    -1,    -1,    -1,   186,    -1,    -1,    -1,
   190,    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,
   200,   201,   202,   203,    -1,   205,   206,    -1,   208,   209,
    -1,   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,
   220,   221,    -1,    -1,   224,   225,   226,   227,   228,   229,
   230,   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,
   240,    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
   260,   261,   262,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,   275,   276,   277,    -1,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    -1,    26,    -1,    28,    29,    30,    31,
    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    75,    76,    77,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    87,    -1,    -1,    -1,    -1,
    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   108,    -1,   110,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,   121,
    -1,    -1,   124,    -1,    -1,    -1,    -1,   129,   130,    -1,
    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,    -1,
   142,   143,    -1,    -1,    -1,    -1,   148,   149,    -1,   151,
   152,    -1,    -1,    -1,    -1,   157,    -1,    -1,   160,   161,
    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,   171,
    -1,    -1,   174,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,    -1,    -1,   185,   186,    -1,    -1,    -1,    -1,    -1,
    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
   242,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
   262,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,   275,   276,   277,    -1,    -1,   280,   281,
   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,
   292,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   300,     3,
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
   274,   275,    -1,   277,    -1,    -1,   280,   281,   282,    -1,
    -1,    -1,    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,
   294,    -1,    -1,    -1,    -1,    -1,   300,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,
    76,    77,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,   124,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,   140,    -1,   142,   143,    -1,    -1,
    -1,    -1,   148,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,   157,    -1,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,   174,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,   185,
   186,    -1,    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,   242,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
    -1,   257,   258,   259,   260,   261,   262,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,
    -1,   277,    -1,    -1,   280,   281,   282,    -1,    -1,    -1,
    -1,    -1,   288,    -1,    -1,    -1,   292,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   300,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,    -1,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    93,    94,    -1,    -1,    -1,
    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   108,    -1,   110,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   118,    -1,    -1,   121,    -1,    -1,   124,    -1,    -1,    -1,
    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,
   138,    -1,   140,    -1,   142,   143,    -1,   145,    -1,   147,
   148,   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   160,   161,    -1,    -1,   164,    -1,   166,    -1,
    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,    -1,
   178,   179,   180,   181,    -1,    -1,    -1,    -1,   186,    -1,
    -1,    -1,    -1,    -1,    -1,   193,    -1,    -1,    -1,    -1,
    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,    -1,
   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,   217,
   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,   227,
   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,   237,
    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,    -1,
   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,   257,
   258,   259,   260,   261,   262,    -1,   264,   265,   266,   267,
   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,   277,
    -1,    -1,   280,   281,   282,    -1,    -1,    -1,    -1,    -1,
   288,    -1,    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,
    -1,    -1,   300,     3,     4,     5,     6,     7,     8,     9,
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
   140,    -1,   142,   143,    -1,    -1,    -1,   147,   148,   149,
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
    -1,   271,    -1,   273,   274,   275,    -1,   277,    -1,    -1,
   280,   281,   282,    -1,    -1,    -1,    -1,    -1,   288,    -1,
    -1,    -1,   292,    -1,   294,    -1,    -1,    -1,    -1,    -1,
   300,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    -1,    13,    -1,    15,    16,    17,    18,    19,    20,    21,
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
   172,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,   181,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,   201,
   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,   211,
    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,   221,
    -1,    -1,   224,   225,   226,   227,   228,   229,   230,   231,
   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,    -1,
    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,   251,
   252,   253,    -1,   255,    -1,   257,   258,   259,   260,   261,
    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,    -1,    -1,   277,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    -1,    13,    -1,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,   300,    26,
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
    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,    -1,
    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,   206,
    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,   216,
   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,   226,
   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,    -1,
   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,   246,
    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,    -1,
   257,   258,   259,   260,   261,   262,    -1,   264,   265,   266,
   267,   268,    -1,    -1,   271,    -1,   273,   274,   275,   276,
   277,    -1,    -1,   280,    -1,   282,    -1,   284,   285,   286,
   287,   288,   289,   290,   291,     3,     4,     5,     6,     7,
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
   288,   289,   290,   291,     3,     4,     5,     6,     7,     8,
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
    -1,    -1,   121,    -1,    -1,    -1,    -1,    -1,    -1,   128,
   129,   130,    -1,    -1,    -1,    -1,    -1,   136,   137,   138,
    -1,    -1,    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,
   149,    -1,   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,   160,   161,   162,    -1,   164,    -1,   166,    -1,    -1,
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
    -1,    -1,   271,    -1,   273,   274,    -1,   276,   277,    -1,
    -1,    -1,    -1,    -1,    -1,   284,   285,   286,   287,   288,
   289,   290,   291,     3,     4,     5,     6,     7,     8,     9,
    10,    11,    -1,    13,    -1,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    -1,    26,    -1,    28,    29,
    30,    31,    32,    -1,    34,    35,    36,    37,    38,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    60,    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    87,    -1,    -1,
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
   240,    -1,   242,   243,   244,    -1,   246,    -1,   248,    -1,
   250,   251,   252,   253,    -1,   255,    -1,   257,   258,   259,
   260,   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,
    -1,   271,    -1,   273,   274,    -1,    -1,   277,    -1,    -1,
    -1,   281,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   289,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    -1,
    13,    -1,    15,    16,    17,    18,    19,    20,    21,    22,
    23,    24,    -1,    26,    -1,    28,    29,    30,    31,    32,
    -1,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
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
   273,   274,    -1,    -1,   277,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    -1,    13,   289,    15,    16,    17,
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
    13,   289,    15,    16,    17,    18,    19,    20,    21,    22,
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
     8,     9,    10,    11,    -1,    13,   289,    15,    16,    17,
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
    13,   289,    15,    16,    17,    18,    19,    20,    21,    22,
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
     8,     9,    10,    11,    -1,    13,   289,    15,    16,    17,
    18,    19,    20,    21,    22,    23,    24,    -1,    26,    -1,
    28,    29,    30,    31,    32,    -1,    34,    35,    36,    37,
    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    76,    77,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
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
    -1,   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,
   255,    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,
   265,   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,
    -1,    -1,   277,    -1,    -1,    -1,   281,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    63,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,
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
    -1,    -1,    -1,    93,    -1,    -1,    96,    -1,    -1,    -1,
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
    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,   271,
    -1,   273,   274,    -1,    -1,   277,   278,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    74,    -1,
    -1,    -1,    -1,    -1,    80,    -1,    -1,    -1,    84,    -1,
    -1,    87,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,
    -1,    -1,    -1,    -1,   100,    -1,   102,   103,    -1,    -1,
    -1,    -1,   108,    -1,    -1,    -1,   112,    -1,    -1,    -1,
   116,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,
   146,    -1,    -1,   149,    -1,   151,   152,    -1,   154,    -1,
    -1,   157,   158,    -1,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,   175,
    -1,   177,   178,   179,   180,   181,    -1,    -1,   184,    -1,
   186,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
   196,    -1,    -1,    -1,   200,   201,   202,   203,   204,   205,
   206,   207,   208,   209,   210,   211,   212,   213,   214,   215,
   216,   217,   218,   219,   220,   221,   222,   223,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,   234,    -1,
   236,   237,   238,   239,   240,   241,   242,   243,   244,   245,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
   256,   257,   258,   259,   260,   261,   262,   263,   264,   265,
   266,   267,   268,    -1,   270,   271,   272,   273,   274,    -1,
    -1,   277,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    76,    77,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    93,    94,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   110,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,   118,    -1,    -1,
   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,   140,
    -1,   142,   143,    -1,    -1,    -1,    -1,   148,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,   193,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    78,    -1,    -1,    -1,    -1,    -1,    84,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    93,    -1,    -1,
    -1,    -1,    -1,    -1,   100,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,   112,    -1,    -1,    -1,
    -1,    -1,   118,    -1,    -1,   121,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,   129,   130,    -1,    -1,    -1,    -1,    -1,
   136,   137,   138,    -1,    -1,    -1,    -1,   143,    -1,    -1,
    -1,    -1,    -1,   149,    -1,   151,   152,    -1,    -1,    -1,
    -1,    -1,    -1,   159,   160,   161,    -1,    -1,   164,    -1,
   166,    -1,    -1,    -1,   170,   171,    -1,    -1,    -1,    -1,
    -1,    -1,   178,   179,   180,   181,    -1,    -1,    -1,    -1,
    -1,    -1,   188,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,   200,   201,   202,   203,    -1,   205,
   206,    -1,   208,   209,    -1,   211,    -1,    -1,   214,   215,
   216,   217,   218,    -1,   220,   221,    -1,    -1,   224,   225,
   226,   227,   228,   229,   230,   231,   232,    -1,    -1,    -1,
    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,    -1,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
    -1,   277,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
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
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,   139,    -1,
    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,
   161,    -1,    -1,   164,    -1,   166,    -1,    -1,    -1,   170,
   171,    -1,    -1,    -1,    -1,    -1,    -1,   178,   179,   180,
   181,    -1,    -1,   184,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,     5,
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
    -1,   277,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    -1,    26,    -1,    28,    29,    30,
    31,    32,    -1,    34,    35,    36,    37,    38,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    60,
    61,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    93,    -1,    -1,    -1,    -1,    -1,    -1,   100,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,   116,    -1,   118,    -1,    -1,
   121,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   129,   130,
    -1,    -1,    -1,    -1,    -1,   136,   137,   138,    -1,    -1,
    -1,    -1,   143,    -1,    -1,    -1,    -1,    -1,   149,    -1,
   151,   152,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   160,
   161,    -1,   163,   164,    -1,   166,    -1,    -1,    -1,   170,
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
    -1,    -1,   178,   179,   180,   181,    -1,    -1,   184,    -1,
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
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
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
   181,    -1,    -1,   184,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   200,
   201,   202,   203,    -1,   205,   206,    -1,   208,   209,    -1,
   211,    -1,    -1,   214,   215,   216,   217,   218,    -1,   220,
   221,    -1,    -1,   224,   225,   226,   227,   228,   229,   230,
   231,   232,    -1,    -1,    -1,    -1,   237,    -1,   239,   240,
    -1,    -1,   243,   244,    -1,   246,    -1,   248,    -1,   250,
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,     5,
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
    -1,   237,    -1,   239,   240,    -1,    -1,   243,   244,   245,
   246,    -1,   248,    -1,   250,   251,   252,   253,    -1,   255,
    -1,   257,   258,   259,   260,   261,    -1,    -1,   264,   265,
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
    -1,   277,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
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
    -1,    -1,   243,   244,   245,   246,    -1,   248,    -1,   250,
   251,   252,   253,    -1,   255,    -1,   257,   258,   259,   260,
   261,    -1,    -1,   264,   265,   266,   267,   268,    -1,    -1,
   271,    -1,   273,   274,    -1,    -1,   277,     3,     4,     5,
     6,     7,     8,     9,    10,    11,    -1,    13,    -1,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    -1,
    26,    -1,    28,    29,    30,    31,    32,    -1,    34,    35,
    36,    37,    38,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    60,    61,    -1,    -1,    -1,    -1,
    -1,    67,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
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
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
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
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
    -1,   277,     3,     4,     5,     6,     7,     8,     9,    10,
    11,    -1,    13,    -1,    15,    16,    17,    18,    19,    20,
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
   266,   267,   268,    -1,    -1,   271,    -1,   273,   274,    -1,
    -1,   277
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
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("%"), yyvsp[0].str); ;
    break;}
case 155:
#line 1426 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
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
#line 1432 "preproc.y"
{	yyerror("boolean expressions not supported in DEFAULT"); ;
    break;}
case 159:
#line 1438 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 160:
#line 1440 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 161:
#line 1442 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str); ;
    break;}
case 162:
#line 1444 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str) , make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 163:
#line 1448 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 164:
#line 1450 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); ;
    break;}
case 165:
#line 1452 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); ;
    break;}
case 166:
#line 1454 "preproc.y"
{
					if (!strcmp("<=", yyvsp[-1].str) || !strcmp(">=", yyvsp[-1].str))
						yyerror("boolean expressions not supported in DEFAULT");
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 167:
#line 1460 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 168:
#line 1462 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 169:
#line 1465 "preproc.y"
{	yyval.str = make1_str("current_date"); ;
    break;}
case 170:
#line 1467 "preproc.y"
{	yyval.str = make1_str("current_time"); ;
    break;}
case 171:
#line 1469 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr, "CURRENT_TIME(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = "current_time";
				;
    break;}
case 172:
#line 1475 "preproc.y"
{	yyval.str = make1_str("current_timestamp"); ;
    break;}
case 173:
#line 1477 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr, "CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = "current_timestamp";
				;
    break;}
case 174:
#line 1483 "preproc.y"
{	yyval.str = make1_str("current_user"); ;
    break;}
case 175:
#line 1485 "preproc.y"
{       yyval.str = make1_str("user"); ;
    break;}
case 176:
#line 1493 "preproc.y"
{
						yyval.str = cat3_str(make1_str("constraint"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 177:
#line 1497 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 178:
#line 1501 "preproc.y"
{
					yyval.str = make3_str(make1_str("check("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 179:
#line 1505 "preproc.y"
{
					yyval.str = make3_str(make1_str("unique("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 180:
#line 1509 "preproc.y"
{
					yyval.str = make3_str(make1_str("primary key("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 181:
#line 1513 "preproc.y"
{
					fprintf(stderr, "CREATE TABLE/FOREIGN KEY clause ignored; not yet implemented");
					yyval.str = "";
				;
    break;}
case 182:
#line 1520 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 183:
#line 1524 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 184:
#line 1530 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 185:
#line 1532 "preproc.y"
{	yyval.str = make1_str("null"); ;
    break;}
case 186:
#line 1534 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 187:
#line 1538 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 188:
#line 1540 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 189:
#line 1542 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 190:
#line 1544 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 191:
#line 1546 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("%"), yyvsp[0].str); ;
    break;}
case 192:
#line 1548 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 193:
#line 1550 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 194:
#line 1552 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 195:
#line 1554 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 196:
#line 1560 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 197:
#line 1562 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 198:
#line 1564 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 199:
#line 1568 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 200:
#line 1572 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 201:
#line 1574 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); }
				;
    break;}
case 202:
#line 1578 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 203:
#line 1582 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 204:
#line 1584 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 205:
#line 1586 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 206:
#line 1588 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 207:
#line 1590 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 208:
#line 1592 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 209:
#line 1594 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 210:
#line 1596 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 211:
#line 1598 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 212:
#line 1600 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 213:
#line 1602 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 214:
#line 1604 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 215:
#line 1606 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); ;
    break;}
case 216:
#line 1608 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); ;
    break;}
case 217:
#line 1610 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); ;
    break;}
case 218:
#line 1612 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); ;
    break;}
case 219:
#line 1614 "preproc.y"
{	yyval.str = cat4_str(yyvsp[-4].str, make1_str("in ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 220:
#line 1616 "preproc.y"
{	yyval.str = cat4_str(yyvsp[-5].str, make1_str("not in ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 221:
#line 1618 "preproc.y"
{	yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 222:
#line 1620 "preproc.y"
{	yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 223:
#line 1623 "preproc.y"
{
		yyval.str = make3_str(yyvsp[-2].str, make1_str(", "), yyvsp[0].str);
	;
    break;}
case 224:
#line 1627 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 225:
#line 1632 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 226:
#line 1636 "preproc.y"
{ yyval.str = make1_str("match full"); ;
    break;}
case 227:
#line 1637 "preproc.y"
{ yyval.str = make1_str("match partial"); ;
    break;}
case 228:
#line 1638 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 229:
#line 1641 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 230:
#line 1642 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 231:
#line 1643 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 232:
#line 1646 "preproc.y"
{ yyval.str = cat2_str(make1_str("on delete"), yyvsp[0].str); ;
    break;}
case 233:
#line 1647 "preproc.y"
{ yyval.str = cat2_str(make1_str("on update"), yyvsp[0].str); ;
    break;}
case 234:
#line 1650 "preproc.y"
{ yyval.str = make1_str("no action"); ;
    break;}
case 235:
#line 1651 "preproc.y"
{ yyval.str = make1_str("cascade"); ;
    break;}
case 236:
#line 1652 "preproc.y"
{ yyval.str = make1_str("set default"); ;
    break;}
case 237:
#line 1653 "preproc.y"
{ yyval.str = make1_str("set null"); ;
    break;}
case 238:
#line 1656 "preproc.y"
{ yyval.str = make3_str(make1_str("inherits ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 239:
#line 1657 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 240:
#line 1661 "preproc.y"
{
			yyval.str = cat5_str(cat3_str(make1_str("create"), yyvsp[-5].str, make1_str("table")), yyvsp[-3].str, yyvsp[-2].str, make1_str("as"), yyvsp[0].str); 
		;
    break;}
case 241:
#line 1666 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 242:
#line 1667 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 243:
#line 1670 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 244:
#line 1671 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 245:
#line 1674 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 246:
#line 1685 "preproc.y"
{
					yyval.str = cat3_str(make1_str("create sequence"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 247:
#line 1691 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 248:
#line 1692 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 249:
#line 1696 "preproc.y"
{
					yyval.str = cat2_str(make1_str("cache"), yyvsp[0].str);
				;
    break;}
case 250:
#line 1700 "preproc.y"
{
					yyval.str = make1_str("cycle");
				;
    break;}
case 251:
#line 1704 "preproc.y"
{
					yyval.str = cat2_str(make1_str("increment"), yyvsp[0].str);
				;
    break;}
case 252:
#line 1708 "preproc.y"
{
					yyval.str = cat2_str(make1_str("maxvalue"), yyvsp[0].str);
				;
    break;}
case 253:
#line 1712 "preproc.y"
{
					yyval.str = cat2_str(make1_str("minvalue"), yyvsp[0].str);
				;
    break;}
case 254:
#line 1716 "preproc.y"
{
					yyval.str = cat2_str(make1_str("start"), yyvsp[0].str);
				;
    break;}
case 255:
#line 1721 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 256:
#line 1722 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 257:
#line 1725 "preproc.y"
{
                                       yyval.str = yyvsp[0].str;
                               ;
    break;}
case 258:
#line 1729 "preproc.y"
{
                                       yyval.str = cat2_str(make1_str("-"), yyvsp[0].str);
                               ;
    break;}
case 259:
#line 1736 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 260:
#line 1740 "preproc.y"
{
					yyval.str = cat2_str(make1_str("-"), yyvsp[0].str);
				;
    break;}
case 261:
#line 1755 "preproc.y"
{
				yyval.str = cat4_str(cat5_str(make1_str("create"), yyvsp[-7].str, make1_str("precedural language"), yyvsp[-4].str, make1_str("handler")), yyvsp[-2].str, make1_str("langcompiler"), yyvsp[0].str);
			;
    break;}
case 262:
#line 1760 "preproc.y"
{ yyval.str = make1_str("trusted"); ;
    break;}
case 263:
#line 1761 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 264:
#line 1764 "preproc.y"
{
				yyval.str = cat2_str(make1_str("drop procedural language"), yyvsp[0].str);
			;
    break;}
case 265:
#line 1780 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create trigger"), yyvsp[-11].str, yyvsp[-10].str, yyvsp[-9].str, make1_str("on")), yyvsp[-7].str, yyvsp[-6].str, make1_str("execute procedure"), yyvsp[-3].str), make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 266:
#line 1785 "preproc.y"
{ yyval.str = make1_str("before"); ;
    break;}
case 267:
#line 1786 "preproc.y"
{ yyval.str = make1_str("after"); ;
    break;}
case 268:
#line 1790 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 269:
#line 1794 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str);
				;
    break;}
case 270:
#line 1798 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("or"), yyvsp[-2].str, make1_str("or"), yyvsp[0].str);
				;
    break;}
case 271:
#line 1803 "preproc.y"
{ yyval.str = make1_str("insert"); ;
    break;}
case 272:
#line 1804 "preproc.y"
{ yyval.str = make1_str("delete"); ;
    break;}
case 273:
#line 1805 "preproc.y"
{ yyval.str = make1_str("update"); ;
    break;}
case 274:
#line 1809 "preproc.y"
{
					yyval.str = cat3_str(make1_str("for"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 275:
#line 1814 "preproc.y"
{ yyval.str = make1_str("each"); ;
    break;}
case 276:
#line 1815 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 277:
#line 1818 "preproc.y"
{ yyval.str = make1_str("row"); ;
    break;}
case 278:
#line 1819 "preproc.y"
{ yyval.str = make1_str("statement"); ;
    break;}
case 279:
#line 1823 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 280:
#line 1825 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 281:
#line 1827 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 282:
#line 1831 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 283:
#line 1835 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 284:
#line 1838 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 285:
#line 1839 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 286:
#line 1843 "preproc.y"
{
					yyval.str = cat4_str(make1_str("drop trigger"), yyvsp[-2].str, make1_str("on"), yyvsp[0].str);
				;
    break;}
case 287:
#line 1856 "preproc.y"
{
					yyval.str = cat3_str(make1_str("create"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 288:
#line 1862 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 289:
#line 1867 "preproc.y"
{ yyval.str = make1_str("operator"); ;
    break;}
case 290:
#line 1868 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 291:
#line 1869 "preproc.y"
{ yyval.str = make1_str("aggregate"); ;
    break;}
case 292:
#line 1872 "preproc.y"
{ yyval.str = make1_str("procedure"); ;
    break;}
case 293:
#line 1873 "preproc.y"
{ yyval.str = make1_str("join"); ;
    break;}
case 294:
#line 1874 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 295:
#line 1875 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 296:
#line 1876 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 297:
#line 1879 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 298:
#line 1882 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 299:
#line 1883 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 300:
#line 1886 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 301:
#line 1890 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 302:
#line 1894 "preproc.y"
{
					yyval.str = cat2_str(make1_str("default ="), yyvsp[0].str);
				;
    break;}
case 303:
#line 1899 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 304:
#line 1900 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 305:
#line 1901 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 306:
#line 1902 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 307:
#line 1904 "preproc.y"
{
					yyval.str = cat2_str(make1_str("setof"), yyvsp[0].str);
				;
    break;}
case 308:
#line 1917 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop table"), yyvsp[0].str);
				;
    break;}
case 309:
#line 1921 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop sequence"), yyvsp[0].str);
				;
    break;}
case 310:
#line 1938 "preproc.y"
{
					if (strncmp(yyvsp[-4].str, "relative", strlen("relative")) == 0 && atol(yyvsp[-3].str) == 0L)
						yyerror("FETCH/RELATIVE at current position is not supported");

					yyval.str = cat4_str(make1_str("fetch"), yyvsp[-4].str, yyvsp[-3].str, yyvsp[-2].str);
				;
    break;}
case 311:
#line 1945 "preproc.y"
{
					yyval.str = cat4_str(make1_str("fetch"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 312:
#line 1950 "preproc.y"
{ yyval.str = make1_str("forward"); ;
    break;}
case 313:
#line 1951 "preproc.y"
{ yyval.str = make1_str("backward"); ;
    break;}
case 314:
#line 1952 "preproc.y"
{ yyval.str = make1_str("relative"); ;
    break;}
case 315:
#line 1954 "preproc.y"
{
					fprintf(stderr, "FETCH/ABSOLUTE not supported, using RELATIVE");
					yyval.str = make1_str("absolute");
				;
    break;}
case 316:
#line 1958 "preproc.y"
{ yyval.str = make1_str(""); /* default */ ;
    break;}
case 317:
#line 1961 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 318:
#line 1962 "preproc.y"
{ yyval.str = make2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 319:
#line 1963 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 320:
#line 1964 "preproc.y"
{ yyval.str = make1_str("next"); ;
    break;}
case 321:
#line 1965 "preproc.y"
{ yyval.str = make1_str("prior"); ;
    break;}
case 322:
#line 1966 "preproc.y"
{ yyval.str = make1_str(""); /*default*/ ;
    break;}
case 323:
#line 1969 "preproc.y"
{ yyval.str = cat2_str(make1_str("in"), yyvsp[0].str); ;
    break;}
case 324:
#line 1970 "preproc.y"
{ yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 325:
#line 1972 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 326:
#line 1984 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("grant"), yyvsp[-5].str, make1_str("on"), yyvsp[-3].str, make1_str("to")), yyvsp[-1].str);
				;
    break;}
case 327:
#line 1990 "preproc.y"
{
				 yyval.str = make1_str("all privileges");
				;
    break;}
case 328:
#line 1994 "preproc.y"
{
				 yyval.str = make1_str("all");
				;
    break;}
case 329:
#line 1998 "preproc.y"
{
				 yyval.str = yyvsp[0].str;
				;
    break;}
case 330:
#line 2004 "preproc.y"
{
						yyval.str = yyvsp[0].str;
				;
    break;}
case 331:
#line 2008 "preproc.y"
{
						yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 332:
#line 2014 "preproc.y"
{
						yyval.str = make1_str("select");
				;
    break;}
case 333:
#line 2018 "preproc.y"
{
						yyval.str = make1_str("insert");
				;
    break;}
case 334:
#line 2022 "preproc.y"
{
						yyval.str = make1_str("update");
				;
    break;}
case 335:
#line 2026 "preproc.y"
{
						yyval.str = make1_str("delete");
				;
    break;}
case 336:
#line 2030 "preproc.y"
{
						yyval.str = make1_str("rule");
				;
    break;}
case 337:
#line 2036 "preproc.y"
{
						yyval.str = make1_str("public");
				;
    break;}
case 338:
#line 2040 "preproc.y"
{
						yyval.str = cat2_str(make1_str("group"), yyvsp[0].str);
				;
    break;}
case 339:
#line 2044 "preproc.y"
{
						yyval.str = yyvsp[0].str;
				;
    break;}
case 340:
#line 2050 "preproc.y"
{
					yyerror("WITH GRANT OPTION is not supported.  Only relation owners can set privileges");
				 ;
    break;}
case 342:
#line 2065 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("revoke"), yyvsp[-4].str, make1_str("on"), yyvsp[-2].str, make1_str("from")), yyvsp[0].str);
				;
    break;}
case 343:
#line 2084 "preproc.y"
{
					/* should check that access_method is valid,
					   etc ... but doesn't */
					yyval.str = cat5_str(cat5_str(make1_str("create"), yyvsp[-9].str, make1_str("index"), yyvsp[-7].str, make1_str("on")), yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("("), yyvsp[-2].str, make1_str(")")), yyvsp[0].str);
				;
    break;}
case 344:
#line 2091 "preproc.y"
{ yyval.str = make1_str("unique"); ;
    break;}
case 345:
#line 2092 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 346:
#line 2095 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 347:
#line 2096 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 348:
#line 2099 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 349:
#line 2100 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 350:
#line 2103 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 351:
#line 2104 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 352:
#line 2108 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-5].str, make3_str(make1_str("("), yyvsp[-3].str, ")"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 353:
#line 2114 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 354:
#line 2119 "preproc.y"
{ yyval.str = cat2_str(make1_str(":"), yyvsp[0].str); ;
    break;}
case 355:
#line 2120 "preproc.y"
{ yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 356:
#line 2121 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 357:
#line 2130 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 358:
#line 2131 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 359:
#line 2132 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 360:
#line 2143 "preproc.y"
{
					yyval.str = cat3_str(make1_str("extend index"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 361:
#line 2180 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create function"), yyvsp[-8].str, yyvsp[-7].str, make1_str("returns"), yyvsp[-5].str), yyvsp[-4].str, make1_str("as"), yyvsp[-2].str, make1_str("language")), yyvsp[0].str);
				;
    break;}
case 362:
#line 2184 "preproc.y"
{ yyval.str = cat2_str(make1_str("with"), yyvsp[0].str); ;
    break;}
case 363:
#line 2185 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 364:
#line 2188 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 365:
#line 2189 "preproc.y"
{ yyval.str = make1_str("()"); ;
    break;}
case 366:
#line 2192 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 367:
#line 2194 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 368:
#line 2198 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 369:
#line 2203 "preproc.y"
{ yyval.str = make1_str("setof"); ;
    break;}
case 370:
#line 2204 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 371:
#line 2226 "preproc.y"
{
					yyval.str = cat3_str(make1_str("drop"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 372:
#line 2231 "preproc.y"
{  yyval.str = make1_str("type"); ;
    break;}
case 373:
#line 2232 "preproc.y"
{  yyval.str = make1_str("index"); ;
    break;}
case 374:
#line 2233 "preproc.y"
{  yyval.str = make1_str("rule"); ;
    break;}
case 375:
#line 2234 "preproc.y"
{  yyval.str = make1_str("view"); ;
    break;}
case 376:
#line 2239 "preproc.y"
{
						yyval.str = cat3_str(make1_str("drop aggregate"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 377:
#line 2244 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 378:
#line 2245 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 379:
#line 2250 "preproc.y"
{
						yyval.str = cat3_str(make1_str("drop function"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 380:
#line 2257 "preproc.y"
{
					yyval.str = cat3_str(make1_str("drop operator"), yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 383:
#line 2264 "preproc.y"
{ yyval.str = make1_str("+"); ;
    break;}
case 384:
#line 2265 "preproc.y"
{ yyval.str = make1_str("-"); ;
    break;}
case 385:
#line 2266 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 386:
#line 2267 "preproc.y"
{ yyval.str = make1_str("%"); ;
    break;}
case 387:
#line 2268 "preproc.y"
{ yyval.str = make1_str("/"); ;
    break;}
case 388:
#line 2269 "preproc.y"
{ yyval.str = make1_str("<"); ;
    break;}
case 389:
#line 2270 "preproc.y"
{ yyval.str = make1_str(">"); ;
    break;}
case 390:
#line 2271 "preproc.y"
{ yyval.str = make1_str("="); ;
    break;}
case 391:
#line 2275 "preproc.y"
{
				   yyerror("parser: argument type missing (use NONE for unary operators)");
				;
    break;}
case 392:
#line 2279 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 393:
#line 2281 "preproc.y"
{ yyval.str = cat2_str(make1_str("none,"), yyvsp[0].str); ;
    break;}
case 394:
#line 2283 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-2].str, make1_str(", none")); ;
    break;}
case 395:
#line 2297 "preproc.y"
{
					yyval.str = cat4_str(cat5_str(make1_str("alter table"), yyvsp[-6].str, yyvsp[-5].str, make1_str("rename"), yyvsp[-3].str), yyvsp[-2].str, make1_str("to"), yyvsp[0].str);
				;
    break;}
case 396:
#line 2302 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 397:
#line 2303 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 398:
#line 2306 "preproc.y"
{ yyval.str = make1_str("colmunn"); ;
    break;}
case 399:
#line 2307 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 400:
#line 2321 "preproc.y"
{ QueryIsRule=1; ;
    break;}
case 401:
#line 2324 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(cat5_str(make1_str("create rule"), yyvsp[-10].str, make1_str("as on"), yyvsp[-6].str, make1_str("to")), yyvsp[-4].str, yyvsp[-3].str, make1_str("do"), yyvsp[-1].str), yyvsp[0].str);
				;
    break;}
case 402:
#line 2329 "preproc.y"
{ yyval.str = make1_str("nothing"); ;
    break;}
case 403:
#line 2330 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 404:
#line 2331 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 405:
#line 2332 "preproc.y"
{ yyval.str = cat3_str(make1_str("["), yyvsp[-1].str, make1_str("]")); ;
    break;}
case 406:
#line 2333 "preproc.y"
{ yyval.str = cat3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 407:
#line 2336 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 408:
#line 2337 "preproc.y"
{  yyval.str = yyvsp[0].str; ;
    break;}
case 409:
#line 2341 "preproc.y"
{  yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 410:
#line 2343 "preproc.y"
{  yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, make1_str(";")); ;
    break;}
case 411:
#line 2345 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, make1_str(";")); ;
    break;}
case 416:
#line 2355 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 417:
#line 2359 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 418:
#line 2365 "preproc.y"
{ yyval.str = make1_str("select"); ;
    break;}
case 419:
#line 2366 "preproc.y"
{ yyval.str = make1_str("update"); ;
    break;}
case 420:
#line 2367 "preproc.y"
{ yyval.str = make1_str("delete"); ;
    break;}
case 421:
#line 2368 "preproc.y"
{ yyval.str = make1_str("insert"); ;
    break;}
case 422:
#line 2371 "preproc.y"
{ yyval.str = make1_str("instead"); ;
    break;}
case 423:
#line 2372 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 424:
#line 2385 "preproc.y"
{
					yyval.str = cat2_str(make1_str("notify"), yyvsp[0].str);
				;
    break;}
case 425:
#line 2391 "preproc.y"
{
					yyval.str = cat2_str(make1_str("listen"), yyvsp[0].str);
                                ;
    break;}
case 426:
#line 2397 "preproc.y"
{
					yyval.str = cat2_str(make1_str("unlisten"), yyvsp[0].str);
                                ;
    break;}
case 427:
#line 2401 "preproc.y"
{
					yyval.str = make1_str("unlisten *");
                                ;
    break;}
case 428:
#line 2418 "preproc.y"
{ yyval.str = make1_str("rollback"); ;
    break;}
case 429:
#line 2419 "preproc.y"
{ yyval.str = make1_str("begin transaction"); ;
    break;}
case 430:
#line 2420 "preproc.y"
{ yyval.str = make1_str("commit"); ;
    break;}
case 431:
#line 2421 "preproc.y"
{ yyval.str = make1_str("commit"); ;
    break;}
case 432:
#line 2422 "preproc.y"
{ yyval.str = make1_str("rollback"); ;
    break;}
case 433:
#line 2424 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 434:
#line 2425 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 435:
#line 2426 "preproc.y"
{ yyval.str = ""; ;
    break;}
case 436:
#line 2437 "preproc.y"
{
					yyval.str = cat4_str(make1_str("create view"), yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
				;
    break;}
case 437:
#line 2451 "preproc.y"
{
					yyval.str = cat2_str(make1_str("load"), yyvsp[0].str);
				;
    break;}
case 438:
#line 2465 "preproc.y"
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
case 439:
#line 2475 "preproc.y"
{
					yyval.str = cat2_str(make1_str("create database"), yyvsp[0].str);
				;
    break;}
case 440:
#line 2480 "preproc.y"
{ yyval.str = cat2_str(make1_str("location ="), yyvsp[0].str); ;
    break;}
case 441:
#line 2481 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 442:
#line 2484 "preproc.y"
{ yyval.str = cat2_str(make1_str("encoding ="), yyvsp[0].str); ;
    break;}
case 443:
#line 2485 "preproc.y"
{ yyval.str = NULL; ;
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
#line 2493 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 448:
#line 2494 "preproc.y"
{ yyval.str = make1_str("default"); ;
    break;}
case 449:
#line 2495 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 450:
#line 2506 "preproc.y"
{
					yyval.str = cat2_str(make1_str("drop database"), yyvsp[0].str);
				;
    break;}
case 451:
#line 2520 "preproc.y"
{
				   yyval.str = cat4_str(make1_str("cluster"), yyvsp[-2].str, make1_str("on"), yyvsp[0].str);
				;
    break;}
case 452:
#line 2534 "preproc.y"
{
					yyval.str = cat3_str(make1_str("vacuum"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 453:
#line 2538 "preproc.y"
{
					if ( strlen(yyvsp[0].str) > 0 && strlen(yyvsp[-1].str) == 0 )
						yyerror("parser: syntax error at or near \"(\"");
					yyval.str = cat5_str(make1_str("vacuum"), yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 454:
#line 2545 "preproc.y"
{ yyval.str = make1_str("verbose"); ;
    break;}
case 455:
#line 2546 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 456:
#line 2549 "preproc.y"
{ yyval.str = make1_str("analyse"); ;
    break;}
case 457:
#line 2550 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 458:
#line 2553 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 459:
#line 2554 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 460:
#line 2558 "preproc.y"
{ yyval.str=yyvsp[0].str; ;
    break;}
case 461:
#line 2560 "preproc.y"
{ yyval.str=cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 462:
#line 2572 "preproc.y"
{
					yyval.str = cat3_str(make1_str("explain"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 469:
#line 2612 "preproc.y"
{
					yyval.str = cat3_str(make1_str("insert into"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 470:
#line 2618 "preproc.y"
{
					yyval.str = make3_str(make1_str("values("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 471:
#line 2622 "preproc.y"
{
					yyval.str = make1_str("default values");
				;
    break;}
case 472:
#line 2626 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 473:
#line 2630 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-5].str, make1_str(") values ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 474:
#line 2634 "preproc.y"
{
					yyval.str = make4_str(make1_str("("), yyvsp[-2].str, make1_str(")"), yyvsp[0].str);
				;
    break;}
case 475:
#line 2639 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 476:
#line 2640 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 477:
#line 2645 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 478:
#line 2647 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 479:
#line 2651 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 480:
#line 2666 "preproc.y"
{
					yyval.str = cat3_str(make1_str("delete from"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 481:
#line 2672 "preproc.y"
{
					yyval.str = cat3_str(make1_str("lock"), yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 482:
#line 2676 "preproc.y"
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
case 483:
#line 2707 "preproc.y"
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
case 484:
#line 2727 "preproc.y"
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
case 485:
#line 2743 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 486:
#line 2744 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 487:
#line 2761 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("update"), yyvsp[-4].str, make1_str("set"), yyvsp[-2].str, yyvsp[-1].str), yyvsp[0].str);
				;
    break;}
case 488:
#line 2774 "preproc.y"
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
case 489:
#line 2804 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 490:
#line 2805 "preproc.y"
{ yyval.str = make1_str("insensitive"); ;
    break;}
case 491:
#line 2806 "preproc.y"
{ yyval.str = make1_str("scroll"); ;
    break;}
case 492:
#line 2807 "preproc.y"
{ yyval.str = make1_str("insensitive scroll"); ;
    break;}
case 493:
#line 2808 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 494:
#line 2811 "preproc.y"
{ yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 495:
#line 2812 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 496:
#line 2816 "preproc.y"
{ yyval.str = make1_str("read only"); ;
    break;}
case 497:
#line 2818 "preproc.y"
{
                               yyerror("DECLARE/UPDATE not supported; Cursors must be READ ONLY.");
                       ;
    break;}
case 498:
#line 2823 "preproc.y"
{ yyval.str = make2_str(make1_str("of"), yyvsp[0].str); ;
    break;}
case 499:
#line 2840 "preproc.y"
{
					if (strlen(yyvsp[-1].str) > 0 && ForUpdateNotAllowed != 0)
							yyerror("SELECT FOR UPDATE is not allowed in this context");

					ForUpdateNotAllowed = 0;
					yyval.str = cat4_str(yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 500:
#line 2857 "preproc.y"
{
                               yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); 
                        ;
    break;}
case 501:
#line 2861 "preproc.y"
{
                               yyval.str = yyvsp[0].str; 
                        ;
    break;}
case 502:
#line 2865 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-2].str, make1_str("except"), yyvsp[0].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 503:
#line 2870 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-3].str, make1_str("union"), yyvsp[-1].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 504:
#line 2875 "preproc.y"
{
				yyval.str = cat3_str(yyvsp[-3].str, make1_str("intersect"), yyvsp[-1].str);
				ForUpdateNotAllowed = 1;
			;
    break;}
case 505:
#line 2885 "preproc.y"
{
					yyval.str = cat4_str(cat5_str(make1_str("select"), yyvsp[-6].str, yyvsp[-5].str, yyvsp[-4].str, yyvsp[-3].str), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
					if (strlen(yyvsp[-1].str) > 0 || strlen(yyvsp[0].str) > 0)
						ForUpdateNotAllowed = 1;
				;
    break;}
case 506:
#line 2892 "preproc.y"
{ yyval.str= cat4_str(make1_str("into"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 507:
#line 2893 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 508:
#line 2894 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 509:
#line 2897 "preproc.y"
{ yyval.str = make1_str("table"); ;
    break;}
case 510:
#line 2898 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 511:
#line 2901 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 512:
#line 2902 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 513:
#line 2905 "preproc.y"
{ yyval.str = make1_str("distinct"); ;
    break;}
case 514:
#line 2906 "preproc.y"
{ yyval.str = cat2_str(make1_str("distinct on"), yyvsp[0].str); ;
    break;}
case 515:
#line 2907 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 516:
#line 2908 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 517:
#line 2911 "preproc.y"
{ yyval.str = cat2_str(make1_str("order by"), yyvsp[0].str); ;
    break;}
case 518:
#line 2912 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 519:
#line 2915 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 520:
#line 2916 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 521:
#line 2920 "preproc.y"
{
					 yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 522:
#line 2925 "preproc.y"
{ yyval.str = cat2_str(make1_str("using"), yyvsp[0].str); ;
    break;}
case 523:
#line 2926 "preproc.y"
{ yyval.str = make1_str("using <"); ;
    break;}
case 524:
#line 2927 "preproc.y"
{ yyval.str = make1_str("using >"); ;
    break;}
case 525:
#line 2928 "preproc.y"
{ yyval.str = make1_str("asc"); ;
    break;}
case 526:
#line 2929 "preproc.y"
{ yyval.str = make1_str("desc"); ;
    break;}
case 527:
#line 2930 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 528:
#line 2934 "preproc.y"
{ yyval.str = cat4_str(make1_str("limit"), yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 529:
#line 2936 "preproc.y"
{ yyval.str = cat4_str(make1_str("limit"), yyvsp[-2].str, make1_str("offset"), yyvsp[0].str); ;
    break;}
case 530:
#line 2938 "preproc.y"
{ yyval.str = cat2_str(make1_str("limit"), yyvsp[0].str); ;
    break;}
case 531:
#line 2940 "preproc.y"
{ yyval.str = cat4_str(make1_str("offset"), yyvsp[-2].str, make1_str("limit"), yyvsp[0].str); ;
    break;}
case 532:
#line 2942 "preproc.y"
{ yyval.str = cat2_str(make1_str("offset"), yyvsp[0].str); ;
    break;}
case 533:
#line 2944 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 534:
#line 2947 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 535:
#line 2948 "preproc.y"
{ yyval.str = make1_str("all"); ;
    break;}
case 536:
#line 2949 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 537:
#line 2952 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 538:
#line 2953 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 539:
#line 2963 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 540:
#line 2964 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 541:
#line 2967 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 542:
#line 2970 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 543:
#line 2972 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 544:
#line 2975 "preproc.y"
{ yyval.str = cat2_str(make1_str("groub by"), yyvsp[0].str); ;
    break;}
case 545:
#line 2976 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 546:
#line 2980 "preproc.y"
{
					yyval.str = cat2_str(make1_str("having"), yyvsp[0].str);
				;
    break;}
case 547:
#line 2983 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 548:
#line 2987 "preproc.y"
{
                	yyval.str = make1_str("for update"); 
		;
    break;}
case 549:
#line 2991 "preproc.y"
{
                        yyval.str = make1_str("");
                ;
    break;}
case 550:
#line 2996 "preproc.y"
{
			yyval.str = cat2_str(make1_str("of"), yyvsp[0].str);
	      ;
    break;}
case 551:
#line 3000 "preproc.y"
{
                        yyval.str = make1_str("");
              ;
    break;}
case 552:
#line 3014 "preproc.y"
{
			yyval.str = cat2_str(make1_str("from"), yyvsp[0].str);
		;
    break;}
case 553:
#line 3018 "preproc.y"
{
			yyval.str = make1_str("");
		;
    break;}
case 554:
#line 3024 "preproc.y"
{ yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 555:
#line 3026 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 556:
#line 3028 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 557:
#line 3032 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 558:
#line 3034 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 559:
#line 3038 "preproc.y"
{
                                        yyval.str = cat3_str(yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
                                ;
    break;}
case 560:
#line 3042 "preproc.y"
{
                                        yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 561:
#line 3046 "preproc.y"
{
                                        yyval.str = yyvsp[0].str;
                                ;
    break;}
case 562:
#line 3056 "preproc.y"
{       yyval.str = yyvsp[0].str; ;
    break;}
case 563:
#line 3058 "preproc.y"
{       yyerror("UNION JOIN not yet implemented"); ;
    break;}
case 564:
#line 3062 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 565:
#line 3068 "preproc.y"
{
                                        yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 566:
#line 3072 "preproc.y"
{
                                        yyval.str = yyvsp[0].str;
                                ;
    break;}
case 567:
#line 3085 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-3].str, make1_str("join"), yyvsp[-1].str, yyvsp[0].str);
                                ;
    break;}
case 568:
#line 3089 "preproc.y"
{
					yyval.str = cat4_str(make1_str("natural"), yyvsp[-2].str, make1_str("join"), yyvsp[0].str);
                                ;
    break;}
case 569:
#line 3093 "preproc.y"
{ 	yyval.str = cat2_str(make1_str("cross join"), yyvsp[0].str); ;
    break;}
case 570:
#line 3098 "preproc.y"
{
                                        yyval.str = cat2_str(make1_str("full"), yyvsp[0].str);
                                        fprintf(stderr,"FULL OUTER JOIN not yet implemented\n");
                                ;
    break;}
case 571:
#line 3103 "preproc.y"
{
                                        yyval.str = cat2_str(make1_str("left"), yyvsp[0].str);
                                        fprintf(stderr,"LEFT OUTER JOIN not yet implemented\n");
                                ;
    break;}
case 572:
#line 3108 "preproc.y"
{
                                        yyval.str = cat2_str(make1_str("right"), yyvsp[0].str);
                                        fprintf(stderr,"RIGHT OUTER JOIN not yet implemented\n");
                                ;
    break;}
case 573:
#line 3113 "preproc.y"
{
                                        yyval.str = make1_str("outer");
                                        fprintf(stderr,"OUTER JOIN not yet implemented\n");
                                ;
    break;}
case 574:
#line 3118 "preproc.y"
{
                                        yyval.str = make1_str("inner");
				;
    break;}
case 575:
#line 3122 "preproc.y"
{
                                        yyval.str = make1_str("");
				;
    break;}
case 576:
#line 3127 "preproc.y"
{ yyval.str = make1_str("outer"); ;
    break;}
case 577:
#line 3128 "preproc.y"
{ yyval.str = make1_str("");  /* no qualifiers */ ;
    break;}
case 578:
#line 3139 "preproc.y"
{ yyval.str = make3_str(make1_str("using ("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 579:
#line 3140 "preproc.y"
{ yyval.str = cat2_str(make1_str("on"), yyvsp[0].str); ;
    break;}
case 580:
#line 3143 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 581:
#line 3144 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 582:
#line 3148 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 583:
#line 3153 "preproc.y"
{ yyval.str = cat2_str(make1_str("where"), yyvsp[0].str); ;
    break;}
case 584:
#line 3154 "preproc.y"
{ yyval.str = make1_str("");  /* no qualifiers */ ;
    break;}
case 585:
#line 3158 "preproc.y"
{
					/* normal relations */
					yyval.str = yyvsp[0].str;
				;
    break;}
case 586:
#line 3163 "preproc.y"
{
					/* inheritance query */
					yyval.str = cat2_str(yyvsp[-1].str, make1_str("*"));
				;
    break;}
case 587:
#line 3169 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 588:
#line 3175 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 589:
#line 3181 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 590:
#line 3189 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 591:
#line 3195 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 592:
#line 3201 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 593:
#line 3219 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].index.str);
				;
    break;}
case 594:
#line 3222 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 595:
#line 3224 "preproc.y"
{
					yyval.str = cat2_str(make1_str("setof"), yyvsp[0].str);
				;
    break;}
case 597:
#line 3230 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 598:
#line 3231 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 599:
#line 3235 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 600:
#line 3240 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 601:
#line 3241 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 602:
#line 3242 "preproc.y"
{ yyval.str = make1_str("at"); ;
    break;}
case 603:
#line 3243 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 604:
#line 3244 "preproc.y"
{ yyval.str = make1_str("break"); ;
    break;}
case 605:
#line 3245 "preproc.y"
{ yyval.str = make1_str("call"); ;
    break;}
case 606:
#line 3246 "preproc.y"
{ yyval.str = make1_str("connect"); ;
    break;}
case 607:
#line 3247 "preproc.y"
{ yyval.str = make1_str("connection"); ;
    break;}
case 608:
#line 3248 "preproc.y"
{ yyval.str = make1_str("continue"); ;
    break;}
case 609:
#line 3249 "preproc.y"
{ yyval.str = make1_str("deallocate"); ;
    break;}
case 610:
#line 3250 "preproc.y"
{ yyval.str = make1_str("disconnect"); ;
    break;}
case 611:
#line 3251 "preproc.y"
{ yyval.str = make1_str("found"); ;
    break;}
case 612:
#line 3252 "preproc.y"
{ yyval.str = make1_str("go"); ;
    break;}
case 613:
#line 3253 "preproc.y"
{ yyval.str = make1_str("goto"); ;
    break;}
case 614:
#line 3254 "preproc.y"
{ yyval.str = make1_str("identified"); ;
    break;}
case 615:
#line 3255 "preproc.y"
{ yyval.str = make1_str("immediate"); ;
    break;}
case 616:
#line 3256 "preproc.y"
{ yyval.str = make1_str("indicator"); ;
    break;}
case 617:
#line 3257 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 618:
#line 3258 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 619:
#line 3259 "preproc.y"
{ yyval.str = make1_str("open"); ;
    break;}
case 620:
#line 3260 "preproc.y"
{ yyval.str = make1_str("prepare"); ;
    break;}
case 621:
#line 3261 "preproc.y"
{ yyval.str = make1_str("release"); ;
    break;}
case 622:
#line 3262 "preproc.y"
{ yyval.str = make1_str("section"); ;
    break;}
case 623:
#line 3263 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 624:
#line 3264 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 625:
#line 3265 "preproc.y"
{ yyval.str = make1_str("sqlerror"); ;
    break;}
case 626:
#line 3266 "preproc.y"
{ yyval.str = make1_str("sqlprint"); ;
    break;}
case 627:
#line 3267 "preproc.y"
{ yyval.str = make1_str("sqlwarning"); ;
    break;}
case 628:
#line 3268 "preproc.y"
{ yyval.str = make1_str("stop"); ;
    break;}
case 629:
#line 3269 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 630:
#line 3270 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 631:
#line 3271 "preproc.y"
{ yyval.str = make1_str("var"); ;
    break;}
case 632:
#line 3272 "preproc.y"
{ yyval.str = make1_str("whenever"); ;
    break;}
case 633:
#line 3281 "preproc.y"
{
					yyval.str = cat2_str(make1_str("float"), yyvsp[0].str);
				;
    break;}
case 634:
#line 3285 "preproc.y"
{
					yyval.str = make1_str("double precision");
				;
    break;}
case 635:
#line 3289 "preproc.y"
{
					yyval.str = cat2_str(make1_str("decimal"), yyvsp[0].str);
				;
    break;}
case 636:
#line 3293 "preproc.y"
{
					yyval.str = cat2_str(make1_str("numeric"), yyvsp[0].str);
				;
    break;}
case 637:
#line 3299 "preproc.y"
{	yyval.str = make1_str("float"); ;
    break;}
case 638:
#line 3301 "preproc.y"
{	yyval.str = make1_str("double precision"); ;
    break;}
case 639:
#line 3303 "preproc.y"
{	yyval.str = make1_str("decimal"); ;
    break;}
case 640:
#line 3305 "preproc.y"
{	yyval.str = make1_str("numeric"); ;
    break;}
case 641:
#line 3309 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1)
						yyerror("precision for FLOAT must be at least 1");
					else if (atol(yyvsp[-1].str) >= 16)
						yyerror("precision for FLOAT must be less than 16");
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 642:
#line 3317 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 643:
#line 3323 "preproc.y"
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
#line 3335 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1 || atol(yyvsp[-1].str) > NUMERIC_MAX_PRECISION) {
						sprintf(errortext, "NUMERIC precision %s must be between 1 and %d", yyvsp[-1].str, NUMERIC_MAX_PRECISION);
						yyerror(errortext);
					}
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 645:
#line 3343 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 646:
#line 3349 "preproc.y"
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
case 647:
#line 3361 "preproc.y"
{
					if (atol(yyvsp[-1].str) < 1 || atol(yyvsp[-1].str) > NUMERIC_MAX_PRECISION) {
						sprintf(errortext, "NUMERIC precision %s must be between 1 and %d", yyvsp[-1].str, NUMERIC_MAX_PRECISION);
						yyerror(errortext);
					}
					yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 648:
#line 3369 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 649:
#line 3382 "preproc.y"
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
case 650:
#line 3402 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 651:
#line 3408 "preproc.y"
{
					if (strlen(yyvsp[0].str) > 0) 
						fprintf(stderr, "COLLATE %s not yet implemented",yyvsp[0].str);

					yyval.str = cat4_str(make1_str("character"), yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 652:
#line 3414 "preproc.y"
{ yyval.str = cat2_str(make1_str("char"), yyvsp[0].str); ;
    break;}
case 653:
#line 3415 "preproc.y"
{ yyval.str = make1_str("varchar"); ;
    break;}
case 654:
#line 3416 "preproc.y"
{ yyval.str = cat2_str(make1_str("national character"), yyvsp[0].str); ;
    break;}
case 655:
#line 3417 "preproc.y"
{ yyval.str = cat2_str(make1_str("nchar"), yyvsp[0].str); ;
    break;}
case 656:
#line 3420 "preproc.y"
{ yyval.str = make1_str("varying"); ;
    break;}
case 657:
#line 3421 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 658:
#line 3424 "preproc.y"
{ yyval.str = cat2_str(make1_str("character set"), yyvsp[0].str); ;
    break;}
case 659:
#line 3425 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 660:
#line 3428 "preproc.y"
{ yyval.str = cat2_str(make1_str("collate"), yyvsp[0].str); ;
    break;}
case 661:
#line 3429 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 662:
#line 3433 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 663:
#line 3437 "preproc.y"
{
					yyval.str = cat2_str(make1_str("timestamp"), yyvsp[0].str);
				;
    break;}
case 664:
#line 3441 "preproc.y"
{
					yyval.str = make1_str("time");
				;
    break;}
case 665:
#line 3445 "preproc.y"
{
					yyval.str = cat2_str(make1_str("interval"), yyvsp[0].str);
				;
    break;}
case 666:
#line 3450 "preproc.y"
{ yyval.str = make1_str("year"); ;
    break;}
case 667:
#line 3451 "preproc.y"
{ yyval.str = make1_str("month"); ;
    break;}
case 668:
#line 3452 "preproc.y"
{ yyval.str = make1_str("day"); ;
    break;}
case 669:
#line 3453 "preproc.y"
{ yyval.str = make1_str("hour"); ;
    break;}
case 670:
#line 3454 "preproc.y"
{ yyval.str = make1_str("minute"); ;
    break;}
case 671:
#line 3455 "preproc.y"
{ yyval.str = make1_str("second"); ;
    break;}
case 672:
#line 3458 "preproc.y"
{ yyval.str = make1_str("with time zone"); ;
    break;}
case 673:
#line 3459 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 674:
#line 3462 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 675:
#line 3463 "preproc.y"
{ yyval.str = make1_str("year to #month"); ;
    break;}
case 676:
#line 3464 "preproc.y"
{ yyval.str = make1_str("day to hour"); ;
    break;}
case 677:
#line 3465 "preproc.y"
{ yyval.str = make1_str("day to minute"); ;
    break;}
case 678:
#line 3466 "preproc.y"
{ yyval.str = make1_str("day to second"); ;
    break;}
case 679:
#line 3467 "preproc.y"
{ yyval.str = make1_str("hour to minute"); ;
    break;}
case 680:
#line 3468 "preproc.y"
{ yyval.str = make1_str("minute to second"); ;
    break;}
case 681:
#line 3469 "preproc.y"
{ yyval.str = make1_str("hour to second"); ;
    break;}
case 682:
#line 3470 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 683:
#line 3481 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 684:
#line 3483 "preproc.y"
{
					yyval.str = make1_str("null");
				;
    break;}
case 685:
#line 3498 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-5].str, make1_str(") in ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 686:
#line 3502 "preproc.y"
{
					yyval.str = make5_str(make1_str("("), yyvsp[-6].str, make1_str(") not in ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 687:
#line 3506 "preproc.y"
{
					yyval.str = make4_str(make5_str(make1_str("("), yyvsp[-6].str, make1_str(")"), yyvsp[-4].str, yyvsp[-3].str), make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 688:
#line 3510 "preproc.y"
{
					yyval.str = make3_str(make5_str(make1_str("("), yyvsp[-5].str, make1_str(")"), yyvsp[-3].str, make1_str("(")), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 689:
#line 3514 "preproc.y"
{
					yyval.str = cat3_str(make3_str(make1_str("("), yyvsp[-5].str, make1_str(")")), yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 690:
#line 3520 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 691:
#line 3525 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 692:
#line 3526 "preproc.y"
{ yyval.str = "<"; ;
    break;}
case 693:
#line 3527 "preproc.y"
{ yyval.str = "="; ;
    break;}
case 694:
#line 3528 "preproc.y"
{ yyval.str = ">"; ;
    break;}
case 695:
#line 3529 "preproc.y"
{ yyval.str = "+"; ;
    break;}
case 696:
#line 3530 "preproc.y"
{ yyval.str = "-"; ;
    break;}
case 697:
#line 3531 "preproc.y"
{ yyval.str = "*"; ;
    break;}
case 698:
#line 3532 "preproc.y"
{ yyval.str = "%"; ;
    break;}
case 699:
#line 3533 "preproc.y"
{ yyval.str = "/"; ;
    break;}
case 700:
#line 3536 "preproc.y"
{ yyval.str = make1_str("ANY"); ;
    break;}
case 701:
#line 3537 "preproc.y"
{ yyval.str = make1_str("ALL"); ;
    break;}
case 702:
#line 3542 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
				;
    break;}
case 703:
#line 3546 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 704:
#line 3561 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 705:
#line 3565 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 706:
#line 3567 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 707:
#line 3569 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 708:
#line 3573 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 709:
#line 3575 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 710:
#line 3577 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 711:
#line 3579 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 712:
#line 3581 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("%"), yyvsp[0].str); ;
    break;}
case 713:
#line 3583 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 714:
#line 3585 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 715:
#line 3587 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 716:
#line 3589 "preproc.y"
{       yyval.str = cat2_str(yyvsp[-2].str, make1_str("= NULL")); ;
    break;}
case 717:
#line 3591 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 718:
#line 3596 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 719:
#line 3598 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 720:
#line 3600 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 721:
#line 3604 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 722:
#line 3608 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 723:
#line 3610 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 724:
#line 3612 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 725:
#line 3614 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 726:
#line 3616 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 727:
#line 3618 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 728:
#line 3620 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make1_str("(*)")); 
				;
    break;}
case 729:
#line 3624 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 730:
#line 3628 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 731:
#line 3632 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 732:
#line 3636 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 733:
#line 3640 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 734:
#line 3646 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 735:
#line 3650 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 736:
#line 3656 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 737:
#line 3660 "preproc.y"
{
  		     		        yyval.str = make1_str("user");
			     	;
    break;}
case 738:
#line 3665 "preproc.y"
{
					yyval.str = make3_str(make1_str("exists("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 739:
#line 3669 "preproc.y"
{
					yyval.str = make3_str(make1_str("extract("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 740:
#line 3673 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 741:
#line 3677 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 742:
#line 3682 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 743:
#line 3686 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 744:
#line 3690 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 745:
#line 3694 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 746:
#line 3698 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 747:
#line 3700 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 748:
#line 3702 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 749:
#line 3704 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 750:
#line 3711 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); }
				;
    break;}
case 751:
#line 3715 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); }
				;
    break;}
case 752:
#line 3719 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); }
				;
    break;}
case 753:
#line 3723 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); }
				;
    break;}
case 754:
#line 3727 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 755:
#line 3731 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 756:
#line 3735 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(" in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 757:
#line 3739 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str(" not in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 758:
#line 3743 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-4].str, yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 759:
#line 3747 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("+("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 760:
#line 3751 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("-("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 761:
#line 3755 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("/("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 762:
#line 3759 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("%("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 763:
#line 3763 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("*("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 764:
#line 3767 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("<("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 765:
#line 3771 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(">("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 766:
#line 3775 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("=("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 767:
#line 3779 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("any("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 768:
#line 3783 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+ any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 769:
#line 3787 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("- any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 770:
#line 3791 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/ any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 771:
#line 3795 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("% any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 772:
#line 3799 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("* any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 773:
#line 3803 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("< any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 774:
#line 3807 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("> any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 775:
#line 3811 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("= any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 776:
#line 3815 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("all ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 777:
#line 3819 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+ all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 778:
#line 3823 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("- all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 779:
#line 3827 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/ all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 780:
#line 3831 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("% all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 781:
#line 3835 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("* all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 782:
#line 3839 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("< all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 783:
#line 3843 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("> all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 784:
#line 3847 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("= all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 785:
#line 3851 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 786:
#line 3853 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 787:
#line 3855 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 788:
#line 3857 "preproc.y"
{       yyval.str = yyvsp[0].str; ;
    break;}
case 789:
#line 3859 "preproc.y"
{ yyval.str = make1_str("?"); ;
    break;}
case 790:
#line 3868 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 791:
#line 3872 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 792:
#line 3874 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 793:
#line 3878 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 794:
#line 3880 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 795:
#line 3882 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 796:
#line 3884 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 797:
#line 3886 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("%"), yyvsp[0].str); ;
    break;}
case 798:
#line 3888 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 799:
#line 3893 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 800:
#line 3895 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 801:
#line 3897 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 802:
#line 3901 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 803:
#line 3905 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 804:
#line 3907 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 805:
#line 3909 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 806:
#line 3911 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 807:
#line 3913 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 808:
#line 3917 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 809:
#line 3921 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 810:
#line 3925 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 811:
#line 3929 "preproc.y"
{
					if (yyvsp[-1].str != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 812:
#line 3935 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 813:
#line 3939 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 814:
#line 3945 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 815:
#line 3949 "preproc.y"
{
					yyval.str = make1_str("user");
				;
    break;}
case 816:
#line 3953 "preproc.y"
{
					yyval.str = make3_str(make1_str("position ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 817:
#line 3957 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring ("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 818:
#line 3962 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 819:
#line 3966 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 820:
#line 3970 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 821:
#line 3974 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 822:
#line 3978 "preproc.y"
{ 	yyval.str = yyvsp[0].str; ;
    break;}
case 823:
#line 3982 "preproc.y"
{
					yyval.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].str);
				;
    break;}
case 824:
#line 3986 "preproc.y"
{
					yyval.str = cat2_str(cat5_str(make1_str("["), yyvsp[-4].str, make1_str(":"), yyvsp[-2].str, make1_str("]")), yyvsp[0].str);
				;
    break;}
case 825:
#line 3990 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 826:
#line 3994 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 827:
#line 3996 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str); ;
    break;}
case 828:
#line 3998 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str("using"), yyvsp[0].str); ;
    break;}
case 829:
#line 4002 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("from"), yyvsp[0].str);
				;
    break;}
case 830:
#line 4006 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 831:
#line 4008 "preproc.y"
{ yyval.str = make1_str("?"); ;
    break;}
case 832:
#line 4011 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 833:
#line 4012 "preproc.y"
{ yyval.str = make1_str("timezone_hour"); ;
    break;}
case 834:
#line 4013 "preproc.y"
{ yyval.str = make1_str("timezone_minute"); ;
    break;}
case 835:
#line 4017 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("in"), yyvsp[0].str); ;
    break;}
case 836:
#line 4019 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 837:
#line 4023 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 838:
#line 4027 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 839:
#line 4029 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 840:
#line 4031 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 841:
#line 4033 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 842:
#line 4035 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 843:
#line 4037 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("%"), yyvsp[0].str); ;
    break;}
case 844:
#line 4039 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 845:
#line 4041 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 846:
#line 4043 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 847:
#line 4047 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 848:
#line 4051 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 849:
#line 4053 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 850:
#line 4055 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 851:
#line 4057 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 852:
#line 4059 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 853:
#line 4063 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()"));
				;
    break;}
case 854:
#line 4067 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 855:
#line 4071 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 856:
#line 4075 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 857:
#line 4080 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 858:
#line 4084 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 859:
#line 4088 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 860:
#line 4092 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 861:
#line 4098 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 862:
#line 4102 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 863:
#line 4106 "preproc.y"
{	yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 864:
#line 4108 "preproc.y"
{
					yyval.str = make1_str("");
				;
    break;}
case 865:
#line 4114 "preproc.y"
{	yyval.str = cat2_str(make1_str("for"), yyvsp[0].str); ;
    break;}
case 866:
#line 4116 "preproc.y"
{	yyval.str = make1_str(""); ;
    break;}
case 867:
#line 4120 "preproc.y"
{ yyval.str = cat3_str(yyvsp[-2].str, make1_str("from"), yyvsp[0].str); ;
    break;}
case 868:
#line 4122 "preproc.y"
{ yyval.str = cat2_str(make1_str("from"), yyvsp[0].str); ;
    break;}
case 869:
#line 4124 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 870:
#line 4128 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 871:
#line 4132 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 872:
#line 4136 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 873:
#line 4138 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);;
    break;}
case 874:
#line 4142 "preproc.y"
{
					yyval.str = yyvsp[0].str; 
				;
    break;}
case 875:
#line 4146 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 876:
#line 4150 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 877:
#line 4152 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);;
    break;}
case 878:
#line 4171 "preproc.y"
{ yyval.str = cat5_str(make1_str("case"), yyvsp[-3].str, yyvsp[-2].str, yyvsp[-1].str, make1_str("end")); ;
    break;}
case 879:
#line 4173 "preproc.y"
{
					yyval.str = cat5_str(make1_str("nullif("), yyvsp[-3].str, make1_str(","), yyvsp[-1].str, make1_str(")"));

					fprintf(stderr, "NULLIF() not yet fully implemented");
                                ;
    break;}
case 880:
#line 4179 "preproc.y"
{
					yyval.str = cat3_str(make1_str("coalesce("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 881:
#line 4185 "preproc.y"
{ yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 882:
#line 4187 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 883:
#line 4191 "preproc.y"
{
					yyval.str = cat4_str(make1_str("when"), yyvsp[-2].str, make1_str("then"), yyvsp[0].str);
                               ;
    break;}
case 884:
#line 4196 "preproc.y"
{ yyval.str = cat2_str(make1_str("else"), yyvsp[0].str); ;
    break;}
case 885:
#line 4197 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 886:
#line 4201 "preproc.y"
{
                                       yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
                               ;
    break;}
case 887:
#line 4205 "preproc.y"
{
                                       yyval.str = yyvsp[0].str;
                               ;
    break;}
case 888:
#line 4209 "preproc.y"
{       yyval.str = make1_str(""); ;
    break;}
case 889:
#line 4213 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 890:
#line 4217 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str);
				;
    break;}
case 891:
#line 4223 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 892:
#line 4225 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str); ;
    break;}
case 893:
#line 4227 "preproc.y"
{ yyval.str = make2_str(yyvsp[-2].str, make1_str(".*")); ;
    break;}
case 894:
#line 4238 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","),yyvsp[0].str);  ;
    break;}
case 895:
#line 4240 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 896:
#line 4241 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 897:
#line 4245 "preproc.y"
{
					yyval.str = cat4_str(yyvsp[-3].str, yyvsp[-2].str, make1_str("="), yyvsp[0].str);
				;
    break;}
case 898:
#line 4249 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 899:
#line 4253 "preproc.y"
{
					yyval.str = make2_str(yyvsp[-2].str, make1_str(".*"));
				;
    break;}
case 900:
#line 4264 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);  ;
    break;}
case 901:
#line 4266 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 902:
#line 4271 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("as"), yyvsp[0].str);
				;
    break;}
case 903:
#line 4275 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 904:
#line 4279 "preproc.y"
{
					yyval.str = make2_str(yyvsp[-2].str, make1_str(".*"));
				;
    break;}
case 905:
#line 4283 "preproc.y"
{
					yyval.str = make1_str("*");
				;
    break;}
case 906:
#line 4288 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 907:
#line 4289 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 908:
#line 4293 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 909:
#line 4297 "preproc.y"
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
case 910:
#line 4309 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 911:
#line 4310 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 912:
#line 4311 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 913:
#line 4312 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 914:
#line 4313 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 915:
#line 4319 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 916:
#line 4320 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 917:
#line 4322 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 918:
#line 4329 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 919:
#line 4333 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 920:
#line 4337 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 921:
#line 4341 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 922:
#line 4345 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 923:
#line 4347 "preproc.y"
{
					yyval.str = make1_str("true");
				;
    break;}
case 924:
#line 4351 "preproc.y"
{
					yyval.str = make1_str("false");
				;
    break;}
case 925:
#line 4357 "preproc.y"
{
					yyval.str = cat2_str(make_name(), yyvsp[0].str);
				;
    break;}
case 926:
#line 4362 "preproc.y"
{ yyval.str = make_name();;
    break;}
case 927:
#line 4363 "preproc.y"
{ yyval.str = make_name();;
    break;}
case 928:
#line 4364 "preproc.y"
{
							yyval.str = (char *)mm_alloc(strlen(yyvsp[0].str) + 3);
							yyval.str[0]='\'';
				     		        strcpy(yyval.str+1, yyvsp[0].str);
							yyval.str[strlen(yyvsp[0].str)+2]='\0';
							yyval.str[strlen(yyvsp[0].str)+1]='\'';
							free(yyvsp[0].str);
						;
    break;}
case 929:
#line 4372 "preproc.y"
{ yyval.str = yyvsp[0].str;;
    break;}
case 930:
#line 4380 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 931:
#line 4382 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 932:
#line 4384 "preproc.y"
{	yyval.str = yyvsp[0].str; ;
    break;}
case 933:
#line 4394 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 934:
#line 4395 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 935:
#line 4396 "preproc.y"
{ yyval.str = make1_str("absolute"); ;
    break;}
case 936:
#line 4397 "preproc.y"
{ yyval.str = make1_str("action"); ;
    break;}
case 937:
#line 4398 "preproc.y"
{ yyval.str = make1_str("after"); ;
    break;}
case 938:
#line 4399 "preproc.y"
{ yyval.str = make1_str("aggregate"); ;
    break;}
case 939:
#line 4400 "preproc.y"
{ yyval.str = make1_str("backward"); ;
    break;}
case 940:
#line 4401 "preproc.y"
{ yyval.str = make1_str("before"); ;
    break;}
case 941:
#line 4402 "preproc.y"
{ yyval.str = make1_str("cache"); ;
    break;}
case 942:
#line 4403 "preproc.y"
{ yyval.str = make1_str("createdb"); ;
    break;}
case 943:
#line 4404 "preproc.y"
{ yyval.str = make1_str("createuser"); ;
    break;}
case 944:
#line 4405 "preproc.y"
{ yyval.str = make1_str("cycle"); ;
    break;}
case 945:
#line 4406 "preproc.y"
{ yyval.str = make1_str("database"); ;
    break;}
case 946:
#line 4407 "preproc.y"
{ yyval.str = make1_str("delimiters"); ;
    break;}
case 947:
#line 4408 "preproc.y"
{ yyval.str = make1_str("double"); ;
    break;}
case 948:
#line 4409 "preproc.y"
{ yyval.str = make1_str("each"); ;
    break;}
case 949:
#line 4410 "preproc.y"
{ yyval.str = make1_str("encoding"); ;
    break;}
case 950:
#line 4411 "preproc.y"
{ yyval.str = make1_str("forward"); ;
    break;}
case 951:
#line 4412 "preproc.y"
{ yyval.str = make1_str("function"); ;
    break;}
case 952:
#line 4413 "preproc.y"
{ yyval.str = make1_str("handler"); ;
    break;}
case 953:
#line 4414 "preproc.y"
{ yyval.str = make1_str("increment"); ;
    break;}
case 954:
#line 4415 "preproc.y"
{ yyval.str = make1_str("index"); ;
    break;}
case 955:
#line 4416 "preproc.y"
{ yyval.str = make1_str("inherits"); ;
    break;}
case 956:
#line 4417 "preproc.y"
{ yyval.str = make1_str("insensitive"); ;
    break;}
case 957:
#line 4418 "preproc.y"
{ yyval.str = make1_str("instead"); ;
    break;}
case 958:
#line 4419 "preproc.y"
{ yyval.str = make1_str("isnull"); ;
    break;}
case 959:
#line 4420 "preproc.y"
{ yyval.str = make1_str("key"); ;
    break;}
case 960:
#line 4421 "preproc.y"
{ yyval.str = make1_str("language"); ;
    break;}
case 961:
#line 4422 "preproc.y"
{ yyval.str = make1_str("lancompiler"); ;
    break;}
case 962:
#line 4423 "preproc.y"
{ yyval.str = make1_str("location"); ;
    break;}
case 963:
#line 4424 "preproc.y"
{ yyval.str = make1_str("match"); ;
    break;}
case 964:
#line 4425 "preproc.y"
{ yyval.str = make1_str("maxvalue"); ;
    break;}
case 965:
#line 4426 "preproc.y"
{ yyval.str = make1_str("minvalue"); ;
    break;}
case 966:
#line 4427 "preproc.y"
{ yyval.str = make1_str("next"); ;
    break;}
case 967:
#line 4428 "preproc.y"
{ yyval.str = make1_str("nocreatedb"); ;
    break;}
case 968:
#line 4429 "preproc.y"
{ yyval.str = make1_str("nocreateuser"); ;
    break;}
case 969:
#line 4430 "preproc.y"
{ yyval.str = make1_str("nothing"); ;
    break;}
case 970:
#line 4431 "preproc.y"
{ yyval.str = make1_str("notnull"); ;
    break;}
case 971:
#line 4432 "preproc.y"
{ yyval.str = make1_str("of"); ;
    break;}
case 972:
#line 4433 "preproc.y"
{ yyval.str = make1_str("oids"); ;
    break;}
case 973:
#line 4434 "preproc.y"
{ yyval.str = make1_str("only"); ;
    break;}
case 974:
#line 4435 "preproc.y"
{ yyval.str = make1_str("operator"); ;
    break;}
case 975:
#line 4436 "preproc.y"
{ yyval.str = make1_str("option"); ;
    break;}
case 976:
#line 4437 "preproc.y"
{ yyval.str = make1_str("password"); ;
    break;}
case 977:
#line 4438 "preproc.y"
{ yyval.str = make1_str("prior"); ;
    break;}
case 978:
#line 4439 "preproc.y"
{ yyval.str = make1_str("privileges"); ;
    break;}
case 979:
#line 4440 "preproc.y"
{ yyval.str = make1_str("procedural"); ;
    break;}
case 980:
#line 4441 "preproc.y"
{ yyval.str = make1_str("read"); ;
    break;}
case 981:
#line 4443 "preproc.y"
{ yyval.str = make1_str("relative"); ;
    break;}
case 982:
#line 4444 "preproc.y"
{ yyval.str = make1_str("rename"); ;
    break;}
case 983:
#line 4445 "preproc.y"
{ yyval.str = make1_str("returns"); ;
    break;}
case 984:
#line 4446 "preproc.y"
{ yyval.str = make1_str("row"); ;
    break;}
case 985:
#line 4447 "preproc.y"
{ yyval.str = make1_str("rule"); ;
    break;}
case 986:
#line 4448 "preproc.y"
{ yyval.str = make1_str("scroll"); ;
    break;}
case 987:
#line 4449 "preproc.y"
{ yyval.str = make1_str("sequence"); ;
    break;}
case 988:
#line 4450 "preproc.y"
{ yyval.str = make1_str("serial"); ;
    break;}
case 989:
#line 4451 "preproc.y"
{ yyval.str = make1_str("start"); ;
    break;}
case 990:
#line 4452 "preproc.y"
{ yyval.str = make1_str("statement"); ;
    break;}
case 991:
#line 4453 "preproc.y"
{ yyval.str = make1_str("stdin"); ;
    break;}
case 992:
#line 4454 "preproc.y"
{ yyval.str = make1_str("stdout"); ;
    break;}
case 993:
#line 4455 "preproc.y"
{ yyval.str = make1_str("time"); ;
    break;}
case 994:
#line 4456 "preproc.y"
{ yyval.str = make1_str("timestamp"); ;
    break;}
case 995:
#line 4457 "preproc.y"
{ yyval.str = make1_str("timezone_hour"); ;
    break;}
case 996:
#line 4458 "preproc.y"
{ yyval.str = make1_str("timezone_minute"); ;
    break;}
case 997:
#line 4459 "preproc.y"
{ yyval.str = make1_str("trigger"); ;
    break;}
case 998:
#line 4460 "preproc.y"
{ yyval.str = make1_str("trusted"); ;
    break;}
case 999:
#line 4461 "preproc.y"
{ yyval.str = make1_str("type"); ;
    break;}
case 1000:
#line 4462 "preproc.y"
{ yyval.str = make1_str("valid"); ;
    break;}
case 1001:
#line 4463 "preproc.y"
{ yyval.str = make1_str("version"); ;
    break;}
case 1002:
#line 4464 "preproc.y"
{ yyval.str = make1_str("zone"); ;
    break;}
case 1003:
#line 4465 "preproc.y"
{ yyval.str = make1_str("at"); ;
    break;}
case 1004:
#line 4466 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 1005:
#line 4467 "preproc.y"
{ yyval.str = make1_str("break"); ;
    break;}
case 1006:
#line 4468 "preproc.y"
{ yyval.str = make1_str("call"); ;
    break;}
case 1007:
#line 4469 "preproc.y"
{ yyval.str = make1_str("connect"); ;
    break;}
case 1008:
#line 4470 "preproc.y"
{ yyval.str = make1_str("connection"); ;
    break;}
case 1009:
#line 4471 "preproc.y"
{ yyval.str = make1_str("continue"); ;
    break;}
case 1010:
#line 4472 "preproc.y"
{ yyval.str = make1_str("deallocate"); ;
    break;}
case 1011:
#line 4473 "preproc.y"
{ yyval.str = make1_str("disconnect"); ;
    break;}
case 1012:
#line 4474 "preproc.y"
{ yyval.str = make1_str("found"); ;
    break;}
case 1013:
#line 4475 "preproc.y"
{ yyval.str = make1_str("go"); ;
    break;}
case 1014:
#line 4476 "preproc.y"
{ yyval.str = make1_str("goto"); ;
    break;}
case 1015:
#line 4477 "preproc.y"
{ yyval.str = make1_str("identified"); ;
    break;}
case 1016:
#line 4478 "preproc.y"
{ yyval.str = make1_str("immediate"); ;
    break;}
case 1017:
#line 4479 "preproc.y"
{ yyval.str = make1_str("indicator"); ;
    break;}
case 1018:
#line 4480 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 1019:
#line 4481 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 1020:
#line 4482 "preproc.y"
{ yyval.str = make1_str("open"); ;
    break;}
case 1021:
#line 4483 "preproc.y"
{ yyval.str = make1_str("prepare"); ;
    break;}
case 1022:
#line 4484 "preproc.y"
{ yyval.str = make1_str("release"); ;
    break;}
case 1023:
#line 4485 "preproc.y"
{ yyval.str = make1_str("section"); ;
    break;}
case 1024:
#line 4486 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 1025:
#line 4487 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1026:
#line 4488 "preproc.y"
{ yyval.str = make1_str("sqlerror"); ;
    break;}
case 1027:
#line 4489 "preproc.y"
{ yyval.str = make1_str("sqlprint"); ;
    break;}
case 1028:
#line 4490 "preproc.y"
{ yyval.str = make1_str("sqlwarning"); ;
    break;}
case 1029:
#line 4491 "preproc.y"
{ yyval.str = make1_str("stop"); ;
    break;}
case 1030:
#line 4492 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 1031:
#line 4493 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 1032:
#line 4494 "preproc.y"
{ yyval.str = make1_str("var"); ;
    break;}
case 1033:
#line 4495 "preproc.y"
{ yyval.str = make1_str("whenever"); ;
    break;}
case 1034:
#line 4507 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1035:
#line 4508 "preproc.y"
{ yyval.str = make1_str("abort"); ;
    break;}
case 1036:
#line 4509 "preproc.y"
{ yyval.str = make1_str("analyze"); ;
    break;}
case 1037:
#line 4510 "preproc.y"
{ yyval.str = make1_str("binary"); ;
    break;}
case 1038:
#line 4511 "preproc.y"
{ yyval.str = make1_str("case"); ;
    break;}
case 1039:
#line 4512 "preproc.y"
{ yyval.str = make1_str("cluster"); ;
    break;}
case 1040:
#line 4513 "preproc.y"
{ yyval.str = make1_str("coalesce"); ;
    break;}
case 1041:
#line 4514 "preproc.y"
{ yyval.str = make1_str("constraint"); ;
    break;}
case 1042:
#line 4515 "preproc.y"
{ yyval.str = make1_str("copy"); ;
    break;}
case 1043:
#line 4516 "preproc.y"
{ yyval.str = make1_str("current"); ;
    break;}
case 1044:
#line 4517 "preproc.y"
{ yyval.str = make1_str("do"); ;
    break;}
case 1045:
#line 4518 "preproc.y"
{ yyval.str = make1_str("else"); ;
    break;}
case 1046:
#line 4519 "preproc.y"
{ yyval.str = make1_str("end"); ;
    break;}
case 1047:
#line 4520 "preproc.y"
{ yyval.str = make1_str("explain"); ;
    break;}
case 1048:
#line 4521 "preproc.y"
{ yyval.str = make1_str("extend"); ;
    break;}
case 1049:
#line 4522 "preproc.y"
{ yyval.str = make1_str("false"); ;
    break;}
case 1050:
#line 4523 "preproc.y"
{ yyval.str = make1_str("foreign"); ;
    break;}
case 1051:
#line 4524 "preproc.y"
{ yyval.str = make1_str("group"); ;
    break;}
case 1052:
#line 4525 "preproc.y"
{ yyval.str = make1_str("listen"); ;
    break;}
case 1053:
#line 4526 "preproc.y"
{ yyval.str = make1_str("load"); ;
    break;}
case 1054:
#line 4527 "preproc.y"
{ yyval.str = make1_str("lock"); ;
    break;}
case 1055:
#line 4528 "preproc.y"
{ yyval.str = make1_str("move"); ;
    break;}
case 1056:
#line 4529 "preproc.y"
{ yyval.str = make1_str("new"); ;
    break;}
case 1057:
#line 4530 "preproc.y"
{ yyval.str = make1_str("none"); ;
    break;}
case 1058:
#line 4531 "preproc.y"
{ yyval.str = make1_str("nullif"); ;
    break;}
case 1059:
#line 4532 "preproc.y"
{ yyval.str = make1_str("order"); ;
    break;}
case 1060:
#line 4533 "preproc.y"
{ yyval.str = make1_str("position"); ;
    break;}
case 1061:
#line 4534 "preproc.y"
{ yyval.str = make1_str("precision"); ;
    break;}
case 1062:
#line 4535 "preproc.y"
{ yyval.str = make1_str("reset"); ;
    break;}
case 1063:
#line 4536 "preproc.y"
{ yyval.str = make1_str("setof"); ;
    break;}
case 1064:
#line 4537 "preproc.y"
{ yyval.str = make1_str("show"); ;
    break;}
case 1065:
#line 4538 "preproc.y"
{ yyval.str = make1_str("table"); ;
    break;}
case 1066:
#line 4539 "preproc.y"
{ yyval.str = make1_str("then"); ;
    break;}
case 1067:
#line 4540 "preproc.y"
{ yyval.str = make1_str("transaction"); ;
    break;}
case 1068:
#line 4541 "preproc.y"
{ yyval.str = make1_str("true"); ;
    break;}
case 1069:
#line 4542 "preproc.y"
{ yyval.str = make1_str("vacuum"); ;
    break;}
case 1070:
#line 4543 "preproc.y"
{ yyval.str = make1_str("verbose"); ;
    break;}
case 1071:
#line 4544 "preproc.y"
{ yyval.str = make1_str("when"); ;
    break;}
case 1072:
#line 4548 "preproc.y"
{
					if (QueryIsRule)
						yyval.str = make1_str("current");
					else
						yyerror("CURRENT used in non-rule query");
				;
    break;}
case 1073:
#line 4555 "preproc.y"
{
					if (QueryIsRule)
						yyval.str = make1_str("new");
					else
						yyerror("NEW used in non-rule query");
				;
    break;}
case 1074:
#line 4571 "preproc.y"
{
			yyval.str = make5_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str, make1_str(","), yyvsp[-1].str);
                ;
    break;}
case 1075:
#line 4575 "preproc.y"
{
                	yyval.str = make1_str("NULL,NULL,NULL,\"DEFAULT\"");
                ;
    break;}
case 1076:
#line 4580 "preproc.y"
{
		       yyval.str = make3_str(make1_str("NULL,"), yyvsp[0].str, make1_str(",NULL"));
		;
    break;}
case 1077:
#line 4585 "preproc.y"
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
case 1078:
#line 4596 "preproc.y"
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
case 1079:
#line 4619 "preproc.y"
{
		  yyval.str = yyvsp[0].str;
		;
    break;}
case 1080:
#line 4623 "preproc.y"
{
		  yyval.str = mm_strdup(yyvsp[0].str);
		  yyval.str[0] = '\"';
		  yyval.str[strlen(yyval.str) - 1] = '\"';
		  free(yyvsp[0].str);
		;
    break;}
case 1081:
#line 4631 "preproc.y"
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
case 1082:
#line 4648 "preproc.y"
{
		  if (strcmp(yyvsp[-1].str, "@") != 0 && strcmp(yyvsp[-1].str, "://") != 0)
		  {
		    sprintf(errortext, "parse error at or near '%s'", yyvsp[-1].str);
		    yyerror(errortext);
		  }

		  yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str);
	        ;
    break;}
case 1083:
#line 4658 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1084:
#line 4659 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1085:
#line 4661 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1086:
#line 4662 "preproc.y"
{ yyval.str = make3_str(yyvsp[-2].str, make1_str("."), yyvsp[0].str); ;
    break;}
case 1087:
#line 4664 "preproc.y"
{ yyval.str = make2_str(make1_str(":"), yyvsp[0].str); ;
    break;}
case 1088:
#line 4665 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1089:
#line 4667 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1090:
#line 4668 "preproc.y"
{ yyval.str = make1_str("NULL"); ;
    break;}
case 1091:
#line 4670 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1092:
#line 4671 "preproc.y"
{ yyval.str = make1_str("NULL,NULL"); ;
    break;}
case 1093:
#line 4674 "preproc.y"
{
                        yyval.str = make2_str(yyvsp[0].str, make1_str(",NULL"));
	        ;
    break;}
case 1094:
#line 4678 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1095:
#line 4682 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-3].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1096:
#line 4686 "preproc.y"
{
        		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
                ;
    break;}
case 1097:
#line 4690 "preproc.y"
{ if (yyvsp[0].str[0] == '\"')
				yyval.str = yyvsp[0].str;
			  else
				yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\""));
			;
    break;}
case 1098:
#line 4695 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1099:
#line 4696 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1100:
#line 4699 "preproc.y"
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
case 1101:
#line 4723 "preproc.y"
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
case 1102:
#line 4735 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1103:
#line 4742 "preproc.y"
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
case 1104:
#line 4784 "preproc.y"
{ yyval.str = make3_str(make1_str("ECPGdeallocate(__LINE__, \""), yyvsp[0].str, make1_str("\");")); ;
    break;}
case 1105:
#line 4790 "preproc.y"
{
		fputs("/* exec sql begin declare section */", yyout);
	        output_line_number();
	;
    break;}
case 1106:
#line 4795 "preproc.y"
{
		fprintf(yyout, "%s/* exec sql end declare section */", yyvsp[-1].str);
		free(yyvsp[-1].str);
		output_line_number();
	;
    break;}
case 1107:
#line 4801 "preproc.y"
{;
    break;}
case 1108:
#line 4803 "preproc.y"
{;
    break;}
case 1109:
#line 4806 "preproc.y"
{
		yyval.str = make1_str("");
	;
    break;}
case 1110:
#line 4810 "preproc.y"
{
		yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
	;
    break;}
case 1111:
#line 4815 "preproc.y"
{
		actual_storage[struct_level] = mm_strdup(yyvsp[0].str);
	;
    break;}
case 1112:
#line 4819 "preproc.y"
{
		actual_type[struct_level].type_enum = yyvsp[0].type.type_enum;
		actual_type[struct_level].type_dimension = yyvsp[0].type.type_dimension;
		actual_type[struct_level].type_index = yyvsp[0].type.type_index;
	;
    break;}
case 1113:
#line 4825 "preproc.y"
{
 		yyval.str = cat4_str(yyvsp[-5].str, yyvsp[-3].type.type_str, yyvsp[-1].str, make1_str(";\n"));
	;
    break;}
case 1114:
#line 4829 "preproc.y"
{ yyval.str = make1_str("extern"); ;
    break;}
case 1115:
#line 4830 "preproc.y"
{ yyval.str = make1_str("static"); ;
    break;}
case 1116:
#line 4831 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1117:
#line 4832 "preproc.y"
{ yyval.str = make1_str("const"); ;
    break;}
case 1118:
#line 4833 "preproc.y"
{ yyval.str = make1_str("register"); ;
    break;}
case 1119:
#line 4834 "preproc.y"
{ yyval.str = make1_str("auto"); ;
    break;}
case 1120:
#line 4835 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1121:
#line 4838 "preproc.y"
{
			yyval.type.type_enum = yyvsp[0].type_enum;
			yyval.type.type_str = mm_strdup(ECPGtype_name(yyvsp[0].type_enum));
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1122:
#line 4845 "preproc.y"
{
			yyval.type.type_enum = ECPGt_varchar;
			yyval.type.type_str = make1_str("");
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1123:
#line 4852 "preproc.y"
{
			yyval.type.type_enum = ECPGt_struct;
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1124:
#line 4859 "preproc.y"
{
			yyval.type.type_enum = ECPGt_union;
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1125:
#line 4866 "preproc.y"
{
			yyval.type.type_str = yyvsp[0].str;
			yyval.type.type_enum = ECPGt_int;
		
	yyval.type.type_dimension = -1;
  			yyval.type.type_index = -1;
		;
    break;}
case 1126:
#line 4874 "preproc.y"
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
case 1127:
#line 4886 "preproc.y"
{
		yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1128:
#line 4890 "preproc.y"
{ yyval.str = cat2_str(make1_str("enum"), yyvsp[0].str); ;
    break;}
case 1129:
#line 4893 "preproc.y"
{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1130:
#line 4900 "preproc.y"
{
	    ECPGfree_struct_member(struct_member_list[struct_level]);
	    free(actual_storage[struct_level--]);
	    yyval.str = cat4_str(yyvsp[-3].str, make1_str("{"), yyvsp[-1].str, make1_str("}"));
	;
    break;}
case 1131:
#line 4907 "preproc.y"
{
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    yyval.str = cat2_str(make1_str("struct"), yyvsp[0].str);
	;
    break;}
case 1132:
#line 4915 "preproc.y"
{
            struct_member_list[struct_level++] = NULL;
            if (struct_level >= STRUCT_DEPTH)
                 yyerror("Too many levels in nested structure definition");
	    yyval.str = cat2_str(make1_str("union"), yyvsp[0].str);
	;
    break;}
case 1133:
#line 4922 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1134:
#line 4923 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1135:
#line 4925 "preproc.y"
{ yyval.type_enum = ECPGt_short; ;
    break;}
case 1136:
#line 4926 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_short; ;
    break;}
case 1137:
#line 4927 "preproc.y"
{ yyval.type_enum = ECPGt_int; ;
    break;}
case 1138:
#line 4928 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_int; ;
    break;}
case 1139:
#line 4929 "preproc.y"
{ yyval.type_enum = ECPGt_long; ;
    break;}
case 1140:
#line 4930 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_long; ;
    break;}
case 1141:
#line 4931 "preproc.y"
{ yyval.type_enum = ECPGt_float; ;
    break;}
case 1142:
#line 4932 "preproc.y"
{ yyval.type_enum = ECPGt_double; ;
    break;}
case 1143:
#line 4933 "preproc.y"
{ yyval.type_enum = ECPGt_bool; ;
    break;}
case 1144:
#line 4934 "preproc.y"
{ yyval.type_enum = ECPGt_char; ;
    break;}
case 1145:
#line 4935 "preproc.y"
{ yyval.type_enum = ECPGt_unsigned_char; ;
    break;}
case 1146:
#line 4937 "preproc.y"
{ yyval.type_enum = ECPGt_varchar; ;
    break;}
case 1147:
#line 4940 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 1148:
#line 4944 "preproc.y"
{
		yyval.str = cat3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
	;
    break;}
case 1149:
#line 4949 "preproc.y"
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
case 1150:
#line 5023 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1151:
#line 5024 "preproc.y"
{ yyval.str = make2_str(make1_str("="), yyvsp[0].str); ;
    break;}
case 1152:
#line 5026 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1153:
#line 5027 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 1154:
#line 5034 "preproc.y"
{
		/* this is only supported for compatibility */
		yyval.str = cat3_str(make1_str("/* declare statement"), yyvsp[0].str, make1_str("*/"));
	;
    break;}
case 1155:
#line 5041 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1156:
#line 5043 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1157:
#line 5044 "preproc.y"
{ yyval.str = make1_str("CURRENT"); ;
    break;}
case 1158:
#line 5045 "preproc.y"
{ yyval.str = make1_str("ALL"); ;
    break;}
case 1159:
#line 5046 "preproc.y"
{ yyval.str = make1_str("CURRENT"); ;
    break;}
case 1160:
#line 5048 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1161:
#line 5049 "preproc.y"
{ yyval.str = make1_str("DEFAULT"); ;
    break;}
case 1162:
#line 5055 "preproc.y"
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
case 1163:
#line 5068 "preproc.y"
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
case 1164:
#line 5079 "preproc.y"
{
		yyval.str = make1_str("?");
	;
    break;}
case 1166:
#line 5084 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1167:
#line 5090 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1168:
#line 5095 "preproc.y"
{
		yyval.str = yyvsp[-1].str;
;
    break;}
case 1169:
#line 5099 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1170:
#line 5100 "preproc.y"
{
					/* yyerror ("open cursor with variables not implemented yet"); */
					yyval.str = make1_str("");
				;
    break;}
case 1173:
#line 5112 "preproc.y"
{
		yyval.str = make4_str(make1_str("\""), yyvsp[-2].str, make1_str("\", "), yyvsp[0].str);
	;
    break;}
case 1174:
#line 5122 "preproc.y"
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
case 1175:
#line 5138 "preproc.y"
{
				yyval.str = yyvsp[0].str;
                        ;
    break;}
case 1176:
#line 5146 "preproc.y"
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
case 1177:
#line 5188 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1178:
#line 5194 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1179:
#line 5200 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1180:
#line 5206 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1181:
#line 5212 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 1182:
#line 5220 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1183:
#line 5226 "preproc.y"
{
                            yyval.index.index1 = 0;
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat2_str(make1_str("[]"), yyvsp[0].index.str);
                        ;
    break;}
case 1184:
#line 5232 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1185:
#line 5238 "preproc.y"
{
                            yyval.index.index1 = atol(yyvsp[-2].str);
                            yyval.index.index2 = yyvsp[0].index.index1;
                            yyval.index.str = cat4_str(make1_str("["), yyvsp[-2].str, make1_str("]"), yyvsp[0].index.str);
                        ;
    break;}
case 1186:
#line 5244 "preproc.y"
{
                            yyval.index.index1 = -1;
                            yyval.index.index2 = -1;
                            yyval.index.str= make1_str("");
                        ;
    break;}
case 1187:
#line 5250 "preproc.y"
{ yyval.str = make1_str("reference"); ;
    break;}
case 1188:
#line 5251 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1189:
#line 5254 "preproc.y"
{
		yyval.type.type_str = make1_str("char");
                yyval.type.type_enum = ECPGt_char;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1190:
#line 5261 "preproc.y"
{
		yyval.type.type_str = make1_str("varchar");
                yyval.type.type_enum = ECPGt_varchar;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1191:
#line 5268 "preproc.y"
{
		yyval.type.type_str = make1_str("float");
                yyval.type.type_enum = ECPGt_float;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1192:
#line 5275 "preproc.y"
{
		yyval.type.type_str = make1_str("double");
                yyval.type.type_enum = ECPGt_double;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1193:
#line 5282 "preproc.y"
{
		yyval.type.type_str = make1_str("int");
       	        yyval.type.type_enum = ECPGt_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1194:
#line 5289 "preproc.y"
{
		yyval.type.type_str = make1_str("int");
       	        yyval.type.type_enum = ECPGt_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1195:
#line 5296 "preproc.y"
{
		yyval.type.type_str = make1_str("short");
       	        yyval.type.type_enum = ECPGt_short;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1196:
#line 5303 "preproc.y"
{
		yyval.type.type_str = make1_str("long");
       	        yyval.type.type_enum = ECPGt_long;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1197:
#line 5310 "preproc.y"
{
		yyval.type.type_str = make1_str("bool");
       	        yyval.type.type_enum = ECPGt_bool;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1198:
#line 5317 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned int");
       	        yyval.type.type_enum = ECPGt_unsigned_int;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1199:
#line 5324 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned short");
       	        yyval.type.type_enum = ECPGt_unsigned_short;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1200:
#line 5331 "preproc.y"
{
		yyval.type.type_str = make1_str("unsigned long");
       	        yyval.type.type_enum = ECPGt_unsigned_long;
		yyval.type.type_index = -1;
		yyval.type.type_dimension = -1;
	;
    break;}
case 1201:
#line 5338 "preproc.y"
{
		struct_member_list[struct_level++] = NULL;
		if (struct_level >= STRUCT_DEPTH)
        		yyerror("Too many levels in nested structure definition");
	;
    break;}
case 1202:
#line 5343 "preproc.y"
{
		ECPGfree_struct_member(struct_member_list[struct_level--]);
		yyval.type.type_str = cat3_str(make1_str("struct {"), yyvsp[-1].str, make1_str("}"));
		yyval.type.type_enum = ECPGt_struct;
                yyval.type.type_index = -1;
                yyval.type.type_dimension = -1;
	;
    break;}
case 1203:
#line 5351 "preproc.y"
{
		struct_member_list[struct_level++] = NULL;
		if (struct_level >= STRUCT_DEPTH)
        		yyerror("Too many levels in nested structure definition");
	;
    break;}
case 1204:
#line 5356 "preproc.y"
{
		ECPGfree_struct_member(struct_member_list[struct_level--]);
		yyval.type.type_str = cat3_str(make1_str("union {"), yyvsp[-1].str, make1_str("}"));
		yyval.type.type_enum = ECPGt_union;
                yyval.type.type_index = -1;
                yyval.type.type_dimension = -1;
	;
    break;}
case 1205:
#line 5364 "preproc.y"
{
		struct typedefs *this = get_typedef(yyvsp[0].str);

		yyval.type.type_str = mm_strdup(yyvsp[0].str);
		yyval.type.type_enum = this->type->type_enum;
		yyval.type.type_dimension = this->type->type_dimension;
		yyval.type.type_index = this->type->type_index;
		struct_member_list[struct_level] = this->struct_member_list;
	;
    break;}
case 1208:
#line 5377 "preproc.y"
{
		yyval.str = make1_str("");
	;
    break;}
case 1209:
#line 5381 "preproc.y"
{
		yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
	;
    break;}
case 1210:
#line 5387 "preproc.y"
{
		actual_type[struct_level].type_enum = yyvsp[0].type.type_enum;
		actual_type[struct_level].type_dimension = yyvsp[0].type.type_dimension;
		actual_type[struct_level].type_index = yyvsp[0].type.type_index;
	;
    break;}
case 1211:
#line 5393 "preproc.y"
{
		yyval.str = cat3_str(yyvsp[-3].type.type_str, yyvsp[-1].str, make1_str(";"));
	;
    break;}
case 1212:
#line 5398 "preproc.y"
{
		yyval.str = yyvsp[0].str;
	;
    break;}
case 1213:
#line 5402 "preproc.y"
{
		yyval.str = make3_str(yyvsp[-2].str, make1_str(","), yyvsp[0].str);
	;
    break;}
case 1214:
#line 5407 "preproc.y"
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
case 1215:
#line 5478 "preproc.y"
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
case 1216:
#line 5532 "preproc.y"
{
	when_error.code = yyvsp[0].action.code;
	when_error.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever sqlerror "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1217:
#line 5537 "preproc.y"
{
	when_nf.code = yyvsp[0].action.code;
	when_nf.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever not found "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1218:
#line 5542 "preproc.y"
{
	when_warn.code = yyvsp[0].action.code;
	when_warn.command = yyvsp[0].action.command;
	yyval.str = cat3_str(make1_str("/* exec sql whenever sql_warning "), yyvsp[0].action.str, make1_str("; */\n"));
;
    break;}
case 1219:
#line 5548 "preproc.y"
{
	yyval.action.code = W_NOTHING;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("continue");
;
    break;}
case 1220:
#line 5553 "preproc.y"
{
	yyval.action.code = W_SQLPRINT;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("sqlprint");
;
    break;}
case 1221:
#line 5558 "preproc.y"
{
	yyval.action.code = W_STOP;
	yyval.action.command = NULL;
	yyval.action.str = make1_str("stop");
;
    break;}
case 1222:
#line 5563 "preproc.y"
{
        yyval.action.code = W_GOTO;
        yyval.action.command = strdup(yyvsp[0].str);
	yyval.action.str = cat2_str(make1_str("goto "), yyvsp[0].str);
;
    break;}
case 1223:
#line 5568 "preproc.y"
{
        yyval.action.code = W_GOTO;
        yyval.action.command = strdup(yyvsp[0].str);
	yyval.action.str = cat2_str(make1_str("goto "), yyvsp[0].str);
;
    break;}
case 1224:
#line 5573 "preproc.y"
{
	yyval.action.code = W_DO;
	yyval.action.command = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
	yyval.action.str = cat2_str(make1_str("do"), mm_strdup(yyval.action.command));
;
    break;}
case 1225:
#line 5578 "preproc.y"
{
        yyval.action.code = W_BREAK;
        yyval.action.command = NULL;
        yyval.action.str = make1_str("break");
;
    break;}
case 1226:
#line 5583 "preproc.y"
{
	yyval.action.code = W_DO;
	yyval.action.command = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")"));
	yyval.action.str = cat2_str(make1_str("call"), mm_strdup(yyval.action.command));
;
    break;}
case 1227:
#line 5591 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str);
				;
    break;}
case 1228:
#line 5595 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 1229:
#line 5597 "preproc.y"
{	yyval.str = yyvsp[0].str;  ;
    break;}
case 1230:
#line 5599 "preproc.y"
{
					yyval.str = yyvsp[0].str;
				;
    break;}
case 1231:
#line 5603 "preproc.y"
{	yyval.str = cat2_str(make1_str("-"), yyvsp[0].str); ;
    break;}
case 1232:
#line 5605 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("+"), yyvsp[0].str); ;
    break;}
case 1233:
#line 5607 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("-"), yyvsp[0].str); ;
    break;}
case 1234:
#line 5609 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("/"), yyvsp[0].str); ;
    break;}
case 1235:
#line 5611 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("%"), yyvsp[0].str); ;
    break;}
case 1236:
#line 5613 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("*"), yyvsp[0].str); ;
    break;}
case 1237:
#line 5615 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("<"), yyvsp[0].str); ;
    break;}
case 1238:
#line 5617 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str(">"), yyvsp[0].str); ;
    break;}
case 1239:
#line 5619 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("="), yyvsp[0].str); ;
    break;}
case 1240:
#line 5623 "preproc.y"
{	yyval.str = cat2_str(make1_str(";"), yyvsp[0].str); ;
    break;}
case 1241:
#line 5625 "preproc.y"
{	yyval.str = cat2_str(make1_str("|"), yyvsp[0].str); ;
    break;}
case 1242:
#line 5627 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-2].str, make1_str("::"), yyvsp[0].str);
				;
    break;}
case 1243:
#line 5631 "preproc.y"
{
					yyval.str = cat3_str(make2_str(make1_str("cast("), yyvsp[-3].str), make1_str("as"), make2_str(yyvsp[-1].str, make1_str(")")));
				;
    break;}
case 1244:
#line 5635 "preproc.y"
{	yyval.str = make3_str(make1_str("("), yyvsp[-1].str, make1_str(")")); ;
    break;}
case 1245:
#line 5637 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, yyvsp[-1].str, yyvsp[0].str);	;
    break;}
case 1246:
#line 5639 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("like"), yyvsp[0].str); ;
    break;}
case 1247:
#line 5641 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-3].str, make1_str("not like"), yyvsp[0].str); ;
    break;}
case 1248:
#line 5643 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1249:
#line 5645 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1250:
#line 5647 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-3].str, make1_str("(*)")); 
				;
    break;}
case 1251:
#line 5651 "preproc.y"
{
					yyval.str = cat2_str(yyvsp[-2].str, make1_str("()")); 
				;
    break;}
case 1252:
#line 5655 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-3].str, make1_str("("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1253:
#line 5659 "preproc.y"
{
					yyval.str = make1_str("current_date");
				;
    break;}
case 1254:
#line 5663 "preproc.y"
{
					yyval.str = make1_str("current_time");
				;
    break;}
case 1255:
#line 5667 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIME(%s) precision not implemented; zero used instead", yyvsp[-1].str);
					yyval.str = make1_str("current_time");
				;
    break;}
case 1256:
#line 5673 "preproc.y"
{
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 1257:
#line 5677 "preproc.y"
{
					if (atol(yyvsp[-1].str) != 0)
						fprintf(stderr,"CURRENT_TIMESTAMP(%s) precision not implemented; zero used instead",yyvsp[-1].str);
					yyval.str = make1_str("current_timestamp");
				;
    break;}
case 1258:
#line 5683 "preproc.y"
{
					yyval.str = make1_str("current_user");
				;
    break;}
case 1259:
#line 5687 "preproc.y"
{
					yyval.str = make3_str(make1_str("exists("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1260:
#line 5691 "preproc.y"
{
					yyval.str = make3_str(make1_str("extract("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1261:
#line 5695 "preproc.y"
{
					yyval.str = make3_str(make1_str("position("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1262:
#line 5699 "preproc.y"
{
					yyval.str = make3_str(make1_str("substring("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1263:
#line 5704 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(both"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1264:
#line 5708 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(leading"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1265:
#line 5712 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim(trailing"), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1266:
#line 5716 "preproc.y"
{
					yyval.str = make3_str(make1_str("trim("), yyvsp[-1].str, make1_str(")"));
				;
    break;}
case 1267:
#line 5720 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("isnull")); ;
    break;}
case 1268:
#line 5722 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is null")); ;
    break;}
case 1269:
#line 5724 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-1].str, make1_str("notnull")); ;
    break;}
case 1270:
#line 5726 "preproc.y"
{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not null")); ;
    break;}
case 1271:
#line 5733 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is true")); }
				;
    break;}
case 1272:
#line 5737 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not false")); }
				;
    break;}
case 1273:
#line 5741 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-2].str, make1_str("is false")); }
				;
    break;}
case 1274:
#line 5745 "preproc.y"
{
				{	yyval.str = cat2_str(yyvsp[-3].str, make1_str("is not true")); }
				;
    break;}
case 1275:
#line 5749 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-4].str, make1_str("between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 1276:
#line 5753 "preproc.y"
{
					yyval.str = cat5_str(yyvsp[-5].str, make1_str("not between"), yyvsp[-2].str, make1_str("and"), yyvsp[0].str); 
				;
    break;}
case 1277:
#line 5757 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1278:
#line 5761 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("not in ("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1279:
#line 5765 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-4].str, yyvsp[-3].str, make3_str(make1_str("("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1280:
#line 5769 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("+("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1281:
#line 5773 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("-("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1282:
#line 5777 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("/("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1283:
#line 5781 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("%("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1284:
#line 5785 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("*("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1285:
#line 5789 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("<("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1286:
#line 5793 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str(">("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1287:
#line 5797 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-4].str, make1_str("=("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1288:
#line 5801 "preproc.y"
{
					yyval.str = cat3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("any ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1289:
#line 5805 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+ any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1290:
#line 5809 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("- any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1291:
#line 5813 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/ any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1292:
#line 5817 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("% any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1293:
#line 5821 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("* any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1294:
#line 5825 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("< any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1295:
#line 5829 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("> any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1296:
#line 5833 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("= any("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1297:
#line 5837 "preproc.y"
{
					yyval.str = make3_str(yyvsp[-5].str, yyvsp[-4].str, make3_str(make1_str("all ("), yyvsp[-1].str, make1_str(")"))); 
				;
    break;}
case 1298:
#line 5841 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("+ all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1299:
#line 5845 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("- all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1300:
#line 5849 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("/ all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1301:
#line 5853 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("% all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1302:
#line 5857 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("* all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1303:
#line 5861 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("< all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1304:
#line 5865 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("> all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1305:
#line 5869 "preproc.y"
{
					yyval.str = make4_str(yyvsp[-5].str, make1_str("=all("), yyvsp[-1].str, make1_str(")")); 
				;
    break;}
case 1306:
#line 5873 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("and"), yyvsp[0].str); ;
    break;}
case 1307:
#line 5875 "preproc.y"
{	yyval.str = cat3_str(yyvsp[-2].str, make1_str("or"), yyvsp[0].str); ;
    break;}
case 1308:
#line 5877 "preproc.y"
{	yyval.str = cat2_str(make1_str("not"), yyvsp[0].str); ;
    break;}
case 1309:
#line 5879 "preproc.y"
{ 	yyval.str = yyvsp[0].str; ;
    break;}
case 1312:
#line 5884 "preproc.y"
{ reset_variables();;
    break;}
case 1313:
#line 5886 "preproc.y"
{ yyval.str = make1_str(""); ;
    break;}
case 1314:
#line 5887 "preproc.y"
{ yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1315:
#line 5889 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1316:
#line 5890 "preproc.y"
{ yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str); ;
    break;}
case 1317:
#line 5892 "preproc.y"
{
		add_variable(&argsresult, find_variable(yyvsp[-1].str), (yyvsp[0].str == NULL) ? &no_indicator : find_variable(yyvsp[0].str)); 
;
    break;}
case 1318:
#line 5896 "preproc.y"
{
		add_variable(&argsinsert, find_variable(yyvsp[-1].str), (yyvsp[0].str == NULL) ? &no_indicator : find_variable(yyvsp[0].str)); 
;
    break;}
case 1319:
#line 5900 "preproc.y"
{
		add_variable(&argsinsert, find_variable(yyvsp[0].str), &no_indicator); 
		yyval.str = make1_str("?");
;
    break;}
case 1320:
#line 5905 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1321:
#line 5907 "preproc.y"
{ yyval.str = NULL; ;
    break;}
case 1322:
#line 5908 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1323:
#line 5909 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1324:
#line 5910 "preproc.y"
{ check_indicator((find_variable(yyvsp[0].str))->type); yyval.str = yyvsp[0].str; ;
    break;}
case 1325:
#line 5912 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1326:
#line 5913 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1327:
#line 5918 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1328:
#line 5920 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1329:
#line 5922 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1330:
#line 5924 "preproc.y"
{
			yyval.str = make2_str(yyvsp[-1].str, yyvsp[0].str);
		;
    break;}
case 1332:
#line 5928 "preproc.y"
{ yyval.str = make1_str(";"); ;
    break;}
case 1333:
#line 5930 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1334:
#line 5931 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1335:
#line 5932 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1336:
#line 5933 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1337:
#line 5934 "preproc.y"
{ yyval.str = make1_str("*"); ;
    break;}
case 1338:
#line 5935 "preproc.y"
{ yyval.str = make1_str("auto"); ;
    break;}
case 1339:
#line 5936 "preproc.y"
{ yyval.str = make1_str("bool"); ;
    break;}
case 1340:
#line 5937 "preproc.y"
{ yyval.str = make1_str("char"); ;
    break;}
case 1341:
#line 5938 "preproc.y"
{ yyval.str = make1_str("const"); ;
    break;}
case 1342:
#line 5939 "preproc.y"
{ yyval.str = make1_str("double"); ;
    break;}
case 1343:
#line 5940 "preproc.y"
{ yyval.str = make1_str("enum"); ;
    break;}
case 1344:
#line 5941 "preproc.y"
{ yyval.str = make1_str("extern"); ;
    break;}
case 1345:
#line 5942 "preproc.y"
{ yyval.str = make1_str("float"); ;
    break;}
case 1346:
#line 5943 "preproc.y"
{ yyval.str = make1_str("int"); ;
    break;}
case 1347:
#line 5944 "preproc.y"
{ yyval.str = make1_str("long"); ;
    break;}
case 1348:
#line 5945 "preproc.y"
{ yyval.str = make1_str("register"); ;
    break;}
case 1349:
#line 5946 "preproc.y"
{ yyval.str = make1_str("short"); ;
    break;}
case 1350:
#line 5947 "preproc.y"
{ yyval.str = make1_str("signed"); ;
    break;}
case 1351:
#line 5948 "preproc.y"
{ yyval.str = make1_str("static"); ;
    break;}
case 1352:
#line 5949 "preproc.y"
{ yyval.str = make1_str("struct"); ;
    break;}
case 1353:
#line 5950 "preproc.y"
{ yyval.str = make1_str("union"); ;
    break;}
case 1354:
#line 5951 "preproc.y"
{ yyval.str = make1_str("unsigned"); ;
    break;}
case 1355:
#line 5952 "preproc.y"
{ yyval.str = make1_str("varchar"); ;
    break;}
case 1356:
#line 5953 "preproc.y"
{ yyval.str = make_name(); ;
    break;}
case 1357:
#line 5954 "preproc.y"
{ yyval.str = make1_str("["); ;
    break;}
case 1358:
#line 5955 "preproc.y"
{ yyval.str = make1_str("]"); ;
    break;}
case 1359:
#line 5956 "preproc.y"
{ yyval.str = make1_str("("); ;
    break;}
case 1360:
#line 5957 "preproc.y"
{ yyval.str = make1_str(")"); ;
    break;}
case 1361:
#line 5958 "preproc.y"
{ yyval.str = make1_str("="); ;
    break;}
case 1362:
#line 5959 "preproc.y"
{ yyval.str = make1_str(","); ;
    break;}
case 1363:
#line 5961 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1364:
#line 5962 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\""));;
    break;}
case 1365:
#line 5963 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1366:
#line 5964 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1367:
#line 5965 "preproc.y"
{ yyval.str = make1_str(","); ;
    break;}
case 1368:
#line 5967 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1369:
#line 5968 "preproc.y"
{ yyval.str = make3_str(make1_str("\""), yyvsp[0].str, make1_str("\"")); ;
    break;}
case 1370:
#line 5969 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1371:
#line 5970 "preproc.y"
{ yyval.str = yyvsp[0].str; ;
    break;}
case 1372:
#line 5971 "preproc.y"
{ yyval.str = make3_str(make1_str("{"), yyvsp[-1].str, make1_str("}")); ;
    break;}
case 1373:
#line 5973 "preproc.y"
{
    braces_open++;
    yyval.str = make1_str("{");
;
    break;}
case 1374:
#line 5978 "preproc.y"
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
#line 5983 "preproc.y"


void yyerror(char * error)
{
    fprintf(stderr, "%s:%d: %s\n", input_filename, yylineno, error);
    exit(PARSE_ERROR);
}
