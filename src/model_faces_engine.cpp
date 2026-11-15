// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The shared upright face detector adapted to decoded coordinates for alignment, and
// MobileFaceNet on ncnn for the 128-float vector. The rules the vectors obey live in model_faces.cpp.
// Owned by docs/faces.md.

#include "pch.h"

#include "app_resources.h"
#include "model_faces.h"

#include <net.h>

#undef near
#undef far

namespace
{
	class ncnn_face_engine final : public faces::face_engine
	{
		// ncnn references the weight bytes rather than copying them, so the blob outlives the net.
		core::blob _weights;
		ncnn::Net _net;
		bool _loaded = false;
		// Reused across files, so a collection-wide pass allocates none of these per picture.
		faces::face_detector_ptr _detector = faces::create_face_detector();
		std::vector<uint8_t> _aligned;
		// The aligned crop as NV12, and its RGB form. Only ever 112x112, whatever the picture.
		std::vector<uint8_t> _aligned_yuv;
		std::vector<uint8_t> _aligned_rgb;

	public:
		ncnn_face_engine() :
			_aligned(static_cast<size_t>(faces::aligned_extent) * faces::aligned_extent * 4),
			_aligned_yuv(static_cast<size_t>(faces::aligned_extent) * faces::aligned_extent * 3 / 2),
			_aligned_rgb(static_cast<size_t>(faces::aligned_extent) * faces::aligned_extent * 3)
		{
		}

		bool load()
		{
			// A pool sized to the machine underneath a queue that has already decided how much of the
			// machine to spend is not a decision anyone made. Parallelism belongs at the item level.
			_net.opt.num_threads = 1;
			_net.opt.use_vulkan_compute = false;

			auto param = load_app_resource(app_resource::face_model_param);
			_weights = load_app_resource(app_resource::face_model_bin);

			if (param.empty() || _weights.empty())
			{
				core::log(__FUNCTION__, "Face model resource is missing"sv);
				return false;
			}

			// load_param_mem needs a null-terminated string, which a resource blob is not.
			const std::string param_text(std::bit_cast<const char*>(param.data()), param.size());

			if (_net.load_param_mem(param_text.c_str()) != 0)
			{
				// ncnn names the layer it could not create, which is the failure worth having when
				// the pruned layer set and the model have drifted apart.
				core::log(__FUNCTION__, "Face model could not be parsed"sv);
				return false;
			}

			if (_net.load_model(_weights.data()) == 0)
			{
				core::log(__FUNCTION__, "Face model weights could not be read"sv);
				return false;
			}

			_loaded = true;
			return true;
		}

		std::vector<faces::detection> detect(const core::const_surface_ptr& surface, const faces::tuning& t) override
		{
			std::vector<faces::detection> result;

			if (!_loaded || !core::is_valid(surface)) return result;

			const auto extent = surface->dimensions();
			const auto orientation = surface->orientation();
			const auto detected = _detector->detect(surface, t);
			result.reserve(detected.size());
			for (const auto& face : detected)
			{
				faces::detection d;
				d.confidence = face.confidence;
				d.bounds = faces::from_upright(face.bounds, extent, orientation);
				for (auto m = 0; m < faces::landmark_count; ++m)
				{
					d.marks[m] = faces::from_upright(face.marks[m], extent, orientation);
				}
				result.emplace_back(d);
			}

			return result;
		}

		bool embed(const core::const_surface_ptr& surface, const affined& transform,
		           faces::face_vector& result) override
		{
			if (!_loaded || !core::is_valid(surface)) return false;

			const auto format = surface->format();
			const auto packed = core::is_packed(format);
			if (!packed && format != core::texture_format::NV12) return false;

			const auto extent = surface->dimensions();
			if (extent.is_empty()) return false;

			// warpaffine walks the destination and asks where each pixel came from, so it takes the
			// inverse of the source-to-template fit. affined stores columns where ncnn stores rows.
			const auto inverse = transform.invert();
			const auto* const to_source = inverse.coefficients();
			if (!std::all_of(to_source, to_source + 6, [](const double v) { return std::isfinite(v); })) return false;
			const float tm[6]{
				static_cast<float>(to_source[0]), static_cast<float>(to_source[2]), static_cast<float>(to_source[4]),
				static_cast<float>(to_source[1]), static_cast<float>(to_source[3]), static_cast<float>(to_source[5]),
			};

			constexpr int edge = faces::aligned_extent;
			const auto src_stride = static_cast<int>(surface->stride());

			// Warped straight out of the decoded surface, so only these 112x112 pixels are ever read -
			// whatever the size of the picture they came from.
			if (packed)
			{
				ncnn::warpaffine_bilinear_c4(surface->pixels(), extent.cx, extent.cy, src_stride, _aligned.data(), edge,
				                             edge, edge * 4, tm);
			}
			else
			{
				// Luma and chroma are warped apart because the packed-plane helper assumes stride equals
				// width, and a surface row is padded. Chroma is half resolution on both axes, so the same
				// map applies with its translation halved.
				auto* const dst_y = _aligned_yuv.data();
				auto* const dst_uv = dst_y + static_cast<size_t>(edge) * edge;
				const auto* const src_y = surface->pixels();
				const auto* const src_uv = src_y + static_cast<size_t>(src_stride) * extent.cy;
				const float tm_uv[6]{tm[0], tm[1], tm[2] * 0.5f, tm[3], tm[4], tm[5] * 0.5f};

				ncnn::warpaffine_bilinear_c1(src_y, extent.cx, extent.cy, src_stride, dst_y, edge, edge, edge, tm);
				ncnn::warpaffine_bilinear_c2(src_uv, extent.cx / 2, extent.cy / 2, src_stride, dst_uv, edge / 2,
				                             edge / 2, edge, tm_uv);

				// Converted here rather than by ncnn's yuv420sp2rgb_nv12, which is fixed full-range
				// BT.601. The surface knows which matrix and range it actually is - a WebP decode is
				// 601 limited, a JPEG is 601 full, a video frame is usually 709 limited - and putting
				// limited-range samples through a full-range conversion lifts the black level and flattens
				// the contrast. That shifts the vector, and a vector only means anything against others
				// made the same way.
				const auto m = core::compute_yuv_matrix(surface->color_space());

				for (auto y = 0; y < edge; ++y)
				{
					const auto* const luma_row = dst_y + (static_cast<size_t>(y) * edge);
					const auto* const chroma_row = dst_uv + (static_cast<size_t>(y / 2) * edge);
					auto* const rgb_row = _aligned_rgb.data() + (static_cast<size_t>(y) * edge * 3);

					for (auto x = 0; x < edge; ++x)
					{
						const auto luma = luma_row[x] / 255.0f;
						const auto cb = chroma_row[(x & ~1)] / 255.0f;
						const auto cr = chroma_row[(x & ~1) + 1] / 255.0f;

						const auto to_byte = [](const float v)
						{
							return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
						};

						rgb_row[x * 3] = to_byte(m.m[0] * luma + m.m[1] * cb + m.m[2] * cr + m.m[3]);
						rgb_row[x * 3 + 1] = to_byte(m.m[4] * luma + m.m[5] * cb + m.m[6] * cr + m.m[7]);
						rgb_row[x * 3 + 2] = to_byte(m.m[8] * luma + m.m[9] * cb + m.m[10] * cr + m.m[11]);
					}
				}
			}

			// The weights are trained on RGB, and the mean and scale are already baked into the graph
			// as its first two operations, so raw bytes in the right channel order are the whole input.
			const auto input = packed ? ncnn::Mat::from_pixels(_aligned.data(), ncnn::Mat::PIXEL_BGRA2RGB, edge, edge,
			                                                   edge * 4)
			                          : ncnn::Mat::from_pixels(_aligned_rgb.data(), ncnn::Mat::PIXEL_RGB, edge, edge,
			                                                   edge * 3);

			// Threading is decided once, on the net's options, because parallelism belongs at the item
			// level and not inside one embedding.
			auto ex = _net.create_extractor();

			if (ex.input("data", input) != 0) return false;

			ncnn::Mat out;
			if (ex.extract("fc1", out) != 0) return false;
			if (out.w * out.h * out.c != faces::vector_size) return false;

			const auto* const values = static_cast<const float*>(out.data);
			if (!std::all_of(values, values + faces::vector_size, [](const float v) { return std::isfinite(v); }))
				return false;

			faces::face_vector embedded;
			std::copy_n(values, faces::vector_size, embedded.begin());

			// Normalised once here so that every later comparison is a dot product.
			faces::normalise(embedded);
			if (!std::all_of(embedded.begin(), embedded.end(), [](const float v) { return std::isfinite(v); }))
				return false;
			result = embedded;
			return true;
		}
	};
}

namespace faces
{
	face_engine_ptr create_face_engine()
	{
#ifdef DIFFRACTOR_HAS_FACE_MODEL
		auto engine = std::make_unique<ncnn_face_engine>();
		if (!engine->load()) return {};
		return engine;
#else
		return {};
#endif
	}

	bool face_engine_available()
	{
#ifdef DIFFRACTOR_HAS_FACE_MODEL
		return true;
#else
		return false;
#endif
	}
}
