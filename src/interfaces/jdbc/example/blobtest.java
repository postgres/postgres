package example;

import java.io.*;
import java.sql.*;
import org.postgresql.largeobject.*;

/**
 * This test attempts to create a blob in the database, then to read
 * it back.
 *
 * Important note: You will notice we import the org.postgresql.largeobject
 * package, but don't import the org.postgresql package. The reason for this is
 * that importing postgresql can confuse javac (we have conflicting class names
 * in org.postgresql.* and java.sql.*). This doesn't cause any problems, as
 * long as no code imports org.postgresql.
 *
 * Under normal circumstances, code using any jdbc driver only needs to import
 * java.sql, so this isn't a problem.
 *
 * It's only if you use the non jdbc facilities, do you have to take this into
 * account.
 *
 */

public class blobtest
{
  Connection db;
  Statement s;
  LargeObjectManager lobj;
  
  public blobtest(String args[]) throws ClassNotFoundException, FileNotFoundException, IOException, SQLException
  {
    String url = args[0];
    String usr = args[1];
    String pwd = args[2];
    
    // Load the driver
    Class.forName("org.postgresql.Driver");
    
    // Connect to database
    System.out.println("Connecting to Database URL = " + url);
    db = DriverManager.getConnection(url, usr, pwd);
    
    // This is required for all LargeObject calls
    System.out.println("Connected... First turn off autoCommit()");
    db.setAutoCommit(false);
    
    System.out.println("Now creating a statement");
    s = db.createStatement();
    
    // Now run tests using postgresql's own Large object api
    // NOTE: The methods shown in this example are _NOT_ JDBC, but are
    // an implementation of the calls found in libpq. Unless you need to
    // use this functionality, look at the jdbc tests on how to access blobs.
    ownapi();
    
    // Now run tests using JDBC methods
    //jdbcapi(db,s);
    
    // Finally close the database
    System.out.println("Now closing the connection");
    s.close();
    db.close();
    
  }
  
  /**
   * Now this is an extension to JDBC, unique to postgresql. Here we fetch
   * an PGlobj object, which provides us with access to postgresql's
   * large object api.
   */
  public void ownapi() throws FileNotFoundException, IOException, SQLException
  {
    System.out.println("\n----------------------------------------------------------------------\nTesting postgresql large object api\n----------------------------------------------------------------------\n");
    
    // Internally, the driver provides JDBC compliant methods to access large
    // objects, however the unique methods available to postgresql makes
    // things a little easier.
    System.out.println("Gaining access to large object api");
    lobj = ((org.postgresql.Connection)db).getLargeObjectAPI();
    
    int oid = ownapi_test1();
    ownapi_test2(oid);
    
    // Now call the jdbc2api test
    jdbc2api(oid);
    
    // finally delete the large object
    ownapi_test3(oid);
    System.out.println("\n\nOID="+oid);
  }
  
  private int ownapi_test1() throws FileNotFoundException, IOException, SQLException
  {
    System.out.println("Test 1 Creating a large object\n");
    
    // Ok, test 1 is to create a large object. To do this, we use the create
    // method.
    System.out.println("Creating a large object");
    int oid = lobj.create(LargeObjectManager.READ|LargeObjectManager.WRITE);
    DriverManager.println("got large object oid="+oid);
    
    LargeObject obj = lobj.open(oid,LargeObjectManager.WRITE);
    DriverManager.println("got large object obj="+obj);
    
    // Now open a test file - this class will do
    System.out.println("Opening test source object");
    FileInputStream fis = new FileInputStream("example/blobtest.java");
    
    // copy the data
    System.out.println("Copying file to large object");
    byte buf[] = new byte[2048];
    int s,tl=0;
    while((s=fis.read(buf,0,2048))>0) {
      System.out.println("Block size="+s+" offset="+tl);
      //System.out.write(buf);
      obj.write(buf,0,s);
      tl+=s;
    }
    DriverManager.println("Copied "+tl+" bytes");
    
    // Close the object
    System.out.println("Closing object");
    obj.close();
    
    return oid;
  }
  
  private void ownapi_test2(int oid) throws FileNotFoundException, IOException, SQLException
  {
    System.out.println("Test 2 Reading a large object and save as a file\n");
    
    // Now open the large object
    System.out.println("Opening large object "+oid);
    LargeObject obj = lobj.open(oid,LargeObjectManager.READ);
    DriverManager.println("got obj="+obj);
    
    // Now open a test file - this class will do
    System.out.println("Opening test destination object");
    FileOutputStream fos = new FileOutputStream("blob_testoutput");
    
    // copy the data
    System.out.println("Copying large object to file");
    byte buf[] = new byte[512];
    int s=obj.size();
    int tl=0;
    while(s>0) {
      int rs = buf.length;
      if(s<rs) rs=s;
      obj.read(buf,0,rs);
      fos.write(buf,0,rs);
      tl+=rs;
      s-=rs;
    }
    DriverManager.println("Copied "+tl+"/"+obj.size()+" bytes");
    
    // Close the object
    System.out.println("Closing object");
    obj.close();
  }
  
  private void ownapi_test3(int oid) throws SQLException
  {
    System.out.println("Test 3 Deleting a large object\n");
    
    // Now open the large object
    System.out.println("Deleting large object "+oid);
    lobj.unlink(oid);
  }
  
    //=======================================================================
    // This tests the Blob interface of the JDBC 2.0 specification
    public void jdbc2api(int oid) throws SQLException, IOException
    {
	System.out.println("Testing JDBC2 Blob interface:");
	jdbc2api_cleanup();
	
	System.out.println("Creating Blob on large object "+oid);
	s.executeUpdate("create table basic (a oid)");
	
	System.out.println("Inserting row");
	s.executeUpdate("insert into basic values ("+oid+")");
	
	System.out.println("Selecting row");
	ResultSet rs = s.executeQuery("select a from basic");
	if(rs!=null) {
	    while(rs.next()) {
		System.out.println("Fetching Blob");
		Blob b = rs.getBlob("a");
		System.out.println("Blob.length() = "+b.length());
		System.out.println("Characters 400-500:");
		System.out.write(b.getBytes(400l,100));
		System.out.println();
	    }
	    rs.close();
	}
	
	System.out.println("Cleaning up");
	jdbc2api_cleanup();
    }
    
    private void jdbc2api_cleanup() throws SQLException
    {
	db.setAutoCommit(true);
	try {
	    s.executeUpdate("drop table basic");
	} catch(Exception ex) {
	    // We ignore any errors here
	}
	db.setAutoCommit(false);
    }
    
    //=======================================================================
  
  public static void instructions()
  {
    System.err.println("java example.blobtest jdbc-url user password [debug]");
    System.err.println("\nExamples:\n");
    System.err.println("java -Djdbc.driver=org.postgresql.Driver example.blobtest jdbc:postgresql:test postgres password\nThis will run the tests on the database test on the local host.\n");
    System.err.println("java -Djdbc.driver=org.postgresql.Driver example.blobtest jdbc:postgresql:test postgres password debug\nThis is the same as above, but will output debug information.\n");
    
    System.err.println("This example tests the binary large object api of the driver.\nThis allows images or java objects to be stored in the database, and retrieved\nusing both postgresql's own api, and the standard JDBC api.");
  }
  
  public static void main(String args[])
  {
    System.out.println("PostgreSQL blobtest v7.0 rev 1\n");
    
    if(args.length<3) {
      instructions();
      System.exit(1);
    }
    
    // This line outputs debug information to stderr. To enable this, simply
    // add an extra parameter to the command line
    if(args.length>3)
      DriverManager.setLogStream(System.err);
    
    // Now run the tests
    try {
      blobtest test = new blobtest(args);
    } catch(Exception ex) {
      System.err.println("Exception caught.\n"+ex);
      ex.printStackTrace();
    }
  }
}
