-- first some tests of basic functionality
--
-- better succeed
--
select stupid();

-- check static and global data
--
SELECT static_test();
SELECT static_test();
SELECT global_test_one();
SELECT global_test_two();

-- import python modules
--
SELECT import_fail();
SELECT import_succeed();

-- test import and simple argument handling
--
SELECT import_test_one('sha hash of this string');

-- test import and tuple argument handling
--
select import_test_two(users) from users where fname = 'willem';

-- test multiple arguments
--
select argument_test_one(users, fname, lname) from users where lname = 'doe';


-- spi and nested calls
--
select nested_call_one('pass this along');
select spi_prepared_plan_test_one('doe');
select spi_prepared_plan_test_one('smith');
select spi_prepared_plan_test_nested('smith');

-- quick peek at the table
--
SELECT * FROM users;

-- should fail
--
UPDATE users SET fname = 'william' WHERE fname = 'willem';

-- should modify william to willem and create username
--
INSERT INTO users (fname, lname) VALUES ('william', 'smith');
INSERT INTO users (fname, lname, username) VALUES ('charles', 'darwin', 'beagle');

SELECT * FROM users;


SELECT join_sequences(sequences) FROM sequences;
SELECT join_sequences(sequences) FROM sequences
	WHERE join_sequences(sequences) ~* '^A';
SELECT join_sequences(sequences) FROM sequences
	WHERE join_sequences(sequences) ~* '^B';

-- error in trigger
--

