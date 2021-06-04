#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>

#define USB_VENDOR_ID_NZXT 0x1e71
#define USB_PRODUCT_ID_NZXT_GRID_V3 0x1711
#define USB_PRODUCT_ID_NZXT_SMART_DEVICE_V1 0x1714

#define MAX_CHANNELS 6

enum input_report_id {
	INPUT_REPORT_ID_STATUS = 0x4,
};

enum fan_type {
	FAN_TYPE_INVALID = -1,
	FAN_TYPE_NONE = 0,
	FAN_TYPE_DC = 1,
	FAN_TYPE_PWM = 2,
};

struct status_report {
	uint8_t report_id; /* INPUT_REPORT_ID_STATUS = 0x4 */
	uint8_t unknown1[2];
	__be16 rpm;
	uint8_t unknown2[2];
	uint8_t in_volt;
	uint8_t in_centivolt;
	uint8_t curr_amp;
	uint8_t curr_centiamp;
	uint8_t firmware_version_major;
	__be16 firmware_version_minor;
	uint8_t firmware_version_patch;
#ifdef __BIG_ENDIAN_BITFIELD
	uint8_t channel_index : 4;
	uint8_t fan_type : 4; /* should be one of enum fan_type */
#else
	uint8_t fan_type : 4; /* should be one of enum fan_type */
	uint8_t channel_index : 4;
#endif
	uint8_t unknown4[5];
} __attribute__((__packed__));

enum output_report_id {
	OUTPUT_REPORT_ID_INIT_COMMAND = 0x1,
	OUTPUT_REPORT_ID_CHANNEL_COMMAND = 0x2,
};

struct init_command_report {
	uint8_t report_id; /* OUTPUT_REPORT_ID_INIT_COMMAND = 0x1 */
	uint8_t command;
} __attribute__((__packed__));

static const uint8_t init_command_sequence[] = { 0x5c, 0x5d, 0x59 };

enum channel_command_id {
	CHANNEL_COMMAND_ID_SET_FAN_SPEED = 0x4d,
};

struct set_fan_speed_report {
	uint8_t report_id; /* OUTPUT_REPORT_ID_CHANNEL_COMMAND = 0x2 */
	uint8_t command; /* CHANNEL_COMMAND_ID_SET_FAN_SPEED = 0x4d */
	uint8_t channel_index;
	uint8_t unknown1;
	uint8_t fan_speed_percent;
	uint8_t padding[60];
} __attribute__((__packed__));

struct channel_status {
	enum fan_type type;
	long speed_rpm;
	long in_millivolt;
	long curr_milliamp;
};

struct drvdata {
	struct hid_device *hid;
	struct device *hwmon;
	struct channel_status channel[MAX_CHANNELS];
	rwlock_t lock;
};

static void update_channel_status(struct channel_status *status,
				  struct status_report *report)
{
	uint8_t fan_type = report->fan_type;
	switch (fan_type) {
	case FAN_TYPE_NONE:
	case FAN_TYPE_DC:
	case FAN_TYPE_PWM:
		status->type = (enum fan_type)fan_type;
		break;

	default:
		pr_warn("Invalid fan type %#x\n", fan_type);
		status->type = FAN_TYPE_INVALID;
	}

	status->speed_rpm = get_unaligned_be16(&report->rpm);
	status->in_millivolt =
		report->in_volt * 1000L + report->in_centivolt * 10L;
	status->curr_milliamp =
		report->curr_amp * 1000L + report->curr_centiamp * 10L;
}

static struct channel_status *get_channel_status(struct drvdata *drvdata,
						 int channel_index)
{
	if (channel_index < 0 || channel_index >= MAX_CHANNELS) {
		pr_warn("Invalid channel index %d\n", channel_index);
		return NULL;
	}

	return &drvdata->channel[channel_index];
}

static void update_status(struct drvdata *drvdata, struct status_report *report)
{
	struct channel_status *channel_status =
		get_channel_status(drvdata, report->channel_index);

	if (channel_status) {
		unsigned long irq_flags;
		write_lock_irqsave(&drvdata->lock, irq_flags);
		update_channel_status(channel_status, report);
		write_unlock_irqrestore(&drvdata->lock, irq_flags);
	}
}

static int send_init_commands(struct drvdata *drvdata)
{
	struct init_command_report *report =
		kzalloc(sizeof(struct init_command_report), GFP_KERNEL);
	int i;

	if (!report)
		return -ENOMEM;

	report->report_id = OUTPUT_REPORT_ID_INIT_COMMAND;

	for (i = 0; i < ARRAY_SIZE(init_command_sequence); i++) {
		int ret;
		report->command = init_command_sequence[i];
		ret = hid_hw_output_report(drvdata->hid, (void *)report,
					   sizeof(*report));
		if (ret < 0) {
			pr_warn("Failed to send init command: %d\n", ret);
			kfree(report);
			return ret;
		}
	}

	kfree(report);
	return 0;
}

static umode_t hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_input)
		return S_IWUSR;

	return S_IRUGO;
}

static int hwmon_read_fan(struct channel_status *channel_status, u32 attr,
			  long *val)
{
	switch (attr) {
	case hwmon_fan_enable:
		*val = (channel_status->type == FAN_TYPE_NONE) ? 0 : 1;
		return 0;

	case hwmon_fan_input:
		*val = channel_status->speed_rpm;
		return 0;

	default:
		return -EINVAL;
	}
}

static int hwmon_read_pwm(struct channel_status *channel_status, u32 attr,
			  long *val)
{
	switch (attr) {
	case hwmon_pwm_enable:
		*val = (channel_status->type == FAN_TYPE_NONE) ? 0 : 1;
		return 0;

	case hwmon_pwm_mode:
		*val = (channel_status->type == FAN_TYPE_PWM) ? 1 : 0;
		return 0;

	default:
		return -EINVAL;
	}
}

static int hwmon_read_in(struct channel_status *channel_status, u32 attr,
			 long *val)
{
	switch (attr) {
	case hwmon_in_enable:
		*val = (channel_status->type == FAN_TYPE_NONE) ? 0 : 1;
		return 0;

	case hwmon_in_input:
		*val = channel_status->in_millivolt;
		return 0;

	default:
		return -EINVAL;
	}
}

static int hwmon_read_curr(struct channel_status *channel_status, u32 attr,
			   long *val)
{
	switch (attr) {
	case hwmon_curr_enable:
		*val = (channel_status->type == FAN_TYPE_NONE) ? 0 : 1;
		return 0;

	case hwmon_curr_input:
		*val = channel_status->curr_milliamp;
		return 0;

	default:
		return -EINVAL;
	}
}

static int hwmon_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);
	struct channel_status *channel_status =
		get_channel_status(drvdata, channel);
	unsigned long irq_flags;
	int ret;

	if (!channel_status)
		return -EINVAL;

	read_lock_irqsave(&drvdata->lock, irq_flags);

	switch (type) {
	case hwmon_fan:
		ret = hwmon_read_fan(channel_status, attr, val);
		break;

	case hwmon_pwm:
		ret = hwmon_read_pwm(channel_status, attr, val);
		break;

	case hwmon_in:
		ret = hwmon_read_in(channel_status, attr, val);
		break;

	case hwmon_curr:
		ret = hwmon_read_curr(channel_status, attr, val);
		break;

	default:
		ret = -EINVAL;
	}

	read_unlock_irqrestore(&drvdata->lock, irq_flags);

	return ret;
}

static int hwmon_write_pwm_input(struct drvdata *drvdata, int channel, long val)
{
	struct set_fan_speed_report *report =
		kzalloc(sizeof(struct set_fan_speed_report), GFP_KERNEL);
	int ret;

	if (!report)
		return -ENOMEM;

	report->report_id = OUTPUT_REPORT_ID_CHANNEL_COMMAND;
	report->command = CHANNEL_COMMAND_ID_SET_FAN_SPEED;
	report->channel_index = channel;

	if (val >= 255)
		report->fan_speed_percent = 100;
	else if (val <= 0)
		report->fan_speed_percent = 0;
	else
		report->fan_speed_percent = val * 100 / 255;

	ret = hid_hw_output_report(drvdata->hid, (void *)report,
				   sizeof(*report));
	kfree(report);

	return ret;
}

static int hwmon_write_pwm(struct drvdata *drvdata, u32 attr, int channel,
			   long val)
{
	switch (attr) {
	case hwmon_pwm_input:
		return hwmon_write_pwm_input(drvdata, channel, val);

	default:
		return -EINVAL;
	}
}

static int hwmon_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct drvdata *drvdata = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_pwm:
		return hwmon_write_pwm(drvdata, attr, channel, val);

	default:
		return -EINVAL;
	}
}

static const struct hwmon_ops hwmon_ops = {
	.is_visible = hwmon_is_visible,
	.read = hwmon_read,
	.write = hwmon_write,
};

#define FAN_CHANNEL (HWMON_F_INPUT | HWMON_F_ENABLE)
#define PWM_CHANNEL (HWMON_PWM_MODE | HWMON_PWM_INPUT | HWMON_PWM_ENABLE)
#define IN_CHANNEL (HWMON_I_INPUT | HWMON_I_ENABLE)
#define CURR_CHANNEL (HWMON_C_INPUT | HWMON_C_ENABLE)

static const struct hwmon_channel_info *grid_v3_channel_info[] = {
	HWMON_CHANNEL_INFO(fan, FAN_CHANNEL, FAN_CHANNEL, FAN_CHANNEL,
			   FAN_CHANNEL, FAN_CHANNEL, FAN_CHANNEL),
	HWMON_CHANNEL_INFO(pwm, PWM_CHANNEL, PWM_CHANNEL, PWM_CHANNEL,
			   PWM_CHANNEL, PWM_CHANNEL, PWM_CHANNEL),
	HWMON_CHANNEL_INFO(in, IN_CHANNEL, IN_CHANNEL, IN_CHANNEL, IN_CHANNEL,
			   IN_CHANNEL, IN_CHANNEL),
	HWMON_CHANNEL_INFO(curr, CURR_CHANNEL, CURR_CHANNEL, CURR_CHANNEL,
			   CURR_CHANNEL, CURR_CHANNEL, CURR_CHANNEL),
	NULL
};

static const struct hwmon_chip_info grid_v3_chip_info = {
	.ops = &hwmon_ops,
	.info = grid_v3_channel_info,
};

static const struct hwmon_channel_info *smart_device_v1_channel_info[] = {
	HWMON_CHANNEL_INFO(fan, FAN_CHANNEL, FAN_CHANNEL, FAN_CHANNEL),
	HWMON_CHANNEL_INFO(pwm, PWM_CHANNEL, PWM_CHANNEL, PWM_CHANNEL),
	HWMON_CHANNEL_INFO(in, IN_CHANNEL, IN_CHANNEL, IN_CHANNEL),
	HWMON_CHANNEL_INFO(curr, CURR_CHANNEL, CURR_CHANNEL, CURR_CHANNEL), NULL
};

static const struct hwmon_chip_info smart_device_v1_chip_info = {
	.ops = &hwmon_ops,
	.info = smart_device_v1_channel_info,
};

enum {
	DEVICE_CONFIG_GRID_V3,
	DEVICE_CONFIG_SMART_DEVICE_V1,
	DEVICE_CONFIG_COUNT
};

static const struct hwmon_chip_info *device_configs[DEVICE_CONFIG_COUNT] = {
	[DEVICE_CONFIG_GRID_V3] = &grid_v3_chip_info,
	[DEVICE_CONFIG_SMART_DEVICE_V1] = &smart_device_v1_chip_info,
};

static const struct hid_device_id hid_id_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_PRODUCT_ID_NZXT_GRID_V3),
	  .driver_data = DEVICE_CONFIG_GRID_V3 },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT,
			 USB_PRODUCT_ID_NZXT_SMART_DEVICE_V1),
	  .driver_data = DEVICE_CONFIG_SMART_DEVICE_V1 },
	{}
};

static int hid_raw_event(struct hid_device *hdev, struct hid_report *report,
			 u8 *data, int size)
{
	struct drvdata *drvdata = hid_get_drvdata(hdev);
	uint8_t report_id = *data;

	if (report_id != INPUT_REPORT_ID_STATUS) {
		pr_warn("Unknown input report: type %#x, size %d\n", report_id,
			size);
		return 0;
	}

	if (size != sizeof(struct status_report)) {
		pr_warn("Invalid status report size %d\n", size);
		return 0;
	}

	update_status(drvdata, (struct status_report *)data);
	return 0;
}

static int hid_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct drvdata *drvdata;
	int ret;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(struct drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	rwlock_init(&drvdata->lock);

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

	ret = send_init_commands(drvdata);
	if (ret)
		goto out_hw_close;

	drvdata->hwmon =
		hwmon_device_register_with_info(&hdev->dev, "nzxtgrid", drvdata,
						device_configs[id->driver_data],
						NULL);
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

static struct hid_driver driver = {
	.name = "nzxt-grid",
	.id_table = hid_id_table,
	.probe = hid_probe,
	.remove = hid_remove,
	.raw_event = hid_raw_event,
};

static int __init nzxtgrid_init(void)
{
	return hid_register_driver(&driver);
}

static void __exit nzxtgrid_exit(void)
{
	hid_unregister_driver(&driver);
}

MODULE_DEVICE_TABLE(hid, hid_id_table);
MODULE_AUTHOR("Aleksandr Mezin <mezin.alexander@gmail.com>");
MODULE_DESCRIPTION(
	"Driver for NZXT Grid V3 fan controller/NZXT Smart Device V1");
MODULE_LICENSE("GPL");

/*
 * From corsair-cpro:
 * When compiling this driver as built-in, hwmon initcalls will get called before the
 * hid driver and this driver would fail to register. late_initcall solves this.
 */
late_initcall(nzxtgrid_init);
module_exit(nzxtgrid_exit);
