/*-------------------------------------------------------------------------
 *
 * pg_proc.h--
 *	  definition of the system "procedure" relation (pg_proc)
 *	  along with the relation's initial contents.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_proc.h,v 1.49 1998/03/30 17:25:24 momjian Exp $
 *
 * NOTES
 *	  The script catalog/genbki.sh reads this file and generates .bki
 *	  information from the DATA() statements.  utils/Gen_fmgrtab.sh
 *	  generates fmgr.h and fmgrtab.c the same way.
 *
 *	  XXX do NOT break up DATA() statements into multiple lines!
 *		  the scripts are not as smart as you might think...
 *	  XXX (eg. #if 0 #endif won't do what you think)
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROC_H
#define PG_PROC_H

#include <tcop/dest.h>

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
	NameData	proname;
	Oid			proowner;
	Oid			prolang;
	bool		proisinh;
	bool		proistrusted;
	bool		proiscachable;
	int2		pronargs;
	bool		proretset;
	Oid			prorettype;
	oid8		proargtypes;
	int4		probyte_pct;
	int4		properbyte_cpu;
	int4		propercall_cpu;
	int4		prooutin_ratio;
	text		prosrc;			/* VARIABLE LENGTH FIELD */
	bytea		probin;			/* VARIABLE LENGTH FIELD */
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
#define Natts_pg_proc					16
#define Anum_pg_proc_proname			1
#define Anum_pg_proc_proowner			2
#define Anum_pg_proc_prolang			3
#define Anum_pg_proc_proisinh			4
#define Anum_pg_proc_proistrusted		5
#define Anum_pg_proc_proiscachable		6
#define Anum_pg_proc_pronargs			7
#define Anum_pg_proc_proretset			8
#define Anum_pg_proc_prorettype			9
#define Anum_pg_proc_proargtypes		10
#define Anum_pg_proc_probyte_pct		11
#define Anum_pg_proc_properbyte_cpu		12
#define Anum_pg_proc_propercall_cpu		13
#define Anum_pg_proc_prooutin_ratio		14
#define Anum_pg_proc_prosrc				15
#define Anum_pg_proc_probin				16

/* ----------------
 *		initial contents of pg_proc
 * ----------------
 */

/* keep the following ordered by OID so that later changes can be made easier*/

/* OIDS 1 - 99 */

DATA(insert OID = 1242 (  boolin		   PGUID 11 f t f 1 f 16 "0" 100 0 0  100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1243 (  boolout		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1244 (  byteain		   PGUID 11 f t f 1 f 17 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  31 (  byteaout		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1245 (  charin		   PGUID 11 f t f 1 f 18 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  33 (  charout		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  34 (  namein			   PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  35 (  nameout		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  38 (  int2in			   PGUID 11 f t f 1 f 21 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  39 (  int2out		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  40 (  int28in		   PGUID 11 f t f 1 f 22 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  41 (  int28out		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  42 (  int4in			   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  43 (  int4out		   PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  44 (  regprocin		   PGUID 11 f t f 1 f 24 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  45 (  regprocout		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  46 (  textin			   PGUID 11 f t f 1 f 25 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
#define TextInRegProcedure 46

DATA(insert OID =  47 (  textout		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  48 (  tidin			   PGUID 11 f t f 1 f 27 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  49 (  tidout			   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  50 (  xidin			   PGUID 11 f t f 1 f 28 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  51 (  xidout			   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  52 (  cidin			   PGUID 11 f t f 1 f 29 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  53 (  cidout			   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  54 (  oid8in			   PGUID 11 f t f 1 f 30 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  55 (  oid8out		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  56 (  boollt			   PGUID 11 f t f 2 f 16 "16 16" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID =  57 (  boolgt			   PGUID 11 f t f 2 f 16 "16 16" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID =  60 (  booleq			   PGUID 11 f t f 2 f 16 "16 16" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID =  61 (  chareq			   PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DESCR("equals");
#define		  CharacterEqualRegProcedure	  61

DATA(insert OID =  62 (  nameeq			   PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DESCR("equals");
#define NameEqualRegProcedure			62

DATA(insert OID =  63 (  int2eq			   PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DESCR("equals");
#define Integer16EqualRegProcedure		63

DATA(insert OID =  64 (  int2lt			   PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID =  65 (  int4eq			   PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DESCR("equals");
#define Integer32EqualRegProcedure		65

DATA(insert OID =  66 (  int4lt			   PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID =  67 (  texteq			   PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0	foo bar ));
DESCR("equals");
#define TextEqualRegProcedure			67

DATA(insert OID =  68 (  xideq			   PGUID 11 f t f 2 f 16 "28 28" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID =  69 (  cideq			   PGUID 11 f t f 2 f 16 "29 29" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID =  70 (  charne			   PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 1246 (  charlt		   PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID =  72 (  charle			   PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID =  73 (  chargt			   PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID =  74 (  charge			   PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1248 (  charpl		   PGUID 11 f t f 2 f 18 "18 18" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 1250 (  charmi		   PGUID 11 f t f 2 f 18 "18 18" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID =  77 (  charmul		   PGUID 11 f t f 2 f 18 "18 18" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID =  78 (  chardiv		   PGUID 11 f t f 2 f 18 "18 18" 100 0 0 100  foo bar ));
DESCR("divide");

DATA(insert OID =  79 (  nameregexeq	   PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DESCR("matches regex., case-sensitive");
DATA(insert OID = 1252 (  nameregexne	   PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DESCR("does not match regex., case-sensitive");
DATA(insert OID = 1254 (  textregexeq	   PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0	foo bar ));
DESCR("matches regex., case-sensitive");
DATA(insert OID = 1256 (  textregexne	   PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0	foo bar ));
DESCR("does not match regex., case-sensitive");
DATA(insert OID = 1257 (  textlen		   PGUID 11 f t f 1 f 23 "25" 100 0 1 0  foo bar ));
DESCR("length");
DATA(insert OID = 1258 (  textcat		   PGUID 11 f t f 2 f 25 "25 25" 100 0 1 0	foo bar ));
DESCR("concat");
DATA(insert OID =  84 (  boolne			   PGUID 11 f t f 2 f 16 "16 16" 100 0 0 100  foo bar ));
DESCR("not equal");

DATA(insert OID = 1265 (  rtsel			   PGUID 11 f t f 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DESCR("r-tree");
DATA(insert OID = 1266 (  rtnpage		   PGUID 11 f t f 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DESCR("r-tree");
DATA(insert OID = 1268 (  btreesel		   PGUID 11 f t f 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DESCR("btree selectivity");

/* OIDS 100 - 199 */

DATA(insert OID = 1270 (  btreenpage	   PGUID 11 f t f 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DESCR("btree");
DATA(insert OID = 1272 (  eqsel			   PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DESCR("general selectivity");
#define EqualSelectivityProcedure 1272

DATA(insert OID = 102 (  neqsel			   PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DESCR("not-equals selectivity");
DATA(insert OID = 103 (  intltsel		   PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DESCR("selectivity");
DATA(insert OID = 104 (  intgtsel		   PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DESCR("selectivity");
DATA(insert OID = 105 (  eqjoinsel		   PGUID 11 f t f 5 f 701 "26 26 21 26 21" 100 0 0 100	foo bar ));
DESCR("selectivity");
DATA(insert OID = 106 (  neqjoinsel		   PGUID 11 f t f 5 f 701 "26 26 21 26 21" 100 0 0 100	foo bar ));
DESCR("selectivity");
DATA(insert OID = 107 (  intltjoinsel	   PGUID 11 f t f 5 f 701 "26 26 21 26 21" 100 0 0 100	foo bar ));
DESCR("selectivity");
DATA(insert OID = 108 (  intgtjoinsel	   PGUID 11 f t f 5 f 701 "26 26 21 26 21" 100 0 0 100	foo bar ));
DESCR("selectivity");

DATA(insert OID = 112 (  int4_text		   PGUID 11 f t f 1 f  25 "23" 100 0 0 100	foo bar ));
DESCR("convert");
DATA(insert OID = 113 (  int2_text		   PGUID 11 f t f 1 f  25 "21" 100 0 0 100	foo bar ));
DESCR("convert");
DATA(insert OID = 114 (  oid_text		   PGUID 11 f t f 1 f  25 "26" 100 0 0 100	foo bar ));
DESCR("convert");

DATA(insert OID = 115 (  box_above		   PGUID 11 f t f 2 f  16 "603 603" 100 1 0 100  foo bar ));
DESCR("is above");
DATA(insert OID = 116 (  box_below		   PGUID 11 f t f 2 f  16 "603 603" 100 1 0 100  foo bar ));
DESCR("is below");

DATA(insert OID = 117 (  point_in		   PGUID 11 f t f 1 f 600 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 118 (  point_out		   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 119 (  lseg_in		   PGUID 11 f t f 1 f 601 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 120 (  lseg_out		   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 121 (  path_in		   PGUID 11 f t f 1 f 602 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 122 (  path_out		   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 123 (  box_in			   PGUID 11 f t f 1 f 603 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 124 (  box_out		   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 125 (  box_overlap	   PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100	foo bar ));
DESCR("overlaps");
DATA(insert OID = 126 (  box_ge			   PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100	foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 127 (  box_gt			   PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 128 (  box_eq			   PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 129 (  box_lt			   PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 130 (  box_le			   PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 131 (  point_above	   PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100	foo bar ));
DESCR("is above");
DATA(insert OID = 132 (  point_left		   PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100	foo bar ));
DESCR("is left of");
DATA(insert OID = 133 (  point_right	   PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100	foo bar ));
DESCR("is left of");
DATA(insert OID = 134 (  point_below	   PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100	foo bar ));
DESCR("is below");
DATA(insert OID = 135 (  point_eq		   PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100	foo bar ));
DESCR("same as");
DATA(insert OID = 136 (  on_pb			   PGUID 11 f t f 2 f 16 "600 603" 100 0 0 100	foo bar ));
DESCR("point is inside");
DATA(insert OID = 137 (  on_ppath		   PGUID 11 f t f 2 f 16 "600 602" 100 0 1 0  foo bar ));
DESCR("contained in");
DATA(insert OID = 138 (  box_center		   PGUID 11 f t f 1 f 600 "603" 100 1 0 100  foo bar ));
DESCR("center of");
DATA(insert OID = 139 (  areasel		   PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DESCR("selectivity");
DATA(insert OID = 140 (  areajoinsel	   PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DESCR("selectivity");
DATA(insert OID = 141 (  int4mul		   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 142 (  int4fac		   PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DESCR("fraction");
DATA(insert OID = 143 (  pointdist		   PGUID 11 f t f 2 f 23 "600 600" 100 0 0 100	foo bar ));
DESCR("");
DATA(insert OID = 144 (  int4ne			   PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 145 (  int2ne			   PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 146 (  int2gt			   PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 147 (  int4gt			   PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 148 (  int2le			   PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 149 (  int4le			   PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 150 (  int4ge			   PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
#define INT4GE_PROC_OID 150
DATA(insert OID = 151 (  int2ge			   PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 152 (  int2mul		   PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 153 (  int2div		   PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID = 154 (  int4div		   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID = 155 (  int2mod		   PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DESCR("modulus");
DATA(insert OID = 156 (  int4mod		   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("modulus");
DATA(insert OID = 157 (  textne			   PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0	foo bar ));
DESCR("not equal");
DATA(insert OID = 158 (  int24eq		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID = 159 (  int42eq		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID = 160 (  int24lt		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 161 (  int42lt		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 162 (  int24gt		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 163 (  int42gt		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 164 (  int24ne		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 165 (  int42ne		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 166 (  int24le		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 167 (  int42le		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 168 (  int24ge		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 169 (  int42ge		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 170 (  int24mul		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 171 (  int42mul		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 172 (  int24div		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID = 173 (  int42div		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID = 174 (  int24mod		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("modulus");
DATA(insert OID = 175 (  int42mod		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("modulus");
DATA(insert OID = 176 (  int2pl			   PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 177 (  int4pl			   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 178 (  int24pl		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 179 (  int42pl		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 180 (  int2mi			   PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 181 (  int4mi			   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 182 (  int24mi		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 183 (  int42mi		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 184 (  oideq			   PGUID 11 f t f 2 f 16 "26 26" 100 0 0 100  foo bar ));
DESCR("equals");
#define ObjectIdEqualRegProcedure		184

DATA(insert OID = 185 (  oidne			   PGUID 11 f t f 2 f 16 "26 26" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 186 (  box_same		   PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100	foo bar ));
DESCR("same as");
DATA(insert OID = 187 (  box_contain	   PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100	foo bar ));
DESCR("contains");
DATA(insert OID = 188 (  box_left		   PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100	foo bar ));
DESCR("is left of");
DATA(insert OID = 189 (  box_overleft	   PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100	foo bar ));
DESCR("overlaps, but does not extend to right of");
DATA(insert OID = 190 (  box_overright	   PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100	foo bar ));
DESCR("overlaps, but does not extend to left of");
DATA(insert OID = 191 (  box_right		   PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100	foo bar ));
DESCR("is left of");
DATA(insert OID = 192 (  box_contained	   PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100	foo bar ));
DESCR("contained in");
DATA(insert OID = 193 (  rt_box_union	   PGUID 11 f t f 2 f 603 "603 603" 100 0 0 100  foo bar ));
DESCR("r-tree");
DATA(insert OID = 194 (  rt_box_inter	   PGUID 11 f t f 2 f 603 "603 603" 100 0 0 100  foo bar ));
DESCR("r-tree");
DATA(insert OID = 195 (  rt_box_size	   PGUID 11 f t f 2 f 700 "603 700" 100 0 0 100  foo bar ));
DESCR("r-tree");
DATA(insert OID = 196 (  rt_bigbox_size    PGUID 11 f t f 2 f 700 "603 700" 100 0 0 100  foo bar ));
DESCR("r-tree");
DATA(insert OID = 197 (  rt_poly_union	   PGUID 11 f t f 2 f 604 "604 604" 100 0 0 100  foo bar ));
DESCR("r-tree");
DATA(insert OID = 198 (  rt_poly_inter	   PGUID 11 f t f 2 f 604 "604 604" 100 0 0 100  foo bar ));
DESCR("r-tree");
DATA(insert OID = 199 (  rt_poly_size	   PGUID 11 f t f 2 f 23 "604 23" 100 0 0 100  foo bar ));
DESCR("r-tree");

/* OIDS 200 - 299 */

DATA(insert OID = 200 (  float4in		   PGUID 11 f t f 1 f 700 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 201 (  float4out		   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 202 (  float4mul		   PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 203 (  float4div		   PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID = 204 (  float4pl		   PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 205 (  float4mi		   PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 206 (  float4um		   PGUID 11 f t f 1 f 700 "700" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 207 (  float4abs		   PGUID 11 f t f 1 f 700 "700 700" 100 0 0 100  foo bar ));
DESCR("absolute value");
DATA(insert OID = 208 (  float4inc		   PGUID 11 f t f 1 f 700 "700" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 209 (  float4larger	   PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 211 (  float4smaller	   PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID = 212 (  int4um			   PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 213 (  int2um			   PGUID 11 f t f 1 f 21 "21" 100 0 0 100  foo bar ));
DESCR("subtract");

DATA(insert OID = 214 (  float8in		   PGUID 11 f t f 1 f 701 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 215 (  float8out		   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 216 (  float8mul		   PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 217 (  float8div		   PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID = 218 (  float8pl		   PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 219 (  float8mi		   PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 220 (  float8um		   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 221 (  float8abs		   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("absolute value");
DATA(insert OID = 222 (  float8inc		   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 223 (  float8larger	   PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 224 (  float8smaller	   PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID = 225 (  lseg_center	   PGUID 11 f t f 1 f 600 "601" 100 0 0 100  foo bar ));
DESCR("center of");
DATA(insert OID = 226 (  path_center	   PGUID 11 f t f 1 f 600 "602" 100 0 0 100  foo bar ));
DESCR("center of");
DATA(insert OID = 227 (  poly_center	   PGUID 11 f t f 1 f 600 "604" 100 0 0 100  foo bar ));
DESCR("center of");

DATA(insert OID = 228 (  dround			   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("truncate to integer");
DATA(insert OID = 229 (  dtrunc			   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("truncate to integer");
DATA(insert OID = 230 (  dsqrt			   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("square root");
DATA(insert OID = 231 (  dcbrt			   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("cube root");
DATA(insert OID = 232 (  dpow			   PGUID 11 f t f 2 f 701 "701" 100 0 0 100  foo bar ));
DESCR("exponentiation");
DATA(insert OID = 233 (  dexp			   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("exponential");
DATA(insert OID = 234 (  dlog1			   PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DESCR("natural logarith (in psql, protect with ()");

DATA(insert OID = 235 (  i2tod			   PGUID 11 f t f 1 f 701  "21" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 236 (  i2tof			   PGUID 11 f t f 1 f 700  "21" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 237 (  dtoi2			   PGUID 11 f t f 1 f  21 "701" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 238 (  ftoi2			   PGUID 11 f t f 1 f  21 "700" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 239 (  line_distance	   PGUID 11 f t f 2 f 701 "628 628" 100 0 0 100  foo bar ));
DESCR("distance between");

DATA(insert OID = 240 (  nabstimein		   PGUID 11 f t f 1 f 702 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 241 (  nabstimeout	   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 242 (  reltimein		   PGUID 11 f t f 1 f 703 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 243 (  reltimeout		   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 244 (  timepl			   PGUID 11 f t f 2 f 702 "702 703" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 245 (  timemi			   PGUID 11 f t f 2 f 702 "702 703" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 246 (  tintervalin	   PGUID 11 f t f 1 f 704 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 247 (  tintervalout	   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 248 (  ininterval		   PGUID 11 f t f 2 f 16 "702 704" 100 0 0 100	foo bar ));
DESCR("abstime in tinterval");
DATA(insert OID = 249 (  intervalrel	   PGUID 11 f t f 1 f 703 "704" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 250 (  timenow		   PGUID 11 f t f 0 f 702 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 251 (  abstimeeq		   PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 252 (  abstimene		   PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 253 (  abstimelt		   PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 254 (  abstimegt		   PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 255 (  abstimele		   PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 256 (  abstimege		   PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 257 (  reltimeeq		   PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 258 (  reltimene		   PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 259 (  reltimelt		   PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 260 (  reltimegt		   PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 261 (  reltimele		   PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 262 (  reltimege		   PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 263 (  intervalsame	   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("same as");
DATA(insert OID = 264 (  intervalct		   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 265 (  intervalov		   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("overlaps");
DATA(insert OID = 266 (  intervalleneq	   PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100	foo bar ));
DESCR("length equals");
DATA(insert OID = 267 (  intervallenne	   PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100	foo bar ));
DESCR("length not equal to");
DATA(insert OID = 268 (  intervallenlt	   PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100	foo bar ));
DESCR("length less-than");
DATA(insert OID = 269 (  intervallengt	   PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100	foo bar ));
DESCR("length greater-than");
DATA(insert OID = 270 (  intervallenle	   PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100	foo bar ));
DESCR("length less-than-or-equals");
DATA(insert OID = 271 (  intervallenge	   PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100	foo bar ));
DESCR("length greater-than-or-equals");
DATA(insert OID = 272 (  intervalstart	   PGUID 11 f t f 1 f 702 "704" 100 0 0 100  foo bar ));
DESCR("start of interval");
DATA(insert OID = 273 (  intervalend	   PGUID 11 f t f 1 f 702 "704" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 274 (  timeofday		   PGUID 11 f t f 0 f 25 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 275 (  abstime_finite    PGUID 11 f t f 1 f 16 "702" 100 0 0 100	foo bar ));
DESCR("");

DATA(insert OID = 276 (  int2fac		   PGUID 11 f t f 1 f 21 "21" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID = 277 (  inter_sl		   PGUID 11 f t f 2 f 16 "601 628" 100 0 0 100	foo bar ));
DESCR("");
DATA(insert OID = 278 (  inter_lb		   PGUID 11 f t f 2 f 16 "628 603" 100 0 0 100	foo bar ));
DESCR("");

DATA(insert OID = 279 (  float48mul		   PGUID 11 f t f 2 f 701 "700 701" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 280 (  float48div		   PGUID 11 f t f 2 f 701 "700 701" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID = 281 (  float48pl		   PGUID 11 f t f 2 f 701 "700 701" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 282 (  float48mi		   PGUID 11 f t f 2 f 701 "700 701" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 283 (  float84mul		   PGUID 11 f t f 2 f 701 "701 700" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 284 (  float84div		   PGUID 11 f t f 2 f 701 "701 700" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID = 285 (  float84pl		   PGUID 11 f t f 2 f 701 "701 700" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 286 (  float84mi		   PGUID 11 f t f 2 f 701 "701 700" 100 0 0 100  foo bar ));
DESCR("subtract");

DATA(insert OID = 287 (  float4eq		   PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 288 (  float4ne		   PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 289 (  float4lt		   PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 290 (  float4le		   PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 291 (  float4gt		   PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 292 (  float4ge		   PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");

DATA(insert OID = 293 (  float8eq		   PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 294 (  float8ne		   PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 295 (  float8lt		   PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 296 (  float8le		   PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 297 (  float8gt		   PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 298 (  float8ge		   PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");

DATA(insert OID = 299 (  float48eq		   PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100	foo bar ));
DESCR("equals");

/* OIDS 300 - 399 */

DATA(insert OID = 300 (  float48ne		   PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 301 (  float48lt		   PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 302 (  float48le		   PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 303 (  float48gt		   PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 304 (  float48ge		   PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 305 (  float84eq		   PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 306 (  float84ne		   PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 307 (  float84lt		   PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 308 (  float84le		   PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 309 (  float84gt		   PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 310 (  float84ge		   PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");

DATA(insert OID = 311 (  ftod			   PGUID 11 f t f 2 f 701 "700" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 312 (  dtof			   PGUID 11 f t f 2 f 700 "701" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 313 (  i2toi4			   PGUID 11 f t f 1 f  23  "21" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 314 (  i4toi2			   PGUID 11 f t f 1 f  21  "23" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 315 (  keyfirsteq		   PGUID 11 f t f 2 f  16	"0 21" 100 0 0 100	foo bar ));
DESCR("");
DATA(insert OID = 316 (  i4tod			   PGUID 11 f t f 1 f 701  "23" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 317 (  dtoi4			   PGUID 11 f t f 1 f  23 "701" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 318 (  i4tof			   PGUID 11 f t f 1 f 700  "23" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 319 (  ftoi4			   PGUID 11 f t f 1 f  23 "700" 100 0 0 100  foo bar ));
DESCR("convert");

DATA(insert OID = 320 (  rtinsert		   PGUID 11 f t f 5 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");
DATA(insert OID = 321 (  rtdelete		   PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");
DATA(insert OID = 322 (  rtgettuple		   PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");
DATA(insert OID = 323 (  rtbuild		   PGUID 11 f t f 9 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");
DATA(insert OID = 324 (  rtbeginscan	   PGUID 11 f t f 4 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");
DATA(insert OID = 325 (  rtendscan		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");
DATA(insert OID = 326 (  rtmarkpos		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");
DATA(insert OID = 327 (  rtrestrpos		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");
DATA(insert OID = 328 (  rtrescan		   PGUID 11 f t f 3 f 23 "0" 100 0 0 100  foo bar ));
DESCR("r-tree(internal)");

DATA(insert OID = 330 (  btgettuple		   PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 331 (  btinsert		   PGUID 11 f t f 5 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 332 (  btdelete		   PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 333 (  btbeginscan	   PGUID 11 f t f 4 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 334 (  btrescan		   PGUID 11 f t f 3 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 335 (  btendscan		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 336 (  btmarkpos		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 337 (  btrestrpos		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 338 (  btbuild		   PGUID 11 f t f 9 f 23 "0" 100 0 0 100  foo bar ));
DESCR("btree(internal)");
DATA(insert OID = 339 (  poly_same		   PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DESCR("same as");
DATA(insert OID = 340 (  poly_contain	   PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DESCR("contains");
DATA(insert OID = 341 (  poly_left		   PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DESCR("is left of");
DATA(insert OID = 342 (  poly_overleft	   PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DESCR("overlaps, but does not extend to right of");
DATA(insert OID = 343 (  poly_overright    PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DESCR("overlaps, but does not extend to left of");
DATA(insert OID = 344 (  poly_right		   PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DESCR("is left of");
DATA(insert OID = 345 (  poly_contained    PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DESCR("contained in");
DATA(insert OID = 346 (  poly_overlap	   PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DESCR("overlaps");
DATA(insert OID = 347 (  poly_in		   PGUID 11 f t f 1 f 604 "0" 100 0 1 0  foo bar ));
DESCR("(internal)");
DATA(insert OID = 348 (  poly_out		   PGUID 11 f t f 1 f 23  "0" 100 0 1 0  foo bar ));
DESCR("(internal)");

DATA(insert OID = 350 (  btint2cmp		   PGUID 11 f t f 2 f 23 "21 21" 100 0 0 100  foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 351 (  btint4cmp		   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 352 (  btint42cmp		   PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 353 (  btint24cmp		   PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 354 (  btfloat4cmp	   PGUID 11 f t f 2 f 23 "700 700" 100 0 0 100	foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 355 (  btfloat8cmp	   PGUID 11 f t f 2 f 23 "701 701" 100 0 0 100	foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 356 (  btoidcmp		   PGUID 11 f t f 2 f 23 "26 26" 100 0 0 100  foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 357 (  btabstimecmp	   PGUID 11 f t f 2 f 23 "702 702" 100 0 0 100	foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 358 (  btcharcmp		   PGUID 11 f t f 2 f 23 "18 18" 100 0 0 100  foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 359 (  btnamecmp		   PGUID 11 f t f 2 f 23 "19 19" 100 0 0 100  foo bar ));
DESCR("btree less-equal-greater");
DATA(insert OID = 360 (  bttextcmp		   PGUID 11 f t f 2 f 23 "25 25" 100 0 0 100  foo bar ));
DESCR("btree less-equal-greater");

DATA(insert OID = 361 (  lseg_distance	   PGUID 11 f t f 2 f 701 "601 601" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 362 (  lseg_interpt	   PGUID 11 f t f 2 f 600 "601 601" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 363 (  dist_ps		   PGUID 11 f t f 2 f 701 "600 601" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 364 (  dist_pb		   PGUID 11 f t f 2 f 701 "600 603" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 365 (  dist_sb		   PGUID 11 f t f 2 f 701 "601 603" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 366 (  close_ps		   PGUID 11 f t f 2 f 600 "600 601" 100 0 0 100  foo bar ));
DESCR("closest point on line segment");
DATA(insert OID = 367 (  close_pb		   PGUID 11 f t f 2 f 600 "600 603" 100 0 0 100  foo bar ));
DESCR("closest point on box");
DATA(insert OID = 368 (  close_sb		   PGUID 11 f t f 2 f 600 "601 603" 100 0 0 100  foo bar ));
DESCR("closest point to line segment on box");
DATA(insert OID = 369 (  on_ps			   PGUID 11 f t f 2 f 16 "600 601" 100 0 0 100	foo bar ));
DESCR("contained in");
DATA(insert OID = 370 (  path_distance	   PGUID 11 f t f 2 f 701 "602 602" 100 0 1 0 foo bar ));
DESCR("distance between");
DATA(insert OID = 371 (  dist_ppath		   PGUID 11 f t f 2 f 701 "600 602" 100 0 1 0 foo bar ));
DESCR("distance between");
DATA(insert OID = 372 (  on_sb			   PGUID 11 f t f 2 f 16 "601 603" 100 0 0 100	foo bar ));
DESCR("contained in");
DATA(insert OID = 373 (  inter_sb		   PGUID 11 f t f 2 f 16 "601 603" 100 0 0 100	foo bar ));
DESCR("");

/* OIDS 400 - 499 */

DATA(insert OID =  438 (  hashsel		   PGUID 11 f t t 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DESCR("selectivity");
DATA(insert OID =  439 (  hashnpage		   PGUID 11 f t t 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DESCR("hash");

DATA(insert OID = 440 (  hashgettuple	   PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 441 (  hashinsert		   PGUID 11 f t f 5 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 442 (  hashdelete		   PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 443 (  hashbeginscan	   PGUID 11 f t f 4 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 444 (  hashrescan		   PGUID 11 f t f 3 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 445 (  hashendscan	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 446 (  hashmarkpos	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 447 (  hashrestrpos	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 448 (  hashbuild		   PGUID 11 f t f 9 f 23 "0" 100 0 0 100  foo bar ));
DESCR("hash(internal)");
DATA(insert OID = 449 (  hashint2		   PGUID 11 f t f 2 f 23 "21 21" 100 0 0 100  foo bar ));
DESCR("hash");
DATA(insert OID = 450 (  hashint4		   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("hash");
DATA(insert OID = 451 (  hashfloat4		   PGUID 11 f t f 2 f 23 "700 700" 100 0 0 100	foo bar ));
DESCR("hash");
DATA(insert OID = 452 (  hashfloat8		   PGUID 11 f t f 2 f 23 "701 701" 100 0 0 100	foo bar ));
DESCR("hash");
DATA(insert OID = 453 (  hashoid		   PGUID 11 f t f 2 f 23 "26 26" 100 0 0 100  foo bar ));
DESCR("hash");
DATA(insert OID = 454 (  hashchar		   PGUID 11 f t f 2 f 23 "18 18" 100 0 0 100  foo bar ));
DESCR("hash");
DATA(insert OID = 455 (  hashname		   PGUID 11 f t f 2 f 23 "19 19" 100 0 0 100  foo bar ));
DESCR("hash");
DATA(insert OID = 456 (  hashtext		   PGUID 11 f t f 2 f 23 "25 25" 100 0 0 100  foo bar ));
DESCR("hash");

/* OIDS 500 - 599 */

/* OIDS 600 - 699 */

DATA(insert OID = 1285 (  int4notin		   PGUID 11 f t f 2 f 16 "21 0" 100 0 0 100  foo bar ));
DESCR("not in");
DATA(insert OID = 1286 (  oidnotin		   PGUID 11 f t f 2 f 16 "26 0" 100 0 0 100  foo bar ));
DESCR("not in");
DATA(insert OID = 1287 (  int44in		   PGUID 11 f t f 1 f 22 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 653 (  int44out		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 655 (  namelt			   PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 656 (  namele			   PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 657 (  namegt			   PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 658 (  namege			   PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 659 (  namene			   PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 682 (  mktinterval	   PGUID 11 f t f 2 f 704 "702 702" 100 0 0 100 foo bar ));
DESCR("convert to interval");
DATA(insert OID = 683 (  oid8eq			   PGUID 11 f t f 2 f 16 "30 30" 100 0 0 100  foo bar ));
DESCR("equals");

/* OIDS 700 - 799 */


DATA(insert OID = 710 (  getpgusername	   PGUID 11 f t f 0 f 19 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 711 (  userfntest		   PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 713 (  oidrand		   PGUID 11 f t f 2 f 16 "26 23" 100 0 0 100  foo bar ));
DESCR("random");
DATA(insert OID = 715 (  oidsrand		   PGUID 11 f t f 1 f 16 "23" 100 0 0 100  foo bar ));
DESCR("seed random number generator");
DATA(insert OID = 716 (  oideqint4		   PGUID 11 f t f 2 f 16 "26 23" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID = 717 (  int4eqoid		   PGUID 11 f t f 2 f 16 "23 26" 100 0 0 100  foo bar ));
DESCR("equals");

DATA(insert OID = 720 (  byteaGetSize	   PGUID 11 f t f 1 f 23 "17" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 721 (  byteaGetByte	   PGUID 11 f t f 2 f 23 "17 23" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 722 (  byteaSetByte	   PGUID 11 f t f 3 f 17 "17 23 23" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 723 (  byteaGetBit	   PGUID 11 f t f 2 f 23 "17 23" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 724 (  byteaSetBit	   PGUID 11 f t f 3 f 17 "17 23 23" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID = 725 (  dist_pl		   PGUID 11 f t f 2 f 701 "600 628" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 726 (  dist_lb		   PGUID 11 f t f 2 f 701 "628 603" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 727 (  dist_sl		   PGUID 11 f t f 2 f 701 "601 628" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 728 (  dist_cpoly		   PGUID 11 f t f 2 f 701 "718 604" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 729 (  poly_distance	   PGUID 11 f t f 2 f 701 "604 604" 100 0 0 100  foo bar ));
DESCR("distance between");

DATA(insert OID = 730 (  pqtest			   PGUID 11 f t f 1 f 23 "25" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID = 740 (  text_lt		   PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0	foo bar ));
DESCR("less-than");
DATA(insert OID = 741 (  text_le		   PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 742 (  text_gt		   PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0	foo bar ));
DESCR("greater-than");
DATA(insert OID = 743 (  text_ge		   PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0	foo bar ));
DESCR("greater-than-or-equals");

DATA(insert OID = 744 (  array_eq		   PGUID 11 f t f 2 f 16 "0 0" 100 0 0 100 foo bar));
DESCR("equals");
DATA(insert OID = 745 (  array_assgn	   PGUID 11 f t f 8 f 23 "0 23 0 0 0 23 23 0" 100 0 0 100 foo bar));
DESCR("array");
DATA(insert OID = 746 (  array_clip		   PGUID 11 f t f 7 f 23 "0 23 0 0 23 23 0" 100 0 0 100 foo bar));
DESCR("array");
DATA(insert OID = 747 (  array_dims		   PGUID 11 f t f 1 f 25 "0" 100 0 0 100 foo bar));
DESCR("array(internal)");
DATA(insert OID = 748 (  array_set		   PGUID 11 f t f 8 f 23 "0 23 0 0 23 23 23 0" 100 0 0 100 foo bar));
DESCR("array");
DATA(insert OID = 749 (  array_ref		   PGUID 11 f t f 7 f 23 "0 23 0 23 23 23 0" 100 0 0 100 foo bar));
DESCR("array");
DATA(insert OID = 750 (  array_in		   PGUID 11 f t f 2 f 23 "0 0" 100 0 0 100	foo bar ));
DESCR("array");
DATA(insert OID = 751 (  array_out		   PGUID 11 f t f 2 f 23 "0 0" 100 0 0 100	foo bar ));
DESCR("array");

DATA(insert OID = 752 (  filename_in	   PGUID 11 f t f 2 f 605 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 753 (  filename_out	   PGUID 11 f t f 2 f 19  "0" 100 0 0 100  foo bar ));
DESCR("(internal)");

DATA(insert OID = 760 (  smgrin			   PGUID 11 f t f 1 f 210 "0" 100 0 0 100  foo bar ));
DESCR("storage manager(internal)");
DATA(insert OID = 761 (  smgrout		   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DESCR("storage manager(internal)");
DATA(insert OID = 762 (  smgreq			   PGUID 11 f t f 2 f 16 "210 210" 100 0 0 100	foo bar ));
DESCR("storage manager");
DATA(insert OID = 763 (  smgrne			   PGUID 11 f t f 2 f 16 "210 210" 100 0 0 100	foo bar ));
DESCR("storage manager");

DATA(insert OID = 764 (  lo_import		   PGUID 11 f t f 1 f 26 "25" 100 0 0 100  foo bar ));
DESCR("large object import");
DATA(insert OID = 765 (  lo_export		   PGUID 11 f t f 2 f 23 "26 25" 100 0 0 100  foo bar ));
DESCR("large object export");

DATA(insert OID = 766 (  int4inc		   PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 767 (  int2inc		   PGUID 11 f t f 1 f 21 "21" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 768 (  int4larger		   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 769 (  int4smaller	   PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 770 (  int2larger		   PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 771 (  int2smaller	   PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 772 (  gistsel		   PGUID 11 f t t 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DESCR("gist selectivity");
DATA(insert OID = 773 (  gistnpage		   PGUID 11 f t t 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DESCR("gist");
DATA(insert OID = 774 (  gistgettuple	   PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");
DATA(insert OID = 775 (  gistinsert		   PGUID 11 f t f 5 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");
DATA(insert OID = 776 (  gistdelete		   PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");
DATA(insert OID = 777 (  gistbeginscan	   PGUID 11 f t f 4 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");
DATA(insert OID = 778 (  gistrescan		   PGUID 11 f t f 3 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");
DATA(insert OID = 779 (  gistendscan	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");
DATA(insert OID = 780 (  gistmarkpos	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");
DATA(insert OID = 781 (  gistrestrpos	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");
DATA(insert OID = 782 (  gistbuild		   PGUID 11 f t f 9 f 23 "0" 100 0 0 100  foo bar ));
DESCR("gist(internal)");

DATA(insert OID = 784 (  intervaleq		   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 785 (  intervalne		   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 786 (  intervallt		   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 787 (  intervalgt		   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 788 (  intervalle		   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 789 (  intervalge		   PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");

/* OIDS 800 - 899 */

DATA(insert OID = 817 (  text_oid		   PGUID 11 f t f 1 f 26 "25" 100 0 0 100  foo bar));
DESCR("convert");
DATA(insert OID = 818 (  text_int2		   PGUID 11 f t f 1 f 21 "25" 100 0 0 100  foo bar));
DESCR("convert");
DATA(insert OID = 819 (  text_int4		   PGUID 11 f t f 1 f 23 "25" 100 0 0 100  foo bar));
DESCR("convert");

DATA(insert OID = 820 (  oidint2in		   PGUID 11 f t f 1 f 810 "0" 100 0 0 100  foo bar));
DESCR("(internal)");
DATA(insert OID = 821 (  oidint2out		   PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar));
DESCR("(internal)");
DATA(insert OID = 822 (  oidint2lt		   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100	foo bar));
DESCR("less-than");
DATA(insert OID = 823 (  oidint2le		   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100	foo bar));
DESCR("less-than-or-equals");
DATA(insert OID = 824 (  oidint2eq		   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100	foo bar));
DESCR("equals");

#define OidInt2EqRegProcedure 824

DATA(insert OID = 825 (  oidint2ge		   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100	foo bar));
DESCR("greater-than-or-equals");
DATA(insert OID = 826 (  oidint2gt		   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100	foo bar));
DESCR("greater-than");
DATA(insert OID = 827 (  oidint2ne		   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100	foo bar));
DESCR("not equal");
DATA(insert OID = 828 (  oidint2cmp		   PGUID 11 f t f 2 f 21 "810 810" 100 0 0 100	foo bar));
DESCR("less-equal-greater");
DATA(insert OID = 829 (  mkoidint2		   PGUID 11 f t f 2 f 810 "26 21" 100 0 0 100  foo bar));
DESCR("");

DATA(insert OID =  849 (  textpos		   PGUID 11 f t f 2 f 23 "25 25" 100 0 1 0 foo bar ));
DESCR("return position of substring");
DATA(insert OID =  850 (  textlike		   PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0 foo bar ));
DESCR("matches LIKE expression");
DATA(insert OID =  851 (  textnlike		   PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0 foo bar ));
DESCR("does not match LIKE expression");
DATA(insert OID =  858 (  namelike		   PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DESCR("matches LIKE expression");
DATA(insert OID =  859 (  namenlike		   PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DESCR("does not match LIKE expression");

DATA(insert OID =  846 (  cash_mul_flt4		   PGUID 11 f t f 2 f 790 "790 700" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID =  847 (  cash_div_flt4		   PGUID 11 f t f 2 f 790 "790 700" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID =  848 (  flt4_mul_cash		   PGUID 11 f t f 2 f 790 "700 790" 100 0 0 100  foo bar ));
DESCR("multiply");

DATA(insert OID =  862 (  int4_mul_cash		   PGUID 11 f t f 2 f 790 "23 790" 100 0 0 100	foo bar ));
DESCR("multiply");
DATA(insert OID =  863 (  int2_mul_cash		   PGUID 11 f t f 2 f 790 "21 790" 100 0 0 100	foo bar ));
DESCR("multiply");
DATA(insert OID =  864 (  cash_mul_int4		   PGUID 11 f t f 2 f 790 "790 23" 100 0 0 100	foo bar ));
DESCR("multiply");
DATA(insert OID =  865 (  cash_div_int4		   PGUID 11 f t f 2 f 790 "790 23" 100 0 0 100	foo bar ));
DESCR("divide");
DATA(insert OID =  866 (  cash_mul_int2		   PGUID 11 f t f 2 f 790 "790 21" 100 0 0 100	foo bar ));
DESCR("multiply");
DATA(insert OID =  867 (  cash_div_int2		   PGUID 11 f t f 2 f 790 "790 21" 100 0 0 100	foo bar ));
DESCR("divide");

DATA(insert OID =  886 (  cash_in		   PGUID 11 f t f 1 f 790 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  887 (  cash_out		   PGUID 11 f t f 1 f  23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID =  1273 (  cash_words_out  PGUID 11 f t f 1 f  25 "790" 100 0 0 100  foo bar ));
DESCR("output amount as words");
DATA(insert OID =  888 (  cash_eq		   PGUID 11 f t f 2 f  16 "790 790" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID =  889 (  cash_ne		   PGUID 11 f t f 2 f  16 "790 790" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID =  890 (  cash_lt		   PGUID 11 f t f 2 f  16 "790 790" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID =  891 (  cash_le		   PGUID 11 f t f 2 f  16 "790 790" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID =  892 (  cash_gt		   PGUID 11 f t f 2 f  16 "790 790" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID =  893 (  cash_ge		   PGUID 11 f t f 2 f  16 "790 790" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID =  894 (  cash_pl		   PGUID 11 f t f 2 f 790 "790 790" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID =  895 (  cash_mi		   PGUID 11 f t f 2 f 790 "790 790" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID =  896 (  cash_mul_flt8		   PGUID 11 f t f 2 f 790 "790 701" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID =  897 (  cash_div_flt8		   PGUID 11 f t f 2 f 790 "790 701" 100 0 0 100  foo bar ));
DESCR("divide");
DATA(insert OID =  898 (  cashlarger	   PGUID 11 f t f 2 f 790 "790 790" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID =  899 (  cashsmaller	   PGUID 11 f t f 2 f 790 "790 790" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID =  919 (  flt8_mul_cash		   PGUID 11 f t f 2 f 790 "701 790" 100 0 0 100  foo bar ));
DESCR("multiply");

/* OIDS 900 - 999 */

DATA(insert OID = 920 (  oidint4in		   PGUID 11 f t f 1 f 910 "0" 100 0 0 100  foo bar));
DESCR("(internal)");
DATA(insert OID = 921 (  oidint4out		   PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar));
DESCR("(internal)");
DATA(insert OID = 922 (  oidint4lt		   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100	foo bar));
DESCR("less-than");
DATA(insert OID = 923 (  oidint4le		   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100	foo bar));
DESCR("less-than-or-equals");
DATA(insert OID = 924 (  oidint4eq		   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100	foo bar));
DESCR("equals");

#define OidInt4EqRegProcedure 924

DATA(insert OID = 925 (  oidint4ge		   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100	foo bar));
DESCR("greater-than-or-equals");
DATA(insert OID = 926 (  oidint4gt		   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100	foo bar));
DESCR("greater-than");
DATA(insert OID = 927 (  oidint4ne		   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100	foo bar));
DESCR("not equal");
DATA(insert OID = 928 (  oidint4cmp		   PGUID 11 f t f 2 f 23 "910 910" 100 0 0 100	foo bar));
DESCR("less-equal-greater");
DATA(insert OID = 929 (  mkoidint4		   PGUID 11 f t f 2 f 910 "26 23" 100 0 0 100  foo bar));
DESCR("");

/* isoldpath, upgradepath, upgradepoly, revertpoly are used to update pre-v6.1 to v6.1 - tgl 97/06/03 */
DATA(insert OID = 936 (  isoldpath		   PGUID 11 f t f 1 f  16 "602" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 937 (  upgradepath	   PGUID 11 f t f 1 f 602 "602" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 938 (  upgradepoly	   PGUID 11 f t f 1 f 604 "604" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 939 (  revertpoly		   PGUID 11 f t f 1 f 604 "604" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID = 940 (  oidnamein		   PGUID 11 f t f 1 f 911 "0" 100 0 0 100  foo bar));
DESCR("(internal)");
DATA(insert OID = 941 (  oidnameout		   PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar));
DESCR("(internal)");
DATA(insert OID = 942 (  oidnamelt		   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100	foo bar));
DESCR("less-than");
DATA(insert OID = 943 (  oidnamele		   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100	foo bar));
DESCR("less-than-or-equals");
DATA(insert OID = 944 (  oidnameeq		   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100	foo bar));
DESCR("equals");

#define OidNameEqRegProcedure 944

DATA(insert OID = 945 (  oidnamege		   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100	foo bar));
DESCR("greater-than-or-equals");
DATA(insert OID = 946 (  oidnamegt		   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100	foo bar));
DESCR("greater-than");
DATA(insert OID = 947 (  oidnamene		   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100	foo bar));
DESCR("not equal");
DATA(insert OID = 948 (  oidnamecmp		   PGUID 11 f t f 2 f 23 "911 911" 100 0 0 100	foo bar));
DESCR("less-equal-greater");
DATA(insert OID = 949 (  mkoidname		   PGUID 11 f t f 2 f 911 "26 19" 100 0 0 100  foo bar));
DESCR("");

DATA(insert OID = 950 (  istrue			   PGUID 11 f t f 1 f 16 "16" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 951 (  isfalse		   PGUID 11 f t f 1 f 16 "16" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID = 952 (  lo_open		   PGUID 11 f t f 2 f 23 "26 23" 100 0 0 100  foo bar ));
DESCR("large object open");
DATA(insert OID = 953 (  lo_close		   PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DESCR("large object close");
DATA(insert OID = 954 (  loread			   PGUID 11 f t f 2 f 17 "23 23" 100 0 0 100  foo bar ));
DESCR("large object read");
DATA(insert OID = 955 (  lowrite		   PGUID 11 f t f 2 f 23 "23 17" 100 0 0 100  foo bar ));
DESCR("large object write");
DATA(insert OID = 956 (  lo_lseek		   PGUID 11 f t f 3 f 23 "23 23 23" 100 0 0 100  foo bar ));
DESCR("large object seek");
DATA(insert OID = 957 (  lo_creat		   PGUID 11 f t f 1 f 26 "23" 100 0 0 100  foo bar ));
DESCR("large object create");
DATA(insert OID = 958 (  lo_tell		   PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DESCR("large object position");

DATA(insert OID = 959 (  on_pl			   PGUID 11 f t f 2 f  16 "600 628" 100 0 10 100  foo bar ));
DESCR("contained in");
DATA(insert OID = 960 (  on_sl			   PGUID 11 f t f 2 f  16 "601 628" 100 0 10 100  foo bar ));
DESCR("contained in");
DATA(insert OID = 961 (  close_pl		   PGUID 11 f t f 2 f 600 "600 628" 100 0 10 100  foo bar ));
DESCR("closest point on line");
DATA(insert OID = 962 (  close_sl		   PGUID 11 f t f 2 f 600 "601 628" 100 0 10 100  foo bar ));
DESCR("closest point to line segment on line");
DATA(insert OID = 963 (  close_lb		   PGUID 11 f t f 2 f 600 "628 603" 100 0 10 100  foo bar ));
DESCR("closest point to line on box");

DATA(insert OID = 964 (  lo_unlink		   PGUID 11 f t f 1 f  23 "23" 100 0 0 100	foo bar ));
DESCR("large object unlink(delete)");
DATA(insert OID = 972 (  regproctooid	   PGUID 11 f t f 1 f  26 "24" 100 0 0 100	foo bar ));
DESCR("get oid for regproc");

DATA(insert OID = 973 (  path_inter		   PGUID 11 f t f 2 f  16 "602 602" 100 0 10 100  foo bar ));
DESCR("");
DATA(insert OID = 975 (  box_area		   PGUID 11 f t f 1 f 701 "603" 100 0 0 100  foo bar ));
DESCR("box area");
DATA(insert OID = 976 (  box_width		   PGUID 11 f t f 1 f 701 "603" 100 0 0 100  foo bar ));
DESCR("box width");
DATA(insert OID = 977 (  box_height		   PGUID 11 f t f 1 f 701 "603" 100 0 0 100  foo bar ));
DESCR("box height");
DATA(insert OID = 978 (  box_distance	   PGUID 11 f t f 2 f 701 "603 603" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 980 (  box_intersect	   PGUID 11 f t f 2 f 603 "603 603" 100 0 0 100  foo bar ));
DESCR("intersects");
DATA(insert OID = 981 (  box_diagonal	   PGUID 11 f t f 1 f 601 "603" 100 0 0 100  foo bar ));
DESCR("box diagonal");
DATA(insert OID = 982 (  path_n_lt		   PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 983 (  path_n_gt		   PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 984 (  path_n_eq		   PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 985 (  path_n_le		   PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 986 (  path_n_ge		   PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 987 (  path_length	   PGUID 11 f t f 1 f 701 "602" 100 0 1 0  foo bar ));
DESCR("sum of path segments");
DATA(insert OID = 988 (  point_ne		   PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 989 (  point_vert		   PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100	foo bar ));
DESCR("is vertical");
DATA(insert OID = 990 (  point_horiz	   PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100	foo bar ));
DESCR("is horizontal");
DATA(insert OID = 991 (  point_distance    PGUID 11 f t f 2 f 701 "600 600" 100 0 0 100  foo bar ));
DESCR("distance between");
DATA(insert OID = 992 (  point_slope	   PGUID 11 f t f 2 f 701 "600 600" 100 0 0 100  foo bar ));
DESCR("slope between points");
DATA(insert OID = 993 (  lseg_construct    PGUID 11 f t f 2 f 601 "600 600" 100 0 0 100  foo bar ));
DESCR("convert points to line segment");
DATA(insert OID = 994 (  lseg_intersect    PGUID 11 f t f 2 f 16 "601 601" 100 0 0 100	foo bar ));
DESCR("intersects");
DATA(insert OID = 995 (  lseg_parallel	   PGUID 11 f t f 2 f 16 "601 601" 100 0 0 100	foo bar ));
DESCR("is parallel to");
DATA(insert OID = 996 (  lseg_perp		   PGUID 11 f t f 2 f 16 "601 601" 100 0 0 100	foo bar ));
DESCR("is perpendicular to");
DATA(insert OID = 997 (  lseg_vertical	   PGUID 11 f t f 1 f 16 "601" 100 0 0 100	foo bar ));
DESCR("is vertical");
DATA(insert OID = 998 (  lseg_horizontal   PGUID 11 f t f 1 f 16 "601" 100 0 0 100	foo bar ));
DESCR("is horizontal");
DATA(insert OID = 999 (  lseg_eq		   PGUID 11 f t f 2 f 16 "601 601" 100 0 0 100	foo bar ));
DESCR("equals");

/* OIDS 1000 - 1999 */

DATA(insert OID = 1029 (  nullvalue		   PGUID 11 f t f 1 f 16 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
#define NullValueRegProcedure 1029
DATA(insert OID = 1030 (  nonnullvalue	   PGUID 11 f t f 1 f 16 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
#define NonNullValueRegProcedure 1030
DATA(insert OID = 1031 (  aclitemin		   PGUID 11 f t f 1 f 1033 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1032 (  aclitemout	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1035 (  aclinsert		   PGUID 11 f t f 2 f 1034 "1034 1033" 100 0 0 100	foo bar ));
DESCR("addition");
DATA(insert OID = 1036 (  aclremove		   PGUID 11 f t f 2 f 1034 "1034 1033" 100 0 0 100	foo bar ));
DESCR("subtract");
DATA(insert OID = 1037 (  aclcontains	   PGUID 11 f t f 2 f 16 "1034 1033" 100 0 0 100  foo bar ));
DESCR("matches regex., case-sensitive");
DATA(insert OID = 1038 (  seteval		   PGUID 11 f t f 1 f 23 "26" 100 0 0 100  foo bar ));
DESCR("");
#define SetEvalRegProcedure 1038

DATA(insert OID = 1044 (  bpcharin		   PGUID 11 f t f 3 f 1042 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1045 (  bpcharout		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1046 (  varcharin		   PGUID 11 f t f 3 f 1043 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1047 (  varcharout	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1048 (  bpchareq		   PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID = 1049 (  bpcharlt		   PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 1050 (  bpcharle		   PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1051 (  bpchargt		   PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 1052 (  bpcharge		   PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1053 (  bpcharne		   PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 1070 (  varchareq		   PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID = 1071 (  varcharlt		   PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 1072 (  varcharle		   PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1073 (  varchargt		   PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 1074 (  varcharge		   PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1075 (  varcharne		   PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 1078 (  bpcharcmp		   PGUID 11 f t f 2 f 23 "1042 1042" 100 0 0 100  foo bar ));
DESCR("less-equal-greater");
DATA(insert OID = 1079 (  varcharcmp	   PGUID 11 f t f 2 f 23 "1043 1043" 100 0 0 100  foo bar ));
DESCR("less-equal-greater");
DATA(insert OID = 1080 (  hashbpchar	   PGUID 11 f t f 1 f 23 "1042" 100 0 0 100  foo bar ));
DESCR("hash");
DATA(insert OID = 1081 (  hashvarchar	   PGUID 11 f t f 1 f 23 "1043" 100 0 0 100  foo bar ));
DESCR("hash");

DATA(insert OID = 1084 (  date_in		   PGUID 11 f t f 1 f 1082 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1085 (  date_out		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1086 (  date_eq		   PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID = 1087 (  date_lt		   PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 1088 (  date_le		   PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1089 (  date_gt		   PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 1090 (  date_ge		   PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1091 (  date_ne		   PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 1092 (  date_cmp		   PGUID 11 f t f 2 f 23 "1082 1082" 100 0 0 100  foo bar ));
DESCR("less-equal-greater");

/* OIDS 1100 - 1199 */

DATA(insert OID = 1102 (  time_lt		   PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 1103 (  time_le		   PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1104 (  time_gt		   PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 1105 (  time_ge		   PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1106 (  time_ne		   PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 1107 (  time_cmp		   PGUID 11 f t f 2 f 23 "1083 1083" 100 0 0 100  foo bar ));
DESCR("less-equal-greater");
DATA(insert OID = 1138 (  date_larger	   PGUID 11 f t f 2 f 1082 "1082 1082" 100 0 0 100	foo bar ));
DESCR("");
DATA(insert OID = 1139 (  date_smaller	   PGUID 11 f t f 2 f 1082 "1082 1082" 100 0 0 100	foo bar ));
DESCR("");
DATA(insert OID = 1140 (  date_mi		   PGUID 11 f t f 2 f 23 "1082 1082" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 1141 (  date_pli		   PGUID 11 f t f 2 f 1082 "1082 23" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 1142 (  date_mii		   PGUID 11 f t f 2 f 1082 "1082 23" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 1143 (  time_in		   PGUID 11 f t f 1 f 1083 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1144 (  time_out		   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1145 (  time_eq		   PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DESCR("equals");

DATA(insert OID = 1146 (  circle_add_pt    PGUID 11 f t f 2 f 718 "718 600" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 1147 (  circle_sub_pt    PGUID 11 f t f 2 f 718 "718 600" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 1148 (  circle_mul_pt    PGUID 11 f t f 2 f 718 "718 600" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 1149 (  circle_div_pt    PGUID 11 f t f 2 f 718 "718 600" 100 0 0 100  foo bar ));
DESCR("divide");

DATA(insert OID = 1150 (  datetime_in	   PGUID 11 f t f 1 f 1184 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1151 (  datetime_out	   PGUID 11 f t f 1 f	23 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1152 (  datetime_eq	   PGUID 11 f t f 2 f	16 "1184 1184" 100 0 0 100	foo bar ));
DESCR("equals");
DATA(insert OID = 1153 (  datetime_ne	   PGUID 11 f t f 2 f	16 "1184 1184" 100 0 0 100	foo bar ));
DESCR("not equal");
DATA(insert OID = 1154 (  datetime_lt	   PGUID 11 f t f 2 f	16 "1184 1184" 100 0 0 100	foo bar ));
DESCR("less-than");
DATA(insert OID = 1155 (  datetime_le	   PGUID 11 f t f 2 f	16 "1184 1184" 100 0 0 100	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1156 (  datetime_ge	   PGUID 11 f t f 2 f	16 "1184 1184" 100 0 0 100	foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1157 (  datetime_gt	   PGUID 11 f t f 2 f	16 "1184 1184" 100 0 0 100	foo bar ));
DESCR("greater-than");
DATA(insert OID = 1158 (  datetime_finite  PGUID 11 f t f 1 f	16 "1184" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1159 (  datetime_zone    PGUID 11 f t f 2 f	25 "25 1184" 100 0 0 100  foo bar ));
DESCR("");

DATA(insert OID = 1160 (  timespan_in	   PGUID 11 f t f 1 f 1186 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1161 (  timespan_out	   PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1162 (  timespan_eq	   PGUID 11 f t f 2 f 16 "1186 1186" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID = 1163 (  timespan_ne	   PGUID 11 f t f 2 f 16 "1186 1186" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 1164 (  timespan_lt	   PGUID 11 f t f 2 f 16 "1186 1186" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 1165 (  timespan_le	   PGUID 11 f t f 2 f 16 "1186 1186" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1166 (  timespan_ge	   PGUID 11 f t f 2 f 16 "1186 1186" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1167 (  timespan_gt	   PGUID 11 f t f 2 f 16 "1186 1186" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 1168 (  timespan_um	   PGUID 11 f t f 1 f 1186 "1186" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 1169 (  timespan_pl	  PGUID 11 f t f 2 f 1186 "1186 1186" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 1170 (  timespan_mi	  PGUID 11 f t f 2 f 1186 "1186 1186" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 1171 (  datetime_part    PGUID 11 f t f 2 f  701 "25 1184" 100 0 0 100  foo bar ));
DESCR("extract field from datetime");
DATA(insert OID = 1172 (  timespan_part    PGUID 11 f t f 2 f  701 "25 1186" 100 0 0 100  foo bar ));
DESCR("extract field from timespan");

DATA(insert OID = 1173 (  abstime_datetime	 PGUID 11 f t f 1 f 1184  "702" 100 0 0 100  foo bar ));
DESCR("convert abstime to datetime");
DATA(insert OID = 1174 (  date_datetime		 PGUID 11 f t f 1 f 1184 "1082" 100 0 0 100  foo bar ));
DESCR("convert date to datetime");
DATA(insert OID = 1175 (  timestamp_datetime PGUID 11 f t f 1 f 1184 "1296" 100 0 0 100  foo bar ));
DESCR("convert timestamp to datetime");
DATA(insert OID = 1176 (  datetime_datetime  PGUID 11 f t f 2 f 1184 "1082 1083" 100 0 0 100  foo bar ));
DESCR("convert date and time to datetime");
DATA(insert OID = 1177 (  reltime_timespan	 PGUID 11 f t f 1 f 1186  "703" 100 0 0 100  foo bar ));
DESCR("convert reltime to timespan");
DATA(insert OID = 1178 (  datetime_date		 PGUID 11 f t f 1 f 1082 "1184" 100 0 0 100  foo bar ));
DESCR("convert datetime to date");
DATA(insert OID = 1179 (  abstime_date		 PGUID 11 f t f 1 f 1082  "702" 100 0 0 100  foo bar ));
DESCR("convert abstime to date");
DATA(insert OID = 1180 (  datetime_abstime	 PGUID 11 f t f 1 f  702 "1184" 100 0 0 100  foo bar ));
DESCR("convert datetime to abstime");

DATA(insert OID = 1188 (  datetime_mi		PGUID 11 f t f 2 f 1186 "1184 1184" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 1189 (  datetime_pl_span	PGUID 11 f t f 2 f 1184 "1184 1186" 100 0 0 100  foo bar ));
DESCR("plus");
DATA(insert OID = 1190 (  datetime_mi_span	PGUID 11 f t f 2 f 1184 "1184 1186" 100 0 0 100  foo bar ));
DESCR("minus");
DATA(insert OID = 1191 (  text_datetime		 PGUID 11 f t f 1 f 1184 "25" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 1192 (  datetime_text		 PGUID 11 f t f 1 f   25 "1184" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 1193 (  timespan_text		 PGUID 11 f t f 1 f   25 "1186" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 1194 (  timespan_reltime	 PGUID 11 f t f 1 f  703 "1186" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 1195 (  datetime_smaller	 PGUID 11 f t f 2 f 1184 "1184 1184" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1196 (  datetime_larger	 PGUID 11 f t f 2 f 1184 "1184 1184" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1197 (  timespan_smaller	 PGUID 11 f t f 2 f 1186 "1186 1186" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1198 (  timespan_larger	 PGUID 11 f t f 2 f 1186 "1186 1186" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1199 (  datetime_age		 PGUID 11 f t f 2 f 1186 "1184 1184" 100 0 0 100  foo bar ));
DESCR("date difference preserving months and years");

/* OIDS 1200 - 1299 */

DATA(insert OID = 1200 (  int42reltime	   PGUID 11 f t f 1 f  703 "21" 100 0 0 100  foo bar ));
DESCR("convert");

DATA(insert OID = 1217 (  datetime_trunc   PGUID 11 f t f 2 f 1184 "25 1184" 100 0 0 100  foo bar ));
DESCR("truncate datetime to specified units");
DATA(insert OID = 1218 (  timespan_trunc   PGUID 11 f t f 2 f 1186 "25 1186" 100 0 0 100  foo bar ));
DESCR("truncate timespan to specified units");

DATA(insert OID = 1238 (  texticregexeq    PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0	foo bar ));
DESCR("matches regex., case-insensitive");
DATA(insert OID = 1239 (  texticregexne    PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0	foo bar ));
DESCR("does not match regex., case-insensitive");
DATA(insert OID = 1240 (  nameicregexeq    PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DESCR("matches regex., case-insensitive");
DATA(insert OID = 1241 (  nameicregexne    PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DESCR("does not match regex., case-insensitive");

DATA(insert OID = 1251 (  bpcharlen		   PGUID 11 f t f 1 f 23 "1042" 100 0 0 100  foo bar ));
DESCR("octet length");
DATA(insert OID = 1253 (  varcharlen	   PGUID 11 f t f 1 f 23 "1043" 100 0 0 100  foo bar ));
DESCR("octet length");

DATA(insert OID = 1263 (  text_timespan    PGUID 11 f t f 1 f 1186 "25" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 1271 (  timespan_finite  PGUID 11 f t f 1 f	16 "1186" 100 0 0 100  foo bar ));
DESCR("boolean test");

DATA(insert OID = 1297 (  timestamp_in	   PGUID 11 f t f 1 f 1296 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1298 (  timestamp_out    PGUID 11 f t f 1 f	23 "0" 100 0 0 100	foo bar ));
DESCR("(internal)");
DATA(insert OID = 1299 (  now			   PGUID 11 f t f 0 f 1296 "0" 100 0 0 100	foo bar ));
DESCR("current transaction time");

/* OIDS 1300 - 1399 */

DATA(insert OID = 1306 (  timestampeq		 PGUID 11 f t f 2 f   16 "1296 1296" 100 0 0 100  foo bar ));
DESCR("equals");
DATA(insert OID = 1307 (  timestampne		 PGUID 11 f t f 2 f   16 "1296 1296" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 1308 (  timestamplt		 PGUID 11 f t f 2 f   16 "1296 1296" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 1309 (  timestampgt		 PGUID 11 f t f 2 f   16 "1296 1296" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 1310 (  timestample		 PGUID 11 f t f 2 f   16 "1296 1296" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1311 (  timestampge		 PGUID 11 f t f 2 f   16 "1296 1296" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1314 (  datetime_cmp		 PGUID 11 f t f 2 f   23 "1184 1184" 100 0 0 100  foo bar ));
DESCR("less-equal-greater");
DATA(insert OID = 1315 (  timespan_cmp		 PGUID 11 f t f 2 f   23 "1186 1186" 100 0 0 100  foo bar ));
DESCR("less-equal-greater");
DATA(insert OID = 1316 (  datetime_time		 PGUID 11 f t f 1 f 1083 "1184" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 1318 (  datetime_timestamp PGUID 11 f t f 1 f 1296 "1184" 100 0 0 100  foo bar ));
DESCR("convert");
DATA(insert OID = 1326 (  timespan_div		 PGUID 11 f t f 2 f 1186 "1186 701" 100 0 0 100  foo bar ));
DESCR("divide");

DATA(insert OID = 1339 (  date_zone			 PGUID 14 f t f 2 f   25 "25 1184" 100 0 0 100	"select datetime_zone($1, $2)" - ));
DESCR("");
DATA(insert OID = 1340 (  text				 PGUID 14 f t f 1 f   25 "1184" 100 0 0 100  "select datetime_text($1)" - ));
DESCR("convert");
DATA(insert OID = 1341 (  text				 PGUID 14 f t f 1 f   25 "1186" 100 0 0 100  "select timespan_text($1)" - ));
DESCR("convert");
DATA(insert OID = 1342 (  text				 PGUID 14 f t f 1 f   25 "23" 100 0 0 100  "select int4_text($1)" - ));
DESCR("convert");
DATA(insert OID = 1343 (  text				 PGUID 14 f t f 1 f   25 "21" 100 0 0 100  "select int2_text($1)" - ));
DESCR("convert");
DATA(insert OID = 1344 (  text				 PGUID 14 f t f 1 f   25 "26" 100 0 0 100  "select oid_text($1)" - ));
DESCR("convert");
DATA(insert OID = 1345 (  oid				 PGUID 14 f t f 1 f   26 "25" 100 0 0 100  "select text_oid($1)" - ));
DESCR("convert");
DATA(insert OID = 1346 (  int2				 PGUID 14 f t f 1 f   21 "25" 100 0 0 100  "select text_int2($1)" - ));
DESCR("convert");
DATA(insert OID = 1347 (  int4				 PGUID 14 f t f 1 f   23 "25" 100 0 0 100  "select text_int4($1)" - ));
DESCR("convert");
DATA(insert OID = 1348 (  obj_description	 PGUID 14 f t f 1 f   25 "26" 100 0 0 100  "select description from pg_description where objoid = $1" - ));
DESCR("get description for object id");
DATA(insert OID = 1349 (  oid8types			 PGUID 11 f t f 1 f   25 "30" 100 0 0 100  foo bar ));
DESCR("print type names of oid8 field");

DATA(insert OID = 1350 (  datetime			 PGUID 14 f t f 1 f 1184 "1184" 100 0 0 100  "select $1" - ));
DESCR("convert");
DATA(insert OID = 1351 (  datetime			 PGUID 14 f t f 1 f 1184 "25" 100 0 0 100  "select text_datetime($1)" - ));
DESCR("convert");
DATA(insert OID = 1352 (  datetime			 PGUID 14 f t f 1 f 1184 "702" 100 0 0 100	"select abstime_datetime($1)" - ));
DESCR("convert");
DATA(insert OID = 1353 (  datetime			 PGUID 14 f t f 1 f 1184 "1082" 100 0 0 100  "select date_datetime($1)" - ));
DESCR("convert");
DATA(insert OID = 1354 (  datetime			 PGUID 14 f t f 1 f 1184 "1296" 100 0 0 100  "select timestamp_datetime($1)" - ));
DESCR("convert");
DATA(insert OID = 1355 (  datetime			 PGUID 14 f t f 2 f 1184 "1082 1083" 100 0 0 100  "select datetime_datetime($1, $2)" - ));
DESCR("convert");
DATA(insert OID = 1356 (  timespan			 PGUID 14 f t f 1 f 1186 "1186" 100 0 0 100  "select $1" - ));
DESCR("convert");
DATA(insert OID = 1357 (  timespan			 PGUID 14 f t f 1 f 1186 "703" 100 0 0 100	"select reltime_timespan($1)" - ));
DESCR("convert");
DATA(insert OID = 1358 (  timespan			 PGUID 14 f t f 1 f 1186 "1083" 100 0 0 100  "select time_timespan($1)" - ));
DESCR("convert");
DATA(insert OID = 1359 (  date				 PGUID 14 f t f 1 f 1082 "1082" 100 0 0 100  "select $1" - ));
DESCR("convert");
DATA(insert OID = 1360 (  date				 PGUID 14 f t f 1 f 1082 "1184" 100 0 0 100  "select datetime_date($1)" - ));
DESCR("convert");
DATA(insert OID = 1361 (  date				 PGUID 14 f t f 1 f 1082 "702" 100 0 0 100	"select abstime_date($1)" - ));
DESCR("convert");
DATA(insert OID = 1362 (  time				 PGUID 14 f t f 1 f 1083 "1083" 100 0 0 100  "select $1" - ));
DESCR("convert");
DATA(insert OID = 1363 (  time				 PGUID 14 f t f 1 f 1083 "1184" 100 0 0 100  "select datetime_time($1)" - ));
DESCR("convert");
DATA(insert OID = 1364 (  time				 PGUID 14 f t f 1 f 1083 "702" 100 0 0 100	"select abstime_time($1)" - ));
DESCR("convert");
DATA(insert OID = 1365 (  abstime			 PGUID 14 f t f 1 f  702 "702" 100 0 0 100	"select $1" - ));
DESCR("convert");
DATA(insert OID = 1366 (  abstime			 PGUID 14 f t f 1 f  702 "1184" 100 0 0 100  "select datetime_abstime($1)" - ));
DESCR("convert");
DATA(insert OID = 1367 (  reltime			 PGUID 14 f t f 1 f  703 "703" 100 0 0 100	"select $1" - ));
DESCR("convert");
DATA(insert OID = 1368 (  reltime			 PGUID 14 f t f 1 f  703 "1186" 100 0 0 100  "select timespan_reltime($1)" - ));
DESCR("convert");
DATA(insert OID = 1369 (  timestamp			 PGUID 14 f t f 1 f 1296 "1296" 100 0 0 100  "select $1" - ));
DESCR("convert");
DATA(insert OID = 1370 (  timestamp			 PGUID 14 f t f 1 f 1296 "1184" 100 0 0 100  "select datetime_stamp($1)" - ));
DESCR("convert");
DATA(insert OID = 1371 (  length			 PGUID 14 f t f 1 f   23   "25" 100 0 0 100  "select textlen($1)" - ));
DESCR("octet length");
DATA(insert OID = 1372 (  length			 PGUID 14 f t f 1 f   23   "1042" 100 0 0 100  "select bpcharlen($1)" - ));
DESCR("octet length");
DATA(insert OID = 1373 (  length			 PGUID 14 f t f 1 f   23   "1043" 100 0 0 100  "select varcharlen($1)" - ));
DESCR("octet length");

DATA(insert OID = 1380 (  date_part    PGUID 14 f t f 2 f  701 "25 1184" 100 0 0 100  "select datetime_part($1, $2)" - ));
DESCR("extract field from datetime");
DATA(insert OID = 1381 (  date_part    PGUID 14 f t f 2 f  701 "25 1186" 100 0 0 100  "select timespan_part($1, $2)" - ));
DESCR("extract field from timespan");
DATA(insert OID = 1382 (  date_part    PGUID 14 f t f 2 f  701 "25 702" 100 0 0 100  "select datetime_part($1, datetime($2))" - ));
DESCR("extract field from abstime");
DATA(insert OID = 1383 (  date_part    PGUID 14 f t f 2 f  701 "25 703" 100 0 0 100  "select timespan_part($1, timespan($2))" - ));
DESCR("extract field from reltime");
DATA(insert OID = 1384 (  date_part    PGUID 14 f t f 2 f  701 "25 1082" 100 0 0 100  "select datetime_part($1, datetime($2))" - ));
DESCR("extract field from date");
DATA(insert OID = 1385 (  date_part    PGUID 14 f t f 2 f  701 "25 1083" 100 0 0 100  "select timespan_part($1, timespan($2))" - ));
DESCR("extract field from time");
DATA(insert OID = 1386 (  date_trunc   PGUID 14 f t f 2 f 1184 "25 1184" 100 0 0 100  "select datetime_trunc($1, $2)" - ));
DESCR("truncate datetime to field");
DATA(insert OID = 1387 (  date_trunc   PGUID 14 f t f 2 f 1186 "25 1186" 100 0 0 100  "select timespan_trunc($1, $2)" - ));
DESCR("truncate timespan to field");
DATA(insert OID = 1388 (  age		   PGUID 14 f t f 2 f 1186 "1184 1184" 100 0 0 100	"select datetime_age($1, $2)" - ));
DESCR("difference between datetimes but leave years and months unresolved");
DATA(insert OID = 1389 (  age		   PGUID 14 f t f 1 f 1186 "1184" 100 0 0 100  "select datetime_age(\'today\', $1)" - ));
DESCR("difference between datetime and today but leave years and months unresolved");

DATA(insert OID = 1390 (  isfinite	   PGUID 14 f t f 1 f	16 "1184" 100 0 0 100  "select datetime_finite($1)" - ));
DESCR("boolean test");
DATA(insert OID = 1391 (  isfinite	   PGUID 14 f t f 1 f	16 "1186" 100 0 0 100  "select timespan_finite($1)" - ));
DESCR("boolean test");
DATA(insert OID = 1392 (  isfinite	   PGUID 14 f t f 1 f	16 "702" 100 0 0 100  "select abstime_finite($1)" - ));
DESCR("boolean test");

DATA(insert OID = 1393 (  timespan	   PGUID 14 f t f 1 f 1186 "25" 100 0 0 100  "select text_timespan($1)" - ));
DESCR("convert");

/* reserve OIDs 1370-1399 for additional date/time conversion routines! tgl 97/04/01 */

/* OIDS 1400 - 1499 */

DATA(insert OID = 1400 (  float		   PGUID 14 f t f 1 f  701	"701" 100 0 0 100  "select $1" - ));
DESCR("convert float8 to float8 (no-op)");
DATA(insert OID = 1401 (  float		   PGUID 14 f t f 1 f  701	"700" 100 0 0 100  "select ftod($1)" - ));
DESCR("convert float4 to float8");
DATA(insert OID = 1402 (  float4	   PGUID 14 f t f 1 f  700	"700" 100 0 0 100  "select $1" - ));
DESCR("convert float4 to float4 (no-op)");
DATA(insert OID = 1403 (  float4	   PGUID 14 f t f 1 f  700	"701" 100 0 0 100  "select dtof($1)" - ));
DESCR("convert float8 to float4");
DATA(insert OID = 1404 (  int		   PGUID 14 f t f 1 f	23	 "23" 100 0 0 100  "select $1" - ));
DESCR("convert int4 to int4 (no-op)");
DATA(insert OID = 1405 (  int2		   PGUID 14 f t f 1 f	21	 "21" 100 0 0 100  "select $1" - ));
DESCR("convert int2 to int2 (no-op)");

DATA(insert OID = 1406 (  float8	   PGUID 14 f t f 1 f  701	"701" 100 0 0 100  "select $1" - ));
DESCR("convert float8 to float8 (no-op)");
DATA(insert OID = 1407 (  float8	   PGUID 14 f t f 1 f  701	"700" 100 0 0 100  "select ftod($1)" - ));
DESCR("convert float4 to float8");
DATA(insert OID = 1408 (  float8	   PGUID 14 f t f 1 f  701	 "23" 100 0 0 100  "select i4tod($1)" - ));
DESCR("convert int4 to float8");
DATA(insert OID = 1409 (  float8	   PGUID 14 f t f 1 f  701	 "21" 100 0 0 100  "select i2tod($1)" - ));
DESCR("convert int2 to float8");
DATA(insert OID = 1410 (  float4	   PGUID 14 f t f 1 f  700	 "23" 100 0 0 100  "select i4tof($1)" - ));
DESCR("convert int4 to float4");
DATA(insert OID = 1411 (  float4	   PGUID 14 f t f 1 f  700	 "21" 100 0 0 100  "select i2tof($1)" - ));
DESCR("convert int2 to float4");
DATA(insert OID = 1412 (  int4		   PGUID 14 f t f 1 f	23	 "23" 100 0 0 100  "select $1" - ));
DESCR("convert int4 to int4 (no-op)");
DATA(insert OID = 1413 (  int4		   PGUID 14 f t f 1 f	23	"701" 100 0 0 100  "select dtoi4($1)" - ));
DESCR("convert float8 to int4");
DATA(insert OID = 1414 (  int4		   PGUID 14 f t f 1 f	23	 "21" 100 0 0 100  "select i2toi4($1)" - ));
DESCR("convert int2 to int4");
DATA(insert OID = 1415 (  int4		   PGUID 14 f t f 1 f	23	"700" 100 0 0 100  "select ftoi4($1)" - ));
DESCR("convert float4 to int4");
DATA(insert OID = 1416 (  int2		   PGUID 14 f t f 1 f	21	 "21" 100 0 0 100  "select $1" - ));
DESCR("convert int2 to int2 (no-op)");
DATA(insert OID = 1417 (  int2		   PGUID 14 f t f 1 f	21	 "23" 100 0 0 100  "select i4toi2($1)" - ));
DESCR("convert int4 to int2");
DATA(insert OID = 1418 (  int2		   PGUID 14 f t f 1 f	21	"701" 100 0 0 100  "select dtoi2($1)" - ));
DESCR("convert float8 to int2");
DATA(insert OID = 1419 (  int2		   PGUID 14 f t f 1 f	21	"700" 100 0 0 100  "select ftoi2($1)" - ));
DESCR("convert float4 to int2");

DATA(insert OID = 1421 (  box				PGUID 11 f t f 2 f 603 "600 600" 100 0 0 100  foo bar ));
DESCR("convert points to box");
DATA(insert OID = 1422 (  box_add			PGUID 11 f t f 2 f 603 "603 600" 100 0 0 100  foo bar ));
DESCR("add point to box (translate)");
DATA(insert OID = 1423 (  box_sub			PGUID 11 f t f 2 f 603 "603 600" 100 0 0 100  foo bar ));
DESCR("subtract point from box (translate)");
DATA(insert OID = 1424 (  box_mul			PGUID 11 f t f 2 f 603 "603 600" 100 0 0 100  foo bar ));
DESCR("multiply box by point (scale)");
DATA(insert OID = 1425 (  box_div			PGUID 11 f t f 2 f 603 "603 600" 100 0 0 100  foo bar ));
DESCR("divide box by point (scale)");
DATA(insert OID = 1426 (  path_contain_pt	PGUID 11 f t f 2 f	16 "602 600" 100 0 0 100  foo bar ));
DESCR("path contains point?");
DATA(insert OID = 1427 (  pt_contained_path PGUID 11 f t f 2 f	16 "600 602" 100 0 0 100  foo bar ));
DESCR("point contained in path?");
DATA(insert OID = 1428 (  poly_contain_pt	PGUID 11 f t f 2 f	16 "604 600" 100 0 0 100  foo bar ));
DESCR("polygon contains point?");
DATA(insert OID = 1429 (  pt_contained_poly PGUID 11 f t f 2 f	16 "600 604" 100 0 0 100  foo bar ));
DESCR("point contained by polygon?");

DATA(insert OID = 1430 (  path_isclosed		PGUID 11 f t f 1 f	16 "602" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1431 (  path_isopen		PGUID 11 f t f 1 f	16 "602" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1432 (  path_npoints		PGUID 11 f t f 1 f	23 "602" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1433 (  path_close		PGUID 11 f t f 1 f 602 "602" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1434 (  path_open			PGUID 11 f t f 1 f 602 "602" 100 0 0 100  foo bar ));
DESCR("");
DATA(insert OID = 1435 (  path_add			PGUID 11 f t f 2 f 602 "602 602" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 1436 (  path_add_pt		PGUID 11 f t f 2 f 602 "602 600" 100 0 0 100  foo bar ));
DESCR("addition");
DATA(insert OID = 1437 (  path_sub_pt		PGUID 11 f t f 2 f 602 "602 600" 100 0 0 100  foo bar ));
DESCR("subtract");
DATA(insert OID = 1438 (  path_mul_pt		PGUID 11 f t f 2 f 602 "602 600" 100 0 0 100  foo bar ));
DESCR("multiply");
DATA(insert OID = 1439 (  path_div_pt		PGUID 11 f t f 2 f 602 "602 600" 100 0 0 100  foo bar ));
DESCR("divide");

DATA(insert OID = 1440 (  point				PGUID 11 f t f 2 f 600 "701 701" 100 0 0 100  foo bar ));
DESCR("convert x, y to point");
DATA(insert OID = 1441 (  point_add			PGUID 11 f t f 2 f 600 "600 600" 100 0 0 100  foo bar ));
DESCR("add points (translate)");
DATA(insert OID = 1442 (  point_sub			PGUID 11 f t f 2 f 600 "600 600" 100 0 0 100  foo bar ));
DESCR("subtract points (translate)");
DATA(insert OID = 1443 (  point_mul			PGUID 11 f t f 2 f 600 "600 600" 100 0 0 100  foo bar ));
DESCR("multiply points (scale/rotate)");
DATA(insert OID = 1444 (  point_div			PGUID 11 f t f 2 f 600 "600 600" 100 0 0 100  foo bar ));
DESCR("divide points (scale/rotate)");

DATA(insert OID = 1445 (  poly_npoints		PGUID 11 f t f 1 f	23 "604" 100 0 0 100  foo bar ));
DESCR("number of points in polygon");
DATA(insert OID = 1446 (  poly_box			PGUID 11 f t f 1 f 603 "604" 100 0 0 100  foo bar ));
DESCR("convert polygon to bounding box");
DATA(insert OID = 1447 (  poly_path			PGUID 11 f t f 1 f 602 "604" 100 0 0 100  foo bar ));
DESCR("convert polygon to path");
DATA(insert OID = 1448 (  box_poly			PGUID 11 f t f 1 f 604 "603" 100 0 0 100  foo bar ));
DESCR("convert box to polygon");
DATA(insert OID = 1449 (  path_poly			PGUID 11 f t f 1 f 604 "602" 100 0 0 100  foo bar ));
DESCR("convert path to polygon");

DATA(insert OID = 1450 (  circle_in			PGUID 11 f t f 1 f 718 "0" 100 0 1 0  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1451 (  circle_out		PGUID 11 f t f 1 f	23	"0" 100 0 1 0  foo bar ));
DESCR("(internal)");
DATA(insert OID = 1452 (  circle_same		PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("same as");
DATA(insert OID = 1453 (  circle_contain	PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("contains");
DATA(insert OID = 1454 (  circle_left		PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("is left of");
DATA(insert OID = 1455 (  circle_overleft	PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("overlaps, but does not extend to right of");
DATA(insert OID = 1456 (  circle_overright	PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("");
DATA(insert OID = 1457 (  circle_right		PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("is left of");
DATA(insert OID = 1458 (  circle_contained	PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("");
DATA(insert OID = 1459 (  circle_overlap	PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("overlaps");
DATA(insert OID = 1460 (  circle_below		PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("is below");
DATA(insert OID = 1461 (  circle_above		PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("is above");
DATA(insert OID = 1462 (  circle_eq			PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("equals");
DATA(insert OID = 1463 (  circle_ne			PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("not equal");
DATA(insert OID = 1464 (  circle_lt			PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("less-than");
DATA(insert OID = 1465 (  circle_gt			PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("greater-than");
DATA(insert OID = 1466 (  circle_le			PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1467 (  circle_ge			PGUID 11 f t f 2 f	16 "718 718" 100 0 1 0	foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1468 (  circle_area		PGUID 11 f t f 1 f 701 "718" 100 0 1 0	foo bar ));
DESCR("area");
DATA(insert OID = 1469 (  circle_diameter	PGUID 11 f t f 1 f 701 "718" 100 0 1 0	foo bar ));
DESCR("diameter");
DATA(insert OID = 1470 (  circle_radius		PGUID 11 f t f 1 f 701 "718" 100 0 1 0	foo bar ));
DESCR("radius");
DATA(insert OID = 1471 (  circle_distance	PGUID 11 f t f 2 f 701 "718 718" 100 0 1 0	foo bar ));
DESCR("distance between");
DATA(insert OID = 1472 (  circle_center		PGUID 11 f t f 1 f 600 "718" 100 0 1 0	foo bar ));
DESCR("center of");
DATA(insert OID = 1473 (  circle			PGUID 11 f t f 2 f 718 "600 701" 100 0 1 0	foo bar ));
DESCR("convert");
DATA(insert OID = 1474 (  poly_circle		PGUID 11 f t f 1 f 718 "604" 100 0 1 0	foo bar ));
DESCR("convert");
DATA(insert OID = 1475 (  circle_poly		PGUID 11 f t f 2 f 604 "23 718" 100 0 1 0  foo bar ));
DESCR("convert");
DATA(insert OID = 1476 (  dist_pc			PGUID 11 f t f 2 f 604 "600 718" 100 0 1 0	foo bar ));
DESCR("distance between");
DATA(insert OID = 1477 (  circle_contain_pt   PGUID 11 f t f 2 f  16 "718 600" 100 0 0 100	foo bar ));
DESCR("");
DATA(insert OID = 1478 (  pt_contained_circle PGUID 11 f t f 2 f  16 "600 718" 100 0 0 100	foo bar ));
DESCR("");
DATA(insert OID = 1479 (  box_circle		PGUID 11 f t f 1 f 718 "603" 100 0 1 0	foo bar ));
DESCR("convert");
DATA(insert OID = 1480 (  circle_box		PGUID 11 f t f 1 f 603 "718" 100 0 1 0	foo bar ));
DESCR("convert");

DATA(insert OID = 1481 (  text_substr		PGUID 11 f t f 3 f 25 "25 23 23" 100 0 0 100  foo bar ));
DESCR("return portion of string");

DATA(insert OID = 1482 (  lseg_ne			PGUID 11 f t f 2 f	16 "601 601" 100 0 0 100  foo bar ));
DESCR("not equal");
DATA(insert OID = 1483 (  lseg_lt			PGUID 11 f t f 2 f	16 "601 601" 100 0 0 100  foo bar ));
DESCR("less-than");
DATA(insert OID = 1484 (  lseg_le			PGUID 11 f t f 2 f	16 "601 601" 100 0 0 100  foo bar ));
DESCR("less-than-or-equals");
DATA(insert OID = 1485 (  lseg_gt			PGUID 11 f t f 2 f	16 "601 601" 100 0 0 100  foo bar ));
DESCR("greater-than");
DATA(insert OID = 1486 (  lseg_ge			PGUID 11 f t f 2 f	16 "601 601" 100 0 0 100  foo bar ));
DESCR("greater-than-or-equals");
DATA(insert OID = 1487 (  lseg_length		PGUID 11 f t f 1 f 701 "601" 100 0 1 0	foo bar ));
DESCR("distance between endpoints");
DATA(insert OID = 1488 (  close_ls			PGUID 11 f t f 2 f 600 "628 601" 100 0 10 100  foo bar ));
DESCR("closest point to line on line segment");
DATA(insert OID = 1489 (  close_lseg		PGUID 11 f t f 2 f 600 "601 601" 100 0 10 100  foo bar ));
DESCR("closest point to line segment on line segment");

DATA(insert OID = 1530 (  point				PGUID 14 f t f 2 f 600 "601 601" 100 0 0 100  "select lseg_interpt($1, $2)" - ));
DESCR("convert");
DATA(insert OID = 1531 (  point				PGUID 14 f t f 1 f 600 "718" 100 0 0 100  "select circle_center($1)" - ));
DESCR("convert");
DATA(insert OID = 1532 (  isvertical		PGUID 14 f t f 2 f	16 "600 600" 100 0 0 100  "select point_vert($1, $2)" - ));
DESCR("");
DATA(insert OID = 1533 (  ishorizontal		PGUID 14 f t f 2 f	16 "600 600" 100 0 0 100  "select point_horiz($1, $2)" - ));
DESCR("");
DATA(insert OID = 1534 (  slope				PGUID 14 f t f 2 f 701 "600 600" 100 0 0 100  "select point_slope($1, $2)" - ));
DESCR("");

DATA(insert OID = 1540 (  lseg				PGUID 14 f t f 2 f 601 "600 600" 100 0 0 100  "select lseg_construct($1, $2)" - ));
DESCR("");
DATA(insert OID = 1541 (  lseg				PGUID 14 f t f 1 f 601 "603" 100 0 0 100  "select box_diagonal($1)" - ));
DESCR("");
DATA(insert OID = 1542 (  isparallel		PGUID 14 f t f 2 f	16 "601 601" 100 0 0 100  "select lseg_parallel($1, $2)" - ));
DESCR("");
DATA(insert OID = 1543 (  isperpendicular	PGUID 14 f t f 2 f	16 "601 601" 100 0 0 100  "select lseg_perp($1, $2)" - ));
DESCR("");
DATA(insert OID = 1544 (  isvertical		PGUID 14 f t f 1 f	16 "601" 100 0 0 100  "select lseg_vertical($1)" - ));
DESCR("");
DATA(insert OID = 1545 (  ishorizontal		PGUID 14 f t f 1 f	16 "601" 100 0 0 100  "select lseg_horizontal($1)" - ));
DESCR("");

/* pclose and popen might better be named close and open, but that crashes initdb.
 * - tgl 97/04/20
 */

DATA(insert OID = 1550 (  path				PGUID 14 f t f 1 f 602 "604" 100 0 0 100  "select poly_path($1)" - ));
DESCR("");
DATA(insert OID = 1551 (  length			PGUID 14 f t f 1 f 701 "602" 100 0 1 0	"select path_length($1)" - ));
DESCR("sum of lengths of path segments");
DATA(insert OID = 1552 (  points			PGUID 14 f t f 1 f	23 "602" 100 0 0 100  "select path_npoints($1)" - ));
DESCR("");
DATA(insert OID = 1553 (  pclose			PGUID 14 f t f 1 f 602 "602" 100 0 0 100  "select path_close($1)" - ));
DESCR("");
DATA(insert OID = 1554 (  popen				PGUID 14 f t f 1 f 602 "602" 100 0 0 100  "select path_open($1)" - ));
DESCR("");
DATA(insert OID = 1555 (  isopen			PGUID 14 f t f 1 f	16 "602" 100 0 0 100  "select path_isopen($1)" - ));
DESCR("");
DATA(insert OID = 1556 (  isclosed			PGUID 14 f t f 1 f	16 "602" 100 0 0 100  "select path_isclosed($1)" - ));
DESCR("");

DATA(insert OID = 1560 (  box				PGUID 14 f t f 2 f 603 "603 603" 100 0 0 100  "select box_intersect($1, $2)" - ));
DESCR("convert");
DATA(insert OID = 1561 (  box				PGUID 14 f t f 1 f 603 "604" 100 0 0 100  "select poly_box($1)" - ));
DESCR("convert");
DATA(insert OID = 1562 (  width				PGUID 14 f t f 1 f 701 "603" 100 0 0 100  "select box_width($1)" - ));
DESCR("");
DATA(insert OID = 1563 (  height			PGUID 14 f t f 1 f 701 "603" 100 0 0 100  "select box_height($1)" - ));
DESCR("");
DATA(insert OID = 1564 (  center			PGUID 14 f t f 1 f 600 "603" 100 0 0 100  "select box_center($1)" - ));
DESCR("");
DATA(insert OID = 1565 (  area				PGUID 14 f t f 1 f 701 "603" 100 0 0 100  "select box_area($1)" - ));
DESCR("");
DATA(insert OID = 1569 (  box				PGUID 14 f t f 1 f 603 "718" 100 0 0 100  "select circle_box($1)" - ));
DESCR("convert");

DATA(insert OID = 1570 (  polygon			PGUID 14 f t f 1 f 604 "602" 100 0 0 100  "select path_poly($1)" - ));
DESCR("convert");
DATA(insert OID = 1571 (  polygon			PGUID 14 f t f 1 f 604 "603" 100 0 0 100  "select box_poly($1)" - ));
DESCR("convert");
DATA(insert OID = 1572 (  polygon			PGUID 14 f t f 2 f 604 "23 718" 100 0 0 100  "select circle_poly($1, $2)" - ));
DESCR("convert");
DATA(insert OID = 1573 (  polygon			PGUID 14 f t f 1 f 604 "718" 100 0 0 100  "select circle_poly(12, $1)" - ));
DESCR("convert");
DATA(insert OID = 1574 (  points			PGUID 14 f t f 1 f	23 "604" 100 0 0 100  "select poly_npoints($1)" - ));
DESCR("");
DATA(insert OID = 1575 (  center			PGUID 14 f t f 1 f 600 "604" 100 0 0 100  "select poly_center($1)" - ));
DESCR("");
DATA(insert OID = 1576 (  length			PGUID 14 f t f 1 f 701 "601" 100 0 1 0	"select lseg_length($1)" - ));
DESCR("distance between endpoints");

DATA(insert OID = 1579 (  circle			PGUID 14 f t f 1 f 718 "603" 100 0 0 100  "select box_circle($1)" - ));
DESCR("convert");
DATA(insert OID = 1580 (  circle			PGUID 14 f t f 1 f 718 "604" 100 0 0 100  "select poly_circle($1)" - ));
DESCR("convert");
DATA(insert OID = 1581 (  center			PGUID 14 f t f 1 f 600 "718" 100 0 0 100  "select circle_center($1)" - ));
DESCR("");
DATA(insert OID = 1582 (  radius			PGUID 14 f t f 1 f 701 "718" 100 0 0 100  "select circle_radius($1)" - ));
DESCR("");
DATA(insert OID = 1583 (  diameter			PGUID 14 f t f 1 f 701 "718" 100 0 0 100  "select circle_diameter($1)" - ));
DESCR("");
DATA(insert OID = 1584 (  area				PGUID 14 f t f 1 f 701 "718" 100 0 0 100  "select circle_area($1)" - ));
DESCR("");

/* Oracle Compatibility Related Functions - By Edmund Mergl <E.Mergl@bawue.de> */
DATA(insert OID =  868 (  strpos	   PGUID 14 f t f 2 f 23 "25 25" 100 0 0 100  "select textpos($1, $2)" - ));
DESCR("find position of substring");

DATA(insert OID =  869 (  trim		   PGUID 14 f t f 1 f 25 "25" 100 0 0 100  "select btrim($1, \' \')" - ));
DESCR("trim trailing whitespace");
DATA(insert OID =  870 (  lower		   PGUID 11 f t f 1 f 25 "25" 100 0 0 100  foo bar ));
DESCR("lowercase");
DATA(insert OID =  871 (  upper		   PGUID 11 f t f 1 f 25 "25" 100 0 0 100  foo bar ));
DESCR("uppercase");
DATA(insert OID =  872 (  initcap	   PGUID 11 f t f 1 f 25 "25" 100 0 0 100  foo bar ));
DESCR("capitalize each word");
DATA(insert OID =  873 (  lpad		   PGUID 11 f t f 3 f 25 "25 23 25" 100 0 0 100  foo bar ));
DESCR("left-pad string to length");
DATA(insert OID =  874 (  rpad		   PGUID 11 f t f 3 f 25 "25 23 25" 100 0 0 100  foo bar ));
DESCR("right-pad string to length");
DATA(insert OID =  875 (  ltrim		   PGUID 11 f t f 2 f 25 "25 25" 100 0 0 100  foo bar ));
DESCR("left-pad string to length");
DATA(insert OID =  876 (  rtrim		   PGUID 11 f t f 2 f 25 "25 25" 100 0 0 100  foo bar ));
DESCR("right-pad string to length");
DATA(insert OID =  877 (  substr	   PGUID 14 f t f 3 f 25 "25 23 23" 100 0 0 100  "select text_substr($1, $2, $3)" - ));
DESCR("return portion of string");
DATA(insert OID =  878 (  translate    PGUID 11 f t f 3 f 25 "25 18 18" 100 0 0 100  foo bar ));
DESCR("modify string by substring replacement");
DATA(insert OID =  879 (  lpad		   PGUID 14 f t f 2 f 25 "25 23" 100 0 0 100  "select lpad($1, $2, \' \')" - ));
DESCR("left-pad string to length");
DATA(insert OID =  880 (  rpad		   PGUID 14 f t f 2 f 25 "25 23" 100 0 0 100  "select rpad($1, $2, \' \')" - ));
DESCR("right-pad string to length");
DATA(insert OID =  881 (  ltrim		   PGUID 14 f t f 1 f 25 "25" 100 0 0 100  "select ltrim($1, \' \')" - ));
DESCR("remove initial characters from string");
DATA(insert OID =  882 (  rtrim		   PGUID 14 f t f 1 f 25 "25" 100 0 0 100  "select rtrim($1, \' \')" - ));
DESCR("remove trailing characters from string");
DATA(insert OID =  883 (  substr	   PGUID 14 f t f 2 f 25 "25 23" 100 0 0 100  "select substr($1, $2, -1)" - ));
DESCR("return portion of string");
DATA(insert OID =  884 (  btrim		   PGUID 11 f t f 2 f 25 "25 25" 100 0 0 100  foo bar ));
DESCR("trim both ends of string");
DATA(insert OID =  885 (  btrim		   PGUID 14 f t f 1 f 25 "25" 100 0 0 100  "select btrim($1, \' \')" - ));
DESCR("trim both ends of string");

/* SEQUENCEs nextval & currval functions */
DATA(insert OID =  1317 (  nextval	   PGUID 11 f t f 1 f 23 "25" 100 0 0 100  foo bar ));
DESCR("sequence next value");
DATA(insert OID =  1319 (  currval	   PGUID 11 f t f 1 f 23 "25" 100 0 0 100  foo bar ));
DESCR("sequence current value");
#define SeqNextValueRegProcedure 1317
#define SeqCurrValueRegProcedure 1319

/*
 * prototypes for functions pg_proc.c
 */
extern Oid
ProcedureCreate(char *procedureName,
				bool returnsSet,
				char *returnTypeName,
				char *languageName,
				char *prosrc,
				char *probin,
				bool canCache,
				bool trusted,
				int32 byte_pct,
				int32 perbyte_cpu,
				int32 percall_cpu,
				int32 outin_ratio,
				List *argList,
				CommandDest dest);


#endif							/* PG_PROC_H */
