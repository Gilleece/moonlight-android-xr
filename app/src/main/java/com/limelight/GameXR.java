package com.limelight;

/**
 * The streaming activity again, immersive this time. Behaviour is entirely
 * inherited, this exists only so the two entry points can carry different
 * manifest attributes.
 *
 * It used to be an activity-alias, which does not work: Pico reads the app
 * type from the target activity, so marking Game as a 2d panel to stop flat
 * streaming hanging also downgraded the immersive launch and the dock drew
 * over the stream. Two real activities keep the two paths independent.
 */
public class GameXR extends Game {
}
