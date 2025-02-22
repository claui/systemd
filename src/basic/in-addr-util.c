/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "alloc-util.h"
#include "errno-util.h"
#include "in-addr-util.h"
#include "macro.h"
#include "parse-util.h"
#include "random-util.h"
#include "string-util.h"
#include "strxcpyx.h"
#include "util.h"

bool in4_addr_is_null(const struct in_addr *a) {
        assert(a);

        return a->s_addr == 0;
}

bool in6_addr_is_null(const struct in6_addr *a) {
        assert(a);

        return IN6_IS_ADDR_UNSPECIFIED(a);
}

int in_addr_is_null(int family, const union in_addr_union *u) {
        assert(u);

        if (family == AF_INET)
                return in4_addr_is_null(&u->in);

        if (family == AF_INET6)
                return in6_addr_is_null(&u->in6);

        return -EAFNOSUPPORT;
}

bool in4_addr_is_link_local(const struct in_addr *a) {
        assert(a);

        return (be32toh(a->s_addr) & UINT32_C(0xFFFF0000)) == (UINT32_C(169) << 24 | UINT32_C(254) << 16);
}

bool in6_addr_is_link_local(const struct in6_addr *a) {
        assert(a);

        return IN6_IS_ADDR_LINKLOCAL(a); /* lgtm [cpp/potentially-dangerous-function] */
}

int in_addr_is_link_local(int family, const union in_addr_union *u) {
        assert(u);

        if (family == AF_INET)
                return in4_addr_is_link_local(&u->in);

        if (family == AF_INET6)
                return in6_addr_is_link_local(&u->in6);

        return -EAFNOSUPPORT;
}

bool in6_addr_is_link_local_all_nodes(const struct in6_addr *a) {
        assert(a);

        /* ff02::1 */
        return be32toh(a->s6_addr32[0]) == UINT32_C(0xff020000) &&
                a->s6_addr32[1] == 0 &&
                a->s6_addr32[2] == 0 &&
                be32toh(a->s6_addr32[3]) == UINT32_C(0x00000001);
}

int in_addr_is_multicast(int family, const union in_addr_union *u) {
        assert(u);

        if (family == AF_INET)
                return IN_MULTICAST(be32toh(u->in.s_addr));

        if (family == AF_INET6)
                return IN6_IS_ADDR_MULTICAST(&u->in6);

        return -EAFNOSUPPORT;
}

bool in4_addr_is_local_multicast(const struct in_addr *a) {
        assert(a);

        return (be32toh(a->s_addr) & UINT32_C(0xffffff00)) == UINT32_C(0xe0000000);
}

bool in4_addr_is_localhost(const struct in_addr *a) {
        assert(a);

        /* All of 127.x.x.x is localhost. */
        return (be32toh(a->s_addr) & UINT32_C(0xFF000000)) == UINT32_C(127) << 24;
}

bool in4_addr_is_non_local(const struct in_addr *a) {
        /* Whether the address is not null and not localhost.
         *
         * As such, it is suitable to configure as DNS/NTP server from DHCP. */
        return !in4_addr_is_null(a) &&
               !in4_addr_is_localhost(a);
}

int in_addr_is_localhost(int family, const union in_addr_union *u) {
        assert(u);

        if (family == AF_INET)
                return in4_addr_is_localhost(&u->in);

        if (family == AF_INET6)
                return IN6_IS_ADDR_LOOPBACK(&u->in6); /* lgtm [cpp/potentially-dangerous-function] */

        return -EAFNOSUPPORT;
}

bool in6_addr_is_ipv4_mapped_address(const struct in6_addr *a) {
        return a->s6_addr32[0] == 0 &&
                a->s6_addr32[1] == 0 &&
                a->s6_addr32[2] == htobe32(UINT32_C(0x0000ffff));
}

bool in4_addr_equal(const struct in_addr *a, const struct in_addr *b) {
        assert(a);
        assert(b);

        return a->s_addr == b->s_addr;
}

bool in6_addr_equal(const struct in6_addr *a, const struct in6_addr *b) {
        assert(a);
        assert(b);

        return IN6_ARE_ADDR_EQUAL(a, b);
}

int in_addr_equal(int family, const union in_addr_union *a, const union in_addr_union *b) {
        assert(a);
        assert(b);

        if (family == AF_INET)
                return in4_addr_equal(&a->in, &b->in);

        if (family == AF_INET6)
                return in6_addr_equal(&a->in6, &b->in6);

        return -EAFNOSUPPORT;
}

int in_addr_prefix_intersect(
                int family,
                const union in_addr_union *a,
                unsigned aprefixlen,
                const union in_addr_union *b,
                unsigned bprefixlen) {

        unsigned m;

        assert(a);
        assert(b);

        /* Checks whether there are any addresses that are in both
         * networks */

        m = MIN(aprefixlen, bprefixlen);

        if (family == AF_INET) {
                uint32_t x, nm;

                x = be32toh(a->in.s_addr ^ b->in.s_addr);
                nm = (m == 0) ? 0 : 0xFFFFFFFFUL << (32 - m);

                return (x & nm) == 0;
        }

        if (family == AF_INET6) {
                unsigned i;

                if (m > 128)
                        m = 128;

                for (i = 0; i < 16; i++) {
                        uint8_t x, nm;

                        x = a->in6.s6_addr[i] ^ b->in6.s6_addr[i];

                        if (m < 8)
                                nm = 0xFF << (8 - m);
                        else
                                nm = 0xFF;

                        if ((x & nm) != 0)
                                return 0;

                        if (m > 8)
                                m -= 8;
                        else
                                m = 0;
                }

                return 1;
        }

        return -EAFNOSUPPORT;
}

int in_addr_prefix_next(int family, union in_addr_union *u, unsigned prefixlen) {
        assert(u);

        /* Increases the network part of an address by one. Returns 0 if that succeeds, or -ERANGE if
         * this overflows. */

        return in_addr_prefix_nth(family, u, prefixlen, 1);
}

/*
 * Calculates the nth prefix of size prefixlen starting from the address denoted by u.
 *
 * On success 0 will be returned and the calculated prefix will be available in
 * u. In case the calculation cannot be performed (invalid prefix length,
 * overflows would occur) -ERANGE is returned. If the address family given isn't
 * supported -EAFNOSUPPORT will be returned.
 *
 * Examples:
 *   - in_addr_prefix_nth(AF_INET, 192.168.0.0, 24, 2), returns 0, writes 192.168.2.0 to u
 *   - in_addr_prefix_nth(AF_INET, 192.168.0.0, 24, 0), returns 0, no data written
 *   - in_addr_prefix_nth(AF_INET, 255.255.255.0, 24, 1), returns -ERANGE, no data written
 *   - in_addr_prefix_nth(AF_INET, 255.255.255.0, 0, 1), returns -ERANGE, no data written
 *   - in_addr_prefix_nth(AF_INET6, 2001:db8, 64, 0xff00) returns 0, writes 2001:0db8:0000:ff00:: to u
 */
int in_addr_prefix_nth(int family, union in_addr_union *u, unsigned prefixlen, uint64_t nth) {
        assert(u);

        if (prefixlen <= 0)
                return -ERANGE;

        if (family == AF_INET) {
                uint32_t c, n, t;

                if (prefixlen > 32)
                        return -ERANGE;

                c = be32toh(u->in.s_addr);

                t = nth << (32 - prefixlen);

                /* Check for wrap */
                if (c > UINT32_MAX - t)
                        return -ERANGE;

                n = c + t;

                n &= UINT32_C(0xFFFFFFFF) << (32 - prefixlen);
                u->in.s_addr = htobe32(n);
                return 0;
        }

        if (family == AF_INET6) {
                bool overflow = false;

                if (prefixlen > 128)
                        return -ERANGE;

                for (unsigned i = 16; i > 0; i--) {
                        unsigned t, j = i - 1, p = j * 8;

                        if (p >= prefixlen) {
                                u->in6.s6_addr[j] = 0;
                                continue;
                        }

                        if (prefixlen - p < 8) {
                                u->in6.s6_addr[j] &= 0xff << (8 - (prefixlen - p));
                                t = u->in6.s6_addr[j] + ((nth & 0xff) << (8 - (prefixlen - p)));
                                nth >>= prefixlen - p;
                        } else {
                                t = u->in6.s6_addr[j] + (nth & 0xff) + overflow;
                                nth >>= 8;
                        }

                        overflow = t > UINT8_MAX;
                        u->in6.s6_addr[j] = (uint8_t) (t & 0xff);
                }

                if (overflow || nth != 0)
                        return -ERANGE;

                return 0;
        }

        return -EAFNOSUPPORT;
}

int in_addr_random_prefix(
                int family,
                union in_addr_union *u,
                unsigned prefixlen_fixed_part,
                unsigned prefixlen) {

        assert(u);

        /* Random network part of an address by one. */

        if (prefixlen <= 0)
                return 0;

        if (family == AF_INET) {
                uint32_t c, n;

                if (prefixlen_fixed_part > 32)
                        prefixlen_fixed_part = 32;
                if (prefixlen > 32)
                        prefixlen = 32;
                if (prefixlen_fixed_part >= prefixlen)
                        return -EINVAL;

                c = be32toh(u->in.s_addr);
                c &= ((UINT32_C(1) << prefixlen_fixed_part) - 1) << (32 - prefixlen_fixed_part);

                random_bytes(&n, sizeof(n));
                n &= ((UINT32_C(1) << (prefixlen - prefixlen_fixed_part)) - 1) << (32 - prefixlen);

                u->in.s_addr = htobe32(n | c);
                return 1;
        }

        if (family == AF_INET6) {
                struct in6_addr n;
                unsigned i, j;

                if (prefixlen_fixed_part > 128)
                        prefixlen_fixed_part = 128;
                if (prefixlen > 128)
                        prefixlen = 128;
                if (prefixlen_fixed_part >= prefixlen)
                        return -EINVAL;

                random_bytes(&n, sizeof(n));

                for (i = 0; i < 16; i++) {
                        uint8_t mask_fixed_part = 0, mask = 0;

                        if (i < (prefixlen_fixed_part + 7) / 8) {
                                if (i < prefixlen_fixed_part / 8)
                                        mask_fixed_part = 0xffu;
                                else {
                                        j = prefixlen_fixed_part % 8;
                                        mask_fixed_part = ((UINT8_C(1) << (j + 1)) - 1) << (8 - j);
                                }
                        }

                        if (i < (prefixlen + 7) / 8) {
                                if (i < prefixlen / 8)
                                        mask = 0xffu ^ mask_fixed_part;
                                else {
                                        j = prefixlen % 8;
                                        mask = (((UINT8_C(1) << (j + 1)) - 1) << (8 - j)) ^ mask_fixed_part;
                                }
                        }

                        u->in6.s6_addr[i] &= mask_fixed_part;
                        u->in6.s6_addr[i] |= n.s6_addr[i] & mask;
                }

                return 1;
        }

        return -EAFNOSUPPORT;
}

int in_addr_prefix_range(
                int family,
                const union in_addr_union *in,
                unsigned prefixlen,
                union in_addr_union *ret_start,
                union in_addr_union *ret_end) {

        union in_addr_union start, end;
        int r;

        assert(in);

        if (!IN_SET(family, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        if (ret_start) {
                start = *in;
                r = in_addr_prefix_nth(family, &start, prefixlen, 0);
                if (r < 0)
                        return r;
        }

        if (ret_end) {
                end = *in;
                r = in_addr_prefix_nth(family, &end, prefixlen, 1);
                if (r < 0)
                        return r;
        }

        if (ret_start)
                *ret_start = start;
        if (ret_end)
                *ret_end = end;

        return 0;
}

int in_addr_to_string(int family, const union in_addr_union *u, char **ret) {
        _cleanup_free_ char *x = NULL;
        size_t l;

        assert(u);
        assert(ret);

        if (family == AF_INET)
                l = INET_ADDRSTRLEN;
        else if (family == AF_INET6)
                l = INET6_ADDRSTRLEN;
        else
                return -EAFNOSUPPORT;

        x = new(char, l);
        if (!x)
                return -ENOMEM;

        errno = 0;
        if (!inet_ntop(family, u, x, l))
                return errno_or_else(EINVAL);

        *ret = TAKE_PTR(x);
        return 0;
}

int in_addr_prefix_to_string(int family, const union in_addr_union *u, unsigned prefixlen, char **ret) {
        _cleanup_free_ char *x = NULL;
        char *p;
        size_t l;

        assert(u);
        assert(ret);

        if (family == AF_INET)
                l = INET_ADDRSTRLEN + 3;
        else if (family == AF_INET6)
                l = INET6_ADDRSTRLEN + 4;
        else
                return -EAFNOSUPPORT;

        if (prefixlen > FAMILY_ADDRESS_SIZE(family) * 8)
                return -EINVAL;

        x = new(char, l);
        if (!x)
                return -ENOMEM;

        errno = 0;
        if (!inet_ntop(family, u, x, l))
                return errno_or_else(EINVAL);

        p = x + strlen(x);
        l -= strlen(x);
        (void) strpcpyf(&p, l, "/%u", prefixlen);

        *ret = TAKE_PTR(x);
        return 0;
}

int in_addr_port_ifindex_name_to_string(int family, const union in_addr_union *u, uint16_t port, int ifindex, const char *server_name, char **ret) {
        _cleanup_free_ char *ip_str = NULL, *x = NULL;
        int r;

        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(u);
        assert(ret);

        /* Much like in_addr_to_string(), but optionally appends the zone interface index to the address, to properly
         * handle IPv6 link-local addresses. */

        r = in_addr_to_string(family, u, &ip_str);
        if (r < 0)
                return r;

        if (family == AF_INET6) {
                r = in_addr_is_link_local(family, u);
                if (r < 0)
                        return r;
                if (r == 0)
                        ifindex = 0;
        } else
                ifindex = 0; /* For IPv4 address, ifindex is always ignored. */

        if (port == 0 && ifindex == 0 && isempty(server_name)) {
                *ret = TAKE_PTR(ip_str);
                return 0;
        }

        const char *separator = isempty(server_name) ? "" : "#";
        server_name = strempty(server_name);

        if (port > 0) {
                if (family == AF_INET6) {
                        if (ifindex > 0)
                                r = asprintf(&x, "[%s]:%"PRIu16"%%%i%s%s", ip_str, port, ifindex, separator, server_name);
                        else
                                r = asprintf(&x, "[%s]:%"PRIu16"%s%s", ip_str, port, separator, server_name);
                } else
                        r = asprintf(&x, "%s:%"PRIu16"%s%s", ip_str, port, separator, server_name);
        } else {
                if (ifindex > 0)
                        r = asprintf(&x, "%s%%%i%s%s", ip_str, ifindex, separator, server_name);
                else {
                        x = strjoin(ip_str, separator, server_name);
                        r = x ? 0 : -ENOMEM;
                }
        }
        if (r < 0)
                return -ENOMEM;

        *ret = TAKE_PTR(x);
        return 0;
}

int in_addr_from_string(int family, const char *s, union in_addr_union *ret) {
        union in_addr_union buffer;
        assert(s);

        if (!IN_SET(family, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        errno = 0;
        if (inet_pton(family, s, ret ?: &buffer) <= 0)
                return errno_or_else(EINVAL);

        return 0;
}

int in_addr_from_string_auto(const char *s, int *ret_family, union in_addr_union *ret) {
        int r;

        assert(s);

        r = in_addr_from_string(AF_INET, s, ret);
        if (r >= 0) {
                if (ret_family)
                        *ret_family = AF_INET;
                return 0;
        }

        r = in_addr_from_string(AF_INET6, s, ret);
        if (r >= 0) {
                if (ret_family)
                        *ret_family = AF_INET6;
                return 0;
        }

        return -EINVAL;
}

unsigned char in4_addr_netmask_to_prefixlen(const struct in_addr *addr) {
        assert(addr);

        return 32U - u32ctz(be32toh(addr->s_addr));
}

struct in_addr* in4_addr_prefixlen_to_netmask(struct in_addr *addr, unsigned char prefixlen) {
        assert(addr);
        assert(prefixlen <= 32);

        /* Shifting beyond 32 is not defined, handle this specially. */
        if (prefixlen == 0)
                addr->s_addr = 0;
        else
                addr->s_addr = htobe32((0xffffffff << (32 - prefixlen)) & 0xffffffff);

        return addr;
}

int in4_addr_default_prefixlen(const struct in_addr *addr, unsigned char *prefixlen) {
        uint8_t msb_octet = *(uint8_t*) addr;

        /* addr may not be aligned, so make sure we only access it byte-wise */

        assert(addr);
        assert(prefixlen);

        if (msb_octet < 128)
                /* class A, leading bits: 0 */
                *prefixlen = 8;
        else if (msb_octet < 192)
                /* class B, leading bits 10 */
                *prefixlen = 16;
        else if (msb_octet < 224)
                /* class C, leading bits 110 */
                *prefixlen = 24;
        else
                /* class D or E, no default prefixlen */
                return -ERANGE;

        return 0;
}

int in4_addr_default_subnet_mask(const struct in_addr *addr, struct in_addr *mask) {
        unsigned char prefixlen;
        int r;

        assert(addr);
        assert(mask);

        r = in4_addr_default_prefixlen(addr, &prefixlen);
        if (r < 0)
                return r;

        in4_addr_prefixlen_to_netmask(mask, prefixlen);
        return 0;
}

int in_addr_mask(int family, union in_addr_union *addr, unsigned char prefixlen) {
        assert(addr);

        if (family == AF_INET) {
                struct in_addr mask;

                if (!in4_addr_prefixlen_to_netmask(&mask, prefixlen))
                        return -EINVAL;

                addr->in.s_addr &= mask.s_addr;
                return 0;
        }

        if (family == AF_INET6) {
                unsigned i;

                for (i = 0; i < 16; i++) {
                        uint8_t mask;

                        if (prefixlen >= 8) {
                                mask = 0xFF;
                                prefixlen -= 8;
                        } else {
                                mask = 0xFF << (8 - prefixlen);
                                prefixlen = 0;
                        }

                        addr->in6.s6_addr[i] &= mask;
                }

                return 0;
        }

        return -EAFNOSUPPORT;
}

int in_addr_prefix_covers(int family,
                          const union in_addr_union *prefix,
                          unsigned char prefixlen,
                          const union in_addr_union *address) {

        union in_addr_union masked_prefix, masked_address;
        int r;

        assert(prefix);
        assert(address);

        masked_prefix = *prefix;
        r = in_addr_mask(family, &masked_prefix, prefixlen);
        if (r < 0)
                return r;

        masked_address = *address;
        r = in_addr_mask(family, &masked_address, prefixlen);
        if (r < 0)
                return r;

        return in_addr_equal(family, &masked_prefix, &masked_address);
}

int in_addr_parse_prefixlen(int family, const char *p, unsigned char *ret) {
        uint8_t u;
        int r;

        if (!IN_SET(family, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        r = safe_atou8(p, &u);
        if (r < 0)
                return r;

        if (u > FAMILY_ADDRESS_SIZE(family) * 8)
                return -ERANGE;

        *ret = u;
        return 0;
}

int in_addr_prefix_from_string(
                const char *p,
                int family,
                union in_addr_union *ret_prefix,
                unsigned char *ret_prefixlen) {

        _cleanup_free_ char *str = NULL;
        union in_addr_union buffer;
        const char *e, *l;
        unsigned char k;
        int r;

        assert(p);

        if (!IN_SET(family, AF_INET, AF_INET6))
                return -EAFNOSUPPORT;

        e = strchr(p, '/');
        if (e) {
                str = strndup(p, e - p);
                if (!str)
                        return -ENOMEM;

                l = str;
        } else
                l = p;

        r = in_addr_from_string(family, l, &buffer);
        if (r < 0)
                return r;

        if (e) {
                r = in_addr_parse_prefixlen(family, e+1, &k);
                if (r < 0)
                        return r;
        } else
                k = FAMILY_ADDRESS_SIZE(family) * 8;

        if (ret_prefix)
                *ret_prefix = buffer;
        if (ret_prefixlen)
                *ret_prefixlen = k;

        return 0;
}

int in_addr_prefix_from_string_auto_internal(
                const char *p,
                InAddrPrefixLenMode mode,
                int *ret_family,
                union in_addr_union *ret_prefix,
                unsigned char *ret_prefixlen) {

        _cleanup_free_ char *str = NULL;
        union in_addr_union buffer;
        const char *e, *l;
        unsigned char k;
        int family, r;

        assert(p);

        e = strchr(p, '/');
        if (e) {
                str = strndup(p, e - p);
                if (!str)
                        return -ENOMEM;

                l = str;
        } else
                l = p;

        r = in_addr_from_string_auto(l, &family, &buffer);
        if (r < 0)
                return r;

        if (e) {
                r = in_addr_parse_prefixlen(family, e+1, &k);
                if (r < 0)
                        return r;
        } else
                switch (mode) {
                case PREFIXLEN_FULL:
                        k = FAMILY_ADDRESS_SIZE(family) * 8;
                        break;
                case PREFIXLEN_REFUSE:
                        return -ENOANO; /* To distinguish this error from others. */
                case PREFIXLEN_LEGACY:
                        if (family == AF_INET) {
                                r = in4_addr_default_prefixlen(&buffer.in, &k);
                                if (r < 0)
                                        return r;
                        } else
                                k = 0;
                        break;
                default:
                        assert_not_reached();
                }

        if (ret_family)
                *ret_family = family;
        if (ret_prefix)
                *ret_prefix = buffer;
        if (ret_prefixlen)
                *ret_prefixlen = k;

        return 0;

}

static void in_addr_data_hash_func(const struct in_addr_data *a, struct siphash *state) {
        assert(a);
        assert(state);

        siphash24_compress(&a->family, sizeof(a->family), state);
        siphash24_compress(&a->address, FAMILY_ADDRESS_SIZE(a->family), state);
}

static int in_addr_data_compare_func(const struct in_addr_data *x, const struct in_addr_data *y) {
        int r;

        assert(x);
        assert(y);

        r = CMP(x->family, y->family);
        if (r != 0)
                return r;

        return memcmp(&x->address, &y->address, FAMILY_ADDRESS_SIZE(x->family));
}

DEFINE_HASH_OPS(in_addr_data_hash_ops, struct in_addr_data, in_addr_data_hash_func, in_addr_data_compare_func);

static void in_addr_prefix_hash_func(const struct in_addr_prefix *a, struct siphash *state) {
        assert(a);
        assert(state);

        siphash24_compress(&a->family, sizeof(a->family), state);
        siphash24_compress(&a->prefixlen, sizeof(a->prefixlen), state);
        siphash24_compress(&a->address, FAMILY_ADDRESS_SIZE(a->family), state);
}

static int in_addr_prefix_compare_func(const struct in_addr_prefix *x, const struct in_addr_prefix *y) {
        int r;

        assert(x);
        assert(y);

        r = CMP(x->family, y->family);
        if (r != 0)
                return r;

        r = CMP(x->prefixlen, y->prefixlen);
        if (r != 0)
                return r;

        return memcmp(&x->address, &y->address, FAMILY_ADDRESS_SIZE(x->family));
}

DEFINE_HASH_OPS(in_addr_prefix_hash_ops, struct in_addr_prefix, in_addr_prefix_hash_func, in_addr_prefix_compare_func);
DEFINE_HASH_OPS_WITH_KEY_DESTRUCTOR(in_addr_prefix_hash_ops_free, struct in_addr_prefix, in_addr_prefix_hash_func, in_addr_prefix_compare_func, free);

void in6_addr_hash_func(const struct in6_addr *addr, struct siphash *state) {
        assert(addr);
        assert(state);

        siphash24_compress(addr, sizeof(*addr), state);
}

int in6_addr_compare_func(const struct in6_addr *a, const struct in6_addr *b) {
        assert(a);
        assert(b);

        return memcmp(a, b, sizeof(*a));
}

DEFINE_HASH_OPS(in6_addr_hash_ops, struct in6_addr, in6_addr_hash_func, in6_addr_compare_func);
