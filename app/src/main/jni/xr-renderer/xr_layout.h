// The plain geometry of what hangs around the picture: which handle a point
// is over, and the stand in screen a room measures its furniture against.
// Nothing in here touches GL, OpenXR beyond its plain value types, or the
// context, so it can be built and checked on a desktop as well as on the
// headset.

#ifndef XR_LAYOUT_H
#define XR_LAYOUT_H

#include "xr_math.h"

// What the ray is over. Handles only show while hovered, which is how spatial
// panels usually behave: nothing visible until you go looking for it.
#define HOVER_NONE   0
#define HOVER_SCREEN 1
#define HOVER_BAR    2
#define HOVER_CORNER 3
#define HOVER_ENVBUTTON 4
#define HOVER_PICKER    5
// Nothing under the ray, but close enough to the screen to keep drawing it
#define HOVER_HALO      6
#define HOVER_LOCK      7
#define HOVER_COGBUTTON 8
#define HOVER_COGPANEL  9
#define HOVER_KBBUTTON  10
#define HOVER_KBPANEL   11
#define HOVER_EXITBUTTON 12
#define HOVER_EXITPROMPT 13
// How far past each edge that reaches, as a fraction of the screen
#define HALO_FRAC 0.5f

// Handle art, one small swapchain each so there is no atlas offset convention
// to get wrong
#define BAR_TEX_W 256
#define BAR_TEX_H 24
#define CORNER_TEX_W 64
#define CORNER_TEX_H 64

// All as a fraction of screen width, so the handles keep their proportions as
// the screen is resized
#define BAR_WIDTH_FRAC  0.14f
// Height follows the art rather than being picked separately. The two used to
// disagree by 2.5x, which stretched the rounded ends into a slab.
#define BAR_HEIGHT_FRAC (BAR_WIDTH_FRAC * (float)BAR_TEX_H / (float)BAR_TEX_W)
#define BAR_GAP_FRAC    0.035f
#define CORNER_FRAC     0.075f
// Hover zones are bigger than the art, since aiming at a thin bar is fussy
#define HOVER_MARGIN 1.7f
#define CORNER_HOVER 1.5f
// The bar is small on purpose, so its hover zone is proportionally wider
#define BAR_HOVER 2.0f

// The screen the furniture hangs against inside a room: the size and distance
// the picture starts at outside one, straight ahead of the seat at eye level.
// A room hangs its own picture on a wall metres off and as wide as the wall, so
// measuring the buttons and the panels against that gave them a different size
// and distance in every room, and shrank them along with the picture.
#define STAND_IN_WIDTH_M 3.0f
#define STAND_IN_DISTANCE_M 3.0f

// Which affordance a point on a screen of that size is over, u and v across
// it from the top left. cornerSide is how big the corner brackets are drawn,
// in metres, and 0 where there are none, which lets the ray fall through to
// what is behind them. Corners are numbered 0 top left, 1 top right, 2 bottom
// left, 3 bottom right.
int hoverTest(float u, float v, float width, float height, float cornerSide, int* corner);

// Where the stand in screen hangs, square to the seat
XrPosef standInPose(void);

#endif
