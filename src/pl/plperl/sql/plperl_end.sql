-- test END block handling

-- Not included in the normal testing
-- because it's beyond the scope of the test harness.
-- Available here for manual developer testing.

DO $do$
	my $testlog = "/tmp/pgplperl_test.log";

	warn "Run test, then examine contents of $testlog (which must already exist)\n";
	return unless -f $testlog;

    use IO::Handle; # for autoflush
	open my $fh, '>', $testlog
		or die "Can't write to $testlog: $!";
    $fh->autoflush(1);

    print $fh "# you should see just 3 'Warn: ...' lines: PRE, END and SPI ...\n";
    $SIG{__WARN__} = sub { print $fh "Warn: @_" };
    $SIG{__DIE__}  = sub { print $fh "Die: @_" unless $^S; die @_ };

	END {
		warn "END\n";
		eval { spi_exec_query("select 1") };
		warn $@;
	}
    warn "PRE\n";

$do$ language plperlu;
