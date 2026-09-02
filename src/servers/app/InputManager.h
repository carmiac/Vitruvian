/*
 * Copyright 2005, Haiku, Inc. All Rights Reserved.
 * Copyright 2026, Dario Casalinuovo.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */
#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H


#include <Locker.h>
#include <ObjectList.h>


class EventStream;

class InputManager : public BLocker {
	public:
		InputManager();
		virtual ~InputManager();

		void UpdateScreenBounds(BRect bounds, int32 orientation = 0);

		bool AddStream(EventStream* stream);
		void RemoveStream(EventStream* stream);

		EventStream* GetStream();
		void PutStream(EventStream* stream);

	private:
		BObjectList<EventStream, true> fFreeStreams;
		BObjectList<EventStream, true> fUsedStreams;
};

extern InputManager* gInputManager;

#endif	/* INPUT_MANAGER_H */
