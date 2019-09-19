/*-------------------------------------------------------------------------
 *
 * fe-secure-gssapi.c
 *   The front-end (client) encryption support for GSSAPI
 *
 * Portions Copyright (c) 2016-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *  src/interfaces/libpq/fe-secure-gssapi.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-gssapi-common.h"
#include "port/pg_bswap.h"

/*
 * Require encryption support, as well as mutual authentication and
 * tamperproofing measures.
 */
#define GSS_REQUIRED_FLAGS GSS_C_MUTUAL_FLAG | GSS_C_REPLAY_FLAG | \
	GSS_C_SEQUENCE_FLAG | GSS_C_CONF_FLAG | GSS_C_INTEG_FLAG

/*
 * We use fixed-size buffers for handling the encryption/decryption
 * which are larger than PQComm's buffer will typically be to minimize
 * the times where we have to make multiple packets and therefore sets
 * of recv/send calls for a single read/write call to us.
 *
 * NOTE: The client and server have to agree on the max packet size,
 * because we have to pass an entire packet to GSSAPI at a time and we
 * don't want the other side to send arbitrairly huge packets as we
 * would have to allocate memory for them to then pass them to GSSAPI.
 */
#define PQ_GSS_SEND_BUFFER_SIZE 16384
#define PQ_GSS_RECV_BUFFER_SIZE 16384

/* PqGSSSendBuffer is for *encrypted* data */
static char PqGSSSendBuffer[PQ_GSS_SEND_BUFFER_SIZE];
static int	PqGSSSendPointer;	/* Next index to store a byte in
								 * PqGSSSendBuffer */
static int	PqGSSSendStart;		/* Next index to send a byte in
								 * PqGSSSendBuffer */

/* PqGSSRecvBuffer is for *encrypted* data */
static char PqGSSRecvBuffer[PQ_GSS_RECV_BUFFER_SIZE];
static int	PqGSSRecvPointer;	/* Next index to read a byte from
								 * PqGSSRecvBuffer */
static int	PqGSSRecvLength;	/* End of data available in PqGSSRecvBuffer */

/* PqGSSResultBuffer is for *unencrypted* data */
static char PqGSSResultBuffer[PQ_GSS_RECV_BUFFER_SIZE];
static int	PqGSSResultPointer; /* Next index to read a byte from
								 * PqGSSResultBuffer */
static int	PqGSSResultLength;	/* End of data available in PqGSSResultBuffer */

uint32		max_packet_size;	/* Maximum size we can encrypt and fit the
								 * results into our output buffer */

/*
 * Write len bytes of data from ptr along a GSSAPI-encrypted connection.  Note
 * that the connection must be already set up for GSSAPI encryption (i.e.,
 * GSSAPI transport negotiation is complete).  Returns len when all data has
 * been written; retry when errno is EWOULDBLOCK or similar with the same
 * values of ptr and len.  On non-socket failures, will log an error message.
 */
ssize_t
pg_GSS_write(PGconn *conn, const void *ptr, size_t len)
{
	gss_buffer_desc input,
				output = GSS_C_EMPTY_BUFFER;
	OM_uint32	major,
				minor;
	ssize_t		ret = -1;
	size_t		bytes_to_encrypt = len;
	size_t		bytes_encrypted = 0;

	/*
	 * Loop through encrypting data and sending it out until
	 * pqsecure_raw_write() complains (which would likely mean that the socket
	 * is non-blocking and the requested send() would block, or there was some
	 * kind of actual error) and then return.
	 */
	while (bytes_to_encrypt || PqGSSSendPointer)
	{
		int			conf_state = 0;
		uint32		netlen;

		/*
		 * Check if we have data in the encrypted output buffer that needs to
		 * be sent, and if so, try to send it.  If we aren't able to, return
		 * that back up to the caller.
		 */
		if (PqGSSSendPointer)
		{
			ssize_t		ret;
			ssize_t		amount = PqGSSSendPointer - PqGSSSendStart;

			ret = pqsecure_raw_write(conn, PqGSSSendBuffer + PqGSSSendStart, amount);
			if (ret < 0)
			{
				/*
				 * If we encrypted some data and it's in our output buffer,
				 * but send() is saying that we would block, then tell the
				 * client how far we got with encrypting the data so that they
				 * can call us again with whatever is left, at which point we
				 * will try to send the remaining encrypted data first and
				 * then move on to encrypting the rest of the data.
				 */
				if (bytes_encrypted != 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
					return bytes_encrypted;
				else
					return ret;
			}

			/*
			 * Partial write, move forward that far in our buffer and try
			 * again
			 */
			if (ret != amount)
			{
				PqGSSSendStart += ret;
				continue;
			}

			/* All encrypted data was sent, our buffer is empty now. */
			PqGSSSendPointer = PqGSSSendStart = 0;
		}

		/*
		 * Check if there are any bytes left to encrypt.  If not, we're done.
		 */
		if (!bytes_to_encrypt)
			return bytes_encrypted;

		/*
		 * Check how much we are being asked to send, if it's too much, then
		 * we will have to loop and possibly be called multiple times to get
		 * through all the data.
		 */
		if (bytes_to_encrypt > max_packet_size)
			input.length = max_packet_size;
		else
			input.length = bytes_to_encrypt;

		input.value = (char *) ptr + bytes_encrypted;

		output.value = NULL;
		output.length = 0;

		/* Create the next encrypted packet */
		major = gss_wrap(&minor, conn->gctx, 1, GSS_C_QOP_DEFAULT,
						 &input, &conf_state, &output);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(libpq_gettext("GSSAPI wrap error"), conn, major, minor);
			goto cleanup;
		}
		else if (conf_state == 0)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("outgoing GSSAPI message would not use confidentiality\n"));
			goto cleanup;
		}

		if (output.length > PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("client tried to send oversize GSSAPI packet (%zu > %zu)\n"),
							  (size_t) output.length,
							  PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32));
			goto cleanup;
		}

		bytes_encrypted += input.length;
		bytes_to_encrypt -= input.length;

		/* 4 network-order bytes of length, then payload */
		netlen = htonl(output.length);
		memcpy(PqGSSSendBuffer + PqGSSSendPointer, &netlen, sizeof(uint32));
		PqGSSSendPointer += sizeof(uint32);

		memcpy(PqGSSSendBuffer + PqGSSSendPointer, output.value, output.length);
		PqGSSSendPointer += output.length;
	}

	ret = bytes_encrypted;

cleanup:
	if (output.value != NULL)
		gss_release_buffer(&minor, &output);
	return ret;
}

/*
 * Read up to len bytes of data into ptr from a GSSAPI-encrypted connection.
 * Note that GSSAPI transport must already have been negotiated.  Returns the
 * number of bytes read into ptr; otherwise, returns -1.  Retry with the same
 * ptr and len when errno is EWOULDBLOCK or similar.
 */
ssize_t
pg_GSS_read(PGconn *conn, void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input = GSS_C_EMPTY_BUFFER,
				output = GSS_C_EMPTY_BUFFER;
	ssize_t		ret = 0;
	size_t		bytes_to_return = len;
	size_t		bytes_returned = 0;

	/*
	 * The goal here is to read an incoming encrypted packet, one at a time,
	 * decrypt it into our out buffer, returning to the caller what they asked
	 * for, and then saving anything else for the next call.
	 *
	 * We get a read request, we look if we have cleartext bytes available
	 * and, if so, copy those to the result, and then we try to decrypt the
	 * next packet.
	 *
	 * We should not try to decrypt the next packet until the read buffer is
	 * completely empty.
	 *
	 * If the caller asks for more bytes than one decrypted packet, then we
	 * should try to return all bytes asked for.
	 */
	while (bytes_to_return)
	{
		int			conf_state = 0;

		/* Check if we have data in our buffer that we can return immediately */
		if (PqGSSResultPointer < PqGSSResultLength)
		{
			int			bytes_in_buffer = PqGSSResultLength - PqGSSResultPointer;
			int			bytes_to_copy = bytes_in_buffer < len - bytes_returned ? bytes_in_buffer : len - bytes_returned;

			/*
			 * Copy the data from our output buffer into the caller's buffer,
			 * at the point where we last left off filling their buffer
			 */
			memcpy((char *) ptr + bytes_returned, PqGSSResultBuffer + PqGSSResultPointer, bytes_to_copy);
			PqGSSResultPointer += bytes_to_copy;
			bytes_to_return -= bytes_to_copy;
			bytes_returned += bytes_to_copy;

			/* Check if our result buffer is now empty and, if so, reset */
			if (PqGSSResultPointer == PqGSSResultLength)
				PqGSSResultPointer = PqGSSResultLength = 0;

			continue;
		}

		/*
		 * At this point, our output buffer should be empty with more bytes
		 * being requested to be read.  We are now ready to load the next
		 * packet and decrypt it (entirely) into our buffer.
		 *
		 * If we get a partial read back while trying to read a packet off the
		 * wire then we return back what bytes we were able to return and wait
		 * to be called again, until we get a full packet to decrypt.
		 */

		/* Check if we got a partial read just trying to get the length */
		if (PqGSSRecvLength < sizeof(uint32))
		{
			/* Try to get whatever of the length we still need */
			ret = pqsecure_raw_read(conn, PqGSSRecvBuffer + PqGSSRecvLength,
									sizeof(uint32) - PqGSSRecvLength);
			if (ret < 0)
				return bytes_returned ? bytes_returned : ret;

			PqGSSRecvLength += ret;
			if (PqGSSRecvLength < sizeof(uint32))
				return bytes_returned;
		}

		/*
		 * We should have the whole length at this point, so pull it out and
		 * then read whatever we have left of the packet
		 */
		input.length = ntohl(*(uint32 *) PqGSSRecvBuffer);

		/* Check for over-length packet */
		if (input.length > PQ_GSS_RECV_BUFFER_SIZE - sizeof(uint32))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("oversize GSSAPI packet sent by the server (%zu > %zu)\n"),
							  (size_t) input.length,
							  PQ_GSS_RECV_BUFFER_SIZE - sizeof(uint32));
			ret = -1;
			goto cleanup;
		}

		/*
		 * Read as much of the packet as we are able to on this call into
		 * wherever we left off from the last time we were called.
		 */
		ret = pqsecure_raw_read(conn, PqGSSRecvBuffer + PqGSSRecvLength,
								input.length - (PqGSSRecvLength - sizeof(uint32)));
		if (ret < 0)
			return bytes_returned ? bytes_returned : ret;

		/*
		 * If we got less than the rest of the packet then we need to return
		 * and be called again.
		 */
		PqGSSRecvLength += ret;
		if (PqGSSRecvLength - sizeof(uint32) < input.length)
			return bytes_returned ? bytes_returned : -1;

		/*
		 * We now have the full packet and we can perform the decryption and
		 * refill our output buffer, then loop back up to pass that back to
		 * the user.
		 */
		output.value = NULL;
		output.length = 0;
		input.value = PqGSSRecvBuffer + sizeof(uint32);

		major = gss_unwrap(&minor, conn->gctx, &input, &output, &conf_state, NULL);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(libpq_gettext("GSSAPI unwrap error"), conn,
						 major, minor);
			ret = -1;
			goto cleanup;
		}
		else if (conf_state == 0)
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("incoming GSSAPI message did not use confidentiality\n"));
			ret = -1;
			goto cleanup;
		}

		memcpy(PqGSSResultBuffer, output.value, output.length);
		PqGSSResultLength = output.length;

		/* Our buffer is now empty, reset it */
		PqGSSRecvPointer = PqGSSRecvLength = 0;

		gss_release_buffer(&minor, &output);
	}

	ret = bytes_returned;

cleanup:
	if (output.value != NULL)
		gss_release_buffer(&minor, &output);
	return ret;
}

/*
 * Simple wrapper for reading from pqsecure_raw_read.
 *
 * This takes the same arguments as pqsecure_raw_read, plus an output parameter
 * to return the number of bytes read.  This handles if blocking would occur and
 * if we detect EOF on the connection.
 */
static PostgresPollingStatusType
gss_read(PGconn *conn, void *recv_buffer, size_t length, ssize_t *ret)
{
	*ret = pqsecure_raw_read(conn, recv_buffer, length);
	if (*ret < 0 && errno == EWOULDBLOCK)
		return PGRES_POLLING_READING;
	else if (*ret < 0)
		return PGRES_POLLING_FAILED;

	/* Check for EOF */
	if (*ret == 0)
	{
		int			result = pqReadReady(conn);

		if (result < 0)
			return PGRES_POLLING_FAILED;

		if (!result)
			return PGRES_POLLING_READING;

		*ret = pqsecure_raw_read(conn, recv_buffer, length);
		if (*ret == 0)
			return PGRES_POLLING_FAILED;
	}

	return PGRES_POLLING_OK;
}

/*
 * Negotiate GSSAPI transport for a connection.  When complete, returns
 * PGRES_POLLING_OK.  Will return PGRES_POLLING_READING or
 * PGRES_POLLING_WRITING as appropriate whenever it would block, and
 * PGRES_POLLING_FAILED if transport could not be negotiated.
 */
PostgresPollingStatusType
pqsecure_open_gss(PGconn *conn)
{
	static int	first = 1;
	ssize_t		ret;
	OM_uint32	major,
				minor;
	uint32		netlen;
	PostgresPollingStatusType result;
	gss_buffer_desc input = GSS_C_EMPTY_BUFFER,
				output = GSS_C_EMPTY_BUFFER;

	/* Check for data that needs to be written */
	if (first)
	{
		PqGSSSendPointer = PqGSSSendStart = PqGSSRecvPointer = PqGSSRecvLength = PqGSSResultPointer = PqGSSResultLength = 0;
		first = 0;
	}

	/*
	 * Check if we have anything to send from a prior call and if so, send it.
	 */
	if (PqGSSSendPointer)
	{
		ssize_t		amount = PqGSSSendPointer - PqGSSSendStart;

		ret = pqsecure_raw_write(conn, PqGSSSendBuffer + PqGSSSendStart, amount);
		if (ret < 0 && errno == EWOULDBLOCK)
			return PGRES_POLLING_WRITING;

		if (ret != amount)
		{
			PqGSSSendStart += amount;
			return PGRES_POLLING_WRITING;
		}

		PqGSSSendPointer = PqGSSSendStart = 0;
	}

	/*
	 * Client sends first, and sending creates a context, therefore this will
	 * be false the first time through, and then when we get called again we
	 * will check for incoming data.
	 */
	if (conn->gctx)
	{
		/* Process any incoming data we might have */

		/* See if we are still trying to get the length */
		if (PqGSSRecvLength < sizeof(uint32))
		{
			/* Attempt to get the length first */
			result = gss_read(conn, PqGSSRecvBuffer + PqGSSRecvLength, sizeof(uint32) - PqGSSRecvLength, &ret);
			if (result != PGRES_POLLING_OK)
				return result;

			PqGSSRecvLength += ret;

			if (PqGSSRecvLength < sizeof(uint32))
				return PGRES_POLLING_READING;
		}

		/*
		 * Check if we got an error packet
		 *
		 * This is safe to do because we shouldn't ever get a packet over 8192
		 * and therefore the actual length bytes, being that they are in
		 * network byte order, for any real packet will start with two zero
		 * bytes.
		 */
		if (PqGSSRecvBuffer[0] == 'E')
		{
			/*
			 * For an error packet during startup, we don't get a length, so
			 * simply read as much as we can fit into our buffer (as a string,
			 * so leave a spot at the end for a NULL byte too) and report that
			 * back to the caller.
			 */
			result = gss_read(conn, PqGSSRecvBuffer + PqGSSRecvLength, PQ_GSS_RECV_BUFFER_SIZE - PqGSSRecvLength - 1, &ret);
			if (result != PGRES_POLLING_OK)
				return result;

			PqGSSRecvLength += ret;

			printfPQExpBuffer(&conn->errorMessage, "%s\n", PqGSSRecvBuffer + 1);

			return PGRES_POLLING_FAILED;
		}

		/*
		 * We should have the whole length at this point, so pull it out and
		 * then read whatever we have left of the packet
		 */

		/* Get the length and check for over-length packet */
		input.length = ntohl(*(uint32 *) PqGSSRecvBuffer);
		if (input.length > PQ_GSS_RECV_BUFFER_SIZE - sizeof(uint32))
		{
			printfPQExpBuffer(&conn->errorMessage,
							  libpq_gettext("oversize GSSAPI packet sent by the server (%zu > %zu)\n"),
							  (size_t) input.length,
							  PQ_GSS_RECV_BUFFER_SIZE - sizeof(uint32));
			return PGRES_POLLING_FAILED;
		}

		/*
		 * Read as much of the packet as we are able to on this call into
		 * wherever we left off from the last time we were called.
		 */
		result = gss_read(conn, PqGSSRecvBuffer + PqGSSRecvLength,
						  input.length - (PqGSSRecvLength - sizeof(uint32)), &ret);
		if (result != PGRES_POLLING_OK)
			return result;

		PqGSSRecvLength += ret;

		/*
		 * If we got less than the rest of the packet then we need to return
		 * and be called again.
		 */
		if (PqGSSRecvLength - sizeof(uint32) < input.length)
			return PGRES_POLLING_READING;

		input.value = PqGSSRecvBuffer + sizeof(uint32);
	}

	/* Load the service name (no-op if already done */
	ret = pg_GSS_load_servicename(conn);
	if (ret != STATUS_OK)
		return PGRES_POLLING_FAILED;

	/*
	 * Call GSS init context, either with an empty input, or with a complete
	 * packet from the server.
	 */
	major = gss_init_sec_context(&minor, conn->gcred, &conn->gctx,
								 conn->gtarg_nam, GSS_C_NO_OID,
								 GSS_REQUIRED_FLAGS, 0, 0, &input, NULL,
								 &output, NULL, NULL);

	/* GSS Init Sec Context uses the whole packet, so clear it */
	PqGSSRecvPointer = PqGSSRecvLength = 0;

	if (GSS_ERROR(major))
	{
		pg_GSS_error(libpq_gettext("could not initiate GSSAPI security context"),
					 conn, major, minor);
		return PGRES_POLLING_FAILED;
	}
	else if (output.length == 0)
	{
		/*
		 * We're done - hooray!  Kind of gross, but we need to disable SSL
		 * here so that we don't accidentally tunnel one over the other.
		 */
#ifdef USE_SSL
		conn->allow_ssl_try = false;
#endif
		gss_release_cred(&minor, &conn->gcred);
		conn->gcred = GSS_C_NO_CREDENTIAL;
		conn->gssenc = true;

		/*
		 * Determine the max packet size which will fit in our buffer, after
		 * accounting for the length
		 */
		major = gss_wrap_size_limit(&minor, conn->gctx, 1, GSS_C_QOP_DEFAULT,
									PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32), &max_packet_size);

		if (GSS_ERROR(major))
			pg_GSS_error(libpq_gettext("GSSAPI size check error"), conn,
						 major, minor);

		return PGRES_POLLING_OK;
	}

	/* Must have output.length > 0 */
	if (output.length > PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))
	{
		pg_GSS_error(libpq_gettext("GSSAPI context establishment error"),
					 conn, major, minor);
		return PGRES_POLLING_FAILED;
	}

	/* Queue the token for writing */
	netlen = htonl(output.length);

	memcpy(PqGSSSendBuffer, (char *) &netlen, sizeof(uint32));
	PqGSSSendPointer += sizeof(uint32);

	memcpy(PqGSSSendBuffer + PqGSSSendPointer, output.value, output.length);
	PqGSSSendPointer += output.length;

	gss_release_buffer(&minor, &output);

	/* Asked to be called again to write data */
	return PGRES_POLLING_WRITING;
}

/*
 * GSSAPI Information functions.
 */

/*
 * Return the GSSAPI Context itself.
 */
void *
PQgetgssctx(PGconn *conn)
{
	if (!conn)
		return NULL;

	return conn->gctx;
}

/*
 * Return true if GSSAPI encryption is in use.
 */
int
PQgssEncInUse(PGconn *conn)
{
	if (!conn || !conn->gctx)
		return 0;

	return conn->gssenc;
}
