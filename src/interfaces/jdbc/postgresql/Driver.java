package postgresql;

import java.sql.*;
import java.util.*;

import postgresql.util.PSQLException;

/**
 * The Java SQL framework allows for multiple database drivers.  Each
 * driver should supply a class that implements the Driver interface
 *
 * <p>The DriverManager will try to load as many drivers as it can find and
 * then for any given connection request, it will ask each driver in turn
 * to try to connect to the target URL.
 *
 * <p>It is strongly recommended that each Driver class should be small and
 * standalone so that the Driver class can be loaded and queried without
 * bringing in vast quantities of supporting code.
 *
 * <p>When a Driver class is loaded, it should create an instance of itself
 * and register it with the DriverManager.  This means that a user can load
 * and register a driver by doing Class.forName("foo.bah.Driver")
 *
 * @see postgresql.Connection
 * @see java.sql.Driver
 */
public class Driver implements java.sql.Driver 
{
  // These should be in sync with the backend that the driver was
  // distributed with
  static final int MAJORVERSION = 6;
  static final int MINORVERSION = 5;
    
  static 
  {
    try {
      // moved the registerDriver from the constructor to here
      // because some clients call the driver themselves (I know, as
      // my early jdbc work did - and that was based on other examples).
      // Placing it here, means that the driver is registered once only.
      java.sql.DriverManager.registerDriver(new Driver());
    } catch (SQLException e) {
      e.printStackTrace();
    }
  }
  
  /**
   * Construct a new driver and register it with DriverManager
   *
   * @exception SQLException for who knows what!
   */
  public Driver() throws SQLException
  {
      // Set the connectClass variable so that future calls will handle the correct
      // base class
      //if(System.getProperty("java.version").startsWith("1.1")) {
      //connectClass = "postgresql.jdbc1.Connection";
      //} else {
      //connectClass = "postgresql.jdbc2.Connection";
      //}
      
      // Ok, when the above code was introduced in 6.5 it's intention was to allow
      // the driver to automatically detect which version of JDBC was being used
      // and to detect the version of the JVM accordingly.
      //
      // It did this by using the java.version parameter.
      //
      // However, it was quickly discovered that not all JVM's returned an easily
      // parseable version number (ie "1.2") and some don't return a value at all.
      // The latter came from a discussion on the advanced java list.
      //
      // So, to solve this, I've moved the decision out of the driver, and it's now
      // a compile time parameter.
      //
      // For this to work, the Makefile creates a pseudo class which contains the class
      // name that will actually make the connection.
  }
  
  /**
   * Try to make a database connection to the given URL.  The driver
   * should return "null" if it realizes it is the wrong kind of
   * driver to connect to the given URL.  This will be common, as
   * when the JDBC driverManager is asked to connect to a given URL,
   * it passes the URL to each loaded driver in turn.
   *
   * <p>The driver should raise an SQLException if it is the right driver
   * to connect to the given URL, but has trouble connecting to the
   * database.
   *
   * <p>The java.util.Properties argument can be used to pass arbitrary
   * string tag/value pairs as connection arguments.  Normally, at least
   * "user" and "password" properties should be included in the 
   * properties.
   *
   * Our protocol takes the forms:
   * <PRE>
   *	jdbc:postgresql://host:port/database?param1=val1&...
   * </PRE>
   *
   * @param url the URL of the database to connect to
   * @param info a list of arbitrary tag/value pairs as connection
   *	arguments
   * @return a connection to the URL or null if it isnt us
   * @exception SQLException if a database access error occurs
   * @see java.sql.Driver#connect
   */
  public java.sql.Connection connect(String url, Properties info) throws SQLException
  {
    if((props = parseURL(url,info))==null)
      return null;
    
    DriverManager.println("Using "+DriverClass.connectClass);
    
    try {
	postgresql.Connection con = (postgresql.Connection)(Class.forName(DriverClass.connectClass).newInstance());
	con.openConnection (host(), port(), props, database(), url, this);
	return (java.sql.Connection)con;
    } catch(ClassNotFoundException ex) {
	throw new PSQLException("postgresql.jvm.version",ex);
    } catch(PSQLException ex1) {
	// re-throw the exception, otherwise it will be caught next, and a
	// postgresql.unusual error will be returned instead.
	throw ex1;
    } catch(Exception ex2) {
	throw new PSQLException("postgresql.unusual",ex2);
    }
  }
  
  /**
   * Returns true if the driver thinks it can open a connection to the
   * given URL.  Typically, drivers will return true if they understand
   * the subprotocol specified in the URL and false if they don't.  Our
   * protocols start with jdbc:postgresql:
   *
   * @see java.sql.Driver#acceptsURL
   * @param url the URL of the driver
   * @return true if this driver accepts the given URL
   * @exception SQLException if a database-access error occurs
   * 	(Dont know why it would *shrug*)
   */
  public boolean acceptsURL(String url) throws SQLException
  {
    if(parseURL(url,null)==null)
      return false;
    return true;
  }
  
  /**
   * The getPropertyInfo method is intended to allow a generic GUI
   * tool to discover what properties it should prompt a human for
   * in order to get enough information to connect to a database.
   *
   * <p>Note that depending on the values the human has supplied so
   * far, additional values may become necessary, so it may be necessary
   * to iterate through several calls to getPropertyInfo
   *
   * @param url the Url of the database to connect to
   * @param info a proposed list of tag/value pairs that will be sent on
   * 	connect open.
   * @return An array of DriverPropertyInfo objects describing
   * 	possible properties.  This array may be an empty array if
   *	no properties are required
   * @exception SQLException if a database-access error occurs
   * @see java.sql.Driver#getPropertyInfo
   */
  public DriverPropertyInfo[] getPropertyInfo(String url, Properties info) throws SQLException
  {
    Properties p = parseURL(url,info);
    
    // naughty, but its best for speed. If anyone adds a property here, then
    // this _MUST_ be increased to accomodate them.
    DriverPropertyInfo d,dpi[] = new DriverPropertyInfo[0];
    //int i=0;
    
    //dpi[i++] = d = new DriverPropertyInfo("auth",p.getProperty("auth","default"));
    //d.description = "determines if password authentication is used";
    //d.choices = new String[4];
    //d.choices[0]="default";	// Get value from postgresql.auth property, defaults to trust
    //d.choices[1]="trust";	// No password authentication
    //d.choices[2]="password";	// Password authentication
    //d.choices[3]="ident";	// Ident (RFC 1413) protocol
    
    return dpi;
  }
  
  /**
   * Gets the drivers major version number
   *
   * @return the drivers major version number
   */
  public int getMajorVersion()
  {
    return MAJORVERSION;
  }
  
  /**
   * Get the drivers minor version number
   *
   * @return the drivers minor version number
   */
  public int getMinorVersion()
  {
    return MINORVERSION;
  }
  
  /**
   * Report whether the driver is a genuine JDBC compliant driver.  A
   * driver may only report "true" here if it passes the JDBC compliance
   * tests, otherwise it is required to return false.  JDBC compliance
   * requires full support for the JDBC API and full support for SQL 92
   * Entry Level.  
   *
   * <p>For PostgreSQL, this is not yet possible, as we are not SQL92
   * compliant (yet).
   */
  public boolean jdbcCompliant()
  {
    return false;
  }
  
  private Properties props;
  
  static private String[] protocols = { "jdbc","postgresql" };
  
  /**
   * Constructs a new DriverURL, splitting the specified URL into its
   * component parts
   * @param url JDBC URL to parse
   * @param defaults Default properties
   * @return Properties with elements added from the url
   * @exception SQLException
   */
  Properties parseURL(String url,Properties defaults) throws SQLException
  {
    int state = -1;
    Properties urlProps = new Properties(defaults);
    String key = new String();
    String value = new String();
    
    StringTokenizer st = new StringTokenizer(url, ":/;=&?", true);
    for (int count = 0; (st.hasMoreTokens()); count++) {
      String token = st.nextToken();
      
      // PM June 29 1997
      // Added this, to help me understand how this works.
      // Unless you want each token to be processed, leave this commented out
      // but don't delete it.
      //DriverManager.println("wellFormedURL: state="+state+" count="+count+" token='"+token+"'");
      
      // PM Aug 2 1997 - Modified to allow multiple backends
      if (count <= 3) {
	if ((count % 2) == 1 && token.equals(":"))
	  ;
	else if((count % 2) == 0) {
	  boolean found=(count==0)?true:false;
	  for(int tmp=0;tmp<protocols.length;tmp++) {
	    if(token.equals(protocols[tmp])) {
	      // PM June 29 1997 Added this property to enable the driver
	      // to handle multiple backend protocols.
	      if(count == 2 && tmp > 0) {
		urlProps.put("Protocol",token);
		found=true;
	      }
	    }
	  }
	  
	  if(found == false)
	    return null;
	} else return null;
      }
      else if (count > 3) {
	if (count == 4 && token.equals("/")) state = 0;
	else if (count == 4) {
	  urlProps.put("PGDBNAME", token);
	  state = -2;
	}
	else if (count == 5 && state == 0 && token.equals("/"))
	  state = 1;
	else if (count == 5 && state == 0)
	  return null;
	else if (count == 6 && state == 1)
	  urlProps.put("PGHOST", token);
	else if (count == 7 && token.equals(":")) state = 2;
	else if (count == 8 && state == 2) {
	  try {
	    Integer portNumber = Integer.decode(token);
	    urlProps.put("PGPORT", portNumber.toString());
	  } catch (Exception e) {
	    return null;
	  }
	}
	else if ((count == 7 || count == 9) &&
		 (state == 1 || state == 2) && token.equals("/"))
	  state = -1;
	else if (state == -1) {
	  urlProps.put("PGDBNAME", token);
	  state = -2;
	}
	else if (state <= -2 && (count % 2) == 1) {
	  // PM Aug 2 1997 - added tests for ? and &
	  if (token.equals(";") || token.equals("?") || token.equals("&") ) state = -3;
	  else if (token.equals("=")) state = -5;
	}
	else if (state <= -2 && (count % 2) == 0) {
	  if (state == -3) key = token;
	  else if (state == -5) {
	    value = token;
	    //DriverManager.println("put("+key+","+value+")");
	    urlProps.put(key, value);
	    state = -2;
	  }
	}
      }
    }
    
    // PM June 29 1997
    // This now outputs the properties only if we are logging
    // PM Sep 13 1999 Commented out, as it throws a Deprecation warning
    // when compiled under JDK1.2.
    //if(DriverManager.getLogStream() != null)
    //  urlProps.list(DriverManager.getLogStream());
    
    return urlProps;
    
  }
  
  /**
   * @return the hostname portion of the URL
   */
  public String host()
  {
    return props.getProperty("PGHOST","localhost");
  }
  
  /**
   * @return the port number portion of the URL or -1 if no port was specified
   */
  public int port()
  {
    return Integer.parseInt(props.getProperty("PGPORT","5432"));
  }
  
  /**
   * @return the database name of the URL
   */
  public String database()
  {
    return props.getProperty("PGDBNAME");
  }
  
  /**
   * @return the value of any property specified in the URL or properties
   * passed to connect(), or null if not found.
   */
  public String property(String name)
  {
    return props.getProperty(name);
  }
    
    /**
     * This method was added in v6.5, and simply throws an SQLException
     * for an unimplemented method. I decided to do it this way while
     * implementing the JDBC2 extensions to JDBC, as it should help keep the
     * overall driver size down.
     */
    public static SQLException notImplemented()
    {
	return new PSQLException("postgresql.unimplemented");
    }
}

