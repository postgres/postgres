package example;

import java.io.*;
import java.sql.*;
import java.text.*;

/**
 * This example application is not really an example. It actually performs
 * some tests on various methods in the DatabaseMetaData and ResultSetMetaData
 * classes.
 *
 * To use it, simply have a database created. It will create some work tables
 * and run tests on them.
 */

public class metadata
{
  Connection	   db;		// The connection to the database
  Statement	   st;		// Our statement to run queries with
  DatabaseMetaData dbmd;	// This defines the structure of the database
  
  /**
   * These are the available tests on DatabaseMetaData
   */
  public void doDatabaseMetaData() throws SQLException {
    if(doTest("getProcedures() - should show all available procedures"))
      displayResult(dbmd.getProcedures(null,null,null));
    
    if(doTest("getProcedures() with pattern - should show all circle procedures"))
      displayResult(dbmd.getProcedures(null,null,"circle%"));
    
    if(doTest("getProcedureColumns() on circle procedures"))
      displayResult(dbmd.getProcedureColumns(null,null,"circle%",null));
    
    if(doTest("getTables()"))
      displayResult(dbmd.getTables(null,null,null,null));
    
    if(doTest("getColumns() - should show all tables, can take a while to run"))
      displayResult(dbmd.getColumns(null,null,null,null));
    
    if(doTest("getColumns() - should show the test_b table"))
      displayResult(dbmd.getColumns(null,null,"test_b",null));
    
    if(doTest("getColumnPrivileges() - should show all tables"))
      displayResult(dbmd.getColumnPrivileges(null,null,null,null));
    
    if(doTest("getPrimaryKeys()"))
      displayResult(dbmd.getPrimaryKeys(null,null,null));
    
    if(doTest("getTypeInfo()"))
      displayResult(dbmd.getTypeInfo());
    
  }
  
  /**
   * These are the available tests on ResultSetMetaData
   */
  public void doResultSetMetaData() throws SQLException {
    
    String sql = "select imagename,descr,source,cost from test_a,test_b,test_c where test_a.id=test_b.imageid and test_a.id=test_c.imageid";
    
    System.out.println("Executing query for tests");
    ResultSet rs = st.executeQuery(sql);
    ResultSetMetaData rsmd = rs.getMetaData();
    
    if(doTest("isCurrency()"))
      System.out.println("isCurrency on col 1 = "+rsmd.isCurrency(1)+" should be false\nisCurrency on col 4 = "+rsmd.isCurrency(4)+" should be true");
    
    // Finally close the query. Now give the user a chance to display the
    // ResultSet.
    //
    // NB: displayResult() actually closes the ResultSet.
    if(doTest("Display query result")) {
      System.out.println("Query: "+sql);
      displayResult(rs);
    } else
      rs.close();
  }
  
  /**
   * This creates some test data
   */
  public void init() throws SQLException {
    System.out.println("Creating some tables");
    cleanup();
    st.executeUpdate("create table test_a (imagename name,image oid,id int4)");
    st.executeUpdate("create table test_b (descr text,imageid int4,id int4)");
    st.executeUpdate("create table test_c (source text,cost money,imageid int4)");
    
    System.out.println("Adding some data");
    st.executeUpdate("insert into test_a values ('test1',0,1)");
    st.executeUpdate("insert into test_b values ('A test description',1,2)");
    st.executeUpdate("insert into test_c values ('nowhere particular','$10.99',1)");
  }
  
  /**
   * This removes the test data
   */
  public void cleanup() throws SQLException {
    try {
      st.executeUpdate("drop table test_a");
      st.executeUpdate("drop table test_b");
      st.executeUpdate("drop table test_c");
    } catch(Exception ex) {
      // We ignore any errors here
    }
  }
  
  public metadata(String args[]) throws ClassNotFoundException, FileNotFoundException, IOException, SQLException
  {
    String url = args[0];
    String usr = args[1];
    String pwd = args[2];
    
    // Load the driver
    Class.forName("org.postgresql.Driver");
    
    // Connect to database
    System.out.println("Connecting to Database URL = " + url);
    db = DriverManager.getConnection(url, usr, pwd);
    
    dbmd = db.getMetaData();
    st = db.createStatement();
    
    // This prints the backend's version
    System.out.println("Connected to "+dbmd.getDatabaseProductName()+" "+dbmd.getDatabaseProductVersion());
    
    init();
    
    System.out.println();
    
    // Now the tests
    if(doTest("Test DatabaseMetaData"))
      doDatabaseMetaData();
    
    if(doTest("Test ResultSetMetaData"))
      doResultSetMetaData();
    
    System.out.println("\nNow closing the connection");
    st.close();
    db.close();
    
    cleanup();
  }
  
  /**
   * This asks if the user requires to run a test.
   */
  public boolean doTest(String s) {
    System.out.println();
    System.out.print(s);
    System.out.print(" Perform test? Y or N:");
    System.out.flush();
    char c = ' ';
    try {
      while(!(c=='n' || c=='y' || c=='N' || c=='Y')) {
	c=(char)System.in.read();
      }
    } catch(IOException ioe) {
      return false;
    }
    
    return c=='y' || c=='Y';
  }
  
  /**
   * This displays a result set.
   * Note: it closes the result once complete.
   */
  public void displayResult(ResultSet rs) throws SQLException
  {
    ResultSetMetaData rsmd = rs.getMetaData();
    int count=0;
    
    // Print the result column names
    int cols = rsmd.getColumnCount();
    for(int i=1;i<=cols;i++)
      System.out.print(rsmd.getColumnLabel(i)+(i<cols?"\t":"\n"));
    
    // now the results
    while(rs.next()) {
      count++;
      for(int i=1;i<=cols;i++) {
	Object o = rs.getObject(i);
	if(rs.wasNull())
	  System.out.print("{null}"+(i<cols?"\t":"\n"));
	else
	  System.out.print(o.toString()+(i<cols?"\t":"\n"));
      }
    }
    
    System.out.println("Result returned "+count+" rows.");
    
    // finally close the result set
    rs.close();
  }
  
  /**
   * This process / commands (for now just /d)
   */
  public void processSlashCommand(String line) throws SQLException
  {
    if(line.startsWith("\\d")) {
      
      if(line.startsWith("\\d ")) {
	// Display details about a table
	String table=line.substring(3);
	displayResult(dbmd.getColumns(null,null,table,"%"));
      } else {
	String types[] = null;
	if(line.equals("\\d"))
	  types=allUserTables;
	else if(line.equals("\\di"))
	  types=usrIndices;
	else if(line.equals("\\dt"))
	  types=usrTables;
	else if(line.equals("\\ds"))
	  types=usrSequences;
	else if(line.equals("\\dS"))
	  types=sysTables;
	else
	  throw new SQLException("Unsupported \\d command: "+line);
	
	// Display details about all system tables
	//
	// Note: the first two arguments are ignored. To keep to the spec,
	//       you must put null here
	//
	displayResult(dbmd.getTables(null,null,"%",types));
      }
    } else
      throw new SQLException("Unsupported \\ command: "+line);
  }
  
  private static final String allUserTables[] = {"TABLE","INDEX","SEQUENCE"};
  private static final String usrIndices[] = {"INDEX"};
  private static final String usrTables[] = {"TABLE"};
  private static final String usrSequences[] = {"SEQUENCE"};
  private static final String sysTables[] = {"SYSTEM TABLE","SYSTEM INDEX"};
  
  /**
   * Display some instructions on how to run the example
   */
  public static void instructions()
  {
    System.out.println("\nThis is not really an example, but is used to test the various methods in\nthe DatabaseMetaData and ResultSetMetaData classes.\n");
    System.out.println("Useage:\n java example.metadata jdbc:postgresql:database user password [debug]\n\nThe debug field can be anything. It's presence will enable DriverManager's\ndebug trace. Unless you want to see screens of debug items, don't put anything in\nhere.");
    System.exit(1);
  }
  
  /**
   * This little lot starts the test
   */
  public static void main(String args[])
  {
    System.out.println("PostgreSQL metdata tester v6.4 rev 1\n");
    
    if(args.length<3)
      instructions();
    
    // This line outputs debug information to stderr. To enable this, simply
    // add an extra parameter to the command line
    if(args.length>3)
      DriverManager.setLogStream(System.err);
    
    // Now run the tests
    try {
      metadata test = new metadata(args);
    } catch(Exception ex) {
      System.err.println("Exception caught.\n"+ex);
      ex.printStackTrace();
    }
  }
}
