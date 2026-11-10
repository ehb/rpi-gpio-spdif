// SPDX-License-Identifier: GPL-2.0-only
/*
 * spdifbmc: ALSA plugin that biphase-mark codes S/PDIF sub-frames for an ordinary I2S output.
 * It takes IEC958_SUBFRAME_LE, the format alsa-lib's iec958 plugin produces (channel status,
 * preamble marks, validity and parity are iec958's), and writes each stereo frame as two
 * frames of S32_LE stereo at twice the rate; the I2S block of a Raspberry Pi then sends
 * S/PDIF on its data pin with the stock bcm2835-i2s driver.
 *
 *   pcm_type.spdifbmc { lib "/path/libasound_module_pcm_spdifbmc.so" }
 *   pcm.bmc_coder { type spdifbmc slave.pcm "hw:CARD=spdifgpio21" extra_ms 80 }
 *   pcm.spdif_out {
 *       type iec958
 *       slave { pcm "bmc_coder" format IEC958_SUBFRAME_LE }
 *       status [ 0x04 0x82 0x00 0x01 ]
 *   }
 *
 * extra_ms N keeps N ms of coded silence queued ahead of the audio: more delay, reported to
 * the player, which an early copy (alsa-leadtap) needs as head start. It is rounded up to
 * whole 192-frame blocks, so that iec958's first block starts on a block boundary; the
 * silence carries a plain PCM channel status with the stream's rate.
 *
 * The slave is a hw device given by name. ioplug never passes transfer more than the free
 * room its pointer reports, which fits into the slave, and a hw device takes a write that fits
 * whole: no pending output, no waiting here. Not linked against libasound: no *_near and no
 * get_*_min/max calls (they would bind to the old ALSA_0.9 symbols).
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

#include "spdif_bmc.c"

#define CHUNK 1024 /* stereo frames encoded per write */

static const unsigned int rates[] = {32000, 44100, 48000, 88200, 96000};
static const uint8_t rate_cs[] = {0x03, 0x00, 0x02, 0x08, 0x0a}; /* channel status byte 3 */

typedef struct {
	snd_pcm_ioplug_t io;
	snd_pcm_t *slave;
	snd_pcm_uframes_t slave_buffer;
	uint64_t written; /* application frames handed to the slave since prepare */
	unsigned int extra_ms;
	snd_pcm_uframes_t extra; /* application frames of silence queued ahead of the audio */
	uint32_t out[CHUNK * SPDIF_BMC_WORDS];
} bmc_t;

/* Write n encoded frames from out; a hw slave takes them whole (see the top). */
static snd_pcm_sframes_t put(bmc_t *b, snd_pcm_uframes_t n)
{
	snd_pcm_sframes_t r = snd_pcm_writei(b->slave, b->out, n * 2);

	return r < 0 ? r : r / 2;
}

/* C bit of frame f of the lead silence: consumer, PCM, original (iec958's default byte 1), rate */
static unsigned int lead_cbit(unsigned int rate, unsigned int f)
{
	uint8_t cs[4] = {0x04, 0x82, 0x00, 0x01};

	for (unsigned int i = 0; i < 5; i++)
		if (rates[i] == rate)
			cs[3] = rate_cs[i];
	return f < 32 ? (cs[f >> 3] >> (f & 7)) & 1 : 0;
}

static int bm_start(snd_pcm_ioplug_t *io)
{
	return snd_pcm_start(((bmc_t *)io->private_data)->slave);
}

static int bm_stop(snd_pcm_ioplug_t *io)
{
	return snd_pcm_drop(((bmc_t *)io->private_data)->slave);
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
		return io->buffer_size ? (snd_pcm_sframes_t)(io->hw_ptr % io->buffer_size) : 0;
	av = snd_pcm_avail(b->slave);
	if (av < 0)
		return av;
	/* slave frames in the slave, rounded up to whole application frames */
	queued = (((snd_pcm_uframes_t)av < b->slave_buffer ? b->slave_buffer - (snd_pcm_uframes_t)av : 0) + 1) / 2;
	/* the application must not count the silence as room: it keeps the slave that much fuller */
	done = b->written + b->extra > queued ? b->written + b->extra - queued : 0;
	if (done > b->written)
		done = b->written;
	return (snd_pcm_sframes_t)(done % io->buffer_size);
}

static int bm_delay(snd_pcm_ioplug_t *io, snd_pcm_sframes_t *delayp)
{
	int err = snd_pcm_delay(((bmc_t *)io->private_data)->slave, delayp);

	*delayp /= 2; /* not used by ioplug on error */
	return err;
}

/*
 * All of size: a short return makes an alsa-lib plugin in front (iec958, via mmap) undo frames
 * it has already counted, and iec958's 192-frame counter does not follow an undo, so its
 * channel status blocks would slip.
 */
static snd_pcm_sframes_t bm_transfer(snd_pcm_ioplug_t *io, const snd_pcm_channel_area_t *areas,
				     snd_pcm_uframes_t offset, snd_pcm_uframes_t size)
{
	bmc_t *b = io->private_data;
	const uint32_t *src = (const uint32_t *)((const char *)areas[0].addr + (areas[0].first + offset * areas[0].step) / 8);
	snd_pcm_uframes_t total = 0;

	while (total < size) {
		snd_pcm_uframes_t n = size - total < CHUNK ? size - total : CHUNK, i;
		snd_pcm_sframes_t r;

		for (i = 0; i < n; i++)
			spdif_bmc_encode_subframes(b->out + i * SPDIF_BMC_WORDS, src[(total + i) * 2], src[(total + i) * 2 + 1]);
		r = put(b, n);
		if (r == -EPIPE)
			snd_pcm_ioplug_set_state(io, SND_PCM_STATE_XRUN);
		if (r < 0)
			return total ? (snd_pcm_sframes_t)total : r;
		b->written += (snd_pcm_uframes_t)r;
		total += (snd_pcm_uframes_t)r;
		if ((snd_pcm_uframes_t)r < n) /* not a hw-like slave: report what it took */
			break;
	}
	return (snd_pcm_sframes_t)total;
}

static int bm_prepare(snd_pcm_ioplug_t *io)
{
	bmc_t *b = io->private_data;
	snd_pcm_uframes_t f = 0;
	int err = snd_pcm_prepare(b->slave);

	if (err < 0)
		return err;
	b->written = 0;
	/* the lead silence, whole blocks; the slave stands (start threshold) until bm_start */
	while (f < b->extra) {
		snd_pcm_uframes_t n = b->extra - f < CHUNK ? b->extra - f : CHUNK, i;
		snd_pcm_sframes_t r;

		for (i = 0; i < n; i++, f++) {
			unsigned int k = (unsigned int)(f % SPDIF_BMC_BLOCK);
			uint32_t c = lead_cbit(io->rate, k) << 30;

			spdif_bmc_encode_subframes(b->out + i * SPDIF_BMC_WORDS,
						   (k ? SPDIF_BMC_MARK_M : SPDIF_BMC_MARK_B) | c, SPDIF_BMC_MARK_W | c);
		}
		r = put(b, n);
		if (r < 0)
			return (int)r;
		if ((snd_pcm_uframes_t)r < n)
			return -EIO;
	}
	return 0;
}

/* the kernel starts a prepared stream that holds data by itself on drain */
static int bm_drain(snd_pcm_ioplug_t *io)
{
	return snd_pcm_drain(((bmc_t *)io->private_data)->slave);
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

/* Slave hw_params: twice our period if the slave allows it, and the buffer for ours plus the
   lead, exactly or else at least (a failed set_* leaves the params as they were). */
static int bm_hw_params(snd_pcm_ioplug_t *io, snd_pcm_hw_params_t *params)
{
	bmc_t *b = io->private_data;
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t buffer, period;
	int err;

	(void)params;
	b->extra = (snd_pcm_uframes_t)io->rate * b->extra_ms / 1000;
	b->extra = (b->extra + SPDIF_BMC_BLOCK - 1) / SPDIF_BMC_BLOCK * SPDIF_BMC_BLOCK;
	buffer = (io->buffer_size + b->extra) * 2;
	snd_pcm_hw_params_alloca(&hw);
	if ((err = snd_pcm_hw_params_any(b->slave, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(b->slave, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(b->slave, hw, SND_PCM_FORMAT_S32_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(b->slave, hw, 2)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(b->slave, hw, io->rate * 2, 0)) < 0)
		return err;
	snd_pcm_hw_params_set_period_size(b->slave, hw, io->period_size * 2, 0);
	if (snd_pcm_hw_params_set_buffer_size(b->slave, hw, buffer) < 0 &&
	    (err = snd_pcm_hw_params_set_buffer_size_min(b->slave, hw, &buffer)) < 0)
		return err;
	if ((err = snd_pcm_hw_params(b->slave, hw)) < 0 ||
	    (err = snd_pcm_get_params(b->slave, &b->slave_buffer, &period)) < 0)
		return err;
	return slave_sw(b);
}

/* a hw slave has more than the one poll descriptor ioplug's default handles */
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

	snd_pcm_close(b->slave); /* close only runs once ioplug exists, i.e. after the slave opened */
	free(b);
	return 0;
}

/* No pause: the Pi's DMA engine cannot pause, so the card does not offer it either. */
static const snd_pcm_ioplug_callback_t bm_callback = {
	.start = bm_start,
	.stop = bm_stop,
	.pointer = bm_pointer,
	.transfer = bm_transfer,
	.close = bm_close,
	.hw_params = bm_hw_params,
	.prepare = bm_prepare,
	.drain = bm_drain,
	.delay = bm_delay,
	.poll_descriptors_count = bm_poll_count,
	.poll_descriptors = bm_poll_descriptors,
	.poll_revents = bm_poll_revents,
};

/* Limits of the application from the slave's, a slave frame being half an application frame.
   set_*_min/max report the resulting limit; the get_*_min/max getters must not be used here. */
static int constraints(bmc_t *b)
{
	/* mmap for alsa-lib plugins in front (iec958 wants it from its slave); ioplug emulates it
	   with its own buffer and hands the frames to transfer at commit */
	static const unsigned int access[] = {SND_PCM_ACCESS_RW_INTERLEAVED, SND_PCM_ACCESS_MMAP_INTERLEAVED};
	static const unsigned int formats[] = {SND_PCM_FORMAT_IEC958_SUBFRAME_LE};
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
	/* bytes: an application frame (two 32-bit sub-frames) is as big as a slave frame, and
	   there are half as many */
	if ((err = snd_pcm_ioplug_set_param_list(&b->io, SND_PCM_IOPLUG_HW_ACCESS, 2, access)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&b->io, SND_PCM_IOPLUG_HW_FORMAT, 1, formats)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&b->io, SND_PCM_IOPLUG_HW_CHANNELS, 2, 2)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_list(&b->io, SND_PCM_IOPLUG_HW_RATE, 5, rates)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&b->io, SND_PCM_IOPLUG_HW_PERIOD_BYTES, (unsigned int)(pmin * 4), (unsigned int)(pmax * 4))) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&b->io, SND_PCM_IOPLUG_HW_PERIODS, 2, 1024)) < 0 ||
	    (err = snd_pcm_ioplug_set_param_minmax(&b->io, SND_PCM_IOPLUG_HW_BUFFER_BYTES, (unsigned int)(bmin * 4), (unsigned int)(bmax * 4))) < 0)
		return err;
	return 0;
}

SND_PCM_PLUGIN_DEFINE_FUNC(spdifbmc)
{
	snd_config_iterator_t i, next;
	const char *slave_name = NULL;
	long extra_ms = 0;
	bmc_t *b;
	int err;

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
		if (!strcmp(id, "extra_ms")) {
			if (snd_config_get_integer(n, &extra_ms) < 0 || extra_ms < 0 || extra_ms > 500) {
				SNDERR("spdifbmc: extra_ms must be 0..500");
				return -EINVAL;
			}
			continue;
		}
		SNDERR("spdifbmc: unknown field %s (channel status comes from the iec958 plugin in front)", id);
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
	b->extra_ms = (unsigned int)extra_ms;
	err = snd_pcm_open_lconf(&b->slave, slave_name, stream, 0, root);
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
	err = constraints(b);
	if (err < 0) {
		snd_pcm_ioplug_delete(&b->io);
		return err;
	}
	*pcmp = b->io.pcm;
	return 0;
}

SND_PCM_PLUGIN_SYMBOL(spdifbmc);
