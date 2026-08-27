/*
 * Copyright 2026, Vitruvian Project.
 * Distributed under the terms of the MIT License.
 *
 * Standalone uinput event injector for the input test harness, run under
 * sudo. Split out of testinput.cpp: writing /dev/uinput needs root, but
 * testinput needs to run as the session user to have a window at all. Plain
 * C, not linked against libbe/libroot.
 *
 * Do NOT fix this with a udev rule/group permission for /dev/uinput; that
 * would let any input-group process synthesize keystrokes into any session,
 * permanently. sudo per test run is the narrower grant.
 *
 * Wire protocol: one command per line on stdin, plain text.
 *
 *   SETUP KEYBOARD           create the relative keyboard+mouse device
 *   SETUP TABLET [maxabs]    create the absolute-positioning device
 *                            (default maxabs 32767)
 *   SETUP KEYS               create a pure-keyboard device
 *   EMIT <dev> <type> <code> <value>
 *                            a raw input_event; dev is 0/1/2 for
 *                            keyboard+mouse/tablet/pure-keyboard
 *   SLEEP <microseconds>
 *   QUIT
 *
 * "READY\n" on stdout once per successful SETUP. Exit status is 0 if at
 * least one SETUP ever succeeded, 1 otherwise.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/input.h>
#include <linux/uinput.h>


static int sFD = -1;
static int sAbsFD = -1;
static int sKeyFD = -1;
static bool sAnyReady = false;


static bool
setup_keyboard(void)
{
	if (sFD >= 0)
		return true;

	sFD = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (sFD < 0) {
		fprintf(stderr, "testinject: open /dev/uinput: %s (uid %d)\n",
			strerror(errno), (int)getuid());
		return false;
	}

	ioctl(sFD, UI_SET_EVBIT, EV_KEY);
	ioctl(sFD, UI_SET_EVBIT, EV_REL);
	ioctl(sFD, UI_SET_EVBIT, EV_SYN);
	for (int code = 0; code <= KEY_MAX; code++)
		ioctl(sFD, UI_SET_KEYBIT, code);
	ioctl(sFD, UI_SET_RELBIT, REL_X);
	ioctl(sFD, UI_SET_RELBIT, REL_Y);
	ioctl(sFD, UI_SET_RELBIT, REL_WHEEL);

	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x1209;
	setup.id.product = 0x0001;
	strcpy(setup.name, "testinject virtual device");

	if (ioctl(sFD, UI_DEV_SETUP, &setup) < 0
		|| ioctl(sFD, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "testinject: uinput create: %s\n", strerror(errno));
		close(sFD);
		sFD = -1;
		return false;
	}

	return true;
}


// SETUP KEYBOARD's device carries BTN_LEFT + REL_X/Y, so it's claimed as a
// pointer, not a keyboard. This device sets neither, so it claims as a
// keyboard instead.
static bool
setup_keys(void)
{
	if (sKeyFD >= 0)
		return true;

	sKeyFD = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (sKeyFD < 0) {
		fprintf(stderr, "testinject: open /dev/uinput (keys): %s (uid %d)\n",
			strerror(errno), (int)getuid());
		return false;
	}

	ioctl(sKeyFD, UI_SET_EVBIT, EV_KEY);
	ioctl(sKeyFD, UI_SET_EVBIT, EV_SYN);
	ioctl(sKeyFD, UI_SET_EVBIT, EV_LED);
	ioctl(sKeyFD, UI_SET_LEDBIT, LED_CAPSL);
	ioctl(sKeyFD, UI_SET_LEDBIT, LED_NUML);
	ioctl(sKeyFD, UI_SET_LEDBIT, LED_SCROLLL);

	// Skip the BTN_* blocks; KEY_OK onwards is genuine keyboard territory.
	for (int code = 1; code <= KEY_MAX; code++) {
		if ((code >= BTN_MISC && code < KEY_OK)
			|| code >= BTN_TRIGGER_HAPPY) {
			continue;
		}
		ioctl(sKeyFD, UI_SET_KEYBIT, code);
	}

	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x1209;
	setup.id.product = 0x0003;
	strcpy(setup.name, "testinject virtual keyboard");

	if (ioctl(sKeyFD, UI_DEV_SETUP, &setup) < 0
		|| ioctl(sKeyFD, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "testinject: keyboard create: %s\n", strerror(errno));
		close(sKeyFD);
		sKeyFD = -1;
		return false;
	}

	return true;
}


static bool
setup_tablet(int maxAbs)
{
	if (sAbsFD >= 0)
		return true;
	if (maxAbs <= 0)
		maxAbs = 32767;

	sAbsFD = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (sAbsFD < 0) {
		fprintf(stderr, "testinject: open /dev/uinput (tablet): %s\n",
			strerror(errno));
		return false;
	}

	ioctl(sAbsFD, UI_SET_EVBIT, EV_KEY);
	ioctl(sAbsFD, UI_SET_EVBIT, EV_ABS);
	ioctl(sAbsFD, UI_SET_EVBIT, EV_SYN);
	ioctl(sAbsFD, UI_SET_KEYBIT, BTN_LEFT);
	ioctl(sAbsFD, UI_SET_KEYBIT, BTN_RIGHT);
	ioctl(sAbsFD, UI_SET_KEYBIT, BTN_MIDDLE);

	struct uinput_abs_setup abs;
	memset(&abs, 0, sizeof(abs));
	abs.absinfo.minimum = 0;
	abs.absinfo.maximum = maxAbs;
	abs.code = ABS_X;
	ioctl(sAbsFD, UI_ABS_SETUP, &abs);
	abs.code = ABS_Y;
	ioctl(sAbsFD, UI_ABS_SETUP, &abs);

	struct uinput_setup setup;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_USB;
	setup.id.vendor = 0x1209;
	setup.id.product = 0x0002;
	strcpy(setup.name, "testinject virtual tablet");

	if (ioctl(sAbsFD, UI_DEV_SETUP, &setup) < 0
		|| ioctl(sAbsFD, UI_DEV_CREATE) < 0) {
		fprintf(stderr, "testinject: tablet create: %s\n", strerror(errno));
		close(sAbsFD);
		sAbsFD = -1;
		return false;
	}

	return true;
}


static void
teardown(void)
{
	if (sFD >= 0) {
		ioctl(sFD, UI_DEV_DESTROY);
		close(sFD);
		sFD = -1;
	}
	if (sAbsFD >= 0) {
		ioctl(sAbsFD, UI_DEV_DESTROY);
		close(sAbsFD);
		sAbsFD = -1;
	}
	if (sKeyFD >= 0) {
		ioctl(sKeyFD, UI_DEV_DESTROY);
		close(sKeyFD);
		sKeyFD = -1;
	}
}


static void
emit(int dev, int type, int code, int value)
{
	int fd = dev == 1 ? sAbsFD : (dev == 2 ? sKeyFD : sFD);
	if (fd < 0)
		return;

	struct input_event event;
	memset(&event, 0, sizeof(event));
	event.type = (unsigned short)type;
	event.code = (unsigned short)code;
	event.value = value;
	write(fd, &event, sizeof(event));
}


int
main(void)
{
	// Line-buffered: READY has to reach the driving process as soon as
	// it's written, not batched behind stdio's default block buffering
	// once stdout is a pipe rather than a tty.
	setvbuf(stdout, NULL, _IOLBF, 0);

	char line[256];
	while (fgets(line, sizeof(line), stdin) != NULL) {
		char* newline = strchr(line, '\n');
		if (newline != NULL)
			*newline = '\0';

		char verb[16] = "";
		sscanf(line, "%15s", verb);

		if (strcmp(verb, "SETUP") == 0) {
			char what[16] = "";
			int maxAbs = 32767;
			sscanf(line, "%*s %15s %d", what, &maxAbs);
			bool ok;
			if (strcmp(what, "TABLET") == 0)
				ok = setup_tablet(maxAbs);
			else if (strcmp(what, "KEYS") == 0)
				ok = setup_keys();
			else
				ok = setup_keyboard();
			if (ok) {
				sAnyReady = true;
				printf("READY\n");
			}
		} else if (strcmp(verb, "EMIT") == 0) {
			int dev, type, code, value;
			if (sscanf(line, "%*s %d %d %d %d", &dev, &type, &code,
					&value) == 4) {
				emit(dev, type, code, value);
			}
		} else if (strcmp(verb, "SLEEP") == 0) {
			long micros = 0;
			sscanf(line, "%*s %ld", &micros);
			if (micros > 0)
				usleep((useconds_t)micros);
		} else if (strcmp(verb, "QUIT") == 0) {
			break;
		}
		// Unrecognized lines are ignored, not fatal; a newer client
		// talking to an older tool shouldn't abort the sequence mid-run.
	}

	teardown();
	return sAnyReady ? 0 : 1;
}
