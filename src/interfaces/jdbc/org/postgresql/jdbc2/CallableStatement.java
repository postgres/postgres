package org.postgresql.jdbc2;

// IMPORTANT NOTE: This file implements the JDBC 2 version of the driver.
// If you make any modifications to this file, you must make sure that the
// changes are also made (if relevent) to the related JDBC 1 class in the
// org.postgresql.jdbc1 package.

import java.sql.*;
import java.math.*;
import org.postgresql.util.*;
/*
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
 * @author Paul Bethe (implementer)
 */

public class CallableStatement extends org.postgresql.jdbc2.PreparedStatement implements java.sql.CallableStatement
{
	/*
	 * @exception SQLException on failure
	 */
	public CallableStatement(Jdbc2Connection c, String q) throws SQLException
	{
		super(c, q); // don't parse yet..
	}


	/**
	 * allows this class to tweak the standard JDBC call !see Usage
	 * -> and replace with the pgsql function syntax
	 * ie. select <function ([params])> as RESULT;
	 */

	protected void parseSqlStmt () throws SQLException {
		modifyJdbcCall ();
		super.parseSqlStmt ();
	}
	/** 
	 * this method will turn a string of the form
	 * {? = call <some_function> (?, [?,..]) }
	 * into the PostgreSQL format which is 
	 * select <some_function> (?, [?, ...]) as result
	 * 
	 */
	private void modifyJdbcCall () throws SQLException {
		// syntax checking is not complete only a few basics :(
		originalSql = sql; // save for error msgs..
		int index = sql.indexOf ("="); // is implied func or proc?
		boolean isValid = true;
		if (index != -1) {
			isFunction = true;
			isValid = sql.indexOf ("?") < index; // ? before =			
		}
		sql = sql.trim ();
		if (sql.startsWith ("{") && sql.endsWith ("}")) {
			sql = sql.substring (1, sql.length() -1);
		} else isValid = false;
		index = sql.indexOf ("call"); 
		if (index == -1 || !isValid)
			throw new PSQLException ("postgresql.call.malformed", 
									 new Object[]{sql, JDBC_SYNTAX});
		sql = sql.replace ('{', ' '); // replace these characters
		sql = sql.replace ('}', ' ');
		sql = sql.replace (';', ' ');
		
		// this removes the 'call' string and also puts a hidden '?'
		// at the front of the line for functions, this will
		// allow the registerOutParameter to work correctly
                // because in the source sql there was one more ? for the return
                // value that is not needed by the postgres syntax.  But to make 
                // sure that the parameter numbers are the same as in the original
                // sql we add a dummy parameter in this case
		sql = (isFunction ? "?" : "") + sql.substring (index + 4);
		
		sql = "select " + sql + " as " + RESULT_COLUMN + ";";	  
	}

	// internals 
	static final String JDBC_SYNTAX = "{[? =] call <some_function> ([? [,?]*]) }";
	static final String RESULT_COLUMN = "result";
	String originalSql = "";
	boolean isFunction;
	// functionReturnType contains the user supplied value to check
	// testReturn contains a modified version to make it easier to 
	// check the getXXX methods..
	int functionReturnType;
	int testReturn;
	// returnTypeSet is true when a proper call to registerOutParameter has been made
	boolean returnTypeSet;
	Object result;

	/*
	 * Before executing a stored procedure call you must explicitly
	 * call registerOutParameter to register the java.sql.Type of each
	 * out parameter.
	 *
	 * <p>Note: When reading the value of an out parameter, you must use
	 * the getXXX method whose Java type XXX corresponds to the
	 * parameter's registered SQL type.
	 *
	 * ONLY 1 RETURN PARAMETER if {?= call ..} syntax is used
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @param sqlType SQL type code defined by java.sql.Types; for
	 * parameters of type Numeric or Decimal use the version of
	 * registerOutParameter that accepts a scale value
	 * @exception SQLException if a database-access error occurs.
	 */
	public void registerOutParameter(int parameterIndex, int sqlType) throws SQLException
		{
			if (parameterIndex != 1)
				throw new PSQLException ("postgresql.call.noinout");
			if (!isFunction)
				throw new PSQLException ("postgresql.call.procasfunc", originalSql);
			
			// functionReturnType contains the user supplied value to check
			// testReturn contains a modified version to make it easier to 
			// check the getXXX methods..
			functionReturnType = sqlType;
			testReturn = sqlType;
			if (functionReturnType == Types.CHAR || 
				functionReturnType == Types.LONGVARCHAR)
				testReturn = Types.VARCHAR;
			else if (functionReturnType == Types.FLOAT)
				testReturn = Types.REAL; // changes to streamline later error checking
			returnTypeSet = true;
		}

	/**
	 * allow calls to execute update
	 * @return 1 if succesful call otherwise 0
	 */
	public int executeUpdate() throws SQLException
	{
		java.sql.ResultSet rs = super.executeQuery (compileQuery());	
		if (isFunction) {
			if (!rs.next ())
				throw new PSQLException ("postgresql.call.noreturnval");
			result = rs.getObject(1);
			int columnType = rs.getMetaData().getColumnType(1);
			if (columnType != functionReturnType) 
				throw new PSQLException ("postgresql.call.wrongrtntype",
										 new Object[]{
					getSqlTypeName (columnType), getSqlTypeName (functionReturnType) }); 
		}
		rs.close ();
		return 1;
	}


	/**
	 * allow calls to execute update
	 * @return true if succesful
	 */
	public boolean execute() throws SQLException
	{
		return (executeUpdate() == 1);
	}

	/*
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
			registerOutParameter (parameterIndex, sqlType); // ignore for now..
		}

	/*
	 * override this method to check for set @ 1 when declared function..
	 *
	 * @param paramIndex the index into the inString
	 * @param s a string to be stored
	 * @exception SQLException if something goes wrong
	 */
	protected void set(int paramIndex, String s) throws SQLException
	{
		if (paramIndex == 1 && isFunction) // need to registerOut instead
			throw new PSQLException ("postgresql.call.funcover");
		super.set (paramIndex, s); // else set as usual..
	}

		/*
	 * Helper - this compiles the SQL query from the various parameters
	 * This is identical to toString() except it throws an exception if a
	 * parameter is unused.
	 */
	protected synchronized String compileQuery()
	throws SQLException
	{
		if (isFunction && !returnTypeSet)
			throw new PSQLException("postgresql.call.noreturntype");
		if (isFunction) { // set entry 1 to dummy entry..
			inStrings[0] = ""; // dummy entry which ensured that no one overrode
			// and calls to setXXX (2,..) really went to first arg in a function call..
		}
		return super.compileQuery ();
	}

	/*
	 * An OUT parameter may have the value of SQL NULL; wasNull
	 * reports whether the last value read has this special value.
	 *
	 * <p>Note: You must first call getXXX on a parameter to read its
	 * value and then call wasNull() to see if the value was SQL NULL.
	 * @return true if the last parameter read was SQL NULL
	 * @exception SQLException if a database-access error occurs.
	 */
	public boolean wasNull() throws SQLException
	{
		// check to see if the last access threw an exception
		return (result == null);
	}

	/*
	 * Get the value of a CHAR, VARCHAR, or LONGVARCHAR parameter as a
	 * Java String.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public String getString(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.VARCHAR, "String");
		return (String)result;
	}
	//public String getVarChar(int parameterIndex) throws SQLException {
	//	 return null;
	//}

	//public String getLongVarChar(int parameterIndex) throws SQLException {
	//return null;
	//}

	/*
	 * Get the value of a BIT parameter as a Java boolean.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is false
	 * @exception SQLException if a database-access error occurs.
	 */
	public boolean getBoolean(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.BIT, "Boolean");
		if (result == null) return false;
		return ((Boolean)result).booleanValue ();
	}

	/*
	 * Get the value of a TINYINT parameter as a Java byte.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public byte getByte(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.TINYINT, "Byte");
		if (result == null) return 0;
		return (byte)((Integer)result).intValue ();
	}

	/*
	 * Get the value of a SMALLINT parameter as a Java short.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public short getShort(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.SMALLINT, "Short");
		if (result == null) return 0;
		return (short)((Integer)result).intValue ();
	}
		

	/*
	 * Get the value of an INTEGER parameter as a Java int.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public int getInt(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.INTEGER, "Int");
		if (result == null) return 0;
		return ((Integer)result).intValue ();
	}

	/*
	 * Get the value of a BIGINT parameter as a Java long.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public long getLong(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.BIGINT, "Long");
		if (result == null) return 0;
		return ((Long)result).longValue ();
	}

	/*
	 * Get the value of a FLOAT parameter as a Java float.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public float getFloat(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.REAL, "Float");
		if (result == null) return 0;
		return ((Float)result).floatValue ();
	}

	/*
	 * Get the value of a DOUBLE parameter as a Java double.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is 0
	 * @exception SQLException if a database-access error occurs.
	 */
	public double getDouble(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.DOUBLE, "Double");
		if (result == null) return 0;
		return ((Double)result).doubleValue ();
	}

	/*
	 * Get the value of a NUMERIC parameter as a java.math.BigDecimal
	 * object.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @param scale a value greater than or equal to zero representing the
	 * desired number of digits to the right of the decimal point
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 * @deprecated in Java2.0
	 */
	public BigDecimal getBigDecimal(int parameterIndex, int scale)
	throws SQLException
	{
		checkIndex (parameterIndex, Types.NUMERIC, "BigDecimal");
		return ((BigDecimal)result);
	}

	/*
	 * Get the value of a SQL BINARY or VARBINARY parameter as a Java
	 * byte[]
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public byte[] getBytes(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.VARBINARY, "Bytes");
		return ((byte [])result);
	}

	// New API (JPM) (getLongVarBinary)
	//public byte[] getBinaryStream(int parameterIndex) throws SQLException {
	//return null;
	//}

	/*
	 * Get the value of a SQL DATE parameter as a java.sql.Date object
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public java.sql.Date getDate(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.DATE, "Date");
		return (java.sql.Date)result;
	}

	/*
	 * Get the value of a SQL TIME parameter as a java.sql.Time object.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public java.sql.Time getTime(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.TIME, "Time");
		return (java.sql.Time)result;
	}

	/*
	 * Get the value of a SQL TIMESTAMP parameter as a java.sql.Timestamp object.
	 *
	 * @param parameterIndex the first parameter is 1, the second is 2,...
	 * @return the parameter value; if the value is SQL NULL, the result is null
	 * @exception SQLException if a database-access error occurs.
	 */
	public java.sql.Timestamp getTimestamp(int parameterIndex)
	throws SQLException
	{
		checkIndex (parameterIndex, Types.TIMESTAMP, "Timestamp");
		return (java.sql.Timestamp)result;
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
	/*
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
	throws SQLException
	{
		checkIndex (parameterIndex);		
		return result;
	}

	// ** JDBC 2 Extensions **

	public java.sql.Array getArray(int i) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public java.math.BigDecimal getBigDecimal(int parameterIndex) throws SQLException
	{
		checkIndex (parameterIndex, Types.NUMERIC, "BigDecimal");
		return ((BigDecimal)result);
	}

	public Blob getBlob(int i) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public Clob getClob(int i) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public Object getObject(int i, java.util.Map map) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public Ref getRef(int i) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public java.sql.Date getDate(int i, java.util.Calendar cal) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public Time getTime(int i, java.util.Calendar cal) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	public Timestamp getTimestamp(int i, java.util.Calendar cal) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}

	// no custom types allowed yet..
	public void registerOutParameter(int parameterIndex, int sqlType, String typeName) throws SQLException
	{
		throw org.postgresql.Driver.notImplemented();
	}



	/** helperfunction for the getXXX calls to check isFunction and index == 1
	 */
	private void checkIndex (int parameterIndex, int type, String getName) 
		throws SQLException {
		checkIndex (parameterIndex);
		if (type != this.testReturn) 
			throw new PSQLException("postgresql.call.wrongget",
									new Object[]{getSqlTypeName (testReturn),
													 getName,
													 getSqlTypeName (type)});
	}
	/** helperfunction for the getXXX calls to check isFunction and index == 1
	 * @param parameterIndex index of getXXX (index)
	 * check to make sure is a function and index == 1
	 */
	private void checkIndex (int parameterIndex) throws SQLException {
		if (!isFunction)
			throw new PSQLException("postgresql.call.noreturntype");
		if (parameterIndex != 1)
			throw new PSQLException("postgresql.call.noinout");
	}
		
	/** helper function for creating msg with type names
	 * @param sqlType a java.sql.Types.XX constant
	 * @return String which is the name of the constant..
	 */
	private static String getSqlTypeName (int sqlType) {
		switch (sqlType)
			{
			case Types.BIT:
				return "BIT";
			case Types.SMALLINT:
				return "SMALLINT";
			case Types.INTEGER:
				return "INTEGER";
			case Types.BIGINT:
				return "BIGINT";
			case Types.NUMERIC:
				return "NUMERIC";
			case Types.REAL:
				return "REAL";
			case Types.DOUBLE:
				return "DOUBLE";
			case Types.FLOAT:
				return "FLOAT";
			case Types.CHAR:
				return "CHAR";
			case Types.VARCHAR:
				return "VARCHAR";
			case Types.DATE:
				return "DATE";
			case Types.TIME:
				return "TIME";
			case Types.TIMESTAMP:
				return "TIMESTAMP";
			case Types.BINARY:
				return "BINARY";
			case Types.VARBINARY:
				return "VARBINARY";
			default:
				return "UNKNOWN";
			}
	}
}

