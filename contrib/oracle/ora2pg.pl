#!/usr/bin/perl
#------------------------------------------------------------------------------
# Project  : Oracle to Postgresql converter
# Name     : ora2pg.pl
# Language : perl, v5.6.1
# OS       : linux RedHat 7.3 kernel 2.4.18-17.7.xsmp
# Author   : Gilles Darold, gilles@darold.net
# Copyright: Copyright (c) 2000-2002 : Gilles Darold - All rights reserved -
# Function : Script used to convert Oracle Database to PostgreSQL
#------------------------------------------------------------------------------
# Version  : 2.0
#------------------------------------------------------------------------------

BEGIN {
        $ENV{ORACLE_HOME} = '/usr/local/oracle/oracle816';
}

use strict;

use Ora2Pg;

# Initialyze the database connection
my $dbsrc = 'dbi:Oracle:host=localhost;sid=TEST';
my $dbuser = 'system';
my $dbpwd = 'manager';

# Create an instance of the Ora2Pg perl module
my $schema = new Ora2Pg (
	datasource => $dbsrc,		# Database DBD datasource
	user => $dbuser,		# Database user
	password => $dbpwd,		# Database password
	debug => 1,			# Verbose mode
#	export_schema => 1,		# Export Oracle schema to Postgresql 7.3 schema
#	schema => 'APPS',		# Extract only the given schema namespace
	type => 'TABLE',		# Extract table
#	type => 'PACKAGE',		# Extract PACKAGE information
#	type => 'DATA',			# Extract data with output as INSERT statement
#	type => 'COPY',			# Extract data with output as COPY statement
#	type => 'VIEW',			# Extract views
#	type => 'GRANT',		# Extract privileges
#	type => 'SEQUENCE',		# Extract sequences
#	type => 'TRIGGER',		# Extract triggers
#	type => 'FUNCTION',		# Extract functions
#	type => 'PROCEDURE',		# Extract procedures
#	tables => [('TX_DATA')],		# simple indexes
#	tables => [('NDW_BROWSER_ATTRIBUTES')],	# view
#	tables => [('TRIP_DATA')],	# Foreign key
#	showtableid => 1,		# Display only table indice during extraction
#	min => 1,			# Extract begin at indice 3
#	max => 10,			# Extract ended at indice 5
#	data_limit => 1000,		# Extract all data by dump of 1000 tuples
#	data_limit => 0,		# Extract all data in one pass. Be sure to have enougth memory.
);

# Just export data of the following fields from table 's_txcot'
#$schema->modify_struct('s_txcot','dossier', 'rub', 'datapp');

#### Function to use for extraction when type option is set to DATA or COPY

	# Send exported data directly to a PostgreSQL database
	#$schema->send_to_pgdb('dbi:Pg:dbname=test_db;host=localhost;port=5432','test','test');

	# Output the data extracted from Oracle DB to a file or to STDOUT if no argument.
	#$schema->export_data("output.sql");

#### Function to use for extraction of other type

	# Create the POSTGRESQL representation of all objects in the database
	$schema->export_schema("output.sql");

exit(0);

