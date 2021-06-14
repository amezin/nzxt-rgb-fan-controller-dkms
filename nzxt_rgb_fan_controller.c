/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (c) 2021 Aleksandr Mezin
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>

enum { INPUT_REPORT_ID_STATUS = 0x67 };

enum {
	STATUS_REPORT_MAGIC_FAN_STATUS = 0x02,
	STATUS_REPORT_MAGIC_UNKNOWN_STATUS = 0x04
};

struct status_report_header {
	uint8_t report_id; /* INPUT_REPORT_ID_STATUS = 0x67 */
	uint8_t magic;
} __attribute__((__packed__));

struct fan_status_report {
	uint8_t report_id;
	uint8_t magic;
	uint8_t unknown1[22];
	__le16 fan_rpm[3];
	uint8_t unknown2[34];
} __attribute__((__packed__));

static const uint8_t INIT_DATA[][64] = {
	{ 0x60, 0x03 },
	{ 0x60, 0x02, 0x01, 0xe8, 0x03, 0x01, 0xe8, 0x03 }
};

struct drvdata {
	struct hid_device *hid;
	struct device *hwmon;
	uint16_t fan_rpm[3];
};

static void handle_fan_status_report(struct drvdata *drvdata, void *data,
				     int size)
{
	struct fan_status_report *report = data;
	int i;

	if (size < sizeof(struct fan_status_report)) {
		pr_warn("Fan status report size too small: %d (should be more than %zu)\n",
			size, sizeof(struct fan_status_report));
		return;
	}

	if (size > sizeof(struct fan_status_report)) {
		pr_warn("Fan status report size too large: %d (should be %zu, ignoring excess data)\n",
			size, sizeof(struct fan_status_report));
		return;
	}

	for (i = 0; i < 3; i++)
		drvdata->fan_rpm[i] = get_unaligned_le16(&report->fan_rpm[i]);
}

static void handle_status_report(struct drvdata *drvdata, void *data, int size)
{
	struct status_report_header *header = data;

	if (size < sizeof(struct status_report_header)) {
		pr_warn("Status report size too small: %d (should be more than %zu)\n",
			size, sizeof(struct status_report_header));
		return;
	}

	switch (header->magic) {
	case STATUS_REPORT_MAGIC_FAN_STATUS:
		handle_fan_status_report(drvdata, data, size);
		return;
	case STATUS_REPORT_MAGIC_UNKNOWN_STATUS:
		/* No idea what it means and how to parse it */
		return;
	default:
		pr_warn("Unexpected value of 'magic' field: %d (report size %d)\n",
			header->magic, size);
		return;
	}
}

static int hwmon_read_fan(struct drvdata *drvdata, u32 attr, int channel,
			  long *val)
{
	if (channel < 0 || channel >= ARRAY_SIZE(drvdata->fan_rpm))
		return -EINVAL;

	switch (attr) {
	case hwmon_fan_input:
		*val = drvdata->fan_rpm[channel];
		return 0;

	default:
		return -EINVAL;
	}
}

static umode_t hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	return S_IRUGO;
}

static int hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);
	int ret;

	switch (type) {
	case hwmon_fan:
		ret = hwmon_read_fan(drvdata, attr, channel, val);
		break;

	default:
		ret = -EINVAL;
	}

	return ret;
}

static const struct hwmon_ops hwmon_ops = {
	.is_visible = hwmon_is_visible,
	.read = hwmon_read,
};

static const struct hwmon_channel_info *channel_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_chip_info chip_info = {
	.ops = &hwmon_ops,
	.info = channel_info,
};

static int hid_reset_resume(struct hid_device *hdev)
{
	uint8_t *data = kmalloc(sizeof(INIT_DATA[0]), GFP_KERNEL);
	int i, ret;

	if (!data)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(INIT_DATA); i++) {
		memcpy(data, INIT_DATA[i], sizeof(INIT_DATA[i]));
		ret = hid_hw_output_report(hdev, data, sizeof(INIT_DATA[i]));

		if (ret < 0)
			break;
	}

	kfree(data);

	return (ret < 0) ? ret : 0;
}

static int hid_raw_event(struct hid_device *hdev, struct hid_report *report,
			 u8 *data, int size)
{
	struct drvdata *drvdata = hid_get_drvdata(hdev);
	uint8_t report_id = *data;

	if (report_id != INPUT_REPORT_ID_STATUS) {
		pr_warn("Unknown input report: id %#x, size %d\n", report_id,
			size);
		return 0;
	}

	handle_status_report(drvdata, data, size);
	return 0;
}

static int hid_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct drvdata *drvdata;
	int ret;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(struct drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->hid = hdev;
	hid_set_drvdata(hdev, drvdata);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto out_hw_stop;

	hid_device_io_start(hdev);

	ret = hid_reset_resume(hdev);
	if (ret)
		goto out_hw_close;

	drvdata->hwmon =
		hwmon_device_register_with_info(&hdev->dev,
						"nzxt_rgb_fan_controller",
						drvdata, &chip_info, NULL);
	if (IS_ERR(drvdata->hwmon)) {
		ret = PTR_ERR(drvdata->hwmon);
		goto out_hw_close;
	}

	return 0;

out_hw_close:
	hid_hw_close(hdev);

out_hw_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void hid_remove(struct hid_device *hdev)
{
	struct drvdata *drvdata = hid_get_drvdata(hdev);
	hwmon_device_unregister(drvdata->hwmon);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id hid_id_table[] = {
	{ HID_USB_DEVICE(0x1e71, 0x2009) },
	{}
};

static struct hid_driver hid_driver = {
	.name = "nzxt_rgb_fan_controller",
	.id_table = hid_id_table,
	.probe = hid_probe,
	.remove = hid_remove,
	.raw_event = hid_raw_event,
#ifdef CONFIG_PM
	.reset_resume = hid_reset_resume,
#endif
};

static int __init nzxtfan_init(void)
{
	return hid_register_driver(&hid_driver);
}

static void __exit nzxtfan_exit(void)
{
	hid_unregister_driver(&hid_driver);
}

MODULE_DEVICE_TABLE(hid, hid_id_table);
MODULE_AUTHOR("Aleksandr Mezin <mezin.alexander@gmail.com>");
MODULE_DESCRIPTION("Driver for NZXT RGB & Fan controller");
MODULE_LICENSE("GPL");

late_initcall(nzxtfan_init);
module_exit(nzxtfan_exit);
