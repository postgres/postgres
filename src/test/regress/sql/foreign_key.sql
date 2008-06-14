--
-- FOREIGN KEY
--

-- MATCH FULL
--
-- First test, check and cascade
--
CREATE TABLE PKTABLE ( ptest1 int PRIMARY KEY, ptest2 text );
CREATE TABLE FKTABLE ( ftest1 int REFERENCES PKTABLE MATCH FULL ON DELETE CASCADE ON UPDATE CASCADE, ftest2 int );

-- Insert test data into PKTABLE
INSERT INTO PKTABLE VALUES (1, 'Test1');
INSERT INTO PKTABLE VALUES (2, 'Test2');
INSERT INTO PKTABLE VALUES (3, 'Test3');
INSERT INTO PKTABLE VALUES (4, 'Test4');
INSERT INTO PKTABLE VALUES (5, 'Test5');

-- Insert successful rows into FK TABLE
INSERT INTO FKTABLE VALUES (1, 2);
INSERT INTO FKTABLE VALUES (2, 3);
INSERT INTO FKTABLE VALUES (3, 4);
INSERT INTO FKTABLE VALUES (NULL, 1);

-- Insert a failed row into FK TABLE
INSERT INTO FKTABLE VALUES (100, 2);

-- Check FKTABLE
SELECT * FROM FKTABLE;

-- Delete a row from PK TABLE
DELETE FROM PKTABLE WHERE ptest1=1;

-- Check FKTABLE for removal of matched row
SELECT * FROM FKTABLE;

-- Update a row from PK TABLE
UPDATE PKTABLE SET ptest1=1 WHERE ptest1=2;

-- Check FKTABLE for update of matched row
SELECT * FROM FKTABLE;

DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

--
-- check set NULL and table constraint on multiple columns
--
CREATE TABLE PKTABLE ( ptest1 int, ptest2 int, ptest3 text, PRIMARY KEY(ptest1, ptest2) );
CREATE TABLE FKTABLE ( ftest1 int, ftest2 int, ftest3 int, CONSTRAINT constrname FOREIGN KEY(ftest1, ftest2) 
                       REFERENCES PKTABLE MATCH FULL ON DELETE SET NULL ON UPDATE SET NULL);

-- Test comments
COMMENT ON CONSTRAINT constrname_wrong ON FKTABLE IS 'fk constraint comment';
COMMENT ON CONSTRAINT constrname ON FKTABLE IS 'fk constraint comment';
COMMENT ON CONSTRAINT constrname ON FKTABLE IS NULL;

-- Insert test data into PKTABLE
INSERT INTO PKTABLE VALUES (1, 2, 'Test1');
INSERT INTO PKTABLE VALUES (1, 3, 'Test1-2');
INSERT INTO PKTABLE VALUES (2, 4, 'Test2');
INSERT INTO PKTABLE VALUES (3, 6, 'Test3');
INSERT INTO PKTABLE VALUES (4, 8, 'Test4');
INSERT INTO PKTABLE VALUES (5, 10, 'Test5');

-- Insert successful rows into FK TABLE
INSERT INTO FKTABLE VALUES (1, 2, 4);
INSERT INTO FKTABLE VALUES (1, 3, 5);
INSERT INTO FKTABLE VALUES (2, 4, 8);
INSERT INTO FKTABLE VALUES (3, 6, 12);
INSERT INTO FKTABLE VALUES (NULL, NULL, 0);

-- Insert failed rows into FK TABLE
INSERT INTO FKTABLE VALUES (100, 2, 4);
INSERT INTO FKTABLE VALUES (2, 2, 4);
INSERT INTO FKTABLE VALUES (NULL, 2, 4);
INSERT INTO FKTABLE VALUES (1, NULL, 4);

-- Check FKTABLE
SELECT * FROM FKTABLE;

-- Delete a row from PK TABLE
DELETE FROM PKTABLE WHERE ptest1=1 and ptest2=2;

-- Check FKTABLE for removal of matched row
SELECT * FROM FKTABLE;

-- Delete another row from PK TABLE
DELETE FROM PKTABLE WHERE ptest1=5 and ptest2=10;

-- Check FKTABLE (should be no change)
SELECT * FROM FKTABLE;

-- Update a row from PK TABLE
UPDATE PKTABLE SET ptest1=1 WHERE ptest1=2;

-- Check FKTABLE for update of matched row
SELECT * FROM FKTABLE;

-- Try altering the column type where foreign keys are involved
ALTER TABLE PKTABLE ALTER COLUMN ptest1 TYPE bigint;
ALTER TABLE FKTABLE ALTER COLUMN ftest1 TYPE bigint;
SELECT * FROM PKTABLE;
SELECT * FROM FKTABLE;

DROP TABLE PKTABLE CASCADE;
DROP TABLE FKTABLE;

--
-- check set default and table constraint on multiple columns
--
CREATE TABLE PKTABLE ( ptest1 int, ptest2 int, ptest3 text, PRIMARY KEY(ptest1, ptest2) );
CREATE TABLE FKTABLE ( ftest1 int DEFAULT -1, ftest2 int DEFAULT -2, ftest3 int, CONSTRAINT constrname2 FOREIGN KEY(ftest1, ftest2) 
                       REFERENCES PKTABLE MATCH FULL ON DELETE SET DEFAULT ON UPDATE SET DEFAULT);

-- Insert a value in PKTABLE for default
INSERT INTO PKTABLE VALUES (-1, -2, 'The Default!');

-- Insert test data into PKTABLE
INSERT INTO PKTABLE VALUES (1, 2, 'Test1');
INSERT INTO PKTABLE VALUES (1, 3, 'Test1-2');
INSERT INTO PKTABLE VALUES (2, 4, 'Test2');
INSERT INTO PKTABLE VALUES (3, 6, 'Test3');
INSERT INTO PKTABLE VALUES (4, 8, 'Test4');
INSERT INTO PKTABLE VALUES (5, 10, 'Test5');

-- Insert successful rows into FK TABLE
INSERT INTO FKTABLE VALUES (1, 2, 4);
INSERT INTO FKTABLE VALUES (1, 3, 5);
INSERT INTO FKTABLE VALUES (2, 4, 8);
INSERT INTO FKTABLE VALUES (3, 6, 12);
INSERT INTO FKTABLE VALUES (NULL, NULL, 0);

-- Insert failed rows into FK TABLE
INSERT INTO FKTABLE VALUES (100, 2, 4);
INSERT INTO FKTABLE VALUES (2, 2, 4);
INSERT INTO FKTABLE VALUES (NULL, 2, 4);
INSERT INTO FKTABLE VALUES (1, NULL, 4);

-- Check FKTABLE
SELECT * FROM FKTABLE;

-- Delete a row from PK TABLE
DELETE FROM PKTABLE WHERE ptest1=1 and ptest2=2;

-- Check FKTABLE to check for removal
SELECT * FROM FKTABLE;

-- Delete another row from PK TABLE
DELETE FROM PKTABLE WHERE ptest1=5 and ptest2=10;

-- Check FKTABLE (should be no change)
SELECT * FROM FKTABLE;

-- Update a row from PK TABLE
UPDATE PKTABLE SET ptest1=1 WHERE ptest1=2;

-- Check FKTABLE for update of matched row
SELECT * FROM FKTABLE;

-- this should fail for lack of CASCADE
DROP TABLE PKTABLE;
DROP TABLE PKTABLE CASCADE;
DROP TABLE FKTABLE;


--
-- First test, check with no on delete or on update
--
CREATE TABLE PKTABLE ( ptest1 int PRIMARY KEY, ptest2 text );
CREATE TABLE FKTABLE ( ftest1 int REFERENCES PKTABLE MATCH FULL, ftest2 int );

-- Insert test data into PKTABLE
INSERT INTO PKTABLE VALUES (1, 'Test1');
INSERT INTO PKTABLE VALUES (2, 'Test2');
INSERT INTO PKTABLE VALUES (3, 'Test3');
INSERT INTO PKTABLE VALUES (4, 'Test4');
INSERT INTO PKTABLE VALUES (5, 'Test5');

-- Insert successful rows into FK TABLE
INSERT INTO FKTABLE VALUES (1, 2);
INSERT INTO FKTABLE VALUES (2, 3);
INSERT INTO FKTABLE VALUES (3, 4);
INSERT INTO FKTABLE VALUES (NULL, 1);

-- Insert a failed row into FK TABLE
INSERT INTO FKTABLE VALUES (100, 2);

-- Check FKTABLE
SELECT * FROM FKTABLE;

-- Check PKTABLE
SELECT * FROM PKTABLE;

-- Delete a row from PK TABLE (should fail)
DELETE FROM PKTABLE WHERE ptest1=1;

-- Delete a row from PK TABLE (should succeed)
DELETE FROM PKTABLE WHERE ptest1=5;

-- Check PKTABLE for deletes
SELECT * FROM PKTABLE;

-- Update a row from PK TABLE (should fail)
UPDATE PKTABLE SET ptest1=0 WHERE ptest1=2;

-- Update a row from PK TABLE (should succeed)
UPDATE PKTABLE SET ptest1=0 WHERE ptest1=4;

-- Check PKTABLE for updates
SELECT * FROM PKTABLE;

DROP TABLE FKTABLE;
DROP TABLE PKTABLE;


-- MATCH unspecified

-- Base test restricting update/delete
CREATE TABLE PKTABLE ( ptest1 int, ptest2 int, ptest3 int, ptest4 text, PRIMARY KEY(ptest1, ptest2, ptest3) );
CREATE TABLE FKTABLE ( ftest1 int, ftest2 int, ftest3 int, ftest4 int,  CONSTRAINT constrname3
			FOREIGN KEY(ftest1, ftest2, ftest3) REFERENCES PKTABLE);

-- Insert Primary Key values
INSERT INTO PKTABLE VALUES (1, 2, 3, 'test1');
INSERT INTO PKTABLE VALUES (1, 3, 3, 'test2');
INSERT INTO PKTABLE VALUES (2, 3, 4, 'test3');
INSERT INTO PKTABLE VALUES (2, 4, 5, 'test4');

-- Insert Foreign Key values
INSERT INTO FKTABLE VALUES (1, 2, 3, 1); 
INSERT INTO FKTABLE VALUES (NULL, 2, 3, 2);
INSERT INTO FKTABLE VALUES (2, NULL, 3, 3);
INSERT INTO FKTABLE VALUES (NULL, 2, 7, 4);
INSERT INTO FKTABLE VALUES (NULL, 3, 4, 5);

-- Insert a failed values
INSERT INTO FKTABLE VALUES (1, 2, 7, 6);

-- Show FKTABLE
SELECT * from FKTABLE;

-- Try to update something that should fail
UPDATE PKTABLE set ptest2=5 where ptest2=2;

-- Try to update something that should succeed
UPDATE PKTABLE set ptest1=1 WHERE ptest2=3;

-- Try to delete something that should fail
DELETE FROM PKTABLE where ptest1=1 and ptest2=2 and ptest3=3;

-- Try to delete something that should work
DELETE FROM PKTABLE where ptest1=2;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;

SELECT * from FKTABLE;

DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

-- cascade update/delete
CREATE TABLE PKTABLE ( ptest1 int, ptest2 int, ptest3 int, ptest4 text, PRIMARY KEY(ptest1, ptest2, ptest3) );
CREATE TABLE FKTABLE ( ftest1 int, ftest2 int, ftest3 int, ftest4 int,  CONSTRAINT constrname3
			FOREIGN KEY(ftest1, ftest2, ftest3) REFERENCES PKTABLE
			ON DELETE CASCADE ON UPDATE CASCADE);

-- Insert Primary Key values
INSERT INTO PKTABLE VALUES (1, 2, 3, 'test1');
INSERT INTO PKTABLE VALUES (1, 3, 3, 'test2');
INSERT INTO PKTABLE VALUES (2, 3, 4, 'test3');
INSERT INTO PKTABLE VALUES (2, 4, 5, 'test4');

-- Insert Foreign Key values
INSERT INTO FKTABLE VALUES (1, 2, 3, 1); 
INSERT INTO FKTABLE VALUES (NULL, 2, 3, 2);
INSERT INTO FKTABLE VALUES (2, NULL, 3, 3);
INSERT INTO FKTABLE VALUES (NULL, 2, 7, 4);
INSERT INTO FKTABLE VALUES (NULL, 3, 4, 5);

-- Insert a failed values
INSERT INTO FKTABLE VALUES (1, 2, 7, 6);

-- Show FKTABLE
SELECT * from FKTABLE;

-- Try to update something that will cascade
UPDATE PKTABLE set ptest2=5 where ptest2=2;

-- Try to update something that should not cascade
UPDATE PKTABLE set ptest1=1 WHERE ptest2=3;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

-- Try to delete something that should cascade
DELETE FROM PKTABLE where ptest1=1 and ptest2=5 and ptest3=3;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

-- Try to delete something that should not have a cascade
DELETE FROM PKTABLE where ptest1=2;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

-- set null update / set default delete
CREATE TABLE PKTABLE ( ptest1 int, ptest2 int, ptest3 int, ptest4 text, PRIMARY KEY(ptest1, ptest2, ptest3) );
CREATE TABLE FKTABLE ( ftest1 int DEFAULT 0, ftest2 int, ftest3 int, ftest4 int,  CONSTRAINT constrname3
			FOREIGN KEY(ftest1, ftest2, ftest3) REFERENCES PKTABLE
			ON DELETE SET DEFAULT ON UPDATE SET NULL);

-- Insert Primary Key values
INSERT INTO PKTABLE VALUES (1, 2, 3, 'test1');
INSERT INTO PKTABLE VALUES (1, 3, 3, 'test2');
INSERT INTO PKTABLE VALUES (2, 3, 4, 'test3');
INSERT INTO PKTABLE VALUES (2, 4, 5, 'test4');

-- Insert Foreign Key values
INSERT INTO FKTABLE VALUES (1, 2, 3, 1); 
INSERT INTO FKTABLE VALUES (2, 3, 4, 1); 
INSERT INTO FKTABLE VALUES (NULL, 2, 3, 2);
INSERT INTO FKTABLE VALUES (2, NULL, 3, 3);
INSERT INTO FKTABLE VALUES (NULL, 2, 7, 4);
INSERT INTO FKTABLE VALUES (NULL, 3, 4, 5);

-- Insert a failed values
INSERT INTO FKTABLE VALUES (1, 2, 7, 6);

-- Show FKTABLE
SELECT * from FKTABLE;

-- Try to update something that will set null
UPDATE PKTABLE set ptest2=5 where ptest2=2;

-- Try to update something that should not set null
UPDATE PKTABLE set ptest2=2 WHERE ptest2=3 and ptest1=1;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

-- Try to delete something that should set default
DELETE FROM PKTABLE where ptest1=2 and ptest2=3 and ptest3=4;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

-- Try to delete something that should not set default
DELETE FROM PKTABLE where ptest2=5;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

-- set default update / set null delete
CREATE TABLE PKTABLE ( ptest1 int, ptest2 int, ptest3 int, ptest4 text, PRIMARY KEY(ptest1, ptest2, ptest3) );
CREATE TABLE FKTABLE ( ftest1 int DEFAULT 0, ftest2 int DEFAULT -1, ftest3 int, ftest4 int,  CONSTRAINT constrname3
			FOREIGN KEY(ftest1, ftest2, ftest3) REFERENCES PKTABLE
			ON DELETE SET NULL ON UPDATE SET DEFAULT);

-- Insert Primary Key values
INSERT INTO PKTABLE VALUES (1, 2, 3, 'test1');
INSERT INTO PKTABLE VALUES (1, 3, 3, 'test2');
INSERT INTO PKTABLE VALUES (2, 3, 4, 'test3');
INSERT INTO PKTABLE VALUES (2, 4, 5, 'test4');
INSERT INTO PKTABLE VALUES (2, -1, 5, 'test5');

-- Insert Foreign Key values
INSERT INTO FKTABLE VALUES (1, 2, 3, 1); 
INSERT INTO FKTABLE VALUES (2, 3, 4, 1); 
INSERT INTO FKTABLE VALUES (2, 4, 5, 1);
INSERT INTO FKTABLE VALUES (NULL, 2, 3, 2);
INSERT INTO FKTABLE VALUES (2, NULL, 3, 3);
INSERT INTO FKTABLE VALUES (NULL, 2, 7, 4);
INSERT INTO FKTABLE VALUES (NULL, 3, 4, 5);

-- Insert a failed values
INSERT INTO FKTABLE VALUES (1, 2, 7, 6);

-- Show FKTABLE
SELECT * from FKTABLE;

-- Try to update something that will fail
UPDATE PKTABLE set ptest2=5 where ptest2=2;

-- Try to update something that will set default
UPDATE PKTABLE set ptest1=0, ptest2=5, ptest3=10 where ptest2=2;
UPDATE PKTABLE set ptest2=10 where ptest2=4;

-- Try to update something that should not set default
UPDATE PKTABLE set ptest2=2 WHERE ptest2=3 and ptest1=1;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

-- Try to delete something that should set null
DELETE FROM PKTABLE where ptest1=2 and ptest2=3 and ptest3=4;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

-- Try to delete something that should not set null
DELETE FROM PKTABLE where ptest2=5;

-- Show PKTABLE and FKTABLE
SELECT * from PKTABLE;
SELECT * from FKTABLE;

DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

CREATE TABLE PKTABLE (ptest1 int PRIMARY KEY);
CREATE TABLE FKTABLE_FAIL1 ( ftest1 int, CONSTRAINT fkfail1 FOREIGN KEY (ftest2) REFERENCES PKTABLE);
CREATE TABLE FKTABLE_FAIL2 ( ftest1 int, CONSTRAINT fkfail1 FOREIGN KEY (ftest1) REFERENCES PKTABLE(ptest2));

DROP TABLE FKTABLE_FAIL1;
DROP TABLE FKTABLE_FAIL2;
DROP TABLE PKTABLE;

-- Test for referencing column number smaller than referenced constraint
CREATE TABLE PKTABLE (ptest1 int, ptest2 int, UNIQUE(ptest1, ptest2));
CREATE TABLE FKTABLE_FAIL1 (ftest1 int REFERENCES pktable(ptest1));

DROP TABLE FKTABLE_FAIL1;
DROP TABLE PKTABLE;

--
-- Tests for mismatched types
--
-- Basic one column, two table setup 
CREATE TABLE PKTABLE (ptest1 int PRIMARY KEY);
INSERT INTO PKTABLE VALUES(42);
-- This next should fail, because int=inet does not exist
CREATE TABLE FKTABLE (ftest1 inet REFERENCES pktable);
-- This should also fail for the same reason, but here we
-- give the column name
CREATE TABLE FKTABLE (ftest1 inet REFERENCES pktable(ptest1));
-- This should succeed, even though they are different types,
-- because int=int8 exists and is a member of the integer opfamily
CREATE TABLE FKTABLE (ftest1 int8 REFERENCES pktable);
-- Check it actually works
INSERT INTO FKTABLE VALUES(42);		-- should succeed
INSERT INTO FKTABLE VALUES(43);		-- should fail
UPDATE FKTABLE SET ftest1 = ftest1;	-- should succeed
UPDATE FKTABLE SET ftest1 = ftest1 + 1;	-- should fail
DROP TABLE FKTABLE;
-- This should fail, because we'd have to cast numeric to int which is
-- not an implicit coercion (or use numeric=numeric, but that's not part
-- of the integer opfamily)
CREATE TABLE FKTABLE (ftest1 numeric REFERENCES pktable);
DROP TABLE PKTABLE;
-- On the other hand, this should work because int implicitly promotes to
-- numeric, and we allow promotion on the FK side
CREATE TABLE PKTABLE (ptest1 numeric PRIMARY KEY);
INSERT INTO PKTABLE VALUES(42);
CREATE TABLE FKTABLE (ftest1 int REFERENCES pktable);
-- Check it actually works
INSERT INTO FKTABLE VALUES(42);		-- should succeed
INSERT INTO FKTABLE VALUES(43);		-- should fail
UPDATE FKTABLE SET ftest1 = ftest1;	-- should succeed
UPDATE FKTABLE SET ftest1 = ftest1 + 1;	-- should fail
DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

-- Two columns, two tables
CREATE TABLE PKTABLE (ptest1 int, ptest2 inet, PRIMARY KEY(ptest1, ptest2));
-- This should fail, because we just chose really odd types
CREATE TABLE FKTABLE (ftest1 cidr, ftest2 timestamp, FOREIGN KEY(ftest1, ftest2) REFERENCES pktable);
-- Again, so should this...
CREATE TABLE FKTABLE (ftest1 cidr, ftest2 timestamp, FOREIGN KEY(ftest1, ftest2) REFERENCES pktable(ptest1, ptest2));
-- This fails because we mixed up the column ordering
CREATE TABLE FKTABLE (ftest1 int, ftest2 inet, FOREIGN KEY(ftest2, ftest1) REFERENCES pktable);
-- As does this...
CREATE TABLE FKTABLE (ftest1 int, ftest2 inet, FOREIGN KEY(ftest2, ftest1) REFERENCES pktable(ptest1, ptest2));
-- And again..
CREATE TABLE FKTABLE (ftest1 int, ftest2 inet, FOREIGN KEY(ftest1, ftest2) REFERENCES pktable(ptest2, ptest1));
-- This works...
CREATE TABLE FKTABLE (ftest1 int, ftest2 inet, FOREIGN KEY(ftest2, ftest1) REFERENCES pktable(ptest2, ptest1));
DROP TABLE FKTABLE;
-- As does this
CREATE TABLE FKTABLE (ftest1 int, ftest2 inet, FOREIGN KEY(ftest1, ftest2) REFERENCES pktable(ptest1, ptest2));
DROP TABLE FKTABLE;
DROP TABLE PKTABLE;

-- Two columns, same table
-- Make sure this still works...
CREATE TABLE PKTABLE (ptest1 int, ptest2 inet, ptest3 int, ptest4 inet, PRIMARY KEY(ptest1, ptest2), FOREIGN KEY(ptest3,
ptest4) REFERENCES pktable(ptest1, ptest2));
DROP TABLE PKTABLE;
-- And this, 
CREATE TABLE PKTABLE (ptest1 int, ptest2 inet, ptest3 int, ptest4 inet, PRIMARY KEY(ptest1, ptest2), FOREIGN KEY(ptest3,
ptest4) REFERENCES pktable);
DROP TABLE PKTABLE;
-- This shouldn't (mixed up columns)
CREATE TABLE PKTABLE (ptest1 int, ptest2 inet, ptest3 int, ptest4 inet, PRIMARY KEY(ptest1, ptest2), FOREIGN KEY(ptest3,
ptest4) REFERENCES pktable(ptest2, ptest1));
-- Nor should this... (same reason, we have 4,3 referencing 1,2 which mismatches types
CREATE TABLE PKTABLE (ptest1 int, ptest2 inet, ptest3 int, ptest4 inet, PRIMARY KEY(ptest1, ptest2), FOREIGN KEY(ptest4,
ptest3) REFERENCES pktable(ptest1, ptest2));
-- Not this one either... Same as the last one except we didn't defined the columns being referenced.
CREATE TABLE PKTABLE (ptest1 int, ptest2 inet, ptest3 int, ptest4 inet, PRIMARY KEY(ptest1, ptest2), FOREIGN KEY(ptest4,
ptest3) REFERENCES pktable);

--
-- Now some cases with inheritance
-- Basic 2 table case: 1 column of matching types.
create table pktable_base (base1 int not null);
create table pktable (ptest1 int, primary key(base1), unique(base1, ptest1)) inherits (pktable_base);
create table fktable (ftest1 int references pktable(base1));
-- now some ins, upd, del
insert into pktable(base1) values (1);
insert into pktable(base1) values (2);
--  let's insert a non-existant fktable value
insert into fktable(ftest1) values (3);
--  let's make a valid row for that
insert into pktable(base1) values (3);
insert into fktable(ftest1) values (3);
-- let's try removing a row that should fail from pktable
delete from pktable where base1>2;
-- okay, let's try updating all of the base1 values to *4
-- which should fail.
update pktable set base1=base1*4;
-- okay, let's try an update that should work.
update pktable set base1=base1*4 where base1<3;
-- and a delete that should work
delete from pktable where base1>3;
-- cleanup
drop table fktable;
delete from pktable;

-- Now 2 columns 2 tables, matching types
create table fktable (ftest1 int, ftest2 int, foreign key(ftest1, ftest2) references pktable(base1, ptest1));
-- now some ins, upd, del
insert into pktable(base1, ptest1) values (1, 1);
insert into pktable(base1, ptest1) values (2, 2);
--  let's insert a non-existant fktable value
insert into fktable(ftest1, ftest2) values (3, 1);
--  let's make a valid row for that
insert into pktable(base1,ptest1) values (3, 1);
insert into fktable(ftest1, ftest2) values (3, 1);
-- let's try removing a row that should fail from pktable
delete from pktable where base1>2;
-- okay, let's try updating all of the base1 values to *4
-- which should fail.
update pktable set base1=base1*4;
-- okay, let's try an update that should work.
update pktable set base1=base1*4 where base1<3;
-- and a delete that should work
delete from pktable where base1>3;
-- cleanup
drop table fktable;
drop table pktable;
drop table pktable_base;

-- Now we'll do one all in 1 table with 2 columns of matching types
create table pktable_base(base1 int not null, base2 int);
create table pktable(ptest1 int, ptest2 int, primary key(base1, ptest1), foreign key(base2, ptest2) references
                                             pktable(base1, ptest1)) inherits (pktable_base);
insert into pktable (base1, ptest1, base2, ptest2) values (1, 1, 1, 1);
insert into pktable (base1, ptest1, base2, ptest2) values (2, 1, 1, 1);
insert into pktable (base1, ptest1, base2, ptest2) values (2, 2, 2, 1);
insert into pktable (base1, ptest1, base2, ptest2) values (1, 3, 2, 2);
-- fails (3,2) isn't in base1, ptest1
insert into pktable (base1, ptest1, base2, ptest2) values (2, 3, 3, 2);
-- fails (2,2) is being referenced
delete from pktable where base1=2;
-- fails (1,1) is being referenced (twice)
update pktable set base1=3 where base1=1;
-- this sequence of two deletes will work, since after the first there will be no (2,*) references
delete from pktable where base2=2;
delete from pktable where base1=2;
drop table pktable;
drop table pktable_base;

-- 2 columns (2 tables), mismatched types
create table pktable_base(base1 int not null);
create table pktable(ptest1 inet, primary key(base1, ptest1)) inherits (pktable_base);
-- just generally bad types (with and without column references on the referenced table)
create table fktable(ftest1 cidr, ftest2 int[], foreign key (ftest1, ftest2) references pktable);
create table fktable(ftest1 cidr, ftest2 int[], foreign key (ftest1, ftest2) references pktable(base1, ptest1));
-- let's mix up which columns reference which
create table fktable(ftest1 int, ftest2 inet, foreign key(ftest2, ftest1) references pktable);
create table fktable(ftest1 int, ftest2 inet, foreign key(ftest2, ftest1) references pktable(base1, ptest1));
create table fktable(ftest1 int, ftest2 inet, foreign key(ftest1, ftest2) references pktable(ptest1, base1));
drop table pktable;
drop table pktable_base;

-- 2 columns (1 table), mismatched types
create table pktable_base(base1 int not null, base2 int);
create table pktable(ptest1 inet, ptest2 inet[], primary key(base1, ptest1), foreign key(base2, ptest2) references
                                             pktable(base1, ptest1)) inherits (pktable_base);
create table pktable(ptest1 inet, ptest2 inet, primary key(base1, ptest1), foreign key(base2, ptest2) references
                                             pktable(ptest1, base1)) inherits (pktable_base);
create table pktable(ptest1 inet, ptest2 inet, primary key(base1, ptest1), foreign key(ptest2, base2) references
                                             pktable(base1, ptest1)) inherits (pktable_base);
create table pktable(ptest1 inet, ptest2 inet, primary key(base1, ptest1), foreign key(ptest2, base2) references
                                             pktable(base1, ptest1)) inherits (pktable_base);
drop table pktable;
drop table pktable_base;

--
-- Deferrable constraints
--		(right now, only FOREIGN KEY constraints can be deferred)
--

-- deferrable, explicitly deferred
CREATE TABLE pktable (
	id		INT4 PRIMARY KEY,
	other	INT4
);

CREATE TABLE fktable (
	id		INT4 PRIMARY KEY,
	fk		INT4 REFERENCES pktable DEFERRABLE
);

-- default to immediate: should fail
INSERT INTO fktable VALUES (5, 10);

-- explicitly defer the constraint
BEGIN;

SET CONSTRAINTS ALL DEFERRED;

INSERT INTO fktable VALUES (10, 15);
INSERT INTO pktable VALUES (15, 0); -- make the FK insert valid

COMMIT;

DROP TABLE fktable, pktable;

-- deferrable, initially deferred
CREATE TABLE pktable (
	id		INT4 PRIMARY KEY,
	other	INT4
);

CREATE TABLE fktable (
	id		INT4 PRIMARY KEY,
	fk		INT4 REFERENCES pktable DEFERRABLE INITIALLY DEFERRED
);

-- default to deferred, should succeed
BEGIN;

INSERT INTO fktable VALUES (100, 200);
INSERT INTO pktable VALUES (200, 500); -- make the FK insert valid

COMMIT;

-- default to deferred, explicitly make immediate
BEGIN;

SET CONSTRAINTS ALL IMMEDIATE;

-- should fail
INSERT INTO fktable VALUES (500, 1000);

COMMIT;

DROP TABLE fktable, pktable;

-- tricky behavior: according to SQL99, if a deferred constraint is set
-- to 'immediate' mode, it should be checked for validity *immediately*,
-- not when the current transaction commits (i.e. the mode change applies
-- retroactively)
CREATE TABLE pktable (
	id		INT4 PRIMARY KEY,
	other	INT4
);

CREATE TABLE fktable (
	id		INT4 PRIMARY KEY,
	fk		INT4 REFERENCES pktable DEFERRABLE
);

BEGIN;

SET CONSTRAINTS ALL DEFERRED;

-- should succeed, for now
INSERT INTO fktable VALUES (1000, 2000);

-- should cause transaction abort, due to preceding error
SET CONSTRAINTS ALL IMMEDIATE;

INSERT INTO pktable VALUES (2000, 3); -- too late

COMMIT;

DROP TABLE fktable, pktable;

-- deferrable, initially deferred
CREATE TABLE pktable (
	id		INT4 PRIMARY KEY,
	other	INT4
);

CREATE TABLE fktable (
	id		INT4 PRIMARY KEY,
	fk		INT4 REFERENCES pktable DEFERRABLE INITIALLY DEFERRED
);

BEGIN;

-- no error here
INSERT INTO fktable VALUES (100, 200);

-- error here on commit
COMMIT;

DROP TABLE pktable, fktable;

-- test notice about expensive referential integrity checks,
-- where the index cannot be used because of type incompatibilities.

CREATE TEMP TABLE pktable (
        id1     INT4 PRIMARY KEY,
        id2     VARCHAR(4) UNIQUE,
        id3     REAL UNIQUE,
        UNIQUE(id1, id2, id3)
);

CREATE TEMP TABLE fktable (
        x1      INT4 REFERENCES pktable(id1),
        x2      VARCHAR(4) REFERENCES pktable(id2),
        x3      REAL REFERENCES pktable(id3),
        x4      TEXT,
        x5      INT2
);

-- check individual constraints with alter table.

-- should fail

-- varchar does not promote to real
ALTER TABLE fktable ADD CONSTRAINT fk_2_3
FOREIGN KEY (x2) REFERENCES pktable(id3);

-- nor to int4
ALTER TABLE fktable ADD CONSTRAINT fk_2_1
FOREIGN KEY (x2) REFERENCES pktable(id1);

-- real does not promote to int4
ALTER TABLE fktable ADD CONSTRAINT fk_3_1
FOREIGN KEY (x3) REFERENCES pktable(id1);

-- int4 does not promote to text
ALTER TABLE fktable ADD CONSTRAINT fk_1_2
FOREIGN KEY (x1) REFERENCES pktable(id2);

-- should succeed

-- int4 promotes to real
ALTER TABLE fktable ADD CONSTRAINT fk_1_3
FOREIGN KEY (x1) REFERENCES pktable(id3);

-- text is compatible with varchar
ALTER TABLE fktable ADD CONSTRAINT fk_4_2
FOREIGN KEY (x4) REFERENCES pktable(id2);

-- int2 is part of integer opfamily as of 8.0
ALTER TABLE fktable ADD CONSTRAINT fk_5_1
FOREIGN KEY (x5) REFERENCES pktable(id1);

-- check multikey cases, especially out-of-order column lists

-- these should work

ALTER TABLE fktable ADD CONSTRAINT fk_123_123
FOREIGN KEY (x1,x2,x3) REFERENCES pktable(id1,id2,id3);

ALTER TABLE fktable ADD CONSTRAINT fk_213_213
FOREIGN KEY (x2,x1,x3) REFERENCES pktable(id2,id1,id3);

ALTER TABLE fktable ADD CONSTRAINT fk_253_213
FOREIGN KEY (x2,x5,x3) REFERENCES pktable(id2,id1,id3);

-- these should fail

ALTER TABLE fktable ADD CONSTRAINT fk_123_231
FOREIGN KEY (x1,x2,x3) REFERENCES pktable(id2,id3,id1);

ALTER TABLE fktable ADD CONSTRAINT fk_241_132
FOREIGN KEY (x2,x4,x1) REFERENCES pktable(id1,id3,id2);

DROP TABLE pktable, fktable;

-- test a tricky case: we can elide firing the FK check trigger during
-- an UPDATE if the UPDATE did not change the foreign key
-- field. However, we can't do this if our transaction was the one that
-- created the updated row and the trigger is deferred, since our UPDATE
-- will have invalidated the original newly-inserted tuple, and therefore
-- cause the on-INSERT RI trigger not to be fired.

CREATE TEMP TABLE pktable (
    id int primary key,
    other int
);

CREATE TEMP TABLE fktable (
    id int primary key,
    fk int references pktable deferrable initially deferred
);

INSERT INTO pktable VALUES (5, 10);

BEGIN;

-- doesn't match PK, but no error yet
INSERT INTO fktable VALUES (0, 20);

-- don't change FK
UPDATE fktable SET id = id + 1;

-- should catch error from initial INSERT
COMMIT;

-- check same case when insert is in a different subtransaction than update

BEGIN;

-- doesn't match PK, but no error yet
INSERT INTO fktable VALUES (0, 20);

-- UPDATE will be in a subxact
SAVEPOINT savept1;

-- don't change FK
UPDATE fktable SET id = id + 1;

-- should catch error from initial INSERT
COMMIT;

BEGIN;

-- INSERT will be in a subxact
SAVEPOINT savept1;

-- doesn't match PK, but no error yet
INSERT INTO fktable VALUES (0, 20);

RELEASE SAVEPOINT savept1;

-- don't change FK
UPDATE fktable SET id = id + 1;

-- should catch error from initial INSERT
COMMIT;

BEGIN;

-- doesn't match PK, but no error yet
INSERT INTO fktable VALUES (0, 20);

-- UPDATE will be in a subxact
SAVEPOINT savept1;

-- don't change FK
UPDATE fktable SET id = id + 1;

-- Roll back the UPDATE
ROLLBACK TO savept1;

-- should catch error from initial INSERT
COMMIT;

-- test order of firing of FK triggers when several RI-induced changes need to
-- be made to the same row.  This was broken by subtransaction-related
-- changes in 8.0.

CREATE TEMP TABLE users (
  id INT PRIMARY KEY,
  name VARCHAR NOT NULL
);

INSERT INTO users VALUES (1, 'Jozko');
INSERT INTO users VALUES (2, 'Ferko');
INSERT INTO users VALUES (3, 'Samko');

CREATE TEMP TABLE tasks (
  id INT PRIMARY KEY,
  owner INT REFERENCES users ON UPDATE CASCADE ON DELETE SET NULL,
  worker INT REFERENCES users ON UPDATE CASCADE ON DELETE SET NULL,
  checked_by INT REFERENCES users ON UPDATE CASCADE ON DELETE SET NULL
);

INSERT INTO tasks VALUES (1,1,NULL,NULL);
INSERT INTO tasks VALUES (2,2,2,NULL);
INSERT INTO tasks VALUES (3,3,3,3);

SELECT * FROM tasks;

UPDATE users SET id = 4 WHERE id = 3;

SELECT * FROM tasks;

DELETE FROM users WHERE id = 4;

SELECT * FROM tasks;

-- could fail with only 2 changes to make, if row was already updated
BEGIN;
UPDATE tasks set id=id WHERE id=2;
SELECT * FROM tasks;
DELETE FROM users WHERE id = 2;
SELECT * FROM tasks;
COMMIT;
