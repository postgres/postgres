package org.postgresql;

import java.sql.*;
import java.util.Properties;
import java.util.Vector;
import org.postgresql.core.Encoding;
import org.postgresql.fastpath.Fastpath;
import org.postgresql.largeobject.LargeObjectManager;

/* $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/Attic/PGConnection.java,v 1.1 2002/07/23 03:59:55 barry Exp $
 * This interface defines PostgreSQL extentions to the java.sql.Connection interface.
 * Any java.sql.Connection object returned by the driver will also implement this 
 * interface
 */
public interface PGConnection
{
    /*
     * Get the character encoding to use for this connection.
     */
    public Encoding getEncoding() throws SQLException;

    /* 
     * This method returns the java.sql.Types type for a postgres datatype name
     */
    public int getSQLType(String pgTypeName) throws SQLException;

    /*
     * This returns the java.sql.Types type for a postgres datatype OID
     */
    public int getSQLType(int oid) throws SQLException;

    /*
     * This returns the postgres datatype name from the 
     * postgres datatype OID
     */
    public String getPGType(int oid) throws SQLException;

    /*
     * This returns the postgres datatype OID from the
     * postgres datatype name
     */
    public int getPGType(String typeName) throws SQLException;

    /*
     * This returns the LargeObject API for the current connection.
     */
    public LargeObjectManager getLargeObjectAPI() throws SQLException;

    /*
     * This returns the Fastpath API for the current connection.
     */
    public Fastpath getFastpathAPI() throws SQLException;

    /*
     * This method is used internally to return an object based around
     * org.postgresql's more unique data types.
     *
     * <p>It uses an internal Hashtable to get the handling class. If the
     * type is not supported, then an instance of org.postgresql.util.PGobject
     * is returned.
     *
     * You can use the getValue() or setValue() methods to handle the returned
     * object. Custom objects can have their own methods.
     *
     * @return PGobject for this type, and set to value
     * @exception SQLException if value is not correct for this type
     * @see org.postgresql.util.Serialize
     */
    public Object getObject(String type, String value) throws SQLException;

}

