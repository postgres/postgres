CREATE EXTENSION jsonb_plperl CASCADE;


CREATE FUNCTION testHVToJsonb() RETURNS jsonb
LANGUAGE plperl
TRANSFORM FOR TYPE jsonb
AS $$
$val = {a => 1, b => 'boo', c => undef};
return $val;
$$;

SELECT testHVToJsonb();


CREATE FUNCTION testAVToJsonb() RETURNS jsonb
LANGUAGE plperl
TRANSFORM FOR TYPE jsonb
AS $$
$val = [{a => 1, b => 'boo', c => undef}, {d => 2}];
return $val;
$$;

SELECT testAVToJsonb();


CREATE FUNCTION testSVToJsonb() RETURNS jsonb
LANGUAGE plperl
TRANSFORM FOR TYPE jsonb
AS $$
$val = 1;
return $val;
$$;

SELECT testSVToJsonb();


CREATE FUNCTION testUVToJsonb() RETURNS jsonb
LANGUAGE plperl
TRANSFORM FOR TYPE jsonb
as $$
$val = ~0;
return $val;
$$;

-- this might produce either 18446744073709551615 or 4294967295
SELECT testUVToJsonb() IN ('18446744073709551615'::jsonb, '4294967295'::jsonb);


-- test tied hash and array cases
CREATE FUNCTION tied_jsonb_hash() RETURNS jsonb
LANGUAGE plperl
TRANSFORM FOR TYPE jsonb
AS $$
{
	package PLPerlTiedHash;
	sub TIEHASH  { bless {}, $_[0] }
	sub STORE    { $_[0]{$_[1]} = $_[2] }
	sub FETCH    { $_[0]{$_[1]} }
	sub FIRSTKEY { my $a = keys %{$_[0]}; each %{$_[0]} }
	sub NEXTKEY  { each %{$_[0]} }
}
my %h;
tie %h, 'PLPerlTiedHash';
$h{a} = 1; $h{b} = 'boo'; $h{c} = undef;
return \%h;
$$;

SELECT tied_jsonb_hash();

CREATE FUNCTION tied_jsonb_array() RETURNS jsonb
LANGUAGE plperl TRANSFORM FOR TYPE jsonb AS $$
{
	package PLPerlTiedArray;
	sub TIEARRAY  { bless [], $_[0] }
	sub STORE     { $_[0][$_[1]] = $_[2] }
	sub FETCH     { $_[0][$_[1]] }
	sub FETCHSIZE { scalar @{$_[0]} }
}
my @a;
tie @a, 'PLPerlTiedArray';
$a[0] = 1; $a[1] = 'boo';
return \@a;
$$;

SELECT tied_jsonb_array();


-- this revealed a bug in the original implementation
CREATE FUNCTION testRegexpResultToJsonb() RETURNS jsonb
LANGUAGE plperl
TRANSFORM FOR TYPE jsonb
AS $$
return ('1' =~ m(0\t2));
$$;

SELECT testRegexpResultToJsonb();


-- this revealed a different bug
CREATE FUNCTION testTextToJsonbObject(text) RETURNS jsonb
LANGUAGE plperl
TRANSFORM FOR TYPE jsonb
AS $$
my $x = shift;
return {a => $x};
$$;

SELECT testTextToJsonbObject('abc');
SELECT testTextToJsonbObject(NULL);


CREATE FUNCTION roundtrip(val jsonb, ref text = '') RETURNS jsonb
LANGUAGE plperl
TRANSFORM FOR TYPE jsonb
AS $$
# can't use Data::Dumper, but let's at least check for unexpected ref type
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
DROP EXTENSION plperl CASCADE;
