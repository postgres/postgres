--
-- This runs some common tests against the type
--
-- It's used just for development
--

-- ignore any errors here - simply drop the table if it already exists
drop table a;

-- create the test table
create table a (fname name,image lo);

-- insert a null object
insert into a values ('null');

-- insert an empty large object
insert into a values ('empty','');

-- insert a large object based on a file
insert into a values ('/etc/group',lo_import('/etc/group')::lo);

-- now select the table
select * from a;

-- this select also returns an oid based on the lo column
select *,image::oid from a;

-- now test the trigger
create trigger t_a before update or delete on a for each row execute procedure lo_manage(image);

-- insert
insert into a values ('aa','');
select * from a where fname like 'aa%';

-- update
update a set image=lo_import('/etc/group')::lo where fname='aa';
select * from a where fname like 'aa%';

-- update the 'empty' row which should be null
update a set image=lo_import('/etc/hosts')::lo where fname='empty';
select * from a where fname like 'empty%';
update a set image=null where fname='empty';
select * from a where fname like 'empty%';

-- delete the entry
delete from a where fname='aa';
select * from a where fname like 'aa%';

-- This deletes the table contents. Note, if you comment this out, and
-- expect the drop table to remove the objects, think again. The trigger
-- doesn't get thrown by drop table.
delete from a;

-- finally drop the table
drop table a;

-- end of tests
