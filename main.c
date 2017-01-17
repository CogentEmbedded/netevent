#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <ctype.h>

#include "main.h"

const char *toggle_file = 0;
const char *toggle_cmd = 0;
bool no_grab = false;
bool count_syn = false;
bool be_quiet = false;

int read_device(const char *devname, const char *hostname, int port);
int spawn_device(int port);
int spawn_device_emulator(int port);
int show_events(int count, const char *devname);

/*
static int isdigit(char ch)
{
	return ((ch <= '9') && (ch >= '0'));
}
*/

static void usage(const char *arg0)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, " %s [options]-read <device> <ip/hostname> <port>\n", arg0);
	fprintf(stderr, " %s -write <port>         Fully clone other side device, binary protocol\n", arg0);
	fprintf(stderr, " %s -emulate <port>       Create virtual touch, text protocol (see sources)\n", arg0);
	fprintf(stderr, "[options] -showevents <count> <device>\n");
	fprintf(stderr, "options are:\n");
	fprintf(stderr, "  -ontoggle <command>     Command to execute when grabbing is toggled.\n");
	fprintf(stderr, "  -toggler <fifo>         Fifo to keep opening and reading the on-status.\n");
	fprintf(stderr, "  -nograb                 Do not grab the device at startup.\n");
	fprintf(stderr, "  -countsyn               Also count SYN events in showevents.\n");
	fprintf(stderr, "  -hotkey t:c:v <command> Run a command on the Type:Code:Value event.\n");
	fprintf(stderr, "  -quiet                  Shut up -showevents.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "a count of 0 in -showevents means keep going forever\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "example hotkey: -hotkey EV_KEY:161:0 \"\" -hotkey EV_KEY:161:1 \"play sound.wav\"\n");
	fprintf(stderr, "  will ignore the eject key-down event, but play sound.wav when releasing the key.\n");
	fprintf(stderr, "The special hotkey command '@toggle' toggles device grabbing.\n");
	fprintf(stderr, "You can use '@toggle-on' and '@toggle-off' to specifically enable or disable grabbing.\n");
	exit(1);
}

int main(int argc, char **argv)
{
	const char *arg0 = argv[0];
	if (argc < 2)
		usage(arg0);

	while (1) {
		if (argc < 2)
			usage(arg0);
		char *command = argv[1];
		if ((strcmp(command, "-h") == 0) || 
		    (strcmp(command, "--help") == 0) ||
		    (strcmp(command, "-help") == 0)) {
			usage(arg0);
		}
		else if (strcmp(command, "-read") == 0) {
			if (argc < 5)
				usage(arg0);
			return read_device(argv[2], argv[3], atoi(argv[4]));
		}
		else if (strcmp(command, "-write") == 0) {
			if (argc < 3)
				usage(arg0);
			return spawn_device(atoi(argv[2]));
		}
		else if (strcmp(command, "-emulate") == 0) {
			if (argc < 3)
				usage(arg0);
			return spawn_device_emulator(atoi(argv[2]));
		}
		else if (strcmp(command, "-toggler") == 0) {
			if (argc < 3)
				usage(arg0);
			toggle_file = argv[2];
			argv += 2;
			argc -= 2;
		}
		else if (strcmp(command, "-ontoggle") == 0) {
			if (argc < 3)
				usage(arg0);
			toggle_cmd = argv[2];
			argv += 2;
			argc -= 2;
		}
		else if (strcmp(command, "-nograb") == 0) {
			no_grab = true;
			++argv;
			--argc;
		}
		else if (strcmp(command, "-showevents") == 0) {
			if (argc < 4)
				usage(arg0);
			return show_events(atoi(argv[2]), argv[3]);
		}
		else if (strcmp(command, "-countsyn") == 0) {
			count_syn = true;
			++argv;
			--argc;
		}
		else if (strcmp(command, "-quiet") == 0) {
			be_quiet = true;
			++argv;
			--argc;
		}
		else if (strcmp(command, "-hotkey") == 0) {
			if (argc < 4)
				usage(arg0);
			if (!add_hotkey(argv[2], argv[3]))
				usage(arg0);
			argv += 3;
			argc -= 3;
		}
		else {
			fprintf(stderr, "invalid parameter: %s\n",argv[1]);
			usage(arg0);
		}
	}
	return 1;
}

#define MAX_HOTKEYS	16
hotkey_t hotkeys[MAX_HOTKEYS];
int hotkey_cnt = 0;

bool add_hotkey(const char *keydef, const char *command)
{
	int i = 0;
	hotkey_t hk;
	char spart[3][64];
	size_t part = 0;
	const char *p;

	if (hotkey_cnt >= MAX_HOTKEYS)
		return false;

	for (p = keydef; *p && part < 3; ++p) {
		if (*p == ':') {
			++part;
			spart[part][i] = 0;
			i = 0;
		} else {
			spart[part][i++] = *p;
		}
	}

	if (!strlen(spart[0]) || !strlen(spart[1]) || !strlen(spart[2])) {
		fprintf(stderr, "Invalid hotkey parameter, need to be of the format <type>:<code>:<value>.\n");
		fprintf(stderr, "For example: EV_KEY:161:1\n");
		return false;
	}

	if (!isdigit(spart[1][0])) {
		fprintf(stderr, "Invalid code parameter: %s\n", spart[1]);
		return false;
	}

	if (!isdigit(spart[2][0])) {
		fprintf(stderr, "Invalid value parameter: %s\n", spart[2]);
		return false;
	}

	if (isdigit(spart[0][0]))
		hk.type = atoi(spart[0]);
	else
		hk.type = evid(spart[0]);
	if (hk.type < 0 || hk.type >= EV_MAX) {
		fprintf(stderr, "Invalid type: %s\n", spart[0]);
		return false;
	}

	hk.code = atoi(spart[1]);
	hk.value = atoi(spart[2]);
	hk.command = strdup(command);

	if (!be_quiet) {
		fprintf(stderr, "Adding hotkey: %d:%d:%d = %s\n",
			hk.type, hk.code, hk.value, hk.command);
	}
	hotkeys[hotkey_cnt++] = hk;

	return true;
}

void tog_signal(int sig);
bool hotkey_hook(int type, int code, int value)
{
	int i;
	hotkey_t *hi;
	for (i = 0; i < hotkey_cnt; i++) {
		hi = &hotkeys[i];
		if (hi->type == type && hi->code == code && hi->value == value) {
			if (!hi->command)
				return true;
			if (strcmp(hi->command, "@toggle") == 0) {
				on = !on;
				return true;
			}
			if (strcmp(hi->command, "@toggle-on") == 0) {
				on = true;
				return true;
			}
			if (strcmp(hi->command, "@toggle-off") == 0) {
				on = false;
				return true;
			}
			if (!fork()) {
				execlp("sh", "sh", "-c", hi->command, NULL);
				fprintf(stderr, "Failed to run command: %d\n", errno);
				exit(1);
			}
			return true;
		}
	}
	return false;
}
