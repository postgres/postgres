package postgresql;

import java.sql.*;
import java.util.*;
import postgresql.*;

/**
 * @version 1.0 15-APR-1997
 * @author <A HREF="mailto:adrian@hottub.org">Adrian Hall</A>
 *
 * The Java SQL framework allows for multiple database drivers.  Each
 * driver should supply a class that implements the Driver interface
 *
 * The DriverManager will try to load as many drivers as it can find and then
 * for any given connection request, it will ask each driver in turn to try
 * to connect to the target URL.
 *
 * It is strongly recommended that each Driver class should be small and
 * standalone so that the Driver class can be loaded and queried without
 * bringing in vast quantities of supporting code.
 *
 * When a Driver class is loaded, it should create an instance of itself and
 * register it with the DriverManager.  This means that a user can load and
 * register a driver by doing Class.forName("foo.bah.Driver")
 *
 * @see postgresql.Connection
 * @see java.sql.Driver
 */
public class Driver implements java.sql.Driver 
{

	static 
	{
		try
		{
			new Driver();
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
		java.sql.DriverManager.registerDriver(this);
	}
	
	/**
	 * Try to make a database connection to the given URL.  The driver
	 * should return "null" if it realizes it is the wrong kind of
	 * driver to connect to the given URL.  This will be common, as
	 * when the JDBC driverManager is asked to connect to a given URL,
	 * it passes the URL to each loaded driver in turn.
	 *
	 * The driver should raise an SQLException if it is the right driver
	 * to connect to the given URL, but has trouble connecting to the
	 * database.
	 *
	 * The java.util.Properties argument can be used to pass arbitrary
	 * string tag/value pairs as connection arguments.  Normally, at least
	 * "user" and "password" properties should be included in the 
	 * properties.
	 *
	 * Our protocol takes the form:
	 * <PRE>
	 *	jdbc:postgresql://host:port/database
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
		DriverURL dr = new DriverURL(url);
		int port;

		if (!(dr.protocol().equals("jdbc")))
                        return null;
		if (!(dr.subprotocol().equals("postgresql")))
                        return null;
		if (dr.host().equals("unknown"))
                        return null;
		port = dr.port();
		if (port == -1)
                        port = 5432;    // Default PostgreSQL port
		return new Connection (dr.host(), port, info, dr.database(), url, this);
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
		DriverURL dr = new DriverURL(url);

		if (dr.protocol().equals("jdbc"))
			if (dr.subprotocol().equals("postgresql"))
				return true;
		return false;
	}

	/**
	 * The getPropertyInfo method is intended to allow a generic GUI
	 * tool to discover what properties it should prompt a human for
	 * in order to get enough information to connect to a database.
	 * Note that depending on the values the human has supplied so
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
		return null;		// We don't need anything except
					// the username, which is a default
	}

	/**
	 * Gets the drivers major version number
	 *
	 * @return the drivers major version number
	 */
	public int getMajorVersion()
	{
		return 1;
	}
	
	/**
	 * Get the drivers minor version number
	 *
	 * @return the drivers minor version number
	 */
	public int getMinorVersion()
	{
		return 0;
	}
	
	/**
	 * Report whether the driver is a genuine JDBC compliant driver.  A
	 * driver may only report "true" here if it passes the JDBC compliance
	 * tests, otherwise it is required to return false.  JDBC compliance
	 * requires full support for the JDBC API and full support for SQL 92
	 * Entry Level.  
	 */
	public boolean jdbcCompliant()
	{
		return false;
	}
}

/**
 * The DriverURL class splits a JDBC URL into its subcomponents
 *
 *	protocol:subprotocol:/[/host[:port]/][database]
 */
class DriverURL
{
	private String protocol, subprotocol, host, database;
	private int port = -1;

	/**
	 * Constructs a new DriverURL, splitting the specified URL into its
	 * component parts
	 */
	public DriverURL(String url) throws SQLException
	{
		int a, b, c;
		String tmp, hostport, dbportion;

		a = url.indexOf(':');
		if (a == -1)
			throw new SQLException("Bad URL Protocol specifier");
		b = url.indexOf(':', a+1);
		if (b == -1)
			throw new SQLException("Bad URL Subprotocol specifier");
                protocol = new String(url.substring(0, a));
		subprotocol = new String(url.substring(a+1, b));
                tmp = new String(url.substring(b+1, url.length()));
		if (tmp.length() < 2)
			throw new SQLException("Bad URL Database specifier");
                if (!tmp.substring(0, 2).equals("//"))
		{
			host = new String("unknown");
			port = -1;
			database = new String(tmp.substring(1, tmp.length()));
			return;
		}
		dbportion = new String(tmp.substring(2, tmp.length()));
		c = dbportion.indexOf('/');
		if (c == -1)
			throw new SQLException("Bad URL Database specifier");
		a = dbportion.indexOf(':');
		if (a == -1)
		{
			host = new String(dbportion.substring(0, c));
			port = -1;
			database = new String(dbportion.substring(c+1, dbportion.length()));
		} else {
			host = new String(dbportion.substring(0, a));
			port = Integer.valueOf(dbportion.substring(a+1, c)).intValue();
			database = new String(dbportion.substring(c+1, dbportion.length()));
		}
	}

	/**
	 * Returns the protocol name of the DriverURL
	 */
	public String protocol()
	{
		return protocol;
	}

	/**
	 * Returns the subprotocol name of the DriverURL
	 */
	public String subprotocol()
	{
		return subprotocol;
	}

	/**
	 * Returns the hostname portion of the URL
	 */
	public String host()
	{
		return host;
	}

	/**
	 * Returns the port number portion of the URL
	 * or -1 if no port was specified
	 */
	public int port()
	{
		return port;
	}

	/**
	 * Returns the database name of the URL
	 */
	public String database()
	{
		return database;
	}
}
