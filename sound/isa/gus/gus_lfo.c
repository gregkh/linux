/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *  Routines for control of LFO generators (tremolo & vibrato) for
 *  GF1/InterWave chips...
 *
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
#include <sound/core.h>
#include <sound/gus.h>

/*
 *  called by engine routines
 */

static signed char snd_gf1_lfo_compute_value(snd_gus_card_t * gus,
					     unsigned char *ptr)
{
	unsigned int twaveinc, depth_delta;
	signed int result;
	unsigned short control, twave, depth, depth_final;
	unsigned char *ptr1;

	control = *(unsigned short *) (ptr + 0x00);
	ptr1 = ptr + ((control & 0x4000) >> 12);
	/* 1. add TWAVEINC to TWAVE and write the result back */
	/* LFO update rate is 689Hz, effect timer is in ms */
	if (gus->gf1.timer_slave)
		twaveinc = (689 * gus->gf1.timer_master_gus->gf1.effect_timer) / 1000;
	else
		twaveinc = (689 * gus->gf1.effect_timer) / 1000;
	if (!twaveinc)
		twaveinc++;
#if 0
	printk("twaveinc = 0x%x, effect_timer = %i\n", twaveinc, gus->gf1.effect_timer);
#endif

	depth = *(unsigned short *) (ptr1 + 0x0a);
	depth_final = *(unsigned char *) (ptr + 0x02) << 5;
	if (depth != depth_final) {
		depth_delta = ((twaveinc * *(ptr + 0x03)) + *(unsigned short *) (ptr + 0x04));
		*(unsigned short *) (ptr + 0x04) = depth_delta % 8000;
		depth_delta /= 8000;
		if (depth < depth_final) {
			if (depth + depth_delta > depth_final)
				depth = depth_final;
			else
				depth += depth_delta;
		}
		if (depth > depth_final) {
			if (depth - depth_delta < depth_final)
				depth = depth_final;
			else
				depth -= depth_delta;
		}
		*(unsigned short *) (ptr1 + 0x0a) = depth;
	}
	twaveinc *= (unsigned int) control & 0x7ff;
	twaveinc += *(unsigned short *) (ptr + 0x06);
	*(unsigned short *) (ptr + 0x06) = twaveinc % 1000;

	twave = *(unsigned short *) (ptr1 + 0x08);
	twave += (unsigned short) (twaveinc / (unsigned int) 1000);
	*(unsigned short *) (ptr1 + 0x08) = twave;

	if (!(control & 0x2000)) {
		/* 2. if shift is low */
		if (twave & 0x4000) {	/* bit 14 high -> invert TWAVE 13-0 */
			twave ^= 0x3fff;
			twave &= ~0x4000;
		}
		/* TWAVE bit 15 is exclusive or'd with the invert bit (12) */
		twave ^= (control & 0x1000) << 3;
	} else {
		/* 2. if shift is high */
		if (twave & 0x8000)	/* bit 15 high -> invert TWAVE 14-0 */
			twave ^= 0x7fff;
		/* the invert bit (12) is used as sign bit */
		if (control & 0x1000)
			twave |= 0x8000;
		else
			twave &= ~0x8000;
	}
	/* 3. multiply the 14-bit LFO waveform magnitude by 13-bit DEPTH */
#if 0
	printk("c=0x%x,tw=0x%x,to=0x%x,d=0x%x,df=0x%x,di=0x%x,r=0x%x,r1=%i\n",
	       control, twave,
	       *(unsigned short *) (ptr1 + 0x08),
	       depth, depth_final, *(ptr + 0x03),
	     (twave & 0x7fff) * depth, ((twave & 0x7fff) * depth) >> 21);
#endif
	result = (twave & 0x7fff) * depth;
	if (result) {
		/* shift */
		result >>= 21;
		result &= 0x3f;
	}
	/* add sign */
	if (twave & 0x8000)
		result = -result;
#if 0
	printk("lfo final value = %i\n", result);
#endif
	return result;
}

static void snd_gf1_lfo_register_setup(snd_gus_card_t * gus,
				       snd_gf1_voice_t * voice,
				       int lfo_type)
{
	unsigned long flags;

	if (gus->gf1.enh_mode) {
		CLI(&flags);
		gf1_select_voice(gus, voice->number);
		if (lfo_type & 1) {
			snd_gf1_write8(gus, GF1_VB_FREQUENCY_LFO, voice->lfo_fc);
			voice->lfo_fc = 0;
		}
		if (lfo_type & 2) {
			snd_gf1_write8(gus, GF1_VB_VOLUME_LFO, voice->lfo_volume);
			voice->lfo_volume = 0;
		}
		STI(&flags);
	} else {
		/*
		 * ok.. with old GF1 chip can be only vibrato emulated...
		 * volume register can be in volume ramp state, so tremolo isn't simple..
		 */
		if (!(lfo_type & 1))
			return;
#if 0
		if (voice->lfo_fc)
			printk("setup - %i = %i\n", voice->number, voice->lfo_fc);
#endif
		CLI(&flags);
		gf1_select_voice(gus, voice->number);
		snd_gf1_write16(gus, GF1_VW_FREQUENCY, voice->fc_register + voice->lfo_fc);
		STI(&flags);
	}
}

void snd_gf1_lfo_effect_interrupt(snd_gus_card_t * gus, snd_gf1_voice_t * voice)
{
	unsigned char *ptr;

#if 0
	if (voice->number != 0)
		return;
#endif
	ptr = gus->gf1.lfos + ((voice->number) << 5);
	/* 1. vibrato */
	if (*(unsigned short *) (ptr + 0x00) & 0x8000)
		voice->lfo_fc = snd_gf1_lfo_compute_value(gus, ptr);
	/* 2. tremolo */
	ptr += 16;
	if (*(unsigned short *) (ptr + 0x00) & 0x8000)
		voice->lfo_volume = snd_gf1_lfo_compute_value(gus, ptr);
	/* 3. register setup */
	snd_gf1_lfo_register_setup(gus, voice, 3);
}

/*

 */

void snd_gf1_lfo_init(snd_gus_card_t * gus)
{
	if (gus->gf1.hw_lfo) {
		snd_gf1_i_write16(gus, GF1_GW_LFO_BASE, 0x0000);
		snd_gf1_dram_setmem(gus, 0, 0x0000, 1024);
		/* now enable LFO */
		snd_gf1_i_write8(gus, GF1_GB_GLOBAL_MODE, snd_gf1_i_look8(gus, GF1_GB_GLOBAL_MODE) | 0x02);
	}
	if (gus->gf1.sw_lfo) {
#if 1
		gus->gf1.lfos = snd_calloc(1024);
		if (!gus->gf1.lfos)
#endif
			gus->gf1.sw_lfo = 0;
	}
}

void snd_gf1_lfo_done(snd_gus_card_t * gus)
{
	if (gus->gf1.sw_lfo) {
		if (gus->gf1.lfos) {
			snd_gf1_free(gus->gf1.lfos, 1024);
			gus->gf1.lfos = NULL;
		}
	}
}

void snd_gf1_lfo_program(snd_gus_card_t * gus, int voice, int lfo_type,
                         struct ULTRA_STRU_IW_LFO_PROGRAM *program)
{
	unsigned int lfo_addr, wave_select;

	wave_select = (program->freq_and_control & 0x4000) >> 12;
	lfo_addr = (voice << 5) | (lfo_type << 4);
	if (gus->gf1.hw_lfo) {
#if 0
		printk("LMCI = 0x%x\n", snd_gf1_i_look8(gus, 0x53));
		printk("lfo_program: lfo_addr=0x%x,wave_sel=0x%x,fac=0x%x,df=0x%x,di=0x%x,twave=0x%x,depth=0x%x\n",
		       lfo_addr, wave_select,
		       program->freq_and_control,
		       program->depth_final,
		       program->depth_inc,
		       program->twave,
		       program->depth);
#endif
		snd_gf1_poke(gus, lfo_addr + 0x02, program->depth_final);
		snd_gf1_poke(gus, lfo_addr + 0x03, program->depth_inc);
		snd_gf1_pokew(gus, lfo_addr + 0x08 + wave_select, program->twave);
		snd_gf1_pokew(gus, lfo_addr + 0x0a + wave_select, program->depth);
		snd_gf1_pokew(gus, lfo_addr + 0x00, program->freq_and_control);
#if 0
		{
			int i = 0;
			for (i = 0; i < 16; i++)
				printk("%02x:", snd_gf1_peek(gus, lfo_addr + i));
			printk("\n");
		}
#endif
	}
	if (gus->gf1.sw_lfo) {
		unsigned char *ptr = gus->gf1.lfos + lfo_addr;

		*(ptr + 0x02) = program->depth_final;
		*(ptr + 0x03) = program->depth_inc;
		*(unsigned short *) (ptr + 0x08 + wave_select) = program->twave;
		*(unsigned short *) (ptr + 0x0a + wave_select) = program->depth;
		*(unsigned short *) (ptr + 0x00) = program->freq_and_control;
	}
}

void snd_gf1_lfo_enable(snd_gus_card_t * gus, int voice, int lfo_type)
{
	unsigned int lfo_addr;

	lfo_addr = (voice << 5) | (lfo_type << 4);
	if (gus->gf1.hw_lfo)
		snd_gf1_pokew(gus, lfo_addr + 0x00, snd_gf1_peekw(gus, lfo_addr + 0x00) | 0x8000);
	if (gus->gf1.sw_lfo) {
		unsigned char *ptr = gus->gf1.lfos + lfo_addr;

		*(unsigned short *) (ptr + 0x00) |= 0x8000;
	}
}

void snd_gf1_lfo_disable(snd_gus_card_t * gus, int voice, int lfo_type)
{
	unsigned int lfo_addr;

	lfo_addr = (voice << 5) | (lfo_type << 4);
	if (gus->gf1.hw_lfo)
		snd_gf1_pokew(gus, lfo_addr + 0x00,
				snd_gf1_peekw(gus, lfo_addr + 0x00) & ~0x8000);
	if (gus->gf1.sw_lfo) {
		unsigned char *ptr = gus->gf1.lfos + lfo_addr;

		*(unsigned short *) (ptr + 0x00) &= ~0x8000;
	}
}

void snd_gf1_lfo_change_freq(snd_gus_card_t * gus, int voice,
		             int lfo_type, int freq)
{
	unsigned int lfo_addr;

	lfo_addr = (voice << 5) | (lfo_type << 4);
	if (gus->gf1.hw_lfo)
		snd_gf1_pokew(gus, lfo_addr + 0x00,
				(snd_gf1_peekw(gus, lfo_addr + 0x00) & ~0x7ff) | (freq & 0x7ff));
	if (gus->gf1.sw_lfo) {
		unsigned long flags;
		unsigned char *ptr = gus->gf1.lfos + lfo_addr;

		CLI(&flags);
		*(unsigned short *) (ptr + 0x00) &= ~0x7ff;
		*(unsigned short *) (ptr + 0x00) |= freq & 0x7ff;
		STI(&flags);
	}
}

void snd_gf1_lfo_change_depth(snd_gus_card_t * gus, int voice,
			      int lfo_type, int depth)
{
	unsigned long flags;
	unsigned int lfo_addr;
	unsigned short control = 0;
	unsigned char *ptr;

	lfo_addr = (voice << 5) | (lfo_type << 4);
	ptr = gus->gf1.lfos + lfo_addr;
	if (gus->gf1.hw_lfo)
		control = snd_gf1_peekw(gus, lfo_addr + 0x00);
	if (gus->gf1.sw_lfo)
		control = *(unsigned short *) (ptr + 0x00);
	if (depth < 0) {
		control |= 0x1000;
		depth = -depth;
	} else
		control &= ~0x1000;
	if (gus->gf1.hw_lfo) {
		CLI(&flags);
		snd_gf1_poke(gus, lfo_addr + 0x02, (unsigned char) depth);
		snd_gf1_pokew(gus, lfo_addr + 0x0a + ((control & 0x4000) >> 12), depth << 5);
		snd_gf1_pokew(gus, lfo_addr + 0x00, control);
		STI(&flags);
	}
	if (gus->gf1.sw_lfo) {
		unsigned char *ptr = gus->gf1.lfos + lfo_addr;

		CLI(&flags);
		*(ptr + 0x02) = (unsigned char) depth;
		*(unsigned short *) (ptr + 0x0a + ((control & 0x4000) >> 12)) = depth << 5;
		*(unsigned short *) (ptr + 0x00) = control;
		STI(&flags);
	}
}

void snd_gf1_lfo_setup(snd_gus_card_t * gus, int voice, int lfo_type,
		       int freq, int current_depth, int depth, int sweep,
		       int shape)
{
	struct ULTRA_STRU_IW_LFO_PROGRAM program;

	program.freq_and_control = 0x8000 | (freq & 0x7ff);
	if (shape & ULTRA_STRU_IW_LFO_SHAPE_POSTRIANGLE)
		program.freq_and_control |= 0x2000;
	if (depth < 0) {
		program.freq_and_control |= 0x1000;
		depth = -depth;
	}
	program.twave = 0;
	program.depth = current_depth;
	program.depth_final = depth;
	if (sweep) {
		program.depth_inc = (unsigned char) (((int) ((depth << 5) - current_depth) << 9) / (sweep * 4410L));
		if (!program.depth_inc)
			program.depth_inc++;
	} else
		program.depth = (unsigned short) (depth << 5);
	snd_gf1_lfo_program(gus, voice, lfo_type, &program);
}

void snd_gf1_lfo_shutdown(snd_gus_card_t * gus, int voice, int lfo_type)
{
	unsigned long flags;
	unsigned int lfo_addr;

	lfo_addr = (voice << 5) | (lfo_type << 4);
	if (gus->gf1.hw_lfo) {
		snd_gf1_pokew(gus, lfo_addr + 0x00, 0x0000);
		CLI(&flags);
		gf1_select_voice(gus, voice);
		snd_gf1_write8(gus, lfo_type == ULTRA_LFO_VIBRATO ? GF1_VB_FREQUENCY_LFO : GF1_VB_VOLUME_LFO, 0);
		STI(&flags);
	}
	if (gus->gf1.sw_lfo) {
		unsigned char *ptr = gus->gf1.lfos + lfo_addr;
		snd_gf1_voice_t *pvoice;

		*(unsigned short *) (ptr + 0x00) = 0;
		*(unsigned short *) (ptr + 0x04) = 0;
		*(unsigned short *) (ptr + 0x06) = 0;
		if (gus->gf1.syn_voices) {
			pvoice = gus->gf1.syn_voices + voice;
			if (lfo_type == ULTRA_LFO_VIBRATO)
				pvoice->lfo_fc = 0;
			else
				pvoice->lfo_volume = 0;
			snd_gf1_lfo_register_setup(gus, pvoice, lfo_type == ULTRA_LFO_VIBRATO ? 1 : 2);
		} else if (gus->gf1.enh_mode) {
			CLI(&flags);
			gf1_select_voice(gus, voice);
			snd_gf1_write8(gus, lfo_type == ULTRA_LFO_VIBRATO ? GF1_VB_FREQUENCY_LFO : GF1_VB_VOLUME_LFO, 0);
			STI(&flags);
		}
	}
}

void snd_gf1_lfo_command(snd_gus_card_t * gus, int voice, unsigned char *data)
{
	int lfo_type;
	int lfo_command;
	int temp1, temp2;

	lfo_type = *data >> 7;
	lfo_command = *data & 0x7f;
	switch (lfo_command) {
	case ULTRA_LFO_SETUP:	/* setup */
		temp1 = snd_gf1_get_word(data, 2);
		temp2 = snd_gf1_get_word(data, 4);
		snd_gf1_lfo_setup(gus, voice, lfo_type, temp1 & 0x7ff, 0, temp2 > 255 ? 255 : temp2, snd_gf1_get_byte(data, 1), (temp1 & 0x2000) >> 13);
		break;
	case ULTRA_LFO_FREQ:	/* freq */
		snd_gf1_lfo_change_depth(gus, voice, lfo_type, snd_gf1_get_word(data, 2));
		break;
	case ULTRA_LFO_DEPTH:	/* depth */
		snd_gf1_lfo_change_freq(gus, voice, lfo_type, snd_gf1_get_word(data, 2));
		break;
	case ULTRA_LFO_ENABLE:	/* enable */
		snd_gf1_lfo_enable(gus, voice, lfo_type);
		break;
	case ULTRA_LFO_DISABLE:	/* disable */
		snd_gf1_lfo_disable(gus, voice, lfo_type);
		break;
	case ULTRA_LFO_SHUTDOWN:	/* shutdown */
		snd_gf1_lfo_shutdown(gus, voice, lfo_type);
		break;
	}
}
