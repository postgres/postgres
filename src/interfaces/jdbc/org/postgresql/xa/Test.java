package org.postgresql.xa;


import java.sql.*;
import javax.sql.*;
import javax.transaction.xa.*;


public class Test
{


    public static void main( String args[] )
    {
	XADataSource           xaDS;
	java.io.PrintWriter    log;

	log = new java.io.PrintWriter( System.out );
	try {

	    xaDS = new XADataSource();
	    xaDS.setDatabaseName( "test" );
	    xaDS.setUser( "arkin" );
	    xaDS.setPassword( "natasha" );
	    xaDS.setLogWriter( log );

	    Thread1 thread1;

	    thread1 = new Thread1();
	    thread1.xaConn = xaDS.getXAConnection();
	    thread1.xid1 = new XidImpl();

	    Thread2 thread2;

	    thread2 = new Thread2();
	    thread1.thread2 = thread2;
	    thread2.thread1 = thread1;
	    thread2.xaConn = xaDS.getXAConnection();
	    thread2.xid1 = thread1.xid1;
	    thread2.xid2 = new XidImpl();
	    
	    thread1.start();
	    thread2.start();
	    
	} catch ( Exception except ) {
	    System.out.println( except );
	    except.printStackTrace();
	}
	log.flush();
    }


}


class Thread1
    extends Thread
{


    public void run()
    {
	Connection conn;
	XAResource xaRes;
	Statement stmt;
	ResultSet rs;

	try {
	    conn  = xaConn.getConnection();
	    xaRes = xaConn.getXAResource();
	} catch ( Exception except ) {
	    System.out.println( except );
	    return;
	}
	// Initially the table should have no value.
	try {
	    stmt = conn.createStatement();
	    stmt.executeUpdate( "update test set text='nothing' where id=1" );
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}


	// Begin a transaction on this connection.
	// Perform an update on the table.
	System.out.println( "[Thread1] Starting transaction" );
	try {
	    xaRes.start( xid1, XAResource.TMNOFLAGS );
	} catch ( XAException except ) {
	    System.out.println( except );
	    return;	    
	}
	System.out.println( "[Thread1] Updating table" );
	try {
	    stmt = conn.createStatement();
	    stmt.executeUpdate( "update test set text='first' where id=1" );
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}


	// Thread2 will start a new transction and attempt
	// to perform an update on the table and will lock.
	System.out.println( "[Thread1] Waking up Thread2" );
	thread2.interrupt();
	try {
	    sleep( Integer.MAX_VALUE );
	} catch ( InterruptedException except ) { }


	// Perform a select from the table just to prove
	// that Thread2 failed in its update.
	System.out.println( "[Thread1] Selecting from table" );
	try {
	    stmt = conn.createStatement();
	    rs = stmt.executeQuery( "select text from test where id=1" );
	    rs.next();
	    System.out.println( "First = " + rs.getString( 1 ) );
	    rs.close();
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}


	// Thread2 will now attempt to join our transaction
	// and perform an update on the table.
	System.out.println( "[Thread1] Waking up Thread2" );
	thread2.interrupt();
	try {
	    sleep( Integer.MAX_VALUE );
	} catch ( InterruptedException except ) { }


	// Perform a select from the table to prove that
	// Thread2 managed to update it.
	System.out.println( "[Thread1] Selecting from table" );
	try {
	    stmt = conn.createStatement();
	    rs = stmt.executeQuery( "select text from test where id=1" );
	    rs.next();
	    System.out.println( "First = " + rs.getString( 1 ) );
	    rs.close();
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}


	// We now end the transaction for this thread.
	// We are no longer in the shared transaction.
	// Perform an update on the table and the update
	// will lock.
	System.out.println( "[Thread1] Ending transaction" );
	try {
	    xaRes.end( xid1, XAResource.TMSUCCESS );
	} catch ( XAException except ) {
	    System.out.println( except );
	    return;	    
	}
	System.out.println( "[Thread1] Selecting from table" );
	try {
	    stmt = conn.createStatement();
	    stmt.executeUpdate( "update test set text='first' where id=1" );
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}


	// Thread 2 will now end the transcation and commit it.
	System.out.println( "[Thread1] Waking up Thread2" );
	thread2.interrupt();
	try {
	    sleep( Integer.MAX_VALUE );
	} catch ( InterruptedException except ) { }


	// Perform a select on the table to prove that it
	// was only updated inside the transaction.
	System.out.println( "[Thread1] Selecting from table" );
	try {
	    stmt = conn.createStatement();
	    rs = stmt.executeQuery( "select text from test where id=1" );
	    rs.next();
	    System.out.println( "First = " + rs.getString( 1 ) );
	    rs.close();
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}
    }


    javax.sql.XAConnection xaConn;


    Xid                    xid1;


    Thread                 thread2;


}


class Thread2
    extends Thread
{


    public void run()
    {
	Connection conn;
	XAResource xaRes;
	Statement stmt;
	ResultSet rs;

       
	try {
	    conn  = xaConn.getConnection();
	    xaRes = xaConn.getXAResource();
	} catch ( Exception except ) {
	    System.out.println( except );
	    return;
	}
	// Thread2 immediately goes to sleep, waits
	// for Thread1 to wake it up.
	try {
	    sleep( Integer.MAX_VALUE );
	} catch ( InterruptedException except ) { }


	// Begin a transaction on this connection.
	// Perform an update on the table. This will
	// lock since Thread1 is in a different transaction
	// updating the same table.
	System.out.println( "[Thread2] Starting transaction" );
	try {
	    xaRes.start( xid2, XAResource.TMNOFLAGS );
	} catch ( XAException except ) {
	    System.out.println( except );
	    return;	    
	}
	System.out.println( "[Thread2] Updating table" );
	try {
	    stmt = conn.createStatement();
	    stmt.executeUpdate( "update test set text='second' where id=1" );
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}


	// Thread1 will now proof that it owns the
	// transaction.
	System.out.println( "[Thread2] Waking up Thread1" );
	thread1.interrupt();
	try {
	    sleep( Integer.MAX_VALUE );
	} catch ( InterruptedException except ) { }


	// We will now join the transaction shared with
	// Thread1 and try to update the table again.
	System.out.println( "[Thread2] Dumping transaction" );
	try {
	    xaRes.end( xid2, XAResource.TMFAIL );
	    // xaRes.rollback( xid2 );
	    xaRes.forget( xid2 );
	} catch ( XAException except ) {
	    System.out.println( except );
	    return;	    
	}
	System.out.println( "[Thread2] Joining transaction of Thread1" );
	try {
	    xaRes.start( xid1, XAResource.TMJOIN );
	} catch ( XAException except ) {
	    System.out.println( except );
	    return;	    
	}
	System.out.println( "[Thread2] Updating table" );
	try {
	    stmt = conn.createStatement();
	    stmt.executeUpdate( "update test set text='second' where id=1" );
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}


	// Thread1 will now proof that it could update
	// the table.
	System.out.println( "[Thread2] Waking up Thread1" );
	thread1.interrupt();
	try {
	    sleep( Integer.MAX_VALUE );
	} catch ( InterruptedException except ) { }


	// We will now end the transaction and commit it.
	System.out.println( "[Thread2] Commiting transaction" );
	try {
	    xaRes.end( xid1, XAResource.TMSUCCESS );
	    xaRes.prepare( xid1 );
	    xaRes.commit( xid1, false );
	    xaRes.forget( xid1 );
	} catch ( XAException except ) {
	    System.out.println( except );
	    return;	    
	}


	// Perform a select on the table to prove that it
	// was only updated inside the transaction.
	System.out.println( "[Thread2] Selecting from table" );
	try {
	    stmt = conn.createStatement();
	    rs = stmt.executeQuery( "select text from test where id=1" );
	    rs.next();
	    System.out.println( "First = " + rs.getString( 1 ) );
	    rs.close();
	    stmt.close();
	} catch ( SQLException except ) {
	    System.out.println( except );
	}


	// Thread1 will now proof that the table was only
	// updated inside the transaction. Thread 2 will die.
	System.out.println( "[Thread2] Waking up Thread1" );
	thread1.interrupt();
    }


    javax.sql.XAConnection xaConn;


    Xid                    xid1;


    Xid                    xid2;


    Thread                 thread1;


}



class XidImpl
    implements Xid
{
    
    
    public byte[] getBranchQualifier()
    {
	return null;
    }
    
    
    public byte[] getGlobalTransactionId()
    {
	return null;
    }
    
    
    public int getFormatId()
    {
	return 0;
    }
    
    
}
