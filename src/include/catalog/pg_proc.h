/*-------------------------------------------------------------------------
 *
 * pg_proc.h
 *	  definition of the system "procedure" relation (pg_proc)
 *	  along with the relation's initial contents.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_proc.h,v 1.314 2003/10/21 16:23:16 tgl Exp $
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
 *		postgres.h contains the system type definitions and the
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
	bool		proisstrict;	/* strict with respect to NULLs? */
	bool		proretset;		/* returns a set? */
	char		provolatile;	/* see PROVOLATILE_ categories below */
	int2		pronargs;		/* number of arguments */
	Oid			prorettype;		/* OID of result type */
	oidvector	proargtypes;	/* OIDs of argument types */
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
#define Natts_pg_proc					15
#define Anum_pg_proc_proname			1
#define Anum_pg_proc_pronamespace		2
#define Anum_pg_proc_proowner			3
#define Anum_pg_proc_prolang			4
#define Anum_pg_proc_proisagg			5
#define Anum_pg_proc_prosecdef			6
#define Anum_pg_proc_proisstrict		7
#define Anum_pg_proc_proretset			8
#define Anum_pg_proc_provolatile		9
#define Anum_pg_proc_pronargs			10
#define Anum_pg_proc_prorettype			11
#define Anum_pg_proc_proargtypes		12
#define Anum_pg_proc_prosrc				13
#define Anum_pg_proc_probin				14
#define Anum_pg_proc_proacl				15

/* ----------------
 *		initial contents of pg_proc
 * ----------------
 */

/* keep the following ordered by OID so that later changes can be made easier */

/* OIDS 1 - 99 */

DATA(insert OID = 1242 (  boolin		   PGNSP PGUID 12 f f t f i 1 16 "2275"  boolin - _null_ ));
DESCR("I/O");
DATA(insert OID = 1243 (  boolout		   PGNSP PGUID 12 f f t f i 1 2275 "16" boolout - _null_ ));
DESCR("I/O");
DATA(insert OID = 1244 (  byteain		   PGNSP PGUID 12 f f t f i 1 17 "2275" byteain - _null_ ));
DESCR("I/O");
DATA(insert OID =  31 (  byteaout		   PGNSP PGUID 12 f f t f i 1 2275 "17" byteaout - _null_ ));
DESCR("I/O");
DATA(insert OID = 1245 (  charin		   PGNSP PGUID 12 f f t f i 1 18 "2275" charin - _null_ ));
DESCR("I/O");
DATA(insert OID =  33 (  charout		   PGNSP PGUID 12 f f t f i 1 2275 "18" charout - _null_ ));
DESCR("I/O");
DATA(insert OID =  34 (  namein			   PGNSP PGUID 12 f f t f i 1 19 "2275" namein - _null_ ));
DESCR("I/O");
DATA(insert OID =  35 (  nameout		   PGNSP PGUID 12 f f t f i 1 2275 "19" nameout - _null_ ));
DESCR("I/O");
DATA(insert OID =  38 (  int2in			   PGNSP PGUID 12 f f t f i 1 21 "2275" int2in - _null_ ));
DESCR("I/O");
DATA(insert OID =  39 (  int2out		   PGNSP PGUID 12 f f t f i 1 2275 "21" int2out - _null_ ));
DESCR("I/O");
DATA(insert OID =  40 (  int2vectorin	   PGNSP PGUID 12 f f t f i 1 22 "2275" int2vectorin - _null_ ));
DESCR("I/O");
DATA(insert OID =  41 (  int2vectorout	   PGNSP PGUID 12 f f t f i 1 2275 "22" int2vectorout - _null_ ));
DESCR("I/O");
DATA(insert OID =  42 (  int4in			   PGNSP PGUID 12 f f t f i 1 23 "2275" int4in - _null_ ));
DESCR("I/O");
DATA(insert OID =  43 (  int4out		   PGNSP PGUID 12 f f t f i 1 2275 "23" int4out - _null_ ));
DESCR("I/O");
DATA(insert OID =  44 (  regprocin		   PGNSP PGUID 12 f f t f s 1 24 "2275" regprocin - _null_ ));
DESCR("I/O");
DATA(insert OID =  45 (  regprocout		   PGNSP PGUID 12 f f t f s 1 2275 "24" regprocout - _null_ ));
DESCR("I/O");
DATA(insert OID =  46 (  textin			   PGNSP PGUID 12 f f t f i 1 25 "2275" textin - _null_ ));
DESCR("I/O");
DATA(insert OID =  47 (  textout		   PGNSP PGUID 12 f f t f i 1 2275 "25" textout - _null_ ));
DESCR("I/O");
DATA(insert OID =  48 (  tidin			   PGNSP PGUID 12 f f t f i 1 27 "2275" tidin - _null_ ));
DESCR("I/O");
DATA(insert OID =  49 (  tidout			   PGNSP PGUID 12 f f t f i 1 2275 "27" tidout - _null_ ));
DESCR("I/O");
DATA(insert OID =  50 (  xidin			   PGNSP PGUID 12 f f t f i 1 28 "2275" xidin - _null_ ));
DESCR("I/O");
DATA(insert OID =  51 (  xidout			   PGNSP PGUID 12 f f t f i 1 2275 "28" xidout - _null_ ));
DESCR("I/O");
DATA(insert OID =  52 (  cidin			   PGNSP PGUID 12 f f t f i 1 29 "2275" cidin - _null_ ));
DESCR("I/O");
DATA(insert OID =  53 (  cidout			   PGNSP PGUID 12 f f t f i 1 2275 "29" cidout - _null_ ));
DESCR("I/O");
DATA(insert OID =  54 (  oidvectorin	   PGNSP PGUID 12 f f t f i 1 30 "2275" oidvectorin - _null_ ));
DESCR("I/O");
DATA(insert OID =  55 (  oidvectorout	   PGNSP PGUID 12 f f t f i 1 2275 "30" oidvectorout - _null_ ));
DESCR("I/O");
DATA(insert OID =  56 (  boollt			   PGNSP PGUID 12 f f t f i 2 16 "16 16"	boollt - _null_ ));
DESCR("less-than");
DATA(insert OID =  57 (  boolgt			   PGNSP PGUID 12 f f t f i 2 16 "16 16"	boolgt - _null_ ));
DESCR("greater-than");
DATA(insert OID =  60 (  booleq			   PGNSP PGUID 12 f f t f i 2 16 "16 16"	booleq - _null_ ));
DESCR("equal");
DATA(insert OID =  61 (  chareq			   PGNSP PGUID 12 f f t f i 2 16 "18 18"	chareq - _null_ ));
DESCR("equal");
DATA(insert OID =  62 (  nameeq			   PGNSP PGUID 12 f f t f i 2 16 "19 19"	nameeq - _null_ ));
DESCR("equal");
DATA(insert OID =  63 (  int2eq			   PGNSP PGUID 12 f f t f i 2 16 "21 21"	int2eq - _null_ ));
DESCR("equal");
DATA(insert OID =  64 (  int2lt			   PGNSP PGUID 12 f f t f i 2 16 "21 21"	int2lt - _null_ ));
DESCR("less-than");
DATA(insert OID =  65 (  int4eq			   PGNSP PGUID 12 f f t f i 2 16 "23 23"	int4eq - _null_ ));
DESCR("equal");
DATA(insert OID =  66 (  int4lt			   PGNSP PGUID 12 f f t f i 2 16 "23 23"	int4lt - _null_ ));
DESCR("less-than");
DATA(insert OID =  67 (  texteq			   PGNSP PGUID 12 f f t f i 2 16 "25 25"	texteq - _null_ ));
DESCR("equal");
DATA(insert OID =  68 (  xideq			   PGNSP PGUID 12 f f t f i 2 16 "28 28"	xideq - _null_ ));
DESCR("equal");
DATA(insert OID =  69 (  cideq			   PGNSP PGUID 12 f f t f i 2 16 "29 29"	cideq - _null_ ));
DESCR("equal");
DATA(insert OID =  70 (  charne			   PGNSP PGUID 12 f f t f i 2 16 "18 18"	charne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1246 (  charlt		   PGNSP PGUID 12 f f t f i 2 16 "18 18"	charlt - _null_ ));
DESCR("less-than");
DATA(insert OID =  72 (  charle			   PGNSP PGUID 12 f f t f i 2 16 "18 18"	charle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID =  73 (  chargt			   PGNSP PGUID 12 f f t f i 2 16 "18 18"	chargt - _null_ ));
DESCR("greater-than");
DATA(insert OID =  74 (  charge			   PGNSP PGUID 12 f f t f i 2 16 "18 18"	charge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1248 (  charpl		   PGNSP PGUID 12 f f t f i 2 18 "18 18"	charpl - _null_ ));
DESCR("add");
DATA(insert OID = 1250 (  charmi		   PGNSP PGUID 12 f f t f i 2 18 "18 18"	charmi - _null_ ));
DESCR("subtract");
DATA(insert OID =  77 (  charmul		   PGNSP PGUID 12 f f t f i 2 18 "18 18"	charmul - _null_ ));
DESCR("multiply");
DATA(insert OID =  78 (  chardiv		   PGNSP PGUID 12 f f t f i 2 18 "18 18"	chardiv - _null_ ));
DESCR("divide");

DATA(insert OID =  79 (  nameregexeq	   PGNSP PGUID 12 f f t f i 2 16 "19 25"	nameregexeq - _null_ ));
DESCR("matches regex., case-sensitive");
DATA(insert OID = 1252 (  nameregexne	   PGNSP PGUID 12 f f t f i 2 16 "19 25"	nameregexne - _null_ ));
DESCR("does not match regex., case-sensitive");
DATA(insert OID = 1254 (  textregexeq	   PGNSP PGUID 12 f f t f i 2 16 "25 25"	textregexeq - _null_ ));
DESCR("matches regex., case-sensitive");
DATA(insert OID = 1256 (  textregexne	   PGNSP PGUID 12 f f t f i 2 16 "25 25"	textregexne - _null_ ));
DESCR("does not match regex., case-sensitive");
DATA(insert OID = 1257 (  textlen		   PGNSP PGUID 12 f f t f i 1 23 "25"  textlen - _null_ ));
DESCR("length");
DATA(insert OID = 1258 (  textcat		   PGNSP PGUID 12 f f t f i 2 25 "25 25"	textcat - _null_ ));
DESCR("concatenate");

DATA(insert OID =  84 (  boolne			   PGNSP PGUID 12 f f t f i 2 16 "16 16"	boolne - _null_ ));
DESCR("not equal");
DATA(insert OID =  89 (  version		   PGNSP PGUID 12 f f t f s 0 25 "" pgsql_version - _null_ ));
DESCR("PostgreSQL version string");

/* OIDS 100 - 199 */

DATA(insert OID = 100 (  int8fac		   PGNSP PGUID 12 f f t f i 1 20 "20"  int8fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 101 (  eqsel			   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  eqsel - _null_ ));
DESCR("restriction selectivity of = and related operators");
DATA(insert OID = 102 (  neqsel			   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  neqsel - _null_ ));
DESCR("restriction selectivity of <> and related operators");
DATA(insert OID = 103 (  scalarltsel	   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  scalarltsel - _null_ ));
DESCR("restriction selectivity of < and related operators on scalar datatypes");
DATA(insert OID = 104 (  scalargtsel	   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  scalargtsel - _null_ ));
DESCR("restriction selectivity of > and related operators on scalar datatypes");
DATA(insert OID = 105 (  eqjoinsel		   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  eqjoinsel - _null_ ));
DESCR("join selectivity of = and related operators");
DATA(insert OID = 106 (  neqjoinsel		   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  neqjoinsel - _null_ ));
DESCR("join selectivity of <> and related operators");
DATA(insert OID = 107 (  scalarltjoinsel   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  scalarltjoinsel - _null_ ));
DESCR("join selectivity of < and related operators on scalar datatypes");
DATA(insert OID = 108 (  scalargtjoinsel   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  scalargtjoinsel - _null_ ));
DESCR("join selectivity of > and related operators on scalar datatypes");

DATA(insert OID =  109 (  unknownin		   PGNSP PGUID 12 f f t f i 1 705 "2275"	unknownin - _null_ ));
DESCR("I/O");
DATA(insert OID =  110 (  unknownout	   PGNSP PGUID 12 f f t f i 1 2275	"705"	unknownout - _null_ ));
DESCR("I/O");

DATA(insert OID = 112 (  text			   PGNSP PGUID 12 f f t f i 1  25 "23"	int4_text - _null_ ));
DESCR("convert int4 to text");
DATA(insert OID = 113 (  text			   PGNSP PGUID 12 f f t f i 1  25 "21"	int2_text - _null_ ));
DESCR("convert int2 to text");
DATA(insert OID = 114 (  text			   PGNSP PGUID 12 f f t f i 1  25 "26"	oid_text - _null_ ));
DESCR("convert oid to text");

DATA(insert OID = 115 (  box_above		   PGNSP PGUID 12 f f t f i 2  16 "603 603"  box_above - _null_ ));
DESCR("is above");
DATA(insert OID = 116 (  box_below		   PGNSP PGUID 12 f f t f i 2  16 "603 603"  box_below - _null_ ));
DESCR("is below");

DATA(insert OID = 117 (  point_in		   PGNSP PGUID 12 f f t f i 1 600 "2275"  point_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 118 (  point_out		   PGNSP PGUID 12 f f t f i 1 2275 "600"  point_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 119 (  lseg_in		   PGNSP PGUID 12 f f t f i 1 601 "2275"  lseg_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 120 (  lseg_out		   PGNSP PGUID 12 f f t f i 1 2275 "601"  lseg_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 121 (  path_in		   PGNSP PGUID 12 f f t f i 1 602 "2275"  path_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 122 (  path_out		   PGNSP PGUID 12 f f t f i 1 2275 "602"  path_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 123 (  box_in			   PGNSP PGUID 12 f f t f i 1 603 "2275"  box_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 124 (  box_out		   PGNSP PGUID 12 f f t f i 1 2275 "603"  box_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 125 (  box_overlap	   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_overlap - _null_ ));
DESCR("overlaps");
DATA(insert OID = 126 (  box_ge			   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_ge - _null_ ));
DESCR("greater-than-or-equal by area");
DATA(insert OID = 127 (  box_gt			   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_gt - _null_ ));
DESCR("greater-than by area");
DATA(insert OID = 128 (  box_eq			   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_eq - _null_ ));
DESCR("equal by area");
DATA(insert OID = 129 (  box_lt			   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_lt - _null_ ));
DESCR("less-than by area");
DATA(insert OID = 130 (  box_le			   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_le - _null_ ));
DESCR("less-than-or-equal by area");
DATA(insert OID = 131 (  point_above	   PGNSP PGUID 12 f f t f i 2 16 "600 600"	point_above - _null_ ));
DESCR("is above");
DATA(insert OID = 132 (  point_left		   PGNSP PGUID 12 f f t f i 2 16 "600 600"	point_left - _null_ ));
DESCR("is left of");
DATA(insert OID = 133 (  point_right	   PGNSP PGUID 12 f f t f i 2 16 "600 600"	point_right - _null_ ));
DESCR("is right of");
DATA(insert OID = 134 (  point_below	   PGNSP PGUID 12 f f t f i 2 16 "600 600"	point_below - _null_ ));
DESCR("is below");
DATA(insert OID = 135 (  point_eq		   PGNSP PGUID 12 f f t f i 2 16 "600 600"	point_eq - _null_ ));
DESCR("same as?");
DATA(insert OID = 136 (  on_pb			   PGNSP PGUID 12 f f t f i 2 16 "600 603"	on_pb - _null_ ));
DESCR("point inside box?");
DATA(insert OID = 137 (  on_ppath		   PGNSP PGUID 12 f f t f i 2 16 "600 602"	on_ppath - _null_ ));
DESCR("point within closed path, or point on open path");
DATA(insert OID = 138 (  box_center		   PGNSP PGUID 12 f f t f i 1 600 "603"  box_center - _null_ ));
DESCR("center of");
DATA(insert OID = 139 (  areasel		   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  areasel - _null_ ));
DESCR("restriction selectivity for area-comparison operators");
DATA(insert OID = 140 (  areajoinsel	   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  areajoinsel - _null_ ));
DESCR("join selectivity for area-comparison operators");
DATA(insert OID = 141 (  int4mul		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 142 (  int4fac		   PGNSP PGUID 12 f f t f i 1 23 "23"  int4fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 144 (  int4ne			   PGNSP PGUID 12 f f t f i 2 16 "23 23"	int4ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 145 (  int2ne			   PGNSP PGUID 12 f f t f i 2 16 "21 21"	int2ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 146 (  int2gt			   PGNSP PGUID 12 f f t f i 2 16 "21 21"	int2gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 147 (  int4gt			   PGNSP PGUID 12 f f t f i 2 16 "23 23"	int4gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 148 (  int2le			   PGNSP PGUID 12 f f t f i 2 16 "21 21"	int2le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 149 (  int4le			   PGNSP PGUID 12 f f t f i 2 16 "23 23"	int4le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 150 (  int4ge			   PGNSP PGUID 12 f f t f i 2 16 "23 23"	int4ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 151 (  int2ge			   PGNSP PGUID 12 f f t f i 2 16 "21 21"	int2ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 152 (  int2mul		   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 153 (  int2div		   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2div - _null_ ));
DESCR("divide");
DATA(insert OID = 154 (  int4div		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4div - _null_ ));
DESCR("divide");
DATA(insert OID = 155 (  int2mod		   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 156 (  int4mod		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 157 (  textne			   PGNSP PGUID 12 f f t f i 2 16 "25 25"	textne - _null_ ));
DESCR("not equal");
DATA(insert OID = 158 (  int24eq		   PGNSP PGUID 12 f f t f i 2 16 "21 23"	int24eq - _null_ ));
DESCR("equal");
DATA(insert OID = 159 (  int42eq		   PGNSP PGUID 12 f f t f i 2 16 "23 21"	int42eq - _null_ ));
DESCR("equal");
DATA(insert OID = 160 (  int24lt		   PGNSP PGUID 12 f f t f i 2 16 "21 23"	int24lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 161 (  int42lt		   PGNSP PGUID 12 f f t f i 2 16 "23 21"	int42lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 162 (  int24gt		   PGNSP PGUID 12 f f t f i 2 16 "21 23"	int24gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 163 (  int42gt		   PGNSP PGUID 12 f f t f i 2 16 "23 21"	int42gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 164 (  int24ne		   PGNSP PGUID 12 f f t f i 2 16 "21 23"	int24ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 165 (  int42ne		   PGNSP PGUID 12 f f t f i 2 16 "23 21"	int42ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 166 (  int24le		   PGNSP PGUID 12 f f t f i 2 16 "21 23"	int24le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 167 (  int42le		   PGNSP PGUID 12 f f t f i 2 16 "23 21"	int42le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 168 (  int24ge		   PGNSP PGUID 12 f f t f i 2 16 "21 23"	int24ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 169 (  int42ge		   PGNSP PGUID 12 f f t f i 2 16 "23 21"	int42ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 170 (  int24mul		   PGNSP PGUID 12 f f t f i 2 23 "21 23"	int24mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 171 (  int42mul		   PGNSP PGUID 12 f f t f i 2 23 "23 21"	int42mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 172 (  int24div		   PGNSP PGUID 12 f f t f i 2 23 "21 23"	int24div - _null_ ));
DESCR("divide");
DATA(insert OID = 173 (  int42div		   PGNSP PGUID 12 f f t f i 2 23 "23 21"	int42div - _null_ ));
DESCR("divide");
DATA(insert OID = 174 (  int24mod		   PGNSP PGUID 12 f f t f i 2 23 "21 23"	int24mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 175 (  int42mod		   PGNSP PGUID 12 f f t f i 2 23 "23 21"	int42mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 176 (  int2pl			   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2pl - _null_ ));
DESCR("add");
DATA(insert OID = 177 (  int4pl			   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4pl - _null_ ));
DESCR("add");
DATA(insert OID = 178 (  int24pl		   PGNSP PGUID 12 f f t f i 2 23 "21 23"	int24pl - _null_ ));
DESCR("add");
DATA(insert OID = 179 (  int42pl		   PGNSP PGUID 12 f f t f i 2 23 "23 21"	int42pl - _null_ ));
DESCR("add");
DATA(insert OID = 180 (  int2mi			   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 181 (  int4mi			   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 182 (  int24mi		   PGNSP PGUID 12 f f t f i 2 23 "21 23"	int24mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 183 (  int42mi		   PGNSP PGUID 12 f f t f i 2 23 "23 21"	int42mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 184 (  oideq			   PGNSP PGUID 12 f f t f i 2 16 "26 26"	oideq - _null_ ));
DESCR("equal");
DATA(insert OID = 185 (  oidne			   PGNSP PGUID 12 f f t f i 2 16 "26 26"	oidne - _null_ ));
DESCR("not equal");
DATA(insert OID = 186 (  box_same		   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_same - _null_ ));
DESCR("same as?");
DATA(insert OID = 187 (  box_contain	   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_contain - _null_ ));
DESCR("contains?");
DATA(insert OID = 188 (  box_left		   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_left - _null_ ));
DESCR("is left of");
DATA(insert OID = 189 (  box_overleft	   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_overleft - _null_ ));
DESCR("overlaps or is left of");
DATA(insert OID = 190 (  box_overright	   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_overright - _null_ ));
DESCR("overlaps or is right of");
DATA(insert OID = 191 (  box_right		   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_right - _null_ ));
DESCR("is right of");
DATA(insert OID = 192 (  box_contained	   PGNSP PGUID 12 f f t f i 2 16 "603 603"	box_contained - _null_ ));
DESCR("contained in?");
DATA(insert OID = 193 (  rt_box_union	   PGNSP PGUID 12 f f t f i 2 603 "603 603"  rt_box_union - _null_ ));
DESCR("r-tree");
DATA(insert OID = 194 (  rt_box_inter	   PGNSP PGUID 12 f f t f i 2 2278 "603 603"  rt_box_inter - _null_ ));
DESCR("r-tree");
DATA(insert OID = 195 (  rt_box_size	   PGNSP PGUID 12 f f t f i 2 2278 "603 2281"  rt_box_size - _null_ ));
DESCR("r-tree");
DATA(insert OID = 196 (  rt_bigbox_size    PGNSP PGUID 12 f f t f i 2 2278 "603 2281"  rt_bigbox_size - _null_ ));
DESCR("r-tree");
DATA(insert OID = 197 (  rt_poly_union	   PGNSP PGUID 12 f f t f i 2 604 "604 604"  rt_poly_union - _null_ ));
DESCR("r-tree");
DATA(insert OID = 198 (  rt_poly_inter	   PGNSP PGUID 12 f f t f i 2 2278 "604 604"  rt_poly_inter - _null_ ));
DESCR("r-tree");
DATA(insert OID = 199 (  rt_poly_size	   PGNSP PGUID 12 f f t f i 2 2278 "604 2281"  rt_poly_size - _null_ ));
DESCR("r-tree");

/* OIDS 200 - 299 */

DATA(insert OID = 200 (  float4in		   PGNSP PGUID 12 f f t f i 1 700 "2275"  float4in - _null_ ));
DESCR("I/O");
DATA(insert OID = 201 (  float4out		   PGNSP PGUID 12 f f t f i 1 2275 "700"  float4out - _null_ ));
DESCR("I/O");
DATA(insert OID = 202 (  float4mul		   PGNSP PGUID 12 f f t f i 2 700 "700 700"  float4mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 203 (  float4div		   PGNSP PGUID 12 f f t f i 2 700 "700 700"  float4div - _null_ ));
DESCR("divide");
DATA(insert OID = 204 (  float4pl		   PGNSP PGUID 12 f f t f i 2 700 "700 700"  float4pl - _null_ ));
DESCR("add");
DATA(insert OID = 205 (  float4mi		   PGNSP PGUID 12 f f t f i 2 700 "700 700"  float4mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 206 (  float4um		   PGNSP PGUID 12 f f t f i 1 700 "700"  float4um - _null_ ));
DESCR("negate");
DATA(insert OID = 207 (  float4abs		   PGNSP PGUID 12 f f t f i 1 700 "700"  float4abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 208 (  float4_accum	   PGNSP PGUID 12 f f t f i 2 1022 "1022 700"  float4_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 209 (  float4larger	   PGNSP PGUID 12 f f t f i 2 700 "700 700"  float4larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 211 (  float4smaller	   PGNSP PGUID 12 f f t f i 2 700 "700 700"  float4smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 212 (  int4um			   PGNSP PGUID 12 f f t f i 1 23 "23"  int4um - _null_ ));
DESCR("negate");
DATA(insert OID = 213 (  int2um			   PGNSP PGUID 12 f f t f i 1 21 "21"  int2um - _null_ ));
DESCR("negate");

DATA(insert OID = 214 (  float8in		   PGNSP PGUID 12 f f t f i 1 701 "2275"  float8in - _null_ ));
DESCR("I/O");
DATA(insert OID = 215 (  float8out		   PGNSP PGUID 12 f f t f i 1 2275 "701"  float8out - _null_ ));
DESCR("I/O");
DATA(insert OID = 216 (  float8mul		   PGNSP PGUID 12 f f t f i 2 701 "701 701"  float8mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 217 (  float8div		   PGNSP PGUID 12 f f t f i 2 701 "701 701"  float8div - _null_ ));
DESCR("divide");
DATA(insert OID = 218 (  float8pl		   PGNSP PGUID 12 f f t f i 2 701 "701 701"  float8pl - _null_ ));
DESCR("add");
DATA(insert OID = 219 (  float8mi		   PGNSP PGUID 12 f f t f i 2 701 "701 701"  float8mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 220 (  float8um		   PGNSP PGUID 12 f f t f i 1 701 "701"  float8um - _null_ ));
DESCR("negate");
DATA(insert OID = 221 (  float8abs		   PGNSP PGUID 12 f f t f i 1 701 "701"  float8abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 222 (  float8_accum	   PGNSP PGUID 12 f f t f i 2 1022 "1022 701"  float8_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 223 (  float8larger	   PGNSP PGUID 12 f f t f i 2 701 "701 701"  float8larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 224 (  float8smaller	   PGNSP PGUID 12 f f t f i 2 701 "701 701"  float8smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 225 (  lseg_center	   PGNSP PGUID 12 f f t f i 1 600 "601"  lseg_center - _null_ ));
DESCR("center of");
DATA(insert OID = 226 (  path_center	   PGNSP PGUID 12 f f t f i 1 600 "602"  path_center - _null_ ));
DESCR("center of");
DATA(insert OID = 227 (  poly_center	   PGNSP PGUID 12 f f t f i 1 600 "604"  poly_center - _null_ ));
DESCR("center of");

DATA(insert OID = 228 (  dround			   PGNSP PGUID 12 f f t f i 1 701 "701"  dround - _null_ ));
DESCR("round to nearest integer");
DATA(insert OID = 229 (  dtrunc			   PGNSP PGUID 12 f f t f i 1 701 "701"  dtrunc - _null_ ));
DESCR("truncate to integer");
DATA(insert OID = 2308 ( ceil			   PGNSP PGUID 12 f f t f i 1 701 "701"  dceil - _null_ ));
DESCR("smallest integer >= value");
DATA(insert OID = 2309 ( floor			   PGNSP PGUID 12 f f t f i 1 701 "701"  dfloor - _null_ ));
DESCR("largest integer <= value");
DATA(insert OID = 2310 ( sign			   PGNSP PGUID 12 f f t f i 1 701 "701"  dsign - _null_ ));
DESCR("sign of value");
DATA(insert OID = 230 (  dsqrt			   PGNSP PGUID 12 f f t f i 1 701 "701"  dsqrt - _null_ ));
DESCR("square root");
DATA(insert OID = 231 (  dcbrt			   PGNSP PGUID 12 f f t f i 1 701 "701"  dcbrt - _null_ ));
DESCR("cube root");
DATA(insert OID = 232 (  dpow			   PGNSP PGUID 12 f f t f i 2 701 "701 701"  dpow - _null_ ));
DESCR("exponentiation (x^y)");
DATA(insert OID = 233 (  dexp			   PGNSP PGUID 12 f f t f i 1 701 "701"  dexp - _null_ ));
DESCR("natural exponential (e^x)");
DATA(insert OID = 234 (  dlog1			   PGNSP PGUID 12 f f t f i 1 701 "701"  dlog1 - _null_ ));
DESCR("natural logarithm");
DATA(insert OID = 235 (  float8			   PGNSP PGUID 12 f f t f i 1 701  "21"  i2tod - _null_ ));
DESCR("convert int2 to float8");
DATA(insert OID = 236 (  float4			   PGNSP PGUID 12 f f t f i 1 700  "21"  i2tof - _null_ ));
DESCR("convert int2 to float4");
DATA(insert OID = 237 (  int2			   PGNSP PGUID 12 f f t f i 1  21 "701"  dtoi2 - _null_ ));
DESCR("convert float8 to int2");
DATA(insert OID = 238 (  int2			   PGNSP PGUID 12 f f t f i 1  21 "700"  ftoi2 - _null_ ));
DESCR("convert float4 to int2");
DATA(insert OID = 239 (  line_distance	   PGNSP PGUID 12 f f t f i 2 701 "628 628"  line_distance - _null_ ));
DESCR("distance between");

DATA(insert OID = 240 (  abstimein		   PGNSP PGUID 12 f f t f s 1 702 "2275"  abstimein - _null_ ));
DESCR("I/O");
DATA(insert OID = 241 (  abstimeout		   PGNSP PGUID 12 f f t f s 1 2275 "702"  abstimeout - _null_ ));
DESCR("I/O");
DATA(insert OID = 242 (  reltimein		   PGNSP PGUID 12 f f t f s 1 703 "2275"  reltimein - _null_ ));
DESCR("I/O");
DATA(insert OID = 243 (  reltimeout		   PGNSP PGUID 12 f f t f s 1 2275 "703"  reltimeout - _null_ ));
DESCR("I/O");
DATA(insert OID = 244 (  timepl			   PGNSP PGUID 12 f f t f i 2 702 "702 703"  timepl - _null_ ));
DESCR("add");
DATA(insert OID = 245 (  timemi			   PGNSP PGUID 12 f f t f i 2 702 "702 703"  timemi - _null_ ));
DESCR("subtract");
DATA(insert OID = 246 (  tintervalin	   PGNSP PGUID 12 f f t f s 1 704 "2275"  tintervalin - _null_ ));
DESCR("I/O");
DATA(insert OID = 247 (  tintervalout	   PGNSP PGUID 12 f f t f s 1 2275 "704"  tintervalout - _null_ ));
DESCR("I/O");
DATA(insert OID = 248 (  intinterval	   PGNSP PGUID 12 f f t f i 2 16 "702 704"	intinterval - _null_ ));
DESCR("abstime in tinterval");
DATA(insert OID = 249 (  tintervalrel	   PGNSP PGUID 12 f f t f i 1 703 "704"  tintervalrel - _null_ ));
DESCR("tinterval to reltime");
DATA(insert OID = 250 (  timenow		   PGNSP PGUID 12 f f t f s 0 702 ""  timenow - _null_ ));
DESCR("Current date and time (abstime)");
DATA(insert OID = 251 (  abstimeeq		   PGNSP PGUID 12 f f t f i 2 16 "702 702"	abstimeeq - _null_ ));
DESCR("equal");
DATA(insert OID = 252 (  abstimene		   PGNSP PGUID 12 f f t f i 2 16 "702 702"	abstimene - _null_ ));
DESCR("not equal");
DATA(insert OID = 253 (  abstimelt		   PGNSP PGUID 12 f f t f i 2 16 "702 702"	abstimelt - _null_ ));
DESCR("less-than");
DATA(insert OID = 254 (  abstimegt		   PGNSP PGUID 12 f f t f i 2 16 "702 702"	abstimegt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 255 (  abstimele		   PGNSP PGUID 12 f f t f i 2 16 "702 702"	abstimele - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 256 (  abstimege		   PGNSP PGUID 12 f f t f i 2 16 "702 702"	abstimege - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 257 (  reltimeeq		   PGNSP PGUID 12 f f t f i 2 16 "703 703"	reltimeeq - _null_ ));
DESCR("equal");
DATA(insert OID = 258 (  reltimene		   PGNSP PGUID 12 f f t f i 2 16 "703 703"	reltimene - _null_ ));
DESCR("not equal");
DATA(insert OID = 259 (  reltimelt		   PGNSP PGUID 12 f f t f i 2 16 "703 703"	reltimelt - _null_ ));
DESCR("less-than");
DATA(insert OID = 260 (  reltimegt		   PGNSP PGUID 12 f f t f i 2 16 "703 703"	reltimegt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 261 (  reltimele		   PGNSP PGUID 12 f f t f i 2 16 "703 703"	reltimele - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 262 (  reltimege		   PGNSP PGUID 12 f f t f i 2 16 "703 703"	reltimege - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 263 (  tintervalsame	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervalsame - _null_ ));
DESCR("same as?");
DATA(insert OID = 264 (  tintervalct	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervalct - _null_ ));
DESCR("less-than");
DATA(insert OID = 265 (  tintervalov	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervalov - _null_ ));
DESCR("overlaps");
DATA(insert OID = 266 (  tintervalleneq    PGNSP PGUID 12 f f t f i 2 16 "704 703"	tintervalleneq - _null_ ));
DESCR("length equal");
DATA(insert OID = 267 (  tintervallenne    PGNSP PGUID 12 f f t f i 2 16 "704 703"	tintervallenne - _null_ ));
DESCR("length not equal to");
DATA(insert OID = 268 (  tintervallenlt    PGNSP PGUID 12 f f t f i 2 16 "704 703"	tintervallenlt - _null_ ));
DESCR("length less-than");
DATA(insert OID = 269 (  tintervallengt    PGNSP PGUID 12 f f t f i 2 16 "704 703"	tintervallengt - _null_ ));
DESCR("length greater-than");
DATA(insert OID = 270 (  tintervallenle    PGNSP PGUID 12 f f t f i 2 16 "704 703"	tintervallenle - _null_ ));
DESCR("length less-than-or-equal");
DATA(insert OID = 271 (  tintervallenge    PGNSP PGUID 12 f f t f i 2 16 "704 703"	tintervallenge - _null_ ));
DESCR("length greater-than-or-equal");
DATA(insert OID = 272 (  tintervalstart    PGNSP PGUID 12 f f t f i 1 702 "704"  tintervalstart - _null_ ));
DESCR("start of interval");
DATA(insert OID = 273 (  tintervalend	   PGNSP PGUID 12 f f t f i 1 702 "704"  tintervalend - _null_ ));
DESCR("end of interval");
DATA(insert OID = 274 (  timeofday		   PGNSP PGUID 12 f f t f v 0 25 "" timeofday - _null_ ));
DESCR("Current date and time - increments during transactions");
DATA(insert OID = 275 (  isfinite		   PGNSP PGUID 12 f f t f i 1 16 "702"	abstime_finite - _null_ ));
DESCR("finite abstime?");

DATA(insert OID = 276 (  int2fac		   PGNSP PGUID 12 f f t f i 1 23 "21"  int2fac - _null_ ));
DESCR("factorial");

DATA(insert OID = 277 (  inter_sl		   PGNSP PGUID 12 f f t f i 2 16 "601 628"	inter_sl - _null_ ));
DESCR("intersect?");
DATA(insert OID = 278 (  inter_lb		   PGNSP PGUID 12 f f t f i 2 16 "628 603"	inter_lb - _null_ ));
DESCR("intersect?");

DATA(insert OID = 279 (  float48mul		   PGNSP PGUID 12 f f t f i 2 701 "700 701"  float48mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 280 (  float48div		   PGNSP PGUID 12 f f t f i 2 701 "700 701"  float48div - _null_ ));
DESCR("divide");
DATA(insert OID = 281 (  float48pl		   PGNSP PGUID 12 f f t f i 2 701 "700 701"  float48pl - _null_ ));
DESCR("add");
DATA(insert OID = 282 (  float48mi		   PGNSP PGUID 12 f f t f i 2 701 "700 701"  float48mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 283 (  float84mul		   PGNSP PGUID 12 f f t f i 2 701 "701 700"  float84mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 284 (  float84div		   PGNSP PGUID 12 f f t f i 2 701 "701 700"  float84div - _null_ ));
DESCR("divide");
DATA(insert OID = 285 (  float84pl		   PGNSP PGUID 12 f f t f i 2 701 "701 700"  float84pl - _null_ ));
DESCR("add");
DATA(insert OID = 286 (  float84mi		   PGNSP PGUID 12 f f t f i 2 701 "701 700"  float84mi - _null_ ));
DESCR("subtract");

DATA(insert OID = 287 (  float4eq		   PGNSP PGUID 12 f f t f i 2 16 "700 700"	float4eq - _null_ ));
DESCR("equal");
DATA(insert OID = 288 (  float4ne		   PGNSP PGUID 12 f f t f i 2 16 "700 700"	float4ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 289 (  float4lt		   PGNSP PGUID 12 f f t f i 2 16 "700 700"	float4lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 290 (  float4le		   PGNSP PGUID 12 f f t f i 2 16 "700 700"	float4le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 291 (  float4gt		   PGNSP PGUID 12 f f t f i 2 16 "700 700"	float4gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 292 (  float4ge		   PGNSP PGUID 12 f f t f i 2 16 "700 700"	float4ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 293 (  float8eq		   PGNSP PGUID 12 f f t f i 2 16 "701 701"	float8eq - _null_ ));
DESCR("equal");
DATA(insert OID = 294 (  float8ne		   PGNSP PGUID 12 f f t f i 2 16 "701 701"	float8ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 295 (  float8lt		   PGNSP PGUID 12 f f t f i 2 16 "701 701"	float8lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 296 (  float8le		   PGNSP PGUID 12 f f t f i 2 16 "701 701"	float8le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 297 (  float8gt		   PGNSP PGUID 12 f f t f i 2 16 "701 701"	float8gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 298 (  float8ge		   PGNSP PGUID 12 f f t f i 2 16 "701 701"	float8ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 299 (  float48eq		   PGNSP PGUID 12 f f t f i 2 16 "700 701"	float48eq - _null_ ));
DESCR("equal");

/* OIDS 300 - 399 */

DATA(insert OID = 300 (  float48ne		   PGNSP PGUID 12 f f t f i 2 16 "700 701"	float48ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 301 (  float48lt		   PGNSP PGUID 12 f f t f i 2 16 "700 701"	float48lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 302 (  float48le		   PGNSP PGUID 12 f f t f i 2 16 "700 701"	float48le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 303 (  float48gt		   PGNSP PGUID 12 f f t f i 2 16 "700 701"	float48gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 304 (  float48ge		   PGNSP PGUID 12 f f t f i 2 16 "700 701"	float48ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 305 (  float84eq		   PGNSP PGUID 12 f f t f i 2 16 "701 700"	float84eq - _null_ ));
DESCR("equal");
DATA(insert OID = 306 (  float84ne		   PGNSP PGUID 12 f f t f i 2 16 "701 700"	float84ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 307 (  float84lt		   PGNSP PGUID 12 f f t f i 2 16 "701 700"	float84lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 308 (  float84le		   PGNSP PGUID 12 f f t f i 2 16 "701 700"	float84le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 309 (  float84gt		   PGNSP PGUID 12 f f t f i 2 16 "701 700"	float84gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 310 (  float84ge		   PGNSP PGUID 12 f f t f i 2 16 "701 700"	float84ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 311 (  float8			   PGNSP PGUID 12 f f t f i 1 701 "700"  ftod - _null_ ));
DESCR("convert float4 to float8");
DATA(insert OID = 312 (  float4			   PGNSP PGUID 12 f f t f i 1 700 "701"  dtof - _null_ ));
DESCR("convert float8 to float4");
DATA(insert OID = 313 (  int4			   PGNSP PGUID 12 f f t f i 1  23  "21"  i2toi4 - _null_ ));
DESCR("convert int2 to int4");
DATA(insert OID = 314 (  int2			   PGNSP PGUID 12 f f t f i 1  21  "23"  i4toi2 - _null_ ));
DESCR("convert int4 to int2");
DATA(insert OID = 315 (  int2vectoreq	   PGNSP PGUID 12 f f t f i 2  16  "22 22"	int2vectoreq - _null_ ));
DESCR("equal");
DATA(insert OID = 316 (  float8			   PGNSP PGUID 12 f f t f i 1 701  "23"  i4tod - _null_ ));
DESCR("convert int4 to float8");
DATA(insert OID = 317 (  int4			   PGNSP PGUID 12 f f t f i 1  23 "701"  dtoi4 - _null_ ));
DESCR("convert float8 to int4");
DATA(insert OID = 318 (  float4			   PGNSP PGUID 12 f f t f i 1 700  "23"  i4tof - _null_ ));
DESCR("convert int4 to float4");
DATA(insert OID = 319 (  int4			   PGNSP PGUID 12 f f t f i 1  23 "700"  ftoi4 - _null_ ));
DESCR("convert float4 to int4");

DATA(insert OID = 320 (  rtinsert		   PGNSP PGUID 12 f f t f v 6 2281 "2281 2281 2281 2281 2281 2281"	rtinsert - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 322 (  rtgettuple		   PGNSP PGUID 12 f f t f v 2 16 "2281 2281"  rtgettuple - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 323 (  rtbuild		   PGNSP PGUID 12 f f t f v 3 2278 "2281 2281 2281" rtbuild - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 324 (  rtbeginscan	   PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281"  rtbeginscan - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 325 (  rtendscan		   PGNSP PGUID 12 f f t f v 1 2278 "2281"	rtendscan - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 326 (  rtmarkpos		   PGNSP PGUID 12 f f t f v 1 2278 "2281"	rtmarkpos - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 327 (  rtrestrpos		   PGNSP PGUID 12 f f t f v 1 2278 "2281"	rtrestrpos - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 328 (  rtrescan		   PGNSP PGUID 12 f f t f v 2 2278 "2281 2281"	rtrescan - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 321 (  rtbulkdelete	   PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281" rtbulkdelete - _null_ ));
DESCR("r-tree(internal)");
DATA(insert OID = 1265 (  rtcostestimate   PGNSP PGUID 12 f f t f v 8 2278 "2281 2281 2281 2281 2281 2281 2281 2281"  rtcostestimate - _null_ ));
DESCR("r-tree(internal)");

DATA(insert OID = 330 (  btgettuple		   PGNSP PGUID 12 f f t f v 2 16 "2281 2281"  btgettuple - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 331 (  btinsert		   PGNSP PGUID 12 f f t f v 6 2281 "2281 2281 2281 2281 2281 2281"	btinsert - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 333 (  btbeginscan	   PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281"  btbeginscan - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 334 (  btrescan		   PGNSP PGUID 12 f f t f v 2 2278 "2281 2281"	btrescan - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 335 (  btendscan		   PGNSP PGUID 12 f f t f v 1 2278 "2281"	btendscan - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 336 (  btmarkpos		   PGNSP PGUID 12 f f t f v 1 2278 "2281"	btmarkpos - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 337 (  btrestrpos		   PGNSP PGUID 12 f f t f v 1 2278 "2281"	btrestrpos - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 338 (  btbuild		   PGNSP PGUID 12 f f t f v 3 2278 "2281 2281 2281" btbuild - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 332 (  btbulkdelete	   PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281" btbulkdelete - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 972 (  btvacuumcleanup   PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281" btvacuumcleanup - _null_ ));
DESCR("btree(internal)");
DATA(insert OID = 1268 (  btcostestimate   PGNSP PGUID 12 f f t f v 8 2278 "2281 2281 2281 2281 2281 2281 2281 2281"  btcostestimate - _null_ ));
DESCR("btree(internal)");

DATA(insert OID = 339 (  poly_same		   PGNSP PGUID 12 f f t f i 2 16 "604 604"	poly_same - _null_ ));
DESCR("same as?");
DATA(insert OID = 340 (  poly_contain	   PGNSP PGUID 12 f f t f i 2 16 "604 604"	poly_contain - _null_ ));
DESCR("contains?");
DATA(insert OID = 341 (  poly_left		   PGNSP PGUID 12 f f t f i 2 16 "604 604"	poly_left - _null_ ));
DESCR("is left of");
DATA(insert OID = 342 (  poly_overleft	   PGNSP PGUID 12 f f t f i 2 16 "604 604"	poly_overleft - _null_ ));
DESCR("overlaps or is left of");
DATA(insert OID = 343 (  poly_overright    PGNSP PGUID 12 f f t f i 2 16 "604 604"	poly_overright - _null_ ));
DESCR("overlaps or is right of");
DATA(insert OID = 344 (  poly_right		   PGNSP PGUID 12 f f t f i 2 16 "604 604"	poly_right - _null_ ));
DESCR("is right of");
DATA(insert OID = 345 (  poly_contained    PGNSP PGUID 12 f f t f i 2 16 "604 604"	poly_contained - _null_ ));
DESCR("contained in?");
DATA(insert OID = 346 (  poly_overlap	   PGNSP PGUID 12 f f t f i 2 16 "604 604"	poly_overlap - _null_ ));
DESCR("overlaps");
DATA(insert OID = 347 (  poly_in		   PGNSP PGUID 12 f f t f i 1 604 "2275"  poly_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 348 (  poly_out		   PGNSP PGUID 12 f f t f i 1 2275 "604"  poly_out - _null_ ));
DESCR("I/O");

DATA(insert OID = 350 (  btint2cmp		   PGNSP PGUID 12 f f t f i 2 23 "21 21"	btint2cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 351 (  btint4cmp		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	btint4cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 842 (  btint8cmp		   PGNSP PGUID 12 f f t f i 2 23 "20 20"	btint8cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 354 (  btfloat4cmp	   PGNSP PGUID 12 f f t f i 2 23 "700 700"	btfloat4cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 355 (  btfloat8cmp	   PGNSP PGUID 12 f f t f i 2 23 "701 701"	btfloat8cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 356 (  btoidcmp		   PGNSP PGUID 12 f f t f i 2 23 "26 26"	btoidcmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 404 (  btoidvectorcmp    PGNSP PGUID 12 f f t f i 2 23 "30 30"	btoidvectorcmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 357 (  btabstimecmp	   PGNSP PGUID 12 f f t f i 2 23 "702 702"	btabstimecmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 358 (  btcharcmp		   PGNSP PGUID 12 f f t f i 2 23 "18 18"	btcharcmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 359 (  btnamecmp		   PGNSP PGUID 12 f f t f i 2 23 "19 19"	btnamecmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 360 (  bttextcmp		   PGNSP PGUID 12 f f t f i 2 23 "25 25"	bttextcmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 377 (  cash_cmp		   PGNSP PGUID 12 f f t f i 2 23 "790 790"	cash_cmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 380 (  btreltimecmp	   PGNSP PGUID 12 f f t f i 2 23 "703 703"	btreltimecmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 381 (  bttintervalcmp	   PGNSP PGUID 12 f f t f i 2 23 "704 704"	bttintervalcmp - _null_ ));
DESCR("btree less-equal-greater");
DATA(insert OID = 382 (  btarraycmp		   PGNSP PGUID 12 f f t f i 2 23 "2277 2277"	btarraycmp - _null_ ));
DESCR("btree less-equal-greater");

DATA(insert OID = 361 (  lseg_distance	   PGNSP PGUID 12 f f t f i 2 701 "601 601"  lseg_distance - _null_ ));
DESCR("distance between");
DATA(insert OID = 362 (  lseg_interpt	   PGNSP PGUID 12 f f t f i 2 600 "601 601"  lseg_interpt - _null_ ));
DESCR("intersection point");
DATA(insert OID = 363 (  dist_ps		   PGNSP PGUID 12 f f t f i 2 701 "600 601"  dist_ps - _null_ ));
DESCR("distance between");
DATA(insert OID = 364 (  dist_pb		   PGNSP PGUID 12 f f t f i 2 701 "600 603"  dist_pb - _null_ ));
DESCR("distance between point and box");
DATA(insert OID = 365 (  dist_sb		   PGNSP PGUID 12 f f t f i 2 701 "601 603"  dist_sb - _null_ ));
DESCR("distance between segment and box");
DATA(insert OID = 366 (  close_ps		   PGNSP PGUID 12 f f t f i 2 600 "600 601"  close_ps - _null_ ));
DESCR("closest point on line segment");
DATA(insert OID = 367 (  close_pb		   PGNSP PGUID 12 f f t f i 2 600 "600 603"  close_pb - _null_ ));
DESCR("closest point on box");
DATA(insert OID = 368 (  close_sb		   PGNSP PGUID 12 f f t f i 2 600 "601 603"  close_sb - _null_ ));
DESCR("closest point to line segment on box");
DATA(insert OID = 369 (  on_ps			   PGNSP PGUID 12 f f t f i 2 16 "600 601"	on_ps - _null_ ));
DESCR("point contained in segment?");
DATA(insert OID = 370 (  path_distance	   PGNSP PGUID 12 f f t f i 2 701 "602 602"  path_distance - _null_ ));
DESCR("distance between paths");
DATA(insert OID = 371 (  dist_ppath		   PGNSP PGUID 12 f f t f i 2 701 "600 602"  dist_ppath - _null_ ));
DESCR("distance between point and path");
DATA(insert OID = 372 (  on_sb			   PGNSP PGUID 12 f f t f i 2 16 "601 603"	on_sb - _null_ ));
DESCR("lseg contained in box?");
DATA(insert OID = 373 (  inter_sb		   PGNSP PGUID 12 f f t f i 2 16 "601 603"	inter_sb - _null_ ));
DESCR("intersect?");

/* OIDS 400 - 499 */

DATA(insert OID =  401 (  text			   PGNSP PGUID 12 f f t f i 1 25 "1042"  rtrim1 - _null_ ));
DESCR("convert char(n) to text");
DATA(insert OID =  406 (  text			   PGNSP PGUID 12 f f t f i 1 25 "19" name_text - _null_ ));
DESCR("convert name to text");
DATA(insert OID =  407 (  name			   PGNSP PGUID 12 f f t f i 1 19 "25" text_name - _null_ ));
DESCR("convert text to name");
DATA(insert OID =  408 (  bpchar		   PGNSP PGUID 12 f f t f i 1 1042 "19" name_bpchar - _null_ ));
DESCR("convert name to char(n)");
DATA(insert OID =  409 (  name			   PGNSP PGUID 12 f f t f i 1 19 "1042"  bpchar_name - _null_ ));
DESCR("convert char(n) to name");

DATA(insert OID = 440 (  hashgettuple	   PGNSP PGUID 12 f f t f v 2 16 "2281 2281"  hashgettuple - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 441 (  hashinsert		   PGNSP PGUID 12 f f t f v 6 2281 "2281 2281 2281 2281 2281 2281"	hashinsert - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 443 (  hashbeginscan	   PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281"  hashbeginscan - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 444 (  hashrescan		   PGNSP PGUID 12 f f t f v 2 2278 "2281 2281"	hashrescan - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 445 (  hashendscan	   PGNSP PGUID 12 f f t f v 1 2278 "2281"	hashendscan - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 446 (  hashmarkpos	   PGNSP PGUID 12 f f t f v 1 2278 "2281"	hashmarkpos - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 447 (  hashrestrpos	   PGNSP PGUID 12 f f t f v 1 2278 "2281"	hashrestrpos - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 448 (  hashbuild		   PGNSP PGUID 12 f f t f v 3 2278 "2281 2281 2281" hashbuild - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 442 (  hashbulkdelete    PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281" hashbulkdelete - _null_ ));
DESCR("hash(internal)");
DATA(insert OID = 438 (  hashcostestimate  PGNSP PGUID 12 f f t f v 8 2278 "2281 2281 2281 2281 2281 2281 2281 2281"  hashcostestimate - _null_ ));
DESCR("hash(internal)");

DATA(insert OID = 449 (  hashint2		   PGNSP PGUID 12 f f t f i 1 23 "21"  hashint2 - _null_ ));
DESCR("hash");
DATA(insert OID = 450 (  hashint4		   PGNSP PGUID 12 f f t f i 1 23 "23"  hashint4 - _null_ ));
DESCR("hash");
DATA(insert OID = 949 (  hashint8		   PGNSP PGUID 12 f f t f i 1 23 "20"  hashint8 - _null_ ));
DESCR("hash");
DATA(insert OID = 451 (  hashfloat4		   PGNSP PGUID 12 f f t f i 1 23 "700"	hashfloat4 - _null_ ));
DESCR("hash");
DATA(insert OID = 452 (  hashfloat8		   PGNSP PGUID 12 f f t f i 1 23 "701"	hashfloat8 - _null_ ));
DESCR("hash");
DATA(insert OID = 453 (  hashoid		   PGNSP PGUID 12 f f t f i 1 23 "26"  hashoid - _null_ ));
DESCR("hash");
DATA(insert OID = 454 (  hashchar		   PGNSP PGUID 12 f f t f i 1 23 "18"  hashchar - _null_ ));
DESCR("hash");
DATA(insert OID = 455 (  hashname		   PGNSP PGUID 12 f f t f i 1 23 "19"  hashname - _null_ ));
DESCR("hash");
DATA(insert OID = 400 (  hashtext		   PGNSP PGUID 12 f f t f i 1 23 "25" hashtext - _null_ ));
DESCR("hash");
DATA(insert OID = 456 (  hashvarlena	   PGNSP PGUID 12 f f t f i 1 23 "2281" hashvarlena - _null_ ));
DESCR("hash any varlena type");
DATA(insert OID = 457 (  hashoidvector	   PGNSP PGUID 12 f f t f i 1 23 "30"  hashoidvector - _null_ ));
DESCR("hash");
DATA(insert OID = 329 (  hash_aclitem	   PGNSP PGUID 12 f f t f i 1 23 "1033"  hash_aclitem - _null_ ));
DESCR("hash");
DATA(insert OID = 398 (  hashint2vector    PGNSP PGUID 12 f f t f i 1 23 "22"  hashint2vector - _null_ ));
DESCR("hash");
DATA(insert OID = 399 (  hashmacaddr	   PGNSP PGUID 12 f f t f i 1 23 "829"	hashmacaddr - _null_ ));
DESCR("hash");
DATA(insert OID = 458 (  text_larger	   PGNSP PGUID 12 f f t f i 2 25 "25 25"	text_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 459 (  text_smaller	   PGNSP PGUID 12 f f t f i 2 25 "25 25"	text_smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 460 (  int8in			   PGNSP PGUID 12 f f t f i 1 20 "2275" int8in - _null_ ));
DESCR("I/O");
DATA(insert OID = 461 (  int8out		   PGNSP PGUID 12 f f t f i 1 2275 "20" int8out - _null_ ));
DESCR("I/O");
DATA(insert OID = 462 (  int8um			   PGNSP PGUID 12 f f t f i 1 20 "20"  int8um - _null_ ));
DESCR("negate");
DATA(insert OID = 463 (  int8pl			   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8pl - _null_ ));
DESCR("add");
DATA(insert OID = 464 (  int8mi			   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 465 (  int8mul		   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 466 (  int8div		   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8div - _null_ ));
DESCR("divide");
DATA(insert OID = 467 (  int8eq			   PGNSP PGUID 12 f f t f i 2 16 "20 20"	int8eq - _null_ ));
DESCR("equal");
DATA(insert OID = 468 (  int8ne			   PGNSP PGUID 12 f f t f i 2 16 "20 20"	int8ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 469 (  int8lt			   PGNSP PGUID 12 f f t f i 2 16 "20 20"	int8lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 470 (  int8gt			   PGNSP PGUID 12 f f t f i 2 16 "20 20"	int8gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 471 (  int8le			   PGNSP PGUID 12 f f t f i 2 16 "20 20"	int8le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 472 (  int8ge			   PGNSP PGUID 12 f f t f i 2 16 "20 20"	int8ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 474 (  int84eq		   PGNSP PGUID 12 f f t f i 2 16 "20 23"	int84eq - _null_ ));
DESCR("equal");
DATA(insert OID = 475 (  int84ne		   PGNSP PGUID 12 f f t f i 2 16 "20 23"	int84ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 476 (  int84lt		   PGNSP PGUID 12 f f t f i 2 16 "20 23"	int84lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 477 (  int84gt		   PGNSP PGUID 12 f f t f i 2 16 "20 23"	int84gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 478 (  int84le		   PGNSP PGUID 12 f f t f i 2 16 "20 23"	int84le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 479 (  int84ge		   PGNSP PGUID 12 f f t f i 2 16 "20 23"	int84ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 480 (  int4			   PGNSP PGUID 12 f f t f i 1  23 "20"	int84 - _null_ ));
DESCR("convert int8 to int4");
DATA(insert OID = 481 (  int8			   PGNSP PGUID 12 f f t f i 1  20 "23"	int48 - _null_ ));
DESCR("convert int4 to int8");
DATA(insert OID = 482 (  float8			   PGNSP PGUID 12 f f t f i 1 701 "20"	i8tod - _null_ ));
DESCR("convert int8 to float8");
DATA(insert OID = 483 (  int8			   PGNSP PGUID 12 f f t f i 1  20 "701"  dtoi8 - _null_ ));
DESCR("convert float8 to int8");

/* OIDS 500 - 599 */

/* OIDS 600 - 699 */

DATA(insert OID = 652 (  float4			   PGNSP PGUID 12 f f t f i 1 700 "20"	i8tof - _null_ ));
DESCR("convert int8 to float4");
DATA(insert OID = 653 (  int8			   PGNSP PGUID 12 f f t f i 1  20 "700"  ftoi8 - _null_ ));
DESCR("convert float4 to int8");

DATA(insert OID = 714 (  int2			   PGNSP PGUID 12 f f t f i 1  21 "20"	int82 - _null_ ));
DESCR("convert int8 to int2");
DATA(insert OID = 754 (  int8			   PGNSP PGUID 12 f f t f i 1  20 "21"	int28 - _null_ ));
DESCR("convert int2 to int8");

DATA(insert OID = 1285 (  int4notin		   PGNSP PGUID 12 f f t f s 2 16 "23 25"	int4notin - _null_ ));
DESCR("not in");
DATA(insert OID = 1286 (  oidnotin		   PGNSP PGUID 12 f f t f s 2 16 "26 25"	oidnotin - _null_ ));
DESCR("not in");
DATA(insert OID = 655 (  namelt			   PGNSP PGUID 12 f f t f i 2 16 "19 19"	namelt - _null_ ));
DESCR("less-than");
DATA(insert OID = 656 (  namele			   PGNSP PGUID 12 f f t f i 2 16 "19 19"	namele - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 657 (  namegt			   PGNSP PGUID 12 f f t f i 2 16 "19 19"	namegt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 658 (  namege			   PGNSP PGUID 12 f f t f i 2 16 "19 19"	namege - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 659 (  namene			   PGNSP PGUID 12 f f t f i 2 16 "19 19"	namene - _null_ ));
DESCR("not equal");

DATA(insert OID = 668 (  bpchar			   PGNSP PGUID 12 f f t f i 3 1042 "1042 23 16" bpchar - _null_ ));
DESCR("adjust char() to typmod length");
DATA(insert OID = 669 (  varchar		   PGNSP PGUID 12 f f t f i 3 1043 "1043 23 16" varchar - _null_ ));
DESCR("adjust varchar() to typmod length");

DATA(insert OID = 676 (  mktinterval	   PGNSP PGUID 12 f f t f i 2 704 "702 702" mktinterval - _null_ ));
DESCR("convert to tinterval");
DATA(insert OID = 619 (  oidvectorne	   PGNSP PGUID 12 f f t f i 2 16 "30 30"	oidvectorne - _null_ ));
DESCR("not equal");
DATA(insert OID = 677 (  oidvectorlt	   PGNSP PGUID 12 f f t f i 2 16 "30 30"	oidvectorlt - _null_ ));
DESCR("less-than");
DATA(insert OID = 678 (  oidvectorle	   PGNSP PGUID 12 f f t f i 2 16 "30 30"	oidvectorle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 679 (  oidvectoreq	   PGNSP PGUID 12 f f t f i 2 16 "30 30"	oidvectoreq - _null_ ));
DESCR("equal");
DATA(insert OID = 680 (  oidvectorge	   PGNSP PGUID 12 f f t f i 2 16 "30 30"	oidvectorge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 681 (  oidvectorgt	   PGNSP PGUID 12 f f t f i 2 16 "30 30"	oidvectorgt - _null_ ));
DESCR("greater-than");

/* OIDS 700 - 799 */
DATA(insert OID = 710 (  getpgusername	   PGNSP PGUID 12 f f t f s 0 19 "" current_user - _null_ ));
DESCR("deprecated -- use current_user");
DATA(insert OID = 716 (  oidlt			   PGNSP PGUID 12 f f t f i 2 16 "26 26"	oidlt - _null_ ));
DESCR("less-than");
DATA(insert OID = 717 (  oidle			   PGNSP PGUID 12 f f t f i 2 16 "26 26"	oidle - _null_ ));
DESCR("less-than-or-equal");

DATA(insert OID = 720 (  octet_length	   PGNSP PGUID 12 f f t f i 1 23 "17"  byteaoctetlen - _null_ ));
DESCR("octet length");
DATA(insert OID = 721 (  get_byte		   PGNSP PGUID 12 f f t f i 2 23 "17 23"	byteaGetByte - _null_ ));
DESCR("get byte");
DATA(insert OID = 722 (  set_byte		   PGNSP PGUID 12 f f t f i 3 17 "17 23 23"  byteaSetByte - _null_ ));
DESCR("set byte");
DATA(insert OID = 723 (  get_bit		   PGNSP PGUID 12 f f t f i 2 23 "17 23"	byteaGetBit - _null_ ));
DESCR("get bit");
DATA(insert OID = 724 (  set_bit		   PGNSP PGUID 12 f f t f i 3 17 "17 23 23"  byteaSetBit - _null_ ));
DESCR("set bit");

DATA(insert OID = 725 (  dist_pl		   PGNSP PGUID 12 f f t f i 2 701 "600 628"  dist_pl - _null_ ));
DESCR("distance between point and line");
DATA(insert OID = 726 (  dist_lb		   PGNSP PGUID 12 f f t f i 2 701 "628 603"  dist_lb - _null_ ));
DESCR("distance between line and box");
DATA(insert OID = 727 (  dist_sl		   PGNSP PGUID 12 f f t f i 2 701 "601 628"  dist_sl - _null_ ));
DESCR("distance between lseg and line");
DATA(insert OID = 728 (  dist_cpoly		   PGNSP PGUID 12 f f t f i 2 701 "718 604"  dist_cpoly - _null_ ));
DESCR("distance between");
DATA(insert OID = 729 (  poly_distance	   PGNSP PGUID 12 f f t f i 2 701 "604 604"  poly_distance - _null_ ));
DESCR("distance between");

DATA(insert OID = 740 (  text_lt		   PGNSP PGUID 12 f f t f i 2 16 "25 25"	text_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 741 (  text_le		   PGNSP PGUID 12 f f t f i 2 16 "25 25"	text_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 742 (  text_gt		   PGNSP PGUID 12 f f t f i 2 16 "25 25"	text_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 743 (  text_ge		   PGNSP PGUID 12 f f t f i 2 16 "25 25"	text_ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 745 (  current_user	   PGNSP PGUID 12 f f t f s 0 19 "" current_user - _null_ ));
DESCR("current user name");
DATA(insert OID = 746 (  session_user	   PGNSP PGUID 12 f f t f s 0 19 "" session_user - _null_ ));
DESCR("session user name");

DATA(insert OID = 744 (  array_eq		   PGNSP PGUID 12 f f t f i 2 16 "2277 2277" array_eq - _null_ ));
DESCR("array equal");
DATA(insert OID = 390 (  array_ne		   PGNSP PGUID 12 f f t f i 2 16 "2277 2277" array_ne - _null_ ));
DESCR("array not equal");
DATA(insert OID = 391 (  array_lt		   PGNSP PGUID 12 f f t f i 2 16 "2277 2277" array_lt - _null_ ));
DESCR("array less than");
DATA(insert OID = 392 (  array_gt		   PGNSP PGUID 12 f f t f i 2 16 "2277 2277" array_gt - _null_ ));
DESCR("array greater than");
DATA(insert OID = 393 (  array_le		   PGNSP PGUID 12 f f t f i 2 16 "2277 2277" array_le - _null_ ));
DESCR("array less than or equal");
DATA(insert OID = 396 (  array_ge		   PGNSP PGUID 12 f f t f i 2 16 "2277 2277" array_ge - _null_ ));
DESCR("array greater than or equal");
DATA(insert OID = 747 (  array_dims		   PGNSP PGUID 12 f f t f i 1 25 "2277" array_dims - _null_ ));
DESCR("array dimensions");
DATA(insert OID = 750 (  array_in		   PGNSP PGUID 12 f f t f s 3 2277 "2275 26 23"  array_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 751 (  array_out		   PGNSP PGUID 12 f f t f s 1 2275 "2277"  array_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2091 (  array_lower	   PGNSP PGUID 12 f f t f i 2 23 "2277 23" array_lower - _null_ ));
DESCR("array lower dimension");
DATA(insert OID = 2092 (  array_upper	   PGNSP PGUID 12 f f t f i 2 23 "2277 23" array_upper - _null_ ));
DESCR("array upper dimension");
DATA(insert OID = 378 (  array_append	   PGNSP PGUID 12 f f t f i 2 2277 "2277 2283" array_push - _null_ ));
DESCR("append element onto end of array");
DATA(insert OID = 379 (  array_prepend	   PGNSP PGUID 12 f f t f i 2 2277 "2283 2277" array_push - _null_ ));
DESCR("prepend element onto front of array");
DATA(insert OID = 383 (  array_cat		   PGNSP PGUID 12 f f t f i 2 2277 "2277 2277" array_cat - _null_ ));
DESCR("concatenate two arrays");
DATA(insert OID = 384  (  array_coerce	   PGNSP PGUID 12 f f t f i 1 2277 "2277" array_type_coerce - _null_ ));
DESCR("coerce array type to another array type");
DATA(insert OID = 394 (  string_to_array   PGNSP PGUID 12 f f t f i 2 1009 "25 25" text_to_array - _null_ ));
DESCR("split delimited text into text[]");
DATA(insert OID = 395 (  array_to_string   PGNSP PGUID 12 f f t f i 2 25 "2277 25" array_to_text - _null_ ));
DESCR("concatenate array elements, using delimiter, into text");

DATA(insert OID = 760 (  smgrin			   PGNSP PGUID 12 f f t f s 1 210 "2275"  smgrin - _null_ ));
DESCR("I/O");
DATA(insert OID = 761 (  smgrout		   PGNSP PGUID 12 f f t f s 1 2275 "210"  smgrout - _null_ ));
DESCR("I/O");
DATA(insert OID = 762 (  smgreq			   PGNSP PGUID 12 f f t f i 2 16 "210 210"	smgreq - _null_ ));
DESCR("storage manager");
DATA(insert OID = 763 (  smgrne			   PGNSP PGUID 12 f f t f i 2 16 "210 210"	smgrne - _null_ ));
DESCR("storage manager");

DATA(insert OID = 764 (  lo_import		   PGNSP PGUID 12 f f t f v 1 26 "25"  lo_import - _null_ ));
DESCR("large object import");
DATA(insert OID = 765 (  lo_export		   PGNSP PGUID 12 f f t f v 2 23 "26 25"	lo_export - _null_ ));
DESCR("large object export");

DATA(insert OID = 766 (  int4inc		   PGNSP PGUID 12 f f t f i 1 23 "23"  int4inc - _null_ ));
DESCR("increment");
DATA(insert OID = 768 (  int4larger		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 769 (  int4smaller	   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 770 (  int2larger		   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 771 (  int2smaller	   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 774 (  gistgettuple	   PGNSP PGUID 12 f f t f v 2 16 "2281 2281"  gistgettuple - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 775 (  gistinsert		   PGNSP PGUID 12 f f t f v 6 2281 "2281 2281 2281 2281 2281 2281"	gistinsert - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 777 (  gistbeginscan	   PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281"  gistbeginscan - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 778 (  gistrescan		   PGNSP PGUID 12 f f t f v 2 2278 "2281 2281"	gistrescan - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 779 (  gistendscan	   PGNSP PGUID 12 f f t f v 1 2278 "2281"	gistendscan - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 780 (  gistmarkpos	   PGNSP PGUID 12 f f t f v 1 2278 "2281"	gistmarkpos - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 781 (  gistrestrpos	   PGNSP PGUID 12 f f t f v 1 2278 "2281"	gistrestrpos - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 782 (  gistbuild		   PGNSP PGUID 12 f f t f v 3 2278 "2281 2281 2281" gistbuild - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 776 (  gistbulkdelete    PGNSP PGUID 12 f f t f v 3 2281 "2281 2281 2281" gistbulkdelete - _null_ ));
DESCR("gist(internal)");
DATA(insert OID = 772 (  gistcostestimate  PGNSP PGUID 12 f f t f v 8 2278 "2281 2281 2281 2281 2281 2281 2281 2281"  gistcostestimate - _null_ ));
DESCR("gist(internal)");

DATA(insert OID = 784 (  tintervaleq	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervaleq - _null_ ));
DESCR("equal");
DATA(insert OID = 785 (  tintervalne	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervalne - _null_ ));
DESCR("not equal");
DATA(insert OID = 786 (  tintervallt	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervallt - _null_ ));
DESCR("less-than");
DATA(insert OID = 787 (  tintervalgt	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervalgt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 788 (  tintervalle	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervalle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 789 (  tintervalge	   PGNSP PGUID 12 f f t f i 2 16 "704 704"	tintervalge - _null_ ));
DESCR("greater-than-or-equal");

/* OIDS 800 - 899 */

DATA(insert OID = 817 (  oid			   PGNSP PGUID 12 f f t f i 1 26 "25"  text_oid - _null_ ));
DESCR("convert text to oid");
DATA(insert OID = 818 (  int2			   PGNSP PGUID 12 f f t f i 1 21 "25"  text_int2 - _null_ ));
DESCR("convert text to int2");
DATA(insert OID = 819 (  int4			   PGNSP PGUID 12 f f t f i 1 23 "25"  text_int4 - _null_ ));
DESCR("convert text to int4");

DATA(insert OID = 838 (  float8			   PGNSP PGUID 12 f f t f i 1 701 "25"	text_float8 - _null_ ));
DESCR("convert text to float8");
DATA(insert OID = 839 (  float4			   PGNSP PGUID 12 f f t f i 1 700 "25"	text_float4 - _null_ ));
DESCR("convert text to float4");
DATA(insert OID = 840 (  text			   PGNSP PGUID 12 f f t f i 1  25 "701"  float8_text - _null_ ));
DESCR("convert float8 to text");
DATA(insert OID = 841 (  text			   PGNSP PGUID 12 f f t f i 1  25 "700"  float4_text - _null_ ));
DESCR("convert float4 to text");

DATA(insert OID =  846 (  cash_mul_flt4    PGNSP PGUID 12 f f t f i 2 790 "790 700"  cash_mul_flt4 - _null_ ));
DESCR("multiply");
DATA(insert OID =  847 (  cash_div_flt4    PGNSP PGUID 12 f f t f i 2 790 "790 700"  cash_div_flt4 - _null_ ));
DESCR("divide");
DATA(insert OID =  848 (  flt4_mul_cash    PGNSP PGUID 12 f f t f i 2 790 "700 790"  flt4_mul_cash - _null_ ));
DESCR("multiply");

DATA(insert OID =  849 (  position		   PGNSP PGUID 12 f f t f i 2 23 "25 25" textpos - _null_ ));
DESCR("return position of substring");
DATA(insert OID =  850 (  textlike		   PGNSP PGUID 12 f f t f i 2 16 "25 25" textlike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID =  851 (  textnlike		   PGNSP PGUID 12 f f t f i 2 16 "25 25" textnlike - _null_ ));
DESCR("does not match LIKE expression");

DATA(insert OID =  852 (  int48eq		   PGNSP PGUID 12 f f t f i 2 16 "23 20"	int48eq - _null_ ));
DESCR("equal");
DATA(insert OID =  853 (  int48ne		   PGNSP PGUID 12 f f t f i 2 16 "23 20"	int48ne - _null_ ));
DESCR("not equal");
DATA(insert OID =  854 (  int48lt		   PGNSP PGUID 12 f f t f i 2 16 "23 20"	int48lt - _null_ ));
DESCR("less-than");
DATA(insert OID =  855 (  int48gt		   PGNSP PGUID 12 f f t f i 2 16 "23 20"	int48gt - _null_ ));
DESCR("greater-than");
DATA(insert OID =  856 (  int48le		   PGNSP PGUID 12 f f t f i 2 16 "23 20"	int48le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID =  857 (  int48ge		   PGNSP PGUID 12 f f t f i 2 16 "23 20"	int48ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID =  858 (  namelike		   PGNSP PGUID 12 f f t f i 2 16 "19 25"	namelike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID =  859 (  namenlike		   PGNSP PGUID 12 f f t f i 2 16 "19 25"	namenlike - _null_ ));
DESCR("does not match LIKE expression");

DATA(insert OID =  860 (  bpchar		   PGNSP PGUID 12 f f t f i 1 1042 "18"  char_bpchar - _null_ ));
DESCR("convert char to char()");

DATA(insert OID = 861 ( current_database	   PGNSP PGUID 12 f f t f i 0 19 "" current_database - _null_ ));
DESCR("returns the current database");

DATA(insert OID =  862 (  int4_mul_cash		   PGNSP PGUID 12 f f t f i 2 790 "23 790"	int4_mul_cash - _null_ ));
DESCR("multiply");
DATA(insert OID =  863 (  int2_mul_cash		   PGNSP PGUID 12 f f t f i 2 790 "21 790"	int2_mul_cash - _null_ ));
DESCR("multiply");
DATA(insert OID =  864 (  cash_mul_int4		   PGNSP PGUID 12 f f t f i 2 790 "790 23"	cash_mul_int4 - _null_ ));
DESCR("multiply");
DATA(insert OID =  865 (  cash_div_int4		   PGNSP PGUID 12 f f t f i 2 790 "790 23"	cash_div_int4 - _null_ ));
DESCR("divide");
DATA(insert OID =  866 (  cash_mul_int2		   PGNSP PGUID 12 f f t f i 2 790 "790 21"	cash_mul_int2 - _null_ ));
DESCR("multiply");
DATA(insert OID =  867 (  cash_div_int2		   PGNSP PGUID 12 f f t f i 2 790 "790 21"	cash_div_int2 - _null_ ));
DESCR("divide");

DATA(insert OID =  886 (  cash_in		   PGNSP PGUID 12 f f t f i 1 790 "2275"  cash_in - _null_ ));
DESCR("I/O");
DATA(insert OID =  887 (  cash_out		   PGNSP PGUID 12 f f t f i 1 2275 "790"  cash_out - _null_ ));
DESCR("I/O");
DATA(insert OID =  888 (  cash_eq		   PGNSP PGUID 12 f f t f i 2  16 "790 790"  cash_eq - _null_ ));
DESCR("equal");
DATA(insert OID =  889 (  cash_ne		   PGNSP PGUID 12 f f t f i 2  16 "790 790"  cash_ne - _null_ ));
DESCR("not equal");
DATA(insert OID =  890 (  cash_lt		   PGNSP PGUID 12 f f t f i 2  16 "790 790"  cash_lt - _null_ ));
DESCR("less-than");
DATA(insert OID =  891 (  cash_le		   PGNSP PGUID 12 f f t f i 2  16 "790 790"  cash_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID =  892 (  cash_gt		   PGNSP PGUID 12 f f t f i 2  16 "790 790"  cash_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID =  893 (  cash_ge		   PGNSP PGUID 12 f f t f i 2  16 "790 790"  cash_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID =  894 (  cash_pl		   PGNSP PGUID 12 f f t f i 2 790 "790 790"  cash_pl - _null_ ));
DESCR("add");
DATA(insert OID =  895 (  cash_mi		   PGNSP PGUID 12 f f t f i 2 790 "790 790"  cash_mi - _null_ ));
DESCR("subtract");
DATA(insert OID =  896 (  cash_mul_flt8    PGNSP PGUID 12 f f t f i 2 790 "790 701"  cash_mul_flt8 - _null_ ));
DESCR("multiply");
DATA(insert OID =  897 (  cash_div_flt8    PGNSP PGUID 12 f f t f i 2 790 "790 701"  cash_div_flt8 - _null_ ));
DESCR("divide");
DATA(insert OID =  898 (  cashlarger	   PGNSP PGUID 12 f f t f i 2 790 "790 790"  cashlarger - _null_ ));
DESCR("larger of two");
DATA(insert OID =  899 (  cashsmaller	   PGNSP PGUID 12 f f t f i 2 790 "790 790"  cashsmaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID =  919 (  flt8_mul_cash    PGNSP PGUID 12 f f t f i 2 790 "701 790"  flt8_mul_cash - _null_ ));
DESCR("multiply");
DATA(insert OID =  935 (  cash_words	   PGNSP PGUID 12 f f t f i 1  25 "790"  cash_words - _null_ ));
DESCR("output amount as words");

/* OIDS 900 - 999 */

DATA(insert OID = 940 (  mod			   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 941 (  mod			   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 942 (  mod			   PGNSP PGUID 12 f f t f i 2 23 "21 23"	int24mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 943 (  mod			   PGNSP PGUID 12 f f t f i 2 23 "23 21"	int42mod - _null_ ));
DESCR("modulus");

DATA(insert OID = 945 (  int8mod		   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 947 (  mod			   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8mod - _null_ ));
DESCR("modulus");

DATA(insert OID = 944 (  char			   PGNSP PGUID 12 f f t f i 1 18 "25"  text_char - _null_ ));
DESCR("convert text to char");
DATA(insert OID = 946 (  text			   PGNSP PGUID 12 f f t f i 1 25 "18"  char_text - _null_ ));
DESCR("convert char to text");

DATA(insert OID = 950 (  istrue			   PGNSP PGUID 12 f f f f i 1 16 "16"  istrue - _null_ ));
DESCR("bool is true (not false or unknown)");
DATA(insert OID = 951 (  isfalse		   PGNSP PGUID 12 f f f f i 1 16 "16"  isfalse - _null_ ));
DESCR("bool is false (not true or unknown)");

DATA(insert OID = 952 (  lo_open		   PGNSP PGUID 12 f f t f v 2 23 "26 23"	lo_open - _null_ ));
DESCR("large object open");
DATA(insert OID = 953 (  lo_close		   PGNSP PGUID 12 f f t f v 1 23 "23"  lo_close - _null_ ));
DESCR("large object close");
DATA(insert OID = 954 (  loread			   PGNSP PGUID 12 f f t f v 2 17 "23 23"	loread - _null_ ));
DESCR("large object read");
DATA(insert OID = 955 (  lowrite		   PGNSP PGUID 12 f f t f v 2 23 "23 17"	lowrite - _null_ ));
DESCR("large object write");
DATA(insert OID = 956 (  lo_lseek		   PGNSP PGUID 12 f f t f v 3 23 "23 23 23"  lo_lseek - _null_ ));
DESCR("large object seek");
DATA(insert OID = 957 (  lo_creat		   PGNSP PGUID 12 f f t f v 1 26 "23"  lo_creat - _null_ ));
DESCR("large object create");
DATA(insert OID = 958 (  lo_tell		   PGNSP PGUID 12 f f t f v 1 23 "23"  lo_tell - _null_ ));
DESCR("large object position");

DATA(insert OID = 959 (  on_pl			   PGNSP PGUID 12 f f t f i 2  16 "600 628"  on_pl - _null_ ));
DESCR("point on line?");
DATA(insert OID = 960 (  on_sl			   PGNSP PGUID 12 f f t f i 2  16 "601 628"  on_sl - _null_ ));
DESCR("lseg on line?");
DATA(insert OID = 961 (  close_pl		   PGNSP PGUID 12 f f t f i 2 600 "600 628"  close_pl - _null_ ));
DESCR("closest point on line");
DATA(insert OID = 962 (  close_sl		   PGNSP PGUID 12 f f t f i 2 600 "601 628"  close_sl - _null_ ));
DESCR("closest point to line segment on line");
DATA(insert OID = 963 (  close_lb		   PGNSP PGUID 12 f f t f i 2 600 "628 603"  close_lb - _null_ ));
DESCR("closest point to line on box");

DATA(insert OID = 964 (  lo_unlink		   PGNSP PGUID 12 f f t f v 1  23 "26"	lo_unlink - _null_ ));
DESCR("large object unlink(delete)");

DATA(insert OID = 973 (  path_inter		   PGNSP PGUID 12 f f t f i 2  16 "602 602"  path_inter - _null_ ));
DESCR("intersect?");
DATA(insert OID = 975 (  area			   PGNSP PGUID 12 f f t f i 1 701 "603"  box_area - _null_ ));
DESCR("box area");
DATA(insert OID = 976 (  width			   PGNSP PGUID 12 f f t f i 1 701 "603"  box_width - _null_ ));
DESCR("box width");
DATA(insert OID = 977 (  height			   PGNSP PGUID 12 f f t f i 1 701 "603"  box_height - _null_ ));
DESCR("box height");
DATA(insert OID = 978 (  box_distance	   PGNSP PGUID 12 f f t f i 2 701 "603 603"  box_distance - _null_ ));
DESCR("distance between boxes");
DATA(insert OID = 980 (  box_intersect	   PGNSP PGUID 12 f f t f i 2 603 "603 603"  box_intersect - _null_ ));
DESCR("box intersection (another box)");
DATA(insert OID = 981 (  diagonal		   PGNSP PGUID 12 f f t f i 1 601 "603"  box_diagonal - _null_ ));
DESCR("box diagonal");
DATA(insert OID = 982 (  path_n_lt		   PGNSP PGUID 12 f f t f i 2 16 "602 602"	path_n_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 983 (  path_n_gt		   PGNSP PGUID 12 f f t f i 2 16 "602 602"	path_n_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 984 (  path_n_eq		   PGNSP PGUID 12 f f t f i 2 16 "602 602"	path_n_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 985 (  path_n_le		   PGNSP PGUID 12 f f t f i 2 16 "602 602"	path_n_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 986 (  path_n_ge		   PGNSP PGUID 12 f f t f i 2 16 "602 602"	path_n_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 987 (  path_length	   PGNSP PGUID 12 f f t f i 1 701 "602"  path_length - _null_ ));
DESCR("sum of path segment lengths");
DATA(insert OID = 988 (  point_ne		   PGNSP PGUID 12 f f t f i 2 16 "600 600"	point_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 989 (  point_vert		   PGNSP PGUID 12 f f t f i 2 16 "600 600"	point_vert - _null_ ));
DESCR("vertically aligned?");
DATA(insert OID = 990 (  point_horiz	   PGNSP PGUID 12 f f t f i 2 16 "600 600"	point_horiz - _null_ ));
DESCR("horizontally aligned?");
DATA(insert OID = 991 (  point_distance    PGNSP PGUID 12 f f t f i 2 701 "600 600"  point_distance - _null_ ));
DESCR("distance between");
DATA(insert OID = 992 (  slope			   PGNSP PGUID 12 f f t f i 2 701 "600 600"  point_slope - _null_ ));
DESCR("slope between points");
DATA(insert OID = 993 (  lseg			   PGNSP PGUID 12 f f t f i 2 601 "600 600"  lseg_construct - _null_ ));
DESCR("convert points to line segment");
DATA(insert OID = 994 (  lseg_intersect    PGNSP PGUID 12 f f t f i 2 16 "601 601"	lseg_intersect - _null_ ));
DESCR("intersect?");
DATA(insert OID = 995 (  lseg_parallel	   PGNSP PGUID 12 f f t f i 2 16 "601 601"	lseg_parallel - _null_ ));
DESCR("parallel?");
DATA(insert OID = 996 (  lseg_perp		   PGNSP PGUID 12 f f t f i 2 16 "601 601"	lseg_perp - _null_ ));
DESCR("perpendicular?");
DATA(insert OID = 997 (  lseg_vertical	   PGNSP PGUID 12 f f t f i 1 16 "601"	lseg_vertical - _null_ ));
DESCR("vertical?");
DATA(insert OID = 998 (  lseg_horizontal   PGNSP PGUID 12 f f t f i 1 16 "601"	lseg_horizontal - _null_ ));
DESCR("horizontal?");
DATA(insert OID = 999 (  lseg_eq		   PGNSP PGUID 12 f f t f i 2 16 "601 601"	lseg_eq - _null_ ));
DESCR("equal");

DATA(insert OID =  748 (  date			   PGNSP PGUID 12 f f t f s 1 1082 "25" text_date - _null_ ));
DESCR("convert text to date");
DATA(insert OID =  749 (  text			   PGNSP PGUID 12 f f t f s 1 25 "1082" date_text - _null_ ));
DESCR("convert date to text");
DATA(insert OID =  837 (  time			   PGNSP PGUID 12 f f t f s 1 1083 "25" text_time - _null_ ));
DESCR("convert text to time");
DATA(insert OID =  948 (  text			   PGNSP PGUID 12 f f t f i 1 25 "1083" time_text - _null_ ));
DESCR("convert time to text");
DATA(insert OID =  938 (  timetz		   PGNSP PGUID 12 f f t f s 1 1266 "25" text_timetz - _null_ ));
DESCR("convert text to timetz");
DATA(insert OID =  939 (  text			   PGNSP PGUID 12 f f t f i 1 25 "1266" timetz_text - _null_ ));
DESCR("convert timetz to text");

/* OIDS 1000 - 1999 */

DATA(insert OID = 1026 (  timezone		   PGNSP PGUID 12 f f t f s 2 1114 "1186 1184"	timestamptz_izone - _null_ ));
DESCR("adjust timestamp to new time zone");

DATA(insert OID = 1029 (  nullvalue		   PGNSP PGUID 12 f f f f i 1 16 "2276" nullvalue - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1030 (  nonnullvalue	   PGNSP PGUID 12 f f f f i 1 16 "2276" nonnullvalue - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1031 (  aclitemin		   PGNSP PGUID 12 f f t f s 1 1033 "2275"  aclitemin - _null_ ));
DESCR("I/O");
DATA(insert OID = 1032 (  aclitemout	   PGNSP PGUID 12 f f t f s 1 2275 "1033"  aclitemout - _null_ ));
DESCR("I/O");
DATA(insert OID = 1035 (  aclinsert		   PGNSP PGUID 12 f f t f s 2 1034 "1034 1033"	aclinsert - _null_ ));
DESCR("add/update ACL item");
DATA(insert OID = 1036 (  aclremove		   PGNSP PGUID 12 f f t f s 2 1034 "1034 1033"	aclremove - _null_ ));
DESCR("remove ACL item");
DATA(insert OID = 1037 (  aclcontains	   PGNSP PGUID 12 f f t f s 2 16 "1034 1033"	aclcontains - _null_ ));
DESCR("does ACL contain item?");
DATA(insert OID = 1062 (  aclitemeq		   PGNSP PGUID 12 f f t f s 2 16 "1033 1033"	aclitem_eq - _null_ ));
DESCR("equality operator for ACL items");
DATA(insert OID = 1365 (  makeaclitem	   PGNSP PGUID 12 f f t f s 5 1033 "23 23 23 25 16" makeaclitem - _null_ ));
DESCR("make ACL item");
DATA(insert OID = 1038 (  seteval		   PGNSP PGUID 12 f f t t v 1 23 "26"  seteval - _null_ ));
DESCR("internal function supporting PostQuel-style sets");
DATA(insert OID = 1044 (  bpcharin		   PGNSP PGUID 12 f f t f i 3 1042 "2275 26 23" bpcharin - _null_ ));
DESCR("I/O");
DATA(insert OID = 1045 (  bpcharout		   PGNSP PGUID 12 f f t f i 1 2275 "1042"	bpcharout - _null_ ));
DESCR("I/O");
DATA(insert OID = 1046 (  varcharin		   PGNSP PGUID 12 f f t f i 3 1043 "2275 26 23" varcharin - _null_ ));
DESCR("I/O");
DATA(insert OID = 1047 (  varcharout	   PGNSP PGUID 12 f f t f i 1 2275 "1043"	varcharout - _null_ ));
DESCR("I/O");
DATA(insert OID = 1048 (  bpchareq		   PGNSP PGUID 12 f f t f i 2 16 "1042 1042"	bpchareq - _null_ ));
DESCR("equal");
DATA(insert OID = 1049 (  bpcharlt		   PGNSP PGUID 12 f f t f i 2 16 "1042 1042"	bpcharlt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1050 (  bpcharle		   PGNSP PGUID 12 f f t f i 2 16 "1042 1042"	bpcharle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1051 (  bpchargt		   PGNSP PGUID 12 f f t f i 2 16 "1042 1042"	bpchargt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1052 (  bpcharge		   PGNSP PGUID 12 f f t f i 2 16 "1042 1042"	bpcharge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1053 (  bpcharne		   PGNSP PGUID 12 f f t f i 2 16 "1042 1042"	bpcharne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1078 (  bpcharcmp		   PGNSP PGUID 12 f f t f i 2 23 "1042 1042"	bpcharcmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1080 (  hashbpchar	   PGNSP PGUID 12 f f t f i 1 23 "1042"  hashbpchar - _null_ ));
DESCR("hash");
DATA(insert OID = 1081 (  format_type	   PGNSP PGUID 12 f f f f s 2 25 "26 23" format_type - _null_ ));
DESCR("format a type oid and atttypmod to canonical SQL");
DATA(insert OID = 1084 (  date_in		   PGNSP PGUID 12 f f t f s 1 1082 "2275"	date_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1085 (  date_out		   PGNSP PGUID 12 f f t f s 1 2275 "1082"	date_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1086 (  date_eq		   PGNSP PGUID 12 f f t f i 2 16 "1082 1082"	date_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1087 (  date_lt		   PGNSP PGUID 12 f f t f i 2 16 "1082 1082"	date_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1088 (  date_le		   PGNSP PGUID 12 f f t f i 2 16 "1082 1082"	date_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1089 (  date_gt		   PGNSP PGUID 12 f f t f i 2 16 "1082 1082"	date_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1090 (  date_ge		   PGNSP PGUID 12 f f t f i 2 16 "1082 1082"	date_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1091 (  date_ne		   PGNSP PGUID 12 f f t f i 2 16 "1082 1082"	date_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1092 (  date_cmp		   PGNSP PGUID 12 f f t f i 2 23 "1082 1082"	date_cmp - _null_ ));
DESCR("less-equal-greater");

/* OIDS 1100 - 1199 */

DATA(insert OID = 1102 (  time_lt		   PGNSP PGUID 12 f f t f i 2 16 "1083 1083"	time_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1103 (  time_le		   PGNSP PGUID 12 f f t f i 2 16 "1083 1083"	time_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1104 (  time_gt		   PGNSP PGUID 12 f f t f i 2 16 "1083 1083"	time_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1105 (  time_ge		   PGNSP PGUID 12 f f t f i 2 16 "1083 1083"	time_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1106 (  time_ne		   PGNSP PGUID 12 f f t f i 2 16 "1083 1083"	time_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1107 (  time_cmp		   PGNSP PGUID 12 f f t f i 2 23 "1083 1083"	time_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1138 (  date_larger	   PGNSP PGUID 12 f f t f i 2 1082 "1082 1082"	date_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1139 (  date_smaller	   PGNSP PGUID 12 f f t f i 2 1082 "1082 1082"	date_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 1140 (  date_mi		   PGNSP PGUID 12 f f t f i 2 23 "1082 1082"	date_mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1141 (  date_pli		   PGNSP PGUID 12 f f t f i 2 1082 "1082 23"	date_pli - _null_ ));
DESCR("add");
DATA(insert OID = 1142 (  date_mii		   PGNSP PGUID 12 f f t f i 2 1082 "1082 23"	date_mii - _null_ ));
DESCR("subtract");
DATA(insert OID = 1143 (  time_in		   PGNSP PGUID 12 f f t f s 3 1083 "2275 26 23" time_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1144 (  time_out		   PGNSP PGUID 12 f f t f i 1 2275 "1083"	time_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1145 (  time_eq		   PGNSP PGUID 12 f f t f i 2 16 "1083 1083"	time_eq - _null_ ));
DESCR("equal");

DATA(insert OID = 1146 (  circle_add_pt    PGNSP PGUID 12 f f t f i 2 718 "718 600"  circle_add_pt - _null_ ));
DESCR("add");
DATA(insert OID = 1147 (  circle_sub_pt    PGNSP PGUID 12 f f t f i 2 718 "718 600"  circle_sub_pt - _null_ ));
DESCR("subtract");
DATA(insert OID = 1148 (  circle_mul_pt    PGNSP PGUID 12 f f t f i 2 718 "718 600"  circle_mul_pt - _null_ ));
DESCR("multiply");
DATA(insert OID = 1149 (  circle_div_pt    PGNSP PGUID 12 f f t f i 2 718 "718 600"  circle_div_pt - _null_ ));
DESCR("divide");

DATA(insert OID = 1150 (  timestamptz_in   PGNSP PGUID 12 f f t f s 3 1184 "2275 26 23" timestamptz_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1151 (  timestamptz_out  PGNSP PGUID 12 f f t f s 1 2275 "1184"	timestamptz_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1152 (  timestamptz_eq   PGNSP PGUID 12 f f t f i 2 16 "1184 1184"	timestamp_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1153 (  timestamptz_ne   PGNSP PGUID 12 f f t f i 2 16 "1184 1184"	timestamp_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1154 (  timestamptz_lt   PGNSP PGUID 12 f f t f i 2 16 "1184 1184"	timestamp_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1155 (  timestamptz_le   PGNSP PGUID 12 f f t f i 2 16 "1184 1184"	timestamp_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1156 (  timestamptz_ge   PGNSP PGUID 12 f f t f i 2 16 "1184 1184"	timestamp_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1157 (  timestamptz_gt   PGNSP PGUID 12 f f t f i 2 16 "1184 1184"	timestamp_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1159 (  timezone		   PGNSP PGUID 12 f f t f s 2 1114 "25 1184"  timestamptz_zone - _null_ ));
DESCR("adjust timestamp to new time zone");

DATA(insert OID = 1160 (  interval_in	   PGNSP PGUID 12 f f t f s 3 1186 "2275 26 23" interval_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1161 (  interval_out	   PGNSP PGUID 12 f f t f i 1 2275 "1186"	interval_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1162 (  interval_eq	   PGNSP PGUID 12 f f t f i 2 16 "1186 1186"	interval_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1163 (  interval_ne	   PGNSP PGUID 12 f f t f i 2 16 "1186 1186"	interval_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1164 (  interval_lt	   PGNSP PGUID 12 f f t f i 2 16 "1186 1186"	interval_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1165 (  interval_le	   PGNSP PGUID 12 f f t f i 2 16 "1186 1186"	interval_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1166 (  interval_ge	   PGNSP PGUID 12 f f t f i 2 16 "1186 1186"	interval_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1167 (  interval_gt	   PGNSP PGUID 12 f f t f i 2 16 "1186 1186"	interval_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1168 (  interval_um	   PGNSP PGUID 12 f f t f i 1 1186 "1186"  interval_um - _null_ ));
DESCR("subtract");
DATA(insert OID = 1169 (  interval_pl	   PGNSP PGUID 12 f f t f i 2 1186 "1186 1186"	interval_pl - _null_ ));
DESCR("add");
DATA(insert OID = 1170 (  interval_mi	   PGNSP PGUID 12 f f t f i 2 1186 "1186 1186"	interval_mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1171 (  date_part		   PGNSP PGUID 12 f f t f s 2  701 "25 1184"	timestamptz_part - _null_ ));
DESCR("extract field from timestamp with time zone");
DATA(insert OID = 1172 (  date_part		   PGNSP PGUID 12 f f t f i 2  701 "25 1186"	interval_part - _null_ ));
DESCR("extract field from interval");
DATA(insert OID = 1173 (  timestamptz	   PGNSP PGUID 12 f f t f s 1 1184 "702"	abstime_timestamptz - _null_ ));
DESCR("convert abstime to timestamp with time zone");
DATA(insert OID = 1174 (  timestamptz	   PGNSP PGUID 12 f f t f s 1 1184 "1082"  date_timestamptz - _null_ ));
DESCR("convert date to timestamp with time zone");
DATA(insert OID = 1176 (  timestamptz	   PGNSP PGUID 14 f f t f s 2 1184 "1082 1083"	"select cast(($1 + $2) as timestamp with time zone)" - _null_ ));
DESCR("convert date and time to timestamp with time zone");
DATA(insert OID = 1177 (  interval		   PGNSP PGUID 12 f f t f i 1 1186 "703"	reltime_interval - _null_ ));
DESCR("convert reltime to interval");
DATA(insert OID = 1178 (  date			   PGNSP PGUID 12 f f t f s 1 1082 "1184"  timestamptz_date - _null_ ));
DESCR("convert timestamp with time zone to date");
DATA(insert OID = 1179 (  date			   PGNSP PGUID 12 f f t f s 1 1082 "702"	abstime_date - _null_ ));
DESCR("convert abstime to date");
DATA(insert OID = 1180 (  abstime		   PGNSP PGUID 12 f f t f s 1  702 "1184"  timestamptz_abstime - _null_ ));
DESCR("convert timestamp with time zone to abstime");
DATA(insert OID = 1181 (  age			   PGNSP PGUID 12 f f t f s 1 23 "28"  xid_age - _null_ ));
DESCR("age of a transaction ID, in transactions before current transaction");

DATA(insert OID = 1188 (  timestamptz_mi   PGNSP PGUID 12 f f t f i 2 1186 "1184 1184"	timestamp_mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1189 (  timestamptz_pl_span PGNSP PGUID 12 f f t f i 2 1184 "1184 1186"  timestamptz_pl_span - _null_ ));
DESCR("plus");
DATA(insert OID = 1190 (  timestamptz_mi_span PGNSP PGUID 12 f f t f i 2 1184 "1184 1186"  timestamptz_mi_span - _null_ ));
DESCR("minus");
DATA(insert OID = 1191 (  timestamptz		PGNSP PGUID 12 f f t f s 1 1184 "25"	text_timestamptz - _null_ ));
DESCR("convert text to timestamp with time zone");
DATA(insert OID = 1192 (  text				PGNSP PGUID 12 f f t f s 1	 25 "1184"	timestamptz_text - _null_ ));
DESCR("convert timestamp with time zone to text");
DATA(insert OID = 1193 (  text				PGNSP PGUID 12 f f t f i 1	 25 "1186"	interval_text - _null_ ));
DESCR("convert interval to text");
DATA(insert OID = 1194 (  reltime			PGNSP PGUID 12 f f t f i 1	703 "1186"	interval_reltime - _null_ ));
DESCR("convert interval to reltime");
DATA(insert OID = 1195 (  timestamptz_smaller PGNSP PGUID 12 f f t f i 2 1184 "1184 1184"  timestamp_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 1196 (  timestamptz_larger  PGNSP PGUID 12 f f t f i 2 1184 "1184 1184"  timestamp_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1197 (  interval_smaller	PGNSP PGUID 12 f f t f i 2 1186 "1186 1186"  interval_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 1198 (  interval_larger	PGNSP PGUID 12 f f t f i 2 1186 "1186 1186"  interval_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1199 (  age				PGNSP PGUID 12 f f t f i 2 1186 "1184 1184"  timestamptz_age - _null_ ));
DESCR("date difference preserving months and years");

/* OIDS 1200 - 1299 */

DATA(insert OID = 1200 (  interval			PGNSP PGUID 12 f f t f i 2 1186 "1186 23"	interval_scale - _null_ ));
DESCR("adjust interval precision");

DATA(insert OID = 1215 (  obj_description	PGNSP PGUID 14 f f t f s 2	25 "26 19"	"select description from pg_catalog.pg_description where objoid = $1 and classoid = (select oid from pg_catalog.pg_class where relname = $2 and relnamespace = PGNSP) and objsubid = 0" - _null_ ));
DESCR("get description for object id and catalog name");
DATA(insert OID = 1216 (  col_description	PGNSP PGUID 14 f f t f s 2	25 "26 23"	"select description from pg_catalog.pg_description where objoid = $1 and classoid = \'pg_catalog.pg_class\'::regclass and objsubid = $2" - _null_ ));
DESCR("get description for table column");

DATA(insert OID = 1217 (  date_trunc	   PGNSP PGUID 12 f f t f i 2 1184 "25 1184"	timestamptz_trunc - _null_ ));
DESCR("truncate timestamp with time zone to specified units");
DATA(insert OID = 1218 (  date_trunc	   PGNSP PGUID 12 f f t f i 2 1186 "25 1186"	interval_trunc - _null_ ));
DESCR("truncate interval to specified units");

DATA(insert OID = 1219 (  int8inc		   PGNSP PGUID 12 f f t f i 1 20 "20"  int8inc - _null_ ));
DESCR("increment");
DATA(insert OID = 1230 (  int8abs		   PGNSP PGUID 12 f f t f i 1 20 "20"  int8abs - _null_ ));
DESCR("absolute value");

DATA(insert OID = 1236 (  int8larger	   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1237 (  int8smaller	   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 1238 (  texticregexeq    PGNSP PGUID 12 f f t f i 2 16 "25 25"	texticregexeq - _null_ ));
DESCR("matches regex., case-insensitive");
DATA(insert OID = 1239 (  texticregexne    PGNSP PGUID 12 f f t f i 2 16 "25 25"	texticregexne - _null_ ));
DESCR("does not match regex., case-insensitive");
DATA(insert OID = 1240 (  nameicregexeq    PGNSP PGUID 12 f f t f i 2 16 "19 25"	nameicregexeq - _null_ ));
DESCR("matches regex., case-insensitive");
DATA(insert OID = 1241 (  nameicregexne    PGNSP PGUID 12 f f t f i 2 16 "19 25"	nameicregexne - _null_ ));
DESCR("does not match regex., case-insensitive");

DATA(insert OID = 1251 (  int4abs		   PGNSP PGUID 12 f f t f i 1 23 "23"  int4abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1253 (  int2abs		   PGNSP PGUID 12 f f t f i 1 21 "21"  int2abs - _null_ ));
DESCR("absolute value");

DATA(insert OID = 1263 (  interval		   PGNSP PGUID 12 f f t f s 1 1186 "25"  text_interval - _null_ ));
DESCR("convert text to interval");

DATA(insert OID = 1271 (  overlaps		   PGNSP PGUID 12 f f f f i 4 16 "1266 1266 1266 1266"	overlaps_timetz - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1272 (  datetime_pl	   PGNSP PGUID 12 f f t f i 2 1114 "1082 1083"	datetime_timestamp - _null_ ));
DESCR("convert date and time to timestamp");
DATA(insert OID = 1273 (  date_part		   PGNSP PGUID 12 f f t f i 2  701 "25 1266"	timetz_part - _null_ ));
DESCR("extract field from time with time zone");
DATA(insert OID = 1274 (  int84pl		   PGNSP PGUID 12 f f t f i 2 20 "20 23"	int84pl - _null_ ));
DESCR("add");
DATA(insert OID = 1275 (  int84mi		   PGNSP PGUID 12 f f t f i 2 20 "20 23"	int84mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1276 (  int84mul		   PGNSP PGUID 12 f f t f i 2 20 "20 23"	int84mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 1277 (  int84div		   PGNSP PGUID 12 f f t f i 2 20 "20 23"	int84div - _null_ ));
DESCR("divide");
DATA(insert OID = 1278 (  int48pl		   PGNSP PGUID 12 f f t f i 2 20 "23 20"	int48pl - _null_ ));
DESCR("add");
DATA(insert OID = 1279 (  int48mi		   PGNSP PGUID 12 f f t f i 2 20 "23 20"	int48mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 1280 (  int48mul		   PGNSP PGUID 12 f f t f i 2 20 "23 20"	int48mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 1281 (  int48div		   PGNSP PGUID 12 f f t f i 2 20 "23 20"	int48div - _null_ ));
DESCR("divide");

DATA(insert OID = 1287 (  oid			   PGNSP PGUID 12 f f t f i 1 26 "20"  i8tooid - _null_ ));
DESCR("convert int8 to oid");
DATA(insert OID = 1288 (  int8			   PGNSP PGUID 12 f f t f i 1 20 "26"  oidtoi8 - _null_ ));
DESCR("convert oid to int8");

DATA(insert OID = 1289 (  text			   PGNSP PGUID 12 f f t f i 1 25 "20"  int8_text - _null_ ));
DESCR("convert int8 to text");
DATA(insert OID = 1290 (  int8			   PGNSP PGUID 12 f f t f i 1 20 "25"  text_int8 - _null_ ));
DESCR("convert text to int8");

DATA(insert OID = 1291 (  array_length_coerce	PGNSP PGUID 12 f f t f i 3 2277 "2277 23 16"	array_length_coerce - _null_ ));
DESCR("adjust any array to element typmod length");

DATA(insert OID = 1292 ( tideq			   PGNSP PGUID 12 f f t f i 2 16 "27 27"	tideq - _null_ ));
DESCR("equal");
DATA(insert OID = 1293 ( currtid		   PGNSP PGUID 12 f f t f v 2 27 "26 27"	currtid_byreloid - _null_ ));
DESCR("latest tid of a tuple");
DATA(insert OID = 1294 ( currtid2		   PGNSP PGUID 12 f f t f v 2 27 "25 27"	currtid_byrelname - _null_ ));
DESCR("latest tid of a tuple");

DATA(insert OID = 1296 (  timedate_pl	   PGNSP PGUID 14 f f t f i 2 1114 "1083 1082"	"select ($2 + $1)" - _null_ ));
DESCR("convert time and date to timestamp");
DATA(insert OID = 1297 (  datetimetz_pl    PGNSP PGUID 12 f f t f i 2 1184 "1082 1266"	datetimetz_timestamptz - _null_ ));
DESCR("convert date and time with time zone to timestamp with time zone");
DATA(insert OID = 1298 (  timetzdate_pl    PGNSP PGUID 14 f f t f i 2 1184 "1266 1082"	"select ($2 + $1)" - _null_ ));
DESCR("convert time with time zone and date to timestamp with time zone");
DATA(insert OID = 1299 (  now			   PGNSP PGUID 12 f f t f s 0 1184 ""  now - _null_ ));
DESCR("current transaction time");

/* OIDS 1300 - 1399 */

DATA(insert OID = 1300 (  positionsel		   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  positionsel - _null_ ));
DESCR("restriction selectivity for position-comparison operators");
DATA(insert OID = 1301 (  positionjoinsel	   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  positionjoinsel - _null_ ));
DESCR("join selectivity for position-comparison operators");
DATA(insert OID = 1302 (  contsel		   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  contsel - _null_ ));
DESCR("restriction selectivity for containment comparison operators");
DATA(insert OID = 1303 (  contjoinsel	   PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  contjoinsel - _null_ ));
DESCR("join selectivity for containment comparison operators");

DATA(insert OID = 1304 ( overlaps			 PGNSP PGUID 12 f f f f i 4 16 "1184 1184 1184 1184"	overlaps_timestamp - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1305 ( overlaps			 PGNSP PGUID 14 f f f f i 4 16 "1184 1186 1184 1186"	"select ($1, ($1 + $2)) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1306 ( overlaps			 PGNSP PGUID 14 f f f f i 4 16 "1184 1184 1184 1186"	"select ($1, $2) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1307 ( overlaps			 PGNSP PGUID 14 f f f f i 4 16 "1184 1186 1184 1184"	"select ($1, ($1 + $2)) overlaps ($3, $4)" - _null_ ));
DESCR("SQL92 interval comparison");

DATA(insert OID = 1308 ( overlaps			 PGNSP PGUID 12 f f f f i 4 16 "1083 1083 1083 1083"	overlaps_time - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1309 ( overlaps			 PGNSP PGUID 14 f f f f i 4 16 "1083 1186 1083 1186"	"select ($1, ($1 + $2)) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1310 ( overlaps			 PGNSP PGUID 14 f f f f i 4 16 "1083 1083 1083 1186"	"select ($1, $2) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 1311 ( overlaps			 PGNSP PGUID 14 f f f f i 4 16 "1083 1186 1083 1083"	"select ($1, ($1 + $2)) overlaps ($3, $4)" - _null_ ));
DESCR("SQL92 interval comparison");

DATA(insert OID = 1312 (  timestamp_in		 PGNSP PGUID 12 f f t f s 3 1114 "2275 26 23" timestamp_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1313 (  timestamp_out		 PGNSP PGUID 12 f f t f s 1 2275 "1114" timestamp_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1314 (  timestamptz_cmp	 PGNSP PGUID 12 f f t f i 2 23 "1184 1184"	timestamp_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1315 (  interval_cmp		 PGNSP PGUID 12 f f t f i 2 23 "1186 1186"	interval_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1316 (  time				 PGNSP PGUID 12 f f t f i 1 1083 "1114"  timestamp_time - _null_ ));
DESCR("convert timestamp to time");

DATA(insert OID = 1317 (  length			 PGNSP PGUID 12 f f t f i 1 23 "25"  textlen - _null_ ));
DESCR("length");
DATA(insert OID = 1318 (  length			 PGNSP PGUID 12 f f t f i 1 23 "1042"  bpcharlen - _null_ ));
DESCR("character length");

DATA(insert OID = 1319 (  xideqint4			 PGNSP PGUID 12 f f t f i 2 16 "28 23"	xideq - _null_ ));
DESCR("equal");

DATA(insert OID = 1326 (  interval_div		 PGNSP PGUID 12 f f t f i 2 1186 "1186 701"  interval_div - _null_ ));
DESCR("divide");

DATA(insert OID = 1339 (  dlog10			 PGNSP PGUID 12 f f t f i 1 701 "701"  dlog10 - _null_ ));
DESCR("base 10 logarithm");
DATA(insert OID = 1340 (  log				 PGNSP PGUID 12 f f t f i 1 701 "701"  dlog10 - _null_ ));
DESCR("base 10 logarithm");
DATA(insert OID = 1341 (  ln				 PGNSP PGUID 12 f f t f i 1 701 "701"  dlog1 - _null_ ));
DESCR("natural logarithm");
DATA(insert OID = 1342 (  round				 PGNSP PGUID 12 f f t f i 1 701 "701"  dround - _null_ ));
DESCR("round to nearest integer");
DATA(insert OID = 1343 (  trunc				 PGNSP PGUID 12 f f t f i 1 701 "701"  dtrunc - _null_ ));
DESCR("truncate to integer");
DATA(insert OID = 1344 (  sqrt				 PGNSP PGUID 12 f f t f i 1 701 "701"  dsqrt - _null_ ));
DESCR("square root");
DATA(insert OID = 1345 (  cbrt				 PGNSP PGUID 12 f f t f i 1 701 "701"  dcbrt - _null_ ));
DESCR("cube root");
DATA(insert OID = 1346 (  pow				 PGNSP PGUID 12 f f t f i 2 701 "701 701"  dpow - _null_ ));
DESCR("exponentiation");
DATA(insert OID = 1347 (  exp				 PGNSP PGUID 12 f f t f i 1 701 "701"  dexp - _null_ ));
DESCR("exponential");

/*
 * This form of obj_description is now deprecated, since it will fail if
 * OIDs are not unique across system catalogs.	Use the other forms instead.
 */
DATA(insert OID = 1348 (  obj_description	 PGNSP PGUID 14 f f t f s 1 25 "26"  "select description from pg_catalog.pg_description where objoid = $1 and objsubid = 0" - _null_ ));
DESCR("get description for object id (deprecated)");
DATA(insert OID = 1349 (  oidvectortypes	 PGNSP PGUID 12 f f t f s 1 25 "30"  oidvectortypes - _null_ ));
DESCR("print type names of oidvector field");


DATA(insert OID = 1350 (  timetz_in		   PGNSP PGUID 12 f f t f s 3 1266 "2275 26 23" timetz_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1351 (  timetz_out	   PGNSP PGUID 12 f f t f i 1 2275 "1266"	timetz_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1352 (  timetz_eq		   PGNSP PGUID 12 f f t f i 2 16 "1266 1266"	timetz_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1353 (  timetz_ne		   PGNSP PGUID 12 f f t f i 2 16 "1266 1266"	timetz_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1354 (  timetz_lt		   PGNSP PGUID 12 f f t f i 2 16 "1266 1266"	timetz_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1355 (  timetz_le		   PGNSP PGUID 12 f f t f i 2 16 "1266 1266"	timetz_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1356 (  timetz_ge		   PGNSP PGUID 12 f f t f i 2 16 "1266 1266"	timetz_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1357 (  timetz_gt		   PGNSP PGUID 12 f f t f i 2 16 "1266 1266"	timetz_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1358 (  timetz_cmp	   PGNSP PGUID 12 f f t f i 2 23 "1266 1266"	timetz_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 1359 (  timestamptz	   PGNSP PGUID 12 f f t f i 2 1184 "1082 1266"	datetimetz_timestamptz - _null_ ));
DESCR("convert date and time with time zone to timestamp with time zone");

DATA(insert OID = 1364 (  time			   PGNSP PGUID 14 f f t f s 1 1083 "702"  "select cast(cast($1 as timestamp without time zone) as time)" - _null_ ));
DESCR("convert abstime to time");

DATA(insert OID = 1367 (  character_length	PGNSP PGUID 12 f f t f i 1	23 "1042"  bpcharlen - _null_ ));
DESCR("character length");
DATA(insert OID = 1369 (  character_length	PGNSP PGUID 12 f f t f i 1	23 "25"  textlen - _null_ ));
DESCR("character length");

DATA(insert OID = 1370 (  interval			 PGNSP PGUID 12 f f t f i 1 1186 "1083"  time_interval - _null_ ));
DESCR("convert time to interval");
DATA(insert OID = 1372 (  char_length		 PGNSP PGUID 12 f f t f i 1 23	 "1042"  bpcharlen - _null_ ));
DESCR("character length");

DATA(insert OID = 1374 (  octet_length			 PGNSP PGUID 12 f f t f i 1 23	 "25"  textoctetlen - _null_ ));
DESCR("octet length");
DATA(insert OID = 1375 (  octet_length			 PGNSP PGUID 12 f f t f i 1 23	 "1042"  bpcharoctetlen - _null_ ));
DESCR("octet length");

DATA(insert OID = 1377 (  time_larger	   PGNSP PGUID 12 f f t f i 2 1083 "1083 1083"	time_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1378 (  time_smaller	   PGNSP PGUID 12 f f t f i 2 1083 "1083 1083"	time_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 1379 (  timetz_larger    PGNSP PGUID 12 f f t f i 2 1266 "1266 1266"	timetz_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1380 (  timetz_smaller   PGNSP PGUID 12 f f t f i 2 1266 "1266 1266"	timetz_smaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 1381 (  char_length	   PGNSP PGUID 12 f f t f i 1 23 "25"  textlen - _null_ ));
DESCR("character length");

DATA(insert OID = 1382 (  date_part    PGNSP PGUID 14 f f t f s 2  701 "25 702"  "select pg_catalog.date_part($1, cast($2 as timestamp with time zone))" - _null_ ));
DESCR("extract field from abstime");
DATA(insert OID = 1383 (  date_part    PGNSP PGUID 14 f f t f s 2  701 "25 703"  "select pg_catalog.date_part($1, cast($2 as pg_catalog.interval))" - _null_ ));
DESCR("extract field from reltime");
DATA(insert OID = 1384 (  date_part    PGNSP PGUID 14 f f t f i 2  701 "25 1082"	"select pg_catalog.date_part($1, cast($2 as timestamp without time zone))" - _null_ ));
DESCR("extract field from date");
DATA(insert OID = 1385 (  date_part    PGNSP PGUID 12 f f t f i 2  701 "25 1083"  time_part - _null_ ));
DESCR("extract field from time");
DATA(insert OID = 1386 (  age		   PGNSP PGUID 14 f f t f s 1 1186 "1184"  "select pg_catalog.age(cast(current_date as timestamp with time zone), $1)" - _null_ ));
DESCR("date difference from today preserving months and years");

DATA(insert OID = 1388 (  timetz	   PGNSP PGUID 12 f f t f s 1 1266 "1184"  timestamptz_timetz - _null_ ));
DESCR("convert timestamptz to timetz");

DATA(insert OID = 1389 (  isfinite	   PGNSP PGUID 12 f f t f i 1 16 "1184"  timestamp_finite - _null_ ));
DESCR("finite timestamp?");
DATA(insert OID = 1390 (  isfinite	   PGNSP PGUID 12 f f t f i 1 16 "1186"  interval_finite - _null_ ));
DESCR("finite interval?");


DATA(insert OID = 1391 (  factorial		   PGNSP PGUID 12 f f t f i 1 23 "21"  int2fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 1392 (  factorial		   PGNSP PGUID 12 f f t f i 1 23 "23"  int4fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 1393 (  factorial		   PGNSP PGUID 12 f f t f i 1 20 "20"  int8fac - _null_ ));
DESCR("factorial");
DATA(insert OID = 1394 (  abs			   PGNSP PGUID 12 f f t f i 1 700 "700"  float4abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1395 (  abs			   PGNSP PGUID 12 f f t f i 1 701 "701"  float8abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1396 (  abs			   PGNSP PGUID 12 f f t f i 1 20 "20"  int8abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1397 (  abs			   PGNSP PGUID 12 f f t f i 1 23 "23"  int4abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1398 (  abs			   PGNSP PGUID 12 f f t f i 1 21 "21"  int2abs - _null_ ));
DESCR("absolute value");

/* OIDS 1400 - 1499 */

DATA(insert OID = 1400 (  name		   PGNSP PGUID 12 f f t f i 1 19 "1043"  text_name - _null_ ));
DESCR("convert varchar to name");
DATA(insert OID = 1401 (  varchar	   PGNSP PGUID 12 f f t f i 1 1043 "19"  name_text - _null_ ));
DESCR("convert name to varchar");

DATA(insert OID = 1402 (  current_schema	PGNSP PGUID 12 f f t f s 0	  19 "" current_schema - _null_ ));
DESCR("current schema name");
DATA(insert OID = 1403 (  current_schemas	PGNSP PGUID 12 f f t f s 1	1003 "16"	current_schemas - _null_ ));
DESCR("current schema search list");

DATA(insert OID = 1404 (  overlay			PGNSP PGUID 14 f f t f i 4 25 "25 25 23 23"  "select pg_catalog.substring($1, 1, ($3 - 1)) || $2 || pg_catalog.substring($1, ($3 + $4))" - _null_ ));
DESCR("substitute portion of string");
DATA(insert OID = 1405 (  overlay			PGNSP PGUID 14 f f t f i 3 25 "25 25 23"  "select pg_catalog.substring($1, 1, ($3 - 1)) || $2 || pg_catalog.substring($1, ($3 + pg_catalog.char_length($2)))" - _null_ ));
DESCR("substitute portion of string");

DATA(insert OID = 1406 (  isvertical		PGNSP PGUID 12 f f t f i 2	16 "600 600"  point_vert - _null_ ));
DESCR("vertically aligned?");
DATA(insert OID = 1407 (  ishorizontal		PGNSP PGUID 12 f f t f i 2	16 "600 600"  point_horiz - _null_ ));
DESCR("horizontally aligned?");
DATA(insert OID = 1408 (  isparallel		PGNSP PGUID 12 f f t f i 2	16 "601 601"  lseg_parallel - _null_ ));
DESCR("parallel?");
DATA(insert OID = 1409 (  isperp			PGNSP PGUID 12 f f t f i 2	16 "601 601"  lseg_perp - _null_ ));
DESCR("perpendicular?");
DATA(insert OID = 1410 (  isvertical		PGNSP PGUID 12 f f t f i 1	16 "601"  lseg_vertical - _null_ ));
DESCR("vertical?");
DATA(insert OID = 1411 (  ishorizontal		PGNSP PGUID 12 f f t f i 1	16 "601"  lseg_horizontal - _null_ ));
DESCR("horizontal?");
DATA(insert OID = 1412 (  isparallel		PGNSP PGUID 12 f f t f i 2	16 "628 628"  line_parallel - _null_ ));
DESCR("parallel?");
DATA(insert OID = 1413 (  isperp			PGNSP PGUID 12 f f t f i 2	16 "628 628"  line_perp - _null_ ));
DESCR("perpendicular?");
DATA(insert OID = 1414 (  isvertical		PGNSP PGUID 12 f f t f i 1	16 "628"  line_vertical - _null_ ));
DESCR("vertical?");
DATA(insert OID = 1415 (  ishorizontal		PGNSP PGUID 12 f f t f i 1	16 "628"  line_horizontal - _null_ ));
DESCR("horizontal?");
DATA(insert OID = 1416 (  point				PGNSP PGUID 12 f f t f i 1 600 "718"	circle_center - _null_ ));
DESCR("center of");

DATA(insert OID = 1417 (  isnottrue			PGNSP PGUID 12 f f f f i 1 16 "16"	isnottrue - _null_ ));
DESCR("bool is not true (ie, false or unknown)");
DATA(insert OID = 1418 (  isnotfalse		PGNSP PGUID 12 f f f f i 1 16 "16"	isnotfalse - _null_ ));
DESCR("bool is not false (ie, true or unknown)");

DATA(insert OID = 1419 (  time				PGNSP PGUID 12 f f t f i 1 1083 "1186"	interval_time - _null_ ));
DESCR("convert interval to time");

DATA(insert OID = 1421 (  box				PGNSP PGUID 12 f f t f i 2 603 "600 600"	points_box - _null_ ));
DESCR("convert points to box");
DATA(insert OID = 1422 (  box_add			PGNSP PGUID 12 f f t f i 2 603 "603 600"	box_add - _null_ ));
DESCR("add point to box (translate)");
DATA(insert OID = 1423 (  box_sub			PGNSP PGUID 12 f f t f i 2 603 "603 600"	box_sub - _null_ ));
DESCR("subtract point from box (translate)");
DATA(insert OID = 1424 (  box_mul			PGNSP PGUID 12 f f t f i 2 603 "603 600"	box_mul - _null_ ));
DESCR("multiply box by point (scale)");
DATA(insert OID = 1425 (  box_div			PGNSP PGUID 12 f f t f i 2 603 "603 600"	box_div - _null_ ));
DESCR("divide box by point (scale)");
DATA(insert OID = 1426 (  path_contain_pt	PGNSP PGUID 14 f f t f i 2	16 "602 600"  "select pg_catalog.on_ppath($2, $1)" - _null_ ));
DESCR("path contains point?");
DATA(insert OID = 1428 (  poly_contain_pt	PGNSP PGUID 12 f f t f i 2	16 "604 600"  poly_contain_pt - _null_ ));
DESCR("polygon contains point?");
DATA(insert OID = 1429 (  pt_contained_poly PGNSP PGUID 12 f f t f i 2	16 "600 604"  pt_contained_poly - _null_ ));
DESCR("point contained in polygon?");

DATA(insert OID = 1430 (  isclosed			PGNSP PGUID 12 f f t f i 1	16 "602"  path_isclosed - _null_ ));
DESCR("path closed?");
DATA(insert OID = 1431 (  isopen			PGNSP PGUID 12 f f t f i 1	16 "602"  path_isopen - _null_ ));
DESCR("path open?");
DATA(insert OID = 1432 (  path_npoints		PGNSP PGUID 12 f f t f i 1	23 "602"  path_npoints - _null_ ));
DESCR("number of points in path");

/* pclose and popen might better be named close and open, but that crashes initdb.
 * - thomas 97/04/20
 */

DATA(insert OID = 1433 (  pclose			PGNSP PGUID 12 f f t f i 1 602 "602"	path_close - _null_ ));
DESCR("close path");
DATA(insert OID = 1434 (  popen				PGNSP PGUID 12 f f t f i 1 602 "602"	path_open - _null_ ));
DESCR("open path");
DATA(insert OID = 1435 (  path_add			PGNSP PGUID 12 f f t f i 2 602 "602 602"	path_add - _null_ ));
DESCR("concatenate open paths");
DATA(insert OID = 1436 (  path_add_pt		PGNSP PGUID 12 f f t f i 2 602 "602 600"	path_add_pt - _null_ ));
DESCR("add (translate path)");
DATA(insert OID = 1437 (  path_sub_pt		PGNSP PGUID 12 f f t f i 2 602 "602 600"	path_sub_pt - _null_ ));
DESCR("subtract (translate path)");
DATA(insert OID = 1438 (  path_mul_pt		PGNSP PGUID 12 f f t f i 2 602 "602 600"	path_mul_pt - _null_ ));
DESCR("multiply (rotate/scale path)");
DATA(insert OID = 1439 (  path_div_pt		PGNSP PGUID 12 f f t f i 2 602 "602 600"	path_div_pt - _null_ ));
DESCR("divide (rotate/scale path)");

DATA(insert OID = 1440 (  point				PGNSP PGUID 12 f f t f i 2 600 "701 701"	construct_point - _null_ ));
DESCR("convert x, y to point");
DATA(insert OID = 1441 (  point_add			PGNSP PGUID 12 f f t f i 2 600 "600 600"	point_add - _null_ ));
DESCR("add points (translate)");
DATA(insert OID = 1442 (  point_sub			PGNSP PGUID 12 f f t f i 2 600 "600 600"	point_sub - _null_ ));
DESCR("subtract points (translate)");
DATA(insert OID = 1443 (  point_mul			PGNSP PGUID 12 f f t f i 2 600 "600 600"	point_mul - _null_ ));
DESCR("multiply points (scale/rotate)");
DATA(insert OID = 1444 (  point_div			PGNSP PGUID 12 f f t f i 2 600 "600 600"	point_div - _null_ ));
DESCR("divide points (scale/rotate)");

DATA(insert OID = 1445 (  poly_npoints		PGNSP PGUID 12 f f t f i 1	23 "604"  poly_npoints - _null_ ));
DESCR("number of points in polygon");
DATA(insert OID = 1446 (  box				PGNSP PGUID 12 f f t f i 1 603 "604"	poly_box - _null_ ));
DESCR("convert polygon to bounding box");
DATA(insert OID = 1447 (  path				PGNSP PGUID 12 f f t f i 1 602 "604"	poly_path - _null_ ));
DESCR("convert polygon to path");
DATA(insert OID = 1448 (  polygon			PGNSP PGUID 12 f f t f i 1 604 "603"	box_poly - _null_ ));
DESCR("convert box to polygon");
DATA(insert OID = 1449 (  polygon			PGNSP PGUID 12 f f t f i 1 604 "602"	path_poly - _null_ ));
DESCR("convert path to polygon");

DATA(insert OID = 1450 (  circle_in			PGNSP PGUID 12 f f t f i 1 718 "2275"  circle_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1451 (  circle_out		PGNSP PGUID 12 f f t f i 1 2275 "718"  circle_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1452 (  circle_same		PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_same - _null_ ));
DESCR("same as?");
DATA(insert OID = 1453 (  circle_contain	PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_contain - _null_ ));
DESCR("contains?");
DATA(insert OID = 1454 (  circle_left		PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_left - _null_ ));
DESCR("is left of");
DATA(insert OID = 1455 (  circle_overleft	PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_overleft - _null_ ));
DESCR("overlaps or is left of");
DATA(insert OID = 1456 (  circle_overright	PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_overright - _null_ ));
DESCR("overlaps or is right of");
DATA(insert OID = 1457 (  circle_right		PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_right - _null_ ));
DESCR("is right of");
DATA(insert OID = 1458 (  circle_contained	PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_contained - _null_ ));
DESCR("contained in?");
DATA(insert OID = 1459 (  circle_overlap	PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_overlap - _null_ ));
DESCR("overlaps");
DATA(insert OID = 1460 (  circle_below		PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_below - _null_ ));
DESCR("is below");
DATA(insert OID = 1461 (  circle_above		PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_above - _null_ ));
DESCR("is above");
DATA(insert OID = 1462 (  circle_eq			PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_eq - _null_ ));
DESCR("equal by area");
DATA(insert OID = 1463 (  circle_ne			PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_ne - _null_ ));
DESCR("not equal by area");
DATA(insert OID = 1464 (  circle_lt			PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_lt - _null_ ));
DESCR("less-than by area");
DATA(insert OID = 1465 (  circle_gt			PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_gt - _null_ ));
DESCR("greater-than by area");
DATA(insert OID = 1466 (  circle_le			PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_le - _null_ ));
DESCR("less-than-or-equal by area");
DATA(insert OID = 1467 (  circle_ge			PGNSP PGUID 12 f f t f i 2	16 "718 718"  circle_ge - _null_ ));
DESCR("greater-than-or-equal by area");
DATA(insert OID = 1468 (  area				PGNSP PGUID 12 f f t f i 1 701 "718"	circle_area - _null_ ));
DESCR("area of circle");
DATA(insert OID = 1469 (  diameter			PGNSP PGUID 12 f f t f i 1 701 "718"	circle_diameter - _null_ ));
DESCR("diameter of circle");
DATA(insert OID = 1470 (  radius			PGNSP PGUID 12 f f t f i 1 701 "718"	circle_radius - _null_ ));
DESCR("radius of circle");
DATA(insert OID = 1471 (  circle_distance	PGNSP PGUID 12 f f t f i 2 701 "718 718"	circle_distance - _null_ ));
DESCR("distance between");
DATA(insert OID = 1472 (  circle_center		PGNSP PGUID 12 f f t f i 1 600 "718"	circle_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1473 (  circle			PGNSP PGUID 12 f f t f i 2 718 "600 701"	cr_circle - _null_ ));
DESCR("convert point and radius to circle");
DATA(insert OID = 1474 (  circle			PGNSP PGUID 12 f f t f i 1 718 "604"	poly_circle - _null_ ));
DESCR("convert polygon to circle");
DATA(insert OID = 1475 (  polygon			PGNSP PGUID 12 f f t f i 2 604 "23 718"  circle_poly - _null_ ));
DESCR("convert vertex count and circle to polygon");
DATA(insert OID = 1476 (  dist_pc			PGNSP PGUID 12 f f t f i 2 701 "600 718"	dist_pc - _null_ ));
DESCR("distance between point and circle");
DATA(insert OID = 1477 (  circle_contain_pt PGNSP PGUID 12 f f t f i 2	16 "718 600"  circle_contain_pt - _null_ ));
DESCR("circle contains point?");
DATA(insert OID = 1478 (  pt_contained_circle	PGNSP PGUID 12 f f t f i 2	16 "600 718"  pt_contained_circle - _null_ ));
DESCR("point inside circle?");
DATA(insert OID = 1479 (  circle			PGNSP PGUID 12 f f t f i 1 718 "603"	box_circle - _null_ ));
DESCR("convert box to circle");
DATA(insert OID = 1480 (  box				PGNSP PGUID 12 f f t f i 1 603 "718"	circle_box - _null_ ));
DESCR("convert circle to box");
DATA(insert OID = 1481 (  tinterval			 PGNSP PGUID 12 f f t f i 2 704 "702 702" mktinterval - _null_ ));
DESCR("convert to tinterval");

DATA(insert OID = 1482 (  lseg_ne			PGNSP PGUID 12 f f t f i 2	16 "601 601"  lseg_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1483 (  lseg_lt			PGNSP PGUID 12 f f t f i 2	16 "601 601"  lseg_lt - _null_ ));
DESCR("less-than by length");
DATA(insert OID = 1484 (  lseg_le			PGNSP PGUID 12 f f t f i 2	16 "601 601"  lseg_le - _null_ ));
DESCR("less-than-or-equal by length");
DATA(insert OID = 1485 (  lseg_gt			PGNSP PGUID 12 f f t f i 2	16 "601 601"  lseg_gt - _null_ ));
DESCR("greater-than by length");
DATA(insert OID = 1486 (  lseg_ge			PGNSP PGUID 12 f f t f i 2	16 "601 601"  lseg_ge - _null_ ));
DESCR("greater-than-or-equal by length");
DATA(insert OID = 1487 (  lseg_length		PGNSP PGUID 12 f f t f i 1 701 "601"	lseg_length - _null_ ));
DESCR("distance between endpoints");
DATA(insert OID = 1488 (  close_ls			PGNSP PGUID 12 f f t f i 2 600 "628 601"	close_ls - _null_ ));
DESCR("closest point to line on line segment");
DATA(insert OID = 1489 (  close_lseg		PGNSP PGUID 12 f f t f i 2 600 "601 601"	close_lseg - _null_ ));
DESCR("closest point to line segment on line segment");

DATA(insert OID = 1490 (  line_in			PGNSP PGUID 12 f f t f i 1 628 "2275"  line_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1491 (  line_out			PGNSP PGUID 12 f f t f i 1 2275 "628"	line_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1492 (  line_eq			PGNSP PGUID 12 f f t f i 2	16 "628 628"	line_eq - _null_ ));
DESCR("lines equal?");
DATA(insert OID = 1493 (  line				PGNSP PGUID 12 f f t f i 2 628 "600 600"	line_construct_pp - _null_ ));
DESCR("line from points");
DATA(insert OID = 1494 (  line_interpt		PGNSP PGUID 12 f f t f i 2 600 "628 628"	line_interpt - _null_ ));
DESCR("intersection point");
DATA(insert OID = 1495 (  line_intersect	PGNSP PGUID 12 f f t f i 2	16 "628 628"  line_intersect - _null_ ));
DESCR("intersect?");
DATA(insert OID = 1496 (  line_parallel		PGNSP PGUID 12 f f t f i 2	16 "628 628"  line_parallel - _null_ ));
DESCR("parallel?");
DATA(insert OID = 1497 (  line_perp			PGNSP PGUID 12 f f t f i 2	16 "628 628"  line_perp - _null_ ));
DESCR("perpendicular?");
DATA(insert OID = 1498 (  line_vertical		PGNSP PGUID 12 f f t f i 1	16 "628"  line_vertical - _null_ ));
DESCR("vertical?");
DATA(insert OID = 1499 (  line_horizontal	PGNSP PGUID 12 f f t f i 1	16 "628"  line_horizontal - _null_ ));
DESCR("horizontal?");

/* OIDS 1500 - 1599 */

DATA(insert OID = 1530 (  length			PGNSP PGUID 12 f f t f i 1 701 "601"	lseg_length - _null_ ));
DESCR("distance between endpoints");
DATA(insert OID = 1531 (  length			PGNSP PGUID 12 f f t f i 1 701 "602"	path_length - _null_ ));
DESCR("sum of path segments");


DATA(insert OID = 1532 (  point				PGNSP PGUID 12 f f t f i 1 600 "601"	lseg_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1533 (  point				PGNSP PGUID 12 f f t f i 1 600 "602"	path_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1534 (  point				PGNSP PGUID 12 f f t f i 1 600 "603"	box_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1540 (  point				PGNSP PGUID 12 f f t f i 1 600 "604"	poly_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1541 (  lseg				PGNSP PGUID 12 f f t f i 1 601 "603"	box_diagonal - _null_ ));
DESCR("diagonal of");
DATA(insert OID = 1542 (  center			PGNSP PGUID 12 f f t f i 1 600 "603"	box_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1543 (  center			PGNSP PGUID 12 f f t f i 1 600 "718"	circle_center - _null_ ));
DESCR("center of");
DATA(insert OID = 1544 (  polygon			PGNSP PGUID 14 f f t f i 1 604 "718"	"select pg_catalog.polygon(12, $1)" - _null_ ));
DESCR("convert circle to 12-vertex polygon");
DATA(insert OID = 1545 (  npoints			PGNSP PGUID 12 f f t f i 1	23 "602"  path_npoints - _null_ ));
DESCR("number of points in path");
DATA(insert OID = 1556 (  npoints			PGNSP PGUID 12 f f t f i 1	23 "604"  poly_npoints - _null_ ));
DESCR("number of points in polygon");

DATA(insert OID = 1564 (  bit_in			PGNSP PGUID 12 f f t f i 3 1560 "2275 26 23"	bit_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1565 (  bit_out			PGNSP PGUID 12 f f t f i 1 2275 "1560"	bit_out - _null_ ));
DESCR("I/O");

DATA(insert OID = 1569 (  like				PGNSP PGUID 12 f f t f i 2 16 "25 25"  textlike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 1570 (  notlike			PGNSP PGUID 12 f f t f i 2 16 "25 25"  textnlike - _null_ ));
DESCR("does not match LIKE expression");
DATA(insert OID = 1571 (  like				PGNSP PGUID 12 f f t f i 2 16 "19 25"  namelike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 1572 (  notlike			PGNSP PGUID 12 f f t f i 2 16 "19 25"  namenlike - _null_ ));
DESCR("does not match LIKE expression");


/* SEQUENCEs nextval & currval functions */
DATA(insert OID = 1574 (  nextval			PGNSP PGUID 12 f f t f v 1 20 "25"	nextval - _null_ ));
DESCR("sequence next value");
DATA(insert OID = 1575 (  currval			PGNSP PGUID 12 f f t f v 1 20 "25"	currval - _null_ ));
DESCR("sequence current value");
DATA(insert OID = 1576 (  setval			PGNSP PGUID 12 f f t f v 2 20 "25 20"  setval - _null_ ));
DESCR("set sequence value");
DATA(insert OID = 1765 (  setval			PGNSP PGUID 12 f f t f v 3 20 "25 20 16"	setval_and_iscalled - _null_ ));
DESCR("set sequence value and iscalled status");

DATA(insert OID = 1579 (  varbit_in			PGNSP PGUID 12 f f t f i 3 1562 "2275 26 23"	varbit_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1580 (  varbit_out		PGNSP PGUID 12 f f t f i 1 2275 "1562"	varbit_out - _null_ ));
DESCR("I/O");

DATA(insert OID = 1581 (  biteq				PGNSP PGUID 12 f f t f i 2 16 "1560 1560"  biteq - _null_ ));
DESCR("equal");
DATA(insert OID = 1582 (  bitne				PGNSP PGUID 12 f f t f i 2 16 "1560 1560"  bitne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1592 (  bitge				PGNSP PGUID 12 f f t f i 2 16 "1560 1560"  bitge - _null_ ));
DESCR("greater than or equal");
DATA(insert OID = 1593 (  bitgt				PGNSP PGUID 12 f f t f i 2 16 "1560 1560"  bitgt - _null_ ));
DESCR("greater than");
DATA(insert OID = 1594 (  bitle				PGNSP PGUID 12 f f t f i 2 16 "1560 1560"  bitle - _null_ ));
DESCR("less than or equal");
DATA(insert OID = 1595 (  bitlt				PGNSP PGUID 12 f f t f i 2 16 "1560 1560"  bitlt - _null_ ));
DESCR("less than");
DATA(insert OID = 1596 (  bitcmp			PGNSP PGUID 12 f f t f i 2 23 "1560 1560"  bitcmp - _null_ ));
DESCR("compare");

DATA(insert OID = 1598 (  random			PGNSP PGUID 12 f f t f v 0 701 ""  drandom - _null_ ));
DESCR("random value");
DATA(insert OID = 1599 (  setseed			PGNSP PGUID 12 f f t f v 1	23 "701"	setseed - _null_ ));
DESCR("set random seed");

/* OIDS 1600 - 1699 */

DATA(insert OID = 1600 (  asin				PGNSP PGUID 12 f f t f i 1 701 "701"	dasin - _null_ ));
DESCR("arcsine");
DATA(insert OID = 1601 (  acos				PGNSP PGUID 12 f f t f i 1 701 "701"	dacos - _null_ ));
DESCR("arccosine");
DATA(insert OID = 1602 (  atan				PGNSP PGUID 12 f f t f i 1 701 "701"	datan - _null_ ));
DESCR("arctangent");
DATA(insert OID = 1603 (  atan2				PGNSP PGUID 12 f f t f i 2 701 "701 701"	datan2 - _null_ ));
DESCR("arctangent, two arguments");
DATA(insert OID = 1604 (  sin				PGNSP PGUID 12 f f t f i 1 701 "701"	dsin - _null_ ));
DESCR("sine");
DATA(insert OID = 1605 (  cos				PGNSP PGUID 12 f f t f i 1 701 "701"	dcos - _null_ ));
DESCR("cosine");
DATA(insert OID = 1606 (  tan				PGNSP PGUID 12 f f t f i 1 701 "701"	dtan - _null_ ));
DESCR("tangent");
DATA(insert OID = 1607 (  cot				PGNSP PGUID 12 f f t f i 1 701 "701"	dcot - _null_ ));
DESCR("cotangent");
DATA(insert OID = 1608 (  degrees			PGNSP PGUID 12 f f t f i 1 701 "701"	degrees - _null_ ));
DESCR("radians to degrees");
DATA(insert OID = 1609 (  radians			PGNSP PGUID 12 f f t f i 1 701 "701"	radians - _null_ ));
DESCR("degrees to radians");
DATA(insert OID = 1610 (  pi				PGNSP PGUID 12 f f t f i 0 701 ""  dpi - _null_ ));
DESCR("PI");

DATA(insert OID = 1618 (  interval_mul		PGNSP PGUID 12 f f t f i 2 1186 "1186 701"	interval_mul - _null_ ));
DESCR("multiply interval");

DATA(insert OID = 1620 (  ascii				PGNSP PGUID 12 f f t f i 1 23 "25"	ascii - _null_ ));
DESCR("convert first char to int4");
DATA(insert OID = 1621 (  chr				PGNSP PGUID 12 f f t f i 1 25 "23"	chr - _null_ ));
DESCR("convert int4 to char");
DATA(insert OID = 1622 (  repeat			PGNSP PGUID 12 f f t f i 2 25 "25 23"  repeat - _null_ ));
DESCR("replicate string int4 times");

DATA(insert OID = 1623 (  similar_escape	PGNSP PGUID 12 f f f f i 2 25 "25 25" similar_escape - _null_ ));
DESCR("convert SQL99 regexp pattern to POSIX style");

DATA(insert OID = 1624 (  mul_d_interval	PGNSP PGUID 12 f f t f i 2 1186 "701 1186"	mul_d_interval - _null_ ));

DATA(insert OID = 1631 (  bpcharlike	   PGNSP PGUID 12 f f t f i 2 16 "1042 25" textlike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 1632 (  bpcharnlike	   PGNSP PGUID 12 f f t f i 2 16 "1042 25" textnlike - _null_ ));
DESCR("does not match LIKE expression");

DATA(insert OID = 1633 (  texticlike		PGNSP PGUID 12 f f t f i 2 16 "25 25" texticlike - _null_ ));
DESCR("matches LIKE expression, case-insensitive");
DATA(insert OID = 1634 (  texticnlike		PGNSP PGUID 12 f f t f i 2 16 "25 25" texticnlike - _null_ ));
DESCR("does not match LIKE expression, case-insensitive");
DATA(insert OID = 1635 (  nameiclike		PGNSP PGUID 12 f f t f i 2 16 "19 25"  nameiclike - _null_ ));
DESCR("matches LIKE expression, case-insensitive");
DATA(insert OID = 1636 (  nameicnlike		PGNSP PGUID 12 f f t f i 2 16 "19 25"  nameicnlike - _null_ ));
DESCR("does not match LIKE expression, case-insensitive");
DATA(insert OID = 1637 (  like_escape		PGNSP PGUID 12 f f t f i 2 25 "25 25" like_escape - _null_ ));
DESCR("convert LIKE pattern to use backslash escapes");

DATA(insert OID = 1656 (  bpcharicregexeq	 PGNSP PGUID 12 f f t f i 2 16 "1042 25"	texticregexeq - _null_ ));
DESCR("matches regex., case-insensitive");
DATA(insert OID = 1657 (  bpcharicregexne	 PGNSP PGUID 12 f f t f i 2 16 "1042 25"	texticregexne - _null_ ));
DESCR("does not match regex., case-insensitive");
DATA(insert OID = 1658 (  bpcharregexeq    PGNSP PGUID 12 f f t f i 2 16 "1042 25"	textregexeq - _null_ ));
DESCR("matches regex., case-sensitive");
DATA(insert OID = 1659 (  bpcharregexne    PGNSP PGUID 12 f f t f i 2 16 "1042 25"	textregexne - _null_ ));
DESCR("does not match regex., case-sensitive");
DATA(insert OID = 1660 (  bpchariclike		PGNSP PGUID 12 f f t f i 2 16 "1042 25" texticlike - _null_ ));
DESCR("matches LIKE expression, case-insensitive");
DATA(insert OID = 1661 (  bpcharicnlike		PGNSP PGUID 12 f f t f i 2 16 "1042 25" texticnlike - _null_ ));
DESCR("does not match LIKE expression, case-insensitive");

DATA(insert OID = 1689 (  update_pg_pwd_and_pg_group  PGNSP PGUID 12 f f t f v 0 2279  ""	update_pg_pwd_and_pg_group - _null_ ));
DESCR("update pg_pwd and pg_group files");

/* Oracle Compatibility Related Functions - By Edmund Mergl <E.Mergl@bawue.de> */
DATA(insert OID =  868 (  strpos	   PGNSP PGUID 12 f f t f i 2 23 "25 25"	textpos - _null_ ));
DESCR("find position of substring");
DATA(insert OID =  870 (  lower		   PGNSP PGUID 12 f f t f i 1 25 "25"  lower - _null_ ));
DESCR("lowercase");
DATA(insert OID =  871 (  upper		   PGNSP PGUID 12 f f t f i 1 25 "25"  upper - _null_ ));
DESCR("uppercase");
DATA(insert OID =  872 (  initcap	   PGNSP PGUID 12 f f t f i 1 25 "25"  initcap - _null_ ));
DESCR("capitalize each word");
DATA(insert OID =  873 (  lpad		   PGNSP PGUID 12 f f t f i 3 25 "25 23 25"  lpad - _null_ ));
DESCR("left-pad string to length");
DATA(insert OID =  874 (  rpad		   PGNSP PGUID 12 f f t f i 3 25 "25 23 25"  rpad - _null_ ));
DESCR("right-pad string to length");
DATA(insert OID =  875 (  ltrim		   PGNSP PGUID 12 f f t f i 2 25 "25 25"	ltrim - _null_ ));
DESCR("trim selected characters from left end of string");
DATA(insert OID =  876 (  rtrim		   PGNSP PGUID 12 f f t f i 2 25 "25 25"	rtrim - _null_ ));
DESCR("trim selected characters from right end of string");
DATA(insert OID =  877 (  substr	   PGNSP PGUID 12 f f t f i 3 25 "25 23 23"  text_substr - _null_ ));
DESCR("return portion of string");
DATA(insert OID =  878 (  translate    PGNSP PGUID 12 f f t f i 3 25 "25 25 25"  translate - _null_ ));
DESCR("map a set of character appearing in string");
DATA(insert OID =  879 (  lpad		   PGNSP PGUID 14 f f t f i 2 25 "25 23"	"select pg_catalog.lpad($1, $2, \' \')" - _null_ ));
DESCR("left-pad string to length");
DATA(insert OID =  880 (  rpad		   PGNSP PGUID 14 f f t f i 2 25 "25 23"	"select pg_catalog.rpad($1, $2, \' \')" - _null_ ));
DESCR("right-pad string to length");
DATA(insert OID =  881 (  ltrim		   PGNSP PGUID 12 f f t f i 1 25 "25"  ltrim1 - _null_ ));
DESCR("trim spaces from left end of string");
DATA(insert OID =  882 (  rtrim		   PGNSP PGUID 12 f f t f i 1 25 "25"  rtrim1 - _null_ ));
DESCR("trim spaces from right end of string");
DATA(insert OID =  883 (  substr	   PGNSP PGUID 12 f f t f i 2 25 "25 23"	text_substr_no_len - _null_ ));
DESCR("return portion of string");
DATA(insert OID =  884 (  btrim		   PGNSP PGUID 12 f f t f i 2 25 "25 25"	btrim - _null_ ));
DESCR("trim selected characters from both ends of string");
DATA(insert OID =  885 (  btrim		   PGNSP PGUID 12 f f t f i 1 25 "25"  btrim1 - _null_ ));
DESCR("trim spaces from both ends of string");

DATA(insert OID =  936 (  substring    PGNSP PGUID 12 f f t f i 3 25 "25 23 23"  text_substr - _null_ ));
DESCR("return portion of string");
DATA(insert OID =  937 (  substring    PGNSP PGUID 12 f f t f i 2 25 "25 23"	text_substr_no_len - _null_ ));
DESCR("return portion of string");
DATA(insert OID =  2087 ( replace	   PGNSP PGUID 12 f f t f i 3 25 "25 25 25"  replace_text - _null_ ));
DESCR("replace all occurrences of old_substr with new_substr in string");
DATA(insert OID =  2088 ( split_part   PGNSP PGUID 12 f f t f i 3 25 "25 25 23"  split_text - _null_ ));
DESCR("split string by field_sep and return field_num");
DATA(insert OID =  2089 ( to_hex	   PGNSP PGUID 12 f f t f i 1 25 "23"  to_hex32 - _null_ ));
DESCR("convert int4 number to hex");
DATA(insert OID =  2090 ( to_hex	   PGNSP PGUID 12 f f t f i 1 25 "20"  to_hex64 - _null_ ));
DESCR("convert int8 number to hex");

/* for character set encoding support */

/* return database encoding name */
DATA(insert OID = 1039 (  getdatabaseencoding	   PGNSP PGUID 12 f f t f s 0 19 "" getdatabaseencoding - _null_ ));
DESCR("encoding name of current database");

/* return client encoding name i.e. session encoding */
DATA(insert OID = 810 (  pg_client_encoding    PGNSP PGUID 12 f f t f s 0 19 "" pg_client_encoding - _null_ ));
DESCR("encoding name of current database");

DATA(insert OID = 1717 (  convert		   PGNSP PGUID 12 f f t f s 2 25 "25 19"	pg_convert - _null_ ));
DESCR("convert string with specified destination encoding name");

DATA(insert OID = 1813 (  convert		   PGNSP PGUID 12 f f t f s 3 25 "25 19 19"  pg_convert2 - _null_ ));
DESCR("convert string with specified encoding names");

DATA(insert OID = 1619 (  convert_using    PGNSP PGUID 12 f f t f s 2 25 "25 25"  pg_convert_using - _null_ ));
DESCR("convert string with specified conversion name");

DATA(insert OID = 1264 (  pg_char_to_encoding	   PGNSP PGUID 12 f f t f s 1 23 "19"  PG_char_to_encoding - _null_ ));
DESCR("convert encoding name to encoding id");

DATA(insert OID = 1597 (  pg_encoding_to_char	   PGNSP PGUID 12 f f t f s 1 19 "23"  PG_encoding_to_char - _null_ ));
DESCR("convert encoding id to encoding name");

DATA(insert OID = 1638 (  oidgt				   PGNSP PGUID 12 f f t f i 2 16 "26 26"	oidgt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1639 (  oidge				   PGNSP PGUID 12 f f t f i 2 16 "26 26"	oidge - _null_ ));
DESCR("greater-than-or-equal");

/* System-view support functions */
DATA(insert OID = 1573 (  pg_get_ruledef	   PGNSP PGUID 12 f f t f s 1 25 "26"  pg_get_ruledef - _null_ ));
DESCR("source text of a rule");
DATA(insert OID = 1640 (  pg_get_viewdef	   PGNSP PGUID 12 f f t f s 1 25 "25"  pg_get_viewdef_name - _null_ ));
DESCR("select statement of a view");
DATA(insert OID = 1641 (  pg_get_viewdef	   PGNSP PGUID 12 f f t f s 1 25 "26"  pg_get_viewdef - _null_ ));
DESCR("select statement of a view");
DATA(insert OID = 1642 (  pg_get_userbyid	   PGNSP PGUID 12 f f t f s 1 19 "23"  pg_get_userbyid - _null_ ));
DESCR("user name by UID (with fallback)");
DATA(insert OID = 1643 (  pg_get_indexdef	   PGNSP PGUID 12 f f t f s 1 25 "26"  pg_get_indexdef - _null_ ));
DESCR("index description");
DATA(insert OID = 1662 (  pg_get_triggerdef    PGNSP PGUID 12 f f t f s 1 25 "26"  pg_get_triggerdef - _null_ ));
DESCR("trigger description");
DATA(insert OID = 1387 (  pg_get_constraintdef PGNSP PGUID 12 f f t f s 1 25 "26"  pg_get_constraintdef - _null_ ));
DESCR("constraint description");
DATA(insert OID = 1716 (  pg_get_expr		   PGNSP PGUID 12 f f t f s 2 25 "25 26"	pg_get_expr - _null_ ));
DESCR("deparse an encoded expression");


/* Generic referential integrity constraint triggers */
DATA(insert OID = 1644 (  RI_FKey_check_ins		PGNSP PGUID 12 f f t f v 0 2279 ""	RI_FKey_check_ins - _null_ ));
DESCR("referential integrity FOREIGN KEY ... REFERENCES");
DATA(insert OID = 1645 (  RI_FKey_check_upd		PGNSP PGUID 12 f f t f v 0 2279 ""	RI_FKey_check_upd - _null_ ));
DESCR("referential integrity FOREIGN KEY ... REFERENCES");
DATA(insert OID = 1646 (  RI_FKey_cascade_del	PGNSP PGUID 12 f f t f v 0 2279 ""	RI_FKey_cascade_del - _null_ ));
DESCR("referential integrity ON DELETE CASCADE");
DATA(insert OID = 1647 (  RI_FKey_cascade_upd	PGNSP PGUID 12 f f t f v 0 2279 ""	RI_FKey_cascade_upd - _null_ ));
DESCR("referential integrity ON UPDATE CASCADE");
DATA(insert OID = 1648 (  RI_FKey_restrict_del	PGNSP PGUID 12 f f t f v 0 2279 ""	RI_FKey_restrict_del - _null_ ));
DESCR("referential integrity ON DELETE RESTRICT");
DATA(insert OID = 1649 (  RI_FKey_restrict_upd	PGNSP PGUID 12 f f t f v 0 2279 ""	RI_FKey_restrict_upd - _null_ ));
DESCR("referential integrity ON UPDATE RESTRICT");
DATA(insert OID = 1650 (  RI_FKey_setnull_del	PGNSP PGUID 12 f f t f v 0 2279 ""	RI_FKey_setnull_del - _null_ ));
DESCR("referential integrity ON DELETE SET NULL");
DATA(insert OID = 1651 (  RI_FKey_setnull_upd	PGNSP PGUID 12 f f t f v 0 2279 ""	RI_FKey_setnull_upd - _null_ ));
DESCR("referential integrity ON UPDATE SET NULL");
DATA(insert OID = 1652 (  RI_FKey_setdefault_del PGNSP PGUID 12 f f t f v 0 2279 "" RI_FKey_setdefault_del - _null_ ));
DESCR("referential integrity ON DELETE SET DEFAULT");
DATA(insert OID = 1653 (  RI_FKey_setdefault_upd PGNSP PGUID 12 f f t f v 0 2279 "" RI_FKey_setdefault_upd - _null_ ));
DESCR("referential integrity ON UPDATE SET DEFAULT");
DATA(insert OID = 1654 (  RI_FKey_noaction_del PGNSP PGUID 12 f f t f v 0 2279 ""  RI_FKey_noaction_del - _null_ ));
DESCR("referential integrity ON DELETE NO ACTION");
DATA(insert OID = 1655 (  RI_FKey_noaction_upd PGNSP PGUID 12 f f t f v 0 2279 ""  RI_FKey_noaction_upd - _null_ ));
DESCR("referential integrity ON UPDATE NO ACTION");

DATA(insert OID = 1666 (  varbiteq			PGNSP PGUID 12 f f t f i 2 16 "1562 1562"  biteq - _null_ ));
DESCR("equal");
DATA(insert OID = 1667 (  varbitne			PGNSP PGUID 12 f f t f i 2 16 "1562 1562"  bitne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1668 (  varbitge			PGNSP PGUID 12 f f t f i 2 16 "1562 1562"  bitge - _null_ ));
DESCR("greater than or equal");
DATA(insert OID = 1669 (  varbitgt			PGNSP PGUID 12 f f t f i 2 16 "1562 1562"  bitgt - _null_ ));
DESCR("greater than");
DATA(insert OID = 1670 (  varbitle			PGNSP PGUID 12 f f t f i 2 16 "1562 1562"  bitle - _null_ ));
DESCR("less than or equal");
DATA(insert OID = 1671 (  varbitlt			PGNSP PGUID 12 f f t f i 2 16 "1562 1562"  bitlt - _null_ ));
DESCR("less than");
DATA(insert OID = 1672 (  varbitcmp			PGNSP PGUID 12 f f t f i 2 23 "1562 1562"  bitcmp - _null_ ));
DESCR("compare");

DATA(insert OID = 1673 (  bitand			PGNSP PGUID 12 f f t f i 2 1560 "1560 1560"  bitand - _null_ ));
DESCR("bitwise and");
DATA(insert OID = 1674 (  bitor				PGNSP PGUID 12 f f t f i 2 1560 "1560 1560"  bitor - _null_ ));
DESCR("bitwise or");
DATA(insert OID = 1675 (  bitxor			PGNSP PGUID 12 f f t f i 2 1560 "1560 1560"  bitxor - _null_ ));
DESCR("bitwise exclusive or");
DATA(insert OID = 1676 (  bitnot			PGNSP PGUID 12 f f t f i 1 1560 "1560"	bitnot - _null_ ));
DESCR("bitwise negation");
DATA(insert OID = 1677 (  bitshiftleft		PGNSP PGUID 12 f f t f i 2 1560 "1560 23"  bitshiftleft - _null_ ));
DESCR("bitwise left shift");
DATA(insert OID = 1678 (  bitshiftright		PGNSP PGUID 12 f f t f i 2 1560 "1560 23"  bitshiftright - _null_ ));
DESCR("bitwise right shift");
DATA(insert OID = 1679 (  bitcat			PGNSP PGUID 12 f f t f i 2 1560 "1560 1560"  bitcat - _null_ ));
DESCR("bitwise concatenation");
DATA(insert OID = 1680 (  substring			PGNSP PGUID 12 f f t f i 3 1560 "1560 23 23"	bitsubstr - _null_ ));
DESCR("return portion of bitstring");
DATA(insert OID = 1681 (  length			PGNSP PGUID 12 f f t f i 1 23 "1560"	bitlength - _null_ ));
DESCR("bitstring length");
DATA(insert OID = 1682 (  octet_length		PGNSP PGUID 12 f f t f i 1 23 "1560"	bitoctetlength - _null_ ));
DESCR("octet length");
DATA(insert OID = 1683 (  bit				PGNSP PGUID 12 f f t f i 1 1560 "23"	bitfromint4 - _null_ ));
DESCR("int4 to bitstring");
DATA(insert OID = 1684 (  int4				PGNSP PGUID 12 f f t f i 1 23 "1560"	bittoint4 - _null_ ));
DESCR("bitstring to int4");

DATA(insert OID = 1685 (  bit			   PGNSP PGUID 12 f f t f i 3 1560 "1560 23 16" bit - _null_ ));
DESCR("adjust bit() to typmod length");
DATA(insert OID = 1687 (  varbit		   PGNSP PGUID 12 f f t f i 3 1562 "1562 23 16" varbit - _null_ ));
DESCR("adjust varbit() to typmod length");

DATA(insert OID = 1698 (  position		   PGNSP PGUID 12 f f t f i 2 23 "1560 1560" bitposition - _null_ ));
DESCR("return position of sub-bitstring");
DATA(insert OID = 1699 (  substring			PGNSP PGUID 14 f f t f i 2 1560 "1560 23"  "select pg_catalog.substring($1, $2, -1)" - _null_ ));
DESCR("return portion of bitstring");


/* for mac type support */
DATA(insert OID = 436 (  macaddr_in			PGNSP PGUID 12 f f t f i 1 829 "2275"  macaddr_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 437 (  macaddr_out		PGNSP PGUID 12 f f t f i 1 2275 "829"  macaddr_out - _null_ ));
DESCR("I/O");

DATA(insert OID = 752 (  text				PGNSP PGUID 12 f f t f i 1 25 "829"  macaddr_text - _null_ ));
DESCR("MAC address to text");
DATA(insert OID = 753 (  trunc				PGNSP PGUID 12 f f t f i 1 829 "829"	macaddr_trunc - _null_ ));
DESCR("MAC manufacturer fields");
DATA(insert OID = 767 (  macaddr			PGNSP PGUID 12 f f t f i 1 829 "25"  text_macaddr - _null_ ));
DESCR("text to MAC address");

DATA(insert OID = 830 (  macaddr_eq			PGNSP PGUID 12 f f t f i 2 16 "829 829"  macaddr_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 831 (  macaddr_lt			PGNSP PGUID 12 f f t f i 2 16 "829 829"  macaddr_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 832 (  macaddr_le			PGNSP PGUID 12 f f t f i 2 16 "829 829"  macaddr_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 833 (  macaddr_gt			PGNSP PGUID 12 f f t f i 2 16 "829 829"  macaddr_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 834 (  macaddr_ge			PGNSP PGUID 12 f f t f i 2 16 "829 829"  macaddr_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 835 (  macaddr_ne			PGNSP PGUID 12 f f t f i 2 16 "829 829"  macaddr_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 836 (  macaddr_cmp		PGNSP PGUID 12 f f t f i 2 23 "829 829"  macaddr_cmp - _null_ ));
DESCR("less-equal-greater");

/* for inet type support */
DATA(insert OID = 910 (  inet_in			PGNSP PGUID 12 f f t f i 1 869 "2275"  inet_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 911 (  inet_out			PGNSP PGUID 12 f f t f i 1 2275 "869"  inet_out - _null_ ));
DESCR("I/O");

/* for cidr type support */
DATA(insert OID = 1267 (  cidr_in			PGNSP PGUID 12 f f t f i 1 650 "2275"  cidr_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1427 (  cidr_out			PGNSP PGUID 12 f f t f i 1 2275 "650"  cidr_out - _null_ ));
DESCR("I/O");

/* these are used for both inet and cidr */
DATA(insert OID = 920 (  network_eq			PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 921 (  network_lt			PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 922 (  network_le			PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 923 (  network_gt			PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 924 (  network_ge			PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 925 (  network_ne			PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 926 (  network_cmp		PGNSP PGUID 12 f f t f i 2 23 "869 869"  network_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 927 (  network_sub		PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_sub - _null_ ));
DESCR("is-subnet");
DATA(insert OID = 928 (  network_subeq		PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_subeq - _null_ ));
DESCR("is-subnet-or-equal");
DATA(insert OID = 929 (  network_sup		PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_sup - _null_ ));
DESCR("is-supernet");
DATA(insert OID = 930 (  network_supeq		PGNSP PGUID 12 f f t f i 2 16 "869 869"  network_supeq - _null_ ));
DESCR("is-supernet-or-equal");

/* inet/cidr functions */
DATA(insert OID = 605 (  abbrev				PGNSP PGUID 12 f f t f i 1 25 "869"  network_abbrev - _null_ ));
DESCR("abbreviated display of inet/cidr value");
DATA(insert OID = 711 (  family				PGNSP PGUID 12 f f t f i 1 23 "869"  network_family - _null_ ));
DESCR("return address family (4 for IPv4, 6 for IPv6)");
DATA(insert OID = 683 (  network			PGNSP PGUID 12 f f t f i 1 650 "869"	network_network - _null_ ));
DESCR("network part of address");
DATA(insert OID = 696 (  netmask			PGNSP PGUID 12 f f t f i 1 869 "869"	network_netmask - _null_ ));
DESCR("netmask of address");
DATA(insert OID = 697 (  masklen			PGNSP PGUID 12 f f t f i 1 23 "869"  network_masklen - _null_ ));
DESCR("netmask length");
DATA(insert OID = 698 (  broadcast			PGNSP PGUID 12 f f t f i 1 869 "869"	network_broadcast - _null_ ));
DESCR("broadcast address of network");
DATA(insert OID = 699 (  host				PGNSP PGUID 12 f f t f i 1 25 "869"  network_host - _null_ ));
DESCR("show address octets only");
DATA(insert OID = 730 (  text				PGNSP PGUID 12 f f t f i 1 25 "869"  network_show - _null_ ));
DESCR("show all parts of inet/cidr value");
DATA(insert OID = 1362 (  hostmask			PGNSP PGUID 12 f f t f i 1 869 "869"	network_hostmask - _null_ ));
DESCR("hostmask of address");
DATA(insert OID = 1713 (  inet				PGNSP PGUID 12 f f t f i 1 869 "25"  text_inet - _null_ ));
DESCR("text to inet");
DATA(insert OID = 1714 (  cidr				PGNSP PGUID 12 f f t f i 1 650 "25"  text_cidr - _null_ ));
DESCR("text to cidr");
DATA(insert OID = 1715 (  set_masklen		PGNSP PGUID 12 f f t f i 2 869 "869 23"  inet_set_masklen - _null_ ));
DESCR("change the netmask of an inet");

DATA(insert OID = 1686 ( numeric			PGNSP PGUID 12 f f t f i 1 1700 "25"	text_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1688 ( text				PGNSP PGUID 12 f f t f i 1 25 "1700"	numeric_text - _null_ ));
DESCR("(internal)");

DATA(insert OID = 1690 ( time_mi_time		PGNSP PGUID 12 f f t f i 2 1186 "1083 1083"  time_mi_time - _null_ ));
DESCR("minus");

DATA(insert OID =  1691 (  boolle			PGNSP PGUID 12 f f t f i 2 16 "16 16"  boolle - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID =  1692 (  boolge			PGNSP PGUID 12 f f t f i 2 16 "16 16"  boolge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1693 (  btboolcmp			PGNSP PGUID 12 f f t f i 2 23 "16 16"  btboolcmp - _null_ ));
DESCR("btree less-equal-greater");

DATA(insert OID = 1696 (  timetz_hash		PGNSP PGUID 12 f f t f i 1 23 "1266"	timetz_hash - _null_ ));
DESCR("hash");
DATA(insert OID = 1697 (  interval_hash		PGNSP PGUID 12 f f t f i 1 23 "1186"	interval_hash - _null_ ));
DESCR("hash");


/* OID's 1700 - 1799 NUMERIC data type */
DATA(insert OID = 1701 ( numeric_in				PGNSP PGUID 12 f f t f i 3 1700 "2275 26 23"  numeric_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 1702 ( numeric_out			PGNSP PGUID 12 f f t f i 1 2275 "1700"	numeric_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 1703 ( numeric				PGNSP PGUID 12 f f t f i 2 1700 "1700 23"  numeric - _null_ ));
DESCR("adjust numeric to typmod precision/scale");
DATA(insert OID = 1704 ( numeric_abs			PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1705 ( abs					PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_abs - _null_ ));
DESCR("absolute value");
DATA(insert OID = 1706 ( sign					PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_sign - _null_ ));
DESCR("sign of value");
DATA(insert OID = 1707 ( round					PGNSP PGUID 12 f f t f i 2 1700 "1700 23"  numeric_round - _null_ ));
DESCR("value rounded to 'scale'");
DATA(insert OID = 1708 ( round					PGNSP PGUID 14 f f t f i 1 1700 "1700"	"select pg_catalog.round($1,0)" - _null_ ));
DESCR("value rounded to 'scale' of zero");
DATA(insert OID = 1709 ( trunc					PGNSP PGUID 12 f f t f i 2 1700 "1700 23"  numeric_trunc - _null_ ));
DESCR("value truncated to 'scale'");
DATA(insert OID = 1710 ( trunc					PGNSP PGUID 14 f f t f i 1 1700 "1700"	"select pg_catalog.trunc($1,0)" - _null_ ));
DESCR("value truncated to 'scale' of zero");
DATA(insert OID = 1711 ( ceil					PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_ceil - _null_ ));
DESCR("smallest integer >= value");
DATA(insert OID = 1712 ( floor					PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_floor - _null_ ));
DESCR("largest integer <= value");
DATA(insert OID = 1718 ( numeric_eq				PGNSP PGUID 12 f f t f i 2 16 "1700 1700"  numeric_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1719 ( numeric_ne				PGNSP PGUID 12 f f t f i 2 16 "1700 1700"  numeric_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1720 ( numeric_gt				PGNSP PGUID 12 f f t f i 2 16 "1700 1700"  numeric_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1721 ( numeric_ge				PGNSP PGUID 12 f f t f i 2 16 "1700 1700"  numeric_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1722 ( numeric_lt				PGNSP PGUID 12 f f t f i 2 16 "1700 1700"  numeric_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1723 ( numeric_le				PGNSP PGUID 12 f f t f i 2 16 "1700 1700"  numeric_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1724 ( numeric_add			PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_add - _null_ ));
DESCR("add");
DATA(insert OID = 1725 ( numeric_sub			PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_sub - _null_ ));
DESCR("subtract");
DATA(insert OID = 1726 ( numeric_mul			PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_mul - _null_ ));
DESCR("multiply");
DATA(insert OID = 1727 ( numeric_div			PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_div - _null_ ));
DESCR("divide");
DATA(insert OID = 1728 ( mod					PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 1729 ( numeric_mod			PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_mod - _null_ ));
DESCR("modulus");
DATA(insert OID = 1730 ( sqrt					PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_sqrt - _null_ ));
DESCR("square root");
DATA(insert OID = 1731 ( numeric_sqrt			PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_sqrt - _null_ ));
DESCR("square root");
DATA(insert OID = 1732 ( exp					PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_exp - _null_ ));
DESCR("e raised to the power of n");
DATA(insert OID = 1733 ( numeric_exp			PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_exp - _null_ ));
DESCR("e raised to the power of n");
DATA(insert OID = 1734 ( ln						PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_ln - _null_ ));
DESCR("natural logarithm of n");
DATA(insert OID = 1735 ( numeric_ln				PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_ln - _null_ ));
DESCR("natural logarithm of n");
DATA(insert OID = 1736 ( log					PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_log - _null_ ));
DESCR("logarithm base m of n");
DATA(insert OID = 1737 ( numeric_log			PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_log - _null_ ));
DESCR("logarithm base m of n");
DATA(insert OID = 1738 ( pow					PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_power - _null_ ));
DESCR("m raised to the power of n");
DATA(insert OID = 1739 ( numeric_power			PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_power - _null_ ));
DESCR("m raised to the power of n");
DATA(insert OID = 1740 ( numeric				PGNSP PGUID 12 f f t f i 1 1700 "23"	int4_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1741 ( log					PGNSP PGUID 14 f f t f i 1 1700 "1700"	"select pg_catalog.log(10, $1)" - _null_ ));
DESCR("logarithm base 10 of n");
DATA(insert OID = 1742 ( numeric				PGNSP PGUID 12 f f t f i 1 1700 "700"  float4_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1743 ( numeric				PGNSP PGUID 12 f f t f i 1 1700 "701"  float8_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1744 ( int4					PGNSP PGUID 12 f f t f i 1 23 "1700"	numeric_int4 - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1745 ( float4					PGNSP PGUID 12 f f t f i 1 700 "1700"  numeric_float4 - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1746 ( float8					PGNSP PGUID 12 f f t f i 1 701 "1700"  numeric_float8 - _null_ ));
DESCR("(internal)");

DATA(insert OID = 1747 ( time_pl_interval		PGNSP PGUID 12 f f t f i 2 1083 "1083 1186"  time_pl_interval - _null_ ));
DESCR("plus");
DATA(insert OID = 1748 ( time_mi_interval		PGNSP PGUID 12 f f t f i 2 1083 "1083 1186"  time_mi_interval - _null_ ));
DESCR("minus");
DATA(insert OID = 1749 ( timetz_pl_interval		PGNSP PGUID 12 f f t f i 2 1266 "1266 1186"  timetz_pl_interval - _null_ ));
DESCR("plus");
DATA(insert OID = 1750 ( timetz_mi_interval		PGNSP PGUID 12 f f t f i 2 1266 "1266 1186"  timetz_mi_interval - _null_ ));
DESCR("minus");

DATA(insert OID = 1764 ( numeric_inc			PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_inc - _null_ ));
DESCR("increment by one");
DATA(insert OID = 1766 ( numeric_smaller		PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_smaller - _null_ ));
DESCR("smaller of two numbers");
DATA(insert OID = 1767 ( numeric_larger			PGNSP PGUID 12 f f t f i 2 1700 "1700 1700"  numeric_larger - _null_ ));
DESCR("larger of two numbers");
DATA(insert OID = 1769 ( numeric_cmp			PGNSP PGUID 12 f f t f i 2 23 "1700 1700"  numeric_cmp - _null_ ));
DESCR("compare two numbers");
DATA(insert OID = 1771 ( numeric_uminus			PGNSP PGUID 12 f f t f i 1 1700 "1700"	numeric_uminus - _null_ ));
DESCR("negate");
DATA(insert OID = 1779 ( int8					PGNSP PGUID 12 f f t f i 1 20 "1700"	numeric_int8 - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1781 ( numeric				PGNSP PGUID 12 f f t f i 1 1700 "20"	int8_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1782 ( numeric				PGNSP PGUID 12 f f t f i 1 1700 "21"	int2_numeric - _null_ ));
DESCR("(internal)");
DATA(insert OID = 1783 ( int2					PGNSP PGUID 12 f f t f i 1 21 "1700"	numeric_int2 - _null_ ));
DESCR("(internal)");

/* formatting */
DATA(insert OID = 1770 ( to_char			PGNSP PGUID 12 f f t f s 2	25 "1184 25"  timestamptz_to_char - _null_ ));
DESCR("format timestamp with time zone to text");
DATA(insert OID = 1772 ( to_char			PGNSP PGUID 12 f f t f i 2	25 "1700 25"  numeric_to_char - _null_ ));
DESCR("format numeric to text");
DATA(insert OID = 1773 ( to_char			PGNSP PGUID 12 f f t f i 2	25 "23 25"	int4_to_char - _null_ ));
DESCR("format int4 to text");
DATA(insert OID = 1774 ( to_char			PGNSP PGUID 12 f f t f i 2	25 "20 25"	int8_to_char - _null_ ));
DESCR("format int8 to text");
DATA(insert OID = 1775 ( to_char			PGNSP PGUID 12 f f t f i 2	25 "700 25"  float4_to_char - _null_ ));
DESCR("format float4 to text");
DATA(insert OID = 1776 ( to_char			PGNSP PGUID 12 f f t f i 2	25 "701 25"  float8_to_char - _null_ ));
DESCR("format float8 to text");
DATA(insert OID = 1777 ( to_number			PGNSP PGUID 12 f f t f i 2	1700 "25 25"  numeric_to_number - _null_ ));
DESCR("convert text to numeric");
DATA(insert OID = 1778 ( to_timestamp		PGNSP PGUID 12 f f t f s 2	1184 "25 25"  to_timestamp - _null_ ));
DESCR("convert text to timestamp with time zone");
DATA(insert OID = 1780 ( to_date			PGNSP PGUID 12 f f t f i 2	1082 "25 25"  to_date - _null_ ));
DESCR("convert text to date");
DATA(insert OID = 1768 ( to_char			PGNSP PGUID 12 f f t f i 2	25 "1186 25"  interval_to_char - _null_ ));
DESCR("format interval to text");

DATA(insert OID =  1282 ( quote_ident	   PGNSP PGUID 12 f f t f i 1 25 "25" quote_ident - _null_ ));
DESCR("quote an identifier for usage in a querystring");
DATA(insert OID =  1283 ( quote_literal    PGNSP PGUID 12 f f t f i 1 25 "25" quote_literal - _null_ ));
DESCR("quote a literal for usage in a querystring");

DATA(insert OID = 1798 (  oidin			   PGNSP PGUID 12 f f t f i 1 26 "2275" oidin - _null_ ));
DESCR("I/O");
DATA(insert OID = 1799 (  oidout		   PGNSP PGUID 12 f f t f i 1 2275 "26" oidout - _null_ ));
DESCR("I/O");


DATA(insert OID = 1810 (  bit_length	   PGNSP PGUID 14 f f t f i 1 23 "17" "select pg_catalog.octet_length($1) * 8" - _null_ ));
DESCR("length in bits");
DATA(insert OID = 1811 (  bit_length	   PGNSP PGUID 14 f f t f i 1 23 "25" "select pg_catalog.octet_length($1) * 8" - _null_ ));
DESCR("length in bits");
DATA(insert OID = 1812 (  bit_length	   PGNSP PGUID 14 f f t f i 1 23 "1560" "select pg_catalog.length($1)" - _null_ ));
DESCR("length in bits");

/* Selectivity estimators for LIKE and related operators */
DATA(insert OID = 1814 ( iclikesel			PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  iclikesel - _null_ ));
DESCR("restriction selectivity of ILIKE");
DATA(insert OID = 1815 ( icnlikesel			PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  icnlikesel - _null_ ));
DESCR("restriction selectivity of NOT ILIKE");
DATA(insert OID = 1816 ( iclikejoinsel		PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  iclikejoinsel - _null_ ));
DESCR("join selectivity of ILIKE");
DATA(insert OID = 1817 ( icnlikejoinsel		PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  icnlikejoinsel - _null_ ));
DESCR("join selectivity of NOT ILIKE");
DATA(insert OID = 1818 ( regexeqsel			PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  regexeqsel - _null_ ));
DESCR("restriction selectivity of regex match");
DATA(insert OID = 1819 ( likesel			PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  likesel - _null_ ));
DESCR("restriction selectivity of LIKE");
DATA(insert OID = 1820 ( icregexeqsel		PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  icregexeqsel - _null_ ));
DESCR("restriction selectivity of case-insensitive regex match");
DATA(insert OID = 1821 ( regexnesel			PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  regexnesel - _null_ ));
DESCR("restriction selectivity of regex non-match");
DATA(insert OID = 1822 ( nlikesel			PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  nlikesel - _null_ ));
DESCR("restriction selectivity of NOT LIKE");
DATA(insert OID = 1823 ( icregexnesel		PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 23"  icregexnesel - _null_ ));
DESCR("restriction selectivity of case-insensitive regex non-match");
DATA(insert OID = 1824 ( regexeqjoinsel		PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  regexeqjoinsel - _null_ ));
DESCR("join selectivity of regex match");
DATA(insert OID = 1825 ( likejoinsel		PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  likejoinsel - _null_ ));
DESCR("join selectivity of LIKE");
DATA(insert OID = 1826 ( icregexeqjoinsel	PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  icregexeqjoinsel - _null_ ));
DESCR("join selectivity of case-insensitive regex match");
DATA(insert OID = 1827 ( regexnejoinsel		PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  regexnejoinsel - _null_ ));
DESCR("join selectivity of regex non-match");
DATA(insert OID = 1828 ( nlikejoinsel		PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  nlikejoinsel - _null_ ));
DESCR("join selectivity of NOT LIKE");
DATA(insert OID = 1829 ( icregexnejoinsel	PGNSP PGUID 12 f f t f s 4 701 "2281 26 2281 21"  icregexnejoinsel - _null_ ));
DESCR("join selectivity of case-insensitive regex non-match");

/* Aggregate-related functions */
DATA(insert OID = 1830 (  float8_avg	   PGNSP PGUID 12 f f t f i 1 701 "1022"	float8_avg - _null_ ));
DESCR("AVG aggregate final function");
DATA(insert OID = 1831 (  float8_variance  PGNSP PGUID 12 f f t f i 1 701 "1022"	float8_variance - _null_ ));
DESCR("VARIANCE aggregate final function");
DATA(insert OID = 1832 (  float8_stddev    PGNSP PGUID 12 f f t f i 1 701 "1022"	float8_stddev - _null_ ));
DESCR("STDDEV aggregate final function");
DATA(insert OID = 1833 (  numeric_accum    PGNSP PGUID 12 f f t f i 2 1231 "1231 1700"	numeric_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1834 (  int2_accum	   PGNSP PGUID 12 f f t f i 2 1231 "1231 21"	int2_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1835 (  int4_accum	   PGNSP PGUID 12 f f t f i 2 1231 "1231 23"	int4_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1836 (  int8_accum	   PGNSP PGUID 12 f f t f i 2 1231 "1231 20"	int8_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1837 (  numeric_avg	   PGNSP PGUID 12 f f t f i 1 1700 "1231"  numeric_avg - _null_ ));
DESCR("AVG aggregate final function");
DATA(insert OID = 1838 (  numeric_variance PGNSP PGUID 12 f f t f i 1 1700 "1231"  numeric_variance - _null_ ));
DESCR("VARIANCE aggregate final function");
DATA(insert OID = 1839 (  numeric_stddev   PGNSP PGUID 12 f f t f i 1 1700 "1231"  numeric_stddev - _null_ ));
DESCR("STDDEV aggregate final function");
DATA(insert OID = 1840 (  int2_sum		   PGNSP PGUID 12 f f f f i 2 20 "20 21"	int2_sum - _null_ ));
DESCR("SUM(int2) transition function");
DATA(insert OID = 1841 (  int4_sum		   PGNSP PGUID 12 f f f f i 2 20 "20 23"	int4_sum - _null_ ));
DESCR("SUM(int4) transition function");
DATA(insert OID = 1842 (  int8_sum		   PGNSP PGUID 12 f f f f i 2 1700 "1700 20"	int8_sum - _null_ ));
DESCR("SUM(int8) transition function");
DATA(insert OID = 1843 (  interval_accum   PGNSP PGUID 12 f f t f i 2 1187 "1187 1186"	interval_accum - _null_ ));
DESCR("aggregate transition function");
DATA(insert OID = 1844 (  interval_avg	   PGNSP PGUID 12 f f t f i 1 1186 "1187"  interval_avg - _null_ ));
DESCR("AVG aggregate final function");
DATA(insert OID = 1962 (  int2_avg_accum   PGNSP PGUID 12 f f t f i 2 1016 "1016 21"	int2_avg_accum - _null_ ));
DESCR("AVG(int2) transition function");
DATA(insert OID = 1963 (  int4_avg_accum   PGNSP PGUID 12 f f t f i 2 1016 "1016 23"	int4_avg_accum - _null_ ));
DESCR("AVG(int4) transition function");
DATA(insert OID = 1964 (  int8_avg		   PGNSP PGUID 12 f f t f i 1 1700 "1016"  int8_avg - _null_ ));
DESCR("AVG(int) aggregate final function");

/* To ASCII conversion */
DATA(insert OID = 1845 ( to_ascii	PGNSP PGUID 12 f f t f i 1	25 "25"  to_ascii_default - _null_ ));
DESCR("encode text from DB encoding to ASCII text");
DATA(insert OID = 1846 ( to_ascii	PGNSP PGUID 12 f f t f i 2	25 "25 23"	to_ascii_enc - _null_ ));
DESCR("encode text from encoding to ASCII text");
DATA(insert OID = 1847 ( to_ascii	PGNSP PGUID 12 f f t f i 2	25 "25 19"	to_ascii_encname - _null_ ));
DESCR("encode text from encoding to ASCII text");

DATA(insert OID = 1848 ( interval_pl_time		PGNSP PGUID 12 f f t f i 2 1083 "1186 1083"  interval_pl_time - _null_ ));
DESCR("plus");

DATA(insert OID = 1850 (  int28eq		   PGNSP PGUID 12 f f t f i 2 16 "21 20"	int28eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1851 (  int28ne		   PGNSP PGUID 12 f f t f i 2 16 "21 20"	int28ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1852 (  int28lt		   PGNSP PGUID 12 f f t f i 2 16 "21 20"	int28lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1853 (  int28gt		   PGNSP PGUID 12 f f t f i 2 16 "21 20"	int28gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1854 (  int28le		   PGNSP PGUID 12 f f t f i 2 16 "21 20"	int28le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1855 (  int28ge		   PGNSP PGUID 12 f f t f i 2 16 "21 20"	int28ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 1856 (  int82eq		   PGNSP PGUID 12 f f t f i 2 16 "20 21"	int82eq - _null_ ));
DESCR("equal");
DATA(insert OID = 1857 (  int82ne		   PGNSP PGUID 12 f f t f i 2 16 "20 21"	int82ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 1858 (  int82lt		   PGNSP PGUID 12 f f t f i 2 16 "20 21"	int82lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1859 (  int82gt		   PGNSP PGUID 12 f f t f i 2 16 "20 21"	int82gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1860 (  int82le		   PGNSP PGUID 12 f f t f i 2 16 "20 21"	int82le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1861 (  int82ge		   PGNSP PGUID 12 f f t f i 2 16 "20 21"	int82ge - _null_ ));
DESCR("greater-than-or-equal");

DATA(insert OID = 1892 (  int2and		   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2and - _null_ ));
DESCR("binary and");
DATA(insert OID = 1893 (  int2or		   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2or - _null_ ));
DESCR("binary or");
DATA(insert OID = 1894 (  int2xor		   PGNSP PGUID 12 f f t f i 2 21 "21 21"	int2xor - _null_ ));
DESCR("binary xor");
DATA(insert OID = 1895 (  int2not		   PGNSP PGUID 12 f f t f i 1 21 "21"  int2not - _null_ ));
DESCR("binary not");
DATA(insert OID = 1896 (  int2shl		   PGNSP PGUID 12 f f t f i 2 21 "21 23"	int2shl - _null_ ));
DESCR("binary shift left");
DATA(insert OID = 1897 (  int2shr		   PGNSP PGUID 12 f f t f i 2 21 "21 23"	int2shr - _null_ ));
DESCR("binary shift right");

DATA(insert OID = 1898 (  int4and		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4and - _null_ ));
DESCR("binary and");
DATA(insert OID = 1899 (  int4or		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4or - _null_ ));
DESCR("binary or");
DATA(insert OID = 1900 (  int4xor		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4xor - _null_ ));
DESCR("binary xor");
DATA(insert OID = 1901 (  int4not		   PGNSP PGUID 12 f f t f i 1 23 "23"  int4not - _null_ ));
DESCR("binary not");
DATA(insert OID = 1902 (  int4shl		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4shl - _null_ ));
DESCR("binary shift left");
DATA(insert OID = 1903 (  int4shr		   PGNSP PGUID 12 f f t f i 2 23 "23 23"	int4shr - _null_ ));
DESCR("binary shift right");

DATA(insert OID = 1904 (  int8and		   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8and - _null_ ));
DESCR("binary and");
DATA(insert OID = 1905 (  int8or		   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8or - _null_ ));
DESCR("binary or");
DATA(insert OID = 1906 (  int8xor		   PGNSP PGUID 12 f f t f i 2 20 "20 20"	int8xor - _null_ ));
DESCR("binary xor");
DATA(insert OID = 1907 (  int8not		   PGNSP PGUID 12 f f t f i 1 20 "20"  int8not - _null_ ));
DESCR("binary not");
DATA(insert OID = 1908 (  int8shl		   PGNSP PGUID 12 f f t f i 2 20 "20 23"	int8shl - _null_ ));
DESCR("binary shift left");
DATA(insert OID = 1909 (  int8shr		   PGNSP PGUID 12 f f t f i 2 20 "20 23"	int8shr - _null_ ));
DESCR("binary shift right");

DATA(insert OID = 1910 (  int8up		   PGNSP PGUID 12 f f t f i 1 20	"20"	int8up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1911 (  int2up		   PGNSP PGUID 12 f f t f i 1 21	"21"	int2up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1912 (  int4up		   PGNSP PGUID 12 f f t f i 1 23	"23"	int4up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1913 (  float4up		   PGNSP PGUID 12 f f t f i 1 700 "700"		float4up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1914 (  float8up		   PGNSP PGUID 12 f f t f i 1 701 "701"		float8up - _null_ ));
DESCR("unary plus");
DATA(insert OID = 1915 (  numeric_uplus    PGNSP PGUID 12 f f t f i 1 1700 "1700"  numeric_uplus - _null_ ));
DESCR("unary plus");

DATA(insert OID = 1922 (  has_table_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 25 25"  has_table_privilege_name_name - _null_ ));
DESCR("user privilege on relation by username, rel name");
DATA(insert OID = 1923 (  has_table_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 26 25"  has_table_privilege_name_id - _null_ ));
DESCR("user privilege on relation by username, rel oid");
DATA(insert OID = 1924 (  has_table_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 25 25"  has_table_privilege_id_name - _null_ ));
DESCR("user privilege on relation by usesysid, rel name");
DATA(insert OID = 1925 (  has_table_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 26 25"  has_table_privilege_id_id - _null_ ));
DESCR("user privilege on relation by usesysid, rel oid");
DATA(insert OID = 1926 (  has_table_privilege		   PGNSP PGUID 12 f f t f s 2 16 "25 25"	has_table_privilege_name - _null_ ));
DESCR("current user privilege on relation by rel name");
DATA(insert OID = 1927 (  has_table_privilege		   PGNSP PGUID 12 f f t f s 2 16 "26 25"	has_table_privilege_id - _null_ ));
DESCR("current user privilege on relation by rel oid");


DATA(insert OID = 1928 (  pg_stat_get_numscans			PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_numscans - _null_ ));
DESCR("Statistics: Number of scans done for table/index");
DATA(insert OID = 1929 (  pg_stat_get_tuples_returned	PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_tuples_returned - _null_ ));
DESCR("Statistics: Number of tuples read by seqscan");
DATA(insert OID = 1930 (  pg_stat_get_tuples_fetched	PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_tuples_fetched - _null_ ));
DESCR("Statistics: Number of tuples fetched by idxscan");
DATA(insert OID = 1931 (  pg_stat_get_tuples_inserted	PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_tuples_inserted - _null_ ));
DESCR("Statistics: Number of tuples inserted");
DATA(insert OID = 1932 (  pg_stat_get_tuples_updated	PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_tuples_updated - _null_ ));
DESCR("Statistics: Number of tuples updated");
DATA(insert OID = 1933 (  pg_stat_get_tuples_deleted	PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_tuples_deleted - _null_ ));
DESCR("Statistics: Number of tuples deleted");
DATA(insert OID = 1934 (  pg_stat_get_blocks_fetched	PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_blocks_fetched - _null_ ));
DESCR("Statistics: Number of blocks fetched");
DATA(insert OID = 1935 (  pg_stat_get_blocks_hit		PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_blocks_hit - _null_ ));
DESCR("Statistics: Number of blocks found in cache");
DATA(insert OID = 1936 (  pg_stat_get_backend_idset		PGNSP PGUID 12 f f t t s 0 23 ""	pg_stat_get_backend_idset - _null_ ));
DESCR("Statistics: Currently active backend IDs");
DATA(insert OID = 2026 (  pg_backend_pid				PGNSP PGUID 12 f f t f s 0 23 ""	pg_backend_pid - _null_ ));
DESCR("Statistics: Current backend PID");
DATA(insert OID = 2274 (  pg_stat_reset				PGNSP PGUID 12 f f f f v 0 16  ""	pg_stat_reset - _null_ ));
DESCR("Statistics: Reset collected statistics");
DATA(insert OID = 1937 (  pg_stat_get_backend_pid		PGNSP PGUID 12 f f t f s 1 23 "23"	pg_stat_get_backend_pid - _null_ ));
DESCR("Statistics: PID of backend");
DATA(insert OID = 1938 (  pg_stat_get_backend_dbid		PGNSP PGUID 12 f f t f s 1 26 "23"	pg_stat_get_backend_dbid - _null_ ));
DESCR("Statistics: Database ID of backend");
DATA(insert OID = 1939 (  pg_stat_get_backend_userid	PGNSP PGUID 12 f f t f s 1 23 "23"	pg_stat_get_backend_userid - _null_ ));
DESCR("Statistics: User ID of backend");
DATA(insert OID = 1940 (  pg_stat_get_backend_activity	PGNSP PGUID 12 f f t f s 1 25 "23"	pg_stat_get_backend_activity - _null_ ));
DESCR("Statistics: Current query of backend");
DATA(insert OID = 2094 (  pg_stat_get_backend_activity_start PGNSP PGUID 12 f f t f s 1 1184 "23"  pg_stat_get_backend_activity_start - _null_));
DESCR("Statistics: Start time for current query of backend");
DATA(insert OID = 1941 (  pg_stat_get_db_numbackends	PGNSP PGUID 12 f f t f s 1 23 "26"	pg_stat_get_db_numbackends - _null_ ));
DESCR("Statistics: Number of backends in database");
DATA(insert OID = 1942 (  pg_stat_get_db_xact_commit	PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_db_xact_commit - _null_ ));
DESCR("Statistics: Transactions committed");
DATA(insert OID = 1943 (  pg_stat_get_db_xact_rollback	PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_db_xact_rollback - _null_ ));
DESCR("Statistics: Transactions rolled back");
DATA(insert OID = 1944 (  pg_stat_get_db_blocks_fetched PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_db_blocks_fetched - _null_ ));
DESCR("Statistics: Blocks fetched for database");
DATA(insert OID = 1945 (  pg_stat_get_db_blocks_hit		PGNSP PGUID 12 f f t f s 1 20 "26"	pg_stat_get_db_blocks_hit - _null_ ));
DESCR("Statistics: Blocks found in cache for database");

DATA(insert OID = 1946 (  encode						PGNSP PGUID 12 f f t f i 2 25 "17 25"  binary_encode - _null_ ));
DESCR("Convert bytea value into some ascii-only text string");
DATA(insert OID = 1947 (  decode						PGNSP PGUID 12 f f t f i 2 17 "25 25"  binary_decode - _null_ ));
DESCR("Convert ascii-encoded text string into bytea value");

DATA(insert OID = 1948 (  byteaeq		   PGNSP PGUID 12 f f t f i 2 16 "17 17"	byteaeq - _null_ ));
DESCR("equal");
DATA(insert OID = 1949 (  bytealt		   PGNSP PGUID 12 f f t f i 2 16 "17 17"	bytealt - _null_ ));
DESCR("less-than");
DATA(insert OID = 1950 (  byteale		   PGNSP PGUID 12 f f t f i 2 16 "17 17"	byteale - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 1951 (  byteagt		   PGNSP PGUID 12 f f t f i 2 16 "17 17"	byteagt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 1952 (  byteage		   PGNSP PGUID 12 f f t f i 2 16 "17 17"	byteage - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 1953 (  byteane		   PGNSP PGUID 12 f f t f i 2 16 "17 17"	byteane - _null_ ));
DESCR("not equal");
DATA(insert OID = 1954 (  byteacmp		   PGNSP PGUID 12 f f t f i 2 23 "17 17"	byteacmp - _null_ ));
DESCR("less-equal-greater");

DATA(insert OID = 1961 (  timestamp		   PGNSP PGUID 12 f f t f i 2 1114 "1114 23"	timestamp_scale - _null_ ));
DESCR("adjust timestamp precision");

DATA(insert OID = 1965 (  oidlarger		   PGNSP PGUID 12 f f t f i 2 26 "26 26"	oidlarger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 1966 (  oidsmaller	   PGNSP PGUID 12 f f t f i 2 26 "26 26"	oidsmaller - _null_ ));
DESCR("smaller of two");

DATA(insert OID = 1967 (  timestamptz	   PGNSP PGUID 12 f f t f i 2 1184 "1184 23"	timestamptz_scale - _null_ ));
DESCR("adjust timestamptz precision");
DATA(insert OID = 1968 (  time			   PGNSP PGUID 12 f f t f i 2 1083 "1083 23"	time_scale - _null_ ));
DESCR("adjust time precision");
DATA(insert OID = 1969 (  timetz		   PGNSP PGUID 12 f f t f i 2 1266 "1266 23"	timetz_scale - _null_ ));
DESCR("adjust time with time zone precision");

DATA(insert OID = 2005 (  bytealike		   PGNSP PGUID 12 f f t f i 2 16 "17 17" bytealike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 2006 (  byteanlike	   PGNSP PGUID 12 f f t f i 2 16 "17 17" byteanlike - _null_ ));
DESCR("does not match LIKE expression");
DATA(insert OID = 2007 (  like			   PGNSP PGUID 12 f f t f i 2 16 "17 17"	bytealike - _null_ ));
DESCR("matches LIKE expression");
DATA(insert OID = 2008 (  notlike		   PGNSP PGUID 12 f f t f i 2 16 "17 17"	byteanlike - _null_ ));
DESCR("does not match LIKE expression");
DATA(insert OID = 2009 (  like_escape	   PGNSP PGUID 12 f f t f i 2 17 "17 17" like_escape_bytea - _null_ ));
DESCR("convert LIKE pattern to use backslash escapes");
DATA(insert OID = 2010 (  length		   PGNSP PGUID 12 f f t f i 1 23 "17"  byteaoctetlen - _null_ ));
DESCR("octet length");
DATA(insert OID = 2011 (  byteacat		   PGNSP PGUID 12 f f t f i 2 17 "17 17"	byteacat - _null_ ));
DESCR("concatenate");
DATA(insert OID = 2012 (  substring		   PGNSP PGUID 12 f f t f i 3 17 "17 23 23"  bytea_substr - _null_ ));
DESCR("return portion of string");
DATA(insert OID = 2013 (  substring		   PGNSP PGUID 12 f f t f i 2 17 "17 23"	bytea_substr_no_len - _null_ ));
DESCR("return portion of string");
DATA(insert OID = 2085 (  substr		   PGNSP PGUID 12 f f t f i 3 17 "17 23 23"  bytea_substr - _null_ ));
DESCR("return portion of string");
DATA(insert OID = 2086 (  substr		   PGNSP PGUID 12 f f t f i 2 17 "17 23"	bytea_substr_no_len - _null_ ));
DESCR("return portion of string");
DATA(insert OID = 2014 (  position		   PGNSP PGUID 12 f f t f i 2 23 "17 17"	byteapos - _null_ ));
DESCR("return position of substring");
DATA(insert OID = 2015 (  btrim			   PGNSP PGUID 12 f f t f i 2 17 "17 17"	byteatrim - _null_ ));
DESCR("trim both ends of string");

DATA(insert OID = 2019 (  time				PGNSP PGUID 12 f f t f s 1 1083 "1184"	timestamptz_time - _null_ ));
DESCR("convert timestamptz to time");
DATA(insert OID = 2020 (  date_trunc		PGNSP PGUID 12 f f t f i 2 1114 "25 1114"  timestamp_trunc - _null_ ));
DESCR("truncate timestamp to specified units");
DATA(insert OID = 2021 (  date_part			PGNSP PGUID 12 f f t f i 2	701 "25 1114"  timestamp_part - _null_ ));
DESCR("extract field from timestamp");
DATA(insert OID = 2022 (  timestamp			PGNSP PGUID 12 f f t f s 1 1114 "25"	text_timestamp - _null_ ));
DESCR("convert text to timestamp");
DATA(insert OID = 2023 (  timestamp			PGNSP PGUID 12 f f t f s 1 1114 "702"  abstime_timestamp - _null_ ));
DESCR("convert abstime to timestamp");
DATA(insert OID = 2024 (  timestamp			PGNSP PGUID 12 f f t f i 1 1114 "1082"	date_timestamp - _null_ ));
DESCR("convert date to timestamp");
DATA(insert OID = 2025 (  timestamp			PGNSP PGUID 12 f f t f i 2 1114 "1082 1083"  datetime_timestamp - _null_ ));
DESCR("convert date and time to timestamp");
DATA(insert OID = 2027 (  timestamp			PGNSP PGUID 12 f f t f s 1 1114 "1184"	timestamptz_timestamp - _null_ ));
DESCR("convert timestamp with time zone to timestamp");
DATA(insert OID = 2028 (  timestamptz		PGNSP PGUID 12 f f t f s 1 1184 "1114"	timestamp_timestamptz - _null_ ));
DESCR("convert timestamp to timestamp with time zone");
DATA(insert OID = 2029 (  date				PGNSP PGUID 12 f f t f i 1 1082 "1114"	timestamp_date - _null_ ));
DESCR("convert timestamp to date");
DATA(insert OID = 2030 (  abstime			PGNSP PGUID 12 f f t f s 1	702 "1114"	timestamp_abstime - _null_ ));
DESCR("convert timestamp to abstime");
DATA(insert OID = 2031 (  timestamp_mi		PGNSP PGUID 12 f f t f i 2 1186 "1114 1114"  timestamp_mi - _null_ ));
DESCR("subtract");
DATA(insert OID = 2032 (  timestamp_pl_span PGNSP PGUID 12 f f t f i 2 1114 "1114 1186"  timestamp_pl_span - _null_ ));
DESCR("plus");
DATA(insert OID = 2033 (  timestamp_mi_span PGNSP PGUID 12 f f t f i 2 1114 "1114 1186"  timestamp_mi_span - _null_ ));
DESCR("minus");
DATA(insert OID = 2034 (  text				PGNSP PGUID 12 f f t f s 1	 25 "1114"	timestamp_text - _null_ ));
DESCR("convert timestamp to text");
DATA(insert OID = 2035 (  timestamp_smaller PGNSP PGUID 12 f f t f i 2 1114 "1114 1114"  timestamp_smaller - _null_ ));
DESCR("smaller of two");
DATA(insert OID = 2036 (  timestamp_larger	PGNSP PGUID 12 f f t f i 2 1114 "1114 1114"  timestamp_larger - _null_ ));
DESCR("larger of two");
DATA(insert OID = 2037 (  timezone			PGNSP PGUID 12 f f t f s 2 1266 "25 1266"  timetz_zone - _null_ ));
DESCR("adjust time with time zone to new zone");
DATA(insert OID = 2038 (  timezone			PGNSP PGUID 12 f f t f i 2 1266 "1186 1266"  timetz_izone - _null_ ));
DESCR("adjust time with time zone to new zone");
DATA(insert OID = 2041 ( overlaps			PGNSP PGUID 12 f f f f i 4 16 "1114 1114 1114 1114"  overlaps_timestamp - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 2042 ( overlaps			PGNSP PGUID 14 f f f f i 4 16 "1114 1186 1114 1186"  "select ($1, ($1 + $2)) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 2043 ( overlaps			PGNSP PGUID 14 f f f f i 4 16 "1114 1114 1114 1186"  "select ($1, $2) overlaps ($3, ($3 + $4))" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 2044 ( overlaps			PGNSP PGUID 14 f f f f i 4 16 "1114 1186 1114 1114"  "select ($1, ($1 + $2)) overlaps ($3, $4)" - _null_ ));
DESCR("SQL92 interval comparison");
DATA(insert OID = 2045 (  timestamp_cmp		PGNSP PGUID 12 f f t f i 2	23 "1114 1114"	timestamp_cmp - _null_ ));
DESCR("less-equal-greater");
DATA(insert OID = 2046 (  time				PGNSP PGUID 12 f f t f i 1 1083 "1266"	timetz_time - _null_ ));
DESCR("convert time with time zone to time");
DATA(insert OID = 2047 (  timetz			PGNSP PGUID 12 f f t f s 1 1266 "1083"	time_timetz - _null_ ));
DESCR("convert time to timetz");
DATA(insert OID = 2048 (  isfinite			PGNSP PGUID 12 f f t f i 1	 16 "1114"	timestamp_finite - _null_ ));
DESCR("finite timestamp?");
DATA(insert OID = 2049 ( to_char			PGNSP PGUID 12 f f t f s 2	25 "1114 25"  timestamp_to_char - _null_ ));
DESCR("format timestamp to text");
DATA(insert OID = 2050 ( interval_mi_time	PGNSP PGUID 14 f f t f i 2 1083 "1186 1083"  "select $2 - $1" - _null_ ));
DESCR("minus");
DATA(insert OID = 2051 ( interval_mi_timetz PGNSP PGUID 14 f f t f i 2 1266 "1186 1266"  "select $2 - $1" - _null_ ));
DESCR("minus");
DATA(insert OID = 2052 (  timestamp_eq		PGNSP PGUID 12 f f t f i 2 16 "1114 1114"  timestamp_eq - _null_ ));
DESCR("equal");
DATA(insert OID = 2053 (  timestamp_ne		PGNSP PGUID 12 f f t f i 2 16 "1114 1114"  timestamp_ne - _null_ ));
DESCR("not equal");
DATA(insert OID = 2054 (  timestamp_lt		PGNSP PGUID 12 f f t f i 2 16 "1114 1114"  timestamp_lt - _null_ ));
DESCR("less-than");
DATA(insert OID = 2055 (  timestamp_le		PGNSP PGUID 12 f f t f i 2 16 "1114 1114"  timestamp_le - _null_ ));
DESCR("less-than-or-equal");
DATA(insert OID = 2056 (  timestamp_ge		PGNSP PGUID 12 f f t f i 2 16 "1114 1114"  timestamp_ge - _null_ ));
DESCR("greater-than-or-equal");
DATA(insert OID = 2057 (  timestamp_gt		PGNSP PGUID 12 f f t f i 2 16 "1114 1114"  timestamp_gt - _null_ ));
DESCR("greater-than");
DATA(insert OID = 2058 (  age				PGNSP PGUID 12 f f t f i 2 1186 "1114 1114"  timestamp_age - _null_ ));
DESCR("date difference preserving months and years");
DATA(insert OID = 2059 (  age				PGNSP PGUID 14 f f t f s 1 1186 "1114"	"select pg_catalog.age(cast(current_date as timestamp without time zone), $1)" - _null_ ));
DESCR("date difference from today preserving months and years");

DATA(insert OID = 2069 (  timezone			PGNSP PGUID 12 f f t f s 2 1184 "25 1114"  timestamp_zone - _null_ ));
DESCR("adjust timestamp to new time zone");
DATA(insert OID = 2070 (  timezone			PGNSP PGUID 12 f f t f s 2 1184 "1186 1114"  timestamp_izone - _null_ ));
DESCR("adjust timestamp to new time zone");
DATA(insert OID = 2071 (  date_pl_interval	PGNSP PGUID 14 f f t f i 2 1114 "1082 1186"  "select cast($1 as timestamp without time zone) + $2;" - _null_ ));
DESCR("add");
DATA(insert OID = 2072 (  date_mi_interval	PGNSP PGUID 14 f f t f i 2 1114 "1082 1186"  "select cast($1 as timestamp without time zone) - $2;" - _null_ ));
DESCR("subtract");

DATA(insert OID = 2073 (  substring			PGNSP PGUID 12 f f t f i 2 25 "25 25"	textregexsubstr - _null_ ));
DESCR("extracts text matching regular expression");
DATA(insert OID = 2074 (  substring			PGNSP PGUID 14 f f t f i 3 25 "25 25 25"	"select pg_catalog.substring($1, pg_catalog.similar_escape($2, $3))" - _null_ ));
DESCR("extracts text matching SQL99 regular expression");

DATA(insert OID = 2075 (  bit				PGNSP PGUID 12 f f t f i 1 1560 "20"	bitfromint8 - _null_ ));
DESCR("int8 to bitstring");
DATA(insert OID = 2076 (  int8				PGNSP PGUID 12 f f t f i 1 20 "1560"	bittoint8 - _null_ ));
DESCR("bitstring to int8");

DATA(insert OID = 2077 (  current_setting	PGNSP PGUID 12 f f t f s 1 25 "25" show_config_by_name - _null_ ));
DESCR("SHOW X as a function");
DATA(insert OID = 2078 (  set_config		PGNSP PGUID 12 f f f f v 3 25 "25 25 16" set_config_by_name - _null_ ));
DESCR("SET X as a function");
DATA(insert OID = 2084 (  pg_show_all_settings	PGNSP PGUID 12 f f t t s 0 2249 "" show_all_settings - _null_ ));
DESCR("SHOW ALL as a function");
DATA(insert OID = 1371 (  pg_lock_status   PGNSP PGUID 12 f f f t v 0 2249 "" pg_lock_status - _null_ ));
DESCR("view system lock information");

DATA(insert OID = 2079 (  pg_table_is_visible		PGNSP PGUID 12 f f t f s 1 16 "26"	pg_table_is_visible - _null_ ));
DESCR("is table visible in search path?");
DATA(insert OID = 2080 (  pg_type_is_visible		PGNSP PGUID 12 f f t f s 1 16 "26"	pg_type_is_visible - _null_ ));
DESCR("is type visible in search path?");
DATA(insert OID = 2081 (  pg_function_is_visible	PGNSP PGUID 12 f f t f s 1 16 "26"	pg_function_is_visible - _null_ ));
DESCR("is function visible in search path?");
DATA(insert OID = 2082 (  pg_operator_is_visible	PGNSP PGUID 12 f f t f s 1 16 "26"	pg_operator_is_visible - _null_ ));
DESCR("is operator visible in search path?");
DATA(insert OID = 2083 (  pg_opclass_is_visible		PGNSP PGUID 12 f f t f s 1 16 "26"	pg_opclass_is_visible - _null_ ));
DESCR("is opclass visible in search path?");
DATA(insert OID = 2093 (  pg_conversion_is_visible		PGNSP PGUID 12 f f t f s 1 16 "26"	pg_conversion_is_visible - _null_ ));
DESCR("is conversion visible in search path?");


/* Aggregates (moved here from pg_aggregate for 7.3) */

DATA(insert OID = 2100 (  avg				PGNSP PGUID 12 t f f f i 1 1700 "20"  aggregate_dummy - _null_ ));
DATA(insert OID = 2101 (  avg				PGNSP PGUID 12 t f f f i 1 1700 "23"  aggregate_dummy - _null_ ));
DATA(insert OID = 2102 (  avg				PGNSP PGUID 12 t f f f i 1 1700 "21"  aggregate_dummy - _null_ ));
DATA(insert OID = 2103 (  avg				PGNSP PGUID 12 t f f f i 1 1700 "1700"	aggregate_dummy - _null_ ));
DATA(insert OID = 2104 (  avg				PGNSP PGUID 12 t f f f i 1 701 "700"  aggregate_dummy - _null_ ));
DATA(insert OID = 2105 (  avg				PGNSP PGUID 12 t f f f i 1 701 "701"  aggregate_dummy - _null_ ));
DATA(insert OID = 2106 (  avg				PGNSP PGUID 12 t f f f i 1 1186 "1186"	aggregate_dummy - _null_ ));

DATA(insert OID = 2107 (  sum				PGNSP PGUID 12 t f f f i 1 1700 "20"  aggregate_dummy - _null_ ));
DATA(insert OID = 2108 (  sum				PGNSP PGUID 12 t f f f i 1 20 "23"	aggregate_dummy - _null_ ));
DATA(insert OID = 2109 (  sum				PGNSP PGUID 12 t f f f i 1 20 "21"	aggregate_dummy - _null_ ));
DATA(insert OID = 2110 (  sum				PGNSP PGUID 12 t f f f i 1 700 "700"  aggregate_dummy - _null_ ));
DATA(insert OID = 2111 (  sum				PGNSP PGUID 12 t f f f i 1 701 "701"  aggregate_dummy - _null_ ));
DATA(insert OID = 2112 (  sum				PGNSP PGUID 12 t f f f i 1 790 "790"  aggregate_dummy - _null_ ));
DATA(insert OID = 2113 (  sum				PGNSP PGUID 12 t f f f i 1 1186 "1186"	aggregate_dummy - _null_ ));
DATA(insert OID = 2114 (  sum				PGNSP PGUID 12 t f f f i 1 1700 "1700"	aggregate_dummy - _null_ ));

DATA(insert OID = 2115 (  max				PGNSP PGUID 12 t f f f i 1 20 "20"	aggregate_dummy - _null_ ));
DATA(insert OID = 2116 (  max				PGNSP PGUID 12 t f f f i 1 23 "23"	aggregate_dummy - _null_ ));
DATA(insert OID = 2117 (  max				PGNSP PGUID 12 t f f f i 1 21 "21"	aggregate_dummy - _null_ ));
DATA(insert OID = 2118 (  max				PGNSP PGUID 12 t f f f i 1 26 "26"	aggregate_dummy - _null_ ));
DATA(insert OID = 2119 (  max				PGNSP PGUID 12 t f f f i 1 700 "700"  aggregate_dummy - _null_ ));
DATA(insert OID = 2120 (  max				PGNSP PGUID 12 t f f f i 1 701 "701"  aggregate_dummy - _null_ ));
DATA(insert OID = 2121 (  max				PGNSP PGUID 12 t f f f i 1 702 "702"  aggregate_dummy - _null_ ));
DATA(insert OID = 2122 (  max				PGNSP PGUID 12 t f f f i 1 1082 "1082"	aggregate_dummy - _null_ ));
DATA(insert OID = 2123 (  max				PGNSP PGUID 12 t f f f i 1 1083 "1083"	aggregate_dummy - _null_ ));
DATA(insert OID = 2124 (  max				PGNSP PGUID 12 t f f f i 1 1266 "1266"	aggregate_dummy - _null_ ));
DATA(insert OID = 2125 (  max				PGNSP PGUID 12 t f f f i 1 790 "790"  aggregate_dummy - _null_ ));
DATA(insert OID = 2126 (  max				PGNSP PGUID 12 t f f f i 1 1114 "1114"	aggregate_dummy - _null_ ));
DATA(insert OID = 2127 (  max				PGNSP PGUID 12 t f f f i 1 1184 "1184"	aggregate_dummy - _null_ ));
DATA(insert OID = 2128 (  max				PGNSP PGUID 12 t f f f i 1 1186 "1186"	aggregate_dummy - _null_ ));
DATA(insert OID = 2129 (  max				PGNSP PGUID 12 t f f f i 1 25 "25"	aggregate_dummy - _null_ ));
DATA(insert OID = 2130 (  max				PGNSP PGUID 12 t f f f i 1 1700 "1700"	aggregate_dummy - _null_ ));

DATA(insert OID = 2131 (  min				PGNSP PGUID 12 t f f f i 1 20 "20"	aggregate_dummy - _null_ ));
DATA(insert OID = 2132 (  min				PGNSP PGUID 12 t f f f i 1 23 "23"	aggregate_dummy - _null_ ));
DATA(insert OID = 2133 (  min				PGNSP PGUID 12 t f f f i 1 21 "21"	aggregate_dummy - _null_ ));
DATA(insert OID = 2134 (  min				PGNSP PGUID 12 t f f f i 1 26 "26"	aggregate_dummy - _null_ ));
DATA(insert OID = 2135 (  min				PGNSP PGUID 12 t f f f i 1 700 "700"  aggregate_dummy - _null_ ));
DATA(insert OID = 2136 (  min				PGNSP PGUID 12 t f f f i 1 701 "701"  aggregate_dummy - _null_ ));
DATA(insert OID = 2137 (  min				PGNSP PGUID 12 t f f f i 1 702 "702"  aggregate_dummy - _null_ ));
DATA(insert OID = 2138 (  min				PGNSP PGUID 12 t f f f i 1 1082 "1082"	aggregate_dummy - _null_ ));
DATA(insert OID = 2139 (  min				PGNSP PGUID 12 t f f f i 1 1083 "1083"	aggregate_dummy - _null_ ));
DATA(insert OID = 2140 (  min				PGNSP PGUID 12 t f f f i 1 1266 "1266"	aggregate_dummy - _null_ ));
DATA(insert OID = 2141 (  min				PGNSP PGUID 12 t f f f i 1 790 "790"  aggregate_dummy - _null_ ));
DATA(insert OID = 2142 (  min				PGNSP PGUID 12 t f f f i 1 1114 "1114"	aggregate_dummy - _null_ ));
DATA(insert OID = 2143 (  min				PGNSP PGUID 12 t f f f i 1 1184 "1184"	aggregate_dummy - _null_ ));
DATA(insert OID = 2144 (  min				PGNSP PGUID 12 t f f f i 1 1186 "1186"	aggregate_dummy - _null_ ));
DATA(insert OID = 2145 (  min				PGNSP PGUID 12 t f f f i 1 25 "25"	aggregate_dummy - _null_ ));
DATA(insert OID = 2146 (  min				PGNSP PGUID 12 t f f f i 1 1700 "1700"	aggregate_dummy - _null_ ));

DATA(insert OID = 2147 (  count				PGNSP PGUID 12 t f f f i 1 20 "2276"  aggregate_dummy - _null_ ));

DATA(insert OID = 2148 (  variance			PGNSP PGUID 12 t f f f i 1 1700 "20"  aggregate_dummy - _null_ ));
DATA(insert OID = 2149 (  variance			PGNSP PGUID 12 t f f f i 1 1700 "23"  aggregate_dummy - _null_ ));
DATA(insert OID = 2150 (  variance			PGNSP PGUID 12 t f f f i 1 1700 "21"  aggregate_dummy - _null_ ));
DATA(insert OID = 2151 (  variance			PGNSP PGUID 12 t f f f i 1 701 "700"  aggregate_dummy - _null_ ));
DATA(insert OID = 2152 (  variance			PGNSP PGUID 12 t f f f i 1 701 "701"  aggregate_dummy - _null_ ));
DATA(insert OID = 2153 (  variance			PGNSP PGUID 12 t f f f i 1 1700 "1700"	aggregate_dummy - _null_ ));

DATA(insert OID = 2154 (  stddev			PGNSP PGUID 12 t f f f i 1 1700 "20"  aggregate_dummy - _null_ ));
DATA(insert OID = 2155 (  stddev			PGNSP PGUID 12 t f f f i 1 1700 "23"  aggregate_dummy - _null_ ));
DATA(insert OID = 2156 (  stddev			PGNSP PGUID 12 t f f f i 1 1700 "21"  aggregate_dummy - _null_ ));
DATA(insert OID = 2157 (  stddev			PGNSP PGUID 12 t f f f i 1 701 "700"  aggregate_dummy - _null_ ));
DATA(insert OID = 2158 (  stddev			PGNSP PGUID 12 t f f f i 1 701 "701"  aggregate_dummy - _null_ ));
DATA(insert OID = 2159 (  stddev			PGNSP PGUID 12 t f f f i 1 1700 "1700"	aggregate_dummy - _null_ ));

DATA(insert OID = 2160 ( text_pattern_lt	 PGNSP PGUID 12 f f t f i 2 16 "25 25" text_pattern_lt - _null_ ));
DATA(insert OID = 2161 ( text_pattern_le	 PGNSP PGUID 12 f f t f i 2 16 "25 25" text_pattern_le - _null_ ));
DATA(insert OID = 2162 ( text_pattern_eq	 PGNSP PGUID 12 f f t f i 2 16 "25 25" text_pattern_eq - _null_ ));
DATA(insert OID = 2163 ( text_pattern_ge	 PGNSP PGUID 12 f f t f i 2 16 "25 25" text_pattern_ge - _null_ ));
DATA(insert OID = 2164 ( text_pattern_gt	 PGNSP PGUID 12 f f t f i 2 16 "25 25" text_pattern_gt - _null_ ));
DATA(insert OID = 2165 ( text_pattern_ne	 PGNSP PGUID 12 f f t f i 2 16 "25 25" text_pattern_ne - _null_ ));
DATA(insert OID = 2166 ( bttext_pattern_cmp  PGNSP PGUID 12 f f t f i 2 23 "25 25" bttext_pattern_cmp - _null_ ));

/* We use the same procedures here as above since the types are binary compatible. */
DATA(insert OID = 2174 ( bpchar_pattern_lt	  PGNSP PGUID 12 f f t f i 2 16 "1042 1042" text_pattern_lt - _null_ ));
DATA(insert OID = 2175 ( bpchar_pattern_le	  PGNSP PGUID 12 f f t f i 2 16 "1042 1042" text_pattern_le - _null_ ));
DATA(insert OID = 2176 ( bpchar_pattern_eq	  PGNSP PGUID 12 f f t f i 2 16 "1042 1042" text_pattern_eq - _null_ ));
DATA(insert OID = 2177 ( bpchar_pattern_ge	  PGNSP PGUID 12 f f t f i 2 16 "1042 1042" text_pattern_ge - _null_ ));
DATA(insert OID = 2178 ( bpchar_pattern_gt	  PGNSP PGUID 12 f f t f i 2 16 "1042 1042" text_pattern_gt - _null_ ));
DATA(insert OID = 2179 ( bpchar_pattern_ne	  PGNSP PGUID 12 f f t f i 2 16 "1042 1042" text_pattern_ne - _null_ ));
DATA(insert OID = 2180 ( btbpchar_pattern_cmp PGNSP PGUID 12 f f t f i 2 23 "1042 1042" bttext_pattern_cmp - _null_ ));

DATA(insert OID = 2181 ( name_pattern_lt	PGNSP PGUID 12 f f t f i 2 16 "19 19" name_pattern_lt - _null_ ));
DATA(insert OID = 2182 ( name_pattern_le	PGNSP PGUID 12 f f t f i 2 16 "19 19" name_pattern_le - _null_ ));
DATA(insert OID = 2183 ( name_pattern_eq	PGNSP PGUID 12 f f t f i 2 16 "19 19" name_pattern_eq - _null_ ));
DATA(insert OID = 2184 ( name_pattern_ge	PGNSP PGUID 12 f f t f i 2 16 "19 19" name_pattern_ge - _null_ ));
DATA(insert OID = 2185 ( name_pattern_gt	PGNSP PGUID 12 f f t f i 2 16 "19 19" name_pattern_gt - _null_ ));
DATA(insert OID = 2186 ( name_pattern_ne	PGNSP PGUID 12 f f t f i 2 16 "19 19" name_pattern_ne - _null_ ));
DATA(insert OID = 2187 ( btname_pattern_cmp PGNSP PGUID 12 f f t f i 2 23 "19 19" btname_pattern_cmp - _null_ ));


DATA(insert OID = 2212 (  regprocedurein	PGNSP PGUID 12 f f t f s 1 2202 "2275"	regprocedurein - _null_ ));
DESCR("I/O");
DATA(insert OID = 2213 (  regprocedureout	PGNSP PGUID 12 f f t f s 1 2275 "2202"	regprocedureout - _null_ ));
DESCR("I/O");
DATA(insert OID = 2214 (  regoperin			PGNSP PGUID 12 f f t f s 1 2203 "2275"	regoperin - _null_ ));
DESCR("I/O");
DATA(insert OID = 2215 (  regoperout		PGNSP PGUID 12 f f t f s 1 2275 "2203"	regoperout - _null_ ));
DESCR("I/O");
DATA(insert OID = 2216 (  regoperatorin		PGNSP PGUID 12 f f t f s 1 2204 "2275"	regoperatorin - _null_ ));
DESCR("I/O");
DATA(insert OID = 2217 (  regoperatorout	PGNSP PGUID 12 f f t f s 1 2275 "2204"	regoperatorout - _null_ ));
DESCR("I/O");
DATA(insert OID = 2218 (  regclassin		PGNSP PGUID 12 f f t f s 1 2205 "2275"	regclassin - _null_ ));
DESCR("I/O");
DATA(insert OID = 2219 (  regclassout		PGNSP PGUID 12 f f t f s 1 2275 "2205"	regclassout - _null_ ));
DESCR("I/O");
DATA(insert OID = 2220 (  regtypein			PGNSP PGUID 12 f f t f s 1 2206 "2275"	regtypein - _null_ ));
DESCR("I/O");
DATA(insert OID = 2221 (  regtypeout		PGNSP PGUID 12 f f t f s 1 2275 "2206"	regtypeout - _null_ ));
DESCR("I/O");

DATA(insert OID = 2246 ( fmgr_internal_validator PGNSP PGUID 12 f f t f s 1 2278 "26" fmgr_internal_validator - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2247 ( fmgr_c_validator	PGNSP PGUID 12 f f t f s 1	 2278 "26"	fmgr_c_validator - _null_ ));
DESCR("(internal)");
DATA(insert OID = 2248 ( fmgr_sql_validator PGNSP PGUID 12 f f t f s 1	 2278 "26"	fmgr_sql_validator - _null_ ));
DESCR("(internal)");

DATA(insert OID = 2250 (  has_database_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 25 25"  has_database_privilege_name_name - _null_ ));
DESCR("user privilege on database by username, database name");
DATA(insert OID = 2251 (  has_database_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 26 25"  has_database_privilege_name_id - _null_ ));
DESCR("user privilege on database by username, database oid");
DATA(insert OID = 2252 (  has_database_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 25 25"  has_database_privilege_id_name - _null_ ));
DESCR("user privilege on database by usesysid, database name");
DATA(insert OID = 2253 (  has_database_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 26 25"  has_database_privilege_id_id - _null_ ));
DESCR("user privilege on database by usesysid, database oid");
DATA(insert OID = 2254 (  has_database_privilege		   PGNSP PGUID 12 f f t f s 2 16 "25 25"	has_database_privilege_name - _null_ ));
DESCR("current user privilege on database by database name");
DATA(insert OID = 2255 (  has_database_privilege		   PGNSP PGUID 12 f f t f s 2 16 "26 25"	has_database_privilege_id - _null_ ));
DESCR("current user privilege on database by database oid");

DATA(insert OID = 2256 (  has_function_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 25 25"  has_function_privilege_name_name - _null_ ));
DESCR("user privilege on function by username, function name");
DATA(insert OID = 2257 (  has_function_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 26 25"  has_function_privilege_name_id - _null_ ));
DESCR("user privilege on function by username, function oid");
DATA(insert OID = 2258 (  has_function_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 25 25"  has_function_privilege_id_name - _null_ ));
DESCR("user privilege on function by usesysid, function name");
DATA(insert OID = 2259 (  has_function_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 26 25"  has_function_privilege_id_id - _null_ ));
DESCR("user privilege on function by usesysid, function oid");
DATA(insert OID = 2260 (  has_function_privilege		   PGNSP PGUID 12 f f t f s 2 16 "25 25"	has_function_privilege_name - _null_ ));
DESCR("current user privilege on function by function name");
DATA(insert OID = 2261 (  has_function_privilege		   PGNSP PGUID 12 f f t f s 2 16 "26 25"	has_function_privilege_id - _null_ ));
DESCR("current user privilege on function by function oid");

DATA(insert OID = 2262 (  has_language_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 25 25"  has_language_privilege_name_name - _null_ ));
DESCR("user privilege on language by username, language name");
DATA(insert OID = 2263 (  has_language_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 26 25"  has_language_privilege_name_id - _null_ ));
DESCR("user privilege on language by username, language oid");
DATA(insert OID = 2264 (  has_language_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 25 25"  has_language_privilege_id_name - _null_ ));
DESCR("user privilege on language by usesysid, language name");
DATA(insert OID = 2265 (  has_language_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 26 25"  has_language_privilege_id_id - _null_ ));
DESCR("user privilege on language by usesysid, language oid");
DATA(insert OID = 2266 (  has_language_privilege		   PGNSP PGUID 12 f f t f s 2 16 "25 25"	has_language_privilege_name - _null_ ));
DESCR("current user privilege on language by language name");
DATA(insert OID = 2267 (  has_language_privilege		   PGNSP PGUID 12 f f t f s 2 16 "26 25"	has_language_privilege_id - _null_ ));
DESCR("current user privilege on language by language oid");

DATA(insert OID = 2268 (  has_schema_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 25 25"  has_schema_privilege_name_name - _null_ ));
DESCR("user privilege on schema by username, schema name");
DATA(insert OID = 2269 (  has_schema_privilege		   PGNSP PGUID 12 f f t f s 3 16 "19 26 25"  has_schema_privilege_name_id - _null_ ));
DESCR("user privilege on schema by username, schema oid");
DATA(insert OID = 2270 (  has_schema_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 25 25"  has_schema_privilege_id_name - _null_ ));
DESCR("user privilege on schema by usesysid, schema name");
DATA(insert OID = 2271 (  has_schema_privilege		   PGNSP PGUID 12 f f t f s 3 16 "23 26 25"  has_schema_privilege_id_id - _null_ ));
DESCR("user privilege on schema by usesysid, schema oid");
DATA(insert OID = 2272 (  has_schema_privilege		   PGNSP PGUID 12 f f t f s 2 16 "25 25"	has_schema_privilege_name - _null_ ));
DESCR("current user privilege on schema by schema name");
DATA(insert OID = 2273 (  has_schema_privilege		   PGNSP PGUID 12 f f t f s 2 16 "26 25"	has_schema_privilege_id - _null_ ));
DESCR("current user privilege on schema by schema oid");



DATA(insert OID = 2290 (  record_in			PGNSP PGUID 12 f f t f i 1 2249 "2275"	record_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2291 (  record_out		PGNSP PGUID 12 f f t f i 1 2275 "2249"	record_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2292 (  cstring_in		PGNSP PGUID 12 f f t f i 1 2275 "2275"	cstring_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2293 (  cstring_out		PGNSP PGUID 12 f f t f i 1 2275 "2275"	cstring_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2294 (  any_in			PGNSP PGUID 12 f f t f i 1 2276 "2275"	any_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2295 (  any_out			PGNSP PGUID 12 f f t f i 1 2275 "2276"	any_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2296 (  anyarray_in		PGNSP PGUID 12 f f t f i 1 2277 "2275"	anyarray_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2297 (  anyarray_out		PGNSP PGUID 12 f f t f s 1 2275 "2277"	anyarray_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2298 (  void_in			PGNSP PGUID 12 f f t f i 1 2278 "2275"	void_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2299 (  void_out			PGNSP PGUID 12 f f t f i 1 2275 "2278"	void_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2300 (  trigger_in		PGNSP PGUID 12 f f t f i 1 2279 "2275"	trigger_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2301 (  trigger_out		PGNSP PGUID 12 f f t f i 1 2275 "2279"	trigger_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2302 (  language_handler_in	PGNSP PGUID 12 f f t f i 1 2280 "2275"	language_handler_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2303 (  language_handler_out	PGNSP PGUID 12 f f t f i 1 2275 "2280"	language_handler_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2304 (  internal_in		PGNSP PGUID 12 f f t f i 1 2281 "2275"	internal_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2305 (  internal_out		PGNSP PGUID 12 f f t f i 1 2275 "2281"	internal_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2306 (  opaque_in			PGNSP PGUID 12 f f t f i 1 2282 "2275"	opaque_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2307 (  opaque_out		PGNSP PGUID 12 f f t f i 1 2275 "2282"	opaque_out - _null_ ));
DESCR("I/O");
DATA(insert OID = 2312 (  anyelement_in		PGNSP PGUID 12 f f t f i 1 2283 "2275"	anyelement_in - _null_ ));
DESCR("I/O");
DATA(insert OID = 2313 (  anyelement_out	PGNSP PGUID 12 f f t f i 1 2275 "2283"	anyelement_out - _null_ ));
DESCR("I/O");

/* cryptographic */
DATA(insert OID =  2311 (  md5	   PGNSP PGUID 12 f f t f i 1 25 "25"  md5_text - _null_ ));
DESCR("calculates md5 hash");


DATA(insert OID = 2400 (  array_recv		   PGNSP PGUID 12 f f t f s 2 2277 "2281 26"  array_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2401 (  array_send		   PGNSP PGUID 12 f f t f s 2 17 "2277 26"	array_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2402 (  record_recv		   PGNSP PGUID 12 f f t f i 1 2249 "2281"  record_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2403 (  record_send		   PGNSP PGUID 12 f f t f i 1 17 "2249"  record_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2404 (  int2recv			   PGNSP PGUID 12 f f t f i 1 21 "2281"  int2recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2405 (  int2send			   PGNSP PGUID 12 f f t f i 1 17 "21"  int2send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2406 (  int4recv			   PGNSP PGUID 12 f f t f i 1 23 "2281"  int4recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2407 (  int4send			   PGNSP PGUID 12 f f t f i 1 17 "23"  int4send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2408 (  int8recv			   PGNSP PGUID 12 f f t f i 1 20 "2281"  int8recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2409 (  int8send			   PGNSP PGUID 12 f f t f i 1 17 "20"  int8send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2410 (  int2vectorrecv	   PGNSP PGUID 12 f f t f i 1 22 "2281"  int2vectorrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2411 (  int2vectorsend	   PGNSP PGUID 12 f f t f i 1 17 "22"  int2vectorsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2412 (  bytearecv			   PGNSP PGUID 12 f f t f i 1 17 "2281"  bytearecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2413 (  byteasend			   PGNSP PGUID 12 f f t f i 1 17 "17"  byteasend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2414 (  textrecv			   PGNSP PGUID 12 f f t f s 1 25 "2281"  textrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2415 (  textsend			   PGNSP PGUID 12 f f t f s 1 17 "25"  textsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2416 (  unknownrecv		   PGNSP PGUID 12 f f t f i 1 705 "2281"  unknownrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2417 (  unknownsend		   PGNSP PGUID 12 f f t f i 1 17 "705"	unknownsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2418 (  oidrecv			   PGNSP PGUID 12 f f t f i 1 26 "2281"  oidrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2419 (  oidsend			   PGNSP PGUID 12 f f t f i 1 17 "26"  oidsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2420 (  oidvectorrecv		   PGNSP PGUID 12 f f t f i 1 30 "2281"  oidvectorrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2421 (  oidvectorsend		   PGNSP PGUID 12 f f t f i 1 17 "30"  oidvectorsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2422 (  namerecv			   PGNSP PGUID 12 f f t f s 1 19 "2281"  namerecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2423 (  namesend			   PGNSP PGUID 12 f f t f s 1 17 "19"  namesend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2424 (  float4recv		   PGNSP PGUID 12 f f t f i 1 700 "2281"  float4recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2425 (  float4send		   PGNSP PGUID 12 f f t f i 1 17 "700"	float4send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2426 (  float8recv		   PGNSP PGUID 12 f f t f i 1 701 "2281"  float8recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2427 (  float8send		   PGNSP PGUID 12 f f t f i 1 17 "701"	float8send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2428 (  point_recv		   PGNSP PGUID 12 f f t f i 1 600 "2281"  point_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2429 (  point_send		   PGNSP PGUID 12 f f t f i 1 17 "600"	point_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2430 (  bpcharrecv		   PGNSP PGUID 12 f f t f s 1 1042 "2281"  bpcharrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2431 (  bpcharsend		   PGNSP PGUID 12 f f t f s 1 17 "1042"  bpcharsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2432 (  varcharrecv		   PGNSP PGUID 12 f f t f s 1 1043 "2281"  varcharrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2433 (  varcharsend		   PGNSP PGUID 12 f f t f s 1 17 "1043"  varcharsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2434 (  charrecv			   PGNSP PGUID 12 f f t f i 1 18 "2281"  charrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2435 (  charsend			   PGNSP PGUID 12 f f t f i 1 17 "18"  charsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2436 (  boolrecv			   PGNSP PGUID 12 f f t f i 1 16 "2281"  boolrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2437 (  boolsend			   PGNSP PGUID 12 f f t f i 1 17 "16"  boolsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2438 (  tidrecv			   PGNSP PGUID 12 f f t f i 1 27 "2281"  tidrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2439 (  tidsend			   PGNSP PGUID 12 f f t f i 1 17 "27"  tidsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2440 (  xidrecv			   PGNSP PGUID 12 f f t f i 1 28 "2281"  xidrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2441 (  xidsend			   PGNSP PGUID 12 f f t f i 1 17 "28"  xidsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2442 (  cidrecv			   PGNSP PGUID 12 f f t f i 1 29 "2281"  cidrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2443 (  cidsend			   PGNSP PGUID 12 f f t f i 1 17 "29"  cidsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2444 (  regprocrecv		   PGNSP PGUID 12 f f t f i 1 24 "2281"  regprocrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2445 (  regprocsend		   PGNSP PGUID 12 f f t f i 1 17 "24"  regprocsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2446 (  regprocedurerecv	   PGNSP PGUID 12 f f t f i 1 2202 "2281"  regprocedurerecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2447 (  regproceduresend	   PGNSP PGUID 12 f f t f i 1 17 "2202"  regproceduresend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2448 (  regoperrecv		   PGNSP PGUID 12 f f t f i 1 2203 "2281"  regoperrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2449 (  regopersend		   PGNSP PGUID 12 f f t f i 1 17 "2203"  regopersend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2450 (  regoperatorrecv	   PGNSP PGUID 12 f f t f i 1 2204 "2281"  regoperatorrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2451 (  regoperatorsend	   PGNSP PGUID 12 f f t f i 1 17 "2204"  regoperatorsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2452 (  regclassrecv		   PGNSP PGUID 12 f f t f i 1 2205 "2281"  regclassrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2453 (  regclasssend		   PGNSP PGUID 12 f f t f i 1 17 "2205"  regclasssend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2454 (  regtyperecv		   PGNSP PGUID 12 f f t f i 1 2206 "2281"  regtyperecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2455 (  regtypesend		   PGNSP PGUID 12 f f t f i 1 17 "2206"  regtypesend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2456 (  bit_recv			   PGNSP PGUID 12 f f t f i 1 1560 "2281"  bit_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2457 (  bit_send			   PGNSP PGUID 12 f f t f i 1 17 "1560"  bit_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2458 (  varbit_recv		   PGNSP PGUID 12 f f t f i 1 1562 "2281"  varbit_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2459 (  varbit_send		   PGNSP PGUID 12 f f t f i 1 17 "1562"  varbit_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2460 (  numeric_recv		   PGNSP PGUID 12 f f t f i 1 1700 "2281"  numeric_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2461 (  numeric_send		   PGNSP PGUID 12 f f t f i 1 17 "1700"  numeric_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2462 (  abstimerecv		   PGNSP PGUID 12 f f t f i 1 702 "2281"  abstimerecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2463 (  abstimesend		   PGNSP PGUID 12 f f t f i 1 17 "702"	abstimesend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2464 (  reltimerecv		   PGNSP PGUID 12 f f t f i 1 703 "2281"  reltimerecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2465 (  reltimesend		   PGNSP PGUID 12 f f t f i 1 17 "703"	reltimesend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2466 (  tintervalrecv		   PGNSP PGUID 12 f f t f i 1 704 "2281"  tintervalrecv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2467 (  tintervalsend		   PGNSP PGUID 12 f f t f i 1 17 "704"	tintervalsend - _null_ ));
DESCR("I/O");
DATA(insert OID = 2468 (  date_recv			   PGNSP PGUID 12 f f t f i 1 1082 "2281"  date_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2469 (  date_send			   PGNSP PGUID 12 f f t f i 1 17 "1082"  date_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2470 (  time_recv			   PGNSP PGUID 12 f f t f i 1 1083 "2281"  time_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2471 (  time_send			   PGNSP PGUID 12 f f t f i 1 17 "1083"  time_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2472 (  timetz_recv		   PGNSP PGUID 12 f f t f i 1 1266 "2281"  timetz_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2473 (  timetz_send		   PGNSP PGUID 12 f f t f i 1 17 "1266"  timetz_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2474 (  timestamp_recv	   PGNSP PGUID 12 f f t f i 1 1114 "2281"  timestamp_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2475 (  timestamp_send	   PGNSP PGUID 12 f f t f i 1 17 "1114"  timestamp_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2476 (  timestamptz_recv	   PGNSP PGUID 12 f f t f i 1 1184 "2281"  timestamptz_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2477 (  timestamptz_send	   PGNSP PGUID 12 f f t f i 1 17 "1184"  timestamptz_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2478 (  interval_recv		   PGNSP PGUID 12 f f t f i 1 1186 "2281"  interval_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2479 (  interval_send		   PGNSP PGUID 12 f f t f i 1 17 "1186"  interval_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2480 (  lseg_recv			   PGNSP PGUID 12 f f t f i 1 601 "2281"  lseg_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2481 (  lseg_send			   PGNSP PGUID 12 f f t f i 1 17 "601"	lseg_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2482 (  path_recv			   PGNSP PGUID 12 f f t f i 1 602 "2281"  path_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2483 (  path_send			   PGNSP PGUID 12 f f t f i 1 17 "602"	path_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2484 (  box_recv			   PGNSP PGUID 12 f f t f i 1 603 "2281"  box_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2485 (  box_send			   PGNSP PGUID 12 f f t f i 1 17 "603"	box_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2486 (  poly_recv			   PGNSP PGUID 12 f f t f i 1 604 "2281"  poly_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2487 (  poly_send			   PGNSP PGUID 12 f f t f i 1 17 "604"	poly_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2488 (  line_recv			   PGNSP PGUID 12 f f t f i 1 628 "2281"  line_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2489 (  line_send			   PGNSP PGUID 12 f f t f i 1 17 "628"	line_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2490 (  circle_recv		   PGNSP PGUID 12 f f t f i 1 718 "2281"  circle_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2491 (  circle_send		   PGNSP PGUID 12 f f t f i 1 17 "718"	circle_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2492 (  cash_recv			   PGNSP PGUID 12 f f t f i 1 790 "2281"  cash_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2493 (  cash_send			   PGNSP PGUID 12 f f t f i 1 17 "790"	cash_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2494 (  macaddr_recv		   PGNSP PGUID 12 f f t f i 1 829 "2281"  macaddr_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2495 (  macaddr_send		   PGNSP PGUID 12 f f t f i 1 17 "829"	macaddr_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2496 (  inet_recv			   PGNSP PGUID 12 f f t f i 1 869 "2281"  inet_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2497 (  inet_send			   PGNSP PGUID 12 f f t f i 1 17 "869"	inet_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2498 (  cidr_recv			   PGNSP PGUID 12 f f t f i 1 650 "2281"  cidr_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2499 (  cidr_send			   PGNSP PGUID 12 f f t f i 1 17 "650"	cidr_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2500 (  cstring_recv		   PGNSP PGUID 12 f f t f s 1 2275 "2281"  cstring_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2501 (  cstring_send		   PGNSP PGUID 12 f f t f s 1 17 "2275"  cstring_send - _null_ ));
DESCR("I/O");
DATA(insert OID = 2502 (  anyarray_recv		   PGNSP PGUID 12 f f t f s 1 2277 "2281"  anyarray_recv - _null_ ));
DESCR("I/O");
DATA(insert OID = 2503 (  anyarray_send		   PGNSP PGUID 12 f f t f s 1 17 "2277"  anyarray_send - _null_ ));
DESCR("I/O");

/* System-view support functions with pretty-print option */
DATA(insert OID = 2504 (  pg_get_ruledef	   PGNSP PGUID 12 f f t f s 2 25 "26 16"  pg_get_ruledef_ext - _null_ ));
DESCR("source text of a rule with pretty-print option");
DATA(insert OID = 2505 (  pg_get_viewdef	   PGNSP PGUID 12 f f t f s 2 25 "25 16"  pg_get_viewdef_name_ext - _null_ ));
DESCR("select statement of a view with pretty-print option");
DATA(insert OID = 2506 (  pg_get_viewdef	   PGNSP PGUID 12 f f t f s 2 25 "26 16"  pg_get_viewdef_ext - _null_ ));
DESCR("select statement of a view with pretty-print option");
DATA(insert OID = 2507 (  pg_get_indexdef	   PGNSP PGUID 12 f f t f s 3 25 "26 23 16"  pg_get_indexdef_ext - _null_ ));
DESCR("index description (full create statement or single expression) with pretty-print option");
DATA(insert OID = 2508 (  pg_get_constraintdef PGNSP PGUID 12 f f t f s 2 25 "26 16"  pg_get_constraintdef_ext - _null_ ));
DESCR("constraint description with pretty-print option");
DATA(insert OID = 2509 (  pg_get_expr		   PGNSP PGUID 12 f f t f s 3 25 "25 26 16" pg_get_expr_ext - _null_ ));
DESCR("deparse an encoded expression with pretty-print option");


/*
 * Symbolic values for provolatile column: these indicate whether the result
 * of a function is dependent *only* on the values of its explicit arguments,
 * or can change due to outside factors (such as parameter variables or
 * table contents).  NOTE: functions having side-effects, such as setval(),
 * must be labeled volatile to ensure they will not get optimized away,
 * even if the actual return value is not changeable.
 */
#define PROVOLATILE_IMMUTABLE	'i'		/* never changes for given input */
#define PROVOLATILE_STABLE		's'		/* does not change within a scan */
#define PROVOLATILE_VOLATILE	'v'		/* can change even within a scan */


/*
 * prototypes for functions in pg_proc.c
 */
extern Oid ProcedureCreate(const char *procedureName,
				Oid procNamespace,
				bool replace,
				bool returnsSet,
				Oid returnType,
				Oid languageObjectId,
				Oid languageValidator,
				const char *prosrc,
				const char *probin,
				bool isAgg,
				bool security_definer,
				bool isStrict,
				char volatility,
				int parameterCount,
				const Oid *parameterTypes);

extern void check_sql_fn_retval(Oid rettype, char fn_typtype,
					List *queryTreeList);

#endif   /* PG_PROC_H */
