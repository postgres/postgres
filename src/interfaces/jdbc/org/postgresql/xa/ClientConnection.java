/**
 * Redistribution and use of this software and associated documentation
 * ("Software"), with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * 1. Redistributions of source code must retain copyright
 *    statements and notices.  Redistributions must also contain a
 *    copy of this document.
 *
 * 2. Redistributions in binary form must reproduce the
 *    above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 *
 * 3. The name "Exolab" must not be used to endorse or promote
 *    products derived from this Software without prior written
 *    permission of Exoffice Technologies.  For written permission,
 *    please contact info@exolab.org.
 *
 * 4. Products derived from this Software may not be called "Exolab"
 *    nor may "Exolab" appear in their names without prior written
 *    permission of Exoffice Technologies. Exolab is a registered
 *    trademark of Exoffice Technologies.
 *
 * 5. Due credit should be given to the Exolab Project
 *    (http://www.exolab.org/).
 *
 * THIS SOFTWARE IS PROVIDED BY EXOFFICE TECHNOLOGIES AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * EXOFFICE TECHNOLOGIES OR ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Copyright 1999 (C) Exoffice Technologies Inc. All Rights Reserved.
 *
 * $Id: ClientConnection.java,v 1.1 2000/04/17 20:07:55 peter Exp $
 */


package org.postgresql.xa;


import java.util.*;
import java.sql.*;


/**
 * Encapsulates an application's view of an XA/pooled connection.
 * The XA connection is managed by the application server through it's
 * {@link javax.sql.XAConnection} interface. The underlying JDBC
 * connection is a standard JDBC connection. The application's
 * JDBC connection gives access to the underlying JDBC connection but
 * is managed by the application server. The application is given an
 * instance of this class and not the underlying connection directly.
 *
 *
 * @author <a href="arkin@exoffice.com">Assaf Arkin</a>
 * @version 1.0
 * @see XAConnectionImpl
 * @see XADataSourceImpl
 * @see Connection
 */
final class ClientConnection
    implements Connection
{


    /**
     * The pooled XA connection that created this client connection
     * and should be used to report closure and fatal errors.
     */
    private XAConnectionImpl  _xaConn;


    /**
     * This identifier was handed on to use when we were created by
     * {@link XAConnection}. If since then the XA connection was asked
     * to create another connection or was closed, our identifier will
     * no longer be valid and any call to {@link
     * XAConnection#getUnderlying} will throw an exception. Previously,
     * the XA connection would hold a reference to use and tell us to
     * terminate, but that prevented ClientConnection from being
     * finalized.
     */
    private int               _clientId;




    /**
     * Construct a new client connection to provide access to the
     * underlying JDBC connection (<tt>underlying</tt>) on behalf of
     * an XA/pooled connection (<tt>xaConn<tt/>). The pooled connection
     * is required to notify of connection closure and fatal errors.
     *
     * @param xaConn The XA/pooled connection that created this
     *   client connection
     * @param clientId A unique identifier handed to us by
     *   {@link XAConnection}
     * @param underlying The underlying JDBC connection
     */
    ClientConnection( XAConnectionImpl xaConn, int clientId )
    {
	_xaConn = xaConn;
	_clientId = clientId;
    }


    public Statement createStatement()
        throws SQLException
    {
	try {
	    return getUnderlying().createStatement();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public Statement createStatement( int resultSetType, int resultSetConcurrency )
        throws SQLException
    {
	try {
	    return getUnderlying().createStatement( resultSetType, resultSetConcurrency );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public PreparedStatement prepareStatement( String sql )
        throws SQLException
    {
	try {
	    return getUnderlying().prepareStatement( sql );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public PreparedStatement prepareStatement( String sql, int resultSetType, int resultSetConcurrency )
        throws SQLException
    {
	try {
	    return getUnderlying().prepareStatement( sql, resultSetType, resultSetConcurrency );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public CallableStatement prepareCall( String sql )
        throws SQLException
    {
	try {
	    return getUnderlying().prepareCall( sql );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public CallableStatement prepareCall( String sql, int resultSetType, int resultSetConcurrency )
        throws SQLException
    {
	try {
	    return getUnderlying().prepareCall( sql, resultSetType, resultSetConcurrency );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public String nativeSQL( String sql )
        throws SQLException
    {
	try {
	    return getUnderlying().nativeSQL( sql );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public DatabaseMetaData getMetaData()
        throws SQLException
    {
	try {
	    return getUnderlying().getMetaData();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public void setCatalog( String catalog )
        throws SQLException
    {
	try {
	    getUnderlying().setCatalog( catalog );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public String getCatalog()
        throws SQLException
    {
	try {
	    return getUnderlying().getCatalog();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public SQLWarning getWarnings()
        throws SQLException
    {
	try {
	    return getUnderlying().getWarnings();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public void clearWarnings()
        throws SQLException
    {
	try {
	    getUnderlying().clearWarnings();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public Map getTypeMap()
        throws SQLException
    {
	try {
	    return getUnderlying().getTypeMap();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public void setTypeMap( Map map )
        throws SQLException
    {
	try {
	    getUnderlying().setTypeMap( map );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public void setAutoCommit( boolean autoCommit )
        throws SQLException
    {
	// Cannot set auto-commit inside a transaction.
	if ( _xaConn.insideGlobalTx() )
	    throw new SQLException( "Cannot commit/rollback a connection managed by the transaction manager" );
	try {
	    getUnderlying().setAutoCommit( autoCommit );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public boolean getAutoCommit()
        throws SQLException
    {
	try {
	    return getUnderlying().getAutoCommit();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public void commit()
        throws SQLException
    {
	// Cannot commit directly if we're inside a global transaction.
	if ( _xaConn.insideGlobalTx() )
	    throw new SQLException( "Cannot commit/rollback a connection managed by the transaction manager" );
	// Cannot commit a read-only transaction.
	if ( isReadOnly() )
	    throw new SQLException( "Cannot commit/rollback a read-only transaction" );

	// This only occurs if not inside a local transaction.
	try {
	    getUnderlying().commit();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }



    public void rollback()
        throws SQLException
    {
	// Cannot commit directly if we're inside a global transaction.
	if ( _xaConn.insideGlobalTx() )
	    throw new SQLException( "Cannot commit/rollback a connection managed by the transaction manager" );

	// This only occurs if not inside a local transaction.
	try {
	    getUnderlying().rollback();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public void setReadOnly( boolean readOnly )
        throws SQLException
    {
	try {
	    getUnderlying().setReadOnly( readOnly );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public boolean isReadOnly()
        throws SQLException
    {
	try {
	    return getUnderlying().isReadOnly();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }
    

    public void setTransactionIsolation( int level )
        throws SQLException
    {
	try {
	    getUnderlying().setTransactionIsolation( level );
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public int getTransactionIsolation()
        throws SQLException
    {
	try {
	    return getUnderlying().getTransactionIsolation();
	} catch ( SQLException except ) {
	    notifyError( except );
	    throw except;
	}
    }


    public synchronized void close()
	throws SQLException
    {
	if ( _xaConn == null ) 
	    return;

	// Notify the XA connection that we are no longer going
	// to be used. Whether the underlying connection is released,
	// held until the transaction terminates, etc is not
	// a concern of us.
	_xaConn.notifyClose( _clientId );
	_xaConn = null;
    }


    public synchronized boolean isClosed()
    {
	// Simple way of determining if this connection is closed.
	// The actual connection is never closed, it is pooled.
	return ( _xaConn == null );
    }


    /**
     * Called by {@link XAConnectionImpl} to terminate this connection
     * by dissociating it from the underlying JDBC connection.
     * The application would call {@link #close} but {@link
     * XAConnectionImpl} cannot, since pooled connection requirements
     * will cause an inifinite loop. This method should not attempt
     * to notify either a closure or fatal error, but rather throw an
     * exception if it fails.
     */
    /* Deprecated: see XAConnection._clientId
    void terminate()
    {
	_xaConn = null;
    }
    */


    protected void finalize()
	throws Throwable
    {
	close();
    }


    public String toString()
    {
	try {
	    return getUnderlying().toString();
	} catch ( SQLException except ) {
	    return "XAConnection: Connection closed";
	}
    }


    /**
     * Called when an exception is thrown by the underlying connection
     * to determine whether the exception is critical or not. If the
     * exception is critical, notifies the XA connection to forget
     * about this connection.
     *
     * @param except The exception thrown by the underlying
     *   connection
     */
    void notifyError( SQLException except )
    {
	if ( _xaConn != null )
	    _xaConn.notifyError( _clientId, except );
    }


    /**
     * Called to retrieve the underlying JDBC connection. Actual JDBC
     * operations are performed against it. Throws an SQLException if
     * this connection has been closed.
     */
    Connection getUnderlying()
        throws SQLException
    {
	if ( _xaConn == null )
	    throw new SQLException( "This connection has been closed" );
	// Must pass the client identifier so XAConnection can determine
	// whether we are still valid. If it tells us we're no longer
	// valid, we have little to do.
	try {
	    return _xaConn.getUnderlying( _clientId );
	} catch ( SQLException except ) {
	    _xaConn = null;
	    throw except;
	}
    }


}



