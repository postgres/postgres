package org.postgresql.jdbc2;

import org.postgresql.util.MessageTranslator;

/*
 * This class extends java.sql.BatchUpdateException, and provides our
 * internationalisation handling.
 */
class PBatchUpdateException extends java.sql.BatchUpdateException
{

	private String message;

	public PBatchUpdateException(
		String error, Object arg1, Object arg2, int[] updateCounts )
	{

		super(updateCounts);

		Object[] argv = new Object[2];
		argv[0] = arg1;
		argv[1] = arg2;
		translate(error, argv);
	}

	private void translate(String error, Object[] args)
	{
		message = MessageTranslator.translate(error, args);
	}

	// Overides Throwable
	public String getLocalizedMessage()
	{
		return message;
	}

	// Overides Throwable
	public String getMessage()
	{
		return message;
	}

	// Overides Object
	public String toString()
	{
		return message;
	}
}
