package Solution;

#
# Package that encapsulates a Visual C++ solution file generation
#
# src/tools/msvc/Solution.pm
#
use Carp;
use strict;
use warnings;
use VSObjectFactory;

no warnings qw(redefine);    ## no critic

sub _new
{
	my $classname = shift;
	my $options   = shift;
	my $self      = {
		projects                   => {},
		options                    => $options,
		VisualStudioVersion        => undef,
		MinimumVisualStudioVersion => undef,
		vcver                      => undef,
		platform                   => undef,
	};
	bless($self, $classname);

	$self->DeterminePlatform();

	if ($options->{xslt} && !$options->{xml})
	{
		die "XSLT requires XML\n";
	}
	$options->{blocksize} = 8
	  unless $options->{blocksize};    # undef or 0 means default
	die "Bad blocksize $options->{blocksize}"
	  unless grep { $_ == $options->{blocksize} } (1, 2, 4, 8, 16, 32);
	$options->{segsize} = 1
	  unless $options->{segsize};      # undef or 0 means default
	 # only allow segsize 1 for now, as we can't do large files yet in windows
	die "Bad segsize $options->{segsize}"
	  unless $options->{segsize} == 1;
	$options->{wal_blocksize} = 8
	  unless $options->{wal_blocksize};    # undef or 0 means default
	die "Bad wal_blocksize $options->{wal_blocksize}"
	  unless grep { $_ == $options->{wal_blocksize} }
	  (1, 2, 4, 8, 16, 32, 64);

	return $self;
}

sub GetAdditionalHeaders
{
	return '';
}

sub DeterminePlatform
{
	my $self = shift;

	if ($^O eq "MSWin32")
	{
		# Examine CL help output to determine if we are in 32 or 64-bit mode.
		my $output = `cl /? 2>&1`;
		$? >> 8 == 0 or die "cl command not found";
		$self->{platform} =
		  ($output =~ /^\/favor:<.+AMD64/m) ? 'x64' : 'Win32';
	}
	else
	{
		$self->{platform} = 'FAKE';
	}
	print "Detected hardware platform: $self->{platform}\n";
	return;
}

# Return 1 if $oldfile is newer than $newfile, or if $newfile doesn't exist.
# Special case - if config.pl has changed, always return 1
sub IsNewer
{
	my ($newfile, $oldfile) = @_;
	-e $oldfile or warn "source file \"$oldfile\" does not exist";
	if (   $oldfile ne 'src/tools/msvc/config.pl'
		&& $oldfile ne 'src/tools/msvc/config_default.pl')
	{
		return 1
		  if (-f 'src/tools/msvc/config.pl')
		  && IsNewer($newfile, 'src/tools/msvc/config.pl');
		return 1
		  if (-f 'src/tools/msvc/config_default.pl')
		  && IsNewer($newfile, 'src/tools/msvc/config_default.pl');
	}
	return 1 if (!(-e $newfile));
	my @nstat = stat($newfile);
	my @ostat = stat($oldfile);
	return 1 if ($nstat[9] < $ostat[9]);
	return 0;
}

# Copy a file, *not* preserving date. Only works for text files.
sub copyFile
{
	my ($src, $dest) = @_;
	open(my $i, '<', $src)  || croak "Could not open $src";
	open(my $o, '>', $dest) || croak "Could not open $dest";
	while (<$i>)
	{
		print $o $_;
	}
	close($i);
	close($o);
	return;
}

# Fetch version of OpenSSL based on a parsing of the command shipped with
# the installer this build is linking to.  This returns as result an array
# made of the three first digits of the OpenSSL version, which is enough
# to decide which options to apply depending on the version of OpenSSL
# linking with.
sub GetOpenSSLVersion
{
	my $self = shift;

	# Attempt to get OpenSSL version and location.  This assumes that
	# openssl.exe is in the specified directory.
	# Quote the .exe name in case it has spaces
	my $opensslcmd =
	  qq("$self->{options}->{openssl}\\bin\\openssl.exe" version 2>&1);
	my $sslout = `$opensslcmd`;

	$? >> 8 == 0
	  or croak
	  "Unable to determine OpenSSL version: The openssl.exe command wasn't found.";

	if ($sslout =~ /(\d+)\.(\d+)\.(\d+)(\D)/m)
	{
		return ($1, $2, $3);
	}

	croak
	  "Unable to determine OpenSSL version: The openssl.exe version could not be determined.";
}

sub GenerateFiles
{
	my $self          = shift;
	my $bits          = $self->{platform} eq 'Win32' ? 32 : 64;
	my $ac_init_found = 0;
	my $package_name;
	my $package_version;
	my $package_bugreport;
	my $package_url;
	my ($majorver, $minorver);
	my $ac_define_openssl_api_compat_found = 0;
	my $openssl_api_compat;

	# Parse configure.ac to get version numbers
	open(my $c, '<', "configure.ac")
	  || confess("Could not open configure.ac for reading\n");
	while (<$c>)
	{
		if (/^AC_INIT\(\[([^\]]+)\], \[([^\]]+)\], \[([^\]]+)\], \[([^\]]*)\], \[([^\]]+)\]/
		  )
		{
			$ac_init_found = 1;

			$package_name      = $1;
			$package_version   = $2;
			$package_bugreport = $3;
			#$package_tarname   = $4;
			$package_url = $5;

			if ($package_version !~ /^(\d+)(?:\.(\d+))?/)
			{
				confess "Bad format of version: $self->{strver}\n";
			}
			$majorver = sprintf("%d", $1);
			$minorver = sprintf("%d", $2 ? $2 : 0);
		}
		elsif (/\bAC_DEFINE\(OPENSSL_API_COMPAT, \[([0-9xL]+)\]/)
		{
			$ac_define_openssl_api_compat_found = 1;
			$openssl_api_compat = $1;
		}
	}
	close($c);
	confess "Unable to parse configure.ac for all variables!"
	  unless $ac_init_found && $ac_define_openssl_api_compat_found;

	if (IsNewer("src/include/pg_config_os.h", "src/include/port/win32.h"))
	{
		print "Copying pg_config_os.h...\n";
		copyFile("src/include/port/win32.h", "src/include/pg_config_os.h");
	}

	print "Generating configuration headers...\n";
	my $extraver = $self->{options}->{extraver};
	$extraver = '' unless defined $extraver;
	my $port = $self->{options}->{"--with-pgport"} || 5432;

	# Every symbol in pg_config.h.in must be accounted for here.  Set
	# to undef if the symbol should not be defined.
	my %define = (
		ACCEPT_TYPE_ARG1           => 'unsigned int',
		ACCEPT_TYPE_ARG2           => 'struct sockaddr *',
		ACCEPT_TYPE_ARG3           => 'int',
		ACCEPT_TYPE_RETURN         => 'unsigned int PASCAL',
		ALIGNOF_DOUBLE             => 8,
		ALIGNOF_INT                => 4,
		ALIGNOF_LONG               => 4,
		ALIGNOF_LONG_LONG_INT      => 8,
		ALIGNOF_PG_INT128_TYPE     => undef,
		ALIGNOF_SHORT              => 2,
		AC_APPLE_UNIVERSAL_BUILD   => undef,
		BLCKSZ                     => 1024 * $self->{options}->{blocksize},
		CONFIGURE_ARGS             => '"' . $self->GetFakeConfigure() . '"',
		DEF_PGPORT                 => $port,
		DEF_PGPORT_STR             => qq{"$port"},
		ENABLE_GSS                 => $self->{options}->{gss} ? 1 : undef,
		ENABLE_NLS                 => $self->{options}->{nls} ? 1 : undef,
		ENABLE_THREAD_SAFETY       => 1,
		GETTIMEOFDAY_1ARG          => undef,
		HAVE_APPEND_HISTORY        => undef,
		HAVE_ASN1_STRING_GET0_DATA => undef,
		HAVE_ATOMICS               => 1,
		HAVE_ATOMIC_H              => undef,
		HAVE_BACKTRACE_SYMBOLS     => undef,
		HAVE_BIO_GET_DATA          => undef,
		HAVE_BIO_METH_NEW          => undef,
		HAVE_CLOCK_GETTIME         => undef,
		HAVE_COMPUTED_GOTO         => undef,
		HAVE_COPYFILE              => undef,
		HAVE_COPYFILE_H            => undef,
		HAVE_CRTDEFS_H             => undef,
		HAVE_CRYPTO_LOCK           => undef,
		HAVE_DECL_FDATASYNC        => 0,
		HAVE_DECL_F_FULLFSYNC      => 0,
		HAVE_DECL_LLVMCREATEGDBREGISTRATIONLISTENER => undef,
		HAVE_DECL_LLVMCREATEPERFJITEVENTLISTENER    => undef,
		HAVE_DECL_LLVMGETHOSTCPUNAME                => 0,
		HAVE_DECL_LLVMGETHOSTCPUFEATURES            => 0,
		HAVE_DECL_LLVMORCGETSYMBOLADDRESSIN         => 0,
		HAVE_DECL_POSIX_FADVISE                     => undef,
		HAVE_DECL_RTLD_GLOBAL                       => 0,
		HAVE_DECL_RTLD_NOW                          => 0,
		HAVE_DECL_STRLCAT                           => undef,
		HAVE_DECL_STRLCPY                           => undef,
		HAVE_DECL_STRNLEN                           => 1,
		HAVE_DECL_STRTOLL                           => 1,
		HAVE_DECL_STRTOULL                          => 1,
		HAVE_DLOPEN                                 => undef,
		HAVE_EDITLINE_HISTORY_H                     => undef,
		HAVE_EDITLINE_READLINE_H                    => undef,
		HAVE_EXECINFO_H                             => undef,
		HAVE_EXPLICIT_BZERO                         => undef,
		HAVE_FDATASYNC                              => undef,
		HAVE_FLS                                    => undef,
		HAVE_FSEEKO                                 => 1,
		HAVE_FUNCNAME__FUNC                         => undef,
		HAVE_FUNCNAME__FUNCTION                     => 1,
		HAVE_GCC__ATOMIC_INT32_CAS                  => undef,
		HAVE_GCC__ATOMIC_INT64_CAS                  => undef,
		HAVE_GCC__SYNC_CHAR_TAS                     => undef,
		HAVE_GCC__SYNC_INT32_CAS                    => undef,
		HAVE_GCC__SYNC_INT32_TAS                    => undef,
		HAVE_GCC__SYNC_INT64_CAS                    => undef,
		HAVE_GETADDRINFO                            => undef,
		HAVE_GETHOSTBYNAME_R                        => undef,
		HAVE_GETIFADDRS                             => undef,
		HAVE_GETOPT                                 => undef,
		HAVE_GETOPT_H                               => undef,
		HAVE_GETOPT_LONG                            => undef,
		HAVE_GETPEEREID                             => undef,
		HAVE_GETPEERUCRED                           => undef,
		HAVE_GETPWUID_R                             => undef,
		HAVE_GETRLIMIT                              => undef,
		HAVE_GETRUSAGE                              => undef,
		HAVE_GETTIMEOFDAY                           => undef,
		HAVE_GSSAPI_GSSAPI_H                        => undef,
		HAVE_GSSAPI_H                               => undef,
		HAVE_HISTORY_H                              => undef,
		HAVE_HISTORY_TRUNCATE_FILE                  => undef,
		HAVE_IFADDRS_H                              => undef,
		HAVE_INET_ATON                              => undef,
		HAVE_INT_TIMEZONE                           => 1,
		HAVE_INT64                                  => undef,
		HAVE_INT8                                   => undef,
		HAVE_INTTYPES_H                             => undef,
		HAVE_INT_OPTERR                             => undef,
		HAVE_INT_OPTRESET                           => undef,
		HAVE_IPV6                                   => 1,
		HAVE_I_CONSTRAINT__BUILTIN_CONSTANT_P       => undef,
		HAVE_KQUEUE                                 => undef,
		HAVE_LANGINFO_H                             => undef,
		HAVE_LDAP_H                                 => undef,
		HAVE_LDAP_INITIALIZE                        => undef,
		HAVE_LIBCRYPTO                              => undef,
		HAVE_LIBLDAP                                => undef,
		HAVE_LIBLDAP_R                              => undef,
		HAVE_LIBM                                   => undef,
		HAVE_LIBPAM                                 => undef,
		HAVE_LIBREADLINE                            => undef,
		HAVE_LIBSELINUX                             => undef,
		HAVE_LIBSSL                                 => undef,
		HAVE_LIBWLDAP32                             => undef,
		HAVE_LIBXML2                                => undef,
		HAVE_LIBXSLT                                => undef,
		HAVE_LIBZ                   => $self->{options}->{zlib} ? 1 : undef,
		HAVE_LINK                   => undef,
		HAVE_LOCALE_T               => 1,
		HAVE_LONG_INT_64            => undef,
		HAVE_LONG_LONG_INT_64       => 1,
		HAVE_MBARRIER_H             => undef,
		HAVE_MBSTOWCS_L             => 1,
		HAVE_MEMORY_H               => 1,
		HAVE_MEMSET_S               => undef,
		HAVE_MINIDUMP_TYPE          => 1,
		HAVE_MKDTEMP                => undef,
		HAVE_NETINET_TCP_H          => undef,
		HAVE_NET_IF_H               => undef,
		HAVE_OPENSSL_INIT_SSL       => undef,
		HAVE_OSSP_UUID_H            => undef,
		HAVE_PAM_PAM_APPL_H         => undef,
		HAVE_POLL                   => undef,
		HAVE_POLL_H                 => undef,
		HAVE_POSIX_FADVISE          => undef,
		HAVE_POSIX_FALLOCATE        => undef,
		HAVE_PPC_LWARX_MUTEX_HINT   => undef,
		HAVE_PPOLL                  => undef,
		HAVE_PREAD                  => undef,
		HAVE_PSTAT                  => undef,
		HAVE_PS_STRINGS             => undef,
		HAVE_PTHREAD                => undef,
		HAVE_PTHREAD_IS_THREADED_NP => undef,
		HAVE_PTHREAD_PRIO_INHERIT   => undef,
		HAVE_PWRITE                 => undef,
		HAVE_RANDOM                 => undef,
		HAVE_READLINE_H             => undef,
		HAVE_READLINE_HISTORY_H     => undef,
		HAVE_READLINE_READLINE_H    => undef,
		HAVE_READLINK               => undef,
		HAVE_RL_COMPLETION_APPEND_CHARACTER      => undef,
		HAVE_RL_COMPLETION_MATCHES               => undef,
		HAVE_RL_COMPLETION_SUPPRESS_QUOTE        => undef,
		HAVE_RL_FILENAME_COMPLETION_FUNCTION     => undef,
		HAVE_RL_FILENAME_QUOTE_CHARACTERS        => undef,
		HAVE_RL_FILENAME_QUOTING_FUNCTION        => undef,
		HAVE_RL_RESET_SCREEN_SIZE                => undef,
		HAVE_SECURITY_PAM_APPL_H                 => undef,
		HAVE_SETPROCTITLE                        => undef,
		HAVE_SETPROCTITLE_FAST                   => undef,
		HAVE_SETSID                              => undef,
		HAVE_SHM_OPEN                            => undef,
		HAVE_SPINLOCKS                           => 1,
		HAVE_SRANDOM                             => undef,
		HAVE_STDBOOL_H                           => 1,
		HAVE_STDINT_H                            => 1,
		HAVE_STDLIB_H                            => 1,
		HAVE_STRCHRNUL                           => undef,
		HAVE_STRERROR_R                          => undef,
		HAVE_STRINGS_H                           => undef,
		HAVE_STRING_H                            => 1,
		HAVE_STRLCAT                             => undef,
		HAVE_STRLCPY                             => undef,
		HAVE_STRNLEN                             => 1,
		HAVE_STRSIGNAL                           => undef,
		HAVE_STRTOF                              => 1,
		HAVE_STRTOLL                             => 1,
		HAVE_STRTOQ                              => undef,
		HAVE_STRTOULL                            => 1,
		HAVE_STRTOUQ                             => undef,
		HAVE_STRUCT_ADDRINFO                     => 1,
		HAVE_STRUCT_CMSGCRED                     => undef,
		HAVE_STRUCT_OPTION                       => undef,
		HAVE_STRUCT_SOCKADDR_SA_LEN              => undef,
		HAVE_STRUCT_SOCKADDR_STORAGE             => 1,
		HAVE_STRUCT_SOCKADDR_STORAGE_SS_FAMILY   => 1,
		HAVE_STRUCT_SOCKADDR_STORAGE_SS_LEN      => undef,
		HAVE_STRUCT_SOCKADDR_STORAGE___SS_FAMILY => undef,
		HAVE_STRUCT_SOCKADDR_STORAGE___SS_LEN    => undef,
		HAVE_STRUCT_SOCKADDR_UN                  => undef,
		HAVE_STRUCT_TM_TM_ZONE                   => undef,
		HAVE_SYNC_FILE_RANGE                     => undef,
		HAVE_SYMLINK                             => 1,
		HAVE_SYSLOG                              => undef,
		HAVE_SYS_EPOLL_H                         => undef,
		HAVE_SYS_EVENT_H                         => undef,
		HAVE_SYS_IPC_H                           => undef,
		HAVE_SYS_PRCTL_H                         => undef,
		HAVE_SYS_PROCCTL_H                       => undef,
		HAVE_SYS_PSTAT_H                         => undef,
		HAVE_SYS_RESOURCE_H                      => undef,
		HAVE_SYS_SELECT_H                        => undef,
		HAVE_SYS_SEM_H                           => undef,
		HAVE_SYS_SHM_H                           => undef,
		HAVE_SYS_SOCKIO_H                        => undef,
		HAVE_SYS_STAT_H                          => 1,
		HAVE_SYS_TAS_H                           => undef,
		HAVE_SYS_TYPES_H                         => 1,
		HAVE_SYS_UCRED_H                         => undef,
		HAVE_SYS_UN_H                            => undef,
		HAVE_TERMIOS_H                           => undef,
		HAVE_TYPEOF                              => undef,
		HAVE_UCRED_H                             => undef,
		HAVE_UINT64                              => undef,
		HAVE_UINT8                               => undef,
		HAVE_UNION_SEMUN                         => undef,
		HAVE_UNISTD_H                            => 1,
		HAVE_UNSETENV                            => undef,
		HAVE_USELOCALE                           => undef,
		HAVE_UUID_BSD                            => undef,
		HAVE_UUID_E2FS                           => undef,
		HAVE_UUID_OSSP                           => undef,
		HAVE_UUID_H                              => undef,
		HAVE_UUID_UUID_H                         => undef,
		HAVE_WINLDAP_H                           => undef,
		HAVE_WCSTOMBS_L                          => 1,
		HAVE_WCTYPE_H                            => 1,
		HAVE_X509_GET_SIGNATURE_NID              => 1,
		HAVE_X86_64_POPCNTQ                      => undef,
		HAVE__BOOL                               => undef,
		HAVE__BUILTIN_BSWAP16                    => undef,
		HAVE__BUILTIN_BSWAP32                    => undef,
		HAVE__BUILTIN_BSWAP64                    => undef,
		HAVE__BUILTIN_CLZ                        => undef,
		HAVE__BUILTIN_CONSTANT_P                 => undef,
		HAVE__BUILTIN_CTZ                        => undef,
		HAVE__BUILTIN_OP_OVERFLOW                => undef,
		HAVE__BUILTIN_POPCOUNT                   => undef,
		HAVE__BUILTIN_TYPES_COMPATIBLE_P         => undef,
		HAVE__BUILTIN_UNREACHABLE                => undef,
		HAVE__CONFIGTHREADLOCALE                 => 1,
		HAVE__CPUID                              => 1,
		HAVE__GET_CPUID                          => undef,
		HAVE__STATIC_ASSERT                      => undef,
		HAVE___STRTOLL                           => undef,
		HAVE___STRTOULL                          => undef,
		INT64_MODIFIER                           => qq{"ll"},
		LOCALE_T_IN_XLOCALE                      => undef,
		MAXIMUM_ALIGNOF                          => 8,
		MEMSET_LOOP_LIMIT                        => 1024,
		OPENSSL_API_COMPAT                       => $openssl_api_compat,
		PACKAGE_BUGREPORT                        => qq{"$package_bugreport"},
		PACKAGE_NAME                             => qq{"$package_name"},
		PACKAGE_STRING      => qq{"$package_name $package_version"},
		PACKAGE_TARNAME     => lc qq{"$package_name"},
		PACKAGE_URL         => qq{"$package_url"},
		PACKAGE_VERSION     => qq{"$package_version"},
		PG_INT128_TYPE      => undef,
		PG_INT64_TYPE       => 'long long int',
		PG_KRB_SRVNAM       => qq{"postgres"},
		PG_MAJORVERSION     => qq{"$majorver"},
		PG_MAJORVERSION_NUM => $majorver,
		PG_MINORVERSION_NUM => $minorver,
		PG_PRINTF_ATTRIBUTE => undef,
		PG_USE_STDBOOL      => 1,
		PG_VERSION          => qq{"$package_version$extraver"},
		PG_VERSION_NUM      => sprintf("%d%04d", $majorver, $minorver),
		PG_VERSION_STR =>
		  qq{"PostgreSQL $package_version$extraver, compiled by Visual C++ build " CppAsString2(_MSC_VER) ", $bits-bit"},
		PROFILE_PID_DIR         => undef,
		PTHREAD_CREATE_JOINABLE => undef,
		RELSEG_SIZE             => (1024 / $self->{options}->{blocksize}) *
		  $self->{options}->{segsize} * 1024,
		SIZEOF_BOOL                         => 1,
		SIZEOF_LONG                         => 4,
		SIZEOF_OFF_T                        => undef,
		SIZEOF_SIZE_T                       => $bits / 8,
		SIZEOF_VOID_P                       => $bits / 8,
		STDC_HEADERS                        => 1,
		STRERROR_R_INT                      => undef,
		USE_ARMV8_CRC32C                    => undef,
		USE_ARMV8_CRC32C_WITH_RUNTIME_CHECK => undef,
		USE_ASSERT_CHECKING => $self->{options}->{asserts} ? 1 : undef,
		USE_BONJOUR         => undef,
		USE_BSD_AUTH        => undef,
		USE_ICU => $self->{options}->{icu} ? 1 : undef,
		USE_LIBXML                 => undef,
		USE_LIBXSLT                => undef,
		USE_LDAP                   => $self->{options}->{ldap} ? 1 : undef,
		USE_LLVM                   => undef,
		USE_NAMED_POSIX_SEMAPHORES => undef,
		USE_OPENSSL                => undef,
		USE_PAM                    => undef,
		USE_SLICING_BY_8_CRC32C    => undef,
		USE_SSE42_CRC32C           => undef,
		USE_SSE42_CRC32C_WITH_RUNTIME_CHECK => 1,
		USE_SYSTEMD                         => undef,
		USE_SYSV_SEMAPHORES                 => undef,
		USE_SYSV_SHARED_MEMORY              => undef,
		USE_UNNAMED_POSIX_SEMAPHORES        => undef,
		USE_WIN32_SEMAPHORES                => 1,
		USE_WIN32_SHARED_MEMORY             => 1,
		WCSTOMBS_L_IN_XLOCALE               => undef,
		WORDS_BIGENDIAN                     => undef,
		XLOG_BLCKSZ       => 1024 * $self->{options}->{wal_blocksize},
		_FILE_OFFSET_BITS => undef,
		_LARGEFILE_SOURCE => undef,
		_LARGE_FILES      => undef,
		inline            => '__inline',
		pg_restrict       => '__restrict',
		# not defined, because it'd conflict with __declspec(restrict)
		restrict => undef,
		typeof   => undef,);

	if ($self->{options}->{uuid})
	{
		$define{HAVE_UUID_OSSP} = 1;
		$define{HAVE_UUID_H}    = 1;
	}
	if ($self->{options}->{xml})
	{
		$define{HAVE_LIBXML2} = 1;
		$define{USE_LIBXML}   = 1;
	}
	if ($self->{options}->{xslt})
	{
		$define{HAVE_LIBXSLT} = 1;
		$define{USE_LIBXSLT}  = 1;
	}
	if ($self->{options}->{openssl})
	{
		$define{USE_OPENSSL} = 1;

		my ($digit1, $digit2, $digit3) = $self->GetOpenSSLVersion();

		# More symbols are needed with OpenSSL 1.1.0 and above.
		if ($digit1 >= '1' && $digit2 >= '1' && $digit3 >= '0')
		{
			$define{HAVE_ASN1_STRING_GET0_DATA} = 1;
			$define{HAVE_BIO_GET_DATA}          = 1;
			$define{HAVE_BIO_METH_NEW}          = 1;
			$define{HAVE_OPENSSL_INIT_SSL}      = 1;
		}
	}

	$self->GenerateConfigHeader('src/include/pg_config.h',     \%define, 1);
	$self->GenerateConfigHeader('src/include/pg_config_ext.h', \%define, 0);
	$self->GenerateConfigHeader('src/interfaces/ecpg/include/ecpg_config.h',
		\%define, 0);

	$self->GenerateDefFile(
		"src/interfaces/libpq/libpqdll.def",
		"src/interfaces/libpq/exports.txt",
		"LIBPQ");
	$self->GenerateDefFile(
		"src/interfaces/ecpg/ecpglib/ecpglib.def",
		"src/interfaces/ecpg/ecpglib/exports.txt",
		"LIBECPG");
	$self->GenerateDefFile(
		"src/interfaces/ecpg/compatlib/compatlib.def",
		"src/interfaces/ecpg/compatlib/exports.txt",
		"LIBECPG_COMPAT");
	$self->GenerateDefFile(
		"src/interfaces/ecpg/pgtypeslib/pgtypeslib.def",
		"src/interfaces/ecpg/pgtypeslib/exports.txt",
		"LIBPGTYPES");

	chdir('src/backend/utils');
	my $pg_proc_dat = '../../../src/include/catalog/pg_proc.dat';
	if (   IsNewer('fmgr-stamp', 'Gen_fmgrtab.pl')
		|| IsNewer('fmgr-stamp', '../catalog/Catalog.pm')
		|| IsNewer('fmgr-stamp', $pg_proc_dat)
		|| IsNewer('fmgr-stamp', '../../../src/include/access/transam.h'))
	{
		system(
			"perl -I ../catalog Gen_fmgrtab.pl --include-path ../../../src/include/ $pg_proc_dat"
		);
		open(my $f, '>', 'fmgr-stamp')
		  || confess "Could not touch fmgr-stamp";
		close($f);
	}
	chdir('../../..');

	if (IsNewer(
			'src/include/utils/fmgroids.h',
			'src/backend/utils/fmgroids.h'))
	{
		copyFile('src/backend/utils/fmgroids.h',
			'src/include/utils/fmgroids.h');
	}

	if (IsNewer(
			'src/include/utils/fmgrprotos.h',
			'src/backend/utils/fmgrprotos.h'))
	{
		copyFile(
			'src/backend/utils/fmgrprotos.h',
			'src/include/utils/fmgrprotos.h');
	}

	if (IsNewer(
			'src/include/storage/lwlocknames.h',
			'src/backend/storage/lmgr/lwlocknames.txt'))
	{
		print "Generating lwlocknames.c and lwlocknames.h...\n";
		chdir('src/backend/storage/lmgr');
		system('perl generate-lwlocknames.pl lwlocknames.txt');
		chdir('../../../..');
	}
	if (IsNewer(
			'src/include/storage/lwlocknames.h',
			'src/backend/storage/lmgr/lwlocknames.h'))
	{
		copyFile(
			'src/backend/storage/lmgr/lwlocknames.h',
			'src/include/storage/lwlocknames.h');
	}

	if (IsNewer('src/include/utils/probes.h', 'src/backend/utils/probes.d'))
	{
		print "Generating probes.h...\n";
		system(
			'perl src/backend/utils/Gen_dummy_probes.pl src/backend/utils/probes.d > src/include/utils/probes.h'
		);
	}

	if ($self->{options}->{python}
		&& IsNewer(
			'src/pl/plpython/spiexceptions.h',
			'src/backend/utils/errcodes.txt'))
	{
		print "Generating spiexceptions.h...\n";
		system(
			'perl src/pl/plpython/generate-spiexceptions.pl src/backend/utils/errcodes.txt > src/pl/plpython/spiexceptions.h'
		);
	}

	if (IsNewer(
			'src/include/utils/errcodes.h',
			'src/backend/utils/errcodes.txt'))
	{
		print "Generating errcodes.h...\n";
		system(
			'perl src/backend/utils/generate-errcodes.pl src/backend/utils/errcodes.txt > src/backend/utils/errcodes.h'
		);
		copyFile('src/backend/utils/errcodes.h',
			'src/include/utils/errcodes.h');
	}

	if (IsNewer(
			'src/pl/plpgsql/src/plerrcodes.h',
			'src/backend/utils/errcodes.txt'))
	{
		print "Generating plerrcodes.h...\n";
		system(
			'perl src/pl/plpgsql/src/generate-plerrcodes.pl src/backend/utils/errcodes.txt > src/pl/plpgsql/src/plerrcodes.h'
		);
	}

	if ($self->{options}->{tcl}
		&& IsNewer(
			'src/pl/tcl/pltclerrcodes.h', 'src/backend/utils/errcodes.txt'))
	{
		print "Generating pltclerrcodes.h...\n";
		system(
			'perl src/pl/tcl/generate-pltclerrcodes.pl src/backend/utils/errcodes.txt > src/pl/tcl/pltclerrcodes.h'
		);
	}

	if (IsNewer(
			'src/backend/utils/sort/qsort_tuple.c',
			'src/backend/utils/sort/gen_qsort_tuple.pl'))
	{
		print "Generating qsort_tuple.c...\n";
		system(
			'perl src/backend/utils/sort/gen_qsort_tuple.pl > src/backend/utils/sort/qsort_tuple.c'
		);
	}

	if (IsNewer('src/bin/psql/sql_help.h', 'src/bin/psql/create_help.pl'))
	{
		print "Generating sql_help.h...\n";
		chdir('src/bin/psql');
		system("perl create_help.pl ../../../doc/src/sgml/ref sql_help");
		chdir('../../..');
	}

	if (IsNewer('src/common/kwlist_d.h', 'src/include/parser/kwlist.h'))
	{
		print "Generating kwlist_d.h...\n";
		system(
			'perl -I src/tools src/tools/gen_keywordlist.pl --extern -o src/common src/include/parser/kwlist.h'
		);
	}

	if (IsNewer(
			'src/pl/plpgsql/src/pl_reserved_kwlist_d.h',
			'src/pl/plpgsql/src/pl_reserved_kwlist.h')
		|| IsNewer(
			'src/pl/plpgsql/src/pl_unreserved_kwlist_d.h',
			'src/pl/plpgsql/src/pl_unreserved_kwlist.h'))
	{
		print
		  "Generating pl_reserved_kwlist_d.h and pl_unreserved_kwlist_d.h...\n";
		chdir('src/pl/plpgsql/src');
		system(
			'perl -I ../../../tools ../../../tools/gen_keywordlist.pl --varname ReservedPLKeywords pl_reserved_kwlist.h'
		);
		system(
			'perl -I ../../../tools ../../../tools/gen_keywordlist.pl --varname UnreservedPLKeywords pl_unreserved_kwlist.h'
		);
		chdir('../../../..');
	}

	if (IsNewer(
			'src/interfaces/ecpg/preproc/c_kwlist_d.h',
			'src/interfaces/ecpg/preproc/c_kwlist.h')
		|| IsNewer(
			'src/interfaces/ecpg/preproc/ecpg_kwlist_d.h',
			'src/interfaces/ecpg/preproc/ecpg_kwlist.h'))
	{
		print "Generating c_kwlist_d.h and ecpg_kwlist_d.h...\n";
		chdir('src/interfaces/ecpg/preproc');
		system(
			'perl -I ../../../tools ../../../tools/gen_keywordlist.pl --varname ScanCKeywords --no-case-fold c_kwlist.h'
		);
		system(
			'perl -I ../../../tools ../../../tools/gen_keywordlist.pl --varname ScanECPGKeywords ecpg_kwlist.h'
		);
		chdir('../../../..');
	}

	if (IsNewer(
			'src/interfaces/ecpg/preproc/preproc.y',
			'src/backend/parser/gram.y'))
	{
		print "Generating preproc.y...\n";
		chdir('src/interfaces/ecpg/preproc');
		system('perl parse.pl < ../../../backend/parser/gram.y > preproc.y');
		chdir('../../../..');
	}

	unless (-f "src/port/pg_config_paths.h")
	{
		print "Generating pg_config_paths.h...\n";
		open(my $o, '>', 'src/port/pg_config_paths.h')
		  || confess "Could not open pg_config_paths.h";
		print $o <<EOF;
#define PGBINDIR "/bin"
#define PGSHAREDIR "/share"
#define SYSCONFDIR "/etc"
#define INCLUDEDIR "/include"
#define PKGINCLUDEDIR "/include"
#define INCLUDEDIRSERVER "/include/server"
#define LIBDIR "/lib"
#define PKGLIBDIR "/lib"
#define LOCALEDIR "/share/locale"
#define DOCDIR "/doc"
#define HTMLDIR "/doc"
#define MANDIR "/man"
EOF
		close($o);
	}

	my $mf = Project::read_file('src/backend/catalog/Makefile');
	$mf =~ s{\\\r?\n}{}g;
	$mf =~ /^CATALOG_HEADERS\s*:?=(.*)$/gm
	  || croak "Could not find CATALOG_HEADERS in Makefile\n";
	my @bki_srcs = split /\s+/, $1;
	$mf =~ /^POSTGRES_BKI_DATA\s*:?=[^,]+,(.*)\)$/gm
	  || croak "Could not find POSTGRES_BKI_DATA in Makefile\n";
	my @bki_data = split /\s+/, $1;

	my $need_genbki = 0;
	foreach my $bki (@bki_srcs, @bki_data)
	{
		next if $bki eq "";
		if (IsNewer(
				'src/backend/catalog/bki-stamp',
				"src/include/catalog/$bki"))
		{
			$need_genbki = 1;
			last;
		}
	}
	$need_genbki = 1
	  if IsNewer('src/backend/catalog/bki-stamp',
		'src/backend/catalog/genbki.pl');
	$need_genbki = 1
	  if IsNewer('src/backend/catalog/bki-stamp',
		'src/backend/catalog/Catalog.pm');
	if ($need_genbki)
	{
		chdir('src/backend/catalog');
		my $bki_srcs = join(' ../../../src/include/catalog/', @bki_srcs);
		system(
			"perl genbki.pl --include-path ../../../src/include/ --set-version=$majorver $bki_srcs"
		);
		open(my $f, '>', 'bki-stamp')
		  || confess "Could not touch bki-stamp";
		close($f);
		chdir('../../..');
	}

	if (IsNewer(
			'src/include/catalog/header-stamp',
			'src/backend/catalog/bki-stamp'))
	{
		# Copy generated headers to include directory.
		opendir(my $dh, 'src/backend/catalog/')
		  || die "Can't opendir src/backend/catalog/ $!";
		my @def_headers = grep { /pg_\w+_d\.h$/ } readdir($dh);
		closedir $dh;
		foreach my $def_header (@def_headers)
		{
			copyFile(
				"src/backend/catalog/$def_header",
				"src/include/catalog/$def_header");
		}
		copyFile(
			'src/backend/catalog/schemapg.h',
			'src/include/catalog/schemapg.h');
		open(my $chs, '>', 'src/include/catalog/header-stamp')
		  || confess "Could not touch header-stamp";
		close($chs);
	}

	open(my $o, '>', "doc/src/sgml/version.sgml")
	  || croak "Could not write to version.sgml\n";
	print $o <<EOF;
<!ENTITY version "$package_version">
<!ENTITY majorversion "$majorver">
EOF
	close($o);
	return;
}

# Read lines from input file and substitute symbols using the same
# logic that config.status uses.  There should be one call of this for
# each AC_CONFIG_HEADERS call in configure.ac.
#
# If the "required" argument is true, we also keep track which of our
# defines have been found and error out if any are left unused at the
# end.  That way we avoid accumulating defines in this file that are
# no longer used by configure.
sub GenerateConfigHeader
{
	my ($self, $config_header, $defines, $required) = @_;

	my $config_header_in = $config_header . '.in';

	if (   IsNewer($config_header, $config_header_in)
		|| IsNewer($config_header, __FILE__))
	{
		my %defines_copy = %$defines;

		open(my $i, '<', $config_header_in)
		  || confess "Could not open $config_header_in\n";
		open(my $o, '>', $config_header)
		  || confess "Could not write to $config_header\n";

		print $o
		  "/* $config_header.  Generated from $config_header_in by src/tools/msvc/Solution.pm.  */\n";

		while (<$i>)
		{
			if (m/^#(\s*)undef\s+(\w+)/)
			{
				my $ws    = $1;
				my $macro = $2;
				if (exists $defines->{$macro})
				{
					if (defined $defines->{$macro})
					{
						print $o "#${ws}define $macro ", $defines->{$macro},
						  "\n";
					}
					else
					{
						print $o "/* #${ws}undef $macro */\n";
					}
					delete $defines_copy{$macro};
				}
				else
				{
					croak
					  "undefined symbol: $macro at $config_header line $.";
				}
			}
			else
			{
				print $o $_;
			}
		}
		close($o);
		close($i);

		if ($required && scalar(keys %defines_copy) > 0)
		{
			croak "unused defines: " . join(' ', keys %defines_copy);
		}
	}
}

sub GenerateDefFile
{
	my ($self, $deffile, $txtfile, $libname) = @_;

	if (IsNewer($deffile, $txtfile))
	{
		print "Generating $deffile...\n";
		open(my $if, '<', $txtfile) || confess("Could not open $txtfile\n");
		open(my $of, '>', $deffile) || confess("Could not open $deffile\n");
		print $of "LIBRARY $libname\nEXPORTS\n";
		while (<$if>)
		{
			next if (/^#/);
			next if (/^\s*$/);
			my ($f, $o) = split;
			print $of " $f @ $o\n";
		}
		close($of);
		close($if);
	}
	return;
}

sub AddProject
{
	my ($self, $name, $type, $folder, $initialdir) = @_;

	my $proj =
	  VSObjectFactory::CreateProject($self->{vcver}, $name, $type, $self);
	push @{ $self->{projects}->{$folder} }, $proj;
	$proj->AddDir($initialdir) if ($initialdir);
	if ($self->{options}->{zlib})
	{
		$proj->AddIncludeDir($self->{options}->{zlib} . '\include');
		$proj->AddLibrary($self->{options}->{zlib} . '\lib\zdll.lib');
	}
	if ($self->{options}->{openssl})
	{
		$proj->AddIncludeDir($self->{options}->{openssl} . '\include');
		my ($digit1, $digit2, $digit3) = $self->GetOpenSSLVersion();

		# Starting at version 1.1.0 the OpenSSL installers have
		# changed their library names from:
		# - libeay to libcrypto
		# - ssleay to libssl
		if ($digit1 >= '1' && $digit2 >= '1' && $digit3 >= '0')
		{
			my $dbgsuffix;
			my $libsslpath;
			my $libcryptopath;

			# The format name of the libraries is slightly
			# different between the Win32 and Win64 platform, so
			# adapt.
			if (-e "$self->{options}->{openssl}/lib/VC/sslcrypto32MD.lib")
			{
				# Win32 here, with a debugging library set.
				$dbgsuffix     = 1;
				$libsslpath    = '\lib\VC\libssl32.lib';
				$libcryptopath = '\lib\VC\libcrypto32.lib';
			}
			elsif (-e "$self->{options}->{openssl}/lib/VC/sslcrypto64MD.lib")
			{
				# Win64 here, with a debugging library set.
				$dbgsuffix     = 1;
				$libsslpath    = '\lib\VC\libssl64.lib';
				$libcryptopath = '\lib\VC\libcrypto64.lib';
			}
			else
			{
				# On both Win32 and Win64 the same library
				# names are used without a debugging context.
				$dbgsuffix     = 0;
				$libsslpath    = '\lib\libssl.lib';
				$libcryptopath = '\lib\libcrypto.lib';
			}

			$proj->AddLibrary($self->{options}->{openssl} . $libsslpath,
				$dbgsuffix);
			$proj->AddLibrary($self->{options}->{openssl} . $libcryptopath,
				$dbgsuffix);
		}
		else
		{
			# Choose which set of libraries to use depending on if
			# debugging libraries are in place in the installer.
			if (-e "$self->{options}->{openssl}/lib/VC/ssleay32MD.lib")
			{
				$proj->AddLibrary(
					$self->{options}->{openssl} . '\lib\VC\ssleay32.lib', 1);
				$proj->AddLibrary(
					$self->{options}->{openssl} . '\lib\VC\libeay32.lib', 1);
			}
			else
			{
				# We don't expect the config-specific library
				# to be here, so don't ask for it in last
				# parameter.
				$proj->AddLibrary(
					$self->{options}->{openssl} . '\lib\ssleay32.lib', 0);
				$proj->AddLibrary(
					$self->{options}->{openssl} . '\lib\libeay32.lib', 0);
			}
		}
	}
	if ($self->{options}->{nls})
	{
		$proj->AddIncludeDir($self->{options}->{nls} . '\include');
		$proj->AddLibrary($self->{options}->{nls} . '\lib\libintl.lib');
	}
	if ($self->{options}->{gss})
	{
		$proj->AddIncludeDir($self->{options}->{gss} . '\inc\krb5');
		$proj->AddLibrary($self->{options}->{gss} . '\lib\i386\krb5_32.lib');
		$proj->AddLibrary($self->{options}->{gss} . '\lib\i386\comerr32.lib');
		$proj->AddLibrary($self->{options}->{gss} . '\lib\i386\gssapi32.lib');
	}
	if ($self->{options}->{iconv})
	{
		$proj->AddIncludeDir($self->{options}->{iconv} . '\include');
		$proj->AddLibrary($self->{options}->{iconv} . '\lib\iconv.lib');
	}
	if ($self->{options}->{icu})
	{
		$proj->AddIncludeDir($self->{options}->{icu} . '\include');
		if ($self->{platform} eq 'Win32')
		{
			$proj->AddLibrary($self->{options}->{icu} . '\lib\icuin.lib');
			$proj->AddLibrary($self->{options}->{icu} . '\lib\icuuc.lib');
			$proj->AddLibrary($self->{options}->{icu} . '\lib\icudt.lib');
		}
		else
		{
			$proj->AddLibrary($self->{options}->{icu} . '\lib64\icuin.lib');
			$proj->AddLibrary($self->{options}->{icu} . '\lib64\icuuc.lib');
			$proj->AddLibrary($self->{options}->{icu} . '\lib64\icudt.lib');
		}
	}
	if ($self->{options}->{xml})
	{
		$proj->AddIncludeDir($self->{options}->{xml} . '\include');
		$proj->AddIncludeDir($self->{options}->{xml} . '\include\libxml2');
		$proj->AddLibrary($self->{options}->{xml} . '\lib\libxml2.lib');
	}
	if ($self->{options}->{xslt})
	{
		$proj->AddIncludeDir($self->{options}->{xslt} . '\include');
		$proj->AddLibrary($self->{options}->{xslt} . '\lib\libxslt.lib');
	}
	if ($self->{options}->{uuid})
	{
		$proj->AddIncludeDir($self->{options}->{uuid} . '\include');
		$proj->AddLibrary($self->{options}->{uuid} . '\lib\uuid.lib');
	}
	return $proj;
}

sub Save
{
	my ($self) = @_;
	my %flduid;

	$self->GenerateFiles();
	foreach my $fld (keys %{ $self->{projects} })
	{
		foreach my $proj (@{ $self->{projects}->{$fld} })
		{
			$proj->Save();
		}
	}

	open(my $sln, '>', "pgsql.sln") || croak "Could not write to pgsql.sln\n";
	print $sln <<EOF;
Microsoft Visual Studio Solution File, Format Version $self->{solutionFileVersion}
# $self->{visualStudioName}
EOF

	print $sln $self->GetAdditionalHeaders();

	foreach my $fld (keys %{ $self->{projects} })
	{
		foreach my $proj (@{ $self->{projects}->{$fld} })
		{
			print $sln <<EOF;
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "$proj->{name}", "$proj->{name}$proj->{filenameExtension}", "$proj->{guid}"
EndProject
EOF
		}
		if ($fld ne "")
		{
			$flduid{$fld} = $^O eq "MSWin32" ? Win32::GuidGen() : 'FAKE';
			print $sln <<EOF;
Project("{2150E333-8FDC-42A3-9474-1A3956D46DE8}") = "$fld", "$fld", "$flduid{$fld}"
EndProject
EOF
		}
	}

	print $sln <<EOF;
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|$self->{platform}= Debug|$self->{platform}
		Release|$self->{platform} = Release|$self->{platform}
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
EOF

	foreach my $fld (keys %{ $self->{projects} })
	{
		foreach my $proj (@{ $self->{projects}->{$fld} })
		{
			print $sln <<EOF;
		$proj->{guid}.Debug|$self->{platform}.ActiveCfg = Debug|$self->{platform}
		$proj->{guid}.Debug|$self->{platform}.Build.0  = Debug|$self->{platform}
		$proj->{guid}.Release|$self->{platform}.ActiveCfg = Release|$self->{platform}
		$proj->{guid}.Release|$self->{platform}.Build.0 = Release|$self->{platform}
EOF
		}
	}

	print $sln <<EOF;
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
	GlobalSection(NestedProjects) = preSolution
EOF

	foreach my $fld (keys %{ $self->{projects} })
	{
		next if ($fld eq "");
		foreach my $proj (@{ $self->{projects}->{$fld} })
		{
			print $sln "\t\t$proj->{guid} = $flduid{$fld}\n";
		}
	}

	print $sln <<EOF;
	EndGlobalSection
EndGlobal
EOF
	close($sln);
	return;
}

sub GetFakeConfigure
{
	my $self = shift;

	my $cfg = '--enable-thread-safety';
	$cfg .= ' --enable-cassert'   if ($self->{options}->{asserts});
	$cfg .= ' --enable-nls'       if ($self->{options}->{nls});
	$cfg .= ' --enable-tap-tests' if ($self->{options}->{tap_tests});
	$cfg .= ' --with-ldap'        if ($self->{options}->{ldap});
	$cfg .= ' --without-zlib' unless ($self->{options}->{zlib});
	$cfg .= ' --with-extra-version' if ($self->{options}->{extraver});
	$cfg .= ' --with-openssl'       if ($self->{options}->{openssl});
	$cfg .= ' --with-uuid'          if ($self->{options}->{uuid});
	$cfg .= ' --with-libxml'        if ($self->{options}->{xml});
	$cfg .= ' --with-libxslt'       if ($self->{options}->{xslt});
	$cfg .= ' --with-gssapi'        if ($self->{options}->{gss});
	$cfg .= ' --with-icu'           if ($self->{options}->{icu});
	$cfg .= ' --with-tcl'           if ($self->{options}->{tcl});
	$cfg .= ' --with-perl'          if ($self->{options}->{perl});
	$cfg .= ' --with-python'        if ($self->{options}->{python});

	return $cfg;
}

package VS2013Solution;

#
# Package that encapsulates a Visual Studio 2013 solution file
#

use Carp;
use strict;
use warnings;
use base qw(Solution);

no warnings qw(redefine);    ## no critic

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion}        = '12.00';
	$self->{vcver}                      = '12.00';
	$self->{visualStudioName}           = 'Visual Studio 2013';
	$self->{VisualStudioVersion}        = '12.0.21005.1';
	$self->{MinimumVisualStudioVersion} = '10.0.40219.1';

	return $self;
}

package VS2015Solution;

#
# Package that encapsulates a Visual Studio 2015 solution file
#

use Carp;
use strict;
use warnings;
use base qw(Solution);

no warnings qw(redefine);    ## no critic

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion}        = '12.00';
	$self->{vcver}                      = '14.00';
	$self->{visualStudioName}           = 'Visual Studio 2015';
	$self->{VisualStudioVersion}        = '14.0.24730.2';
	$self->{MinimumVisualStudioVersion} = '10.0.40219.1';

	return $self;
}

package VS2017Solution;

#
# Package that encapsulates a Visual Studio 2017 solution file
#

use Carp;
use strict;
use warnings;
use base qw(Solution);

no warnings qw(redefine);    ## no critic

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion}        = '12.00';
	$self->{vcver}                      = '15.00';
	$self->{visualStudioName}           = 'Visual Studio 2017';
	$self->{VisualStudioVersion}        = '15.0.26730.3';
	$self->{MinimumVisualStudioVersion} = '10.0.40219.1';

	return $self;
}

package VS2019Solution;

#
# Package that encapsulates a Visual Studio 2019 solution file
#

use Carp;
use strict;
use warnings;
use base qw(Solution);

no warnings qw(redefine);    ## no critic

sub new
{
	my $classname = shift;
	my $self      = $classname->SUPER::_new(@_);
	bless($self, $classname);

	$self->{solutionFileVersion}        = '12.00';
	$self->{vcver}                      = '16.00';
	$self->{visualStudioName}           = 'Visual Studio 2019';
	$self->{VisualStudioVersion}        = '16.0.28729.10';
	$self->{MinimumVisualStudioVersion} = '10.0.40219.1';

	return $self;
}

sub GetAdditionalHeaders
{
	my ($self, $f) = @_;

	return qq|VisualStudioVersion = $self->{VisualStudioVersion}
MinimumVisualStudioVersion = $self->{MinimumVisualStudioVersion}
|;
}

1;
