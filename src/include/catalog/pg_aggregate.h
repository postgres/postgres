/*-------------------------------------------------------------------------
 *
 * pg_aggregate.h
 *	  definition of the system "aggregate" relation (pg_aggregate)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_aggregate.h,v 1.27 2000/07/17 03:05:23 tgl Exp $
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
 *	aggname				name of the aggregate
 *	aggowner			owner (creator) of the aggregate
 *	aggtransfn			transition function
 *	aggfinalfn			final function
 *	aggbasetype			type of data on which aggregate operates
 *	aggtranstype		type of aggregate's transition (state) data
 *	aggfinaltype		type of aggregate's final result
 *	agginitval			initial value for transition state
 * ----------------------------------------------------------------
 */
CATALOG(pg_aggregate)
{
	NameData	aggname;
	int4		aggowner;
	regproc		aggtransfn;
	regproc		aggfinalfn;
	Oid			aggbasetype;
	Oid			aggtranstype;
	Oid			aggfinaltype;
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

#define Natts_pg_aggregate				8
#define Anum_pg_aggregate_aggname		1
#define Anum_pg_aggregate_aggowner		2
#define Anum_pg_aggregate_aggtransfn	3
#define Anum_pg_aggregate_aggfinalfn	4
#define Anum_pg_aggregate_aggbasetype	5
#define Anum_pg_aggregate_aggtranstype	6
#define Anum_pg_aggregate_aggfinaltype	7
#define Anum_pg_aggregate_agginitval	8


/* ----------------
 * initial contents of pg_aggregate
 * ---------------
 */

DATA(insert OID = 0 ( avg	PGUID int8_accum	numeric_avg		20	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( avg	PGUID int4_accum	numeric_avg		23	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( avg	PGUID int2_accum	numeric_avg		21	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( avg	PGUID numeric_accum  numeric_avg	1700 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( avg	PGUID float4_accum	float8_avg		700	 1022 701 "{0,0,0}" ));
DATA(insert OID = 0 ( avg	PGUID float8_accum	float8_avg		701	 1022 701 "{0,0,0}" ));
DATA(insert OID = 0 ( avg	PGUID interval_accum interval_avg	1186 1187 1186 "{0,0}" ));

DATA(insert OID = 0 ( sum	PGUID int8_sum			-   20 1700 1700 _null_ ));
DATA(insert OID = 0 ( sum	PGUID int4_sum			-   23 1700 1700 _null_ ));
DATA(insert OID = 0 ( sum	PGUID int2_sum			-   21 1700 1700 _null_ ));
DATA(insert OID = 0 ( sum	PGUID float4pl			-  700  700  700 _null_ ));
DATA(insert OID = 0 ( sum	PGUID float8pl			-  701  701  701 _null_ ));
DATA(insert OID = 0 ( sum	PGUID cash_pl			-  790  790  790 _null_ ));
DATA(insert OID = 0 ( sum	PGUID interval_pl		- 1186 1186 1186 _null_ ));
DATA(insert OID = 0 ( sum	PGUID numeric_add		- 1700 1700 1700 _null_ ));

DATA(insert OID = 0 ( max	PGUID int8larger		-   20   20   20 _null_ ));
DATA(insert OID = 0 ( max	PGUID int4larger		-   23   23   23 _null_ ));
DATA(insert OID = 0 ( max	PGUID int2larger		-   21   21   21 _null_ ));
DATA(insert OID = 0 ( max	PGUID float4larger		-  700  700  700 _null_ ));
DATA(insert OID = 0 ( max	PGUID float8larger		-  701  701  701 _null_ ));
DATA(insert OID = 0 ( max	PGUID int4larger		-  702  702  702 _null_ ));
DATA(insert OID = 0 ( max	PGUID date_larger		- 1082 1082 1082 _null_ ));
DATA(insert OID = 0 ( max	PGUID time_larger		- 1083 1083 1083 _null_ ));
DATA(insert OID = 0 ( max	PGUID timetz_larger		- 1266 1266 1266 _null_ ));
DATA(insert OID = 0 ( max	PGUID cashlarger		-  790  790  790 _null_ ));
DATA(insert OID = 0 ( max	PGUID timestamp_larger	- 1184 1184 1184 _null_ ));
DATA(insert OID = 0 ( max	PGUID interval_larger	- 1186 1186 1186 _null_ ));
DATA(insert OID = 0 ( max	PGUID text_larger		-   25   25   25 _null_ ));
DATA(insert OID = 0 ( max	PGUID numeric_larger	- 1700 1700 1700 _null_ ));

DATA(insert OID = 0 ( min	PGUID int8smaller		-   20   20   20 _null_ ));
DATA(insert OID = 0 ( min	PGUID int4smaller		-   23   23   23 _null_ ));
DATA(insert OID = 0 ( min	PGUID int2smaller		-   21   21   21 _null_ ));
DATA(insert OID = 0 ( min	PGUID float4smaller		-  700  700  700 _null_ ));
DATA(insert OID = 0 ( min	PGUID float8smaller		-  701  701  701 _null_ ));
DATA(insert OID = 0 ( min	PGUID int4smaller		-  702  702  702 _null_ ));
DATA(insert OID = 0 ( min	PGUID date_smaller		- 1082 1082 1082 _null_ ));
DATA(insert OID = 0 ( min	PGUID time_smaller		- 1083 1083 1083 _null_ ));
DATA(insert OID = 0 ( min	PGUID timetz_smaller	- 1266 1266 1266 _null_ ));
DATA(insert OID = 0 ( min	PGUID cashsmaller		-  790  790  790 _null_ ));
DATA(insert OID = 0 ( min	PGUID timestamp_smaller - 1184 1184 1184 _null_ ));
DATA(insert OID = 0 ( min	PGUID interval_smaller	- 1186 1186 1186 _null_ ));
DATA(insert OID = 0 ( min	PGUID text_smaller		-   25   25   25 _null_ ));
DATA(insert OID = 0 ( min	PGUID numeric_smaller	- 1700 1700 1700 _null_ ));

/*
 * Using int4inc for count() is cheating a little, since it really only
 * takes 1 parameter not 2, but nodeAgg.c won't complain ...
 */
DATA(insert OID = 0 ( count PGUID int4inc           - 0 23 23 0 ));

DATA(insert OID = 0 ( variance	PGUID int8_accum	numeric_variance	20	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( variance	PGUID int4_accum	numeric_variance	23	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( variance	PGUID int2_accum	numeric_variance	21	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( variance	PGUID float4_accum	float8_variance		700	 1022 701 "{0,0,0}" ));
DATA(insert OID = 0 ( variance	PGUID float8_accum	float8_variance		701	 1022 701 "{0,0,0}" ));
DATA(insert OID = 0 ( variance	PGUID numeric_accum  numeric_variance	1700 1231 1700 "{0,0,0}" ));

DATA(insert OID = 0 ( stddev	PGUID int8_accum	numeric_stddev		20	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( stddev	PGUID int4_accum	numeric_stddev		23	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( stddev	PGUID int2_accum	numeric_stddev		21	 1231 1700 "{0,0,0}" ));
DATA(insert OID = 0 ( stddev	PGUID float4_accum	float8_stddev		700	 1022 701 "{0,0,0}" ));
DATA(insert OID = 0 ( stddev	PGUID float8_accum	float8_stddev		701	 1022 701 "{0,0,0}" ));
DATA(insert OID = 0 ( stddev	PGUID numeric_accum  numeric_stddev		1700 1231 1700 "{0,0,0}" ));

/*
 * prototypes for functions in pg_aggregate.c
 */
extern void AggregateCreate(char *aggName,
				char *aggtransfnName,
				char *aggfinalfnName,
				char *aggbasetypeName,
				char *aggtranstypeName,
				char *agginitval);

extern Datum AggNameGetInitVal(char *aggName, Oid basetype,
							   bool *isNull);

#endif	 /* PG_AGGREGATE_H */
