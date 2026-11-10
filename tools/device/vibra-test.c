// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * vibra-test：通过 evdev 播放一次 FF_RUMBLE，验证 gpio-vibrator 马达。
 * 用法：vibra-test <event 设备> [毫秒]
 */

#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	char name[256];
	struct ff_effect effect = {
		.type = FF_RUMBLE,
		.id = -1,
		.u.rumble = {
			.strong_magnitude = 0x8000,
			.weak_magnitude = 0,
		},
	};
	struct input_event ev = {
		.type = EV_FF,
	};
	int ms = 200;
	int fd;
	int ret = 1;

	if (argc < 2) {
		fprintf(stderr, "用法: %s <event 设备> [毫秒]\n", argv[0]);
		return 2;
	}
	if (argc > 2) {
		char *end;
		long value = strtol(argv[2], &end, 10);

		/* ff_replay.length 为 16 位，超出范围会被截断成无意义的时长 */
		if (*end || value < 0 || value > 65535) {
			fprintf(stderr, "毫秒数必须在 0..65535 之间\n");
			return 2;
		}
		ms = (int)value;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}

	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
		strcpy(name, "未知设备");
	printf("设备: %s\n", name);

	effect.replay.length = (__u16)ms;
	if (ioctl(fd, EVIOCSFF, &effect) < 0) {
		perror("EVIOCSFF");
		goto out;
	}

	ev.code = (__u16)effect.id;
	ev.value = 1;
	if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
		perror("写入 EV_FF");
		goto out_erase;
	}

	usleep((useconds_t)ms * 1000 + 100000);

	ev.value = 0;
	if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
		perror("停止 EV_FF");

	printf("已播放 %d ms\n", ms);
	ret = 0;

out_erase:
	if (ioctl(fd, EVIOCRMFF, effect.id) < 0)
		perror("EVIOCRMFF");
out:
	close(fd);
	return ret;
}
