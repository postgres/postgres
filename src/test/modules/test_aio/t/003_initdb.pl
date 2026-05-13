# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Test initdb for each IO method. This is done separately from 001_aio.pl, as
# it isn't fast. This way the more commonly failing / hacked-on 001_aio.pl can
# be iterated on more quickly.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use TestAio;


foreach my $method (TestAio::supported_io_methods())
{
	test_create_node($method);
}

done_testing();


sub test_create_node
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $io_method = shift;

	my $node = PostgreSQL::Test::Cluster->new($io_method);

	# Want to test initdb for each IO method, otherwise we could just reuse
	# the cluster.
	#
	# Unfortunately Cluster::init() puts PG_TEST_INITDB_EXTRA_OPTS after the
	# options specified by ->extra, if somebody puts -c io_method=xyz in
	# PG_TEST_INITDB_EXTRA_OPTS it would break this test. Fix that up if we
	# detect it.
	local $ENV{PG_TEST_INITDB_EXTRA_OPTS} = $ENV{PG_TEST_INITDB_EXTRA_OPTS};
	if (defined $ENV{PG_TEST_INITDB_EXTRA_OPTS}
		&& $ENV{PG_TEST_INITDB_EXTRA_OPTS} =~ m/io_method=/)
	{
		$ENV{PG_TEST_INITDB_EXTRA_OPTS} .= " -c io_method=$io_method";
	}

	$node->init(extra => [ '-c', "io_method=$io_method" ]);

	TestAio::configure($node);

	# Even though we used -c io_method=... above, if TEMP_CONFIG sets
	# io_method, it'd override the setting persisted at initdb time. While
	# using (and later verifying) the setting from initdb provides some
	# verification of having used the io_method during initdb, it's probably
	# not worth the complication of only appending if the variable is set in
	# in TEMP_CONFIG.
	$node->append_conf(
		'postgresql.conf', qq(
io_method=$io_method
));

	ok(1, "$io_method: initdb");

	$node->start();
	$node->stop();
	ok(1, "$io_method: start & stop");

	return $node;
}
