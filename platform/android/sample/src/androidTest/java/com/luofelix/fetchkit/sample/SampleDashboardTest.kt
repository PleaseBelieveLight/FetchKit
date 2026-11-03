package com.luofelix.fetchkit.sample

import android.graphics.Bitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class SampleDashboardTest {
    @get:Rule val compose = createAndroidComposeRule<MainActivity>()

    private fun capture(name: String) {
        if (InstrumentationRegistry.getArguments().getString("fetchkitSampleScreenshots") != "true") return
        compose.waitForIdle()
        val instrumentation = InstrumentationRegistry.getInstrumentation()
        val node = compose.onNodeWithTag(if (name == "presets") "download-form" else "sample-app")
        val bitmap = node.captureToImage().asAndroidBitmap()
        File(instrumentation.targetContext.cacheDir, "sample-preview-$name.png").outputStream().use {
            check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it))
        }
    }

    @Test fun presetsAreReadyWithoutTypingAndBatchSelectionChangesAction() {
        capture("home")
        compose.onNodeWithTag("new-download").assertIsDisplayed().assertContentDescriptionEquals("新建下载").performClick()
        compose.onNodeWithText("中科大镜像 · Alpine Standard").assertIsDisplayed()
        compose.onNodeWithText("南大镜像 · Alpine Virtual").assertIsDisplayed()
        compose.onNodeWithTag("preset-ustc-alpine-3-22-5-standard").assertIsOn()
        capture("presets")
        compose.onNodeWithText("更多镜像 · 大文件与快捷验证").performScrollTo().performClick()
        compose.onNodeWithTag("preset-nju-alpine-3-22-5-extended").performScrollTo().assertIsOff()
        compose.onNodeWithTag("preset-tuna-alpine-3-22-5-sha256").performScrollTo().assertIsOff()
        compose.onNodeWithText("收起更多镜像").performScrollTo().performClick()
        compose.onNodeWithTag("submit-download").performScrollTo().assertIsEnabled()
        compose.onNodeWithText("开始下载 · 1 个文件", useUnmergedTree = true).assertExists()
        compose.onNodeWithTag("preset-nju-alpine-3-24-3-virt").performScrollTo().performClick().assertIsOn()
        compose.onNodeWithTag("submit-download").performScrollTo().assertIsEnabled()
        compose.onNodeWithText("开始下载 · 2 个文件", useUnmergedTree = true).assertExists()
        compose.onNodeWithTag("preset-ustc-alpine-3-22-5-standard").performScrollTo().performClick().assertIsOff()
        compose.onNodeWithTag("preset-nju-alpine-3-24-3-virt").performScrollTo().performClick().assertIsOff()
        compose.onNodeWithTag("submit-download").performScrollTo().assertIsNotEnabled()
    }

    @Test fun directLookupValidatesIdAndShowsNotFound() {
        compose.onNodeWithText("按 ID 查询").performClick()
        compose.onNodeWithText("即时查询任务").assertIsDisplayed()
        compose.onNodeWithText("查询").assertIsNotEnabled()
        compose.onNodeWithTag("task-lookup-id").performTextInput("0".repeat(64))
        compose.onNodeWithText("查询").performClick()
        compose.waitUntil(5000) { compose.onAllNodesWithText("没有找到这个任务").fetchSemanticsNodes().isNotEmpty() }
        compose.onNodeWithText("关闭").performClick()
        compose.onNodeWithText("即时查询任务").assertDoesNotExist()
    }

    @Test fun storageAndGuideAreReachable() {
        compose.onNodeWithText("存储", useUnmergedTree = true).performClick()
        compose.onNodeWithText("资源与存储").assertIsDisplayed()
        compose.onNodeWithText("SDK 配额").assertIsDisplayed()
        capture("storage")
        compose.onNodeWithText("能力指南", useUnmergedTree = true).performClick()
        compose.onNodeWithText("把能力跑一遍").assertIsDisplayed()
        compose.onNodeWithText("01  /  多文件并发").assertIsDisplayed()
        capture("guide")
    }
}
