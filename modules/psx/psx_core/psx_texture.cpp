#include "psx_texture.h"

#include <algorithm>
#include <cstring>

namespace psx {

static int32_t next_pow2(int32_t p_value) {
	int32_t p = 1;
	while (p < p_value) {
		p <<= 1;
	}
	return p;
}

static int32_t clamp_pow2(int32_t p_value, int32_t p_lo, int32_t p_hi) {
	const int32_t p = next_pow2(p_value);
	return p < p_lo ? p_lo : (p > p_hi ? p_hi : p);
}

// 8-bit to 5-bit, rounded.
static uint32_t to5(uint32_t p_value) {
	return (p_value * 31 + 127) / 255;
}

namespace {

// A texel after alpha classification: 15-bit colour plus the semi flag in
// bit 15, or TRANSPARENT.
constexpr uint32_t TRANSPARENT = 0xFFFFFFFFu;

struct HistEntry {
	uint16_t key; // rgb15 | semi << 15
	uint32_t count;
};

int channel(uint16_t p_key, int p_c) {
	return (p_key >> (p_c * 5)) & 31;
}

// Median cut over the histogram, count-weighted; deterministic.
std::vector<uint16_t> median_cut(std::vector<HistEntry> p_hist, int32_t p_colors) {
	struct Box {
		size_t begin, end;
	};
	std::vector<Box> boxes = { { 0, p_hist.size() } };
	auto box_range = [&](const Box &b, int &r_channel) {
		int best = -1;
		for (int c = 0; c < 3; c++) {
			int lo = 31, hi = 0;
			for (size_t i = b.begin; i < b.end; i++) {
				const int v = channel(p_hist[i].key, c);
				lo = v < lo ? v : lo;
				hi = v > hi ? v : hi;
			}
			if (hi - lo > best) {
				best = hi - lo;
				r_channel = c;
			}
		}
		return best;
	};
	while ((int32_t)boxes.size() < p_colors) {
		// Split the box with the widest channel range, weighted by pixel count.
		int64_t best_score = 0;
		int best_box = -1, best_channel = 0;
		for (size_t i = 0; i < boxes.size(); i++) {
			if (boxes[i].end - boxes[i].begin < 2) {
				continue;
			}
			int c = 0;
			const int range = box_range(boxes[i], c);
			uint64_t count = 0;
			for (size_t k = boxes[i].begin; k < boxes[i].end; k++) {
				count += p_hist[k].count;
			}
			const int64_t score = (int64_t)range * 1000000 + (int64_t)(count > 999999 ? 999999 : count);
			if (range > 0 && score > best_score) {
				best_score = score;
				best_box = (int)i;
				best_channel = c;
			}
		}
		if (best_box < 0) {
			break;
		}
		Box b = boxes[best_box];
		const int c = best_channel;
		std::sort(p_hist.begin() + b.begin, p_hist.begin() + b.end, [c](const HistEntry &x, const HistEntry &y) {
			const int vx = channel(x.key, c), vy = channel(y.key, c);
			return vx != vy ? vx < vy : x.key < y.key;
		});
		uint64_t total = 0;
		for (size_t k = b.begin; k < b.end; k++) {
			total += p_hist[k].count;
		}
		uint64_t acc = 0;
		size_t split = b.begin + 1;
		for (size_t k = b.begin; k < b.end - 1; k++) {
			acc += p_hist[k].count;
			split = k + 1;
			if (acc * 2 >= total) {
				break;
			}
		}
		// Never split between equal channel values.
		while (split < b.end && split > b.begin + 1 && channel(p_hist[split].key, c) == channel(p_hist[split - 1].key, c)) {
			split++;
		}
		if (split >= b.end) {
			split = b.begin + 1;
			while (split < b.end && channel(p_hist[split].key, c) == channel(p_hist[split - 1].key, c)) {
				split++;
			}
		}
		if (split >= b.end) {
			break;
		}
		boxes[best_box] = { b.begin, split };
		boxes.push_back({ split, b.end });
	}
	std::vector<uint16_t> palette;
	for (const Box &b : boxes) {
		uint64_t sum[3] = { 0, 0, 0 }, total = 0, semi = 0;
		for (size_t k = b.begin; k < b.end; k++) {
			const uint32_t n = p_hist[k].count;
			for (int c = 0; c < 3; c++) {
				sum[c] += (uint64_t)channel(p_hist[k].key, c) * n;
			}
			semi += (p_hist[k].key & 0x8000) ? n : 0;
			total += n;
		}
		if (total == 0) {
			continue;
		}
		uint16_t key = 0;
		for (int c = 0; c < 3; c++) {
			key |= (uint16_t)(((sum[c] + total / 2) / total) << (c * 5));
		}
		key |= semi * 2 > total ? 0x8000 : 0;
		palette.push_back(key);
	}
	std::sort(palette.begin(), palette.end());
	palette.erase(std::unique(palette.begin(), palette.end()), palette.end());
	return palette;
}

} // namespace

bool convert_texture(const uint8_t *p_rgba, int32_t p_width, int32_t p_height, const TextureOptions &p_options, TextureData &r_texture) {
	r_texture = TextureData();
	if (!p_rgba || p_width <= 0 || p_height <= 0) {
		return false;
	}
	const int32_t max_size = clamp_pow2(p_options.max_size, 8, 256);
	const int32_t tw = clamp_pow2(p_width, 8, max_size);
	const int32_t th = clamp_pow2(p_height, 8, max_size);

	// Resample to tw x th: area average when shrinking (colour weighted by
	// alpha, so cut-out texels do not bleed), nearest when growing.
	const bool use_alpha = p_options.alpha_mode != ALPHA_IGNORE;
	std::vector<uint32_t> texel(tw * th);
	uint64_t mean_sum[3] = { 0, 0, 0 }, mean_count = 0;
	for (int32_t ty = 0; ty < th; ty++) {
		const int32_t y0 = (int32_t)((int64_t)ty * p_height / th);
		const int32_t y1 = std::max(y0 + 1, (int32_t)((int64_t)(ty + 1) * p_height / th));
		for (int32_t tx = 0; tx < tw; tx++) {
			const int32_t x0 = (int32_t)((int64_t)tx * p_width / tw);
			const int32_t x1 = std::max(x0 + 1, (int32_t)((int64_t)(tx + 1) * p_width / tw));
			uint64_t rgb[3] = { 0, 0, 0 }, plain[3] = { 0, 0, 0 }, alpha = 0, n = 0;
			for (int32_t y = y0; y < y1; y++) {
				const uint8_t *row = p_rgba + ((size_t)y * p_width) * 4;
				for (int32_t x = x0; x < x1; x++) {
					const uint8_t *px = row + x * 4;
					const uint32_t a = use_alpha ? px[3] : 255;
					for (int c = 0; c < 3; c++) {
						rgb[c] += (uint64_t)px[c] * a;
						plain[c] += px[c];
					}
					alpha += a;
					n++;
				}
			}
			uint32_t c8[3];
			for (int c = 0; c < 3; c++) {
				c8[c] = (uint32_t)(alpha > 0 ? (rgb[c] + alpha / 2) / alpha : (plain[c] + n / 2) / n);
			}
			const uint32_t a8 = (uint32_t)((alpha + n / 2) / n);
			bool transparent = false, semi = false;
			switch (p_options.alpha_mode) {
				case ALPHA_CUT:
					transparent = a8 < (uint32_t)(p_options.alpha_cut * 255.0f + 0.5f);
					break;
				case ALPHA_BLEND:
					transparent = a8 <= 64;
					semi = !transparent && a8 < 191;
					break;
				default:
					break;
			}
			if (transparent) {
				texel[ty * tw + tx] = TRANSPARENT;
				r_texture.has_transparent = true;
				continue;
			}
			for (int c = 0; c < 3; c++) {
				mean_sum[c] += c8[c];
			}
			mean_count++;
			const uint32_t key = to5(c8[0]) | (to5(c8[1]) << 5) | (to5(c8[2]) << 10) | (semi ? 0x8000u : 0u);
			texel[ty * tw + tx] = key;
			r_texture.has_semi = r_texture.has_semi || semi;
		}
	}
	if (mean_count > 0) {
		for (int c = 0; c < 3; c++) {
			r_texture.mean[c] = (uint8_t)((mean_sum[c] + mean_count / 2) / mean_count);
		}
	}
	r_texture.width = tw;
	r_texture.height = th;

	// Final halfword for a colour key: STP from the semi flag; opaque black
	// is 8000h (0000h would be transparent), or the darkest grey if the
	// texture is drawn semi-transparent for its STP texels.
	const bool has_semi = r_texture.has_semi;
	auto texel_value = [has_semi](uint32_t p_key) -> uint16_t {
		if (p_key == 0) {
			return has_semi ? (uint16_t)0x0421 : (uint16_t)0x8000;
		}
		return (uint16_t)p_key;
	};

	// Histogram of visible colours.
	std::vector<uint32_t> counts(65536, 0);
	for (uint32_t t : texel) {
		if (t != TRANSPARENT) {
			counts[t]++;
		}
	}
	std::vector<HistEntry> hist;
	for (uint32_t k = 0; k < 65536; k++) {
		if (counts[k]) {
			hist.push_back({ (uint16_t)k, counts[k] });
		}
	}
	const int32_t reserved = r_texture.has_transparent ? 1 : 0;
	const int32_t unique = (int32_t)hist.size();
	int32_t limit_colors = p_options.max_colors > 0 ? std::min(std::max(p_options.max_colors, 2), 256) : 256;

	uint8_t depth = TEXTURE_8BIT;
	switch (p_options.depth) {
		case DEPTH_FORCE_4BIT:
			depth = TEXTURE_4BIT;
			break;
		case DEPTH_FORCE_8BIT:
			depth = TEXTURE_8BIT;
			break;
		case DEPTH_FORCE_15BIT:
			depth = TEXTURE_15BIT;
			break;
		default:
			depth = std::min(unique, limit_colors) + reserved <= 16 ? TEXTURE_4BIT : TEXTURE_8BIT;
			break;
	}
	r_texture.depth = depth;

	if (depth == TEXTURE_15BIT) {
		r_texture.texels.resize((size_t)tw * th);
		for (size_t i = 0; i < texel.size(); i++) {
			r_texture.texels[i] = texel[i] == TRANSPARENT ? 0 : texel_value(texel[i]);
		}
		r_texture.colors = unique;
		return true;
	}

	const int32_t entries = depth == TEXTURE_4BIT ? 16 : 256;
	const int32_t available = std::max(1, std::min(entries, limit_colors) - reserved);
	std::vector<uint16_t> palette;
	if (unique <= available) {
		for (const HistEntry &h : hist) {
			palette.push_back(h.key);
		}
	} else {
		palette = median_cut(hist, available);
	}
	r_texture.colors = (int32_t)palette.size() + reserved;

	// Nearest palette entry for every colour in use (semi flag must match
	// when the palette has an entry of that kind).
	std::vector<int16_t> lookup(65536, -1);
	for (const HistEntry &h : hist) {
		const bool semi = (h.key & 0x8000) != 0;
		int best = -1;
		int best_d = 1 << 30;
		for (int pass = 0; pass < 2 && best < 0; pass++) {
			for (size_t p = 0; p < palette.size(); p++) {
				if (pass == 0 && ((palette[p] & 0x8000) != 0) != semi) {
					continue;
				}
				int d = 0;
				for (int c = 0; c < 3; c++) {
					const int diff = channel(h.key, c) - channel(palette[p], c);
					d += diff * diff;
				}
				if (d < best_d) {
					best_d = d;
					best = (int)p;
				}
			}
		}
		lookup[h.key] = (int16_t)(best + reserved);
	}

	r_texture.clut.assign(entries, 0);
	r_texture.clut_semi.assign(entries, 0);
	r_texture.clut_white.assign(entries, 0);
	for (size_t p = 0; p < palette.size(); p++) {
		const uint16_t v = texel_value(palette[p]);
		r_texture.clut[p + reserved] = v;
		r_texture.clut_semi[p + reserved] = (uint16_t)(v | 0x8000);
		r_texture.clut_white[p + reserved] = 0xFFFF;
	}

	const int32_t per_hw = depth == TEXTURE_4BIT ? 4 : 2;
	const int32_t bits = 16 / per_hw;
	const int32_t hw = tw / per_hw;
	r_texture.texels.assign((size_t)hw * th, 0);
	for (int32_t y = 0; y < th; y++) {
		for (int32_t x = 0; x < tw; x++) {
			const uint32_t t = texel[y * tw + x];
			const uint32_t index = t == TRANSPARENT ? 0u : (uint32_t)lookup[t];
			r_texture.texels[(size_t)y * hw + x / per_hw] |= (uint16_t)(index << ((x % per_hw) * bits));
		}
	}
	return true;
}

void upload_rect(GPU &p_gpu, int32_t p_x, int32_t p_y, int32_t p_width, int32_t p_height, const uint16_t *p_data, uint16_t p_bank) {
	if (p_width <= 0 || p_height <= 0) {
		return;
	}
	const GP0Encoder &enc = p_gpu.get_encoder();
	if (p_bank != 0) {
		p_gpu.write_gp0(GP0Encoder::texture_bank(p_bank));
	}
	p_gpu.write_gp0(0xA0000000u);
	p_gpu.write_gp0(enc.xy(p_x, p_y));
	p_gpu.write_gp0(enc.xy(p_width, p_height));
	const size_t count = (size_t)p_width * p_height;
	for (size_t i = 0; i < count; i += 2) {
		const uint32_t lo = p_data[i];
		const uint32_t hi = i + 1 < count ? p_data[i + 1] : 0u;
		p_gpu.write_gp0(lo | (hi << 16));
	}
	if (p_bank != 0) {
		p_gpu.write_gp0(GP0Encoder::texture_bank(0));
	}
}

// --- VramAllocator ---

void VramAllocator::reset() {
	cells.assign((size_t)GRID_W * GRID_H, 0);
	clut_blocks.clear();
}

int32_t VramAllocator::box_width(int32_t p_hw_width) {
	return clamp_pow2(p_hw_width, CELL_W, VRAM_BASE_WIDTH);
}

int32_t VramAllocator::box_height(int32_t p_height) {
	return clamp_pow2(p_height, CELL_H, 256);
}

bool VramAllocator::_fits(int32_t p_cx, int32_t p_cy, int32_t p_cw, int32_t p_ch) const {
	for (int32_t y = p_cy; y < p_cy + p_ch; y++) {
		const uint8_t *row = &cells[(size_t)y * GRID_W + p_cx];
		for (int32_t x = 0; x < p_cw; x++) {
			if (row[x]) {
				return false;
			}
		}
	}
	return true;
}

void VramAllocator::_mark(int32_t p_cx, int32_t p_cy, int32_t p_cw, int32_t p_ch, uint8_t p_value) {
	for (int32_t y = p_cy; y < p_cy + p_ch; y++) {
		memset(&cells[(size_t)y * GRID_W + p_cx], p_value, (size_t)p_cw);
	}
}

void VramAllocator::reserve(int32_t p_x, int32_t p_y, int32_t p_width, int32_t p_height) {
	const int32_t x0 = std::max(0, p_x), y0 = std::max(0, p_y);
	const int32_t x1 = std::min(VRAM_BASE_WIDTH, p_x + p_width), y1 = std::min(VRAM_BASE_HEIGHT, p_y + p_height);
	if (x1 <= x0 || y1 <= y0) {
		return;
	}
	const int32_t cx0 = x0 / CELL_W, cy0 = y0 / CELL_H;
	const int32_t cx1 = (x1 + CELL_W - 1) / CELL_W, cy1 = (y1 + CELL_H - 1) / CELL_H;
	_mark(cx0, cy0, cx1 - cx0, cy1 - cy0, 1);
}

bool VramAllocator::alloc(int32_t p_hw_width, int32_t p_height, int32_t &r_x, int32_t &r_y) {
	const int32_t bw = box_width(p_hw_width), bh = box_height(p_height);
	if (p_hw_width > 256 || p_height > 256) {
		return false;
	}
	const int32_t cw = bw / CELL_W, ch = bh / CELL_H;
	for (int32_t cy = 0; cy + ch <= GRID_H; cy += ch) {
		for (int32_t cx = 0; cx + cw <= GRID_W; cx += cw) {
			if (_fits(cx, cy, cw, ch)) {
				_mark(cx, cy, cw, ch, 1);
				r_x = cx * CELL_W;
				r_y = cy * CELL_H;
				return true;
			}
		}
	}
	return false;
}

void VramAllocator::free(int32_t p_x, int32_t p_y, int32_t p_hw_width, int32_t p_height) {
	const int32_t bw = box_width(p_hw_width), bh = box_height(p_height);
	_mark(p_x / CELL_W, p_y / CELL_H, bw / CELL_W, bh / CELL_H, 0);
}

bool VramAllocator::alloc_clut(int32_t p_entries, int32_t &r_x, int32_t &r_y) {
	const int32_t slots = p_entries <= 16 ? 1 : 16;
	const uint32_t mask = slots == 16 ? 0xFFFFu : 1u;
	for (int pass = 0; pass < 2; pass++) {
		for (ClutBlock &b : clut_blocks) {
			for (int row = 0; row < CELL_H; row++) {
				for (int s = 0; s + slots <= 16; s += slots) {
					if ((b.rows[row] & (mask << s)) == 0) {
						b.rows[row] = (uint16_t)(b.rows[row] | (mask << s));
						r_x = b.x + s * 16;
						r_y = b.y + row;
						return true;
					}
				}
			}
		}
		if (pass == 0) {
			ClutBlock b;
			if (!alloc(256, CELL_H, b.x, b.y)) {
				return false;
			}
			clut_blocks.push_back(b);
		}
	}
	return false;
}

void VramAllocator::free_clut(int32_t p_x, int32_t p_y, int32_t p_entries) {
	const int32_t slots = p_entries <= 16 ? 1 : 16;
	const uint32_t mask = slots == 16 ? 0xFFFFu : 1u;
	for (size_t i = 0; i < clut_blocks.size(); i++) {
		ClutBlock &b = clut_blocks[i];
		if (p_x < b.x || p_x >= b.x + 256 || p_y < b.y || p_y >= b.y + CELL_H) {
			continue;
		}
		const int s = (p_x - b.x) / 16;
		b.rows[p_y - b.y] = (uint16_t)(b.rows[p_y - b.y] & ~(mask << s));
		bool empty = true;
		for (int row = 0; row < CELL_H; row++) {
			empty = empty && b.rows[row] == 0;
		}
		if (empty) {
			free(b.x, b.y, 256, CELL_H);
			clut_blocks.erase(clut_blocks.begin() + i);
		}
		return;
	}
}

uint32_t VramAllocator::free_cells() const {
	uint32_t n = 0;
	for (uint8_t c : cells) {
		n += c ? 0 : 1;
	}
	return n;
}

} // namespace psx
