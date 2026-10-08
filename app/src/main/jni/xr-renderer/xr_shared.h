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

// Where the session stands for the hold a removed headset gets: focused, gone
// to stopping or idle after a first focus, or anything else
#define PRESENCE_OTHER   0
#define PRESENCE_FOCUSED 1
#define PRESENCE_AWAY    2

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
// Real depth from a model, ZipDepth or MiDaS, run in Java on LiteRT
#define DEPTH_MODE_MODEL 6

// The depth map is the size of the model input it is made from, which Java
// picks per session from the model and the headset and hands to nativeInit:
// 512x288 for ZipDepth on a Gen 2 headset, 256 square for MiDaS and for the
// Gen 1 ZipDepth. The default is what a size out of range falls back to, and
// the maxima are the largest map any route runs at.
#define DEPTH_TEX_SIZE_DEFAULT 256
#define DEPTH_TEX_W_MAX 512
#define DEPTH_TEX_H_MAX 288

// Sets of depth staging a capture travels in: the readback, the model input
// it is turned into and the output the model writes. Two, so one capture can
// be read back and the last map uploaded while the model runs the other.
#define DEPTH_PAIRS 2

// How many depth maps a second the model may make, the setting's range and
// its starting points: 20 is what every third frame of a 60 fps stream gave,
// 12 every sixth of a Gen 1 headset's 72
#define DEPTH_RATE_MIN 5
#define DEPTH_RATE_MAX 45
#define DEPTH_RATE_DEFAULT 20
#define DEPTH_RATE_GEN1 12

// Enough for a dozen lines of stats without being big enough to matter
#define OVERLAY_WIDTH 768
#define OVERLAY_HEIGHT 512

// The sheet the launch splash shows: the logo over the app's name, drawn once
// per number of the logo's wedges open, none to all six, one under the other,
// so ticking the wedges is a different rectangle of the same image rather than
// an upload. The rows sit on nothing. Under the last, clear of it, is a square
// of the ground with its glow, which the quad that covers the view is cut
// from, less an inset all round so filtering never reaches past it, so the
// two fade as one.
#define SPLASH_WEDGES 6
#define SPLASH_ROWS (SPLASH_WEDGES + 1)
#define SPLASH_TEX_W 384
#define SPLASH_ROW_H 256
#define SPLASH_GROUND_PX 256
#define SPLASH_GROUND_INSET 8
#define SPLASH_TEX_H (SPLASH_ROW_H * SPLASH_ROWS + 8 + SPLASH_GROUND_PX)
// Both locked to the head, in metres: the ground wider than any view, and the
// sheet a little in front of it at the size that gives a row 0.3 m
#define SPLASH_GROUND_M 6.0f
#define SPLASH_GROUND_DISTANCE_M 1.5f
#define SPLASH_SHEET_W_M 0.45f
#define SPLASH_SHEET_DISTANCE_M 1.45f

// The toast, a short notice hung off the eyes for a few seconds: one sheet,
// drawn in Java whenever what it says changes. What a notice is about, which
// the native side raises and Java turns into words. The rate's number is its
// Hz, and a message of Java's own is a slot in Java's list of them.
#define TOAST_TEX_W 1024
#define TOAST_TEX_H 160
#define TOAST_RATE 0
#define TOAST_HANDS_LOCKED 1
#define TOAST_HANDS_UNLOCKED 2
#define TOAST_3D_OFF 3
#define TOAST_3D_ON 4
#define TOAST_TEXT 5
#define TOAST_TEXT_SLOTS 4
#define TOAST_HEAD_AIM_OFF 6
#define TOAST_HEAD_AIM_ON 7
#define TOAST_GAMEPAD_MODE 8
#define TOAST_POINTER_MODE 9

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
// How far the head has turned from the screen, radians, positive to the left.
// 0 with the screen locked to the head. The virtual surround turns its
// speakers by it. Written every frame, carried over when the head is lost.
#define IN_HEAD_YAW 22
// Which room a room's own setting belongs to, as the picker cell showing it,
// or -1 with no room up. Every frame, since each room keeps its own values.
#define IN_SETTING_ROOM 23
// What the strip beside the Room, Picture or Screen tab's tracks says. First
// which of them it is for, one of the READOUT_ values, or -1 while none is up,
// then a value a row. The Room tab's three are the percent drawn beside each:
// brightness and light level as the place along their lanes, the size as the
// share of the room's screen, -1 in a room whose size is fixed. The Picture
// tab's four are its values in their own whole units. The Screen tab's two
// are head aim's pixels a degree and dead zone, only while its rows are live.
#define IN_READOUT  24
#define READOUT_VALUES 5
#define READOUT_ROOM 0
#define READOUT_PICTURE 1
#define READOUT_SCREEN 2
// 1 while the 3D is on, and 0 once the bar or the 3D tab has switched it off
// for the rest of the session, or in a session started without it. Every
// frame, since it says whether the model is to be fed.
#define IN_STEREO   (IN_READOUT + READOUT_VALUES)
// A notice for the toast to say, the frame it goes up: one of the TOAST_
// kinds, or -1 for none, and its number
#define IN_TOAST    (IN_STEREO + 1)
#define IN_TOAST_ARG (IN_TOAST + 1)
// 1 on the frame a press landed on a panel's cell, button, key or track, so
// Java can tick
#define IN_CLICK    (IN_TOAST_ARG + 1)
// The display tab's cell in force on each of its option rows, in row order,
// which Java draws the marks over them from, or -1 first while the tab is not
// up, and -1 for a row greyed where it can do nothing. Every frame. As many as
// COG_OPTION_COUNT below, which the native side checks when it builds.
#define IN_MARKS    (IN_CLICK + 1)
#define MARK_VALUES 12
// 1 while the settings panel is up, fading out included, so Java keeps the
// clock line over it up to the minute
#define IN_COG_OPEN (IN_MARKS + MARK_VALUES)
// The keyboard's modifiers, KB_MOD_ bits: those held down with this frame's
// key, then those lit on the keyboard now, every frame, which Java holds down
// on the host for as long as they stay lit. Last the sheet showing, or -1
// with the keyboard put away, which Java redraws when the lit ones change.
#define IN_KEY_MODS (IN_COG_OPEN + 1)
#define IN_KB_MODS  (IN_KEY_MODS + 1)
#define IN_KB_SHEET (IN_KB_MODS + 1)
// The report sheet: what a press on it did this frame, one of the REPORT_
// zones or REPORT_OPENED, or -1, and every frame the zone under the ray, or
// -1 while the sheet is down. While it is up the keyboard types into it
// rather than to the host.
#define IN_REPORT   (IN_KB_SHEET + 1)
#define IN_REPORT_ZONE (IN_REPORT + 1)
// 1 while head aim is on and can act: switched on, with the screen locked to
// the head and no room up. Every frame. Paused for a panel it still reads 1,
// since the controller's pointer stays relative.
#define IN_HEAD_AIM (IN_REPORT_ZONE + 1)
// Whole pixels to move the host's mouse by this frame, right and down
// positive: the head's turn while head aim is on, plus the controller's
// pointer nudging it, since in that mode the ray moves the cursor by how far
// its point moved rather than to it. 0 and 0 for none.
#define IN_MOUSE_DX (IN_HEAD_AIM + 1)
#define IN_MOUSE_DY (IN_MOUSE_DX + 1)
// Gamepad mode's pad: 1 while it is plugged in on the host, which is gamepad
// mode with a controller in either hand, else 0. Then what it reads, every
// frame: PAD_ bits, each trigger 0 to 255 and each stick -32766 to 32766,
// right and up positive, all at rest while a panel is up or the session is
// not focused.
#define IN_PAD      (IN_MOUSE_DY + 1)
#define IN_PAD_BUTTONS (IN_PAD + 1)
#define IN_PAD_LT   (IN_PAD_BUTTONS + 1)
#define IN_PAD_RT   (IN_PAD_LT + 1)
#define IN_PAD_LX   (IN_PAD_RT + 1)
#define IN_PAD_LY   (IN_PAD_LX + 1)
#define IN_PAD_RX   (IN_PAD_LY + 1)
#define IN_PAD_RY   (IN_PAD_RX + 1)
// Set to 1 the frame the hand lock hint is put away with "Don't show this
// again", which Java stores so later sessions skip it. 0 otherwise.
#define IN_HINT     (IN_PAD_RY + 1)
#define IN_SLOTS    (IN_HINT + 1)

// Gamepad mode's buttons, in the bits Moonlight's controller packet carries
// them in, which are ControllerPacket's flags: the four face buttons, the
// bumpers, the stick clicks and Start, which is everything two controllers
// have for an app. Touch-style controllers have no Back, guide button or
// d-pad; the Steam Frame's controllers do, so those bits are here for them.
#define PAD_UP      0x0001
#define PAD_DOWN    0x0002
#define PAD_LEFT    0x0004
#define PAD_RIGHT   0x0008
#define PAD_START   0x0010
#define PAD_BACK    0x0020
#define PAD_LS_CLICK 0x0040
#define PAD_RS_CLICK 0x0080
#define PAD_LB      0x0100
#define PAD_RB      0x0200
#define PAD_A       0x1000
#define PAD_B       0x2000
#define PAD_X       0x4000
#define PAD_Y       0x8000
#define PAD_BUTTONS (PAD_UP + PAD_DOWN + PAD_LEFT + PAD_RIGHT + PAD_BACK + PAD_START + PAD_LS_CLICK + PAD_RS_CLICK + PAD_LB + PAD_RB + PAD_A + PAD_B + PAD_X + PAD_Y)

// The shortcut on the controllers that switches between the pointer and the
// pad, in the order the 2D setting lists them: the left menu button held with
// the left grip, both stick clicks, or both triggers with both grips
#define PAD_SHORTCUT_MENU_GRIP 0
#define PAD_SHORTCUT_STICKS 1
#define PAD_SHORTCUT_TRIGGERS_GRIPS 2

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
#define SETTING_SUPERSAMPLE 9
// A room's own values, stored under that room's id (IN_SETTING_ROOM)
#define SETTING_ROOM_BRIGHTNESS 10
#define SETTING_ROOM_GLOW 11
#define SETTING_ROOM_LIGHT_LEVEL 12
#define SETTING_ROOM_SCREEN 13
// Whether a still controller's pointer pauses, Off 0 or On 1
#define SETTING_POINTER_SLEEP 14
// Whether a press on the panels ticks, Off 0 or On 1
#define SETTING_CLICK_SOUND 15
// The picture grade's four values in their whole units, one id each in the
// PICTURE_ order, and all four back to the picture as streamed
#define SETTING_PICTURE_BRIGHTNESS 16
#define SETTING_PICTURE_CONTRAST 17
#define SETTING_PICTURE_GAMMA 18
#define SETTING_PICTURE_SATURATION 19
#define SETTING_RESET_PICTURE 20
// Whether the bundled controller model is drawn at each hand, Off 0 or On 1
#define SETTING_CONTROLLER_MODEL 21
// Head aim's pixels a degree and its dead zone in degrees a second, in the
// HEAD_AIM_ lanes below
#define SETTING_HEAD_AIM_SENSITIVITY 22
#define SETTING_HEAD_AIM_DEADZONE 23

// The lanes the Room tab's rows move along, in the units the preferences
// hold. Brightness is the room's own in hundredths of the room as baked, from
// near black to a little over half as bright again. The light off the picture
// is in hundredths of the gain the room's row gives it, and starts three
// quarters of the way along its lane. The size is whole percent of the room's
// screen anchor, from a quarter of it to all of it.
#define ROOM_BRIGHTNESS_MIN 5
#define ROOM_BRIGHTNESS_MAX 165
#define ROOM_LIGHT_MIN 0
#define ROOM_LIGHT_MAX 200
#define ROOM_LIGHT_DEFAULT 150
#define ROOM_SCREEN_MIN 25
#define ROOM_SCREEN_MAX 100

// Where each baked room starts on those rows, which the room table in
// xr_room.c is built from and the preferences take their defaults from, so a
// room is retuned in one place. Glow and resizable are 1 or 0. The light level
// starts at ROOM_LIGHT_DEFAULT in every room.
#define ROOM_THEATER_BRIGHTNESS 37
#define ROOM_THEATER_SCREEN 80
#define ROOM_THEATER_GLOW 1
#define ROOM_THEATER_RESIZABLE 1
#define ROOM_GRAND_CINEMA_BRIGHTNESS 21
#define ROOM_GRAND_CINEMA_SCREEN 100
#define ROOM_GRAND_CINEMA_GLOW 0
#define ROOM_GRAND_CINEMA_RESIZABLE 0
#define ROOM_SYNTHWAVE_BRIGHTNESS 53
#define ROOM_SYNTHWAVE_SCREEN 100
#define ROOM_SYNTHWAVE_GLOW 1
#define ROOM_SYNTHWAVE_RESIZABLE 1

// The picture grade, in the whole units its preferences hold, one set for
// every room and session. Brightness is hundredths of full scale added after
// the contrast, contrast is percent of gain about mid grey, gamma is in
// hundredths, and saturation is percent of the picture's own. Each default is
// the picture as streamed, where the grade does nothing at all.
#define PICTURE_BRIGHTNESS_MIN -50
#define PICTURE_BRIGHTNESS_MAX 50
#define PICTURE_BRIGHTNESS_DEFAULT 0
#define PICTURE_CONTRAST_MIN 50
#define PICTURE_CONTRAST_MAX 150
#define PICTURE_CONTRAST_DEFAULT 100
#define PICTURE_GAMMA_MIN 50
#define PICTURE_GAMMA_MAX 200
#define PICTURE_GAMMA_DEFAULT 100
#define PICTURE_SATURATION_MIN 0
#define PICTURE_SATURATION_MAX 200
#define PICTURE_SATURATION_DEFAULT 100
// The four in the order the Picture tab draws them, which is also the order
// they travel in wherever they go together
#define PICTURE_BRIGHTNESS 0
#define PICTURE_CONTRAST 1
#define PICTURE_GAMMA 2
#define PICTURE_SATURATION 3
#define PICTURE_VALUES 4

// Head aim, in the whole units its preferences hold: how far the mouse moves
// for a degree the head turns, in pixels, and how slowly the head can turn,
// in degrees a second, before nothing is sent, which swallows tracking
// jitter while the head is held still
#define HEAD_AIM_SENSITIVITY_MIN 1
#define HEAD_AIM_SENSITIVITY_MAX 30
#define HEAD_AIM_SENSITIVITY_DEFAULT 8
#define HEAD_AIM_DEADZONE_MIN 0
#define HEAD_AIM_DEADZONE_MAX 20
#define HEAD_AIM_DEADZONE_DEFAULT 2

// Which Environment Res tier the room draws at
#define ENV_RES_LOW 0
#define ENV_RES_STANDARD 1
#define ENV_RES_HIGH 2
#define ENV_RES_ULTRA 3

// Environment picker. A grid of thumbnails drawn in Java and shown as one
// quad, with the hover and selection marks as separate outline quads so
// pointing around the grid never costs an upload. One band: a header strip
// carrying its name, then a row of cells under it.
#define PICKER_COLS 5
#define PICKER_ROWS 1
#define PICKER_CELLS (PICKER_COLS * PICKER_ROWS)
#define PICKER_HEADER_PX 40
#define PICKER_CELL_PX 256
// Follows the cell, so the columns always divide the texture exactly and a
// cell stays square however many of them there are
#define PICKER_TEX_W (PICKER_CELL_PX * PICKER_COLS)
#define PICKER_BAND_PX (PICKER_HEADER_PX + PICKER_CELL_PX)
#define PICKER_TEX_H (PICKER_BAND_PX * PICKER_ROWS)
// The cells of the grid, in the order they are drawn, and how many of them
// there are. A cell is a place in the grid and nothing more: what gets saved
// is the stable id it maps to.
#define ENV_CELL_PASSTHROUGH 0
#define ENV_CELL_VOID 1
#define ENV_CELL_HOME_THEATER 2
#define ENV_CELL_GRAND_CINEMA 3
#define ENV_CELL_SYNTHWAVE 4
#define ENV_CELL_COUNT 5

// The buttons under the bar are all drawn at this size
#define BUTTON_TEX 128

// Settings panel. Same shape as the picker: the art is drawn in Java and shown
// on one quad, the thumbs are separate little quads so dragging one never costs
// an upload.
#define COG_TEX_W 768
#define COG_TEX_H 640

// Where the tabs, rows and tracks sit in the panel texture, as fractions of it
#define COG_TRACK_L 0.42f
#define COG_TRACK_R 0.93f
// A step button at each end of every track, a press on one moving the value
// a step that way. Each is this wide inside the track's ends, its hit zone
// reaching a little further out, and the run the thumb travels stops this far
// in from each end, so a thumb at either end of it clears the button.
#define COG_CHEVRON_W 0.036f
#define COG_CHEVRON_REACH 0.02f
#define COG_RUN_INSET 0.075f
#define COG_RUN_L (COG_TRACK_L + COG_RUN_INSET)
#define COG_RUN_R (COG_TRACK_R - COG_RUN_INSET)
// Anything above this is the tab bar, split evenly between the tabs
#define COG_TAB_BAR_B 0.16f
// Where the rows of the 3D, Picture and Room tabs sit. The screen tab and the
// display tab carry more rows and have spacings of their own below.
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

// One sheet per tab, all kept in memory from the start, so switching is one
// upload into the panel's swapchain
#define COG_TAB_SCREEN  0
#define COG_TAB_DISPLAY 1
#define COG_TAB_3D      2
#define COG_TAB_PICTURE 3
#define COG_TAB_ABOUT   4
#define COG_TAB_COUNT   5
// And the sheets a room shows instead. While one is up the first tab is the
// Room tab, whose size row is live or greyed as the room allows, and the other
// tabs are drawn again with that name over the first slot.
#define COG_ART_ROOM         5
#define COG_ART_ROOM_FIXED   6
#define COG_ART_ROOM_DISPLAY 7
#define COG_ART_ROOM_3D      8
#define COG_ART_ROOM_PICTURE 9
#define COG_ART_ROOM_ABOUT   10
// And the screen and display tabs for a screen that is not locked to the head,
// where head aim's rows are greyed, since it only acts on one that is
#define COG_ART_SCREEN_WORLD 11
#define COG_ART_DISPLAY_WORLD 12
#define COG_ART_COUNT        13

// Screen tab rows, in the order they are drawn: the six that place the
// screen, which the reset button under them puts back, then head aim's two,
// live only with the screen locked to the head
#define COG_SLIDER_DISTANCE 0
#define COG_SLIDER_HEIGHT   1
#define COG_SLIDER_TILT     2
#define COG_SLIDER_ROTATE   3
#define COG_SLIDER_CURVE    4
#define COG_SLIDER_SIZE     5
#define COG_SLIDER_COUNT    6
#define COG_SLIDER_AIM_SENSITIVITY 6
#define COG_SLIDER_AIM_DEADZONE 7
#define COG_SCREEN_ROW_COUNT 8
// Eight rows over the reset button, so they start higher and sit closer
// together than the other tabs' with shallower cells. The last row's band
// ends at 0.8575, clear of the reset button.
#define COG_SCREEN_ROW_V0 0.22f
#define COG_SCREEN_ROW_STEP 0.085f
#define COG_SCREEN_ROW_HALF 0.0425f
#define COG_SCREEN_CELL_HALF 0.036f

// Room tab rows, in the order they are drawn: a track, two rows of cells and
// two more tracks, with the screen light's level under its switch. Only in a
// room, in the first tab's place, since a room places and sizes the picture
// itself and none of the screen tab's rows can do anything there.
#define COG_ROOM_ROW_BRIGHTNESS 0
#define COG_ROOM_ROW_GLOW 1
#define COG_ROOM_ROW_LIGHT 2
#define COG_ROOM_ROW_LIGHT_LEVEL 3
#define COG_ROOM_ROW_SIZE 4
#define COG_ROOM_ROW_COUNT 5
#define COG_ROOM_SWITCH_CELLS 2
// The percent beside each of its tracks, and the Picture tab's values beside
// its. Drawn in Java whenever one changes and shown as a strip of its own over
// the panel rather than as part of the sheet, so a drag costs an upload this
// size rather than the whole sheet's. Where the strip sits on the panel, as
// fractions of it.
#define COG_READOUT_TEX_W 96
#define COG_READOUT_TEX_H 448
#define COG_READOUT_L 0.26f
#define COG_READOUT_T 0.19f

// 3D tab rows, in the order they are drawn: a row of preset cells over two
// sliders like the screen tab's, then the switch the bar's 3D button also
// works. Only values that take effect the moment they move belong here: the
// depth source itself is settled when the session starts, so it stays in the
// 2d settings, and the switch only pauses a session that started with 3D.
#define COG_ROW3D_PRESET 0
#define COG_ROW3D_SEPARATION 1
#define COG_ROW3D_CONVERGENCE 2
#define COG_ROW3D_SWITCH 3
#define COG_ROW3D_COUNT 4
#define COG_STEREO_CELLS 2
// The presets, in the order their cells are drawn. Each is a separation on the
// depth track: Balanced is the running model's default, and Comfort and Strong
// are this many steps of the track under and over it, kept on the track.
#define COG_PRESET_COMFORT 0
#define COG_PRESET_BALANCED 1
#define COG_PRESET_STRONG 2
#define COG_PRESET_CELLS 3
#define COG_PRESET_STEPS 3
// Right hand end of the separation track, as a fraction of frame width. Three
// times the 0.5 percent that phase 6 measured as the useful maximum: past
// there depth stops growing and only the strain does, so the far end of the
// track is drawn marked rather than left off.
#define COG_SEP_MAX 0.015f
// Steps along that track, so a dragged value lands exactly on one of the
// tenths of a percent the preference is stored in
#define COG_SEP_STEPS 15

// Picture tab rows are the four PICTURE_ values in their order, each a track
// with a tick at the picture as streamed, over the reset button. Live in
// every session and every room, since the grade only changes colours.

// About tab: the app and its version, and no rows, only two buttons: the one
// that opens the Ko-fi sheet with the page's QR code, with a line under it
// saying it is optional, over the one that puts the panel away for the report
// sheet. Where the buttons sit on the panel.
#define COG_KOFI_L 0.25f
#define COG_KOFI_R 0.75f
#define COG_KOFI_T 0.45f
#define COG_KOFI_B 0.57f
#define COG_REPORT_L 0.25f
#define COG_REPORT_R 0.75f
#define COG_REPORT_T 0.69f
#define COG_REPORT_B 0.81f

// Display tab rows. Cells rather than a track, so a press picks one instead of
// dragging a value. The head aim, gamepad and ray rows are the bar's buttons
// for the session, so they are not stored. Head aim's is greyed unless the
// screen is locked to the head outside a room. The gamepad row is Pointer or
// Gamepad, what the controllers are; the controllers row is whether their
// model is drawn.
#define COG_OPTION_SHARPEN 0
#define COG_OPTION_SUPERSAMPLE 1
#define COG_OPTION_STATS   2
#define COG_OPTION_HEAD_LOCK 3
#define COG_OPTION_HEAD_AIM 4
#define COG_OPTION_GAMEPAD 5
#define COG_OPTION_POINTER_SLEEP 6
#define COG_OPTION_RAY 7
#define COG_OPTION_CONTROLLERS 8
#define COG_OPTION_CLICK_SOUND 9
#define COG_OPTION_AMBILIGHT 10
#define COG_OPTION_ROOM_LIGHT 11
#define COG_OPTION_COUNT   12
#define COG_SHARPEN_CELLS 3
#define COG_SUPERSAMPLE_CELLS 3
#define COG_STATS_CELLS   2
#define COG_HEAD_LOCK_CELLS 2
#define COG_HEAD_AIM_CELLS 2
#define COG_GAMEPAD_CELLS 2
#define COG_POINTER_SLEEP_CELLS 2
#define COG_RAY_CELLS 2
#define COG_CONTROLLERS_CELLS 2
#define COG_CLICK_SOUND_CELLS 2
#define COG_AMBI_CELLS    2
#define COG_ROOM_LIGHT_CELLS 2
// The one row on this tab that is a track rather than cells, under the option
// rows, so the glow can be turned down without leaving the tab it lives on.
// This tab has no reset button for it to land on.
#define COG_DISPLAY_SLIDER_ROW 12
// Thirteen rows on this tab, so its rows start a little higher and sit closer
// together than the other tabs', with shallower cells to keep a gap between
// them. The last is centred at 0.917 and its thumb still clears the bottom
// edge, grown or not. The hit band is half the pitch, so neighbouring bands
// meet without overlapping, and the first starts just under the tab bar.
#define COG_DISPLAY_ROW_V0 0.197f
#define COG_DISPLAY_ROW_STEP 0.06f
#define COG_DISPLAY_ROW_HALF 0.03f
#define COG_DISPLAY_CELL_HALF 0.025f
// The marks on the display tab's cells, which of each row's cells is in
// force, drawn in Java as one strip over the column of cells rather than a
// ring each, so the tab costs one layer for them however many rows it has.
// Redrawn when one changes. Where the strip sits, as fractions of the panel.
#define COG_MARKS_TEX_W 408
#define COG_MARKS_TEX_H 460
#define COG_MARKS_L 0.41f
#define COG_MARKS_T 0.17f
// The time and the battery on a strip just over the panel's top edge, drawn
// in Java when the minute or the battery moves. Half the panel's width.
#define COG_CLOCK_TEX_W 384
#define COG_CLOCK_TEX_H 48

// In world keyboard, for the login boxes and chat windows that turn up mid
// stream. One sheet of art per state, drawn in Java like the other panels, and
// the layout arrives with it: the native side is handed rectangles and codes
// and knows nothing else about what the keys say. The fourth sheet, reached
// from the symbols, has the keys that type nothing: Esc, the F keys, the
// arrows and the rest, and the Ctrl, Alt and Win that stay held until the
// next key goes with them.
#define KB_TEX_W 1120
#define KB_TEX_H 448
#define KB_STATE_LOWER   0
#define KB_STATE_UPPER   1
#define KB_STATE_SYMBOLS 2
#define KB_STATE_FN      3
#define KB_STATE_COUNT   4
// Codes under zero change the keyboard instead of typing, and 0 is a blank
// with no key on it. Everything from 8 up to KB_CODE_VK is sent on as it
// stands, and a Windows virtual key code added to KB_CODE_VK is sent as that
// key.
#define KB_CODE_SHIFT   -2
#define KB_CODE_SYMBOLS -3
#define KB_CODE_HIDE    -4
#define KB_CODE_FN      -5
#define KB_CODE_CTRL    -6
#define KB_CODE_ALT     -7
#define KB_CODE_WIN     -8
#define KB_CODE_VK      65536
// The modifier bits, the same as the host protocol's own
#define KB_MOD_CTRL 2
#define KB_MOD_ALT  4
#define KB_MOD_WIN  8

// Report a problem, the sheet the About tab's button opens over the picture
// with the keyboard under it: a note, an address to answer, a line saying
// what goes, and Cancel and Send. Unlike the other panels it is drawn again
// in Java whenever what it shows changes, the words in its fields above all,
// so it is one sheet. Where its parts sit, as fractions of it.
#define REPORT_TEX_W 1024
#define REPORT_TEX_H 640
#define REPORT_FIELD_L 0.05f
#define REPORT_FIELD_R 0.95f
#define REPORT_NOTE_T 0.175f
#define REPORT_NOTE_B 0.425f
#define REPORT_EMAIL_T 0.50f
#define REPORT_EMAIL_B 0.5875f
#define REPORT_BTN_T 0.8125f
#define REPORT_BTN_B 0.9375f
#define REPORT_CANCEL_L 0.05f
#define REPORT_CANCEL_R 0.47f
#define REPORT_SEND_L 0.53f
#define REPORT_SEND_R 0.95f
// The parts a press can land on, which is also what IN_REPORT says was
// pressed, and the sheet having just come up
#define REPORT_ZONE_NONE 0
#define REPORT_ZONE_NOTE 1
#define REPORT_ZONE_EMAIL 2
#define REPORT_ZONE_CANCEL 3
#define REPORT_ZONE_SEND 4
#define REPORT_OPENED 5

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

// The hand lock hint, the sheet that says once a session how the triple pinch
// locks the hands, the first time they point: its words over OK and "Don't
// show this again", in the exit prompt's style. One sheet, drawn in Java at
// the start of a session that may show it, with the ring over the button
// under the ray a quad of its own. Where the buttons sit, as fractions of it.
#define HINT_TEX_W 1024
#define HINT_TEX_H 448
#define HINT_BTN_T 0.70f
#define HINT_BTN_B 0.88f
#define HINT_OK_L 0.06f
#define HINT_OK_R 0.36f
#define HINT_NEVER_L 0.42f
#define HINT_NEVER_R 0.94f
// Which of its buttons a point is on
#define HINT_ZONE_NONE  0
#define HINT_ZONE_OK    1
#define HINT_ZONE_NEVER 2

// The Ko-fi sheet the About tab's button opens, in the exit prompt's style:
// the title, the page's QR code on the left, its address and a line on the
// right, and one Close button. Drawn once in Java at the start of a session,
// with the ring over the button a quad of its own. The code goes on a pixel
// for a pixel, its quiet zone included, with its top left corner where these
// say; the button sits where these say, all as fractions of the sheet.
#define KOFI_TEX_W 1024
#define KOFI_TEX_H 512
#define KOFI_QR_PX 296
#define KOFI_QR_L 0.05f
#define KOFI_QR_T 0.22f
#define KOFI_BTN_T 0.76f
#define KOFI_BTN_B 0.92f
#define KOFI_CLOSE_L 0.64f
#define KOFI_CLOSE_R 0.95f
// Whether a point is on its button
#define KOFI_ZONE_NONE  0
#define KOFI_ZONE_CLOSE 1

#endif
