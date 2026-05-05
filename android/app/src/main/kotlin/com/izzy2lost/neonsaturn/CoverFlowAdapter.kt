package com.izzy2lost.neonsaturn

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import coil.ImageLoader
import coil.load
import coil.request.CachePolicy

class CoverFlowAdapter(
    private val onGameSelected: (GameLibraryEntry) -> Unit,
    private val coverMode: Int,
    private val imageLoader: ImageLoader
) : RecyclerView.Adapter<CoverFlowAdapter.CoverViewHolder>() {

    private var entries: List<GameLibraryEntry> = emptyList()

    fun submitList(items: List<GameLibraryEntry>) {
        entries = items
        notifyDataSetChanged()
    }

    /**
     * Position aligned with entry index 0, near the middle of the virtual range.
     * Scroll the RecyclerView here after submitList so the user can wrap in
     * either direction without practically reaching an edge.
     */
    fun centerStartPosition(): Int {
        if (entries.isEmpty()) return 0
        val mid = Int.MAX_VALUE / 2
        return mid - (mid % entries.size)
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): CoverViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_cover_flow, parent, false)
        return CoverViewHolder(view, coverMode)
    }

    override fun onBindViewHolder(holder: CoverViewHolder, position: Int) {
        val entry = entries[position % entries.size]
        holder.bind(entry, coverMode, imageLoader, onGameSelected)
    }

    override fun getItemCount(): Int = if (entries.isEmpty()) 0 else Int.MAX_VALUE

    class CoverViewHolder(itemView: View, coverMode: Int) : RecyclerView.ViewHolder(itemView) {
        private val coverImage: ImageView = itemView.findViewById(R.id.coverImage)
        private val reflectionImage: ImageView = itemView.findViewById(R.id.reflectionImage)
        private val titleText: TextView = itemView.findViewById(R.id.coverTitleText)

        init {
            val density = itemView.context.resources.displayMetrics.density
            // NA boxes are portrait (787×1208), Japan jewel cases are near-square (709×694)
            val coverHeightDp = if (coverMode == BootstrapStore.VIEW_MODE_NA_COVERS) 228 else 146
            val reflHeightDp = if (coverMode == BootstrapStore.VIEW_MODE_NA_COVERS) 68 else 44
            val coverHeightPx = (coverHeightDp * density).toInt()
            val reflHeightPx = (reflHeightDp * density).toInt()

            coverImage.layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                coverHeightPx
            )

            // Reflection: ImageView matches the cover's full height so the bitmap is
            // sized identically to the cover (fitCenter). The parent FrameLayout is
            // shorter (reflHeightPx) and clips — so only the cover's bottom slice
            // shows once we flip with scaleY=-1.
            val reflParent = reflectionImage.parent as android.view.View
            reflParent.layoutParams = reflParent.layoutParams.also { it.height = reflHeightPx }
            reflectionImage.layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                coverHeightPx,
                android.view.Gravity.TOP
            )
            reflectionImage.scaleY = -1f
        }

        fun bind(
            entry: GameLibraryEntry,
            coverMode: Int,
            imageLoader: ImageLoader,
            onGameSelected: (GameLibraryEntry) -> Unit
        ) {
            titleText.text = entry.title
            val url = CoverArtManager.coverUrl(entry.title, coverMode)
            val placeholderRes = if (coverMode == BootstrapStore.VIEW_MODE_NA_COVERS)
                R.drawable.ic_cover_placeholder_na
            else
                R.drawable.ic_cover_placeholder_japan

            if (url != null) {
                coverImage.load(url, imageLoader) {
                    crossfade(true)
                    memoryCachePolicy(CachePolicy.ENABLED)
                    diskCachePolicy(CachePolicy.ENABLED)
                    placeholder(placeholderRes)
                    error(placeholderRes)
                }
                // Same URL into reflection — Coil serves from memory cache immediately
                reflectionImage.load(url, imageLoader) {
                    memoryCachePolicy(CachePolicy.ENABLED)
                    diskCachePolicy(CachePolicy.ENABLED)
                    placeholder(placeholderRes)
                    error(placeholderRes)
                }
            } else {
                // Route through Coil so any in-flight request from a recycled
                // holder is cancelled — otherwise the previous game's cover
                // can land on top of the placeholder after recycle.
                coverImage.load(placeholderRes, imageLoader)
                reflectionImage.load(placeholderRes, imageLoader)
            }

            itemView.setOnClickListener { onGameSelected(entry) }
        }
    }
}
