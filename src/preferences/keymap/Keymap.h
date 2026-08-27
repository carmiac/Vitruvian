/*
 * Copyright 2004-2011 Haiku Inc. All rights reserved.
 * Copyright 2026, Dario Casalinuovo <b.vitruvio@gmail.com>.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jérôme Duval
 *		Axel Dörfler, axeld@pinc-software.de.
 *		Dario Casalinuovo
 */
#ifndef KEYMAP_H
#define KEYMAP_H


#include <Keymap.h>

#include <Entry.h>
#include <Messenger.h>
#include <String.h>


enum dead_key_index {
	kDeadKeyAcute = 1,
	kDeadKeyGrave,
	kDeadKeyCircumflex,
	kDeadKeyDiaeresis,
	kDeadKeyTilde
};


class Keymap : public BKeymap {
public:
								Keymap();
								~Keymap();

			void				SetTarget(BMessenger target,
									BMessage* modificationMessage);

			status_t			Load(const entry_ref& ref);
			status_t			Save(const entry_ref& ref);

			void				DumpKeymap();

			status_t			SetModifier(uint32 keyCode, uint32 modifier);

			void				SetDeadKeyEnabled(uint32 keyCode,
									uint32 modifiers, bool enabled);
			void				GetDeadKeyTrigger(dead_key_index deadKeyIndex,
									BString& outTrigger);
			void				SetDeadKeyTrigger(dead_key_index deadKeyIndex,
									const BString& trigger);

			status_t			RestoreSystemDefault();
			status_t			Use();
			// Call only from a real user decision to activate this keymap,
			// never implicitly from Use(). systemWide selects
			// /etc/default/keyboard instead of the per-user override.
			void				ApplyXkbLayout(bool systemWide = false);
			// Writes /etc/default/keyboard without activating, so a caller
			// that wants both destinations activates once, not twice.
			void				WriteSystemXkbLayout() const;
			// Writes input/layout and input/xkb_layout without
			// activating; pair with a single Use() call afterward.
			void				WriteXkbLayout() const;

			void				SetKey(uint32 keyCode, uint32 modifiers,
									int8 deadKey, const char* bytes,
									int32 numBytes = -1);

			// The display name. Free-form: the preflet renames an edited
			// map to "(Current)", so this must not be what xkb derives
			// from; see SetLayoutName().
			void				SetName(const char* name);

			// The layout identity (shipped keymap name or synthetic
			// "xkb:<layout>[:<variant>]" id); survives a rename.
			void				SetLayoutName(const char* name);
			const char*			LayoutName() const;

			// The xkb rules "model=" value. Empty means unset; Model()
			// always returns a usable string, falling back to "pc105".
			void				SetModel(const char* model);
			const char*			Model() const;

			const key_map&		Map() const { return fKeys; }
			key_map&			Map() { return fKeys; }

			Keymap&				operator=(const Keymap& other);

private:
			bool				_SetChars(int32 offset, const char* bytes,
									int32 numBytes);
			void				_WriteXkbLayout() const;
			void				_WriteSystemXkbLayout() const;
			void				_WriteLayoutName() const;

private:
			char				fName[B_FILE_NAME_LENGTH];
			char				fLayoutName[B_FILE_NAME_LENGTH];
			char				fModel[64];

			BMessenger			fTarget;
			BMessage*			fModificationMessage;
};


#endif	// KEYMAP_H
