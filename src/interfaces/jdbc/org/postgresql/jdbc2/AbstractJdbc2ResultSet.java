package org.postgresql.jdbc2;


import java.math.BigDecimal;
import java.io.*;
import java.sql.*;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.Vector;
import org.postgresql.Field;
import org.postgresql.core.Encoding;
import org.postgresql.largeobject.*;
import org.postgresql.util.PGbytea;
import org.postgresql.util.PSQLException;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/AbstractJdbc2ResultSet.java,v 1.1 2002/07/23 03:59:55 barry Exp $
 * This class defines methods of the jdbc2 specification.  This class extends
 * org.postgresql.jdbc1.AbstractJdbc1ResultSet which provides the jdbc1
 * methods.  The real Statement class (for jdbc2) is org.postgresql.jdbc2.Jdbc2ResultSet
 */
public class AbstractJdbc2ResultSet extends org.postgresql.jdbc1.AbstractJdbc1ResultSet
{
	protected Jdbc2Statement statement;

	protected String sqlQuery=null;

	public AbstractJdbc2ResultSet(org.postgresql.PGConnection conn, Field[] fields, Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor)
	{
		super(conn, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}

	public java.net.URL getURL(int columnIndex) throws SQLException
	{
		return null;
	}

	public java.net.URL getURL(String columnName) throws SQLException
	{
		return null;
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

		checkResultSet( columnIndex );

		wasNullFlag = (this_row[columnIndex - 1] == null);
		if (wasNullFlag)
			return null;

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
			case Types.ARRAY:
				return getArray(columnIndex);
			default:
				String type = field.getPGType();
				// if the backend doesn't know the type then coerce to String
				if (type.equals("unknown"))
				{
					return getString(columnIndex);
				}
				else
				{
					return connection.getObject(field.getPGType(), getString(columnIndex));
				}
		}
	}

	public boolean absolute(int index) throws SQLException
	{
		// index is 1-based, but internally we use 0-based indices
		int internalIndex;

		if (index == 0)
			throw new SQLException("Cannot move to index of 0");

		final int rows_size = rows.size();

		//if index<0, count from the end of the result set, but check
		//to be sure that it is not beyond the first index
		if (index < 0)
		{
			if (index >= -rows_size)
				internalIndex = rows_size + index;
			else
			{
				beforeFirst();
				return false;
			}
		}
		else
		{
			//must be the case that index>0,
			//find the correct place, assuming that
			//the index is not too large
			if (index <= rows_size)
				internalIndex = index - 1;
			else
			{
				afterLast();
				return false;
			}
		}

		current_row = internalIndex;
		this_row = (byte [][])rows.elementAt(internalIndex);
		return true;
	}

	public void afterLast() throws SQLException
	{
		final int rows_size = rows.size();
		if (rows_size > 0)
			current_row = rows_size;
	}

	public void beforeFirst() throws SQLException
	{
		if (rows.size() > 0)
			current_row = -1;
	}

	public void cancelRowUpdates() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void deleteRow() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public boolean first() throws SQLException
	{
		if (rows.size() <= 0)
			return false;

		current_row = 0;
		this_row = (byte [][])rows.elementAt(current_row);

		rowBuffer=new byte[this_row.length][];
		System.arraycopy(this_row,0,rowBuffer,0,this_row.length);

		return true;
	}

	public java.sql.Array getArray(String colName) throws SQLException
	{
		return getArray(findColumn(colName));
	}

	public java.sql.Array getArray(int i) throws SQLException
	{
		wasNullFlag = (this_row[i - 1] == null);
		if (wasNullFlag)
			return null;

		if (i < 1 || i > fields.length)
			throw new PSQLException("postgresql.res.colrange");
		return (java.sql.Array) new org.postgresql.jdbc2.Array( connection, i, fields[i - 1], (java.sql.ResultSet)this );
	}

	public java.math.BigDecimal getBigDecimal(int columnIndex) throws SQLException
	{
		return getBigDecimal(columnIndex, -1);
	}

	public java.math.BigDecimal getBigDecimal(String columnName) throws SQLException
	{
		return getBigDecimal(findColumn(columnName));
	}

	public Blob getBlob(String columnName) throws SQLException
	{
		return getBlob(findColumn(columnName));
	}

	public Blob getBlob(int i) throws SQLException
	{
		return new org.postgresql.largeobject.PGblob(connection, getInt(i));
	}

	public java.io.Reader getCharacterStream(String columnName) throws SQLException
	{
		return getCharacterStream(findColumn(columnName));
	}

	public java.io.Reader getCharacterStream(int i) throws SQLException
	{
		checkResultSet( i );
		wasNullFlag = (this_row[i - 1] == null);
		if (wasNullFlag)
			return null;

		if (((AbstractJdbc2Connection)connection).haveMinimumCompatibleVersion("7.2"))
		{
			//Version 7.2 supports AsciiStream for all the PG text types
			//As the spec/javadoc for this method indicate this is to be used for
			//large text values (i.e. LONGVARCHAR)	PG doesn't have a separate
			//long string datatype, but with toast the text datatype is capable of
			//handling very large values.  Thus the implementation ends up calling
			//getString() since there is no current way to stream the value from the server
			return new CharArrayReader(getString(i).toCharArray());
		}
		else
		{
			// In 7.1 Handle as BLOBS so return the LargeObject input stream
			Encoding encoding = connection.getEncoding();
			InputStream input = getBinaryStream(i);
			return encoding.getDecodingReader(input);
		}
	}

	public Clob getClob(String columnName) throws SQLException
	{
		return getClob(findColumn(columnName));
	}

	public Clob getClob(int i) throws SQLException
	{
		return new org.postgresql.largeobject.PGclob(connection, getInt(i));
	}

	public int getConcurrency() throws SQLException
	{
		// The standard ResultSet class will now return
		// CONCUR_READ_ONLY. A sub-class will overide this if the query was
		// updateable.
		return java.sql.ResultSet.CONCUR_READ_ONLY;
	}

	public java.sql.Date getDate(int i, java.util.Calendar cal) throws SQLException
	{
		// If I read the specs, this should use cal only if we don't
		// store the timezone, and if we do, then act just like getDate()?
		// for now...
		return getDate(i);
	}

	public Time getTime(int i, java.util.Calendar cal) throws SQLException
	{
		// If I read the specs, this should use cal only if we don't
		// store the timezone, and if we do, then act just like getTime()?
		// for now...
		return getTime(i);
	}

	public Timestamp getTimestamp(int i, java.util.Calendar cal) throws SQLException
	{
		// If I read the specs, this should use cal only if we don't
		// store the timezone, and if we do, then act just like getDate()?
		// for now...
		return getTimestamp(i);
	}

	public java.sql.Date getDate(String c, java.util.Calendar cal) throws SQLException
	{
		return getDate(findColumn(c), cal);
	}

	public Time getTime(String c, java.util.Calendar cal) throws SQLException
	{
		return getTime(findColumn(c), cal);
	}

	public Timestamp getTimestamp(String c, java.util.Calendar cal) throws SQLException
	{
		return getTimestamp(findColumn(c), cal);
	}

	public int getFetchDirection() throws SQLException
	{
		//PostgreSQL normally sends rows first->last
		return java.sql.ResultSet.FETCH_FORWARD;
	}

	public int getFetchSize() throws SQLException
	{
		// In this implementation we return the entire result set, so
		// here return the number of rows we have. Sub-classes can return a proper
		// value
		return rows.size();
	}

	public Object getObject(String columnName, java.util.Map map) throws SQLException
	{
		return getObject(findColumn(columnName), map);
	}

	/*
	 * This checks against map for the type of column i, and if found returns
	 * an object based on that mapping. The class must implement the SQLData
	 * interface.
	 */
	public Object getObject(int i, java.util.Map map) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public Ref getRef(String columnName) throws SQLException
	{
		return getRef(findColumn(columnName));
	}

	public Ref getRef(int i) throws SQLException
	{
		//The backend doesn't yet have SQL3 REF types
		throw new PSQLException("postgresql.psqlnotimp");
	}

	public int getRow() throws SQLException
	{
		final int rows_size = rows.size();

		if (current_row < 0 || current_row >= rows_size)
			return 0;

		return current_row + 1;
	}

	// This one needs some thought, as not all ResultSets come from a statement
	public java.sql.Statement getStatement() throws SQLException
	{
		return statement;
	}

	public int getType() throws SQLException
	{
		// This implementation allows scrolling but is not able to
		// see any changes. Sub-classes may overide this to return a more
		// meaningful result.
		return java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE;
	}

	public void insertRow() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public boolean isAfterLast() throws SQLException
	{
		final int rows_size = rows.size();
		return (current_row >= rows_size && rows_size > 0);
	}

	public boolean isBeforeFirst() throws SQLException
	{
		return (current_row < 0 && rows.size() > 0);
	}

	public boolean isFirst() throws SQLException
	{
		return (current_row == 0 && rows.size() >= 0);
	}

	public boolean isLast() throws SQLException
	{
		final int rows_size = rows.size();
		return (current_row == rows_size - 1 && rows_size > 0);
	}

	public boolean last() throws SQLException
	{
		final int rows_size = rows.size();
		if (rows_size <= 0)
			return false;

		current_row = rows_size - 1;
		this_row = (byte [][])rows.elementAt(current_row);

		rowBuffer=new byte[this_row.length][];
		System.arraycopy(this_row,0,rowBuffer,0,this_row.length);

		return true;
	}

	public void moveToCurrentRow() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void moveToInsertRow() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public boolean previous() throws SQLException
	{
		if (--current_row < 0)
			return false;
		this_row = (byte [][])rows.elementAt(current_row);
		System.arraycopy(this_row,0,rowBuffer,0,this_row.length);
		return true;
	}

	public void refreshRow() throws SQLException
	{
		throw new PSQLException("postgresql.notsensitive");
	}

	public boolean relative(int rows) throws SQLException
	{
		//have to add 1 since absolute expects a 1-based index
		return absolute(current_row + 1 + rows);
	}

	public boolean rowDeleted() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
		return false; // javac complains about not returning a value!
	}

	public boolean rowInserted() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
		return false; // javac complains about not returning a value!
	}

	public boolean rowUpdated() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
		return false; // javac complains about not returning a value!
	}

	public void setFetchDirection(int direction) throws SQLException
	{
		throw new PSQLException("postgresql.psqlnotimp");
	}

	public void setFetchSize(int rows) throws SQLException
	{
		// Sub-classes should implement this as part of their cursor support
		throw org.postgresql.Driver.notImplemented();
	}

	public void updateAsciiStream(int columnIndex,
								  java.io.InputStream x,
								  int length
								 ) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateAsciiStream(String columnName,
								  java.io.InputStream x,
								  int length
								 ) throws SQLException
	{
		updateAsciiStream(findColumn(columnName), x, length);
	}

	public void updateBigDecimal(int columnIndex,
								 java.math.BigDecimal x
								) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateBigDecimal(String columnName,
								 java.math.BigDecimal x
								) throws SQLException
	{
		updateBigDecimal(findColumn(columnName), x);
	}

	public void updateBinaryStream(int columnIndex,
								   java.io.InputStream x,
								   int length
								  ) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateBinaryStream(String columnName,
								   java.io.InputStream x,
								   int length
								  ) throws SQLException
	{
		updateBinaryStream(findColumn(columnName), x, length);
	}

	public void updateBoolean(int columnIndex, boolean x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateBoolean(String columnName, boolean x) throws SQLException
	{
		updateBoolean(findColumn(columnName), x);
	}

	public void updateByte(int columnIndex, byte x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateByte(String columnName, byte x) throws SQLException
	{
		updateByte(findColumn(columnName), x);
	}

	public void updateBytes(String columnName, byte[] x) throws SQLException
	{
		updateBytes(findColumn(columnName), x);
	}

	public void updateBytes(int columnIndex, byte[] x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateCharacterStream(int columnIndex,
									  java.io.Reader x,
									  int length
									 ) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateCharacterStream(String columnName,
									  java.io.Reader x,
									  int length
									 ) throws SQLException
	{
		updateCharacterStream(findColumn(columnName), x, length);
	}

	public void updateDate(int columnIndex, java.sql.Date x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateDate(String columnName, java.sql.Date x) throws SQLException
	{
		updateDate(findColumn(columnName), x);
	}

	public void updateDouble(int columnIndex, double x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateDouble(String columnName, double x) throws SQLException
	{
		updateDouble(findColumn(columnName), x);
	}

	public void updateFloat(int columnIndex, float x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateFloat(String columnName, float x) throws SQLException
	{
		updateFloat(findColumn(columnName), x);
	}

	public void updateInt(int columnIndex, int x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateInt(String columnName, int x) throws SQLException
	{
		updateInt(findColumn(columnName), x);
	}

    	public void updateLong(int columnIndex, long x) throws SQLException
    	{
    		// only sub-classes implement CONCUR_UPDATEABLE
    		notUpdateable();
    	}

    	public void updateLong(String columnName, long x) throws SQLException
    	{
    		updateLong(findColumn(columnName), x);
    	}

    	public void updateNull(int columnIndex) throws SQLException
    	{
    		// only sub-classes implement CONCUR_UPDATEABLE
    		notUpdateable();
    	}

	public void updateNull(String columnName) throws SQLException
	{
		updateNull(findColumn(columnName));
	}

	public void updateObject(int columnIndex, Object x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateObject(String columnName, Object x) throws SQLException
	{
		updateObject(findColumn(columnName), x);
	}

	public void updateObject(int columnIndex, Object x, int scale) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateObject(String columnName, Object x, int scale) throws SQLException
	{
		updateObject(findColumn(columnName), x, scale);
	}

	public void updateRow() throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateShort(int columnIndex, short x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateShort(String columnName, short x) throws SQLException
	{
		updateShort(findColumn(columnName), x);
	}

	public void updateString(int columnIndex, String x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateString(String columnName, String x) throws SQLException
	{
		updateString(findColumn(columnName), x);
	}

	public void updateTime(int columnIndex, Time x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateTime(String columnName, Time x) throws SQLException
	{
		updateTime(findColumn(columnName), x);
	}

	public void updateTimestamp(int columnIndex, Timestamp x) throws SQLException
	{
		// only sub-classes implement CONCUR_UPDATEABLE
		notUpdateable();
	}

	public void updateTimestamp(String columnName, Timestamp x) throws SQLException
	{
		updateTimestamp(findColumn(columnName), x);
	}

	// helper method. Throws an SQLException when an update is not possible
	public void notUpdateable() throws SQLException
	{
		throw new PSQLException("postgresql.noupdate");
	}

	/*
	 * It's used currently by getStatement() but may also with the new core
	 * package.
	 */
	public void setStatement(Jdbc2Statement statement)
	{
		this.statement = statement;
	}

	public void setSQLQuery(String sqlQuery) {
		this.sqlQuery=sqlQuery;
	}
}

