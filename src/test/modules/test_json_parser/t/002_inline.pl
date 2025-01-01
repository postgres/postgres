
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test success or failure of the incremental (table-driven) JSON parser
# for a variety of small inputs.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

use File::Temp qw(tempfile);

my $dir = PostgreSQL::Test::Utils::tempdir;
my @exe;

sub test
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($name, $json, %params) = @_;
	my $chunk = length($json);

	# Test the input with chunk sizes from max(input_size, 64) down to 1

	if ($chunk > 64)
	{
		$chunk = 64;
	}

	my ($fh, $fname) = tempfile(DIR => $dir);
	print $fh "$json";
	close($fh);

	foreach my $size (reverse(1 .. $chunk))
	{
		my ($stdout, $stderr) = run_command([ @exe, "-c", $size, $fname ]);

		if (defined($params{error}))
		{
			unlike($stdout, qr/SUCCESS/,
				"$name, chunk size $size: test fails");
			like($stderr, $params{error},
				"$name, chunk size $size: correct error output");
		}
		else
		{
			like($stdout, qr/SUCCESS/,
				"$name, chunk size $size: test succeeds");
			is($stderr, "", "$name, chunk size $size: no error output");
		}
	}
}

my @exes = (
	[ "test_json_parser_incremental", ],
	[ "test_json_parser_incremental", "-o", ],
	[ "test_json_parser_incremental_shlib", ],
	[ "test_json_parser_incremental_shlib", "-o", ]);

foreach (@exes)
{
	@exe = @$_;
	note "testing executable @exe";

	test("number", "12345");
	test("string", '"hello"');
	test("false", "false");
	test("true", "true");
	test("null", "null");
	test("empty object", "{}");
	test("empty array", "[]");
	test("array with number", "[12345]");
	test("array with numbers", "[12345,67890]");
	test("array with null", "[null]");
	test("array with string", '["hello"]');
	test("array with boolean", '[false]');
	test("single pair", '{"key": "value"}');
	test("heavily nested array", "[" x 3200 . "]" x 3200);
	test("serial escapes", '"\\\\\\\\\\\\\\\\"');
	test("interrupted escapes", '"\\\\\\"\\\\\\\\\\"\\\\"');
	test("whitespace", '     ""     ');

	test("unclosed empty object",
		"{", error => qr/input string ended unexpectedly/);
	test("bad key", "{{",
		error => qr/Expected string or "}", but found "\{"/);
	test("bad key", "{{}",
		error => qr/Expected string or "}", but found "\{"/);
	test("numeric key", "{1234: 2}",
		error => qr/Expected string or "}", but found "1234"/);
	test(
		"second numeric key",
		'{"a": "a", 1234: 2}',
		error => qr/Expected string, but found "1234"/);
	test(
		"unclosed object with pair",
		'{"key": "value"',
		error => qr/input string ended unexpectedly/);
	test("missing key value",
		'{"key": }', error => qr/Expected JSON value, but found "}"/);
	test(
		"missing colon",
		'{"key" 12345}',
		error => qr/Expected ":", but found "12345"/);
	test(
		"missing comma",
		'{"key": 12345 12345}',
		error => qr/Expected "," or "}", but found "12345"/);
	test("overnested array",
		"[" x 6401, error => qr/maximum permitted depth is 6400/);
	test("overclosed array",
		"[]]", error => qr/Expected end of input, but found "]"/);
	test("unexpected token in array",
		"[ }}} ]", error => qr/Expected array element or "]", but found "}"/);
	test("junk punctuation", "[ ||| ]", error => qr/Token "|" is invalid/);
	test("missing comma in array",
		"[123 123]", error => qr/Expected "," or "]", but found "123"/);
	test("misspelled boolean", "tru", error => qr/Token "tru" is invalid/);
	test(
		"misspelled boolean in array",
		"[tru]",
		error => qr/Token "tru" is invalid/);
	test(
		"smashed top-level scalar",
		"12zz",
		error => qr/Token "12zz" is invalid/);
	test(
		"smashed scalar in array",
		"[12zz]",
		error => qr/Token "12zz" is invalid/);
	test(
		"unknown escape sequence",
		'"hello\vworld"',
		error => qr/Escape sequence "\\v" is invalid/);
	test("unescaped control",
		"\"hello\tworld\"",
		error => qr/Character with value 0x09 must be escaped/);
	test(
		"incorrect escape count",
		'"\\\\\\\\\\\\\\"',
		error => qr/Token ""\\\\\\\\\\\\\\"" is invalid/);

	# Case with three bytes: double-quote, backslash and <f5>.
	# Both invalid-token and invalid-escape are possible errors, because for
	# smaller chunk sizes the incremental parser skips the string parsing when
	# it cannot find an ending quote.
	test("incomplete UTF-8 sequence",
		"\"\\\x{F5}",
		error => qr/(Token|Escape sequence) ""?\\\x{F5}" is invalid/);
}

done_testing();
