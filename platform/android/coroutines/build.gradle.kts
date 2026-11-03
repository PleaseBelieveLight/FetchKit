plugins {
    alias(libs.plugins.android.library)
}

abstract class GenerateCoroutinesLicense : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val licenseFile: RegularFileProperty

    @get:OutputDirectory
    abstract val outputDirectory: DirectoryProperty

    @TaskAction
    fun generate() {
        val target = outputDirectory.file("META-INF/fetchkit-coroutines/LICENSE").get().asFile
        target.parentFile.mkdirs()
        licenseFile.get().asFile.copyTo(target, overwrite = true)
    }
}

android {
    namespace = "com.luofelix.fetchkit.coroutines"
    compileSdk { version = release(37) }
    defaultConfig { minSdk = 24 }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

androidComponents {
    onVariants { variant ->
        val license = tasks.register<GenerateCoroutinesLicense>("generate${variant.name.replaceFirstChar { it.uppercase() }}License") {
            licenseFile.set(rootProject.layout.projectDirectory.file("LICENSE"))
        }
        variant.sources.resources?.addGeneratedSourceDirectory(license, GenerateCoroutinesLicense::outputDirectory)
    }
}

dependencies {
    api(project(":sdk"))
    api(libs.kotlinx.coroutines.core)
    testImplementation(libs.junit)
}
