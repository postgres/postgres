package org.postgresql.jdbc2;

// IMPORTANT NOTE: This is the begining of supporting updatable ResultSets.
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

/**
 * @see ResultSet
 * @see ResultSetMetaData
 * @see java.sql.ResultSet
 */
public class UpdateableResultSet extends org.postgresql.jdbc2.ResultSet
{

  /**
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
  public UpdateableResultSet(Connection conn, Field[] fields, Vector tuples, String status, int updateCount,int insertOID)
  {
      super(conn,fields,tuples,status,updateCount,insertOID);
  }

  /**
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
  public UpdateableResultSet(Connection conn, Field[] fields, Vector tuples, String status, int updateCount)
  {
      super(conn,fields,tuples,status,updateCount,0);
  }

    public void cancelRowUpdates() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void deleteRow() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public int getConcurrency() throws SQLException
    {
        // New in 7.1 - The updateable ResultSet class will now return
        // CONCUR_UPDATEABLE.
	return CONCUR_UPDATABLE;
    }

    public void insertRow() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void moveToCurrentRow() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void moveToInsertRow() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public boolean rowDeleted() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
      //return false; // javac complains about not returning a value!
    }

    public boolean rowInserted() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
      //return false; // javac complains about not returning a value!
    }

    public boolean rowUpdated() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
      //return false; // javac complains about not returning a value!
    }

    public void updateAsciiStream(int columnIndex,
				  java.io.InputStream x,
				  int length
				  ) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateBigDecimal(int columnIndex,
				  java.math.BigDecimal x
				  ) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateBinaryStream(int columnIndex,
				  java.io.InputStream x,
				  int length
				  ) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateBoolean(int columnIndex,boolean x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateByte(int columnIndex,byte x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateBytes(int columnIndex,byte[] x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateCharacterStream(int columnIndex,
				      java.io.Reader x,
				      int length
				      ) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateDate(int columnIndex,java.sql.Date x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateDouble(int columnIndex,double x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateFloat(int columnIndex,float x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateInt(int columnIndex,int x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateLong(int columnIndex,long x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateNull(int columnIndex) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateObject(int columnIndex,Object x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateObject(int columnIndex,Object x,int scale) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateRow() throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateShort(int columnIndex,short x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateString(int columnIndex,String x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateTime(int columnIndex,Time x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

    public void updateTimestamp(int columnIndex,Timestamp x) throws SQLException
    {
      // only sub-classes implement CONCUR_UPDATEABLE
      throw org.postgresql.Driver.notImplemented();
    }

}

