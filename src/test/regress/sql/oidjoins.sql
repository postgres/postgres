--
-- This is created by pgsql/contrib/findoidjoins/make_oidjoin_check
--
SELECT	ctid, pg_aggregate.aggtransfn 
FROM	pg_aggregate 
WHERE	pg_aggregate.aggtransfn != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_aggregate.aggtransfn);
SELECT	ctid, pg_aggregate.aggfinalfn 
FROM	pg_aggregate 
WHERE	pg_aggregate.aggfinalfn != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_aggregate.aggfinalfn);
SELECT	ctid, pg_aggregate.aggbasetype 
FROM	pg_aggregate 
WHERE	pg_aggregate.aggbasetype != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_aggregate.aggbasetype);
SELECT	ctid, pg_aggregate.aggtranstype 
FROM	pg_aggregate 
WHERE	pg_aggregate.aggtranstype != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_aggregate.aggtranstype);
SELECT	ctid, pg_aggregate.aggfinaltype 
FROM	pg_aggregate 
WHERE	pg_aggregate.aggfinaltype != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_aggregate.aggfinaltype);
SELECT	ctid, pg_am.amgettuple 
FROM	pg_am 
WHERE	pg_am.amgettuple != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amgettuple);
SELECT	ctid, pg_am.aminsert 
FROM	pg_am 
WHERE	pg_am.aminsert != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.aminsert);
SELECT	ctid, pg_am.ambeginscan 
FROM	pg_am 
WHERE	pg_am.ambeginscan != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.ambeginscan);
SELECT	ctid, pg_am.amrescan 
FROM	pg_am 
WHERE	pg_am.amrescan != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amrescan);
SELECT	ctid, pg_am.amendscan 
FROM	pg_am 
WHERE	pg_am.amendscan != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amendscan);
SELECT	ctid, pg_am.ammarkpos 
FROM	pg_am 
WHERE	pg_am.ammarkpos != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.ammarkpos);
SELECT	ctid, pg_am.amrestrpos 
FROM	pg_am 
WHERE	pg_am.amrestrpos != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amrestrpos);
SELECT	ctid, pg_am.ambuild 
FROM	pg_am 
WHERE	pg_am.ambuild != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.ambuild);
SELECT	ctid, pg_am.ambulkdelete 
FROM	pg_am 
WHERE	pg_am.ambulkdelete != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.ambulkdelete);
SELECT	ctid, pg_am.amcostestimate 
FROM	pg_am 
WHERE	pg_am.amcostestimate != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amcostestimate);
SELECT	ctid, pg_amop.amopclaid 
FROM	pg_amop 
WHERE	pg_amop.amopclaid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_opclass AS t1 WHERE t1.oid = pg_amop.amopclaid);
SELECT	ctid, pg_amop.amopopr 
FROM	pg_amop 
WHERE	pg_amop.amopopr != 0 AND 
	NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_amop.amopopr);
SELECT	ctid, pg_amproc.amopclaid 
FROM	pg_amproc 
WHERE	pg_amproc.amopclaid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_opclass AS t1 WHERE t1.oid = pg_amproc.amopclaid);
SELECT	ctid, pg_amproc.amproc 
FROM	pg_amproc 
WHERE	pg_amproc.amproc != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_amproc.amproc);
SELECT	ctid, pg_attribute.attrelid 
FROM	pg_attribute 
WHERE	pg_attribute.attrelid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_attribute.attrelid);
SELECT	ctid, pg_attribute.atttypid 
FROM	pg_attribute 
WHERE	pg_attribute.atttypid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_attribute.atttypid);
SELECT	ctid, pg_class.reltype 
FROM	pg_class 
WHERE	pg_class.reltype != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_class.reltype);
SELECT	ctid, pg_class.relam 
FROM	pg_class 
WHERE	pg_class.relam != 0 AND 
	NOT EXISTS(SELECT * FROM pg_am AS t1 WHERE t1.oid = pg_class.relam);
SELECT	ctid, pg_class.reltoastrelid 
FROM	pg_class 
WHERE	pg_class.reltoastrelid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_class.reltoastrelid);
SELECT	ctid, pg_class.reltoastidxid 
FROM	pg_class 
WHERE	pg_class.reltoastidxid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_class.reltoastidxid);
SELECT	ctid, pg_description.classoid 
FROM	pg_description 
WHERE	pg_description.classoid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_description.classoid);
SELECT	ctid, pg_index.indexrelid 
FROM	pg_index 
WHERE	pg_index.indexrelid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_index.indexrelid);
SELECT	ctid, pg_index.indrelid 
FROM	pg_index 
WHERE	pg_index.indrelid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_index.indrelid);
SELECT	ctid, pg_opclass.opcamid 
FROM	pg_opclass 
WHERE	pg_opclass.opcamid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_am AS t1 WHERE t1.oid = pg_opclass.opcamid);
SELECT	ctid, pg_opclass.opcintype 
FROM	pg_opclass 
WHERE	pg_opclass.opcintype != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_opclass.opcintype);
SELECT	ctid, pg_operator.oprleft 
FROM	pg_operator 
WHERE	pg_operator.oprleft != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_operator.oprleft);
SELECT	ctid, pg_operator.oprright 
FROM	pg_operator 
WHERE	pg_operator.oprright != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_operator.oprright);
SELECT	ctid, pg_operator.oprresult 
FROM	pg_operator 
WHERE	pg_operator.oprresult != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_operator.oprresult);
SELECT	ctid, pg_operator.oprcom 
FROM	pg_operator 
WHERE	pg_operator.oprcom != 0 AND 
	NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_operator.oprcom);
SELECT	ctid, pg_operator.oprnegate 
FROM	pg_operator 
WHERE	pg_operator.oprnegate != 0 AND 
	NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_operator.oprnegate);
SELECT	ctid, pg_operator.oprlsortop 
FROM	pg_operator 
WHERE	pg_operator.oprlsortop != 0 AND 
	NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_operator.oprlsortop);
SELECT	ctid, pg_operator.oprrsortop 
FROM	pg_operator 
WHERE	pg_operator.oprrsortop != 0 AND 
	NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_operator.oprrsortop);
SELECT	ctid, pg_operator.oprcode 
FROM	pg_operator 
WHERE	pg_operator.oprcode != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_operator.oprcode);
SELECT	ctid, pg_operator.oprrest 
FROM	pg_operator 
WHERE	pg_operator.oprrest != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_operator.oprrest);
SELECT	ctid, pg_operator.oprjoin 
FROM	pg_operator 
WHERE	pg_operator.oprjoin != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_operator.oprjoin);
SELECT	ctid, pg_proc.prolang 
FROM	pg_proc 
WHERE	pg_proc.prolang != 0 AND 
	NOT EXISTS(SELECT * FROM pg_language AS t1 WHERE t1.oid = pg_proc.prolang);
SELECT	ctid, pg_proc.prorettype 
FROM	pg_proc 
WHERE	pg_proc.prorettype != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_proc.prorettype);
SELECT	ctid, pg_rewrite.ev_class 
FROM	pg_rewrite 
WHERE	pg_rewrite.ev_class != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_rewrite.ev_class);
SELECT	ctid, pg_statistic.starelid 
FROM	pg_statistic 
WHERE	pg_statistic.starelid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_statistic.starelid);
SELECT	ctid, pg_statistic.staop1 
FROM	pg_statistic 
WHERE	pg_statistic.staop1 != 0 AND 
	NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_statistic.staop1);
SELECT	ctid, pg_statistic.staop2 
FROM	pg_statistic 
WHERE	pg_statistic.staop2 != 0 AND 
	NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_statistic.staop2);
SELECT	ctid, pg_statistic.staop3 
FROM	pg_statistic 
WHERE	pg_statistic.staop3 != 0 AND 
	NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_statistic.staop3);
SELECT	ctid, pg_trigger.tgrelid 
FROM	pg_trigger 
WHERE	pg_trigger.tgrelid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_trigger.tgrelid);
SELECT	ctid, pg_trigger.tgfoid 
FROM	pg_trigger 
WHERE	pg_trigger.tgfoid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_trigger.tgfoid);
SELECT	ctid, pg_type.typrelid 
FROM	pg_type 
WHERE	pg_type.typrelid != 0 AND 
	NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_type.typrelid);
SELECT	ctid, pg_type.typelem 
FROM	pg_type 
WHERE	pg_type.typelem != 0 AND 
	NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_type.typelem);
SELECT	ctid, pg_type.typinput 
FROM	pg_type 
WHERE	pg_type.typinput != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_type.typinput);
SELECT	ctid, pg_type.typoutput 
FROM	pg_type 
WHERE	pg_type.typoutput != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_type.typoutput);
SELECT	ctid, pg_type.typreceive 
FROM	pg_type 
WHERE	pg_type.typreceive != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_type.typreceive);
SELECT	ctid, pg_type.typsend 
FROM	pg_type 
WHERE	pg_type.typsend != 0 AND 
	NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_type.typsend);
