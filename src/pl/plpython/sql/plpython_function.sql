

CREATE FUNCTION global_test_one() returns text
    AS
'if not SD.has_key("global_test"):
	SD["global_test"] = "set by global_test_one"
if not GD.has_key("global_test"):
	GD["global_test"] = "set by global_test_one"
return "SD: " + SD["global_test"] + ", GD: " + GD["global_test"]'
    LANGUAGE plpythonu;

CREATE FUNCTION global_test_two() returns text
    AS
'if not SD.has_key("global_test"):
	SD["global_test"] = "set by global_test_two"
if not GD.has_key("global_test"):
	GD["global_test"] = "set by global_test_two"
return "SD: " + SD["global_test"] + ", GD: " + GD["global_test"]'
    LANGUAGE plpythonu;


CREATE FUNCTION static_test() returns int4
    AS
'if SD.has_key("call"):
	SD["call"] = SD["call"] + 1
else:
	SD["call"] = 1
return SD["call"]
'
    LANGUAGE plpythonu;

-- import python modules

CREATE FUNCTION import_fail() returns text
    AS
'try:
	import foosocket
except Exception, ex:
	plpy.notice("import socket failed -- %s" % str(ex))
	return "failed as expected"
return "succeeded, that wasn''t supposed to happen"'
    LANGUAGE plpythonu;


CREATE FUNCTION import_succeed() returns text
	AS
'try:
  import array
  import bisect
  import calendar
  import cmath
  import errno
  import math
  import md5
  import operator
  import random
  import re
  import sha
  import string
  import time
except Exception, ex:
	plpy.notice("import failed -- %s" % str(ex))
	return "failed, that wasn''t supposed to happen"
return "succeeded, as expected"'
    LANGUAGE plpythonu;

CREATE FUNCTION import_test_one(text) RETURNS text
	AS
'import sha
digest = sha.new(args[0])
return digest.hexdigest()'
	LANGUAGE plpythonu;

CREATE FUNCTION import_test_two(users) RETURNS text
	AS
'import sha
plain = args[0]["fname"] + args[0]["lname"]
digest = sha.new(plain);
return "sha hash of " + plain + " is " + digest.hexdigest()'
	LANGUAGE plpythonu;

CREATE FUNCTION argument_test_one(users, text, text) RETURNS text
	AS
'keys = args[0].keys()
keys.sort()
out = []
for key in keys:
    out.append("%s: %s" % (key, args[0][key]))
words = args[1] + " " + args[2] + " => {" + ", ".join(out) + "}"
return words'
	LANGUAGE plpythonu;


-- these triggers are dedicated to HPHC of RI who
-- decided that my kid's name was william not willem, and
-- vigorously resisted all efforts at correction.  they have
-- since gone bankrupt...

CREATE FUNCTION users_insert() returns trigger
	AS
'if TD["new"]["fname"] == None or TD["new"]["lname"] == None:
	return "SKIP"
if TD["new"]["username"] == None:
	TD["new"]["username"] = TD["new"]["fname"][:1] + "_" + TD["new"]["lname"]
	rv = "MODIFY"
else:
	rv = None
if TD["new"]["fname"] == "william":
	TD["new"]["fname"] = TD["args"][0]
	rv = "MODIFY"
return rv'
	LANGUAGE plpythonu;


CREATE FUNCTION users_update() returns trigger
	AS
'if TD["event"] == "UPDATE":
	if TD["old"]["fname"] != TD["new"]["fname"] and TD["old"]["fname"] == TD["args"][0]:
		return "SKIP"
return None'
	LANGUAGE plpythonu;


CREATE FUNCTION users_delete() RETURNS trigger
	AS
'if TD["old"]["fname"] == TD["args"][0]:
	return "SKIP"
return None'
	LANGUAGE plpythonu;


CREATE TRIGGER users_insert_trig BEFORE INSERT ON users FOR EACH ROW
	EXECUTE PROCEDURE users_insert ('willem');

CREATE TRIGGER users_update_trig BEFORE UPDATE ON users FOR EACH ROW
	EXECUTE PROCEDURE users_update ('willem');

CREATE TRIGGER users_delete_trig BEFORE DELETE ON users FOR EACH ROW
	EXECUTE PROCEDURE users_delete ('willem');


-- nested calls
--

CREATE FUNCTION nested_call_one(text) RETURNS text
	AS
'q = "SELECT nested_call_two(''%s'')" % args[0]
r = plpy.execute(q)
return r[0]'
	LANGUAGE plpythonu ;

CREATE FUNCTION nested_call_two(text) RETURNS text
	AS
'q = "SELECT nested_call_three(''%s'')" % args[0]
r = plpy.execute(q)
return r[0]'
	LANGUAGE plpythonu ;

CREATE FUNCTION nested_call_three(text) RETURNS text
	AS
'return args[0]'
	LANGUAGE plpythonu ;

-- some spi stuff

CREATE FUNCTION spi_prepared_plan_test_one(text) RETURNS text
	AS
'if not SD.has_key("myplan"):
	q = "SELECT count(*) FROM users WHERE lname = $1"
	SD["myplan"] = plpy.prepare(q, [ "text" ])
try:
	rv = plpy.execute(SD["myplan"], [args[0]])
	return "there are " + str(rv[0]["count"]) + " " + str(args[0]) + "s"
except Exception, ex:
	plpy.error(str(ex))
return None
'
	LANGUAGE plpythonu;

CREATE FUNCTION spi_prepared_plan_test_nested(text) RETURNS text
	AS
'if not SD.has_key("myplan"):
	q = "SELECT spi_prepared_plan_test_one(''%s'') as count" % args[0]
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


/* really stupid function just to get the module loaded
*/
CREATE FUNCTION stupid() RETURNS text AS 'return "zarkon"' LANGUAGE plpythonu;

/* a typo
*/
CREATE FUNCTION invalid_type_uncaught(text) RETURNS text
	AS
'if not SD.has_key("plan"):
	q = "SELECT fname FROM users WHERE lname = $1"
	SD["plan"] = plpy.prepare(q, [ "test" ])
rv = plpy.execute(SD["plan"], [ args[0] ])
if len(rv):
	return rv[0]["fname"]
return None
'
	LANGUAGE plpythonu;

/* for what it's worth catch the exception generated by
 * the typo, and return None
 */
CREATE FUNCTION invalid_type_caught(text) RETURNS text
	AS
'if not SD.has_key("plan"):
	q = "SELECT fname FROM users WHERE lname = $1"
	try:
		SD["plan"] = plpy.prepare(q, [ "test" ])
	except plpy.SPIError, ex:
		plpy.notice(str(ex))
		return None
rv = plpy.execute(SD["plan"], [ args[0] ])
if len(rv):
	return rv[0]["fname"]
return None
'
	LANGUAGE plpythonu;

/* for what it's worth catch the exception generated by
 * the typo, and reraise it as a plain error
 */
CREATE FUNCTION invalid_type_reraised(text) RETURNS text
	AS
'if not SD.has_key("plan"):
	q = "SELECT fname FROM users WHERE lname = $1"
	try:
		SD["plan"] = plpy.prepare(q, [ "test" ])
	except plpy.SPIError, ex:
		plpy.error(str(ex))
rv = plpy.execute(SD["plan"], [ args[0] ])
if len(rv):
	return rv[0]["fname"]
return None
'
	LANGUAGE plpythonu;


/* no typo no messing about
*/
CREATE FUNCTION valid_type(text) RETURNS text
	AS
'if not SD.has_key("plan"):
	SD["plan"] = plpy.prepare("SELECT fname FROM users WHERE lname = $1", [ "text" ])
rv = plpy.execute(SD["plan"], [ args[0] ])
if len(rv):
	return rv[0]["fname"]
return None
'
	LANGUAGE plpythonu;
/* Flat out syntax error
*/
CREATE FUNCTION sql_syntax_error() RETURNS text
        AS
'plpy.execute("syntax error")'
        LANGUAGE plpythonu;

/* check the handling of uncaught python exceptions
 */
CREATE FUNCTION exception_index_invalid(text) RETURNS text
	AS
'return args[1]'
	LANGUAGE plpythonu;

/* check handling of nested exceptions
 */
CREATE FUNCTION exception_index_invalid_nested() RETURNS text
	AS
'rv = plpy.execute("SELECT test5(''foo'')")
return rv[0]'
	LANGUAGE plpythonu;


CREATE FUNCTION join_sequences(sequences) RETURNS text
	AS
'if not args[0]["multipart"]:
	return args[0]["sequence"]
q = "SELECT sequence FROM xsequences WHERE pid = ''%s''" % args[0]["pid"]
rv = plpy.execute(q)
seq = args[0]["sequence"]
for r in rv:
	seq = seq + r["sequence"]
return seq
'
	LANGUAGE plpythonu;

--
-- Universal Newline Support
-- 

CREATE OR REPLACE FUNCTION newline_lf() RETURNS integer AS
'x = 100\ny = 23\nreturn x + y\n'
LANGUAGE plpythonu;

CREATE OR REPLACE FUNCTION newline_cr() RETURNS integer AS
'x = 100\ry = 23\rreturn x + y\r'
LANGUAGE plpythonu;

CREATE OR REPLACE FUNCTION newline_crlf() RETURNS integer AS
'x = 100\r\ny = 23\r\nreturn x + y\r\n'
LANGUAGE plpythonu;

--
-- Unicode error handling
--

CREATE FUNCTION unicode_return_error() RETURNS text AS '
return u"\\x80"
' LANGUAGE plpythonu;

CREATE FUNCTION unicode_trigger_error() RETURNS trigger AS '
TD["new"]["testvalue"] = u"\\x80"
return "MODIFY"
' LANGUAGE plpythonu;

CREATE TRIGGER unicode_test_bi BEFORE INSERT ON unicode_test
  FOR EACH ROW EXECUTE PROCEDURE unicode_trigger_error();

CREATE FUNCTION unicode_plan_error1() RETURNS text AS '
plan = plpy.prepare("SELECT $1 AS testvalue", ["text"])
rv = plpy.execute(plan, [u"\\x80"], 1)
return rv[0]["testvalue"]
' LANGUAGE plpythonu;

CREATE FUNCTION unicode_plan_error2() RETURNS text AS '
plan = plpy.prepare("SELECT $1 AS testvalue1, $2 AS testvalue2", ["text", "text"])
rv = plpy.execute(plan, u"\\x80", 1)
return rv[0]["testvalue1"]
' LANGUAGE plpythonu;
