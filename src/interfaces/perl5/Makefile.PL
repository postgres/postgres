#-------------------------------------------------------
#
# $Id: Makefile.PL,v 1.18 2001/08/26 22:28:04 petere Exp $
#
# Copyright (c) 1997, 1998  Edmund Mergl
#
#-------------------------------------------------------

use ExtUtils::MakeMaker;
use Config;
use strict;

my $srcdir=$ENV{SRCDIR};

my %opts;

%opts = (
    NAME         => 'Pg',
    VERSION_FROM => "Pg.pm",
    OBJECT       => "Pg\$(OBJ_EXT)",
# explicit mappings required for VPATH builds
    PM           => { "$srcdir/Pg.pm" => '$(INST_LIBDIR)/Pg.pm' },
    MAN3PODS     => { "$srcdir/Pg.pm" => '$(INST_MAN3DIR)/Pg.$(MAN3EXT)' },
);


if (! -d $ENV{POSTGRES_LIB} || ! -d $ENV{POSTGRES_INCLUDE}) {

    # Check that we actually are inside the Postgres source tree
    if (! -d "../libpq") {
	die
"To install Pg separately from the Postgres distribution, you must
set environment variables POSTGRES_LIB and POSTGRES_INCLUDE to point
to where Postgres is installed (often /usr/local/pgsql/{lib,include}).\n";
    }

} else {

    # Setup for standalone installation when Postgres already is installed.

    %opts = (
        %opts,
        INC          => "-I$ENV{POSTGRES_INCLUDE}",
        LIBS         => ["-L$ENV{POSTGRES_LIB} -lpq"],
    );
}


WriteMakefile(%opts);



# Put the proper runpath into the shared object.

sub MY::dynamic_lib {
    package MY;
    my $inherited= shift->SUPER::dynamic_lib(@_);

    my $pglibdir = $ENV{PGLIBDIR};
    return $inherited if $pglibdir eq '';

    # Remove any misguided attempts to set the runpath.
    $inherited =~ s/LD_RUN_PATH=\"\$\(LD_RUN_PATH\)\" //g;
    $inherited =~ s/-R\S*//g;
    $inherited =~ s/-rpath\S*//g;

    my $rpath;
    # Note that this could be different from what Makefile.port has
    # because a different compiler/linker could be used.
    SWITCH: for ($Config::Config{'osname'}) {
	/hpux/      and $rpath = "+b $pglibdir", last;
	/freebsd/   and $rpath = "-R$pglibdir", last;
	/irix/      and $rpath = "-R$pglibdir", last;
	/linux/     and $rpath = "-Wl,-rpath,$pglibdir", last;
	/netbsd/    and $rpath = "-R$pglibdir", last;
	/openbsd/   and $rpath = "-R$pglibdir", last;
	/solaris/   and $rpath = "-R$pglibdir", last;
	/svr5/      and $rpath = "-R$pglibdir", last;
    }

    $inherited=~ s,OTHERLDFLAGS =,OTHERLDFLAGS = $rpath , if defined $rpath;
    $inherited;
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

# Delete this rule.  We can use the above one.
sub MY::xs_o {
    '';
}


# This rule tries to rebuild the Makefile from Makefile.PL.  We can do
# that better ourselves.
sub MY::makefile {
    '';
}
