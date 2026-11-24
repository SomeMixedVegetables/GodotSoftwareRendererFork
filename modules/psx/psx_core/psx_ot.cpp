#include "psx_ot.h"

namespace psx {

uint32_t PacketArena::alloc(uint32_t p_payload) {
	if (p_payload > 255 || used + 1 + p_payload > words.size() || used + 1 + p_payload > OT_END) {
		overflowed = true;
		return OT_END;
	}
	const uint32_t index = used;
	used += 1 + p_payload;
	words[index] = (p_payload << 24) | OT_END;
	return index;
}

OrderingTable::OrderingTable(PacketArena &p_arena, uint32_t p_size) :
		arena(p_arena), size(p_size < 1 ? 1 : p_size) {}

void OrderingTable::clear() {
	base = OT_END;
	if (arena.used + size > arena.words.size()) {
		arena.overflowed = true;
		return;
	}
	base = arena.used;
	arena.used += size;
	// Entry i links to entry i-1; entry 0 ends the list.
	arena.words[base] = OT_END;
	for (uint32_t i = 1; i < size; i++) {
		arena.words[base + i] = base + i - 1;
	}
}

void OrderingTable::add(uint32_t p_otz, uint32_t p_packet) {
	if (base == OT_END || p_packet == OT_END) {
		return;
	}
	if (p_otz >= size) {
		p_otz = size - 1;
	}
	uint32_t &entry = arena.words[base + p_otz];
	uint32_t &packet = arena.words[p_packet];
	packet = (packet & 0xFF000000u) | (entry & 0x00FFFFFFu);
	entry = (entry & 0xFF000000u) | p_packet;
}

void OrderingTable::add_table(uint32_t p_otz, const OrderingTable &p_child) {
	if (base == OT_END || p_child.base == OT_END) {
		return;
	}
	if (p_otz >= size) {
		p_otz = size - 1;
	}
	// The child's chain ends after its entry 0 and whatever is linked there;
	// that tail continues into what the bucket pointed at, and the bucket
	// then points at the child's far end.
	uint32_t tail = p_child.last_index();
	while ((arena.words[tail] & 0x00FFFFFFu) != OT_END) {
		tail = arena.words[tail] & 0x00FFFFFFu;
	}
	uint32_t &entry = arena.words[base + p_otz];
	uint32_t &child_last = arena.words[tail];
	child_last = (child_last & 0xFF000000u) | (entry & 0x00FFFFFFu);
	entry = (entry & 0xFF000000u) | p_child.first_index();
}

uint32_t OrderingTable::draw(GPU &p_gpu) const {
	if (base == OT_END) {
		return 0;
	}
	uint32_t index = first_index();
	uint32_t packets = 0;
	uint32_t guard = arena.used + 1; // A corrupt list can never loop forever.
	while (index != OT_END && index < arena.used && guard-- > 0) {
		const uint32_t tag = arena.words[index];
		const uint32_t count = tag >> 24;
		if (count > 0) {
			p_gpu.write_gp0(&arena.words[index + 1], count);
			packets++;
		}
		index = tag & 0x00FFFFFFu;
	}
	return packets;
}

// --- PacketBuilder ---

static uint32_t color_word(uint8_t p_r, uint8_t p_g, uint8_t p_b) {
	return rgb24(p_r, p_g, p_b);
}

uint32_t PacketBuilder::polygon(const Vert *p_verts, int p_count, const Flags &p_flags, uint16_t p_clut, uint16_t p_texpage) {
	const bool quad = p_count == 4;
	const int n = quad ? 4 : 3;
	const uint32_t words = 1 + n * (p_flags.textured ? 2 : 1) + (p_flags.shaded ? n - 1 : 0);
	const uint32_t index = arena.alloc(words);
	if (index == OT_END) {
		return OT_END;
	}
	uint32_t *w = arena.payload(index);
	const uint32_t op = 0x20u | (p_flags.shaded ? 0x10u : 0) | (quad ? 0x08u : 0) | (p_flags.textured ? 0x04u : 0) | (p_flags.semi ? 0x02u : 0) | (p_flags.raw ? 0x01u : 0);
	int k = 0;
	for (int i = 0; i < n; i++) {
		const Vert &v = p_verts[i];
		if (i == 0) {
			w[k++] = (op << 24) | color_word(v.r, v.g, v.b);
		} else if (p_flags.shaded) {
			w[k++] = color_word(v.r, v.g, v.b);
		}
		w[k++] = enc.vertex(v.x, v.y);
		if (p_flags.textured) {
			const uint16_t high = i == 0 ? p_clut : (i == 1 ? p_texpage : 0);
			w[k++] = enc.texcoord(v.u, v.v, high);
		}
	}
	return index;
}

uint32_t PacketBuilder::line(const Vert &p_a, const Vert &p_b, bool p_shaded, bool p_semi) {
	const uint32_t index = arena.alloc(p_shaded ? 4 : 3);
	if (index == OT_END) {
		return OT_END;
	}
	uint32_t *w = arena.payload(index);
	const uint32_t op = 0x40u | (p_shaded ? 0x10u : 0) | (p_semi ? 0x02u : 0);
	int k = 0;
	w[k++] = (op << 24) | color_word(p_a.r, p_a.g, p_a.b);
	w[k++] = enc.vertex(p_a.x, p_a.y);
	if (p_shaded) {
		w[k++] = color_word(p_b.r, p_b.g, p_b.b);
	}
	w[k++] = enc.vertex(p_b.x, p_b.y);
	return index;
}

uint32_t PacketBuilder::rectangle(int32_t p_x, int32_t p_y, int32_t p_w, int32_t p_h, uint8_t p_r, uint8_t p_g, uint8_t p_b, bool p_textured, bool p_raw, bool p_semi, uint8_t p_u, uint8_t p_v, uint16_t p_clut) {
	const uint32_t index = arena.alloc(p_textured ? 4 : 3);
	if (index == OT_END) {
		return OT_END;
	}
	uint32_t *w = arena.payload(index);
	const uint32_t op = 0x60u | (p_textured ? 0x04u : 0) | (p_semi ? 0x02u : 0) | (p_raw ? 0x01u : 0);
	int k = 0;
	w[k++] = (op << 24) | color_word(p_r, p_g, p_b);
	w[k++] = enc.vertex(p_x, p_y);
	if (p_textured) {
		w[k++] = enc.texcoord(p_u, p_v, p_clut);
	}
	w[k++] = enc.xy(p_w, p_h);
	return index;
}

uint32_t PacketBuilder::fill(int32_t p_x, int32_t p_y, int32_t p_w, int32_t p_h, uint8_t p_r, uint8_t p_g, uint8_t p_b) {
	const uint32_t index = arena.alloc(3);
	if (index == OT_END) {
		return OT_END;
	}
	uint32_t *w = arena.payload(index);
	w[0] = 0x02000000u | color_word(p_r, p_g, p_b);
	w[1] = enc.xy(p_x, p_y);
	w[2] = enc.xy(p_w, p_h);
	return index;
}

uint32_t PacketBuilder::environment(int32_t p_left, int32_t p_top, int32_t p_right, int32_t p_bottom, int32_t p_offset_x, int32_t p_offset_y, uint16_t p_texpage, uint32_t p_window_word) {
	const uint32_t index = arena.alloc(5);
	if (index == OT_END) {
		return OT_END;
	}
	uint32_t *w = arena.payload(index);
	w[0] = enc.draw_mode(p_texpage);
	w[1] = p_window_word;
	w[2] = enc.draw_area_top_left(p_left, p_top);
	w[3] = enc.draw_area_bottom_right(p_right, p_bottom);
	w[4] = enc.draw_offset(p_offset_x, p_offset_y);
	return index;
}

uint32_t PacketBuilder::raw_words(const uint32_t *p_words, uint32_t p_count) {
	const uint32_t index = arena.alloc(p_count);
	if (index == OT_END) {
		return OT_END;
	}
	uint32_t *w = arena.payload(index);
	for (uint32_t i = 0; i < p_count; i++) {
		w[i] = p_words[i];
	}
	return index;
}

} // namespace psx
