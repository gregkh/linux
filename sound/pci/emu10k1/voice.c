/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *                   Creative Labs, Inc.
 *  Routines for control of EMU10K1 chips - voice manager
 *
 *  BUGS:
 *    --
 *
 *  TODO:
 *    --
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <sound/driver.h>
#include <linux/time.h>
#include <sound/core.h>
#include <sound/emu10k1.h>

static int voice_alloc(emu10k1_t *emu, emu10k1_voice_type_t type, int pair, emu10k1_voice_t **rvoice)
{
	emu10k1_voice_t *voice, *voice2;
	int idx;

	*rvoice = NULL;
	for (idx = 0; idx < 64; idx += pair ? 2 : 1) {
		voice = &emu->voices[idx];
		voice2 = pair ? &emu->voices[idx+1] : NULL;
		if (voice->use || (voice2 && voice2->use))
			continue;
		voice->use = 1;
		if (voice2)
			voice2->use = 1;
		switch (type) {
		case EMU10K1_PCM:
			voice->pcm = 1;
			if (voice2)
				voice2->pcm = 1;
			break;
		case EMU10K1_SYNTH:
			voice->synth = 1;
			break;
		case EMU10K1_MIDI:
			voice->midi = 1;
			break;
		}
		// printk("voice alloc - %i, pair = %i\n", voice->number, pair);
		*rvoice = voice;
		return 0;
	}
	return -ENOMEM;
}

int snd_emu10k1_voice_alloc(emu10k1_t *emu, emu10k1_voice_type_t type, int pair, emu10k1_voice_t **rvoice)
{
	unsigned long flags;
	int result;

	snd_assert(rvoice != NULL, return -EINVAL);
	snd_assert(!pair || type == EMU10K1_PCM, return -EINVAL);

	spin_lock_irqsave(&emu->voice_lock, flags);
	for (;;) {
		result = voice_alloc(emu, type, pair, rvoice);
		if (result == 0 || type != EMU10K1_PCM)
			break;

		/* free a voice from synth */
		if (emu->get_synth_voice) {
			result = emu->get_synth_voice(emu);
			if (result >= 0) {
				emu10k1_voice_t *pvoice = &emu->voices[result];
				pvoice->interrupt = NULL;
				pvoice->use = pvoice->pcm = pvoice->synth = pvoice->midi = 0;
				pvoice->epcm = NULL;
			}
		}
		if (result < 0)
			break;
	}
	spin_unlock_irqrestore(&emu->voice_lock, flags);

	return result;
}

int snd_emu10k1_voice_free(emu10k1_t *emu, emu10k1_voice_t *pvoice)
{
	unsigned long flags;

	snd_assert(pvoice != NULL, return -EINVAL);
	spin_lock_irqsave(&emu->voice_lock, flags);
	pvoice->interrupt = NULL;
	pvoice->use = pvoice->pcm = pvoice->synth = pvoice->midi = 0;
	pvoice->epcm = NULL;
	snd_emu10k1_voice_init(emu, pvoice->number);
	spin_unlock_irqrestore(&emu->voice_lock, flags);
	return 0;
}
