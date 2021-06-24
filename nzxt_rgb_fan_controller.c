// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright (c) 2021 Aleksandr Mezin
 */

#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>

/*
 * The device has only 3 fan channels/connectors. But all HID reports have
 * space reserved for up to 8 channels.
 */
#define FAN_CHANNELS 3
#define FAN_CHANNELS_MAX 8

#define UPDATE_INTERVAL_PRECISION_MS 250
#define UPDATE_INTERVAL_DEFAULT_MS 1000

enum {
	INPUT_REPORT_ID_FAN_CONFIG = 0x61,
	INPUT_REPORT_ID_FAN_STATUS = 0x67,
};

enum {
	FAN_STATUS_REPORT_SPEED = 0x02,
	FAN_STATUS_REPORT_VOLTAGE = 0x04,
};

enum {
	FAN_TYPE_NONE = 0,
	FAN_TYPE_DC = 1,
	FAN_TYPE_PWM = 2,
};

struct unknown_static_data {
	/*
	 * Some configuration data? Stays the same after fan speed changes,
	 * changes in fan configuration, reboots and driver reloads.
	 *
	 * The same data in multiple report types.
	 *
	 * Byte 12 seems to be the number of fan channels, but I am not sure.
	 */
	uint8_t unknown1[14];
} __packed;

struct fan_config_report {
	/* report_id should be INPUT_REPORT_ID_FAN_CONFIG = 0x61 */
	uint8_t report_id;
	/* Always 0x03 */
	uint8_t magic;
	struct unknown_static_data unknown_data;
	/* Fan type as detected by the device. See FAN_TYPE_* enum. */
	uint8_t fan_type[FAN_CHANNELS_MAX];
} __packed;

struct fan_status_report {
	/* report_id should be INPUT_REPORT_ID_STATUS = 0x67 */
	uint8_t report_id;
	/* FAN_STATUS_REPORT_SPEED = 0x02 or FAN_STATUS_REPORT_VOLTAGE = 0x04 */
	uint8_t type;
	struct unknown_static_data unknown_data;
	/* Fan type as detected by the device. See FAN_TYPE_* enum. */
	uint8_t fan_type[FAN_CHANNELS_MAX];

	union {
		/* When type == FAN_STATUS_REPORT_SPEED */
		struct {
			/* Fan speed, in RPM. Zero for channels without fans connected. */
			__le16 fan_rpm[FAN_CHANNELS_MAX];
			/* Fan duty cycle, in percent. Non-zero even for channels without fans connected. */
			uint8_t duty_percent[FAN_CHANNELS_MAX];
			/* Exactly the same values as duty_percent[], non-zero for disconnected fans too. */
			uint8_t duty_percent_dup[FAN_CHANNELS_MAX];
			/* "Case Noise" in db */
			uint8_t noise_db;
		} __packed fan_speed;
		/* When type == FAN_STATUS_REPORT_VOLTAGE */
		struct {
			/* Voltage, in millivolts. Non-zero even when fan is not connected */
			__le16 fan_in[FAN_CHANNELS_MAX];
			/* Current, in milliamperes. Near-zero when disconnected */
			__le16 fan_current[FAN_CHANNELS_MAX];
		} __packed fan_voltage;
	} __packed;
} __packed;

#define OUTPUT_REPORT_SIZE 64

enum {
	OUTPUT_REPORT_ID_INIT_COMMAND = 0x60,
	OUTPUT_REPORT_ID_SET_FAN_SPEED = 0x62,
};

enum {
	INIT_COMMAND_SET_UPDATE_INTERVAL = 0x02,
	INIT_COMMAND_DETECT_FANS = 0x03,
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
} __packed;

struct drvdata {
	struct hid_device *hid;
	struct device *hwmon;

	uint8_t fan_duty_percent[FAN_CHANNELS];
	uint16_t fan_rpm[FAN_CHANNELS];
	bool pwm_status_received;

	uint16_t fan_in[FAN_CHANNELS];
	uint16_t fan_curr[FAN_CHANNELS];
	bool voltage_status_received;

	uint8_t fan_type[FAN_CHANNELS];
	bool fan_config_received;

	wait_queue_head_t wq;

	long update_interval;
	struct mutex update_interval_mutex;
};

static long scale_pwm_value(long val, long orig_max, long new_max)
{
	if (val <= 0)
		return 0;

	if (val >= orig_max)
		return new_max;

	val *= new_max;

	if ((val % orig_max) * 2 >= orig_max)
		return val / orig_max + 1;

	/*
	 * Non-zero values should not become zero: 0 completely turns off the
	 * fan
	 */
	return max(val / orig_max, 1L);
}

static void handle_fan_config_report(struct drvdata *drvdata, void *data,
				     int size)
{
	struct fan_config_report *report = data;
	int i;

	if (size < sizeof(struct fan_config_report))
		return;

	if (report->magic != 0x03)
		return;

	spin_lock(&drvdata->wq.lock);

	for (i = 0; i < FAN_CHANNELS; i++)
		drvdata->fan_type[i] = report->fan_type[i];

	if (!drvdata->fan_config_received) {
		drvdata->fan_config_received = true;
		wake_up_all_locked(&drvdata->wq);
	}

	spin_unlock(&drvdata->wq.lock);
}

static void handle_fan_status_report(struct drvdata *drvdata, void *data,
				     int size)
{
	struct fan_status_report *report = data;
	int i;

	if (size < sizeof(struct fan_status_report))
		return;

	spin_lock(&drvdata->wq.lock);

	/*
	 * The device sends INPUT_REPORT_ID_FAN_CONFIG = 0x61 report in response
	 * to "detect fans" command. Only accept other data after getting 0x61,
	 * to make sure that fan detection is complete and the data is not stale.
	 */
	if (!drvdata->fan_config_received) {
		spin_unlock(&drvdata->wq.lock);
		return;
	}

	switch (report->type) {
	case FAN_STATUS_REPORT_SPEED:
		for (i = 0; i < FAN_CHANNELS; i++) {
			drvdata->fan_type[i] = report->fan_type[i];
			drvdata->fan_rpm[i] = get_unaligned_le16(
				&report->fan_speed.fan_rpm[i]);
			drvdata->fan_duty_percent[i] =
				report->fan_speed.duty_percent[i];
		}

		if (!drvdata->pwm_status_received) {
			drvdata->pwm_status_received = true;
			wake_up_all_locked(&drvdata->wq);
		}
		break;

	case FAN_STATUS_REPORT_VOLTAGE:
		for (i = 0; i < FAN_CHANNELS; i++) {
			drvdata->fan_type[i] = report->fan_type[i];
			drvdata->fan_in[i] = get_unaligned_le16(
				&report->fan_voltage.fan_in[i]);
			drvdata->fan_curr[i] = get_unaligned_le16(
				&report->fan_voltage.fan_current[i]);
		}

		if (!drvdata->voltage_status_received) {
			drvdata->voltage_status_received = true;
			wake_up_all_locked(&drvdata->wq);
		}
		break;
	}

	spin_unlock(&drvdata->wq.lock);
}

static umode_t hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;

		default:
			return 0444;
		}

	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return 0644;

		default:
			return 0444;
		}

	default:
		return 0444;
	}
}

static int hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);
	int res;

	if (type == hwmon_chip) {
		switch (attr) {
		case hwmon_chip_update_interval:
			*val = drvdata->update_interval;
			return 0;

		default:
			BUG();
		}
	}

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_input:
			BUG_ON(channel >= ARRAY_SIZE(drvdata->fan_rpm));

			spin_lock_irq(&drvdata->wq.lock);
			res = wait_event_interruptible_locked_irq(
				drvdata->wq, drvdata->pwm_status_received);

			if (res == 0)
				*val = drvdata->fan_rpm[channel];

			spin_unlock_irq(&drvdata->wq.lock);
			return res;

		default:
			BUG();
		}

	case hwmon_pwm:
		/*
		 * fancontrol:
		 * 1) remembers pwm* values when it starts
		 * 2) needs pwm*_enable to be 1 on controlled fans
		 * So make sure we have correct data before allowing pwm* reads.
		 */
		switch (attr) {
		case hwmon_pwm_enable:
			BUG_ON(channel >= ARRAY_SIZE(drvdata->fan_type));

			spin_lock_irq(&drvdata->wq.lock);
			res = wait_event_interruptible_locked_irq(
				drvdata->wq, drvdata->fan_config_received);

			if (res == 0)
				*val = drvdata->fan_type[channel] !=
				       FAN_TYPE_NONE;

			spin_unlock_irq(&drvdata->wq.lock);
			return res;

		case hwmon_pwm_mode:
			BUG_ON(channel >= ARRAY_SIZE(drvdata->fan_type));

			spin_lock_irq(&drvdata->wq.lock);
			res = wait_event_interruptible_locked_irq(
				drvdata->wq, drvdata->fan_config_received);

			if (res == 0)
				*val = drvdata->fan_type[channel] ==
				       FAN_TYPE_PWM;

			spin_unlock_irq(&drvdata->wq.lock);
			return res;

		case hwmon_pwm_input:
			BUG_ON(channel >=
			       ARRAY_SIZE(drvdata->fan_duty_percent));

			spin_lock_irq(&drvdata->wq.lock);
			res = wait_event_interruptible_locked_irq(
				drvdata->wq, drvdata->pwm_status_received);

			if (res == 0)
				*val = scale_pwm_value(
					drvdata->fan_duty_percent[channel], 100,
					255);

			spin_unlock_irq(&drvdata->wq.lock);
			return res;

		default:
			BUG();
		}

	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			BUG_ON(channel >= ARRAY_SIZE(drvdata->fan_in));

			spin_lock_irq(&drvdata->wq.lock);
			res = wait_event_interruptible_locked_irq(
				drvdata->wq, drvdata->voltage_status_received);

			if (res == 0)
				*val = drvdata->fan_in[channel];

			spin_unlock_irq(&drvdata->wq.lock);
			return res;

		default:
			BUG();
		}

	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			BUG_ON(channel >= ARRAY_SIZE(drvdata->fan_curr));

			spin_lock_irq(&drvdata->wq.lock);
			res = wait_event_interruptible_locked_irq(
				drvdata->wq, drvdata->voltage_status_received);

			if (res == 0)
				*val = drvdata->fan_curr[channel];

			spin_unlock_irq(&drvdata->wq.lock);
			return res;

		default:
			BUG();
		}

	default:
		BUG();
	}
}

static int send_output_report(struct hid_device *hdev, const void *data,
			      size_t data_size)
{
	void *buffer;
	int ret;

	if (data_size > OUTPUT_REPORT_SIZE)
		return -EINVAL;

	buffer = kzalloc(OUTPUT_REPORT_SIZE, GFP_KERNEL);

	if (!buffer)
		return -ENOMEM;

	memcpy(buffer, data, data_size);
	ret = hid_hw_output_report(hdev, buffer, OUTPUT_REPORT_SIZE);
	kfree(buffer);
	return ret < 0 ? ret : 0;
}

static int set_pwm(struct drvdata *drvdata, int channel, long val)
{
	int ret;
	uint8_t duty_percent = scale_pwm_value(val, 255, 100);

	struct set_fan_speed_report report = {
		.report_id = OUTPUT_REPORT_ID_SET_FAN_SPEED,
		.magic = 1,
		.channel_bit_mask = 1 << channel
	};

	BUG_ON(channel >= ARRAY_SIZE(report.duty_percent));
	report.duty_percent[channel] = duty_percent;
	ret = send_output_report(drvdata->hid, &report, sizeof(report));

	if (ret == 0) {
		/*
		 * pwmconfig and fancontrol scripts expect pwm writes to take
		 * effect immediately (i. e. read from pwm* sysfs should return
		 * the value written into it). The device seems to always
		 * accept pwm values - even when there is no fan connected - so
		 * update pwm status without waiting for a report, to make
		 * pwmconfig and fancontrol happy.
		 *
		 * This avoids "fan stuck" messages from pwmconfig, and
		 * fancontrol setting fan speed to 100% during shutdown.
		 */
		drvdata->fan_duty_percent[channel] = duty_percent;
	}

	return ret;
}

/*
 * Workaround for fancontrol/pwmconfig trying to write to pwm*_enable even if it
 * already is 1.
 */
static int set_pwm_enable(struct drvdata *drvdata, int channel, long val)
{
	long expected_val;
	int res;

	BUG_ON(channel >= ARRAY_SIZE(drvdata->fan_type));

	spin_lock_irq(&drvdata->wq.lock);

	res = wait_event_interruptible_locked_irq(drvdata->wq,
						  drvdata->fan_config_received);

	if (res == 0)
		expected_val = drvdata->fan_type[channel] != FAN_TYPE_NONE;

	spin_unlock_irq(&drvdata->wq.lock);

	if (res)
		return res;

	return (val == expected_val) ? 0 : -ENOTSUPP;
}

static int set_update_interval(struct drvdata *drvdata, long val)
{
	uint8_t val_transformed =
		clamp_val(val / UPDATE_INTERVAL_PRECISION_MS - 1, 0, 255);
	uint8_t report[] = {
		OUTPUT_REPORT_ID_INIT_COMMAND,
		INIT_COMMAND_SET_UPDATE_INTERVAL,
		0x01,
		0xe8,
		val_transformed,
		0x01,
		0xe8,
		val_transformed,
	};

	int ret;

	ret = mutex_lock_interruptible(&drvdata->update_interval_mutex);
	if (ret)
		return ret;

	ret = send_output_report(drvdata->hid, report, sizeof(report));
	if (ret == 0)
		drvdata->update_interval =
			(val_transformed + 1) * UPDATE_INTERVAL_PRECISION_MS;

	mutex_unlock(&drvdata->update_interval_mutex);

	return ret;
}

static int detect_fans(struct hid_device *hdev)
{
	uint8_t report[] = {
		OUTPUT_REPORT_ID_INIT_COMMAND,
		INIT_COMMAND_DETECT_FANS,
	};

	return send_output_report(hdev, report, sizeof(report));
}

static int init_device(struct drvdata *drvdata, long update_interval)
{
	int ret;

	spin_lock_bh(&drvdata->wq.lock);
	drvdata->fan_config_received = false;
	drvdata->pwm_status_received = false;
	drvdata->voltage_status_received = false;
	spin_unlock_bh(&drvdata->wq.lock);

	ret = detect_fans(drvdata->hid);
	if (ret)
		return ret;

	return set_update_interval(drvdata, update_interval);
}

static int hwmon_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			return set_pwm_enable(drvdata, channel, val);

		case hwmon_pwm_input:
			return set_pwm(drvdata, channel, val);

		default:
			BUG();
		}

	case hwmon_chip:
		switch (attr) {
		case hwmon_chip_update_interval:
			return set_update_interval(drvdata, val);

		default:
			BUG();
		}

	default:
		BUG();
	}
}

static const struct hwmon_ops hwmon_ops = {
	.is_visible = hwmon_is_visible,
	.read = hwmon_read,
	.write = hwmon_write,
};

static const struct hwmon_channel_info *channel_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_MODE | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT, HWMON_C_INPUT, HWMON_C_INPUT),
	HWMON_CHANNEL_INFO(chip, HWMON_C_UPDATE_INTERVAL),
	NULL
};

static const struct hwmon_chip_info chip_info = {
	.ops = &hwmon_ops,
	.info = channel_info,
};

static int hid_raw_event(struct hid_device *hdev, struct hid_report *report,
			 u8 *data, int size)
{
	struct drvdata *drvdata = hid_get_drvdata(hdev);
	uint8_t report_id = *data;

	switch (report_id) {
	case INPUT_REPORT_ID_FAN_CONFIG:
		handle_fan_config_report(drvdata, data, size);
		break;

	case INPUT_REPORT_ID_FAN_STATUS:
		handle_fan_status_report(drvdata, data, size);
		break;
	}

	return 0;
}

static int hid_reset_resume(struct hid_device *hdev)
{
	struct drvdata *drvdata = hid_get_drvdata(hdev);

	return init_device(drvdata, drvdata->update_interval);
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
	init_waitqueue_head(&drvdata->wq);
	mutex_init(&drvdata->update_interval_mutex);

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

	init_device(drvdata, UPDATE_INTERVAL_DEFAULT_MS);

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
	{},
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
