/*-------------------------------------------------------------------------
 *
 * $Header: /cvsroot/pgsql/src/include/catalog/pg_cast.h,v 1.1 2002/07/18 23:11:30 petere Exp $
 *
 * Copyright (c) 2002, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CAST_H
#define PG_CAST_H

CATALOG(pg_cast)
{
	Oid			castsource;
	Oid			casttarget;
	Oid			castfunc;		/* 0 = binary compatible */
	bool		castimplicit;
} FormData_pg_cast;

typedef FormData_pg_cast *Form_pg_cast;

#define Natts_pg_cast				4
#define Anum_pg_cast_castsource		1
#define Anum_pg_cast_casttarget		2
#define Anum_pg_cast_castfunc		3
#define Anum_pg_cast_castimplicit	4

/* ----------------
 *		initial contents of pg_cast
 * ----------------
 */

/*
 * binary compatible casts
 */
DATA(insert (   25 1042    0 t ));
DATA(insert (   25 1043    0 t ));
DATA(insert ( 1042   25    0 t ));
DATA(insert ( 1042 1043    0 t ));
DATA(insert ( 1043   25    0 t ));
DATA(insert ( 1043 1042    0 t ));

DATA(insert (   23   24    0 t ));
DATA(insert (   23   26    0 t ));
DATA(insert (   23 2202    0 t ));
DATA(insert (   23 2203    0 t ));
DATA(insert (   23 2204    0 t ));
DATA(insert (   23 2205    0 t ));
DATA(insert (   23 2206    0 t ));
DATA(insert (   24   23    0 t ));
DATA(insert (   24   26    0 t ));
DATA(insert (   24 2202    0 t ));
DATA(insert (   24 2203    0 t ));
DATA(insert (   24 2204    0 t ));
DATA(insert (   24 2205    0 t ));
DATA(insert (   24 2206    0 t ));
DATA(insert (   26   23    0 t ));
DATA(insert (   26   24    0 t ));
DATA(insert (   26 2202    0 t ));
DATA(insert (   26 2203    0 t ));
DATA(insert (   26 2204    0 t ));
DATA(insert (   26 2205    0 t ));
DATA(insert (   26 2206    0 t ));
DATA(insert ( 2202   23    0 t ));
DATA(insert ( 2202   24    0 t ));
DATA(insert ( 2202   26    0 t ));
DATA(insert ( 2202 2203    0 t ));
DATA(insert ( 2202 2204    0 t ));
DATA(insert ( 2202 2205    0 t ));
DATA(insert ( 2202 2206    0 t ));
DATA(insert ( 2203   23    0 t ));
DATA(insert ( 2203   24    0 t ));
DATA(insert ( 2203   26    0 t ));
DATA(insert ( 2203 2202    0 t ));
DATA(insert ( 2203 2204    0 t ));
DATA(insert ( 2203 2205    0 t ));
DATA(insert ( 2203 2206    0 t ));
DATA(insert ( 2204   23    0 t ));
DATA(insert ( 2204   24    0 t ));
DATA(insert ( 2204   26    0 t ));
DATA(insert ( 2204 2202    0 t ));
DATA(insert ( 2204 2203    0 t ));
DATA(insert ( 2204 2205    0 t ));
DATA(insert ( 2204 2206    0 t ));
DATA(insert ( 2205   23    0 t ));
DATA(insert ( 2205   24    0 t ));
DATA(insert ( 2205   26    0 t ));
DATA(insert ( 2205 2202    0 t ));
DATA(insert ( 2205 2203    0 t ));
DATA(insert ( 2205 2204    0 t ));
DATA(insert ( 2205 2206    0 t ));
DATA(insert ( 2206   23    0 t ));
DATA(insert ( 2206   24    0 t ));
DATA(insert ( 2206   26    0 t ));
DATA(insert ( 2206 2202    0 t ));
DATA(insert ( 2206 2203    0 t ));
DATA(insert ( 2206 2204    0 t ));
DATA(insert ( 2206 2205    0 t ));

DATA(insert (   23  702    0 t ));
DATA(insert (  702   23    0 t ));

DATA(insert (   23  703    0 t ));
DATA(insert (  703   23    0 t ));

DATA(insert (  650  869    0 t ));
DATA(insert (  869  650    0 t ));

DATA(insert ( 1560 1562    0 t ));
DATA(insert ( 1562 1560    0 t ));

/*
 * regular casts through a function
 *
 * This list can be obtained from the following query as long as the
 * naming convention of the cast functions remains the same:
 *
 * select p.proargtypes[0] as source, p.prorettype as target, p.oid as func, p.proimplicit as implicit from pg_proc p, pg_type t where p.pronargs=1 and p.proname = t.typname and p.prorettype = t.oid order by 1, 2;
 */
DATA(insert (   18   25  946 t ));
DATA(insert (   18 1042  860 t ));
DATA(insert (   19   25  406 t ));
DATA(insert (   19 1042  408 t ));
DATA(insert (   19 1043 1401 t ));
DATA(insert (   20   21  714 t ));
DATA(insert (   20   23  480 t ));
DATA(insert (   20   25 1288 t ));
DATA(insert (   20  701  482 t ));
DATA(insert (   20 1043 1623 f ));
DATA(insert (   20 1700 1781 t ));
DATA(insert (   21   20  754 t ));
DATA(insert (   21   23  313 t ));
DATA(insert (   21   25  113 t ));
DATA(insert (   21  700  236 t ));
DATA(insert (   21  701  235 t ));
DATA(insert (   21 1700 1782 t ));
DATA(insert (   23   20  481 t ));
DATA(insert (   23   21  314 t ));
DATA(insert (   23   25  112 t ));
DATA(insert (   23  700  318 t ));
DATA(insert (   23  701  316 t ));
/*xDATA(insert (   23  703 1200 f ));*/
DATA(insert (   23 1043 1619 f ));
DATA(insert (   23 1700 1740 t ));
DATA(insert (   25   18  944 t ));
DATA(insert (   25   19  407 t ));
DATA(insert (   25   20 1289 f ));
DATA(insert (   25   21  818 f ));
DATA(insert (   25   23  819 f ));
DATA(insert (   25   26  817 f ));
DATA(insert (   25  650 1714 f ));
DATA(insert (   25  700  839 f ));
DATA(insert (   25  701  838 f ));
DATA(insert (   25  829  767 f ));
DATA(insert (   25  869 1713 f ));
DATA(insert (   25 1082  748 f ));
DATA(insert (   25 1083  837 f ));
DATA(insert (   25 1114 2022 f ));
DATA(insert (   25 1184 1191 f ));
DATA(insert (   25 1186 1263 f ));
DATA(insert (   25 1266  938 f ));
DATA(insert (   26   25  114 f ));
DATA(insert (  601  600 1532 f ));
DATA(insert (  602  600 1533 f ));
DATA(insert (  602  604 1449 f ));
DATA(insert (  603  600 1534 f ));
DATA(insert (  603  601 1541 f ));
DATA(insert (  603  604 1448 f ));
DATA(insert (  603  718 1479 f ));
DATA(insert (  604  600 1540 f ));
DATA(insert (  604  602 1447 f ));
DATA(insert (  604  603 1446 f ));
DATA(insert (  604  718 1474 f ));
DATA(insert (  700   21  238 f ));
DATA(insert (  700   23  319 f ));
DATA(insert (  700   25  841 t ));
DATA(insert (  700  701  311 t ));
DATA(insert (  700 1700 1742 t ));
DATA(insert (  701   20  483 f ));
DATA(insert (  701   21  237 f ));
DATA(insert (  701   23  317 f ));
DATA(insert (  701   25  840 t ));
DATA(insert (  701  700  312 t ));
DATA(insert (  701 1700 1743 t ));
DATA(insert (  702 1082 1179 f ));
DATA(insert (  702 1083 1364 f ));
DATA(insert (  702 1114 2023 t ));
DATA(insert (  702 1184 1173 t ));
DATA(insert (  703 1186 1177 t ));
DATA(insert (  718  600 1416 f ));
DATA(insert (  718  603 1480 f ));
DATA(insert (  718  604 1544 f ));
DATA(insert (  829   25  752 f ));
DATA(insert (  869   25  730 f ));
DATA(insert ( 1042   19  409 t ));
DATA(insert ( 1043   19 1400 t ));
DATA(insert ( 1082   25  749 t ));
DATA(insert ( 1082 1114 2024 t ));
DATA(insert ( 1082 1184 1174 t ));
DATA(insert ( 1083   25  948 t ));
DATA(insert ( 1083 1186 1370 t ));
DATA(insert ( 1083 1266 2047 t ));
DATA(insert ( 1114   25 2034 t ));
DATA(insert ( 1114  702 2030 f ));
DATA(insert ( 1114 1082 2029 f ));
DATA(insert ( 1114 1083 1316 f ));
DATA(insert ( 1114 1184 2028 t ));
DATA(insert ( 1184   25 1192 t ));
DATA(insert ( 1184  702 1180 f ));
DATA(insert ( 1184 1082 1178 f ));
DATA(insert ( 1184 1083 2019 f ));
DATA(insert ( 1184 1114 2027 t ));
DATA(insert ( 1184 1266 1388 f ));
DATA(insert ( 1186   25 1193 t ));
DATA(insert ( 1186  703 1194 f ));
DATA(insert ( 1186 1083 1419 f ));
DATA(insert ( 1266   25  939 t ));
DATA(insert ( 1266 1083 2046 t ));
DATA(insert ( 1700   20 1779 f ));
DATA(insert ( 1700   21 1783 f ));
DATA(insert ( 1700   23 1744 f ));
DATA(insert ( 1700  700 1745 f ));
DATA(insert ( 1700  701 1746 f ));

#endif   /* PG_CAST_H */
