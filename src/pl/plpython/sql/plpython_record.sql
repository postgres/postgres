--
-- Test returning tuples
--

CREATE FUNCTION test_table_record_as(typ text, first text, second integer, retnull boolean) RETURNS table_record AS $$
if retnull:
	return None
if typ == 'dict':
	return { 'first': first, 'second': second, 'additionalfield': 'must not cause trouble' }
elif typ == 'tuple':
	return ( first, second )
elif typ == 'list':
	return [ first, second ]
elif typ == 'obj':
	class type_record: pass
	type_record.first = first
	type_record.second = second
	return type_record
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_type_record_as(typ text, first text, second integer, retnull boolean) RETURNS type_record AS $$
if retnull:
	return None
if typ == 'dict':
	return { 'first': first, 'second': second, 'additionalfield': 'must not cause trouble' }
elif typ == 'tuple':
	return ( first, second )
elif typ == 'list':
	return [ first, second ]
elif typ == 'obj':
	class type_record: pass
	type_record.first = first
	type_record.second = second
	return type_record
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_in_out_params(first in text, second out text) AS $$
return first + '_in_to_out';
$$ LANGUAGE plpythonu;

-- this doesn't work yet :-(
CREATE FUNCTION test_in_out_params_multi(first in text,
                                         second out text, third out text) AS $$
return first + '_record_in_to_out';
$$ LANGUAGE plpythonu;

CREATE FUNCTION test_inout_params(first inout text) AS $$
return first + '_inout';
$$ LANGUAGE plpythonu;


-- Test tuple returning functions
SELECT * FROM test_table_record_as('dict', null, null, false);
SELECT * FROM test_table_record_as('dict', 'one', null, false);
SELECT * FROM test_table_record_as('dict', null, 2, false);
SELECT * FROM test_table_record_as('dict', 'three', 3, false);
SELECT * FROM test_table_record_as('dict', null, null, true);

SELECT * FROM test_table_record_as('tuple', null, null, false);
SELECT * FROM test_table_record_as('tuple', 'one', null, false);
SELECT * FROM test_table_record_as('tuple', null, 2, false);
SELECT * FROM test_table_record_as('tuple', 'three', 3, false);
SELECT * FROM test_table_record_as('tuple', null, null, true);

SELECT * FROM test_table_record_as('list', null, null, false);
SELECT * FROM test_table_record_as('list', 'one', null, false);
SELECT * FROM test_table_record_as('list', null, 2, false);
SELECT * FROM test_table_record_as('list', 'three', 3, false);
SELECT * FROM test_table_record_as('list', null, null, true);

SELECT * FROM test_table_record_as('obj', null, null, false);
SELECT * FROM test_table_record_as('obj', 'one', null, false);
SELECT * FROM test_table_record_as('obj', null, 2, false);
SELECT * FROM test_table_record_as('obj', 'three', 3, false);
SELECT * FROM test_table_record_as('obj', null, null, true);

SELECT * FROM test_type_record_as('dict', null, null, false);
SELECT * FROM test_type_record_as('dict', 'one', null, false);
SELECT * FROM test_type_record_as('dict', null, 2, false);
SELECT * FROM test_type_record_as('dict', 'three', 3, false);
SELECT * FROM test_type_record_as('dict', null, null, true);

SELECT * FROM test_type_record_as('tuple', null, null, false);
SELECT * FROM test_type_record_as('tuple', 'one', null, false);
SELECT * FROM test_type_record_as('tuple', null, 2, false);
SELECT * FROM test_type_record_as('tuple', 'three', 3, false);
SELECT * FROM test_type_record_as('tuple', null, null, true);

SELECT * FROM test_type_record_as('list', null, null, false);
SELECT * FROM test_type_record_as('list', 'one', null, false);
SELECT * FROM test_type_record_as('list', null, 2, false);
SELECT * FROM test_type_record_as('list', 'three', 3, false);
SELECT * FROM test_type_record_as('list', null, null, true);

SELECT * FROM test_type_record_as('obj', null, null, false);
SELECT * FROM test_type_record_as('obj', 'one', null, false);
SELECT * FROM test_type_record_as('obj', null, 2, false);
SELECT * FROM test_type_record_as('obj', 'three', 3, false);
SELECT * FROM test_type_record_as('obj', null, null, true);

SELECT * FROM test_in_out_params('test_in');
-- this doesn't work yet :-(
SELECT * FROM test_in_out_params_multi('test_in');
SELECT * FROM test_inout_params('test_in');
