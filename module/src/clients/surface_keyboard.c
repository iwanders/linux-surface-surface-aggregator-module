// SPDX-License-Identifier: GPL-2.0+
/*
 * Surface System Aggregator Module (SSAM) legacy HID input device driver.
 *
 * Provides support for the legacy HID keyboard device found on the Surface
 * Laptop 1 and 2.
 *
 * Copyright (C) 2019-2020 Maximilian Luz <luzmaximilian@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/usb/ch9.h>

#include "../../include/linux/surface_aggregator/device.h"
#include "../../include/linux/surface_aggregator/controller.h"

#define SHID_RETRY			3
#define shid_retry(fn, args...)		ssam_retry(fn, SHID_RETRY, args)


enum surface_hid_descriptor_entry {
	SURFACE_HID_DESC_HID    = 0,
	SURFACE_HID_DESC_REPORT = 1,
	SURFACE_HID_DESC_ATTRS  = 2,
};

struct surface_hid_descriptor {
	__u8 desc_len;			// = 9
	__u8 desc_type;			// = HID_DT_HID
	__le16 hid_version;
	__u8 country_code;
	__u8 num_descriptors;		// = 1

	__u8 report_desc_type;		// = HID_DT_REPORT
	__le16 report_desc_len;
} __packed;

static_assert(sizeof(struct surface_hid_descriptor) == 9);

struct surface_hid_attributes {
	__le32 length;
	__le16 vendor;
	__le16 product;
	__le16 version;
	__u8 _unknown[22];
} __packed;

static_assert(sizeof(struct surface_hid_attributes) == 32);

enum surface_kbd_cid {
	SURFACE_KBD_CID_GET_DESCRIPTOR    = 0x00,
	SURFACE_KBD_CID_SET_CAPSLOCK_LED  = 0x01,
	SURFACE_KBD_CID_EVT_INPUT_GENERIC = 0x03,
	SURFACE_KBD_CID_EVT_INPUT_HOTKEYS = 0x04,
};

struct surface_hid_device {
	struct device *dev;
	struct ssam_controller *ctrl;
	struct ssam_device_uid uid;

	struct surface_hid_descriptor hid_desc;
	struct surface_hid_attributes attrs;
	u8 *report_desc;

	struct ssam_event_notifier notif;
	struct hid_device *hdev;
};


/* -- SAM requests. --------------------------------------------------------- */

static int kbd_load_descriptor(struct surface_hid_device *shid, u8 entry,
			       u8 *buf, size_t len)
{
	struct ssam_request rqst;
	struct ssam_response rsp;
	int status;

	rqst.target_category = shid->uid.category;
	rqst.target_id = shid->uid.target;
	rqst.command_id = SURFACE_KBD_CID_GET_DESCRIPTOR;
	rqst.instance_id = shid->uid.instance;
	rqst.flags = SSAM_REQUEST_HAS_RESPONSE;
	rqst.length = sizeof(u8);
	rqst.payload = &entry;

	rsp.capacity = len;
	rsp.length = 0;
	rsp.pointer = buf;

	status = shid_retry(ssam_request_sync, shid->ctrl, &rqst, &rsp);
	if (status)
		return status;

	if (rsp.length != len) {
		dev_err(shid->dev, "invalid descriptor length: got %zu, "
			"expected, %zu\n", rsp.length, len);
		return -EPROTO;
	}

	return 0;
}

static int kbd_set_caps_led(struct surface_hid_device *shid, bool value)
{
	struct ssam_request rqst;
	u8 value_u8 = value;

	rqst.target_category = shid->uid.category;
	rqst.target_id = shid->uid.target;
	rqst.command_id = SURFACE_KBD_CID_SET_CAPSLOCK_LED;
	rqst.instance_id = shid->uid.instance;
	rqst.flags = SSAM_REQUEST_UNSEQUENCED;
	rqst.length = sizeof(u8);
	rqst.payload = &value_u8;

	return shid_retry(ssam_request_sync, shid->ctrl, &rqst, NULL);
}


/* -- Device descriptor access. --------------------------------------------- */

static int surface_hid_load_hid_descriptor(struct surface_hid_device *shid)
{
	int status;

	status = kbd_load_descriptor(shid, SURFACE_HID_DESC_HID,
				     (u8 *)&shid->hid_desc,
				     sizeof(shid->hid_desc));
	if (status)
		return status;

	if (shid->hid_desc.desc_len != sizeof(shid->hid_desc)) {
		dev_err(shid->dev, "unexpected hid descriptor length: got %u, "
			"expected %zu\n", shid->hid_desc.desc_len,
			sizeof(shid->hid_desc));
		return -EPROTO;
	}

	if (shid->hid_desc.desc_type != HID_DT_HID) {
		dev_err(shid->dev, "unexpected hid descriptor type: got 0x%x, "
			"expected 0x%x\n", shid->hid_desc.desc_type,
			HID_DT_HID);
		return -EPROTO;
	}

	if (shid->hid_desc.num_descriptors != 1) {
		dev_err(shid->dev, "unexpected number of descriptors: got %u, "
			"expected 1\n", shid->hid_desc.num_descriptors);
		return -EPROTO;
	}

	if (shid->hid_desc.report_desc_type != HID_DT_REPORT) {
		dev_err(shid->dev, "unexpected report descriptor type: got 0x%x, "
			"expected 0x%x\n", shid->hid_desc.report_desc_type,
			HID_DT_REPORT);
		return -EPROTO;
	}

	return 0;
}

static int surface_hid_load_report_descriptor(struct surface_hid_device *shid)
{
	int status;

	shid->report_desc = kzalloc(shid->hid_desc.report_desc_len, GFP_KERNEL);
	if (!shid->report_desc)
		return -ENOMEM;

	status = kbd_load_descriptor(shid, SURFACE_HID_DESC_REPORT,
				     shid->report_desc,
				     shid->hid_desc.report_desc_len);
	if (status) {
		kfree(shid->report_desc);
		shid->report_desc = NULL;
	}

	return status;
}

static int surface_hid_load_device_attributes(struct surface_hid_device *shid)
{
	int status;

	status = kbd_load_descriptor(shid, SURFACE_HID_DESC_ATTRS,
				     (u8 *)&shid->attrs, sizeof(shid->attrs));
	if (status)
		return status;

	if (shid->attrs.length != sizeof(shid->attrs)) {
		dev_err(shid->dev, "unexpected attribute length: got %u, "
			"expected %zu\n", shid->attrs.length,
			sizeof(shid->attrs));
		return -EPROTO;
	}

	return 0;
}

static int surface_hid_load_descriptors(struct surface_hid_device *shid)
{
	int status;

	status = surface_hid_load_hid_descriptor(shid);
	if (status)
		return status;

	status = surface_hid_load_device_attributes(shid);
	if (status)
		return status;

	return surface_hid_load_report_descriptor(shid);
}

static void surface_hid_free_descriptors(struct surface_hid_device *shid)
{
	kfree(shid->report_desc);
	shid->report_desc = NULL;
}


/* -- Transport driver. ----------------------------------------------------- */

static int kbd_get_caps_led_value(struct hid_device *hdev, u8 *data, size_t len)
{
	struct hid_field *field;
	unsigned offset, size;
	int i;

	// get led field
	field = hidinput_get_led_field(hdev);
	if (!field)
		return -ENOENT;

	// check if we got the correct report
	if (len != hid_report_len(field->report))
		return -ENOENT;

	if (!field->report->id || data[0] != field->report->id)
		return -ENOENT;

	// get caps lock led index
	for (i = 0; i < field->report_count; i++)
		if ((field->usage[i].hid & 0xffff) == 0x02)
			break;

	if (i == field->report_count)
		return -ENOENT;

	// extract value
	size = field->report_size;
	offset = field->report_offset + i * size;
	return !!hid_field_extract(hdev, data + 1, size, offset);
}

static int kbd_output_report(struct surface_hid_device *shid, u8 *data, size_t len)
{
	int caps_led;

	caps_led = kbd_get_caps_led_value(shid->hdev, data, len);
	if (caps_led < 0)
		return -EIO;	// only caps output reports are supported

	return kbd_set_caps_led(shid, caps_led);
}


static bool surface_keyboard_is_input_event(const struct ssam_event *event)
{
	if (event->command_id == SURFACE_KBD_CID_EVT_INPUT_GENERIC)
		return true;

	if (event->command_id == SURFACE_KBD_CID_EVT_INPUT_HOTKEYS)
		return true;

	return false;
}

static u32 surface_keyboard_event_fn(struct ssam_event_notifier *nf,
				     const struct ssam_event *event)
{
	struct surface_hid_device *shid;
	int status;

	shid = container_of(nf, struct surface_hid_device, notif);

	/*
	 * Check against device UID manually, as registry and device target
	 * category doesn't line up.
	 */

	if (shid->uid.category != event->target_category)
		return 0;

	if (shid->uid.target != event->target_id)
		return 0;

	if (shid->uid.instance != event->instance_id)
		return 0;

	if (surface_keyboard_is_input_event(event))
		return 0;

	status = hid_input_report(shid->hdev, HID_INPUT_REPORT,
				  (u8 *)&event->data[0], event->length, 0);

	return ssam_notifier_from_errno(status) | SSAM_NOTIF_HANDLED;
}

static int surface_hid_start(struct hid_device *hdev)
{
	struct surface_hid_device *shid = hdev->driver_data;

	return ssam_notifier_register(shid->ctrl, &shid->notif);
}

static void surface_hid_stop(struct hid_device *hdev)
{
	struct surface_hid_device *shid = hdev->driver_data;

	// Note: This call will log errors for us, so ignore them here.
	ssam_notifier_unregister(shid->ctrl, &shid->notif);
}

static int surface_hid_open(struct hid_device *hdev)
{
	return 0;
}

static void surface_hid_close(struct hid_device *hdev)
{
}

static int surface_hid_parse(struct hid_device *hdev)
{
	struct surface_hid_device *shid = hdev->driver_data;

	return hid_parse_report(hdev, shid->report_desc,
				shid->hid_desc.report_desc_len);
}

static int surface_hid_raw_request(struct hid_device *hdev,
		unsigned char reportnum, u8 *buf, size_t len,
		unsigned char rtype, int reqtype)
{
	struct surface_hid_device *shid = hdev->driver_data;

	hid_info(hdev, "%s: reportnum=%d, rtype=%d, reqtype=%d\n",
		 __func__, reportnum, rtype, reqtype);

	print_hex_dump(KERN_INFO, "report: ", DUMP_PREFIX_OFFSET, 16, 1,
		       buf, len, false);

	if (rtype == HID_OUTPUT_REPORT && reqtype == HID_REQ_SET_REPORT)
		return kbd_output_report(shid, buf, len);

	return -EIO;
}

static struct hid_ll_driver surface_hid_ll_driver = {
	.start         = surface_hid_start,
	.stop          = surface_hid_stop,
	.open          = surface_hid_open,
	.close         = surface_hid_close,
	.parse         = surface_hid_parse,
	.raw_request   = surface_hid_raw_request,
};


/* -- Common device setup. -------------------------------------------------- */

static int surface_hid_device_add(struct surface_hid_device *shid)
{
	int status;

	status = surface_hid_load_descriptors(shid);
	if (status)
		return status;

	shid->hdev = hid_allocate_device();
	if (IS_ERR(shid->hdev)) {
		status = PTR_ERR(shid->hdev);
		goto err_alloc;
	}

	shid->hdev->dev.parent = shid->dev;
	shid->hdev->bus = BUS_VIRTUAL;		// TODO: BUS_SURFACE
	shid->hdev->vendor = cpu_to_le16(shid->attrs.vendor);
	shid->hdev->product = cpu_to_le16(shid->attrs.product);
	shid->hdev->version = cpu_to_le16(shid->hid_desc.hid_version);
	shid->hdev->country = shid->hid_desc.country_code;

	snprintf(shid->hdev->name, sizeof(shid->hdev->name),
		 "Microsoft Surface %04X:%04X",
		 shid->hdev->vendor, shid->hdev->product);

	strlcpy(shid->hdev->phys, dev_name(shid->dev), sizeof(shid->hdev->phys));

	shid->hdev->driver_data = shid;
	shid->hdev->ll_driver = &surface_hid_ll_driver;

	status = hid_add_device(shid->hdev);
	if (status)
		goto err_add;

	return 0;

err_add:
	hid_destroy_device(shid->hdev);
err_alloc:
	surface_hid_free_descriptors(shid);
	return status;
}

static void surface_hid_device_destroy(struct surface_hid_device *shid)
{
	hid_destroy_device(shid->hdev);
	surface_hid_free_descriptors(shid);
}


/* -- PM ops. --------------------------------------------------------------- */

#ifdef CONFIG_PM

static int surface_hid_suspend(struct device *dev)
{
	struct surface_hid_device *d = dev_get_drvdata(dev);

	if (d->hdev->driver && d->hdev->driver->suspend)
		return d->hdev->driver->suspend(d->hdev, PMSG_SUSPEND);

	return 0;
}

static int surface_hid_resume(struct device *dev)
{
	struct surface_hid_device *d = dev_get_drvdata(dev);

	if (d->hdev->driver && d->hdev->driver->resume)
		return d->hdev->driver->resume(d->hdev);

	return 0;
}

static int surface_hid_freeze(struct device *dev)
{
	struct surface_hid_device *d = dev_get_drvdata(dev);

	if (d->hdev->driver && d->hdev->driver->suspend)
		return d->hdev->driver->suspend(d->hdev, PMSG_FREEZE);

	return 0;
}

static int surface_hid_poweroff(struct device *dev)
{
	struct surface_hid_device *d = dev_get_drvdata(dev);

	if (d->hdev->driver && d->hdev->driver->suspend)
		return d->hdev->driver->suspend(d->hdev, PMSG_HIBERNATE);

	return 0;
}

static int surface_hid_restore(struct device *dev)
{
	struct surface_hid_device *d = dev_get_drvdata(dev);

	if (d->hdev->driver && d->hdev->driver->reset_resume)
		return d->hdev->driver->reset_resume(d->hdev);

	return 0;
}

struct dev_pm_ops surface_hid_pm_ops = {
	.freeze   = surface_hid_freeze,
	.thaw     = surface_hid_resume,
	.suspend  = surface_hid_suspend,
	.resume   = surface_hid_resume,
	.poweroff = surface_hid_poweroff,
	.restore  = surface_hid_restore,
};

#else /* CONFIG_PM */

struct dev_pm_ops surface_hid_pm_ops = { };

#endif /* CONFIG_PM */


/* -- Driver setup. --------------------------------------------------------- */

static int surface_keyboard_probe(struct platform_device *pdev)
{
	struct ssam_controller *ctrl;
	struct surface_hid_device *shid;
	int status;

	// add device link to EC
	status = ssam_client_bind(&pdev->dev, &ctrl);
	if (status)
		return status == -ENXIO ? -EPROBE_DEFER : status;

	shid = devm_kzalloc(&pdev->dev, sizeof(*shid), GFP_KERNEL);
	if (!shid)
		return -ENOMEM;

	shid->dev = &pdev->dev;
	shid->ctrl = ctrl;

	shid->uid.domain = SSAM_DOMAIN_SERIALHUB;
	shid->uid.category = SSAM_SSH_TC_KBD;
	shid->uid.target = 2;
	shid->uid.instance = 0;
	shid->uid.function = 0;

	shid->notif.base.priority = 1;
	shid->notif.base.fn = surface_keyboard_event_fn;
	shid->notif.event.reg = SSAM_EVENT_REGISTRY_SAM;
	shid->notif.event.id.target_category = shid->uid.category;
	shid->notif.event.id.instance = shid->uid.instance;
	shid->notif.event.mask = SSAM_EVENT_MASK_NONE;
	shid->notif.event.flags = 0;

	platform_set_drvdata(pdev, shid);
	return surface_hid_device_add(shid);
}

static int surface_keyboard_remove(struct platform_device *pdev)
{
	surface_hid_device_destroy(platform_get_drvdata(pdev));
	return 0;
}

static const struct acpi_device_id surface_keyboard_match[] = {
	{ "MSHW0096" },
	{ },
};
MODULE_DEVICE_TABLE(acpi, surface_keyboard_match);

static struct platform_driver surface_keyboard_driver = {
	.probe = surface_keyboard_probe,
	.remove = surface_keyboard_remove,
	.driver = {
		.name = "surface_keyboard",
		.acpi_match_table = surface_keyboard_match,
		.pm = &surface_hid_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(surface_keyboard_driver);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Legacy HID keyboard driver for Surface System Aggregator Module");
MODULE_LICENSE("GPL");
