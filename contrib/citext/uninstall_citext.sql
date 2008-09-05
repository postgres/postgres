/* $PostgreSQL: pgsql/contrib/citext/uninstall_citext.sql,v 1.3 2008/09/05 18:25:16 tgl Exp $ */

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
DROP CAST (boolean AS citext);
DROP CAST (inet AS citext);

DROP FUNCTION citext(bpchar);
DROP FUNCTION citext(boolean);
DROP FUNCTION citext(inet);
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
DROP FUNCTION regexp_matches( citext, citext );
DROP FUNCTION regexp_matches( citext, citext, text );
DROP FUNCTION regexp_replace( citext, citext, text );
DROP FUNCTION regexp_replace( citext, citext, text, text );
DROP FUNCTION regexp_split_to_array( citext, citext );
DROP FUNCTION regexp_split_to_array( citext, citext, text );
DROP FUNCTION regexp_split_to_table( citext, citext );
DROP FUNCTION regexp_split_to_table( citext, citext, text );
DROP FUNCTION strpos( citext, citext );
DROP FUNCTION replace( citext, citext, citext );
DROP FUNCTION split_part( citext, citext, int );
DROP FUNCTION translate( citext, citext, text );

DROP TYPE citext CASCADE;
