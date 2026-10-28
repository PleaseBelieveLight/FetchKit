plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.luofelix.fetchkit.sample"
    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.luofelix.fetchkit"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        create("publicDemo") {
            // 所有者授权公开的演示密钥，仅供 sample 使用，不作为生产应用的签名身份。
            storeFile = file("signing/fetchkit-public-demo.p12")
            storeType = "PKCS12"
            storePassword = "fetchkit-demo-public"
            keyAlias = "fetchkit-public-demo"
            keyPassword = "fetchkit-demo-public"
        }
    }

    buildTypes {
        release {
            signingConfig = signingConfigs.getByName("publicDemo")
            optimization {
                // 统一优化应用与依赖，避免选择性优化后 Kotlin 继承链跨包导致访问错误；JNI 入口由 SDK consumer rules 保留。
                enable = true
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }
}

androidComponents {
    onVariants(selector().withBuildType("debug")) { variant ->
        if (providers.gradleProperty("fetchkit.probes").orNull == "true") {
            variant.androidTest?.sources?.kotlin?.addStaticSourceDirectory("src/probeTest/java")
            variant.androidTest?.sources?.assets?.addStaticSourceDirectory("../../../tests/fixtures")
        }
    }
}

dependencies {
    implementation(project(":sdk"))
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    testImplementation(libs.junit)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
