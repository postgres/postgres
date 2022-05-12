use strict;
use warnings;

our $config;

$config->{"tap_tests"} = 1;
$config->{"asserts"}   = 1;

$config->{"openssl"} = "c:/openssl/1.1/";
$config->{"perl"}    = "c:/strawberry/$ENV{DEFAULT_PERL_VERSION}/perl/";
$config->{"python"}  = "c:/python/";

1;
