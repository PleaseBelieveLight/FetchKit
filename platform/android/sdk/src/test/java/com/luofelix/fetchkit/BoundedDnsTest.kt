package com.luofelix.fetchkit

import org.junit.Assert.*
import org.junit.Test
import java.net.InetAddress
import java.net.SocketTimeoutException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class BoundedDnsTest {
    @Test fun cancellationDoesNotWaitForUncooperativeResolver() {
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val cancelled = AtomicBoolean()
        val finished = CountDownLatch(1)
        val worker = Thread {
            try {
                BoundedDns.lookup("synthetic", 5000, { cancelled.get() }) {
                    entered.countDown()
                    while (release.count != 0L) try { release.await() } catch (_: InterruptedException) {}
                    listOf(InetAddress.getLoopbackAddress())
                }
                fail("Cancellation accepted a result")
            } catch (_: java.io.InterruptedIOException) { finished.countDown() }
        }
        worker.start()
        try {
            assertTrue(entered.await(2, TimeUnit.SECONDS)); cancelled.set(true)
            assertTrue(finished.await(1, TimeUnit.SECONDS))
        } finally { release.countDown(); worker.join(2000) }
    }
    @Test(expected = SocketTimeoutException::class) fun resolverHasDeadline() {
        val release = CountDownLatch(1)
        try { BoundedDns.lookup("synthetic", 25, { false }) {
            while (release.count != 0L) try { release.await() } catch (_: InterruptedException) {}
            listOf(InetAddress.getLoopbackAddress())
        } } finally { release.countDown() }
    }
}
