package com.izzy2lost.neonsaturn

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts.OpenDocument
import androidx.activity.result.contract.ActivityResultContracts.OpenDocumentTree
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.isVisible
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.progressindicator.LinearProgressIndicator
import java.io.File
import java.util.Locale
import kotlin.concurrent.thread

class LauncherActivity : AppCompatActivity() {
    private lateinit var store: BootstrapStore
    private lateinit var importer: ContentImporter
    private lateinit var paths: NeonSaturnPaths
    private lateinit var libraryAdapter: GameLibraryAdapter

    private lateinit var wizardContainer: android.view.View
    private lateinit var libraryContainer: android.view.View
    private lateinit var iplValueText: TextView
    private lateinit var cdbValueText: TextView
    private lateinit var gamesFolderValueText: TextView
    private lateinit var libraryFolderValueText: TextView
    private lateinit var emptyLibraryText: TextView
    private lateinit var libraryProgressIndicator: LinearProgressIndicator
    private lateinit var libraryRecyclerView: RecyclerView

    private var currentGamesFolderUri: String? = null
    private var currentLibraryEntries: List<GameLibraryEntry> = emptyList()
    private var libraryScanGeneration = 0
    private var launchInProgress = false

    private val importIplLauncher = registerForActivityResult(OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        importDocument(uri, ContentBucket.IPL) { file -> store.saveIpl(file.absolutePath) }
    }

    private val importCdbLauncher = registerForActivityResult(OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        importDocument(uri, ContentBucket.CDB) { file -> store.saveCdb(file.absolutePath) }
    }

    private val gamesFolderLauncher = registerForActivityResult(OpenDocumentTree()) { uri ->
        uri ?: return@registerForActivityResult
        runCatching {
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            store.saveGamesFolderUri(uri.toString())
        }.onFailure { error ->
            Toast.makeText(
                this,
                "Unable to use that games folder: ${error.message ?: error::class.java.simpleName}",
                Toast.LENGTH_LONG
            ).show()
        }.onSuccess {
            refreshUi(forceRescan = true)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_launcher)

        store = BootstrapStore(this)
        paths = applicationContext.neonSaturnPaths()
        migrateLegacyStorageIfNeeded()
        paths.ensureAll()
        importer = ContentImporter(this, paths)

        libraryAdapter = GameLibraryAdapter(::launchGame)

        wizardContainer = findViewById(R.id.wizardContainer)
        libraryContainer = findViewById(R.id.libraryContainer)
        iplValueText = findViewById(R.id.iplValueText)
        cdbValueText = findViewById(R.id.cdbValueText)
        gamesFolderValueText = findViewById(R.id.gamesFolderValueText)
        libraryFolderValueText = findViewById(R.id.libraryFolderValueText)
        emptyLibraryText = findViewById(R.id.emptyLibraryText)
        libraryProgressIndicator = findViewById(R.id.libraryProgressIndicator)
        libraryRecyclerView = findViewById(R.id.libraryRecyclerView)

        libraryRecyclerView.layoutManager = LinearLayoutManager(this)
        libraryRecyclerView.adapter = libraryAdapter

        findViewById<TextView>(R.id.storageHintText).text =
            getString(R.string.storage_hint, paths.root.absolutePath)

        findViewById<MaterialButton>(R.id.importIplButton).setOnClickListener {
            importIplLauncher.launch(arrayOf("*/*"))
        }
        findViewById<MaterialButton>(R.id.importCdbButton).setOnClickListener {
            importCdbLauncher.launch(arrayOf("*/*"))
        }
        findViewById<MaterialButton>(R.id.chooseGamesFolderButton).setOnClickListener {
            gamesFolderLauncher.launch(null)
        }
        findViewById<MaterialButton>(R.id.libraryChangeGamesFolderButton).setOnClickListener {
            gamesFolderLauncher.launch(null)
        }
        findViewById<MaterialButton>(R.id.libraryImportIplButton).setOnClickListener {
            importIplLauncher.launch(arrayOf("*/*"))
        }
        findViewById<MaterialButton>(R.id.libraryImportCdbButton).setOnClickListener {
            importCdbLauncher.launch(arrayOf("*/*"))
        }
        findViewById<MaterialButton>(R.id.libraryRefreshButton).setOnClickListener {
            refreshUi(forceRescan = true)
        }

        refreshUi(forceRescan = true)
    }

    override fun onResume() {
        super.onResume()
        refreshUi(forceRescan = false)
    }

    private fun importDocument(
        uri: Uri,
        bucket: ContentBucket,
        onStored: (File) -> Unit
    ) {
        runCatching {
            importer.importDocument(uri, bucket)
        }.onSuccess { file ->
            onStored(file)
            refreshUi(forceRescan = false)
        }.onFailure { error ->
            Toast.makeText(
                this,
                "Import failed: ${error.message ?: error::class.java.simpleName}",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun refreshUi(forceRescan: Boolean) {
        val selection = resolveSelection()
        val gamesFolderUri = selection.gamesFolderUri
        val setupComplete = selection.iplPath != null && gamesFolderUri != null

        iplValueText.text = selection.iplPath?.let(::fileLabel) ?: getString(R.string.not_set)
        cdbValueText.text = selection.cdbPath?.let(::fileLabel) ?: getString(R.string.not_set_optional)
        val folderLabel = gamesFolderUri?.let(::folderLabel) ?: getString(R.string.not_set)
        gamesFolderValueText.text = folderLabel
        libraryFolderValueText.text = folderLabel

        wizardContainer.isVisible = !setupComplete
        libraryContainer.isVisible = setupComplete

        if (!setupComplete) {
            currentGamesFolderUri = null
            currentLibraryEntries = emptyList()
            libraryAdapter.submitList(emptyList())
            emptyLibraryText.isVisible = false
            setLibraryLoading(false)
            return
        }

        if (forceRescan || currentGamesFolderUri != gamesFolderUri || currentLibraryEntries.isEmpty()) {
            refreshLibrary(requireNotNull(gamesFolderUri))
        }
    }

    private fun refreshLibrary(gamesFolderUri: String) {
        val scanGeneration = ++libraryScanGeneration
        currentGamesFolderUri = gamesFolderUri
        setLibraryLoading(true)
        emptyLibraryText.isVisible = false

        thread(name = "game-library-scan") {
            val result = runCatching {
                applicationContext.scanGameLibrary(Uri.parse(gamesFolderUri))
            }

            runOnUiThread {
                if (isFinishing || scanGeneration != libraryScanGeneration) {
                    return@runOnUiThread
                }

                setLibraryLoading(false)

                result.onSuccess { entries ->
                    currentLibraryEntries = entries
                    libraryAdapter.submitList(entries)
                    emptyLibraryText.isVisible = entries.isEmpty()
                    emptyLibraryText.text = getString(R.string.library_empty)
                }.onFailure { error ->
                    currentLibraryEntries = emptyList()
                    libraryAdapter.submitList(emptyList())
                    emptyLibraryText.isVisible = true
                    emptyLibraryText.text = getString(
                        R.string.library_scan_error_inline,
                        error.message ?: error::class.java.simpleName
                    )
                }
            }
        }
    }

    private fun launchGame(entry: GameLibraryEntry) {
        if (launchInProgress) {
            return
        }

        val selection = resolveSelection()
        val validationError = validateCoreSelection(selection)
        if (validationError != null) {
            Toast.makeText(this, validationError, Toast.LENGTH_LONG).show()
            return
        }

        launchInProgress = true
        setLibraryLoading(true)

        thread(name = "game-launch-prep") {
            val result = runCatching {
                val stagedDisc = applicationContext.stageGameForLaunch(entry, paths)
                val gameControllerDbPath = applicationContext.installBundledGameControllerDb(paths).absolutePath
                val launchSelection = StoredLaunchSelection(
                    iplPath = selection.iplPath,
                    cdbPath = selection.cdbPath,
                    discPath = stagedDisc.absolutePath,
                    gamesFolderUri = selection.gamesFolderUri
                )

                validateSelection(launchSelection)?.let { error ->
                    throw IllegalStateException(error)
                }

                launchSelection to gameControllerDbPath
            }

            runOnUiThread {
                launchInProgress = false
                setLibraryLoading(false)
                if (isFinishing) {
                    return@runOnUiThread
                }

                result.onSuccess { (launchSelection, gameControllerDbPath) ->
                    startActivity(EmulatorActivity.createIntent(this, launchSelection, paths, gameControllerDbPath))
                }.onFailure { error ->
                    Toast.makeText(
                        this,
                        "Unable to launch ${entry.title}: ${error.message ?: error::class.java.simpleName}",
                        Toast.LENGTH_LONG
                    ).show()
                }
            }
        }
    }

    private fun setLibraryLoading(loading: Boolean) {
        libraryProgressIndicator.isVisible = loading
        libraryRecyclerView.alpha = if (loading) 0.5f else 1.0f
        libraryRecyclerView.isEnabled = !loading
    }

    private fun fileLabel(path: String): String = File(path).name

    private fun folderLabel(uriString: String): String {
        val uri = Uri.parse(uriString)
        return androidx.documentfile.provider.DocumentFile.fromTreeUri(this, uri)?.name
            ?: uri.lastPathSegment
            ?: getString(R.string.games_folder_unknown)
    }

    private fun migrateLegacyStorageIfNeeded() {
        val legacyPaths = applicationContext.legacyNeonSaturnPaths()
        if (legacyPaths.root.absolutePath == paths.root.absolutePath || !legacyPaths.root.exists()) {
            return
        }

        runCatching {
            paths.ensureAll()
            legacyPaths.root.walkTopDown().forEach { source ->
                val relative = runCatching { source.relativeTo(legacyPaths.root) }.getOrNull() ?: return@forEach
                val target = File(paths.root, relative.path)
                if (source.isDirectory) {
                    if (!target.exists()) {
                        target.mkdirs()
                    }
                } else if (source.isFile && !target.exists()) {
                    target.parentFile?.mkdirs()
                    source.copyTo(target)
                }
            }
            store.migratePaths(legacyPaths.root, paths.root)
        }.onFailure { error ->
            Toast.makeText(
                this,
                "Storage migration warning: ${error.message ?: error::class.java.simpleName}",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun resolveSelection(): StoredLaunchSelection {
        val stored = store.load()
        val resolvedIplPath = resolveStoredOrFirst(stored.iplPath, paths.iplDir, ROM_EXTENSIONS)
        val resolvedCdbPath = resolveStoredOrFirst(stored.cdbPath, paths.cdbDir, ROM_EXTENSIONS)

        if (resolvedIplPath != stored.iplPath || resolvedCdbPath != stored.cdbPath) {
            store.save(
                stored.copy(
                    iplPath = resolvedIplPath,
                    cdbPath = resolvedCdbPath
                )
            )
        }

        return stored.copy(
            iplPath = resolvedIplPath,
            cdbPath = resolvedCdbPath,
            discPath = null
        )
    }

    private fun resolveStoredOrFirst(
        storedPath: String?,
        directory: File,
        allowedExtensions: Set<String>
    ): String? {
        if (storedPath != null && File(storedPath).isFile) {
            return storedPath
        }

        return directory.listFiles()
            ?.asSequence()
            ?.filter(File::isFile)
            ?.filter { file -> file.extension.lowercase(Locale.ROOT) in allowedExtensions }
            ?.sortedBy { file -> file.name.lowercase(Locale.ROOT) }
            ?.firstOrNull()
            ?.absolutePath
    }

    private fun validateCoreSelection(selection: StoredLaunchSelection): String? {
        val iplPath = selection.iplPath ?: return getString(R.string.validation_missing_bios)
        if (!File(iplPath).isFile) {
            return getString(R.string.validation_missing_selected_bios, paths.iplDir.absolutePath)
        }

        val cdbPath = selection.cdbPath
        if (cdbPath != null && !File(cdbPath).isFile) {
            store.saveCdb(null)
            return getString(R.string.validation_missing_selected_cdb, paths.cdbDir.absolutePath)
        }

        return null
    }

    private fun validateSelection(selection: StoredLaunchSelection): String? {
        validateCoreSelection(selection)?.let { return it }

        val discPath = selection.discPath ?: return getString(R.string.validation_missing_disc)
        val discFile = File(discPath)
        if (!discFile.isFile) {
            return getString(R.string.validation_missing_selected_disc)
        }

        if (isLikelyMultiFileDescriptor(discFile) && !hasLikelyCompanionFiles(discFile)) {
            return getString(R.string.validation_missing_companion_disc_files)
        }

        return null
    }

    private fun isLikelyMultiFileDescriptor(file: File): Boolean =
        file.extension.lowercase(Locale.ROOT) in MULTI_FILE_DISC_DESCRIPTOR_EXTENSIONS

    private fun hasLikelyCompanionFiles(file: File): Boolean {
        val companionExtensions = when (file.extension.lowercase(Locale.ROOT)) {
            "cue" -> setOf("bin", "wav", "mp3", "flac", "ogg")
            "ccd" -> setOf("img", "sub")
            "mds" -> setOf("mdf")
            else -> emptySet()
        }
        if (companionExtensions.isEmpty()) {
            return true
        }

        val parent = file.parentFile ?: return false
        return parent.listFiles()
            ?.any { candidate ->
                candidate.isFile &&
                    candidate != file &&
                    candidate.extension.lowercase(Locale.ROOT) in companionExtensions
            } == true
    }

    private companion object {
        val ROM_EXTENSIONS = setOf("bin", "rom")
        val MULTI_FILE_DISC_DESCRIPTOR_EXTENSIONS = setOf("cue", "ccd", "mds")
    }
}
