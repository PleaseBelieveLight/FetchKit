plugins {
    alias(libs.plugins.android.application)
}

val sizeMode = providers.gradleProperty("fetchkit.sizeMode").get()
require(sizeMode in setOf("bare", "scaffold", "probe", "okhttp"))
require((providers.gradleProperty("fetchkit.probes").orNull == "true") == (sizeMode in setOf("probe", "okhttp")))

android {
    namespace = "com.luofelix.fetchkit.sizehost"
    compileSdk { version = release(37) }
    enableKotlin = false
    defaultConfig {
        applicationId = "com.luofelix.fetchkit.sizehost"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += providers.gradleProperty("fetchkit.abis").orNull ?: "arm64-v8a" }
    }
    buildTypes {
        release { optimization { enable = false } }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures { buildConfig = false }
}

androidComponents {
    onVariants { variant ->
        val sourceMode = if (sizeMode == "okhttp") "probe" else sizeMode
        variant.sources.java?.addStaticSourceDirectory("src/modes/$sourceMode/java")
    }
}

dependencies {
    if (sizeMode != "bare") implementation(project(":sdk"))
}
