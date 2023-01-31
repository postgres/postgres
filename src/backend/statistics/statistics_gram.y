%{
#include "postgres.h"

#include "statistics/extended_stats_internal.h"
#include "statistics/statistics.h"

MVNDistinct *mvndistinct_parse_result;
MVDependencies *mvdependencies_parse_result;
/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it use palloc instead of malloc.  This prevents
 * memory leaks if we error out during parsing.  Note this only works with
 * bison >= 2.0.  However, in bison 1.875 the default is to use alloca()
 * if possible, so there's not really much problem anyhow, at least if
 * you're building with gcc.
 */
#define YYMALLOC palloc
#define YYFREE   pfree

%}

%expect 0
%name-prefix="statistic_yy"


%union {
		uint32					uintval;
		double					doubleval;

		MVNDistinct				*ndistinct;
		MVNDistinctItem				*ndistinct_item;

		MVDependencies				*dependencies;
		MVDependency				*dependency;

		Bitmapset				*bitmap;
		List					*list;
}

/* Non-keyword tokens */
%token <uintval> UCONST
%token <doubleval> DOUBLE
%token ARROW

%type <ndistinct>	ndistinct
%type <ndistinct_item>	ndistinct_item
%type <list>	ndistinct_item_list

%type <dependencies>	dependencies
%type <list>	dependency_item_list
%type <dependency>	dependency_item

%type <bitmap>	attrs

%%

extended_statistic:
	ndistinct { } |
	dependencies { }
	;

/*
 * "ndistinct" rule helps to parse the input string recursively and stores the output into MVNDistinct structure.
 * Exmple:
 * 	intput : '{"1, 2": 1,"2, 3": 2, "3, 1", 2}'
 * 	output : returns MVNDistinct object
*/
ndistinct:
	'{' ndistinct_item_list '}'
		{
			ListCell *cell;
			$$ = palloc0(MAXALIGN(offsetof(MVNDistinct, items)) +
						 list_length($2) * sizeof(MVNDistinctItem));
			mvndistinct_parse_result = $$;
			$$->magic = STATS_NDISTINCT_MAGIC;
			$$->type = STATS_NDISTINCT_TYPE_BASIC;
			$$->nitems = list_length($2);
			MVNDistinctItem *pointer = $$->items;
			foreach (cell, $2)
			{
				memcpy(pointer, lfirst(cell), sizeof(MVNDistinctItem));
				pointer += 1;
			}
		}
	;

ndistinct_item_list:
	ndistinct_item_list ',' ndistinct_item
		{
			$$ = lappend($1, $3);
		}
	| ndistinct_item { $$ = lappend(NIL, $1);}
	;

ndistinct_item:
	'"' attrs '"' ':' DOUBLE
		{
			int attrCount = 0;
			$$ = (MVNDistinctItem *)palloc0(sizeof(MVNDistinctItem));
			$$->attributes = build_attnums_array($2, 0, &attrCount);
			$$->nattributes = attrCount;
			$$->ndistinct = $5;
		}
	;

attrs:
	attrs ',' UCONST
		{
			$$ = bms_add_member($1, $3);

		}
	| UCONST ',' UCONST
		{
			$$ = bms_make_singleton($1);
			$$ = bms_add_member($$, $3);
		}
	;

/*
 * "dependencies" rule helps to parse the input string recursively and stores the output into MVDependencies structure.
 * example:
 *	intput : '{"1 => 2": 1.000000, "2 => 3": 2.000000}'
 * 	output : returns MVDependencies	object
*/
dependencies:
	'{' dependency_item_list '}'
		{
			$$ = palloc0(MAXALIGN(offsetof(MVDependencies, deps)) + list_length($2) * sizeof(MVDependency *));
			mvdependencies_parse_result = $$;

			$$->magic = STATS_DEPS_MAGIC;
			$$->type = STATS_DEPS_TYPE_BASIC;
			$$->ndeps = list_length($2);

			for (int i=0; i<$$->ndeps; i++)
			{
				$$->deps[i] = list_nth($2, i);
			}
		}
	;

dependency_item_list:
	dependency_item_list ',' dependency_item
		{
			$$ = lappend($1, $3);
		}
	| dependency_item { $$ = lappend(NIL, $1);}
	;

dependency_item:
	'"' attrs ARROW UCONST '"' ':' DOUBLE
	{
		int attrCount = 0;
		AttrNumber *ptr = build_attnums_array($2, 0, &attrCount);
		$$ = (MVDependency *)palloc0(sizeof(MVDependency) + sizeof(AttrNumber) * (attrCount + 1));
		$$->nattributes = attrCount + 1;
		$$->degree = $7;
		for (int i = 0; i < attrCount; i++)
		{
			$$->attributes[i] = *(ptr+i);
		}
		$$->attributes[$$->nattributes - 1] = $4;
	}
	| '"' UCONST ARROW UCONST '"' ':' DOUBLE
	{
		$$ = (MVDependency *)palloc0(sizeof(MVDependency) + sizeof(AttrNumber) * 2);
		$$->nattributes = 2;
		$$->degree = $7;
		$$->attributes[0] = $2;
		$$->attributes[1] = $4;
	}
	;
%%
