package example;

import java.io.*;
import java.sql.*;
import java.text.*;

/**
 * This example application demonstrates some of the drivers other features
 * by implementing a simple psql replacement in Java.
 *
 */

public class psql
{
  Connection	   db;		// The connection to the database
  Statement	   st;		// Our statement to run queries with
  DatabaseMetaData dbmd;	// This defines the structure of the database
  
  public psql(String args[]) throws ClassNotFoundException, FileNotFoundException, IOException, SQLException
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
    
    System.out.println();
    
    // This provides us the means of reading from stdin
    StreamTokenizer input = new StreamTokenizer(new InputStreamReader(System.in));
    input.resetSyntax();
    input.slashSlashComments(true);	// allow // as a comment delimiter
    input.eolIsSignificant(false);	// treat eol's as spaces
    input.wordChars(32,126);
    input.whitespaceChars(59,59);
    input.quoteChar(39);
    
    // Now the main loop.
    int tt=0,lineno=1;
    while(tt!=StreamTokenizer.TT_EOF) {
      System.out.print("["+lineno+"] ");
      System.out.flush();
      
      // Here, we trap SQLException so they don't terminate the application
      try {
	if((tt=input.nextToken())==StreamTokenizer.TT_WORD) {
	  processLine(input.sval);
	  lineno++;
	}
      } catch(SQLException ex) {
	System.out.println(ex.getMessage());
      }
    }
    
    System.out.println("Now closing the connection");
    st.close();
    db.close();
    
  }
  
  /**
   * This processes a statement
   */
  public void processLine(String line) throws SQLException
  {
    if(line.startsWith("\\")) {
      processSlashCommand(line);
      return;
    }
    
    boolean type = st.execute(line);
    boolean loop=true;
    while(loop) {
      if(type) {
	// A ResultSet was returned
	ResultSet rs=st.getResultSet();
	displayResult(rs);
      } else {
	int count = st.getUpdateCount();
	
	if(count==-1) {
	  // This indicates nothing left
	  loop=false;
	} else {
	  // An update count was returned
	  System.out.println("Updated "+st.getUpdateCount()+" rows");
	}
      }
      
      if(loop)
	type = st.getMoreResults();
    }
  }
  
  /**
   * This displays a result set.
   * Note: it closes the result once complete.
   */
  public void displayResult(ResultSet rs) throws SQLException
  {
    ResultSetMetaData rsmd = rs.getMetaData();
    
    // Print the result column names
    int cols = rsmd.getColumnCount();
    for(int i=1;i<=cols;i++)
      System.out.print(rsmd.getColumnLabel(i)+(i<cols?"\t":"\n"));
    
    // now the results
    while(rs.next()) {
      for(int i=1;i<=cols;i++) {
	Object o = rs.getObject(i);
	if(rs.wasNull())
	  System.out.print("{null}"+(i<cols?"\t":"\n"));
	else
	  System.out.print(o.toString()+(i<cols?"\t":"\n"));
      }
    }
    
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
    System.out.println("\nThis example shows how some of the other JDBC features work within the\ndriver. It does this by implementing a very simple psql equivalent in java.\nNot everything that psql does is implemented.\n");
    System.out.println("Useage:\n java example.psql jdbc:postgresql:database user password [debug]\n\nThe debug field can be anything. It's presence will enable DriverManager's\ndebug trace. Unless you want to see screens of items, don't put anything in\nhere.");
    System.exit(1);
  }
  
  /**
   * This little lot starts the test
   */
  public static void main(String args[])
  {
    System.out.println("PostgreSQL psql example v6.3 rev 1\n");
    
    if(args.length<3)
      instructions();
    
    // This line outputs debug information to stderr. To enable this, simply
    // add an extra parameter to the command line
    if(args.length>3)
      DriverManager.setLogStream(System.err);
    
    // Now run the tests
    try {
      psql test = new psql(args);
    } catch(Exception ex) {
      System.err.println("Exception caught.\n"+ex);
      ex.printStackTrace();
    }
  }
}
