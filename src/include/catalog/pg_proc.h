/*-------------------------------------------------------------------------
 *
 * pg_proc.h
 *	  definition of the system "procedure" relation (pg_proc)
 *	  along with the relation's initial contents.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_proc.h,v 1.237 2002/05/18 13:48:00 petere Exp $
 *
 * NOTES
 *	  The script catalog/genbki.sh reads this file and generates .bki
 *	  information from the DATA() statements.  utils/Gen_fmgrtab.sh
 *	  generates fmgroids.h and fmgrtab.c the same way.
 *
 *	  XXX do NOT break up DATA() statements into multiple lines!
 *		  the scripts are not as smart as you might think...
 *	  XXX (eg. #if 0 #endif won't do what you think)
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROC_H
#define PG_PROC_H

#include "nodes/pg_list.h"

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_proc definition.  cpp turns this into
 *		typedef struct FormData_pg_proc
 * ----------------
 */
CATALOG(pg_proc) BOOTSTRAP
{
	NameData	proname;		/* procedure name */
	Oid			pronamespace;	/* OID of namespace containing this proc */
	int4		proowner;		/* proc owner */
	Oid			prolang;		/* OID of pg_language entry */
	bool		proisagg;		/* is it an aggregate? */
	bool		prosecdef;		/* security definer */
	bool		proimplicit;	/* can be invoked as implicit coercion? */
	bool		proisstrict;	/* strict with respect to NULLs? */
	bool		proretset;		/* returns a set? */
	char		provolatile;	/* see PROVOLATILE_ categories below */
	int2		pronargs;		/* number of arguments */
	Oid			prorettype;		/* OID of result type */
	oidvector	proargtypes;	/* OIDs of argument types */
	int4		probyte_pct;	/* currently unused */
	int4		properbyte_cpu;	/* unused */
	int4		propercall_cpu;	/* unused */
	int4		prooutin_ratio;	/* unused */
	text		prosrc;			/* VARIABLE LENGTH FIELD */
	bytea		probin;			/* VARIABLE LENGTH FIELD */
	aclitem		proacl[1];		/* VARIABLE LENGTH FIELD */
} FormData_pg_proc;

/* ----------------
 *		Form_pg_proc corresponds to a pointer to a tuple with
 *		the format of pg_proc relation.
 * ----------------
 */
typedef FormData_pg_proc *Form_pg_proc;

/* ----------------
 *		compiler constants for pg_proc
 * ----------------
 */
#define Natts_pg_proc					20
#define Anum_pg_proc_proname			1
#define Anum_pg_proc_pronamespace		2
#define Anum_pg_proc_proowner			3
#define Anum_pg_proc_prolang			4
#define Anum_pg_proc_proisagg			5
#define Anum_pg_proc_prosecdef			6
#define Anum_pg_proc_proimplicit		7
#define Anum_pg_proc_proisstrict		8
#define Anum_pg_proc_proretset			9
#define Anum_pg_proc_provolatile		10
#define Anum_pg_proc_pronargs			11
#define Anum_pg_proc_prorettype			12
#define Anum_pg_proc_proargtypes		13
#define Anum_pg_proc_probyte_pct		14
#define Anum_pg_proc_properbyte_cpu		15
#define Anum_pg_proc_propercall_cpu		16
#define Anum_pg_proc_prooutin_ratio		17
#define Anum_pg_proc_prosrc				18
#define Anum_pg_proc_probin				19
#define Anum_pg_proc_proacl				20

/* ----------------
 *		initial contents of pg_proc
 * ----------------
 */

/* keep the following ordered by OID so that later changes can be made easier */

/* OIDS 1 - 99 */

DATA(insert OID = 1242 (  boolin		   PGNSP PGUID 12 f f f t f i 1 16 "0" 100 0 0	100  boolin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1243 (  boolout		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	boolout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1244 (  byteain		   PGNSP PGUID 12 f f f t f i 1 17 "0" 100 0 0 100	byteain - _null_ ));
DESCR("(internal)");
DATA(insert OID =  31 (  byteaout		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	byteaout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1245 (  charin		   PGNSP PGUID 12 f f f t f i 1 18 "0" 100 0 0 100	charin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  33 (  charout		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	charout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  34 (  namein			   PGNSP PGUID 12 f f f t f i 1 19 "0" 100 0 0 100	namein - _null_ ));
DESCR("(internal)");
DATA(insert OID =  35 (  nameout		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	nameout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  38 (  int2in			   PGNSP PGUID 12 f f f t f i 1 21 "0" 100 0 0 100	int2in - _null_ ));
DESCR("(internal)");
DATA(insert OID =  39 (  int2out		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	int2out - _null_ ));
DESCR("(internal)");
DATA(insert OID =  40 (  int2vectorin	   PGNSP PGUID 12 f f f t f i 1 22 "0" 100 0 0 100	int2vectorin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  41 (  int2vectorout	   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	int2vectorout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  42 (  int4in			   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	int4in - _null_ ));
DESCR("(internal)");
DATA(insert OID =  43 (  int4out		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	int4out - _null_ ));
DESCR("(internal)");
DATA(insert OID =  44 (  regprocin		   PGNSP PGUID 12 f f f t f s 1 24 "0" 100 0 0 100	regprocin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  45 (  regprocout		   PGNSP PGUID 12 f f f t f s 1 23 "0" 100 0 0 100	regprocout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  46 (  textin			   PGNSP PGUID 12 f f f t f i 1 25 "0" 100 0 0 100	textin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  47 (  textout		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	textout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  48 (  tidin			   PGNSP PGUID 12 f f f t f i 1 27 "0" 100 0 0 100	tidin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  49 (  tidout			   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	tidout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  50 (  xidin			   PGNSP PGUID 12 f f f t f i 1 28 "0" 100 0 0 100	xidin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  51 (  xidout			   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	xidout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  52 (  cidin			   PGNSP PGUID 12 f f f t f i 1 29 "0" 100 0 0 100	cidin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  53 (  cidout			   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	cidout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  54 (  oidvectorin	   PGNSP PGUID 12 f f f t f i 1 30 "0" 100 0 0 100	oidvectorin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  55 (  oidvectorout	   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	oidvectorout - _null_ ));
DESCR("(internal)");
DATA(insert OID =  56 (  boollt			   PGNSP PGUID 12 f f f t f i 2 16 "16 16" 100 0 0 100	boollt - _null_ ));
DESCR("less-than");
DATA(insert OID =  57 (  boolgt			   PGNSP PGUID 12 f f f t f i 2 16 "16 16" 100 0 0 100	boolgt - _null_ ));
DESCR("greater-than");
DATA(insert OID =  60 (  booleq			   PGNSP PGUID 12 f f f t f i 2 16 "16 16" 100 0 0 100	booleq - _null_ ));
DESCR("equal");
DATA(insert OID =  61 (  chareq			   PGNSP PGUID 12 f f f t f i 2 16 "18 18" 100 0 0 100	chareq - _null_ ));
DESCR("equal");
DATA(insert OID =  62 (  nameeq			   PGNSP PGUID 12 f f f t f i 2 16 "19 19" 100 0 0 100	nameeq - _null_ ));
DESCR("equal");
DATA(insert OID =  63 (  int2eq			   PGNSP PGUID 12 f f f t f i 2 16 "21 21" 100 0 0 100	int2eq - _null_ ));
DESCR("equal");
DATA(insert OID =  64 (  int2lt			   PGNSP PGUID 12 f f f t f i 2 16 "21 21" 100 0 0 100	int2lt - _null_ ));
DESCR("less-than");
DATA(insert OID =  65 (  int4eq			   PGNSP PGUID 12 f f f t f i 2 16 "23 23" 100 0 0 100	int4eq - _null_ ));
DESCR("equal");
DATA(insert OID =  66 (  int4lt			   PGNSP PGUID 12 f f f t f i 2 16 "23 23" 100 0 0 100	int4lt - _null_ ));
DESCR("less-than");
DATA(insert OID =  67 (  texteq			   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	texteq - _null_ ));
DESCR("equal");
DATA(insert OID =  68 (  xideq			   PGNSP PGUID 12 f f f t f i 2 16 "28 28" 100 0 0 100	xideq - _null_ ));
DESCR("equal");
DATA(insert OID =  69 (  cideq			   PGNSP PGUID 12 f f f t f i 2 16 "29 29" 100 0 0 100	cideq - _null_ ));
DESCR("equal");
DATA(insert OID =  70 (  charne			   PGNSP PGUID 12 f f f t f i 2 16 "18 18" 100 0 0 100	charne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1246 (  charlt		   PGNSP PGUID 12 f f f t f i 2 16 "18 18" 100 0 0 100	charlt - _null_ ));
DESCR("less-than");
DATA(insert OID =  72 (  charle			   PGNSP PGUID 12 f f f t f i 2 16 "18 18" 100 0 0 100	charle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID =  73 (  chargt			   PGNSP PGUID 12 f f f t f i 2 16 "18 18" 100 0 0 100	chargt - _null_ ));
DESCR("greater-than");
DATA(insert OID =  74 (  charge			   PGNSP PGUID 12 f f f t f i 2 16 "18 18" 100 0 0 100	charge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1248 (  charpl		   PGNSP PGUID 12 f f f t f i 2 18 "18 18" 100 0 0 100	charpl - _null_ ));
DESCR("add");
DATA(insert OID = 1250 (  charmi		   PGNSP PGUID 12 f f f t f i 2 18 "18 18" 100 0 0 100	charmi - _null_ ));
DESCR("subtract");
DATA(insert OID =  77 (  charmul		   PGNSP PGUID 12 f f f t f i 2 18 "18 18" 100 0 0 100	charmul - _null_ ));
DESCR("multiply");
DATA(insert OID =  78 (  chardiv		   PGNSP PGUID 12 f f f t f i 2 18 "18 18" 100 0 0 100	chardiv - _null_ ));
DESCR("divide");

DATA(insert OID =  79 (  nameregexeq	   PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100	nameregexeq - _null_ ));
DESCR("matches regex., case-sensitive");
DATA(insert OID = 1252 (  nameregexne	   PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100	nameregexne - _null_ ));
DESCR("does not match regex., case-sensitive");
DATA(insert OID = 1254 (  textregexeq	   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	textregexeq - _null_ ));
DESCR("matches regex., case-sensitive");
DATA(insert OID = 1256 (  textregexne	   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	textregexne - _null_ ));
DESCR("does not match regex., case-sensitive");
DATA(insert OID = 1257 (  textlen		   PGNSP PGUID 12 f f f t f i 1 23 "25" 100 0 0 100  textlen - _null_ ));
DESCR("length");
DATA(insert OID = 1258 (  textcat		   PGNSP PGUID 12 f f f t f i 2 25 "25 25" 100 0 0 100	textcat - _null_ ));
DESCR("concatenate");

DATA(insert OID =  84 (  boolne			   PGNSP PGUID 12 f f f t f i 2 16 "16 16" 100 0 0 100	boolne - _null_ ));
DESCR("not equal");
DATA(insert OID =  89 (  version		   PGNSP PGUID 12 f f f t f s 0 25 "" 100 0 0 100 pgsql_version - _null_ ));
DESCR("PostgreSQL version string");

/* OIDS 100 - 199 */

DATA(insert OID = 100 (  int8fac		   PGNSP PGUID 12 f f f t f i 1 20 "20" 100 0 0 100  int8fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 101 (  eqsel			   PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  eqsel - _null_ ));
DESCR("restriction selectivity of = and related operators");
DATA(insert OID = 102 (  neqsel			   PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  neqsel - _null_ ));
DESCR("restriction selectivity of <> and related operators");
DATA(insert OID = 103 (  scalarltsel	   PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  scalarltsel - _null_ ));
DESCR("restriction selectivity of < and related operators on scalar datatypes");
DATA(insert OID = 104 (  scalargtsel	   PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  scalargtsel - _null_ ));
DESCR("restriction selectivity of > and related operators on scalar datatypes");
DATA(insert OID = 105 (  eqjoinsel		   PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  eqjoinsel - _null_ ));
DESCR("join selectivity of = and related operators");
DATA(insert OID = 106 (  neqjoinsel		   PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  neqjoinsel - _null_ ));
DESCR("join selectivity of <> and related operators");
DATA(insert OID = 107 (  scalarltjoinsel   PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  scalarltjoinsel - _null_ ));
DESCR("join selectivity of < and related operators on scalar datatypes");
DATA(insert OID = 108 (  scalargtjoinsel   PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  scalargtjoinsel - _null_ ));
DESCR("join selectivity of > and related operators on scalar datatypes");

DATA(insert OID =  109 (  unknownin		   PGNSP PGUID 12 f f f t f i 1 705 "0" 100 0 0 100	unknownin - _null_ ));
DESCR("(internal)");
DATA(insert OID =  110 (  unknownout	   PGNSP PGUID 12 f f f t f i 1 23  "0" 100 0 0 100	unknownout - _null_ ));
DESCR("(internal)");

DATA(insert OID = 112 (  text			   PGNSP PGUID 12 f f t t f i 1  25 "23" 100 0 0 100  int4_text - _null_ ));
DESCR("convert int4 to text");
DATA(insert OID = 113 (  text			   PGNSP PGUID 12 f f t t f i 1  25 "21" 100 0 0 100  int2_text - _null_ ));
DESCR("convert int2 to text");
DATA(insert OID = 114 (  text			   PGNSP PGUID 12 f f f t f i 1  25 "26" 100 0 0 100  oid_text - _null_ ));
DESCR("convert oid to text");

DATA(insert OID = 115 (  box_above		   PGNSP PGUID 12 f f f t f i 2  16 "603 603" 100 0 0 100  box_above - _null_ ));
DESCR("is above");
DATA(insert OID = 116 (  box_below		   PGNSP PGUID 12 f f f t f i 2  16 "603 603" 100 0 0 100  box_below - _null_ ));
DESCR("is below");

DATA(insert OID = 117 (  point_in		   PGNSP PGUID 12 f f f t f i 1 600 "0" 100 0 0 100  point_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 118 (  point_out		   PGNSP PGUID 12 f f f t f i 1 23	"600" 100 0 0 100  point_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 119 (  lseg_in		   PGNSP PGUID 12 f f f t f i 1 601 "0" 100 0 0 100  lseg_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 120 (  lseg_out		   PGNSP PGUID 12 f f f t f i 1 23	"0" 100 0 0 100  lseg_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 121 (  path_in		   PGNSP PGUID 12 f f f t f i 1 602 "0" 100 0 0 100  path_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 122 (  path_out		   PGNSP PGUID 12 f f f t f i 1 23	"0" 100 0 0 100  path_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 123 (  box_in			   PGNSP PGUID 12 f f f t f i 1 603 "0" 100 0 0 100  box_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 124 (  box_out		   PGNSP PGUID 12 f f f t f i 1 23	"0" 100 0 0 100  box_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 125 (  box_overlap	   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_overlap - _null_ ));
DESCR("overlaps");
DATA(insert OID = 126 (  box_ge			   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_ge - _null_ ));
DESCR("greater-than-or-equal by area");
DATA(insert OID = 127 (  box_gt			   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_gt - _null_ ));
DESCR("greater-than by area");
DATA(insert OID = 128 (  box_eq			   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_eq - _null_ ));
DESCR("equal by area");
DATA(insert OID = 129 (  box_lt			   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_lt - _null_ ));
DESCR("less-than by area");
DATA(insert OID = 130 (  box_le			   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_le - _null_ ));
DESCR("less-than-or-equal by area");
DATA(insert OID = 131 (  point_above	   PGNSP PGUID 12 f f f t f i 2 16 "600 600" 100 0 0 100  point_above - _null_ ));
DESCR("is above");
DATA(insert OID = 132 (  point_left		   PGNSP PGUID 12 f f f t f i 2 16 "600 600" 100 0 0 100  point_left - _null_ ));
DESCR("is left of");
DATA(insert OID = 133 (  point_right	   PGNSP PGUID 12 f f f t f i 2 16 "600 600" 100 0 0 100  point_right - _null_ ));
DESCR("is right of");
DATA(insert OID = 134 (  point_below	   PGNSP PGUID 12 f f f t f i 2 16 "600 600" 100 0 0 100  point_below - _null_ ));
DESCR("is below");
DATA(insert OID = 135 (  point_eq		   PGNSP PGUID 12 f f f t f i 2 16 "600 600" 100 0 0 100  point_eq - _null_ ));
DESCR("same as");
DATA(insert OID = 136 (  on_pb			   PGNSP PGUID 12 f f f t f i 2 16 "600 603" 100 0 0 100  on_pb - _null_ ));
DESCR("point is inside");
DATA(insert OID = 137 (  on_ppath		   PGNSP PGUID 12 f f f t f i 2 16 "600 602" 100 0 0 100  on_ppath - _null_ ));
DESCR("contained in");
DATA(insert OID = 138 (  box_center		   PGNSP PGUID 12 f f f t f i 1 600 "603" 100 0 0 100  box_center - _null_ ));
DESCR("center of");
DATA(insert OID = 139 (  areasel		   PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  areasel - _null_ ));
DESCR("restriction selectivity for area-comparison operators");
DATA(insert OID = 140 (  areajoinsel	   PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  areajoinsel - _null_ ));
DESCR("join selectivity for area-comparison operators");
DATA(insert OID = 141 (  int4mul		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 142 (  int4fac		   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  int4fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 144 (  int4ne			   PGNSP PGUID 12 f f f t f i 2 16 "23 23" 100 0 0 100	int4ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 145 (  int2ne			   PGNSP PGUID 12 f f f t f i 2 16 "21 21" 100 0 0 100	int2ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 146 (  int2gt			   PGNSP PGUID 12 f f f t f i 2 16 "21 21" 100 0 0 100	int2gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 147 (  int4gt			   PGNSP PGUID 12 f f f t f i 2 16 "23 23" 100 0 0 100	int4gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 148 (  int2le			   PGNSP PGUID 12 f f f t f i 2 16 "21 21" 100 0 0 100	int2le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 149 (  int4le			   PGNSP PGUID 12 f f f t f i 2 16 "23 23" 100 0 0 100	int4le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 150 (  int4ge			   PGNSP PGUID 12 f f f t f i 2 16 "23 23" 100 0 0 100	int4ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 151 (  int2ge			   PGNSP PGUID 12 f f f t f i 2 16 "21 21" 100 0 0 100	int2ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 152 (  int2mul		   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 153 (  int2div		   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2div - _null_ ));
DESCR("divide");
DATA(insert OID = 154 (  int4div		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4div - _null_ ));
DESCR("divide");
DATA(insert OID = 155 (  int2mod		   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 156 (  int4mod		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 157 (  textne			   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	textne - _null_ ));
DESCR("not equal");
DATA(insert OID = 158 (  int24eq		   PGNSP PGUID 12 f f f t f i 2 16 "21 23" 100 0 0 100	int24eq - _null_ ));
DESCR("equal");
DATA(insert OID = 159 (  int42eq		   PGNSP PGUID 12 f f f t f i 2 16 "23 21" 100 0 0 100	int42eq - _null_ ));
DESCR("equal");
DATA(insert OID = 160 (  int24lt		   PGNSP PGUID 12 f f f t f i 2 16 "21 23" 100 0 0 100	int24lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 161 (  int42lt		   PGNSP PGUID 12 f f f t f i 2 16 "23 21" 100 0 0 100	int42lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 162 (  int24gt		   PGNSP PGUID 12 f f f t f i 2 16 "21 23" 100 0 0 100	int24gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 163 (  int42gt		   PGNSP PGUID 12 f f f t f i 2 16 "23 21" 100 0 0 100	int42gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 164 (  int24ne		   PGNSP PGUID 12 f f f t f i 2 16 "21 23" 100 0 0 100	int24ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 165 (  int42ne		   PGNSP PGUID 12 f f f t f i 2 16 "23 21" 100 0 0 100	int42ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 166 (  int24le		   PGNSP PGUID 12 f f f t f i 2 16 "21 23" 100 0 0 100	int24le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 167 (  int42le		   PGNSP PGUID 12 f f f t f i 2 16 "23 21" 100 0 0 100	int42le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 168 (  int24ge		   PGNSP PGUID 12 f f f t f i 2 16 "21 23" 100 0 0 100	int24ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 169 (  int42ge		   PGNSP PGUID 12 f f f t f i 2 16 "23 21" 100 0 0 100	int42ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 170 (  int24mul		   PGNSP PGUID 12 f f f t f i 2 23 "21 23" 100 0 0 100	int24mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 171 (  int42mul		   PGNSP PGUID 12 f f f t f i 2 23 "23 21" 100 0 0 100	int42mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 172 (  int24div		   PGNSP PGUID 12 f f f t f i 2 23 "21 23" 100 0 0 100	int24div - _null_ ));
DESCR("divide");
DATA(insert OID = 173 (  int42div		   PGNSP PGUID 12 f f f t f i 2 23 "23 21" 100 0 0 100	int42div - _null_ ));
DESCR("divide");
DATA(insert OID = 174 (  int24mod		   PGNSP PGUID 12 f f f t f i 2 23 "21 23" 100 0 0 100	int24mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 175 (  int42mod		   PGNSP PGUID 12 f f f t f i 2 23 "23 21" 100 0 0 100	int42mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 176 (  int2pl			   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2pl - _null_ ));
DESCR("add");
DATA(insert OID = 177 (  int4pl			   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4pl - _null_ ));
DESCR("add");
DATA(insert OID = 178 (  int24pl		   PGNSP PGUID 12 f f f t f i 2 23 "21 23" 100 0 0 100	int24pl - _null_ ));
DESCR("add");
DATA(insert OID = 179 (  int42pl		   PGNSP PGUID 12 f f f t f i 2 23 "23 21" 100 0 0 100	int42pl - _null_ ));
DESCR("add");
DATA(insert OID = 180 (  int2mi			   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 181 (  int4mi			   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 182 (  int24mi		   PGNSP PGUID 12 f f f t f i 2 23 "21 23" 100 0 0 100	int24mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 183 (  int42mi		   PGNSP PGUID 12 f f f t f i 2 23 "23 21" 100 0 0 100	int42mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 184 (  oideq			   PGNSP PGUID 12 f f f t f i 2 16 "26 26" 100 0 0 100	oideq - _null_ ));
DESCR("equal");
DATA(insert OID = 185 (  oidne			   PGNSP PGUID 12 f f f t f i 2 16 "26 26" 100 0 0 100	oidne - _null_ ));
DESCR("not equal");
DATA(insert OID = 186 (  box_same		   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_same - _null_ ));
DESCR("same as");
DATA(insert OID = 187 (  box_contain	   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_contain - _null_ ));
DESCR("contains");
DATA(insert OID = 188 (  box_left		   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_left - _null_ ));
DESCR("is left of");
DATA(insert OID = 189 (  box_overleft	   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_overleft - _null_ ));
DESCR("overlaps, but does not extend to right of");
DATA(insert OID = 190 (  box_overright	   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_overright - _null_ ));
DESCR("overlaps, but does not extend to left of");
DATA(insert OID = 191 (  box_right		   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_right - _null_ ));
DESCR("is right of");
DATA(insert OID = 192 (  box_contained	   PGNSP PGUID 12 f f f t f i 2 16 "603 603" 100 0 0 100  box_contained - _null_ ));
DESCR("contained in");
DATA(insert OID = 193 (  rt_box_union	   PGNSP PGUID 12 f f f t f i 2 603 "603 603" 100 0 0 100  rt_box_union - _null_ ));
DESCR("r-tree");
DATA(insert OID = 194 (  rt_box_inter	   PGNSP PGUID 12 f f f t f i 2 603 "603 603" 100 0 0 100  rt_box_inter - _null_ ));
DESCR("r-tree");
DATA(insert OID = 195 (  rt_box_size	   PGNSP PGUID 12 f f f t f i 2 700 "603 700" 100 0 0 100  rt_box_size - _null_ ));
DESCR("r-tree");
DATA(insert OID = 196 (  rt_bigbox_size    PGNSP PGUID 12 f f f t f i 2 700 "603 700" 100 0 0 100  rt_bigbox_size - _null_ ));
DESCR("r-tree");
DATA(insert OID = 197 (  rt_poly_union	   PGNSP PGUID 12 f f f t f i 2 604 "604 604" 100 0 0 100  rt_poly_union - _null_ ));
DESCR("r-tree");
DATA(insert OID = 198 (  rt_poly_inter	   PGNSP PGUID 12 f f f t f i 2 604 "604 604" 100 0 0 100  rt_poly_inter - _null_ ));
DESCR("r-tree");
DATA(insert OID = 199 (  rt_poly_size	   PGNSP PGUID 12 f f f t f i 2 23 "604 700" 100 0 0 100  rt_poly_size - _null_ ));
DESCR("r-tree");

/* OIDS 200 - 299 */

DATA(insert OID = 200 (  float4in		   PGNSP PGUID 12 f f f t f i 1 700 "0" 100 0 0 100  float4in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 201 (  float4out		   PGNSP PGUID 12 f f f t f i 1 23	"700" 100 0 0 100  float4out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 202 (  float4mul		   PGNSP PGUID 12 f f f t f i 2 700 "700 700" 100 0 0 100  float4mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 203 (  float4div		   PGNSP PGUID 12 f f f t f i 2 700 "700 700" 100 0 0 100  float4div - _null_ ));
DESCR("divide");
DATA(insert OID = 204 (  float4pl		   PGNSP PGUID 12 f f f t f i 2 700 "700 700" 100 0 0 100  float4pl - _null_ ));
DESCR("add");
DATA(insert OID = 205 (  float4mi		   PGNSP PGUID 12 f f f t f i 2 700 "700 700" 100 0 0 100  float4mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 206 (  float4um		   PGNSP PGUID 12 f f f t f i 1 700 "700" 100 0 0 100  float4um - _null_ ));
DESCR("negate");
DATA(insert OID = 207 (  float4abs		   PGNSP PGUID 12 f f f t f i 1 700 "700" 100 0 0 100  float4abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 208 (  float4_accum	   PGNSP PGUID 12 f f f t f i 2 1022 "1022 700" 100 0 0 100  float4_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 209 (  float4larger	   PGNSP PGUID 12 f f f t f i 2 700 "700 700" 100 0 0 100  float4larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 211 (  float4smaller	   PGNSP PGUID 12 f f f t f i 2 700 "700 700" 100 0 0 100  float4smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 212 (  int4um			   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  int4um - _null_ ));
DESCR("negate");
DATA(insert OID = 213 (  int2um			   PGNSP PGUID 12 f f f t f i 1 21 "21" 100 0 0 100  int2um - _null_ ));
DESCR("negate");

DATA(insert OID = 214 (  float8in		   PGNSP PGUID 12 f f f t f i 1 701 "0" 100 0 0 100  float8in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 215 (  float8out		   PGNSP PGUID 12 f f f t f i 1 23	"701" 100 0 0 100  float8out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 216 (  float8mul		   PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100  float8mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 217 (  float8div		   PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100  float8div - _null_ ));
DESCR("divide");
DATA(insert OID = 218 (  float8pl		   PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100  float8pl - _null_ ));
DESCR("add");
DATA(insert OID = 219 (  float8mi		   PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100  float8mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 220 (  float8um		   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  float8um - _null_ ));
DESCR("negate");
DATA(insert OID = 221 (  float8abs		   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  float8abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 222 (  float8_accum	   PGNSP PGUID 12 f f f t f i 2 1022 "1022 701" 100 0 0 100  float8_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 223 (  float8larger	   PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100  float8larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 224 (  float8smaller	   PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100  float8smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 225 (  lseg_center	   PGNSP PGUID 12 f f f t f i 1 600 "601" 100 0 0 100  lseg_center - _null_ ));
DESCR("center of");
DATA(insert OID = 226 (  path_center	   PGNSP PGUID 12 f f f t f i 1 600 "602" 100 0 0 100  path_center - _null_ ));
DESCR("center of");
DATA(insert OID = 227 (  poly_center	   PGNSP PGUID 12 f f f t f i 1 600 "604" 100 0 0 100  poly_center - _null_ ));
DESCR("center of");

DATA(insert OID = 228 (  dround			   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dround - _null_ ));
DESCR("round to nearest integer");
DATA(insert OID = 229 (  dtrunc			   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dtrunc - _null_ ));
DESCR("truncate to integer");
DATA(insert OID = 230 (  dsqrt			   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dsqrt - _null_ ));
DESCR("square root");
DATA(insert OID = 231 (  dcbrt			   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dcbrt - _null_ ));
DESCR("cube root");
DATA(insert OID = 232 (  dpow			   PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100  dpow - _null_ ));
DESCR("exponentiation (x^y)");
DATA(insert OID = 233 (  dexp			   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dexp - _null_ ));
DESCR("natural exponential (e^x)");
DATA(insert OID = 234 (  dlog1			   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dlog1 - _null_ ));
DESCR("natural logarithm");
DATA(insert OID = 235 (  float8			   PGNSP PGUID 12 f f t t f i 1 701  "21" 100 0 0 100  i2tod - _null_ ));
DESCR("convert int2 to float8");
DATA(insert OID = 236 (  float4			   PGNSP PGUID 12 f f t t f i 1 700  "21" 100 0 0 100  i2tof - _null_ ));
DESCR("convert int2 to float4");
DATA(insert OID = 237 (  int2			   PGNSP PGUID 12 f f f t f i 1  21 "701" 100 0 0 100  dtoi2 - _null_ ));
DESCR("convert float8 to int2");
DATA(insert OID = 238 (  int2			   PGNSP PGUID 12 f f f t f i 1  21 "700" 100 0 0 100  ftoi2 - _null_ ));
DESCR("convert float4 to int2");
DATA(insert OID = 239 (  line_distance	   PGNSP PGUID 12 f f f t f i 2 701 "628 628" 100 0 0 100  line_distance - _null_ ));
DESCR("distance between");

DATA(insert OID = 240 (  nabstimein		   PGNSP PGUID 12 f f f t f s 1 702 "0" 100 0 0 100  nabstimein - _null_ ));
DESCR("(internal)");
DATA(insert OID = 241 (  nabstimeout	   PGNSP PGUID 12 f f f t f s 1 23	"0" 100 0 0 100  nabstimeout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 242 (  reltimein		   PGNSP PGUID 12 f f f t f s 1 703 "0" 100 0 0 100  reltimein - _null_ ));
DESCR("(internal)");
DATA(insert OID = 243 (  reltimeout		   PGNSP PGUID 12 f f f t f s 1 23	"0" 100 0 0 100  reltimeout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 244 (  timepl			   PGNSP PGUID 12 f f f t f i 2 702 "702 703" 100 0 0 100  timepl - _null_ ));
DESCR("add");
DATA(insert OID = 245 (  timemi			   PGNSP PGUID 12 f f f t f i 2 702 "702 703" 100 0 0 100  timemi - _null_ ));
DESCR("subtract");
DATA(insert OID = 246 (  tintervalin	   PGNSP PGUID 12 f f f t f s 1 704 "0" 100 0 0 100  tintervalin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 247 (  tintervalout	   PGNSP PGUID 12 f f f t f s 1 23	"0" 100 0 0 100  tintervalout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 248 (  intinterval	   PGNSP PGUID 12 f f f t f i 2 16 "702 704" 100 0 0 100  intinterval - _null_ ));
DESCR("abstime in tinterval");
DATA(insert OID = 249 (  tintervalrel	   PGNSP PGUID 12 f f f t f i 1 703 "704" 100 0 0 100  tintervalrel - _null_ ));
DESCR("");
DATA(insert OID = 250 (  timenow		   PGNSP PGUID 12 f f f t f s 0 702 "0" 100 0 0 100  timenow - _null_ ));
DESCR("Current date and time (abstime)");
DATA(insert OID = 251 (  abstimeeq		   PGNSP PGUID 12 f f f t f i 2 16 "702 702" 100 0 0 100  abstimeeq - _null_ ));
DESCR("equal");
DATA(insert OID = 252 (  abstimene		   PGNSP PGUID 12 f f f t f i 2 16 "702 702" 100 0 0 100  abstimene - _null_ ));
DESCR("not equal");
DATA(insert OID = 253 (  abstimelt		   PGNSP PGUID 12 f f f t f i 2 16 "702 702" 100 0 0 100  abstimelt - _null_ ));
DESCR("less-than");
DATA(insert OID = 254 (  abstimegt		   PGNSP PGUID 12 f f f t f i 2 16 "702 702" 100 0 0 100  abstimegt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 255 (  abstimele		   PGNSP PGUID 12 f f f t f i 2 16 "702 702" 100 0 0 100  abstimele - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 256 (  abstimege		   PGNSP PGUID 12 f f f t f i 2 16 "702 702" 100 0 0 100  abstimege - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 257 (  reltimeeq		   PGNSP PGUID 12 f f f t f i 2 16 "703 703" 100 0 0 100  reltimeeq - _null_ ));
DESCR("equal");
DATA(insert OID = 258 (  reltimene		   PGNSP PGUID 12 f f f t f i 2 16 "703 703" 100 0 0 100  reltimene - _null_ ));
DESCR("not equal");
DATA(insert OID = 259 (  reltimelt		   PGNSP PGUID 12 f f f t f i 2 16 "703 703" 100 0 0 100  reltimelt - _null_ ));
DESCR("less-than");
DATA(insert OID = 260 (  reltimegt		   PGNSP PGUID 12 f f f t f i 2 16 "703 703" 100 0 0 100  reltimegt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 261 (  reltimele		   PGNSP PGUID 12 f f f t f i 2 16 "703 703" 100 0 0 100  reltimele - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 262 (  reltimege		   PGNSP PGUID 12 f f f t f i 2 16 "703 703" 100 0 0 100  reltimege - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 263 (  tintervalsame	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervalsame - _null_ ));
DESCR("same as");
DATA(insert OID = 264 (  tintervalct	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervalct - _null_ ));
DESCR("less-than");
DATA(insert OID = 265 (  tintervalov	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervalov - _null_ ));
DESCR("overlaps");
DATA(insert OID = 266 (  tintervalleneq    PGNSP PGUID 12 f f f t f i 2 16 "704 703" 100 0 0 100  tintervalleneq - _null_ ));
DESCR("length equal");
DATA(insert OID = 267 (  tintervallenne    PGNSP PGUID 12 f f f t f i 2 16 "704 703" 100 0 0 100  tintervallenne - _null_ ));
DESCR("length not equal to");
DATA(insert OID = 268 (  tintervallenlt    PGNSP PGUID 12 f f f t f i 2 16 "704 703" 100 0 0 100  tintervallenlt - _null_ ));
DESCR("length less-than");
DATA(insert OID = 269 (  tintervallengt    PGNSP PGUID 12 f f f t f i 2 16 "704 703" 100 0 0 100  tintervallengt - _null_ ));
DESCR("length greater-than");
DATA(insert OID = 270 (  tintervallenle    PGNSP PGUID 12 f f f t f i 2 16 "704 703" 100 0 0 100  tintervallenle - _null_ ));
DESCR("length less-than-or-equal");
DATA(insert OID = 271 (  tintervallenge    PGNSP PGUID 12 f f f t f i 2 16 "704 703" 100 0 0 100  tintervallenge - _null_ ));
DESCR("length greater-than-or-equal");
DATA(insert OID = 272 (  tintervalstart    PGNSP PGUID 12 f f f t f i 1 702 "704" 100 0 0 100  tintervalstart - _null_ ));
DESCR("start of interval");
DATA(insert OID = 273 (  tintervalend	   PGNSP PGUID 12 f f f t f i 1 702 "704" 100 0 0 100  tintervalend - _null_ ));
DESCR("");
DATA(insert OID = 274 (  timeofday		   PGNSP PGUID 12 f f f t f v 0 25 "0" 100 0 0 100	timeofday - _null_ ));
DESCR("Current date and time - increments during transactions");
DATA(insert OID = 275 (  isfinite		   PGNSP PGUID 12 f f f t f i 1 16 "702" 100 0 0 100  abstime_finite - _null_ ));
DESCR("");

DATA(insert OID = 276 (  int2fac		   PGNSP PGUID 12 f f f t f i 1 23 "21" 100 0 0 100  int2fac - _null_ ));
DESCR("factorial");

DATA(insert OID = 277 (  inter_sl		   PGNSP PGUID 12 f f f t f i 2 16 "601 628" 100 0 0 100  inter_sl - _null_ ));
DESCR("");
DATA(insert OID = 278 (  inter_lb		   PGNSP PGUID 12 f f f t f i 2 16 "628 603" 100 0 0 100  inter_lb - _null_ ));
DESCR("");

DATA(insert OID = 279 (  float48mul		   PGNSP PGUID 12 f f f t f i 2 701 "700 701" 100 0 0 100  float48mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 280 (  float48div		   PGNSP PGUID 12 f f f t f i 2 701 "700 701" 100 0 0 100  float48div - _null_ ));
DESCR("divide");
DATA(insert OID = 281 (  float48pl		   PGNSP PGUID 12 f f f t f i 2 701 "700 701" 100 0 0 100  float48pl - _null_ ));
DESCR("add");
DATA(insert OID = 282 (  float48mi		   PGNSP PGUID 12 f f f t f i 2 701 "700 701" 100 0 0 100  float48mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 283 (  float84mul		   PGNSP PGUID 12 f f f t f i 2 701 "701 700" 100 0 0 100  float84mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 284 (  float84div		   PGNSP PGUID 12 f f f t f i 2 701 "701 700" 100 0 0 100  float84div - _null_ ));
DESCR("divide");
DATA(insert OID = 285 (  float84pl		   PGNSP PGUID 12 f f f t f i 2 701 "701 700" 100 0 0 100  float84pl - _null_ ));
DESCR("add");
DATA(insert OID = 286 (  float84mi		   PGNSP PGUID 12 f f f t f i 2 701 "701 700" 100 0 0 100  float84mi - _null_ ));
DESCR("subtract");

DATA(insert OID = 287 (  float4eq		   PGNSP PGUID 12 f f f t f i 2 16 "700 700" 100 0 0 100  float4eq - _null_ ));
DESCR("equal");
DATA(insert OID = 288 (  float4ne		   PGNSP PGUID 12 f f f t f i 2 16 "700 700" 100 0 0 100  float4ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 289 (  float4lt		   PGNSP PGUID 12 f f f t f i 2 16 "700 700" 100 0 0 100  float4lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 290 (  float4le		   PGNSP PGUID 12 f f f t f i 2 16 "700 700" 100 0 0 100  float4le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 291 (  float4gt		   PGNSP PGUID 12 f f f t f i 2 16 "700 700" 100 0 0 100  float4gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 292 (  float4ge		   PGNSP PGUID 12 f f f t f i 2 16 "700 700" 100 0 0 100  float4ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 293 (  float8eq		   PGNSP PGUID 12 f f f t f i 2 16 "701 701" 100 0 0 100  float8eq - _null_ ));
DESCR("equal");
DATA(insert OID = 294 (  float8ne		   PGNSP PGUID 12 f f f t f i 2 16 "701 701" 100 0 0 100  float8ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 295 (  float8lt		   PGNSP PGUID 12 f f f t f i 2 16 "701 701" 100 0 0 100  float8lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 296 (  float8le		   PGNSP PGUID 12 f f f t f i 2 16 "701 701" 100 0 0 100  float8le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 297 (  float8gt		   PGNSP PGUID 12 f f f t f i 2 16 "701 701" 100 0 0 100  float8gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 298 (  float8ge		   PGNSP PGUID 12 f f f t f i 2 16 "701 701" 100 0 0 100  float8ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 299 (  float48eq		   PGNSP PGUID 12 f f f t f i 2 16 "700 701" 100 0 0 100  float48eq - _null_ ));
DESCR("equal");

/* OIDS 300 - 399 */

DATA(insert OID = 300 (  float48ne		   PGNSP PGUID 12 f f f t f i 2 16 "700 701" 100 0 0 100  float48ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 301 (  float48lt		   PGNSP PGUID 12 f f f t f i 2 16 "700 701" 100 0 0 100  float48lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 302 (  float48le		   PGNSP PGUID 12 f f f t f i 2 16 "700 701" 100 0 0 100  float48le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 303 (  float48gt		   PGNSP PGUID 12 f f f t f i 2 16 "700 701" 100 0 0 100  float48gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 304 (  float48ge		   PGNSP PGUID 12 f f f t f i 2 16 "700 701" 100 0 0 100  float48ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 305 (  float84eq		   PGNSP PGUID 12 f f f t f i 2 16 "701 700" 100 0 0 100  float84eq - _null_ ));
DESCR("equal");
DATA(insert OID = 306 (  float84ne		   PGNSP PGUID 12 f f f t f i 2 16 "701 700" 100 0 0 100  float84ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 307 (  float84lt		   PGNSP PGUID 12 f f f t f i 2 16 "701 700" 100 0 0 100  float84lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 308 (  float84le		   PGNSP PGUID 12 f f f t f i 2 16 "701 700" 100 0 0 100  float84le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 309 (  float84gt		   PGNSP PGUID 12 f f f t f i 2 16 "701 700" 100 0 0 100  float84gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 310 (  float84ge		   PGNSP PGUID 12 f f f t f i 2 16 "701 700" 100 0 0 100  float84ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 311 (  float8			   PGNSP PGUID 12 f f t t f i 1 701 "700" 100 0 0 100  ftod - _null_ ));
DESCR("convert float4 to float8");
DATA(insert OID = 312 (  float4			   PGNSP PGUID 12 f f t t f i 1 700 "701" 100 0 0 100  dtof - _null_ ));
DESCR("convert float8 to float4");
DATA(insert OID = 313 (  int4			   PGNSP PGUID 12 f f t t f i 1  23  "21" 100 0 0 100  i2toi4 - _null_ ));
DESCR("convert int2 to int4");
DATA(insert OID = 314 (  int2			   PGNSP PGUID 12 f f t t f i 1  21  "23" 100 0 0 100  i4toi2 - _null_ ));
DESCR("convert int4 to int2");
DATA(insert OID = 315 (  int2vectoreq	   PGNSP PGUID 12 f f f t f i 2  16  "22 22" 100 0 0 100  int2vectoreq - _null_ ));
DESCR("equal");
DATA(insert OID = 316 (  float8			   PGNSP PGUID 12 f f t t f i 1 701  "23" 100 0 0 100  i4tod - _null_ ));
DESCR("convert int4 to float8");
DATA(insert OID = 317 (  int4			   PGNSP PGUID 12 f f f t f i 1  23 "701" 100 0 0 100  dtoi4 - _null_ ));
DESCR("convert float8 to int4");
DATA(insert OID = 318 (  float4			   PGNSP PGUID 12 f f t t f i 1 700  "23" 100 0 0 100  i4tof - _null_ ));
DESCR("convert int4 to float4");
DATA(insert OID = 319 (  int4			   PGNSP PGUID 12 f f f t f i 1  23 "700" 100 0 0 100  ftoi4 - _null_ ));
DESCR("convert float4 to int4");

DATA(insert OID = 320 (  rtinsert		   PGNSP PGUID 12 f f f t f v 5 23 "0 0 0 0 0" 100 0 0 100	rtinsert - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 322 (  rtgettuple		   PGNSP PGUID 12 f f f t f v 2 23 "0 0" 100 0 0 100  rtgettuple - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 323 (  rtbuild		   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	rtbuild - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 324 (  rtbeginscan	   PGNSP PGUID 12 f f f t f v 4 23 "0 0 0 0" 100 0 0 100  rtbeginscan - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 325 (  rtendscan		   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	rtendscan - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 326 (  rtmarkpos		   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	rtmarkpos - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 327 (  rtrestrpos		   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	rtrestrpos - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 328 (  rtrescan		   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	rtrescan - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 321 (  rtbulkdelete	   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	rtbulkdelete - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 1265 (  rtcostestimate   PGNSP PGUID 12 f f f t f v 8 0 "0 0 0 0 0 0 0 0" 100 0 0 100  rtcostestimate - _null_ ));
DESCR("r-tree(internal)");

DATA(insert OID = 330 (  btgettuple		   PGNSP PGUID 12 f f f t f v 2 23 "0 0" 100 0 0 100  btgettuple - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 331 (  btinsert		   PGNSP PGUID 12 f f f t f v 5 23 "0 0 0 0 0" 100 0 0 100	btinsert - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 333 (  btbeginscan	   PGNSP PGUID 12 f f f t f v 4 23 "0 0 0 0" 100 0 0 100  btbeginscan - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 334 (  btrescan		   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	btrescan - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 335 (  btendscan		   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	btendscan - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 336 (  btmarkpos		   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	btmarkpos - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 337 (  btrestrpos		   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	btrestrpos - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 338 (  btbuild		   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	btbuild - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 332 (  btbulkdelete	   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	btbulkdelete - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 1268 (  btcostestimate   PGNSP PGUID 12 f f f t f v 8 0 "0 0 0 0 0 0 0 0" 100 0 0 100  btcostestimate - _null_ ));
DESCR("btree(internal)");

DATA(insert OID = 339 (  poly_same		   PGNSP PGUID 12 f f f t f i 2 16 "604 604" 100 0 0 100  poly_same - _null_ ));
DESCR("same as");
DATA(insert OID = 340 (  poly_contain	   PGNSP PGUID 12 f f f t f i 2 16 "604 604" 100 0 0 100  poly_contain - _null_ ));
DESCR("contains");
DATA(insert OID = 341 (  poly_left		   PGNSP PGUID 12 f f f t f i 2 16 "604 604" 100 0 0 100  poly_left - _null_ ));
DESCR("is left of");
DATA(insert OID = 342 (  poly_overleft	   PGNSP PGUID 12 f f f t f i 2 16 "604 604" 100 0 0 100  poly_overleft - _null_ ));
DESCR("overlaps, but does not extend to right of");
DATA(insert OID = 343 (  poly_overright    PGNSP PGUID 12 f f f t f i 2 16 "604 604" 100 0 0 100  poly_overright - _null_ ));
DESCR("overlaps, but does not extend to left of");
DATA(insert OID = 344 (  poly_right		   PGNSP PGUID 12 f f f t f i 2 16 "604 604" 100 0 0 100  poly_right - _null_ ));
DESCR("is right of");
DATA(insert OID = 345 (  poly_contained    PGNSP PGUID 12 f f f t f i 2 16 "604 604" 100 0 0 100  poly_contained - _null_ ));
DESCR("contained in");
DATA(insert OID = 346 (  poly_overlap	   PGNSP PGUID 12 f f f t f i 2 16 "604 604" 100 0 0 100  poly_overlap - _null_ ));
DESCR("overlaps");
DATA(insert OID = 347 (  poly_in		   PGNSP PGUID 12 f f f t f i 1 604 "0" 100 0 0 100  poly_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 348 (  poly_out		   PGNSP PGUID 12 f f f t f i 1 23	"0" 100 0 0 100  poly_out - _null_ ));
DESCR("(internal)");

DATA(insert OID = 350 (  btint2cmp		   PGNSP PGUID 12 f f f t f i 2 23 "21 21" 100 0 0 100	btint2cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 351 (  btint4cmp		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	btint4cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 842 (  btint8cmp		   PGNSP PGUID 12 f f f t f i 2 23 "20 20" 100 0 0 100	btint8cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 354 (  btfloat4cmp	   PGNSP PGUID 12 f f f t f i 2 23 "700 700" 100 0 0 100  btfloat4cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 355 (  btfloat8cmp	   PGNSP PGUID 12 f f f t f i 2 23 "701 701" 100 0 0 100  btfloat8cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 356 (  btoidcmp		   PGNSP PGUID 12 f f f t f i 2 23 "26 26" 100 0 0 100	btoidcmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 404 (  btoidvectorcmp    PGNSP PGUID 12 f f f t f i 2 23 "30 30" 100 0 0 100	btoidvectorcmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 357 (  btabstimecmp	   PGNSP PGUID 12 f f f t f i 2 23 "702 702" 100 0 0 100  btabstimecmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 358 (  btcharcmp		   PGNSP PGUID 12 f f f t f i 2 23 "18 18" 100 0 0 100	btcharcmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 359 (  btnamecmp		   PGNSP PGUID 12 f f f t f i 2 23 "19 19" 100 0 0 100	btnamecmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 360 (  bttextcmp		   PGNSP PGUID 12 f f f t f i 2 23 "25 25" 100 0 0 100	bttextcmp - _null_ ));
DESCR("btree less-equal-greater");

DATA(insert OID = 361 (  lseg_distance	   PGNSP PGUID 12 f f f t f i 2 701 "601 601" 100 0 0 100  lseg_distance - _null_ ));
DESCR("distance between");
DATA(insert OID = 362 (  lseg_interpt	   PGNSP PGUID 12 f f f t f i 2 600 "601 601" 100 0 0 100  lseg_interpt - _null_ ));
DESCR("");
DATA(insert OID = 363 (  dist_ps		   PGNSP PGUID 12 f f f t f i 2 701 "600 601" 100 0 0 100  dist_ps - _null_ ));
DESCR("distance between");
DATA(insert OID = 364 (  dist_pb		   PGNSP PGUID 12 f f f t f i 2 701 "600 603" 100 0 0 100  dist_pb - _null_ ));
DESCR("distance between point and box");
DATA(insert OID = 365 (  dist_sb		   PGNSP PGUID 12 f f f t f i 2 701 "601 603" 100 0 0 100  dist_sb - _null_ ));
DESCR("distance between segment and box");
DATA(insert OID = 366 (  close_ps		   PGNSP PGUID 12 f f f t f i 2 600 "600 601" 100 0 0 100  close_ps - _null_ ));
DESCR("closest point on line segment");
DATA(insert OID = 367 (  close_pb		   PGNSP PGUID 12 f f f t f i 2 600 "600 603" 100 0 0 100  close_pb - _null_ ));
DESCR("closest point on box");
DATA(insert OID = 368 (  close_sb		   PGNSP PGUID 12 f f f t f i 2 600 "601 603" 100 0 0 100  close_sb - _null_ ));
DESCR("closest point to line segment on box");
DATA(insert OID = 369 (  on_ps			   PGNSP PGUID 12 f f f t f i 2 16 "600 601" 100 0 0 100  on_ps - _null_ ));
DESCR("point contained in segment");
DATA(insert OID = 370 (  path_distance	   PGNSP PGUID 12 f f f t f i 2 701 "602 602" 100 0 0 100  path_distance - _null_ ));
DESCR("distance between paths");
DATA(insert OID = 371 (  dist_ppath		   PGNSP PGUID 12 f f f t f i 2 701 "600 602" 100 0 0 100  dist_ppath - _null_ ));
DESCR("distance between point and path");
DATA(insert OID = 372 (  on_sb			   PGNSP PGUID 12 f f f t f i 2 16 "601 603" 100 0 0 100  on_sb - _null_ ));
DESCR("contained in");
DATA(insert OID = 373 (  inter_sb		   PGNSP PGUID 12 f f f t f i 2 16 "601 603" 100 0 0 100  inter_sb - _null_ ));
DESCR("intersects?");

/* OIDS 400 - 499 */

DATA(insert OID =  406 (  text			   PGNSP PGUID 12 f f t t f i 1 25 "19" 100 0 0 100 name_text - _null_ ));
DESCR("convert name to text");
DATA(insert OID =  407 (  name			   PGNSP PGUID 12 f f t t f i 1 19 "25" 100 0 0 100 text_name - _null_ ));
DESCR("convert text to name");
DATA(insert OID =  408 (  bpchar		   PGNSP PGUID 12 f f t t f i 1 1042 "19" 100 0 0 100 name_bpchar - _null_ ));
DESCR("convert name to char()");
DATA(insert OID =  409 (  name			   PGNSP PGUID 12 f f t t f i 1 19 "1042" 100 0 0 100  bpchar_name - _null_ ));
DESCR("convert char() to name");

DATA(insert OID = 440 (  hashgettuple	   PGNSP PGUID 12 f f f t f v 2 23 "0 0" 100 0 0 100  hashgettuple - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 441 (  hashinsert		   PGNSP PGUID 12 f f f t f v 5 23 "0 0 0 0 0" 100 0 0 100	hashinsert - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 443 (  hashbeginscan	   PGNSP PGUID 12 f f f t f v 4 23 "0 0 0 0" 100 0 0 100  hashbeginscan - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 444 (  hashrescan		   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	hashrescan - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 445 (  hashendscan	   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	hashendscan - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 446 (  hashmarkpos	   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	hashmarkpos - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 447 (  hashrestrpos	   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	hashrestrpos - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 448 (  hashbuild		   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	hashbuild - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 442 (  hashbulkdelete    PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	hashbulkdelete - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 438 (  hashcostestimate  PGNSP PGUID 12 f f f t f v 8 0 "0 0 0 0 0 0 0 0" 100 0 0 100  hashcostestimate - _null_ ));
DESCR("hash(internal)");

DATA(insert OID = 449 (  hashint2		   PGNSP PGUID 12 f f f t f i 1 23 "21" 100 0 0 100  hashint2 - _null_ ));
DESCR("hash");
DATA(insert OID = 450 (  hashint4		   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  hashint4 - _null_ ));
DESCR("hash");
DATA(insert OID = 949 (  hashint8		   PGNSP PGUID 12 f f f t f i 1 23 "20" 100 0 0 100  hashint8 - _null_ ));
DESCR("hash");
DATA(insert OID = 451 (  hashfloat4		   PGNSP PGUID 12 f f f t f i 1 23 "700" 100 0 0 100  hashfloat4 - _null_ ));
DESCR("hash");
DATA(insert OID = 452 (  hashfloat8		   PGNSP PGUID 12 f f f t f i 1 23 "701" 100 0 0 100  hashfloat8 - _null_ ));
DESCR("hash");
DATA(insert OID = 453 (  hashoid		   PGNSP PGUID 12 f f f t f i 1 23 "26" 100 0 0 100  hashoid - _null_ ));
DESCR("hash");
DATA(insert OID = 454 (  hashchar		   PGNSP PGUID 12 f f f t f i 1 23 "18" 100 0 0 100  hashchar - _null_ ));
DESCR("hash");
DATA(insert OID = 455 (  hashname		   PGNSP PGUID 12 f f f t f i 1 23 "19" 100 0 0 100  hashname - _null_ ));
DESCR("hash");
DATA(insert OID = 456 (  hashvarlena	   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	hashvarlena - _null_ ));
DESCR("hash any varlena type");
DATA(insert OID = 457 (  hashoidvector	   PGNSP PGUID 12 f f f t f i 1 23 "30" 100 0 0 100  hashoidvector - _null_ ));
DESCR("hash");
DATA(insert OID = 399 (  hashmacaddr	   PGNSP PGUID 12 f f f t f i 1 23 "829" 100 0 0 100  hashmacaddr - _null_ ));
DESCR("hash");
DATA(insert OID = 458 (  text_larger	   PGNSP PGUID 12 f f f t f i 2 25 "25 25" 100 0 0 100	text_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 459 (  text_smaller	   PGNSP PGUID 12 f f f t f i 2 25 "25 25" 100 0 0 100	text_smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 460 (  int8in			   PGNSP PGUID 12 f f f t f i 1 20 "0" 100 0 0 100	int8in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 461 (  int8out		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	int8out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 462 (  int8um			   PGNSP PGUID 12 f f f t f i 1 20 "20" 100 0 0 100  int8um - _null_ ));
DESCR("negate");
DATA(insert OID = 463 (  int8pl			   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8pl - _null_ ));
DESCR("add");
DATA(insert OID = 464 (  int8mi			   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 465 (  int8mul		   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 466 (  int8div		   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8div - _null_ ));
DESCR("divide");
DATA(insert OID = 467 (  int8eq			   PGNSP PGUID 12 f f f t f i 2 16 "20 20" 100 0 0 100	int8eq - _null_ ));
DESCR("equal");
DATA(insert OID = 468 (  int8ne			   PGNSP PGUID 12 f f f t f i 2 16 "20 20" 100 0 0 100	int8ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 469 (  int8lt			   PGNSP PGUID 12 f f f t f i 2 16 "20 20" 100 0 0 100	int8lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 470 (  int8gt			   PGNSP PGUID 12 f f f t f i 2 16 "20 20" 100 0 0 100	int8gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 471 (  int8le			   PGNSP PGUID 12 f f f t f i 2 16 "20 20" 100 0 0 100	int8le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 472 (  int8ge			   PGNSP PGUID 12 f f f t f i 2 16 "20 20" 100 0 0 100	int8ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 474 (  int84eq		   PGNSP PGUID 12 f f f t f i 2 16 "20 23" 100 0 0 100	int84eq - _null_ ));
DESCR("equal");
DATA(insert OID = 475 (  int84ne		   PGNSP PGUID 12 f f f t f i 2 16 "20 23" 100 0 0 100	int84ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 476 (  int84lt		   PGNSP PGUID 12 f f f t f i 2 16 "20 23" 100 0 0 100	int84lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 477 (  int84gt		   PGNSP PGUID 12 f f f t f i 2 16 "20 23" 100 0 0 100	int84gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 478 (  int84le		   PGNSP PGUID 12 f f f t f i 2 16 "20 23" 100 0 0 100	int84le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 479 (  int84ge		   PGNSP PGUID 12 f f f t f i 2 16 "20 23" 100 0 0 100	int84ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 480 (  int4			   PGNSP PGUID 12 f f t t f i 1  23 "20" 100 0 0 100  int84 - _null_ ));
DESCR("convert int8 to int4");
DATA(insert OID = 481 (  int8			   PGNSP PGUID 12 f f t t f i 1  20 "23" 100 0 0 100  int48 - _null_ ));
DESCR("convert int4 to int8");
DATA(insert OID = 482 (  float8			   PGNSP PGUID 12 f f t t f i 1 701 "20" 100 0 0 100  i8tod - _null_ ));
DESCR("convert int8 to float8");
DATA(insert OID = 483 (  int8			   PGNSP PGUID 12 f f f t f i 1  20 "701" 100 0 0 100  dtoi8 - _null_ ));
DESCR("convert float8 to int8");

DATA(insert OID = 714 (  int2			   PGNSP PGUID 12 f f t t f i 1  21 "20" 100 0 0 100  int82 - _null_ ));
DESCR("convert int8 to int2");
DATA(insert OID = 754 (  int8			   PGNSP PGUID 12 f f t t f i 1  20 "21" 100 0 0 100  int28 - _null_ ));
DESCR("convert int2 to int8");

/* OIDS 500 - 599 */

/* OIDS 600 - 699 */

DATA(insert OID = 1285 (  int4notin		   PGNSP PGUID 12 f f f t f s 2 16 "23 25" 100 0 0 100	int4notin - _null_ ));
DESCR("not in");
DATA(insert OID = 1286 (  oidnotin		   PGNSP PGUID 12 f f f t f s 2 16 "26 25" 100 0 0 100	oidnotin - _null_ ));
DESCR("not in");
DATA(insert OID = 1287 (  int44in		   PGNSP PGUID 12 f f f t f i 1 22 "0" 100 0 0 100	int44in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 653 (  int44out		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	int44out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 655 (  namelt			   PGNSP PGUID 12 f f f t f i 2 16 "19 19" 100 0 0 100	namelt - _null_ ));
DESCR("less-than");
DATA(insert OID = 656 (  namele			   PGNSP PGUID 12 f f f t f i 2 16 "19 19" 100 0 0 100	namele - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 657 (  namegt			   PGNSP PGUID 12 f f f t f i 2 16 "19 19" 100 0 0 100	namegt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 658 (  namege			   PGNSP PGUID 12 f f f t f i 2 16 "19 19" 100 0 0 100	namege - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 659 (  namene			   PGNSP PGUID 12 f f f t f i 2 16 "19 19" 100 0 0 100	namene - _null_ ));
DESCR("not equal");

DATA(insert OID = 668 (  bpchar			   PGNSP PGUID 12 f f t t f i 2 1042 "1042 23" 100 0 0 100	bpchar - _null_ ));
DESCR("adjust char() to typmod length");
DATA(insert OID = 669 (  varchar		   PGNSP PGUID 12 f f t t f i 2 1043 "1043 23" 100 0 0 100	varchar - _null_ ));
DESCR("adjust varchar() to typmod length");

DATA(insert OID = 676 (  mktinterval	   PGNSP PGUID 12 f f f t f i 2 704 "702 702" 100 0 0 100 mktinterval - _null_ ));
DESCR("convert to tinterval");
DATA(insert OID = 619 (  oidvectorne	   PGNSP PGUID 12 f f f t f i 2 16 "30 30" 100 0 0 100	oidvectorne - _null_ ));
DESCR("not equal");
DATA(insert OID = 677 (  oidvectorlt	   PGNSP PGUID 12 f f f t f i 2 16 "30 30" 100 0 0 100	oidvectorlt - _null_ ));
DESCR("less-than");
DATA(insert OID = 678 (  oidvectorle	   PGNSP PGUID 12 f f f t f i 2 16 "30 30" 100 0 0 100	oidvectorle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 679 (  oidvectoreq	   PGNSP PGUID 12 f f f t f i 2 16 "30 30" 100 0 0 100	oidvectoreq - _null_ ));
DESCR("equal");
DATA(insert OID = 680 (  oidvectorge	   PGNSP PGUID 12 f f f t f i 2 16 "30 30" 100 0 0 100	oidvectorge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 681 (  oidvectorgt	   PGNSP PGUID 12 f f f t f i 2 16 "30 30" 100 0 0 100	oidvectorgt - _null_ ));
DESCR("greater-than");

/* OIDS 700 - 799 */
DATA(insert OID = 710 (  getpgusername	   PGNSP PGUID 12 f f f t f s 0 19 "0" 100 0 0 100	current_user - _null_ ));
DESCR("deprecated -- use current_user");
DATA(insert OID = 711 (  userfntest		   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  userfntest - _null_ ));
DESCR("");
DATA(insert OID = 713 (  oidrand		   PGNSP PGUID 12 f f f t f v 2 16 "26 23" 100 0 0 100	oidrand - _null_ ));
DESCR("random");
DATA(insert OID = 715 (  oidsrand		   PGNSP PGUID 12 f f f t f v 1 16 "23" 100 0 0 100  oidsrand - _null_ ));
DESCR("seed random number generator");
DATA(insert OID = 716 (  oidlt			   PGNSP PGUID 12 f f f t f i 2 16 "26 26" 100 0 0 100	oidlt - _null_ ));
DESCR("less-than");
DATA(insert OID = 717 (  oidle			   PGNSP PGUID 12 f f f t f i 2 16 "26 26" 100 0 0 100	oidle - _null_ ));
DESCR("less-than-or-equal");

DATA(insert OID = 720 (  octet_length	   PGNSP PGUID 12 f f f t f i 1 23 "17" 100 0 0 100  byteaoctetlen - _null_ ));
DESCR("octet length");
DATA(insert OID = 721 (  get_byte		   PGNSP PGUID 12 f f f t f i 2 23 "17 23" 100 0 0 100	byteaGetByte - _null_ ));
DESCR("");
DATA(insert OID = 722 (  set_byte		   PGNSP PGUID 12 f f f t f i 3 17 "17 23 23" 100 0 0 100  byteaSetByte - _null_ ));
DESCR("");
DATA(insert OID = 723 (  get_bit		   PGNSP PGUID 12 f f f t f i 2 23 "17 23" 100 0 0 100	byteaGetBit - _null_ ));
DESCR("");
DATA(insert OID = 724 (  set_bit		   PGNSP PGUID 12 f f f t f i 3 17 "17 23 23" 100 0 0 100  byteaSetBit - _null_ ));
DESCR("");

DATA(insert OID = 725 (  dist_pl		   PGNSP PGUID 12 f f f t f i 2 701 "600 628" 100 0 0 100  dist_pl - _null_ ));
DESCR("distance between point and line");
DATA(insert OID = 726 (  dist_lb		   PGNSP PGUID 12 f f f t f i 2 701 "628 603" 100 0 0 100  dist_lb - _null_ ));
DESCR("distance between line and box");
DATA(insert OID = 727 (  dist_sl		   PGNSP PGUID 12 f f f t f i 2 701 "601 628" 100 0 0 100  dist_sl - _null_ ));
DESCR("distance between lseg and line");
DATA(insert OID = 728 (  dist_cpoly		   PGNSP PGUID 12 f f f t f i 2 701 "718 604" 100 0 0 100  dist_cpoly - _null_ ));
DESCR("distance between");
DATA(insert OID = 729 (  poly_distance	   PGNSP PGUID 12 f f f t f i 2 701 "604 604" 100 0 0 100  poly_distance - _null_ ));
DESCR("distance between");

DATA(insert OID = 740 (  text_lt		   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	text_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 741 (  text_le		   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	text_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 742 (  text_gt		   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	text_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 743 (  text_ge		   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	text_ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 744 (  array_eq		   PGNSP PGUID 12 f f f t f i 2 16 "0 0" 100 0 0 100 array_eq - _null_ ));
DESCR("array equal");

DATA(insert OID = 745 (  current_user	   PGNSP PGUID 12 f f f t f s 0 19 "0" 100 0 0 100	current_user - _null_ ));
DESCR("current user name");
DATA(insert OID = 746 (  session_user	   PGNSP PGUID 12 f f f t f s 0 19 "0" 100 0 0 100	session_user - _null_ ));
DESCR("session user name");

DATA(insert OID = 747 (  array_dims		   PGNSP PGUID 12 f f f t f i 1 25 "0" 100 0 0 100 array_dims - _null_ ));
DESCR("array dimensions");
DATA(insert OID = 750 (  array_in		   PGNSP PGUID 12 f f f t f i 3 23 "0 26 23" 100 0 0 100  array_in - _null_ ));
DESCR("array");
DATA(insert OID = 751 (  array_out		   PGNSP PGUID 12 f f f t f i 2 23 "0 26" 100 0 0 100  array_out - _null_ ));
DESCR("array");

DATA(insert OID = 760 (  smgrin			   PGNSP PGUID 12 f f f t f s 1 210 "0" 100 0 0 100  smgrin - _null_ ));
DESCR("storage manager(internal)");
DATA(insert OID = 761 (  smgrout		   PGNSP PGUID 12 f f f t f s 1 23	"0" 100 0 0 100  smgrout - _null_ ));
DESCR("storage manager(internal)");
DATA(insert OID = 762 (  smgreq			   PGNSP PGUID 12 f f f t f i 2 16 "210 210" 100 0 0 100  smgreq - _null_ ));
DESCR("storage manager");
DATA(insert OID = 763 (  smgrne			   PGNSP PGUID 12 f f f t f i 2 16 "210 210" 100 0 0 100  smgrne - _null_ ));
DESCR("storage manager");

DATA(insert OID = 764 (  lo_import		   PGNSP PGUID 12 f f f t f v 1 26 "25" 100 0 0 100  lo_import - _null_ ));
DESCR("large object import");
DATA(insert OID = 765 (  lo_export		   PGNSP PGUID 12 f f f t f v 2 23 "26 25" 100 0 0 100	lo_export - _null_ ));
DESCR("large object export");

DATA(insert OID = 766 (  int4inc		   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  int4inc - _null_ ));
DESCR("increment");
DATA(insert OID = 768 (  int4larger		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 769 (  int4smaller	   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 770 (  int2larger		   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 771 (  int2smaller	   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 774 (  gistgettuple	   PGNSP PGUID 12 f f f t f v 2 23 "0 0" 100 0 0 100  gistgettuple - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 775 (  gistinsert		   PGNSP PGUID 12 f f f t f v 5 23 "0 0 0 0 0" 100 0 0 100	gistinsert - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 777 (  gistbeginscan	   PGNSP PGUID 12 f f f t f v 4 23 "0 0 0 0" 100 0 0 100  gistbeginscan - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 778 (  gistrescan		   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	gistrescan - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 779 (  gistendscan	   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	gistendscan - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 780 (  gistmarkpos	   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	gistmarkpos - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 781 (  gistrestrpos	   PGNSP PGUID 12 f f f t f v 1 23 "0" 100 0 0 100	gistrestrpos - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 782 (  gistbuild		   PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	gistbuild - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 776 (  gistbulkdelete    PGNSP PGUID 12 f f f t f v 3 23 "0 0 0" 100 0 0 100	gistbulkdelete - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 772 (  gistcostestimate  PGNSP PGUID 12 f f f t f v 8 0 "0 0 0 0 0 0 0 0" 100 0 0 100  gistcostestimate - _null_ ));
DESCR("gist(internal)");

DATA(insert OID = 784 (  tintervaleq	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervaleq - _null_ ));
DESCR("equal");
DATA(insert OID = 785 (  tintervalne	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervalne - _null_ ));
DESCR("not equal");
DATA(insert OID = 786 (  tintervallt	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervallt - _null_ ));
DESCR("less-than");
DATA(insert OID = 787 (  tintervalgt	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervalgt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 788 (  tintervalle	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervalle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 789 (  tintervalge	   PGNSP PGUID 12 f f f t f i 2 16 "704 704" 100 0 0 100  tintervalge - _null_ ));
DESCR("greater-than-or-equal");

/* OIDS 800 - 899 */

DATA(insert OID = 817 (  oid			   PGNSP PGUID 12 f f f t f i 1 26 "25" 100 0 0 100  text_oid - _null_ ));
DESCR("convert text to oid");
DATA(insert OID = 818 (  int2			   PGNSP PGUID 12 f f f t f i 1 21 "25" 100 0 0 100  text_int2 - _null_ ));
DESCR("convert text to int2");
DATA(insert OID = 819 (  int4			   PGNSP PGUID 12 f f f t f i 1 23 "25" 100 0 0 100  text_int4 - _null_ ));
DESCR("convert text to int4");

DATA(insert OID = 838 (  float8			   PGNSP PGUID 12 f f f t f i 1 701 "25" 100 0 0 100  text_float8 - _null_ ));
DESCR("convert text to float8");
DATA(insert OID = 839 (  float4			   PGNSP PGUID 12 f f f t f i 1 700 "25" 100 0 0 100  text_float4 - _null_ ));
DESCR("convert text to float4");
DATA(insert OID = 840 (  text			   PGNSP PGUID 12 f f t t f i 1  25 "701" 100 0 0 100  float8_text - _null_ ));
DESCR("convert float8 to text");
DATA(insert OID = 841 (  text			   PGNSP PGUID 12 f f t t f i 1  25 "700" 100 0 0 100  float4_text - _null_ ));
DESCR("convert float4 to text");

DATA(insert OID =  846 (  cash_mul_flt4    PGNSP PGUID 12 f f f t f i 2 790 "790 700" 100 0 0 100  cash_mul_flt4 - _null_ ));
DESCR("multiply");
DATA(insert OID =  847 (  cash_div_flt4    PGNSP PGUID 12 f f f t f i 2 790 "790 700" 100 0 0 100  cash_div_flt4 - _null_ ));
DESCR("divide");
DATA(insert OID =  848 (  flt4_mul_cash    PGNSP PGUID 12 f f f t f i 2 790 "700 790" 100 0 0 100  flt4_mul_cash - _null_ ));
DESCR("multiply");

DATA(insert OID =  849 (  position		   PGNSP PGUID 12 f f f t f i 2 23 "25 25" 100 0 0 100 textpos - _null_ ));
DESCR("return position of substring");
DATA(insert OID =  850 (  textlike		   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100 textlike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID =  851 (  textnlike		   PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100 textnlike - _null_ ));
DESCR("does not match LIKE expression");

DATA(insert OID =  852 (  int48eq		   PGNSP PGUID 12 f f f t f i 2 16 "23 20" 100 0 0 100	int48eq - _null_ ));
DESCR("equal");
DATA(insert OID =  853 (  int48ne		   PGNSP PGUID 12 f f f t f i 2 16 "23 20" 100 0 0 100	int48ne - _null_ ));
DESCR("not equal");
DATA(insert OID =  854 (  int48lt		   PGNSP PGUID 12 f f f t f i 2 16 "23 20" 100 0 0 100	int48lt - _null_ ));
DESCR("less-than");
DATA(insert OID =  855 (  int48gt		   PGNSP PGUID 12 f f f t f i 2 16 "23 20" 100 0 0 100	int48gt - _null_ ));
DESCR("greater-than");
DATA(insert OID =  856 (  int48le		   PGNSP PGUID 12 f f f t f i 2 16 "23 20" 100 0 0 100	int48le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID =  857 (  int48ge		   PGNSP PGUID 12 f f f t f i 2 16 "23 20" 100 0 0 100	int48ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID =  858 (  namelike		   PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100	namelike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID =  859 (  namenlike		   PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100	namenlike - _null_ ));
DESCR("does not match LIKE expression");

DATA(insert OID =  860 (  bpchar		   PGNSP PGUID 12 f f t t f i 1 1042 "18" 100 0 0 100  char_bpchar - _null_ ));
DESCR("convert char to char()");

DATA(insert OID =  862 (  int4_mul_cash		   PGNSP PGUID 12 f f f t f i 2 790 "23 790" 100 0 0 100  int4_mul_cash - _null_ ));
DESCR("multiply");
DATA(insert OID =  863 (  int2_mul_cash		   PGNSP PGUID 12 f f f t f i 2 790 "21 790" 100 0 0 100  int2_mul_cash - _null_ ));
DESCR("multiply");
DATA(insert OID =  864 (  cash_mul_int4		   PGNSP PGUID 12 f f f t f i 2 790 "790 23" 100 0 0 100  cash_mul_int4 - _null_ ));
DESCR("multiply");
DATA(insert OID =  865 (  cash_div_int4		   PGNSP PGUID 12 f f f t f i 2 790 "790 23" 100 0 0 100  cash_div_int4 - _null_ ));
DESCR("divide");
DATA(insert OID =  866 (  cash_mul_int2		   PGNSP PGUID 12 f f f t f i 2 790 "790 21" 100 0 0 100  cash_mul_int2 - _null_ ));
DESCR("multiply");
DATA(insert OID =  867 (  cash_div_int2		   PGNSP PGUID 12 f f f t f i 2 790 "790 21" 100 0 0 100  cash_div_int2 - _null_ ));
DESCR("divide");

DATA(insert OID =  886 (  cash_in		   PGNSP PGUID 12 f f f t f i 1 790 "0" 100 0 0 100  cash_in - _null_ ));
DESCR("(internal)");
DATA(insert OID =  887 (  cash_out		   PGNSP PGUID 12 f f f t f i 1  23 "0" 100 0 0 100  cash_out - _null_ ));
DESCR("(internal)");
DATA(insert OID =  888 (  cash_eq		   PGNSP PGUID 12 f f f t f i 2  16 "790 790" 100 0 0 100  cash_eq - _null_ ));
DESCR("equal");
DATA(insert OID =  889 (  cash_ne		   PGNSP PGUID 12 f f f t f i 2  16 "790 790" 100 0 0 100  cash_ne - _null_ ));
DESCR("not equal");
DATA(insert OID =  890 (  cash_lt		   PGNSP PGUID 12 f f f t f i 2  16 "790 790" 100 0 0 100  cash_lt - _null_ ));
DESCR("less-than");
DATA(insert OID =  891 (  cash_le		   PGNSP PGUID 12 f f f t f i 2  16 "790 790" 100 0 0 100  cash_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID =  892 (  cash_gt		   PGNSP PGUID 12 f f f t f i 2  16 "790 790" 100 0 0 100  cash_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID =  893 (  cash_ge		   PGNSP PGUID 12 f f f t f i 2  16 "790 790" 100 0 0 100  cash_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID =  894 (  cash_pl		   PGNSP PGUID 12 f f f t f i 2 790 "790 790" 100 0 0 100  cash_pl - _null_ ));
DESCR("add");
DATA(insert OID =  895 (  cash_mi		   PGNSP PGUID 12 f f f t f i 2 790 "790 790" 100 0 0 100  cash_mi - _null_ ));
DESCR("subtract");
DATA(insert OID =  896 (  cash_mul_flt8    PGNSP PGUID 12 f f f t f i 2 790 "790 701" 100 0 0 100  cash_mul_flt8 - _null_ ));
DESCR("multiply");
DATA(insert OID =  897 (  cash_div_flt8    PGNSP PGUID 12 f f f t f i 2 790 "790 701" 100 0 0 100  cash_div_flt8 - _null_ ));
DESCR("divide");
DATA(insert OID =  898 (  cashlarger	   PGNSP PGUID 12 f f f t f i 2 790 "790 790" 100 0 0 100  cashlarger - _null_ ));
DESCR("larger of two");
DATA(insert OID =  899 (  cashsmaller	   PGNSP PGUID 12 f f f t f i 2 790 "790 790" 100 0 0 100  cashsmaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID =  919 (  flt8_mul_cash    PGNSP PGUID 12 f f f t f i 2 790 "701 790" 100 0 0 100  flt8_mul_cash - _null_ ));
DESCR("multiply");
DATA(insert OID =  935 (  cash_words	   PGNSP PGUID 12 f f f t f i 1  25 "790" 100 0 0 100  cash_words - _null_ ));
DESCR("output amount as words");

/* OIDS 900 - 999 */

DATA(insert OID = 940 (  mod			   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 941 (  mod			   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 942 (  mod			   PGNSP PGUID 12 f f f t f i 2 23 "21 23" 100 0 0 100	int24mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 943 (  mod			   PGNSP PGUID 12 f f f t f i 2 23 "23 21" 100 0 0 100	int42mod - _null_ ));
DESCR("modulus");

DATA(insert OID = 945 (  int8mod		   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 947 (  mod			   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8mod - _null_ ));
DESCR("modulus");

DATA(insert OID = 944 (  char			   PGNSP PGUID 12 f f t t f i 1 18 "25" 100 0 0 100  text_char - _null_ ));
DESCR("convert text to char");
DATA(insert OID = 946 (  text			   PGNSP PGUID 12 f f t t f i 1 25 "18" 100 0 0 100  char_text - _null_ ));
DESCR("convert char to text");

DATA(insert OID = 950 (  istrue			   PGNSP PGUID 12 f f f f f i 1 16 "16" 100 0 0 100  istrue - _null_ ));
DESCR("bool is true (not false or unknown)");
DATA(insert OID = 951 (  isfalse		   PGNSP PGUID 12 f f f f f i 1 16 "16" 100 0 0 100  isfalse - _null_ ));
DESCR("bool is false (not true or unknown)");

DATA(insert OID = 952 (  lo_open		   PGNSP PGUID 12 f f f t f v 2 23 "26 23" 100 0 0 100	lo_open - _null_ ));
DESCR("large object open");
DATA(insert OID = 953 (  lo_close		   PGNSP PGUID 12 f f f t f v 1 23 "23" 100 0 0 100  lo_close - _null_ ));
DESCR("large object close");
DATA(insert OID = 954 (  loread			   PGNSP PGUID 12 f f f t f v 2 17 "23 23" 100 0 0 100	loread - _null_ ));
DESCR("large object read");
DATA(insert OID = 955 (  lowrite		   PGNSP PGUID 12 f f f t f v 2 23 "23 17" 100 0 0 100	lowrite - _null_ ));
DESCR("large object write");
DATA(insert OID = 956 (  lo_lseek		   PGNSP PGUID 12 f f f t f v 3 23 "23 23 23" 100 0 0 100  lo_lseek - _null_ ));
DESCR("large object seek");
DATA(insert OID = 957 (  lo_creat		   PGNSP PGUID 12 f f f t f v 1 26 "23" 100 0 0 100  lo_creat - _null_ ));
DESCR("large object create");
DATA(insert OID = 958 (  lo_tell		   PGNSP PGUID 12 f f f t f v 1 23 "23" 100 0 0 100  lo_tell - _null_ ));
DESCR("large object position");

DATA(insert OID = 959 (  on_pl			   PGNSP PGUID 12 f f f t f i 2  16 "600 628" 100 0 0 100  on_pl - _null_ ));
DESCR("point on line?");
DATA(insert OID = 960 (  on_sl			   PGNSP PGUID 12 f f f t f i 2  16 "601 628" 100 0 0 100  on_sl - _null_ ));
DESCR("lseg on line?");
DATA(insert OID = 961 (  close_pl		   PGNSP PGUID 12 f f f t f i 2 600 "600 628" 100 0 0 100  close_pl - _null_ ));
DESCR("closest point on line");
DATA(insert OID = 962 (  close_sl		   PGNSP PGUID 12 f f f t f i 2 600 "601 628" 100 0 0 100  close_sl - _null_ ));
DESCR("closest point to line segment on line");
DATA(insert OID = 963 (  close_lb		   PGNSP PGUID 12 f f f t f i 2 600 "628 603" 100 0 0 100  close_lb - _null_ ));
DESCR("closest point to line on box");

DATA(insert OID = 964 (  lo_unlink		   PGNSP PGUID 12 f f f t f v 1  23 "26" 100 0 0 100  lo_unlink - _null_ ));
DESCR("large object unlink(delete)");

DATA(insert OID = 973 (  path_inter		   PGNSP PGUID 12 f f f t f i 2  16 "602 602" 100 0 0 100  path_inter - _null_ ));
DESCR("paths intersect?");
DATA(insert OID = 975 (  area			   PGNSP PGUID 12 f f f t f i 1 701 "603" 100 0 0 100  box_area - _null_ ));
DESCR("box area");
DATA(insert OID = 976 (  width			   PGNSP PGUID 12 f f f t f i 1 701 "603" 100 0 0 100  box_width - _null_ ));
DESCR("box width");
DATA(insert OID = 977 (  height			   PGNSP PGUID 12 f f f t f i 1 701 "603" 100 0 0 100  box_height - _null_ ));
DESCR("box height");
DATA(insert OID = 978 (  box_distance	   PGNSP PGUID 12 f f f t f i 2 701 "603 603" 100 0 0 100  box_distance - _null_ ));
DESCR("distance between boxes");
DATA(insert OID = 980 (  box_intersect	   PGNSP PGUID 12 f f f t f i 2 603 "603 603" 100 0 0 100  box_intersect - _null_ ));
DESCR("box intersection (another box)");
DATA(insert OID = 981 (  diagonal		   PGNSP PGUID 12 f f f t f i 1 601 "603" 100 0 0 100  box_diagonal - _null_ ));
DESCR("box diagonal");
DATA(insert OID = 982 (  path_n_lt		   PGNSP PGUID 12 f f f t f i 2 16 "602 602" 100 0 0 100  path_n_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 983 (  path_n_gt		   PGNSP PGUID 12 f f f t f i 2 16 "602 602" 100 0 0 100  path_n_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 984 (  path_n_eq		   PGNSP PGUID 12 f f f t f i 2 16 "602 602" 100 0 0 100  path_n_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 985 (  path_n_le		   PGNSP PGUID 12 f f f t f i 2 16 "602 602" 100 0 0 100  path_n_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 986 (  path_n_ge		   PGNSP PGUID 12 f f f t f i 2 16 "602 602" 100 0 0 100  path_n_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 987 (  path_length	   PGNSP PGUID 12 f f f t f i 1 701 "602" 100 0 0 100  path_length - _null_ ));
DESCR("sum of path segments");
DATA(insert OID = 988 (  point_ne		   PGNSP PGUID 12 f f f t f i 2 16 "600 600" 100 0 0 100  point_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 989 (  point_vert		   PGNSP PGUID 12 f f f t f i 2 16 "600 600" 100 0 0 100  point_vert - _null_ ));
DESCR("vertically aligned?");
DATA(insert OID = 990 (  point_horiz	   PGNSP PGUID 12 f f f t f i 2 16 "600 600" 100 0 0 100  point_horiz - _null_ ));
DESCR("horizontally aligned?");
DATA(insert OID = 991 (  point_distance    PGNSP PGUID 12 f f f t f i 2 701 "600 600" 100 0 0 100  point_distance - _null_ ));
DESCR("distance between");
DATA(insert OID = 992 (  slope			   PGNSP PGUID 12 f f f t f i 2 701 "600 600" 100 0 0 100  point_slope - _null_ ));
DESCR("slope between points");
DATA(insert OID = 993 (  lseg			   PGNSP PGUID 12 f f f t f i 2 601 "600 600" 100 0 0 100  lseg_construct - _null_ ));
DESCR("convert points to line segment");
DATA(insert OID = 994 (  lseg_intersect    PGNSP PGUID 12 f f f t f i 2 16 "601 601" 100 0 0 100  lseg_intersect - _null_ ));
DESCR("intersect?");
DATA(insert OID = 995 (  lseg_parallel	   PGNSP PGUID 12 f f f t f i 2 16 "601 601" 100 0 0 100  lseg_parallel - _null_ ));
DESCR("parallel?");
DATA(insert OID = 996 (  lseg_perp		   PGNSP PGUID 12 f f f t f i 2 16 "601 601" 100 0 0 100  lseg_perp - _null_ ));
DESCR("perpendicular?");
DATA(insert OID = 997 (  lseg_vertical	   PGNSP PGUID 12 f f f t f i 1 16 "601" 100 0 0 100  lseg_vertical - _null_ ));
DESCR("vertical?");
DATA(insert OID = 998 (  lseg_horizontal   PGNSP PGUID 12 f f f t f i 1 16 "601" 100 0 0 100  lseg_horizontal - _null_ ));
DESCR("horizontal?");
DATA(insert OID = 999 (  lseg_eq		   PGNSP PGUID 12 f f f t f i 2 16 "601 601" 100 0 0 100  lseg_eq - _null_ ));
DESCR("equal");

DATA(insert OID =  748 (  date			   PGNSP PGUID 12 f f f t f s 1 1082 "25" 100 0 0 100 text_date - _null_ ));
DESCR("convert text to date");
DATA(insert OID =  749 (  text			   PGNSP PGUID 12 f f t t f s 1 25 "1082" 100 0 0 100 date_text - _null_ ));
DESCR("convert date to text");
DATA(insert OID =  837 (  time			   PGNSP PGUID 12 f f f t f s 1 1083 "25" 100 0 0 100 text_time - _null_ ));
DESCR("convert text to time");
DATA(insert OID =  948 (  text			   PGNSP PGUID 12 f f t t f i 1 25 "1083" 100 0 0 100 time_text - _null_ ));
DESCR("convert time to text");
DATA(insert OID =  938 (  timetz		   PGNSP PGUID 12 f f f t f s 1 1266 "25" 100 0 0 100 text_timetz - _null_ ));
DESCR("convert text to timetz");
DATA(insert OID =  939 (  text			   PGNSP PGUID 12 f f t t f i 1 25 "1266" 100 0 0 100 timetz_text - _null_ ));
DESCR("convert timetz to text");

/* OIDS 1000 - 1999 */

DATA(insert OID = 1026 (  timezone		   PGNSP PGUID 12 f f f t f s 2 1186 "1186 1184" 100 0 0 100	timestamptz_izone - _null_ ));
DESCR("time zone");

DATA(insert OID = 1029 (  nullvalue		   PGNSP PGUID 12 f f f f f i 1 16 "0" 100 0 0 100	nullvalue - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1030 (  nonnullvalue	   PGNSP PGUID 12 f f f f f i 1 16 "0" 100 0 0 100	nonnullvalue - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1031 (  aclitemin		   PGNSP PGUID 12 f f f t f s 1 1033 "0" 100 0 0 100  aclitemin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1032 (  aclitemout	   PGNSP PGUID 12 f f f t f s 1 23 "1033" 100 0 0 100  aclitemout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1035 (  aclinsert		   PGNSP PGUID 12 f f f t f s 2 1034 "1034 1033" 100 0 0 100  aclinsert - _null_ ));
DESCR("add/update ACL item");
DATA(insert OID = 1036 (  aclremove		   PGNSP PGUID 12 f f f t f s 2 1034 "1034 1033" 100 0 0 100  aclremove - _null_ ));
DESCR("remove ACL item");
DATA(insert OID = 1037 (  aclcontains	   PGNSP PGUID 12 f f f t f s 2 16 "1034 1033" 100 0 0 100	aclcontains - _null_ ));
DESCR("does ACL contain item?");
DATA(insert OID = 1038 (  seteval		   PGNSP PGUID 12 f f f t t v 1 23 "26" 100 0 0 100  seteval - _null_ ));
DESCR("internal function supporting PostQuel-style sets");
DATA(insert OID = 1044 (  bpcharin		   PGNSP PGUID 12 f f f t f i 3 1042 "0 26 23" 100 0 0 100 bpcharin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1045 (  bpcharout		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	bpcharout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1046 (  varcharin		   PGNSP PGUID 12 f f f t f i 3 1043 "0 26 23" 100 0 0 100 varcharin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1047 (  varcharout	   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	varcharout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1048 (  bpchareq		   PGNSP PGUID 12 f f f t f i 2 16 "1042 1042" 100 0 0 100	bpchareq - _null_ ));
DESCR("equal");
DATA(insert OID = 1049 (  bpcharlt		   PGNSP PGUID 12 f f f t f i 2 16 "1042 1042" 100 0 0 100	bpcharlt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1050 (  bpcharle		   PGNSP PGUID 12 f f f t f i 2 16 "1042 1042" 100 0 0 100	bpcharle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1051 (  bpchargt		   PGNSP PGUID 12 f f f t f i 2 16 "1042 1042" 100 0 0 100	bpchargt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1052 (  bpcharge		   PGNSP PGUID 12 f f f t f i 2 16 "1042 1042" 100 0 0 100	bpcharge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1053 (  bpcharne		   PGNSP PGUID 12 f f f t f i 2 16 "1042 1042" 100 0 0 100	bpcharne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1070 (  varchareq		   PGNSP PGUID 12 f f f t f i 2 16 "1043 1043" 100 0 0 100	varchareq - _null_ ));
DESCR("equal");
DATA(insert OID = 1071 (  varcharlt		   PGNSP PGUID 12 f f f t f i 2 16 "1043 1043" 100 0 0 100	varcharlt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1072 (  varcharle		   PGNSP PGUID 12 f f f t f i 2 16 "1043 1043" 100 0 0 100	varcharle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1073 (  varchargt		   PGNSP PGUID 12 f f f t f i 2 16 "1043 1043" 100 0 0 100	varchargt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1074 (  varcharge		   PGNSP PGUID 12 f f f t f i 2 16 "1043 1043" 100 0 0 100	varcharge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1075 (  varcharne		   PGNSP PGUID 12 f f f t f i 2 16 "1043 1043" 100 0 0 100	varcharne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1078 (  bpcharcmp		   PGNSP PGUID 12 f f f t f i 2 23 "1042 1042" 100 0 0 100	bpcharcmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1079 (  varcharcmp	   PGNSP PGUID 12 f f f t f i 2 23 "1043 1043" 100 0 0 100	varcharcmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1080 (  hashbpchar	   PGNSP PGUID 12 f f f t f i 1 23 "1042" 100 0 0 100  hashbpchar - _null_ ));
DESCR("hash");
DATA(insert OID = 1081 (  format_type	   PGNSP PGUID 12 f f f f f s 2 25 "26 23" 100 0 0 100 format_type - _null_ ));
DESCR("format a type oid and atttypmod to canonical SQL");
DATA(insert OID = 1084 (  date_in		   PGNSP PGUID 12 f f f t f s 1 1082 "0" 100 0 0 100  date_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1085 (  date_out		   PGNSP PGUID 12 f f f t f s 1 23 "0" 100 0 0 100	date_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1086 (  date_eq		   PGNSP PGUID 12 f f f t f i 2 16 "1082 1082" 100 0 0 100	date_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1087 (  date_lt		   PGNSP PGUID 12 f f f t f i 2 16 "1082 1082" 100 0 0 100	date_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1088 (  date_le		   PGNSP PGUID 12 f f f t f i 2 16 "1082 1082" 100 0 0 100	date_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1089 (  date_gt		   PGNSP PGUID 12 f f f t f i 2 16 "1082 1082" 100 0 0 100	date_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1090 (  date_ge		   PGNSP PGUID 12 f f f t f i 2 16 "1082 1082" 100 0 0 100	date_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1091 (  date_ne		   PGNSP PGUID 12 f f f t f i 2 16 "1082 1082" 100 0 0 100	date_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1092 (  date_cmp		   PGNSP PGUID 12 f f f t f i 2 23 "1082 1082" 100 0 0 100	date_cmp - _null_ ));
DESCR("less-equal-greater");

/* OIDS 1100 - 1199 */

DATA(insert OID = 1102 (  time_lt		   PGNSP PGUID 12 f f f t f i 2 16 "1083 1083" 100 0 0 100	time_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1103 (  time_le		   PGNSP PGUID 12 f f f t f i 2 16 "1083 1083" 100 0 0 100	time_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1104 (  time_gt		   PGNSP PGUID 12 f f f t f i 2 16 "1083 1083" 100 0 0 100	time_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1105 (  time_ge		   PGNSP PGUID 12 f f f t f i 2 16 "1083 1083" 100 0 0 100	time_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1106 (  time_ne		   PGNSP PGUID 12 f f f t f i 2 16 "1083 1083" 100 0 0 100	time_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1107 (  time_cmp		   PGNSP PGUID 12 f f f t f i 2 23 "1083 1083" 100 0 0 100	time_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1138 (  date_larger	   PGNSP PGUID 12 f f f t f i 2 1082 "1082 1082" 100 0 0 100  date_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1139 (  date_smaller	   PGNSP PGUID 12 f f f t f i 2 1082 "1082 1082" 100 0 0 100  date_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 1140 (  date_mi		   PGNSP PGUID 12 f f f t f i 2 23 "1082 1082" 100 0 0 100	date_mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1141 (  date_pli		   PGNSP PGUID 12 f f f t f i 2 1082 "1082 23" 100 0 0 100	date_pli - _null_ ));
DESCR("add");
DATA(insert OID = 1142 (  date_mii		   PGNSP PGUID 12 f f f t f i 2 1082 "1082 23" 100 0 0 100	date_mii - _null_ ));
DESCR("subtract");
DATA(insert OID = 1143 (  time_in		   PGNSP PGUID 12 f f f t f s 1 1083 "0" 100 0 0 100  time_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1144 (  time_out		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	time_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1145 (  time_eq		   PGNSP PGUID 12 f f f t f i 2 16 "1083 1083" 100 0 0 100	time_eq - _null_ ));
DESCR("equal");

DATA(insert OID = 1146 (  circle_add_pt    PGNSP PGUID 12 f f f t f i 2 718 "718 600" 100 0 0 100  circle_add_pt - _null_ ));
DESCR("add");
DATA(insert OID = 1147 (  circle_sub_pt    PGNSP PGUID 12 f f f t f i 2 718 "718 600" 100 0 0 100  circle_sub_pt - _null_ ));
DESCR("subtract");
DATA(insert OID = 1148 (  circle_mul_pt    PGNSP PGUID 12 f f f t f i 2 718 "718 600" 100 0 0 100  circle_mul_pt - _null_ ));
DESCR("multiply");
DATA(insert OID = 1149 (  circle_div_pt    PGNSP PGUID 12 f f f t f i 2 718 "718 600" 100 0 0 100  circle_div_pt - _null_ ));
DESCR("divide");

DATA(insert OID = 1150 (  timestamptz_in   PGNSP PGUID 12 f f f t f s 1 1184 "0" 100 0 0 100  timestamptz_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1151 (  timestamptz_out  PGNSP PGUID 12 f f f t f s 1 23 "0" 100 0 0 100	timestamptz_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1152 (  timestamptz_eq   PGNSP PGUID 12 f f f t f i 2 16 "1184 1184" 100 0 0 100	timestamp_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1153 (  timestamptz_ne   PGNSP PGUID 12 f f f t f i 2 16 "1184 1184" 100 0 0 100	timestamp_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1154 (  timestamptz_lt   PGNSP PGUID 12 f f f t f i 2 16 "1184 1184" 100 0 0 100	timestamp_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1155 (  timestamptz_le   PGNSP PGUID 12 f f f t f i 2 16 "1184 1184" 100 0 0 100	timestamp_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1156 (  timestamptz_ge   PGNSP PGUID 12 f f f t f i 2 16 "1184 1184" 100 0 0 100	timestamp_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1157 (  timestamptz_gt   PGNSP PGUID 12 f f f t f i 2 16 "1184 1184" 100 0 0 100	timestamp_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1159 (  timezone		   PGNSP PGUID 12 f f f t f s 2 1114 "25 1184" 100 0 0 100  timestamptz_zone - _null_ ));
DESCR("timestamp at a specified time zone");

DATA(insert OID = 1160 (  interval_in	   PGNSP PGUID 12 f f f t f s 1 1186 "0" 100 0 0 100  interval_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1161 (  interval_out	   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	interval_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1162 (  interval_eq	   PGNSP PGUID 12 f f f t f i 2 16 "1186 1186" 100 0 0 100	interval_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1163 (  interval_ne	   PGNSP PGUID 12 f f f t f i 2 16 "1186 1186" 100 0 0 100	interval_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1164 (  interval_lt	   PGNSP PGUID 12 f f f t f i 2 16 "1186 1186" 100 0 0 100	interval_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1165 (  interval_le	   PGNSP PGUID 12 f f f t f i 2 16 "1186 1186" 100 0 0 100	interval_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1166 (  interval_ge	   PGNSP PGUID 12 f f f t f i 2 16 "1186 1186" 100 0 0 100	interval_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1167 (  interval_gt	   PGNSP PGUID 12 f f f t f i 2 16 "1186 1186" 100 0 0 100	interval_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1168 (  interval_um	   PGNSP PGUID 12 f f f t f i 1 1186 "1186" 100 0 0 100  interval_um - _null_ ));
DESCR("subtract");
DATA(insert OID = 1169 (  interval_pl	   PGNSP PGUID 12 f f f t f i 2 1186 "1186 1186" 100 0 0 100  interval_pl - _null_ ));
DESCR("add");
DATA(insert OID = 1170 (  interval_mi	   PGNSP PGUID 12 f f f t f i 2 1186 "1186 1186" 100 0 0 100  interval_mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1171 (  date_part		   PGNSP PGUID 12 f f f t f s 2  701 "25 1184" 100 0 0 100	timestamptz_part - _null_ ));
DESCR("extract field from timestamp with time zone");
DATA(insert OID = 1172 (  date_part		   PGNSP PGUID 12 f f f t f i 2  701 "25 1186" 100 0 0 100	interval_part - _null_ ));
DESCR("extract field from interval");
DATA(insert OID = 1173 (  timestamptz	   PGNSP PGUID 12 f f t t f s 1 1184 "702" 100 0 0 100	abstime_timestamptz - _null_ ));
DESCR("convert abstime to timestamp with time zone");
DATA(insert OID = 1174 (  timestamptz	   PGNSP PGUID 12 f f t t f s 1 1184 "1082" 100 0 0 100  date_timestamptz - _null_ ));
DESCR("convert date to timestamp with time zone");
DATA(insert OID = 1176 (  timestamptz	   PGNSP PGUID 14 f f f t f s 2 1184 "1082 1083" 100 0 0 100  "select timestamptz($1 + $2)" - _null_ ));
DESCR("convert date and time to timestamp with time zone");
DATA(insert OID = 1177 (  interval		   PGNSP PGUID 12 f f t t f i 1 1186 "703" 100 0 0 100	reltime_interval - _null_ ));
DESCR("convert reltime to interval");
DATA(insert OID = 1178 (  date			   PGNSP PGUID 12 f f f t f s 1 1082 "1184" 100 0 0 100  timestamptz_date - _null_ ));
DESCR("convert timestamp with time zone to date");
DATA(insert OID = 1179 (  date			   PGNSP PGUID 12 f f f t f s 1 1082 "702" 100 0 0 100	abstime_date - _null_ ));
DESCR("convert abstime to date");
DATA(insert OID = 1180 (  abstime		   PGNSP PGUID 12 f f f t f s 1  702 "1184" 100 0 0 100  timestamptz_abstime - _null_ ));
DESCR("convert timestamp with time zone to abstime");
DATA(insert OID = 1181 (  age			   PGNSP PGUID 12 f f f t f s 1 23 "28" 100 0 0 100  xid_age - _null_ ));
DESCR("age of a transaction ID, in transactions before current transaction");

DATA(insert OID = 1188 (  timestamptz_mi   PGNSP PGUID 12 f f f t f i 2 1186 "1184 1184" 100 0 0 100  timestamp_mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1189 (  timestamptz_pl_span PGNSP PGUID 12 f f f t f i 2 1184 "1184 1186" 100 0 0 100  timestamptz_pl_span - _null_ ));
DESCR("plus");
DATA(insert OID = 1190 (  timestamptz_mi_span PGNSP PGUID 12 f f f t f i 2 1184 "1184 1186" 100 0 0 100  timestamptz_mi_span - _null_ ));
DESCR("minus");
DATA(insert OID = 1191 (  timestamptz		PGNSP PGUID 12 f f f t f s 1 1184 "25" 100 0 0 100	text_timestamptz - _null_ ));
DESCR("convert text to timestamp with time zone");
DATA(insert OID = 1192 (  text				PGNSP PGUID 12 f f t t f s 1	 25 "1184" 100 0 0 100	timestamptz_text - _null_ ));
DESCR("convert timestamp to text");
DATA(insert OID = 1193 (  text				PGNSP PGUID 12 f f t t f i 1	 25 "1186" 100 0 0 100	interval_text - _null_ ));
DESCR("convert interval to text");
DATA(insert OID = 1194 (  reltime			PGNSP PGUID 12 f f f t f i 1	703 "1186" 100 0 0 100	interval_reltime - _null_ ));
DESCR("convert interval to reltime");
DATA(insert OID = 1195 (  timestamptz_smaller PGNSP PGUID 12 f f f t f i 2 1184 "1184 1184" 100 0 0 100  timestamp_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 1196 (  timestamptz_larger  PGNSP PGUID 12 f f f t f i 2 1184 "1184 1184" 100 0 0 100  timestamp_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1197 (  interval_smaller	PGNSP PGUID 12 f f f t f i 2 1186 "1186 1186" 100 0 0 100  interval_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 1198 (  interval_larger	PGNSP PGUID 12 f f f t f i 2 1186 "1186 1186" 100 0 0 100  interval_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1199 (  age				PGNSP PGUID 12 f f f t f i 2 1186 "1184 1184" 100 0 0 100  timestamptz_age - _null_ ));
DESCR("date difference preserving months and years");

/* OIDS 1200 - 1299 */

DATA(insert OID = 1200 (  reltime		   PGNSP PGUID 12 f f f t f i 1  703 "23" 100 0 0 100  int4reltime - _null_ ));
DESCR("convert int4 to reltime");

DATA(insert OID = 1215 (  obj_description	PGNSP PGUID 14 f f f t f s 2	25 "26 19" 100 0 0 100	"select description from pg_description where objoid = $1 and classoid = (select oid from pg_class where relname = $2 and relnamespace = PGNSP) and objsubid = 0" - _null_ ));
DESCR("get description for object id and catalog name");
DATA(insert OID = 1216 (  col_description	PGNSP PGUID 14 f f f t f s 2	25 "26 23" 100 0 0 100	"select description from pg_description where objoid = $1 and classoid = \'pg_catalog.pg_class\'::regclass and objsubid = $2" - _null_ ));
DESCR("get description for table column");

DATA(insert OID = 1217 (  date_trunc	   PGNSP PGUID 12 f f f t f i 2 1184 "25 1184" 100 0 0 100	timestamptz_trunc - _null_ ));
DESCR("truncate timestamp with time zone to specified units");
DATA(insert OID = 1218 (  date_trunc	   PGNSP PGUID 12 f f f t f i 2 1186 "25 1186" 100 0 0 100	interval_trunc - _null_ ));
DESCR("truncate interval to specified units");

DATA(insert OID = 1219 (  int8inc		   PGNSP PGUID 12 f f f t f i 1 20 "20" 100 0 0 100  int8inc - _null_ ));
DESCR("increment");
DATA(insert OID = 1230 (  int8abs		   PGNSP PGUID 12 f f f t f i 1 20 "20" 100 0 0 100  int8abs - _null_ ));
DESCR("absolute value");

DATA(insert OID = 1236 (  int8larger	   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1237 (  int8smaller	   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 1238 (  texticregexeq    PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	texticregexeq - _null_ ));
DESCR("matches regex., case-insensitive");
DATA(insert OID = 1239 (  texticregexne    PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100	texticregexne - _null_ ));
DESCR("does not match regex., case-insensitive");
DATA(insert OID = 1240 (  nameicregexeq    PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100	nameicregexeq - _null_ ));
DESCR("matches regex., case-insensitive");
DATA(insert OID = 1241 (  nameicregexne    PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100	nameicregexne - _null_ ));
DESCR("does not match regex., case-insensitive");

DATA(insert OID = 1251 (  int4abs		   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  int4abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1253 (  int2abs		   PGNSP PGUID 12 f f f t f i 1 21 "21" 100 0 0 100  int2abs - _null_ ));
DESCR("absolute value");

DATA(insert OID = 1263 (  interval		   PGNSP PGUID 12 f f f t f s 1 1186 "25" 100 0 0 100  text_interval - _null_ ));
DESCR("convert text to interval");

DATA(insert OID = 1271 (  overlaps		   PGNSP PGUID 12 f f f f f i 4 16 "1266 1266 1266 1266" 100 0 0 100  overlaps_timetz - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1272 (  datetime_pl	   PGNSP PGUID 12 f f f t f i 2 1114 "1082 1083" 100 0 0 100  datetime_timestamp - _null_ ));
DESCR("convert date and time to timestamp");
DATA(insert OID = 1273 (  date_part		   PGNSP PGUID 12 f f f t f i 2  701 "25 1266" 100 0 0 100	timetz_part - _null_ ));
DESCR("extract field from time with time zone");
DATA(insert OID = 1274 (  int84pl		   PGNSP PGUID 12 f f f t f i 2 20 "20 23" 100 0 0 100	int84pl - _null_ ));
DESCR("add");
DATA(insert OID = 1275 (  int84mi		   PGNSP PGUID 12 f f f t f i 2 20 "20 23" 100 0 0 100	int84mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1276 (  int84mul		   PGNSP PGUID 12 f f f t f i 2 20 "20 23" 100 0 0 100	int84mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 1277 (  int84div		   PGNSP PGUID 12 f f f t f i 2 20 "20 23" 100 0 0 100	int84div - _null_ ));
DESCR("divide");
DATA(insert OID = 1278 (  int48pl		   PGNSP PGUID 12 f f f t f i 2 20 "23 20" 100 0 0 100	int48pl - _null_ ));
DESCR("add");
DATA(insert OID = 1279 (  int48mi		   PGNSP PGUID 12 f f f t f i 2 20 "23 20" 100 0 0 100	int48mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1280 (  int48mul		   PGNSP PGUID 12 f f f t f i 2 20 "23 20" 100 0 0 100	int48mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 1281 (  int48div		   PGNSP PGUID 12 f f f t f i 2 20 "23 20" 100 0 0 100	int48div - _null_ ));
DESCR("divide");

DATA(insert OID = 1288 (  text			   PGNSP PGUID 12 f f t t f i 1 25 "20" 100 0 0 100  int8_text - _null_ ));
DESCR("convert int8 to text");
DATA(insert OID = 1289 (  int8			   PGNSP PGUID 12 f f f t f i 1 20 "25" 100 0 0 100  text_int8 - _null_ ));
DESCR("convert text to int8");

DATA(insert OID = 1290 (  _bpchar		   PGNSP PGUID 12 f f t t f i 2 1014 "1014 23" 100 0 0 100	_bpchar - _null_ ));
DESCR("adjust char()[] to typmod length");
DATA(insert OID = 1291 (  _varchar		   PGNSP PGUID 12 f f t t f i 2 1015 "1015 23" 100 0 0 100	_varchar - _null_ ));
DESCR("adjust varchar()[] to typmod length");

DATA(insert OID = 1292 ( tideq			   PGNSP PGUID 12 f f f t f i 2 16 "27 27" 100 0 0 100	tideq - _null_ ));
DESCR("equal");
DATA(insert OID = 1293 ( currtid		   PGNSP PGUID 12 f f f t f v 2 27 "26 27" 100 0 0 100	currtid_byreloid - _null_ ));
DESCR("latest tid of a tuple");
DATA(insert OID = 1294 ( currtid2		   PGNSP PGUID 12 f f f t f v 2 27 "25 27" 100 0 0 100	currtid_byrelname - _null_ ));
DESCR("latest tid of a tuple");

DATA(insert OID = 1296 (  timedate_pl	   PGNSP PGUID 14 f f f t f i 2 1114 "1083 1082" 100 0 0 100  "select ($2 + $1)" - _null_ ));
DESCR("convert time and date to timestamp");
DATA(insert OID = 1297 (  datetimetz_pl    PGNSP PGUID 12 f f f t f i 2 1184 "1082 1266" 100 0 0 100  datetimetz_timestamptz - _null_ ));
DESCR("convert date and time with time zone to timestamp with time zone");
DATA(insert OID = 1298 (  timetzdate_pl    PGNSP PGUID 14 f f f t f i 2 1184 "1266 1082" 100 0 0 100  "select ($2 + $1)" - _null_ ));
DESCR("convert time with time zone and date to timestamp");
DATA(insert OID = 1299 (  now			   PGNSP PGUID 12 f f f t f s 0 1184 "0" 100 0 0 100  now - _null_ ));
DESCR("current transaction time");

/* OIDS 1300 - 1399 */

DATA(insert OID = 1300 (  positionsel		   PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  positionsel - _null_ ));
DESCR("restriction selectivity for position-comparison operators");
DATA(insert OID = 1301 (  positionjoinsel	   PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  positionjoinsel - _null_ ));
DESCR("join selectivity for position-comparison operators");
DATA(insert OID = 1302 (  contsel		   PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  contsel - _null_ ));
DESCR("restriction selectivity for containment comparison operators");
DATA(insert OID = 1303 (  contjoinsel	   PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  contjoinsel - _null_ ));
DESCR("join selectivity for containment comparison operators");

DATA(insert OID = 1304 ( overlaps			 PGNSP PGUID 12 f f f f f i 4 16 "1184 1184 1184 1184" 100 0 0 100	overlaps_timestamp - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1305 ( overlaps			 PGNSP PGUID 14 f f f f f i 4 16 "1184 1186 1184 1186" 100 0 0 100	"select ($1, ($1 + $2)) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1306 ( overlaps			 PGNSP PGUID 14 f f f f f i 4 16 "1184 1184 1184 1186" 100 0 0 100	"select ($1, $2) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1307 ( overlaps			 PGNSP PGUID 14 f f f f f i 4 16 "1184 1186 1184 1184" 100 0 0 100	"select ($1, ($1 + $2)) overlaps ($3, $4)" - _null_ ));
DESCR("SQL92 interval comparison");

DATA(insert OID = 1308 ( overlaps			 PGNSP PGUID 12 f f f f f i 4 16 "1083 1083 1083 1083" 100 0 0 100	overlaps_time - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1309 ( overlaps			 PGNSP PGUID 14 f f f f f i 4 16 "1083 1186 1083 1186" 100 0 0 100	"select ($1, ($1 + $2)) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1310 ( overlaps			 PGNSP PGUID 14 f f f f f i 4 16 "1083 1083 1083 1186" 100 0 0 100	"select ($1, $2) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1311 ( overlaps			 PGNSP PGUID 14 f f f f f i 4 16 "1083 1186 1083 1083" 100 0 0 100	"select ($1, ($1 + $2)) overlaps ($3, $4)" - _null_ ));
DESCR("SQL92 interval comparison");

DATA(insert OID = 1312 (  timestamp_in		 PGNSP PGUID 12 f f f t f s 1 1114 "0" 100 0 0 100	timestamp_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1313 (  timestamp_out		 PGNSP PGUID 12 f f f t f s 1 23 "0" 100 0 0 100  timestamp_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1314 (  timestamptz_cmp	 PGNSP PGUID 12 f f f t f i 2	23 "1184 1184" 100 0 0 100	timestamp_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1315 (  interval_cmp		 PGNSP PGUID 12 f f f t f i 2	23 "1186 1186" 100 0 0 100	interval_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1316 (  time				 PGNSP PGUID 12 f f f t f i 1 1083 "1114" 100 0 0 100  timestamp_time - _null_ ));
DESCR("convert timestamp to time");

DATA(insert OID = 1317 (  length			 PGNSP PGUID 12 f f f t f i 1	23 "25" 100 0 0 100  textlen - _null_ ));
DESCR("length");
DATA(insert OID = 1318 (  length			 PGNSP PGUID 12 f f f t f i 1	23 "1042" 100 0 0 100  bpcharlen - _null_ ));
DESCR("character length");
DATA(insert OID = 1319 (  length			 PGNSP PGUID 12 f f f t f i 1	23 "1043" 100 0 0 100  varcharlen - _null_ ));
DESCR("character length");

DATA(insert OID = 1326 (  interval_div		 PGNSP PGUID 12 f f f t f i 2 1186 "1186 701" 100 0 0 100  interval_div - _null_ ));
DESCR("divide");

DATA(insert OID = 1339 (  dlog10			 PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dlog10 - _null_ ));
DESCR("base 10 logarithm");
DATA(insert OID = 1340 (  log				 PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dlog10 - _null_ ));
DESCR("base 10 logarithm");
DATA(insert OID = 1341 (  ln				 PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dlog1 - _null_ ));
DESCR("natural logarithm");
DATA(insert OID = 1342 (  round				 PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dround - _null_ ));
DESCR("round to nearest integer");
DATA(insert OID = 1343 (  trunc				 PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dtrunc - _null_ ));
DESCR("truncate to integer");
DATA(insert OID = 1344 (  sqrt				 PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dsqrt - _null_ ));
DESCR("square root");
DATA(insert OID = 1345 (  cbrt				 PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dcbrt - _null_ ));
DESCR("cube root");
DATA(insert OID = 1346 (  pow				 PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100  dpow - _null_ ));
DESCR("exponentiation");
DATA(insert OID = 1347 (  exp				 PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  dexp - _null_ ));
DESCR("exponential");

/*
 * This form of obj_description is now deprecated, since it will fail if
 * OIDs are not unique across system catalogs.	Use the other forms instead.
 */
DATA(insert OID = 1348 (  obj_description	 PGNSP PGUID 14 f f f t f s 1	25 "26" 100 0 0 100  "select description from pg_description where objoid = $1 and objsubid = 0" - _null_ ));
DESCR("get description for object id (deprecated)");
DATA(insert OID = 1349 (  oidvectortypes	 PGNSP PGUID 12 f f f t f s 1	25 "30" 100 0 0 100  oidvectortypes - _null_ ));
DESCR("print type names of oidvector field");


DATA(insert OID = 1350 (  timetz_in		   PGNSP PGUID 12 f f f t f s 1 1266 "0" 100 0 0 100  timetz_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1351 (  timetz_out	   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	timetz_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1352 (  timetz_eq		   PGNSP PGUID 12 f f f t f i 2 16 "1266 1266" 100 0 0 100	timetz_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1353 (  timetz_ne		   PGNSP PGUID 12 f f f t f i 2 16 "1266 1266" 100 0 0 100	timetz_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1354 (  timetz_lt		   PGNSP PGUID 12 f f f t f i 2 16 "1266 1266" 100 0 0 100	timetz_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1355 (  timetz_le		   PGNSP PGUID 12 f f f t f i 2 16 "1266 1266" 100 0 0 100	timetz_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1356 (  timetz_ge		   PGNSP PGUID 12 f f f t f i 2 16 "1266 1266" 100 0 0 100	timetz_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1357 (  timetz_gt		   PGNSP PGUID 12 f f f t f i 2 16 "1266 1266" 100 0 0 100	timetz_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1358 (  timetz_cmp	   PGNSP PGUID 12 f f f t f i 2 23 "1266 1266" 100 0 0 100	timetz_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1359 (  timestamptz	   PGNSP PGUID 12 f f f t f i 2 1184 "1082 1266" 100 0 0 100  datetimetz_timestamptz - _null_ ));
DESCR("convert date and time with time zone to timestamp with time zone");

DATA(insert OID = 1364 (  time				 PGNSP PGUID 14 f f f t f i 1 1083 "702" 100 0 0 100  "select time(cast($1 as timestamp without time zone))" - _null_ ));
DESCR("convert abstime to time");

DATA(insert OID = 1367 (  character_length	PGNSP PGUID 12 f f f t f i 1	23 "1042" 100 0 0 100  bpcharlen - _null_ ));
DESCR("character length");
DATA(insert OID = 1368 (  character_length	PGNSP PGUID 12 f f f t f i 1	23 "1043" 100 0 0 100  varcharlen - _null_ ));
DESCR("character length");
DATA(insert OID = 1369 (  character_length	PGNSP PGUID 12 f f f t f i 1	23 "25" 100 0 0 100  textlen - _null_ ));
DESCR("character length");

DATA(insert OID = 1370 (  interval			 PGNSP PGUID 12 f f t t f i 1 1186 "1083" 100 0 0 100  time_interval - _null_ ));
DESCR("convert time to interval");
DATA(insert OID = 1372 (  char_length		 PGNSP PGUID 12 f f f t f i 1	23	 "1042" 100 0 0 100  bpcharlen - _null_ ));
DESCR("character length");
DATA(insert OID = 1373 (  char_length		 PGNSP PGUID 12 f f f t f i 1	23	 "1043" 100 0 0 100  varcharlen - _null_ ));
DESCR("character length");

DATA(insert OID = 1374 (  octet_length			 PGNSP PGUID 12 f f f t f i 1	23	 "25" 100 0 0 100  textoctetlen - _null_ ));
DESCR("octet length");
DATA(insert OID = 1375 (  octet_length			 PGNSP PGUID 12 f f f t f i 1	23	 "1042" 100 0 0 100  bpcharoctetlen - _null_ ));
DESCR("octet length");
DATA(insert OID = 1376 (  octet_length			 PGNSP PGUID 12 f f f t f i 1	23	 "1043" 100 0 0 100  varcharoctetlen - _null_ ));
DESCR("octet length");

DATA(insert OID = 1377 (  time_larger	   PGNSP PGUID 12 f f f t f i 2 1083 "1083 1083" 100 0 0 100  time_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1378 (  time_smaller	   PGNSP PGUID 12 f f f t f i 2 1083 "1083 1083" 100 0 0 100  time_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 1379 (  timetz_larger    PGNSP PGUID 12 f f f t f i 2 1266 "1266 1266" 100 0 0 100  timetz_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1380 (  timetz_smaller   PGNSP PGUID 12 f f f t f i 2 1266 "1266 1266" 100 0 0 100  timetz_smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 1381 (  char_length	   PGNSP PGUID 12 f f f t f i 1 23 "25" 100 0 0 100  textlen - _null_ ));
DESCR("character length");

DATA(insert OID = 1382 (  date_part    PGNSP PGUID 14 f f f t f s 2  701 "25 702" 100 0 0 100  "select date_part($1, timestamptz($2))" - _null_ ));
DESCR("extract field from abstime");
DATA(insert OID = 1383 (  date_part    PGNSP PGUID 14 f f f t f s 2  701 "25 703" 100 0 0 100  "select date_part($1, cast($2 as interval))" - _null_ ));
DESCR("extract field from reltime");
DATA(insert OID = 1384 (  date_part    PGNSP PGUID 14 f f f t f i 2  701 "25 1082" 100 0 0 100	"select date_part($1, cast($2 as timestamp without time zone))" - _null_ ));
DESCR("extract field from date");
DATA(insert OID = 1385 (  date_part	   PGNSP PGUID 12 f f f t f i 2  701 "25 1083" 100 0 0 100  time_part - _null_ ));
DESCR("extract field from time");
DATA(insert OID = 1386 (  age		   PGNSP PGUID 14 f f f t f s 1 1186 "1184" 100 0 0 100  "select age(cast(current_date as timestamp with time zone), $1)" - _null_ ));
DESCR("date difference from today preserving months and years");

DATA(insert OID = 1388 (  timetz	   PGNSP PGUID 12 f f f t f s 1 1266 "1184" 100 0 0 100  timestamptz_timetz - _null_ ));
DESCR("convert timestamptz to timetz");

DATA(insert OID = 1389 (  isfinite	   PGNSP PGUID 12 f f f t f i 1 16 "1184" 100 0 0 100  timestamp_finite - _null_ ));
DESCR("boolean test");
DATA(insert OID = 1390 (  isfinite	   PGNSP PGUID 12 f f f t f i 1 16 "1186" 100 0 0 100  interval_finite - _null_ ));
DESCR("boolean test");


DATA(insert OID = 1391 (  factorial		   PGNSP PGUID 12 f f f t f i 1 23 "21" 100 0 0 100  int2fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 1392 (  factorial		   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  int4fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 1393 (  factorial		   PGNSP PGUID 12 f f f t f i 1 20 "20" 100 0 0 100  int8fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 1394 (  abs			   PGNSP PGUID 12 f f f t f i 1 700 "700" 100 0 0 100  float4abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1395 (  abs			   PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100  float8abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1396 (  abs			   PGNSP PGUID 12 f f f t f i 1 20 "20" 100 0 0 100  int8abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1397 (  abs			   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  int4abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1398 (  abs			   PGNSP PGUID 12 f f f t f i 1 21 "21" 100 0 0 100  int2abs - _null_ ));
DESCR("absolute value");

/* OIDS 1400 - 1499 */

DATA(insert OID = 1400 (  name		   PGNSP PGUID 12 f f t t f i 1 19 "1043" 100 0 0 100  text_name - _null_ ));
DESCR("convert varchar to name");
DATA(insert OID = 1401 (  varchar	   PGNSP PGUID 12 f f t t f i 1 1043 "19" 100 0 0 100  name_text - _null_ ));
DESCR("convert name to varchar");

DATA(insert OID = 1402 (  current_schema	PGNSP PGUID 12 f f f t f s 0    19 "0" 100 0 0 100	current_schema - _null_ ));
DESCR("current schema name");
DATA(insert OID = 1403 (  current_schemas	PGNSP PGUID 12 f f f t f s 0  1003 "0" 100 0 0 100	current_schemas - _null_ ));
DESCR("current schema search list");

DATA(insert OID = 1406 (  isvertical		PGNSP PGUID 12 f f f t f i 2	16 "600 600" 100 0 0 100  point_vert - _null_ ));
DESCR("vertically aligned?");
DATA(insert OID = 1407 (  ishorizontal		PGNSP PGUID 12 f f f t f i 2	16 "600 600" 100 0 0 100  point_horiz - _null_ ));
DESCR("horizontally aligned?");
DATA(insert OID = 1408 (  isparallel		PGNSP PGUID 12 f f f t f i 2	16 "601 601" 100 0 0 100  lseg_parallel - _null_ ));
DESCR("parallel?");
DATA(insert OID = 1409 (  isperp			PGNSP PGUID 12 f f f t f i 2	16 "601 601" 100 0 0 100  lseg_perp - _null_ ));
DESCR("perpendicular?");
DATA(insert OID = 1410 (  isvertical		PGNSP PGUID 12 f f f t f i 1	16 "601" 100 0 0 100  lseg_vertical - _null_ ));
DESCR("vertical?");
DATA(insert OID = 1411 (  ishorizontal		PGNSP PGUID 12 f f f t f i 1	16 "601" 100 0 0 100  lseg_horizontal - _null_ ));
DESCR("horizontal?");
DATA(insert OID = 1412 (  isparallel		PGNSP PGUID 12 f f f t f i 2	16 "628 628" 100 0 0 100  line_parallel - _null_ ));
DESCR("lines parallel?");
DATA(insert OID = 1413 (  isperp			PGNSP PGUID 12 f f f t f i 2	16 "628 628" 100 0 0 100  line_perp - _null_ ));
DESCR("lines perpendicular?");
DATA(insert OID = 1414 (  isvertical		PGNSP PGUID 12 f f f t f i 1	16 "628" 100 0 0 100  line_vertical - _null_ ));
DESCR("lines vertical?");
DATA(insert OID = 1415 (  ishorizontal		PGNSP PGUID 12 f f f t f i 1	16 "628" 100 0 0 100  line_horizontal - _null_ ));
DESCR("lines horizontal?");
DATA(insert OID = 1416 (  point				PGNSP PGUID 12 f f f t f i 1 600 "718" 100 0 0 100	circle_center - _null_ ));
DESCR("center of");

DATA(insert OID = 1417 (  isnottrue			PGNSP PGUID 12 f f f f f i 1 16 "16" 100 0 0 100  isnottrue - _null_ ));
DESCR("bool is not true (ie, false or unknown)");
DATA(insert OID = 1418 (  isnotfalse		PGNSP PGUID 12 f f f f f i 1 16 "16" 100 0 0 100  isnotfalse - _null_ ));
DESCR("bool is not false (ie, true or unknown)");

DATA(insert OID = 1419 (  time				PGNSP PGUID 12 f f f t f i 1 1083 "1186" 100 0 0 100  interval_time - _null_ ));
DESCR("convert interval to time");

DATA(insert OID = 1421 (  box				PGNSP PGUID 12 f f f t f i 2 603 "600 600" 100 0 0 100	points_box - _null_ ));
DESCR("convert points to box");
DATA(insert OID = 1422 (  box_add			PGNSP PGUID 12 f f f t f i 2 603 "603 600" 100 0 0 100	box_add - _null_ ));
DESCR("add point to box (translate)");
DATA(insert OID = 1423 (  box_sub			PGNSP PGUID 12 f f f t f i 2 603 "603 600" 100 0 0 100	box_sub - _null_ ));
DESCR("subtract point from box (translate)");
DATA(insert OID = 1424 (  box_mul			PGNSP PGUID 12 f f f t f i 2 603 "603 600" 100 0 0 100	box_mul - _null_ ));
DESCR("multiply box by point (scale)");
DATA(insert OID = 1425 (  box_div			PGNSP PGUID 12 f f f t f i 2 603 "603 600" 100 0 0 100	box_div - _null_ ));
DESCR("divide box by point (scale)");
DATA(insert OID = 1426 (  path_contain_pt	PGNSP PGUID 14 f f f t f i 2	16 "602 600" 100 0 0 100  "select on_ppath($2, $1)" - _null_ ));
DESCR("path contains point?");
DATA(insert OID = 1428 (  poly_contain_pt	PGNSP PGUID 12 f f f t f i 2	16 "604 600" 100 0 0 100  poly_contain_pt - _null_ ));
DESCR("polygon contains point?");
DATA(insert OID = 1429 (  pt_contained_poly PGNSP PGUID 12 f f f t f i 2	16 "600 604" 100 0 0 100  pt_contained_poly - _null_ ));
DESCR("point contained by polygon?");

DATA(insert OID = 1430 (  isclosed			PGNSP PGUID 12 f f f t f i 1	16 "602" 100 0 0 100  path_isclosed - _null_ ));
DESCR("path closed?");
DATA(insert OID = 1431 (  isopen			PGNSP PGUID 12 f f f t f i 1	16 "602" 100 0 0 100  path_isopen - _null_ ));
DESCR("path open?");
DATA(insert OID = 1432 (  path_npoints		PGNSP PGUID 12 f f f t f i 1	23 "602" 100 0 0 100  path_npoints - _null_ ));
DESCR("# points in path");

/* pclose and popen might better be named close and open, but that crashes initdb.
 * - thomas 97/04/20
 */

DATA(insert OID = 1433 (  pclose			PGNSP PGUID 12 f f f t f i 1 602 "602" 100 0 0 100	path_close - _null_ ));
DESCR("close path");
DATA(insert OID = 1434 (  popen				PGNSP PGUID 12 f f f t f i 1 602 "602" 100 0 0 100	path_open - _null_ ));
DESCR("open path");
DATA(insert OID = 1435 (  path_add			PGNSP PGUID 12 f f f t f i 2 602 "602 602" 100 0 0 100	path_add - _null_ ));
DESCR("concatenate open paths");
DATA(insert OID = 1436 (  path_add_pt		PGNSP PGUID 12 f f f t f i 2 602 "602 600" 100 0 0 100	path_add_pt - _null_ ));
DESCR("add (translate path)");
DATA(insert OID = 1437 (  path_sub_pt		PGNSP PGUID 12 f f f t f i 2 602 "602 600" 100 0 0 100	path_sub_pt - _null_ ));
DESCR("subtract (translate path)");
DATA(insert OID = 1438 (  path_mul_pt		PGNSP PGUID 12 f f f t f i 2 602 "602 600" 100 0 0 100	path_mul_pt - _null_ ));
DESCR("multiply (rotate/scale path)");
DATA(insert OID = 1439 (  path_div_pt		PGNSP PGUID 12 f f f t f i 2 602 "602 600" 100 0 0 100	path_div_pt - _null_ ));
DESCR("divide (rotate/scale path)");

DATA(insert OID = 1440 (  point				PGNSP PGUID 12 f f f t f i 2 600 "701 701" 100 0 0 100	construct_point - _null_ ));
DESCR("convert x, y to point");
DATA(insert OID = 1441 (  point_add			PGNSP PGUID 12 f f f t f i 2 600 "600 600" 100 0 0 100	point_add - _null_ ));
DESCR("add points (translate)");
DATA(insert OID = 1442 (  point_sub			PGNSP PGUID 12 f f f t f i 2 600 "600 600" 100 0 0 100	point_sub - _null_ ));
DESCR("subtract points (translate)");
DATA(insert OID = 1443 (  point_mul			PGNSP PGUID 12 f f f t f i 2 600 "600 600" 100 0 0 100	point_mul - _null_ ));
DESCR("multiply points (scale/rotate)");
DATA(insert OID = 1444 (  point_div			PGNSP PGUID 12 f f f t f i 2 600 "600 600" 100 0 0 100	point_div - _null_ ));
DESCR("divide points (scale/rotate)");

DATA(insert OID = 1445 (  poly_npoints		PGNSP PGUID 12 f f f t f i 1	23 "604" 100 0 0 100  poly_npoints - _null_ ));
DESCR("number of points in polygon");
DATA(insert OID = 1446 (  box				PGNSP PGUID 12 f f f t f i 1 603 "604" 100 0 0 100	poly_box - _null_ ));
DESCR("convert polygon to bounding box");
DATA(insert OID = 1447 (  path				PGNSP PGUID 12 f f f t f i 1 602 "604" 100 0 0 100	poly_path - _null_ ));
DESCR("convert polygon to path");
DATA(insert OID = 1448 (  polygon			PGNSP PGUID 12 f f f t f i 1 604 "603" 100 0 0 100	box_poly - _null_ ));
DESCR("convert box to polygon");
DATA(insert OID = 1449 (  polygon			PGNSP PGUID 12 f f f t f i 1 604 "602" 100 0 0 100	path_poly - _null_ ));
DESCR("convert path to polygon");

DATA(insert OID = 1450 (  circle_in			PGNSP PGUID 12 f f f t f i 1 718 "0" 100 0 0 100  circle_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1451 (  circle_out		PGNSP PGUID 12 f f f t f i 1	23	"718" 100 0 0 100  circle_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1452 (  circle_same		PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_same - _null_ ));
DESCR("same as");
DATA(insert OID = 1453 (  circle_contain	PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_contain - _null_ ));
DESCR("contains");
DATA(insert OID = 1454 (  circle_left		PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_left - _null_ ));
DESCR("is left of");
DATA(insert OID = 1455 (  circle_overleft	PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_overleft - _null_ ));
DESCR("overlaps, but does not extend to right of");
DATA(insert OID = 1456 (  circle_overright	PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_overright - _null_ ));
DESCR("");
DATA(insert OID = 1457 (  circle_right		PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_right - _null_ ));
DESCR("is right of");
DATA(insert OID = 1458 (  circle_contained	PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_contained - _null_ ));
DESCR("");
DATA(insert OID = 1459 (  circle_overlap	PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_overlap - _null_ ));
DESCR("overlaps");
DATA(insert OID = 1460 (  circle_below		PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_below - _null_ ));
DESCR("is below");
DATA(insert OID = 1461 (  circle_above		PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_above - _null_ ));
DESCR("is above");
DATA(insert OID = 1462 (  circle_eq			PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_eq - _null_ ));
DESCR("equal by area");
DATA(insert OID = 1463 (  circle_ne			PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_ne - _null_ ));
DESCR("not equal by area");
DATA(insert OID = 1464 (  circle_lt			PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_lt - _null_ ));
DESCR("less-than by area");
DATA(insert OID = 1465 (  circle_gt			PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_gt - _null_ ));
DESCR("greater-than by area");
DATA(insert OID = 1466 (  circle_le			PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_le - _null_ ));
DESCR("less-than-or-equal by area");
DATA(insert OID = 1467 (  circle_ge			PGNSP PGUID 12 f f f t f i 2	16 "718 718" 100 0 0 100  circle_ge - _null_ ));
DESCR("greater-than-or-equal by area");
DATA(insert OID = 1468 (  area				PGNSP PGUID 12 f f f t f i 1 701 "718" 100 0 0 100	circle_area - _null_ ));
DESCR("area of circle");
DATA(insert OID = 1469 (  diameter			PGNSP PGUID 12 f f f t f i 1 701 "718" 100 0 0 100	circle_diameter - _null_ ));
DESCR("diameter of circle");
DATA(insert OID = 1470 (  radius			PGNSP PGUID 12 f f f t f i 1 701 "718" 100 0 0 100	circle_radius - _null_ ));
DESCR("radius of circle");
DATA(insert OID = 1471 (  circle_distance	PGNSP PGUID 12 f f f t f i 2 701 "718 718" 100 0 0 100	circle_distance - _null_ ));
DESCR("distance between");
DATA(insert OID = 1472 (  circle_center		PGNSP PGUID 12 f f f t f i 1 600 "718" 100 0 0 100	circle_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1473 (  circle			PGNSP PGUID 12 f f f t f i 2 718 "600 701" 100 0 0 100	cr_circle - _null_ ));
DESCR("convert point and radius to circle");
DATA(insert OID = 1474 (  circle			PGNSP PGUID 12 f f f t f i 1 718 "604" 100 0 0 100	poly_circle - _null_ ));
DESCR("convert polygon to circle");
DATA(insert OID = 1475 (  polygon			PGNSP PGUID 12 f f f t f i 2 604 "23 718" 100 0 0 100  circle_poly - _null_ ));
DESCR("convert vertex count and circle to polygon");
DATA(insert OID = 1476 (  dist_pc			PGNSP PGUID 12 f f f t f i 2 701 "600 718" 100 0 0 100	dist_pc - _null_ ));
DESCR("distance between point and circle");
DATA(insert OID = 1477 (  circle_contain_pt PGNSP PGUID 12 f f f t f i 2	16 "718 600" 100 0 0 100  circle_contain_pt - _null_ ));
DESCR("circle contains point?");
DATA(insert OID = 1478 (  pt_contained_circle	PGNSP PGUID 12 f f f t f i 2	16 "600 718" 100 0 0 100  pt_contained_circle - _null_ ));
DESCR("point inside circle?");
DATA(insert OID = 1479 (  circle			PGNSP PGUID 12 f f f t f i 1 718 "603" 100 0 0 100	box_circle - _null_ ));
DESCR("convert box to circle");
DATA(insert OID = 1480 (  box				PGNSP PGUID 12 f f f t f i 1 603 "718" 100 0 0 100	circle_box - _null_ ));
DESCR("convert circle to box");
DATA(insert OID = 1481 (  tinterval			 PGNSP PGUID 12 f f f t f i 2 704 "702 702" 100 0 0 100 mktinterval - _null_ ));
DESCR("convert to tinterval");

DATA(insert OID = 1482 (  lseg_ne			PGNSP PGUID 12 f f f t f i 2	16 "601 601" 100 0 0 100  lseg_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1483 (  lseg_lt			PGNSP PGUID 12 f f f t f i 2	16 "601 601" 100 0 0 100  lseg_lt - _null_ ));
DESCR("less-than by length");
DATA(insert OID = 1484 (  lseg_le			PGNSP PGUID 12 f f f t f i 2	16 "601 601" 100 0 0 100  lseg_le - _null_ ));
DESCR("less-than-or-equal by length");
DATA(insert OID = 1485 (  lseg_gt			PGNSP PGUID 12 f f f t f i 2	16 "601 601" 100 0 0 100  lseg_gt - _null_ ));
DESCR("greater-than by length");
DATA(insert OID = 1486 (  lseg_ge			PGNSP PGUID 12 f f f t f i 2	16 "601 601" 100 0 0 100  lseg_ge - _null_ ));
DESCR("greater-than-or-equal by length");
DATA(insert OID = 1487 (  lseg_length		PGNSP PGUID 12 f f f t f i 1 701 "601" 100 0 0 100	lseg_length - _null_ ));
DESCR("distance between endpoints");
DATA(insert OID = 1488 (  close_ls			PGNSP PGUID 12 f f f t f i 2 600 "628 601" 100 0 0 100	close_ls - _null_ ));
DESCR("closest point to line on line segment");
DATA(insert OID = 1489 (  close_lseg		PGNSP PGUID 12 f f f t f i 2 600 "601 601" 100 0 0 100	close_lseg - _null_ ));
DESCR("closest point to line segment on line segment");

DATA(insert OID = 1490 (  line_in			PGNSP PGUID 12 f f f t f i 1 628 "0" 100 0 0 100  line_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1491 (  line_out			PGNSP PGUID 12 f f f t f i 1 23  "628" 100 0 0 100	line_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1492 (  line_eq			PGNSP PGUID 12 f f f t f i 2  16 "628 628" 100 0 0 100	line_eq - _null_ ));
DESCR("lines equal?");
DATA(insert OID = 1493 (  line				PGNSP PGUID 12 f f f t f i 2 628 "600 600" 100 0 0 100	line_construct_pp - _null_ ));
DESCR("line from points");
DATA(insert OID = 1494 (  line_interpt		PGNSP PGUID 12 f f f t f i 2 600 "628 628" 100 0 0 100	line_interpt - _null_ ));
DESCR("intersection point");
DATA(insert OID = 1495 (  line_intersect	PGNSP PGUID 12 f f f t f i 2	16 "628 628" 100 0 0 100  line_intersect - _null_ ));
DESCR("lines intersect?");
DATA(insert OID = 1496 (  line_parallel		PGNSP PGUID 12 f f f t f i 2	16 "628 628" 100 0 0 100  line_parallel - _null_ ));
DESCR("lines parallel?");
DATA(insert OID = 1497 (  line_perp			PGNSP PGUID 12 f f f t f i 2	16 "628 628" 100 0 0 100  line_perp - _null_ ));
DESCR("lines perpendicular?");
DATA(insert OID = 1498 (  line_vertical		PGNSP PGUID 12 f f f t f i 1	16 "628" 100 0 0 100  line_vertical - _null_ ));
DESCR("lines vertical?");
DATA(insert OID = 1499 (  line_horizontal	PGNSP PGUID 12 f f f t f i 1	16 "628" 100 0 0 100  line_horizontal - _null_ ));
DESCR("lines horizontal?");

/* OIDS 1500 - 1599 */

DATA(insert OID = 1530 (  length			PGNSP PGUID 12 f f f t f i 1 701 "601" 100 0 0 100	lseg_length - _null_ ));
DESCR("distance between endpoints");
DATA(insert OID = 1531 (  length			PGNSP PGUID 12 f f f t f i 1 701 "602" 100 0 0 100	path_length - _null_ ));
DESCR("sum of path segments");


DATA(insert OID = 1532 (  point				PGNSP PGUID 12 f f f t f i 1 600 "601" 100 0 0 100	lseg_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1533 (  point				PGNSP PGUID 12 f f f t f i 1 600 "602" 100 0 0 100	path_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1534 (  point				PGNSP PGUID 12 f f f t f i 1 600 "603" 100 0 0 100	box_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1540 (  point				PGNSP PGUID 12 f f f t f i 1 600 "604" 100 0 0 100	poly_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1541 (  lseg				PGNSP PGUID 12 f f f t f i 1 601 "603" 100 0 0 100	box_diagonal - _null_ ));
DESCR("diagonal of");
DATA(insert OID = 1542 (  center			PGNSP PGUID 12 f f f t f i 1 600 "603" 100 0 0 100	box_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1543 (  center			PGNSP PGUID 12 f f f t f i 1 600 "718" 100 0 0 100	circle_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1544 (  polygon			PGNSP PGUID 14 f f f t f i 1 604 "718" 100 0 0 100	"select polygon(12, $1)" - _null_ ));
DESCR("convert circle to 12-vertex polygon");
DATA(insert OID = 1545 (  npoints			PGNSP PGUID 12 f f f t f i 1	23 "602" 100 0 0 100  path_npoints - _null_ ));
DESCR("# points in path");
DATA(insert OID = 1556 (  npoints			PGNSP PGUID 12 f f f t f i 1	23 "604" 100 0 0 100  poly_npoints - _null_ ));
DESCR("number of points in polygon");

DATA(insert OID = 1564 (  bit_in			PGNSP PGUID 12 f f f t f i 1 1560 "0" 100 0 0 100  bit_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1565 (  bit_out			PGNSP PGUID 12 f f f t f i 1   23 "0" 100 0 0 100  bit_out - _null_ ));
DESCR("(internal)");

DATA(insert OID = 1569 (  like				PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100  textlike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 1570 (  notlike			PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100  textnlike - _null_ ));
DESCR("does not match LIKE expression");
DATA(insert OID = 1571 (  like				PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100  namelike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 1572 (  notlike			PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100  namenlike - _null_ ));
DESCR("does not match LIKE expression");


/* SEQUENCEs nextval & currval functions */
DATA(insert OID = 1574 (  nextval			PGNSP PGUID 12 f f f t f v 1 20 "25" 100 0 0 100  nextval - _null_ ));
DESCR("sequence next value");
DATA(insert OID = 1575 (  currval			PGNSP PGUID 12 f f f t f v 1 20 "25" 100 0 0 100  currval - _null_ ));
DESCR("sequence current value");
DATA(insert OID = 1576 (  setval			PGNSP PGUID 12 f f f t f v 2 20 "25 20" 100 0 0 100  setval - _null_ ));
DESCR("set sequence value");
DATA(insert OID = 1765 (  setval			PGNSP PGUID 12 f f f t f v 3 20 "25 20 16" 100 0 0 100	setval_and_iscalled - _null_ ));
DESCR("set sequence value and iscalled status");

DATA(insert OID = 1579 (  varbit_in			PGNSP PGUID 12 f f f t f i 1 1562 "0" 100 0 0 100  varbit_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1580 (  varbit_out		PGNSP PGUID 12 f f f t f i 1   23 "0" 100 0 0 100  varbit_out - _null_ ));
DESCR("(internal)");

DATA(insert OID = 1581 (  biteq				PGNSP PGUID 12 f f f t f i 2 16 "1560 1560" 100 0 0 100  biteq - _null_ ));
DESCR("equal");
DATA(insert OID = 1582 (  bitne				PGNSP PGUID 12 f f f t f i 2 16 "1560 1560" 100 0 0 100  bitne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1592 (  bitge				PGNSP PGUID 12 f f f t f i 2 16 "1560 1560" 100 0 0 100  bitge - _null_ ));
DESCR("greater than or equal");
DATA(insert OID = 1593 (  bitgt				PGNSP PGUID 12 f f f t f i 2 16 "1560 1560" 100 0 0 100  bitgt - _null_ ));
DESCR("greater than");
DATA(insert OID = 1594 (  bitle				PGNSP PGUID 12 f f f t f i 2 16 "1560 1560" 100 0 0 100  bitle - _null_ ));
DESCR("less than or equal");
DATA(insert OID = 1595 (  bitlt				PGNSP PGUID 12 f f f t f i 2 16 "1560 1560" 100 0 0 100  bitlt - _null_ ));
DESCR("less than");
DATA(insert OID = 1596 (  bitcmp			PGNSP PGUID 12 f f f t f i 2 23 "1560 1560" 100 0 0 100  bitcmp - _null_ ));
DESCR("compare");

DATA(insert OID = 1598 (  random			PGNSP PGUID 12 f f f t f v 0 701 "0" 100 0 0 100  drandom - _null_ ));
DESCR("random value");
DATA(insert OID = 1599 (  setseed			PGNSP PGUID 12 f f f t f v 1  23 "701" 100 0 0 100	setseed - _null_ ));
DESCR("set random seed");

/* OIDS 1600 - 1699 */

DATA(insert OID = 1600 (  asin				PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	dasin - _null_ ));
DESCR("arcsine");
DATA(insert OID = 1601 (  acos				PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	dacos - _null_ ));
DESCR("arccosine");
DATA(insert OID = 1602 (  atan				PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	datan - _null_ ));
DESCR("arctangent");
DATA(insert OID = 1603 (  atan2				PGNSP PGUID 12 f f f t f i 2 701 "701 701" 100 0 0 100	datan2 - _null_ ));
DESCR("arctangent, two arguments");
DATA(insert OID = 1604 (  sin				PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	dsin - _null_ ));
DESCR("sine");
DATA(insert OID = 1605 (  cos				PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	dcos - _null_ ));
DESCR("cosine");
DATA(insert OID = 1606 (  tan				PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	dtan - _null_ ));
DESCR("tangent");
DATA(insert OID = 1607 (  cot				PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	dcot - _null_ ));
DESCR("cotangent");
DATA(insert OID = 1608 (  degrees			PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	degrees - _null_ ));
DESCR("radians to degrees");
DATA(insert OID = 1609 (  radians			PGNSP PGUID 12 f f f t f i 1 701 "701" 100 0 0 100	radians - _null_ ));
DESCR("degrees to radians");
DATA(insert OID = 1610 (  pi				PGNSP PGUID 12 f f f t f i 0 701 "0" 100 0 0 100  dpi - _null_ ));
DESCR("PI");

DATA(insert OID = 1618 (  interval_mul		PGNSP PGUID 12 f f f t f i 2 1186 "1186 701" 100 0 0 100  interval_mul - _null_ ));
DESCR("multiply interval");
DATA(insert OID = 1619 (  varchar			PGNSP PGUID 12 f f f t f i 1 1043 "23" 100 0 0 100	int4_text - _null_ ));
DESCR("convert int4 to varchar");

DATA(insert OID = 1620 (  ascii				PGNSP PGUID 12 f f f t f i 1 23 "25" 100 0 0 100  ascii - _null_ ));
DESCR("convert first char to int4");
DATA(insert OID = 1621 (  chr				PGNSP PGUID 12 f f f t f i 1 25 "23" 100 0 0 100  chr - _null_ ));
DESCR("convert int4 to char");
DATA(insert OID = 1622 (  repeat			PGNSP PGUID 12 f f f t f i 2 25 "25 23" 100 0 0 100  repeat - _null_ ));
DESCR("replicate string int4 times");

DATA(insert OID = 1623 (  varchar			PGNSP PGUID 12 f f f t f i 1 1043 "20" 100 0 0 100	int8_text - _null_ ));
DESCR("convert int8 to varchar");
DATA(insert OID = 1624 (  mul_d_interval	PGNSP PGUID 12 f f f t f i 2 1186 "701 1186" 100 0 0 100  mul_d_interval - _null_ ));

DATA(insert OID = 1633 (  texticlike		PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100 texticlike - _null_ ));
DESCR("matches LIKE expression, case-insensitive");
DATA(insert OID = 1634 (  texticnlike		PGNSP PGUID 12 f f f t f i 2 16 "25 25" 100 0 0 100 texticnlike - _null_ ));
DESCR("does not match LIKE expression, case-insensitive");
DATA(insert OID = 1635 (  nameiclike		PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100  nameiclike - _null_ ));
DESCR("matches LIKE expression, case-insensitive");
DATA(insert OID = 1636 (  nameicnlike		PGNSP PGUID 12 f f f t f i 2 16 "19 25" 100 0 0 100  nameicnlike - _null_ ));
DESCR("does not match LIKE expression, case-insensitive");
DATA(insert OID = 1637 (  like_escape		PGNSP PGUID 12 f f f t f i 2 25 "25 25" 100 0 0 100 like_escape - _null_ ));
DESCR("convert match pattern to use backslash escapes");

DATA(insert OID = 1689 (  update_pg_pwd_and_pg_group  PGNSP PGUID 12 f f f t f v 0 0  ""  100 0 0 100  update_pg_pwd_and_pg_group - _null_ ));
DESCR("update pg_pwd and pg_group files");

/* Oracle Compatibility Related Functions - By Edmund Mergl <E.Mergl@bawue.de> */
DATA(insert OID =  868 (  strpos	   PGNSP PGUID 12 f f f t f i 2 23 "25 25" 100 0 0 100	textpos - _null_ ));
DESCR("find position of substring");
DATA(insert OID =  870 (  lower		   PGNSP PGUID 12 f f f t f i 1 25 "25" 100 0 0 100  lower - _null_ ));
DESCR("lowercase");
DATA(insert OID =  871 (  upper		   PGNSP PGUID 12 f f f t f i 1 25 "25" 100 0 0 100  upper - _null_ ));
DESCR("uppercase");
DATA(insert OID =  872 (  initcap	   PGNSP PGUID 12 f f f t f i 1 25 "25" 100 0 0 100  initcap - _null_ ));
DESCR("capitalize each word");
DATA(insert OID =  873 (  lpad		   PGNSP PGUID 12 f f f t f i 3 25 "25 23 25" 100 0 0 100  lpad - _null_ ));
DESCR("left-pad string to length");
DATA(insert OID =  874 (  rpad		   PGNSP PGUID 12 f f f t f i 3 25 "25 23 25" 100 0 0 100  rpad - _null_ ));
DESCR("right-pad string to length");
DATA(insert OID =  875 (  ltrim		   PGNSP PGUID 12 f f f t f i 2 25 "25 25" 100 0 0 100	ltrim - _null_ ));
DESCR("left-pad string to length");
DATA(insert OID =  876 (  rtrim		   PGNSP PGUID 12 f f f t f i 2 25 "25 25" 100 0 0 100	rtrim - _null_ ));
DESCR("right-pad string to length");
DATA(insert OID =  877 (  substr	   PGNSP PGUID 12 f f f t f i 3 25 "25 23 23" 100 0 0 100  text_substr - _null_ ));
DESCR("return portion of string");
DATA(insert OID =  878 (  translate    PGNSP PGUID 12 f f f t f i 3 25 "25 25 25" 100 0 0 100  translate - _null_ ));
DESCR("map a set of character appearing in string");
DATA(insert OID =  879 (  lpad		   PGNSP PGUID 14 f f f t f i 2 25 "25 23" 100 0 0 100	"select lpad($1, $2, \' \')" - _null_ ));
DESCR("left-pad string to length");
DATA(insert OID =  880 (  rpad		   PGNSP PGUID 14 f f f t f i 2 25 "25 23" 100 0 0 100	"select rpad($1, $2, \' \')" - _null_ ));
DESCR("right-pad string to length");
DATA(insert OID =  881 (  ltrim		   PGNSP PGUID 14 f f f t f i 1 25 "25" 100 0 0 100  "select ltrim($1, \' \')" - _null_ ));
DESCR("remove initial characters from string");
DATA(insert OID =  882 (  rtrim		   PGNSP PGUID 14 f f f t f i 1 25 "25" 100 0 0 100  "select rtrim($1, \' \')" - _null_ ));
DESCR("remove trailing characters from string");
DATA(insert OID =  883 (  substr	   PGNSP PGUID 14 f f f t f i 2 25 "25 23" 100 0 0 100	"select substr($1, $2, -1)" - _null_ ));
DESCR("return portion of string");
DATA(insert OID =  884 (  btrim		   PGNSP PGUID 12 f f f t f i 2 25 "25 25" 100 0 0 100	btrim - _null_ ));
DESCR("trim both ends of string");
DATA(insert OID =  885 (  btrim		   PGNSP PGUID 14 f f f t f i 1 25 "25" 100 0 0 100  "select btrim($1, \' \')" - _null_ ));
DESCR("trim both ends of string");

DATA(insert OID =  936 (  substring    PGNSP PGUID 12 f f f t f i 3 25 "25 23 23" 100 0 0 100  text_substr - _null_ ));
DESCR("return portion of string");
DATA(insert OID =  937 (  substring    PGNSP PGUID 14 f f f t f i 2 25 "25 23" 100 0 0 100	"select substring($1, $2, -1)" - _null_ ));
DESCR("return portion of string");

/* for multi-byte support */

/* return database encoding name */
DATA(insert OID = 1039 (  getdatabaseencoding	   PGNSP PGUID 12 f f f t f s 0 19 "0" 100 0 0 100	getdatabaseencoding - _null_ ));
DESCR("encoding name of current database");

/* return client encoding name i.e. session encoding */
DATA(insert OID = 810 (  pg_client_encoding    PGNSP PGUID 12 f f f t f s 0 19 "0" 100 0 0 100	pg_client_encoding - _null_ ));
DESCR("encoding name of current database");

DATA(insert OID = 1717 (  convert		   PGNSP PGUID 12 f f f t f s 2 25 "25 19" 100 0 0 100	pg_convert - _null_ ));
DESCR("convert string with specified destination encoding name");

DATA(insert OID = 1813 (  convert		   PGNSP PGUID 12 f f f t f s 3 25 "25 19 19" 100 0 0 100  pg_convert2 - _null_ ));
DESCR("convert string with specified encoding names");

DATA(insert OID = 1264 (  pg_char_to_encoding	   PGNSP PGUID 12 f f f t f s 1 23 "19" 100 0 0 100  PG_char_to_encoding - _null_ ));
DESCR("convert encoding name to encoding id");

DATA(insert OID = 1597 (  pg_encoding_to_char	   PGNSP PGUID 12 f f f t f s 1 19 "23" 100 0 0 100  PG_encoding_to_char - _null_ ));
DESCR("convert encoding id to encoding name");

DATA(insert OID = 1638 (  oidgt				   PGNSP PGUID 12 f f f t f i 2 16 "26 26" 100 0 0 100	oidgt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1639 (  oidge				   PGNSP PGUID 12 f f f t f i 2 16 "26 26" 100 0 0 100	oidge - _null_ ));
DESCR("greater-than-or-equal");

/* System-view support functions */
DATA(insert OID = 1573 (  pg_get_ruledef	   PGNSP PGUID 12 f f f t f s 1 25 "26" 100 0 0 100  pg_get_ruledef - _null_ ));
DESCR("source text of a rule");
DATA(insert OID = 1640 (  pg_get_viewdef	   PGNSP PGUID 12 f f f t f s 1 25 "25" 100 0 0 100  pg_get_viewdef_name - _null_ ));
DESCR("select statement of a view");
DATA(insert OID = 1641 (  pg_get_viewdef	   PGNSP PGUID 12 f f f t f s 1 25 "26" 100 0 0 100  pg_get_viewdef - _null_ ));
DESCR("select statement of a view");
DATA(insert OID = 1642 (  pg_get_userbyid	   PGNSP PGUID 12 f f f t f s 1 19 "23" 100 0 0 100  pg_get_userbyid - _null_ ));
DESCR("user name by UID (with fallback)");
DATA(insert OID = 1643 (  pg_get_indexdef	   PGNSP PGUID 12 f f f t f s 1 25 "26" 100 0 0 100  pg_get_indexdef - _null_ ));
DESCR("index description");
DATA(insert OID = 1716 (  pg_get_expr		   PGNSP PGUID 12 f f f t f s 2 25 "25 26" 100 0 0 100	pg_get_expr - _null_ ));
DESCR("deparse an encoded expression");


/* Generic referential integrity constraint triggers */
DATA(insert OID = 1644 (  RI_FKey_check_ins		PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_check_ins - _null_ ));
DESCR("referential integrity FOREIGN KEY ... REFERENCES");
DATA(insert OID = 1645 (  RI_FKey_check_upd		PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_check_upd - _null_ ));
DESCR("referential integrity FOREIGN KEY ... REFERENCES");
DATA(insert OID = 1646 (  RI_FKey_cascade_del	PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_cascade_del - _null_ ));
DESCR("referential integrity ON DELETE CASCADE");
DATA(insert OID = 1647 (  RI_FKey_cascade_upd	PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_cascade_upd - _null_ ));
DESCR("referential integrity ON UPDATE CASCADE");
DATA(insert OID = 1648 (  RI_FKey_restrict_del	PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_restrict_del - _null_ ));
DESCR("referential integrity ON DELETE RESTRICT");
DATA(insert OID = 1649 (  RI_FKey_restrict_upd	PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_restrict_upd - _null_ ));
DESCR("referential integrity ON UPDATE RESTRICT");
DATA(insert OID = 1650 (  RI_FKey_setnull_del	PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_setnull_del - _null_ ));
DESCR("referential integrity ON DELETE SET NULL");
DATA(insert OID = 1651 (  RI_FKey_setnull_upd	PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_setnull_upd - _null_ ));
DESCR("referential integrity ON UPDATE SET NULL");
DATA(insert OID = 1652 (  RI_FKey_setdefault_del PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100	RI_FKey_setdefault_del - _null_ ));
DESCR("referential integrity ON DELETE SET DEFAULT");
DATA(insert OID = 1653 (  RI_FKey_setdefault_upd PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100	RI_FKey_setdefault_upd - _null_ ));
DESCR("referential integrity ON UPDATE SET DEFAULT");
DATA(insert OID = 1654 (  RI_FKey_noaction_del PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_noaction_del - _null_ ));
DESCR("referential integrity ON DELETE NO ACTION");
DATA(insert OID = 1655 (  RI_FKey_noaction_upd PGNSP PGUID 12 f f f t f v 0 0 "" 100 0 0 100  RI_FKey_noaction_upd - _null_ ));
DESCR("referential integrity ON UPDATE NO ACTION");

DATA(insert OID = 1666 (  varbiteq			PGNSP PGUID 12 f f f t f i 2 16 "1562 1562" 100 0 0 100  biteq - _null_ ));
DESCR("equal");
DATA(insert OID = 1667 (  varbitne			PGNSP PGUID 12 f f f t f i 2 16 "1562 1562" 100 0 0 100  bitne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1668 (  varbitge			PGNSP PGUID 12 f f f t f i 2 16 "1562 1562" 100 0 0 100  bitge - _null_ ));
DESCR("greater than or equal");
DATA(insert OID = 1669 (  varbitgt			PGNSP PGUID 12 f f f t f i 2 16 "1562 1562" 100 0 0 100  bitgt - _null_ ));
DESCR("greater than");
DATA(insert OID = 1670 (  varbitle			PGNSP PGUID 12 f f f t f i 2 16 "1562 1562" 100 0 0 100  bitle - _null_ ));
DESCR("less than or equal");
DATA(insert OID = 1671 (  varbitlt			PGNSP PGUID 12 f f f t f i 2 16 "1562 1562" 100 0 0 100  bitlt - _null_ ));
DESCR("less than");
DATA(insert OID = 1672 (  varbitcmp			PGNSP PGUID 12 f f f t f i 2 23 "1562 1562" 100 0 0 100  bitcmp - _null_ ));
DESCR("compare");

DATA(insert OID = 1673 (  bitand			PGNSP PGUID 12 f f f t f i 2 1560 "1560 1560" 100 0 0 100  bitand - _null_ ));
DESCR("bitwise and");
DATA(insert OID = 1674 (  bitor				PGNSP PGUID 12 f f f t f i 2 1560 "1560 1560" 100 0 0 100  bitor - _null_ ));
DESCR("bitwise or");
DATA(insert OID = 1675 (  bitxor			PGNSP PGUID 12 f f f t f i 2 1560 "1560 1560" 100 0 0 100  bitxor - _null_ ));
DESCR("bitwise exclusive or");
DATA(insert OID = 1676 (  bitnot			PGNSP PGUID 12 f f f t f i 1 1560 "1560" 100 0 0 100  bitnot - _null_ ));
DESCR("bitwise negation");
DATA(insert OID = 1677 (  bitshiftleft		PGNSP PGUID 12 f f f t f i 2 1560 "1560 23" 100 0 0 100  bitshiftleft - _null_ ));
DESCR("bitwise left shift");
DATA(insert OID = 1678 (  bitshiftright		PGNSP PGUID 12 f f f t f i 2 1560 "1560 23" 100 0 0 100  bitshiftright - _null_ ));
DESCR("bitwise right shift");
DATA(insert OID = 1679 (  bitcat			PGNSP PGUID 12 f f f t f i 2 1560 "1560 1560" 100 0 0 100  bitcat - _null_ ));
DESCR("bitwise concatenation");
DATA(insert OID = 1680 (  substring			PGNSP PGUID 12 f f f t f i 3 1560 "1560 23 23" 100 0 0 100	bitsubstr - _null_ ));
DESCR("return portion of bitstring");
DATA(insert OID = 1681 (  length			PGNSP PGUID 12 f f f t f i 1 23 "1560" 100 0 0 100	bitlength - _null_ ));
DESCR("bitstring length");
DATA(insert OID = 1682 (  octet_length		PGNSP PGUID 12 f f f t f i 1 23 "1560" 100 0 0 100	bitoctetlength - _null_ ));
DESCR("octet length");
DATA(insert OID = 1683 (  bitfromint4		PGNSP PGUID 12 f f f t f i 1 1560 "23" 100 0 0 100	bitfromint4 - _null_ ));
DESCR("int4 to bitstring");
DATA(insert OID = 1684 (  bittoint4			PGNSP PGUID 12 f f f t f i 1 23 "1560" 100 0 0 100	bittoint4 - _null_ ));
DESCR("bitstring to int4");

DATA(insert OID = 1685 (  bit			   PGNSP PGUID 12 f f t t f i 2 1560 "1560 23" 100 0 0 100	bit - _null_ ));
DESCR("adjust bit() to typmod length");
DATA(insert OID = 1686 (  _bit			   PGNSP PGUID 12 f f t t f i 2 1561 "1561 23" 100 0 0 100	_bit - _null_ ));
DESCR("adjust bit()[] to typmod length");
DATA(insert OID = 1687 (  varbit		   PGNSP PGUID 12 f f t t f i 2 1562 "1562 23" 100 0 0 100	varbit - _null_ ));
DESCR("adjust varbit() to typmod length");
DATA(insert OID = 1688 (  _varbit		   PGNSP PGUID 12 f f t t f i 2 1563 "1563 23" 100 0 0 100	_varbit - _null_ ));
DESCR("adjust varbit()[] to typmod length");

DATA(insert OID = 1698 (  position		   PGNSP PGUID 12 f f f t f i 2 23 "1560 1560" 100 0 0 100 bitposition - _null_ ));
DESCR("return position of sub-bitstring");
DATA(insert OID = 1699 (  substring			PGNSP PGUID 14 f f f t f i 2 1560 "1560 23" 100 0 0 100  "select substring($1, $2, -1)" - _null_ ));
DESCR("return portion of bitstring");


/* for mac type support */
DATA(insert OID = 436 (  macaddr_in			PGNSP PGUID 12 f f f t f i 1 829 "0" 100 0 0 100  macaddr_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 437 (  macaddr_out		PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100  macaddr_out - _null_ ));
DESCR("(internal)");

DATA(insert OID = 752 (  text				PGNSP PGUID 12 f f f t f i 1 25 "829" 100 0 0 100  macaddr_text - _null_ ));
DESCR("MAC address to text");
DATA(insert OID = 753 (  trunc				PGNSP PGUID 12 f f f t f i 1 829 "829" 100 0 0 100	macaddr_trunc - _null_ ));
DESCR("MAC manufacturer fields");
DATA(insert OID = 767 (  macaddr			PGNSP PGUID 12 f f f t f i 1 829 "25" 100 0 0 100  text_macaddr - _null_ ));
DESCR("text to MAC address");

DATA(insert OID = 830 (  macaddr_eq			PGNSP PGUID 12 f f f t f i 2 16 "829 829" 100 0 0 100  macaddr_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 831 (  macaddr_lt			PGNSP PGUID 12 f f f t f i 2 16 "829 829" 100 0 0 100  macaddr_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 832 (  macaddr_le			PGNSP PGUID 12 f f f t f i 2 16 "829 829" 100 0 0 100  macaddr_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 833 (  macaddr_gt			PGNSP PGUID 12 f f f t f i 2 16 "829 829" 100 0 0 100  macaddr_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 834 (  macaddr_ge			PGNSP PGUID 12 f f f t f i 2 16 "829 829" 100 0 0 100  macaddr_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 835 (  macaddr_ne			PGNSP PGUID 12 f f f t f i 2 16 "829 829" 100 0 0 100  macaddr_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 836 (  macaddr_cmp		PGNSP PGUID 12 f f f t f i 2 23 "829 829" 100 0 0 100  macaddr_cmp - _null_ ));
DESCR("less-equal-greater");

/* for inet type support */
DATA(insert OID = 910 (  inet_in			PGNSP PGUID 12 f f f t f i 1 869 "0" 100 0 0 100  inet_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 911 (  inet_out			PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100  inet_out - _null_ ));
DESCR("(internal)");

/* for cidr type support */
DATA(insert OID = 1267 (  cidr_in			PGNSP PGUID 12 f f f t f i 1 650 "0" 100 0 0 100  cidr_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1427 (  cidr_out			PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100  cidr_out - _null_ ));
DESCR("(internal)");

/* these are used for both inet and cidr */
DATA(insert OID = 920 (  network_eq			PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 921 (  network_lt			PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 922 (  network_le			PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 923 (  network_gt			PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 924 (  network_ge			PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 925 (  network_ne			PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 926 (  network_cmp		PGNSP PGUID 12 f f f t f i 2 23 "869 869" 100 0 0 100  network_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 927 (  network_sub		PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_sub - _null_ ));
DESCR("is-subnet");
DATA(insert OID = 928 (  network_subeq		PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_subeq - _null_ ));
DESCR("is-subnet-or-equal");
DATA(insert OID = 929 (  network_sup		PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_sup - _null_ ));
DESCR("is-supernet");
DATA(insert OID = 930 (  network_supeq		PGNSP PGUID 12 f f f t f i 2 16 "869 869" 100 0 0 100  network_supeq - _null_ ));
DESCR("is-supernet-or-equal");

/* inet/cidr functions */
DATA(insert OID = 605 (  abbrev				PGNSP PGUID 12 f f f t f i 1 25 "869" 100 0 0 100  network_abbrev - _null_ ));
DESCR("abbreviated display of inet/cidr value");
DATA(insert OID = 683 (  network			PGNSP PGUID 12 f f f t f i 1 650 "869" 100 0 0 100	network_network - _null_ ));
DESCR("network part of address");
DATA(insert OID = 696 (  netmask			PGNSP PGUID 12 f f f t f i 1 869 "869" 100 0 0 100	network_netmask - _null_ ));
DESCR("netmask of address");
DATA(insert OID = 697 (  masklen			PGNSP PGUID 12 f f f t f i 1 23 "869" 100 0 0 100  network_masklen - _null_ ));
DESCR("netmask length");
DATA(insert OID = 698 (  broadcast			PGNSP PGUID 12 f f f t f i 1 869 "869" 100 0 0 100	network_broadcast - _null_ ));
DESCR("broadcast address of network");
DATA(insert OID = 699 (  host				PGNSP PGUID 12 f f f t f i 1 25 "869" 100 0 0 100  network_host - _null_ ));
DESCR("show address octets only");
DATA(insert OID = 730 (  text				PGNSP PGUID 12 f f f t f i 1 25 "869" 100 0 0 100  network_show - _null_ ));
DESCR("show all parts of inet/cidr value");
DATA(insert OID = 1713 (  inet				PGNSP PGUID 12 f f f t f i 1 869 "25" 100 0 0 100  text_inet - _null_ ));
DESCR("text to inet");
DATA(insert OID = 1714 (  cidr				PGNSP PGUID 12 f f f t f i 1 650 "25" 100 0 0 100  text_cidr - _null_ ));
DESCR("text to cidr");
DATA(insert OID = 1715 (  set_masklen		PGNSP PGUID 12 f f f t f i 2 869 "869 23" 100 0 0 100  inet_set_masklen - _null_ ));
DESCR("change the netmask of an inet");

DATA(insert OID = 1690 ( time_mi_time		PGNSP PGUID 12 f f f t f i 2 1186 "1083 1083" 100 0 0 100  time_mi_time - _null_ ));
DESCR("minus");

DATA(insert OID =  1691 (  boolle			PGNSP PGUID 12 f f f t f i 2 16 "16 16" 100 0 0 100  boolle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID =  1692 (  boolge			PGNSP PGUID 12 f f f t f i 2 16 "16 16" 100 0 0 100  boolge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1693 (  btboolcmp			PGNSP PGUID 12 f f f t f i 2 23 "16 16" 100 0 0 100  btboolcmp - _null_ ));
DESCR("btree less-equal-greater");

DATA(insert OID = 1696 (  timetz_hash		PGNSP PGUID 12 f f f t f i 1 23 "1266" 100 0 0 100	timetz_hash - _null_ ));
DESCR("hash");
DATA(insert OID = 1697 (  interval_hash		PGNSP PGUID 12 f f f t f i 1 23 "1186" 100 0 0 100	interval_hash - _null_ ));
DESCR("hash");


/* OID's 1700 - 1799 NUMERIC data type */
DATA(insert OID = 1701 ( numeric_in				PGNSP PGUID 12 f f f t f i 3 1700 "0 26 23" 100 0 0 100  numeric_in - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1702 ( numeric_out			PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100  numeric_out - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1703 ( numeric				PGNSP PGUID 12 f f t t f i 2 1700 "1700 23" 100 0 0 100  numeric - _null_ ));
DESCR("adjust numeric to typmod precision/scale");
DATA(insert OID = 1704 ( numeric_abs			PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1705 ( abs					PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1706 ( sign					PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_sign - _null_ ));
DESCR("sign of value");
DATA(insert OID = 1707 ( round					PGNSP PGUID 12 f f f t f i 2 1700 "1700 23" 100 0 0 100  numeric_round - _null_ ));
DESCR("value rounded to 'scale'");
DATA(insert OID = 1708 ( round					PGNSP PGUID 14 f f f t f i 1 1700 "1700" 100 0 0 100  "select round($1,0)" - _null_ ));
DESCR("value rounded to 'scale' of zero");
DATA(insert OID = 1709 ( trunc					PGNSP PGUID 12 f f f t f i 2 1700 "1700 23" 100 0 0 100  numeric_trunc - _null_ ));
DESCR("value truncated to 'scale'");
DATA(insert OID = 1710 ( trunc					PGNSP PGUID 14 f f f t f i 1 1700 "1700" 100 0 0 100  "select trunc($1,0)" - _null_ ));
DESCR("value truncated to 'scale' of zero");
DATA(insert OID = 1711 ( ceil					PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_ceil - _null_ ));
DESCR("smallest integer >= value");
DATA(insert OID = 1712 ( floor					PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_floor - _null_ ));
DESCR("largest integer <= value");
DATA(insert OID = 1718 ( numeric_eq				PGNSP PGUID 12 f f f t f i 2 16 "1700 1700" 100 0 0 100  numeric_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1719 ( numeric_ne				PGNSP PGUID 12 f f f t f i 2 16 "1700 1700" 100 0 0 100  numeric_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1720 ( numeric_gt				PGNSP PGUID 12 f f f t f i 2 16 "1700 1700" 100 0 0 100  numeric_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1721 ( numeric_ge				PGNSP PGUID 12 f f f t f i 2 16 "1700 1700" 100 0 0 100  numeric_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1722 ( numeric_lt				PGNSP PGUID 12 f f f t f i 2 16 "1700 1700" 100 0 0 100  numeric_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1723 ( numeric_le				PGNSP PGUID 12 f f f t f i 2 16 "1700 1700" 100 0 0 100  numeric_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1724 ( numeric_add			PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_add - _null_ ));
DESCR("add");
DATA(insert OID = 1725 ( numeric_sub			PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_sub - _null_ ));
DESCR("subtract");
DATA(insert OID = 1726 ( numeric_mul			PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 1727 ( numeric_div			PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_div - _null_ ));
DESCR("divide");
DATA(insert OID = 1728 ( mod					PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 1729 ( numeric_mod			PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 1730 ( sqrt					PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_sqrt - _null_ ));
DESCR("square root");
DATA(insert OID = 1731 ( numeric_sqrt			PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_sqrt - _null_ ));
DESCR("square root");
DATA(insert OID = 1732 ( exp					PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_exp - _null_ ));
DESCR("e raised to the power of n");
DATA(insert OID = 1733 ( numeric_exp			PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_exp - _null_ ));
DESCR("e raised to the power of n");
DATA(insert OID = 1734 ( ln						PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_ln - _null_ ));
DESCR("natural logarithm of n");
DATA(insert OID = 1735 ( numeric_ln				PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_ln - _null_ ));
DESCR("natural logarithm of n");
DATA(insert OID = 1736 ( log					PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_log - _null_ ));
DESCR("logarithm base m of n");
DATA(insert OID = 1737 ( numeric_log			PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_log - _null_ ));
DESCR("logarithm base m of n");
DATA(insert OID = 1738 ( pow					PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_power - _null_ ));
DESCR("m raised to the power of n");
DATA(insert OID = 1739 ( numeric_power			PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_power - _null_ ));
DESCR("m raised to the power of n");
DATA(insert OID = 1740 ( numeric				PGNSP PGUID 12 f f t t f i 1 1700 "23" 100 0 0 100	int4_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1741 ( log					PGNSP PGUID 14 f f f t f i 1 1700 "1700" 100 0 0 100  "select log(10, $1)" - _null_ ));
DESCR("logarithm base 10 of n");
DATA(insert OID = 1742 ( numeric				PGNSP PGUID 12 f f t t f i 1 1700 "700" 100 0 0 100  float4_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1743 ( numeric				PGNSP PGUID 12 f f t t f i 1 1700 "701" 100 0 0 100  float8_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1744 ( int4					PGNSP PGUID 12 f f f t f i 1 23 "1700" 100 0 0 100	numeric_int4 - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1745 ( float4					PGNSP PGUID 12 f f f t f i 1 700 "1700" 100 0 0 100  numeric_float4 - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1746 ( float8					PGNSP PGUID 12 f f f t f i 1 701 "1700" 100 0 0 100  numeric_float8 - _null_ ));
DESCR("(internal)");

DATA(insert OID = 1747 ( time_pl_interval		PGNSP PGUID 12 f f f t f i 2 1083 "1083 1186" 100 0 0 100  time_pl_interval - _null_ ));
DESCR("plus");
DATA(insert OID = 1748 ( time_mi_interval		PGNSP PGUID 12 f f f t f i 2 1083 "1083 1186" 100 0 0 100  time_mi_interval - _null_ ));
DESCR("minus");
DATA(insert OID = 1749 ( timetz_pl_interval		PGNSP PGUID 12 f f f t f i 2 1266 "1266 1186" 100 0 0 100  timetz_pl_interval - _null_ ));
DESCR("plus");
DATA(insert OID = 1750 ( timetz_mi_interval		PGNSP PGUID 12 f f f t f i 2 1266 "1266 1186" 100 0 0 100  timetz_mi_interval - _null_ ));
DESCR("minus");

DATA(insert OID = 1764 ( numeric_inc			PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_inc - _null_ ));
DESCR("increment by one");
DATA(insert OID = 1766 ( numeric_smaller		PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_smaller - _null_ ));
DESCR("smaller of two numbers");
DATA(insert OID = 1767 ( numeric_larger			PGNSP PGUID 12 f f f t f i 2 1700 "1700 1700" 100 0 0 100  numeric_larger - _null_ ));
DESCR("larger of two numbers");
DATA(insert OID = 1769 ( numeric_cmp			PGNSP PGUID 12 f f f t f i 2 23 "1700 1700" 100 0 0 100  numeric_cmp - _null_ ));
DESCR("compare two numbers");
DATA(insert OID = 1771 ( numeric_uminus			PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_uminus - _null_ ));
DESCR("negate");
DATA(insert OID = 1779 ( int8					PGNSP PGUID 12 f f f t f i 1 20 "1700" 100 0 0 100	numeric_int8 - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1781 ( numeric				PGNSP PGUID 12 f f t t f i 1 1700 "20" 100 0 0 100	int8_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1782 ( numeric				PGNSP PGUID 12 f f t t f i 1 1700 "21" 100 0 0 100	int2_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1783 ( int2					PGNSP PGUID 12 f f f t f i 1 21 "1700" 100 0 0 100	numeric_int2 - _null_ ));
DESCR("(internal)");

/* formatting */
DATA(insert OID = 1770 ( to_char			PGNSP PGUID 12 f f f t f s 2	25 "1184 25" 100 0 0 100  timestamptz_to_char - _null_ ));
DESCR("format timestamp with time zone to text");
DATA(insert OID = 1772 ( to_char			PGNSP PGUID 12 f f f t f i 2	25 "1700 25" 100 0 0 100  numeric_to_char - _null_ ));
DESCR("format numeric to text");
DATA(insert OID = 1773 ( to_char			PGNSP PGUID 12 f f f t f i 2	25 "23 25" 100 0 0 100	int4_to_char - _null_ ));
DESCR("format int4 to text");
DATA(insert OID = 1774 ( to_char			PGNSP PGUID 12 f f f t f i 2	25 "20 25" 100 0 0 100	int8_to_char - _null_ ));
DESCR("format int8 to text");
DATA(insert OID = 1775 ( to_char			PGNSP PGUID 12 f f f t f i 2	25 "700 25" 100 0 0 100  float4_to_char - _null_ ));
DESCR("format float4 to text");
DATA(insert OID = 1776 ( to_char			PGNSP PGUID 12 f f f t f i 2	25 "701 25" 100 0 0 100  float8_to_char - _null_ ));
DESCR("format float8 to text");
DATA(insert OID = 1777 ( to_number			PGNSP PGUID 12 f f f t f i 2	1700 "25 25" 100 0 0 100  numeric_to_number - _null_ ));
DESCR("convert text to numeric");
DATA(insert OID = 1778 ( to_timestamp		PGNSP PGUID 12 f f f t f s 2	1184 "25 25" 100 0 0 100  to_timestamp - _null_ ));
DESCR("convert text to timestamp");
DATA(insert OID = 1780 ( to_date			PGNSP PGUID 12 f f f t f i 2	1082 "25 25" 100 0 0 100  to_date - _null_ ));
DESCR("convert text to date");
DATA(insert OID = 1768 ( to_char			PGNSP PGUID 12 f f f t f i 2	25 "1186 25" 100 0 0 100  interval_to_char - _null_ ));
DESCR("format interval to text");

DATA(insert OID =  1282 ( quote_ident	   PGNSP PGUID 12 f f f t f i 1 25 "25" 100 0 0 100 quote_ident - _null_ ));
DESCR("quote an identifier for usage in a querystring");
DATA(insert OID =  1283 ( quote_literal    PGNSP PGUID 12 f f f t f i 1 25 "25" 100 0 0 100 quote_literal - _null_ ));
DESCR("quote a literal for usage in a querystring");

DATA(insert OID = 1798 (  oidin			   PGNSP PGUID 12 f f f t f i 1 26 "0" 100 0 0 100	oidin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1799 (  oidout		   PGNSP PGUID 12 f f f t f i 1 23 "0" 100 0 0 100	oidout - _null_ ));
DESCR("(internal)");


DATA(insert OID = 1810 (  bit_length	   PGNSP PGUID 14 f f f t f i 1 23 "17" 100 0 0 100 "select octet_length($1) * 8" - _null_ ));
DESCR("length in bits");
DATA(insert OID = 1811 (  bit_length	   PGNSP PGUID 14 f f f t f i 1 23 "25" 100 0 0 100 "select octet_length($1) * 8" - _null_ ));
DESCR("length in bits");
DATA(insert OID = 1812 (  bit_length	   PGNSP PGUID 14 f f f t f i 1 23 "1560" 100 0 0 100 "select length($1)" - _null_ ));
DESCR("length in bits");

/* Selectivity estimators for LIKE and related operators */
DATA(insert OID = 1814 ( iclikesel			PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  iclikesel - _null_ ));
DESCR("restriction selectivity of ILIKE");
DATA(insert OID = 1815 ( icnlikesel			PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  icnlikesel - _null_ ));
DESCR("restriction selectivity of NOT ILIKE");
DATA(insert OID = 1816 ( iclikejoinsel		PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  iclikejoinsel - _null_ ));
DESCR("join selectivity of ILIKE");
DATA(insert OID = 1817 ( icnlikejoinsel		PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  icnlikejoinsel - _null_ ));
DESCR("join selectivity of NOT ILIKE");
DATA(insert OID = 1818 ( regexeqsel			PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  regexeqsel - _null_ ));
DESCR("restriction selectivity of regex match");
DATA(insert OID = 1819 ( likesel			PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  likesel - _null_ ));
DESCR("restriction selectivity of LIKE");
DATA(insert OID = 1820 ( icregexeqsel		PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  icregexeqsel - _null_ ));
DESCR("restriction selectivity of case-insensitive regex match");
DATA(insert OID = 1821 ( regexnesel			PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  regexnesel - _null_ ));
DESCR("restriction selectivity of regex non-match");
DATA(insert OID = 1822 ( nlikesel			PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  nlikesel - _null_ ));
DESCR("restriction selectivity of NOT LIKE");
DATA(insert OID = 1823 ( icregexnesel		PGNSP PGUID 12 f f f t f s 4 701 "0 26 0 23" 100 0 0 100  icregexnesel - _null_ ));
DESCR("restriction selectivity of case-insensitive regex non-match");
DATA(insert OID = 1824 ( regexeqjoinsel		PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  regexeqjoinsel - _null_ ));
DESCR("join selectivity of regex match");
DATA(insert OID = 1825 ( likejoinsel		PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  likejoinsel - _null_ ));
DESCR("join selectivity of LIKE");
DATA(insert OID = 1826 ( icregexeqjoinsel	PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  icregexeqjoinsel - _null_ ));
DESCR("join selectivity of case-insensitive regex match");
DATA(insert OID = 1827 ( regexnejoinsel		PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  regexnejoinsel - _null_ ));
DESCR("join selectivity of regex non-match");
DATA(insert OID = 1828 ( nlikejoinsel		PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  nlikejoinsel - _null_ ));
DESCR("join selectivity of NOT LIKE");
DATA(insert OID = 1829 ( icregexnejoinsel	PGNSP PGUID 12 f f f t f s 3 701 "0 26 0" 100 0 0 100  icregexnejoinsel - _null_ ));
DESCR("join selectivity of case-insensitive regex non-match");

/* Aggregate-related functions */
DATA(insert OID = 1830 (  float8_avg	   PGNSP PGUID 12 f f f t f i 1 701 "1022" 100 0 0 100	float8_avg - _null_ ));
DESCR("AVG aggregate final function");
DATA(insert OID = 1831 (  float8_variance  PGNSP PGUID 12 f f f t f i 1 701 "1022" 100 0 0 100	float8_variance - _null_ ));
DESCR("VARIANCE aggregate final function");
DATA(insert OID = 1832 (  float8_stddev    PGNSP PGUID 12 f f f t f i 1 701 "1022" 100 0 0 100	float8_stddev - _null_ ));
DESCR("STDDEV aggregate final function");
DATA(insert OID = 1833 (  numeric_accum    PGNSP PGUID 12 f f f t f i 2 1231 "1231 1700" 100 0 0 100  numeric_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1834 (  int2_accum	   PGNSP PGUID 12 f f f t f i 2 1231 "1231 21" 100 0 0 100	int2_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1835 (  int4_accum	   PGNSP PGUID 12 f f f t f i 2 1231 "1231 23" 100 0 0 100	int4_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1836 (  int8_accum	   PGNSP PGUID 12 f f f t f i 2 1231 "1231 20" 100 0 0 100	int8_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1837 (  numeric_avg	   PGNSP PGUID 12 f f f t f i 1 1700 "1231" 100 0 0 100  numeric_avg - _null_ ));
DESCR("AVG aggregate final function");
DATA(insert OID = 1838 (  numeric_variance PGNSP PGUID 12 f f f t f i 1 1700 "1231" 100 0 0 100  numeric_variance - _null_ ));
DESCR("VARIANCE aggregate final function");
DATA(insert OID = 1839 (  numeric_stddev   PGNSP PGUID 12 f f f t f i 1 1700 "1231" 100 0 0 100  numeric_stddev - _null_ ));
DESCR("STDDEV aggregate final function");
DATA(insert OID = 1840 (  int2_sum		   PGNSP PGUID 12 f f f f f i 2 20 "20 21" 100 0 0 100	int2_sum - _null_ ));
DESCR("SUM(int2) transition function");
DATA(insert OID = 1841 (  int4_sum		   PGNSP PGUID 12 f f f f f i 2 20 "20 23" 100 0 0 100	int4_sum - _null_ ));
DESCR("SUM(int4) transition function");
DATA(insert OID = 1842 (  int8_sum		   PGNSP PGUID 12 f f f f f i 2 1700 "1700 20" 100 0 0 100	int8_sum - _null_ ));
DESCR("SUM(int8) transition function");
DATA(insert OID = 1843 (  interval_accum   PGNSP PGUID 12 f f f t f i 2 1187 "1187 1186" 100 0 0 100  interval_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1844 (  interval_avg	   PGNSP PGUID 12 f f f t f i 1 1186 "1187" 100 0 0 100  interval_avg - _null_ ));
DESCR("AVG aggregate final function");
DATA(insert OID = 1962 (  int2_avg_accum   PGNSP PGUID 12 f f f t f i 2 1016 "1016 21" 100 0 0 100	int2_avg_accum - _null_ ));
DESCR("AVG(int2) transition function");
DATA(insert OID = 1963 (  int4_avg_accum   PGNSP PGUID 12 f f f t f i 2 1016 "1016 23" 100 0 0 100	int4_avg_accum - _null_ ));
DESCR("AVG(int4) transition function");
DATA(insert OID = 1964 (  int8_avg		   PGNSP PGUID 12 f f f t f i 1 1700 "1016" 100 0 0 100  int8_avg - _null_ ));
DESCR("AVG(int) aggregate final function");

/* To ASCII conversion */
DATA(insert OID = 1845 ( to_ascii	PGNSP PGUID 12 f f f t f i 1	25 "25" 100 0 0 100  to_ascii_default - _null_ ));
DESCR("encode text from DB encoding to ASCII text");
DATA(insert OID = 1846 ( to_ascii	PGNSP PGUID 12 f f f t f i 2	25 "25 23" 100 0 0 100	to_ascii_enc - _null_ ));
DESCR("encode text from encoding to ASCII text");
DATA(insert OID = 1847 ( to_ascii	PGNSP PGUID 12 f f f t f i 2	25 "25 19" 100 0 0 100	to_ascii_encname - _null_ ));
DESCR("encode text from encoding to ASCII text");

DATA(insert OID = 1848 ( interval_pl_time		PGNSP PGUID 12 f f f t f i 2 1083 "1186 1083" 100 0 0 100  interval_pl_time - _null_ ));
DESCR("plus");

DATA(insert OID = 1850 (  int28eq		   PGNSP PGUID 12 f f f t f i 2 16 "21 20" 100 0 0 100	int28eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1851 (  int28ne		   PGNSP PGUID 12 f f f t f i 2 16 "21 20" 100 0 0 100	int28ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1852 (  int28lt		   PGNSP PGUID 12 f f f t f i 2 16 "21 20" 100 0 0 100	int28lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1853 (  int28gt		   PGNSP PGUID 12 f f f t f i 2 16 "21 20" 100 0 0 100	int28gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1854 (  int28le		   PGNSP PGUID 12 f f f t f i 2 16 "21 20" 100 0 0 100	int28le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1855 (  int28ge		   PGNSP PGUID 12 f f f t f i 2 16 "21 20" 100 0 0 100	int28ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 1856 (  int82eq		   PGNSP PGUID 12 f f f t f i 2 16 "20 21" 100 0 0 100	int82eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1857 (  int82ne		   PGNSP PGUID 12 f f f t f i 2 16 "20 21" 100 0 0 100	int82ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1858 (  int82lt		   PGNSP PGUID 12 f f f t f i 2 16 "20 21" 100 0 0 100	int82lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1859 (  int82gt		   PGNSP PGUID 12 f f f t f i 2 16 "20 21" 100 0 0 100	int82gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1860 (  int82le		   PGNSP PGUID 12 f f f t f i 2 16 "20 21" 100 0 0 100	int82le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1861 (  int82ge		   PGNSP PGUID 12 f f f t f i 2 16 "20 21" 100 0 0 100	int82ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 1892 (  int2and		   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2and - _null_ ));
DESCR("binary and");
DATA(insert OID = 1893 (  int2or		   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2or - _null_ ));
DESCR("binary or");
DATA(insert OID = 1894 (  int2xor		   PGNSP PGUID 12 f f f t f i 2 21 "21 21" 100 0 0 100	int2xor - _null_ ));
DESCR("binary xor");
DATA(insert OID = 1895 (  int2not		   PGNSP PGUID 12 f f f t f i 1 21 "21" 100 0 0 100  int2not - _null_ ));
DESCR("binary not");
DATA(insert OID = 1896 (  int2shl		   PGNSP PGUID 12 f f f t f i 2 21 "21 23" 100 0 0 100	int2shl - _null_ ));
DESCR("binary shift left");
DATA(insert OID = 1897 (  int2shr		   PGNSP PGUID 12 f f f t f i 2 21 "21 23" 100 0 0 100	int2shr - _null_ ));
DESCR("binary shift right");

DATA(insert OID = 1898 (  int4and		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4and - _null_ ));
DESCR("binary and");
DATA(insert OID = 1899 (  int4or		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4or - _null_ ));
DESCR("binary or");
DATA(insert OID = 1900 (  int4xor		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4xor - _null_ ));
DESCR("binary xor");
DATA(insert OID = 1901 (  int4not		   PGNSP PGUID 12 f f f t f i 1 23 "23" 100 0 0 100  int4not - _null_ ));
DESCR("binary not");
DATA(insert OID = 1902 (  int4shl		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4shl - _null_ ));
DESCR("binary shift left");
DATA(insert OID = 1903 (  int4shr		   PGNSP PGUID 12 f f f t f i 2 23 "23 23" 100 0 0 100	int4shr - _null_ ));
DESCR("binary shift right");

DATA(insert OID = 1904 (  int8and		   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8and - _null_ ));
DESCR("binary and");
DATA(insert OID = 1905 (  int8or		   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8or - _null_ ));
DESCR("binary or");
DATA(insert OID = 1906 (  int8xor		   PGNSP PGUID 12 f f f t f i 2 20 "20 20" 100 0 0 100	int8xor - _null_ ));
DESCR("binary xor");
DATA(insert OID = 1907 (  int8not		   PGNSP PGUID 12 f f f t f i 1 20 "20" 100 0 0 100  int8not - _null_ ));
DESCR("binary not");
DATA(insert OID = 1908 (  int8shl		   PGNSP PGUID 12 f f f t f i 2 20 "20 23" 100 0 0 100	int8shl - _null_ ));
DESCR("binary shift left");
DATA(insert OID = 1909 (  int8shr		   PGNSP PGUID 12 f f f t f i 2 20 "20 23" 100 0 0 100	int8shr - _null_ ));
DESCR("binary shift right");

DATA(insert OID = 1910 (  int8up		   PGNSP PGUID 12 f f f t f i 1 20	"20"   100 0 0 100	int8up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1911 (  int2up		   PGNSP PGUID 12 f f f t f i 1 21	"21"   100 0 0 100	int2up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1912 (  int4up		   PGNSP PGUID 12 f f f t f i 1 23	"23"   100 0 0 100	int4up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1913 (  float4up		   PGNSP PGUID 12 f f f t f i 1 700 "700"  100 0 0 100	float4up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1914 (  float8up		   PGNSP PGUID 12 f f f t f i 1 701 "701"  100 0 0 100	float8up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1915 (  numeric_uplus    PGNSP PGUID 12 f f f t f i 1 1700 "1700" 100 0 0 100  numeric_uplus - _null_ ));
DESCR("unary plus");

DATA(insert OID = 1922 (  has_table_privilege		   PGNSP PGUID 12 f f f t f s 3 16 "19 25 25" 100 0 0 100  has_table_privilege_name_name - _null_ ));
DESCR("user privilege on relation by username, relname");
DATA(insert OID = 1923 (  has_table_privilege		   PGNSP PGUID 12 f f f t f s 3 16 "19 26 25" 100 0 0 100  has_table_privilege_name_id - _null_ ));
DESCR("user privilege on relation by username, rel oid");
DATA(insert OID = 1924 (  has_table_privilege		   PGNSP PGUID 12 f f f t f s 3 16 "23 25 25" 100 0 0 100  has_table_privilege_id_name - _null_ ));
DESCR("user privilege on relation by usesysid, relname");
DATA(insert OID = 1925 (  has_table_privilege		   PGNSP PGUID 12 f f f t f s 3 16 "23 26 25" 100 0 0 100  has_table_privilege_id_id - _null_ ));
DESCR("user privilege on relation by usesysid, rel oid");
DATA(insert OID = 1926 (  has_table_privilege		   PGNSP PGUID 12 f f f t f s 2 16 "25 25" 100 0 0 100	has_table_privilege_name - _null_ ));
DESCR("current user privilege on relation by relname");
DATA(insert OID = 1927 (  has_table_privilege		   PGNSP PGUID 12 f f f t f s 2 16 "26 25" 100 0 0 100	has_table_privilege_id - _null_ ));
DESCR("current user privilege on relation by rel oid");


DATA(insert OID = 1928 (  pg_stat_get_numscans			PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_numscans - _null_ ));
DESCR("Statistics: Number of scans done for table/index");
DATA(insert OID = 1929 (  pg_stat_get_tuples_returned	PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_tuples_returned - _null_ ));
DESCR("Statistics: Number of tuples read by seqscan");
DATA(insert OID = 1930 (  pg_stat_get_tuples_fetched	PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_tuples_fetched - _null_ ));
DESCR("Statistics: Number of tuples fetched by idxscan");
DATA(insert OID = 1931 (  pg_stat_get_tuples_inserted	PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_tuples_inserted - _null_ ));
DESCR("Statistics: Number of tuples inserted");
DATA(insert OID = 1932 (  pg_stat_get_tuples_updated	PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_tuples_updated - _null_ ));
DESCR("Statistics: Number of tuples updated");
DATA(insert OID = 1933 (  pg_stat_get_tuples_deleted	PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_tuples_deleted - _null_ ));
DESCR("Statistics: Number of tuples deleted");
DATA(insert OID = 1934 (  pg_stat_get_blocks_fetched	PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_blocks_fetched - _null_ ));
DESCR("Statistics: Number of blocks fetched");
DATA(insert OID = 1935 (  pg_stat_get_blocks_hit		PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_blocks_hit - _null_ ));
DESCR("Statistics: Number of blocks found in cache");
DATA(insert OID = 1936 (  pg_stat_get_backend_idset		PGNSP PGUID 12 f f f t t s 0 23 "" 100 0 0 100	pg_stat_get_backend_idset - _null_ ));
DESCR("Statistics: Currently active backend IDs");
DATA(insert OID = 1937 (  pg_stat_get_backend_pid		PGNSP PGUID 12 f f f t f s 1 23 "23" 100 0 0 100  pg_stat_get_backend_pid - _null_ ));
DESCR("Statistics: PID of backend");
DATA(insert OID = 1938 (  pg_stat_get_backend_dbid		PGNSP PGUID 12 f f f t f s 1 26 "23" 100 0 0 100  pg_stat_get_backend_dbid - _null_ ));
DESCR("Statistics: Database ID of backend");
DATA(insert OID = 1939 (  pg_stat_get_backend_userid	PGNSP PGUID 12 f f f t f s 1 26 "23" 100 0 0 100  pg_stat_get_backend_userid - _null_ ));
DESCR("Statistics: User ID of backend");
DATA(insert OID = 1940 (  pg_stat_get_backend_activity	PGNSP PGUID 12 f f f t f s 1 25 "23" 100 0 0 100  pg_stat_get_backend_activity - _null_ ));
DESCR("Statistics: Current query of backend");
DATA(insert OID = 1941 (  pg_stat_get_db_numbackends	PGNSP PGUID 12 f f f t f s 1 23 "26" 100 0 0 100  pg_stat_get_db_numbackends - _null_ ));
DESCR("Statistics: Number of backends in database");
DATA(insert OID = 1942 (  pg_stat_get_db_xact_commit	PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_db_xact_commit - _null_ ));
DESCR("Statistics: Transactions committed");
DATA(insert OID = 1943 (  pg_stat_get_db_xact_rollback	PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_db_xact_rollback - _null_ ));
DESCR("Statistics: Transactions rolled back");
DATA(insert OID = 1944 (  pg_stat_get_db_blocks_fetched PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_db_blocks_fetched - _null_ ));
DESCR("Statistics: Blocks fetched for database");
DATA(insert OID = 1945 (  pg_stat_get_db_blocks_hit		PGNSP PGUID 12 f f f t f s 1 20 "26" 100 0 0 100  pg_stat_get_db_blocks_hit - _null_ ));
DESCR("Statistics: Block found in cache for database");

DATA(insert OID = 1946 (  encode						PGNSP PGUID 12 f f f t f i 2 25 "17 25" 100 0 0 100  binary_encode - _null_ ));
DESCR("Convert bytea value into some ascii-only text string");
DATA(insert OID = 1947 (  decode						PGNSP PGUID 12 f f f t f i 2 17 "25 25" 100 0 0 100  binary_decode - _null_ ));
DESCR("Convert ascii-encoded text string into bytea value");

DATA(insert OID = 1948 (  byteaeq		   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100	byteaeq - _null_ ));
DESCR("equal");
DATA(insert OID = 1949 (  bytealt		   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100	bytealt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1950 (  byteale		   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100	byteale - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1951 (  byteagt		   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100	byteagt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1952 (  byteage		   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100	byteage - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1953 (  byteane		   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100	byteane - _null_ ));
DESCR("not equal");
DATA(insert OID = 1954 (  byteacmp		   PGNSP PGUID 12 f f f t f i 2 23 "17 17" 100 0 0 100	byteacmp - _null_ ));
DESCR("less-equal-greater");

DATA(insert OID = 1961 (  timestamp		   PGNSP PGUID 12 f f t t f i 2 1114 "1114 23" 100 0 0 100	timestamp_scale - _null_ ));
DESCR("adjust time precision");

DATA(insert OID = 1965 (  oidlarger		   PGNSP PGUID 12 f f f t f i 2 26 "26 26" 100 0 0 100	oidlarger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1966 (  oidsmaller	   PGNSP PGUID 12 f f f t f i 2 26 "26 26" 100 0 0 100	oidsmaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 1967 (  timestamptz	   PGNSP PGUID 12 f f t t f i 2 1184 "1184 23" 100 0 0 100	timestamptz_scale - _null_ ));
DESCR("adjust time precision");
DATA(insert OID = 1968 (  time			   PGNSP PGUID 12 f f t t f i 2 1083 "1083 23" 100 0 0 100	time_scale - _null_ ));
DESCR("adjust time precision");
DATA(insert OID = 1969 (  timetz		   PGNSP PGUID 12 f f t t f i 2 1266 "1266 23" 100 0 0 100	timetz_scale - _null_ ));
DESCR("adjust time with time zone precision");

DATA(insert OID = 2005 (  bytealike		   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100 bytealike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 2006 (  byteanlike	   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100 byteanlike - _null_ ));
DESCR("does not match LIKE expression");
DATA(insert OID = 2007 (  like			   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100	bytealike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 2008 (  notlike		   PGNSP PGUID 12 f f f t f i 2 16 "17 17" 100 0 0 100	byteanlike - _null_ ));
DESCR("does not match LIKE expression");
DATA(insert OID = 2009 (  like_escape	   PGNSP PGUID 12 f f f t f i 2 17 "17 17" 100 0 0 100 like_escape_bytea - _null_ ));
DESCR("convert match pattern to use backslash escapes");
DATA(insert OID = 2010 (  length		   PGNSP PGUID 12 f f f t f i 1 23 "17" 100 0 0 100  byteaoctetlen - _null_ ));
DESCR("octet length");
DATA(insert OID = 2011 (  byteacat		   PGNSP PGUID 12 f f f t f i 2 17 "17 17" 100 0 0 100	byteacat - _null_ ));
DESCR("concatenate");
DATA(insert OID = 2012 (  substring		   PGNSP PGUID 12 f f f t f i 3 17 "17 23 23" 100 0 0 100  bytea_substr - _null_ ));
DESCR("return portion of string");
DATA(insert OID = 2013 (  substring		   PGNSP PGUID 14 f f f t f i 2 17 "17 23" 100 0 0 100	"select substring($1, $2, -1)" - _null_ ));
DESCR("return portion of string");
DATA(insert OID = 2014 (  position		   PGNSP PGUID 12 f f f t f i 2 23 "17 17" 100 0 0 100	byteapos - _null_ ));
DESCR("return position of substring");
DATA(insert OID = 2015 (  btrim			   PGNSP PGUID 12 f f f t f i 2 17 "17 17" 100 0 0 100	byteatrim - _null_ ));
DESCR("trim both ends of string");

DATA(insert OID = 2019 (  time				PGNSP PGUID 12 f f f t f s 1 1083 "1184" 100 0 0 100  timestamptz_time - _null_ ));
DESCR("convert timestamptz to time");
DATA(insert OID = 2020 (  date_trunc		PGNSP PGUID 12 f f f t f i 2 1114 "25 1114" 100 0 0 100  timestamp_trunc - _null_ ));
DESCR("truncate timestamp to specified units");
DATA(insert OID = 2021 (  date_part			PGNSP PGUID 12 f f f t f i 2  701 "25 1114" 100 0 0 100  timestamp_part - _null_ ));
DESCR("extract field from timestamp");
DATA(insert OID = 2022 (  timestamp			PGNSP PGUID 12 f f f t f s 1 1114 "25" 100 0 0 100	text_timestamp - _null_ ));
DESCR("convert text to timestamp");
DATA(insert OID = 2023 (  timestamp			PGNSP PGUID 12 f f t t f s 1 1114 "702" 100 0 0 100  abstime_timestamp - _null_ ));
DESCR("convert abstime to timestamp");
DATA(insert OID = 2024 (  timestamp			PGNSP PGUID 12 f f t t f i 1 1114 "1082" 100 0 0 100  date_timestamp - _null_ ));
DESCR("convert date to timestamp");
DATA(insert OID = 2025 (  timestamp			PGNSP PGUID 12 f f f t f i 2 1114 "1082 1083" 100 0 0 100  datetime_timestamp - _null_ ));
DESCR("convert date and time to timestamp");
DATA(insert OID = 2027 (  timestamp			PGNSP PGUID 12 f f t t f s 1 1114 "1184" 100 0 0 100  timestamptz_timestamp - _null_ ));
DESCR("convert date and time with time zone to timestamp");
DATA(insert OID = 2028 (  timestamptz		PGNSP PGUID 12 f f t t f s 1 1184 "1114" 100 0 0 100  timestamp_timestamptz - _null_ ));
DESCR("convert date and time with time zone to timestamp");
DATA(insert OID = 2029 (  date				PGNSP PGUID 12 f f f t f i 1 1082 "1114" 100 0 0 100  timestamp_date - _null_ ));
DESCR("convert timestamp to date");
DATA(insert OID = 2030 (  abstime			PGNSP PGUID 12 f f f t f s 1  702 "1114" 100 0 0 100  timestamp_abstime - _null_ ));
DESCR("convert timestamp to abstime");
DATA(insert OID = 2031 (  timestamp_mi		PGNSP PGUID 12 f f f t f i 2 1186 "1114 1114" 100 0 0 100  timestamp_mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 2032 (  timestamp_pl_span PGNSP PGUID 12 f f f t f i 2 1114 "1114 1186" 100 0 0 100  timestamp_pl_span - _null_ ));
DESCR("plus");
DATA(insert OID = 2033 (  timestamp_mi_span PGNSP PGUID 12 f f f t f i 2 1114 "1114 1186" 100 0 0 100  timestamp_mi_span - _null_ ));
DESCR("minus");
DATA(insert OID = 2034 (  text				PGNSP PGUID 12 f f t t f s 1   25 "1114" 100 0 0 100  timestamp_text - _null_ ));
DESCR("convert timestamp to text");
DATA(insert OID = 2035 (  timestamp_smaller PGNSP PGUID 12 f f f t f i 2 1114 "1114 1114" 100 0 0 100  timestamp_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 2036 (  timestamp_larger	PGNSP PGUID 12 f f f t f i 2 1114 "1114 1114" 100 0 0 100  timestamp_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 2037 (  timezone			PGNSP PGUID 12 f f f t f s 2 1266 "25 1266" 100 0 0 100  timetz_zone - _null_ ));
DESCR("time with time zone");
DATA(insert OID = 2038 (  timezone			PGNSP PGUID 12 f f f t f i 2 1266 "1186 1266" 100 0 0 100  timetz_izone - _null_ ));
DESCR("time with time zone");
DATA(insert OID = 2041 ( overlaps			PGNSP PGUID 12 f f f f f i 4 16 "1114 1114 1114 1114" 100 0 0 100  overlaps_timestamp - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 2042 ( overlaps			PGNSP PGUID 14 f f f f f i 4 16 "1114 1186 1114 1186" 100 0 0 100  "select ($1, ($1 + $2)) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 2043 ( overlaps			PGNSP PGUID 14 f f f f f i 4 16 "1114 1114 1114 1186" 100 0 0 100  "select ($1, $2) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 2044 ( overlaps			PGNSP PGUID 14 f f f f f i 4 16 "1114 1186 1114 1114" 100 0 0 100  "select ($1, ($1 + $2)) overlaps ($3, $4)" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 2045 (  timestamp_cmp		PGNSP PGUID 12 f f f t f i 2	23 "1114 1114" 100 0 0 100	timestamp_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 2046 (  time				PGNSP PGUID 12 f f t t f i 1 1083 "1266" 100 0 0 100  timetz_time - _null_ ));
DESCR("convert time with time zone to time");
DATA(insert OID = 2047 (  timetz			PGNSP PGUID 12 f f t t f s 1 1266 "1083" 100 0 0 100  time_timetz - _null_ ));
DESCR("convert time to timetz");
DATA(insert OID = 2048 (  isfinite			PGNSP PGUID 12 f f f t f i 1   16 "1114" 100 0 0 100  timestamp_finite - _null_ ));
DESCR("boolean test");
DATA(insert OID = 2049 ( to_char			PGNSP PGUID 12 f f f t f s 2	25 "1114 25" 100 0 0 100  timestamp_to_char - _null_ ));
DESCR("format timestamp to text");
DATA(insert OID = 2050 ( interval_mi_time	PGNSP PGUID 14 f f f t f i 2 1083 "1186 1083" 100 0 0 100  "select $2 - $1" - _null_ ));
DESCR("minus");
DATA(insert OID = 2051 ( interval_mi_timetz PGNSP PGUID 14 f f f t f i 2 1266 "1186 1266" 100 0 0 100  "select $2 - $1" - _null_ ));
DESCR("minus");
DATA(insert OID = 2052 (  timestamp_eq		PGNSP PGUID 12 f f f t f i 2 16 "1114 1114" 100 0 0 100  timestamp_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 2053 (  timestamp_ne		PGNSP PGUID 12 f f f t f i 2 16 "1114 1114" 100 0 0 100  timestamp_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 2054 (  timestamp_lt		PGNSP PGUID 12 f f f t f i 2 16 "1114 1114" 100 0 0 100  timestamp_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 2055 (  timestamp_le		PGNSP PGUID 12 f f f t f i 2 16 "1114 1114" 100 0 0 100  timestamp_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 2056 (  timestamp_ge		PGNSP PGUID 12 f f f t f i 2 16 "1114 1114" 100 0 0 100  timestamp_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 2057 (  timestamp_gt		PGNSP PGUID 12 f f f t f i 2 16 "1114 1114" 100 0 0 100  timestamp_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 2058 (  age				PGNSP PGUID 12 f f f t f i 2 1186 "1114 1114" 100 0 0 100  timestamp_age - _null_ ));
DESCR("date difference preserving months and years");
DATA(insert OID = 2059 (  age				PGNSP PGUID 14 f f f t f s 1 1186 "1114" 100 0 0 100  "select age(cast(current_date as timestamp without time zone), $1)" - _null_ ));
DESCR("date difference from today preserving months and years");

DATA(insert OID = 2069 (  timezone			PGNSP PGUID 12 f f f t f s 2 1184 "25 1114" 100 0 0 100  timestamp_zone - _null_ ));
DESCR("timestamp at a specified time zone");
DATA(insert OID = 2070 (  timezone			PGNSP PGUID 12 f f f t f s 2 1184 "1186 1114" 100 0 0 100  timestamp_izone - _null_ ));
DESCR("time zone");
DATA(insert OID = 2071 (  date_pl_interval	PGNSP PGUID 14 f f f t f i 2 1114 "1082 1186" 100 0 0 100  "select cast($1 as timestamp without time zone) + $2;" - _null_ ));
DESCR("add");
DATA(insert OID = 2072 (  date_mi_interval	PGNSP PGUID 14 f f f t f i 2 1114 "1082 1186" 100 0 0 100  "select cast($1 as timestamp without time zone) - $2;" - _null_ ));
DESCR("subtract");

/* Aggregates (moved here from pg_aggregate for 7.3) */

DATA(insert OID = 2100 (  avg				PGNSP PGUID 12 t f f f f i 1 1700 "20" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2101 (  avg				PGNSP PGUID 12 t f f f f i 1 1700 "23" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2102 (  avg				PGNSP PGUID 12 t f f f f i 1 1700 "21" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2103 (  avg				PGNSP PGUID 12 t f f f f i 1 1700 "1700" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2104 (  avg				PGNSP PGUID 12 t f f f f i 1 701 "700" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2105 (  avg				PGNSP PGUID 12 t f f f f i 1 701 "701" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2106 (  avg				PGNSP PGUID 12 t f f f f i 1 1186 "1186" 100 0 0 100  aggregate_dummy - _null_ ));

DATA(insert OID = 2107 (  sum				PGNSP PGUID 12 t f f f f i 1 1700 "20" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2108 (  sum				PGNSP PGUID 12 t f f f f i 1 20 "23" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2109 (  sum				PGNSP PGUID 12 t f f f f i 1 20 "21" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2110 (  sum				PGNSP PGUID 12 t f f f f i 1 700 "700" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2111 (  sum				PGNSP PGUID 12 t f f f f i 1 701 "701" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2112 (  sum				PGNSP PGUID 12 t f f f f i 1 790 "790" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2113 (  sum				PGNSP PGUID 12 t f f f f i 1 1186 "1186" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2114 (  sum				PGNSP PGUID 12 t f f f f i 1 1700 "1700" 100 0 0 100  aggregate_dummy - _null_ ));

DATA(insert OID = 2115 (  max				PGNSP PGUID 12 t f f f f i 1 20 "20" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2116 (  max				PGNSP PGUID 12 t f f f f i 1 23 "23" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2117 (  max				PGNSP PGUID 12 t f f f f i 1 21 "21" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2118 (  max				PGNSP PGUID 12 t f f f f i 1 26 "26" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2119 (  max				PGNSP PGUID 12 t f f f f i 1 700 "700" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2120 (  max				PGNSP PGUID 12 t f f f f i 1 701 "701" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2121 (  max				PGNSP PGUID 12 t f f f f i 1 702 "702" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2122 (  max				PGNSP PGUID 12 t f f f f i 1 1082 "1082" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2123 (  max				PGNSP PGUID 12 t f f f f i 1 1083 "1083" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2124 (  max				PGNSP PGUID 12 t f f f f i 1 1266 "1266" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2125 (  max				PGNSP PGUID 12 t f f f f i 1 790 "790" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2126 (  max				PGNSP PGUID 12 t f f f f i 1 1114 "1114" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2127 (  max				PGNSP PGUID 12 t f f f f i 1 1184 "1184" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2128 (  max				PGNSP PGUID 12 t f f f f i 1 1186 "1186" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2129 (  max				PGNSP PGUID 12 t f f f f i 1 25 "25" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2130 (  max				PGNSP PGUID 12 t f f f f i 1 1700 "1700" 100 0 0 100  aggregate_dummy - _null_ ));

DATA(insert OID = 2131 (  min				PGNSP PGUID 12 t f f f f i 1 20 "20" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2132 (  min				PGNSP PGUID 12 t f f f f i 1 23 "23" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2133 (  min				PGNSP PGUID 12 t f f f f i 1 21 "21" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2134 (  min				PGNSP PGUID 12 t f f f f i 1 26 "26" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2135 (  min				PGNSP PGUID 12 t f f f f i 1 700 "700" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2136 (  min				PGNSP PGUID 12 t f f f f i 1 701 "701" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2137 (  min				PGNSP PGUID 12 t f f f f i 1 702 "702" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2138 (  min				PGNSP PGUID 12 t f f f f i 1 1082 "1082" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2139 (  min				PGNSP PGUID 12 t f f f f i 1 1083 "1083" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2140 (  min				PGNSP PGUID 12 t f f f f i 1 1266 "1266" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2141 (  min				PGNSP PGUID 12 t f f f f i 1 790 "790" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2142 (  min				PGNSP PGUID 12 t f f f f i 1 1114 "1114" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2143 (  min				PGNSP PGUID 12 t f f f f i 1 1184 "1184" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2144 (  min				PGNSP PGUID 12 t f f f f i 1 1186 "1186" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2145 (  min				PGNSP PGUID 12 t f f f f i 1 25 "25" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2146 (  min				PGNSP PGUID 12 t f f f f i 1 1700 "1700" 100 0 0 100  aggregate_dummy - _null_ ));

DATA(insert OID = 2147 (  count				PGNSP PGUID 12 t f f f f i 1 20 "0" 100 0 0 100  aggregate_dummy - _null_ ));

DATA(insert OID = 2148 (  variance			PGNSP PGUID 12 t f f f f i 1 1700 "20" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2149 (  variance			PGNSP PGUID 12 t f f f f i 1 1700 "23" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2150 (  variance			PGNSP PGUID 12 t f f f f i 1 1700 "21" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2151 (  variance			PGNSP PGUID 12 t f f f f i 1 701 "700" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2152 (  variance			PGNSP PGUID 12 t f f f f i 1 701 "701" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2153 (  variance			PGNSP PGUID 12 t f f f f i 1 1700 "1700" 100 0 0 100  aggregate_dummy - _null_ ));

DATA(insert OID = 2154 (  stddev			PGNSP PGUID 12 t f f f f i 1 1700 "20" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2155 (  stddev			PGNSP PGUID 12 t f f f f i 1 1700 "23" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2156 (  stddev			PGNSP PGUID 12 t f f f f i 1 1700 "21" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2157 (  stddev			PGNSP PGUID 12 t f f f f i 1 701 "700" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2158 (  stddev			PGNSP PGUID 12 t f f f f i 1 701 "701" 100 0 0 100  aggregate_dummy - _null_ ));
DATA(insert OID = 2159 (  stddev			PGNSP PGUID 12 t f f f f i 1 1700 "1700" 100 0 0 100  aggregate_dummy - _null_ ));


DATA(insert OID = 2212 (  regprocedurein	PGNSP PGUID 12 f f f t f s 1 2202 "0" 100 0 0 100	regprocedurein - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2213 (  regprocedureout	PGNSP PGUID 12 f f f t f s 1   23 "0" 100 0 0 100	regprocedureout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2214 (  regoperin			PGNSP PGUID 12 f f f t f s 1 2203 "0" 100 0 0 100	regoperin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2215 (  regoperout		PGNSP PGUID 12 f f f t f s 1   23 "0" 100 0 0 100	regoperout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2216 (  regoperatorin		PGNSP PGUID 12 f f f t f s 1 2204 "0" 100 0 0 100	regoperatorin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2217 (  regoperatorout	PGNSP PGUID 12 f f f t f s 1   23 "0" 100 0 0 100	regoperatorout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2218 (  regclassin		PGNSP PGUID 12 f f f t f s 1 2205 "0" 100 0 0 100	regclassin - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2219 (  regclassout		PGNSP PGUID 12 f f f t f s 1   23 "0" 100 0 0 100	regclassout - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2220 (  regtypein			PGNSP PGUID 12 f f f t f s 1 2206 "0" 100 0 0 100	regtypein - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2221 (  regtypeout		PGNSP PGUID 12 f f f t f s 1   23 "0" 100 0 0 100	regtypeout - _null_ ));
DESCR("(internal)");


/*
 * Symbolic values for provolatile column: these indicate whether the result
 * of a function is dependent *only* on the values of its explicit arguments,
 * or can change due to outside factors (such as parameter variables or
 * table contents).  NOTE: functions having side-effects, such as setval(),
 * must be labeled volatile to ensure they will not get optimized away,
 * even if the actual return value is not changeable.
 */
#define PROVOLATILE_IMMUTABLE	'i'	/* never changes for given input */
#define PROVOLATILE_STABLE		's'	/* does not change within a scan */
#define PROVOLATILE_VOLATILE	'v'	/* can change even within a scan */


/*
 * prototypes for functions in pg_proc.c
 */
extern Oid ProcedureCreate(const char *procedureName,
				Oid procNamespace,
				bool replace,
				bool returnsSet,
				Oid returnType,
				Oid languageObjectId,
				const char *prosrc,
				const char *probin,
				bool isAgg,
				bool security_definer,
				bool isImplicit,
				bool isStrict,
				char volatility,
				int32 byte_pct,
				int32 perbyte_cpu,
				int32 percall_cpu,
				int32 outin_ratio,
				int parameterCount,
				const Oid *parameterTypes);

#endif   /* PG_PROC_H */
