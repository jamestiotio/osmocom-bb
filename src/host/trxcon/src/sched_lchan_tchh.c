/*
 * OsmocomBB <-> SDR connection bridge
 * TDMA scheduler: handlers for DL / UL bursts on logical channels
 *
 * (C) 2018-2022 by Vadim Yanitskiy <axilirator@gmail.com>
 * (C) 2018 by Harald Welte <laforge@gnumonks.org>
 * Contributions by sysmocom - s.f.m.c. GmbH
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <osmocom/core/logging.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/bits.h>

#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/gsm/gsm_utils.h>

#include <osmocom/coding/gsm0503_coding.h>
#include <osmocom/codec/codec.h>

#include <osmocom/bb/l1sched/l1sched.h>
#include <osmocom/bb/l1sched/logging.h>

static const uint8_t tch_h0_traffic_block_map[3][4] = {
	/* B0(0,2,4,6), B1(4,6,8,10), B2(8,10,0,2) */
	{ 0, 2, 4, 6 },
	{ 4, 6, 8, 10 },
	{ 8, 10, 0, 2 },
};

static const uint8_t tch_h1_traffic_block_map[3][4] = {
	/* B0(1,3,5,7), B1(5,7,9,11), B2(9,11,1,3) */
	{ 1, 3, 5, 7 },
	{ 5, 7, 9, 11 },
	{ 9, 11, 1, 3 },
};

static const uint8_t tch_h0_dl_facch_block_map[3][6] = {
	/* B0(4,6,8,10,13,15), B1(13,15,17,19,21,23), B2(21,23,0,2,4,6) */
	{ 4, 6, 8, 10, 13, 15 },
	{ 13, 15, 17, 19, 21, 23 },
	{ 21, 23, 0, 2, 4, 6 },
};

static const uint8_t tch_h0_ul_facch_block_map[3][6] = {
	/* B0(0,2,4,6,8,10), B1(8,10,13,15,17,19), B2(17,19,21,23,0,2) */
	{ 0, 2, 4, 6, 8, 10 },
	{ 8, 10, 13, 15, 17, 19 },
	{ 17, 19, 21, 23, 0, 2 },
};

static const uint8_t tch_h1_dl_facch_block_map[3][6] = {
	/* B0(5,7,9,11,14,16), B1(14,16,18,20,22,24), B2(22,24,1,3,5,7) */
	{ 5, 7, 9, 11, 14, 16 },
	{ 14, 16, 18, 20, 22, 24 },
	{ 22, 24, 1, 3, 5, 7 },
};

const uint8_t tch_h1_ul_facch_block_map[3][6] = {
	/* B0(1,3,5,7,9,11), B1(9,11,14,16,18,20), B2(18,20,22,24,1,3) */
	{ 1, 3, 5, 7, 9, 11 },
	{ 9, 11, 14, 16, 18, 20 },
	{ 18, 20, 22, 24, 1, 3 },
};

/**
 * Can a TCH/H block transmission be initiated / finished
 * on a given frame number and a given channel type?
 *
 * See GSM 05.02, clause 7, table 1
 *
 * @param  chan   channel type (L1SCHED_TCHH_0 or L1SCHED_TCHH_1)
 * @param  fn     the current frame number
 * @param  ul     Uplink or Downlink?
 * @param  facch  FACCH/H or traffic?
 * @param  start  init or end of transmission?
 * @return        true (yes) or false (no)
 */
bool l1sched_tchh_block_map_fn(enum l1sched_lchan_type chan,
	uint32_t fn, bool ul, bool facch, bool start)
{
	uint8_t fn_mf;
	int i = 0;

	/* Just to be sure */
	OSMO_ASSERT(chan == L1SCHED_TCHH_0 || chan == L1SCHED_TCHH_1);

	/* Calculate a modulo */
	fn_mf = facch ? (fn % 26) : (fn % 13);

#define MAP_GET_POS(map) \
	(start ? 0 : ARRAY_SIZE(map[i]) - 1)

#define BLOCK_MAP_FN(map) \
	do { \
		if (map[i][MAP_GET_POS(map)] == fn_mf) \
			return true; \
	} while (++i < ARRAY_SIZE(map))

	/* Choose a proper block map */
	if (facch) {
		if (ul) {
			if (chan == L1SCHED_TCHH_0)
				BLOCK_MAP_FN(tch_h0_ul_facch_block_map);
			else
				BLOCK_MAP_FN(tch_h1_ul_facch_block_map);
		} else {
			if (chan == L1SCHED_TCHH_0)
				BLOCK_MAP_FN(tch_h0_dl_facch_block_map);
			else
				BLOCK_MAP_FN(tch_h1_dl_facch_block_map);
		}
	} else {
		if (chan == L1SCHED_TCHH_0)
			BLOCK_MAP_FN(tch_h0_traffic_block_map);
		else
			BLOCK_MAP_FN(tch_h1_traffic_block_map);
	}

	return false;
}

/**
 * Calculates a frame number of the first burst
 * using given frame number of the last burst.
 *
 * See GSM 05.02, clause 7, table 1
 *
 * @param  chan      channel type (L1SCHED_TCHH_0 or L1SCHED_TCHH_1)
 * @param  last_fn   frame number of the last burst
 * @param  facch     FACCH/H or traffic?
 * @return           either frame number of the first burst,
 *                   or fn=last_fn if calculation failed
 */
static uint32_t tchh_block_dl_first_fn(enum l1sched_lchan_type chan,
				       uint32_t last_fn, bool facch)
{
	uint8_t fn_mf, fn_diff;
	int i = 0;

	/* Just to be sure */
	OSMO_ASSERT(chan == L1SCHED_TCHH_0 || chan == L1SCHED_TCHH_1);

	/* Calculate a modulo */
	fn_mf = facch ? (last_fn % 26) : (last_fn % 13);

#define BLOCK_FIRST_FN(map) \
	do { \
		if (map[i][ARRAY_SIZE(map[i]) - 1] == fn_mf) { \
			fn_diff = GSM_TDMA_FN_DIFF(fn_mf, map[i][0]); \
			return GSM_TDMA_FN_SUB(last_fn, fn_diff); \
		} \
	} while (++i < ARRAY_SIZE(map))

	/* Choose a proper block map */
	if (facch) {
		if (chan == L1SCHED_TCHH_0)
			BLOCK_FIRST_FN(tch_h0_dl_facch_block_map);
		else
			BLOCK_FIRST_FN(tch_h1_dl_facch_block_map);
	} else {
		if (chan == L1SCHED_TCHH_0)
			BLOCK_FIRST_FN(tch_h0_traffic_block_map);
		else
			BLOCK_FIRST_FN(tch_h1_traffic_block_map);
	}

#if 0
	LOGP_LCHAND(lchan, LOGL_ERROR,
		    "Failed to calculate TDMA frame number of the first burst of %s block, "
		    "using the current fn=%u\n", facch ? "FACCH/H" : "TCH/H", last_fn);
#endif

	/* Couldn't calculate the first fn, return the last */
	return last_fn;
}

int rx_tchh_fn(struct l1sched_lchan_state *lchan,
	       uint32_t fn, uint8_t bid, const sbit_t *bits,
	       const struct l1sched_meas_set *meas)
{
	int n_errors = -1, n_bits_total, rc;
	sbit_t *buffer, *offset;
	uint8_t l2[128], *mask;
	size_t l2_len;

	/* Set up pointers */
	mask = &lchan->rx_burst_mask;
	buffer = lchan->rx_bursts;

	LOGP_LCHAND(lchan, LOGL_DEBUG, "Traffic received: fn=%u bid=%u\n", fn, bid);

	if (*mask == 0x00) {
		/* Align to the first burst */
		if (bid > 0)
			return 0;

		/* Align reception of the first FACCH/H frame */
		if (lchan->tch_mode == GSM48_CMODE_SIGN) {
			if (!l1sched_tchh_facch_start(lchan->type, fn, 0))
				return 0;
		} else { /* or TCH/H traffic frame */
			if (!l1sched_tchh_traffic_start(lchan->type, fn, 0))
				return 0;
		}
	}

	/* Update mask */
	*mask |= (1 << bid);

	/* Store the measurements */
	l1sched_lchan_meas_push(lchan, meas);

	/* Copy burst to the end of buffer of 6 bursts */
	offset = buffer + bid * 116 + 464;
	memcpy(offset, bits + 3, 58);
	memcpy(offset + 58, bits + 87, 58);

	/* Wait until the second burst */
	if (bid != 1)
		return 0;

	/* Wait for complete set of bursts */
	if (lchan->tch_mode == GSM48_CMODE_SIGN) {
		/* FACCH/H is interleaved over 6 bursts */
		if ((*mask & 0x3f) != 0x3f)
			goto bfi_shift;
	} else {
		/* Traffic is interleaved over 4 bursts */
		if ((*mask & 0x0f) != 0x0f)
			goto bfi_shift;
	}

	/* Skip decoding attempt in case of FACCH/H */
	if (lchan->dl_ongoing_facch) {
		lchan->dl_ongoing_facch = false;
		goto bfi_shift; /* 2/2 BFI */
	}

	switch (lchan->tch_mode) {
	case GSM48_CMODE_SIGN:
	case GSM48_CMODE_SPEECH_V1: /* HR */
		rc = gsm0503_tch_hr_decode(l2, buffer,
			!l1sched_tchh_facch_end(lchan->type, fn, 0),
			&n_errors, &n_bits_total);
		break;
	case GSM48_CMODE_SPEECH_AMR: /* AMR */
		/**
		 * TODO: AMR requires a dedicated loop,
		 * which will be implemented later...
		 */
		LOGP_LCHAND(lchan, LOGL_ERROR, "AMR isn't supported yet\n");
		return -ENOTSUP;
	default:
		LOGP_LCHAND(lchan, LOGL_ERROR, "Invalid TCH mode: %u\n", lchan->tch_mode);
		return -EINVAL;
	}

	/* Shift buffer by 4 bursts for interleaving */
	memcpy(buffer, buffer + 232, 232);
	memcpy(buffer + 232, buffer + 464, 232);

	/* Shift burst mask */
	*mask = *mask << 2;

	/* Check decoding result */
	if (rc < 4) {
		/* Calculate AVG of the measurements (assuming 4 bursts) */
		l1sched_lchan_meas_avg(lchan, 4);

		LOGP_LCHAND(lchan, LOGL_ERROR,
			    "Received bad frame (rc=%d, ber=%d/%d) at fn=%u\n",
			    rc, n_errors, n_bits_total, lchan->meas_avg.fn);

		/* Send BFI */
		goto bfi;
	} else if (rc == GSM_MACBLOCK_LEN) {
		/* Skip decoding of the next 2 stolen bursts */
		lchan->dl_ongoing_facch = true;

		/* Calculate AVG of the measurements (FACCH/H takes 6 bursts) */
		l1sched_lchan_meas_avg(lchan, 6);

		/* FACCH/H received, forward to the higher layers */
		l1sched_handle_data_ind(lchan, l2, GSM_MACBLOCK_LEN,
					n_errors, n_bits_total,
					L1SCHED_DT_SIGNALING);

		/* Send BFI substituting 1/2 stolen TCH frames */
		n_errors = -1; /* ensure fake measurements */
		goto bfi;
	} else {
		/* A good TCH frame received */
		l2_len = rc;

		/* Calculate AVG of the measurements (traffic takes 4 bursts) */
		l1sched_lchan_meas_avg(lchan, 4);
	}

	/* Send a traffic frame to the higher layers */
	return l1sched_handle_data_ind(lchan, l2, l2_len,
				       n_errors, n_bits_total,
				       L1SCHED_DT_TRAFFIC);

bfi_shift:
	/* Shift buffer */
	memcpy(buffer, buffer + 232, 232);
	memcpy(buffer + 232, buffer + 464, 232);

	/* Shift burst mask */
	*mask = *mask << 2;

bfi:
	/* Didn't try to decode, fake measurements */
	if (n_errors < 0) {
		lchan->meas_avg = (struct l1sched_meas_set) {
			.fn = tchh_block_dl_first_fn(lchan->type, fn, false),
			.toa256 = 0,
			.rssi = -110,
		};

		/* No bursts => no errors */
		n_errors = 0;
	}

	/* BFI is not applicable in signalling mode */
	if (lchan->tch_mode == GSM48_CMODE_SIGN) {
		return l1sched_handle_data_ind(lchan, NULL, 0,
					       n_errors, n_bits_total,
					       L1SCHED_DT_SIGNALING);
	}

	/* Bad frame indication */
	l2_len = l1sched_bad_frame_ind(l2, lchan);

	/* Send a BFI frame to the higher layers */
	return l1sched_handle_data_ind(lchan, l2, l2_len,
				       n_errors, n_bits_total,
				       L1SCHED_DT_TRAFFIC);
}

int tx_tchh_fn(struct l1sched_lchan_state *lchan,
	       struct l1sched_burst_req *br)
{
	ubit_t *buffer, *offset;
	const uint8_t *tsc;
	uint8_t *mask;
	size_t l2_len;
	int rc;

	/* Set up pointers */
	mask = &lchan->tx_burst_mask;
	buffer = lchan->tx_bursts;

	if (br->bid > 0) {
		/* Align to the first burst */
		if (*mask == 0x00)
			return 0;
		goto send_burst;
	}

	if (*mask == 0x00) {
		/* Align transmission of the first FACCH/H frame */
		if (lchan->tch_mode == GSM48_CMODE_SIGN)
			if (!l1sched_tchh_facch_start(lchan->type, br->fn, 1))
				return 0;
	}

	/* Shift buffer by 2 bursts back for interleaving */
	memcpy(buffer, buffer + 232, 232);

	/* Also shift TX burst mask */
	*mask = *mask << 2;

	/* If FACCH/H blocks are still pending */
	if (lchan->ul_facch_blocks > 2) {
		memcpy(buffer + 232, buffer + 464, 232);
		goto send_burst;
	}

	/* Check the current TCH mode */
	switch (lchan->tch_mode) {
	case GSM48_CMODE_SIGN:
	case GSM48_CMODE_SPEECH_V1: /* HR */
		l2_len = GSM_HR_BYTES + 1;
		break;
	case GSM48_CMODE_SPEECH_AMR: /* AMR */
		/**
		 * TODO: AMR requires a dedicated loop,
		 * which will be implemented later...
		 */
		LOGP_LCHAND(lchan, LOGL_ERROR,
			    "AMR isn't supported yet, dropping frame...\n");

		/* Forget this primitive */
		l1sched_prim_drop(lchan);
		return -ENOTSUP;
	default:
		LOGP_LCHAND(lchan, LOGL_ERROR,
			    "Invalid TCH mode: %u, dropping frame...\n",
			    lchan->tch_mode);

		/* Forget this primitive */
		l1sched_prim_drop(lchan);
		return -EINVAL;
	}

	/* Determine payload length */
	if (L1SCHED_PRIM_IS_FACCH(lchan->prim)) {
		l2_len = GSM_MACBLOCK_LEN; /* FACCH */
	} else if (lchan->prim->payload_len != l2_len) {
		LOGP_LCHAND(lchan, LOGL_ERROR, "Primitive has odd length %zu "
			    "(expected %zu for TCH or %u for FACCH), so dropping...\n",
			    lchan->prim->payload_len, l2_len, GSM_MACBLOCK_LEN);

		/* Forget this primitive */
		l1sched_prim_drop(lchan);
		return -EINVAL;
	}

	/* Encode the payload */
	rc = gsm0503_tch_hr_encode(buffer, lchan->prim->payload, l2_len);
	if (rc) {
		LOGP_LCHAND(lchan, LOGL_ERROR, "Failed to encode L2 payload (len=%zu): %s\n",
			    lchan->prim->payload_len, osmo_hexdump(lchan->prim->payload,
								   lchan->prim->payload_len));

		/* Forget this primitive */
		l1sched_prim_drop(lchan);
		return -EINVAL;
	}

	/* A FACCH/H frame occupies 6 bursts */
	if (L1SCHED_PRIM_IS_FACCH(lchan->prim))
		lchan->ul_facch_blocks = 6;

send_burst:
	/* Determine which burst should be sent */
	offset = buffer + br->bid * 116;

	/* Update mask */
	*mask |= (1 << br->bid);

	/* Choose proper TSC */
	tsc = l1sched_nb_training_bits[lchan->tsc];

	/* Compose a new burst */
	memset(br->burst, 0, 3); /* TB */
	memcpy(br->burst + 3, offset, 58); /* Payload 1/2 */
	memcpy(br->burst + 61, tsc, 26); /* TSC */
	memcpy(br->burst + 87, offset + 58, 58); /* Payload 2/2 */
	memset(br->burst + 145, 0, 3); /* TB */
	br->burst_len = GSM_BURST_LEN;

	LOGP_LCHAND(lchan, LOGL_DEBUG, "Scheduled fn=%u burst=%u\n", br->fn, br->bid);

	/* In case of a FACCH/H frame, one block less */
	if (lchan->ul_facch_blocks)
		lchan->ul_facch_blocks--;

	if ((*mask & 0x0f) == 0x0f) {
		/**
		 * If no more FACCH/H blocks pending,
		 * confirm data / traffic sending
		 */
		if (!lchan->ul_facch_blocks) {
			enum l1sched_data_type dt = L1SCHED_PRIM_IS_TCH(lchan->prim) ?
							L1SCHED_DT_TRAFFIC : L1SCHED_DT_SIGNALING;
			l1sched_handle_data_cnf(lchan, br->fn, dt);
		}

		/* Forget processed primitive */
		l1sched_prim_drop(lchan);
	}

	return 0;
}
