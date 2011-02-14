/* contrib/adminpack/adminpack--unpackaged--1.0.sql */

ALTER EXTENSION adminpack ADD function pg_catalog.pg_file_write(text,text,boolean);
ALTER EXTENSION adminpack ADD function pg_catalog.pg_file_rename(text,text,text);
ALTER EXTENSION adminpack ADD function pg_catalog.pg_file_rename(text,text);
ALTER EXTENSION adminpack ADD function pg_catalog.pg_file_unlink(text);
ALTER EXTENSION adminpack ADD function pg_catalog.pg_logdir_ls();
ALTER EXTENSION adminpack ADD function pg_catalog.pg_file_read(text,bigint,bigint);
ALTER EXTENSION adminpack ADD function pg_catalog.pg_file_length(text);
ALTER EXTENSION adminpack ADD function pg_catalog.pg_logfile_rotate();
