-- Without Pretty formatted

-- Create a specific role to test
CREATE ROLE test_database_ddl_role WITH SUPERUSER;
CREATE DATABASE "test_get_database_ddl"
    OWNER test_database_ddl_role ENCODING utf8 LC_COLLATE "C" LC_CTYPE "C" TEMPLATE template0;
SELECT pg_get_database_ddl('test_get_database_ddl', false);
DROP DATABASE "test_get_database_ddl";


-- Test LOCAL_PROVIDER and BUILTIN_LOCALE for builtin type
CREATE DATABASE "test_get_database_ddl_builtin"
    OWNER test_database_ddl_role TEMPLATE template0 ENCODING 'UTF8'
    BUILTIN_LOCALE 'C.UTF-8' LOCALE_PROVIDER 'builtin';
SELECT pg_get_database_ddl('test_get_database_ddl_builtin', false);
DROP DATABASE "test_get_database_ddl_builtin";


-- Test ALLOW_CONNECTION and CONNECTION_LIMIT
CREATE DATABASE "test_get_database_ddl_conn"
    OWNER test_database_ddl_role TEMPLATE template0 ENCODING 'UTF8'
    ALLOW_CONNECTIONS 0 CONNECTION LIMIT 50;
SELECT pg_get_database_ddl('test_get_database_ddl_conn', false);
DROP DATABASE "test_get_database_ddl_conn";


-- With Pretty formatted
\pset format unaligned
CREATE DATABASE "test_get_database_ddl"
    OWNER test_database_ddl_role ENCODING utf8 LC_COLLATE "C" LC_CTYPE "C" TEMPLATE template0;
SELECT pg_get_database_ddl('test_get_database_ddl', true);
DROP DATABASE "test_get_database_ddl";


-- Test LOCAL_PROVIDER and BUILTIN_LOCALE for builtin type
CREATE DATABASE "test_get_database_ddl_builtin"
    OWNER test_database_ddl_role TEMPLATE template0 ENCODING 'UTF8'
    BUILTIN_LOCALE 'C.UTF-8' LOCALE_PROVIDER 'builtin';
SELECT pg_get_database_ddl('test_get_database_ddl_builtin', true);
DROP DATABASE "test_get_database_ddl_builtin";


-- Test ALLOW_CONNECTION and CONNECTION_LIMIT
CREATE DATABASE "test_get_database_ddl_conn"
    OWNER test_database_ddl_role TEMPLATE template0 ENCODING 'UTF8'
    ALLOW_CONNECTIONS 0 CONNECTION LIMIT 50;
SELECT pg_get_database_ddl('test_get_database_ddl_conn', true);
DROP DATABASE "test_get_database_ddl_conn";


-- Clean up
DROP ROLE test_database_ddl_role;
