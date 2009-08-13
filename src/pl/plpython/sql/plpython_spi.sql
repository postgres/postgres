
-- nested calls
--

CREATE FUNCTION nested_call_one(a text) RETURNS text
	AS
'q = "SELECT nested_call_two(''%s'')" % a
r = plpy.execute(q)
return r[0]'
	LANGUAGE plpythonu ;

CREATE FUNCTION nested_call_two(a text) RETURNS text
	AS
'q = "SELECT nested_call_three(''%s'')" % a
r = plpy.execute(q)
return r[0]'
	LANGUAGE plpythonu ;

CREATE FUNCTION nested_call_three(a text) RETURNS text
	AS
'return a'
	LANGUAGE plpythonu ;

-- some spi stuff

CREATE FUNCTION spi_prepared_plan_test_one(a text) RETURNS text
	AS
'if not SD.has_key("myplan"):
	q = "SELECT count(*) FROM users WHERE lname = $1"
	SD["myplan"] = plpy.prepare(q, [ "text" ])
try:
	rv = plpy.execute(SD["myplan"], [a])
	return "there are " + str(rv[0]["count"]) + " " + str(a) + "s"
except Exception, ex:
	plpy.error(str(ex))
return None
'
	LANGUAGE plpythonu;

CREATE FUNCTION spi_prepared_plan_test_nested(a text) RETURNS text
	AS
'if not SD.has_key("myplan"):
	q = "SELECT spi_prepared_plan_test_one(''%s'') as count" % a
	SD["myplan"] = plpy.prepare(q)
try:
	rv = plpy.execute(SD["myplan"])
	if len(rv):
		return rv[0]["count"]
except Exception, ex:
	plpy.error(str(ex))
return None
'
	LANGUAGE plpythonu;




CREATE FUNCTION join_sequences(s sequences) RETURNS text
	AS
'if not s["multipart"]:
	return s["sequence"]
q = "SELECT sequence FROM xsequences WHERE pid = ''%s''" % s["pid"]
rv = plpy.execute(q)
seq = s["sequence"]
for r in rv:
	seq = seq + r["sequence"]
return seq
'
	LANGUAGE plpythonu;





-- spi and nested calls
--
select nested_call_one('pass this along');
select spi_prepared_plan_test_one('doe');
select spi_prepared_plan_test_one('smith');
select spi_prepared_plan_test_nested('smith');




SELECT join_sequences(sequences) FROM sequences;
SELECT join_sequences(sequences) FROM sequences
	WHERE join_sequences(sequences) ~* '^A';
SELECT join_sequences(sequences) FROM sequences
	WHERE join_sequences(sequences) ~* '^B';
