/*-------------------------------------------------------------------------
 *
 * AbstractJdbc1ResultSet.java
 *     This class defines methods of the jdbc1 specification.  This class is
 *     extended by org.postgresql.jdbc2.AbstractJdbc2ResultSet which adds the
 *     jdbc2 methods.  The real ResultSet class (for jdbc1) is 
 *     org.postgresql.jdbc1.Jdbc1ResultSet
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc1/Attic/AbstractJdbc1ResultSet.java,v 1.22.2.5 2005/01/13 14:22:23 jurka Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.jdbc1;

import java.math.BigDecimal;
import java.io.*;
import java.sql.*;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.Vector;
import org.postgresql.Driver;
import org.postgresql.core.BaseConnection;
import org.postgresql.core.BaseResultSet;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Field;
import org.postgresql.core.Encoding;
import org.postgresql.core.QueryExecutor;
import org.postgresql.largeobject.*;
import org.postgresql.util.PGbytea;
import org.postgresql.util.PGtokenizer;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;

public abstract class AbstractJdbc1ResultSet implements BaseResultSet
{

	protected Vector rows;			// The results
	protected BaseStatement statement;
	protected Field fields[];		// The field descriptions
	protected String status;		// Status of the result
	protected int updateCount;		// How many rows did we get back?
	protected long insertOID;		// The oid of an inserted row
	protected int current_row;		// Our pointer to where we are at
	protected byte[][] this_row;		// the current row result
	protected BaseConnection connection;	// the connection which we returned from
	protected SQLWarning warnings = null;	// The warning chain
	protected boolean wasNullFlag = false;	// the flag for wasNull()

	// We can chain multiple resultSets together - this points to
	// next resultSet in the chain.
	protected BaseResultSet next = null;

	private StringBuffer sbuf = null;
	public byte[][] rowBuffer = null;

 	private SimpleDateFormat m_tsFormat = null;
 	private SimpleDateFormat m_tstzFormat = null;
 	private SimpleDateFormat m_dateFormat = null;

	private int fetchSize;      // Fetch size for next read (might be 0).
	private int lastFetchSize;  // Fetch size of last read (might be 0).

	public abstract ResultSetMetaData getMetaData() throws SQLException;

	public AbstractJdbc1ResultSet(BaseStatement statement,
				      Field[] fields,
				      Vector tuples,
				      String status,
				      int updateCount,
				      long insertOID)
	{
		this.connection = statement.getPGConnection();
		this.statement = statement;
		this.fields = fields;
		this.rows = tuples;
		this.status = status;
		this.updateCount = updateCount;

		this.insertOID = insertOID;
		this.this_row = null;
		this.current_row = -1;

		this.lastFetchSize = this.fetchSize = (statement == null ? 0 : statement.getFetchSize());
	}

    public BaseStatement getPGStatement() {
		return statement;
	}

	public StringBuffer getStringBuffer() {
		return sbuf;
	}

	//This is implemented in jdbc2
	public void setStatement(BaseStatement statement) {
	}

	//method to reinitialize a result set with more data
	public void reInit (Field[] fields, Vector tuples, String status,
			  int updateCount, long insertOID)
	{
		this.fields = fields;
		// on a reinit the size of this indicates how many we pulled
		// back. If it's 0 then the res set has ended.
		this.rows = tuples;
		this.status = status;
		this.updateCount = updateCount;
		this.insertOID = insertOID;
		this.this_row = null;
		this.current_row = -1;
	}

	//
	// Part of the JDBC2 support, but convenient to implement here.
	//
  
	public void setFetchSize(int rows) throws SQLException
	{
		fetchSize = rows;
	}


	public int getFetchSize() throws SQLException
	{
		return fetchSize;
	}

	public boolean next() throws SQLException
	{
		if (rows == null)
			throw new PSQLException("postgresql.con.closed", PSQLState.CONNECTION_DOES_NOT_EXIST);

		if (current_row+1 >= rows.size())
		{
 			String cursorName = statement.getFetchingCursorName();
			if (cursorName == null || lastFetchSize == 0 || rows.size() < lastFetchSize) {
				current_row = rows.size();
				this_row = null;
				rowBuffer = null;
				return false;  // Not doing a cursor-based fetch or the last fetch was the end of the query
			}
 
  			// Use the ref to the statement to get
  			// the details we need to do another cursor
  			// query - it will use reinit() to repopulate this
  			// with the right data.
 
 			// NB: We can reach this point with fetchSize == 0 
 			// if the fetch size is changed halfway through reading results.
 			// Use "FETCH FORWARD ALL" in that case to complete the query.
 			String[] sql = new String[] {
 				fetchSize == 0 ? ("FETCH FORWARD ALL FROM " + cursorName) :
 				("FETCH FORWARD " + fetchSize + " FROM " + cursorName)
 			};
 
  			QueryExecutor.execute(sql,
 								  new String[0],
  								  this);
  
  			// Test the new rows array.
 			lastFetchSize = fetchSize;
  			if (rows.size() == 0) {
				this_row = null;
				rowBuffer = null;
  				return false;
			}

			// Otherwise reset the counter and let it go on...
			current_row = 0;
		} else {
			current_row++;
		}

		this_row = (byte [][])rows.elementAt(current_row);

		rowBuffer = new byte[this_row.length][];
		System.arraycopy(this_row, 0, rowBuffer, 0, this_row.length);
		return true;
	}

	public void close() throws SQLException
	{
		//release resources held (memory for tuples)
		if (rows != null)
		{
			rows = null;
		}
	}

	public boolean wasNull() throws SQLException
	{
		return wasNullFlag;
	}

	public String getString(int columnIndex) throws SQLException
	{
		checkResultSet( columnIndex );
		wasNullFlag = (this_row[columnIndex - 1] == null);
		if (wasNullFlag)
			return null;

		Encoding encoding = connection.getEncoding();
		return trimString(columnIndex, encoding.decode(this_row[columnIndex-1]));
	}

	public boolean getBoolean(int columnIndex) throws SQLException
	{
		return toBoolean( getString(columnIndex) );
	}


	public byte getByte(int columnIndex) throws SQLException
	{
		String s = getString(columnIndex);

		if (s != null)
		{
			try
			{
				switch(fields[columnIndex-1].getSQLType())
				{
					case Types.NUMERIC:
					case Types.REAL:
					case Types.DOUBLE:
					case Types.FLOAT:
					case Types.DECIMAL:
						int loc = s.indexOf(".");
						if (loc!=-1 && Integer.parseInt(s.substring(loc+1,s.length()))==0)
						{
							s = s.substring(0,loc);
						}
						break;
					case Types.CHAR:
						s = s.trim();
						break;
				}
				return Byte.parseByte(s);
			}
			catch (NumberFormatException e)
			{
				throw new PSQLException("postgresql.res.badbyte", PSQLState.NUMERIC_VALUE_OUT_OF_RANGE, s);
			}
		}
		return 0; // SQL NULL
	}

	public short getShort(int columnIndex) throws SQLException
	{
		String s = getFixedString(columnIndex);

		if (s != null)
		{
			try
			{
				switch(fields[columnIndex-1].getSQLType())
				{
					case Types.NUMERIC:
					case Types.REAL:
					case Types.DOUBLE:
					case Types.FLOAT:
					case Types.DECIMAL:
						int loc = s.indexOf(".");
						if (loc!=-1 && Integer.parseInt(s.substring(loc+1,s.length()))==0)
						{
							s = s.substring(0,loc);
						}
						break;
					case Types.CHAR:
						s = s.trim();
						break;
				}
				return Short.parseShort(s);
			}
			catch (NumberFormatException e)
			{
				throw new PSQLException("postgresql.res.badshort", PSQLState.NUMERIC_VALUE_OUT_OF_RANGE, s);
			}
		}
		return 0; // SQL NULL
	}

	public int getInt(int columnIndex) throws SQLException
	{
		return toInt( getFixedString(columnIndex) );
	}

	public long getLong(int columnIndex) throws SQLException
	{
		return toLong( getFixedString(columnIndex) );
	}

	public float getFloat(int columnIndex) throws SQLException
	{
		return toFloat( getFixedString(columnIndex) );
	}

	public double getDouble(int columnIndex) throws SQLException
	{
		return toDouble( getFixedString(columnIndex) );
	}

	public BigDecimal getBigDecimal(int columnIndex, int scale) throws SQLException
	{
		return toBigDecimal( getFixedString(columnIndex), scale );
	}

	/*
	 * Get the value of a column in the current row as a Java byte array.
	 *
	 * <p>In normal use, the bytes represent the raw values returned by the
	 * backend. However, if the column is an OID, then it is assumed to
	 * refer to a Large Object, and that object is returned as a byte array.
	 *
	 * <p><b>Be warned</b> If the large object is huge, then you may run out
	 * of memory.
	 *
	 * @param columnIndex the first column is 1, the second is 2, ...
	 * @return the column value; if the value is SQL NULL, the result
	 *	is null
	 * @exception SQLException if a database access error occurs
	 */
	public byte[] getBytes(int columnIndex) throws SQLException
	{
		checkResultSet( columnIndex );
		wasNullFlag = (this_row[columnIndex - 1] == null);
		if (!wasNullFlag)
		{
			if (fields[columnIndex -1].getFormat() == Field.BINARY_FORMAT)
			{
				//If the data is already binary then just return it
				return this_row[columnIndex - 1];
			}
			else if (connection.haveMinimumCompatibleVersion("7.2"))
			{
				//Version 7.2 supports the bytea datatype for byte arrays
				if (fields[columnIndex - 1].getPGType().equals("bytea"))
				{
					return trimBytes(columnIndex, PGbytea.toBytes(this_row[columnIndex - 1]));
				}
				else
				{
					return trimBytes(columnIndex, this_row[columnIndex - 1]);
				}
			}
			else
			{
				//Version 7.1 and earlier supports LargeObjects for byte arrays
				// Handle OID's as BLOBS
				if ( fields[columnIndex - 1].getOID() == 26)
				{
					LargeObjectManager lom = connection.getLargeObjectAPI();
					LargeObject lob = lom.open(getInt(columnIndex));
					byte buf[] = lob.read(lob.size());
					lob.close();
					return trimBytes(columnIndex, buf);
				}
				else
				{
					return trimBytes(columnIndex, this_row[columnIndex - 1]);
				}
			}
		}
		return null;
	}

	public java.sql.Date getDate(int columnIndex) throws SQLException
	{
		return toDate( getString(columnIndex) );
	}

	public Time getTime(int columnIndex) throws SQLException
	{
		return toTime( getString(columnIndex), this, fields[columnIndex - 1].getPGType() );
	}

	public Timestamp getTimestamp(int columnIndex) throws SQLException
	{
		return toTimestamp( getString(columnIndex), this, fields[columnIndex - 1].getPGType() );
	}

	public InputStream getAsciiStream(int columnIndex) throws SQLException
	{
		checkResultSet( columnIndex );
		wasNullFlag = (this_row[columnIndex - 1] == null);
		if (wasNullFlag)
			return null;

		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports AsciiStream for all the PG text types
			//As the spec/javadoc for this method indicate this is to be used for
			//large text values (i.e. LONGVARCHAR)	PG doesn't have a separate
			//long string datatype, but with toast the text datatype is capable of
			//handling very large values.  Thus the implementation ends up calling
			//getString() since there is no current way to stream the value from the server
			try
			{
				return new ByteArrayInputStream(getString(columnIndex).getBytes("ASCII"));
			}
			catch (UnsupportedEncodingException l_uee)
			{
				throw new PSQLException("postgresql.unusual", PSQLState.UNEXPECTED_ERROR, l_uee);
			}
		}
		else
		{
			// In 7.1 Handle as BLOBS so return the LargeObject input stream
			return getBinaryStream(columnIndex);
		}
	}

	public InputStream getUnicodeStream(int columnIndex) throws SQLException
	{
		checkResultSet( columnIndex );
		wasNullFlag = (this_row[columnIndex - 1] == null);
		if (wasNullFlag)
			return null;

		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports AsciiStream for all the PG text types
			//As the spec/javadoc for this method indicate this is to be used for
			//large text values (i.e. LONGVARCHAR)	PG doesn't have a separate
			//long string datatype, but with toast the text datatype is capable of
			//handling very large values.  Thus the implementation ends up calling
			//getString() since there is no current way to stream the value from the server
			try
			{
				return new ByteArrayInputStream(getString(columnIndex).getBytes("UTF-8"));
			}
			catch (UnsupportedEncodingException l_uee)
			{
				throw new PSQLException("postgresql.unusual", PSQLState.UNEXPECTED_ERROR, l_uee);
			}
		}
		else
		{
			// In 7.1 Handle as BLOBS so return the LargeObject input stream
			return getBinaryStream(columnIndex);
		}
	}

	public InputStream getBinaryStream(int columnIndex) throws SQLException
	{
		checkResultSet( columnIndex );
		wasNullFlag = (this_row[columnIndex - 1] == null);
		if (wasNullFlag)
			return null;

		if (connection.haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports BinaryStream for all PG bytea type
			//As the spec/javadoc for this method indicate this is to be used for
			//large binary values (i.e. LONGVARBINARY)	PG doesn't have a separate
			//long binary datatype, but with toast the bytea datatype is capable of
			//handling very large values.  Thus the implementation ends up calling
			//getBytes() since there is no current way to stream the value from the server
			byte b[] = getBytes(columnIndex);
			if (b != null)
				return new ByteArrayInputStream(b);
		}
		else
		{
			// In 7.1 Handle as BLOBS so return the LargeObject input stream
			if ( fields[columnIndex - 1].getOID() == 26)
			{
				LargeObjectManager lom = connection.getLargeObjectAPI();
				LargeObject lob = lom.open(getInt(columnIndex));
				return lob.getInputStream();
			}
		}
		return null;
	}

	public String getString(String columnName) throws SQLException
	{
		return getString(findColumn(columnName));
	}

	public boolean getBoolean(String columnName) throws SQLException
	{
		return getBoolean(findColumn(columnName));
	}

	public byte getByte(String columnName) throws SQLException
	{

		return getByte(findColumn(columnName));
	}

	public short getShort(String columnName) throws SQLException
	{
		return getShort(findColumn(columnName));
	}

	public int getInt(String columnName) throws SQLException
	{
		return getInt(findColumn(columnName));
	}

	public long getLong(String columnName) throws SQLException
	{
		return getLong(findColumn(columnName));
	}

	public float getFloat(String columnName) throws SQLException
	{
		return getFloat(findColumn(columnName));
	}

	public double getDouble(String columnName) throws SQLException
	{
		return getDouble(findColumn(columnName));
	}

	public BigDecimal getBigDecimal(String columnName, int scale) throws SQLException
	{
		return getBigDecimal(findColumn(columnName), scale);
	}

	public byte[] getBytes(String columnName) throws SQLException
	{
		return getBytes(findColumn(columnName));
	}

	public java.sql.Date getDate(String columnName) throws SQLException
	{
		return getDate(findColumn(columnName));
	}

	public Time getTime(String columnName) throws SQLException
	{
		return getTime(findColumn(columnName));
	}

	public Timestamp getTimestamp(String columnName) throws SQLException
	{
		return getTimestamp(findColumn(columnName));
	}

	public InputStream getAsciiStream(String columnName) throws SQLException
	{
		return getAsciiStream(findColumn(columnName));
	}

	public InputStream getUnicodeStream(String columnName) throws SQLException
	{
		return getUnicodeStream(findColumn(columnName));
	}

	public InputStream getBinaryStream(String columnName) throws SQLException
	{
		return getBinaryStream(findColumn(columnName));
	}

	public SQLWarning getWarnings() throws SQLException
	{
		return warnings;
	}

	public void clearWarnings() throws SQLException
	{
		warnings = null;
	}

	public void addWarnings(SQLWarning warnings)
	{
		if ( this.warnings != null )
			this.warnings.setNextWarning(warnings);
		else
			this.warnings = warnings;
	}

	public String getCursorName() throws SQLException
	{
		return (connection.getCursorName());
	}

	/*
	 * Get the value of a column in the current row as a Java object
	 *
	 * <p>This method will return the value of the given column as a
	 * Java object.  The type of the Java object will be the default
	 * Java Object type corresponding to the column's SQL type, following
	 * the mapping specified in the JDBC specification.
	 *
	 * <p>This method may also be used to read database specific abstract
	 * data types.
	 *
	 * @param columnIndex the first column is 1, the second is 2...
	 * @return a Object holding the column value
	 * @exception SQLException if a database access error occurs
	 */
	public Object getObject(int columnIndex) throws SQLException
	{
		Field field;

		if (columnIndex < 1 || columnIndex > fields.length)
			throw new PSQLException("postgresql.res.colrange", PSQLState.INVALID_PARAMETER_VALUE);
		field = fields[columnIndex - 1];

		// some fields can be null, mainly from those returned by MetaData methods
		if (field == null)
		{
			wasNullFlag = true;
			return null;
		}

		switch (field.getSQLType())
		{
			case Types.BIT:
				return getBoolean(columnIndex) ? Boolean.TRUE : Boolean.FALSE;
			case Types.SMALLINT:
				return new Short(getShort(columnIndex));
			case Types.INTEGER:
				return new Integer(getInt(columnIndex));
			case Types.BIGINT:
				return new Long(getLong(columnIndex));
			case Types.NUMERIC:
				return getBigDecimal
					   (columnIndex, (field.getMod() == -1) ? -1 : ((field.getMod() - 4) & 0xffff));
			case Types.REAL:
				return new Float(getFloat(columnIndex));
			case Types.DOUBLE:
				return new Double(getDouble(columnIndex));
			case Types.CHAR:
			case Types.VARCHAR:
				return getString(columnIndex);
			case Types.DATE:
				return getDate(columnIndex);
			case Types.TIME:
				return getTime(columnIndex);
			case Types.TIMESTAMP:
				return getTimestamp(columnIndex);
			case Types.BINARY:
			case Types.VARBINARY:
				return getBytes(columnIndex);
			default:
				String type = field.getPGType();

				// if the backend doesn't know the type then coerce to String
				if (type.equals("unknown"))
				{
					return getString(columnIndex);
				}
                                // Specialized support for ref cursors is neater.
                                else if (type.equals("refcursor"))
                                {
                                        String cursorName = getString(columnIndex);
                                        return statement.createRefCursorResultSet(cursorName);
                                }
				else
				{
					return connection.getObject(field.getPGType(), getString(columnIndex));
				}
		}
	}

	public Object getObject(String columnName) throws SQLException
	{
		return getObject(findColumn(columnName));
	}

	/*
	 * Map a ResultSet column name to a ResultSet column index
	 */
	public int findColumn(String columnName) throws SQLException
	{
		int i;

		final int flen = fields.length;
		for (i = 0 ; i < flen; ++i)
			if (fields[i].getName().equalsIgnoreCase(columnName))
				return (i + 1);
		throw new PSQLException ("postgresql.res.colname", columnName);
	}


	/*
	 * We at times need to know if the resultSet we are working
	 * with is the result of an UPDATE, DELETE or INSERT (in which
	 * case, we only have a row count), or of a SELECT operation
	 * (in which case, we have multiple fields) - this routine
	 * tells us.
	 */
	public boolean reallyResultSet()
	{
		return (fields != null);
	}

	/*
	 * Since ResultSets can be chained, we need some method of
	 * finding the next one in the chain.  The method getNext()
	 * returns the next one in the chain.
	 *
	 * @return the next ResultSet, or null if there are none
	 */
	public ResultSet getNext()
	{
		return (ResultSet)next;
	}

	/*
	 * This following method allows us to add a ResultSet object
	 * to the end of the current chain.
	 */
	public void append(BaseResultSet r)
	{
		if (next == null)
			next = r;
		else
			next.append(r);
	}

	/*
	 * If we are just a place holder for results, we still need
	 * to get an updateCount.  This method returns it.
	 */
	public int getResultCount()
	{
		return updateCount;
	}

	/*
	 * We also need to provide a couple of auxiliary functions for
	 * the implementation of the ResultMetaData functions.	In
	 * particular, we need to know the number of rows and the
	 * number of columns.  Rows are also known as Tuples
	 */
	public int getTupleCount()
	{
		return rows.size();
	}

	/*
	 * getColumnCount returns the number of columns
	 */
	public int getColumnCount()
	{
		return fields.length;
	}

	/*
	 * Returns the status message from the backend.<p>
	 * It is used internally by the driver.
	 */
	public String getStatusString()
	{
		return status;
	}

	/*
	 * returns the OID of a field.<p>
	 * It is used internally by the driver.
	 */
	public int getColumnOID(int field)
	{
		return fields[field -1].getOID();
	}

	/*
	 * returns the OID of the last inserted row.  Deprecated in 7.2 because
			* range for OID values is greater than java signed int.
	 * @deprecated Replaced by getLastOID() in 7.2
	 */
	public int getInsertedOID()
	{
		return (int) getLastOID();
	}


	/*
	 * returns the OID of the last inserted row
			* @since 7.2
	 */
	public long getLastOID()
	{
		return insertOID;
	}

	/*
	 * This is used to fix get*() methods on Money fields. It should only be
	 * used by those methods!
	 *
	 * It converts ($##.##) to -##.## and $##.## to ##.##
	 */
	public String getFixedString(int col) throws SQLException
	{
		String s = getString(col);

		// Handle SQL Null
		wasNullFlag = (this_row[col - 1] == null);
		if (wasNullFlag)
			return null;

		// if we don't have at least 2 characters it can't be money.
 		if (s.length() < 2)
 			return s;

		// Handle Money
		if (s.charAt(0) == '(')
		{
			s = "-" + PGtokenizer.removePara(s).substring(1);
		}
		if (s.charAt(0) == '$')
		{
			s = s.substring(1);
		}
 		else if (s.charAt(0) == '-' && s.charAt(1) == '$')
 		{
 			s = "-" + s.substring(2);
		}

		return s;
	}

	protected void checkResultSet( int column ) throws SQLException
	{
		if ( this_row == null )
			throw new PSQLException("postgresql.res.nextrequired");
		if ( column < 1 || column > fields.length )
			throw new PSQLException("postgresql.res.colrange", PSQLState.INVALID_PARAMETER_VALUE );
	}

	//----------------- Formatting Methods -------------------

	public static boolean toBoolean(String s)
	{
		if (s != null)
		{
			s = s.trim();

			if (s.equalsIgnoreCase("true") || s.equalsIgnoreCase("t"))
				return true;

			try
			{
				if (Double.valueOf(s).doubleValue()==1)
					return true;
			}
			catch (NumberFormatException e)
			{
			}
		}
		return false;		// SQL NULL
	}

	public static int toInt(String s) throws SQLException
	{
		if (s != null)
		{
			try
			{
				s = s.trim();
				return Integer.parseInt(s);
			}
			catch (NumberFormatException e)
			{
				throw new PSQLException ("postgresql.res.badint", PSQLState.NUMERIC_VALUE_OUT_OF_RANGE, s);
			}
		}
		return 0;		// SQL NULL
	}

	public static long toLong(String s) throws SQLException
	{
		if (s != null)
		{
			try
			{
				s = s.trim();		
				return Long.parseLong(s);
			}
			catch (NumberFormatException e)
			{
				throw new PSQLException ("postgresql.res.badlong", PSQLState.NUMERIC_VALUE_OUT_OF_RANGE, s);
			}
		}
		return 0;		// SQL NULL
	}

	public static BigDecimal toBigDecimal(String s, int scale) throws SQLException
	{
		BigDecimal val;
		if (s != null)
		{
			try
			{
				s = s.trim();
				val = new BigDecimal(s);
			}
			catch (NumberFormatException e)
			{
				throw new PSQLException ("postgresql.res.badbigdec", PSQLState.NUMERIC_VALUE_OUT_OF_RANGE, s);
			}
			if (scale == -1)
				return val;
			try
			{
				return val.setScale(scale);
			}
			catch (ArithmeticException e)
			{
				throw new PSQLException ("postgresql.res.badbigdec", PSQLState.NUMERIC_VALUE_OUT_OF_RANGE, s);
			}
		}
		return null;		// SQL NULL
	}

	public static float toFloat(String s) throws SQLException
	{
		if (s != null)
		{
			try
			{
				s = s.trim();
				return Float.valueOf(s).floatValue();
			}
			catch (NumberFormatException e)
			{
				throw new PSQLException ("postgresql.res.badfloat", PSQLState.NUMERIC_VALUE_OUT_OF_RANGE, s);
			}
		}
		return 0;		// SQL NULL
	}

	public static double toDouble(String s) throws SQLException
	{
		if (s != null)
		{
			try
			{
				s = s.trim();
				return Double.valueOf(s).doubleValue();
			}
			catch (NumberFormatException e)
			{
				throw new PSQLException ("postgresql.res.baddouble", PSQLState.NUMERIC_VALUE_OUT_OF_RANGE, s);
			}
		}
		return 0;		// SQL NULL
	}

	public static java.sql.Date toDate(String s) throws SQLException
	{
		if (s == null)
			return null;
		// length == 10: SQL Date
		// length >  10: SQL Timestamp, assumes PGDATESTYLE=ISO
		try
		{
			s = s.trim();
			return java.sql.Date.valueOf((s.length() == 10) ? s : s.substring(0, 10));
		}
		catch (NumberFormatException e)
		{
			throw new PSQLException("postgresql.res.baddate",PSQLState.BAD_DATETIME_FORMAT, s);
		}
	}

	public static Time toTime(String s, BaseResultSet resultSet, String pgDataType) throws SQLException
	{
		if (s == null)
			return null; // SQL NULL
		try
		{
			s = s.trim();
			if (s.length() == 8)
			{
				//value is a time value
				return java.sql.Time.valueOf(s);
			}
			else if (s.indexOf(".") == 8)
			{
				//value is a time value with fractional seconds
				java.sql.Time l_time = java.sql.Time.valueOf(s.substring(0, 8));
				String l_strMillis = s.substring(9);
				if (l_strMillis.length() > 3)
					l_strMillis = l_strMillis.substring(0, 3);
				int l_millis = Integer.parseInt(l_strMillis);
				if (l_millis < 10)
				{
					l_millis = l_millis * 100;
				}
				else if (l_millis < 100)
				{
					l_millis = l_millis * 10;
				}
				return new java.sql.Time(l_time.getTime() + l_millis);
			}
			else
			{
				//value is a timestamp
				return new java.sql.Time(toTimestamp(s, resultSet, pgDataType).getTime());
			}
		}
		catch (NumberFormatException e)
		{
			throw new PSQLException("postgresql.res.badtime", PSQLState.BAD_DATETIME_FORMAT, s);
		}
	}

	/**
	* Parse a string and return a timestamp representing its value.
	*
	* The driver is set to return ISO date formated strings. We modify this
	* string from the ISO format to a format that Java can understand. Java
	* expects timezone info as 'GMT+09:00' where as ISO gives '+09'.
	* Java also expects fractional seconds to 3 places where postgres
	* will give, none, 2 or 6 depending on the time and postgres version.
	* From version 7.2 postgres returns fractional seconds to 6 places.
	*
	* According to the Timestamp documentation, fractional digits are kept
	* in the nanos field of Timestamp and not in the milliseconds of Date.
	* Thus, parsing for fractional digits is entirely separated from the
	* rest.
	*
	* The method assumes that there are no more than 9 fractional
	* digits given. Undefined behavior if this is not the case.
	*
	* @param s		   The ISO formated date string to parse.
	* @param resultSet The ResultSet this date is part of.
	*
	* @return null if s is null or a timestamp of the parsed string s.
	*
	* @throws SQLException if there is a problem parsing s.
	**/
	public static Timestamp toTimestamp(String s, BaseResultSet resultSet, String pgDataType)
	throws SQLException
	{
		BaseResultSet rs = resultSet;
		if (s == null)
			return null;

		s = s.trim();
		// We must be synchronized here incase more theads access the ResultSet
		// bad practice but possible. Anyhow this is to protect sbuf and
		// SimpleDateFormat objects
		synchronized (rs)
		{
			StringBuffer l_sbuf = rs.getStringBuffer();
			SimpleDateFormat df = null;
			if ( Driver.logDebug )
				Driver.debug("the data from the DB is " + s);

			// If first time, create the buffer, otherwise clear it.
			if (l_sbuf == null)
				l_sbuf = new StringBuffer(32);
			else
			{
				l_sbuf.setLength(0);
			}

			// Copy s into sbuf for parsing.
			l_sbuf.append(s);
			int slen = s.length();

			// For a Timestamp, the fractional seconds are stored in the
			// nanos field. As a DateFormat is used for parsing which can
			// only parse to millisecond precision and which returns a
			// Date object, the fractional second parsing is completely
			// separate.
			int nanos = 0;

			if (slen > 19)
			{
				// The len of the ISO string to the second value is 19 chars. If
				// greater then 19, there may be tz info and perhaps fractional
				// second info which we need to change to java to read it.

				// cut the copy to second value "2001-12-07 16:29:22"
				int i = 19;
				l_sbuf.setLength(i);

				char c = s.charAt(i++);
				if (c == '.')
				{
					// Found a fractional value.
					final int start = i;
					while (true)
					{
						c = s.charAt(i++);
						if (!Character.isDigit(c))
							break;
						if (i == slen)
							{
								i++;
								break;
							}
					}

					// The range [start, i - 1) contains all fractional digits.
					final int end = i - 1;
					try
						{
							nanos = Integer.parseInt(s.substring(start, end));
						}
					catch (NumberFormatException e)
						{
							throw new PSQLException("postgresql.unusual", PSQLState.UNEXPECTED_ERROR, e);
						}

					// The nanos field stores nanoseconds. Adjust the parsed
					// value to the correct magnitude.
					for (int digitsToNano = 9 - (end - start);
						 digitsToNano > 0; --digitsToNano)
						nanos *= 10;
				}

				if (i < slen)
				{
					// prepend the GMT part and then add the remaining bit of
					// the string.
					l_sbuf.append(" GMT");
					l_sbuf.append(c);
					l_sbuf.append(s.substring(i, slen));

					// Lastly, if the tz part doesn't specify the :MM part then
					// we add ":00" for java.
					if (slen - i < 5)
						l_sbuf.append(":00");

					// we'll use this dateformat string to parse the result.
					df = rs.getTimestampTZFormat();
				}
				else
				{
					// Just found fractional seconds but no timezone.
					//If timestamptz then we use GMT, else local timezone
					if (pgDataType.equals("timestamptz"))
					{
						l_sbuf.append(" GMT");
						df = rs.getTimestampTZFormat();
					}
					else
					{
						df = rs.getTimestampFormat();
					}
				}
			}
			else if (slen == 19)
			{
				// No tz or fractional second info.
				//If timestamptz then we use GMT, else local timezone
				if (pgDataType.equals("timestamptz"))
				{
					l_sbuf.append(" GMT");
					df = rs.getTimestampTZFormat();
				}
				else
				{
					df = rs.getTimestampFormat();
				}
			}
			else
			{
				if (slen == 8 && s.equals("infinity")) {
					//java doesn't have a concept of postgres's infinity
					//so set to an arbitrary future date
					l_sbuf.setLength(0);
					l_sbuf.append("9999-01-01");
				}
				else if (slen == 9 && s.equals("-infinity")) {
					//java doesn't have a concept of postgres's infinity
					//so set to an arbitrary old date
					l_sbuf.setLength(0);
					l_sbuf.append("0001-01-01");
				}

				// We must just have a date. This case is
				// needed if this method is called on a date
				// column
				df = rs.getDateFormat();
			}

			try
			{
				// All that's left is to parse the string and return the ts.
				if ( Driver.logDebug )
					Driver.debug("the data after parsing is " 
                     + l_sbuf.toString() + " with " + nanos + " nanos");

 				Timestamp result = new Timestamp(df.parse(l_sbuf.toString()).getTime());
				result.setNanos(nanos);
				return result;
			}
			catch (ParseException e)
			{
				throw new PSQLException("postgresql.res.badtimestamp", PSQLState.BAD_DATETIME_FORMAT, new Integer(e.getErrorOffset()), s);
			}
		}
	}
	
	private boolean isColumnTrimmable(int columnIndex) throws SQLException
	{
		switch (fields[columnIndex-1].getSQLType())
		{
			case Types.CHAR:
			case Types.VARCHAR:
		  	case Types.LONGVARCHAR:
		   	case Types.BINARY:
		   	case Types.VARBINARY:
		   	case Types.LONGVARBINARY:
		   		return true;
		}
	   	return false;
	}

	private byte[] trimBytes(int p_columnIndex, byte[] p_bytes) throws SQLException
	{
		int l_maxSize = statement.getMaxFieldSize();
		//we need to trim if maxsize is set and the length is greater than maxsize and the
		//type of this column is a candidate for trimming
		if (l_maxSize > 0 && p_bytes.length > l_maxSize && isColumnTrimmable(p_columnIndex))
		{
			byte[] l_bytes = new byte[l_maxSize];
			System.arraycopy (p_bytes, 0, l_bytes, 0, l_maxSize);
			return l_bytes;
		}
		else
		{
			return p_bytes;
		}
	}

	private String trimString(int p_columnIndex, String p_string) throws SQLException
	{
		int l_maxSize = statement.getMaxFieldSize();
		//we need to trim if maxsize is set and the length is greater than maxsize and the
		//type of this column is a candidate for trimming
		if (l_maxSize > 0 && p_string.length() > l_maxSize && isColumnTrimmable(p_columnIndex))
		{
			return p_string.substring(0,l_maxSize);
		} 
		else 
		{
			return p_string;
		}
	}
	
	public SimpleDateFormat getTimestampTZFormat() {
		if (m_tstzFormat == null) {
			m_tstzFormat = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss z");
		}
		return m_tstzFormat;
	}
	public SimpleDateFormat getTimestampFormat() {
		if (m_tsFormat == null) {
			m_tsFormat = new SimpleDateFormat ("yyyy-MM-dd HH:mm:ss");
		}
		return m_tsFormat;
	}
	public SimpleDateFormat getDateFormat() {
		if (m_dateFormat == null) {
			m_dateFormat = new SimpleDateFormat("yyyy-MM-dd");
		}
		return m_dateFormat;
	}
}

