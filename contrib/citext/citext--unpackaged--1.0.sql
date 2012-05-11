/* contrib/citext/citext--unpackaged--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION citext" to load this file. \quit

ALTER EXTENSION citext ADD type citext;
ALTER EXTENSION citext ADD function citextin(cstring);
ALTER EXTENSION citext ADD function citextout(citext);
ALTER EXTENSION citext ADD function citextrecv(internal);
ALTER EXTENSION citext ADD function citextsend(citext);
ALTER EXTENSION citext ADD function citext(character);
ALTER EXTENSION citext ADD function citext(boolean);
ALTER EXTENSION citext ADD function citext(inet);
ALTER EXTENSION citext ADD cast (citext as text);
ALTER EXTENSION citext ADD cast (citext as character varying);
ALTER EXTENSION citext ADD cast (citext as character);
ALTER EXTENSION citext ADD cast (text as citext);
ALTER EXTENSION citext ADD cast (character varying as citext);
ALTER EXTENSION citext ADD cast (character as citext);
ALTER EXTENSION citext ADD cast (boolean as citext);
ALTER EXTENSION citext ADD cast (inet as citext);
ALTER EXTENSION citext ADD function citext_eq(citext,citext);
ALTER EXTENSION citext ADD function citext_ne(citext,citext);
ALTER EXTENSION citext ADD function citext_lt(citext,citext);
ALTER EXTENSION citext ADD function citext_le(citext,citext);
ALTER EXTENSION citext ADD function citext_gt(citext,citext);
ALTER EXTENSION citext ADD function citext_ge(citext,citext);
ALTER EXTENSION citext ADD operator <>(citext,citext);
ALTER EXTENSION citext ADD operator =(citext,citext);
ALTER EXTENSION citext ADD operator >(citext,citext);
ALTER EXTENSION citext ADD operator >=(citext,citext);
ALTER EXTENSION citext ADD operator <(citext,citext);
ALTER EXTENSION citext ADD operator <=(citext,citext);
ALTER EXTENSION citext ADD function citext_cmp(citext,citext);
ALTER EXTENSION citext ADD function citext_hash(citext);
ALTER EXTENSION citext ADD operator family citext_ops using btree;
ALTER EXTENSION citext ADD operator class citext_ops using btree;
ALTER EXTENSION citext ADD operator family citext_ops using hash;
ALTER EXTENSION citext ADD operator class citext_ops using hash;
ALTER EXTENSION citext ADD function citext_smaller(citext,citext);
ALTER EXTENSION citext ADD function citext_larger(citext,citext);
ALTER EXTENSION citext ADD function min(citext);
ALTER EXTENSION citext ADD function max(citext);
ALTER EXTENSION citext ADD function texticlike(citext,citext);
ALTER EXTENSION citext ADD function texticnlike(citext,citext);
ALTER EXTENSION citext ADD function texticregexeq(citext,citext);
ALTER EXTENSION citext ADD function texticregexne(citext,citext);
ALTER EXTENSION citext ADD operator !~(citext,citext);
ALTER EXTENSION citext ADD operator ~(citext,citext);
ALTER EXTENSION citext ADD operator !~*(citext,citext);
ALTER EXTENSION citext ADD operator ~*(citext,citext);
ALTER EXTENSION citext ADD operator !~~(citext,citext);
ALTER EXTENSION citext ADD operator ~~(citext,citext);
ALTER EXTENSION citext ADD operator !~~*(citext,citext);
ALTER EXTENSION citext ADD operator ~~*(citext,citext);
ALTER EXTENSION citext ADD function texticlike(citext,text);
ALTER EXTENSION citext ADD function texticnlike(citext,text);
ALTER EXTENSION citext ADD function texticregexeq(citext,text);
ALTER EXTENSION citext ADD function texticregexne(citext,text);
ALTER EXTENSION citext ADD operator !~(citext,text);
ALTER EXTENSION citext ADD operator ~(citext,text);
ALTER EXTENSION citext ADD operator !~*(citext,text);
ALTER EXTENSION citext ADD operator ~*(citext,text);
ALTER EXTENSION citext ADD operator !~~(citext,text);
ALTER EXTENSION citext ADD operator ~~(citext,text);
ALTER EXTENSION citext ADD operator !~~*(citext,text);
ALTER EXTENSION citext ADD operator ~~*(citext,text);
ALTER EXTENSION citext ADD function regexp_matches(citext,citext);
ALTER EXTENSION citext ADD function regexp_matches(citext,citext,text);
ALTER EXTENSION citext ADD function regexp_replace(citext,citext,text);
ALTER EXTENSION citext ADD function regexp_replace(citext,citext,text,text);
ALTER EXTENSION citext ADD function regexp_split_to_array(citext,citext);
ALTER EXTENSION citext ADD function regexp_split_to_array(citext,citext,text);
ALTER EXTENSION citext ADD function regexp_split_to_table(citext,citext);
ALTER EXTENSION citext ADD function regexp_split_to_table(citext,citext,text);
ALTER EXTENSION citext ADD function strpos(citext,citext);
ALTER EXTENSION citext ADD function replace(citext,citext,citext);
ALTER EXTENSION citext ADD function split_part(citext,citext,integer);
ALTER EXTENSION citext ADD function translate(citext,citext,text);

--
-- As of 9.1, type citext should be marked collatable.  There is no ALTER TYPE
-- command for this, so we have to do it by poking the pg_type entry directly.
-- We have to poke any derived copies in pg_attribute or pg_index as well,
-- as well as those for arrays/domains based directly or indirectly on citext.
-- Notes: 100 is the OID of the "pg_catalog.default" collation --- it seems
-- easier and more reliable to hard-wire that here than to pull it out of
-- pg_collation.  Also, we don't need to make pg_depend entries since the
-- default collation is pinned.
--

WITH RECURSIVE typeoids(typoid) AS
  ( SELECT 'citext'::pg_catalog.regtype UNION
    SELECT oid FROM pg_catalog.pg_type, typeoids
      WHERE typelem = typoid OR typbasetype = typoid )
UPDATE pg_catalog.pg_type SET typcollation = 100
FROM typeoids
WHERE oid = typeoids.typoid;

WITH RECURSIVE typeoids(typoid) AS
  ( SELECT 'citext'::pg_catalog.regtype UNION
    SELECT oid FROM pg_catalog.pg_type, typeoids
      WHERE typelem = typoid OR typbasetype = typoid )
UPDATE pg_catalog.pg_attribute SET attcollation = 100
FROM typeoids
WHERE atttypid = typeoids.typoid;

UPDATE pg_catalog.pg_index SET indcollation[0] = 100
WHERE indclass[0] IN (
  WITH RECURSIVE typeoids(typoid) AS
    ( SELECT 'citext'::pg_catalog.regtype UNION
      SELECT oid FROM pg_catalog.pg_type, typeoids
        WHERE typelem = typoid OR typbasetype = typoid )
  SELECT oid FROM pg_catalog.pg_opclass, typeoids
  WHERE opcintype = typeoids.typoid
);

UPDATE pg_catalog.pg_index SET indcollation[1] = 100
WHERE indclass[1] IN (
  WITH RECURSIVE typeoids(typoid) AS
    ( SELECT 'citext'::pg_catalog.regtype UNION
      SELECT oid FROM pg_catalog.pg_type, typeoids
        WHERE typelem = typoid OR typbasetype = typoid )
  SELECT oid FROM pg_catalog.pg_opclass, typeoids
  WHERE opcintype = typeoids.typoid
);

UPDATE pg_catalog.pg_index SET indcollation[2] = 100
WHERE indclass[2] IN (
  WITH RECURSIVE typeoids(typoid) AS
    ( SELECT 'citext'::pg_catalog.regtype UNION
      SELECT oid FROM pg_catalog.pg_type, typeoids
        WHERE typelem = typoid OR typbasetype = typoid )
  SELECT oid FROM pg_catalog.pg_opclass, typeoids
  WHERE opcintype = typeoids.typoid
);

UPDATE pg_catalog.pg_index SET indcollation[3] = 100
WHERE indclass[3] IN (
  WITH RECURSIVE typeoids(typoid) AS
    ( SELECT 'citext'::pg_catalog.regtype UNION
      SELECT oid FROM pg_catalog.pg_type, typeoids
        WHERE typelem = typoid OR typbasetype = typoid )
  SELECT oid FROM pg_catalog.pg_opclass, typeoids
  WHERE opcintype = typeoids.typoid
);

UPDATE pg_catalog.pg_index SET indcollation[4] = 100
WHERE indclass[4] IN (
  WITH RECURSIVE typeoids(typoid) AS
    ( SELECT 'citext'::pg_catalog.regtype UNION
      SELECT oid FROM pg_catalog.pg_type, typeoids
        WHERE typelem = typoid OR typbasetype = typoid )
  SELECT oid FROM pg_catalog.pg_opclass, typeoids
  WHERE opcintype = typeoids.typoid
);

UPDATE pg_catalog.pg_index SET indcollation[5] = 100
WHERE indclass[5] IN (
  WITH RECURSIVE typeoids(typoid) AS
    ( SELECT 'citext'::pg_catalog.regtype UNION
      SELECT oid FROM pg_catalog.pg_type, typeoids
        WHERE typelem = typoid OR typbasetype = typoid )
  SELECT oid FROM pg_catalog.pg_opclass, typeoids
  WHERE opcintype = typeoids.typoid
);

UPDATE pg_catalog.pg_index SET indcollation[6] = 100
WHERE indclass[6] IN (
  WITH RECURSIVE typeoids(typoid) AS
    ( SELECT 'citext'::pg_catalog.regtype UNION
      SELECT oid FROM pg_catalog.pg_type, typeoids
        WHERE typelem = typoid OR typbasetype = typoid )
  SELECT oid FROM pg_catalog.pg_opclass, typeoids
  WHERE opcintype = typeoids.typoid
);

UPDATE pg_catalog.pg_index SET indcollation[7] = 100
WHERE indclass[7] IN (
  WITH RECURSIVE typeoids(typoid) AS
    ( SELECT 'citext'::pg_catalog.regtype UNION
      SELECT oid FROM pg_catalog.pg_type, typeoids
        WHERE typelem = typoid OR typbasetype = typoid )
  SELECT oid FROM pg_catalog.pg_opclass, typeoids
  WHERE opcintype = typeoids.typoid
);

-- somewhat arbitrarily, we assume no citext indexes have more than 8 columns
