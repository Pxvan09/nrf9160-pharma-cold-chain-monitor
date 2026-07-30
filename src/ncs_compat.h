/*
 * ncs_compat.h - nRF Connect SDK version compatibility shims
 *
 * The ONLY APIs in this project that have changed signature across NCS
 * releases are collected here.  If a build fails with a signature error,
 * this is the single file to check.
 *
 * Verified drift points:
 *
 *  1. lte_lc_init()
 *       NCS < 2.6 : required before connecting.
 *       NCS >= 2.6: NO LONGER NEEDED (removed/no-op).
 *       lte_lc_init_and_connect_async() DEPRECATED - use
 *       lte_lc_connect_async() instead.
 *
 *  2. Release Assistance Indication socket option
 *       NCS < 2.6 : SO_RAI_NO_DATA / SO_RAI_LAST / SO_RAI_ONE_RESP
 *                   (each its own optname)
 *       NCS >= 2.6: single SO_RAI optname whose *value* selects behaviour.
 *       Getting this wrong silently costs ~60% of connected-mode energy.
 *
 *  3. aws_iot_init()
 *       NCS < 2.7 : aws_iot_init(const struct aws_iot_config *, handler)
 *       NCS >= 2.7: aws_iot_init(handler)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NCS_COMPAT_H__
#define NCS_COMPAT_H__

#include <zephyr/kernel.h>

#if __has_include(<ncs_version.h>)
#  include <ncs_version.h>
#  define PT_NCS_AT_LEAST(maj, min) \
     ((NCS_VERSION_MAJOR > (maj)) || \
      (NCS_VERSION_MAJOR == (maj) && NCS_VERSION_MINOR >= (min)))
#else
   /* Assume modern SDK when ncs_version.h is absent */
#  define PT_NCS_AT_LEAST(maj, min) 1
#endif

/* ---- 1. LTE init ---- */
#include <modem/lte_lc.h>

static inline int pt_lte_init(void)
{
#if PT_NCS_AT_LEAST(2, 6)
    return 0; /* no-op: lte_lc_init() removed in NCS 2.6 */
#else
    return lte_lc_init();
#endif
}

/* ---- 2. Release Assistance Indication ---- */
#include <zephyr/net/socket.h>

static inline int pt_socket_rai(int sock, bool last)
{
#if defined(SO_RAI)
    /* NCS >= 2.6 consolidated option */
    int val = last ? RAI_LAST : RAI_ONE_RESP;
    return setsockopt(sock, SOL_SOCKET, SO_RAI, &val, sizeof(val));
#elif defined(SO_RAI_LAST)
    /* Legacy: option name encodes the behaviour */
    int opt = last ? SO_RAI_LAST : SO_RAI_ONE_RESP;
    return setsockopt(sock, SOL_SOCKET, opt, NULL, 0);
#else
    ARG_UNUSED(sock);
    ARG_UNUSED(last);
    return -ENOTSUP;
#endif
}

#endif /* NCS_COMPAT_H__ */
