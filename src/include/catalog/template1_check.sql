--
-- This is created by pgsql/contrib/findoidjoins/make_oidjoin_check
--
SELECT	oid, pg_aggregate.aggtransfn1 
FROM	pg_aggregate 
WHERE	RegprocToOid(pg_aggregate.aggtransfn1) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_aggregate.aggtransfn1) != 0;
SELECT	oid, pg_aggregate.aggtransfn2 
FROM	pg_aggregate 
WHERE	RegprocToOid(pg_aggregate.aggtransfn2) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_aggregate.aggtransfn2) != 0;
SELECT	oid, pg_aggregate.aggfinalfn 
FROM	pg_aggregate 
WHERE	RegprocToOid(pg_aggregate.aggfinalfn) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_aggregate.aggfinalfn) != 0;
SELECT	oid, pg_aggregate.aggtranstype2 
FROM	pg_aggregate 
WHERE	pg_aggregate.aggtranstype2 NOT IN (SELECT oid FROM pg_type) AND 
	pg_aggregate.aggtranstype2 != 0;
SELECT	oid, pg_am.amgettuple 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.amgettuple) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.amgettuple) != 0;
SELECT	oid, pg_am.aminsert 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.aminsert) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.aminsert) != 0;
SELECT	oid, pg_am.amdelete 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.amdelete) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.amdelete) != 0;
SELECT	oid, pg_am.ambeginscan 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.ambeginscan) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.ambeginscan) != 0;
SELECT	oid, pg_am.amrescan 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.amrescan) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.amrescan) != 0;
SELECT	oid, pg_am.amendscan 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.amendscan) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.amendscan) != 0;
SELECT	oid, pg_am.ammarkpos 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.ammarkpos) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.ammarkpos) != 0;
SELECT	oid, pg_am.amrestrpos 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.amrestrpos) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.amrestrpos) != 0;
SELECT	oid, pg_am.ambuild 
FROM	pg_am 
WHERE	RegprocToOid(pg_am.ambuild) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_am.ambuild) != 0;
SELECT	oid, pg_amop.amopid 
FROM	pg_amop 
WHERE	pg_amop.amopid NOT IN (SELECT oid FROM pg_am) AND 
	pg_amop.amopid != 0;
SELECT	oid, pg_amop.amopclaid 
FROM	pg_amop 
WHERE	pg_amop.amopclaid NOT IN (SELECT oid FROM pg_opclass) AND 
	pg_amop.amopclaid != 0;
SELECT	oid, pg_amop.amopselect 
FROM	pg_amop 
WHERE	RegprocToOid(pg_amop.amopselect) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_amop.amopselect) != 0;
SELECT	oid, pg_amop.amopnpages 
FROM	pg_amop 
WHERE	RegprocToOid(pg_amop.amopnpages) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_amop.amopnpages) != 0;
SELECT	oid, pg_amproc.amid 
FROM	pg_amproc 
WHERE	pg_amproc.amid NOT IN (SELECT oid FROM pg_am) AND 
	pg_amproc.amid != 0;
SELECT	oid, pg_attribute.attrelid 
FROM	pg_attribute 
WHERE	pg_attribute.attrelid NOT IN (SELECT oid FROM pg_class) AND 
	pg_attribute.attrelid != 0;
SELECT	oid, pg_attribute.atttypid 
FROM	pg_attribute 
WHERE	pg_attribute.atttypid NOT IN (SELECT oid FROM pg_type) AND 
	pg_attribute.atttypid != 0;
SELECT	oid, pg_class.reltype 
FROM	pg_class 
WHERE	pg_class.reltype NOT IN (SELECT oid FROM pg_type) AND 
	pg_class.reltype != 0;
SELECT	oid, pg_class.relam 
FROM	pg_class 
WHERE	pg_class.relam NOT IN (SELECT oid FROM pg_am) AND 
	pg_class.relam != 0;
SELECT	oid, pg_index.indexrelid 
FROM	pg_index 
WHERE	pg_index.indexrelid NOT IN (SELECT oid FROM pg_class) AND 
	pg_index.indexrelid != 0;
SELECT	oid, pg_index.indrelid 
FROM	pg_index 
WHERE	pg_index.indrelid NOT IN (SELECT oid FROM pg_class) AND 
	pg_index.indrelid != 0;
SELECT	oid, pg_opclass.opcdeftype 
FROM	pg_opclass 
WHERE	pg_opclass.opcdeftype NOT IN (SELECT oid FROM pg_type) AND 
	pg_opclass.opcdeftype != 0;
SELECT	oid, pg_operator.oprleft 
FROM	pg_operator 
WHERE	pg_operator.oprleft NOT IN (SELECT oid FROM pg_type) AND 
	pg_operator.oprleft != 0;
SELECT	oid, pg_operator.oprright 
FROM	pg_operator 
WHERE	pg_operator.oprright NOT IN (SELECT oid FROM pg_type) AND 
	pg_operator.oprright != 0;
SELECT	oid, pg_operator.oprresult 
FROM	pg_operator 
WHERE	pg_operator.oprresult NOT IN (SELECT oid FROM pg_type) AND 
	pg_operator.oprresult != 0;
SELECT	oid, pg_operator.oprcom 
FROM	pg_operator 
WHERE	pg_operator.oprcom NOT IN (SELECT oid FROM pg_operator) AND 
	pg_operator.oprcom != 0;
SELECT	oid, pg_operator.oprnegate 
FROM	pg_operator 
WHERE	pg_operator.oprnegate NOT IN (SELECT oid FROM pg_operator) AND 
	pg_operator.oprnegate != 0;
SELECT	oid, pg_operator.oprlsortop 
FROM	pg_operator 
WHERE	pg_operator.oprlsortop NOT IN (SELECT oid FROM pg_operator) AND 
	pg_operator.oprlsortop != 0;
SELECT	oid, pg_operator.oprrsortop 
FROM	pg_operator 
WHERE	pg_operator.oprrsortop NOT IN (SELECT oid FROM pg_operator) AND 
	pg_operator.oprrsortop != 0;
SELECT	oid, pg_operator.oprcode 
FROM	pg_operator 
WHERE	RegprocToOid(pg_operator.oprcode) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_operator.oprcode) != 0;
SELECT	oid, pg_operator.oprrest 
FROM	pg_operator 
WHERE	RegprocToOid(pg_operator.oprrest) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_operator.oprrest) != 0;
SELECT	oid, pg_operator.oprjoin 
FROM	pg_operator 
WHERE	RegprocToOid(pg_operator.oprjoin) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_operator.oprjoin) != 0;
SELECT	oid, pg_parg.partype 
FROM	pg_parg 
WHERE	pg_parg.partype NOT IN (SELECT oid FROM pg_type) AND 
	pg_parg.partype != 0;
SELECT	oid, pg_proc.prolang 
FROM	pg_proc 
WHERE	pg_proc.prolang NOT IN (SELECT oid FROM pg_language) AND 
	pg_proc.prolang != 0;
SELECT	oid, pg_proc.prorettype 
FROM	pg_proc 
WHERE	pg_proc.prorettype NOT IN (SELECT oid FROM pg_type) AND 
	pg_proc.prorettype != 0;
SELECT	oid, pg_rewrite.ev_class 
FROM	pg_rewrite 
WHERE	pg_rewrite.ev_class NOT IN (SELECT oid FROM pg_class) AND 
	pg_rewrite.ev_class != 0;
SELECT	oid, pg_type.typrelid 
FROM	pg_type 
WHERE	pg_type.typrelid NOT IN (SELECT oid FROM pg_class) AND 
	pg_type.typrelid != 0;
SELECT	oid, pg_type.typinput 
FROM	pg_type 
WHERE	RegprocToOid(pg_type.typinput) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_type.typinput) != 0;
SELECT	oid, pg_type.typoutput 
FROM	pg_type 
WHERE	RegprocToOid(pg_type.typoutput) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_type.typoutput) != 0;
SELECT	oid, pg_type.typreceive 
FROM	pg_type 
WHERE	RegprocToOid(pg_type.typreceive) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_type.typreceive) != 0;
SELECT	oid, pg_type.typsend 
FROM	pg_type 
WHERE	RegprocToOid(pg_type.typsend) NOT IN (SELECT oid FROM pg_proc) AND 
	RegprocToOid(pg_type.typsend) != 0;
