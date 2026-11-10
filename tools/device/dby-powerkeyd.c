// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * dby-powerkeyd —— DBY-W09 电源键与音量键手势策略。
 *
 * 短按电源键：屏幕点亮时锁定会话并熄屏，屏幕关闭时点亮屏幕。
 * 按住电源键 2.5 秒：点亮屏幕并弹出 KDE 离开对话框（注销、重启、关机、
 *   睡眠）；继续按住到 7 秒执行强制重启。
 * 电源键与音量键组合：执行平板设置中绑定的命令，默认电源+音量下截屏。
 *
 * systemd-logind 的长按判定固定为 5 秒，短按动作在松手时立即执行，
 * 本机需要更细的手势划分，因此由本进程独占三颗按键的输入设备
 * （EVIOCGRAB），自行计时与分发。
 *
 * 音量键被独占后桌面收不到音量事件，未命中组合键的按键经 uinput 虚拟
 * 设备转发，KWin 的音量快捷键与 OSD 保持原行为。转发前留出 60 ms 宽限，
 * 覆盖两指同时按下时事件先后到达的情况。
 *
 * 关闭与点亮屏幕交给 KWin：内核 sysfs 的 dpms 属性只读，DPMS 由合成器
 * 通过私有 Wayland 协议控制，kscreen-doctor 是命令行入口。按键被独占后
 * KWin 看不到电源键，唤醒由本进程完成。
 *
 * 组合键与离开对话框通过会话总线调用桌面服务。以会话用户身份执行命令
 * 前切换到该用户，并设置 XDG_RUNTIME_DIR、WAYLAND_DISPLAY、
 * DBUS_SESSION_BUS_ADDRESS 与 XDG_CURRENT_DESKTOP。
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
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
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 驱动的 input 设备名，见 drivers/input/misc/pm8941-pwrkey.c 与设备树 */
#define POWER_KEY_NAME "pm8941_pwrkey"
#define VOLUME_UP_KEY_NAME "gpio-keys"
#define VOLUME_DOWN_KEY_NAME "pm8941_resin"

/* 按住电源键的时长：弹出离开对话框，以及之后的强制重启 */
#define DIALOG_DELAY_MS 2500
#define FORCE_REBOOT_MS 7000
/* 先按住的音量键在此时长内按下电源键，视为组合键 */
#define COMBO_WINDOW_MS 300
/* 音量键转发前等待的宽限，用于识别同时按下 */
#define VOLUME_GRACE_MS 60

/* 设备尚未出现时的重试间隔 */
#define RETRY_SEC 1
/* 外部命令的时间上限，避免合成器无响应时子进程长期占用 */
#define COMMAND_TIMEOUT_MS 5000
/* 自定义组合键命令的时间上限 */
#define COMBO_TIMEOUT_MS 30000
/* 外部命令标准输出的保存上限 */
#define OUTPUT_MAX 4096
/* 动作子进程尚未回收时的轮询间隔 */
#define REAP_INTERVAL_MS 1000
/* 同时运行的动作子进程上限 */
#define MAX_CHILDREN 4
/* 组合键配置路径与单行长度上限 */
#define CONFIG_PATH "/var/lib/loa/powerkey.conf"
#define CONFIG_LINE_MAX 1024

/* 配置文件缺失时的默认截屏命令，与 loa-tabletd 的 PRESET_KEY_BINDINGS 一致。 */
#define SCREENSHOT_COMMAND "spectacle -b -f"

/* 图形会话：用户身份、Wayland display 名称与是否为登录界面 */
struct session {
	uid_t uid;
	char wayland[256];
	bool greeter;
};

/* 音量键状态：物理按下、等待转发、已转发、被组合键吞掉 */
struct key {
	int fd;
	uint16_t code;
	const char *label;
	bool down;
	bool pending;
	bool forwarding;
	bool suppressed;
	int64_t down_ms;
};

static struct key volume_up = {
	.code = KEY_VOLUMEUP,
	.label = "音量加",
};
static struct key volume_down = {
	.code = KEY_VOLUMEDOWN,
	.label = "音量减",
};
static struct key *volume_keys[] = { &volume_up, &volume_down };
static int volume_key_count = sizeof(volume_keys) / sizeof(volume_keys[0]);

/* uinput 虚拟设备，转发未命中组合键的音量事件 */
static int uinput_fd = -1;
/* 电源键手势计时器与音量键宽限计时器 */
static int gesture_tfd = -1;
static int grace_tfd = -1;

/* 电源键手势状态 */
static bool power_down;
static int64_t power_down_ms;
static bool dialog_shown;
static bool combo_triggered;

/* 动作子进程：短按动作需要串行，避免连续两次短按互相覆盖 DPMS 状态 */
static pid_t children[MAX_CHILDREN];
static bool children_exclusive[MAX_CHILDREN];

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void arm_timer(int tfd, int milliseconds)
{
	struct itimerspec its = {
		.it_value = {
			.tv_sec = milliseconds / 1000,
			.tv_nsec = (long)(milliseconds % 1000) * 1000000L,
		},
	};

	if (timerfd_settime(tfd, 0, &its, NULL) < 0)
		perror("timerfd_settime");
}

static void disarm_timer(int tfd)
{
	struct itimerspec its = {0};

	timerfd_settime(tfd, 0, &its, NULL);
}

static bool device_has_key(int fd, uint16_t code)
{
	unsigned long bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 2] = {0};

	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
		return false;
	return !!(bits[code / (8 * sizeof(unsigned long))] &
		  (1UL << (code % (8 * sizeof(unsigned long)))));
}

/*
 * 按设备名与目标键码查找输入设备。设备尚未出现时持续重试。同名设备可能
 * 有多个（gpio-keys 是通用名称），键码不匹配时继续查找其余设备，只在
 * 首次遇到时提示，避免守护进程因无关设备退出后反复重启。
 */
static int open_input_device(const char *name, uint16_t code)
{
	bool warned = false;

	for (;;) {
		DIR *dir = opendir("/dev/input");
		struct dirent *de;

		if (!dir) {
			perror("opendir /dev/input");
			sleep(RETRY_SEC);
			continue;
		}

		while ((de = readdir(dir))) {
			char path[300];
			char devname[256] = {0};
			int fd;

			if (strncmp(de->d_name, "event", 5))
				continue;

			snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
			fd = open(path, O_RDONLY | O_CLOEXEC);
			if (fd < 0)
				continue;

			if (ioctl(fd, EVIOCGNAME(sizeof(devname) - 1), devname) >= 0 &&
			    !strcmp(devname, name)) {
				if (device_has_key(fd, code)) {
					closedir(dir);
					return fd;
				}
				if (!warned) {
					fprintf(stderr,
						"%s 缺少键码 %u，继续查找其余同名设备\n",
						name, code);
					warned = true;
				}
			}
			close(fd);
		}

		closedir(dir);
		sleep(RETRY_SEC);
	}
}

static bool path_is_socket(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int run_command(char *const argv[], uid_t uid, const char *wayland,
		       int timeout_ms, char *out, size_t out_len);

/*
 * 读取 loginctl 的单个属性。--value 只输出值；这里仍兼容
 * "Property=value" 形式，并去掉首尾空白。
 */
static void strip_property_value(char *text)
{
	char *start = text;
	char *end;
	char *equal;

	equal = strchr(text, '=');
	if (equal)
		start = equal + 1;
	while (*start == ' ' || *start == '\t')
		start++;
	end = start + strlen(start);
	while (end > start && (end[-1] == '\n' || end[-1] == '\r' ||
			       end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	if (start != text)
		memmove(text, start, strlen(start) + 1);
}

static bool active_session_id(char *out, size_t len)
{
	char *argv[] = { "loginctl", "show-seat", "seat0", "-p",
			 "ActiveSession", "--value", NULL };

	out[0] = '\0';
	if (run_command(argv, (uid_t)-1, NULL, COMMAND_TIMEOUT_MS, out, len) != 0)
		return false;
	strip_property_value(out);
	return out[0] != '\0';
}

static bool session_user(const char *session_id, char *out, size_t len)
{
	char *argv[] = { "loginctl", "show-session", (char *)session_id,
			 "-p", "User", "--value", NULL };

	out[0] = '\0';
	if (run_command(argv, (uid_t)-1, NULL, COMMAND_TIMEOUT_MS, out, len) != 0)
		return false;
	strip_property_value(out);
	return out[0] != '\0';
}

static bool session_class(const char *session_id, char *out, size_t len)
{
	char *argv[] = { "loginctl", "show-session", (char *)session_id,
			 "-p", "Class", "--value", NULL };

	out[0] = '\0';
	if (run_command(argv, (uid_t)-1, NULL, COMMAND_TIMEOUT_MS, out, len) != 0)
		return false;
	strip_property_value(out);
	return out[0] != '\0';
}

/*
 * 在指定用户的运行时目录中查找 Wayland 套接字，存在多个时取编号最小的
 * display。
 */
static bool find_wayland_in_uid(uid_t uid, struct session *s)
{
	char base[64];
	DIR *dir;
	struct dirent *de;
	long best = -1;
	bool found = false;

	snprintf(base, sizeof(base), "/run/user/%u", (unsigned)uid);
	dir = opendir(base);
	if (!dir)
		return false;

	while ((de = readdir(dir))) {
		char path[512];
		char *end;
		long display;

		if (strncmp(de->d_name, "wayland-", 8))
			continue;
		display = strtol(de->d_name + 8, &end, 10);
		if (*end)
			continue;
		snprintf(path, sizeof(path), "%s/%s", base, de->d_name);
		if (!path_is_socket(path))
			continue;
		if (found && display >= best)
			continue;
		best = display;
		s->uid = uid;
		snprintf(s->wayland, sizeof(s->wayland), "%s", de->d_name);
		found = true;
	}

	closedir(dir);
	return found;
}

/* logind 查询失败时的回退：扫描全部用户运行时目录，取最小 display。 */
static bool find_any_wayland_session(struct session *s)
{
	DIR *dir = opendir("/run/user");
	struct dirent *de;
	long best = -1;
	bool found = false;

	if (!dir) {
		perror("opendir /run/user");
		return false;
	}

	while ((de = readdir(dir))) {
		struct session candidate;
		char *end;
		long uid = strtol(de->d_name, &end, 10);
		long display;

		if (!de->d_name[0] || *end || uid <= 0)
			continue;
		if (!find_wayland_in_uid((uid_t)uid, &candidate))
			continue;
		candidate.greeter = false;
		display = strtol(candidate.wayland + 8, NULL, 10);
		if (found && display >= best)
			continue;
		best = display;
		*s = candidate;
		found = true;
	}

	closedir(dir);
	return found;
}

/*
 * 取 seat0 的活动会话对应的用户与 Wayland display。登录界面与用户会话
 * 可能同时存在，按 display 编号最小选择会落到 greeter；活动会话才能
 * 对应屏幕上正在显示的合成器。logind 查询失败时回退为扫描全部运行时目录。
 */
static bool find_graphical_session(struct session *s)
{
	char session_id[64];
	char user_text[32];
	char class[32];
	char *end;
	unsigned long uid;

	if (active_session_id(session_id, sizeof(session_id)) &&
	    session_user(session_id, user_text, sizeof(user_text))) {
		uid = strtoul(user_text, &end, 10);
		if (!*end && uid > 0 && find_wayland_in_uid((uid_t)uid, s)) {
			s->greeter = session_class(session_id, class,
						   sizeof(class)) &&
				     !strcmp(class, "greeter");
			return true;
		}
	}

	return find_any_wayland_session(s);
}

/*
 * 在截止时间前轮询等待子进程退出。返回 0 表示已回收，1 表示到期仍未
 * 退出，-1 表示 waitpid 出错。轮询间隔 10 ms，避免忙等。
 */
static int wait_child(pid_t pid, int *status, int64_t deadline)
{
	for (;;) {
		pid_t ret = waitpid(pid, status, WNOHANG);

		if (ret == pid)
			return 0;
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (now_ms() >= deadline)
			return 1;
		usleep(10000);
	}
}

/*
 * 取用户主目录。logind 的用户记录没有 Home 属性，静态链接下 getpwuid
 * 依赖 NSS 模块，因此调用 getent 读取标准的 passwd 记录。
 */
static bool find_user_home(uid_t uid, char *out, size_t len)
{
	char command[64];
	char line[512];
	char *field;
	char *end;
	FILE *pipe;
	int i;

	snprintf(command, sizeof(command), "getent passwd %u", (unsigned)uid);
	pipe = popen(command, "re");
	if (!pipe)
		return false;
	if (!fgets(line, sizeof(line), pipe)) {
		pclose(pipe);
		return false;
	}
	pclose(pipe);

	/* name:passwd:uid:gid:gecos:home:shell */
	field = line;
	for (i = 0; i < 5; i++) {
		field = strchr(field, ':');
		if (!field)
			return false;
		field++;
	}
	end = strchr(field, ':');
	if (!end || end == field)
		return false;
	*end = '\0';
	snprintf(out, len, "%s", field);
	return true;
}

/*
 * 以指定身份执行命令并收集标准输出。uid 为 (uid_t)-1 时保持当前身份，
 * 用于以 root 调用 systemctl。返回子进程退出码；启动失败、超时或异常
 * 终止返回 -1。
 */
static int run_command(char *const argv[], uid_t uid, const char *wayland,
		       int timeout_ms, char *out, size_t out_len)
{
	char runtime[64];
	char home[256] = "";
	gid_t gid = 0;
	int fds[2];
	pid_t pid;
	size_t used = 0;
	int64_t deadline;
	int status = 0;
	bool timed_out = false;
	bool reaped = false;

	if (pipe2(fds, O_CLOEXEC) < 0) {
		perror("pipe2");
		return -1;
	}

	if (uid != (uid_t)-1) {
		struct stat st;

		/*
		 * 主组取自 /run/user/<uid> 的属主。静态链接下 getpwuid 依赖
		 * 目标机的 NSS 模块，版本不一致时会失败。
		 */
		snprintf(runtime, sizeof(runtime), "/run/user/%u", (unsigned)uid);
		if (stat(runtime, &st) < 0) {
			perror(runtime);
			fprintf(stderr, "%s: 找不到会话运行时目录\n", argv[0]);
			close(fds[0]);
			close(fds[1]);
			return -1;
		}
		gid = st.st_gid;
		if (!find_user_home(uid, home, sizeof(home)))
			fprintf(stderr, "%s: 无法获取 uid %u 的主目录\n",
				argv[0], (unsigned)uid);
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		close(fds[0]);
		close(fds[1]);
		return -1;
	}

	if (pid == 0) {
		if (dup2(fds[1], STDOUT_FILENO) < 0)
			_exit(126);
		close(fds[0]);
		close(fds[1]);

		if (uid != (uid_t)-1) {
			char bus[96];

			setenv("XDG_RUNTIME_DIR", runtime, 1);
			setenv("WAYLAND_DISPLAY", wayland, 1);
			snprintf(bus, sizeof(bus), "unix:path=%s/bus", runtime);
			setenv("DBUS_SESSION_BUS_ADDRESS", bus, 1);
			setenv("XDG_CURRENT_DESKTOP", "KDE", 1);
			if (home[0])
				setenv("HOME", home, 1);
			if (setgroups(0, NULL) < 0 || setgid(gid) < 0 ||
			    setuid(uid) < 0) {
				fprintf(stderr, "%s: 切换用户失败: %s\n",
					argv[0], strerror(errno));
				_exit(126);
			}
		}

		execvp(argv[0], argv);
		fprintf(stderr, "%s: 执行失败: %s\n", argv[0], strerror(errno));
		_exit(127);
	}

	deadline = now_ms() + timeout_ms;
	close(fds[1]);
	if (out && out_len)
		out[0] = '\0';

	for (;;) {
		struct pollfd pfd = {
			.fd = fds[0],
			.events = POLLIN,
		};
		size_t space = (out && out_len) ? out_len - 1 - used : 0;
		char scratch[256];
		int64_t remaining;
		ssize_t n;

		/* 每次 poll 只使用剩余时间，输出持续到达也受总上限约束 */
		remaining = deadline - now_ms();
		if (remaining <= 0) {
			timed_out = true;
			break;
		}

		if (poll(&pfd, 1, (int)remaining) < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (!pfd.revents) {
			timed_out = true;
			break;
		}

		/* 输出超过缓冲区时继续读到结尾，多余部分丢弃 */
		n = space ? read(fds[0], out + used, space)
			  : read(fds[0], scratch, sizeof(scratch));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			break;
		}
		if (n == 0)
			break;
		if (space)
			used += (size_t)n;
	}

	close(fds[0]);

	/*
	 * 读取结束后子进程可能仍在运行，例如关闭标准输出后继续工作。
	 * 等待退出同样受截止时间约束：到期先终止，再给 100 ms 回收
	 * 窗口，避免事件循环无限阻塞。
	 */
	if (!timed_out && wait_child(pid, &status, deadline) == 0)
		reaped = true;

	if (!reaped) {
		kill(pid, SIGKILL);
		timed_out = true;
		fprintf(stderr, "%s: 执行超时，已终止\n", argv[0]);
		if (wait_child(pid, &status, now_ms() + 100) == 0)
			reaped = true;
	}

	if (!reaped) {
		fprintf(stderr, "%s: 子进程未退出\n", argv[0]);
		return -1;
	}

	if (out && out_len)
		out[used < out_len ? used : out_len - 1] = '\0';

	if (timed_out || !WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

/* 返回 1 表示屏幕点亮，0 表示屏幕关闭，-1 表示无法判定 */
static int query_dpms(const struct session *s)
{
	char out[OUTPUT_MAX];
	char *argv[] = { "kscreen-doctor", "--dpms", "show", NULL };
	int ret;

	ret = run_command(argv, s->uid, s->wayland, COMMAND_TIMEOUT_MS, out,
			  sizeof(out));
	if (ret != 0) {
		fprintf(stderr, "kscreen-doctor --dpms show 失败（退出码 %d）\n",
			ret);
		return -1;
	}

	/* 任一输出处于 off 即视为屏幕已关闭 */
	if (strstr(out, ": off"))
		return 0;
	if (strstr(out, ": on"))
		return 1;

	fprintf(stderr, "无法解析 DPMS 状态: %s\n", out);
	return -1;
}

static void set_dpms(const struct session *s, const char *mode)
{
	char *argv[] = { "kscreen-doctor", "--dpms", (char *)mode, NULL };

	if (run_command(argv, s->uid, s->wayland, COMMAND_TIMEOUT_MS, NULL, 0) != 0)
		fprintf(stderr, "kscreen-doctor --dpms %s 失败\n", mode);
}

static void lock_sessions(void)
{
	char *argv[] = { "loginctl", "lock-sessions", NULL };

	if (run_command(argv, (uid_t)-1, NULL, COMMAND_TIMEOUT_MS, NULL, 0) != 0)
		fprintf(stderr, "loginctl lock-sessions 失败\n");
}

/* 回收已结束的动作子进程，避免留下僵尸进程。 */
static void reap_children(void)
{
	int i;

	for (i = 0; i < MAX_CHILDREN; i++) {
		pid_t pid = children[i];
		pid_t ret;

		if (pid <= 0)
			continue;
		ret = waitpid(pid, NULL, WNOHANG);
		if (ret == pid || (ret < 0 && errno == ECHILD)) {
			children[i] = 0;
			children_exclusive[i] = false;
		}
	}
}

static bool exclusive_running(void)
{
	int i;

	for (i = 0; i < MAX_CHILDREN; i++)
		if (children[i] > 0 && children_exclusive[i])
			return true;
	return false;
}

static bool children_running(void)
{
	int i;

	for (i = 0; i < MAX_CHILDREN; i++)
		if (children[i] > 0)
			return true;
	return false;
}

/*
 * 短按、弹窗与组合键命令都可能耗时数秒，放到独立子进程执行，
 * 事件循环在此期间继续处理按住计时与新的按键事件。
 */
static void spawn_action(bool exclusive, void (*action)(void))
{
	pid_t pid;
	int slot = -1;
	int i;

	reap_children();
	if (exclusive && exclusive_running()) {
		fprintf(stderr, "上一轮短按处理尚未结束，忽略本次\n");
		return;
	}
	for (i = 0; i < MAX_CHILDREN; i++) {
		if (children[i] == 0) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		fprintf(stderr, "动作子进程已满，忽略本次\n");
		return;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return;
	}
	if (pid == 0) {
		action();
		_exit(0);
	}
	children[slot] = pid;
	children_exclusive[slot] = exclusive;
}

static void handle_short_press(void)
{
	struct session s;
	int on;

	if (!find_graphical_session(&s)) {
		fprintf(stderr, "没有找到图形会话，短按未处理\n");
		return;
	}

	on = query_dpms(&s);
	if (on < 0)
		return;

	fprintf(stderr, "短按：会话 uid=%u，屏幕%s\n", (unsigned)s.uid,
		on ? "点亮" : "关闭");

	if (on) {
		/* 关屏前锁定会话，与桌面电源设置的行为一致 */
		lock_sessions();
		set_dpms(&s, "off");
	} else {
		set_dpms(&s, "on");
	}
}

/*
 * 点亮屏幕后调用 KDE 的离开对话框。登录界面没有 ksmserver 的
 * LogoutPrompt 服务，直接执行有序关机；用户会话里调用失败时只记录
 * 日志，避免一次瞬时错误触发关机，用户仍可继续按住到强制重启。
 */
static void show_power_menu(void)
{
	struct session s;
	char *on[] = { "kscreen-doctor", "--dpms", "on", NULL };
	char *prompt[] = {
		"qdbus6", "org.kde.LogoutPrompt", "/LogoutPrompt",
		"org.kde.LogoutPrompt.promptAll", NULL,
	};
	char *poweroff[] = { "systemctl", "poweroff", "--no-block", NULL };
	int ret;

	if (!find_graphical_session(&s)) {
		fprintf(stderr, "没有图形会话，回退为有序关机\n");
		run_command(poweroff, (uid_t)-1, NULL, COMMAND_TIMEOUT_MS, NULL, 0);
		return;
	}

	if (s.greeter) {
		fprintf(stderr, "登录界面没有离开对话框，执行有序关机\n");
		run_command(poweroff, (uid_t)-1, NULL, COMMAND_TIMEOUT_MS, NULL, 0);
		return;
	}

	run_command(on, s.uid, s.wayland, COMMAND_TIMEOUT_MS, NULL, 0);
	ret = run_command(prompt, s.uid, s.wayland, COMMAND_TIMEOUT_MS, NULL, 0);
	if (ret != 0)
		fprintf(stderr, "离开对话框调用失败（退出码 %d），可继续按住电源键强制重启\n",
			ret);
}

/*
 * 读取组合键配置。配置文件不存在时使用默认绑定（音量下截屏、音量上
 * 无动作）；文件存在但某一行缺失或为空时，对应组合键不执行动作。
 */
static void load_combo_commands(char *down, size_t down_len, char *up,
				size_t up_len)
{
	FILE *file;
	char line[CONFIG_LINE_MAX];

	down[0] = '\0';
	up[0] = '\0';

	file = fopen(CONFIG_PATH, "re");
	if (!file) {
		snprintf(down, down_len, "%s", SCREENSHOT_COMMAND);
		return;
	}

	while (fgets(line, sizeof(line), file)) {
		char *key = line;
		char *value;
		char *end;
		char *eq;

		while (*key == ' ' || *key == '\t')
			key++;
		if (*key == '#' || *key == '\n' || *key == '\0')
			continue;
		eq = strchr(key, '=');
		if (!eq)
			continue;
		*eq = '\0';
		end = eq;
		while (end > key && (end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';
		value = eq + 1;
		while (*value == ' ' || *value == '\t')
			value++;
		end = value + strlen(value);
		while (end > value && (end[-1] == '\n' || end[-1] == '\r' ||
				       end[-1] == ' ' || end[-1] == '\t'))
			*--end = '\0';

		if (!strcmp(key, "combo_volume_down"))
			snprintf(down, down_len, "%s", value);
		else if (!strcmp(key, "combo_volume_up"))
			snprintf(up, up_len, "%s", value);
	}

	fclose(file);
}

/*
 * 组合键只在屏幕点亮时执行。命令交由会话用户的 shell 解释执行，
 * 配置由该用户维护，权限与用户自己的终端一致。
 */
static void run_combo_action(bool is_volume_down)
{
	char down[CONFIG_LINE_MAX];
	char up[CONFIG_LINE_MAX];
	struct session s;
	const char *command;
	char *argv[4];

	load_combo_commands(down, sizeof(down), up, sizeof(up));
	command = is_volume_down ? down : up;
	if (!command[0]) {
		fprintf(stderr, "组合键未绑定动作\n");
		return;
	}

	if (!find_graphical_session(&s)) {
		fprintf(stderr, "没有找到图形会话，组合键未执行\n");
		return;
	}
	if (query_dpms(&s) <= 0) {
		fprintf(stderr, "屏幕关闭，组合键未执行\n");
		return;
	}

	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = (char *)command;
	argv[3] = NULL;
	if (run_command(argv, s.uid, s.wayland, COMBO_TIMEOUT_MS, NULL, 0) != 0)
		fprintf(stderr, "组合键命令执行失败: %s\n", command);
}

static void run_combo_volume_down(void)
{
	run_combo_action(true);
}

static void run_combo_volume_up(void)
{
	run_combo_action(false);
}

static void spawn_combo_action(bool is_volume_down)
{
	spawn_action(false, is_volume_down ? run_combo_volume_down
					   : run_combo_volume_up);
}

static void spawn_dialog(void)
{
	spawn_action(false, show_power_menu);
}

static void spawn_short_press(void)
{
	spawn_action(true, handle_short_press);
}

/* 向 uinput 虚拟设备写入按键事件，KWin 据此执行音量快捷键与 OSD。 */
static void emit_key_event(uint16_t code, int value)
{
	struct input_event events[2] = {
		{ .type = EV_KEY, .code = code, .value = value },
		{ .type = EV_SYN, .code = SYN_REPORT, .value = 0 },
	};
	ssize_t written = write(uinput_fd, events, sizeof(events));

	if (written != (ssize_t)sizeof(events)) {
		fprintf(stderr, "uinput 写入失败: %s\n",
			written < 0 ? strerror(errno) : "长度不符");
	}
}

static bool any_pending_volume(void)
{
	int i;

	for (i = 0; i < volume_key_count; i++)
		if (volume_keys[i]->pending)
			return true;
	return false;
}

static void forward_volume_press(struct key *key)
{
	if (!key->pending)
		return;
	key->pending = false;
	if (key->suppressed)
		return;
	emit_key_event(key->code, 1);
	key->forwarding = true;
	fprintf(stderr, "转发：%s 按下\n", key->label);
}

/*
 * 音量键事件。先按住的音量键在宽限期内不立即转发，给同时按下的电源键
 * 留出识别组合键的窗口；命中组合键的按键在松手时不转发。
 */
static void handle_volume_event(struct key *key, int value)
{
	if (value == 2)
		return;

	if (value == 1) {
		if (key->down)
			return;
		key->down = true;
		key->down_ms = now_ms();
		key->pending = false;
		key->forwarding = false;
		key->suppressed = false;

		if (power_down && !dialog_shown && !combo_triggered) {
			combo_triggered = true;
			key->suppressed = true;
			disarm_timer(gesture_tfd);
			fprintf(stderr, "组合键：电源+%s\n", key->label);
			spawn_combo_action(key == &volume_down);
			return;
		}

		key->pending = true;
		arm_timer(grace_tfd, VOLUME_GRACE_MS);
		return;
	}

	if (value == 0) {
		if (!key->down)
			return;
		key->down = false;

		if (key->suppressed) {
			key->suppressed = false;
			fprintf(stderr, "组合键已吞掉 %s 松开\n", key->label);
		} else if (key->forwarding) {
			emit_key_event(key->code, 0);
			key->forwarding = false;
			fprintf(stderr, "转发：%s 松开\n", key->label);
		} else if (key->pending) {
			/* 宽限期内完成轻按，成对转发 */
			key->pending = false;
			emit_key_event(key->code, 1);
			emit_key_event(key->code, 0);
			fprintf(stderr, "转发：%s 轻按\n", key->label);
		}
	}
}

/*
 * 电源键事件。按下时检查已按住的音量键；命中组合键则取消弹窗计时，
 * 松手时也不执行短按动作。
 */
static void handle_power_event(int value)
{
	if (value == 2)
		return;

	if (value == 1) {
		struct key *combo_key = NULL;
		int i;

		if (power_down)
			return;
		power_down = true;
		power_down_ms = now_ms();
		dialog_shown = false;
		combo_triggered = false;

		for (i = 0; i < volume_key_count; i++) {
			struct key *key = volume_keys[i];

			if (key->down &&
			    power_down_ms - key->down_ms <= COMBO_WINDOW_MS) {
				combo_key = key;
				break;
			}
		}

		if (combo_key) {
			combo_triggered = true;
			if (combo_key->pending) {
				combo_key->pending = false;
				combo_key->suppressed = true;
				if (!any_pending_volume())
					disarm_timer(grace_tfd);
			} else if (!combo_key->forwarding) {
				combo_key->suppressed = true;
			}
			fprintf(stderr, "组合键：电源+%s\n", combo_key->label);
			spawn_combo_action(combo_key == &volume_down);
		} else {
			arm_timer(gesture_tfd, DIALOG_DELAY_MS);
		}
		return;
	}

	if (value == 0) {
		if (!power_down)
			return;
		power_down = false;
		disarm_timer(gesture_tfd);
		if (!combo_triggered && !dialog_shown)
			spawn_short_press();
	}
}

static void handle_grace_timer(void)
{
	uint64_t expirations;
	ssize_t got;
	int i;

	got = read(grace_tfd, &expirations, sizeof(expirations));
	if (got != (ssize_t)sizeof(expirations)) {
		/*
		 * 同一轮 poll 中命中组合键会先 disarm 计时器，随后这里读到
		 * EAGAIN；该轮待转发按键已经被组合键取消，直接返回。
		 */
		if (got < 0 && errno == EAGAIN)
			return;
		fprintf(stderr, "音量键计时器读取失败: %s\n",
			got < 0 ? strerror(errno) : "长度不符");
		return;
	}

	for (i = 0; i < volume_key_count; i++)
		forward_volume_press(volume_keys[i]);
}

static void handle_gesture_timer(void)
{
	uint64_t expirations;
	ssize_t got;

	got = read(gesture_tfd, &expirations, sizeof(expirations));
	if (got != (ssize_t)sizeof(expirations)) {
		/* 松手已经在同一轮 poll 中 disarm 了计时器 */
		if (got < 0 && errno == EAGAIN)
			return;
		fprintf(stderr, "电源键计时器读取失败: %s\n",
			got < 0 ? strerror(errno) : "长度不符");
		return;
	}

	/* 松手与计时到点同时发生时以松手为准 */
	if (!power_down || combo_triggered)
		return;

	if (!dialog_shown) {
		dialog_shown = true;
		fprintf(stderr, "电源键按住 %d ms，弹出离开对话框\n",
			DIALOG_DELAY_MS);
		spawn_dialog();
		arm_timer(gesture_tfd, FORCE_REBOOT_MS - DIALOG_DELAY_MS);
		return;
	}

	fprintf(stderr, "电源键按住 %d ms，强制重启\n", FORCE_REBOOT_MS);
	execlp("systemctl", "systemctl", "reboot", "--force", "--force", NULL);
	perror("execlp systemctl reboot");
	_exit(EXIT_FAILURE);
}

static void handle_key_event(uint16_t code, int value)
{
	if (code == KEY_POWER)
		handle_power_event(value);
	else if (code == volume_up.code)
		handle_volume_event(&volume_up, value);
	else if (code == volume_down.code)
		handle_volume_event(&volume_down, value);
}

/* 三颗按键分别来自不同设备，同一轮 poll 中按事件时间合并后分发。 */
struct key_event {
	struct timeval time;
	uint16_t code;
	int value;
};

#define KEY_EVENT_BATCH 64

static size_t collect_key_events(int fd, uint16_t code, const char *label,
				 struct key_event *out, size_t capacity)
{
	struct input_event events[16];
	ssize_t got;
	size_t count;
	size_t i;
	size_t n = 0;

	got = read(fd, events, sizeof(events));
	if (got <= 0) {
		/* 设备被移除或读取异常，退出后由 systemd 重新拉起 */
		fprintf(stderr, "%s 读取失败: %s\n", label,
			got < 0 ? strerror(errno) : "EOF");
		exit(EXIT_FAILURE);
	}

	count = (size_t)got / sizeof(events[0]);
	for (i = 0; i < count && n < capacity; i++) {
		if (events[i].type != EV_KEY || events[i].code != code)
			continue;
		out[n].time = events[i].time;
		out[n].code = code;
		out[n].value = events[i].value;
		n++;
	}
	return n;
}

/*
 * 按事件时间排序。设备文件按固定顺序读取，若直接按读取顺序处理，
 * 「松开电源键后立刻按下音量键」与「松开音量键后立刻按下电源键」
 * 会在同一轮 poll 中被错误地合并成组合键。
 */
static int compare_key_events(const void *a, const void *b)
{
	const struct key_event *ea = a;
	const struct key_event *eb = b;
	long long ta = (long long)ea->time.tv_sec * 1000000 + ea->time.tv_usec;
	long long tb = (long long)eb->time.tv_sec * 1000000 + eb->time.tv_usec;

	if (ta < tb)
		return -1;
	if (ta > tb)
		return 1;
	return 0;
}

static int create_virtual_keyboard(void)
{
	struct uinput_setup setup = {0};
	int fd;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/uinput");
		return -1;
	}

	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP) < 0 ||
	    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN) < 0) {
		perror("uinput 能力设置");
		close(fd);
		return -1;
	}

	snprintf(setup.name, sizeof(setup.name), "LoA Volume Keys");
	setup.id.bustype = BUS_VIRTUAL;
	setup.id.vendor = 0x1d6b;
	setup.id.product = 0x0101;

	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
	    ioctl(fd, UI_DEV_CREATE) < 0) {
		perror("uinput 设备创建");
		close(fd);
		return -1;
	}

	return fd;
}

int main(void)
{
	int power_fd;
	int i;

	uinput_fd = create_virtual_keyboard();
	if (uinput_fd < 0)
		return EXIT_FAILURE;

	power_fd = open_input_device(POWER_KEY_NAME, KEY_POWER);
	if (power_fd < 0)
		return EXIT_FAILURE;
	volume_up.fd = open_input_device(VOLUME_UP_KEY_NAME, KEY_VOLUMEUP);
	if (volume_up.fd < 0)
		return EXIT_FAILURE;
	volume_down.fd = open_input_device(VOLUME_DOWN_KEY_NAME, KEY_VOLUMEDOWN);
	if (volume_down.fd < 0)
		return EXIT_FAILURE;

	for (i = 0; i < volume_key_count; i++) {
		if (ioctl(volume_keys[i]->fd, EVIOCGRAB, 1) < 0) {
			perror("EVIOCGRAB 音量键");
			return EXIT_FAILURE;
		}
	}
	if (ioctl(power_fd, EVIOCGRAB, 1) < 0) {
		perror("EVIOCGRAB 电源键");
		return EXIT_FAILURE;
	}

	/*
	 * 非阻塞：同一轮 poll 中先处理按键再处理计时器，按键回调可能
	 * disarm 计时器并清掉到期计数，此时读取应当返回 EAGAIN 而不是阻塞。
	 */
	gesture_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	grace_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (gesture_tfd < 0 || grace_tfd < 0) {
		perror("timerfd_create");
		return EXIT_FAILURE;
	}

	for (;;) {
		struct pollfd pfd[5] = {
			{ .fd = volume_up.fd, .events = POLLIN },
			{ .fd = volume_down.fd, .events = POLLIN },
			{ .fd = power_fd, .events = POLLIN },
			{ .fd = gesture_tfd, .events = POLLIN },
			{ .fd = grace_tfd, .events = POLLIN },
		};
		struct key_event events[KEY_EVENT_BATCH];
		size_t event_count = 0;
		size_t i;
		int ret;

		/*
		 * 动作子进程尚未回收时使用有限超时，让空闲状态下的 poll
		 * 保持无限等待。
		 */
		ret = poll(pfd, 5, children_running() ? REAP_INTERVAL_MS : -1);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return EXIT_FAILURE;
		}

		reap_children();

		/* 设备被移除时 poll 可能只报 POLLHUP/POLLERR，读取后退出重来 */
		if (pfd[0].revents & (POLLIN | POLLERR | POLLHUP))
			event_count += collect_key_events(volume_up.fd, KEY_VOLUMEUP,
							  volume_up.label,
							  events + event_count,
							  KEY_EVENT_BATCH - event_count);
		if (pfd[1].revents & (POLLIN | POLLERR | POLLHUP))
			event_count += collect_key_events(volume_down.fd,
							  KEY_VOLUMEDOWN,
							  volume_down.label,
							  events + event_count,
							  KEY_EVENT_BATCH - event_count);
		if (pfd[2].revents & (POLLIN | POLLERR | POLLHUP))
			event_count += collect_key_events(power_fd, KEY_POWER,
							  "电源键",
							  events + event_count,
							  KEY_EVENT_BATCH - event_count);

		/* 按事件时间分发，处理顺序不对组合键判定产生影响 */
		if (event_count > 1)
			qsort(events, event_count, sizeof(events[0]),
			      compare_key_events);
		for (i = 0; i < event_count; i++)
			handle_key_event(events[i].code, events[i].value);

		/* 计时器在按键之后处理：松手与到点同时发生时以松手为准 */
		if (pfd[3].revents & POLLIN)
			handle_gesture_timer();
		if (pfd[4].revents & POLLIN)
			handle_grace_timer();
	}
}
