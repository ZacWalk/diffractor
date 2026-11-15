// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: A Commodore 64 run live in the media pane. lib64 starts a disk's program, a tape's, a program
// file or a cartridge as a user would, on the emulator queue's worker, paced by the sound device; each
// finished picture is published whole for the display to upload. The keyboard drives its joystick and keys.

#pragma once

#include "util_interfaces.h"

class machine;

// The live machine. Shared by the emulator queue's worker, which alone touches the emulator, and the UI
// thread, which reads the published picture and status and hands over keys; see implementation.md's
// synchronized types.
class c64_session final : public std::enable_shared_from_this<c64_session>
{
public:
	enum class status
	{
		starting,
		running,
		failed,
		stopped,
	};

	// The file to run and, for a disk or a tape, the PETSCII name of the entry in it as it is stored. That entry
	// runs, whatever else its name would match as the pattern a drive takes it for.
	c64_session(core::file_path path, std::vector<uint8_t> entry);
	~c64_session();

	c64_session(const c64_session&) = delete;
	c64_session& operator=(const c64_session&) = delete;

	// Queues the machine on the emulator queue. The worker reads the file and plays through the sound
	// device named, or the default.
	void start(df::async_i& async, std::string audio_device);
	// Ends the machine within a frame. Safe to repeat, and from any thread.
	void stop();

	status state() const { return _status.load(); }
	bool running() const { return _status.load() == status::running; }

	// The newest finished picture and its number, which rises with each one. Null before the first.
	core::const_surface_ptr frame(uint64_t& number) const;

	// Input, from the UI thread; the worker applies it at the start of the next frame, so a press shorter
	// than a frame is still seen.
	void joystick(uint8_t bits);
	void key(int row, int col, bool pressed);
	void restore();
	void joystick_port(int port);
	int joystick_port() const { return _port.load(); }
	// 0 to 1, as the player's own volume.
	void volume(double v);

	// The worker's own steps, public for the tests: reading and starting the file, and one frame - emulated,
	// with its picture published. A frame's samples are written to out, which must hold
	// max_frame_samples. Returns the number of samples.
	bool boot(machine& m) const;
	int frame(machine& m, int16_t* out, bool publish);
	static constexpr int max_frame_samples = 4096;

	// The picture's size: the VIC-II's visible area, its border included.
	static constexpr int picture_width = 384;
	static constexpr int picture_height = 272;

	enum joystick_bits : uint8_t
	{
		joy_up = 0x01,
		joy_down = 0x02,
		joy_left = 0x04,
		joy_right = 0x08,
		joy_fire = 0x10,
	};

private:
	void run(const std::string& audio_device);
	void run_machine(const std::string& audio_device);
	void apply_input(machine& m);
	void publish(const machine& m);

	const core::file_path _path;
	const std::vector<uint8_t> _entry;

	std::atomic<status> _status = status::starting;
	std::atomic_bool _stop = false;
	// Set by stop(), so a wait for the sound device or the next frame ends at once.
	core::thread_event _wake;

	std::atomic<uint8_t> _joystick = 0;
	// Directions and fire pressed since the worker last looked, so a tap is held for at least a frame.
	std::atomic<uint8_t> _joystick_taps = 0;
	std::atomic<int> _port = 2;
	std::atomic_bool _restore = false;
	std::atomic<int> _volume = 1000;

	struct key_change
	{
		uint8_t row;
		uint8_t col;
		bool pressed;
	};

	mutable core::mutex _input_mutex;
	_Guarded_by_(_input_mutex) std::vector<key_change> _keys;

	mutable core::mutex _frame_mutex;
	_Guarded_by_(_frame_mutex) core::const_surface_ptr _frame;
	_Guarded_by_(_frame_mutex) uint64_t _frame_number = 0;

	// The worker's two pictures, written in turn; one the UI still holds is replaced rather than reused.
	core::surface_ptr _surfaces[2];
	int _next_surface = 0;
};

// The keys held on the machine, kept on the UI thread. Arrows and Space are the joystick, Space the space bar
// too; the other keys the C64 has are pressed where they sit on its keyboard. Key-ups are not delivered, so a
// key is released once the keyboard reports it up.
class c64_keyboard
{
public:
	// False for a key that is not the C64's, so the application keeps it.
	bool press(c64_session& session, char32_t key, bool repeat);
	// Releases the keys no longer held. Called on every tick while the machine runs.
	void sync(c64_session& session);
	void release_all(c64_session& session);

private:
	struct held_key
	{
		char32_t key;
		uint8_t joystick;
		int count;
		uint8_t rows[2];
		uint8_t cols[2];
	};

	std::vector<held_key> _held;
	uint8_t _joystick = 0;
};

// The C64 keyboard matrix positions a host key stands for, as row and column: one, or two for a key the C64
// types with its shift held. Returns how many; 0 for a key it does not have.
int c64_matrix_keys(char32_t key, uint8_t rows[2], uint8_t cols[2]);

// Whether a listing row, by its type byte, names something the machine can start: a program on a disk, the
// cartridge, or any file on a tape.
constexpr bool c64_row_runs(const uint8_t file_type, const bool tape)
{
	return tape || file_type == 0x99 || (file_type & 0x07) == 2;
}
