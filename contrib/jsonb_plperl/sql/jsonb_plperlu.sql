CREATE EXTENSION jsonb_plperlu CASCADE;


CREATE FUNCTION testHVToJsonb() RETURNS jsonb
LANGUAGE plperlu
TRANSFORM FOR TYPE jsonb
AS $$
$val = {a => 1, b => 'boo', c => undef};
return $val;
$$;

SELECT testHVToJsonb();


CREATE FUNCTION testAVToJsonb() RETURNS jsonb
LANGUAGE plperlu
TRANSFORM FOR TYPE jsonb
AS $$
$val = [{a => 1, b => 'boo', c => undef}, {d => 2}];
return $val;
$$;

SELECT testAVToJsonb();


CREATE FUNCTION testSVToJsonb() RETURNS jsonb
LANGUAGE plperlu
TRANSFORM FOR TYPE jsonb
AS $$
$val = 1;
return $val;
$$;

SELECT testSVToJsonb();


-- this revealed a bug in the original implementation
CREATE FUNCTION testRegexpResultToJsonb() RETURNS jsonb
LANGUAGE plperlu
TRANSFORM FOR TYPE jsonb
AS $$
return ('1' =~ m(0\t2));
$$;

SELECT testRegexpResultToJsonb();


CREATE FUNCTION roundtrip(val jsonb, ref text = '') RETURNS jsonb
LANGUAGE plperlu
TRANSFORM FOR TYPE jsonb
AS $$
use Data::Dumper;
$Data::Dumper::Sortkeys = 1;
$Data::Dumper::Indent = 0;
elog(INFO, Dumper($_[0]));
die 'unexpected '.(ref($_[0]) || 'not a').' reference'
    if ref($_[0]) ne $_[1];
return $_[0];
$$;


SELECT roundtrip('null') is null;
SELECT roundtrip('1');
SELECT roundtrip('1E+131071');
SELECT roundtrip('-1');
SELECT roundtrip('1.2');
SELECT roundtrip('-1.2');
SELECT roundtrip('"string"');
SELECT roundtrip('"NaN"');

SELECT roundtrip('true');
SELECT roundtrip('false');

SELECT roundtrip('[]', 'ARRAY');
SELECT roundtrip('[null, null]', 'ARRAY');
SELECT roundtrip('[1, 2, 3]', 'ARRAY');
SELECT roundtrip('[-1, 2, -3]', 'ARRAY');
SELECT roundtrip('[1.2, 2.3, 3.4]', 'ARRAY');
SELECT roundtrip('[-1.2, 2.3, -3.4]', 'ARRAY');
SELECT roundtrip('["string1", "string2"]', 'ARRAY');
SELECT roundtrip('[["string1", "string2"]]', 'ARRAY');

SELECT roundtrip('{}', 'HASH');
SELECT roundtrip('{"1": null}', 'HASH');
SELECT roundtrip('{"1": 1}', 'HASH');
SELECT roundtrip('{"1": -1}', 'HASH');
SELECT roundtrip('{"1": 1.1}', 'HASH');
SELECT roundtrip('{"1": -1.1}', 'HASH');
SELECT roundtrip('{"1": "string1"}', 'HASH');

SELECT roundtrip('{"1": {"2": [3, 4, 5]}, "2": 3}', 'HASH');


\set VERBOSITY terse \\ -- suppress cascade details
DROP EXTENSION plperlu CASCADE;
