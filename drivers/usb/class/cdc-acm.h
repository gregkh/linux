/*
 *
 * Includes for cdc-acm.c
 *
 * Mainly take from usbnet's cdc-ether part
 *
 */

/*
 * CMSPAR, some architectures can't have space and mark parity.
 */

#ifndef CMSPAR
#define CMSPAR			0
#endif

/*
 * Major and minor numbers.
 */

#define ACM_TTY_MAJOR		166
#define ACM_TTY_MINORS		32

/*
 * Requests.
 */

#define USB_RT_ACM		(USB_TYPE_CLASS | USB_RECIP_INTERFACE)

#define ACM_REQ_COMMAND		0x00
#define ACM_REQ_RESPONSE	0x01
#define ACM_REQ_SET_FEATURE	0x02
#define ACM_REQ_GET_FEATURE	0x03
#define ACM_REQ_CLEAR_FEATURE	0x04

#define ACM_REQ_SET_LINE	0x20
#define ACM_REQ_GET_LINE	0x21
#define ACM_REQ_SET_CONTROL	0x22
#define ACM_REQ_SEND_BREAK	0x23

/*
 * IRQs.
 */

#define ACM_IRQ_NETWORK		0x00
#define ACM_IRQ_LINE_STATE	0x20

/*
 * Output control lines.
 */

#define ACM_CTRL_DTR		0x01
#define ACM_CTRL_RTS		0x02

/*
 * Input control lines and line errors.
 */

#define ACM_CTRL_DCD		0x01
#define ACM_CTRL_DSR		0x02
#define ACM_CTRL_BRK		0x04
#define ACM_CTRL_RI		0x08

#define ACM_CTRL_FRAMING	0x10
#define ACM_CTRL_PARITY		0x20
#define ACM_CTRL_OVERRUN	0x40

/*
 * Line speed and caracter encoding.
 */

struct acm_line {
	__le32 speed;
	__u8 stopbits;
	__u8 parity;
	__u8 databits;
} __attribute__ ((packed));

/*
 * Internal driver structures.
 */

struct acm {
	struct usb_device *dev;				/* the corresponding usb device */
	struct usb_interface *control;			/* control interface */
	struct usb_interface *data;			/* data interface */
	struct tty_struct *tty;				/* the corresponding tty */
	struct urb *ctrlurb, *readurb, *writeurb;	/* urbs */
	u8 *ctrl_buffer, *read_buffer, *write_buffer;	/* buffers of urbs */
	dma_addr_t ctrl_dma, read_dma, write_dma;	/* dma handles of buffers */
	struct acm_line line;				/* line coding (bits, stop, parity) */
	struct work_struct work;			/* work queue entry for line discipline waking up */
	struct tasklet_struct bh;			/* rx processing */
	spinlock_t throttle_lock;			/* synchronize throtteling and read callback */
	unsigned int ctrlin;				/* input control lines (DCD, DSR, RI, break, overruns) */
	unsigned int ctrlout;				/* output control lines (DTR, RTS) */
	unsigned int writesize;				/* max packet size for the output bulk endpoint */
	unsigned int readsize,ctrlsize;			/* buffer sizes for freeing */
	unsigned int used;				/* someone has this acm's device open */
	unsigned int minor;				/* acm minor number */
	unsigned char throttle;				/* throttled by tty layer */
	unsigned char clocal;				/* termios CLOCAL */
	unsigned char ready_for_write;			/* write urb can be used */
	unsigned char resubmit_to_unthrottle;		/* throtteling has disabled the read urb */
	unsigned int ctrl_caps;				/* control capabilities from the class specific header */
};

/* "Union Functional Descriptor" from CDC spec 5.2.3.X */
struct union_desc {
	u8	bLength;
	u8	bDescriptorType;
	u8	bDescriptorSubType;

	u8	bMasterInterface0;
	u8	bSlaveInterface0;
	/* ... and there could be other slave interfaces */
} __attribute__ ((packed));

/* class specific descriptor types */
#define CDC_HEADER_TYPE			0x00
#define CDC_CALL_MANAGEMENT_TYPE	0x01
#define CDC_AC_MANAGEMENT_TYPE		0x02
#define CDC_UNION_TYPE			0x06
#define CDC_COUNTRY_TYPE		0x07

#define CDC_DATA_INTERFACE_TYPE	0x0a

/* constants describing various quirks and errors */
#define NO_UNION_NORMAL			1
