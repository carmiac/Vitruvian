/*
 * Copyright 2026, Vitruvian Project.
 * Distributed under the terms of the MIT License.
 *
 * testwatch: isolates which layer of directory watching is silent.
 *
 * The input_server relies on BPathMonitor to notice keyboards and mice
 * appearing in /dev/input. When a hotplugged device is never picked up,
 * the break can be in BPathMonitor, in watch_node(), or in the kernel's
 * notification path; and from the input_server alone the three are
 * indistinguishable. This watches the same directory three ways at once
 * and prints whatever arrives, so the silent layer names itself.
 *
 *   testwatch [seconds] [directory]
 *
 * Defaults to 30 seconds on /dev/input. A control directory under /tmp is
 * always watched alongside it: if the control fires and the target does
 * not, the mechanism works and the target's filesystem is the problem.
 */


#include <Entry.h>
#include <Handler.h>
#include <Looper.h>
#include <NodeMonitor.h>
#include <PathMonitor.h>
#include <String.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


using namespace BPrivate;


static const char* kControlDir = "/tmp/testwatch-control";


static const char*
opcode_name(int32 opcode)
{
	switch (opcode) {
		case B_ENTRY_CREATED:	return "B_ENTRY_CREATED";
		case B_ENTRY_REMOVED:	return "B_ENTRY_REMOVED";
		case B_ENTRY_MOVED:		return "B_ENTRY_MOVED";
		case B_STAT_CHANGED:	return "B_STAT_CHANGED";
		case B_ATTR_CHANGED:	return "B_ATTR_CHANGED";
		case B_DEVICE_MOUNTED:	return "B_DEVICE_MOUNTED";
		case B_DEVICE_UNMOUNTED: return "B_DEVICE_UNMOUNTED";
		default:				return "?";
	}
}


class WatchHandler : public BHandler {
public:
								WatchHandler(const char* tag);

	virtual	void				MessageReceived(BMessage* message);

			int32				fCount;

private:
			const char*			fTag;
};


WatchHandler::WatchHandler(const char* tag)
	:
	BHandler(tag),
	fCount(0),
	fTag(tag)
{
}


void
WatchHandler::MessageReceived(BMessage* message)
{
	if (message->what != B_PATH_MONITOR && message->what != B_NODE_MONITOR) {
		BHandler::MessageReceived(message);
		return;
	}

	fCount++;

	int32 opcode = -1;
	message->FindInt32("opcode", &opcode);

	const char* path = NULL;
	const char* name = NULL;
	message->FindString("path", &path);
	message->FindString("name", &name);

	// BPathMonitor bails out silently unless both "virtual:node" and
	// "virtual:directory" are present, so log which fields actually shipped.
	BString fields;
	char* fieldName;
	type_code fieldType;
	int32 fieldCount;
	for (int32 i = 0; message->GetInfo(B_ANY_TYPE, i, &fieldName, &fieldType,
			&fieldCount) == B_OK; i++) {
		if (i > 0)
			fields << ",";
		fields << fieldName;
	}

	printf("EVENT|%s|what=%.4s|opcode=%s(%" B_PRId32 ")|path=%s|name=%s"
		"|fields=%s\n",
		fTag, (char*)&message->what, opcode_name(opcode), opcode,
		path != NULL ? path : "-", name != NULL ? name : "-",
		fields.String());
	fflush(stdout);
}


static void
report(const char* what, status_t status)
{
	printf("RESULT|%s|%s|status=%s (0x%" B_PRIx32 ")\n",
		status == B_OK ? "PASS" : "FAIL", what, strerror(status),
		(int32)status);
	fflush(stdout);
}


static void
watch_three_ways(BLooper* looper, const char* dir, const char* tag,
	WatchHandler* pathHandler, WatchHandler* dirPathHandler,
	WatchHandler* nodeHandler)
{
	BString label;

	// 1. Exactly what AddOnManager::StartMonitoringDevice() does.
	label.SetToFormat("BPathMonitor.files_only.%s", tag);
	report(label.String(), BPathMonitor::StartWatching(dir,
		B_WATCH_FILES_ONLY | B_WATCH_RECURSIVELY,
		BMessenger(pathHandler, looper)));

	// 1b. Same layer, but asking for directory entries explicitly. If this
	// one fires and (1) does not, the flags AddOnManager passes are the
	// problem rather than BPathMonitor itself.
	label.SetToFormat("BPathMonitor.directory.%s", tag);
	report(label.String(), BPathMonitor::StartWatching(dir,
		B_WATCH_DIRECTORY, BMessenger(dirPathHandler, looper)));

	// 2. The directory node itself, no path-monitor layer in between.
	BEntry entry(dir);
	node_ref ref;
	status_t status = entry.GetNodeRef(&ref);
	label.SetToFormat("BEntry.GetNodeRef.%s", tag);
	report(label.String(), status);

	if (status == B_OK) {
		// vdevice/vnode is what watch_node() actually passes down.
		printf("INFO|%s|node_ref real dev=%lld ino=%lld"
			" | virtual dev=%lld ino=%lld\n",
			tag, (long long)ref.device(), (long long)ref.node(),
			(long long)ref.vdevice(), (long long)ref.vnode());
		fflush(stdout);

		label.SetToFormat("watch_node.directory.%s", tag);
		report(label.String(), watch_node(&ref, B_WATCH_DIRECTORY,
			BMessenger(nodeHandler, looper)));
	}
}


int
main(int argc, char** argv)
{
	int32 seconds = argc > 1 ? atoi(argv[1]) : 30;
	const char* target = argc > 2 ? argv[2] : "/dev/input";

	mkdir(kControlDir, 0755);

	BLooper* looper = new BLooper("testwatch");

	WatchHandler targetPath("target.path");
	WatchHandler targetDirPath("target.dirpath");
	WatchHandler targetNode("target.node");
	WatchHandler controlPath("control.path");
	WatchHandler controlDirPath("control.dirpath");
	WatchHandler controlNode("control.node");

	looper->Lock();
	looper->AddHandler(&targetPath);
	looper->AddHandler(&targetDirPath);
	looper->AddHandler(&targetNode);
	looper->AddHandler(&controlPath);
	looper->AddHandler(&controlDirPath);
	looper->AddHandler(&controlNode);
	looper->Unlock();

	looper->Run();

	printf("INFO|target=%s control=%s seconds=%" B_PRId32 "\n",
		target, kControlDir, seconds);
	fflush(stdout);

	watch_three_ways(looper, target, "target", &targetPath, &targetDirPath,
		&targetNode);
	watch_three_ways(looper, kControlDir, "control", &controlPath,
		&controlDirPath, &controlNode);

	// Prove the control path can fire at all: create and remove a file in
	// the control directory once watching is established.
	snooze(500000);
	BString probe(kControlDir);
	probe << "/probe-file";
	close(creat(probe.String(), 0644));
	snooze(500000);
	unlink(probe.String());

	printf("INFO|control probe file created and removed; now waiting\n");
	fflush(stdout);

	snooze((bigtime_t)seconds * 1000000LL);

	printf("SUMMARY|target.path=%" B_PRId32 "|target.dirpath=%" B_PRId32
		"|target.node=%" B_PRId32 "|control.path=%" B_PRId32
		"|control.dirpath=%" B_PRId32 "|control.node=%" B_PRId32 "\n",
		targetPath.fCount, targetDirPath.fCount, targetNode.fCount,
		controlPath.fCount, controlDirPath.fCount, controlNode.fCount);
	fflush(stdout);

	BPathMonitor::StopWatching(BMessenger(&targetPath, looper));
	BPathMonitor::StopWatching(BMessenger(&targetDirPath, looper));
	BPathMonitor::StopWatching(BMessenger(&controlPath, looper));
	BPathMonitor::StopWatching(BMessenger(&controlDirPath, looper));
	stop_watching(BMessenger(&targetNode, looper));
	stop_watching(BMessenger(&controlNode, looper));

	// BLooper::Quit() deletes the handlers it owns, and these live on the
	// stack; take them back first.
	looper->Lock();
	looper->RemoveHandler(&targetPath);
	looper->RemoveHandler(&targetDirPath);
	looper->RemoveHandler(&targetNode);
	looper->RemoveHandler(&controlPath);
	looper->RemoveHandler(&controlDirPath);
	looper->RemoveHandler(&controlNode);
	looper->Quit();

	return 0;
}
