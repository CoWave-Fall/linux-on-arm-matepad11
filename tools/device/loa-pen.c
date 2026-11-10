// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * loa-pen —— DBY-W09 M-Pencil 3 面板坐标与蓝牙 HID 合并守护进程
 *
 * 面板扫描（novatek-n9z）提供悬浮坐标与 BTN_TOOL_PEN，笔的 BLE HID
 * 报文提供压力与接触，笔身轻点由笔固件识别后从键盘节点上报 KEY_F20。
 * 两条链路分属不同子系统，本进程用 EVIOCGRAB 接管面板笔设备与笔的
 * 键盘节点，再把坐标、压力、接触与橡皮状态合成到同一个 uinput 平板
 * 设备中，保证每次 SYN_REPORT 内的状态一致。
 *
 * 笔的 BLE 输出报文（实测）：
 *   0x55 报文长度 17，四路有符号 16 位压力从偏移 3、5、7、9 开始，
 *   满量程 4095；纯悬停时通常没有报文或压力为 0。
 *
 * 事件顺序：进入时先 BTN_TOOL_PEN 后 BTN_TOUCH，离开时先 BTN_TOUCH=0
 * 后 BTN_TOOL_PEN=0。压力在面板帧内合并；笔静止时由 HID 报文触发独立
 * 的压力帧，坐标沿用缓存值，应用不会丢失位置。
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* 驱动的面板笔设备名，见 novatek-n9z-core.c */
#define STYLUS_NAME "Novatek N9Z Stylus"
/* 笔的 HOGP 键盘节点名，hid-generic 按 HID 报告描述符生成 */
#define PEN_KEYBOARD_NAME "HUAWEI M-Pencil 3 Keyboard"
/* 笔的蓝牙 HID 标识 */
#define HUAWEI_VID 0x12d1
#define PEN_PID 0x10a5
/* 合成后的 uinput 设备名 */
#define UINPUT_NAME "DBY-W09 M-Pencil"
/* 面板笔使能参数：1 表示下发 HPP3.0 使能序列 */
#define PEN_ENABLE_PATH "/sys/module/novatek_n9z/parameters/pen_enable"
/* 0x55 报文 */
#define PEN_REPORT_PRESSURE 0x55
#define PEN_PRESSURE_LEN 17
#define PEN_PRESSURE_MAX 4095
/*
 * 接触判定阈值。实测悬停报文压力为 0，书写从约 62 起；取 1 可覆盖
 * 最轻的笔画。若后续发现悬停存在小幅噪声，抬高该值即可。
 */
#define TOUCH_MIN_PRESSURE 1
/* 无 0x55 报文后判定抬笔的时间。书写报文间隔通常小于 45 ms */
#define PRESSURE_TIMEOUT_MS 150
/*
 * 笔身轻点由固件识别，一次手势会上报两组 KEY_F20（实测间隔 2..15 ms）。
 * 相同时间窗内的第二次按下视为同一次手势，只切换一次橡皮。
 */
#define TAP_COALESCE_MS 100
/* 缺失 hidraw 或键盘节点时的重试间隔 */
#define RETRY_MS 500
/* uinput 坐标分辨率回退值，单位为每毫米像素数，libinput 要求非零 */
#define AXIS_RESOLUTION 11

/*
 * 一个 SYN_REPORT 内收集到的面板笔状态。坐标直接写入 daemon 的
 * x/y，帧结束时由 emit_state() 统一合成，不再保留事件缓冲。
 */
struct panel_frame {
	bool coords;
	bool enter;
	bool leave;
};

struct daemon {
	int uinput_fd;
	int stylus_fd;
	int hidraw_fd;
	int kbd_fd;
	int x_max;
	int y_max;
	int x;
	int y;
	int pressure;
	int pressure_field;
	bool pen_active;
	bool touching;
	bool eraser;
	unsigned long last_hid_ms;
	unsigned long last_tap_ms;
	bool verbose;
	struct panel_frame frame;
};

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
	(void)sig;
	running = 0;
}

static void die(const char *what)
{
	fprintf(stderr, "loa-pen: %s: %s\n", what, strerror(errno));
	exit(EXIT_FAILURE);
}

static unsigned long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void emit(struct daemon *d, uint16_t type, uint16_t code, int value)
{
	struct input_event ev = {
		.type = type,
		.code = code,
		.value = value,
	};

	if (write(d->uinput_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		die("写入 uinput");
}

static void emit_syn(struct daemon *d)
{
	emit(d, EV_SYN, SYN_REPORT, 0);
}

/*
 * 用缓存状态发一帧。坐标可选：压力帧也带坐标，应用不会因为缺少位置
 * 而丢弃压力。
 */
static void emit_state(struct daemon *d, bool with_coords)
{
	if (!d->pen_active)
		return;

	emit(d, EV_KEY, BTN_TOOL_PEN, 1);
	if (with_coords) {
		emit(d, EV_ABS, ABS_X, d->x);
		emit(d, EV_ABS, ABS_Y, d->y);
	}
	emit(d, EV_ABS, ABS_PRESSURE, d->pressure);
	emit(d, EV_KEY, BTN_TOUCH, d->touching ? 1 : 0);
	emit(d, EV_KEY, BTN_TOOL_RUBBER, d->eraser ? 1 : 0);
	emit_syn(d);
}

/* 笔离开面板：先抬接触，再抬笔，最后回到悬浮状态 */
static void emit_leave(struct daemon *d)
{
	emit(d, EV_KEY, BTN_TOUCH, 0);
	emit(d, EV_ABS, ABS_PRESSURE, 0);
	emit(d, EV_KEY, BTN_TOOL_RUBBER, 0);
	emit(d, EV_KEY, BTN_TOOL_PEN, 0);
	emit_syn(d);

	/*
	 * 工具离开后同步复位橡皮标志。保留该标志会让下一次进入重新上报
	 * BTN_TOOL_RUBBER=1，与离开帧上报的 0 前后矛盾。
	 */
	d->eraser = false;
}

static int open_input_by_name(const char *name, bool grab, const char *what)
{
	char path[64];
	char found[256];
	int i;

	for (i = 0; i < 64; i++) {
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		memset(found, 0, sizeof(found));
		if (ioctl(fd, EVIOCGNAME(sizeof(found) - 1), found) < 0 ||
		    strcmp(found, name) != 0) {
			close(fd);
			continue;
		}

		if (grab && ioctl(fd, EVIOCGRAB, 1) < 0) {
			fprintf(stderr, "loa-pen: 独占 %s 失败: %s\n", what,
				strerror(errno));
			close(fd);
			return -1;
		}

		return fd;
	}

	return -1;
}

/*
 * 按 HID_ID 查找笔的 hidraw 节点。sysfs 的 uevent 形如
 * HID_ID=0005:000012D1:000010A5，bus、vid、pid 为十六进制。
 */
static int hidraw_matches(const char *dir)
{
	char path[512];
	char line[256];
	unsigned int bus, vid, pid;
	FILE *f;
	int matched = 0;

	snprintf(path, sizeof(path), "%s/device/uevent", dir);
	f = fopen(path, "r");
	if (!f)
		return 0;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) != 3)
			continue;
		if (bus == BUS_BLUETOOTH && vid == HUAWEI_VID &&
		    pid == PEN_PID) {
			matched = 1;
			break;
		}
	}

	fclose(f);
	return matched;
}

static int open_pen_hidraw(void)
{
	struct dirent *ent;
	DIR *dir;

	dir = opendir("/sys/class/hidraw");
	if (!dir)
		return -1;

	while ((ent = readdir(dir)) != NULL) {
		/* 目录前缀 + NAME_MAX（255）即可容纳节点名 */
		char syspath[288];
		char devpath[288];
		int fd;

		if (strncmp(ent->d_name, "hidraw", 6) != 0)
			continue;

		snprintf(syspath, sizeof(syspath), "/sys/class/hidraw/%s",
			 ent->d_name);
		if (!hidraw_matches(syspath))
			continue;

		snprintf(devpath, sizeof(devpath), "/dev/%s", ent->d_name);
		fd = open(devpath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		closedir(dir);
		return fd;
	}

	closedir(dir);
	return -1;
}

static int create_uinput(struct daemon *d)
{
	struct uinput_setup setup = {
		.id = {
			.bustype = BUS_BLUETOOTH,
			.vendor = HUAWEI_VID,
			.product = PEN_PID,
		},
		.name = UINPUT_NAME,
	};
	struct uinput_abs_setup abs_setup;
	struct input_absinfo absinfo;
	int x_resolution = AXIS_RESOLUTION;
	int y_resolution = AXIS_RESOLUTION;
	int fd;

	/*
	 * 坐标分辨率优先取面板笔设备上报的 absinfo。驱动未提供或值为 0
	 * 时回退到当前面板的常量，保证 libinput 拿到非零分辨率。
	 */
	if (ioctl(d->stylus_fd, EVIOCGABS(ABS_X), &absinfo) == 0 &&
	    absinfo.resolution > 0)
		x_resolution = absinfo.resolution;
	if (ioctl(d->stylus_fd, EVIOCGABS(ABS_Y), &absinfo) == 0 &&
	    absinfo.resolution > 0)
		y_resolution = absinfo.resolution;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		die("打开 /dev/uinput");

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN) < 0 ||
	    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
	    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_RUBBER) < 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_X) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_Y) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_PRESSURE) < 0 ||
	    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0)
		die("配置 uinput 能力");

	memset(&abs_setup, 0, sizeof(abs_setup));
	abs_setup.code = ABS_X;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = d->x_max;
	abs_setup.absinfo.resolution = x_resolution;
	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0)
		die("配置 ABS_X");

	abs_setup.code = ABS_Y;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = d->y_max;
	abs_setup.absinfo.resolution = y_resolution;
	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0)
		die("配置 ABS_Y");

	memset(&abs_setup, 0, sizeof(abs_setup));
	abs_setup.code = ABS_PRESSURE;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = PEN_PRESSURE_MAX;
	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0)
		die("配置 ABS_PRESSURE");

	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
	    ioctl(fd, UI_DEV_CREATE) < 0)
		die("创建 uinput 设备");

	return fd;
}

static void enable_panel_pen(void)
{
	int fd = open(PEN_ENABLE_PATH, O_WRONLY | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "loa-pen: 打开 %s 失败: %s\n", PEN_ENABLE_PATH,
			strerror(errno));
		return;
	}
	if (write(fd, "1", 1) != 1)
		fprintf(stderr, "loa-pen: 写入 %s 失败: %s\n", PEN_ENABLE_PATH,
			strerror(errno));
	close(fd);
}

/* 丢弃未完成的帧状态。SYN_DROPPED 之后收集到的内容已经不能代表当前状态 */
static void reset_panel_frame(struct daemon *d)
{
	d->frame.coords = false;
	d->frame.enter = false;
	d->frame.leave = false;
}

/* 面板帧结束：用当前坐标与压力合成一帧 */
static void flush_panel_frame(struct daemon *d)
{
	struct panel_frame *f = &d->frame;

	if (!f->coords && !f->enter && !f->leave)
		return;

	if (f->leave) {
		d->pen_active = false;
		d->touching = false;
		d->pressure = 0;
		emit_leave(d);
		reset_panel_frame(d);
		return;
	}

	if (f->enter)
		d->pen_active = true;

	if (d->pen_active)
		emit_state(d, true);

	reset_panel_frame(d);
}

/* 事件缓冲区溢出后按当前设备状态重新同步，避免丢失抬笔或坐标 */
static void resync_stylus(struct daemon *d)
{
	unsigned long keys[KEY_CNT / (8 * sizeof(unsigned long))];
	struct input_absinfo abs;
	bool active;

	/* 丢弃断点之前的半个帧，避免把旧事件拼到同步后的状态上 */
	reset_panel_frame(d);

	memset(keys, 0, sizeof(keys));
	active = false;
	if (ioctl(d->stylus_fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
		active = (keys[BTN_TOOL_PEN / (8 * sizeof(unsigned long))] >>
			  (BTN_TOOL_PEN % (8 * sizeof(unsigned long)))) & 1;

	if (ioctl(d->stylus_fd, EVIOCGABS(ABS_X), &abs) >= 0)
		d->x = abs.value;
	if (ioctl(d->stylus_fd, EVIOCGABS(ABS_Y), &abs) >= 0)
		d->y = abs.value;

	if (!active) {
		if (d->pen_active) {
			d->pen_active = false;
			d->pressure = 0;
			d->touching = false;
			emit_leave(d);
		}
		return;
	}

	d->pen_active = true;
	emit_state(d, true);
}

static void stylus_event(struct daemon *d, const struct input_event *ev)
{
	struct panel_frame *f = &d->frame;

	if (ev->type == EV_SYN) {
		if (ev->code == SYN_REPORT)
			flush_panel_frame(d);
		else if (ev->code == SYN_DROPPED) {
			fprintf(stderr, "loa-pen: 面板事件丢失，重新同步\n");
			resync_stylus(d);
		}
		return;
	}

	if (ev->type == EV_KEY && ev->code == BTN_TOOL_PEN) {
		if (ev->value)
			f->enter = true;
		else
			f->leave = true;
		return;
	}

	if (ev->type == EV_ABS && (ev->code == ABS_X || ev->code == ABS_Y)) {
		if (ev->code == ABS_X)
			d->x = ev->value;
		else
			d->y = ev->value;
		f->coords = true;
	}
}

static void read_stylus(struct daemon *d)
{
	struct input_event ev;

	for (;;) {
		ssize_t n = read(d->stylus_fd, &ev, sizeof(ev));

		if (n == (ssize_t)sizeof(ev)) {
			stylus_event(d, &ev);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		/* 设备消失属于致命错误，交给 systemd 重新拉起 */
		die("读取面板笔设备");
	}
}

/*
 * 压力变化立即合成一帧，笔静止时也能及时反映落笔与抬笔。
 * 值为 0 且未接触时不刷新，避免悬停期间产生多余帧。
 */
static void hid_pressure(struct daemon *d, int pressure)
{
	bool touching;

	if (pressure < 0)
		pressure = 0;
	if (pressure > PEN_PRESSURE_MAX)
		pressure = PEN_PRESSURE_MAX;
	touching = pressure >= TOUCH_MIN_PRESSURE;

	d->last_hid_ms = now_ms();
	if (pressure == d->pressure && touching == d->touching)
		return;

	d->pressure = pressure;
	d->touching = touching;
	if (d->pen_active)
		emit_state(d, true);
}

static void read_hidraw(struct daemon *d)
{
	uint8_t buf[64];
	ssize_t n;
	int offset;

	n = read(d->hidraw_fd, buf, sizeof(buf));
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		goto disconnect;
	}
	if (n == 0)
		goto disconnect;

	if (n < PEN_PRESSURE_LEN || buf[0] != PEN_REPORT_PRESSURE)
		return;

	offset = 3 + d->pressure_field * 2;
	if (offset + 1 >= n)
		return;

	hid_pressure(d, (int16_t)(buf[offset] | (buf[offset + 1] << 8)));
	if (d->verbose)
		fprintf(stderr, "loa-pen: 0x55 pressure raw %d status %02x %02x\n",
			(int16_t)(buf[offset] | (buf[offset + 1] << 8)),
			buf[11], buf[12]);
	return;

disconnect:
	fprintf(stderr, "loa-pen: 笔的 hidraw 断开\n");
	close(d->hidraw_fd);
	d->hidraw_fd = -1;
	d->last_hid_ms = 0;
	if (d->pen_active && (d->pressure || d->touching)) {
		d->pressure = 0;
		d->touching = false;
		emit_state(d, true);
	}
}

static void toggle_eraser(struct daemon *d)
{
	unsigned long now = now_ms();

	/* 一次轻点手势产生两次按下，时间窗内的第二次不再切换 */
	if (d->last_tap_ms && now - d->last_tap_ms < TAP_COALESCE_MS)
		return;
	d->last_tap_ms = now;

	d->eraser = !d->eraser;
	fprintf(stderr, "loa-pen: 橡皮 %s\n", d->eraser ? "开启" : "关闭");
	if (d->pen_active)
		emit_state(d, true);
}

static void read_keyboard(struct daemon *d)
{
	struct input_event ev;

	for (;;) {
		ssize_t n = read(d->kbd_fd, &ev, sizeof(ev));

		if (n == (ssize_t)sizeof(ev)) {
			if (ev.type == EV_KEY && ev.code == KEY_F20 &&
			    ev.value == 1)
				toggle_eraser(d);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		fprintf(stderr, "loa-pen: 笔的键盘节点断开\n");
		close(d->kbd_fd);
		d->kbd_fd = -1;
		return;
	}
}

static void check_pressure_timeout(struct daemon *d)
{
	if (!d->pen_active || !d->last_hid_ms)
		return;
	if (now_ms() - d->last_hid_ms <= PRESSURE_TIMEOUT_MS)
		return;
	if (!d->pressure && !d->touching)
		return;

	d->pressure = 0;
	d->touching = false;
	emit_state(d, true);
	if (d->verbose)
		fprintf(stderr, "loa-pen: 压力超时，抬笔\n");
}

static void open_optional_devices(struct daemon *d, unsigned long now,
				  unsigned long *next_retry)
{
	if (now < *next_retry)
		return;
	*next_retry = now + RETRY_MS;

	if (d->hidraw_fd < 0) {
		d->hidraw_fd = open_pen_hidraw();
		if (d->hidraw_fd >= 0) {
			fprintf(stderr, "loa-pen: hidraw 已连接\n");
			d->last_hid_ms = 0;
		}
	}

	if (d->kbd_fd < 0) {
		d->kbd_fd = open_input_by_name(PEN_KEYBOARD_NAME, true, "笔键盘");
		if (d->kbd_fd >= 0)
			fprintf(stderr, "loa-pen: 键盘节点已连接\n");
	}
}

int main(int argc, char **argv)
{
	struct daemon d;
	struct sigaction sa;
	unsigned long next_retry = 0;
	struct input_absinfo abs;
	int i;

	memset(&d, 0, sizeof(d));
	d.hidraw_fd = -1;
	d.kbd_fd = -1;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0) {
			d.verbose = true;
		} else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			d.pressure_field = atoi(argv[++i]);
			if (d.pressure_field < 0 || d.pressure_field > 3)
				d.pressure_field = 0;
		} else {
			fprintf(stderr, "用法: %s [-v] [-p 0..3]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	/*
	 * 面板笔设备随触摸驱动在开机时创建；缺失时持续重试，
	 * 触摸驱动重新加载后仍可恢复。
	 */
	for (;;) {
		d.stylus_fd = open_input_by_name(STYLUS_NAME, true, "面板笔");
		if (d.stylus_fd >= 0)
			break;
		fprintf(stderr, "loa-pen: 等待 %s\n", STYLUS_NAME);
		sleep(1);
	}

	if (ioctl(d.stylus_fd, EVIOCGABS(ABS_X), &abs) < 0)
		die("读取 ABS_X 量程");
	d.x_max = abs.maximum;
	if (ioctl(d.stylus_fd, EVIOCGABS(ABS_Y), &abs) < 0)
		die("读取 ABS_Y 量程");
	d.y_max = abs.maximum;

	d.uinput_fd = create_uinput(&d);
	fprintf(stderr, "loa-pen: %s %dx%d -> %s\n", STYLUS_NAME,
		d.x_max + 1, d.y_max + 1, UINPUT_NAME);

	/* 驱动按参数在 workqueue 中下发 HPP3.0 使能序列 */
	enable_panel_pen();

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	open_optional_devices(&d, now_ms(), &next_retry);

	while (running) {
		struct pollfd pfd[3];
		unsigned int nfds = 0;
		unsigned long now;
		int ret;

		pfd[nfds].fd = d.stylus_fd;
		pfd[nfds++].events = POLLIN;
		if (d.hidraw_fd >= 0) {
			pfd[nfds].fd = d.hidraw_fd;
			pfd[nfds++].events = POLLIN;
		}
		if (d.kbd_fd >= 0) {
			pfd[nfds].fd = d.kbd_fd;
			pfd[nfds++].events = POLLIN;
		}

		ret = poll(pfd, nfds, 20);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			die("poll");
		}

		for (i = 0; i < (int)nfds; i++) {
			if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			if (pfd[i].fd == d.stylus_fd)
				read_stylus(&d);
			else if (pfd[i].fd == d.hidraw_fd)
				read_hidraw(&d);
			else if (pfd[i].fd == d.kbd_fd)
				read_keyboard(&d);
		}

		now = now_ms();
		open_optional_devices(&d, now, &next_retry);
		check_pressure_timeout(&d);
	}

	ioctl(d.uinput_fd, UI_DEV_DESTROY);
	close(d.uinput_fd);
	close(d.stylus_fd);
	if (d.hidraw_fd >= 0)
		close(d.hidraw_fd);
	if (d.kbd_fd >= 0)
		close(d.kbd_fd);

	return EXIT_SUCCESS;
}
