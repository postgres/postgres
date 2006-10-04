/*-------------------------------------------------------------------------
 *
 * pg_amproc.h
 *	  definition of the system "amproc" relation (pg_amproc)
 *	  along with the relation's initial contents.
 *
 * The amproc table identifies support procedures associated with index
 * opclasses.  These procedures can't be listed in pg_amop since they are
 * not the implementation of any indexable operator for the opclass.
 *
 * The primary key for this table is <amopclaid, amprocsubtype, amprocnum>.
 * amprocsubtype is equal to zero for an opclass's "default" procedures.
 * Usually a nondefault amprocsubtype indicates a support procedure to be
 * used with operators having the same nondefault amopsubtype.	The exact
 * behavior depends on the index AM, however, and some don't pay attention
 * to subtype at all.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_amproc.h,v 1.60 2006/10/04 00:30:07 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMPROC_H
#define PG_AMPROC_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_amproc definition.  cpp turns this into
 *		typedef struct FormData_pg_amproc
 * ----------------
 */
#define AccessMethodProcedureRelationId  2603

CATALOG(pg_amproc,2603) BKI_WITHOUT_OIDS
{
	Oid			amopclaid;		/* the index opclass this entry is for */
	Oid			amprocsubtype;	/* procedure subtype, or zero if default */
	int2		amprocnum;		/* support procedure index */
	regproc		amproc;			/* OID of the proc */
} FormData_pg_amproc;

/* ----------------
 *		Form_pg_amproc corresponds to a pointer to a tuple with
 *		the format of pg_amproc relation.
 * ----------------
 */
typedef FormData_pg_amproc *Form_pg_amproc;

/* ----------------
 *		compiler constants for pg_amproc
 * ----------------
 */
#define Natts_pg_amproc					4
#define Anum_pg_amproc_amopclaid		1
#define Anum_pg_amproc_amprocsubtype	2
#define Anum_pg_amproc_amprocnum		3
#define Anum_pg_amproc_amproc			4

/* ----------------
 *		initial contents of pg_amproc
 * ----------------
 */

/* btree */
DATA(insert (	 397	0 1 382 ));
DATA(insert (	 421	0 1 357 ));
DATA(insert (	 423	0 1 1596 ));
DATA(insert (	 424	0 1 1693 ));
DATA(insert (	 426	0 1 1078 ));
DATA(insert (	 428	0 1 1954 ));
DATA(insert (	 429	0 1 358 ));
DATA(insert (	 432	0 1 926 ));
DATA(insert (	 434	0 1 1092 ));
DATA(insert (	 434 1114 1 2344 ));
DATA(insert (	 434 1184 1 2357 ));
DATA(insert (	1970	0 1 354 ));
DATA(insert (	1970  701 1 2194 ));
DATA(insert (	1972	0 1 355 ));
DATA(insert (	1972  700 1 2195 ));
DATA(insert (	1974	0 1 926 ));
DATA(insert (	1976	0 1 350 ));
DATA(insert (	1976   23 1 2190 ));
DATA(insert (	1976   20 1 2192 ));
DATA(insert (	1978	0 1 351 ));
DATA(insert (	1978   20 1 2188 ));
DATA(insert (	1978   21 1 2191 ));
DATA(insert (	1980	0 1 842 ));
DATA(insert (	1980   23 1 2189 ));
DATA(insert (	1980   21 1 2193 ));
DATA(insert (	1982	0 1 1315 ));
DATA(insert (	1984	0 1 836 ));
DATA(insert (	1986	0 1 359 ));
DATA(insert (	1988	0 1 1769 ));
DATA(insert (	1989	0 1 356 ));
DATA(insert (	1991	0 1 404 ));
DATA(insert (	1994	0 1 360 ));
DATA(insert (	1996	0 1 1107 ));
DATA(insert (	1998	0 1 1314 ));
DATA(insert (	1998 1082 1 2383 ));
DATA(insert (	1998 1114 1 2533 ));
DATA(insert (	2000	0 1 1358 ));
DATA(insert (	2002	0 1 1672 ));
DATA(insert (	2003	0 1 360 ));
DATA(insert (	2039	0 1 2045 ));
DATA(insert (	2039 1082 1 2370 ));
DATA(insert (	2039 1184 1 2526 ));
DATA(insert (	2095	0 1 2166 ));
DATA(insert (	2096	0 1 2166 ));
DATA(insert (	2097	0 1 2180 ));
DATA(insert (	2098	0 1 2187 ));
DATA(insert (	2099	0 1  377 ));
DATA(insert (	2233	0 1  380 ));
DATA(insert (	2234	0 1  381 ));
DATA(insert (	2789	0 1 2794 ));


/* hash */
DATA(insert (	 427	0 1 1080 ));
DATA(insert (	 431	0 1 454 ));
DATA(insert (	 433	0 1 422 ));
DATA(insert (	 435	0 1 450 ));
DATA(insert (	1971	0 1 451 ));
DATA(insert (	1973	0 1 452 ));
DATA(insert (	1975	0 1 422 ));
DATA(insert (	1977	0 1 449 ));
DATA(insert (	1979	0 1 450 ));
DATA(insert (	1981	0 1 949 ));
DATA(insert (	1983	0 1 1697 ));
DATA(insert (	1985	0 1 399 ));
DATA(insert (	1987	0 1 455 ));
DATA(insert (	1990	0 1 453 ));
DATA(insert (	1992	0 1 457 ));
DATA(insert (	1995	0 1 400 ));
DATA(insert (	1997	0 1 452 ));
DATA(insert (	1999	0 1 452 ));
DATA(insert (	2001	0 1 1696 ));
DATA(insert (	2004	0 1 400 ));
DATA(insert (	2040	0 1 452 ));
DATA(insert (	2222	0 1 454 ));
DATA(insert (	2223	0 1 456 ));
DATA(insert (	2224	0 1 398 ));
DATA(insert (	2225	0 1 450 ));
DATA(insert (	2226	0 1 450 ));
DATA(insert (	2227	0 1 450 ));
DATA(insert (	2228	0 1 450 ));
DATA(insert (	2229	0 1 456 ));
DATA(insert (	2230	0 1 456 ));
DATA(insert (	2231	0 1 456 ));
DATA(insert (	2232	0 1 455 ));
DATA(insert (	2235	0 1 329 ));


/* gist */
DATA(insert (	2593	0 1 2578 ));
DATA(insert (	2593	0 2 2583 ));
DATA(insert (	2593	0 3 2579 ));
DATA(insert (	2593	0 4 2580 ));
DATA(insert (	2593	0 5 2581 ));
DATA(insert (	2593	0 6 2582 ));
DATA(insert (	2593	0 7 2584 ));
DATA(insert (	2594	0 1 2585 ));
DATA(insert (	2594	0 2 2583 ));
DATA(insert (	2594	0 3 2586 ));
DATA(insert (	2594	0 4 2580 ));
DATA(insert (	2594	0 5 2581 ));
DATA(insert (	2594	0 6 2582 ));
DATA(insert (	2594	0 7 2584 ));
DATA(insert (	2595	0 1 2591 ));
DATA(insert (	2595	0 2 2583 ));
DATA(insert (	2595	0 3 2592 ));
DATA(insert (	2595	0 4 2580 ));
DATA(insert (	2595	0 5 2581 ));
DATA(insert (	2595	0 6 2582 ));
DATA(insert (	2595	0 7 2584 ));

/* gin */
DATA(insert (	2745	0 1  351 ));
DATA(insert (	2745	0 2 2743 ));
DATA(insert (	2745	0 3 2743 ));
DATA(insert (	2745	0 4 2744 ));
DATA(insert (	2746	0 1  360 ));
DATA(insert (	2746	0 2 2743 ));
DATA(insert (	2746	0 3 2743 ));
DATA(insert (	2746	0 4 2744 ));
DATA(insert (	2753	0 1 357 ));
DATA(insert (	2753	0 2 2743 ));
DATA(insert (	2753	0 3 2743 ));
DATA(insert (	2753	0 4 2744 ));
DATA(insert (	2754	0 1 1596 ));
DATA(insert (	2754	0 2 2743 ));
DATA(insert (	2754	0 3 2743 ));
DATA(insert (	2754	0 4 2744 ));
DATA(insert (	2755	0 1 1693 ));
DATA(insert (	2755	0 2 2743 ));
DATA(insert (	2755	0 3 2743 ));
DATA(insert (	2755	0 4 2744 ));
DATA(insert (	2756	0 1 1078 ));
DATA(insert (	2756	0 2 2743 ));
DATA(insert (	2756	0 3 2743 ));
DATA(insert (	2756	0 4 2744 ));
DATA(insert (	2757	0 1 1954 ));
DATA(insert (	2757	0 2 2743 ));
DATA(insert (	2757	0 3 2743 ));
DATA(insert (	2757	0 4 2744 ));
DATA(insert (	2758	0 1 358 ));
DATA(insert (	2758	0 2 2743 ));
DATA(insert (	2758	0 3 2743 ));
DATA(insert (	2758	0 4 2744 ));
DATA(insert (	2759	0 1 926 ));
DATA(insert (	2759	0 2 2743 ));
DATA(insert (	2759	0 3 2743 ));
DATA(insert (	2759	0 4 2744 ));
DATA(insert (	2760	0 1 1092 ));
DATA(insert (	2760	0 2 2743 ));
DATA(insert (	2760	0 3 2743 ));
DATA(insert (	2760	0 4 2744 ));
DATA(insert (	2761	0 1 354 ));
DATA(insert (	2761	0 2 2743 ));
DATA(insert (	2761	0 3 2743 ));
DATA(insert (	2761	0 4 2744 ));
DATA(insert (	2762	0 1 355 ));
DATA(insert (	2762	0 2 2743 ));
DATA(insert (	2762	0 3 2743 ));
DATA(insert (	2762	0 4 2744 ));
DATA(insert (	2763	0 1 926 ));
DATA(insert (	2763	0 2 2743 ));
DATA(insert (	2763	0 3 2743 ));
DATA(insert (	2763	0 4 2744 ));
DATA(insert (	2764	0 1 350 ));
DATA(insert (	2764	0 2 2743 ));
DATA(insert (	2764	0 3 2743 ));
DATA(insert (	2764	0 4 2744 ));
DATA(insert (	2765	0 1 842 ));
DATA(insert (	2765	0 2 2743 ));
DATA(insert (	2765	0 3 2743 ));
DATA(insert (	2765	0 4 2744 ));
DATA(insert (	2766	0 1 1315 ));
DATA(insert (	2766	0 2 2743 ));
DATA(insert (	2766	0 3 2743 ));
DATA(insert (	2766	0 4 2744 ));
DATA(insert (	2767	0 1 836 ));
DATA(insert (	2767	0 2 2743 ));
DATA(insert (	2767	0 3 2743 ));
DATA(insert (	2767	0 4 2744 ));
DATA(insert (	2768	0 1 359 ));
DATA(insert (	2768	0 2 2743 ));
DATA(insert (	2768	0 3 2743 ));
DATA(insert (	2768	0 4 2744 ));
DATA(insert (	2769	0 1 1769 ));
DATA(insert (	2769	0 2 2743 ));
DATA(insert (	2769	0 3 2743 ));
DATA(insert (	2769	0 4 2744 ));
DATA(insert (	2770	0 1 356 ));
DATA(insert (	2770	0 2 2743 ));
DATA(insert (	2770	0 3 2743 ));
DATA(insert (	2770	0 4 2744 ));
DATA(insert (	2771	0 1 404 ));
DATA(insert (	2771	0 2 2743 ));
DATA(insert (	2771	0 3 2743 ));
DATA(insert (	2771	0 4 2744 ));
DATA(insert (	2772	0 1 1107 ));
DATA(insert (	2772	0 2 2743 ));
DATA(insert (	2772	0 3 2743 ));
DATA(insert (	2772	0 4 2744 ));
DATA(insert (	2773	0 1 1314 ));
DATA(insert (	2773	0 2 2743 ));
DATA(insert (	2773	0 3 2743 ));
DATA(insert (	2773	0 4 2744 ));
DATA(insert (	2774	0 1 1358 ));
DATA(insert (	2774	0 2 2743 ));
DATA(insert (	2774	0 3 2743 ));
DATA(insert (	2774	0 4 2744 ));
DATA(insert (	2775	0 1 1672 ));
DATA(insert (	2775	0 2 2743 ));
DATA(insert (	2775	0 3 2743 ));
DATA(insert (	2775	0 4 2744 ));
DATA(insert (	2776	0 1 360 ));
DATA(insert (	2776	0 2 2743 ));
DATA(insert (	2776	0 3 2743 ));
DATA(insert (	2776	0 4 2744 ));
DATA(insert (	2777	0 1 2045 ));
DATA(insert (	2777	0 2 2743 ));
DATA(insert (	2777	0 3 2743 ));
DATA(insert (	2777	0 4 2744 ));
DATA(insert (	2778	0 1 377 ));
DATA(insert (	2778	0 2 2743 ));
DATA(insert (	2778	0 3 2743 ));
DATA(insert (	2778	0 4 2744 ));
DATA(insert (	2779	0 1 380 ));
DATA(insert (	2779	0 2 2743 ));
DATA(insert (	2779	0 3 2743 ));
DATA(insert (	2779	0 4 2744 ));
DATA(insert (	2780	0 1 381 ));
DATA(insert (	2780	0 2 2743 ));
DATA(insert (	2780	0 3 2743 ));
DATA(insert (	2780	0 4 2744 ));

#endif   /* PG_AMPROC_H */
