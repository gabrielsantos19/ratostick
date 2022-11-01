// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (c) 1999-2001 Vojtech Pavlik
 *
 *  USB HIDBP Mouse support
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb/input.h>
#include <linux/hid.h>

/*
 * Version Information
 */
#define DRIVER_VERSION "v1.6"
#define DRIVER_AUTHOR "Vojtech Pavlik <vojtech@ucw.cz>"
#define DRIVER_DESC "USB HID Boot Protocol mouse driver"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

struct usb_mouse {
	char name[128];
	char phys[64];
	struct usb_device *usbdev;
	struct input_dev *dev;
	struct urb *irq;

	signed char *data;
	dma_addr_t data_dma;
};

static void usb_mouse_irq(struct urb *urb)
{
	struct usb_mouse *mouse = urb->context;
	unsigned char *data = mouse->data;
	struct input_dev *dev = mouse->dev;
	int status;

	switch (urb->status) {
	case 0:			/* success */
		break;
	case -ECONNRESET:	/* unlink */
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	/* -EPIPE:  should clear the halt */
	default:		/* error */
		goto resubmit;
	}



	// Byte 0 -> Analógico esquerdo X
	// Byte 1 -> Analógico esquerdo Y
	// Byte 2 -> ???
	// Byte 3 -> Analógico direito X
	// Byte 4 -> Analógico direito Y
	// Byte 5 -> Contagem no sentido horário das setas pressionadas
	//   0x0F == 0 ->
	//   0x0F == 2 ->
	//   0x0F == 4 ->
	//   0x0F == 6 ->
	//   0x10 -> 1
	//   0x20 -> 2
	//   0x40 -> 3
	//   0x80 -> 4
	// Byte 6
	//   0x01 -> L1
	//   0x02 -> R1
	//   0x04 -> L2
	//   0x08 -> R2
	//   0x10 -> 9
	//   0x20 -> 10 (Start)
	//   0x40 -> Analógico esquerdo - L3
	//   0x80 -> Analógico direito - R3

	// Mapear as setas do joystick
	input_report_key(dev, KEY_UP,     (data[5] & 0x0F) == 0);
	input_report_key(dev, KEY_RIGHT,  (data[5] & 0x0F) == 2);
	input_report_key(dev, KEY_DOWN,   (data[5] & 0x0F) == 4);
	input_report_key(dev, KEY_LEFT,   (data[5] & 0x0F) == 6);
	// Mapear os botões numéricos do joystick
	input_report_key(dev, KEY_1,   data[5] & 0x10);
	input_report_key(dev, KEY_2,   data[5] & 0x20);
	input_report_key(dev, KEY_3,   data[5] & 0x40);
	input_report_key(dev, KEY_4,   data[5] & 0x80);
	// Mapear coisas aleatórias
	input_report_key(dev, KEY_ENTER,     data[6] & 0x01);
	input_report_key(dev, KEY_BACKSPACE, data[6] & 0x02);
	input_report_key(dev, KEY_9,         data[6] & 0x10);
	input_report_key(dev, KEY_0,         data[6] & 0x20);
	// Mapear joystick para funcionalidade de mouse
	input_report_key(dev, BTN_LEFT,   data[6] & 0x04);
	input_report_key(dev, BTN_RIGHT,  data[6] & 0x08);
	input_report_rel(dev, REL_X,      (data[3] - 128) / 32);
	input_report_rel(dev, REL_Y,      (data[4] - 128) / 32);
	input_report_rel(dev, REL_WHEEL,  (data[1] - 128) / 64 * -1);



	// Informa aos que recebem os eventos gerados por este driver que
	// enviamos um informe completo.
	// https://www.kernel.org/doc/html/latest/input/input-programming.html
	input_sync(dev);
resubmit:
	status = usb_submit_urb (urb, GFP_ATOMIC);
	if (status)
		dev_err(&mouse->usbdev->dev,
			"can't resubmit intr, %s-%s/input0, status %d\n",
			mouse->usbdev->bus->bus_name,
			mouse->usbdev->devpath, status);
}

static int usb_mouse_open(struct input_dev *dev)
{
	struct usb_mouse *mouse = input_get_drvdata(dev);

	mouse->irq->dev = mouse->usbdev;
	if (usb_submit_urb(mouse->irq, GFP_KERNEL))
		return -EIO;

	return 0;
}

static void usb_mouse_close(struct input_dev *dev)
{
	struct usb_mouse *mouse = input_get_drvdata(dev);

	usb_kill_urb(mouse->irq);
}

// Essa função é chamada quando um dispositivo que corresponde à informação
// fornecida em .id_table é visto.
// Ver: https://www.kernel.org/doc/htmldocs/writing_usb_driver/basics.html
static int usb_mouse_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_host_interface *interface;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_mouse *mouse;
	struct input_dev *input_dev;
	int pipe, maxp;
	int error = -ENOMEM;

	interface = intf->cur_altsetting;

	if (interface->desc.bNumEndpoints != 2)
		return -ENODEV;

	endpoint = &interface->endpoint[0].desc;
	if (!usb_endpoint_is_int_in(endpoint))
		return -ENODEV;

	pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

	mouse = kzalloc(sizeof(struct usb_mouse), GFP_KERNEL);
	// https://www.kernel.org/doc/html/latest/driver-api/input.html
	input_dev = input_allocate_device();
	if (!mouse || !input_dev)
		goto fail1;

	mouse->data = usb_alloc_coherent(dev, 8, GFP_KERNEL, &mouse->data_dma);
	if (!mouse->data)
		goto fail1;

	mouse->irq = usb_alloc_urb(0, GFP_KERNEL);
	if (!mouse->irq)
		goto fail2;

	mouse->usbdev = dev;
	mouse->dev = input_dev;

	if (dev->manufacturer)
		strlcpy(mouse->name, dev->manufacturer, sizeof(mouse->name));

	if (dev->product) {
		if (dev->manufacturer)
			strlcat(mouse->name, " ", sizeof(mouse->name));
		strlcat(mouse->name, dev->product, sizeof(mouse->name));
	}

	if (!strlen(mouse->name))
		snprintf(mouse->name, sizeof(mouse->name),
			 "USB HIDBP Mouse %04x:%04x",
			 le16_to_cpu(dev->descriptor.idVendor),
			 le16_to_cpu(dev->descriptor.idProduct));

	usb_make_path(dev, mouse->phys, sizeof(mouse->phys));
	strlcat(mouse->phys, "/input0", sizeof(mouse->phys));

	input_dev->name = mouse->name;
	input_dev->phys = mouse->phys;
	usb_to_input_id(dev, &input_dev->id);
	input_dev->dev.parent = &intf->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) | BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);



	set_bit(KEY_UP, input_dev->keybit);
	set_bit(KEY_LEFT, input_dev->keybit);
	set_bit(KEY_RIGHT, input_dev->keybit);
	set_bit(KEY_DOWN, input_dev->keybit);
	set_bit(KEY_ENTER, input_dev->keybit);
	set_bit(KEY_BACKSPACE, input_dev->keybit);
	set_bit(KEY_1, input_dev->keybit);
	set_bit(KEY_2, input_dev->keybit);
	set_bit(KEY_3, input_dev->keybit);
	set_bit(KEY_4, input_dev->keybit);
	set_bit(KEY_9, input_dev->keybit);
	set_bit(KEY_0, input_dev->keybit);



	input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] |= BIT_MASK(BTN_SIDE) | BIT_MASK(BTN_EXTRA);
	input_dev->relbit[0] |= BIT_MASK(REL_WHEEL);

	input_set_drvdata(input_dev, mouse);

	input_dev->open = usb_mouse_open;
	input_dev->close = usb_mouse_close;

	usb_fill_int_urb(mouse->irq, dev, pipe, mouse->data,
			 (maxp > 8 ? 8 : maxp),
			 usb_mouse_irq, mouse, endpoint->bInterval);
	mouse->irq->transfer_dma = mouse->data_dma;
	mouse->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	error = input_register_device(mouse->dev);
	if (error)
		goto fail3;

	usb_set_intfdata(intf, mouse);
	return 0;

fail3:	
	usb_free_urb(mouse->irq);
fail2:	
	usb_free_coherent(dev, 8, mouse->data, mouse->data_dma);
fail1:	
	input_free_device(input_dev);
	kfree(mouse);
	return error;
}

// Essa função é chamada quando um dispositivo que corresponde à informação
// fornecida em .id_table é removido.
// Ver: https://www.kernel.org/doc/htmldocs/writing_usb_driver/basics.html
static void usb_mouse_disconnect(struct usb_interface *intf)
{
	struct usb_mouse *mouse = usb_get_intfdata (intf);

	usb_set_intfdata(intf, NULL);
	if (mouse) {
		usb_kill_urb(mouse->irq);
		input_unregister_device(mouse->dev);
		usb_free_urb(mouse->irq);
		usb_free_coherent(interface_to_usbdev(intf), 8, mouse->data, mouse->data_dma);
		kfree(mouse);
	}
}

#define USB_VENDOR_ID      0x0079
#define USB_PRODUCT_ID     0x0006

// Tabela de dispositivos que funcionam com esse driver.
// https://www.kernel.org/doc/html/latest/driver-api/usb/hotplug.html
static struct usb_device_id usb_mouse_id_table [] = {
    { USB_DEVICE(USB_VENDOR_ID, USB_PRODUCT_ID) },
    { }
};

MODULE_DEVICE_TABLE (usb, usb_mouse_id_table);

static struct usb_driver usb_mouse_driver = {
	.name		= "ratostick",
	.probe		= usb_mouse_probe,
	.disconnect	= usb_mouse_disconnect,
	.id_table	= usb_mouse_id_table,
};

// https://www.kernel.org/doc/html/latest/driver-api/usb/usb.html
module_usb_driver(usb_mouse_driver);
