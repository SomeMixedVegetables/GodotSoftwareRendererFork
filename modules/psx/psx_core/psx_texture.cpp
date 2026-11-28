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

// 4x4 Bayer matrix, 0..15.
static const uint8_t BAYER4[4][4] = {
	{ 0, 8, 2, 10 },
	{ 12, 4, 14, 6 },
	{ 3, 11, 1, 9 },
	{ 15, 7, 13, 5 },
};

// The dither offset at texel (x, y), in 8-bit units, for an amplitude of
// p_amp16 / 16 5-bit steps: centred on 0, within +-amplitude / 2.
static int32_t dither_offset(int32_t p_x, int32_t p_y, int32_t p_amp16) {
	const int32_t num = (2 * (int32_t)BAYER4[p_y & 3][p_x & 3] + 1 - 16) * p_amp16 * 255;
	const int32_t den = 32 * 31 * 16;
	return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

// An 8-bit channel plus a dither offset, to 5 bits.
static uint32_t to5_dithered(uint32_t p_value, int32_t p_offset) {
	const int32_t v = (int32_t)p_value + p_offset;
	return to5((uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v)));
}

static int32_t isqrt(int64_t p_value) {
	int64_t r = 0;
	while ((r + 1) * (r + 1) <= p_value) {
		r++;
	}
	return (int32_t)r;
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
	// A box's widest channel, its range and its pixel count are measured
	// once, when the box is made (a box's entries only change when it splits).
	struct Box {
		size_t begin, end;
		int range = -1, channel = 0;
		uint64_t count = 0;
	};
	auto make_box = [&](size_t p_begin, size_t p_end) {
		Box b;
		b.begin = p_begin;
		b.end = p_end;
		int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
		for (size_t i = p_begin; i < p_end; i++) {
			const uint16_t key = p_hist[i].key;
			for (int c = 0; c < 3; c++) {
				const int v = channel(key, c);
				lo[c] = v < lo[c] ? v : lo[c];
				hi[c] = v > hi[c] ? v : hi[c];
			}
			b.count += p_hist[i].count;
		}
		for (int c = 0; c < 3; c++) {
			if (hi[c] - lo[c] > b.range) {
				b.range = hi[c] - lo[c];
				b.channel = c;
			}
		}
		return b;
	};
	std::vector<Box> boxes = { make_box(0, p_hist.size()) };
	std::vector<uint64_t> sorted, sorted_tmp;
	while ((int32_t)boxes.size() < p_colors) {
		// Split the box with the widest channel range, weighted by pixel count.
		int64_t best_score = 0;
		int best_box = -1;
		for (size_t i = 0; i < boxes.size(); i++) {
			const Box &box = boxes[i];
			if (box.end - box.begin < 2) {
				continue;
			}
			const int64_t score = (int64_t)box.range * 1000000 + (int64_t)(box.count > 999999 ? 999999 : box.count);
			if (box.range > 0 && score > best_score) {
				best_score = score;
				best_box = (int)i;
			}
		}
		if (best_box < 0) {
			break;
		}
		const Box b = boxes[best_box];
		const int c = b.channel;
		// Order the box by the channel, then by key (keys are unique, so the
		// order is complete), packed as integers: sorted directly when the box
		// is small, else by a radix sort (key low byte, key high byte, then the
		// channel, each pass keeping the order of the one before).
		const size_t n = b.end - b.begin;
		sorted.resize(n);
		sorted_tmp.resize(n);
		for (size_t k = 0; k < n; k++) {
			const HistEntry &h = p_hist[b.begin + k];
			sorted[k] = ((uint64_t)channel(h.key, c) << 48) | ((uint64_t)h.key << 32) | h.count;
		}
		const bool few = n < 512;
		if (few) {
			std::sort(sorted.begin(), sorted.end());
		}
		for (int shift : { 32, 40, 48 }) {
			if (few) {
				break;
			}
			size_t start[257] = {};
			for (size_t k = 0; k < n; k++) {
				start[((sorted[k] >> shift) & 0xFF) + 1]++;
			}
			for (int d = 0; d < 256; d++) {
				start[d + 1] += start[d];
			}
			for (size_t k = 0; k < n; k++) {
				sorted_tmp[start[(sorted[k] >> shift) & 0xFF]++] = sorted[k];
			}
			sorted.swap(sorted_tmp);
		}
		for (size_t k = 0; k < n; k++) {
			const uint64_t v = sorted[k];
			p_hist[b.begin + k] = { (uint16_t)(v >> 32), (uint32_t)v };
		}
		const uint64_t total = b.count;
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
		boxes[best_box] = make_box(b.begin, split);
		boxes.push_back(make_box(split, b.end));
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
	const bool use_palette = p_options.palette && p_options.palette_size > 0;
	std::vector<uint32_t> texel(tw * th);
	// The resampled 8-bit colour (r | g << 8 | b << 16), for dithering.
	std::vector<uint32_t> color8(p_options.dither ? tw * th : 0);
	uint64_t mean_sum[3] = { 0, 0, 0 }, mean_count = 0;
	// The source columns each texel column covers, worked out once.
	std::vector<int32_t> span_x((size_t)tw + 1);
	for (int32_t tx = 0; tx <= tw; tx++) {
		span_x[tx] = (int32_t)((int64_t)tx * p_width / tw);
	}
	for (int32_t ty = 0; ty < th; ty++) {
		const int32_t y0 = (int32_t)((int64_t)ty * p_height / th);
		const int32_t y1 = std::max(y0 + 1, (int32_t)((int64_t)(ty + 1) * p_height / th));
		for (int32_t tx = 0; tx < tw; tx++) {
			const int32_t x0 = span_x[tx];
			const int32_t x1 = std::max(x0 + 1, span_x[tx + 1]);
			uint32_t c8[3];
			uint32_t a8;
			if (y1 - y0 == 1 && x1 - x0 == 1) {
				// One source texel (the image is already this size, or grows):
				// the averages below come out as the texel itself.
				const uint8_t *px = p_rgba + ((size_t)y0 * p_width + x0) * 4;
				c8[0] = px[0];
				c8[1] = px[1];
				c8[2] = px[2];
				a8 = use_alpha ? px[3] : 255;
			} else {
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
				for (int c = 0; c < 3; c++) {
					c8[c] = (uint32_t)(alpha > 0 ? (rgb[c] + alpha / 2) / alpha : (plain[c] + n / 2) / n);
				}
				a8 = (uint32_t)((alpha + n / 2) / n);
			}
			bool transparent = false, semi = false;
			switch (p_options.alpha_mode) {
				case ALPHA_CUT:
					transparent = a8 < (uint32_t)(p_options.alpha_cut * 255.0f + 0.5f);
					break;
				case ALPHA_BLEND:
					transparent = a8 <= 64;
					semi = !transparent && a8 < 191 && !use_palette;
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
			if (p_options.dither) {
				color8[ty * tw + tx] = c8[0] | (c8[1] << 8) | (c8[2] << 16);
			}
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

	// Histogram of visible colours, in key order. The table of counts for
	// every key is kept per thread and only the keys used are cleared
	// again, so a small texture does not pay for clearing and scanning all
	// 65536.
	thread_local std::vector<uint32_t> counts(65536, 0);
	std::vector<uint16_t> keys;
	for (uint32_t t : texel) {
		if (t != TRANSPARENT && counts[t]++ == 0) {
			keys.push_back((uint16_t)t);
		}
	}
	std::sort(keys.begin(), keys.end());
	std::vector<HistEntry> hist;
	hist.reserve(keys.size());
	for (uint16_t k : keys) {
		hist.push_back({ k, counts[k] });
		counts[k] = 0;
	}
	const int32_t reserved = (r_texture.has_transparent || use_palette) ? 1 : 0;
	const int32_t unique = (int32_t)hist.size();
	int32_t limit_colors = p_options.max_colors > 0 ? std::min(std::max(p_options.max_colors, 2), 256) : 256;

	uint8_t depth = TEXTURE_8BIT;
	switch (use_palette ? (p_options.palette_entries > 16 ? DEPTH_FORCE_8BIT : DEPTH_FORCE_4BIT) : p_options.depth) {
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

	// texel[i] dithered by p_amp16 / 16 5-bit steps (semi flag kept).
	auto dithered = [&](size_t p_i, int32_t p_amp16) -> uint32_t {
		const int32_t off = dither_offset((int32_t)(p_i % tw), (int32_t)(p_i / tw), p_amp16);
		const uint32_t c = color8[p_i];
		return to5_dithered(c & 0xFF, off) | (to5_dithered((c >> 8) & 0xFF, off) << 5) | (to5_dithered((c >> 16) & 0xFF, off) << 10) | (texel[p_i] & 0x8000u);
	};

	if (depth == TEXTURE_15BIT) {
		r_texture.texels.resize((size_t)tw * th);
		for (size_t i = 0; i < texel.size(); i++) {
			if (texel[i] == TRANSPARENT) {
				r_texture.texels[i] = 0;
				continue;
			}
			r_texture.texels[i] = texel_value(p_options.dither ? dithered(i, 16) : texel[i]);
		}
		r_texture.colors = unique;
		return true;
	}

	const int32_t entries = depth == TEXTURE_4BIT ? 16 : 256;
	const int32_t available = std::max(1, std::min(entries, limit_colors) - reserved);
	std::vector<uint16_t> palette;
	if (use_palette) {
		r_texture.shared_palette = true;
		for (int32_t i = 0; i < std::min(p_options.palette_size, entries - 1); i++) {
			palette.push_back((uint16_t)(p_options.palette[i] & 0x7FFF));
		}
	} else if (unique <= available) {
		for (const HistEntry &h : hist) {
			palette.push_back(h.key);
		}
	} else {
		palette = median_cut(hist, available);
	}
	r_texture.colors = (int32_t)palette.size() + reserved;
	const bool exact = !use_palette && unique <= available;

	// Nearest palette entry for a colour key (semi flag must match when the
	// palette has an entry of that kind), found once per key.
	// Kept per thread like counts: the keys found are set back to -1 at the end.
	thread_local std::vector<int16_t> lookup(65536, -1);
	std::vector<uint16_t> looked_up;
	// The result is the first palette entry (lowest index) at the smallest
	// distance. To find it without measuring every entry, the entries are
	// kept in order of red, and the search walks outwards from the key's red
	// until the red difference alone is further than the best so far.
	struct PaletteColor {
		int r, g, b;
		int index;
		bool semi;
	};
	std::vector<PaletteColor> by_red(palette.size());
	for (size_t p = 0; p < palette.size(); p++) {
		by_red[p] = { channel(palette[p], 0), channel(palette[p], 1), channel(palette[p], 2), (int)p, (palette[p] & 0x8000) != 0 };
	}
	std::stable_sort(by_red.begin(), by_red.end(), [](const PaletteColor &x, const PaletteColor &y) { return x.r < y.r; });
	auto nearest = [&](uint32_t p_key) -> uint32_t {
		if (lookup[p_key] >= 0) {
			return (uint32_t)lookup[p_key];
		}
		const bool semi = (p_key & 0x8000) != 0;
		const int kr = channel((uint16_t)p_key, 0), kg = channel((uint16_t)p_key, 1), kb = channel((uint16_t)p_key, 2);
		size_t start = 0;
		while (start < by_red.size() && by_red[start].r < kr) {
			start++;
		}
		int best = -1;
		int best_d = 1 << 30;
		// Pass 0 only looks at entries of the key's kind (semi or not);
		// pass 1, needed when there are none, at all of them.
		for (int pass = 0; pass < 2 && best < 0; pass++) {
			auto consider = [&](const PaletteColor &e) {
				if (pass == 0 && e.semi != semi) {
					return;
				}
				const int d = (kr - e.r) * (kr - e.r) + (kg - e.g) * (kg - e.g) + (kb - e.b) * (kb - e.b);
				if (d < best_d || (d == best_d && e.index < best)) {
					best_d = d;
					best = e.index;
				}
			};
			// Entries with an equal red difference can still tie, so the walk
			// stops only once that difference is strictly further.
			for (size_t i = start; i < by_red.size() && (by_red[i].r - kr) * (by_red[i].r - kr) <= best_d; i++) {
				consider(by_red[i]);
			}
			for (size_t i = start; i-- > 0 && (kr - by_red[i].r) * (kr - by_red[i].r) <= best_d;) {
				consider(by_red[i]);
			}
		}
		lookup[p_key] = (int16_t)(best + reserved);
		looked_up.push_back((uint16_t)p_key);
		return (uint32_t)lookup[p_key];
	};

	// Dither amplitude per channel (the offset moves every channel at once):
	// the mean distance from each palette colour to its nearest neighbour
	// over sqrt(3), so two greys a distance apart are bridged exactly; in
	// 1/16 5-bit steps, from one step to eight (a sparse palette would
	// otherwise speckle dark areas with its light colours).
	int32_t amp16 = 0;
	if (p_options.dither && !exact && palette.size() > 1) {
		int64_t sum = 0;
		for (size_t p = 0; p < palette.size(); p++) {
			int64_t best = INT64_MAX;
			for (size_t q = 0; q < palette.size(); q++) {
				if (q == p) {
					continue;
				}
				int64_t d = 0;
				for (int c = 0; c < 3; c++) {
					const int64_t diff = channel(palette[p], c) - channel(palette[q], c);
					d += diff * diff;
				}
				best = d < best ? d : best;
			}
			sum += isqrt(best * 256 / 3);
		}
		amp16 = std::min<int32_t>(128, std::max<int32_t>(16, (int32_t)(sum / (int64_t)palette.size())));
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
			const size_t i = (size_t)y * tw + x;
			const uint32_t t = texel[i];
			const uint32_t index = t == TRANSPARENT ? 0u : nearest(amp16 > 0 ? dithered(i, amp16) : t);
			r_texture.texels[(size_t)y * hw + x / per_hw] |= (uint16_t)(index << ((x % per_hw) * bits));
		}
	}
	for (uint16_t k : looked_up) {
		lookup[k] = -1;
	}
	return true;
}

bool make_palette(const PaletteSource *p_sources, int32_t p_count, int32_t p_colors, std::vector<uint16_t> &r_palette) {
	r_palette.clear();
	std::vector<uint32_t> counts(32768, 0);
	for (int32_t s = 0; s < p_count; s++) {
		const PaletteSource &src = p_sources[s];
		if (!src.rgba || src.width <= 0 || src.height <= 0) {
			continue;
		}
		const size_t n = (size_t)src.width * src.height;
		for (size_t i = 0; i < n; i++) {
			const uint8_t *px = src.rgba + i * 4;
			if (px[3] >= 128) {
				counts[to5(px[0]) | (to5(px[1]) << 5) | (to5(px[2]) << 10)]++;
			}
		}
	}
	std::vector<HistEntry> hist;
	for (uint32_t k = 0; k < 32768; k++) {
		if (counts[k]) {
			hist.push_back({ (uint16_t)k, counts[k] });
		}
	}
	if (hist.empty() || p_colors <= 0) {
		return false;
	}
	if ((int32_t)hist.size() <= p_colors) {
		for (const HistEntry &h : hist) {
			r_palette.push_back(h.key);
		}
	} else {
		r_palette = median_cut(hist, p_colors);
	}
	return !r_palette.empty();
}

bool apply_palette(const uint16_t *p_palette, int32_t p_count, TextureData &r_texture) {
	const int32_t entries = r_texture.clut_entries();
	if (!r_texture.shared_palette || !p_palette || p_count <= 0 || p_count > entries - 1) {
		return false;
	}
	r_texture.clut.assign(entries, 0);
	r_texture.clut_semi.assign(entries, 0);
	r_texture.clut_white.assign(entries, 0);
	for (int32_t i = 0; i < p_count; i++) {
		const uint16_t key = p_palette[i] & 0x7FFF;
		const uint16_t v = key == 0 ? (uint16_t)0x8000 : key; // Opaque black
		r_texture.clut[i + 1] = v;
		r_texture.clut_semi[i + 1] = (uint16_t)(v | 0x8000);
		r_texture.clut_white[i + 1] = 0xFFFF;
	}
	r_texture.colors = p_count + 1;
	return true;
}

namespace {

constexpr uint32_t BAKED_MAGIC = 0x54585350u; // "PSXT"
constexpr uint32_t BAKED_VERSION = 1;

struct ByteWriter {
	std::vector<uint8_t> &out;
	void u8(uint8_t p_v) { out.push_back(p_v); }
	void u32(uint32_t p_v) {
		for (int i = 0; i < 4; i++) {
			out.push_back((uint8_t)(p_v >> (i * 8)));
		}
	}
	void f32(float p_v) {
		uint32_t bits;
		std::memcpy(&bits, &p_v, 4);
		u32(bits);
	}
	void halfwords(const std::vector<uint16_t> &p_v) {
		u32((uint32_t)p_v.size());
		for (uint16_t h : p_v) {
			out.push_back((uint8_t)h);
			out.push_back((uint8_t)(h >> 8));
		}
	}
};

struct ByteReader {
	const uint8_t *data;
	size_t size;
	size_t pos = 0;
	bool ok = true;
	bool need(size_t p_n) {
		ok = ok && pos + p_n <= size;
		return ok;
	}
	uint8_t u8() { return need(1) ? data[pos++] : 0; }
	uint32_t u32() {
		if (!need(4)) {
			return 0;
		}
		const uint32_t v = (uint32_t)data[pos] | (uint32_t)data[pos + 1] << 8 | (uint32_t)data[pos + 2] << 16 | (uint32_t)data[pos + 3] << 24;
		pos += 4;
		return v;
	}
	float f32() {
		const uint32_t bits = u32();
		float v;
		std::memcpy(&v, &bits, 4);
		return v;
	}
	void halfwords(std::vector<uint16_t> &r_v, size_t p_max) {
		const uint32_t n = u32();
		if (n > p_max || !need((size_t)n * 2)) {
			ok = false;
			return;
		}
		r_v.resize(n);
		for (uint32_t i = 0; i < n; i++) {
			r_v[i] = (uint16_t)(data[pos] | data[pos + 1] << 8);
			pos += 2;
		}
	}
};

} // namespace

void write_baked_textures(const std::vector<BakedTexture> &p_textures, std::vector<uint8_t> &r_bytes) {
	ByteWriter w{ r_bytes };
	w.u32(BAKED_MAGIC);
	w.u32(BAKED_VERSION);
	w.u32((uint32_t)p_textures.size());
	for (const BakedTexture &b : p_textures) {
		const TextureData &t = b.data;
		w.u8(b.alpha_mode);
		w.f32(b.alpha_cut);
		w.u32((uint32_t)t.width);
		w.u32((uint32_t)t.height);
		w.u8(t.depth);
		w.u8((t.has_transparent ? 1 : 0) | (t.has_semi ? 2 : 0) | (t.shared_palette ? 4 : 0));
		w.u8(t.mean[0]);
		w.u8(t.mean[1]);
		w.u8(t.mean[2]);
		w.u32((uint32_t)t.colors);
		w.halfwords(t.clut);
		w.halfwords(t.clut_semi);
		w.halfwords(t.clut_white);
		w.halfwords(t.texels);
	}
}

bool read_baked_textures(const uint8_t *p_bytes, size_t p_size, std::vector<BakedTexture> &r_textures) {
	r_textures.clear();
	if (!p_bytes) {
		return false;
	}
	ByteReader r{ p_bytes, p_size };
	if (r.u32() != BAKED_MAGIC || r.u32() != BAKED_VERSION) {
		return false;
	}
	const uint32_t count = r.u32();
	if (!r.ok || count > 16) {
		return false;
	}
	for (uint32_t i = 0; i < count && r.ok; i++) {
		BakedTexture b;
		TextureData &t = b.data;
		b.alpha_mode = r.u8();
		b.alpha_cut = r.f32();
		t.width = (int32_t)r.u32();
		t.height = (int32_t)r.u32();
		t.depth = r.u8();
		const uint8_t flags = r.u8();
		t.has_transparent = (flags & 1) != 0;
		t.has_semi = (flags & 2) != 0;
		t.shared_palette = (flags & 4) != 0;
		t.mean[0] = r.u8();
		t.mean[1] = r.u8();
		t.mean[2] = r.u8();
		t.colors = (int32_t)r.u32();
		r.halfwords(t.clut, 256);
		r.halfwords(t.clut_semi, 256);
		r.halfwords(t.clut_white, 256);
		r.halfwords(t.texels, 256 * 256);
		// The texture must be one convert_texture could have made.
		const bool size_ok = t.width >= 8 && t.width <= 256 && t.height >= 8 && t.height <= 256 && (t.width & (t.width - 1)) == 0 && (t.height & (t.height - 1)) == 0;
		const bool depth_ok = t.depth == TEXTURE_4BIT || t.depth == TEXTURE_8BIT || t.depth == TEXTURE_15BIT;
		if (!r.ok || !size_ok || !depth_ok || b.alpha_mode > ALPHA_BLEND || t.texels.size() != (size_t)t.hw_width() * t.height || t.clut.size() != (size_t)t.clut_entries() || (!t.clut_semi.empty() && t.clut_semi.size() != t.clut.size()) || (!t.clut_white.empty() && t.clut_white.size() != t.clut.size())) {
			r_textures.clear();
			return false;
		}
		r_textures.push_back(std::move(b));
	}
	if (!r.ok) {
		r_textures.clear();
		return false;
	}
	return true;
}

void decode_texture(const TextureData &p_texture, bool p_semi_alpha, std::vector<uint8_t> &r_rgba) {
	const TextureData &t = p_texture;
	r_rgba.assign((size_t)t.width * t.height * 4, 0);
	if (t.texels.size() != (size_t)t.hw_width() * t.height) {
		return;
	}
	const int32_t per_hw = t.depth == TEXTURE_4BIT ? 4 : (t.depth == TEXTURE_8BIT ? 2 : 1);
	const int32_t bits = 16 / per_hw;
	const uint32_t mask = per_hw == 1 ? 0xFFFFu : (1u << bits) - 1;
	for (int32_t y = 0; y < t.height; y++) {
		for (int32_t x = 0; x < t.width; x++) {
			const uint16_t hw = t.texels[(size_t)y * t.hw_width() + x / per_hw];
			uint16_t c = hw;
			if (per_hw > 1) {
				const uint32_t index = (hw >> ((x % per_hw) * bits)) & mask;
				c = index < t.clut.size() ? t.clut[index] : 0;
			}
			uint8_t *px = &r_rgba[((size_t)y * t.width + x) * 4];
			if (c == 0) {
				continue; // Transparent
			}
			for (int ch = 0; ch < 3; ch++) {
				const uint32_t v = (c >> (ch * 5)) & 31;
				px[ch] = (uint8_t)((v << 3) | (v >> 2));
			}
			// Without semi texels, STP only marks opaque black (8000h).
			px[3] = (p_semi_alpha && t.has_semi && (c & 0x8000)) ? 128 : 255;
		}
	}
}

void resample_alpha(const uint8_t *p_rgba, int32_t p_width, int32_t p_height, int32_t p_tw, int32_t p_th, std::vector<uint8_t> &r_alpha) {
	r_alpha.assign((size_t)std::max(p_tw, 0) * std::max(p_th, 0), 255);
	if (!p_rgba || p_width <= 0 || p_height <= 0 || p_tw <= 0 || p_th <= 0) {
		return;
	}
	// The same spans and rounding as convert_texture's resample.
	for (int32_t ty = 0; ty < p_th; ty++) {
		const int32_t y0 = (int32_t)((int64_t)ty * p_height / p_th);
		const int32_t y1 = std::max(y0 + 1, (int32_t)((int64_t)(ty + 1) * p_height / p_th));
		for (int32_t tx = 0; tx < p_tw; tx++) {
			const int32_t x0 = (int32_t)((int64_t)tx * p_width / p_tw);
			const int32_t x1 = std::max(x0 + 1, (int32_t)((int64_t)(tx + 1) * p_width / p_tw));
			uint64_t alpha = 0, n = 0;
			for (int32_t y = y0; y < y1; y++) {
				for (int32_t x = x0; x < x1; x++) {
					alpha += p_rgba[((size_t)y * p_width + x) * 4 + 3];
					n++;
				}
			}
			r_alpha[(size_t)ty * p_tw + tx] = (uint8_t)((alpha + n / 2) / n);
		}
	}
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
