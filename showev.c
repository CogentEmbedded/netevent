#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>

#include "main.h"

const char *evname(unsigned int e)
{
	static char buf[128];
	switch (e) {
#define ETOS(x) case x: return #x
		ETOS(EV_SYN);
		ETOS(EV_KEY);
		ETOS(EV_REL);
		ETOS(EV_ABS);
		ETOS(EV_MSC);
		ETOS(EV_SW);
		ETOS(EV_LED);
		ETOS(EV_SND);
		ETOS(EV_REP);
		ETOS(EV_FF);
		ETOS(EV_PWR);
		ETOS(EV_FF_STATUS);
		ETOS(EV_MAX);
		default:
		{
			sprintf(buf, "0x%04x", e);
			return buf;
		}
	}
	return "<0xDeadC0de>";
}
#undef ETOS

const char *absname(unsigned int e)
{
	static char buf[128];
	switch (e) {
#define ETOS(x) case x: return #x
		ETOS(ABS_X);
		ETOS(ABS_Y);
		ETOS(ABS_MT_POSITION_X);
		ETOS(ABS_MT_POSITION_Y);
		ETOS(ABS_MT_TRACKING_ID);
		ETOS(ABS_MT_SLOT);
		default:
		{
			sprintf(buf, "0x%04x", e);
			return buf;
		}
	}
	return "<0xDeadC0de>";
}
#undef ETOS

int evid(const char *name)
{
#define ETOS(x) if (!strcmp(name, #x)) return x
       	ETOS(EV_SYN);
       	ETOS(EV_KEY);
       	ETOS(EV_REL);
       	ETOS(EV_ABS);
       	ETOS(EV_MSC);
       	ETOS(EV_SW);
       	ETOS(EV_LED);
       	ETOS(EV_SND);
       	ETOS(EV_REP);
       	ETOS(EV_FF);
       	ETOS(EV_PWR);
       	ETOS(EV_FF_STATUS);
	return EV_MAX;
}
#undef ETOS

static int fd;

static void toggle_hook()
{
	if (ioctl(fd, EVIOCGRAB, (on ? (void*)1 : (void*)0)) == -1) {
		fprintf(stderr, "Grab failed: %d\n", errno);
	}
	setenv("GRAB", (on ? "1" : "0"), 1);
	if (toggle_cmd) {
		if (!fork()) {
			execlp("sh", "sh", "-c", toggle_cmd, NULL);
			fprintf(stderr, "Failed to run command: %d\n", errno);
			exit(1);
		}
	}
}

void ev_toggle(int sig)
{
	if (sig == SIGUSR1) {
		on = !on;
		toggle_hook();
	}
}

int show_events(int count, const char *devname)
{
	int c;
	ssize_t s;
	struct input_event ev;

	if (count < 0) {
		fprintf(stderr, "Bogus number specified: cannot print %d events.\n", count);
		return 1;
	}

	fd = open(devname, O_RDONLY);
	
	if (fd < 0) {
		fprintf(stderr, "Failed to open device '%s': %d\n", devname, errno);
		return 1;
	}

	signal(SIGUSR1, ev_toggle);

	on = !no_grab;
	if (on) {
		if (ioctl(fd, EVIOCGRAB, (void*)1) == -1) {
			fprintf(stderr, "Failed to grab device: %d\n", errno);
			close(fd);
			return 1;
		}
		setenv("GRAB", "1", -1);
	}
	else
		setenv("GRAB", "0", -1);

	for (c = 0; !count || c < count; ++c) {
		int dummy;
		waitpid(0, &dummy, WNOHANG);
		s = read(fd, &ev, sizeof(ev));
		if (s < 0) {
			fprintf(stderr, "Error while reading from device: %d\n", errno);
			close(fd);
			return 1;
		}
		if (s == 0) {
			fprintf(stderr, "End of data.\n");
			close(fd);
			return 1;
		}
		if (!be_quiet) {
			time_t curtime = ev.time.tv_sec;
			struct tm *tmp = localtime(&curtime);

			fprintf(stderr, "Event time: %s", asctime(tmp));
			fprintf(stderr, " Type = 0x%02x (%s)\n", ev.type, evname(ev.type));
			fprintf(stderr, " Code = 0x%02x (%s)\n", ev.code,
				ev.type == EV_ABS ? absname(ev.code) : "");
			fprintf(stderr, " Value = %d\n", ev.value);
			if (ev.type == EV_SYN && !count_syn)
				fprintf(stderr, "----SYNC---\n");
			if (!count_syn && ev.type == EV_SYN)
				--c;
		}
		bool old_on = on;
		if (hotkey_hook(ev.type, ev.code, ev.value)) {
			if (old_on != on)
				toggle_hook();
		}
	}
	
	return 0;
}
