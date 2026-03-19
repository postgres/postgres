--
-- Tests for pg_get_tablespace_ddl()
--

SET allow_in_place_tablespaces = true;
CREATE ROLE regress_tblspc_ddl_user;

-- error: non-existent tablespace by name
SELECT * FROM pg_get_tablespace_ddl('regress_nonexistent_tblsp');

-- error: non-existent tablespace by OID
SELECT * FROM pg_get_tablespace_ddl(0::oid);

-- NULL input returns no rows (name variant)
SELECT * FROM pg_get_tablespace_ddl(NULL::name);

-- NULL input returns no rows (OID variant)
SELECT * FROM pg_get_tablespace_ddl(NULL::oid);

-- tablespace name requiring quoting
CREATE TABLESPACE "regress_ tblsp" OWNER regress_tblspc_ddl_user LOCATION '';
SELECT * FROM pg_get_tablespace_ddl('regress_ tblsp');
DROP TABLESPACE "regress_ tblsp";

-- tablespace with multiple options
CREATE TABLESPACE regress_allopt_tblsp OWNER regress_tblspc_ddl_user LOCATION ''
  WITH (seq_page_cost = '1.5', random_page_cost = '1.1234567890',
        effective_io_concurrency = '17', maintenance_io_concurrency = '18');
SELECT * FROM pg_get_tablespace_ddl('regress_allopt_tblsp');

-- pretty-printed output
\pset format unaligned
SELECT * FROM pg_get_tablespace_ddl('regress_allopt_tblsp', 'pretty', 'true');
\pset format aligned

-- tablespace with owner suppressed
SELECT * FROM pg_get_tablespace_ddl('regress_allopt_tblsp', 'owner', 'false');

DROP TABLESPACE regress_allopt_tblsp;

-- test by OID
CREATE TABLESPACE regress_oid_tblsp OWNER regress_tblspc_ddl_user LOCATION '';
SELECT oid AS tsid FROM pg_tablespace WHERE spcname = 'regress_oid_tblsp' \gset
SELECT * FROM pg_get_tablespace_ddl(:tsid);
DROP TABLESPACE regress_oid_tblsp;

-- Permission check: revoke SELECT on pg_tablespace
CREATE TABLESPACE regress_acl_tblsp OWNER regress_tblspc_ddl_user LOCATION '';
CREATE ROLE regress_tblspc_ddl_noaccess;
REVOKE SELECT ON pg_tablespace FROM PUBLIC;
SET ROLE regress_tblspc_ddl_noaccess;
SELECT * FROM pg_get_tablespace_ddl('regress_acl_tblsp');  -- should fail
RESET ROLE;
GRANT SELECT ON pg_tablespace TO PUBLIC;
DROP TABLESPACE regress_acl_tblsp;
DROP ROLE regress_tblspc_ddl_noaccess;

DROP ROLE regress_tblspc_ddl_user;
