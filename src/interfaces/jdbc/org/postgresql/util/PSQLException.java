/*-------------------------------------------------------------------------
 *
 * PSQLException.java
 *     This class extends SQLException, and provides our internationalisation
 *     handling
 *
 * Copyright (c) 2003, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/interfaces/jdbc/org/postgresql/util/Attic/PSQLException.java,v 1.13.2.1 2003/12/12 18:39:01 davec Exp $
 *
 *-------------------------------------------------------------------------
 */
package org.postgresql.util;

import java.io.ByteArrayOutputStream;
import java.io.PrintWriter;
import java.sql.SQLException;
import java.util.Hashtable;
import org.postgresql.Driver;

public class PSQLException extends SQLException
{
	private String message;
	private PSQLState state;

	//-------start new constructors-------
	
	public PSQLException(String msg, PSQLState state)
	{
		this.state = state;
		translate(msg, null);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);		
	}
	
	public PSQLException(String msg, PSQLState state, Object[] argv)
	{
		this.state = state;
		translate(msg, argv);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}

	//Helper version for one arg
	public PSQLException(String msg, PSQLState state, Object arg1)
	{
		this.state = state;
		Object[] argv = new Object[1];
		argv[0] = arg1;
		translate(msg, argv);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}
	
	//Helper version for two args
	public PSQLException(String msg, PSQLState state, Object arg1, Object arg2)
	{
		this.state = state;
		Object[] argv = new Object[2];
		argv[0] = arg1;
		argv[1] = arg2;
		translate(msg, argv);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}
	
	//-------end new constructors-------

	public static PSQLException parseServerError(String p_serverError) 
	{
		if (Driver.logDebug)
			Driver.debug("Constructing exception from server message: " + p_serverError);
		char[] l_chars = p_serverError.toCharArray();
		int l_pos = 0;
		int l_length = l_chars.length;
		Hashtable l_mesgParts = new Hashtable();
		while (l_pos < l_length) {
			char l_mesgType = l_chars[l_pos];
			if (l_mesgType != '\0') {
				l_pos++;
				int l_startString = l_pos;
				while (l_chars[l_pos] != '\0' && l_pos < l_length) {
					l_pos++;
				}
				String l_mesgPart = new String(l_chars, l_startString, l_pos - l_startString);
				l_mesgParts.put(new Character(l_mesgType),l_mesgPart);
			}
			l_pos++;
		}

        //Now construct the message from what the server sent
		//The general format is:
		//SEVERITY: Message \n
		//  Detail: \n
		//  Hint: \n
		//  Position: \n
		//  Where: \n
		//  Location: File:Line:Routine \n
		//  SQLState: \n
		//
		//Normally only the message and detail is included.
		//If INFO level logging is enabled then detail, hint, position and where are
		//included.  If DEBUG level logging is enabled then all information 
		//is included.

		StringBuffer l_totalMessage = new StringBuffer();
		String l_message = (String)l_mesgParts.get(MESSAGE_TYPE_S);
		if (l_message != null) 
			l_totalMessage.append(l_message).append(": ");
		l_message = (String)l_mesgParts.get(MESSAGE_TYPE_M);
		if (l_message != null)
			l_totalMessage.append(l_message).append('\n');
		if (Driver.logInfo) {
			l_message = (String)l_mesgParts.get(MESSAGE_TYPE_D);
			if (l_message != null)
				l_totalMessage.append("  ").append(MessageTranslator.translate("postgresql.error.detail", l_message)).append('\n');
			l_message = (String)l_mesgParts.get(MESSAGE_TYPE_H);
			if (l_message != null)
				l_totalMessage.append("  ").append(MessageTranslator.translate("postgresql.error.hint", l_message)).append('\n');
			l_message = (String)l_mesgParts.get(MESSAGE_TYPE_P);
			if (l_message != null)
				l_totalMessage.append("  ").append(MessageTranslator.translate("postgresql.error.position", l_message)).append('\n');
			l_message = (String)l_mesgParts.get(MESSAGE_TYPE_W);
			if (l_message != null)
				l_totalMessage.append("  ").append(MessageTranslator.translate("postgresql.error.where", l_message)).append('\n');
	    }
		if (Driver.logDebug) {
			String l_file = (String)l_mesgParts.get(MESSAGE_TYPE_F);
			String l_line = (String)l_mesgParts.get(MESSAGE_TYPE_L);
			String l_routine = (String)l_mesgParts.get(MESSAGE_TYPE_R);
			if (l_file != null || l_line != null || l_routine != null)
				l_totalMessage.append("  ").append(MessageTranslator.translate("postgresql.error.location", l_file+":"+l_line+":"+l_routine)).append('\n');
			l_message = (String)l_mesgParts.get(MESSAGE_TYPE_C);
			if (l_message != null)
				l_totalMessage.append("  ").append("ServerSQLState: " + l_message).append('\n');
		}

		PSQLException l_return = new PSQLException(l_totalMessage.toString(), PSQLState.UNKNOWN_STATE);
		l_return.state = new PSQLState((String)l_mesgParts.get(MESSAGE_TYPE_C));
		return l_return;
	}
	
	private static final Character MESSAGE_TYPE_S = new Character('S');
	private static final Character MESSAGE_TYPE_M = new Character('M');
	private static final Character MESSAGE_TYPE_D = new Character('D');
	private static final Character MESSAGE_TYPE_H = new Character('H');
	private static final Character MESSAGE_TYPE_P = new Character('P');
	private static final Character MESSAGE_TYPE_W = new Character('W');
	private static final Character MESSAGE_TYPE_F = new Character('F');
	private static final Character MESSAGE_TYPE_L = new Character('L');
	private static final Character MESSAGE_TYPE_R = new Character('R');
	private static final Character MESSAGE_TYPE_C = new Character('C');

	/*
	 * This provides the same functionality to SQLException
	 * @param error Error string
	 */
	public PSQLException(String error)
	{
		translate(error, null);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}

	/*
	 * Helper version for 1 arg
	 */
	public PSQLException(String error, Object arg)
	{
		Object[] argv = new Object[1];
		argv[0] = arg;
		translate(error, argv);
		if (Driver.logDebug)
			Driver.debug("Exception: " + this);
	}


    private void translate(String error, Object[] args) {
	//We convert exception objects to Strings that 
		//contain the full stack trace 
		if (args != null) {
			for (int i = 0; i < args.length; i++) {
				if (args[i] instanceof Exception && !(args[i] instanceof PSQLException)) {
					Exception ex = (Exception) args[i];
					try {
						ByteArrayOutputStream baos = new ByteArrayOutputStream();
						PrintWriter pw = new PrintWriter(baos);
						pw.println("Exception: " + ex.toString() + "\nStack Trace:\n");
						ex.printStackTrace(pw);
						pw.println("End of Stack Trace");
						pw.flush();
						args[i] = baos.toString();
						pw.close();
						baos.close();
					}
					catch (Exception ioe)
					{
				   		args[i] = ex.toString() + "\nIO Error on stack trace generation! " + ioe.toString();
					}
				}
			}
		}

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
	
	public String getSQLState()
	{
		if (state == null)
			return PSQLState.UNKNOWN_STATE.getState();
		return state.getState();
	}
}
