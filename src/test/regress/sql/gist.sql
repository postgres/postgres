--
-- Test GiST indexes.
--
-- There are other tests to test different GiST opclasses. This is for
-- testing GiST code itself. Vacuuming in particular.

create table gist_point_tbl(id int4, p point);
create index gist_pointidx on gist_point_tbl using gist(p);

-- Insert enough data to create a tree that's a couple of levels deep.
insert into gist_point_tbl (id, p)
select g,        point(g*10, g*10) from generate_series(1, 10000) g;

insert into gist_point_tbl (id, p)
select g+100000, point(g*10+1, g*10+1) from generate_series(1, 10000) g;

-- To test vacuum, delete some entries from all over the index.
delete from gist_point_tbl where id % 2 = 1;

-- And also delete some concentration of values. (GiST doesn't currently
-- attempt to delete pages even when they become empty, but if it did, this
-- would exercise it)
delete from gist_point_tbl where id < 10000;

vacuum gist_point_tbl;
