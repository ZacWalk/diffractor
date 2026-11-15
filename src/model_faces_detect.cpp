// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The face detector - libfacedetection's YuNet network and weights, run through its Highway
// kernels, behind faces::face_detector. Diffractor owns the input stage, feeding the network an
// upright luminance plane straight from a decoded surface, and the selection, which keeps sub-pixel
// geometry. The rules its input and output obey are in model_faces.cpp. Owned by docs/faces.md.

#include "pch.h"

#include "model_faces.h"

#include <hw_blob.h>
#include <hw_model.h>
#include <hw_network.h>
#include <hw_postprocess.h>

namespace
{
	// The overlap and candidate bounds libfacedetection's own entry point applies, kept here because
	// that entry point is not the one Diffractor calls.
	constexpr float detector_overlap_threshold = 0.45f;
	constexpr int detector_top_k = 1000;
	constexpr int detector_keep_top_k = 512;

	// The network reads a 3x3 neighbourhood at half resolution, packed into 32 channels: 27 values
	// and five zeros. Its first layer is 1x1 pointwise over those 32, so the whole of its colour
	// dependence is one linear map, and a grey pixel is a colour it reads like any other. Luminance is
	// what Diffractor feeds - one plane to reduce and turn, and an NV12 decode's Y plane as it stands -
	// which is why this stage owns the packing rather than calling facedetect_hw_cnn: that entry point
	// hard-codes three interleaved channels. The face tests hold what luminance finds to what that
	// entry point finds in the same colour photographs.
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

	// libfacedetection's DetectionOutputHw, keeping the geometry in floats: that one truncates boxes
	// and landmarks to whole pixels of the detector's input, which is several pixels of the picture's.
	void select_detections(const fdt::hw::HwDecodedOutputs& decoded, const int confidence_floor,
	                       std::vector<detector_candidate>& candidates, std::vector<detector_candidate>& selected)
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
			// floor is above it too.
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
			const auto marks_finite = std::ranges::all_of(candidate.marks,
			                                              [](const float v) { return std::isfinite(v); });
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
			{ return overlap(candidate, kept) <= detector_overlap_threshold; }))
			{
				selected.emplace_back(candidate);
				if (selected.size() >= detector_keep_top_k) break;
			}
		}
	}

	// libfacedetection's ImageToInitialBlobHwTo, reading one luminance plane where that reads three
	// interleaved channels.
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

	// The weights, compiled in as a table of 53 convolution layers. Loaded once, on the first detection:
	// the table is static const and the filters repack it for SIMD.
	const fdt::hw::HwModel& detector_model()
	{
		static const fdt::hw::HwModel model = fdt::hw::LoadStaticModel();
		return model;
	}

	class yunet_face_detector final : public faces::face_detector
	{
		// Reused across pictures, so detecting a run of them allocates none of these per picture.
		core::surface_ptr _reduced;
		std::vector<uint8_t> _stored_luma;
		std::vector<uint8_t> _upright_luma;
		fdt::hw::HwBlob _input;
		fdt::hw::HwNetworkWorkspace _workspace;
		fdt::hw::HwHeadOutputs _heads;
		fdt::hw::HwDecodedOutputs _decoded;
		std::vector<detector_candidate> _candidates;
		std::vector<detector_candidate> _selected;

	public:
		std::vector<faces::face> detect(const core::const_surface_ptr& surface, const faces::tuning& t) override
		{
			std::vector<faces::face> result;

			if (!core::is_valid(surface)) return result;

			// Packed BGRX/BGRA, or NV12 - whose first plane already is the luminance the network
			// wants. P010 is excluded because its luma is 16-bit and would need a conversion this
			// stage deliberately does not do.
			if (const auto format = surface->format(); !core::is_packed(format) && format != core::texture_format::NV12)
			{
				return result;
			}

			const auto extent = surface->dimensions();
			if (extent.is_empty()) return result;

			auto reduced = extent;

			if (const auto long_edge = std::max(extent.cx, extent.cy); long_edge > t.detector_edge)
			{
				const auto ratio = static_cast<double>(t.detector_edge) / long_edge;
				reduced = {std::max(1, static_cast<int>(extent.cx * ratio)),
				           std::max(1, static_cast<int>(extent.cy * ratio))};
			}

			// The 8-bit plane the network reads. An NV12 decode's Y plane is reduced on its own, with no
			// colour image at any size; a packed decode is reduced in colour by the shared scaler first,
			// then weighed down to luminance.
			if (!faces::reduce_to_luma(surface, reduced, _reduced, _stored_luma)) return result;

			// Turned once the plane is small: the network finds upright faces, and a portrait photograph
			// decoded in its file's own orientation has them on their side.
			const auto orientation = surface->orientation();
			const auto input_extent = faces::upright_extent(reduced, orientation);
			const auto* input = _stored_luma.data();

			if (orientation != core::orientation::top_left && orientation != core::orientation::none)
			{
				if (!faces::orient_plane(_stored_luma, reduced, orientation, _upright_luma)) return result;
				input = _upright_luma.data();
			}

			pack_luma_blob(input, input_extent, input_extent.cx, &_input);
			fdt::hw::ForwardNetworkHwTo(_input, detector_model(), &_workspace, &_heads);
			fdt::hw::DecodeAndConcatHwTo(&_heads, &_decoded);
			select_detections(_decoded, t.detector_confidence, _candidates, _selected);
			result.reserve(_selected.size());

			const auto upright = faces::upright_extent(extent, orientation);
			const auto scale_x = static_cast<double>(upright.cx) / input_extent.cx;
			const auto scale_y = static_cast<double>(upright.cy) / input_extent.cy;

			// A face this small carries too few pixels to be worth reporting.
			const auto min_edge = std::min(upright.cx, upright.cy) * t.min_face_edge_fraction;

			for (const auto& candidate : _selected)
			{
				faces::face f;
				f.confidence = static_cast<int>(candidate.score * 100.0f);
				f.bounds = rectd{candidate.xmin * scale_x, candidate.ymin * scale_y,
				                 (candidate.xmax - candidate.xmin) * scale_x,
				                 (candidate.ymax - candidate.ymin) * scale_y};

				if (f.bounds.Width < min_edge || f.bounds.Height < min_edge) continue;

				for (auto m = 0; m < faces::landmark_count; ++m)
				{
					f.marks[m] = pointd{candidate.marks[m * 2] * scale_x, candidate.marks[(m * 2) + 1] * scale_y};
				}

				result.emplace_back(f);
			}

			return result;
		}
	};
}

namespace faces
{
	face_detector_ptr create_face_detector()
	{
		return std::make_unique<yunet_face_detector>();
	}
}
