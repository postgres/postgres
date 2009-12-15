-- first some tests of basic functionality
CREATE LANGUAGE plpython2u;

-- really stupid function just to get the module loaded
CREATE FUNCTION stupid() RETURNS text AS 'return "zarkon"' LANGUAGE plpythonu;

select stupid();

-- check 2/3 versioning
CREATE FUNCTION stupidn() RETURNS text AS 'return "zarkon"' LANGUAGE plpython2u;

select stupidn();

-- test multiple arguments
CREATE FUNCTION argument_test_one(u users, a1 text, a2 text) RETURNS text
	AS
'keys = list(u.keys())
keys.sort()
out = []
for key in keys:
    out.append("%s: %s" % (key, u[key]))
words = a1 + " " + a2 + " => {" + ", ".join(out) + "}"
return words'
	LANGUAGE plpythonu;

select argument_test_one(users, fname, lname) from users where lname = 'doe' order by 1;


CREATE FUNCTION elog_test() RETURNS void
AS $$
plpy.debug('debug')
plpy.log('log')
plpy.info('info')
plpy.info(37)
plpy.info()
plpy.info('info', 37, [1, 2, 3])
plpy.notice('notice')
plpy.warning('warning')
plpy.error('error')
$$ LANGUAGE plpythonu;

SELECT elog_test();
