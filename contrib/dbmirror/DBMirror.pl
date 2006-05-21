#!/usr/bin/perl
#############################################################################
#
# DBMirror.pl
# Contains the Database mirroring script.
# This script queries the pending table off the database specified
# (along with the associated schema) for updates that are pending on a 
# specific host.  The database on that host is then updated with the changes.
#
#
#    Written by Steven Singer (ssinger@navtechinc.com)
#    (c) 2001-2002 Navtech Systems Support Inc.
# ALL RIGHTS RESERVED;
#
# Permission to use, copy, modify, and distribute this software and its
# documentation for any purpose, without fee, and without a written agreement
# is hereby granted, provided that the above copyright notice and this
# paragraph and the following two paragraphs appear in all copies.
#
# IN NO EVENT SHALL THE AUTHOR OR DISTRIBUTORS BE LIABLE TO ANY PARTY FOR
# DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
# LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
# DOCUMENTATION, EVEN IF THE AUTHOR OR DISTRIBUTORS HAVE BEEN ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#
# THE AUTHOR AND DISTRIBUTORS SPECIFICALLY DISCLAIMS ANY WARRANTIES,
# INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
# ON AN "AS IS" BASIS, AND THE AUTHOR AND DISTRIBUTORS HAS NO OBLIGATIONS TO
# PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
#
#
# 
#
##############################################################################
# $Id: DBMirror.pl,v 1.6.4.1 2006/05/21 19:57:06 momjian Exp $ 
#
##############################################################################

=head1 NAME

DBMirror.pl - A Perl module to mirror database changes from a master database
to a slave.

=head1 SYNPOSIS


DBMirror.pl slaveConfigfile.conf


=head1 DESCRIPTION

This Perl script will connect to the master database and query its pending 
table for a list of pending changes.

The transactions of the original changes to the master will be preserved
when sending things to the slave.

=cut


=head1 METHODS

=over 4

=cut


BEGIN {
  # add in a global path to files
  # Pg should be included. 
}


use strict;
use Pg;
use IO::Handle;
sub mirrorCommand($$$$$$);
sub mirrorInsert($$$$$);
sub mirrorDelete($$$$$);
sub mirrorUpdate($$$$$);
sub sendQueryToSlaves($$);
sub logErrorMessage($);
sub openSlaveConnection($);
sub updateMirrorHostTable($$);
			sub extractData($$);
local $::masterHost;
local $::masterDb; 
local $::masterUser; 
local $::masterPassword; 
local $::errorThreshold=5;
local $::errorEmailAddr=undef;

my %slaveInfoHash;
local $::slaveInfo = \%slaveInfoHash;

my $lastErrorMsg;
my $repeatErrorCount=0;

my $lastXID;
my $commandCount=0;

my $masterConn;

Main();

sub Main() {
  
#run the configuration file.
  if ($#ARGV != 0) {
    die "usage: DBMirror.pl configFile\n";
  }
  if( ! defined do $ARGV[0]) {
    logErrorMessage("Invalid Configuration file $ARGV[0]");
    die;
  }
  
  
  my $connectString = "host=$::masterHost dbname=$::masterDb user=$::masterUser password=$::masterPassword";
  
  $masterConn = Pg::connectdb($connectString);
  
  unless($masterConn->status == PGRES_CONNECTION_OK) {
    logErrorMessage("Can't connect to master database\n" .
		    $masterConn->errorMessage);
    die;
  }
    
  my $setQuery;
  $setQuery = "SET search_path = public";
  my $setResult = $masterConn->exec($setQuery);
  if($setResult->resultStatus!=PGRES_COMMAND_OK) { 
    logErrorMessage($masterConn->errorMessage . "\n" . 
		    $setQuery);
    die;
  }
    
  my $firstTime = 1;
  while(1) {
    if($firstTime == 0) {
      sleep 60; 
    } 
    $firstTime = 0;
# Open up the connection to the slave.
    if(! defined $::slaveInfo->{"status"} ||
       $::slaveInfo->{"status"} == -1) {
      openSlaveConnection($::slaveInfo);	    
    }
    
    
   
    sendQueryToSlaves(undef,"SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    sendQueryToSlaves(undef,"SET CONSTRAINTS ALL DEFERRED");
    
    
    #Obtain a list of pending transactions using ordering by our approximation
    #to the commit time.  The commit time approximation is taken to be the
    #SeqId of the last row edit in the transaction.
    my $pendingTransQuery = "SELECT pd.\"XID\",MAX(\"SeqId\") FROM \"Pending\" pd";
    $pendingTransQuery .= " LEFT JOIN \"MirroredTransaction\" mt INNER JOIN";
    $pendingTransQuery .= " \"MirrorHost\" mh ON mt.\"MirrorHostId\" = ";
    $pendingTransQuery .= " mh.\"MirrorHostId\" AND mh.\"HostName\"=";
    $pendingTransQuery .= " '$::slaveInfo->{\"slaveHost\"}' "; 
    $pendingTransQuery .= " ON pd.\"XID\"";
    $pendingTransQuery .= " = mt.\"XID\" WHERE mt.\"XID\" is null  ";
    $pendingTransQuery .= " GROUP BY pd.\"XID\" ";
    $pendingTransQuery .= " ORDER BY MAX(pd.\"SeqId\")";
    
    
    my $pendingTransResults = $masterConn->exec($pendingTransQuery);
    unless($pendingTransResults->resultStatus==PGRES_TUPLES_OK) {
      logErrorMessage("Can't query pending table\n" . $masterConn->errorMessage);
      die;
    }
    
    my $numPendingTrans = $pendingTransResults->ntuples;
    my $curTransTuple = 0;
    
    
    #
    # This loop loops through each pending transaction in the proper order.
    # The Pending row edits for that transaction will be queried from the 
    # master and sent + committed to the slaves.
    while($curTransTuple < $numPendingTrans) {
      my $XID = $pendingTransResults->getvalue($curTransTuple,0);
      my $maxSeqId = $pendingTransResults->getvalue($curTransTuple,1);
      my $seqId;
      
      my $pendingQuery = "SELECT pnd.\"SeqId\",pnd.\"TableName\",";
      $pendingQuery .= " pnd.\"Op\",pnddata.\"IsKey\", pnddata.\"Data\" AS \"Data\" ";
      $pendingQuery .= " FROM \"Pending\" pnd, \"PendingData\" pnddata ";
      $pendingQuery .= " WHERE pnd.\"SeqId\" = pnddata.\"SeqId\" AND ";
      
      $pendingQuery .= " pnd.\"XID\"=$XID ORDER BY \"SeqId\", \"IsKey\" DESC";
      
      
      my $pendingResults = $masterConn->exec($pendingQuery);
      unless($pendingResults->resultStatus==PGRES_TUPLES_OK) {
	logErrorMessage("Can't query pending table\n" . $masterConn->errorMessage);
	die;
      }
      
	    
	    
      my $numPending = $pendingResults->ntuples;
      my $curTuple = 0;
      sendQueryToSlaves(undef,"BEGIN");
      while ($curTuple < $numPending) {
	$seqId = $pendingResults->getvalue($curTuple,0);
	my $tableName = $pendingResults->getvalue($curTuple,1);
	my $op = $pendingResults->getvalue($curTuple,2);
	
	$curTuple = mirrorCommand($seqId,$tableName,$op,$XID,
				  $pendingResults,$curTuple) +1;
	if($::slaveInfo->{"status"}==-1) {
	    last;
	}
	
      }
      #Now commit the transaction.
      if($::slaveInfo->{"status"}==-1) {
	  last;
      }
      sendQueryToSlaves(undef,"COMMIT");
      updateMirrorHostTable($XID,$seqId);
      if($commandCount > 5000) {
	$commandCount = 0;
	$::slaveInfo->{"status"} = -1;
	$::slaveInfo->{"slaveConn"}->reset;
	#Open the connection right away.
	openSlaveConnection($::slaveInfo);
	
      }
      
      $pendingResults = undef;
      $curTransTuple = $curTransTuple +1;
    }#while transactions left.
	
	$pendingTransResults = undef;
    
  }#while(1)
}#Main



=item mirrorCommand(SeqId,tableName,op,transId,pendingResults,curTuple)

Mirrors a single SQL Command(change to a single row) to the slave.

=over 4

=item * SeqId

The id number of the change to mirror.  This is the
primary key of the pending table.


=item * tableName

The name of the table the transaction takes place on.

=item * op

The type of operation this transaction is.  'i' for insert, 'u' for update or
'd' for delete.

=item * transId

The Transaction of of the Transaction that this command is part of.

=item * pendingResults

A Results set structure returned from Pg::execute that contains the 
join of the Pending and PendingData tables for all of the pending row
edits in this transaction. 

=item * currentTuple 


The tuple(or row) number of the pendingRow for the command that is about
to be edited.   If the command is an update then this points to the row
with IsKey equal to true.  The next row, curTuple+1 is the contains the
PendingData with IsKey false for the update.


=item returns


The tuple number of last tuple for this command.  This might be equal to
currentTuple or it might be larger (+1 in the case of an Update).


=back

=cut


sub mirrorCommand($$$$$$) {
    my $seqId = $_[0];
    my $tableName = $_[1];
    my $op = $_[2];
    my $transId = $_[3];
    my $pendingResults = $_[4];
    my $currentTuple = $_[5];

    if($op eq 'i') {
      $currentTuple = mirrorInsert($seqId,$tableName,$transId,$pendingResults
			       ,$currentTuple);
    }
    if($op eq 'd') {
      $currentTuple = mirrorDelete($seqId,$tableName,$transId,$pendingResults,
			       $currentTuple);
    }
    if($op eq 'u') {
      $currentTuple = mirrorUpdate($seqId,$tableName,$transId,$pendingResults,
		   $currentTuple);
    }
    $commandCount = $commandCount +1;
    if($commandCount % 100 == 0) {
    #  print "Sent 100 commmands on SeqId $seqId \n";
    #  flush STDOUT;
    }
    return $currentTuple
  }


=item mirrorInsert(transId,tableName,transId,pendingResults,currentTuple)

Mirrors an INSERT operation to the slave database.  A new row is placed
in the slave database containing the primary key from pendingKeys along with
the data fields contained in the row identified by sourceOid.

=over 4

=item * transId

The sequence id of the INSERT operation being mirrored. This is the primary
key of the pending table.

=item * tableName


The name of the table the transaction takes place on.

=item * sourceOid

The OID of the row in the master database for which this transaction effects.
If the transaction is a delete then the operation is not valid.

=item * transId 

The Transaction Id of transaction that this insert is part of.



=item * pendingResults

A Results set structure returned from Pg::execute that contains the 
join of the Pending and PendingData tables for all of the pending row
edits in this transaction. 

=item * currentTuple 


The tuple(or row) number of the pendingRow for the command that is about
to be edited.   In the case of an insert this should point to the one 
row for the row edit.

=item returns

The tuple number of the last tuple for the row edit.  This should be 
currentTuple.


=back

=cut


sub mirrorInsert($$$$$) {
    my $seqId = $_[0];
    my $tableName = $_[1];
    my $transId = $_[2];
    my $pendingResults = $_[3];
    my $currentTuple = $_[4];
    my $counter;
    my $column;

    my $firstIteration=1;
    my %recordValues = extractData($pendingResults,$currentTuple);

        
    #Now build the insert query.
    my $insertQuery = "INSERT INTO $tableName (";
    my $valuesQuery = ") VALUES (";
    foreach $column (keys (%recordValues)) {
	if($firstIteration==0) {
	    $insertQuery .= " ,";
	    $valuesQuery .= " ,";
	}
      $insertQuery .= "\"$column\"";
      if(defined $recordValues{$column}) {
	my $quotedValue = $recordValues{$column};
	$quotedValue =~ s/\\/\\\\/g;
	$quotedValue =~ s/'/''/g;
	$valuesQuery .= "'$quotedValue'";
      }
      else {
	$valuesQuery .= "null";
      }
	$firstIteration=0;
    }
    $valuesQuery .= ")";
    sendQueryToSlaves(undef,$insertQuery . $valuesQuery);
    return $currentTuple;
}

=item mirrorDelete(SeqId,tableName,transId,pendingResult,currentTuple)

Deletes a single row from the slave database.  The row is identified by the
primary key for the transaction in the pendingKeys table.

=over 4

=item * SeqId

The Sequence id for this delete request.

=item * tableName

The name of the table to delete the row from.

=item * transId 

The Transaction Id of the transaction that this command is part of.



=item * pendingResults

A Results set structure returned from Pg::execute that contains the 
join of the Pending and PendingData tables for all of the pending row
edits in this transaction. 

=item * currentTuple 


The tuple(or row) number of the pendingRow for the command that is about
to be edited.   In the case of a  delete this should point to the one 
row for the row edit.

=item returns

The tuple number of the last tuple for the row edit.  This should be 
currentTuple.


=back

=cut


sub mirrorDelete($$$$$) {
    my $seqId = $_[0];
    my $tableName = $_[1];
    my $transId = $_[2];
    my $pendingResult = $_[3];
    my $currentTuple = $_[4];
    my %dataHash;
    my $currentField;
    my $firstField=1;
    %dataHash = extractData($pendingResult,$currentTuple);

    my $counter=0;
    my $deleteQuery = "DELETE FROM $tableName WHERE ";
    foreach $currentField (keys %dataHash) {
      if($firstField==0) {
	$deleteQuery .= " AND ";
      }
      my $currentValue = $dataHash{$currentField};
      $deleteQuery .= "\"";
      $deleteQuery .= $currentField;
      if(defined $currentValue) {
	$deleteQuery .= "\"='";
	$deleteQuery .= $currentValue;
	$deleteQuery .= "'";
      }
      else {
	$deleteQuery .= " is null ";
      }
      $counter++;
      $firstField=0;
    }
    
    sendQueryToSlaves($transId,$deleteQuery);
    return $currentTuple;
}


=item mirrorUpdate(seqId,tableName,transId,pendingResult,currentTuple)

Mirrors over an edit request to a single row of the database.
The primary key from before the edit is used to determine which row in the
slave should be changed.  

After the edit takes place on the slave its primary key will match the primary 
key the master had immediatly following the edit.  All other fields will be set
to the current values.   

Data integrity is maintained because the mirroring is performed in an 
SQL transcation so either all pending changes are made or none are.

=over 4

=item * seqId 

The Sequence id of the update.

=item * tableName

The name of the table to perform the update on.

=item * transId

The transaction Id for the transaction that this command is part of.


=item * pendingResults

A Results set structure returned from Pg::execute that contains the 
join of the Pending and PendingData tables for all of the pending row
edits in this transaction. 

=item * currentTuple 


The tuple(or row) number of the pendingRow for the command that is about
to be edited.   In the case of a  delete this should point to the one 
row for the row edit.

=item returns

The tuple number of the last tuple for the row edit.  This should be 
currentTuple +1.  Which points to the non key row of the update.


=back

=cut

sub mirrorUpdate($$$$$) {
    my $seqId = $_[0];
    my $tableName = $_[1];
    my $transId = $_[2];
    my $pendingResult = $_[3];
    my $currentTuple = $_[4];

    my $counter;
    my $quotedValue;
    my $updateQuery = "UPDATE $tableName SET ";
    my $currentField;



    my %keyValueHash;
    my %dataValueHash;
    my $firstIteration=1;

    #Extract the Key values. This row contains the values of the
    # key fields before the update occours(the WHERE clause)
    %keyValueHash = extractData($pendingResult,$currentTuple);


    #Extract the data values.  This is a SET clause that contains 
    #values for the entire row AFTER the update.    
    %dataValueHash = extractData($pendingResult,$currentTuple+1);

    $firstIteration=1;
    foreach $currentField (keys (%dataValueHash)) {
      if($firstIteration==0) {
	$updateQuery .= ", ";
      }
      $updateQuery .= " \"$currentField\"=";
      my $currentValue = $dataValueHash{$currentField};
      if(defined $currentValue ) {
	$quotedValue = $currentValue;
	$quotedValue =~ s/\\/\\\\/g;
	$quotedValue =~ s/'/''/g;
	$updateQuery .= "'$quotedValue'";
	}
      else {
	$updateQuery .= "null ";
      }
      $firstIteration=0;
    }

   
    $updateQuery .= " WHERE ";
    $firstIteration=1;
    foreach $currentField (keys (%keyValueHash)) {   
      my $currentValue;
      if($firstIteration==0) {
	$updateQuery .= " AND ";
      }
      $updateQuery .= "\"$currentField\"=";
      $currentValue = $keyValueHash{$currentField};
      if(defined $currentValue) {
	$quotedValue = $currentValue;
	$quotedValue =~ s/\\/\\\\/g;
        $quotedValue =~ s/'/''/g;
	$updateQuery .= "'$quotedValue'";
      }
      else {
	$updateQuery .= " null ";
      }
      $firstIteration=0;
    }
    
    sendQueryToSlaves($transId,$updateQuery);
    return $currentTuple+1;
}



=item sendQueryToSlaves(seqId,sqlQuery)

Sends an SQL query to the slave.


=over 4

=item * seqId

The sequence Id of the command being sent. Undef if no command is associated 
with the query being sent.

=item * sqlQuery


SQL operation to perform on the slave.

=back

=cut

sub sendQueryToSlaves($$) {
    my $seqId = $_[0];
    my  $sqlQuery = $_[1];
       
   if($::slaveInfo->{"status"} == 0) {
       my $queryResult = $::slaveInfo->{"slaveConn"}->exec($sqlQuery);
       unless($queryResult->resultStatus == PGRES_COMMAND_OK) {
	   my $errorMessage;
	   $errorMessage = "Error sending query  $seqId to " ;
	   $errorMessage .= $::slaveInfo->{"slaveHost"};
	   $errorMessage .=$::slaveInfo->{"slaveConn"}->errorMessage;
	   $errorMessage .= "\n" . $sqlQuery;
	   logErrorMessage($errorMessage);
	   $::slaveInfo->{"slaveConn"}->exec("ROLLBACK");
	   $::slaveInfo->{"status"} = -1;
       }
   }

}


=item logErrorMessage(error)

Mails an error message to the users specified $errorEmailAddr
The error message is also printed to STDERR.

=over 4

=item * error

The error message to log.

=back

=cut

sub logErrorMessage($) {
    my $error = $_[0];

    if(defined $lastErrorMsg and $error eq $lastErrorMsg) {
	if($repeatErrorCount<$::errorThreshold) {
	    $repeatErrorCount++;
	    warn($error);
	    return;
	}

    }
    $repeatErrorCount=0;
    if(defined $::errorEmailAddr) {
      my $mailPipe;
      open (mailPipe, "|/bin/mail -s DBMirror.pl $::errorEmailAddr");
      print mailPipe "=====================================================\n";
      print mailPipe "         DBMirror.pl                                 \n";
      print mailPipe "\n";
      print mailPipe " The DBMirror.pl script has encountred an error.     \n";
      print mailPipe " It might indicate that either the master database has\n";
      print mailPipe " gone down or that the connection to a slave database can\n";
      print mailPipe " not be made.                                         \n";
      print mailPipe " Process-Id: $$ on $::masterHost database $::masterDb\n";
      print mailPipe  "\n";
      print mailPipe $error;
      print mailPipe "\n\n\n=================================================\n";
      close mailPipe;
    }
    warn($error);    
    
    $lastErrorMsg = $error;

}

sub openSlaveConnection($) {
    my $slavePtr = $_[0];
    my $slaveConn;
    
    
    my $slaveConnString = "host=" . $slavePtr->{"slaveHost"};    
    $slaveConnString .= " dbname=" . $slavePtr->{"slaveDb"};
    $slaveConnString .= " user=" . $slavePtr->{"slaveUser"};
    $slaveConnString .= " password=" . $slavePtr->{"slavePassword"};
    
    $slaveConn = Pg::connectdb($slaveConnString);
    
    if($slaveConn->status != PGRES_CONNECTION_OK) {
	my $errorMessage = "Can't connect to slave database " ;
	$errorMessage .= $slavePtr->{"slaveHost"} . "\n";
	$errorMessage .= $slaveConn->errorMessage;
	logErrorMessage($errorMessage);    
	$slavePtr->{"status"} = -1;
    }
    else {
	$slavePtr->{"slaveConn"} = $slaveConn;
	$slavePtr->{"status"} = 0;
	#Determine the MirrorHostId for the slave from the master's database
	my $resultSet = $masterConn->exec('SELECT "MirrorHostId" FROM '
					  . ' "MirrorHost" WHERE "HostName"'
					  . '=\'' . $slavePtr->{"slaveHost"}
					  . '\'');
	if($resultSet->ntuples !=1) {
	    my $errorMessage .= $slavePtr->{"slaveHost"} ."\n";
	    $errorMessage .= "Has no MirrorHost entry on master\n";
	    logErrorMessage($errorMessage);
	    $slavePtr->{"status"}=-1;
	    return;
	    
	}
	$slavePtr->{"MirrorHostId"} = $resultSet->getvalue(0,0);
	
	
	
    }

}


=item updateMirrorHostTable(lastTransId,lastSeqId)

Updates the MirroredTransaction table to reflect the fact that
this transaction has been sent to the current slave.

=over 4 

=item * lastTransId

The Transaction id for the last transaction that has been succesfully mirrored to
the currently open slaves.

=item * lastSeqId 

The Sequence Id of the last command that has been succefully mirrored


=back


=cut

sub updateMirrorHostTable($$) {
    my $lastTransId = shift;
    my $lastSeqId = shift;

    if($::slaveInfo->{"status"}==0) {
	my $deleteTransactionQuery;
	my $deleteResult;
	my $updateMasterQuery = "INSERT INTO \"MirroredTransaction\" ";
	$updateMasterQuery .= " (\"XID\",\"LastSeqId\",\"MirrorHostId\")";
	$updateMasterQuery .= " VALUES ($lastTransId,$lastSeqId,$::slaveInfo->{\"MirrorHostId\"}) ";
	
	my $updateResult = $masterConn->exec($updateMasterQuery);
	unless($updateResult->resultStatus == PGRES_COMMAND_OK) {
	    my $errorMessage = $masterConn->errorMessage . "\n";
	    $errorMessage .= $updateMasterQuery;
	    logErrorMessage($errorMessage);
	    die;
	}
#	print "Updated slaves to transaction $lastTransId\n" ;	 
#        flush STDOUT;  

	#If this transaction has now been mirrored to all mirror hosts
	#then it can be deleted.
	$deleteTransactionQuery = 'DELETE FROM "Pending" WHERE "XID"='
	    . $lastTransId . ' AND (SELECT COUNT(*) FROM "MirroredTransaction"'
		. ' WHERE "XID"=' . $lastTransId . ')=(SELECT COUNT(*) FROM'
		    . ' "MirrorHost")';
	
	$deleteResult = $masterConn->exec($deleteTransactionQuery);
	if($deleteResult->resultStatus!=PGRES_COMMAND_OK) { 
	    logErrorMessage($masterConn->errorMessage . "\n" . 
			    $deleteTransactionQuery);
	    die;
	}
	
    }
    
}


sub extractData($$) {
  my $pendingResult = $_[0];
  my $currentTuple = $_[1];
  my $fnumber;
  my %valuesHash;
  $fnumber = 4;
  my $dataField = $pendingResult->getvalue($currentTuple,$fnumber);

  while(length($dataField)>0) {
    # Extract the field name that is surronded by double quotes
    $dataField =~ m/(\".*?\")/s;
    my $fieldName = $1;
    $dataField = substr $dataField ,length($fieldName);
    $fieldName =~ s/\"//g; #Remove the surronding " signs.

    if($dataField =~ m/(^= )/s) {
      #Matched null
	$dataField = substr $dataField , length($1);
      $valuesHash{$fieldName}=undef;
    }
    elsif ($dataField =~ m/(^=\')/s) {
      #Has data.
      my $value;
      $dataField = substr $dataField ,2; #Skip the ='
    LOOP: {  #This is to allow us to use last from a do loop.
	     #Recommended in perlsyn manpage.
      do {
	my $matchString;
	#Find the substring ending with the first ' or first \
	$dataField =~ m/(.*?[\'\\])?/s; 
	$matchString = $1;
	$value .= substr $matchString,0,length($matchString)-1;

	if($matchString =~ m/(\'$)/s) {
	  # $1 runs to the end of the field value.
	    $dataField = substr $dataField,length($matchString)+1;
	    last;
	  
	}
	else {
	  #deal with the escape character.
	  #It The character following the escape gets appended.
	    $dataField = substr $dataField,length($matchString);	    
	    $dataField =~ s/(^.)//s;	    
	    $value .=  $1;


	  
	}
	
	   
      } until(length($dataField)==0);
  }
      $valuesHash{$fieldName} = $value;
      
      
      }#else if 
	  else {
	    
	    logErrorMessage "Error in PendingData Sequence Id " .
		$pendingResult->getvalue($currentTuple,0);
	    die;
	  }
    
    
    
  } #while
  return %valuesHash;
    
}
