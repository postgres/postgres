/*-------------------------------------------------------------------------
 *
 * pg_aggregate.h
 *	  definition of the system "aggregate" relation (pg_aggregate)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_aggregate.h,v 1.41 2003/08/04 02:40:10 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AGGREGATE_H
#define PG_AGGREGATE_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
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
 *	aggfinalfn			final function
 *	aggtranstype		type of aggregate's transition (state) data
 *	agginitval			initial value for transition state
 * ----------------------------------------------------------------
 */
CATALOG(pg_aggregate) BKI_WITHOUT_OIDS
{
	regproc		aggfnoid;
	regproc		aggtransfn;
	regproc		aggfinalfn;
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

#define Natts_pg_aggregate				5
#define Anum_pg_aggregate_aggfnoid		1
#define Anum_pg_aggregate_aggtransfn	2
#define Anum_pg_aggregate_aggfinalfn	3
#define Anum_pg_aggregate_aggtranstype	4
#define Anum_pg_aggregate_agginitval	5


/* ----------------
 * initial contents of pg_aggregate
 * ---------------
 */

/* avg */
DATA(insert ( 2100	int8_accum		numeric_avg		1231	"{0,0,0}" ));
DATA(insert ( 2101	int4_avg_accum	int8_avg		1016	"{0,0}" ));
DATA(insert ( 2102	int2_avg_accum	int8_avg		1016	"{0,0}" ));
DATA(insert ( 2103	numeric_accum	numeric_avg		1231	"{0,0,0}" ));
DATA(insert ( 2104	float4_accum	float8_avg		1022	"{0,0,0}" ));
DATA(insert ( 2105	float8_accum	float8_avg		1022	"{0,0,0}" ));
DATA(insert ( 2106	interval_accum	interval_avg	1187	"{0 second,0 second}" ));

/* sum */
DATA(insert ( 2107	int8_sum		-				1700	_null_ ));
DATA(insert ( 2108	int4_sum		-				20		_null_ ));
DATA(insert ( 2109	int2_sum		-				20		_null_ ));
DATA(insert ( 2110	float4pl		-				700		_null_ ));
DATA(insert ( 2111	float8pl		-				701		_null_ ));
DATA(insert ( 2112	cash_pl			-				790		_null_ ));
DATA(insert ( 2113	interval_pl		-				1186	_null_ ));
DATA(insert ( 2114	numeric_add		-				1700	_null_ ));

/* max */
DATA(insert ( 2115	int8larger		-				20		_null_ ));
DATA(insert ( 2116	int4larger		-				23		_null_ ));
DATA(insert ( 2117	int2larger		-				21		_null_ ));
DATA(insert ( 2118	oidlarger		-				26		_null_ ));
DATA(insert ( 2119	float4larger	-				700		_null_ ));
DATA(insert ( 2120	float8larger	-				701		_null_ ));
DATA(insert ( 2121	int4larger		-				702		_null_ ));
DATA(insert ( 2122	date_larger		-				1082	_null_ ));
DATA(insert ( 2123	time_larger		-				1083	_null_ ));
DATA(insert ( 2124	timetz_larger	-				1266	_null_ ));
DATA(insert ( 2125	cashlarger		-				790		_null_ ));
DATA(insert ( 2126	timestamp_larger	-			1114	_null_ ));
DATA(insert ( 2127	timestamptz_larger	-			1184	_null_ ));
DATA(insert ( 2128	interval_larger -				1186	_null_ ));
DATA(insert ( 2129	text_larger		-				25		_null_ ));
DATA(insert ( 2130	numeric_larger	-				1700	_null_ ));

/* min */
DATA(insert ( 2131	int8smaller		-				20		_null_ ));
DATA(insert ( 2132	int4smaller		-				23		_null_ ));
DATA(insert ( 2133	int2smaller		-				21		_null_ ));
DATA(insert ( 2134	oidsmaller		-				26		_null_ ));
DATA(insert ( 2135	float4smaller	-				700		_null_ ));
DATA(insert ( 2136	float8smaller	-				701		_null_ ));
DATA(insert ( 2137	int4smaller		-				702		_null_ ));
DATA(insert ( 2138	date_smaller	-				1082	_null_ ));
DATA(insert ( 2139	time_smaller	-				1083	_null_ ));
DATA(insert ( 2140	timetz_smaller	-				1266	_null_ ));
DATA(insert ( 2141	cashsmaller		-				790		_null_ ));
DATA(insert ( 2142	timestamp_smaller	-			1114	_null_ ));
DATA(insert ( 2143	timestamptz_smaller -			1184	_null_ ));
DATA(insert ( 2144	interval_smaller	-			1186	_null_ ));
DATA(insert ( 2145	text_smaller	-				25		_null_ ));
DATA(insert ( 2146	numeric_smaller -				1700	_null_ ));

/*
 * Using int8inc for count() is cheating a little, since it really only
 * takes 1 parameter not 2, but nodeAgg.c won't complain ...
 */
DATA(insert ( 2147	int8inc		-					 20		0 ));

/* variance */
DATA(insert ( 2148	int8_accum	numeric_variance	1231	"{0,0,0}" ));
DATA(insert ( 2149	int4_accum	numeric_variance	1231	"{0,0,0}" ));
DATA(insert ( 2150	int2_accum	numeric_variance	1231	"{0,0,0}" ));
DATA(insert ( 2151	float4_accum	float8_variance 1022	"{0,0,0}" ));
DATA(insert ( 2152	float8_accum	float8_variance 1022	"{0,0,0}" ));
DATA(insert ( 2153	numeric_accum	numeric_variance	1231	"{0,0,0}" ));

/* stddev */
DATA(insert ( 2154	int8_accum	numeric_stddev		1231	"{0,0,0}" ));
DATA(insert ( 2155	int4_accum	numeric_stddev		1231	"{0,0,0}" ));
DATA(insert ( 2156	int2_accum	numeric_stddev		1231	"{0,0,0}" ));
DATA(insert ( 2157	float4_accum	float8_stddev	1022	"{0,0,0}" ));
DATA(insert ( 2158	float8_accum	float8_stddev	1022	"{0,0,0}" ));
DATA(insert ( 2159	numeric_accum	numeric_stddev	1231	"{0,0,0}" ));

/*
 * prototypes for functions in pg_aggregate.c
 */
extern void AggregateCreate(const char *aggName,
				Oid aggNamespace,
				List *aggtransfnName,
				List *aggfinalfnName,
				Oid aggBaseType,
				Oid aggTransType,
				const char *agginitval);

#endif   /* PG_AGGREGATE_H */
