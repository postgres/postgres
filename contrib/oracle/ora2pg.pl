#!/usr/bin/perl
#------------------------------------------------------------------------------
# Project  : Oracle2Postgresql
# Name     : ora2pg.pl
# Language : 5.006 built for i686-linux
# OS       : linux RedHat 6.2 kernel 2.2.14-5
# Author   : Gilles Darold, gilles@darold.net
# Copyright: Copyright (c) 2000 : Gilles Darold - All rights reserved -
# Function : Script used to convert Oracle Database schema to PostgreSQL
#------------------------------------------------------------------------------
# Version  : 1.1
#------------------------------------------------------------------------------

BEGIN {
        $ENV{ORACLE_HOME} = '/usr/local/oracle/oracle816';
}

use strict;

use Ora2Pg;

# Initialyze the database connection
my $dbsrc = 'dbi:Oracle:host=test.mydomain.com;sid=TEST;port=1521';
my $dbuser = 'system';
my $dbpwd = 'manager';

# Create an instance of the XSD::DBISchema perl module
my $schema = new Ora2Pg (
	datasource => $dbsrc,		# Database DBD datasource
	user => $dbuser,		# Database user
	password => $dbpwd,		# Database password
	debug => 1,			# Verbose mode
	schema => 'ALICIA7',		# Extract only APPS schema
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
#	tables => [('FND_USER_PREFERENCES')],	# unique index + users
#	tables => [('CUSTOMER_DATA')],		# Unique and primary key
#	tables => [('TX_DATA')],		# simple indexes
#	tables => [('NDW_BROWSER_ATTRIBUTES')],	# view
#	tables => [('TRIP_DATA')],	# Foreign key
#	tables => [('JO_TMP')],	# Foreign key
#	showtableid => 1,		# Display only table indice during extraction
#	min => 1,			# Extract begin at indice 3
#	max => 10,			# Extract ended at indice 5
#	data_limit => 1000,		# Extract all data by dump of 1000 tuples
#	data_limit => 0,		# Extract all data in one pass. Be sure to have enougth memory
);

# Just export data of the following fields from table 's_txcot'
#$schema->modify_struct('s_txcot','dossier', 'rub', 'datapp');

#### Function to use for extraction when type option is set to DATA or COPY

	# Send exported data to a PostgreSQL database
	#$schema->send_to_pgdb('dbi:Pg:dbname=template1;host=localhost;port=5432','test','test');

	# Output the data extracted from Oracle DB to a file or to STDOUT if no argument.
	# If you set the send_to_pgdb() method the output is given to PG database. See above
	#$schema->export_data("output.sql");

#### Function to use for extraction of other type

	# Create the POSTGRESQL representation of all objects in the database
	$schema->export_schema("output.sql");

exit(0);

