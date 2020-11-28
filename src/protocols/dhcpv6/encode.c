/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file protocols/dhcpv6/encode.c
 * @brief Functions to encode DHCP options.
 *
 * @author Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 *
 * @copyright 2018 The freeradius server project
 * @copyright 2018 NetworkRADIUS SARL (legal@networkradius.com)
 */
#include <stdint.h>
#include <stddef.h>
#include <talloc.h>
#include <freeradius-devel/util/dbuff.h>
#include <freeradius-devel/util/pair.h>
#include <freeradius-devel/util/types.h>
#include <freeradius-devel/util/proto.h>
#include <freeradius-devel/util/struct.h>
#include <freeradius-devel/util/dns.h>
#include <freeradius-devel/io/test_point.h>

#include "dhcpv6.h"
#include "attrs.h"

static ssize_t encode_value(fr_dbuff_t *dbuff,
			    fr_da_stack_t *da_stack, unsigned int depth,
			    fr_cursor_t *cursor, void *encoder_ctx);

static ssize_t encode_rfc_hdr(fr_dbuff_t *dbuff,
			      fr_da_stack_t *da_stack, unsigned int depth,
			      fr_cursor_t *cursor, void *encoder_ctx);

static ssize_t encode_tlv_hdr(fr_dbuff_t *dbuff,
			      fr_da_stack_t *da_stack, unsigned int depth,
			      fr_cursor_t *cursor, void *encoder_ctx);

static ssize_t encode_tlv(fr_dbuff_t *dbuff,
			  fr_da_stack_t *da_stack, unsigned int depth,
			  fr_cursor_t *cursor, void *encoder_ctx);

/** Macro-like function for encoding an option header
 *
 *    0                   1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |          option-code          |           option-len          |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * @param[out] dbuff		Where to write the 4 byte option header.
 * @param[in] option		The option number (host byte order).
 * @param[in] data_len		The length of the option (host byte order).
 * @return
 *	- <0	How much data would have been required as a negative value.
 *	- 4	The length of data written.
 */
static inline ssize_t encode_option_hdr(fr_dbuff_t *dbuff, uint16_t option, size_t data_len)
{
	fr_dbuff_t	work_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);

	FR_DBUFF_IN_RETURN(&work_dbuff, option);
	FR_DBUFF_IN_RETURN(&work_dbuff, (uint16_t) data_len);

	return fr_dbuff_set(dbuff, &work_dbuff);
}


static int8_t pair_sort_increasing(void const *a, void const *b)
{
	fr_pair_t const *my_a = a;
	fr_pair_t const *my_b = b;

	return (my_a->da->attr > my_b->da->attr) - (my_a->da->attr < my_b->da->attr);
}

static ssize_t encode_value(fr_dbuff_t *dbuff,
			    fr_da_stack_t *da_stack, unsigned int depth,
			    fr_cursor_t *cursor, void *encoder_ctx)
{
	ssize_t			slen;
	fr_dbuff_t		work_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_pair_t const		*vp = fr_cursor_current(cursor);
	fr_dict_attr_t const	*da = da_stack->da[depth];

	VP_VERIFY(vp);
	FR_PROTO_STACK_PRINT(da_stack, depth);

	/*
	 *	Pack multiple attributes into into a single option
	 */
	if (vp->da->type == FR_TYPE_STRUCT) {
		fr_cursor_t child_cursor;
		fr_pair_t *sorted = fr_cursor_current(cursor); /* NOT const */

		fr_assert(vp->da == da);

		fr_pair_list_sort(&sorted->vp_group, pair_sort_increasing);
		fr_cursor_init(&child_cursor, &sorted->vp_group);

		/*
		 *	Build the da_stack for the new structure.
		 */
		vp = fr_cursor_head(&child_cursor);
		fr_proto_da_stack_build(da_stack, vp ? vp->da : NULL);

		slen = fr_struct_to_network(&work_dbuff, da_stack, depth, cursor, encoder_ctx, encode_value, encode_tlv);
		if (slen <= 0) return slen;

		(void) fr_cursor_next(cursor); /* skip the VP containing the struct */

		/*
		 *	Rebuild the da_stack for the next option.
		 */
		vp = fr_cursor_current(cursor);
		fr_proto_da_stack_build(da_stack, vp ? vp->da : NULL);
		return fr_dbuff_set(dbuff, &work_dbuff);
	}

	/*
	 *	If there's a struct on the da_stack, then the struct
	 *	members MUST be in a flat list.
	 */
	if (da->type == FR_TYPE_STRUCT) {
		slen = fr_struct_to_network(&work_dbuff, da_stack, depth, cursor, encoder_ctx, encode_value, encode_tlv);
		if (slen <= 0) return slen;

		vp = fr_cursor_current(cursor);
		fr_proto_da_stack_build(da_stack, vp ? vp->da : NULL);
		return fr_dbuff_set(dbuff, &work_dbuff);
	}

	/*
	 *	If it's not a TLV, it should be a value type RFC
	 *	attribute make sure that it is.
	 */
	if (da_stack->da[depth + 1] != NULL) {
		fr_strerror_printf("%s: Encoding value but not at top of stack", __FUNCTION__);
		return PAIR_ENCODE_FATAL_ERROR;
	}

	if (vp->da != da) {
		fr_strerror_printf("%s: Top of stack does not match vp->da", __FUNCTION__);
		return PAIR_ENCODE_FATAL_ERROR;
	}

	switch (da->type) {
	case FR_TYPE_TLV:
	case FR_TYPE_VENDOR:
	case FR_TYPE_VSA:
		fr_strerror_printf("%s: Called with structural type %s", __FUNCTION__,
				   fr_table_str_by_value(fr_value_box_type_table, da->type, "?Unknown?"));
		return PAIR_ENCODE_FATAL_ERROR;

	default:
		break;
	}


	switch (da->type) {
	/*
	 *    0                   1                   2                   3
	 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   |          option-code          |          option-len           |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   .                            String                             .
	 *   |                              ...                              |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	case FR_TYPE_STRING:
		/*
		 *	DNS labels get a special encoder.  DNS labels
		 *	MUST NOT be compressed in DHCP.
		 *
		 *	https://tools.ietf.org/html/rfc8415#section-10
		 */
		if (!da->flags.extra &&
		    ((da->flags.subtype == FLAG_ENCODE_DNS_LABEL) ||
		     (da->flags.subtype == FLAG_ENCODE_PARTIAL_DNS_LABEL))) {
			fr_dbuff_marker_t	p;

			fr_dbuff_marker(&p, &work_dbuff);
			slen = fr_dns_label_from_value_box_dbuff(&work_dbuff, false, &vp->data);
			if (slen < 0) return slen;

			/*
			 *	RFC 4704 says "FQDN", unless it's a
			 *	single label, in which case it's a
			 *	partial name, and we omit the trailing
			 *	zero.
			 */
			if ((da->flags.subtype == FLAG_ENCODE_PARTIAL_DNS_LABEL) &&
			    (*(fr_dbuff_current(&p) + fr_dbuff_current(&p)[0] + 1) == 0)) {
				fr_dbuff_set_to_start(&work_dbuff);
				fr_dbuff_advance(&work_dbuff, slen - 1);
			}
			break;
		}
		goto to_network;

	/*
	 * Common encoder might add scope byte, so we just copy the address portion
	 *
	 *    0                   1                   2                   3
	 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   |          option-code          |           option-len          |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   |                                                               |
	 *   |                         ipv6-address                          |
	 *   |                                                               |
	 *   |                                                               |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	case FR_TYPE_IPV6_ADDR:
		FR_DBUFF_IN_MEMCPY_RETURN(&work_dbuff, vp->vp_ipv6addr, sizeof(vp->vp_ipv6addr));
		break;

	/*
	 *	Common encoder doesn't add a reserved byte after prefix, but it also
	 *	doesn't do the variable length encoding required.
	 *
	 *      0                   1                   2                   3
	 *      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *     |          option-code          |         option-length         |
	 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *     |  prefix6len   |              ipv6-prefix                      |
	 *     +-+-+-+-+-+-+-+-+           (variable length)                   |
	 *     .                                                               .
	 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	case FR_TYPE_IPV6_PREFIX:
	{
		size_t	prefix_len;

		/*
		 *	Structs have fixed length value fields.
		 */
		if (da->parent->type == FR_TYPE_STRUCT) {
			prefix_len = sizeof(vp->vp_ipv6addr);
		} else {
			prefix_len = fr_bytes_from_bits(vp->vp_ip.prefix);
		}

		FR_DBUFF_IN_BYTES_RETURN(&work_dbuff, vp->vp_ip.prefix);
		FR_DBUFF_IN_MEMCPY_RETURN(&work_dbuff, (uint8_t const *)&vp->vp_ipv6addr, prefix_len); /* Only copy the minimum address bytes required */
	}
		break;

	/*
	 *	Not actually specified by the DHCPv6 RFC, but will probably come
	 *	in handy at some point if we need to have the DHCPv6 server
	 *	hand out v4 prefixes.
	 */
	case FR_TYPE_IPV4_PREFIX:
	{
		size_t prefix_len;

		/*
		 *	Structs have fixed length value fields.
		 */
		if (da->parent->type == FR_TYPE_STRUCT) {
			prefix_len = sizeof(vp->vp_ipv4addr);
		} else {
			prefix_len = fr_bytes_from_bits(vp->vp_ip.prefix);
		}

		FR_DBUFF_IN_BYTES_RETURN(&work_dbuff, vp->vp_ip.prefix);
		FR_DBUFF_IN_MEMCPY_RETURN(&work_dbuff, (uint8_t const *)&vp->vp_ipv4addr, prefix_len);	/* Only copy the minimum address bytes required */
	}
		break;

	/*
	 *    0                   1                   2                   3
	 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   |          option-code          |           option-len          |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	case FR_TYPE_BOOL:
		/*
		 *	Don't encode anything!  The mere existence of
		 *	the attribute signifies a "true" value.
		 */
		break;

	/*
	 *	A standard 32bit integer, but unlike normal UNIX timestamps
	 *	starts from the 1st of January 2000.
	 *
	 *	In the decoder we add 30 years to any values, so here
	 *	we need to subtract that time, or if the value is less
	 *	than that time, just encode a 0x0000000000
	 *	value.
	 */
	case FR_TYPE_DATE:
	{
		uint64_t date = fr_unix_time_to_sec(vp->vp_date);

		if (date < DHCPV6_DATE_OFFSET) {	/* 30 years */
			FR_DBUFF_IN_RETURN(&work_dbuff, (uint32_t) 0);
			break;
		}

		FR_DBUFF_IN_RETURN(&work_dbuff, (uint32_t)(date - DHCPV6_DATE_OFFSET));
	}
		break;

	case FR_TYPE_GROUP:
	{
		fr_pair_list_t child;
		fr_cursor_t child_cursor;

		/*
		 *	Encode the child options.
		 */
		child = vp->vp_group;
		if (child) {
			(void) fr_cursor_init(&child_cursor, &child);

			while (fr_cursor_current(&child_cursor) != NULL) {
				child = fr_cursor_current(&child_cursor);
				slen = fr_dhcpv6_encode_option(&work_dbuff, &child_cursor, encoder_ctx);
				if (slen == PAIR_ENCODE_SKIPPED) continue;

				if (slen < 0) return PAIR_ENCODE_FATAL_ERROR;
				if (slen == 0) break;
			}
		}
	}
		break;

	/*
	 *	The value_box functions will take care of fixed-width
	 *	"string" and "octets" options.
	 */
	to_network:
	case FR_TYPE_OCTETS:
		/*
		 *	Hack until we find all places that don't set data.enumv
		 */
		if (vp->da->flags.length && (vp->data.enumv != vp->da)) {
			fr_dict_attr_t const * const *c = &vp->data.enumv;
			fr_dict_attr_t **u;

			memcpy(&u, &c, sizeof(c)); /* const issues */
			memcpy(u, &vp->da, sizeof(vp->da));			
		}
		FALL_THROUGH;

	/*
	 *    0                   1                   2                   3
	 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   |          option-code          |          option-len           |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   | 8-bit-integer |
	 *   +-+-+-+-+-+-+-+-+
	 */
	case FR_TYPE_UINT8:

	/*
	 *    0                   1                   2                   3
	 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   |          option-code          |           option-len          |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   |         16-bit-integer        |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	case FR_TYPE_UINT16:
	/*
	 *    0                   1                   2                   3
	 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *    |          option-code          |           option-len          |
	 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *    |                         32-bit-integer                        |
	 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	case FR_TYPE_UINT32:
	default:
		slen = fr_value_box_to_network(&work_dbuff, &vp->data);
		if (slen < 0) return PAIR_ENCODE_FATAL_ERROR;
		break;
	}

	/*
	 *	Rebuilds the TLV stack for encoding the next attribute
	 */
	vp = fr_cursor_next(cursor);
	fr_proto_da_stack_build(da_stack, vp ? vp->da : NULL);

	return fr_dbuff_set(dbuff, &work_dbuff);
}

static inline ssize_t encode_array(fr_dbuff_t *dbuff,
				   fr_da_stack_t *da_stack, int depth,
				   fr_cursor_t *cursor, void *encoder_ctx)
{
	ssize_t			slen;
	size_t			element_len;
	fr_dbuff_t		work_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_pair_t		*vp;
	fr_dict_attr_t const	*da = da_stack->da[depth];

	if (!fr_cond_assert_msg(da->flags.array,
				"%s: Internal sanity check failed, attribute \"%s\" does not have array bit set",
				__FUNCTION__, da->name)) return PAIR_ENCODE_FATAL_ERROR;

	/*
	 *	DNS labels have internalized length, so we don't need
	 *	length headers.
	 */
	if ((da->type == FR_TYPE_STRING) && !da->flags.extra && da->flags.subtype){
		while (fr_dbuff_extend(&work_dbuff)) {
			vp = fr_cursor_current(cursor);

			/*
			 *	DNS labels get a special encoder.  DNS labels
			 *	MUST NOT be compressed in DHCP.
			 *
			 *	https://tools.ietf.org/html/rfc8415#section-10
			 */
			slen = fr_dns_label_from_value_box_dbuff(&work_dbuff, false, &vp->data);
			if (slen <= 0) return PAIR_ENCODE_FATAL_ERROR;

			vp = fr_cursor_next(cursor);
			if (!vp || (vp->da != da)) break;		/* Stop if we have an attribute of a different type */
		}

		return fr_dbuff_set(dbuff, &work_dbuff);
	}

	while (fr_dbuff_extend(&work_dbuff)) {
		bool		len_field = false;
		fr_dbuff_t	element_dbuff = FR_DBUFF_NO_ADVANCE(&work_dbuff);

		element_len = fr_dhcpv6_option_len(fr_cursor_current(cursor));

		/*
		 *	If the data is variable length i.e. strings or octets
		 *	we need to include a length field before each element.
		 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-...-+-+-+-+-+-+-+
		 *   |       text-len                |        String                 |
		 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-...-+-+-+-+-+-+-+
		 */
		if (!da->flags.length) {
			len_field = true;
			FR_DBUFF_ADVANCE_RETURN(&element_dbuff, sizeof(uint16_t));	/* Make room for the length field */
		}

		slen = encode_value(&element_dbuff, da_stack, depth, cursor, encoder_ctx);
		if (slen < 0) return slen;
		if (!fr_cond_assert(slen < UINT16_MAX)) return PAIR_ENCODE_FATAL_ERROR;

		/*
		 *	Ensure we always create elements of the correct length.
		 *	This is mainly for fixed length octets type attributes
		 *	containing one or more keys.
		 */
		if (da->flags.length) {
			if ((size_t)slen < element_len) {
				FR_DBUFF_MEMSET_RETURN(&element_dbuff, 0, element_len - slen);
				slen = element_len;
			} else if ((size_t)slen > element_len){
				slen = element_len;
			}
		}

		/*
		 *	Populate the length field
		 */
		if (len_field) fr_dbuff_in(&work_dbuff, (uint16_t) slen);
		fr_dbuff_set(&work_dbuff, &element_dbuff);


		vp = fr_cursor_current(cursor);
		if (!vp || (vp->da != da)) break;		/* Stop if we have an attribute of a different type */
	}

	return fr_dbuff_set(dbuff, &work_dbuff);
}

static ssize_t encode_tlv(fr_dbuff_t *dbuff,
			  fr_da_stack_t *da_stack, unsigned int depth,
			  fr_cursor_t *cursor, void *encoder_ctx)
{
	fr_dbuff_t		work_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_pair_t const	*vp = fr_cursor_current(cursor);
	fr_dict_attr_t const	*da = da_stack->da[depth];
	ssize_t			len;
	fr_dbuff_extend_status_t	status = FR_DBUFF_EXTENDABLE;

	while (fr_dbuff_extend_lowat(&status, &work_dbuff, DHCPV6_OPT_HDR_LEN) > DHCPV6_OPT_HDR_LEN) {
		FR_PROTO_STACK_PRINT(da_stack, depth);

		/*
		 *	Determine the nested type and call the appropriate encoder
		 */
		if (da_stack->da[depth + 1]->type == FR_TYPE_TLV) {
			len = encode_tlv_hdr(&work_dbuff, da_stack, depth + 1, cursor, encoder_ctx);
		} else {
			len = encode_rfc_hdr(&work_dbuff, da_stack, depth + 1, cursor, encoder_ctx);
		}
		if (len < 0) return len;

		/*
		 *	If nothing updated the attribute, stop
		 */
		if (!fr_cursor_current(cursor) || (vp == fr_cursor_current(cursor))) break;

		/*
		 *	We can encode multiple sub TLVs, if after
		 *	rebuilding the TLV Stack, the attribute
		 *	at this depth is the same.
		 */
		if ((da != da_stack->da[depth]) || (da_stack->depth < da->depth)) break;
		vp = fr_cursor_current(cursor);
	}

	FR_PROTO_HEX_DUMP(fr_dbuff_start(&work_dbuff), fr_dbuff_used(&work_dbuff), "Done TLV body");

	return fr_dbuff_set(dbuff, &work_dbuff);
}

/** Encode an RFC format TLV.
 *
 * This could be a standard attribute, or a TLV data type.
 * If it's a standard attribute, then vp->da->attr == attribute.
 * Otherwise, attribute may be something else.
 */
static ssize_t encode_rfc_hdr(fr_dbuff_t *dbuff,
			      fr_da_stack_t *da_stack, unsigned int depth,
			      fr_cursor_t *cursor, void *encoder_ctx)
{
	fr_dbuff_t		work_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_dbuff_t		hdr_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_dict_attr_t const	*da = da_stack->da[depth];
	ssize_t			len;

	FR_PROTO_STACK_PRINT(da_stack, depth);

	/*
	 *	Make space for the header...
	 */
	FR_DBUFF_ADVANCE_RETURN(&work_dbuff, DHCPV6_OPT_HDR_LEN);

	/*
	 *	Write out the option's value
	 */
	if (da->flags.array) {
		len = encode_array(&work_dbuff, da_stack, depth, cursor, encoder_ctx);
	} else {
		len = encode_value(&work_dbuff, da_stack, depth, cursor, encoder_ctx);
	}
	if (len < 0) return len;

	/*
	 *	Write out the option number and length (before the value we just wrote)
	 */
	encode_option_hdr(&hdr_dbuff, (uint16_t)da->attr, (uint16_t) (fr_dbuff_used(&work_dbuff) - DHCPV6_OPT_HDR_LEN));

	FR_PROTO_HEX_DUMP(fr_dbuff_start(&work_dbuff), fr_dbuff_used(&work_dbuff), "Done RFC header");

	return fr_dbuff_set(dbuff, &work_dbuff);
}

static ssize_t encode_tlv_hdr(fr_dbuff_t *dbuff,
			      fr_da_stack_t *da_stack, unsigned int depth,
			      fr_cursor_t *cursor, void *encoder_ctx)
{
	fr_dbuff_t		work_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_dbuff_t		hdr_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_dict_attr_t const	*da = da_stack->da[depth];
	ssize_t			len;

	VP_VERIFY(fr_cursor_current(cursor));
	FR_PROTO_STACK_PRINT(da_stack, depth);

	if (da_stack->da[depth]->type != FR_TYPE_TLV) {
		fr_strerror_printf("%s: Expected type \"tlv\" got \"%s\"", __FUNCTION__,
				   fr_table_str_by_value(fr_value_box_type_table, da_stack->da[depth]->type, "?Unknown?"));
		return PAIR_ENCODE_FATAL_ERROR;
	}

	if (!da_stack->da[depth + 1]) {
		fr_strerror_printf("%s: Can't encode empty TLV", __FUNCTION__);
		return PAIR_ENCODE_FATAL_ERROR;
	}

	FR_DBUFF_ADVANCE_RETURN(&work_dbuff, DHCPV6_OPT_HDR_LEN);	/* Make room for option header */

	len = encode_tlv(&work_dbuff, da_stack, depth, cursor, encoder_ctx);
	if (len < 0) return len;

	/*
	 *    0                   1                   2                   3
	 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *   |          option-code          |           option-len          |
	 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */
	encode_option_hdr(&hdr_dbuff, (uint16_t)da->attr, (uint16_t) (fr_dbuff_used(&work_dbuff) - DHCPV6_OPT_HDR_LEN));

	FR_PROTO_HEX_DUMP(fr_dbuff_start(&work_dbuff), fr_dbuff_used(&work_dbuff), "Done TLV header");

	return fr_dbuff_set(dbuff, &work_dbuff);
}

/** Encode a Vendor-Specific Information Option
 *
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |      OPTION_VENDOR_OPTS       |           option-len          |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     |                       enterprise-number                       |
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *     .                                                               .
 *     .                          option-data                          .
 *     .                                                               .
 *     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
static ssize_t encode_vsio_hdr(fr_dbuff_t *dbuff,
			       fr_da_stack_t *da_stack, unsigned int depth,
			       fr_cursor_t *cursor, void *encoder_ctx)
{
	fr_dbuff_t		work_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_dbuff_t		hdr_dbuff = FR_DBUFF_NO_ADVANCE(dbuff);
	fr_dict_attr_t const	*da = da_stack->da[depth];
	fr_dict_attr_t const	*dv;
	ssize_t			len;

	FR_PROTO_STACK_PRINT(da_stack, depth);

	/*
	 *	DA should be a VSA type with the value of OPTION_VENDOR_OPTS.
	 */
	if (da->type != FR_TYPE_VSA) {
		fr_strerror_printf("%s: Expected type \"vsa\" got \"%s\"", __FUNCTION__,
				   fr_table_str_by_value(fr_value_box_type_table, da->type, "?Unknown?"));
		return PAIR_ENCODE_FATAL_ERROR;
	}

	/*
	 *	Now process the vendor ID part (which is one attribute deeper)
	 */
	dv = da_stack->da[++depth];
	FR_PROTO_STACK_PRINT(da_stack, depth);

	if (dv->type != FR_TYPE_VENDOR) {
		fr_strerror_printf("%s: Expected type \"vsa\" got \"%s\"", __FUNCTION__,
				   fr_table_str_by_value(fr_value_box_type_table, dv->type, "?Unknown?"));
		return PAIR_ENCODE_FATAL_ERROR;
	}

	FR_DBUFF_ADVANCE_RETURN(&work_dbuff, DHCPV6_OPT_HDR_LEN);
	FR_DBUFF_IN_RETURN(&work_dbuff, dv->attr);

	/*
	 *	https://tools.ietf.org/html/rfc8415#section-21.17 says:
	 *
	 *   The vendor-option-data field MUST be encoded as a sequence of
	 *   code/length/value fields of format identical to the DHCP options (see
	 *   Section 21.1).  The sub-option codes are defined by the vendor
	 *   identified in the enterprise-number field and are not managed by
	 *   IANA.  Each of the sub-options is formatted as follows:
	 *
	 *       0                   1                   2                   3
	 *       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	 *      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *      |          sub-opt-code         |         sub-option-len        |
	 *      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 *      .                                                               .
	 *      .                        sub-option-data                        .
	 *      .                                                               .
	 *      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	 */

	/*
	 *	Encode the different data types
	 */
	if (da->type == FR_TYPE_TLV) {
		len = encode_tlv_hdr(&work_dbuff, da_stack, depth + 1, cursor, encoder_ctx);
	} else {
		/*
		 *	Normal vendor option
		 */
		len = encode_rfc_hdr(&work_dbuff, da_stack, depth + 1, cursor, encoder_ctx);
	}
	if (len < 0) return len;

	encode_option_hdr(&hdr_dbuff, da->attr, fr_dbuff_used(&work_dbuff) - DHCPV6_OPT_HDR_LEN);

	FR_PROTO_HEX_DUMP(fr_dbuff_start(&work_dbuff), fr_dbuff_used(&work_dbuff), "Done VSIO header");

	return fr_dbuff_set(dbuff, &work_dbuff);
}

/** Encode a Relay-Message
 *
 *	Header + stuff
 */
static ssize_t encode_relay_message(fr_dbuff_t *dbuff,
				    fr_da_stack_t *da_stack, unsigned int depth,
				    fr_cursor_t *cursor, UNUSED void *encoder_ctx)
{
	fr_dbuff_marker_t	start_m;
	fr_dbuff_marker_t	len_m;
	ssize_t			slen;

	fr_dict_attr_t const	*da = da_stack->da[depth];
	fr_pair_t		*vp;

	FR_PROTO_STACK_PRINT(da_stack, depth);

	/*
	 *	Skip empty relay messages...
	 *	This shouldn't really happen.
	 */
	vp = fr_cursor_current(cursor);
	if (!vp->vp_group) {
		vp = fr_cursor_next(cursor);
		fr_proto_da_stack_build(da_stack, vp ? vp->da : NULL);
		return PAIR_ENCODE_SKIPPED;
	}

	/*
	 *	Write out the header
	 */
	fr_dbuff_marker(&start_m, dbuff);		/* Mark where we started */
	FR_DBUFF_IN_RETURN(dbuff, (uint16_t)da->attr);	/* Write out the option header */
	fr_dbuff_marker(&len_m, dbuff);			/* Mark where we'll need to put the length field */
	FR_DBUFF_ADVANCE_RETURN(dbuff, 2);		/* Advanced past the length field */

	vp = fr_cursor_current(cursor);
	slen = fr_dhcpv6_encode(dbuff, NULL, 0, 0, vp->vp_group);
	if (slen <= 0) {
		fr_dbuff_marker_release(&start_m);
		return slen;
	}

	fr_dbuff_in(&len_m, (uint16_t)slen);		/* Write out the length value */

	FR_PROTO_HEX_DUMP(fr_dbuff_start(dbuff), fr_dbuff_behind(&start_m), "Done Relay-Message header");

	vp = fr_cursor_next(cursor);
	fr_proto_da_stack_build(da_stack, vp ? vp->da : NULL);

	return fr_dbuff_marker_release_behind(&start_m);
}

/** Encode a DHCPv6 option and any sub-options.
 *
 * @param[out] dbuff		Where to write encoded DHCP attributes.
 * @param[in] cursor		with current VP set to the option to be encoded.
 *				Will be advanced to the next option to encode.
 * @param[in] encoder_ctx	containing parameters for the encoder.
 * @return
 *	- > 0 length of data written.
 *	- < 0 error.
 */
ssize_t fr_dhcpv6_encode_option(fr_dbuff_t *dbuff, fr_cursor_t *cursor, void * encoder_ctx)
{
	fr_pair_t		*vp;
	unsigned int		depth = 0;
	fr_da_stack_t		da_stack;
	fr_dbuff_t		work_dbuff = FR_DBUFF_MAX_NO_ADVANCE(dbuff, DHCPV6_OPT_HDR_LEN + UINT16_MAX);
	ssize_t			len;

	vp = fr_cursor_current(cursor);
	if (!vp) return 0;

	FR_PROTO_TRACE("encoding option %s", vp->da->name);

	if (vp->da->flags.internal) {
		fr_strerror_printf("Attribute \"%s\" is not a DHCPv6 option", vp->da->name);
		fr_cursor_next(cursor);
		return PAIR_ENCODE_SKIPPED;
	}

	fr_proto_da_stack_build(&da_stack, vp->da);

	FR_PROTO_STACK_PRINT(&da_stack, depth);

	/*
	 *	Deal with nested options
	 */
	switch (da_stack.da[depth]->type) {
	case FR_TYPE_TLV:
		len = encode_tlv_hdr(&work_dbuff, &da_stack, depth, cursor, encoder_ctx);
		break;

	case FR_TYPE_VSA:
		len = encode_vsio_hdr(&work_dbuff, &da_stack, depth, cursor, encoder_ctx);
		break;

	case FR_TYPE_GROUP:
		if (da_stack.da[depth] == attr_relay_message) {
			len = encode_relay_message(&work_dbuff, &da_stack, depth, cursor, encoder_ctx);
			break;
		}
		FALL_THROUGH;

	default:
		len = encode_rfc_hdr(&work_dbuff, &da_stack, depth, cursor, encoder_ctx);
		break;
	}
	if (len < 0) return len;

	FR_PROTO_TRACE("Complete option is %zu byte(s)", fr_dbuff_used(&work_dbuff));
	FR_PROTO_HEX_DUMP(fr_dbuff_start(&work_dbuff), fr_dbuff_used(&work_dbuff), NULL);

	return fr_dbuff_set(dbuff, &work_dbuff);
}

static int _test_ctx_free(UNUSED fr_dhcpv6_encode_ctx_t *ctx)
{
	fr_dhcpv6_global_free();

	return 0;
}

static int encode_test_ctx(void **out, TALLOC_CTX *ctx)
{
	fr_dhcpv6_encode_ctx_t	*test_ctx;

	if (fr_dhcpv6_global_init() < 0) return -1;

	test_ctx = talloc_zero(ctx, fr_dhcpv6_encode_ctx_t);
	if (!test_ctx) return -1;

	test_ctx->root = fr_dict_root(dict_dhcpv6);
	talloc_set_destructor(test_ctx, _test_ctx_free);

	*out = test_ctx;

	return 0;
}

static ssize_t fr_dhcpv6_encode_proto(UNUSED TALLOC_CTX *ctx, fr_pair_t *vps, uint8_t *data, size_t data_len, UNUSED void *proto_ctx)
{
	ssize_t slen;

	slen = fr_dhcpv6_encode(&FR_DBUFF_TMP(data, data_len), NULL, 0, 0, vps);

#ifndef NDEBUG
	if (slen <= 0) return slen;

	if (fr_debug_lvl > 2) {
		fr_dhcpv6_print_hex(stdout, data, slen);
	}
#endif

	return slen;
}

/*
 *	Test points
 */
extern fr_test_point_pair_encode_t dhcpv6_tp_encode_pair;
fr_test_point_pair_encode_t dhcpv6_tp_encode_pair = {
	.test_ctx	= encode_test_ctx,
	.func		= fr_dhcpv6_encode_option,
	.next_encodable	= fr_dhcpv6_next_encodable,
};

extern fr_test_point_proto_encode_t dhcpv6_tp_encode_proto;
fr_test_point_proto_encode_t dhcpv6_tp_encode_proto = {
	.test_ctx	= encode_test_ctx,
	.func		= fr_dhcpv6_encode_proto
};
