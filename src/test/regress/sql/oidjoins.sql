--
-- OIDJOIN
-- This is created by pgsql/contrib/findoidjoins/make_oidjoin_check
--

SELECT oid, pg_aggregate.aggtransfn1 
  FROM pg_aggregate 
  WHERE pg_aggregate.aggtransfn1 != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_aggregate.aggtransfn1);
SELECT oid, pg_aggregate.aggtransfn2 
  FROM pg_aggregate 
  WHERE pg_aggregate.aggtransfn2 != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_aggregate.aggtransfn2);
SELECT oid, pg_aggregate.aggfinalfn 
  FROM pg_aggregate 
  WHERE pg_aggregate.aggfinalfn != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_aggregate.aggfinalfn);
SELECT oid, pg_aggregate.aggbasetype 
  FROM pg_aggregate 
  WHERE pg_aggregate.aggbasetype != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_aggregate.aggbasetype);
SELECT oid, pg_aggregate.aggtranstype1 
  FROM pg_aggregate 
  WHERE pg_aggregate.aggtranstype1 != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_aggregate.aggtranstype1);
SELECT oid, pg_aggregate.aggtranstype2 
  FROM pg_aggregate 
  WHERE pg_aggregate.aggtranstype2 != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_aggregate.aggtranstype2);
SELECT oid, pg_aggregate.aggfinaltype 
  FROM pg_aggregate 
  WHERE pg_aggregate.aggfinaltype != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_aggregate.aggfinaltype);
SELECT oid, pg_am.amgettuple 
  FROM pg_am 
  WHERE pg_am.amgettuple != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amgettuple);
SELECT oid, pg_am.aminsert 
  FROM pg_am 
  WHERE pg_am.aminsert != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.aminsert);
SELECT oid, pg_am.amdelete 
  FROM pg_am 
  WHERE pg_am.amdelete != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amdelete);
SELECT oid, pg_am.ambeginscan 
  FROM pg_am 
  WHERE pg_am.ambeginscan != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.ambeginscan);
SELECT oid, pg_am.amrescan 
  FROM pg_am 
  WHERE pg_am.amrescan != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amrescan);
SELECT oid, pg_am.amendscan 
  FROM pg_am 
  WHERE pg_am.amendscan != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amendscan);
SELECT oid, pg_am.ammarkpos 
  FROM pg_am 
  WHERE pg_am.ammarkpos != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.ammarkpos);
SELECT oid, pg_am.amrestrpos 
  FROM pg_am 
  WHERE pg_am.amrestrpos != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.amrestrpos);
SELECT oid, pg_am.ambuild 
  FROM pg_am 
  WHERE pg_am.ambuild != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_am.ambuild);
SELECT oid, pg_amop.amopid 
  FROM pg_amop 
  WHERE pg_amop.amopid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_am AS t1 WHERE t1.oid = pg_amop.amopid);
SELECT oid, pg_amop.amopclaid 
  FROM pg_amop 
  WHERE pg_amop.amopclaid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_opclass AS t1 WHERE t1.oid = pg_amop.amopclaid);
SELECT oid, pg_amop.amopopr 
  FROM pg_amop 
  WHERE pg_amop.amopopr != 0 AND 
    NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_amop.amopopr);
SELECT oid, pg_amop.amopselect 
  FROM pg_amop 
  WHERE pg_amop.amopselect != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_amop.amopselect);
SELECT oid, pg_amop.amopnpages 
  FROM pg_amop 
  WHERE pg_amop.amopnpages != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_amop.amopnpages);
SELECT oid, pg_amproc.amid 
  FROM pg_amproc 
  WHERE pg_amproc.amid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_am AS t1 WHERE t1.oid = pg_amproc.amid);
SELECT oid, pg_amproc.amopclaid 
  FROM pg_amproc 
  WHERE pg_amproc.amopclaid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_opclass AS t1 WHERE t1.oid = pg_amproc.amopclaid);
SELECT oid, pg_amproc.amproc 
  FROM pg_amproc 
  WHERE pg_amproc.amproc != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_amproc.amproc);
SELECT oid, pg_attribute.attrelid 
  FROM pg_attribute 
  WHERE pg_attribute.attrelid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_attribute.attrelid);
SELECT oid, pg_attribute.atttypid 
  FROM pg_attribute 
  WHERE pg_attribute.atttypid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_attribute.atttypid);
SELECT oid, pg_class.reltype 
  FROM pg_class 
  WHERE pg_class.reltype != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_class.reltype);
SELECT oid, pg_class.relam 
  FROM pg_class 
  WHERE pg_class.relam != 0 AND 
    NOT EXISTS(SELECT * FROM pg_am AS t1 WHERE t1.oid = pg_class.relam);
SELECT oid, pg_index.indexrelid 
  FROM pg_index 
  WHERE pg_index.indexrelid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_index.indexrelid);
SELECT oid, pg_index.indrelid 
  FROM pg_index 
  WHERE pg_index.indrelid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_index.indrelid);
SELECT oid, pg_opclass.opcdeftype 
  FROM pg_opclass 
  WHERE pg_opclass.opcdeftype != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_opclass.opcdeftype);
SELECT oid, pg_operator.oprleft 
  FROM pg_operator 
  WHERE pg_operator.oprleft != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_operator.oprleft);
SELECT oid, pg_operator.oprright 
  FROM pg_operator 
  WHERE pg_operator.oprright != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_operator.oprright);
SELECT oid, pg_operator.oprresult 
  FROM pg_operator 
  WHERE pg_operator.oprresult != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_operator.oprresult);
SELECT oid, pg_operator.oprcom 
  FROM pg_operator 
  WHERE pg_operator.oprcom != 0 AND 
    NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_operator.oprcom);
SELECT oid, pg_operator.oprnegate 
  FROM pg_operator 
  WHERE pg_operator.oprnegate != 0 AND 
    NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_operator.oprnegate);
SELECT oid, pg_operator.oprlsortop 
  FROM pg_operator 
  WHERE pg_operator.oprlsortop != 0 AND 
    NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_operator.oprlsortop);
SELECT oid, pg_operator.oprrsortop 
  FROM pg_operator 
  WHERE pg_operator.oprrsortop != 0 AND 
    NOT EXISTS(SELECT * FROM pg_operator AS t1 WHERE t1.oid = pg_operator.oprrsortop);
SELECT oid, pg_operator.oprcode 
  FROM pg_operator 
  WHERE pg_operator.oprcode != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_operator.oprcode);
SELECT oid, pg_operator.oprrest 
  FROM pg_operator 
  WHERE pg_operator.oprrest != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_operator.oprrest);
SELECT oid, pg_operator.oprjoin 
  FROM pg_operator 
  WHERE pg_operator.oprjoin != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_operator.oprjoin);
SELECT oid, pg_proc.prolang 
  FROM pg_proc 
  WHERE pg_proc.prolang != 0 AND 
    NOT EXISTS(SELECT * FROM pg_language AS t1 WHERE t1.oid = pg_proc.prolang);
SELECT oid, pg_proc.prorettype 
  FROM pg_proc 
  WHERE pg_proc.prorettype != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_proc.prorettype);
SELECT oid, pg_rewrite.ev_class 
  FROM pg_rewrite 
  WHERE pg_rewrite.ev_class != 0 AND 
    NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_rewrite.ev_class);
SELECT oid, pg_type.typrelid 
  FROM pg_type 
  WHERE pg_type.typrelid != 0 AND 
    NOT EXISTS(SELECT * FROM pg_class AS t1 WHERE t1.oid = pg_type.typrelid);
SELECT oid, pg_type.typelem 
  FROM pg_type 
  WHERE pg_type.typelem != 0 AND 
    NOT EXISTS(SELECT * FROM pg_type AS t1 WHERE t1.oid = pg_type.typelem);
SELECT oid, pg_type.typinput 
  FROM pg_type 
  WHERE pg_type.typinput != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_type.typinput);
SELECT oid, pg_type.typoutput 
  FROM pg_type 
  WHERE pg_type.typoutput != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_type.typoutput);
SELECT oid, pg_type.typreceive 
  FROM pg_type 
  WHERE pg_type.typreceive != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_type.typreceive);
SELECT oid, pg_type.typsend 
  FROM pg_type 
  WHERE pg_type.typsend != 0 AND 
    NOT EXISTS(SELECT * FROM pg_proc AS t1 WHERE t1.oid = pg_type.typsend);

