my @def;

die "Usage: gendef.pl <modulepath>\n" unless ($ARGV[0] =~ /\\([^\\]+$)/);
my $defname = uc $1;

if (-f "$ARGV[0]/$defname.def") {
	print "Not re-generating $defname.DEF, file already exists.\n";
	exit(0);
}

print "Generating $defname.DEF from directory $ARGV[0]\n";

while (<$ARGV[0]/*>) {
    print ".";
	open(F,"dumpbin /symbols $_|") || die "Could not open $_\n";
	while (<F>) {
		s/\(\)//g;
		next unless /^\d/;
		my @pieces = split ;
		next unless $pieces[6];
		next if ($pieces[2] eq "UNDEF");
		next unless ($pieces[4] eq "External");
		next if $pieces[6] =~ /^@/;
		next if $pieces[6] =~ /^\(/;
		next if $pieces[6] =~ /^__real/;
		next if $pieces[6] =~ /^__imp/;
		next if $pieces[6] =~ /NULL_THUNK_DATA$/;
		next if $pieces[6] =~ /^__IMPORT_DESCRIPTOR/;
		next if $pieces[6] =~ /^__NULL_IMPORT/;

		push @def, $pieces[6];
	}
	close(F);
}
print "\n";

open(DEF,">$ARGV[0]/$defname.def") || die "Could not write to $defname\n";
print DEF "EXPORTS\n";
my $i = 0;
my $last = "";
foreach my $f (sort @def) {
   next if ($f eq $last);
   $last = $f;
   $f =~ s/^_//;
   $i++;
#   print DEF "  $f \@ $i\n";  # ordinaled exports?
   print DEF "  $f\n";
}
close(DEF);
print "Generated $i symbols\n";
