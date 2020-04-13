package Project;

#
# Package that encapsulates a Visual C++ project file generation
#
# src/tools/msvc/Project.pm
#
use Carp;
use strict;
use warnings;
use File::Basename;

sub _new
{
	my ($classname, $name, $type, $solution) = @_;
	my $good_types = {
		lib => 1,
		exe => 1,
		dll => 1,
	};
	confess("Bad project type: $type\n") unless exists $good_types->{$type};
	my $self = {
		name                  => $name,
		type                  => $type,
		guid                  => $^O eq "MSWin32" ? Win32::GuidGen() : 'FAKE',
		files                 => {},
		references            => [],
		libraries             => [],
		suffixlib             => [],
		includes              => '',
		prefixincludes        => '',
		defines               => ';',
		solution              => $solution,
		disablewarnings       => '4018;4244;4273;4102;4090;4267',
		disablelinkerwarnings => '',
		platform              => $solution->{platform},
	};

	bless($self, $classname);
	return $self;
}

sub AddFile
{
	my ($self, $filename) = @_;

	$self->{files}->{$filename} = 1;
	return;
}

sub AddFiles
{
	my $self = shift;
	my $dir  = shift;

	while (my $f = shift)
	{
		$self->{files}->{ $dir . "/" . $f } = 1;
	}
	return;
}

sub ReplaceFile
{
	my ($self, $filename, $newname) = @_;
	my $re = "\\/$filename\$";

	foreach my $file (keys %{ $self->{files} })
	{

		# Match complete filename
		if ($filename =~ m!/!)
		{
			if ($file eq $filename)
			{
				delete $self->{files}{$file};
				$self->{files}{$newname} = 1;
				return;
			}
		}
		elsif ($file =~ m/($re)/)
		{
			delete $self->{files}{$file};
			$self->{files}{"$newname/$filename"} = 1;
			return;
		}
	}
	confess("Could not find file $filename to replace\n");
}

sub RemoveFile
{
	my ($self, $filename) = @_;
	my $orig = scalar keys %{ $self->{files} };
	delete $self->{files}->{$filename};
	if ($orig > scalar keys %{ $self->{files} })
	{
		return;
	}
	confess("Could not find file $filename to remove\n");
}

sub RelocateFiles
{
	my ($self, $targetdir, $proc) = @_;
	foreach my $f (keys %{ $self->{files} })
	{
		my $r = &$proc($f);
		if ($r)
		{
			$self->RemoveFile($f);
			$self->AddFile($targetdir . '/' . basename($f));
		}
	}
	return;
}

sub AddReference
{
	my $self = shift;

	while (my $ref = shift)
	{
		push @{ $self->{references} }, $ref;
		$self->AddLibrary(
			"__CFGNAME__/" . $ref->{name} . "/" . $ref->{name} . ".lib");
	}
	return;
}

sub AddLibrary
{
	my ($self, $lib, $dbgsuffix) = @_;

	# quote lib name if it has spaces and isn't already quoted
	if ($lib =~ m/\s/ && $lib !~ m/^[&]quot;/)
	{
		$lib = '&quot;' . $lib . "&quot;";
	}

	push @{ $self->{libraries} }, $lib;
	if ($dbgsuffix)
	{
		push @{ $self->{suffixlib} }, $lib;
	}
	return;
}

sub AddIncludeDir
{
	my ($self, $inc) = @_;

	if ($self->{includes} ne '')
	{
		$self->{includes} .= ';';
	}
	$self->{includes} .= $inc;
	return;
}

sub AddPrefixInclude
{
	my ($self, $inc) = @_;

	$self->{prefixincludes} = $inc . ';' . $self->{prefixincludes};
	return;
}

sub AddDefine
{
	my ($self, $def) = @_;

	$def =~ s/"/&quot;&quot;/g;
	$self->{defines} .= $def . ';';
	return;
}

sub FullExportDLL
{
	my ($self, $libname) = @_;

	$self->{builddef} = 1;
	$self->{def}      = "./__CFGNAME__/$self->{name}/$self->{name}.def";
	$self->{implib}   = "__CFGNAME__/$self->{name}/$libname";
	return;
}

sub UseDef
{
	my ($self, $def) = @_;

	$self->{def} = $def;
	return;
}

sub AddDir
{
	my ($self, $reldir) = @_;
	my $mf = read_makefile($reldir);

	$mf =~ s{\\\r?\n}{}g;
	if ($mf =~ m{^(?:SUB)?DIRS[^=]*=\s*(.*)$}mg)
	{
		foreach my $subdir (split /\s+/, $1)
		{
			next
			  if $subdir eq "\$(top_builddir)/src/timezone"
			  ;    #special case for non-standard include
			next
			  if $reldir . "/" . $subdir eq "src/backend/port/darwin";

			$self->AddDir($reldir . "/" . $subdir);
		}
	}
	while ($mf =~ m{^(?:EXTRA_)?OBJS[^=]*=\s*(.*)$}m)
	{
		my $s         = $1;
		my $filter_re = qr{\$\(filter ([^,]+),\s+\$\(([^\)]+)\)\)};
		while ($s =~ /$filter_re/)
		{

			# Process $(filter a b c, $(VAR)) expressions
			my $list   = $1;
			my $filter = $2;
			$list =~ s/\.o/\.c/g;
			my @pieces = split /\s+/, $list;
			my $matches = "";
			foreach my $p (@pieces)
			{

				if ($filter eq "LIBOBJS")
				{
					no warnings qw(once);
					if (grep(/$p/, @main::pgportfiles) == 1)
					{
						$p =~ s/\.c/\.o/;
						$matches .= $p . " ";
					}
				}
				else
				{
					confess "Unknown filter $filter\n";
				}
			}
			$s =~ s/$filter_re/$matches/;
		}
		foreach my $f (split /\s+/, $s)
		{
			next if $f =~ /^\s*$/;
			next if $f eq "\\";
			next if $f =~ /\/SUBSYS.o$/;
			$f =~ s/,$//
			  ;    # Remove trailing comma that can show up from filter stuff
			next unless $f =~ /.*\.o$/;
			$f =~ s/\.o$/\.c/;
			if ($f =~ /^\$\(top_builddir\)\/(.*)/)
			{
				$f = $1;
				$self->{files}->{$f} = 1;
			}
			else
			{
				$self->{files}->{"$reldir/$f"} = 1;
			}
		}
		$mf =~ s{OBJS[^=]*=\s*(.*)$}{}m;
	}

	# Match rules that pull in source files from different directories, eg
	# pgstrcasecmp.c rint.c snprintf.c: % : $(top_srcdir)/src/port/%
	my $replace_re =
	  qr{^([^:\n\$]+\.c)\s*:\s*(?:%\s*: )?\$(\([^\)]+\))\/(.*)\/[^\/]+\n}m;
	while ($mf =~ m{$replace_re}m)
	{
		my $match  = $1;
		my $top    = $2;
		my $target = $3;
		my @pieces = split /\s+/, $match;
		foreach my $fn (@pieces)
		{
			if ($top eq "(top_srcdir)")
			{
				eval { $self->ReplaceFile($fn, $target) };
			}
			elsif ($top eq "(backend_src)")
			{
				eval { $self->ReplaceFile($fn, "src/backend/$target") };
			}
			else
			{
				confess "Bad replacement top: $top, on line $_\n";
			}
		}
		$mf =~ s{$replace_re}{}m;
	}

	$self->AddDirResourceFile($reldir);
	return;
}

# If the directory's Makefile bears a description string, add a resource file.
sub AddDirResourceFile
{
	my ($self, $reldir) = @_;
	my $mf = read_makefile($reldir);

	if ($mf =~ /^PGFILEDESC\s*=\s*\"([^\"]+)\"/m)
	{
		my $desc = $1;
		my $ico;
		if ($mf =~ /^PGAPPICON\s*=\s*(.*)$/m) { $ico = $1; }
		$self->AddResourceFile($reldir, $desc, $ico);
	}
	return;
}

sub AddResourceFile
{
	my ($self, $dir, $desc, $ico) = @_;

	my ($sec, $min, $hour, $mday, $mon, $year, $wday, $yday, $isdst) =
	  localtime(time);
	my $d = sprintf("%02d%03d", ($year - 100), $yday);

	if (Solution::IsNewer("$dir/win32ver.rc", 'src/port/win32ver.rc'))
	{
		print "Generating win32ver.rc for $dir\n";
		open(my $i, '<', 'src/port/win32ver.rc')
		  || confess "Could not open win32ver.rc";
		open(my $o, '>', "$dir/win32ver.rc")
		  || confess "Could not write win32ver.rc";
		my $icostr = $ico ? "IDI_ICON ICON \"src/port/$ico.ico\"" : "";
		while (<$i>)
		{
			s/FILEDESC/"$desc"/gm;
			s/_ICO_/$icostr/gm;
			s/(VERSION.*),0/$1,$d/;
			if ($self->{type} eq "dll")
			{
				s/VFT_APP/VFT_DLL/gm;
				my $name = $self->{name};
				s/_INTERNAL_NAME_/"$name"/;
				s/_ORIGINAL_NAME_/"$name.dll"/;
			}
			else
			{
				/_INTERNAL_NAME_/ && next;
				/_ORIGINAL_NAME_/ && next;
			}
			print $o $_;
		}
		close($o);
		close($i);
	}
	$self->AddFile("$dir/win32ver.rc");
	return;
}

sub DisableLinkerWarnings
{
	my ($self, $warnings) = @_;

	$self->{disablelinkerwarnings} .= ','
	  unless ($self->{disablelinkerwarnings} eq '');
	$self->{disablelinkerwarnings} .= $warnings;
	return;
}

sub Save
{
	my ($self) = @_;

	# If doing DLL and haven't specified a DEF file, do a full export of all symbols
	# in the project.
	if ($self->{type} eq "dll" && !$self->{def})
	{
		$self->FullExportDLL($self->{name} . ".lib");
	}

	# Warning 4197 is about double exporting, disable this per
	# http://connect.microsoft.com/VisualStudio/feedback/ViewFeedback.aspx?FeedbackID=99193
	$self->DisableLinkerWarnings('4197') if ($self->{platform} eq 'x64');

	# Dump the project
	open(my $f, '>', "$self->{name}$self->{filenameExtension}")
	  || croak(
		"Could not write to $self->{name}$self->{filenameExtension}\n");
	$self->WriteHeader($f);
	$self->WriteFiles($f);
	$self->Footer($f);
	close($f);
	return;
}

sub GetAdditionalLinkerDependencies
{
	my ($self, $cfgname, $separator) = @_;
	my $libcfg = (uc $cfgname eq "RELEASE") ? "MD" : "MDd";
	my $libs = '';
	foreach my $lib (@{ $self->{libraries} })
	{
		my $xlib = $lib;
		foreach my $slib (@{ $self->{suffixlib} })
		{
			if ($slib eq $lib)
			{
				$xlib =~ s/\.lib$/$libcfg.lib/;
				last;
			}
		}
		$libs .= $xlib . $separator;
	}
	$libs =~ s/.$//;
	$libs =~ s/__CFGNAME__/$cfgname/g;
	return $libs;
}

# Utility function that loads a complete file
sub read_file
{
	my $filename = shift;
	my $F;
	local $/ = undef;
	open($F, '<', $filename) || croak "Could not open file $filename\n";
	my $txt = <$F>;
	close($F);

	return $txt;
}

sub read_makefile
{
	my $reldir = shift;
	my $F;
	local $/ = undef;
	open($F, '<', "$reldir/GNUmakefile")
	  || open($F, '<', "$reldir/Makefile")
	  || confess "Could not open $reldir/Makefile\n";
	my $txt = <$F>;
	close($F);

	return $txt;
}

1;
