package org.postgresql.jdbc1;

// IMPORTANT NOTE: This file implements the JDBC 1 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 2 class in the
// org.postgresql.jdbc2 package.

import java.sql.*;
import java.math.*;

/**
 * CallableStatement is used to execute SQL stored procedures.
 *
 * <p>JDBC provides a stored procedure SQL escape that allows stored
 * procedures to be called in a standard way for all RDBMS's. This escape
 * syntax has one form that includes a result parameter and one that does
 * not. If used, the result parameter must be registered as an OUT
 * parameter. The other parameters may be used for input, output or both.
 * Parameters are refered to sequentially, by number. The first parameter
 * is 1.
 *
 * {?= call <procedure-name>[<arg1>,<arg2>, ...]}                 
 * {call <procedure-name>[<arg1>,<arg2>, ...]}       
 *
 *
 * <p>IN parameter values are set using the set methods inherited from
 * PreparedStatement. The type of all OUT parameters must be registered
 * prior to executing the stored procedure; their values are retrieved
 * after execution via the get methods provided here.
 *
 * <p>A Callable statement may return a ResultSet or multiple ResultSets.
 * Multiple ResultSets are handled using operations inherited from
 * Statement.
 *
 * <p>For maximum portability, a call's ResultSets and update counts should 
 * be processed prior to getting the values of output parameters.        
 *
 * @see Connection#prepareCall
 * @see ResultSet
 */

public class CallableStatement extends PreparedStatement implements java.sql.CallableStatement
{
  /**
   * @exception SQLException on failure
   */
  CallableStatement(Connection c,String q) throws SQLException
  {
    super(c,q);
  }
  
  /**
   * Before executing a stored procedure call you must explicitly
   * call registerOutParameter to register the java.sql.Type of each
   * out parameter.
   *
   * <p>Note: When reading the value of an out parameter, you must use
   * the getXXX method whose Java type XXX corresponds to the
   * parameter's registered SQL type.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @param sqlType SQL type code defined by java.sql.Types; for
   * parameters of type Numeric or Decimal use the version of
   * registerOutParameter that accepts a scale value
   * @exception SQLException if a database-access error occurs.
   */
  public void registerOutParameter(int parameterIndex, int sqlType) throws SQLException {
  }
  
  /**
   * You must also specify the scale for numeric/decimal types:
   *
   * <p>Note: When reading the value of an out parameter, you must use
   * the getXXX method whose Java type XXX corresponds to the
   * parameter's registered SQL type.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @param sqlType use either java.sql.Type.NUMERIC or java.sql.Type.DECIMAL
   * @param scale a value greater than or equal to zero representing the
   * desired number of digits to the right of the decimal point
   * @exception SQLException if a database-access error occurs.
   */
  public void registerOutParameter(int parameterIndex, int sqlType,
				   int scale) throws SQLException
  {
  }
  
  // Old api?
  //public boolean isNull(int parameterIndex) throws SQLException {
  //return true;
  //}
  
  /**
   * An OUT parameter may have the value of SQL NULL; wasNull
   * reports whether the last value read has this special value.
   *
   * <p>Note: You must first call getXXX on a parameter to read its
   * value and then call wasNull() to see if the value was SQL NULL.
   * @return true if the last parameter read was SQL NULL
   * @exception SQLException if a database-access error occurs.
   */
  public boolean wasNull() throws SQLException {
    // check to see if the last access threw an exception
    return false; // fake it for now
  }
  
  // Old api?
  //public String getChar(int parameterIndex) throws SQLException {
  //return null;
  //}
  
  /**
   * Get the value of a CHAR, VARCHAR, or LONGVARCHAR parameter as a
   * Java String.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is null
   * @exception SQLException if a database-access error occurs.
   */
  public String getString(int parameterIndex) throws SQLException {
    return null;
  }
  //public String getVarChar(int parameterIndex) throws SQLException {
  //   return null;
  //}
  
  //public String getLongVarChar(int parameterIndex) throws SQLException {
  //return null;
  //}
  
  /**
   * Get the value of a BIT parameter as a Java boolean.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is false
   * @exception SQLException if a database-access error occurs.
   */
  public boolean getBoolean(int parameterIndex) throws SQLException {
    return false;
  }
  
  /**
   * Get the value of a TINYINT parameter as a Java byte.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is 0
   * @exception SQLException if a database-access error occurs.
   */
  public byte getByte(int parameterIndex) throws SQLException {
    return 0;
  }
  
  /**
   * Get the value of a SMALLINT parameter as a Java short.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is 0
   * @exception SQLException if a database-access error occurs.
   */
  public short getShort(int parameterIndex) throws SQLException {
    return 0;
  }
  
  /**
   * Get the value of an INTEGER parameter as a Java int.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is 0
   * @exception SQLException if a database-access error occurs.
   */
public int getInt(int parameterIndex) throws SQLException {
    return 0;
  }
  
  /**
   * Get the value of a BIGINT parameter as a Java long.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is 0
   * @exception SQLException if a database-access error occurs.
   */
  public long getLong(int parameterIndex) throws SQLException {
    return 0;
  }
  
  /**
   * Get the value of a FLOAT parameter as a Java float.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is 0
   * @exception SQLException if a database-access error occurs.
   */
  public float getFloat(int parameterIndex) throws SQLException {
    return (float) 0.0;
  }
  
  /**
   * Get the value of a DOUBLE parameter as a Java double.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is 0
   * @exception SQLException if a database-access error occurs.
   */
  public double getDouble(int parameterIndex) throws SQLException {
    return 0.0;
  }
  
  /**
   * Get the value of a NUMERIC parameter as a java.math.BigDecimal
   * object.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @param scale a value greater than or equal to zero representing the
   * desired number of digits to the right of the decimal point
   * @return the parameter value; if the value is SQL NULL, the result is null
   * @exception SQLException if a database-access error occurs.
   */
  public BigDecimal getBigDecimal(int parameterIndex, int scale)
       throws SQLException {
	 return null;
  }
  
  /**
   * Get the value of a SQL BINARY or VARBINARY parameter as a Java
   * byte[]
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is null
   * @exception SQLException if a database-access error occurs.
   */
  public byte[] getBytes(int parameterIndex) throws SQLException {
    return null;
  }
  
  // New API (JPM) (getLongVarBinary)
  //public byte[] getBinaryStream(int parameterIndex) throws SQLException {
  //return null;
  //}
  
  /**
   * Get the value of a SQL DATE parameter as a java.sql.Date object
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is null
   * @exception SQLException if a database-access error occurs.
   */
  public java.sql.Date getDate(int parameterIndex) throws SQLException {
    return null;
  }
  
  /**
   * Get the value of a SQL TIME parameter as a java.sql.Time object.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is null
   * @exception SQLException if a database-access error occurs.
   */
  public java.sql.Time getTime(int parameterIndex) throws SQLException {
    return null;
  }
  
  /**
   * Get the value of a SQL TIMESTAMP parameter as a java.sql.Timestamp object.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return the parameter value; if the value is SQL NULL, the result is null
   * @exception SQLException if a database-access error occurs.
   */
  public java.sql.Timestamp getTimestamp(int parameterIndex)
       throws SQLException {
	 return null;
  }
  
  //----------------------------------------------------------------------
  // Advanced features:
  
  // You can obtain a ParameterMetaData object to get information 
  // about the parameters to this CallableStatement.
  //public DatabaseMetaData getMetaData() {
  //return null;
  //}
  
  // getObject returns a Java object for the parameter.
  // See the JDBC spec's "Dynamic Programming" chapter for details.
  /**
   * Get the value of a parameter as a Java object.
   *
   * <p>This method returns a Java object whose type coresponds to the
   * SQL type that was registered for this parameter using
   * registerOutParameter.
   *
   * <P>Note that this method may be used to read datatabase-specific,
   * abstract data types. This is done by specifying a targetSqlType
   * of java.sql.types.OTHER, which allows the driver to return a
   * database-specific Java type.
   *
   * <p>See the JDBC spec's "Dynamic Programming" chapter for details.
   *
   * @param parameterIndex the first parameter is 1, the second is 2,...
   * @return A java.lang.Object holding the OUT parameter value.
   * @exception SQLException if a database-access error occurs.
   */
  public Object getObject(int parameterIndex)
       throws SQLException {
	 return null;
  }
}

