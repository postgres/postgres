package org.postgresql.jdbc3;

import org.postgresql.jdbc2.optional.PooledConnectionImpl;

import java.sql.Connection;

/**
 * JDBC3 implementation of PooledConnection, which manages
 * a connection in a connection pool.
 *
 * @author Aaron Mulder (ammulder@alumni.princeton.edu)
 * @version $Revision: 1.1 $
 */
public class Jdbc3PooledConnection extends PooledConnectionImpl
{
    Jdbc3PooledConnection(Connection con, boolean autoCommit)
    {
        super(con, autoCommit);
    }
}
