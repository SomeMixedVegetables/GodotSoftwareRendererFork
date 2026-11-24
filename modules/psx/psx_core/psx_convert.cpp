#include "psx_convert.h"

#include <cmath>
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

static bool convert_direct(const MeshInput &p_input, const ConvertOptions &p_options, MeshData &r_mesh);

bool convert_mesh(const MeshInput &p_input, const ConvertOptions &p_options, MeshData &r_mesh) {
	if (p_options.max_edge > 0.0f && p_options.scale > 0.0f && p_input.positions && p_input.vertex_count > 0) {
		Subdivided sub;
		if (subdivide(p_input, p_options, sub)) {
			return convert_direct(sub.input, p_options, r_mesh);
		}
	}
	return convert_direct(p_input, p_options, r_mesh);
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
			const double x = p_input.normals[i * 3], y = p_input.normals[i * 3 + 1], z = p_input.normals[i * 3 + 2];
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

	// UVs: wrapped to 0..255 texels of a 256-texel page (refined with textures).
	if (p_input.uvs) {
		r_mesh.uvs.resize((size_t)n * 2);
		for (int32_t i = 0; i < n * 2; i++) {
			const float u = p_input.uvs[i] - std::floor(p_input.uvs[i]);
			r_mesh.uvs[i] = (uint8_t)((int)(u * 256.0f) & 255);
		}
	}
	return true;
}

} // namespace psx
