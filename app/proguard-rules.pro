# JNI
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.anc.app.engine.** { *; }

# Compose
-dontwarn androidx.compose.**
-keep class androidx.compose.** { *; }

# Kotlin Coroutines
-keepnames class kotlinx.coroutines.internal.MainDispatcherFactory {}
-keepnames class kotlinx.coroutines.CoroutineExceptionHandler {}
-keepclassmembers class kotlinx.coroutines.** {
    volatile <fields>;
}

# Oboe JNI callbacks
-keep class com.google.oboe.** { *; }
