// SPDX-License-Identifier: GPL-2.0-only
/* LD_PRELOAD shim for test_plugin.py: the slave "sink" takes one frame less than offered; with
   SHORT_EAGAIN set it also refuses every third write outright. */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t n)
{
	static snd_pcm_sframes_t (*real)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
	static unsigned int calls;
	const char *name = snd_pcm_name(pcm);

	if (!real)
		real = (snd_pcm_sframes_t (*)(snd_pcm_t *, const void *, snd_pcm_uframes_t))dlsym(RTLD_NEXT, "snd_pcm_writei");
	if (name && !strcmp(name, "sink") && n > 3) {
		if (getenv("SHORT_EAGAIN") && ++calls % 3 == 0)
			return -EAGAIN;
		n--;
	}
	return real(pcm, buf, n);
}
