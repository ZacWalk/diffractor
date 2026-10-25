// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Audio spectrum visualization using FFT. Transforms audio samples into frequency
// data and renders animated bar visualizations synchronized with playback.

#pragma once

constexpr uint32_t FFT_BUFFER_SIZE_LOG = 9u;
constexpr uint32_t FFT_BUFFER_SIZE = 1 << FFT_BUFFER_SIZE_LOG;

class fast_fft
{
public:
	using sound_sample = int16_t;

private:
	// Table to speed up bit reverse copy
	uint32_t reversed[FFT_BUFFER_SIZE];

	// The next two tables could be made to use less space in memory, since they
	//overlap hugely, but hey.
	float sintable[FFT_BUFFER_SIZE / 2];
	float costable[FFT_BUFFER_SIZE / 2];
	float window[FFT_BUFFER_SIZE];

	// Prepare data to perform an FFT on
	void prepare(const sound_sample* input, float* re, float* im) const
	{
		auto* realptr = re;
		auto* imagptr = im;

		/* Get input, in reverse bit order */
		for (const auto i : reversed)
		{
			*realptr++ = input[i] * window[i];
			*imagptr++ = 0;
		}
	}

	/*
	* Take result of an FFT and calculate the intensities of each frequency
	* Note: only produces half as many data points as the input had.
	* This is roughly a consequence of the Nyquist sampling theorem.
	* (FIXME - make this comment better, and helpful.)
	*
	* The two divisions by 4 are also a consequence of this: the contributions
	* returned for each frequency are split into two parts, one at i in the
	* table, and the other at FFT_BUFFER_SIZE - i, except for i = 0 and
	* FFT_BUFFER_SIZE which would otherwise get float (and then 4* when squared)
	* the contributions.
	*/
	static void calc_output(const float* re, const float* im, float* output)
	{
		auto* outputptr = output;
		const auto* realptr = re;
		const auto* imagptr = im;
		auto* const endptr = output + FFT_BUFFER_SIZE / 2;

		while (outputptr <= endptr)
		{
			*outputptr = *realptr * *realptr + *imagptr * *imagptr;
			outputptr++;
			realptr++;
			imagptr++;
		}
		/* Do divisions to keep the constant and highest frequency terms in scale
		* with the other terms. */
		*output /= 4;
		*endptr /= 4;
	}

	/*
	* Actually perform the FFT
	*/
	void calculate(float* re, float* im) const
	{
		/* Set up some variables to reduce calculation in the loops */
		uint32_t exchanges = 1;
		uint32_t factfact = FFT_BUFFER_SIZE / 2;

		/* Loop through the divide and conquer steps */
		for (uint32_t i = FFT_BUFFER_SIZE_LOG; i != 0; i--)
		{
			/* In this step, we have 2 ^ (i - 1) exchange groups, each with
			* 2 ^ (FFT_BUFFER_SIZE_LOG - i) exchanges
			*/
			/* Loop through the exchanges in a group */
			for (uint32_t j = 0; j != exchanges; j++)
			{
				/* Work out factor for this exchange
				* factor ^ (exchanges) = -1
				* So, real = cos(j * PI / exchanges),
				*     imag = sin(j * PI / exchanges)
				*/
				const float fact_real = costable[j * factfact];
				const float fact_imag = sintable[j * factfact];

				/* Loop through all the exchange groups */
				for (uint32_t k = j; k < FFT_BUFFER_SIZE; k += exchanges << 1)
				{
					const int k1 = k + exchanges;
					/* newval[k]  := val[k] + factor * val[k1]
					* newval[k1] := val[k] - factor * val[k1]
					**/
					/* FIXME - potential scope for more optimization here? */
					const float tmp_real = fact_real * re[k1] - fact_imag * im[k1];
					const float tmp_imag = fact_real * im[k1] + fact_imag * re[k1];
					re[k1] = re[k] - tmp_real;
					im[k1] = im[k] - tmp_imag;
					re[k] += tmp_real;
					im[k] += tmp_imag;
				}
			}
			exchanges <<= 1;
			factfact >>= 1;
		}
	}

	static uint32_t reverse_bits(uint32_t initial)
	{
		uint32_t result = 0;

		for (uint32_t loop = 0; loop < FFT_BUFFER_SIZE_LOG; loop++)
		{
			result <<= 1;
			result += initial & 1;
			initial >>= 1;
		}
		return result;
	}

public:
	fast_fft()
	{
		for (uint32_t i = 0; i < FFT_BUFFER_SIZE; i++)
		{
			reversed[i] = reverse_bits(i);
			window[i] = static_cast<float>(0.5 - 0.5 * cos(2.0 * M_PI * i / (FFT_BUFFER_SIZE - 1)));
		}

		for (uint32_t i = 0; i < FFT_BUFFER_SIZE / 2; i++)
		{
			const auto j = 2 * M_PI * i / FFT_BUFFER_SIZE;
			costable[i] = static_cast<float>(cos(j));
			sintable[i] = static_cast<float>(sin(j));
		}
	}

	~fast_fft()
	{
	}

	void calc(const sound_sample* input, float* output)
	{
		// Temporary data stores to perform FFT in. 
		float real[FFT_BUFFER_SIZE];
		float imag[FFT_BUFFER_SIZE];

		// Convert data from sound format to be ready for FFT
		prepare(input, real, imag);

		// Do the actual FFT
		calculate(real, imag);

		// Convert the FFT output into intensities
		calc_output(real, imag, output);
	}
};


class av_visualizer
{
	static constexpr int num_bars = 64;
	static constexpr double animation_lead = 0.030;
	static constexpr double attack_seconds = 0.025;
	static constexpr double release_seconds = 0.150;

	static fast_fft fft;
	friend void should_compact_consumed_audio_only_when_needed();
	friend void should_time_visualizer_independently_of_refresh_rate();
	friend void should_share_one_immutable_visualizer_frequency_table();
	friend void should_keep_visualizer_frequency_bins_stable_across_sessions();

	struct frame
	{
		int _data[2][num_bars];
		double _time;

		frame(frame&&) noexcept = default;
		frame& operator=(frame&&) noexcept = default;

		explicit frame(const double t = 0) noexcept : _time(t)
		{
			memset(_data, 0, sizeof(_data));
		}

		frame(const frame& other) noexcept : _time(other._time)
		{
			memcpy_s(_data, sizeof(_data), other._data, sizeof(_data));
		}

		frame& operator=(const frame& other) noexcept
		{
			_time = other._time;
			memcpy_s(_data, sizeof(_data), other._data, sizeof(_data));
			return *this;
		}

		void include_peak(const frame& other)
		{
			for (auto ch = 0; ch < 2; ++ch)
			{
				for (auto i = 0; i < num_bars; ++i)
				{
					_data[ch][i] = std::max(_data[ch][i], other._data[ch][i]);
				}
			}
		}

		bool animate(const frame& latest, const frame& peak, const double elapsed)
		{
			auto changed = false;
			const auto attack = 1.0 - exp(-elapsed / attack_seconds);
			const auto release = 1.0 - exp(-elapsed / release_seconds);

			for (auto ch = 0; ch < 2; ++ch)
			{
				for (auto i = 0; i < num_bars; ++i)
				{
					const auto current = _data[ch][i];
					const auto target = peak._data[ch][i] > current ? peak._data[ch][i] : latest._data[ch][i];

					if (current != target)
					{
						const auto alpha = target > current ? attack : release;
						auto next = df::round(current + (target - current) * alpha);
						if (next == current) next += target > current ? 1 : -1;
						_data[ch][i] = next;
						changed = true;
					}
				}
			}

			return changed;
		}

		bool operator<(const frame& other) const
		{
			return _time < other._time;
		}
	};

	class frame_queue
	{
		std::deque<frame> _q;
		frame _current;

	public:
		frame_queue()
		{
		}

		~frame_queue()
		{
		}

		void clear()
		{
			_q.clear();
			_current = frame(0.0);
		}

		void push(frame& frame)
		{
			// Backstop against an undrained queue: the visualisation only ever looks a little way
			// ahead, so anything beyond this is unbounded growth rather than useful lookahead.
			constexpr size_t max_queued_frames = 1024;
			if (_q.size() >= max_queued_frames) _q.pop_front();

			const auto found = std::lower_bound(_q.begin(), _q.end(), frame, [](auto&& l, auto&& r)
			{
				return l._time < r._time;
			});

			_q.emplace(found, frame); // .emplace_back(frame);
		}

		bool pop_animate(frame& output, const double time, const double elapsed)
		{
			bool popped = false;
			frame peak = _current;

			while (!_q.empty() && _q.front()._time <= time)
			{
				_current = _q.front();
				peak.include_peak(_current);
				_q.pop_front();
				popped = true;
			}

			return elapsed > 0.0 && output.animate(_current, popped ? peak : _current, elapsed);
		}
	};

	mutable platform::mutex _mutex;
	_Guarded_by_(_mutex) frame_queue _frames;
	_Guarded_by_(_mutex) frame _frame;
	_Guarded_by_(_mutex) double _last_step_time = -1.0;

public:
	av_visualizer()
	{
	}

	void update(audio_buffer& buffer_in)
	{
		auto* data = buffer_in.data + buffer_in.start_pos;
		auto data_avail = buffer_in.used_bytes() / 4;
		auto time_in = buffer_in.time;
		auto bytes_processed = 0;

		int16_t lBuffer[FFT_BUFFER_SIZE];
		int16_t rBuffer[FFT_BUFFER_SIZE];

		while (data_avail >= FFT_BUFFER_SIZE)
		{
			auto* l = lBuffer;
			auto* r = rBuffer;
			const auto* p = std::bit_cast<const uint16_t*>(data);

			for (uint32_t i = 0; i < FFT_BUFFER_SIZE; i++)
			{
				*l++ = *p++;
				*r++ = *p++;
			}

			const auto sample_rate = buffer_in.format.sample_rate;
			const auto window_center = sample_rate > 0 ? FFT_BUFFER_SIZE * 0.5 / sample_rate : 0.0;
			frame frame(time_in + window_center);

			calc_bars(lBuffer, frame._data[0]);
			calc_bars(rBuffer, frame._data[1]);

			{
				platform::exclusive_lock lock(_mutex);
				_frames.push(frame);
			}

			bytes_processed += FFT_BUFFER_SIZE * 4;
			data += FFT_BUFFER_SIZE * 4;
			data_avail -= FFT_BUFFER_SIZE;
			time_in += FFT_BUFFER_SIZE * 4.0 / static_cast<double>(buffer_in.bytes_per_second());
		}

		buffer_in.remove(bytes_processed);
	}

	static constexpr uint32_t min_sample_bytes()
	{
		return FFT_BUFFER_SIZE * 4;
	}

	bool step(const double time)
	{
		platform::exclusive_lock lock(_mutex);
		const auto elapsed = _last_step_time < 0.0 ? 1.0 / 60.0 : std::clamp(time - _last_step_time, 0.0, 0.1);
		_last_step_time = time;
		return _frames.pop_animate(_frame, time + animation_lead, elapsed);
	}

	void clear()
	{
		platform::exclusive_lock lock(_mutex);
		_frames.clear();
		_frame = frame(0.0);
		_last_step_time = -1.0;
	}

	void render(const ui::vertices_ptr& verts, const recti rect, const pointi offset, const float alpha,
	            const double time) const
	{
		recti rects[num_bars];
		ui::color colors[num_bars];

		{
			platform::shared_lock lock(_mutex);

			constexpr double scaleY = 444.0;
			const double hh = rect.height() / 2.0;
			const double y = static_cast<double>(rect.top + rect.bottom) / 2.0;
			const double step = std::clamp(static_cast<double>(rect.width()) / (static_cast<double>(num_bars) + 1.0),
			                               3.0, 16.0);
			double x = static_cast<double>(rect.left) + (static_cast<double>(rect.width()) - (static_cast<double>(
				num_bars) + 1.0) * static_cast<double>(step)) / 2.0;

			for (auto i = 0; i < num_bars; i++)
			{
				x += step;

				const auto yl = static_cast<double>(_frame._data[0][i]);
				const auto yr = static_cast<double>(_frame._data[1][i]);
				const double yy = yl + yr;
				const double a = std::clamp((yy + scaleY / 2.0) / (scaleY * 2.0) * alpha, 0.4, 1.0);
				const double inflate = step / 2.0 + yy * (step / 2.0) / (scaleY * 3.0);

				// Cosine palette based colors
				constexpr auto pi4 = M_PI / 0.25;
				const auto ic = (0.25 + i * 0.003 + yy * 0.04 / scaleY) * pi4;
				const auto rr = static_cast<float>((cos(ic) + 1.0) / 2.0);
				const auto gg = static_cast<float>((cos(ic - 0.125 * pi4) + 1.0) / 2.0);
				const auto bb = static_cast<float>((cos(ic - 0.25 * pi4) + 1.0) / 2.0);
				colors[i] = ui::color(rr, gg, bb, static_cast<float>(a));

				recti col(df::round(x - inflate), df::round(y - yl * hh / scaleY), df::round(x + inflate),
				          df::round(y + yr * hh / scaleY));
				rects[i] = col.offset(offset);
			}
		}

		verts->update(rects, colors, num_bars);
	}

private:
	static const std::array<int, num_bars + 1>& frequency_scale()
	{
		static const auto scale = []
		{
			++frequency_scale_init_count();
			std::array<int, num_bars + 1> result{};

			for (auto i = 0; i <= num_bars; i++)
			{
				result[i] = df::round(0.5 + pow(FFT_BUFFER_SIZE / 2.0, static_cast<double>(i) / num_bars));
				if (i > 0 && result[i] <= result[i - 1]) result[i] = result[i - 1] + 1;
				result[i] = std::min(result[i], static_cast<int>(FFT_BUFFER_SIZE / 2 - 1));
			}

			return result;
		}();

		return scale;
	}

	static std::atomic<int>& frequency_scale_init_count()
	{
		static std::atomic<int> result = 0;
		return result;
	}

	static void calc_bars(const int16_t* src, int* bars)
	{
		float tmp_out[FFT_BUFFER_SIZE / 2 + 1];

		fft.calc(src, tmp_out);
		const auto& scale = frequency_scale();

		for (auto i = 0; i < num_bars; i++)
		{
			auto y = 0;

			for (auto c = scale[i]; c < scale[i + 1]; c++)
			{
				const auto d = static_cast<int>(sqrtf(tmp_out[c + 1]));
				if (d > y) y = d;
			}

			y >>= 12;
			if (y > 0)
				y = static_cast<int>(log(static_cast<float>(y)) * 28.0f * (1.8f + static_cast<float>(i) /
					num_bars));

			bars[i] = y;
		}
	}

	static float linear_interpolate(const float a, const float b, const float t)
	{
		return a * (1.0f - t) + b * t;
	}

	static ui::color interpolate(const ui::color ca, const ui::color cb, const float t, const float a)
	{
		ui::color result;
		result.r = linear_interpolate(ca.r, cb.r, t);
		result.g = linear_interpolate(ca.g, cb.g, t);
		result.b = linear_interpolate(ca.b, cb.b, t);
		result.a = a;
		return result;
	}
};
