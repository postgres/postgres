--
-- Test SP-GiST indexes.
--
-- There are other tests to test different SP-GiST opclasses. This is for
-- testing SP-GiST code itself.

create table spgist_point_tbl(id int4, p point);
create index spgist_point_idx on spgist_point_tbl using spgist(p);

-- Test vacuum-root operation. It gets invoked when the root is also a leaf,
-- i.e. the index is very small.
insert into spgist_point_tbl (id, p)
select g, point(g*10, g*10) from generate_series(1, 10) g;
delete from spgist_point_tbl where id < 5;
vacuum spgist_point_tbl;

-- Insert more data, to make the index a few levels deep.
insert into spgist_point_tbl (id, p)
select g,      point(g*10, g*10) from generate_series(1, 10000) g;
insert into spgist_point_tbl (id, p)
select g+100000, point(g*10+1, g*10+1) from generate_series(1, 10000) g;

-- To test vacuum, delete some entries from all over the index.
delete from spgist_point_tbl where id % 2 = 1;

-- And also delete some concentration of values. (SP-GiST doesn't currently
-- attempt to delete pages even when they become empty, but if it did, this
-- would exercise it)
delete from spgist_point_tbl where id < 10000;

vacuum spgist_point_tbl;


-- The point opclass's choose method only uses the spgMatchNode action,
-- so the other actions are not tested by the above. Create an index using
-- text opclass, which uses the others actions.

create table spgist_text_tbl(id int4, t text);
create index spgist_text_idx on spgist_text_tbl using spgist(t);

insert into spgist_text_tbl (id, t)
select g, 'f' || repeat('o', 100) || g from generate_series(1, 10000) g
union all
select g, 'baaaaaaaaaaaaaar' || g from generate_series(1, 1000) g;

-- Do a lot of insertions that have to split an existing node. Hopefully
-- one of these will cause the page to run out of space, causing the inner
-- tuple to be moved to another page.
insert into spgist_text_tbl (id, t)
select -g, 'f' || repeat('o', 100-g) || 'surprise' from generate_series(1, 100) g;
