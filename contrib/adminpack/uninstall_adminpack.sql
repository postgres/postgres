/* $PostgreSQL: pgsql/contrib/adminpack/uninstall_adminpack.sql,v 1.4 2007/11/13 04:24:27 momjian Exp $ */

DROP FUNCTION pg_catalog.pg_file_write(text, text, bool) ;
DROP FUNCTION pg_catalog.pg_file_rename(text, text, text) ;
DROP FUNCTION pg_catalog.pg_file_rename(text, text) ;
DROP FUNCTION pg_catalog.pg_file_unlink(text) ;
DROP FUNCTION pg_catalog.pg_logdir_ls() ;
DROP FUNCTION pg_catalog.pg_file_read(text, bigint, bigint) ;
DROP FUNCTION pg_catalog.pg_file_length(text) ;
DROP FUNCTION pg_catalog.pg_logfile_rotate() ;
