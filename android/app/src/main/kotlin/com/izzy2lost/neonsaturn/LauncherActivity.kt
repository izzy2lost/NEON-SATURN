package com.izzy2lost.neonsaturn

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.widget.ImageButton
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts.OpenDocument
import androidx.activity.result.contract.ActivityResultContracts.OpenDocumentTree
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.core.view.updatePaddingRelative
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.LinearSnapHelper
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.chip.ChipGroup
import com.google.android.material.dialog.MaterialAlertDialogBuilder
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
    private lateinit var librarySettingsButton: ImageButton
    private lateinit var iplValueText: TextView
    private lateinit var cdbValueText: TextView
    private lateinit var gamesFolderValueText: TextView
    private lateinit var emptyLibraryText: TextView
    private lateinit var libraryProgressIndicator: LinearProgressIndicator
    private lateinit var libraryRecyclerView: RecyclerView
    private lateinit var viewModeChipGroup: ChipGroup

    private var currentGamesFolderUri: String? = null
    private var currentLibraryEntries: List<GameLibraryEntry> = emptyList()
    private var libraryScanGeneration = 0
    private var librarySettingsDialog: AlertDialog? = null
    private var launchInProgress = false

    private var currentViewMode = BootstrapStore.VIEW_MODE_LIST
    private var coverFlowAdapter: CoverFlowAdapter? = null
    private val coverFlowTransformer = CoverFlowScrollTransformer()
    private var snapHelper: LinearSnapHelper? = null

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
        WindowCompat.setDecorFitsSystemWindows(window, false)
        setContentView(R.layout.activity_launcher)

        store = BootstrapStore(this)
        paths = applicationContext.neonSaturnPaths()
        migrateLegacyStorageIfNeeded()
        paths.ensureAll()
        importer = ContentImporter(this, paths)

        libraryAdapter = GameLibraryAdapter(::launchGame)

        wizardContainer = findViewById(R.id.wizardContainer)
        libraryContainer = findViewById(R.id.libraryContainer)
        librarySettingsButton = findViewById(R.id.librarySettingsButton)
        iplValueText = findViewById(R.id.iplValueText)
        cdbValueText = findViewById(R.id.cdbValueText)
        gamesFolderValueText = findViewById(R.id.gamesFolderValueText)
        emptyLibraryText = findViewById(R.id.emptyLibraryText)
        libraryProgressIndicator = findViewById(R.id.libraryProgressIndicator)
        libraryRecyclerView = findViewById(R.id.libraryRecyclerView)
        viewModeChipGroup = findViewById(R.id.viewModeChipGroup)

        currentViewMode = store.loadLibraryViewMode()
        applyChipSelection(currentViewMode)
        applyViewMode(currentViewMode)

        viewModeChipGroup.setOnCheckedStateChangeListener { _, checkedIds ->
            val mode = when (checkedIds.firstOrNull()) {
                R.id.chipViewNACovers -> BootstrapStore.VIEW_MODE_NA_COVERS
                R.id.chipViewJapanCovers -> BootstrapStore.VIEW_MODE_JAPAN_COVERS
                else -> BootstrapStore.VIEW_MODE_LIST
            }
            if (mode != currentViewMode) {
                currentViewMode = mode
                store.saveLibraryViewMode(mode)
                applyViewMode(mode)
            }
        }

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
        librarySettingsButton.setOnClickListener {
            showLibrarySettingsDialog()
        }

        // Adjust header padding dynamically for status bar height and camera cutout.
        val headerContainer = findViewById<android.view.View>(R.id.headerContainer)
        val headerBasePaddingStart = headerContainer.paddingStart
        val headerBasePaddingTop = headerContainer.paddingTop
        val headerBasePaddingEnd = headerContainer.paddingEnd
        ViewCompat.setOnApplyWindowInsetsListener(headerContainer) { v, insets ->
            val systemBars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            v.updatePaddingRelative(
                start = headerBasePaddingStart + systemBars.left,
                top = headerBasePaddingTop + systemBars.top,
                end = headerBasePaddingEnd + systemBars.right
            )
            insets
        }

        // Adjust bottom padding of scrollable containers for the navigation bar.
        val wizardBasePaddingBottom = wizardContainer.paddingBottom
        val libraryBasePaddingBottom = libraryContainer.paddingBottom
        ViewCompat.setOnApplyWindowInsetsListener(wizardContainer) { v, insets ->
            val bottom = insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom
            v.updatePadding(bottom = wizardBasePaddingBottom + bottom)
            insets
        }
        ViewCompat.setOnApplyWindowInsetsListener(libraryContainer) { v, insets ->
            val bottom = insets.getInsets(WindowInsetsCompat.Type.navigationBars()).bottom
            v.updatePadding(bottom = libraryBasePaddingBottom + bottom)
            insets
        }

        refreshUi(forceRescan = true)
    }

    override fun onResume() {
        super.onResume()
        refreshUi(forceRescan = false)
    }

    override fun onPause() {
        librarySettingsDialog?.dismiss()
        super.onPause()
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

        wizardContainer.isVisible = !setupComplete
        libraryContainer.isVisible = setupComplete
        librarySettingsButton.isVisible = setupComplete

        if (!setupComplete) {
            librarySettingsDialog?.dismiss()
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

    private fun showLibrarySettingsDialog() {
        if (!libraryContainer.isVisible || isFinishing || isDestroyed || librarySettingsDialog?.isShowing == true) {
            return
        }

        val content = LayoutInflater.from(this).inflate(R.layout.dialog_library_settings, null, false)

        // Aspect ratio chips
        val aspectGroup = content.findViewById<com.google.android.material.chip.ChipGroup>(R.id.aspectRatioChipGroup)
        when (store.loadAspectRatio()) {
            BootstrapStore.ASPECT_16_9 -> aspectGroup.check(R.id.aspectChip16x9)
            BootstrapStore.ASPECT_STRETCH -> aspectGroup.check(R.id.aspectChipStretch)
            else -> aspectGroup.check(R.id.aspectChip4x3)
        }
        aspectGroup.setOnCheckedStateChangeListener { _, checkedIds ->
            val value = when (checkedIds.firstOrNull()) {
                R.id.aspectChip16x9 -> BootstrapStore.ASPECT_16_9
                R.id.aspectChipStretch -> BootstrapStore.ASPECT_STRETCH
                else -> BootstrapStore.ASPECT_4_3
            }
            store.saveAspectRatio(value)
        }

        // Texture filter chips
        val filterGroup = content.findViewById<com.google.android.material.chip.ChipGroup>(R.id.textureFilterChipGroup)
        when (store.loadTextureFilter()) {
            BootstrapStore.FILTER_BILINEAR -> filterGroup.check(R.id.filterChipSmooth)
            else -> filterGroup.check(R.id.filterChipSharp)
        }
        filterGroup.setOnCheckedStateChangeListener { _, checkedIds ->
            val value = when (checkedIds.firstOrNull()) {
                R.id.filterChipSmooth -> BootstrapStore.FILTER_BILINEAR
                else -> BootstrapStore.FILTER_NEAREST
            }
            store.saveTextureFilter(value)
        }

        val touchControlsSwitch =
            content.findViewById<com.google.android.material.materialswitch.MaterialSwitch>(R.id.touchControlsSwitch)
        touchControlsSwitch.isChecked = store.loadTouchControlsEnabled()
        touchControlsSwitch.setOnCheckedChangeListener { _, isChecked ->
            store.saveTouchControlsEnabled(isChecked)
        }

        val dialog = MaterialAlertDialogBuilder(this, R.style.ThemeOverlay_NeonSaturn_LibrarySettingsDialog)
            .setView(content)
            .setCancelable(true)
            .create()

        content.findViewById<MaterialButton>(R.id.downloadCoversButton).setOnClickListener {
            val mode = currentViewMode
            if (mode == BootstrapStore.VIEW_MODE_LIST) {
                Toast.makeText(this, "Switch to NA Boxes or Japan view first", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            dialog.dismiss()
            CoverArtManager.prefetchAll(this, currentLibraryEntries, mode, paths)
            Toast.makeText(this, getString(R.string.covers_downloading), Toast.LENGTH_LONG).show()
        }

        content.findViewById<MaterialButton>(R.id.clearCoverCacheButton).setOnClickListener {
            dialog.dismiss()
            CoverArtManager.clearCache(paths)
            Toast.makeText(this, getString(R.string.covers_cache_cleared), Toast.LENGTH_SHORT).show()
        }

        content.findViewById<MaterialButton>(R.id.refreshLibraryButton).setOnClickListener {
            dialog.dismiss()
            refreshUi(forceRescan = true)
        }
        content.findViewById<MaterialButton>(R.id.redoSetupButton).setOnClickListener {
            dialog.dismiss()
            store.clearSetup()
            refreshUi(forceRescan = false)
        }
        content.findViewById<MaterialButton>(R.id.editTouchControlsButton).setOnClickListener {
            dialog.dismiss()
            startActivity(Intent(this, TouchControlsEditorActivity::class.java))
        }

        dialog.setOnDismissListener {
            librarySettingsDialog = null
        }

        librarySettingsDialog = dialog
        dialog.show()
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
                    if (currentViewMode == BootstrapStore.VIEW_MODE_LIST) {
                        libraryAdapter.submitList(entries)
                    } else {
                        coverFlowAdapter?.submitList(entries)
                        libraryRecyclerView.post {
                            coverFlowTransformer.applyTransforms(libraryRecyclerView)
                        }
                    }
                    emptyLibraryText.isVisible = entries.isEmpty()
                    emptyLibraryText.text = getString(R.string.library_empty)
                }.onFailure { error ->
                    currentLibraryEntries = emptyList()
                    libraryAdapter.submitList(emptyList())
                    coverFlowAdapter?.submitList(emptyList())
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
                    startActivity(EmulatorActivity.createIntent(
                        this, launchSelection, paths, gameControllerDbPath,
                        aspectRatio = store.loadAspectRatio(),
                        textureFilter = store.loadTextureFilter()
                    ))
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

    private fun applyChipSelection(mode: Int) {
        viewModeChipGroup.check(
            when (mode) {
                BootstrapStore.VIEW_MODE_NA_COVERS -> R.id.chipViewNACovers
                BootstrapStore.VIEW_MODE_JAPAN_COVERS -> R.id.chipViewJapanCovers
                else -> R.id.chipViewList
            }
        )
    }

    private fun applyViewMode(mode: Int) {
        // Detach coverflow helpers from previous mode
        snapHelper?.attachToRecyclerView(null)
        snapHelper = null
        libraryRecyclerView.removeOnScrollListener(coverFlowTransformer)

        val density = resources.displayMetrics.density
        val px24 = (24 * density).toInt()

        if (mode == BootstrapStore.VIEW_MODE_LIST) {
            libraryRecyclerView.layoutManager = LinearLayoutManager(this)
            libraryRecyclerView.adapter = libraryAdapter
            libraryRecyclerView.setPadding(px24, 0, px24, px24)
            libraryRecyclerView.clipToPadding = false
            libraryAdapter.submitList(currentLibraryEntries)
        } else {
            val il = CoverArtManager.imageLoader(this, paths)
            val cfa = CoverFlowAdapter(::launchGame, mode, il)
            coverFlowAdapter = cfa

            libraryRecyclerView.layoutManager =
                LinearLayoutManager(this, LinearLayoutManager.HORIZONTAL, false)
            libraryRecyclerView.adapter = cfa
            libraryRecyclerView.clipToPadding = false

            // Center first/last item horizontally — compute padding once width is known
            libraryRecyclerView.post {
                val itemWidthPx = (160 * density).toInt()
                val hPad = ((libraryRecyclerView.width - itemWidthPx) / 2).coerceAtLeast(0)
                libraryRecyclerView.setPadding(hPad, 0, hPad, 0)
            }

            val snap = LinearSnapHelper()
            snap.attachToRecyclerView(libraryRecyclerView)
            snapHelper = snap

            libraryRecyclerView.addOnScrollListener(coverFlowTransformer)

            cfa.submitList(currentLibraryEntries)

            libraryRecyclerView.post {
                coverFlowTransformer.applyTransforms(libraryRecyclerView)
            }
        }
    }

    private companion object {
        val ROM_EXTENSIONS = setOf("bin", "rom")
        val MULTI_FILE_DISC_DESCRIPTOR_EXTENSIONS = setOf("cue", "ccd", "mds")
    }
}
