-- Make sure we can create an exclusion constraint
-- across a partitioned table.
-- That code looks at strategy numbers that can differ in regular gist vs btree_gist,
-- so we want to make sure it works here too.
create table parttmp (
  id int,
  valid_at daterange,
  exclude using gist (id with =, valid_at with &&)
) partition by range (id);

create table parttmp_1_to_10 partition of parttmp for values from (1) to (10);
create table parttmp_11_to_20 partition of parttmp for values from (11) to (20);

insert into parttmp (id, valid_at) values
  (1, '[2000-01-01, 2000-02-01)'),
  (1, '[2000-02-01, 2000-03-01)'),
  (2, '[2000-01-01, 2000-02-01)'),
  (11, '[2000-01-01, 2000-02-01)'),
  (11, '[2000-02-01, 2000-03-01)'),
  (12, '[2000-01-01, 2000-02-01)');

select * from parttmp order by id, valid_at;
select * from parttmp_1_to_10 order by id, valid_at;
select * from parttmp_11_to_20 order by id, valid_at;

update parttmp set valid_at = valid_at * '[2000-01-15,2000-02-15)' where id = 1;

select * from parttmp order by id, valid_at;
select * from parttmp_1_to_10 order by id, valid_at;
select * from parttmp_11_to_20 order by id, valid_at;

-- make sure the excluson constraint excludes:
insert into parttmp (id, valid_at) values
  (2, '[2000-01-15, 2000-02-01)');

drop table parttmp;

-- should fail with a good error message:
create table parttmp (id int, valid_at daterange, exclude using gist (id with <>, valid_at with &&)) partition by range (id);
