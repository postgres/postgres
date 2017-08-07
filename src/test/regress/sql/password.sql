--
-- Tests for password verifiers
--

-- Tests for GUC password_encryption
SET password_encryption = 'novalue'; -- error
SET password_encryption = true; -- ok
SET password_encryption = 'md5'; -- ok
SET password_encryption = 'scram-sha-256'; -- ok

-- consistency of password entries
SET password_encryption = 'md5';
CREATE ROLE regress_passwd1 PASSWORD 'role_pwd1';
SET password_encryption = 'on';
CREATE ROLE regress_passwd2 PASSWORD 'role_pwd2';
SET password_encryption = 'scram-sha-256';
CREATE ROLE regress_passwd3 PASSWORD 'role_pwd3';
CREATE ROLE regress_passwd4 PASSWORD NULL;

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
ALTER ROLE regress_passwd2 RENAME TO regress_passwd2_new;
-- md5 entry should have been removed
SELECT rolname, rolpassword
    FROM pg_authid
    WHERE rolname LIKE 'regress_passwd2_new'
    ORDER BY rolname, rolpassword;
ALTER ROLE regress_passwd2_new RENAME TO regress_passwd2;

-- Change passwords with ALTER USER. With plaintext or already-encrypted
-- passwords.
SET password_encryption = 'md5';

-- encrypt with MD5
ALTER ROLE regress_passwd2 PASSWORD 'foo';
-- already encrypted, use as they are
ALTER ROLE regress_passwd1 PASSWORD 'md5cd3578025fe2c3d7ed1b9a9b26238b70';
ALTER ROLE regress_passwd3 PASSWORD 'SCRAM-SHA-256$4096:VLK4RMaQLCvNtQ==$6YtlR4t69SguDiwFvbVgVZtuz6gpJQQqUMZ7IQJK5yI=:ps75jrHeYU4lXCcXI4O8oIdJ3eO8o2jirjruw9phBTo=';

SET password_encryption = 'scram-sha-256';
-- create SCRAM verifier
ALTER ROLE  regress_passwd4 PASSWORD 'foo';
-- already encrypted with MD5, use as it is
CREATE ROLE regress_passwd5 PASSWORD 'md5e73a4b11df52a6068f8b39f90be36023';

SELECT rolname, regexp_replace(rolpassword, '(SCRAM-SHA-256)\$(\d+):([a-zA-Z0-9+/=]+)\$([a-zA-Z0-9+=/]+):([a-zA-Z0-9+/=]+)', '\1$\2:<salt>$<storedkey>:<serverkey>') as rolpassword_masked
    FROM pg_authid
    WHERE rolname LIKE 'regress_passwd%'
    ORDER BY rolname, rolpassword;

-- An empty password is not allowed, in any form
CREATE ROLE regress_passwd_empty PASSWORD '';
ALTER ROLE regress_passwd_empty PASSWORD 'md585939a5ce845f1a1b620742e3c659e0a';
ALTER ROLE regress_passwd_empty PASSWORD 'SCRAM-SHA-256$4096:hpFyHTUsSWcR7O9P$LgZFIt6Oqdo27ZFKbZ2nV+vtnYM995pDh9ca6WSi120=:qVV5NeluNfUPkwm7Vqat25RjSPLkGeoZBQs6wVv+um4=';
SELECT rolpassword FROM pg_authid WHERE rolname='regress_passwd_empty';

DROP ROLE regress_passwd1;
DROP ROLE regress_passwd2;
DROP ROLE regress_passwd3;
DROP ROLE regress_passwd4;
DROP ROLE regress_passwd5;
DROP ROLE regress_passwd_empty;

-- all entries should have been removed
SELECT rolname, rolpassword
    FROM pg_authid
    WHERE rolname LIKE 'regress_passwd%'
    ORDER BY rolname, rolpassword;
