/*-------------------------------------------------------------------------
 *
 * pg_proc.h--
 *    definition of the system "procedure" relation (pg_proc)
 *    along with the relation's initial contents.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_proc.h,v 1.9 1997/03/12 21:27:28 scrappy Exp $
 *
 * NOTES
 *    The script catalog/genbki.sh reads this file and generates .bki
 *    information from the DATA() statements.  utils/Gen_fmgrtab.sh 
 *    generates fmgr.h and fmgrtab.c the same way.
 *
 *    XXX do NOT break up DATA() statements into multiple lines!
 *        the scripts are not as smart as you might think...
 *    XXX (eg. #if 0 #endif won't do what you think)
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PROC_H
#define PG_PROC_H

#include <tcop/dest.h>

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *	pg_proc definition.  cpp turns this into
 *	typedef struct FormData_pg_proc
 * ----------------
 */
CATALOG(pg_proc) BOOTSTRAP {
    NameData 	proname;
    Oid 	proowner;
    Oid 	prolang;
    bool 	proisinh;
    bool 	proistrusted;
    bool 	proiscachable;
    int2 	pronargs;
    bool	proretset;
    Oid 	prorettype;
    oid8        proargtypes;
    int4        probyte_pct;
    int4        properbyte_cpu;
    int4        propercall_cpu;
    int4        prooutin_ratio;
    text 	prosrc;		/* VARIABLE LENGTH FIELD */
    bytea 	probin;		/* VARIABLE LENGTH FIELD */
} FormData_pg_proc;

/* ----------------
 *	Form_pg_proc corresponds to a pointer to a tuple with
 *	the format of pg_proc relation.
 * ----------------
 */
typedef FormData_pg_proc	*Form_pg_proc;

/* ----------------
 *	compiler constants for pg_proc
 * ----------------
 */
#define Natts_pg_proc			16
#define Anum_pg_proc_proname		1
#define Anum_pg_proc_proowner		2
#define Anum_pg_proc_prolang		3
#define Anum_pg_proc_proisinh		4
#define Anum_pg_proc_proistrusted	5
#define Anum_pg_proc_proiscachable	6
#define Anum_pg_proc_pronargs		7
#define Anum_pg_proc_proretset		8
#define Anum_pg_proc_prorettype		9
#define Anum_pg_proc_proargtypes        10
#define Anum_pg_proc_probyte_pct        11
#define Anum_pg_proc_properbyte_cpu     12
#define Anum_pg_proc_propercall_cpu     13
#define Anum_pg_proc_prooutin_ratio     14 
#define Anum_pg_proc_prosrc		15
#define Anum_pg_proc_probin		16

/* ----------------
 *	initial contents of pg_proc
 * ----------------
 */

/* keep the following ordered by OID so that later changes can be made easier*/

/* OIDS 1 - 99 */
DATA(insert OID = 1242 (  boolin            PGUID 11 f t f 1 f 16 "0" 100 0 0  100  foo bar ));
DATA(insert OID = 1243 (  boolout           PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1244 (  byteain           PGUID 11 f t f 1 f 17 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  31 (  byteaout          PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1245 (  charin            PGUID 11 f t f 1 f 18 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  33 (  charout           PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  34 (  namein          PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  35 (  nameout         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  36 (  char16in          PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  37 (  char16out         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  38 (  int2in            PGUID 11 f t f 1 f 21 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  39 (  int2out           PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  40 (  int28in           PGUID 11 f t f 1 f 22 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  41 (  int28out          PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  42 (  int4in            PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  43 (  int4out           PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  44 (  regprocin         PGUID 11 f t f 1 f 24 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  45 (  regprocout        PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  46 (  textin            PGUID 11 f t f 1 f 25 "0" 100 0 0 100  foo bar ));
#define TextInRegProcedure 46

DATA(insert OID =  47 (  textout           PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  48 (  tidin             PGUID 11 f t f 1 f 27 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  49 (  tidout            PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  50 (  xidin             PGUID 11 f t f 1 f 28 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  51 (  xidout            PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  52 (  cidin             PGUID 11 f t f 1 f 29 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  53 (  cidout            PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  54 (  oid8in            PGUID 11 f t f 1 f 30 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  55 (  oid8out           PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  60 (  booleq            PGUID 11 f t f 2 f 16 "16 16" 100 0 0 100  foo bar ));
DATA(insert OID =  61 (  chareq            PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
#define       CharacterEqualRegProcedure      61

DATA(insert OID =  62 (  nameeq          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
#define NameEqualRegProcedure		62
    
DATA(insert OID =  63 (  int2eq            PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
#define Integer16EqualRegProcedure	63
    
DATA(insert OID =  64 (  int2lt            PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID =  65 (  int4eq            PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
#define Integer32EqualRegProcedure	65
    
DATA(insert OID =  66 (  int4lt            PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID =  67 (  texteq            PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0  foo bar ));
#define TextEqualRegProcedure           67

DATA(insert OID =  68 (  xideq             PGUID 11 f t f 2 f 16 "28 28" 100 0 0 100  foo bar ));
DATA(insert OID =  69 (  cideq             PGUID 11 f t f 2 f 16 "29 29" 100 0 0 100  foo bar ));
DATA(insert OID =  70 (  charne            PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID = 1246 (  charlt            PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID =  72 (  charle            PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID =  73 (  chargt            PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID =  74 (  charge            PGUID 11 f t f 2 f 16 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID = 1248 (  charpl            PGUID 11 f t f 2 f 18 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID = 1250 (  charmi            PGUID 11 f t f 2 f 18 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID =  77 (  charmul           PGUID 11 f t f 2 f 18 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID =  78 (  chardiv           PGUID 11 f t f 2 f 18 "18 18" 100 0 0 100  foo bar ));

DATA(insert OID =  79 (  nameregexeq     PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DATA(insert OID = 1252 (  nameregexne     PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DATA(insert OID = 1254 (  textregexeq       PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0  foo bar ));
DATA(insert OID = 1256 (  textregexne       PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0  foo bar ));
DATA(insert OID = 1258 (  textcat           PGUID 11 f t f 2 f 25 "25 25" 100 0 1 0  foo bar ));
DATA(insert OID =  84 (  boolne            PGUID 11 f t f 2 f 16 "16 16" 100 0 0 100  foo bar ));

DATA(insert OID = 1265 (  rtsel             PGUID 11 f t f 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DATA(insert OID = 1266 (  rtnpage           PGUID 11 f t f 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DATA(insert OID = 1268 (  btreesel          PGUID 11 f t f 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));

/* OIDS 100 - 199 */

DATA(insert OID = 1270 (  btreenpage        PGUID 11 f t f 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DATA(insert OID = 1272 (  eqsel             PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
#define EqualSelectivityProcedure 1272

DATA(insert OID = 102 (  neqsel            PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DATA(insert OID = 103 (  intltsel          PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DATA(insert OID = 104 (  intgtsel          PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DATA(insert OID = 105 (  eqjoinsel         PGUID 11 f t f 5 f 701 "26 26 21 26 21" 100 0 0 100  foo bar ));
DATA(insert OID = 106 (  neqjoinsel        PGUID 11 f t f 5 f 701 "26 26 21 26 21" 100 0 0 100  foo bar ));
DATA(insert OID = 107 (  intltjoinsel      PGUID 11 f t f 5 f 701 "26 26 21 26 21" 100 0 0 100  foo bar ));
DATA(insert OID = 108 (  intgtjoinsel      PGUID 11 f t f 5 f 701 "26 26 21 26 21" 100 0 0 100  foo bar ));



DATA(insert OID = 117 (  point_in          PGUID 11 f t f 1 f 600 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 118 (  point_out         PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 119 (  lseg_in           PGUID 11 f t f 1 f 601 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 120 (  lseg_out          PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 121 (  path_in           PGUID 11 f t f 1 f 602 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 122 (  path_out          PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 123 (  box_in            PGUID 11 f t f 1 f 603 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 124 (  box_out           PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 125 (  box_overlap       PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100  foo bar ));
DATA(insert OID = 126 (  box_ge            PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100  foo bar ));
DATA(insert OID = 127 (  box_gt            PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100  foo bar ));
DATA(insert OID = 128 (  box_eq            PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100  foo bar ));
DATA(insert OID = 129 (  box_lt            PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100  foo bar ));
DATA(insert OID = 130 (  box_le            PGUID 11 f t f 2 f 16 "603 603" 100 1 0 100  foo bar ));
DATA(insert OID = 131 (  point_above       PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 132 (  point_left        PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 133 (  point_right       PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 134 (  point_below       PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 135 (  point_eq          PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 136 (  on_pb             PGUID 11 f t f 2 f 16 "600 603" 100 0 0 100  foo bar ));
DATA(insert OID = 137 (  on_ppath          PGUID 11 f t f 2 f 16 "600 602" 100 0 1 0  foo bar ));
DATA(insert OID = 138 (  box_center        PGUID 11 f t f 1 f 600 "603" 100 1 0 100  foo bar ));
DATA(insert OID = 139 (  areasel           PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DATA(insert OID = 140 (  areajoinsel       PGUID 11 f t f 5 f 701 "26 26 21 0 23" 100 0 0 100  foo bar ));
DATA(insert OID = 141 (  int4mul           PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 142 (  int4fac           PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 143 (  pointdist         PGUID 11 f t f 2 f 23 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 144 (  int4ne            PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 145 (  int2ne            PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 146 (  int2gt            PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 147 (  int4gt            PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 148 (  int2le            PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 149 (  int4le            PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 150 (  int4ge            PGUID 11 f t f 2 f 16 "23 23" 100 0 0 100  foo bar ));
#define INT4GE_PROC_OID 150
DATA(insert OID = 151 (  int2ge            PGUID 11 f t f 2 f 16 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 152 (  int2mul           PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 153 (  int2div           PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 154 (  int4div           PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 155 (  int2mod           PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 156 (  int4mod           PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 157 (  textne            PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0  foo bar ));
DATA(insert OID = 158 (  int24eq           PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 159 (  int42eq           PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 160 (  int24lt           PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 161 (  int42lt           PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 162 (  int24gt           PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 163 (  int42gt           PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 164 (  int24ne           PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 165 (  int42ne           PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 166 (  int24le           PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 167 (  int42le           PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 168 (  int24ge           PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 169 (  int42ge           PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 170 (  int24mul          PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 171 (  int42mul          PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 172 (  int24div          PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 173 (  int42div          PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 174 (  int24mod          PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 175 (  int42mod          PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 176 (  int2pl            PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 177 (  int4pl            PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 178 (  int24pl           PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 179 (  int42pl           PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 180 (  int2mi            PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 181 (  int4mi            PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 182 (  int24mi           PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 183 (  int42mi           PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 184 (  oideq             PGUID 11 f t f 2 f 16 "26 26" 100 0 0 100  foo bar ));
#define ObjectIdEqualRegProcedure	184
    
DATA(insert OID = 185 (  oidne             PGUID 11 f t f 2 f 16 "26 26" 100 0 0 100  foo bar ));
DATA(insert OID = 186 (  box_same          PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 187 (  box_contain       PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 188 (  box_left          PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 189 (  box_overleft      PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 190 (  box_overright     PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 191 (  box_right         PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 192 (  box_contained     PGUID 11 f t f 2 f 16 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 193 (  rt_box_union      PGUID 11 f t f 2 f 603 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 194 (  rt_box_inter      PGUID 11 f t f 2 f 603 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 195 (  rt_box_size       PGUID 11 f t f 2 f 700 "603 700" 100 0 0 100  foo bar ));
DATA(insert OID = 196 (  rt_bigbox_size    PGUID 11 f t f 2 f 700 "603 700" 100 0 0 100  foo bar ));
DATA(insert OID = 197 (  rt_poly_union     PGUID 11 f t f 2 f 604 "604 604" 100 0 0 100  foo bar ));
DATA(insert OID = 198 (  rt_poly_inter     PGUID 11 f t f 2 f 604 "604 604" 100 0 0 100  foo bar ));
DATA(insert OID = 199 (  rt_poly_size      PGUID 11 f t f 2 f 23 "604 23" 100 0 0 100  foo bar ));

/* OIDS 200 - 299 */

DATA(insert OID = 200 (  float4in          PGUID 11 f t f 1 f 700 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 201 (  float4out         PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 202 (  float4mul         PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 203 (  float4div         PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 204 (  float4pl          PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 205 (  float4mi          PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 206 (  float4um          PGUID 11 f t f 1 f 700 "700" 100 0 0 100  foo bar ));
DATA(insert OID = 207 (  float4abs         PGUID 11 f t f 1 f 700 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 208 (  float4inc         PGUID 11 f t f 1 f 700 "700" 100 0 0 100  foo bar ));
DATA(insert OID = 209 (  float4larger      PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 211 (  float4smaller     PGUID 11 f t f 2 f 700 "700 700" 100 0 0 100  foo bar ));

DATA(insert OID = 212 (  int4um            PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 213 (  int2um            PGUID 11 f t f 1 f 21 "21" 100 0 0 100  foo bar ));
    
DATA(insert OID = 214 (  float8in          PGUID 11 f t f 1 f 701 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 215 (  float8out         PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 216 (  float8mul         PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 217 (  float8div         PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 218 (  float8pl          PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 219 (  float8mi          PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 220 (  float8um          PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 221 (  float8abs         PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 222 (  float8inc         PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 223 (  float8larger      PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 224 (  float8smaller     PGUID 11 f t f 2 f 701 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 228 (  dround            PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 229 (  dtrunc            PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 230 (  dsqrt             PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 231 (  dcbrt             PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 232 (  dpow              PGUID 11 f t f 2 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 233 (  dexp              PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 234 (  dlog1             PGUID 11 f t f 1 f 701 "701" 100 0 0 100  foo bar ));
    
DATA(insert OID = 240 (  nabstimein        PGUID 11 f t f 1 f 702 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 241 (  nabstimeout       PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 242 (  reltimein         PGUID 11 f t f 1 f 703 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 243 (  reltimeout        PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 244 (  timepl            PGUID 11 f t f 2 f 702 "702 703" 100 0 0 100  foo bar ));
DATA(insert OID = 245 (  timemi            PGUID 11 f t f 2 f 702 "702 703" 100 0 0 100  foo bar ));
DATA(insert OID = 246 (  tintervalin       PGUID 11 f t f 1 f 704 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 247 (  tintervalout      PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 248 (  ininterval        PGUID 11 f t f 2 f 16 "702 704" 100 0 0 100  foo bar ));
DATA(insert OID = 249 (  intervalrel       PGUID 11 f t f 1 f 703 "704" 100 0 0 100  foo bar ));
DATA(insert OID = 250 (  timenow           PGUID 11 f t f 0 f 702 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 251 (  abstimeeq         PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100  foo bar ));
DATA(insert OID = 252 (  abstimene         PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100  foo bar ));
DATA(insert OID = 253 (  abstimelt         PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100  foo bar ));
DATA(insert OID = 254 (  abstimegt         PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100  foo bar ));
DATA(insert OID = 255 (  abstimele         PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100  foo bar ));
DATA(insert OID = 256 (  abstimege         PGUID 11 f t f 2 f 16 "702 702" 100 0 0 100  foo bar ));
DATA(insert OID = 257 (  reltimeeq         PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100  foo bar ));
DATA(insert OID = 258 (  reltimene         PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100  foo bar ));
DATA(insert OID = 259 (  reltimelt         PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100  foo bar ));
DATA(insert OID = 260 (  reltimegt         PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100  foo bar ));
DATA(insert OID = 261 (  reltimele         PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100  foo bar ));
DATA(insert OID = 262 (  reltimege         PGUID 11 f t f 2 f 16 "703 703" 100 0 0 100  foo bar ));
DATA(insert OID = 263 (  intervaleq        PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100  foo bar ));
DATA(insert OID = 264 (  intervalct        PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100  foo bar ));
DATA(insert OID = 265 (  intervalov        PGUID 11 f t f 2 f 16 "704 704" 100 0 0 100  foo bar ));
DATA(insert OID = 266 (  intervalleneq     PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100  foo bar ));
DATA(insert OID = 267 (  intervallenne     PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100  foo bar ));
DATA(insert OID = 268 (  intervallenlt     PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100  foo bar ));
DATA(insert OID = 269 (  intervallengt     PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100  foo bar ));
DATA(insert OID = 270 (  intervallenle     PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100  foo bar ));
DATA(insert OID = 271 (  intervallenge     PGUID 11 f t f 2 f 16 "704 703" 100 0 0 100  foo bar ));
DATA(insert OID = 272 (  intervalstart     PGUID 11 f t f 1 f 702 "704" 100 0 0 100  foo bar ));
DATA(insert OID = 273 (  intervalend       PGUID 11 f t f 1 f 702 "704" 100 0 0 100  foo bar ));
DATA(insert OID = 274 (  timeofday         PGUID 11 f t f 0 f 25 "0" 100 0 0 100  foo bar ));

DATA(insert OID = 276 (  int2fac           PGUID 11 f t f 1 f 21 "21" 100 0 0 100  foo bar ));
DATA(insert OID = 279 (  float48mul        PGUID 11 f t f 2 f 701 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 280 (  float48div        PGUID 11 f t f 2 f 701 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 281 (  float48pl         PGUID 11 f t f 2 f 701 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 282 (  float48mi         PGUID 11 f t f 2 f 701 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 283 (  float84mul        PGUID 11 f t f 2 f 701 "701 700" 100 0 0 100  foo bar ));
DATA(insert OID = 284 (  float84div        PGUID 11 f t f 2 f 701 "701 700" 100 0 0 100  foo bar ));
DATA(insert OID = 285 (  float84pl         PGUID 11 f t f 2 f 701 "701 700" 100 0 0 100  foo bar ));
DATA(insert OID = 286 (  float84mi         PGUID 11 f t f 2 f 701 "701 700" 100 0 0 100  foo bar ));

DATA(insert OID = 287 (  float4eq          PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 288 (  float4ne          PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 289 (  float4lt          PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 290 (  float4le          PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 291 (  float4gt          PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 292 (  float4ge          PGUID 11 f t f 2 f 16 "700 700" 100 0 0 100  foo bar ));

DATA(insert OID = 293 (  float8eq          PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 294 (  float8ne          PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 295 (  float8lt          PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 296 (  float8le          PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 297 (  float8gt          PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 298 (  float8ge          PGUID 11 f t f 2 f 16 "701 701" 100 0 0 100  foo bar ));

DATA(insert OID = 299 (  float48eq         PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100  foo bar ));

/* OIDS 300 - 399 */

DATA(insert OID = 300 (  float48ne         PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 301 (  float48lt         PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 302 (  float48le         PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 303 (  float48gt         PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 304 (  float48ge         PGUID 11 f t f 2 f 16 "700 701" 100 0 0 100  foo bar ));
DATA(insert OID = 305 (  float84eq         PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100  foo bar ));
DATA(insert OID = 306 (  float84ne         PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100  foo bar ));
DATA(insert OID = 307 (  float84lt         PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100  foo bar ));
DATA(insert OID = 308 (  float84le         PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100  foo bar ));
DATA(insert OID = 309 (  float84gt         PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100  foo bar ));
DATA(insert OID = 310 (  float84ge         PGUID 11 f t f 2 f 16 "701 700" 100 0 0 100  foo bar ));

DATA(insert OID = 311 (  ftod              PGUID 11 f t f 2 f 701 "700" 100 0 0 100  foo bar ));
DATA(insert OID = 312 (  dtof              PGUID 11 f t f 2 f 700 "701" 100 0 0 100  foo bar ));
DATA(insert OID = 313 (  i2toi4            PGUID 11 f t f 2 f 23 "21" 100 0 0 100  foo bar ));
DATA(insert OID = 314 (  i4toi2            PGUID 11 f t f 2 f 21 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 315 (  keyfirsteq        PGUID 11 f t f 2 f 16 "0 21" 100 0 0 100  foo bar ));

DATA(insert OID = 320 (  rtinsert          PGUID 11 f t f 5 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 321 (  rtdelete          PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 322 (  rtgettuple        PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 323 (  rtbuild           PGUID 11 f t f 9 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 324 (  rtbeginscan       PGUID 11 f t f 4 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 325 (  rtendscan         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 326 (  rtmarkpos         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 327 (  rtrestrpos        PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 328 (  rtrescan          PGUID 11 f t f 3 f 23 "0" 100 0 0 100  foo bar ));

DATA(insert OID = 330 (  btgettuple        PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 331 (  btinsert          PGUID 11 f t f 5 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 332 (  btdelete          PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 333 (  btbeginscan       PGUID 11 f t f 4 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 334 (  btrescan          PGUID 11 f t f 3 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 335 (  btendscan         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 336 (  btmarkpos         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 337 (  btrestrpos        PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 338 (  btbuild           PGUID 11 f t f 9 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 339 (  poly_same         PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DATA(insert OID = 340 (  poly_contain      PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DATA(insert OID = 341 (  poly_left         PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DATA(insert OID = 342 (  poly_overleft     PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DATA(insert OID = 343 (  poly_overright    PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DATA(insert OID = 344 (  poly_right        PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DATA(insert OID = 345 (  poly_contained    PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DATA(insert OID = 346 (  poly_overlap      PGUID 11 f t f 2 f 16 "604 604" 100 0 1 0  foo bar ));
DATA(insert OID = 347 (  poly_in           PGUID 11 f t f 1 f 604 "0" 100 0 1 0  foo bar ));
DATA(insert OID = 348 (  poly_out          PGUID 11 f t f 1 f 23  "0" 100 0 1 0  foo bar ));

DATA(insert OID = 350 (  btint2cmp         PGUID 11 f t f 2 f 23 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 351 (  btint4cmp         PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 352 (  btint42cmp        PGUID 11 f t f 2 f 23 "23 21" 100 0 0 100  foo bar ));
DATA(insert OID = 353 (  btint24cmp        PGUID 11 f t f 2 f 23 "21 23" 100 0 0 100  foo bar ));
DATA(insert OID = 354 (  btfloat4cmp       PGUID 11 f t f 2 f 23 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 355 (  btfloat8cmp       PGUID 11 f t f 2 f 23 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 356 (  btoidcmp          PGUID 11 f t f 2 f 23 "26 26" 100 0 0 100  foo bar ));
DATA(insert OID = 357 (  btabstimecmp      PGUID 11 f t f 2 f 23 "702 702" 100 0 0 100  foo bar ));
DATA(insert OID = 358 (  btcharcmp         PGUID 11 f t f 2 f 23 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID = 359 (  btnamecmp       PGUID 11 f t f 2 f 23 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 360 (  bttextcmp         PGUID 11 f t f 2 f 23 "25 25" 100 0 0 100  foo bar ));

DATA(insert OID = 361 (  lseg_distance     PGUID 11 f t f 2 f 701 "601 601" 100 0 0 100  foo bar ));
DATA(insert OID = 362 (  lseg_interpt      PGUID 11 f t f 2 f 600 "601 601" 100 0 0 100  foo bar ));
DATA(insert OID = 363 (  dist_ps           PGUID 11 f t f 2 f 701 "600 601" 100 0 0 100  foo bar ));
DATA(insert OID = 364 (  dist_pb           PGUID 11 f t f 2 f 701 "600 603" 100 0 0 100  foo bar ));
DATA(insert OID = 365 (  dist_sb           PGUID 11 f t f 2 f 701 "601 603" 100 0 0 100  foo bar ));
DATA(insert OID = 366 (  close_ps          PGUID 11 f t f 2 f 600 "600 601" 100 0 0 100  foo bar ));
DATA(insert OID = 367 (  close_pb          PGUID 11 f t f 2 f 600 "600 603" 100 0 0 100  foo bar ));
DATA(insert OID = 368 (  close_sb          PGUID 11 f t f 2 f 600 "601 603" 100 0 0 100  foo bar ));
DATA(insert OID = 369 (  on_ps             PGUID 11 f t f 2 f 16 "600 601" 100 0 0 100  foo bar ));
DATA(insert OID = 370 (  path_distance     PGUID 11 f t f 2 f 701 "602 602" 100 0 1 0 foo bar ));
DATA(insert OID = 371 (  dist_ppth         PGUID 11 f t f 2 f 701 "600 602" 100 0 1 0 foo bar ));
DATA(insert OID = 372 (  on_sb             PGUID 11 f t f 2 f 16 "601 603" 100 0 0 100  foo bar ));
DATA(insert OID = 373 (  inter_sb          PGUID 11 f t f 2 f 16 "601 603" 100 0 0 100  foo bar ));
DATA(insert OID = 1274 (  btchar16cmp       PGUID 11 f t f 2 f 23 "19 19" 100 0 0 100  foo bar ));

/* OIDS 400 - 499 */

DATA(insert OID =  438 (  hashsel          PGUID 11 f t t 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DATA(insert OID =  439 (  hashnpage        PGUID 11 f t t 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));

DATA(insert OID = 440 (  hashgettuple     PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 441 (  hashinsert       PGUID 11 f t f 5 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 442 (  hashdelete       PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 443 (  hashbeginscan    PGUID 11 f t f 4 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 444 (  hashrescan       PGUID 11 f t f 3 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 445 (  hashendscan      PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 446 (  hashmarkpos      PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 447 (  hashrestrpos     PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 448 (  hashbuild        PGUID 11 f t f 9 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 449 (  hashint2         PGUID 11 f t f 2 f 23 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 450 (  hashint4         PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 451 (  hashfloat4       PGUID 11 f t f 2 f 23 "700 700" 100 0 0 100  foo bar ));
DATA(insert OID = 452 (  hashfloat8       PGUID 11 f t f 2 f 23 "701 701" 100 0 0 100  foo bar ));
DATA(insert OID = 453 (  hashoid          PGUID 11 f t f 2 f 23 "26 26" 100 0 0 100  foo bar ));
DATA(insert OID = 454 (  hashchar         PGUID 11 f t f 2 f 23 "18 18" 100 0 0 100  foo bar ));
DATA(insert OID = 455 (  hashname       PGUID 11 f t f 2 f 23 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 456 (  hashtext         PGUID 11 f t f 2 f 23 "25 25" 100 0 0 100  foo bar ));
DATA(insert OID = 466 (  char2in          PGUID 11 f t f 1 f 409 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 467 (  char4in          PGUID 11 f t f 1 f 410 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 468 (  char8in          PGUID 11 f t f 1 f 411 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 469 (  char2out         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 470 (  char4out         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 471 (  char8out         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 472 (  char2eq          PGUID 11 f t f 2 f 16 "409 409" 100 0 0 100  foo bar ));
DATA(insert OID = 473 (  char4eq          PGUID 11 f t f 2 f 16 "410 410" 100 0 0 100  foo bar ));
DATA(insert OID = 474 (  char8eq          PGUID 11 f t f 2 f 16 "411 411" 100 0 0 100  foo bar ));
DATA(insert OID = 475 (  char2lt          PGUID 11 f t f 2 f 16 "409 409" 100 0 0 100  foo bar ));
DATA(insert OID = 476 (  char4lt          PGUID 11 f t f 2 f 16 "410 410" 100 0 0 100  foo bar ));
DATA(insert OID = 477 (  char8lt          PGUID 11 f t f 2 f 16 "411 411" 100 0 0 100  foo bar ));
DATA(insert OID = 478 (  char2le          PGUID 11 f t f 2 f 16 "409 409" 100 0 0 100  foo bar ));
DATA(insert OID = 479 (  char4le          PGUID 11 f t f 2 f 16 "410 410" 100 0 0 100  foo bar ));
DATA(insert OID = 480 (  char8le          PGUID 11 f t f 2 f 16 "411 411" 100 0 0 100  foo bar ));
DATA(insert OID = 481 (  char2gt          PGUID 11 f t f 2 f 16 "409 409" 100 0 0 100  foo bar ));
DATA(insert OID = 482 (  char4gt          PGUID 11 f t f 2 f 16 "410 410" 100 0 0 100  foo bar ));
DATA(insert OID = 483 (  char8gt          PGUID 11 f t f 2 f 16 "411 411" 100 0 0 100  foo bar ));
DATA(insert OID = 484 (  char2ge          PGUID 11 f t f 2 f 16 "409 409" 100 0 0 100  foo bar ));
DATA(insert OID = 1275 (  char16eq          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
#define Character16EqualRegProcedure	1275
DATA(insert OID = 1276 (  char16lt          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 1277 (  char16le          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 1278 (  char16gt          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 1279 (  char16ge          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 1280 (  char16ne          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));

DATA(insert OID = 1281 (  hashchar16       PGUID 11 f t f 2 f 23 "19 19" 100 0 0 100  foo bar ));

/* OIDS 500 - 599 */

/* OIDS 600 - 699 */

DATA(insert OID = 1285 (  int4notin         PGUID 11 f t f 2 f 16 "21 0" 100 0 0 100  foo bar ));
DATA(insert OID = 1286 (  oidnotin          PGUID 11 f t f 2 f 16 "26 0" 100 0 0 100  foo bar ));
DATA(insert OID = 1287 (  int44in           PGUID 11 f t f 1 f 22 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 653 (  int44out          PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 655 (  namelt          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 656 (  namele          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 657 (  namegt          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 658 (  namege          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 659 (  namene          PGUID 11 f t f 2 f 16 "19 19" 100 0 0 100  foo bar ));
DATA(insert OID = 682 (  mktinterval       PGUID 11 f t f 2 f 704 "702 702" 100 0 0 100 foo bar ));
DATA(insert OID = 683 (  oid8eq		   PGUID 11 f t f 2 f 16 "30 30" 100 0 0 100  foo bar ));
DATA(insert OID = 684 (  char4ge          PGUID 11 f t f 2 f 16 "410 410" 100 0 0 100  foo bar ));
DATA(insert OID = 685 (  char8ge          PGUID 11 f t f 2 f 16 "411 411" 100 0 0 100  foo bar ));
DATA(insert OID = 686 (  char2ne          PGUID 11 f t f 2 f 16 "409 409" 100 0 0 100  foo bar ));
DATA(insert OID = 687 (  char4ne          PGUID 11 f t f 2 f 16 "410 410" 100 0 0 100  foo bar ));
DATA(insert OID = 688 (  char8ne          PGUID 11 f t f 2 f 16 "411 411" 100 0 0 100  foo bar ));
DATA(insert OID = 689 (  btchar2cmp       PGUID 11 f t f 2 f 23 "409 409" 100 0 0 100  foo bar ));
DATA(insert OID = 690 (  btchar4cmp       PGUID 11 f t f 2 f 23 "410 410" 100 0 0 100  foo bar ));
DATA(insert OID = 691 (  btchar8cmp       PGUID 11 f t f 2 f 23 "411 411" 100 0 0 100  foo bar ));
DATA(insert OID = 692 (  hashchar2       PGUID 11 f t f 2 f 23 "409 409" 100 0 0 100  foo bar ));
DATA(insert OID = 693 (  hashchar4       PGUID 11 f t f 2 f 23 "410 410" 100 0 0 100  foo bar ));
DATA(insert OID = 694 (  hashchar8       PGUID 11 f t f 2 f 23 "411 411" 100 0 0 100  foo bar ));
DATA(insert OID =  695 (  char8regexeq     PGUID 11 f t f 2 f 16 "411 25" 100 0 0 100  foo bar ));
DATA(insert OID =  696 (  char8regexne     PGUID 11 f t f 2 f 16 "411 25" 100 0 0 100  foo bar ));
DATA(insert OID =  699 (  char2regexeq     PGUID 11 f t f 2 f 16 "409 25" 100 0 0 100  foo bar ));

/* OIDS 700 - 799 */
DATA(insert OID = 1288 (  char16regexeq     PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DATA(insert OID = 1289 (  char16regexne     PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));

DATA(insert OID = 710 (  GetPgUserName       PGUID 11 f t f 0 f 19 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 711 (  userfntest        PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 713 (  oidrand          PGUID 11 f t f 2 f 16 "26 23" 100 0 0 100  foo bar ));
DATA(insert OID = 715 (  oidsrand         PGUID 11 f t f 1 f 16 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 716 (  oideqint4        PGUID 11 f t f 2 f 16 "26 23" 100 0 0 100  foo bar ));
DATA(insert OID = 717 (  int4eqoid        PGUID 11 f t f 2 f 16 "23 26" 100 0 0 100  foo bar ));


DATA(insert OID = 720 (  byteaGetSize	   PGUID 11 f t f 1 f 23 "17" 100 0 0 100  foo bar ));
DATA(insert OID = 721 (  byteaGetByte	   PGUID 11 f t f 2 f 23 "17 23" 100 0 0 100  foo bar ));
DATA(insert OID = 722 (  byteaSetByte	   PGUID 11 f t f 3 f 17 "17 23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 723 (  byteaGetBit	   PGUID 11 f t f 2 f 23 "17 23" 100 0 0 100  foo bar ));
DATA(insert OID = 724 (  byteaSetBit	   PGUID 11 f t f 3 f 17 "17 23 23" 100 0 0 100  foo bar ));

DATA(insert OID = 730 (  pqtest            PGUID 11 f t f 1 f 23 "25" 100 0 0 100  foo bar ));

DATA(insert OID = 740 (  text_lt           PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0  foo bar ));
DATA(insert OID = 741 (  text_le           PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0  foo bar ));
DATA(insert OID = 742 (  text_gt           PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0  foo bar ));
DATA(insert OID = 743 (  text_ge           PGUID 11 f t f 2 f 16 "25 25" 100 0 0 0  foo bar ));

DATA(insert OID = 744 (  array_eq         PGUID 11 f t f 2 f 16 "0 0" 100 0 0 100 foo bar));
DATA(insert OID = 745 (  array_assgn      PGUID 11 f t f 8 f 23 "0 23 0 0 0 23 23 0" 100 0 0 100 foo bar));
DATA(insert OID = 746 (  array_clip        PGUID 11 f t f 7 f 23 "0 23 0 0 23 23 0" 100 0 0 100 foo bar));
DATA(insert OID = 747 (  array_dims        PGUID 11 f t f 1 f 25 "0" 100 0 0 100 foo bar));
DATA(insert OID = 748 (  array_set         PGUID 11 f t f 8 f 23 "0 23 0 0 23 23 23 0" 100 0 0 100 foo bar));
DATA(insert OID = 749 (  array_ref         PGUID 11 f t f 7 f 23 "0 23 0 23 23 23 0" 100 0 0 100 foo bar));
DATA(insert OID = 750 (  array_in          PGUID 11 f t f 2 f 23 "0 0" 100 0 0 100  foo bar ));
DATA(insert OID = 751 (  array_out         PGUID 11 f t f 2 f 23 "0 0" 100 0 0 100  foo bar ));

DATA(insert OID = 752 (  filename_in       PGUID 11 f t f 2 f 605 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 753 (  filename_out      PGUID 11 f t f 2 f 19  "0" 100 0 0 100  foo bar ));

DATA(insert OID = 760 (  smgrin		   PGUID 11 f t f 1 f 210 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 761 (  smgrout	   PGUID 11 f t f 1 f 23  "0" 100 0 0 100  foo bar ));
DATA(insert OID = 762 (  smgreq		   PGUID 11 f t f 2 f 16 "210 210" 100 0 0 100  foo bar ));
DATA(insert OID = 763 (  smgrne		   PGUID 11 f t f 2 f 16 "210 210" 100 0 0 100  foo bar ));

DATA(insert OID = 764 (  lo_import         PGUID 11 f t f 1 f 26 "25" 100 0 0 100  foo bar ));
DATA(insert OID = 765 (  lo_export         PGUID 11 f t f 2 f 23 "26 25" 100 0 0 100  foo bar ));

DATA(insert OID = 766 (  int4inc           PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 767 (  int2inc           PGUID 11 f t f 1 f 21 "21" 100 0 0 100  foo bar ));
DATA(insert OID = 768 (  int4larger        PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 769 (  int4smaller       PGUID 11 f t f 2 f 23 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 770 (  int2larger        PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID = 771 (  int2smaller       PGUID 11 f t f 2 f 21 "21 21" 100 0 0 100  foo bar ));
DATA(insert OID =  772 (  gistsel          PGUID 11 f t t 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DATA(insert OID =  773 (  gistnpage        PGUID 11 f t t 7 f 701 "26 26 21 0 23 23 26" 100 0 0 100  foo bar ));
DATA(insert OID = 774 (  gistgettuple     PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 775 (  gistinsert       PGUID 11 f t f 5 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 776 (  gistdelete       PGUID 11 f t f 2 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 777 (  gistbeginscan    PGUID 11 f t f 4 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 778 (  gistrescan       PGUID 11 f t f 3 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 779 (  gistendscan      PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 780 (  gistmarkpos      PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 781 (  gistrestrpos     PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 782 (  gistbuild        PGUID 11 f t f 9 f 23 "0" 100 0 0 100  foo bar ));

/* OIDS 800 - 899 */
DATA(insert OID = 820 (  oidint2in	   PGUID 11 f t f 1 f 810 "0" 100 0 0 100  foo bar));
DATA(insert OID = 821 (  oidint2out	   PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar));
DATA(insert OID = 822 (  oidint2lt	   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100  foo bar));
DATA(insert OID = 823 (  oidint2le	   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100  foo bar));
DATA(insert OID = 824 (  oidint2eq	   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100  foo bar));

#define OidInt2EqRegProcedure 824

DATA(insert OID = 825 (  oidint2ge	   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100  foo bar));
DATA(insert OID = 826 (  oidint2gt	   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100  foo bar));
DATA(insert OID = 827 (  oidint2ne	   PGUID 11 f t f 2 f 16 "810 810" 100 0 0 100  foo bar));
DATA(insert OID = 828 (  oidint2cmp	   PGUID 11 f t f 2 f 21 "810 810" 100 0 0 100  foo bar));
DATA(insert OID = 829 (  mkoidint2	   PGUID 11 f t f 2 f 810 "26 21" 100 0 0 100  foo bar));

DATA(insert OID =  837 (  char2regexne     PGUID 11 f t f 2 f 16 "409 25" 100 0 0 100  foo bar ));
DATA(insert OID =  836 (  char4regexeq     PGUID 11 f t f 2 f 16 "410 25" 100 0 0 100  foo bar ));
DATA(insert OID =  838 (  char4regexne     PGUID 11 f t f 2 f 16 "410 25" 100 0 0 100  foo bar ));

DATA(insert OID =  850 (  textlike     PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0 foo bar ));
DATA(insert OID =  851 (  textnlike    PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0 foo bar ));
DATA(insert OID =  852 (  char2like    PGUID 11 f t f 2 f 16 "409 25" 100 0 0 100  foo bar ));
DATA(insert OID =  853 (  char2nlike   PGUID 11 f t f 2 f 16 "409 25" 100 0 0 100  foo bar ));
DATA(insert OID =  854 (  char4like    PGUID 11 f t f 2 f 16 "410 25" 100 0 0 100  foo bar ));
DATA(insert OID =  855 (  char4nlike   PGUID 11 f t f 2 f 16 "410 25" 100 0 0 100  foo bar ));
DATA(insert OID =  856 (  char8like    PGUID 11 f t f 2 f 16 "411 25" 100 0 0 100  foo bar ));
DATA(insert OID =  857 (  char8nlike   PGUID 11 f t f 2 f 16 "411 25" 100 0 0 100  foo bar ));
DATA(insert OID =  858 (  namelike   PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DATA(insert OID =  859 (  namenlike  PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DATA(insert OID =  860 (  char16like   PGUID 11 f t f 2 f 16 "20 25" 100 0 0 100  foo bar ));
DATA(insert OID =  861 (  char16nlike  PGUID 11 f t f 2 f 16 "20 25" 100 0 0 100  foo bar ));
 
/* OIDS 900 - 999 */

DATA(insert OID = 920 (  oidint4in	   PGUID 11 f t f 1 f 910 "0" 100 0 0 100  foo bar));
DATA(insert OID = 921 (  oidint4out	   PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar));
DATA(insert OID = 922 (  oidint4lt	   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100  foo bar));
DATA(insert OID = 923 (  oidint4le	   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100  foo bar));
DATA(insert OID = 924 (  oidint4eq	   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100  foo bar));

#define OidInt4EqRegProcedure 924

DATA(insert OID = 925 (  oidint4ge	   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100  foo bar));
DATA(insert OID = 926 (  oidint4gt	   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100  foo bar));
DATA(insert OID = 927 (  oidint4ne	   PGUID 11 f t f 2 f 16 "910 910" 100 0 0 100  foo bar));
DATA(insert OID = 928 (  oidint4cmp	   PGUID 11 f t f 2 f 23 "910 910" 100 0 0 100  foo bar));
DATA(insert OID = 929 (  mkoidint4	   PGUID 11 f t f 2 f 910 "26 23" 100 0 0 100  foo bar));

DATA(insert OID = 940 (  oidnamein	   PGUID 11 f t f 1 f 911 "0" 100 0 0 100  foo bar));
DATA(insert OID = 941 (  oidnameout	   PGUID 11 f t f 1 f 19 "0" 100 0 0 100  foo bar));
DATA(insert OID = 942 (  oidnamelt	   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100  foo bar));
DATA(insert OID = 943 (  oidnamele	   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100  foo bar));
DATA(insert OID = 944 (  oidnameeq	   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100  foo bar));

#define OidNameEqRegProcedure 944

DATA(insert OID = 945 (  oidnamege	   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100  foo bar));
DATA(insert OID = 946 (  oidnamegt	   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100  foo bar));
DATA(insert OID = 947 (  oidnamene	   PGUID 11 f t f 2 f 16 "911 911" 100 0 0 100  foo bar));
DATA(insert OID = 948 (  oidnamecmp	   PGUID 11 f t f 2 f 23 "911 911" 100 0 0 100  foo bar));
DATA(insert OID = 949 (  mkoidname	   PGUID 11 f t f 2 f 911 "26 19" 100 0 0 100  foo bar));

DATA(insert OID = 952 (  lo_open           PGUID 11 f t f 2 f 23 "26 23" 100 0 0 100  foo bar ));
DATA(insert OID = 953 (  lo_close          PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 954 (  LOread            PGUID 11 f t f 2 f 17 "23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 955 (  LOwrite           PGUID 11 f t f 2 f 23 "23 17" 100 0 0 100  foo bar ));
DATA(insert OID = 956 (  lo_lseek          PGUID 11 f t f 3 f 23 "23 23 23" 100 0 0 100  foo bar ));
DATA(insert OID = 957 (  lo_creat          PGUID 11 f t f 1 f 26 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 958 (  lo_tell           PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));
DATA(insert OID = 964 (  lo_unlink         PGUID 11 f t f 1 f 23 "23" 100 0 0 100  foo bar ));

DATA(insert OID = 972 (  RegprocToOid      PGUID 11 f t f 1 f 26 "24" 100 0 0 100  foo bar ));

DATA(insert OID = 973 (  path_inter        PGUID 11 f t f 2 f 16 "602 602" 100 0 10 100  foo bar ));
DATA(insert OID = 974 (  box_copy          PGUID 11 f t f 1 f 603 "603" 100 0 0 100  foo bar ));
DATA(insert OID = 975 (  box_area          PGUID 11 f t f 1 f 701 "603" 100 0 0 100  foo bar ));
DATA(insert OID = 976 (  box_length        PGUID 11 f t f 1 f 701 "603" 100 0 0 100  foo bar ));
DATA(insert OID = 977 (  box_height        PGUID 11 f t f 1 f 701 "603" 100 0 0 100  foo bar ));
DATA(insert OID = 978 (  box_distance      PGUID 11 f t f 2 f 701 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 980 (  box_intersect     PGUID 11 f t f 2 f 603 "603 603" 100 0 0 100  foo bar ));
DATA(insert OID = 981 (  box_diagonal      PGUID 11 f t f 1 f 601 "603" 100 0 0 100  foo bar ));
DATA(insert OID = 982 (  path_n_lt         PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100  foo bar ));
DATA(insert OID = 983 (  path_n_gt         PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100  foo bar ));
DATA(insert OID = 984 (  path_n_eq         PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100  foo bar ));
DATA(insert OID = 985 (  path_n_le         PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100  foo bar ));
DATA(insert OID = 986 (  path_n_ge         PGUID 11 f t f 2 f 16 "602 602" 100 0 0 100  foo bar ));
DATA(insert OID = 987 (  path_length       PGUID 11 f t f 1 f 701 "602" 100 0 1 0  foo bar ));
DATA(insert OID = 988 (  point_copy        PGUID 11 f t f 1 f 600 "600" 100 0 0 100  foo bar ));
DATA(insert OID = 989 (  point_vert        PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 990 (  point_horiz       PGUID 11 f t f 2 f 16 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 991 (  point_distance    PGUID 11 f t f 2 f 701 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 992 (  point_slope       PGUID 11 f t f 2 f 701 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 993 (  lseg_construct    PGUID 11 f t f 2 f 601 "600 600" 100 0 0 100  foo bar ));
DATA(insert OID = 994 (  lseg_intersect    PGUID 11 f t f 2 f 16 "601 601" 100 0 0 100  foo bar ));
DATA(insert OID = 995 (  lseg_parallel     PGUID 11 f t f 2 f 16 "601 601" 100 0 0 100  foo bar ));
DATA(insert OID = 996 (  lseg_perp         PGUID 11 f t f 2 f 16 "601 601" 100 0 0 100  foo bar ));
DATA(insert OID = 997 (  lseg_vertical     PGUID 11 f t f 1 f 16 "601" 100 0 0 100  foo bar ));
DATA(insert OID = 998 (  lseg_horizontal   PGUID 11 f t f 1 f 16 "601" 100 0 0 100  foo bar ));
DATA(insert OID = 999 (  lseg_eq           PGUID 11 f t f 2 f 16 "601 601" 100 0 0 100  foo bar ));

/* OIDS 1000 - 1999 */

DATA(insert OID = 1029 (  NullValue        PGUID 11 f t f 1 f 16 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1030 (  NonNullValue     PGUID 11 f t f 1 f 16 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1031 (  aclitemin        PGUID 11 f t f 1 f 1033 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1032 (  aclitemout       PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1035 (  aclinsert        PGUID 11 f t f 2 f 1034 "1034 1033" 100 0 0 100  foo bar ));
DATA(insert OID = 1036 (  aclremove        PGUID 11 f t f 2 f 1034 "1034 1033" 100 0 0 100  foo bar ));
DATA(insert OID = 1037 (  aclcontains      PGUID 11 f t f 2 f 16 "1034 1033" 100 0 0 100  foo bar ));
DATA(insert OID = 1038 (  seteval          PGUID 11 f t f 1 f 23 "26" 100 0 0 100  foo bar ));
#define SetEvalRegProcedure 1038

DATA(insert OID = 1044 (  bpcharin         PGUID 11 f t f 3 f 1042 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1045 (  bpcharout        PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1046 (  varcharin        PGUID 11 f t f 3 f 1043 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1047 (  varcharout       PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1048 (  bpchareq         PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DATA(insert OID = 1049 (  bpcharlt         PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DATA(insert OID = 1050 (  bpcharle         PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DATA(insert OID = 1051 (  bpchargt         PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DATA(insert OID = 1052 (  bpcharge         PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DATA(insert OID = 1053 (  bpcharne         PGUID 11 f t f 2 f 16 "1042 1042" 100 0 0 100  foo bar ));
DATA(insert OID = 1070 (  varchareq        PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DATA(insert OID = 1071 (  varcharlt        PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DATA(insert OID = 1072 (  varcharle        PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DATA(insert OID = 1073 (  varchargt        PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DATA(insert OID = 1074 (  varcharge        PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DATA(insert OID = 1075 (  varcharne        PGUID 11 f t f 2 f 16 "1043 1043" 100 0 0 100  foo bar ));
DATA(insert OID = 1078 (  bpcharcmp        PGUID 11 f t f 2 f 23 "1042 1042" 100 0 0 100  foo bar ));
DATA(insert OID = 1079 (  varcharcmp       PGUID 11 f t f 2 f 23 "1043 1043" 100 0 0 100  foo bar ));
DATA(insert OID = 1080 (  hashbpchar       PGUID 11 f t f 1 f 23 "1042" 100 0 0 100  foo bar ));
DATA(insert OID = 1081 (  hashvarchar      PGUID 11 f t f 1 f 23 "1043" 100 0 0 100  foo bar ));

DATA(insert OID = 1084 (  date_in          PGUID 11 f t f 1 f 1082 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1085 (  date_out         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1086 (  date_eq          PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1087 (  date_lt          PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1088 (  date_le          PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1089 (  date_gt          PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1090 (  date_ge          PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1091 (  date_ne          PGUID 11 f t f 2 f 16 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1092 (  date_cmp         PGUID 11 f t f 2 f 23 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1093 (  date_larger      PGUID 11 f t f 2 f 1082 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1094 (  date_smaller     PGUID 11 f t f 2 f 1082 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1095 (  date_mi          PGUID 11 f t f 2 f 23 "1082 1082" 100 0 0 100  foo bar ));
DATA(insert OID = 1096 (  date_pli         PGUID 11 f t f 2 f 1082 "1082 23" 100 0 0 100  foo bar ));
DATA(insert OID = 1097 (  date_mii         PGUID 11 f t f 2 f 1082 "1082 23" 100 0 0 100  foo bar ));
DATA(insert OID = 1099 (  time_in          PGUID 11 f t f 1 f 1083 "0" 100 0 0 100  foo bar ));

/* OIDS 1100 - 1199 */
DATA(insert OID = 1100 (  time_out         PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID = 1101 (  time_eq          PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DATA(insert OID = 1102 (  time_lt          PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DATA(insert OID = 1103 (  time_le          PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DATA(insert OID = 1104 (  time_gt          PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DATA(insert OID = 1105 (  time_ge          PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DATA(insert OID = 1106 (  time_ne          PGUID 11 f t f 2 f 16 "1083 1083" 100 0 0 100  foo bar ));
DATA(insert OID = 1107 (  time_cmp         PGUID 11 f t f 2 f 23 "1083 1083" 100 0 0 100  foo bar ));
DATA(insert OID = 1200 (  int42reltime      PGUID 11 f t f 1 f 703 "21" 100 0 0 100  foo bar ));

DATA(insert OID =  1290 (  char2icregexeq     PGUID 11 f t f 2 f 16 "409 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1291 (  char2icregexne     PGUID 11 f t f 2 f 16 "409 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1292 (  char4icregexeq     PGUID 11 f t f 2 f 16 "410 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1293 (  char4icregexne     PGUID 11 f t f 2 f 16 "410 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1294 (  char8icregexeq     PGUID 11 f t f 2 f 16 "411 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1295 (  char8icregexne     PGUID 11 f t f 2 f 16 "411 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1236 (  char16icregexeq     PGUID 11 f t f 2 f 16 "20 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1237 (  char16icregexne     PGUID 11 f t f 2 f 16 "20 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1238 (  texticregexeq       PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0  foo bar ));
DATA(insert OID =  1239 (  texticregexne       PGUID 11 f t f 2 f 16 "25 25" 100 0 1 0  foo bar ));
DATA(insert OID =  1240 (  nameicregexeq     PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1241 (  nameicregexne     PGUID 11 f t f 2 f 16 "19 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1297 (  timestamp_in      PGUID 11 f t f 1 f 1296 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  1298 (  timestamp_out     PGUID 11 f t f 1 f 23 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  1299 (  now               PGUID 11 f t f 0 f 1296 "0" 100 0 0 100  foo bar ));
DATA(insert OID =  1306 (  timestampeq      PGUID 11 f t f 2 f 16 "1296 1296" 100 0 0 100  foo bar ));
DATA(insert OID =  1307 (  timestampne      PGUID 11 f t f 2 f 16 "1296 1296" 100 0 0 100  foo bar ));
DATA(insert OID =  1308 (  timestamplt      PGUID 11 f t f 2 f 16 "1296 1296" 100 0 0 100  foo bar ));
DATA(insert OID =  1309 (  timestampgt      PGUID 11 f t f 2 f 16 "1296 1296" 100 0 0 100  foo bar ));
DATA(insert OID =  1310 (  timestample      PGUID 11 f t f 2 f 16 "1296 1296" 100 0 0 100  foo bar ));
DATA(insert OID =  1311 (  timestampge      PGUID 11 f t f 2 f 16 "1296 1296" 100 0 0 100  foo bar ));

/* Oracle Compatibility Related Functions - By Edmund Mergl <E.Mergl@bawue.de> */
DATA(insert OID =  1260 (  lower             PGUID 11 f t f 1 f 25 "25" 100 0 0 100  foo bar ));
DATA(insert OID =  1261 (  upper             PGUID 11 f t f 1 f 25 "25" 100 0 0 100  foo bar ));
DATA(insert OID =  1262 (  initcap           PGUID 11 f t f 1 f 25 "25" 100 0 0 100  foo bar ));
DATA(insert OID =  1263 (  lpad              PGUID 11 f t f 3 f 25 "25 23 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1264 (  rpad              PGUID 11 f t f 3 f 25 "25 23 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1265 (  ltrim             PGUID 11 f t f 2 f 25 "25 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1266 (  rtrim             PGUID 11 f t f 2 f 25 "25 25" 100 0 0 100  foo bar ));
DATA(insert OID =  1267 (  substr            PGUID 11 f t f 3 f 25 "25 23 23" 100 0 0 100  foo bar ));
DATA(insert OID =  1268 (  translate         PGUID 11 f t f 3 f 25 "25 18 18" 100 0 0 100  foo bar ));

/* 
 * prototypes for functions pg_proc.c 
 */
extern Oid ProcedureCreate(char* procedureName, 
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


#endif /* PG_PROC_H */
