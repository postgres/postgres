package postgresql;

import java.io.*;
import java.math.*;
import java.sql.*;
import java.text.*;
import java.util.*;
import postgresql.largeobject.*;
import postgresql.util.*;

/**
 * A SQL Statement is pre-compiled and stored in a PreparedStatement object.
 * This object can then be used to efficiently execute this statement multiple
 * times.
 *
 * <p><B>Note:</B> The setXXX methods for setting IN parameter values must
 * specify types that are compatible with the defined SQL type of the input
 * parameter.  For instance, if the IN parameter has SQL type Integer, then
 * setInt should be used.
 *
 * <p>If arbitrary parameter type conversions are required, then the setObject 
 * method should be used with a target SQL type.
 *
 * @see ResultSet
 * @see java.sql.PreparedStatement
 */
public class PreparedStatement extends Statement implements java.sql.PreparedStatement 
{
	String sql;
	String[] templateStrings;
	String[] inStrings;
	Connection connection;

	/**
	 * Constructor for the PreparedStatement class.
	 * Split the SQL statement into segments - separated by the arguments.
	 * When we rebuild the thing with the arguments, we can substitute the
	 * args and join the whole thing together.
	 *
	 * @param conn the instanatiating connection
	 * @param sql the SQL statement with ? for IN markers
	 * @exception SQLException if something bad occurs
	 */
	public PreparedStatement(Connection connection, String sql) throws SQLException
	{
		super(connection);

		Vector v = new Vector();
		boolean inQuotes = false;
		int lastParmEnd = 0, i;

		this.sql = sql;
		this.connection = connection;
		for (i = 0; i < sql.length(); ++i)
		{
			int c = sql.charAt(i);

			if (c == '\'')
				inQuotes = !inQuotes;
			if (c == '?' && !inQuotes)
			{
				v.addElement(sql.substring (lastParmEnd, i));
				lastParmEnd = i + 1;
			}
		}
		v.addElement(sql.substring (lastParmEnd, sql.length()));

		templateStrings = new String[v.size()];
		inStrings = new String[v.size() - 1];
		clearParameters();

		for (i = 0 ; i < templateStrings.length; ++i)
			templateStrings[i] = (String)v.elementAt(i);
	}

	/**
	 * A Prepared SQL query is executed and its ResultSet is returned
	 *
	 * @return a ResultSet that contains the data produced by the
	 *	query - never null
	 * @exception SQLException if a database access error occurs
	 */
	public java.sql.ResultSet executeQuery() throws SQLException
	{
		StringBuffer s = new StringBuffer();
		int i;

		for (i = 0 ; i < inStrings.length ; ++i)
		{
			if (inStrings[i] == null)
				throw new SQLException("No value specified for parameter " + (i + 1));
			s.append (templateStrings[i]);
			s.append (inStrings[i]);
		}
		s.append(templateStrings[inStrings.length]);
		return super.executeQuery(s.toString()); 	// in Statement class
	}

	/**
	 * Execute a SQL INSERT, UPDATE or DELETE statement.  In addition,
	 * SQL statements that return nothing such as SQL DDL statements can
	 * be executed.
	 *
	 * @return either the row count for INSERT, UPDATE or DELETE; or
	 * 	0 for SQL statements that return nothing.
	 * @exception SQLException if a database access error occurs
	 */
	public int executeUpdate() throws SQLException
	{
		StringBuffer s = new StringBuffer();
		int i;

		for (i = 0 ; i < inStrings.length ; ++i)
		{
			if (inStrings[i] == null)
				throw new SQLException("No value specified for parameter " + (i + 1));
			s.append (templateStrings[i]);
			s.append (inStrings[i]);
		}
		s.append(templateStrings[inStrings.length]);
		return super.executeUpdate(s.toString()); 	// in Statement class
	}	

	/**
	 * Set a parameter to SQL NULL
	 *
	 * <p><B>Note:</B> You must specify the parameters SQL type (although
	 * PostgreSQL ignores it)
	 *
	 * @param parameterIndex the first parameter is 1, etc...
	 * @param sqlType the SQL type code defined in java.sql.Types
	 * @exception SQLException if a database access error occurs
	 */
	public void setNull(int parameterIndex, int sqlType) throws SQLException
	{
		set(parameterIndex, "null");
	}

	/**
	 * Set a parameter to a Java boolean value.  The driver converts this
	 * to a SQL BIT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setBoolean(int parameterIndex, boolean x) throws SQLException
	{
		set(parameterIndex, x ? "'t'" : "'f'");
	}

	/**
	 * Set a parameter to a Java byte value.  The driver converts this to
	 * a SQL TINYINT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setByte(int parameterIndex, byte x) throws SQLException
	{
		set(parameterIndex, (new Integer(x)).toString());
	}

	/**
	 * Set a parameter to a Java short value.  The driver converts this
	 * to a SQL SMALLINT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setShort(int parameterIndex, short x) throws SQLException
	{
		set(parameterIndex, (new Integer(x)).toString());
	}

	/**
	 * Set a parameter to a Java int value.  The driver converts this to
	 * a SQL INTEGER value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setInt(int parameterIndex, int x) throws SQLException
	{
		set(parameterIndex, (new Integer(x)).toString());
	}

	/**
	 * Set a parameter to a Java long value.  The driver converts this to
	 * a SQL BIGINT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setLong(int parameterIndex, long x) throws SQLException
	{
		set(parameterIndex, (new Long(x)).toString());
	}

	/**
	 * Set a parameter to a Java float value.  The driver converts this
	 * to a SQL FLOAT value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setFloat(int parameterIndex, float x) throws SQLException
	{
		set(parameterIndex, (new Float(x)).toString());
	}

	/**
	 * Set a parameter to a Java double value.  The driver converts this
	 * to a SQL DOUBLE value when it sends it to the database
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setDouble(int parameterIndex, double x) throws SQLException
	{
		set(parameterIndex, (new Double(x)).toString());
	}

	/**
	 * Set a parameter to a java.lang.BigDecimal value.  The driver
	 * converts this to a SQL NUMERIC value when it sends it to the
	 * database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setBigDecimal(int parameterIndex, BigDecimal x) throws SQLException
	{
		set(parameterIndex, x.toString());
	}

	/**
	 * Set a parameter to a Java String value.  The driver converts this
	 * to a SQL VARCHAR or LONGVARCHAR value (depending on the arguments
	 * size relative to the driver's limits on VARCHARs) when it sends it
	 * to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setString(int parameterIndex, String x) throws SQLException
	{
	  // if the passed string is null, then set this column to null
	  if(x==null)
	    set(parameterIndex,"null");
	  else {
	    StringBuffer b = new StringBuffer();
	    int i;
	    
	    b.append('\'');
	    for (i = 0 ; i < x.length() ; ++i)
	      {
		char c = x.charAt(i);
		if (c == '\\' || c == '\'')
		  b.append((char)'\\');
		b.append(c);
	      }
	    b.append('\'');
	    set(parameterIndex, b.toString());
	  }
	}

  /**
   * Set a parameter to a Java array of bytes.  The driver converts this
   * to a SQL VARBINARY or LONGVARBINARY (depending on the argument's
   * size relative to the driver's limits on VARBINARYs) when it sends
   * it to the database.
   *
   * <p>Implementation note:
   * <br>With postgresql, this creates a large object, and stores the
   * objects oid in this column.
   *
   * @param parameterIndex the first parameter is 1...
   * @param x the parameter value
   * @exception SQLException if a database access error occurs
   */
  public void setBytes(int parameterIndex, byte x[]) throws SQLException
  {
    LargeObjectManager lom = connection.getLargeObjectAPI();
    int oid = lom.create();
    LargeObject lob = lom.open(oid);
    lob.write(x);
    lob.close();
    setInt(parameterIndex,oid);
  }

	/**
	 * Set a parameter to a java.sql.Date value.  The driver converts this
	 * to a SQL DATE value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setDate(int parameterIndex, java.sql.Date x) throws SQLException
	{
	  SimpleDateFormat df = new SimpleDateFormat("''"+connection.getDateStyle()+"''");
	  	  
	  // Ideally the following should work:
	  //
	  //    set(parameterIndex, df.format(x));
	  //
	  // however, SimpleDateFormat seems to format a date to the previous
	  // day. So a fix (for now) is to add a day before formatting.
	  // This needs more people to confirm this is really happening, or
	  // possibly for us to implement our own formatting code.
	  //
	  // I've tested this with the Linux jdk1.1.3 and the Win95 JRE1.1.5
	  //
	  set(parameterIndex, df.format(new java.util.Date(x.getTime()+DAY)));
	}
  
  // This equates to 1 day
  private static final int DAY = 86400000;

	/**
	 * Set a parameter to a java.sql.Time value.  The driver converts
	 * this to a SQL TIME value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...));
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setTime(int parameterIndex, Time x) throws SQLException
	{
		set(parameterIndex, "'" + x.toString() + "'");
	}

	/**
	 * Set a parameter to a java.sql.Timestamp value.  The driver converts
	 * this to a SQL TIMESTAMP value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setTimestamp(int parameterIndex, Timestamp x) throws SQLException
	{
		set(parameterIndex, "'" + x.toString() + "'");
	}

	/**
	 * When a very large ASCII value is input to a LONGVARCHAR parameter,
	 * it may be more practical to send it via a java.io.InputStream.
	 * JDBC will read the data from the stream as needed, until it reaches
	 * end-of-file.  The JDBC driver will do any necessary conversion from
	 * ASCII to the database char format.
	 *
	 * <P><B>Note:</B> This stream object can either be a standard Java
	 * stream object or your own subclass that implements the standard
	 * interface.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @param length the number of bytes in the stream
	 * @exception SQLException if a database access error occurs
	 */
	public void setAsciiStream(int parameterIndex, InputStream x, int length) throws SQLException
	{
		setBinaryStream(parameterIndex, x, length);
	}

	/**
	 * When a very large Unicode value is input to a LONGVARCHAR parameter,
	 * it may be more practical to send it via a java.io.InputStream.
	 * JDBC will read the data from the stream as needed, until it reaches
	 * end-of-file.  The JDBC driver will do any necessary conversion from
	 * UNICODE to the database char format.
	 *
	 * <P><B>Note:</B> This stream object can either be a standard Java
	 * stream object or your own subclass that implements the standard
	 * interface.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setUnicodeStream(int parameterIndex, InputStream x, int length) throws SQLException
	{
		setBinaryStream(parameterIndex, x, length);
	}

	/**
	 * When a very large binary value is input to a LONGVARBINARY parameter,
	 * it may be more practical to send it via a java.io.InputStream.
	 * JDBC will read the data from the stream as needed, until it reaches
	 * end-of-file.  
	 *
	 * <P><B>Note:</B> This stream object can either be a standard Java
	 * stream object or your own subclass that implements the standard
	 * interface.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setBinaryStream(int parameterIndex, InputStream x, int length) throws SQLException
	{
		throw new SQLException("InputStream as parameter not supported");
	}

	/**
	 * In general, parameter values remain in force for repeated used of a
	 * Statement.  Setting a parameter value automatically clears its
	 * previous value.  However, in coms cases, it is useful to immediately
	 * release the resources used by the current parameter values; this
	 * can be done by calling clearParameters
	 *
	 * @exception SQLException if a database access error occurs
	 */
	public void clearParameters() throws SQLException
	{
		int i;

		for (i = 0 ; i < inStrings.length ; i++)
			inStrings[i] = null;
	}

	/**
	 * Set the value of a parameter using an object; use the java.lang
	 * equivalent objects for integral values.
	 *
	 * <P>The given Java object will be converted to the targetSqlType before
	 * being sent to the database.
	 *
	 * <P>note that this method may be used to pass database-specific
	 * abstract data types.  This is done by using a Driver-specific
	 * Java type and using a targetSqlType of java.sql.Types.OTHER
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the object containing the input parameter value
	 * @param targetSqlType The SQL type to be send to the database
	 * @param scale For java.sql.Types.DECIMAL or java.sql.Types.NUMERIC
	 *	types this is the number of digits after the decimal.  For 
	 *	all other types this value will be ignored.
	 * @exception SQLException if a database access error occurs
	 */
	public void setObject(int parameterIndex, Object x, int targetSqlType, int scale) throws SQLException
	{
		switch (targetSqlType)
		{
			case Types.TINYINT:
			case Types.SMALLINT:
			case Types.INTEGER:
			case Types.BIGINT:
			case Types.REAL:
			case Types.FLOAT:
			case Types.DOUBLE:
			case Types.DECIMAL:
			case Types.NUMERIC:
				if (x instanceof Boolean)
					set(parameterIndex, ((Boolean)x).booleanValue() ? "1" : "0");
				else
					set(parameterIndex, x.toString());
				break;
			case Types.CHAR:
			case Types.VARCHAR:
			case Types.LONGVARCHAR:
				setString(parameterIndex, x.toString());
			case Types.DATE:
				setDate(parameterIndex, (java.sql.Date)x);
			case Types.TIME:
				setTime(parameterIndex, (Time)x);
			case Types.TIMESTAMP:
				setTimestamp(parameterIndex, (Timestamp)x);
			case Types.OTHER:
				setString(parameterIndex, ((PGobject)x).getValue());
			default:
				throw new SQLException("Unknown Types value");
		}
	}

	public void setObject(int parameterIndex, Object x, int targetSqlType) throws SQLException
	{
		setObject(parameterIndex, x, targetSqlType, 0);
	}

	public void setObject(int parameterIndex, Object x) throws SQLException
	{
		if (x instanceof String)
			setString(parameterIndex, (String)x);
		else if (x instanceof BigDecimal)
			setBigDecimal(parameterIndex, (BigDecimal)x);
		else if (x instanceof Integer)
			setInt(parameterIndex, ((Integer)x).intValue());
		else if (x instanceof Long)
			setLong(parameterIndex, ((Long)x).longValue());
		else if (x instanceof Float)
			setFloat(parameterIndex, ((Float)x).floatValue());
		else if (x instanceof Double)
			setDouble(parameterIndex, ((Double)x).doubleValue());
		else if (x instanceof byte[])
			setBytes(parameterIndex, (byte[])x);
		else if (x instanceof java.sql.Date)
			setDate(parameterIndex, (java.sql.Date)x);
		else if (x instanceof Time)
			setTime(parameterIndex, (Time)x);
		else if (x instanceof Timestamp)
			setTimestamp(parameterIndex, (Timestamp)x);
		else if (x instanceof Boolean)
			setBoolean(parameterIndex, ((Boolean)x).booleanValue());
		else if (x instanceof PGobject)
			setString(parameterIndex, ((PGobject)x).getValue());
		else
			throw new SQLException("Unknown object type");
	}

	/**
	 * Some prepared statements return multiple results; the execute method
	 * handles these complex statements as well as the simpler form of 
	 * statements handled by executeQuery and executeUpdate
	 *
	 * @return true if the next result is a ResultSet; false if it is an
	 *	update count or there are no more results
	 * @exception SQLException if a database access error occurs
	 */
	public boolean execute() throws SQLException
	{
		StringBuffer s = new StringBuffer();
		int i;

		for (i = 0 ; i < inStrings.length ; ++i)
		{
			if (inStrings[i] == null)
				throw new SQLException("No value specified for parameter " + (i + 1));
			s.append (templateStrings[i]);
			s.append (inStrings[i]);
		}
		s.append(templateStrings[inStrings.length]);
		return super.execute(s.toString()); 	// in Statement class
	}

	// **************************************************************
	//	END OF PUBLIC INTERFACE	
	// **************************************************************
	
	/**
	 * There are a lot of setXXX classes which all basically do
	 * the same thing.  We need a method which actually does the
	 * set for us.
	 *
	 * @param paramIndex the index into the inString
	 * @param s a string to be stored
	 * @exception SQLException if something goes wrong
	 */
	private void set(int paramIndex, String s) throws SQLException
	{
		if (paramIndex < 1 || paramIndex > inStrings.length)
			throw new SQLException("Parameter index out of range");
		inStrings[paramIndex - 1] = s;
	}
}
