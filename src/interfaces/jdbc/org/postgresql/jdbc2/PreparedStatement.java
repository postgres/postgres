package org.postgresql.jdbc2;

// IMPORTANT NOTE: This file implements the JDBC 2 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 1 class in the
// org.postgresql.jdbc1 package.

import java.io.*;
import java.math.*;
import java.sql.*;
import java.text.*;
import java.util.*;
import org.postgresql.largeobject.*;
import org.postgresql.util.*;

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

        // Some performance caches
        private StringBuffer sbuf = new StringBuffer();

        // We use ThreadLocal for SimpleDateFormat's because they are not that
        // thread safe, so each calling thread has its own object.
        private ThreadLocal tl_df   = new ThreadLocal(); // setDate() SimpleDateFormat
        private ThreadLocal tl_tsdf = new ThreadLocal(); // setTimestamp() SimpleDateFormat

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
         * New in 7.1 - overides Statement.close() to dispose of a few local objects
         */
        public void close() throws SQLException {
          // free the ThreadLocal caches
          tl_df.set(null);

          super.close();
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
		return super.executeQuery(compileQuery()); 	// in Statement class
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
		return super.executeUpdate(compileQuery()); 	// in Statement class
	}

        /**
         * Helper - this compiles the SQL query from the various parameters
         * This is identical to toString() except it throws an exception if a
         * parameter is unused.
         */
        private synchronized String compileQuery() throws SQLException
        {
                sbuf.setLength(0);
		int i;

		for (i = 0 ; i < inStrings.length ; ++i)
		{
			if (inStrings[i] == null)
				throw new PSQLException("postgresql.prep.param",new Integer(i + 1));
			sbuf.append (templateStrings[i]).append (inStrings[i]);
		}
		sbuf.append(templateStrings[inStrings.length]);
                return sbuf.toString();
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
		set(parameterIndex, Integer.toString(x));
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
		set(parameterIndex, Integer.toString(x));
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
		set(parameterIndex, Integer.toString(x));
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
		set(parameterIndex, Long.toString(x));
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
		set(parameterIndex, Float.toString(x));
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
		set(parameterIndex, Double.toString(x));
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
            // use the shared buffer object. Should never clash but this makes
            // us thread safe!
	    synchronized(sbuf) {
              sbuf.setLength(0);
              int i;

              sbuf.append('\'');
              for (i = 0 ; i < x.length() ; ++i)
                {
                  char c = x.charAt(i);
                  if (c == '\\' || c == '\'')
                    sbuf.append((char)'\\');
                  sbuf.append(c);
                }
              sbuf.append('\'');
              set(parameterIndex, sbuf.toString());
            }
	  }
	}

  /**
   * Set a parameter to a Java array of bytes.  The driver converts this
   * to a SQL VARBINARY or LONGVARBINARY (depending on the argument's
   * size relative to the driver's limits on VARBINARYs) when it sends
   * it to the database.
   *
   * <p>Implementation note:
   * <br>With org.postgresql, this creates a large object, and stores the
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
          SimpleDateFormat df = (SimpleDateFormat) tl_df.get();
          if(df==null) {
            df = new SimpleDateFormat("''yyyy-MM-dd''");
            tl_df.set(df);
          }

	  set(parameterIndex, df.format(x));

	  // The above is how the date should be handled.
	  //
	  // However, in JDK's prior to 1.1.6 (confirmed with the
	  // Linux jdk1.1.3 and the Win95 JRE1.1.5), SimpleDateFormat seems
	  // to format a date to the previous day. So the fix is to add a day
	  // before formatting.
	  //
	  // PS: 86400000 is one day
	  //
	  //set(parameterIndex, df.format(new java.util.Date(x.getTime()+86400000)));
	}

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
          SimpleDateFormat df = (SimpleDateFormat) tl_tsdf.get();
          if(df==null) {
            df = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss");
            tl_tsdf.set(df);
          }
          df.setTimeZone(TimeZone.getTimeZone("GMT"));

          // Use the shared StringBuffer
          synchronized(sbuf) {
            sbuf.setLength(0);
            sbuf.append("'").append(df.format(x)).append('.').append(x.getNanos()/10000000).append("+00'");
            set(parameterIndex, sbuf.toString());
          }

          // The above works, but so does the following. I'm leaving the above in, but this seems
          // to be identical. Pays to read the docs ;-)
          //set(parameterIndex,"'"+x.toString()+"'");
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
	 * ** DEPRECIATED IN JDBC 2 **
	 *
	 * <P><B>Note:</B> This stream object can either be a standard Java
	 * stream object or your own subclass that implements the standard
	 * interface.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 * @deprecated
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
          LargeObjectManager lom = connection.getLargeObjectAPI();
          int oid = lom.create();
          LargeObject lob = lom.open(oid);
          OutputStream los = lob.getOutputStream();
          try {
            // could be buffered, but then the OutputStream returned by LargeObject
            // is buffered internally anyhow, so there would be no performance
            // boost gained, if anything it would be worse!
            int c=x.read();
            int p=0;
            while(c>-1 && p<length) {
              los.write(c);
              c=x.read();
              p++;
            }
            los.close();
          } catch(IOException se) {
            throw new PSQLException("postgresql.prep.is",se);
          }
          // lob is closed by the stream so don't call lob.close()
          setInt(parameterIndex,oid);
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
				break;
			case Types.DATE:
				setDate(parameterIndex, (java.sql.Date)x);
				break;
			case Types.TIME:
				setTime(parameterIndex, (Time)x);
				break;
			case Types.TIMESTAMP:
				setTimestamp(parameterIndex, (Timestamp)x);
				break;
			case Types.OTHER:
				setString(parameterIndex, ((PGobject)x).getValue());
				break;
			default:
				throw new PSQLException("postgresql.prep.type");
		}
	}

	public void setObject(int parameterIndex, Object x, int targetSqlType) throws SQLException
	{
		setObject(parameterIndex, x, targetSqlType, 0);
	}

  /**
   * This stores an Object into a parameter.
   * <p>New for 6.4, if the object is not recognised, but it is
   * Serializable, then the object is serialised using the
   * org.postgresql.util.Serialize class.
   */
	public void setObject(int parameterIndex, Object x) throws SQLException
	{
		if (x instanceof String)
			setString(parameterIndex, (String)x);
		else if (x instanceof BigDecimal)
			setBigDecimal(parameterIndex, (BigDecimal)x);
		else if (x instanceof Short)
			setShort(parameterIndex, ((Short)x).shortValue());
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
			setLong(parameterIndex, connection.putObject(x));
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
		return super.execute(compileQuery()); 	// in Statement class
	}

	/**
	 * Returns the SQL statement with the current template values
	 * substituted.
         * NB: This is identical to compileQuery() except instead of throwing
         * SQLException if a parameter is null, it places ? instead.
	 */
	public String toString() {
          synchronized(sbuf) {
                sbuf.setLength(0);
		int i;

		for (i = 0 ; i < inStrings.length ; ++i)
		{
			if (inStrings[i] == null)
				sbuf.append( '?' );
			else
				sbuf.append (templateStrings[i]);
			sbuf.append (inStrings[i]);
		}
		sbuf.append(templateStrings[inStrings.length]);
		return sbuf.toString();
          }
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
			throw new PSQLException("postgresql.prep.range");
		inStrings[paramIndex - 1] = s;
	}

    // ** JDBC 2 Extensions **

    /**
     * This parses the query and adds it to the current batch
     */
    public void addBatch() throws SQLException
    {
	super.addBatch(compileQuery());
    }

    /**
     * Not sure what this one does, so I'm saying this returns the MetaData for
     * the last ResultSet returned!
     */
    public java.sql.ResultSetMetaData getMetaData() throws SQLException
    {
      java.sql.ResultSet rs = getResultSet();
      if(rs!=null)
        return rs.getMetaData();

      // Does anyone really know what this method does?
      return null;
    }

    public void setArray(int i,Array x) throws SQLException
    {
	throw org.postgresql.Driver.notImplemented();
    }

    /**
     * Sets a Blob - basically its similar to setBinaryStream()
     */
    public void setBlob(int i,Blob x) throws SQLException
    {
      setBinaryStream(i,x.getBinaryStream(),(int)x.length());
    }

    /**
     * This is similar to setBinaryStream except it uses a Reader instead of
     * InputStream.
     */
    public void setCharacterStream(int i,java.io.Reader x,int length) throws SQLException
    {
          LargeObjectManager lom = connection.getLargeObjectAPI();
          int oid = lom.create();
          LargeObject lob = lom.open(oid);
          OutputStream los = lob.getOutputStream();
          try {
            // could be buffered, but then the OutputStream returned by LargeObject
            // is buffered internally anyhow, so there would be no performance
            // boost gained, if anything it would be worse!
            int c=x.read();
            int p=0;
            while(c>-1 && p<length) {
              los.write(c);
              c=x.read();
              p++;
            }
            los.close();
          } catch(IOException se) {
            throw new PSQLException("postgresql.prep.is",se);
          }
          // lob is closed by the stream so don't call lob.close()
          setInt(i,oid);
    }

    /**
     * New in 7.1
     */
    public void setClob(int i,Clob x) throws SQLException
    {
      setBinaryStream(i,x.getAsciiStream(),(int)x.length());
    }

    /**
     * At least this works as in PostgreSQL null represents anything null ;-)
     *
     * New in 7,1
     */
    public void setNull(int i,int t,String s) throws SQLException
    {
	setNull(i,t);
    }

    public void setRef(int i,Ref x) throws SQLException
    {
	throw org.postgresql.Driver.notImplemented();
    }

    /**
     * New in 7,1
     */
    public void setDate(int i,java.sql.Date d,java.util.Calendar cal) throws SQLException
    {
      if(cal==null)
        setDate(i,d);
      else {
        cal.setTime(d);
        setDate(i,new java.sql.Date(cal.getTime().getTime()));
      }
    }

    /**
     * New in 7,1
     */
    public void setTime(int i,Time t,java.util.Calendar cal) throws SQLException
    {
      if(cal==null)
        setTime(i,t);
      else {
        cal.setTime(t);
        setTime(i,new java.sql.Time(cal.getTime().getTime()));
      }
    }

    /**
     * New in 7,1
     */
    public void setTimestamp(int i,Timestamp t,java.util.Calendar cal) throws SQLException
    {
      if(cal==null)
        setTimestamp(i,t);
      else {
        cal.setTime(t);
        setTimestamp(i,new java.sql.Timestamp(cal.getTime().getTime()));
      }
    }

}
