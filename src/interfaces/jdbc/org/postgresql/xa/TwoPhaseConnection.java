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
 * $Id: TwoPhaseConnection.java,v 1.1 2000/04/17 20:07:55 peter Exp $
 */


package org.postgresql.xa;


import java.sql.SQLException;


/**
 * Defines two-phase commit support for a JDBC connection used by
 * {@link XAConnection}. A JDBC connection that can implement any of
 * these features should extend this interface and attempt to
 * implement as much as it can.
 * <p>
 * {@link #prepare} is used as part of the two phase commit protocol
 * to determine whether the transaction can commit or must rollback.
 * Failure to implement this method will cause all connections to vote
 * for commit, whether or not they can actually commit, leading to
 * mixed heuristics.
 * <p>
 * {@link #enableSQLTransactions} allows the SQL begin/commit/rollback
 * commands to be disabled for the duration of a transaction managed
 * through an {@link javax.transaction.xaXAResource}, preventing the
 * application from demarcating transactions directly.
 * <p>
 * {@link #isCriticalError} is used to tell if an exception thrown by
 * the connection is fatal and the connection should not be returned
 * to the pool.
 *
 *
 * @author <a href="arkin@exoffice.com">Assaf Arkin</a>
 * @version 1.0
 */
public interface TwoPhaseConnection
{


    /**
     * Enables or disables transaction demarcation through SQL commit
     * and rollback. When the connection falls under control of
     * {@link XAConnection}, SQL commit/rollback commands will be
     * disabled to prevent direct transaction demarcation.
     *
     * @param flag True to enable SQL transactions (the default)
     */
    public void enableSQLTransactions( boolean flag );


    /**
     * Called to prepare the transaction for commit. Returns true if
     * the transaction is prepared, false if the transaction is
     * read-only. If the transaction has been marked for rollback,
     * throws a {@link RollbackException}.
     *
     * @return True if can commit, false if read-only
     * @throws SQLException If transaction has been marked for
     *   rollback or cannot commit for any other reason
     */
    public boolean prepare()
	throws SQLException;


    /**
     * Returns true if the error issued by this connection is a
     * critical error and the connection should be terminated.
     *
     * @param except The exception thrown by this connection
     */
    public boolean isCriticalError( SQLException except );


}
