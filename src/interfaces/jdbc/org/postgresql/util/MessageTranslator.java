/*-------------------------------------------------------------------------
 *
 * MessageTranslator.java
 *     A singleton class to translate JDBC driver messages in SQLException's.
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/util/Attic/MessageTranslator.java,v 1.5 2003/09/08 17:30:22 barry Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.util;

import java.text.MessageFormat;
import java.util.MissingResourceException;
import java.util.ResourceBundle;

public class MessageTranslator
{

	// The singleton instance.
	private static MessageTranslator instance = null;

	private ResourceBundle bundle;

	private MessageTranslator()
	{
		try
		{
			bundle = ResourceBundle.getBundle("org.postgresql.errors");
		}
		catch (MissingResourceException e)
		{
			// translation files have not been installed.
			bundle = null;
		}
	}

	// Synchronized, otherwise multiple threads may perform the test and
	// assign to the singleton instance simultaneously.
	private synchronized final static MessageTranslator getInstance()
	{
		if (instance == null)
		{
			instance = new MessageTranslator();
		}
		return instance;
	}

	public final static String translate(String id, Object[] args)
	{

		MessageTranslator translator = MessageTranslator.getInstance();

		return translator._translate(id, args);
	}

	public final static String translate(String id, Object arg)
	{
		MessageTranslator translator = MessageTranslator.getInstance();
		Object[] args = new Object[1];
		args[0] = arg;
		return translator._translate(id, args);
	}

	private final String _translate(String id, Object[] args)
	{
		String message;

		if (bundle != null && id != null)
		{
			// Now look up a localized message. If one is not found, then use
			// the supplied message instead.
			try
			{
				message = bundle.getString(id);
			}
			catch (MissingResourceException e)
			{
				message = id;
			}
		}
		else
		{
			message = id;
		}

		// Expand any arguments
		if (args != null && message != null)
		{
			message = MessageFormat.format(message, args);
		}

		return message;
	}
}
