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

int64_t ntohll(int64_t value){
    int num = 42;
    if(*(char *)&num == 42) //test big/little endian
        return value;
    else
        return (((int64_t)ntohl(value)) << 32) + ntohl(value >> 32);
}

static const char *uinput_file[] = {
	"/dev/uinput",
	"/dev/input/uinput",
	"/dev/misc/uinput",
};
static const size_t uinput_cnt = sizeof(uinput_file) / sizeof(uinput_file[0]);

static uint16_t strsz;

int socket_start_listen(int port)
{
	int ret;
	int sockfd;
	int val = 1;
	
	struct sockaddr_in serv_addr;

	printf("starting on port %d\n", port);

	sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sockfd < 0) {
		fprintf(stderr, "ERROR opening socket %d\n", sockfd);
		return sockfd;
	}

	ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &val,
			 sizeof(val));
	if (ret < 0) {
		fprintf(stderr, "ERROR on setsockopt REUSEADDR: %s\n",
			strerror(errno));
		return ret;
	}

	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);
	ret = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
	if (ret	< 0) {
		fprintf(stderr, "ERROR on binding: %s\n",
			strerror(errno));
		return ret;
	}

	ret = setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &val,
			 sizeof(val));
	if (ret < 0) {
		fprintf(stderr, "ERROR on setsockopt KEEPALIVE: %s\n",
			strerror(errno));
		return ret;
	}

	val = 5;
	ret = setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &val,
			 sizeof(val));
	if (ret < 0) {
		fprintf(stderr, "ERROR on setsockopt KEEPCNT: %s\n",
			strerror(errno));
		return ret;
	}

	val = 1;
	ret = setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &val,
			 sizeof(val));
	if (ret < 0) {
		fprintf(stderr, "ERROR on setsockopt KEEPIDLE: %s\n",
			strerror(errno));
		return ret;
	}

	val = 1;
	ret = setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &val,
			 sizeof(val));
	if (ret < 0) {
		fprintf(stderr, "ERROR on setsockopt KEEPINTVL: %s\n",
			strerror(errno));
		return ret;
	}

	listen(sockfd, 1);
	
	return sockfd;
}

int socket_wait_connection(int sockfd)
{
	int newsockfd;
	struct sockaddr_in cli_addr;
	socklen_t clilen;

	clilen = sizeof(cli_addr);
	newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
	if (newsockfd < 0) {
		fprintf(stderr, "ERROR on accept: %s\n",
			strerror(errno));
		return newsockfd;
	}
	return newsockfd;
}

int uinput_open(void)
{
	int i;
	int fd;

	for (i = 0; i < uinput_cnt; ++i) {
		fd = open(uinput_file[i], O_WRONLY | O_NDELAY);
		if (fd >= 0)
			break;
	}

	return fd;
}

int spawn_device_new(int sock_con)
{
	int e;
	int fd;
	int i;
	ssize_t si;
	struct uinput_user_dev dev;
	struct input_event ev;
	unsigned char input_bits[1+EV_MAX/8];

	memset(input_bits, 0, sizeof(input_bits));

	fd = uinput_open();
	if (fd < 0) {
		fprintf(stderr, "Failed to open uinput device file. Please specify.\n");
		return 1;
	}

	read(sock_con, (char*)&strsz, sizeof(strsz));
	strsz = ntohs(strsz);
	if (strsz != sizeof(dev)) {
		fprintf(stderr, "Device information field sizes do not match (%d != %d). Sorry.\n",
			strsz, (int)sizeof(dev));
		goto err_close;
	}

	memset(&dev, 0, sizeof(dev));
	read(sock_con, dev.name, sizeof(dev.name));
	read(sock_con, &dev.id, sizeof(dev.id));
	
	read(sock_con, input_bits, sizeof(input_bits));
	for (i = 0; i < EV_MAX; ++i) {
		if (!testbit(input_bits, i))
			continue;
		if (ioctl(fd, UI_SET_EVBIT, i) == -1) {
			fprintf(stderr, "Failed to set evbit %d, %d\n", i, errno);
			goto error;
		}
	}

#define RecieveBitsFor(REL, rel, REL_MAX, RELBIT) \
	do { \
	if (testbit(input_bits, EV_##REL)) { \
		unsigned char bits##rel[1+REL_MAX/8]; \
		fprintf(stderr, "Reading " #rel "-bits\n"); \
		read(sock_con, (char*)bits##rel, sizeof(bits##rel)); \
		for (i = 0; i < REL_MAX; ++i) { \
			if (!testbit(bits##rel, i)) continue; \
			if (ioctl(fd, UI_SET_##RELBIT, i) == -1) { \
				fprintf(stderr, "Failed to set " #rel "-bit: %d, %d\n", i, errno); \
				goto err_close; \
			} \
		} \
	} \
	} while(0)

	RecieveBitsFor(KEY, key, KEY_MAX, KEYBIT);
	RecieveBitsFor(ABS, abs, ABS_MAX, ABSBIT);
	RecieveBitsFor(REL, rel, REL_MAX, RELBIT);
	RecieveBitsFor(MSC, msc, MSC_MAX, MSCBIT);
	RecieveBitsFor(SW, sw, SW_MAX, SWBIT);
	RecieveBitsFor(LED, led, LED_MAX, LEDBIT);

#define RecieveDataFor(KEY, key, KEY_MAX, KEYBIT) \
	do { \
	if (testbit(input_bits, EV_##KEY)) { \
		unsigned char bits##key[1+KEY_MAX/8]; \
		fprintf(stderr, "Reading " #key "-data\n"); \
		read(sock_con, (char*)bits##key, sizeof(bits##key)); \
		for (i = 0; i < KEY_MAX; ++i) { \
			if (!testbit(bits##key, i)) continue; \
			if (ioctl(fd, UI_SET_##KEYBIT, i) == -1) { \
				fprintf(stderr, "Failed to activate " #key "-bit: %d, %d\n", i, errno); \
				goto err_close; \
			} \
		} \
	} \
	} while(0)

	RecieveDataFor(KEY, key, KEY_MAX, KEYBIT);
	RecieveDataFor(LED, led, LED_MAX, LEDBIT);
	RecieveDataFor(SW, sw, SW_MAX, SWBIT);

	if (testbit(input_bits, EV_ABS)) {
		struct input_absinfo ai;
		for (i = 0; i < ABS_MAX; ++i) {
			read(sock_con, (char*)&ai, sizeof(ai));
			dev.absmin[i] = ai.minimum;
			dev.absmax[i] = ai.maximum;
		}
	}

	si = write(fd, &dev, sizeof(dev));
	if (si < (ssize_t)sizeof(dev)) {
		fprintf(stderr, "Failed to write initial data to device: %s\n",
			strerror(errno));
		goto err_close;
	}

	if (ioctl(fd, UI_DEV_CREATE) == -1) {
		fprintf(stderr, "Failed to create device: %s\n",
			strerror(errno));
		goto err_close;
	}

	fprintf(stderr, "Transferring input events.\n");
	while (true) {
		int ret;
		input_event_t et;
		int dummy;
		waitpid(0, &dummy, WNOHANG);
		ret = read(sock_con, (char*)&et, sizeof(et));

		if (ret <= 0) {
			fprintf(stderr, "End of data\n");
			break;
		}
		ev.time.tv_sec = ntohll(et.tv_sec);
		ev.time.tv_usec = ntohl(et.tv_usec);
		ev.type = ntohs(et.type);
		ev.code = ntohs(et.code);
		ev.value = ntohl(et.value);
		//fprintf(stderr, "EV %d.%06d: type %d, code %d, value %d\n",
		//	(int)ev.time.tv_sec, (int)ev.time.tv_usec, (int)ev.type, ev.code, ev.value);
		if (hotkey_hook(ev.type, ev.code, ev.value))
			continue;
		if (write(fd, &ev, sizeof(ev)) < (ssize_t)sizeof(ev)) {
			fprintf(stderr, "Write error: %d\n", errno);
			goto err_close;
		}
	}

	goto end;

err_close:
	if (sock_con > 0)
		close(sock_con);
error:
	e = 1;
end:
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	
	return e;
}

int spawn_device(int port)
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
		ret = spawn_device_new(sock_con);
		printf("...connection closed %d\n", ret);
		sleep(1);
	}
	return 0;
err_close:
	close(sock_listen);
	return -1;
}
