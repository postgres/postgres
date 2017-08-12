CREATE EXTENSION adminpack;

-- create new file
SELECT pg_file_write('test_file1', 'test1', false);
SELECT pg_read_file('test_file1');

-- append
SELECT pg_file_write('test_file1', 'test1', true);
SELECT pg_read_file('test_file1');

-- error, already exists
SELECT pg_file_write('test_file1', 'test1', false);
SELECT pg_read_file('test_file1');

-- disallowed file paths
SELECT pg_file_write('../test_file0', 'test0', false);
SELECT pg_file_write('/tmp/test_file0', 'test0', false);
SELECT pg_file_write(current_setting('data_directory') || '/test_file4', 'test4', false);
SELECT pg_file_write(current_setting('data_directory') || '/../test_file4', 'test4', false);


-- rename file
SELECT pg_file_rename('test_file1', 'test_file2');
SELECT pg_read_file('test_file1');  -- not there
SELECT pg_read_file('test_file2');

-- error
SELECT pg_file_rename('test_file1', 'test_file2');

-- rename file and archive
SELECT pg_file_write('test_file3', 'test3', false);
SELECT pg_file_rename('test_file2', 'test_file3', 'test_file3_archive');
SELECT pg_read_file('test_file2');  -- not there
SELECT pg_read_file('test_file3');
SELECT pg_read_file('test_file3_archive');


-- unlink
SELECT pg_file_unlink('test_file1');  -- does not exist
SELECT pg_file_unlink('test_file2');  -- does not exist
SELECT pg_file_unlink('test_file3');
SELECT pg_file_unlink('test_file3_archive');
SELECT pg_file_unlink('test_file4');


-- superuser checks
CREATE USER regress_user1;
SET ROLE regress_user1;

SELECT pg_file_write('test_file0', 'test0', false);
SELECT pg_file_rename('test_file0', 'test_file0');
SELECT pg_file_unlink('test_file0');
SELECT pg_logdir_ls();

RESET ROLE;
DROP USER regress_user1;


-- no further tests for pg_logdir_ls() because it depends on the
-- server's logging setup
