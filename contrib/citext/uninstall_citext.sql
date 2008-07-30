/* $PostgreSQL: pgsql/contrib/citext/uninstall_citext.sql,v 1.2 2008/07/30 17:08:52 tgl Exp $ */

-- Adjust this setting to control where the objects get dropped.
SET search_path = public;

DROP OPERATOR CLASS citext_ops USING btree CASCADE;
DROP OPERATOR CLASS citext_ops USING hash CASCADE;

DROP AGGREGATE min(citext);
DROP AGGREGATE max(citext);

DROP OPERATOR = (citext, citext);
DROP OPERATOR <> (citext, citext);
DROP OPERATOR < (citext, citext);
DROP OPERATOR <= (citext, citext);
DROP OPERATOR >= (citext, citext);
DROP OPERATOR > (citext, citext);

DROP OPERATOR ~ (citext, citext);
DROP OPERATOR ~* (citext, citext);
DROP OPERATOR !~ (citext, citext);
DROP OPERATOR !~* (citext, citext);
DROP OPERATOR ~~ (citext, citext);
DROP OPERATOR ~~* (citext, citext);
DROP OPERATOR !~~ (citext, citext);
DROP OPERATOR !~~* (citext, citext);

DROP OPERATOR ~ (citext, text);
DROP OPERATOR ~* (citext, text);
DROP OPERATOR !~ (citext, text);
DROP OPERATOR !~* (citext, text);
DROP OPERATOR ~~ (citext, text);
DROP OPERATOR ~~* (citext, text);
DROP OPERATOR !~~ (citext, text);
DROP OPERATOR !~~* (citext, text);

DROP CAST (citext AS text);
DROP CAST (citext AS varchar);
DROP CAST (citext AS bpchar);
DROP CAST (text AS citext);
DROP CAST (varchar AS citext);
DROP CAST (bpchar AS citext);

DROP FUNCTION citext(bpchar);
DROP FUNCTION citext_eq(citext, citext);
DROP FUNCTION citext_ne(citext, citext);
DROP FUNCTION citext_lt(citext, citext);
DROP FUNCTION citext_le(citext, citext);
DROP FUNCTION citext_gt(citext, citext);
DROP FUNCTION citext_ge(citext, citext);
DROP FUNCTION citext_cmp(citext, citext);
DROP FUNCTION citext_hash(citext);
DROP FUNCTION citext_smaller(citext, citext);
DROP FUNCTION citext_larger(citext, citext);
DROP FUNCTION texticlike(citext, citext);
DROP FUNCTION texticnlike(citext, citext);
DROP FUNCTION texticregexeq(citext, citext);
DROP FUNCTION texticregexne(citext, citext);
DROP FUNCTION texticlike(citext, text);
DROP FUNCTION texticnlike(citext, text);
DROP FUNCTION texticregexeq(citext, text);
DROP FUNCTION texticregexne(citext, text);

DROP TYPE citext CASCADE;
