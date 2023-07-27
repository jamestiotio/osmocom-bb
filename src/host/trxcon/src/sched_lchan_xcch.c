/*
 * OsmocomBB <-> SDR connection bridge
 * TDMA scheduler: handlers for DL / UL bursts on logical channels
 *
 * (C) 2017-2022 by Vadim Yanitskiy <axilirator@gmail.com>
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

#include <osmocom/core/logging.h>
#include <osmocom/core/bits.h>

#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/protocol/gsm_04_08.h>
#include <osmocom/coding/gsm0503_coding.h>

#include <osmocom/bb/l1sched/l1sched.h>
#include <osmocom/bb/l1sched/logging.h>

int rx_data_fn(struct l1sched_lchan_state *lchan,
	       const struct l1sched_burst_ind *bi)
{
	uint8_t l2[GSM_MACBLOCK_LEN], *mask;
	int n_errors, n_bits_total, rc;
	sbit_t *bursts_p, *burst;

	/* Set up pointers */
	mask = &lchan->rx_burst_mask;
	bursts_p = lchan->rx_bursts;

	LOGP_LCHAND(lchan, LOGL_DEBUG,
		    "Data received: fn=%u bid=%u\n", bi->fn, bi->bid);

	/* Align to the first burst of a block */
	if (*mask == 0x00 && bi->bid != 0)
		return 0;

	/* Update mask */
	*mask |= (1 << bi->bid);

	/* Store the measurements */
	l1sched_lchan_meas_push(lchan, bi);

	/* Copy burst to buffer of 4 bursts */
	burst = bursts_p + bi->bid * 116;
	memcpy(burst, bi->burst + 3, 58);
	memcpy(burst + 58, bi->burst + 87, 58);

	/* Wait until complete set of bursts */
	if (bi->bid != 3)
		return 0;

	/* Calculate AVG of the measurements */
	l1sched_lchan_meas_avg(lchan, 4);

	/* Check for complete set of bursts */
	if ((*mask & 0xf) != 0xf) {
		LOGP_LCHAND(lchan, LOGL_ERROR,
			    "Received incomplete (%s) data frame at fn=%u (%u/%u)\n",
			    l1sched_burst_mask2str(mask, 4), lchan->meas_avg.fn,
			    lchan->meas_avg.fn % lchan->ts->mf_layout->period,
			    lchan->ts->mf_layout->period);
		/* NOTE: xCCH has an insane amount of redundancy for error
		 * correction, so even just 2 valid bursts might be enough
		 * to reconstruct some L2 frames. This is why we do not
		 * abort here. */
	}

	/* Keep the mask updated */
	*mask = *mask << 4;

	/* Attempt to decode */
	rc = gsm0503_xcch_decode(l2, bursts_p, &n_errors, &n_bits_total);
	if (rc) {
		LOGP_LCHAND(lchan, LOGL_ERROR,
			    "Received bad frame (rc=%d, ber=%d/%d) at fn=%u\n",
			    rc, n_errors, n_bits_total, lchan->meas_avg.fn);
	}

	/* Send a L2 frame to the higher layers */
	return l1sched_lchan_emit_data_ind(lchan, l2, rc ? 0 : GSM_MACBLOCK_LEN,
					   n_errors, n_bits_total, false);
}

static struct msgb *prim_dequeue_xcch(struct l1sched_lchan_state *lchan)
{
	struct msgb *msg;

	if (L1SCHED_CHAN_IS_SACCH(lchan->type))
		return l1sched_lchan_prim_dequeue_sacch(lchan);
	if ((msg = msgb_dequeue(&lchan->tx_prims)) == NULL)
		return NULL;

	/* Check the prim payload length */
	if (msgb_l2len(msg) != GSM_MACBLOCK_LEN) {
		LOGP_LCHAND(lchan, LOGL_ERROR,
			    "Primitive has odd length %u (expected %u), so dropping...\n",
			    msgb_l2len(msg), GSM_MACBLOCK_LEN);
		msgb_free(msg);
		return NULL;
	}

	return msg;
}

int tx_data_fn(struct l1sched_lchan_state *lchan,
	       struct l1sched_burst_req *br)
{
	ubit_t *bursts_p, *burst;
	const uint8_t *tsc;
	uint8_t *mask;
	int rc;

	/* Set up pointers */
	mask = &lchan->tx_burst_mask;
	bursts_p = lchan->tx_bursts;

	if (br->bid > 0) {
		if ((*mask & 0x01) != 0x01)
			return -ENOENT;
		goto send_burst;
	}

	*mask = *mask << 4;

	struct msgb *msg = prim_dequeue_xcch(lchan);
	if (msg == NULL)
		msg = l1sched_lchan_prim_dummy_lapdm(lchan);
	OSMO_ASSERT(msg != NULL);

	/* Encode payload */
	rc = gsm0503_xcch_encode(bursts_p, msgb_l2(msg));
	if (rc) {
		LOGP_LCHAND(lchan, LOGL_ERROR, "Failed to encode L2 payload (len=%u): %s\n",
			    msgb_l2len(msg), msgb_hexdump_l2(msg));
		msgb_free(msg);
		return -EINVAL;
	}

	/* Confirm data sending (pass ownership of the msgb/prim) */
	l1sched_lchan_emit_data_cnf(lchan, msg, br->fn);

send_burst:
	/* Determine which burst should be sent */
	burst = bursts_p + br->bid * 116;

	/* Update mask */
	*mask |= (1 << br->bid);

	/* Choose proper TSC */
	tsc = l1sched_nb_training_bits[lchan->tsc];

	/* Compose a new burst */
	memset(br->burst, 0, 3); /* TB */
	memcpy(br->burst + 3, burst, 58); /* Payload 1/2 */
	memcpy(br->burst + 61, tsc, 26); /* TSC */
	memcpy(br->burst + 87, burst + 58, 58); /* Payload 2/2 */
	memset(br->burst + 145, 0, 3); /* TB */
	br->burst_len = GSM_NBITS_NB_GMSK_BURST;

	LOGP_LCHAND(lchan, LOGL_DEBUG, "Scheduled fn=%u burst=%u\n", br->fn, br->bid);

	return 0;
}
