// Ordering table and primitive buffer: per-primitive painter's algorithm in
// place of a Z-buffer (design doc, section 7).
//
// Everything lives in one word arena, addressed by 24-bit word indices, so the
// layout matches GPU packets on the real machine: a packet's first word (its
// "tag") holds the packet's word count in bits 24-31 and the index of the next
// packet in bits 0-23; 00FFFFFFh ends the list. OT entries are empty packets
// (length 0) in the same arena.
//
// Reverse OT (ClearOTagR style): entry size-1 is the far end and is drawn
// first, entry 0 is the near end and is drawn last, so OTZ can be used as the
// index directly. A newer packet linked at the same OTZ is drawn before the
// older ones, so it ends up behind them; games relied on that.

#pragma once

#include "psx_gpu.h"

#include <cstdint>
#include <vector>

namespace psx {

constexpr uint32_t OT_END = 0x00FFFFFFu;

class PacketArena {
public:
	explicit PacketArena(uint32_t p_capacity_words = 1u << 18) { words.resize(p_capacity_words); }

	void clear() { used = 0; }
	uint32_t get_used() const { return used; }
	uint32_t get_capacity() const { return (uint32_t)words.size(); }
	bool is_full() const { return overflowed; }

	// Allocates a packet of p_payload words plus its tag; returns the tag
	// index, or OT_END if the arena is full.
	uint32_t alloc(uint32_t p_payload);
	uint32_t *payload(uint32_t p_index) { return &words[p_index + 1]; }
	uint32_t &tag(uint32_t p_index) { return words[p_index]; }
	const uint32_t &tag(uint32_t p_index) const { return words[p_index]; }
	const uint32_t *payload(uint32_t p_index) const { return &words[p_index + 1]; }

private:
	std::vector<uint32_t> words;
	uint32_t used = 0;
	bool overflowed = false;

	friend class OrderingTable;
};

class OrderingTable {
public:
	OrderingTable(PacketArena &p_arena, uint32_t p_size);

	uint32_t get_size() const { return size; }

	// Allocates the entries in the arena and links them far to near.
	void clear();
	// Links packet p_packet (a tag index from PacketArena::alloc) into bucket
	// p_otz, clamped to the table. O(1).
	void add(uint32_t p_otz, uint32_t p_packet);
	// Links another OT's whole chain into bucket p_otz (a nested sub-OT).
	void add_table(uint32_t p_otz, const OrderingTable &p_child);
	// Walks the list from the far end and feeds every packet to the GPU.
	// Returns the number of packets sent.
	uint32_t draw(GPU &p_gpu) const;

	uint32_t first_index() const { return base + size - 1; }
	uint32_t last_index() const { return base; }

private:
	PacketArena &arena;
	uint32_t size;
	uint32_t base = OT_END;
};

// Writes GPU packets into an arena: the libgpu-style "prim" builders.
// Each returns the packet's tag index (or OT_END if the arena is full).
class PacketBuilder {
public:
	struct Vert {
		int32_t x = 0, y = 0; // Screen position (before the drawing offset)
		uint8_t r = 128, g = 128, b = 128;
		uint8_t u = 0, v = 0;
	};

	struct Flags {
		bool shaded = false; // Gouraud (otherwise vertex 0's colour)
		bool textured = false;
		bool raw = false; // Textured without colour modulation
		bool semi = false; // Semi-transparent
	};

	PacketBuilder(PacketArena &p_arena, const GP0Encoder &p_encoder) :
			arena(p_arena), enc(p_encoder) {}

	// Triangle (3 vertices) or quad (4, in PS1 "Z" order: TL, TR, BL, BR).
	// p_clut and p_texpage are GP0Encoder::clut / ::texpage halfwords.
	uint32_t polygon(const Vert *p_verts, int p_count, const Flags &p_flags, uint16_t p_clut = 0, uint16_t p_texpage = 0);
	uint32_t line(const Vert &p_a, const Vert &p_b, bool p_shaded, bool p_semi);
	// Sprite / tile. Untextured when p_textured is false.
	uint32_t rectangle(int32_t p_x, int32_t p_y, int32_t p_w, int32_t p_h, uint8_t p_r, uint8_t p_g, uint8_t p_b, bool p_textured = false, bool p_raw = false, bool p_semi = false, uint8_t p_u = 0, uint8_t p_v = 0, uint16_t p_clut = 0);
	uint32_t fill(int32_t p_x, int32_t p_y, int32_t p_w, int32_t p_h, uint8_t p_r, uint8_t p_g, uint8_t p_b);
	// Drawing environment: area, offset, draw mode (texpage) and texture window.
	uint32_t environment(int32_t p_left, int32_t p_top, int32_t p_right, int32_t p_bottom, int32_t p_offset_x, int32_t p_offset_y, uint16_t p_texpage, uint32_t p_window_word = 0xE2000000u);
	// Any raw GP0 words.
	uint32_t raw_words(const uint32_t *p_words, uint32_t p_count);

private:
	PacketArena &arena;
	GP0Encoder enc;
};

} // namespace psx
