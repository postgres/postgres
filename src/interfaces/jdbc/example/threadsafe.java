package example;

import java.io.*;
import java.sql.*;
import java.text.*;

// rare in user code, but we use the LargeObject API in this test
import org.postgresql.largeobject.*;

/**
 * This example tests the thread safety of the driver.
 *
 * It does this by performing several queries, in different threads. Each
 * thread has it's own Statement object, which is (in my understanding of the
 * jdbc specification) the minimum requirement.
 *
 */

public class threadsafe
{
  Connection db;	// The connection to the database
  Statement  st;	// Our statement to run queries with
  
  public threadsafe(String args[]) throws ClassNotFoundException, FileNotFoundException, IOException, SQLException
  {
    String url = args[0];
    String usr = args[1];
    String pwd = args[2];
    
    // Load the driver
    Class.forName("org.postgresql.Driver");
    
    // Connect to database
    System.out.println("Connecting to Database URL = " + url);
    db = DriverManager.getConnection(url, usr, pwd);
    
    System.out.println("Connected...Now creating a statement");
    st = db.createStatement();
    
    // Clean up the database (in case we failed earlier) then initialise
    cleanup();
    
    // Because we use LargeObjects, we must use Transactions
    db.setAutoCommit(false);
    
    // Now run tests using JDBC methods, then LargeObjects
    doexample();
    
    // Clean up the database
    cleanup();
    
    // Finally close the database
    System.out.println("Now closing the connection");
    st.close();
    db.close();
    
  }
  
  /**
   * This drops the table (if it existed). No errors are reported.
   */
  public void cleanup()
  {
    try {
      st.executeUpdate("drop table basic1");
    } catch(Exception ex) {
      // We ignore any errors here
    }
    
    try {
      st.executeUpdate("drop table basic2");
    } catch(Exception ex) {
      // We ignore any errors here
    }
  }
  
  /**
   * This performs the example
   */
  public void doexample() throws SQLException
  {
    System.out.println("\nThis test runs three Threads. Two simply insert data into a table, then\nthey perform a query. While they are running, a third thread is running,\nand it load data into, then reads from a Large Object.\n\nIf alls well, this should run without any errors. If so, we are Thread Safe.\nWhy test JDBC & LargeObject's? Because both will run over the network\nconnection, and if locking on the stream isn't done correctly, the backend\nwill get pretty confused!\n");
    
    thread3 thread3=null;
    
    try {
      
      // create the two threads
      Thread thread0 = Thread.currentThread();
      Thread thread1 = new thread1(db);
      Thread thread2 = new thread2(db);
      thread3 = new thread3(db);
      
      // now run, and wait for them
      thread1.start();
      thread2.start();
      thread3.start();
      
      // ok, I know this is bad, but it does the trick here as our main thread
      // will yield as long as either of the children are still running
      System.out.println("Waiting for threads to run");
      while(thread1.isAlive() || thread2.isAlive() || thread3.isAlive())
	thread0.yield();
      
    } finally {
      // clean up after thread3 (the finally ensures this is run even
      // if an exception is thrown inside the try { } construct)
      if(thread3 != null)
	thread3.cleanup();
    }
    
    System.out.println("No Exceptions have been thrown. This is a good omen, as it means that we are\npretty much thread safe as we can get.");
  }
  
  // This is the first thread. It's the same as the basic test
  class thread1 extends Thread
  {
    Connection c;
    Statement st;
    
    public thread1(Connection c) throws SQLException {
      this.c = c;
      st = c.createStatement();
    }
    
    public void run() {
      try {
	System.out.println("Thread 1 running...");
	
	// First we need a table to store data in
	st.executeUpdate("create table basic1 (a int2, b int2)");
	
	// Now insert some data, using the Statement
	st.executeUpdate("insert into basic1 values (1,1)");
	st.executeUpdate("insert into basic1 values (2,1)");
	st.executeUpdate("insert into basic1 values (3,1)");
	
	// For large inserts, a PreparedStatement is more efficient, because it
	// supports the idea of precompiling the SQL statement, and to store
	// directly, a Java object into any column. PostgreSQL doesnt support
	// precompiling, but does support setting a column to the value of a
	// Java object (like Date, String, etc).
	//
	// Also, this is the only way of writing dates in a datestyle independent
	// manner. (DateStyles are PostgreSQL's way of handling different methods
	// of representing dates in the Date data type.)
	PreparedStatement ps = db.prepareStatement("insert into basic1 values (?,?)");
	for(int i=2;i<2000;i++) {
	  ps.setInt(1,4);		// "column a" = 5
	  ps.setInt(2,i);		// "column b" = i
	  ps.executeUpdate();	// executeUpdate because insert returns no data
//	  c.commit();
	  if((i%50)==0)
	    DriverManager.println("Thread 1 done "+i+" inserts");
	}
	ps.close();			// Always close when we are done with it
	
	// Finally perform a query on the table
	DriverManager.println("Thread 1 performing a query");
	ResultSet rs = st.executeQuery("select a, b from basic1");
	int cnt=0;
	if(rs!=null) {
	  // Now we run through the result set, printing out the result.
	  // Note, we must call .next() before attempting to read any results
	  while(rs.next()) {
	    int a = rs.getInt("a");	// This shows how to get the value by name
	    int b = rs.getInt(2);	// This shows how to get the value by column
	    //System.out.println("  a="+a+" b="+b);
	    cnt++;
	  }
	  rs.close();	// again, you must close the result when done
	}
	DriverManager.println("Thread 1 read "+cnt+" rows");
	
	// The last thing to do is to drop the table. This is done in the
	// cleanup() method.
	System.out.println("Thread 1 finished");
      } catch(SQLException se) {
	System.err.println("Thread 1: "+se.toString());
	se.printStackTrace();
	System.exit(1);
      }
    }
  }
  
  // This is the second thread. It's the similar to the basic test, and thread1
  // except it works on another table.
  class thread2 extends Thread
  {
    Connection c;
    Statement st;
    
    public thread2(Connection c) throws SQLException {
      this.c = c;
      st = c.createStatement();
    }
    
    public void run() {
      try {
	System.out.println("Thread 2 running...");
	
	// First we need a table to store data in
	st.executeUpdate("create table basic2 (a int2, b int2)");
	
	// For large inserts, a PreparedStatement is more efficient, because it
	// supports the idea of precompiling the SQL statement, and to store
	// directly, a Java object into any column. PostgreSQL doesnt support
	// precompiling, but does support setting a column to the value of a
	// Java object (like Date, String, etc).
	//
	// Also, this is the only way of writing dates in a datestyle independent
	// manner. (DateStyles are PostgreSQL's way of handling different methods
	// of representing dates in the Date data type.)
	PreparedStatement ps = db.prepareStatement("insert into basic2 values (?,?)");
	for(int i=2;i<2000;i++) {
	  ps.setInt(1,4);		// "column a" = 5
	  ps.setInt(2,i);		// "column b" = i
	  ps.executeUpdate();	// executeUpdate because insert returns no data
//	  c.commit();
	  if((i%50)==0)
	    DriverManager.println("Thread 2 done "+i+" inserts");
	}
	ps.close();			// Always close when we are done with it
	
	// Finally perform a query on the table
	DriverManager.println("Thread 2 performing a query");
	ResultSet rs = st.executeQuery("select * from basic2 where b>1");
	int cnt=0;
	if(rs!=null) {
	  // First find out the column numbers.
	  //
	  // It's best to do this here, as calling the methods with the column
	  // numbers actually performs this call each time they are called. This
	  // really speeds things up on large queries.
	  //
	  int col_a = rs.findColumn("a");
	  int col_b = rs.findColumn("b");
	  
	  // Now we run through the result set, printing out the result.
	  // Again, we must call .next() before attempting to read any results
	  while(rs.next()) {
	    int a = rs.getInt(col_a); // This shows how to get the value by name
	    int b = rs.getInt(col_b); // This shows how to get the value by column
	    //System.out.println("  a="+a+" b="+b);
	    cnt++;
	  }
	  rs.close();	// again, you must close the result when done
	}
	DriverManager.println("Thread 2 read "+cnt+" rows");
	
	// The last thing to do is to drop the table. This is done in the
	// cleanup() method.
	System.out.println("Thread 2 finished");
      } catch(SQLException se) {
	System.err.println("Thread 2: "+se.toString());
	se.printStackTrace();
	System.exit(1);
      }
    }
  }
  
  // This is the third thread. It loads, then reads from a LargeObject, using
  // our LargeObject api.
  //
  // The purpose of this is to test that FastPath will work in between normal
  // JDBC queries.
  class thread3 extends Thread
  {
    Connection c;
    Statement st;
    LargeObjectManager lom;
    LargeObject lo;
    int oid;
    
    public thread3(Connection c) throws SQLException {
      this.c = c;
      //st = c.createStatement();
      
      // create a blob
      lom = ((org.postgresql.Connection)c).getLargeObjectAPI();
      oid = lom.create();
      System.out.println("Thread 3 has created a blob of oid "+oid);
    }
    
    public void run() {
      try {
	System.out.println("Thread 3 running...");
	
	DriverManager.println("Thread 3: Loading data into blob "+oid);
	lo = lom.open(oid);
	FileInputStream fis = new FileInputStream("example/threadsafe.java");
	// keep the buffer size small, to allow the other thread a chance
	byte buf[] = new byte[128];
	int rc,bc=1,bs=0;
	while((rc=fis.read(buf))>0) {
	  DriverManager.println("Thread 3 read block "+bc+" "+bs+" bytes");
	  lo.write(buf,0,rc);
	  bc++;
	  bs+=rc;
	}
	lo.close();
	fis.close();
	
	DriverManager.println("Thread 3: Reading blob "+oid);
	lo=lom.open(oid);
	bc=0;
	while(buf.length>0) {
	  buf=lo.read(buf.length);
	  if(buf.length>0) {
	    String s = new String(buf);
	    bc++;
	    DriverManager.println("Thread 3 block "+bc);
	    DriverManager.println("Block "+bc+" got "+s);
	  }
	}
	lo.close();
	
	System.out.println("Thread 3 finished");
      } catch(Exception se) {
	System.err.println("Thread 3: "+se.toString());
	se.printStackTrace();
	System.exit(1);
      }
    }
    
    public void cleanup() throws SQLException {
      if(lom!=null && oid!=0) {
	System.out.println("Thread 3: Removing blob oid="+oid);
	lom.delete(oid);
      }
    }
  }
  
  /**
   * Display some instructions on how to run the example
   */
  public static void instructions()
  {
    System.out.println("\nThis tests the thread safety of the driver.\n\nThis is done in two parts, the first with standard JDBC calls, and the\nsecond mixing FastPath and LargeObject calls with queries.\n");
    System.out.println("Useage:\n java example.threadsafe jdbc:postgresql:database user password [debug]\n\nThe debug field can be anything. It's presence will enable DriverManager's\ndebug trace. Unless you want to see screens of items, don't put anything in\nhere.");
    System.exit(1);
  }
  
  /**
   * This little lot starts the test
   */
  public static void main(String args[])
  {
    System.out.println("PostgreSQL Thread Safety test v6.4 rev 1\n");
    
    if(args.length<3)
      instructions();
    
    // This line outputs debug information to stderr. To enable this, simply
    // add an extra parameter to the command line
    if(args.length>3)
      DriverManager.setLogStream(System.err);
    
    // Now run the tests
    try {
      threadsafe test = new threadsafe(args);
    } catch(Exception ex) {
      System.err.println("Exception caught.\n"+ex);
      ex.printStackTrace();
    }
  }
}
