plugins {
    alias(libs.plugins.android.application)
}

val sizeMode = providers.gradleProperty("fetchkit.sizeMode").get()
val r8Enabled = providers.gradleProperty("fetchkit.r8").orNull == "true"
val smokeSign = providers.gradleProperty("fetchkit.smokeSign").orNull == "true"
require(sizeMode in setOf("bare", "scaffold", "probe", "okhttp", "production"))
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
        release {
            optimization {
                enable = r8Enabled
                if (r8Enabled) packageScope = setOf("com.luofelix.**", "okhttp3.**", "okio.**", "androidx.**", "kotlin.**", "kotlinx.**")
            }
            if (smokeSign) signingConfig = signingConfigs.getByName("debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures { buildConfig = false }
    if (sizeMode == "production") {
        sourceSets.getByName("release") {
            manifest.srcFile("src/modes/production/AndroidManifest.xml")
            res.srcDir("src/modes/production/res")
        }
    }
}

androidComponents {
    onVariants { variant ->
        val sourceMode = if (sizeMode == "okhttp") "probe" else sizeMode
        variant.sources.java?.addStaticSourceDirectory("src/modes/$sourceMode/java")
    }
}

dependencies {
    val externalAar = providers.gradleProperty("fetchkit.aar").orNull
    if (sizeMode != "bare") {
        if (externalAar == null) implementation(project(":sdk"))
        else { implementation(files(externalAar)); implementation(libs.okhttp) }
    }
}
