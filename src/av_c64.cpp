// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: A Commodore 64 run live in the media pane. lib64 starts a disk's program, a tape's, a program
// file or a cartridge as a user would, on the emulator queue's worker, paced by the sound device; each
// finished picture is published whole for the display to upload. The keyboard drives its joystick and keys.

#include "pch.h"
#include "av_c64.h"

#include "disk.h"
#include "machine.h"

namespace
{
	// The largest file the machine is given: a 1 MB EasyFlash cartridge with its headers, with room to spare.
	constexpr size_t max_c64_file_bytes = 4 * 1024 * 1024;
	// A PAL frame: 19656 cycles at 985248 Hz.
	constexpr double frame_seconds = pal_cycles_per_frame / static_cast<double>(sid_clock_pal);
	// The sound device's ring: long enough to ride out a slow frame, short enough that a key is heard promptly.
	constexpr double audio_ring_seconds = 0.08;
	// Sound queued before the device starts, so it does not run dry on its first period.
	constexpr double audio_prime_seconds = 0.04;
	// While the KERNAL boots and the program loads, frames run as fast as they go and only some are shown.
	constexpr int warp_frames_per_picture = 8;

	bool has_prefix(const core::blob& b, const std::string_view prefix)
	{
		return b.size() >= prefix.size() && std::memcmp(b.data(), prefix.data(), prefix.size()) == 0;
	}

	// The sound device in its own format: each frame's mono samples converted and spread over the front left
	// and right speakers, at the device's rate or a whole fraction of it. Writing waits for room, which is
	// what paces the machine to the device's clock.
	class c64_audio
	{
	public:
		bool open(const std::string& device, machine& m)
		{
			_out = platform::create_audio_output(device, audio_ring_seconds);
			if (!_out) return false;

			_format = _out->format();
			const auto supported = _format.sample_format == platform::audio_sample_format::float32 ||
			                       _format.sample_format == platform::audio_sample_format::int16 ||
			                       _format.sample_format == platform::audio_sample_format::int32;

			_repeat = 1;
			while (_format.sample_rate / _repeat > 96000) ++_repeat;

			if (!supported || _format.channels == 0 || _format.sample_rate < 8000 || _format.sample_rate % _repeat != 0)
			{
				_out.reset();
				return false;
			}

			m.set_audio_sample_rate(_format.sample_rate / _repeat);
			return true;
		}

		// False once the device is lost or the machine is stopping.
		bool write(const int16_t* pcm, const int count, const core::thread_event& wake, const std::atomic_bool& stop)
		{
			const auto channels = _format.channels;
			const size_t sample_bytes = _format.sample_format == platform::audio_sample_format::int16 ? 2 : 4;
			const auto frames = static_cast<size_t>(count) * _repeat;
			_bytes.resize(frames * channels * sample_bytes);

			auto* p = _bytes.data();
			for (int i = 0; i < count; ++i)
			{
				for (uint32_t r = 0; r < _repeat; ++r)
				{
					for (uint32_t c = 0; c < channels; ++c)
					{
						const int16_t s = c < 2 ? pcm[i] : 0;
						if (_format.sample_format == platform::audio_sample_format::float32)
						{
							const float f = s / 32768.0f;
							std::memcpy(p, &f, sizeof(f));
						}
						else if (_format.sample_format == platform::audio_sample_format::int32)
						{
							const int32_t v = static_cast<int32_t>(s) * 65536;
							std::memcpy(p, &v, sizeof(v));
						}
						else
						{
							std::memcpy(p, &s, sizeof(s));
						}
						p += sample_bytes;
					}
				}
			}

			size_t offset = 0;
			while (offset < _bytes.size())
			{
				offset += _out->write({_bytes.data() + offset, _bytes.size() - offset});
				if (offset >= _bytes.size()) break;

				if (!_started)
				{
					_out->start();
					_started = true;
				}

				if (stop.load() || platform::is_closing || _out->is_device_lost()) return false;
				_out->wait_for_buffer(wake, 50);
			}

			_queued += frames;
			if (!_started && _queued >= static_cast<size_t>(_format.sample_rate * audio_prime_seconds))
			{
				_out->start();
				_started = true;
			}

			return !_out->is_device_lost();
		}

		void volume(const double v) const
		{
			if (_out) _out->volume(v);
		}

		void close()
		{
			if (_out)
			{
				_out->stop();
				_out.reset();
			}
		}

	private:
		platform::audio_output_ptr _out;
		platform::audio_output_format _format;
		uint32_t _repeat = 1;
		std::vector<uint8_t> _bytes;
		size_t _queued = 0;
		bool _started = false;
	};
}

c64_session::c64_session(core::file_path path, std::vector<uint8_t> entry) :
	_path(path), _entry(std::move(entry)), _wake(true, false)
{
}

c64_session::~c64_session()
{
	stop();
}

void c64_session::start(df::async_i& async, std::string audio_device)
{
	async.queue_async(async_queue::emulator,
	                  [self = shared_from_this(), device = std::move(audio_device)] { self->run(device); });
}

void c64_session::stop()
{
	_stop = true;
	_wake.set();
}

core::const_surface_ptr c64_session::frame(uint64_t& number) const
{
	core::exclusive_lock lock(_frame_mutex);
	number = _frame_number;
	return _frame;
}

void c64_session::joystick(const uint8_t bits)
{
	_joystick = bits;
	_joystick_taps.fetch_or(bits);
}

void c64_session::key(const int row, const int col, const bool pressed)
{
	core::exclusive_lock lock(_input_mutex);
	_keys.push_back({static_cast<uint8_t>(row), static_cast<uint8_t>(col), pressed});
}

void c64_session::restore()
{
	_restore = true;
}

void c64_session::joystick_port(const int port)
{
	_port = port == 1 ? 1 : 2;
}

void c64_session::volume(const double v)
{
	_volume = std::clamp(core::round(v * 1000.0), 0, 1000);
}

bool c64_session::boot(machine& m) const
{
	const auto bytes = platform::blob_head_from_file(_path, max_c64_file_bytes);
	if (bytes.empty() || bytes.size() >= max_c64_file_bytes) return false;

	const auto* data = bytes.data();
	const auto size = bytes.size();

	// Told apart in the order libfiles lists them (list_c64_media), so a file runs as what its listing shows.
	// autostart_prg takes a .p00's program as it takes a .prg.
	if (has_prefix(bytes, "C64File")) return m.autostart_prg(data, size);

	// A T64's 32-byte description is not fixed text, but every writer's says "tape".
	if (has_prefix(bytes, "C64") && size > 32 &&
	    std::string_view(reinterpret_cast<const char*>(data), 32).find("tape") != std::string_view::npos)
	{
		std::vector<uint8_t> program;
		return read_tape_file_entry(data, size, _entry.data(), _entry.size(), program) &&
		       m.autostart_prg(program.data(), program.size());
	}

	if (has_prefix(bytes, "C64 CARTRIDGE   ")) return m.load_crt(data, size);

	if (disk_directory dir; read_disk_directory(data, size, dir))
		return m.autostart_disk(data, size, _entry.data(), _entry.size());

	return m.autostart_prg(data, size);
}

void c64_session::apply_input(machine& m)
{
	const auto bits = static_cast<uint8_t>(_joystick.load() | _joystick_taps.exchange(0));
	const auto port = _port.load();
	m.set_joystick(port, (bits & joy_up) != 0, (bits & joy_down) != 0, (bits & joy_left) != 0, (bits & joy_right) != 0,
	               (bits & joy_fire) != 0);
	m.set_joystick(port == 1 ? 2 : 1, false, false, false, false, false);

	std::vector<key_change> keys;
	{
		core::exclusive_lock lock(_input_mutex);
		keys.swap(_keys);
	}

	for (const auto& k : keys) m.queue_key(k.row, k.col, k.pressed);
	if (_restore.exchange(false)) m.press_restore();
}

void c64_session::publish(const machine& m)
{
	static_assert(picture_width == machine::framebuffer_width() && picture_height == machine::framebuffer_height());
	constexpr int w = picture_width;
	constexpr int h = picture_height;

	// The picture written into is never the newest, which the UI may be reading; one it still holds from
	// before is left to it and replaced.
	auto& slot = _surfaces[_next_surface];
	if (!slot || slot.use_count() > 1)
	{
		auto fresh = std::make_shared<core::surface>();
		if (!fresh->alloc(w, h, core::texture_format::RGB)) return;
		slot = std::move(fresh);
	}

	// The frame buffer is 0x00RRGGBB, which in memory is the blue, green, red order an RGB surface holds.
	const auto* source = m.framebuffer();
	for (int y = 0; y < h; ++y)
	{
		std::memcpy(slot->pixels_line(y), source + static_cast<size_t>(y) * w, static_cast<size_t>(w) * 4);
	}

	{
		core::exclusive_lock lock(_frame_mutex);
		_frame = slot;
		++_frame_number;
	}

	_next_surface ^= 1;
}

int c64_session::frame(machine& m, int16_t* out, const bool publish_picture)
{
	apply_input(m);
	m.exec(pal_cycles_per_frame);
	const auto samples = std::max(0, m.generate_audio(out, max_frame_samples));
	if (publish_picture) publish(m);
	return samples;
}

void c64_session::run(const std::string& audio_device)
{
	try
	{
		run_machine(audio_device);
	}
	catch (const std::exception& e)
	{
		// The worker would log it and carry on, leaving a machine that claims to run and never shows a frame.
		core::log(__FUNCTION__, e.what());
		_status = status::failed;
	}
}

void c64_session::run_machine(const std::string& audio_device)
{
	auto m = std::make_unique<machine>();

	if (_stop.load() || !boot(*m))
	{
		_status = _stop.load() ? status::stopped : status::failed;
		return;
	}

	m->set_render_enabled(true);

	c64_audio audio;
	auto has_audio = audio.open(audio_device, *m);
	auto volume = -1;
	std::vector<int16_t> pcm(max_frame_samples);
	auto warping = true;
	auto warp_frames = 0;
	auto next = platform::monotonic_seconds();
	const std::vector<std::reference_wrapper<core::thread_event>> events = {_wake, platform::event_exit};

	_status = status::running;

	while (!_stop.load() && !platform::is_closing)
	{
		// The boot and the load run unpaced and silent; the program starts at the machine's own speed.
		if (warping && !m->autostarting())
		{
			warping = false;
			m->set_audio_enabled(has_audio);
			next = platform::monotonic_seconds();
		}

		if (warping)
		{
			frame(*m, pcm.data(), ++warp_frames % warp_frames_per_picture == 0);
			continue;
		}

		const auto samples = frame(*m, pcm.data(), true);

		if (has_audio)
		{
			if (const auto v = _volume.load(); v != volume)
			{
				volume = v;
				audio.volume(v / 1000.0);
			}

			if (audio.write(pcm.data(), samples, _wake, _stop)) continue;
			if (_stop.load() || platform::is_closing) break;

			// The device went away: carry on, silent, by the clock.
			audio.close();
			has_audio = false;
			m->set_audio_enabled(false);
			next = platform::monotonic_seconds();
		}

		next += frame_seconds;
		const auto wait = next - platform::monotonic_seconds();
		if (wait > 0.001) platform::wait_for(events, static_cast<uint32_t>(wait * 1000.0) + 1, false);
		else if (wait < -0.25) next = platform::monotonic_seconds();
	}

	audio.close();
	_status = status::stopped;
}

int c64_matrix_keys(const char32_t key, uint8_t rows[2], uint8_t cols[2])
{
	const auto one = [&](const int row, const int col)
	{
		rows[0] = static_cast<uint8_t>(row);
		cols[0] = static_cast<uint8_t>(col);
		return 1;
	};
	// Typed on the C64 with its left shift held.
	const auto shifted = [&](const int row, const int col)
	{
		rows[0] = 1;
		cols[0] = 7;
		rows[1] = static_cast<uint8_t>(row);
		cols[1] = static_cast<uint8_t>(col);
		return 2;
	};

	// Letters and digits sit where they do on the C64's keyboard, as VICE places them.
	static constexpr std::array<std::array<uint8_t, 2>, 26> letters = {{
		{1, 2}, {3, 4}, {2, 4}, {2, 2}, {1, 6}, {2, 5}, {3, 2}, {3, 5}, {4, 1}, {4, 2}, {4, 5}, {5, 2}, {4, 4},
		{4, 7}, {4, 6}, {5, 1}, {7, 6}, {2, 1}, {1, 5}, {2, 6}, {3, 6}, {3, 7}, {1, 1}, {2, 7}, {3, 1}, {1, 4},
	}};
	static constexpr std::array<std::array<uint8_t, 2>, 10> digits = {
		{{4, 3}, {7, 0}, {7, 3}, {1, 0}, {1, 3}, {2, 0}, {2, 3}, {3, 0}, {3, 3}, {4, 0}}};

	if (key >= U'A' && key <= U'Z') return one(letters[key - U'A'][0], letters[key - U'A'][1]);
	if (key >= U'0' && key <= U'9') return one(digits[key - U'0'][0], digits[key - U'0'][1]);

	if (key == platform::keys::OEM_COMMA) return one(5, 7);
	if (key == platform::keys::OEM_PERIOD) return one(5, 4);
	if (key == platform::keys::OEM_2) return one(6, 7);
	if (key == platform::keys::OEM_1) return one(6, 2);
	if (key == platform::keys::OEM_MINUS) return one(5, 3);
	if (key == platform::keys::OEM_PLUS) return one(5, 0);
	if (key == platform::keys::OEM_3) return one(7, 1);
	if (key == platform::keys::OEM_7) return one(5, 5);
	if (key == platform::keys::OEM_4) return one(5, 6);
	if (key == platform::keys::OEM_6) return one(6, 1);
	if (key == platform::keys::OEM_5 || key == platform::keys::OEM_102) return one(6, 6);
	if (key == platform::keys::NEXT) return one(6, 5);
	if (key == platform::keys::END) return one(6, 0);
	if (key == platform::keys::RETURN) return one(0, 1);
	if (key == platform::keys::SPACE) return one(7, 4);
	if (key == platform::keys::BACK || key == platform::keys::DEL) return one(0, 0);
	if (key == platform::keys::INSERT) return shifted(0, 0);
	if (key == platform::keys::SHIFT) return one(1, 7);
	if (key == platform::keys::CONTROL) return one(7, 2);
	if (key == platform::keys::TAB) return one(7, 5);
	if (key == platform::keys::HOME) return one(6, 3);
	// Escape leaves the machine, so RUN/STOP is on Pause and F9.
	if (key == platform::keys::PAUSE || key == platform::keys::F9) return one(7, 7);
	if (key == platform::keys::F1) return one(0, 4);
	if (key == platform::keys::F3) return one(0, 5);
	if (key == platform::keys::F5) return one(0, 6);
	if (key == platform::keys::F7) return one(0, 3);
	if (key == platform::keys::F2) return shifted(0, 4);
	if (key == platform::keys::F4) return shifted(0, 5);
	if (key == platform::keys::F6) return shifted(0, 6);
	if (key == platform::keys::F8) return shifted(0, 3);
	return 0;
}

namespace
{
	uint8_t joystick_bit(const char32_t key)
	{
		if (key == platform::keys::UP) return c64_session::joy_up;
		if (key == platform::keys::DOWN) return c64_session::joy_down;
		if (key == platform::keys::LEFT) return c64_session::joy_left;
		if (key == platform::keys::RIGHT) return c64_session::joy_right;
		if (key == platform::keys::SPACE) return c64_session::joy_fire;
		return 0;
	}
}

bool c64_keyboard::press(c64_session& session, const char32_t key, const bool repeat)
{
	// RESTORE is not a held key but a pulse on the NMI line.
	if (key == platform::keys::PRIOR)
	{
		if (!repeat) session.restore();
		return true;
	}

	if (std::ranges::find(_held, key, &held_key::key) != _held.end()) return true;

	// Space is the joystick's fire and the C64's space bar both, as in lib64's own host, so a title screen
	// that waits for SPACE starts as one that waits for fire does.
	held_key held{key, joystick_bit(key), 0, {}, {}};
	if (!held.joystick || key == platform::keys::SPACE) held.count = c64_matrix_keys(key, held.rows, held.cols);
	if (!held.joystick && held.count == 0) return false;

	if (held.joystick)
	{
		_joystick |= held.joystick;
		session.joystick(_joystick);
	}

	for (auto i = 0; i < held.count; ++i) session.key(held.rows[i], held.cols[i], true);
	_held.push_back(held);
	return true;
}

void c64_keyboard::sync(c64_session& session)
{
	auto joystick = _joystick;

	std::erase_if(_held, [&](const held_key& held)
	{
		if (platform::is_key_down_async(held.key)) return false;

		joystick &= static_cast<uint8_t>(~held.joystick);
		for (auto i = 0; i < held.count; ++i) session.key(held.rows[i], held.cols[i], false);
		return true;
	});

	if (joystick != _joystick)
	{
		_joystick = joystick;
		session.joystick(_joystick);
	}
}

void c64_keyboard::release_all(c64_session& session)
{
	for (const auto& held : _held)
	{
		for (auto i = 0; i < held.count; ++i) session.key(held.rows[i], held.cols[i], false);
	}

	_held.clear();
	_joystick = 0;
	session.joystick(0);
}
