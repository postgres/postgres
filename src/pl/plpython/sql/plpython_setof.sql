--
-- Test returning SETOF
--

CREATE FUNCTION test_setof_error() RETURNS SETOF text AS $$
return 37
$$ LANGUAGE plpythonu;

SELECT test_setof_error();


CREATE FUNCTION test_setof_as_list(count integer, content text) RETURNS SETOF text AS $$
return [ content ]*count
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_setof_as_tuple(count integer, content text) RETURNS SETOF text AS $$
t = ()
for i in range(count):
	t += ( content, )
return t
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_setof_as_iterator(count integer, content text) RETURNS SETOF text AS $$
class producer:
	def __init__ (self, icount, icontent):
		self.icontent = icontent
		self.icount = icount
	def __iter__ (self):
		return self
	def next (self):
		if self.icount == 0:
			raise StopIteration
		self.icount -= 1
		return self.icontent
return producer(count, content)
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_setof_spi_in_iterator() RETURNS SETOF text AS
$$
    for s in ('Hello', 'Brave', 'New', 'World'):
        plpy.execute('select 1')
        yield s
        plpy.execute('select 2')
$$
LANGUAGE plpythonu;


-- Test set returning functions
SELECT test_setof_as_list(0, 'list');
SELECT test_setof_as_list(1, 'list');
SELECT test_setof_as_list(2, 'list');
SELECT test_setof_as_list(2, null);

SELECT test_setof_as_tuple(0, 'tuple');
SELECT test_setof_as_tuple(1, 'tuple');
SELECT test_setof_as_tuple(2, 'tuple');
SELECT test_setof_as_tuple(2, null);

SELECT test_setof_as_iterator(0, 'list');
SELECT test_setof_as_iterator(1, 'list');
SELECT test_setof_as_iterator(2, 'list');
SELECT test_setof_as_iterator(2, null);

SELECT test_setof_spi_in_iterator();


-- setof function with an SPI result set (used to crash because of
-- memory management issues across multiple calls)

CREATE OR REPLACE FUNCTION get_user_records()
RETURNS SETOF users
AS $$
    return plpy.execute("SELECT * FROM users ORDER BY username")
$$ LANGUAGE plpythonu;

SELECT get_user_records();
