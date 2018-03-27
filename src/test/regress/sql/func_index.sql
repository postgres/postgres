create table keyvalue(id integer primary key, info jsonb);
create index nameindex on keyvalue((info->>'name')) with (recheck_on_update=false);
insert into keyvalue values (1, '{"name": "john", "data": "some data"}');
update keyvalue set info='{"name": "john", "data": "some other data"}' where id=1;
select pg_stat_get_xact_tuples_hot_updated('keyvalue'::regclass);
drop table keyvalue;

create table keyvalue(id integer primary key, info jsonb);
create index nameindex on keyvalue((info->>'name')) with (recheck_on_update=true);
insert into keyvalue values (1, '{"name": "john", "data": "some data"}');
update keyvalue set info='{"name": "john", "data": "some other data"}' where id=1;
select pg_stat_get_xact_tuples_hot_updated('keyvalue'::regclass);
update keyvalue set info='{"name": "smith", "data": "some other data"}' where id=1;
select pg_stat_get_xact_tuples_hot_updated('keyvalue'::regclass);
update keyvalue set info='{"name": "smith", "data": "some more data"}' where id=1;
select pg_stat_get_xact_tuples_hot_updated('keyvalue'::regclass);
drop table keyvalue;

create table keyvalue(id integer primary key, info jsonb);
create index nameindex on keyvalue((info->>'name'));
insert into keyvalue values (1, '{"name": "john", "data": "some data"}');
update keyvalue set info='{"name": "john", "data": "some other data"}' where id=1;
select pg_stat_get_xact_tuples_hot_updated('keyvalue'::regclass);
update keyvalue set info='{"name": "smith", "data": "some other data"}' where id=1;
select pg_stat_get_xact_tuples_hot_updated('keyvalue'::regclass);
update keyvalue set info='{"name": "smith", "data": "some more data"}' where id=1;
select pg_stat_get_xact_tuples_hot_updated('keyvalue'::regclass);
drop table keyvalue;


