package com.izzy2lost.neonsaturn

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.card.MaterialCardView

class GameLibraryAdapter(
    private val onGameSelected: (GameLibraryEntry) -> Unit
) : RecyclerView.Adapter<GameLibraryAdapter.GameViewHolder>() {
    private var entries: List<GameLibraryEntry> = emptyList()

    fun submitList(items: List<GameLibraryEntry>) {
        entries = items
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): GameViewHolder {
        val inflater = LayoutInflater.from(parent.context)
        val view = inflater.inflate(R.layout.item_game_entry, parent, false)
        return GameViewHolder(view)
    }

    override fun onBindViewHolder(holder: GameViewHolder, position: Int) {
        holder.bind(entries[position], onGameSelected)
    }

    override fun getItemCount(): Int = entries.size

    class GameViewHolder(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val card = itemView.findViewById<MaterialCardView>(R.id.gameCard)
        private val title = itemView.findViewById<TextView>(R.id.gameTitleText)
        private val subtitle = itemView.findViewById<TextView>(R.id.gameSubtitleText)
        private val type = itemView.findViewById<TextView>(R.id.gameTypeText)

        fun bind(entry: GameLibraryEntry, onGameSelected: (GameLibraryEntry) -> Unit) {
            title.text = entry.title
            subtitle.text = entry.subtitle
            type.text = entry.launchFileName.substringAfterLast('.', "").uppercase()
            card.setOnClickListener { onGameSelected(entry) }
        }
    }
}
