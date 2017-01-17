#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "main.h"
#include "suinput.h"

#ifndef UI_ABS_SETUP
/* old kernel, does not support set abs values one-by-one */
#define OLD_WAY
/* from newer kernel */
struct uinput_abs_setup {
	__u16  code; /* axis code */
	/* __u16 filler; */
	struct input_absinfo absinfo;
};

/* function to enable abs event and set proper values to uinput_user_dev */
int uinput_set_abs(struct uinput_user_dev *dev, struct uinput_abs_setup *abs)
{
	int i = abs->code;

	if (i > ABS_MAX)
		return -1;

	dev->absmax[i] = abs->absinfo.maximum;
	dev->absmin[i] = abs->absinfo.minimum;
	dev->absfuzz[i] = abs->absinfo.fuzz;
	dev->absflat[i] = abs->absinfo.flat;

	return 0;
}

#endif

static uint16_t strsz;

extern int64_t ntohll(int64_t value);
extern int uinput_open(void);
extern int socket_start_listen(int port);
extern int socket_wait_connection(int sockfd);
extern int uinput_open(void);

static struct uinput_user_dev dev = {
	.name = "CogentEmbedded virtual touch",
	.id = {
		.bustype = 0x0003,
		.vendor  = 0x1234,
		.product = 0xdead,
		.version = 0xbeef,
	},
	.ff_effects_max = 0x0,
};

#define RESOLUTION_X	4096
#define RESOLUTION_Y	4096
#define TOUCH_SLOTS	10
#define MAX_TRACK	65535

static struct uinput_abs_setup abs_settings[] ={
	{
		.code = ABS_X,
		.absinfo = {0, 0, RESOLUTION_X - 1, 0, 0, 0},
	},
	{
		.code = ABS_Y,
		.absinfo = {0, 0, RESOLUTION_Y - 1, 0, 0, 0},
	},
	{
		.code = ABS_MT_SLOT,
		.absinfo = {0, 0, TOUCH_SLOTS - 1, 0, 0, 0},
	},
	{
		.code = ABS_MT_POSITION_X,
		.absinfo = {0, 0, RESOLUTION_X - 1, 0, 0, 0},
	},
	{
		.code = ABS_MT_POSITION_Y,
		.absinfo = {0, 0, RESOLUTION_Y - 1, 0, 0, 0},
	},
	{
		.code = ABS_MT_TRACKING_ID,
		.absinfo = {0, 0, MAX_TRACK, 0, 0, 0},
	},
};

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof(*(a)))

static int buffer_has_str(char *buffer, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (buffer[i] == 0)
			return 0;
		if (buffer[i] == '\n')
			return i + 1;
	}
	return 0;
}

extern const char *evname(unsigned int e);
extern const char *absname(unsigned int e);

static int uinput_event(int fd, __u16 type, __u16 code, __s32 value)
{
	struct input_event ev;

	gettimeofday(&ev.time, NULL);
	ev.type = type;
	ev.code = code;
	ev.value = value;

#if 1
	fprintf(stderr, "Event time: %d.%06d\n", ev.time.tv_sec, ev.time.tv_usec);
	fprintf(stderr, " Type = 0x%02x (%s)\n", ev.type, evname(ev.type));
	fprintf(stderr, " Code = 0x%02x (%s)\n", ev.code,
		ev.type == EV_ABS ? absname(ev.code) : "");
	fprintf(stderr, " Value = %d\n", ev.value);
	if (ev.type == EV_SYN)
		fprintf(stderr, "----SYNC---\n");
#endif

	if (!write(fd, (const char*)&ev, sizeof(ev))) {
		fprintf(stderr, "uinput write failed: %d\n", errno);
		return -1;
	}
	return 0;
}

static char thacking_id = 0;
static char finger_id[TOUCH_SLOTS];

static int get_tracking_id(void)
{
	int tmp = thacking_id;

	thacking_id = (thacking_id + 1) % (MAX_TRACK - 1);

	return tmp + 1;
}

static int touch_cnt(void)
{
	int i;
	int cnt = 0;

	for (i = 0; i < TOUCH_SLOTS; i++)
		if (finger_id[i])
			cnt++;
	return cnt;
}

static int uinput_touch_start(int fd, unsigned int id, int x, int y)
{
	int ret;
	int new = 0;
	int touch, new_touch;

	if (id > TOUCH_SLOTS)
		return -1;

	if (finger_id[id]) {
		fprintf(stderr, "Slot %d allready touch-started while new touch-start\n", id);
		//return 0;
	}

	touch = !!touch_cnt();
	if (finger_id[id] == 0) {
		new = 1;
		finger_id[id] = get_tracking_id();
	}
	new_touch = !!touch_cnt();

	ret  = uinput_event(fd, EV_ABS, ABS_MT_SLOT, id);
	if (new)
		ret  = uinput_event(fd, EV_ABS, ABS_MT_TRACKING_ID, finger_id[id]);
	ret |= uinput_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
	ret |= uinput_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);

	if (touch != new_touch)
		ret |= uinput_event(fd, EV_KEY, BTN_TOUCH, new_touch);

	if (touch_cnt() == 1) {
		ret |= uinput_event(fd, EV_ABS, ABS_X, x);
		ret |= uinput_event(fd, EV_ABS, ABS_Y, y);
	}

	ret |= uinput_event(fd, EV_SYN, 0, 0);

	return ret;
}

static int uinput_touch_move(int fd, unsigned int id, int x, int y)
{
	int ret;

	if (id > TOUCH_SLOTS)
		return -1;

	if (finger_id[id] == 0) {
		fprintf(stderr, "Slot %d is not touch-started while move called\n", id);

		return uinput_touch_start(fd, id, x, y);
		//return 0;
	}

	ret  = uinput_event(fd, EV_ABS, ABS_MT_SLOT, id);
	ret |= uinput_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
	ret |= uinput_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);

	if (touch_cnt() == 1) {
		ret |= uinput_event(fd, EV_ABS, ABS_X, x);
		ret |= uinput_event(fd, EV_ABS, ABS_Y, y);
	}

	ret |= uinput_event(fd, EV_SYN, 0, 0);

	return ret;
}

static int uinput_touch_end(int fd, unsigned int id)
{
	int ret;
	int touch, new_touch;

	if (id > TOUCH_SLOTS)
		return -1;

	if (finger_id[id] == 0) {
		fprintf(stderr, "Slot %d allready touch-ended\n", id);
		return 0;
	}

	touch = !!touch_cnt();
	finger_id[id] = 0;
	new_touch = !!touch_cnt();

	ret  = uinput_event(fd, EV_ABS, ABS_MT_SLOT, id);
	ret  = uinput_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);

	if (touch != new_touch)
		ret |= uinput_event(fd, EV_KEY, BTN_TOUCH, new_touch);

	ret |= uinput_event(fd, EV_SYN, 0, 0);

	return ret;
}


#define RECV_BUFFER_SIZE	256
static int handle_input_msg(int input_fd, unsigned char *ptr, int size)
{
	int i;
	int ret;
	int touch_id, touch_x, touch_y;

	printf("%s : buffer = %s \n",__func__, ptr);

	if (!strncmp(ptr, "bye", 3)) {
		return -1;
	} else if (!strncmp(ptr, "touch-start", 11)) {
		ret = !(3 == sscanf(ptr, "%*s %d %d %d\n",
			&touch_id, &touch_x, &touch_y));
		if (ret)
			goto format_err;
		ret = uinput_touch_start(input_fd, touch_id, touch_x, touch_y);
	} else if (!strncmp(ptr, "touch-move", 10)) {
		ret = !(3 == sscanf(ptr, "%*s %d %d %d\n",
			&touch_id, &touch_x, &touch_y));
		if (ret)
			goto format_err;
		ret = uinput_touch_move(input_fd, touch_id, touch_x, touch_y);
	} else if (!strncmp(ptr, "touch-end", 9)) {
		ret = !(1 == sscanf(ptr, "%*s %d\n",
			&touch_id));
		if (ret)
			goto format_err;
		ret = uinput_touch_end(input_fd, touch_id);
	} else if (!strncmp(ptr, "sync", 4)) {
		/* nope for now */
	} else {
		fprintf(stderr, "\"%s\"\nAugh?\n", ptr);
		return 0;
	}

	if (ret)
		fprintf(stderr, "uinput write error: %d\n", ret);
	return ret;

format_err:
	fprintf(stderr, "Message format error:\n%s\n", ptr);
	return ret;
}

static int handle_input(int sock_con, int input_fd)
{
	int tail = 0;
	unsigned char buffer[RECV_BUFFER_SIZE] = {0};

	while (true) {
		int i;
		int size;
		int dummy;
		int end;
		fd_set read_set;
		struct timeval timeout;

		waitpid(0, &dummy, WNOHANG);

		timeout.tv_sec = 10;
		timeout.tv_usec = 0;

		FD_ZERO(&read_set);
		FD_SET(sock_con, &read_set);

		int ret = select(sock_con + 1, &read_set, NULL, NULL, &timeout);

		if (ret < 0) {
			fprintf(stderr, "select return %d\n", ret);
			return ret;
		} else if (ret == 0) {
			continue;
		}

		ret = recv(sock_con, buffer,
			RECV_BUFFER_SIZE, MSG_DONTWAIT);
		if (ret < 0) {
			fprintf(stderr, "recv return %d\n", ret);
			return ret;
		} else if (ret == 0) {
			fprintf(stderr, "recv == 0, shutdown\n");
			return ret;
		}

		size = ret;

		{
			char * start = buffer;
			char * token = strtok (buffer,"\n");

			while (token != NULL)
			{
			    ret = handle_input_msg(input_fd, token, token - start);
			    if (ret < 0)
                                return ret;

			    token = strtok (NULL, "\n");
			    start = token;
			}
		}
		memset(buffer, 0, size);
	}
	return 0;
}

int spawn_device_new_emulator(int sock_fd)
{
	int e;
	int fd;
	int i;
	int ret;
	ssize_t si;
	unsigned char input_bits[1+EV_MAX/8];

	memset(input_bits, 0, sizeof(input_bits));

	fd = uinput_open();
	if (fd < 0) {
		fprintf(stderr, "Failed to open uinput device file. Please specify.\n");
		return 1;
	}

#ifdef OLD_WAY
	/* put min, max and other ABS settings in dev struct */
	for (i = 0; i < ARRAY_SIZE(abs_settings); i++) {
		ret = uinput_set_abs(&dev, &abs_settings[i]);
		if (ret)
			fprintf(stderr, "Failed to setup ABS event: %d\n", ret);
	}
#endif

	ret = suinput_enable_event(fd, EV_KEY, BTN_TOUCH);
	if (ret) {
		fprintf(stderr, "Failed to set BTN_TOUCH\n");
		return -1;
	}

#ifdef OLD_WAY
	for (i = 0; i < ARRAY_SIZE(abs_settings); i++) {
		ret = suinput_enable_event(fd, EV_ABS, abs_settings[i].code);
		if (ret)
			fprintf(stderr, "Failed to enable ABS event 0x%02x: %d\n",
				abs_settings[i].code, ret);
	}
#endif

	si = write(fd, &dev, sizeof(dev));
	if (si < (ssize_t)sizeof(dev)) {
		fprintf(stderr, "Failed to write initial data to device: %d\n", errno);
		goto err_close;
	}

#ifndef OLD_WAY
	for (i = 0; i < ARRAY_SIZE(abs_settings); i++) {
		ret = suinput_enable_abs_event(fd, &abs_settings[i]);
		if (ret)
			fprintf(stderr, "Failed to setup ABS event 0x%02x: %d\n",
				abs_settings[i].code, ret);
	}
#endif

	if (ioctl(fd, UI_DEV_CREATE) == -1) {
		fprintf(stderr, "Failed to create device: %d\n", errno);
		goto err_close;
	}

	fprintf(stderr, "Transferring input events.\n");

	ret = handle_input(sock_fd, fd);

err_close:
	if (sock_fd > 0)
		close(sock_fd);
error:
	e = 1;
end:
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	
	return e;
}

int spawn_device_emulator(int port)
{
	int ret;
	int sock_listen, sock_con;
	printf("...%d\n", port);

	sock_listen = socket_start_listen(port);
	if (sock_listen < 0)
		return sock_listen;

	while(1) {
		sock_con = socket_wait_connection(sock_listen);
		if (sock_con < 0)
			goto err_close;
		printf("Got connection on port %d\n", port);
		ret = spawn_device_new_emulator(sock_con);
		printf("...connection closed %d\n", ret);
		sleep(1);
	}
	return 0;
err_close:
	close(sock_listen);
	return -1;
}
