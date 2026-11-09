// SPDX-License-Identifier: GPL-2.0-only
/*
 * S/PDIF output on a Raspberry Pi GPIO (PCM_DOUT, GPIO21 on the 40-pin header)
 * without a transceiver chip: the PCM/I2S block runs at fs x 128 with 32-bit frames
 * and sends a biphase-mark coded stream built in software (spdif_bmc.c). A TOSLINK
 * transmitter module on the pin is all the hardware needed. BCM2835..BCM2711 only;
 * the Pi 5 has a different I2S block.
 *
 * Why not a mode of bcm2835-i2s: that driver hands its data path to the generic
 * dmaengine PCM, which DMAs straight from the application buffer. Here every 4-byte
 * stereo sample becomes 16 bytes of line code, so the driver needs its own DMA ring
 * and its own pointer/delay accounting -- the part a mode would have to replace.
 *
 * - A cyclic DMA transfer over a ring of chunks x chunk_frames encoded frames feeds
 *   the FIFO. It starts in prepare and carries encoded silence while nothing plays,
 *   so the receiver stays locked ("Keep Alive" control).
 * - After each finished chunk the DMA callback encodes the chunks that are free again
 *   from the ALSA buffer. The playing chunk comes from the DMA residue, so merged
 *   callbacks lose nothing; pointer() reports the frames taken from the ALSA buffer,
 *   runtime->delay the encoded frames still ahead of the output.
 * - Channel status comes from the "IEC958 Playback Default" control (non-audio for
 *   AC3/DTS passthrough); sample rate and word length are filled in where the control
 *   leaves them unspecified. A new status starts with the next 192-frame block.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <sound/asoundef.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_iec958.h>
#include <sound/pcm_params.h>

#include "spdif_bmc.h"

#define DRV_NAME "rpi-gpio-spdif"

/* PCM/I2S registers and bits, as in sound/soc/bcm/bcm2835-i2s.c */
#define CS_A		0x00
#define FIFO_A		0x04
#define MODE_A		0x08
#define RXC_A		0x0c
#define TXC_A		0x10
#define DREQ_A		0x14
#define INTEN_A		0x18

#define CS_STBY		BIT(25)
#define CS_TXERR	BIT(15)
#define CS_DMAEN	BIT(9)
#define CS_TXTHR(v)	((v) << 5)
#define CS_TXCLR	BIT(3)
#define CS_TXON		BIT(2)
#define CS_EN		BIT(0)

#define MODE_FLEN(v)	((v) << 10)
#define MODE_FSLEN(v)	(v)

#define TXC_CH1(v)	((v) << 16)
#define CH_WEX		BIT(15)
#define CH_EN		BIT(14)
#define CH_WID(v)	(v)

#define DREQ_TX_PANIC(v) ((v) << 24)
#define DREQ_TX(v)	((v) << 8)

#define FRAME_BYTES	(SPDIF_BMC_WORDS * 4)	/* encoded stereo frame */
#define FIFO_FRAMES	16			/* 64-word TX FIFO */

static int index = SNDRV_DEFAULT_IDX1;
module_param(index, int, 0444);
MODULE_PARM_DESC(index, "ALSA card index");

static unsigned int chunk_frames = 256;
module_param(chunk_frames, uint, 0444);
MODULE_PARM_DESC(chunk_frames, "frames per DMA chunk (64..2048)");

static unsigned int chunks = 4;
module_param(chunks, uint, 0444);
MODULE_PARM_DESC(chunks, "DMA chunks in the ring (3..32); latency is about chunks-1 chunks");

static bool keep_alive = true;
module_param(keep_alive, bool, 0444);
MODULE_PARM_DESC(keep_alive, "initial value of the Keep Alive control");

struct rso {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct dma_chan *dma;
	dma_addr_t fifo_addr;
	dma_cookie_t cookie;		/* the running cyclic transfer */
	u32 *ring;			/* encoded frames, chunks x chunk_frames */
	dma_addr_t ring_dma;
	unsigned int chunk_frames, chunks;
	size_t chunk_bytes, ring_bytes;
	struct snd_card *card;

	struct mutex hw_lock;		/* clock and DMA start/stop */
	bool gone;			/* removed: no more starts; under hw_lock */
	bool notifying;			/* callback is inside ALSA with the substream */

	spinlock_t lock;		/* everything below */
	unsigned int rate;		/* rate the ring runs at, 0 = stopped */
	s64 late_us;			/* a callback this late means the ring wrapped */
	ktime_t last_cb;
	struct snd_pcm_substream *ss;	/* playback feeding the ring */
	bool active;			/* between trigger start and stop */
	bool keep_alive;
	unsigned int next_fill;		/* next chunk to encode */
	snd_pcm_uframes_t pos, period_acc;
	struct spdif_bmc enc;
	u8 status[SPDIF_BMC_STATUS_BYTES];	/* control value */
};

/* Channel status for the encoder; caller holds r->lock. */
static void rso_update_status(struct rso *r)
{
	u8 cs[SPDIF_BMC_STATUS_BYTES];

	memcpy(cs, r->status, sizeof(cs));
	if (r->ss)
		WARN_ON_ONCE(snd_pcm_fill_iec958_consumer(r->ss->runtime, cs, sizeof(cs)) < 0);
	spdif_bmc_set_status(&r->enc, cs);
}

static s32 rso_sample(const struct snd_pcm_runtime *rt, const void *frame, int ch)
{
	switch (rt->format) {
	case SNDRV_PCM_FORMAT_S16_LE:
		return (s32)((u32)(u16)((const s16 *)frame)[ch] << 16);
	case SNDRV_PCM_FORMAT_S24_LE:
		return (s32)((((const u32 *)frame)[ch] & 0xffffff) << 8);
	default:	/* S32_LE */
		return ((const s32 *)frame)[ch];
	}
}

/* Encode one chunk; caller holds r->lock. */
static void rso_fill_chunk(struct rso *r, unsigned int chunk, bool *period)
{
	u32 *out = r->ring + (size_t)chunk * r->chunk_frames * SPDIF_BMC_WORDS;
	struct snd_pcm_runtime *rt = r->active && r->ss ? r->ss->runtime : NULL;
	snd_pcm_sframes_t avail = 0;
	unsigned int i;

	if (rt) {
		/* written by the application and not yet taken; one snapshot of hw_ptr */
		snd_pcm_uframes_t hw = READ_ONCE(rt->status->hw_ptr);
		snd_pcm_sframes_t queued = READ_ONCE(rt->control->appl_ptr) - hw;

		/* hw_ptr may run ahead of the data (underrun); only a boundary wrap is huge */
		if (queued < -(snd_pcm_sframes_t)(rt->boundary / 2))
			queued += rt->boundary;
		avail = queued - (snd_pcm_sframes_t)((r->pos + rt->buffer_size -
						      hw % rt->buffer_size) % rt->buffer_size);
		if (avail < 0)
			avail = 0;
	}
	for (i = 0; i < r->chunk_frames; i++, out += SPDIF_BMC_WORDS) {
		s32 left = 0, right = 0;

		if (rt) {
			if ((snd_pcm_sframes_t)i < avail) {
				const void *f = rt->dma_area + frames_to_bytes(rt, r->pos);

				left = rso_sample(rt, f, 0);
				right = rso_sample(rt, f, 1);
			}
			/* advance even past the end of the data: ALSA then sees the xrun */
			if (++r->pos == rt->buffer_size)
				r->pos = 0;
		}
		spdif_bmc_encode(&r->enc, out, left, right);
	}
	if (rt) {
		r->period_acc += r->chunk_frames;
		if (r->period_acc >= rt->period_size) {
			r->period_acc %= rt->period_size;
			*period = true;
		}
	}
}

/* Chunk the DMA is reading now, from the residue of the cyclic transfer. */
static int rso_playing_chunk(struct rso *r, size_t *offset)
{
	struct dma_tx_state st;
	size_t done;

	/* residue 0 (end of the ring) also ends up here; the caller's fallback is right */
	if (dmaengine_tx_status(r->dma, r->cookie, &st) == DMA_COMPLETE ||
	    st.residue == 0 || st.residue > r->ring_bytes)
		return -1;
	done = r->ring_bytes - st.residue;
	if (offset)
		*offset = done % r->chunk_bytes;
	return done / r->chunk_bytes;
}

static void rso_dma_callback(void *arg)
{
	struct snd_pcm_substream *ss = NULL;
	bool period = false, lapped = false;
	struct rso *r = arg;
	ktime_t now = ktime_get();
	unsigned long flags;
	int playing;
	u32 cs;

	cs = readl(r->base + CS_A);
	if (cs & CS_TXERR) {
		writel(cs, r->base + CS_A);	/* write 1 to clear */
		dev_warn_ratelimited(r->dev, "TX FIFO underrun\n");
	}

	spin_lock_irqsave(&r->lock, flags);
	if (ktime_us_delta(now, r->last_cb) > r->late_us) {
		dev_warn_ratelimited(r->dev, "DMA callback %lld us late, part of the ring repeated\n",
				     ktime_us_delta(now, r->last_cb));
		lapped = true;	/* the residue cannot tell how many laps went by */
	}
	r->last_cb = now;
	playing = rso_playing_chunk(r, NULL);
	if (playing < 0)	/* no residue: one chunk per callback */
		playing = (r->next_fill + 1) % r->chunks;
	while (r->next_fill != playing) {
		rso_fill_chunk(r, r->next_fill, &period);
		if (++r->next_fill == r->chunks)
			r->next_fill = 0;
	}
	if (period || lapped)
		ss = r->ss;
	r->notifying = !!ss;
	spin_unlock_irqrestore(&r->lock, flags);

	if (ss) {
		if (lapped)
			snd_pcm_stop_xrun(ss);
		else
			snd_pcm_period_elapsed(ss);
		/* pairs with the acquire in close: everything above is finished */
		smp_store_release(&r->notifying, false);
	}
}

/* Clock and DMA, sleeping context, hw_lock held */
static void rso_hw_stop(struct rso *r)
{
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	if (!r->rate) {
		spin_unlock_irqrestore(&r->lock, flags);
		return;
	}
	r->rate = 0;
	spin_unlock_irqrestore(&r->lock, flags);

	writel(readl(r->base + CS_A) & ~CS_TXON, r->base + CS_A);
	dmaengine_terminate_sync(r->dma);
	writel(0, r->base + CS_A);
	clk_disable_unprepare(r->clk);
}

static int rso_hw_start(struct rso *r, unsigned int rate)
{
	struct dma_async_tx_descriptor *desc;
	bool dummy = false;
	unsigned long flags;
	unsigned int i;
	int ret;

	if (r->gone)
		return -ENODEV;
	if (READ_ONCE(r->rate) == rate)
		return 0;
	rso_hw_stop(r);

	ret = clk_set_rate(r->clk, (unsigned long)rate * 128);
	if (ret)
		return ret;
	ret = clk_prepare_enable(r->clk);
	if (ret)
		return ret;

	/* enable, leave standby; one 32-bit channel per 32-bit frame, clock master */
	writel(CS_EN, r->base + CS_A);
	writel(CS_EN | CS_STBY, r->base + CS_A);
	writel(MODE_FLEN(31) | MODE_FSLEN(1), r->base + MODE_A);
	writel(0, r->base + RXC_A);
	writel(TXC_CH1(CH_WEX | CH_EN | CH_WID(8)), r->base + TXC_A);
	writel(0, r->base + INTEN_A);
	writel(DREQ_TX_PANIC(0x10) | DREQ_TX(0x30), r->base + DREQ_A);
	writel(CS_EN | CS_STBY | CS_TXTHR(1) | CS_DMAEN | CS_TXCLR, r->base + CS_A);
	usleep_range(10, 20);	/* TXCLR needs two PCM clocks */

	/* nothing plays yet (prepare comes first), so this is the silence table */
	spin_lock_irqsave(&r->lock, flags);
	r->rate = rate;
	r->late_us = div_u64((u64)(r->chunks - 1) * r->chunk_frames * USEC_PER_SEC, rate);
	rso_update_status(r);
	spdif_bmc_reset(&r->enc);
	spin_unlock_irqrestore(&r->lock, flags);
	for (i = 0; i < r->chunks; i++) {	/* one chunk per hold of the lock */
		spin_lock_irqsave(&r->lock, flags);
		rso_fill_chunk(r, i, &dummy);
		spin_unlock_irqrestore(&r->lock, flags);
	}
	spin_lock_irqsave(&r->lock, flags);
	r->next_fill = 0;
	spin_unlock_irqrestore(&r->lock, flags);

	desc = dmaengine_prep_dma_cyclic(r->dma, r->ring_dma, r->ring_bytes, r->chunk_bytes,
					 DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc) {
		ret = -ENOMEM;
		goto fail;
	}
	desc->callback = rso_dma_callback;
	desc->callback_param = r;
	r->cookie = dmaengine_submit(desc);
	ret = dma_submit_error(r->cookie);
	if (ret)
		goto fail;
	spin_lock_irqsave(&r->lock, flags);
	r->last_cb = ktime_get();	/* the lateness clock starts with the DMA */
	spin_unlock_irqrestore(&r->lock, flags);
	dma_async_issue_pending(r->dma);
	writel(CS_EN | CS_STBY | CS_TXTHR(1) | CS_DMAEN | CS_TXON | CS_TXERR, r->base + CS_A);
	dev_dbg(r->dev, "running at %u Hz (bit clock %lu Hz)\n", rate, clk_get_rate(r->clk));
	return 0;

fail:
	WRITE_ONCE(r->rate, 0);
	writel(0, r->base + CS_A);
	clk_disable_unprepare(r->clk);
	return ret;
}

/* PCM */

static const struct snd_pcm_hardware rso_playback_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_BATCH | SNDRV_PCM_INFO_PAUSE,
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
	.rate_min = 32000,
	.rate_max = 96000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 512 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 128 * 1024,
	.periods_min = 2,
	.periods_max = 1024,
};

static int rso_open(struct snd_pcm_substream *ss)
{
	struct rso *r = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	int ret;

	rt->hw = rso_playback_hw;
	/* the ring takes whole chunks: periods and buffer must not be smaller */
	ret = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					   r->chunk_frames, UINT_MAX);
	if (ret < 0)
		return ret;
	/* twice the ring (periods_min is 2): a catch-up must not wrap the buffer unseen */
	ret = snd_pcm_hw_constraint_minmax(rt, SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
					   2 * r->chunks * r->chunk_frames, UINT_MAX);
	if (ret < 0)
		return ret;
	return snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
}

static int rso_close(struct snd_pcm_substream *ss)
{
	struct rso *r = snd_pcm_substream_chip(ss);
	unsigned int rate;
	unsigned long flags;
	bool stop;

	spin_lock_irqsave(&r->lock, flags);
	r->ss = NULL;
	r->active = false;
	stop = !r->keep_alive;
	rate = r->rate;
	spin_unlock_irqrestore(&r->lock, flags);
	/* the DMA keeps running with Keep Alive; ALSA frees the runtime after this returns */
	while (smp_load_acquire(&r->notifying))
		usleep_range(50, 100);
	if (!stop)
		return 0;

	if (rate)	/* let the ring play out what is still in it */
		msleep(DIV_ROUND_UP(r->chunks * r->chunk_frames * 1000, rate));
	/* no new prepare can come in: ALSA holds pcm->open_mutex across close */
	mutex_lock(&r->hw_lock);
	rso_hw_stop(r);
	mutex_unlock(&r->hw_lock);
	return 0;
}

static int rso_prepare(struct snd_pcm_substream *ss)
{
	struct rso *r = snd_pcm_substream_chip(ss);
	unsigned long flags;
	unsigned int rate;
	int ret;

	spin_lock_irqsave(&r->lock, flags);
	r->ss = ss;
	r->active = false;
	r->pos = 0;
	r->period_acc = 0;
	rate = ss->runtime->rate;
	rso_update_status(r);
	spin_unlock_irqrestore(&r->lock, flags);

	mutex_lock(&r->hw_lock);
	ret = rso_hw_start(r, rate);
	mutex_unlock(&r->hw_lock);
	if (ret) {
		spin_lock_irqsave(&r->lock, flags);
		r->ss = NULL;
		spin_unlock_irqrestore(&r->lock, flags);
	}
	return ret;
}

static int rso_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct rso *r = snd_pcm_substream_chip(ss);
	unsigned long flags;
	bool on;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		on = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		on = false;	/* the ring goes on with silence */
		break;
	default:
		return -EINVAL;
	}
	spin_lock_irqsave(&r->lock, flags);
	r->active = on;
	spin_unlock_irqrestore(&r->lock, flags);
	return 0;
}

static snd_pcm_uframes_t rso_pointer(struct snd_pcm_substream *ss)
{
	struct rso *r = snd_pcm_substream_chip(ss);
	snd_pcm_uframes_t pos;
	unsigned long flags;
	size_t offset = 0;
	int playing;

	spin_lock_irqsave(&r->lock, flags);
	pos = r->pos;
	/* encoded frames ahead of the output: chunks after the playing one, its rest, FIFO */
	playing = rso_playing_chunk(r, &offset);
	if (playing >= 0)
		ss->runtime->delay = ((r->next_fill + r->chunks - playing - 1) % r->chunks) *
				     r->chunk_frames + (r->chunk_bytes - offset) / FRAME_BYTES;
	else
		ss->runtime->delay = (r->chunks - 1) * r->chunk_frames;
	ss->runtime->delay += FIFO_FRAMES;
	spin_unlock_irqrestore(&r->lock, flags);
	return pos;
}

static const struct snd_pcm_ops rso_pcm_ops = {
	.open = rso_open,
	.close = rso_close,
	.prepare = rso_prepare,
	.trigger = rso_trigger,
	.pointer = rso_pointer,
};

/* Controls */

static int rso_iec958_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int rso_iec958_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	struct rso *r = snd_kcontrol_chip(k);
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	memcpy(v->value.iec958.status, r->status, sizeof(r->status));
	spin_unlock_irqrestore(&r->lock, flags);
	return 0;
}

static int rso_iec958_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	struct rso *r = snd_kcontrol_chip(k);
	unsigned long flags;
	int changed;

	if (v->value.iec958.status[0] & IEC958_AES0_PROFESSIONAL)
		return -EINVAL;	/* the encoder writes the consumer format */
	spin_lock_irqsave(&r->lock, flags);
	changed = memcmp(r->status, v->value.iec958.status, sizeof(r->status)) != 0;
	memcpy(r->status, v->value.iec958.status, sizeof(r->status));
	rso_update_status(r);
	spin_unlock_irqrestore(&r->lock, flags);
	return changed;
}

static int rso_iec958_mask_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	v->value.iec958.status[0] = IEC958_AES0_NONAUDIO | IEC958_AES0_CON_NOT_COPYRIGHT |
				    IEC958_AES0_CON_EMPHASIS;
	v->value.iec958.status[1] = 0xff;	/* category, original */
	v->value.iec958.status[2] = 0xff;	/* source and channel number */
	v->value.iec958.status[3] = IEC958_AES3_CON_FS | IEC958_AES3_CON_CLOCK;
	v->value.iec958.status[4] = IEC958_AES4_CON_MAX_WORDLEN_24 | IEC958_AES4_CON_WORDLEN;
	return 0;
}

static int rso_keep_alive_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	struct rso *r = snd_kcontrol_chip(k);

	v->value.integer.value[0] = READ_ONCE(r->keep_alive);
	return 0;
}

static int rso_keep_alive_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	struct rso *r = snd_kcontrol_chip(k);
	bool on = !!v->value.integer.value[0];

	if (on == READ_ONCE(r->keep_alive))
		return 0;
	WRITE_ONCE(r->keep_alive, on);	/* takes effect at the next close */
	return 1;
}

static const struct snd_kcontrol_new rso_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = SNDRV_CTL_NAME_IEC958("", PLAYBACK, DEFAULT),
		.info = rso_iec958_info,
		.get = rso_iec958_get,
		.put = rso_iec958_put,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ,
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = SNDRV_CTL_NAME_IEC958("", PLAYBACK, CON_MASK),
		.info = rso_iec958_info,
		.get = rso_iec958_mask_get,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Keep Alive Playback Switch",
		.info = snd_ctl_boolean_mono_info,
		.get = rso_keep_alive_get,
		.put = rso_keep_alive_put,
	},
};

/* Probe and remove */

static void rso_release_dma(void *dma)
{
	dma_release_channel(dma);
}

static void rso_free_ring(void *data)
{
	struct rso *r = data;

	dma_free_coherent(r->dma->device->dev, r->ring_bytes, r->ring, r->ring_dma);
}

static int rso_probe(struct platform_device *pdev)
{
	struct dma_slave_config cfg = {};
	struct device *dev = &pdev->dev;
	struct snd_card *card;
	struct resource *res;
	struct snd_pcm *pcm;
	unsigned int i;
	struct rso *r;
	int ret;

	if (chunk_frames < 64 || chunk_frames > 2048 || chunks < 3 || chunks > 32)
		return dev_err_probe(dev, -EINVAL, "chunk_frames 64..2048, chunks 3..32\n");

	ret = snd_devm_card_new(dev, index, "SPDIFout", THIS_MODULE, sizeof(*r), &card);
	if (ret)
		return ret;
	r = card->private_data;
	r->card = card;
	r->dev = dev;
	spin_lock_init(&r->lock);
	mutex_init(&r->hw_lock);
	r->chunk_frames = chunk_frames;
	r->chunks = chunks;
	r->chunk_bytes = (size_t)chunk_frames * FRAME_BYTES;
	r->ring_bytes = r->chunk_bytes * chunks;
	r->keep_alive = keep_alive;
	snd_pcm_create_iec958_consumer_default(r->status, sizeof(r->status));
	spdif_bmc_init(&r->enc);

	r->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(r->base))
		return PTR_ERR(r->base);
	/* translated resource address, as bcm2835-i2s does (not the raw DT bus address) */
	r->fifo_addr = res->start + FIFO_A;
	r->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(r->clk))
		return dev_err_probe(dev, PTR_ERR(r->clk), "no clock\n");

	r->dma = dma_request_chan(dev, "tx");
	if (IS_ERR(r->dma))
		return dev_err_probe(dev, PTR_ERR(r->dma), "no tx DMA channel\n");
	ret = devm_add_action_or_reset(dev, rso_release_dma, r->dma);
	if (ret)
		return ret;
	cfg.direction = DMA_MEM_TO_DEV;
	cfg.dst_addr = r->fifo_addr;
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_maxburst = 2;
	ret = dmaengine_slave_config(r->dma, &cfg);
	if (ret)
		return ret;
	r->ring = dma_alloc_coherent(r->dma->device->dev, r->ring_bytes, &r->ring_dma,
				     GFP_KERNEL);
	if (!r->ring)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, rso_free_ring, r);
	if (ret)
		return ret;

	strscpy(card->driver, "RPiGPIOSPDIF", sizeof(card->driver));
	strscpy(card->shortname, "RPi S/PDIF out", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "RPi S/PDIF out (GPIO, %u x %u frames)",
		 chunks, chunk_frames);

	ret = snd_pcm_new(card, "S/PDIF", 0, 1, 0, &pcm);
	if (ret)
		return ret;
	pcm->private_data = r;
	strscpy(pcm->name, "S/PDIF out", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &rso_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL, 0, 0);

	for (i = 0; i < ARRAY_SIZE(rso_controls); i++) {
		ret = snd_ctl_add(card, snd_ctl_new1(&rso_controls[i], r));
		if (ret)
			return ret;
	}
	ret = snd_card_register(card);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, r);
	dev_info(dev, "card %d, %u chunks x %u frames\n", card->number, chunks, chunk_frames);
	return 0;
}

static void rso_remove(struct platform_device *pdev)
{
	struct rso *r = platform_get_drvdata(pdev);

	/* no new prepare from here on; the card itself is freed by devres */
	snd_card_disconnect(r->card);
	mutex_lock(&r->hw_lock);
	r->gone = true;
	rso_hw_stop(r);
	mutex_unlock(&r->hw_lock);
}

static const struct of_device_id rso_of_match[] = {
	{ .compatible = "ehb,rpi-gpio-spdif" },
	{ }
};
MODULE_DEVICE_TABLE(of, rso_of_match);

static struct platform_driver rso_driver = {
	.probe = rso_probe,
	.remove = rso_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = rso_of_match,
	},
};
module_platform_driver(rso_driver);

MODULE_AUTHOR("ehb");
MODULE_DESCRIPTION("S/PDIF output on a Raspberry Pi GPIO via the PCM/I2S block");
MODULE_LICENSE("GPL");
