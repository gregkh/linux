/*
 *  Harmony chipset driver
 *
 *	This is a sound driver for ASP's and Lasi's Harmony sound chip
 *	and is unlikely to be used for anything other than on a HP PA-RISC.
 *
 *	Harmony is found in HP 712s, 715/new and many other GSC based machines.
 *	On older 715 machines you'll find the technically identical chip 
 *	called 'Vivace'. Both Harmony and Vivace are supported by this driver.
 *
 *  this ALSA driver is based on OSS driver by:
 *	Copyright 2000 (c) Linuxcare Canada, Alex deVries <alex@linuxcare.com>
 *	Copyright 2000-2002 (c) Helge Deller <deller@gmx.de>
 *	Copyright 2001 (c) Matthieu Delahaye <delahaym@esiee.fr>
 *
 * TODO:
 * - use generic DMA interface and ioremap()/iounmap()
 * - capture is still untested (and probaby non-working)
 * - spin locks
 * - implement non-consistent DMA pages
 * - implement gain meter
 * - module parameters
 * - correct cleaning sequence
 * - better error checking
 * - try to have a better quality.
 *   
 */

/*
 * Harmony chipset 'modus operandi'.
 * - This chipset is found in some HP 32bit workstations, like 712, or B132 class.
 * most of controls are done through registers. Register are found at a fixed offset
 * from the hard physical adress, given in struct dev by register_parisc_driver.
 *
 * Playback and recording use 4kb pages (dma or not, depending on the machine).
 *
 * Most of PCM playback & capture is done through interrupt. When harmony needs
 * a new buffer to put recorded data or read played PCM, it sends an interrupt.
 * Bits 2 and 10 of DSTATUS register are '1' when harmony needs respectively
 * a new page for recording and playing. 
 * Interrupt are disabled/enabled by writing to bit 32 of DSTATUS. 
 * Adresses of next page to be played is put in PNXTADD register, next page
 * to be recorded is put in RNXTADD. There is 2 read-only registers, PCURADD and 
 * RCURADD that provides adress of current page.
 * 
 * Harmony has no way to control full duplex or half duplex mode. It means
 * that we always need to provide adresses of playback and capture data, even
 * when this is not needed. That's why we statically alloc one graveyard
 * buffer (to put recorded data in play-only mode) and a silence buffer.
 * 
 * Bitrate, number of channels and data format are controlled with
 * the CNTL register.
 *
 * Mixer work is done through one register (GAINCTL). Only input gain,
 * output attenuation and general attenuation control is provided. There is
 * also controls for enabling/disabling internal speaker and line
 * input.
 *
 * Buffers used by this driver are all DMA consistent.
 */

#include <linux/delay.h>
#include <sound/driver.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>
#include <sound/info.h>
#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/parisc-device.h>

MODULE_AUTHOR("Laurent Canet <canetl@esiee.fr>");
MODULE_DESCRIPTION("ALSA Harmony sound driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{ALSA,Harmony soundcard}}");

#undef DEBUG
#ifdef DEBUG
# define DPRINTK printk 
#else
# define DPRINTK(x,...)
#endif

#define PFX	"harmony: "

#define MAX_PCM_DEVICES		1
#define MAX_PCM_SUBSTREAMS	4
#define MAX_MIDI_DEVICES	0

#define HARMONY_BUF_SIZE	4096
#define MAX_BUFS		10
#define MAX_BUFFER_SIZE		(MAX_BUFS * HARMONY_BUF_SIZE)

/* number of silence & graveyard buffers */
#define GRAVEYARD_BUFS		3
#define SILENCE_BUFS		3

#define HARMONY_CNTL_C		0x80000000

#define HARMONY_DSTATUS_PN	0x00000200
#define HARMONY_DSTATUS_RN	0x00000002
#define HARMONY_DSTATUS_IE	0x80000000

#define HARMONY_DF_16BIT_LINEAR	0x00000000
#define HARMONY_DF_8BIT_ULAW	0x00000001
#define HARMONY_DF_8BIT_ALAW	0x00000002

#define HARMONY_SS_MONO		0x00000000
#define HARMONY_SS_STEREO	0x00000001

/*
 * Channels Mask in mixer register
 * try some "reasonable" default gain values
 */

#define HARMONY_GAIN_TOTAL_SILENCE 0x00F00FFF

/* the following should be enough (mixer is 
 * very sensible on harmony)
 */
#define HARMONY_GAIN_DEFAULT       0x0F2FF082


/* useless since only one card is supported ATM */
static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for Harmony device.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for Harmony device.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable Harmony device.");

/* Register offset (from base hpa) */
#define REG_ID		0x00
#define REG_RESET	0x04
#define REG_CNTL	0x08
#define REG_GAINCTL	0x0C
#define REG_PNXTADD	0x10
#define REG_PCURADD	0x14
#define REG_RNXTADD	0x18
#define REG_RCURADD	0x1C
#define REG_DSTATUS	0x20
#define REG_OV		0x24
#define REG_PIO		0x28
#define REG_DIAG	0x3C

/*
 * main harmony structure
 */

typedef struct snd_card_harmony {

	/* spinlocks (To be done) */
	spinlock_t mixer_lock;
	spinlock_t control_lock;

	/* parameters */	
	int irq;
	unsigned long hpa;
	int id;
	int rev;
	
	u32 current_gain;
	int data_format;		/* HARMONY_DF_xx_BIT_xxx */
	int sample_rate;		/* HARMONY_SR_xx_KHZ */
	int stereo_select;	/* HARMONY_SS_MONO or HARMONY_SS_STEREO */
	int format_initialized;
	
	unsigned long ply_buffer;
	int ply_buf;
	int ply_count;
	int ply_size;
	int ply_stopped;
	int ply_total;
	
	unsigned long cap_buffer;
	int cap_buf;
	int cap_count;
	int cap_size;
	int cap_stopped;
	int cap_total;

	struct parisc_device *pa_dev;

	struct snd_dma_device dma_dev;

	/* the graveyard buffer is used as recording buffer when playback, 
	 * because harmony always want a buffer to put recorded data */
	struct snd_dma_buffer graveyard_dma;
	int graveyard_count;
	
	/* same thing for silence buffer */
	struct snd_dma_buffer silence_dma;
	int silence_count;

	/* alsa stuff */
	snd_card_t *card;
	snd_pcm_t *pcm;
	snd_pcm_substream_t *playback_substream;
	snd_pcm_substream_t *capture_substream;
	snd_info_entry_t *proc_entry;
} snd_card_harmony_t;

static snd_card_t *snd_harmony_cards[SNDRV_CARDS] = SNDRV_DEFAULT_PTR;

/* wait to be out of control mode */
static inline void snd_harmony_wait_cntl(snd_card_harmony_t *harmony)
{
	int timeout = 5000;

	while ( (gsc_readl(harmony->hpa+REG_CNTL) & HARMONY_CNTL_C) && --timeout)
	{
		/* Wait */ ;	
	}
	if (timeout == 0) DPRINTK(KERN_DEBUG PFX "Error: wait cntl timeouted\n");
}


/*
 * sample rate routines 
 */
static unsigned int snd_card_harmony_rates[] = {
	5125, 6615, 8000, 9600,
	11025, 16000, 18900, 22050,
	27428, 32000, 33075, 37800,
	44100, 48000
};

static snd_pcm_hw_constraint_list_t hw_constraint_rates = {
	.count = ARRAY_SIZE(snd_card_harmony_rates),
	.list = snd_card_harmony_rates,
	.mask = 0,
};

#define HARMONY_SR_8KHZ		0x08
#define HARMONY_SR_16KHZ	0x09
#define HARMONY_SR_27KHZ	0x0A
#define HARMONY_SR_32KHZ	0x0B
#define HARMONY_SR_48KHZ	0x0E
#define HARMONY_SR_9KHZ		0x0F
#define HARMONY_SR_5KHZ		0x10
#define HARMONY_SR_11KHZ	0x11
#define HARMONY_SR_18KHZ	0x12
#define HARMONY_SR_22KHZ	0x13
#define HARMONY_SR_37KHZ	0x14
#define HARMONY_SR_44KHZ	0x15
#define HARMONY_SR_33KHZ	0x16
#define HARMONY_SR_6KHZ		0x17

/* bits corresponding to the entries of snd_card_harmony_rates */
static unsigned int rate_bits[14] = {
	HARMONY_SR_5KHZ, HARMONY_SR_6KHZ, HARMONY_SR_8KHZ,
	HARMONY_SR_9KHZ, HARMONY_SR_11KHZ, HARMONY_SR_16KHZ,
	HARMONY_SR_18KHZ, HARMONY_SR_22KHZ, HARMONY_SR_27KHZ,
	HARMONY_SR_32KHZ, HARMONY_SR_33KHZ, HARMONY_SR_37KHZ,
	HARMONY_SR_44KHZ, HARMONY_SR_48KHZ
};

/* snd_card_harmony_rate_bits
 * @rate:	index of current data rate in list
 * returns: harmony hex code for registers
 */
static unsigned int snd_card_harmony_rate_bits(int rate)
{
	unsigned int idx;
	
	for (idx = 0; idx < ARRAY_SIZE(snd_card_harmony_rates); idx++)
		if (snd_card_harmony_rates[idx] == rate)
			return rate_bits[idx];
	return HARMONY_SR_44KHZ; /* fallback */
}

/*
 * update controls (data format, sample rate, number of channels)
 * according to value supplied in data structure
 */
void snd_harmony_update_control(snd_card_harmony_t *harmony) 
{
	u32 default_cntl;
	
	/* Set CNTL */
	default_cntl = (HARMONY_CNTL_C |  	/* The C bit */
		(harmony->data_format << 6) |	/* Set the data format */
		(harmony->stereo_select << 5) |	/* Stereo select */
		(harmony->sample_rate));		/* Set sample rate */
	
	/* initialize CNTL */
 	snd_harmony_wait_cntl(harmony);
	
	gsc_writel(default_cntl, harmony->hpa+REG_CNTL);
	
}

/*
 * interruption controls routines
 */

static void snd_harmony_disable_interrupts(snd_card_harmony_t *chip) 
{
 	snd_harmony_wait_cntl(chip);
	gsc_writel(0, chip->hpa+REG_DSTATUS); 
}

static void snd_harmony_enable_interrupts(snd_card_harmony_t *chip) 
{
 	snd_harmony_wait_cntl(chip);
	gsc_writel(HARMONY_DSTATUS_IE, chip->hpa+REG_DSTATUS); 
}

/*
 * interruption routine:
 * The interrupt routine must provide adresse of next physical pages 
 * used by harmony
 */
static int snd_card_harmony_interrupt(int irq, void *dev, struct pt_regs *regs)
{
	snd_card_harmony_t *harmony = (snd_card_harmony_t *)dev;
	u32 dstatus = 0;
	unsigned long hpa = harmony->hpa;
	
	/* Turn off interrupts */
	snd_harmony_disable_interrupts(harmony);
	
	/* wait for control to free */
 	snd_harmony_wait_cntl(harmony);
	
	/* Read dstatus and pcuradd (the current address) */
	dstatus = gsc_readl(hpa+REG_DSTATUS);
	
	/* Check if this is a request to get the next play buffer */
	if (dstatus & HARMONY_DSTATUS_PN) {
		if (harmony->playback_substream) {
			harmony->ply_buf += harmony->ply_count;
			harmony->ply_buf %= harmony->ply_size;
		
			gsc_writel(harmony->ply_buffer + harmony->ply_buf,
					hpa+REG_PNXTADD);
		
			snd_pcm_period_elapsed(harmony->playback_substream);
			harmony->ply_total++;
		} else {
			gsc_writel(harmony->silence_dma.addr + 
				   (HARMONY_BUF_SIZE*harmony->silence_count),
				   hpa+REG_PNXTADD);
			harmony->silence_count++;
			harmony->silence_count %= SILENCE_BUFS;
		}
	}
	
	/* Check if we're being asked to fill in a recording buffer */
	if (dstatus & HARMONY_DSTATUS_RN) {
		if (harmony->capture_substream) {
			harmony->cap_buf += harmony->cap_count;
			harmony->cap_buf %= harmony->cap_size;
		
			gsc_writel(harmony->cap_buffer + harmony->cap_buf,
					hpa+REG_RNXTADD);
		
			snd_pcm_period_elapsed(harmony->capture_substream);
			harmony->cap_total++;
		} else {
			/* graveyard buffer */
			gsc_writel(harmony->graveyard_dma.addr +
				   (HARMONY_BUF_SIZE*harmony->graveyard_count),
				   hpa+REG_RNXTADD);
			harmony->graveyard_count++;
			harmony->graveyard_count %= GRAVEYARD_BUFS;
		}
	}
	snd_harmony_enable_interrupts(harmony);

	return IRQ_HANDLED;
}

/* 
 * proc entry
 * this proc file will give some debugging info
 */

static void snd_harmony_proc_read(snd_info_entry_t *entry, snd_info_buffer_t *buffer)
{
	snd_card_harmony_t *harmony = (snd_card_harmony_t *)entry->private_data;

	snd_iprintf(buffer, "LASI Harmony driver\nLaurent Canet <canetl@esiee.fr>\n\n");
	snd_iprintf(buffer, "IRQ %d, hpa %lx, id %d rev %d\n",
			harmony->irq, harmony->hpa,
			harmony->id, harmony->rev);
	snd_iprintf(buffer, "Current gain %lx\n", (unsigned long) harmony->current_gain);
	snd_iprintf(buffer, "\tsample rate=%d\n", harmony->sample_rate);
	snd_iprintf(buffer, "\tstereo select=%d\n", harmony->stereo_select);
	snd_iprintf(buffer, "\tbitperchan=%d\n\n", harmony->data_format);
	
	snd_iprintf(buffer, "Play status:\n");
	snd_iprintf(buffer, "\tstopped %d\n", harmony->ply_stopped);
	snd_iprintf(buffer, "\tbuffer %lx, count %d\n", harmony->ply_buffer, harmony->ply_count);
	snd_iprintf(buffer, "\tbuf %d size %d\n\n", harmony->ply_buf, harmony->ply_size);
	
	snd_iprintf(buffer, "Capture status:\n");
	snd_iprintf(buffer, "\tstopped %d\n", harmony->cap_stopped);
	snd_iprintf(buffer, "\tbuffer %lx, count %d\n", harmony->cap_buffer, harmony->cap_count);
	snd_iprintf(buffer, "\tbuf %d, size %d\n\n", harmony->cap_buf, harmony->cap_size);

	snd_iprintf(buffer, "Funny stats: total played=%d, recorded=%d\n\n", harmony->ply_total, harmony->cap_total);
		
	snd_iprintf(buffer, "Register:\n");
	snd_iprintf(buffer, "\tgainctl: %lx\n", (unsigned long) gsc_readl(harmony->hpa+REG_GAINCTL));
	snd_iprintf(buffer, "\tcntl: %lx\n", (unsigned long) gsc_readl(harmony->hpa+REG_CNTL));
	snd_iprintf(buffer, "\tid: %lx\n", (unsigned long) gsc_readl(harmony->hpa+REG_ID));
	snd_iprintf(buffer, "\tpcuradd: %lx\n", (unsigned long) gsc_readl(harmony->hpa+REG_PCURADD));
	snd_iprintf(buffer, "\trcuradd: %lx\n", (unsigned long) gsc_readl(harmony->hpa+REG_RCURADD));
	snd_iprintf(buffer, "\tpnxtadd: %lx\n", (unsigned long) gsc_readl(harmony->hpa+REG_PNXTADD));
	snd_iprintf(buffer, "\trnxtadd: %lx\n", (unsigned long) gsc_readl(harmony->hpa+REG_RNXTADD));
	snd_iprintf(buffer, "\tdstatus: %lx\n", (unsigned long) gsc_readl(harmony->hpa+REG_DSTATUS));
	snd_iprintf(buffer, "\tov: %lx\n\n", (unsigned long) gsc_readl(harmony->hpa+REG_OV));
	
}

static void __devinit snd_harmony_proc_init(snd_card_harmony_t *harmony)
{
	snd_info_entry_t *entry;
	
	if (! snd_card_proc_new(harmony->card, "harmony", &entry))
		snd_info_set_text_ops(entry, harmony, 2048, snd_harmony_proc_read);
}

/* 
 * PCM Stuff
 */

static int snd_card_harmony_playback_ioctl(snd_pcm_substream_t * substream,
				         unsigned int cmd,
				         void *arg)
{
	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

static int snd_card_harmony_capture_ioctl(snd_pcm_substream_t * substream,
					unsigned int cmd,
					void *arg)
{
	return snd_pcm_lib_ioctl(substream, cmd, arg);
}

static int snd_card_harmony_playback_trigger(snd_pcm_substream_t * substream,
					   int cmd)
{
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	
	switch (cmd) {
		case SNDRV_PCM_TRIGGER_STOP:
			if (harmony->ply_stopped) 
				return -EBUSY;
			harmony->ply_stopped = 1;
			snd_harmony_disable_interrupts(harmony);
			break;
		case SNDRV_PCM_TRIGGER_START:
			if (!harmony->ply_stopped)
				return -EBUSY;
			harmony->ply_stopped = 0;
			/* write the location of the first buffer to play */
			gsc_writel(harmony->ply_buffer, harmony->hpa+REG_PNXTADD);
			snd_harmony_enable_interrupts(harmony);
			break;
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		case SNDRV_PCM_TRIGGER_SUSPEND:
			DPRINTK(KERN_INFO PFX "received unimplemented trigger: %d\n", cmd);
		default:
			return -EINVAL;
	}
	return 0;
}

static int snd_card_harmony_capture_trigger(snd_pcm_substream_t * substream,
					  int cmd)
{
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	
	switch (cmd) {
		case SNDRV_PCM_TRIGGER_STOP:
			if (harmony->cap_stopped) 
				return -EBUSY;
			harmony->cap_stopped = 1;
			snd_harmony_disable_interrupts(harmony);
			break;
		case SNDRV_PCM_TRIGGER_START:
			if (!harmony->cap_stopped)
				return -EBUSY;
			harmony->cap_stopped = 0;
			snd_harmony_enable_interrupts(harmony);
			break;
		case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		case SNDRV_PCM_TRIGGER_SUSPEND:
			DPRINTK(KERN_INFO PFX "Received unimplemented trigger: %d\n", cmd);
		default:
			return -EINVAL;
	}
	return 0;
}

/* set data format */
static int snd_harmony_set_data_format(snd_card_harmony_t *harmony, int pcm_format)
{
	int old_format = harmony->data_format;
	int new_format = old_format;
	switch (pcm_format) {
	case SNDRV_PCM_FORMAT_S16_BE:
		new_format = HARMONY_DF_16BIT_LINEAR;
		break;
	case SNDRV_PCM_FORMAT_A_LAW:
		new_format = HARMONY_DF_8BIT_ALAW;
		break;
	case SNDRV_PCM_FORMAT_MU_LAW:
		new_format = HARMONY_DF_8BIT_ULAW;
		break;
	}
	/* re-initialize silence buffer if needed */
	if (old_format != new_format)
		snd_pcm_format_set_silence(pcm_format, harmony->silence_dma.area,
					   (HARMONY_BUF_SIZE * SILENCE_BUFS * 8) / snd_pcm_format_width(pcm_format));

	return new_format;
}

static int snd_card_harmony_playback_prepare(snd_pcm_substream_t * substream)
{
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	
	harmony->ply_size 			= snd_pcm_lib_buffer_bytes(substream);
	harmony->ply_count 			= snd_pcm_lib_period_bytes(substream);
	harmony->ply_buf			= 0;
	harmony->ply_stopped		= 1;
	
	/* initialize given sample rate */
	harmony->sample_rate = snd_card_harmony_rate_bits(runtime->rate);

	/* data format */
	harmony->data_format = snd_harmony_set_data_format(harmony, runtime->format);

	/* number of channels */
	if (runtime->channels == 2)
		harmony->stereo_select = HARMONY_SS_STEREO;
	else
		harmony->stereo_select = HARMONY_SS_MONO;
	
	DPRINTK(KERN_INFO PFX "Playback_prepare, sr=%d(%x), df=%x, ss=%x hpa=%lx\n", runtime->rate,
				harmony->sample_rate, harmony->data_format, harmony->stereo_select, harmony->hpa);
	snd_harmony_update_control(harmony);
	harmony->format_initialized = 1;
	harmony->ply_buffer = runtime->dma_addr;
	
	return 0;
}

static int snd_card_harmony_capture_prepare(snd_pcm_substream_t * substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	
	harmony->cap_size 			= snd_pcm_lib_buffer_bytes(substream);
	harmony->cap_count 			= snd_pcm_lib_period_bytes(substream);
	harmony->cap_count			= 0;
	harmony->cap_stopped		= 1;

	/* initialize given sample rate */
	harmony->sample_rate = snd_card_harmony_rate_bits(runtime->rate);
	
	/* data format */
	harmony->data_format = snd_harmony_set_data_format(harmony, runtime->format);
	
	/* number of channels */
	if (runtime->channels == 1)
		harmony->stereo_select = HARMONY_SS_MONO;
	else if (runtime->channels == 2)
		harmony->stereo_select = HARMONY_SS_STEREO;
		
	snd_harmony_update_control(harmony);
	harmony->format_initialized = 1;
	
	harmony->cap_buffer = runtime->dma_addr;

	return 0;
}

static snd_pcm_uframes_t snd_card_harmony_capture_pointer(snd_pcm_substream_t * substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	unsigned long rcuradd;
	int recorded;
	
	if (harmony->cap_stopped) return 0;
	if (harmony->capture_substream == NULL) return 0;

	rcuradd = gsc_readl(harmony->hpa+REG_RCURADD);
	recorded = (rcuradd - harmony->cap_buffer);
	recorded %= harmony->cap_size;
		
	return bytes_to_frames(runtime, recorded);
}

/*
 */

static snd_pcm_uframes_t snd_card_harmony_playback_pointer(snd_pcm_substream_t * substream)
{
	snd_pcm_runtime_t *runtime = substream->runtime;
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	int played;
	long int pcuradd = gsc_readl(harmony->hpa+REG_PCURADD);
	
	if ((harmony->ply_stopped) || (harmony->playback_substream == NULL)) return 0;
	if ((harmony->ply_buffer == 0) || (harmony->ply_size == 0)) return 0;
	
	played = (pcuradd - harmony->ply_buffer);
	
	printk(KERN_DEBUG PFX "Pointer is %lx-%lx = %d\n", pcuradd, harmony->ply_buffer, played);	

	if (pcuradd > harmony->ply_buffer + harmony->ply_size) return 0;
	
	return bytes_to_frames(runtime, played);
}

static snd_pcm_hardware_t snd_card_harmony_playback =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED | 
					SNDRV_PCM_INFO_JOINT_DUPLEX | 
					SNDRV_PCM_INFO_MMAP_VALID |
					SNDRV_PCM_INFO_BLOCK_TRANSFER),
	.formats =		(SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_BE | 
					SNDRV_PCM_FMTBIT_A_LAW | SNDRV_PCM_FMTBIT_MU_LAW),
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		5500,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	MAX_BUFFER_SIZE,
	.period_bytes_min =	HARMONY_BUF_SIZE,
	.period_bytes_max =	HARMONY_BUF_SIZE,
	.periods_min =		1,
	.periods_max =		MAX_BUFS,
	.fifo_size =		0,
};

static snd_pcm_hardware_t snd_card_harmony_capture =
{
	.info =			(SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED | 
					SNDRV_PCM_INFO_JOINT_DUPLEX | 
					SNDRV_PCM_INFO_MMAP_VALID |
					SNDRV_PCM_INFO_BLOCK_TRANSFER),
	.formats =		(SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_BE | 
					SNDRV_PCM_FMTBIT_A_LAW | SNDRV_PCM_FMTBIT_MU_LAW),
	.rates =		SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_8000_48000,
	.rate_min =		5500,
	.rate_max =		48000,
	.channels_min =		1,
	.channels_max =		2,
	.buffer_bytes_max =	MAX_BUFFER_SIZE,
	.period_bytes_min =	HARMONY_BUF_SIZE,
	.period_bytes_max =	HARMONY_BUF_SIZE,
	.periods_min =		1,
	.periods_max =		MAX_BUFS,
	.fifo_size =		0,
};

static int snd_card_harmony_playback_open(snd_pcm_substream_t * substream)
{
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	int err;
	
	harmony->playback_substream = substream;
	runtime->hw = snd_card_harmony_playback;
	snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, &hw_constraint_rates);
	
	if ((err = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS)) < 0)
		return err;
	
	return 0;
}

static int snd_card_harmony_capture_open(snd_pcm_substream_t * substream)
{
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	snd_pcm_runtime_t *runtime = substream->runtime;
	int err;
	
	harmony->capture_substream = substream;
	runtime->hw = snd_card_harmony_capture;
	snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE, &hw_constraint_rates);
	if ((err = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS)) < 0)
		return err;
	return 0;

}

static int snd_card_harmony_playback_close(snd_pcm_substream_t * substream)
{
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	
	harmony->playback_substream = NULL;
	harmony->ply_size 			= 0;
	harmony->ply_buf			= 0;
	harmony->ply_buffer			= 0;
	harmony->ply_count			= 0;
	harmony->ply_stopped		= 1;
	harmony->format_initialized = 0;
	
	return 0;
}

static int snd_card_harmony_capture_close(snd_pcm_substream_t * substream)
{
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	
	harmony->capture_substream = NULL;
	harmony->cap_size 			= 0;
	harmony->cap_buf			= 0;
	harmony->cap_buffer			= 0;
	harmony->cap_count			= 0;
	harmony->cap_stopped		= 1;
	harmony->format_initialized = 0;
	
	return 0;
}

static int snd_card_harmony_hw_params(snd_pcm_substream_t *substream, 
	                   snd_pcm_hw_params_t * hw_params)
{
	int err;
	snd_card_harmony_t *harmony = snd_pcm_substream_chip(substream);
	
	err = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(hw_params));
	if (err > 0 && harmony->dma_dev.type == SNDRV_DMA_TYPE_CONTINUOUS)
		substream->runtime->dma_addr = __pa(substream->runtime->dma_area);
	DPRINTK(KERN_INFO PFX "HW Params returned %d, dma_addr %lx\n", err,
			(unsigned long)substream->runtime->dma_addr);
	return err;
}

static int snd_card_harmony_hw_free(snd_pcm_substream_t *substream) 
{
	snd_pcm_lib_free_pages(substream);		
	return 0;
}

static snd_pcm_ops_t snd_card_harmony_playback_ops = {
	.open =			snd_card_harmony_playback_open,
	.close =		snd_card_harmony_playback_close,
	.ioctl =		snd_card_harmony_playback_ioctl,
	.hw_params = 	snd_card_harmony_hw_params,
	.hw_free = 		snd_card_harmony_hw_free,
	.prepare =		snd_card_harmony_playback_prepare,
	.trigger =		snd_card_harmony_playback_trigger,
 	.pointer =		snd_card_harmony_playback_pointer,
};

static snd_pcm_ops_t snd_card_harmony_capture_ops = {
	.open =			snd_card_harmony_capture_open,
	.close =		snd_card_harmony_capture_close,
	.ioctl =		snd_card_harmony_capture_ioctl,
	.hw_params = 	snd_card_harmony_hw_params,
	.hw_free = 		snd_card_harmony_hw_free,
	.prepare =		snd_card_harmony_capture_prepare,
	.trigger =		snd_card_harmony_capture_trigger,
	.pointer =		snd_card_harmony_capture_pointer,
};

static int snd_card_harmony_pcm_init(snd_card_harmony_t *harmony)
{
	snd_pcm_t *pcm;
	int err;

	/* Request that IRQ */
	if (request_irq(harmony->irq, snd_card_harmony_interrupt, 0 ,"harmony", harmony)) {
		printk(KERN_ERR PFX "Error requesting irq %d.\n", harmony->irq);
		return -EFAULT;
	}
	
	snd_harmony_disable_interrupts(harmony);
	
   	if ((err = snd_pcm_new(harmony->card, "Harmony", 0, 1, 1, &pcm)) < 0)
		return err;
	
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_card_harmony_playback_ops);
 	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_card_harmony_capture_ops); 
	
	pcm->private_data = harmony;
	pcm->info_flags = 0;
	strcpy(pcm->name, "Harmony");
	harmony->pcm = pcm;
	
	/* initialize graveyard buffer */
	harmony->dma_dev.type = SNDRV_DMA_TYPE_DEV;
	harmony->dma_dev.dev = &harmony->pa_dev->dev;
	err = snd_dma_alloc_pages(harmony->dma_dev.type,
				  harmony->dma_dev.dev,
				  HARMONY_BUF_SIZE*GRAVEYARD_BUFS,
				  &harmony->graveyard_dma);
	if (err == -ENOMEM) {
		/* use continuous buffers */
		harmony->dma_dev.type = SNDRV_DMA_TYPE_CONTINUOUS;
		harmony->dma_dev.dev = snd_dma_continuous_data(GFP_KERNEL);
		err = snd_dma_alloc_pages(harmony->dma_dev.type,
					  harmony->dma_dev.dev,
					  HARMONY_BUF_SIZE*GRAVEYARD_BUFS,
					  &harmony->graveyard_dma);
	}
	if (err < 0) {
		printk(KERN_ERR PFX "can't allocate graveyard buffer\n");
		return err;
	}
	harmony->graveyard_count = 0;
	
	/* initialize silence buffers */
	err = snd_dma_alloc_pages(harmony->dma_dev.type,
				  harmony->dma_dev.dev,
				  HARMONY_BUF_SIZE*SILENCE_BUFS,
				  &harmony->silence_dma);
	if (err < 0) {
		printk(KERN_ERR PFX "can't allocate silence buffer\n");
		return err;
	}
	harmony->silence_count = 0;

	if (harmony->dma_dev.type == SNDRV_DMA_TYPE_CONTINUOUS) {
		harmony->graveyard_dma.addr = __pa(harmony->graveyard_dma.area);
		harmony->silence_dma.addr = __pa(harmony->silence_dma.area);
	}

	harmony->ply_stopped = harmony->cap_stopped = 1;
	
	harmony->playback_substream = NULL;
	harmony->capture_substream = NULL;
	harmony->graveyard_count = 0;

	err = snd_pcm_lib_preallocate_pages_for_all(pcm, harmony->dma_dev.type,
						    harmony->dma_dev.dev,
						    MAX_BUFFER_SIZE, MAX_BUFFER_SIZE);
	if (err < 0) {
		printk(KERN_ERR PFX "buffer allocation error %d\n", err);
		// return err;
	}

	return 0;
}

/*
 * mixer routines
 */

static void snd_harmony_set_new_gain(snd_card_harmony_t *harmony)
{
	DPRINTK(KERN_INFO PFX "Setting new gain %x at %lx\n", harmony->current_gain, harmony->hpa+REG_GAINCTL);
	/* Wait until we're out of control mode */
 	snd_harmony_wait_cntl(harmony);
	
	gsc_writel(harmony->current_gain, harmony->hpa+REG_GAINCTL);
}

#define HARMONY_VOLUME(xname, left_shift, right_shift, mask, invert) \
{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
  .info = snd_harmony_mixercontrol_info, \
  .get = snd_harmony_volume_get, .put = snd_harmony_volume_put, \
  .private_value = ((left_shift) | ((right_shift) << 8) | ((mask) << 16) | ((invert) << 24)) }

static int snd_harmony_mixercontrol_info(snd_kcontrol_t * kcontrol, snd_ctl_elem_info_t * uinfo)
{
	int mask = (kcontrol->private_value >> 16) & 0xff;
	int left_shift = (kcontrol->private_value) & 0xff;
	int right_shift = (kcontrol->private_value >> 8) & 0xff;
	
	uinfo->type = (mask == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN : SNDRV_CTL_ELEM_TYPE_INTEGER);
	uinfo->count = (left_shift == right_shift) ? 1 : 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = mask;
	return 0;
}
 
static int snd_harmony_volume_get(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	snd_card_harmony_t *harmony = snd_kcontrol_chip(kcontrol);
	int shift_left = (kcontrol->private_value) & 0xff;
	int shift_right = (kcontrol->private_value >> 8) & 0xff;
	int mask = (kcontrol->private_value >> 16) & 0xff;
	int invert = (kcontrol->private_value >> 24) & 0xff;
	unsigned long flags;
	int left, right;
	
	spin_lock_irqsave(&harmony->mixer_lock, flags);
	left = (harmony->current_gain >> shift_left) & mask;
	right = (harmony->current_gain >> shift_right) & mask;

	if (invert) {
		left = mask - left;
		right = mask - right;
	}
	ucontrol->value.integer.value[0] = left;
	ucontrol->value.integer.value[1] = right;
	spin_unlock_irqrestore(&harmony->mixer_lock, flags);

	return 0;
}  

static int snd_harmony_volume_put(snd_kcontrol_t * kcontrol, snd_ctl_elem_value_t * ucontrol)
{
	snd_card_harmony_t *harmony = snd_kcontrol_chip(kcontrol);
	int shift_left = (kcontrol->private_value) & 0xff;
	int shift_right = (kcontrol->private_value >> 8) & 0xff;
	int mask = (kcontrol->private_value >> 16) & 0xff;
	int invert = (kcontrol->private_value >> 24) & 0xff;
	unsigned long flags;
	int left, right;
	int old_gain = harmony->current_gain;
	
	left = ucontrol->value.integer.value[0] & mask;
	right = ucontrol->value.integer.value[1] & mask;
	if (invert) {
		left = mask - left;
		right = mask - right;
	}
	
	spin_lock_irqsave(&harmony->mixer_lock, flags);
	harmony->current_gain = harmony->current_gain & ~( (mask << shift_right) | (mask << shift_left));
 	harmony->current_gain = harmony->current_gain | ((left << shift_left) | (right << shift_right) );
	snd_harmony_set_new_gain(harmony);
	spin_unlock_irqrestore(&harmony->mixer_lock, flags);
	
	return (old_gain - harmony->current_gain);
}

#define HARMONY_CONTROLS (sizeof(snd_harmony_controls)/sizeof(snd_kcontrol_new_t))

static snd_kcontrol_new_t snd_harmony_controls[] = {
HARMONY_VOLUME("PCM Capture Volume", 12, 16, 0x0f, 0),
HARMONY_VOLUME("Master Volume", 20, 20, 0x0f, 1),
HARMONY_VOLUME("PCM Playback Volume", 6, 0, 0x3f, 1),
};

static void __init snd_harmony_reset_codec(snd_card_harmony_t *harmony)
{
 	snd_harmony_wait_cntl(harmony);
	gsc_writel(1, harmony->hpa+REG_RESET);
	mdelay(50);		/* wait 50 ms */
	gsc_writel(0, harmony->hpa+REG_RESET);
}

/*
 * Mute all the output and reset Harmony.
 */

static void __init snd_harmony_mixer_reset(snd_card_harmony_t *harmony)
{
	harmony->current_gain = HARMONY_GAIN_TOTAL_SILENCE;
	snd_harmony_set_new_gain(harmony);
	snd_harmony_reset_codec(harmony);
	harmony->current_gain = HARMONY_GAIN_DEFAULT;
	snd_harmony_set_new_gain(harmony);
}


static int __init snd_card_harmony_mixer_init(snd_card_harmony_t *harmony)
{
	snd_card_t *card = harmony->card;
	int idx, err;

	snd_assert(harmony != NULL, return -EINVAL);
	strcpy(card->mixername, "Harmony Gain control interface");

	for (idx = 0; idx < HARMONY_CONTROLS; idx++) {
		if ((err = snd_ctl_add(card, snd_ctl_new1(&snd_harmony_controls[idx], harmony))) < 0)
			return err;
	}
	
	snd_harmony_mixer_reset(harmony);

	return 0;
}

static int snd_card_harmony_create(snd_card_t *card, struct parisc_device *pa_dev, snd_card_harmony_t *harmony)
{
	u32	cntl;
	
	harmony->card = card;
	
	harmony->pa_dev = pa_dev;

	/* Set the HPA of harmony */
	harmony->hpa = pa_dev->hpa;
	
	harmony->irq = pa_dev->irq;
	if (!harmony->irq) {
		printk(KERN_ERR PFX "no irq found\n");
		return -ENODEV;
	}

	/* Grab the ID and revision from the device */
	harmony->id = (gsc_readl(harmony->hpa+REG_ID)&0x00ff0000) >> 16;
	if ((harmony->id | 1) != 0x15) {
		printk(KERN_WARNING PFX "wrong harmony id 0x%02x\n", harmony->id);
		return -EBUSY;
	}
	cntl = gsc_readl(harmony->hpa+REG_CNTL);
	harmony->rev = (cntl>>20) & 0xff;

	printk(KERN_INFO "Lasi Harmony Audio driver h/w id %i, rev. %i at 0x%lx, IRQ %i\n",	harmony->id, harmony->rev, pa_dev->hpa, harmony->irq);
	
	/* Make sure the control bit isn't set, although I don't think it 
	   ever is. */
	if (cntl & HARMONY_CNTL_C) {
		printk(KERN_WARNING PFX "CNTL busy\n");
		harmony->hpa = 0;
		return -EBUSY;
	}
	
	return 0;
}
	
static int __init snd_card_harmony_probe(struct parisc_device *pa_dev)
{
	static int dev;
	snd_card_harmony_t *chip;
	snd_card_t *card;
	int err;
	
	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		dev++;
		return -ENOENT;
	}
	
	snd_harmony_cards[dev] = snd_card_new(index[dev], id[dev], THIS_MODULE,
			    sizeof(snd_card_harmony_t));
	card = snd_harmony_cards[dev];
				
	if (card == NULL)
		return -ENOMEM;
	chip = (struct snd_card_harmony *)card->private_data;
	spin_lock_init(&chip->control_lock);
	spin_lock_init(&chip->mixer_lock);
	
	if ((err = snd_card_harmony_create(card, pa_dev, chip)) < 0) {
		printk(KERN_ERR PFX "Creation failed\n");
		snd_card_free(card);
		return err;
	}
	if ((err = snd_card_harmony_pcm_init(chip)) < 0) {
		printk(KERN_ERR PFX "PCM Init failed\n");
		snd_card_free(card);
		return err;
	}
	if ((err = snd_card_harmony_mixer_init(chip)) < 0) {
		printk(KERN_ERR PFX "Mixer init failed\n");
		snd_card_free(card);
		return err;
	}
	
	snd_harmony_proc_init(chip);
	
	strcpy(card->driver, "Harmony");
	strcpy(card->shortname, "ALSA driver for LASI Harmony");
	sprintf(card->longname, "%s at h/w, id %i, rev. %i hpa 0x%lx, IRQ %i\n",card->shortname, chip->id, chip->rev, pa_dev->hpa, chip->irq);

	if ((err = snd_card_register(card)) < 0) {
		snd_card_free(card);
		return err;
	}

	printk(KERN_DEBUG PFX "Successfully registered harmony pcm backend & mixer %d\n", dev);
	dev++;
	return 0;
}

static struct parisc_device_id snd_card_harmony_devicetbl[] = {
 { HPHW_FIO, HVERSION_REV_ANY_ID, HVERSION_ANY_ID, 0x0007A }, /* Bushmaster/Flounder */
 { HPHW_FIO, HVERSION_REV_ANY_ID, HVERSION_ANY_ID, 0x0007B }, /* 712/715 Audio */
 { HPHW_FIO, HVERSION_REV_ANY_ID, HVERSION_ANY_ID, 0x0007E }, /* Pace Audio */
 { HPHW_FIO, HVERSION_REV_ANY_ID, HVERSION_ANY_ID, 0x0007F }, /* Outfield / Coral II */
 { 0, }
};

MODULE_DEVICE_TABLE(parisc, snd_card_harmony_devicetbl);

/*
 * bloc device parisc. c'est une structure qui definit un device
 * que l'on trouve sur parisc. 
 * On y trouve les differents numeros HVERSION correspondant au device
 * en question (ce qui permet a l'inventory de l'identifier) et la fonction
 * d'initialisation du chose 
 */

static struct parisc_driver snd_card_harmony_driver = {
	.name		= "Lasi ALSA Harmony",
	.id_table	= snd_card_harmony_devicetbl,
	.probe		= snd_card_harmony_probe,
};

static int __init alsa_card_harmony_init(void)
{
	int err;
	
	if ((err = register_parisc_driver(&snd_card_harmony_driver)) < 0) {
		printk(KERN_ERR "Harmony soundcard not found or device busy\n");
		return err;
	}

	return 0;
}

static void __exit alsa_card_harmony_exit(void)
{
	int idx;
	snd_card_harmony_t *harmony;
	
	for (idx = 0; idx < SNDRV_CARDS; idx++)
	{
		if (snd_harmony_cards[idx] != NULL)
		{	
			DPRINTK(KERN_INFO PFX "Freeing card %d\n", idx);
			harmony = snd_harmony_cards[idx]->private_data;
			free_irq(harmony->irq, harmony);
			printk(KERN_INFO PFX "Card unloaded %d, irq=%d\n", idx, harmony->irq);
			snd_card_free(snd_harmony_cards[idx]);
		}
	}	
	if (unregister_parisc_driver(&snd_card_harmony_driver) < 0)
		printk(KERN_ERR PFX "Failed to unregister Harmony driver\n");
}

module_init(alsa_card_harmony_init)
module_exit(alsa_card_harmony_exit)
