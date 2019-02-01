/*
 * Copyright 2018 The Fuchsia Authors.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef GARNET_DRIVERS_WLAN_THIRD_PARTY_ATHEROS_ATH10K_UTILS_H_
#define GARNET_DRIVERS_WLAN_THIRD_PARTY_ATHEROS_ATH10K_UTILS_H_

#include <stdbool.h>
#include <stdint.h>

// Converts an Ethernet MAC address |addr| to a string buffer |str|.
// Note that the |str| must be at least ETH_ALEN * 3 bytes (18 bytes).
// Typical usage:
//
//     char buf[ETH_ALEN * 3];
//     ethaddr_sprintf(buf, arvif->bssid);
//     ath10k_err("MAC address is [%a]s\n", buf);
//
void ethaddr_sprintf(char* str, const uint8_t* addr);

// Returns true if the given MAC address is all zero-es.
bool is_zero_ether_addr(const uint8_t* addr);

#endif  // GARNET_DRIVERS_WLAN_THIRD_PARTY_ATHEROS_ATH10K_UTILS_H_
