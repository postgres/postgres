/*
 *	PostgreSQL type definitions for the INET and CIDR types.
 *
 *	src/backend/utils/adt/network.c
 *
 *	Jon Postel RIP 16 Oct 1998
 */

#include "postgres.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/stratnum.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hashutils.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"


static int32 network_cmp_internal(inet *a1, inet *a2);
static List *match_network_function(Node *leftop,
									Node *rightop,
									int indexarg,
									Oid funcid,
									Oid opfamily);
static List *match_network_subset(Node *leftop,
								  Node *rightop,
								  bool is_eq,
								  Oid opfamily);
static bool addressOK(unsigned char *a, int bits, int family);
static inet *internal_inetpl(inet *ip, int64 addend);


/*
 * Common INET/CIDR input routine
 */
static inet *
network_in(char *src, bool is_cidr)
{
	int			bits;
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	/*
	 * First, check to see if this is an IPv6 or IPv4 address.  IPv6 addresses
	 * will have a : somewhere in them (several, in fact) so if there is one
	 * present, assume it's V6, otherwise assume it's V4.
	 */

	if (strchr(src, ':') != NULL)
		ip_family(dst) = PGSQL_AF_INET6;
	else
		ip_family(dst) = PGSQL_AF_INET;

	bits = inet_net_pton(ip_family(dst), src, ip_addr(dst),
						 is_cidr ? ip_addrsize(dst) : -1);
	if ((bits < 0) || (bits > ip_maxbits(dst)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		/* translator: first %s is inet or cidr */
				 errmsg("invalid input syntax for type %s: \"%s\"",
						is_cidr ? "cidr" : "inet", src)));

	/*
	 * Error check: CIDR values must not have any bits set beyond the masklen.
	 */
	if (is_cidr)
	{
		if (!addressOK(ip_addr(dst), bits, ip_family(dst)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid cidr value: \"%s\"", src),
					 errdetail("Value has bits set to right of mask.")));
	}

	ip_bits(dst) = bits;
	SET_INET_VARSIZE(dst);

	return dst;
}

Datum
inet_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, false));
}

Datum
cidr_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, true));
}


/*
 * Common INET/CIDR output routine
 */
static char *
network_out(inet *src, bool is_cidr)
{
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];
	char	   *dst;
	int			len;

	dst = inet_net_ntop(ip_family(src), ip_addr(src), ip_bits(src),
						tmp, sizeof(tmp));
	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* For CIDR, add /n if not present */
	if (is_cidr && strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(src));
	}

	return pstrdup(tmp);
}

Datum
inet_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);

	PG_RETURN_CSTRING(network_out(src, false));
}

Datum
cidr_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);

	PG_RETURN_CSTRING(network_out(src, true));
}


/*
 *		network_recv		- converts external binary format to inet
 *
 * The external representation is (one byte apiece for)
 * family, bits, is_cidr, address length, address in network byte order.
 *
 * Presence of is_cidr is largely for historical reasons, though it might
 * allow some code-sharing on the client side.  We send it correctly on
 * output, but ignore the value on input.
 */
static inet *
network_recv(StringInfo buf, bool is_cidr)
{
	inet	   *addr;
	char	   *addrptr;
	int			bits;
	int			nb,
				i;

	/* make sure any unused bits in a CIDR value are zeroed */
	addr = (inet *) palloc0(sizeof(inet));

	ip_family(addr) = pq_getmsgbyte(buf);
	if (ip_family(addr) != PGSQL_AF_INET &&
		ip_family(addr) != PGSQL_AF_INET6)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid address family in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));
	bits = pq_getmsgbyte(buf);
	if (bits < 0 || bits > ip_maxbits(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid bits in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));
	ip_bits(addr) = bits;
	i = pq_getmsgbyte(buf);		/* ignore is_cidr */
	nb = pq_getmsgbyte(buf);
	if (nb != ip_addrsize(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid length in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));

	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		addrptr[i] = pq_getmsgbyte(buf);

	/*
	 * Error check: CIDR values must not have any bits set beyond the masklen.
	 */
	if (is_cidr)
	{
		if (!addressOK(ip_addr(addr), bits, ip_family(addr)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid external \"cidr\" value"),
					 errdetail("Value has bits set to right of mask.")));
	}

	SET_INET_VARSIZE(addr);

	return addr;
}

Datum
inet_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INET_P(network_recv(buf, false));
}

Datum
cidr_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INET_P(network_recv(buf, true));
}


/*
 *		network_send		- converts inet to binary format
 */
static bytea *
network_send(inet *addr, bool is_cidr)
{
	StringInfoData buf;
	char	   *addrptr;
	int			nb,
				i;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, ip_family(addr));
	pq_sendbyte(&buf, ip_bits(addr));
	pq_sendbyte(&buf, is_cidr);
	nb = ip_addrsize(addr);
	if (nb < 0)
		nb = 0;
	pq_sendbyte(&buf, nb);
	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		pq_sendbyte(&buf, addrptr[i]);
	return pq_endtypsend(&buf);
}

Datum
inet_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);

	PG_RETURN_BYTEA_P(network_send(addr, false));
}

Datum
cidr_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);

	PG_RETURN_BYTEA_P(network_send(addr, true));
}


Datum
inet_to_cidr(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits;

	bits = ip_bits(src);

	/* safety check */
	if ((bits < 0) || (bits > ip_maxbits(src)))
		elog(ERROR, "invalid inet bit length: %d", bits);

	PG_RETURN_INET_P(cidr_set_masklen_internal(src, bits));
}

Datum
inet_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits = PG_GETARG_INT32(1);
	inet	   *dst;

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	/* clone the original data */
	dst = (inet *) palloc(VARSIZE_ANY(src));
	memcpy(dst, src, VARSIZE_ANY(src));

	ip_bits(dst) = bits;

	PG_RETURN_INET_P(dst);
}

Datum
cidr_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits = PG_GETARG_INT32(1);

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	PG_RETURN_INET_P(cidr_set_masklen_internal(src, bits));
}

/*
 * Copy src and set mask length to 'bits' (which must be valid for the family)
 */
inet *
cidr_set_masklen_internal(const inet *src, int bits)
{
	inet	   *dst = (inet *) palloc0(sizeof(inet));

	ip_family(dst) = ip_family(src);
	ip_bits(dst) = bits;

	if (bits > 0)
	{
		Assert(bits <= ip_maxbits(dst));

		/* Clone appropriate bytes of the address, leaving the rest 0 */
		memcpy(ip_addr(dst), ip_addr(src), (bits + 7) / 8);

		/* Clear any unwanted bits in the last partial byte */
		if (bits % 8)
			ip_addr(dst)[bits / 8] &= ~(0xFF >> (bits % 8));
	}

	/* Set varlena header correctly */
	SET_INET_VARSIZE(dst);

	return dst;
}

/*
 *	Basic comparison function for sorting and inet/cidr comparisons.
 *
 * Comparison is first on the common bits of the network part, then on
 * the length of the network part, and then on the whole unmasked address.
 * The effect is that the network part is the major sort key, and for
 * equal network parts we sort on the host part.  Note this is only sane
 * for CIDR if address bits to the right of the mask are guaranteed zero;
 * otherwise logically-equal CIDRs might compare different.
 */

static int32
network_cmp_internal(inet *a1, inet *a2)
{
	if (ip_family(a1) == ip_family(a2))
	{
		int			order;

		order = bitncmp(ip_addr(a1), ip_addr(a2),
						Min(ip_bits(a1), ip_bits(a2)));
		if (order != 0)
			return order;
		order = ((int) ip_bits(a1)) - ((int) ip_bits(a2));
		if (order != 0)
			return order;
		return bitncmp(ip_addr(a1), ip_addr(a2), ip_maxbits(a1));
	}

	return ip_family(a1) - ip_family(a2);
}

Datum
network_cmp(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_INT32(network_cmp_internal(a1, a2));
}

/*
 *	Boolean ordering tests.
 */
Datum
network_lt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) < 0);
}

Datum
network_le(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) <= 0);
}

Datum
network_eq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) == 0);
}

Datum
network_ge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) >= 0);
}

Datum
network_gt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) > 0);
}

Datum
network_ne(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) != 0);
}

/*
 * MIN/MAX support functions.
 */
Datum
network_smaller(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (network_cmp_internal(a1, a2) < 0)
		PG_RETURN_INET_P(a1);
	else
		PG_RETURN_INET_P(a2);
}

Datum
network_larger(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (network_cmp_internal(a1, a2) > 0)
		PG_RETURN_INET_P(a1);
	else
		PG_RETURN_INET_P(a2);
}

/*
 * Support function for hash indexes on inet/cidr.
 */
Datum
hashinet(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);
	int			addrsize = ip_addrsize(addr);

	/* XXX this assumes there are no pad bytes in the data structure */
	return hash_any((unsigned char *) VARDATA_ANY(addr), addrsize + 2);
}

Datum
hashinetextended(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);
	int			addrsize = ip_addrsize(addr);

	return hash_any_extended((unsigned char *) VARDATA_ANY(addr), addrsize + 2,
							 PG_GETARG_INT64(1));
}

/*
 *	Boolean network-inclusion tests.
 */
Datum
network_sub(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) > ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_subeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) >= ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_sup(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) < ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_supeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) <= ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_overlap(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(bitncmp(ip_addr(a1), ip_addr(a2),
							   Min(ip_bits(a1), ip_bits(a2))) == 0);
	}

	PG_RETURN_BOOL(false);
}

/*
 * Planner support function for network subset/superset operators
 */
Datum
network_subset_support(PG_FUNCTION_ARGS)
{
	Node	   *rawreq = (Node *) PG_GETARG_POINTER(0);
	Node	   *ret = NULL;

	if (IsA(rawreq, SupportRequestIndexCondition))
	{
		/* Try to convert operator/function call to index conditions */
		SupportRequestIndexCondition *req = (SupportRequestIndexCondition *) rawreq;

		if (is_opclause(req->node))
		{
			OpExpr	   *clause = (OpExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_network_function((Node *) linitial(clause->args),
									   (Node *) lsecond(clause->args),
									   req->indexarg,
									   req->funcid,
									   req->opfamily);
		}
		else if (is_funcclause(req->node))	/* be paranoid */
		{
			FuncExpr   *clause = (FuncExpr *) req->node;

			Assert(list_length(clause->args) == 2);
			ret = (Node *)
				match_network_function((Node *) linitial(clause->args),
									   (Node *) lsecond(clause->args),
									   req->indexarg,
									   req->funcid,
									   req->opfamily);
		}
	}

	PG_RETURN_POINTER(ret);
}

/*
 * match_network_function
 *	  Try to generate an indexqual for a network subset/superset function.
 *
 * This layer is just concerned with identifying the function and swapping
 * the arguments if necessary.
 */
static List *
match_network_function(Node *leftop,
					   Node *rightop,
					   int indexarg,
					   Oid funcid,
					   Oid opfamily)
{
	switch (funcid)
	{
		case F_NETWORK_SUB:
			/* indexkey must be on the left */
			if (indexarg != 0)
				return NIL;
			return match_network_subset(leftop, rightop, false, opfamily);

		case F_NETWORK_SUBEQ:
			/* indexkey must be on the left */
			if (indexarg != 0)
				return NIL;
			return match_network_subset(leftop, rightop, true, opfamily);

		case F_NETWORK_SUP:
			/* indexkey must be on the right */
			if (indexarg != 1)
				return NIL;
			return match_network_subset(rightop, leftop, false, opfamily);

		case F_NETWORK_SUPEQ:
			/* indexkey must be on the right */
			if (indexarg != 1)
				return NIL;
			return match_network_subset(rightop, leftop, true, opfamily);

		default:

			/*
			 * We'd only get here if somebody attached this support function
			 * to an unexpected function.  Maybe we should complain, but for
			 * now, do nothing.
			 */
			return NIL;
	}
}

/*
 * match_network_subset
 *	  Try to generate an indexqual for a network subset function.
 */
static List *
match_network_subset(Node *leftop,
					 Node *rightop,
					 bool is_eq,
					 Oid opfamily)
{
	List	   *result;
	Datum		rightopval;
	Oid			datatype = INETOID;
	Oid			opr1oid;
	Oid			opr2oid;
	Datum		opr1right;
	Datum		opr2right;
	Expr	   *expr;

	/*
	 * Can't do anything with a non-constant or NULL comparison value.
	 *
	 * Note that since we restrict ourselves to cases with a hard constant on
	 * the RHS, it's a-fortiori a pseudoconstant, and we don't need to worry
	 * about verifying that.
	 */
	if (!IsA(rightop, Const) ||
		((Const *) rightop)->constisnull)
		return NIL;
	rightopval = ((Const *) rightop)->constvalue;

	/*
	 * Must check that index's opfamily supports the operators we will want to
	 * apply.
	 *
	 * We insist on the opfamily being the specific one we expect, else we'd
	 * do the wrong thing if someone were to make a reverse-sort opfamily with
	 * the same operators.
	 */
	if (opfamily != NETWORK_BTREE_FAM_OID)
		return NIL;

	/*
	 * create clause "key >= network_scan_first( rightopval )", or ">" if the
	 * operator disallows equality.
	 *
	 * Note: seeing that this function supports only fixed values for opfamily
	 * and datatype, we could just hard-wire the operator OIDs instead of
	 * looking them up.  But for now it seems better to be general.
	 */
	if (is_eq)
	{
		opr1oid = get_opfamily_member(opfamily, datatype, datatype,
									  BTGreaterEqualStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no >= operator for opfamily %u", opfamily);
	}
	else
	{
		opr1oid = get_opfamily_member(opfamily, datatype, datatype,
									  BTGreaterStrategyNumber);
		if (opr1oid == InvalidOid)
			elog(ERROR, "no > operator for opfamily %u", opfamily);
	}

	opr1right = network_scan_first(rightopval);

	expr = make_opclause(opr1oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1,
											InvalidOid, /* not collatable */
											-1, opr1right,
											false, false),
						 InvalidOid, InvalidOid);
	result = list_make1(expr);

	/* create clause "key <= network_scan_last( rightopval )" */

	opr2oid = get_opfamily_member(opfamily, datatype, datatype,
								  BTLessEqualStrategyNumber);
	if (opr2oid == InvalidOid)
		elog(ERROR, "no <= operator for opfamily %u", opfamily);

	opr2right = network_scan_last(rightopval);

	expr = make_opclause(opr2oid, BOOLOID, false,
						 (Expr *) leftop,
						 (Expr *) makeConst(datatype, -1,
											InvalidOid, /* not collatable */
											-1, opr2right,
											false, false),
						 InvalidOid, InvalidOid);
	result = lappend(result, expr);

	return result;
}


/*
 * Extract data from a network datatype.
 */
Datum
network_host(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *ptr;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	/* force display of max bits, regardless of masklen... */
	if (inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
					  tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* Suppress /n if present (shouldn't happen now) */
	if ((ptr = strchr(tmp, '/')) != NULL)
		*ptr = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

/*
 * network_show implements the inet and cidr casts to text.  This is not
 * quite the same behavior as network_out, hence we can't drop it in favor
 * of CoerceViaIO.
 */
Datum
network_show(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int			len;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	if (inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
					  tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* Add /n if not present (which it won't be) */
	if (strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(ip));
	}

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
inet_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *dst;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	dst = inet_net_ntop(ip_family(ip), ip_addr(ip),
						ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
cidr_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *dst;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	dst = inet_cidr_ntop(ip_family(ip), ip_addr(ip),
						 ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format cidr value: %m")));

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
network_masklen(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);

	PG_RETURN_INT32(ip_bits(ip));
}

Datum
network_family(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);

	switch (ip_family(ip))
	{
		case PGSQL_AF_INET:
			PG_RETURN_INT32(4);
			break;
		case PGSQL_AF_INET6:
			PG_RETURN_INT32(6);
			break;
		default:
			PG_RETURN_INT32(0);
			break;
	}
}

Datum
network_broadcast(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(sizeof(inet));

	maxbytes = ip_addrsize(ip);
	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	for (byte = 0; byte < maxbytes; byte++)
	{
		if (bits >= 8)
		{
			mask = 0x00;
			bits -= 8;
		}
		else if (bits == 0)
			mask = 0xff;
		else
		{
			mask = 0xff >> bits;
			bits = 0;
		}

		b[byte] = a[byte] | mask;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_network(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(sizeof(inet));

	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	byte = 0;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = a[byte] & mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_netmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(sizeof(inet));

	bits = ip_bits(ip);
	b = ip_addr(dst);

	byte = 0;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_hostmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(sizeof(inet));

	maxbytes = ip_addrsize(ip);
	bits = ip_maxbits(ip) - ip_bits(ip);
	b = ip_addr(dst);

	byte = maxbytes - 1;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff >> (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte--;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

/*
 * Returns true if the addresses are from the same family, or false.  Used to
 * check that we can create a network which contains both of the networks.
 */
Datum
inet_same_family(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(ip_family(a1) == ip_family(a2));
}

/*
 * Returns the smallest CIDR which contains both of the inputs.
 */
Datum
inet_merge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0),
			   *a2 = PG_GETARG_INET_PP(1);
	int			commonbits;

	if (ip_family(a1) != ip_family(a2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot merge addresses from different families")));

	commonbits = bitncommon(ip_addr(a1), ip_addr(a2),
							Min(ip_bits(a1), ip_bits(a2)));

	PG_RETURN_INET_P(cidr_set_masklen_internal(a1, commonbits));
}

/*
 * Convert a value of a network datatype to an approximate scalar value.
 * This is used for estimating selectivities of inequality operators
 * involving network types.
 *
 * On failure (e.g., unsupported typid), set *failure to true;
 * otherwise, that variable is not changed.
 */
double
convert_network_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case INETOID:
		case CIDROID:
			{
				inet	   *ip = DatumGetInetPP(value);
				int			len;
				double		res;
				int			i;

				/*
				 * Note that we don't use the full address for IPv6.
				 */
				if (ip_family(ip) == PGSQL_AF_INET)
					len = 4;
				else
					len = 5;

				res = ip_family(ip);
				for (i = 0; i < len; i++)
				{
					res *= 256;
					res += ip_addr(ip)[i];
				}
				return res;
			}
		case MACADDROID:
			{
				macaddr    *mac = DatumGetMacaddrP(value);
				double		res;

				res = (mac->a << 16) | (mac->b << 8) | (mac->c);
				res *= 256 * 256 * 256;
				res += (mac->d << 16) | (mac->e << 8) | (mac->f);
				return res;
			}
		case MACADDR8OID:
			{
				macaddr8   *mac = DatumGetMacaddr8P(value);
				double		res;

				res = (mac->a << 24) | (mac->b << 16) | (mac->c << 8) | (mac->d);
				res *= ((double) 256) * 256 * 256 * 256;
				res += (mac->e << 24) | (mac->f << 16) | (mac->g << 8) | (mac->h);
				return res;
			}
	}

	*failure = true;
	return 0;
}

/*
 * int
 * bitncmp(l, r, n)
 *		compare bit masks l and r, for n bits.
 * return:
 *		<0, >0, or 0 in the libc tradition.
 * note:
 *		network byte order assumed.  this means 192.5.5.240/28 has
 *		0x11110000 in its fourth octet.
 * author:
 *		Paul Vixie (ISC), June 1996
 */
int
bitncmp(const unsigned char *l, const unsigned char *r, int n)
{
	unsigned int lb,
				rb;
	int			x,
				b;

	b = n / 8;
	x = memcmp(l, r, b);
	if (x || (n % 8) == 0)
		return x;

	lb = l[b];
	rb = r[b];
	for (b = n % 8; b > 0; b--)
	{
		if (IS_HIGHBIT_SET(lb) != IS_HIGHBIT_SET(rb))
		{
			if (IS_HIGHBIT_SET(lb))
				return 1;
			return -1;
		}
		lb <<= 1;
		rb <<= 1;
	}
	return 0;
}

/*
 * bitncommon: compare bit masks l and r, for up to n bits.
 *
 * Returns the number of leading bits that match (0 to n).
 */
int
bitncommon(const unsigned char *l, const unsigned char *r, int n)
{
	int			byte,
				nbits;

	/* number of bits to examine in last byte */
	nbits = n % 8;

	/* check whole bytes */
	for (byte = 0; byte < n / 8; byte++)
	{
		if (l[byte] != r[byte])
		{
			/* at least one bit in the last byte is not common */
			nbits = 7;
			break;
		}
	}

	/* check bits in last partial byte */
	if (nbits != 0)
	{
		/* calculate diff of first non-matching bytes */
		unsigned int diff = l[byte] ^ r[byte];

		/* compare the bits from the most to the least */
		while ((diff >> (8 - nbits)) != 0)
			nbits--;
	}

	return (8 * byte) + nbits;
}


/*
 * Verify a CIDR address is OK (doesn't have bits set past the masklen)
 */
static bool
addressOK(unsigned char *a, int bits, int family)
{
	int			byte;
	int			nbits;
	int			maxbits;
	int			maxbytes;
	unsigned char mask;

	if (family == PGSQL_AF_INET)
	{
		maxbits = 32;
		maxbytes = 4;
	}
	else
	{
		maxbits = 128;
		maxbytes = 16;
	}
	Assert(bits <= maxbits);

	if (bits == maxbits)
		return true;

	byte = bits / 8;

	nbits = bits % 8;
	mask = 0xff;
	if (bits != 0)
		mask >>= nbits;

	while (byte < maxbytes)
	{
		if ((a[byte] & mask) != 0)
			return false;
		mask = 0xff;
		byte++;
	}

	return true;
}


/*
 * These functions are used by planner to generate indexscan limits
 * for clauses a << b and a <<= b
 */

/* return the minimal value for an IP on a given network */
Datum
network_scan_first(Datum in)
{
	return DirectFunctionCall1(network_network, in);
}

/*
 * return "last" IP on a given network. It's the broadcast address,
 * however, masklen has to be set to its max bits, since
 * 192.168.0.255/24 is considered less than 192.168.0.255/32
 *
 * inet_set_masklen() hacked to max out the masklength to 128 for IPv6
 * and 32 for IPv4 when given '-1' as argument.
 */
Datum
network_scan_last(Datum in)
{
	return DirectFunctionCall2(inet_set_masklen,
							   DirectFunctionCall1(network_broadcast, in),
							   Int32GetDatum(-1));
}


/*
 * IP address that the client is connecting from (NULL if Unix socket)
 */
Datum
inet_client_addr(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		remote_host[NI_MAXHOST];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_host[0] = '\0';

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(port->raddr.addr.ss_family, remote_host);

	PG_RETURN_INET_P(network_in(remote_host, false));
}


/*
 * port that the client is connecting from (NULL if Unix socket)
 */
Datum
inet_client_port(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		remote_port[NI_MAXSERV];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_port[0] = '\0';

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 NULL, 0,
							 remote_port, sizeof(remote_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in, CStringGetDatum(remote_port)));
}


/*
 * IP address that the server accepted the connection on (NULL if Unix socket)
 */
Datum
inet_server_addr(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		local_host[NI_MAXHOST];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->laddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	local_host[0] = '\0';

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 local_host, sizeof(local_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(port->laddr.addr.ss_family, local_host);

	PG_RETURN_INET_P(network_in(local_host, false));
}


/*
 * port that the server accepted the connection on (NULL if Unix socket)
 */
Datum
inet_server_port(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		local_port[NI_MAXSERV];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->laddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	local_port[0] = '\0';

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 NULL, 0,
							 local_port, sizeof(local_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in, CStringGetDatum(local_port)));
}


Datum
inetnot(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = ~pip[nb];
	}
	ip_bits(dst) = ip_bits(ip);

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


Datum
inetand(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot AND inet values of different sizes")));
	else
	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = pip[nb] & pip2[nb];
	}
	ip_bits(dst) = Max(ip_bits(ip), ip_bits(ip2));

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


Datum
inetor(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot OR inet values of different sizes")));
	else
	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		unsigned char *pdst = ip_addr(dst);

		while (--nb >= 0)
			pdst[nb] = pip[nb] | pip2[nb];
	}
	ip_bits(dst) = Max(ip_bits(ip), ip_bits(ip2));

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


static inet *
internal_inetpl(inet *ip, int64 addend)
{
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pdst = ip_addr(dst);
		int			carry = 0;

		while (--nb >= 0)
		{
			carry = pip[nb] + (int) (addend & 0xFF) + carry;
			pdst[nb] = (unsigned char) (carry & 0xFF);
			carry >>= 8;

			/*
			 * We have to be careful about right-shifting addend because
			 * right-shift isn't portable for negative values, while simply
			 * dividing by 256 doesn't work (the standard rounding is in the
			 * wrong direction, besides which there may be machines out there
			 * that round the wrong way).  So, explicitly clear the low-order
			 * byte to remove any doubt about the correct result of the
			 * division, and then divide rather than shift.
			 */
			addend &= ~((int64) 0xFF);
			addend /= 0x100;
		}

		/*
		 * At this point we should have addend and carry both zero if original
		 * addend was >= 0, or addend -1 and carry 1 if original addend was <
		 * 0.  Anything else means overflow.
		 */
		if (!((addend == 0 && carry == 0) ||
			  (addend == -1 && carry == 1)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("result is out of range")));
	}

	ip_bits(dst) = ip_bits(ip);
	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	return dst;
}


Datum
inetpl(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int64		addend = PG_GETARG_INT64(1);

	PG_RETURN_INET_P(internal_inetpl(ip, addend));
}


Datum
inetmi_int8(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int64		addend = PG_GETARG_INT64(1);

	PG_RETURN_INET_P(internal_inetpl(ip, -addend));
}


Datum
inetmi(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	int64		res = 0;

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot subtract inet values of different sizes")));
	else
	{
		/*
		 * We form the difference using the traditional complement, increment,
		 * and add rule, with the increment part being handled by starting the
		 * carry off at 1.  If you don't think integer arithmetic is done in
		 * two's complement, too bad.
		 */
		int			nb = ip_addrsize(ip);
		int			byte = 0;
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		int			carry = 1;

		while (--nb >= 0)
		{
			int			lobyte;

			carry = pip[nb] + (~pip2[nb] & 0xFF) + carry;
			lobyte = carry & 0xFF;
			if (byte < sizeof(int64))
			{
				res |= ((int64) lobyte) << (byte * 8);
			}
			else
			{
				/*
				 * Input wider than int64: check for overflow.  All bytes to
				 * the left of what will fit should be 0 or 0xFF, depending on
				 * sign of the now-complete result.
				 */
				if ((res < 0) ? (lobyte != 0xFF) : (lobyte != 0))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("result is out of range")));
			}
			carry >>= 8;
			byte++;
		}

		/*
		 * If input is narrower than int64, overflow is not possible, but we
		 * have to do proper sign extension.
		 */
		if (carry == 0 && byte < sizeof(int64))
			res |= ((uint64) (int64) -1) << (byte * 8);
	}

	PG_RETURN_INT64(res);
}


/*
 * clean_ipv6_addr --- remove any '%zone' part from an IPv6 address string
 *
 * XXX This should go away someday!
 *
 * This is a kluge needed because we don't yet support zones in stored inet
 * values.  Since the result of getnameinfo() might include a zone spec,
 * call this to remove it anywhere we want to feed getnameinfo's output to
 * network_in.  Beats failing entirely.
 *
 * An alternative approach would be to let network_in ignore %-parts for
 * itself, but that would mean we'd silently drop zone specs in user input,
 * which seems not such a good idea.
 */
void
clean_ipv6_addr(int addr_family, char *addr)
{
#ifdef HAVE_IPV6
	if (addr_family == AF_INET6)
	{
		char	   *pct = strchr(addr, '%');

		if (pct)
			*pct = '\0';
	}
#endif
}
