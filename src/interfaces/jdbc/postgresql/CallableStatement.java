package postgresql;

import java.sql.*;
import java.math.*;

/**
 * JDBC Interface to Postgres95 functions
 */

// Copy methods from the Result set object here.

public class CallableStatement extends PreparedStatement implements java.sql.CallableStatement
{
  CallableStatement(Connection c,String q) throws SQLException
  {
    super(c,q);
  }
  
  // Before executing a stored procedure call you must explicitly
  // call registerOutParameter to register the java.sql.Type of each
  // out parameter.
  public void registerOutParameter(int parameterIndex, int sqlType) throws SQLException {
  }
  
  // You must also specify the scale for numeric/decimal types:	
  public void registerOutParameter(int parameterIndex, int sqlType,
				   int scale) throws SQLException
  {
  }
  
  public boolean isNull(int parameterIndex) throws SQLException {
    return true;
  }
  
  // New API (JPM)
  public boolean wasNull() throws SQLException {
    // check to see if the last access threw an exception
    return false; // fake it for now
  }
  
  // Methods for retrieving OUT parameters from this statement.
  public String getChar(int parameterIndex) throws SQLException {
    return null;
  }
  
  // New API (JPM)
  public String getString(int parameterIndex) throws SQLException {
    return null;
  }
  //public String getVarChar(int parameterIndex) throws SQLException {
  //   return null;
  //}
  
  public String getLongVarChar(int parameterIndex) throws SQLException {
    return null;
  }
  
  // New API (JPM) (getBit)
  public boolean getBoolean(int parameterIndex) throws SQLException {
    return false;
  }
  
  // New API (JPM) (getTinyInt)
  public byte getByte(int parameterIndex) throws SQLException {
    return 0;
  }
  
  // New API (JPM) (getSmallInt)
  public short getShort(int parameterIndex) throws SQLException {
    return 0;
  }
  
  // New API (JPM) (getInteger)
  public int getInt(int parameterIndex) throws SQLException {
    return 0;
  }
  
  // New API (JPM) (getBigInt)
  public long getLong(int parameterIndex) throws SQLException {
    return 0;
  }
  
  public float getFloat(int parameterIndex) throws SQLException {
    return (float) 0.0;
  }
  
  public double getDouble(int parameterIndex) throws SQLException {
    return 0.0;
  }
  
  public BigDecimal getBigDecimal(int parameterIndex, int scale)
       throws SQLException {
	 return null;
  }
  
  // New API (JPM) (getBinary)
  public byte[] getBytes(int parameterIndex) throws SQLException {
    return null;
  }
  
  // New API (JPM) (getLongVarBinary)
  public byte[] getBinaryStream(int parameterIndex) throws SQLException {
    return null;
  }
  
  public java.sql.Date getDate(int parameterIndex) throws SQLException {
    return null;
  }
  public java.sql.Time getTime(int parameterIndex) throws SQLException {
    return null;
  }
  public java.sql.Timestamp getTimestamp(int parameterIndex)
       throws SQLException {
	 return null;
  }
  
  //----------------------------------------------------------------------
  // Advanced features:
  
  // You can obtain a ParameterMetaData object to get information 
  // about the parameters to this CallableStatement.
  public DatabaseMetaData getMetaData() {
    return null;
  }
  
  // getObject returns a Java object for the parameter.
  // See the JDBC spec's "Dynamic Programming" chapter for details.
  public Object getObject(int parameterIndex)
       throws SQLException {
	 return null;
  }
}

