import java.io.*;
import java.lang.*;
import java.sql.*;

class JDBC_Test
{
  public JDBC_Test() 
  {
  }
  
  public static void main(String argv[])
  {
    if(argv.length<3) {
      System.err.println("java JDBC_Test jdbc-url user password [debug]");
      System.exit(1);
    }
    
    String url = new String(argv[0]);
    String usr = new String(argv[1]);
    String pwd = new String(argv[2]);
    Connection db;
    Statement s;
    
    // This line outputs debug information to stderr. To enable this, simply
    // add an extra parameter to the command line
    if(argv.length>3)
      DriverManager.setLogStream(System.err);
    
    // Load the driver
    try {
      Class.forName("postgresql.Driver");
    } catch (ClassNotFoundException e) {
      System.err.println("Exception: " + e.toString());
    }
    
    // Lets do a few things -- it doesn't do everything, but
    // it tests out basic functionality
    try {
      //----------------------------------------
      // Connect to database
      System.out.println("Connecting to Database URL = " + url);
      db = DriverManager.getConnection(url, usr, pwd);
      System.out.println("Connected...Now creating a statement");
      s = db.createStatement();
      
      //----------------------------------------
      // test DateStyle & Warnings
      System.out.println("Ok... now set European date style");
      s.executeUpdate("set datestyle='european'");
      
      System.out.println("and see what style we are now using (handled by warnings)");
      s.executeUpdate("show datestyle");
      SQLWarning sw = db.getWarnings();
      while(sw!=null) {
	System.out.println("--> "+sw.getMessage());
	sw=sw.getNextWarning();
      }
      db.clearWarnings();
      
      //----------------------------------------
      // Creating a table
      System.out.println("Ok...now we will create a table");
      s.executeUpdate("create table test (a int2, b int2,c timestamp,d date)");
      
      //----------------------------------------
      // Simple inserts
      System.out.println("Now we will insert some columns");
      s.executeUpdate("insert into test values (1, 1,'now','now')");
      s.executeUpdate("insert into test values (2, 1,'now','01-11-1997')"); // As we are in european, this should mean 1 November 1997
      s.executeUpdate("insert into test values (3, 1,'now','11-01-1997')"); // As we are in european, this should mean 11 January 1997
      System.out.println("Inserted some data");
      
      //----------------------------------------
      // Now a select (see seperate method at end)
      System.out.println("Now lets try a select");
      select(s,"");
      
      //----------------------------------------
      // Now run some tests
      runTests(db,s);
      
      //----------------------------------------
      // Dropping a table
      System.out.println("Ok...dropping the table");
      s.executeUpdate("drop table test");
      
      //----------------------------------------
      // Closing the connection
      System.out.println("Now closing the connection");
      s.close();
      db.close();
      
      //----------------------------------------
    } catch (SQLException e) {
      System.out.println("Exception: " + e.toString());
    }
  }
  
  /**
   * This performs some tests - not really part of an example, hence
   * they are in a seperate method.
   */
  public static void runTests(Connection db, Statement s) throws SQLException
  {
    //----------------------------------------
    // This is a bug at the moment... when you use set datestyle
    // it must be followed by show datestyle
    System.out.println("Now switch to US date format");
    s.executeUpdate("set datestyle='US'");
    s.executeUpdate("show datestyle");
    
    System.out.println("Now lets try a select");
    select(s,"");
    
    //----------------------------------------
    // Inserting dates using PreparedStatement
    System.out.println("Ok, now a test using PreparedStatement");
    Date dt = new Date(97,11,1);
    PreparedStatement ps = db.prepareStatement("insert into test values (?,?,'now',?)");
    
    // first insert in US style
    s.executeUpdate("set datestyle='US'");
    s.executeUpdate("show datestyle");
    ps.setInt(1,8);
    ps.setInt(2,8);
    ps.setDate(3,dt);
    ps.executeUpdate();
    
    // second insert in European style
    s.executeUpdate("set datestyle='european'");
    s.executeUpdate("show datestyle");
    ps.setInt(1,9);
    ps.setInt(2,9);
    ps.setDate(3,dt);
    ps.executeUpdate();
    
    System.out.println("Now run the select again - first as European");
    select(s,"where a>7");
    
    s.executeUpdate("set datestyle='US'");
    s.executeUpdate("show datestyle");
    System.out.println("Then as US");
    select(s,"where a>7");
  }
  
  /**
   * This performs a select. It's seperate because the tests use it in
   * multiple places.
   * @param s Statement to run under
   * @throws SQLException
   */
  public static void select(Statement s,String sql) throws SQLException
  {
    sql="select a, b,c,d from test "+sql;
    System.out.println("\nQuery: "+sql);
    ResultSet rs = s.executeQuery(sql);
    System.out.println("row	a	b	c                    	d          'd as string'");
    System.out.println("-------------------------------------------------------------------------------");
    int i = 0;
    while(rs.next()) {
      int a = rs.getInt("a");	// Example of retriving by column name
      int b = rs.getInt("b");
      Timestamp c = rs.getTimestamp(3); // Example of by column number
      java.sql.Date d = rs.getDate(4);  // Note, java.sql.Date here
      System.out.println("row " + i + "	" + a + "	" + b + "	" + c + "	" + d + " '"+rs.getString(4)+"'");
      i++;
    }
    rs.close();
  }
  
}
