--
-- This runs some common tests against the type
--
-- It's used just for development
--

-- ignore any errors here - simply drop the table if it already exists
DROP TABLE a;

-- create the test table
CREATE TABLE a (fname name,image lo);

-- insert a null object
INSERT INTO a VALUES ('null');

-- insert an empty large object
INSERT INTO a VALUES ('empty','');

-- insert a large object based on a file
INSERT INTO a VALUES ('/etc/group',lo_import('/etc/group')::lo);

-- now select the table
SELECT * FROM a;

-- this select also returns an oid based on the lo column
SELECT *,image::oid from a;

-- now test the trigger
CREATE TRIGGER t_a
BEFORE UPDATE OR DELETE ON a
FOR EACH ROW
EXECUTE PROCEDURE lo_manage(image);

-- insert
INSERT INTO a VALUES ('aa','');
SELECT * FROM a
WHERE fname LIKE 'aa%';

-- update
UPDATE a SET image=lo_import('/etc/group')::lo
WHERE fname='aa';
SELECT * FROM a
WHERE fname LIKE 'aa%';

-- update the 'empty' row which should be null
UPDATE a SET image=lo_import('/etc/hosts')::lo
WHERE fname='empty';
SELECT * FROM a
WHERE fname LIKE 'empty%';
UPDATE a SET image=null
WHERE fname='empty';
SELECT * FROM a
WHERE fname LIKE 'empty%';

-- delete the entry
DELETE FROM a
WHERE fname='aa';
SELECT * FROM a
WHERE fname LIKE 'aa%';

-- This deletes the table contents. Note, if you comment this out, and
-- expect the drop table to remove the objects, think again. The trigger
-- doesn't get thrown by drop table.
DELETE FROM a;

-- finally drop the table
DROP TABLE a;

-- end of tests
