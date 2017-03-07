--
-- Tests for password verifiers
--

-- Tests for GUC password_encryption
SET password_encryption = 'novalue'; -- error
SET password_encryption = true; -- ok
SET password_encryption = 'md5'; -- ok
SET password_encryption = 'plain'; -- ok
SET password_encryption = 'scram'; -- ok

-- consistency of password entries
SET password_encryption = 'plain';
CREATE ROLE regress_passwd1 PASSWORD 'role_pwd1';
SET password_encryption = 'md5';
CREATE ROLE regress_passwd2 PASSWORD 'role_pwd2';
SET password_encryption = 'on';
CREATE ROLE regress_passwd3 PASSWORD 'role_pwd3';
SET password_encryption = 'scram';
CREATE ROLE regress_passwd4 PASSWORD 'role_pwd4';
SET password_encryption = 'plain';
CREATE ROLE regress_passwd5 PASSWORD NULL;

-- check list of created entries
--
-- The scram verifier will look something like:
-- scram-sha-256:E4HxLGtnRzsYwg==:4096:5ebc825510cb7862efd87dfa638d8337179e6913a724441dc9e888a856fbc10c:e966b1c72fad89d69aaebb156eae04edc9581286f92207c044711e79cd461bee
--
-- Since the salt is random, the exact value stored will be different on every test
-- run. Use a regular expression to mask the changing parts.
SELECT rolname, regexp_replace(rolpassword, '(scram-sha-256):([a-zA-Z0-9+/]+==):(\d+):(\w+):(\w+)', '\1:<salt>:\3:<storedkey>:<serverkey>') as rolpassword_masked
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

ALTER ROLE regress_passwd4 ENCRYPTED PASSWORD 'scram-sha-256:VLK4RMaQLCvNtQ==:4096:3ded2376f7aafa93b1bdbd71bcc18b7d6ee50ed018029cc583d152ef3fc7d430:a6dd36dfc94c181956a6ae95f05e01b1864f0a22a2657d1de4ba84d2a24dc438'; -- client-supplied SCRAM verifier, use as it is

SET password_encryption = 'scram';
ALTER ROLE  regress_passwd5 ENCRYPTED PASSWORD 'foo'; -- create SCRAM verifier
CREATE ROLE regress_passwd6 ENCRYPTED PASSWORD 'md53725413363ab045e20521bf36b8d8d7f'; -- encrypted with MD5, use as it is

SELECT rolname, regexp_replace(rolpassword, '(scram-sha-256):([a-zA-Z0-9+/]+==):(\d+):(\w+):(\w+)', '\1:<salt>:\3:<storedkey>:<serverkey>') as rolpassword_masked
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
