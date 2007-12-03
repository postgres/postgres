package Install;

#
# Package that provides 'make install' functionality for msvc builds
#
# $PostgreSQL: pgsql/src/tools/msvc/Install.pm,v 1.26 2007/12/03 15:42:58 mha Exp $
#
use strict;
use warnings;
use Carp;
use File::Basename;
use File::Copy;
use File::Find ();

use Exporter;
our (@ISA,@EXPORT_OK);
@ISA = qw(Exporter);
@EXPORT_OK = qw(Install);

sub lcopy
{
	my $src = shift;
	my $target = shift;

        if (-f $target)
        {
           unlink $target || confess "Could not delete $target\n";
        }

	copy($src,$target)
          || confess "Could not copy $src to $target\n";

}

sub Install
{
    $| = 1;

    my $target = shift;
    our $config;
    require 'config.pl';

    chdir("../../..") if (-f "../../../configure");
    chdir("../../../..") if (-f "../../../../configure");
    my $conf = "";
    if (-d "debug")
    {
        $conf = "debug";
    }
    if (-d "release")
    {
        $conf = "release";
    }
    die "Could not find debug or release binaries" if ($conf eq "");
    print "Installing for $conf in $target\n";

    EnsureDirectories($target, 'bin','lib','share','share/timezonesets','share/contrib','doc',
        'doc/contrib', 'symbols', 'share/tsearch_data');

    CopySolutionOutput($conf, $target);
    lcopy($target . '/lib/libpq.dll', $target . '/bin/libpq.dll');
	my $sample_files = [];
	File::Find::find({wanted => 
						  sub { /^.*\.sample\z/s && 
									push(@$sample_files, $File::Find::name); 
							} 
				  }, 
					 "src" );
    CopySetOfFiles('config files', $sample_files , $target . '/share/');
    CopyFiles(
        'Import libraries',
        $target .'/lib/',
        "$conf\\", "postgres\\postgres.lib","libpq\\libpq.lib", "libecpg\\libecpg.lib", "libpgport\\libpgport.lib"
    );
    CopySetOfFiles('timezone names', 
				   [ glob('src\timezone\tznames\*.txt') ] ,
				   $target . '/share/timezonesets/');
    CopyFiles(
        'timezone sets',
        $target . '/share/timezonesets/',
        'src/timezone/tznames/', 'Default','Australia','India'
    );
    CopySetOfFiles('BKI files', [ glob("src\\backend\\catalog\\postgres.*") ], 
				   $target .'/share/');
    CopySetOfFiles('SQL files', [ glob("src\\backend\\catalog\\*.sql") ], 
				   $target . '/share/');
    CopyFiles(
        'Information schema data',
        $target . '/share/',
        'src/backend/catalog/', 'sql_features.txt'
    );
    GenerateConversionScript($target);
    GenerateTimezoneFiles($target,$conf);
    GenerateTsearchFiles($target);
    CopySetOfFiles('Stopword files', 
				   [ glob ("src\\backend\\snowball\\stopwords\\*.stop") ], 
				   $target . '/share/tsearch_data/');
    CopySetOfFiles('Dictionaries sample files', 
				   [ glob ("src\\backend\\tsearch\\*_sample.*" ) ], 
				   $target . '/share/tsearch_data/');
    CopyContribFiles($config,$target);
    CopyIncludeFiles($target);

    GenerateNLSFiles($target,$config->{nls}) if ($config->{nls});

    print "Installation complete.\n";
}

sub EnsureDirectories
{
    my $target = shift;
    mkdir $target unless -d ($target);
    while (my $d = shift)
    {
        mkdir $target . '/' . $d unless -d ($target . '/' . $d);
    }
}

sub CopyFiles
{
    my $what = shift;
    my $target = shift;
    my $basedir = shift;

    print "Copying $what";
    while (my $f = shift)
    {
        print ".";
        $f = $basedir . $f;
        die "No file $f\n" if (!-f $f);
        lcopy($f, $target . basename($f));
    }
    print "\n";
}

sub CopySetOfFiles
{
    my $what = shift;
    my $flist = shift;
    my $target = shift;
    print "Copying $what" if $what;
    foreach (@$flist)
    {
        next if /regress/; # Skip temporary install in regression subdir
        next if /ecpg.test/; # Skip temporary install in regression subdir
        my $tgt = $target . basename($_);
        print ".";
        lcopy($_, $tgt) || croak "Could not copy $_: $!\n";
    }
    print "\n";
}

sub CopySolutionOutput
{
    my $conf = shift;
    my $target = shift;
    my $rem = qr{Project\("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}"\) = "([^"]+)"};

    my $sln = read_file("pgsql.sln") || croak "Could not open pgsql.sln\n";
    print "Copying build output files...";
    while ($sln =~ $rem)
    {
        my $pf = $1;
        my $dir;
        my $ext;

        $sln =~ s/$rem//;

        my $proj = read_file("$pf.vcproj") || croak "Could not open $pf.vcproj\n";
        if ($proj !~ qr{ConfigurationType="([^"]+)"})
        {
            croak "Could not parse $pf.vcproj\n";
        }
        if ($1 == 1)
        {
            $dir = "bin";
            $ext = "exe";
        }
        elsif ($1 == 2)
        {
            $dir = "lib";
            $ext = "dll";
        }
        else
        {

            # Static lib, such as libpgport, only used internally during build, don't install
            next;
        }
        lcopy("$conf\\$pf\\$pf.$ext","$target\\$dir\\$pf.$ext") || croak "Could not copy $pf.$ext\n";
        lcopy("$conf\\$pf\\$pf.pdb","$target\\symbols\\$pf.pdb") || croak "Could not copy $pf.pdb\n";
        print ".";
    }
    print "\n";
}

sub GenerateConversionScript
{
    my $target = shift;
    my $sql = "";
    my $F;

    print "Generating conversion proc script...";
    my $mf = read_file('src/backend/utils/mb/conversion_procs/Makefile');
    $mf =~ s{\\\s*[\r\n]+}{}mg;
    $mf =~ /^CONVERSIONS\s*=\s*(.*)$/m
      || die "Could not find CONVERSIONS line in conversions Makefile\n";
    my @pieces = split /\s+/,$1;
    while ($#pieces > 0)
    {
        my $name = shift @pieces;
        my $se = shift @pieces;
        my $de = shift @pieces;
        my $func = shift @pieces;
        my $obj = shift @pieces;
        $sql .= "-- $se --> $de\n";
        $sql .=
"CREATE OR REPLACE FUNCTION $func (INTEGER, INTEGER, CSTRING, INTERNAL, INTEGER) RETURNS VOID AS '\$libdir/$obj', '$func' LANGUAGE C STRICT;\n";
        $sql .= "DROP CONVERSION pg_catalog.$name;\n";
        $sql .= "CREATE DEFAULT CONVERSION pg_catalog.$name FOR '$se' TO '$de' FROM $func;\n";
    }
    open($F,">$target/share/conversion_create.sql")
      || die "Could not write to conversion_create.sql\n";
    print $F $sql;
    close($F);
    print "\n";
}

sub GenerateTimezoneFiles
{
    my $target = shift;
    my $conf = shift;
    my $mf = read_file("src/timezone/Makefile");
    $mf =~ s{\\\s*[\r\n]+}{}mg;
    $mf =~ /^TZDATA\s*:?=\s*(.*)$/m || die "Could not find TZDATA row in timezone makefile\n";
    my @tzfiles = split /\s+/,$1;
    unshift @tzfiles,'';
    print "Generating timezone files...";
    system("$conf\\zic\\zic -d \"$target/share/timezone\" " . join(" src/timezone/data/", @tzfiles));
    print "\n";
}

sub GenerateTsearchFiles
{
    my $target = shift;

    print "Generating tsearch script...";
    my $F;
    my $tmpl = read_file('src/backend/snowball/snowball.sql.in');
    my $mf = read_file('src/backend/snowball/Makefile');
    $mf =~ s{\\\s*[\r\n]+}{}mg;
    $mf =~ /^LANGUAGES\s*=\s*(.*)$/m
      || die "Could not find LANGUAGES line in snowball Makefile\n";
    my @pieces = split /\s+/,$1;
    open($F,">$target/share/snowball_create.sql")
      || die "Could not write snowball_create.sql";
    print $F read_file('src/backend/snowball/snowball_func.sql.in');
    while ($#pieces > 0)
    {
        my $lang = shift @pieces || last;
        my $asclang = shift @pieces || last;
        my $txt = $tmpl;
        my $stop = '';

        if (-s "src/backend/snowball/stopwords/$lang.stop") {
            $stop = ", StopWords=$lang";
        }

        $txt =~ s#_LANGNAME_#${lang}#gs;
        $txt =~ s#_DICTNAME_#${lang}_stem#gs;
        $txt =~ s#_CFGNAME_#${lang}#gs;
        $txt =~ s#_ASCDICTNAME_#${asclang}_stem#gs;
        $txt =~ s#_NONASCDICTNAME_#${lang}_stem#gs;
        $txt =~ s#_STOPWORDS_#$stop#gs;
        print $F $txt;
        print ".";
    }
    close($F);
    print "\n";
}

sub CopyContribFiles
{
    my $config = shift;
    my $target = shift;

    print "Copying contrib data files...";
    my $D;
    opendir($D, 'contrib') || croak "Could not opendir on contrib!\n";
    while (my $d = readdir($D))
    {
        next if ($d =~ /^\./);
        next unless (-f "contrib/$d/Makefile");
        next if ($d eq "uuid-ossp");
        next if ($d eq "sslinfo" && !defined($config->{openssl}));
        next if ($d eq "xml2" && !defined($config->{xml}));

        my $mf = read_file("contrib/$d/Makefile");
        $mf =~ s{\\s*[\r\n]+}{}mg;
        my $flist = '';
        if ($mf =~ /^DATA_built\s*=\s*(.*)$/m) {$flist .= $1}
        if ($mf =~ /^DATA\s*=\s*(.*)$/m) {$flist .= " $1"}
        $flist =~ s/^\s*//; # Remove leading spaces if we had only DATA_built

        if ($flist ne '')
        {
            $flist = ParseAndCleanRule($flist, $mf);

            # Special case for contrib/spi
            $flist = "autoinc.sql insert_username.sql moddatetime.sql refint.sql timetravel.sql"
              if ($d eq 'spi');
            foreach my $f (split /\s+/,$flist)
            {
                lcopy('contrib/' . $d . '/' . $f,$target . '/share/contrib/' . basename($f))
                  || croak("Could not copy file $f in contrib $d");
                print '.';
            }
        }

        $flist = '';
        if ($mf =~ /^DATA_TSEARCH\s*=\s*(.*)$/m) {$flist .= $1}
        if ($flist ne '')
        {
            $flist = ParseAndCleanRule($flist, $mf);

            foreach my $f (split /\s+/,$flist)
            {
                lcopy('contrib/' . $d . '/' . $f,$target . '/share/tsearch_data/' . basename($f))
                  || croak("Could not copy file $f in contrib $d");
                print '.';
            }
        }

        $flist = '';
        if ($mf =~ /^DOCS\s*=\s*(.*)$/mg) {$flist .= $1}
        if ($flist ne '')
        {
            $flist = ParseAndCleanRule($flist, $mf);

            # Special case for contrib/spi
            $flist =
"autoinc.example insert_username.example moddatetime.example refint.example timetravel.example"
              if ($d eq 'spi');
            foreach my $f (split /\s+/,$flist)
            {
                lcopy('contrib/' . $d . '/' . $f, $target . '/doc/contrib/' . $f)
                  || croak("Could not copy file $f in contrib $d");
                print '.';
            }
        }
    }
    closedir($D);
    print "\n";
}

sub ParseAndCleanRule
{
    my $flist = shift;
    my $mf = shift;

    # Strip out $(addsuffix) rules
    if (index($flist, '$(addsuffix ') >= 0)
    {
        my $pcount = 0;
        my $i;
        for ($i = index($flist, '$(addsuffix ') + 12; $i < length($flist); $i++)
        {
            $pcount++ if (substr($flist, $i, 1) eq '(');
            $pcount-- if (substr($flist, $i, 1) eq ')');
            last if ($pcount < 0);
        }
        $flist = substr($flist, 0, index($flist, '$(addsuffix ')) . substr($flist, $i+1);
    }
    return $flist;
}

sub CopyIncludeFiles
{
    my $target = shift;

    EnsureDirectories($target, 'include', 'include/libpq',
        'include/internal', 'include/internal/libpq',
        'include/server');

    CopyFiles(
        'Public headers',
        $target . '/include/',
        'src/include/', 'postgres_ext.h', 'pg_config.h', 'pg_config_os.h', 'pg_config_manual.h'
    );
    lcopy('src/include/libpq/libpq-fs.h', $target . '/include/libpq/')
      || croak 'Could not copy libpq-fs.h';

    CopyFiles('Libpq headers', $target . '/include/', 'src/interfaces/libpq/', 'libpq-fe.h');
    CopyFiles(
        'Libpq internal headers',
        $target .'/include/internal/',
        'src/interfaces/libpq/', 'libpq-int.h', 'pqexpbuffer.h'
    );

    CopyFiles(
        'Internal headers',
        $target . '/include/internal/',
        'src/include/', 'c.h', 'port.h', 'postgres_fe.h'
    );
    lcopy('src/include/libpq/pqcomm.h', $target . '/include/internal/libpq/')
      || croak 'Could not copy pqcomm.h';

    CopyFiles(
        'Server headers',
        $target . '/include/server/',
        'src/include/', 'pg_config.h', 'pg_config_os.h'
    );
    CopySetOfFiles('', 
				   [ glob( "src\\include\\*.h" ) ], 
				   $target . '/include/server/');
    my $D;
    opendir($D, 'src/include') || croak "Could not opendir on src/include!\n";

    while (my $d = readdir($D))
    {
        next if ($d =~ /^\./);
        next if ($d eq 'CVS');
        next unless (-d 'src/include/' . $d);

        EnsureDirectories($target . '/include/server', $d);
        system(
            "xcopy /s /i /q /r /y src\\include\\$d\\*.h \"$target\\include\\server\\$d\\\"")
          && croak("Failed to copy include directory $d\n");
    }
    closedir($D);

    my $mf = read_file('src/interfaces/ecpg/include/Makefile');
    $mf =~ s{\\s*[\r\n]+}{}mg;
    $mf =~ /^ecpg_headers\s*=\s*(.*)$/m || croak "Could not find ecpg_headers line\n";
    CopyFiles(
        'ECPG headers',
        $target . '/include/',
        'src/interfaces/ecpg/include/',
        'ecpg_config.h', split /\s+/,$1
    );
    $mf =~ /^informix_headers\s*=\s*(.*)$/m || croak "Could not find informix_headers line\n";
    EnsureDirectories($target . '/include', 'informix', 'informix/esql');
    CopyFiles(
        'ECPG informix headers',
        $target .'/include/informix/esql/',
        'src/interfaces/ecpg/include/',
        split /\s+/,$1
    );
}

sub GenerateNLSFiles
{
    my $target = shift;
    my $nlspath = shift;

    print "Installing NLS files...";
    EnsureDirectories($target, "share/locale");
	my @flist;
	File::Find::find({wanted => 
						  sub { /^nls\.mk\z/s && 
									!                                                                       push(@flist, $File::Find::name); 	
							} 
				  }, "src");
    foreach (@flist)
    {
        s/nls.mk/po/;
        my $dir = $_;
        next unless ($dir =~ /([^\\]+)\\po$/);
        my $prgm = $1;
        $prgm = 'postgres' if ($prgm eq 'backend');
        foreach (glob("$dir/*.po"))
        {
            my $lang;
            next unless /^(.*)\.po/;
            $lang = $1;

            EnsureDirectories($target, "share/locale/$lang", "share/locale/$lang/LC_MESSAGES");
            system(
"$nlspath\\bin\\msgfmt -o $target\\share\\locale\\$lang\\LC_MESSAGES\\$prgm.mo $dir\\$_"
              )
              && croak("Could not run msgfmt on $dir\\$_");
            print ".";
        }
    }
    print "\n";
}

sub read_file
{
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

1;
