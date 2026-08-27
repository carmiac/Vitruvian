/*
 * Copyright 2004-2009 Haiku Inc. All rights reserved.
 * Copyright 2026, Dario Casalinuovo <b.vitruvio@gmail.com>.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Sandor Vroemisse
 *		Jérôme Duval
 *		Dario Casalinuovo
 */
#ifndef KEYMAP_LIST_ITEM_H
#define KEYMAP_LIST_ITEM_H


/*
 * A BStringItem modified so that it holds the BEntry object of the
 * corresponding keymap.
 */


#include <ListItem.h>
#include <Entry.h>
#include <String.h>


class BBitmap;


class KeymapListItem : public BStringItem {
public:
								KeymapListItem(entry_ref& keymap,
									const char* name = NULL);
								// An xkb-derived row; no real file behind
								// it, the id routes the selection.
								KeymapListItem(const BString& xkbId,
									const char* name, uint32 outlineLevel = 0,
									bool expanded = true);
	virtual						~KeymapListItem();

			entry_ref&			EntryRef() { return fKeymap; };
			const BString&		XkbId() const { return fXkbId; }

	virtual	void				Update(BView* owner, const BFont* font);

	virtual	void				DrawItem(BView* owner, BRect frame,
									bool complete = false);

protected:
			entry_ref			fKeymap;
			BString				fXkbId;

private:
			BString				_FlagCountryCode() const;

			BBitmap*			fIcon;
};

#endif	// KEYMAP_LIST_ITEM_H
