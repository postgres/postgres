

CREATE FUNCTION global_test_one() returns text
    AS
'if not SD.has_key("global_test"):
	SD["global_test"] = "set by global_test_one"
if not GD.has_key("global_test"):
	GD["global_test"] = "set by global_test_one"
return "SD: " + SD["global_test"] + ", GD: " + GD["global_test"]'
    LANGUAGE 'plpython';

CREATE FUNCTION global_test_two() returns text
    AS
'if not SD.has_key("global_test"):
	SD["global_test"] = "set by global_test_two"
if not GD.has_key("global_test"):
	GD["global_test"] = "set by global_test_two"
return "SD: " + SD["global_test"] + ", GD: " + GD["global_test"]'
    LANGUAGE 'plpython';


CREATE FUNCTION static_test() returns int4
    AS
'if SD.has_key("call"):
	SD["call"] = SD["call"] + 1
else:
	SD["call"] = 1
return SD["call"]
'
    LANGUAGE 'plpython';

-- import python modules

CREATE FUNCTION import_fail() returns text
    AS
'try:
	import socket
except Exception, ex:
	plpy.notice("import socket failed -- %s" % str(ex))
	return "failed as expected"
return "succeeded, that wasn''t supposed to happen"'
    LANGUAGE 'plpython';


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
  import whrandom
except Exception, ex:
	plpy.notice("import failed -- %s" % str(ex))
	return "failed, that wasn''t supposed to happen"
return "succeeded, as expected"'
    LANGUAGE 'plpython';

CREATE FUNCTION import_test_one(text) RETURNS text
	AS
'import sha
digest = sha.new(args[0])
return digest.hexdigest()'
	LANGUAGE 'plpython';

CREATE FUNCTION import_test_two(users) RETURNS text
	AS
'import sha
plain = args[0]["fname"] + args[0]["lname"]
digest = sha.new(plain);
return "sha hash of " + plain + " is " + digest.hexdigest()'
	LANGUAGE 'plpython';

CREATE FUNCTION argument_test_one(users, text, text) RETURNS text
	AS
'words = args[1] + " " + args[2] + " => " + str(args[0])
return words'
	LANGUAGE 'plpython';


-- these triggers are dedicated to HPHC of RI who
-- decided that my kid's name was william not willem, and
-- vigorously resisted all efforts at correction.  they have
-- since gone bankrupt...

CREATE FUNCTION users_insert() returns opaque
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
	LANGUAGE 'plpython';


CREATE FUNCTION users_update() returns opaque
	AS
'if TD["event"] == "UPDATE":
	if TD["old"]["fname"] != TD["new"]["fname"] and TD["old"]["fname"] == TD["args"][0]:
		return "SKIP"
return None'
	LANGUAGE 'plpython';


CREATE FUNCTION users_delete() RETURNS opaque
	AS
'if TD["old"]["fname"] == TD["args"][0]:
	return "SKIP"
return None'
	LANGUAGE 'plpython';


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
	LANGUAGE 'plpython' ;

CREATE FUNCTION nested_call_two(text) RETURNS text
	AS
'q = "SELECT nested_call_three(''%s'')" % args[0]
r = plpy.execute(q)
return r[0]'
	LANGUAGE 'plpython' ;

CREATE FUNCTION nested_call_three(text) RETURNS text
	AS
'return args[0]'
	LANGUAGE 'plpython' ;

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
	LANGUAGE 'plpython';

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
	LANGUAGE 'plpython';


/* really stupid function just to get the module loaded
*/
CREATE FUNCTION stupid() RETURNS text AS 'return "zarkon"' LANGUAGE 'plpython';

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
	LANGUAGE 'plpython';

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
	LANGUAGE 'plpython';

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
	LANGUAGE 'plpython';


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
	LANGUAGE 'plpython';

/* check the handling of uncaught python exceptions
 */
CREATE FUNCTION exception_index_invalid(text) RETURNS text
	AS
'return args[1]'
	LANGUAGE 'plpython';

/* check handling of nested exceptions
 */
CREATE FUNCTION exception_index_invalid_nested() RETURNS text
	AS
'rv = plpy.execute("SELECT test5(''foo'')")
return rv[0]'
	LANGUAGE 'plpython';


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
	LANGUAGE 'plpython';



