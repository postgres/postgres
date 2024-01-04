#!/usr/bin/perl

# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# contrib/intarray/bench/create_test.pl

use strict;
use warnings FATAL => 'all';

print <<EOT;
create table message (
	mid	int not null,
	sections	int[]
);
create table message_section_map (
	mid	int not null,
	sid	int not null
);

EOT

open(my $msg, '>', "message.tmp") || die;
open(my $map, '>', "message_section_map.tmp") || die;

srand(1);

#foreach my $i ( 1..1778 ) {
#foreach my $i ( 1..3443 ) {
#foreach my $i ( 1..5000 ) {
#foreach my $i ( 1..29362 ) {
#foreach my $i ( 1..33331 ) {
#foreach my $i ( 1..83268 ) {
foreach my $i (1 .. 200000)
{
	my @sect;
	if (rand() < 0.7)
	{
		$sect[0] = int((rand()**4) * 100);
	}
	else
	{
		my %hash;
		@sect =
		  grep { $hash{$_}++; $hash{$_} <= 1 }
		  map { int((rand()**4) * 100) } 0 .. (int(rand() * 5));
	}
	if ($#sect < 0 || rand() < 0.1)
	{
		print $msg "$i\t\\N\n";
	}
	else
	{
		print $msg "$i\t{" . join(',', @sect) . "}\n";
		print $map "$i\t$_\n" foreach @sect;
	}
}
close $map;
close $msg;

copytable('message');
copytable('message_section_map');

print <<EOT;

CREATE unique index message_key on message ( mid );
--CREATE unique index message_section_map_key1 on message_section_map ( mid, sid );
CREATE unique index message_section_map_key2 on message_section_map ( sid, mid );
CREATE INDEX message_rdtree_idx on message using gist ( sections gist__int_ops );
VACUUM ANALYZE;

select count(*) from message;
select count(*) from message_section_map;



EOT


unlink 'message.tmp', 'message_section_map.tmp';

sub copytable
{
	my $t = shift;

	print "COPY $t from stdin;\n";
	open(my $fff, '<', "$t.tmp") || die;
	while (<$fff>) { print; }
	close $fff;
	print "\\.\n";
	return;
}
