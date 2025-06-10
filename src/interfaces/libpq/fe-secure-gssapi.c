/*-------------------------------------------------------------------------
 *
 * fe-secure-gssapi.c
 *   The front-end (client) encryption support for GSSAPI
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *  src/interfaces/libpq/fe-secure-gssapi.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include "fe-gssapi-common.h"
#include "libpq-fe.h"
#include "libpq-int.h"
#include "port/pg_bswap.h"


/*
 * Require encryption support, as well as mutual authentication and
 * tamperproofing measures.
 */
#define GSS_REQUIRED_FLAGS GSS_C_MUTUAL_FLAG | GSS_C_REPLAY_FLAG | \
	GSS_C_SEQUENCE_FLAG | GSS_C_CONF_FLAG | GSS_C_INTEG_FLAG

/*
 * Handle the encryption/decryption of data using GSSAPI.
 *
 * In the encrypted data stream on the wire, we break up the data
 * into packets where each packet starts with a uint32-size length
 * word (in network byte order), then encrypted data of that length
 * immediately following.  Decryption yields the same data stream
 * that would appear when not using encryption.
 *
 * Encrypted data typically ends up being larger than the same data
 * unencrypted, so we use fixed-size buffers for handling the
 * encryption/decryption which are larger than PQComm's buffer will
 * typically be to minimize the times where we have to make multiple
 * packets (and therefore multiple recv/send calls for a single
 * read/write call to us).
 *
 * NOTE: The client and server have to agree on the max packet size,
 * because we have to pass an entire packet to GSSAPI at a time and we
 * don't want the other side to send arbitrarily huge packets as we
 * would have to allocate memory for them to then pass them to GSSAPI.
 *
 * Therefore, this #define is effectively part of the protocol
 * spec and can't ever be changed.
 */
#define PQ_GSS_MAX_PACKET_SIZE 16384	/* includes uint32 header word */

/*
 * However, during the authentication exchange we must cope with whatever
 * message size the GSSAPI library wants to send (because our protocol
 * doesn't support splitting those messages).  Depending on configuration
 * those messages might be as much as 64kB.
 */
#define PQ_GSS_AUTH_BUFFER_SIZE 65536	/* includes uint32 header word */

/*
 * We need these state variables per-connection.  To allow the functions
 * in this file to look mostly like those in be-secure-gssapi.c, set up
 * these macros.
 */
#define PqGSSSendBuffer (conn->gss_SendBuffer)
#define PqGSSSendLength (conn->gss_SendLength)
#define PqGSSSendNext (conn->gss_SendNext)
#define PqGSSSendConsumed (conn->gss_SendConsumed)
#define PqGSSRecvBuffer (conn->gss_RecvBuffer)
#define PqGSSRecvLength (conn->gss_RecvLength)
#define PqGSSResultBuffer (conn->gss_ResultBuffer)
#define PqGSSResultLength (conn->gss_ResultLength)
#define PqGSSResultNext (conn->gss_ResultNext)
#define PqGSSMaxPktSize (conn->gss_MaxPktSize)


/*
 * Attempt to write len bytes of data from ptr to a GSSAPI-encrypted connection.
 *
 * The connection must be already set up for GSSAPI encryption (i.e., GSSAPI
 * transport negotiation is complete).
 *
 * On success, returns the number of data bytes consumed (possibly less than
 * len).  On failure, returns -1 with errno set appropriately.  If the errno
 * indicates a non-retryable error, a message is added to conn->errorMessage.
 * For retryable errors, caller should call again (passing the same or more
 * data) once the socket is ready.
 */
ssize_t
pg_GSS_write(PGconn *conn, const void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input,
				output = GSS_C_EMPTY_BUFFER;
	ssize_t		ret = -1;
	size_t		bytes_to_encrypt;
	size_t		bytes_encrypted;
	gss_ctx_id_t gctx = conn->gctx;

	/*
	 * When we get a retryable failure, we must not tell the caller we have
	 * successfully transmitted everything, else it won't retry.  For
	 * simplicity, we claim we haven't transmitted anything until we have
	 * successfully transmitted all "len" bytes.  Between calls, the amount of
	 * the current input data that's already been encrypted and placed into
	 * PqGSSSendBuffer (and perhaps transmitted) is remembered in
	 * PqGSSSendConsumed.  On a retry, the caller *must* be sending that data
	 * again, so if it offers a len less than that, something is wrong.
	 *
	 * Note: it may seem attractive to report partial write completion once
	 * we've successfully sent any encrypted packets.  However, doing that
	 * expands the state space of this processing and has been responsible for
	 * bugs in the past (cf. commit d053a879b).  We won't save much,
	 * typically, by letting callers discard data early, so don't risk it.
	 */
	if (len < PqGSSSendConsumed)
	{
		appendPQExpBufferStr(&conn->errorMessage,
							 "GSSAPI caller failed to retransmit all data needing to be retried\n");
		errno = EINVAL;
		return -1;
	}

	/* Discount whatever source data we already encrypted. */
	bytes_to_encrypt = len - PqGSSSendConsumed;
	bytes_encrypted = PqGSSSendConsumed;

	/*
	 * Loop through encrypting data and sending it out until it's all done or
	 * pqsecure_raw_write() complains (which would likely mean that the socket
	 * is non-blocking and the requested send() would block, or there was some
	 * kind of actual error).
	 */
	while (bytes_to_encrypt || PqGSSSendLength)
	{
		int			conf_state = 0;
		uint32		netlen;

		/*
		 * Check if we have data in the encrypted output buffer that needs to
		 * be sent (possibly left over from a previous call), and if so, try
		 * to send it.  If we aren't able to, return that fact back up to the
		 * caller.
		 */
		if (PqGSSSendLength)
		{
			ssize_t		retval;
			ssize_t		amount = PqGSSSendLength - PqGSSSendNext;

			retval = pqsecure_raw_write(conn, PqGSSSendBuffer + PqGSSSendNext, amount);
			if (retval <= 0)
				return retval;

			/*
			 * Check if this was a partial write, and if so, move forward that
			 * far in our buffer and try again.
			 */
			if (retval < amount)
			{
				PqGSSSendNext += retval;
				continue;
			}

			/* We've successfully sent whatever data was in the buffer. */
			PqGSSSendLength = PqGSSSendNext = 0;
		}

		/*
		 * Check if there are any bytes left to encrypt.  If not, we're done.
		 */
		if (!bytes_to_encrypt)
			break;

		/*
		 * Check how much we are being asked to send, if it's too much, then
		 * we will have to loop and possibly be called multiple times to get
		 * through all the data.
		 */
		if (bytes_to_encrypt > PqGSSMaxPktSize)
			input.length = PqGSSMaxPktSize;
		else
			input.length = bytes_to_encrypt;

		input.value = (char *) ptr + bytes_encrypted;

		output.value = NULL;
		output.length = 0;

		/*
		 * Create the next encrypted packet.  Any failure here is considered a
		 * hard failure, so we return -1 even if some data has been sent.
		 */
		major = gss_wrap(&minor, gctx, 1, GSS_C_QOP_DEFAULT,
						 &input, &conf_state, &output);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(libpq_gettext("GSSAPI wrap error"), conn, major, minor);
			errno = EIO;		/* for lack of a better idea */
			goto cleanup;
		}

		if (conf_state == 0)
		{
			libpq_append_conn_error(conn, "outgoing GSSAPI message would not use confidentiality");
			errno = EIO;		/* for lack of a better idea */
			goto cleanup;
		}

		if (output.length > PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32))
		{
			libpq_append_conn_error(conn, "client tried to send oversize GSSAPI packet (%zu > %zu)",
									(size_t) output.length,
									PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32));
			errno = EIO;		/* for lack of a better idea */
			goto cleanup;
		}

		bytes_encrypted += input.length;
		bytes_to_encrypt -= input.length;
		PqGSSSendConsumed += input.length;

		/* 4 network-order bytes of length, then payload */
		netlen = pg_hton32(output.length);
		memcpy(PqGSSSendBuffer + PqGSSSendLength, &netlen, sizeof(uint32));
		PqGSSSendLength += sizeof(uint32);

		memcpy(PqGSSSendBuffer + PqGSSSendLength, output.value, output.length);
		PqGSSSendLength += output.length;

		/* Release buffer storage allocated by GSSAPI */
		gss_release_buffer(&minor, &output);
	}

	/* If we get here, our counters should all match up. */
	Assert(len == PqGSSSendConsumed);
	Assert(len == bytes_encrypted);

	/* We're reporting all the data as sent, so reset PqGSSSendConsumed. */
	PqGSSSendConsumed = 0;

	ret = bytes_encrypted;

cleanup:
	/* Release GSSAPI buffer storage, if we didn't already */
	if (output.value != NULL)
		gss_release_buffer(&minor, &output);
	return ret;
}

/*
 * Read up to len bytes of data into ptr from a GSSAPI-encrypted connection.
 *
 * The connection must be already set up for GSSAPI encryption (i.e., GSSAPI
 * transport negotiation is complete).
 *
 * Returns the number of data bytes read, or on failure, returns -1
 * with errno set appropriately.  If the errno indicates a non-retryable
 * error, a message is added to conn->errorMessage.  For retryable errors,
 * caller should call again once the socket is ready.
 */
ssize_t
pg_GSS_read(PGconn *conn, void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input = GSS_C_EMPTY_BUFFER,
				output = GSS_C_EMPTY_BUFFER;
	ssize_t		ret;
	size_t		bytes_returned = 0;
	gss_ctx_id_t gctx = conn->gctx;

	/*
	 * The plan here is to read one incoming encrypted packet into
	 * PqGSSRecvBuffer, decrypt it into PqGSSResultBuffer, and then dole out
	 * data from there to the caller.  When we exhaust the current input
	 * packet, read another.
	 */
	while (bytes_returned < len)
	{
		int			conf_state = 0;

		/* Check if we have data in our buffer that we can return immediately */
		if (PqGSSResultNext < PqGSSResultLength)
		{
			size_t		bytes_in_buffer = PqGSSResultLength - PqGSSResultNext;
			size_t		bytes_to_copy = Min(bytes_in_buffer, len - bytes_returned);

			/*
			 * Copy the data from our result buffer into the caller's buffer,
			 * at the point where we last left off filling their buffer.
			 */
			memcpy((char *) ptr + bytes_returned, PqGSSResultBuffer + PqGSSResultNext, bytes_to_copy);
			PqGSSResultNext += bytes_to_copy;
			bytes_returned += bytes_to_copy;

			/*
			 * At this point, we've either filled the caller's buffer or
			 * emptied our result buffer.  Either way, return to caller.  In
			 * the second case, we could try to read another encrypted packet,
			 * but the odds are good that there isn't one available.  (If this
			 * isn't true, we chose too small a max packet size.)  In any
			 * case, there's no harm letting the caller process the data we've
			 * already returned.
			 */
			break;
		}

		/* Result buffer is empty, so reset buffer pointers */
		PqGSSResultLength = PqGSSResultNext = 0;

		/*
		 * Because we chose above to return immediately as soon as we emit
		 * some data, bytes_returned must be zero at this point.  Therefore
		 * the failure exits below can just return -1 without worrying about
		 * whether we already emitted some data.
		 */
		Assert(bytes_returned == 0);

		/*
		 * At this point, our result buffer is empty with more bytes being
		 * requested to be read.  We are now ready to load the next packet and
		 * decrypt it (entirely) into our result buffer.
		 */

		/* Collect the length if we haven't already */
		if (PqGSSRecvLength < sizeof(uint32))
		{
			ret = pqsecure_raw_read(conn, PqGSSRecvBuffer + PqGSSRecvLength,
									sizeof(uint32) - PqGSSRecvLength);

			/* If ret <= 0, pqsecure_raw_read already set the correct errno */
			if (ret <= 0)
				return ret;

			PqGSSRecvLength += ret;

			/* If we still haven't got the length, return to the caller */
			if (PqGSSRecvLength < sizeof(uint32))
			{
				errno = EWOULDBLOCK;
				return -1;
			}
		}

		/* Decode the packet length and check for overlength packet */
		input.length = pg_ntoh32(*(uint32 *) PqGSSRecvBuffer);

		if (input.length > PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32))
		{
			libpq_append_conn_error(conn, "oversize GSSAPI packet sent by the server (%zu > %zu)",
									(size_t) input.length,
									PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32));
			errno = EIO;		/* for lack of a better idea */
			return -1;
		}

		/*
		 * Read as much of the packet as we are able to on this call into
		 * wherever we left off from the last time we were called.
		 */
		ret = pqsecure_raw_read(conn, PqGSSRecvBuffer + PqGSSRecvLength,
								input.length - (PqGSSRecvLength - sizeof(uint32)));
		/* If ret <= 0, pqsecure_raw_read already set the correct errno */
		if (ret <= 0)
			return ret;

		PqGSSRecvLength += ret;

		/* If we don't yet have the whole packet, return to the caller */
		if (PqGSSRecvLength - sizeof(uint32) < input.length)
		{
			errno = EWOULDBLOCK;
			return -1;
		}

		/*
		 * We now have the full packet and we can perform the decryption and
		 * refill our result buffer, then loop back up to pass data back to
		 * the caller.  Note that error exits below here must take care of
		 * releasing the gss output buffer.
		 */
		output.value = NULL;
		output.length = 0;
		input.value = PqGSSRecvBuffer + sizeof(uint32);

		major = gss_unwrap(&minor, gctx, &input, &output, &conf_state, NULL);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(libpq_gettext("GSSAPI unwrap error"), conn,
						 major, minor);
			ret = -1;
			errno = EIO;		/* for lack of a better idea */
			goto cleanup;
		}

		if (conf_state == 0)
		{
			libpq_append_conn_error(conn, "incoming GSSAPI message did not use confidentiality");
			ret = -1;
			errno = EIO;		/* for lack of a better idea */
			goto cleanup;
		}

		memcpy(PqGSSResultBuffer, output.value, output.length);
		PqGSSResultLength = output.length;

		/* Our receive buffer is now empty, reset it */
		PqGSSRecvLength = 0;

		/* Release buffer storage allocated by GSSAPI */
		gss_release_buffer(&minor, &output);
	}

	ret = bytes_returned;

cleanup:
	/* Release GSSAPI buffer storage, if we didn't already */
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
	if (*ret < 0)
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return PGRES_POLLING_READING;
		else
			return PGRES_POLLING_FAILED;
	}

	/* Check for EOF */
	if (*ret == 0)
	{
		int			result = pqReadReady(conn);

		if (result < 0)
			return PGRES_POLLING_FAILED;

		if (!result)
			return PGRES_POLLING_READING;

		*ret = pqsecure_raw_read(conn, recv_buffer, length);
		if (*ret < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return PGRES_POLLING_READING;
			else
				return PGRES_POLLING_FAILED;
		}
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
	ssize_t		ret;
	OM_uint32	major,
				minor,
				gss_flags = GSS_REQUIRED_FLAGS;
	uint32		netlen;
	PostgresPollingStatusType result;
	gss_buffer_desc input = GSS_C_EMPTY_BUFFER,
				output = GSS_C_EMPTY_BUFFER;

	/*
	 * If first time through for this connection, allocate buffers and
	 * initialize state variables.  By malloc'ing the buffers separately, we
	 * ensure that they are sufficiently aligned for the length-word accesses
	 * that we do in some places in this file.
	 *
	 * We'll use PQ_GSS_AUTH_BUFFER_SIZE-sized buffers until transport
	 * negotiation is complete, then switch to PQ_GSS_MAX_PACKET_SIZE.
	 */
	if (PqGSSSendBuffer == NULL)
	{
		PqGSSSendBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
		PqGSSRecvBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
		PqGSSResultBuffer = malloc(PQ_GSS_AUTH_BUFFER_SIZE);
		if (!PqGSSSendBuffer || !PqGSSRecvBuffer || !PqGSSResultBuffer)
		{
			libpq_append_conn_error(conn, "out of memory");
			return PGRES_POLLING_FAILED;
		}
		PqGSSSendLength = PqGSSSendNext = PqGSSSendConsumed = 0;
		PqGSSRecvLength = PqGSSResultLength = PqGSSResultNext = 0;
	}

	/*
	 * Check if we have anything to send from a prior call and if so, send it.
	 */
	if (PqGSSSendLength)
	{
		ssize_t		amount = PqGSSSendLength - PqGSSSendNext;

		ret = pqsecure_raw_write(conn, PqGSSSendBuffer + PqGSSSendNext, amount);
		if (ret < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				return PGRES_POLLING_WRITING;
			else
				return PGRES_POLLING_FAILED;
		}

		if (ret < amount)
		{
			PqGSSSendNext += ret;
			return PGRES_POLLING_WRITING;
		}

		PqGSSSendLength = PqGSSSendNext = 0;
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
			result = gss_read(conn, PqGSSRecvBuffer + PqGSSRecvLength, PQ_GSS_AUTH_BUFFER_SIZE - PqGSSRecvLength - 1, &ret);
			if (result != PGRES_POLLING_OK)
				return result;

			PqGSSRecvLength += ret;

			Assert(PqGSSRecvLength < PQ_GSS_AUTH_BUFFER_SIZE);
			PqGSSRecvBuffer[PqGSSRecvLength] = '\0';
			appendPQExpBuffer(&conn->errorMessage, "%s\n", PqGSSRecvBuffer + 1);

			return PGRES_POLLING_FAILED;
		}

		/*
		 * We should have the whole length at this point, so pull it out and
		 * then read whatever we have left of the packet
		 */

		/* Get the length and check for over-length packet */
		input.length = pg_ntoh32(*(uint32 *) PqGSSRecvBuffer);
		if (input.length > PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32))
		{
			libpq_append_conn_error(conn, "oversize GSSAPI packet sent by the server (%zu > %zu)",
									(size_t) input.length,
									PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32));
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

	if (conn->gssdelegation && conn->gssdelegation[0] == '1')
	{
		/* Acquire credentials if possible */
		if (conn->gcred == GSS_C_NO_CREDENTIAL)
			(void) pg_GSS_have_cred_cache(&conn->gcred);

		/*
		 * We have credentials and gssdelegation is enabled, so request
		 * credential delegation.  This may or may not actually result in
		 * credentials being delegated- it depends on if the forwardable flag
		 * has been set in the credential and if the server is configured to
		 * accept delegated credentials.
		 */
		if (conn->gcred != GSS_C_NO_CREDENTIAL)
			gss_flags |= GSS_C_DELEG_FLAG;
	}

	/*
	 * Call GSS init context, either with an empty input, or with a complete
	 * packet from the server.
	 */
	major = gss_init_sec_context(&minor, conn->gcred, &conn->gctx,
								 conn->gtarg_nam, GSS_C_NO_OID,
								 gss_flags, 0, 0, &input, NULL,
								 &output, NULL, NULL);

	/* GSS Init Sec Context uses the whole packet, so clear it */
	PqGSSRecvLength = 0;

	if (GSS_ERROR(major))
	{
		pg_GSS_error(libpq_gettext("could not initiate GSSAPI security context"),
					 conn, major, minor);
		return PGRES_POLLING_FAILED;
	}

	if (output.length == 0)
	{
		/*
		 * We're done - hooray!  Set flag to tell the low-level I/O routines
		 * to do GSS wrapping/unwrapping.
		 */
		conn->gssenc = true;
		conn->gssapi_used = true;

		/* Clean up */
		gss_release_cred(&minor, &conn->gcred);
		conn->gcred = GSS_C_NO_CREDENTIAL;
		gss_release_buffer(&minor, &output);

		/*
		 * Release the large authentication buffers and allocate the ones we
		 * want for normal operation.  (This maneuver is safe only because
		 * pqDropConnection will drop the buffers; otherwise, during a
		 * reconnection we'd be at risk of using undersized buffers during
		 * negotiation.)
		 */
		free(PqGSSSendBuffer);
		free(PqGSSRecvBuffer);
		free(PqGSSResultBuffer);
		PqGSSSendBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
		PqGSSRecvBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
		PqGSSResultBuffer = malloc(PQ_GSS_MAX_PACKET_SIZE);
		if (!PqGSSSendBuffer || !PqGSSRecvBuffer || !PqGSSResultBuffer)
		{
			libpq_append_conn_error(conn, "out of memory");
			return PGRES_POLLING_FAILED;
		}
		PqGSSSendLength = PqGSSSendNext = PqGSSSendConsumed = 0;
		PqGSSRecvLength = PqGSSResultLength = PqGSSResultNext = 0;

		/*
		 * Determine the max packet size which will fit in our buffer, after
		 * accounting for the length.  pg_GSS_write will need this.
		 */
		major = gss_wrap_size_limit(&minor, conn->gctx, 1, GSS_C_QOP_DEFAULT,
									PQ_GSS_MAX_PACKET_SIZE - sizeof(uint32),
									&PqGSSMaxPktSize);

		if (GSS_ERROR(major))
		{
			pg_GSS_error(libpq_gettext("GSSAPI size check error"), conn,
						 major, minor);
			return PGRES_POLLING_FAILED;
		}

		return PGRES_POLLING_OK;
	}

	/* Must have output.length > 0 */
	if (output.length > PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32))
	{
		libpq_append_conn_error(conn, "client tried to send oversize GSSAPI packet (%zu > %zu)",
								(size_t) output.length,
								PQ_GSS_AUTH_BUFFER_SIZE - sizeof(uint32));
		gss_release_buffer(&minor, &output);
		return PGRES_POLLING_FAILED;
	}

	/* Queue the token for writing */
	netlen = pg_hton32(output.length);

	memcpy(PqGSSSendBuffer, &netlen, sizeof(uint32));
	PqGSSSendLength += sizeof(uint32);

	memcpy(PqGSSSendBuffer + PqGSSSendLength, output.value, output.length);
	PqGSSSendLength += output.length;

	/* We don't bother with PqGSSSendConsumed here */

	/* Release buffer storage allocated by GSSAPI */
	gss_release_buffer(&minor, &output);

	/* Ask to be called again to write data */
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
