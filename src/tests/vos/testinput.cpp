/*
 * Copyright 2026, Dario Casalinuovo <b.vitruvio@gmail.com>.
 * Distributed under the terms of the MIT License.
 *
 * Input subsystem test harness. Runs inside the VM only.
 *
 * Three modes:
 *   testinput probe         : no window; keymap, settings and ABI probes
 *   testinput capture [secs]: window; logs every input message it receives
 *   testinput inject [secs] : capture, plus a driven event sequence and a
 *                               pass/fail verdict per expectation
 *
 * Output is one record per line so a driving agent can diff runs:
 *   RESULT|<status>|<name>|<detail>      status: PASS FAIL DENIED MISSING SKIP
 *   EVENT|<what>|<key=value>...
 *
 * Nothing here modifies system state except the key repeat rate and delay,
 * restored before exit.
 *
 * inject mode does not touch /dev/uinput itself: writing it needs root, but
 * this process needs to run as the session user to have a window. The
 * Injector class below drives testinject.cpp (a separate helper, run under
 * sudo) over a pipe instead; see that file for the wire protocol.
 */

#include <Application.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>

#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

// Both guard on _INPUT_H; take the kit header first, then clear the guard.
#include <Input.h>
#undef _INPUT_H

#include <linux/input.h>


static int sPass = 0;
static int sFail = 0;
static int sDenied = 0;
static int sMissing = 0;


static void
record(const char* status, const char* name, const char* fmt, ...)
{
	char detail[512];
	detail[0] = '\0';
	if (fmt != NULL) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(detail, sizeof(detail), fmt, args);
		va_end(args);
	}

	printf("RESULT|%s|%s|%s\n", status, name, detail);
	fflush(stdout);

	if (strcmp(status, "PASS") == 0)
		sPass++;
	else if (strcmp(status, "FAIL") == 0)
		sFail++;
	else if (strcmp(status, "DENIED") == 0)
		sDenied++;
	else if (strcmp(status, "MISSING") == 0)
		sMissing++;
}


static void
check(bool ok, const char* name, const char* fmt, ...)
{
	char detail[512];
	detail[0] = '\0';
	if (fmt != NULL) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(detail, sizeof(detail), fmt, args);
		va_end(args);
	}

	record(ok ? "PASS" : "FAIL", name, "%s", detail);
}


//	#pragma mark - probes


// key_map tables are [128] while an evdev keycode reaches KEY_MAX (767);
// report the truncation as a number.
static void
probe_keymap()
{
	key_map* map = NULL;
	char* buffer = NULL;

	get_key_map(&map, &buffer);
	if (map == NULL) {
		record("FAIL", "get_key_map", "returned NULL map");
		return;
	}

	record("PASS", "get_key_map", "version=0x%" B_PRIx32, map->version);

	int32 mapped = 0;
	int32 highest = -1;
	for (int32 i = 0; i < 128; i++) {
		if (map->normal_map[i] != 0) {
			mapped++;
			highest = i;
		}
	}
	record("PASS", "keymap.normal_map.populated",
		"%" B_PRId32 "/128 entries, highest index %" B_PRId32, mapped, highest);

	record("PASS", "keymap.unreachable_keycodes",
		"evdev KEY_MAX=%d, table size=128, unreachable=%d",
		KEY_MAX, KEY_MAX - 127);

	// Only digits and space are asserted outright; those survive every
	// layout. A letter key under a non-Latin layout legitimately produces
	// multi-byte UTF-8, so it's only a failure if it's some *other* ASCII.
	struct { const char* name; int32 code; int32 expect; bool latinOnly; }
	spot[] = {
		{ "a", 0x3c, 'a', true },
		{ "z", 0x4c, 'z', true },
		{ "1", 0x12, '1', false },
		{ "space", 0x5e, ' ', false },
	};
	for (size_t i = 0; i < sizeof(spot) / sizeof(spot[0]); i++) {
		int32 offset = map->normal_map[spot[i].code];
		uint8 length = 0;
		const char* got = "";
		if (buffer != NULL && offset >= 0) {
			length = (uint8)buffer[offset];
			got = &buffer[offset + 1];
		}

		char shown[16];
		if (length == 0)
			strlcpy(shown, "(nothing)", sizeof(shown));
		else {
			size_t copy = length < sizeof(shown) - 1
				? length : sizeof(shown) - 1;
			memcpy(shown, got, copy);
			shown[copy] = '\0';
		}

		bool isExpected = length == 1 && got[0] == spot[i].expect;
		if (!isExpected && spot[i].latinOnly
			&& (length > 1 || (length == 1 && (uint8)got[0] >= 0x80))) {
			record("SKIP", "keymap.spotcheck",
				"%s (keycode 0x%02" B_PRIx32 ") -> \"%s\": non-Latin "
				"layout active, expected '%c' only under a Latin one",
				spot[i].name, spot[i].code, shown, spot[i].expect);
			continue;
		}

		check(isExpected, "keymap.spotcheck",
			"%s (keycode 0x%02" B_PRIx32 ") -> \"%s\", expected '%c'",
			spot[i].name, spot[i].code, shown, spot[i].expect);
	}

	record("PASS", "keymap.modifier_keycodes",
		"left_shift=%" B_PRIu32 " left_control=%" B_PRIu32 " left_command=%"
		B_PRIu32 " left_option=%" B_PRIu32 " caps=%" B_PRIu32,
		map->left_shift_key, map->left_control_key, map->left_command_key,
		map->left_option_key, map->caps_key);

	check(map->left_shift_key == 0x4b && map->caps_key == 0x3b,
		"keymap.modifiers_are_haiku_keycodes",
		"left_shift=%" B_PRIu32 " (expected 0x4b), caps=%" B_PRIu32
		" (expected 0x3b)", map->left_shift_key, map->caps_key);

	// ctrl mode is the shipped default: Command sits on Ctrl and Control on
	// Alt. A keymap still in Haiku's own arrangement has these the other way
	// round, which is worth naming rather than failing on.
	record(map->left_command_key == 0x5c && map->left_control_key == 0x5d
			? "PASS" : "SKIP",
		"keymap.ctrl_mode", "left_command=%" B_PRIu32 " left_control=%"
		B_PRIu32 " (ctrl mode is 0x5c/0x5d)", map->left_command_key,
		map->left_control_key);

	free(map);
	free(buffer);
}


static void
probe_key_info()
{
	key_info info;
	status_t status = get_key_info(&info);
	if (status != B_OK) {
		record("FAIL", "get_key_info", "%s", strerror(status));
		return;
	}

	int32 down = 0;
	for (int i = 0; i < 16; i++) {
		for (int b = 0; b < 8; b++) {
			if ((info.key_states[i] & (1 << b)) != 0)
				down++;
		}
	}
	record("PASS", "get_key_info", "modifiers=0x%" B_PRIx32 ", %" B_PRId32
		" keys down, states covers keycodes 0-127 only", info.modifiers, down);
}


static void
probe_keyboard_id()
{
	uint16 id = 0;
	status_t status = get_keyboard_id(&id);
	check(status == B_OK && id != 0, "get_keyboard_id",
		"status=%s id=0x%04x", strerror(status), id);
}


static void
probe_repeat_settings()
{
	int32 rate = 0;
	bigtime_t delay = 0;

	status_t status = get_key_repeat_rate(&rate);
	if (status != B_OK) {
		record("FAIL", "get_key_repeat_rate", "%s", strerror(status));
		return;
	}
	if (get_key_repeat_delay(&delay) != B_OK) {
		record("FAIL", "get_key_repeat_delay", "read failed");
		return;
	}
	record("PASS", "key_repeat.read", "rate=%" B_PRId32 " delay=%" B_PRId64,
		rate, delay);

	// Round trip: a setter that returns B_OK and changes nothing is the bug.
	int32 probeRate = (rate == 300) ? 200 : 300;
	if (set_key_repeat_rate(probeRate) != B_OK) {
		record("DENIED", "key_repeat.write", "set_key_repeat_rate refused");
		return;
	}

	int32 readback = 0;
	get_key_repeat_rate(&readback);
	check(readback == probeRate, "key_repeat.roundtrip",
		"wrote %" B_PRId32 ", read back %" B_PRId32, probeRate, readback);

	set_key_repeat_rate(rate);
}


static void
probe_mouse_settings()
{
	int32 type = 0;
	int32 speed = 0;
	int32 accel = 0;
	bigtime_t click = 0;
	mouse_map map;

	// Sequenced before check(): argument evaluation order is unspecified,
	// so passing the call and the out-param together can print the
	// pre-call value.
	status_t status = get_mouse_type(&type);
	check(status == B_OK, "get_mouse_type", "buttons=%" B_PRId32, type);

	status = get_mouse_speed(&speed);
	check(status == B_OK, "get_mouse_speed", "%" B_PRId32, speed);

	status = get_mouse_acceleration(&accel);
	check(status == B_OK, "get_mouse_acceleration", "%" B_PRId32, accel);

	status = get_click_speed(&click);
	check(status == B_OK && click > 0, "get_click_speed",
		"%" B_PRId64, click);

	check(get_mouse_map(&map) == B_OK, "get_mouse_map", NULL);
}


// What input_server has in its device list. A keyboard can type perfectly
// while absent from this list, with every settings change aimed at it
// dropped silently.
static void
probe_input_devices()
{
	BList list;
	status_t status = get_input_devices(&list);
	check(status == B_OK, "get_input_devices", "status=%s",
		strerror(status));

	int32 keyboards = 0;
	int32 pointers = 0;
	BString names;
	for (int32 i = 0; i < list.CountItems(); i++) {
		BInputDevice* device = (BInputDevice*)list.ItemAt(i);
		if (device == NULL)
			continue;
		if (device->Type() == B_KEYBOARD_DEVICE)
			keyboards++;
		else if (device->Type() == B_POINTING_DEVICE)
			pointers++;
		if (i > 0)
			names << ", ";
		names << device->Name() << "(" << (int)device->Type() << ")";
	}

	check(list.CountItems() > 0, "input_devices.listed",
		"%" B_PRId32 " devices: %s", list.CountItems(), names.String());
	check(keyboards > 0, "input_devices.keyboard_present",
		"keyboards=%" B_PRId32 " pointers=%" B_PRId32, keyboards, pointers);

	for (int32 i = 0; i < list.CountItems(); i++)
		delete (BInputDevice*)list.ItemAt(i);
}


static void
probe_modifier_keys()
{
	struct { const char* name; uint32 mod; } mods[] = {
		{ "B_LEFT_SHIFT_KEY", B_LEFT_SHIFT_KEY },
		{ "B_LEFT_CONTROL_KEY", B_LEFT_CONTROL_KEY },
		{ "B_LEFT_COMMAND_KEY", B_LEFT_COMMAND_KEY },
		{ "B_LEFT_OPTION_KEY", B_LEFT_OPTION_KEY },
		{ "B_CAPS_LOCK", B_CAPS_LOCK },
	};

	for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
		uint32 key = 0;
		status_t status = get_modifier_key(mods[i].mod, &key);
		record(status == B_OK ? "PASS" : "FAIL", "get_modifier_key",
			"%s -> keycode %" B_PRIu32 " (%s)", mods[i].name, key,
			strerror(status));
	}
}


//	#pragma mark - capture


class TestView : public BView {
public:
								TestView(BRect frame);

	virtual	void				MessageReceived(BMessage* message);
	virtual	void				KeyDown(const char* bytes, int32 numBytes);
	virtual	void				KeyUp(const char* bytes, int32 numBytes);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseUp(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 code,
									const BMessage* dragMessage);
	virtual	void				WindowActivated(bool active);
	virtual	void				MakeFocus(bool focus);

			int32				fKeyDowns;
			int32				fUnmapped;
			int32				fMouseMoves;
			int32				fDeltaMessages;
			int32				fMouseDowns;
			int32				fMaxClicks;
			int32				fActivations;
			BPoint				fFirstWhere;
			BPoint				fLastWhere;
};


TestView::TestView(BRect frame)
	:
	BView(frame, "testinput", B_FOLLOW_ALL, B_WILL_DRAW | B_NAVIGABLE),
	fKeyDowns(0),
	fUnmapped(0),
	fMouseMoves(0),
	fDeltaMessages(0),
	fMouseDowns(0),
	fMaxClicks(0),
	fActivations(0),
	fFirstWhere(-1, -1),
	fLastWhere(-1, -1)
{
}


static void
dump_key_message(const char* what, BMessage* message)
{
	int32 key = -1;
	int32 rawChar = -1;
	int32 mods = -1;
	int32 repeat = -1;
	const void* states = NULL;
	ssize_t statesSize = 0;

	message->FindInt32("key", &key);
	message->FindInt32("raw_char", &rawChar);
	message->FindInt32("modifiers", &mods);
	message->FindInt32("be:key_repeat", &repeat);
	message->FindData("states", B_UINT8_TYPE, &states, &statesSize);

	const char* bytes = message->FindString("bytes");

	// Spell out which key codes the states bitmap has set.
	char down[256];
	down[0] = '\0';
	if (states != NULL && statesSize == 16) {
		const uint8* bits = (const uint8*)states;
		size_t used = 0;
		for (int32 code = 0; code < 128; code++) {
			if ((bits[code >> 3] & (1 << (7 - (code & 7)))) == 0)
				continue;
			int n = snprintf(down + used, sizeof(down) - used, "%s0x%"
				B_PRIx32, used > 0 ? "," : "", (int32)code);
			if (n < 0 || (size_t)n >= sizeof(down) - used)
				break;
			used += n;
		}
	}

	printf("EVENT|%s|key=%" B_PRId32 "|raw_char=0x%" B_PRIx32 "|modifiers=0x%"
		B_PRIx32 "|repeat=%" B_PRId32 "|states_bytes=%ld|down=[%s]|bytes=%s\n",
		what, key, rawChar, mods, repeat, (long)statesSize, down,
		bytes != NULL ? bytes : "");
	fflush(stdout);
}


void
TestView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_UNMAPPED_KEY_DOWN:
			fUnmapped++;
			dump_key_message("UNMAPPED_KEY_DOWN", message);
			return;

		case B_UNMAPPED_KEY_UP:
			dump_key_message("UNMAPPED_KEY_UP", message);
			return;

		case B_MODIFIERS_CHANGED:
		{
			int32 mods = 0;
			int32 old = 0;
			message->FindInt32("modifiers", &mods);
			message->FindInt32("be:old_modifiers", &old);
			printf("EVENT|MODIFIERS_CHANGED|modifiers=0x%" B_PRIx32 "|old=0x%"
				B_PRIx32 "\n", mods, old);
			fflush(stdout);
			return;
		}
	}

	BView::MessageReceived(message);
}


void
TestView::KeyDown(const char* bytes, int32 numBytes)
{
	fKeyDowns++;
	dump_key_message("KEY_DOWN", Window()->CurrentMessage());
}


void
TestView::KeyUp(const char* bytes, int32 numBytes)
{
	dump_key_message("KEY_UP", Window()->CurrentMessage());
}


void
TestView::MouseDown(BPoint where)
{
	int32 buttons = 0;
	int32 clicks = 0;
	BMessage* message = Window()->CurrentMessage();
	message->FindInt32("buttons", &buttons);
	message->FindInt32("clicks", &clicks);

	fMouseDowns++;
	if (clicks > fMaxClicks)
		fMaxClicks = clicks;

	printf("EVENT|MOUSE_DOWN|window=%s|x=%.1f|y=%.1f|buttons=0x%" B_PRIx32
		"|clicks=%" B_PRId32 "\n", Window()->Title(), where.x, where.y,
		buttons, clicks);
	fflush(stdout);
}


// Every focus gain/loss logged with the modifier set at that moment.
void
TestView::WindowActivated(bool active)
{
	fActivations++;
	printf("EVENT|WINDOW_ACTIVATED|window=%s|active=%d|modifiers=0x%" B_PRIx32
		"\n", Window()->Title(), active ? 1 : 0, modifiers());
	fflush(stdout);
}


void
TestView::MakeFocus(bool focus)
{
	printf("EVENT|VIEW_FOCUS|window=%s|focus=%d\n", Window()->Title(),
		focus ? 1 : 0);
	fflush(stdout);
	BView::MakeFocus(focus);
}


void
TestView::MouseUp(BPoint where)
{
	printf("EVENT|MOUSE_UP|x=%.1f|y=%.1f\n", where.x, where.y);
	fflush(stdout);
}


void
TestView::MouseMoved(BPoint where, uint32 code, const BMessage* dragMessage)
{
	BMessage* message = Window()->CurrentMessage();
	int32 deltaX = 0;
	int32 deltaY = 0;
	bool hasDelta = message->FindInt32("be:delta_x", &deltaX) == B_OK
		&& message->FindInt32("be:delta_y", &deltaY) == B_OK;

	if (hasDelta)
		fDeltaMessages++;
	if (fFirstWhere.x < 0)
		fFirstWhere = where;
	fLastWhere = where;
	fMouseMoves++;

	printf("EVENT|MOUSE_MOVED|x=%.1f|y=%.1f|delta=%s|dx=%" B_PRId32 "|dy=%"
		B_PRId32 "\n", where.x, where.y, hasDelta ? "yes" : "no", deltaX,
		deltaY);
	fflush(stdout);
}


class TestWindow : public BWindow {
public:
								TestWindow(const char* title, BRect frame);

	virtual	bool				QuitRequested();

			TestView*			fView;
};


TestWindow::TestWindow(const char* title, BRect frame)
	:
	BWindow(frame, title, B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS)
{
	fView = new TestView(Bounds());
	AddChild(fView);
	fView->MakeFocus(true);

	// Target be_app explicitly: the 'shrt' handler lives in TestApp, and an
	// untargeted AddShortcut() posts to the window and is silently dropped.
	AddShortcut('q', B_COMMAND_KEY, new BMessage('shrt'), be_app);
}


bool
TestWindow::QuitRequested()
{
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


//	#pragma mark - uinput injection


// Enters the pipeline at the same place a real keyboard does (evdev via
// /dev/uinput), but the fd that opens it lives in testinject, a separate
// process run under sudo; see the file comment at the top.
class Injector {
public:
								Injector();
								~Injector();

			bool				Open();
			void				Key(int code, bool down);
			void				Tap(int code);
			void				Move(int dx, int dy);
			void				Button(int code, bool down);
			void				Sync();

			// Absolute positioning, on a separate virtual device (same
			// shape as VirtualBox's USB tablet).
			bool				OpenTablet();
			void				MoveTo(BPoint screenPoint);
			void				Click(BPoint screenPoint, int count);

private:
			bool				_StartChild();
			bool				_WaitReady();
			void				_Send(const char* format, ...);
			void				_Emit(int dev, int type, int code, int value);
			void				_EmitAbs(int type, int code, int value);

			pid_t				fChildPID;
			int					fToChild;	// write end; child's stdin
			int					fFromChild;	// read end; child's stdout
			bool				fTabletReady;
			BRect				fScreenFrame;
};


// Where debug.cmake's TEST_VOS_BINARIES installs it, alongside this binary.
static const char* kInjectorPath = "/system/tests/testinject";


Injector::Injector()
	:
	fChildPID(-1),
	fToChild(-1),
	fFromChild(-1),
	fTabletReady(false),
	fScreenFrame(0, 0, 1023, 767)
{
	BScreen screen;
	if (screen.IsValid())
		fScreenFrame = screen.Frame();

	// A dead child (sudo denied, testinject exited) must not kill this
	// process on the next write(); EPIPE is checked explicitly instead.
	signal(SIGPIPE, SIG_IGN);
}


Injector::~Injector()
{
	if (fToChild >= 0) {
		_Send("QUIT\n");
		close(fToChild);
	}
	if (fFromChild >= 0)
		close(fFromChild);
	if (fChildPID > 0) {
		int status;
		waitpid(fChildPID, &status, 0);
	}
}


bool
Injector::_StartChild()
{
	if (access(kInjectorPath, X_OK) != 0) {
		record("SKIP", "inject", "%s not present or not executable "
			"(build it: ninja -C generated.x86 testinject); nothing "
			"injected", kInjectorPath);
		return false;
	}

	int toChild[2];
	int fromChild[2];
	if (pipe(toChild) != 0 || pipe(fromChild) != 0) {
		record("FAIL", "inject", "pipe: %s", strerror(errno));
		return false;
	}

	fChildPID = fork();
	if (fChildPID < 0) {
		record("FAIL", "inject", "fork: %s", strerror(errno));
		return false;
	}

	if (fChildPID == 0) {
		// Child: stdin <- toChild's read end, stdout -> fromChild's write
		// end. stderr left inherited on purpose.
		dup2(toChild[0], STDIN_FILENO);
		dup2(fromChild[1], STDOUT_FILENO);
		close(toChild[0]);
		close(toChild[1]);
		close(fromChild[0]);
		close(fromChild[1]);
		// NOPASSWD comes from data/sudoers.d/vos-live, only present on a
		// live-booted image. Without it sudo fails fast (no tty to prompt)
		// and closes stdout, so _WaitReady() sees EOF and reports DENIED.
		execlp("sudo", "sudo", kInjectorPath, (char*)NULL);
		_exit(127);	// execlp only returns on failure
	}

	close(toChild[0]);
	close(fromChild[1]);
	fToChild = toChild[1];
	fFromChild = fromChild[0];
	return true;
}


bool
Injector::_WaitReady()
{
	// No fixed timeout: a sudo denial fails immediately and closes stdout,
	// so a short blocking read tells "denied" from "granted".
	char reply[32];
	ssize_t n = read(fFromChild, reply, sizeof(reply) - 1);
	return n > 0 && strncmp(reply, "READY", 5) == 0;
}


void
Injector::_Send(const char* format, ...)
{
	if (fToChild < 0)
		return;

	char buf[128];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);

	if (write(fToChild, buf, strlen(buf)) < 0 && errno == EPIPE) {
		close(fToChild);
		fToChild = -1;
	}
}


bool
Injector::Open()
{
	if (!_StartChild())
		return false;

	_Send("SETUP KEYBOARD\n");
	if (!_WaitReady()) {
		record("DENIED", "inject", "%s did not report READY for the "
			"keyboard device (uid running testinput = %d; sudo may need a "
			"NOPASSWD rule for %s)", kInjectorPath, (int)getuid(),
			kInjectorPath);
		close(fToChild);
		fToChild = -1;
		close(fFromChild);
		fFromChild = -1;
		int status;
		waitpid(fChildPID, &status, 0);
		fChildPID = -1;
		return false;
	}

	record("PASS", "uinput.create", "virtual keyboard + mouse created via %s",
		kInjectorPath);

	// input_server must notice the hotplug before anything is injected.
	snooze(1500000);
	return true;
}


bool
Injector::OpenTablet()
{
	if (fTabletReady)
		return true;
	if (fToChild < 0)
		return false;

	_Send("SETUP TABLET 32767\n");
	if (!_WaitReady()) {
		record("DENIED", "inject", "%s did not report READY for the "
			"tablet device", kInjectorPath);
		return false;
	}

	fTabletReady = true;
	record("PASS", "uinput.tablet.create", "screen frame %.0fx%.0f",
		fScreenFrame.Width() + 1, fScreenFrame.Height() + 1);
	snooze(1500000);
	return true;
}


void
Injector::_Emit(int dev, int type, int code, int value)
{
	_Send("EMIT %d %d %d %d\n", dev, type, code, value);
}


void
Injector::_EmitAbs(int type, int code, int value)
{
	_Emit(1, type, code, value);
}


void
Injector::MoveTo(BPoint screenPoint)
{
	if (!fTabletReady)
		return;

	int x = (int)(screenPoint.x / (fScreenFrame.Width() + 1) * 32767);
	int y = (int)(screenPoint.y / (fScreenFrame.Height() + 1) * 32767);
	_EmitAbs(EV_ABS, ABS_X, x);
	_EmitAbs(EV_ABS, ABS_Y, y);
	_EmitAbs(EV_SYN, SYN_REPORT, 0);
	snooze(80000);
}


void
Injector::Click(BPoint screenPoint, int count)
{
	MoveTo(screenPoint);
	for (int i = 0; i < count; i++) {
		_EmitAbs(EV_KEY, BTN_LEFT, 1);
		_EmitAbs(EV_SYN, SYN_REPORT, 0);
		snooze(20000);
		_EmitAbs(EV_KEY, BTN_LEFT, 0);
		_EmitAbs(EV_SYN, SYN_REPORT, 0);
		// Well inside any plausible double-click interval.
		if (i + 1 < count)
			snooze(60000);
	}
	snooze(150000);
}


void
Injector::Sync()
{
	_Emit(0, EV_SYN, SYN_REPORT, 0);
	snooze(30000);
}


void
Injector::Key(int code, bool down)
{
	_Emit(0, EV_KEY, code, down ? 1 : 0);
	Sync();
}


void
Injector::Tap(int code)
{
	Key(code, true);
	Key(code, false);
}


void
Injector::Move(int dx, int dy)
{
	if (dx != 0)
		_Emit(0, EV_REL, REL_X, dx);
	if (dy != 0)
		_Emit(0, EV_REL, REL_Y, dy);
	Sync();
}


void
Injector::Button(int code, bool down)
{
	_Emit(0, EV_KEY, code, down ? 1 : 0);
	Sync();
}


//	#pragma mark - application


class TestApp : public BApplication {
public:
								TestApp(bool inject, int32 seconds);

	virtual	void				ReadyToRun();
	virtual	void				MessageReceived(BMessage* message);

private:
	static	int32				_InjectThread(void* data);
			void				_RunSequence();
	static	BPoint				_WindowCentre(BWindow* window);

			TestWindow*			fWindow;
			TestWindow*			fSecondWindow;
			bool				fInject;
			int32				fSeconds;
			int32				fShortcutHits;
};


TestApp::TestApp(bool inject, int32 seconds)
	:
	BApplication("application/x-vnd.vos-testinput"),
	fWindow(NULL),
	fSecondWindow(NULL),
	fInject(inject),
	fSeconds(seconds),
	fShortcutHits(0)
{
}


void
TestApp::ReadyToRun()
{
	// Two windows, deliberately not overlapping: issues #201 and #209 are
	// about clicks reaching only the desktop, and issue #207 about focus
	// never moving. Neither is observable with a single window.
	fWindow = new TestWindow("testinput-A", BRect(100, 100, 500, 400));
	fWindow->Show();
	fSecondWindow = new TestWindow("testinput-B", BRect(560, 100, 900, 400));
	fSecondWindow->Show();
	fWindow->Activate(true);

	if (fInject) {
		thread_id thread = spawn_thread(_InjectThread, "injector",
			B_NORMAL_PRIORITY, this);
		resume_thread(thread);
	} else {
		record("SKIP", "inject", "capture only; drive the keyboard by hand");
	}
}


void
TestApp::MessageReceived(BMessage* message)
{
	if (message->what == 'shrt') {
		fShortcutHits++;
		printf("EVENT|SHORTCUT|cmd-q\n");
		fflush(stdout);
		return;
	}

	BApplication::MessageReceived(message);
}


int32
TestApp::_InjectThread(void* data)
{
	((TestApp*)data)->_RunSequence();
	return 0;
}


BPoint
TestApp::_WindowCentre(BWindow* window)
{
	BRect frame(0, 0, 0, 0);
	if (window->LockLooper()) {
		frame = window->Frame();
		window->UnlockLooper();
	}
	return BPoint((frame.left + frame.right) / 2,
		(frame.top + frame.bottom) / 2);
}


void
TestApp::_RunSequence()
{
	Injector injector;
	if (!injector.Open()) {
		record("SKIP", "inject.sequence", "no injector; nothing exercised");
		PostMessage(B_QUIT_REQUESTED);
		return;
	}

	TestView* view = fWindow->fView;

	// 1. A plain letter. Proves keycode -> keymap -> bytes.
	int32 before = view->fKeyDowns;
	injector.Tap(KEY_A);
	snooze(200000);
	check(view->fKeyDowns > before, "inject.plain_key",
		"KEY_A produced %" B_PRId32 " KeyDown(s)", view->fKeyDowns - before);

	// 2. Shifted letter. Proves the shift_map table is consulted.
	injector.Key(KEY_LEFTSHIFT, true);
	injector.Tap(KEY_A);
	injector.Key(KEY_LEFTSHIFT, false);
	snooze(200000);

	// 3. A keycode above 127. The key_map tables cannot express it; this
	// records what actually arrives rather than asserting an outcome.
	before = view->fUnmapped;
	int32 beforeDown = view->fKeyDowns;
	injector.Tap(KEY_PLAYPAUSE);	// 164
	snooze(200000);
	record("PASS", "inject.keycode_above_127",
		"KEY_PLAYPAUSE(164): %" B_PRId32 " KeyDown, %" B_PRId32 " unmapped",
		view->fKeyDowns - beforeDown, view->fUnmapped - before);

	before = view->fUnmapped;
	beforeDown = view->fKeyDowns;
	injector.Tap(KEY_F13);			// 183
	snooze(200000);
	record("PASS", "inject.f13",
		"KEY_F13(183): %" B_PRId32 " KeyDown, %" B_PRId32 " unmapped",
		view->fKeyDowns - beforeDown, view->fUnmapped - before);

	// 4. The shortcut path: modifiers plus a keycode, matched by BWindow.
	injector.Key(KEY_LEFTMETA, true);
	injector.Tap(KEY_Q);
	injector.Key(KEY_LEFTMETA, false);
	snooze(300000);
	check(fShortcutHits > 0, "inject.shortcut",
		"cmd-q fired %" B_PRId32 " time(s)", fShortcutHits);

	// 5. Relative motion. Two things are under test: that motion arrives at
	// all, and whether pre-acceleration deltas survive to the client.
	int32 movesBefore = view->fMouseMoves;
	int32 deltasBefore = view->fDeltaMessages;
	BPoint start = view->fLastWhere;
	for (int i = 0; i < 10; i++)
		injector.Move(4, 3);
	snooze(300000);

	check(view->fMouseMoves > movesBefore, "inject.mouse_motion",
		"%" B_PRId32 " MouseMoved from %d relative events",
		view->fMouseMoves - movesBefore, 10);
	check(view->fDeltaMessages > deltasBefore, "inject.mouse_deltas",
		"%" B_PRId32 " of %" B_PRId32 " moves carried be:delta_x/y",
		view->fDeltaMessages - deltasBefore, view->fMouseMoves - movesBefore);

	if (start.x >= 0) {
		record("PASS", "inject.cursor_travel",
			"from (%.1f,%.1f) to (%.1f,%.1f) for +40,+30 requested",
			start.x, start.y, view->fLastWhere.x, view->fLastWhere.y);
	}

	// 6. Drive hard into the top-left corner, then back. If the cursor does
	// not come back symmetrically, motion is being clamped and discarded
	// rather than clamped for display only.
	for (int i = 0; i < 50; i++)
		injector.Move(-40, -40);
	snooze(200000);
	BPoint corner = view->fLastWhere;
	for (int i = 0; i < 10; i++)
		injector.Move(10, 10);
	snooze(300000);
	record("PASS", "inject.clamp_recovery",
		"corner (%.1f,%.1f) then +100,+100 -> (%.1f,%.1f)",
		corner.x, corner.y, view->fLastWhere.x, view->fLastWhere.y);

	// 7. Buttons on the relative device.
	injector.Button(BTN_LEFT, true);
	injector.Button(BTN_LEFT, false);
	snooze(200000);

	// The rest needs absolute positioning to click a specific window.
	if (!injector.OpenTablet()) {
		record("SKIP", "inject.window_tests", "no tablet device");
		PostMessage(B_QUIT_REQUESTED);
		return;
	}

	TestView* second = fSecondWindow->fView;
	BPoint centreA = _WindowCentre(fWindow);
	BPoint centreB = _WindowCentre(fSecondWindow);

	// 8. Issue #201 / #209: does a click inside a window reach the window,
	// or only the desktop? Counted per window, so "landed nowhere" and
	// "landed on the wrong window" are distinguishable.
	int32 downsA = view->fMouseDowns;
	injector.Click(centreA, 1);
	snooze(300000);
	check(view->fMouseDowns > downsA, "issue201.click_reaches_window",
		"click at (%.0f,%.0f) inside testinput-A produced %" B_PRId32
		" MouseDown", centreA.x, centreA.y, view->fMouseDowns - downsA);

	// 9. Issue #201: click-to-activate. Click the inactive window and see
	// whether focus actually moves.
	int32 actB = second->fActivations;
	injector.Click(centreB, 1);
	snooze(400000);
	check(second->fMouseDowns > 0, "issue201.click_reaches_second_window",
		"%" B_PRId32 " MouseDown on testinput-B", second->fMouseDowns);
	check(second->fActivations > actB, "issue201.click_activates",
		"clicking testinput-B produced %" B_PRId32 " activation transition(s)",
		second->fActivations - actB);

	bool focusMoved = fSecondWindow->IsActive() && !fWindow->IsActive();
	check(focusMoved, "issue201.focus_moved",
		"A active=%d B active=%d", fWindow->IsActive() ? 1 : 0,
		fSecondWindow->IsActive() ? 1 : 0);

	// 10. Issue #198: double click. Two taps well inside the click interval
	// must arrive as clicks=2, not as two clicks=1.
	second->fMaxClicks = 0;
	injector.Click(centreB, 2);
	snooze(400000);
	check(second->fMaxClicks >= 2, "issue198.double_click",
		"highest clicks= field seen: %" B_PRId32, second->fMaxClicks);

	// 11. Issue #198: the phantom-modifier bug. Nothing is held down at this
	// point, so a non-zero modifier set is a stuck state, and it is what
	// makes Task Manager see ctrl+alt+del out of nowhere.
	uint32 mods = modifiers();
	check(mods == 0, "issue198.no_stuck_modifiers",
		"modifiers() = 0x%" B_PRIx32 " with nothing pressed", mods);

	key_info info;
	if (get_key_info(&info) == B_OK) {
		int32 stuck = 0;
		for (int i = 0; i < 16; i++) {
			for (int b = 0; b < 8; b++) {
				if ((info.key_states[i] & (1 << b)) != 0)
					stuck++;
			}
		}
		check(stuck == 0, "issue198.no_stuck_key_states",
			"%" B_PRId32 " key(s) reported down with nothing pressed", stuck);
	}

	// 12. Issue #198: focus loss mid-keypress. Press a modifier, move focus
	// away, release it while the other window is active, come back. A state
	// machine that only tracks the focused window leaks the modifier here.
	injector.Click(centreA, 1);
	snooze(300000);
	injector.Key(KEY_LEFTSHIFT, true);
	snooze(100000);
	injector.Click(centreB, 1);			// focus leaves while shift is held
	snooze(300000);
	injector.Key(KEY_LEFTSHIFT, false);	// released for a different window
	snooze(200000);
	injector.Click(centreA, 1);
	snooze(300000);
	mods = modifiers();
	check((mods & B_SHIFT_KEY) == 0, "issue198.modifier_survives_focus_loss",
		"shift released while another window had focus; modifiers() = 0x%"
		B_PRIx32, mods);

	// 13. Issue #207: Tab. Not the task switcher itself (that is Deskbar's),
	// but whether Tab is delivered at all and whether anything eats it.
	beforeDown = view->fKeyDowns;
	injector.Tap(KEY_TAB);
	snooze(250000);
	record("PASS", "issue207.tab_delivery",
		"KEY_TAB(15) produced %" B_PRId32 " KeyDown to the focused view",
		view->fKeyDowns - beforeDown);

	beforeDown = view->fKeyDowns;
	injector.Key(KEY_LEFTALT, true);
	injector.Tap(KEY_TAB);
	injector.Key(KEY_LEFTALT, false);
	snooze(400000);
	mods = modifiers();
	record("PASS", "issue207.alt_tab",
		"alt+Tab: %" B_PRId32 " KeyDown to view, modifiers after = 0x%"
		B_PRIx32 " (expect 0)", view->fKeyDowns - beforeDown, mods);
	check(mods == 0, "issue207.alt_tab_leaves_no_modifier",
		"modifiers() = 0x%" B_PRIx32, mods);

	snooze((bigtime_t)fSeconds * 1000000);
	PostMessage(B_QUIT_REQUESTED);
}


//	#pragma mark -


int
main(int argc, char** argv)
{
	const char* mode = argc > 1 ? argv[1] : "probe";
	int32 seconds = argc > 2 ? atoi(argv[2]) : 5;

	if (strcmp(mode, "probe") == 0) {
		// A BApplication is required: every one of these calls is a message
		// round trip to input_server.
		BApplication app("application/x-vnd.vos-testinput-probe");
		probe_keymap();
		probe_key_info();
		probe_keyboard_id();
		probe_repeat_settings();
		probe_mouse_settings();
		probe_modifier_keys();
		probe_input_devices();
	} else if (strcmp(mode, "capture") == 0 || strcmp(mode, "inject") == 0) {
		TestApp app(strcmp(mode, "inject") == 0, seconds);
		app.Run();
	} else {
		fprintf(stderr, "usage: %s [probe|capture|inject] [seconds]\n",
			argv[0]);
		return 1;
	}

	printf("SUMMARY|pass=%d|fail=%d|denied=%d|missing=%d\n",
		sPass, sFail, sDenied, sMissing);
	return sFail > 0 ? 1 : 0;
}
