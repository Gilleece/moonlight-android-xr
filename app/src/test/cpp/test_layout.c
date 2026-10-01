// The hover zones, the stand in screen, the Room tab's lanes and the 3D tab's
// depth track and presets, checked against answers that can be worked out by
// hand
#include "check.h"
#include "xr_layout.h"
#include "xr_shared.h"

// A 3 m picture at the video's usual shape, the size a screen starts at
static const float W = 3.0f;
static const float H = 3.0f * 9.0f / 16.0f;

static void testCornersFollowTheirArt(void) {
    float side = CORNER_FRAC * W;
    // The bracket is drawn centred half a bracket outside each corner, and
    // its centre is where the zone is centred too
    float outU = side * 0.5f / W;
    float outV = side * 0.5f / H;
    int corner = -1;
    CHECK(hoverTest(-outU, -outV, W, H, side, &corner) == HOVER_CORNER && corner == 0);
    CHECK(hoverTest(1.0f + outU, -outV, W, H, side, &corner) == HOVER_CORNER && corner == 1);
    CHECK(hoverTest(-outU, 1.0f + outV, W, H, side, &corner) == HOVER_CORNER && corner == 2);
    CHECK(hoverTest(1.0f + outU, 1.0f + outV, W, H, side, &corner) == HOVER_CORNER
          && corner == 3);

    // The corner itself, where the bracket's inner tip touches, is inside the
    // zone and so is the picture a little in from it
    CHECK(hoverTest(0.0f, 0.0f, W, H, side, &corner) == HOVER_CORNER && corner == 0);
    CHECK(hoverTest(0.01f, 0.01f, W, H, side, &corner) == HOVER_CORNER);

    // The zone reaches CORNER_HOVER halves of a bracket each way from its
    // centre and no further
    float reachU = side * CORNER_HOVER * 0.5f / W;
    CHECK(hoverTest(-outU - reachU * 0.98f, -outV, W, H, side, &corner) == HOVER_CORNER);
    CHECK(hoverTest(-outU - reachU * 1.02f, -outV, W, H, side, &corner) != HOVER_CORNER);
    CHECK(hoverTest(-outU + reachU * 1.02f, -outV, W, H, side, &corner) != HOVER_CORNER);

    // Along an edge away from the corners is the picture or its margin
    CHECK(hoverTest(0.5f, 0.0f, W, H, side, &corner) == HOVER_SCREEN);
    CHECK(hoverTest(0.5f, -outV, W, H, side, &corner) == HOVER_HALO);
}

static void testNoCornersWhereThereAreNone(void) {
    // A room that keeps its picture whole has no brackets, so the ray falls
    // through to the picture and its margin
    int corner = -1;
    float side = CORNER_FRAC * W;
    float outU = side * 0.5f / W;
    float outV = side * 0.5f / H;
    CHECK(hoverTest(-outU, -outV, W, H, 0.0f, &corner) == HOVER_HALO);
    CHECK(hoverTest(0.0f, 0.0f, W, H, 0.0f, &corner) == HOVER_SCREEN);
    CHECK(hoverTest(1.0f + outU, 1.0f + outV, W, H, 0.0f, &corner) == HOVER_HALO);
    CHECK(corner == -1);
}

static void testTheRestOfThePicture(void) {
    int corner = -1;
    float side = CORNER_FRAC * W;
    CHECK(hoverTest(0.5f, 0.5f, W, H, side, &corner) == HOVER_SCREEN);
    // The bar's zone under the bottom edge, and past the halo nothing at all
    CHECK(hoverTest(0.5f, 1.05f, W, H, side, &corner) == HOVER_BAR);
    CHECK(hoverTest(0.5f, 2.0f, W, H, side, &corner) == HOVER_NONE);
    CHECK(hoverTest(-0.6f, 0.5f, W, H, side, &corner) == HOVER_NONE);
}

static void testTheStandIn(void) {
    // Straight ahead of the seat at eye level, square to it, 3 m wide
    XrPosef pose = standInPose();
    CHECK_NEAR(pose.position.x, 0.0f, 1e-6);
    CHECK_NEAR(pose.position.y, 0.0f, 1e-6);
    CHECK_NEAR(pose.position.z, -STAND_IN_DISTANCE_M, 1e-6);
    CHECK_NEAR(pose.orientation.w, 1.0f, 1e-6);
    CHECK_NEAR(STAND_IN_WIDTH_M, 3.0f, 1e-6);

    // A ray from the seat lands on it where the placement says it should, so
    // the furniture hit tests follow what is drawn: straight ahead is the
    // middle, and aimed at the bar's place under it is the bar's zone
    XrPosef aim;
    aim.position.x = 0.0f;
    aim.position.y = 0.0f;
    aim.position.z = 0.0f;
    aim.orientation.x = 0.0f;
    aim.orientation.y = 0.0f;
    aim.orientation.z = 0.0f;
    aim.orientation.w = 1.0f;
    float u, v;
    CHECK(screenProject(aim, pose, STAND_IN_WIDTH_M, H, 0.0f, 0, &u, &v));
    CHECK_NEAR(u, 0.5f, 1e-5);
    CHECK_NEAR(v, 0.5f, 1e-5);

    float barY = -(H * 0.5f + STAND_IN_WIDTH_M * (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f));
    float pitch = atan2f(barY, STAND_IN_DISTANCE_M);
    aim.orientation.x = sinf(pitch * 0.5f);
    aim.orientation.w = cosf(pitch * 0.5f);
    CHECK(screenProject(aim, pose, STAND_IN_WIDTH_M, H, 0.0f, 0, &u, &v));
    int corner;
    CHECK(hoverTest(u, v, STAND_IN_WIDTH_M, H, 0.0f, &corner) == HOVER_BAR);
}

static void testRoomCornersLookTheSameEverywhere(void) {
    // At the stand in's distance a bracket is the size one is on a 3 m screen
    CHECK_NEAR(roomCornerSide(STAND_IN_DISTANCE_M), CORNER_FRAC * STAND_IN_WIDTH_M, 1e-6);
    // Further off it grows with the distance, so it subtends the same angle
    CHECK_NEAR(roomCornerSide(14.0f), CORNER_FRAC * STAND_IN_WIDTH_M * 14.0f / 3.0f, 1e-5);
    CHECK_NEAR(roomCornerSide(4.15f) / 4.15f, roomCornerSide(14.0f) / 14.0f, 1e-6);
    CHECK_NEAR(roomCornerSide(0.0f), 0.0f, 1e-6);
}

static void testLanes(void) {
    // Each room's starting brightness reads as the percent of the lane it sits at
    CHECK(lanePercent(ROOM_THEATER_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == 20);
    CHECK(lanePercent(ROOM_GRAND_CINEMA_BRIGHTNESS, ROOM_BRIGHTNESS_MIN,
                      ROOM_BRIGHTNESS_MAX) == 10);
    CHECK(lanePercent(ROOM_SYNTHWAVE_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == 30);
    CHECK(lanePercent(ROOM_LIGHT_DEFAULT, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX) == 75);

    // Both ends, and a place off either end held at it
    CHECK(laneUnits(0.0f, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == ROOM_BRIGHTNESS_MIN);
    CHECK(laneUnits(1.0f, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == ROOM_BRIGHTNESS_MAX);
    CHECK(laneUnits(-0.3f, ROOM_SCREEN_MIN, ROOM_SCREEN_MAX) == ROOM_SCREEN_MIN);
    CHECK(laneUnits(1.4f, ROOM_SCREEN_MIN, ROOM_SCREEN_MAX) == ROOM_SCREEN_MAX);
    CHECK_NEAR(lanePlace(ROOM_BRIGHTNESS_MIN - 3, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX),
               0.0f, 1e-6);
    CHECK_NEAR(lanePlace(ROOM_LIGHT_MAX + 30, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX), 1.0f, 1e-6);

    // Every unit on each lane survives the trip to a thumb's place and back,
    // which is what lets a thumb show exactly what gets written
    int lanes[3][2] = { { ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX },
                        { ROOM_LIGHT_MIN, ROOM_LIGHT_MAX },
                        { ROOM_SCREEN_MIN, ROOM_SCREEN_MAX } };
    int trips = 0;
    for (int lane = 0; lane < 3; lane++) {
        int min = lanes[lane][0];
        int max = lanes[lane][1];
        for (int units = min; units <= max; units++) {
            trips += laneUnits(lanePlace(units, min, max), min, max) == units;
        }
    }
    CHECK(trips == (ROOM_BRIGHTNESS_MAX - ROOM_BRIGHTNESS_MIN + 1)
                   + (ROOM_LIGHT_MAX - ROOM_LIGHT_MIN + 1)
                   + (ROOM_SCREEN_MAX - ROOM_SCREEN_MIN + 1));
}

static void testTheSizeClamp(void) {
    CHECK(roomScreenClamp(80, 1) == 80);
    CHECK(roomScreenClamp(10, 1) == ROOM_SCREEN_MIN);
    CHECK(roomScreenClamp(-40, 1) == ROOM_SCREEN_MIN);
    CHECK(roomScreenClamp(130, 1) == ROOM_SCREEN_MAX);
    // A room that keeps its picture whole hangs all of it whatever was saved
    CHECK(roomScreenClamp(40, 0) == ROOM_SCREEN_MAX);
    CHECK(roomScreenClamp(ROOM_SCREEN_MIN, 0) == ROOM_SCREEN_MAX);
}

static void testTheCornerDrag(void) {
    // Scaled from where the drag started, in whole percent
    CHECK(roomResizePercent(80, 1.0f, 1.0f) == 80);
    CHECK(roomResizePercent(80, 1.0f, 0.5f) == 40);
    CHECK(roomResizePercent(80, 1.0f, 1.1f) == 88);
    CHECK(roomResizePercent(60, 1.0f, 1.333f) == 80);
    // Taking hold of a bracket out past the corner moves nothing until the
    // hand does, however far out it was taken hold of
    CHECK(roomResizePercent(25, 1.41f, 1.41f) == 25);
    CHECK(roomResizePercent(25, 2.03f, 2.03f) == 25);
    CHECK(roomResizePercent(40, 2.0f, 1.0f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(40, 2.0f, 3.0f) == 60);
    // Never under a quarter of the room's screen or over all of it
    CHECK(roomResizePercent(80, 1.0f, 0.1f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(100, 1.0f, 0.25f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(100, 1.0f, 0.2f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(80, 1.0f, 2.0f) == ROOM_SCREEN_MAX);
    CHECK(roomResizePercent(25, 1.0f, 4.0f) == ROOM_SCREEN_MAX);
    // A ray behind the centre, or a grab begun on it, holds rather than
    // flipping the picture or dividing by nothing
    CHECK(roomResizePercent(80, 1.0f, -0.5f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(80, 0.0f, 0.05f) == 80);
    // Every step between lands on a whole percent inside the lane
    int inside = 1;
    for (int i = 1; i <= 400; i++) {
        int p = roomResizePercent(60, 1.2f, i * 0.01f);
        inside &= p >= ROOM_SCREEN_MIN && p <= ROOM_SCREEN_MAX;
    }
    CHECK(inside);
}

static void testTheDepthTrack(void) {
    // Every step of the track survives the trip to the fraction the warp
    // takes and back, and to a thumb's place and back, so what the thumb
    // shows is what gets written
    int trips = 0;
    for (int units = 0; units <= COG_SEP_STEPS; units++) {
        trips += separationUnits(separationOf(units)) == units;
        trips += laneUnits(lanePlace(units, 0, COG_SEP_STEPS), 0, COG_SEP_STEPS) == units;
    }
    CHECK(trips == 2 * (COG_SEP_STEPS + 1));
    CHECK(separationUnits(COG_SEP_MAX) == COG_SEP_STEPS);
    CHECK_NEAR(separationOf(5), 0.005f, 1e-7);

    // A drag lands on the nearest step and never off either end
    CHECK(laneUnits(0.39f, 0, COG_SEP_STEPS) == 6);
    CHECK(laneUnits(0.36f, 0, COG_SEP_STEPS) == 5);
    CHECK(laneUnits(-0.2f, 0, COG_SEP_STEPS) == 0);
    CHECK(laneUnits(1.3f, 0, COG_SEP_STEPS) == COG_SEP_STEPS);

    // The debug property can ask for more than the track shows, and that is
    // read as it is rather than pulled back onto the track
    CHECK(separationUnits(0.04f) == 40);
}

static void testThePresetAccent(void) {
    // ZipDepth's three, and MiDaS's
    int zip[COG_PRESET_CELLS] = { 3, 6, 9 };
    int midas[COG_PRESET_CELLS] = { 2, 5, 8 };
    CHECK(cogPresetAt(3, zip) == COG_PRESET_COMFORT);
    CHECK(cogPresetAt(6, zip) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(9, zip) == COG_PRESET_STRONG);
    CHECK(cogPresetAt(2, midas) == COG_PRESET_COMFORT);
    CHECK(cogPresetAt(5, midas) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(8, midas) == COG_PRESET_STRONG);

    // Anywhere else on the track, or past it, rings nothing
    int elsewhere = 0;
    for (int units = 0; units <= COG_SEP_STEPS; units++) {
        if (units != 3 && units != 6 && units != 9) {
            elsewhere += cogPresetAt(units, zip) == -1;
        }
    }
    CHECK(elsewhere == COG_SEP_STEPS + 1 - 3);
    CHECK(cogPresetAt(40, zip) == -1);

    // A default at an end clamps another preset onto it, and that is Balanced
    int low[COG_PRESET_CELLS] = { 0, 0, 3 };
    int high[COG_PRESET_CELLS] = { 12, 15, 15 };
    CHECK(cogPresetAt(0, low) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(15, high) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(3, low) == COG_PRESET_STRONG);

    // Read back off the fraction the warp holds, the way the ring reads it
    CHECK(cogPresetAt(separationUnits(separationOf(9)), zip) == COG_PRESET_STRONG);
    CHECK(cogPresetAt(separationUnits(0.0061f), zip) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(separationUnits(0.0068f), zip) == -1);
}

// Every tab's rows fit inside the panel below the tab bar, with their hit
// bands apart and every cell drawn inside its own band
static void testTheRowsFit(void) {
    int counts[COG_TAB_COUNT] = {
        COG_SLIDER_COUNT, COG_DISPLAY_SLIDER_ROW + 1, COG_ROW3D_COUNT, PICTURE_VALUES
    };
    for (int tab = 0; tab < COG_TAB_COUNT; tab++) {
        float half = cogRowHalf(tab);
        CHECK(cogCellHalf(tab) < half);
        CHECK(cogRowV(tab, 0) - half >= COG_TAB_BAR_B);
        for (int row = 1; row < counts[tab]; row++) {
            CHECK(cogRowV(tab, row) - half >= cogRowV(tab, row - 1) + half - 1e-6f);
        }
        float last = cogRowV(tab, counts[tab] - 1);
        // The thumb on a track is 0.085 of the panel's height across
        CHECK(last + 0.0425f < 1.0f - 0.03f);
        CHECK(last + half <= 1.0f);
    }
    // The display tab's nine rows, the glow level track last
    CHECK_NEAR(cogRowV(COG_TAB_DISPLAY, 0), 0.215, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW), 0.911, 1e-5);
    // The other tabs keep the rows they always had
    CHECK_NEAR(cogRowV(COG_TAB_SCREEN, 5), COG_ROW_V0 + 5 * COG_ROW_STEP, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_3D, 3), COG_ROW_V0 + 3 * COG_ROW_STEP, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_COUNT, 2), COG_ROW_V0 + 2 * COG_ROW_STEP, 1e-6);
    CHECK_NEAR(cogCellHalf(COG_TAB_3D), COG_CELL_HALF, 1e-6);
    // The Picture tab is the fourth, with the screen tab's spacing, and its
    // last row's band ends well clear of the reset button
    CHECK(COG_TAB_PICTURE == 3 && COG_TAB_COUNT == 4);
    CHECK_NEAR(cogRowV(COG_TAB_PICTURE, PICTURE_SATURATION),
               COG_ROW_V0 + PICTURE_SATURATION * COG_ROW_STEP, 1e-6);
    CHECK(cogRowV(COG_TAB_PICTURE, PICTURE_VALUES - 1) + cogRowHalf(COG_TAB_PICTURE)
          < COG_RESET_T);
    // Every room sheet follows the tabs, one per tab and the fixed Room tab
    CHECK(COG_ART_ROOM == COG_TAB_COUNT);
    CHECK(COG_ART_COUNT == COG_ART_ROOM + COG_TAB_COUNT + 1);
    // The strip beside the tracks reaches every Picture row
    float stripB = COG_READOUT_T + (float)COG_READOUT_TEX_H / (float)COG_TEX_H;
    for (int row = 0; row < PICTURE_VALUES; row++) {
        CHECK(cogRowV(COG_TAB_PICTURE, row) - COG_CELL_HALF > COG_READOUT_T);
        CHECK(cogRowV(COG_TAB_PICTURE, row) + COG_CELL_HALF < stripB);
    }
}

// The step buttons at each end of a track, the run between them, and where a
// press of one lands
static void testTheTrackParts(void) {
    float mid = (COG_TRACK_L + COG_TRACK_R) * 0.5f;
    CHECK(cogTrackPart(mid) == TRACK_PART_RUN);
    CHECK(cogTrackPart(COG_TRACK_L + COG_CHEVRON_W * 0.5f) == TRACK_PART_DOWN);
    CHECK(cogTrackPart(COG_TRACK_R - COG_CHEVRON_W * 0.5f) == TRACK_PART_UP);
    // The buttons reach a little past the track's ends, and no further
    CHECK(cogTrackPart(COG_TRACK_L - COG_CHEVRON_REACH * 0.9f) == TRACK_PART_DOWN);
    CHECK(cogTrackPart(COG_TRACK_R + COG_CHEVRON_REACH * 0.9f) == TRACK_PART_UP);
    CHECK(cogTrackPart(COG_TRACK_L - COG_CHEVRON_REACH * 1.1f) == TRACK_PART_NONE);
    CHECK(cogTrackPart(COG_TRACK_R + COG_CHEVRON_REACH * 1.1f) == TRACK_PART_NONE);
    // The run's zone stops where a button starts, so a press on a button is
    // never a jump along the track
    CHECK(cogTrackPart(COG_TRACK_L + COG_CHEVRON_W * 1.01f) == TRACK_PART_RUN);
    CHECK(cogTrackPart(COG_TRACK_R - COG_CHEVRON_W * 1.01f) == TRACK_PART_RUN);
    CHECK(cogTrackPart(0.1f) == TRACK_PART_NONE);
    CHECK(cogTrackPart(0.99f) == TRACK_PART_NONE);

    // The run sits inside the buttons with room for a thumb at either end
    float thumbHalfU = 0.085f * 0.5f * (float)COG_TEX_H / (float)COG_TEX_W;
    CHECK(COG_RUN_L - thumbHalfU > COG_TRACK_L + COG_CHEVRON_W);
    CHECK(COG_RUN_R + thumbHalfU < COG_TRACK_R - COG_CHEVRON_W);
    CHECK_NEAR(cogRunPlace(COG_RUN_L), 0.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_RUN_R), 1.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_TRACK_L), 0.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_TRACK_R), 1.0, 1e-6);
    CHECK_NEAR(cogRunPlace(cogRunU(0.37f)), 0.37, 1e-6);
}

static void testTheStepsLandOnTheGrid(void) {
    // On a step, one either way
    CHECK(cogStepIndex(5.0f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.0f / 20.0f, 20, -1) == 4);
    // Read back a hair off it, still that step
    CHECK(cogStepIndex(5.0f / 20.0f - 1e-6f, 20, 1) == 6);
    CHECK(cogStepIndex(5.0f / 20.0f + 1e-6f, 20, -1) == 4);
    // Between two, the nearer one that way
    CHECK(cogStepIndex(5.4f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.4f / 20.0f, 20, -1) == 5);
    CHECK(cogStepIndex(5.6f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.6f / 20.0f, 20, -1) == 5);
    // Held to the ends
    CHECK(cogStepIndex(0.0f, 20, -1) == 0);
    CHECK(cogStepIndex(1.0f, 20, 1) == 20);
    CHECK(cogStepIndex(0.5f, 0, 1) == 0);
}

static void testEveryTrackHasItsSteps(void) {
    for (int row = 0; row < COG_SLIDER_COUNT; row++) {
        CHECK(cogTrackSteps(COG_TAB_SCREEN, row) > 0);
    }
    CHECK(cogTrackSteps(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW) == 20);
    CHECK(cogTrackSteps(COG_TAB_DISPLAY, COG_OPTION_STATS) == 0);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_SEPARATION) == COG_SEP_STEPS);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_CONVERGENCE) == 100);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_PRESET) == 0);
    CHECK(cogTrackSteps(COG_TAB_COUNT, COG_ROOM_ROW_GLOW) == 0);
    CHECK(cogTrackSteps(COG_TAB_COUNT, COG_ROOM_ROW_SIZE) == 75);

    // A Room lane's steps land on whole units, so a press always moves the
    // stored value by the same number of them, from wherever a drag left it
    int lanes[2][3] = {
        { COG_ROOM_ROW_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX },
        { COG_ROOM_ROW_LIGHT_LEVEL, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX }
    };
    for (int l = 0; l < 2; l++) {
        int steps = cogTrackSteps(COG_TAB_COUNT, lanes[l][0]);
        int min = lanes[l][1];
        int max = lanes[l][2];
        CHECK((max - min) % steps == 0);
        int stride = (max - min) / steps;
        int units = min;
        int presses = 0;
        while (units < max && presses < 1000) {
            int step = cogStepIndex(lanePlace(units, min, max), steps, 1);
            int next = laneUnits((float)step / (float)steps, min, max);
            CHECK(next - units == stride);
            units = next;
            presses++;
        }
        CHECK(presses == steps);
        // From off the grid, down to the step below
        int off = min + stride * 3 + 1;
        int step = cogStepIndex(lanePlace(off, min, max), steps, -1);
        CHECK(laneUnits((float)step / (float)steps, min, max) == min + stride * 3);
    }

    // The Picture tab steps in whole units, one a press, and every row's
    // default, where its tick is, is a step
    int pictureLanes[PICTURE_VALUES][3] = {
        { PICTURE_BRIGHTNESS_MIN, PICTURE_BRIGHTNESS_MAX, PICTURE_BRIGHTNESS_DEFAULT },
        { PICTURE_CONTRAST_MIN, PICTURE_CONTRAST_MAX, PICTURE_CONTRAST_DEFAULT },
        { PICTURE_GAMMA_MIN, PICTURE_GAMMA_MAX, PICTURE_GAMMA_DEFAULT },
        { PICTURE_SATURATION_MIN, PICTURE_SATURATION_MAX, PICTURE_SATURATION_DEFAULT }
    };
    CHECK(cogTrackSteps(COG_TAB_PICTURE, PICTURE_VALUES) == 0);
    CHECK(cogTrackSteps(COG_TAB_PICTURE, -1) == 0);
    for (int row = 0; row < PICTURE_VALUES; row++) {
        int min = pictureLanes[row][0];
        int max = pictureLanes[row][1];
        int def = pictureLanes[row][2];
        int steps = cogTrackSteps(COG_TAB_PICTURE, row);
        CHECK(steps == max - min);
        int up = cogStepIndex(lanePlace(def, min, max), steps, 1);
        CHECK(laneUnits((float)up / (float)steps, min, max) == def + 1);
        int down = cogStepIndex(lanePlace(def, min, max), steps, -1);
        CHECK(laneUnits((float)down / (float)steps, min, max) == def - 1);
        CHECK(laneUnits(lanePlace(def, min, max), min, max) == def);
        CHECK(laneUnits(0.0f, min, max) == min && laneUnits(1.0f, min, max) == max);
    }

    // The tilt and roll steps clear the snap to level either side of it
    float tiltStep = 2.0f * 40.0f / (float)cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_TILT);
    float rollStep = 2.0f * 90.0f / (float)cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_ROTATE);
    CHECK(tiltStep * 3.14159265f / 180.0f > 0.0388f);
    CHECK(rollStep * 3.14159265f / 180.0f > 0.0873f);
    // And level is a step of each
    CHECK(cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_TILT) % 2 == 0);
    CHECK(cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_ROTATE) % 2 == 0);
}

int main(void) {
    testTheRowsFit();
    testCornersFollowTheirArt();
    testNoCornersWhereThereAreNone();
    testTheRestOfThePicture();
    testTheStandIn();
    testRoomCornersLookTheSameEverywhere();
    testLanes();
    testTheSizeClamp();
    testTheCornerDrag();
    testTheDepthTrack();
    testThePresetAccent();
    testTheTrackParts();
    testTheStepsLandOnTheGrid();
    testEveryTrackHasItsSteps();
    return checksDone("xr_layout");
}
