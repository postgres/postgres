--
-- Test foreign-data wrapper and server management.
--

-- Clean up in case a prior regression run failed

-- Suppress NOTICE messages when roles don't exist
SET client_min_messages TO 'error';

DROP ROLE IF EXISTS foreign_data_user, regress_test_role, regress_test_role2, regress_test_role_super, regress_test_indirect, unpriviled_role;

RESET client_min_messages;

CREATE ROLE foreign_data_user LOGIN SUPERUSER;
SET SESSION AUTHORIZATION 'foreign_data_user';

CREATE ROLE regress_test_role;
CREATE ROLE regress_test_role2;
CREATE ROLE regress_test_role_super SUPERUSER;
CREATE ROLE regress_test_indirect;
CREATE ROLE unprivileged_role;

CREATE FOREIGN DATA WRAPPER dummy;
CREATE FOREIGN DATA WRAPPER postgresql VALIDATOR postgresql_fdw_validator;

-- At this point we should have 2 built-in wrappers and no servers.
SELECT fdwname, fdwvalidator::regproc, fdwoptions FROM pg_foreign_data_wrapper ORDER BY 1, 2, 3;
SELECT srvname, srvoptions FROM pg_foreign_server;
SELECT * FROM pg_user_mapping;

-- CREATE FOREIGN DATA WRAPPER
CREATE FOREIGN DATA WRAPPER foo VALIDATOR bar;            -- ERROR
CREATE FOREIGN DATA WRAPPER foo;
\dew

CREATE FOREIGN DATA WRAPPER foo; -- duplicate
DROP FOREIGN DATA WRAPPER foo;
CREATE FOREIGN DATA WRAPPER foo OPTIONS (testing '1');
\dew+

DROP FOREIGN DATA WRAPPER foo;
CREATE FOREIGN DATA WRAPPER foo OPTIONS (testing '1', testing '2');   -- ERROR
CREATE FOREIGN DATA WRAPPER foo OPTIONS (testing '1', another '2');
\dew+

DROP FOREIGN DATA WRAPPER foo;
SET ROLE regress_test_role;
CREATE FOREIGN DATA WRAPPER foo; -- ERROR
RESET ROLE;
CREATE FOREIGN DATA WRAPPER foo VALIDATOR postgresql_fdw_validator;
\dew+

-- ALTER FOREIGN DATA WRAPPER
ALTER FOREIGN DATA WRAPPER foo;                             -- ERROR
ALTER FOREIGN DATA WRAPPER foo VALIDATOR bar;               -- ERROR
ALTER FOREIGN DATA WRAPPER foo NO VALIDATOR;
\dew+

ALTER FOREIGN DATA WRAPPER foo OPTIONS (a '1', b '2');
ALTER FOREIGN DATA WRAPPER foo OPTIONS (SET c '4');         -- ERROR
ALTER FOREIGN DATA WRAPPER foo OPTIONS (DROP c);            -- ERROR
ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD x '1', DROP x);
\dew+

ALTER FOREIGN DATA WRAPPER foo OPTIONS (DROP a, SET b '3', ADD c '4');
\dew+

ALTER FOREIGN DATA WRAPPER foo OPTIONS (a '2');
ALTER FOREIGN DATA WRAPPER foo OPTIONS (b '4');             -- ERROR
\dew+

SET ROLE regress_test_role;
ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD d '5');         -- ERROR
SET ROLE regress_test_role_super;
ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD d '5');
\dew+

ALTER FOREIGN DATA WRAPPER foo OWNER TO regress_test_role;  -- ERROR
ALTER FOREIGN DATA WRAPPER foo OWNER TO regress_test_role_super;
ALTER ROLE regress_test_role_super NOSUPERUSER;
SET ROLE regress_test_role_super;
ALTER FOREIGN DATA WRAPPER foo OPTIONS (ADD e '6');         -- ERROR
RESET ROLE;
\dew+

-- DROP FOREIGN DATA WRAPPER
DROP FOREIGN DATA WRAPPER nonexistent;                      -- ERROR
DROP FOREIGN DATA WRAPPER IF EXISTS nonexistent;
\dew+

DROP ROLE regress_test_role_super;                          -- ERROR
SET ROLE regress_test_role_super;
DROP FOREIGN DATA WRAPPER foo;                              -- ERROR
RESET ROLE;
ALTER ROLE regress_test_role_super SUPERUSER;
DROP FOREIGN DATA WRAPPER foo;
DROP ROLE regress_test_role_super;
\dew+

CREATE FOREIGN DATA WRAPPER foo;
CREATE SERVER s1 FOREIGN DATA WRAPPER foo;
CREATE USER MAPPING FOR current_user SERVER s1;
\dew+
\des+
\deu+
DROP FOREIGN DATA WRAPPER foo;                              -- ERROR
SET ROLE regress_test_role;
DROP FOREIGN DATA WRAPPER foo CASCADE;                      -- ERROR
RESET ROLE;
DROP FOREIGN DATA WRAPPER foo CASCADE;
\dew+
\des+
\deu+

-- exercise CREATE SERVER
CREATE SERVER s1 FOREIGN DATA WRAPPER foo;                  -- ERROR
CREATE FOREIGN DATA WRAPPER foo OPTIONS (test_wrapper 'true');
CREATE SERVER s1 FOREIGN DATA WRAPPER foo;
CREATE SERVER s1 FOREIGN DATA WRAPPER foo;                  -- ERROR
CREATE SERVER s2 FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');
CREATE SERVER s3 TYPE 'oracle' FOREIGN DATA WRAPPER foo;
CREATE SERVER s4 TYPE 'oracle' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');
CREATE SERVER s5 VERSION '15.0' FOREIGN DATA WRAPPER foo;
CREATE SERVER s6 VERSION '16.0' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');
CREATE SERVER s7 TYPE 'oracle' VERSION '17.0' FOREIGN DATA WRAPPER foo OPTIONS (host 'a', dbname 'b');
CREATE SERVER s8 FOREIGN DATA WRAPPER postgresql OPTIONS (foo '1'); -- ERROR
CREATE SERVER s8 FOREIGN DATA WRAPPER postgresql OPTIONS (host 'localhost', dbname 's8db');
\des+
SET ROLE regress_test_role;
CREATE SERVER t1 FOREIGN DATA WRAPPER foo;                 -- ERROR: no usage on FDW
RESET ROLE;
GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;
SET ROLE regress_test_role;
CREATE SERVER t1 FOREIGN DATA WRAPPER foo;
RESET ROLE;
\des+

REVOKE USAGE ON FOREIGN DATA WRAPPER foo FROM regress_test_role;
GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_indirect;
SET ROLE regress_test_role;
CREATE SERVER t2 FOREIGN DATA WRAPPER foo;                 -- ERROR
RESET ROLE;
GRANT regress_test_indirect TO regress_test_role;
SET ROLE regress_test_role;
CREATE SERVER t2 FOREIGN DATA WRAPPER foo;
\des+
RESET ROLE;
REVOKE regress_test_indirect FROM regress_test_role;

-- ALTER SERVER
ALTER SERVER s0;                                            -- ERROR
ALTER SERVER s0 OPTIONS (a '1');                            -- ERROR
ALTER SERVER s1 VERSION '1.0' OPTIONS (servername 's1');
ALTER SERVER s2 VERSION '1.1';
ALTER SERVER s3 OPTIONS (tnsname 'orcl', port '1521');
GRANT USAGE ON FOREIGN SERVER s1 TO regress_test_role;
GRANT USAGE ON FOREIGN SERVER s6 TO regress_test_role2 WITH GRANT OPTION;
\des+
SET ROLE regress_test_role;
ALTER SERVER s1 VERSION '1.1';                              -- ERROR
ALTER SERVER s1 OWNER TO regress_test_role;                 -- ERROR
RESET ROLE;
ALTER SERVER s1 OWNER TO regress_test_role;
GRANT regress_test_role2 TO regress_test_role;
SET ROLE regress_test_role;
ALTER SERVER s1 VERSION '1.1';
ALTER SERVER s1 OWNER TO regress_test_role2;                -- ERROR
RESET ROLE;
ALTER SERVER s8 OPTIONS (foo '1');                          -- ERROR option validation
ALTER SERVER s8 OPTIONS (connect_timeout '30', SET dbname 'db1', DROP host);
SET ROLE regress_test_role;
ALTER SERVER s1 OWNER TO regress_test_indirect;             -- ERROR
RESET ROLE;
GRANT regress_test_indirect TO regress_test_role;
SET ROLE regress_test_role;
ALTER SERVER s1 OWNER TO regress_test_indirect;
RESET ROLE;
GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_indirect;
SET ROLE regress_test_role;
ALTER SERVER s1 OWNER TO regress_test_indirect;
RESET ROLE;
DROP ROLE regress_test_indirect;                            -- ERROR
\des+

-- DROP SERVER
DROP SERVER nonexistent;                                    -- ERROR
DROP SERVER IF EXISTS nonexistent;
\des
SET ROLE regress_test_role;
DROP SERVER s2;                                             -- ERROR
DROP SERVER s1;
RESET ROLE;
\des
ALTER SERVER s2 OWNER TO regress_test_role;
SET ROLE regress_test_role;
DROP SERVER s2;
RESET ROLE;
\des
CREATE USER MAPPING FOR current_user SERVER s3;
\deu
DROP SERVER s3;                                             -- ERROR
DROP SERVER s3 CASCADE;
\des
\deu

-- CREATE USER MAPPING
CREATE USER MAPPING FOR regress_test_missing_role SERVER s1;  -- ERROR
CREATE USER MAPPING FOR current_user SERVER s1;             -- ERROR
CREATE USER MAPPING FOR current_user SERVER s4;
CREATE USER MAPPING FOR user SERVER s4;                     -- ERROR duplicate
CREATE USER MAPPING FOR public SERVER s4 OPTIONS (mapping 'is public');
CREATE USER MAPPING FOR user SERVER s8 OPTIONS (username 'test', password 'secret');    -- ERROR
CREATE USER MAPPING FOR user SERVER s8 OPTIONS (user 'test', password 'secret');
ALTER SERVER s5 OWNER TO regress_test_role;
ALTER SERVER s6 OWNER TO regress_test_indirect;
SET ROLE regress_test_role;
CREATE USER MAPPING FOR current_user SERVER s5;
CREATE USER MAPPING FOR current_user SERVER s6 OPTIONS (username 'test');
CREATE USER MAPPING FOR current_user SERVER s7;             -- ERROR
CREATE USER MAPPING FOR public SERVER s8;                   -- ERROR
RESET ROLE;

ALTER SERVER t1 OWNER TO regress_test_indirect;
SET ROLE regress_test_role;
CREATE USER MAPPING FOR current_user SERVER t1 OPTIONS (username 'bob', password 'boo');
CREATE USER MAPPING FOR public SERVER t1;
RESET ROLE;
\deu

-- ALTER USER MAPPING
ALTER USER MAPPING FOR regress_test_missing_role SERVER s4 OPTIONS (gotcha 'true'); -- ERROR
ALTER USER MAPPING FOR user SERVER ss4 OPTIONS (gotcha 'true'); -- ERROR
ALTER USER MAPPING FOR public SERVER s5 OPTIONS (gotcha 'true');            -- ERROR
ALTER USER MAPPING FOR current_user SERVER s8 OPTIONS (username 'test');    -- ERROR
ALTER USER MAPPING FOR current_user SERVER s8 OPTIONS (DROP user, SET password 'public');
SET ROLE regress_test_role;
ALTER USER MAPPING FOR current_user SERVER s5 OPTIONS (ADD modified '1');
ALTER USER MAPPING FOR public SERVER s4 OPTIONS (ADD modified '1'); -- ERROR
ALTER USER MAPPING FOR public SERVER t1 OPTIONS (ADD modified '1');
RESET ROLE;
\deu+

-- DROP USER MAPPING
DROP USER MAPPING FOR regress_test_missing_role SERVER s4;  -- ERROR
DROP USER MAPPING FOR user SERVER ss4;
DROP USER MAPPING FOR public SERVER s7;                     -- ERROR
DROP USER MAPPING IF EXISTS FOR regress_test_missing_role SERVER s4;
DROP USER MAPPING IF EXISTS FOR user SERVER ss4;
DROP USER MAPPING IF EXISTS FOR public SERVER s7;
CREATE USER MAPPING FOR public SERVER s8;
SET ROLE regress_test_role;
DROP USER MAPPING FOR public SERVER s8;                     -- ERROR
RESET ROLE;
DROP SERVER s7;
\deu

-- Information schema

SELECT * FROM information_schema.foreign_data_wrappers ORDER BY 1, 2;
SELECT * FROM information_schema.foreign_data_wrapper_options ORDER BY 1, 2, 3;
SELECT * FROM information_schema.foreign_servers ORDER BY 1, 2;
SELECT * FROM information_schema.foreign_server_options ORDER BY 1, 2, 3;
SELECT * FROM information_schema.user_mappings ORDER BY lower(authorization_identifier), 2, 3;
SELECT * FROM information_schema.user_mapping_options ORDER BY lower(authorization_identifier), 2, 3, 4;
SELECT * FROM information_schema.usage_privileges WHERE object_type LIKE 'FOREIGN%' ORDER BY 1, 2, 3, 4, 5;
SELECT * FROM information_schema.role_usage_grants WHERE object_type LIKE 'FOREIGN%' ORDER BY 1, 2, 3, 4, 5;
SET ROLE regress_test_role;
SELECT * FROM information_schema.user_mapping_options ORDER BY 1, 2, 3, 4;
SELECT * FROM information_schema.usage_privileges WHERE object_type LIKE 'FOREIGN%' ORDER BY 1, 2, 3, 4, 5;
SELECT * FROM information_schema.role_usage_grants WHERE object_type LIKE 'FOREIGN%' ORDER BY 1, 2, 3, 4, 5;
DROP USER MAPPING FOR current_user SERVER t1;
SET ROLE regress_test_role2;
SELECT * FROM information_schema.user_mapping_options ORDER BY 1, 2, 3, 4;
RESET ROLE;


-- has_foreign_data_wrapper_privilege
SELECT has_foreign_data_wrapper_privilege('regress_test_role',
    (SELECT oid FROM pg_foreign_data_wrapper WHERE fdwname='foo'), 'USAGE');
SELECT has_foreign_data_wrapper_privilege('regress_test_role', 'foo', 'USAGE');
SELECT has_foreign_data_wrapper_privilege(
    (SELECT oid FROM pg_roles WHERE rolname='regress_test_role'),
    (SELECT oid FROM pg_foreign_data_wrapper WHERE fdwname='foo'), 'USAGE');
SELECT has_foreign_data_wrapper_privilege(
    (SELECT oid FROM pg_foreign_data_wrapper WHERE fdwname='foo'), 'USAGE');
SELECT has_foreign_data_wrapper_privilege(
    (SELECT oid FROM pg_roles WHERE rolname='regress_test_role'), 'foo', 'USAGE');
SELECT has_foreign_data_wrapper_privilege('foo', 'USAGE');
GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;
SELECT has_foreign_data_wrapper_privilege('regress_test_role', 'foo', 'USAGE');

-- has_server_privilege
SELECT has_server_privilege('regress_test_role',
    (SELECT oid FROM pg_foreign_server WHERE srvname='s8'), 'USAGE');
SELECT has_server_privilege('regress_test_role', 's8', 'USAGE');
SELECT has_server_privilege(
    (SELECT oid FROM pg_roles WHERE rolname='regress_test_role'),
    (SELECT oid FROM pg_foreign_server WHERE srvname='s8'), 'USAGE');
SELECT has_server_privilege(
    (SELECT oid FROM pg_foreign_server WHERE srvname='s8'), 'USAGE');
SELECT has_server_privilege(
    (SELECT oid FROM pg_roles WHERE rolname='regress_test_role'), 's8', 'USAGE');
SELECT has_server_privilege('s8', 'USAGE');
GRANT USAGE ON FOREIGN SERVER s8 TO regress_test_role;
SELECT has_server_privilege('regress_test_role', 's8', 'USAGE');
REVOKE USAGE ON FOREIGN SERVER s8 FROM regress_test_role;

GRANT USAGE ON FOREIGN SERVER s4 TO regress_test_role;
DROP USER MAPPING FOR public SERVER s4;
ALTER SERVER s6 OPTIONS (DROP host, DROP dbname);
ALTER USER MAPPING FOR regress_test_role SERVER s6 OPTIONS (DROP username);
ALTER FOREIGN DATA WRAPPER foo VALIDATOR postgresql_fdw_validator;

-- Privileges
SET ROLE unprivileged_role;
CREATE FOREIGN DATA WRAPPER foobar;                             -- ERROR
ALTER FOREIGN DATA WRAPPER foo OPTIONS (gotcha 'true');         -- ERROR
ALTER FOREIGN DATA WRAPPER foo OWNER TO unprivileged_role;      -- ERROR
DROP FOREIGN DATA WRAPPER foo;                                  -- ERROR
GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;   -- ERROR
CREATE SERVER s9 FOREIGN DATA WRAPPER foo;                      -- ERROR
ALTER SERVER s4 VERSION '0.5';                                  -- ERROR
ALTER SERVER s4 OWNER TO unprivileged_role;                     -- ERROR
DROP SERVER s4;                                                 -- ERROR
GRANT USAGE ON FOREIGN SERVER s4 TO regress_test_role;          -- ERROR
CREATE USER MAPPING FOR public SERVER s4;                       -- ERROR
ALTER USER MAPPING FOR regress_test_role SERVER s6 OPTIONS (gotcha 'true'); -- ERROR
DROP USER MAPPING FOR regress_test_role SERVER s6;              -- ERROR
RESET ROLE;

GRANT USAGE ON FOREIGN DATA WRAPPER postgresql TO unprivileged_role;
GRANT USAGE ON FOREIGN DATA WRAPPER foo TO unprivileged_role WITH GRANT OPTION;
SET ROLE unprivileged_role;
CREATE FOREIGN DATA WRAPPER foobar;                             -- ERROR
ALTER FOREIGN DATA WRAPPER foo OPTIONS (gotcha 'true');         -- ERROR
DROP FOREIGN DATA WRAPPER foo;                                  -- ERROR
GRANT USAGE ON FOREIGN DATA WRAPPER postgresql TO regress_test_role; -- WARNING
GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;
CREATE SERVER s9 FOREIGN DATA WRAPPER postgresql;
ALTER SERVER s6 VERSION '0.5';                                  -- ERROR
DROP SERVER s6;                                                 -- ERROR
GRANT USAGE ON FOREIGN SERVER s6 TO regress_test_role;          -- ERROR
GRANT USAGE ON FOREIGN SERVER s9 TO regress_test_role;
CREATE USER MAPPING FOR public SERVER s6;                       -- ERROR
CREATE USER MAPPING FOR public SERVER s9;
ALTER USER MAPPING FOR regress_test_role SERVER s6 OPTIONS (gotcha 'true'); -- ERROR
DROP USER MAPPING FOR regress_test_role SERVER s6;              -- ERROR
RESET ROLE;

REVOKE USAGE ON FOREIGN DATA WRAPPER foo FROM unprivileged_role; -- ERROR
REVOKE USAGE ON FOREIGN DATA WRAPPER foo FROM unprivileged_role CASCADE;
SET ROLE unprivileged_role;
GRANT USAGE ON FOREIGN DATA WRAPPER foo TO regress_test_role;   -- ERROR
CREATE SERVER s10 FOREIGN DATA WRAPPER foo;                     -- ERROR
ALTER SERVER s9 VERSION '1.1';
GRANT USAGE ON FOREIGN SERVER s9 TO regress_test_role;
CREATE USER MAPPING FOR current_user SERVER s9;
DROP SERVER s9 CASCADE;
RESET ROLE;
CREATE SERVER s9 FOREIGN DATA WRAPPER foo;
GRANT USAGE ON FOREIGN SERVER s9 TO unprivileged_role;
SET ROLE unprivileged_role;
ALTER SERVER s9 VERSION '1.2';                                  -- ERROR
GRANT USAGE ON FOREIGN SERVER s9 TO regress_test_role;          -- WARNING
CREATE USER MAPPING FOR current_user SERVER s9;
DROP SERVER s9 CASCADE;                                         -- ERROR
RESET ROLE;

-- Cleanup
DROP ROLE regress_test_role;                                -- ERROR
DROP SERVER s5 CASCADE;
DROP SERVER t1 CASCADE;
DROP SERVER t2;
DROP USER MAPPING FOR regress_test_role SERVER s6;
DROP FOREIGN DATA WRAPPER foo CASCADE;
DROP SERVER s8 CASCADE;
DROP ROLE regress_test_indirect;
DROP ROLE regress_test_role;
DROP ROLE unprivileged_role;                                -- ERROR
REVOKE ALL ON FOREIGN DATA WRAPPER postgresql FROM unprivileged_role;
DROP ROLE unprivileged_role;
DROP ROLE regress_test_role2;
DROP FOREIGN DATA WRAPPER postgresql CASCADE;
DROP FOREIGN DATA WRAPPER dummy CASCADE;
\c
DROP ROLE foreign_data_user;

-- At this point we should have no wrappers, no servers, and no mappings.
SELECT fdwname, fdwvalidator, fdwoptions FROM pg_foreign_data_wrapper;
SELECT srvname, srvoptions FROM pg_foreign_server;
SELECT * FROM pg_user_mapping;
