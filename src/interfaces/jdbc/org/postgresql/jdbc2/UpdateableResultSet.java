package org.postgresql.jdbc2;

// IMPORTANT NOTE: This is the begining of supporting updateable ResultSets.
// It is not a working solution (yet)!
//
// You will notice here we really do throw org.postgresql.Driver.notImplemented()
// This is because here we should be updateable, so any unimplemented methods
// must say so.
//
// Also you'll notice that the String columnName based calls are not present.
// They are not required as they are in the super class.
//

import java.lang.*;
import java.io.*;
import java.math.*;
import java.text.*;
import java.util.*;
import java.sql.*;
import org.postgresql.Field;
import org.postgresql.largeobject.*;
import org.postgresql.util.*;
import org.postgresql.Driver;

/*
 * @see ResultSet
 * @see ResultSetMetaData
 * @see java.sql.ResultSet
 */
public class UpdateableResultSet extends org.postgresql.jdbc2.ResultSet
{


  class PrimaryKey
  {
    int index;              // where in the result set is this primaryKey
    String name;            // what is the columnName of this primary Key

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

  private boolean usingOID = false;   // are we using the OID for the primary key?

  private Vector primaryKeys;    // list of primary keys

  private int numKeys = 0;

  private boolean singleTable = false;

	protected String tableName = null;

	/**
	 * PreparedStatement used to delete data
	 */

	protected java.sql.PreparedStatement updateStatement = null;

	/**
	 * PreparedStatement used to insert data
	 */

	protected java.sql.PreparedStatement insertStatement = null;

	/**
	 * PreparedStatement used to delete data
	 */

	protected java.sql.PreparedStatement deleteStatement = null;


  private java.sql.Statement currentStatement = null;


	/**
	 * Is this result set updateable?
	 */

	protected boolean updateable = false;

	/**
	 * Are we in the middle of doing updates to the current row?
	 */

	protected boolean doingUpdates = false;


	/**
	 * Are we on the insert row?
	 */

	protected boolean onInsertRow = false;


	protected Hashtable updateValues = new Hashtable();

	// The Row Buffer will be used to cache updated rows..then we shall sync this with the rows vector


	/*
	 * Create a new ResultSet - Note that we create ResultSets to
	 * represent the results of everything.
	 *
	 * @param fields an array of Field objects (basically, the
	 *	ResultSet MetaData)
	 * @param tuples Vector of the actual data
	 * @param status the status string returned from the back end
	 * @param updateCount the number of rows affected by the operation
	 * @param cursor the positioned update/delete cursor name
	 */
	public UpdateableResultSet(Connection conn, Field[] fields, Vector tuples, String status, int updateCount, long insertOID, boolean binaryCursor)
	{
		super(conn, fields, tuples, status, updateCount, insertOID, binaryCursor);
	}

  /**
   *
   * @throws SQLException
   */
	public synchronized void cancelRowUpdates() throws SQLException
  {
		if (doingUpdates)
    {
			doingUpdates = false;

			clearRowBuffer();
		}
	}

  /**
   *
   * @throws SQLException
   */
	public synchronized void deleteRow() throws SQLException
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


      StringBuffer deleteSQL= new StringBuffer("DELETE FROM " ).append(tableName).append(" where " );

      for ( int i=0; i < numKeys ; i++ )
      {
        deleteSQL.append( ((PrimaryKey)primaryKeys.get(i)).name ).append( " = ? " );
        if ( i < numKeys-1 )
        {
          deleteSQL.append( " and " );
        }
      }

      deleteStatement = ((java.sql.Connection)connection).prepareStatement(deleteSQL.toString());
    }
    deleteStatement.clearParameters();

    for ( int i =0; i < numKeys; i++ )
    {
      deleteStatement.setObject(i+1, ((PrimaryKey)primaryKeys.get(i)).getValue());
    }


    deleteStatement.executeUpdate();

    rows.removeElementAt(current_row);
  }


  /**
   *
   * @return
   * @throws SQLException
   */
	public int getConcurrency() throws SQLException
	{
		// New in 7.1 - The updateable ResultSet class will now return
		// CONCURuPDATEABLE.
		return CONCUR_UPDATABLE;
	}

  /**
   *
   * @throws SQLException
   */

	public synchronized void insertRow() throws SQLException
  {
		if ( !isUpdateable() )
    {
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}

		if (!onInsertRow)
    {
			throw new PSQLException( "postgresql.updateable.notoninsertrow" );
		}
		else
    {

      // loop through the keys in the insertTable and create the sql statement
      // we have to create the sql every time since the user could insert different
      // columns each time

      StringBuffer insertSQL=new StringBuffer("INSERT INTO ").append(tableName).append(" (");
      StringBuffer paramSQL = new StringBuffer(") values (" );

      Enumeration columnNames = updateValues.keys();
      int numColumns = updateValues.size();

      for ( int i=0; columnNames.hasMoreElements() ; i++ )
      {
        String columnName = (String)columnNames.nextElement();

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
      insertStatement = ((java.sql.Connection)connection).prepareStatement(insertSQL.toString());

      Enumeration keys = updateValues.keys();

      for( int i=1; keys.hasMoreElements() ; i++)
      {
        String key = (String)keys.nextElement();
        insertStatement.setObject(i, updateValues.get( key ) );
      }

      insertStatement.executeUpdate();

      if ( usingOID )
      {
        // we have to get the last inserted OID and put it in the resultset

        long insertedOID = ((org.postgresql.Statement)insertStatement).getLastOID();

        updateValues.put("oid", new Long(insertedOID) );

      }

      // update the underlying row to the new inserted data
      updateRowBuffer();

			rows.addElement(rowBuffer);

		  // we should now reflect the current data in this_row
      // that way getXXX will get the newly inserted data
      this_row = rowBuffer;

      // need to clear this in case of another insert
      clearRowBuffer();


		}
	}


  /**
   *
   * @throws SQLException
   */

	public synchronized void moveToCurrentRow() throws SQLException
	{
		this_row = (byte [][])rows.elementAt(current_row);

		rowBuffer=new byte[this_row.length][];
		System.arraycopy(this_row,0,rowBuffer,0,this_row.length);

    onInsertRow = false;
    doingUpdates = false;
	}

  /**
   *
   * @throws SQLException
   */
	public synchronized void  moveToInsertRow() throws SQLException
	{
		// only sub-classes implement CONCURuPDATEABLE
		if (!updateable)
    {
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}

		if (insertStatement != null)
    {
      insertStatement = null;
		}


    // make sure the underlying data is null
    clearRowBuffer();

		onInsertRow = true;
		doingUpdates = false;

	}

  /**
   *
   * @throws SQLException
   */
	private synchronized void clearRowBuffer() throws SQLException
  {
    // rowBuffer is the temporary storage for the row
		rowBuffer=new byte[fields.length][];

    // clear the updateValues hashTable for the next set of updates
    updateValues.clear();

	}


  /**
   *
   * @return
   * @throws SQLException
   */
	public boolean rowDeleted() throws SQLException
	{
		// only sub-classes implement CONCURuPDATEABLE
		throw Driver.notImplemented();
	}

  /**
   *
   * @return
   * @throws SQLException
   */
	public boolean rowInserted() throws SQLException
	{
		// only sub-classes implement CONCURuPDATEABLE
		throw Driver.notImplemented();
	}

  /**
   *
   * @return
   * @throws SQLException
   */
	public boolean rowUpdated() throws SQLException
	{
		// only sub-classes implement CONCURuPDATEABLE
		throw Driver.notImplemented();
	}

  /**
   *
   * @param columnIndex
   * @param x
   * @param length
   * @throws SQLException
   */
	public synchronized void updateAsciiStream(int columnIndex,
								  java.io.InputStream x,
								  int length
								 ) throws SQLException
	{

		byte[] theData=null;

		try
    {
			x.read(theData,0,length);
		}
    catch (NullPointerException ex )
    {
      throw new PSQLException("postgresql.updateable.inputstream");
    }
		catch (IOException ie)
    {
			throw new PSQLException("postgresql.updateable.ioerror" + ie);
		}

    doingUpdates = !onInsertRow;

    updateValues.put( fields[columnIndex-1].getName(), theData );

	}

  /**
   *
   * @param columnIndex
   * @param x
   * @throws SQLException
   */
	public synchronized void updateBigDecimal(int columnIndex,
                                           java.math.BigDecimal x )
                                                                throws SQLException
	{

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), x );

	}

  /**
   *
   * @param columnIndex
   * @param x
   * @param length
   * @throws SQLException
   */
	public synchronized void updateBinaryStream(int columnIndex,
                                               java.io.InputStream x,
                                               int length
                                              ) throws SQLException
	{


		byte[] theData=null;

		try {
			x.read(theData,0,length);

		}
    catch( NullPointerException ex )
    {
      throw new PSQLException("postgresql.updateable.inputstream");
    }
		catch (IOException ie)
    {
			throw new PSQLException("postgresql.updateable.ioerror" + ie);
		}

    doingUpdates = !onInsertRow;

    updateValues.put( fields[columnIndex-1].getName(), theData );

	}

  /**
   *
   * @param columnIndex
   * @param x
   * @throws SQLException
   */
	public synchronized void updateBoolean(int columnIndex, boolean x) throws SQLException
	{

		if ( Driver.logDebug ) Driver.debug("updating boolean "+fields[columnIndex-1].getName()+"="+x);

    doingUpdates = !onInsertRow;
	  updateValues.put( fields[columnIndex-1].getName(), new Boolean(x) );

	}

  /**
   *
   * @param columnIndex
   * @param x
   * @throws SQLException
   */
	public synchronized void updateByte(int columnIndex, byte x) throws SQLException
	{

    doingUpdates = true;
    updateValues.put( fields[columnIndex-1].getName(), String.valueOf(x) );
	}

  /**
   *
   * @param columnIndex
   * @param x
   * @throws SQLException
   */
	public synchronized void updateBytes(int columnIndex, byte[] x) throws SQLException
	{

    doingUpdates = !onInsertRow;
		updateValues.put( fields[columnIndex-1].getName(), x );

	}

  /**
   *
   * @param columnIndex
   * @param x
   * @param length
   * @throws SQLException
   */
	public synchronized void updateCharacterStream(int columnIndex,
                                                  java.io.Reader x,
                                                  int length
                                                 ) throws SQLException
	{


		char[] theData=null;

		try
    {
			x.read(theData,0,length);

		}
    catch (NullPointerException ex)
    {
      throw new PSQLException("postgresql.updateable.inputstream");
    }
		catch (IOException ie)
    {
			throw new PSQLException("postgresql.updateable.ioerror" + ie);
		}

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), theData);

	}

	public synchronized void updateDate(int columnIndex, java.sql.Date x) throws SQLException
	{

    doingUpdates = !onInsertRow;
		updateValues.put( fields[columnIndex-1].getName(), x );
	}

	public synchronized void updateDouble(int columnIndex, double x) throws SQLException
	{
		if ( Driver.logDebug ) Driver.debug("updating double "+fields[columnIndex-1].getName()+"="+x);

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), new Double(x) );

	}

	public synchronized void updateFloat(int columnIndex, float x) throws SQLException
	{
		if ( Driver.logDebug ) Driver.debug("updating float "+fields[columnIndex-1].getName()+"="+x);

    doingUpdates = !onInsertRow;

		updateValues.put( fields[columnIndex-1].getName(), new Float(x) );

	}

	public synchronized void updateInt(int columnIndex, int x) throws SQLException
	{
		if ( Driver.logDebug ) Driver.debug("updating int "+fields[columnIndex-1].getName()+"="+x);

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), new Integer(x) );

	}

	public synchronized void updateLong(int columnIndex, long x) throws SQLException
	{
		if ( Driver.logDebug ) Driver.debug("updating long "+fields[columnIndex-1].getName()+"="+x);

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), new Long(x) );

	}

	public synchronized void updateNull(int columnIndex) throws SQLException
	{

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), null);


	}

	public synchronized void updateObject(int columnIndex, Object x) throws SQLException
	{


		if ( Driver.logDebug ) Driver.debug("updating object " + fields[columnIndex-1].getName() + " = " + x);

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), x );
	}

	public synchronized void updateObject(int columnIndex, Object x, int scale) throws SQLException
	{

    this.updateObject(columnIndex, x);

	}

  /**
   *
   * @throws SQLException
   */
	public synchronized void updateRow() throws SQLException
  {
		if ( !isUpdateable() )
    {
			throw new PSQLException( "postgresql.updateable.notupdateable" );
		}

		if (doingUpdates)
    {

			try
      {

        StringBuffer updateSQL=new StringBuffer("UPDATE "+tableName+" SET  ");

        int numColumns = updateValues.size();
        Enumeration columns = updateValues.keys();

        for (int i=0; columns.hasMoreElements() ; i++ )
        {

          String column = (String)columns.nextElement();
          updateSQL.append( column + "= ?");

          if ( i < numColumns - 1 )
          {

            updateSQL.append(", ");
          }

        }
        updateSQL.append( " WHERE " );

        int numKeys = primaryKeys.size();

        for ( int i = 0; i < numKeys; i++ )
        {

          PrimaryKey primaryKey = ((PrimaryKey)primaryKeys.get(i));
          updateSQL.append(primaryKey.name).append("= ?");

          if ( i < numKeys -1 )
          {
            updateSQL.append(" and ");
          }
        }
        if ( Driver.logDebug ) Driver.debug("updating "+updateSQL.toString());
        updateStatement = ((java.sql.Connection)connection).prepareStatement(updateSQL.toString());

        int i = 0;
        Iterator iterator = updateValues.values().iterator();
        for (; iterator.hasNext(); i++)
        {
          updateStatement.setObject( i+1, iterator.next() );

        }
        for( int j=0; j < numKeys; j++, i++)
        {
          updateStatement.setObject( i+1, ((PrimaryKey)primaryKeys.get(j)).getValue() );
        }

        updateStatement.executeUpdate();
        updateStatement.close();

        updateStatement = null;
        updateRowBuffer();


				if ( Driver.logDebug ) Driver.debug("copying data");
				System.arraycopy(rowBuffer,0,this_row,0,rowBuffer.length);

				rows.setElementAt( rowBuffer, current_row );
        if ( Driver.logDebug ) Driver.debug("done updates");

			  doingUpdates = false;
			}
			catch(Exception e)
      {
				if ( Driver.logDebug ) Driver.debug(e.getClass().getName()+e);
        throw new SQLException( e.getMessage() );
			}

		}

	}

	public synchronized void updateShort(int columnIndex, short x) throws SQLException
	{
		if ( Driver.logDebug ) Driver.debug("in update Short "+fields[columnIndex-1].getName()+" = "+x);


    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), new Short(x) );

	}

	public synchronized void updateString(int columnIndex, String x) throws SQLException
	{
		if ( Driver.logDebug ) Driver.debug("in update String "+fields[columnIndex-1].getName()+" = "+x);

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), x );

	}

	public synchronized void updateTime(int columnIndex, Time x) throws SQLException
	{
		if ( Driver.logDebug ) Driver.debug("in update Time "+fields[columnIndex-1].getName()+" = "+x);


    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), x );

	}

	public synchronized void updateTimestamp(int columnIndex, Timestamp x) throws SQLException
	{
		if ( Driver.logDebug ) Driver.debug("updating Timestamp "+fields[columnIndex-1].getName()+" = "+x);

    doingUpdates = !onInsertRow;
    updateValues.put( fields[columnIndex-1].getName(), x );


	}

	public synchronized void updateNull(String columnName) throws SQLException
  {
		updateNull(findColumn(columnName));
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a boolean value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateBoolean(String columnName, boolean x) throws SQLException
  {
		updateBoolean(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a byte value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateByte(String columnName, byte x) throws SQLException
  {
		updateByte(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a short value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateShort(String columnName, short x) throws SQLException
  {
		updateShort(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with an integer value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateInt(String columnName, int x) throws SQLException
  {
		updateInt(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a long value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateLong(String columnName, long x) throws SQLException
  {
		updateLong(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a float value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateFloat(String columnName, float x) throws SQLException
  {
		updateFloat(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a double value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateDouble(String columnName, double x) throws SQLException
  {
		updateDouble(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a BigDecimal value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateBigDecimal(String columnName, BigDecimal x)
		                                                           throws SQLException
  {
		updateBigDecimal(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a String value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateString(String columnName, String x) throws SQLException
  {
		updateString(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a byte array value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateBytes(String columnName, byte x[]) throws SQLException
  {
		updateBytes(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a Date value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateDate(String columnName, java.sql.Date x)
		                                                                   throws SQLException
  {
		updateDate(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a Time value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateTime(String columnName, java.sql.Time x)
		                                                                   throws SQLException
  {
		updateTime(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a Timestamp value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateTimestamp(String columnName, java.sql.Timestamp x)
                                                                                 throws SQLException
  {
		updateTimestamp(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with an ascii stream value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @param length of the stream
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateAsciiStream(
                                              String columnName,
                                              java.io.InputStream x,
                                              int length)
                                              throws SQLException
  {
		updateAsciiStream(findColumn(columnName), x, length);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a binary stream value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @param length of the stream
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateBinaryStream(
                                              String columnName,
                                              java.io.InputStream x,
                                              int length)
                                              throws SQLException
  {
		updateBinaryStream(findColumn(columnName), x, length);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with a character stream value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @param length of the stream
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateCharacterStream(
                                                  String columnName,
                                                  java.io.Reader reader,
                                                  int length)
                                                  throws SQLException
  {
		updateCharacterStream(findColumn(columnName), reader,length);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with an Object value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @param scale For java.sql.Types.DECIMAL or java.sql.Types.NUMERIC types
	 *  this is the number of digits after the decimal.  For all other
	 *  types this value will be ignored.
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateObject(String columnName, Object x, int scale)
		                                                              throws SQLException
  {
		updateObject(findColumn(columnName), x);
	}

	/**
	 * JDBC 2.0
	 *
	 * Update a column with an Object value.
	 *
	 * The updateXXX() methods are used to update column values in the
	 * current row, or the insert row.  The updateXXX() methods do not
	 * update the underlying database, instead the updateRow() or insertRow()
	 * methods are called to update the database.
	 *
	 * @param columnName the name of the column
	 * @param x the new column value
	 * @exception SQLException if a database-access error occurs
	 */

	public synchronized void updateObject(String columnName, Object x) throws SQLException
  {
		updateObject(findColumn(columnName), x);
	}



  private int _findColumn( String columnName )
  {
		int i;

		final int flen = fields.length;
		for (i = 0 ; i < flen; ++i)
    {
			if (fields[i].getName().equalsIgnoreCase(columnName))
      {
				return (i + 1);
      }
    }
    return -1;
	}


	/**
	 * Is this ResultSet updateable?
	 */

	boolean isUpdateable() throws SQLException
  {

    if (updateable) return true;

    if ( Driver.logDebug ) Driver.debug("checking if rs is updateable");

    parseQuery();

    if ( singleTable == false )
    {
		  if ( Driver.logDebug ) Driver.debug("not a single table");
      return false;
    }

		if ( Driver.logDebug ) Driver.debug("getting primary keys");

		//
		// Contains the primary key?
		//

		primaryKeys = new Vector();

    // this is not stricty jdbc spec, but it will make things much faster if used
    // the user has to select oid, * from table and then we will just use oid


    usingOID = false;
    int oidIndex = _findColumn( "oid" );
    int i = 0;


    // if we find the oid then just use it

    if ( oidIndex > 0 )
    {
      i++;
      primaryKeys.add( new PrimaryKey( oidIndex, "oid" ) );
      usingOID = true;
    }
    else
    {
      // otherwise go and get the primary keys and create a hashtable of keys
      java.sql.ResultSet rs  = ((org.postgresql.jdbc2.Connection)connection).getMetaData().getPrimaryKeys("","",tableName);


      for( ; rs.next(); i++ )
      {
        String columnName = rs.getString(4);    // get the columnName

        int index = findColumn( columnName );

        if ( index > 0 )
        {
          primaryKeys.add( new PrimaryKey(index, columnName ) ); // get the primary key information
        }
      }

      rs.close();
    }

    numKeys = primaryKeys.size();

    if ( Driver.logDebug ) Driver.debug( "no of keys=" + i );

    if ( i < 1 )
    {
			throw new SQLException("No Primary Keys");
		}

		updateable = primaryKeys.size() > 0;

		if ( Driver.logDebug ) Driver.debug( "checking primary key " + updateable );

		return updateable;
	}


  /**
   *
   */
	public void parseQuery()
  {
		StringTokenizer st=new StringTokenizer(sqlQuery," \r\t");
		boolean tableFound=false, tablesChecked = false;
		String name="";

    singleTable = true;

    while ( !tableFound && !tablesChecked && st.hasMoreTokens() )
    {
			name=st.nextToken();
      if ( !tableFound )
      {
        if (name.toLowerCase().equals("from"))
        {
          tableName=st.nextToken();
          tableFound=true;
        }
      }
      else
      {
        tablesChecked = true;
        // if the very next token is , then there are multiple tables
        singleTable =  !name.equalsIgnoreCase(",");
      }
		}
	}


  private void updateRowBuffer() throws SQLException
  {

    Enumeration columns = updateValues.keys();

    while( columns.hasMoreElements() )
    {
      String columnName = (String)columns.nextElement();
      int columnIndex = _findColumn( columnName ) - 1;

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

          try
          {
            rowBuffer[columnIndex] = String.valueOf( updateValues.get( columnName ) ).getBytes(connection.getEncoding().name() );
          }
          catch ( UnsupportedEncodingException ex)
          {
            throw new SQLException("Unsupported Encoding "+connection.getEncoding().name());
          }
        case Types.NULL:
          continue;
        default:
          rowBuffer[columnIndex] = (byte [])updateValues.get( columnName );
      }

    }
  }

}

