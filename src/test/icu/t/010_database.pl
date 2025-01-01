# Copyright (c) 2022-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{with_icu} ne 'yes')
{
	plan skip_all => 'ICU not supported by this build';
}

my $node1 = PostgreSQL::Test::Cluster->new('node1');
$node1->init;
$node1->start;

$node1->safe_psql('postgres',
	q{CREATE DATABASE dbicu LOCALE_PROVIDER icu LOCALE 'C' ICU_LOCALE 'en@colCaseFirst=upper' ENCODING 'UTF8' TEMPLATE template0}
);

$node1->safe_psql(
	'dbicu',
	q{
CREATE COLLATION upperfirst (provider = icu, locale = 'en@colCaseFirst=upper');
CREATE TABLE icu (def text, en text COLLATE "en-x-icu", upfirst text COLLATE upperfirst);
INSERT INTO icu VALUES ('a', 'a', 'a'), ('b', 'b', 'b'), ('A', 'A', 'A'), ('B', 'B', 'B');
});

is($node1->safe_psql('dbicu', q{SELECT icu_unicode_version() IS NOT NULL}),
	qq(t), 'ICU unicode version defined');

is( $node1->safe_psql('dbicu', q{SELECT def FROM icu ORDER BY def}),
	qq(A
a
B
b),
	'sort by database default locale');

is( $node1->safe_psql(
		'dbicu', q{SELECT def FROM icu ORDER BY def COLLATE "en-x-icu"}),
	qq(a
A
b
B),
	'sort by explicit collation standard');

is( $node1->safe_psql(
		'dbicu', q{SELECT def FROM icu ORDER BY en COLLATE upperfirst}),
	qq(A
a
B
b),
	'sort by explicit collation upper first');


# Test that LOCALE='C' works for ICU
is( $node1->psql(
		'postgres',
		q{CREATE DATABASE dbicu1 LOCALE_PROVIDER icu LOCALE 'C' TEMPLATE template0 ENCODING UTF8}
	),
	0,
	"C locale works for ICU");

# Test that LOCALE works for ICU locales if LC_COLLATE and LC_CTYPE
# are specified
is( $node1->psql(
		'postgres',
		q{CREATE DATABASE dbicu2 LOCALE_PROVIDER icu LOCALE '@colStrength=primary'
          LC_COLLATE='C' LC_CTYPE='C' TEMPLATE template0 ENCODING UTF8}
	),
	0,
	"LOCALE works for ICU locales if LC_COLLATE and LC_CTYPE are specified");

my ($ret, $stdout, $stderr) = $node1->psql('postgres',
	q{CREATE DATABASE dbicu3 LOCALE_PROVIDER builtin LOCALE 'C' TEMPLATE dbicu}
);
isnt($ret, 0, "locale provider must match template: exit code not 0");
like(
	$stderr,
	qr/ERROR:  new locale provider \(builtin\) does not match locale provider of the template database \(icu\)/,
	"locale provider must match template: error message");

done_testing();
