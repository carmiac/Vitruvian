/*
 * Copyright 2004-2014 Haiku, Inc. All rights reserved.
 * Copyright 2026, Dario Casalinuovo <b.vitruvio@gmail.com>.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Alexandre Deckner, alex@zappotek.com
 *		Axel Dörfler, axeld@pinc-software.de
 *		Jérôme Duval
 *		John Scipione, jscipione@gmai.com
 *		Sandor Vroemisse
 *		Dario Casalinuovo
 */
#ifndef KEYMAP_WINDOW_H
#define KEYMAP_WINDOW_H


#include <FilePanel.h>
#include <ListView.h>
#include <OutlineListView.h>
#include <String.h>
#include <Window.h>

#include "Keymap.h"


class BMenu;
class BMenuBar;
class BMenuField;
class BTextControl;
class KeyboardLayoutView;
class KeymapListItem;


class KeymapWindow : public BWindow {
public:
								KeymapWindow();
	virtual						~KeymapWindow();

	virtual	bool				QuitRequested();
	virtual void				MessageReceived(BMessage* message);

protected:
			BMenuBar*			_CreateMenu();
			BView*				_CreateMapLists();
			status_t			_SetKeyboardLayout(const char* path);
			status_t			_FindKeyboardLayoutPath(const char* name,
									BPath& _path);
			void				_AutoPickKeyboardTemplate();

			void				_AddModelMenu(BMenu* menu);
			void				_MarkCurrentModel();

			void				_XkbLayoutSelected(const char* id,
									const char* label);

			void				_UpdateSwitchShortcutButton();
			void				_UpdateButtons();
			void				_SwitchShortcutKeys();

			void				_UseKeymap();
			void				_DefaultKeymap();
			void				_RevertKeymap();

			BMenuField*			_CreateDeadKeyMenuField();
			void				_UpdateDeadKeyMenu();

			void 				_FillSystemMaps();
			void				_FillUserMaps();
			void				_SetListViewSize(BListView* listView);

			status_t			_GetCurrentKeymap(entry_ref& ref);
			BString				_GetActiveKeymapName();
			BString				_GetActiveLayoutId();
			bool				_SelectCurrentSystemMap();
			void				_SelectCurrentMap();

			status_t			_GetSettings(BFile& file, int mode) const;
			status_t			_LoadSettings(BRect& frame);
			status_t			_SaveSettings();

private:
			BOutlineListView*	fSystemListView;
			BListView*			fUserListView;
			BButton*			fDefaultsButton;
			BButton*			fRevertButton;
			BMenu*				fModelMenu;
			BMenu*				fFontMenu;
			KeyboardLayoutView*	fKeyboardLayoutView;
			BTextControl*		fTextControl;
			BButton*			fSwitchShortcutsButton;
			BMenu*				fAcuteMenu;
			BMenu*				fCircumflexMenu;
			BMenu*				fDiaeresisMenu;
			BMenu*				fGraveMenu;
			BMenu*				fTildeMenu;

			Keymap				fCurrentMap;
			Keymap				fPreviousMap;
			Keymap				fAppliedMap;
			BString				fCurrentMapName;

			// Set only by an explicit shape pick from the model menu, so
			// the geometry auto-pick never fights a manual choice.
			bool				fUserPickedLayout;
			BString				fKeyboardLayoutPath;

#ifndef __VOS__
			BFilePanel*			fOpenPanel;
			BFilePanel*			fSavePanel;
#endif
};

#endif	// KEYMAP_WINDOW_H
