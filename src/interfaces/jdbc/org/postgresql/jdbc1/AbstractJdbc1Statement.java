package org.postgresql.jdbc1;

import java.io.*;

import java.math.BigDecimal;
import java.sql.*;
import java.util.Vector;
import org.postgresql.largeobject.*;
import org.postgresql.util.*;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc1/Attic/AbstractJdbc1Statement.java,v 1.2 2002/07/24 22:08:39 barry Exp $
 * This class defines methods of the jdbc1 specification.  This class is
 * extended by org.postgresql.jdbc2.AbstractJdbc2Statement which adds the jdbc2
 * methods.  The real Statement class (for jdbc1) is org.postgresql.jdbc1.Jdbc1Statement
 */
public abstract class AbstractJdbc1Statement implements org.postgresql.PGStatement
{

        // The connection who created us
	protected AbstractJdbc1Connection connection;  

	/** The warnings chain. */
	protected SQLWarning warnings = null;

	/** Maximum number of rows to return, 0 = unlimited */
	protected int maxrows = 0;

	/** Timeout (in seconds) for a query (not used) */
	protected int timeout = 0;

	protected boolean escapeProcessing = true;

	/** The current results */
	protected java.sql.ResultSet result = null;

	// Static variables for parsing SQL when escapeProcessing is true.
	private static final short IN_SQLCODE = 0;
	private static final short IN_STRING = 1;
	private static final short BACKSLASH = 2;
	private static final short ESC_TIMEDATE = 3;

	// Some performance caches
	private StringBuffer sbuf = new StringBuffer();

        //Used by the preparedstatement style methods
	protected String sql;
	protected String[] templateStrings;
	protected String[] inStrings;



	public AbstractJdbc1Statement (AbstractJdbc1Connection connection)
	{
		this.connection = connection;
	}

	public AbstractJdbc1Statement (AbstractJdbc1Connection connection, String sql) throws SQLException
	{
		this.sql = sql;
		this.connection = connection;
                parseSqlStmt();  // this allows Callable stmt to override
	}

	protected void parseSqlStmt () throws SQLException {
		Vector v = new Vector();
		boolean inQuotes = false;
		int lastParmEnd = 0, i;

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


	/*
	 * Execute a SQL statement that retruns a single ResultSet
	 *
	 * @param sql typically a static SQL SELECT statement
	 * @return a ResulSet that contains the data produced by the query
	 * @exception SQLException if a database access error occurs
	 */
	public java.sql.ResultSet executeQuery(String sql) throws SQLException
	{
		this.execute(sql);
		while (result != null && !((AbstractJdbc1ResultSet)result).reallyResultSet())
			result = ((AbstractJdbc1ResultSet)result).getNext();
		if (result == null)
			throw new PSQLException("postgresql.stat.noresult");
		return result;
	}

	/*
	 * A Prepared SQL query is executed and its ResultSet is returned
	 *
	 * @return a ResultSet that contains the data produced by the
	 *		 *	query - never null
	 * @exception SQLException if a database access error occurs
	 */
	public java.sql.ResultSet executeQuery() throws SQLException
	{
	    return executeQuery(compileQuery());
	}

	/*
	 * Execute a SQL INSERT, UPDATE or DELETE statement.  In addition
	 * SQL statements that return nothing such as SQL DDL statements
	 * can be executed
	 *
	 * @param sql a SQL statement
	 * @return either a row count, or 0 for SQL commands
	 * @exception SQLException if a database access error occurs
	 */
	public int executeUpdate(String sql) throws SQLException
	{
		this.execute(sql);
		if (((AbstractJdbc1ResultSet)result).reallyResultSet())
			throw new PSQLException("postgresql.stat.result");
		return this.getUpdateCount();
	}

	/*
	 * Execute a SQL INSERT, UPDATE or DELETE statement.  In addition,
	 * SQL statements that return nothing such as SQL DDL statements can
	 * be executed.
	 *
	 * @return either the row count for INSERT, UPDATE or DELETE; or
	 *		 *	0 for SQL statements that return nothing.
	 * @exception SQLException if a database access error occurs
	 */
	public int executeUpdate() throws SQLException
	{
	    return executeUpdate(compileQuery());
	}

	/*
	 * Execute a SQL statement that may return multiple results. We
	 * don't have to worry about this since we do not support multiple
	 * ResultSets.	 You can use getResultSet or getUpdateCount to
	 * retrieve the result.
	 *
	 * @param sql any SQL statement
	 * @return true if the next result is a ResulSet, false if it is
	 *	an update count or there are no more results
	 * @exception SQLException if a database access error occurs
	 */
	public boolean execute(String sql) throws SQLException
	{
		if (escapeProcessing)
			sql = escapeSQL(sql);

		// New in 7.1, if we have a previous resultset then force it to close
		// This brings us nearer to compliance, and helps memory management.
		// Internal stuff will call ExecSQL directly, bypassing this.
		if (result != null)
		{
			java.sql.ResultSet rs = getResultSet();
			if (rs != null)
				rs.close();
		}


		// New in 7.1, pass Statement so that ExecSQL can customise to it
		result = ((AbstractJdbc1Connection)connection).ExecSQL(sql, (java.sql.Statement)this);

		return (result != null && ((AbstractJdbc1ResultSet)result).reallyResultSet());
	}

	/*
	 * Some prepared statements return multiple results; the execute method
	 * handles these complex statements as well as the simpler form of
	 * statements handled by executeQuery and executeUpdate
	 *
	 * @return true if the next result is a ResultSet; false if it is an
	 *		 *	update count or there are no more results
	 * @exception SQLException if a database access error occurs
	 */
	public boolean execute() throws SQLException
	{
	    return execute(compileQuery());
	}

	/*
	 * setCursorName defines the SQL cursor name that will be used by
	 * subsequent execute methods.	This name can then be used in SQL
	 * positioned update/delete statements to identify the current row
	 * in the ResultSet generated by this statement.  If a database
	 * doesn't support positioned update/delete, this method is a
	 * no-op.
	 *
	 * <p><B>Note:</B> By definition, positioned update/delete execution
	 * must be done by a different Statement than the one which
	 * generated the ResultSet being used for positioning.	Also, cursor
	 * names must be unique within a Connection.
	 *
	 * <p>We throw an additional constriction.	There can only be one
	 * cursor active at any one time.
	 *
	 * @param name the new cursor name
	 * @exception SQLException if a database access error occurs
	 */
	public void setCursorName(String name) throws SQLException
	{
		((AbstractJdbc1Connection)connection).setCursorName(name);
	}


	/*
	 * getUpdateCount returns the current result as an update count,
	 * if the result is a ResultSet or there are no more results, -1
	 * is returned.  It should only be called once per result.
	 *
	 * @return the current result as an update count.
	 * @exception SQLException if a database access error occurs
	 */
	public int getUpdateCount() throws SQLException
	{
		if (result == null)
			return -1;
		if (((AbstractJdbc1ResultSet)result).reallyResultSet())
			return -1;
		return ((AbstractJdbc1ResultSet)result).getResultCount();
	}

	/*
	 * getMoreResults moves to a Statement's next result.  If it returns
	 * true, this result is a ResulSet.
	 *
	 * @return true if the next ResultSet is valid
	 * @exception SQLException if a database access error occurs
	 */
	public boolean getMoreResults() throws SQLException
	{
		result = ((AbstractJdbc1ResultSet)result).getNext();
		return (result != null && ((AbstractJdbc1ResultSet)result).reallyResultSet());
	}











	/*
	 * Returns the status message from the current Result.<p>
	 * This is used internally by the driver.
	 *
	 * @return status message from backend
	 */
	public String getResultStatusString()
	{
		if (result == null)
			return null;
		return ((AbstractJdbc1ResultSet)result).getStatusString();
	}

	/*
	 * The maxRows limit is set to limit the number of rows that
	 * any ResultSet can contain.  If the limit is exceeded, the
	 * excess rows are silently dropped.
	 *
	 * @return the current maximum row limit; zero means unlimited
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxRows() throws SQLException
	{
		return maxrows;
	}

	/*
	 * Set the maximum number of rows
	 *
	 * @param max the new max rows limit; zero means unlimited
	 * @exception SQLException if a database access error occurs
	 * @see getMaxRows
	 */
	public void setMaxRows(int max) throws SQLException
	{
		maxrows = max;
	}

	/*
	 * If escape scanning is on (the default), the driver will do escape
	 * substitution before sending the SQL to the database.
	 *
	 * @param enable true to enable; false to disable
	 * @exception SQLException if a database access error occurs
	 */
	public void setEscapeProcessing(boolean enable) throws SQLException
	{
		escapeProcessing = enable;
	}

	/*
	 * The queryTimeout limit is the number of seconds the driver
	 * will wait for a Statement to execute.  If the limit is
	 * exceeded, a SQLException is thrown.
	 *
	 * @return the current query timeout limit in seconds; 0 = unlimited
	 * @exception SQLException if a database access error occurs
	 */
	public int getQueryTimeout() throws SQLException
	{
		return timeout;
	}

	/*
	 * Sets the queryTimeout limit
	 *
	 * @param seconds - the new query timeout limit in seconds
	 * @exception SQLException if a database access error occurs
	 */
	public void setQueryTimeout(int seconds) throws SQLException
	{
		timeout = seconds;
	}

	/**
	 * This adds a warning to the warning chain.
	 * @param msg message to add
	 */
	public void addWarning(String msg)
        {
	    if (warnings != null)
		warnings.setNextWarning(new SQLWarning(msg));
	    else
		warnings = new SQLWarning(msg);
	}

	/*
	 * The first warning reported by calls on this Statement is
	 * returned.  A Statement's execute methods clear its SQLWarning
	 * chain.  Subsequent Statement warnings will be chained to this
	 * SQLWarning.
	 *
	 * <p>The Warning chain is automatically cleared each time a statement
	 * is (re)executed.
	 *
	 * <p><B>Note:</B>	If you are processing a ResultSet then any warnings
	 * associated with ResultSet reads will be chained on the ResultSet
	 * object.
	 *
	 * @return the first SQLWarning on null
	 * @exception SQLException if a database access error occurs
	 */
	public SQLWarning getWarnings() throws SQLException
	{
		return warnings;
	}

	/*
	 * The maxFieldSize limit (in bytes) is the maximum amount of
	 * data returned for any column value; it only applies to
	 * BINARY, VARBINARY, LONGVARBINARY, CHAR, VARCHAR and LONGVARCHAR
	 * columns.  If the limit is exceeded, the excess data is silently
	 * discarded.
	 *
	 * @return the current max column size limit; zero means unlimited
	 * @exception SQLException if a database access error occurs
	 */
	public int getMaxFieldSize() throws SQLException
	{
		return 8192;		// We cannot change this
	}

	/*
	 * Sets the maxFieldSize - NOT! - We throw an SQLException just
	 * to inform them to stop doing this.
	 *
	 * @param max the new max column size limit; zero means unlimited
	 * @exception SQLException if a database access error occurs
	 */
	public void setMaxFieldSize(int max) throws SQLException
	{
		throw new PSQLException("postgresql.stat.maxfieldsize");
	}

	/*
	 * After this call, getWarnings returns null until a new warning
	 * is reported for this Statement.
	 *
	 * @exception SQLException if a database access error occurs
	 */
	public void clearWarnings() throws SQLException
	{
		warnings = null;
	}

	/*
	 * Cancel can be used by one thread to cancel a statement that
	 * is being executed by another thread.
	 * <p>
	 * Not implemented, this method is a no-op.
	 *
	 * @exception SQLException only because thats the spec.
	 */
	public void cancel() throws SQLException
	{
		throw new PSQLException("postgresql.unimplemented"); 
	}

	/*
	 * getResultSet returns the current result as a ResultSet.	It
	 * should only be called once per result.
	 *
	 * @return the current result set; null if there are no more
	 * @exception SQLException if a database access error occurs (why?)
	 */
	public java.sql.ResultSet getResultSet() throws SQLException
	{
		if (result != null && ((AbstractJdbc1ResultSet) result).reallyResultSet())
			return result;
		return null;
	}

	/*
	 * In many cases, it is desirable to immediately release a
	 * Statement's database and JDBC resources instead of waiting
	 * for this to happen when it is automatically closed.	The
	 * close method provides this immediate release.
	 *
	 * <p><B>Note:</B> A Statement is automatically closed when it is
	 * garbage collected.  When a Statement is closed, its current
	 * ResultSet, if one exists, is also closed.
	 *
	 * @exception SQLException if a database access error occurs (why?)
	 */
	public void close() throws SQLException
	{
		// Force the ResultSet to close
		java.sql.ResultSet rs = getResultSet();
		if (rs != null)
			rs.close();

		// Disasociate it from us (For Garbage Collection)
		result = null;
	}

	/*
	 * Filter the SQL string of Java SQL Escape clauses.
	 *
	 * Currently implemented Escape clauses are those mentioned in 11.3
	 * in the specification. Basically we look through the sql string for
	 * {d xxx}, {t xxx} or {ts xxx} in non-string sql code. When we find
	 * them, we just strip the escape part leaving only the xxx part.
	 * So, something like "select * from x where d={d '2001-10-09'}" would
	 * return "select * from x where d= '2001-10-09'".
	 */
	protected static String escapeSQL(String sql)
	{
		// Since escape codes can only appear in SQL CODE, we keep track
		// of if we enter a string or not.
		StringBuffer newsql = new StringBuffer();
		short state = IN_SQLCODE;

		int i = -1;
		int len = sql.length();
		while (++i < len)
		{
			char c = sql.charAt(i);
			switch (state)
			{
				case IN_SQLCODE:
					if (c == '\'')				  // start of a string?
						state = IN_STRING;
					else if (c == '{')			  // start of an escape code?
						if (i + 1 < len)
						{
							char next = sql.charAt(i + 1);
							if (next == 'd')
							{
								state = ESC_TIMEDATE;
								i++;
								break;
							}
							else if (next == 't')
							{
								state = ESC_TIMEDATE;
								i += (i + 2 < len && sql.charAt(i + 2) == 's') ? 2 : 1;
								break;
							}
						}
					newsql.append(c);
					break;

				case IN_STRING:
					if (c == '\'')				   // end of string?
						state = IN_SQLCODE;
					else if (c == '\\')			   // a backslash?
						state = BACKSLASH;

					newsql.append(c);
					break;

				case BACKSLASH:
					state = IN_STRING;

					newsql.append(c);
					break;

				case ESC_TIMEDATE:
					if (c == '}')
						state = IN_SQLCODE;		  // end of escape code.
					else
						newsql.append(c);
					break;
			} // end switch
		}

		return newsql.toString();
	}

    /* 
     *
     * The following methods are postgres extensions and are defined 
     * in the interface org.postgresql.Statement
     *
     */

	/*
	 * Returns the Last inserted/updated oid.  Deprecated in 7.2 because
         * range of OID values is greater than a java signed int.
	 * @deprecated Replaced by getLastOID in 7.2
	 */
	public int getInsertedOID() throws SQLException
	{
		if (result == null)
			return 0;
		return (int)((AbstractJdbc1ResultSet)result).getLastOID();
	}

	/*
	 * Returns the Last inserted/updated oid. 
	 * @return OID of last insert
         * @since 7.2
	 */
	public long getLastOID() throws SQLException
	{
		if (result == null)
			return 0;
		return ((AbstractJdbc1ResultSet)result).getLastOID();
	}

	/*
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

	/*
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

	/*
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

	/*
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

	/*
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

	/*
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

	/*
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

	/*
	 * Set a parameter to a Java double value.	The driver converts this
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

	/*
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
		if (x == null)
			setNull(parameterIndex, Types.OTHER);
		else
		{
		    set(parameterIndex, x.toString());
		}
	}

	/*
	 * Set a parameter to a Java String value.	The driver converts this
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
		if (x == null)
			setNull(parameterIndex, Types.OTHER);
		else
		{
			// use the shared buffer object. Should never clash but this makes
			// us thread safe!
			synchronized (sbuf)
			{
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

	/*
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
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports the bytea datatype for byte arrays
			if (null == x)
			{
				setNull(parameterIndex, Types.OTHER);
			}
			else
			{
				setString(parameterIndex, PGbytea.toPGString(x));
			}
		}
		else
		{
			//Version 7.1 and earlier support done as LargeObjects
			LargeObjectManager lom = connection.getLargeObjectAPI();
			int oid = lom.create();
			LargeObject lob = lom.open(oid);
			lob.write(x);
			lob.close();
			setInt(parameterIndex, oid);
		}
	}

	/*
	 * Set a parameter to a java.sql.Date value.  The driver converts this
	 * to a SQL DATE value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setDate(int parameterIndex, java.sql.Date x) throws SQLException
	{
		if (null == x)
		{
			setNull(parameterIndex, Types.OTHER);
		}
		else
		{
			set(parameterIndex, "'" + x.toString() + "'");
		}
	}

	/*
	 * Set a parameter to a java.sql.Time value.  The driver converts
	 * this to a SQL TIME value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...));
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setTime(int parameterIndex, Time x) throws SQLException
	{
		if (null == x)
		{
			setNull(parameterIndex, Types.OTHER);
		}
		else
		{
			set(parameterIndex, "'" + x.toString() + "'");
		}
	}

	/*
	 * Set a parameter to a java.sql.Timestamp value.  The driver converts
	 * this to a SQL TIMESTAMP value when it sends it to the database.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the parameter value
	 * @exception SQLException if a database access error occurs
	 */
	public void setTimestamp(int parameterIndex, Timestamp x) throws SQLException
	{
		if (null == x)
		{
			setNull(parameterIndex, Types.OTHER);
		}
		else
		    {
			// Use the shared StringBuffer
			synchronized (sbuf)
			{
				sbuf.setLength(0);
				sbuf.append("'");
                                //format the timestamp
                                //we do our own formating so that we can get a format
                                //that works with both timestamp with time zone and
                                //timestamp without time zone datatypes.
                                //The format is '2002-01-01 23:59:59.123456-0130'
                                //we need to include the local time and timezone offset
                                //so that timestamp without time zone works correctly
		                int l_year = x.getYear() + 1900;
                                sbuf.append(l_year);
                                sbuf.append('-');
		                int l_month = x.getMonth() + 1;
                                if (l_month < 10) sbuf.append('0');
                                sbuf.append(l_month);
                                sbuf.append('-');
		                int l_day = x.getDate();
                                if (l_day < 10) sbuf.append('0');
                                sbuf.append(l_day);
                                sbuf.append(' ');
		                int l_hours = x.getHours();
                                if (l_hours < 10) sbuf.append('0');
                                sbuf.append(l_hours);
                                sbuf.append(':');
		                int l_minutes = x.getMinutes();
                                if (l_minutes < 10) sbuf.append('0');
                                sbuf.append(l_minutes);
                                sbuf.append(':');
                                int l_seconds = x.getSeconds();
                                if (l_seconds < 10) sbuf.append('0');
                                sbuf.append(l_seconds);
                                // Make decimal from nanos.
                                char[] l_decimal = {'0','0','0','0','0','0','0','0','0'};
                                char[] l_nanos = Integer.toString(x.getNanos()).toCharArray();
                                System.arraycopy(l_nanos, 0, l_decimal, l_decimal.length - l_nanos.length, l_nanos.length);
                                sbuf.append('.');
                                if (connection.haveMinimumServerVersion("7.2")) {
                                  sbuf.append(l_decimal,0,6);
                                } else {
                                  // Because 7.1 include bug that "hh:mm:59.999" becomes "hh:mm:60.00".
                                  sbuf.append(l_decimal,0,2);
                                }
                                //add timezone offset
                                int l_offset = -(x.getTimezoneOffset());
                                int l_houros = l_offset/60;
                                if (l_houros >= 0) {
                                  sbuf.append('+');
                                } else {
                                  sbuf.append('-');
                                }
                                if (l_houros > -10 && l_houros < 10) sbuf.append('0');
                                if (l_houros >= 0) {
                                  sbuf.append(l_houros);
                                } else {
                                  sbuf.append(-l_houros);
                                }
                                int l_minos = l_offset - (l_houros *60);
                                if (l_minos != 0) {
                                  if (l_minos < 10) sbuf.append('0');
                                  sbuf.append(l_minos);
                                }
				sbuf.append("'");
				set(parameterIndex, sbuf.toString());
			}

		}
	}

	/*
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
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports AsciiStream for all PG text types (char, varchar, text)
			//As the spec/javadoc for this method indicate this is to be used for
			//large String values (i.e. LONGVARCHAR)  PG doesn't have a separate
			//long varchar datatype, but with toast all text datatypes are capable of
			//handling very large values.  Thus the implementation ends up calling
			//setString() since there is no current way to stream the value to the server
			try
			{
				InputStreamReader l_inStream = new InputStreamReader(x, "ASCII");
				char[] l_chars = new char[length];
				int l_charsRead = l_inStream.read(l_chars, 0, length);
				setString(parameterIndex, new String(l_chars, 0, l_charsRead));
			}
			catch (UnsupportedEncodingException l_uee)
			{
				throw new PSQLException("postgresql.unusual", l_uee);
			}
			catch (IOException l_ioe)
			{
				throw new PSQLException("postgresql.unusual", l_ioe);
			}
		}
		else
		{
			//Version 7.1 supported only LargeObjects by treating everything
			//as binary data
			setBinaryStream(parameterIndex, x, length);
		}
	}

	/*
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
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports AsciiStream for all PG text types (char, varchar, text)
			//As the spec/javadoc for this method indicate this is to be used for
			//large String values (i.e. LONGVARCHAR)  PG doesn't have a separate
			//long varchar datatype, but with toast all text datatypes are capable of
			//handling very large values.  Thus the implementation ends up calling
			//setString() since there is no current way to stream the value to the server
			try
			{
				InputStreamReader l_inStream = new InputStreamReader(x, "UTF-8");
				char[] l_chars = new char[length];
				int l_charsRead = l_inStream.read(l_chars, 0, length);
				setString(parameterIndex, new String(l_chars, 0, l_charsRead));
			}
			catch (UnsupportedEncodingException l_uee)
			{
				throw new PSQLException("postgresql.unusual", l_uee);
			}
			catch (IOException l_ioe)
			{
				throw new PSQLException("postgresql.unusual", l_ioe);
			}
		}
		else
		{
			//Version 7.1 supported only LargeObjects by treating everything
			//as binary data
			setBinaryStream(parameterIndex, x, length);
		}
	}

	/*
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
		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports BinaryStream for for the PG bytea type
			//As the spec/javadoc for this method indicate this is to be used for
			//large binary values (i.e. LONGVARBINARY)	PG doesn't have a separate
			//long binary datatype, but with toast the bytea datatype is capable of
			//handling very large values.  Thus the implementation ends up calling
			//setBytes() since there is no current way to stream the value to the server
			byte[] l_bytes = new byte[length];
			int l_bytesRead;
			try
			{
				l_bytesRead = x.read(l_bytes, 0, length);
			}
			catch (IOException l_ioe)
			{
				throw new PSQLException("postgresql.unusual", l_ioe);
			}
			if (l_bytesRead == length)
			{
				setBytes(parameterIndex, l_bytes);
			}
			else
			{
				//the stream contained less data than they said
				byte[] l_bytes2 = new byte[l_bytesRead];
				System.arraycopy(l_bytes, 0, l_bytes2, 0, l_bytesRead);
				setBytes(parameterIndex, l_bytes2);
			}
		}
		else
		{
			//Version 7.1 only supported streams for LargeObjects
			//but the jdbc spec indicates that streams should be
			//available for LONGVARBINARY instead
			LargeObjectManager lom = connection.getLargeObjectAPI();
			int oid = lom.create();
			LargeObject lob = lom.open(oid);
			OutputStream los = lob.getOutputStream();
			try
			{
				// could be buffered, but then the OutputStream returned by LargeObject
				// is buffered internally anyhow, so there would be no performance
				// boost gained, if anything it would be worse!
				int c = x.read();
				int p = 0;
				while (c > -1 && p < length)
				{
					los.write(c);
					c = x.read();
					p++;
				}
				los.close();
			}
			catch (IOException se)
			{
				throw new PSQLException("postgresql.unusual", se);
			}
			// lob is closed by the stream so don't call lob.close()
			setInt(parameterIndex, oid);
		}
	}


	/*
	 * In general, parameter values remain in force for repeated used of a
	 * Statement.  Setting a parameter value automatically clears its
	 * previous value.	However, in coms cases, it is useful to immediately
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

	/*
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
	 *		 *	types this is the number of digits after the decimal.  For
	 *		 *	all other types this value will be ignored.
	 * @exception SQLException if a database access error occurs
	 */
	public void setObject(int parameterIndex, Object x, int targetSqlType, int scale) throws SQLException
	{
		if (x == null)
		{
			setNull(parameterIndex, Types.OTHER);
			return;
		}
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
			case Types.BIT:
				if (x instanceof Boolean)
				{
					set(parameterIndex, ((Boolean)x).booleanValue() ? "TRUE" : "FALSE");
				}
				else
				{
					throw new PSQLException("postgresql.prep.type");
				}
				break;
			case Types.BINARY:
			case Types.VARBINARY:
				setObject(parameterIndex, x);
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

	/*
	 * This stores an Object into a parameter.
	 * <p>New for 6.4, if the object is not recognised, but it is
	 * Serializable, then the object is serialised using the
	 * org.postgresql.util.Serialize class.
	 */
	public void setObject(int parameterIndex, Object x) throws SQLException
	{
		if (x == null)
		{
			setNull(parameterIndex, Types.OTHER);
			return;
		}
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
			// Try to store java object in database
			setSerialize(parameterIndex, connection.storeObject(x), x.getClass().getName() );
	}

	/*
	 * Returns the SQL statement with the current template values
	 * substituted.
			* NB: This is identical to compileQuery() except instead of throwing
			* SQLException if a parameter is null, it places ? instead.
	 */
	public String toString()
	{
		synchronized (sbuf)
		{
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

	/*
	 * There are a lot of setXXX classes which all basically do
	 * the same thing.	We need a method which actually does the
	 * set for us.
	 *
	 * @param paramIndex the index into the inString
	 * @param s a string to be stored
	 * @exception SQLException if something goes wrong
	 */
	protected void set(int paramIndex, String s) throws SQLException
	{
		if (paramIndex < 1 || paramIndex > inStrings.length)
			throw new PSQLException("postgresql.prep.range");
		inStrings[paramIndex - 1] = s;
	}

	/*
	 * Helper - this compiles the SQL query from the various parameters
	 * This is identical to toString() except it throws an exception if a
	 * parameter is unused.
	 */
	protected synchronized String compileQuery()
	throws SQLException
	{
		sbuf.setLength(0);
		int i;

		for (i = 0 ; i < inStrings.length ; ++i)
		{
			if (inStrings[i] == null)
				throw new PSQLException("postgresql.prep.param", new Integer(i + 1));
			sbuf.append (templateStrings[i]).append (inStrings[i]);
		}
		sbuf.append(templateStrings[inStrings.length]);
		return sbuf.toString();
	}

	/*
	 * Set a parameter to a tablerow-type oid reference.
	 *
	 * @param parameterIndex the first parameter is 1...
	 * @param x the oid of the object from org.postgresql.util.Serialize.store
	 * @param classname the classname of the java object x
	 * @exception SQLException if a database access error occurs
	 */
	private void setSerialize(int parameterIndex, long x, String classname) throws SQLException
	{
		// converts . to _, toLowerCase, and ensures length<32
		String tablename = Serialize.toPostgreSQL( classname );
		DriverManager.println("setSerialize: setting " + x + "::" + tablename );

		// OID reference to tablerow-type must be cast like:  <oid>::<tablename>
		// Note that postgres support for tablerow data types is incomplete/broken.
		// This cannot be just a plain OID because then there would be ambiguity
		// between when you want the oid itself and when you want the object
		// an oid references.
		set(parameterIndex, Long.toString(x) + "::" + tablename );
	}


}
