package postgresql;

import java.io.*;
import java.lang.*;
import java.net.*;
import java.util.*;
import java.sql.*;
import postgresql.fastpath.*;
import postgresql.largeobject.*;
import postgresql.util.*;

/**
 * A Connection represents a session with a specific database.  Within the
 * context of a Connection, SQL statements are executed and results are
 * returned.
 *
 * <P>A Connection's database is able to provide information describing
 * its tables, its supported SQL grammar, its stored procedures, the
 * capabilities of this connection, etc.  This information is obtained
 * with the getMetaData method.
 *
 * <p><B>Note:</B> By default, the Connection automatically commits changes
 * after executing each statement.  If auto-commit has been disabled, an
 * explicit commit must be done or database changes will not be saved.
 *
 * @see java.sql.Connection
 */
public class Connection implements java.sql.Connection 
{
  // This is the network stream associated with this connection
  protected PG_Stream pg_stream;
  
  // This is set by postgresql.Statement.setMaxRows()
  protected int maxrows = 0;		// maximum no. of rows; 0 = unlimited
  
  private String PG_HOST;
  private int PG_PORT;
  private String PG_USER;
  private String PG_PASSWORD;
  private String PG_DATABASE;
  private boolean PG_STATUS;
  
  public boolean CONNECTION_OK = true;
  public boolean CONNECTION_BAD = false;
  
  //private static final int STARTUP_LEN  = 288;	// Length of a startup packet
  
  // These are defined in src/include/libpq/pqcomm.h
  //private int STARTUP_CODE = STARTUP_USER;
  //private static final int STARTUP_USER = 7;	// User auth
  //private static final int STARTUP_KRB4 = 10;	// Kerberos 4 (unused)
  //private static final int STARTUP_KRB5 = 11;	// Kerberos 5 (unused)
  //private static final int STARTUP_HBA  = 12;	// Host Based
  //private static final int STARTUP_NONE = 13;	// Unauthenticated (unused)
  //private static final int STARTUP_PASS = 14;	// Password auth
  
  private boolean autoCommit = true;
  private boolean readOnly = false;
  
  protected Driver this_driver;
  private String this_url;
  private String cursor = null;	// The positioned update cursor name
  
  // These are new for v6.3, they determine the current protocol versions
  // supported by this version of the driver. They are defined in
  // src/include/libpq/pqcomm.h
  protected static final int PG_PROTOCOL_LATEST_MAJOR = 1;
  protected static final int PG_PROTOCOL_LATEST_MINOR = 0;
  private static final int SM_DATABASE	= 64;
  private static final int SM_USER	= 32;
  private static final int SM_OPTIONS	= 64;
  private static final int SM_UNUSED	= 64;
  private static final int SM_TTY	= 64;
  
  private static final int AUTH_REQ_OK       = 0;
  private static final int AUTH_REQ_KRB4     = 1;
  private static final int AUTH_REQ_KRB5     = 2;
  private static final int AUTH_REQ_PASSWORD = 3;
  private static final int AUTH_REQ_CRYPT    = 4;
  
  // New for 6.3, salt value for crypt authorisation
  private String salt;
  
  // This is used by Field to cache oid -> names.
  // It's here, because it's shared across this connection only.
  // Hence it cannot be static within the Field class, because it would then
  // be across all connections, which could be to different backends.
  protected Hashtable fieldCache = new Hashtable();
  
  /**
   * This is the current date style of the backend
   */
  protected int currentDateStyle;
  
  /**
   * This defines the formats for dates, according to the various date styles.
   *
   * <p>There are two strings for each entry. The first is the string to search
   * for in the datestyle message, and the second the format to use.
   *
   * <p>To add a new date style, work out the format. Then with psql running
   * in the date style you wish to add, type: show datestyle;
   *
   * <p>eg:
   * <br><pre>
   * => show datestyle;
   * NOTICE:  Datestyle is SQL with European conventions
   *                       ^^^^^^^^^^^^^^^^^
   * </pre>The marked part of the string is the first string below. The second
   * is your format. If a style (like ISO) ignores the US/European variants,
   * then you can ignore the "with" part of the string.
   */
  protected static final String dateStyles[] = {
    "Postgres with European",	"dd-MM-yyyy",
    "Postgres with US",		"MM-dd-yyyy",
    "ISO",			"yyyy-MM-dd",
    "SQL with European",	"dd/MM/yyyy",
    "SQL with US",		"MM/dd/yyyy",
    "German",			"dd.MM.yyyy"
  };
  
  // Now handle notices as warnings, so things like "show" now work
  protected SQLWarning firstWarning = null;
  
  /**
   * Connect to a PostgreSQL database back end.
   *
   * <p><b>Important Notice</b>
   *
   * <br>Although this will connect to the database, user code should open
   * the connection via the DriverManager.getConnection() methods only.
   *
   * <br>This should only be called from the postgresql.Driver class.
   *
   * @param host the hostname of the database back end
   * @param port the port number of the postmaster process
   * @param info a Properties[] thing of the user and password
   * @param database the database to connect to
   * @param u the URL of the connection
   * @param d the Driver instantation of the connection
   * @return a valid connection profile
   * @exception SQLException if a database access error occurs
   */
  public Connection(String host, int port, Properties info, String database, String url, Driver d) throws SQLException
  {
    //int len = STARTUP_LEN;	// Length of a startup packet
    
    // Throw an exception if the user or password properties are missing
    // This occasionally occurs when the client uses the properties version
    // of getConnection(), and is a common question on the email lists
    if(info.getProperty("user")==null)
      throw new SQLException("The user property is missing. It is mandatory.");
    if(info.getProperty("password")==null)
      throw new SQLException("The password property is missing. It is mandatory.");
    
    this_driver = d;
    this_url = new String(url);
    PG_DATABASE = new String(database);
    PG_PASSWORD = new String(info.getProperty("password"));
    PG_USER = new String(info.getProperty("user"));
    PG_PORT = port;
    PG_HOST = new String(host);
    PG_STATUS = CONNECTION_BAD;
    
    // Pre 6.3 code
    // This handles the auth property. Any value begining with p enables
    // password authentication, while anything begining with i enables
    // ident (RFC 1413) authentication. Any other values default to trust.
    //
    // Also, the postgresql.auth system property can be used to change the
    // local default, if the auth property is not present.
    //
    //String auth = info.getProperty("auth",System.getProperty("postgresql.auth","trust")).toLowerCase();
    //if(auth.startsWith("p")) {
    //// Password authentication
    //STARTUP_CODE=STARTUP_PASS;
    //} else if(auth.startsWith("i")) {
    //// Ident (RFC 1413) authentication
    //STARTUP_CODE=STARTUP_HBA;
    //} else {
    //// Anything else defaults to trust authentication
    //STARTUP_CODE=STARTUP_USER;
    //}
    
    // Now make the initial connection
    try
      {
	pg_stream = new PG_Stream(host, port);
      } catch (IOException e) {
	throw new SQLException ("Connection failed: " + e.toString());
      }
      
      // Now we need to construct and send a startup packet
      try
	{
	  // Pre 6.3 code
	  //pg_stream.SendInteger(len, 4);			len -= 4;
	  //pg_stream.SendInteger(STARTUP_CODE, 4);		len -= 4;
	  //pg_stream.Send(database.getBytes(), 64);	len -= 64;
	  //pg_stream.Send(PG_USER.getBytes(), len);
	  //
	  //// Send the password packet if required
	  //if(STARTUP_CODE == STARTUP_PASS) {
	  //len=STARTUP_LEN;
	  //pg_stream.SendInteger(len, 4);			len -= 4;
	  //pg_stream.SendInteger(STARTUP_PASS, 4);		len -= 4;
	  //pg_stream.Send(PG_USER.getBytes(), PG_USER.length());
	  //len-=PG_USER.length();
	  //pg_stream.SendInteger(0,1);			len -= 1;
	  //pg_stream.Send(PG_PASSWORD.getBytes(), len);
	  //}
	  
	  // Ver 6.3 code
	  pg_stream.SendInteger(4+4+SM_DATABASE+SM_USER+SM_OPTIONS+SM_UNUSED+SM_TTY,4);
	  pg_stream.SendInteger(PG_PROTOCOL_LATEST_MAJOR,2);
	  pg_stream.SendInteger(PG_PROTOCOL_LATEST_MINOR,2);
	  pg_stream.Send(database.getBytes(),SM_DATABASE);
	  pg_stream.Send(PG_USER.getBytes(),SM_USER+SM_OPTIONS+SM_UNUSED+SM_TTY);
	  // The last send includes the unused fields
	  
	  // Now get the response from the backend, either an error message
	  // or an authentication request
	  int areq = -1; // must have a value here
	  do {
	    int beresp = pg_stream.ReceiveChar();
	    switch(beresp)
	      {
	      case 'E':
		throw new SQLException(pg_stream.ReceiveString(4096));
		
	      case 'R':
		// Get the type of request
		areq = pg_stream.ReceiveIntegerR(4);
		
		// Get the password salt if there is one
		if(areq == AUTH_REQ_CRYPT) {
		  byte[] rst = new byte[2];
		  rst[0] = (byte)pg_stream.ReceiveChar();
		  rst[1] = (byte)pg_stream.ReceiveChar();
		  salt = new String(rst,0,2);
		  DriverManager.println("Salt="+salt);
		}
		
		// now send the auth packet
		switch(areq)
		  {
		  case AUTH_REQ_OK:
		    break;
		    
		  case AUTH_REQ_KRB4:
		    DriverManager.println("postgresql: KRB4");
		    throw new SQLException("Kerberos 4 not supported");
		    
		  case AUTH_REQ_KRB5:
		    DriverManager.println("postgresql: KRB5");
		    throw new SQLException("Kerberos 5 not supported");
		    
		  case AUTH_REQ_PASSWORD:
		    DriverManager.println("postgresql: PASSWORD");
		    pg_stream.SendInteger(5+PG_PASSWORD.length(),4);
		    pg_stream.Send(PG_PASSWORD.getBytes());
		    pg_stream.SendInteger(0,1);
		    //pg_stream.SendPacket(PG_PASSWORD.getBytes());
		    break;
		    
		  case AUTH_REQ_CRYPT:
		    DriverManager.println("postgresql: CRYPT");
		    String crypted = UnixCrypt.crypt(salt,PG_PASSWORD);
		    pg_stream.SendInteger(5+crypted.length(),4);
		    pg_stream.Send(crypted.getBytes());
		    pg_stream.SendInteger(0,1);
		    //pg_stream.SendPacket(UnixCrypt.crypt(salt,PG_PASSWORD).getBytes());
		    break;
		    
		  default:
		    throw new SQLException("Authentication type "+areq+" not supported");
		  }
		break;
		
	      default:
		throw new SQLException("error getting authentication request");
	      }
	    } while(areq != AUTH_REQ_OK);
	  
	} catch (IOException e) {
	  throw new SQLException("Connection failed: " + e.toString());
	}
	
	// Find out the date style by issuing the SQL: show datestyle
	// This actually issues a warning, and our own warning handling
	// code handles this itself.
	//
	// Also, this query replaced the NULL query issued to test the
	// connection.
	//
	clearWarnings();
	ExecSQL("show datestyle");
	
	// Initialise object handling
	initObjectTypes();
	
	// Mark the connection as ok, and cleanup
	clearWarnings();
	PG_STATUS = CONNECTION_OK;
  }
  
  /**
   * SQL statements without parameters are normally executed using
   * Statement objects.  If the same SQL statement is executed many
   * times, it is more efficient to use a PreparedStatement
   *
   * @return a new Statement object
   * @exception SQLException passed through from the constructor
   */
  public java.sql.Statement createStatement() throws SQLException
  {
    return new Statement(this);
  }
  
  /**
   * A SQL statement with or without IN parameters can be pre-compiled
   * and stored in a PreparedStatement object.  This object can then
   * be used to efficiently execute this statement multiple times.
   *
   * <B>Note:</B> This method is optimized for handling parametric
   * SQL statements that benefit from precompilation if the drivers
   * supports precompilation.  PostgreSQL does not support precompilation.
   * In this case, the statement is not sent to the database until the
   * PreparedStatement is executed.  This has no direct effect on users;
   * however it does affect which method throws certain SQLExceptions
   *
   * @param sql a SQL statement that may contain one or more '?' IN
   *	parameter placeholders
   * @return a new PreparedStatement object containing the pre-compiled
   *	statement.
   * @exception SQLException if a database access error occurs.
   */
  public java.sql.PreparedStatement prepareStatement(String sql) throws SQLException
  {
    return new PreparedStatement(this, sql);
  }
  
  /**
   * A SQL stored procedure call statement is handled by creating a
   * CallableStatement for it.  The CallableStatement provides methods
   * for setting up its IN and OUT parameters and methods for executing
   * it.
   *
   * <B>Note:</B> This method is optimised for handling stored procedure
   * call statements.  Some drivers may send the call statement to the
   * database when the prepareCall is done; others may wait until the
   * CallableStatement is executed.  This has no direct effect on users;
   * however, it does affect which method throws certain SQLExceptions
   *
   * @param sql a SQL statement that may contain one or more '?' parameter
   *	placeholders.  Typically this statement is a JDBC function call
   *	escape string.
   * @return a new CallableStatement object containing the pre-compiled
   *	SQL statement
   * @exception SQLException if a database access error occurs
   */
  public java.sql.CallableStatement prepareCall(String sql) throws SQLException
  {
    throw new SQLException("Callable Statements are not supported at this time");
    //		return new CallableStatement(this, sql);
  }
  
  /**
   * A driver may convert the JDBC sql grammar into its system's
   * native SQL grammar prior to sending it; nativeSQL returns the
   * native form of the statement that the driver would have sent.
   *
   * @param sql a SQL statement that may contain one or more '?'
   *	parameter placeholders
   * @return the native form of this statement
   * @exception SQLException if a database access error occurs
   */
  public String nativeSQL(String sql) throws SQLException
  {
    return sql;
  }
  
  /**
   * If a connection is in auto-commit mode, than all its SQL
   * statements will be executed and committed as individual
   * transactions.  Otherwise, its SQL statements are grouped
   * into transactions that are terminated by either commit()
   * or rollback().  By default, new connections are in auto-
   * commit mode.  The commit occurs when the statement completes
   * or the next execute occurs, whichever comes first.  In the
   * case of statements returning a ResultSet, the statement
   * completes when the last row of the ResultSet has been retrieved
   * or the ResultSet has been closed.  In advanced cases, a single
   * statement may return multiple results as well as output parameter
   * values.  Here the commit occurs when all results and output param
   * values have been retrieved.
   *
   * @param autoCommit - true enables auto-commit; false disables it
   * @exception SQLException if a database access error occurs
   */
  public void setAutoCommit(boolean autoCommit) throws SQLException
  {
    if (this.autoCommit == autoCommit)
      return;
    if (autoCommit)
      ExecSQL("end");
    else
      ExecSQL("begin");
    this.autoCommit = autoCommit;
  }
  
  /**
   * gets the current auto-commit state
   * 
   * @return Current state of the auto-commit mode
   * @exception SQLException (why?)
   * @see setAutoCommit
   */
  public boolean getAutoCommit() throws SQLException
  {
    return this.autoCommit;
  }
  
  /**
   * The method commit() makes all changes made since the previous
   * commit/rollback permanent and releases any database locks currently
   * held by the Connection.  This method should only be used when
   * auto-commit has been disabled.  (If autoCommit == true, then we
   * just return anyhow)
   *
   * @exception SQLException if a database access error occurs
   * @see setAutoCommit
   */
  public void commit() throws SQLException
  {
    if (autoCommit)
      return;
    ExecSQL("commit");
    autoCommit = true;
    ExecSQL("begin");
    autoCommit = false;
  }
  
  /**
   * The method rollback() drops all changes made since the previous
   * commit/rollback and releases any database locks currently held by
   * the Connection. 
   *
   * @exception SQLException if a database access error occurs
   * @see commit
   */
  public void rollback() throws SQLException
  {
    if (autoCommit)
      return;
    ExecSQL("rollback");
    autoCommit = true;
    ExecSQL("begin");
    autoCommit = false;
  }
  
  /**
   * In some cases, it is desirable to immediately release a Connection's
   * database and JDBC resources instead of waiting for them to be
   * automatically released (cant think why off the top of my head)
   *
   * <B>Note:</B> A Connection is automatically closed when it is
   * garbage collected.  Certain fatal errors also result in a closed
   * connection.
   *
   * @exception SQLException if a database access error occurs
   */
  public void close() throws SQLException
  {
    if (pg_stream != null)
      {
	try
	  {
	    pg_stream.close();
	  } catch (IOException e) {}
	  pg_stream = null;
      }
  }
  
  /**
   * Tests to see if a Connection is closed
   *
   * @return the status of the connection
   * @exception SQLException (why?)
   */
  public boolean isClosed() throws SQLException
  {
    return (pg_stream == null);
  }
  
  /**
   * A connection's database is able to provide information describing
   * its tables, its supported SQL grammar, its stored procedures, the
   * capabilities of this connection, etc.  This information is made
   * available through a DatabaseMetaData object.
   *
   * @return a DatabaseMetaData object for this connection
   * @exception SQLException if a database access error occurs
   */
  public java.sql.DatabaseMetaData getMetaData() throws SQLException
  {
    return new DatabaseMetaData(this);
  }
  
  /**
   * You can put a connection in read-only mode as a hunt to enable
   * database optimizations
   *
   * <B>Note:</B> setReadOnly cannot be called while in the middle
   * of a transaction
   *
   * @param readOnly - true enables read-only mode; false disables it
   * @exception SQLException if a database access error occurs
   */
  public void setReadOnly (boolean readOnly) throws SQLException
  {
    this.readOnly = readOnly;
  }
  
  /**
   * Tests to see if the connection is in Read Only Mode.  Note that
   * we cannot really put the database in read only mode, but we pretend
   * we can by returning the value of the readOnly flag
   *
   * @return true if the connection is read only
   * @exception SQLException if a database access error occurs
   */
  public boolean isReadOnly() throws SQLException
  {
    return readOnly;
  }
  
  /**
   * A sub-space of this Connection's database may be selected by
   * setting a catalog name.  If the driver does not support catalogs,
   * it will silently ignore this request
   *
   * @exception SQLException if a database access error occurs
   */
  public void setCatalog(String catalog) throws SQLException
  {
    // No-op
  }
  
  /**
   * Return the connections current catalog name, or null if no
   * catalog name is set, or we dont support catalogs.
   *
   * @return the current catalog name or null
   * @exception SQLException if a database access error occurs
   */
  public String getCatalog() throws SQLException
  {
    return null;
  }
  
  /**
   * You can call this method to try to change the transaction
   * isolation level using one of the TRANSACTION_* values.  
   *
   * <B>Note:</B> setTransactionIsolation cannot be called while
   * in the middle of a transaction
   *
   * @param level one of the TRANSACTION_* isolation values with
   *	the exception of TRANSACTION_NONE; some databases may
   *	not support other values
   * @exception SQLException if a database access error occurs
   * @see java.sql.DatabaseMetaData#supportsTransactionIsolationLevel
   */
  public void setTransactionIsolation(int level) throws SQLException
  {
    throw new SQLException("Transaction Isolation Levels are not implemented");
  }
  
  /**
   * Get this Connection's current transaction isolation mode.
   * 
   * @return the current TRANSACTION_* mode value
   * @exception SQLException if a database access error occurs
   */
  public int getTransactionIsolation() throws SQLException
  {
    return java.sql.Connection.TRANSACTION_SERIALIZABLE;
  }
  
  /**
   * The first warning reported by calls on this Connection is
   * returned.
   *
   * <B>Note:</B> Sebsequent warnings will be changed to this
   * SQLWarning
   *
   * @return the first SQLWarning or null
   * @exception SQLException if a database access error occurs
   */
  public SQLWarning getWarnings() throws SQLException
  {
    return firstWarning;
  }
  
  /**
   * After this call, getWarnings returns null until a new warning
   * is reported for this connection.
   *
   * @exception SQLException if a database access error occurs
   */
  public void clearWarnings() throws SQLException
  {
    firstWarning = null;
  }
  
  // **********************************************************
  //		END OF PUBLIC INTERFACE
  // **********************************************************
  
  /**
   * This adds a warning to the warning chain.
   * @param msg message to add
   */
  public void addWarning(String msg)
  {
    //PrintStream log = DriverManager.getLogStream();
    //if(log!=null) 
    DriverManager.println(msg);
    
    // Add the warning to the chain
    if(firstWarning!=null)
      firstWarning.setNextWarning(new SQLWarning(msg));
    else
      firstWarning = new SQLWarning(msg);
    
    // Now check for some specific messages
    
    // This is generated by the SQL "show datestyle"
    if(msg.startsWith("NOTICE:") && msg.indexOf("DateStyle")>0) {
      // 13 is the length off "DateStyle is "
      msg = msg.substring(msg.indexOf("DateStyle is ")+13);

      for(int i=0;i<dateStyles.length;i+=2)
	if(msg.startsWith(dateStyles[i]))
	  currentDateStyle=i+1; // this is the index of the format
    }
  }
  
  /**
   * @return the date format for the current date style of the backend
   */
  public String getDateStyle()
  {
    return dateStyles[currentDateStyle];
  }
  
  /**
   * Send a query to the backend.  Returns one of the ResultSet
   * objects.
   *
   * <B>Note:</B> there does not seem to be any method currently
   * in existance to return the update count.
   *
   * @param sql the SQL statement to be executed
   * @return a ResultSet holding the results
   * @exception SQLException if a database error occurs
   */
  public synchronized ResultSet ExecSQL(String sql) throws SQLException
  {
    Field[] fields = null;
    Vector tuples = new Vector();
    byte[] buf = new byte[sql.length()];
    int fqp = 0;
    boolean hfr = false;
    String recv_status = null, msg;
    SQLException final_error = null;
    
    if (sql.length() > 8192)
      throw new SQLException("SQL Statement too long: " + sql);
    try
      {
	pg_stream.SendChar('Q');
	buf = sql.getBytes();
	pg_stream.Send(buf);
	pg_stream.SendChar(0);
      } catch (IOException e) {
	throw new SQLException("I/O Error: " + e.toString());
      }
      
      while (!hfr || fqp > 0)
	{
	  Object tup=null;	// holds rows as they are recieved
	  
	  int c = pg_stream.ReceiveChar();
	  
	  switch (c)
	    {
	    case 'A':	// Asynchronous Notify
	      int pid = pg_stream.ReceiveInteger(4);
	      msg = pg_stream.ReceiveString(8192);
	      break;
	    case 'B':	// Binary Data Transfer
	      if (fields == null)
		throw new SQLException("Tuple received before MetaData");
	      tup = pg_stream.ReceiveTuple(fields.length, true);
	      // This implements Statement.setMaxRows()
	      if(maxrows==0 || tuples.size()<maxrows)
		tuples.addElement(tup);
	      break;
	    case 'C':	// Command Status
	      recv_status = pg_stream.ReceiveString(8192);
	      if (fields != null)
		hfr = true;
	      else
		{
		  try
		    {
		      pg_stream.SendChar('Q');
		      pg_stream.SendChar(' ');
		      pg_stream.SendChar(0);
		    } catch (IOException e) {
		      throw new SQLException("I/O Error: " + e.toString());
		    }
		    fqp++;
		}
	      break;
	    case 'D':	// Text Data Transfer
	      if (fields == null)
		throw new SQLException("Tuple received before MetaData");
	      tup = pg_stream.ReceiveTuple(fields.length, false);
	      // This implements Statement.setMaxRows()
	      if(maxrows==0 || tuples.size()<maxrows)
		tuples.addElement(tup);
	      break;
	    case 'E':	// Error Message
	      msg = pg_stream.ReceiveString(4096);
	      final_error = new SQLException(msg);
	      hfr = true;
	      break;
	    case 'I':	// Empty Query
	      int t = pg_stream.ReceiveChar();
	      
	      if (t != 0)
		throw new SQLException("Garbled Data");
	      if (fqp > 0)
		fqp--;
	      if (fqp == 0)
		hfr = true;
	      break;
	    case 'N':	// Error Notification
	      addWarning(pg_stream.ReceiveString(4096));
	      break;
	    case 'P':	// Portal Name
	      String pname = pg_stream.ReceiveString(8192);
	      break;
	    case 'T':	// MetaData Field Description
	      if (fields != null)
		throw new SQLException("Cannot handle multiple result groups");
	      fields = ReceiveFields();
	      break;
	    default:
	      throw new SQLException("Unknown Response Type: " + (char)c);
	    }
	}
      if (final_error != null)
	throw final_error;
      return new ResultSet(this, fields, tuples, recv_status, 1);
  }
  
  /**
   * Receive the field descriptions from the back end
   *
   * @return an array of the Field object describing the fields
   * @exception SQLException if a database error occurs
   */
  private Field[] ReceiveFields() throws SQLException
  {
    int nf = pg_stream.ReceiveIntegerR(2), i;
    Field[] fields = new Field[nf];
    
    for (i = 0 ; i < nf ; ++i)
      {
	String typname = pg_stream.ReceiveString(8192);
	int typid = pg_stream.ReceiveIntegerR(4);
	int typlen = pg_stream.ReceiveIntegerR(2);
	fields[i] = new Field(this, typname, typid, typlen);
      }
    return fields;
  }
  
  /**
   * In SQL, a result table can be retrieved through a cursor that
   * is named.  The current row of a result can be updated or deleted
   * using a positioned update/delete statement that references the
   * cursor name.
   *
   * We support one cursor per connection.
   *
   * setCursorName sets the cursor name.
   *
   * @param cursor the cursor name
   * @exception SQLException if a database access error occurs
   */
  public void setCursorName(String cursor) throws SQLException
  {
    this.cursor = cursor;
  }
  
  /**
   * getCursorName gets the cursor name.
   *
   * @return the current cursor name
   * @exception SQLException if a database access error occurs
   */
  public String getCursorName() throws SQLException
  {
    return cursor;
  }
  
  /**
   * We are required to bring back certain information by
   * the DatabaseMetaData class.  These functions do that.
   *
   * Method getURL() brings back the URL (good job we saved it)
   *
   * @return the url
   * @exception SQLException just in case...
   */
  public String getURL() throws SQLException
  {
    return this_url;
  }
  
  /**
   * Method getUserName() brings back the User Name (again, we
   * saved it)
   *
   * @return the user name
   * @exception SQLException just in case...
   */
  public String getUserName() throws SQLException
  {
    return PG_USER;
  }
  
  /**
   * This returns the Fastpath API for the current connection.
   *
   * <p><b>NOTE:</b> This is not part of JDBC, but allows access to
   * functions on the postgresql backend itself.
   *
   * <p>It is primarily used by the LargeObject API
   *
   * <p>The best way to use this is as follows:
   *
   * <p><pre>
   * import postgresql.fastpath.*;
   * ...
   * Fastpath fp = ((postgresql.Connection)myconn).getFastpathAPI();
   * </pre>
   *
   * <p>where myconn is an open Connection to postgresql.
   *
   * @return Fastpath object allowing access to functions on the postgresql
   * backend.
   * @exception SQLException by Fastpath when initialising for first time
   */
  public Fastpath getFastpathAPI() throws SQLException
  {
    if(fastpath==null)
      fastpath = new Fastpath(this,pg_stream);
    return fastpath;
  }
  
  // This holds a reference to the Fastpath API if already open
  private Fastpath fastpath = null;
  
  /**
   * This returns the LargeObject API for the current connection.
   *
   * <p><b>NOTE:</b> This is not part of JDBC, but allows access to
   * functions on the postgresql backend itself.
   *
   * <p>The best way to use this is as follows:
   *
   * <p><pre>
   * import postgresql.largeobject.*;
   * ...
   * LargeObjectManager lo = ((postgresql.Connection)myconn).getLargeObjectAPI();
   * </pre>
   *
   * <p>where myconn is an open Connection to postgresql.
   *
   * @return LargeObject object that implements the API
   * @exception SQLException by LargeObject when initialising for first time
   */
  public LargeObjectManager getLargeObjectAPI() throws SQLException
  {
    if(largeobject==null)
      largeobject = new LargeObjectManager(this);
    return largeobject;
  }
  
  // This holds a reference to the LargeObject API if already open
  private LargeObjectManager largeobject = null;
  
  /**
   * This method is used internally to return an object based around
   * postgresql's more unique data types.
   *
   * <p>It uses an internal Hashtable to get the handling class. If the
   * type is not supported, then an instance of postgresql.util.PGobject
   * is returned.
   *
   * You can use the getValue() or setValue() methods to handle the returned
   * object. Custom objects can have their own methods.
   *
   * @return PGobject for this type, and set to value
   * @exception SQLException if value is not correct for this type
   */
  protected PGobject getObject(String type,String value) throws SQLException
  {
    PGobject obj = null;
    try {
      String name = (String)objectTypes.get(type);
      obj = (PGobject)(Class.forName(name==null?"postgresql.util.PGobject":name).newInstance());
    } catch(Exception ex) {
      throw new SQLException("Failed to create object for "+type+": "+ex);
    }
    if(obj!=null) {
      obj.setType(type);
      obj.setValue(value);
    }
    return obj;
  }
  
  /**
   * This allows client code to add a handler for one of postgresql's
   * more unique data types.
   *
   * <p><b>NOTE:</b> This is not part of JDBC, but an extension.
   *
   * <p>The best way to use this is as follows:
   *
   * <p><pre>
   * ...
   * ((postgresql.Connection)myconn).addDataType("mytype","my.class.name");
   * ...
   * </pre>
   *
   * <p>where myconn is an open Connection to postgresql.
   *
   * <p>The handling class must extend postgresql.util.PGobject
   *
   * @see postgresql.util.PGobject
   */
  public void addDataType(String type,String name)
  {
    objectTypes.put(type,name);
  }
  
  // This holds the available types
  private Hashtable objectTypes = new Hashtable();
  
  // This array contains the types that are supported as standard.
  //
  // The first entry is the types name on the database, the second
  // the full class name of the handling class.
  //
  private static final String defaultObjectTypes[][] = {
    {"box",	"postgresql.geometric.PGbox"},
    {"circle",	"postgresql.geometric.PGcircle"},
    {"lseg",	"postgresql.geometric.PGlseg"},
    {"path",	"postgresql.geometric.PGpath"},
    {"point",	"postgresql.geometric.PGpoint"},
    {"polygon",	"postgresql.geometric.PGpolygon"}
  };
  
  // This initialises the objectTypes hashtable
  private void initObjectTypes()
  {
    for(int i=0;i<defaultObjectTypes.length;i++)
      objectTypes.put(defaultObjectTypes[i][0],defaultObjectTypes[i][1]);
  }
}

// ***********************************************************************

