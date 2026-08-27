/*
 * Copyright 2019-2020, Haiku, Inc.
 * Copyright 2026, Dario Casalinuovo <b.vitruvio@gmail.com>.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Preetpal Kaur <preetpalok123@gmail.com>
 *		Adrien Destugues <pulkomandy@gmail.com>
 *		Dario Casalinuovo
 */


#include <ctype.h>

#include <CardLayout.h>
#include <CardView.h>
#include <Catalog.h>
#include <Control.h>
#include <ControlLook.h>
#include <InputServerDevice.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>

#include <private/input/InputServerTypes.h>

#include "InputConstants.h"
#include "InputDeviceView.h"
#include "InputKeyboard.h"
#include "InputMouse.h"
#include "InputTouchpadPref.h"
#include "InputTouchpadPrefView.h"
#include "InputWindow.h"
#include "MouseSettings.h"
#include "SettingsView.h"

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InputWindow"


InputWindow::InputWindow(BRect rect)
	:
	BWindow(rect, B_TRANSLATE_SYSTEM_NAME("Input"), B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS
			| B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE)
{
	fDeviceListView = new BListView(B_TRANSLATE("Device List"));
	fDeviceListView->SetSelectionMessage(new BMessage(ITEM_SELECTED));
	fDeviceListView->SetExplicitMinSize(
		BSize(be_control_look->ComposeIconSize(32).Width()
				+ fDeviceListView->StringWidth("Extended PS/2 Mouse 1"),
			B_SIZE_UNSET));

	BScrollView* scrollView = new BScrollView(
		"scrollView", fDeviceListView, 0, false, B_FANCY_BORDER);
	fCardView = new BCardView();

	BLayoutBuilder::Group<>(this, B_HORIZONTAL, 10)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(scrollView, 1)
		.Add(fCardView, 3);

	FindDevice();
}


void
InputWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case ITEM_SELECTED:
		{
			int32 index = message->GetInt32("index", 0);
			if (index >= 0)
				fCardView->CardLayout()->SetVisibleItem(index);
			break;
		}
		case kMsgMouseType:
		case kMsgMouseMap:
		case kMsgMouseFocusMode:
		case kMsgFollowsMouseMode:
		case kMsgAcceptFirstClick:
		case kMsgDoubleClickSpeed:
		case kMsgMouseSpeed:
		case kMsgAccelerationFactor:
		case kMsgDefaults:
		case kMsgRevert:
		{
			PostMessage(
				message, fCardView->CardLayout()->VisibleItem()->View());
			break;
		}
		case SCROLL_AREA_CHANGED:
		case SCROLL_CONTROL_CHANGED:
		case TAP_CONTROL_CHANGED:
		case PAD_SPEED_CHANGED:
		case PAD_ACCELERATION_CHANGED:
		case DEFAULT_SETTINGS:
		case REVERT_SETTINGS:
		{
			PostMessage(
				message, fCardView->CardLayout()->VisibleItem()->View());
			break;
		}
		case kMsgSliderrepeatrate:
		case kMsgSliderdelayrate:
		{
			PostMessage(
				message, fCardView->CardLayout()->VisibleItem()->View());
			break;
		}

		case B_INPUT_DEVICES_CHANGED:
		{
			int32 operation = message->FindInt32("be:opcode");
			BString name = message->FindString("be:device_name");

			if (operation == B_INPUT_DEVICE_ADDED) {
				BInputDevice* device = find_input_device(name);
				if (device)
					AddDevice(device);
			} else if (operation == B_INPUT_DEVICE_REMOVED) {
				for (int i = 0; i < fDeviceListView->CountItems(); i++) {
					DeviceListItemView* item
						= dynamic_cast<DeviceListItemView*>(
							fDeviceListView->ItemAt(i));
					if (item != NULL && item->DeviceName() == name) {
						fDeviceListView->RemoveItem(i);
						BView* settings = fCardView->ChildAt(i);
						fCardView->RemoveChild(settings);
						delete settings;
						break;
					}
				}
			}

			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
InputWindow::Show()
{
	CenterOnScreen();
	BWindow::Show();
	watch_input_devices(this, true);
}


void
InputWindow::Hide()
{
	BWindow::Hide();
	watch_input_devices(this, false);
}


status_t
InputWindow::FindDevice()
{
	BList devList;
	status_t status = get_input_devices(&devList);
	if (status != B_OK)
		return status;

	int32 i = 0;

	while (true) {
		BInputDevice* dev = (BInputDevice*)devList.ItemAt(i);
		if (dev == NULL)
			break;
		i++;

		AddDevice(dev);
	}

	return B_OK;
}


// Drops vid:pid token from i2c-HID device names ("ASUE140D:00 04F3:31B9 Mouse" -> "ASUE140D:00 Mouse")
// ACPI HID stays (distinguishes devices on same controller)
static BString
tidy_device_name(const BString& raw)
{
	BString result;
	int32 start = 0;

	while (start < raw.Length()) {
		int32 end = raw.FindFirst(' ', start);
		if (end < 0)
			end = raw.Length();

		BString token;
		raw.CopyInto(token, start, end - start);

		bool isIdPair = token.Length() == 9 && token[4] == ':';
		for (int32 i = 0; isIdPair && i < token.Length(); i++) {
			if (i == 4)
				continue;
			if (!isxdigit(token[i]))
				isIdPair = false;
		}

		if (!isIdPair && token.Length() > 0) {
			if (result.Length() > 0)
				result << ' ';
			result << token;
		}

		start = end + 1;
	}

	return result.Length() > 0 ? result : raw;
}


void
InputWindow::AddDevice(BInputDevice* dev)
{
	// Identity and settings key; never replace with the description.
	BString name = dev->Name();

	// Display only; falls back to the identity name if the add-on doesn't
	// implement B_GET_DEVICE_DESCRIPTION.
	BString hardwareName = name;
	BMessage description;
	if (dev->Control(B_GET_DEVICE_DESCRIPTION, &description) == B_OK) {
		BString reported;
		if (description.FindString("description", &reported) == B_OK
			&& !reported.IsEmpty()) {
			hardwareName = reported;
		}
	}

	// The list gets the tidied form; the device's own view shows the full
	// hardware name at the top.
	BString displayName = tidy_device_name(hardwareName);

	if (dev->Type() == B_POINTING_DEVICE && name.FindFirst("Touchpad") >= 0) {
		TouchpadPrefView* view = new TouchpadPrefView(dev,
			hardwareName.String());
		fCardView->AddChild(view);

		DeviceListItemView* touchpad
			= new DeviceListItemView(displayName, TOUCHPAD_TYPE, name);
		fDeviceListView->AddItem(touchpad);
	} else if (dev->Type() == B_POINTING_DEVICE) {
		MouseSettings* settings;
		settings = fMultipleMouseSettings.AddMouseSettings(name);

		InputMouse* view = new InputMouse(dev, settings,
			hardwareName.String());
		fCardView->AddChild(view);

		DeviceListItemView* mouse
			= new DeviceListItemView(displayName, MOUSE_TYPE, name);
		fDeviceListView->AddItem(mouse);
	} else if (dev->Type() == B_KEYBOARD_DEVICE) {
		InputKeyboard* view = new InputKeyboard(dev, hardwareName.String());
		fCardView->AddChild(view);

		DeviceListItemView* keyboard
			= new DeviceListItemView(displayName, KEYBOARD_TYPE, name);
		fDeviceListView->AddItem(keyboard);
	} else
		delete dev;
}
