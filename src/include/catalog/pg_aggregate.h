/*-------------------------------------------------------------------------
 *
 * pg_aggregate.h--
 *	  definition of the system "aggregate" relation (pg_aggregate)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_aggregate.h,v 1.18 1998/12/08 06:18:11 thomas Exp $
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
 *	aggtransfn1			transition function 1
 *	aggtransfn2			transition function 2
 *	aggfinalfn			final function
 *	aggbasetype			type of data on which aggregate operates
 *	aggtranstype1		output types for transition func 1
 *	aggtranstype2		output types for transition func 2
 *	aggfinaltype		output type for final function
 *	agginitval1			initial aggregate value
 *	agginitval2			initial value for transition state 2
 * ----------------------------------------------------------------
 */
CATALOG(pg_aggregate)
{
	NameData	aggname;
	int4		aggowner;
	regproc		aggtransfn1;
	regproc		aggtransfn2;
	regproc		aggfinalfn;
	Oid			aggbasetype;
	Oid			aggtranstype1;
	Oid			aggtranstype2;
	Oid			aggfinaltype;
	text		agginitval1;	/* VARIABLE LENGTH FIELD */
	text		agginitval2;	/* VARIABLE LENGTH FIELD */
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

#define Natts_pg_aggregate				11
#define Anum_pg_aggregate_aggname		1
#define Anum_pg_aggregate_aggowner		2
#define Anum_pg_aggregate_aggtransfn1	3
#define Anum_pg_aggregate_aggtransfn2	4
#define Anum_pg_aggregate_aggfinalfn	5
#define Anum_pg_aggregate_aggbasetype	6
#define Anum_pg_aggregate_aggtranstype1 7
#define Anum_pg_aggregate_aggtranstype2 8
#define Anum_pg_aggregate_aggfinaltype	9
#define Anum_pg_aggregate_agginitval1	10
#define Anum_pg_aggregate_agginitval2	11


/* ----------------
 * initial contents of pg_aggregate
 * ---------------
 */

DATA(insert OID = 0 ( avg	PGUID int8pl	  int4inc	int84div		20	 20   23   20 _null_ 0 ));
DATA(insert OID = 0 ( avg	PGUID int4pl	  int4inc	int4div			23	 23   23   23 _null_ 0 ));
DATA(insert OID = 0 ( avg	PGUID int2pl	  int2inc	int2div			21	 21   21   21 _null_ 0 ));
DATA(insert OID = 0 ( avg	PGUID float4pl	  float4inc float4div	   700	700  700  700 _null_ 0.0 ));
DATA(insert OID = 0 ( avg	PGUID float8pl	  float8inc float8div	   701	701  701  701 _null_ 0.0 ));
DATA(insert OID = 0 ( avg	PGUID cash_pl	  float8inc cash_div_flt8  790	790  701  790 _null_ 0.0 ));
DATA(insert OID = 0 ( avg	PGUID timespan_pl float8inc timespan_div  1186 1186  701 1186 _null_ 0.0 ));

DATA(insert OID = 0 ( sum	PGUID int8pl			- -   20   20 0   20 _null_ _null_ ));
DATA(insert OID = 0 ( sum	PGUID int4pl			- -   23   23 0   23 _null_ _null_ ));
DATA(insert OID = 0 ( sum	PGUID int2pl			- -   21   21 0   21 _null_ _null_ ));
DATA(insert OID = 0 ( sum	PGUID float4pl			- -  700  700 0  700 _null_ _null_ ));
DATA(insert OID = 0 ( sum	PGUID float8pl			- -  701  701 0  701 _null_ _null_ ));
DATA(insert OID = 0 ( sum	PGUID cash_pl			- -  790  790 0  790 _null_ _null_ ));
DATA(insert OID = 0 ( sum	PGUID timespan_pl		- - 1186 1186 0 1186 _null_ _null_ ));

DATA(insert OID = 0 ( max	PGUID int8larger		- -   20   20 0   20 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID int4larger		- -   23   23 0   23 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID int2larger		- -   21   21 0   21 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID float4larger		- -  700  700 0  700 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID float8larger		- -  701  701 0  701 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID int4larger		- -  702  702 0  702 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID date_larger		- - 1082 1082 0 1082 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID float8larger		- - 1084 1084 0 1084 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID cashlarger		- -  790  790 0  790 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID datetime_larger	- - 1184 1184 0 1184 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID timespan_larger	- - 1186 1186 0 1186 _null_ _null_ ));
DATA(insert OID = 0 ( max	PGUID text_larger		- -   25   25 0   25 _null_ _null_ ));

DATA(insert OID = 0 ( min	PGUID int8smaller		- -   20   20 0   20 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID int4smaller		- -   23   23 0   23 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID int2smaller		- -   21   21 0   21 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID float4smaller		- -  700  700 0  700 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID float8smaller		- -  701  701 0  701 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID int4smaller		- -  702  702 0  702 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID date_smaller		- - 1082 1082 0 1082 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID float8smaller		- - 1084 1084 0 1084 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID cashsmaller		- -  790  790 0  790 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID datetime_smaller	- - 1184 1184 0 1184 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID timespan_smaller	- - 1186 1186 0 1186 _null_ _null_ ));
DATA(insert OID = 0 ( min	PGUID text_smaller		- -   25   25 0   25 _null_ _null_ ));

DATA(insert OID = 0 ( count PGUID - int4inc - 0 0 23 23 _null_ 0 ));

/*
 * prototypes for functions in pg_aggregate.c
 */
extern void AggregateCreate(char *aggName,
				char *aggtransfn1Name,
				char *aggtransfn2Name,
				char *aggfinalfnName,
				char *aggbasetypeName,
				char *aggtransfn1typeName,
				char *aggtransfn2typeName,
				char *agginitval1,
				char *agginitval2);
extern char *AggNameGetInitVal(char *aggName, Oid basetype,
				  int xfuncno, bool *isNull);

#endif	 /* PG_AGGREGATE_H */
