package org.postgresql;

import java.io.*;
import java.net.*;
import java.sql.*;
import java.util.*;
import org.postgresql.Field;
import org.postgresql.fastpath.*;
import org.postgresql.largeobject.*;
import org.postgresql.util.*;
import org.postgresql.core.Encoding;

/**
 * $Id: Connection.java,v 1.25 2001/08/10 14:42:07 momjian Exp $
 *
 * This abstract class is used by org.postgresql.Driver to open either the JDBC1 or
 * JDBC2 versions of the Connection class.
 *
 */
public abstract class Connection
{
  // This is the network stream associated with this connection
  public PG_Stream pg_stream;

  // This is set by org.postgresql.Statement.setMaxRows()
  //public int maxrows = 0;		// maximum no. of rows; 0 = unlimited

  private String PG_HOST;
  private int PG_PORT;
  private String PG_USER;
  private String PG_PASSWORD;
  private String PG_DATABASE;
  private boolean PG_STATUS;

  /**
   *  The encoding to use for this connection.
   */
  private Encoding encoding = Encoding.defaultEncoding();

  private String dbVersionNumber;

  public boolean CONNECTION_OK = true;
  public boolean CONNECTION_BAD = false;

  public boolean autoCommit = true;
  public boolean readOnly = false;

  public Driver this_driver;
  private String this_url;
  private String cursor = null;	// The positioned update cursor name

  // These are new for v6.3, they determine the current protocol versions
  // supported by this version of the driver. They are defined in
  // src/include/libpq/pqcomm.h
  protected static final int PG_PROTOCOL_LATEST_MAJOR = 2;
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
  public Hashtable fieldCache = new Hashtable();

  // Now handle notices as warnings, so things like "show" now work
  public SQLWarning firstWarning = null;

  /**
   * Cache of the current isolation level
   */
  private int isolationLevel = java.sql.Connection.TRANSACTION_READ_COMMITTED;

    // The PID an cancellation key we get from the backend process
    public int pid;
    public int ckey;

    /**
     * This is called by Class.forName() from within org.postgresql.Driver
     */
    public Connection()
    {
    }

    /**
     * This method actually opens the connection. It is called by Driver.
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
    protected void openConnection(String host, int port, Properties info, String database, String url, Driver d) throws SQLException
    {
    // Throw an exception if the user or password properties are missing
    // This occasionally occurs when the client uses the properties version
    // of getConnection(), and is a common question on the email lists
    if(info.getProperty("user")==null)
      throw new PSQLException("postgresql.con.user");
    if(info.getProperty("password")==null)
      throw new PSQLException("postgresql.con.pass");

    this_driver = d;
    this_url = url;
    PG_DATABASE = database;
    PG_PASSWORD = info.getProperty("password");
    PG_USER = info.getProperty("user");
    PG_PORT = port;
    PG_HOST = host;
    PG_STATUS = CONNECTION_BAD;

    // Now make the initial connection
    try
      {
	pg_stream = new PG_Stream(host, port);
      } catch (ConnectException cex) {
	// Added by Peter Mount <peter@retep.org.uk>
	// ConnectException is thrown when the connection cannot be made.
	// we trap this an return a more meaningful message for the end user
	throw new PSQLException ("postgresql.con.refused");
      } catch (IOException e) {
	throw new PSQLException ("postgresql.con.failed",e);
      }

      // Now we need to construct and send a startup packet
      try
	{
	  // Ver 6.3 code
	  pg_stream.SendInteger(4+4+SM_DATABASE+SM_USER+SM_OPTIONS+SM_UNUSED+SM_TTY,4);
	  pg_stream.SendInteger(PG_PROTOCOL_LATEST_MAJOR,2);
	  pg_stream.SendInteger(PG_PROTOCOL_LATEST_MINOR,2);
	  pg_stream.Send(database.getBytes(),SM_DATABASE);

	  // This last send includes the unused fields
	  pg_stream.Send(PG_USER.getBytes(),SM_USER+SM_OPTIONS+SM_UNUSED+SM_TTY);

	  // now flush the startup packets to the backend
	  pg_stream.flush();

	  // Now get the response from the backend, either an error message
	  // or an authentication request
	  int areq = -1; // must have a value here
	  do {
	    int beresp = pg_stream.ReceiveChar();
	    switch(beresp)
	      {
	      case 'E':
		// An error occured, so pass the error message to the
		// user.
		//
		// The most common one to be thrown here is:
		// "User authentication failed"
		//
                throw new SQLException(pg_stream.ReceiveString(encoding));

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
		    throw new PSQLException("postgresql.con.kerb4");

		  case AUTH_REQ_KRB5:
		    DriverManager.println("postgresql: KRB5");
		    throw new PSQLException("postgresql.con.kerb5");

		  case AUTH_REQ_PASSWORD:
		    DriverManager.println("postgresql: PASSWORD");
		    pg_stream.SendInteger(5+PG_PASSWORD.length(),4);
		    pg_stream.Send(PG_PASSWORD.getBytes());
		    pg_stream.SendInteger(0,1);
		    pg_stream.flush();
		    break;

		  case AUTH_REQ_CRYPT:
		    DriverManager.println("postgresql: CRYPT");
		    String crypted = UnixCrypt.crypt(salt,PG_PASSWORD);
		    pg_stream.SendInteger(5+crypted.length(),4);
		    pg_stream.Send(crypted.getBytes());
		    pg_stream.SendInteger(0,1);
		    pg_stream.flush();
		    break;

		  default:
		    throw new PSQLException("postgresql.con.auth",new Integer(areq));
		  }
		break;

	      default:
		throw new PSQLException("postgresql.con.authfail");
	      }
	    } while(areq != AUTH_REQ_OK);

	} catch (IOException e) {
	  throw new PSQLException("postgresql.con.failed",e);
	}


      // As of protocol version 2.0, we should now receive the cancellation key and the pid
      int beresp = pg_stream.ReceiveChar();
      switch(beresp) {
        case 'K':
          pid = pg_stream.ReceiveInteger(4);
          ckey = pg_stream.ReceiveInteger(4);
          break;
	case 'E':
	case 'N':
           throw new SQLException(pg_stream.ReceiveString(encoding));
        default:
          throw new PSQLException("postgresql.con.setup");
      }

      // Expect ReadyForQuery packet
      beresp = pg_stream.ReceiveChar();
      switch(beresp) {
        case 'Z':
	   break;
	case 'E':
	case 'N':
           throw new SQLException(pg_stream.ReceiveString(encoding));
        default:
          throw new PSQLException("postgresql.con.setup");
      }

      firstWarning = null;

      // "pg_encoding_to_char(1)" will return 'EUC_JP' for a backend compiled with multibyte,
      // otherwise it's hardcoded to 'SQL_ASCII'.
      // If the backend doesn't know about multibyte we can't assume anything about the encoding
      // used, so we denote this with 'UNKNOWN'.

      final String encodingQuery =
	  "case when pg_encoding_to_char(1) = 'SQL_ASCII' then 'UNKNOWN' else getdatabaseencoding() end";

      // Set datestyle and fetch db encoding in a single call, to avoid making
      // more than one round trip to the backend during connection startup.

      java.sql.ResultSet resultSet =
	  ExecSQL("set datestyle to 'ISO'; select version(), " + encodingQuery + ";");

      if (! resultSet.next()) {
	  throw new PSQLException("postgresql.con.failed", "failed getting backend encoding");
      }
      String version = resultSet.getString(1);
      dbVersionNumber = extractVersionNumber(version);

      String dbEncoding = resultSet.getString(2);
      encoding = Encoding.getEncoding(dbEncoding, info.getProperty("charSet"));

      // Initialise object handling
      initObjectTypes();

      // Mark the connection as ok, and cleanup
      firstWarning = null;
      PG_STATUS = CONNECTION_OK;
    }

    // These methods used to be in the main Connection implementation. As they
    // are common to all implementations (JDBC1 or 2), they are placed here.
    // This should make it easy to maintain the two specifications.

    /**
     * This adds a warning to the warning chain.
     * @param msg message to add
     */
    public void addWarning(String msg)
    {
	DriverManager.println(msg);

	// Add the warning to the chain
	if(firstWarning!=null)
	    firstWarning.setNextWarning(new SQLWarning(msg));
	else
	    firstWarning = new SQLWarning(msg);

	// Now check for some specific messages

	// This is obsolete in 6.5, but I've left it in here so if we need to use this
	// technique again, we'll know where to place it.
	//
	// This is generated by the SQL "show datestyle"
	//if(msg.startsWith("NOTICE:") && msg.indexOf("DateStyle")>0) {
	//// 13 is the length off "DateStyle is "
	//msg = msg.substring(msg.indexOf("DateStyle is ")+13);
	//
	//for(int i=0;i<dateStyles.length;i+=2)
	//if(msg.startsWith(dateStyles[i]))
	//currentDateStyle=i+1; // this is the index of the format
	//}
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
    public java.sql.ResultSet ExecSQL(String sql) throws SQLException
    {
      return ExecSQL(sql,null);
    }

    /**
     * Send a query to the backend.  Returns one of the ResultSet
     * objects.
     *
     * <B>Note:</B> there does not seem to be any method currently
     * in existance to return the update count.
     *
     * @param sql the SQL statement to be executed
     * @param stat The Statement associated with this query (may be null)
     * @return a ResultSet holding the results
     * @exception SQLException if a database error occurs
     */
    public java.sql.ResultSet ExecSQL(String sql,java.sql.Statement stat) throws SQLException
    {
      // added Jan 30 2001 to correct maxrows per statement
      int maxrows=0;
      if(stat!=null)
        maxrows=stat.getMaxRows();

	// added Oct 7 1998 to give us thread safety.
	synchronized(pg_stream) {
 	    // Deallocate all resources in the stream associated
  	    // with a previous request.
  	    // This will let the driver reuse byte arrays that has already
  	    // been allocated instead of allocating new ones in order
  	    // to gain performance improvements.
  	    // PM 17/01/01: Commented out due to race bug. See comments in
            // PG_Stream
            //pg_stream.deallocate();

	    Field[] fields = null;
	    Vector tuples = new Vector();
	    byte[] buf = null;
	    int fqp = 0;
	    boolean hfr = false;
	    String recv_status = null, msg;
	    int update_count = 1;
	    int insert_oid = 0;
	    SQLException final_error = null;

	    buf = encoding.encode(sql);
	    try
		{
		    pg_stream.SendChar('Q');
		    pg_stream.Send(buf);
		    pg_stream.SendChar(0);
		    pg_stream.flush();
		} catch (IOException e) {
		    throw new PSQLException("postgresql.con.ioerror",e);
		}

	    while (!hfr || fqp > 0)
		{
		    Object tup=null;	// holds rows as they are recieved

		    int c = pg_stream.ReceiveChar();

		    switch (c)
			{
			case 'A':	// Asynchronous Notify
			    pid = pg_stream.ReceiveInteger(4);
                            msg = pg_stream.ReceiveString(encoding);
			    break;
			case 'B':	// Binary Data Transfer
			    if (fields == null)
				throw new PSQLException("postgresql.con.tuple");
			    tup = pg_stream.ReceiveTuple(fields.length, true);
			    // This implements Statement.setMaxRows()
			    if(maxrows==0 || tuples.size()<maxrows)
				tuples.addElement(tup);
			    break;
			case 'C':	// Command Status
                            recv_status = pg_stream.ReceiveString(encoding);

				// Now handle the update count correctly.
				if(recv_status.startsWith("INSERT") || recv_status.startsWith("UPDATE") || recv_status.startsWith("DELETE") || recv_status.startsWith("MOVE")) {
					try {
						update_count = Integer.parseInt(recv_status.substring(1+recv_status.lastIndexOf(' ')));
					} catch(NumberFormatException nfe) {
						throw new PSQLException("postgresql.con.fathom",recv_status);
					}
					if(recv_status.startsWith("INSERT")) {
					    try {
						insert_oid = Integer.parseInt(recv_status.substring(1+recv_status.indexOf(' '),recv_status.lastIndexOf(' ')));
					    } catch(NumberFormatException nfe) {
						throw new PSQLException("postgresql.con.fathom",recv_status);
					    }
					}
				}
			    if (fields != null)
				hfr = true;
			    else
				{
				    try
					{
					    pg_stream.SendChar('Q');
					    pg_stream.SendChar(' ');
					    pg_stream.SendChar(0);
					    pg_stream.flush();
					} catch (IOException e) {
					    throw new PSQLException("postgresql.con.ioerror",e);
					}
				    fqp++;
				}
			    break;
			case 'D':	// Text Data Transfer
			    if (fields == null)
				throw new PSQLException("postgresql.con.tuple");
			    tup = pg_stream.ReceiveTuple(fields.length, false);
			    // This implements Statement.setMaxRows()
			    if(maxrows==0 || tuples.size()<maxrows)
				tuples.addElement(tup);
			    break;
			case 'E':	// Error Message
                            msg = pg_stream.ReceiveString(encoding);
			    final_error = new SQLException(msg);
			    hfr = true;
			    break;
			case 'I':	// Empty Query
			    int t = pg_stream.ReceiveChar();

			    if (t != 0)
				throw new PSQLException("postgresql.con.garbled");
			    if (fqp > 0)
				fqp--;
			    if (fqp == 0)
				hfr = true;
			    break;
			case 'N':	// Error Notification
                            addWarning(pg_stream.ReceiveString(encoding));
			    break;
			case 'P':	// Portal Name
                            String pname = pg_stream.ReceiveString(encoding);
			    break;
			case 'T':	// MetaData Field Description
			    if (fields != null)
				throw new PSQLException("postgresql.con.multres");
			    fields = ReceiveFields();
			    break;
			case 'Z':       // backend ready for query, ignore for now :-)
			    break;
			default:
			    throw new PSQLException("postgresql.con.type",new Character((char)c));
			}
		}
	    if (final_error != null)
		throw final_error;

	    return getResultSet(this, stat, fields, tuples, recv_status, update_count, insert_oid);
	}
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
                String typname = pg_stream.ReceiveString(encoding);
		int typid = pg_stream.ReceiveIntegerR(4);
		int typlen = pg_stream.ReceiveIntegerR(2);
		int typmod = pg_stream.ReceiveIntegerR(4);
		fields[i] = new Field(this, typname, typid, typlen, typmod);
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
     * Get the character encoding to use for this connection.
     */
    public Encoding getEncoding() throws SQLException {
        return encoding;
    }

    /**
     * This returns the Fastpath API for the current connection.
     *
     * <p><b>NOTE:</b> This is not part of JDBC, but allows access to
     * functions on the org.postgresql backend itself.
     *
     * <p>It is primarily used by the LargeObject API
     *
     * <p>The best way to use this is as follows:
     *
     * <p><pre>
     * import org.postgresql.fastpath.*;
     * ...
     * Fastpath fp = ((org.postgresql.Connection)myconn).getFastpathAPI();
     * </pre>
     *
     * <p>where myconn is an open Connection to org.postgresql.
     *
     * @return Fastpath object allowing access to functions on the org.postgresql
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
     * functions on the org.postgresql backend itself.
     *
     * <p>The best way to use this is as follows:
     *
     * <p><pre>
     * import org.postgresql.largeobject.*;
     * ...
     * LargeObjectManager lo = ((org.postgresql.Connection)myconn).getLargeObjectAPI();
     * </pre>
     *
     * <p>where myconn is an open Connection to org.postgresql.
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
     * org.postgresql's more unique data types.
     *
     * <p>It uses an internal Hashtable to get the handling class. If the
     * type is not supported, then an instance of org.postgresql.util.PGobject
     * is returned.
     *
     * You can use the getValue() or setValue() methods to handle the returned
     * object. Custom objects can have their own methods.
     *
     * In 6.4, this is extended to use the org.postgresql.util.Serialize class to
     * allow the Serialization of Java Objects into the database without using
     * Blobs. Refer to that class for details on how this new feature works.
     *
     * @return PGobject for this type, and set to value
     * @exception SQLException if value is not correct for this type
     * @see org.postgresql.util.Serialize
     */
    public Object getObject(String type,String value) throws SQLException
    {
	try {
	    Object o = objectTypes.get(type);

	    // If o is null, then the type is unknown, so check to see if type
	    // is an actual table name. If it does, see if a Class is known that
	    // can handle it
	    if(o == null) {
		Serialize ser = new Serialize(this,type);
		objectTypes.put(type,ser);
		return ser.fetch(Integer.parseInt(value));
	    }

	    // If o is not null, and it is a String, then its a class name that
	    // extends PGobject.
	    //
	    // This is used to implement the org.postgresql unique types (like lseg,
	    // point, etc).
	    if(o instanceof String) {
		// 6.3 style extending PG_Object
		PGobject obj = null;
		obj = (PGobject)(Class.forName((String)o).newInstance());
		obj.setType(type);
		obj.setValue(value);
		return (Object)obj;
	    } else {
		// If it's an object, it should be an instance of our Serialize class
		// If so, then call it's fetch method.
		if(o instanceof Serialize)
		    return ((Serialize)o).fetch(Integer.parseInt(value));
	    }
	} catch(SQLException sx) {
	    // rethrow the exception. Done because we capture any others next
	    sx.fillInStackTrace();
	    throw sx;
	} catch(Exception ex) {
	    throw new PSQLException("postgresql.con.creobj",type,ex);
	}

	// should never be reached
	return null;
    }

    /**
     * This stores an object into the database.
     * @param o Object to store
     * @return OID of the new rectord
     * @exception SQLException if value is not correct for this type
     * @see org.postgresql.util.Serialize
     */
    public int putObject(Object o) throws SQLException
    {
	try {
	    String type = o.getClass().getName();
	    Object x = objectTypes.get(type);

	    // If x is null, then the type is unknown, so check to see if type
	    // is an actual table name. If it does, see if a Class is known that
	    // can handle it
	    if(x == null) {
		Serialize ser = new Serialize(this,type);
		objectTypes.put(type,ser);
		return ser.store(o);
	    }

	    // If it's an object, it should be an instance of our Serialize class
	    // If so, then call it's fetch method.
	    if(x instanceof Serialize)
		return ((Serialize)x).store(o);

	    // Thow an exception because the type is unknown
	    throw new PSQLException("postgresql.con.strobj");

	} catch(SQLException sx) {
	    // rethrow the exception. Done because we capture any others next
	    sx.fillInStackTrace();
	    throw sx;
	} catch(Exception ex) {
	    throw new PSQLException("postgresql.con.strobjex",ex);
	}
    }

    /**
     * This allows client code to add a handler for one of org.postgresql's
     * more unique data types.
     *
     * <p><b>NOTE:</b> This is not part of JDBC, but an extension.
     *
     * <p>The best way to use this is as follows:
     *
     * <p><pre>
     * ...
     * ((org.postgresql.Connection)myconn).addDataType("mytype","my.class.name");
     * ...
     * </pre>
     *
     * <p>where myconn is an open Connection to org.postgresql.
     *
     * <p>The handling class must extend org.postgresql.util.PGobject
     *
     * @see org.postgresql.util.PGobject
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
	{"box",		"org.postgresql.geometric.PGbox"},
	{"circle",	"org.postgresql.geometric.PGcircle"},
	{"line",	"org.postgresql.geometric.PGline"},
	{"lseg",	"org.postgresql.geometric.PGlseg"},
	{"path",	"org.postgresql.geometric.PGpath"},
	{"point",	"org.postgresql.geometric.PGpoint"},
	{"polygon",	"org.postgresql.geometric.PGpolygon"},
	{"money",	"org.postgresql.util.PGmoney"}
    };

    // This initialises the objectTypes hashtable
    private void initObjectTypes()
    {
	for(int i=0;i<defaultObjectTypes.length;i++)
	    objectTypes.put(defaultObjectTypes[i][0],defaultObjectTypes[i][1]);
    }

    // These are required by other common classes
    public abstract java.sql.Statement createStatement() throws SQLException;

    /**
     * This returns a resultset. It must be overridden, so that the correct
     * version (from jdbc1 or jdbc2) are returned.
     */
    protected abstract java.sql.ResultSet getResultSet(org.postgresql.Connection conn,java.sql.Statement stat, Field[] fields, Vector tuples, String status, int updateCount,int insertOID) throws SQLException;

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
    public void close() throws SQLException {
	if (pg_stream != null) {
	    try {
		pg_stream.SendChar('X');
		pg_stream.flush();
		pg_stream.close();
	    } catch (IOException e) {}
	    pg_stream = null;
	}
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
    public String nativeSQL(String sql) throws SQLException {
	return sql;
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
    public SQLWarning getWarnings() throws SQLException {
	return firstWarning;
    }

    /**
     * After this call, getWarnings returns null until a new warning
     * is reported for this connection.
     *
     * @exception SQLException if a database access error occurs
     */
    public void clearWarnings() throws SQLException {
	firstWarning = null;
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
    public void setReadOnly(boolean readOnly) throws SQLException {
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
    public boolean isReadOnly() throws SQLException {
	return readOnly;
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
    public void setAutoCommit(boolean autoCommit) throws SQLException {
	if (this.autoCommit == autoCommit)
	    return;
	if (autoCommit)
	    ExecSQL("end");
	else {
	    ExecSQL("begin; " + getIsolationLevelSQL());
	}
	this.autoCommit = autoCommit;
    }

    /**
     * gets the current auto-commit state
     *
     * @return Current state of the auto-commit mode
     * @exception SQLException (why?)
     * @see setAutoCommit
     */
    public boolean getAutoCommit() throws SQLException {
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
    public void commit() throws SQLException {
	if (autoCommit)
	    return;
	ExecSQL("commit; begin; " + getIsolationLevelSQL());
    }

    /**
     * The method rollback() drops all changes made since the previous
     * commit/rollback and releases any database locks currently held by
     * the Connection.
     *
     * @exception SQLException if a database access error occurs
     * @see commit
     */
    public void rollback() throws SQLException {
	if (autoCommit)
	    return;
	ExecSQL("rollback; begin; " + getIsolationLevelSQL());
    }

    /**
     * Get this Connection's current transaction isolation mode.
     *
     * @return the current TRANSACTION_* mode value
     * @exception SQLException if a database access error occurs
     */
    public int getTransactionIsolation() throws SQLException {
	clearWarnings();
	ExecSQL("show xactisolevel");

	SQLWarning warning = getWarnings();
	if (warning != null) {
	    String message = warning.getMessage();
	    clearWarnings();
	    if (message.indexOf("READ COMMITTED") != -1)
		return java.sql.Connection.TRANSACTION_READ_COMMITTED;
	    else if (message.indexOf("READ UNCOMMITTED") != -1)
		return java.sql.Connection.TRANSACTION_READ_UNCOMMITTED;
	    else if (message.indexOf("REPEATABLE READ") != -1)
		return java.sql.Connection.TRANSACTION_REPEATABLE_READ;
	    else if (message.indexOf("SERIALIZABLE") != -1)
		return java.sql.Connection.TRANSACTION_SERIALIZABLE;
	}
	return java.sql.Connection.TRANSACTION_READ_COMMITTED;
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
    public void setTransactionIsolation(int level) throws SQLException {
	//In 7.1 and later versions of the server it is possible using
        //the "set session" command to set this once for all future txns
        //however in 7.0 and prior versions it is necessary to set it in 
        //each transaction, thus adding complexity below.
        //When we decide to drop support for servers older than 7.1
        //this can be simplified
        isolationLevel = level;
        String isolationLevelSQL;

	if (!haveMinimumServerVersion("7.1")) {
	    isolationLevelSQL = getIsolationLevelSQL();
	} else {
	    isolationLevelSQL = "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL ";
	    switch(isolationLevel) {
		case java.sql.Connection.TRANSACTION_READ_COMMITTED:
		    isolationLevelSQL += "READ COMMITTED";
		    break;
		case java.sql.Connection.TRANSACTION_SERIALIZABLE:
		    isolationLevelSQL += "SERIALIZABLE";
		    break;
		default:
		    throw new PSQLException("postgresql.con.isolevel",
					    new Integer(isolationLevel));
	    }
	}
	ExecSQL(isolationLevelSQL);
    }

    /**
     * Helper method used by setTransactionIsolation(), commit(), rollback()
     * and setAutoCommit(). This returns the SQL string needed to
     * set the isolation level for a transaction.  In 7.1 and later it
     * is possible to set a default isolation level that applies to all
     * future transactions, this method is only necesary for 7.0 and older
     * servers, and should be removed when support for these older
     * servers are dropped
     */
    protected String getIsolationLevelSQL() throws SQLException {
	//7.1 and higher servers have a default specified so 
	//no additional SQL is required to set the isolation level
	if (haveMinimumServerVersion("7.1")) {
          return "";
        }
	String q = "SET TRANSACTION ISOLATION LEVEL";

	switch(isolationLevel) {
	    case java.sql.Connection.TRANSACTION_READ_COMMITTED:
		q = q + " READ COMMITTED";
                break;

	    case java.sql.Connection.TRANSACTION_SERIALIZABLE:
		q = q + " SERIALIZABLE";
                break;

	    default:
		throw new PSQLException("postgresql.con.isolevel",new Integer(isolationLevel));
	}
        return q;
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
      //no-op
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
	return PG_DATABASE;
    }

    /**
     * Overides finalize(). If called, it closes the connection.
     *
     * This was done at the request of Rachel Greenham
     * <rachel@enlarion.demon.co.uk> who hit a problem where multiple
     * clients didn't close the connection, and once a fortnight enough
     * clients were open to kill the org.postgres server.
     */
    public void finalize() throws Throwable
    {
	close();
    }

  private static String extractVersionNumber(String fullVersionString)
  {
      StringTokenizer versionParts = new StringTokenizer(fullVersionString);
      versionParts.nextToken(); /* "PostgreSQL" */
      return versionParts.nextToken(); /* "X.Y.Z" */
  }

    /**
     * Get server version number
     */
    public String getDBVersionNumber() {
	return dbVersionNumber;
    }

  public boolean haveMinimumServerVersion(String ver) throws SQLException
  {
      return (getDBVersionNumber().compareTo(ver) >= 0);
  }
}
   
