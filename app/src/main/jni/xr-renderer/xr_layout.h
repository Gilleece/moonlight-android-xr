// The plain geometry of what hangs around the picture: which handle a point
// is over, and the stand in screen a room measures its furniture against,
// with the arithmetic behind the settings panel's tracks and presets.
// Nothing in here touches GL, OpenXR beyond its plain value types, or the
// context, so it can be built and checked on a desktop as well as on the
// headset.

#ifndef XR_LAYOUT_H
#define XR_LAYOUT_H

#include "xr_math.h"
#include "xr_shared.h"

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
#define HOVER_STEREOBUTTON 14
#define HOVER_REPORT    15
#define HOVER_RAYBUTTON 16
#define HOVER_AIMBUTTON 17
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

// The Room tab's lanes. A place along a track, 0 at the left end and 1 at the
// right, turned into the whole units the preference is stored in, and back,
// and the percent of the lane drawn beside it.
int laneUnits(float t, int min, int max);
float lanePlace(int units, int min, int max);
int lanePercent(int units, int min, int max);

// Where a row sits down the settings panel, as a fraction of its height, how
// far either side of that the row's hit band reaches, and how tall a row of
// cells is drawn, half of it. The display and screen tabs pack their rows
// closer than the rest. tab is one of the COG_TAB_ values, or anything past
// them for a face that is not a tab.
float cogRowV(int tab, int row);
float cogRowHalf(int tab);
float cogCellHalf(int tab);

// How big a track's thumb is drawn, as a fraction of the panel's height,
// before it grows under the ray
float cogThumbSize(int tab);

// Which part of a track's row a point across the panel is on: the button at
// the left end that steps down, the one at the right that steps up, the run
// between them where a press jumps the thumb, or none of it
#define TRACK_PART_NONE 0
#define TRACK_PART_RUN 1
#define TRACK_PART_DOWN 2
#define TRACK_PART_UP 3
int cogTrackPart(float pu);

// A place along the run, 0 at its left end and 1 at its right, from a point
// across the panel and back
float cogRunPlace(float pu);
float cogRunU(float t);

// The step a press of a step button lands on, for a track of that many
// steps with the thumb at t: the next whole step that way, so a thumb a drag
// left between two goes to the nearer one in that direction. Held to the
// track's ends.
int cogStepIndex(float t, int steps, int dir);

// How many steps a track has, each the unit its value is kept in or a sensible
// share of a value kept as a float, or 0 for a row that is not a track. tab
// is one of the COG_TAB_ values, or anything past them for the Room tab.
int cogTrackSteps(int tab, int row);

// Whether a point on the settings panel is on the About tab's button
int cogReportButtonAt(float pu, float pv);

// Which part of the report sheet a point on it is over, one of the
// REPORT_ZONE_ values, u and v across the sheet from its top left
int reportZone(float u, float v);

// The 3D tab's depth track. A separation, as a fraction of frame width, in the
// tenths of a percent the preference stores, which is also the step the track
// moves in, and back.
int separationUnits(float separation);
float separationOf(int units);

// Which preset a separation in those units is, as its cell, or -1 when it is
// none of them. The three are in cell order. Balanced is tried first, so a
// default near an end that clamps another preset onto it still reads as
// Balanced.
int cogPresetAt(int units, const int presets[COG_PRESET_CELLS]);

// How much of a room's screen the picture hangs at, in whole percent: inside
// the size row's lane where the room may be resized, and all of it where not
int roomScreenClamp(int percent, int resizable);

// Where a corner drag in a room leaves the picture: the size it started at,
// scaled by how far along the half diagonal the ray now reaches against how
// far it reached when the grab began, in the same whole percent and the same
// lane. Against the start rather than the corner, so taking hold of a bracket
// that hangs well outside a small picture does not throw the picture out to
// the ray.
int roomResizePercent(int startPercent, float startReach, float reach);

// How big a corner bracket on a room's picture is drawn, in metres, for a
// picture that far from the seat: whatever size looks the same as a bracket
// on the stand in screen, so a picture on a far wall or shrunk to a quarter
// still has corners the size of the ones outside a room
float roomCornerSide(float distance);

#endif
