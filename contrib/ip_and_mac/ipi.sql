begin;
select pgam.oid from pg_am  pgam
where amname = 'btree';
--
-- Temporary oper table
--
-- drop table tmp_op;
create table tmp_op ( oprname name, opi int2);
--
-- Fill in this table
--
insert into tmp_op values('<','1');
insert into tmp_op values('<=','2');
insert into tmp_op values('=','3');
insert into tmp_op values('>=','4');
insert into tmp_op values('>','5');
select * from tmp_op;
--
-- Add record to the pg_opclass
--
DELETE FROM pg_opclass WHERE opcname = 'ipaddr_ops';
INSERT INTO pg_opclass (opcname,opcdeftype)
select 'ipaddr_ops',oid from pg_type where typname = 'ipaddr';
--
-- And determine oid
--
SELECT opc.oid,opc.opcname
 FROM pg_opclass opc
 WHERE opc.opcname = 'ipaddr_ops';
--
SELECT o.oid AS opoid, o.oprname
INTO TABLE ipaddr_tmp
FROM pg_operator o, pg_type t
WHERE o.oprleft = t.oid and o.oprright = t.oid and t.typname = 'ipaddr';
SELECT * FROM ipaddr_tmp;
--
INSERT INTO pg_amop (amopid, amopclaid, amopopr, amopstrategy, amopselect, amopnpages)
SELECT am.oid,opcl.oid,c.opoid,t.opi,'btreesel'::regproc, 'btreenpage'::regproc
FROM pg_am am, pg_opclass opcl, ipaddr_tmp c, tmp_op t
WHERE t.oprname = c.oprname
      and amname = 'btree'
      and opcname = 'ipaddr_ops';
--
SELECT prc.oid, prc.proname FROM pg_proc prc
WHERE prc.proname = 'ipaddr_cmp';
--
INSERT INTO pg_amproc (amid, amopclaid, amproc, amprocnum)
SELECT pgam.oid, opc.oid,prc.oid,'1'::int2
FROM  pg_am  pgam,
      pg_opclass opc,
      pg_proc prc
WHERE prc.proname = 'ipaddr_cmp'
 and  pgam.amname = 'btree'
 and  opc.opcname = 'ipaddr_ops';

drop table tmp_op;
DROP TABLE ipaddr_tmp;
COMMIT;

-- *****************************************************************
-- * Now you should test this by running   test1.sql and test2.sql *
-- * In test2, be sure the 'explain' operator show you             *
-- * search by index in the last line of output                    *
-- *****************************************************************
