/*
 * Copyright 2026, The Vitruvian Project
 * Distributed under the terms of the MIT License.
 */
#ifndef PANEL_ORIENTATION_TRANSFORM_H
#define PANEL_ORIENTATION_TRANSFORM_H


#include <SupportDefs.h>


// Matches the kernel's DRM_MODE_PANEL_ORIENTATION_* values and the
// "screen_orientation" field on B_SCREEN_BOUNDS_CHANGED.
enum {
	B_PANEL_ORIENTATION_NORMAL      = 0,
	B_PANEL_ORIENTATION_UPSIDE_DOWN = 1,
	B_PANEL_ORIENTATION_LEFT_UP     = 2,
	B_PANEL_ORIENTATION_RIGHT_UP    = 3
};


// Rotates a normalized (0..1) point in physical panel space into
// normalized logical (landscape, post-rotation) desktop space.
static inline void
rotate_panel_point(int32 orientation, float u, float v, float& outU,
	float& outV)
{
	switch (orientation) {
		case B_PANEL_ORIENTATION_UPSIDE_DOWN:
			outU = 1.0f - u;
			outV = 1.0f - v;
			break;
		case B_PANEL_ORIENTATION_LEFT_UP:
			outU = 1.0f - v;
			outV = u;
			break;
		case B_PANEL_ORIENTATION_RIGHT_UP:
			outU = v;
			outV = 1.0f - u;
			break;
		default:
			outU = u;
			outV = v;
			break;
	}
}


// Rotates a relative motion or scroll delta the same way, without the
// translation component (pure vector rotation) — keep this in the same
// direction convention as rotate_panel_point() or the pointer moves
// correctly on one axis and inverted on the other.
static inline void
rotate_panel_delta(int32 orientation, float dx, float dy, float& outDx,
	float& outDy)
{
	switch (orientation) {
		case B_PANEL_ORIENTATION_UPSIDE_DOWN:
			outDx = -dx;
			outDy = -dy;
			break;
		case B_PANEL_ORIENTATION_LEFT_UP:
			outDx = -dy;
			outDy = dx;
			break;
		case B_PANEL_ORIENTATION_RIGHT_UP:
			outDx = dy;
			outDy = -dx;
			break;
		default:
			outDx = dx;
			outDy = dy;
			break;
	}
}


#endif	// PANEL_ORIENTATION_TRANSFORM_H
