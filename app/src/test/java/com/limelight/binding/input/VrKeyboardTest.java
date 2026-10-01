package com.limelight.binding.input;

import com.limelight.binding.video.XrShared;
import com.limelight.nvstream.input.KeyboardPacket;

import org.junit.Test;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static org.junit.Assert.assertEquals;

/**
 * The host events the in world keyboard sends: the modifiers held down for as
 * long as they are lit, the keys typed with them, and plain typing unchanged.
 */
public class VrKeyboardTest {

    private static final int CTRL = XrShared.KB_MOD_CTRL;
    private static final int ALT = XrShared.KB_MOD_ALT;
    private static final int WIN = XrShared.KB_MOD_WIN;

    // Every event as "down A2 m2", "up 43 m2" or "text @"
    private static final class Recorder implements VrKeyboard.Sink {
        final List<String> events = new ArrayList<>();

        @Override
        public void key(short keyCode, byte action, byte modifiers) {
            String what = action == KeyboardPacket.KEY_DOWN ? "down" : "up";
            events.add(String.format("%s %02X m%d", what, keyCode, modifiers));
        }

        @Override
        public void text(String text) {
            events.add("text " + text);
        }
    }

    private static int vk(int code) {
        return XrShared.KB_CODE_VK + code;
    }

    @Test
    public void ctrlC() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.hold(CTRL);
        kb.type('c', CTRL);
        kb.hold(0);
        assertEquals(Arrays.asList("down A2 m2", "down 43 m2", "up 43 m2", "up A2 m0"), r.events);
        assertEquals(0, kb.held());
    }

    @Test
    public void altTab() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.hold(ALT);
        kb.type(9, ALT);
        kb.hold(0);
        assertEquals(Arrays.asList("down A4 m4", "down 09 m4", "up 09 m4", "up A4 m0"), r.events);
    }

    @Test
    public void winD() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.hold(WIN);
        kb.type('d', WIN);
        kb.hold(0);
        assertEquals(Arrays.asList("down 5B m8", "down 44 m8", "up 44 m8", "up 5B m0"), r.events);
    }

    @Test
    public void winAloneIsATapOfIt() {
        // Lit and put out again with nothing between, which opens Start
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.hold(WIN);
        kb.hold(0);
        assertEquals(Arrays.asList("down 5B m8", "up 5B m0"), r.events);
    }

    @Test
    public void ctrlAltTogetherComeUpTheOtherWay() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.hold(CTRL);
        kb.hold(CTRL | ALT);
        kb.type(vk(0x73), CTRL | ALT);
        kb.hold(0);
        assertEquals(Arrays.asList("down A2 m2", "down A4 m6", "down 73 m6", "up 73 m6",
                "up A4 m2", "up A2 m0"), r.events);
    }

    @Test
    public void ctrlShiftT() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.hold(CTRL);
        kb.type('T', CTRL);
        kb.hold(0);
        assertEquals(Arrays.asList("down A2 m2", "down 10 m2", "down 54 m3", "up 54 m3",
                "up 10 m2", "up A2 m0"), r.events);
    }

    @Test
    public void aKeyWithModifiersNotYetDownPutsThemDown() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.type('v', CTRL);
        assertEquals(Arrays.asList("down A2 m2", "down 56 m2", "up 56 m2"), r.events);
        assertEquals(CTRL, kb.held());
        kb.releaseAll();
        assertEquals("up A2 m0", r.events.get(r.events.size() - 1));
    }

    @Test
    public void theFnSheetsKeysGoAsTheirVirtualKeys() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.type(vk(0x1B), 0);
        kb.type(vk(0x7B), 0);
        kb.type(vk(0x2E), 0);
        kb.type(vk(0x25), 0);
        assertEquals(Arrays.asList("down 1B m0", "up 1B m0", "down 7B m0", "up 7B m0",
                "down 2E m0", "up 2E m0", "down 25 m0", "up 25 m0"), r.events);
    }

    @Test
    public void plainTypingIsAsItWas() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.type('a', 0);
        kb.type('A', 0);
        kb.type('7', 0);
        kb.type(8, 0);
        kb.type(13, 0);
        kb.type(32, 0);
        kb.type('@', 0);
        kb.type('-', 0);
        assertEquals(Arrays.asList("down 41 m0", "up 41 m0",
                "down 10 m0", "down 41 m1", "up 41 m1", "up 10 m0",
                "down 37 m0", "up 37 m0", "down 08 m0", "up 08 m0", "down 0D m0", "up 0D m0",
                "down 20 m0", "up 20 m0", "text @", "text -"), r.events);
        assertEquals(0, kb.held());
    }

    @Test
    public void punctuationWithAModifierGoesAsItsKey() {
        // Ctrl and minus zooms out, which text could not do
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.hold(CTRL);
        kb.type('-', CTRL);
        kb.type('=', CTRL);
        kb.type('/', CTRL);
        // A shifted one has no key of its own and still goes as text
        kb.type('+', CTRL);
        assertEquals(Arrays.asList("down A2 m2", "down BD m2", "up BD m2", "down BB m2", "up BB m2",
                "down BF m2", "up BF m2", "text +"), r.events);
    }

    @Test
    public void holdingWhatIsHeldSendsNothing() {
        Recorder r = new Recorder();
        VrKeyboard kb = new VrKeyboard(r);
        kb.hold(0);
        kb.releaseAll();
        kb.hold(ALT);
        kb.hold(ALT);
        assertEquals(Arrays.asList("down A4 m4"), r.events);
    }
}
