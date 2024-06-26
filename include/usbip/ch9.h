#pragma once

/*
 * Declarations from <uapi/linux/usb/ch9.h>
 */

/*
 * bcdUSB field of USB device descriptor.
 * The value is in binary coded decimal with a format of 0xJJMN.
 */
enum {
	bcdUSB10 = 0x0100,
	bcdUSB11 = 0x0110,
	bcdUSB20 = 0x0200,
	bcdUSB30 = 0x0300,
	bcdUSB31 = 0x0310
};

enum usb_device_speed {
        USB_SPEED_UNKNOWN,			/* enumerating */
        USB_SPEED_LOW, USB_SPEED_FULL,		/* usb 1.1 */
        USB_SPEED_HIGH,				/* usb 2.0 */
        USB_SPEED_WIRELESS,			/* wireless (usb 2.5) */
        USB_SPEED_SUPER,			/* usb 3.0 */
        USB_SPEED_SUPER_PLUS			/* usb 3.1 */
};
