#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/module.h>

#define USB_VENDOR_ID_NZXT 0x1e71
#define USB_PRODUCT_ID_NZXT_GRID_V3 0x1711

#define NZXT_GRID_MAX_CHANNELS 6

#define NZXT_GRID_STATUS_REPORT_ID 4

struct nzxt_grid_status_report {
	uint8_t report_id;
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
	uint8_t type : 2;
	uint8_t unknown3 : 2;
	uint8_t channel : 4;
	uint8_t unknown4[5];
} __attribute__((packed));

enum nzxt_grid_fan_type {
	nzxt_grid_fan_none = 0,
	nzxt_grid_fan_dc = 1,
	nzxt_grid_fan_pwm = 2,
};

struct nzxt_grid_channel_status {
	enum nzxt_grid_fan_type type;
	long speed_rpm;
	long in_millivolt;
	long curr_milliamp;
};

struct nzxt_grid_device {
	struct hid_device *hid;
	struct device *hwmon;
	struct nzxt_grid_channel_status channel[NZXT_GRID_MAX_CHANNELS];
	rwlock_t lock;
};

static umode_t nzxt_grid_is_visible(const void *data,
				    enum hwmon_sensor_types type, u32 attr,
				    int channel)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_input)
		return S_IWUSR;

	return S_IRUGO;
}

static int
nzxt_grid_hwmon_read_fan(struct nzxt_grid_channel_status *channel_status,
			 u32 attr, long *val)
{
	switch (attr) {
	case hwmon_fan_input:
		*val = channel_status->speed_rpm;
		return 0;

	default:
		return -EINVAL;
	}
}

static int
nzxt_grid_hwmon_read_pwm(struct nzxt_grid_channel_status *channel_status,
			 u32 attr, long *val)
{
	switch (attr) {
	case hwmon_pwm_mode:
		*val = (channel_status->type == nzxt_grid_fan_pwm) ? 1 : 0;
		return 0;

	default:
		return -EINVAL;
	}
}

static int
nzxt_grid_hwmon_read_in(struct nzxt_grid_channel_status *channel_status,
			u32 attr, long *val)
{
	switch (attr) {
	case hwmon_in_input:
		*val = channel_status->in_millivolt;
		return 0;

	default:
		return -EINVAL;
	}
}

static int
nzxt_grid_hwmon_read_curr(struct nzxt_grid_channel_status *channel_status,
			  u32 attr, long *val)
{
	switch (attr) {
	case hwmon_curr_input:
		*val = channel_status->curr_milliamp;
		return 0;

	default:
		return -EINVAL;
	}
}

static int nzxt_grid_hwmon_read(struct device *dev,
				enum hwmon_sensor_types type, u32 attr,
				int channel, long *val)
{
	struct nzxt_grid_device *grid = dev_get_drvdata(dev);
	struct nzxt_grid_channel_status *channel_status =
		&grid->channel[channel];
	unsigned long irq_flags;
	int ret;

	read_lock_irqsave(&grid->lock, irq_flags);

	switch (type) {
	case hwmon_fan:
		ret = nzxt_grid_hwmon_read_fan(channel_status, attr, val);
		break;

	case hwmon_pwm:
		ret = nzxt_grid_hwmon_read_pwm(channel_status, attr, val);
		break;

	case hwmon_in:
		ret = nzxt_grid_hwmon_read_in(channel_status, attr, val);
		break;

	case hwmon_curr:
		ret = nzxt_grid_hwmon_read_curr(channel_status, attr, val);
		break;

	default:
		ret = -EINVAL;
	}

	read_unlock_irqrestore(&grid->lock, irq_flags);

	return ret;
}

static uint8_t nzxt_grid_pwm_to_percent(long hwmon_value)
{
	if (hwmon_value < 0)
		return 0;

	if (hwmon_value >= 255)
		return 100;

	return (uint8_t)(hwmon_value * 100 / 255);
}

static int nzxt_grid_hwmon_write_pwm_fixed(struct nzxt_grid_device *grid,
					   int channel, long val)
{
	uint8_t *buffer = kzalloc(65, GFP_KERNEL);
	int ret;

	if (!buffer)
		return -ENOMEM;

	buffer[0] = 0x2;
	buffer[1] = 0x4d;
	buffer[2] = (uint8_t)channel;
	buffer[4] = nzxt_grid_pwm_to_percent(val);

	ret = hid_hw_output_report(grid->hid, buffer, 65);

	kfree(buffer);

	return (ret < 0) ? ret : 0;
}

static int nzxt_grid_hwmon_write_pwm(struct nzxt_grid_device *grid, u32 attr,
				     int channel, long val)
{
	switch (attr) {
	case hwmon_pwm_input:
		return nzxt_grid_hwmon_write_pwm_fixed(grid, channel, val);

	default:
		return -EINVAL;
	}
}

static int nzxt_grid_hwmon_write(struct device *dev,
				 enum hwmon_sensor_types type, u32 attr,
				 int channel, long val)
{
	struct nzxt_grid_device *grid = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_pwm:
		return nzxt_grid_hwmon_write_pwm(grid, attr, channel, val);

	default:
		return -EINVAL;
	}
}

static const struct hwmon_ops nzxt_grid_hwmon_ops = {
	.is_visible = nzxt_grid_is_visible,
	.read = nzxt_grid_hwmon_read,
	.write = nzxt_grid_hwmon_write,
};

static const struct hwmon_channel_info *nzxt_grid_channel_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
			   HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_MODE | HWMON_PWM_INPUT,
			   HWMON_PWM_MODE | HWMON_PWM_INPUT,
			   HWMON_PWM_MODE | HWMON_PWM_INPUT,
			   HWMON_PWM_MODE | HWMON_PWM_INPUT,
			   HWMON_PWM_MODE | HWMON_PWM_INPUT,
			   HWMON_PWM_MODE | HWMON_PWM_INPUT),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT,
			   HWMON_I_INPUT, HWMON_I_INPUT, HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT, HWMON_C_INPUT, HWMON_C_INPUT,
			   HWMON_C_INPUT, HWMON_C_INPUT, HWMON_C_INPUT),
	NULL
};

static const struct hwmon_chip_info nzxt_grid_chip_info = {
	.ops = &nzxt_grid_hwmon_ops,
	.info = nzxt_grid_channel_info,
};

static int nzxt_grid_raw_event(struct hid_device *hdev,
			       struct hid_report *report, u8 *data, int size)
{
	struct nzxt_grid_device *grid = hid_get_drvdata(hdev);
	struct nzxt_grid_status_report *status_report;
	struct nzxt_grid_channel_status *channel_status;
	unsigned long irq_flags;

	if (size != sizeof(*status_report))
		return 0;

	status_report = (void *)data;

	if (status_report->channel >= NZXT_GRID_MAX_CHANNELS)
		return 0;

	write_lock_irqsave(&grid->lock, irq_flags);

	channel_status = &grid->channel[status_report->channel];

	switch (status_report->type) {
	case nzxt_grid_fan_dc:
	case nzxt_grid_fan_pwm:
		channel_status->type =
			(enum nzxt_grid_fan_type)(status_report->type);
		break;

	default:
		channel_status->type = nzxt_grid_fan_none;
	}

	channel_status->speed_rpm = be16_to_cpup(&status_report->rpm);
	channel_status->in_millivolt = status_report->in_volt * 1000L +
				       status_report->in_centivolt * 10L;
	channel_status->curr_milliamp = status_report->curr_amp * 1000L +
					status_report->curr_centiamp * 10L;

	write_unlock_irqrestore(&grid->lock, irq_flags);

	return 0;
}

static int nzxt_grid_probe(struct hid_device *hdev,
			   const struct hid_device_id *id)
{
	struct nzxt_grid_device *grid;
	int ret;

	grid = devm_kzalloc(&hdev->dev, sizeof(struct nzxt_grid_device),
			    GFP_KERNEL);
	if (!grid)
		return -ENOMEM;

	rwlock_init(&grid->lock);

	grid->hid = hdev;
	hid_set_drvdata(hdev, grid);

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

	grid->hwmon = hwmon_device_register_with_info(
		&hdev->dev, "nzxtgrid", grid, &nzxt_grid_chip_info, 0);
	if (IS_ERR(grid->hwmon)) {
		ret = PTR_ERR(grid->hwmon);
		goto out_hw_close;
	}

	return 0;

out_hw_close:
	hid_hw_close(hdev);
out_hw_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void nzxt_grid_remove(struct hid_device *hdev)
{
	struct nzxt_grid_device *grid = hid_get_drvdata(hdev);
	hwmon_device_unregister(grid->hwmon);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_report_id nzxt_grid_reports[] = {
	{ HID_REPORT_ID(NZXT_GRID_STATUS_REPORT_ID) },
	{}
};

static const struct hid_device_id nzxt_grid_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_PRODUCT_ID_NZXT_GRID_V3) },
	{}
};

static struct hid_driver nzxt_grid_driver = {
	.name = "nzxt-grid",
	.id_table = nzxt_grid_devices,
	.probe = nzxt_grid_probe,
	.remove = nzxt_grid_remove,
	.report_table = nzxt_grid_reports,
	.raw_event = nzxt_grid_raw_event,
};

static int __init nzxt_grid_init(void)
{
	return hid_register_driver(&nzxt_grid_driver);
}

static void __exit nzxt_grid_exit(void)
{
	hid_unregister_driver(&nzxt_grid_driver);
}

MODULE_DEVICE_TABLE(hid, nzxt_grid_devices);
MODULE_AUTHOR("Aleksandr Mezin <mezin.alexander@gmail.com>");
MODULE_DESCRIPTION("Driver for NZXT Grid V3 fan controller");
MODULE_LICENSE("GPL");

/*
 * From corsair-cpro:
 * When compiling this driver as built-in, hwmon initcalls will get called before the
 * hid driver and this driver would fail to register. late_initcall solves this.
 */
late_initcall(nzxt_grid_init);
module_exit(nzxt_grid_exit);
