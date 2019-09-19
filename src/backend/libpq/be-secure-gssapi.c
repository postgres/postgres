/*-------------------------------------------------------------------------
 *
 * be-secure-gssapi.c
 *  GSSAPI encryption support
 *
 * Portions Copyright (c) 2018-2019, PostgreSQL Global Development Group
 *
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


/*
 * Handle the encryption/decryption of data using GSSAPI.
 *
 * In the encrypted data stream on the wire, we break up the data
 * into packets where each packet starts with a sizeof(uint32)-byte
 * length (not allowed to be larger than the buffer sizes defined
 * below) and then the encrypted data of that length immediately
 * following.
 *
 * Encrypted data typically ends up being larger than the same data
 * unencrypted, so we use fixed-size buffers for handling the
 * encryption/decryption which are larger than PQComm's buffer will
 * typically be to minimize the times where we have to make multiple
 * packets and therefore sets of recv/send calls for a single
 * read/write call to us.
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
static int	PqGSSRecvLength;	/* End of data available in PqGSSRecvBuffer */

/* PqGSSResultBuffer is for *unencrypted* data */
static char PqGSSResultBuffer[PQ_GSS_RECV_BUFFER_SIZE];
static int	PqGSSResultPointer; /* Next index to read a byte from
								 * PqGSSResultBuffer */
static int	PqGSSResultLength;	/* End of data available in PqGSSResultBuffer */

uint32		max_packet_size;	/* Maximum size we can encrypt and fit the
								 * results into our output buffer */

/*
 * Attempt to write len bytes of data from ptr along a GSSAPI-encrypted connection.
 *
 * Connection must be fully established (including authentication step) before
 * calling.  Returns the bytes actually consumed once complete.  Data is
 * internally buffered; in the case of an incomplete write, the amount of data we
 * processed (encrypted into our output buffer to be sent) will be returned.  If
 * an error occurs or we would block, a negative value is returned and errno is
 * set appropriately.
 *
 * To continue writing in the case of EWOULDBLOCK and similar, call this function
 * again with matching ptr and len parameters.
 */
ssize_t
be_gssapi_write(Port *port, void *ptr, size_t len)
{
	size_t		bytes_to_encrypt = len;
	size_t		bytes_encrypted = 0;

	/*
	 * Loop through encrypting data and sending it out until
	 * secure_raw_write() complains (which would likely mean that the socket
	 * is non-blocking and the requested send() would block, or there was some
	 * kind of actual error) and then return.
	 */
	while (bytes_to_encrypt || PqGSSSendPointer)
	{
		OM_uint32	major,
					minor;
		gss_buffer_desc input,
					output;
		int			conf_state = 0;
		uint32		netlen;
		pg_gssinfo *gss = port->gss;

		/*
		 * Check if we have data in the encrypted output buffer that needs to
		 * be sent, and if so, try to send it.  If we aren't able to, return
		 * that back up to the caller.
		 */
		if (PqGSSSendPointer)
		{
			ssize_t		ret;
			ssize_t		amount = PqGSSSendPointer - PqGSSSendStart;

			ret = secure_raw_write(port, PqGSSSendBuffer + PqGSSSendStart, amount);
			if (ret <= 0)
			{
				/*
				 * If we encrypted some data and it's in our output buffer,
				 * but send() is saying that we would block, then tell the
				 * caller how far we got with encrypting the data so that they
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
			 * Check if this was a partial write, and if so, move forward that
			 * far in our buffer and try again.
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
		 * max_packet_size is the maximum amount of unencrypted data that,
		 * when encrypted, will fit into our encrypted-data output buffer.
		 *
		 * If we are being asked to send more than max_packet_size unencrypted
		 * data, then we will loop and create multiple packets, each with
		 * max_packet_size unencrypted data encrypted in them (at least, until
		 * secure_raw_write returns a failure saying we would be blocked, at
		 * which point we will let the caller know how far we got).
		 */
		if (bytes_to_encrypt > max_packet_size)
			input.length = max_packet_size;
		else
			input.length = bytes_to_encrypt;

		input.value = (char *) ptr + bytes_encrypted;

		output.value = NULL;
		output.length = 0;

		/* Create the next encrypted packet */
		major = gss_wrap(&minor, gss->ctx, 1, GSS_C_QOP_DEFAULT,
						 &input, &conf_state, &output);
		if (major != GSS_S_COMPLETE)
			pg_GSS_error(FATAL, gettext_noop("GSSAPI wrap error"), major, minor);

		if (conf_state == 0)
			ereport(FATAL,
					(errmsg("outgoing GSSAPI message would not use confidentiality")));

		if (output.length > PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))
			ereport(FATAL,
					(errmsg("server tried to send oversize GSSAPI packet (%zu > %zu)",
							(size_t) output.length,
							PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))));

		bytes_encrypted += input.length;
		bytes_to_encrypt -= input.length;

		/* 4 network-order length bytes, then payload */
		netlen = htonl(output.length);
		memcpy(PqGSSSendBuffer + PqGSSSendPointer, &netlen, sizeof(uint32));
		PqGSSSendPointer += sizeof(uint32);

		memcpy(PqGSSSendBuffer + PqGSSSendPointer, output.value, output.length);
		PqGSSSendPointer += output.length;
	}

	return bytes_encrypted;
}

/*
 * Read up to len bytes from a GSSAPI-encrypted connection into ptr.  Call
 * only after the connection has been fully established (i.e., GSSAPI
 * authentication is complete).  On success, returns the number of bytes
 * written into ptr; otherwise, returns -1 and sets errno appropriately.
 */
ssize_t
be_gssapi_read(Port *port, void *ptr, size_t len)
{
	OM_uint32	major,
				minor;
	gss_buffer_desc input,
				output;
	ssize_t		ret;
	size_t		bytes_to_return = len;
	size_t		bytes_returned = 0;
	int			conf_state = 0;
	pg_gssinfo *gss = port->gss;

	/*
	 * The goal here is to read an incoming encrypted packet, one at a time,
	 * decrypt it into our out buffer, returning to the caller what they asked
	 * for, and then saving anything else for the next call.
	 *
	 * First we look to see if we have unencrypted bytes available and, if so,
	 * copy those to the result.  If the caller asked for more than we had
	 * immediately available, then we try to read a packet off the wire and
	 * decrypt it.  If the read would block, then return the amount of
	 * unencrypted data we copied into the caller's ptr.
	 */
	while (bytes_to_return)
	{
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
		 * wire then we return the number of unencrypted bytes we were able to
		 * copy (if any, if we didn't copy any, then we return whatever
		 * secure_raw_read returned when we called it; likely -1) into the
		 * caller's ptr and wait to be called again, until we get a full
		 * packet to decrypt.
		 */

		/* Check if we have the size of the packet already in our buffer. */
		if (PqGSSRecvLength < sizeof(uint32))
		{
			/*
			 * We were not able to get the length of the packet last time, so
			 * we need to do that first.
			 */
			ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength,
								  sizeof(uint32) - PqGSSRecvLength);
			if (ret < 0)
				return bytes_returned ? bytes_returned : ret;

			PqGSSRecvLength += ret;

			/*
			 * If we only got part of the packet length, then return however
			 * many unencrypted bytes we copied to the caller and wait to be
			 * called again.
			 */
			if (PqGSSRecvLength < sizeof(uint32))
				return bytes_returned;
		}

		/*
		 * We have the length of the next packet at this point, so pull it out
		 * and then read whatever we have left of the packet to read.
		 */
		input.length = ntohl(*(uint32 *) PqGSSRecvBuffer);

		/* Check for over-length packet */
		if (input.length > PQ_GSS_RECV_BUFFER_SIZE - sizeof(uint32))
			ereport(FATAL,
					(errmsg("oversize GSSAPI packet sent by the client (%zu > %zu)",
							(size_t) input.length,
							PQ_GSS_RECV_BUFFER_SIZE - sizeof(uint32))));

		/*
		 * Read as much of the packet as we are able to on this call into
		 * wherever we left off from the last time we were called.
		 */
		ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength,
							  input.length - (PqGSSRecvLength - sizeof(uint32)));
		if (ret < 0)
			return bytes_returned ? bytes_returned : ret;

		PqGSSRecvLength += ret;

		/*
		 * If we got less than the rest of the packet then we need to return
		 * and be called again.  If we didn't have any bytes to return on this
		 * run then return -1 and set errno to EWOULDBLOCK.
		 */
		if (PqGSSRecvLength - sizeof(uint32) < input.length)
		{
			if (!bytes_returned)
			{
				errno = EWOULDBLOCK;
				return -1;
			}

			return bytes_returned;
		}

		/*
		 * We now have the full packet and we can perform the decryption and
		 * refill our output buffer, then loop back up to pass that back to
		 * the user.
		 */
		output.value = NULL;
		output.length = 0;
		input.value = PqGSSRecvBuffer + sizeof(uint32);

		major = gss_unwrap(&minor, gss->ctx, &input, &output, &conf_state, NULL);
		if (major != GSS_S_COMPLETE)
			pg_GSS_error(FATAL, gettext_noop("GSSAPI unwrap error"),
						 major, minor);

		if (conf_state == 0)
			ereport(FATAL,
					(errmsg("incoming GSSAPI message did not use confidentiality")));

		memcpy(PqGSSResultBuffer, output.value, output.length);

		PqGSSResultLength = output.length;

		/* Our buffer is now empty, reset it */
		PqGSSRecvLength = 0;

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
	while (PqGSSRecvLength != len)
	{
		ret = secure_raw_read(port, PqGSSRecvBuffer + PqGSSRecvLength, len - PqGSSRecvLength);

		/*
		 * If we got back an error and it wasn't just EWOULDBLOCK/EAGAIN, then
		 * give up.
		 */
		if (ret < 0 && !(errno == EWOULDBLOCK || errno == EAGAIN))
			return -1;

		/*
		 * Ok, we got back either a positive value, zero, or a negative result
		 * but EWOULDBLOCK or EAGAIN was set.
		 *
		 * If it was zero or negative, then we try to wait on the socket to be
		 * readable again.
		 */
		if (ret <= 0)
		{
			/*
			 * If we got back less than zero, indicating an error, and that
			 * wasn't just a EWOULDBLOCK/EAGAIN, then give up.
			 */
			if (ret < 0 && !(errno == EWOULDBLOCK || errno == EAGAIN))
				return -1;

			/*
			 * We got back either zero, or -1 with EWOULDBLOCK/EAGAIN, so wait
			 * on socket to be readable again.
			 */
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
			else
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

	/* initialize state variables */
	PqGSSSendPointer = PqGSSSendStart = PqGSSRecvLength = PqGSSResultPointer = PqGSSResultLength = 0;

	/*
	 * Use the configured keytab, if there is one.  Unfortunately, Heimdal
	 * doesn't support the cred store extensions, so use the env var.
	 */
	if (pg_krb_server_keyfile != NULL && strlen(pg_krb_server_keyfile) > 0)
		setenv("KRB5_KTNAME", pg_krb_server_keyfile, 1);

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
		input.length = ntohl(*(uint32 *) PqGSSRecvBuffer);

		/* Done with the length, reset our buffer */
		PqGSSRecvLength = 0;

		/*
		 * During initialization, packets are always fully consumed and
		 * shouldn't ever be over PQ_GSS_RECV_BUFFER_SIZE in length.
		 *
		 * Verify on our side that the client doesn't do something funny.
		 */
		if (input.length > PQ_GSS_RECV_BUFFER_SIZE)
			ereport(FATAL,
					(errmsg("oversize GSSAPI packet sent by the client (%zu > %d)",
							(size_t) input.length,
							PQ_GSS_RECV_BUFFER_SIZE)));

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
			pg_GSS_error(ERROR, gettext_noop("could not accept GSSAPI security context"),
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
		if (output.length != 0)
		{
			uint32		netlen = htonl(output.length);

			if (output.length > PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))
				ereport(FATAL,
						(errmsg("server tried to send oversize GSSAPI packet (%zu > %zu)",
								(size_t) output.length,
								PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32))));

			memcpy(PqGSSSendBuffer, (char *) &netlen, sizeof(uint32));
			PqGSSSendPointer += sizeof(uint32);

			memcpy(PqGSSSendBuffer + PqGSSSendPointer, output.value, output.length);
			PqGSSSendPointer += output.length;

			while (PqGSSSendStart != sizeof(uint32) + output.length)
			{
				ret = secure_raw_write(port, PqGSSSendBuffer + PqGSSSendStart, sizeof(uint32) + output.length - PqGSSSendStart);
				if (ret <= 0)
				{
					WaitLatchOrSocket(MyLatch,
									  WL_SOCKET_WRITEABLE | WL_EXIT_ON_PM_DEATH,
									  port->sock, 0, WAIT_EVENT_GSS_OPEN_SERVER);
					continue;
				}

				PqGSSSendStart += ret;
			}

			/* Done sending the packet, reset our buffer */
			PqGSSSendStart = PqGSSSendPointer = 0;

			gss_release_buffer(&minor, &output);
		}

		/*
		 * If we got back that the connection is finished being set up, now
		 * that's we've sent the last packet, exit our loop.
		 */
		if (complete_next)
			break;
	}

	/*
	 * Determine the max packet size which will fit in our buffer, after
	 * accounting for the length
	 */
	major = gss_wrap_size_limit(&minor, port->gss->ctx, 1, GSS_C_QOP_DEFAULT,
								PQ_GSS_SEND_BUFFER_SIZE - sizeof(uint32), &max_packet_size);

	if (GSS_ERROR(major))
		pg_GSS_error(FATAL, gettext_noop("GSSAPI size check error"),
					 major, minor);

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
 * Return the GSSAPI principal used for authentication on this connection.
 */
const char *
be_gssapi_get_princ(Port *port)
{
	if (!port || !port->gss->auth)
		return NULL;

	return port->gss->princ;
}
