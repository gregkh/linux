/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AV7110_CA_H_
#define _AV7110_CA_H_

struct av7110;

void CI_handle(struct av7110 *av7110, u8 *data, u16 len);
void ci_get_data(struct dvb_ringbuffer *cibuf, u8 *data, int len);

int av7110_ca_register(struct av7110 *av7110);
void av7110_ca_unregister(struct av7110 *av7110);
int av7110_ca_init(struct av7110 *av7110);
void av7110_ca_exit(struct av7110 *av7110);

#endif /* _AV7110_CA_H_ */
