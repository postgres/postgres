# /usr/bin/perl

# doc/src/sgml/mk_feature_tables.pl

use strict;
use warnings FATAL => 'all';

my $yesno = $ARGV[0];

open my $pack, '<', $ARGV[1] or die;

my %feature_packages;

while (<$pack>)
{
	chomp;
	my ($fid, $pname) = split /\t/;
	if ($feature_packages{$fid})
	{
		$feature_packages{$fid} .= ", $pname";
	}
	else
	{
		$feature_packages{$fid} = $pname;
	}
}

close $pack;

open my $feat, '<', $ARGV[2] or die;

print "<tbody>\n";

while (<$feat>)
{
	chomp;
	my ($feature_id, $feature_name, $subfeature_id,
		$subfeature_name, $is_supported, $comments) = split /\t/;

	$is_supported eq $yesno || next;

	$feature_name =~ s/</&lt;/g;
	$feature_name =~ s/>/&gt;/g;
	$subfeature_name =~ s/</&lt;/g;
	$subfeature_name =~ s/>/&gt;/g;

	print " <row>\n";

	if ($subfeature_id)
	{
		print "  <entry>$feature_id-$subfeature_id</entry>\n";
	}
	else
	{
		print "  <entry>$feature_id</entry>\n";
	}
	print "  <entry>",
	  defined($feature_packages{$feature_id})
	  ? $feature_packages{$feature_id}
	  : "",
	  "</entry>\n";
	if ($subfeature_id)
	{
		print "  <entry>$subfeature_name</entry>\n";
	}
	else
	{
		print "  <entry>$feature_name</entry>\n";
	}
	print "  <entry>$comments</entry>\n";

	print " </row>\n";
}

print "</tbody>\n";

close $feat;
