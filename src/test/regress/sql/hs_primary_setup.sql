--
-- Hot Standby tests
--
-- hs_primary_setup.sql
--

drop table if exists hs1;
create table hs1 (col1 integer primary key);
insert into hs1 values (1);

drop table if exists hs2;
create table hs2 (col1 integer primary key);
insert into hs2 values (12);
insert into hs2 values (13);

drop table if exists hs3;
create table hs3 (col1 integer primary key);
insert into hs3 values (113);
insert into hs3 values (114);
insert into hs3 values (115);

DROP sequence if exists hsseq;
create sequence hsseq;

SELECT pg_switch_wal();
