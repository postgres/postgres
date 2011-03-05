--
-- For paranoia's sake, don't leave an untrusted language sitting around
--
SET client_min_messages = WARNING;

DROP EXTENSION plpythonu CASCADE;

DROP EXTENSION IF EXISTS plpython2u CASCADE;
