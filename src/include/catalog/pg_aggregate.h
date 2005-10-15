/*-------------------------------------------------------------------------
 *
 * pg_aggregate.h
 *	  definition of the system "aggregate" relation (pg_aggregate)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_aggregate.h,v 1.52 2005/10/15 02:49:42 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AGGREGATE_H
#define PG_AGGREGATE_H

#include "nodes/pg_list.h"

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------------------------------------------------------
 *		pg_aggregate definition.
 *
 *		cpp turns this into typedef struct FormData_pg_aggregate
 *
 *	aggfnoid			pg_proc OID of the aggregate itself
 *	aggtransfn			transition function
 *	aggfinalfn			final function (0 if none)
 *	aggsortop			associated sort operator (0 if none)
 *	aggtranstype		type of aggregate's transition (state) data
 *	agginitval			initial value for transition state (can be NULL)
 * ----------------------------------------------------------------
 */
#define AggregateRelationId  2600

CATALOG(pg_aggregate,2600) BKI_WITHOUT_OIDS
{
	regproc		aggfnoid;
	regproc		aggtransfn;
	regproc		aggfinalfn;
	Oid			aggsortop;
	Oid			aggtranstype;
	text		agginitval;		/* VARIABLE LENGTH FIELD */
} FormData_pg_aggregate;

/* ----------------
 *		Form_pg_aggregate corresponds to a pointer to a tuple with
 *		the format of pg_aggregate relation.
 * ----------------
 */
typedef FormData_pg_aggregate *Form_pg_aggregate;

/* ----------------
 *		compiler constants for pg_aggregate
 * ----------------
 */

#define Natts_pg_aggregate				6
#define Anum_pg_aggregate_aggfnoid		1
#define Anum_pg_aggregate_aggtransfn	2
#define Anum_pg_aggregate_aggfinalfn	3
#define Anum_pg_aggregate_aggsortop		4
#define Anum_pg_aggregate_aggtranstype	5
#define Anum_pg_aggregate_agginitval	6


/* ----------------
 * initial contents of pg_aggregate
 * ---------------
 */

/* avg */
DATA(insert ( 2100	int8_accum		numeric_avg		0	1231	"{0,0,0}" ));
DATA(insert ( 2101	int4_avg_accum	int8_avg		0	1016	"{0,0}" ));
DATA(insert ( 2102	int2_avg_accum	int8_avg		0	1016	"{0,0}" ));
DATA(insert ( 2103	numeric_accum	numeric_avg		0	1231	"{0,0,0}" ));
DATA(insert ( 2104	float4_accum	float8_avg		0	1022	"{0,0,0}" ));
DATA(insert ( 2105	float8_accum	float8_avg		0	1022	"{0,0,0}" ));
DATA(insert ( 2106	interval_accum	interval_avg	0	1187	"{0 second,0 second}" ));

/* sum */
DATA(insert ( 2107	int8_sum		-				0	1700	_null_ ));
DATA(insert ( 2108	int4_sum		-				0	20		_null_ ));
DATA(insert ( 2109	int2_sum		-				0	20		_null_ ));
DATA(insert ( 2110	float4pl		-				0	700		_null_ ));
DATA(insert ( 2111	float8pl		-				0	701		_null_ ));
DATA(insert ( 2112	cash_pl			-				0	790		_null_ ));
DATA(insert ( 2113	interval_pl		-				0	1186	_null_ ));
DATA(insert ( 2114	numeric_add		-				0	1700	_null_ ));

/* max */
DATA(insert ( 2115	int8larger		-				413		20		_null_ ));
DATA(insert ( 2116	int4larger		-				521		23		_null_ ));
DATA(insert ( 2117	int2larger		-				520		21		_null_ ));
DATA(insert ( 2118	oidlarger		-				610		26		_null_ ));
DATA(insert ( 2119	float4larger	-				623		700		_null_ ));
DATA(insert ( 2120	float8larger	-				674		701		_null_ ));
DATA(insert ( 2121	int4larger		-				563		702		_null_ ));
DATA(insert ( 2122	date_larger		-				1097	1082	_null_ ));
DATA(insert ( 2123	time_larger		-				1112	1083	_null_ ));
DATA(insert ( 2124	timetz_larger	-				1554	1266	_null_ ));
DATA(insert ( 2125	cashlarger		-				903		790		_null_ ));
DATA(insert ( 2126	timestamp_larger	-			2064	1114	_null_ ));
DATA(insert ( 2127	timestamptz_larger	-			1324	1184	_null_ ));
DATA(insert ( 2128	interval_larger -				1334	1186	_null_ ));
DATA(insert ( 2129	text_larger		-				666		25		_null_ ));
DATA(insert ( 2130	numeric_larger	-				1756	1700	_null_ ));
DATA(insert ( 2050	array_larger	-				1073	2277	_null_ ));
DATA(insert ( 2244	bpchar_larger	-				1060	1042	_null_ ));

/* min */
DATA(insert ( 2131	int8smaller		-				412		20		_null_ ));
DATA(insert ( 2132	int4smaller		-				97		23		_null_ ));
DATA(insert ( 2133	int2smaller		-				95		21		_null_ ));
DATA(insert ( 2134	oidsmaller		-				609		26		_null_ ));
DATA(insert ( 2135	float4smaller	-				622		700		_null_ ));
DATA(insert ( 2136	float8smaller	-				672		701		_null_ ));
DATA(insert ( 2137	int4smaller		-				562		702		_null_ ));
DATA(insert ( 2138	date_smaller	-				1095	1082	_null_ ));
DATA(insert ( 2139	time_smaller	-				1110	1083	_null_ ));
DATA(insert ( 2140	timetz_smaller	-				1552	1266	_null_ ));
DATA(insert ( 2141	cashsmaller		-				902		790		_null_ ));
DATA(insert ( 2142	timestamp_smaller	-			2062	1114	_null_ ));
DATA(insert ( 2143	timestamptz_smaller -			1322	1184	_null_ ));
DATA(insert ( 2144	interval_smaller	-			1332	1186	_null_ ));
DATA(insert ( 2145	text_smaller	-				664		25		_null_ ));
DATA(insert ( 2146	numeric_smaller -				1754	1700	_null_ ));
DATA(insert ( 2051	array_smaller	-				1072	2277	_null_ ));
DATA(insert ( 2245	bpchar_smaller	-				1058	1042	_null_ ));

/*
 * Using int8inc for count() is cheating a little, since it really only
 * takes 1 parameter not 2, but nodeAgg.c won't complain ...
 */
DATA(insert ( 2147	int8inc		-					0	20		0 ));

/* variance */
DATA(insert ( 2148	int8_accum	numeric_variance	0	1231	"{0,0,0}" ));
DATA(insert ( 2149	int4_accum	numeric_variance	0	1231	"{0,0,0}" ));
DATA(insert ( 2150	int2_accum	numeric_variance	0	1231	"{0,0,0}" ));
DATA(insert ( 2151	float4_accum	float8_variance 0	1022	"{0,0,0}" ));
DATA(insert ( 2152	float8_accum	float8_variance 0	1022	"{0,0,0}" ));
DATA(insert ( 2153	numeric_accum  numeric_variance 0	1231	"{0,0,0}" ));

/* stddev */
DATA(insert ( 2154	int8_accum	numeric_stddev		0	1231	"{0,0,0}" ));
DATA(insert ( 2155	int4_accum	numeric_stddev		0	1231	"{0,0,0}" ));
DATA(insert ( 2156	int2_accum	numeric_stddev		0	1231	"{0,0,0}" ));
DATA(insert ( 2157	float4_accum	float8_stddev	0	1022	"{0,0,0}" ));
DATA(insert ( 2158	float8_accum	float8_stddev	0	1022	"{0,0,0}" ));
DATA(insert ( 2159	numeric_accum	numeric_stddev	0	1231	"{0,0,0}" ));

/* boolean-and and boolean-or */
DATA(insert ( 2517	booland_statefunc	-			0	16		_null_ ));
DATA(insert ( 2518	boolor_statefunc	-			0	16		_null_ ));
DATA(insert ( 2519	booland_statefunc	-			0	16		_null_ ));

/* bitwise integer */
DATA(insert ( 2236 int2and		  -					0	21		_null_ ));
DATA(insert ( 2237 int2or		  -					0	21		_null_ ));
DATA(insert ( 2238 int4and		  -					0	23		_null_ ));
DATA(insert ( 2239 int4or		  -					0	23		_null_ ));
DATA(insert ( 2240 int8and		  -					0	20		_null_ ));
DATA(insert ( 2241 int8or		  -					0	20		_null_ ));
DATA(insert ( 2242 bitand		  -					0	1560	_null_ ));
DATA(insert ( 2243 bitor		  -					0	1560	_null_ ));

/*
 * prototypes for functions in pg_aggregate.c
 */
extern void AggregateCreate(const char *aggName,
				Oid aggNamespace,
				Oid aggBaseType,
				List *aggtransfnName,
				List *aggfinalfnName,
				List *aggsortopName,
				Oid aggTransType,
				const char *agginitval);

#endif   /* PG_AGGREGATE_H */
