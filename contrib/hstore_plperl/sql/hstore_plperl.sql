CREATE EXTENSION hstore_plperl CASCADE;

SELECT transforms.udt_schema, transforms.udt_name,
       routine_schema, routine_name,
       group_name, transform_type
FROM information_schema.transforms JOIN information_schema.routines
     USING (specific_catalog, specific_schema, specific_name)
ORDER BY 1, 2, 5, 6;


-- test perl -> hstore
CREATE FUNCTION test2() RETURNS hstore
LANGUAGE plperl
TRANSFORM FOR TYPE hstore
AS $$
$val = {a => 1, b => 'boo', c => undef};
return $val;
$$;

SELECT test2();


-- test perl -> hstore[]
CREATE FUNCTION test2arr() RETURNS hstore[]
LANGUAGE plperl
TRANSFORM FOR TYPE hstore
AS $$
$val = [{a => 1, b => 'boo', c => undef}, {d => 2}];
return $val;
$$;

SELECT test2arr();

-- check error cases
CREATE OR REPLACE FUNCTION test2() RETURNS hstore
LANGUAGE plperl
TRANSFORM FOR TYPE hstore
AS $$
return 42;
$$;

SELECT test2();

CREATE OR REPLACE FUNCTION test2() RETURNS hstore
LANGUAGE plperl
TRANSFORM FOR TYPE hstore
AS $$
return [1, 2];
$$;

SELECT test2();

-- test with a tied hash
CREATE FUNCTION tied_hstore() RETURNS hstore
LANGUAGE plperl
TRANSFORM FOR TYPE hstore
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
$h{a} = '1'; $h{b} = '2'; $h{c} = undef;
return \%h;
$$;

SELECT tied_hstore();


DROP FUNCTION test2();
DROP FUNCTION test2arr();
DROP FUNCTION tied_hstore();


DROP EXTENSION hstore_plperl;
DROP EXTENSION hstore;
DROP EXTENSION plperl;
