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
 * $Id: XADataSourceImpl.java,v 1.1 2000/04/17 20:07:56 peter Exp $
 */


package org.postgresql.xa;


import java.io.Serializable;
import java.io.PrintWriter;
import java.util.Hashtable;
import java.util.Vector;
import java.util.Stack;
import java.util.Enumeration;
import java.sql.Connection;
import java.sql.SQLException;

import javax.sql.DataSource;
import javax.sql.PooledConnection;
import javax.sql.ConnectionPoolDataSource;
import javax.sql.XAConnection;
import javax.sql.XADataSource;
import javax.transaction.xa.Xid;



/**
 * Implements a JDBC 2.0 {@link XADataSource} for any JDBC driver
 * with JNDI persistance support. The base implementation is actually
 * provided by a different {@link DataSource} class; although this is
 * the super class, it only provides the pooling and XA specific
 * implementation.
 *
 *
 * @author <a href="arkin@exoffice.com">Assaf Arkin</a>
 * @version 1.0
 */
public abstract class XADataSourceImpl
    implements DataSource, ConnectionPoolDataSource,
	       XADataSource, Serializable, Runnable
{


    /**
     * Maps underlying JDBC connections into global transaction Xids.
     */
    private transient Hashtable _txConnections = new Hashtable();


    /**
     * This is a pool of free underlying JDBC connections. If two
     * XA connections are used in the same transaction, the second
     * one will make its underlying JDBC connection available to
     * the pool. This is not a real connection pool, only a marginal
     * efficiency solution for dealing with shared transactions.
     */
    private transient Stack     _pool = new Stack();


    /**
     * A background deamon thread terminating connections that have
     * timed out.
     */
    private transient Thread    _background;


    /**
     * The default timeout for all new transactions.
     */
    private int                 _txTimeout = DEFAULT_TX_TIMEOUT;


    /**
     * The default timeout for all new transactions is 10 seconds.
     */
    public final static int     DEFAULT_TX_TIMEOUT = 10;




    /**
     * Implementation details:
     *   If two XAConnections are associated with the same transaction
     *   (one with a start the other with a join) they must use the
     *   same underlying JDBC connection. They lookup the underlying
     *   JDBC connection based on the transaction's Xid in the
     *   originating XADataSource.
     *
     *   Currently the XADataSource must be the exact same object,
     *   this should be changed so all XADataSources that are equal
     *   share a table of all enlisted connections
     *
     *   To test is two connections should fall under the same
     *   transaction we match the resource managers by comparing the
     *   database/user they fall under using a comparison of the
     *   XADataSource properties.
     */


    public XADataSourceImpl()
    {
	super();

	// Create a background thread that will track transactions
	// that timeout, abort them and release the underlying
	// connections to the pool.
	_background = new Thread( this, "XADataSource Timeout Daemon"  );
	_background.setPriority( Thread.MIN_PRIORITY );
	_background.setDaemon( true );
	_background.start();
    }


    public XAConnection getXAConnection()
        throws SQLException
    {
	// Construct a new XAConnection with no underlying connection.
	// When a JDBC method requires an underlying connection, one
	// will be created. We don't create the underlying connection
	// beforehand, as it might be coming from an existing
	// transaction.
	return new XAConnectionImpl( this, null );
    } 


    public XAConnection getXAConnection( String user, String password )
        throws SQLException
    {
	// Since we create the connection on-demand with newConnection
	// or obtain it from a transaction, we cannot support XA
	// connections with a caller specified user name.
	throw new SQLException( "XAConnection does not support connections with caller specified user name" );
    }
    
    
    public PooledConnection getPooledConnection()
        throws SQLException
    {
	// Construct a new pooled connection and an underlying JDBC
	// connection to go along with it.
	return new XAConnectionImpl( this, getConnection() );
    }
    
    
    public PooledConnection getPooledConnection( String user, String password )
        throws SQLException
    {
	// Construct a new pooled connection and an underlying JDBC
	// connection to go along with it.
	return new XAConnectionImpl( this, getConnection( user, password ) );
    }


    /**
     * Returns the default timeout for all transactions.
     */
    public int getTransactionTimeout()
    {
	return _txTimeout;
    }


    /**
     * This method is defined in the interface and implemented in the
     * derived class, we re-define it just to make sure it does not
     * throw an {@link SQLException} and that we do not need to
     * catch one.
     */
    public abstract java.io.PrintWriter getLogWriter();


    /**
     * Sets the default timeout for all transactions. The timeout is
     * specified in seconds. Use zero for the default timeout. Calling
     * this method does not affect transactions in progress.
     *
     * @param seconds The timeout in seconds
     */
    public void setTransactionTimeout( int seconds )
    {
	if ( seconds <= 0 )
	    _txTimeout = DEFAULT_TX_TIMEOUT;
	else
	    _txTimeout = seconds;
	_background.interrupt();
    }
    

    /**
     * Returns an underlying connection for the global transaction,
     * if one has been associated before.
     *
     * @param xid The transaction Xid
     * @return A connection associated with that transaction, or null
     */
    TxConnection getTxConnection( Xid xid )
    {
	return (TxConnection) _txConnections.get( xid );
    }


    /**
     * Associates the global transaction with an underlying connection,
     * or dissociate it when null is passed.
     *
     * @param xid The transaction Xid
     * @param conn The connection to associate, null to dissociate
     */
    TxConnection setTxConnection( Xid xid, TxConnection txConn )
    {
	if ( txConn == null )
	    return (TxConnection) _txConnections.remove( xid );
	else
	    return (TxConnection) _txConnections.put( xid, txConn );
    }


    /**
     * Release an unused connection back to the pool. If an XA
     * connection has been asked to join an existing transaction,
     * it will no longer use it's own connection and make it available
     * to newly created connections.
     *
     * @param conn An open connection that is no longer in use
     */
    void releaseConnection( Connection conn )
    {
	_pool.push( conn );
    }


    /**
     * Creates a new underlying connection. Used by XA connection
     * that lost it's underlying connection when joining a
     * transaction and is now asked to produce a new connection.
     *
     * @return An open connection ready for use
     * @throws SQLException An error occured trying to open
     *   a connection
     */
    Connection newConnection()
	throws SQLException
    {
	Connection conn;

	// Check in the pool first.
	if ( ! _pool.empty() ) {
	    conn = (Connection) _pool.pop();
	    return conn;
	}
	return getConnection();
    }


    /**
     * XXX Not fully implemented yet and no code to really
     *     test it.
     */
    Xid[] getTxRecover()
    {
	Vector       list;
	Enumeration  enum;
	TxConnection txConn;

	list = new Vector();
	enum = _txConnections.elements();
	while ( enum.hasMoreElements() ) {
	    txConn = (TxConnection) enum.nextElement();
	    if ( txConn.conn != null && txConn.prepared )
		list.add( txConn.xid );
	}
	return (Xid[]) list.toArray();
    }


    /**
     * Returns the transaction isolation level to use with all newly
     * created transactions, or {@link Connection#TRANSACTION_NONE}
     * if using the driver's default isolation level.
     */
    public int isolationLevel()
    {
	return Connection.TRANSACTION_NONE;
    }


    public void run()
    {
	Enumeration  enum;
	int          reduce;
	long         timeout;
	TxConnection txConn;

	while ( true ) {
	    // Go to sleep for the duration of a transaction
	    // timeout. This mean transactions will timeout on average
	    // at _txTimeout * 1.5.
	    try {
		Thread.sleep( _txTimeout * 1000 );
	    } catch ( InterruptedException except ) {
	    }

	    try {
		// Check to see if there are any pooled connections
		// we can release. We release 10% of the pooled
                // connections each time, so in a heavy loaded
                // environment we don't get to release that many, but
                // as load goes down we do. These are not actually
                // pooled connections, but connections that happen to
                // get in and out of a transaction, not that many.
		reduce = _pool.size() - ( _pool.size() / 10 ) - 1;
		if ( reduce >= 0 && _pool.size() > reduce ) {
		    if ( getLogWriter() != null )
			getLogWriter().println( "DataSource " + toString() +
						": Reducing internal connection pool size from " +
						_pool.size() + " to " + reduce );
		    while ( _pool.size() > reduce ) {
			try {
			    ( (Connection) _pool.pop() ).close();
			} catch ( SQLException except ) { }
		    }
		}
	    } catch ( Exception except ) { }
		
	    // Look for all connections inside a transaction that
	    // should have timed out by now.
	    timeout = System.currentTimeMillis();
	    enum = _txConnections.elements();
	    while ( enum.hasMoreElements() ) {
		txConn = (TxConnection) enum.nextElement();
		// If the transaction timed out, we roll it back and
		// invalidate it, but do not remove it from the transaction
		// list yet. We wait for the next iteration, minimizing the
		// chance of a NOTA exception.
		if ( txConn.conn == null ) {
		    _txConnections.remove( txConn.xid );
		    // Chose not to use an iterator so we must
		    // re-enumerate the list after removing
		    // an element from it.
		    enum = _txConnections.elements();
		} else if ( txConn.timeout < timeout ) {
		    
		    try {
			Connection underlying;
			
			synchronized ( txConn ) {
			    if ( txConn.conn == null )
				continue;
			    if ( getLogWriter() != null )
				getLogWriter().println( "DataSource " + toString() +
							": Transaction timed out and being aborted: " +
							txConn.xid );
			    // Remove the connection from the transaction
			    // association. XAConnection will now have
			    // no underlying connection and attempt to
			    // create a new one.
			    underlying = txConn.conn;
			    txConn.conn = null;
			    txConn.timedOut = true;
			    
			    // Rollback the underlying connection to
			    // abort the transaction and release the
			    // underlying connection to the pool.
			    try {
				underlying.rollback();
				releaseConnection( underlying );
			    } catch ( SQLException except ) {
				if ( getLogWriter() != null )
				    getLogWriter().println( "DataSource " + toString() +
							    ": Error aborting timed out transaction: " + except );
				try {
				    underlying.close();
				} catch ( SQLException e2 ) { }
			    }
			}
		    } catch ( Exception except ) { }
		    
		}
	    }
	}
    }



    public void debug( PrintWriter writer )
    {
	Enumeration  enum;
	TxConnection txConn;
	StringBuffer buffer;

	writer.println( "Debug info for XADataSource:" );
	enum = _txConnections.elements();
	if ( ! enum.hasMoreElements() )
	    writer.println( "Empty" );
	while ( enum.hasMoreElements() ) {
	    buffer = new StringBuffer();
	    txConn = (TxConnection) enum.nextElement();
	    buffer.append( "TxConnection " );
	    if ( txConn.xid != null )
		buffer.append( txConn.xid );
	    if ( txConn.conn != null )
		buffer.append( ' ' ).append( txConn.conn );
	    buffer.append( " count: " ).append( txConn.count );
	    if ( txConn.prepared )
		buffer.append( " prepared" );
	    if ( txConn.timedOut )
		buffer.append( " timed-out" );
	    if ( txConn.readOnly )
		buffer.append( " read-only" );
	    writer.println( buffer.toString() );
	}
	enum = _pool.elements();
	while ( enum.hasMoreElements() )
	    writer.println( "Pooled underlying: " + enum.nextElement().toString() );
    }

    
}
