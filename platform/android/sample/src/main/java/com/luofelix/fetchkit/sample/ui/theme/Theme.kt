package com.luofelix.fetchkit.sample.ui.theme

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

private val LightColors = lightColorScheme(
    primary = Color(0xFF006B5E), onPrimary = Color.White,
    primaryContainer = Color(0xFFD0F0E6), onPrimaryContainer = Color(0xFF00382F),
    secondary = Color(0xFF466270), secondaryContainer = Color(0xFFDCEAF0),
    background = Color(0xFFF5F7F8), surface = Color(0xFFF5F7F8),
    surfaceContainerLow = Color.White, surfaceContainer = Color(0xFFECF1F2),
    surfaceContainerHigh = Color(0xFFE5ECEE), onSurface = Color(0xFF182C36),
    onSurfaceVariant = Color(0xFF526570), outlineVariant = Color(0xFFD8E2E5),
)
private val DarkColors = darkColorScheme(
    primary = Color(0xFF79D7BD), primaryContainer = Color(0xFF164E43),
    onPrimary = Color(0xFF00382F), onPrimaryContainer = Color(0xFFC1F5E4),
    secondary = Color(0xFFAACBD9), background = Color(0xFF0E191F), surface = Color(0xFF0E191F),
    surfaceContainerLow = Color(0xFF18262D), surfaceContainer = Color(0xFF213138),
    surfaceContainerHigh = Color(0xFF2B3C44), onSurface = Color(0xFFE0EBEF),
    onSurfaceVariant = Color(0xFFB0C3CC), outlineVariant = Color(0xFF3A4F59),
)

@Composable
fun FetchKitAndroidTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    dynamicColor: Boolean = false,
    content: @Composable () -> Unit,
) {
    val colors = if (dynamicColor && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        val context = LocalContext.current
        if (darkTheme) dynamicDarkColorScheme(context) else dynamicLightColorScheme(context)
    } else if (darkTheme) DarkColors else LightColors
    MaterialTheme(colorScheme = colors, typography = Typography, content = content)
}
