drop table stat_t;
create table stat_t(i1 ipaddr, i2 ipaddr, p int4, b int4);
create index stat_i1 on stat_t using btree (i1 ipaddr_ops);
create index stat_i2 on stat_t using btree (i2 ipaddr_ops);
copy stat_t from '/v/noc/src/ip_class/test.DATA' using delimiters ' ';
--
-- Please, check if your test data are not in /v/noc/src/ip_class/test.DATA file,
-- edit test1.sql file and repeatr it.
--
-- If everything is OK, you should run test2.sql now
--
