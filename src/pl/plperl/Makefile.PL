use ExtUtils::MakeMaker;
use ExtUtils::Embed;
use DynaLoader;
use Config;

# On some platforms you can't build plperl unless libperl is a shared
# library.  (Actually, it would be enough if code in libperl.a is
# compiled to be position-independent, but that is hard to check for
# and seems pretty unlikely anyway.)  On some platforms it doesn't
# matter and they can pass in the --force flag to build anyway.
# (Having a shared libperl is still a lot better for efficiency,
# though.)

if ($Config{'useshrplib'} ne 'true' && $ARGV[0] ne '--force') {
	open(OUT, ">Makefile") or die "Can't write Makefile: $!\n";
	print OUT <<'EndOfMakefile';
# Dummy Makefile for use when we can't build plperl

all:
	@echo ""; \
	 echo "*** Cannot build PL/Perl because libperl is not a shared library." ; \
	 echo "*** You might have to rebuild your Perl installation.  Refer to"; \
	 echo "*** the documentation for details."; \
	 echo ""

install:

clean realclean:
	rm -f Makefile

EndOfMakefile
	close(OUT);
	exit(0);
}

my $ldopts=ldopts();
$ldopts=~s/$Config{ccdlflags}//;

WriteMakefile( 'NAME' => 'plperl', 
        dynamic_lib => { 'OTHERLDFLAGS' =>  $ldopts } ,
	XS => { 'SPI.xs' => 'SPI.c' },
	OBJECT => 'plperl.o eloglvl.o SPI.o',
	);


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

qq[
install :: all
	cp \$(INST_DYNAMIC) $ENV{plperl_installdir}
];

}


sub MY::xs_o {
    '';
}

sub MY::makefile {
    '';
}

# VPATH-aware version of this rule
sub MY::xs_c {
    my($self) = shift;
    return '' unless $self->needs_linking();
    '
.xs.c:
	$(PERL) -I$(PERL_ARCHLIB) -I$(PERL_LIB) $(XSUBPP) $(XSPROTOARG) $(XSUBPPARGS) $< > $@
';
}

# somebody doesn't know how to write make rules...
sub MY::c_o {
    package MY; # so that "SUPER" works right
    my $inherited = shift->SUPER::c_o(@_);
    $inherited =~ s/\$\*\.\S+/\$\</g;
    $inherited;
}
