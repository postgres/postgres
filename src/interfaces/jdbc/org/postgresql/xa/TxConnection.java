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
 * $Id: TxConnection.java,v 1.1 2000/04/17 20:07:56 peter Exp $
 */


package org.postgresql.xa;


import java.sql.Connection;
import javax.transaction.xa.Xid;


/**
 * Describes an open connection associated with a transaction. When a
 * transaction is opened for a connection, this record is created for
 * the connection. It indicates the underlying JDBC connection and
 * transaction Xid. Multiple XA connection that fall under the same
 * transaction Xid will share the same TxConnection object.
 *
 *
 * @author <a href="arkin@exoffice.com">Assaf Arkin</a>
 * @version 1.0
 * @see Xid
 * @see XAConnectionImpl
 */
final class TxConnection
{
    
    
    /**
     * The Xid of the transactions. Connections that are not
     * associated with a transaction are not represented here.
     */
    Xid                        xid;
    
    
    /**
     * Holds the underlying JDBC connection for as long as this
     * connection is useable. If the connection has been rolled back,
     * timed out or had any other error, this variable will null
     * and the connection is considered failed.
     */
    Connection                 conn;
    
    

    /**
     * Indicates the clock time (in ms) when the transaction should
     * time out. The transaction times out when
     * <tt>System.currentTimeMillis() > timeout</tt>.
     */ 
    long                       timeout;


    /**
     * Indicates the clock time (in ms) when the transaction started.
     */ 
    long                       started;
    
    
    /**
     * Reference counter indicates how many XA connections share this
     * underlying connection and transaction. Always one or more.
     */
    int                        count;
    
    
    /**
     * True if the transaction has failed due to time out.
     */
    boolean                   timedOut;


    /**
     * True if the transaction has already been prepared.
     */
    boolean                   prepared;


    /**
     * True if the transaction has been prepared and found out to be
     * read-only. Read-only transactions do not require commit/rollback.
     */
    boolean                   readOnly;
    
    
}

