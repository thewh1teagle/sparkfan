// SPDX-License-Identifier: GPL-2.0-only
/*
 * sparkfan: minimal, floor-only fan gate for NVIDIA GB10 / DGX Spark.
 *
 * The EC owns the fan curve. This module only lets root raise the fan RPM
 * *floor* (EC override slot 0x119192, inner command 5) through the SoC's
 * FF-A eSPI service (OEM command 17 + ns_shm0 shared page). It never touches
 * the cap slot, so it can never restrict cooling below the stock curve.
 *
 * sysfs (on /sys/bus/arm_ffa/devices/arm-ffa-N/ where N binds the EC eSPI
 * partition 884a63a0-3285-4120-83aa-eec008a0a546):
 *   floor      rw  "auto" | "max" | <rpm>   last acknowledged floor request
 *   caps       ro  "mode=<0|1> fan0=<min>-<max> fan1=<min>-<max>" (inner cmd 1)
 *   telemetry  ro  64 hex bytes of the EC thermal snapshot (inner cmd 7)
 *   fault      ro  0, or the errno that latched the transport (module reload clears)
 *
 * Safety gates: floor clamped to the EC-reported fan1 range, one request at a
 * time, shared mailbox must be idle, reply must echo the request, shared page
 * restored after every exchange, transport failure latches the device read-only.
 *
 * Protocol derived from 841973620/dgx-spark-fan-override (GPL-2.0).
 */
#include <linux/arm_ffa.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pfn.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>
#include "sparkfan_proto.h"

extern int pfn_is_map_memory(unsigned long pfn);

#define OEM_GENERIC_EMI		17U
#define NS_SHM_PA		0x933dd000ULL
#define NS_SHM_SIZE		0x1000U
#define SHM_IN_LEN		0x00
#define SHM_OUT_LEN		0x01
#define SHM_OUT_OFF		0x02
#define SHM_ACCEPTED		0x03
#define SHM_READY		0x04
#define SHM_DATA		0x10
#define SHM_SAVE		128U		/* bytes snapshotted/restored around a request */


#define REPLY_TIMEOUT_MS	5000U
#define REPLY_POLL_MS		10U

struct sparkfan {
	struct ffa_device *fdev;
	struct mutex lock;
	int fault;
	bool floor_known;
	u16 floor;
	bool caps_known;
	struct ec_caps caps;
};

/*
 * One EC thermal-mailbox exchange. req = inner frame [cmd, status=0, data...],
 * reply written to rsp (rsp_len bytes, starting at the outer byte).
 * Latches state->fault on transport/timeout faults only.
 */
static int ec_exchange(struct sparkfan *s, const u8 *req, u8 req_len, u8 *rsp, u8 rsp_len)
{
	struct ffa_device *fdev = s->fdev;
	struct ffa_send_direct_data2 msg = {};
	u8 snapshot[SHM_SAVE];
	u8 *shm;
	unsigned long pfn = PHYS_PFN(NS_SHM_PA);
	u32 status;
	unsigned int waited;
	int ret = 0;

	if (s->fault)
		return s->fault;
	if (req_len < 2 || rsp_len < 2 || SHM_DATA + rsp_len > SHM_SAVE || SHM_DATA + req_len > SHM_SAVE)
		return -EINVAL;
	if (pfn_is_map_memory(pfn) && !PageReserved(pfn_to_page(pfn))) {
		dev_err(&fdev->dev, "ns_shm0 page is unreserved kernel memory, refusing\n");
		return -EPERM;
	}
	shm = memremap(NS_SHM_PA, NS_SHM_SIZE, MEMREMAP_WB);
	if (!shm)
		return -ENOMEM;
	memcpy(snapshot, shm, SHM_SAVE);
	if (snapshot[SHM_ACCEPTED] || snapshot[SHM_READY]) {
		ret = -EBUSY;
		goto unmap;
	}

	memset(shm, 0, SHM_SAVE);
	shm[SHM_IN_LEN] = req_len;
	shm[SHM_OUT_LEN] = rsp_len;
	shm[SHM_OUT_OFF] = 0;
	memcpy(shm + SHM_DATA, req, req_len);
	mb();

	((u8 *)msg.data)[0] = OEM_GENERIC_EMI;
	ret = fdev->ops->msg_ops->sync_send_receive2(fdev, &msg);
	if (ret) {
		dev_crit(&fdev->dev, "FF-A transport failure %d, latching\n", ret);
		ret = ret > 0 ? -EIO : ret;
		goto fault;
	}
	status = get_unaligned_le32((u8 *)msg.data);
	if (status) {
		dev_crit(&fdev->dev, "eSPI service status %u, latching\n", status);
		ret = status == 10 ? -EBUSY : -EIO;
		goto fault;
	}
	for (waited = 0; waited < REPLY_TIMEOUT_MS; waited += REPLY_POLL_MS) {
		if (READ_ONCE(shm[SHM_READY]) == 1)
			break;
		msleep(REPLY_POLL_MS);
	}
	if (READ_ONCE(shm[SHM_READY]) != 1) {
		dev_crit(&fdev->dev, "EC reply timeout, latching\n");
		ret = -ETIMEDOUT;
		goto fault;
	}
	mb();
	memcpy(rsp, shm + SHM_DATA, rsp_len);
	if (rsp[0] != req[0] || rsp[1] != req[1])
		ret = -EPROTO;
	else if (rsp[2] != 0)
		ret = -EOPNOTSUPP;	/* status 0xff: unsupported inner command */
	memcpy(shm, snapshot, SHM_SAVE);
	mb();
	if (memcmp(shm, snapshot, SHM_SAVE)) {
		dev_crit(&fdev->dev, "shared page restore failed, latching\n");
		ret = -EUCLEAN;
		goto fault;
	}
	goto unmap;
fault:
	s->fault = ret;
unmap:
	memunmap(shm);
	return ret;
}

static int read_caps(struct sparkfan *s)
{
	u8 req[3] = { EC_OUTER_THERMAL, EC_CMD_CAPS, 0 };
	u8 rsp[13];
	int ret = ec_exchange(s, req, sizeof(req), rsp, sizeof(rsp));

	if (ret)
		return ret;
	if (parse_caps(rsp, &s->caps))
		return -EPROTO;
	s->caps_known = true;
	return 0;
}

static int set_floor(struct sparkfan *s, u16 rpm)
{
	u8 req[5], rsp[5];
	int ret;

	build_set_floor(req, rpm);
	ret = ec_exchange(s, req, sizeof(req), rsp, sizeof(rsp));
	if (ret)
		return ret;
	if (check_set_floor_reply(req, rsp))
		return -EPROTO;
	s->floor = rpm;
	s->floor_known = true;
	return 0;
}

static ssize_t floor_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct sparkfan *s = dev_get_drvdata(dev);
	ssize_t n;

	mutex_lock(&s->lock);
	if (!s->floor_known)
		n = sysfs_emit(buf, "unknown\n");
	else if (s->floor == FLOOR_AUTO)
		n = sysfs_emit(buf, "auto\n");
	else
		n = sysfs_emit(buf, "%u\n", s->floor);
	mutex_unlock(&s->lock);
	return n;
}

static ssize_t floor_store(struct device *dev, struct device_attribute *a, const char *buf, size_t count)
{
	struct sparkfan *s = dev_get_drvdata(dev);
	u16 target;
	int ret;

	if (count > 16 || memchr(buf, '\0', count))
		return -EINVAL;
	{
		char tmp[17];

		memcpy(tmp, buf, count);
		tmp[count] = '\0';
		if (parse_floor_str(tmp, &target))
			return -EINVAL;
	}

	mutex_lock(&s->lock);
	if (s->fault) {
		ret = s->fault;
		goto out;
	}
	if (!s->caps_known && (ret = read_caps(s)))
		goto out;
	if (target == 0)	/* "max" */
		target = s->caps.fan1_max ? s->caps.fan1_max : FLOOR_MAX_DEFAULT;
	target = clamp_floor(target, &s->caps);	/* gate: inside what the EC says fan1 can do */
	ret = set_floor(s, target);
	if (!ret)
		dev_info(dev, "fan floor %s\n", target == FLOOR_AUTO ? "auto" : "set");
out:
	mutex_unlock(&s->lock);
	return ret ? ret : (ssize_t)count;
}
static DEVICE_ATTR_RW(floor);

static ssize_t caps_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct sparkfan *s = dev_get_drvdata(dev);
	int ret;
	ssize_t n;

	mutex_lock(&s->lock);
	ret = s->caps_known ? 0 : read_caps(s);
	n = ret ? sysfs_emit(buf, "error %d\n", ret)
		: sysfs_emit(buf, "mode=%u fan0=%u-%u fan1=%u-%u\n", s->caps.mode,
			     s->caps.fan0_min, s->caps.fan0_max, s->caps.fan1_min, s->caps.fan1_max);
	mutex_unlock(&s->lock);
	return n;
}
static DEVICE_ATTR_RO(caps);

static ssize_t telemetry_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct sparkfan *s = dev_get_drvdata(dev);
	u8 req[3] = { EC_OUTER_THERMAL, EC_CMD_TELEMETRY, 0 };
	u8 rsp[3 + 64];
	int ret;
	ssize_t n = 0;
	unsigned int i;

	mutex_lock(&s->lock);
	ret = ec_exchange(s, req, sizeof(req), rsp, sizeof(rsp));
	mutex_unlock(&s->lock);
	if (ret)
		return sysfs_emit(buf, "error %d\n", ret);
	for (i = 3; i < sizeof(rsp); i++)
		n += sysfs_emit_at(buf, n, "%02x", rsp[i]);
	n += sysfs_emit_at(buf, n, "\n");
	return n;
}
static DEVICE_ATTR_RO(telemetry);

static ssize_t fault_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct sparkfan *s = dev_get_drvdata(dev);
	ssize_t n;

	mutex_lock(&s->lock);
	n = sysfs_emit(buf, "%d\n", s->fault);
	mutex_unlock(&s->lock);
	return n;
}
static DEVICE_ATTR_RO(fault);

static struct attribute *sparkfan_attrs[] = {
	&dev_attr_floor.attr, &dev_attr_caps.attr, &dev_attr_telemetry.attr, &dev_attr_fault.attr, NULL,
};
static const struct attribute_group sparkfan_group = { .name = "sparkfan", .attrs = sparkfan_attrs };

static int sparkfan_probe(struct ffa_device *fdev)
{
	struct sparkfan *s;

	if (!fdev->ops || !fdev->ops->msg_ops || !fdev->ops->msg_ops->sync_send_receive2)
		return -EOPNOTSUPP;
	if (fdev->mode_32bit)
		return -EOPNOTSUPP;
	s = devm_kzalloc(&fdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->fdev = fdev;
	mutex_init(&s->lock);
	dev_set_drvdata(&fdev->dev, s);
	dev_info(&fdev->dev, "sparkfan bound (partition %#x)\n", fdev->id);
	return devm_device_add_group(&fdev->dev, &sparkfan_group);
}

static void sparkfan_remove(struct ffa_device *fdev)
{
	/* Leave the EC as it is: a floor keeps cooling; nothing here can reduce it. */
}

static const struct ffa_device_id sparkfan_ids[] = {
	{ .uuid = UUID_INIT(0x884a63a0, 0x3285, 0x4120, 0x83, 0xaa, 0xee, 0xc0, 0x08, 0xa0, 0xa5, 0x46) },
	{},
};

static struct ffa_driver sparkfan_driver = {
	.name = "sparkfan",
	.probe = sparkfan_probe,
	.remove = sparkfan_remove,
	.id_table = sparkfan_ids,
};
module_ffa_driver(sparkfan_driver);

MODULE_DESCRIPTION("GB10/DGX Spark EC fan floor gate (sysfs)");
MODULE_AUTHOR("thewh1teagle");
MODULE_LICENSE("GPL");
