#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/i2c.h>
#include <linux/types.h>
#include <linux/videodev.h>
#include <linux/init.h>

#include <media/id.h>
#include <media/saa6752hs.h>

/* Addresses to scan */
static unsigned short normal_i2c[] = {0x20, I2C_CLIENT_END};
static unsigned short normal_i2c_range[] = {I2C_CLIENT_END};
I2C_CLIENT_INSMOD;

MODULE_DESCRIPTION("device driver for saa6752hs MPEG2 encoder");
MODULE_AUTHOR("Andrew de Quincey");
MODULE_LICENSE("GPL");

static struct i2c_driver driver;
static struct i2c_client client_template;


enum saa6752hs_command {
	SAA6752HS_COMMAND_RESET = 0,
    	SAA6752HS_COMMAND_STOP = 1,
    	SAA6752HS_COMMAND_START = 2,
    	SAA6752HS_COMMAND_PAUSE = 3,
    	SAA6752HS_COMMAND_RECONFIGURE = 4,
    	SAA6752HS_COMMAND_SLEEP = 5,
	SAA6752HS_COMMAND_RECONFIGURE_FORCE = 6,

	SAA6752HS_COMMAND_MAX
};


/* ---------------------------------------------------------------------- */

static u8 PAT[] = {
	0xc2, // i2c register
	0x00, // table number for encoder

	0x47, // sync
	0x40, 0x00, // transport_error_indicator(0), payload_unit_start(1), transport_priority(0), pid(0)
	0x10, // transport_scrambling_control(00), adaptation_field_control(01), continuity_counter(0)

	0x00, // PSI pointer to start of table

	0x00, // tid(0)
	0xb0, 0x0d, // section_syntax_indicator(1), section_length(13)

	0x00, 0x01, // transport_stream_id(1)

	0xc1, // version_number(0), current_next_indicator(1)

	0x00, 0x00, // section_number(0), last_section_number(0)

	0x00, 0x01, // program_number(1)

	0xe0, 0x10, // PMT PID(0x10)

	0x76, 0xf1, 0x44, 0xd1 // CRC32
};

static u8 PMT[] = {
	0xc2, // i2c register
	0x01, // table number for encoder

	0x47, // sync
	0x40, 0x10, // transport_error_indicator(0), payload_unit_start(1), transport_priority(0), pid(0x10)
	0x10, // transport_scrambling_control(00), adaptation_field_control(01), continuity_counter(0)

	0x00, // PSI pointer to start of table

	0x02, // tid(2)
	0xb0, 0x17, // section_syntax_indicator(1), section_length(23)

	0x00, 0x01, // program_number(1)

	0xc1, // version_number(0), current_next_indicator(1)

	0x00, 0x00, // section_number(0), last_section_number(0)

	0xe1, 0x04, // PCR_PID (0x104)

	0xf0, 0x00, // program_info_length(0)

	0x02, 0xe1, 0x00, 0xf0, 0x00, // video stream type(2), pid(0x100)
	0x04, 0xe1, 0x03, 0xf0, 0x00, // audio stream type(4), pid(0x103)

	0xa1, 0xca, 0x0f, 0x82 // CRC32
};

static struct mpeg_params mpeg_params_template =
{
	.bitrate_mode = MPEG_BITRATE_MODE_CBR,
	.video_target_bitrate = 5000,
	.audio_bitrate = MPEG_AUDIO_BITRATE_256,
	.total_bitrate = 6000,
};


/* ---------------------------------------------------------------------- */


static int saa6752hs_chip_command(struct i2c_client* client,
				  enum saa6752hs_command command)
{
	unsigned char buf[3];
	unsigned long timeout;
	int status = 0;

	// execute the command
	switch(command) {
  	case SAA6752HS_COMMAND_RESET:
  		buf[0] = 0x00;
		break;

	case SAA6752HS_COMMAND_STOP:
		  	buf[0] = 0x03;
		break;

	case SAA6752HS_COMMAND_START:
  		buf[0] = 0x02;
		break;

	case SAA6752HS_COMMAND_PAUSE:
  		buf[0] = 0x04;
		break;

	case SAA6752HS_COMMAND_RECONFIGURE:
		buf[0] = 0x05;
		break;

  	case SAA6752HS_COMMAND_SLEEP:
  		buf[0] = 0x06;
		break;

  	case SAA6752HS_COMMAND_RECONFIGURE_FORCE:
		buf[0] = 0x07;
		break;

	default:
		return -EINVAL;
	}

  	// set it and wait for it to be so
	i2c_master_send(client, buf, 1);
	timeout = jiffies + HZ * 3;
	for (;;) {
		// get the current status
		buf[0] = 0x10;
	  	i2c_master_send(client, buf, 1);
		i2c_master_recv(client, buf, 1);

		if (!(buf[0] & 0x20))
			break;
		if (time_after(jiffies,timeout)) {
			status = -ETIMEDOUT;
			break;
		}

		// wait a bit
		msleep(10);
	}

	// delay a bit to let encoder settle
	msleep(50);

	// done
  	return status;
}


static int saa6752hs_set_bitrate(struct i2c_client* client,
				 struct mpeg_params* params)
{
  	u8 buf[3];

	// set the bitrate mode
	buf[0] = 0x71;
	buf[1] = params->bitrate_mode;
	i2c_master_send(client, buf, 2);

	// set the video bitrate
	if (params->bitrate_mode == MPEG_BITRATE_MODE_VBR) {
		// set the target bitrate
		buf[0] = 0x80;
	    	buf[1] = params->video_target_bitrate >> 8;
	  	buf[2] = params->video_target_bitrate & 0xff;
		i2c_master_send(client, buf, 3);

		// set the max bitrate
		buf[0] = 0x81;
	    	buf[1] = params->video_max_bitrate >> 8;
	  	buf[2] = params->video_max_bitrate & 0xff;
		i2c_master_send(client, buf, 3);
	} else {
		// set the target bitrate (no max bitrate for CBR)
  		buf[0] = 0x81;
	    	buf[1] = params->video_target_bitrate >> 8;
	  	buf[2] = params->video_target_bitrate & 0xff;
		i2c_master_send(client, buf, 3);
	}

	// set the audio bitrate
 	buf[0] = 0x94;
  	buf[1] = params->audio_bitrate;
	i2c_master_send(client, buf, 2);

	// set the total bitrate
	buf[0] = 0xb1;
  	buf[1] = params->total_bitrate >> 8;
  	buf[2] = params->total_bitrate & 0xff;
	i2c_master_send(client, buf, 3);

	return 0;
}


static int saa6752hs_init(struct i2c_client* client, struct mpeg_params* params)
{
	unsigned char buf[3];
	void *data;

	// check the bitrate parameters first
	if (params != NULL) {
		if (params->bitrate_mode >= MPEG_BITRATE_MODE_MAX)
			return -EINVAL;
		if (params->video_target_bitrate >= MPEG_VIDEO_TARGET_BITRATE_MAX)
			return -EINVAL;
  		if (params->video_max_bitrate >= MPEG_VIDEO_MAX_BITRATE_MAX)
			return -EINVAL;
		if (params->audio_bitrate >= MPEG_AUDIO_BITRATE_MAX)
			return -EINVAL;
		if (params->total_bitrate >= MPEG_TOTAL_BITRATE_MAX)
        		return -EINVAL;
		if (params->bitrate_mode         == MPEG_BITRATE_MODE_MAX &&
		    params->video_target_bitrate <= params->video_max_bitrate)
			return -EINVAL;
	}

    	// Set GOP structure {3, 13}
	buf[0] = 0x72;
	buf[1] = 0x03;
	buf[2] = 0x0D;
	i2c_master_send(client,buf,3);

    	// Set minimum Q-scale {4}
	buf[0] = 0x82;
	buf[1] = 0x04;
	i2c_master_send(client,buf,2);

    	// Set maximum Q-scale {12}
	buf[0] = 0x83;
	buf[1] = 0x0C;
	i2c_master_send(client,buf,2);

    	// Set Output Protocol
	buf[0] = 0xD0;
	buf[1] = 0x01;
	i2c_master_send(client,buf,2);

    	// Set video output stream format {TS}
	buf[0] = 0xB0;
	buf[1] = 0x05;
	i2c_master_send(client,buf,2);

    	// Set Audio PID {0x103}
	buf[0] = 0xC1;
	buf[1] = 0x01;
	buf[2] = 0x03;
	i2c_master_send(client,buf,3);

        // setup bitrate settings
	data = i2c_get_clientdata(client);
	if (params) {
		saa6752hs_set_bitrate(client, params);
		memcpy(data, params, sizeof(struct mpeg_params));
	} else {
		// parameters were not supplied. use the previous set
   		saa6752hs_set_bitrate(client, (struct mpeg_params*) data);
	}

	// Send SI tables
  	i2c_master_send(client,PAT,sizeof(PAT));
  	i2c_master_send(client,PMT,sizeof(PMT));

	// mute then unmute audio. This removes buzzing artefacts
	buf[0] = 0xa4;
	buf[1] = 1;
	i2c_master_send(client, buf, 2);
  	buf[1] = 0;
	i2c_master_send(client, buf, 2);

	// start it going
	saa6752hs_chip_command(client, SAA6752HS_COMMAND_START);

	return 0;
}

static int saa6752hs_attach(struct i2c_adapter *adap, int addr, int kind)
{
	struct i2c_client *client;
	struct mpeg_params* params;

        client_template.adapter = adap;
        client_template.addr = addr;

        printk("saa6752hs: chip found @ 0x%x\n", addr<<1);

        if (NULL == (client = kmalloc(sizeof(struct i2c_client), GFP_KERNEL)))
                return -ENOMEM;
        memcpy(client,&client_template,sizeof(struct i2c_client));
	strlcpy(client->name, "saa6752hs", sizeof(client->name));

	if (NULL == (params = kmalloc(sizeof(struct mpeg_params), GFP_KERNEL)))
		return -ENOMEM;
	memcpy(params,&mpeg_params_template,sizeof(struct mpeg_params));
	i2c_set_clientdata(client, params);

        i2c_attach_client(client);

	return 0;
}

static int saa6752hs_probe(struct i2c_adapter *adap)
{
	if (adap->class & I2C_CLASS_TV_ANALOG)
		return i2c_probe(adap, &addr_data, saa6752hs_attach);
	return 0;
}

static int saa6752hs_detach(struct i2c_client *client)
{
	void *data;

	data = i2c_get_clientdata(client);
	i2c_detach_client(client);
	kfree(data);
	kfree(client);
	return 0;
}

static int
saa6752hs_command(struct i2c_client *client, unsigned int cmd, void *arg)
{
	struct mpeg_params* init_arg = arg;

        switch (cmd) {
	case MPEG_SETPARAMS:
   		return saa6752hs_init(client, init_arg);

	default:
		/* nothing */
		break;
	}

	return 0;
}

/* ----------------------------------------------------------------------- */

static struct i2c_driver driver = {
	.owner          = THIS_MODULE,
        .name           = "i2c saa6752hs MPEG encoder",
        .id             = I2C_DRIVERID_SAA6752HS,
        .flags          = I2C_DF_NOTIFY,
        .attach_adapter = saa6752hs_probe,
        .detach_client  = saa6752hs_detach,
        .command        = saa6752hs_command,
};

static struct i2c_client client_template =
{
	I2C_DEVNAME("(saa6752hs unset)"),
	.flags      = I2C_CLIENT_ALLOW_USE,
        .driver     = &driver,
};

static int __init saa6752hs_init_module(void)
{
	return i2c_add_driver(&driver);
}

static void __exit saa6752hs_cleanup_module(void)
{
	i2c_del_driver(&driver);
}

module_init(saa6752hs_init_module);
module_exit(saa6752hs_cleanup_module);

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
