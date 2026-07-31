package com.izzy2lost.neonsaturn

import android.content.Intent
import android.content.res.Configuration
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
import androidx.core.view.WindowInsetsControllerCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.core.view.updatePaddingRelative
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.PagerSnapHelper
import androidx.recyclerview.widget.RecyclerView
import androidx.recyclerview.widget.SnapHelper
import com.google.android.material.button.MaterialButton
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
    private lateinit var viewModeToggleButton: ImageButton
    private lateinit var starfieldView: StarfieldView

    private var currentGamesFolderUri: String? = null
    private var currentLibraryEntries: List<GameLibraryEntry> = emptyList()
    private var libraryScanGeneration = 0
    private var librarySettingsDialog: AlertDialog? = null
    private var launchInProgress = false

    private var currentViewMode = BootstrapStore.VIEW_MODE_LIST
    private var coverFlowAdapter: CoverFlowAdapter? = null
    private val coverFlowTransformer = CoverFlowScrollTransformer()
    private var snapHelper: SnapHelper? = null

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
        viewModeToggleButton = findViewById(R.id.viewModeToggleButton)
        starfieldView = findViewById(R.id.starfieldView)

        currentViewMode = store.loadLibraryViewMode()
        applyToggleIcon(currentViewMode)
        applyViewMode(currentViewMode)

        viewModeToggleButton.setOnClickListener {
            val next = when (currentViewMode) {
                BootstrapStore.VIEW_MODE_LIST       -> BootstrapStore.VIEW_MODE_NA_COVERS
                BootstrapStore.VIEW_MODE_NA_COVERS  -> BootstrapStore.VIEW_MODE_JAPAN_COVERS
                else                               -> BootstrapStore.VIEW_MODE_LIST
            }
            currentViewMode = next
            store.saveLibraryViewMode(next)
            applyToggleIcon(next)
            applyViewMode(next)
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
        starfieldView.resume()
        refreshUi(forceRescan = false)
    }

    override fun onPause() {
        starfieldView.pause()
        librarySettingsDialog?.dismiss()
        super.onPause()
    }

    /**
     * The launcher theme leaves the system bars transparent so whatever is behind them
     * shows through. That is the always-dark starfield once the library is up, and the
     * theme's own background during the setup wizard - so the bar icons follow whichever
     * is actually on screen.
     */
    private fun applySystemBarAppearance(overStarfield: Boolean) {
        val nightMode = resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK ==
            Configuration.UI_MODE_NIGHT_YES
        val lightBars = !overStarfield && !nightMode
        WindowInsetsControllerCompat(window, window.decorView).apply {
            isAppearanceLightStatusBars = lightBars
            isAppearanceLightNavigationBars = lightBars
        }
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
        viewModeToggleButton.isVisible = setupComplete
        // The setup wizard keeps the plain background; the starfield is the library's.
        starfieldView.isVisible = setupComplete
        applySystemBarAppearance(overStarfield = setupComplete)

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

        // Theme chips
        val themeGroup = content.findViewById<com.google.android.material.chip.ChipGroup>(R.id.themeChipGroup)
        when (store.loadThemeMode()) {
            BootstrapStore.THEME_SYSTEM -> themeGroup.check(R.id.themeChipSystem)
            BootstrapStore.THEME_LIGHT -> themeGroup.check(R.id.themeChipLight)
            else -> themeGroup.check(R.id.themeChipDark)
        }
        themeGroup.setOnCheckedStateChangeListener { _, checkedIds ->
            val value = when (checkedIds.firstOrNull()) {
                R.id.themeChipSystem -> BootstrapStore.THEME_SYSTEM
                R.id.themeChipLight -> BootstrapStore.THEME_LIGHT
                else -> BootstrapStore.THEME_DARK
            }
            if (value != store.loadThemeMode()) {
                store.saveThemeMode(value)
                // Switching modes recreates the activity, so close the dialog first rather
                // than leaving its window attached to the outgoing one.
                librarySettingsDialog?.dismiss()
                androidx.appcompat.app.AppCompatDelegate.setDefaultNightMode(
                    BootstrapStore.nightModeFor(value)
                )
            }
        }

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

        // Upscaling filter chips
        val upscaleGroup = content.findViewById<com.google.android.material.chip.ChipGroup>(R.id.upscaleFilterChipGroup)
        when (store.loadUpscaleFilter()) {
            BootstrapStore.UPSCALE_XBRZ_6X -> upscaleGroup.check(R.id.upscaleChipXbrz)
            else -> upscaleGroup.check(R.id.upscaleChipOff)
        }
        upscaleGroup.setOnCheckedStateChangeListener { _, checkedIds ->
            val value = when (checkedIds.firstOrNull()) {
                R.id.upscaleChipXbrz -> BootstrapStore.UPSCALE_XBRZ_6X
                else -> BootstrapStore.UPSCALE_OFF
            }
            store.saveUpscaleFilter(value)
        }

        val deinterlaceSwitch =
            content.findViewById<com.google.android.material.materialswitch.MaterialSwitch>(R.id.deinterlaceSwitch)
        deinterlaceSwitch.isChecked = store.loadDeinterlaceEnabled()
        deinterlaceSwitch.setOnCheckedChangeListener { _, isChecked ->
            store.saveDeinterlaceEnabled(isChecked)
        }

        val transparentMeshesSwitch =
            content.findViewById<com.google.android.material.materialswitch.MaterialSwitch>(R.id.transparentMeshesSwitch)
        transparentMeshesSwitch.isChecked = store.loadTransparentMeshesEnabled()
        transparentMeshesSwitch.setOnCheckedChangeListener { _, isChecked ->
            store.saveTransparentMeshesEnabled(isChecked)
        }

        val rewindSwitch =
            content.findViewById<com.google.android.material.materialswitch.MaterialSwitch>(R.id.rewindSwitch)
        rewindSwitch.isChecked = store.loadRewindEnabled()
        rewindSwitch.setOnCheckedChangeListener { _, isChecked ->
            store.saveRewindEnabled(isChecked)
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
                        coverFlowAdapter?.let { cfa ->
                            if (entries.isNotEmpty()) {
                                libraryRecyclerView.scrollToPosition(cfa.centerStartPosition())
                            }
                        }
                        val density = resources.displayMetrics.density
                        val coverflowTopPaddingPx =
                            resources.getDimensionPixelSize(R.dimen.coverflow_top_padding)
                        libraryRecyclerView.post {
                            updateCoverFlowPadding(
                                currentViewMode,
                                density,
                                coverflowTopPaddingPx
                            )
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
                        textureFilter = store.loadTextureFilter(),
                        upscaleFilter = store.loadUpscaleFilter(),
                        deinterlace = store.loadDeinterlaceEnabled(),
                        transparentMeshes = store.loadTransparentMeshesEnabled(),
                        rewindEnabled = store.loadRewindEnabled()
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

    private fun applyToggleIcon(mode: Int) {
        val (iconRes, descRes, useTint) = when (mode) {
            BootstrapStore.VIEW_MODE_NA_COVERS    -> Triple(R.drawable.coverflow_box,        R.string.view_mode_na_covers,    false)
            BootstrapStore.VIEW_MODE_JAPAN_COVERS -> Triple(R.drawable.coverflow_jewel_case, R.string.view_mode_japan_covers, false)
            else                                  -> Triple(R.drawable.list_24px,            R.string.view_mode_list,         true)
        }
        viewModeToggleButton.contentDescription = getString(descRes)
        if (useTint) {
            viewModeToggleButton.setImageResource(iconRes)
            viewModeToggleButton.imageTintList = android.content.res.ColorStateList.valueOf(
                com.google.android.material.color.MaterialColors.getColor(
                    viewModeToggleButton, com.google.android.material.R.attr.colorOnSurface
                )
            )
        } else {
            // Complex coverflow vectors collapse into a solid blob when scaled
            // straight to ~36px. Rasterize at native (300dp) size, then let
            // the ImageButton downscale that bitmap with bilinear filtering.
            viewModeToggleButton.imageTintList = null
            viewModeToggleButton.setImageBitmap(rasterizeVector(iconRes))
        }
    }

    private fun rasterizeVector(@androidx.annotation.DrawableRes res: Int): android.graphics.Bitmap {
        val drawable = androidx.appcompat.content.res.AppCompatResources.getDrawable(this, res)!!
        val w = drawable.intrinsicWidth.coerceAtLeast(1)
        val h = drawable.intrinsicHeight.coerceAtLeast(1)
        val bitmap = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bitmap)
        drawable.setBounds(0, 0, w, h)
        drawable.draw(canvas)
        return bitmap
    }

    private fun applyViewMode(mode: Int) {
        // Detach coverflow helpers from previous mode
        snapHelper?.attachToRecyclerView(null)
        snapHelper = null
        libraryRecyclerView.removeOnScrollListener(coverFlowTransformer)

        val density = resources.displayMetrics.density
        val px24 = (24 * density).toInt()
        val coverflowTopPaddingPx = resources.getDimensionPixelSize(R.dimen.coverflow_top_padding)

        if (mode == BootstrapStore.VIEW_MODE_LIST) {
            // Restore default clipping so list items don't render under the header
            libraryRecyclerView.clipChildren = true
            (libraryRecyclerView.parent as? android.view.ViewGroup)?.clipChildren = true

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
            // Let transformed/translated neighbors render without being clipped at recycler edges.
            libraryRecyclerView.clipChildren = false
            (libraryRecyclerView.parent as? android.view.ViewGroup)?.clipChildren = false

            // Center first/last item horizontally and, in portrait, center the
            // coverflow vertically once the RecyclerView has its measured size.
            libraryRecyclerView.post {
                updateCoverFlowPadding(mode, density, coverflowTopPaddingPx)
            }

            // PagerSnapHelper: snaps one cover at a time, centered — the right feel for coverflow.
            val snap = PagerSnapHelper()
            snap.attachToRecyclerView(libraryRecyclerView)
            snapHelper = snap

            libraryRecyclerView.addOnScrollListener(coverFlowTransformer)

            cfa.submitList(currentLibraryEntries)
            if (currentLibraryEntries.isNotEmpty()) {
                libraryRecyclerView.scrollToPosition(cfa.centerStartPosition())
            }

            libraryRecyclerView.post {
                coverFlowTransformer.applyTransforms(libraryRecyclerView)
            }

            // Download the cover index; when ready, rebind all visible items so covers appear
            CoverArtManager.ensureIndexLoaded(mode) {
                coverFlowAdapter?.notifyDataSetChanged()
                libraryRecyclerView.post {
                    coverFlowTransformer.applyTransforms(libraryRecyclerView)
                }
            }
        }
    }

    private fun updateCoverFlowPadding(mode: Int, density: Float, minTopPaddingPx: Int) {
        val itemWidthPx = (COVERFLOW_ITEM_WIDTH_DP * density).toInt()
        val hPad = ((libraryRecyclerView.width - itemWidthPx) / 2).coerceAtLeast(0)
        val topPad = coverFlowTopPaddingFor(mode, density, minTopPaddingPx)
        libraryRecyclerView.setPadding(hPad, topPad, hPad, 0)
    }

    private fun coverFlowTopPaddingFor(mode: Int, density: Float, minTopPaddingPx: Int): Int {
        if (resources.configuration.orientation != Configuration.ORIENTATION_PORTRAIT) {
            return minTopPaddingPx
        }

        val availableHeight = libraryRecyclerView.height
        if (availableHeight <= 0) return minTopPaddingPx

        val measuredItemHeight = maxVisibleCoverFlowChildHeight()
        val itemHeight = if (measuredItemHeight > 0) {
            measuredItemHeight
        } else {
            estimatedCoverFlowItemHeightPx(mode, density)
        }
        val centeredTopPadding = ((availableHeight - itemHeight) / 2f).toInt()
        return centeredTopPadding.coerceAtLeast(minTopPaddingPx)
    }

    private fun maxVisibleCoverFlowChildHeight(): Int {
        var maxHeight = 0
        for (i in 0 until libraryRecyclerView.childCount) {
            maxHeight = maxHeight.coerceAtLeast(libraryRecyclerView.getChildAt(i).height)
        }
        return maxHeight
    }

    private fun estimatedCoverFlowItemHeightPx(mode: Int, density: Float): Int {
        val coverHeightDp = if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) {
            COVERFLOW_NA_COVER_HEIGHT_DP
        } else {
            COVERFLOW_JAPAN_COVER_HEIGHT_DP
        }
        val reflectionHeightDp = if (mode == BootstrapStore.VIEW_MODE_NA_COVERS) {
            COVERFLOW_NA_REFLECTION_HEIGHT_DP
        } else {
            COVERFLOW_JAPAN_REFLECTION_HEIGHT_DP
        }
        return ((coverHeightDp + reflectionHeightDp + COVERFLOW_TITLE_SPACE_DP) * density).toInt()
    }

    private companion object {
        const val COVERFLOW_ITEM_WIDTH_DP = 160
        const val COVERFLOW_NA_COVER_HEIGHT_DP = 228
        const val COVERFLOW_JAPAN_COVER_HEIGHT_DP = 146
        const val COVERFLOW_NA_REFLECTION_HEIGHT_DP = 68
        const val COVERFLOW_JAPAN_REFLECTION_HEIGHT_DP = 44
        const val COVERFLOW_TITLE_SPACE_DP = 54

        val ROM_EXTENSIONS = setOf("bin", "rom")
        val MULTI_FILE_DISC_DESCRIPTOR_EXTENSIONS = setOf("cue", "ccd", "mds")
    }
}
