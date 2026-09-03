# Debug only, alongside proguard-rules.pro.
#
# The debug build used to get this by way of getDefaultProguardFile(
# 'proguard-android.txt'), which AGP 9 no longer accepts precisely because it
# carries this flag. Keeping it here preserves what the debug build always
# did — shrink and obfuscate, but skip R8's optimization passes, so a debug
# build stays quick and its stack traces stay close to the source — without
# touching the release build, which has always optimized.
-dontoptimize
