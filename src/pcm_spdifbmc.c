// SPDX-License-Identifier: GPL-2.0-only
/*
 * spdifbmc: ALSA plugin that turns PCM into a biphase-mark coded S/PDIF stream for
 * an ordinary I2S output. One stereo frame becomes two frames of S32_LE stereo at twice the
 * rate; the I2S block of a Raspberry Pi then sends S/PDIF on its data pin with the stock
 * bcm2835-i2s driver.
 *
 *   pcm_type.spdifbmc { lib "/path/libasound_module_pcm_spdifbmc.so" }
 *   pcm.bmc { type spdifbmc slave.pcm "hw:CARD=sndrpihifiberry" }
 *
 * aes0 .. aes3 set the first channel status bytes as Kodi passes them for passthrough; without
 * them the status is derived from the stream. extra_ms N keeps N ms of coded silence queued ahead of the
 * audio: more delay, reported to the player, which an early copy (alsa-leadtap) needs as head start.
 *
 * The slave is given by name. Not linked against libasound: no *_near and no get_*_min/max
 * calls (they would bind to the old ALSA_0.9 symbols).
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "spdif_bmc.c"

#define CHUNK 1024 /* stereo frames encoded per call */

typedef struct {
	snd_pcm_ioplug_t io;
	snd_pcm_t *slave;
	struct spdif_bmc enc;
	snd_pcm_uframes_t slave_buffer;
	uint64_t written; /* application frames handed to the slave since prepare */
	snd_pcm_sframes_t last_ptr;
	int nonaudio;
	int aes[4]; /* channel status bytes from the configuration, -1: not given */
	unsigned int extra_ms;
	snd_pcm_uframes_t extra; /* application frames of silence queued ahead of the audio */
	unsigned int pend, pos;	 /* encoded slave frames the slave has not taken yet, and where they start */
	u32 out[CHUNK * SPDIF_BMC_WORDS];
} bmc_t;

/* Hand the pending slave frames over. Encoded data is never dropped or re-encoded: what the
   slave does not take stays pending and goes first next time. */
static int flush(bmc_t *b)
{
	while (b->pend) {
		snd_pcm_sframes_t r = snd_pcm_writei(b->slave, b->out + b->pos * 2, b->pend);

		if (r < 0)
			return (int)r;
		if (!r)
			return -EAGAIN;
		b->pos += (unsigned int)r;
		b->pend -= (unsigned int)r;
	}
	return 0;
}

/* a failure of snd_pcm_drop on a slave that is not running is not one */
static int benign(int err)
{
	return err == -EBADFD ? 0 : err;
}

static void set_status(bmc_t *b)
{
	u8 cs[SPDIF_BMC_STATUS_BYTES] = {0};
	u8 fs;

	switch (b->io.rate) {
	case 32000: fs = 0x3; break;
	case 44100: fs = 0x0; break;
	case 88200: fs = 0x8; break;
	case 96000: fs = 0xa; break;
	default: fs = 0x2; break;
	}
	cs[0] = 0x04 | (b->nonaudio ? 0x02 : 0); /* consumer, no copyright asserted */
	cs[2] = 0x10;				 /* channel number 1 for A; B gets 2 */
	cs[3] = fs;
	cs[4] = 0x0b;				 /* 24 bit */
	for (unsigned int i = 0; i < 4; i++)
		if (b->aes[i] >= 0)
			cs[i] = (u8)b->aes[i];
	if (b->aes[2] >= 0)
		cs[2] = 0x10; /* channel numbers are ours whatever the player says */
	spdif_bmc_set_status(&b->enc, cs);
}

static int bm_start(snd_pcm_ioplug_t *io)
{
	return snd_pcm_start(((bmc_t *)io->private_data)->slave);
}

static int bm_stop(snd_pcm_ioplug_t *io)
{
	return benign(snd_pcm_drop(((bmc_t *)io->private_data)->slave));
}

static snd_pcm_sframes_t bm_pointer(snd_pcm_ioplug_t *io)
{
	bmc_t *b = io->private_data;
	snd_pcm_state_t st = snd_pcm_state(b->slave);
	snd_pcm_sframes_t av;
	uint64_t queued, done;

	if (st == SND_PCM_STATE_XRUN)
		return -EPIPE;
	if (st == SND_PCM_STATE_SETUP || st == SND_PCM_STATE_OPEN)
		return b->last_ptr;
	av = snd_pcm_avail(b->slave);
	if (av < 0)
		return av;
	/* slave frames in the slave or still pending, rounded up to whole application frames */
	queued = (((snd_pcm_uframes_t)av < b->slave_buffer ? b->slave_buffer - (snd_pcm_uframes_t)av : 0) + b->pend + 1) / 2;
	/* the application must not count the silence as room: it keeps the slave that much fuller */
	done = b->written + b->extra > queued ? b->written + b->extra - queued : 0;
	if (done > b->written)
		done = b->written;
	b->last_ptr = (snd_pcm_sframes_t)(done % io->buffer_size);
	return b->last_ptr;
}

static int bm_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
	snd_pcm_sframes_t dl;
	int err = snd_pcm_delay(((bmc_t *)io->private_data)->slave, &dl);

	if (err < 0)
		return err;
	*delayp = dl / 2;
	return 0;
}

static snd_pcm_sframes_t bm_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas,
				     snd_pcm_uframes_t offset, snd_pcm_uframes_t size)
{
	bmc_t *b = io->private_data;
	const char *src = (const char *)areas[0].addr + (areas[0].first + offset * areas[0].step) / 8;
	snd_pcm_uframes_t n, i;
	snd_pcm_sframes_t av, r;
	u32 *o = b->out;

	for (;;) {
		av = flush(b);
		if (av == -EAGAIN)
			av = 0;
		else if (!av)
			av = snd_pcm_avail(b->slave);
		if (av == -EPIPE) {
			snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
			return -EPIPE;
		}
		if (av < 0)
			return av;
		if (av >= 2 && !b->pend)
			break;
		if (io->nonblock)
			return -EAGAIN;
		r = snd_pcm_wait(b->slave, 100);
		if (r < 0) {
			if (r == -EPIPE)
				snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
			return r;
		}
	}
	n = size < CHUNK ? size : CHUNK;
	if (n > (snd_pcm_uframes_t)av / 2)
		n = (snd_pcm_uframes_t)av / 2;

	for (i = 0; i < n; i++, o += SPDIF_BMC_WORDS) {
		s32 l, r32;

		if (io->format == SND_PCM_FORMAT_S16_LE) {
			const int16_t *p = (const int16_t *)src + i * 2;

			l = (s32)p[0] * 65536;
			r32 = (s32)p[1] * 65536;
		} else {
			const int32_t *p = (const int32_t *)src + i * 2;

			l = p[0];
			r32 = p[1];
		}
		spdif_bmc_encode(&b->enc, o, l, r32);
	}

	/* The frames count as accepted once encoded; whatever the slave does not take now (short
	   write, error) is delivered or reported by the next call or by pointer. */
	b->pend = (unsigned int)n * 2;
	b->pos = 0;
	flush(b);
	b->written += n;
	return (snd_pcm_sframes_t)n;
}

static int bm_prepare(snd_pcm_ioplug_t *io)
{
	bmc_t *b = io->private_data;
	int err = snd_pcm_prepare(b->slave);

	if (err < 0)
		return err;
	b->written = b->pend = 0;
	b->last_ptr = 0;
	set_status(b);
	spdif_bmc_reset(&b->enc);
	for (snd_pcm_uframes_t left = b->extra; left;) {
		snd_pcm_uframes_t n = left < CHUNK ? left : CHUNK, i;
		u32 *o = b->out;

		for (i = 0; i < n; i++, o += SPDIF_BMC_WORDS)
			spdif_bmc_encode(&b->enc, o, 0, 0);
		b->pend = (unsigned int)n * 2;
		b->pos = 0;
		err = flush(b);
		if (err < 0) /* the slave cannot hold the lead (-EAGAIN) or failed: prepare is repeated whole */
			return err == -EAGAIN ? -EIO : err;
		left -= n;
	}
	return 0;
}

static int bm_drain(snd_pcm_ioplug_t *io)
{
	bmc_t *b = io->private_data;
	int err;

	snd_pcm_nonblock(b->slave, 0);
	err = flush(b);
	if (!err && snd_pcm_state(b->slave) == SND_PCM_STATE_PREPARED && b->written)
		err = snd_pcm_start(b->slave);
	if (!err)
		err = benign(snd_pcm_drain(b->slave));
	snd_pcm_nonblock(b->slave, 1);
	return err;
}

static int bm_pause(snd_pcm_ioplug_t *io, int enable)
{
	return snd_pcm_pause(((bmc_t *)io->private_data)->slave, enable);
}

/* Slave start threshold and wake-up. ALSA expects a standing device in PREPARED; bm_start is the
   one place that starts it. The slave buffer can exceed twice ours (whole periods only): wake up
   when one period of OUR buffer is free, or poll would report the slave ready all the time. */
static int slave_sw(bmc_t *b)
{
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t boundary;
	int err;

	snd_pcm_sw_params_alloca(&sw);
	if ((err = snd_pcm_sw_params_current(b->slave, sw)) < 0 ||
	    (err = snd_pcm_sw_params_get_boundary(sw, &boundary)) < 0 ||
	    (err = snd_pcm_sw_params_set_start_threshold(b->slave, sw, boundary)) < 0 ||
	    (err = snd_pcm_sw_params_set_avail_min(b->slave, sw, b->slave_buffer - (b->io.buffer_size + b->extra) * 2 + b->io.period_size * 2)) < 0)
		return err;
	return snd_pcm_sw_params(b->slave, sw);
}

/* Slave hw_params: the exact period and buffer we want, or, if the slave refuses that at this
   rate, just a buffer of at least the size (the avail_min above copes with a larger one). */
static int slave_hw(bmc_t *b, snd_pcm_hw_params_t *hw, const snd_pcm_hw_params_t *base, int exact,
		    snd_pcm_uframes_t period, snd_pcm_uframes_t buffer)
{
	int err;

	snd_pcm_hw_params_copy(hw, base);
	if (exact && snd_pcm_hw_params_set_period_size(b->slave, hw, period, 0) < 0)
		snd_pcm_hw_params_copy(hw, base);
	if ((err = snd_pcm_hw_params_set_buffer_size_min(b->slave, hw, &buffer)) < 0 ||
	    (exact && (err = snd_pcm_hw_params_set_buffer_size(b->slave, hw, buffer)) < 0))
		return err;
	return snd_pcm_hw_params(b->slave, hw);
}

static int bm_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
	bmc_t *b = io->private_data;
	snd_pcm_hw_params_t *hw, *base;
	snd_pcm_uframes_t period;
	int err;

	(void)params;
	b->extra = (snd_pcm_uframes_t)io->rate * b->extra_ms / 1000;
	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_alloca(&base);
	if ((err = snd_pcm_hw_params_any(b->slave, base)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(b->slave, base, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(b->slave, base, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(b->slave, base, 2)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(b->slave, base, io->rate * 2, 0)) < 0)
		return err;
	if ((err = slave_hw(b, hw, base, 1, io->period_size * 2, (io->buffer_size + b->extra) * 2)) < 0 &&
	    (err = slave_hw(b, hw, base, 0, io->period_size * 2, (io->buffer_size + b->extra) * 2)) < 0)
		return err;
	if ((err = snd_pcm_get_params(b->slave, &b->slave_buffer, &period)) < 0)
		return err;
	return slave_sw(b);
}

static int bm_hw_free(snd_pcm_ioplug_t *io)
{
	return snd_pcm_hw_free(((bmc_t *)io->private_data)->slave);
}

static int bm_poll_count(snd_pcm_ioplug_t *io)
{
	return snd_pcm_poll_descriptors_count(((bmc_t *)io->private_data)->slave);
}

static int bm_poll_descriptors(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int space)
{
	return snd_pcm_poll_descriptors(((bmc_t *)io->private_data)->slave, pfd, space);
}

static int bm_poll_revents(snd_pcm_ioplug_t *io, struct pollfd *pfd, unsigned int nfds, unsigned short *revents)
{
	return snd_pcm_poll_descriptors_revents(((bmc_t *)io->private_data)->slave, pfd, nfds, revents);
}

static int bm_close(snd_pcm_ioplug_t *io)
{
	bmc_t *b = io->private_data;

	if (b->slave)
		snd_pcm_close(b->slave);
	free(b);
	return 0;
}

static const snd_pcm_ioplug_callback_t bm_callback = {
	.start = bm_start,
	.stop = bm_stop,
	.pointer = bm_pointer,
	.transfer = bm_transfer,
	.close = bm_close,
	.hw_params = bm_hw_params,
	.hw_free = bm_hw_free,
	.prepare = bm_prepare,
	.drain = bm_drain,
	.pause = bm_pause,
	.resume = NULL,
	.delay = bm_delay,
	.poll_descriptors_count = bm_poll_count,
	.poll_descriptors = bm_poll_descriptors,
	.poll_revents = bm_poll_revents,
};

/* Limits of the application from the slave's, a slave frame being half an application frame.
   set_*_min/max report the resulting limit; the get_*_min/max getters must not be used here. */
static int constraints(bmc_t *b)
{
	static const unsigned int access[] = {SND_PCM_ACCESS_RW_INTERLEAVED};
	static const unsigned int formats[] = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S32_LE};
	static const unsigned int rates[] = {32000, 44100, 48000, 88200, 96000};
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t bmin = 1, bmax = 1u << 24, pmin = 1, pmax = 1u << 20;
	int err, dir = 0;

	snd_pcm_hw_params_alloca(&hw);
	if ((err = snd_pcm_hw_params_any(b->slave, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(b->slave, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(b->slave, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(b->slave, hw, 2)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_min(b->slave, hw, &bmin)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_max(b->slave, hw, &bmax)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_min(b->slave, hw, &pmin, &dir)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_max(b->slave, hw, &pmax, &dir)) < 0)
		return err;
	/* bytes of the application: S32 frames take the most for the minimum, S16 for the maximum */
	if ((err = snd_pcm_ioplug_set_param_list(&b->io, SND_PCM_IOPLUG_HW_ACCESS, 1, access)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&b->io, SND_PCM_IOPLUG_HW_FORMAT, 2, formats)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&b->io, SND_PCM_IOPLUG_HW_CHANNELS, 2, 2)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&b->io, SND_PCM_IOPLUG_HW_RATE, 5, rates)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&b->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, (unsigned int)(pmin * 4), (unsigned int)(pmax * 2))) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&b->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 1024)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&b->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, (unsigned int)(bmin * 4), (unsigned int)(bmax * 2))) < 0)
		return err;
	return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(spdifbmc)
{
	snd_config_iterator_t i, next;
	const char *slave_name = NULL;
	int nonaudio = 0, aes[4] = {-1, -1, -1, -1}, err;
	long extra_ms = 0;
	bmc_t *b;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i), *m;
		const char *id;

		if (snd_config_get_id(n, &id) < 0 || !strcmp(id, "comment") || !strcmp(id, "type") || !strcmp(id, "hint"))
			continue;
		if (!strcmp(id, "slave")) {
			if (snd_config_search(n, "pcm", &m) < 0 || snd_config_get_string(m, &slave_name) < 0) {
				SNDERR("spdifbmc: slave.pcm must be a name");
				return -EINVAL;
			}
			continue;
		}
		if (!strcmp(id, "nonaudio")) {
			if ((nonaudio = snd_config_get_bool(n)) < 0)
				return nonaudio;
			continue;
		}
		if (!strcmp(id, "extra_ms")) {
			if (snd_config_get_integer(n, &extra_ms) < 0 || extra_ms < 0 || extra_ms > 500) {
				SNDERR("spdifbmc: extra_ms must be 0..500");
				return -EINVAL;
			}
			continue;
		}
		if (!strncmp(id, "aes", 3) && id[3] >= '0' && id[3] <= '3' && !id[4]) {
			long v;

			if (snd_config_get_integer(n, &v) < 0 || v < -1 || v > 255) {
				SNDERR("spdifbmc: %s must be a byte", id);
				return -EINVAL;
			}
			aes[id[3] - '0'] = (int)v;
			continue;
		}
		SNDERR("spdifbmc: unknown field %s", id);
		return -EINVAL;
	}
	if (!slave_name) {
		SNDERR("spdifbmc: slave.pcm is required");
		return -EINVAL;
	}
	if (stream != SND_PCM_STREAM_PLAYBACK)
		return -EINVAL;

	b = calloc(1, sizeof(*b));
	if (!b)
		return -ENOMEM;
	b->nonaudio = nonaudio;
	memcpy(b->aes, aes, sizeof(aes));
	b->extra_ms = (unsigned int)extra_ms;
	spdif_bmc_init(&b->enc);
	err = snd_pcm_open_lconf(&b->slave, slave_name, stream, SND_PCM_NONBLOCK, root);
	if (err < 0) {
		SNDERR("spdifbmc: cannot open slave %s: %s", slave_name, snd_strerror(err));
		free(b);
		return err;
	}
	b->io.version = SND_PCM_IOPLUG_VERSION;
	b->io.name = "S/PDIF biphase-mark coder";
	b->io.callback = &bm_callback;
	b->io.private_data = b;
	err = snd_pcm_ioplug_create(&b->io, name, stream, mode);
	if (err < 0) {
		snd_pcm_close(b->slave);
		free(b);
		return err;
	}
	b->io.nonblock = !!(mode & SND_PCM_NONBLOCK);
	err = constraints(b);
	if (err < 0) {
		snd_pcm_ioplug_delete(&b->io);
		return err;
	}
	*pcmp = b->io.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(spdifbmc);
