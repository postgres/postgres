#!/usr/bin/perl

# contrib/intarray/bench/create_test.pl

use strict;
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

open(MSG, ">message.tmp")             || die;
open(MAP, ">message_section_map.tmp") || die;

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
		print MSG "$i\t\\N\n";
	}
	else
	{
		print MSG "$i\t{" . join(',', @sect) . "}\n";
		map { print MAP "$i\t$_\n" } @sect;
	}
}
close MAP;
close MSG;

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
	open(FFF, "$t.tmp") || die;
	while (<FFF>) { print; }
	close FFF;
	print "\\.\n";
}
