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

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): CoverViewHolder {
        val view = LayoutInflater.from(parent.context)
            .inflate(R.layout.item_cover_flow, parent, false)
        return CoverViewHolder(view, coverMode)
    }

    override fun onBindViewHolder(holder: CoverViewHolder, position: Int) {
        holder.bind(entries[position], coverMode, imageLoader, onGameSelected)
    }

    override fun getItemCount(): Int = entries.size

    class CoverViewHolder(itemView: View, coverMode: Int) : RecyclerView.ViewHolder(itemView) {
        private val coverImage: ImageView = itemView.findViewById(R.id.coverImage)
        private val reflectionImage: ImageView = itemView.findViewById(R.id.reflectionImage)
        private val titleText: TextView = itemView.findViewById(R.id.coverTitleText)

        init {
            val density = itemView.context.resources.displayMetrics.density
            // NA boxes are portrait (787×1208), Japan jewel cases are near-square (709×694)
            val coverHeightDp = if (coverMode == BootstrapStore.VIEW_MODE_NA_COVERS) 228 else 146
            val reflHeightDp = if (coverMode == BootstrapStore.VIEW_MODE_NA_COVERS) 68 else 44

            coverImage.layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                (coverHeightDp * density).toInt()
            )
            reflectionImage.layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                (reflHeightDp * density).toInt()
            )
            // Flip reflection upside-down around its own center
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

            if (url != null) {
                coverImage.load(url, imageLoader) {
                    crossfade(true)
                    memoryCachePolicy(CachePolicy.ENABLED)
                    diskCachePolicy(CachePolicy.ENABLED)
                    placeholder(R.drawable.ic_cover_placeholder)
                    error(R.drawable.ic_cover_placeholder)
                }
                // Same URL into reflection — Coil serves from memory cache immediately
                reflectionImage.load(url, imageLoader) {
                    memoryCachePolicy(CachePolicy.ENABLED)
                    diskCachePolicy(CachePolicy.ENABLED)
                    placeholder(R.drawable.ic_cover_placeholder)
                    error(R.drawable.ic_cover_placeholder)
                }
            } else {
                coverImage.setImageResource(R.drawable.ic_cover_placeholder)
                reflectionImage.setImageResource(R.drawable.ic_cover_placeholder)
            }

            itemView.setOnClickListener { onGameSelected(entry) }
        }
    }
}
