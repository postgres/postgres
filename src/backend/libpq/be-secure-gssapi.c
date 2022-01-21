/*-------------------------------------------------------------------------
 *
 * be-secure-gssapi.c
 *  GSSAPI encryption support
 *
 * Portions Copyright (c) 2018-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *  src/backend/libpq/be-secure-gssapi.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>

#include "libpq/auth.h"
#include "libpq/be-gssapi-common.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/memutils.h"


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
 * Therefore, these two #define's are effectively part of the protocol
 * spec and can't ever be changed.
 */
#define PQ_GSS_SEND_BUFFER_SIZE 16384
#define PQ_GSS_RECV_BUFFER_SIZE 16384

/*
 * Since we manage at most one GSS-encrypted connection per backend,
 * we can just keep all this state in static variables.  The char *
 * variables point to buffers that are allocated once and re-used.
 */
static char *PqGSSSendBuffer;	/* Encrypted data waiting to be sent */
static int	PqGSSSendLength;	/* End of data available in PqGSSSendBuffer */
static int	PqGSSSendNext;		/* Next index to send a byte from
								 * PqGSSSendBuffer */
static int	PqGSSSendConsumed;	/* Number of *unencrypted* bytes consumed for
								 * current contents of PqGSSSendBuffer */

static char *PqGSSRecvBuffer;	/* Received, encrypted data */
static int	PqGSSRecvLength;	/* End of data available in PqGSSRecvBuffer */

static char *PqGSSResultBuffer; /* Decryption of data in gss_RecvBuffer */
static int	PqGSSResultLength;	/* End of data available in PqGSSResultBuffer */
static int	PqGSSResultNext;	/* Next index to read a byte from
								 * PqGSSResultBuffer */

static uint32 PqGSSMaxPktSize;	/* Maximum size we can encrypt and fit the
								 * results into our output buffer */


/*
 * Attempt to write len bytes of data from ptr to a GSSAPI-encrypted connection.
 *
 * The connection must be already set up for GSSAPI encryption (i.e., GSSAPI
 * transport negotiation is complete).
 *
 * On success, returns the number of data bytes consumed (possibly less than
 * len).  On failure, returns -1 with errno set appropriately.  For retryable
 * errors, caller should call again (passing the same data) once the socket
 * is ready.
 *
 * Dealing with fatal errors here is a bit tricky: we can't invoke elog(FATAL)
 * since it would try to write to the client, probably resulting in infinite
 * recursion.  Instead, use elog(COMMERROR) to log extra info about the
 * failure if necessary, and then return an errno indicating connection loss.
 */
ssize_t
be_gssapi_write(Port *port, void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input,
				output;
	size_t		bytes_sent = 0;
	size_t		bytes_to_encrypt;
	size_t		bytes_encrypted;
	gss_ctx_id_t gctx = port->gss->ctx;

	/*
	 * When we get a failure, we must not tell the caller we have successfully
	 * transmitted everything, else it won't retry.  Hence a "success"
	 * (positive) return value must only count source bytes corresponding to
	 * fully-transmitted encrypted packets.  The amount of source data
	 * corresponding to the current partly-transmitted packet is remembered in
	 * PqGSSSendConsumed.  On a retry, the caller *must* be sending that data
	 * again, so if it offers a len less than that, something is wrong.
	 */
	if (len < PqGSSSendConsumed)
	{
		elog(COMMERROR, "GSSAPI caller failed to retransmit all data needing to be retried");
		errno = ECONNRESET;
		return -1;
	}
	/* Discount whatever source data we already encrypted. */
	bytes_to_encrypt = len - PqGSSSendConsumed;
	bytes_encrypted = PqGSSSendConsumed;

	/*
	 * Loop through encrypting data and sending it out until it's all done or
	 * secure_raw_write() complains (which would likely mean that the socket
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
			ssize_t		ret;
			ssize_t		amount = PqGSSSendLength - PqGSSSendNext;

			ret = secure_raw_write(port, PqGSSSendBuffer + PqGSSSendNext, amount);
			if (ret <= 0)
			{
				/*
				 * Report any previously-sent data; if there was none, reflect
				 * the secure_raw_write result up to our caller.  When there
				 * was some, we're effectively assuming that any interesting
				 * failure condition will recur on the next try.
				 */
				if (bytes_sent)
					return bytes_sent;
				return ret;
			}

			/*
			 * Check if this was a partial write, and if so, move forward that
			 * far in our buffer and try again.
			 */
			if (ret != amount)
			{
				PqGSSSendNext += ret;
				continue;
			}

			/* We've successfully sent whatever data was in that packet. */
			bytes_sent += PqGSSSendConsumed;

			/* All encrypted data was sent, our buffer is empty now. */
			PqGSSSendLength = PqGSSSendNext = PqGSSSendConsumed = 0;
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

		/* Create the next encrypted packet */
		major = gss_wrap(&minor, gctx, 1, GSS_C_QOP_DEFAULT,
						 &input, &conf_state, &output);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(_("GSSAPI wrap error"), major, minor);
			errno = ECONNRESET;
			return -1;
		}
		if (conf_state == 0)
		{
			ereport(COMMERROR,
					(errmsg("outgoing GSSAPI message would not use confidentiality")));
			errno = ECONNRESET;
			return -1;
		}
		if (output.length > PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))
		{
			ereport(COMMERROR,
					(errmsg("server tried to send oversize GSSAPI packet (%zu > %zu)",
							(size_t) output.length,
							PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))));
			errno = ECONNRESET;
			return -1;
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
	Assert(bytes_sent == len);
	Assert(bytes_sent == bytes_encrypted);

	return bytes_sent;
}

/*
 * Read up to len bytes of data into ptr from a GSSAPI-encrypted connection.
 *
 * The connection must be already set up for GSSAPI encryption (i.e., GSSAPI
 * transport negotiation is complete).
 *
 * Returns the number of data bytes read, or on failure, returns -1
 * with errno set appropriately.  For retryable errors, caller should call
 * again once the socket is ready.
 *
 * We treat fatal errors the same as in be_gssapi_write(), even though the
 * argument about infinite recursion doesn't apply here.
 */
ssize_t
be_gssapi_read(Port *port, void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input,
				output;
	ssize_t		ret;
	size_t		bytes_returned = 0;
	gss_ctx_id_t gctx = port->gss->ctx;

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
			ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength,
								  sizeof(uint32) - PqGSSRecvLength);

			/* If ret <= 0, secure_raw_read already set the correct errno */
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

		if (input.length > PQ_GSS_RECV_BUFFER_SIZE - sizeof(uint32))
		{
			ereport(COMMERROR,
					(errmsg("oversize GSSAPI packet sent by the client (%zu > %zu)",
							(size_t) input.length,
							PQ_GSS_RECV_BUFFER_SIZE - sizeof(uint32))));
			errno = ECONNRESET;
			return -1;
		}

		/*
		 * Read as much of the packet as we are able to on this call into
		 * wherever we left off from the last time we were called.
		 */
		ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength,
							  input.length - (PqGSSRecvLength - sizeof(uint32)));
		/* If ret <= 0, secure_raw_read already set the correct errno */
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
		 * the caller.
		 */
		output.value = NULL;
		output.length = 0;
		input.value = PqGSSRecvBuffer + sizeof(uint32);

		major = gss_unwrap(&minor, gctx, &input, &output, &conf_state, NULL);
		if (major != GSS_S_COMPLETE)
		{
			pg_GSS_error(_("GSSAPI unwrap error"), major, minor);
			errno = ECONNRESET;
			return -1;
		}
		if (conf_state == 0)
		{
			ereport(COMMERROR,
					(errmsg("incoming GSSAPI message did not use confidentiality")));
			errno = ECONNRESET;
			return -1;
		}

		memcpy(PqGSSResultBuffer, output.value, output.length);
		PqGSSResultLength = output.length;

		/* Our receive buffer is now empty, reset it */
		PqGSSRecvLength = 0;

		/* Release buffer storage allocated by GSSAPI */
		gss_release_buffer(&minor, &output);
	}

	return bytes_returned;
}

/*
 * Read the specified number of bytes off the wire, waiting using
 * WaitLatchOrSocket if we would block.
 *
 * Results are read into PqGSSRecvBuffer.
 *
 * Will always return either -1, to indicate a permanent error, or len.
 */
static ssize_t
read_or_wait(Port *port, ssize_t len)
{
	ssize_t		ret;

	/*
	 * Keep going until we either read in everything we were asked to, or we
	 * error out.
	 */
	while (PqGSSRecvLength < len)
	{
		ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength, len - PqGSSRecvLength);

		/*
		 * If we got back an error and it wasn't just
		 * EWOULDBLOCK/EAGAIN/EINTR, then give up.
		 */
		if (ret < 0 &&
			!(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR))
			return -1;

		/*
		 * Ok, we got back either a positive value, zero, or a negative result
		 * indicating we should retry.
		 *
		 * If it was zero or negative, then we wait on the socket to be
		 * readable again.
		 */
		if (ret <= 0)
		{
			WaitLatchOrSocket(MyLatch,
							  WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
							  port->sock, 0, WAIT_EVENT_GSS_OPEN_SERVER);

			/*
			 * If we got back zero bytes, and then waited on the socket to be
			 * readable and got back zero bytes on a second read, then this is
			 * EOF and the client hung up on us.
			 *
			 * If we did get data here, then we can just fall through and
			 * handle it just as if we got data the first time.
			 *
			 * Otherwise loop back to the top and try again.
			 */
			if (ret == 0)
			{
				ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength, len - PqGSSRecvLength);
				if (ret == 0)
					return -1;
			}
			if (ret < 0)
				continue;
		}

		PqGSSRecvLength += ret;
	}

	return len;
}

/*
 * Start up a GSSAPI-encrypted connection.  This performs GSSAPI
 * authentication; after this function completes, it is safe to call
 * be_gssapi_read and be_gssapi_write.  Returns -1 and logs on failure;
 * otherwise, returns 0 and marks the connection as ready for GSSAPI
 * encryption.
 *
 * Note that unlike the be_gssapi_read/be_gssapi_write functions, this
 * function WILL block on the socket to be ready for read/write (using
 * WaitLatchOrSocket) as appropriate while establishing the GSSAPI
 * session.
 */
ssize_t
secure_open_gssapi(Port *port)
{
	bool		complete_next = false;
	OM_uint32	major,
				minor;

	/*
	 * Allocate subsidiary Port data for GSSAPI operations.
	 */
	port->gss = (pg_gssinfo *)
		MemoryContextAllocZero(TopMemoryContext, sizeof(pg_gssinfo));

	/*
	 * Allocate buffers and initialize state variables.  By malloc'ing the
	 * buffers at this point, we avoid wasting static data space in processes
	 * that will never use them, and we ensure that the buffers are
	 * sufficiently aligned for the length-word accesses that we do in some
	 * places in this file.
	 */
	PqGSSSendBuffer = malloc(PQ_GSS_SEND_BUFFER_SIZE);
	PqGSSRecvBuffer = malloc(PQ_GSS_RECV_BUFFER_SIZE);
	PqGSSResultBuffer = malloc(PQ_GSS_RECV_BUFFER_SIZE);
	if (!PqGSSSendBuffer || !PqGSSRecvBuffer || !PqGSSResultBuffer)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));
	PqGSSSendLength = PqGSSSendNext = PqGSSSendConsumed = 0;
	PqGSSRecvLength = PqGSSResultLength = PqGSSResultNext = 0;

	/*
	 * Use the configured keytab, if there is one.  Unfortunately, Heimdal
	 * doesn't support the cred store extensions, so use the env var.
	 */
	if (pg_krb_server_keyfile != NULL && pg_krb_server_keyfile[0] != '\0')
	{
		if (setenv("KRB5_KTNAME", pg_krb_server_keyfile, 1) != 0)
		{
			/* The only likely failure cause is OOM, so use that errcode */
			ereport(FATAL,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not set environment: %m")));
		}
	}

	while (true)
	{
		ssize_t		ret;
		gss_buffer_desc input,
					output = GSS_C_EMPTY_BUFFER;

		/*
		 * The client always sends first, so try to go ahead and read the
		 * length and wait on the socket to be readable again if that fails.
		 */
		ret = read_or_wait(port, sizeof(uint32));
		if (ret < 0)
			return ret;

		/*
		 * Get the length for this packet from the length header.
		 */
		input.length = pg_ntoh32(*(uint32 *) PqGSSRecvBuffer);

		/* Done with the length, reset our buffer */
		PqGSSRecvLength = 0;

		/*
		 * During initialization, packets are always fully consumed and
		 * shouldn't ever be over PQ_GSS_RECV_BUFFER_SIZE in length.
		 *
		 * Verify on our side that the client doesn't do something funny.
		 */
		if (input.length > PQ_GSS_RECV_BUFFER_SIZE)
		{
			ereport(COMMERROR,
					(errmsg("oversize GSSAPI packet sent by the client (%zu > %d)",
							(size_t) input.length,
							PQ_GSS_RECV_BUFFER_SIZE)));
			return -1;
		}

		/*
		 * Get the rest of the packet so we can pass it to GSSAPI to accept
		 * the context.
		 */
		ret = read_or_wait(port, input.length);
		if (ret < 0)
			return ret;

		input.value = PqGSSRecvBuffer;

		/* Process incoming data.  (The client sends first.) */
		major = gss_accept_sec_context(&minor, &port->gss->ctx,
									   GSS_C_NO_CREDENTIAL, &input,
									   GSS_C_NO_CHANNEL_BINDINGS,
									   &port->gss->name, NULL, &output, NULL,
									   NULL, NULL);
		if (GSS_ERROR(major))
		{
			pg_GSS_error(_("could not accept GSSAPI security context"),
						 major, minor);
			gss_release_buffer(&minor, &output);
			return -1;
		}
		else if (!(major & GSS_S_CONTINUE_NEEDED))
		{
			/*
			 * rfc2744 technically permits context negotiation to be complete
			 * both with and without a packet to be sent.
			 */
			complete_next = true;
		}

		/* Done handling the incoming packet, reset our buffer */
		PqGSSRecvLength = 0;

		/*
		 * Check if we have data to send and, if we do, make sure to send it
		 * all
		 */
		if (output.length > 0)
		{
			uint32		netlen = pg_hton32(output.length);

			if (output.length > PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))
			{
				ereport(COMMERROR,
						(errmsg("server tried to send oversize GSSAPI packet (%zu > %zu)",
								(size_t) output.length,
								PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))));
				gss_release_buffer(&minor, &output);
				return -1;
			}

			memcpy(PqGSSSendBuffer, (char *) &netlen, sizeof(uint32));
			PqGSSSendLength += sizeof(uint32);

			memcpy(PqGSSSendBuffer + PqGSSSendLength, output.value, output.length);
			PqGSSSendLength += output.length;

			/* we don't bother with PqGSSSendConsumed here */

			while (PqGSSSendNext < PqGSSSendLength)
			{
				ret = secure_raw_write(port, PqGSSSendBuffer + PqGSSSendNext,
									   PqGSSSendLength - PqGSSSendNext);

				/*
				 * If we got back an error and it wasn't just
				 * EWOULDBLOCK/EAGAIN/EINTR, then give up.
				 */
				if (ret < 0 &&
					!(errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR))
				{
					gss_release_buffer(&minor, &output);
					return -1;
				}

				/* Wait and retry if we couldn't write yet */
				if (ret <= 0)
				{
					WaitLatchOrSocket(MyLatch,
									  WL_SOCKET_WRITEABLE | WL_EXIT_ON_PM_DEATH,
									  port->sock, 0, WAIT_EVENT_GSS_OPEN_SERVER);
					continue;
				}

				PqGSSSendNext += ret;
			}

			/* Done sending the packet, reset our buffer */
			PqGSSSendLength = PqGSSSendNext = 0;

			gss_release_buffer(&minor, &output);
		}

		/*
		 * If we got back that the connection is finished being set up, now
		 * that we've sent the last packet, exit our loop.
		 */
		if (complete_next)
			break;
	}

	/*
	 * Determine the max packet size which will fit in our buffer, after
	 * accounting for the length.  be_gssapi_write will need this.
	 */
	major = gss_wrap_size_limit(&minor, port->gss->ctx, 1, GSS_C_QOP_DEFAULT,
								PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32),
								&PqGSSMaxPktSize);

	if (GSS_ERROR(major))
	{
		pg_GSS_error(_("GSSAPI size check error"), major, minor);
		return -1;
	}

	port->gss->enc = true;

	return 0;
}

/*
 * Return if GSSAPI authentication was used on this connection.
 */
bool
be_gssapi_get_auth(Port *port)
{
	if (!port || !port->gss)
		return false;

	return port->gss->auth;
}

/*
 * Return if GSSAPI encryption is enabled and being used on this connection.
 */
bool
be_gssapi_get_enc(Port *port)
{
	if (!port || !port->gss)
		return false;

	return port->gss->enc;
}

/*
 * Return the GSSAPI principal used for authentication on this connection
 * (NULL if we did not perform GSSAPI authentication).
 */
const char *
be_gssapi_get_princ(Port *port)
{
	if (!port || !port->gss)
		return NULL;

	return port->gss->princ;
}
