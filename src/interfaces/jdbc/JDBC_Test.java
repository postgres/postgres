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
    String url = new String(argv[0]);
    String usr = new String(argv[1]);
    String pwd = new String(argv[2]);
    Connection db;
    Statement s;
    ResultSet rs;
    
    // This line outputs debug information to stderr. To enable this, simply
    // remove the //
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
      System.out.println("Connecting to Database URL = " + url);
      db = DriverManager.getConnection(url, usr, pwd);
      System.out.println("Connected...Now creating a statement");
      s = db.createStatement();
      
      // test Date & Warnings
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
      
      System.out.println("Ok...now we will create a table");
      s.executeUpdate("create table test (a int2, b int2,c timestamp,d date)");
      
      System.out.println("Now we will insert some columns");
      s.executeUpdate("insert into test values (1, 1,'now','now')");
      s.executeUpdate("insert into test values (2, 1,'now','01-11-1997')"); // As we are in european, this should mean 1 November 1997
      s.executeUpdate("insert into test values (3, 1,'now','11-01-1997')"); // As we are in european, this should mean 11 January 1997
      System.out.println("Inserted some data");
      
      System.out.println("Now lets try a select");
      rs = s.executeQuery("select a, b,c,d from test");
      System.out.println("Back from the select...the following are results");
      System.out.println("row	a	b	c                    	d          'd as string'");
      int i = 0;
      while (rs.next())
	{
	  int a = rs.getInt("a");	// Example of retriving by column name
	  int b = rs.getInt("b");
	  Timestamp c = rs.getTimestamp(3); // Example of by column number
	  java.sql.Date d = rs.getDate(4);  // Note, java.sql.Date here
	  System.out.println("row " + i + "	" + a + "	" + b + "	" + c + "	" + d + " '"+rs.getString(4)+"'");
	  i++;
	}
      
      // This is a bug at the moment... when you use set datestyle
      // it must be followed by show datestyle
      System.out.println("Now switch to US date format");
      s.executeUpdate("set datestyle='US'");
      s.executeUpdate("show datestyle");
      
      System.out.println("Now lets try a select");
      rs = s.executeQuery("select a, b,c,d from test");
      System.out.println("Back from the select...the following are results");
      //int i = 0;
      System.out.println("row	a	b	c                    	d          'd as string'");
      while (rs.next())
	{
	  int a = rs.getInt("a");	// Example of retriving by column name
	  int b = rs.getInt("b");
	  Timestamp c = rs.getTimestamp(3); // Example of by column number
	  java.sql.Date d = rs.getDate(4);  // Note, java.sql.Date here
	  System.out.println("row " + i + "	" + a + "	" + b + "	" + c + "	" + d + " '"+rs.getString(4)+"'");
	  i++;
	}
      
      System.out.println("Ok...dropping the table");
      s.executeUpdate("drop table test");
      
      System.out.println("Now closing the connection");
      s.close();
      db.close();
    } catch (SQLException e) {
      System.out.println("Exception: " + e.toString());
    }
  }
}
