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


-- unsupported (for now)
CREATE FUNCTION testRegexpToJsonb() RETURNS jsonb
LANGUAGE plperlu
TRANSFORM FOR TYPE jsonb
AS $$
my $a = qr/foo/;
return ($a);
$$;

SELECT testRegexpToJsonb();


-- this revealed a bug in the original implementation
CREATE FUNCTION testRegexpResultToJsonb() RETURNS jsonb
LANGUAGE plperlu
TRANSFORM FOR TYPE jsonb
AS $$
return ('1' =~ m(0\t2));
$$;

SELECT testRegexpResultToJsonb();


CREATE FUNCTION roundtrip(val jsonb) RETURNS jsonb
LANGUAGE plperlu
TRANSFORM FOR TYPE jsonb
AS $$
return $_[0];
$$;


SELECT roundtrip('null');
SELECT roundtrip('1');
SELECT roundtrip('1E+131071');
SELECT roundtrip('-1');
SELECT roundtrip('1.2');
SELECT roundtrip('-1.2');
SELECT roundtrip('"string"');
SELECT roundtrip('"NaN"');

SELECT roundtrip('true');
SELECT roundtrip('false');

SELECT roundtrip('[]');
SELECT roundtrip('[null, null]');
SELECT roundtrip('[1, 2, 3]');
SELECT roundtrip('[-1, 2, -3]');
SELECT roundtrip('[1.2, 2.3, 3.4]');
SELECT roundtrip('[-1.2, 2.3, -3.4]');
SELECT roundtrip('["string1", "string2"]');

SELECT roundtrip('{}');
SELECT roundtrip('{"1": null}');
SELECT roundtrip('{"1": 1}');
SELECT roundtrip('{"1": -1}');
SELECT roundtrip('{"1": 1.1}');
SELECT roundtrip('{"1": -1.1}');
SELECT roundtrip('{"1": "string1"}');

SELECT roundtrip('{"1": {"2": [3, 4, 5]}, "2": 3}');


\set VERBOSITY terse \\ -- suppress cascade details
DROP EXTENSION plperlu CASCADE;
