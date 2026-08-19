-- Test tied Perl values via handmade classes, so as not to require
-- Tie::Hash and Tie::Array; some popular platforms don't have those
-- in their base Perl installations.

DO $$
{
	package PLPerlTiedHash;
	sub TIEHASH  { bless {}, $_[0] }
	sub STORE    { $_[0]{$_[1]} = $_[2] }
	sub FETCH    { $_[0]{$_[1]} }
	sub FIRSTKEY { my $a = keys %{$_[0]}; each %{$_[0]} }
	sub NEXTKEY  { each %{$_[0]} }

	package PLPerlTiedArray;
	sub TIEARRAY  { bless [], $_[0] }
	sub STORE     { $_[0][$_[1]] = $_[2] }
	sub FETCH     { $_[0][$_[1]] }
	sub FETCHSIZE { scalar @{$_[0]} }

	package PLPerlTiedScalar;
	sub TIESCALAR { my $s; bless \$s, $_[0] }
	sub STORE     { ${$_[0]} = $_[1] }
	sub FETCH     { ${$_[0]} }
}
$$ LANGUAGE plperl;

CREATE FUNCTION tied_setof() RETURNS SETOF text AS $$
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 'a'; $a[1] = 'b'; $a[2] = 'c';
	return \@a;
$$ LANGUAGE plperl;

SELECT * FROM tied_setof();

CREATE FUNCTION tied_scalar() RETURNS text AS $$
	my $s;
	tie $s, 'PLPerlTiedScalar';
	$s = 'hello';
	return $s;
$$ LANGUAGE plperl;

SELECT tied_scalar();

CREATE TYPE tiedrowperl AS (f1 integer, f2 text, f3 text);

CREATE FUNCTION tied_composite() RETURNS tiedrowperl AS $$
	my %h;
	tie %h, 'PLPerlTiedHash';
	$h{f1} = 1; $h{f2} = 'hello'; $h{f3} = 'world';
	return \%h;
$$ LANGUAGE plperl;

SELECT tied_composite();

CREATE FUNCTION tied_int_array() RETURNS integer[] AS $$
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 1; $a[1] = 2; $a[2] = 3;
	return \@a;
$$ LANGUAGE plperl;

SELECT tied_int_array();

-- Tied scalar whose FETCH returns an arrayref.
CREATE FUNCTION tied_scalar_setof() RETURNS SETOF text AS $$
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 'x'; $a[1] = 'y';
	my $s;
	tie $s, 'PLPerlTiedScalar';
	$s = \@a;
	return $s;
$$ LANGUAGE plperl;

SELECT * FROM tied_scalar_setof();

CREATE FUNCTION tied_scalar_int_array() RETURNS integer[] AS $$
	my @a;
	tie @a, 'PLPerlTiedArray';
	$a[0] = 4; $a[1] = 5;
	my $s;
	tie $s, 'PLPerlTiedScalar';
	$s = \@a;
	return $s;
$$ LANGUAGE plperl;

SELECT tied_scalar_int_array();

-- Artificial: $_TD->{new} and the trigger return value are tied scalars.
CREATE TABLE tied_trigger_test (i int, v text);

CREATE FUNCTION tied_modify() RETURNS trigger AS $$
	my %h;
	tie %h, 'PLPerlTiedHash';
	$h{i} = $_TD->{new}{i};
	$h{v} = 'from_tie';
	my $new;
	tie $new, 'PLPerlTiedScalar';
	$new = \%h;
	$_TD->{new} = $new;
	my $ret;
	tie $ret, 'PLPerlTiedScalar';
	$ret = 'MODIFY';
	return $ret;
$$ LANGUAGE plperl;

CREATE TRIGGER tied_modify_trig BEFORE INSERT ON tied_trigger_test
FOR EACH ROW EXECUTE PROCEDURE tied_modify();

INSERT INTO tied_trigger_test (i, v) VALUES (1, 'orig');

SELECT i, v FROM tied_trigger_test;
