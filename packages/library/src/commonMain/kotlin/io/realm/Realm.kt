/*
 * Copyright 2020 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package io.realm

import io.realm.internal.SuspendableWriter
import io.realm.internal.util.runBlocking
import io.realm.interop.NativePointer
import io.realm.interop.RealmInterop
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

// TODO API-PUBLIC Document platform specific internals (RealmInitilizer, etc.)
class Realm private constructor(configuration: RealmConfiguration, dbPointer: NativePointer) :
    BaseRealm(configuration, dbPointer) {

    private val writer: SuspendableWriter = SuspendableWriter(configuration, configuration.writeDispatcher())
    private val realmPointerMutex = Mutex()

    companion object {
        /**
         * Default name for Realm files unless overridden by [RealmConfiguration.Builder.name].
         */
        public const val DEFAULT_FILE_NAME = "default.realm"

        /**
         * Default tag used by log entries
         */
        public const val DEFAULT_LOG_TAG = "REALM"

        fun open(realmConfiguration: RealmConfiguration): Realm {
            // TODO API-INTERNAL
            //  IN Android use lazy property delegation init to load the shared library use the
            //  function call (lazy init to do any preprocessing before starting Realm eg: log level etc)
            //  or implement an init method which is a No-OP in iOS but in Android it load the shared library
            val realm = Realm(realmConfiguration, RealmInterop.realm_open(realmConfiguration.nativeConfig))
            realm.log.info("Opened Realm: ${realmConfiguration.path}")
            return realm
        }
    }

    /**
     * Open a Realm instance. This instance grants access to an underlying Realm file defined by
     * the provided [RealmConfiguration].
     *
     * FIXME Figure out how to describe the constructor better
     * FIXME Once the implementation of this class moves to the frozen architecture
     *  this constructor should be the primary way to open Realms (as you only need
     *  to do it once pr. app).
     */
    public constructor(configuration: RealmConfiguration) :
        this(configuration, RealmInterop.realm_open(configuration.nativeConfig))

    /**
     * Modify the underlying Realm file in a suspendable transaction on the default Realm
     * dispatcher.
     *
     * NOTE: Objects and results retrieved before a write are no longer valid. This restriction
     * will be lifted when the frozen architecture is fully in place.
     *
     * The write transaction always represent the latest version of data in the Realm file, even if
     * the calling Realm not yet represent this.
     *
     * Write transactions automatically commit any changes made when the closure returns unless
     * [MutableRealm.cancelWrite] was called.
     *
     * @param block function that should be run within the context of a write transaction.
     * FIXME Isn't this impossible to achieve in a way where we can guarantee that we freeze all
     *  objects leaving the transaction? It currently works for RealmObjects, and can maybe be done
     *  for collections and RealmResults, but what it is bundled in other containers, etc. Should
     *  we define an interface of _returnable_ objects that follows some convention?
     * @return any value returned from the provided write block as frozen/immutable objects.
     * @see [RealmConfiguration.writeDispatcher]
     */
    // TODO Would we be able to offer a per write error handler by adding a CoroutineExceptinoHandler
    internal suspend fun <R> write(block: MutableRealm.() -> R): R {
        @Suppress("TooGenericExceptionCaught") // FIXME https://github.com/realm/realm-kotlin/issues/70
        try {
            val (nativePointer, versionId, result) = this.writer.write(block)
            // Update the user facing Realm before returning the result.
            // That way, querying the Realm right after the `write` completes will return
            // the written data. Otherwise, we would have to wait for the Notifier thread
            // to detect it and update the user Realm.
            updateRealmPointer(nativePointer, versionId)
            // FIXME What if the result is of a different version than the realm (some other
            //  write transaction finished before)
            return result
        } catch (e: Exception) {
            throw e
        }
    }

    private suspend fun updateRealmPointer(newRealm: NativePointer, newVersion: VersionId) {
        realmPointerMutex.withLock {
            log.debug("$version -> $newVersion")
            if (newVersion >= version) {
                // FIXME Currently we need this to be a live realm to be able to continue doing
                //  writeBlocking transactions.
                dbPointer = RealmInterop.realm_thaw(newRealm)
                version = newVersion
            }
        }
    }

    /**
     * Modify the underlying Realm file by creating a write transaction on the current thread. Write
     * transactions automatically commit any changes made when the closure returns unless
     * [MutableRealm.cancelWrite] was called.
     *
     * The write transaction always represent the latest version of data in the Realm file, even if the calling
     * Realm not yet represent this.
     *
     * @param block function that should be run within the context of a write transaction.
     * @return any value returned from the provided write block.
     */
    @Suppress("TooGenericExceptionCaught")
    fun <R> writeBlocking(block: MutableRealm.() -> R): R {
        // While not efficiently to open a new Realm just for a write, it makes it a lot
        // easier to control the API surface between Realm and MutableRealm
        val writerRealm = MutableRealm(configuration, dbPointer)
        try {
            writerRealm.beginTransaction()
            val returnValue: R = block(writerRealm)
            if (writerRealm.commitWrite && !isClosed()) {
                writerRealm.commitTransaction()
            }
            return returnValue
        } catch (e: Exception) {
            // Only cancel writes for exceptions. For errors assume that something has gone
            // horribly wrong and the process is exiting. And canceling the write might just
            // hide the true underlying error.
            try {
                writerRealm.cancelWrite()
            } catch (cancelException: Throwable) {
                // Swallow any exception from `cancelWrite` as the primary error is more important.
                log.error(e)
            }
            throw e
        }
    }

    /**
     * Close this Realm and all underlying resources. Accessing any methods or Realm Objects after this
     * method has been called will then an [IllegalStateException].
     */
    public override fun close() {
        super.close()
        // TODO There is currently nothing that tears down the dispatcher
        runBlocking() {
            writer.close()
        }
    }
}
