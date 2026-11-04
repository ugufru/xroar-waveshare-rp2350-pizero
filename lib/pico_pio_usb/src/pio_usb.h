
#pragma once

#include "pio_usb_configuration.h"
#include "usb_definitions.h"

#ifdef __cplusplus
 extern "C" {
#endif

// Host functions
usb_device_t *pio_usb_host_init(const pio_usb_configuration_t *c);
int pio_usb_host_add_port(uint8_t pin_dp, PIO_USB_PINOUT pinout);
void pio_usb_host_task(void);
void pio_usb_host_stop(void);
void pio_usb_host_restart(void);
uint32_t pio_usb_host_get_frame_number(void);

// Call this every 1ms when skip_alarm_pool is true.
void pio_usb_host_frame(void);

// PIZERO-255 (this project): isochronous OUT streaming. Once started on an
// open isochronous OUT endpoint, the frame handler calls fill every frame
// (1 ms) for that frame's packet, at most max bytes, and sends it as DATA0
// with no handshake, as isochronous transfers are. fill runs in the frame
// handler's interrupt, so it must be quick and must not block. NULL stops
// the stream. One stream at a time; up to PIO_USB_ISO_MAX bytes a packet.
#define PIO_USB_ISO_MAX 200
typedef uint16_t (*pio_usb_iso_fill_t)(uint8_t *buf, uint16_t max);
bool pio_usb_host_iso_out_stream(uint8_t root_idx, uint8_t device_address,
                                 uint8_t ep_address, pio_usb_iso_fill_t fill);
uint32_t pio_usb_host_iso_frames(void);   // packets sent, for telemetry
// PIZERO-322 (this project): waits for an audio packet that gave up after 1 ms.
uint32_t pio_usb_bus_tx_wait_timeouts(void);

// Device functions
usb_device_t *pio_usb_device_init(const pio_usb_configuration_t *c,
                                  const usb_descriptor_buffers_t *buffers);
void pio_usb_device_task(void);

// Common functions
endpoint_t *pio_usb_get_endpoint(usb_device_t *device, uint8_t idx);
int pio_usb_get_in_data(endpoint_t *ep, uint8_t *buffer, uint8_t len);
int pio_usb_set_out_data(endpoint_t *ep, const uint8_t *buffer, uint8_t len);

#ifdef __cplusplus
 }
#endif
