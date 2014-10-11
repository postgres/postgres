-- Strings.
SELECT '""'::jsonb;				-- OK.
SELECT $$''$$::jsonb;			-- ERROR, single quotes are not allowed
SELECT '"abc"'::jsonb;			-- OK
SELECT '"abc'::jsonb;			-- ERROR, quotes not closed
SELECT '"abc
def"'::jsonb;					-- ERROR, unescaped newline in string constant
SELECT '"\n\"\\"'::jsonb;		-- OK, legal escapes
SELECT '"\v"'::jsonb;			-- ERROR, not a valid JSON escape
SELECT '"\u"'::jsonb;			-- ERROR, incomplete escape
SELECT '"\u00"'::jsonb;			-- ERROR, incomplete escape
SELECT '"\u000g"'::jsonb;		-- ERROR, g is not a hex digit
SELECT '"\u0000"'::jsonb;		-- OK, legal escape
-- use octet_length here so we don't get an odd unicode char in the
-- output
SELECT octet_length('"\uaBcD"'::jsonb::text); -- OK, uppercase and lower case both OK

-- Numbers.
SELECT '1'::jsonb;				-- OK
SELECT '0'::jsonb;				-- OK
SELECT '01'::jsonb;				-- ERROR, not valid according to JSON spec
SELECT '0.1'::jsonb;				-- OK
SELECT '9223372036854775808'::jsonb;	-- OK, even though it's too large for int8
SELECT '1e100'::jsonb;			-- OK
SELECT '1.3e100'::jsonb;			-- OK
SELECT '1f2'::jsonb;				-- ERROR
SELECT '0.x1'::jsonb;			-- ERROR
SELECT '1.3ex100'::jsonb;		-- ERROR

-- Arrays.
SELECT '[]'::jsonb;				-- OK
SELECT '[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]'::jsonb;  -- OK
SELECT '[1,2]'::jsonb;			-- OK
SELECT '[1,2,]'::jsonb;			-- ERROR, trailing comma
SELECT '[1,2'::jsonb;			-- ERROR, no closing bracket
SELECT '[1,[2]'::jsonb;			-- ERROR, no closing bracket

-- Objects.
SELECT '{}'::jsonb;				-- OK
SELECT '{"abc"}'::jsonb;			-- ERROR, no value
SELECT '{"abc":1}'::jsonb;		-- OK
SELECT '{1:"abc"}'::jsonb;		-- ERROR, keys must be strings
SELECT '{"abc",1}'::jsonb;		-- ERROR, wrong separator
SELECT '{"abc"=1}'::jsonb;		-- ERROR, totally wrong separator
SELECT '{"abc"::1}'::jsonb;		-- ERROR, another wrong separator
SELECT '{"abc":1,"def":2,"ghi":[3,4],"hij":{"klm":5,"nop":[6]}}'::jsonb; -- OK
SELECT '{"abc":1:2}'::jsonb;		-- ERROR, colon in wrong spot
SELECT '{"abc":1,3}'::jsonb;		-- ERROR, no value

-- Miscellaneous stuff.
SELECT 'true'::jsonb;			-- OK
SELECT 'false'::jsonb;			-- OK
SELECT 'null'::jsonb;			-- OK
SELECT ' true '::jsonb;			-- OK, even with extra whitespace
SELECT 'true false'::jsonb;		-- ERROR, too many values
SELECT 'true, false'::jsonb;		-- ERROR, too many values
SELECT 'truf'::jsonb;			-- ERROR, not a keyword
SELECT 'trues'::jsonb;			-- ERROR, not a keyword
SELECT ''::jsonb;				-- ERROR, no value
SELECT '    '::jsonb;			-- ERROR, no value

-- make sure jsonb is passed through json generators without being escaped
SELECT array_to_json(ARRAY [jsonb '{"a":1}', jsonb '{"b":[2,3]}']);

-- jsonb extraction functions
CREATE TEMP TABLE test_jsonb (
       json_type text,
       test_json jsonb
);

INSERT INTO test_jsonb VALUES
('scalar','"a scalar"'),
('array','["zero", "one","two",null,"four","five", [1,2,3],{"f1":9}]'),
('object','{"field1":"val1","field2":"val2","field3":null, "field4": 4, "field5": [1,2,3], "field6": {"f1":9}}');

SELECT test_json -> 'x' FROM test_jsonb WHERE json_type = 'scalar';
SELECT test_json -> 'x' FROM test_jsonb WHERE json_type = 'array';
SELECT test_json -> 'x' FROM test_jsonb WHERE json_type = 'object';
SELECT test_json -> 'field2' FROM test_jsonb WHERE json_type = 'object';

SELECT test_json ->> 'field2' FROM test_jsonb WHERE json_type = 'scalar';
SELECT test_json ->> 'field2' FROM test_jsonb WHERE json_type = 'array';
SELECT test_json ->> 'field2' FROM test_jsonb WHERE json_type = 'object';

SELECT test_json -> 2 FROM test_jsonb WHERE json_type = 'scalar';
SELECT test_json -> 2 FROM test_jsonb WHERE json_type = 'array';
SELECT test_json -> 9 FROM test_jsonb WHERE json_type = 'array';
SELECT test_json -> 2 FROM test_jsonb WHERE json_type = 'object';

SELECT test_json ->> 6 FROM test_jsonb WHERE json_type = 'array';
SELECT test_json ->> 7 FROM test_jsonb WHERE json_type = 'array';

SELECT test_json ->> 'field4' FROM test_jsonb WHERE json_type = 'object';
SELECT test_json ->> 'field5' FROM test_jsonb WHERE json_type = 'object';
SELECT test_json ->> 'field6' FROM test_jsonb WHERE json_type = 'object';

SELECT test_json ->> 2 FROM test_jsonb WHERE json_type = 'scalar';
SELECT test_json ->> 2 FROM test_jsonb WHERE json_type = 'array';
SELECT test_json ->> 2 FROM test_jsonb WHERE json_type = 'object';

SELECT jsonb_object_keys(test_json) FROM test_jsonb WHERE json_type = 'scalar';
SELECT jsonb_object_keys(test_json) FROM test_jsonb WHERE json_type = 'array';
SELECT jsonb_object_keys(test_json) FROM test_jsonb WHERE json_type = 'object';

-- nulls
SELECT (test_json->'field3') IS NULL AS expect_false FROM test_jsonb WHERE json_type = 'object';
SELECT (test_json->>'field3') IS NULL AS expect_true FROM test_jsonb WHERE json_type = 'object';
SELECT (test_json->3) IS NULL AS expect_false FROM test_jsonb WHERE json_type = 'array';
SELECT (test_json->>3) IS NULL AS expect_true FROM test_jsonb WHERE json_type = 'array';

-- corner cases
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb -> null::text;
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb -> null::int;
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb -> 1;
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb -> 'z';
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb -> '';
select '[{"b": "c"}, {"b": "cc"}]'::jsonb -> 1;
select '[{"b": "c"}, {"b": "cc"}]'::jsonb -> 3;
select '[{"b": "c"}, {"b": "cc"}]'::jsonb -> 'z';
select '{"a": "c", "b": null}'::jsonb -> 'b';
select '"foo"'::jsonb -> 1;
select '"foo"'::jsonb -> 'z';

select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb ->> null::text;
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb ->> null::int;
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb ->> 1;
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb ->> 'z';
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb ->> '';
select '[{"b": "c"}, {"b": "cc"}]'::jsonb ->> 1;
select '[{"b": "c"}, {"b": "cc"}]'::jsonb ->> 3;
select '[{"b": "c"}, {"b": "cc"}]'::jsonb ->> 'z';
select '{"a": "c", "b": null}'::jsonb ->> 'b';
select '"foo"'::jsonb ->> 1;
select '"foo"'::jsonb ->> 'z';

-- equality and inequality
SELECT '{"x":"y"}'::jsonb = '{"x":"y"}'::jsonb;
SELECT '{"x":"y"}'::jsonb = '{"x":"z"}'::jsonb;

SELECT '{"x":"y"}'::jsonb <> '{"x":"y"}'::jsonb;
SELECT '{"x":"y"}'::jsonb <> '{"x":"z"}'::jsonb;

-- containment
SELECT jsonb_contains('{"a":"b", "b":1, "c":null}', '{"a":"b"}');
SELECT jsonb_contains('{"a":"b", "b":1, "c":null}', '{"a":"b", "c":null}');
SELECT jsonb_contains('{"a":"b", "b":1, "c":null}', '{"a":"b", "g":null}');
SELECT jsonb_contains('{"a":"b", "b":1, "c":null}', '{"g":null}');
SELECT jsonb_contains('{"a":"b", "b":1, "c":null}', '{"a":"c"}');
SELECT jsonb_contains('{"a":"b", "b":1, "c":null}', '{"a":"b"}');
SELECT jsonb_contains('{"a":"b", "b":1, "c":null}', '{"a":"b", "c":"q"}');
SELECT '{"a":"b", "b":1, "c":null}'::jsonb @> '{"a":"b"}';
SELECT '{"a":"b", "b":1, "c":null}'::jsonb @> '{"a":"b", "c":null}';
SELECT '{"a":"b", "b":1, "c":null}'::jsonb @> '{"a":"b", "g":null}';
SELECT '{"a":"b", "b":1, "c":null}'::jsonb @> '{"g":null}';
SELECT '{"a":"b", "b":1, "c":null}'::jsonb @> '{"a":"c"}';
SELECT '{"a":"b", "b":1, "c":null}'::jsonb @> '{"a":"b"}';
SELECT '{"a":"b", "b":1, "c":null}'::jsonb @> '{"a":"b", "c":"q"}';

SELECT '[1,2]'::jsonb @> '[1,2,2]'::jsonb;
SELECT '[1,1,2]'::jsonb @> '[1,2,2]'::jsonb;
SELECT '[[1,2]]'::jsonb @> '[[1,2,2]]'::jsonb;
SELECT '[1,2,2]'::jsonb <@ '[1,2]'::jsonb;
SELECT '[1,2,2]'::jsonb <@ '[1,1,2]'::jsonb;
SELECT '[[1,2,2]]'::jsonb <@ '[[1,2]]'::jsonb;

SELECT jsonb_contained('{"a":"b"}', '{"a":"b", "b":1, "c":null}');
SELECT jsonb_contained('{"a":"b", "c":null}', '{"a":"b", "b":1, "c":null}');
SELECT jsonb_contained('{"a":"b", "g":null}', '{"a":"b", "b":1, "c":null}');
SELECT jsonb_contained('{"g":null}', '{"a":"b", "b":1, "c":null}');
SELECT jsonb_contained('{"a":"c"}', '{"a":"b", "b":1, "c":null}');
SELECT jsonb_contained('{"a":"b"}', '{"a":"b", "b":1, "c":null}');
SELECT jsonb_contained('{"a":"b", "c":"q"}', '{"a":"b", "b":1, "c":null}');
SELECT '{"a":"b"}'::jsonb <@ '{"a":"b", "b":1, "c":null}';
SELECT '{"a":"b", "c":null}'::jsonb <@ '{"a":"b", "b":1, "c":null}';
SELECT '{"a":"b", "g":null}'::jsonb <@ '{"a":"b", "b":1, "c":null}';
SELECT '{"g":null}'::jsonb <@ '{"a":"b", "b":1, "c":null}';
SELECT '{"a":"c"}'::jsonb <@ '{"a":"b", "b":1, "c":null}';
SELECT '{"a":"b"}'::jsonb <@ '{"a":"b", "b":1, "c":null}';
SELECT '{"a":"b", "c":"q"}'::jsonb <@ '{"a":"b", "b":1, "c":null}';
-- Raw scalar may contain another raw scalar, array may contain a raw scalar
SELECT '[5]'::jsonb @> '[5]';
SELECT '5'::jsonb @> '5';
SELECT '[5]'::jsonb @> '5';
-- But a raw scalar cannot contain an array
SELECT '5'::jsonb @> '[5]';
-- In general, one thing should always contain itself. Test array containment:
SELECT '["9", ["7", "3"], 1]'::jsonb @> '["9", ["7", "3"], 1]'::jsonb;
SELECT '["9", ["7", "3"], ["1"]]'::jsonb @> '["9", ["7", "3"], ["1"]]'::jsonb;
-- array containment string matching confusion bug
SELECT '{ "name": "Bob", "tags": [ "enim", "qui"]}'::jsonb @> '{"tags":["qu"]}';

-- array length
SELECT jsonb_array_length('[1,2,3,{"f1":1,"f2":[5,6]},4]');
SELECT jsonb_array_length('[]');
SELECT jsonb_array_length('{"f1":1,"f2":[5,6]}');
SELECT jsonb_array_length('4');

-- each
SELECT jsonb_each('{"f1":[1,2,3],"f2":{"f3":1},"f4":null}');
SELECT jsonb_each('{"a":{"b":"c","c":"b","1":"first"},"b":[1,2],"c":"cc","1":"first","n":null}'::jsonb) AS q;
SELECT * FROM jsonb_each('{"f1":[1,2,3],"f2":{"f3":1},"f4":null,"f5":99,"f6":"stringy"}') q;
SELECT * FROM jsonb_each('{"a":{"b":"c","c":"b","1":"first"},"b":[1,2],"c":"cc","1":"first","n":null}'::jsonb) AS q;

SELECT jsonb_each_text('{"f1":[1,2,3],"f2":{"f3":1},"f4":null,"f5":"null"}');
SELECT jsonb_each_text('{"a":{"b":"c","c":"b","1":"first"},"b":[1,2],"c":"cc","1":"first","n":null}'::jsonb) AS q;
SELECT * FROM jsonb_each_text('{"f1":[1,2,3],"f2":{"f3":1},"f4":null,"f5":99,"f6":"stringy"}') q;
SELECT * FROM jsonb_each_text('{"a":{"b":"c","c":"b","1":"first"},"b":[1,2],"c":"cc","1":"first","n":null}'::jsonb) AS q;

-- exists
SELECT jsonb_exists('{"a":null, "b":"qq"}', 'a');
SELECT jsonb_exists('{"a":null, "b":"qq"}', 'b');
SELECT jsonb_exists('{"a":null, "b":"qq"}', 'c');
SELECT jsonb_exists('{"a":"null", "b":"qq"}', 'a');
SELECT jsonb '{"a":null, "b":"qq"}' ? 'a';
SELECT jsonb '{"a":null, "b":"qq"}' ? 'b';
SELECT jsonb '{"a":null, "b":"qq"}' ? 'c';
SELECT jsonb '{"a":"null", "b":"qq"}' ? 'a';
-- array exists - array elements should behave as keys
SELECT count(*) from testjsonb  WHERE j->'array' ? 'bar';
-- type sensitive array exists - should return no rows (since "exists" only
-- matches strings that are either object keys or array elements)
SELECT count(*) from testjsonb  WHERE j->'array' ? '5'::text;
-- However, a raw scalar is *contained* within the array
SELECT count(*) from testjsonb  WHERE j->'array' @> '5'::jsonb;

SELECT jsonb_exists_any('{"a":null, "b":"qq"}', ARRAY['a','b']);
SELECT jsonb_exists_any('{"a":null, "b":"qq"}', ARRAY['b','a']);
SELECT jsonb_exists_any('{"a":null, "b":"qq"}', ARRAY['c','a']);
SELECT jsonb_exists_any('{"a":null, "b":"qq"}', ARRAY['c','d']);
SELECT jsonb_exists_any('{"a":null, "b":"qq"}', '{}'::text[]);
SELECT jsonb '{"a":null, "b":"qq"}' ?| ARRAY['a','b'];
SELECT jsonb '{"a":null, "b":"qq"}' ?| ARRAY['b','a'];
SELECT jsonb '{"a":null, "b":"qq"}' ?| ARRAY['c','a'];
SELECT jsonb '{"a":null, "b":"qq"}' ?| ARRAY['c','d'];
SELECT jsonb '{"a":null, "b":"qq"}' ?| '{}'::text[];

SELECT jsonb_exists_all('{"a":null, "b":"qq"}', ARRAY['a','b']);
SELECT jsonb_exists_all('{"a":null, "b":"qq"}', ARRAY['b','a']);
SELECT jsonb_exists_all('{"a":null, "b":"qq"}', ARRAY['c','a']);
SELECT jsonb_exists_all('{"a":null, "b":"qq"}', ARRAY['c','d']);
SELECT jsonb_exists_all('{"a":null, "b":"qq"}', '{}'::text[]);
SELECT jsonb '{"a":null, "b":"qq"}' ?& ARRAY['a','b'];
SELECT jsonb '{"a":null, "b":"qq"}' ?& ARRAY['b','a'];
SELECT jsonb '{"a":null, "b":"qq"}' ?& ARRAY['c','a'];
SELECT jsonb '{"a":null, "b":"qq"}' ?& ARRAY['c','d'];
SELECT jsonb '{"a":null, "b":"qq"}' ?& ARRAY['a','a', 'b', 'b', 'b'];
SELECT jsonb '{"a":null, "b":"qq"}' ?& '{}'::text[];

-- typeof
SELECT jsonb_typeof('{}') AS object;
SELECT jsonb_typeof('{"c":3,"p":"o"}') AS object;
SELECT jsonb_typeof('[]') AS array;
SELECT jsonb_typeof('["a", 1]') AS array;
SELECT jsonb_typeof('null') AS "null";
SELECT jsonb_typeof('1') AS number;
SELECT jsonb_typeof('-1') AS number;
SELECT jsonb_typeof('1.0') AS number;
SELECT jsonb_typeof('1e2') AS number;
SELECT jsonb_typeof('-1.0') AS number;
SELECT jsonb_typeof('true') AS boolean;
SELECT jsonb_typeof('false') AS boolean;
SELECT jsonb_typeof('"hello"') AS string;
SELECT jsonb_typeof('"true"') AS string;
SELECT jsonb_typeof('"1.0"') AS string;

-- extract_path, extract_path_as_text
SELECT jsonb_extract_path('{"f2":{"f3":1},"f4":{"f5":99,"f6":"stringy"}}','f4','f6');
SELECT jsonb_extract_path('{"f2":{"f3":1},"f4":{"f5":99,"f6":"stringy"}}','f2');
SELECT jsonb_extract_path('{"f2":["f3",1],"f4":{"f5":99,"f6":"stringy"}}','f2',0::text);
SELECT jsonb_extract_path('{"f2":["f3",1],"f4":{"f5":99,"f6":"stringy"}}','f2',1::text);
SELECT jsonb_extract_path_text('{"f2":{"f3":1},"f4":{"f5":99,"f6":"stringy"}}','f4','f6');
SELECT jsonb_extract_path_text('{"f2":{"f3":1},"f4":{"f5":99,"f6":"stringy"}}','f2');
SELECT jsonb_extract_path_text('{"f2":["f3",1],"f4":{"f5":99,"f6":"stringy"}}','f2',0::text);
SELECT jsonb_extract_path_text('{"f2":["f3",1],"f4":{"f5":99,"f6":"stringy"}}','f2',1::text);

-- extract_path nulls
SELECT jsonb_extract_path('{"f2":{"f3":1},"f4":{"f5":null,"f6":"stringy"}}','f4','f5') IS NULL AS expect_false;
SELECT jsonb_extract_path_text('{"f2":{"f3":1},"f4":{"f5":null,"f6":"stringy"}}','f4','f5') IS NULL AS expect_true;
SELECT jsonb_extract_path('{"f2":{"f3":1},"f4":[0,1,2,null]}','f4','3') IS NULL AS expect_false;
SELECT jsonb_extract_path_text('{"f2":{"f3":1},"f4":[0,1,2,null]}','f4','3') IS NULL AS expect_true;

-- extract_path operators
SELECT '{"f2":{"f3":1},"f4":{"f5":99,"f6":"stringy"}}'::jsonb#>array['f4','f6'];
SELECT '{"f2":{"f3":1},"f4":{"f5":99,"f6":"stringy"}}'::jsonb#>array['f2'];
SELECT '{"f2":["f3",1],"f4":{"f5":99,"f6":"stringy"}}'::jsonb#>array['f2','0'];
SELECT '{"f2":["f3",1],"f4":{"f5":99,"f6":"stringy"}}'::jsonb#>array['f2','1'];

SELECT '{"f2":{"f3":1},"f4":{"f5":99,"f6":"stringy"}}'::jsonb#>>array['f4','f6'];
SELECT '{"f2":{"f3":1},"f4":{"f5":99,"f6":"stringy"}}'::jsonb#>>array['f2'];
SELECT '{"f2":["f3",1],"f4":{"f5":99,"f6":"stringy"}}'::jsonb#>>array['f2','0'];
SELECT '{"f2":["f3",1],"f4":{"f5":99,"f6":"stringy"}}'::jsonb#>>array['f2','1'];

-- corner cases for same
select '{"a": {"b":{"c": "foo"}}}'::jsonb #> '{}';
select '[1,2,3]'::jsonb #> '{}';
select '"foo"'::jsonb #> '{}';
select '42'::jsonb #> '{}';
select 'null'::jsonb #> '{}';
select '{"a": {"b":{"c": "foo"}}}'::jsonb #> array['a'];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #> array['a', null];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #> array['a', ''];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #> array['a','b'];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #> array['a','b','c'];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #> array['a','b','c','d'];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #> array['a','z','c'];
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb #> array['a','1','b'];
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb #> array['a','z','b'];
select '[{"b": "c"}, {"b": "cc"}]'::jsonb #> array['1','b'];
select '[{"b": "c"}, {"b": "cc"}]'::jsonb #> array['z','b'];
select '[{"b": "c"}, {"b": null}]'::jsonb #> array['1','b'];
select '"foo"'::jsonb #> array['z'];
select '42'::jsonb #> array['f2'];
select '42'::jsonb #> array['0'];

select '{"a": {"b":{"c": "foo"}}}'::jsonb #>> '{}';
select '[1,2,3]'::jsonb #>> '{}';
select '"foo"'::jsonb #>> '{}';
select '42'::jsonb #>> '{}';
select 'null'::jsonb #>> '{}';
select '{"a": {"b":{"c": "foo"}}}'::jsonb #>> array['a'];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #>> array['a', null];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #>> array['a', ''];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #>> array['a','b'];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #>> array['a','b','c'];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #>> array['a','b','c','d'];
select '{"a": {"b":{"c": "foo"}}}'::jsonb #>> array['a','z','c'];
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb #>> array['a','1','b'];
select '{"a": [{"b": "c"}, {"b": "cc"}]}'::jsonb #>> array['a','z','b'];
select '[{"b": "c"}, {"b": "cc"}]'::jsonb #>> array['1','b'];
select '[{"b": "c"}, {"b": "cc"}]'::jsonb #>> array['z','b'];
select '[{"b": "c"}, {"b": null}]'::jsonb #>> array['1','b'];
select '"foo"'::jsonb #>> array['z'];
select '42'::jsonb #>> array['f2'];
select '42'::jsonb #>> array['0'];

-- array_elements
SELECT jsonb_array_elements('[1,true,[1,[2,3]],null,{"f1":1,"f2":[7,8,9]},false]');
SELECT * FROM jsonb_array_elements('[1,true,[1,[2,3]],null,{"f1":1,"f2":[7,8,9]},false]') q;
SELECT jsonb_array_elements_text('[1,true,[1,[2,3]],null,{"f1":1,"f2":[7,8,9]},false,"stringy"]');
SELECT * FROM jsonb_array_elements_text('[1,true,[1,[2,3]],null,{"f1":1,"f2":[7,8,9]},false,"stringy"]') q;

-- populate_record
CREATE TYPE jbpop AS (a text, b int, c timestamp);

SELECT * FROM jsonb_populate_record(NULL::jbpop,'{"a":"blurfl","x":43.2}') q;
SELECT * FROM jsonb_populate_record(row('x',3,'2012-12-31 15:30:56')::jbpop,'{"a":"blurfl","x":43.2}') q;

SELECT * FROM jsonb_populate_record(NULL::jbpop,'{"a":"blurfl","x":43.2}') q;
SELECT * FROM jsonb_populate_record(row('x',3,'2012-12-31 15:30:56')::jbpop,'{"a":"blurfl","x":43.2}') q;

SELECT * FROM jsonb_populate_record(NULL::jbpop,'{"a":[100,200,false],"x":43.2}') q;
SELECT * FROM jsonb_populate_record(row('x',3,'2012-12-31 15:30:56')::jbpop,'{"a":[100,200,false],"x":43.2}') q;
SELECT * FROM jsonb_populate_record(row('x',3,'2012-12-31 15:30:56')::jbpop,'{"c":[100,200,false],"x":43.2}') q;

-- populate_recordset
SELECT * FROM jsonb_populate_recordset(NULL::jbpop,'[{"a":"blurfl","x":43.2},{"b":3,"c":"2012-01-20 10:42:53"}]') q;
SELECT * FROM jsonb_populate_recordset(row('def',99,NULL)::jbpop,'[{"a":"blurfl","x":43.2},{"b":3,"c":"2012-01-20 10:42:53"}]') q;
SELECT * FROM jsonb_populate_recordset(NULL::jbpop,'[{"a":"blurfl","x":43.2},{"b":3,"c":"2012-01-20 10:42:53"}]') q;
SELECT * FROM jsonb_populate_recordset(row('def',99,NULL)::jbpop,'[{"a":"blurfl","x":43.2},{"b":3,"c":"2012-01-20 10:42:53"}]') q;
SELECT * FROM jsonb_populate_recordset(row('def',99,NULL)::jbpop,'[{"a":[100,200,300],"x":43.2},{"a":{"z":true},"b":3,"c":"2012-01-20 10:42:53"}]') q;
SELECT * FROM jsonb_populate_recordset(row('def',99,NULL)::jbpop,'[{"c":[100,200,300],"x":43.2},{"a":{"z":true},"b":3,"c":"2012-01-20 10:42:53"}]') q;

SELECT * FROM jsonb_populate_recordset(NULL::jbpop,'[{"a":"blurfl","x":43.2},{"b":3,"c":"2012-01-20 10:42:53"}]') q;
SELECT * FROM jsonb_populate_recordset(row('def',99,NULL)::jbpop,'[{"a":"blurfl","x":43.2},{"b":3,"c":"2012-01-20 10:42:53"}]') q;
SELECT * FROM jsonb_populate_recordset(row('def',99,NULL)::jbpop,'[{"a":[100,200,300],"x":43.2},{"a":{"z":true},"b":3,"c":"2012-01-20 10:42:53"}]') q;

-- handling of unicode surrogate pairs

SELECT octet_length((jsonb '{ "a":  "\ud83d\ude04\ud83d\udc36" }' -> 'a')::text) AS correct_in_utf8;
SELECT jsonb '{ "a":  "\ud83d\ud83d" }' -> 'a'; -- 2 high surrogates in a row
SELECT jsonb '{ "a":  "\ude04\ud83d" }' -> 'a'; -- surrogates in wrong order
SELECT jsonb '{ "a":  "\ud83dX" }' -> 'a'; -- orphan high surrogate
SELECT jsonb '{ "a":  "\ude04X" }' -> 'a'; -- orphan low surrogate

-- handling of simple unicode escapes
SELECT jsonb '{ "a":  "the Copyright \u00a9 sign" }' ->> 'a' AS correct_in_utf8;
SELECT jsonb '{ "a":  "dollar \u0024 character" }' ->> 'a' AS correct_everyWHERE;
SELECT jsonb '{ "a":  "null \u0000 escape" }' ->> 'a' AS not_unescaped;

-- jsonb_to_record and jsonb_to_recordset

select * from jsonb_to_record('{"a":1,"b":"foo","c":"bar"}')
    as x(a int, b text, d text);

select * from jsonb_to_recordset('[{"a":1,"b":"foo","d":false},{"a":2,"b":"bar","c":true}]')
    as x(a int, b text, c boolean);

-- indexing
SELECT count(*) FROM testjsonb WHERE j @> '{"wait":null}';
SELECT count(*) FROM testjsonb WHERE j @> '{"wait":"CC"}';
SELECT count(*) FROM testjsonb WHERE j @> '{"wait":"CC", "public":true}';
SELECT count(*) FROM testjsonb WHERE j @> '{"age":25}';
SELECT count(*) FROM testjsonb WHERE j @> '{"age":25.0}';
SELECT count(*) FROM testjsonb WHERE j ? 'public';
SELECT count(*) FROM testjsonb WHERE j ? 'bar';
SELECT count(*) FROM testjsonb WHERE j ?| ARRAY['public','disabled'];
SELECT count(*) FROM testjsonb WHERE j ?& ARRAY['public','disabled'];

CREATE INDEX jidx ON testjsonb USING gin (j);
SET enable_seqscan = off;

SELECT count(*) FROM testjsonb WHERE j @> '{"wait":null}';
SELECT count(*) FROM testjsonb WHERE j @> '{"wait":"CC"}';
SELECT count(*) FROM testjsonb WHERE j @> '{"wait":"CC", "public":true}';
SELECT count(*) FROM testjsonb WHERE j @> '{"age":25}';
SELECT count(*) FROM testjsonb WHERE j @> '{"age":25.0}';
SELECT count(*) FROM testjsonb WHERE j @> '{"array":["foo"]}';
SELECT count(*) FROM testjsonb WHERE j @> '{"array":["bar"]}';
-- excercise GIN_SEARCH_MODE_ALL
SELECT count(*) FROM testjsonb WHERE j @> '{}';
SELECT count(*) FROM testjsonb WHERE j ? 'public';
SELECT count(*) FROM testjsonb WHERE j ? 'bar';
SELECT count(*) FROM testjsonb WHERE j ?| ARRAY['public','disabled'];
SELECT count(*) FROM testjsonb WHERE j ?& ARRAY['public','disabled'];

-- array exists - array elements should behave as keys (for GIN index scans too)
CREATE INDEX jidx_array ON testjsonb USING gin((j->'array'));
SELECT count(*) from testjsonb  WHERE j->'array' ? 'bar';
-- type sensitive array exists - should return no rows (since "exists" only
-- matches strings that are either object keys or array elements)
SELECT count(*) from testjsonb  WHERE j->'array' ? '5'::text;
-- However, a raw scalar is *contained* within the array
SELECT count(*) from testjsonb  WHERE j->'array' @> '5'::jsonb;

RESET enable_seqscan;

SELECT count(*) FROM (SELECT (jsonb_each(j)).key FROM testjsonb) AS wow;
SELECT key, count(*) FROM (SELECT (jsonb_each(j)).key FROM testjsonb) AS wow GROUP BY key ORDER BY count DESC, key;

-- sort/hash
SELECT count(distinct j) FROM testjsonb;
SET enable_hashagg = off;
SELECT count(*) FROM (SELECT j FROM (SELECT * FROM testjsonb UNION ALL SELECT * FROM testjsonb) js GROUP BY j) js2;
SET enable_hashagg = on;
SET enable_sort = off;
SELECT count(*) FROM (SELECT j FROM (SELECT * FROM testjsonb UNION ALL SELECT * FROM testjsonb) js GROUP BY j) js2;
SELECT distinct * FROM (values (jsonb '{}' || ''),('{}')) v(j);
SET enable_sort = on;

RESET enable_hashagg;
RESET enable_sort;

DROP INDEX jidx;
DROP INDEX jidx_array;
-- btree
CREATE INDEX jidx ON testjsonb USING btree (j);
SET enable_seqscan = off;

SELECT count(*) FROM testjsonb WHERE j > '{"p":1}';
SELECT count(*) FROM testjsonb WHERE j = '{"pos":98, "line":371, "node":"CBA", "indexed":true}';

--gin path opclass
DROP INDEX jidx;
CREATE INDEX jidx ON testjsonb USING gin (j jsonb_path_ops);
SET enable_seqscan = off;

SELECT count(*) FROM testjsonb WHERE j @> '{"wait":null}';
SELECT count(*) FROM testjsonb WHERE j @> '{"wait":"CC"}';
SELECT count(*) FROM testjsonb WHERE j @> '{"wait":"CC", "public":true}';
SELECT count(*) FROM testjsonb WHERE j @> '{"age":25}';
SELECT count(*) FROM testjsonb WHERE j @> '{"age":25.0}';
-- excercise GIN_SEARCH_MODE_ALL
SELECT count(*) FROM testjsonb WHERE j @> '{}';

RESET enable_seqscan;
DROP INDEX jidx;

-- nested tests
SELECT '{"ff":{"a":12,"b":16}}'::jsonb;
SELECT '{"ff":{"a":12,"b":16},"qq":123}'::jsonb;
SELECT '{"aa":["a","aaa"],"qq":{"a":12,"b":16,"c":["c1","c2"],"d":{"d1":"d1","d2":"d2","d1":"d3"}}}'::jsonb;
SELECT '{"aa":["a","aaa"],"qq":{"a":"12","b":"16","c":["c1","c2"],"d":{"d1":"d1","d2":"d2"}}}'::jsonb;
SELECT '{"aa":["a","aaa"],"qq":{"a":"12","b":"16","c":["c1","c2",["c3"],{"c4":4}],"d":{"d1":"d1","d2":"d2"}}}'::jsonb;
SELECT '{"ff":["a","aaa"]}'::jsonb;

SELECT
  '{"ff":{"a":12,"b":16},"qq":123,"x":[1,2],"Y":null}'::jsonb -> 'ff',
  '{"ff":{"a":12,"b":16},"qq":123,"x":[1,2],"Y":null}'::jsonb -> 'qq',
  ('{"ff":{"a":12,"b":16},"qq":123,"x":[1,2],"Y":null}'::jsonb -> 'Y') IS NULL AS f,
  ('{"ff":{"a":12,"b":16},"qq":123,"x":[1,2],"Y":null}'::jsonb ->> 'Y') IS NULL AS t,
   '{"ff":{"a":12,"b":16},"qq":123,"x":[1,2],"Y":null}'::jsonb -> 'x';

-- nested containment
SELECT '{"a":[1,2],"c":"b"}'::jsonb @> '{"a":[1,2]}';
SELECT '{"a":[2,1],"c":"b"}'::jsonb @> '{"a":[1,2]}';
SELECT '{"a":{"1":2},"c":"b"}'::jsonb @> '{"a":[1,2]}';
SELECT '{"a":{"2":1},"c":"b"}'::jsonb @> '{"a":[1,2]}';
SELECT '{"a":{"1":2},"c":"b"}'::jsonb @> '{"a":{"1":2}}';
SELECT '{"a":{"2":1},"c":"b"}'::jsonb @> '{"a":{"1":2}}';
SELECT '["a","b"]'::jsonb @> '["a","b","c","b"]';
SELECT '["a","b","c","b"]'::jsonb @> '["a","b"]';
SELECT '["a","b","c",[1,2]]'::jsonb @> '["a",[1,2]]';
SELECT '["a","b","c",[1,2]]'::jsonb @> '["b",[1,2]]';

SELECT '{"a":[1,2],"c":"b"}'::jsonb @> '{"a":[1]}';
SELECT '{"a":[1,2],"c":"b"}'::jsonb @> '{"a":[2]}';
SELECT '{"a":[1,2],"c":"b"}'::jsonb @> '{"a":[3]}';

SELECT '{"a":[1,2,{"c":3,"x":4}],"c":"b"}'::jsonb @> '{"a":[{"c":3}]}';
SELECT '{"a":[1,2,{"c":3,"x":4}],"c":"b"}'::jsonb @> '{"a":[{"x":4}]}';
SELECT '{"a":[1,2,{"c":3,"x":4}],"c":"b"}'::jsonb @> '{"a":[{"x":4},3]}';
SELECT '{"a":[1,2,{"c":3,"x":4}],"c":"b"}'::jsonb @> '{"a":[{"x":4},1]}';

-- nested object field / array index lookup
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb -> 'n';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb -> 'a';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb -> 'b';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb -> 'c';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb -> 'd';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb -> 'd' -> '1';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb -> 'e';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb -> 0; --expecting error

SELECT '["a","b","c",[1,2],null]'::jsonb -> 0;
SELECT '["a","b","c",[1,2],null]'::jsonb -> 1;
SELECT '["a","b","c",[1,2],null]'::jsonb -> 2;
SELECT '["a","b","c",[1,2],null]'::jsonb -> 3;
SELECT '["a","b","c",[1,2],null]'::jsonb -> 3 -> 1;
SELECT '["a","b","c",[1,2],null]'::jsonb -> 4;
SELECT '["a","b","c",[1,2],null]'::jsonb -> 5;
SELECT '["a","b","c",[1,2],null]'::jsonb -> -1;

--nested path extraction
SELECT '{"a":"b","c":[1,2,3]}'::jsonb #> '{0}';
SELECT '{"a":"b","c":[1,2,3]}'::jsonb #> '{a}';
SELECT '{"a":"b","c":[1,2,3]}'::jsonb #> '{c}';
SELECT '{"a":"b","c":[1,2,3]}'::jsonb #> '{c,0}';
SELECT '{"a":"b","c":[1,2,3]}'::jsonb #> '{c,1}';
SELECT '{"a":"b","c":[1,2,3]}'::jsonb #> '{c,2}';
SELECT '{"a":"b","c":[1,2,3]}'::jsonb #> '{c,3}';
SELECT '{"a":"b","c":[1,2,3]}'::jsonb #> '{c,-1}';

SELECT '[0,1,2,[3,4],{"5":"five"}]'::jsonb #> '{0}';
SELECT '[0,1,2,[3,4],{"5":"five"}]'::jsonb #> '{3}';
SELECT '[0,1,2,[3,4],{"5":"five"}]'::jsonb #> '{4}';
SELECT '[0,1,2,[3,4],{"5":"five"}]'::jsonb #> '{4,5}';

--nested exists
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb ? 'n';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb ? 'a';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb ? 'b';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb ? 'c';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb ? 'd';
SELECT '{"n":null,"a":1,"b":[1,2],"c":{"1":2},"d":{"1":[2,3]}}'::jsonb ? 'e';
