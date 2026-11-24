#include "psx_render.h"

#include "psx_convert.h"

#include <algorithm>
#include <cmath>

namespace psx {

void DrawStats::add(const DrawStats &p_other) {
	triangles_in += p_other.triangles_in;
	culled_near += p_other.culled_near;
	near_clips += p_other.near_clips;
	near_splits += p_other.near_splits;
	culled_far += p_other.culled_far;
	culled_backface += p_other.culled_backface;
	culled_offscreen += p_other.culled_offscreen;
	culled_alpha += p_other.culled_alpha;
	packets += p_other.packets;
	textured += p_other.textured;
	windowed += p_other.windowed;
	fog_passes += p_other.fog_passes;
	banked += p_other.banked;
	arena_full += p_other.arena_full;
}

// sRGB <-> linear tables for LIGHTING_LINEAR: 8-bit sRGB to 16-bit linear,
// 12-bit linear back to 8-bit sRGB, and 12-bit linear (up to 2.0) to a
// texture modulation colour (128 = 1.0).
struct LinearTables {
	uint16_t to_linear[256];
	uint8_t to_srgb[4097];
	uint8_t to_mod[8193];
	LinearTables() {
		for (int i = 0; i < 256; i++) {
			to_linear[i] = (uint16_t)std::lround(srgb_to_linear(i / 255.0f) * 65535.0f);
		}
		for (int i = 0; i <= 4096; i++) {
			to_srgb[i] = (uint8_t)std::lround(linear_to_srgb(i / 4096.0f) * 255.0f);
		}
		for (int i = 0; i <= 8192; i++) {
			const long m = std::lround(linear_to_srgb(i / 4096.0f) * 128.0f);
			to_mod[i] = (uint8_t)(m > 255 ? 255 : m);
		}
	}
};

static const LinearTables &linear_tables() {
	static const LinearTables tables; // Thread-safe one-time initialisation.
	return tables;
}

// Depth cue factor (IR0, 0..4096) for screen Z p_sz, as RTPS computes it.
static int16_t depth_cue(const GTE &p_gte, uint16_t p_sz) {
	bool overflow = false;
	const int64_t n = GTE::unr_divide(p_gte.h, p_sz, overflow);
	const int64_t i0 = (n * p_gte.dqa + p_gte.dqb) >> 12;
	return (int16_t)(i0 < 0 ? 0 : (i0 > 0x1000 ? 0x1000 : i0));
}

// Orthographic projection of V0-V2: MVMVA per vertex, then the screen offset
// and the SXY/SZ FIFOs written from the CPU (as game code could with MTC2).
// Returns a mask of the vertices nearer than the near plane (bit i = Vi).
static int project_ortho(GTE &p_gte, const DrawParams &p_params) {
	const int32_t lim_lo = -0x400 * p_gte.scale, lim_hi = 0x400 * p_gte.scale - 1;
	int near_mask = 0;
	for (int i = 0; i < 3; i++) {
		p_gte.mvmva_rt(i);
		const int32_t z = p_gte.mac[3];
		near_mask |= z < p_params.near_z ? 1 << i : 0;
		const int32_t x = p_gte.mac[1] + (p_gte.ofx >> 16);
		const int32_t y = p_gte.mac[2] + (p_gte.ofy >> 16);
		p_gte.sx[i] = (int16_t)(x < lim_lo ? lim_lo : (x > lim_hi ? lim_hi : x));
		p_gte.sy[i] = (int16_t)(y < lim_lo ? lim_lo : (y > lim_hi ? lim_hi : y));
		p_gte.sz[i + 1] = (uint16_t)(z < 0 ? 0 : (z > 0xFFFF ? 0xFFFF : z));
	}
	return near_mask;
}

// Light level (1.0 = 4096 per channel) a vertex gets from the point lights,
// with Godot's omni falloff and a hard spot cone.
static void point_light_level(const DrawParams &p_params, const SVector &p_pos, const SVector &p_normal, int32_t r_level[3]) {
	const float nx = p_normal.x / 4096.0f, ny = p_normal.y / 4096.0f, nz = p_normal.z / 4096.0f;
	float acc[3] = { 0, 0, 0 };
	for (int i = 0; i < p_params.point_light_count; i++) {
		const PointLight &l = p_params.point_lights[i];
		float dx = l.position[0] - p_pos.x * p_params.point_scale[0];
		float dy = l.position[1] - p_pos.y * p_params.point_scale[1];
		float dz = l.position[2] - p_pos.z * p_params.point_scale[2];
		const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
		const float dist = len * l.metres_per_unit;
		if (dist >= l.range || len <= 0.0f) {
			continue;
		}
		dx /= len;
		dy /= len;
		dz /= len;
		const float ndotl = nx * dx + ny * dy + nz * dz;
		if (ndotl <= 0.0f) {
			continue;
		}
		if (l.spot_cos > -1.5f && -(dx * l.spot_axis[0] + dy * l.spot_axis[1] + dz * l.spot_axis[2]) < l.spot_cos) {
			continue;
		}
		const float ratio = dist / l.range;
		const float window = 1.0f - ratio * ratio * ratio * ratio;
		const float atten = window * window * std::pow(dist > 0.0001f ? dist : 0.0001f, -l.decay) * ndotl;
		for (int c = 0; c < 3; c++) {
			acc[c] += l.color[c] * atten;
		}
	}
	for (int c = 0; c < 3; c++) {
		const float v = acc[c] * 4096.0f;
		r_level[c] = v > 32767.0f ? 32767 : (int32_t)v;
	}
}

static uint8_t add_saturate(uint8_t p_a, uint8_t p_b) {
	const int s = p_a + p_b;
	return (uint8_t)(s > 255 ? 255 : s);
}

DrawTexture make_draw_texture(const TextureData &p_texture, int32_t p_x, int32_t p_y, int32_t p_clut_x, int32_t p_clut_y, int32_t p_semi_x, int32_t p_semi_y, int32_t p_white_x, int32_t p_white_y, uint8_t p_blend, bool p_dither, uint16_t p_bank) {
	DrawTexture d;
	d.bank = p_bank;
	const int32_t page_x = p_x & ~63, page_y = p_y & 256;
	const int32_t per_hw = p_texture.depth == TEXTURE_4BIT ? 4 : (p_texture.depth == TEXTURE_8BIT ? 2 : 1);
	d.texpage = GP0Encoder::texpage(page_x, page_y, p_texture.depth, p_blend, p_dither);
	d.u_offset = (uint8_t)((p_x - page_x) * per_hw);
	d.v_offset = (uint8_t)(p_y - page_y);
	if (p_texture.depth != TEXTURE_15BIT) {
		d.clut = GP0Encoder::clut(p_clut_x, p_clut_y);
		d.has_clut_semi = p_semi_x >= 0;
		d.clut_semi = d.has_clut_semi ? GP0Encoder::clut(p_semi_x, p_semi_y) : d.clut;
		d.has_clut_white = p_white_x >= 0;
		d.clut_white = d.has_clut_white ? GP0Encoder::clut(p_white_x, p_white_y) : d.clut;
	}
	// Repeat inside the texture's own box: the mask clears the bits above
	// the texture size, the offset puts back its position in the page.
	const uint8_t mask_x = (uint8_t)(((256 - p_texture.width) >> 3) & 0x1F);
	const uint8_t mask_y = (uint8_t)(((256 - p_texture.height) >> 3) & 0x1F);
	d.window_word = GP0Encoder().texture_window(mask_x, mask_y, (uint8_t)(d.u_offset >> 3), (uint8_t)(d.v_offset >> 3));
	d.has_semi_texels = p_texture.has_semi;
	return d;
}

// Texture modulation colour for a 0..255 colour: 128 = 1.0.
static uint8_t to_modulation(uint8_t p_value) {
	return (uint8_t)((p_value * 128 + 127) / 255);
}

void draw_mesh(GTE &p_gte, PacketBuilder &p_builder, OrderingTable &p_ot, const MeshData &p_mesh, const DrawParams &p_params, DrawStats &r_stats) {
	const uint32_t tri_count = p_mesh.triangle_count();
	const bool lit = p_params.lit && !p_mesh.normals.empty();
	const bool linear = p_params.lighting == LIGHTING_LINEAR;
	const bool emissive = p_params.emission[0] | p_params.emission[1] | p_params.emission[2];
	const uint32_t mode_word = 0xE1000000u | GP0Encoder::texpage(0, 0, 0, p_params.blend, p_params.dither);
	const uint32_t window_off = 0xE2000000u;
	const uint32_t *idx = p_mesh.indices.data();
	const uint8_t *col = p_mesh.colors.data();
	const bool tinted = (p_params.tint[0] & p_params.tint[1] & p_params.tint[2]) != 255;
	// Textured: the colour modulates the texel instead of being drawn.
	const DrawTexture *tex = (p_params.texture && p_mesh.textured && p_mesh.tri_uvs.size() >= (size_t)tri_count * 6) ? p_params.texture : nullptr;
	const uint8_t *tri_uv = tex ? p_mesh.tri_uvs.data() : nullptr;
	const uint8_t *tri_window = (tex && !p_mesh.tri_window.empty()) ? p_mesh.tri_window.data() : nullptr;
	// Alpha-mix materials use the per-triangle classes; additive and
	// subtractive ones blend every triangle.
	const bool per_triangle_alpha = p_params.semi && p_params.blend == BLEND_HALF && !p_params.semi_whole;
	const uint8_t *tri_alpha = (per_triangle_alpha && !p_mesh.triangle_alpha.empty()) ? p_mesh.triangle_alpha.data() : nullptr;
	const LinearTables &lt = linear_tables();
	uint32_t emission_linear[3] = { 0, 0, 0 };
	uint8_t emission[3];
	for (int c = 0; c < 3; c++) {
		emission_linear[c] = lt.to_linear[p_params.emission[c]];
		emission[c] = tex ? to_modulation(p_params.emission[c]) : p_params.emission[c];
	}
	// Textured fog: DPCS toward black darkens the texel by (1 - f); the fog
	// colour times f is added by a second polygon.
	const LVector far_color = p_gte.far_color;
	if (tex) {
		p_gte.far_color = LVector();
	}
	const uint32_t add_mode_word = 0xE1000000u | GP0Encoder::texpage(0, 0, 0, BLEND_ADD, p_params.dither);
	// Dither other than the frame's: set before each triangle, restored after.
	const bool own_dither = p_params.dither != p_params.frame_dither;
	const uint32_t restore_mode_word = 0xE1000000u | GP0Encoder::texpage(0, 0, 0, BLEND_HALF, p_params.frame_dither);
	const uint16_t add_texpage = tex ? (uint16_t)((tex->texpage & ~(3u << 5)) | (BLEND_ADD << 5)) : 0;

	// Near plane: a triangle crossing it is clipped in object space (where
	// view Z is a linear function of the position, from RT and TR) into one
	// or two triangles, with normals, colours and UVs interpolated; one
	// wholly nearer is dropped. Neighbours share the cut points (each edge's
	// cut depends only on its two vertices), so no cracks open along it.
	// Pieces whose screen coordinates saturate, or that are too big for the
	// GPU (it refuses them), are split at their edge midpoints, as PS1 games
	// subdivided polygons near the camera, down to NEAR_SPLIT_DEPTH levels.
	const bool has_normals = !p_mesh.normals.empty();
	struct Corner {
		SVector pos;
		SVector normal;
		uint8_t color[3];
		int32_t u, v; // Texel coordinates * 256
	};
	struct Piece {
		Corner c[3];
		int depth;
		bool clipped; // Already cut at the near plane
	};
	// View Z of an object-space position, as RTPS/MVMVA compute MAC3.
	auto view_z = [&](const SVector &p_pos) -> int64_t {
		const Matrix &r = p_gte.rotation;
		return (int64_t)p_gte.translation.z + (((int64_t)r.m[2][0] * p_pos.x + (int64_t)r.m[2][1] * p_pos.y + (int64_t)r.m[2][2] * p_pos.z) >> 12);
	};
	// Vertices with view Z below near_limit are nearer than the near plane
	// (SZ <= H/2 overflows the divide); cuts are made at clip_z, a unit
	// further, so rounding the cut point stays in front.
	const int64_t near_limit = p_params.ortho ? p_params.near_z : (int64_t)p_gte.h / 2 + 1;
	const int64_t clip_z = near_limit + 1;
	auto lerp = [&](const Corner &p_a, const Corner &p_b, double p_t) {
		Corner m;
		auto mix = [&](double p_x, double p_y) { return p_x + (p_y - p_x) * p_t; };
		m.pos.x = (int16_t)std::lround(mix(p_a.pos.x, p_b.pos.x));
		m.pos.y = (int16_t)std::lround(mix(p_a.pos.y, p_b.pos.y));
		m.pos.z = (int16_t)std::lround(mix(p_a.pos.z, p_b.pos.z));
		if (has_normals) {
			const double nx = mix(p_a.normal.x, p_b.normal.x), ny = mix(p_a.normal.y, p_b.normal.y), nz = mix(p_a.normal.z, p_b.normal.z);
			const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
			const double k = len > 0.0 ? 4096.0 / len : 0.0;
			m.normal.x = (int16_t)std::lround(nx * k);
			m.normal.y = (int16_t)std::lround(ny * k);
			m.normal.z = (int16_t)std::lround(nz * k);
		}
		for (int ch = 0; ch < 3; ch++) {
			m.color[ch] = (uint8_t)std::lround(mix(p_a.color[ch], p_b.color[ch]));
		}
		m.u = (int32_t)std::lround(mix(p_a.u, p_b.u));
		m.v = (int32_t)std::lround(mix(p_a.v, p_b.v));
		return m;
	};
	const int32_t max_w = 1024 * p_gte.scale, max_h = 512 * p_gte.scale;
	Piece stack[3 * NEAR_SPLIT_DEPTH + 4];
	auto midpoint = [&](const Corner &p_a, const Corner &p_b) {
		Corner m;
		m.pos.x = (int16_t)(((int32_t)p_a.pos.x + p_b.pos.x) >> 1);
		m.pos.y = (int16_t)(((int32_t)p_a.pos.y + p_b.pos.y) >> 1);
		m.pos.z = (int16_t)(((int32_t)p_a.pos.z + p_b.pos.z) >> 1);
		if (has_normals) {
			const float nx = (float)p_a.normal.x + p_b.normal.x, ny = (float)p_a.normal.y + p_b.normal.y, nz = (float)p_a.normal.z + p_b.normal.z;
			const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
			const float k = len > 0.0f ? 4096.0f / len : 0.0f;
			m.normal.x = (int16_t)std::lround(nx * k);
			m.normal.y = (int16_t)std::lround(ny * k);
			m.normal.z = (int16_t)std::lround(nz * k);
		}
		for (int ch = 0; ch < 3; ch++) {
			m.color[ch] = (uint8_t)((p_a.color[ch] + p_b.color[ch] + 1) >> 1);
		}
		m.u = (p_a.u + p_b.u) >> 1;
		m.v = (p_a.v + p_b.v) >> 1;
		return m;
	};
	// Four pieces with the parent's winding.
	auto split = [&](const Piece &p_piece, int &r_sp) {
		const Corner &a = p_piece.c[0], &b = p_piece.c[1], &c = p_piece.c[2];
		const Corner ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
		const Corner pieces[4][3] = { { a, ab, ca }, { ab, b, bc }, { ca, bc, c }, { ab, bc, ca } };
		for (int k = 0; k < 4; k++) {
			Piece &dst = stack[r_sp++];
			dst.c[0] = pieces[k][0];
			dst.c[1] = pieces[k][1];
			dst.c[2] = pieces[k][2];
			dst.depth = p_piece.depth + 1;
			dst.clipped = p_piece.clipped;
		}
	};
	bool stop = false;

	for (uint32_t t = 0; t < tri_count && !stop; t++) {
		r_stats.triangles_in++;
		bool tri_semi = p_params.semi && !per_triangle_alpha;
		if (tri_alpha) {
			if (tri_alpha[t] == TRI_HIDDEN) {
				r_stats.culled_alpha++;
				continue;
			}
			tri_semi = tri_alpha[t] == TRI_HALF;
		}
		const uint8_t *uv = tri_uv ? tri_uv + (size_t)t * 6 : nullptr;
		for (int i = 0; i < 3; i++) {
			const uint32_t id = idx[t * 3 + i];
			Corner &k = stack[0].c[i];
			k.pos = p_mesh.positions[id];
			k.normal = has_normals ? p_mesh.normals[id] : SVector();
			const uint8_t *c = col + id * 3;
			for (int ch = 0; ch < 3; ch++) {
				k.color[ch] = tinted ? (uint8_t)((c[ch] * p_params.tint[ch] + 127) / 255) : c[ch];
			}
			k.u = uv ? (int32_t)uv[i * 2] << 8 : 0;
			k.v = uv ? (int32_t)uv[i * 2 + 1] << 8 : 0;
		}
		stack[0].depth = 0;
		stack[0].clipped = false;
		int sp = 1;
		while (sp > 0) {
			const Piece piece = stack[--sp];
			bool semi = tri_semi;
			if (!piece.clipped) {
				int64_t z[3];
				int inside = 0;
				for (int i = 0; i < 3; i++) {
					z[i] = view_z(piece.c[i].pos);
					inside += z[i] >= near_limit ? 1 : 0;
				}
				if (inside == 0) {
					r_stats.culled_near++;
					continue;
				}
				if (inside < 3) {
					// Sutherland-Hodgman against Z = clip_z, keeping the winding.
					Corner poly[4];
					int n = 0;
					for (int i = 0; i < 3; i++) {
						const int j = (i + 1) % 3;
						const bool in_i = z[i] >= near_limit, in_j = z[j] >= near_limit;
						if (in_i) {
							poly[n++] = piece.c[i];
						}
						if (in_i != in_j) {
							// Cut from the inside vertex toward the outside one.
							const int a = in_i ? i : j, b = in_i ? j : i;
							const double cut = std::clamp((double)(z[a] - clip_z) / (double)(z[a] - z[b]), 0.0, 1.0);
							poly[n++] = lerp(piece.c[a], piece.c[b], cut);
						}
					}
					for (int k = n - 1; k >= 2; k--) { // Fan: (0, k - 1, k)
						Piece &dst = stack[sp++];
						dst.c[0] = poly[0];
						dst.c[1] = poly[k - 1];
						dst.c[2] = poly[k];
						dst.depth = piece.depth;
						dst.clipped = true;
					}
					r_stats.near_clips++;
					continue;
				}
			}
			p_gte.v[0] = piece.c[0].pos;
			p_gte.v[1] = piece.c[1].pos;
			p_gte.v[2] = piece.c[2].pos;
			int near_mask = 0;
			bool saturated = false;
			if (p_params.ortho) {
				near_mask = project_ortho(p_gte, p_params);
			} else {
				p_gte.rtpt();
				// A vertex nearer than H/2 overflows the divide.
				if (p_gte.flag & FLAG_DIVIDE) {
					for (int i = 0; i < 3; i++) {
						near_mask |= (uint32_t)p_gte.h >= (uint32_t)p_gte.sz[i + 1] * 2 ? 1 << i : 0;
					}
					near_mask = near_mask ? near_mask : 7;
				}
				saturated = (p_gte.flag & (FLAG_SX_SAT | FLAG_SY_SAT)) != 0;
			}
			if (near_mask) {
				// Still nearer after the cut (rounding): dropped.
				r_stats.culled_near++;
				continue;
			}
			const uint16_t zs[3] = { p_gte.sz[1], p_gte.sz[2], p_gte.sz[3] };
			if (zs[0] > p_params.far_z && zs[1] > p_params.far_z && zs[2] > p_params.far_z) {
				r_stats.culled_far++;
				continue;
			}

			// Back-face culling from the screen-space winding (positive = front).
			if (p_params.cull != CULL_NONE) {
				p_gte.nclip();
				const int32_t area = p_gte.mac[0];
				if (area == 0 || (p_params.cull == CULL_BACK ? area < 0 : area > 0)) {
					r_stats.culled_backface++;
					continue;
				}
			}

			const int32_t x0 = p_gte.sx[0], x1 = p_gte.sx[1], x2 = p_gte.sx[2];
			const int32_t y0 = p_gte.sy[0], y1 = p_gte.sy[1], y2 = p_gte.sy[2];
			if ((x0 < 0 && x1 < 0 && x2 < 0) || (y0 < 0 && y1 < 0 && y2 < 0) ||
					(x0 >= p_params.screen_width && x1 >= p_params.screen_width && x2 >= p_params.screen_width) ||
					(y0 >= p_params.screen_height && y1 >= p_params.screen_height && y2 >= p_params.screen_height)) {
				r_stats.culled_offscreen++;
				continue;
			}
			if (piece.depth < NEAR_SPLIT_DEPTH) {
				const int32_t w = std::max(x0, std::max(x1, x2)) - std::min(x0, std::min(x1, x2));
				const int32_t tall = std::max(y0, std::max(y1, y2)) - std::min(y0, std::min(y1, y2));
				if (saturated || w >= max_w || tall >= max_h) {
					split(piece, sp);
					r_stats.near_splits++;
					continue;
				}
			}

			p_gte.avsz3();
			const uint32_t otz = (uint32_t)std::clamp((int32_t)((uint32_t)p_gte.otz >> p_params.ot_shift) + p_params.ot_bias, 0, (int32_t)p_ot.get_size() - 1);

			PacketBuilder::Vert verts[3];
			int16_t fog_amount[3] = { 0, 0, 0 }; // IR0 per vertex, 4096 = full fog
			const int32_t xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
			for (int i = 0; i < 3; i++) {
				const uint8_t *c = piece.c[i].color;
				verts[i].x = xs[i];
				verts[i].y = ys[i];
				const uint8_t base[3] = { tex ? to_modulation(c[0]) : c[0], tex ? to_modulation(c[1]) : c[1], tex ? to_modulation(c[2]) : c[2] };
				uint8_t out[3] = { base[0], base[1], base[2] };
				// Linear 12-bit (1.0 = 4096) back to a drawn or modulation colour.
				auto from_linear = [&](uint64_t p_v) -> uint8_t {
					return tex ? lt.to_mod[p_v > 8192 ? 8192 : p_v] : lt.to_srgb[p_v > 4096 ? 4096 : p_v];
				};
				if (lit && p_params.point_light_count > 0) {
					// Directional + ambient from the GTE (NCS), point lights from
					// the CPU, then the colour maths of the chosen mode.
					p_gte.v[0] = piece.c[i].normal;
					p_gte.ncs();
					int32_t level[3];
					point_light_level(p_params, piece.c[i].pos, piece.c[i].normal, level);
					for (int ch = 0; ch < 3; ch++) {
						int32_t l = p_gte.ir[ch + 1] + level[ch];
						l = l > 0x7FFF ? 0x7FFF : l;
						if (linear) {
							uint64_t v = ((uint64_t)lt.to_linear[c[ch]] * (uint32_t)l) >> 12;
							v += emission_linear[ch];
							out[ch] = from_linear(v >> 4);
						} else {
							const int32_t v = (base[ch] * l) >> 12;
							out[ch] = (uint8_t)(v > 255 ? 255 : v);
						}
					}
				} else if (lit && !linear) {
					// PS1: the display colour times the light, on the GTE.
					p_gte.v[0] = piece.c[i].normal;
					p_gte.rgbc.r = base[0];
					p_gte.rgbc.g = base[1];
					p_gte.rgbc.b = base[2];
					p_gte.nccs();
					out[0] = p_gte.rgb[2].r;
					out[1] = p_gte.rgb[2].g;
					out[2] = p_gte.rgb[2].b;
				} else if (lit) {
					// Godot match: the light level from the GTE, applied to the
					// linear colour, plus linear emission, back to sRGB.
					p_gte.v[0] = piece.c[i].normal;
					p_gte.ncs();
					for (int ch = 0; ch < 3; ch++) {
						uint64_t v = ((uint64_t)lt.to_linear[c[ch]] * (uint16_t)p_gte.ir[ch + 1]) >> 12;
						v += emission_linear[ch];
						out[ch] = from_linear(v >> 4); // 16-bit to 12-bit linear
					}
				}
				if (emissive && !(lit && linear)) {
					for (int ch = 0; ch < 3; ch++) {
						out[ch] = add_saturate(out[ch], emission[ch]);
					}
				}
				if (p_params.fog) {
					// Depth cue toward FC by this vertex's own IR0.
					p_gte.rgbc.r = out[0];
					p_gte.rgbc.g = out[1];
					p_gte.rgbc.b = out[2];
					p_gte.ir[0] = depth_cue(p_gte, zs[i]);
					fog_amount[i] = p_gte.ir[0];
					p_gte.dpcs();
					out[0] = p_gte.rgb[2].r;
					out[1] = p_gte.rgb[2].g;
					out[2] = p_gte.rgb[2].b;
				}
				verts[i].r = out[0];
				verts[i].g = out[1];
				verts[i].b = out[2];
			}

			PacketBuilder::Flags flags;
			flags.shaded = !(verts[0].r == verts[1].r && verts[0].g == verts[1].g && verts[0].b == verts[1].b &&
					verts[0].r == verts[2].r && verts[0].g == verts[2].g && verts[0].b == verts[2].b);
			uint16_t clut = 0, texpage = 0;
			bool windowed = false;
			if (tex) {
				flags.textured = true;
				// A whole-polygon blend (50% triangle, additive, subtractive)
				// needs every texel to blend: the all-STP palette. An opaque
				// triangle of a blended material still blends its STP texels.
				const bool whole = semi;
				if (!semi && per_triangle_alpha && tex->has_semi_texels) {
					semi = true;
				}
				clut = (whole && tex->has_clut_semi) ? tex->clut_semi : tex->clut;
				texpage = tex->texpage;
				windowed = tri_window && tri_window[t];
				for (int i = 0; i < 3; i++) {
					const uint8_t u = (uint8_t)((piece.c[i].u + 128) >> 8);
					const uint8_t v = (uint8_t)((piece.c[i].v + 128) >> 8);
					verts[i].u = windowed ? u : (uint8_t)(tex->u_offset + u);
					verts[i].v = windowed ? v : (uint8_t)(tex->v_offset + v);
				}
			}
			flags.semi = semi;

			// Fog pass over a textured triangle: FC * f, added. Cut-out textures
			// draw it textured with the all-white palette so holes stay empty.
			const bool fog_pass = tex && p_params.fog && (fog_amount[0] | fog_amount[1] | fog_amount[2]);
			const bool fog_textured = fog_pass && tex->has_clut_white;
			PacketBuilder::Vert fog_verts[3];
			if (fog_pass) {
				const int32_t fc[3] = { far_color.x >> 4, far_color.y >> 4, far_color.z >> 4 };
				for (int i = 0; i < 3; i++) {
					fog_verts[i] = verts[i];
					uint8_t *dst[3] = { &fog_verts[i].r, &fog_verts[i].g, &fog_verts[i].b };
					for (int ch = 0; ch < 3; ch++) {
						int32_t v = (fc[ch] * fog_amount[i]) >> 12;
						v = fog_textured ? (v * 128 + 127) / 255 : v;
						*dst[ch] = (uint8_t)(v > 255 ? 255 : (v < 0 ? 0 : v));
					}
				}
			}

			// Packets in drawing order; added to the bucket in reverse (it is LIFO).
			uint32_t order[12];
			int count = 0;
			bool full = false;
			auto push = [&](uint32_t p_packet) {
				if (p_packet == OT_END) {
					full = true;
				} else {
					order[count++] = p_packet;
				}
			};
			// Texture bank: selected for this triangle, back to VRAM after it.
			const bool banked = tex && tex->bank != 0;
			const uint32_t bank_on = banked ? GP0Encoder::texture_bank(tex->bank) : 0u;
			const uint32_t bank_off = GP0Encoder::texture_bank(0);
			if (banked) {
				push(p_builder.raw_words(&bank_on, 1));
			}
			if (windowed) {
				push(p_builder.raw_words(&tex->window_word, 1));
			}
			if ((semi && !tex) || own_dither) {
				// Untextured polygons blend with the current draw mode; it also
				// carries the dither.
				push(p_builder.raw_words(&mode_word, 1));
			}
			push(p_builder.polygon(verts, 3, flags, clut, texpage));
			if (fog_textured) {
				PacketBuilder::Flags ff = flags;
				ff.semi = true;
				ff.shaded = true;
				push(p_builder.polygon(fog_verts, 3, ff, tex->clut_white, add_texpage));
			}
			if (windowed) {
				push(p_builder.raw_words(&window_off, 1));
			}
			if (banked) {
				push(p_builder.raw_words(&bank_off, 1));
			}
			if (fog_pass && !fog_textured) {
				PacketBuilder::Flags ff;
				ff.semi = true;
				ff.shaded = true;
				push(p_builder.raw_words(&add_mode_word, 1));
				push(p_builder.polygon(fog_verts, 3, ff));
			}
			if (own_dither) {
				push(p_builder.raw_words(&restore_mode_word, 1));
			}
			if (full) {
				r_stats.arena_full++;
				stop = true;
				break;
			}
			for (int k = count - 1; k >= 0; k--) {
				p_ot.add(otz, order[k]);
			}
			r_stats.packets += count;
			if (tex) {
				r_stats.textured++;
				r_stats.windowed += windowed ? 1 : 0;
				r_stats.fog_passes += fog_pass ? 1 : 0;
				r_stats.banked += banked ? 1 : 0;
			}
		}
	}
	p_gte.far_color = far_color;
}

} // namespace psx
