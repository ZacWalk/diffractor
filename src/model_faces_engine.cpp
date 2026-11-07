// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The two vendored libraries behind the face pipeline - libfacedetection's weights and
// forward pass for detection, and MobileFaceNet on ncnn for the 128-float vector. Built only when
// both resolved; the rules the vectors then obey live in model_faces.cpp. Owned by docs/faces.md.

#include "pch.h"

#include "app_resources.h"
#include "model_faces.h"

#include <hw_blob.h>
#include <hw_image.h>
#include <hw_model.h>
#include <hw_network.h>
#include <hw_postprocess.h>
#include <net.h>

#undef near
#undef far

namespace
{
	// The detection thresholds libfacedetection's own entry point applies, kept here because that
	// entry point is not the one Diffractor calls.
	constexpr float detector_overlap_threshold = 0.45f;
	constexpr float detector_score_threshold = 0.2f;
	constexpr int detector_top_k = 1000;
	constexpr int detector_keep_top_k = 512;

	// The network reads a 3x3 neighbourhood at half resolution, packed into 32 channels: 27 values
	// and five zeros. Its first layer is 1x1 pointwise over those 32, so the whole of its colour
	// dependence is one linear map - and measurement says feeding luminance in place of B, G and R
	// changes no detection and moves the confidence by about a point. Luminance is therefore what
	// Diffractor feeds, which is why this stage owns the packing rather than calling
	// facedetect_hw_cnn: that entry point hard-codes three interleaved channels.
	constexpr int detector_pad_divisor = 32;
	constexpr int detector_blob_channels = 32;

	struct detector_candidate
	{
		float score = 0;
		float xmin = 0;
		float ymin = 0;
		float xmax = 0;
		float ymax = 0;
		std::array<float, faces::landmark_count * 2> marks{};
	};

	void flatten_heads(const std::array<fdt::hw::HwBlob, 3>& heads, fdt::hw::HwBlob& output)
	{
		auto channels = 0;
		for (const auto& head : heads) channels += head.rows() * head.cols() * head.channels();

		output.ResizeForOverwrite(1, 1, channels);
		auto* out = output.Ptr(0, 0);

		for (const auto& head : heads)
		{
			for (auto row = 0; row < head.rows(); ++row)
			{
				for (auto col = 0; col < head.cols(); ++col)
				{
					std::copy_n(head.Ptr(row, col), head.channels(), out);
					out += head.channels();
				}
			}
		}

		std::fill(out, output.Ptr(0, 0) + output.channel_step(), 0.0f);
	}

	void decode_heads(fdt::hw::HwHeadOutputs& heads, std::array<fdt::hw::HwBlob, 3>& priors,
	                  fdt::hw::HwDecodedOutputs& decoded)
	{
		for (auto i = 0; i < 3; ++i)
		{
			const auto stride = 8 << i;
			const auto rows = heads.cls[i].rows();
			const auto cols = heads.cls[i].cols();

			if (priors[i].rows() != rows || priors[i].cols() != cols)
			{
				priors[i] = fdt::hw::MeshgridHw(cols, rows, stride);
			}

			fdt::hw::BboxDecodeHw(heads.reg[i], priors[i], stride);
			fdt::hw::KpsDecodeHw(heads.kps[i], priors[i], stride);
		}

		flatten_heads(heads.cls, decoded.cls);
		flatten_heads(heads.reg, decoded.reg);
		flatten_heads(heads.kps, decoded.kps);
		flatten_heads(heads.obj, decoded.obj);
		fdt::hw::SigmoidHw(decoded.cls);
		fdt::hw::SigmoidHw(decoded.obj);
	}

	float overlap(const detector_candidate& a, const detector_candidate& b)
	{
		const auto width = std::min(a.xmax, b.xmax) - std::max(a.xmin, b.xmin);
		const auto height = std::min(a.ymax, b.ymax) - std::max(a.ymin, b.ymin);
		if (width <= 0.0f || height <= 0.0f) return 0.0f;

		const auto intersection = width * height;
		const auto area_a = (a.xmax - a.xmin) * (a.ymax - a.ymin);
		const auto area_b = (b.xmax - b.xmin) * (b.ymax - b.ymin);
		const auto combined = area_a + area_b - intersection;
		return combined > 0.0f ? intersection / combined : 0.0f;
	}

	void select_detections(const fdt::hw::HwDecodedOutputs& decoded, const int confidence_floor,
	                       std::vector<detector_candidate>& candidates,
	                       std::vector<detector_candidate>& selected)
	{
		candidates.clear();
		selected.clear();

		const auto* const cls = decoded.cls.Ptr(0, 0);
		const auto* const reg = decoded.reg.Ptr(0, 0);
		const auto* const kps = decoded.kps.Ptr(0, 0);
		const auto* const obj = decoded.obj.Ptr(0, 0);

		for (auto i = 0; i < decoded.cls.channels(); ++i)
		{
			const auto score = std::sqrt(cls[i] * obj[i]);
			if (!std::isfinite(score)) continue;

			// Refused here rather than after the sort and the overlap pass, which is exactly equivalent:
			// a box is only ever suppressed by a HIGHER-scoring box, so any suppressor of a box above the
			// floor is above it too. The vendored 0.2 floor let the whole pipeline run over detections
			// the caller was always going to discard.
			if (static_cast<int>(score * 100.0f) < confidence_floor) continue;

			detector_candidate candidate;
			candidate.score = score;
			candidate.xmin = reg[4 * i];
			candidate.ymin = reg[4 * i + 1];
			candidate.xmax = reg[4 * i + 2];
			candidate.ymax = reg[4 * i + 3];
			std::copy_n(kps + 10 * i, candidate.marks.size(), candidate.marks.begin());

			const auto bounds_finite = std::isfinite(candidate.xmin) && std::isfinite(candidate.ymin) &&
				std::isfinite(candidate.xmax) && std::isfinite(candidate.ymax);
			const auto marks_finite = std::ranges::all_of(candidate.marks, [](const float v) { return std::isfinite(v); });
			if (bounds_finite && marks_finite && candidate.xmax > candidate.xmin && candidate.ymax > candidate.ymin)
			{
				candidates.emplace_back(candidate);
			}
		}

		std::ranges::stable_sort(candidates, std::greater{}, &detector_candidate::score);
		if (candidates.size() > detector_top_k) candidates.resize(detector_top_k);
		selected.reserve(std::min(candidates.size(), static_cast<size_t>(detector_keep_top_k)));

		for (const auto& candidate : candidates)
		{
			if (std::ranges::all_of(selected, [&candidate](const detector_candidate& kept)
				{
					return overlap(candidate, kept) <= detector_overlap_threshold;
				}))
			{
				selected.emplace_back(candidate);
				if (selected.size() >= detector_keep_top_k) break;
			}
		}
	}

	void pack_luma_blob(const uint8_t* luma, const sizei extent, const int stride, fdt::hw::HwBlob* blob)
	{
		const auto rows = ((extent.cy - 1) / detector_pad_divisor + 1) * detector_pad_divisor / 2;
		const auto cols = ((extent.cx - 1) / detector_pad_divisor + 1) * detector_pad_divisor / 2;
		blob->ResizeForOverwrite(rows, cols, detector_blob_channels);

		for (auto row = 0; row < rows; ++row)
		{
			const auto center_y = row * 2;

			for (auto col = 0; col < cols; ++col)
			{
				const auto center_x = col * 2;
				auto* const out = blob->Ptr(row, col);

				if (center_y > 0 && center_y + 1 < extent.cy && center_x > 0 && center_x + 1 < extent.cx)
				{
					for (auto fy = -1; fy <= 1; ++fy)
					{
						const auto* const in = luma + (static_cast<size_t>(stride) * (center_y + fy)) + center_x - 1;
						auto* const cell = out + ((fy + 1) * 9);

						for (auto fx = 0; fx < 3; ++fx)
						{
							// One sample fills a whole BGR triple: the collapsed response of the first
							// layer to a grey pixel is exactly this.
							const auto v = static_cast<float>(in[fx]);
							cell[fx * 3] = v;
							cell[(fx * 3) + 1] = v;
							cell[(fx * 3) + 2] = v;
						}
					}

					out[27] = 0.0f;
					out[28] = 0.0f;
					out[29] = 0.0f;
					out[30] = 0.0f;
					out[31] = 0.0f;
					continue;
				}

				for (auto ch = 0; ch < detector_blob_channels; ++ch) out[ch] = 0.0f;

				for (auto fy = -1; fy <= 1; ++fy)
				{
					const auto src_y = center_y + fy;
					if (src_y < 0 || src_y >= extent.cy) continue;

					for (auto fx = -1; fx <= 1; ++fx)
					{
						const auto src_x = center_x + fx;
						if (src_x < 0 || src_x >= extent.cx) continue;

						const auto v = static_cast<float>(luma[(static_cast<size_t>(stride) * src_y) + src_x]);
						auto* const cell = out + (((fy + 1) * 3 + fx + 1) * 3);
						cell[0] = v;
						cell[1] = v;
						cell[2] = v;
					}
				}
			}
		}
	}

	// 435 KB of weights across 53 filters, and the only part of libfacedetection Diffractor treats as
	// irreplaceable. Loaded once: the table is static const and the filters repack it for SIMD.
	const fdt::hw::HwModel& detector_model()
	{
		static const fdt::hw::HwModel model = fdt::hw::LoadStaticModel();
		return model;
	}

	class ncnn_face_engine final : public faces::face_engine
	{
		// ncnn references the weight bytes rather than copying them, so the blob outlives the net.
		df::blob _weights;
		ncnn::Net _net;
		bool _loaded = false;
		// Reused across files, so a collection-wide pass allocates none of these per picture.
		ui::surface_ptr _reduced;
		std::vector<uint8_t> _detector_input;
		std::vector<uint8_t> _aligned;
		// The aligned crop as NV12, and its RGB form. Only ever 112x112, whatever the picture.
		std::vector<uint8_t> _aligned_yuv;
		std::vector<uint8_t> _aligned_rgb;
		fdt::hw::HwBlob _detector_blob;
		fdt::hw::HwNetworkWorkspace _network_workspace;
		fdt::hw::HwHeadOutputs _head_outputs;
		fdt::hw::HwDecodedOutputs _decoded;
		std::array<fdt::hw::HwBlob, 3> _priors;
		std::vector<detector_candidate> _candidates;
		std::vector<detector_candidate> _selected;

	public:
		ncnn_face_engine() : _aligned(static_cast<size_t>(faces::aligned_extent) * faces::aligned_extent * 4),
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
				df::log(__FUNCTION__, "Face model resource is missing"sv);
				return false;
			}

			// load_param_mem needs a null-terminated string, which a resource blob is not.
			const std::string param_text(std::bit_cast<const char*>(param.data()), param.size());

			if (_net.load_param_mem(param_text.c_str()) != 0)
			{
				// ncnn names the layer it could not create, which is the failure worth having when
				// the pruned layer set and the model have drifted apart.
				df::log(__FUNCTION__, "Face model could not be parsed"sv);
				return false;
			}

			if (_net.load_model(_weights.data()) == 0)
			{
				df::log(__FUNCTION__, "Face model weights could not be read"sv);
				return false;
			}

			_loaded = true;
			return true;
		}

		std::vector<faces::detection> detect(const ui::const_surface_ptr& surface, const faces::tuning& t) override
		{
			std::vector<faces::detection> result;

			if (!_loaded || !ui::is_valid(surface)) return result;

			// Packed BGRX/BGRA, or NV12 - whose first plane already is the luminance the network
			// wants. P010 is excluded because its luma is 16-bit and would need a conversion this
			// stage deliberately does not do.
			if (const auto format = surface->format();
				!ui::is_packed(format) && format != ui::texture_format::NV12)
			{
				return result;
			}

			const auto extent = surface->dimensions();
			if (extent.is_empty()) return result;

			auto reduced = extent;
			auto scale_x = 1.0;
			auto scale_y = 1.0;

			if (const auto long_edge = std::max(extent.cx, extent.cy); long_edge > t.detector_edge)
			{
				const auto ratio = static_cast<double>(t.detector_edge) / long_edge;
				reduced = {
					std::max(1, static_cast<int>(extent.cx * ratio)),
					std::max(1, static_cast<int>(extent.cy * ratio))
				};
				scale_x = static_cast<double>(extent.cx) / reduced.cx;
				scale_y = static_cast<double>(extent.cy) / reduced.cy;
			}

			// One pass from the decoded surface to the 8-bit plane the network reads. There is no
			// intermediate colour image at any size, and an NV12 frame reaches this with its Y plane
			// untouched.
			if (!faces::reduce_to_luma(surface, reduced, _reduced, _detector_input)) return result;

			pack_luma_blob(_detector_input.data(), reduced, reduced.cx, &_detector_blob);

			fdt::hw::ForwardNetworkHwTo(_detector_blob, detector_model(), &_network_workspace, &_head_outputs);
			decode_heads(_head_outputs, _priors, _decoded);
			select_detections(_decoded, t.detector_confidence, _candidates, _selected);
			result.reserve(_selected.size());

			// A face this small carries no identity, and admitting it costs an embedding and produces
			// a group that means nothing.
			const auto min_edge = std::min(extent.cx, extent.cy) * t.min_face_edge_fraction;

			for (const auto& face : _selected)
			{
				const auto confidence = static_cast<int>(face.score * 100.0f);

				if (confidence < t.detector_confidence) continue;

				faces::detection d;
				d.confidence = confidence;
				d.bounds = rectd{face.xmin * scale_x, face.ymin * scale_y,
				                  (face.xmax - face.xmin) * scale_x, (face.ymax - face.ymin) * scale_y};

				if (d.bounds.Width < min_edge || d.bounds.Height < min_edge) continue;

				for (auto m = 0; m < faces::landmark_count; ++m)
				{
					d.marks[m] = pointd{face.marks[m * 2] * scale_x, face.marks[(m * 2) + 1] * scale_y};
				}

				result.emplace_back(d);
			}

			return result;
		}

		bool embed(const ui::const_surface_ptr& surface, const affined& transform,
		           faces::face_vector& result) override
		{
			if (!_loaded || !ui::is_valid(surface)) return false;

			const auto format = surface->format();
			const auto packed = ui::is_packed(format);
			if (!packed && format != ui::texture_format::NV12) return false;

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
				ncnn::warpaffine_bilinear_c4(surface->pixels(), extent.cx, extent.cy, src_stride,
				                             _aligned.data(), edge, edge, edge * 4, tm);
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

				ncnn::warpaffine_bilinear_c1(src_y, extent.cx, extent.cy, src_stride,
				                             dst_y, edge, edge, edge, tm);
				ncnn::warpaffine_bilinear_c2(src_uv, extent.cx / 2, extent.cy / 2, src_stride,
				                             dst_uv, edge / 2, edge / 2, edge, tm_uv);

				// Converted here rather than by ncnn's yuv420sp2rgb_nv12, which is fixed full-range
				// BT.601. The surface knows which matrix and range it actually is - a WebP decode is
				// 601 limited, a JPEG is 601 full, a video frame is usually 709 limited - and putting
				// limited-range samples through a full-range conversion lifts the black level and flattens
				// the contrast. That shifts the vector, and a vector only means anything against others
				// made the same way.
				const auto m = ui::compute_yuv_matrix(surface->color_space());

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
			const auto input = packed
				                   ? ncnn::Mat::from_pixels(_aligned.data(), ncnn::Mat::PIXEL_BGRA2RGB, edge, edge,
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
			if (!std::all_of(values, values + faces::vector_size, [](const float v) { return std::isfinite(v); })) return false;

			faces::face_vector embedded;
			std::copy_n(values, faces::vector_size, embedded.begin());

			// Normalised once here so that every later comparison is a dot product.
			faces::normalise(embedded);
			if (!std::all_of(embedded.begin(), embedded.end(), [](const float v) { return std::isfinite(v); })) return false;
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
