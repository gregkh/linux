#ifndef _ATP870U_H
#define _ATP870U_H

#include <linux/types.h>

/* I/O Port */

#define MAX_CDB		12
#define MAX_SENSE	14
#define qcnt		32
#define ATP870U_SCATTER 128
#define ATP870U_CMDLUN 	1

struct atp_unit {
	unsigned long ioport;
	unsigned long pciport;
	unsigned char last_cmd;
	unsigned char in_snd;
	unsigned char in_int;
	unsigned char quhdu;
	unsigned char quendu;
	unsigned char scam_on;
	unsigned char global_map;
	unsigned char chip_veru;
	unsigned char host_idu;
	volatile int working;
	unsigned short wide_idu;
	unsigned short active_idu;
	unsigned short ultra_map;
	unsigned short async;
	unsigned short deviceid;
	unsigned char ata_cdbu[16];
	unsigned char sp[16];
	struct scsi_cmnd *querequ[qcnt];
	struct atp_id {
		unsigned char dirctu;
		unsigned char devspu;
		unsigned char devtypeu;
		unsigned long prdaddru;
		unsigned long tran_lenu;
		unsigned long last_lenu;
		unsigned char *prd_posu;
		unsigned char *prd_tableu;
		dma_addr_t prd_phys;
		struct scsi_cmnd *curr_req;
	} id[16];
	struct Scsi_Host *host;
	struct pci_dev *pdev;
	unsigned int unit;
};

#endif
