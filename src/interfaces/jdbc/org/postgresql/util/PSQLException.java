/*-------------------------------------------------------------------------
 *
 * PSQLException.java
 *     This class extends SQLException, and provides our internationalisation
 *     handling
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/util/Attic/PSQLException.java,v 1.12 2003/08/11 21:18:47 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.util;

import java.io.ByteArrayOutputStream;
import java.io.PrintWriter;
import java.sql.SQLException;
import org.postgresql.Driver;

public class PSQLException extends SQLException
{
	private String message;

	/*
	 * This provides the same functionality to SQLException
	 * @param error Error string
	 */
	public PSQLException(String error)
	{
		super();
		translate(error, null);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}

	/*
	 * A more generic entry point.
	 * @param error Error string or standard message id
	 * @param args Array of arguments
	 */
	public PSQLException(String error, Object[] args)
	{
		super();
		translate(error, args);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}

	/*
	 * Helper version for 1 arg
	 */
	public PSQLException(String error, Object arg)
	{
		super();
		Object[] argv = new Object[1];
		argv[0] = arg;
		translate(error, argv);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}

	/*
	 * Helper version for 1 arg. This is used for debug purposes only with
	 * some unusual Exception's. It allows the originiating Exceptions stack
	 * trace to be returned.
	 */
	public PSQLException(String error, Exception ex)
	{
		super();

		Object[] argv = new Object[1];

		try
		{
			ByteArrayOutputStream baos = new ByteArrayOutputStream();
			PrintWriter pw = new PrintWriter(baos);
			pw.println("Exception: " + ex.toString() + "\nStack Trace:\n");
			ex.printStackTrace(pw);
			pw.println("End of Stack Trace");
			pw.flush();
			argv[0] = baos.toString();
			pw.close();
			baos.close();
		}
		catch (Exception ioe)
		{
			argv[0] = ex.toString() + "\nIO Error on stack trace generation! " + ioe.toString();
		}

		translate(error, argv);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}

	/*
	 * Helper version for 2 args
	 */
	public PSQLException(String error, Object arg1, Object arg2)
	{
		super();
		Object[] argv = new Object[2];
		argv[0] = arg1;
		argv[1] = arg2;
		translate(error, argv);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}

	private void translate(String error, Object[] args)
	{
		message = MessageTranslator.translate(error, args);
	}

	/*
	 * Overides Throwable
	 */
	public String getLocalizedMessage()
	{
		return message;
	}

	/*
	 * Overides Throwable
	 */
	public String getMessage()
	{
		return message;
	}
}
