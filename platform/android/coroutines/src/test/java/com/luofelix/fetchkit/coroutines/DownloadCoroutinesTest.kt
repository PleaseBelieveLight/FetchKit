package com.luofelix.fetchkit.coroutines

import java.util.concurrent.CompletableFuture
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class DownloadCoroutinesTest {
    @Test fun cancellationDuringCreationReleasesLateHandle() = runBlocking {
        val source = CompletableFuture<Int>()
        val released = mutableListOf<Int>()
        val job = launch(start = CoroutineStart.UNDISPATCHED) {
            withOwned({ source }, { handle ->
                released.add(handle)
                CompletableFuture.completedFuture(Unit)
            }) { error("Cancelled scope must not receive the handle") }
        }
        job.cancel()
        assertFalse(source.isCancelled)
        source.complete(42)
        job.join()
        assertEquals(listOf(42), released)
    }

    @Test fun cancellingValueWaitLeavesFutureRunning() = runBlocking {
        val source = CompletableFuture<Int>()
        val job = launch(start = CoroutineStart.UNDISPATCHED) { source.awaitValue() }
        job.cancel()
        job.join()
        assertFalse(source.isDone)
    }
}
