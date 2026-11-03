package com.luofelix.fetchkit.sizehost

import com.luofelix.fetchkit.DownloadEngine
import com.luofelix.fetchkit.DownloadRequest
import com.luofelix.fetchkit.TaskSnapshot
import com.luofelix.fetchkit.TaskState
import com.luofelix.fetchkit.coroutines.observeSnapshots
import com.luofelix.fetchkit.coroutines.withResourceLease
import com.luofelix.fetchkit.coroutines.withSubscription
import com.luofelix.fetchkit.coroutines.awaitValue
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout

internal object CoroutineCheck {
    @JvmStatic
    fun verify(engine: DownloadEngine, request: DownloadRequest, snapshot: TaskSnapshot) = runBlocking {
        withTimeout(15000) {
            engine.withSubscription(request, start = false) { subscription ->
                val observation = subscription.observeSnapshots(this)
                try {
                    val observed = observation.snapshots.first { it != null }
                    check(observed?.state == TaskState.Completed && observed.persistentId == snapshot.persistentId)
                } finally {
                    observation.close()
                }
            }
            engine.withResourceLease(snapshot.persistentId) { lease ->
                check(lease.read(0, 64).awaitValue().size == 64)
            }
        }
    }
}
