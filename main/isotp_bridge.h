#ifndef BRIDGE_H
#define BRIDGE_H

void		isotp_init();
void		isotp_deinit();
void		isotp_start_task();
void		isotp_stop_task();

void		bridge_connect();
void		bridge_disconnect();
void		bridge_received_ble(const void* src, size_t size);
void		bridge_received_wifi(uint16_t tx_id, uint16_t rx_id,
							 const uint8_t *data, size_t len);
int32_t		bridge_send_isotp(send_message_t *msg);
uint16_t	bridge_send_available();

#endif