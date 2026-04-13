package com.izzy2lost.neonsaturn

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.File
import java.io.IOException
import java.util.ArrayDeque
import java.util.LinkedHashMap
import java.util.Locale

private val SUPPORTED_GAME_EXTENSIONS = setOf("chd", "cue", "iso", "ccd", "mds")
private val CUE_FILE_REGEX = Regex("^\\s*FILE\\s+\"([^\"]+)\"", RegexOption.IGNORE_CASE)

data class GameLibraryFile(
    val name: String,
    val uri: Uri
)

data class GameLibraryEntry(
    val title: String,
    val subtitle: String,
    val launchFileName: String,
    val sourceFiles: List<GameLibraryFile>
)

fun Context.scanGameLibrary(treeUri: Uri): List<GameLibraryEntry> {
    val root = DocumentFile.fromTreeUri(this, treeUri)
        ?: throw IOException("Unable to open the selected games folder")

    val entries = mutableListOf<GameLibraryEntry>()
    val pending = ArrayDeque<Pair<DocumentFile, String>>()
    pending += root to ""

    while (pending.isNotEmpty()) {
        val (directory, relativePath) = pending.removeFirst()
        val children = directory.listFiles()
            .sortedBy { child -> child.name?.lowercase(Locale.ROOT).orEmpty() }

        val siblingFiles = children
            .filter { child -> child.isFile && !child.name.isNullOrBlank() }
            .associateBy { child -> child.name!!.lowercase(Locale.ROOT) }

        for (child in children) {
            when {
                child.isDirectory -> {
                    val childName = child.name.orEmpty()
                    val childPath = if (relativePath.isBlank()) "$childName/" else "$relativePath$childName/"
                    pending += child to childPath
                }

                child.isFile -> {
                    val fileName = child.name ?: continue
                    val extension = fileName.substringAfterLast('.', "").lowercase(Locale.ROOT)
                    if (extension !in SUPPORTED_GAME_EXTENSIONS) {
                        continue
                    }

                    val sourceFiles = when (extension) {
                        "cue" -> resolveCueFiles(child, siblingFiles)
                        "ccd" -> resolveSiblingFamily(child, siblingFiles, listOf("img", "sub"))
                        "mds" -> resolveSiblingFamily(child, siblingFiles, listOf("mdf"))
                        else -> listOf(GameLibraryFile(fileName, child.uri))
                    }

                    val subtitle = if (relativePath.isBlank()) {
                        fileName
                    } else {
                        "$relativePath$fileName"
                    }

                    entries += GameLibraryEntry(
                        title = fileName.substringBeforeLast('.'),
                        subtitle = subtitle,
                        launchFileName = fileName,
                        sourceFiles = sourceFiles
                    )
                }
            }
        }
    }

    return entries.sortedBy { entry -> entry.title.lowercase(Locale.ROOT) }
}

fun Context.stageGameForLaunch(
    entry: GameLibraryEntry,
    paths: NeonSaturnPaths = neonSaturnPaths()
): File {
    paths.ensureAll()

    val stageRoot = File(paths.supportDir, "staged-game")
    if (stageRoot.exists()) {
        stageRoot.deleteRecursively()
    }
    stageRoot.mkdirs()

    val copiedFiles = entry.sourceFiles.distinctBy { file -> file.name.lowercase(Locale.ROOT) }
    for (source in copiedFiles) {
        val target = File(stageRoot, source.name)
        contentResolver.openInputStream(source.uri)?.use { input ->
            target.outputStream().use { output ->
                input.copyTo(output)
            }
        } ?: throw IOException("Unable to read ${source.name} from the games folder")
    }

    return File(stageRoot, entry.launchFileName).takeIf(File::isFile)
        ?: throw IOException("Unable to prepare ${entry.title} for launch")
}

private fun Context.resolveCueFiles(
    cueDocument: DocumentFile,
    siblings: Map<String, DocumentFile>
): List<GameLibraryFile> {
    val orderedFiles = LinkedHashMap<String, GameLibraryFile>()
    val cueName = cueDocument.name ?: return emptyList()
    orderedFiles[cueName.lowercase(Locale.ROOT)] = GameLibraryFile(cueName, cueDocument.uri)

    val referencedFiles = contentResolver.openInputStream(cueDocument.uri)?.bufferedReader()?.useLines { lines ->
        lines.mapNotNull { line ->
            CUE_FILE_REGEX.find(line)?.groupValues?.getOrNull(1)
        }.toList()
    }.orEmpty()

    for (reference in referencedFiles) {
        val siblingName = reference.substringAfterLast('/').substringAfterLast('\\')
        val sibling = siblings[siblingName.lowercase(Locale.ROOT)] ?: continue
        val siblingFileName = sibling.name ?: continue
        orderedFiles[siblingFileName.lowercase(Locale.ROOT)] = GameLibraryFile(siblingFileName, sibling.uri)
    }

    return orderedFiles.values.toList()
}

private fun resolveSiblingFamily(
    primaryDocument: DocumentFile,
    siblings: Map<String, DocumentFile>,
    companionExtensions: List<String>
): List<GameLibraryFile> {
    val orderedFiles = LinkedHashMap<String, GameLibraryFile>()
    val primaryName = primaryDocument.name ?: return emptyList()
    orderedFiles[primaryName.lowercase(Locale.ROOT)] = GameLibraryFile(primaryName, primaryDocument.uri)

    val stem = primaryName.substringBeforeLast('.')
    for (extension in companionExtensions) {
        val siblingName = "$stem.$extension"
        val sibling = siblings[siblingName.lowercase(Locale.ROOT)] ?: continue
        val siblingFileName = sibling.name ?: continue
        orderedFiles[siblingFileName.lowercase(Locale.ROOT)] = GameLibraryFile(siblingFileName, sibling.uri)
    }

    return orderedFiles.values.toList()
}
