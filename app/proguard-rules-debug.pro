# Debug builds only, on top of proguard-rules.pro.
#
# The debug build used to get this flag from proguard-android.txt, which AGP 9
# no longer accepts precisely because it carries it. Kept here so a debug build
# goes on doing what it always did: shrink and obfuscate, but skip R8's
# optimization passes, so it builds quickly and its stack traces stay close to
# the source. The release build has always optimized and is untouched.
-dontoptimize
