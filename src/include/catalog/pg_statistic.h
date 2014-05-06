/*-------------------------------------------------------------------------
 *
 * pg_statistic.h
 *	  definition of the system "statistic" relation (pg_statistic)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_statistic.h,v 1.42 2010/01/05 01:06:57 tgl Exp $
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_STATISTIC_H
#define PG_STATISTIC_H

#include "catalog/genbki.h"

/*
 * The CATALOG definition has to refer to the type of stavaluesN as
 * "anyarray" so that bootstrap mode recognizes it.  There is no real
 * typedef for that, however.  Since the fields are potentially-null and
 * therefore can't be accessed directly from C code, there is no particular
 * need for the C struct definition to show a valid field type --- instead
 * we just make it int.
 */
#define anyarray int

/* ----------------
 *		pg_statistic definition.  cpp turns this into
 *		typedef struct FormData_pg_statistic
 * ----------------
 */
#define StatisticRelationId  2619

CATALOG(pg_statistic,2619) BKI_WITHOUT_OIDS
{
	/* These fields form the unique key for the entry: */
	Oid			starelid;		/* relation containing attribute */
	int2		staattnum;		/* attribute (column) stats are for */
	bool		stainherit;		/* true if inheritance children are included */

	/* the fraction of the column's entries that are NULL: */
	float4		stanullfrac;

	/*
	 * stawidth is the average width in bytes of non-null entries.  For
	 * fixed-width datatypes this is of course the same as the typlen, but for
	 * var-width types it is more useful.  Note that this is the average width
	 * of the data as actually stored, post-TOASTing (eg, for a
	 * moved-out-of-line value, only the size of the pointer object is
	 * counted).  This is the appropriate definition for the primary use of
	 * the statistic, which is to estimate sizes of in-memory hash tables of
	 * tuples.
	 */
	int4		stawidth;

	/* ----------------
	 * stadistinct indicates the (approximate) number of distinct non-null
	 * data values in the column.  The interpretation is:
	 *		0		unknown or not computed
	 *		> 0		actual number of distinct values
	 *		< 0		negative of multiplier for number of rows
	 * The special negative case allows us to cope with columns that are
	 * unique (stadistinct = -1) or nearly so (for example, a column in
	 * which values appear about twice on the average could be represented
	 * by stadistinct = -0.5).  Because the number-of-rows statistic in
	 * pg_class may be updated more frequently than pg_statistic is, it's
	 * important to be able to describe such situations as a multiple of
	 * the number of rows, rather than a fixed number of distinct values.
	 * But in other cases a fixed number is correct (eg, a boolean column).
	 * ----------------
	 */
	float4		stadistinct;

	/* ----------------
	 * To allow keeping statistics on different kinds of datatypes,
	 * we do not hard-wire any particular meaning for the remaining
	 * statistical fields.  Instead, we provide several "slots" in which
	 * statistical data can be placed.  Each slot includes:
	 *		kind			integer code identifying kind of data
	 *		op				OID of associated operator, if needed
	 *		numbers			float4 array (for statistical values)
	 *		values			anyarray (for representations of data values)
	 * The ID and operator fields are never NULL; they are zeroes in an
	 * unused slot.  The numbers and values fields are NULL in an unused
	 * slot, and might also be NULL in a used slot if the slot kind has
	 * no need for one or the other.
	 * ----------------
	 */

	int2		stakind1;
	int2		stakind2;
	int2		stakind3;
	int2		stakind4;

	Oid			staop1;
	Oid			staop2;
	Oid			staop3;
	Oid			staop4;

	/*
	 * THE REST OF THESE ARE VARIABLE LENGTH FIELDS, and may even be absent
	 * (NULL). They cannot be accessed as C struct entries; you have to use
	 * the full field access machinery (heap_getattr) for them.  We declare
	 * them here for the catalog machinery.
	 */

	float4		stanumbers1[1];
	float4		stanumbers2[1];
	float4		stanumbers3[1];
	float4		stanumbers4[1];

	/*
	 * Values in these arrays are values of the column's data type.  We
	 * presently have to cheat quite a bit to allow polymorphic arrays of this
	 * kind, but perhaps someday it'll be a less bogus facility.
	 */
	anyarray	stavalues1;
	anyarray	stavalues2;
	anyarray	stavalues3;
	anyarray	stavalues4;
} FormData_pg_statistic;

#define STATISTIC_NUM_SLOTS  4

#undef anyarray


/* ----------------
 *		Form_pg_statistic corresponds to a pointer to a tuple with
 *		the format of pg_statistic relation.
 * ----------------
 */
typedef FormData_pg_statistic *Form_pg_statistic;

/* ----------------
 *		compiler constants for pg_statistic
 * ----------------
 */
#define Natts_pg_statistic				22
#define Anum_pg_statistic_starelid		1
#define Anum_pg_statistic_staattnum		2
#define Anum_pg_statistic_stainherit	3
#define Anum_pg_statistic_stanullfrac	4
#define Anum_pg_statistic_stawidth		5
#define Anum_pg_statistic_stadistinct	6
#define Anum_pg_statistic_stakind1		7
#define Anum_pg_statistic_stakind2		8
#define Anum_pg_statistic_stakind3		9
#define Anum_pg_statistic_stakind4		10
#define Anum_pg_statistic_staop1		11
#define Anum_pg_statistic_staop2		12
#define Anum_pg_statistic_staop3		13
#define Anum_pg_statistic_staop4		14
#define Anum_pg_statistic_stanumbers1	15
#define Anum_pg_statistic_stanumbers2	16
#define Anum_pg_statistic_stanumbers3	17
#define Anum_pg_statistic_stanumbers4	18
#define Anum_pg_statistic_stavalues1	19
#define Anum_pg_statistic_stavalues2	20
#define Anum_pg_statistic_stavalues3	21
#define Anum_pg_statistic_stavalues4	22

/*
 * Currently, three statistical slot "kinds" are defined: most common values,
 * histogram, and correlation.  Additional "kinds" will probably appear in
 * future to help cope with non-scalar datatypes.  Also, custom data types
 * can define their own "kind" codes by mutual agreement between a custom
 * typanalyze routine and the selectivity estimation functions of the type's
 * operators.
 *
 * Code reading the pg_statistic relation should not assume that a particular
 * data "kind" will appear in any particular slot.  Instead, search the
 * stakind fields to see if the desired data is available.  (The standard
 * function get_attstatsslot() may be used for this.)
 */

/*
 * The present allocation of "kind" codes is:
 *
 *	1-99:		reserved for assignment by the core PostgreSQL project
 *				(values in this range will be documented in this file)
 *	100-199:	reserved for assignment by the PostGIS project
 *				(values to be documented in PostGIS documentation)
 *	200-299:	reserved for assignment by the ESRI ST_Geometry project
 *				(values to be documented in ESRI ST_Geometry documentation)
 *	300-9999:	reserved for future public assignments
 *
 * For private use you may choose a "kind" code at random in the range
 * 10000-30000.  However, for code that is to be widely disseminated it is
 * better to obtain a publicly defined "kind" code by request from the
 * PostgreSQL Global Development Group.
 */

/*
 * In a "most common values" slot, staop is the OID of the "=" operator
 * used to decide whether values are the same or not.  stavalues contains
 * the K most common non-null values appearing in the column, and stanumbers
 * contains their frequencies (fractions of total row count).  The values
 * shall be ordered in decreasing frequency.  Note that since the arrays are
 * variable-size, K may be chosen by the statistics collector.  Values should
 * not appear in MCV unless they have been observed to occur more than once;
 * a unique column will have no MCV slot.
 */
#define STATISTIC_KIND_MCV	1

/*
 * A "histogram" slot describes the distribution of scalar data.  staop is
 * the OID of the "<" operator that describes the sort ordering.  (In theory,
 * more than one histogram could appear, if a datatype has more than one
 * useful sort operator.)  stavalues contains M (>=2) non-null values that
 * divide the non-null column data values into M-1 bins of approximately equal
 * population.  The first stavalues item is the MIN and the last is the MAX.
 * stanumbers is not used and should be NULL.  IMPORTANT POINT: if an MCV
 * slot is also provided, then the histogram describes the data distribution
 * *after removing the values listed in MCV* (thus, it's a "compressed
 * histogram" in the technical parlance).  This allows a more accurate
 * representation of the distribution of a column with some very-common
 * values.  In a column with only a few distinct values, it's possible that
 * the MCV list describes the entire data population; in this case the
 * histogram reduces to empty and should be omitted.
 */
#define STATISTIC_KIND_HISTOGRAM  2

/*
 * A "correlation" slot describes the correlation between the physical order
 * of table tuples and the ordering of data values of this column, as seen
 * by the "<" operator identified by staop.  (As with the histogram, more
 * than one entry could theoretically appear.)	stavalues is not used and
 * should be NULL.  stanumbers contains a single entry, the correlation
 * coefficient between the sequence of data values and the sequence of
 * their actual tuple positions.  The coefficient ranges from +1 to -1.
 */
#define STATISTIC_KIND_CORRELATION	3

/*
 * A "most common elements" slot is similar to a "most common values" slot,
 * except that it stores the most common non-null *elements* of the column
 * values.  This is useful when the column datatype is an array or some other
 * type with identifiable elements (for instance, tsvector).  staop contains
 * the equality operator appropriate to the element type.  stavalues contains
 * the most common element values, and stanumbers their frequencies.  Unlike
 * MCV slots, frequencies are measured as the fraction of non-null rows the
 * element value appears in, not the frequency of all rows.  Also unlike
 * MCV slots, the values are sorted into order (to support binary search
 * for a particular value).  Since this puts the minimum and maximum
 * frequencies at unpredictable spots in stanumbers, there are two extra
 * members of stanumbers, holding copies of the minimum and maximum
 * frequencies.
 *
 * Note: in current usage for tsvector columns, the stavalues elements are of
 * type text, even though their representation within tsvector is not
 * exactly text.
 */
#define STATISTIC_KIND_MCELEM  4

#endif   /* PG_STATISTIC_H */
