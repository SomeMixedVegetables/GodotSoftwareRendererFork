#include "psx_render.h"

#include "psx_convert.h"

#include <cmath>

namespace psx {

void DrawStats::add(const DrawStats &p_other) {
	triangles_in += p_other.triangles_in;
	culled_near += p_other.culled_near;
	culled_far += p_other.culled_far;
	culled_backface += p_other.culled_backface;
	culled_offscreen += p_other.culled_offscreen;
	culled_alpha += p_other.culled_alpha;
	packets += p_other.packets;
	arena_full += p_other.arena_full;
}

// sRGB <-> linear tables for LIGHTING_LINEAR: 8-bit sRGB to 16-bit linear,
// and 12-bit linear back to 8-bit sRGB.
struct LinearTables {
	uint16_t to_linear[256];
	uint8_t to_srgb[4097];
	LinearTables() {
		for (int i = 0; i < 256; i++) {
			to_linear[i] = (uint16_t)std::lround(srgb_to_linear(i / 255.0f) * 65535.0f);
		}
		for (int i = 0; i <= 4096; i++) {
			to_srgb[i] = (uint8_t)std::lround(linear_to_srgb(i / 4096.0f) * 255.0f);
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
// Returns false when a vertex is nearer than the near plane.
static bool project_ortho(GTE &p_gte, const DrawParams &p_params) {
	const int32_t lim_lo = -0x400 * p_gte.scale, lim_hi = 0x400 * p_gte.scale - 1;
	bool ok = true;
	for (int i = 0; i < 3; i++) {
		p_gte.mvmva_rt(i);
		const int32_t z = p_gte.mac[3];
		ok = ok && z >= p_params.near_z;
		const int32_t x = p_gte.mac[1] + (p_gte.ofx >> 16);
		const int32_t y = p_gte.mac[2] + (p_gte.ofy >> 16);
		p_gte.sx[i] = (int16_t)(x < lim_lo ? lim_lo : (x > lim_hi ? lim_hi : x));
		p_gte.sy[i] = (int16_t)(y < lim_lo ? lim_lo : (y > lim_hi ? lim_hi : y));
		p_gte.sz[i + 1] = (uint16_t)(z < 0 ? 0 : (z > 0xFFFF ? 0xFFFF : z));
	}
	return ok;
}

// Light level (1.0 = 4096 per channel) a vertex gets from the point lights,
// with Godot's omni falloff and a hard spot cone.
static void point_light_level(const DrawParams &p_params, const SVector &p_pos, const SVector &p_normal, int32_t r_level[3]) {
	const float nx = p_normal.x / 4096.0f, ny = p_normal.y / 4096.0f, nz = p_normal.z / 4096.0f;
	float acc[3] = { 0, 0, 0 };
	for (int i = 0; i < p_params.point_light_count; i++) {
		const PointLight &l = p_params.point_lights[i];
		float dx = l.position[0] - p_pos.x, dy = l.position[1] - p_pos.y, dz = l.position[2] - p_pos.z;
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

void draw_mesh(GTE &p_gte, PacketBuilder &p_builder, OrderingTable &p_ot, const MeshData &p_mesh, const DrawParams &p_params, DrawStats &r_stats) {
	const uint32_t tri_count = p_mesh.triangle_count();
	const bool lit = p_params.lit && !p_mesh.normals.empty();
	const bool linear = p_params.lighting == LIGHTING_LINEAR;
	const bool emissive = p_params.emission[0] | p_params.emission[1] | p_params.emission[2];
	const uint32_t mode_word = 0xE1000000u | GP0Encoder::texpage(0, 0, 0, p_params.blend, p_params.dither);
	const uint32_t *idx = p_mesh.indices.data();
	const uint8_t *col = p_mesh.colors.data();
	// Alpha-mix materials use the per-triangle classes; additive and
	// subtractive ones blend every triangle.
	const bool per_triangle_alpha = p_params.semi && p_params.blend == BLEND_HALF;
	const uint8_t *tri_alpha = (per_triangle_alpha && !p_mesh.triangle_alpha.empty()) ? p_mesh.triangle_alpha.data() : nullptr;
	const LinearTables &lt = linear_tables();
	uint32_t emission_linear[3] = { 0, 0, 0 };
	for (int c = 0; c < 3; c++) {
		emission_linear[c] = lt.to_linear[p_params.emission[c]];
	}

	for (uint32_t t = 0; t < tri_count; t++) {
		r_stats.triangles_in++;
		bool semi = p_params.semi && !per_triangle_alpha;
		if (tri_alpha) {
			if (tri_alpha[t] == TRI_HIDDEN) {
				r_stats.culled_alpha++;
				continue;
			}
			semi = tri_alpha[t] == TRI_HALF;
		}
		const uint32_t i0 = idx[t * 3], i1 = idx[t * 3 + 1], i2 = idx[t * 3 + 2];
		p_gte.v[0] = p_mesh.positions[i0];
		p_gte.v[1] = p_mesh.positions[i1];
		p_gte.v[2] = p_mesh.positions[i2];
		if (p_params.ortho) {
			if (!project_ortho(p_gte, p_params)) {
				r_stats.culled_near++;
				continue;
			}
		} else {
			p_gte.rtpt();
			// Near plane: a vertex nearer than H/2 overflows the divide.
			if (p_gte.flag & FLAG_DIVIDE) {
				r_stats.culled_near++;
				continue;
			}
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

		p_gte.avsz3();
		const uint32_t otz = (uint32_t)p_gte.otz >> p_params.ot_shift;

		PacketBuilder::Vert verts[3];
		const int32_t xs[3] = { x0, x1, x2 }, ys[3] = { y0, y1, y2 };
		const uint32_t ids[3] = { i0, i1, i2 };
		for (int i = 0; i < 3; i++) {
			const uint8_t *c = col + ids[i] * 3;
			verts[i].x = xs[i];
			verts[i].y = ys[i];
			uint8_t out[3] = { c[0], c[1], c[2] };
			if (lit && p_params.point_light_count > 0) {
				// Directional + ambient from the GTE (NCS), point lights from
				// the CPU, then the colour maths of the chosen mode.
				p_gte.v[0] = p_mesh.normals[ids[i]];
				p_gte.ncs();
				int32_t level[3];
				point_light_level(p_params, p_mesh.positions[ids[i]], p_mesh.normals[ids[i]], level);
				for (int ch = 0; ch < 3; ch++) {
					int32_t l = p_gte.ir[ch + 1] + level[ch];
					l = l > 0x7FFF ? 0x7FFF : l;
					if (linear) {
						uint64_t v = ((uint64_t)lt.to_linear[c[ch]] * (uint32_t)l) >> 12;
						v += emission_linear[ch];
						v >>= 4;
						out[ch] = lt.to_srgb[v > 4096 ? 4096 : v];
					} else {
						const int32_t v = (c[ch] * l) >> 12;
						out[ch] = (uint8_t)(v > 255 ? 255 : v);
					}
				}
			} else if (lit && !linear) {
				// PS1: the display colour times the light, on the GTE.
				p_gte.v[0] = p_mesh.normals[ids[i]];
				p_gte.rgbc.r = c[0];
				p_gte.rgbc.g = c[1];
				p_gte.rgbc.b = c[2];
				p_gte.nccs();
				out[0] = p_gte.rgb[2].r;
				out[1] = p_gte.rgb[2].g;
				out[2] = p_gte.rgb[2].b;
			} else if (lit) {
				// Godot match: the light level from the GTE, applied to the
				// linear colour, plus linear emission, back to sRGB.
				p_gte.v[0] = p_mesh.normals[ids[i]];
				p_gte.ncs();
				for (int ch = 0; ch < 3; ch++) {
					uint64_t v = ((uint64_t)lt.to_linear[c[ch]] * (uint16_t)p_gte.ir[ch + 1]) >> 12;
					v += emission_linear[ch];
					v >>= 4; // 16-bit to 12-bit linear
					out[ch] = lt.to_srgb[v > 4096 ? 4096 : v];
				}
			}
			if (emissive && !(lit && linear)) {
				for (int ch = 0; ch < 3; ch++) {
					out[ch] = add_saturate(out[ch], p_params.emission[ch]);
				}
			}
			if (p_params.fog) {
				// Depth cue toward FC by this vertex's own IR0.
				p_gte.rgbc.r = out[0];
				p_gte.rgbc.g = out[1];
				p_gte.rgbc.b = out[2];
				p_gte.ir[0] = depth_cue(p_gte, zs[i]);
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
		flags.semi = semi;
		const uint32_t packet = p_builder.polygon(verts, 3, flags);
		if (packet == OT_END) {
			r_stats.arena_full++;
			return;
		}
		p_ot.add(otz, packet);
		r_stats.packets++;
		if (semi) {
			// Untextured polygons blend with the current draw mode, so set
			// it just before this one (the bucket is LIFO: added after, drawn before).
			const uint32_t mode = p_builder.raw_words(&mode_word, 1);
			if (mode != OT_END) {
				p_ot.add(otz, mode);
				r_stats.packets++;
			}
		}
	}
}

} // namespace psx
