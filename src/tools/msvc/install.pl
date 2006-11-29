use strict;
use warnings;
use Carp;
use File::Basename;
use File::Copy;

$| = 1;

my $target = shift || Usage();

chdir("../../..") if (-f "../../../configure");
my $conf = "";
if (-d "debug") {
	$conf = "debug";
}
if (-d "release") {
	$conf = "release";
}
die "Could not find debug or release binaries" if ($conf eq "");
print "Installing for $conf\n";

EnsureDirectories ('bin','lib','share','share/timezonesets');

CopySetOfFiles('programs', "$conf\\*.exe", $target . '/bin/');
CopySetOfFiles('libraries', "$conf\\*.dll", $target . '/lib/');
copy($target . '/lib/libpq.dll', $target . '/bin/libpq.dll');
CopySetOfFiles('config files', "*.sample", $target . '/share/');
CopySetOfFiles('timezone names', 'src\timezone\tznames\*.txt', $target . '/share/timezonesets/');
CopyFiles('timezone sets', $target . '/share/timezonesets/', 'src/timezone/tznames/', 'Default','Australia','India');
CopySetOfFiles('BKI files', "src\\backend\\catalog\\postgres.*", $target .'/share/');
CopySetOfFiles('SQL files', "src\\backend\\catalog\\*.sql", $target . '/share/');
CopyFiles('Information schema data', $target . '/share/', 'src/backend/catalog/', 'sql_features.txt');
GenerateConversionScript();
GenerateTimezoneFiles();

sub Usage {
	print "Usage: install.pl <targetdir>\n";
	exit(1);
}

sub EnsureDirectories {
	mkdir $target unless -d ($target);
	while (my $d = shift) {
		mkdir $target . '/' . $d unless -d ($target . '/' . $d);
	}
}

sub CopyFiles {
	my $what = shift;
	my $target = shift;
	my $basedir = shift;

	print "Copying $what";
	while (my $f = shift) {
		print ".";
		$f = $basedir . $f;
		die "No file $f\n" if (! -f $f);
		copy($f, $target . basename($f)) || croak "Could not copy $f to $target" . basename($f) . " to $target" . basename($f) . "\n";
	}
	print "\n";
}

sub CopySetOfFiles {
	my $what = shift;
	my $spec = shift;
	my $target = shift;
	my $D;

	print "Copying $what";
	open($D, "dir /b /s $spec |") || croak "Could not list $spec\n";
	while (<$D>) {
		chomp;
		my $tgt = $target . basename($_);
		print ".";
		copy($_, $tgt) || croak "Could not copy $_\n";
	}
	close($D);
	print "\n";
}

sub GenerateConversionScript {
	my $sql = "";
	my $F;

	print "Generating conversion proc script...";
	my $mf = read_file('src/backend/utils/mb/conversion_procs/Makefile');
	$mf =~ s{\\\s*[\r\n]+}{}mg;
	$mf =~ /^CONVERSIONS\s*=\s*(.*)$/m || die "Could not find CONVERSIONS line in conversions Makefile\n";
	my @pieces = split /\s+/,$1;
	while ($#pieces > 0) {
		my $name = shift @pieces;
		my $se = shift @pieces;
		my $de = shift @pieces;
		my $func = shift @pieces;
		my $obj = shift @pieces;
		$sql .= "-- $se --> $de\n";
		$sql .= "CREATE OR REPLACE FUNCTION $func (INTEGER, INTEGER, CSTRING, INTERNAL, INTEGER) RETURNS VOID AS '\$libdir/$obj', '$func' LANGUAGE C STRICT;\n";
		$sql .= "DROP CONVERSION pg_catalog.$name;\n";
		$sql .= "CREATE DEFAULT CONVERSION pg_catalog.$name FOR '$se' TO '$de' FROM $func;\n";
	}
	open($F,">$target/share/conversion_create.sql") || die "Could not write to conversion_create.sql\n";
	print $F $sql;
	close($F);
	print "\n";
}

sub GenerateTimezoneFiles {
	my $mf = read_file("src/timezone/Makefile");
	$mf =~ s{\\\s*[\r\n]+}{}mg;
	$mf =~ /^TZDATA\s*:=\s*(.*)$/m || die "Could not find TZDATA row in timezone makefile\n";
	my @tzfiles = split /\s+/,$1;
	unshift @tzfiles,'';
	print "Generating timezone files...";
	system("$conf\\zic\\zic -d $target/share/timezone " . join(" src/timezone/data/", @tzfiles));
	print "\n";
}


sub read_file {
	my $filename = shift;
	my $F;
	my $t = $/;

	undef $/;
	open($F, $filename) || die "Could not open file $filename\n";
	my $txt = <$F>;
	close($F);
	$/ = $t;

	return $txt;
}

