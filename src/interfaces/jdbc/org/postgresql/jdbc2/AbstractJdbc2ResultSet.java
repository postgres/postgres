/*-------------------------------------------------------------------------
 *
 * AbstractJdbc2ResultSet.java
 *     This class defines methods of the jdbc2 specification.  This class 
 *     extends org.postgresql.jdbc1.AbstractJdbc1ResultSet which provides the 
 *     jdbc1 methods.  The real Statement class (for jdbc2) is 
 *     org.postgresql.jdbc2.Jdbc2ResultSet
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/jdbc2/Attic/AbstractJdbc2ResultSet.java,v 1.25.2.12 2005/04/22 14:48:18 jurka Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.jdbc2;

import java.io.CharArrayReader;
import java.io.InputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.UnsupportedEncodingException;
import java.math.BigDecimal;
import java.sql.*;
import java.util.Enumeration;
import java.util.Hashtable;
import java.util.Iterator;
import java.util.StringTokenizer;
import java.util.Vector;
import org.postgresql.Driver;
import org.postgresql.core.BaseStatement;
import org.postgresql.core.Field;
import org.postgresql.core.Encoding;
import org.postgresql.util.PSQLException;
import org.postgresql.util.PSQLState;


public abstract class AbstractJdbc2ResultSet extends org.postgresql.jdbc1.AbstractJdbc1ResultSet
{

	//needed for updateable result set support
	protected boolean updateable = false;
	protected boolean doingUpdates = false;
	protected boolean onInsertRow = false;
	protected Hashtable updateValues = new Hashtable();
	private boolean usingOID = false;	// are we using the OID for the primary key?
	private Vector primaryKeys;    // list of primary keys
	private int numKeys = 0;
	private boolean singleTable = false;
	protected String tableName = null;
	protected PreparedStatement updateStatement = null;
	protected PreparedStatement insertStatement = null;
	protected PreparedStatement deleteStatement = null;
	private PreparedStatement selectStatement = null;

  
	public AbstractJdbc2ResultSet(BaseStatement statement, Field[] fields, Vector tuples, String status, int updateCount, long insertOID)
	{
		super (statement, fields, tuples, status, updateCount, insertOID);
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


	public boolean absolute(int index) throws SQLException
	{
		// index is 1-based, but internally we use 0-based indices
		int internalIndex;

		if (index == 0) {
			beforeFirst();
			return false;
		}

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
		this_row = (byte[][]) rows.elementAt(internalIndex);

		rowBuffer = new byte[this_row.length][];
		System.arraycopy(this_row, 0, rowBuffer, 0, this_row.length);
		onInsertRow = false;

		return true;
	}


	public void afterLast() throws SQLException
	{
		final int rows_size = rows.size();
		if (rows_size > 0)
			current_row = rows_size;

		onInsertRow = false;
		this_row = null;
		rowBuffer = null;
	}


	public void beforeFirst() throws SQLException
	{
		if (rows.size() > 0)
			current_row = -1;

		onInsertRow = false;
		this_row = null;
		rowBuffer = null;
	}


	public boolean first() throws SQLException
	{
		if (rows.size() <= 0)
			return false;

		current_row = 0;
		this_row = (byte[][]) rows.elementAt(current_row);

		rowBuffer = new byte[this_row.length][];
		System.arraycopy(this_row, 0, rowBuffer, 0, this_row.length);
		onInsertRow = false;

		return true;
	}


	public java.sql.Array getArray(String colName) throws SQLException
	{
		return getArray(findColumn(colName));
	}


	public java.sql.Array getArray(int i) throws SQLException
	{
		checkResultSet( i );

		wasNullFlag = (this_row[i - 1] == null);
		if (wasNullFlag)
			return null;

		if (i < 1 || i > fields.length)
			throw new PSQLException("postgresql.res.colrange", PSQLState.INVALID_PARAMETER_VALUE);
		return (java.sql.Array) new org.postgresql.jdbc2.Array( connection, i, fields[i - 1], this );
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


	public abstract Blob getBlob(int i) throws SQLException;


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

		if (((AbstractJdbc2Connection) connection).haveMinimumCompatibleVersion("7.2"))
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


	public abstract Clob getClob(int i) throws SQLException;


	public int getConcurrency() throws SQLException
	{
		if (statement == null)
			return java.sql.ResultSet.CONCUR_READ_ONLY;
		return statement.getResultSetConcurrency();
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
		throw new PSQLException("postgresql.psqlnotimp", PSQLState.NOT_IMPLEMENTED);
	}


	public int getRow() throws SQLException
	{
		final int rows_size = rows.size();

		if (current_row < 0 || current_row >= rows_size)
			return 0;

		return current_row + 1;
	}


	// This one needs some thought, as not all ResultSets come from a statement
	public Statement getStatement() throws SQLException
	{
		return (Statement) statement;
	}


	public int getType() throws SQLException
	{
		// This implementation allows scrolling but is not able to
		// see any changes. Sub-classes may overide this to return a more
		// meaningful result.
		return java.sql.ResultSet.TYPE_SCROLL_INSENSITIVE;
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
		this_row = (byte[][]) rows.elementAt(current_row);

		rowBuffer = new byte[this_row.length][];
		System.arraycopy(this_row, 0, rowBuffer, 0, this_row.length);
		onInsertRow = false;

		return true;
	}


	public boolean previous() throws SQLException
	{
		if (current_row-1 < 0) {
			current_row = -1;
			this_row = null;
			rowBuffer = null;
			return false;
		} else {
			current_row--;
		}
		this_row = (byte[][]) rows.elementAt(current_row);
		rowBuffer = new byte[this_row.length][];
		System.arraycopy(this_row, 0, rowBuffer, 0, this_row.length);
		return true;
	}


	public boolean relative(int rows) throws SQLException
	{
		//have to add 1 since absolute expects a 1-based index
		return absolute(current_row + 1 + rows);
	}


	public void setFetchDirection(int direction) throws SQLException
	{
		throw new PSQLException("postgresql.psqlnotimp", PSQLState.NOT_IMPLEMENTED);
	}


	public synchronized void cancelRowUpdates()
	throws SQLException
	{
		if (doingUpdates)
		{
			doingUpdates = false;

			clearRowBuffer(true);
		}
	}


	public synchronized void deleteRow()
	throws SQLException
	{
		if ( !isUpdateable() )
		{
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}

		if (onInsertRow)
		{
			throw new PSQLException( "postgresql.updateable.oninsertrow" );
		}

		if (rows.size() == 0)
		{
			throw new PSQLException( "postgresql.updateable.emptydelete" );
		}
		if (isBeforeFirst())
		{
			throw new PSQLException( "postgresql.updateable.beforestartdelete" );
		}
		if (isAfterLast())
		{
			throw new PSQLException( "postgresql.updateable.afterlastdelete" );
		}


		int numKeys = primaryKeys.size();
		if ( deleteStatement == null )
		{


			StringBuffer deleteSQL = new StringBuffer("DELETE FROM " ).append(tableName).append(" where " );

			for ( int i = 0; i < numKeys; i++ )
			{
				deleteSQL.append( ((PrimaryKey) primaryKeys.get(i)).name ).append( " = ? " );
				if ( i < numKeys - 1 )
				{
					deleteSQL.append( " and " );
				}
			}

			deleteStatement = ((java.sql.Connection) connection).prepareStatement(deleteSQL.toString());
		}
		deleteStatement.clearParameters();

		for ( int i = 0; i < numKeys; i++ )
		{
			deleteStatement.setObject(i + 1, ((PrimaryKey) primaryKeys.get(i)).getValue());
		}


		deleteStatement.executeUpdate();

		rows.removeElementAt(current_row);
		current_row--;
		moveToCurrentRow();
	}


	public synchronized void insertRow()
	throws SQLException
	{
		if ( !isUpdateable() )
		{
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}

		if (!onInsertRow)
		{
			throw new PSQLException( "postgresql.updateable.notoninsertrow" );
		}
		if (updateValues.size() == 0)
		{
			throw new PSQLException( "postgresql.updateable.noinsertvalues" );
		}
		else
		{

			// loop through the keys in the insertTable and create the sql statement
			// we have to create the sql every time since the user could insert different
			// columns each time

			StringBuffer insertSQL = new StringBuffer("INSERT INTO ").append(tableName).append(" (");
			StringBuffer paramSQL = new StringBuffer(") values (" );

			Enumeration columnNames = updateValues.keys();
			int numColumns = updateValues.size();

			for ( int i = 0; columnNames.hasMoreElements(); i++ )
			{
				String columnName = (String) columnNames.nextElement();

				insertSQL.append( columnName );
				if ( i < numColumns - 1 )
				{
					insertSQL.append(", ");
					paramSQL.append("?,");
				}
				else
				{
					paramSQL.append("?)");
				}

			}

			insertSQL.append(paramSQL.toString());
			insertStatement = ((java.sql.Connection) connection).prepareStatement(insertSQL.toString());

			Enumeration keys = updateValues.keys();

			for ( int i = 1; keys.hasMoreElements(); i++)
			{
				String key = (String) keys.nextElement();
				Object o = updateValues.get(key);
				if (o instanceof NullObject)
					insertStatement.setNull(i,java.sql.Types.NULL);
				else
					insertStatement.setObject(i, o);
			}

			insertStatement.executeUpdate();

			if ( usingOID )
			{
				// we have to get the last inserted OID and put it in the resultset

				long insertedOID = ((AbstractJdbc2Statement) insertStatement).getLastOID();

				updateValues.put("oid", new Long(insertedOID) );

			}

			// update the underlying row to the new inserted data
			updateRowBuffer();

			rows.addElement(rowBuffer);

			// we should now reflect the current data in this_row
			// that way getXXX will get the newly inserted data
			this_row = rowBuffer;

			// need to clear this in case of another insert
			clearRowBuffer(false);


		}
	}


	public synchronized void moveToCurrentRow()
	throws SQLException
	{
		if (!isUpdateable())
		{
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}

		if (current_row < 0 || current_row >= rows.size()) {
			this_row = null;
			rowBuffer = null;
		} else {
			this_row = (byte[][]) rows.elementAt(current_row);

			rowBuffer = new byte[this_row.length][];
			System.arraycopy(this_row, 0, rowBuffer, 0, this_row.length);
		}

		onInsertRow = false;
		doingUpdates = false;
	}


	public synchronized void moveToInsertRow()
	throws SQLException
	{
		if ( !isUpdateable() )
		{
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}

		if (insertStatement != null)
		{
			insertStatement = null;
		}


		// make sure the underlying data is null
		clearRowBuffer(false);

		onInsertRow = true;
		doingUpdates = false;

	}


	private synchronized void clearRowBuffer(boolean copyCurrentRow)
	throws SQLException
	{
		// rowBuffer is the temporary storage for the row
		rowBuffer = new byte[fields.length][];

		// inserts want an empty array while updates want a copy of the current row
		if (copyCurrentRow) {
			System.arraycopy(this_row, 0, rowBuffer, 0, this_row.length);
		}

		// clear the updateValues hashTable for the next set of updates
		updateValues.clear();

	}


	public boolean rowDeleted() throws SQLException
	{
		return false;
	}


	public boolean rowInserted() throws SQLException
	{
		return false;
	}


	public boolean rowUpdated() throws SQLException
	{
		return false;
	}


	public synchronized void updateAsciiStream(int columnIndex,
			java.io.InputStream x,
			int length
											  )
	throws SQLException
	{
		if (x == null)
		{
			updateNull(columnIndex);
			return;
		}

		try
		{
			InputStreamReader reader = new InputStreamReader(x, "ASCII");
			char data[] = new char[length];
			int numRead = 0;
			while (true)
			{
				int n = reader.read(data, numRead, length - numRead);
				if (n == -1)
					break;

				numRead += n;

				if (numRead == length)
					break;
			}
			updateString(columnIndex, new String(data, 0, numRead));
		}
		catch (UnsupportedEncodingException uee)
		{
			throw new PSQLException("postgresql.unusual", PSQLState.UNEXPECTED_ERROR, uee);
		}
		catch (IOException ie)
		{
			throw new PSQLException("postgresql.updateable.ioerror", ie);
		}

	}


	public synchronized void updateBigDecimal(int columnIndex,
			java.math.BigDecimal x )
	throws SQLException
	{
		updateValue(columnIndex, x);
	}


	public synchronized void updateBinaryStream(int columnIndex,
			java.io.InputStream x,
			int length
											   )
	throws SQLException
	{
		if (x == null)
		{
			updateNull(columnIndex);
			return;
		}

		byte data[] = new byte[length];
		int numRead = 0;
		try
		{
			while (true)
			{
				int n = x.read(data, numRead, length - numRead);
				if (n == -1)
					break;

				numRead += n;

				if (numRead == length)
					break;
			}
		}
		catch (IOException ie)
		{
			throw new PSQLException("postgresql.updateable.ioerror", ie);
		}

		if (numRead == length)
		{
			updateBytes(columnIndex, data);
		}
		else
		{
			// the stream contained less data than they said
			// perhaps this is an error?
			byte data2[] = new byte[numRead];
			System.arraycopy(data, 0, data2, 0, numRead);
			updateBytes(columnIndex, data2);
		}

	}


	public synchronized void updateBoolean(int columnIndex, boolean x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("updating boolean " + fields[columnIndex - 1].getName() + "=" + x);
		updateValue(columnIndex, new Boolean(x));
	}


	public synchronized void updateByte(int columnIndex, byte x)
	throws SQLException
	{
		updateValue(columnIndex, String.valueOf(x));
	}


	public synchronized void updateBytes(int columnIndex, byte[] x)
	throws SQLException
	{
		updateValue(columnIndex, x);
	}


	public synchronized void updateCharacterStream(int columnIndex,
			java.io.Reader x,
			int length
												  )
	throws SQLException
	{
		if (x == null)
		{
			updateNull(columnIndex);
			return;
		}

		try
		{
			char data[] = new char[length];
			int numRead = 0;
			while (true)
			{
				int n = x.read(data, numRead, length - numRead);
				if (n == -1)
					break;

				numRead += n;

				if (numRead == length)
					break;
			}
			updateString(columnIndex, new String(data, 0, numRead));
		}
		catch (IOException ie)
		{
			throw new PSQLException("postgresql.updateable.ioerror", ie);
		}
	}


	public synchronized void updateDate(int columnIndex, java.sql.Date x)
	throws SQLException
	{
		updateValue(columnIndex, x);
	}


	public synchronized void updateDouble(int columnIndex, double x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("updating double " + fields[columnIndex - 1].getName() + "=" + x);
		updateValue(columnIndex, new Double(x));
	}


	public synchronized void updateFloat(int columnIndex, float x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("updating float " + fields[columnIndex - 1].getName() + "=" + x);
		updateValue(columnIndex, new Float(x));
	}


	public synchronized void updateInt(int columnIndex, int x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("updating int " + fields[columnIndex - 1].getName() + "=" + x);
		updateValue(columnIndex, new Integer(x));
	}


	public synchronized void updateLong(int columnIndex, long x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("updating long " + fields[columnIndex - 1].getName() + "=" + x);
		updateValue(columnIndex, new Long(x));
	}


	public synchronized void updateNull(int columnIndex)
	throws SQLException
	{
		updateValue(columnIndex, new NullObject());
	}


	public synchronized void updateObject(int columnIndex, Object x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("updating object " + fields[columnIndex - 1].getName() + " = " + x);
		updateValue(columnIndex, x);
	}


	public synchronized void updateObject(int columnIndex, Object x, int scale)
	throws SQLException
	{
		if ( !isUpdateable() )
		{
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}

		this.updateObject(columnIndex, x);

	}


	public void refreshRow() throws SQLException
	{
		if ( !isUpdateable() )
		{
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}
		if (onInsertRow)
			throw new PSQLException("postgresql.res.oninsertrow");

		if (isBeforeFirst() || isAfterLast() || rows.size() == 0)
			return;

		StringBuffer selectSQL = new StringBuffer( "select ");

		final int numColumns = java.lang.reflect.Array.getLength(fields);

		for (int i = 0; i < numColumns; i++ )
		{

			selectSQL.append( fields[i].getName() );

			if ( i < numColumns - 1 )
			{

				selectSQL.append(", ");
			}

		}
		selectSQL.append(" from " ).append(tableName).append(" where ");

		int numKeys = primaryKeys.size();

		for ( int i = 0; i < numKeys; i++ )
		{

			PrimaryKey primaryKey = ((PrimaryKey) primaryKeys.get(i));
			selectSQL.append(primaryKey.name).append("= ?");

			if ( i < numKeys - 1 )
			{
				selectSQL.append(" and ");
			}
		}
		if ( Driver.logDebug )
			Driver.debug("selecting " + selectSQL.toString());
		selectStatement = ((java.sql.Connection) connection).prepareStatement(selectSQL.toString());


		for ( int j = 0, i = 1; j < numKeys; j++, i++)
		{
			selectStatement.setObject( i, ((PrimaryKey) primaryKeys.get(j)).getValue() );
		}

		AbstractJdbc2ResultSet rs = (AbstractJdbc2ResultSet) selectStatement.executeQuery();

		if ( rs.first() )
		{
			rowBuffer = rs.rowBuffer;
		}

		rows.setElementAt( rowBuffer, current_row );
		this_row = rowBuffer;
		if ( Driver.logDebug )
			Driver.debug("done updates");

		rs.close();
		selectStatement.close();
		selectStatement = null;

	}


	public synchronized void updateRow()
	throws SQLException
	{
		if ( !isUpdateable() )
		{
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}
		if (isBeforeFirst() || isAfterLast() || rows.size() == 0)
		{
			throw new PSQLException("postgresql.updateable.badupdateposition");
		}

		if (doingUpdates)
		{

			try
			{

				StringBuffer updateSQL = new StringBuffer("UPDATE " + tableName + " SET  ");

				int numColumns = updateValues.size();
				Enumeration columns = updateValues.keys();

				for (int i = 0; columns.hasMoreElements(); i++ )
				{

					String column = (String) columns.nextElement();
					updateSQL.append("\"");
					updateSQL.append( column );
					updateSQL.append("\" = ?");

					if ( i < numColumns - 1 )
					{

						updateSQL.append(", ");
					}

				}
				updateSQL.append( " WHERE " );

				int numKeys = primaryKeys.size();

				for ( int i = 0; i < numKeys; i++ )
				{

					PrimaryKey primaryKey = ((PrimaryKey) primaryKeys.get(i));
					updateSQL.append("\"");
					updateSQL.append(primaryKey.name);
					updateSQL.append("\" = ?");

					if ( i < numKeys - 1 )
					{
						updateSQL.append(" and ");
					}
				}
				if ( Driver.logDebug )
					Driver.debug("updating " + updateSQL.toString());
				updateStatement = ((java.sql.Connection) connection).prepareStatement(updateSQL.toString());

				int i = 0;
				Iterator iterator = updateValues.values().iterator();
				for (; iterator.hasNext(); i++)
				{
					Object o = iterator.next();
					if (o instanceof NullObject)
						updateStatement.setNull(i+1,java.sql.Types.NULL);
					else
						updateStatement.setObject( i + 1, o );

				}
				for ( int j = 0; j < numKeys; j++, i++)
				{
					updateStatement.setObject( i + 1, ((PrimaryKey) primaryKeys.get(j)).getValue() );
				}

				updateStatement.executeUpdate();
				updateStatement.close();

				updateStatement = null;
				updateRowBuffer();


				if ( Driver.logDebug )
					Driver.debug("copying data");
				System.arraycopy(rowBuffer, 0, this_row, 0, rowBuffer.length);

				rows.setElementAt( rowBuffer, current_row );
				if ( Driver.logDebug )
					Driver.debug("done updates");
				updateValues.clear();
				doingUpdates = false;
			}
			catch (SQLException e)
			{
				if ( Driver.logDebug )
					Driver.debug(e.getClass().getName() + e);
				throw e;
			}

		}

	}


	public synchronized void updateShort(int columnIndex, short x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("in update Short " + fields[columnIndex - 1].getName() + " = " + x);
		updateValue(columnIndex, new Short(x));
	}


	public synchronized void updateString(int columnIndex, String x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("in update String " + fields[columnIndex - 1].getName() + " = " + x);
		updateValue(columnIndex, x);
	}


	public synchronized void updateTime(int columnIndex, Time x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("in update Time " + fields[columnIndex - 1].getName() + " = " + x);
		updateValue(columnIndex, x);
	}


	public synchronized void updateTimestamp(int columnIndex, Timestamp x)
	throws SQLException
	{
		if ( Driver.logDebug )
			Driver.debug("updating Timestamp " + fields[columnIndex - 1].getName() + " = " + x);
		updateValue(columnIndex, x);

	}


	public synchronized void updateNull(String columnName)
	throws SQLException
	{
		updateNull(findColumn(columnName));
	}


	public synchronized void updateBoolean(String columnName, boolean x)
	throws SQLException
	{
		updateBoolean(findColumn(columnName), x);
	}


	public synchronized void updateByte(String columnName, byte x)
	throws SQLException
	{
		updateByte(findColumn(columnName), x);
	}


	public synchronized void updateShort(String columnName, short x)
	throws SQLException
	{
		updateShort(findColumn(columnName), x);
	}


	public synchronized void updateInt(String columnName, int x)
	throws SQLException
	{
		updateInt(findColumn(columnName), x);
	}


	public synchronized void updateLong(String columnName, long x)
	throws SQLException
	{
		updateLong(findColumn(columnName), x);
	}


	public synchronized void updateFloat(String columnName, float x)
	throws SQLException
	{
		updateFloat(findColumn(columnName), x);
	}


	public synchronized void updateDouble(String columnName, double x)
	throws SQLException
	{
		updateDouble(findColumn(columnName), x);
	}


	public synchronized void updateBigDecimal(String columnName, BigDecimal x)
	throws SQLException
	{
		updateBigDecimal(findColumn(columnName), x);
	}


	public synchronized void updateString(String columnName, String x)
	throws SQLException
	{
		updateString(findColumn(columnName), x);
	}


	public synchronized void updateBytes(String columnName, byte x[])
	throws SQLException
	{
		updateBytes(findColumn(columnName), x);
	}


	public synchronized void updateDate(String columnName, java.sql.Date x)
	throws SQLException
	{
		updateDate(findColumn(columnName), x);
	}


	public synchronized void updateTime(String columnName, java.sql.Time x)
	throws SQLException
	{
		updateTime(findColumn(columnName), x);
	}


	public synchronized void updateTimestamp(String columnName, java.sql.Timestamp x)
	throws SQLException
	{
		updateTimestamp(findColumn(columnName), x);
	}


	public synchronized void updateAsciiStream(
		String columnName,
		java.io.InputStream x,
		int length)
	throws SQLException
	{
		updateAsciiStream(findColumn(columnName), x, length);
	}


	public synchronized void updateBinaryStream(
		String columnName,
		java.io.InputStream x,
		int length)
	throws SQLException
	{
		updateBinaryStream(findColumn(columnName), x, length);
	}


	public synchronized void updateCharacterStream(
		String columnName,
		java.io.Reader reader,
		int length)
	throws SQLException
	{
		updateCharacterStream(findColumn(columnName), reader, length);
	}


	public synchronized void updateObject(String columnName, Object x, int scale)
	throws SQLException
	{
		updateObject(findColumn(columnName), x);
	}


	public synchronized void updateObject(String columnName, Object x)
	throws SQLException
	{
		updateObject(findColumn(columnName), x);
	}


	/**
	 * Is this ResultSet updateable?
	 */

	boolean isUpdateable() throws SQLException
	{

		if (updateable)
			return true;

		if ( Driver.logDebug )
			Driver.debug("checking if rs is updateable");

		parseQuery();

		if ( singleTable == false )
		{
			if ( Driver.logDebug )
				Driver.debug("not a single table");
			return false;
		}

		if ( Driver.logDebug )
			Driver.debug("getting primary keys");

		//
		// Contains the primary key?
		//

		primaryKeys = new Vector();

		// this is not stricty jdbc spec, but it will make things much faster if used
		// the user has to select oid, * from table and then we will just use oid


		usingOID = false;
		int oidIndex = 0;
		try {
		  oidIndex = findColumn( "oid" );
		} catch (SQLException l_se) {
			//Ignore if column oid isn't selected
		}
		int i = 0;


		// if we find the oid then just use it

		//oidIndex will be >0 if the oid was in the select list
		if ( oidIndex > 0 )
		{
			i++;
			primaryKeys.add( new PrimaryKey( oidIndex, "oid" ) );
			usingOID = true;
		}
		else
		{
			// otherwise go and get the primary keys and create a hashtable of keys
			String[] s = quotelessTableName(tableName);
			String quotelessTableName = s[0];
			String quotelessSchemaName = s[1];
			java.sql.ResultSet rs = ((java.sql.Connection) connection).getMetaData().getPrimaryKeys("", quotelessSchemaName, quotelessTableName);
			for (; rs.next(); i++ )
			{
				String columnName = rs.getString(4);	// get the columnName
				int index = findColumn( columnName );

				if ( index > 0 )
				{
					primaryKeys.add( new PrimaryKey(index, columnName ) ); // get the primary key information
				}
			}

			rs.close();
		}

		numKeys = primaryKeys.size();

		if ( Driver.logDebug )
			Driver.debug( "no of keys=" + i );

		if ( i < 1 )
		{
			throw new SQLException("No Primary Keys");
		}

		updateable = primaryKeys.size() > 0;

		if ( Driver.logDebug )
			Driver.debug( "checking primary key " + updateable );

		return updateable;
	}

	/** Cracks out the table name and schema (if it exists) from a fully
	 * qualified table name.
	 * @param fullname string that we are trying to crack.	Test cases:<pre>
	 * Table: table ()
	 * "Table": Table ()
	 * Schema.Table: table (schema)
	 * "Schema"."Table": Table (Schema)
	 * "Schema"."Dot.Table": Dot.Table (Schema)
	 * Schema."Dot.Table": Dot.Table (schema)
	 * </pre>
	 * @return String array with element zero always being the tablename and
	 * element 1 the schema name which may be a zero length string.
	 */
	public static String[] quotelessTableName(String fullname) {
		StringBuffer buf = new StringBuffer(fullname);
		String[] parts = new String[] {null, ""};
		StringBuffer acc = new StringBuffer();
		boolean betweenQuotes = false;
		for (int i = 0; i < buf.length(); i++) {
			char c = buf.charAt(i);
			switch (c) {
				case '"':
					if ((i < buf.length() - 1) && (buf.charAt(i+1) == '"')) {
						// two consecutive quotes - keep one
						i++;
						acc.append(c);	// keep the quote
					}
					else {	// Discard it
						betweenQuotes = !betweenQuotes;
					}
					break;
				case '.':
					if (betweenQuotes) {   // Keep it
						acc.append(c);
					}
					else {	   // Have schema name
						parts[1] = acc.toString();
						acc = new StringBuffer();
					}
					break;
				default:
					acc.append((betweenQuotes) ? c : Character.toLowerCase(c));
					break;
			}
		}
		// Always put table in slot 0
		parts[0] = acc.toString();
		return parts;
	}
 
	public void parseQuery()
	{
		String[] l_sqlFragments = ((AbstractJdbc2Statement)statement).getSqlFragments();
		String l_sql = l_sqlFragments[0];
		StringTokenizer st = new StringTokenizer(l_sql, " \r\t\n");
		boolean tableFound = false, tablesChecked = false;
		String name = "";

		singleTable = true;

		while ( !tableFound && !tablesChecked && st.hasMoreTokens() )
		{
			name = st.nextToken();
			if ( !tableFound )
			{
				if (name.toLowerCase().equals("from"))
				{
					tableName = st.nextToken();
					tableFound = true;
				}
			}
			else
			{
				tablesChecked = true;
				// if the very next token is , then there are multiple tables
				singleTable = !name.equalsIgnoreCase(",");
			}
		}
	}


	private void updateRowBuffer() throws SQLException
	{

		Enumeration columns = updateValues.keys();

		while ( columns.hasMoreElements() )
		{
			String columnName = (String) columns.nextElement();
			int columnIndex = findColumn( columnName ) - 1;

			Object valueObject = updateValues.get(columnName);
			if (valueObject instanceof NullObject) {
				rowBuffer[columnIndex] = null;
			}
			else
			{
				
				switch ( connection.getSQLType( fields[columnIndex].getPGType() ) )
				{

					case Types.DECIMAL:
					case Types.BIGINT:
					case Types.DOUBLE:
					case Types.BIT:
					case Types.VARCHAR:
					case Types.DATE:
					case Types.TIME:
					case Types.TIMESTAMP:
					case Types.SMALLINT:
					case Types.FLOAT:
					case Types.INTEGER:
					case Types.CHAR:
					case Types.NUMERIC:
					case Types.REAL:
					case Types.TINYINT:
					case Types.OTHER:

						rowBuffer[columnIndex] = connection.getEncoding().encode(String.valueOf( valueObject));

					case Types.NULL:
						continue;

					default:
						rowBuffer[columnIndex] = (byte[]) valueObject;
				}

			}
		}
	}


	public void setStatement(BaseStatement statement)
	{
		this.statement = statement;
	}

	protected void updateValue(int columnIndex, Object value) throws SQLException {
		if ( !isUpdateable() )
		{
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}
		if (!onInsertRow && (isBeforeFirst() || isAfterLast() || rows.size() == 0))
		{
			throw new PSQLException("postgresql.updateable.badupdateposition");
		}
		doingUpdates = !onInsertRow;
		if (value == null)
			updateNull(columnIndex);
		else
			updateValues.put(fields[columnIndex - 1].getName(), value);
	}

	private class PrimaryKey
	{
		int index;				// where in the result set is this primaryKey
		String name;			// what is the columnName of this primary Key

		PrimaryKey( int index, String name)
		{
			this.index = index;
			this.name = name;
		}
		Object getValue() throws SQLException
		{
			return getObject(index);
		}
	};

	class NullObject {
	};

}

