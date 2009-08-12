--
-- Unicode handling
--

CREATE TABLE unicode_test (
	testvalue  text NOT NULL
);

CREATE FUNCTION unicode_return_error() RETURNS text AS E'
return u"\\x80"
' LANGUAGE plpythonu;

CREATE FUNCTION unicode_trigger_error() RETURNS trigger AS E'
TD["new"]["testvalue"] = u"\\x80"
return "MODIFY"
' LANGUAGE plpythonu;

CREATE TRIGGER unicode_test_bi BEFORE INSERT ON unicode_test
  FOR EACH ROW EXECUTE PROCEDURE unicode_trigger_error();

CREATE FUNCTION unicode_plan_error1() RETURNS text AS E'
plan = plpy.prepare("SELECT $1 AS testvalue", ["text"])
rv = plpy.execute(plan, [u"\\x80"], 1)
return rv[0]["testvalue"]
' LANGUAGE plpythonu;

CREATE FUNCTION unicode_plan_error2() RETURNS text AS E'
plan = plpy.prepare("SELECT $1 AS testvalue1, $2 AS testvalue2", ["text", "text"])
rv = plpy.execute(plan, u"\\x80", 1)
return rv[0]["testvalue1"]
' LANGUAGE plpythonu;


SELECT unicode_return_error();
INSERT INTO unicode_test (testvalue) VALUES ('test');
SELECT unicode_plan_error1();
SELECT unicode_plan_error2();
