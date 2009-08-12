--
-- Test named and nameless parameters
--

CREATE FUNCTION test_param_names0(integer, integer) RETURNS int AS $$
return args[0] + args[1]
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_param_names1(a0 integer, a1 text) RETURNS boolean AS $$
assert a0 == args[0]
assert a1 == args[1]
return True
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_param_names2(u users) RETURNS text AS $$
assert u == args[0]
return str(u)
$$ LANGUAGE plpythonu;

-- use deliberately wrong parameter names
CREATE FUNCTION test_param_names3(a0 integer) RETURNS boolean AS $$
try:
	assert a1 == args[0]
	return False
except NameError, e:
	assert e.args[0].find("a1") > -1
	return True
$$ LANGUAGE plpythonu;


SELECT test_param_names0(2,7);
SELECT test_param_names1(1,'text');
SELECT test_param_names2(users) from users;
SELECT test_param_names3(1);
