-- Each DefineIndex() ACL check uses either the original userid or the table
-- owner userid; see its header comment.  Here, confirm that DefineIndex()
-- uses its original userid where necessary.  The test works by creating
-- indexes that refer to as many sorts of objects as possible, with the table
-- owner having as few applicable privileges as possible.  (The privileges.sql
-- regress_sro_user tests look for the opposite defect; they confirm that
-- DefineIndex() uses the table owner userid where necessary.)

-- Don't override tablespaces; this version lacks allow_in_place_tablespaces.

BEGIN;
CREATE ROLE regress_minimal;
CREATE SCHEMA s;
CREATE EXTENSION citext SCHEMA s;
-- Revoke all conceivably-relevant ACLs within the extension.  The system
-- doesn't check all these ACLs, but this will provide some coverage if that
-- ever changes.
REVOKE ALL ON TYPE s.citext FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_lt FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_le FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_eq FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_ge FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_gt FROM PUBLIC;
REVOKE ALL ON FUNCTION s.citext_pattern_cmp FROM PUBLIC;
-- Functions sufficient for making an index column that has the side effect of
-- changing search_path at expression planning time.
CREATE FUNCTION public.setter() RETURNS bool VOLATILE
  LANGUAGE SQL AS $$SET search_path = s; SELECT true$$;
CREATE FUNCTION s.const() RETURNS bool IMMUTABLE
  LANGUAGE SQL AS $$SELECT public.setter()$$;
CREATE FUNCTION s.index_this_expr(s.citext, bool) RETURNS s.citext IMMUTABLE
  LANGUAGE SQL AS $$SELECT $1$$;
REVOKE ALL ON FUNCTION public.setter FROM PUBLIC;
REVOKE ALL ON FUNCTION s.const FROM PUBLIC;
REVOKE ALL ON FUNCTION s.index_this_expr FROM PUBLIC;
-- Even for an empty table, expression planning calls s.const & public.setter.
GRANT EXECUTE ON FUNCTION public.setter TO regress_minimal;
GRANT EXECUTE ON FUNCTION s.const TO regress_minimal;
-- Function for index predicate.
CREATE FUNCTION s.index_row_if(s.citext) RETURNS bool IMMUTABLE
  LANGUAGE SQL AS $$SELECT $1 IS NOT NULL$$;
REVOKE ALL ON FUNCTION s.index_row_if FROM PUBLIC;
-- Even for an empty table, CREATE INDEX checks ii_Predicate permissions.
GRANT EXECUTE ON FUNCTION s.index_row_if TO regress_minimal;
-- Non-extension, non-function objects.
CREATE COLLATION s.coll (LOCALE="C");
CREATE TABLE s.x (y s.citext);
ALTER TABLE s.x OWNER TO regress_minimal;
-- Empty-table DefineIndex()
CREATE UNIQUE INDEX u0rows ON s.x USING btree
  ((s.index_this_expr(y, s.const())) COLLATE s.coll s.citext_pattern_ops)
  WHERE s.index_row_if(y);
ALTER TABLE s.x ADD CONSTRAINT e0rows EXCLUDE USING btree
  ((s.index_this_expr(y, s.const())) COLLATE s.coll WITH s.=)
  WHERE (s.index_row_if(y));
-- Make the table nonempty.
INSERT INTO s.x VALUES ('foo'), ('bar');
-- If the INSERT runs the planner on index expressions, a search_path change
-- survives.  As of 2022-06, the INSERT reuses a cached plan.  It does so even
-- under debug_discard_caches, since each index is new-in-transaction.  If
-- future work changes a cache lifecycle, this RESET may become necessary.
RESET search_path;
-- For a nonempty table, owner needs permissions throughout ii_Expressions.
GRANT EXECUTE ON FUNCTION s.index_this_expr TO regress_minimal;
CREATE UNIQUE INDEX u2rows ON s.x USING btree
  ((s.index_this_expr(y, s.const())) COLLATE s.coll s.citext_pattern_ops)
  WHERE s.index_row_if(y);
ALTER TABLE s.x ADD CONSTRAINT e2rows EXCLUDE USING btree
  ((s.index_this_expr(y, s.const())) COLLATE s.coll WITH s.=)
  WHERE (s.index_row_if(y));
-- Shall not find s.coll via search_path, despite the s.const->public.setter
-- call having set search_path=s during expression planning.  Suppress the
-- message itself, which depends on the database encoding.
\set VERBOSITY sqlstate
ALTER TABLE s.x ADD CONSTRAINT underqualified EXCLUDE USING btree
  ((s.index_this_expr(y, s.const())) COLLATE coll WITH s.=)
  WHERE (s.index_row_if(y));
\set VERBOSITY default
ROLLBACK;
