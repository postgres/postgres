package example;

import java.io.*;
import java.sql.*;
import java.text.*;

/**
 * This example tests the various date styles that are available to postgresql.
 *
 * To use this example, you need a database to be in existence. This example
 * will create a table called datestyle.
 *
 */

public class datestyle
{
  Connection db;	// The connection to the database
  Statement  st;	// Our statement to run queries with
  
  // This is our standard to compare results with.
  java.sql.Date standard;
  
  // This is a list of the available date styles including variants.
  // These have to match what the "set datestyle" statement accepts.
  String styles[] = {
    "postgres,european",
    "postgres,us",
    "iso",		// iso has no variants - us/european has no affect
    "sql,european",
    "sql,us",
    "german"		// german has no variants - us/european has no affect
  };
  
  public datestyle(String args[]) throws ClassNotFoundException, FileNotFoundException, IOException, SQLException
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
    init();
    
    // Now run tests using JDBC methods
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
      st.executeUpdate("drop table datestyle");
    } catch(Exception ex) {
      // We ignore any errors here
    }
  }
  
  /**
   * This initialises the database for this example
   */
  public void init() throws SQLException
  {
    // Create a table holding a single date
    st.executeUpdate("create table datestyle (dt date)");
    
    // Now create our standard date for the test.
    //
    // NB: each component of the date should be different, otherwise the tests
    //     will not be valid.
    //
    // NB: January = 0 here
    //
    standard = new java.sql.Date(98,0,8);
    
    // Now store the result.
    //
    // This is an example of how to set a date in a date style independent way.
    // The only way of doing this is by using a PreparedStatement.
    //
    PreparedStatement ps = db.prepareStatement("insert into datestyle values (?)");
    ps.setDate(1,standard);
    ps.executeUpdate();
    ps.close();
    
  }
  
  /**
   * This performs the example
   */
  public void doexample() throws SQLException
  {
    System.out.println("\nRunning tests:");
    
    for(int i=0;i<styles.length;i++) {
      System.out.print("Test "+i+" - "+styles[i]);
      System.out.flush();
      
      // set the style
      st.executeUpdate("set datestyle='"+styles[i]+"'");
      
      // Now because the driver needs to know what the current style is,
      // we have to run the following:
      st.executeUpdate("show datestyle");
      // This is a limitation, but there is no real way around this.
      
      // Now we query the table.
      ResultSet rs = st.executeQuery("select dt from datestyle");
      
      // Throw an exception if there is no result (if the table is empty
      // there should still be a result).
      if(rs==null)
	throw new SQLException("The test query returned no data");
      
      while(rs.next()) {
	// The JDBC spec states we should only read each column once.
	// In the current implementation of the driver, this is not necessary.
	// Here we use this fact to see what the query really returned.
	if(standard.equals(rs.getDate(1)))
	  System.out.println(" passed, returned "+rs.getString(1));
	else
	  System.out.println(" failed, returned "+rs.getString(1));
      }
      rs.close();
    }
  }
  
  /**
   * Display some instructions on how to run the example
   */
  public static void instructions()
  {
    System.out.println("\nThis example tests the drivers ability to handle dates correctly if the\nbackend is running any of the various date styles that it supports.\nIdealy this should work fine. If it doesn't, then there is something wrong\npossibly in postgresql.Connection or in the backend itself. If this does occur\nthen please email a bug report.\n");
    System.out.println("Useage:\n java example.datestyle jdbc:postgresql:database user password [debug]\n\nThe debug field can be anything. It's presence will enable DriverManager's\ndebug trace. Unless you want to see screens of items, don't put anything in\nhere.");
    System.exit(1);
  }
  
  /**
   * This little lot starts the test
   */
  public static void main(String args[])
  {
    System.out.println("PostgreSQL datestyle test v6.3 rev 1\n");
    
    if(args.length<3)
      instructions();
    
    // This line outputs debug information to stderr. To enable this, simply
    // add an extra parameter to the command line
    if(args.length>3)
      DriverManager.setLogStream(System.err);
    
    // Now run the tests
    try {
      datestyle test = new datestyle(args);
    } catch(Exception ex) {
      System.err.println("Exception caught.\n"+ex);
      ex.printStackTrace();
    }
  }
}
