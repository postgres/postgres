--
-- Tests for password verifiers
--

-- Tests for GUC password_encryption
SET password_encryption = 'novalue'; -- error
SET password_encryption = true; -- ok
SET password_encryption = 'md5'; -- ok
SET password_encryption = 'plain'; -- ok
SET password_encryption = 'scram-sha-256'; -- ok

-- consistency of password entries
SET password_encryption = 'plain';
CREATE ROLE regress_passwd1 PASSWORD 'role_pwd1';
SET password_encryption = 'md5';
CREATE ROLE regress_passwd2 PASSWORD 'role_pwd2';
SET password_encryption = 'on';
CREATE ROLE regress_passwd3 PASSWORD 'role_pwd3';
SET password_encryption = 'scram-sha-256';
CREATE ROLE regress_passwd4 PASSWORD 'role_pwd4';
SET password_encryption = 'plain';
CREATE ROLE regress_passwd5 PASSWORD NULL;

-- check list of created entries
--
-- The scram verifier will look something like:
-- SCRAM-SHA-256$4096:E4HxLGtnRzsYwg==$6YtlR4t69SguDiwFvbVgVZtuz6gpJQQqUMZ7IQJK5yI=:ps75jrHeYU4lXCcXI4O8oIdJ3eO8o2jirjruw9phBTo=
--
-- Since the salt is random, the exact value stored will be different on every test
-- run. Use a regular expression to mask the changing parts.
SELECT rolname, regexp_replace(rolpassword, '(SCRAM-SHA-256)\$(\d+):([a-zA-Z0-9+/=]+)\$([a-zA-Z0-9+=/]+):([a-zA-Z0-9+/=]+)', '\1$\2:<salt>$<storedkey>:<serverkey>') as rolpassword_masked
    FROM pg_authid
    WHERE rolname LIKE 'regress_passwd%'
    ORDER BY rolname, rolpassword;

-- Rename a role
ALTER ROLE regress_passwd3 RENAME TO regress_passwd3_new;
-- md5 entry should have been removed
SELECT rolname, rolpassword
    FROM pg_authid
    WHERE rolname LIKE 'regress_passwd3_new'
    ORDER BY rolname, rolpassword;
ALTER ROLE regress_passwd3_new RENAME TO regress_passwd3;

-- ENCRYPTED and UNENCRYPTED passwords
ALTER ROLE regress_passwd1 UNENCRYPTED PASSWORD 'foo'; -- unencrypted
ALTER ROLE regress_passwd2 UNENCRYPTED PASSWORD 'md5dfa155cadd5f4ad57860162f3fab9cdb'; -- encrypted with MD5
SET password_encryption = 'md5';
ALTER ROLE regress_passwd3 ENCRYPTED PASSWORD 'foo'; -- encrypted with MD5

ALTER ROLE regress_passwd4 ENCRYPTED PASSWORD 'SCRAM-SHA-256$4096:VLK4RMaQLCvNtQ==$6YtlR4t69SguDiwFvbVgVZtuz6gpJQQqUMZ7IQJK5yI=:ps75jrHeYU4lXCcXI4O8oIdJ3eO8o2jirjruw9phBTo='; -- client-supplied SCRAM verifier, use as it is

SET password_encryption = 'scram-sha-256';
ALTER ROLE  regress_passwd5 ENCRYPTED PASSWORD 'foo'; -- create SCRAM verifier
CREATE ROLE regress_passwd6 ENCRYPTED PASSWORD 'md53725413363ab045e20521bf36b8d8d7f'; -- encrypted with MD5, use as it is

SELECT rolname, regexp_replace(rolpassword, '(SCRAM-SHA-256)\$(\d+):([a-zA-Z0-9+/=]+)\$([a-zA-Z0-9+=/]+):([a-zA-Z0-9+/=]+)', '\1$\2:<salt>$<storedkey>:<serverkey>') as rolpassword_masked
    FROM pg_authid
    WHERE rolname LIKE 'regress_passwd%'
    ORDER BY rolname, rolpassword;

DROP ROLE regress_passwd1;
DROP ROLE regress_passwd2;
DROP ROLE regress_passwd3;
DROP ROLE regress_passwd4;
DROP ROLE regress_passwd5;
DROP ROLE regress_passwd6;

-- all entries should have been removed
SELECT rolname, rolpassword
    FROM pg_authid
    WHERE rolname LIKE 'regress_passwd%'
    ORDER BY rolname, rolpassword;
