#include "psx_convert.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace psx {

float srgb_to_linear(float p_value) {
	return p_value <= 0.04045f ? p_value / 12.92f : std::pow((p_value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float p_value) {
	return p_value <= 0.0031308f ? p_value * 12.92f : 1.055f * std::pow(p_value, 1.0f / 2.4f) - 0.055f;
}

static uint8_t unit_to_byte(float p_value) {
	const float c = p_value < 0.0f ? 0.0f : (p_value > 1.0f ? 1.0f : p_value);
	return (uint8_t)std::lround(c * 255.0f);
}

static int16_t to_int16(double p_value, bool &r_clamped) {
	const long v = std::lround(p_value);
	if (v < -32768 || v > 32767) {
		r_clamped = true;
		return (int16_t)(v < 0 ? -32768 : 32767);
	}
	return (int16_t)v;
}

// Conforming midpoint subdivision. Each pass splits every edge longer than
// the limit at its midpoint (one shared vertex per edge, so neighbours stay
// crack-free) and re-triangulates each triangle by how many of its edges were
// split (1 -> 2, 2 -> 3, 3 -> 4 triangles), keeping the winding.
struct Subdivided {
	std::vector<float> positions, normals, colors, uvs;
	std::vector<int32_t> indices;
	MeshInput input;
};

static bool subdivide(const MeshInput &p_input, const ConvertOptions &p_options, Subdivided &r_out) {
	const int32_t n = p_input.vertex_count;
	const double limit = p_options.max_edge / p_options.scale; // In input units.
	const double limit2 = limit * limit;
	auto edge_long = [&](const float *p_pos, int32_t a, int32_t b) {
		const double dx = p_pos[a * 3] - p_pos[b * 3], dy = p_pos[a * 3 + 1] - p_pos[b * 3 + 1], dz = p_pos[a * 3 + 2] - p_pos[b * 3 + 2];
		return dx * dx + dy * dy + dz * dz > limit2;
	};
	// Anything to do at all?
	std::vector<int32_t> tris;
	if (p_input.indices) {
		tris.assign(p_input.indices, p_input.indices + (p_input.index_count / 3) * 3);
	} else {
		for (int32_t i = 0; i + 2 < n; i += 3) {
			tris.push_back(i);
			tris.push_back(i + 1);
			tris.push_back(i + 2);
		}
	}
	bool any = false;
	for (size_t t = 0; t + 2 < tris.size() && !any; t += 3) {
		const int32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
		if (a < 0 || b < 0 || c < 0 || a >= n || b >= n || c >= n) {
			continue;
		}
		any = edge_long(p_input.positions, a, b) || edge_long(p_input.positions, b, c) || edge_long(p_input.positions, c, a);
	}
	if (!any) {
		return false;
	}

	r_out.positions.assign(p_input.positions, p_input.positions + n * 3);
	if (p_input.normals) {
		r_out.normals.assign(p_input.normals, p_input.normals + n * 3);
	}
	if (p_input.colors) {
		r_out.colors.assign(p_input.colors, p_input.colors + n * 4);
	}
	if (p_input.uvs) {
		r_out.uvs.assign(p_input.uvs, p_input.uvs + n * 2);
	}
	auto lerp_attr = [](std::vector<float> &r_v, int p_width, int32_t a, int32_t b) {
		if (r_v.empty()) {
			return;
		}
		for (int k = 0; k < p_width; k++) {
			r_v.push_back(0.5f * (r_v[a * p_width + k] + r_v[b * p_width + k]));
		}
	};

	for (int pass = 0; pass < 16; pass++) {
		std::unordered_map<uint64_t, int32_t> mids;
		std::vector<int32_t> next;
		next.reserve(tris.size() * 2);
		bool split_any = false;
		auto midpoint = [&](int32_t a, int32_t b) -> int32_t {
			const uint64_t key = a < b ? ((uint64_t)a << 32 | (uint32_t)b) : ((uint64_t)b << 32 | (uint32_t)a);
			auto it = mids.find(key);
			if (it != mids.end()) {
				return it->second;
			}
			const int32_t m = (int32_t)(r_out.positions.size() / 3);
			lerp_attr(r_out.positions, 3, a, b);
			lerp_attr(r_out.normals, 3, a, b);
			lerp_attr(r_out.colors, 4, a, b);
			lerp_attr(r_out.uvs, 2, a, b);
			mids.emplace(key, m);
			return m;
		};
		const int32_t vcount = (int32_t)(r_out.positions.size() / 3);
		// First mark every long edge, so both triangles sharing it split it.
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			const int32_t v[3] = { tris[t], tris[t + 1], tris[t + 2] };
			if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[0] >= vcount || v[1] >= vcount || v[2] >= vcount) {
				continue;
			}
			for (int e = 0; e < 3; e++) {
				if (edge_long(r_out.positions.data(), v[e], v[(e + 1) % 3])) {
					midpoint(v[e], v[(e + 1) % 3]);
					split_any = true;
				}
			}
		}
		if (!split_any || tris.size() / 3 >= p_options.max_triangles) {
			break;
		}
		auto find_mid = [&](int32_t a, int32_t b) -> int32_t {
			const uint64_t key = a < b ? ((uint64_t)a << 32 | (uint32_t)b) : ((uint64_t)b << 32 | (uint32_t)a);
			auto it = mids.find(key);
			return it == mids.end() ? -1 : it->second;
		};
		auto emit = [&](int32_t a, int32_t b, int32_t c) {
			next.push_back(a);
			next.push_back(b);
			next.push_back(c);
		};
		for (size_t t = 0; t + 2 < tris.size(); t += 3) {
			int32_t v[3] = { tris[t], tris[t + 1], tris[t + 2] };
			if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[0] >= vcount || v[1] >= vcount || v[2] >= vcount) {
				continue;
			}
			int32_t m[3] = { find_mid(v[0], v[1]), find_mid(v[1], v[2]), find_mid(v[2], v[0]) };
			const int splits = (m[0] >= 0) + (m[1] >= 0) + (m[2] >= 0);
			if (splits == 0) {
				emit(v[0], v[1], v[2]);
				continue;
			}
			if (splits == 3) {
				emit(v[0], m[0], m[2]);
				emit(m[0], v[1], m[1]);
				emit(m[2], m[1], v[2]);
				emit(m[0], m[1], m[2]);
				continue;
			}
			// Rotate so that, with one split, edge 0 is split; with two,
			// edge 2 is the one left whole.
			for (int r = 0; r < 3; r++) {
				const bool ok = splits == 1 ? m[0] >= 0 : m[2] < 0;
				if (ok) {
					break;
				}
				const int32_t tv = v[0], tm = m[0];
				v[0] = v[1];
				v[1] = v[2];
				v[2] = tv;
				m[0] = m[1];
				m[1] = m[2];
				m[2] = tm;
			}
			if (splits == 1) {
				emit(v[0], m[0], v[2]);
				emit(m[0], v[1], v[2]);
			} else {
				emit(m[0], v[1], m[1]);
				emit(v[0], m[0], m[1]);
				emit(v[0], m[1], v[2]);
			}
		}
		tris.swap(next);
	}

	r_out.indices.swap(tris);
	r_out.input = MeshInput();
	r_out.input.positions = r_out.positions.data();
	r_out.input.normals = r_out.normals.empty() ? nullptr : r_out.normals.data();
	r_out.input.colors = r_out.colors.empty() ? nullptr : r_out.colors.data();
	r_out.input.uvs = r_out.uvs.empty() ? nullptr : r_out.uvs.data();
	r_out.input.vertex_count = (int32_t)(r_out.positions.size() / 3);
	r_out.input.indices = r_out.indices.data();
	r_out.input.index_count = (int32_t)r_out.indices.size();
	return true;
}

static void finish_geometry(Subdivided &r_geo) {
	r_geo.input = MeshInput();
	r_geo.input.positions = r_geo.positions.data();
	r_geo.input.normals = r_geo.normals.empty() ? nullptr : r_geo.normals.data();
	r_geo.input.colors = r_geo.colors.empty() ? nullptr : r_geo.colors.data();
	r_geo.input.uvs = r_geo.uvs.empty() ? nullptr : r_geo.uvs.data();
	r_geo.input.vertex_count = (int32_t)(r_geo.positions.size() / 3);
	r_geo.input.indices = r_geo.indices.data();
	r_geo.input.index_count = (int32_t)r_geo.indices.size();
}

static int64_t floor_div64(int64_t p_a, int64_t p_b) {
	return p_a >= 0 ? p_a / p_b : -((-p_a + p_b - 1) / p_b);
}

// Texel coordinate of a UV along one axis, as convert_direct rounds it.
static int64_t texel_coord(float p_uv, int32_t p_size) {
	const double t = (double)p_uv * p_size;
	return (int64_t)std::llround(t < -1e9 ? -1e9 : (t > 1e9 ? 1e9 : t));
}

// Whether three texel coordinates of a repeating texture fit one triangle's
// 8-bit UVs: inside one tile, or (sizes below 256) within 255 texels using
// the texture window.
static bool uv_span_fits(const int64_t p_coord[3], int32_t p_size) {
	const int64_t lo = std::min(p_coord[0], std::min(p_coord[1], p_coord[2]));
	const int64_t hi = std::max(p_coord[0], std::max(p_coord[1], p_coord[2]));
	const int64_t span = hi - floor_div64(lo, p_size) * p_size;
	return span <= p_size || (p_size < 256 && span <= 255);
}

// Copies the input, applies the UV transform and, for repeating textures,
// splits triangles whose UVs span more than 8-bit texture coordinates can
// reach along the tile edges (u or v = a multiple of the texture size). New
// vertices are shared per (edge, line), so neighbours stay crack-free.
static void prepare_uvs(const MeshInput &p_input, const ConvertOptions &p_options, Subdivided &r_geo) {
	const int32_t n = p_input.vertex_count;
	r_geo.positions.assign(p_input.positions, p_input.positions + n * 3);
	if (p_input.normals) {
		r_geo.normals.assign(p_input.normals, p_input.normals + n * 3);
	}
	if (p_input.colors) {
		r_geo.colors.assign(p_input.colors, p_input.colors + n * 4);
	}
	r_geo.uvs.resize((size_t)n * 2);
	for (int32_t i = 0; i < n; i++) {
		r_geo.uvs[i * 2] = p_input.uvs[i * 2] * p_options.uv_scale[0] + p_options.uv_offset[0];
		r_geo.uvs[i * 2 + 1] = p_input.uvs[i * 2 + 1] * p_options.uv_scale[1] + p_options.uv_offset[1];
	}
	std::vector<int32_t> tris;
	if (p_input.indices) {
		for (int32_t i = 0; i + 2 < p_input.index_count; i += 3) {
			const int32_t a = p_input.indices[i], b = p_input.indices[i + 1], c = p_input.indices[i + 2];
			if (a >= 0 && b >= 0 && c >= 0 && a < n && b < n && c < n) {
				tris.push_back(a);
				tris.push_back(b);
				tris.push_back(c);
			}
		}
	} else {
		for (int32_t i = 0; i + 2 < n; i += 3) {
			tris.push_back(i);
			tris.push_back(i + 1);
			tris.push_back(i + 2);
		}
	}
	if (!p_options.texture_repeat) {
		r_geo.indices.swap(tris);
		finish_geometry(r_geo);
		return;
	}

	const int32_t sizes[2] = { p_options.texture_width, p_options.texture_height };
	std::map<std::tuple<int32_t, int32_t, int, int64_t>, int32_t> cuts;
	auto lerp_into = [](std::vector<float> &r_v, int p_width, int32_t a, int32_t b, float t) {
		if (r_v.empty()) {
			return;
		}
		for (int k = 0; k < p_width; k++) {
			const float va = r_v[a * p_width + k], vb = r_v[b * p_width + k];
			r_v.push_back(va + (vb - va) * t);
		}
	};
	// Vertex where edge (a, b) crosses texel line p_line on p_axis.
	auto cut = [&](int32_t a, int32_t b, int p_axis, int64_t p_line) -> int32_t {
		if (a > b) {
			std::swap(a, b);
		}
		const auto key = std::make_tuple(a, b, p_axis, p_line);
		auto it = cuts.find(key);
		if (it != cuts.end()) {
			return it->second;
		}
		const double ca = (double)r_geo.uvs[a * 2 + p_axis] * sizes[p_axis];
		const double cb = (double)r_geo.uvs[b * 2 + p_axis] * sizes[p_axis];
		const float t = (float)((p_line - ca) / (cb - ca));
		const int32_t m = (int32_t)(r_geo.positions.size() / 3);
		lerp_into(r_geo.positions, 3, a, b, t);
		lerp_into(r_geo.normals, 3, a, b, t);
		lerp_into(r_geo.colors, 4, a, b, t);
		lerp_into(r_geo.uvs, 2, a, b, t);
		cuts.emplace(key, m);
		return m;
	};
	auto coord = [&](int32_t v, int p_axis) {
		return (double)r_geo.uvs[v * 2 + p_axis] * sizes[p_axis];
	};
	// Keeps the part of p_poly with coord >= p_line (p_keep_above) or <= p_line.
	auto clip = [&](const std::vector<int32_t> &p_poly, int p_axis, int64_t p_line, bool p_keep_above) {
		std::vector<int32_t> out;
		for (size_t i = 0; i < p_poly.size(); i++) {
			const int32_t cur = p_poly[i], next = p_poly[(i + 1) % p_poly.size()];
			const double cc = coord(cur, p_axis), cn = coord(next, p_axis);
			const bool in_cur = p_keep_above ? cc >= p_line : cc <= p_line;
			const bool in_next = p_keep_above ? cn >= p_line : cn <= p_line;
			if (in_cur) {
				out.push_back(cur);
			}
			if (in_cur != in_next) {
				out.push_back(cut(cur, next, p_axis, p_line));
			}
		}
		return out;
	};

	std::vector<int32_t> out_tris;
	out_tris.reserve(tris.size());
	std::vector<std::vector<int32_t>> pieces, next_pieces;
	for (size_t t = 0; t + 2 < tris.size(); t += 3) {
		pieces.assign(1, { tris[t], tris[t + 1], tris[t + 2] });
		for (int axis = 0; axis < 2; axis++) {
			next_pieces.clear();
			for (const std::vector<int32_t> &poly : pieces) {
				const int32_t size = sizes[axis];
				double lo = 1e30, hi = -1e30;
				for (int32_t v : poly) {
					lo = std::fmin(lo, coord(v, axis));
					hi = std::fmax(hi, coord(v, axis));
				}
				bool fits = poly.size() == 3;
				if (fits) {
					const int64_t c3[3] = { texel_coord(r_geo.uvs[poly[0] * 2 + axis], size), texel_coord(r_geo.uvs[poly[1] * 2 + axis], size), texel_coord(r_geo.uvs[poly[2] * 2 + axis], size) };
					fits = uv_span_fits(c3, size);
				} else {
					fits = hi - std::floor(lo / size) * size <= size;
				}
				const int64_t k0 = (int64_t)std::floor(lo / size), k1 = (int64_t)std::ceil(hi / size);
				if (fits || k1 - k0 > 64) {
					next_pieces.push_back(poly); // Fits, or too many tiles: clamped later.
					continue;
				}
				for (int64_t k = k0; k < k1; k++) {
					std::vector<int32_t> strip = clip(poly, axis, k * size, true);
					if (strip.size() >= 3) {
						strip = clip(strip, axis, (k + 1) * size, false);
					}
					if (strip.size() >= 3) {
						next_pieces.push_back(strip);
					}
				}
			}
			pieces.swap(next_pieces);
		}
		for (const std::vector<int32_t> &poly : pieces) {
			for (size_t k = 1; k + 1 < poly.size(); k++) {
				out_tris.push_back(poly[0]);
				out_tris.push_back(poly[k]);
				out_tris.push_back(poly[k + 1]);
			}
		}
	}
	r_geo.indices.swap(out_tris);
	finish_geometry(r_geo);
}

static bool convert_direct(const MeshInput &p_input, const ConvertOptions &p_options, MeshData &r_mesh);

bool convert_mesh(const MeshInput &p_input, const ConvertOptions &p_options, MeshData &r_mesh) {
	if (!p_input.positions || p_input.vertex_count <= 0) {
		r_mesh = MeshData();
		return false;
	}
	const bool textured = p_options.texture_width > 0 && p_options.texture_height > 0 && p_input.uvs;
	Subdivided prepared;
	const MeshInput *src = &p_input;
	if (textured) {
		prepare_uvs(p_input, p_options, prepared);
		src = &prepared.input;
	}
	if (p_options.max_edge > 0.0f && p_options.scale > 0.0f) {
		Subdivided sub;
		if (subdivide(*src, p_options, sub)) {
			return convert_direct(sub.input, p_options, r_mesh);
		}
	}
	return convert_direct(*src, p_options, r_mesh);
}

static bool convert_direct(const MeshInput &p_input, const ConvertOptions &p_options, MeshData &r_mesh) {
	r_mesh = MeshData();
	if (!p_input.positions || p_input.vertex_count <= 0) {
		return false;
	}
	const int32_t n = p_input.vertex_count;

	// Triangles: drop any with an out-of-range index.
	if (p_input.indices) {
		r_mesh.indices.reserve(p_input.index_count);
		for (int32_t i = 0; i + 2 < p_input.index_count; i += 3) {
			const int32_t a = p_input.indices[i], b = p_input.indices[i + 1], c = p_input.indices[i + 2];
			if (a < 0 || b < 0 || c < 0 || a >= n || b >= n || c >= n) {
				continue;
			}
			r_mesh.indices.push_back((uint32_t)a);
			r_mesh.indices.push_back((uint32_t)b);
			r_mesh.indices.push_back((uint32_t)c);
		}
	} else {
		for (int32_t i = 0; i + 2 < n; i += 3) {
			r_mesh.indices.push_back((uint32_t)i);
			r_mesh.indices.push_back((uint32_t)i + 1);
			r_mesh.indices.push_back((uint32_t)i + 2);
		}
	}
	if (r_mesh.indices.empty()) {
		return false;
	}

	// Positions: the smallest shift that fits int16.
	double extent = 0.0;
	for (int32_t i = 0; i < n * 3; i++) {
		extent = std::fmax(extent, std::fabs((double)p_input.positions[i] * p_options.scale));
	}
	int32_t shift = 0;
	while (shift < CONVERT_MAX_SHIFT && std::lround(extent / (double)(1 << shift)) > 32767) {
		shift++;
	}
	r_mesh.shift = shift;
	const double k = (double)p_options.scale / (double)(1 << shift);
	r_mesh.positions.resize(n);
	int32_t lo[3] = { 32767, 32767, 32767 }, hi[3] = { -32768, -32768, -32768 };
	for (int32_t i = 0; i < n; i++) {
		SVector &v = r_mesh.positions[i];
		v.x = to_int16(p_input.positions[i * 3 + 0] * k, r_mesh.clamped);
		v.y = to_int16(p_input.positions[i * 3 + 1] * k, r_mesh.clamped);
		v.z = to_int16(p_input.positions[i * 3 + 2] * k, r_mesh.clamped);
		const int32_t c[3] = { v.x, v.y, v.z };
		for (int a = 0; a < 3; a++) {
			lo[a] = c[a] < lo[a] ? c[a] : lo[a];
			hi[a] = c[a] > hi[a] ? c[a] : hi[a];
		}
	}
	r_mesh.center.x = (int16_t)((lo[0] + hi[0]) / 2);
	r_mesh.center.y = (int16_t)((lo[1] + hi[1]) / 2);
	r_mesh.center.z = (int16_t)((lo[2] + hi[2]) / 2);
	double r2 = 0.0;
	for (const SVector &v : r_mesh.positions) {
		const double dx = v.x - r_mesh.center.x, dy = v.y - r_mesh.center.y, dz = v.z - r_mesh.center.z;
		r2 = std::fmax(r2, dx * dx + dy * dy + dz * dz);
	}
	r_mesh.radius = (int32_t)std::ceil(std::sqrt(r2));

	// Normals: unit length as 1.3.12.
	if (p_input.normals) {
		r_mesh.normals.resize(n);
		for (int32_t i = 0; i < n; i++) {
			const double x = p_input.normals[i * 3] * (double)p_options.normal_scale[0];
			const double y = p_input.normals[i * 3 + 1] * (double)p_options.normal_scale[1];
			const double z = p_input.normals[i * 3 + 2] * (double)p_options.normal_scale[2];
			const double len = std::sqrt(x * x + y * y + z * z);
			const double s = len > 1e-12 ? 4096.0 / len : 0.0;
			bool unused = false;
			r_mesh.normals[i] = { to_int16(x * s, unused), to_int16(y * s, unused), to_int16(z * s, unused) };
		}
	}

	// Colours: what Godot's StandardMaterial3D would output for an unlit
	// surface, linear(albedo) * linear(vertex colour), back to sRGB.
	const float *al = p_options.albedo;
	r_mesh.alpha = al[3];
	r_mesh.colors.resize((size_t)n * 3);
	const bool vc = p_options.use_vertex_colors && p_input.colors;
	if (!vc) {
		const uint8_t c[3] = { unit_to_byte(al[0]), unit_to_byte(al[1]), unit_to_byte(al[2]) };
		for (int32_t i = 0; i < n; i++) {
			r_mesh.colors[i * 3 + 0] = c[0];
			r_mesh.colors[i * 3 + 1] = c[1];
			r_mesh.colors[i * 3 + 2] = c[2];
		}
	} else {
		const float al_lin[3] = { srgb_to_linear(al[0]), srgb_to_linear(al[1]), srgb_to_linear(al[2]) };
		for (int32_t i = 0; i < n; i++) {
			for (int c = 0; c < 3; c++) {
				float v = p_input.colors[i * 4 + c];
				v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
				if (p_options.vertex_colors_srgb) {
					v = srgb_to_linear(v);
				}
				r_mesh.colors[i * 3 + c] = unit_to_byte(linear_to_srgb(al_lin[c] * v));
			}
		}
	}

	// Alpha: the mean of the triangle's vertex alphas, rounded to the PS1's
	// three levels. Skipped when nothing is below the opaque threshold.
	if (p_options.use_alpha) {
		const bool vc_alpha = vc;
		const uint32_t tris = r_mesh.triangle_count();
		std::vector<uint8_t> classes(tris, TRI_OPAQUE);
		bool any = false;
		for (uint32_t t = 0; t < tris; t++) {
			float a = 0.0f;
			for (int corner = 0; corner < 3; corner++) {
				float va = 1.0f;
				if (vc_alpha) {
					va = p_input.colors[r_mesh.indices[t * 3 + corner] * 4 + 3];
				}
				a += al[3] * va;
			}
			a /= 3.0f;
			const uint8_t c = a >= 0.75f ? TRI_OPAQUE : (a > 0.25f ? TRI_HALF : TRI_HIDDEN);
			classes[t] = c;
			any = any || c != TRI_OPAQUE;
		}
		if (any) {
			r_mesh.triangle_alpha = std::move(classes);
		}
	}

	// UVs (already transformed by prepare_uvs): texel coordinates per
	// triangle corner. Repeating textures: each triangle is moved by whole
	// tiles to the tile it starts in; one that still runs past the edge uses
	// the texture window. A texel coordinate equal to the size (the far
	// edge) is pulled in to size - 1, as PS1 tools did.
	if (p_input.uvs && p_options.texture_width > 0 && p_options.texture_height > 0) {
		r_mesh.textured = true;
		const uint32_t tris = r_mesh.triangle_count();
		const int32_t sizes[2] = { p_options.texture_width, p_options.texture_height };
		r_mesh.tri_uvs.resize((size_t)tris * 6);
		std::vector<uint8_t> window(tris, 0);
		bool any_window = false;
		for (uint32_t t = 0; t < tris; t++) {
			int64_t c[2][3];
			bool windowed = false;
			for (int axis = 0; axis < 2; axis++) {
				const int32_t size = sizes[axis];
				for (int corner = 0; corner < 3; corner++) {
					c[axis][corner] = texel_coord(p_input.uvs[r_mesh.indices[t * 3 + corner] * 2 + axis], size);
				}
				if (p_options.texture_repeat) {
					const int64_t first = std::min(c[axis][0], std::min(c[axis][1], c[axis][2]));
					const int64_t tile = floor_div64(first, size) * size;
					int64_t last = 0;
					for (int corner = 0; corner < 3; corner++) {
						c[axis][corner] -= tile;
						last = std::max(last, c[axis][corner]);
					}
					// Sprite sheets keep the far edge, and wrap it (as Godot's
					// repeat does) rather than read past the texture.
					const int64_t past = p_options.keep_far_edge ? size - 1 : size;
					windowed = windowed || (last > past && size < 256 && last <= 255);
				}
			}
			for (int axis = 0; axis < 2; axis++) {
				const int64_t top = windowed ? 255 : sizes[axis] - 1;
				for (int corner = 0; corner < 3; corner++) {
					const int64_t v = c[axis][corner];
					r_mesh.tri_uvs[t * 6 + corner * 2 + axis] = (uint8_t)(v < 0 ? 0 : (v > top ? top : v));
				}
			}
			window[t] = windowed ? 1 : 0;
			any_window = any_window || windowed;
		}
		if (any_window) {
			r_mesh.tri_window = std::move(window);
		}
	}
	return true;
}

} // namespace psx
