package postgresql;

import java.math.*;
import java.sql.*;

/**
 * @version 1.0 15-APR-1997
 * @author <A HREF="mailto:adrian@hottub.org">Adrian Hall</A>
 *
 * CallableStatement is used to execute SQL stored procedures.
 *
 * JDBC provides a stored procedure SQL escape that allows stored procedures
 * to be called in a standard way for all RDBMS's.  This escape syntax has
 * one form that includes a result parameter and one that does not.  If used,
 * the result parameter must be generated as an OUT parameter.  The other
 * parameters may be used for input, output or both.  Parameters are refered
 * to sequentially, by number.  The first parameter is 1.
 *
 * <PRE>
 *	{?= call <procedure-name>[<arg1>,<arg2>, ...]}
 *	{call <procedure-name>[<arg1>,<arg2>, ...]}
 * </PRE>
 *
 * IN parameters are set using the set methods inherited from 
 * PreparedStatement.  The type of all OUT parameters must be registered
 * prior to executing the stored procedure; their values are retrieved
 * after execution via the get methods provided here.
 *
 * A CallableStatement may return a ResultSet or multiple ResultSets.  Multiple
 * ResultSets are handled using operations inherited from Statement.
 *
 * For maximum portability, a call's ResultSets and update counts should be
 * processed prior to getting the values of output parameters.
 *
 * @see java.sql.Connection#prepareCall
 * @see java.sql.ResultSet
 * @see java.sql.CallableStatement
 */
public class CallableStatement implements java.sql.CallableStatement 
{
	public void registerOutParameter (int paramterIndex, int sqlType) throws SQLException
	{
		// XXX-Not Implemented
	}

	public void registerOutParameter (int parameterIndex, int sqlType, int scale) throws SQLException
	{
		// XXX-Not Implemented
	}

	public boolean wasNull () throws SQLException
	{
		// XXX-Not Implemented
	}

	public String getString (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public boolean getBoolean (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public byte getByte (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public short getShort (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public int getInt (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public long getLong (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public float getFloat (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public double getDouble (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public BigDecimal getBigDecimal (int parameterIndex, int scale) throws SQLException
	{
		// XXX-Not Implemented
	}

	public byte[] getBytes (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public Date getDate (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public Time getTime (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public Timestamp getTimestamp (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

	public Object getObject (int parameterIndex) throws SQLException
	{
		// XXX-Not Implemented
	}

}
