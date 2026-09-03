// Every value the native renderer and the Java side have to agree on: the
// protocol of the per frame input array, the ids of the settings the panel
// reports, and the size and layout of every sheet of art Java draws and this
// side hit tests. The build turns this file into XrShared.java, so a value
// changed here moves both sides together and nothing has to be kept in step
// by hand.
//
// Only plain integer and float constants, and expressions over ones defined
// above them, since that is all the generator reads. No casts, no macros with
// arguments, and nothing that only means something in C.

#ifndef XR_SHARED_H
#define XR_SHARED_H

// Levels of the file log
#define FILE_LOG_OFF 0
#define FILE_LOG_BASIC 1
#define FILE_LOG_VERBOSE 2

// Return codes for waitBeginFrame
#define FRAME_EXIT   -1
#define FRAME_IDLE    0
#define FRAME_RENDER  1

// Synthetic depth patterns for the stereo test path
#define DEPTH_MODE_OFF   0
#define DEPTH_MODE_FLAT  1
#define DEPTH_MODE_RAMP  2
#define DEPTH_MODE_BLOB  3
// Tints each eye instead of warping, so eye routing can be checked by
// closing one eye rather than by judging depth
#define DEPTH_MODE_EYETEST 4
// Draws a synthetic bar through the warp and reads back where it landed in
// each eye, so the shift direction is measured rather than eyeballed
#define DEPTH_MODE_SHIFTTEST 5
// Real depth from the MiDaS model, run in Java on LiteRT
#define DEPTH_MODE_MODEL 6

// The depth model's input and output are square at this size
#define DEPTH_TEX_SIZE 256

// Enough for a dozen lines of stats without being big enough to matter
#define OVERLAY_WIDTH 768
#define OVERLAY_HEIGHT 512

// Slots in the float array handed back to Java each frame
#define IN_HIT      0
#define IN_U        1
#define IN_V        2
#define IN_BUTTONS  3
#define IN_SCROLL   4
#define IN_POINTER  5
#define IN_POSE_DIRTY 6
// Which setting the panel just changed, or -1. Zero is a real id, so this one
// has to be said explicitly rather than left at the memset.
#define IN_SETTING  7
// x y z, then the orientation quaternion, then width, cylinder radius and the
// curvature the panel asked for, POSE_VALUES in all
#define IN_POSE     8
#define POSE_VALUES 10
// The cell just chosen in the environment grid, or -1
#define IN_PICKER_PICK 18
#define IN_SETTING_VALUE 19
// The key the in world keyboard just typed, or -1. Unicode with the shift
// already applied, plus the four control codes below 32.
#define IN_KEY      20
// Set to 1 the frame the exit prompt is confirmed. Nothing else is meaningful
// here, so a zeroed slot says nothing happened.
#define IN_EXIT     21
#define IN_SLOTS    22

// Settings the panel can hand back to Java to be applied and stored
#define SETTING_SHARPEN 0
#define SETTING_STATS   1
#define SETTING_SEPARATION 2
#define SETTING_CONVERGENCE 3
#define SETTING_RESET_3D 4
#define SETTING_AMBILIGHT 5
#define SETTING_AMBI_LEVEL 6
#define SETTING_ROOM_LIGHT 7
#define SETTING_HEAD_LOCK 8

// Which Environment Res tier the room draws at
#define ENV_RES_LOW 0
#define ENV_RES_STANDARD 1
#define ENV_RES_HIGH 2
#define ENV_RES_ULTRA 3

// Environment picker. A grid of thumbnails drawn in Java and shown as one
// quad, with the hover and selection marks as separate outline quads so
// pointing around the grid never costs an upload. One band per category: a
// header strip carrying its name, then a row of cells under it.
#define PICKER_COLS 4
#define PICKER_ROWS 2
#define PICKER_CELLS (PICKER_COLS * PICKER_ROWS)
#define PICKER_TEX_W 1024
#define PICKER_HEADER_PX 40
#define PICKER_CELL_PX 256
#define PICKER_BAND_PX (PICKER_HEADER_PX + PICKER_CELL_PX)
#define PICKER_TEX_H (PICKER_BAND_PX * PICKER_ROWS)
// The cells of the grid. The fixed ones come first, then the photos from the
// assets folder in name order take whatever the rooms leave. A cell is a place
// in the grid and nothing more: what gets saved is the stable id it maps to.
#define ENV_CELL_PASSTHROUGH 0
#define ENV_CELL_VOID 1
#define ENV_CELL_MINIMAL_ROOM 2
#define ENV_CELL_PSX_CINEMA 3
#define ENV_CELL_FIRST_PHOTO 4

// The buttons under the bar are all drawn at this size
#define BUTTON_TEX 128
// The padlock that locks the hands out
#define LOCK_TEX 384

// Settings panel. Same shape as the picker: the art is drawn in Java and shown
// on one quad, the thumbs are separate little quads so dragging one never costs
// an upload.
#define COG_TEX_W 768
#define COG_TEX_H 640

// Where the tabs, rows and tracks sit in the panel texture, as fractions of it
#define COG_TRACK_L 0.42f
#define COG_TRACK_R 0.93f
// Anything above this is the tab bar, split evenly between the tabs
#define COG_TAB_BAR_B 0.16f
// Six rows on the screen tab, so they start a little higher and sit closer
// together than they did at five
#define COG_ROW_V0 0.25f
#define COG_ROW_STEP 0.11f
// Half height of a row's hit band. Under half the pitch, so neighbouring
// bands stay disjoint.
#define COG_ROW_HALF 0.05f
#define COG_RESET_L 0.35f
#define COG_RESET_R 0.65f
// Clear of the last row, which reaches 0.80 plus the half band
#define COG_RESET_T 0.87f
#define COG_RESET_B 0.97f
// Half height of an option cell, so the ring drawn over one matches the art
#define COG_CELL_HALF 0.045f

// One texture per tab, all uploaded once, so switching costs a swapchain
// handle rather than an upload
#define COG_TAB_SCREEN  0
#define COG_TAB_DISPLAY 1
#define COG_TAB_3D      2
#define COG_TAB_COUNT   3
// And one more sheet than there are tabs: the screen tab has a second face for
// when a room hangs the picture and none of its rows can do anything
#define COG_ART_ROOM_SCREEN 3
#define COG_ART_COUNT       4

// Screen tab rows, in the order they are drawn
#define COG_SLIDER_DISTANCE 0
#define COG_SLIDER_HEIGHT   1
#define COG_SLIDER_TILT     2
#define COG_SLIDER_ROTATE   3
#define COG_SLIDER_CURVE    4
#define COG_SLIDER_SIZE     5
#define COG_SLIDER_COUNT    6

// 3D tab rows, sliders like the screen tab's. Only values that take effect the
// moment they move belong here: the depth source itself is settled when the
// session starts, so it stays in the 2d settings.
#define COG_ROW3D_SEPARATION 0
#define COG_ROW3D_CONVERGENCE 1
#define COG_ROW3D_COUNT 2
// Right hand end of the separation track, as a fraction of frame width. Three
// times the 0.5 percent that phase 6 measured as the useful maximum: past
// there depth stops growing and only the strain does, so the far end of the
// track is drawn marked rather than left off.
#define COG_SEP_MAX 0.015f
// Steps along that track, so a dragged value lands exactly on one of the
// tenths of a percent the preference is stored in
#define COG_SEP_STEPS 15

// Display tab rows. Cells rather than a track, so a press picks one instead of
// dragging a value.
#define COG_OPTION_SHARPEN 0
#define COG_OPTION_STATS   1
#define COG_OPTION_HEAD_LOCK 2
#define COG_OPTION_AMBILIGHT 3
#define COG_OPTION_ROOM_LIGHT 4
#define COG_OPTION_COUNT   5
#define COG_SHARPEN_CELLS 3
#define COG_STATS_CELLS   2
#define COG_HEAD_LOCK_CELLS 2
#define COG_AMBI_CELLS    2
#define COG_ROOM_LIGHT_CELLS 2
// The one row on this tab that is a track rather than cells, under the option
// rows, so the glow can be turned down without leaving the tab it lives on.
// Six rows on this tab now, the same grid the screen tab already fills.
#define COG_DISPLAY_SLIDER_ROW 5

// In world keyboard, for the login boxes and chat windows that turn up mid
// stream. One sheet of art per state, drawn in Java like the other panels, and
// the layout arrives with it: the native side is handed rectangles and codes
// and knows nothing else about what the keys say.
#define KB_TEX_W 1120
#define KB_TEX_H 448
#define KB_STATE_LOWER   0
#define KB_STATE_UPPER   1
#define KB_STATE_SYMBOLS 2
#define KB_STATE_COUNT   3
// Codes under zero change the keyboard instead of typing. Everything at or
// above 8 is sent on as it stands.
#define KB_CODE_SHIFT   -2
#define KB_CODE_SYMBOLS -3
#define KB_CODE_HIDE    -4

// The button that ends the stream and the prompt it opens. The sheet is drawn
// in Java like the other panels, one per lit button, so hovering one is
// another handle in the layer rather than an upload.
#define EXIT_TEX_W 512
#define EXIT_TEX_H 256
// Which zone of the sheet the ray is on, and the sheet drawn with that zone
// lit, so the two share their numbering
#define EXIT_ZONE_NONE   0
#define EXIT_ZONE_EXIT   1
#define EXIT_ZONE_CANCEL 2
#define EXIT_ART_COUNT   3
// Where the two buttons sit on the sheet, as fractions of it
#define EXIT_BTN_T 0.56f
#define EXIT_BTN_B 0.86f
#define EXIT_EXIT_L 0.08f
#define EXIT_EXIT_R 0.46f
#define EXIT_CANCEL_L 0.54f
#define EXIT_CANCEL_R 0.92f

#endif
