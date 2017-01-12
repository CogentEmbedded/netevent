#include <fcntl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include "suinput.h"

#define testbit(in, bit) (!!( ((in)[bit/8]) & (1<<(bit&7)) ))

int main(int argc, char **argv)
{
	int i;
	struct stat st;
	int fd;
	struct uinput_user_dev dev;
	unsigned char input_bits[1+EV_MAX/8];

	memset(&dev, 0, sizeof(dev));

	printf("struct size %d\n", (int)sizeof(dev));

	if (argc != 2) {
		fprintf(stderr, "usage: %s device-file\n", argv[0]);
		exit(1);
	}
	if (lstat(argv[1], &st) != 0) {
		perror("stat");
		exit(1);
	}
	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "not a character device: %s\n", argv[1]);
		exit(1);
	}
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	if (ioctl(fd, EVIOCGNAME(sizeof(dev.name)), dev.name) == -1) {
		fprintf(stderr, "Failed to get device name: %d\n", errno);
		goto error;
	}

	if (ioctl(fd, EVIOCGID, &dev.id) == -1) {
		fprintf(stderr, "Failed to get device id: %d\n", errno);
		goto error;
	}

	fprintf(stderr, "Device     : %s\n", dev.name);
	fprintf(stderr, " id.bustype: %04x\n", dev.id.bustype);
	fprintf(stderr, " id.vendor : %04x\n", dev.id.vendor);
	fprintf(stderr, " id.product: %04x\n", dev.id.product);
	fprintf(stderr, " id.version: %04x\n", dev.id.version);
	fprintf(stderr, " ff_effects_max: %08x\n", dev.ff_effects_max);

	if (ioctl(fd, EVIOCGBIT(0, sizeof(input_bits)), &input_bits) == -1) {
		fprintf(stderr, "Failed to get input-event bits: %d\n", errno);
		goto error;
	}

	fprintf(stderr, "In Bits:");
	for (i = 0; i < sizeof(input_bits) * 8; i++)
		if (testbit(input_bits, i))
			fprintf(stderr, " %02x", i);
	fprintf(stderr, "\n");

#define GetBitsFor(REL, rel, REL_MAX) \
	do { \
	if (testbit(input_bits, EV_##REL)) { \
		unsigned char bits##rel[1+REL_MAX/8]; \
		fprintf(stderr, "Getting " #rel "-bits:\n"); \
		if (ioctl(fd, EVIOCGBIT(EV_##REL, sizeof(bits##rel)), bits##rel) == -1) { \
			fprintf(stderr, "Failed to get " #rel " bits: %d\n", errno); \
			goto error; \
		} \
		for (i = 0; i < sizeof(bits##rel) * 8; i++) \
			if (testbit(bits##rel, i)) \
				fprintf(stderr, " %02x", i); \
		fprintf(stderr, "\n"); \
	} \
	} while(0)

	GetBitsFor(KEY, key, KEY_MAX);
	GetBitsFor(ABS, abs, ABS_MAX);
	GetBitsFor(REL, rel, REL_MAX);
	GetBitsFor(MSC, msc, MSC_MAX);
	GetBitsFor(SW, sw, SW_MAX);
	GetBitsFor(LED, led, LED_MAX);

#define GetDataFor(KEY, key, KEY_MAX) \
	do { \
	if (testbit(input_bits, EV_##KEY)) { \
		fprintf(stderr, "Getting " #key "-state.\n"); \
		unsigned char bits##key[1+KEY_MAX/8]; \
		if (ioctl(fd, EVIOCG##KEY(sizeof(bits##key)), bits##key) == -1) { \
			fprintf(stderr, "Failed to get " #key " state: %d\n", errno); \
			goto error; \
		} \
		for (i = 0; i < sizeof(bits##key) * 8; i++) \
			if (testbit(bits##key, i)) \
				fprintf(stderr, " %02x", i); \
		fprintf(stderr, "\n"); \
	} \
	} while(0)

	GetDataFor(KEY, key, KEY_MAX);
	GetDataFor(LED, led, LED_MAX);
	GetDataFor(SW, sw, SW_MAX);

	if (testbit(input_bits, EV_ABS)) {
		struct input_absinfo ai;
		unsigned char abs_bits[1+ABS_MAX/8];

		fprintf(stderr, "Getting abs-info.\n");
		if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) == -1) {
			fprintf(stderr, "Failed to get ABS bits: %d\n", errno);
			goto error;
		}
		for (i = 0; i < ABS_MAX; ++i) {
			if (!testbit(abs_bits, i))
				continue;
			if (ioctl(fd, EVIOCGABS(i), &ai) == -1) {
				fprintf(stderr, "Failed to get device id: %d\n", errno);
				goto error;
			}
			fprintf(stderr, " 0x%02x: value %d, min %d, max %d, fuzz %d, flat %d, res %d\n",
				i, ai.value, ai.minimum, ai.maximum, ai.fuzz, ai.flat, ai.resolution);
		}
	}
error:
	close(fd);
	return 0;
}
