/*-------------------------------------------------------------------------
 *
 * pg_amproc.h
 *	  definition of the system "amproc" relation (pg_amproc)
 *	  along with the relation's initial contents.
 *
 * The amproc table identifies support procedures associated with index
 * opclasses.  These procedures can't be listed in pg_amop since they are
 * not associated with indexable operators for the opclass.
 *
 * Note: the primary key for this table is <amopclaid, amprocnum>.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_amproc.h,v 1.44 2003/08/17 19:58:06 tgl Exp $
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
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_amproc definition.  cpp turns this into
 *		typedef struct FormData_pg_amproc
 * ----------------
 */
CATALOG(pg_amproc) BKI_WITHOUT_OIDS
{
	Oid			amopclaid;		/* the index opclass this entry is for */
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
#define Natts_pg_amproc					3
#define Anum_pg_amproc_amopclaid		1
#define Anum_pg_amproc_amprocnum		2
#define Anum_pg_amproc_amproc			3

/* ----------------
 *		initial contents of pg_amproc
 * ----------------
 */

/* rtree */
DATA(insert (	 422 1	193 ));
DATA(insert (	 422 2	194 ));
DATA(insert (	 422 3	196 ));
DATA(insert (	 425 1	193 ));
DATA(insert (	 425 2	194 ));
DATA(insert (	 425 3	195 ));
DATA(insert (	1993 1	197 ));
DATA(insert (	1993 2	198 ));
DATA(insert (	1993 3	199 ));


/* btree */
DATA(insert (	 397 1	382 ));
DATA(insert (	 421 1	357 ));
DATA(insert (	 423 1 1596 ));
DATA(insert (	 424 1 1693 ));
DATA(insert (	 426 1 1078 ));
DATA(insert (	 428 1 1954 ));
DATA(insert (	 429 1	358 ));
DATA(insert (	 432 1	926 ));
DATA(insert (	 434 1 1092 ));
DATA(insert (	1970 1	354 ));
DATA(insert (	1972 1	355 ));
DATA(insert (	1974 1	926 ));
DATA(insert (	1976 1	350 ));
DATA(insert (	1978 1	351 ));
DATA(insert (	1980 1	842 ));
DATA(insert (	1982 1 1315 ));
DATA(insert (	1984 1	836 ));
DATA(insert (	1986 1	359 ));
DATA(insert (	1988 1 1769 ));
DATA(insert (	1989 1	356 ));
DATA(insert (	1991 1	404 ));
DATA(insert (	1994 1	360 ));
DATA(insert (	1996 1 1107 ));
DATA(insert (	1998 1 1314 ));
DATA(insert (	2000 1 1358 ));
DATA(insert (	2002 1 1672 ));
DATA(insert (	2003 1	360 ));
DATA(insert (	2039 1 2045 ));
DATA(insert (	2095 1 2166 ));
DATA(insert (	2096 1 2166 ));
DATA(insert (	2097 1 2180 ));
DATA(insert (	2098 1 2187 ));
DATA(insert (	2099 1  377 ));
DATA(insert (	2233 1  380 ));
DATA(insert (	2234 1  381 ));


/* hash */
DATA(insert (	 427 1 1080 ));
DATA(insert (	 431 1	454 ));
DATA(insert (	 433 1	456 ));
DATA(insert (	 435 1	450 ));
DATA(insert (	1971 1	451 ));
DATA(insert (	1973 1	452 ));
DATA(insert (	1975 1	456 ));
DATA(insert (	1977 1	449 ));
DATA(insert (	1979 1	450 ));
DATA(insert (	1981 1	949 ));
DATA(insert (	1983 1 1697 ));
DATA(insert (	1985 1	399 ));
DATA(insert (	1987 1	455 ));
DATA(insert (	1990 1	453 ));
DATA(insert (	1992 1	457 ));
DATA(insert (	1995 1	400 ));
DATA(insert (	1997 1	452 ));
DATA(insert (	1999 1	452 ));
DATA(insert (	2001 1 1696 ));
DATA(insert (	2004 1	400 ));
DATA(insert (	2040 1	452 ));
DATA(insert (	2222 1	454 ));
DATA(insert (	2223 1	456 ));
DATA(insert (	2224 1	398 ));
DATA(insert (	2225 1	450 ));
DATA(insert (	2226 1	450 ));
DATA(insert (	2227 1	450 ));
DATA(insert (	2228 1	450 ));
DATA(insert (	2229 1	456 ));
DATA(insert (	2230 1	456 ));
DATA(insert (	2231 1	456 ));
DATA(insert (	2232 1	455 ));
DATA(insert (	2235 1	329 ));

#endif   /* PG_AMPROC_H */
