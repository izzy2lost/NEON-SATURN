package com.izzy2lost.neonsaturn

import android.os.Bundle
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts.OpenDocument
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import java.io.File
import java.util.Locale

class LauncherActivity : AppCompatActivity() {
    private lateinit var store: BootstrapStore
    private lateinit var importer: ContentImporter
    private lateinit var paths: NeonSaturnPaths

    private lateinit var iplValueText: TextView
    private lateinit var cdbValueText: TextView
    private lateinit var discValueText: TextView
    private lateinit var startButton: MaterialButton

    private val importIplLauncher = registerForActivityResult(OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        importDocument(uri, ContentBucket.IPL) { file -> store.saveIpl(file.absolutePath) }
    }

    private val importCdbLauncher = registerForActivityResult(OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        importDocument(uri, ContentBucket.CDB) { file -> store.saveCdb(file.absolutePath) }
    }

    private val importDiscLauncher = registerForActivityResult(OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        importDocument(uri, ContentBucket.DISC) { file -> store.saveDisc(file.absolutePath) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_launcher)

        store = BootstrapStore(this)
        paths = applicationContext.neonSaturnPaths()
        migrateLegacyStorageIfNeeded()
        paths.ensureAll()
        importer = ContentImporter(this, paths)

        iplValueText = findViewById(R.id.iplValueText)
        cdbValueText = findViewById(R.id.cdbValueText)
        discValueText = findViewById(R.id.discValueText)
        startButton = findViewById(R.id.startButton)
        findViewById<TextView>(R.id.hintText).text =
            getString(R.string.storage_hint, paths.root.absolutePath)

        findViewById<MaterialButton>(R.id.importIplButton).setOnClickListener {
            importIplLauncher.launch(arrayOf("*/*"))
        }
        findViewById<MaterialButton>(R.id.importCdbButton).setOnClickListener {
            importCdbLauncher.launch(arrayOf("*/*"))
        }
        findViewById<MaterialButton>(R.id.importDiscButton).setOnClickListener {
            importDiscLauncher.launch(arrayOf("*/*"))
        }
        startButton.setOnClickListener {
            launchEmulator()
        }

        refreshUi()
    }

    override fun onResume() {
        super.onResume()
        refreshUi()
    }

    private fun importDocument(
        uri: android.net.Uri,
        bucket: ContentBucket,
        onStored: (File) -> Unit
    ) {
        runCatching {
            importer.importDocument(uri, bucket)
        }.onSuccess { file ->
            onStored(file)
            refreshUi()
        }.onFailure { error ->
            Toast.makeText(
                this,
                "Import failed: ${error.message ?: error::class.java.simpleName}",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun refreshUi() {
        val selection = resolveSelection()
        iplValueText.text = selection.iplPath?.let(::fileLabel) ?: getString(R.string.not_set)
        cdbValueText.text = selection.cdbPath?.let(::fileLabel) ?: getString(R.string.not_set_optional)
        discValueText.text = selection.discPath?.let(::fileLabel) ?: getString(R.string.not_set)
        startButton.isEnabled = selection.iplPath != null && selection.discPath != null
    }

    private fun launchEmulator() {
        val selection = resolveSelection()
        val validationError = validateSelection(selection)
        if (validationError != null) {
            Toast.makeText(this, validationError, Toast.LENGTH_LONG).show()
            return
        }

        runCatching {
            startActivity(EmulatorActivity.createIntent(this, selection, paths))
        }.onFailure { error ->
            Toast.makeText(
                this,
                "Unable to launch emulator: ${error.message ?: error::class.java.simpleName}",
                Toast.LENGTH_LONG
            ).show()
        }
    }

    private fun fileLabel(path: String): String = File(path).name

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
        val resolved = StoredLaunchSelection(
            iplPath = resolveStoredOrFirst(stored.iplPath, paths.iplDir, ROM_EXTENSIONS),
            cdbPath = resolveStoredOrFirst(stored.cdbPath, paths.cdbDir, ROM_EXTENSIONS),
            discPath = resolveStoredOrFirst(stored.discPath, paths.discDir, DISC_ENTRY_EXTENSIONS)
        )
        if (resolved != stored) {
            store.save(resolved)
        }
        return resolved
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

    private fun validateSelection(selection: StoredLaunchSelection): String? {
        val iplPath = selection.iplPath ?: return "Import or copy a Saturn BIOS first."
        val discPath = selection.discPath ?: return "Import or copy a Saturn disc image first."

        val iplFile = File(iplPath)
        if (!iplFile.isFile) {
            return "The selected BIOS file is missing. Reimport it or copy one into ${paths.iplDir.absolutePath}."
        }

        val cdbPath = selection.cdbPath
        if (cdbPath != null && !File(cdbPath).isFile) {
            store.saveCdb(null)
            return "The selected CD Block ROM is missing. Reimport it or remove it from ${paths.cdbDir.absolutePath}."
        }

        val discFile = File(discPath)
        if (!discFile.isFile) {
            return "The selected disc image is missing. Reimport it or copy one into ${paths.discDir.absolutePath}."
        }

        if (isLikelyMultiFileDescriptor(discFile) && !hasLikelyCompanionFiles(discFile)) {
            return "This disc format needs its companion track files. Copy the full set into ${paths.discDir.absolutePath}."
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
        val DISC_ENTRY_EXTENSIONS = setOf("chd", "cue", "iso", "ccd", "mds")
        val MULTI_FILE_DISC_DESCRIPTOR_EXTENSIONS = setOf("cue", "ccd", "mds")
    }
}
