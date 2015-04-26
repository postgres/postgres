CREATE EXTENSION plpython2u;
CREATE EXTENSION ltree_plpython2u;


CREATE FUNCTION test1(val ltree) RETURNS int
LANGUAGE plpythonu
TRANSFORM FOR TYPE ltree
AS $$
plpy.info(repr(val))
return len(val)
$$;

SELECT test1('aa.bb.cc'::ltree);


CREATE FUNCTION test1n(val ltree) RETURNS int
LANGUAGE plpython2u
TRANSFORM FOR TYPE ltree
AS $$
plpy.info(repr(val))
return len(val)
$$;

SELECT test1n('aa.bb.cc'::ltree);


CREATE FUNCTION test2() RETURNS ltree
LANGUAGE plpythonu
TRANSFORM FOR TYPE ltree
AS $$
return ['foo', 'bar', 'baz']
$$;

-- plpython to ltree is not yet implemented, so this will fail,
-- because it will try to parse the Python list as an ltree input
-- string.
SELECT test2();
