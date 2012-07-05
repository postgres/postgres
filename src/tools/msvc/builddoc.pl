# -*-perl-*- hey - emacs - this is a perl file

# Adjust path for your docbook installation in buildenv.pl

# src/tools/msvc/builddoc.pl
# translated from an earlier .bat file

use strict;
use File::Copy;
use Cwd qw(abs_path getcwd);

my $startdir = getcwd();

my $openjade = 'openjade-1.3.1';
my $dsssl    = 'docbook-dsssl-1.79';

chdir '../../..' if (-d '../msvc' && -d '../../../src');

noversion() unless -e 'doc/src/sgml/version.sgml';

require 'src/tools/msvc/buildenv.pl' if -e 'src/tools/msvc/buildenv.pl';

my $docroot = $ENV{DOCROOT};
die "bad DOCROOT '$docroot'" unless ($docroot && -d $docroot);

my @notfound;
foreach my $dir ('docbook', $openjade, $dsssl)
{
	push(@notfound, $dir) unless -d "$docroot/$dir";
}
missing() if @notfound;

my $arg = shift;
renamefiles();

chdir 'doc/src/sgml';

$ENV{SGML_CATALOG_FILES} =
  "$docroot/$openjade/dsssl/catalog;" . "$docroot/docbook/docbook.cat";

my $cmd;

# openjade exits below with a harmless non-zero status, so we
# can't die on "failure"

$cmd =
    "perl mk_feature_tables.pl YES "
  . "../../../src/backend/catalog/sql_feature_packages.txt "
  . "../../../src/backend/catalog/sql_features.txt "
  . "> features-supported.sgml";
system($cmd);
die "features_supported" if $?;
$cmd =
    "perl mk_feature_tables.pl NO "
  . "\"../../../src/backend/catalog/sql_feature_packages.txt\" "
  . "\"../../../src/backend/catalog/sql_features.txt\" "
  . "> features-unsupported.sgml";
system($cmd);
die "features_unsupported" if $?;
$cmd =
"perl generate-errcodes-table.pl \"../../../src/backend/utils/errcodes.txt\" "
  . "> errcodes-table.sgml";
system($cmd);
die "errcodes-table" if $?;

print "Running first build...\n";
$cmd =
    "\"$docroot/$openjade/bin/openjade\" -V html-index -wall "
  . "-wno-unused-param -wno-empty -D . -c \"$docroot/$dsssl/catalog\" "
  . "-d stylesheet.dsl -i output-html -t sgml postgres.sgml 2>&1 "
  . "| findstr /V \"DTDDECL catalog entries are not supported\" ";
system($cmd);    # die "openjade" if $?;
print "Running collateindex...\n";
$cmd = "perl \"$docroot/$dsssl/bin/collateindex.pl\" -f -g -i bookindex "
  . "-o bookindex.sgml HTML.index";
system($cmd);
die "collateindex" if $?;
mkdir "html";
print "Running second build...\n";
$cmd =
    "\"$docroot/$openjade/bin/openjade\" -wall -wno-unused-param -wno-empty "
  . "-D . -c \"$docroot/$dsssl/catalog\" -d stylesheet.dsl -t sgml "
  . "-i output-html -i include-index postgres.sgml 2>&1 "
  . "| findstr /V \"DTDDECL catalog entries are not supported\" ";

system($cmd);    # die "openjade" if $?;

copy "stylesheet.css", "html/stylesheet.css";

print "Docs build complete.\n";

exit;

########################################################

sub renamefiles
{

	# Rename ISO entity files
	my $savedir = getcwd();
	chdir "$docroot/docbook";
	foreach my $f (glob('ISO*'))
	{
		next if $f =~ /\.gml$/i;
		my $nf = $f;
		$nf =~ s/ISO(.*)/ISO-$1.gml/;
		move $f, $nf;
	}
	chdir $savedir;

}

sub missing
{
	print STDERR "could not find $docroot/$_\n" foreach (@notfound);
	exit 1;
}

sub noversion
{
	print STDERR "Could not find version.sgml. ",
	  "Please run mkvcbuild.pl first!\n";
	exit 1;
}
