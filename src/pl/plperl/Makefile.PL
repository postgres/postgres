use ExtUtils::MakeMaker;
use ExtUtils::Embed;
use DynaLoader;
use Config;

#
# Can't build a shared plperl unless libperl is shared too.
# (Actually, it would be enough if code in libperl.a is compiled
# to be position-independent, but that is hard to check for and
# seems pretty unlikely anyway.)
#
if ($Config{'useshrplib'} ne 'true') {
	open(OUT, ">Makefile") or die "Can't write Makefile: $!\n";
	print OUT <<'EndOfMakefile';
# Dummy Makefile for use when we can't build plperl

all install:
	@echo "Cannot build plperl because libperl is not a shared library; skipping it."

clean distclean:
	rm -f Makefile

.DEFAULT dep depend:

EndOfMakefile
	close(OUT);
	exit(0);
}


#
# get the location of the Opcode module
#
my $opcode = '';
{

	$modname = 'Opcode';

	my $dir;
	foreach (@INC) {
		if (-d "$_/auto/$modname") {
			$dir = "$_/auto/$modname";
			last;
		}
	}

	if (defined $dir) {
		$opcode = DynaLoader::dl_findfile("-L$dir", $modname);
	}

}

my $perllib = "-L$Config{archlibexp}/CORE -lperl";

WriteMakefile( 'NAME' => 'plperl', 
	dynamic_lib => { 'OTHERLDFLAGS' => "$opcode $perllib" } ,
	INC => '-I$(SRCDIR)/include -I$(SRCDIR)/backend $(PGSQL_INCLUDES)',
	XS => { 'SPI.xs' => 'SPI.c' },
	OBJECT => 'plperl.o eloglvl.o SPI.o',
	);

sub MY::post_initialize { 

q[
SRCDIR=../../../src
include $(SRCDIR)/Makefile.global
];

}

sub MY::cflags {
    package MY;			# so that "SUPER" works right
    my $inherited = shift->SUPER::cflags(@_);

#
# Hack for building on HPUX (probably should have a general mechanism
# for system-specific hints, but for now ...)
# The default compiler and flags on this platform is cc -Aa, which
# will reject 'long long' declarations that appear in Postgres headers.
# Need to select -Ae instead.
#
    if ($Config::Config{'osname'} eq 'hpux') {
	$inherited =~ s/-Aa/-Ae/;
    }

    $inherited;
}

sub MY::install {

q[
install :: all
	cp $(INST_DYNAMIC) $(LIBDIR)
];

}
