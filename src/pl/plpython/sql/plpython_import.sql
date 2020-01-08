-- import python modules

CREATE FUNCTION import_fail() returns text
    AS
'try:
	import foosocket
except ImportError:
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
  import operator
  import random
  import re
  import string
  import time
except Exception as ex:
	plpy.notice("import failed -- %s" % str(ex))
	return "failed, that wasn''t supposed to happen"
return "succeeded, as expected"'
    LANGUAGE plpythonu;

CREATE FUNCTION import_test_one(p text) RETURNS text
	AS
'try:
    import hashlib
    digest = hashlib.sha1(p.encode("ascii"))
except ImportError:
    import sha
    digest = sha.new(p)
return digest.hexdigest()'
	LANGUAGE plpythonu;

CREATE FUNCTION import_test_two(u users) RETURNS text
	AS
'plain = u["fname"] + u["lname"]
try:
    import hashlib
    digest = hashlib.sha1(plain.encode("ascii"))
except ImportError:
    import sha
    digest = sha.new(plain);
return "sha hash of " + plain + " is " + digest.hexdigest()'
	LANGUAGE plpythonu;


-- import python modules
--
SELECT import_fail();
SELECT import_succeed();

-- test import and simple argument handling
--
SELECT import_test_one('sha hash of this string');

-- test import and tuple argument handling
--
select import_test_two(users) from users where fname = 'willem';
