/*
 * Copyright 2004-2006 Haiku Inc. All rights reserved.
 * Copyright 2026, Dario Casalinuovo <b.vitruvio@gmail.com>.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Sandor Vroemisse
 *		Jérôme Duval
 *		Dario Casalinuovo
 */


#include "KeymapListItem.h"

#include <new>

#include <Bitmap.h>
#include <ControlLook.h>
#include <GraphicsDefs.h>
#include <LocaleRoster.h>


KeymapListItem::KeymapListItem(entry_ref& keymap, const char* name)
	:
	BStringItem(name != NULL ? name : keymap.name),
	fKeymap(keymap),
	fIcon(NULL)
{
}


KeymapListItem::KeymapListItem(const BString& xkbId, const char* name,
	uint32 outlineLevel, bool expanded)
	:
	BStringItem(name, outlineLevel, expanded),
	fXkbId(xkbId),
	fIcon(NULL)
{
	// A fake ref, so name matching against the active keymap keeps working.
	fKeymap.set_name(name);
}


KeymapListItem::~KeymapListItem()
{
	delete fIcon;
}


BString
KeymapListItem::_FlagCountryCode() const
{
	if (fXkbId.Length() == 0)
		return BString();

	BString code(fXkbId);
	if (code.FindFirst("xkb:") == 0)
		code.Remove(0, 4);

	// Variant belongs to the parent's country; drop the variant half
	// ("xkb:de:nodeadkeys" -> German flag).
	int32 colon = code.FindFirst(':');
	if (colon >= 0)
		code.Truncate(colon);

	// GetFlagIconForCountry() uses the last 2 chars; reject non-ISO
	// lengths, or it would pick the wrong flag.
	if (code.Length() != 2)
		return BString();

	return code;
}


void
KeymapListItem::Update(BView* owner, const BFont* font)
{
	BStringItem::Update(owner, font);

	// Reserve the icon column on every row (flag or not), to keep
	// labels aligned.
	float iconSize = Height();
	SetWidth(Width() + iconSize + be_control_look->DefaultLabelSpacing());

	BString countryCode = _FlagCountryCode();
	if (countryCode.IsEmpty())
		return;

	// Update() runs again on a font or attach change; drop the previous
	// bitmap or it leaks.
	delete fIcon;
	fIcon = new(std::nothrow) BBitmap(BRect(0, 0, iconSize - 1, iconSize - 1),
		B_RGBA32);
	if (fIcon != NULL && BLocaleRoster::Default()->GetFlagIconForCountry(
			fIcon, countryCode.String()) != B_OK) {
		delete fIcon;
		fIcon = NULL;
	}
}


void
KeymapListItem::DrawItem(BView* owner, BRect frame, bool complete)
{
	if (Text() == NULL) {
		BStringItem::DrawItem(owner, frame, complete);
		return;
	}

	float iconSize = fIcon != NULL && fIcon->IsValid()
		? fIcon->Bounds().Width() : Height();
	float spacing = be_control_look->DefaultLabelSpacing();
	rgb_color lowColor = owner->LowColor();

	if (IsSelected() || complete) {
		owner->SetLowColor(IsSelected()
			? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
			: owner->ViewColor());
		owner->FillRect(frame, B_SOLID_LOW);
	} else
		owner->SetLowColor(owner->ViewColor());

	BRect iconFrame(frame.left + spacing, frame.top,
		frame.left + spacing + iconSize - 1, frame.top + iconSize - 1);

	if (fIcon != NULL && fIcon->IsValid()) {
		owner->SetDrawingMode(B_OP_OVER);
		owner->DrawBitmap(fIcon, iconFrame);
		owner->SetDrawingMode(B_OP_COPY);
	} else {
		// No single country to show (e.g. "ara" spans many languages);
		// draw a muted outline as the flag slot.
		rgb_color border = ui_color(B_CONTROL_BORDER_COLOR);
		border.alpha = 90;
		rgb_color high = owner->HighColor();
		owner->SetDrawingMode(B_OP_ALPHA);
		owner->SetHighColor(border);
		owner->StrokeRoundRect(iconFrame.InsetByCopy(0, iconSize / 6), 2, 2);
		owner->SetHighColor(high);
		owner->SetDrawingMode(B_OP_COPY);
	}

	// Text offset by icon column (flag or not) to keep labels aligned
	owner->MovePenTo(frame.left + spacing * 2 + iconSize,
		frame.top + BaselineOffset());
	owner->DrawString(Text());

	owner->SetLowColor(lowColor);
}
