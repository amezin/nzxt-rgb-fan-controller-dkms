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

/*
 * The device has only 3 fan channels/connectors. But all HID reports have
 * space reserved for up to 8 channels.
 */
#define FAN_CHANNELS 3
#define FAN_CHANNELS_MAX 8

enum { INPUT_REPORT_ID_FAN_STATUS = 0x67 };

enum { FAN_STATUS_REPORT_SPEED = 0x02, FAN_STATUS_REPORT_VOLTAGE = 0x04 };

enum { FAN_TYPE_NONE = 0, FAN_TYPE_DC = 1, FAN_TYPE_PWM = 2 };

struct fan_status_report {
	/* report_id should be INPUT_REPORT_ID_STATUS = 0x67 */
	uint8_t report_id;
	/* FAN_STATUS_REPORT_SPEED = 0x02 or FAN_STATUS_REPORT_VOLTAGE = 0x04 */
	uint8_t type;
	/* Some configuration data? Doesn't change with fan speed. Same for both 'type' values. */
	uint8_t unknown1[14];
	/* Fan type as detected by the device. See FAN_TYPE_* enum. */
	uint8_t fan_type[FAN_CHANNELS_MAX];

	union {
		struct {
			/* Fan speed, in RPM. Zero for channels without fans connected. */
			__le16 fan_rpm[FAN_CHANNELS_MAX];
			/* Fan duty cycle, in percent. Non-zero even for channels without fans connected. */
			uint8_t duty_percent[FAN_CHANNELS_MAX];
			/* Exactly the same values as duty_percent[], non-zero for disconnected fans too. */
			uint8_t duty_percent_dup[FAN_CHANNELS_MAX];
			/* "Case Noise" in db */
			uint8_t noise_db;
			uint8_t zero_padding4[7];
		} __attribute__((__packed__)) fan_speed;
		struct {
			/* Voltage, in millivolts. Non-zero even when fan is not connected */
			__le16 fan_in[FAN_CHANNELS_MAX];
			/* Current, in milliamperes. Near-zero when disconnected */
			__le16 fan_current[FAN_CHANNELS_MAX];
			uint8_t zero_padding2[8];
		} __attribute__((__packed__)) fan_voltage;
	} __attribute__((__packed__));
} __attribute__((__packed__));

enum {
	OUTPUT_REPORT_ID_INIT_COMMAND = 0x60,
	OUTPUT_REPORT_ID_SET_FAN_SPEED = 0x62,
};

struct set_fan_speed_report {
	/* report_id should be OUTPUT_REPORT_ID_SET_FAN_SPEED = 0x62 */
	uint8_t report_id;
	/* Should be 0x01 */
	uint8_t magic;
	/* To change fan speed on i-th channel, set i-th bit here */
	uint8_t channel_bit_mask;
	/* Fan duty cycle/target speed in percent */
	uint8_t duty_percent[FAN_CHANNELS_MAX];
	uint8_t zero_padding[50];
} __attribute__((__packed__));

static const uint8_t INIT_DATA[][64] = {
	{ OUTPUT_REPORT_ID_INIT_COMMAND, 0x03 },
	{ OUTPUT_REPORT_ID_INIT_COMMAND, 0x02, 0x01, 0xe8, 0x03, 0x01, 0xe8,
	  0x03 }
};

struct fan_channel_status {
	uint8_t type;
	uint8_t duty_percent;
	uint16_t rpm;
	uint16_t in;
	uint16_t curr;
};

struct drvdata {
	struct hid_device *hid;
	struct device *hwmon;
	struct fan_channel_status fan[FAN_CHANNELS];
};

static void handle_fan_status_report(struct drvdata *drvdata, void *data,
				     int size)
{
	struct fan_status_report *report = data;
	int i;

	if (size != sizeof(struct fan_status_report)) {
		pr_warn("Status report size is wrong: %d (should be %zu)\n",
			size, sizeof(struct fan_status_report));
		return;
	}

	switch (report->type) {
	case FAN_STATUS_REPORT_SPEED:
		for (i = 0; i < FAN_CHANNELS; i++) {
			struct fan_channel_status *fan = &drvdata->fan[i];
			fan->type = report->fan_type[i];
			fan->rpm = get_unaligned_le16(
				&report->fan_speed.fan_rpm[i]);
			fan->duty_percent = report->fan_speed.duty_percent[i];
		}
		return;
	case FAN_STATUS_REPORT_VOLTAGE:
		for (i = 0; i < FAN_CHANNELS; i++) {
			struct fan_channel_status *fan = &drvdata->fan[i];
			fan->type = report->fan_type[i];
			fan->in = get_unaligned_le16(
				&report->fan_voltage.fan_in[i]);
			fan->curr = get_unaligned_le16(
				&report->fan_voltage.fan_current[i]);
		}
		return;
	default:
		pr_warn("Unexpected value of 'type' field: %d (report size %d)\n",
			report->type, size);
		return;
	}
}

static umode_t hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_input)
		return S_IRUGO | S_IWUSR;

	return S_IRUGO;
}

static int hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);
	struct fan_channel_status *fan = &drvdata->fan[channel];

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_enable:
			*val = fan->type != FAN_TYPE_NONE;
			return 0;

		case hwmon_fan_input:
			if (fan->type == FAN_TYPE_NONE)
				return -ENODATA;

			*val = fan->rpm;
			return 0;

		default:
			return -EINVAL;
		}

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			*val = fan->type != FAN_TYPE_NONE;
			return 0;

		case hwmon_pwm_mode:
			*val = fan->type == FAN_TYPE_PWM;
			return 0;

		case hwmon_pwm_input:
			if (fan->type == FAN_TYPE_NONE)
				return -ENODATA;

			*val = fan->duty_percent * 255 / 100;
			return 0;

		default:
			return -EINVAL;
		}

	case hwmon_in:
		switch (attr) {
		case hwmon_in_enable:
			*val = fan->type != FAN_TYPE_NONE;
			return 0;

		case hwmon_in_input:
			if (fan->type == FAN_TYPE_NONE)
				return -ENODATA;

			*val = fan->in;
			return 0;

		default:
			return -EINVAL;
		}

	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_enable:
			*val = fan->type != FAN_TYPE_NONE;
			return 0;

		case hwmon_curr_input:
			if (fan->type == FAN_TYPE_NONE)
				return -ENODATA;

			*val = fan->curr;
			return 0;

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

static int hwmon_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);
	struct set_fan_speed_report *report;
	int ret;

	if (type != hwmon_pwm || attr != hwmon_pwm_input)
		return -EINVAL;

	report = kzalloc(sizeof(struct set_fan_speed_report), GFP_KERNEL);
	if (!report)
		return -ENOMEM;

	report->report_id = OUTPUT_REPORT_ID_SET_FAN_SPEED;
	report->magic = 1;
	report->channel_bit_mask = 1 << channel;
	report->duty_percent[channel] = val * 100 / 255;

	ret = hid_hw_output_report(drvdata->hid, (void *)report,
				   sizeof(*report));

	kfree(report);

	return ret;
}

static const struct hwmon_ops hwmon_ops = {
	.is_visible = hwmon_is_visible,
	.read = hwmon_read,
	.write = hwmon_write,
};

static const struct hwmon_channel_info *channel_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_ENABLE,
			   HWMON_F_INPUT | HWMON_F_ENABLE,
			   HWMON_F_INPUT | HWMON_F_ENABLE),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_ENABLE,
			   HWMON_I_INPUT | HWMON_I_ENABLE,
			   HWMON_I_INPUT | HWMON_I_ENABLE),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_ENABLE,
			   HWMON_C_INPUT | HWMON_C_ENABLE,
			   HWMON_C_INPUT | HWMON_C_ENABLE),
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

	if (report_id != INPUT_REPORT_ID_FAN_STATUS) {
		pr_warn("Unknown input report: id %#x, size %d\n", report_id,
			size);
		return 0;
	}

	handle_fan_status_report(drvdata, data, size);
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

static const struct hid_device_id hid_id_table[] = { { HID_USB_DEVICE(0x1e71,
								      0x2009) },
						     {} };

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

static int __init nzxt_rgb_fan_controller_init(void)
{
	return hid_register_driver(&hid_driver);
}

static void __exit nzxt_rgb_fan_controller_exit(void)
{
	hid_unregister_driver(&hid_driver);
}

MODULE_DEVICE_TABLE(hid, hid_id_table);
MODULE_AUTHOR("Aleksandr Mezin <mezin.alexander@gmail.com>");
MODULE_DESCRIPTION("Driver for NZXT RGB & Fan controller");
MODULE_LICENSE("GPL");

late_initcall(nzxt_rgb_fan_controller_init);
module_exit(nzxt_rgb_fan_controller_exit);