use ExtUtils::MakeMaker;
use ExtUtils::Embed;
use DynaLoader;
use Config;

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

WriteMakefile( 'NAME' => 'plperl', 
	dynamic_lib => { 'OTHERLDFLAGS' => "$opcode -L\$(LIBPQDIR) -lpq @{[ldopts()]}" } ,
	INC => '-I$(LIBPQDIR) -I$(SRCDIR)/include -I$(SRCDIR)/backend',
	XS => { 'SPI.xs' => 'SPI.c' },
	OBJECT => 'plperl.o eloglvl.o SPI.o',
	);

sub MY::post_initialize { 

q[
SRCDIR=../../../src
include $(SRCDIR)/Makefile.global
];

}

sub MY::install {

q[
install :: all
	cp $(INST_DYNAMIC) $(LIBDIR)
];

}


