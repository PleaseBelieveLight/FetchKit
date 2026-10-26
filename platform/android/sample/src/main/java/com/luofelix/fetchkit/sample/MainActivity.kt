package com.luofelix.fetchkit.sample

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import com.luofelix.fetchkit.FetchKit
import com.luofelix.fetchkit.sample.ui.theme.FetchKitAndroidTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            FetchKitAndroidTheme {
                Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
                    FetchKitScreen(
                        version = remember { FetchKit.version() },
                        modifier = Modifier.padding(innerPadding)
                    )
                }
            }
        }
    }
}

@Composable
private fun FetchKitScreen(version: String, modifier: Modifier = Modifier) {
    Text(
        text = "FetchKit\nNative core: $version",
        modifier = modifier
    )
}

@Preview(showBackground = true)
@Composable
private fun FetchKitPreview() {
    FetchKitAndroidTheme {
        FetchKitScreen(version = "preview")
    }
}
