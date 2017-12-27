#include "main.h"

#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

int64_t htonll(int64_t value){
    int num = 42;
    if(*(char *)&num == 42) //test big/little endian
        return (((int64_t)htonl(value)) << 32) + htonl(value >> 32);
    else
        return value;
}

static bool running = true;
bool on = true;
static int fd = 0;
static bool tog_on = false;

static pthread_t tog_thread;

#if defined( WITH_INOTIFY )
# include <sys/inotify.h>
static int inf_fd;
static int watch_fd;
#endif

static void toggle_hook();
void tog_signal(int sig)
{
	if (sig == SIGUSR2)
		tog_on = false;
	else if (sig == SIGUSR1) {
		on = !on;
		toggle_hook();
	}
}

static void *tog_func(void *ign)
{
	int tfd;
	char dat[8];
	tog_on = true;
	signal(SIGUSR2, tog_signal);

#if !defined( WITH_INOTIFY )
	struct stat st;
	if (lstat(toggle_file, &st) != 0) {
		fprintf(stderr, "stat failed on %s, %d\n", toggle_file, errno);
		tog_on = false;
	}
	else
	{
		if (!S_ISFIFO(st.st_mode)) {
			fprintf(stderr, "The toggle file is not a fifo, and inotify support has not been compiled in.\n");
			fprintf(stderr, "This is evil, please compile with inotify support.\n");
			tog_on = false;
		}
	}
#else
	inf_fd = inotify_init();
	if (inf_fd == -1) {
		fprintf(stderr, "inotify_init failed: %d\n");
		tog_on = false;
	} else {
		watch_fd = inotify_add_watch(inf_fd, toggle_file, IN_CLOSE_WRITE | IN_CREATE);
		if (watch_fd == -1) {
			fprintf(stderr, "inotify_add_watch failed: %d\n", err);
			tog_on = false;
		}
	}
#endif

	while (tog_on) {
#if defined( WITH_INOTIFY )
		inotify_event iev;
		if (read(inf_fd, &iev, sizeof(iev)) != (ssize_t)sizeof(iev)) {
			fprintf(stderr, "Failed to read from inotify watch: %d\n", err);
			break;
		}
		if (iev.wd != watch_fd) {
			fprintf(stderr, "Inotify sent is bogus information...\n");
			continue;
		}
#endif
		tfd = open(toggle_file, O_RDONLY);
		if (tfd < 0) {
			fprintf(stderr, "Failed to open '%s', %d\n", toggle_file, errno);
			break;
		}
		memset(dat, 0, sizeof(dat));
		read(tfd, dat, sizeof(dat));
		close(tfd);
		dat[sizeof(dat)-1] = 0;
		bool r = !!atoi(dat);
		if (on != r) {
			on = r;
			toggle_hook();
		}
	}

	tog_on = running = false;
	return 0;
}

static void toggle_hook()
{
	if (ioctl(fd, EVIOCGRAB, (on ? (void*)1 : (void*)0)) == -1) {
		fprintf(stderr, "Grab failed: %d\n", errno);
	}
	setenv("GRAB", (on ? "1" : "0"), -1);
	if (toggle_cmd) {
		if (!fork()) {
			execlp("sh", "sh", "-c", toggle_cmd, NULL);
			fprintf(stderr, "Failed to run command: %d\n", errno);
			exit(1);
		}

	}
}

int socket_open(const char *hostname, int port)
{
	int ret;
	int sockfd;
	int val;
	
	struct sockaddr_in serv_addr;
	struct hostent *server;

	printf("connecting to %s:%d\n", hostname, port);

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		fprintf(stderr, "ERROR opening socket\n");
		return sockfd;
	}

	server = gethostbyname(hostname);
	if (server == NULL) {
		fprintf(stderr, "ERROR, no such host\n");
		return -1;
	}

	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr,
		(char *)&serv_addr.sin_addr.s_addr,
		server->h_length);
	serv_addr.sin_port = htons(port);
	ret = connect(sockfd,(struct sockaddr *)&serv_addr,sizeof(serv_addr));
	if (ret < 0) {
		fprintf(stderr, "ERROR connecting %d, %d\n", ret, errno);
		return ret;
	}

	val = 5;
	ret = setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &val,
			 sizeof(val));
	if (ret < 0) {
		fprintf(stderr, "ERROR on setsockopt %d", ret);
		return ret;	
	}

	val = 1;
	ret = setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &val,
			 sizeof(val));
	if (ret < 0) {
		fprintf(stderr, "ERROR on setsockopt %d", ret);
		return ret;	
	}

	val = 1;
	ret = setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &val,
			 sizeof(val));
	if (ret < 0) {
		fprintf(stderr, "ERROR on setsockopt %d", ret);
		return ret;	
	}

	return sockfd;
}

int read_device_new(const char *devfile, const char *hostname, int port)
{
	struct input_event ev;
	int sock_fd;
	size_t i;
	ssize_t s;
	//int e = 0;
	on = !no_grab;

	signal(SIGUSR1, tog_signal);
	signal(SIGPIPE, SIG_IGN);

	fd = open(devfile, O_RDONLY);

	if (fd < 0) {
		std::string err(strerror(errno));
		fprintf(stderr, "Failed to open device '%s', %d\n", devfile, errno);
		return 1;
	}

	if (on) {
		if (ioctl(fd, EVIOCGRAB, (void*)1) == -1) {
			fprintf(stderr, "Failed to grab device: %d\n", errno);
		}
		setenv("GRAB", "1", -1);
	}
	else
		setenv("GRAB", "0", -1);

	struct uinput_user_dev dev;
	memset(&dev, 0, sizeof(dev));

	if (ioctl(fd, EVIOCGNAME(sizeof(dev.name)), dev.name) == -1) {
		fprintf(stderr, "Failed to get device name: %d\n", errno);
		goto error;
	}

	if (ioctl(fd, EVIOCGID, &dev.id) == -1) {
		fprintf(stderr, "Failed to get device id: %d\n", errno);
		goto error;
	}

	fprintf(stderr, " Device: %s\n", dev.name);
	fprintf(stderr, "     Id: %d\n", dev.id.version);
	fprintf(stderr, "BusType: %d\n", dev.id.bustype);

	sock_fd = socket_open(hostname, port);
	if (sock_fd < 0)
		goto error;

	// First thing to write is the size of the structures as a 16 bit uint!
	uint16_t strsz;
	strsz = htons(sizeof(dev));
	if (!write(sock_fd, (const char*)&strsz, sizeof(strsz)))
		goto err_close;

	if (!write(sock_fd, dev.name, sizeof(dev.name)))
		goto err_close;

	if (!write(sock_fd, (const char*)&dev.id, sizeof(dev.id)))
		goto err_close;

	fprintf(stderr, "Getting input bits.\n");
	if (ioctl(fd, EVIOCGBIT(0, sizeof(input_bits)), &input_bits) == -1) {
		fprintf(stderr, "Failed to get input-event bits: %d\n", errno);
		goto error;
	}
	if (!write(sock_fd, (const char*)input_bits, sizeof(input_bits)))
		goto err_close;

#define TransferBitsFor(REL, rel, REL_MAX) \
	do { \
	if (testbit(input_bits, EV_##REL)) { \
		unsigned char bits##rel[1+REL_MAX/8]; \
		fprintf(stderr, "Getting " #rel "-bits.\n"); \
		if (ioctl(fd, EVIOCGBIT(EV_##REL, sizeof(bits##rel)), bits##rel) == -1) { \
			fprintf(stderr, "Failed to get " #rel " bits: %d\n", errno); \
			goto error; \
		} \
		if (!write(sock_fd, (const char*)&bits##rel, sizeof(bits##rel))) \
			goto err_close; \
	} \
	} while(0)

	TransferBitsFor(KEY, key, KEY_MAX);
	TransferBitsFor(ABS, abs, ABS_MAX);
	TransferBitsFor(REL, rel, REL_MAX);
	TransferBitsFor(MSC, msc, MSC_MAX);
	TransferBitsFor(SW, sw, SW_MAX);
	TransferBitsFor(LED, led, LED_MAX);

#define TransferDataFor(KEY, key, KEY_MAX) \
	do { \
	if (testbit(input_bits, EV_##KEY)) { \
		fprintf(stderr, "Getting " #key "-state.\n"); \
		unsigned char bits##key[1+KEY_MAX/8]; \
		if (ioctl(fd, EVIOCG##KEY(sizeof(bits##key)), bits##key) == -1) { \
			fprintf(stderr, "Failed to get " #key " state: %d\n", errno); \
			goto error; \
		} \
		if (!write(sock_fd, (const char*)bits##key, sizeof(bits##key))) \
			goto err_close; \
	} \
	} while(0)

	TransferDataFor(KEY, key, KEY_MAX);
	TransferDataFor(LED, led, LED_MAX);
	TransferDataFor(SW, sw, SW_MAX);

	if (testbit(input_bits, EV_ABS)) {
		struct input_absinfo ai;
		fprintf(stderr, "Getting abs-info.\n");
		for (i = 0; i < ABS_MAX; ++i) {
			if (ioctl(fd, EVIOCGABS(i), &ai) == -1) {
				fprintf(stderr, "Failed to get device id: %d\n", errno);
				goto error;
			}
			if (!write(sock_fd, (const char*)&ai, sizeof(ai)))
				goto err_close;
		}
	}

	if (toggle_file) {
		if (pthread_create(&tog_thread, 0, &tog_func, 0) != 0) {
			fprintf(stderr, "Failed to create toggling-thread: %d\n", errno);
			goto error;
		}
        }

	fprintf(stderr, "Transferring input events.\n");
	while (running) {
		int dummy;
		waitpid(0, &dummy, WNOHANG);
		s = read(fd, &ev, sizeof(ev));
		if (!s) {
			fprintf(stderr, "EOF\n");
			break;
		}
		else if (s < 0) {
			fprintf(stderr, "When reading from device: %d\n", errno);
			goto error;
		}

		bool old_on = on;
		if (hotkey_hook(ev.type, ev.code, ev.value)) {
			if (old_on != on)
				toggle_hook();
		}
		else if (on) {
			input_event_t et;
			int val;
			//fprintf(stderr, "EV %d.%06d: type %d, code %d, value %d\n",
			//	(int)ev.time.tv_sec, (int)ev.time.tv_usec, (int)ev.type, ev.code, ev.value);
			et.tv_sec = htonll(ev.time.tv_sec);
			et.tv_usec = htonl(ev.time.tv_usec);
			et.type = htons(ev.type);
			et.code = htons(ev.code);
			et.value = htonl(ev.value);
			val = write(sock_fd, (const char*)&et, sizeof(et));
			if (val <= 0) {
				fprintf(stderr, "When writin to socket: %d\n", errno);
				goto err_close;
			}
		}
	}

	goto end;
err_close:
	close(sock_fd);
error:
	//e = 1;
end:
	if (tog_on)
		pthread_cancel(tog_thread);
	ioctl(fd, EVIOCGRAB, (void*)0);
	close(fd);

	return 0;
}

int read_device(const char *devfile, const char *hostname, int port)
{
	while (1) {
		read_device_new(devfile, hostname, port);
		sleep(1);
	}
	return 0;
}
